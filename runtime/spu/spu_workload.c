/* spu_workload.c — SPU workload / task dispatch registry (see spu_workload.h).
 *
 * Maps a registered SPU image (by FNV-1a-64 content fingerprint) to its
 * pre-lifted native entry, loads the image into a 256 KB local store, and runs
 * it with the SPURS task ABI. cellSpurs's AddWorkload/CreateTask call
 * spu_workload_dispatch(); the registry is populated by the title's lifted set.
 */
#include "spu_workload.h"
#include "spu_lifted_job.h"   /* spu_run_lifted_job */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- fingerprint ------------------------------------------------------- */

uint64_t spu_workload_fingerprint(const void* data, size_t n)
{
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = 1469598103934665603ULL;          /* FNV offset basis */
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;                     /* FNV prime */
    }
    return h;
}

/* ---- registry ---------------------------------------------------------- */

#ifndef SPU_WORKLOAD_MAX
#define SPU_WORKLOAD_MAX 256
#endif

typedef struct {
    uint64_t            fp;
    spu_lifted_entry_fn fn;
    const char*         name;
} spu_workload_entry;

static spu_workload_entry s_registry[SPU_WORKLOAD_MAX];
static unsigned           s_registry_count = 0;

void spu_workload_register(uint64_t fingerprint, spu_lifted_entry_fn fn,
                           const char* name)
{
    if (!fn) return;
    for (unsigned i = 0; i < s_registry_count; i++) {
        if (s_registry[i].fp == fingerprint) {     /* idempotent on fingerprint */
            s_registry[i].fn   = fn;
            s_registry[i].name = name;
            return;
        }
    }
    if (s_registry_count >= SPU_WORKLOAD_MAX) {
        fprintf(stderr, "[spu_workload] registry full (%u); dropping '%s'\n",
                SPU_WORKLOAD_MAX, name ? name : "?");
        return;
    }
    s_registry[s_registry_count].fp   = fingerprint;
    s_registry[s_registry_count].fn   = fn;
    s_registry[s_registry_count].name = name;
    s_registry_count++;
}

spu_lifted_entry_fn spu_workload_find(uint64_t fingerprint)
{
    for (unsigned i = 0; i < s_registry_count; i++)
        if (s_registry[i].fp == fingerprint)
            return s_registry[i].fn;
    return NULL;
}

unsigned spu_workload_count(void) { return s_registry_count; }

/* ---- SPU ELF loader (32-bit big-endian) -------------------------------- */

static uint16_t rd_be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

int spu_elf_load_to_ls(const uint8_t* image, size_t image_size, uint8_t* ls,
                       uint32_t* entry_out)
{
    if (!image || !ls || image_size < 0x34) return 0;

    /* ELF ident: 0x7F 'E' 'L' 'F', ELFCLASS32 (1), ELFDATA2MSB (2). */
    if (!(image[0] == 0x7F && image[1] == 'E' && image[2] == 'L' && image[3] == 'F'))
        return 0;
    if (image[4] != 1 /*ELFCLASS32*/ || image[5] != 2 /*ELFDATA2MSB*/)
        return 0;

    uint32_t e_entry     = rd_be32(image + 0x18);
    uint32_t e_phoff     = rd_be32(image + 0x1C);
    uint16_t e_phentsize = rd_be16(image + 0x2A);
    uint16_t e_phnum     = rd_be16(image + 0x2C);
    if (e_phentsize < 0x20) e_phentsize = 0x20;

    for (uint16_t i = 0; i < e_phnum; i++) {
        size_t po = (size_t)e_phoff + (size_t)i * e_phentsize;
        if (po + 0x20 > image_size) break;
        const uint8_t* ph = image + po;

        uint32_t p_type   = rd_be32(ph + 0x00);
        if (p_type != 1 /*PT_LOAD*/) continue;
        uint32_t p_offset = rd_be32(ph + 0x04);
        uint32_t p_vaddr  = rd_be32(ph + 0x08);
        uint32_t p_filesz = rd_be32(ph + 0x10);
        uint32_t p_memsz  = rd_be32(ph + 0x14);

        /* bounds: segment must fit in local store and in the image */
        if ((uint64_t)p_vaddr + p_memsz > SPU_LS_SIZE)            return 0;
        if ((uint64_t)p_offset + p_filesz > image_size)          return 0;

        if (p_filesz) memcpy(ls + p_vaddr, image + p_offset, p_filesz);
        if (p_memsz > p_filesz)
            memset(ls + p_vaddr + p_filesz, 0, p_memsz - p_filesz);
    }
    if (entry_out) *entry_out = e_entry;
    return 1;
}

