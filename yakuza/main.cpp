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
#include "../include/ps3emu/endian.h"   /* ps3_bswap32 -- YZ_WATCH_WR old/new dump */

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
extern "C" void yz_w2life_dump(const char*);   /* s31 W2LIFE probe (spu_channels.c) */

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
extern "C" void cellGame_init_from_paramsfo(const char* sfo_path);
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
/* recomp_prx/ts_exit.c (generated) — Sony's SPURS taskset EXIT-HANDLER blob
 * (libsre ea 0x02025500, 0x680 bytes). On a task's EXIT syscall the policy DMAs
 * it to LS 0x10000 and calls it (r3=taskset, r4=taskId, r5=exitCode, r6=args --
 * RPCS3 spursTasksetOnTaskExit, cellSpursSpu.cpp:1669). Until it was lifted,
 * every task exit died as "unknown branch LS 0x10000" (image 5's exit = the
 * entry-7 shader-stream gate). Own image id: the reverse image-switch adopts it
 * at the call and re-adopts the policy on its bi $r0 return. */
extern "C" void spu_recomp_register_tsexit(void);
/* recomp_prx/job_module.c (generated) — Sony's SPURS JOB-CHAIN policy module
 * (libsre ea 0x0202A180, 0x3E80 bytes, loaded to LS 0xA00 like the taskset
 * policy). The game's pxd streaming layer runs a jobchain (wid 1, object
 * 0x4019C880) whose jobs signal the IWL event flag t1 blocks on after the
 * asset sweep; without this lift the kernel dispatched the jobchain into the
 * SERVICE image's code (wrong image) -> wild EA-0 lock-line atomics. */
extern "C" void spu_recomp_register_jobmod(void);
/* recomp_prx/job_bin_{a,b}.c (generated) — the game's two jobchain JOB BINARIES
 * (runtime-loaded per-descriptor SPU code; both EBOOT-static, extracted by
 * eaBinary). A = the 14-way bulk worker (EBOOT 0x01254500, 0x9540 B, loads at
 * ~LS 0x4C00); B = the completion job that sets the IWL event flag 0x4019C680
 * bit 0x8000 t1 waits on (EBOOT 0x01275A00, 0x14C0 B, loads at ~LS 0xE400).
 * Dispatch: mfc_do_transfer records where each binary lands per context;
 * spu_indirect_branch switches image 13 <-> 14/15 on those spans. */
extern "C" void spu_recomp_register_jobbin_a(void);
extern "C" void spu_recomp_register_jobbin_b(void);
/* Alternate-slot lifts (2026-07-08): the jobchain loads its job binaries into
 * descriptor-assigned LS slots — MEASURED round-1 loads jobB (0x01275A00) at
 * LS 0x4C00 (scratch/idboot.err: jobbase B=0x04C00, resident head bytes
 * 43 49 4E = jobB's), while the original lifts are fixed-base (A@0x4C00,
 * B@0xE400). Each binary is therefore lifted at BOTH slot bases and both
 * registrations share the binary's image id (spans don't collide within an
 * image). Without these, spu_lookup's image-0 wildcard silently substituted
 * gs_task's code at the job site and the notify job never ran (the spup17/
 * event-flag wall, DONT_RECHASE #23). */
extern "C" void spu_recomp_register_jobbin_a_e400(void);
extern "C" void spu_recomp_register_jobbin_b_4c00(void);
/* recomp_prx/cri_audio.c (generated) — the CRI SOFDEC/ADX audio codec task
 * (cri_audio_ps3spurs.elf, EBOOT SPU img #7 @0x012B4980, LS base 0x3000, entry
 * 0x3070). It OVERLAPS gs_task in LS, so it registers under a DISTINCT image (3)
 * with the cri_audio_ prefix; spu_task_launch switches ctx->image_id by the
 * TaskInfo ELF EA when the SPURS kernel dispatches the codec taskset (wid 3). */
extern "C" void cri_audio_register_functions(void);
extern "C" void wkl4_register_functions(void);
#include "../recomp_prx/spu_image_table.h"   /* generated: remaining EBOOT images + registry */
extern "C" void spu_begin_image(int image_id);
/* runtime/spu/spu_channels.c — SPU spin-profiler gate (env YZ_SPU_PROF) */
extern "C" int g_spu_prof_on;
extern "C" int g_yz_watch_dlist;
extern "C" int g_yz_slotstore;   /* YZ_SLOTSTORE: wid4 record-store watch (spu_channels.c) */

/* YZ_HWWATCH (s26 ~04:25, the stager hunt's definitive tool): a HARDWARE
 * debug-register watchpoint (DR0, exact 4-byte write watch) on the wid4
 * work-record slot0 value word — immune to every write-path/aliasing gap that
 * defeated the software watches (STATUS ⚡ STAGER-HUNT STATE: all helper
 * widths, atomics, MFC forms, HLE bulk, raw-ptr greps eliminated while the
 * bytes provably change). Armed from the vblank tick once all threads exist;
 * threads created after arming are not covered (acceptable — the writer runs
 * from early rounds). */
#include <tlhelp32.h>
extern "C" uint32_t yz_guest_addr_from_host(const void* rip);  /* defined below */
extern "C" uint32_t yz_thread_current_id(void);                /* threads.cpp */
extern "C" uint8_t* vm_base;
static void* g_hww_addr = nullptr;
static LONG CALLBACK yz_hwwatch_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    if (!(c->Dr6 & 0x1)) return EXCEPTION_CONTINUE_SEARCH;   /* not our DR0 */
    static unsigned long hn = 0; hn++;
    if (hn <= 40 || (hn & 0xFFu) == 0) {
        uint32_t g = yz_guest_addr_from_host((void*)c->Rip);
        uint32_t v = 0; memcpy(&v, g_hww_addr, 4);
        fprintf(stderr, "[hww] n=%lu tid=%u rip=%p guest=0x%08X val_now=0x%08X\n",
                hn, yz_thread_current_id(), (void*)c->Rip, g, ps3_bswap32(v));
        fflush(stderr);
    }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}
extern "C" void yz_hwwatch_arm(void)
{
    g_hww_addr = vm_base + 0x424528A0u;
    AddVectoredExceptionHandler(1, yz_hwwatch_veh);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te; te.dwSize = sizeof(te);
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    int armed = 0;
    if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                              THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!h) continue;
        if (SuspendThread(h) != (DWORD)-1) {
            CONTEXT c; memset(&c, 0, sizeof(c));
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(h, &c)) {
                c.Dr0 = (DWORD64)(uintptr_t)g_hww_addr;
                /* L0=1 (bit0), RW0=01 write (bits16-17), LEN0=11 4-byte
                 * (bits18-19) -> 0xD0001 over the DR0 fields */
                c.Dr7 = (c.Dr7 & ~0xF0003ull) | 0xD0001ull;
                if (SetThreadContext(h, &c)) armed++;
            }
            ResumeThread(h);
        }
        CloseHandle(h);
    } while (Thread32Next(snap, &te));
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    fprintf(stderr, "[hww] ARMED DR0=%p (guest 0x424528A0) on %d threads\n",
            g_hww_addr, armed);
    fflush(stderr);
}

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

extern "C" uint32_t yz_thread_current_id(void);   /* threads.cpp: caller's guest tid */

static void guest_caller(uint32_t opd_addr,
                         uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    /* One lazily-allocated guest stack + context per host thread. */
    static thread_local ppu_context cb_ctx;
    static thread_local uint32_t cb_stack = 0;
    if (!cb_stack) {
        cb_stack = vm_stack_allocate(&g_stacks, 256 * 1024);
        if (!cb_stack) { fprintf(stderr, "[boot] callback stack alloc failed\n"); return; }
        cb_ctx.gpr[1] = ((uint64_t)cb_stack + 256 * 1024 - 0x100) & ~0xFull;
        /* Null back-chain terminator: an SDK stack-trace walker (e.g. libsre's
         * assertion handler) follows [r1] up until it reads 0. Without this the
         * walk runs off the top of the callback stack into garbage and loops
         * forever, blowing the stack. Mirrors the main/created-thread setup. */
        vm_write64(cb_ctx.gpr[1], 0);
    }
    /* Clean context on EVERY invocation: clear all GPR/CR/LR/CTR/XER/FPR + the lwarx
     * reservation so a previous callback's leftovers don't leak in (a stale non-
     * volatile reg read before write, or a stale reservation making the next stwcx.
     * falsely succeed). Preserve only the stack pointer (its back-chain persists in
     * guest memory) and set thread_id so lv2 mutex ownership isn't keyed on tid 0. */
    uint64_t cb_sp = cb_ctx.gpr[1];
    memset(&cb_ctx, 0, sizeof(cb_ctx));
    cb_ctx.gpr[1] = cb_sp;
    cb_ctx.thread_id = yz_thread_current_id();
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
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#include <dbghelp.h>

/* Host vblank driver. The game's frame loop polls guest state (driver_info
 * head vBlankCount + flip completion) that on real HW the RSX vblank interrupt
 * (~59.94x/s) updates. yz_rsx_vblank_tick (import_overrides.cpp) bumps those
 * counters and publishes any pending flip's done bit; it no-ops until sys_rsx
 * context_allocate has set up the driver_info, so starting early is safe. */
static DWORD WINAPI yz_vblank_thread(LPVOID)
{
    /* YZ_VBL_DIV=N (s21, H4 discriminator): divide the vblank rate by N so a
     * ~5-7 FPS boot sees ~2 vblanks/frame (what a 30fps title expects) instead
     * of 8-12. A/B lever for the flip-label stall: if the stall vanishes or
     * moves far later under the throttle, the wall is timing-coupled (H4);
     * unchanged = protocol-shape hypotheses stay in front. Slows every guest
     * clock derived from vblank -- diagnostic only, default 1 (off). */
    static int vdiv = 0;
    if (!vdiv) {
        const char* d = getenv("YZ_VBL_DIV");
        vdiv = d ? atoi(d) : 1;
        if (vdiv < 1) vdiv = 1; if (vdiv > 16) vdiv = 16;
        if (vdiv > 1)
            fprintf(stderr, "[vbl] YZ_VBL_DIV=%d ARMED: vblank ~%.1f Hz\n",
                    vdiv, 62.5 / vdiv);
    }
    /* PRECISE VBLANK (env YZ_VSYNC_PRECISE, 2026-06-25): RPCS3 drives vblank from a
     * drift-corrected absolute schedule (post_event_time = start + count*period/rate),
     * staying locked to ~59.94 Hz. Our old `Sleep(16)` jitters with Windows' ~15.6 ms
     * timer quantum (effective ~32-62 Hz, drifting under load) -> loose frame pacing.
     * This sleeps until the ABSOLUTE next-vblank tick and spins the final sub-ms for
     * precision. Default boot keeps the old behaviour. */
    static int precise = -1; if (precise < 0) precise = getenv("YZ_VSYNC_PRECISE") ? 1 : 0;
    if (precise) {
        LARGE_INTEGER freq, now; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&now);
        const double period = (double)freq.QuadPart * vdiv / 59.94;  /* ticks per vblank */
        double next = (double)now.QuadPart + period;
        for (;;) {
            QueryPerformanceCounter(&now);
            double remain_ms = ((next - (double)now.QuadPart) / (double)freq.QuadPart) * 1000.0;
            if (remain_ms > 2.0) Sleep((DWORD)(remain_ms - 1.5));
            do { QueryPerformanceCounter(&now); } while ((double)now.QuadPart < next);  /* spin last bit */
            next += period;
            yz_rsx_vblank_tick();
        }
    }
    for (;;) {
        Sleep(16 * vdiv);     /* ~62.5 Hz (default); /N under YZ_VBL_DIV */
        yz_rsx_vblank_tick();
    }
    return 0;
}

/* Host-ptr reverse index (built once). The func table is sorted by GUEST addr, but
 * lifted functions are laid out per chunk object, so mapping a host RIP back to its
 * function needs a HOST-ptr-sorted view + a binary search with TRUE containment.
 * Within a chunk functions are back-to-back, so the next entry's host ptr is this
 * function's end; only at a chunk boundary do we fall back to a span cap. This stops
 * the old linear scan from mis-attributing a non-lifted RIP (runtime/lib/system code)
 * to the nearest lifted function with a bogus multi-MB offset (its cap was 16 MB). */
static const yz_func_entry** g_yz_host_idx = nullptr;
static unsigned g_yz_host_idx_n = 0;

