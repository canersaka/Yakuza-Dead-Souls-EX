/*
 * Yakuza: Dead Souls -- recompiled EBOOT runner.
 *
 * Boot sequence:
 *   1. vm_init()                         reserve 4 GB guest space, commit RAM
 *   2. load PT_LOAD segments             from game/EBOOT.elf into guest memory
 *   3. resolve entry OPD                 e_entry -> { code addr, TOC }
 *   4. register lv2 syscalls, install guest-callback hook
 *   5. allocate guest stack, build ppu_context
 *   6. call the recompiled entry function, drain trampolines
 *
 * This uses the GENERATED ppu_context from ppu_recomp.h (the layout
 * ppu_recomp.obj was compiled against) -- see yakuza_runner.h.
 */

#include "ppu_recomp.h"
#include "yakuza_runner.h"
#include "../runtime/memory/vm.h"
#include "../include/ps3emu/guest_call.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

/* Live guest context for the current host thread, set at each guest-thread
 * entry / callback. The crash handler walks its PPC64 back-chain to print a
 * real guest call stack (direct bl calls are invisible to the dispatcher, so
 * the host return-address scan alone misses most of the guest chain). */
thread_local ppu_context* g_yz_cur_ctx = nullptr;

/* Game module TOC (set from the entry OPD); used by the dispatcher's TOC repair
 * (dispatch.cpp yz_tramp_guard). */
extern "C" uint32_t g_yz_game_toc;

/* ---------------------------------------------------------------------------
 * Minimal big-endian ELF64 loader (PT_LOAD only)
 * -----------------------------------------------------------------------*/

static uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t be64(const uint8_t* p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

/* PT_TLS template info, forwarded to the guest entry in r8/r9/r10
 * (the CRT passes them through to sys_initialize_tls). */
/* TLS template, shared with threads.cpp (per-thread TLS blocks) */
extern "C" uint64_t yz_tls_vaddr = 0, yz_tls_filesz = 0, yz_tls_memsz = 0;
#define g_tls_vaddr  yz_tls_vaddr
#define g_tls_filesz yz_tls_filesz
#define g_tls_memsz  yz_tls_memsz
/* From PT_PROC_PARAM (0x60000001), sys_process_param_t.malloc_pagesize.
 * lv2 passes it to the entry point in r12; the CRT stores it into the libc
 * malloc config, where 0 makes every malloc fail. Default is 1 MB when the
 * segment is absent, same as RPCS3's loader. */
static uint32_t g_malloc_pagesize = 0x100000;
static uint64_t g_proc_param_vaddr = 0;

/* runtime/syscalls/lv2_register.c — served by sys_process_get_sdk_version */
extern "C" uint32_t g_ps3_sdk_version;

/* recomp_prx/spurs_kernel2.c + spurs_sysservice.c (generated, C) — register the
 * lifted SPURS kernel and its system-service workload for SPU indirect-branch
 * dispatch. The kernel DMAs the service to LS 0xA00 and branches there. */
extern "C" void spu_recomp_register(void);
extern "C" void spu_recomp_register_sysservice(void);
/* recomp_prx/gs_task.c (generated) — the game's Edge geometry render task
 * (gs_task.elf, EBOOT SPU img #3 @0x0127A580, LS base 0x3000, entry 0x3050).
 * Registered so spu_indirect_branch runs it once the SPURS kernel dispatches
 * the taskset that owns it (1f). */
extern "C" void spu_recomp_register_gstask(void);
/* recomp_prx/policy_module.c (generated) — the SPURS taskset POLICY module
 * (libsre ea 0x02023680). The kernel DMAs it to LS 0xA00 -- SAME address as the
 * system service -- so they must register under DISTINCT image ids and the
 * runtime switches ctx->image_id by DMA source EA (spu_dma.h). */
extern "C" void spu_recomp_register_policy(void);
extern "C" void spu_begin_image(int image_id);
/* runtime/spu/spu_channels.c — SPU spin-profiler gate (env YZ_SPU_PROF) */
extern "C" int g_spu_prof_on;
extern "C" int g_yz_watch_dlist;

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
            g_tls_vaddr  = p_vaddr;
            g_tls_filesz = p_filesz;
            g_tls_memsz  = p_memsz;
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

        printf("[boot] PT_LOAD %u: vaddr 0x%08llX filesz 0x%llX memsz 0x%llX\n",
               i, (unsigned long long)p_vaddr,
               (unsigned long long)p_filesz, (unsigned long long)p_memsz);
    }

    fclose(f);
    *entry_out = e_entry;
    if (g_proc_param_vaddr) {
        /* segments are loaded; read malloc_pagesize (offset 0x18, BE) and
         * sdk_version (offset 0xC, BE — served by sys_process_get_sdk_version) */
        uint32_t v;
        memcpy(&v, vm_base + (uint32_t)g_proc_param_vaddr + 0x18, 4);
        v = _byteswap_ulong(v);
        if (v) g_malloc_pagesize = v;
        memcpy(&v, vm_base + (uint32_t)g_proc_param_vaddr + 0x0C, 4);
        v = _byteswap_ulong(v);
        if (v && v != 0xFFFFFFFFu) g_ps3_sdk_version = v;
        printf("[boot] PROC_PARAM: malloc_pagesize=0x%X sdk_version=0x%X\n",
               g_malloc_pagesize, g_ps3_sdk_version);
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * LLE firmware module loader (flat relocated image from tools/lift_prx.py)
 * -----------------------------------------------------------------------*/

static int load_prx_image(const char* path, uint32_t base)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "ERROR: cannot open %s -- regenerate with "
                "tools/lift_prx.py (the lifted code in this build expects it)\n",
                path);
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
    printf("[boot] LLE module %s: 0x%08X +0x%lX\n", path, base, size);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Guest-callback adapter (HLE modules -> recompiled code)
 * -----------------------------------------------------------------------*/

static vm_stack_alloc g_stacks;

static void guest_caller(uint32_t opd_addr,
                         uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    /* One lazily-allocated guest stack + context per host thread. */
    static thread_local ppu_context cb_ctx;
    static thread_local uint32_t cb_stack = 0;
    if (!cb_stack) {
        cb_stack = vm_stack_allocate(&g_stacks, 256 * 1024);
        if (!cb_stack) { fprintf(stderr, "[boot] callback stack alloc failed\n"); return; }
        memset(&cb_ctx, 0, sizeof(cb_ctx));
        cb_ctx.gpr[1] = ((uint64_t)cb_stack + 256 * 1024 - 0x100) & ~0xFull;
        /* Null back-chain terminator: an SDK stack-trace walker (e.g. libsre's
         * assertion handler) follows [r1] up until it reads 0. Without this the
         * walk runs off the top of the callback stack into garbage and loops
         * forever, blowing the stack. Mirrors the main/created-thread setup. */
        vm_write64(cb_ctx.gpr[1], 0);
    }
    cb_ctx.gpr[3] = a0;
    cb_ctx.gpr[4] = a1;
    cb_ctx.gpr[5] = a2;
    cb_ctx.gpr[6] = a3;
    ppu_context* prev = g_yz_cur_ctx;
    g_yz_cur_ctx = &cb_ctx;
    yz_call_guest_opd(opd_addr, &cb_ctx);
    g_yz_cur_ctx = prev;
}

/* ---------------------------------------------------------------------------
 * Main
 * -----------------------------------------------------------------------*/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>

/* Host vblank driver. The game's frame loop polls guest state (driver_info
 * head vBlankCount + flip completion) that on real HW the RSX vblank interrupt
 * (~59.94x/s) updates. yz_rsx_vblank_tick (import_overrides.cpp) bumps those
 * counters and publishes any pending flip's done bit; it no-ops until sys_rsx
 * context_allocate has set up the driver_info, so starting early is safe. */