size_t spu_elf_image_size(const uint8_t* image, size_t max_avail)
{
    if (!image || max_avail < 0x34) return 0;
    if (!(image[0] == 0x7F && image[1] == 'E' && image[2] == 'L' && image[3] == 'F'))
        return 0;
    if (image[4] != 1 || image[5] != 2) return 0;     /* ELFCLASS32, ELFDATA2MSB */

    uint32_t e_phoff     = rd_be32(image + 0x1C);
    uint32_t e_shoff     = rd_be32(image + 0x20);
    uint16_t e_phentsize = rd_be16(image + 0x2A);
    uint16_t e_phnum     = rd_be16(image + 0x2C);
    uint16_t e_shentsize = rd_be16(image + 0x2E);
    uint16_t e_shnum     = rd_be16(image + 0x30);

    uint64_t end = (uint64_t)e_shoff + (uint64_t)e_shnum * e_shentsize;

    for (uint16_t k = 0; k < e_phnum; k++) {           /* program headers */
        size_t po = (size_t)e_phoff + (size_t)k * e_phentsize;
        if (po + 0x14 > max_avail) break;
        uint32_t p_offset = rd_be32(image + po + 0x04);
        uint32_t p_filesz = rd_be32(image + po + 0x10);
        uint64_t e = (uint64_t)p_offset + p_filesz;
        if (e > end) end = e;
    }
    for (uint16_t k = 0; k < e_shnum; k++) {           /* section headers */
        size_t so = (size_t)e_shoff + (size_t)k * e_shentsize;
        if (so + 0x18 > max_avail) break;
        uint32_t sh_type   = rd_be32(image + so + 0x04);
        uint32_t sh_offset = rd_be32(image + so + 0x10);
        uint32_t sh_size   = rd_be32(image + so + 0x14);
        if (sh_type != 8 /*SHT_NOBITS*/) {
            uint64_t e = (uint64_t)sh_offset + sh_size;
            if (e > end) end = e;
        }
    }
    if (end > max_avail) end = max_avail;
    return (size_t)end;
}

/* ---- dispatch ---------------------------------------------------------- */

int spu_workload_dispatch(const uint8_t* image, uint32_t image_size,
                          uint32_t args_ea)
{
    if (!image || image_size == 0) return 0;

    uint64_t fp = spu_workload_fingerprint(image, image_size);
    spu_lifted_entry_fn fn = spu_workload_find(fp);
    if (!fn) {
        fprintf(stderr,
            "[spu_workload] dispatch MISS fp=0x%016llX size=%u "
            "(no lifted SPU binary registered for this image)\n",
            (unsigned long long)fp, image_size);
        return 0;
    }

    /* Load the SPU ELF into a fresh local store, then run the lifted entry with
     * the task arg in r3. 256 KB is heap-allocated (too large for the stack,
     * and spu_run_lifted_job already builds a full spu_context on its stack). */
    uint8_t* ls = (uint8_t*)calloc(1, SPU_LS_SIZE);
    if (!ls) return 0;

    uint32_t entry = 0;
    if (!spu_elf_load_to_ls(image, image_size, ls, &entry)) {
        fprintf(stderr, "[spu_workload] dispatch fp=0x%016llX: not a valid SPU ELF\n",
                (unsigned long long)fp);
        free(ls);
        return 0;
    }

    fprintf(stderr,
        "[spu_workload] dispatch HIT fp=0x%016llX entry=0x%05X args=0x%08X -> running\n",
        (unsigned long long)fp, entry, args_ea);

    spu_run_lifted_job(fn, ls, args_ea);

    free(ls);
    return 1;
}
