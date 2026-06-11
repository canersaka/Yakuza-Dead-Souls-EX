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

/* ---------------------------------------------------------------------------
 * Minimal big-endian ELF64 loader (PT_LOAD only)
 * -----------------------------------------------------------------------*/

static uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t be64(const uint8_t* p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

/* PT_TLS template info, forwarded to the guest entry in r8/r9/r10
 * (the CRT passes them through to sys_initialize_tls). */
static uint64_t g_tls_vaddr = 0, g_tls_filesz = 0, g_tls_memsz = 0;

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
    }
    cb_ctx.gpr[3] = a0;
    cb_ctx.gpr[4] = a1;
    cb_ctx.gpr[5] = a2;
    cb_ctx.gpr[6] = a3;
    yz_call_guest_opd(opd_addr, &cb_ctx);
}

/* ---------------------------------------------------------------------------
 * Main
 * -----------------------------------------------------------------------*/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static LONG WINAPI yz_crash_handler(EXCEPTION_POINTERS* ep)
{
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    fprintf(stderr, "\n[crash] exception 0x%08lX at host %p",
            er->ExceptionCode, er->ExceptionAddress);
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
    fprintf(stderr, "\n");
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char** argv)
{
    const char* elf_path = (argc > 1) ? argv[1] : "game/EBOOT.elf";

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
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
    printf("[boot] TLS template: vaddr=0x%08llX filesz=0x%llX memsz=0x%llX\n",
           (unsigned long long)g_tls_vaddr, (unsigned long long)g_tls_filesz,
           (unsigned long long)g_tls_memsz);

    printf("[boot] calling func_%08X (sp=0x%08llX toc=0x%08llX)\n",
           entry_code, (unsigned long long)ctx.gpr[1],
           (unsigned long long)ctx.gpr[2]);

    entry_fn(&ctx);
    yz_drain_trampolines(&ctx);

    printf("[boot] entry returned, r3=0x%llX\n", (unsigned long long)ctx.gpr[3]);
    vm_shutdown();
    return (int)(uint32_t)ctx.gpr[3];
}