static int yz_host_idx_cmp(const void* a, const void* b)
{
    const yz_func_entry* ea = *(const yz_func_entry* const*)a;
    const yz_func_entry* eb = *(const yz_func_entry* const*)b;
    if ((uintptr_t)ea->fn < (uintptr_t)eb->fn) return -1;
    if ((uintptr_t)ea->fn > (uintptr_t)eb->fn) return 1;
    return 0;
}

static void yz_build_host_idx(void)
{
    g_yz_host_idx = (const yz_func_entry**)malloc(sizeof(void*) * (g_yz_func_count ? g_yz_func_count : 1));
    unsigned n = 0;
    for (unsigned i = 0; i < g_yz_func_count; i++)
        if (g_yz_func_table[i].fn) g_yz_host_idx[n++] = &g_yz_func_table[i];
    qsort(g_yz_host_idx, n, sizeof(void*), yz_host_idx_cmp);
    g_yz_host_idx_n = n;
}

/* Thread-safe one-time build: yz_func_from_host runs concurrently on the crash VEH,
 * the watchdog thread, and (when YZ_WATCH_* is armed) the guest-store path, so the
 * lazy build must not race (two mallocs + a torn read of a half-sorted array). */
static INIT_ONCE g_yz_host_idx_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK yz_host_idx_init_cb(PINIT_ONCE i, PVOID p, PVOID* c)
{ (void)i; (void)p; (void)c; yz_build_host_idx(); return TRUE; }

