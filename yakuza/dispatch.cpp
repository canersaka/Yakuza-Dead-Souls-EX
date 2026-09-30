/*
 * Indirect calls and trampolines for the lifted Yakuza: Dead Souls EBOOT.
 *
 * The generated code calls ps3_indirect_call(ctx) for bctr/bctrl with the
 * guest target in ctx->ctr.  The dispatcher resolves the target to a lifted
 * function (or, when ctr holds an OPD, to its code and TOC), and hands it to
 * the thread-local g_trampoline_fn instead of calling it, so long bctr
 * tail chains do not grow the host stack.  The DRAIN_TRAMPOLINE loops the
 * lifter emits (and yz_drain_trampolines) run the chain iteratively.
 *
 * A few game functions get host behaviour at this boundary: the mesh loader
 * waits for its source blob, the presentation clock is told when the first
 * authored character pose is ready, and the GCM user-command callback runs to
 * completion with begin/end notifications.
 */

#include "ppu_recomp.h"
#include "yakuza_runner.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <intrin.h>
#include <utility>
#include <vector>

/* kernel32 (this file avoids windows.h). */
extern "C" __declspec(dllimport) void __stdcall Sleep(unsigned long ms);

/* TLS trampoline slot -- the generated code declares this extern and both
 * sets it (direct tail branches) and drains it. */
extern "C" __declspec(thread) void (*g_trampoline_fn)(void*) = nullptr;

extern "C" uint8_t* vm_base;
extern "C" volatile long g_yz_a010_root_active;
extern "C" volatile long g_yz_a010_animation_ready;
/* The GCM user-command callback reports when it starts and finishes, so the
 * FIFO consumer can tell a queued event from one lost before dispatch. */
extern "C" long yz_ucmd_handler_begin(long cause);
extern "C" void yz_ucmd_handler_end(long cause, long epoch);

/* The game module's TOC (set from the entry OPD in main).  Used to repair a
 * lost TOC when a game function is reached with the wrong r2. */
extern "C" uint32_t g_yz_game_toc = 0;

extern "C" void yz_tramp_guard(void* tf, void* ctxv);

static bool addr_readable(uint32_t a)
{
    /* Guest main-memory allocations extend well above the loaded ELF (scene
     * objects live around 0x65xxxxxx), plus the D0 stack arena. */
    return a >= 0x00010000u && a < 0xE0000000u;
}

/* The presentation clock (presentation_clock_hle.cpp) waits for the first
 * authored a010 character pose before it treats a stalled audio position as
 * the scene's silent tail.  A controller's temporary pose is parked at
 * Y=10000; the real one has a finite, matching matrix and palette position. */
static bool yz_a010_character_animation_ready(uint32_t object)
{
    if (!addr_readable(object + 0x2Cu) ||
        vm_read32(object) != 0x011D3F78u)
        return false;

    const uint32_t model = vm_read32(object + 0x2Cu);
    if (!addr_readable(model + 0x218u))
        return false;

    const uint32_t count = vm_read32(model + 0x210u);
    const uint32_t matrices = vm_read32(model + 0x214u);
    const uint32_t palette = vm_read32(model + 0x218u);
    if (count == 0u || count > 512u ||
        !addr_readable(matrices + 0x34u) ||
        !addr_readable(palette + 0x1Cu))
        return false;

    uint32_t matrix_y_bits = vm_read32(matrices + 0x34u);
    uint32_t palette_y_bits = vm_read32(palette + 0x1Cu);
    float matrix_y = 0.0f;
    float palette_y = 0.0f;
    memcpy(&matrix_y, &matrix_y_bits, sizeof(matrix_y));
    memcpy(&palette_y, &palette_y_bits, sizeof(palette_y));
    return std::isfinite(matrix_y) && std::isfinite(palette_y) &&
           std::fabs(matrix_y) < 1000.0f &&
           std::fabs(palette_y) < 1000.0f &&
           std::fabs(matrix_y - palette_y) < 64.0f;
}

/* The mesh loader (func_0036B478, vtable slot +0xC of the loader class) can
 * start while its GSGM source blob is still being staged.  Give the staging
 * producer up to about 200 ms to finish before building. */
static void mesh_build_entry(ppu_context* ctx)
{
    const uint32_t self = (uint32_t)ctx->gpr[3];
    if (addr_readable(self + 0x140u)) {
        const uint32_t blob = vm_read32(self + 0x11Cu);
        uint32_t magic = blob && addr_readable(blob + 4u) ? vm_read32(blob) : 0u;
        if (blob != 0 && magic != 0x4753474Du) {       /* 'GSGM' */
            for (int spin = 0; spin < 200; spin++) {
                Sleep(1);
                magic = vm_read32(blob);
                if (magic == 0x4753474Du) break;
            }
        }
    }
    func_0036B478(ctx);
}

extern "C" yz_ppu_fn yz_lookup_func(uint32_t guest_addr)
{
    if (guest_addr == 0x0036B478u) return mesh_build_entry;
    /* Valid guest code is 4-aligned inside the game module (< 0x02000000) or
     * the lifted shader module window (0x02000000-0x03000000).  The generated
     * table also holds lifter artifacts outside those windows (entries near 0
     * and at 0xFExxxxxx, which shadows the import key range); a corrupted
     * target must not resolve to one of them, so anything else is reported as
     * unknown with the true target. */
    if ((guest_addr & 3u) || guest_addr < 0x10000u ||
        guest_addr >= 0x03000000u) {
        static long rejected = 0;
        long n = _InterlockedIncrement(&rejected);
        if (n <= 8)
            fprintf(stderr,
                    "[dispatch] rejected out-of-range guest target 0x%08X "
                    "(#%ld; not game/PRX code)\n", guest_addr, n);
        return nullptr;
    }
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
        yz_tramp_guard((void*)tf, (void*)ctx);
        tf((void*)ctx);
    }
}

