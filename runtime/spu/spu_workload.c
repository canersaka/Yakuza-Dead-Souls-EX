/* spu_workload.c — SPU workload / task dispatch registry (see spu_workload.h).
 *
 * Maps a registered SPU image (by FNV-1a-64 content fingerprint) to its
 * pre-lifted native entry, loads the image into a 256 KB local store, and runs
 * it with the SPURS task ABI. cellSpurs's AddWorkload/CreateTask call
 * spu_workload_dispatch(); the registry is populated by the title's lifted set.
 */
#include "spu_workload.h"
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
    uint32_t            image_size;
    spu_lifted_entry_fn fn;
    int                 image_id;
    uint32_t            entry_pc;
    const char*         name;
} spu_workload_entry;

static spu_workload_entry s_registry[SPU_WORKLOAD_MAX];
static unsigned           s_registry_count = 0;
static spu_workload_image_executor_fn s_image_executor = NULL;

void spu_workload_register(uint64_t fingerprint, spu_lifted_entry_fn fn,
                           const char* name)
{
    if (!fn) return;
    for (unsigned i = 0; i < s_registry_count; i++) {
        if (s_registry[i].fp == fingerprint && s_registry[i].image_size == 0) {
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

int spu_workload_register_direct(uint64_t fingerprint, uint32_t image_size,
                                 spu_lifted_entry_fn fn, const char* name)
{
    if (!image_size || !fn) return 0;
    for (unsigned i = 0; i < s_registry_count; i++) {
        if (s_registry[i].fp == fingerprint &&
            s_registry[i].image_size == image_size) {
            s_registry[i].fn = fn;
            s_registry[i].image_id = -1;
            s_registry[i].entry_pc = 0;
            s_registry[i].name = name;
            return 1;
        }
    }
    if (s_registry_count >= SPU_WORKLOAD_MAX) {
        fprintf(stderr, "[spu_workload] registry full; rejected '%s'\n",
                name ? name : "?");
        return 0;
    }
    s_registry[s_registry_count++] = (spu_workload_entry){
        fingerprint, image_size, fn, -1, 0, name
    };
    return 1;
}

int spu_workload_register_image(uint64_t fingerprint, uint32_t image_size,
                                int image_id, uint32_t entry_pc,
                                const char* name)
{
    if (!image_size || image_id < 0) return 0;
    for (unsigned i = 0; i < s_registry_count; i++) {
        if (s_registry[i].fp == fingerprint &&
            s_registry[i].image_size == image_size) {
            s_registry[i].fn = NULL;
            s_registry[i].image_id = image_id;
            s_registry[i].entry_pc = entry_pc;
            s_registry[i].name = name;
            return 1;
        }
    }
    if (s_registry_count >= SPU_WORKLOAD_MAX) {
        fprintf(stderr, "[spu_workload] registry full; rejected '%s'\n",
                name ? name : "?");
        return 0;
    }
    s_registry[s_registry_count++] = (spu_workload_entry){
        fingerprint, image_size, NULL, image_id, entry_pc, name
    };
    return 1;
}

void spu_workload_set_image_executor(spu_workload_image_executor_fn executor)
{
    s_image_executor = executor;
}

int spu_workload_resolve(const void* image, uint32_t image_size,
                         spu_workload_image* out)
{
    if (!image || !image_size || !out) return 0;
    const uint64_t fp = spu_workload_fingerprint(image, image_size);
    for (unsigned i = 0; i < s_registry_count; i++) {
        const spu_workload_entry* e = &s_registry[i];
        if (e->fp == fp && e->image_size == image_size) {
            *out = (spu_workload_image){
                e->fp, image_size, e->image_id, e->entry_pc, e->fn, e->name
            };
            return 1;
        }
    }
    fprintf(stderr,
            "[spu_workload] exact image miss fp=0x%016llX size=%u\n",
            (unsigned long long)fp, image_size);
    return 0;
}

int spu_workload_execute(const spu_workload_image* image, spu_context* ctx)
{
    if (!image || !ctx) return 0;
    if (image->direct_entry) {
        image->direct_entry(ctx);
        return 1;
    }
    if (!s_image_executor) {
        fprintf(stderr,
                "[spu_workload] no image executor for '%s' (id=%d pc=0x%05X)\n",
                image->name ? image->name : "?", image->image_id,
                image->entry_pc);
        return 0;
    }
    return s_image_executor(ctx, image->image_id, image->entry_pc);
}

spu_lifted_entry_fn spu_workload_find(uint64_t fingerprint)
{
    for (unsigned i = 0; i < s_registry_count; i++)
        if (s_registry[i].fp == fingerprint)
            return s_registry[i].fn;
    return NULL;
}

unsigned spu_workload_count(void) { return s_registry_count; }
void spu_workload_reset(void)
{
    memset(s_registry, 0, sizeof(s_registry));
    s_registry_count = 0;
    s_image_executor = NULL;
}

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

    spu_workload_image registered;
    if (!spu_workload_resolve(image, image_size, &registered)) {
        return 0;
    }

    /* Load the SPU ELF into a fresh local store, then run the lifted entry with
     * the task arg in r3. 256 KB is heap-allocated (too large for the stack,
     * and spu_run_lifted_job already builds a full spu_context on its stack). */
    uint8_t* ls = (uint8_t*)calloc(1, SPU_LS_SIZE);
    if (!ls) return 0;

    uint32_t entry = 0;
    if (!spu_elf_load_to_ls(image, image_size, ls, &entry)) {
        fprintf(stderr, "[spu_workload] dispatch fp=0x%016llX: invalid SPU ELF\n",
                (unsigned long long)registered.fingerprint);
        free(ls);
        return 0;
    }

    fprintf(stderr,
        "[spu_workload] dispatch HIT fp=0x%016llX entry=0x%05X args=0x%08X -> running\n",
        (unsigned long long)registered.fingerprint, entry, args_ea);

    spu_context* ctx = (spu_context*)malloc(sizeof(*ctx));
    if (!ctx) {
        free(ls);
        return 0;
    }
    spu_context_init(ctx, 0);
    memcpy(ctx->ls, ls, SPU_LS_SIZE);
    ctx->gpr[3]._u32[0] = args_ea;
    int ok = spu_workload_execute(&registered, ctx);
    memcpy(ls, ctx->ls, SPU_LS_SIZE);
    free(ctx);

    free(ls);
    return ok;
}
