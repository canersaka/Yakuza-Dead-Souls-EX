/*
 * Indirect-call dispatcher for the recompiled Yakuza: Dead Souls EBOOT.
 *
 * The generated code (ppu_recomp.c) calls ps3_indirect_call(ctx) for
 * bctr/bctrl and expects:
 *   - the guest target address in ctx->ctr,
 *   - the dispatcher to resolve it to a host function (with OPD fallback:
 *     if ctr points at an OPD descriptor, word0 = code addr, word1 = TOC),
 *   - cross-fragment tail branches to be flattened via the thread-local
 *     g_trampoline_fn + the DRAIN_TRAMPOLINE loops the lifter emits.
 *
 * We resolve the target and set g_trampoline_fn instead of calling directly,
 * so long bctr tail-chains don't grow the host stack; the drain loop at the
 * call site (or in main) executes it iteratively.
 */

#include "ppu_recomp.h"
#include "yakuza_runner.h"

#include <cstdio>
#include <cstdlib>

/* TLS trampoline slot -- the generated code declares this extern and both
 * sets it (direct tail branches) and drains it. */
extern "C" __declspec(thread) void (*g_trampoline_fn)(void*) = nullptr;

static bool addr_readable(uint32_t a)
{
    /* main memory (incl. loaded ELF) or stack region */
    return (a >= 0x00010000u && a < 0x10000000u) ||
           (a >= 0xD0000000u && a < 0xE0000000u);
}

extern "C" yz_ppu_fn yz_lookup_func(uint32_t guest_addr)
{
    unsigned lo = 0, hi = g_yz_func_count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        uint32_t a = g_yz_func_table[mid].addr;
        if (a == guest_addr) return g_yz_func_table[mid].fn;
        if (a < guest_addr)  lo = mid + 1;
        else                 hi = mid;
    }
    return nullptr;
}

extern "C" void yz_drain_trampolines(ppu_context* ctx)
{
    while (g_trampoline_fn) {
        void (*tf)(void*) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        tf((void*)ctx);
    }
}

/* Imported firmware functions: fake-key range -> host HLE bridge index. */
static yz_ppu_fn import_bridge_for(uint32_t code)
{
    if (code >= YZ_IMPORT_FAKE_BASE &&
        code <  YZ_IMPORT_FAKE_BASE + g_yz_import_count * 4)
        return g_yz_import_bridges[(code - YZ_IMPORT_FAKE_BASE) / 4];
    if (code == YZ_GCM_CB_FAKE_KEY)   /* runner-owned gcm fifo callback */
        return yz_gcm_fifo_callback;
    return nullptr;
}

/* Ring buffer of recent indirect-call targets, dumped by the crash handler. */
uint32_t g_yz_last_targets[16];
unsigned g_yz_last_idx;

extern "C" void ps3_indirect_call(ppu_context* ctx)
{
    uint32_t target = (uint32_t)ctx->ctr;
    g_yz_last_targets[g_yz_last_idx++ & 15] = target;

    /* Bridges complete entirely on the host, so call directly. */
    if (yz_ppu_fn bridge = import_bridge_for(target)) {
        /* TEMP DEBUG (SPURS bring-up): trace every import call made by the
         * LLE firmware module (indices >= g_yz_lle_import_first) with args
         * and result. Strip once SPURS init survives. */
        unsigned idx = (target - YZ_IMPORT_FAKE_BASE) / 4;
        int trace = target != YZ_GCM_CB_FAKE_KEY &&
                    idx >= g_yz_lle_import_first && idx < g_yz_import_count;
        if (trace)
            fprintf(stderr, "[lle-call t%u] %s(r3=0x%llX r4=0x%llX r5=0x%llX r6=0x%llX)\n",
                    yz_thread_current_id(), g_yz_import_names[idx],
                    (unsigned long long)ctx->gpr[3], (unsigned long long)ctx->gpr[4],
                    (unsigned long long)ctx->gpr[5], (unsigned long long)ctx->gpr[6]);
        bridge(ctx);
        if (trace) {
            fprintf(stderr, "[lle-call t%u]   -> 0x%llX\n",
                    yz_thread_current_id(), (unsigned long long)ctx->gpr[3]);
            fflush(stderr);
        }
        return;
    }

    yz_ppu_fn fn = yz_lookup_func(target);
    if (!fn && addr_readable(target)) {
        /* OPD fallback: ctr may hold a descriptor address rather than code.
         * The descriptor's code word may itself be an import-bridge key
         * (our synthetic OPDs, or game-held pointers to import slots). */
        uint32_t code = vm_read32(target);
        uint32_t toc  = vm_read32(target + 4);
        if (yz_ppu_fn bridge = import_bridge_for(code)) {
            bridge(ctx);
            return;
        }
        fn = yz_lookup_func(code);
        if (fn) ctx->gpr[2] = toc;
    }

    if (!fn) {
        fprintf(stderr,
                "[dispatch] no host function for guest target 0x%08X "
                "(lr=0x%08llX r2=0x%08llX r3=0x%08llX)\n",
                target,
                (unsigned long long)ctx->lr,
                (unsigned long long)ctx->gpr[2],
                (unsigned long long)ctx->gpr[3]);
        exit(2);
    }

    g_trampoline_fn = (void (*)(void*))fn;
}

extern "C" void yz_call_guest_opd(uint32_t opd_addr, ppu_context* ctx)
{
    if (!addr_readable(opd_addr)) {
        fprintf(stderr, "[dispatch] guest callback OPD 0x%08X out of range\n", opd_addr);
        return;
    }
    uint32_t code = vm_read32(opd_addr);
    uint32_t toc  = vm_read32(opd_addr + 4);
    yz_ppu_fn fn = yz_lookup_func(code);
    if (!fn) {
        fprintf(stderr, "[dispatch] guest callback code 0x%08X (opd 0x%08X) not lifted\n",
                code, opd_addr);
        return;
    }
    uint64_t saved_toc = ctx->gpr[2];
    ctx->gpr[2] = toc;
    fn(ctx);
    yz_drain_trampolines(ctx);
    ctx->gpr[2] = saved_toc;
}