static DWORD WINAPI yz_vblank_thread(LPVOID)
{
    for (;;) {
        Sleep(16);            /* ~62.5 Hz; close enough to PS3 vblank */
        yz_rsx_vblank_tick();
    }
    return 0;
}

/* Map a host code address back to the lifted guest function containing it:
 * the table entry whose host fn pointer is the greatest one <= RIP. Lifted
 * functions are laid out back-to-back in the chunk objs, so nearest-below
 * is the containing function (bounded to 16 MB to reject non-lifted RIPs). */
static const yz_func_entry* yz_func_from_host(const void* rip)
{
    const yz_func_entry* best = nullptr;
    for (unsigned i = 0; i < g_yz_func_count; i++) {
        const void* fn = (const void*)g_yz_func_table[i].fn;
        if (fn <= rip && (!best || fn > (const void*)best->fn))
            best = &g_yz_func_table[i];
    }
    if (best && (uintptr_t)rip - (uintptr_t)best->fn > 0x1000000)
        return nullptr;
    return best;
}

/* ---------------------------------------------------------------------------
 * TEMP DIAG: memory write-watch (page-guard + single-step) to find the wild
 * write that zeroes the gcm-state TOC global at guest 0x0135A5C0 (only when the
 * FIFO consumer runs). Catches the exact writer's RIP across all threads.
 * -----------------------------------------------------------------------*/
static uint32_t g_watch_guest = 0;       /* 0 = disabled */
static void*    g_watch_host  = nullptr;
static int      g_watch_log_after = 0;
static int      g_watch_read  = 0;       /* 1 = read-watch (PAGE_NOACCESS, traps reads) */
static int      g_watch_hits  = 0;       /* read-watch dumps captured (disarm after N) */
static unsigned long g_watch_traps = 0;  /* total in-page traps (slowdown safety cap) */

/* Forward decls so the watch handler can name the GUEST function doing the write
 * (defined later in this TU). */
extern ppu_context* g_yz_main_ctx;
static void yz_dump_guest_state(const ppu_context* gc, const char* tag);

