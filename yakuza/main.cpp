/*
 * Yakuza: Dead Souls recompiled runner.
 *
 * Boot sequence:
 *   1. reserve the guest address space and load EBOOT.elf's PT_LOAD segments
 *   2. load the game's shader module image
 *   3. register the lifted SPU programs, the lv2 syscalls and the import bridges
 *   4. build the main thread's stack, arguments and TLS registers
 *   5. start the vblank driver and call the entry function
 *
 * This file uses the generated ppu_context from ppu_recomp.h, the layout the
 * lifted code was compiled against (see yakuza_runner.h).
 */

#include "ppu_recomp.h"
#include "yakuza_runner.h"
#include "native_scene_jobs.h"
#include "../runtime/memory/vm.h"
#include "../include/ps3emu/guest_call.h"
#include "generated/spu/spu_images.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#include <dbghelp.h>
#pragma comment(lib, "winmm.lib")

/* Guest context of the current host thread, set at every guest thread entry
 * and callback.  The crash handler walks its PPC64 back-chain. */
thread_local ppu_context* g_yz_cur_ctx = nullptr;

/* The game module's TOC (dispatch.cpp restores it for callbacks entered by
 * bare code address). */
extern "C" uint32_t g_yz_game_toc;

extern "C" uint32_t g_ps3_sdk_version;            /* sys_process_get_sdk_version */
extern "C" void cellGame_init_from_paramsfo(const char* sfo_path);
extern "C" void yz_spu_crash_note(void);
extern "C" void yz_rsx_fifo_acquire(void);

extern "C" uint64_t spu_workload_fingerprint(const void*, size_t);
extern "C" size_t spu_elf_image_size(const uint8_t*, size_t);
extern "C" int spu_workload_register_image_bytes(
    uint64_t, const void*, uint32_t, int, uint32_t, const char*);
extern "C" void spu_workload_reset(void);

/* ---------------------------------------------------------------------------
 * ELF loading
 * -----------------------------------------------------------------------*/

static uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t be64(const uint8_t* p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

/* PT_TLS template, shared with threads.cpp for per-thread TLS blocks and
 * passed to the entry point in r8-r10 (the CRT forwards it to
 * sys_initialize_tls). */
extern "C" uint64_t yz_tls_vaddr = 0, yz_tls_filesz = 0, yz_tls_memsz = 0;

/* sys_process_param_t.malloc_pagesize from PT_PROC_PARAM.  lv2 passes it to
 * the entry point in r12 and the CRT stores it in the libc malloc config,
 * where 0 makes every malloc fail.  1 MB when the segment is absent. */
static uint32_t g_malloc_pagesize = 0x100000;
static uint64_t g_proc_param_vaddr = 0;

static int load_elf(const char* path, uint64_t* entry_out)
{
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "ERROR: cannot open %s\n", path); return -1; }

    uint8_t eh[64];
    if (fread(eh, 1, 64, f) != 64 ||
        memcmp(eh, "\x7f""ELF", 4) != 0 || eh[4] != 2 /*ELF64*/ || eh[5] != 2 /*BE*/) {
        fprintf(stderr, "ERROR: %s is not a big-endian ELF64\n", path);
        fclose(f);
        return -1;
    }

    uint64_t e_entry  = be64(eh + 24);
    uint64_t e_phoff  = be64(eh + 32);
    uint16_t e_phentsize = be16(eh + 54);
    uint16_t e_phnum  = be16(eh + 56);

    printf("[boot] %s: entry=0x%08llX, %u program headers\n",
           path, (unsigned long long)e_entry, e_phnum);

    for (uint16_t i = 0; i < e_phnum; i++) {
        uint8_t ph[56];
        if (fseek(f, (long)(e_phoff + (uint64_t)i * e_phentsize), SEEK_SET) != 0 ||
            fread(ph, 1, 56, f) != 56) {
            fprintf(stderr, "ERROR: bad program header %u\n", i);
            fclose(f);
            return -1;
        }
        uint32_t p_type   = be32(ph + 0);
        uint64_t p_offset = be64(ph + 8);
        uint64_t p_vaddr  = be64(ph + 16);
        uint64_t p_filesz = be64(ph + 32);
        uint64_t p_memsz  = be64(ph + 40);

        if (p_type == 7 /*PT_TLS*/) {
            yz_tls_vaddr  = p_vaddr;
            yz_tls_filesz = p_filesz;
            yz_tls_memsz  = p_memsz;
            continue;
        }
        if (p_type == 0x60000001 /*PT_PROC_PARAM*/) {
            g_proc_param_vaddr = p_vaddr;
            continue;
        }
        if (p_type != 1 /*PT_LOAD*/ || p_memsz == 0)
            continue;

        if (p_vaddr < VM_MAIN_MEM_BASE ||
            p_vaddr + p_memsz > (uint64_t)VM_MAIN_MEM_BASE + VM_MAIN_MEM_SIZE) {
            fprintf(stderr, "ERROR: segment %u [0x%llX +0x%llX] outside main memory\n",
                    i, (unsigned long long)p_vaddr, (unsigned long long)p_memsz);
            fclose(f);
            return -1;
        }

        if (p_filesz) {
            if (fseek(f, (long)p_offset, SEEK_SET) != 0 ||
                fread(vm_base + p_vaddr, 1, (size_t)p_filesz, f) != (size_t)p_filesz) {
                fprintf(stderr, "ERROR: short read on segment %u\n", i);
                fclose(f);
                return -1;
            }
        }
        /* memsz > filesz tail (.bss) is already zero from vm_init */
    }

    fclose(f);
    *entry_out = e_entry;
    if (g_proc_param_vaddr) {
        /* segments are loaded; read malloc_pagesize (offset 0x18, BE) and
         * sdk_version (offset 0xC, BE; served by sys_process_get_sdk_version) */
        uint32_t v;
        memcpy(&v, vm_base + (uint32_t)g_proc_param_vaddr + 0x18, 4);
        v = _byteswap_ulong(v);
        if (v) g_malloc_pagesize = v;
        memcpy(&v, vm_base + (uint32_t)g_proc_param_vaddr + 0x0C, 4);
        v = _byteswap_ulong(v);
        if (v && v != 0xFFFFFFFFu) g_ps3_sdk_version = v;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Title menu text
 *
 * The English title-menu labels are ordinary NUL-terminated strings in the
 * EBOOT data segment.  ONLINE FEATURES has a 16-byte slot and the replacement
 * needs 20 bytes, so the label group is repacked into its existing 0x60-byte
 * area and its pointer table updated.  An EBOOT whose strings or pointers
 * differ from the expected layout is left untouched.
 * -----------------------------------------------------------------------*/
static void apply_builtin_text_overrides(void)
{
    static constexpr uint32_t kOnlinePointer     = 0x01130004u;
    static constexpr uint32_t kPackedStrings     = 0x01130008u;
    static constexpr uint32_t kPackedStringsSize = 0x60u;
    static constexpr uint32_t kMenuPointers      = 0x01130068u;

    static constexpr uint32_t kOriginalPointers[] = {
        0x01130008u, /* NEW GAME */
        0x01130018u, /* LOAD GAME */
        0x01130028u, /* PREMIUM NEW GAME */
        0x0112FFD4u, /* PREMIUM ADVENTURE */
        0x01130040u, /* ULTIMATE BATTLE */
        0x01130050u, /* REMINISCE */
        0x0112FFBCu, /* SPECIAL GAMES */
        0x0112FFF4u, /* ONLINE FEATURES */
        0x01130060u, /* OPTIONS */
    };

    if (memcmp(vm_base + 0x0112FFF4u, "ONLINE FEATURES", 16) != 0 ||
        be32(vm_base + kOnlinePointer) != 0x0112FFF4u) {
        fprintf(stderr,
                "[mods] title text override skipped: EBOOT string baseline differs\n");
        return;
    }
    for (size_t i = 0; i < sizeof(kOriginalPointers) / sizeof(kOriginalPointers[0]); i++) {
        if (be32(vm_base + kMenuPointers + (uint32_t)i * 4u) != kOriginalPointers[i]) {
            fprintf(stderr,
                    "[mods] title text override skipped: EBOOT pointer baseline differs\n");
            return;
        }
    }

    static constexpr char kRecompiled[] = "Recompiled by Caner";
    static constexpr char kNewGame[] = "NEW GAME";
    static constexpr char kLoadGame[] = "LOAD GAME";
    static constexpr char kPremiumNewGame[] = "PREMIUM NEW GAME";
    static constexpr char kUltimateBattle[] = "ULTIMATE BATTLE";
    static constexpr char kReminisce[] = "REMINISCE";
    static constexpr char kOptions[] = "OPTIONS";

    uint8_t packed[kPackedStringsSize] = {};
    uint32_t cursor = 0;
    uint32_t replacement_pointers[9] = {};
    const auto append = [&](const char* text, size_t size) -> uint32_t {
        const uint32_t address = kPackedStrings + cursor;
        memcpy(packed + cursor, text, size);
        cursor += (uint32_t)size;
        return address;
    };

    const uint32_t recompiled = append(kRecompiled, sizeof(kRecompiled));
    replacement_pointers[0] = append(kNewGame, sizeof(kNewGame));
    replacement_pointers[1] = append(kLoadGame, sizeof(kLoadGame));
    replacement_pointers[2] = append(kPremiumNewGame, sizeof(kPremiumNewGame));
    replacement_pointers[4] = append(kUltimateBattle, sizeof(kUltimateBattle));
    replacement_pointers[5] = append(kReminisce, sizeof(kReminisce));
    replacement_pointers[8] = append(kOptions, sizeof(kOptions));
    replacement_pointers[3] = kOriginalPointers[3];
    replacement_pointers[6] = kOriginalPointers[6];
    replacement_pointers[7] = recompiled;

    if (cursor > kPackedStringsSize) {
        fprintf(stderr, "[mods] title text override skipped: packed strings overflow\n");
        return;
    }

    memcpy(vm_base + kPackedStrings, packed, sizeof(packed));
    {
        const uint32_t value = _byteswap_ulong(recompiled);
        memcpy(vm_base + kOnlinePointer, &value, sizeof(value));
    }
    for (size_t i = 0; i < sizeof(replacement_pointers) / sizeof(replacement_pointers[0]); i++) {
        const uint32_t value = _byteswap_ulong(replacement_pointers[i]);
        memcpy(vm_base + kMenuPointers + (uint32_t)i * 4u, &value, sizeof(value));
    }

    printf("[mods] title menu: ONLINE FEATURES -> %s\n", kRecompiled);
}

/* The game's own shader module (pxd_shader), relocated to a fixed base.  Its
 * image holds the TOC and module_start descriptor that the sys_prx overrides
 * call when the game loads the module. */
static int load_prx_image(const char* path, uint32_t base)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "ERROR: cannot open %s (produced by tools/lift_prx.py; "
                "the lifted shader code needs it)\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 ||
        (uint64_t)base < VM_MAIN_MEM_BASE ||
        (uint64_t)base + (uint64_t)size > (uint64_t)VM_MAIN_MEM_BASE + VM_MAIN_MEM_SIZE) {
        fprintf(stderr, "ERROR: %s [0x%08X +0x%lX] outside main memory\n",
                path, base, size);
        fclose(f);
        return -1;
    }
    if (fread(vm_base + base, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "ERROR: short read on %s\n", path);
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

/* Register the exact bytes of every SPU program with the native SPURS
 * workload table, so task and job launches resolve to their lifted image. */
static int register_native_spurs_images(void)
{
    spu_workload_reset();
    for (unsigned i = 0; i < g_spu_task_image_count; ++i) {
        const spu_task_image& d = g_spu_task_images[i];
        const uint8_t* image = vm_base + d.elf_ea;
        const size_t size = spu_elf_image_size(image, 16u * 1024u * 1024u);
        if (!size || !spu_workload_register_image_bytes(
                spu_workload_fingerprint(image, size), image, (uint32_t)size,
                d.image_id, d.entry, d.name)) {
            fprintf(stderr,
                    "[native-spurs] failed to register exact task image "
                    "ea=0x%08X id=%d name=%s\n",
                    d.elf_ea, d.image_id, d.name);
            return 0;
        }
    }
    for (unsigned i = 0; i < g_spu_job_binary_count; ++i) {
        const spu_job_binary& d = g_spu_job_binaries[i];
        const uint8_t* image = vm_base + d.ea;
        if (!spu_workload_register_image_bytes(
                spu_workload_fingerprint(image, d.size), image, d.size,
                d.image_id, 0x4c00u, d.name)) {
            fprintf(stderr,
                    "[native-spurs] failed to register exact job image "
                    "ea=0x%08X id=%d name=%s\n",
                    d.ea, d.image_id, d.name);
            return 0;
        }
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 * Guest callbacks (HLE modules calling into lifted code)
 * -----------------------------------------------------------------------*/

static vm_stack_alloc g_stacks;

/* Call a guest function through its OPD on a per-thread callback stack and
 * return its r3 (sceNpTrophyRegisterContext's status callback aborts the
 * registration on a negative return).  guest_caller() drops the result. */
static uint64_t guest_caller_ret(uint32_t opd_addr,
                                 uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
    /* One lazily-allocated guest stack + context per host thread. */
    static thread_local ppu_context cb_ctx;
    static thread_local uint32_t cb_stack = 0;
    if (!cb_stack) {
        cb_stack = vm_stack_allocate(&g_stacks, 256 * 1024);
        if (!cb_stack) { fprintf(stderr, "[boot] callback stack alloc failed\n"); return 0; }
        cb_ctx.gpr[1] = ((uint64_t)cb_stack + 256 * 1024 - 0x100) & ~0xFull;
        /* Null back-chain terminator: a guest stack walker (an assertion
         * handler, for one) follows [r1] until it reads 0, and would otherwise
         * run off the top of the callback stack. */
        vm_write64(cb_ctx.gpr[1], 0);
    }
    /* Start every call from a clean context, so a previous callback's
     * registers or reservation cannot leak in.  Keep only the stack pointer,
     * and set thread_id so lv2 mutex ownership is not keyed on tid 0. */
    uint64_t cb_sp = cb_ctx.gpr[1];
    memset(&cb_ctx, 0, sizeof(cb_ctx));
    cb_ctx.gpr[1] = cb_sp;
    cb_ctx.thread_id = yz_thread_current_id();
    cb_ctx.gpr[3] = a0;
    cb_ctx.gpr[4] = a1;
    cb_ctx.gpr[5] = a2;
    cb_ctx.gpr[6] = a3;
    cb_ctx.gpr[7] = a4;
    cb_ctx.gpr[8] = a5;
    cb_ctx.gpr[9] = a6;
    cb_ctx.gpr[10] = a7;
    ppu_context* prev = g_yz_cur_ctx;
    g_yz_cur_ctx = &cb_ctx;
    yz_call_guest_opd(opd_addr, &cb_ctx);
    g_yz_cur_ctx = prev;
    return cb_ctx.gpr[3];
}

static void guest_caller(uint32_t opd_addr,
                         uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
    (void)guest_caller_ret(opd_addr, a0, a1, a2, a3, a4, a5, a6, a7);
}

/* ---------------------------------------------------------------------------
 * Vblank
 *
 * The game's frame loop polls the vblank count and flip completion that the
 * RSX vblank interrupt updates on hardware.  yz_rsx_vblank_tick publishes
 * both; it does nothing until sys_rsx has set up the driver info, so the
 * thread can start early.  Ticks follow an absolute 59.94 Hz schedule:
 * sleep until shortly before the deadline, then spin the remainder.
 * -----------------------------------------------------------------------*/

static DWORD WINAPI vblank_thread(LPVOID)
{
    yz_thread_adopt_host("vblank");
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    const double period = (double)freq.QuadPart / 59.94;
    double next = (double)now.QuadPart + period;
    for (;;) {
        const uint64_t deadline = (uint64_t)next;
        QueryPerformanceCounter(&now);
        const double remain_ms =
            (next - (double)now.QuadPart) / (double)freq.QuadPart * 1000.0;
        if (remain_ms > 2.0)
            Sleep((DWORD)(remain_ms - 1.5));
        do {
            QueryPerformanceCounter(&now);
        } while ((double)now.QuadPart < next);
        next += period;
        yz_rsx_vblank_tick();
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Symbolization
 *
 * The function table is sorted by guest address, but lifted functions are
 * laid out per chunk object, so mapping a host address back to its function
 * needs a view sorted by host pointer.  Within a chunk functions are back to
 * back, so the next entry bounds this one; at a chunk boundary a span cap
 * keeps runtime code from being attributed to the nearest lifted function.
 * -----------------------------------------------------------------------*/

static const yz_func_entry** g_host_index = nullptr;
static unsigned g_host_index_count = 0;

static int host_index_compare(const void* a, const void* b)
{
    const yz_func_entry* ea = *(const yz_func_entry* const*)a;
    const yz_func_entry* eb = *(const yz_func_entry* const*)b;
    if ((uintptr_t)ea->fn < (uintptr_t)eb->fn) return -1;
    if ((uintptr_t)ea->fn > (uintptr_t)eb->fn) return 1;
    return 0;
}

static void build_host_index(void)
{
    g_host_index = (const yz_func_entry**)malloc(sizeof(void*) * (g_yz_func_count ? g_yz_func_count : 1));
    unsigned n = 0;
    for (unsigned i = 0; i < g_yz_func_count; i++)
        if (g_yz_func_table[i].fn) g_host_index[n++] = &g_yz_func_table[i];
    qsort(g_host_index, n, sizeof(void*), host_index_compare);
    g_host_index_count = n;
}

static INIT_ONCE g_host_index_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK build_host_index_once(PINIT_ONCE, PVOID, PVOID*)
{
    build_host_index();
    return TRUE;
}

static const yz_func_entry* func_from_host(const void* rip)
{
    InitOnceExecuteOnce(&g_host_index_once, build_host_index_once, NULL, NULL);
    if (!g_host_index_count) return nullptr;
    unsigned lo = 0, hi = g_host_index_count;       /* greatest fn <= rip in [lo,hi) */
    while (lo < hi) {
        unsigned mid = (lo + hi) >> 1;
        if ((uintptr_t)g_host_index[mid]->fn <= (uintptr_t)rip) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return nullptr;                    /* below all lifted functions */
    const yz_func_entry* best = g_host_index[lo - 1];
    uintptr_t end = (uintptr_t)best->fn + 0x80000;  /* chunk-boundary span cap (512 KB) */
    if (lo < g_host_index_count) {                  /* within a chunk: next fn = this fn's end */
        uintptr_t nxt = (uintptr_t)g_host_index[lo]->fn;
        if (nxt < end) end = nxt;
    }
    if ((uintptr_t)rip >= end) return nullptr;
    return best;
}

/* The eight guest accessors (shims.cpp) are linked between lifted chunks, so
 * a fault inside one would otherwise be attributed to the preceding lifted
 * function.  Check them first; each span ends at the next helper, capped at
 * 0x400 bytes. */
static const char* vm_helper_from_host(const void* rip, unsigned* off_out)
{
    struct ent { uintptr_t a; const char* n; };
    static ent tab[8];
    static int n = 0;
    if (!n) {
        ent t[8] = {
            { (uintptr_t)&vm_read8,   "vm_read8"   },
            { (uintptr_t)&vm_read16,  "vm_read16"  },
            { (uintptr_t)&vm_read32,  "vm_read32"  },
            { (uintptr_t)&vm_read64,  "vm_read64"  },
            { (uintptr_t)&vm_write8,  "vm_write8"  },
            { (uintptr_t)&vm_write16, "vm_write16" },
            { (uintptr_t)&vm_write32, "vm_write32" },
            { (uintptr_t)&vm_write64, "vm_write64" },
        };
        for (int i = 0; i < 8; i++) {           /* insertion sort by address */
            int j = i;
            while (j > 0 && t[j - 1].a > t[j].a) { ent s = t[j]; t[j] = t[j - 1]; t[j - 1] = s; j--; }
        }
        memcpy(tab, t, sizeof(t));
        n = 8;
    }
    uintptr_t r = (uintptr_t)rip;
    for (int i = n - 1; i >= 0; i--) {
        if (r < tab[i].a) continue;
        uintptr_t end = (i + 1 < n) ? tab[i + 1].a : tab[i].a + 0x400;
        if (end > tab[i].a + 0x400) end = tab[i].a + 0x400;
        if (r < end) {
            if (off_out) *off_out = (unsigned)(r - tab[i].a);
            return tab[i].n;
        }
        break;
    }
    return nullptr;
}

/* The lifted function whose guest address range contains `addr`. */
static const yz_func_entry* func_from_guest(uint32_t addr)
{
    unsigned lo = 0, hi = g_yz_func_count;
    const yz_func_entry* best = nullptr;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (g_yz_func_table[mid].addr <= addr) {
            best = &g_yz_func_table[mid];
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return best;
}

/* Guest registers and the saved-LR back-chain (8-byte aligned, climbing,
 * LR at back-chain + 0x10). */
static void dump_guest_state(const ppu_context* gc, const char* tag)
{
    if (!gc) return;
    fprintf(stderr, "\n[%s] guest cia=0x%08X lr=0x%08X ctr=0x%08X r1=0x%08X",
            tag, (uint32_t)gc->cia, (uint32_t)gc->lr, (uint32_t)gc->ctr,
            (uint32_t)gc->gpr[1]);
    fprintf(stderr, "\n[%s] guest gpr:", tag);
    for (int r = 0; r < 32; r++) {
        if (r % 8 == 0) fprintf(stderr, "\n    r%-2d:", r);
        fprintf(stderr, " %016llX", (unsigned long long)gc->gpr[r]);
    }
    fprintf(stderr, "\n[%s] guest call chain:", tag);
    uint32_t sp = (uint32_t)gc->gpr[1];
    int frames = 0;
    for (int depth = 0; depth < 64 && frames < 24; depth++) {
        if (sp < 0x10000u || (sp & 7) || IsBadReadPtr(vm_base + sp, 8)) break;
        const uint32_t back = (uint32_t)vm_read64(sp);
        if ((back & 7) || back <= sp || back - sp > 0x100000u) break;
        if (IsBadReadPtr(vm_base + back + 0x10, 8)) break;
        const uint32_t lr = (uint32_t)vm_read64(back + 0x10);
        /* EBOOT text, or the shader module at 0x02200000. */
        const bool code = (lr >= 0x00010000u && lr < 0x01310768u) ||
                          (lr >= 0x02000000u && lr < 0x02400000u);
        if (!(lr & 3u) && code) {
            fprintf(stderr, " 0x%08X", lr);
            if (const yz_func_entry* fe = func_from_guest(lr))
                fprintf(stderr, "(f%08X+0x%X)", fe->addr, lr - fe->addr);
            frames++;
        }
        sp = back;
    }
    fprintf(stderr, " [frames=%d]\n", frames);
}

/* ---------------------------------------------------------------------------
 * Crash report
 * -----------------------------------------------------------------------*/

/* Name a host code address: a guest accessor, a lifted function, or its RVA
 * (resolve those against yakuza_recomp.map). */
static void print_host_frame(const void* address)
{
    unsigned offset = 0;
    if (const char* helper = vm_helper_from_host(address, &offset)) {
        fprintf(stderr, "%s +0x%X", helper, offset);
    } else if (const yz_func_entry* fe = func_from_host(address)) {
        fprintf(stderr, "func_%08X +0x%llX", fe->addr,
                (unsigned long long)((uintptr_t)address - (uintptr_t)fe->fn));
    } else {
        fprintf(stderr, "rva 0x%llX",
                (unsigned long long)((uintptr_t)address -
                                     (uintptr_t)GetModuleHandleW(NULL)));
    }
}

static LONG WINAPI crash_handler(EXCEPTION_POINTERS* ep)
{
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    fprintf(stderr, "\n[crash] exception 0x%08lX on tid=%u at ",
            er->ExceptionCode, yz_thread_current_id());
    print_host_frame(er->ExceptionAddress);
    {
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* sym = (SYMBOL_INFO*)buf;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (SymFromAddr(GetCurrentProcess(),
                        (DWORD64)(uintptr_t)er->ExceptionAddress, &disp, sym))
            fprintf(stderr, " [%s+0x%llX]", sym->Name, (unsigned long long)disp);
    }
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        const uint64_t va = (uint64_t)er->ExceptionInformation[1];
        fprintf(stderr, ", %s host 0x%llX",
                er->ExceptionInformation[0] ? "writing" : "reading",
                (unsigned long long)va);
        if (vm_base && va >= (uint64_t)vm_base && va < (uint64_t)vm_base + 0x100000000ull)
            fprintf(stderr, " (guest 0x%08llX)", (unsigned long long)(va - (uint64_t)vm_base));
    }
    /* SPU host threads carry no guest thread id; name the SPU instead. */
    yz_spu_crash_note();

    void* frames[80];
    const USHORT count = RtlCaptureStackBackTrace(0, 80, frames, NULL);
    fprintf(stderr, "\n[crash] host call stack (%u frames, newest first):", count);
    for (USHORT i = 0; i < count; i++) {
        fprintf(stderr, "\n    ");
        print_host_frame(frames[i]);
    }

    fprintf(stderr, "\n[crash] threads in a syscall (tid num a3 a4 a5 age):");
    for (uint32_t tid = 1; tid < 256; tid++) {
        uint32_t num, age;
        uint64_t a3, a4, a5;
        if (yz_wait_get(tid, &num, &a3, &a4, &a5, &age))
            fprintf(stderr, "\n    t%u sc%u 0x%llX 0x%llX 0x%llX %ums",
                    tid, num, (unsigned long long)a3,
                    (unsigned long long)a4, (unsigned long long)a5, age);
    }

    dump_guest_state(g_yz_cur_ctx, "crash");
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* ---------------------------------------------------------------------------
 * Main
 * -----------------------------------------------------------------------*/

int main(int argc, char** argv)
{
    const char* elf_path = (argc > 1) ? argv[1] : "game/EBOOT.elf";

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* Boot pacing depends on Sleep()-based polls.  Use a 1 ms timer, raise
     * the priority and opt out of EcoQoS throttling, so a boot behind other
     * windows is scheduled like a focused one. */
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    {
        PROCESS_POWER_THROTTLING_STATE throttling;
        memset(&throttling, 0, sizeof(throttling));
        throttling.Version     = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        throttling.StateMask   = 0;
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
                              &throttling, sizeof(throttling));
    }

    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    SetUnhandledExceptionFilter(crash_handler);

    printf("=== Yakuza: Dead Souls recomp runner ===\n");

    if (vm_init() != 0) {
        fprintf(stderr, "ERROR: vm_init failed\n");
        return 1;
    }
    vm_stack_alloc_init(&g_stacks);

    uint64_t e_entry = 0;
    if (load_elf(elf_path, &e_entry) != 0)
        return 1;
    apply_builtin_text_overrides();
    load_prx_image("recomp_prx/ogrez_shader_ps3.ppu_image.bin", 0x02200000u);


    spu_register_lifted_images();
    yz_native_scene_jobs_init();
    if (!register_native_spurs_images())
        return 1;

    /* e_entry points at an OPD: word 0 is the code address, word 1 the TOC. */
    const uint32_t entry_code = vm_read32(e_entry);
    const uint32_t entry_toc  = vm_read32(e_entry + 4);
    g_yz_game_toc = entry_toc;

    yz_ppu_fn entry_fn = yz_lookup_func(entry_code);
    if (!entry_fn) {
        fprintf(stderr, "ERROR: entry 0x%08X not in function table (%u funcs)\n",
                entry_code, g_yz_func_count);
        return 1;
    }

    yz_init_syscalls();
    yz_install_imports();
    g_ps3_guest_caller = guest_caller;
    g_ps3_guest_caller_ret = guest_caller_ret;

    /* Title id, title and version for cellGame and cellDiscGame. */
    cellGame_init_from_paramsfo("gamedata/dev_bdvd/PS3_GAME/PARAM.SFO");

    const uint32_t stack_base = vm_stack_allocate(&g_stacks, 1024 * 1024);
    if (!stack_base) {
        fprintf(stderr, "ERROR: stack allocation failed\n");
        return 1;
    }
    yz_threads_init(stack_base, 1024 * 1024);

    static ppu_context ctx;
    ctx.thread_id = 1;
    ctx.gpr[1] = ((uint64_t)stack_base + 1024 * 1024 - 0x100) & ~0xFull;
    ctx.gpr[2] = entry_toc;
    vm_write64(ctx.gpr[1], 0);   /* null back-chain */

    /* Process arguments in guest scratch memory.  lv2 passes argv and envp as
     * arrays of 64-bit pointers that the CRT compacts in place, so they must
     * be real memory even with no extra arguments. */
    const uint32_t args_base = 0x0FDF0000u;
    const char*    pname     = "/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN";
    {
        const uint32_t str = args_base + 0x100;
        for (size_t i = 0; i <= strlen(pname); i++)
            vm_write8(str + (uint32_t)i, (uint8_t)pname[i]);
        vm_write64(args_base + 0x00, str);  /* argv[0] */
        vm_write64(args_base + 0x08, 0);    /* argv terminator */
        vm_write64(args_base + 0x40, 0);    /* envp terminator */
    }

    /* Process entry registers: r3 argc, r4 argv, r5 envp, r6 0, r7 main
     * thread id, r8-r10 TLS template, r11 entry descriptor, r12 malloc page
     * size. */
    ctx.gpr[3]  = 1;
    ctx.gpr[4]  = args_base;
    ctx.gpr[5]  = args_base + 0x40;
    ctx.gpr[6]  = 0;
    ctx.gpr[7]  = 1;
    ctx.gpr[8]  = yz_tls_vaddr;
    ctx.gpr[9]  = yz_tls_filesz;
    ctx.gpr[10] = yz_tls_memsz;
    ctx.gpr[11] = e_entry;
    ctx.gpr[12] = g_malloc_pagesize;

    CreateThread(NULL, 0, vblank_thread, NULL, 0, NULL);

    g_yz_cur_ctx = &ctx;
    entry_fn(&ctx);
    yz_drain_trampolines(&ctx);

    printf("[boot] entry returned, r3=0x%llX\n", (unsigned long long)ctx.gpr[3]);
    /* The process is ending: keep FIFO visitors out through renderer and VM
     * teardown, since live workers must not enter destroyed renderer state. */
    yz_rsx_fifo_acquire();
    vm_shutdown();
    return (int)(uint32_t)ctx.gpr[3];
}