static const yz_func_entry* yz_func_from_host(const void* rip)
{
    InitOnceExecuteOnce(&g_yz_host_idx_once, yz_host_idx_init_cb, NULL, NULL);
    if (!g_yz_host_idx_n) return nullptr;
    unsigned lo = 0, hi = g_yz_host_idx_n;          /* greatest fn <= rip in [lo,hi) */
    while (lo < hi) {
        unsigned mid = (lo + hi) >> 1;
        if ((uintptr_t)g_yz_host_idx[mid]->fn <= (uintptr_t)rip) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return nullptr;                    /* below all lifted functions */
    const yz_func_entry* best = g_yz_host_idx[lo - 1];
    uintptr_t end = (uintptr_t)best->fn + 0x80000;  /* chunk-boundary span cap (512 KB) */
    if (lo < g_yz_host_idx_n) {                      /* within a chunk: next fn = this fn's end */
        uintptr_t nxt = (uintptr_t)g_yz_host_idx[lo]->fn;
        if (nxt < end) end = nxt;
    }
    if ((uintptr_t)rip >= end) return nullptr;
    return best;
}

/* pt35e: exported host-rip -> guest func addr, so the write-watch (shims.cpp) can
 * stack-walk and name the LIFTED caller (not just vm_write32). 0 = not a lifted fn. */
extern "C" uint32_t yz_guest_addr_from_host(const void* rip)
{
    const yz_func_entry* fe = yz_func_from_host(rip);
    return fe ? fe->addr : 0u;
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
void yz_dump_guest_state(const ppu_context* gc, const char* tag);
/* Trampoline-hop ring (defined in dispatch.cpp) -- the reliable caller chain the
 * watch handler uses (the host back-chain mis-symbolizes memcpy/data-lr paths). */
extern "C" __declspec(thread) void*    g_yz_tramp_ring[256];
extern "C" __declspec(thread) unsigned g_yz_tramp_idx;

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
                /* Trampoline-ring (RELIABLE; the host back-chain mis-symbolizes the
                 * memcpy/data-lr append path -- see the appender hunt, pt25b). Names the
                 * recent GAME funcs that led here = the real appender + its callers. */
                { fprintf(stderr, "[watch] tramp-ring (newest first):");
                  for (unsigned k = 0; k < 14 && k < g_yz_tramp_idx; k++) {
                      unsigned slot = (g_yz_tramp_idx - 1 - k) & 255;
                      const yz_func_entry* fe2 = yz_func_from_host(g_yz_tramp_ring[slot]);
                      if (fe2) fprintf(stderr, " func_%08X", fe2->addr); }
                  fprintf(stderr, "\n"); }
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

/* ---------------------------------------------------------------------------
 * YZ_WATCH_WR=hexEA[,hexEA...] -- env-driven multi-address write-watch, NO
 * REBUILD required per question (unlike the yz_watch_bd compile-time tg[]
 * array in shims.cpp, which costs an edit+relink every time a new EA is
 * interesting). REUSES the page-guard/VEH mechanism above (yz_watch_veh's
 * PAGE_READONLY-trap-then-single-step protocol) rather than inventing a
 * second one -- this is a parallel array-based VEH because the existing
 * yz_watch_arm() is a single-slot design (g_watch_guest) already claimed by
 * YZ_WATCH_EA/YZ_WATCH_FLAG; up to 16 independent slots here. Zero code runs
 * when YZ_WATCH_WR is unset (checked once at init, guard at the call site in
 * main()). A page-guard traps the WHOLE 4 KB page, so a watched page shared
 * with a hot SPURS-mgmt lock line (0x40197xxx class) single-steps every
 * atomic on it and can wreck the run -- warn loudly but still arm (the spec
 * calls for a warning, not a refusal).
 * -----------------------------------------------------------------------*/
#define YZ_WATCH_WR_MAX 16
static uint32_t g_wwr_ea[YZ_WATCH_WR_MAX];
static uint8_t* g_wwr_host[YZ_WATCH_WR_MAX];
/* page base (HOST address) per slot. s26 FIX: was uint32_t — vm_base is a
 * 64-bit mapping (~0x1Fxxxxxxxxx), so the stored page truncated, the VEH's
 * page match never hit, and the arming VirtualProtect targeted a bogus low-32
 * address (first ride6 crashed on the un-guarded lazy-commit fault instead).
 * Instrument-class bug: the flag shipped s14 and this was its first real use
 * on a >4GB vm_base (LESSONS #21 — check the instrument). */
static uintptr_t g_wwr_page[YZ_WATCH_WR_MAX];
static uint32_t g_wwr_old[YZ_WATCH_WR_MAX];       /* last-seen guest-BE dword, for old/new */
static int      g_wwr_n = 0;
static unsigned long g_wwr_otherhits = 0;         /* same-page-other-dword counter (all slots) */

static LONG CALLBACK yz_watch_wr_veh(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;

    if (code == EXCEPTION_ACCESS_VIOLATION) {
        if (ep->ExceptionRecord->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
        ULONG_PTR acc = ep->ExceptionRecord->ExceptionInformation[0];  /* 0=read 1=write */
        uintptr_t tgt = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t page = tgt & ~(uintptr_t)0xFFF;
        int onpage = 0;
        for (int i = 0; i < g_wwr_n; i++) if (g_wwr_page[i] == page) { onpage = 1; break; }
        if (!onpage) return EXCEPTION_CONTINUE_SEARCH;   /* not one of our pages */
        (void)acc;   /* PAGE_READONLY only traps writes; reads pass through untouched */
        /* We can't know from the fault alone which bytes changed (x86 doesn't
         * report access width here), so let the write commit, single-step
         * once, then diff every watched EA on this page against its last-seen
         * value in the SINGLE_STEP handler below. */
        DWORD old;
        VirtualProtect((void*)page, 0x1000, PAGE_READWRITE, &old);
        ep->ContextRecord->EFlags |= 0x100;   /* trap flag -> single-step after this insn */
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    /* EXCEPTION_SINGLE_STEP: the write just committed. Diff every watched EA
     * (cheap: <=16 slots) and log any whose dword changed; count writes that
     * landed on a watched PAGE but not a watched DWORD, reported per 4096. */
    int any_watched_changed = 0;
    for (int i = 0; i < g_wwr_n; i++) {
        uint32_t cur;
        memcpy(&cur, g_wwr_host[i], 4);
        if (cur != g_wwr_old[i]) {
            any_watched_changed = 1;
            uint32_t old_be = ps3_bswap32(g_wwr_old[i]);
            uint32_t new_be = ps3_bswap32(cur);
            g_wwr_old[i] = cur;
            uint32_t wtid = yz_thread_current_id();
            void* bt[24];
            unsigned short got = RtlCaptureStackBackTrace(1, 24, bt, 0);
            char chain[300]; int ci = 0; chain[0] = 0;
            for (unsigned k = 0; k < got && ci < 260; k++) {
                uint32_t g = yz_guest_addr_from_host(bt[k]);
                if (g) ci += snprintf(chain + ci, sizeof(chain) - (size_t)ci, "%s0x%08X",
                                       ci ? "<-" : "", g);
            }
            fprintf(stderr, "[watch-wr] tid=%u ea=0x%08X old=0x%08X new=0x%08X bt: %s\n",
                    wtid, g_wwr_ea[i], old_be, new_be, chain);
            /* Trampoline-ring fallback/supplement (reliable across memcpy/data-lr
             * hops the host back-chain mis-symbolizes -- same rationale as the
             * existing yz_watch_veh). */
            fprintf(stderr, "[watch-wr]   tramp-ring (newest first):");
            for (unsigned k = 0; k < 10 && k < g_yz_tramp_idx; k++) {
                unsigned slot2 = (g_yz_tramp_idx - 1 - k) & 255;
                uint32_t g2 = yz_guest_addr_from_host(g_yz_tramp_ring[slot2]);
                if (g2) fprintf(stderr, " 0x%08X", g2);
            }
            fprintf(stderr, "\n");
            fflush(stderr);
        }
    }
    if (!any_watched_changed) {
        /* A write landed on a watched page but touched none of our exact
         * watched dwords (e.g. an adjacent field in the same 4 KB page).
         * Count silently; report once per 4096 (spec: "count same-page-
         * other-dword writes silently, report per 4096"). */
        if ((++g_wwr_otherhits % 4096) == 0) {
            fprintf(stderr, "[watch-wr] %lu same-page-other-dword writes so far\n",
                    g_wwr_otherhits);
            fflush(stderr);
        }
    }
    /* Re-arm every watched page as PAGE_READONLY (traps the next write). */
    for (int i = 0; i < g_wwr_n; i++) {
        bool done_before = false;
        for (int j = 0; j < i; j++) if (g_wwr_page[j] == g_wwr_page[i]) { done_before = true; break; }
        if (done_before) continue;
        DWORD old;
        VirtualProtect((void*)g_wwr_page[i], 0x1000, PAGE_READONLY, &old);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* Parse YZ_WATCH_WR and arm the page guards. Called once from main(), right
 * after vm_init() (same placement rationale as YZ_WATCH_EA: some watched EAs
 * can be written very early, before the "natural" per-subsystem arm point).
 * Env unset -> this function isn't even called (guard at the call site) --
 * a diagnostic must never perturb the system it measures; zero perturbation when off. */
extern "C" void yz_watch_wr_init(void)
{
    const char* s = getenv("YZ_WATCH_WR");
    if (!s || !*s) return;
    while (*s && g_wwr_n < YZ_WATCH_WR_MAX) {
        char* end = nullptr;
        unsigned long v = strtoul(s, &end, 16);
        if (end == s) break;
        uint32_t ea = (uint32_t)v;
        uint8_t* host = vm_base + ea;
        uintptr_t page = (uintptr_t)host & ~(uintptr_t)0xFFF;
        g_wwr_ea[g_wwr_n]   = ea;
        g_wwr_host[g_wwr_n] = host;
        g_wwr_page[g_wwr_n] = page;
        /* Commit the page explicitly BEFORE touching it: this init runs before
         * the vmguard lazy-commit VEH is installed, so a first-touch read here
         * AVs straight into the crash reporter (s26ride6/6b — mis-symbolized
         * as guest func_022001B4 per LESSONS #12; tramp_idx=0 was the tell).
         * MEM_COMMIT on an already-committed page is a no-op. */
        if (!VirtualAlloc((void*)page, 0x1000, MEM_COMMIT, PAGE_READWRITE)) {
            fprintf(stderr, "[watch-wr] COMMIT failed ea=0x%08X err=%lu — slot skipped\n",
                    ea, GetLastError());
            fflush(stderr);
            s = (*end == ',') ? end + 1 : end;
            continue;
        }
        uint32_t cur; memcpy(&cur, host, 4);
        g_wwr_old[g_wwr_n]  = cur;
        g_wwr_n++;
        fprintf(stderr, "[watch-wr] armed ea=0x%08X hostpage=0x%llX\n",
                ea, (unsigned long long)page);
        /* A page-guard traps the WHOLE 4 KB page. Warn loudly if
         * this page overlaps the known-hot SPURS mgmt page class, but arm it
         * anyway (spec: warn, don't refuse) -- caller owns the tradeoff.
         * (s26 fix: compare the GUEST page — the old host-page compare could
         * never match a guest constant.) */
        if ((ea & 0xFFFFF000u) == 0x40197000u) {
            fprintf(stderr, "[watch-wr] WARNING: ea=0x%08X is on the hot SPURS-mgmt page "
                    "0x40197xxx -- this page-guard single-steps EVERY access to the whole "
                    "4KB page and can badly slow or wedge the run.\n", ea);
        }
        fflush(stderr);
        s = (*end == ',') ? end + 1 : end;
    }
    if (g_wwr_n == 0) return;
    AddVectoredExceptionHandler(1, yz_watch_wr_veh);
    for (int i = 0; i < g_wwr_n; i++) {
        bool done_before = false;
        for (int j = 0; j < i; j++) if (g_wwr_page[j] == g_wwr_page[i]) { done_before = true; break; }
        if (done_before) continue;
        DWORD old;
        if (!VirtualProtect((void*)g_wwr_page[i], 0x1000, PAGE_READONLY, &old)) {
            fprintf(stderr, "[watch-wr] VirtualProtect FAILED for ea=0x%08X (err=%lu) — "
                    "watch on this page is DEAD, treat its zero-hits as PLAUSIBLE only\n",
                    g_wwr_ea[i], GetLastError());
            fflush(stderr);
        }
    }
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
/* ---- s24 round-driver chain probe (2026-07-09) --------------------------
 * scratch/patch_chain_probes.py injects `yz_chain_probe(ctx, 0xADDR)` at the
 * ENTRY of the tick-chain functions in the recomp chunks (oracle chain:
 * func_00DDDA6C -> 00DDDB3C -> 000D0CD8 -> 00D1E838 -> round driver 00A9F8AC,
 * plus the job writer 00E5F094 as the known-live control). This is the ONLY
 * instrument that sees direct-`bl` entries: the lifter's bl is a plain C call
 * (no ctx->lr write -> no saved-LR walk) and the tramp guard sees only
 * tail-branch hops. Volume-bounded: 4 first-hits per site + one census line
 * per 5 s (LESSONS #6c); racy counters tolerable (census). The first-hit
 * banner + the control site make zero-hit negatives MEASURED (LESSONS #21b). */
static unsigned g_yz_chain_addrs[16];
static volatile long long g_yz_chain_counts[16];

/* Independent census printer (LESSONS #6d: the in-probe census goes silent the
 * moment the probed functions stop running — exactly when the counters matter
 * most). Called from the probe's 5 s tick AND from the watchdog periodically. */
void yz_chain_census_dump(const char* tag)
{
    fprintf(stderr, "[chain] census(%s) t=%llums:", tag,
            (unsigned long long)GetTickCount64());
    for (int k = 0; k < 16 && g_yz_chain_addrs[k]; k++)
        fprintf(stderr, " 0x%08X=%lld", g_yz_chain_addrs[k],
                (long long)g_yz_chain_counts[k]);
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* s28 (ledger #64): the update loop's existence — set at the first
 * func_00D1E838 entry; gates the park-rel FAST tier (its 100 ms t1-frozen
 * witness misfires during boot-start, applying releases before t1 finalizes
 * the segment: 7/7 boots separated stalled-vs-clean purely by lever-fire
 * order). */
int g_yz_updloop_started = 0;

/* s30 §8.4 (scratch/s30_staging_decision.md): the boot-phase HOLD shim,
 * injected at func_001AB63C entry (the mode-machine's boot-flag clearer:
 * for mode ids [306..322] it ANDs vm[0x136786C] with ~0x400, ending the
 * GameMain boot pump that drives the CRI file preload — MEASURED to fire
 * mid-preload on our port, stranding the chain; on real HW the preload
 * always wins this race). When YZ_HOLD_BOOT_PHASE is set and the transition
 * would strand in-flight preload work, return-early defers the clear.
 * ALWAYS logs each call (mode id + preload state) — the OFF-mode log IS the
 * §8.4 discriminator (call cadence: single-shot vs retried decides whether
 * this hold design can work at all). Faithfulness: the game's own transition
 * manager (func_00507EB4) gates transitions on object-granular "loaded"
 * state; the hold only forbids an ordering real HW never exhibits. Capped:
 * after 1200 held calls it lets the clear through (a stuck preload must not
 * wedge the boot). Default OFF. */
extern "C" int yz_hold_boot(void* ctxv)
{
    /* v2 (§9.3): the primary injection site is func_001AAB20 — the
     * SINGLE-CALLER setter of the exit-request flag vm[0x1367874] that ends
     * GameMain's boot pump (D0140 then returns 0x80004005 and the preload
     * strands). The 1AB63C injection stays as insurance (measured 0 calls).
     * Predicate: the preload is ACTIVE = any live driver-pool slot with an
     * async op in flight (status==2) or open/close pending (phase!=0), a
     * staged command pending (w474), OR the staging seq advanced within the
     * last 120 s (the measured inter-file gap runs ~60 s at fast=100 — a
     * short window would release the hold between files; deliberately
     * immune to the CVM archive handle's permanent idle state). On real HW
     * the intro timeline always outlasts the seconds-long preload — the
     * hold forbids only an ordering hardware never exhibits. */
    static int on = -1;
    if (on < 0) {
        on = getenv("YZ_HOLD_BOOT") ? 1 : 0;
        fprintf(stderr, "[holdboot] ARMED (%s): 1AAB20+1AB63C shims live, hold %s\n",
                on ? "YZ_HOLD_BOOT" : "log-only", on ? "ENABLED" : "disabled");
        fflush(stderr);
    }
    (void)ctxv;
    static uint32_t last_seq = 0xFFFFFFFFu;
    static unsigned long long last_seq_ms = 0;
    const unsigned long long now = GetTickCount64();
    const uint32_t seq = vm_read32(0x16614B8u);
    if (seq != last_seq) { last_seq = seq; last_seq_ms = now; }
    int active = 0;
    uint32_t base = vm_read32(0x135CDFCu);
    if (base) {
        for (uint32_t k = 0; k < 40 && !active; k++) {
            uint32_t D = base + 0x868u + k * 0x4D0u;
            if (vm_read32(D) != 1) continue;
            if (vm_read32(D + 8u) == 2u || vm_read32(D + 0x448u) != 0u) active = 1;
        }
    }
    if (vm_read32(0x1661474u)) active = 1;
    if (last_seq_ms && (now - last_seq_ms) < 120000ull) active = 1;
    static long calls = 0, holds = 0;
    long c = ++calls;
    if (c <= 40 || (c & 0xFF) == 0) {
        fprintf(stderr, "[holdboot] call#%ld active=%d seq=%08X seq_age=%llums held=%ld\n",
                c, active, seq, last_seq_ms ? (now - last_seq_ms) : 0, holds);
        fflush(stderr);
    }
    if (!on) return 0;
    /* cap: 20000 held calls (~1/frame; covers a full slow preload) then let
     * the exit through so a genuinely stuck preload can't wedge the boot */
    if (active && holds < 20000) {
        holds++;
        if ((holds % 100) == 1) {
            fprintf(stderr, "[holdboot] HOLDING boot-exit (hold #%ld, seq=%08X)\n", holds, seq);
            fflush(stderr);
        }
        return 1;
    }
    if (on && holds >= 20000) {
        static int capped = 0;
        if (!capped) { capped = 1;
            fprintf(stderr, "[holdboot] CAP REACHED (20000) - letting the boot-exit through\n");
            fflush(stderr); }
    }
    return 0;
}

extern "C" void yz_chain_probe(void* ctxv, unsigned addr)
{
    if (addr == 0x00D1E838u) g_yz_updloop_started = 1;
    unsigned* as = g_yz_chain_addrs;
    volatile long long* ns = g_yz_chain_counts;
    static int banner = 0;
    static unsigned long long lastms = 0;
    ppu_context* ctx = (ppu_context*)ctxv;
    int i = 0;
    for (; i < 15 && as[i] && as[i] != addr; i++) {}
    if (!as[i]) as[i] = addr;
    long long n = ++ns[i];
    if (!banner) {
        banner = 1;
        fprintf(stderr, "[chainprobe] ARMED (first hit 0x%08X tid=%u)\n",
                addr, yz_thread_current_id());
    }
    if (n <= 4)
        fprintf(stderr, "[chain] 0x%08X hit#%lld tid=%u r3=0x%llX r4=0x%llX r5=0x%llX\n",
                addr, n, yz_thread_current_id(),
                (unsigned long long)ctx->gpr[3], (unsigned long long)ctx->gpr[4],
                (unsigned long long)ctx->gpr[5]);
    unsigned long long now = GetTickCount64();
    if (now - lastms >= 5000) {
        lastms = now;
        yz_chain_census_dump("probe");
    }
}

/* ---- s24 defer-gate watcher (2026-07-09, env YZ_DEFERWATCH) ---------------
 * The gcm flush decides defer-vs-immediate stopper release at ONE compare
 * (S[0x24] vs the buffer-descriptor end, pc ~0xE9BE4C — stopper_drain_re.md).
 * Ours takes DEFER 16x consecutively at the movie boundary (deadlock class);
 * RPCS3's boundary parks zero times. HYPOTHESIS ONLY — this watcher measures
 * the gate's actual inputs at each decision instead of assuming: on every
 * journal-head advance (S[0] += 0x20 = a defer happened) dump the new entry,
 * the compared values, and ring state. Defers are rare (~16/boot): uncapped. */
static DWORD WINAPI yz_deferwatch_thread(LPVOID)
{
    fprintf(stderr, "[deferwatch] ARMED: journal-head advance watcher (1ms poll)\n");
    fflush(stderr);
    uint32_t last_head = 0;
    for (;;) {
        Sleep(1);
        if (!g_yz_game_toc || !vm_base) continue;
        const uint32_t S = vm_read32(g_yz_game_toc - 0x7410u);
        if (S < 0x10000u || S >= 0xE0000000u) continue;
        const uint32_t head = vm_read32(S + 0x00u);
        if (head == last_head) continue;
        const uint32_t prev = last_head;
        last_head = head;
        if (!prev) continue;                      /* first observation: baseline only */
        /* dump every new entry [prev, head) plus the gate inputs */
        const uint32_t ptr14 = vm_read32(g_yz_game_toc - 0x7414u);
        const uint32_t bd    = (ptr14 >= 0x10000u && ptr14 < 0xE0000000u)
                                   ? vm_read32(ptr14) : 0;
        fprintf(stderr, "[deferwatch] head 0x%08X->0x%08X | S1C=0x%08X S20=0x%08X "
                "S24=0x%08X | bufdesc=0x%08X end[+4]=0x%08X cur[+8]=0x%08X | "
                "PUT=0x%08X GET=0x%08X\n",
                prev, head,
                vm_read32(S + 0x1Cu), vm_read32(S + 0x20u), vm_read32(S + 0x24u),
                bd,
                bd ? vm_read32(bd + 4u) : 0, bd ? vm_read32(bd + 8u) : 0,
                vm_read32(0x10000040u), vm_read32(0x10000044u));
        for (uint32_t e = prev; e != head && (e - prev) < 0x400u; e += 0x20u)
            fprintf(stderr, "[deferwatch]   entry@0x%08X tag=0x%02X ea=0x%08X\n",
                    e, vm_read32(e + 0x00u), vm_read32(e + 0x04u));
        fflush(stderr);
    }
    return 0;
}

/* ---- s24 late: 0x7F LOOKAHEAD DRAIN (env-gated via the lever) --------------
 * The park-triggered lever costs one park + threshold wait per stopper while
 * the journal holds ~41x more pending releases than the lever ever applies
 * (2,632 vs 64, scratch/s24dw_journal.md) — the measured seconds-per-flip
 * grind. This applies each tag-0x7F release AS SOON AS it is journaled,
 * provided PUT is ring-ahead of the stopper (the game has committed the body
 * past it — the same faithfulness argument the June review accepted for the
 * park-triggered form, applied earlier). The release VALUE is the game's own
 * semantics per the s24 drain RE (scratch/s24_drain_re.md, DISASM-VERIFIED):
 * the immediate-apply branch writes literal 0 to the stopper word. Journal
 * TAG WORDS ARE LEFT UNTOUCHED (eager tag-zeroing measured harmful 2026-07-02;
 * the consumption-proof retire stays park-side). Kill-switch YZ_NO_LOOKAHEAD.
 * Runs whenever YZ_PARK_REL is armed. */
static DWORD WINAPI yz_lookahead_thread(LPVOID)
{
    fprintf(stderr, "[lookahead] ARMED: journal 0x7F lookahead drain (1ms poll)\n");
    fflush(stderr);
    uint32_t cursor = 0;                       /* last journal entry examined */
    unsigned long applied = 0;
    for (;;) {
        Sleep(1);
        if (!g_yz_game_toc || !vm_base) continue;
        const uint32_t S = vm_read32(g_yz_game_toc - 0x7410u);
        if (S < 0x10000u || S >= 0xE0000000u) continue;
        const uint32_t base = vm_read32(S + 0x08u);
        const uint32_t head = vm_read32(S + 0x00u);
        if (base < 0x10000u || head < base || head - base > 0x1000000u) continue;
        if (cursor < base || cursor > head) cursor = base;
        const uint32_t put = vm_read32(0x10000040u);
        const uint32_t get = vm_read32(0x10000044u);
        const uint32_t ring = 0x800000u;
        for (; cursor < head; cursor += 0x20u) {
            if (vm_read32(cursor + 0x00u) != 0x7Fu) continue;
            const uint32_t stopper_ea = vm_read32(cursor + 0x04u);
            if (stopper_ea < 0x40400000u || stopper_ea >= 0x40C00000u) continue;
            const uint32_t stopper_io = stopper_ea - 0x40400000u;
            /* only when the committed body extends past the stopper:
             * ring-distance from GET must place the stopper strictly inside
             * [GET, PUT) — the same PUT-ahead condition the lever uses. */
            const uint32_t ahead_put = (put - stopper_io + ring) % ring;
            const uint32_t ahead_get = (stopper_io - get + ring) % ring;
            if (ahead_put == 0u || ahead_put >= (ring >> 1)) continue;
            if (ahead_get >= (ring >> 1)) continue;
            const uint32_t w = vm_read32(stopper_ea);
            if ((w & 0xE0000003u) == 0x20000000u &&
                (w & 0x1FFFFFFCu) == stopper_io) {      /* still a live self-jump */
                vm_write32(stopper_ea, 0u);             /* the game's own release value */
                applied++;
                if (applied <= 24 || (applied & 0x3Fu) == 0) {
                    fprintf(stderr, "[lookahead] applied #%lu @io 0x%06X (GET=0x%06X PUT=0x%06X)\n",
                            applied, stopper_io, get, put);
                    fflush(stderr);
                }
            }
        }
    }
    return 0;
}

void yz_dump_guest_state(const ppu_context* gc, const char* tag)
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

    /* VALIDATED chain (2026-07-09 s24): saved-LR walk matching the RPCS3 oracle
     * walker (bc 8-aligned + climbing, LR at bc+0x10, 4-aligned, in a code range
     * INCLUDING firmware 0x02xxxxxx -- the scan above filtered libsre frames out
     * entirely and mis-symbolized frame data, LESSONS #12). One line, oracle
     * format, so ours-vs-RPCS3 chains diff cleanly. */
    {
        fprintf(stderr, "[%s] validated chain (saved-LR walk):", tag);
        uint32_t cur = (uint32_t)gc->gpr[1];
        int printed = 0;
        for (int d = 0; d < 64 && printed < 24; d++) {
            if (cur < 0x10000u || (cur & 7) ||
                (vm_base && IsBadReadPtr(vm_base + cur, 8))) break;
            uint32_t back = (uint32_t)vm_read64(cur);
            if ((back & 7) || back <= cur || back - cur > 0x100000u) break;
            if (vm_base && IsBadReadPtr(vm_base + back + 0x10, 8)) break;
            uint32_t slr = (uint32_t)vm_read64(back + 0x10);
            int in_game = (slr >= EXE_LO && slr < EXE_HI);
            int in_fw   = (slr >= 0x02000000u && slr < 0x02400000u);
            if (!(slr & 3u) && (in_game || in_fw)) {
                fprintf(stderr, " 0x%08X", slr);
                if (const yz_func_entry* fe = yz_func_from_guest(slr))
                    fprintf(stderr, "(f%08X+0x%X)", fe->addr, slr - fe->addr);
                printed++;
            }
            cur = back;
        }
        fprintf(stderr, " [frames=%d]\n", printed);
    }

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
HANDLE g_yz_t1_handle = nullptr;   /* s28: t1 real handle for the [t1-hb] heartbeat */
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
        /* An rip OUTSIDE our module = the thread is parked in a HOST wait
         * (ntdll/kernelbase) reached through an import bridge or runtime
         * helper -- exactly the waits the lv2 syscall recorder cannot see
         * (sys_lwmutex_lock, cellFs HLE, EnterCriticalSection...). Say so
         * instead of printing a meaningless cross-module "rva". */
        HMODULE rmod = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)rip, &rmod);
        if ((uintptr_t)rmod == mod) {
            fprintf(stderr, "[%s] HOST rip rva 0x%llX", tag,
                    (unsigned long long)((uintptr_t)rip - mod));
            if (const yz_func_entry* fe = yz_func_from_host(rip))
                fprintf(stderr, " (func_%08X +0x%llX)", fe->addr,
                        (unsigned long long)((uintptr_t)rip - (uintptr_t)fe->fn));
        } else {
            wchar_t mn[64] = L"?";
            if (rmod) { GetModuleFileNameW(rmod, mn, 64); mn[63] = 0; }
            fprintf(stderr, "[%s] HOST rip 0x%llX in %ls (host wait via import "
                    "bridge/runtime -- read the stack chain)", tag,
                    (unsigned long long)(uintptr_t)rip, mn);
        }
        fprintf(stderr, "\n[%s] HOST stack chain:", tag);
        const uint64_t* sp = (const uint64_t*)cx.Rsp;
        int shown = 0, raw_shown = 0;
        for (int i = 0; i < 1600 && shown < 14; i++) {
            if (IsBadReadPtr((void*)(sp + i), 8)) break;
            uint64_t v = sp[i];
            if (v < mod || v > mod + 0x40000000ull) continue;
            const yz_func_entry* fe = yz_func_from_host((void*)v);
            if (!fe) {
                /* in-module but not a lifted fn: our runtime/libs code --
                 * print the raw rva so it resolves offline vs the .map */
                if (raw_shown < 6) { raw_shown++;
                    fprintf(stderr, "\n    [+0x%03X] rva 0x%llX (runtime -- "
                            "resolve vs yakuza_recomp.map)", i * 8,
                            (unsigned long long)(v - mod));
                }
                continue;
            }
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
extern "C" void yz_thread_registry_raw(void);   /* s29 [t11fate] raw dump */

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

/* LAYER-1 DEADLOCK SNAPSHOT (env YZ_L1SNAP, 2026-06-24). Disambiguates the two
 * remaining root candidates for the FIFO deadlock:
 *   #2 (libgcm reserve ordering): the reserve func_02103AAC waits while GET is
 *      INSIDE its recycle window [reserve_lo, reserve_hi] (t1 frame locals at
 *      r1+0x70 / r1+0x74, io offsets written by func_02103F90). If GET is inside
 *      the very segment GET sits in, the reserve is recycling an occupied segment.
 *   #1 (producer never produced the body / gs_task): every guest thread's
 *      blocked-in-syscall state + whether the SPU/worker threads are idle.
 * Read-only; fires once at the wedge. */
static void yz_dump_layer1_snapshot(const char* tag)
{
    if (g_yz_main_ctx) {
        uint32_t r1  = (uint32_t)g_yz_main_ctx->gpr[1];
        uint32_t rlo = vm_read32(r1 + 0x70);
        uint32_t rhi = vm_read32(r1 + 0x74);
        uint32_t get = vm_read32(0x10000044u) & ~3u;
        uint32_t put = vm_read32(0x10000040u) & ~3u;
        uint32_t bd  = vm_read32(0x02114000u - 0x7FD8u);   /* bufdesc ptr */
        uint32_t cur = (bd >= 0x10000u && bd < 0xE0000000u) ? vm_read32(bd + 0x8) : 0;
        uint32_t beg = (bd >= 0x10000u && bd < 0xE0000000u) ? vm_read32(bd + 0x0) : 0;
        int get_in = (get >= rlo && get <= rhi);
        fprintf(stderr, "[%s] L1: t1.r1=0x%08X  RESERVE-WINDOW reserve_lo=0x%06X reserve_hi=0x%06X "
                "(recycling seg %u)\n", tag, r1, rlo, rhi, (rlo & 0x7FFFFF) >> 20);
        fprintf(stderr, "[%s] L1: GET=0x%06X (seg %u)  PUT=0x%06X (seg %u)  bufdesc.cur=0x%08X begin=0x%08X\n",
                tag, get, get >> 20, put, put >> 20, cur, beg);
        fprintf(stderr, "[%s] L1: GET is %s the reserve window => %s\n", tag,
                get_in ? "INSIDE" : "OUTSIDE",
                get_in ? "reserve waits for GET to leave a segment GET occupies (FIFO ordering / #2)"
                       : "reserve window does NOT contain GET (look upstream: producer never built body / #1)");
        fflush(stderr);
    }
    yz_dump_all_threads(tag);
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
    /* THE INVASIVE DUMPS ARE YZ_L1SNAP-GATED (2026-07-02, twice-burned): the
     * all-threads snapshot suspends every guest thread serially and its
     * host-stack walks take 60+ SECONDS (froze the guest +30s->+90s and
     * invalidated four 8-boot validation loops); even the t1-only host-stack
     * + spin sampler (active again since the g_yz_main_hthread fix -- they
     * had been silently no-opping on a never-assigned handle) suspend t1
     * repeatedly right in the PortStart window and flipped a ~3/4-good
     * baseline to 0/3. A diagnostic must never perturb the system it
     * measures; the read-only guest-state dump below is the only default. */
    if (g_yz_main_ctx) { yz_dump_guest_state(g_yz_main_ctx, "watchdog-30s");
                         if (getenv("YZ_L1SNAP")) yz_dump_main_host_stack("watchdog-30s"); }
    /* s31 W2LIFE (ledger #71): one early (usually pre-CRI-transition) sample of
     * the SPURS wid accounting + per-SPU liveness, then one per watchdog minute
     * below -- the healthy-vs-dead comparison for the wid2 journal consumer. */
    yz_w2life_dump("watchdog-30s");
    if (getenv("YZ_L1SNAP")) { yz_sample_t1_spin("watchdog-30s");
                               yz_dump_layer1_snapshot("watchdog-30s"); }
    Sleep(15000);
    if (g_yz_main_ctx) { yz_dump_guest_state(g_yz_main_ctx, "watchdog-45s");
                         if (getenv("YZ_L1SNAP")) yz_dump_main_host_stack("watchdog-45s"); }
    Sleep(15000);
    if (g_yz_main_ctx) { yz_dump_guest_state(g_yz_main_ctx, "watchdog-60s");
                         if (getenv("YZ_L1SNAP")) yz_dump_main_host_stack("watchdog-60s"); }
    /* s24: keep sampling forever (read-only dumps only). The fixed 30/45/60 s
     * schedule missed every late wedge — e.g. the iteration-4 wedge at ~207 s
     * had no dump. Every 60 s: t1 guest state + the chain-probe census (the
     * in-probe census goes silent exactly when t1 wedges, LESSONS #6d). */
    for (int mins = 2; ; mins++) {
        Sleep(60000);
        char tag[32];
        snprintf(tag, sizeof tag, "watchdog-%dm", mins);
        if (g_yz_main_ctx) yz_dump_guest_state(g_yz_main_ctx, tag);
        yz_chain_census_dump(tag);
        yz_w2life_dump(tag);   /* s31 W2LIFE */
    }
    return 0;
}

/* Pointers to the MAIN (t1) thread's trampoline ring, captured on the main thread
 * so a separate monitor thread can print t1's recent indirect-call path -- reliable,
 * unlike the nearest-symbol back-chain that misattributes return addresses to data. */
static void**    g_yz_main_tramp     = nullptr;
static uint64_t* g_yz_main_tramp_r31 = nullptr;
static uint64_t* g_yz_main_tramp_lr  = nullptr;
static unsigned* g_yz_main_tramp_idx = nullptr;
extern "C" __declspec(thread) uint64_t g_yz_tramp_lr[256];
/* usleep-caller capture (dispatch.cpp): arm g_yz_catch_caller -> next usleep dispatch
 * fills g_yz_caller_bt[] with the live host backtrace = the guest poll-loop chain. */
extern "C" volatile long g_yz_catch_caller;
extern "C" void*         g_yz_caller_bt[32];
extern "C" volatile long g_yz_caller_bt_n;

/* FLIP-ADVANCE monitor (env YZ_FLIPADV, 2026-06-20 pt26). The resync loop proved
 * that forcing GET past t1's wedge frees it to build + flip the NEXT frame -- but a
 * blind GET=PUT OVERSHOOTS that frame's in-stream flip command (method 0x0004E944),
 * so the flip is never dispatched and the fence sticks one frame short (measured:
 * resync gets fence 2->3 then GET=PUT skips frame-4's flip @io 0x50..., fence stays 3).
 * THIS monitor advances GET to the NEXT flip command instead of past it: when GET is
 * stuck with committed data ahead, scan [GET,PUT) (wrap-aware) for 0x0004E944 and set
 * GET there so the consumer dispatches the flip (fence++); if none is committed yet,
 * fall back to GET=PUT to free t1's reserve so it builds the next frame. Repeating this
 * walks the fence forward frame-by-frame = continuous flips (LAYER-1 animation). */
/* All GET/fence writes below hold the RSX FIFO lock and revalidate the sampled
 * state under it (audit 2026-07-01: the unlocked writes raced the consumer's
 * read-decide-write -- GET could be clobbered BACKWARD mid-applier). */
extern "C" void yz_rsx_fifo_acquire(void);
extern "C" void yz_rsx_fifo_release(void);
extern "C" int  yz_rsx_flip_pending_any(void);
static DWORD WINAPI yz_flip_advance(LPVOID)
{
    const uint32_t FIFO_BASE = 0x40400000u;   /* bufdesc+0x14 io base */
    const uint32_t RING_LO = 0x1000u, RING_HI = 0x800000u;
    const uint32_t FLIP_CMD = 0x0004E944u;    /* NV DRIVER_QUEUE flip method, count 1 */
    Sleep(8000);                              /* let frames 1-2 render first */
    uint32_t last_get = ~0u; DWORD stuck_since = 0; int n = 0;
    /* INSURANCE (so the band-aid can never silently be "the reason it's not working"):
     * each intervention type logs a steady heartbeat (rate-limited to 1/750ms, NO hard
     * cap), tagged FORCED, with the running total -- its footprint stays visible for the
     * whole run so a movie bug can always be A/B'd against it. */
    DWORD fa_nudge_lg = 0, fa_flip_lg = 0, fa_free_lg = 0;
    /* The drained-ring fence-nudge (ROOT 2) is now done faithfully at the real vblank
     * flip-completion (import_overrides.cpp); keep the old forced nudge OFF by default
     * (YZ_THR_NUDGE re-enables it for A/B) so the fence isn't double-advanced. */
    /* Default ON: the drained-ring fence nudge breaks the residual flip-throttle deadlock
     * (GET=PUT drained, no pending flip, t1 wedged at the throttle waiting for the fence --
     * the faithful vblank path CANNOT advance the fence here because nothing is pending, so
     * only the nudge breaks it). It never double-advances the faithful fence: it fires only
     * in the drained-no-flip state, which is mutually exclusive with a pending flip.
     * Off-switch: YZ_NO_THR_NUDGE. */
    static int thr_nudge = -1; if (thr_nudge < 0) thr_nudge = getenv("YZ_NO_THR_NUDGE") ? 0 : 1;
    for (;;) {
        Sleep(20);
        uint32_t fence = vm_read32(0x40C00000u);
        if (fence < 2u) { last_get = ~0u; continue; }
        uint32_t put = vm_read32(0x10000040u) & ~3u;
        uint32_t get = vm_read32(0x10000044u) & ~3u;
        DWORD now = GetTickCount();
        if (get != last_get) { last_get = get; stuck_since = now; continue; }
        if ((now - stuck_since) <= 100u) continue;      /* not wedged yet */
        const int in_ring = (get >= RING_LO && get < RING_HI);

        /* FRAME-4 / drained-ring diagnostic (ROOT 2): GET==PUT means the consumer has
         * caught up to the committed write head, yet the fence isn't advancing -- t1
         * wrote the next flip but never committed it (PUT not flushed past it), or the
         * throttle counter the game polls isn't being updated. One-shot census of every
         * flip command in the whole ring + the words around PUT/ctx-current so we can
         * see exactly where the next flip sits relative to PUT. */
        if (get == put) {
            /* FRAME-4 / ROOT 2: the ring is drained and PUT points at the next frame's
             * un-released entry stopper (measured: io 0x50030C = 0x2050030C). t1 wrote it
             * then wedged in the flip THROTTLE func_00EAC46C, which spins
             * `while *(*(0x0164FE78)) + 2 <= target` -- a flip-completion counter our
             * runtime never advances (we update the fence @0x40C00000 + the head flip
             * flags, but NOT the game-object counter the throttle polls). With Root 1
             * (the reserve) now cleared by reaching fence 3, FORCE that counter so the
             * throttle returns -> t1 builds + commits frame 4 (releases the stopper,
             * advances PUT) -> the consumer (GET!=PUT again) processes it. */
            /* The ring is genuinely drained: the consumer processed every committed flip
             * and caught up to t1's write head. If t1 keeps producing, PUT advances and we
             * resume; if t1 has STOPPED (measured: after the first ~6 movie frames t1 spins
             * in guest code, only t7/vblank active, ctr stuck at 0xFE00018C -- waiting on an
             * UPSTREAM producer, not the RSX/throttle), we idle cleanly here. Earlier
             * band-aids (force the throttle counter / circulate GET around the empty ring)
             * just re-presented the static ring content (fence oscillated 2<->6) -- removed:
             * they faked progress. One-shot census so we can see the stop state. */
            /* DRIVE t1 PAST THE FLIP THROTTLE (func_00EAC46C, verified 2026-06-20 from
             * t1's stall dump: r31=0x0164FAE0, base=*(r31+0x398)=0x40C00000, target r28=5).
             * The throttle spins `while *(0x40C00000)+2 <= target` -- its completion
             * counter IS the flip fence @0x40C00000. Once GET drains (get==put) the fence
             * is frozen (no committed flip for the consumer to dispatch), but t1 must pass
             * the throttle to BUILD frame N+1's flip -> circular deadlock. Earlier blind
             * force of the fence faked progress (re-presented static frames); instead nudge
             * it by +1 (release ONE frame) and VERIFY t1 actually produced it -- PUT moving
             * means t1 built a real frame, after which the in_ring scan walks its flip. If
             * PUT stays frozen for 1s of nudging, t1 has genuinely stopped (upstream stall)
             * -> latch thr_dead so we stop nudging (don't fake it) and fall through to diag.
             * A new drain episode is detected by PUT differing from the last (t1 progressed),
             * which re-arms the nudger; nudges are throttled to ~150ms so the fence isn't
             * inflated far past target (usually +1 per frame matches submitted++). */
            static int thr_dead = 0; static uint32_t thr_put = ~0u;
            static DWORD thr_ep0 = 0, thr_last_nudge = 0;
            /* RE-ARM on a new drain point (2026-06-20 pt27): PUT differing from the last
             * episode means t1 made real progress since we latched -- e.g. a PHASE
             * TRANSITION (the game orderly-tears-down the movie/audio via a shutdown event
             * and moves on). Clear thr_dead so the nudger resumes driving the next phase
             * through its own flip throttle, instead of staying latched at the movie wall.
             * Still sound: we only ever nudge when PUT is actively advancing (real work). */
            if (put != thr_put) { thr_put = put; thr_ep0 = now; thr_last_nudge = 0; thr_dead = 0; }
            if (!thr_dead && thr_nudge) {
                if ((now - thr_ep0) <= 1000u) {
                    /* ENFORCE the "mutually exclusive with a pending flip" claim
                     * (audit 2026-07-01): if a real flip is pending retirement, the
                     * faithful vblank path will advance the fence -- nudging too
                     * double-advances it. Fence RMW under the FIFO lock, drained
                     * state revalidated, so the two writers can't interleave. */
                    if (now - thr_last_nudge >= 150u && !yz_rsx_flip_pending_any()) {
                        yz_rsx_fifo_acquire();
                        uint32_t g2 = vm_read32(0x10000044u) & ~3u;
                        uint32_t p2 = vm_read32(0x10000040u) & ~3u;
                        if (g2 == p2 && !yz_rsx_flip_pending_any()) {
                            uint32_t f = vm_read32(0x40C00000u);
                            vm_write32(0x40C00000u, f + 1u);   /* release throttle for one frame */
                            thr_last_nudge = now;
                            ++n;
                            if (now - fa_nudge_lg >= 750u) { fa_nudge_lg = now;
                                fprintf(stderr, "[flipadv] FORCED throttle-nudge fence %u->%u (PUT 0x%06X) [%d total] -- band-aid, not real GPU\n",
                                        f, f + 1u, put, n); fflush(stderr); }
                        }
                        yz_rsx_fifo_release();
                    }
                    continue;
                }
                thr_dead = 1;
                fprintf(stderr, "[flipadv] throttle nudge inert (PUT 0x%06X frozen 1s) -> t1 stopped producing\n", put);
            }
            static int dumped = 0;
            if (!dumped && fence >= 2u && (now - stuck_since) > 1200u) {
                dumped = 1;
                extern ppu_context* g_yz_main_ctx;
                uint32_t t1ctr = g_yz_main_ctx ? (uint32_t)g_yz_main_ctx->ctr : 0u;
                fprintf(stderr, "[flip-diag] DRAINED+t1-stalled: GET=PUT=0x%06X word@PUT=0x%08X t1.ctr=0x%08X\n",
                        get, vm_read32(FIFO_BASE + put), t1ctr);
                int fc = 0;
                for (uint32_t io = RING_LO; io < RING_HI; io += 4u)
                    if (vm_read32(FIFO_BASE + io) == FLIP_CMD) fc++;
                fprintf(stderr, "    flip cmds still in ring: %d (consumer caught up; t1 not producing)\n", fc);
                /* DECISIVE (pt48b): PUT-publish off-by-one vs empty-ring boundary.
                 * Compare the producer WRITE cursor (libgcm bufdesc @0x0210C3FC, cur
                 * at +0x08 as an ea) to PUT, and locate the awaited SET_REFERENCE
                 * header (0x00040050) in the ring. If cur>PUT, t1 wrote commands it
                 * never published (PUT-publish bug); if cur==PUT, the ring is truly
                 * drained (empty-ring boundary / GET-wrap issue). */
                {
                    uint32_t ref  = vm_read32(0x10000048u);
                    uint32_t bd   = 0x0210C3FCu;
                    uint32_t cur  = vm_read32(bd + 0x08u);
                    uint32_t curio = (cur >= FIFO_BASE && cur < FIFO_BASE + 0x800000u) ? (cur - FIFO_BASE) : 0xFFFFFFFFu;
                    fprintf(stderr, "    REF=0x%08X bufdesc:+00=%08X +04=%08X +08cur=%08X +0C=%08X +14=%08X  cur_io=0x%06X PUT=0x%06X %s\n",
                            ref, vm_read32(bd), vm_read32(bd+4u), cur, vm_read32(bd+0xCu), vm_read32(bd+0x14u), curio, put,
                            curio==0xFFFFFFFFu ? "(cur not in ring)" :
                            curio > put ? "<<< CUR>PUT: t1 wrote past PUT (PUT-publish off-by-one)" :
                            curio == put ? "<<< CUR==PUT (everything published; empty-ring/GET-wrap)" : "(cur<put)");
                    fprintf(stderr, "    ring near PUT:");
                    for (int32_t d = -0x20; d <= 0x40; d += 4) {
                        uint32_t io = (put + (uint32_t)d) & 0x7FFFFCu;
                        uint32_t w  = vm_read32(FIFO_BASE + io);
                        const char* m = (w == 0x00040050u) ? "<SETREF" : ((w & 3u)==1u ? "<jmp" : "");
                        fprintf(stderr, " %+d:%08X%s", d, w, m);
                    }
                    fprintf(stderr, "\n");
                }
                /* Reliable: t1's trampoline-hop chain (newest first) names the guest
                 * functions t1 last called -> the poll loop it is stuck in -> the
                 * upstream producer it waits on. (The back-chain/ctr mis-symbolize.) */
                extern const yz_func_entry* yz_func_from_guest(uint32_t);
                if (g_yz_main_tramp && g_yz_main_tramp_idx) {
                    fprintf(stderr, "    t1 trampoline-hop chain (newest first: TARGET <- CALLER):\n");
                    unsigned idx = *g_yz_main_tramp_idx;
                    for (unsigned i = 0; i < 12 && i < idx; i++) {
                        unsigned slot = (idx - 1 - i) & 255;
                        const yz_func_entry* fe = yz_func_from_host(g_yz_main_tramp[slot]);
                        uint32_t lr = g_yz_main_tramp_lr ? (uint32_t)g_yz_main_tramp_lr[slot] : 0u;
                        const yz_func_entry* cf = yz_func_from_guest(lr);
                        fprintf(stderr, "      func_%08X <- caller lr=0x%08X %s%08X\n",
                                fe ? fe->addr : 0xFFFFFFFFu, lr,
                                cf ? "func_" : "?", cf ? cf->addr : lr);
                    }
                }
                /* The poll loop keeps its CONDITION pointer in a callee-saved register
                 * (r14-r31, which survive the usleep call) or a stack local. Dump them +
                 * what they point to so we can spot the flag t1 spins on, then watch it. */
                if (g_yz_main_ctx) {
                    fprintf(stderr, "    t1 callee-saved GPRs (+pointees) [r3=0x%08X]:\n",
                            (uint32_t)g_yz_main_ctx->gpr[3]);
                    for (int r = 14; r <= 31; r++) {
                        uint32_t v = (uint32_t)g_yz_main_ctx->gpr[r];
                        if (v >= 0x10000u && v < 0xE0000000u)
                            fprintf(stderr, "      r%d=0x%08X -> [0]=0x%08X [4]=0x%08X [8]=0x%08X\n",
                                    r, v, vm_read32(v), vm_read32(v + 4u), vm_read32(v + 8u));
                        else
                            fprintf(stderr, "      r%d=0x%08X\n", r, v);
                    }
                    uint32_t sp = (uint32_t)g_yz_main_ctx->gpr[1];
                    fprintf(stderr, "    t1 stack frame [r1=0x%08X]:\n", sp);
                    for (uint32_t o = 0; o <= 0x60u && (sp + o) < 0xE0000000u; o += 4u) {
                        uint32_t w = vm_read32(sp + o);
                        const char* tag = "";
                        if (w >= 0x10000u && w < 0xE0000000u) tag = " (ptr)";
                        fprintf(stderr, "      [sp+0x%02X]=0x%08X%s\n", o, w, tag);
                    }
                    /* FLAG DIAGNOSTIC: dump the engine objects on t1's stack (recurring
                     * across stalls) -- the poll flag t1 spins on is a field in one of
                     * these. 0x013618B8 / 0x015BF120 are game-range engine objects;
                     * follow every distinct stack pointer + dump 0x40 bytes. */
                    uint32_t seen[16]; int ns = 0;
                    for (uint32_t o = 0; o <= 0x60u; o += 4u) {
                        uint32_t p = vm_read32(sp + o);
                        if (p < 0x10000u || p >= 0xE0000000u) continue;
                        if (IsBadReadPtr(vm_base + p, 0x40u)) continue;   /* skip data misread as a ptr (e.g. "SLLZ") */
                        int dup = 0; for (int k = 0; k < ns; k++) if (seen[k] == p) dup = 1;
                        if (dup || ns >= 16) continue; seen[ns++] = p;
                        fprintf(stderr, "    obj @0x%08X:", p);
                        for (uint32_t j = 0; j < 0x40u; j += 4u) fprintf(stderr, " %08X", vm_read32(p + j));
                        fprintf(stderr, "\n");
                    }
                    /* ENGINE VTABLE: func_010F35CC and siblings dispatch via the handler
                     * table at 0x01324BF0 (entries = OPD pointers). Resolve the registered
                     * handlers -> the one t1 waits in (returns the stuck state 0-6). */
                    fprintf(stderr, "    engine vtable raw @0x01324BC0..0x01324C40:\n");
                    for (uint32_t vi = 0; vi < 0x20u; vi++) {
                        uint32_t a = 0x01324BC0u + vi * 4u;
                        uint32_t ptr = vm_read32(a);
                        if (ptr >= 0x10000u && ptr < 0xE0000000u && !IsBadReadPtr(vm_base + ptr, 4u)) {
                            uint32_t code = vm_read32(ptr);
                            fprintf(stderr, "      [0x%08X]=0x%08X -> code=0x%08X\n", a, ptr, code);
                        } else {
                            fprintf(stderr, "      [0x%08X]=0x%08X\n", a, ptr);
                        }
                    }
                }
                g_yz_catch_caller = 1;   /* arm: next usleep dispatch captures the live caller chain */
                fflush(stderr);
            }
            /* Resolve the captured live host backtrace -> the guest poll-loop chain. */
            static int bt_printed = 0;
            if (dumped && !bt_printed && g_yz_caller_bt_n > 0) {
                bt_printed = 1;
                long btn = g_yz_caller_bt_n;
                fprintf(stderr, "[flip-diag] t1 LIVE caller chain at usleep (%ld frames):\n", btn);
                uintptr_t mod = (uintptr_t)GetModuleHandleW(NULL);
                for (long i = 0; i < btn && i < 32; i++) {
                    void* a = g_yz_caller_bt[i];
                    const yz_func_entry* fe = yz_func_from_host(a);
                    if (fe)
                        fprintf(stderr, "    [%ld] func_%08X +0x%llX\n", i, fe->addr,
                                (unsigned long long)((uintptr_t)a - (uintptr_t)fe->fn));
                    else
                        fprintf(stderr, "    [%ld] rva 0x%llX\n", i,
                                (unsigned long long)((uintptr_t)a - mod));
                }
                fflush(stderr);
            }
            continue;
        }

        /* Wedged with committed data ahead [GET,PUT). If GET is parked IN the ring, scan
         * forward (wrap-aware) for the next committed flip command and advance GET to it
         * (dispatch the flip, fence++). If GET is parked OUTSIDE the ring (followed a
         * stale display-list CALL) or no flip is committed yet, free t1 with GET=PUT so
         * it builds + commits the next frame. */
        uint32_t found = 0;
        if (in_ring) {
            /* Scan FORWARD from GET for the next committed flip command, but NEVER wrap
             * into lower segments: when PUT has wrapped below GET, the region [RING_LO,PUT)
             * holds OLD (already-presented) flip commands from the previous lap -- jumping
             * GET backward to them re-presents stale frames AND makes t1's reserve see GET
             * move backward (confusing its danger-region math -> producer stall). So bound
             * the scan to GET..min(PUT-if-ahead, RING_HI), no wrap. */
            uint32_t limit = (put > get) ? put : RING_HI;
            for (uint32_t scan = get; scan < limit; scan += 4) {
                if (vm_read32(FIFO_BASE + scan) == FLIP_CMD) { found = scan; break; }
            }
        }
        /* pt48b ROOT FIX: force-advancing GET past committed commands SKIPS them.
         * If we skip a SET_REFERENCE the game busy-waits on (func_00EBBFB4:
         * while(*(0x10000048)!=N)), t1 deadlocks forever. Before moving GET, APPLY
         * any SET_REFERENCE in the span we're about to skip, so REF tracks like a
         * real consumer would. (Measured: the dropped SET_REFERENCE(3) at PUT-8 was
         * THE wedge -- GET jumped over it, REF stuck at 2, t1 hung on REF!=3.) */
        /* Apply any SET_REFERENCE in the span the band-aid is about to skip, so REF
         * tracks (a real consumer writes it). A general "execute every skipped method
         * packet" was tried and REVERTED -- a linear parse mis-reads vertex/shader DATA
         * as method headers (observed REF=0x8186C083 garbage), and it bought no extra
         * progress (same next wall). SET_REFERENCE (count-1, method 0x50) is specific
         * enough to be data-safe and only writes the bounded REF register. (pt48b: the
         * dropped SET_REFERENCE was THE wedge -- t1 hung on `while(REF!=N)`.) */
        /* REF-apply + GET-advance as ONE atomic step under the FIFO lock (audit
         * 2026-07-01): unlocked, this raced the consumer/applier's own
         * read-decide-write on GET -- either side could clobber the other's GET
         * (including BACKWARD, which stalls t1's reserve math). Revalidate that
         * GET is still where we sampled it; if the consumer moved it, our wedge
         * diagnosis is stale -- drop the intervention and re-observe. */
        yz_rsx_fifo_acquire();
        uint32_t curget = vm_read32(0x10000044u) & ~3u;
        if (curget != get) {
            yz_rsx_fifo_release();
            last_get = curget; stuck_since = now;
            continue;
        }
        {
            uint32_t adv_target = (found && found != get) ? found : put;
            if (adv_target > get) {
                uint32_t refval = 0, refn = 0;
                for (uint32_t scan = get; scan < adv_target; scan += 4u) {
                    uint32_t w = vm_read32(FIFO_BASE + scan);
                    if (!(w & 0xA0030003u) && ((w >> 18) & 0x7FFu) == 1u && (w & 0x3FFFCu) == 0x50u) {
                        refval = vm_read32(FIFO_BASE + scan + 4u);
                        vm_write32(0x10000048u, refval);   /* RSX REF register (DMACTL+0x48) */
                        refn++;
                    }
                }
                if (refn) { static DWORD rlg = 0; if (now - rlg >= 750u) { rlg = now;
                    fprintf(stderr, "[flowctl] applied %u SET_REFERENCE in [0x%06X,0x%06X) -> REF=0x%08X "
                            "(would have been dropped -> t1 REF-wait deadlock)\n", refn, get, adv_target, refval);
                    fflush(stderr); } }
            }
        }
        if (found && found != get) {
            vm_write32(0x10000044u, found);             /* dispatch this frame's flip */
            last_get = found; stuck_since = now; ++n;
            if (now - fa_flip_lg >= 750u) { fa_flip_lg = now;
                fprintf(stderr, "[flowctl] GET 0x%06X -> flip@0x%06X (fence=%u PUT 0x%06X) [%d] -- consumer past stopper; fence advances at real vblank flip\n",
                        get, found, fence, put, n); fflush(stderr); }
        } else {
            vm_write32(0x10000044u, put);               /* free t1 (stale list / no flip) */
            last_get = put; stuck_since = now; ++n;
            if (now - fa_free_lg >= 750u) { fa_free_lg = now;
                fprintf(stderr, "[flowctl] GET 0x%06X -> PUT 0x%06X (fence=%u, %s) [%d] -- freed t1's ring reserve\n",
                        get, put, fence, in_ring ? "no flip" : "stale-list", n); fflush(stderr); }
        }
        yz_rsx_fifo_release();
    }
}

/* TASKSET BITSET WATCH (env YZ_TS_WATCH, 2026-06-20 pt27). Once the CRI codec taskset
 * is captured (g_yz_spurs_taskset, set by dispatch.cpp at CreateTask2), poll its 6 task
 * bitsets in MAIN memory and log every change with a timestamp. Reveals the created
 * task's lifecycle -- enabled->ready->running->done (the SPU ran it) vs
 * enabled->disabled-without-ever-running (the policy rejected it) -- and, cross-checked
 * with the SPU-side [ts-watch] log, whether any clear comes from the PPU (no SPU entry). */
extern "C" uint32_t g_yz_spurs_taskset;
/* GENERIC guest-memory peek (env YZ_PEEK, 2026-07-02): comma-separated hex EAs
 * (up to 16), each dumped as 4 words on CHANGE, VirtualQuery-guarded. Replaces
 * the recompile-a-peek-array-per-question loop that cost a rebuild cycle every
 * time a new address became interesting. Example:
 *   YZ_PEEK=63D61400,63D61600,40197C80 */
static DWORD WINAPI yz_peek_thread(LPVOID)
{
    uint32_t ea[16]; int n = 0;
    { const char* s = getenv("YZ_PEEK");
      while (s && *s && n < 16) {
          char* end = NULL;
          unsigned long v = strtoul(s, &end, 16);
          if (end == s) break;
          ea[n++] = (uint32_t)v;
          s = (*end == ',') ? end + 1 : end;
          if (*s == '\0') break;
      } }
    if (!n) return 0;
    fprintf(stderr, "[peek] watching %d address(es)\n", n);
    uint32_t last[16][4]; memset(last, 0xFF, sizeof(last));
    DWORD t0 = 0;
    for (;;) {
        Sleep(1);
        if (!vm_base) continue;
        if (!t0) t0 = GetTickCount();
        for (int k = 0; k < n; k++) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(vm_base + ea[k], &mbi, sizeof(mbi))
                || mbi.State != MEM_COMMIT) continue;
            uint32_t v[4];
            for (int j = 0; j < 4; j++) v[j] = vm_read32(ea[k] + (uint32_t)j * 4u);
            if (memcmp(v, last[k], sizeof(v)) != 0) {
                fprintf(stderr, "[peek +%lums @%08X] %08X %08X %08X %08X\n",
                        GetTickCount()-t0, ea[k], v[0], v[1], v[2], v[3]);
                fflush(stderr);
                memcpy(last[k], v, sizeof(v));
            }
        }
    }
}

static DWORD WINAPI yz_ts_watch(LPVOID)
{
    /* Watch BOTH live tasksets (2026-07-02): the CRI codec taskset (wid 3) and
     * the 4-task worker pool (wid 4), plus whatever g_yz_spurs_taskset points
     * at. Change-triggered dump of the six task bitmaps -- shows whether the
     * PPU ever sets `signalled` for the WAIT_SIGNAL-parked tasks and whether
     * the kernel moves them waiting->ready->running. */
    /* Codec init-status peeks (2026-07-02, rides with YZ_TS_WATCH): the codec
     * task's entire init writes exactly two blocks -- a 4-byte status at
     * 0x63D11CA0 and 0x20 bytes at 0x63D616A0 (measured, YZ_CODEC_PUT). Dump
     * them on change so we can see the value + whether the PPU voice layer
     * ever updates/consumes them. */
    static const uint32_t peek[6] = { 0x63D61400u, 0x63D61410u,   /* CRI request CellSpursQueue hdr (PPU pushes) */
                                      0x63D61460u, 0x63D61420u,   /* req queue +0x60 (taskset EA — SendSignal gate) + waiter byte +0x20 */
                                      0x63D61600u, 0x63D61610u }; /* CRI response CellSpursQueue hdr */
    uint32_t lastpeek[6][4]; memset(lastpeek, 0xFF, sizeof(lastpeek));
    static const uint32_t fixed[2] = { 0x63D22580u, 0x42425F00u };
    uint32_t last[3][6]; memset(last, 0xFF, sizeof(last));
    DWORD t0 = 0;
    for (;;) {
        Sleep(1);
        if (!vm_base) continue;
        if (!t0) t0 = GetTickCount();
        for (int k = 0; k < 6; k++) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(vm_base + peek[k], &mbi, sizeof(mbi))
                || mbi.State != MEM_COMMIT) continue;
            uint32_t v[4];
            for (int j = 0; j < 4; j++) v[j] = vm_read32(peek[k] + (uint32_t)j * 4u);
            if (memcmp(v, lastpeek[k], sizeof(v)) != 0) {
                fprintf(stderr, "[codec-peek +%lums @%08X] %08X %08X %08X %08X\n",
                        GetTickCount()-t0, peek[k], v[0], v[1], v[2], v[3]);
                fflush(stderr);
                memcpy(lastpeek[k], v, sizeof(v));
            }
        }
        for (int k = 0; k < 3; k++) {
            uint32_t ts = (k < 2) ? fixed[k] : g_yz_spurs_taskset;
            if (!ts || (k == 2 && (ts == fixed[0] || ts == fixed[1]))) continue;
            /* the fixed taskset EAs are allocated by the game mid-boot; reading
             * reserved-but-uncommitted guest memory faults the host */
            { MEMORY_BASIC_INFORMATION mbi;
              if (!VirtualQuery(vm_base + ts, &mbi, sizeof(mbi))
                  || mbi.State != MEM_COMMIT) continue; }
            uint32_t v[6];
            for (int j = 0; j < 6; j++) v[j] = vm_read32(ts + (uint32_t)j * 0x10u);
            if (memcmp(v, last[k], sizeof(v)) != 0) {
                fprintf(stderr, "[ts-poll +%lums ts=%08X] running=%08X ready=%08X pready=%08X "
                        "enabled=%08X signalled=%08X waiting=%08X\n",
                        GetTickCount()-t0, ts, v[0], v[1], v[2], v[3], v[4], v[5]);
                fflush(stderr);
                memcpy(last[k], v, sizeof(v));
            }
        }
    }
}

/* T1-ONLY non-suspending PC/LR sampler (env YZ_T1SAMPLE, 2026-07-04). The
 * sem-4-rendezvous-then-silent-spin finding (scratch/_verify_af.err ~line
 * 7009): t1 issues zero further lv2 syscalls after that point, so the
 * lv2-wait recorder (yz_wait_get) reports it as "running guest code" forever
 * -- we need to know WHERE. A diagnostic must never suspend or perturb the
 * thread it measures, so do NOT suspend t1 or walk
 * its host stack (YZ_L1SNAP's dumps already proved that corrupts exactly
 * this kind of measurement). Instead, read the plain globals dispatch.cpp
 * writes on t1's own indirect-call/trampoline-hop path (g_yz_t1_last_target/
 * _lr/_sample_seq) from a low-rate (2s) timer thread -- no lock, no pause,
 * bounded volume. If g_yz_t1_sample_seq stops incrementing
 * between two reads, t1 has stopped taking indirect calls/trampoline hops
 * entirely (a tight direct-branch loop with no bctr/tail-branch in it);
 * otherwise the printed target/lr pins the poll site directly. */
extern "C" volatile uint32_t g_yz_t1_last_target;
extern "C" volatile uint32_t g_yz_t1_last_lr;
extern "C" volatile long     g_yz_t1_sample_seq;
static DWORD WINAPI yz_t1_sample_thread(LPVOID)
{
    long lseq = -1;
    for (;;) {
        Sleep(2000);
        long seq = g_yz_t1_sample_seq;
        uint32_t tgt = g_yz_t1_last_target;
        uint32_t lr  = g_yz_t1_last_lr;
        const yz_func_entry* ft = yz_func_from_guest(tgt);
        const yz_func_entry* fl = yz_func_from_guest(lr);
        char tgt_sym[48] = "", lr_sym[48] = "";
        if (ft) snprintf(tgt_sym, sizeof tgt_sym, " (func_%08X+0x%X)", ft->addr, tgt - ft->addr);
        if (fl) snprintf(lr_sym,  sizeof lr_sym,  " (func_%08X+0x%X)", fl->addr, lr  - fl->addr);
        fprintf(stderr, "[t1sample] seq=%ld (%s since last read) target=0x%08X%s lr=0x%08X%s\n",
                seq, (seq == lseq) ? "UNCHANGED" : "moved", tgt, tgt_sym, lr, lr_sym);
        fflush(stderr);
        lseq = seq;
    }
    return 0;
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
    fprintf(stderr, "\n[crash] exception 0x%08lX on tid=%u (tramp_idx=%u) at host %p",
            er->ExceptionCode, yz_thread_current_id(), g_yz_tramp_idx, er->ExceptionAddress);
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
    /* Per-thread ground truth: the GLOBAL indirect-target ring is useless here
     * (dozens of CRI poll threads spam it), so for the main thread walk its OWN
     * guest back-chain via g_yz_main_ctx -- on a host stack overflow this names
     * the actual recursion cycle (reads guest mem, cheap on host stack). */
    if (yz_thread_current_id() == 1u && g_yz_main_ctx)
        yz_dump_guest_state(g_yz_main_ctx, "crash-t1");

    /* HOST call stack of the FAULTING thread (newest first). For a host-stack
     * overflow (0xC00000FD) with a SHALLOW guest stack, the recursion is in HOST
     * code -- this names the repeating host frames (the cycle). Resolve lifted
     * frames to func_XXXX; runtime/CRT frames show as rva. */
    {
        void* frames[80];
        USHORT nf = RtlCaptureStackBackTrace(0, 80, frames, NULL);
        uintptr_t mod = (uintptr_t)GetModuleHandleW(NULL);
        fprintf(stderr, "\n[crash] HOST call stack (%u frames, newest first):", nf);
        for (USHORT i = 0; i < nf; i++) {
            const yz_func_entry* fe = yz_func_from_host(frames[i]);
            if (fe)
                fprintf(stderr, "\n    func_%08X +0x%llX", fe->addr,
                        (unsigned long long)((uintptr_t)frames[i] - (uintptr_t)fe->fn));
            else
                fprintf(stderr, "\n    rva 0x%llX", (unsigned long long)((uintptr_t)frames[i] - mod));
        }
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

    /* Boot determinism: the firmware boot is paced by Sleep()-based polls that quantize
     * to the ~15.6ms default Windows timer tick (so the SPU/RSX startup races shift
     * run-to-run), and Windows 11 EcoQoS throttles a BACKGROUNDED process's scheduling
     * (measured: foreground booted the codec task far more reliably than minimized).
     * Pin a 1ms timer, raise priority, and opt out of execution-speed throttling so every
     * boot sees the same scheduling regardless of window focus. */
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    {
        PROCESS_POWER_THROTTLING_STATE pt;
        memset(&pt, 0, sizeof(pt));
        pt.Version     = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;  /* opt out of EcoQoS */
        pt.StateMask   = 0;
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof(pt));
    }

    SymInitialize(GetCurrentProcess(), NULL, TRUE);  /* load yakuza_recomp.pdb */
    SetUnhandledExceptionFilter(yz_crash_handler);

    printf("=== Yakuza: Dead Souls recomp runner ===\n");

    if (vm_init() != 0) {
        fprintf(stderr, "ERROR: vm_init failed\n");
        return 1;
    }
    vm_stack_alloc_init(&g_stacks);

    /* Arm the YZ_WATCH_EA write-watch BEFORE any guest code runs -- the gcm
     * device-object corruptor (0x01622200) races _cellGcmInitBody, which is far
     * too early for the RSX-thread arm to catch. */
    if (const char* we = getenv("YZ_WATCH_EA")) yz_watch_arm((uint32_t)strtoul(we, nullptr, 16));

    /* YZ_WATCH_WR=hexEA[,hexEA...] -- env-driven multi-address write-watch,
     * no rebuild per question. yz_watch_wr_init() itself is a single
     * getenv()+return when the flag is unset (zero perturbation when off),
     * so the call is unconditional here, same as
     * YZ_WATCH_EA's arm above. */
    yz_watch_wr_init();

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

    /* LLE the GAME's own runtime-loaded engine PRX: pxd_shader (OgreZ shader
     * module). The game sys_prx_load_module's it from data/module/ps3/; we
     * decrypted (tools/decrypt_self.py) + relocated it to 0x02200000. Its image
     * holds the TOC (0x02673020) + module_start OPD the sys_prx overrides invoke.
     * Optional (build/boot works without it); only the shader path needs it. */
    load_prx_image("recomp_prx/ogrez_shader_ps3.ppu_image.bin", 0x02200000u);
    (void)&yz_watch_arm;   /* write-watch available for debugging; not armed */

    /* FLAG-PRODUCER HUNT (env YZ_WATCH_FLAG): the post-module stall has t1 polling
     * engine object 0x013618B8 (module-ready flag, +0x18 = 0xFFFFFFFF sentinel). Arm a
     * write-watch on that field NOW -- before the guest runs -- so the VEH names every
     * writer (init + any later) with its reliable trampoline-ring; if it's only written
     * once (to the sentinel) and never again while t1 spins, the producer is missing. */
    if (getenv("YZ_WATCH_FLAG")) yz_watch_arm(0x013618B8u + 0x18u);

    /* Lifted SPU images: register Sony's SPURS kernel (recomp_prx/
     * spurs_kernel2.c) and the system-service workload (spurs_sysservice.c,
     * runs at LS 0xA00) so sys_spu_thread_group_start can run them for real. */
    /* Distinct image ids so the overlapping LS-0xA00 images (system service vs
     * taskset policy) are disambiguated by ctx->image_id (set on DMA dispatch,
     * spu_dma.h). Kernel (LS 0x290..) and gs_task (LS 0x3000) don't overlap, so
     * image 0 (matches any) is fine for them. */
    /* s24: the kernel gets its OWN image id (16) instead of sharing 0 with
     * gs_task — the id-0 collision let wild kernel-era branches resolve into
     * gs_task's lift and mis-attributed every tid-0x2004 death (ledger
     * #34/#49; adoption-trace analysis). Group-start now adopts 16 for every
     * thread (lv2_register.c); spu_lookup refuses wildcard service for
     * kernel contexts (kill-switch YZ_KERN_WILDCARD_OK). */
    spu_begin_image(16); spu_recomp_register();           /* kernel  */
    spu_begin_image(1); spu_recomp_register_sysservice(); /* system service @0xA00 */
    spu_begin_image(2); spu_recomp_register_policy();     /* taskset policy @0xA00 */
    /* cri_audio (image 3) BEFORE gs_task (image 0): both overlap LS 0x3000+, and
     * spu_lookup returns the first match -- registering the codec first lets its
     * image-3 entries win for codec (image 3) lookups over gs_task's image-0
     * wildcard. gs_task is unaffected on its own (image 0) path. */
    spu_begin_image(3); cri_audio_register_functions();   /* CRI SOFDEC/ADX codec @0x3000 (wid 3) */
    spu_begin_image(4); wkl4_register_functions();        /* 4-task worker pool @0x3000 (wid 4, EBOOT img #5 @0x01284200) */
    spu_images_register_extra();                          /* remaining EBOOT task images (ids 5+, generated table) */
    spu_begin_image(12); spu_recomp_register_tsexit();    /* taskset exit-handler overlay @0x10000 (libsre 0x02025500) */
    spu_begin_image(13); spu_recomp_register_jobmod();    /* jobchain policy module @0xA00 (libsre 0x0202A180) */
    spu_begin_image(14); spu_recomp_register_jobbin_a();  /* jobchain bulk-worker job binary (EBOOT 0x01254500, slot base 0x4C00) */
    spu_recomp_register_jobbin_a_e400();                  /*   ...same binary lifted at the other slot base 0xE400 (same image) */
    spu_begin_image(15); spu_recomp_register_jobbin_b();  /* jobchain notify job binary (EBOOT 0x01275A00, slot base 0xE400) */
    spu_recomp_register_jobbin_b_4c00();                  /*   ...same binary lifted at the other slot base 0x4C00 (same image) */
    spu_begin_image(0); spu_recomp_register_gstask();     /* Edge geometry task @0x3000 (image-0 wildcard: LAST) */
    printf("[boot] SPU images registered (kernel + service + policy + %d EBOOT task images)\n",
           SPU_IMAGE_COUNT);

    /* DIAG (1f): function-level spin profiler. YZ_SPU_PROF histograms every
     * SPU tail-call trampoline hop by target LS addr -> pins which lifted
     * SPURS functions the SPU threads spin in (the scheduler loops via
     * trampolines, invisible to spu_indirect_branch). Set before threads run. */
    g_spu_prof_on = getenv("YZ_SPU_PROF") ? 1 : 0;
    g_yz_watch_dlist = getenv("YZ_WATCH_DLIST") ? 1 : 0;
    g_yz_slotstore = getenv("YZ_SLOTSTORE") ? 1 : 0;
    if (g_yz_slotstore) {
        fprintf(stderr, "[slotstore] ARMED: wid4 work-record store watch live\n");
        fflush(stderr);
    }
    /* s28 (ledger #63 / s28_earlystall_diff): t1 host-liveness heartbeat needs
     * a real handle for GetThreadTimes — main() runs ON t1, duplicate here. */
    { extern HANDLE g_yz_t1_handle;
      DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                      &g_yz_t1_handle, 0, FALSE, DUPLICATE_SAME_ACCESS); }

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

    /* Read the real title id (+ title/version) from the game's PARAM.SFO so every
     * title-id-based path (cellGame content/boot/data, cellDiscGame) is correct for
     * ANY title -- not a hardcoded placeholder (the /dev_hdd0/game/BLES00000 bug). */
    cellGame_init_from_paramsfo("gamedata/dev_bdvd/PS3_GAME/PARAM.SFO");

    /* Main PPU thread stack */
    uint32_t stack_base = vm_stack_allocate(&g_stacks, 1024 * 1024);
    if (!stack_base) {
        fprintf(stderr, "ERROR: stack allocation failed\n");
        return 1;
    }

    yz_threads_init(stack_base, 1024 * 1024);

    static ppu_context ctx;   /* zero-initialized */
    ctx.thread_id = 1;        /* main thread; matches s_cur_tid default (lv2 mutex ownership) */
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
    if (getenv("YZ_DEFERWATCH"))
        CreateThread(NULL, 0, yz_deferwatch_thread, NULL, 0, NULL);
    /* s24: lookahead REFUTED on its first boot (DONT_RECHASE #50 — reproduced
     * the June eager-applier wedge: releases without the preceding patch
     * entries hand GET torn content). Explicit opt-in only. */
    if (getenv("YZ_LOOKAHEAD"))
        CreateThread(NULL, 0, yz_lookahead_thread, NULL, 0, NULL);
    /* High-frequency RSX state tracer (TEMP DEBUG): compact GET/PUT/fence/ctr
     * timeline + a full dump at the stall edge. Env-gated so default runs stay
     * clean; on for the deadlock investigation. */
    if (getenv("YZ_TRACE_RSX"))
        CreateThread(NULL, 0, yz_rsx_state_trace, NULL, 0, NULL);
    /* Non-suspending t1 PC/LR sampler (env YZ_T1SAMPLE, 2026-07-04) -- see the
     * comment above yz_t1_sample_thread. Pins the exact poll site for a
     * silent (no-syscall) t1 stall without perturbing the thread. */
    if (getenv("YZ_T1SAMPLE"))
        CreateThread(NULL, 0, yz_t1_sample_thread, NULL, 0, NULL);
    /* RSX FIFO flow-control band-aid -- RETIRED AGAIN (default OFF,
     * 2026-07-02 layer-1 root-cause session; opt back in with YZ_FLOWCTL=1).
     * The race it covered is root-caused: OUR deferred-release applier
     * (import_overrides.cpp) raced Sony's real journal consumer -- gs_task
     * applies the data/CALL patches (plain PUTs, LS pc 0xB60C) and releases
     * stoppers with FENCED 4-byte PUTs (pc 0x5F00) itself; the applier's
     * release-without-patches handed GET unpatched content and wedged t1 at
     * ~+6 s (3/3 applier-on boots, identical site). With BOTH the applier
     * and this lever off, 12/12 boots show zero such wedges (2 slow-but-
     * healthy under compile load; val6 = the pre-existing late audio race,
     * present on default config too). Evidence: scratch/{bad1,cfgA*,cfgB*,
     * val*}.err + the [gs-put]/[jrnl] probes. Delete after quiet sessions. */
    if (getenv("YZ_FLOWCTL"))
        CreateThread(NULL, 0, yz_flip_advance, NULL, 0, NULL);
    /* On-demand all-threads dump (env YZ_DUMP_AT=<seconds>, 2026-07-02 diag):
     * fires yz_dump_all_threads once at +N s regardless of watchdog state --
     * for reading a HEALTHY-but-parked boot (e.g. the post-font.par frontier
     * park at ~+250 s) where the wedge watchdog never triggers. The run is
     * observed at a chosen instant; don't use dump-armed runs for pass/fail
     * rates (must not perturb the thread it measures). */
    if (const char* da = getenv("YZ_DUMP_AT")) {
        static int dump_at_s = atoi(da);
        static int t11fate = getenv("YZ_T11_FATE") ? 1 : 0;
        if (t11fate) fprintf(stderr, "[t11fate] armed (rides YZ_DUMP_AT=%d)\n", dump_at_s);
        if (dump_at_s > 0)
            CreateThread(NULL, 0, [](LPVOID) -> DWORD {
                Sleep((DWORD)dump_at_s * 1000);
                fprintf(stderr, "[dump-at] +%ds on-demand snapshot:\n", dump_at_s);
                yz_dump_all_threads("dump-at");
                /* s29 [t11fate] (probe spec scratch/s29_t11_fate.md): raw
                 * registry + cri_dlg exit-flag + wait-state, all at the same
                 * instant so registry/flag/wait correlate in one snapshot.
                 * Flag EAs = thread arg+0x50 (func_00F00E80's loop-exit
                 * test): t11 arg=0x1661470, t12 arg=0x1661408. */
                if (t11fate) {
                    yz_thread_registry_raw();
                    for (uint32_t tid = 11; tid <= 12; tid++) {
                        uint32_t scn = 0, held = 0; uint64_t a3 = 0, a4 = 0, a5 = 0;
                        if (yz_wait_get(tid, &scn, &a3, &a4, &a5, &held))
                            fprintf(stderr, "[t11fate] t%u BLOCKED sc=%u r3=0x%llX held=%ums\n",
                                    tid, scn, (unsigned long long)a3, (unsigned)held);
                        else
                            fprintf(stderr, "[t11fate] t%u not-in-syscall\n", tid);
                    }
                    fprintf(stderr, "[t11fate] exitflag t11@0x16614C0=0x%08X t12@0x1661458=0x%08X\n",
                            vm_read32(0x16614C0), vm_read32(0x1661458));
                    fflush(stderr);
                }
                return 0;
            }, NULL, 0, NULL);
    }
    if (getenv("YZ_TS_WATCH"))
        CreateThread(NULL, 0, yz_ts_watch, NULL, 0, NULL);
    if (getenv("YZ_PEEK"))
        CreateThread(NULL, 0, yz_peek_thread, NULL, 0, NULL);
    /* s30 staging-decision peek (env YZ_STAGE_DECIDE, spec
     * scratch/s30_staging_decision.md §4 tier 1): every 5 s walk the CRI
     * preload pipeline's state from static roots, reads only, all pointers
     * null-guarded, NO page watches (0x1661xxx is the CRI cs hot page,
     * LESSONS #6c). Discriminates the variant-A/B/C failure stories at the
     * chain death (pre-committed table in the report). */
    if (getenv("YZ_STAGE_DECIDE"))
        CreateThread(NULL, 0, [](LPVOID) -> DWORD {
            fprintf(stderr, "[stagedec] ARMED (YZ_STAGE_DECIDE): 5s CRI pipeline peek\n");
            fflush(stderr);
            /* v2 (s30_staging_decision.md §6): walk the CRI FS DRIVER POOL —
             * 40 slots of 0x4D0 bytes at base+0x868, live when vm[D]==1.
             * The per-live-slot phase/status dump at the death IS the
             * decision-input capture (pre-committed verdicts in §6). */
            for (long tick = 0;; tick++) {
                Sleep(5000);
                uint32_t base = vm_read32(0x135CDFCu);
                int nlive = 0;
                char slots[8][224]; /* capture up to 8 live-slot lines */
                if (base) {
                    for (uint32_t k = 0; k < 40; k++) {
                        uint32_t D = base + 0x868u + k * 0x4D0u;
                        if (vm_read32(D) != 1) continue;
                        if (nlive < 8) {
                            char name[25];
                            for (int w = 0; w < 6; w++) {
                                uint32_t v = vm_read32(D + 0x10u + (uint32_t)w * 4u);
                                name[w*4+0] = (char)(v >> 24); name[w*4+1] = (char)(v >> 16);
                                name[w*4+2] = (char)(v >> 8);  name[w*4+3] = (char)v;
                            }
                            name[24] = 0;
                            for (int j = 0; j < 24; j++)
                                if (name[j] && (name[j] < 32 || name[j] > 126)) name[j] = '.';
                            snprintf(slots[nlive], sizeof slots[0],
                                "[stagedec]   slot%u D=%08X open=%u status=%u mode=%08X tot=%08X cons=%08X take=%08X seq=%08X phase=%u name='%s'\n",
                                k, D, vm_read32(D + 4u), vm_read32(D + 8u),
                                vm_read32(D + 0x414u), vm_read32(D + 0x428u),
                                vm_read32(D + 0x42Cu), vm_read32(D + 0x430u),
                                vm_read32(D + 0x444u), vm_read32(D + 0x448u), name);
                        }
                        nlive++;
                    }
                }
                /* s31: bootflag/exitreq = the two boot-pump exit cells
                 * (vm[0x136786C] bit 0x400 = pump-run flag; vm[0x1367874] =
                 * exit request read by D0140 via 1AB304). BOTH decoded doors
                 * measured 0 calls while the pump exits (s31power1) — these
                 * plain reads catch WHICH cell actually changes at the exit
                 * (the page is too write-hot for a page-guard watch,
                 * s31door1). */
                fprintf(stderr, "[stagedec] tick=%ld nLive=%d w474=%08X stagedSeq=%08X exit=%08X base=%08X pathOps=%08X P2=%08X bootflag=%08X exitreq=%08X\n",
                        tick, nlive,
                        vm_read32(0x1661474u), vm_read32(0x16614B8u), vm_read32(0x16614C0u),
                        base,
                        base ? vm_read32(base + 0x40Cu) : 0xDEAD,
                        base ? vm_read32(base + 0xC8ECu) : 0xDEAD,
                        vm_read32(0x136786Cu), vm_read32(0x1367874u));
                for (int i = 0; i < nlive && i < 8; i++) fputs(slots[i], stderr);
                fflush(stderr);
            }
            return 0;
        }, NULL, 0, NULL);

    g_yz_cur_ctx = &ctx;
    g_yz_main_ctx = &ctx;
    /* Real thread handle for the watchdog's host-stack/spin dumps of t1
     * (was declared but never assigned -- every t1 host-RIP dump silently
     * no-opped; the wedge captures lacked t1 attribution). */
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                    &g_yz_main_hthread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    /* Capture this (main/t1) thread's trampoline-ring instance for the stall dump. */
    g_yz_main_tramp     = g_yz_tramp_ring;
    g_yz_main_tramp_r31 = g_yz_tramp_r31;
    g_yz_main_tramp_lr  = g_yz_tramp_lr;
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