/* Imported system-library functions: fake-key range -> host HLE bridge index. */
static yz_ppu_fn import_bridge_for(uint32_t code)
{
    if (code >= YZ_IMPORT_FAKE_BASE &&
        code <  YZ_IMPORT_FAKE_BASE + g_yz_import_count * 4)
        return g_yz_import_bridges[(code - YZ_IMPORT_FAKE_BASE) / 4];
    if (code == YZ_GCM_CB_FAKE_KEY)   /* runner-owned gcm fifo callback */
        return yz_gcm_fifo_callback;
    return nullptr;
}


/* Guest address of the lifted function whose host entry point is `fn`, 0 if
 * none (the lowest address when a host function has several entries). */
static uint32_t guest_of_host(void* fn)
{
    static const std::vector<std::pair<void*, uint32_t>> index = [] {
        std::vector<std::pair<void*, uint32_t>> v;
        v.reserve(g_yz_func_count);
        for (unsigned i = 0; i < g_yz_func_count; i++)
            v.emplace_back((void*)g_yz_func_table[i].fn, g_yz_func_table[i].addr);
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
            return a.first != b.first ? std::less<void*>()(a.first, b.first)
                                      : a.second < b.second;
        });
        return v;
    }();
    const auto it = std::lower_bound(
        index.begin(), index.end(), fn,
        [](const std::pair<void*, uint32_t>& e, void* key) {
            return std::less<void*>()(e.first, key);
        });
    return it != index.end() && it->first == fn ? it->second : 0u;
}

/* Called before every trampoline hop (the inline DRAIN_TRAMPOLINE loops and
 * yz_drain_trampolines).  A gcm callback such as the flip or vblank handler
 * can be entered by its bare code address rather than its OPD, so the OPD's
 * TOC word is never loaded; the game module has one TOC, so restore it for
 * any game-range target. */
extern "C" void yz_tramp_guard(void* tf, void* ctxv)
{
    ppu_context* ctx = (ppu_context*)ctxv;
    if (!g_yz_game_toc || ctx->gpr[2] == g_yz_game_toc) return;
    const uint32_t g = guest_of_host(tf);
    if (!g || g >= 0x02000000u) return;
    ctx->gpr[2] = g_yz_game_toc;
}

extern "C" void ps3_indirect_call(ppu_context* ctx)
{
    uint32_t target = (uint32_t)ctx->ctr;

    /* First authored a010 character pose (see yz_a010_character_animation_ready). */
    if (g_yz_a010_root_active != 0 && target == 0x00014848u &&
        g_yz_a010_animation_ready == 0 &&
        yz_a010_character_animation_ready((uint32_t)ctx->gpr[3]))
        _InterlockedCompareExchange(&g_yz_a010_animation_ready, 1, 0);

    /* Bridges complete entirely on the host, so call directly. */
    if (yz_ppu_fn bridge = import_bridge_for(target)) {
        bridge(ctx);
        return;
    }

    uint32_t resolved_code = target;
    yz_ppu_fn fn = yz_lookup_func(target);
    /* A gcm callback called by its bare code address keeps the caller's TOC;
     * the game module has exactly one, so force it for any game target. */
    if (fn && target < 0x02000000u && g_yz_game_toc && ctx->gpr[2] != g_yz_game_toc)
        ctx->gpr[2] = g_yz_game_toc;
    if (!fn && addr_readable(target)) {
        /* OPD fallback: ctr may hold a descriptor address rather than code.
         * The descriptor's code word may itself be an import-bridge key
         * (our synthetic OPDs, or game-held pointers to import slots). */
        uint32_t code = vm_read32(target);
        uint32_t toc  = vm_read32(target + 4);
        resolved_code = code;
        if (yz_ppu_fn bridge = import_bridge_for(code)) {
            bridge(ctx);
            return;
        }
        fn = yz_lookup_func(code);
        if (fn) {
            ctx->gpr[2] = toc;
            /* A game-range function reached through an OPD whose TOC word is
             * 0 would fault on its first TOC load. */
            if (code < 0x02000000u && g_yz_game_toc && ctx->gpr[2] != g_yz_game_toc)
                ctx->gpr[2] = g_yz_game_toc;
        }
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

    if (resolved_code == 0x00E7DB10u) {
        /* The GCM user-command callback signals the SPU workload before it
         * finishes staging that signal's work record.  Run it to completion
         * here so the DMA side can wait for a real publication
         * acknowledgement instead of a spin count. */
        const long cause = (long)(uint32_t)ctx->gpr[3];
        const long epoch = yz_ucmd_handler_begin(cause);
        void (*saved_trampoline)(void*) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        fn(ctx);
        yz_drain_trampolines(ctx);
        yz_ucmd_handler_end(cause, epoch);
        g_trampoline_fn = saved_trampoline;
        return;
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
    if (code == 0x00E7DB10u) {
        const long cause = (long)(uint32_t)ctx->gpr[3];
        const long epoch = yz_ucmd_handler_begin(cause);
        void (*saved_trampoline)(void*) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        fn(ctx);
        yz_drain_trampolines(ctx);
        yz_ucmd_handler_end(cause, epoch);
        g_trampoline_fn = saved_trampoline;
    } else {
        fn(ctx);
        yz_drain_trampolines(ctx);
    }
    ctx->gpr[2] = saved_toc;
}
