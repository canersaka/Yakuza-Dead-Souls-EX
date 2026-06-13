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

    /* Guest state + PPC64 back-chain walk: names the real guest call chain
     * (the host return-address scan below misses direct bl callers). */
    if (g_yz_cur_ctx) {
        const ppu_context* gc = g_yz_cur_ctx;
        fprintf(stderr,
                "\n[crash] guest cia=0x%08X lr=0x%08X ctr=0x%08X r1=0x%08X",
                (uint32_t)gc->cia, (uint32_t)gc->lr, (uint32_t)gc->ctr,
                (uint32_t)gc->gpr[1]);
        fprintf(stderr, "\n[crash] guest gpr:");
        for (int r = 0; r < 32; r++) {
            if (r % 8 == 0) fprintf(stderr, "\n    r%-2d:", r);
            fprintf(stderr, " %016llX", (unsigned long long)gc->gpr[r]);
        }
        /* Executable guest range (EBOOT first PT_LOAD, R+X): a slot holding a
         * value here is a real code pointer / saved return address. The ELFv1
         * +16 LR slot proved unreliable for the lifter's frames, so scan each
         * frame for code pointers instead. */
        const uint32_t EXE_LO = 0x00010000u, EXE_HI = 0x01310768u;
        fprintf(stderr, "\n[crash] guest back-chain (per-frame code pointers):");
        uint32_t sp = (uint32_t)gc->gpr[1];
        int total = 0;
        for (int depth = 0; depth < 1200 && sp >= 0x10000u; depth++) {
            if (vm_base && IsBadReadPtr(vm_base + sp, 8)) break;
            uint32_t back = (uint32_t)vm_read64(sp);
            if (back <= sp || back < 0x10000u) break;   /* must climb upward */
            total++;
            /* Print details for the first 8 frames only (the rest are the
             * repeating recursion body); just count the depth otherwise. */
            if (depth < 8) {
                fprintf(stderr, "\n    #%-2d sp=0x%08X size=0x%X:", depth, sp,
                        back - sp);
                uint32_t lim = back - sp; if (lim > 0xB0) lim = 0xB0;
                for (uint32_t off = 0; off + 8 <= lim; off += 8) {
                    if (vm_base && IsBadReadPtr(vm_base + sp + off, 8)) break;
                    uint32_t v = (uint32_t)vm_read64(sp + off);
                    if (v >= EXE_LO && v < EXE_HI) {
                        fprintf(stderr, "\n        +0x%02X -> 0x%08X", off, v);
                        if (const yz_func_entry* fe = yz_func_from_guest(v))
                            fprintf(stderr, " (func_%08X +0x%X)", fe->addr,
                                    v - fe->addr);
                    }
                }
            }
            sp = back;
        }
        fprintf(stderr, "\n[crash] back-chain total frames: %d", total);
    }
    /* Return-address candidates from the faulting thread's stack: values
     * pointing into our image are callers (the faulting accessor is a leaf,
     * so the direct caller is at/near [rsp]). Resolved via the func table. */
    if (ep->ContextRecord) {
        uintptr_t mod = (uintptr_t)GetModuleHandleW(NULL);
        const uint64_t* sp = (const uint64_t*)ep->ContextRecord->Rsp;
        fprintf(stderr, "\n[crash] stack return candidates:");
        int shown = 0;
        for (int i = 0; i < 64 && shown < 6; i++) {
            uint64_t v = 0;
            if (IsBadReadPtr(sp + i, 8)) break;
            v = sp[i];
            if (v < mod || v > mod + 0x40000000ull) continue;
            fprintf(stderr, "\n    rva 0x%llX",
                    (unsigned long long)(v - mod));
            if (const yz_func_entry* fe = yz_func_from_host((const void*)v))
                fprintf(stderr, " (func_%08X +0x%llX)", fe->addr,
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

    /* Lifted SPU images: register Sony's SPURS kernel (recomp_prx/
     * spurs_kernel2.c) and the system-service workload (spurs_sysservice.c,
     * runs at LS 0xA00) so sys_spu_thread_group_start can run them for real. */
    spu_recomp_register();
    spu_recomp_register_sysservice();
    printf("[boot] SPU images registered (SPURS kernel2 + system service)\n");

    /* PS3 e_entry points at an OPD descriptor: word0 = code, word1 = TOC */
    uint32_t entry_code = vm_read32(e_entry);
    uint32_t entry_toc  = vm_read32(e_entry + 4);
    printf("[boot] entry OPD: code=0x%08X toc=0x%08X\n", entry_code, entry_toc);

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

    g_yz_cur_ctx = &ctx;
    entry_fn(&ctx);
    yz_drain_trampolines(&ctx);

    printf("[boot] entry returned, r3=0x%llX\n", (unsigned long long)ctx.gpr[3]);
    vm_shutdown();
    return (int)(uint32_t)ctx.gpr[3];
}