static LONG CALLBACK yz_watch_veh(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    uintptr_t page = (uintptr_t)g_watch_host & ~(uintptr_t)0xFFF;
    ULONG_PTR acc = (code == EXCEPTION_ACCESS_VIOLATION &&
                     ep->ExceptionRecord->NumberParameters >= 2)
                    ? ep->ExceptionRecord->ExceptionInformation[0] : 2; /* 0=read 1=write */
    if (code == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2 &&
        (acc == 1 /*write*/ || (g_watch_read && acc == 0 /*read*/))) {
        uintptr_t tgt = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        if (tgt >= page && tgt < page + 0x1000) {
            g_watch_traps++;
            /* Dump every write of the watched word; for reads (read-watch) cap to
             * the first N so a tight poll loop doesn't trap forever (we disarm
             * below once captured). Runs in the FAULTING thread's context, so the
             * dump names the real accessor + its reliable trampoline-ring chain. */
            int do_dump = (tgt == (uintptr_t)g_watch_host) &&
                          (acc == 1 || g_watch_hits < 8);
            if (do_dump) {
                if (acc == 0) g_watch_hits++;
                void* rip = (void*)ep->ContextRecord->Rip;
                uintptr_t mod = (uintptr_t)GetModuleHandleW(NULL);
                uint64_t cur;
                memcpy(&cur, g_watch_host, 8);
                fprintf(stderr, "[watch] %s guest 0x%08X (=0x%llX) rip rva 0x%llX",
                        acc == 1 ? "WRITE to" : "READ of",
                        g_watch_guest, (unsigned long long)cur,
                        (unsigned long long)((uintptr_t)rip - mod));
                if (const yz_func_entry* fe = yz_func_from_host(rip))
                    fprintf(stderr, " (func_%08X +0x%llX)", fe->addr,
                            (unsigned long long)((uintptr_t)rip - (uintptr_t)fe->fn));
                fprintf(stderr, " tid=%lu\n", GetCurrentThreadId());
                { uint32_t wtid = yz_thread_current_id();
                  ppu_context* wctx = (ppu_context*)yz_thread_context(wtid);
                  if (!wctx) wctx = g_yz_main_ctx;   /* tid 1 = main */
                  fprintf(stderr, "[watch] accessor guest tid=%u\n", wtid);
                  if (wctx) yz_dump_guest_state(wctx, acc == 1 ? "watch-writer" : "watch-reader"); }
                /* Caller chain: the host return addresses name who wanted this
                 * access (consumer / lifted PPU / SPU DMA). rva for the .map. */
                const uint64_t* sp = (const uint64_t*)ep->ContextRecord->Rsp;
                int shown = 0;
                for (int i = 0; i < 48 && shown < 8; i++) {
                    if (IsBadReadPtr((void*)(sp + i), 8)) break;
                    uint64_t v = sp[i];
                    if (v < mod || v > mod + 0x40000000ull) continue;
                    const yz_func_entry* fe = yz_func_from_host((void*)v);
                    fprintf(stderr, "    caller[+0x%X] rva 0x%llX", i * 8,
                            (unsigned long long)(v - mod));
                    if (fe && fe->addr != 0xFFFFFFE4u)
                        fprintf(stderr, " (func_%08X +0x%llX)", fe->addr,
                                (unsigned long long)(v - (uintptr_t)fe->fn));
                    fprintf(stderr, "\n");
                    shown++;
                }
                fflush(stderr);
                if (acc == 1) g_watch_log_after = 1;   /* writes: log new value */
            }
            DWORD old;
            VirtualProtect((void*)page, 0x1000, PAGE_READWRITE, &old);
            ep->ContextRecord->EFlags |= 0x100;   /* trap flag -> single-step */
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code == EXCEPTION_SINGLE_STEP && g_watch_host) {
        if (g_watch_log_after) {
            g_watch_log_after = 0;
            uint64_t after;
            memcpy(&after, g_watch_host, 8);
            fprintf(stderr, "[watch]   -> now 0x%llX\n", (unsigned long long)after);
            fflush(stderr);
        }
        DWORD old;
        /* Re-arm. Read-watch disarms (PAGE_READWRITE = stop trapping) once it has
         * captured enough reads, or as a slowdown safety after many in-page traps. */
        DWORD prot = !g_watch_read ? PAGE_READONLY
                   : (g_watch_hits >= 8 || g_watch_traps > 4000000ul) ? PAGE_READWRITE
                                                                      : PAGE_NOACCESS;
        VirtualProtect((void*)page, 0x1000, prot, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

extern "C" void yz_watch_arm(uint32_t guest_addr)
{
    if (g_watch_guest) return;   /* arm once */
    g_watch_guest = guest_addr;
    g_watch_host  = vm_base + guest_addr;
    AddVectoredExceptionHandler(1, yz_watch_veh);
    DWORD old;
    VirtualProtect((void*)((uintptr_t)g_watch_host & ~(uintptr_t)0xFFF),
                   0x1000, PAGE_READONLY, &old);
    fprintf(stderr, "[watch] armed write-watch on guest 0x%08X (host %p)\n",
            guest_addr, g_watch_host);
}

/* Read-watch: trap READS of guest_addr (PAGE_NOACCESS) so the VEH fires in the
 * READER's context -> reliable trampoline-ring call chain naming what polls it
 * (e.g. t1's flip-fence wait). Disarms after capturing a few (see veh). */
extern "C" void yz_watch_arm_read(uint32_t guest_addr)
{
    if (g_watch_guest) return;   /* arm once */
    g_watch_guest = guest_addr;
    g_watch_host  = vm_base + guest_addr;
    g_watch_read  = 1;
    AddVectoredExceptionHandler(1, yz_watch_veh);
    DWORD old;
    VirtualProtect((void*)((uintptr_t)g_watch_host & ~(uintptr_t)0xFFF),
                   0x1000, PAGE_NOACCESS, &old);
    fprintf(stderr, "[watch] armed READ-watch on guest 0x%08X (host %p) -- PAGE_NOACCESS\n",
            guest_addr, g_watch_host);
}

/* Map a guest code address back to its lifted function: the table entry whose
 * guest addr is the greatest one <= the address (the table is sorted by addr).
 * Used to symbolize back-chain return addresses in the crash handler. */
static const yz_func_entry* yz_func_from_guest(uint32_t addr)
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

/* Dump guest registers + walk the PPC64 back-chain (per-frame code pointers +
 * total depth). Shared by the crash handler and the stall watchdog. */
static void yz_dump_guest_state(const ppu_context* gc, const char* tag)
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
    const uint32_t EXE_LO = 0x00010000u, EXE_HI = 0x01310768u;
    fprintf(stderr, "\n[%s] guest back-chain (per-frame code pointers):", tag);
    uint32_t sp = (uint32_t)gc->gpr[1];
    int total = 0;
    for (int depth = 0; depth < 1200 && sp >= 0x10000u; depth++) {
        if (vm_base && IsBadReadPtr(vm_base + sp, 8)) break;
        uint32_t back = (uint32_t)vm_read64(sp);
        if (back <= sp || back < 0x10000u) break;   /* must climb upward */
        total++;
        if (depth < 10) {
            fprintf(stderr, "\n    #%-2d sp=0x%08X size=0x%X:", depth, sp, back - sp);
            uint32_t lim = back - sp; if (lim > 0x120) lim = 0x120;
            for (uint32_t off = 0; off + 8 <= lim; off += 8) {
                if (vm_base && IsBadReadPtr(vm_base + sp + off, 8)) break;
                uint32_t v = (uint32_t)vm_read64(sp + off);
                if (v >= EXE_LO && v < EXE_HI) {
                    fprintf(stderr, "\n        +0x%02X -> 0x%08X", off, v);
                    if (const yz_func_entry* fe = yz_func_from_guest(v))
                        fprintf(stderr, " (func_%08X +0x%X)", fe->addr, v - fe->addr);
                }
            }
        }
        sp = back;
    }
    fprintf(stderr, "\n[%s] back-chain total frames: %d\n", tag, total);

    /* TEMP DEBUG (flip stall): dump memory at the pointer-like registers + the
     * known gcm sync locations, so we can see exactly which value the parked
     * render thread is polling for "flip done". */
    auto dump_at = [&](const char* nm, uint32_t a) {
        if (a < 0x10000u || a >= 0xE0000000u) return;
        if (vm_base && IsBadReadPtr(vm_base + a, 16)) return;
        fprintf(stderr, "    [%s] @0x%08X:", nm, a);
        for (int i = 0; i < 8; i++) fprintf(stderr, " %08X", vm_read32(a + i * 4));
        fprintf(stderr, "\n");
    };
    fprintf(stderr, "[%s] polled-memory probe:\n", tag);
    dump_at("r3", (uint32_t)gc->gpr[3]);
    dump_at("r4", (uint32_t)gc->gpr[4]);
    dump_at("r5", (uint32_t)gc->gpr[5]);
    dump_at("r6", (uint32_t)gc->gpr[6]);
    dump_at("r29", (uint32_t)gc->gpr[29]);
    dump_at("r30", (uint32_t)gc->gpr[30]);
    dump_at("r31", (uint32_t)gc->gpr[31]);
    dump_at("fence", 0x40C00000u);          /* the flip/vsync counter (blocker #12) */
    dump_at("dma-ctrl@40", 0x10000040u);    /* LLE dma_control: +0x40 PUT +0x44 GET +0x48 REF */
    dump_at("driver_info", 0x10100000u);    /* RsxDriverInfo head (flipFlags/vBlankCount) */
    dump_at("reports", 0x10200000u);        /* label/report area the consumer writes */

    /* gcm command-buffer state the game's render wrapper spins on: func_00E9BC9C
     * reads pointers at TOC[-0x7410/-0x7414/-0x740C] and the space check is
     * roughly [bufdesc+8](current)+4 < [bufdesc+4](limit). Shows whether t1 is
     * waiting on a cached GET/limit that the consumer isn't refreshing. */
    /* libgcm command-buffer GEOMETRY (drives the out-of-space callback's
     * fragment math, 0x02103AAC): config = *(libgcm_TOC[-0x7FD8]). The wrap path
     * computes newBegin = config[0x14] + config[0x28]*4, newEnd via
     * config[0x30]/0x38. A degenerate newBegin==current => the self-jump that
     * parks GET. Dump the geometry to find the bad param. (libgcm TOC=0x02114000) */
    {
        uint32_t cfgp = vm_read32(0x02114000u - 0x7FD8u);   /* config struct ptr */
        fprintf(stderr, "    [gcm-cfg] *TOC[-7FD8]=0x%08X\n", cfgp);
        if (cfgp >= 0x10000u && cfgp < 0xE0000000u) {
            fprintf(stderr, "    [gcm-cfg] +10=%08X begin+14=%08X end+18=%08X +1C=%08X "
                    "+28=%08X +30=%08X +38=%08X +48=%08X\n",
                    vm_read32(cfgp+0x10), vm_read32(cfgp+0x14), vm_read32(cfgp+0x18),
                    vm_read32(cfgp+0x1C), vm_read32(cfgp+0x28), vm_read32(cfgp+0x30),
                    vm_read32(cfgp+0x38), vm_read32(cfgp+0x48));
        }
    }
    if (g_yz_game_toc) {
        uint32_t ctx10 = vm_read32(g_yz_game_toc - 0x7410);
        uint32_t ptr14 = vm_read32(g_yz_game_toc - 0x7414);
        uint32_t lim0C = vm_read32(g_yz_game_toc - 0x740C);
        fprintf(stderr, "    [gcm] ctx@-7410=0x%08X p@-7414=0x%08X p@-740C=0x%08X\n",
                ctx10, ptr14, lim0C);
        dump_at("ctx10",    ctx10);
        dump_at("ctx10+20", ctx10 + 0x20);
        dump_at("p740C",    lim0C);
        if (ptr14 >= 0x10000u && ptr14 < 0xE0000000u) {
            uint32_t bd = vm_read32(ptr14);
            fprintf(stderr, "    [gcm] bufdesc=*(p@-7414)=0x%08X (cur=[+8] limit=[+4])\n", bd);
            dump_at("bufdesc", bd);
        }
        /* DEFERRED STOPPER-RELEASE LIST (blocker #21 RE, 2026-06-14e): the game's
         * gcm flush/reserve (func_00E9BC9C/B630/BF14) places a self-jump stopper at
         * each commit and RELEASES the previous one -- immediately (write 0) iff
         * same-fragment (S[0x24]==ctx->end), else DEFERRED into a [tag=127, stopper_ea]
         * list at S[8]..S[0] (0x20 stride). If nothing drains that list, the deferred
         * stoppers (incl. the io 0x300000 one GET parks on) never release -> deadlock.
         * Dump the list + flag the entry matching GET's stopper. S = *(game_toc-0x7410). */
        if (ctx10 >= 0x10000u && ctx10 < 0xE0000000u) {
            uint32_t S = ctx10;
            uint32_t lbase = vm_read32(S + 0x08);   /* list base   */
            uint32_t lhead = vm_read32(S + 0x00);   /* list write head */
            uint32_t llim  = vm_read32(S + 0x04);   /* list limit  */
            uint32_t getio = vm_read32(0x10000044u);          /* current GET io offset */
            uint32_t getea = 0x40400000u + getio;             /* GET's stopper ea (main ring) */
            fprintf(stderr, "    [defer] S=0x%08X list base=0x%08X head=0x%08X lim=0x%08X "
                    "S[1C]=0x%08X S[20]=0x%08X S[24]=0x%08X | GET io=0x%06X ea=0x%08X\n",
                    S, lbase, lhead, llim, vm_read32(S+0x1C), vm_read32(S+0x20),
                    vm_read32(S+0x24), getio, getea);
            if (lbase >= 0x10000u && lbase < 0xE0000000u && lhead >= lbase &&
                (lhead - lbase) <= 0x4000u) {
                int n = 0, hit = -1;
                for (uint32_t e = lbase; e < lhead && n < 80; e += 0x20, n++) {
                    uint32_t tag = vm_read32(e + 0), ptr = vm_read32(e + 4);
                    int is_get = (ptr == getea);
                    if (is_get) hit = n;
                    if (n < 32 || is_get)
                        fprintf(stderr, "      [defer #%2d] tag=0x%X stopper_ea=0x%08X (io 0x%06X)%s\n",
                                n, tag, ptr, ptr - 0x40400000u, is_get ? "  <<< == GET stopper" : "");
                }
                fprintf(stderr, "    [defer] %d pending entries; GET's stopper %s in the list\n",
                        n, hit >= 0 ? "IS" : "is NOT");
            }
        }
    }
}

/* Main thread's context, exposed for the stall watchdog (thread_local
 * g_yz_cur_ctx isn't readable from another host thread). */
ppu_context* g_yz_main_ctx = nullptr;
static HANDLE g_yz_main_hthread = nullptr;

/* Suspend the main/guest thread and dump its HOST rip + stack-return
 * candidates, reverse-mapped to lifted guest functions. When the guest ctx is
 * frozen but CPU spins, this names the host (lifted) function actually
 * executing -- i.e. the spin/poll loop. (TEMP DEBUG, watchdog only.) */
/* Suspend an arbitrary guest thread and walk its HOST stack, reverse-mapping
 * return addresses to lifted guest functions -- names what each thread is
 * actually executing (the spin/wait it is parked in). (TEMP DIAG, blocker #21) */
static void yz_dump_host_stack_of(HANDLE h, const char* tag)
{
    if (!h) return;
    if (SuspendThread(h) == (DWORD)-1) return;
    CONTEXT cx; memset(&cx, 0, sizeof(cx)); cx.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(h, &cx)) {
        uintptr_t mod = (uintptr_t)GetModuleHandleW(NULL);
        void* rip = (void*)cx.Rip;
        fprintf(stderr, "[%s] HOST rip rva 0x%llX", tag,
                (unsigned long long)((uintptr_t)rip - mod));
        if (const yz_func_entry* fe = yz_func_from_host(rip))
            fprintf(stderr, " (func_%08X +0x%llX)", fe->addr,
                    (unsigned long long)((uintptr_t)rip - (uintptr_t)fe->fn));
        fprintf(stderr, "\n[%s] HOST stack chain:", tag);
        const uint64_t* sp = (const uint64_t*)cx.Rsp;
        int shown = 0;
        for (int i = 0; i < 800 && shown < 14; i++) {
            if (IsBadReadPtr((void*)(sp + i), 8)) break;
            uint64_t v = sp[i];
            if (v < mod || v > mod + 0x40000000ull) continue;
            const yz_func_entry* fe = yz_func_from_host((void*)v);
            if (!fe) continue;
            fprintf(stderr, "\n    [+0x%03X] func_%08X +0x%llX", i * 8, fe->addr,
                    (unsigned long long)(v - (uintptr_t)fe->fn));
            shown++;
        }
        fprintf(stderr, "\n");
        fflush(stderr);
    }
    ResumeThread(h);
}

extern "C" void yz_for_each_thread(void (*cb)(uint32_t, const char*, void*));

/* Name the well-known blocking lv2 syscalls so the per-thread dump reads as
 * "BLOCKED in sys_event_queue_receive" instead of a bare number. */
static const char* yz_sc_name(uint32_t num)
{
    switch (num) {
        case 85:  return "sys_event_flag_wait";
        case 92:  return "sys_semaphore_wait";
        case 97:  return "sys_lwmutex_lock";
        case 102: return "sys_mutex_lock";
        case 107: return "sys_cond_wait";
        case 113: return "sys_lwcond_wait";
        case 130: return "sys_event_queue_receive";
        case 141: return "sys_timer_usleep";
        case 44:  return "sys_ppu_thread_join";
        default:  return "";
    }
}

static void yz_dump_one_thread_cb(uint32_t tid, const char* name, void* handle)
{
    char tag[48];
    _snprintf(tag, sizeof(tag), "thr t%u \"%s\"", tid, name ? name : "?");
    tag[sizeof(tag) - 1] = 0;

    /* (1) AUTHORITATIVE: which syscall is this thread parked in right now? The
     * object-id args survive in the GPRs while the dispatch blocks, so this
     * names exactly what it waits on -- no host-stack guessing. */
    uint32_t scn = 0, held = 0; uint64_t a3 = 0, a4 = 0, a5 = 0;
    if (yz_wait_get(tid, &scn, &a3, &a4, &a5, &held)) {
        const char* nm = yz_sc_name(scn);
        fprintf(stderr, "[%s] BLOCKED in sc %u%s%s%s (r3=0x%llX r4=0x%llX r5=0x%llX) "
                "for %ums\n", tag, scn, *nm ? " " : "", nm, "",
                (unsigned long long)a3, (unsigned long long)a4,
                (unsigned long long)a5, (unsigned)held);
    } else {
        fprintf(stderr, "[%s] in guest code (not currently in a syscall)\n", tag);
    }

    /* (2) AUTHORITATIVE: the thread's live guest context (PC/GPRs/back-chain).
     * Suspend FIRST, then fetch the context: a thread that exits mid-dump clears
     * its ctx on the way out, so reading it only while frozen avoids racing a
     * dangling host-stack-local ppu_context (the install_check_thread crash). */
    if (HANDLE h = (HANDLE)handle) {
        if (SuspendThread(h) != (DWORD)-1) {
            if (ppu_context* gc = (ppu_context*)yz_thread_context(tid))
                yz_dump_guest_state(gc, tag);
            ResumeThread(h);
        }
    }

    /* (3) Secondary, lossy corroboration: the host-stack reverse-map. */
    yz_dump_host_stack_of((HANDLE)handle, tag);
}
/* Dump every guest thread's host stack -- finds the blocked producer. */
static void yz_dump_all_threads(const char* tag)
{
    fprintf(stderr, "[%s] === all guest threads ===\n", tag);
    if (g_yz_main_hthread) {
        /* t1 wait status (its full guest state is dumped by the caller). */
        uint32_t scn = 0, held = 0; uint64_t a3 = 0, a4 = 0, a5 = 0;
        if (yz_wait_get(1, &scn, &a3, &a4, &a5, &held)) {
            const char* nm = yz_sc_name(scn);
            fprintf(stderr, "[thr t1 \"main\"] BLOCKED in sc %u %s (r3=0x%llX r4=0x%llX "
                    "r5=0x%llX) for %ums\n", scn, nm, (unsigned long long)a3,
                    (unsigned long long)a4, (unsigned long long)a5, (unsigned)held);
        } else {
            fprintf(stderr, "[thr t1 \"main\"] in guest code (spin/poll, not in a syscall)\n");
        }
        yz_dump_host_stack_of(g_yz_main_hthread, "thr t1 \"main\"");
    }
    yz_for_each_thread(yz_dump_one_thread_cb);
}

static void yz_dump_main_host_stack(const char* tag)
{
    if (!g_yz_main_hthread) return;
    if (SuspendThread(g_yz_main_hthread) == (DWORD)-1) return;
    CONTEXT cx; memset(&cx, 0, sizeof(cx)); cx.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(g_yz_main_hthread, &cx)) {
        uintptr_t mod = (uintptr_t)GetModuleHandleW(NULL);
        void* rip = (void*)cx.Rip;
        fprintf(stderr, "[%s] main HOST rip rva 0x%llX", tag,
                (unsigned long long)((uintptr_t)rip - mod));
        if (const yz_func_entry* fe = yz_func_from_host(rip))
            fprintf(stderr, " (func_%08X +0x%llX)", fe->addr,
                    (unsigned long long)((uintptr_t)rip - (uintptr_t)fe->fn));
        fprintf(stderr, "\n[%s] main HOST stack chain:", tag);
        const uint64_t* sp = (const uint64_t*)cx.Rsp;
        int shown = 0;
        for (int i = 0; i < 600 && shown < 20; i++) {
            if (IsBadReadPtr((void*)(sp + i), 8)) break;
            uint64_t v = sp[i];
            if (v < mod || v > mod + 0x40000000ull) continue;
            const yz_func_entry* fe = yz_func_from_host((void*)v);
            if (!fe) continue;
            fprintf(stderr, "\n    [+0x%03X] func_%08X +0x%llX", i * 8, fe->addr,
                    (unsigned long long)(v - (uintptr_t)fe->fn));
            shown++;
        }
        fprintf(stderr, "\n");
        fflush(stderr);
    }
    ResumeThread(g_yz_main_hthread);
}

/* TEMP DEBUG: multi-sample host-RIP profiler. Suspends t1 repeatedly and reads
 * its live host RIP (reliable, unlike the guest back-chain which mis-symbolizes
 * string data). A tight guest-code spin clusters at a few host RIPs -> the
 * dominant (func_, offset) names exactly the loop t1 is parked in. */
static void yz_sample_t1_spin(const char* tag)
{
    if (!g_yz_main_hthread) return;
    uintptr_t mod = (uintptr_t)GetModuleHandleW(NULL);
    struct { uint32_t addr; uint64_t off; int n; } hist[48]; int hn = 0, taken = 0;
    for (int s = 0; s < 24; s++) {
        if (SuspendThread(g_yz_main_hthread) == (DWORD)-1) { Sleep(60); continue; }
        CONTEXT cx; memset(&cx, 0, sizeof(cx)); cx.ContextFlags = CONTEXT_CONTROL;
        uint32_t fa = 0xFFFFFFFFu; uint64_t fo = 0;
        if (GetThreadContext(g_yz_main_hthread, &cx)) {
            if (const yz_func_entry* fe = yz_func_from_host((void*)cx.Rip)) {
                fa = fe->addr; fo = (uintptr_t)cx.Rip - (uintptr_t)fe->fn;
            } else { fo = (uintptr_t)cx.Rip - mod; }
        }
        ResumeThread(g_yz_main_hthread);
        taken++;
        int idx = -1;
        for (int i = 0; i < hn; i++) if (hist[i].addr == fa && hist[i].off == fo) { idx = i; break; }
        if (idx < 0 && hn < 48) { idx = hn++; hist[idx].addr = fa; hist[idx].off = fo; hist[idx].n = 0; }
        if (idx >= 0) hist[idx].n++;
        Sleep(60);
    }
    fprintf(stderr, "[%s] t1 host-RIP profile (%d samples, host-RIP is RELIABLE):\n", tag, taken);
    for (int i = 0; i < hn; i++) {
        if (hist[i].addr == 0xFFFFFFFFu)
            fprintf(stderr, "    [%2d] rva-only +0x%llX\n", hist[i].n, (unsigned long long)hist[i].off);
        else
            fprintf(stderr, "    [%2d] func_%08X +0x%llX\n", hist[i].n, hist[i].addr,
                    (unsigned long long)hist[i].off);
    }
    fflush(stderr);
}

/* TEMP DEBUG (window bring-up): after the boot has had time to reach its frame
 * loop, dump the main thread's guest call stack so we can see what it spin-waits
 * on. */
static DWORD WINAPI yz_stall_watchdog(LPVOID)
{
    /* Sample the main thread's call stack a few times after it has reached the
     * post-load stall (shaders open ~by 30s). Identical stacks across samples
     * => t1 is parked; that names the guest function it spin-waits in. */
    Sleep(30000);
    if (g_yz_main_ctx) { yz_dump_guest_state(g_yz_main_ctx, "watchdog-30s");
                         yz_dump_main_host_stack("watchdog-30s"); }
    yz_sample_t1_spin("watchdog-30s");
    Sleep(15000);
    if (g_yz_main_ctx) { yz_dump_guest_state(g_yz_main_ctx, "watchdog-45s");
                         yz_dump_main_host_stack("watchdog-45s"); }
    Sleep(15000);
    if (g_yz_main_ctx) { yz_dump_guest_state(g_yz_main_ctx, "watchdog-60s");
                         yz_dump_main_host_stack("watchdog-60s"); }
    return 0;
}

/* RESYNC PROBE (env YZ_RESYNC_PROBE, 2026-06-20): pin WHERE t1 goes after libgcm's
 * reserve is force-unblocked. When t1 is wedged in the reserve (GET parked behind PUT),
 * set GET=PUT to clear the ring so the reserve returns, then profile t1's host-RIP to see
 * whether it reaches the SPURS job-feed (cellSpurs* / the gs_task queue 0x40197180) or
 * stalls elsewhere. Decides the gs_task solution: break-the-reserve vs a deeper gate. */
static DWORD WINAPI yz_resync_probe(LPVOID)
{
    Sleep(28000);
    for (int it = 0; it < 6; it++) {
        uint32_t put = vm_read32(0x10000040u) & ~3u;   /* RSX PUT */
        uint32_t get = vm_read32(0x10000044u) & ~3u;   /* RSX GET */
        if (get != put) {
            vm_write32(0x10000044u, put);              /* GET = PUT -> drain ring, unblock reserve */
            fprintf(stderr, "[resync-probe] it=%d GET 0x%06X -> PUT 0x%06X (unblock reserve)\n",
                    it, get, put);
            fflush(stderr);
        }
        Sleep(200);                                    /* let t1 run after the unblock */
        char tag[40]; sprintf(tag, "post-resync-%d", it);
        yz_sample_t1_spin(tag);                        /* where did t1 go? */
    }
    return 0;
}

/* RESYNC LOOP (env YZ_RESYNC_LOOP, 2026-06-20): the validated LAYER-1 unblock. t1
 * (PPU render thread) wedges in libgcm's reserve before building the next frame's
 * display list; the resync-probe proved that forcing GET=PUT (drain the ring) frees
 * t1 -> it builds the list + reaches the flip submit -> fence advances (2->3 measured).
 * This makes it CONTINUOUS: whenever GET is stuck (unchanged ~120ms) with the ring not
 * drained (GET != PUT), advance GET=PUT to release t1's reserve, so frames keep flowing. */
static DWORD WINAPI yz_resync_loop(LPVOID)
{
    const int aggr = getenv("YZ_GETFOLLOW") != nullptr;   /* pin GET=PUT every tick */
    Sleep(8000);   /* let the boot reach steady state (frames 1-2 render) first */
    uint32_t last_get = ~0u; DWORD stuck_since = 0; int n = 0;
    for (;;) {
        Sleep(aggr ? 5 : 30);
        uint32_t fence = vm_read32(0x40C00000u);
        if (fence < 2u) { last_get = ~0u; continue; }   /* act only at/after the frame-3 wedge */
        uint32_t put = vm_read32(0x10000040u) & ~3u;
        uint32_t get = vm_read32(0x10000044u) & ~3u;
        if (aggr) {                                /* keep GET pinned to PUT continuously */
            if (get != put) { vm_write32(0x10000044u, put);
                if (++n <= 60 || (n % 200) == 0)
                    fprintf(stderr, "[getfollow] #%d fence=%u GET 0x%06X -> PUT 0x%06X\n", n, fence, get, put); }
            continue;
        }
        DWORD now = GetTickCount();
        if (get != last_get) { last_get = get; stuck_since = now; continue; }
        if (get != put && (now - stuck_since) > 120u) {
            vm_write32(0x10000044u, put);          /* GET = PUT: release t1's reserve */
            last_get = put; stuck_since = now;
            if (++n <= 400 && (n <= 20 || (n % 25) == 0))
                fprintf(stderr, "[resync-loop] #%d fence=%u GET 0x%06X -> PUT 0x%06X\n", n, fence, get, put);
        }
        /* FRAME-4 wedge dump (GET=PUT but reserve still waiting): read t1's libgcm
         * reserve frame to get the danger region [SP+0x70,SP+0x74] it waits for, so we
         * learn the exact GET value that clears it (resolves the io-vs-EA confusion). */
        else if (get == put && fence >= 3u && (now - stuck_since) > 400u) {
            extern ppu_context* g_yz_main_ctx;
            static int dumped = 0;
            if (!dumped && g_yz_main_ctx) { dumped = 1;
                uint32_t sp = (uint32_t)g_yz_main_ctx->gpr[1];
                uint32_t ctr = (uint32_t)g_yz_main_ctx->ctr;
                fprintf(stderr, "[frame4] t1 SP=0x%08X ctr=0x%08X GET=0x%06X PUT=0x%06X fence=%u\n",
                        sp, ctr, get, put, fence);
                for (uint32_t off = 0x60; off <= 0x80; off += 4)
                    fprintf(stderr, "    [SP+0x%02X]=0x%08X\n", off, vm_read32(sp + off));
                fflush(stderr);
            }
        }
    }
}

/* Lightweight high-frequency RSX state tracer (TEMP DEBUG, env YZ_TRACE_RSX).
 * Samples the FIFO GET/PUT, the command word at GET, the flip fence, and t1's
 * ctr every ~120ms WITHOUT suspending any thread, and logs ONLY on change -- so
 * the stderr becomes a compact timeline of every distinct (ctr,GET,PUT,cmd,
 * fence) state and the exact millisecond the render loop latches into the
 * deadlock. Unlike the 30s watchdog (which only ever sees the frozen end-state),
 * this catches the TRANSITION: fence climbing 0->1->2 then freezing the moment
 * GET freezes at io 0x300000. When the state goes quiet (~2.4s no change) it
 * fires ONE full guest+host dump AT THE STALL EDGE, so the heavy snapshot lands
 * on the park itself instead of an arbitrary 30s later. */
/* Pointers to the MAIN (t1) thread's trampoline ring, captured on the main
 * thread so the (separate) trace-stall thread can print t1's recent indirect-
 * call path -- reliable, unlike the nearest-symbol back-chain that misattributes
 * return addresses to data/strings (2026-06-14h: func_01114E4C was a string). */
static void**    g_yz_main_tramp     = nullptr;
static uint64_t* g_yz_main_tramp_r31 = nullptr;
static unsigned* g_yz_main_tramp_idx = nullptr;

static DWORD WINAPI yz_rsx_state_trace(LPVOID)
{
    uint32_t lctr=~0u, lget=~1u, lput=~0u, lcmd=~0u, lfence=~0u, ldepth=~0u, lcend=~0u, lsp=~0u;
    int quiet = 0, edge_dumped = 0, lines = 0;
    const DWORD t0 = GetTickCount();
    for (;;) {
        Sleep(30);
        if (!vm_base) continue;
        const uint32_t put   = vm_read32(0x10000040u);   /* dma_control +0x40 PUT */
        const uint32_t get   = vm_read32(0x10000044u);   /* dma_control +0x44 GET */
        const uint32_t fence = vm_read32(0x40C00000u);   /* flip/vsync counter    */
        const uint32_t ctr   = g_yz_main_ctx ? (uint32_t)g_yz_main_ctx->ctr : 0u;
        uint32_t cmd = 0u;                               /* command word at GET    */
        const uint32_t ea = 0x40400000u + get;           /* FIFO io base (bufdesc+0x14) */
        if (vm_base && !IsBadReadPtr(vm_base + ea, 4)) cmd = vm_read32(ea);

        /* PRODUCER-SIDE gcm release/defer state (Phase 1 linchpin, 2026-06-15g):
         * S=*(game_toc-0x7410); S[0x20]=pending stopper, S[0x24]=frag-end saved when it
         * was placed; ctx->end=*(*(game_toc-0x7414))+4 = current frag-end. A release
         * DEFERS iff S[0x24]!=ctx->end at release time; op-list depth=(S[0]-S[8])/0x20
         * grows by one per defer. Goal: watch the FIRST cross-segment defer form. */
        uint32_t s20=0,s24=0,cend=0,ccur=0,depth=0;
        if (g_yz_game_toc) {
            uint32_t S = vm_read32(g_yz_game_toc - 0x7410u);
            if (S >= 0x10000u && S < 0xE0000000u) {
                s20 = vm_read32(S+0x20u); s24 = vm_read32(S+0x24u);
                uint32_t base = vm_read32(S+0x08u), head = vm_read32(S+0x00u);
                if (head >= base && head-base <= 0x40000u) depth = (head-base)/0x20u;
            }
            uint32_t p14 = vm_read32(g_yz_game_toc - 0x7414u);
            if (p14 >= 0x10000u && p14 < 0xE0000000u) {
                uint32_t gc = vm_read32(p14);
                if (gc >= 0x10000u && gc < 0xE0000000u) { cend = vm_read32(gc+4u); ccur = vm_read32(gc+8u); }
            }
        }

        if (ctr!=lctr || get!=lget || put!=lput || cmd!=lcmd || fence!=lfence ||
            depth!=ldepth || cend!=lcend || s20!=lsp) {
            if (lines < 8000) {
                fprintf(stderr, "[trace +%6lums] GET=0x%08X PUT=0x%08X fence=%u | "
                        "ctx.end=0x%08X cur=0x%08X S[24]=0x%08X S[20]=0x%08X oplist=%u%s\n",
                        (unsigned long)(GetTickCount()-t0), get, put, fence,
                        cend, ccur, s24, s20, depth,
                        (cend && s24 && cend != s24) ? "  <CROSS-SEG (next release DEFERS)" : "");
                fflush(stderr);
                lines++;
            }
            lctr=ctr; lget=get; lput=put; lcmd=cmd; lfence=fence;
            ldepth=depth; lcend=cend; lsp=s20; quiet=0;
        } else if (++quiet == 80 && !edge_dumped && g_yz_main_ctx) {   /* ~2.4s still */
            edge_dumped = 1;
            fprintf(stderr, "[trace] state QUIET ~2.4s -> full dump AT THE STALL EDGE:\n");
            yz_dump_guest_state(g_yz_main_ctx, "trace-stall");
            yz_dump_main_host_stack("trace-stall");
            yz_dump_all_threads("trace-stall");
            /* t1's RELIABLE recent control flow: the indirect-call targets (guest
             * addrs recorded at call time) + t1's trampoline-hop chain (newest
             * first). Unlike the back-chain, these are real call targets, so they
             * name the game function path INTO the stall (what t1 did before it
             * wedged) without nearest-symbol misattribution. */
            { extern uint32_t g_yz_last_targets[16]; extern unsigned g_yz_last_idx;
              fprintf(stderr, "[trace-stall] t1 recent indirect targets (newest first):");
              for (unsigned i = 0; i < 16 && i < g_yz_last_idx; i++) {
                  uint32_t t = g_yz_last_targets[(g_yz_last_idx - 1 - i) & 15];
                  fprintf(stderr, " 0x%08X", t);
                  if (const yz_func_entry* fe = yz_func_from_guest(t))
                      fprintf(stderr, "(func_%08X)", fe->addr);
              }
              fprintf(stderr, "\n"); }
            if (g_yz_main_tramp && g_yz_main_tramp_idx) {
                fprintf(stderr, "[trace-stall] t1 trampoline-hop chain (newest first):\n");
                unsigned idx = *g_yz_main_tramp_idx;
                for (unsigned i = 0; i < 28 && i < idx; i++) {
                    unsigned slot = (idx - 1 - i) & 255;
                    const yz_func_entry* fe = yz_func_from_host(g_yz_main_tramp[slot]);
                    fprintf(stderr, "    func_%08X r31=%08llX\n",
                            fe ? fe->addr : 0xFFFFFFFFu,
                            (unsigned long long)g_yz_main_tramp_r31[slot]);
                }
            }
            fflush(stderr);
        }
    }
    return 0;
}

/* Trampoline-hop ring (defined in dispatch.cpp); dumped by the crash handler
 * to reconstruct the cross-chunk control-flow path into a fault. (#19d) */
extern "C" __declspec(thread) void*    g_yz_tramp_ring[256];
extern "C" __declspec(thread) uint64_t g_yz_tramp_r31[256];
extern "C" __declspec(thread) uint64_t g_yz_tramp_r1[256];
extern "C" __declspec(thread) unsigned g_yz_tramp_idx;

static LONG WINAPI yz_crash_handler(EXCEPTION_POINTERS* ep)
{
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    fprintf(stderr, "\n[crash] exception 0x%08lX at host %p",
            er->ExceptionCode, er->ExceptionAddress);
    /* Module-relative RVA: stable across ASLR, resolvable against the
     * linker map (yakuza_recomp.map) without any debug info. */
    fprintf(stderr, " (rva 0x%llX)",
            (unsigned long long)((uintptr_t)er->ExceptionAddress -
                                 (uintptr_t)GetModuleHandleW(NULL)));
    if (const yz_func_entry* fe = yz_func_from_host(er->ExceptionAddress))
        fprintf(stderr, " (in func_%08X +0x%llX)", fe->addr,
                (unsigned long long)((uintptr_t)er->ExceptionAddress -
                                     (uintptr_t)fe->fn));
    /* PDB symbol for the faulting host code — authoritative for runtime/HLE
     * code, and disambiguates when the table hit above is a tiny stub the
     * linker happened to place just before the real function. */
    {
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* sym = (SYMBOL_INFO*)buf;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (SymFromAddr(GetCurrentProcess(),
                        (DWORD64)(uintptr_t)er->ExceptionAddress, &disp, sym))
            fprintf(stderr, " [sym %s+0x%llX]", sym->Name,
                    (unsigned long long)disp);
    }
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        uint64_t va = (uint64_t)er->ExceptionInformation[1];
        fprintf(stderr, ", %s host addr 0x%llX",
                er->ExceptionInformation[0] ? "writing" : "reading",
                (unsigned long long)va);
        if (vm_base && va >= (uint64_t)vm_base && va < (uint64_t)vm_base + 0x100000000ull)
            fprintf(stderr, " (guest 0x%08llX)",
                    (unsigned long long)(va - (uint64_t)vm_base));
    }
    fprintf(stderr, "\n[crash] recent indirect targets:");
    extern uint32_t g_yz_last_targets[16];
    extern unsigned g_yz_last_idx;
    for (unsigned i = 0; i < 16 && i < g_yz_last_idx; i++)
        fprintf(stderr, " 0x%08X",
                g_yz_last_targets[(g_yz_last_idx - 1 - i) & 15]);

    /* Recent trampoline chunk hops (newest first): the cross-chunk control-flow
     * path into the fault. Reverse-mapped host fn ptr -> guest func. (#19d) */
    fprintf(stderr, "\n[crash] recent trampoline chunks (newest first; r31/r1 at hop entry):");
    for (unsigned i = 0; i < 256 && i < g_yz_tramp_idx; i++) {
        unsigned slot = (g_yz_tramp_idx - 1 - i) & 255;
        const yz_func_entry* fe = yz_func_from_host(g_yz_tramp_ring[slot]);
        fprintf(stderr, "\n    func_%08X  r31=%08llX r1=%08llX",
                fe ? fe->addr : 0xFFFFFFFFu,
                (unsigned long long)g_yz_tramp_r31[slot],
                (unsigned long long)g_yz_tramp_r1[slot]);
    }


    /* Guest state + PPC64 back-chain walk: names the real guest call chain
     * (the host return-address scan below misses direct bl callers). */
    yz_dump_guest_state(g_yz_cur_ctx, "crash");
    /* Return-address candidates from the faulting thread's stack: values
     * pointing into our image are callers (the faulting accessor is a leaf,
     * so the direct caller is at/near [rsp]). Resolved via the func table. */
    if (ep->ContextRecord) {
        uintptr_t mod = (uintptr_t)GetModuleHandleW(NULL);
        const uint64_t* sp = (const uint64_t*)ep->ContextRecord->Rsp;
        fprintf(stderr, "\n[crash] stack return candidates (host call chain):");
        int shown = 0;
        for (int i = 0; i < 768 && shown < 40; i++) {
            uint64_t v = 0;
            if (IsBadReadPtr(sp + i, 8)) break;
            v = sp[i];
            if (v < mod || v > mod + 0x40000000ull) continue;
            const yz_func_entry* fe = yz_func_from_host((const void*)v);
            /* only show entries that resolve to a real lifted/runtime func */
            if (!fe) continue;
            fprintf(stderr, "\n    [+0x%03X] func_%08X +0x%llX", i * 8, fe->addr,
                    (unsigned long long)(v - (uintptr_t)fe->fn));
            shown++;
        }
    }
    /* Guest watch regions: dumped on crash for post-mortem inspection. */
    static const struct { uint32_t addr; uint32_t len; const char* tag; }
    watches[] = {
        { 0x016C34B0, 0x50, "malloc arena info" },
        { 0x016C34D8, 0x40, "malloc heap struct" },
    };
    for (size_t w = 0; w < sizeof(watches) / sizeof(watches[0]); w++) {
        fprintf(stderr, "\n[crash] watch %s @0x%08X:", watches[w].tag,
                watches[w].addr);
        for (uint32_t i = 0; i < watches[w].len; i += 4) {
            if (i % 16 == 0)
                fprintf(stderr, "\n    +0x%02X:", i);
            uint32_t v;
            memcpy(&v, vm_base + watches[w].addr + i, 4);
            v = _byteswap_ulong(v);  /* guest is big-endian */
            fprintf(stderr, " %08X", v);
        }
    }
    fprintf(stderr, "\n");
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char** argv)
{
    const char* elf_path = (argc > 1) ? argv[1] : "game/EBOOT.elf";

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);  /* load yakuza_recomp.pdb */
    SetUnhandledExceptionFilter(yz_crash_handler);

    printf("=== Yakuza: Dead Souls recomp runner ===\n");

    if (vm_init() != 0) {
        fprintf(stderr, "ERROR: vm_init failed\n");
        return 1;
    }
    vm_stack_alloc_init(&g_stacks);

    uint64_t e_entry = 0;
    if (load_elf(elf_path, &e_entry) != 0)
        return 1;

    /* LLE firmware: Sony's libsre at its lift_prx relocation base. Must be
     * in place before yz_install_imports patches its import slots. */
    if (load_prx_image("recomp_prx/libsre_image.bin", YZ_LIBSRE_BASE) != 0)
        return 1;

    /* LLE firmware: Sony's libgcm_sys (RSX driver). Same contract as libsre --
     * the flat image includes its TOC (0x02114000) referenced by the export
     * OPDs the game's cellGcmSys imports bind to. Must be loaded before
     * yz_install_imports. The driver issues sys_rsx syscalls (668-677). */
    if (load_prx_image("recomp_prx/libgcm_sys_image.bin", YZ_LIBGCM_BASE) != 0)
        return 1;
    (void)&yz_watch_arm;   /* write-watch available for debugging; not armed */

    /* Lifted SPU images: register Sony's SPURS kernel (recomp_prx/
     * spurs_kernel2.c) and the system-service workload (spurs_sysservice.c,
     * runs at LS 0xA00) so sys_spu_thread_group_start can run them for real. */
    /* Distinct image ids so the overlapping LS-0xA00 images (system service vs
     * taskset policy) are disambiguated by ctx->image_id (set on DMA dispatch,
     * spu_dma.h). Kernel (LS 0x290..) and gs_task (LS 0x3000) don't overlap, so
     * image 0 (matches any) is fine for them. */
    spu_begin_image(0); spu_recomp_register();            /* kernel  */
    spu_begin_image(1); spu_recomp_register_sysservice(); /* system service @0xA00 */
    spu_begin_image(2); spu_recomp_register_policy();     /* taskset policy @0xA00 */
    spu_begin_image(0); spu_recomp_register_gstask();     /* Edge geometry task @0x3000 */
    printf("[boot] SPU images registered (kernel + service + policy + gs_task)\n");

    /* DIAG (1f): function-level spin profiler. YZ_SPU_PROF histograms every
     * SPU tail-call trampoline hop by target LS addr -> pins which lifted
     * SPURS functions the SPU threads spin in (the scheduler loops via
     * trampolines, invisible to spu_indirect_branch). Set before threads run. */
    g_spu_prof_on = getenv("YZ_SPU_PROF") ? 1 : 0;
    g_yz_watch_dlist = getenv("YZ_WATCH_DLIST") ? 1 : 0;

    /* PS3 e_entry points at an OPD descriptor: word0 = code, word1 = TOC */
    uint32_t entry_code = vm_read32(e_entry);
    uint32_t entry_toc  = vm_read32(e_entry + 4);
    printf("[boot] entry OPD: code=0x%08X toc=0x%08X\n", entry_code, entry_toc);

    /* The game module has a single fixed TOC. Recorded so the dispatcher can
     * restore it when a guest function is reached with r2==0 -- happens when a
     * gcm callback (e.g. the flip/vblank handler) is invoked by its bare code
     * address instead of its OPD, so the OPD's TOC load is skipped. */
    g_yz_game_toc = entry_toc;

    yz_ppu_fn entry_fn = yz_lookup_func(entry_code);
    if (!entry_fn) {
        fprintf(stderr, "ERROR: entry 0x%08X not in function table (%u funcs)\n",
                entry_code, g_yz_func_count);
        return 1;
    }

    yz_init_syscalls();
    yz_install_imports();
    printf("[boot] installed %u import bridges\n", g_yz_import_count);
    g_ps3_guest_caller = guest_caller;

    /* Main PPU thread stack */
    uint32_t stack_base = vm_stack_allocate(&g_stacks, 1024 * 1024);
    if (!stack_base) {
        fprintf(stderr, "ERROR: stack allocation failed\n");
        return 1;
    }

    yz_threads_init(stack_base, 1024 * 1024);

    static ppu_context ctx;   /* zero-initialized */
    ctx.gpr[1] = ((uint64_t)stack_base + 1024 * 1024 - 0x100) & ~0xFull;
    ctx.gpr[2] = entry_toc;
    vm_write64(ctx.gpr[1], 0);   /* null back-chain */

    /* Minimal process arguments in guest scratch memory. lv2 passes argv /
     * envp as arrays of 64-bit pointers; the CRT compacts them to 32-bit
     * in place with a do-while loop, so they must be real memory even for
     * argc=1 with no extras. */
    const uint32_t args_base = 0x0FDF0000u;
    const char*    pname     = "/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN";
    {
        uint32_t str = args_base + 0x100;
        for (size_t i = 0; i <= strlen(pname); i++)
            vm_write8(str + (uint32_t)i, (uint8_t)pname[i]);
        vm_write64(args_base + 0x00, str);  /* argv[0] (64-bit slot) */
        vm_write64(args_base + 0x08, 0);    /* argv terminator */
        vm_write64(args_base + 0x40, 0);    /* envp terminator */
    }

    /* PS3 process entry registers:
     *   r3 = argc, r4 = argv (u64 slots), r5 = envp (u64 slots), r6 = 0,
     *   r7 = main thread id, r8/r9/r10 = TLS template (vaddr/filesz/memsz)
     *   -- the CRT forwards r7..r10 to sys_initialize_tls. */
    ctx.gpr[3]  = 1;
    ctx.gpr[4]  = args_base;
    ctx.gpr[5]  = args_base + 0x40;
    ctx.gpr[6]  = 0;
    ctx.gpr[7]  = 1;
    ctx.gpr[8]  = g_tls_vaddr;
    ctx.gpr[9]  = g_tls_filesz;
    ctx.gpr[10] = g_tls_memsz;
    /* lv2 also passes (verified vs RPCS3 ppu_load_exec + the CRT at
     * 0xDDDB74, which stores r12 into the libc malloc config — leaving it
     * 0 makes every malloc return NULL): */
    ctx.gpr[11] = e_entry;              /* entry descriptor address */
    ctx.gpr[12] = g_malloc_pagesize;    /* sys_process_param malloc_pagesize */
    printf("[boot] TLS template: vaddr=0x%08llX filesz=0x%llX memsz=0x%llX\n",
           (unsigned long long)g_tls_vaddr, (unsigned long long)g_tls_filesz,
           (unsigned long long)g_tls_memsz);

    printf("[boot] calling func_%08X (sp=0x%08llX toc=0x%08llX)\n",
           entry_code, (unsigned long long)ctx.gpr[1],
           (unsigned long long)ctx.gpr[2]);

    /* Drive guest vblank handlers while the game runs (no real RSX vsync). */
    CreateThread(NULL, 0, yz_vblank_thread, NULL, 0, NULL);
    /* Stall watchdog: dumps the main thread's guest call stack if the boot
     * parks (TEMP DEBUG: diagnosing the post-shader-open stall). */
    CreateThread(NULL, 0, yz_stall_watchdog, NULL, 0, NULL);
    /* High-frequency RSX state tracer (TEMP DEBUG): compact GET/PUT/fence/ctr
     * timeline + a full dump at the stall edge. Env-gated so default runs stay
     * clean; on for the deadlock investigation. */
    if (getenv("YZ_TRACE_RSX"))
        CreateThread(NULL, 0, yz_rsx_state_trace, NULL, 0, NULL);
    if (getenv("YZ_RESYNC_PROBE"))
        CreateThread(NULL, 0, yz_resync_probe, NULL, 0, NULL);
    if (getenv("YZ_RESYNC_LOOP"))
        CreateThread(NULL, 0, yz_resync_loop, NULL, 0, NULL);

    g_yz_cur_ctx = &ctx;
    g_yz_main_ctx = &ctx;
    /* Capture this (main/t1) thread's trampoline-ring instance for the stall dump. */
    g_yz_main_tramp     = g_yz_tramp_ring;
    g_yz_main_tramp_r31 = g_yz_tramp_r31;
    g_yz_main_tramp_idx = &g_yz_tramp_idx;
    /* Real handle to this (main/guest) thread, so the watchdog can suspend it
     * and read its HOST rip/stack -- names the host (lifted) function it spins
     * in when the guest ctx alone is ambiguous (TEMP DEBUG). */
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                    GetCurrentProcess(), &g_yz_main_hthread,
                    0, FALSE, DUPLICATE_SAME_ACCESS);
    entry_fn(&ctx);
    yz_drain_trampolines(&ctx);

    printf("[boot] entry returned, r3=0x%llX\n", (unsigned long long)ctx.gpr[3]);
    vm_shutdown();
    return (int)(uint32_t)ctx.gpr[3];
}
