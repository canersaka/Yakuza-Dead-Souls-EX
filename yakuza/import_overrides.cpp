/*
 * Hand-written import bridges that need ppu_context access (the generic
 * generated bridges only marshal gpr[3..10] -> args -> gpr[3]).
 *
 * Semantics derived from the CELL OS ABI as documented by RPCS3's behavior
 * (Emu/Cell/Modules/sys_ppu_thread_.cpp etc.) -- reimplemented, not copied.
 *
 * Guest scratch layout used by the runner (all inside the documented
 * "CRT malloc heap" window 0x00A00000-0x10000000, above Yakuza's last ELF
 * segment which ends ~0x01730000):
 *   0x0D000000 - 0x0FE00000   _sys_heap bump allocator (~46 MB)
 *   0x0FE00000 - 0x0FF00000   main-thread TLS block
 *   0x0FF00000 - ...          synthetic import OPDs (yakuza_runner.h)
 */

#include "ppu_recomp.h"
#include "yakuza_runner.h"
#include "edge_journal_hle.h"
#include "rsx_wait_classifier.h"
#include "rsx_nr_intercept.h"
#include "rsx_nr_backend.h"
#include "rsx_nr_producer_contract.h"
#include "native_gcm_vertical.h"

#include "ps3emu/error_codes.h"
#include "ps3emu/yz_fifo_publication.h"
#include "ps3emu/yz_fe0_timeline.h"
#include "ps3emu/yz_wkl4_cycle.h"
#include "ps3emu/yz_frontier_trace.h"
#include "rsx_null_backend.h"   /* pulls rsx_commands.h: rsx_state, processor */
#include "rsx_live_draw.h"      /* Track B: live NV4097 -> D3D12 draw engine */
#include "movie_ffmpeg.h"       /* host FFmpeg movie decode (CRI Sofdec .sfd)   */
#include "../libs/audio/cellAudio.h"
#include "../libs/filesystem/cellFs.h"
#include "../libs/input/cellPad.h"
#include "../libs/system/cellSaveData.h"
#include "../libs/spurs/cellSpurs.h"

#include <cstdio>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Staged native-render integration: a passive, fixed-memory shadow census and
 * one optional typed flip action at the already-serialized FIFO-consumer
 * boundary.  All queue/head/event plumbing continues through yz_rsx_method.
 * YZ_NR_INTERCEPT is read exactly once before the RSX consumer starts.  When
 * it is unset/empty/0, the hot path is one false branch and performs no
 * initialization, allocation, timing, or output. */
static rsx_nr_intercept g_yz_nr_shadow;
static rsx_nr_ring g_yz_nr_shadow_ring;
static rsx_nr_tokens g_yz_nr_shadow_tokens;
static rsx_nr_backend g_yz_nr_backend;
static rsx_nr_slot g_yz_nr_shadow_slots[128];
static uint32_t g_yz_nr_shadow_side[16384];
static volatile LONG g_yz_nr_shadow_enabled;
static int g_yz_nr_shadow_init_attempted;
static unsigned long long g_yz_nr_live_flip_owned;
static unsigned long long g_yz_nr_live_flip_fallback;
static unsigned long long g_yz_nr_live_clear_owned;
static unsigned long long g_yz_nr_live_clear_fallback;
static unsigned long long g_yz_nr_live_draw_owned;
static unsigned long long g_yz_nr_live_draw_fallback;
static unsigned long long g_yz_nr_live_faults;

static int yz_nr_live_present(void*, uint32_t buffer_id)
{
    rsx_live_draw_native_present(buffer_id);
    return 0;
}

static int yz_nr_live_clear(void*, const rsx_nir_pipeline*,
                            const rsx_nir_clear* clear)
{
    rsx_live_draw_native_clear(clear->mask);
    return 0;
}

static int yz_nr_live_draw(void*, const rsx_nir_pipeline*,
                           const uint32_t*, uint32_t,
                           const rsx_nir_draw*, const uint32_t*)
{
    rsx_live_draw_native_end();
    return 0;
}

static void yz_nr_shadow_init(void)
{
    if (g_yz_nr_shadow_init_attempted)
        return;
    g_yz_nr_shadow_init_attempted = 1;
    const uint32_t families = rsx_nr_parse_families(getenv("YZ_NR_INTERCEPT"));
    if (!families)
        return;
    rsx_nr_tokens_init(&g_yz_nr_shadow_tokens);
    if (rsx_nr_ring_init_fixed(&g_yz_nr_shadow_ring,
                               g_yz_nr_shadow_slots,
                               (uint32_t)(sizeof(g_yz_nr_shadow_slots) /
                                          sizeof(g_yz_nr_shadow_slots[0])),
                               g_yz_nr_shadow_side,
                               (uint32_t)(sizeof(g_yz_nr_shadow_side) /
                                          sizeof(g_yz_nr_shadow_side[0]))) != 0)
        return;
    rsx_nr_intercept_init(&g_yz_nr_shadow, &g_yz_nr_shadow_ring,
                          &g_yz_nr_shadow_tokens, families, 1);
    rsx_nr_exec_ops ops = {};
    ops.present = yz_nr_live_present;
    ops.clear = yz_nr_live_clear;
    ops.draw = yz_nr_live_draw;
    rsx_nr_backend_init(&g_yz_nr_backend, &g_yz_nr_shadow_ring,
                        &g_yz_nr_shadow_tokens, &ops);
    InterlockedExchange(&g_yz_nr_shadow_enabled, 1);
}

/* Called under g_rsx_fifo_lock from yz_rsx_method.  The FIFO has already
 * serialized every earlier method, so synchronous backend drain preserves
 * exact command order without a second consumer thread. */
static int yz_nr_try_live_flip(uint32_t buffer_id)
{
    if (!g_yz_nr_shadow_enabled ||
        !rsx_nr_family_enabled(&g_yz_nr_shadow, RSX_NR_FAM_FLIP))
        return 0;
    if (!rsx_nr_try_flip(&g_yz_nr_shadow, buffer_id, 0, 0, 0)) {
        g_yz_nr_live_flip_fallback++;
        return 0;
    }
    g_yz_nr_live_flip_owned++;
    rsx_nr_backend_run(&g_yz_nr_backend, 0);
    if (rsx_nr_ring_depth(&g_yz_nr_shadow_ring) != 0 ||
        g_yz_nr_backend.stats.exec_errors != 0)
        g_yz_nr_live_faults++;
    return 1;
}

static int yz_nr_try_live_clear(uint32_t mask)
{
    if (!g_yz_nr_shadow_enabled ||
        !rsx_nr_family_enabled(&g_yz_nr_shadow, RSX_NR_FAM_CLEAR))
        return 0;
    const uint32_t color = rsx_dsp_clear_color(&g_yz_nr_shadow.shadow.rsx);
    const uint32_t zstencil =
        rsx_dsp_clear_zstencil(&g_yz_nr_shadow.shadow.rsx);
    if (!rsx_nr_try_clear(&g_yz_nr_shadow, mask, color,
                          zstencil >> 8, zstencil & 0xFFu)) {
        g_yz_nr_live_clear_fallback++;
        return 0;
    }
    g_yz_nr_live_clear_owned++;
    rsx_nr_backend_run(&g_yz_nr_backend, 0);
    if (rsx_nr_ring_depth(&g_yz_nr_shadow_ring) != 0 ||
        g_yz_nr_backend.stats.exec_errors != 0)
        g_yz_nr_live_faults++;
    return 1;
}

static int yz_nr_try_live_draw(void)
{
    if (!g_yz_nr_shadow_enabled ||
        !rsx_nr_family_enabled(&g_yz_nr_shadow, RSX_NR_FAM_DRAW))
        return 0;
    rsx_nir_adapter* shadow = &g_yz_nr_shadow.shadow;
    if (!shadow->batch_count || shadow->draw_mixed) {
        rsx_nr_note_fallback(&g_yz_nr_shadow, RSX_NR_FAM_DRAW,
                             RSX_NR_FB_UNSUPPORTED);
        g_yz_nr_live_draw_fallback++;
        return 0;
    }
    if (!rsx_nr_try_draw(&g_yz_nr_shadow,
                         shadow->rsx.current_primitive,
                         shadow->draw_indexed,
                         shadow->batches, shadow->batch_count)) {
        g_yz_nr_live_draw_fallback++;
        return 0;
    }
    g_yz_nr_live_draw_owned++;
    rsx_nr_backend_run(&g_yz_nr_backend, 0);
    if (rsx_nr_ring_depth(&g_yz_nr_shadow_ring) != 0 ||
        g_yz_nr_backend.stats.exec_errors != 0)
        g_yz_nr_live_faults++;
    return 1;
}

/* A refused typed flip runs through the unchanged FIFO renderer first.  At
 * this consumer-side boundary that means its fallback episode is already
 * drained, so publish the token and consume only the aggregate marker. */
static void yz_nr_live_flip_fallback_complete(void)
{
    rsx_nr_intercept_fifo_drained(&g_yz_nr_shadow,
                                  g_yz_nr_shadow.drain_seq);
    rsx_nr_backend_run(&g_yz_nr_backend, 0);
    if (rsx_nr_ring_depth(&g_yz_nr_shadow_ring) != 0 ||
        g_yz_nr_backend.stats.exec_errors != 0)
        g_yz_nr_live_faults++;
}

static void yz_nr_shadow_shutdown(void)
{
    if (!InterlockedExchange(&g_yz_nr_shadow_enabled, 0))
        return;
    rsx_nr_backend_run(&g_yz_nr_backend, 0);
    char line[1024];
    rsx_nr_shadow_census_format(&g_yz_nr_shadow, line, sizeof(line));
    fprintf(stderr, "[%s]\n", line);
    if (rsx_nr_family_enabled(&g_yz_nr_shadow, RSX_NR_FAM_FLIP)) {
        fprintf(stderr,
                "[nr-live: family=flip owned=%llu fallback=%llu "
                "present=%llu errors=%llu faults=%llu depth=%u rejects=%lld]\n",
                g_yz_nr_live_flip_owned, g_yz_nr_live_flip_fallback,
                g_yz_nr_backend.stats.executed[RSX_NIR_OP_PRESENT],
                g_yz_nr_backend.stats.exec_errors, g_yz_nr_live_faults,
                rsx_nr_ring_depth(&g_yz_nr_shadow_ring),
                g_yz_nr_shadow_ring.rejects);
    }
    if (rsx_nr_family_enabled(&g_yz_nr_shadow, RSX_NR_FAM_CLEAR)) {
        fprintf(stderr,
                "[nr-live: family=clear owned=%llu fallback=%llu "
                "clear=%llu errors=%llu faults=%llu depth=%u rejects=%lld]\n",
                g_yz_nr_live_clear_owned, g_yz_nr_live_clear_fallback,
                g_yz_nr_backend.stats.executed[RSX_NIR_OP_CLEAR],
                g_yz_nr_backend.stats.exec_errors, g_yz_nr_live_faults,
                rsx_nr_ring_depth(&g_yz_nr_shadow_ring),
                g_yz_nr_shadow_ring.rejects);
    }
    if (rsx_nr_family_enabled(&g_yz_nr_shadow, RSX_NR_FAM_DRAW)) {
        fprintf(stderr,
                "[nr-live: family=draw owned=%llu fallback=%llu "
                "draw=%llu errors=%llu faults=%llu depth=%u rejects=%lld]\n",
                g_yz_nr_live_draw_owned, g_yz_nr_live_draw_fallback,
                g_yz_nr_backend.stats.executed[RSX_NIR_OP_DRAW],
                g_yz_nr_backend.stats.exec_errors, g_yz_nr_live_faults,
                rsx_nr_ring_depth(&g_yz_nr_shadow_ring),
                g_yz_nr_shadow_ring.rejects);
    }
    fflush(stderr);
    rsx_nr_ring_destroy(&g_yz_nr_shadow_ring);
}

#if defined(YZ_PERF_PROFILE)
extern "C" void spu_perf_dump(void);
#endif

extern "C" uint8_t* vm_base;
extern "C" uint32_t g_yz_game_toc;
extern "C" volatile uint32_t g_yz_jrnl_cur_ea;
extern "C" uint32_t yz_guest_addr_from_host(const void* rip);
extern "C" void yz_w2life_dump(const char*);   /* s31 W2LIFE probe (spu_channels.c) */
extern "C" void yz_fltrec_dump(const char*);   /* s41 flight recorder (runtime/spu/spu_fltrec.c) */
extern "C" int  yz_bdrain_fire_ea(uint32_t parked_ea, uint32_t io_off);  /* s42 park-time boundary drain (runtime/spu/spu_channels.c) */
extern "C" void yz_frontier_edge_dump(uint32_t parked_ea, uint32_t get, uint32_t put);
extern "C" void yz_fltrec_dump(const char* reason);                      /* s42 broken-phase dump rides the drain fire */
/* s40b v2: the GPU-parked stopper EA, published by the FIFO park tracker below for
 * the SPU-side targeted unstick (gs_task.c YZ_QROT_UNSTICK). 0 = not parked >2s. */
extern "C" {
uint32_t g_yz_parked_pub_ea = 0;
/* YZ_A010_ROOT: authoritative lifetime gate shared with the SPU DMA trace.
 * Set when the orphanage AUTH package opens and cleared when the following
 * a020 SFD opens.  LONG keeps Interlocked reads/writes well-defined. */
volatile LONG g_yz_a010_root_active = 0;
/* Focused release-word tracing needs the same authoritative a010 lifetime
 * boundary without enabling the broad, expensive root diagnostics. */
volatile LONG g_yz_a010_release_scene_active = 0;
/* Set after an authored character pose has reached its live model palette.
 * The repaired AUTH clock waits for this publication instead of outrunning
 * the asynchronous animation loads with every actor still at Y=10000. */
volatile LONG g_yz_a010_animation_ready = 0;
volatile LONG g_yz_a010_spu_puts = 0;
volatile LONG g_yz_a010_spu_put_bytes = 0;
volatile LONG g_yz_a010_spu_groups = 0;
volatile LONG g_yz_a010_spu_group_bytes = 0;
volatile LONG g_yz_a010_spu_headers = 0;
volatile LONG g_yz_a010_spu_args = 0;
volatile LONG g_yz_a010_spu_begin = 0;
volatile LONG g_yz_a010_spu_end = 0;
volatile LONG g_yz_a010_spu_array = 0;
volatile LONG g_yz_a010_spu_index = 0;
volatile LONG g_yz_a010_spu_vp = 0;
volatile LONG g_yz_a010_spu_const = 0;
volatile LONG g_yz_a010_spu_unparsed = 0;
/* Draw-method headers seen at their actual publication sites.  The FIFO
 * consumer drains these at each flip, giving us a producer-vs-consumer count
 * without confusing command packets with executed draws. */
volatile LONG g_yz_a010_ppucmd_headers = 0;
volatile LONG g_yz_a010_spucmd_headers = 0;
}

/*
 * A010 stopper-release flight recorder.
 *
 * The producer-side release choice and any gs_task DMA attempt have already
 * happened by the time GET is observed parked on an old self-jump.  Keep the
 * relevant events in a bounded memory ring and print them only if the guarded
 * missing-release detector fires.  This is observation-only.
 */
namespace {
enum : uint32_t {
    YZ_A010_RELTRACE_PPU = 1u,
    YZ_A010_RELTRACE_SPU = 2u,
    YZ_A010_RELTRACE_GATE = 3u,
    YZ_A010_RELTRACE_SPU_COMMIT = 4u,
    YZ_A010_RELTRACE_PPU_STORE = 5u,
    YZ_A010_RELTRACE_SPU_ATOMIC = 6u,
    YZ_A010_RELTRACE_PPU_BULK = 7u,
    YZ_A010_RELTRACE_STOP_CREATE = 8u,
    YZ_A010_RELTRACE_DISPATCH = 9u,
};

struct yz_a010_reltrace_event {
    volatile LONG64 published_seq;
    ULONGLONG tick_ms;
    uint32_t kind;
    uint32_t actor;
    uint32_t pc;
    uint32_t ea;
    uint32_t size;
    uint32_t aux;
    uint32_t words[4];
    uint32_t state[4];
};

constexpr LONG64 YZ_A010_RELTRACE_CAP = 16384;
static yz_a010_reltrace_event
    g_yz_a010_reltrace[YZ_A010_RELTRACE_CAP] = {};
static volatile LONG64 g_yz_a010_reltrace_seq = 0;
static volatile LONG g_yz_a010_reltrace_mode = -1;
static volatile LONG g_yz_a010_reltrace_banner = 0;
constexpr uint32_t YZ_A010_RELWATCH_CAP = 65536u;
struct yz_a010_relwatch {
    volatile LONG ea;
    volatile LONG expected;
    volatile LONG64 source_seq;
};
static yz_a010_relwatch
    g_yz_a010_relwatch[YZ_A010_RELWATCH_CAP] = {};
static volatile LONG
    g_yz_a010_relwatch_lines[0x800000u / 128u] = {};

/*
 * Ring addresses are reused many times during a scene.  The original release
 * trace keyed only by EA, so a release from an older occupant could be
 * mistaken for the release of the stopper currently blocking GET.  Retain a
 * compact generation record for every self-jump emitted by the real gs_task
 * group publisher.  No guest state is changed.
 */
struct yz_a010_stop_generation {
    volatile LONG ea;
    volatile LONG generation;
    volatile LONG release_generation;
    volatile LONG commit_generation;
    volatile LONG creator_actor;
    volatile LONG creator_pc;
    volatile LONG create_size;
    volatile LONG create_offset;
    volatile LONG payload_hash;
    volatile LONG context[16];
    volatile LONG64 create_seq;
    volatile LONG64 release_seq;
    volatile LONG64 commit_seq;
};
static yz_a010_stop_generation
    g_yz_a010_stop_generation[YZ_A010_RELWATCH_CAP] = {};
static SRWLOCK g_yz_a010_stop_generation_lock = SRWLOCK_INIT;

/* Collision-resistant, low-overhead lifecycle table for the clean-lane
 * root catcher.  The older generation table is direct-mapped and aliases
 * FIFO addresses every 256 KiB, so a producer several MiB ahead can evict
 * the exact stopper before GET finally parks on it. */
/* One slot per word in the 8 MiB title FIFO.  The previous 2^18-entry
 * open-addressed table eventually saturated during a long run, which made
 * every later stopper look unobserved.  With an odd multiplicative hash and
 * 2^21 slots, the 2^21 consecutive FIFO word indices map bijectively. */
constexpr uint32_t YZ_A010_STOPLIFE_CAP = 1u << 21;
struct yz_a010_stoplife {
    volatile LONG ea;
    volatile LONG generation;
    volatile LONG creator_actor;
    volatile LONG creator_pc;
    volatile LONG release_generation;
    volatile LONG release_pc;
    volatile LONG release_word;
    volatile LONG commit_generation;
    volatile LONG commit_word;
    volatile LONG64 create_ms;
    volatile LONG64 release_ms;
    volatile LONG64 commit_ms;
};
static yz_a010_stoplife g_yz_a010_stoplife[YZ_A010_STOPLIFE_CAP] = {};

/* func_00EAB3DC writes a forward JUMP from the command cursor to an aligned
 * inline data payload, then stores the exact cursor after that payload.  The
 * payload can contain vertex-program words which are also syntactically valid
 * RSX flow words.  Retain the producer's complete source/command/end triple so
 * the strict native owner never guesses through those bytes.  Each word has a
 * tiny seqlock: writers are rare, while the read side remains allocation-free
 * and touches only the exact GET slot. */
struct yz_a010_data_island_record {
    volatile LONG sequence;
    volatile LONG command;
    volatile LONG end_ea;
};
static yz_a010_data_island_record
    g_yz_a010_data_island[0x800000u / 4u] = {};

extern "C" void yz_a010_data_island_register(
    uint32_t source_ea, uint32_t command, uint32_t data_end_ea)
{
    if (source_ea < 0x40400000u || source_ea >= 0x40C00000u ||
        (source_ea & 3u) || (command & 0xE0000003u) != 0x20000000u ||
        data_end_ea <= source_ea || data_end_ea >= 0x40C00000u)
        return;
    yz_a010_data_island_record* const record =
        &g_yz_a010_data_island[(source_ea - 0x40400000u) >> 2];
    for (;;) {
        const LONG sequence = InterlockedCompareExchange(
            &record->sequence, 0, 0);
        if (sequence & 1) {
            YieldProcessor();
            continue;
        }
        if (InterlockedCompareExchange(
                &record->sequence, sequence + 1, sequence) != sequence)
            continue;
        InterlockedExchange(&record->command, (LONG)command);
        InterlockedExchange(&record->end_ea, (LONG)data_end_ea);
        MemoryBarrier();
        InterlockedExchange(&record->sequence, sequence + 2);
        return;
    }
}

static int yz_a010_data_island_snapshot(
    uint32_t source_ea, uint32_t* command, uint32_t* data_end_ea,
    uint32_t* generation)
{
    if (!command || !data_end_ea ||
        source_ea < 0x40400000u || source_ea >= 0x40C00000u ||
        (source_ea & 3u))
        return 0;
    yz_a010_data_island_record* const record =
        &g_yz_a010_data_island[(source_ea - 0x40400000u) >> 2];
    /* Empty slots dominate the strict-owner flow path. Reject them with one
     * acquire load before entering the seqlock retry, avoiding a locked RMW
     * for each of the four late-edge candidates on ordinary JUMPs. */
#if defined(YZ_PERF_CLEAN)
    const LONG visible = ReadAcquire(&record->sequence);
#else
    const LONG visible = InterlockedCompareExchange(
        &record->sequence, 0, 0);
#endif
    if (!visible || (visible & 1))
        return 0;
    for (unsigned attempt = 0; attempt < 4u; ++attempt) {
        const LONG before = InterlockedCompareExchange(
            &record->sequence, 0, 0);
        if (!before || (before & 1))
            return 0;
        const uint32_t saved_command = (uint32_t)InterlockedCompareExchange(
            &record->command, 0, 0);
        const uint32_t saved_end = (uint32_t)InterlockedCompareExchange(
            &record->end_ea, 0, 0);
        MemoryBarrier();
        const LONG after = InterlockedCompareExchange(
            &record->sequence, 0, 0);
        if (before != after || (after & 1))
            continue;
        *command = saved_command;
        *data_end_ea = saved_end;
        if (generation)
            *generation = (uint32_t)after >> 1;
        return 1;
    }
    return 0;
}

static uint32_t yz_a010_data_island_end(uint32_t source_ea)
{
    uint32_t command = 0u;
    uint32_t data_end_ea = 0u;
    return yz_a010_data_island_snapshot(
               source_ea, &command, &data_end_ea, nullptr) &&
           vm_read32(source_ea) == command
        ? data_end_ea : 0u;
}

static yz_a010_stoplife* yz_a010_stoplife_find(uint32_t ea, int create)
{
    uint32_t slot = ((ea >> 2) * 2654435761u) &
                    (YZ_A010_STOPLIFE_CAP - 1u);
    for (unsigned probe = 0; probe < 16u; ++probe) {
        yz_a010_stoplife* const life =
            &g_yz_a010_stoplife[(slot + probe) &
                                (YZ_A010_STOPLIFE_CAP - 1u)];
        const uint32_t have = (uint32_t)InterlockedCompareExchange(
            &life->ea, create ? (LONG)ea : 0, 0);
        if (have == ea || (create && have == 0u))
            return life;
        if (!create && have == 0u)
            return nullptr;
    }
    return nullptr;
}

static void yz_a010_stoplife_create(uint32_t ea, uint32_t actor,
                                    uint32_t pc)
{
    yz_a010_stoplife* const life = yz_a010_stoplife_find(ea, 1);
    if (!life)
        return;
    LONG generation = InterlockedIncrement(&life->generation);
    if (generation == 0)
        generation = InterlockedIncrement(&life->generation);
    InterlockedExchange(&life->creator_actor, (LONG)actor);
    InterlockedExchange(&life->creator_pc, (LONG)pc);
    InterlockedExchange(&life->release_generation, 0);
    InterlockedExchange(&life->release_pc, 0);
    InterlockedExchange(&life->release_word, 0);
    InterlockedExchange(&life->commit_generation, 0);
    InterlockedExchange(&life->commit_word, 0);
    InterlockedExchange64(&life->create_ms, (LONG64)GetTickCount64());
    InterlockedExchange64(&life->release_ms, 0);
    InterlockedExchange64(&life->commit_ms, 0);
}

static void yz_a010_stoplife_mark(uint32_t ea, uint32_t pc,
                                  uint32_t word, int commit)
{
    yz_a010_stoplife* const life = yz_a010_stoplife_find(ea, 0);
    if (!life)
        return;
    const LONG generation = InterlockedCompareExchange(
        &life->generation, 0, 0);
    if (commit) {
        InterlockedExchange(&life->commit_word, (LONG)word);
        InterlockedExchange(&life->commit_generation, generation);
        InterlockedExchange64(&life->commit_ms, (LONG64)GetTickCount64());
    } else {
        InterlockedExchange(&life->release_pc, (LONG)pc);
        InterlockedExchange(&life->release_word, (LONG)word);
        InterlockedExchange(&life->release_generation, generation);
        InterlockedExchange64(&life->release_ms, (LONG64)GetTickCount64());
    }
}

static void yz_a010_stoplife_dump(uint32_t ea)
{
    yz_a010_stoplife* const life = yz_a010_stoplife_find(ea, 0);
    if (!life) {
        fprintf(stderr,
                "[a010-stoplife] ea=0x%08X lifecycle=ABSENT\n", ea);
        return;
    }
    fprintf(stderr,
            "[a010-stoplife] ea=0x%08X gen=%u creator=%08X pc=%05X "
            "release-gen=%u release-pc=%05X release-word=%08X "
            "commit-gen=%u commit-word=%08X ms=%lld/%lld/%lld\n",
            ea, (uint32_t)life->generation,
            (uint32_t)life->creator_actor,
            (uint32_t)life->creator_pc,
            (uint32_t)life->release_generation,
            (uint32_t)life->release_pc,
            (uint32_t)life->release_word,
            (uint32_t)life->commit_generation,
            (uint32_t)life->commit_word,
            (long long)life->create_ms,
            (long long)life->release_ms,
            (long long)life->commit_ms);
}

static const char* yz_a010_reltrace_kind_name(uint32_t kind)
{
    switch (kind) {
    case YZ_A010_RELTRACE_PPU: return "ppu-choice";
    case YZ_A010_RELTRACE_SPU: return "spu-attempt";
    case YZ_A010_RELTRACE_GATE: return "gate";
    case YZ_A010_RELTRACE_SPU_COMMIT: return "spu-commit";
    case YZ_A010_RELTRACE_PPU_STORE: return "ppu-store";
    case YZ_A010_RELTRACE_SPU_ATOMIC: return "spu-atomic";
    case YZ_A010_RELTRACE_PPU_BULK: return "ppu-bulk";
    case YZ_A010_RELTRACE_STOP_CREATE: return "stop-create";
    case YZ_A010_RELTRACE_DISPATCH: return "release-dispatch";
    default: return "unknown";
    }
}

static yz_a010_relwatch* yz_a010_relwatch_slot(uint32_t ea)
{
    return &g_yz_a010_relwatch[(ea >> 2) &
                              (YZ_A010_RELWATCH_CAP - 1u)];
}

static uint32_t yz_a010_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static uint32_t yz_a010_hash32(const uint8_t* p, uint32_t size)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static yz_a010_stop_generation* yz_a010_stop_generation_slot(
    uint32_t ea)
{
    return &g_yz_a010_stop_generation[
        (ea >> 2) & (YZ_A010_RELWATCH_CAP - 1u)];
}

static uint32_t yz_a010_stop_generation_create(
    uint32_t actor, uint32_t pc, uint32_t ea, uint32_t size,
    uint32_t offset, uint32_t payload_hash,
    const uint32_t* context)
{
    yz_a010_stop_generation* const slot =
        yz_a010_stop_generation_slot(ea);
    AcquireSRWLockExclusive(&g_yz_a010_stop_generation_lock);
    const uint32_t prior_ea =
        (uint32_t)InterlockedCompareExchange(&slot->ea, 0, 0);
    uint32_t generation = prior_ea == ea
        ? (uint32_t)InterlockedCompareExchange(
              &slot->generation, 0, 0) + 1u
        : 1u;
    if (!generation)
        generation = 1u;
    slot->ea = (LONG)ea;
    slot->generation = (LONG)generation;
    slot->release_generation = 0;
    slot->commit_generation = 0;
    slot->creator_actor = (LONG)actor;
    slot->creator_pc = (LONG)pc;
    slot->create_size = (LONG)size;
    slot->create_offset = (LONG)offset;
    slot->payload_hash = (LONG)payload_hash;
    for (unsigned i = 0; i < 16u; ++i)
        slot->context[i] = (LONG)(context ? context[i] : 0u);
    slot->create_seq = 0;
    slot->release_seq = 0;
    slot->commit_seq = 0;
    ReleaseSRWLockExclusive(&g_yz_a010_stop_generation_lock);
    return generation;
}

static uint32_t yz_a010_stop_generation_mark(
    uint32_t ea, int committed, LONG64 seq)
{
    yz_a010_stop_generation* const slot =
        yz_a010_stop_generation_slot(ea);
    uint32_t generation = 0;
    AcquireSRWLockExclusive(&g_yz_a010_stop_generation_lock);
    if ((uint32_t)slot->ea == ea) {
        generation = (uint32_t)slot->generation;
        if (committed) {
            slot->commit_generation = (LONG)generation;
            slot->commit_seq = seq;
        } else {
            slot->release_generation = (LONG)generation;
            slot->release_seq = seq;
        }
    }
    ReleaseSRWLockExclusive(&g_yz_a010_stop_generation_lock);
    return generation;
}

static void yz_a010_stop_generation_set_create_seq(
    uint32_t ea, uint32_t generation, LONG64 seq)
{
    yz_a010_stop_generation* const slot =
        yz_a010_stop_generation_slot(ea);
    AcquireSRWLockExclusive(&g_yz_a010_stop_generation_lock);
    if ((uint32_t)slot->ea == ea &&
        (uint32_t)slot->generation == generation)
        slot->create_seq = seq;
    ReleaseSRWLockExclusive(&g_yz_a010_stop_generation_lock);
}

static void yz_a010_stop_generation_dump(uint32_t ea)
{
    yz_a010_stop_generation* const slot =
        yz_a010_stop_generation_slot(ea);
    AcquireSRWLockShared(&g_yz_a010_stop_generation_lock);
    if ((uint32_t)slot->ea != ea) {
        fprintf(stderr,
                "[a010-stopgen] ea=0x%08X current-generation=ABSENT\n",
                ea);
        ReleaseSRWLockShared(&g_yz_a010_stop_generation_lock);
        return;
    }
    fprintf(stderr,
            "[a010-stopgen] ea=0x%08X gen=%u create-seq=%lld "
            "release-gen=%u release-seq=%lld commit-gen=%u "
            "commit-seq=%lld creator=%08X pc=%08X size=%X off=%X "
            "hash=%08X\n",
            ea, (uint32_t)slot->generation,
            (long long)slot->create_seq,
            (uint32_t)slot->release_generation,
            (long long)slot->release_seq,
            (uint32_t)slot->commit_generation,
            (long long)slot->commit_seq,
            (uint32_t)slot->creator_actor,
            (uint32_t)slot->creator_pc,
            (uint32_t)slot->create_size,
            (uint32_t)slot->create_offset,
            (uint32_t)slot->payload_hash);
    if ((uint32_t)slot->creator_actor & 0x80000000u) {
        fprintf(stderr,
                "[a010-stopgen] ppu state=%08X "
                "head=%08X journal-count=%08X pending=%08X "
                "limit=%08X guest-pc=%08X tid=%u old=%08X\n",
                (uint32_t)slot->context[0],
                (uint32_t)slot->context[1],
                (uint32_t)slot->context[2],
                (uint32_t)slot->context[3],
                (uint32_t)slot->context[4],
                (uint32_t)slot->context[5],
                (uint32_t)slot->context[6],
                (uint32_t)slot->context[7]);
        fprintf(stderr,
                "[a010-stopgen] ppu-stack="
                "%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X\n",
                (uint32_t)slot->context[8],
                (uint32_t)slot->context[9],
                (uint32_t)slot->context[10],
                (uint32_t)slot->context[11],
                (uint32_t)slot->context[12],
                (uint32_t)slot->context[13],
                (uint32_t)slot->context[14],
                (uint32_t)slot->context[15]);
    } else {
        fprintf(stderr,
                "[a010-stopgen] regs "
                "r3=%08X r4=%08X r5=%08X r6=%08X "
                "r7=%08X r8=%08X r9=%08X r10=%08X\n",
                (uint32_t)slot->context[0],
                (uint32_t)slot->context[1],
                (uint32_t)slot->context[2],
                (uint32_t)slot->context[3],
                (uint32_t)slot->context[4],
                (uint32_t)slot->context[5],
                (uint32_t)slot->context[6],
                (uint32_t)slot->context[7]);
        fprintf(stderr,
                "[a010-stopgen] ls-r3=%08X/%08X/%08X/%08X "
                "ls-r4=%08X/%08X/%08X/%08X\n",
                (uint32_t)slot->context[8],
                (uint32_t)slot->context[9],
                (uint32_t)slot->context[10],
                (uint32_t)slot->context[11],
                (uint32_t)slot->context[12],
                (uint32_t)slot->context[13],
                (uint32_t)slot->context[14],
                (uint32_t)slot->context[15]);
    }
    ReleaseSRWLockShared(&g_yz_a010_stop_generation_lock);
}

static LONG64 yz_a010_stop_generation_create_seq(uint32_t ea)
{
    yz_a010_stop_generation* const slot =
        yz_a010_stop_generation_slot(ea);
    LONG64 seq = 0;
    AcquireSRWLockShared(&g_yz_a010_stop_generation_lock);
    if ((uint32_t)slot->ea == ea)
        seq = slot->create_seq;
    ReleaseSRWLockShared(&g_yz_a010_stop_generation_lock);
    return seq;
}

static int yz_a010_relwatch_range(uint32_t ea, uint32_t size)
{
    if (!size || ea >= 0x40C00000u ||
        (uint64_t)ea + size <= 0x40400000ull)
        return 0;
    uint32_t first = ea < 0x40400000u ? 0u :
        ((ea - 0x40400000u) >> 7);
    uint64_t last_byte = (uint64_t)ea + size - 1u;
    uint32_t last = last_byte >= 0x40C00000ull
        ? ((0x800000u - 1u) >> 7)
        : (((uint32_t)last_byte - 0x40400000u) >> 7);
    for (uint32_t line = first; line <= last; ++line) {
        if (InterlockedCompareExchange(
                &g_yz_a010_relwatch_lines[line], 0, 0) != 0)
            return 1;
    }
    return 0;
}

static int yz_a010_relwatch_first(
    uint32_t ea, uint32_t size, uint32_t* target_out,
    uint32_t* expected_out, uint32_t* source_seq_out)
{
    if (!size)
        return 0;
    const uint64_t end = (uint64_t)ea + size;
    uint32_t word = (ea + 3u) & ~3u;
    for (; (uint64_t)word + 4u <= end; word += 4u) {
        yz_a010_relwatch* const watch = yz_a010_relwatch_slot(word);
        if ((uint32_t)InterlockedCompareExchange(
                &watch->ea, 0, 0) != word)
            continue;
        if (target_out) *target_out = word;
        if (expected_out)
            *expected_out = (uint32_t)InterlockedCompareExchange(
                &watch->expected, 0, 0);
        if (source_seq_out)
            *source_seq_out = (uint32_t)InterlockedCompareExchange64(
                &watch->source_seq, 0, 0);
        return 1;
    }
    return 0;
}

static int yz_a010_reltrace_on()
{
    LONG mode = InterlockedCompareExchange(
        &g_yz_a010_reltrace_mode, -1, -1);
    if (mode < 0) {
        const LONG requested =
            (getenv("YZ_A010_RELEASE_TRACE") ||
             getenv("YZ_A010_RELEASE_RING")) ? 1 : 0;
        const LONG prior = InterlockedCompareExchange(
            &g_yz_a010_reltrace_mode, requested, -1);
        mode = prior < 0 ? requested : prior;
    }
    if (mode && InterlockedCompareExchange(
            &g_yz_a010_reltrace_banner, 1, 0) == 0) {
        fprintf(stderr,
                "[a010-reltrace] ARMED: in-memory PPU release-choice + "
                "gs_task DMA flight recorder\n");
        fflush(stderr);
    }
    return mode != 0;
}

static int yz_a010_reltrace_targeted()
{
    static volatile LONG mode = -1;
    LONG value = InterlockedCompareExchange(&mode, -1, -1);
    if (value < 0) {
        const LONG requested =
            getenv("YZ_A010_RELEASE_TARGETED") ? 1 : 0;
        const LONG prior = InterlockedCompareExchange(
            &mode, requested, -1);
        value = prior < 0 ? requested : prior;
    }
    return value != 0;
}

static int yz_a010_reltrace_eager_dump()
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("YZ_A010_RELEASE_TRACE") ? 1 : 0;
    return enabled;
}

static yz_a010_reltrace_event* yz_a010_reltrace_reserve(
    uint32_t kind, uint32_t actor, uint32_t pc)
{
    const LONG64 seq = InterlockedIncrement64(
        &g_yz_a010_reltrace_seq);
    yz_a010_reltrace_event* const ev =
        &g_yz_a010_reltrace[seq % YZ_A010_RELTRACE_CAP];
    InterlockedExchange64(&ev->published_seq, 0);
    ev->tick_ms = GetTickCount64();
    ev->kind = kind;
    ev->actor = actor;
    ev->pc = pc;
    ev->ea = 0;
    ev->size = 0;
    ev->aux = 0;
    memset(ev->words, 0, sizeof(ev->words));
    memset(ev->state, 0, sizeof(ev->state));
    MemoryBarrier();
    ev->published_seq = -seq;
    return ev;
}

static void yz_a010_reltrace_publish(yz_a010_reltrace_event* ev)
{
    const LONG64 reserved = ev->published_seq;
    MemoryBarrier();
    InterlockedExchange64(&ev->published_seq, -reserved);
}

static void yz_a010_reltrace_dump(uint32_t stopper_ea)
{
    if (!yz_a010_reltrace_on())
        return;

    const LONG64 end = InterlockedCompareExchange64(
        &g_yz_a010_reltrace_seq, 0, 0);
    const LONG64 begin =
        end > YZ_A010_RELTRACE_CAP ? end - YZ_A010_RELTRACE_CAP + 1 : 1;
    unsigned exact = 0;
    unsigned recent = 0;

    fprintf(stderr,
            "[a010-reltrace] FAILURE stopper=0x%08X seq=%lld; "
            "matching producer history follows\n",
            stopper_ea, (long long)end);
    yz_a010_stoplife_dump(stopper_ea);
    yz_a010_stop_generation_dump(stopper_ea);
    const LONG64 create_seq =
        yz_a010_stop_generation_create_seq(stopper_ea);

    for (LONG64 seq = begin; seq <= end; ++seq) {
        const yz_a010_reltrace_event* const ev =
            &g_yz_a010_reltrace[seq % YZ_A010_RELTRACE_CAP];
        if (InterlockedCompareExchange64(
                const_cast<volatile LONG64*>(&ev->published_seq),
                0, 0) != seq)
            continue;
        const uint64_t dma_end =
            (uint64_t)ev->ea + (uint64_t)ev->size;
        const int match =
            ((ev->kind == YZ_A010_RELTRACE_PPU ||
              ev->kind == YZ_A010_RELTRACE_PPU_STORE ||
              ev->kind == YZ_A010_RELTRACE_PPU_BULK) &&
             ev->ea == stopper_ea) ||
            ((ev->kind == YZ_A010_RELTRACE_SPU ||
              ev->kind == YZ_A010_RELTRACE_SPU_COMMIT ||
              ev->kind == YZ_A010_RELTRACE_SPU_ATOMIC) &&
             ev->ea <= stopper_ea && dma_end > stopper_ea);
        if (!match)
            continue;
        exact++;
        fprintf(stderr,
                "[a010-reltrace] EXACT seq=%lld t=%llums kind=%s "
                "actor=%08X pc=%08X ea=%08X size=%X aux=%08X "
                "words=%08X/%08X/%08X/%08X "
                "state=%08X/%08X/%08X/%08X\n",
                (long long)seq,
                (unsigned long long)ev->tick_ms,
                yz_a010_reltrace_kind_name(ev->kind),
                ev->actor, ev->pc, ev->ea, ev->size, ev->aux,
                ev->words[0], ev->words[1], ev->words[2], ev->words[3],
                ev->state[0], ev->state[1], ev->state[2], ev->state[3]);
    }

    if (create_seq > 0) {
        const LONG64 window_begin =
            create_seq > 16 ? create_seq - 16 : begin;
        const LONG64 window_end =
            create_seq + 32 < end ? create_seq + 32 : end;
        for (LONG64 seq = window_begin; seq <= window_end; ++seq) {
            const yz_a010_reltrace_event* const ev =
                &g_yz_a010_reltrace[
                    seq % YZ_A010_RELTRACE_CAP];
            if (InterlockedCompareExchange64(
                    const_cast<volatile LONG64*>(
                        &ev->published_seq), 0, 0) != seq)
                continue;
            fprintf(stderr,
                    "[a010-reltrace] GENWINDOW seq=%lld kind=%s "
                    "actor=%08X pc=%08X ea=%08X size=%X aux=%08X "
                    "words=%08X/%08X/%08X/%08X "
                    "state=%08X/%08X/%08X/%08X\n",
                    (long long)seq,
                    yz_a010_reltrace_kind_name(ev->kind),
                    ev->actor, ev->pc, ev->ea, ev->size, ev->aux,
                    ev->words[0], ev->words[1],
                    ev->words[2], ev->words[3],
                    ev->state[0], ev->state[1],
                    ev->state[2], ev->state[3]);
        }
    }

    const LONG64 tail_begin = end > 96 ? end - 95 : begin;
    for (LONG64 seq = tail_begin; seq <= end; ++seq) {
        const yz_a010_reltrace_event* const ev =
            &g_yz_a010_reltrace[seq % YZ_A010_RELTRACE_CAP];
        if (InterlockedCompareExchange64(
                const_cast<volatile LONG64*>(&ev->published_seq),
                0, 0) != seq)
            continue;
        recent++;
        fprintf(stderr,
                "[a010-reltrace] TAIL seq=%lld kind=%s actor=%08X "
                "pc=%08X ea=%08X size=%X aux=%08X "
                "words=%08X/%08X/%08X/%08X "
                "state=%08X/%08X/%08X/%08X\n",
                (long long)seq,
                yz_a010_reltrace_kind_name(ev->kind),
                ev->actor, ev->pc, ev->ea, ev->size, ev->aux,
                ev->words[0], ev->words[1], ev->words[2], ev->words[3],
                ev->state[0], ev->state[1], ev->state[2], ev->state[3]);
    }
    fprintf(stderr,
            "[a010-reltrace] SUMMARY exact=%u recent=%u\n",
            exact, recent);
    fflush(stderr);
}
}  // namespace

extern "C" void yz_a010_reltrace_ppu(uint32_t pc,
                                      const ppu_context* ctx)
{
    if (!ctx ||
        InterlockedCompareExchange(
            &g_yz_a010_release_scene_active, 0, 0) == 0 ||
        !yz_a010_reltrace_on())
        return;
    if (yz_a010_reltrace_targeted())
        return;

    yz_a010_reltrace_event* const ev =
        yz_a010_reltrace_reserve(
            YZ_A010_RELTRACE_PPU,
            (uint32_t)yz_thread_current_id(), pc);
    const uint32_t state = (uint32_t)ctx->gpr[31];
    const uint32_t stopper = (uint32_t)ctx->gpr[10];
    ev->ea = stopper;
    ev->size = 4;
    ev->aux = (uint32_t)ctx->gpr[11];
    if (stopper >= 0x40400000u && stopper < 0x40C00000u)
        ev->words[0] = vm_read32(stopper);
    if (state >= 0x10000u && state < 0xE0000000u) {
        ev->state[0] = vm_read32(state + 0x00u);
        ev->state[1] = vm_read32(state + 0x1Cu);
        ev->state[2] = vm_read32(state + 0x20u);
        ev->state[3] = vm_read32(state + 0x24u);
    }
    yz_a010_reltrace_publish(ev);
}

extern "C" void yz_a010_reltrace_spu(
    uint32_t spu_id, uint32_t image_id, uint32_t pc,
    uint32_t ea, const uint8_t* payload, uint32_t size,
    const uint32_t* context)
{
    if (!payload || !size ||
        InterlockedCompareExchange(
            &g_yz_a010_release_scene_active, 0, 0) == 0 ||
        !yz_a010_reltrace_on())
        return;

    const int fifo = ea >= 0x40400000u && ea < 0x40C00000u;
    if (!fifo)
        return;

    const int targeted = yz_a010_reltrace_targeted();

    /* In the compact lane, classify the words themselves instead of relying
     * on a gs_task PC.  This catches alternate lifted regions and any other
     * SPU publisher while remaining event-log free. */
    if (targeted) {
        const uint32_t actor =
            (image_id << 16) | (spu_id & 0xFFFFu);
        for (uint32_t off = 0; off + 4u <= size; off += 4u) {
            const uint32_t word_ea = ea + off;
            if (word_ea < 0x40400000u || word_ea >= 0x40C00000u)
                continue;
            const uint32_t word = yz_a010_be32(payload + off);
            const uint32_t self =
                0x20000000u |
                ((word_ea - 0x40400000u) & 0x1FFFFFFCu);
            if (word == self) {
                yz_a010_stoplife_create(word_ea, actor, pc);
            } else if ((word & 0xE0000003u) == 0x20000000u) {
                yz_a010_stoplife_mark(word_ea, pc, word, 0);
            }
        }
        return;
    }

    /*
     * The group publisher can place one or more terminal self-jumps anywhere
     * in a bulk PUT.  Detect those words by comparing their encoded target
     * with their own destination, then start a new lifecycle generation
     * before considering any release record at the same reused ring address.
     */
    if (image_id == 0u && pc == 0x05F70u) {
        const uint32_t payload_hash = yz_a010_hash32(payload, size);
        for (uint32_t off = 0; off + 4u <= size; off += 4u) {
            const uint32_t word_ea = ea + off;
            if (word_ea < 0x40400000u || word_ea >= 0x40C00000u)
                continue;
            const uint32_t word = yz_a010_be32(payload + off);
            const uint32_t self =
                0x20000000u |
                ((word_ea - 0x40400000u) & 0x1FFFFFFCu);
            if (word != self)
                continue;
            const uint32_t actor =
                (image_id << 16) | (spu_id & 0xFFFFu);
            yz_a010_stoplife_create(word_ea, actor, pc);
            if (targeted)
                continue;
            const uint32_t generation =
                yz_a010_stop_generation_create(
                    actor, pc, word_ea, size, off,
                    payload_hash, context);
            yz_a010_reltrace_event* const create_ev =
                yz_a010_reltrace_reserve(
                    YZ_A010_RELTRACE_STOP_CREATE, actor, pc);
            create_ev->ea = word_ea;
            create_ev->size = 4u;
            create_ev->aux = generation;
            create_ev->words[0] = word;
            create_ev->words[1] = vm_read32(word_ea);
            create_ev->words[2] = off;
            create_ev->words[3] = payload_hash;
            for (unsigned i = 0; i < 4u; ++i)
                create_ev->state[i] = context ? context[i] : 0u;
            yz_a010_reltrace_publish(create_ev);
            yz_a010_stop_generation_set_create_seq(
                word_ea, generation,
                InterlockedCompareExchange64(
                    &create_ev->published_seq, 0, 0));

            yz_a010_relwatch* const watch =
                yz_a010_relwatch_slot(word_ea);
            InterlockedExchange(&watch->expected, (LONG)self);
            InterlockedExchange64(
                &watch->source_seq,
                InterlockedCompareExchange64(
                    &create_ev->published_seq, 0, 0));
            MemoryBarrier();
            InterlockedExchange(&watch->ea, (LONG)word_ea);
            InterlockedExchange(
                &g_yz_a010_relwatch_lines[
                    (word_ea - 0x40400000u) >> 7], 1);
        }
    }

    const uint32_t first_word =
        size >= 4u ? yz_a010_be32(payload) : 0u;
    const uint32_t expected_release =
        0x20000000u |
        (((ea - 0x40400000u) + 4u) & 0x1FFFFFFCu);
    const int arm_release =
        image_id == 0u && pc >= 0x05EB8u && pc <= 0x05F20u &&
        size == 4u && first_word == expected_release;
    if (arm_release)
        yz_a010_stoplife_mark(ea, pc, first_word, 0);
    if (targeted)
        return;
    if (!arm_release && !yz_a010_relwatch_range(ea, size))
        return;

    yz_a010_reltrace_event* const ev =
        yz_a010_reltrace_reserve(
            YZ_A010_RELTRACE_SPU,
            (image_id << 16) | (spu_id & 0xFFFFu), pc);
    ev->ea = ea;
    ev->size = size;
    for (unsigned wi = 0; wi < 4u && wi * 4u + 4u <= size; ++wi) {
        const uint32_t off = wi * 4u;
        ev->words[wi] =
            ((uint32_t)payload[off + 0] << 24) |
            ((uint32_t)payload[off + 1] << 16) |
            ((uint32_t)payload[off + 2] << 8) |
            (uint32_t)payload[off + 3];
    }
    if (!arm_release) {
        uint32_t target = 0, expected = 0, source_seq = 0;
        if (yz_a010_relwatch_first(
                ea, size, &target, &expected, &source_seq)) {
            const uint32_t off = target - ea;
            ev->state[0] = target;
            ev->state[1] = vm_read32(target);
            ev->state[2] = yz_a010_be32(payload + off);
            ev->state[3] = source_seq;
            ev->aux = expected;
        }
    }
    if (arm_release) {
        ev->state[3] = yz_a010_stop_generation_mark(
            ea, 0, -ev->published_seq);
        yz_a010_relwatch* const watch = yz_a010_relwatch_slot(ea);
        InterlockedExchange(&watch->expected, (LONG)ev->words[0]);
        InterlockedExchange64(&watch->source_seq, -ev->published_seq);
        MemoryBarrier();
        InterlockedExchange(&watch->ea, (LONG)ea);
        InterlockedExchange(
            &g_yz_a010_relwatch_lines[
                (ea - 0x40400000u) >> 7], 1);
    }
    yz_a010_reltrace_publish(ev);
}

extern "C" void yz_a010_reltrace_dispatch(
    uint32_t spu_id, uint32_t pc, const uint32_t* words)
{
    if (!words || words[0] != 0x7Fu ||
        InterlockedCompareExchange(
            &g_yz_a010_release_scene_active, 0, 0) == 0 ||
        !yz_a010_reltrace_on() || !yz_a010_reltrace_targeted())
        return;

    const uint32_t stopper = words[1];
    if (stopper < 0x40400000u || stopper >= 0x40C00000u)
        return;

    yz_a010_reltrace_event* const ev = yz_a010_reltrace_reserve(
        YZ_A010_RELTRACE_DISPATCH, spu_id & 0xFFFFu, pc);
    ev->ea = stopper;
    ev->size = 4u;
    ev->aux = vm_read32(stopper);
    for (unsigned i = 0; i < 4u; ++i)
        ev->words[i] = words[i];
    yz_a010_reltrace_publish(ev);

    yz_a010_relwatch* const watch = yz_a010_relwatch_slot(stopper);
    InterlockedExchange(&watch->expected, (LONG)ev->aux);
    InterlockedExchange64(
        &watch->source_seq,
        InterlockedCompareExchange64(&ev->published_seq, 0, 0));
    MemoryBarrier();
    InterlockedExchange(&watch->ea, (LONG)stopper);
    InterlockedExchange(
        &g_yz_a010_relwatch_lines[
            (stopper - 0x40400000u) >> 7], 1);
}

extern "C" void yz_a010_reltrace_spu_commit(
    uint32_t spu_id, uint32_t image_id, uint32_t pc,
    uint32_t ea, const uint8_t* payload, uint32_t size)
{
    if (!payload || size < 4u ||
        InterlockedCompareExchange(
            &g_yz_a010_release_scene_active, 0, 0) == 0 ||
        !yz_a010_reltrace_on())
        return;
    const int targeted = yz_a010_reltrace_targeted();
    if (targeted) {
        for (uint32_t off = 0; off + 4u <= size; off += 4u) {
            const uint32_t word_ea = ea + off;
            if (word_ea < 0x40400000u || word_ea >= 0x40C00000u)
                continue;
            const uint32_t word = yz_a010_be32(payload + off);
            const uint32_t self =
                0x20000000u |
                ((word_ea - 0x40400000u) & 0x1FFFFFFCu);
            if (word != self &&
                (word & 0xE0000003u) == 0x20000000u) {
                yz_a010_stoplife_mark(word_ea, pc, word, 1);
            }
        }
        return;
    }
    const uint32_t expected_release =
        0x20000000u |
        (((ea - 0x40400000u) + 4u) & 0x1FFFFFFCu);
    const int release =
        image_id == 0u && pc >= 0x05EB8u && pc <= 0x05F20u &&
        size == 4u && yz_a010_be32(payload) == expected_release;
    if (release)
        yz_a010_stoplife_mark(
            ea, pc, yz_a010_be32(payload), 1);
    if (targeted)
        return;
    uint32_t target = ea;
    if (ea < 0x40400000u || ea >= 0x40C00000u || !release) {
        return;
    }

    yz_a010_reltrace_event* const ev =
        yz_a010_reltrace_reserve(
            YZ_A010_RELTRACE_SPU_COMMIT,
            (image_id << 16) | (spu_id & 0xFFFFu), pc);
    ev->ea = target;
    ev->size = 4u;
    ev->aux = yz_a010_be32(payload + (target - ea));
    ev->words[0] = vm_read32(target);
    ev->state[0] = ev->words[0] == ev->aux;
    ev->state[1] = yz_a010_stop_generation_mark(
        ea, 1, -ev->published_seq);
    yz_a010_reltrace_publish(ev);
}

extern "C" void yz_a010_reltrace_spu_atomic(
    uint32_t spu_id, uint32_t image_id, uint32_t pc,
    uint32_t line_ea, const uint8_t* new_line, uint32_t cmd)
{
    if (!new_line || line_ea < 0x40400000u ||
        line_ea >= 0x40C00000u ||
        InterlockedCompareExchange(
            &g_yz_a010_release_scene_active, 0, 0) == 0 ||
        !yz_a010_reltrace_on() ||
        !yz_a010_relwatch_range(line_ea, 128u))
        return;

    for (uint32_t off = 0; off < 128u; off += 4u) {
        const uint32_t target = line_ea + off;
        yz_a010_relwatch* const watch = yz_a010_relwatch_slot(target);
        if ((uint32_t)InterlockedCompareExchange(
                &watch->ea, 0, 0) != target)
            continue;
        yz_a010_reltrace_event* const ev =
            yz_a010_reltrace_reserve(
                YZ_A010_RELTRACE_SPU_ATOMIC,
                (image_id << 16) | (spu_id & 0xFFFFu), pc);
        ev->ea = target;
        ev->size = 4u;
        ev->aux = cmd;
        ev->words[0] = vm_read32(target);
        ev->words[1] = yz_a010_be32(new_line + off);
        ev->state[0] = (uint32_t)InterlockedCompareExchange64(
            &watch->source_seq, 0, 0);
        yz_a010_reltrace_publish(ev);
    }
}

extern "C" void yz_a010_reltrace_ppu_store(
    uint32_t addr, uint32_t val, uint32_t guest_pc)
{
    if (addr < 0x40400000u || addr >= 0x40C00000u ||
        InterlockedCompareExchange(
            &g_yz_a010_release_scene_active, 0, 0) == 0 ||
        !yz_a010_reltrace_on())
        return;

    const uint32_t io =
        (addr - 0x40400000u) & 0x1FFFFFFCu;
    const uint32_t self = 0x20000000u | io;
    if (yz_a010_reltrace_targeted()) {
        const uint32_t actor =
            0x80000000u |
            ((uint32_t)yz_thread_current_id() & 0x7FFFFFFFu);
        if (val == self) {
            yz_a010_stoplife_create(addr, actor, guest_pc);
        } else if ((val & 0xE0000003u) == 0x20000000u) {
            /* vm_write32 invokes this hook immediately before the coherent
             * store.  A validated FIFO-range store cannot fault, so record
             * both issue and commit for the PPU-owned lifecycle. */
            yz_a010_stoplife_mark(addr, guest_pc, val, 0);
            yz_a010_stoplife_mark(addr, guest_pc, val, 1);
        }
    }
    uint32_t generation = 0;
    if (val == self && !yz_a010_reltrace_targeted()) {
        uint32_t context[16] = {};
        const uint32_t state = g_yz_game_toc
            ? vm_read32(g_yz_game_toc - 0x7410u) : 0u;
        context[0] = state;
        if (state >= 0x10000u && state < 0xE0000000u) {
            context[1] = vm_read32(state + 0x00u);
            context[2] = vm_read32(state + 0x1Cu);
            context[3] = vm_read32(state + 0x20u);
            context[4] = vm_read32(state + 0x24u);
        }
        context[5] = guest_pc;
        context[6] = (uint32_t)yz_thread_current_id();
        context[7] = vm_read32(addr);
        void* stack[8] = {};
        const USHORT frames =
            RtlCaptureStackBackTrace(1, 8, stack, nullptr);
        for (USHORT i = 0; i < frames; ++i)
            context[8u + i] =
                yz_guest_addr_from_host(stack[i]);

        const uint32_t actor =
            0x80000000u |
            ((uint32_t)yz_thread_current_id() & 0x7FFFFFFFu);
        generation = yz_a010_stop_generation_create(
            actor, guest_pc, addr, 4u, 0u, val, context);
        yz_a010_reltrace_event* const create_ev =
            yz_a010_reltrace_reserve(
                YZ_A010_RELTRACE_STOP_CREATE,
                actor, guest_pc);
        create_ev->ea = addr;
        create_ev->size = 4u;
        create_ev->aux = generation;
        create_ev->words[0] = val;
        create_ev->words[1] = context[7];
        create_ev->words[2] = state;
        create_ev->words[3] = context[3];
        for (unsigned i = 0; i < 4u; ++i)
            create_ev->state[i] = context[1u + i];
        yz_a010_reltrace_publish(create_ev);
        const LONG64 create_seq =
            InterlockedCompareExchange64(
                &create_ev->published_seq, 0, 0);
        yz_a010_stop_generation_set_create_seq(
            addr, generation, create_seq);

        yz_a010_relwatch* const create_watch =
            yz_a010_relwatch_slot(addr);
        InterlockedExchange(
            &create_watch->expected, (LONG)self);
        InterlockedExchange64(
            &create_watch->source_seq, create_seq);
        MemoryBarrier();
        InterlockedExchange(
            &create_watch->ea, (LONG)addr);
        InterlockedExchange(
            &g_yz_a010_relwatch_lines[
                (addr - 0x40400000u) >> 7], 1);
    }

    yz_a010_relwatch* const watch = yz_a010_relwatch_slot(addr);
    if ((uint32_t)InterlockedCompareExchange(
            &watch->ea, 0, 0) != addr)
        return;

    yz_a010_reltrace_event* const ev =
        yz_a010_reltrace_reserve(
            YZ_A010_RELTRACE_PPU_STORE,
            (uint32_t)yz_thread_current_id(), guest_pc);
    ev->ea = addr;
    ev->size = 4u;
    ev->aux = (uint32_t)InterlockedCompareExchange(
        &watch->expected, 0, 0);
    ev->words[0] = vm_read32(addr);
    ev->words[1] = val;
    ev->state[0] = (uint32_t)InterlockedCompareExchange64(
        &watch->source_seq, 0, 0);
    if (val != self &&
        (val & 0xE0000003u) == 0x20000000u)
        ev->state[3] = yz_a010_stop_generation_mark(
            addr, 0, -ev->published_seq);
    yz_a010_reltrace_publish(ev);
}

extern "C" void yz_a010_reltrace_ppu_bulk(
    uint32_t dst, const uint8_t* src, uint32_t size,
    uint32_t guest_pc, uint32_t op, uint8_t fill)
{
    const int targeted = yz_a010_reltrace_targeted();
    if (!size ||
        InterlockedCompareExchange(
            &g_yz_a010_release_scene_active, 0, 0) == 0 ||
        !yz_a010_reltrace_on() ||
        (!targeted && !yz_a010_relwatch_range(dst, size)))
        return;

    if (targeted) {
        const uint32_t actor =
            0x80000000u |
            ((uint32_t)yz_thread_current_id() & 0x7FFFFFFFu);
        const uint64_t fifo_begin = dst < 0x40400000u
            ? 0x40400000ull : (uint64_t)dst;
        const uint64_t raw_end = (uint64_t)dst + size;
        const uint64_t fifo_end = raw_end > 0x40C00000ull
            ? 0x40C00000ull : raw_end;
        uint32_t word_ea = (uint32_t)((fifo_begin + 3u) & ~3ull);
        for (; (uint64_t)word_ea + 4u <= fifo_end; word_ea += 4u) {
            const uint32_t word = op == 1u
                ? yz_a010_be32(src + (word_ea - dst))
                : (uint32_t)fill * 0x01010101u;
            const uint32_t self =
                0x20000000u |
                ((word_ea - 0x40400000u) & 0x1FFFFFFCu);
            if (word == self) {
                yz_a010_stoplife_create(word_ea, actor, guest_pc);
            } else if ((word & 0xE0000003u) == 0x20000000u) {
                yz_a010_stoplife_mark(word_ea, guest_pc, word, 0);
                yz_a010_stoplife_mark(word_ea, guest_pc, word, 1);
            }
        }
        return;
    }

    const uint64_t end = (uint64_t)dst + size;
    uint32_t word = (dst + 3u) & ~3u;
    for (; (uint64_t)word + 4u <= end; word += 4u) {
        yz_a010_relwatch* const watch = yz_a010_relwatch_slot(word);
        if ((uint32_t)InterlockedCompareExchange(
                &watch->ea, 0, 0) != word)
            continue;

        yz_a010_reltrace_event* const ev =
            yz_a010_reltrace_reserve(
                YZ_A010_RELTRACE_PPU_BULK,
                (uint32_t)yz_thread_current_id(), guest_pc);
        ev->ea = word;
        ev->size = 4u;
        ev->aux = (uint32_t)InterlockedCompareExchange(
            &watch->expected, 0, 0);
        ev->words[0] = vm_read32(word);
        ev->words[1] = op == 1u
            ? yz_a010_be32(src + (word - dst))
            : (uint32_t)fill * 0x01010101u;
        ev->state[0] = (uint32_t)InterlockedCompareExchange64(
            &watch->source_seq, 0, 0);
        const uint64_t src_host = (uint64_t)(uintptr_t)src;
        const uint64_t vm_host = (uint64_t)(uintptr_t)vm_base;
        ev->state[1] =
            op == 1u && src_host >= vm_host &&
            src_host - vm_host <= UINT32_MAX
                ? (uint32_t)(src_host - vm_host)
                : (op == 2u ? (uint32_t)fill : UINT32_MAX);
        ev->state[2] = size;
        ev->state[3] = op;
        yz_a010_reltrace_publish(ev);
    }
}

extern "C" void yz_a010_reltrace_gate(
    uint32_t spu_id, uint32_t code, uint32_t key,
    uint32_t witness, uint32_t descriptor,
    uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3)
{
    /*
     * The first trace proved the release handler reached its final PUT.  Gate
     * traffic is extremely hot, so retain it only behind a separate opt-in
     * flag while the focused publication trace is active.
     */
    static volatile LONG gate_mode = -1;
    LONG gate_on = InterlockedCompareExchange(&gate_mode, -1, -1);
    if (gate_on < 0) {
        const LONG requested =
            getenv("YZ_A010_RELEASE_GATE_TRACE") ? 1 : 0;
        const LONG prior = InterlockedCompareExchange(
            &gate_mode, requested, -1);
        gate_on = prior < 0 ? requested : prior;
    }
    if (!gate_on || !yz_a010_reltrace_on() ||
        InterlockedCompareExchange(
            &g_yz_a010_release_scene_active, 0, 0) == 0)
        return;

    yz_a010_reltrace_event* const ev =
        yz_a010_reltrace_reserve(
            YZ_A010_RELTRACE_GATE, spu_id & 0xFFFFu, 0x0000B0C4u);
    ev->ea = descriptor;
    ev->words[0] = code;
    ev->words[1] = key;
    ev->words[2] = witness;
    ev->words[3] = descriptor;
    ev->state[0] = d0;
    ev->state[1] = d1;
    ev->state[2] = d2;
    ev->state[3] = d3;
    yz_a010_reltrace_publish(ev);
}

/* lv2 sys_event handlers (runtime/syscalls/sys_event.c) -- driven directly from
 * sys_rsx_context_allocate to set up libgcm's RSX event port/queue. */
extern "C" int64_t sys_event_port_create(ppu_context*);
extern "C" int64_t sys_event_queue_create(ppu_context*);
extern "C" int64_t sys_event_port_connect_local(ppu_context*);
extern "C" int64_t sys_event_port_send(ppu_context*);
extern "C" void yz_hwwatch_arm(void);   /* main.cpp: DR0 watch on the wid4 record slot */
extern HANDLE g_yz_t1_handle;           /* main.cpp: t1 real handle ([t1-hb]) */
extern ppu_context* g_yz_main_ctx;      /* main.cpp: t1's guest context */
extern int g_yz_updloop_started;        /* main.cpp: first func_00D1E838 entry (ledger #64) */
extern "C" void spu_lockline_lock(void);    /* SPU GETLLAR/PUTLLC serialization */
extern "C" void spu_lockline_unlock(void);
extern "C" void yz_rsx_fifo_acquire(void);
extern "C" void yz_rsx_fifo_release(void);
extern "C" int yz_rsx_flip_pending_any(void);
extern "C" void yz_rsx_vblank_tick(void);
extern "C" void cellGcmDispatchUserCommand(uint32_t cause);
extern "C" void yz_a010_auth_probe_poll(void);

/* The PPU ABI passes only arguments 1-8 in r3-r10. Arguments 9+ are in the
 * parameter save area at r1+0x30+i*8 (i is zero-based from r3). */
extern "C" void yz_ovr__cellSpursJobChainAttributeInitialize(ppu_context* ctx)
{
    auto stack_arg = [ctx](unsigned i) -> uint64_t {
        return vm_read64((uint32_t)ctx->gpr[1] + 0x30u + i * 8u);
    };
    const int32_t rc = _cellSpursJobChainAttributeInitialize(
        (uint32_t)ctx->gpr[3],
        (uint32_t)ctx->gpr[4],
        (CellSpursJobChainAttribute*)(vm_base + (uint32_t)ctx->gpr[5]),
        (const uint64_t*)(vm_base + (uint32_t)ctx->gpr[6]),
        (uint16_t)ctx->gpr[7],
        (uint16_t)ctx->gpr[8],
        (const uint8_t*)(vm_base + (uint32_t)ctx->gpr[9]),
        (uint32_t)ctx->gpr[10],
        (uint32_t)stack_arg(8),
        (uint32_t)stack_arg(9),
        (uint32_t)stack_arg(10),
        (uint32_t)stack_arg(11),
        (uint32_t)stack_arg(12),
        (uint32_t)stack_arg(13));
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

#define YZ_TLS_BASE   0x0FE00000u
#define YZ_HEAP_BASE  0x0D000000u
#define YZ_HEAP_END   0x0FE00000u

/* ---------------------------------------------------------------------------
 * sys_initialize_tls(main_thread_id, tls_seg_addr, tls_seg_size, tls_mem_size)
 *
 * Layout: 0x30-byte zeroed system area, then the TLS image (copied from the
 * ELF's TLS template), then zero fill. r13 = block + 0x30 + 0x7000 (the
 * PPC64 TLS bias); thread vars live at r13 - 0x7000 + offset.
 * -----------------------------------------------------------------------*/
extern "C" void yz_ovr_sys_initialize_tls(ppu_context* ctx)
{
    if (ctx->gpr[13] != 0) { ctx->gpr[3] = 0; return; }

    uint32_t seg_addr = (uint32_t)ctx->gpr[4];
    uint32_t seg_size = (uint32_t)ctx->gpr[5];
    uint32_t mem_size = (uint32_t)ctx->gpr[6];

    if (0x30u + mem_size > 0x100000u) {
        fprintf(stderr, "[tls] TLS image too large: 0x%X\n", mem_size);
        ctx->gpr[3] = 0;
        return;
    }

    memset(vm_base + YZ_TLS_BASE, 0, 0x30 + mem_size);
    if (seg_size)
        memcpy(vm_base + YZ_TLS_BASE + 0x30, vm_base + seg_addr, seg_size);

    ctx->gpr[13] = YZ_TLS_BASE + 0x30 + 0x7000;
    ctx->gpr[3]  = 0;

    printf("[boot] TLS initialized (image 0x%08X +0x%X/0x%X, r13=0x%08llX)\n",
           seg_addr, seg_size, mem_size, (unsigned long long)ctx->gpr[13]);
}

/* ---------------------------------------------------------------------------
 * CRT heap (_sys_heap_*): simple bump allocator, free is a no-op.
 * -----------------------------------------------------------------------*/
static uint32_t yz_heap_ptr = YZ_HEAP_BASE;
static SRWLOCK  yz_heap_lock = SRWLOCK_INIT;

static uint32_t yz_heap_alloc(uint32_t size, uint32_t align)
{
    if (align < 16) align = 16;
    AcquireSRWLockExclusive(&yz_heap_lock);
    uint32_t base = (yz_heap_ptr + align - 1) & ~(align - 1);
    if (base + size > YZ_HEAP_END) {
        ReleaseSRWLockExclusive(&yz_heap_lock);
        fprintf(stderr, "[heap] OUT OF MEMORY (req 0x%X)\n", size);
        return 0;
    }
    yz_heap_ptr = base + size;
    ReleaseSRWLockExclusive(&yz_heap_lock);
    return base;
}

extern "C" void yz_ovr__sys_heap_create_heap(ppu_context* ctx)
{
    ctx->gpr[3] = 1;   /* heap id */
}

extern "C" void yz_ovr__sys_heap_delete_heap(ppu_context* ctx)
{
    ctx->gpr[3] = 0;
}

extern "C" void yz_ovr__sys_heap_malloc(ppu_context* ctx)
{
    /* (heap_id, size) */
    ctx->gpr[3] = yz_heap_alloc((uint32_t)ctx->gpr[4], 16);
}

extern "C" void yz_ovr__sys_heap_memalign(ppu_context* ctx)
{
    /* (heap_id, align, size) */
    ctx->gpr[3] = yz_heap_alloc((uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[4]);
}

extern "C" void yz_ovr__sys_heap_free(ppu_context* ctx)
{
    ctx->gpr[3] = 0;
}

/* ---------------------------------------------------------------------------
 * sys_time_get_system_time -> microseconds (64-bit, so not bridgeable
 * through the generic int32-narrowing bridge)
 * -----------------------------------------------------------------------*/
/* U1/U2 fix (2026-07-09): the old code returned microseconds since the
 * QPC counter's own epoch, i.e. leaked this PC's uptime straight into
 * the game -- RPCS3 sys_time.cpp:191-192 calls this out explicitly
 * ("Add an offset to get_timebased_time to avoid leaking PC's uptime
 * into the game / As if PS3 starts at value 0 (base time) when the game
 * boots"). Cache the QPC frequency once and report elapsed time since a
 * first-call anchor instead of raw QPC. Kill-switch YZ_NO_TIMEANCHOR
 * (shared with sys_timer.c's sys_time_get_current_time anchor) restores
 * the old raw-QPC (host-uptime) behaviour for A/B.
 *
 * 2026-08-04 doc-conformance audit: factored into a callable helper and
 * registered as the lv2 timer subsystem's clock (g_lv2_system_time_us).
 * Lv2 Reference p.255/p.257/p.259 define sys_timer base/expiry times and
 * the event data3 stamp in SYSTEM time -- this clock -- while sys_timer.c
 * compared them against Unix-epoch wall time (~1.75e15 us), so every
 * absolute-base timer fired immediately. */
extern "C" uint64_t yz_guest_system_time_us(void)
{
    static LARGE_INTEGER s_freq;
    static LARGE_INTEGER s_anchor;
    static int s_init = 0;
    static int s_no_anchor = -1;

    if (s_no_anchor < 0) {
        s_no_anchor = getenv("YZ_NO_TIMEANCHOR") ? 1 : 0;
        fprintf(stderr, "[yz_time] system_time armed (anchor-to-boot %s)\n",
                s_no_anchor ? "DISABLED by YZ_NO_TIMEANCHOR" : "on");
        fflush(stderr);
    }

    if (!s_init) {
        QueryPerformanceFrequency(&s_freq);
        QueryPerformanceCounter(&s_anchor);
        s_init = 1;
    }

    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    int64_t d = s_no_anchor ? c.QuadPart : (c.QuadPart - s_anchor.QuadPart);
    return (uint64_t)((d * 1000000) / s_freq.QuadPart);
}

/* Register this clock with runtime/syscalls/sys_timer.c before main() runs
 * (dynamic initializer; import_overrides.cpp is always linked into the
 * runner). */
extern "C" uint64_t (*g_lv2_system_time_us)(void);
static const int s_yz_timer_clock_registered =
    (g_lv2_system_time_us = &yz_guest_system_time_us, 0);

/* Frozen-ticket forensics: hand cellSync's spin diagnostic the SPU
 * line-owner dump (runner-only; unit tests leave the pointer NULL). */
extern "C" void yz_spu_dump_line_owners(unsigned ea);
extern "C" void (*g_yz_spu_line_dump)(unsigned);
static const int s_yz_line_dump_registered =
    (g_yz_spu_line_dump = &yz_spu_dump_line_owners, 0);

extern "C" void yz_ovr_sys_time_get_system_time(ppu_context* ctx)
{
    ctx->gpr[3] = yz_guest_system_time_us();
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_get_id(vm::ptr<u64> id)
 * -----------------------------------------------------------------------*/
extern "C" void yz_ovr_sys_ppu_thread_get_id(ppu_context* ctx)
{
    vm_write64(ctx->gpr[3], (uint64_t)yz_thread_current_id());
    ctx->gpr[3] = 0;
}

/* ---------------------------------------------------------------------------
 * sys_prx: runtime loading of the GAME's own engine PRX modules (pt26).
 *
 * The OgreZ engine sys_prx_load_module's its shader module (pxd_shader,
 * data/module/ps3/ogrez_shader_ps3.ppu.sprx) then sys_prx_start_module's it;
 * with both stubbed ENOSYS the shader subsystem never inits and the render
 * thread spin-waits forever (the post-logo stall). We decrypt + relocate +
 * lift the module statically (tools/decrypt_self.py -> lift_prx -> the lifter;
 * image at 0x02200000), so here we just satisfy the prx ABI: load -> a handle,
 * start -> RUN the module's module_start (it sets up the shader subsystem the
 * engine waits on). pxd_shader exports only module_start/stop and imports 0,
 * so module_start is self-contained and safe to run inline on the caller. */
struct yz_prx_mod { uint32_t handle, start_opd, toc; int started; const char* name; };
static yz_prx_mod g_yz_prx_mods[8];
static int        g_yz_prx_nmods = 0;

static yz_prx_mod* yz_prx_find(uint32_t handle)
{
    for (int i = 0; i < g_yz_prx_nmods; i++)
        if (g_yz_prx_mods[i].handle == handle) return &g_yz_prx_mods[i];
    return nullptr;
}

static const char* yz_guest_cstr(uint32_t gaddr, char* buf, int n)
{
    int i = 0;
    if (gaddr >= 0x10000u && gaddr < 0xE0000000u)
        for (; i < n - 1; i++) { uint8_t c = vm_read8(gaddr + (uint32_t)i); buf[i] = (char)c; if (!c) break; }
    buf[i] = 0;
    return buf;
}

extern "C" void yz_ovr_sys_prx_load_module(ppu_context* ctx)
{
    char path[256];
    yz_guest_cstr((uint32_t)ctx->gpr[3], path, sizeof(path));
    uint32_t handle = 0, start_opd = 0, toc = 0; const char* nm = "?";
    if (strstr(path, "ogrez_shader")) {        /* pxd_shader: lifted + loaded */
        handle = 0x23000001u; start_opd = 0x0266AFD8u; toc = 0x02673020u; nm = "pxd_shader";
    }
    if (!handle) {                              /* not LLE'd yet (e.g. dfengine) */
        fprintf(stderr, "[prx] load_module('%s') -> ENOSYS (module not LLE'd yet)\n", path);
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010003; /* CELL_ENOSYS */
        return;
    }
    if (!yz_prx_find(handle) && g_yz_prx_nmods < 8)
        g_yz_prx_mods[g_yz_prx_nmods++] = { handle, start_opd, toc, 0, nm };
    fprintf(stderr, "[prx] load_module('%s') -> handle 0x%08X (%s)\n", path, handle, nm);
    ctx->gpr[3] = handle;
}

extern "C" void yz_ovr_sys_prx_start_module(ppu_context* ctx)
{
    uint32_t handle = (uint32_t)ctx->gpr[3];
    uint32_t modres = (uint32_t)ctx->gpr[6];   /* int* out: module_start result */
    yz_prx_mod* m = yz_prx_find(handle);
    if (!m) { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010002; return; } /* EINVAL */
    if (!m->started) {
        m->started = 1;
        uint64_t a_args = ctx->gpr[4], a_argp = ctx->gpr[5];   /* start_module(id,args,argp,...) */
        fprintf(stderr, "[prx] start_module 0x%08X: running %s module_start(args=0x%llX argp=0x%llX) opd 0x%08X\n",
                handle, m->name, (unsigned long long)a_args, (unsigned long long)a_argp, m->start_opd);
        ctx->gpr[3] = a_args;   /* module_start(size_t args, void* argp) -- pass the game's */
        ctx->gpr[4] = a_argp;
        yz_call_guest_opd(m->start_opd, ctx);   /* sets r2=module TOC, runs it, drains */
        uint32_t res = (uint32_t)ctx->gpr[3];
        fprintf(stderr, "[prx] %s module_start returned 0x%08X\n", m->name, res);
        if (modres >= 0x10000u && modres < 0xE0000000u) vm_write32(modres, res);
    }
    ctx->gpr[3] = 0;   /* CELL_OK */
}

extern "C" void yz_ovr_sys_prx_stop_module(ppu_context* ctx)     { ctx->gpr[3] = 0; }
extern "C" void yz_ovr_sys_prx_unload_module(ppu_context* ctx)   { ctx->gpr[3] = 0; }
extern "C" void yz_ovr_sys_prx_register_library(ppu_context* ctx){ ctx->gpr[3] = 0; }

/* DRM check: disc content has no NPDRM. Returning ENOSYS made the game take its
 * DRM/trophy error path (-> cellMsgDialogOpen2). Report "available" (CELL_OK) so the
 * title sequence proceeds normally. (pt26 stub-fix test for the post-logo stall.) */
extern "C" void yz_ovr_sceNpDrmIsAvailable(ppu_context* ctx) { ctx->gpr[3] = 0; }

/* Audio output (pt26 — THE post-frame-3 black-screen gate). The OgreZ engine's early
 * init spin-polls cellAudioOutGetState until the output reports ENABLED + CELL_OK
 * before it proceeds (proven: t1.ctr = the cellAudioOutGetState import fake-key
 * 0xFE00018C, looping in func_00B1559C -> usleep). Stubbed ENOSYS => never ready =>
 * deadlock. Report a configured stereo/48kHz LPCM primary output so the engine advances.
 * CellAudioOutState: state u8@0, encoder u8@1, reserved[6], downMixer be32@8, soundMode
 * {type u8@C, channel u8@D, fs u8@E, rsvd u8@F, layout be32@10}. */
extern "C" void yz_ovr_cellAudioOutGetState(ppu_context* ctx)
{
    uint32_t s = (uint32_t)ctx->gpr[5];   /* CellAudioOutState* */
    if (s >= 0x10000u && s < 0xE0000000u) {
        vm_write8(s + 0x0u, 0);      /* state   = CELL_AUDIO_OUT_OUTPUT_STATE_ENABLED (0) */
        vm_write8(s + 0x1u, 0);      /* encoder = CELL_AUDIO_OUT_CODING_TYPE_LPCM (0) */
        for (uint32_t i = 2; i < 8; i++) vm_write8(s + i, 0);   /* reserved[6] */
        vm_write32(s + 0x8u, 0);     /* downMixer = NONE */
        vm_write8(s + 0xCu, 0);      /* soundMode.type    = LPCM */
        vm_write8(s + 0xDu, 2);      /* soundMode.channel = 2 (stereo) */
        vm_write8(s + 0xEu, 0x04u);  /* soundMode.fs      = 48KHz */
        vm_write8(s + 0xFu, 0);      /* soundMode.reserved */
        vm_write32(s + 0x10u, 1);    /* soundMode.layout  = 2CH */
    }
    ctx->gpr[3] = 0;   /* CELL_OK */
}
extern "C" void yz_ovr_cellAudioOutConfigure(ppu_context* ctx) { ctx->gpr[3] = 0; }   /* CELL_OK */

/* ---------------------------------------------------------------------------
 * RSX driver (LLE): sys_rsx syscalls + FIFO consumer
 *
 * Sony's real libgcm_sys (recomp_prx/libgcm_sys_*) now runs the gcm API; the
 * game's cellGcmSys imports bind to its export OPDs (the former hand-rolled
 * gcm HLE below is retired -- yz_gcm_fifo_callback is a dead stub kept only
 * for the dispatch routing). libgcm drives the RSX through the lv2 sys_rsx
 * syscalls (668-677, 0x29C-0x2A5) implemented at the bottom of this file.
 *
 * The driver<->driver contract is a set of GUEST-memory structures, laid out
 * per RPCS3 Emu/Cell/lv2/sys_rsx.{h,cpp} (the oracle, reimplemented):
 *   - context_allocate hands libgcm the dma_control / driver_info / reports
 *     base addresses and fills driver_info (version_driver 0x211 -- libgcm
 *     validates it, libgcm_sys_recomp_000.cpp:770 -- frequencies, offsets).
 *   - the game writes the FIFO PUT offset into dma_control (+0x40); our
 *     consumer reads PUT/GET there and executes the committed command stream.
 *     libgcm owns the ring, so there is no producer/consumer race.
 *   - context_attribute(package_id) carries flip / display-buffer / vblank;
 *     flips publish completion into driver_info.head[].flipFlags (set on the
 *     vblank tick, async, so it survives the game's ResetFlipStatus ordering),
 *     which the game's render loop polls inline.
 * -----------------------------------------------------------------------*/

/* RsxDmaControl: put/get/ref at +0x40/+0x44/+0x48 (0x40-byte reserved prefix).
 * Sony's cellGcmGetControlRegister returns dma_control + 0x40. */
#define RSX_DMACTL_PUT 0x40u
#define RSX_DMACTL_GET 0x44u
#define RSX_DMACTL_REF 0x48u
/* RsxDriverInfo.head[8] base (sizeof RsxDriverInfo 0x12F8, head[8] 0x200, so
 * head starts at 0x10B8); each head is 0x40 bytes, flipFlags at +0x08. */
#define RSX_DRIVERINFO_HEAD 0x10B8u
#define RSX_HEAD_STRIDE     0x40u

/* Context region (dma_control/driver_info/reports/device): placed in the
 * RSX-reserved VM window (0x10000000, reserved-not-committed by vm_init), so
 * it never collides with the game's main-memory heap -- like RPCS3's separate
 * vm::rsx_context. Committed on demand. */
#define RSX_CTX_BASE      0x10000000u
#define RSX_DMA_CONTROL   (RSX_CTX_BASE + 0x000000u)
#define RSX_DRIVER_INFO   (RSX_CTX_BASE + 0x100000u)
#define RSX_REPORTS       (RSX_CTX_BASE + 0x200000u)
#define RSX_DEVICE_ADDR   (RSX_CTX_BASE + 0x300000u)

static uint32_t g_rsx_local_mem_size = YZ_GCM_LOCAL_SIZE;
static int      g_rsx_ctx_ready      = 0;   /* context_allocate done */
static uint32_t g_rsx_event_port     = 0;   /* RSX event port (libgcm handler) */
/* Captured by dispatch.cpp's YZ_TASK_TRACE on cellSpursCreateTask2WithBinInfo:
 * the taskset ea, so the vblank tick can dump the SPURS workload-ready state
 * (1f dispatch diagnostic). */
extern "C" { uint32_t g_yz_spurs_taskset = 0; }
/* pt35: the cri_audio codec taskset (wid 3, e.g. 0x63D22580), captured at
 * CreateTaskWithAttr so we can dump its task_info[] and check whether the codec
 * ELF (0x012B4980) actually got written into the taskset (the elf=0 at StartTask). */
extern "C" { uint32_t g_yz_codec_taskset = 0; }

/* io-offset -> guest EA map, 1 MB granularity (index = io >> 20), built by
 * sys_rsx_context_iomap (replaces the libs cellGcmIoOffsetToAddress the HLE
 * used). 0xFFFFFFFF = unmapped. */
static uint32_t g_rsx_iomap_ea[4096];
static int      g_rsx_iomap_init = 0;

/* HLE gcm (2026-06-14f): EA base of the io command-buffer ring (the ioAddress the
 * game passed to _cellGcmInitBody). The ring maps linearly io X -> yz_gcm_io_addr+X,
 * so ea->io for the ring is `ea - yz_gcm_io_addr`. Set by yz_ovr__cellGcmInitBody. */
static uint32_t yz_gcm_io_addr = 0;
static uint32_t yz_gcm_io_size = 0;
static uint32_t g_yz_gcm_segment_bytes = 0;

static void yz_rsx_iomap_ensure_init(void)
{
    if (!g_rsx_iomap_init) {
        g_rsx_iomap_init = 1;
        for (int i = 0; i < 4096; i++) g_rsx_iomap_ea[i] = 0xFFFFFFFFu;
    }
}

static uint32_t yz_rsx_io_to_ea(uint32_t io)
{
    uint32_t page = io >> 20;
    if (page >= 4096) return 0;
    uint32_t base = g_rsx_iomap_ea[page];
    if (base == 0xFFFFFFFFu) return 0;
    return base + (io & 0xFFFFFu);
}

extern "C" uint32_t yz_nr_vertical_io_to_ea(uint32_t io_offset)
{
    return yz_rsx_io_to_ea(io_offset);
}

static uint32_t yz_rsx_head_addr(uint32_t h)
{
    return RSX_DRIVER_INFO + RSX_DRIVERINFO_HEAD + (h & 7u) * RSX_HEAD_STRIDE;
}

/* TEMP DIAG: the consumer should only ever write RSX regions (local 0xC0..,
 * reports/dma 0x10.., io 0x404..). A write into the game's main memory
 * (0x00010000-0x0FFFFFFF) is corruption -- log it with the target+value. */
static void yz_rsx_w32(uint32_t addr, uint32_t val)
{
    /* Legit consumer targets: local VRAM (0xC0000000+), reports/dma/device
     * (0x10000000-0x10400000), io (0x40400000+). A write into game main memory
     * (0x00010000-0x0FFFFFFF) OR a guest stack (0xD0000000-0xDFFFFFFF) is the
     * consumer derailing -- and a stack hit corrupts a live frame's saved
     * non-volatiles (#19d: r31/r27 came back 0 across _cellGcmInitBody). */
    if ((addr >= 0x00010000u && addr < 0x10000000u) ||
        (addr >= 0xD0000000u && addr < 0xE0000000u)) {
        static int n = 0;
        if (n < 24) { n++;
            fprintf(stderr, "[rsx-corrupt] consumer writes %s 0x%08X = 0x%08X\n",
                    addr >= 0xD0000000u ? "STACK" : "GAME", addr, val);
        }
    }
    vm_write32(addr, val);
}

/* Display buffers registered via context_attribute(0x104). */
struct yz_rsx_dispbuf { uint32_t offset, pitch, width, height; };
static yz_rsx_dispbuf g_rsx_dispbuf[8];
static uint32_t       g_rsx_dispbuf_count;

/* Pending flips per head. A flip is QUEUED by GCM_DRIVER_QUEUE (records the head
 * + buffer) and SUBMITTED by the flip-sema RELEASE (label+0x10 = 0xFFFFFFFF) that
 * follows it; only the RELEASE arms `pending` so the vblank tick can never clear
 * the label before the stream writes 0xFFFFFFFF (avoids a clear/redirty race).
 * The vblank tick then presents the head's buffer + clears the label. */
static volatile long g_rsx_flip_pending[8];
/* Running count of flips manufactured by the SEMARM release-arm heuristic
 * (see NV406E SEMAPHORE_RELEASE below). Nonzero means frame-timing numbers
 * from this run include uncommanded presents. */
extern "C" volatile long g_yz_semarm_count;
volatile long g_yz_semarm_count = 0;
static uint32_t      g_rsx_queued_head = 1;   /* head of the last GCM_DRIVER_QUEUE */

/* Real RSX command translator (libs/video/rsx_commands.c): the consumer
 * delegates 3D rendering methods to it (state tracking + backend clear/draw).
 * State init happens at context_allocate. */
static rsx_state g_rsx_state;

/* ===========================================================================
 * YZ_FLIPTRACE (s21, flip-label stall diagnosis -- STATUS 1a). Uncapped,
 * sequence-stamped event log of the flip-label lifecycle: every arm / clear /
 * acquire / queue event touching the flip semaphore (label 0x10200010 =
 * RSX_REPORTS+0x10) or the HW flip credit (device+0x30), plus a value-
 * transition WATCHER thread that catches writers OUTSIDE the instrumented
 * paths (a plain guest store to the label would otherwise be invisible).
 * All events share one seq counter + QPC clock so cross-thread ORDER is
 * recoverable from the log. Diagnostic only, default OFF.
 * =========================================================================*/
#include <stdarg.h>
static int yz_ft_flag = -1;
static inline int yz_ft_on(void)
{
    if (yz_ft_flag < 0) yz_ft_flag = getenv("YZ_FLIPTRACE") ? 1 : 0;
    return yz_ft_flag;
}
static volatile LONG g_ft_seq = 0;
static double        g_ft_qpf = 0.0;
static LONGLONG      g_ft_t0  = 0;
static void yz_ft(const char* fmt, ...)
{
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (!g_ft_qpf) {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        g_ft_qpf = (double)f.QuadPart; g_ft_t0 = now.QuadPart;
    }
    double ms = (double)(now.QuadPart - g_ft_t0) * 1000.0 / g_ft_qpf;
    char buf[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[ft] #%ld t=%.3f tid=%lu %s\n",
            InterlockedIncrement(&g_ft_seq), ms, GetCurrentThreadId(), buf);
}
/* Label-value watcher: ~ms-grain poll of the flip label; logs every value
 * transition, so an arm/clear from ANY writer (including one no instrumented
 * path covers) appears in the same seq/timestamp stream. Sub-ms pulses can
 * alias between polls -- instrumented writers still log their own events. */
static DWORD WINAPI yz_ft_watch_thread(LPVOID)
{
    uint32_t last = vm_read32(RSX_REPORTS + 0x10);
    yz_ft("WATCHER ARMED label@0x%08X initial=0x%08X", RSX_REPORTS + 0x10, last);
    for (;;) {
        for (int i = 0; i < 64; i++) {
            uint32_t v = vm_read32(RSX_REPORTS + 0x10);
            if (v != last) {
                yz_ft("label 0x%08X -> 0x%08X (watcher) pending=[%ld %ld] qhead=%u",
                      last, v, g_rsx_flip_pending[0], g_rsx_flip_pending[1],
                      g_rsx_queued_head);
                last = v;
            }
            YieldProcessor();
        }
        Sleep(1);
    }
    return 0;
}
static void yz_ft_start(void)   /* called from the vblank tick (ctx is up) */
{
    static int started = 0;
    if (!started && yz_ft_on()) {
        started = 1;
        yz_ft("ARMED (YZ_FLIPTRACE): label=0x%08X devcredit=0x%08X",
              RSX_REPORTS + 0x10, RSX_DEVICE_ADDR + 0x30);
        CreateThread(NULL, 0, yz_ft_watch_thread, NULL, 0, NULL);
    }
}

/* Track B live-draw guest-memory resolver: map an RSX (location, offset) to a
 * host pointer into the guest address space. location 0 = RSX local VRAM (the
 * gcm local carve), 1 = main memory via the gcm io map -- mirrors the DMA cases
 * in yz_rsx_sem_addr. Returns NULL for out-of-range/unmapped regions. */
static const u8* yz_rsx_live_guest_ptr(void* user, u32 location, u32 offset,
                                       u32 min_bytes)
{
    (void)user;
    /*
     * The live renderer promises its callers that the returned host pointer
     * covers min_bytes, so validate the whole interval rather than only its
     * first byte.  Texture hashing exposed the old bug when a mip span began
     * inside local VRAM but ended exactly at 0xCF900000, the first uncommitted
     * byte after the console's 249 MB local-memory carve.
     */
    const uint64_t end = (uint64_t)offset + (uint64_t)min_bytes;
    if (end > 0x100000000ull)
        return nullptr;

    uint32_t ea;
    if (location == 0) {                         /* RSX local VRAM */
        const uint32_t local_size =
            g_rsx_local_mem_size < YZ_GCM_LOCAL_SIZE
                ? g_rsx_local_mem_size : YZ_GCM_LOCAL_SIZE;
        if (offset >= local_size || end > local_size)
            return nullptr;
        ea = YZ_GCM_LOCAL_BASE + offset;
    } else if (location == 1) {                  /* main memory via io map */
        ea = yz_rsx_io_to_ea(offset);
        if (!ea) return nullptr;
        if (min_bytes) {
            const uint64_t last_io64 = end - 1u;
            if (last_io64 > UINT32_MAX)
                return nullptr;
            const uint32_t last_ea =
                yz_rsx_io_to_ea((uint32_t)last_io64);
            if (!last_ea ||
                (uint64_t)last_ea !=
                    (uint64_t)ea + (uint64_t)min_bytes - 1u)
                return nullptr;

            /* IO pages are independently mapped.  Matching endpoints are not
             * sufficient when a multi-page texture crosses a discontiguous or
             * unmapped page in between. */
            uint64_t boundary =
                ((uint64_t)offset + 0x100000ull) & ~0xFFFFFull;
            for (; boundary < end; boundary += 0x100000ull) {
                const uint32_t boundary_ea =
                    yz_rsx_io_to_ea((uint32_t)boundary);
                if (!boundary_ea ||
                    (uint64_t)boundary_ea !=
                        (uint64_t)ea + boundary - offset)
                    return nullptr;
            }
        }
    } else {
        return nullptr;
    }
    return (const u8*)(vm_base + ea);
}

/* Read-only resolver for the explicit passive vertical shader oracle. It
 * shares the live renderer's complete range and IO-map validation instead of
 * maintaining a second interpretation of RSX address spaces. */
extern "C" const u8* yz_nr_vertical_guest_ptr(u32 location, u32 offset,
                                                u32 min_bytes)
{
    return yz_rsx_live_guest_ptr(nullptr, location, offset, min_bytes);
}

extern "C" u8* yz_nr_vertical_guest_writable_ptr(u32 location, u32 offset,
                                                   u32 min_bytes)
{
    return const_cast<u8*>(
        yz_rsx_live_guest_ptr(nullptr, location, offset, min_bytes));
}

extern "C" int yz_nr_vertical_space_range_to_ea(u32 location, u32 offset,
                                                   u32 size, u32* out_ea)
{
    if (!out_ea || !size ||
        !yz_rsx_live_guest_ptr(nullptr, location, offset, size))
        return -1;
    if (location == 0u) {
        *out_ea = YZ_GCM_LOCAL_BASE + offset;
        return 0;
    }
    if (location == 1u) {
        const u32 ea = yz_rsx_io_to_ea(offset);
        const uint64_t last_io64 =
            static_cast<uint64_t>(offset) + size - 1u;
        if (!ea || last_io64 > UINT32_MAX)
            return -1;
        const u32 last = yz_rsx_io_to_ea(static_cast<u32>(last_io64));
        if (!last || static_cast<uint64_t>(last) !=
                         static_cast<uint64_t>(ea) + size - 1u)
            return -1;
        *out_ea = ea;
        return 0;
    }
    return -1;
}

/* Resolve one exact graphics page to its guest EA.  The native renderer arms
 * a VM watch only after this proves the full 4 KiB page is contiguous. */
extern "C" int yz_nr_vertical_space_page_to_ea(u32 location,
                                                 u32 page_offset,
                                                 u32* out_ea)
{
    if ((page_offset & 0xFFFu) ||
        yz_nr_vertical_space_range_to_ea(
            location, page_offset, 0x1000u, out_ea) != 0)
        return -1;
    return (*out_ea & 0xFFFu) == 0u ? 0 : -1;
}

/* The guest movie player owns sequencing and post-movie state, but its decoded
 * surface is not yet reaching the live RSX draw path and its software decode
 * runs far below the authored frame rate. The game opens each .sfd once for a
 * small header validation and again for the actual stream. Preserve validation
 * reads, then hold the playback stream while the host presents it and return
 * EOF only to that stream when presentation is complete. */
static SRWLOCK g_movie_open_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_movie_done_cv = CONDITION_VARIABLE_INIT;
static char g_movie_open_path[MAX_PATH];
static volatile LONG g_movie_open_serial = 0;
static volatile LONG g_movie_completed_serial = 0;
static volatile LONG g_movie_cancelled_serial = 0;
static volatile LONG g_movie_closed_serial = 0;
static LONG g_movie_played_serial = 0;
static volatile LONG g_movie_presenting_serial = 0;
static volatile LONG g_movie_presented_frames = 0;
static volatile LONG g_movie_present_timescale = 30;
static volatile LONG g_movie_playing_observed_serial = 0;
/* Consumed by dispatch.cpp on guest PPU thread 1. The host presenter may
 * detect EOS, but CRI player lifecycle calls must execute on the guest thread
 * that owns the mwPly handle. */
extern "C" volatile long g_yz_movie_stop_pending_serial = 0;
extern "C" volatile long g_yz_movie_stop_applied_serial = 0;

/* Process-wide phase bit for the CRI worker fairness shim.  It is armed by the
 * confirmed title-to-menu callback in dispatch.cpp; movie playback is too
 * early because Dolby/title setup still uses the preload scheduling pattern. */
extern "C" volatile LONG g_yz_cri_yield_phase = 0;

static void yz_movie_request_cancel(LONG serial, const char* reason);

/* mwPly HLE control surface. Unlike the retired fd/read bridge, this is keyed
 * directly by the game's Start/Stop lifecycle and never manufactures Stop. */
extern "C" long yz_host_movie_start(const char* host_path)
{
    if (!host_path || !*host_path || !rsx_live_draw_enabled() ||
        !movie_ffmpeg_available())
        return 0;
    AcquireSRWLockExclusive(&g_movie_open_lock);
    strncpy(g_movie_open_path, host_path, sizeof(g_movie_open_path) - 1);
    g_movie_open_path[sizeof(g_movie_open_path) - 1] = '\0';
    const LONG serial = InterlockedIncrement(&g_movie_open_serial);
    InterlockedExchange(&g_movie_presented_frames, 0);
    InterlockedExchange(&g_movie_present_timescale, 30);
    ReleaseSRWLockExclusive(&g_movie_open_lock);
    fprintf(stderr, "[movie] mwPly queued direct host playback serial=%ld '%s'\n",
            serial, host_path);
    fflush(stderr);
    return serial;
}

extern "C" void yz_host_movie_stop(long serial)
{
    yz_movie_request_cancel((LONG)serial, "game mwPly Stop/RequestStop");
}

extern "C" int yz_host_movie_status(long serial)
{
    if (serial <= 0) return 0;
    if (InterlockedCompareExchange(&g_movie_completed_serial, 0, 0) >= serial)
        return 3; /* MWSFD_STAT_PLAYEND */
    if (InterlockedCompareExchange(&g_movie_presenting_serial, 0, 0) == serial) {
        InterlockedExchange(&g_movie_playing_observed_serial, serial);
        return 2; /* MWSFD_STAT_PLAYING */
    }
    return 1;     /* MWSFD_STAT_PREP */
}

extern "C" void yz_host_movie_time(long serial, uint32_t* count,
                                    uint32_t* scale)
{
    if (!count || !scale || serial <= 0) return;
    const LONG timescale =
        InterlockedCompareExchange(&g_movie_present_timescale, 0, 0);
    uint64_t audio_frames = cellAudioHostStreamPositionFrames();
    if (audio_frames) {
        *scale = timescale > 0 ? (uint32_t)timescale : 30u;
        *count = (uint32_t)((audio_frames * *scale) / 48000u);
    } else {
        *scale = timescale > 0 ? (uint32_t)timescale : 30u;
        *count = (uint32_t)InterlockedCompareExchange(
            &g_movie_presented_frames, 0, 0);
    }
}

#define YZ_MOVIE_FD_SLOTS 64
struct yz_movie_fd_state {
    int active;
    int validation;
    unsigned eof_deliveries;
    LONG serial;
    char host_path[MAX_PATH];
};
static yz_movie_fd_state g_movie_fds[YZ_MOVIE_FD_SLOTS];

static void yz_movie_request_cancel(LONG serial, const char* reason)
{
    if (serial <= 0)
        return;
    AcquireSRWLockExclusive(&g_movie_open_lock);
    if (serial > g_movie_completed_serial && serial > g_movie_cancelled_serial) {
        InterlockedExchange(&g_movie_cancelled_serial, serial);
        fprintf(stderr, "[movie] cancel requested serial=%ld (%s)\n",
                serial, reason ? reason : "unspecified");
        fflush(stderr);
    }
    ReleaseSRWLockExclusive(&g_movie_open_lock);
}

static int yz_has_sfd_suffix(const char* path)
{
    if (!path) return 0;
    const size_t n = strlen(path);
    return n >= 4 && _stricmp(path + n - 4, ".sfd") == 0;
}

extern "C" int yz_movie_hle_armed(void);
extern "C" void yz_stage_direct_probe_snapshot(const char* reason);

static bool yz_stage_host_region_readable(const MEMORY_BASIC_INFORMATION& mbi)
{
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD))
        return false;
    const DWORD p = mbi.Protect & 0xFFu;
    return p == PAGE_READONLY || p == PAGE_READWRITE ||
           p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READ ||
           p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

static unsigned yz_stage_find_bytes(const void* needle, size_t needle_size,
                                    uint32_t* found, unsigned found_cap)
{
    static const uint32_t ranges[][2] = {
        {0x00010000u, 0x10010000u}, /* committed 256 MiB main memory */
        {0x40000000u, 0x80000000u}, /* sys_memory/sys_vm allocations */
    };
    unsigned count = 0;
    for (const auto& range : ranges) {
        uint8_t* cursor = vm_base + range[0];
        uint8_t* limit = vm_base + range[1];
        while (cursor < limit) {
            MEMORY_BASIC_INFORMATION mbi = {};
            if (!VirtualQuery(cursor, &mbi, sizeof(mbi)))
                break;
            uint8_t* region_begin = (uint8_t*)mbi.BaseAddress;
            uint8_t* region_end = region_begin + mbi.RegionSize;
            uint8_t* scan = region_begin > cursor ? region_begin : cursor;
            uint8_t* scan_end = region_end < limit ? region_end : limit;
            if (yz_stage_host_region_readable(mbi) &&
                scan_end > scan &&
                (size_t)(scan_end - scan) >= needle_size) {
                const uint8_t first = *(const uint8_t*)needle;
                while (scan + needle_size <= scan_end) {
                    uint8_t* hit = (uint8_t*)memchr(
                        scan, first, (size_t)(scan_end - scan));
                    if (!hit || hit + needle_size > scan_end)
                        break;
                    if (memcmp(hit, needle, needle_size) == 0) {
                        if (count < found_cap)
                            found[count] = (uint32_t)(hit - vm_base);
                        count++;
                    }
                    scan = hit + 1;
                }
            }
            cursor = region_end > cursor ? region_end : cursor + 0x1000;
        }
    }
    return count;
}

static unsigned yz_stage_find_pointer(uint32_t value, uint32_t* found,
                                      unsigned found_cap)
{
    uint8_t be[4] = {
        (uint8_t)(value >> 24), (uint8_t)(value >> 16),
        (uint8_t)(value >> 8), (uint8_t)value
    };
    uint32_t byte_hits[256] = {};
    const unsigned total = yz_stage_find_bytes(
        be, sizeof(be), byte_hits,
        sizeof(byte_hits) / sizeof(byte_hits[0]));
    unsigned count = 0;
    const unsigned retained =
        total < sizeof(byte_hits) / sizeof(byte_hits[0])
            ? total : (unsigned)(sizeof(byte_hits) / sizeof(byte_hits[0]));
    for (unsigned i = 0; i < retained; i++) {
        if ((byte_hits[i] & 3u) != 0)
            continue;
        if (count < found_cap)
            found[count] = byte_hits[i];
        count++;
    }
    return count;
}

/* Find aligned guest pointers while discarding references stored inside the
 * converted GMD itself.  The original two-stage helper retained only its first
 * 256 byte matches, which for the house were all self-references and hid the
 * manager/instance owners that follow them in memory. */
static unsigned yz_stage_find_external_pointer(
    uint32_t value, uint32_t excluded_begin, uint32_t excluded_size,
    uint32_t* found, unsigned found_cap)
{
    static const uint32_t ranges[][2] = {
        {0x00010000u, 0x10010000u},
        {0x40000000u, 0x80000000u},
    };
    const uint8_t be[4] = {
        (uint8_t)(value >> 24), (uint8_t)(value >> 16),
        (uint8_t)(value >> 8), (uint8_t)value
    };
    const uint64_t excluded_end =
        (uint64_t)excluded_begin + (uint64_t)excluded_size;
    unsigned count = 0;

    for (const auto& range : ranges) {
        uint8_t* cursor = vm_base + range[0];
        uint8_t* limit = vm_base + range[1];
        while (cursor < limit) {
            MEMORY_BASIC_INFORMATION mbi = {};
            if (!VirtualQuery(cursor, &mbi, sizeof(mbi)))
                break;
            uint8_t* region_begin = (uint8_t*)mbi.BaseAddress;
            uint8_t* region_end = region_begin + mbi.RegionSize;
            uint8_t* scan = region_begin > cursor ? region_begin : cursor;
            uint8_t* scan_end = region_end < limit ? region_end : limit;
            if (yz_stage_host_region_readable(mbi) &&
                scan_end >= scan + sizeof(be)) {
                uintptr_t guest =
                    (uintptr_t)(scan - vm_base);
                scan += (4u - (guest & 3u)) & 3u;
                for (; scan + sizeof(be) <= scan_end; scan += 4u) {
                    if (memcmp(scan, be, sizeof(be)) != 0)
                        continue;
                    const uint32_t address =
                        (uint32_t)(scan - vm_base);
                    if ((uint64_t)address >= excluded_begin &&
                        (uint64_t)address < excluded_end)
                        continue;
                    if (count < found_cap)
                        found[count] = address;
                    ++count;
                }
            }
            cursor = region_end > cursor
                ? region_end : cursor + 0x1000;
        }
    }
    return count;
}

extern "C" uint32_t g_yz_game_toc;
extern "C" volatile uint32_t g_yz_stage_house_raw;
extern "C" volatile uint32_t g_yz_stage_house_allocation;
extern "C" volatile uint32_t g_yz_stage_house_bytes;
extern "C" volatile uint32_t g_yz_stage_house_slot;
extern "C" volatile uint32_t g_yz_stage_house_published;
extern "C" volatile uint32_t g_yz_stage_house_instance = 0;
extern "C" void yz_watch_wr_rearm(void);

extern "C" void yz_stage_house_external_snapshot(void)
{
    /*
     * YZ_WATCH_WR may target a dynamic stage allocation.  The heap can
     * recommit that page after the early main() arm, restoring write access.
     * Re-arm independently of the much heavier YZ_STAGE_MEMORY snapshot.
     */
    if (getenv("YZ_WATCH_WR"))
        yz_watch_wr_rearm();
    if (!getenv("YZ_STAGE_MEMORY"))
        return;

    const uint32_t converted =
        (uint32_t)InterlockedCompareExchange(
            (volatile LONG*)&g_yz_stage_house_allocation, 0, 0);
    const uint32_t converted_bytes =
        (uint32_t)InterlockedCompareExchange(
            (volatile LONG*)&g_yz_stage_house_bytes, 0, 0);
    if (!converted || !converted_bytes)
        return;

    static LONG once;
    if (InterlockedCompareExchange(&once, 1, 0) != 0)
        return;

    uint32_t refs[1024] = {};
    const unsigned ref_count = yz_stage_find_external_pointer(
        converted, converted, converted_bytes, refs,
        sizeof(refs) / sizeof(refs[0]));
    const uint32_t table =
        g_yz_game_toc
            ? vm_read32(g_yz_game_toc - 0x7680u) : 0u;
    fprintf(stderr,
            "[stage-owner] house=%08X bytes=%08X table=%08X "
            "externalRefs=%u\n",
            converted, converted_bytes, table, ref_count);

    const unsigned retained =
        ref_count < sizeof(refs) / sizeof(refs[0])
            ? ref_count
            : (unsigned)(sizeof(refs) / sizeof(refs[0]));
    for (unsigned i = 0; i < retained; ++i) {
        const uint32_t ref = refs[i];
        const bool in_table =
            table && ref >= table && ref < table + 0x4000u;
        const uint32_t object =
            !in_table && (ref & 0xFFFu) >= 4u ? ref - 4u : 0u;
        const uint32_t vtable = object ? vm_read32(object) : 0u;
        if (vtable == 0x011D6948u)
            InterlockedExchange(
                (volatile LONG*)&g_yz_stage_house_instance,
                (LONG)object);
        fprintf(stderr,
                "[stage-owner] ref=%08X%s object=%08X vtable=%08X",
                ref, in_table ? " TABLE" : "", object, vtable);
        if (in_table)
            fprintf(stderr, " slot=%u",
                    (unsigned)((ref - table) / 4u));

        if (vtable == 0x011D6948u) {
            fprintf(stderr,
                    " words="
                    "%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X "
                    "b0=%04X b2=%02X b3=%02X "
                    "f0=%08X f4=%08X f8=%08X fc=%08X "
                    "f168=%08X f16c=%08X f170=%08X",
                    vm_read32(object + 0x00u),
                    vm_read32(object + 0x04u),
                    vm_read32(object + 0x08u),
                    vm_read32(object + 0x0Cu),
                    vm_read32(object + 0x10u),
                    vm_read32(object + 0x14u),
                    vm_read32(object + 0x18u),
                    vm_read32(object + 0x1Cu),
                    (unsigned)vm_read16(object + 0xB0u),
                    (unsigned)vm_read8(object + 0xB2u),
                    (unsigned)vm_read8(object + 0xB3u),
                    vm_read32(object + 0xF0u),
                    vm_read32(object + 0xF4u),
                    vm_read32(object + 0xF8u),
                    vm_read32(object + 0xFCu),
                    vm_read32(object + 0x168u),
                    vm_read32(object + 0x16Cu),
                    vm_read32(object + 0x170u));

            uint32_t owner_refs[256] = {};
            const unsigned owner_ref_count =
                yz_stage_find_external_pointer(
                    object, object, 0x180u, owner_refs,
                    sizeof(owner_refs) / sizeof(owner_refs[0]));
            fprintf(stderr, " ownerRefs=%u", owner_ref_count);
            const unsigned retained_owner =
                owner_ref_count <
                    sizeof(owner_refs) / sizeof(owner_refs[0])
                    ? owner_ref_count
                    : (unsigned)(sizeof(owner_refs) /
                                 sizeof(owner_refs[0]));
            for (unsigned j = 0; j < retained_owner && j < 16u; ++j)
                fprintf(stderr, " ownerRef%u=%08X",
                        j, owner_refs[j]);
        }
        fputc('\n', stderr);
    }

    /* Enumerate the sibling static-stage render objects as a control group.
     * If F11/F10/F0D have an instance but F12 does not, construction or
     * registration is the first broken checkpoint; if F12 exists, its mask
     * and owner links can be compared directly with those live siblings. */
    uint32_t instances[512] = {};
    const unsigned instance_count = yz_stage_find_pointer(
        0x011D6948u, instances,
        sizeof(instances) / sizeof(instances[0]));
    fprintf(stderr,
            "[stage-owner] stage-instance-vtable matches=%u\n",
            instance_count);
    const unsigned retained_instances =
        instance_count < sizeof(instances) / sizeof(instances[0])
            ? instance_count
            : (unsigned)(sizeof(instances) / sizeof(instances[0]));
    for (unsigned i = 0; i < retained_instances; ++i) {
        const uint32_t object = instances[i];
        const uint32_t gmd = vm_read32(object + 0x04u);
        unsigned table_slot = 0xFFFFFFFFu;
        if (table) {
            for (unsigned slot = 0; slot < 4096u; ++slot) {
                if (vm_read32(table + slot * 4u) == gmd) {
                    table_slot = slot;
                    break;
                }
            }
        }
        if (table_slot != 0xF0Du && table_slot != 0xF10u &&
            table_slot != 0xF11u && table_slot != 0xF12u)
            continue;
        fprintf(stderr,
                "[stage-owner] stage-instance object=%08X "
                "gmd=%08X slot=%08X mask=%08X "
                "f0C=%08X f10=%08X f14=%08X f18=%08X "
                "f1C=%08X f20=%08X f24=%08X "
                "b0=%04X b2=%02X b3=%02X\n",
                object, gmd, table_slot,
                vm_read32(object + 0x08u),
                vm_read32(object + 0x0Cu),
                vm_read32(object + 0x10u),
                vm_read32(object + 0x14u),
                vm_read32(object + 0x18u),
                vm_read32(object + 0x1Cu),
                vm_read32(object + 0x20u),
                vm_read32(object + 0x24u),
                (unsigned)vm_read16(object + 0xB0u),
                (unsigned)vm_read8(object + 0xB2u),
                (unsigned)vm_read8(object + 0xB3u));
    }
    yz_watch_wr_rearm();
    fflush(stderr);
}

static void yz_stage_house_memory_snapshot(void)
{
    if (!getenv("YZ_STAGE_MEMORY"))
        return;

    static LONG once;
    if (InterlockedCompareExchange(&once, 1, 0) != 0)
        return;

    static const char house_name[] = "st_asagao_e_house";
    uint32_t names[16] = {};
    const unsigned name_count = yz_stage_find_bytes(
        house_name, sizeof(house_name) - 1, names,
        sizeof(names) / sizeof(names[0]));
    fprintf(stderr, "[stage-memory] house-name matches=%u\n", name_count);

    const unsigned retained_names =
        name_count < sizeof(names) / sizeof(names[0])
            ? name_count : (unsigned)(sizeof(names) / sizeof(names[0]));
    for (unsigned i = 0; i < retained_names; i++) {
        const uint32_t name = names[i];
        const uint32_t header = name >= 0x12u ? name - 0x12u : 0u;
        fprintf(stderr,
                "[stage-memory] house-name=%08X candidate-header=%08X "
                "tag=%08X pre10=%08X pre0C=%08X\n",
                name, header, header ? vm_read32(header) : 0u,
                name >= 0x10u ? vm_read32(name - 0x10u) : 0u,
                name >= 0xCu ? vm_read32(name - 0xCu) : 0u);
        if (!header || memcmp(vm_base + header, "GSGM", 4) != 0)
            continue;

        uint32_t header_refs[64] = {};
        const unsigned header_ref_count = yz_stage_find_pointer(
            header, header_refs,
            sizeof(header_refs) / sizeof(header_refs[0]));
        fprintf(stderr,
                "[stage-memory] house header=%08X refs=%u "
                "u16@32=%u\n",
                header, header_ref_count,
                (unsigned)vm_read16(header + 0x32u));

        const unsigned retained_refs =
            header_ref_count < sizeof(header_refs) / sizeof(header_refs[0])
                ? header_ref_count
                : (unsigned)(sizeof(header_refs) / sizeof(header_refs[0]));
        for (unsigned j = 0; j < retained_refs; j++) {
            const uint32_t data_field = header_refs[j];
            if (data_field < 4u)
                continue;
            const uint32_t resource = data_field - 4u;
            const uint32_t resource_vtable = vm_read32(resource);
            fprintf(stderr,
                    "[stage-memory] header-ref=%08X resource=%08X "
                    "vtable=%08X f08=%08X f0C=%08X\n",
                    data_field, resource, resource_vtable,
                    vm_read32(resource + 8u), vm_read32(resource + 0xCu));
            if (resource_vtable != 0x011F7330u)
                continue;

            uint32_t resource_refs[128] = {};
            const unsigned resource_ref_count = yz_stage_find_pointer(
                resource, resource_refs,
                sizeof(resource_refs) / sizeof(resource_refs[0]));
            fprintf(stderr,
                    "[stage-memory] resource=%08X refs=%u\n",
                    resource, resource_ref_count);
            const unsigned retained_resource_refs =
                resource_ref_count <
                    sizeof(resource_refs) / sizeof(resource_refs[0])
                    ? resource_ref_count
                    : (unsigned)(sizeof(resource_refs) /
                                 sizeof(resource_refs[0]));
            for (unsigned k = 0; k < retained_resource_refs; k++) {
                const uint32_t resource_field = resource_refs[k];
                if (resource_field < 0x20Cu)
                    continue;
                const uint32_t model = resource_field - 0x20Cu;
                fprintf(stderr,
                        "[stage-memory] model-candidate=%08X "
                        "resource-field=%08X vtable=%08X "
                        "f030=%02X f0C0=%08X f208=%08X "
                        "f20C=%08X f218=%08X f248=%08X\n",
                        model, resource_field, vm_read32(model),
                        (unsigned)vm_read8(model + 0x30u),
                        vm_read32(model + 0xC0u),
                        vm_read32(model + 0x208u),
                        vm_read32(model + 0x20Cu),
                        vm_read32(model + 0x218u),
                        vm_read32(model + 0x248u));
            }
        }
    }

    /* This 32-byte index run begins at file offset 0x8BFB28 in the house GMD.
     * It is absent from the other three large stage models and survives the
     * healthy capture's mesh conversion in shorter runs, so it can locate
     * retained house geometry even if the parser released its file header. */
    static const uint8_t house_index_run[] = {
        0x00,0x6B, 0x00,0x6C, 0x00,0x6D, 0x00,0x6E,
        0x00,0x6F, 0x00,0x72, 0x00,0x70, 0x00,0x71,
        0x00,0x71, 0x00,0x73, 0x00,0x72, 0x00,0x74,
        0x00,0x72, 0x00,0x73, 0x00,0x73, 0x00,0x75
    };
    uint32_t index_hits[32] = {};
    const unsigned index_count = yz_stage_find_bytes(
        house_index_run, sizeof(house_index_run), index_hits,
        sizeof(index_hits) / sizeof(index_hits[0]));
    fprintf(stderr, "[stage-memory] house-index matches=%u\n", index_count);
    const unsigned retained_index =
        index_count < sizeof(index_hits) / sizeof(index_hits[0])
            ? index_count
            : (unsigned)(sizeof(index_hits) / sizeof(index_hits[0]));
    for (unsigned i = 0; i < retained_index; i++) {
        const uint32_t hit = index_hits[i];
        const uint32_t candidate_base =
            hit >= 0x008BFB28u ? hit - 0x008BFB28u : 0u;
        fprintf(stderr,
                "[stage-memory] house-index=%08X candidate-base=%08X "
                "tag=%08X\n",
                hit, candidate_base,
                candidate_base ? vm_read32(candidate_base) : 0u);
        if (!candidate_base ||
            vm_read32(candidate_base) != 0x4753474Du)
            continue;

        for (uint32_t off = 0; off < 0x100u; off += 0x20u) {
            fprintf(stderr,
                    "[stage-memory] house-gsgm +%03X:"
                    " %08X %08X %08X %08X %08X %08X %08X %08X\n",
                    off,
                    vm_read32(candidate_base + off + 0x00u),
                    vm_read32(candidate_base + off + 0x04u),
                    vm_read32(candidate_base + off + 0x08u),
                    vm_read32(candidate_base + off + 0x0Cu),
                    vm_read32(candidate_base + off + 0x10u),
                    vm_read32(candidate_base + off + 0x14u),
                    vm_read32(candidate_base + off + 0x18u),
                    vm_read32(candidate_base + off + 0x1Cu));
        }

        uint32_t base_refs[256] = {};
        const unsigned base_ref_count = yz_stage_find_pointer(
            candidate_base, base_refs,
            sizeof(base_refs) / sizeof(base_refs[0]));
        uint32_t index_refs[256] = {};
        const unsigned index_ref_count = yz_stage_find_pointer(
            hit, index_refs,
            sizeof(index_refs) / sizeof(index_refs[0]));
        const uint32_t table =
            g_yz_game_toc
                ? vm_read32(g_yz_game_toc - 0x7680u) : 0u;
        fprintf(stderr,
                "[stage-memory] house-gsgm base=%08X baseRefs=%u "
                "index=%08X indexRefs=%u toc=%08X table=%08X\n",
                candidate_base, base_ref_count, hit, index_ref_count,
                g_yz_game_toc, table);

        const unsigned retained_base_refs =
            base_ref_count <
                sizeof(base_refs) / sizeof(base_refs[0])
                    ? base_ref_count
                    : (unsigned)(sizeof(base_refs) /
                                 sizeof(base_refs[0]));
        for (unsigned j = 0; j < retained_base_refs; ++j) {
            const uint32_t ref = base_refs[j];
            const bool in_table =
                table && ref >= table && ref < table + 0x4000u;
            fprintf(stderr,
                    "[stage-memory] house-base-ref=%08X%s",
                    ref, in_table ? " TABLE" : "");
            if (in_table)
                fprintf(stderr, " slot=%u",
                        (unsigned)((ref - table) / 4u));
            if (ref >= 8u) {
                fprintf(stderr,
                        " context[-8..+28]="
                        "%08X/%08X/%08X/%08X/%08X/%08X/"
                        "%08X/%08X/%08X/%08X/%08X/%08X/%08X",
                        vm_read32(ref - 0x08u),
                        vm_read32(ref - 0x04u),
                        vm_read32(ref + 0x00u),
                        vm_read32(ref + 0x04u),
                        vm_read32(ref + 0x08u),
                        vm_read32(ref + 0x0Cu),
                        vm_read32(ref + 0x10u),
                        vm_read32(ref + 0x14u),
                        vm_read32(ref + 0x18u),
                        vm_read32(ref + 0x1Cu),
                        vm_read32(ref + 0x20u),
                        vm_read32(ref + 0x24u),
                        vm_read32(ref + 0x28u));
            }
            fprintf(stderr, "\n");
        }
        const unsigned retained_index_refs =
            index_ref_count <
                sizeof(index_refs) / sizeof(index_refs[0])
                    ? index_ref_count
                    : (unsigned)(sizeof(index_refs) /
                                 sizeof(index_refs[0]));
        for (unsigned j = 0; j < retained_index_refs; ++j) {
            const uint32_t ref = index_refs[j];
            const bool in_table =
                table && ref >= table && ref < table + 0x4000u;
            fprintf(stderr,
                    "[stage-memory] house-index-ref=%08X%s",
                    ref, in_table ? " TABLE" : "");
            if (in_table)
                fprintf(stderr, " slot=%u",
                        (unsigned)((ref - table) / 4u));
            fprintf(stderr, "\n");
        }

        if (table) {
            unsigned exact_slots = 0;
            for (unsigned slot = 0; slot < 4096u; ++slot) {
                const uint32_t value = vm_read32(table + slot * 4u);
                if (value != candidate_base && value != hit)
                    continue;
                fprintf(stderr,
                        "[stage-memory] house-table slot=%u value=%08X "
                        "kind=%s\n",
                        slot, value,
                        value == candidate_base ? "base" : "index");
                ++exact_slots;
            }
            fprintf(stderr,
                    "[stage-memory] house-table exactSlots=%u\n",
                    exact_slots);
        }

        const uint32_t converted =
            (uint32_t)InterlockedCompareExchange(
                (volatile LONG*)&g_yz_stage_house_allocation, 0, 0);
        if (converted) {
            const uint32_t converted_bytes =
                (uint32_t)InterlockedCompareExchange(
                    (volatile LONG*)&g_yz_stage_house_bytes, 0, 0);
            uint32_t converted_refs[1024] = {};
            const unsigned converted_ref_count =
                yz_stage_find_external_pointer(
                    converted, converted, converted_bytes,
                    converted_refs,
                    sizeof(converted_refs) /
                        sizeof(converted_refs[0]));
            fprintf(stderr,
                    "[stage-memory] house-converted raw=%08X "
                    "allocation=%08X bytes=%08X slot=%08X "
                    "published=%08X externalRefs=%u\n",
                    (uint32_t)InterlockedCompareExchange(
                        (volatile LONG*)&g_yz_stage_house_raw, 0, 0),
                    converted, converted_bytes,
                    (uint32_t)InterlockedCompareExchange(
                        (volatile LONG*)&g_yz_stage_house_slot, 0, 0),
                    (uint32_t)InterlockedCompareExchange(
                        (volatile LONG*)&g_yz_stage_house_published, 0, 0),
                    converted_ref_count);
            const unsigned retained_converted =
                converted_ref_count <
                    sizeof(converted_refs) /
                        sizeof(converted_refs[0])
                    ? converted_ref_count
                    : (unsigned)(sizeof(converted_refs) /
                                 sizeof(converted_refs[0]));
            for (unsigned j = 0; j < retained_converted; ++j) {
                const uint32_t ref = converted_refs[j];
                const bool in_table =
                    table && ref >= table && ref < table + 0x4000u;
                fprintf(stderr,
                        "[stage-memory] house-converted-ref=%08X%s",
                        ref, in_table ? " TABLE" : "");
                if (in_table)
                    fprintf(stderr, " slot=%u",
                            (unsigned)((ref - table) / 4u));
                if (ref >= 0x10u) {
                    fprintf(stderr,
                            " context[-10..+20]="
                            "%08X/%08X/%08X/%08X/"
                            "%08X/%08X/%08X/%08X/"
                            "%08X/%08X/%08X/%08X/%08X",
                            vm_read32(ref - 0x10u),
                            vm_read32(ref - 0x0Cu),
                            vm_read32(ref - 0x08u),
                            vm_read32(ref - 0x04u),
                            vm_read32(ref + 0x00u),
                            vm_read32(ref + 0x04u),
                            vm_read32(ref + 0x08u),
                            vm_read32(ref + 0x0Cu),
                            vm_read32(ref + 0x10u),
                            vm_read32(ref + 0x14u),
                            vm_read32(ref + 0x18u),
                            vm_read32(ref + 0x1Cu),
                            vm_read32(ref + 0x20u));
                }
                fprintf(stderr, "\n");
            }
        }
    }
    fflush(stderr);
}

static void yz_movie_open_hook(CellFsFd fd, const char* guest_path,
                               const char* host_path)
{
#if defined(YZ_PERF_CLEAN)
    const int a010_auth_diag = 0;
    const int a010_release_trace = 0;
#else
    const int a010_auth_diag = getenv("YZ_A010_AUTH") != nullptr;
    const int a010_release_trace =
        getenv("YZ_A010_RELEASE_TRACE") != nullptr;
#endif
    /*
     * The release-scene lifetime is behavior state used by the clean FIFO
     * publication repair.  It must not disappear when observation-only
     * release tracing is compiled out or left disabled.
     */
    if (guest_path) {
        if (strstr(guest_path, "/auth/a010/a010.par")) {
#if defined(YZ_PERF_CLEAN)
            InterlockedExchange(&g_yz_a010_root_active, 1);
            InterlockedExchange(&g_yz_a010_animation_ready, 0);
#endif
            if (InterlockedCompareExchange(
                    &g_yz_a010_release_scene_active, 1, 0) == 0) {
                if (a010_release_trace) {
                    fprintf(stderr,
                            "[a010-reltrace] SCENE BEGIN on open '%s'\n",
                            guest_path);
                    fflush(stderr);
                }
            }
        } else if (strstr(guest_path, "/movie/a020.sfd") ||
                   strstr(guest_path, "/auth/a020/a020.par")) {
#if defined(YZ_PERF_CLEAN)
            InterlockedExchange(&g_yz_a010_root_active, 0);
#endif
            /* The compact lifecycle catcher must remain armed after a020:
             * the rare terminal stopper has also appeared much later in the
             * FIFO.  This is observation-only and is opt-in. */
            if (yz_a010_reltrace_targeted()) {
                if (InterlockedCompareExchange(
                        &g_yz_a010_release_scene_active, 0, 0) != 0) {
                    fprintf(stderr,
                            "[a010-reltrace] TARGETED lifetime retained on "
                            "open '%s'\n",
                            guest_path);
                    fflush(stderr);
                }
            } else if (InterlockedExchange(
                           &g_yz_a010_release_scene_active, 0) != 0) {
                if (a010_release_trace) {
                    fprintf(stderr,
                            "[a010-reltrace] SCENE END on open '%s'\n",
                            guest_path);
                    fflush(stderr);
                }
            }
        }
    }
#if !defined(YZ_PERF_CLEAN)
    if ((getenv("YZ_A010_ROOT") || getenv("YZ_A010_REQ") ||
         a010_auth_diag) && guest_path) {
        if (strstr(guest_path, "/auth/a010/a010.par")) {
            if (InterlockedCompareExchange(&g_yz_a010_root_active, 1, 0) == 0) {
                InterlockedExchange(&g_yz_a010_animation_ready, 0);
                fprintf(stderr, "[a010-root] BEGIN on open '%s'\n", guest_path);
                fflush(stderr);
                yz_stage_direct_probe_snapshot("a010-root-begin");
                yz_stage_house_memory_snapshot();
            }
        } else if ((!a010_auth_diag &&
                    (strstr(guest_path, "/movie/a020.sfd") ||
                     strstr(guest_path, "/auth/a020/a020.par"))) ||
                   (a010_auth_diag &&
                    (strstr(guest_path, "/movie/a030.sfd") ||
                     strstr(guest_path, "/auth/a030/a030.par")))) {
            if (InterlockedExchange(&g_yz_a010_root_active, 0) != 0) {
                InterlockedExchange(&g_yz_a010_animation_ready, 0);
                fprintf(stderr, "[a010-root] END on open '%s'\n", guest_path);
                fflush(stderr);
            }
        }
    }
#endif
    /* Arm the bounded live-renderer capture at the authoritative scene-file
     * boundary.  This runs before the Route-1 early return because a010 is an
     * in-engine AUTH scene, not an SFD movie. */
    if ((getenv("YZ_RSX_A010_PROBE") ||
         getenv("YZ_RSX_A010_SURFACE_DUMP")) && guest_path &&
        strstr(guest_path, "/auth/a010/a010.par")) {
        static volatile LONG a010_probe_armed = 0;
        if (InterlockedCompareExchange(&a010_probe_armed, 1, 0) == 0) {
            fprintf(stderr, "[a010-probe] arming on open '%s'\n", guest_path);
            rsx_live_draw_a010_probe_begin();
        }
    }
    if (yz_movie_hle_armed())
        return;                 /* Route-1 HLE owns movies; fd-bridge idle */
    if (fd < 0 || fd >= YZ_MOVIE_FD_SLOTS)
        return;

    AcquireSRWLockExclusive(&g_movie_open_lock);
    memset(&g_movie_fds[fd], 0, sizeof(g_movie_fds[fd]));
    if (yz_has_sfd_suffix(guest_path) && host_path && *host_path &&
        !getenv("YZ_NO_MOVIE_PRESENT")) {
        g_movie_fds[fd].active = 1;
        strncpy(g_movie_fds[fd].host_path, host_path,
                sizeof(g_movie_fds[fd].host_path) - 1);
    }
    ReleaseSRWLockExclusive(&g_movie_open_lock);
}

static void yz_movie_close_hook(CellFsFd fd, const char* guest_path)
{
    (void)guest_path;
    if (fd < 0 || fd >= YZ_MOVIE_FD_SLOTS)
        return;
    AcquireSRWLockExclusive(&g_movie_open_lock);
    const LONG serial = g_movie_fds[fd].serial;
    memset(&g_movie_fds[fd], 0, sizeof(g_movie_fds[fd]));
    ReleaseSRWLockExclusive(&g_movie_open_lock);
    if (serial > 0) {
        if (serial > InterlockedCompareExchange(&g_movie_closed_serial, 0, 0))
            InterlockedExchange(&g_movie_closed_serial, serial);
        cellPad_host_movie_skip_end();
        fprintf(stderr, "[movie] guest Close completed fd=%d serial=%ld\n",
                fd, serial);
        fflush(stderr);
    }
    /* Native Stop/Close cancels an outstanding asynchronous source operation;
     * the host decoder observes this request, tears down its output surface,
     * and only then publishes completion to the blocked guest read. */
    yz_movie_request_cancel(serial, "guest close");
}

static int yz_movie_read_eof_hook(CellFsFd fd, const char* guest_path,
                                  u64 offset, u64 nbytes)
{
    if (yz_movie_hle_armed() ||
        !yz_has_sfd_suffix(guest_path) || getenv("YZ_NO_MOVIE_PRESENT") ||
        getenv("YZ_NO_MOVIE_SYNC") || !rsx_live_draw_enabled() ||
        !movie_ffmpeg_available() || fd < 0 || fd >= YZ_MOVIE_FD_SLOTS)
        return 0;

    AcquireSRWLockExclusive(&g_movie_open_lock);
    yz_movie_fd_state* stream = &g_movie_fds[fd];
    if (!stream->active || !stream->host_path[0]) {
        ReleaseSRWLockExclusive(&g_movie_open_lock);
        return 0;
    }

    /* The validation handle reads a 0x5000-byte prefix (and a small follow-up).
     * The playback handle's first request is much larger. Let the validator see
     * real bytes; a stream request is the stable player-instance boundary. */
    if (!stream->serial && offset == 0 && nbytes <= 0x10000u)
        stream->validation = 1;
    if (stream->validation) {
        ReleaseSRWLockExclusive(&g_movie_open_lock);
        return 0;
    }

    if (!stream->serial) {
        strncpy(g_movie_open_path, stream->host_path,
                sizeof(g_movie_open_path) - 1);
        g_movie_open_path[sizeof(g_movie_open_path) - 1] = '\0';
        stream->serial = InterlockedIncrement(&g_movie_open_serial);
        fprintf(stderr,
                "[movie] queued playback fd=%d serial=%ld guest='%s' request=0x%llX\n",
                fd, stream->serial, guest_path, (unsigned long long)nbytes);
        fflush(stderr);
    }

    const LONG serial = stream->serial;
    while (InterlockedCompareExchange(&g_movie_completed_serial, 0, 0) < serial &&
           InterlockedCompareExchange(&g_movie_open_serial, 0, 0) == serial) {
        SleepConditionVariableSRW(&g_movie_done_cv, &g_movie_open_lock, 1000, 0);
    }
    const unsigned delivery = ++stream->eof_deliveries;
    if (delivery <= 8 || (delivery & 63u) == 0) {
        fprintf(stderr,
                "[movie] source EOS delivered fd=%d serial=%ld offset=0x%llX delivery=%u completed=%ld open=%ld\n",
                fd, serial, (unsigned long long)offset, delivery,
                InterlockedCompareExchange(&g_movie_completed_serial, 0, 0),
                InterlockedCompareExchange(&g_movie_open_serial, 0, 0));
        fflush(stderr);
    }
    ReleaseSRWLockExclusive(&g_movie_open_lock);
    return 1;
}

static void yz_movie_complete(LONG serial)
{
    AcquireSRWLockExclusive(&g_movie_open_lock);
    if (serial > g_movie_completed_serial)
        InterlockedExchange(&g_movie_completed_serial, serial);
    WakeAllConditionVariable(&g_movie_done_cv);
    ReleaseSRWLockExclusive(&g_movie_open_lock);
}

/* The FIFO consumer is the sole guest caller of rsx_live_draw_method(), and
 * it holds g_rsx_fifo_lock across each complete method dispatch.  Take that
 * same lock while transferring command-list ownership to or from the host
 * movie thread.  This drains any in-flight guest Close/reset and prevents a
 * new one from starting during the handoff, without adding synchronization
 * to the per-method render hot path. */
static void yz_movie_set_live_draw_mode(int on)
{
    yz_rsx_fifo_acquire();
    rsx_live_draw_set_movie_mode(on);
    yz_nr_vertical_set_movie_mode(on);
    yz_rsx_fifo_release();
}

/* Direct mwPly HLE presentation and the guest's subsequent Stop/Destroy
 * sequence are one graphics-ownership interval.  The host presenter may
 * finish before the guest retires the movie object; releasing ownership at
 * that earlier point lets a native frame preflight against transition pages
 * that the guest is still republishing.  Track the exact session serial so a
 * late close from an older player can never disarm a newer movie. */
static volatile LONG g_movie_live_draw_serial = 0;

static void yz_movie_begin_live_draw_mode(LONG serial)
{
    yz_rsx_fifo_acquire();
    rsx_live_draw_set_movie_mode(1);
    yz_nr_vertical_set_movie_mode(1);
    InterlockedExchange(&g_movie_live_draw_serial, serial);
    yz_rsx_fifo_release();
}

static void yz_movie_end_live_draw_mode(LONG serial)
{
    yz_rsx_fifo_acquire();
    if (InterlockedCompareExchange(
            &g_movie_live_draw_serial, 0, serial) == serial) {
        rsx_live_draw_set_movie_mode(0);
        yz_nr_vertical_set_movie_mode(0);
    }
    yz_rsx_fifo_release();
}

extern "C" void yz_host_movie_graphics_session_closed(long serial)
{
    if (serial > 0)
        yz_movie_end_live_draw_mode((LONG)serial);
}

static void yz_play_queued_movie(const char* path, LONG serial)
{
    if (!rsx_live_draw_enabled() || !movie_ffmpeg_available())
        return;

    const int mwply_hle = yz_movie_hle_armed();

    /* The movie owns graphics from the instant host decode begins, not only
     * after the first picture has been decoded.  AIX/video preparation writes
     * guest-backed pages which can also be referenced by the outgoing RSX
     * frame.  Letting a transactional native frame start in that interval can
     * make its already-preflighted mirror residency change during execution;
     * the frame then cannot safely fall back after earlier native actions.
     * Take the established FIFO-serialized movie handoff before opening the
     * decoder so no native section can straddle that preparation window. */
    yz_movie_begin_live_draw_mode(serial);
    MoviePlayer* mv = movie_open(path);
    if (!mv) {
        fprintf(stderr, "[movie] unable to decode queued movie '%s'\n", path);
        yz_movie_end_live_draw_mode(serial);
        yz_movie_complete(serial);
        return;
    }

    const uint32_t w = (uint32_t)movie_width(mv);
    const uint32_t h = (uint32_t)movie_height(mv);
    int fps = (int)(movie_framerate(mv) + 0.5);
    if (fps <= 0) fps = 30;
    const int accept_fast = getenv("YZ_FRONTIER_ACCEPT_FAST") ||
        (getenv("YZ_A010_ACCEPT_FAST") &&
         (strstr(path, "hd_sega_logo") || strstr(path, "advertise.sfd")));
    /* Decode the first picture before starting the audio clock. MPEG frame
     * startup can cost roughly one frame, which otherwise makes audio lead
     * from the first visible image even though steady-state cadence is sound. */
    double first_pts = 0.0;
    const uint8_t* first_rgba = movie_next_rgba(mv, &first_pts);
    if (!first_rgba) {
        fprintf(stderr, "[movie] no video frames in '%s'\n", path);
        fflush(stderr);
        movie_close(mv);
        yz_movie_end_live_draw_mode(serial);
        yz_movie_complete(serial);
        return;
    }
    /* SFD audio is not always the whole scene mix. Auth sequences can keep
     * dialogue/voice on the game's ordinary audio ports while the movie
     * carries music and ambience. Keep the conservative mute as the default,
     * but honor the same A/B switch as mwPly's non-direct host path so those
     * independent voices can be mixed during direct presentation. */
    const int mix_guest = getenv("YZ_MOVIE_HLE_MIX_GUEST") ? 1 : 0;
    const int host_audio = !accept_fast && movie_has_audio(mv) &&
        cellAudioHostStreamStart(movie_audio_s16(mv),
                                 movie_audio_frames(mv),
                                 mix_guest ? 0 : 1) == 0;
    const ULONGLONG wall_start_ms = GetTickCount64();
    ULONGLONG last_clock_log_ms = wall_start_ms;
    int frames = 0;
    int superseded = 0;
    int cancelled = 0;
    int skip_requested = 0;
    int skip_guest_acked = 0;
    int skip_stop_queued = 0;
    int natural_completion = 0;

    InterlockedExchange(&g_movie_present_timescale, fps);
    InterlockedExchange(&g_movie_presenting_serial, serial);
    fprintf(stderr,
            "[movie] presenting '%s' (%ux%u @ %d fps, audio=%s, owner=%s%s)\n",
            path, w, h, fps, host_audio ? "FFmpeg ADX" : "native/none",
            mwply_hle ? "mwPly HLE" : "fd bridge",
            accept_fast ? ", acceptance=single-frame" : "");
    fflush(stderr);
    cellPad_host_movie_skip_begin();
    for (;;) {
        if (InterlockedCompareExchange(&g_movie_open_serial, 0, 0) != serial) {
            superseded = 1;
            break;
        }
        if (InterlockedCompareExchange(&g_movie_cancelled_serial, 0, 0) >= serial) {
            cancelled = 1;
            break;
        }
        if (cellPad_host_movie_skip_requested()) {
            /* The host and guest observe the same physical Start press.  The
             * host edge is diagnostic only: whether this particular movie is
             * skippable belongs to the guest player.  Keep decoding until the
             * guest's Stop/Close hook requests cancellation; otherwise an
             * ignored or too-early Start must leave playback running. */
            skip_requested = 1;
            fprintf(stderr,
                    "[movie] Start observed; awaiting guest Stop/Close serial=%ld\n",
                    serial);
            fflush(stderr);
        }
        if (skip_requested && !skip_guest_acked &&
            cellPad_host_movie_skip_guest_seen()) {
            skip_guest_acked = 1;
            fprintf(stderr,
                    "[movie] guest received Start serial=%ld\n",
                    serial);
            fflush(stderr);
        }
        if (skip_guest_acked && !skip_stop_queued && !mwply_hle) {
            /* The legacy fd bridge cannot rely on the original player to
             * translate a guest-visible Start edge into Stop while the host
             * owns presentation.  Queue the same guest-thread Stop wrapper
             * used at natural EOS, but only after both host edge detection
             * and cellPadGetData prove that this is a fresh, guest-visible
             * press.  This preserves the held-button guard at movie entry
             * and keeps all CRI lifecycle calls on their owning PPU thread. */
            skip_stop_queued = 1;
            InterlockedExchange(&g_yz_movie_stop_pending_serial, serial);
            fprintf(stderr,
                    "[movie] guest-confirmed Start; queued guest-thread "
                    "mwPlyStop wrapper serial=%ld\n",
                    serial);
            fflush(stderr);
        }
        double pts = 0.0;
        const uint8_t* rgba;
        if (frames == 0) {
            rgba = first_rgba;
            pts = first_pts;
        } else {
            rgba = movie_next_rgba(mv, &pts);
        }
        if (!rgba) {
            natural_completion = 1;
            if (mwply_hle) {
                fprintf(stderr,
                        "[movie] host EOS; publishing PLAYEND to mwPly owner serial=%ld\n",
                        serial);
                fflush(stderr);
            } else {
                InterlockedExchange(&g_yz_movie_stop_pending_serial, serial);
                fprintf(stderr,
                        "[movie] host EOS; queued guest-thread mwPlyStop wrapper serial=%ld\n",
                        serial);
                fflush(stderr);

                /* Legacy fd bridge only: the wrapper runs Stop plus the game's
                 * owner callback on its PPU thread. The mwPly HLE path never
                 * injects Stop; it reports PLAYEND and lets the game call it. */
                while (InterlockedCompareExchange(&g_movie_open_serial, 0, 0) == serial &&
                       InterlockedCompareExchange(&g_movie_cancelled_serial, 0, 0) < serial &&
                       InterlockedCompareExchange(&g_movie_closed_serial, 0, 0) < serial &&
                       InterlockedCompareExchange(&g_yz_movie_stop_applied_serial, 0, 0) < serial) {
                    if (rsx_null_backend_pump_messages() < 0)
                        break;
                    Sleep(8);
                }
                const int wrapper_applied =
                    InterlockedCompareExchange(&g_yz_movie_stop_applied_serial, 0, 0) >= serial;
                fprintf(stderr,
                        "[movie] natural completion wrapper_applied=%d serial=%ld\n",
                        wrapper_applied, serial);
                fflush(stderr);
                if (wrapper_applied)
                    yz_movie_request_cancel(serial, "guest mwPlyStop wrapper");
                cancelled =
                    InterlockedCompareExchange(&g_movie_cancelled_serial, 0, 0) >= serial;
            }
            break;
        }
        const double target = pts > 0.0 ? pts : (double)frames / fps;
        for (;;) {
            const uint64_t audio_cursor =
                host_audio ? cellAudioHostStreamPositionFrames() : 0;
            const int audio_finished =
                host_audio ? cellAudioHostStreamFinished() : 0;
            const double wall_clock =
                (GetTickCount64() - wall_start_ms) / 1000.0;
            /* Audio is authoritative while samples are still playing. Some
             * clips (notably Sega) intentionally have a silent video tail;
             * after audio EOS, continue that tail on the same wall clock. */
            const double clock = host_audio && !audio_finished
                ? (double)audio_cursor / 48000.0
                : wall_clock * (accept_fast ? 32.0 : 1.0);
            if (clock + 0.001 >= target) break;
            const ULONGLONG now_ms = GetTickCount64();
            if (now_ms - last_clock_log_ms >= 1000) {
                last_clock_log_ms = now_ms;
                fprintf(stderr,
                        "[movie-clock] serial=%ld frame=%d target=%.3f "
                        "clock=%.3f audio_cursor=%llu finished=%d\n",
                        serial, frames, target, clock,
                        (unsigned long long)audio_cursor,
                        audio_finished);
                fflush(stderr);
            }
            if (InterlockedCompareExchange(&g_movie_cancelled_serial, 0, 0) >= serial) {
                cancelled = 1;
                break;
            }
            if (rsx_null_backend_pump_messages() < 0) {
                cancelled = 1;
                break;
            }
            Sleep(1);
        }
        if (cancelled) break;
        rsx_live_draw_present_rgba(rgba, w, h);
        frames++;
        InterlockedExchange(&g_movie_presented_frames, frames);
        if (frames <= 3 || (frames % fps) == 0) {
            fprintf(stderr,
                    "[movie-frame] serial=%ld presented=%d pts=%.3f "
                    "audio_cursor=%llu\n",
                    serial, frames, pts,
                    (unsigned long long)(host_audio
                        ? cellAudioHostStreamPositionFrames() : 0));
            fflush(stderr);
        }
        if (accept_fast) {
            /* Acceptance automation only: decoding every picture is still
             * CPU-bound even when its presentation clock is accelerated.
             * One decoded frame proves the stream opened, then use the same
             * natural-completion publication below so the game retains its
             * ordinary PLAYEND/Stop/Destroy ownership sequence. */
            const ULONGLONG observe_deadline = GetTickCount64() + 10000u;
            while (InterlockedCompareExchange(
                       &g_movie_playing_observed_serial, 0, 0) < serial &&
                   InterlockedCompareExchange(
                       &g_movie_cancelled_serial, 0, 0) < serial &&
                   InterlockedCompareExchange(
                       &g_movie_open_serial, 0, 0) == serial &&
                   GetTickCount64() < observe_deadline) {
                if (rsx_null_backend_pump_messages() < 0)
                    break;
                Sleep(1);
            }
            natural_completion = 1;
            fprintf(stderr,
                    "[movie] acceptance shortcut: publishing natural EOS "
                    "after one decoded frame serial=%ld playing_seen=%d\n",
                    serial,
                    InterlockedCompareExchange(
                        &g_movie_playing_observed_serial, 0, 0) >= serial);
            fflush(stderr);
            break;
        }
        if (rsx_null_backend_pump_messages() < 0) break;
    }
    if (host_audio)
        cellAudioHostStreamStop();
    InterlockedCompareExchange(&g_movie_presenting_serial, 0, serial);
    /* Direct mwPly HLE keeps ownership until its guest Destroy wrapper closes
     * this exact session.  The fd bridge has no HLE session object, so its
     * ownership still ends with host presentation. */
    if (!mwply_hle)
        yz_movie_end_live_draw_mode(serial);
    movie_close(mv);
    /* The host movie is an overlay, not an RSX FIFO producer. SAIL orders
     * source EOS before Stop completion, but it does not manufacture a GPU
     * flip at that boundary. Let the guest's ordinary flip/event path retire
     * its own work, then publish the sticky source-completion predicate. */
    yz_movie_complete(serial);
    if (natural_completion && !mwply_hle) {
        while (InterlockedCompareExchange(&g_movie_closed_serial, 0, 0) < serial &&
               InterlockedCompareExchange(&g_movie_open_serial, 0, 0) == serial) {
            if (rsx_null_backend_pump_messages() < 0)
                break;
            Sleep(8);
        }
        fprintf(stderr,
                "[movie] natural completion Close observed=%d serial=%ld\n",
                InterlockedCompareExchange(&g_movie_closed_serial, 0, 0) >= serial,
                serial);
        fflush(stderr);
    }
    cellPad_host_movie_skip_end();
    if (skip_requested && !cancelled && !superseded && !natural_completion) {
        fprintf(stderr,
                "[movie] guest did not accept skip; completed naturally serial=%ld\n",
                serial);
    }
    fprintf(stderr, "[movie] %s after %d frames\n",
            superseded ? "switched streams" :
            cancelled ? "cancelled cleanly" : "presentation complete",
            frames);
    fflush(stderr);
}

/* Dedicated window thread. A Win32 window must be created AND message-pumped on
 * the same thread, so it lives here rather than on the main/consumer threads.
 * rsx_null_backend_init() opens the window and registers the null backend (GDI
 * clear-color present); the consumer + flip path then drive it. When YZ_RSX_DRAW
 * is set, Track B's live NV4097->D3D12 engine binds a swap chain to that same
 * HWND and takes over presentation (the null GDI present is suppressed). */
static DWORD WINAPI yz_window_thread(LPVOID)
{
    yz_thread_adopt_host("window");
    if (rsx_null_backend_init(1280, 720, "Yakuza: Dead Souls (ps3recomp)") != 0) {
        fprintf(stderr, "[rsx] window init failed\n");
        return 1;
    }
    if (rsx_live_draw_enabled()) {
        int r = rsx_live_draw_init(rsx_null_backend_get_hwnd(), 1280, 720,
                                   yz_rsx_live_guest_ptr, nullptr);
        if (r == 0) {
            rsx_null_backend_suppress_present(1);
            fprintf(stderr, "[rsx] live-draw engine up (D3D12); null GDI present suppressed\n");
        } else {
            fprintf(stderr, "[rsx] live-draw init FAILED (%d) -> falling back to null present\n", r);
        }
    }

    cellfs_set_open_hook(yz_movie_open_hook);
    cellfs_set_close_hook(yz_movie_close_hook);
    cellfs_set_read_eof_hook(yz_movie_read_eof_hook);

    /* YZ_MOVIE_TEST=<path.sfd>: standalone proof of the host movie path -- decode
     * the movie with FFmpeg and present it straight to the D3D12 window (movie
     * mode gates the guest's draws off). Not the game hook; just proves
     * decode -> present in-process. */
    const char* mvpath = getenv("YZ_MOVIE_TEST");
    if (mvpath && *mvpath && rsx_live_draw_enabled()) {
        MoviePlayer* mv = movie_open(mvpath);
        if (mv) {
            const uint32_t w = (uint32_t)movie_width(mv), h = (uint32_t)movie_height(mv);
            int fps = (int)(movie_framerate(mv) + 0.5); if (fps <= 0) fps = 30;
            const DWORD frame_ms = (DWORD)(1000 / fps);
            fprintf(stderr, "[movie] YZ_MOVIE_TEST playing %s (%ux%u @ %dfps)\n", mvpath, w, h, fps);
            yz_movie_set_live_draw_mode(1);
            int n = 0;
            for (;;) {
                const uint8_t* rgba = movie_next_rgba(mv, nullptr);
                if (!rgba) break;                          /* end of stream */
                rsx_live_draw_present_rgba(rgba, w, h);
                if (n == 30) {   /* dump one presented frame as proof (RGB PPM) */
                    FILE* f = fopen("scratch/movie_runtime.ppm", "wb");
                    if (f) { fprintf(f, "P6\n%u %u\n255\n", w, h);
                        for (uint32_t i = 0; i < w * h; i++) fwrite(rgba + i * 4, 1, 3, f);
                        fclose(f); fprintf(stderr, "[movie] wrote scratch/movie_runtime.ppm (frame 30)\n"); }
                }
                if (rsx_null_backend_pump_messages() < 0) break;
                Sleep(frame_ms);
                n++;
            }
            fprintf(stderr, "[movie] done (%d frames presented)\n", n);
            movie_close(mv);
            yz_movie_set_live_draw_mode(0);
        } else {
            fprintf(stderr, "[movie] movie_open('%s') failed (ffmpeg_available=%d)\n",
                    mvpath, movie_ffmpeg_available());
        }
    }

    for (;;) {
        char queued_movie[MAX_PATH] = {};
        LONG queued_serial = 0;
        AcquireSRWLockShared(&g_movie_open_lock);
        queued_serial = g_movie_open_serial;
        if (queued_serial != g_movie_played_serial)
            strncpy(queued_movie, g_movie_open_path, sizeof(queued_movie) - 1);
        ReleaseSRWLockShared(&g_movie_open_lock);

        if (queued_movie[0] && queued_serial != g_movie_played_serial) {
            g_movie_played_serial = queued_serial;
            yz_play_queued_movie(queued_movie, queued_serial);
        }
        if (rsx_null_backend_pump_messages() < 0)
            break;            /* window closed */
        Sleep(8);
    }
    /* The render window is the host application's lifetime boundary. The
     * lifted guest main thread has no Win32 close event and otherwise keeps
     * all audio/worker threads alive after DestroyWindow, leaving an
     * invisible process playing sound. End the host process when its window
     * pump observes WM_CLOSE/WM_QUIT. */
#if defined(YZ_PERF_PROFILE)
    spu_perf_dump();
#endif
    yz_rsx_wait_classifier_shutdown_serialized();
    fprintf(stderr, "[window] closed; terminating host process\n");
    fflush(stderr);
    /* Exit dump (reason 3): ExitProcess skips CRT atexit, and the stall
     * dumps are progress-gated — this is the capture of record for a
     * pre-progress freeze (boots 49/51 frame-52 face). */
    yz_frontier_trace_dump(3u);
    ExitProcess(0);
    return 0;
}

/* Present the current frame to the window. The clear color was already tracked
 * into the backend as the consumer processed this frame's NV4097 clear methods,
 * so present() just flips it onto the window. */
static void yz_rsx_present(uint32_t buffer_id)
{
    rsx_backend* b = rsx_get_backend();
    if (!b) return;
    if (b->end_frame) b->end_frame(b->userdata);
    if (b->present)   b->present(b->userdata, buffer_id);
}

/* ---- Minimal RSX fifo consumer ("mini-RSX") --------------------------------
 * The game's inline flush/finish routine writes ctrl->put and spins until
 * ctrl->get (and for SetReference waits, ctrl->ref) catch up — on real
 * hardware the RSX advances them. This host thread walks the command
 * stream from get to put: follows jumps, skips methods by their count
 * field, and executes SET_REFERENCE (NV406E method 0x050; the EBOOT's own
 * inline SetReference writes header 0x00040050) by storing the operand to
 * ctrl->ref. No other method does anything yet — that is the D3D12 tier-1
 * wiring, later. */
/* Execute one method register write. Returns 0 normally, 1 if the fifo
 * must stall (semaphore acquire not yet satisfied).
 * Semaphore semantics mirror RPCS3 (emu/RSX/NV47/HW/nv4097.cpp,
 * rsx_methods.cpp, RSXThread.cpp get_address): each class has a context-DMA
 * selector + offset register; the DMA selects where the semaphore lives:
 *   0x66606660/0x66616661 -> label area (what cellGcmGetLabelAddress returns)
 *   0xFEED0001            -> main memory via the io map (offset = io offset)
 *   0xFEED0000            -> RSX local memory
 * back_end release swizzles the value (ARGB shuffle); the others are raw. */
static uint32_t yz_rsx_sem_dma_406e = 0x66616661;  /* RPCS3 reset values */
static uint32_t yz_rsx_sem_dma_4097 = 0x66606660;
static uint32_t yz_rsx_sem_off_406e;
static uint32_t yz_rsx_sem_off_4097;

static uint32_t yz_rsx_sem_addr(uint32_t dma, uint32_t offset)
{
    switch (dma) {
    case 0x66606660u:                            /* SEMAPHORE_RW (report/label, local) */
    case 0x66616661u:                            /* SEMAPHORE_R  (report/label, main) */
        /* labels/report semaphores live in the reports region context_allocate
         * set up (= what Sony's cellGcmGetLabelAddress returns into). */
        return RSX_REPORTS + (offset & 0xFFFFFu);
    case 0x56616660u:                            /* DEVICE_RW (RPCS3 gcm_enums.h:520) */
    case 0x56616661u:                            /* DEVICE_R  -> device_addr + offset.
        * The flip HW-sync semaphore lives at device+0x30 (RSXThread.cpp:240). */
        return (offset < 0x100000u) ? RSX_DEVICE_ADDR + offset : 0;
    case 0xFEED0001u:                            /* main memory via the io map */
        return yz_rsx_io_to_ea(offset);
    case 0xFEED0000u:                            /* RSX local memory */
        return (offset < YZ_GCM_LOCAL_SIZE) ? YZ_GCM_LOCAL_BASE + offset : 0;
    default: {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[rsx] unknown semaphore context dma 0x%08X\n", dma);
        }
        return 0;
    }
    }
}

/* NV3062 (2D surface) + NV308A (image-from-cpu) state: cellGcmInlineTransfer
 * writes data words into memory through the 2D blit
 * engine — Yakuza uses it to publish its flip/vsync counters to a spot in
 * io memory that the PPU then polls. Semantics per RPCS3 nv308a.cpp:
 * A8R8G8B8/Y32 is a raw word copy to dst_offset + x*4 + y*pitch; operands
 * with index >= SIZE_OUT.x are skipped. */
static uint32_t yz_rsx_blit_dst_dma = 0xFEED0000;
static uint32_t yz_rsx_blit_dst_off;
static uint32_t yz_rsx_blit_pitch   = (64u << 16) | 64u;
static uint32_t yz_rsx_blit_fmt     = 0xB;     /* a8r8g8b8 */
static uint32_t yz_rsx_blit_point;
static uint32_t yz_rsx_blit_size_out = 0x00010001;

extern "C" int64_t yz_sys_rsx_context_attribute(ppu_context*);   /* defined below */

/* Lossless user-interrupt delivery (s25, the round-25 stall root — the FIFTH
 * dropped-guest-notification instance after 0xEB00/0xE920/flip/0x103).
 * MEASURED (scratch/s25ride.err): the game COALESCES its ucmd cause counter
 * (deliveries 1-21 sequential, then one method carrying cause=25 — a
 * documented rapid-user-command coalescing behavior), and that single coalesced
 * send hit a momentarily FULL RSX event queue: sys_event_port_send returned
 * 0x8001000A (EBUSY) and our fire-and-forget path LOST it. The handler never
 * saw cause 25, the wid4 pool never published rounds 22-25, and the stream
 * parked forever on NV406E SEMAPHORE_ACQUIRE want=25 — WITHOUT the 0x2004
 * death (refutes ledger #49's prime-mover attribution). lv1 keeps ONE pending
 * cause register and re-delivers when the queue drains; model that: latch the
 * undelivered cause, retry from the consumer loop (userCmdParam already
 * carries the latest arg, so a retry delivers correct coalesced state).
 * Consumer-thread-only state, no locking. Kill-switch YZ_NO_UCMD_RETRY. */
/* s25 GENERALIZED (post notification-surface audit, scratch/
 * s25_notification_audit.md risk #1): the latch now covers ANY event bits
 * sent to the RSX port, not just USER_CMD — a failed send ORs its cause
 * bits into one pending mask (exactly how the vblank path already delivers
 * multiple causes in one send) and the consumer-top retry sends the
 * accumulated mask once the queue drains. The user-cmd cause arg still
 * travels via driverInfo.userCmdParam (latest-wins, lv1 coalescing). */
/* 0 = none; else latched cause bits. s26: retried from BOTH the consumer loop
 * top and the vblank tick (the consumer-top-only retry deadlocked when the
 * lost delivery parked the consumer itself — s26ride4), so take-and-retry is
 * atomic: one thread wins the exchange, a failed send re-ORs the bits back.
 * Prevents double-delivery of the same latched cause. */
static volatile long long g_rsx_ev_pending = 0;
extern "C" volatile long g_yz_ucmd_handler_arg;
extern "C" volatile long g_yz_ucmd_handler_completed;
extern "C" volatile long g_yz_ucmd_handler_completed_epoch;

static int yz_a010_fifo_publication_repair_enabled(void)
{
#if defined(YZ_PERF_CLEAN)
    return 1;
#else
    static const int enabled =
        getenv("YZ_A010_MISSING_REL") ? 1 : 0;
    return enabled;
#endif
}

/* Historical FE0 recovery re-ran the complete user callback whenever an
 * acquire found all five image-4 tasks parked.  The callback itself signals
 * all five tasks, so each retry injected another complete workload round and
 * masked the barrier's real lifted-SPU -> taskset signal publication.  Exact
 * taskset guest-write routing now carries that edge.  Keep the old replay as
 * an explicit diagnostic fallback only; production never redelivers a
 * callback that has already completed. */
static int yz_fe0_callback_replay_enabled(void)
{
    static const int enabled = [] {
        const char* value = getenv("YZ_FE0_CALLBACK_REPLAY");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}

static int64_t yz_rsx_ev_send(uint64_t bits)
{
    ppu_context sc; memset(&sc, 0, sizeof(sc));
    sc.gpr[3] = g_rsx_event_port;
    sc.gpr[5] = bits;
    int64_t r = sys_event_port_send(&sc);
    if (r != 0) {
        static int no_retry = -1;
        if (no_retry < 0) no_retry = (getenv("YZ_NO_UCMD_RETRY")
                                      || getenv("YZ_NO_EV_RETRY")) ? 1 : 0;
        if (!no_retry) {
            _InterlockedOr64(&g_rsx_ev_pending, (long long)bits);
            fprintf(stderr, "[rsx-ev] send FAILED r=0x%llX bits=0x%llX -> LATCHED "
                    "for retry (queue full; lossless delivery, s25 fix)\n",
                    (unsigned long long)(uint64_t)r, (unsigned long long)bits);
            fflush(stderr);
        }
    }
    return r;
}

static void yz_ucmd_retry_pending(void)
{
    if (!g_rsx_ev_pending || !g_rsx_event_port) return;
    uint64_t bits = (uint64_t)_InterlockedExchange64(&g_rsx_ev_pending, 0);
    if (!bits) return;                     /* another thread took it */
    ppu_context sc; memset(&sc, 0, sizeof(sc));
    sc.gpr[3] = g_rsx_event_port;
    sc.gpr[5] = bits;
    int64_t r = sys_event_port_send(&sc);
    if (r == 0) {
        fprintf(stderr, "[rsx-ev] RETRY delivered latched bits=0x%llX (queue drained)\n",
                (unsigned long long)bits);
        fflush(stderr);
    } else {
        /* still full — put the bits back; log sparsely so a permanently-
         * wedged intr thread is visible without flooding */
        _InterlockedOr64(&g_rsx_ev_pending, (long long)bits);
        static unsigned long rn = 0; rn++;
        if ((rn & 0x3FFu) == 1) {
            fprintf(stderr, "[rsx-ev] retry still failing bits=0x%llX r=0x%llX (n=%lu)\n",
                    (unsigned long long)bits,
                    (unsigned long long)(uint64_t)r, rn);
            fflush(stderr);
        }
    }
}

/* Exact typed-command semaphore acquire. This resolves the same DMA/address
 * contract as the legacy NV406E path, but does not synthesize a method or
 * enter yz_rsx_method. A retained typed span rechecks this value while GET
 * remains on the owned packet. */
extern "C" int yz_nr_vertical_sem_read(uint32_t dma, uint32_t offset,
                                        uint32_t* value)
{
    if (!value)
        return -1;
    const uint32_t address = yz_rsx_sem_addr(dma, offset);
    if (!address)
        return -1;
    *value = vm_read32(address);
    return 0;
}

extern "C" uint64_t cellGcmReportTimestampNs(void);

static int yz_nr_vertical_report_address(uint32_t dma, uint32_t offset,
                                         uint32_t size,
                                         uint32_t* out_address)
{
    if (!out_address || !size || size > 16u)
        return -1;
    uint32_t address = 0;
    switch (dma) {
    case RSX_DMA_REPORT_LOCATION_LOCAL:
    case RSX_DMA_MEMORY_FRAME_BUFFER:
        if (offset < RSX_REPORT_AREA_SIZE &&
            offset <= RSX_REPORT_AREA_SIZE - size)
            address = RSX_REPORTS + offset;
        break;
    case RSX_DMA_REPORT_LOCATION_MAIN:
    case RSX_DMA_MEMORY_HOST_BUFFER:
        {
            uint32_t io = 0;
            if (!rsx_nr_main_report_io_range(offset, size, &io))
                return -1;
            const uint32_t first = yz_rsx_io_to_ea(io);
            const uint32_t last = yz_rsx_io_to_ea(io + size - 1u);
            if (!first || last != first + size - 1u)
                return -1;
            address = first;
        }
        break;
    default:
        break;
    }
    if (!address)
        return -1;
    *out_address = address;
    return 0;
}

extern "C" int yz_nr_vertical_render_condition_read(
    uint32_t dma, uint32_t offset, uint32_t* value)
{
    uint32_t address = 0;
    if (!value || yz_nr_vertical_report_address(
            dma, offset, 16u, &address) != 0)
        return -1;
    *value = vm_read32(address + 8u);
    return 0;
}

static void yz_nr_vertical_406e_release(uint32_t dma, uint32_t offset,
                                        uint32_t value)
{
    const uint32_t address = yz_rsx_sem_addr(dma, offset);
    /* Hardware flip credit: a zero release to DEVICE_R+0x30 publishes one. */
    if (address == RSX_DEVICE_ADDR + 0x30u && value == 0u)
        value = 1u;
    if (yz_ft_on() &&
        ((address >= RSX_REPORTS && address < RSX_REPORTS + 0x1000u) ||
         address == RSX_DEVICE_ADDR + 0x30u))
        yz_ft("REL addr=0x%08X val=0x%08X qhead=%u pending[qh]=%ld",
              address, value, g_rsx_queued_head,
              g_rsx_flip_pending[g_rsx_queued_head & 7u]);
    if (address)
        yz_rsx_w32(address, value);

    /* Preserve the legacy release-arm compatibility policy exactly.  Fast
     * production builds default it off; YZ_SEMARM can explicitly restore it
     * for an A/B, while non-PERF builds retain their historical default. */
    if (address == RSX_REPORTS + 0x10u && value != 0u) {
        static int disabled = -1;
        if (disabled < 0) {
#if defined(YZ_PERF_CLEAN)
            disabled = getenv("YZ_SEMARM") ? 0 : 1;
#else
            disabled = getenv("YZ_NO_SEMARM") ? 1 : 0;
#endif
            fprintf(stderr, "[semarm] armed (release-arm heuristic %s)\n",
                    disabled ? "DISABLED" : "on");
            fflush(stderr);
        }
        if (!disabled) {
            const LONG previous = InterlockedExchange(
                &g_rsx_flip_pending[g_rsx_queued_head & 7u], 1);
            if (previous == 0) {
                const long n = _InterlockedIncrement(&g_yz_semarm_count);
                if (n <= 4 || (n & 0xFF) == 0) {
                    fprintf(stderr,
                            "[semarm] manufactured flip arm total=%ld\n", n);
                    fflush(stderr);
                }
            }
            if (yz_ft_on())
                yz_ft("ARM pending[%u] %ld->1",
                      g_rsx_queued_head & 7u, previous);
        }
    }
}

extern "C" void yz_nr_vertical_sem_write(uint32_t dma, uint32_t offset,
                                           uint32_t value,
                                           uint32_t texture_read)
{
    /* The typed backend has already applied the 1D70 byte-0/2 hardware
     * transform. Release kind 2 is the distinct NV406E path and keeps the
     * legacy device-credit and semarm publication contract in one helper. */
    if (texture_read == 2u) {
        yz_nr_vertical_406e_release(dma, offset, value);
        return;
    }
    if (texture_read > 1u)
        return;
    const uint32_t address = yz_rsx_sem_addr(dma, offset);
    if (address)
        yz_rsx_w32(address, value);
}

extern "C" int yz_nr_vertical_report(uint32_t kind, uint32_t arg,
                                       uint32_t dma)
{
    if (kind != 0u)
        return 0; /* CLEAR_REPORT_VALUE: no ZCULL accumulator is modeled. */

    const uint32_t type = arg >> 24;
    const uint32_t offset = arg & 0x00FFFFFFu;
    uint32_t address = 0;
    if (yz_nr_vertical_report_address(dma, offset, 16u, &address) != 0)
        return -1;

    const uint64_t timestamp = cellGcmReportTimestampNs();
    vm_write64(address, timestamp);
    if (type >= RSX_REPORT_TYPE_ZPASS_PIXEL_CNT &&
        type <= RSX_REPORT_TYPE_ZCULL_STATS3) {
        /* The strict native sink enforces SET_RENDER_ENABLE, unlike the
         * established legacy renderer. Until native GPU occlusion queries
         * exist, conservatively mark an unmodeled ZPASS query visible so its
         * dependent geometry executes; synthesizing zero made every object
         * using this SDK culling path disappear. Other ZCULL stats stay zero. */
        vm_write32(address + 8u,
                   rsx_report_unmodeled_value(type, 1));
        vm_write32(address + 12u, 0u);
    } else {
        /* Ordinary reports leave value@+8 untouched and clear only pad. */
        vm_write32(address + 12u, 0u);
    }
    return 0;
}

extern "C" int yz_nr_vertical_report_can(uint32_t kind, uint32_t arg,
                                           uint32_t dma)
{
    if (kind != 0u)
        return 0; /* CLEAR_REPORT_VALUE has no guest-memory dependency. */

    const uint32_t offset = arg & 0x00FFFFFFu;
    uint32_t address = 0;
    return yz_nr_vertical_report_address(
        dma, offset, 16u, &address);
}

/* Semantic actions shared by the legacy packet decoder and the vertical
 * typed-command backend.  Keeping these below the packet layer is important:
 * an owned typed span must not reconstruct a legacy method and feed it back
 * through yz_rsx_method, otherwise the native path would still pay the
 * decoder/renderer bridge it is intended to replace. */
static void yz_rsx_exec_user_command(uint32_t method, uint32_t arg)
{
    /* s22 ROOT FIX: the RSX USER INTERRUPT pumps the title's
     * wid4 SPU decode pool.  Preserve the exact delivery and stale-recovery
     * rules for both legacy and typed producers. */
    static int nu = -1;
    if (nu < 0) {
        nu = getenv("YZ_NO_UCMD") ? 1 : 0;
        fprintf(stderr, "[ucmd] armed (user-interrupt dispatch %s)\n",
                nu ? "DISABLED by YZ_NO_UCMD" : "on");
        fflush(stderr);
    }
    if (nu)
        return;
    if (InterlockedCompareExchange(&g_yz_a010_root_active, 0, 0) != 0 &&
        yz_a010_fifo_publication_repair_enabled()) {
        const uint32_t have = vm_read32(RSX_REPORTS + 0xFE0u);
        const uint32_t behind = have - arg;
        if (behind < 0x10000u && arg <= have) {
            static unsigned long stale_ucmd = 0;
            stale_ucmd++;
            if (stale_ucmd <= 16 || (stale_ucmd & 0xFFu) == 0u) {
                fprintf(stderr,
                        "[a010-stale-ucmd] skipped n=%lu cause=0x%08X "
                        "completed=0x%08X\n",
                        stale_ucmd, arg, have);
                fflush(stderr);
            }
            return;
        }
    }
    vm_write32(RSX_DRIVER_INFO + 0x12CC, arg);
#ifdef YZ_NATIVE_GCM
    yz_fe0_timeline_emit(YZ_FE0_EVENT_UCMD_DISPATCH, arg, method,
                         yz_rsx_sem_dma_406e, yz_rsx_sem_off_406e,
                         0, 0);
    cellGcmDispatchUserCommand(arg);
    {
        static unsigned long un = 0;
        un++;
        if (un <= 40 || (un & 0xFFu) == 0) {
            fprintf(stderr, "[ucmd-hle] n=%lu cause=0x%08X dispatched\n",
                    un, arg);
            fflush(stderr);
        }
    }
#else
    if (g_rsx_event_port) {
        const int64_t r = yz_rsx_ev_send(0x80ull);
        static unsigned long un = 0;
        un++;
        if (un <= 40 || (un & 0xFFu) == 0) {
            fprintf(stderr,
                    "[ucmd] n=%lu cause=0x%08X handlers=0x%08X send=%lld\n",
                    un, arg, vm_read32(RSX_DRIVER_INFO + 0x12C0),
                    (long long)r);
            fflush(stderr);
        }
    } else {
        static int w = 0;
        if (w < 4) {
            w++;
            fprintf(stderr,
                    "[ucmd] DROPPED cause=0x%08X (no event port yet)\n",
                    arg);
            fflush(stderr);
        }
    }
#endif
}

static void yz_rsx_exec_set_reference(uint32_t arg)
{
    /* RPCS3's SET_REFERENCE flushes before publishing REF.  The live backend
     * keeps this optional compatibility fence in one semantic implementation
     * regardless of whether the action came from packets or typed GCM. */
    static int fs = -1;
    if (fs < 0)
        fs = getenv("YZ_RSX_FENCE_SYNC") ? 1 : 0;
    if (fs) {
        extern void rsx_live_draw_flush(void);
        rsx_live_draw_flush();
    }
    vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_REF, arg);
}

extern "C" void yz_nr_vertical_exec_set_reference(uint32_t value)
{
    yz_rsx_exec_set_reference(value);
}

extern "C" void yz_nr_vertical_exec_user_command(uint32_t cause)
{
    yz_rsx_exec_user_command(0xEB00u, cause);
}

extern "C" void yz_nr_vertical_exec_present_complete(uint32_t buffer_id);

extern "C" void yz_nr_vertical_exec_present(uint32_t buffer_id)
{
    /* Direct semantic equivalent of E944 followed by E924. Presentation of
     * the already-rendered display surface is explicit—no legacy RSX method
     * decoder is called—then the existing queue/flip packages preserve the
     * guest-visible head state, event delivery, completion, and movie route. */
    rsx_live_draw_typed_present(buffer_id);
    yz_nr_vertical_exec_present_complete(buffer_id);
}

extern "C" void yz_nr_vertical_exec_present_complete(uint32_t buffer_id)
{
    /* The native D3D12 sink has already copied/presented its exact scanout.
     * Publish only the guest-visible queue/head packages here. */
    ppu_context sc = {};
    sc.gpr[4] = 0x103u;
    sc.gpr[5] = 1u;
    sc.gpr[6] = buffer_id;
    yz_sys_rsx_context_attribute(&sc);
    memset(&sc, 0, sizeof(sc));
    sc.gpr[4] = 0x102u;
    sc.gpr[5] = 1u;
    sc.gpr[6] = 0x8000010Fu;
    yz_sys_rsx_context_attribute(&sc);
}

static int yz_rsx_method(uint32_t method, uint32_t arg)
{
    /* Deliver any latched (queue-full) RSX event bits before consuming more
     * of the stream — one predicted-not-taken branch when idle. */
    if (g_rsx_ev_pending) yz_ucmd_retry_pending();

    /* GCM_FLIP_HEAD arm banner on the consumer's FIRST method, not the first
     * 0xE920 hit: a "0xE920 never seen" negative is only MEASURED if the log
     * proves the probe ran (docs/LESSONS.md, the s22 honesty rules). */
    static int nf = -1;
    if (nf < 0) { nf = getenv("YZ_NO_FLIPHEAD") ? 1 : 0;
        fprintf(stderr, "[fliphead] armed (immediate-flip dispatch %s)\n",
                nf ? "DISABLED by YZ_NO_FLIPHEAD" : "on"); fflush(stderr); }

    /* Track B live draw: mirror the full method stream into the NV4097->D3D12
     * engine (no-op unless YZ_RSX_DRAW + init succeeded). It accumulates
     * clears/state/geometry and self-presents on the 0xE944 flip method. This
     * runs before the plumbing below so the engine also sees the flip. */
    const int nr_flip_selected =
        method == 0xE944u && g_yz_nr_shadow_enabled &&
        rsx_nr_family_enabled(&g_yz_nr_shadow, RSX_NR_FAM_FLIP);
    const int nr_clear_selected =
        (method & 0x1FFCu) == 0x1D94u && g_yz_nr_shadow_enabled &&
        rsx_nr_family_enabled(&g_yz_nr_shadow, RSX_NR_FAM_CLEAR);
    const int nr_draw_selected =
        (method & 0x1FFCu) == 0x1808u && arg == 0u &&
        g_yz_nr_shadow_enabled &&
        rsx_nr_family_enabled(&g_yz_nr_shadow, RSX_NR_FAM_DRAW);
    const int nr_flip_owned =
        nr_flip_selected ? yz_nr_try_live_flip(arg) : 0;
    const int nr_clear_owned =
        nr_clear_selected ? yz_nr_try_live_clear(arg) : 0;
    const int nr_draw_owned =
        nr_draw_selected ? yz_nr_try_live_draw() : 0;
    if (!nr_flip_owned && !nr_clear_owned && !nr_draw_owned) {
        rsx_live_draw_method(method, arg);
        if (nr_flip_selected || nr_clear_selected || nr_draw_selected)
            yz_nr_live_flip_fallback_complete();
    }

    if (method >= 0xA400 && method < 0xAB00) {     /* NV308A_COLOR window */
        uint32_t index = (method - 0xA400) >> 2;
        uint32_t out_x = yz_rsx_blit_size_out & 0xFFFFu;
        static int a010_blit_trace = -1;
        static unsigned a010_blit_n = 0;
        if (a010_blit_trace < 0)
            a010_blit_trace = getenv("YZ_A010_BLIT") ? 1 : 0;
        const int trace_a010_blit =
            a010_blit_trace &&
            InterlockedCompareExchange(&g_yz_a010_root_active, 0, 0) != 0 &&
            a010_blit_n < 128u;
        if (index >= out_x)
        {
            if (trace_a010_blit) {
                a010_blit_n++;
                fprintf(stderr,
                        "[a010-blit] n=%u method=0x%X index=%u/%u "
                        "CLIPPED dma=%08X off=%08X point=%08X arg=%08X\n",
                        a010_blit_n, method, index, out_x,
                        yz_rsx_blit_dst_dma, yz_rsx_blit_dst_off,
                        yz_rsx_blit_point, arg);
                fflush(stderr);
            }
            return 0;                              /* clipped: skip */
        }
        if (yz_rsx_blit_fmt != 0xB && yz_rsx_blit_fmt != 0x8 /* y32 */) {
            static int warned = 0;
            if (!warned) {
                warned = 1;
                fprintf(stderr, "[rsx] NV308A color format 0x%X unsupported\n",
                        yz_rsx_blit_fmt);
            }
            return 0;
        }
        uint32_t x = (yz_rsx_blit_point & 0xFFFFu) + index;
        uint32_t y = yz_rsx_blit_point >> 16;
        uint32_t pitch = yz_rsx_blit_pitch >> 16;
        uint32_t addr = yz_rsx_sem_addr(yz_rsx_blit_dst_dma,
                                        yz_rsx_blit_dst_off + x * 4 + y * pitch);
        if (addr) {
            yz_rsx_w32(addr, arg);
            rsx_live_draw_note_inline_transfer(
                yz_rsx_blit_dst_dma,
                yz_rsx_blit_dst_off + x * 4 + y * pitch, arg);
        }
        if (trace_a010_blit) {
            a010_blit_n++;
            fprintf(stderr,
                    "[a010-blit] n=%u method=0x%X index=%u/%u "
                    "dma=%08X off=%08X point=%08X pitch=%08X "
                    "addr=%08X arg=%08X readback=%08X\n",
                    a010_blit_n, method, index, out_x,
                    yz_rsx_blit_dst_dma, yz_rsx_blit_dst_off,
                    yz_rsx_blit_point, yz_rsx_blit_pitch,
                    addr, arg, addr ? vm_read32(addr) : 0u);
            fflush(stderr);
        }
        return 0;
    }

    /* GCM_DRIVER_QUEUE (0xE940 + head*4): the game queues a display flip here
     * (RPCS3 gcm::queue_flip -> sys_rsx 0x103; rsx_methods.cpp:1730). arg = the
     * display-buffer id. Record it on the head; the actual "submit" is the
     * flip-sema RELEASE that follows (which arms `pending`). */
    if (method >= 0xE940 && method <= 0xE95C) {
        uint32_t head = ((method - 0xE940) >> 2) & 7u;
        g_rsx_queued_head = head;
        { static int n = 0; if (n < 8) { n++;
            fprintf(stderr, "[rsx] DRIVER_QUEUE head=%u buf=%u (flip queued)\n", head, arg); } }
        if (yz_ft_on()) yz_ft("QUEUE head=%u buf=%u", head, arg);
        /* s23: route through the pkg-0x103 syscall case instead of the old
         * inline duplicate -- RPCS3's gcm::queue_flip method IS that shim
         * (rsx_methods.cpp:101-113 -> sys_rsx 0x103), and the syscall case now
         * also delivers the queue event; boot9 measured the game queues ONLY
         * via this method (the 0x103 syscall itself never fires), so the
         * event delivery was dead until this bridge. */
        ppu_context sc; memset(&sc, 0, sizeof(sc));
        sc.gpr[4] = 0x103;
        sc.gpr[5] = head;
        sc.gpr[6] = arg;
        yz_sys_rsx_context_attribute(&sc);
        return 0;
    }

    /* GCM_FLIP_HEAD (0xE920 + head*4): the IMMEDIATE display flip driven from
     * the GPU command stream (the cellGcmSetFlipImmediate path) -- sibling of
     * the DRIVER_QUEUE path above and the same lv1 driver-method family as the
     * 0xEB00 user interrupt fixed in s22. Our consumer silently dropped it
     * (method coverage audit 2026-07-08, top-ranked gap of blocker #20's
     * missing-mechanism class). RPCS3 binds it as a thin shim onto the SAME
     * syscall our pkg-0x102 case already implements: rsx_methods.cpp:1729
     * bind_range<GCM_FLIP_HEAD,1,2,gcm::driver_flip> ->
     * sys_rsx_context_attribute(0x102, head, arg) (rsx_methods.cpp:93-99,
     * sys_rsx.cpp:574-627: arg bit31 = grab-queued-buffer, else display-buffer
     * offset). Route it there. Kill-switch YZ_NO_FLIPHEAD. */
    if (method == 0xE920 || method == 0xE924) {
        uint32_t head = (method - 0xE920) >> 2;
        static unsigned long fn = 0; fn++;
        if (fn <= 16 || (fn & 0x3FFu) == 0) {
            fprintf(stderr, "[fliphead] n=%lu head=%u arg=0x%08X%s\n",
                    fn, head, arg, nf ? " (DROPPED by YZ_NO_FLIPHEAD)" : "");
            fflush(stderr);
        }
        if (yz_ft_on()) yz_ft("FLIP_HEAD head=%u arg=0x%08X", head, arg);
        if (!nf) {
            ppu_context sc; memset(&sc, 0, sizeof(sc));
            sc.gpr[4] = 0x102;                 /* pkg: display flip */
            sc.gpr[5] = head;
            sc.gpr[6] = arg;
            yz_sys_rsx_context_attribute(&sc);
        }
        return 0;
    }

    uint32_t addr;
    switch (method) {
    case 0x6184:                                  /* NV3062 DMA_IMAGE_SOURCE */
        break;
    case 0x6188:                                  /* NV3062 DMA_IMAGE_DESTIN */
        yz_rsx_blit_dst_dma = arg;
        break;
    case 0x6300:                                  /* NV3062 SET_COLOR_FORMAT */
        yz_rsx_blit_fmt = arg & 0xFFFFu;
        break;
    case 0x6304:                                  /* NV3062 SET_PITCH (pitch<<16|alignment) */
        yz_rsx_blit_pitch = arg;
        break;
    case 0x630C:                                  /* NV3062 SET_OFFSET_DESTIN */
        yz_rsx_blit_dst_off = arg;
        break;
    case 0xA304:                                  /* NV308A_POINT (y<<16|x) */
        yz_rsx_blit_point = arg;
        break;
    case 0xA308:                                  /* NV308A_SIZE_OUT */
        yz_rsx_blit_size_out = arg;
        break;
    }
    switch (method) {
    case 0xEB00:                                  /* GCM_SET_USER_COMMAND (user interrupt) */
    case 0xEB04:
        yz_rsx_exec_user_command(method, arg);
        break;
    case 0x050:                                   /* NV406E SET_REFERENCE */
        yz_rsx_exec_set_reference(arg);
        break;
    case 0x060:                                   /* NV406E SET_CONTEXT_DMA_SEMAPHORE */
        yz_rsx_sem_dma_406e = arg;
        break;
    case 0x064:                                   /* NV406E SEMAPHORE_OFFSET */
        yz_rsx_sem_off_406e = arg;
        break;
    case 0x068:                                   /* NV406E SEMAPHORE_ACQUIRE */
        /*
         * The a010 missing-release recovery resumes a linked display-list
         * chain after an old self-stop.  That chain's FE0 completion wait can
         * be reached before its usual SET_CONTEXT_DMA_SEMAPHORE packet, while
         * the inherited selector still names DEVICE_R from the preceding
         * flip-credit wait.  FE0 is the monotonic reports-label counter; the
         * only legitimate device-memory semaphore in this protocol is +0x30.
         * Restore the reports selector at this exact recovered boundary.
         */
        if (yz_rsx_sem_off_406e == 0xFE0u &&
            (yz_rsx_sem_dma_406e == 0x56616660u ||
             yz_rsx_sem_dma_406e == 0x56616661u) &&
            InterlockedCompareExchange(
                &g_yz_a010_root_active, 0, 0) != 0 &&
            yz_a010_fifo_publication_repair_enabled()) {
            static unsigned long context_repairs = 0;
            yz_rsx_sem_dma_406e = 0x66616661u;
            context_repairs++;
            if (context_repairs <= 16 ||
                (context_repairs & (context_repairs - 1u)) == 0u) {
                fprintf(stderr,
                        "[a010-sem-context-repair] n=%lu "
                        "off=0x%X DEVICE_R -> SEMAPHORE_R\n",
                        context_repairs, yz_rsx_sem_off_406e);
                fflush(stderr);
            }
        }
        addr = yz_rsx_sem_addr(yz_rsx_sem_dma_406e, yz_rsx_sem_off_406e);
        /* Dedup: log only when the (addr,want) pair changes, so a NEW stall
         * surfaces without flooding on the per-poll retries. */
        { static int st=-1; static uint32_t la=0xDEAD, lw=0xDEAD;
          if (st < 0) st = getenv("YZ_RSX_SEM_TRACE") ? 1 : 0;
          if (st && (addr != la || arg != lw)) { la = addr; lw = arg;
            fprintf(stderr, "[sem] ACQUIRE off=0x%X addr=0x%08X want=0x%08X have=0x%08X %s\n",
                    yz_rsx_sem_off_406e, addr, arg, addr?vm_read32(addr):0,
                    (addr && vm_read32(addr)!=arg)?"STALL":"pass"); } }
        /* fliptrace: log the flip-protocol acquires (label / HW credit) on every
         * pass and on stall-episode ENTRY (retries of the same stalled acquire
         * are suppressed so the log stays readable). s21 widened: ANY label-area
         * acquire (0x102000xx) -- the movie phase gates its stream on a decode-
         * sync label at +0xFE0 (boot 9/12 terminal state). */
        if (yz_ft_on() &&
            ((addr >= RSX_REPORTS && addr < RSX_REPORTS + 0x1000u) ||
             addr == RSX_DEVICE_ADDR + 0x30)) {
            uint32_t have = vm_read32(addr);
            int ok = (have == arg);
            static uint32_t fla = 0; static int flr = -1;
            if (addr != fla || ok != flr) { fla = addr; flr = ok;
                yz_ft("ACQ addr=0x%08X want=0x%08X have=0x%08X %s",
                      addr, arg, have, ok ? "pass" : "STALL-enter"); }
        }
        if (addr && vm_read32(addr) != arg) {
            /* 0xFE0 is a monotonic completion counter.  During a010 recovery,
             * a wait older than the already-completed value is satisfied by
             * definition; requiring exact equality would deadlock forever on
             * resident pre-handoff ring history (measured 0x44B vs 0x479). */
            if (addr == RSX_REPORTS + 0xFE0u &&
                yz_a010_fifo_publication_repair_enabled()) {
                const uint32_t have = vm_read32(addr);
                const uint32_t ahead = have - arg;
                const uint32_t lag = arg - have;
                const uint32_t task_running =
                    vm_read32(0x42450E00u + 0x00u);
                const uint32_t task_ready =
                    vm_read32(0x42450E00u + 0x10u);
                const uint32_t task_signalled =
                    vm_read32(0x42450E00u + 0x40u);
                const uint32_t task_waiting =
                    vm_read32(0x42450E00u + 0x50u);
                const int known_pool_parked =
                    task_running == 0u && task_ready == 0u &&
                    task_signalled == 0u && task_waiting != 0u;
                static uint32_t stalled_arg = 0xFFFFFFFFu;
                static uint32_t stalled_have = 0xFFFFFFFFu;
                static ULONGLONG stalled_since = 0;
                static ULONGLONG last_fallback_retry = 0;
                const ULONGLONG stall_now = GetTickCount64();
                if (stalled_arg != arg || stalled_have != have) {
                    stalled_arg = arg;
                    stalled_have = have;
                    stalled_since = stall_now;
                }
                const int overdue_exact_stall =
                    stall_now - stalled_since >= 2000u &&
                    stall_now - last_fallback_retry >= 2000u;
                /*
                 * Reduced-overhead runs exposed a lost wid4 wake after the
                 * user callback had fully staged the requested round.  This
                 * protocol is active during boot as well as a010, so its exact
                 * lost-wake repair must not be scene-gated. Retry exactly once
                 * only when the acquire is a small number of rounds ahead,
                 * the callback completed that exact cause, and either the
                 * known pool has every worker parked or the exact (want,have)
                 * pair has made no progress for two seconds.  The latter is a
                 * fallback for boot phases whose wid4 taskset is not yet at
                 * the later a010 address.  The callback's task signal is a
                 * level bit, so replaying the same cause is idempotent; the
                 * two-second spacing also prevents a slow live worker from
                 * receiving a burst of duplicate callbacks.
                 */
                if (yz_fe0_callback_replay_enabled() &&
                    lag != 0u && lag <= 8u &&
                    vm_read32(RSX_DRIVER_INFO + 0x12CCu) == arg &&
                    (uint32_t)_InterlockedCompareExchange(
                        &g_yz_ucmd_handler_completed, 0, 0) == arg &&
                    (known_pool_parked || overdue_exact_stall) &&
#if defined(YZ_NATIVE_GCM)
                    1) {
#else
                    (vm_read32(RSX_DRIVER_INFO + 0x12C0u) & 0x80u) != 0u &&
                    g_rsx_event_port != 0u) {
#endif
                    static uint32_t last_reissued_cause = 0xFFFFFFFFu;
                    static uint32_t last_reissued_epoch = 0xFFFFFFFFu;
                    static unsigned reissue_attempt = 0;
                    const uint32_t completed_epoch =
                        (uint32_t)_InterlockedCompareExchange(
                            &g_yz_ucmd_handler_completed_epoch, 0, 0);
                    if (last_reissued_cause != arg) {
                        last_reissued_cause = arg;
                        last_reissued_epoch = 0xFFFFFFFFu;
                        reissue_attempt = 0;
                    }
                    /*
                     * A completed replay is a negative acknowledgement when
                     * the label is still behind and every task is parked
                     * again. Allow the next replay only after that completion
                     * epoch changes; FIFO polling alone cannot generate
                     * duplicates. The small cap keeps a genuinely broken
                     * protocol loud and bounded.
                     */
                    if (last_reissued_epoch != completed_epoch &&
                        reissue_attempt < 8u) {
                        last_reissued_epoch = completed_epoch;
                        reissue_attempt++;
                        if (!known_pool_parked)
                            last_fallback_retry = stall_now;
#if defined(YZ_NATIVE_GCM)
                        cellGcmDispatchUserCommand(arg);
                        const int64_t retry = 0;
#else
                        const int64_t retry = yz_rsx_ev_send(0x80ull);
#endif
#if !defined(YZ_PERF_CLEAN)
                        fprintf(stderr,
                                "[fifo-sync] redelivered staged cause="
                                "0x%08X have=0x%08X lag=%u epoch=%u "
                                "attempt=%u parked=%d "
                                "task{run=%08X ready=%08X sig=%08X "
                                "wait=%08X} send=%lld\n",
                                arg, have, lag,
                                completed_epoch, reissue_attempt,
                                known_pool_parked,
                                task_running, task_ready,
                                task_signalled, task_waiting,
                                (long long)retry);
                        fflush(stderr);
#else
                        (void)retry;
#endif
                    }
                }
                if (InterlockedCompareExchange(
                        &g_yz_a010_root_active, 0, 0) != 0 &&
                    ahead != 0u && ahead < 0x10000u) {
                    static unsigned long stale_acquire = 0;
                    stale_acquire++;
                    if (stale_acquire <= 16 ||
                        (stale_acquire & 0xFFu) == 0u) {
                        fprintf(stderr,
                                "[a010-stale-acquire] passed n=%lu "
                                "want=0x%08X completed=0x%08X\n",
                                stale_acquire, arg, have);
                        fflush(stderr);
                    }
                    break;
                }
            }
            /* This recomp runtime cannot block while holding g_rsx_fifo_lock:
             * unlike RPCS3's thread model, the runtime vblank/event publisher may need
             * work that is serialized behind this consumer. Leave GET on the
             * packet and retry later so the external publisher remains free to
             * satisfy the acquire. Movie output EOS is synchronized separately
             * at the host/guest lifecycle boundary. */
            { static int st=-1; static uint32_t ha=0, hw=0; static unsigned long hn=0;
              if (st < 0) st = getenv("YZ_RSX_SEM_TRACE") ? 1 : 0;
              if (st && addr==ha && arg==hw) { hn++;
                  if ((hn & 0xFFFFu)==0) {
                      fprintf(stderr, "[sem-hb] addr=0x%08X want=0x%08X read=0x%08X retries=%lu\n",
                              addr, arg, vm_read32(addr), hn);
                      fflush(stderr); } }
              else if (st) { ha=addr; hw=arg; hn=0; } }
            return 1;                             /* not yet satisfied: stall, retry later */
        }
        break;
    case 0x06C:                                   /* NV406E SEMAPHORE_RELEASE */
        yz_nr_vertical_406e_release(
            yz_rsx_sem_dma_406e, yz_rsx_sem_off_406e, arg);
        break;
    case 0x1A4:                                   /* NV4097 SET_CONTEXT_DMA_SEMAPHORE */
        yz_rsx_sem_dma_4097 = arg;
        break;
    case 0x1D6C:                                  /* NV4097 SET_SEMAPHORE_OFFSET */
        yz_rsx_sem_off_4097 = arg;
        break;
    case 0x1D70:                                  /* BACK_END_WRITE_SEMAPHORE_RELEASE */
        addr = yz_rsx_sem_addr(yz_rsx_sem_dma_4097, yz_rsx_sem_off_4097);
        { static int sl=0; if (sl<60){ sl++;
            fprintf(stderr, "[sem] BE_RELEASE off=0x%X addr=0x%08X val=0x%08X(arg=0x%08X)\n",
                    yz_rsx_sem_off_4097, addr,
                    (arg & 0xFF00FF00u)|((arg&0xFFu)<<16)|((arg>>16)&0xFFu), arg); } }
        if (yz_ft_on() && addr >= RSX_REPORTS && addr < RSX_REPORTS + 0x1000u)
            yz_ft("BE-REL addr=0x%08X arg=0x%08X", addr, arg);
        if (addr)
            yz_rsx_w32(addr, (arg & 0xFF00FF00u) |
                             ((arg & 0xFFu) << 16) | ((arg >> 16) & 0xFFu));
        break;
    case 0x1D74:                                  /* TEXTURE_READ_SEMAPHORE_RELEASE */
        addr = yz_rsx_sem_addr(yz_rsx_sem_dma_4097, yz_rsx_sem_off_4097);
        if (addr)
            yz_rsx_w32(addr, arg);
        break;
    default:
        /* NV3062/NV308A 2D methods were handled by the first switch above;
         * anything else is a 3D rendering method -> toolkit NV4097 translator
         * (tracks state, drives the backend clear/draw). Unknown methods are
         * ignored there (and self-logged in debug builds). */
        if (!((method >= 0x6000 && method < 0x7000) ||
              (method >= 0xA000 && method < 0xB000)))
            rsx_process_method(&g_rsx_state, method, arg);
        break;
    }
    return 0;
}

extern "C" int yz_nr_vertical_mirror_legacy_method(
    uint32_t method, uint32_t arg, uint32_t suppress_action)
{
    if (suppress_action) {
        rsx_live_draw_mirror_state_method(method, arg);
        return 0;
    }
    return yz_rsx_method(method, arg);
}

/* FIFO consumer (RPCS3 FIFO_control model -- Emu/RSX/RSXFIFO.cpp).
 *
 * Under LLE, Sony's libgcm owns the ring and the game (t1) produces commands +
 * advances PUT; this thread is the RSX side that consumes [GET, PUT) and
 * advances GET. The two invariants that keep it from racing t1's live writes:
 *   1. RE-READ PUT before every command -- never read past the committed
 *      boundary into a segment t1 is still filling.
 *   2. WRITE GET BACK on every advance -- t1's get-based flow control reuses
 *      ring space only after GET passes it; a laggy/bogus GET makes t1 rewrite
 *      memory we're mid-read (the old hand-rolled consumer's bug).
 * Process ONE command per iteration (no aggressive drain with a stale PUT), and
 * idle at the jump-to-self stopper t1 places at PUT (re-poll until t1 patches
 * it). On an unsatisfied semaphore acquire, leave GET in place and retry. */

/* DIAG (TEMP, strip before commit): control-transfer ring. Records the last 64
 * jumps/calls/returns the consumer FOLLOWED so that when it parks on a STALE
 * jump-to-self (one with PUT ahead of GET -- the deadlock signature) we can dump
 * the exact path GET took into the island, plus the ring topology. Pins whether
 * the consumer took a wrong jump (path divergence) or the stopper is an unpatched
 * barrier. (blocker #21) */
struct yz_ct_ent { uint32_t from, cmd, to; const char* kind; };
static yz_ct_ent g_ct_ring[64];
static unsigned  g_ct_head = 0;
static inline void yz_ct_push(uint32_t from, uint32_t cmd, uint32_t to, const char* kind) {
    yz_ct_ent& e = g_ct_ring[g_ct_head & 63u];
    e.from = from; e.cmd = cmd; e.to = to; e.kind = kind; g_ct_head++;
}
static void yz_ct_dump(uint32_t parkget, uint32_t put) {
    fprintf(stderr, "[ct] === consumer parked on STALE jump-to-self io=0x%06X (PUT=0x%06X ahead) ===\n",
            parkget, put);
    fprintf(stderr, "[ct] last %u control transfers (oldest first):\n",
            g_ct_head < 64u ? g_ct_head : 64u);
    unsigned start = g_ct_head >= 64u ? g_ct_head - 64u : 0u;
    for (unsigned i = start; i < g_ct_head; i++) {
        yz_ct_ent& e = g_ct_ring[i & 63u];
        fprintf(stderr, "    %-4s io=0x%06X cmd=0x%08X -> io=0x%06X\n",
                e.kind, e.from, e.cmd, e.to);
    }
    fprintf(stderr, "[ct] fragment heads (io F -> [F],[F+4],[F+8]) and tails ([F+0xFFFF8],[F+0xFFFFC]):\n");
    for (uint32_t F = 0; F <= 0x700000u; F += 0x100000u) {
        uint32_t h0 = yz_rsx_io_to_ea(F), h1 = yz_rsx_io_to_ea(F + 4), h2 = yz_rsx_io_to_ea(F + 8);
        uint32_t t0 = yz_rsx_io_to_ea(F + 0xFFFF8u), t1 = yz_rsx_io_to_ea(F + 0xFFFFCu);
        fprintf(stderr, "    io 0x%06X head %08X %08X %08X | tail %08X %08X\n", F,
                h0 ? vm_read32(h0) : 0, h1 ? vm_read32(h1) : 0, h2 ? vm_read32(h2) : 0,
                t0 ? vm_read32(t0) : 0, t1 ? vm_read32(t1) : 0);
    }
    /* what does frame 3 (the live region up to PUT) look like at its head + just below PUT? */
    fprintf(stderr, "[ct] live region: words around PUT=0x%06X:\n", put);
    for (uint32_t off = (put >= 0x10u ? put - 0x10u : 0u); off <= put + 0x8u; off += 4u) {
        uint32_t e = yz_rsx_io_to_ea(off);
        fprintf(stderr, "    io 0x%06X = %08X%s\n", off, e ? vm_read32(e) : 0,
                off == put ? "  <- PUT" : "");
    }
}

extern "C" void yz_watch_arm(uint32_t);   /* main.cpp page-guard write-watch (TEMP) */
extern "C" uint32_t g_yz_game_toc;        /* dispatch.cpp: the game module's TOC */

/* PARSE-TRACE ring (TEMP DIAG, blocker #21): the last method packets the consumer
 * actually parsed (with their counts), so a CALL-not-ready park can be checked for
 * ALIGNMENT -- is GET on a real packet boundary (the CALL is genuine -> blocked
 * producer) or did a mis-counted method land us mid-packet on an ARG misread as a
 * CALL (a phantom list = a consumer parse bug, not a producer block)? */
struct yz_mt_ent { uint32_t get, cmd; uint16_t count, method; };
static yz_mt_ent g_mt_ring[64];
static unsigned  g_mt_head = 0;
static inline void yz_mt_push(uint32_t g, uint32_t c, uint32_t cnt, uint32_t m) {
    yz_mt_ent& e = g_mt_ring[g_mt_head & 63u];
    e.get = g; e.cmd = c; e.count = (uint16_t)cnt; e.method = (uint16_t)m; g_mt_head++;
}
static void yz_mt_dump(uint32_t parkget, uint32_t parkcmd) {
    static int done = 0; if (done) return; done = 1;
    fprintf(stderr, "[mt] parked at CALL io=0x%06X cmd=0x%08X; last %u method packets parsed "
            "(next == this.get+4+count*4 should chain to the CALL if aligned):\n",
            parkget, parkcmd, g_mt_head < 64u ? g_mt_head : 64u);
    unsigned start = g_mt_head >= 64u ? g_mt_head - 64u : 0u;
    for (unsigned i = start; i < g_mt_head; i++) {
        yz_mt_ent& e = g_mt_ring[i & 63u];
        fprintf(stderr, "    io=0x%06X cmd=0x%08X m=0x%04X cnt=%-3u -> next io=0x%06X\n",
                e.get, e.cmd, e.method, e.count, e.get + 4u + (uint32_t)e.count * 4u);
    }
}

/* The game's gcm flush/reserve (func_00E9BC9C / func_00E9B630 / func_00E9BF14)
 * places a self-jump stopper at every commit and RELEASES the previous one. A
 * same-fragment release is IMMEDIATE -- it writes 0 (NOP) over the stopper word
 * (func_00E9BC9C @0xE9BE60: `stw r11,0(r9)` with r11=S[0x1C]=0). A CROSS-fragment
 * release (the commit spanned a gcm buffer recycle, so S[0x24]!=ctx->end) is
 * DEFERRED into the game's gcm op-list and applied later:
 *     S    = *(game_toc - 0x7410)                 (the gcm-state struct)
 *     list = [ S[+8] (base) .. S[+0] (write head) ), 0x20-byte entries
 *     entry[+0] = op tag (0x7F == deferred stopper-release), entry[+4] = stopper EA
 * The @0x300000 deadlock (blocker #21) is exactly this: io 0x300000's release was
 * deferred (verified live: list entry tag=0x7F ea=0x40700000) but the game never
 * drains the list -- it's stuck spinning on the flip fence that needs frame 3 to
 * execute, which needs this very stopper released. Since PUT is already past the
 * stopper, the body IS committed and the game HAS committed to releasing it, so
 * applying the deferred release the moment GET reaches the stopper is faithful to
 * the game's own intent (the accurate form of the 14c timer heuristic).
 * NOTE (2026-07-02): a "live-stopper guard" (refuse when stopper_ea == S[0x20])
 * was tried here and REFUTED 0/4 -- at GET-park + PUT-ahead the PUT position
 * already proves the guarded content is built, so releasing even the current
 * S[0x20] instance is correct (t1 releases it at its next flush anyway; the
 * early release only skips the wait). The match itself lives in
 * yz_gcm_stopper_release_entry below (returns the entry address so the
 * retirement sweep knows how far consumption has proven progress). */

/* ============================================================================
 * JOURNAL RETIREMENT SWEEP (2026-07-02) -- the faithful consumption contract.
 *
 * MEASURED (instrumented RPCS3 [jrnl-dma]/[jrnl-tags]): on
 * real HW the EDGE SPU task (gs_task) consumes the gcm journal and ZEROES each
 * entry's tag word; the producer polls chunk-head tags == 0 before reusing a
 * journal chunk, i.e. the tags are the game's GPU-PROGRESS LEDGER. Every
 * stand-in that zeroed tags AHEAD of actual FIFO consumption (eager, pending-
 * set, lag-by-one -- 0/8, 0/8, 0/4 boot loops) made the game believe work had
 * retired that our GET had not consumed, and it recycled ring segments under
 * GET (torn-content non-command wedges). The faithful rule that survives:
 * ZERO AN ENTRY ONLY WHEN GET HAS PROVABLY CONSUMED PAST IT. GET applying the
 * deferred release of entry A (it parked on A's stopper with PUT ahead) is
 * that proof for entries BEHIND the released region -- NOTE the caveat: the
 * journal orders a segment's patch entries BEFORE its entry-stopper release,
 * so "through A" retires patches for content GET is only ENTERING (a
 * suspected over-retirement; could not be cleanly validated 2026-07-02
 * because the watchdog instrumentation invalidated the loops). OPT-IN
 * (YZ_JRNL=1) until it can be measured against a clean baseline; the REAL
 * fix in flight is restoring the actual consumer (gs_task residency,
 * trace-diff). Data/sublist payloads (tags 4/8/9/D/10)
 * are EDGE content-generation, not flow -- they stay unapplied until the
 * real consumer era. ===================================================== */
static uint32_t g_jrnl_retire_cursor = 0;

static void yz_jrnl_retire_through(uint32_t entry_addr)
{
    static int on = -1;
    if (on < 0) on = getenv("YZ_JRNL") ? 1 : 0;
    if (!on || !g_yz_game_toc) return;
    const uint32_t S = vm_read32(g_yz_game_toc - 0x7410u);
    if (S < 0x10000u || S >= 0xE0000000u) return;
    const uint32_t base = vm_read32(S + 0x08u);
    const uint32_t aend = vm_read32(S + 0x0Cu);
    if (base < 0x10000u || aend <= base || aend - base > 0x1000000u) return;
    if (entry_addr < base || entry_addr >= aend) return;
    uint32_t cur = g_jrnl_retire_cursor;
    if (cur < base || cur >= aend) cur = base;
    /* linear walk with arena wrap; hard cap for safety */
    unsigned zeroed = 0, guardn = 0;
    while (guardn++ < 0x20000u) {
        if (vm_read32(cur) != 0u) { vm_write32(cur, 0u); zeroed++; }
        const int done = (cur == entry_addr);
        cur += 0x20u;
        if (cur >= aend) cur = base;
        if (done) break;
    }
    g_jrnl_retire_cursor = cur;
    { static unsigned total = 0, logged = 0; total += zeroed;
      if (logged < 12 && zeroed) { logged++;
          fprintf(stderr, "[jrnl] retired %u entries through 0x%08X (%u total)\n",
                  zeroed, entry_addr, total); } }
}

/* ============================================================================
 * s40b PRODUCER THROTTLE (native-GCM default K=8; YZ_JRNL_THROTTLE=<K>
 * overrides it and YZ_NO_JRNL_THROTTLE=1 is the rollback).
 * scratch/s40b_findings.md sec.19-20.
 *
 * THE MEASURED ROOT of both terminal modes at the frame-~800 phase transition:
 * our backlog carries live release records ACROSS the game's phase-boundary
 * reinit (which zero-sweeps + rebuilds the dispatch vtables) — a state real
 * HW never reaches because releases complete in ~72us there and the engine is
 * EMPTY at the reinit. This throttle restores that invariant by BACKPRESSURE
 * instead of speed: every [0x11][0x0A] journal append funnels through
 * func_00E7DE88 (single choke point, 4 call sites), whose head calls
 * yz_jrnl_throttle() (a ppu_recomp_006.cpp hand-edit, LOST ON RELIFT) —
 * t1 waits until the consumer's live poll cursor is within K lines of the
 * producer head before publishing more.
 *
 * FAIL-OPEN everywhere: no consumer yet / anchor wiped / cursor outside the
 * arena / arena unparsable => pass through untouched. Timeout escape per
 * append (YZ_JRNL_THROTTLE_MS, default 200) so a consumer wedge can never
 * deadlock t1; 64 CONSECUTIVE timeouts disarm the throttle for the rest of
 * the boot (past the wall the consumer is gone and waiting only burns time).
 * Caught-up slack: the consumer's cursor legitimately polls AT or slightly
 * AHEAD of head between appends — a small forward window reads as gap 0,
 * not as a wrapped full ring. */
extern "C" uint32_t yz_consumer_cursor(void);   /* runtime/spu/spu_channels.c */

extern "C" void yz_jrnl_throttle(void)
{
    static int on = -1; static uint32_t K = 8; static uint32_t budget_ms = 200;
    if (on < 0) {
        const char* e = getenv("YZ_JRNL_THROTTLE");
#if defined(YZ_NATIVE_GCM)
        on = getenv("YZ_NO_JRNL_THROTTLE") ? 0 : 1;
#else
        on = 0;
#endif
        if (!getenv("YZ_NO_JRNL_THROTTLE") && e && *e)
            on = 1;
        if (on) {
            int k = e ? atoi(e) : 0;
            if (k > 0 && k <= 4096) K = (uint32_t)k;
            const char* m = getenv("YZ_JRNL_THROTTLE_MS");
            if (m && atoi(m) > 0) budget_ms = (uint32_t)atoi(m);
            fprintf(stderr, "[jthr] ARMED K=%u lines budget=%ums "
                    "(producer throttle at append 0xE7DE88%s)\n",
                    K, budget_ms,
                    (e && *e) ? ", env override" : ", native-GCM default");
            fflush(stderr);
        } else if (getenv("YZ_NO_JRNL_THROTTLE")) {
            fprintf(stderr, "[jthr] DISABLED (YZ_NO_JRNL_THROTTLE)\n");
            fflush(stderr);
        }
    }
    static int disarmed = 0;
    if (!on || disarmed || !g_yz_game_toc) return;

    /* Call/fail-open census (review finding #1: an inert throttle must be
     * distinguishable from a healthy one — the ARMED banner alone is a false
     * witness since both hand-edit halves are lost on relift independently).
     * Sampled print keeps the log volume flat (LESSONS #6c). */
    static unsigned long ncall = 0, nofcur = 0, engaged = 0, timeouts = 0, consec_to = 0;
    static unsigned long long wait_ms_tot = 0; static unsigned long wait_ms_max = 0;
    static uint32_t lastgap = 0;    /* lines; the steady-state lag sample */
    ncall++;
    if ((ncall & 4095u) == 0) {
        fprintf(stderr, "[jthr] census ncall=%lu cursor-failopen=%lu waited=%lu to=%lu tot=%llums lastgap=%u\n",
                ncall, nofcur, engaged, timeouts, wait_ms_tot, lastgap);
        fflush(stderr);
    }

    const uint32_t S = vm_read32(g_yz_game_toc - 0x7410u);
    if (S < 0x10000u || S >= 0xE0000000u) return;
    const uint32_t base = vm_read32(S + 0x08u);
    const uint32_t aend = vm_read32(S + 0x0Cu);
    if (base < 0x10000u || aend <= base || aend - base > 0x1000000u) return;
    const uint32_t span = aend - base;
    const uint32_t bound = K * 0x80u;
    if (bound >= span / 2u) {                               /* review finding #9 */
        static int warned = 0;
        if (!warned) { warned = 1;
            fprintf(stderr, "[jthr] K=%u out of range for span=0x%X — throttle inert\n", K, span);
            fflush(stderr); }
        return;
    }

    unsigned long long t0 = 0; unsigned long spins = 0;
    for (;;) {
        const uint32_t cur = yz_consumer_cursor();
        if (cur < base || cur >= aend) { nofcur++; return; } /* fail-open (incl. 0) */
        const uint32_t head = vm_read32(S + 0x00u);
        if (head < base || head >= aend) return;            /* fail-open */
        /* Caught-up slack (review findings #2/#3): the consumer's cursor may
         * legitimately poll AT or a few lines PAST head between appends; a
         * cursor up to 8 lines ahead reads as gap 0. A cursor "ahead" by more
         * than that with a near-full ring behind it means the consumer LAPPED
         * — unrecoverable by pacing anyway, so failing open there is the safe
         * direction (no lap counter exists to disambiguate). */
        const uint32_t ahead = (cur - head + span) % span;
        const uint32_t gap = (ahead <= 0x400u) ? 0 : (head - cur + span) % span;
        lastgap = gap / 0x80u;
        { /* one-shot LIVE witness: proves both hand-edit halves are wired
           * (publish + call) and the gap math sees real values */
            static int live = 0;
            if (!live) { live = 1;
                fprintf(stderr, "[jthr] LIVE head=0x%08X cur=0x%08X gap=%u lines (arena 0x%08X..0x%08X)\n",
                        head, cur, gap / 0x80u, base, aend);
                fflush(stderr); }
        }
        if (gap <= bound) {
            consec_to = 0;      /* any pass (fast or waited) breaks the streak */
            if (t0) {                                       /* we waited and won */
                const unsigned long w = (unsigned long)(GetTickCount64() - t0);
                engaged++; wait_ms_tot += w;
                if (w > wait_ms_max) wait_ms_max = w;
                if (engaged <= 4 || (engaged & 4095u) == 0) {
                    fprintf(stderr, "[jthr] n=%lu wait=%lums tot=%llums max=%lums to=%lu\n",
                            engaged, w, wait_ms_tot, wait_ms_max, timeouts);
                    fflush(stderr);
                }
            }
            return;
        }
        const unsigned long long now = GetTickCount64();
        if (!t0) t0 = now;
        else if (now - t0 >= budget_ms) {                   /* timeout escape */
            timeouts++; consec_to++; engaged++;
            wait_ms_tot += (unsigned long long)(now - t0);
            if (timeouts <= 8 || (timeouts & 255u) == 0) {
                fprintf(stderr, "[jthr] TIMEOUT #%lu (consec %lu) gap=%u lines head=0x%08X cur=0x%08X\n",
                        timeouts, consec_to, gap / 0x80u, head, cur);
                fflush(stderr);
            }
            if (consec_to >= 64) {
                disarmed = 1;
                fprintf(stderr, "[jthr] DISARMED after %lu consecutive timeouts (consumer gone; n=%lu tot=%llums)\n",
                        consec_to, engaged, wait_ms_tot);
                fflush(stderr);
            }
            return;                                         /* fail-open */
        }
        /* backoff by iteration count (GetTickCount64 is ~15.6ms-granular —
         * time-based thresholds below one tick would hot-spin a core) */
        spins++;
        if (spins < 256) YieldProcessor();
        else Sleep(spins < 2048 ? 0 : 1);
    }
}

/* Locate the journal entry for a deferred release (same match as
 * yz_gcm_stopper_release_deferred but returns the ENTRY ADDRESS so the
 * retirement sweep knows how far GET's consumption has proven progress). */
/* Region of the last yz_gcm_stopper_release_entry() hit: 0 = none,
 * 1 = ordered window [base..head), 2 = above head [head..end) — an entry
 * written before the ring's head wrapped this lap, or a stale prior-lap
 * entry for a recycled stopper EA. Racy diagnostic (consumer + poller
 * threads); callers read it immediately after the call, and every caller
 * treats region 2 in the REFUSAL direction, so a torn read cannot widen
 * the repair. */
static uint32_t g_yz_relentry_region = 0;

static uint32_t yz_gcm_stopper_release_entry(uint32_t stopper_ea)
{
    g_yz_relentry_region = 0;
    if (!g_yz_game_toc) return 0;
    uint32_t S = vm_read32(g_yz_game_toc - 0x7410u);
    if (S < 0x10000u || S >= 0xE0000000u) return 0;
    uint32_t base = vm_read32(S + 0x08u);
    uint32_t head = vm_read32(S + 0x00u);
    uint32_t end = vm_read32(S + 0x0Cu);
    if (base < 0x10000u || base >= 0xE0000000u) return 0;
    if (head < base || (head - base) > 0x1000000u) return 0;
    for (uint32_t e = base; e < head; e += 0x20u)
        if (vm_read32(e + 0x00u) == 0x7Fu && vm_read32(e + 0x04u) == stopper_ea) {
            g_yz_relentry_region = 1u;
            return e;
        }
    /* WRAP-AWARE LEG (2026-08-06, boot-62 ring decode): the journal is a
     * ring [base..end) (end = S+0x0C, 0x42100080 live) and head WRAPS to
     * base; the linear scan above is blind to entries still live in
     * [head..end). Every pre-08-06 "journal-ABSENT" verdict carried that
     * blindness (boot-62 repairs fired with head=0x41F00E20 just after a
     * wrap). An above-head match may also be a stale prior-lap entry for a
     * recycled stopper EA, so callers must treat region 2 conservatively:
     * journal-owned, wait — never a reason to jump. */
    if (end > head && end - base <= 0x1000000u)
        for (uint32_t e = head; e + 0x20u <= end; e += 0x20u)
            if (vm_read32(e + 0x00u) == 0x7Fu &&
                vm_read32(e + 0x04u) == stopper_ea) {
                g_yz_relentry_region = 2u;
                return e;
            }
    return 0;
}

/*
 * a010 missed-immediate-release recovery.
 *
 * A committed stopper has exactly two legitimate release paths in the game's
 * flush routine: a direct patch of the self-jump, or a tag-0x7F journal entry.
 * The orphanage failure reaches a third, impossible state: GET remains parked
 * on the old self-jump, PUT is already beyond it, no matching 0x7F exists, and
 * S[0x20] names the newer stopper exactly at PUT.  The same state was later
 * captured after the a020 movie at frame 3214.  That last condition proves the
 * producer finished the old commit and moved its pending-stopper cursor; this
 * is a FIFO publication invariant, not an a010 scene-lifetime invariant, and
 * it is not the normal case where RSX merely caught up with an unfinished
 * producer.
 *
 * Re-issue only the missing direct patch after the journal head, pending
 * stopper, and PUT form a stable snapshot.  The clean FIFO publication repair
 * applies wherever this exact proof holds; observation-only release tracing
 * remains restricted to the active a010 AUTH root.  It deliberately does not
 * consume or retire any journal entry.
 *
 * A direct release is not invariably "old stopper -> old + 4".  EDGE reserves
 * data islands in the FIFO and links around them; in a010, treating two such
 * releases as +4 parked GET in vertex-program/geometry data (0x004E0008 and
 * 0x4006954B).
 *
 * The generated draw substreams have a measured, exact eight-word prologue:
 * SET_VERTEX_DATA_ARRAY_OFFSET(0), three SET_VERTEX_DATA_BASE_OFFSET(0)
 * packets, followed by the generated vertex-array declarations.  The bytes
 * immediately before that prologue are vertex payload, while following the
 * substream's own jumps reaches the later user-command/fence packets.  The
 * correct pending chain is the earliest prologue whose flow graph reaches
 * USER_COMMAND(label[0xFE0] + 1).  This matters because the committed window
 * also contains older, still-valid command chains: taking the first prologue
 * replayed waits 0x458..0x475, while the live label was already 0x475.  The
 * pending-cause anchor selects the chain that ends in cause 0x476 instead.
 */
static int yz_a010_generated_prologue_at(uint32_t io)
{
    const uint32_t ea = yz_rsx_io_to_ea(io);
    return ea &&
           vm_read32(ea + 0x00u) == 0x00041710u &&
           vm_read32(ea + 0x04u) == 0u &&
           vm_read32(ea + 0x08u) == 0x00041714u &&
           vm_read32(ea + 0x0Cu) == 0u &&
           vm_read32(ea + 0x10u) == 0x00041714u &&
           vm_read32(ea + 0x14u) == 0u &&
           vm_read32(ea + 0x18u) == 0x00041714u &&
           vm_read32(ea + 0x1Cu) == 0u &&
           (vm_read32(ea + 0x20u) & 0x3FFFCu) == 0x1740u &&
           (vm_read32(ea + 0x28u) & 0x3FFFCu) == 0x1680u;
}

static uint32_t yz_a010_find_generated_prologue(uint32_t start, uint32_t put)
{
    const uint32_t ring = 0x800000u;
    const uint32_t mask = ring - 1u;
    const uint32_t ahead = (put - start + ring) & mask;
    if (ahead == 0u || ahead >= (ring >> 1))
        return 0u;

    for (uint32_t delta = 0u; delta + 0x30u <= ahead; delta += 4u) {
        const uint32_t candidate = (start + delta) & mask;
        if (yz_a010_generated_prologue_at(candidate))
            return candidate;
    }
    return 0u;
}

/* Dry-run an RSX command chain without applying a single method.  The recovery
 * is allowed to release an old stopper only when the proposed entry reaches
 * BOTH the next monotonic USER_COMMAND packet and a subsequent jump-to-self
 * stopper inside the producer's committed window.  This rejects a valid prefix
 * that later falls through into an EDGE data island -- the failure mode of the
 * earlier path_reaches(target) test. */
static int yz_a010_chain_complete(uint32_t start,
                                  uint32_t target,
                                  uint32_t window_start,
                                  uint32_t put,
                                  uint32_t* out_steps,
                                  uint32_t* out_end)
{
    const uint32_t ring = 0x800000u;
    const uint32_t mask = 0x7FFFFFu;
    uint32_t pc = start;
    uint32_t ret = ~0u;
    int reached_target = 0;
    const uint32_t committed = (put - window_start + ring) & mask;
    if (committed == 0u || committed >= (ring >> 1))
        return 0;

    for (uint32_t step = 0; step < 0x40000u; step++) {
        if (pc == target)
            reached_target = 1;

        /* Fixed RSX subroutines live outside the 8 MiB FIFO ring and are
         * reachable only while a CALL return is live.  Every ring address must
         * remain inside the producer-published [window_start, PUT] window. */
        if (pc < ring) {
            const uint32_t delta = (pc - window_start + ring) & mask;
            if (delta > committed)
                return 0;
        } else if (ret == ~0u) {
            return 0;
        }

        const uint32_t ea = yz_rsx_io_to_ea(pc);
        if (!ea)
            return 0;
        const uint32_t cmd = vm_read32(ea);
        if ((cmd & 0xE0000003u) == 0x20000000u ||
            (cmd & 3u) == 1u) {
            const uint32_t next =
                (cmd & 3u) == 1u ? (cmd & 0xFFFFFFFCu)
                                 : (cmd & 0x1FFFFFFCu);
            if (next == pc) {
                /* PUT can be several complete frame batches ahead while GET is
                 * stuck.  The first self-stop reached after the exact next
                 * completion packet is a finalized batch boundary and is the
                 * safe end of this one recovery step. */
                if (!reached_target || ret != ~0u)
                    return 0;
                if (out_steps)
                    *out_steps = step + 1u;
                if (out_end)
                    *out_end = pc;
                return 1;
            }
            if (!yz_rsx_io_to_ea(next))
                return 0;
            pc = next;
            continue;
        }
        if ((cmd & 3u) == 2u) {
            const uint32_t next = cmd & 0x1FFFFFFCu;
            if (ret != ~0u || !yz_rsx_io_to_ea(next))
                return 0;
            ret = (pc + 4u) & mask;
            pc = next;
            continue;
        }
        if ((cmd & 0xFFFF0003u) == 0x00020000u) {
            if (ret == ~0u)
                return 0;
            pc = ret;
            ret = ~0u;
            continue;
        }
        if (cmd & 0xA0030003u)
            return 0;
        const uint32_t count = (cmd >> 18) & 0x7FFu;
        const uint32_t bytes = 4u + count * 4u;
        pc = pc < ring ? ((pc + bytes) & mask) : (pc + bytes);
    }
    return 0;
}

/* The producer intentionally leaves the tail of a frame batch unpublished
 * until RSX consumes its USER_COMMAND completion marker.  Therefore the first
 * release must sometimes be proven only through that marker; consuming it
 * wakes the producer, which publishes the remainder and its next stopper.
 * Keep the same committed-window and one-level CALL checks as the full proof. */
static int yz_a010_chain_reaches_completion(uint32_t start,
                                            uint32_t target,
                                            uint32_t window_start,
                                            uint32_t put,
                                            uint32_t* out_steps)
{
    const uint32_t ring = 0x800000u;
    const uint32_t mask = ring - 1u;
    const uint32_t committed = (put - window_start + ring) & mask;
    uint32_t pc = start;
    uint32_t ret = ~0u;
    if (committed == 0u || committed >= (ring >> 1))
        return 0;

    for (uint32_t step = 0; step < 0x40000u; step++) {
        if (pc == target) {
            if (ret != ~0u)
                return 0;
            if (out_steps)
                *out_steps = step;
            return 1;
        }
        if (pc < ring) {
            const uint32_t delta = (pc - window_start + ring) & mask;
            if (delta > committed)
                return 0;
        } else if (ret == ~0u) {
            return 0;
        }

        const uint32_t ea = yz_rsx_io_to_ea(pc);
        if (!ea)
            return 0;
        const uint32_t cmd = vm_read32(ea);
        if ((cmd & 0xE0000003u) == 0x20000000u ||
            (cmd & 3u) == 1u) {
            const uint32_t next =
                (cmd & 3u) == 1u ? (cmd & 0xFFFFFFFCu)
                                 : (cmd & 0x1FFFFFFCu);
            if (next == pc || !yz_rsx_io_to_ea(next))
                return 0;
            pc = next;
            continue;
        }
        if ((cmd & 3u) == 2u) {
            const uint32_t next = cmd & 0x1FFFFFFCu;
            if (ret != ~0u || !yz_rsx_io_to_ea(next))
                return 0;
            ret = pc + 4u;
            pc = next;
            continue;
        }
        if ((cmd & 0xFFFF0003u) == 0x00020000u) {
            if (ret == ~0u)
                return 0;
            pc = ret;
            ret = ~0u;
            continue;
        }
        if (cmd & 0xA0030003u)
            return 0;
        const uint32_t count = (cmd >> 18) & 0x7FFu;
        const uint32_t bytes = 4u + count * 4u;
        pc = pc < ring ? ((pc + bytes) & mask) : (pc + bytes);
    }
    return 0;
}

enum yz_a010_chain_probe_stop {
    YZ_A010_PROBE_COMPLETE,
    YZ_A010_PROBE_BAD_WINDOW,
    YZ_A010_PROBE_OUTSIDE_WINDOW,
    YZ_A010_PROBE_UNMAPPED,
    YZ_A010_PROBE_SELF_BEFORE_TARGET,
    YZ_A010_PROBE_BAD_JUMP,
    YZ_A010_PROBE_NESTED_CALL,
    YZ_A010_PROBE_RETURN_WITHOUT_CALL,
    YZ_A010_PROBE_MALFORMED,
    YZ_A010_PROBE_STEP_LIMIT
};

struct yz_a010_chain_probe {
    enum yz_a010_chain_probe_stop stop;
    uint32_t stop_pc;
    uint32_t stop_cmd;
    uint32_t steps;
    uint32_t packets;
    uint32_t jumps;
    uint32_t calls;
    uint32_t returns;
    uint32_t begin;
    uint32_t end;
    uint32_t arrays;
    uint32_t indices;
    int reached_target;
};

static const char* yz_a010_chain_probe_stop_name(
    enum yz_a010_chain_probe_stop stop)
{
    switch (stop) {
    case YZ_A010_PROBE_COMPLETE: return "complete";
    case YZ_A010_PROBE_BAD_WINDOW: return "bad-window";
    case YZ_A010_PROBE_OUTSIDE_WINDOW: return "outside-window";
    case YZ_A010_PROBE_UNMAPPED: return "unmapped";
    case YZ_A010_PROBE_SELF_BEFORE_TARGET: return "self-before-target";
    case YZ_A010_PROBE_BAD_JUMP: return "bad-jump";
    case YZ_A010_PROBE_NESTED_CALL: return "nested-call";
    case YZ_A010_PROBE_RETURN_WITHOUT_CALL: return "return-without-call";
    case YZ_A010_PROBE_MALFORMED: return "malformed";
    case YZ_A010_PROBE_STEP_LIMIT: return "step-limit";
    default: return "unknown";
    }
}

/* Diagnostic-only dry run for the candidate chains rejected by the missing
 * release recovery.  Besides the exact stop reason, count the draw methods
 * reachable through the candidate's real JUMP/CALL flow.  This answers the
 * important question the recovery itself cannot: whether its safe resume
 * point is skipping a large, otherwise usable environment command stream, or
 * whether the producer never linked such a stream in the first place. */
static void yz_a010_probe_chain(uint32_t start,
                               uint32_t target,
                               uint32_t window_start,
                               uint32_t put,
                               struct yz_a010_chain_probe* out)
{
    const uint32_t ring = 0x800000u;
    const uint32_t mask = ring - 1u;
    const uint32_t committed = (put - window_start + ring) & mask;
    uint32_t pc = start;
    uint32_t ret = ~0u;
    memset(out, 0, sizeof(*out));
    out->stop = YZ_A010_PROBE_STEP_LIMIT;

    if (committed == 0u || committed >= (ring >> 1)) {
        out->stop = YZ_A010_PROBE_BAD_WINDOW;
        return;
    }

    for (uint32_t step = 0; step < 0x40000u; step++) {
        out->steps = step + 1u;
        out->stop_pc = pc;
        if (pc == target)
            out->reached_target = 1;

        if (pc < ring) {
            const uint32_t delta = (pc - window_start + ring) & mask;
            if (delta > committed) {
                out->stop = YZ_A010_PROBE_OUTSIDE_WINDOW;
                return;
            }
        } else if (ret == ~0u) {
            out->stop = YZ_A010_PROBE_OUTSIDE_WINDOW;
            return;
        }

        const uint32_t ea = yz_rsx_io_to_ea(pc);
        if (!ea) {
            out->stop = YZ_A010_PROBE_UNMAPPED;
            return;
        }
        const uint32_t cmd = vm_read32(ea);
        out->stop_cmd = cmd;

        if ((cmd & 0xE0000003u) == 0x20000000u ||
            (cmd & 3u) == 1u) {
            const uint32_t next =
                (cmd & 3u) == 1u ? (cmd & 0xFFFFFFFCu)
                                 : (cmd & 0x1FFFFFFCu);
            out->jumps++;
            if (next == pc) {
                out->stop = out->reached_target && ret == ~0u
                                ? YZ_A010_PROBE_COMPLETE
                                : YZ_A010_PROBE_SELF_BEFORE_TARGET;
                return;
            }
            if (!yz_rsx_io_to_ea(next)) {
                out->stop = YZ_A010_PROBE_BAD_JUMP;
                return;
            }
            pc = next;
            continue;
        }
        if ((cmd & 3u) == 2u) {
            const uint32_t next = cmd & 0x1FFFFFFCu;
            out->calls++;
            if (ret != ~0u) {
                out->stop = YZ_A010_PROBE_NESTED_CALL;
                return;
            }
            if (!yz_rsx_io_to_ea(next)) {
                out->stop = YZ_A010_PROBE_BAD_JUMP;
                return;
            }
            ret = (pc + 4u) & mask;
            pc = next;
            continue;
        }
        if ((cmd & 0xFFFF0003u) == 0x00020000u) {
            out->returns++;
            if (ret == ~0u) {
                out->stop = YZ_A010_PROBE_RETURN_WITHOUT_CALL;
                return;
            }
            pc = ret;
            ret = ~0u;
            continue;
        }
        if (cmd & 0xA0030003u) {
            out->stop = YZ_A010_PROBE_MALFORMED;
            return;
        }

        const uint32_t count = (cmd >> 18) & 0x7FFu;
        const uint32_t noninc = cmd & 0x40000000u;
        const uint32_t method = cmd & 0x3FFFCu;
        out->packets++;
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t arg_io =
                pc < ring ? ((pc + 4u + i * 4u) & mask)
                          : (pc + 4u + i * 4u);
            const uint32_t arg_ea = yz_rsx_io_to_ea(arg_io);
            if (!arg_ea) {
                out->stop_pc = arg_io;
                out->stop = YZ_A010_PROBE_UNMAPPED;
                return;
            }
            const uint32_t eff = noninc ? method : method + i * 4u;
            const uint32_t canonical = eff & 0x1FFCu;
            const uint32_t value = vm_read32(arg_ea);
            if (canonical == 0x1808u) {
                if (value) out->begin++;
                else       out->end++;
            } else if (canonical == 0x1814u) {
                out->arrays++;
            } else if (canonical == 0x1820u) {
                out->indices++;
            }
        }
        const uint32_t bytes = 4u + count * 4u;
        pc = pc < ring ? ((pc + bytes) & mask) : (pc + bytes);
    }
}

/* A generated batch may be fully formed and draw-balanced even though its
 * terminal self-stop has not yet been linked to the next generated batch.
 * Releasing one such prefix at a time preserves producer order: RSX consumes
 * the batch, parks at its terminal stop, and the next recovery begins strictly
 * after that stop.  This is deliberately narrower than accepting an arbitrary
 * prefix; malformed flow, live calls, and unbalanced draws remain fail-closed. */
static int yz_a010_probe_is_safe_prefix(
    const struct yz_a010_chain_probe* probe,
    uint32_t candidate,
    uint32_t window_start,
    uint32_t put)
{
    const uint32_t ring = 0x800000u;
    const uint32_t mask = ring - 1u;
    const uint32_t committed = (put - window_start + ring) & mask;
    const uint32_t candidate_delta =
        (candidate - window_start + ring) & mask;
    const uint32_t stop_delta =
        (probe->stop_pc - window_start + ring) & mask;
    const uint32_t self_target =
        (probe->stop_cmd & 3u) == 1u
            ? (probe->stop_cmd & 0xFFFFFFFCu)
            : (probe->stop_cmd & 0x1FFFFFFCu);

    return probe->stop == YZ_A010_PROBE_SELF_BEFORE_TARGET &&
           !probe->reached_target &&
           probe->stop_pc < ring &&
           self_target == probe->stop_pc &&
           candidate_delta < stop_delta &&
           stop_delta <= committed &&
           probe->calls == probe->returns &&
           probe->begin != 0u &&
           probe->begin == probe->end &&
           (probe->arrays != 0u || probe->indices != 0u);
}

static int yz_a010_balanced_generated_prefix_at(
    uint32_t candidate, uint32_t window_start, uint32_t window_end)
{
    if (!yz_a010_generated_prologue_at(candidate))
        return 0;
    struct yz_a010_chain_probe probe;
    yz_a010_probe_chain(
        candidate, window_end, window_start, window_end, &probe);
    return yz_a010_probe_is_safe_prefix(
        &probe, candidate, window_start, window_end);
}

/* Sequential generated-data gaps do not need the later FE0 completion packet
 * to be present. Their cursor already orders them after the preceding method,
 * so the earliest exact prologue is safe once its local chain is draw-balanced
 * and terminates at its own forward self-stopper. */
static uint32_t yz_a010_find_balanced_generated_prefix(
    uint32_t start, uint32_t end)
{
    const uint32_t ring = 0x800000u;
    const uint32_t mask = ring - 1u;
    const uint32_t ahead = (end - start + ring) & mask;
    if (ahead == 0u || ahead >= (ring >> 1))
        return 0u;
    for (uint32_t delta = 0u; delta + 0x30u <= ahead; delta += 4u) {
        const uint32_t candidate = (start + delta) & mask;
        if (yz_a010_balanced_generated_prefix_at(
                candidate, start, end))
            return candidate;
    }
    return 0u;
}

/* Find the next user-interrupt packet the guest is waiting to consume, then
 * find the earliest exact generated block that is safe to execute.  A complete
 * chain may reach the interrupt directly; otherwise a balanced prefix can run
 * up to its own self-stop and recovery resumes from there.  Never jump over a
 * validated prefix merely because a later tail already reaches completion. */
static uint32_t yz_a010_find_pending_chain(uint32_t start, uint32_t put,
                                           int emit_summary)
{
    static int trace_enabled = -1;
    static int trace_done = 0;
    static uint32_t trace_label_min = 0u;
    if (trace_enabled < 0) {
        trace_enabled = getenv("YZ_A010_MISSING_REL_TRACE") ? 1 : 0;
        if (trace_enabled && emit_summary) {
            const char* const label_env =
                getenv("YZ_A010_MISSING_REL_TRACE_LABEL");
            if (label_env && *label_env)
                trace_label_min =
                    (uint32_t)strtoul(label_env, nullptr, 0);
            fprintf(stderr,
                    "[a010-chain-trace] ARMED: inventory the first matching "
                    "generated-command region without executing it "
                    "(label >= 0x%08X)\n",
                    trace_label_min);
            fflush(stderr);
        }
    }
    const uint32_t ring = 0x800000u;
    const uint32_t mask = ring - 1u;
    const uint32_t ahead = (put - start + ring) & mask;
    if (ahead == 0u || ahead >= (ring >> 1))
        return 0u;

    const uint32_t have = vm_read32(RSX_REPORTS + 0xFE0u);
    const uint32_t want = have + 1u;
    const int trace = emit_summary &&
        trace_enabled && !trace_done && have >= trace_label_min;
    uint32_t ucmd = 0u;
    for (uint32_t delta = 0u; delta + 8u <= ahead; delta += 4u) {
        const uint32_t candidate = (start + delta) & mask;
        const uint32_t ea = yz_rsx_io_to_ea(candidate);
        if (!ea)
            continue;
        const uint32_t cmd = vm_read32(ea);
        const uint32_t method = cmd & 0x3FFFCu;
        const uint32_t count = (cmd >> 18) & 0x7FFu;
        const uint32_t arg_ea =
            yz_rsx_io_to_ea((candidate + 4u) & mask);
        if ((method == 0xEB00u || method == 0xEB04u) &&
            count != 0u && arg_ea && vm_read32(arg_ea) == want) {
            ucmd = candidate;
            break;
        }
    }
    if (!ucmd)
        return 0u;

    if (trace) {
        fprintf(stderr,
                "[a010-chain-trace] window start=0x%06X target=0x%06X "
                "PUT=0x%06X bytes=0x%X\n",
                start, ucmd, put, ahead);
        fflush(stderr);
    }

    uint32_t steps = 0u;
    uint32_t chain_end = 0u;
    if (yz_a010_chain_complete(start, ucmd, start, put,
                               &steps, &chain_end)) {
        if (trace) {
            struct yz_a010_chain_probe probe;
            yz_a010_probe_chain(start, ucmd, start, put, &probe);
            fprintf(stderr,
                    "[a010-chain-trace] direct entry=0x%06X stop=%s "
                    "pc=0x%06X cmd=0x%08X reached=%d steps=%u packets=%u "
                    "flow=%u/%u/%u draw=%u/%u/%u/%u\n",
                    start, yz_a010_chain_probe_stop_name(probe.stop),
                    probe.stop_pc, probe.stop_cmd, probe.reached_target,
                    probe.steps, probe.packets, probe.jumps, probe.calls,
                    probe.returns, probe.begin, probe.end, probe.arrays,
                    probe.indices);
            trace_done = 1;
            fflush(stderr);
        }
        if (emit_summary) {
            fprintf(stderr,
                    "[a010-pending-chain] label=0x%08X next=0x%08X "
                    "ucmd=0x%06X entry=0x%06X scan-start=0x%06X "
                    "chain-end=0x%06X PUT=0x%06X complete-steps=%u direct\n",
                    have, want, ucmd, start, start, chain_end, put, steps);
            fflush(stderr);
        }
        return start;
    }

    steps = 0u;
    if (yz_a010_chain_reaches_completion(start, ucmd, start, put, &steps)) {
        if (trace) {
            struct yz_a010_chain_probe probe;
            yz_a010_probe_chain(start, ucmd, start, put, &probe);
            fprintf(stderr,
                    "[a010-chain-trace] direct-staged entry=0x%06X stop=%s "
                    "pc=0x%06X cmd=0x%08X reached=%d steps=%u packets=%u "
                    "flow=%u/%u/%u draw=%u/%u/%u/%u\n",
                    start, yz_a010_chain_probe_stop_name(probe.stop),
                    probe.stop_pc, probe.stop_cmd, probe.reached_target,
                    probe.steps, probe.packets, probe.jumps, probe.calls,
                    probe.returns, probe.begin, probe.end, probe.arrays,
                    probe.indices);
            trace_done = 1;
            fflush(stderr);
        }
        if (emit_summary) {
            fprintf(stderr,
                    "[a010-pending-chain] label=0x%08X next=0x%08X "
                    "ucmd=0x%06X entry=0x%06X scan-start=0x%06X "
                    "PUT=0x%06X prefix-steps=%u staged-direct\n",
                    have, want, ucmd, start, start, put, steps);
            fflush(stderr);
        }
        return start;
    }

    unsigned rejected = 0u;
    unsigned trace_logged = 0u;
    for (uint32_t delta = 0u; delta + 0x30u <= ahead; delta += 4u) {
        const uint32_t candidate = (start + delta) & mask;
        if (candidate == ucmd)
            break;
        if (!yz_a010_generated_prologue_at(candidate))
            continue;
        struct yz_a010_chain_probe prefix_probe;
        yz_a010_probe_chain(candidate, ucmd, start, put, &prefix_probe);
        if (yz_a010_probe_is_safe_prefix(&prefix_probe, candidate,
                                         start, put)) {
            if (trace) {
                fprintf(stderr,
                        "[a010-chain-trace] selected-prefix entry=0x%06X "
                        "delta=0x%X stop=%s pc=0x%06X cmd=0x%08X "
                        "steps=%u packets=%u flow=%u/%u/%u "
                        "draw=%u/%u/%u/%u skipped-prologues=%u\n",
                        candidate, delta,
                        yz_a010_chain_probe_stop_name(prefix_probe.stop),
                        prefix_probe.stop_pc, prefix_probe.stop_cmd,
                        prefix_probe.steps, prefix_probe.packets,
                        prefix_probe.jumps, prefix_probe.calls,
                        prefix_probe.returns, prefix_probe.begin,
                        prefix_probe.end, prefix_probe.arrays,
                        prefix_probe.indices, rejected);
                trace_done = 1;
                fflush(stderr);
            }
            if (emit_summary) {
                fprintf(stderr,
                        "[a010-pending-chain] label=0x%08X next=0x%08X "
                        "ucmd=0x%06X entry=0x%06X scan-start=0x%06X "
                        "segment-stop=0x%06X PUT=0x%06X "
                        "draw=%u/%u/%u/%u ordered-prefix\n",
                        have, want, ucmd, candidate, start,
                        prefix_probe.stop_pc, put, prefix_probe.begin,
                        prefix_probe.end, prefix_probe.arrays,
                        prefix_probe.indices);
                fflush(stderr);
            }
            return candidate;
        }
        steps = 0u;
        chain_end = 0u;
        if (yz_a010_chain_complete(candidate, ucmd, start, put,
                                   &steps, &chain_end)) {
            if (trace) {
                struct yz_a010_chain_probe probe;
                yz_a010_probe_chain(candidate, ucmd, start, put, &probe);
                fprintf(stderr,
                        "[a010-chain-trace] selected entry=0x%06X "
                        "delta=0x%X stop=%s pc=0x%06X cmd=0x%08X "
                        "reached=%d steps=%u packets=%u flow=%u/%u/%u "
                        "draw=%u/%u/%u/%u skipped-prologues=%u\n",
                        candidate, delta,
                        yz_a010_chain_probe_stop_name(probe.stop),
                        probe.stop_pc, probe.stop_cmd, probe.reached_target,
                        probe.steps, probe.packets, probe.jumps, probe.calls,
                        probe.returns, probe.begin, probe.end, probe.arrays,
                        probe.indices, rejected);
                trace_done = 1;
                fflush(stderr);
            }
            if (emit_summary) {
                fprintf(stderr,
                        "[a010-pending-chain] label=0x%08X next=0x%08X "
                        "ucmd=0x%06X entry=0x%06X scan-start=0x%06X "
                        "chain-end=0x%06X PUT=0x%06X complete-steps=%u\n",
                        have, want, ucmd, candidate, start,
                        chain_end, put, steps);
                fflush(stderr);
            }
            return candidate;
        }
        steps = 0u;
        if (yz_a010_chain_reaches_completion(candidate, ucmd, start, put,
                                             &steps)) {
            if (trace) {
                struct yz_a010_chain_probe probe;
                yz_a010_probe_chain(candidate, ucmd, start, put, &probe);
                fprintf(stderr,
                        "[a010-chain-trace] selected-staged entry=0x%06X "
                        "delta=0x%X stop=%s pc=0x%06X cmd=0x%08X "
                        "reached=%d steps=%u packets=%u flow=%u/%u/%u "
                        "draw=%u/%u/%u/%u skipped-prologues=%u\n",
                        candidate, delta,
                        yz_a010_chain_probe_stop_name(probe.stop),
                        probe.stop_pc, probe.stop_cmd, probe.reached_target,
                        probe.steps, probe.packets, probe.jumps, probe.calls,
                        probe.returns, probe.begin, probe.end, probe.arrays,
                        probe.indices, rejected);
                trace_done = 1;
                fflush(stderr);
            }
            if (emit_summary) {
                fprintf(stderr,
                        "[a010-pending-chain] label=0x%08X next=0x%08X "
                        "ucmd=0x%06X entry=0x%06X scan-start=0x%06X "
                        "PUT=0x%06X prefix-steps=%u staged\n",
                        have, want, ucmd, candidate, start, put, steps);
                fflush(stderr);
            }
            return candidate;
        }
        if (trace && trace_logged < 128u) {
            struct yz_a010_chain_probe probe;
            yz_a010_probe_chain(candidate, ucmd, start, put, &probe);
            fprintf(stderr,
                    "[a010-chain-trace] rejected entry=0x%06X delta=0x%X "
                    "stop=%s pc=0x%06X cmd=0x%08X reached=%d steps=%u "
                    "packets=%u flow=%u/%u/%u draw=%u/%u/%u/%u\n",
                    candidate, delta,
                    yz_a010_chain_probe_stop_name(probe.stop),
                    probe.stop_pc, probe.stop_cmd, probe.reached_target,
                    probe.steps, probe.packets, probe.jumps, probe.calls,
                    probe.returns, probe.begin, probe.end, probe.arrays,
                    probe.indices);
            trace_logged++;
        }
        rejected++;
    }
    if (trace) {
        fprintf(stderr,
                "[a010-chain-trace] no-safe-entry skipped-prologues=%u "
                "logged=%u\n",
                rejected, trace_logged);
        trace_done = 1;
        fflush(stderr);
    }
    if (emit_summary) {
        fprintf(stderr,
                "[a010-pending-chain] REFUSED label=0x%08X next=0x%08X "
                "ucmd=0x%06X scan-start=0x%06X PUT=0x%06X "
                "incomplete-prologues=%u\n",
                have, want, ucmd, start, put, rejected);
        fflush(stderr);
    }
    return 0u;
}

static uint32_t yz_a010_missing_release_resume(uint32_t get, uint32_t put)
{
    const uint32_t ring = 0x800000u;
    const uint32_t mask = ring - 1u;
    const uint32_t ahead = (put - get + ring) & mask;
    const uint32_t sequential = (get + 4u) & mask;

    const uint32_t pending_chain =
        yz_a010_find_pending_chain(sequential, put, 1);
    if (pending_chain)
        return pending_chain;

    /* Fail closed.  Only a complete chain or a draw-balanced generated prefix
     * with a forward terminal self-stop can be released. */
    return 0u;
}

static int yz_a010_missing_release_try(uint32_t stopper_ea,
                                       uint32_t get,
                                       uint32_t put)
{
    static uint32_t last_dump_stopper = 0;
    static uint32_t last_dump_put = 0;
    static uint32_t last_dump_pending = 0;
    static unsigned long repairs = 0;

    const int enabled = yz_a010_fifo_publication_repair_enabled();
    {
        static int announced = 0;
        if (enabled && !announced) {
            announced = 1;
            /*
             * Reduced normal-path overhead made the already-published/new-
             * pending versus old-unreleased stopper race reproducible.  The
             * recovery is part of the clean lane's FIFO publication contract,
             * not an opt-in timing crutch.  Validation below is snapshot based
             * and fail-closed; it does not wait or sleep.
             */
            fprintf(stderr,
                    "[a010-missing-rel] ARMED%s: recover only an old "
                    "unjournaled self-stop after the producer has published "
                    "a newer stopper exactly at PUT; snapshot validated, "
                    "no dwell\n",
#if defined(YZ_PERF_CLEAN)
                    " (clean FIFO publication repair)"
#else
                    ""
#endif
                    );
            fflush(stderr);
        }
    }
    const int trace = yz_a010_reltrace_on();
    const int release_scene_active =
        InterlockedCompareExchange(
            &g_yz_a010_release_scene_active, 0, 0) != 0;
    /*
     * The repair's complete structural proof is protocol-wide.  Only the
     * observation-only trace is scene-gated; otherwise the identical
     * post-a020 failure captured at frame 3214 can never reach the proof.
     */
    if ((!enabled && !trace) ||
        (!enabled && !release_scene_active) ||
        !g_yz_game_toc)
        return 0;

    if (yz_frontier_trace_is_armed()) {
        static uint32_t last_stopper = 0;
        static uint32_t last_get = 0;
        static uint32_t last_put = 0;
        static uint32_t last_head = 0;
        static uint32_t last_pending = 0;
        const uint32_t trace_state =
            vm_read32(g_yz_game_toc - 0x7410u);
        const int trace_state_ok =
            trace_state >= 0x10000u && trace_state < 0xE0000000u;
        const uint32_t trace_head =
            trace_state_ok ? vm_read32(trace_state + 0x00u) : 0u;
        const uint32_t trace_pending =
            trace_state_ok ? vm_read32(trace_state + 0x20u) : 0u;
        if (stopper_ea != last_stopper || get != last_get ||
            put != last_put || trace_head != last_head ||
            trace_pending != last_pending) {
            last_stopper = stopper_ea;
            last_get = get;
            last_put = put;
            last_head = trace_head;
            last_pending = trace_pending;
            yz_frontier_trace_emit(
                YZ_FT_FIFO_PARK, yz_thread_current_id(), 0,
                get, put, stopper_ea, trace_pending,
                trace_head, vm_read32(stopper_ea));
        }
    }

    /* Refusal witness (boot 59, face-B cycle): the reltrace at the bottom
     * only sees candidates that PASS every gate; the terminal face-B
     * stopper is refused silently upstream. Under the trace flag, name the
     * FIRST refusing gate once per (stopper,gate) — capped, dedup'd, and
     * cheap enough for the consumer thread. */
    static int relgate_trace = -1;
    if (relgate_trace < 0)
        relgate_trace = getenv("YZ_A010_MISSING_REL_TRACE") ? 1 : 0;
    /* Boot 59 lesson (the cap sin, again): routine per-frame stoppers are
     * healthily refused (gate 4 unaligned-producer / gate 5 journal-owned)
     * hundreds of times per minute and burned the print budget long before
     * the terminal stopper. Scope the witness to stoppers that have been
     * the parked candidate for >2 s — face B parks for 180 s, routine ones
     * for milliseconds — and re-print each gate on each 2 s epoch so the
     * terminal stopper's refusal is ALWAYS in the tail of the log. */
    static uint32_t rg_cur_stopper, rg_last_gate;
    static uint64_t rg_first_seen_ms, rg_last_print_ms;
    const uint64_t rg_now = relgate_trace ? GetTickCount64() : 0;
    if (relgate_trace && rg_cur_stopper != stopper_ea) {
        rg_cur_stopper = stopper_ea;
        rg_first_seen_ms = rg_now;
        rg_last_gate = 0;
    }
#define YZ_RELGATE(gate_id, fmt, ...)                                        \
    do {                                                                     \
        if (relgate_trace && rg_now - rg_first_seen_ms > 2000u &&            \
            (rg_last_gate != (gate_id) ||                                    \
             rg_now - rg_last_print_ms > 2000u)) {                           \
            rg_last_gate = (gate_id); rg_last_print_ms = rg_now;             \
            fprintf(stderr, "[relgate] stopper=0x%08X parked=%llums gate=%u "\
                    fmt "\n", stopper_ea,                                    \
                    (unsigned long long)(rg_now - rg_first_seen_ms),         \
                    (gate_id), __VA_ARGS__);                                 \
            fflush(stderr);                                                  \
        }                                                                    \
    } while (0)

    const uint32_t ring = 0x800000u;
    const uint32_t ahead = (put - get + ring) % ring;
    /* RING-FULL-THROTTLE FACE (2026-08-06, boot 63 — ring d7 rsx-state +
     * [defer]): GET parked on a segment-entry JTS while the producer wrote
     * an ENTIRE LAP behind it (put 0x26F4CC -> 0x4BFF90, throttle-parked
     * 0x80 short of GET, t1lr=0xE9BACC in gcm reserve) and the game's own
     * ledger says the stopper is fully retired (S[0x20]=0, S[0x1C]=0) —
     * the physical release write alone went missing. ahead measures
     * ~ring-0x80 here, so the ring/2 sanity bound (built for the
     * "t1 lapped while parked = invalid snapshot" reading, boots 58/61)
     * refuses the repair's own strongest face. Admit it as a DISTINCT
     * branch with stricter proofs (game-ledger-retired at gate 4,
     * journal-quiet gate 7, wrap-aware-ABSENT gate 5) and a SEQUENTIAL-
     * ONLY resume — never the linked-guess resume whose mis-rejoin was
     * boot 60/62's derail. Kill-switch YZ_NO_RINGFULL_REL=1. */
    /* BOOT 64 GENERALIZATION (same day): the producer need not fill the
     * ring — boot 64 parked in frame-drain mid-lap (t1lr=0xE7DCA4, PUT
     * 6.8MB ahead, 1.2MB still free) with the LEDGER POSTURE of the
     * repair's original face: S[0x20] == PUT exactly, S[0x1C]=0, journal
     * quiet, wrap-aware ABSENT — and again ONLY the ring/2 bound refused,
     * 180 s to the watchdog. Two stable, valid, fully-witnessed deep-lag
     * parks (63/64) refute the "ahead >= ring/2 == invalid snapshot"
     * reading (boots 58/61 inherited it); the stable-snapshot recheck and
     * gate 7 carry the actual guarantees. The branch admits the whole
     * deep-lag range with the stricter proofs at gate 4. */
    static int no_deeplag = -1;
    if (no_deeplag < 0) no_deeplag = getenv("YZ_NO_DEEPLAG_REL") ? 1 : 0;
    const int deep_lag = !no_deeplag && ahead >= (ring >> 1);
    if (ahead == 0u || (ahead >= (ring >> 1) && !deep_lag)) {
        YZ_RELGATE(1u, "ahead=0x%X get=0x%06X put=0x%06X", ahead, get, put);
        return 0;
    }

    MemoryBarrier();
    const uint32_t state = vm_read32(g_yz_game_toc - 0x7410u);
    if (state < 0x10000u || state >= 0xE0000000u) {
        YZ_RELGATE(2u, "state=0x%08X", state);
        return 0;
    }
    const uint32_t base = vm_read32(state + 0x08u);
    const uint32_t end = vm_read32(state + 0x0Cu);
    const uint32_t head = vm_read32(state + 0x00u);
    const uint32_t pending = vm_read32(state + 0x20u);
    if (base < 0x10000u || end <= base || end - base > 0x1000000u ||
        head < base || head >= end) {
        YZ_RELGATE(3u, "base=0x%08X end=0x%08X head=0x%08X", base, end, head);
        return 0;
    }

    const uint32_t put_ea = yz_rsx_io_to_ea(put);
    if (deep_lag) {
        /* Deep-lag branch: the game's OWN ledger must be coherent about
         * this stopper being someone else's problem — no deferred ops
         * (S[0x1C]=0) and the pending cursor either fully cleared
         * (boot 63: ring-full throttle, S[0x20]=0) or naming the CURRENT
         * commit at PUT exactly (boot 64: drain-park, S[0x20]==put_ea —
         * the original repair proof). A cursor still naming THIS stopper
         * (or anything else) means the game owes protocol; wait. */
        const uint32_t latch = vm_read32(state + 0x1Cu);
        const int ledger_ok =
            latch == 0u && pending != stopper_ea &&
            (pending == 0u || (put_ea && pending == put_ea));
        if (!ledger_ok) {
            YZ_RELGATE(4u, "deeplag pending=0x%08X latch=0x%08X put_ea=0x%08X",
                       pending, latch, put_ea);
            return 0;
        }
    } else if (!put_ea || pending != put_ea || pending == stopper_ea) {
        YZ_RELGATE(4u, "put_ea=0x%08X pending=0x%08X", put_ea, pending);
        return 0;
    }

    /* A deferred release owns this stopper if it appears at any point before
     * the stable producer head.  In that case the ordered journal consumer,
     * not this direct-release recovery, must apply it. */
    if (yz_gcm_stopper_release_entry(stopper_ea) != 0u) {
        YZ_RELGATE(5u, "journal-owned entry=0x%08X region=%s",
                   yz_gcm_stopper_release_entry(stopper_ea),
                   g_yz_relentry_region == 2u ? "above-head" : "ordered");
        return 0;
    }

    const uint32_t expected = 0x20000000u | (get & 0x1FFFFFFCu);
    if (vm_read32(stopper_ea) != expected) {
        YZ_RELGATE(6u, "*stopper=0x%08X expected=0x%08X",
                   vm_read32(stopper_ea), expected);
        return 0;
    }

    /*
     * A release-trace-only run must be able to report the current generation
     * without enabling the recovery bridge.  Dump once per stable
     * (stopper, PUT, pending-stopper) tuple, then leave GET untouched.
     */
    if (trace && yz_a010_reltrace_eager_dump() &&
        (last_dump_stopper != stopper_ea ||
         last_dump_put != put ||
         last_dump_pending != pending)) {
        last_dump_stopper = stopper_ea;
        last_dump_put = put;
        last_dump_pending = pending;
        yz_a010_reltrace_dump(stopper_ea);
    }
    if (!enabled)
        return 0;

    /* GATE 7 — journal-flowing refusal (2026-08-06, boot-62 ring decode).
     * A missing release while the producer's journal head is still
     * ADVANCING is a DEFERRED-LATE release, not a lost one: boot 62's ring
     * (rel_hunt.py over frontier_ring_d3) caught t1 writing the 0x7F
     * entries for repaired stoppers 0x40858000/0x40880000 at seq
     * 24966429/24968522 — tens of seconds AFTER this repair had already
     * jumped them (repair-time head 0x41F00E20), and the n=3 jump's walk
     * ended on the terminal non-command park at io 0x22C0 (the boot-60
     * mis-rejoin class). The game's deferral drain is flip-coupled and
     * slows to tens of seconds at load boundaries; jumping then trades a
     * self-resolving park for a derailed walk. Repair only once the head
     * has been quiet for 5 s — the genuine idle-producer face the repair
     * was built for (a010 orphanage; post-a020 frame 3214). Kill-switch
     * YZ_A010_EAGER=1 restores the old always-eager behavior for A/B. */
    {
        static uint32_t rel_head_last;
        static uint64_t rel_head_change_ms;
        static int rel_eager = -1;
        if (rel_eager < 0)
            rel_eager = getenv("YZ_A010_EAGER") ? 1 : 0;
        const uint64_t rel_now = GetTickCount64();
        if (rel_head_last != head) {
            rel_head_last = head;
            rel_head_change_ms = rel_now;
        }
        if (!rel_eager && rel_now - rel_head_change_ms < 5000u) {
            YZ_RELGATE(7u, "journal-flowing head=0x%08X age=%llums",
                       head,
                       (unsigned long long)(rel_now - rel_head_change_ms));
            return 0;
        }
    }

    /* Deep-lag resume — BOOT-68 CORRECTION (first firing, n=1): blind
     * sequential (get+4) stepped GET into an EDGE data island (words at
     * get+4 = BF000000.. vertex floats; 178 s non-command park at io
     * 0x304524) — the documented class "EDGE reserves data islands in
     * the FIFO and links around them". Island stoppers release with a
     * jump FORWARD, and the island length lived only in the lost journal
     * entry. Order of preference now:
     *  1. the island-aware, fail-closed pending-chain hunt, scan-bounded
     *     one segment ahead (the real PUT is nearly a ring behind in
     *     deep-lag geometry; the prologue hunt's own ahead<ring/2 guard
     *     would refuse the raw PUT);
     *  2. refuse (stay parked, witnesses live) when no chain is found.
     * A single command-looking word at get+4 is not enough to prove that the
     * rest of the segment is commands. Boot 73 passed that old check, then
     * walked into a data island at io 0x69C2D8 and parked permanently.
     * The boot-60/62 linked-resume hazard (jumping a stopper whose
     * release was still coming) stays excluded by gate 7 + the ledger
     * proofs above, so using the hunt here does not re-open it. */
    uint32_t resume;
    uint32_t island_end = 0u;
    uint32_t island_resume = 0u;
    uint32_t island_word = 0u;
    if (deep_lag) {
        resume = yz_a010_find_pending_chain((get + 4u) & (ring - 1u),
                                            (get + 0x8000u) & (ring - 1u),
                                            1);
        if (!resume) {
            const uint32_t next_io = (get + 4u) & (ring - 1u);
            const uint32_t nea = yz_rsx_io_to_ea(next_io);
            island_end = yz_a010_data_island_end(stopper_ea);
            island_resume = yz_fifo_registered_island_resume(
                stopper_ea, island_end, get, put,
                0x40400000u, ring);
            island_word = island_resume ? vm_read32(island_end) : 0u;
            if (island_resume) {
                resume = island_resume;
                if (relgate_trace) {
                    fprintf(stderr,
                            "[a010-data-island-rel] selected "
                            "stopper=0x%08X island-end=0x%08X "
                            "resume=0x%06X word=0x%08X span=0x%X\n",
                            stopper_ea, island_end, island_resume,
                            island_word, island_end - stopper_ea);
                    fflush(stderr);
                }
            } else if (relgate_trace) {
                YZ_RELGATE(8u, "deeplag no-linked-chain next=0x%08X "
                           "island-end=0x%08X resume=0x%06X word=0x%08X",
                           nea ? vm_read32(nea) : 0u, island_end,
                           island_resume, island_word);
            }
        }
    } else {
        resume = yz_a010_missing_release_resume(get, put);
    }
    if (!resume)
        return 0;

    /*
     * PUT is the producer's publication boundary.  Recheck the entire
     * producer snapshot after validating the candidate chain so concurrent
     * publication cannot turn a sound repair into a stale one.  This replaces
     * the old 32 ms timing debounce with an actual synchronization invariant.
     */
    MemoryBarrier();
    if (vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT) != put ||
        vm_read32(g_yz_game_toc - 0x7410u) != state ||
        vm_read32(state + 0x00u) != head ||
        vm_read32(state + 0x20u) != pending ||
        vm_read32(stopper_ea) != expected ||
        (island_resume &&
         (yz_a010_data_island_end(stopper_ea) != island_end ||
          yz_fifo_registered_island_resume(
              stopper_ea, island_end, get, put,
              0x40400000u, ring) != island_resume)))
        return 0;

    vm_write32(stopper_ea,
               0x20000000u | (resume & 0x1FFFFFFCu));
    vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, resume);
    repairs++;
    fprintf(stderr,
            "[a010-missing-rel] repaired n=%lu old=0x%08X "
            "GET=0x%06X -> resume=0x%06X (%s) PUT=0x%06X ahead=0x%X "
            "new-pending=0x%08X head=0x%08X snapshot=stable\n",
            repairs, stopper_ea, get, resume,
            island_resume ? "registered-island" :
            (resume == ((get + 4u) & (ring - 1u))
                ? "sequential" : "linked"),
            put, ahead, pending, head);
    if (deep_lag)
        fprintf(stderr,
                "[a010-deeplag-rel] lost physical release re-issued "
                "through %s (posture=%s S1C=0, journal quiet); "
                "words@0x%08X now %08X %08X\n",
                island_resume ? "registered data-island edge" :
                                "linked chain",
                pending ? "moved-past(S20==PUT)" : "ledger-retired(S20=0)",
                stopper_ea, vm_read32(stopper_ea),
                vm_read32(stopper_ea + 4u));
    fflush(stderr);
    return 1;
}

extern "C" void yz_frontier_fifo_snapshot(uint32_t get, uint32_t put)
{
    const uint32_t stopper_ea = yz_rsx_io_to_ea(get);
    const uint32_t state =
        g_yz_game_toc ? vm_read32(g_yz_game_toc - 0x7410u) : 0u;
    const int state_ok = state >= 0x10000u && state < 0xE0000000u;
    const uint32_t base = state_ok ? vm_read32(state + 0x08u) : 0u;
    const uint32_t end = state_ok ? vm_read32(state + 0x0Cu) : 0u;
    const uint32_t head = state_ok ? vm_read32(state + 0x00u) : 0u;
    const uint32_t latch = state_ok ? vm_read32(state + 0x1Cu) : 0u;
    const uint32_t pending = state_ok ? vm_read32(state + 0x20u) : 0u;
    const uint32_t saved = state_ok ? vm_read32(state + 0x24u) : 0u;
    const uint32_t entry = stopper_ea
        ? yz_gcm_stopper_release_entry(stopper_ea) : 0u;

    yz_frontier_trace_emit(
        YZ_FT_FIFO_STATE, yz_thread_current_id(), 0,
        get, put, stopper_ea,
        stopper_ea ? vm_read32(stopper_ea) : 0u,
        entry, (put - get + 0x800000u) & 0x7FFFFFu);
    yz_frontier_trace_emit(
        YZ_FT_FIFO_PUBLICATION, yz_thread_current_id(), 0,
        state, head, base, end, latch, pending);
    yz_frontier_trace_emit(
        YZ_FT_FIFO_PUBLICATION, yz_thread_current_id(), 1,
        saved, g_yz_jrnl_cur_ea,
        state_ok ? vm_read32(state + 0x10u) : 0u,
        state_ok ? vm_read32(state + 0x14u) : 0u,
        state_ok ? vm_read32(state + 0x18u) : 0u,
        state_ok ? vm_read32(state + 0x28u) : 0u);
    yz_frontier_trace_emit(
        YZ_FT_RELEASE_JOURNAL, yz_thread_current_id(), 0u,
        state, head, pending, g_yz_jrnl_cur_ea, entry,
        stopper_ea ? vm_read32(stopper_ea) : 0u);
}

/* ============================================================================
 * ORDERED EDGE JOURNAL HLE (YZ_JRNL_HLE, opt-in; merged from the 2026-07-14
 * Mac export, docs/EDGE_JOURNAL_HLE.md + scratch/WINDOWS_HANDOFF_2026-07-14.md).
 *
 * A wedge takeover, not an eager second consumer: considered only while RSX
 * GET is parked on a committed jump-to-self, and only after the producer's
 * journal head has been stable for a debounce window. The pure helper
 * (yakuza/edge_journal_hle.cpp) validates the COMPLETE span from the takeover
 * cursor through the release matching the parked stopper before the first
 * guest write, applies patches before releases in journal order, retires each
 * tag only after its operation, and fails CLOSED on any undecoded tag (the
 * stopper stays locked; the full entry is logged for decoding).
 *
 * Two Windows-side adaptations of the Mac design, both measured-evidence:
 *  - release VALUE: the SPU consumer clears a stopper with a gcm jump-FORWARD
 *    word (top byte 0x20; rpcs3clone oracle census + the lever below), never
 *    zero -- so the release goes through yz_jrnl_hle_release, not write32(0).
 *  - takeover CURSOR: bootstrapped from the real consumer's own poll cursor
 *    (g_yz_jrnl_cur_ea, maintained on every GETLLAR poll), sampled at its
 *    episode MINIMUM because the stuck cursor oscillates across a 7-8 line
 *    window. Walking from arena base instead would re-apply entries the
 *    consumer already consumed without zeroing.
 *
 * Decoded tags: 0x10 memcpy{src@+4,size@+8,dst@+0xC}, 0x7F ordered release
 * {stopper EA@+4}. Tags 0x04/08/09/0A/0D/11 intentionally unsupported here
 * until decoded against the consumer's own apply code + the RPCS3 oracle. */
extern "C" volatile uint32_t g_yz_jrnl_cur_ea;   /* spu_channels.c: live consumer cursor */

enum class yz_jrnl_hle_park_result { waiting, applied };

static int yz_jrnl_hle_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        enabled = getenv("YZ_JRNL_HLE") ? 1 : 0;
        if (enabled) {
            fprintf(stderr,
                    "[jrnl-hle] ARMED: frozen-head ordered takeover; known tags=10,11,0A,7F; "
                    "legacy APPLY_REL/PARK_REL disabled\n");
            fflush(stderr);
        }
    }
    return enabled;
}

static uint32_t yz_jrnl_hle_read32(void*, uint32_t address)
{
    return vm_read32(address);
}

static void yz_jrnl_hle_write32(void*, uint32_t address, uint32_t value)
{
    vm_write32(address, value);
}

static bool yz_jrnl_hle_copy(void*, uint32_t destination, uint32_t source,
                             uint32_t size)
{
    if (!vm_base || source + size < source || destination + size < destination)
        return false;
    memmove(vm_base + destination, vm_base + source, size);
    return true;
}

static void yz_jrnl_hle_release(void*, uint32_t stopper_ea)
{
    /* Faithful release value (DONT_RECHASE #83 + the validated lever below):
     * a gcm jump-forward word targeting the io offset just past the stopper.
     * The io map is linear here (io = ea - 0x40400000, the same mapping the
     * segment classifier uses). A stopper outside the io window should not
     * exist; fall back to a NOP clear and say so once. */
    if (stopper_ea >= 0x40400000u && stopper_ea < 0x40C00000u) {
        const uint32_t io_off = stopper_ea - 0x40400000u;
        vm_write32(stopper_ea, 0x20000000u | ((io_off + 4u) & 0x1FFFFFFCu));
        return;
    }
    static int said = 0;
    if (!said) {
        said = 1;
        fprintf(stderr, "[jrnl-hle] release outside io window ea=0x%08X -> NOP clear\n",
                stopper_ea);
        fflush(stderr);
    }
    vm_write32(stopper_ea, 0u);
}

static yz_jrnl_hle_park_result yz_jrnl_hle_try(uint32_t stopper_ea)
{
    static uint32_t arena_base = 0;
    static uint32_t arena_end = 0;
    static uint32_t cursor = 0;          /* persisted past prior HLE applies */
    static uint32_t episode_min = 0;     /* min consumer-cursor EA this episode */
    static uint32_t last_head = 0;
    static uint32_t last_stopper = 0;
    static ULONGLONG stable_since = 0;
    static unsigned stable_ms = 16;
    static int configured = 0;
    static uint32_t last_problem_entry = 0;
    static uint32_t last_problem_tag = 0;

    if (!configured) {
        configured = 1;
        if (const char* value = getenv("YZ_JRNL_HLE_STABLE_MS")) {
            const unsigned parsed = (unsigned)atoi(value);
            stable_ms = parsed > 5000u ? 5000u : parsed;
        }
        fprintf(stderr, "[jrnl-hle] producer-head stability window=%ums\n", stable_ms);
        fflush(stderr);
    }

    if (!g_yz_game_toc) return yz_jrnl_hle_park_result::waiting;
    const uint32_t state = vm_read32(g_yz_game_toc - 0x7410u);
    if (state < 0x10000u || state >= 0xE0000000u)
        return yz_jrnl_hle_park_result::waiting;

    const uint32_t base = vm_read32(state + 0x08u);
    const uint32_t end = vm_read32(state + 0x0cu);
    const uint32_t head = vm_read32(state + 0x00u);
    if (base < 0x10000u || end <= base || end - base > 0x1000000u ||
        head < base || head >= end || ((end - base) & 0x1fu) != 0 ||
        ((head - base) & 0x1fu) != 0) {
        static uint32_t said_state = 0;
        if (said_state != state) {
            said_state = state;
            fprintf(stderr,
                    "[jrnl-hle] invalid arena state S=0x%08X base=0x%08X end=0x%08X head=0x%08X\n",
                    state, base, end, head);
            fflush(stderr);
        }
        return yz_jrnl_hle_park_result::waiting;
    }

    if (base != arena_base || end != arena_end) {
        arena_base = base;
        arena_end = end;
        cursor = 0;
        episode_min = 0;
        last_head = 0;
        stable_since = 0;
    }

    /* Sample the live consumer cursor toward its episode minimum (the stuck
     * window's start line). Maintained on every consumer GETLLAR poll, so
     * repeated stepper calls during the stability window see the whole
     * oscillation range. */
    {
        const uint32_t live = g_yz_jrnl_cur_ea;
        if (live >= base && live < end && ((live - base) & 0x1fu) == 0 &&
            (episode_min == 0 || live < episode_min))
            episode_min = live;
    }

    const ULONGLONG now = GetTickCount64();
    if (head != last_head || stopper_ea != last_stopper) {
        last_head = head;
        last_stopper = stopper_ea;
        stable_since = now;
        episode_min = 0;   /* new episode: resample the consumer window */
        return yz_jrnl_hle_park_result::waiting;
    }
    if (now - stable_since < stable_ms)
        return yz_jrnl_hle_park_result::waiting;

    /* The consumer's own stuck position is ground truth for "everything
     * before this is consumed": prefer it over the persisted cursor and the
     * arena base. Entries the HLE already applied rescan as harmless tag-0. */
    const uint32_t start = episode_min ? episode_min : (cursor ? cursor : base);
    {
        static uint32_t said_start = 0;
        if (said_start != start) {
            said_start = start;
            fprintf(stderr,
                    "[jrnl-hle] takeover cursor=0x%08X (%s) head=0x%08X stopper=0x%08X\n",
                    start,
                    episode_min ? "consumer-window-min" : (cursor ? "persisted" : "arena-base"),
                    head, stopper_ea);
            fflush(stderr);
        }
    }

    const yz::edge_journal::Io io{nullptr, yz_jrnl_hle_read32, yz_jrnl_hle_write32,
                                  yz_jrnl_hle_copy, yz_jrnl_hle_release};
    spu_lockline_lock();
    const yz::edge_journal::Result result = yz::edge_journal::apply_through_release(
        io, base, end, start, head, stopper_ea);
    spu_lockline_unlock();

    if (result.status == yz::edge_journal::Status::applied) {
        cursor = result.next_cursor;
        last_problem_entry = 0;
        last_problem_tag = 0;
        fprintf(stderr,
                "[jrnl-hle] applied %u ordered entries through release=0x%08X; cursor=0x%08X head=0x%08X\n",
                result.applied_entries, stopper_ea, cursor, head);
        fflush(stderr);
        return yz_jrnl_hle_park_result::applied;
    }

    if (result.status == yz::edge_journal::Status::changed_during_validation) {
        /* Producer or consumer activity was observed despite a stable head.
         * Restart the stability proof instead of racing it. */
        stable_since = now;
        return yz_jrnl_hle_park_result::waiting;
    }

    if (result.problem_entry != last_problem_entry ||
        result.problem_tag != last_problem_tag) {
        last_problem_entry = result.problem_entry;
        last_problem_tag = result.problem_tag;
        fprintf(stderr,
                "[jrnl-hle] BLOCKED status=%s entry=0x%08X tag=0x%08X words="
                "%08X %08X %08X %08X %08X %08X %08X %08X\n",
                yz::edge_journal::status_name(result.status),
                result.problem_entry, result.problem_tag,
                result.problem_words[0], result.problem_words[1],
                result.problem_words[2], result.problem_words[3],
                result.problem_words[4], result.problem_words[5],
                result.problem_words[6], result.problem_words[7]);
        fflush(stderr);
    }
    return yz_jrnl_hle_park_result::waiting;
}

/* SINGLE-SEGMENT regime (env YZ_BIG_SEG, 2026-06-16): pin ctx->end (gcm_ctx+4) to the
 * io-buffer end so the game's same-segment release check (S[0x24]==ctx->end) ALWAYS
 * passes -> every stopper-release is IMMEDIATE (no cross-segment defer, no S[0x1C] latch,
 * no drain needed) -> GET flows. Reproduces the 2026-06-14f single-segment behaviour that
 * reached fence-advance + no deadlock, but via the LIVE context (no _cellGcmInitBody
 * replacement). Tight monitor so it beats the game's stopper-placement rate. (A real ring
 * WRAP needs handling once current nears the buffer end -- many seconds of frames away;
 * fine for the test window. If this flows, add wrap handling + make it faithful.) */
static DWORD WINAPI yz_bigseg_monitor(LPVOID)
{
    /* FORCE-IMMEDIATE gcm stopper-release (2026-06-19): the inline gcm flush
     * func_00E9BC9C @0xE9BCF8 reads S[0x1C] (S = *(game_toc-0x7410)); S[0x1C]!=0 ->
     * DEFER the release into the op-list (tag 0x7F) that never drains while t1 is
     * flip-wedged (blocker #21); S[0x1C]==0 -> release IMMEDIATELY (NOP the stopper
     * word so the RSX flows past it). Clamp S[0x1C] to 0 so every release is
     * immediate -> the op-list never accumulates -> GET is never stranded behind a
     * deferred stopper. (Reliable: same S pointer our deferred-release reader uses.) */
    /* IMMEDIATE STOPPER-RELEASE (env YZ_IMM_REL, 2026-06-19): the inline gcm flush
     * func_00E9BC9C @0xE9BCF8 reads S[0x1C] (S = *(game_toc-0x7410)); S[0x1C]!=0 ->
     * DEFER the stopper-release into the op-list (tag 0x7F) that only drains AFTER
     * the libgcm reserve (func_02103AAC) returns -- but the reserve never returns
     * because it is waiting on GET, which is waiting on this very release. Clamp
     * S[0x1C]=0 so every release is IMMEDIATE (the same-fragment NOP-patch path),
     * matching RPCS3 where releases are consumed promptly. Pair with YZ_NO_DEFER
     * (faithful consumer that spins on the stopper like RPCS3's run_FIFO). */
    fprintf(stderr, "[immrel] monitor up: clamping gcm defer latch S[0x1C]=0 (force immediate release)\n");
    int cleared = 0;
    for (;;) {
        if (g_yz_game_toc) {
            uint32_t S = vm_read32(g_yz_game_toc - 0x7410u);
            if (S >= 0x10000u && S < 0xE0000000u) {
                if (vm_read32(S + 0x1Cu) != 0u) {
                    vm_write32(S + 0x1Cu, 0u);
                    if (cleared < 12) { cleared++;
                        fprintf(stderr, "[immrel] cleared S[0x1C] latch (S=0x%08X)\n", S); }
                }
            }
        }
        Sleep(0);   /* yield, tight -- beat func_00E9BC9C's S[0x1C] read */
    }
}

/* DEFER-DECISION TRACE (env YZ_TRACE_DEFER, 2026-06-24). Pin WHY the game defers a
 * stopper-release (the LAYER-1 deadlock root) vs releases it immediately. From the
 * static decode of the inline gcm flush func_00E9BC9C / func_00E9BE4C:
 *   S = *(game_toc-0x7410)   = gcm-state struct
 *   C = *(game_toc-0x7414);  P = *C;  ctx_end = P[+0x4]   (current segment end)
 *   S[+0x1C] = defer latch    S[+0x20] = pending stopper cursor
 *   S[+0x24] = segment-end recorded WHEN the stopper was placed
 *   S[+0x00] = op-list write head   S[+0x08] = op-list base   (0x20-byte entries;
 *              entry[+0]=tag (0x7F=deferred release), entry[+4]=stopper EA)
 * DECISION (func_00E9BE4C:E9BE58): release is IMMEDIATE iff S[0x24]==ctx_end, else
 * DEFERRED (cross-segment: the producer crossed a 1 MB boundary between place+release).
 * This monitor is READ-ONLY (no clamp, unlike YZ_IMM_REL) -- it just records the
 * decision state so we can see (a) how often defer fires + the S[0x24]/ctx_end values,
 * (b) whether the op-list backlog of tag-0x7F entries EVER drains (count drops) or only
 * grows, and (c) ctx_end segment advances. Logs every change + a 1s heartbeat. */
static DWORD WINAPI yz_defer_trace_mon(LPVOID)
{
    fprintf(stderr, "[defer] trace monitor up (READ-ONLY): watching S[0x1C] latch, "
                    "op-list backlog, S[0x24] vs ctx_end\n");
    uint32_t last_head = ~0u, last_latch = ~0u, last_end = ~0u, last_pend = ~0u;
    DWORD last_hb = 0;
    for (;;) {
        if (g_yz_game_toc) {
            uint32_t S = vm_read32(g_yz_game_toc - 0x7410u);
            uint32_t C = vm_read32(g_yz_game_toc - 0x7414u);
            if (S >= 0x10000u && S < 0xE0000000u) {
                uint32_t latch = vm_read32(S + 0x1Cu);
                uint32_t pend  = vm_read32(S + 0x20u);
                uint32_t saved = vm_read32(S + 0x24u);
                uint32_t base  = vm_read32(S + 0x08u);
                uint32_t head  = vm_read32(S + 0x00u);
                uint32_t ctx_end = 0;
                if (C >= 0x10000u && C < 0xE0000000u) {
                    uint32_t P = vm_read32(C + 0x0u);
                    if (P >= 0x10000u && P < 0xE0000000u) ctx_end = vm_read32(P + 0x4u);
                }
                /* count un-applied tag-0x7F deferred releases in [base,head) */
                uint32_t pend7f = 0, nent = 0;
                if (base >= 0x10000u && base < 0xE0000000u && head >= base && (head - base) <= 0x8000u) {
                    nent = (head - base) / 0x20u;
                    for (uint32_t e = base; e < head; e += 0x20u)
                        if (vm_read32(e + 0x0u) == 0x7Fu) pend7f++;
                }
                uint32_t get = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_GET);
                uint32_t put = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT);
                DWORD now = GetTickCount();

                if (head != last_head) {
                    int delta = (last_head==~0u) ? 0 : (int)((int32_t)head - (int32_t)last_head)/0x20;
                    const char* dir = (last_head!=~0u && head < last_head) ? "  <<< DRAIN (head shrank)" : "";
                    fprintf(stderr, "[defer] op-list head 0x%08X->0x%08X (%+d entries, %u total, %u pending-0x7F)%s\n",
                            last_head==~0u?head:last_head, head, delta, nent, pend7f, dir);
                    last_head = head;
                }
                if (pend7f != last_pend) {
                    if (last_pend != ~0u && pend7f < last_pend)
                        fprintf(stderr, "[defer] *** BACKLOG DRAINED: pending-0x7F %u -> %u (something applied a release)\n", last_pend, pend7f);
                    last_pend = pend7f;
                }
                if (latch != last_latch) {
                    fprintf(stderr, "[defer] latch S[0x1C] 0x%08X->0x%08X  (%s)  pend=0x%08X saved_segend=0x%08X ctx_end=0x%08X\n",
                            last_latch==~0u?latch:last_latch, latch,
                            latch?"DEFER mode armed":"immediate mode", pend, saved, ctx_end);
                    last_latch = latch;
                }
                if (ctx_end != last_end && ctx_end) {
                    fprintf(stderr, "[defer] ctx_end (segment) 0x%08X->0x%08X  saved_segend=0x%08X  -> next release: %s\n",
                            last_end==~0u?ctx_end:last_end, ctx_end, saved,
                            (saved==ctx_end)?"IMMEDIATE":"DEFER (cross-segment)");
                    last_end = ctx_end;
                }
                if (now - last_hb >= 1000u) { last_hb = now;
                    fprintf(stderr, "[defer hb] latch=0x%X pend=0x%08X saved_segend=0x%08X ctx_end=0x%08X | op-list ent=%u pend7f=%u | GET=0x%06X PUT=0x%06X\n",
                            latch, pend, saved, ctx_end, nent, pend7f, get & 0xFFFFFFu, put & 0xFFFFFFu);
                    fflush(stderr);
                }
            }
        }
        Sleep(0);
    }
}

/* PHASE TIMELINE (env YZ_PHASE, 2026-06-24): high-detail producer+consumer timeline to
 * catch the working->broken transition. Frames 1-2 work, frame 3 (first ring wrap)
 * deadlocks; this logs EVERY change to GET/PUT (consumer/commit heads) + the producer's
 * bufdesc cursor (cur/begin/end) + flip count, timestamped, so we can see the exact step
 * where "producer ahead of consumer" flips to "phase-locked one segment apart". Pairs with
 * the reserve-call log (dispatch.cpp, each func_02103AAC) and YZ_FIFO_TRACE (consumer steps). */
static DWORD WINAPI yz_phase_monitor(LPVOID)
{
    fprintf(stderr, "[phase] timeline monitor up (logs every GET/PUT/cursor/flip change)\n");
    const uint32_t pbd = 0x02114000u - 0x7FD8u;
    uint32_t lg=~0u, lp=~0u, lc=~0u, lb=~0u, le=~0u, lf=~0u;
    DWORD t0 = GetTickCount();
    for (;;) {
        uint32_t bd  = vm_read32(pbd);
        uint32_t get = vm_read32(0x10000044u) & ~3u;
        uint32_t put = vm_read32(0x10000040u) & ~3u;
        uint32_t cur=0, beg=0, end=0;
        if (bd >= 0x10000u && bd < 0xE0000000u) {
            cur = vm_read32(bd + 0x8); beg = vm_read32(bd + 0x0); end = vm_read32(bd + 0x4);
        }
        uint32_t fl = vm_read32(0x40C00000u);
        if (get!=lg || put!=lp || cur!=lc || beg!=lb || end!=le || fl!=lf) {
            /* seg# of GET vs cur (producer write head) -> the phase gap */
            int gseg = (int)((get & 0x7FFFFF) >> 20);
            int cseg = (cur >= 0x40400000u && cur < 0x40C00000u) ? (int)((cur - 0x40400000u) >> 20) : -1;
            fprintf(stderr, "[phase] t=%5lu GET=%06X(s%d) PUT=%06X | cur=%08X(s%d) beg=%08X end=%08X | flips=%u | gap=%d\n",
                    GetTickCount()-t0, get, gseg, put, cur, cseg, beg, end, fl,
                    (cseg>=0)? cseg-gseg : 99);
            fflush(stderr);
            lg=get; lp=put; lc=cur; lb=beg; le=end; lf=fl;
        }
        Sleep(0);
    }
}

/* BUFDESC GEOMETRY DUMP (env YZ_DUMP_BUFDESC, 2026-06-20): resolve libgcm's buffer
 * descriptor (bufdesc = *(libgcm_toc 0x02114000 - 0x7FD8) = *0x0210C028) and dump the
 * segment-geometry fields the reserve func_02103AAC waits on (+0x10 dma-ctrl, +0x14 base,
 * +0x18, +0x1C, +0x20, +0x28, +0x30 seg-size, +0x38 guard, +0x4C type). Resolves the
 * conflict: init func_021036D4 writes +0x30=0x2000 but pt22 measured 0x40000 at runtime
 * + called it "dynamic". Three passes (after deadlock latch ~32s, then +8s twice) to
 * catch any dynamic change. Read-only; default boot unaffected. */
static DWORD WINAPI yz_bufdesc_dump(LPVOID)
{
    const uint32_t pbd = 0x02114000u - 0x7FD8u;     /* libgcm_toc - 0x7FD8 = 0x0210C028 */
    for (int pass = 0; pass < 3; pass++) {
        Sleep(pass == 0 ? 32000 : 8000);
        uint32_t bd  = vm_read32(pbd);
        uint32_t get = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_GET);
        uint32_t put = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT);
        fprintf(stderr, "[bufdesc pass%d] ptr@0x%08X -> bd=0x%08X | GET=0x%08X PUT=0x%08X\n",
                pass, pbd, bd, get, put);
        if (bd >= 0x10000u && bd < 0xE0000000u) {
            static const uint32_t offs[] = {0x10,0x14,0x18,0x1C,0x20,0x24,0x28,0x2C,
                                            0x30,0x34,0x38,0x3C,0x40,0x44,0x48,0x4C};
            for (uint32_t i = 0; i < sizeof(offs)/sizeof(offs[0]); i++)
                fprintf(stderr, "    bd+0x%02X = 0x%08X\n", offs[i], vm_read32(bd + offs[i]));
            uint32_t segsz = vm_read32(bd + 0x30);
            uint32_t base  = vm_read32(bd + 0x14);
            fprintf(stderr, "    => base(+0x14)=0x%08X  seg-size(+0x30)<<2=0x%X (%u KB)  guard(+0x38)<<2=0x%X\n",
                    base, segsz << 2, (segsz << 2) >> 10, vm_read32(bd + 0x38) << 2);
        }
        fflush(stderr);
    }
    return 0;
}

/* SINGLE-SEGMENT reserve override (env YZ_ONESEG, 2026-06-20). libgcm's segment-recycle
 * reserve func_02103AAC (r3 = bufdesc 0x0210C3FC: begin@+0/end@+4/current@+8; geometry
 * +0x14 base, +0x18 buffer-end, +0x28 reserve-off, +0x30 seg-size words, +0x34 seg-count)
 * carves the 8 MB FIFO into 8x1 MB segments and advances one per call. MEASURED wedge: when
 * GET follows a CALL OUT of the ring into an unfinalized display list (io 0x1104D00) and the
 * producer then needs to recycle a segment, the reserve waits for GET to return to the ring
 * bounds -> never -> deadlock (scratch/reserve2.txt #36).
 *
 * FIX: promote the ring to ONE segment spanning the whole buffer. While end != buffer-end,
 * set end = buffer-end (+ seg-size=0x200000 words/<<2=8MB, count=1) and SKIP the real reserve
 * -- the producer keeps writing linearly (no recycle) so it never wedges mid-frame; by the
 * time cur reaches buffer-end it has emitted many frames + finalized its display lists, GET
 * has drained, and the real reserve's WRAP path (end==bd+0x18) recreates the full segment and
 * waits on a drained GET. Returns 1 = handled (skip real reserve), 0 = run the real reserve. */
extern "C" int yz_gcm_reserve_oneseg(ppu_context* ctx)
{
    /* OBSERVE-ONLY (the skip-based single-segment was PROVEN structurally wrong, 2026-06-20:
     * func_02103AAC is the per-fragment FIFO COMMIT -- it writes the fragment JUMP, advances
     * the segment AND flushes PUT. Skipping it froze PUT at 0x3544 while cur advanced =
     * starved GET, scratch/oneseg2.txt. So the reserve cannot be bypassed; a correct
     * single-segment would have to REPLICATE its commit minus the recycle-wait. The wedge's
     * true cause: the reserve's GET-wait never clears because GET is parked OUTSIDE the ring
     * (display list io 0x1104D00, > bd+0x20=0x7FFFFC) -- a producer-finalization root.)
     * Always returns 0 = run the real reserve; just trace the call state. */
    uint32_t bd = (uint32_t)ctx->gpr[3];
    if (!bd) return 0;
    static int logn = 0;
    if (logn < 40) { logn++;
        fprintf(stderr, "[reserve #%d t%u] bd=0x%08X begin=0x%08X end=0x%08X cur=0x%08X | "
                "GET=0x%06X PUT=0x%06X | seg=0x%X cnt=0x%X\n",
                logn, yz_thread_current_id(), bd, vm_read32(bd + 0x0), vm_read32(bd + 0x4),
                vm_read32(bd + 0x8), vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_GET),
                vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT),
                vm_read32(bd + 0x30), vm_read32(bd + 0x34)); fflush(stderr); }
    return 0;
}

/* SINGLE-BIG-SEGMENT (env YZ_SEGBIG, 2026-06-20, the principled LAYER-1 fix). libgcm
 * carves the 8 MB FIFO into 1 MB segments (bufdesc+0x30 = 0x40000 words); t1 wedges in
 * the reserve recycling a segment whose GET is parked (proven: resync past it -> frame 3
 * flips). Make the segment size = the rest of the buffer so t1 builds whole frames without
 * recycling. We poke bufdesc+0x30 AS SOON AS init sets it (while ctx is still at segment 0,
 * end=base+1MB), so the next reserve-advance creates a ~7 MB segment ending exactly at the
 * 8 MB buffer end (no overrun). bufdesc = *(libgcm_toc 0x02114000 - 0x7FD8) = *0x0210C028. */
static DWORD WINAPI yz_segsize_mon(LPVOID)
{
    const uint32_t pbd = 0x02114000u - 0x7FD8u;   /* 0x0210C028 */
    for (int i = 0; i < 100000; i++) {
        uint32_t bd = vm_read32(pbd);
        if (bd >= 0x10000u && bd < 0xE0000000u && vm_read32(bd + 0x30u) == 0x40000u) {
            vm_write32(bd + 0x30u, 0x1C0000u);    /* ~7 MB: next segment = rest of buffer */
            fprintf(stderr, "[segbig] bd=0x%08X: seg-size 0x40000 -> 0x1C0000 (single big segment)\n", bd);
            fflush(stderr);
            return 0;
        }
        Sleep(1);
    }
    fprintf(stderr, "[segbig] bufdesc seg-size never read 0x40000; not poked\n");
    return 0;
}

/* Direct write-watch on the frame-3 display list io 0x1104D00 (EA 0x41504D00), armed
 * independent of the consumer path (YZ_WATCH_LIST only arms when GET reaches the CALL,
 * which never happens in faithful YZ_NO_DEFER mode). Waits for the io map + display-list
 * region to commit (~6s), then arms. Answers: does the PRODUCER ever build this list? */
static DWORD WINAPI yz_watch_dlea_mon(LPVOID)
{
    const char* s = getenv("YZ_WATCH_DLEA");
    uint32_t ea = (s && s[0] && s[1] != '\0') ? (uint32_t)strtoul(s, nullptr, 16) : 0x41504D00u;
    if (ea < 0x10000u) ea = 0x41504D00u;   /* "1" etc -> default frame-3 list */
    /* Poll until the page commits, then arm immediately -- catches the FIRST write
     * (e.g. frames 1-2 building io 0x1100100 at ea 0x41500100) to name the producer. */
    for (int i = 0; i < 600; i++) {
        MEMORY_BASIC_INFORMATION mbi;
        if (vm_base && VirtualQuery(vm_base + ea, &mbi, sizeof(mbi)) && (mbi.State & MEM_COMMIT)) {
            fprintf(stderr, "[watch-dlea] arming write-watch on ea 0x%08X (committed @ ~%dms)\n", ea, i * 20);
            yz_watch_arm(ea);
            return 0;
        }
        Sleep(20);
    }
    fprintf(stderr, "[watch-dlea] ea 0x%08X never committed; not armed\n", ea);
    return 0;
}

/* OP-LIST APPEND TRACE (env YZ_WATCH_OPLIST, 2026-06-20 pt25b): the data-patch APPLY
 * never runs (the deadlock is before frame-finalize), but the APPEND does -- t1 builds
 * the frame, deferring data patches (tag 0x04/0x08/0x09) into the op-list. Arm a
 * write-watch on the op-list BASE (entry[0]'s tag field, which the YZ_DUMP_SEG dump
 * showed is a tag-0x08 data patch); the watch fires in the appender's context and dumps
 * its reliable caller chain -> NAMES the function that defers a data patch. That function
 * (+ its apply mirror, usually the same module) is the key to the drain. base = *(S+8),
 * S = *(game_toc-0x7410). Arms once the op-list is allocated. */
static DWORD WINAPI yz_oplist_watch_mon(LPVOID)
{
    for (int i = 0; i < 4000; i++) {
        if (g_yz_game_toc) {
            uint32_t S = vm_read32(g_yz_game_toc - 0x7410u);
            if (S >= 0x10000u && S < 0xE0000000u) {
                uint32_t base = vm_read32(S + 0x08u);
                if (base >= 0x10000u && base < 0xE0000000u) {
                    fprintf(stderr, "[oplist-watch] arming write-watch on op-list base 0x%08X "
                            "(S=0x%08X) -- names the data-patch APPENDER\n", base, S);
                    fflush(stderr);
                    yz_watch_arm(base);   /* entry[0] tag field */
                    return 0;
                }
            }
        }
        Sleep(5);
    }
    fprintf(stderr, "[oplist-watch] op-list base never resolved; not armed\n");
    return 0;
}

/* Pacing A/B (2026-06-20, LAYER-1). The consumer is a background CreateThread that
 * Sleep(1)s at every idle/park point. On Windows Sleep(1) rounds up to the timer
 * quantum (~1 ms with timeBeginPeriod, else ~15 ms), so when GET catches up to PUT
 * or parks on a stopper the consumer NAPS while t1 keeps filling the ring -> t1
 * laps GET and wedges in libgcm's reserve usleep (func_02103AAC). YZ_TIGHT swaps the
 * idle naps for a continuous, FAIR spin (YieldProcessor pause + SwitchToThread) so
 * the consumer drains like a hardware pipeline -- testing whether pacing alone keeps
 * t1 out of the reserve. Default boot path unchanged (tight off). */
static inline void rsx_idle(int tight)
{
    if (tight) {
        for (int i = 0; i < 64; i++) YieldProcessor();
        SwitchToThread();   /* yield to t1 if it's ready on this core; return now if not */
    } else {
        Sleep(1);
    }
}

/* ===========================================================================
 * Faithful RSX FIFO consumer (clean-room reimplementation of RPCS3's
 * FIFO_control + rsx::thread::run_FIFO, Emu/RSX/RSXFIFO.cpp). Replaces ~10
 * sessions of band-aids. Sony's libgcm owns the ring; the guest (t1) produces
 * commands and advances PUT; this thread is the RSX side that consumes the
 * committed [GET, PUT) and advances GET.
 * ===========================================================================*/

/* GET serialization (RPCS3 sys_rsx_mtx analogue). The two writers of the
 * DMA-control GET register -- this consumer loop and the guest sys_rsx pkg001
 * (FIFO set get/put) path -- serialize here so a pkg001 set never tears a GET
 * the consumer is advancing, and the consumer never overwrites a pkg001 set
 * with a stale-derived GET. Race-safe lazy init via InitOnce. */
static CRITICAL_SECTION g_rsx_fifo_lock;
static INIT_ONCE        g_rsx_fifo_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK yz_rsx_fifo_init_cb(PINIT_ONCE, PVOID, PVOID*)
{
    InitializeCriticalSection(&g_rsx_fifo_lock);
    return TRUE;
}
static void yz_rsx_fifo_lock_ensure(void)
{
    InitOnceExecuteOnce(&g_rsx_fifo_once, yz_rsx_fifo_init_cb, NULL, NULL);
}

extern "C" void yz_rsx_wait_classifier_shutdown_serialized(void)
{
    /* Serialize vertical finalization with the same lock held by every FIFO
     * adapter/backend step.  Ordinary lanes only emit fixed counters here;
     * the default-off Hana oracle may additionally retire its pre-recorded
     * copies before mapping the bounded readback buffer. */
    yz_rsx_fifo_lock_ensure();
    EnterCriticalSection(&g_rsx_fifo_lock);
    yz_nr_vertical_shutdown();
    if (g_yz_rsx_wait_classifier_enabled)
        yz_rsx_wait_classifier_shutdown();
    if (g_yz_fe0_timeline_enabled)
        yz_fe0_timeline_shutdown();
    if (g_yz_wkl4_cycle_enabled)
        yz_wkl4_cycle_shutdown();
    yz_nr_shadow_shutdown();
    LeaveCriticalSection(&g_rsx_fifo_lock);
}

/* Exported for the flow-control band-aid (main.cpp yz_flip_advance). Audit
 * 2026-07-01: the band-aid wrote GET and the fence with NO lock, racing this
 * consumer's read-decide-write window -- GET could be clobbered BACKWARD
 * (producer-stall hazard) and the fence could double-advance vs the faithful
 * vblank path. All GET/fence writers must hold g_rsx_fifo_lock. */
extern "C" void yz_rsx_fifo_acquire(void)
{
    yz_rsx_fifo_lock_ensure();
    EnterCriticalSection(&g_rsx_fifo_lock);
}
extern "C" void yz_rsx_fifo_release(void)
{
    LeaveCriticalSection(&g_rsx_fifo_lock);
}

extern "C" int yz_rsx_flip_pending_any(void)
{
    for (int h = 0; h < 8; ++h)
        if (g_rsx_flip_pending[h]) return 1;
    return 0;
}

/* s37 fix (scratch/s37_render_ordering.md, ledger #82 candidate): RPCS3
 * retires a flip -- present + flip-done bit + label clear + the throttle
 * fence bump + FLIP event -- ON THE RSX/FIFO-CONSUMER THREAD, in FIFO order,
 * when GET reaches the flip (ORACLE RSXThread.cpp:3383 handle_emu_flip,
 * sys_rsx.cpp:880-892 0xFEC clears label+0x10). The vblank handler explicitly
 * REFUSES to run on a ppu/timer thread (ORACLE sys_rsx.cpp:896-900, "wrong
 * thread"). Our default retires on the free-running 60 Hz vblank timer
 * instead (yz_rsx_vblank_tick below), decoupling the render throttle's fence
 * from actual consumer progress -- t1 can arm a flip and outrun the consumer
 * because the fence bumps on wall-clock, not on drain.
 *
 * YZ_FLIP_ON_CONSUMER (default OFF) moves the retire into yz_rsx_fifo_step
 * (below), gated on g_rsx_fifo_lock, so it fires exactly once per arm right
 * after GET has drained past the flip -- restoring the RPCS3 lockstep. When
 * ON, yz_rsx_vblank_tick's per-head retire is skipped entirely (it does only
 * vBlankCount + the VBLANK event, mirroring RPCS3's 0xFED). Default OFF is a
 * one-token change at each call site: zero behavior change from the
 * pre-existing vblank-thread retire. */
static int yz_flip_on_consumer(void)
{
    /* Keep the whole-boot A/B, and add an a010-scoped form so the render-order
     * experiment does not perturb the already-working movie/title path. */
    static int mode = -1; /* 0=vblank, 1=consumer always, 2=consumer during a010 */
    if (mode < 0) {
        mode = getenv("YZ_FLIP_ON_CONSUMER") ? 1 :
               getenv("YZ_A010_FLIP_ON_CONSUMER") ? 2 : 0;
        fprintf(stderr,
                "[flip-consumer] armed: mode=%s "
                "(YZ_FLIP_ON_CONSUMER=%s YZ_A010_FLIP_ON_CONSUMER=%s)\n",
                mode == 1 ? "consumer-always" :
                mode == 2 ? "consumer-during-a010" : "vblank-default",
                mode == 1 ? "1" : "0", mode == 2 ? "1" : "0");
        fflush(stderr);
    }
    return mode == 1 || (mode == 2 && g_yz_a010_root_active);
}

/* Faithful rules (NO heuristics, NO deferred-release, NO GET-forcing):
 *   - GET re-read every iteration; PUT bounds us to [GET, PUT). get == put =>
 *     drained: yield and re-poll. GET NEVER reaches or passes PUT.
 *   - old jump (cmd & 0xE0000003 == 0x20000000) / new jump (cmd & 3 == 1):
 *     follow. A jump-to-self (target == get) is the producer's stopper: spin in
 *     place (memwatch) until t1 patches the word; never force past it.
 *   - call (cmd & 3 == 2): one-level return stack; jump to cmd & 0x1FFFFFFC.
 *   - return (cmd & 0xFFFF0003 == 0x00020000): pop the return.
 *   - method packet: count=(cmd>>18)&0x7FF, method=cmd&0x3FFFC,
 *     noninc=cmd&0x40000000. Require the whole packet committed (PUT covers
 *     header+args) before dispatching (RPCS3 inc_get waits for PUT per arg).
 *     Dispatch each arg via yz_rsx_method; an unsatisfied semaphore ACQUIRE
 *     stalls (leave GET on the packet, retry).
 *   - off-ring GET / unmapped jump target / malformed word: recover (resync
 *     GET=PUT, log once, clear the return stack) -- RPCS3 recover_fifo. */
/* One-level return stack, shared between the async consumer thread and any
 * inline pump (only ONE drains at a time -- YZ_RSX_INLINE disables the async
 * thread). Guarded by g_rsx_fifo_lock. */
static uint32_t g_fifo_ret = ~0u;
static uint32_t g_fifo_last_flow_source = ~0u;
static uint32_t g_fifo_last_flow_word = 0u;
static uint32_t g_fifo_last_flow_target = ~0u;
static uint32_t g_fifo_last_flow_kind = 0u; /* 1=jump, 2=call, 3=return */

/* s29 (scratch/s29_terminal_park_re.md, Q4): RPCS3 (RSXFIFO.cpp) treats BOTH a
 * nested CALL (a second CALL before the pending one RETURNs) and a RETURN with
 * no pending CALL as FIFO_ERROR and calls recover_fifo() -- checkpoint/retry,
 * escalating to a fatal abort after 20 recoveries inside a 2 s window. Our port
 * used to (a) silently clobber the one-level g_fifo_ret slot on a nested CALL
 * with zero diagnostic, and (b) idle forever, completely silently after one
 * one-ever warning, on a RETURN-without-CALL (the s28m10/s28m4 terminal park at
 * GET=0x011001EC / 0x0000098C). The loud detection log below fires
 * UNCONDITIONALLY (zero behavior change, always-on diagnostic per the report's
 * "smallest diagnostic first" recommendation). The checkpoint/retry+escalation
 * recovery itself is a behavior change and stays behind YZ_FIFO_RECOVER_RET
 * (default OFF -- unvalidated against a live boot; kill-switch semantics
 * inverted from YZ_NO_FIFO_RECOVER's sibling non-command path because this one
 * hasn't been through an A/B boot yet). Our step function has no valid
 * "rewind" target for either case (GET never advanced into the bad word), so
 * "restore to checkpoint" is a no-op position-wise -- the recovery's value is
 * the bounded, loud retry/escalation cadence instead of a truly-silent
 * infinite idle. Retirement: fold into the default path once a live boot
 * confirms neither state corrupts anything worse than idling. */
static int g_fifo_recover_ret_fatal = 0;
static ULONGLONG g_fifo_recover_ret_last_ms = 0;
static ULONGLONG g_fifo_recover_ret_window0 = 0;
static int g_fifo_recover_ret_n = 0;

static int yz_fifo_recover_ret_enabled(void)
{
    static int rr = -1;
    if (rr < 0) { rr = getenv("YZ_FIFO_RECOVER_RET") ? 1 : 0;
        if (rr) fprintf(stderr, "[fifo-rec-ret] ARMED (YZ_FIFO_RECOVER_RET): RETURN-without-CALL "
                "and CALL-inside-subroutine get the RPCS3 recover_fifo checkpoint-retry analog "
                "(20 strikes / 2s -> fatal) instead of silent-forever-idle / silent-clobber\n"); }
    return rr;
}

/* s33 [fifo-flow] (env YZ_FIFO_FLOWLOG): log every SUCCESSFUL flow-control
 * transfer. The s33 audit's discriminator: JUMP/CALL were silent on success,
 * so a stranded GET's arrival path (jump vs call vs walked) was never in the
 * log — s32resur1's teleport from io 0x8007C to 0x200BF4 had zero trace. */
static int yz_fifo_flowlog(void)
{
    static int fl = -1;
    if (fl < 0) { fl = getenv("YZ_FIFO_FLOWLOG") ? 1 : 0;
        if (fl) { fprintf(stderr, "[fifo-flow] ARMED (jump/call/return transition log)\n"); fflush(stderr); } }
    return fl;
}

/* One recovery attempt for the RETURN-without-CALL / CALL-inside-subroutine
 * states. Rate-limited to ~1 attempt/50ms (our poll loop has no RPCS3-style
 * blocking 2ms sleep between retries, so this substitutes the existing
 * SwitchToThread poll cadence -- the strike count and 2s window are faithful
 * to the RPCS3 shape). After 20 strikes inside a rolling 2s window, latches
 * fatal permanently: one loud print, then this and all further calls are a
 * cheap no-op so the FIFO consumer's existing idle/heartbeat machinery takes
 * over (matches "kills RSX" in spirit without tearing down the process --
 * the consumer parks, loudly, instead of RPCS3's hard exception). */
static void yz_fifo_ret_recover(uint32_t get, const char* what)
{
    if (g_fifo_recover_ret_fatal) return;
    const ULONGLONG now = GetTickCount64();
    if (g_fifo_recover_ret_last_ms && now - g_fifo_recover_ret_last_ms < 50) return;
    g_fifo_recover_ret_last_ms = now;
    if (!g_fifo_recover_ret_window0 || now - g_fifo_recover_ret_window0 > 2000) {
        g_fifo_recover_ret_window0 = now; g_fifo_recover_ret_n = 0;
    }
    g_fifo_recover_ret_n++;
    if (g_fifo_recover_ret_n >= 20) {
        g_fifo_recover_ret_fatal = 1;
        fprintf(stderr, "[fifo-rec-ret] FATAL: %s struck %d recoveries within 2s at io=0x%08X -- "
                "giving up (RPCS3 recover_fifo analog); FIFO consumer parks permanently\n",
                what, g_fifo_recover_ret_n, get);
        fflush(stderr);
        return;
    }
    fprintf(stderr, "[fifo-rec-ret] n=%d %s at io=0x%08X -- retry (RPCS3 recover_fifo analog)\n",
            g_fifo_recover_ret_n, what, get);
    fflush(stderr);
}

/* t1 hop counter from dispatch.cpp — the park-rel fast path's wedge witness:
 * a t1 that makes no hops while the consumer sits parked cannot be on its way
 * to drain the release journal (the drain runs on t1). */
extern "C" volatile long g_yz_t1_sample_seq;
extern "C" volatile void* g_yz_t1_last_tf;   /* s25 spin-witness feed (dispatch.cpp) */
extern "C" volatile uint32_t g_yz_jrnl_cur_ea;  /* s34 live journal-consumer cursor EA (spu_channels.c / spu_dma.h) */

/* The default FIFO buffer starts behind one deliberately published
 * jump-to-self guard.  It is the only self-stopper the consumer may release
 * from PUT alone: segment zero, its reserved 0x1000 head, and a stable PUT
 * snapshot proving a later command boundary.  All recycled/in-stream
 * stoppers retain their journal/SPU publication lifecycle.  The caller owns
 * the final GET write so this same exact rule is usable by both FIFO owners. */
extern "C" int yz_rsx_try_release_published_segment_head(
    void*, uint32_t get, uint32_t put, uint32_t command,
    uint32_t* resume_get)
{
    if (!resume_get || !g_yz_gcm_segment_bytes)
        return 0;
    const uint32_t ring = 0x800000u;
    const uint32_t segment = get / g_yz_gcm_segment_bytes;
    const uint32_t segment_head =
        segment * g_yz_gcm_segment_bytes +
        (segment == 0u ? 0x1000u : 0u);
    const uint32_t ahead = (put - get + ring) & (ring - 1u);
    if (segment != 0u || get != segment_head || ahead <= 4u ||
        ahead >= (ring >> 1))
        return 0;
    const uint32_t ea = yz_rsx_io_to_ea(get);
    if (!ea)
        return 0;
    MemoryBarrier();
    if (vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT) != put ||
        vm_read32(ea) != command)
        return 0;
    const uint32_t resume = (get + 4u) & (ring - 1u);
    vm_write32(ea, 0x20000000u | resume);
    *resume_get = resume;
    return 1;
}

/* Read-only strict-native query for an edge recorded by the title's exact
 * inline-data allocator. It never scans or repairs FIFO contents. */
extern "C" int yz_rsx_registered_data_island_edge(
    void*, uint32_t get, uint32_t put, uint32_t command,
    uint32_t* resume_get)
{
    if (!resume_get)
        return 0;
    const uint32_t source_ea = yz_rsx_io_to_ea(get);
    uint32_t saved_command = 0u;
    uint32_t data_end_ea = 0u;
    uint32_t generation = 0u;
    uint32_t record_source_ea = source_ea;
    uint32_t resume = 0u;
    int result = 1;
    if (yz_a010_data_island_snapshot(
            source_ea, &saved_command, &data_end_ea, &generation) &&
        saved_command == command) {
        resume = yz_fifo_registered_inline_island_resume(
            source_ea, saved_command, data_end_ea, get, put,
            0x40400000u, 0x800000u);
    }

    /* The allocator writes up to four alignment zeros before its edge. If the
     * consumer observes one transient zero, GET reaches the exact payload
     * start before the edge becomes visible. Probe only those four possible
     * source slots in the producer-record table; never scan FIFO contents or
     * admit an interior payload word. */
    if (!resume) {
        result = 2;
        for (uint32_t delta = 4u; delta <= 0x10u && get >= delta;
             delta += 4u) {
            record_source_ea = yz_rsx_io_to_ea(get - delta);
            if (!record_source_ea ||
                !yz_a010_data_island_snapshot(
                    record_source_ea, &saved_command, &data_end_ea,
                    &generation))
                continue;
            resume = yz_fifo_registered_inline_island_member_resume(
                record_source_ea, saved_command, data_end_ea,
                get, put, 0x40400000u, 0x800000u);
            if (resume)
                break;
        }
    }
    if (!resume)
        return 0;
    MemoryBarrier();
    uint32_t recheck_command = 0u;
    uint32_t recheck_end = 0u;
    uint32_t recheck_generation = 0u;
    if (vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT) != put ||
        vm_read32(source_ea) != command ||
        vm_read32(record_source_ea) != saved_command ||
        !yz_a010_data_island_snapshot(
            record_source_ea, &recheck_command, &recheck_end,
            &recheck_generation) ||
        recheck_command != saved_command || recheck_end != data_end_ea ||
        recheck_generation != generation)
        return 0;
    *resume_get = resume;
    return result;
}

/* A generated-block tail is ambiguous from bytes alone: the captured
 * 0x41FFFC case is recycled float data, while 0x45FFFC is a real one-argument
 * method packet released by the immediately preceding stopper.  Admit the
 * latter only when gs_task's exact 0x5F00 MFC_PUTF publication record matches
 * this source/command generation.  The SPU helper publishes that record only
 * after the dependent bytes are visible and clears it when 0x5F70 recycles
 * the source slot. */
extern "C" int yz_rsx_generated_boundary_release_snapshot(
    uint32_t source_ea, uint32_t command);

extern "C" int yz_rsx_released_generated_boundary_edge(
    void*, uint32_t get, uint32_t put, uint32_t command,
    uint32_t target, uint32_t target_word)
{
    (void)put;
    const uint32_t ring = 0x800000u;
    const uint32_t block = 0x20000u;
    if (get >= ring || target != get + 4u ||
        ((target + 4u) & (block - 1u)) != 0u)
        return 0;
    const uint32_t source_ea = yz_rsx_io_to_ea(get);
    const uint32_t target_ea = yz_rsx_io_to_ea(target);
    if (!source_ea || !target_ea ||
        !yz_rsx_generated_boundary_release_snapshot(source_ea, command))
        return 0;
    MemoryBarrier();
    return vm_read32(source_ea) == command &&
           vm_read32(target_ea) == target_word;
}

/* Strict-native counterpart of the retained a010 generated-link repair.
 * This is reached only after the frame owner has observed one unchanged bad
 * target for a bounded publication interval.  It performs one fail-closed
 * structural proof in a fixed 1 MiB generated-list window, anchored to the
 * exact next FE0 user-command value, then revalidates every source witness
 * before publishing a replacement JUMP.  It neither renders nor falls back
 * to the legacy consumer and emits no per-event output. */
extern "C" int yz_rsx_resolve_published_generated_link(
    void*, uint32_t get, uint32_t put, uint32_t command,
    uint32_t target, uint32_t target_word, uint32_t* resume_get)
{
    if (!resume_get || !yz_a010_fifo_publication_repair_enabled())
        return 0;
    const uint32_t ring = 0x800000u;
    const uint32_t mask = ring - 1u;
    if (get >= ring || target >= ring)
        return 0;
    const int old_jump =
        (command & 0xE0000003u) == 0x20000000u;
    const int new_jump = (command & 3u) == 1u;
    if (!old_jump && !new_jump)
        return 0;
    const uint32_t encoded_target = new_jump
        ? (command & 0xFFFFFFFCu) : (command & 0x1FFFFFFCu);
    if (encoded_target != target)
        return 0;
    const int target_flow =
        ((target_word & 0xE0000003u) == 0x20000000u) ||
        ((target_word & 3u) == 1u) || ((target_word & 3u) == 2u) ||
        ((target_word & 0xFFFF0003u) == 0x00020000u);
    const int target_method =
        (target_word & 0xA0030003u) == 0u &&
        ((target_word >> 18) & 0x7FFu) != 0u;
    const uint32_t generated_block = 0x20000u;
    const uint32_t local_resume = (target + 4u) & mask;
    const uint32_t local_end = (local_resume + generated_block) & mask;
    const int local_boundary =
        ((target + 4u) & (generated_block - 1u)) == 0u;
    /* A generated-block tail is producer-owned link storage.  Recycled
     * float/constant bytes can syntactically resemble a valid packet, so the
     * exact boundary proof below—not packet shape—owns admission there. */
    if (target_flow || (target_method && !local_boundary))
        return 0;

    const uint32_t source_ea = yz_rsx_io_to_ea(get);
    const uint32_t target_ea = yz_rsx_io_to_ea(target);
    if (!source_ea || !target_ea)
        return 0;

    /* EDGE's generated command arena is divided into 128 KiB blocks.  A
     * primary FIFO JUMP initially targets the final word of one block; that
     * word is producer-owned link storage and can still hold recycled
     * vertex/constant payload when the source edge becomes visible.  This
     * occurs before the a010 scene root is active as well as during gameplay,
     * so use a protocol proof rather than a scene gate: the following block
     * must contain an exact generated prologue whose prefix is draw-balanced
     * and ends at its own self-stopper.  Recycled payload may precede that
     * first finalized prologue. */
    const uint32_t local_candidate = local_boundary
        ? yz_a010_find_balanced_generated_prefix(local_resume, local_end)
        : 0u;
    const int local_prologue = local_candidate &&
        yz_a010_generated_prologue_at(local_candidate);
    const int local_balanced = local_prologue &&
        yz_a010_balanced_generated_prefix_at(
            local_candidate, local_resume, local_end);
    const uint32_t exact_resume =
        yz_fifo_generated_block_candidate_resume(
        target, target_word, local_candidate, ring, generated_block,
        local_prologue, local_balanced);
    if (exact_resume) {
        MemoryBarrier();
        const uint32_t recheck_candidate =
            yz_a010_find_balanced_generated_prefix(
                local_resume, local_end);
        if (vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT) != put ||
            vm_read32(source_ea) != command ||
            vm_read32(target_ea) != target_word ||
            recheck_candidate != exact_resume ||
            yz_fifo_generated_block_candidate_resume(
                target, vm_read32(target_ea), exact_resume,
                ring, generated_block,
                yz_a010_generated_prologue_at(exact_resume),
                yz_a010_balanced_generated_prefix_at(
                    exact_resume, local_resume, local_end)) !=
                exact_resume)
            return 0;
        const uint32_t repaired = new_jump
            ? (exact_resume | 1u)
            : (0x20000000u | (exact_resume & 0x1FFFFFFCu));
        vm_write32(source_ea, repaired);
        MemoryBarrier();
        if (vm_read32(source_ea) != repaired)
            return 0;
        *resume_get = exact_resume;
        return 1;
    }
    if (local_boundary && !exact_resume)
        return -1; /* exact EDGE block; producer has not finalized it yet */

    /* The older FE0-anchored search is specifically an a010 generated-chain
     * recovery.  Keep that broader proof scene-gated; only the exact block
     * boundary contract above is valid protocol-wide. */
#if defined(YZ_PERF_CLEAN)
    if (ReadAcquire(&g_yz_a010_root_active) == 0)
        return 0;
#else
    if (InterlockedCompareExchange(&g_yz_a010_root_active, 0, 0) == 0)
        return 0;
#endif

    const uint32_t fe0_before = vm_read32(RSX_REPORTS + 0xFE0u);
    const uint32_t scan_start = (target + 4u) & mask;
    const uint32_t scan_end =
        (scan_start + 0x100000u) & mask;
    const uint32_t resume =
        yz_a010_find_pending_chain(scan_start, scan_end, 0);
    if (!resume || !yz_a010_generated_prologue_at(resume))
        return 0;

    MemoryBarrier();
    if (vm_read32(RSX_REPORTS + 0xFE0u) != fe0_before ||
        vm_read32(source_ea) != command ||
        vm_read32(target_ea) != target_word ||
        !yz_a010_generated_prologue_at(resume))
        return 0;
    const uint32_t repaired = new_jump
        ? (resume | 1u)
        : (0x20000000u | (resume & 0x1FFFFFFCu));
    vm_write32(source_ea, repaired);
    MemoryBarrier();
    if (vm_read32(source_ea) != repaired)
        return 0;
    *resume_get = resume;
    return 1;
}

/* A primary cursor can land immediately after a complete method packet but
 * before the generated draw prologue, with a short inline data tail in
 * between (the stable a010 example is 0x1278: 3A2AAAAB 04000000, followed by
 * the exact prologue at 0x1280). Prove the same complete generated chain as
 * the link repair before advancing; never search on every poll and never
 * treat a merely command-shaped word as sufficient. */
extern "C" int yz_rsx_resolve_published_generated_hole(
    void*, uint32_t get, uint32_t put, uint32_t word,
    uint32_t previous_get, uint32_t previous_command,
    uint32_t* resume_get)
{
    if (!resume_get || !yz_a010_fifo_publication_repair_enabled())
        return 0;
    const uint32_t ring = 0x800000u;
    const uint32_t mask = ring - 1u;
    if (get >= ring)
        return 0;
    const uint32_t hole_ea = yz_rsx_io_to_ea(get);
    if (!hole_ea || vm_read32(hole_ea) != word)
        return 0;

    /* Protocol-wide EDGE generated-block boundary.  Do this only from the
     * owner's unsupported/malformed-word path: supported methods which happen
     * to occupy the same 128 KiB alignment are ordinary commands and never
     * enter this proof. */
    const uint32_t generated_block = 0x20000u;
    const uint32_t block_resume = (get + 4u) & mask;
    const uint32_t block_end =
        (block_resume + generated_block) & mask;
    const int block_boundary =
        ((get + 4u) & (generated_block - 1u)) == 0u;
    const uint32_t block_candidate = block_boundary
        ? yz_a010_find_balanced_generated_prefix(block_resume, block_end)
        : 0u;
    const int block_prologue = block_candidate &&
        yz_a010_generated_prologue_at(block_candidate);
    const int block_balanced = block_prologue &&
        yz_a010_balanced_generated_prefix_at(
            block_candidate, block_resume, block_end);
    const uint32_t block_exact =
        yz_fifo_generated_block_candidate_resume(
        get, word, block_candidate, ring, generated_block,
        block_prologue, block_balanced);
    if (block_exact) {
        MemoryBarrier();
        const uint32_t recheck_candidate =
            yz_a010_find_balanced_generated_prefix(
                block_resume, block_end);
        if (vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT) == put &&
            vm_read32(hole_ea) == word &&
            recheck_candidate == block_exact &&
            yz_fifo_generated_block_candidate_resume(
                get, vm_read32(hole_ea), block_exact,
                ring, generated_block,
                yz_a010_generated_prologue_at(block_exact),
                yz_a010_balanced_generated_prefix_at(
                    block_exact, block_resume, block_end)) ==
                block_exact) {
            *resume_get = block_exact;
            return 1;
        }
        return -1;
    }
    if (block_boundary && !block_exact)
        return -1;

    /* Protocol-wide complete inline generated-VP gap. The owner must have
     * consumed the exact preceding NOOP. Find only the first exact prologue
     * in the producer's bounded 0x100-byte inline window, then prove its
     * complete draw-balanced chain independently. This covers both captured
     * legal payload spans (0x28 and 0x38) without executing or interpreting
     * any raw constants between the NOOP and the prologue. */
    const uint32_t inline_scan_start = (get + 4u) & mask;
    const uint32_t inline_scan_end = (get + 0x104u) & mask;
    const uint32_t inline_resume = yz_a010_find_generated_prologue(
        inline_scan_start, inline_scan_end);
    const uint32_t inline_end =
        (inline_resume + generated_block) & mask;
    const int inline_prologue = inline_resume &&
        yz_a010_generated_prologue_at(inline_resume);
    const int inline_balanced = inline_prologue &&
        yz_a010_balanced_generated_prefix_at(
            inline_resume, inline_resume, inline_end);
    const int inline_flow =
        ((word & 0xE0000003u) == 0x20000000u) ||
        ((word & 3u) == 1u) || ((word & 3u) == 2u);
    const uint32_t inline_flow_target = (word & 3u) == 1u
        ? (word & 0xFFFFFFFCu) : (word & 0x1FFFFFFCu);
    const int inline_flow_unmapped =
        inline_flow && inline_flow_target >= ring;
    const uint32_t inline_exact =
        yz_fifo_generated_vp_inline_candidate_resume(
            previous_get, previous_command, word, get, put,
            inline_resume, ring, 0x100u,
            inline_flow_unmapped,
            inline_prologue, inline_balanced);
    if (inline_exact) {
        const uint32_t previous_ea = yz_rsx_io_to_ea(previous_get);
        MemoryBarrier();
        if (previous_ea &&
            vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT) == put &&
            vm_read32(previous_ea) == previous_command &&
            vm_read32(hole_ea) == word &&
            yz_a010_find_generated_prologue(
                inline_scan_start, inline_scan_end) == inline_exact &&
            yz_fifo_generated_vp_inline_candidate_resume(
                previous_get, vm_read32(previous_ea), vm_read32(hole_ea),
                get, put, inline_exact, ring, 0x100u,
                inline_flow_unmapped,
                yz_a010_generated_prologue_at(inline_exact),
                yz_a010_balanced_generated_prefix_at(
                    inline_exact, inline_exact, inline_end)) ==
                inline_exact) {
            *resume_get = inline_exact;
            return 1;
        }
        return -1;
    }
    /* Preserve one pending dependency when the exact NOOP/gap layout is
     * present but its prologue or terminal stopper is still publishing. */
    if (yz_fifo_generated_vp_inline_candidate_resume(
            previous_get, previous_command, word, get, put,
            (get + 4u) & mask, ring, 0x100u,
            inline_flow_unmapped, 1, 1) &&
        (!inline_prologue || !inline_balanced))
        return -1;

    /* The captured 0x1278 family is the eight-byte alignment tail after one
     * exact 17-argument SET_TRANSFORM_CONSTANT_LOAD packet.  Prove that local
     * producer boundary directly; the previous broad generated-chain scan was
     * both unnecessarily expensive and too strict for a sequential prologue
     * whose later completion packet has not been published yet. */
    const uint32_t previous = (get - 0x48u) & mask;
    const uint32_t tail = (get + 4u) & mask;
    const uint32_t local_resume = (get + 8u) & mask;
    const uint32_t tail_ea = yz_rsx_io_to_ea(tail);
    const uint32_t tail_word = tail_ea ? vm_read32(tail_ea) : 0u;
    const int exact_predecessor =
        previous_get == previous && previous_command == 0x00441EFCu;
    const int local_prologue_ready =
        yz_a010_generated_prologue_at(local_resume);
    const uint32_t exact_resume =
        yz_fifo_generated_vp_constant_tail_resume(
            exact_predecessor ? previous_command : 0u,
            word, get, put, ring,
            local_prologue_ready);
    if (exact_resume && tail_word == 0x04000000u) {
        MemoryBarrier();
        if (vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT) == put &&
            vm_read32(hole_ea) == word &&
            vm_read32(tail_ea) == 0x04000000u &&
            yz_fifo_generated_vp_constant_tail_resume(
                previous_command, vm_read32(hole_ea), get, put,
                ring, yz_a010_generated_prologue_at(exact_resume)) ==
                exact_resume) {
            *resume_get = exact_resume;
            return 1;
        }
        return -1;
    }
    /* Recognize the exact packet/tail shape independently of the following
     * prologue so an early proof cannot permanently latch out bytes which
     * the generated-list producer publishes later without another PUT. */
    if (exact_predecessor && tail_word == 0x04000000u &&
        !local_prologue_ready &&
        yz_fifo_generated_vp_constant_tail_resume(
            previous_command, word, get, put, ring, 1))
        return -1;

    /* Only the older broad search depends on the a010 FE0 scene root.  The
     * two exact local producer-boundary proofs above are valid throughout the
     * run and deliberately do not inherit this scene gate. */
#if defined(YZ_PERF_CLEAN)
    if (ReadAcquire(&g_yz_a010_root_active) == 0)
        return 0;
#else
    if (InterlockedCompareExchange(&g_yz_a010_root_active, 0, 0) == 0)
        return 0;
#endif

    const uint32_t scan_start = (get + 4u) & mask;
    const uint32_t scan_end = (scan_start + 0x100000u) & mask;
    const uint32_t resume =
        yz_a010_find_balanced_generated_prefix(scan_start, scan_end);
    if (!resume)
        return 0;
    MemoryBarrier();
    if (vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT) != put ||
        vm_read32(hole_ea) != word ||
        !yz_a010_balanced_generated_prefix_at(
            resume, scan_start, scan_end))
        return 0;
    *resume_get = resume;
    return 1;
}

/* Process exactly ONE FIFO command at GET (self-locked). Returns an observation
 * category: ADVANCING if GET advanced / a method dispatched, otherwise the
 * precise reason it remained idle or stalled. The RPCS3 run_FIFO step, factored
 * out of the old consumer loop so
 * it can run EITHER on the free-running async thread OR inline on the producer
 * thread (YZ_RSX_INLINE: drain coupled to the producer's PUT flush so it can't lap). */
template <bool WaitClassify>
static yz_rsx_wait_category yz_rsx_fifo_step_impl(void)
{
    if (!g_rsx_ctx_ready) {
        if constexpr (WaitClassify) {
            yz_rsx_wait_classifier_record(
                YZ_RSX_WAIT_NO_CONTEXT, 0,
                rsx_live_draw_get_completed_draws(), 0, 0);
        }
        return YZ_RSX_WAIT_NO_CONTEXT;
    }
    EnterCriticalSection(&g_rsx_fifo_lock);
    uint32_t       get = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_GET) & ~3u;
    const uint32_t put = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT) & ~3u;
    uint32_t wait_dispatched_methods = 0;
    const auto transition = [](yz_rsx_wait_category category) {
        if constexpr (WaitClassify)
            yz_rsx_wait_classifier_transition(category);
    };
    const auto finish = [&](yz_rsx_wait_category category) {
        if constexpr (WaitClassify) {
            yz_rsx_wait_classifier_record(
                category, wait_dispatched_methods,
                rsx_live_draw_get_completed_draws(), put, 1);
        }
        LeaveCriticalSection(&g_rsx_fifo_lock);
        return category;
    };
#if defined(YZ_PERF_CLEAN)
    /* This is a read-only lifetime test on the per-command production path.
     * InterlockedCompareExchange emits a locked read/modify/write even when
     * both operands are zero; the Akiyama production profile attributed
     * 0.715 CPU-s here.  ReadAcquire preserves the publication ordering and
     * aligned 32-bit atomicity without taking cache-line ownership.  Keep the
     * interlocked diagnostic posture outside the clean performance build. */
    const int a010_root = ReadAcquire(&g_yz_a010_root_active) != 0;
#else
    const int a010_root =
        InterlockedCompareExchange(&g_yz_a010_root_active, 0, 0) != 0;
#endif
    static unsigned long a010_packet_n = 0;
    static unsigned long a010_arg_n = 0;
    static unsigned long a010_jump_n = 0;
    static unsigned long a010_call_n = 0;
    static unsigned long a010_ret_n = 0;
    static unsigned long a010_sub_args[8] = {};
    static unsigned long a010_begin_n = 0;
    static unsigned long a010_end_n = 0;
    static unsigned long a010_array_n = 0;
    static unsigned long a010_index_n = 0;
    static unsigned long a010_vp_n = 0;
    static unsigned long a010_const_n = 0;
    static uint32_t a010_last_counted_get = 0xFFFFFFFFu;
    static uint32_t a010_src_min = 0xFFFFFFFFu;
    static uint32_t a010_src_max = 0;
    static int a010_flow_trace = -1;
    static int a010_const_trace = -1;
    struct yz_a010_draw_source {
        uint32_t index;
        uint32_t io;
        uint32_t ea;
        uint32_t ret_io;
        uint32_t header;
        uint32_t primitive;
    };
    static yz_a010_draw_source a010_draw_sources[2048] = {};
    static uint32_t a010_draw_source_n = 0;
    static int a010_draw_source_trace = -1;
    static int a010_draw_source_written = 0;
    static unsigned long a010_const_trace_n = 0;
    static uint32_t a010_const_load[8] = {};
#if defined(YZ_PERF_CLEAN)
    const int a010_diag_root = 0;
#else
    const int a010_diag_root = a010_root;
    if (a010_flow_trace < 0)
        a010_flow_trace = getenv("YZ_A010_FLOW") ? 1 : 0;
    if (a010_const_trace < 0)
        a010_const_trace = getenv("YZ_A010_CONST") ? 1 : 0;
    if (a010_draw_source_trace < 0)
        a010_draw_source_trace =
            getenv("YZ_A010_DRAW_SOURCE") ? 1 : 0;
#endif

    /* s33 [fifo-hb] (env YZ_FIFO_HB): uncapped 5 s GET/PUT heartbeat. Every
     * deep-boot terminal FIFO state so far was invisible because the apply/
     * park prints are count-capped (ledger #65 class); this always answers
     * "where is the FIFO right now". Placed BEFORE the empty check so a
     * drained ring (get==put, producer stopped) is a visible state too. */
    { static int hb = -1; static ULONGLONG hb_t = 0;
      if (hb < 0) { hb = getenv("YZ_FIFO_HB") ? 1 : 0;
          if (hb) { fprintf(stderr, "[fifo-hb] ARMED (5s GET/PUT heartbeat)\n"); fflush(stderr); } }
      if (hb) { const ULONGLONG now = GetTickCount64();
          if (now - hb_t >= 5000) { hb_t = now;
              const uint32_t hea = yz_rsx_io_to_ea(get);
              /* s38 reframe discriminator: log the gcm-journal PRODUCER HEAD
               * (S+0x00) + BASE (S+0x08), S=vm[game_toc-0x7410], alongside the
               * SPU consumer cursor (g_yz_jrnl_cur_ea). If the head keeps
               * ADVANCING while the cursor stays stuck in its small window =>
               * the consumer's window LIMIT never refreshes to the head (the
               * fix locus). If the head is FROZEN during the wedge => the
               * PRODUCER (t1) stalled and the consumer is exonerated (root
               * upstream). Distance head-cursor = how far behind the consumer is. */
              uint32_t jS = g_yz_game_toc ? vm_read32(g_yz_game_toc - 0x7410u) : 0u;
              uint32_t jhead = (jS >= 0x10000u && jS < 0xE0000000u) ? vm_read32(jS + 0x00u) : 0u;
              uint32_t jbase = (jS >= 0x10000u && jS < 0xE0000000u) ? vm_read32(jS + 0x08u) : 0u;
              /* Read the consumer's live LS cursor directly so this low-rate
               * heartbeat does not require the high-volume YZ_JRNL_WATCH. */
              uint32_t jcur  = yz_consumer_cursor();
              fprintf(stderr, "[fifo-hb] get=0x%08X put=0x%08X word=0x%08X ret=0x%08X | jhead=0x%08X jcur=0x%08X behind=0x%X jbase=0x%08X\n",
                      get, put, hea ? vm_read32(hea) : 0xDEADDEADu, g_fifo_ret,
                      jhead, jcur, (jhead > jcur) ? (jhead - jcur) : 0u, jbase);
              fflush(stderr); } } }

    /* FIFO_EMPTY: ring drained. Never reach/pass PUT. (RPCS3 read(): put==get) */
    if (get == put)
        return finish(YZ_RSX_WAIT_EMPTY);

    /* Strict full-native mode owns the FIFO at the actual serialized GET.
     * It translates and executes each method once and has no in-frame legacy
     * fallback. This precedes the legacy off-ring recovery deliberately: a
     * malformed native cursor is an exact bounded failure, never a silent
     * resync that discards published work. The ordinary producer-span,
     * section-scanner, and legacy decoder paths below are unreachable while
     * this owner is enabled. */
    uint32_t frame_get = get;
    uint32_t frame_ret = g_fifo_ret;
    const yz_nr_vertical_frame_result frame_result =
        yz_nr_vertical_consume_frame(
            get, put, g_fifo_ret, &frame_get, &frame_ret);
    if (frame_result == YZ_NR_VERTICAL_FRAME_ADVANCED) {
        if (frame_get == get || !yz_rsx_io_to_ea(frame_get))
            return finish(YZ_RSX_WAIT_BAD_FLOW);
        g_fifo_ret = frame_ret;
        vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, frame_get);
        return finish(YZ_RSX_WAIT_ADVANCING);
    }
    if (frame_result == YZ_NR_VERTICAL_FRAME_WAIT_EMPTY)
        return finish(YZ_RSX_WAIT_EMPTY);
    if (frame_result == YZ_NR_VERTICAL_FRAME_WAIT_PARTIAL)
        return finish(YZ_RSX_WAIT_UNFINALIZED_HOLE);
    if (frame_result == YZ_NR_VERTICAL_FRAME_WAIT_STOPPER)
        return finish(YZ_RSX_WAIT_SELF_STOPPER);
    if (frame_result == YZ_NR_VERTICAL_FRAME_WAIT_SEMAPHORE)
        return finish(YZ_RSX_WAIT_SEMAPHORE);
    if (frame_result == YZ_NR_VERTICAL_FRAME_FATAL)
        return finish(YZ_RSX_WAIT_BAD_FLOW);

    const uint32_t ea = yz_rsx_io_to_ea(get);
    if (!ea) {
        const uint32_t pea = yz_rsx_io_to_ea(put);
        if (put != get && pea) {
            static int n = 0; if (n < 12) { n++;
                fprintf(stderr, "[rsx] GET=0x%08X off-ring -> resync to PUT 0x%08X (recover)\n", get, put); }
            vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, put);
            g_fifo_ret = ~0u;
            return finish(YZ_RSX_WAIT_ADVANCING);
        }
        static int warned = 0;
        if (!warned) { warned = 1;
            fprintf(stderr, "[rsx] GET=0x%08X (PUT=0x%08X) not io-mapped; idling\n", get, put); }
        return finish(YZ_RSX_WAIT_BAD_FLOW);
    }

    /* Highest-safe producer interception owns typed spans by their exact
     * guest FIFO address.  A miss is the overwhelmingly common legacy path;
     * a claim executes once through the ordered typed backend and advances
     * GET across the reserved wire span without decoding its NOP words. */
    uint32_t native_words = 0;
    const yz_nr_vertical_consume_result native_result =
        yz_nr_vertical_consume(ea, &native_words);
    if (native_result == YZ_NR_VERTICAL_CONSUME_EXECUTED) {
        if (!native_words || native_words > 0x1000u)
            return finish(YZ_RSX_WAIT_BAD_FLOW);
        vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET,
                   get + native_words * 4u);
        if constexpr (WaitClassify)
            wait_dispatched_methods += native_words;
        return finish(YZ_RSX_WAIT_ADVANCING);
    }
    if (native_result == YZ_NR_VERTICAL_CONSUME_WAIT)
        return finish(YZ_RSX_WAIT_UNFINALIZED_HOLE);
    if (native_result == YZ_NR_VERTICAL_CONSUME_FALLBACK) {
        /* Native execution refused atomically. GET is deliberately unchanged;
         * continue below and decode the retained complete packet once. */
    }
    if (native_result == YZ_NR_VERTICAL_CONSUME_FATAL) {
        static int reported = 0;
        if (!reported++) {
            fprintf(stderr,
                    "[nr-vertical] fatal typed span at GET=0x%08X EA=0x%08X; "
                    "refusing legacy decode of reserved words\n",
                    get, ea);
            fflush(stderr);
        }
        return finish(YZ_RSX_WAIT_BAD_FLOW);
    }

    /* Transactional native ownership is deliberately attempted only after
     * exact producer-owned spans have had first refusal.  The section scanner
     * follows the live FIFO flow and either owns a complete preflighted island
     * or leaves GET/RET untouched for the ordinary legacy decoder below. */
    uint32_t section_get = get;
    uint32_t section_ret = g_fifo_ret;
    const yz_nr_vertical_section_result section_result =
        yz_nr_vertical_consume_section(
            get, put, g_fifo_ret, &section_get, &section_ret);
    if (section_result == YZ_NR_VERTICAL_SECTION_EXECUTED) {
        if (section_get == get || !yz_rsx_io_to_ea(section_get))
            return finish(YZ_RSX_WAIT_BAD_FLOW);
        g_fifo_ret = section_ret;
        vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, section_get);
        return finish(YZ_RSX_WAIT_ADVANCING);
    }
    if (section_result == YZ_NR_VERTICAL_SECTION_WAIT)
        return finish(YZ_RSX_WAIT_UNFINALIZED_HOLE);
    if (section_result == YZ_NR_VERTICAL_SECTION_FATAL) {
        static int section_fatal_reported = 0;
        if (!section_fatal_reported++) {
            fprintf(stderr,
                    "[nr-vertical] fatal native frame island at "
                    "GET=0x%08X PUT=0x%08X; refusing partial fallback\n",
                    get, put);
            fflush(stderr);
        }
        return finish(YZ_RSX_WAIT_BAD_FLOW);
    }

    const uint32_t cmd = vm_read32(ea);
    if constexpr (WaitClassify) {
        const yz_rsx_stopper_wait observed = {
            get, ea, put, (put - get + 0x800000u) & 0x7FFFFFu, cmd
        };
        /* If a previously observed stopper was patched, retain the published
         * exit word/PUT without taking a timestamp.  A later genuine phase
         * transition closes the episode. */
        yz_rsx_wait_classifier_stopper_observe(&observed, 0);
    }

    /* ---- control transfer ---- */
    if ((cmd & 0xE0000003u) == 0x20000000u || (cmd & 3u) == 1u) {   /* old | new jump */
        uint32_t tgt = (cmd & 3u) == 1u ? (cmd & 0xFFFFFFFCu)   /* NEW offset mask */
                                       : (cmd & 0x1FFFFFFCu);  /* OLD offset mask */
        if (tgt == get) {
            if constexpr (WaitClassify) {
                const yz_rsx_stopper_wait stopper = {
                    get, ea, put,
                    (put - get + 0x800000u) & 0x7FFFFFu, cmd
                };
                yz_rsx_wait_classifier_stopper_observe(&stopper, 1);
            }
            /* The reserved default-buffer head is initially guarded by a
             * self-jump.  Release only that startup guard from the consumer
             * side, after PUT proves a later command boundary was published.
             * Recycled and in-stream stoppers keep their journal lifecycle. */
            uint32_t published_resume = get;
            if (yz_rsx_try_release_published_segment_head(
                    nullptr, get, put, cmd, &published_resume)) {
                vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET,
                           published_resume);
                static unsigned released_heads = 0;
                if (++released_heads <= 16u) {
                    fprintf(stderr,
                            "[gcm] released published segment head "
                            "io=0x%06X PUT=0x%06X\n",
                            get, put);
                    fflush(stderr);
                }
                return finish(YZ_RSX_WAIT_ADVANCING);
            }
            /* Jump-to-self stopper. DEFERRED-RELEASE APPLY -- RETIRED (default
             * OFF 2026-07-02, layer-1 root-cause session; opt back in with
             * YZ_APPLY_REL=1 for A/B). This stand-in was built (2026-06-28)
             * when nothing released journaled stoppers because the REAL
             * consumer couldn't run. Post il/SPU_RET/backoff fixes, gs_task
             * (EDGE) demonstrably does the whole job itself: applies the
             * tag-0x04/08/09/10 patches (plain PUTs, LS pc 0xB60C) THEN
             * releases the stopper with a FENCED 4-byte PUT (pc 0x5F00) --
             * measured via the [gs-put] probe; with this path off, GET never
             * once met an unpatched stopper across 12 boots. Leaving it ON
             * races Sony's consumer: it releases WITHOUT the preceding
             * patches, handing GET unpatched content -- 3/3 applier-on boots
             * wedged t1 at ~+6 s at an identical site; 0/12 with it off.
             * Evidence: scratch/{bad1,cfgA*,cfgB*,val*}.err. The default is
             * now a faithful memwatch: spin at the stopper until the real
             * consumer patches it. Delete after quiet sessions. */
            const int journal_hle = yz_jrnl_hle_enabled();
            static int apply = -1;
            if (apply < 0) apply = (!journal_hle && getenv("YZ_APPLY_REL")) ? 1 : 0;
            /* YZ_PARK_REL (s21, the movie-phase deadlock triangle -- full map in
             * scratch/stopper_drain_re.md): at the logo->movie boundary a commit
             * crosses a segment recycle, so the game DEFERS this stopper's
             * release into its tag-0x7F op-list; the drain that would execute it
             * runs only after t1's flip throttle passes -- but the throttled
             * flips sit BEHIND this stopper. Permanent triangle. gs_task is
             * measured idle here (no geometry -> no journal work), so the June
             * race partner (double-apply during the geometry stream, the reason
             * YZ_APPLY_REL was retired) cannot exist. This narrow variant
             * applies the game's OWN journaled release ONLY after parking on
             * the SAME stopper for 3 s with PUT ahead -- a state that is
             * otherwise a permanent deadlock. Opt-in for A/B validation. */
            static int prel = -1; static unsigned fast_ms = 250;
            if (prel < 0) { prel = (!journal_hle && !getenv("YZ_NO_PARK_REL")) ? 1 : 0;
                const char* fm = getenv("YZ_PARKREL_FAST_MS");
                if (fm) fast_ms = (unsigned)atoi(fm);   /* 0 = fast path off (3 s tier only) */
                if (prel) fprintf(stderr, "[park-rel] ARMED (default): deadlock-only deferred-release apply, fast=%ums+t1-frozen witness, fallback=3000ms\n", fast_ms); }
            /* s24 FAST PATH: the 3 s tier alone cost 16x3 s = ~48 s/boot at the
             * movie boundary (scratch/s24pr1.err). Fire early ONLY when the
             * deadlock is witnessed, not merely suspected: (a) parked on this
             * stopper > fast_ms, (b) t1 made ZERO hops since the park began
             * (the journal drain runs on t1 — a frozen t1 cannot be coming),
             * (c) the release is in the game's journal (checked below — we
             * only ever deliver the game's own queued write). The 3 s tier
             * stays as the unconditional fallback for shapes the witness
             * misses (e.g. t1 busy in a long direct-call stretch). */
            static uint32_t park_ea = 0; static ULONGLONG park_t0 = 0;
            static long park_seq = 0; static void* park_tf = 0;
            if (ea != park_ea) { park_ea = ea; park_t0 = GetTickCount64();
                                 park_seq = g_yz_t1_sample_seq;
                                 park_tf  = (void*)g_yz_t1_last_tf; }
            const ULONGLONG parked_ms = GetTickCount64() - park_t0;
            /* s41 FLIGHT RECORDER dump trigger A: fire once when this stopper
             * park first crosses 30s (runtime/spu/spu_fltrec.c). Independent
             * of the [stop-jrnl] witness prints below; a no-op unless
             * YZ_FLTREC armed the recorder (yz_fltrec_dump checks that). */
            { static int fr_dumped = 0;
              if (!fr_dumped && parked_ms > 30000) {
                  fr_dumped = 1;
                  yz_fltrec_dump("park-30s");
              } }
            /* s40b v2 (the adversarial review's targeting requirement,
             * scratch/s40b_refute_unstick.md): publish the stopper EA the GPU is
             * provably parked on, so the SPU-side YZ_QROT_UNSTICK can fire ONLY
             * for the item carrying THIS release (not a blind lottery). >2s parked
             * = published; resets to 0 on any new park until it matures. */
            g_yz_parked_pub_ea = (parked_ms > 2000) ? ea : 0;
            /* s42 BOUNDARY-DRAIN iteration C -- PARK-TIME FIRE (YZ_BOUNDARY_DRAIN,
             * default OFF). The postmortem (scratch/s42_drainB_postmortem.md) proved
             * iteration B's fire MECHANISM sound (v2's fired release advanced GET past
             * every stopper) but its 0x352C-reinit fire SITE unreachable once the park
             * forms. Move the trigger HERE, to the site the wedged consumer actually
             * reaches: when the host FIFO consumer has been parked on the SAME jump-to-
             * self stopper past YZ_BDRAIN_DWELL_MS (default 3000ms), fire the drain's
             * existing guarded release write for THIS stopper EA only (RSX-window bounds
             * + JTS self-jump read-guard, in spu_channels.c -- NO host LS writes). Re-fire
             * is allowed per NEW distinct park (io/ea changes), capped at YZ_BDRAIN_CAP/
             * boot (default 64). BRIDGE, not a root fix: this is the retired park-release
             * lever's fire-point married to the drain's guarded faithful write; WHY our
             * engine stops applying releases post-storm is OPEN (durable road = block-lift
             * gs_task). v2 measured that crossing the wall exposes a downstream guest
             * fault, so expect new crash flavors past it (that is progress). */
            {
                static int      bd_on    = -1;
                static unsigned bd_dwell = 3000u, bd_cap = 64u;
                if (bd_on < 0) {
                    bd_on = getenv("YZ_BOUNDARY_DRAIN") ? 1 : 0;
                    const char* d = getenv("YZ_BDRAIN_DWELL_MS"); if (d) bd_dwell = (unsigned)atoi(d);
                    const char* c = getenv("YZ_BDRAIN_CAP");      if (c) bd_cap   = (unsigned)atoi(c);
                }
                if (bd_on) {
                    static uint32_t bd_acted_ea = 0;   /* last EA acted on (fire or skip) -- gates re-fire to NEW distinct parks */
                    static unsigned bd_fires    = 0;   /* successful guarded writes this boot */
                    if (parked_ms > bd_dwell && ea != bd_acted_ea && bd_fires < bd_cap) {
                        const uint32_t ring = vm_read32(ea);
                        fprintf(stderr, "[bdrain] FIRE: park-time -> parked=%llums @io 0x%06X ea=0x%08X ring@ea=0x%08X (fired so far=%u/%u)\n",
                                (unsigned long long)parked_ms, get, ea, ring, bd_fires, bd_cap);
                        fflush(stderr);
                        const int fired = yz_bdrain_fire_ea(ea, get);
                        bd_acted_ea = ea;   /* do not re-attempt this EA until a NEW distinct park forms */
                        if (fired) {
                            bd_fires++;
                            fprintf(stderr, "[bdrain] SUMMARY: FIRED release for ea=0x%08X (io 0x%06X); GET should advance past it next step. fires=%u/%u\n",
                                    ea, get, bd_fires, bd_cap);
                            /* s42: broken-phase recorder dump rides the drain — the drain
                             * fires only in the post-storm dead-engine phase, and the
                             * park-30s trigger can never fire while the drain keeps parks
                             * short. YZ_BDRAIN_DUMP_AT_FIRE=N dumps the flight-recorder
                             * ring at the Nth successful fire (solidly mid-broken-phase). */
                            { static int dump_at = -2;
                              if (dump_at == -2) { const char* s = getenv("YZ_BDRAIN_DUMP_AT_FIRE");
                                                   dump_at = s ? atoi(s) : -1; }
                              if (dump_at > 0 && (int)bd_fires == dump_at) {
                                  fprintf(stderr, "[bdrain] DUMP trigger: fire #%u -> fltrec dump (broken-phase capture)\n", bd_fires);
                                  yz_fltrec_dump("bdrain-fire");
                              } }
                            fprintf(stderr, "[bdrain] NOTE: past-wall territory -- releasing this stopper crosses the wedge; a downstream guest fault beyond here is PROGRESS, not a drain failure (postmortem v2: 0x23C786AD read).\n");
                        } else {
                            fprintf(stderr, "[bdrain] SUMMARY: SKIP ea=0x%08X (ring@ea=0x%08X not a parked JTS self-jump / out of RSX window) -- no write. fires=%u/%u\n",
                                    ea, ring, bd_fires, bd_cap);
                        }
                        fflush(stderr);
                    }
                }
            }
            /* s33 0x4C24 discriminator (STATUS ⚡ #1, always-on, low-volume):
             * at any >=5 s stopper park, LEVER ON OR OFF, say whether the
             * game's tag-0x7F journal holds this stopper's release entry.
             * PRESENT -> the consumer has a consume-gap (it applied dozens of
             * others, s33retA/B); ABSENT -> the producer never journaled it
             * (t1 wedged pre-append). Resamples every 10 s while parked. */
            { static uint32_t sj_ea = 0; static ULONGLONG sj_t = 0;
              if (parked_ms > 5000 && (sj_ea != ea || GetTickCount64() - sj_t > 10000)) {
                  sj_ea = ea; sj_t = GetTickCount64();
                  const uint32_t je = yz_gcm_stopper_release_entry(ea);
                  fprintf(stderr, "[stop-jrnl] parked %llums @io 0x%06X ea=0x%08X journal-entry=%s (0x%08X%s)\n",
                          (unsigned long long)parked_ms, get, ea, je ? "PRESENT" : "ABSENT", je,
                          je && g_yz_relentry_region == 2u ? " above-head" : "");
                  fflush(stderr);
                  /* Frontier diagnosis: one read-only snapshot of every live
                   * SPU/MFC context at an unjournaled five-second park. */
                  { static int edge_mode = -1;
                    static uint32_t edge_dumped_ea = 0;
                    static unsigned edge_dwell_ms = 5000u;
                    if (edge_mode < 0) {
                        const char* e = getenv("YZ_FRONTIER_EDGE");
                        edge_mode = (e && *e == '1') ? 1 : 0;
                        const char* d = getenv("YZ_FRONTIER_EDGE_DWELL_MS");
                        if (d) edge_dwell_ms = (unsigned)atoi(d);
                    }
                    if (edge_mode && edge_dumped_ea != ea && !je &&
                        parked_ms > edge_dwell_ms) {
                        edge_dumped_ea = ea;
                        yz_frontier_edge_dump(ea, get, put);
                    } }
                  /* Deferred ring dump: retain lifecycle events silently on
                   * normal slow frames, then print the matching producer tail
                   * only after this exact stopper has remained invariant long
                   * enough to be a genuine hard-stall candidate. */
                  { static uint32_t ring_dumped_ea = 0;
                    if (ring_dumped_ea != ea && parked_ms > 30000u &&
                        yz_a010_reltrace_on()) {
                        ring_dumped_ea = ea;
                        yz_a010_reltrace_dump(ea);
                    } }
                  /* s34 CONSUME-GAP HEXDUMP — fires ONCE at the 2nd park
                   * sample (>15s), anchored on the STABLE release entry (NOT
                   * the cursor). The consumer cursor has two measured steady
                   * shapes: LOCKED at one value past the entry (s34gapA:
                   * 0x41F1E300) or CYCLING a ~6-line window that CONTAINS the
                   * entry (s34frzA: sweeps [0x41F2EF80..0x41F2F280] forever,
                   * entry 0x41F2F200 inside it). A cursor-stable gate misses
                   * the cycling flavor entirely, so anchor on the entry and
                   * dump WIDE (6 lines back + entry line + 3 forward) to
                   * capture the whole active window in both. cur is printed so
                   * we see where in the cycle it was. Adds the live RING word
                   * (still jump-to-self?) + a [base,head) scan for EVERY
                   * tag-0x7F entry naming this stopper. Entry stride 0x20:
                   * word0=tag (0x7F release / 04,05,09,0D,10 patch-data /
                   * 0=hole), word1=target EA; consumer accepts 128B lines. */
                  static int sj_dumped = 0;
                  if (je && !sj_dumped && parked_ms > 15000) {
                      sj_dumped = 1;
                      const uint32_t S2    = vm_read32(g_yz_game_toc - 0x7410u);
                      const int Sok        = (S2 >= 0x10000u && S2 < 0xE0000000u);
                      const uint32_t jbase = Sok ? vm_read32(S2 + 0x08u) : 0;
                      const uint32_t jhead = Sok ? vm_read32(S2 + 0x00u) : 0;
                      const uint32_t cur   = g_yz_jrnl_cur_ea;
                      const uint32_t ringword = vm_read32(ea);
                      uint32_t lo = (je & ~127u) - 0x300u;          /* 6 lines back */
                      uint32_t hi = (je & ~127u) + 0x180u;          /* entry line + 3 forward */
                      if (jbase && lo < jbase) lo = jbase;
                      if (hi - lo > 0x800u) hi = lo + 0x800u;
                      fprintf(stderr, "[stop-jrnl] HEXDUMP [0x%08X..0x%08X) cursor=0x%08X entry=0x%08X base=0x%08X head=0x%08X ringword@0x%08X=0x%08X\n",
                              lo, hi, cur, je, jbase, jhead, ea, ringword);
                      for (uint32_t a = lo; a < hi; a += 0x20u) {
                          const char* mark = "    ";
                          if (jhead && a >= jhead) mark = "UNW>";
                          if (cur && (a & ~127u) == (cur & ~127u)) mark = "CUR>";
                          if (a == je) mark = "ENT>";
                          fprintf(stderr, "%s 0x%08X (%+5d): %08X %08X %08X %08X %08X %08X %08X %08X\n",
                                  mark, a, (int)(a - cur),
                                  vm_read32(a+0x00u), vm_read32(a+0x04u), vm_read32(a+0x08u), vm_read32(a+0x0Cu),
                                  vm_read32(a+0x10u), vm_read32(a+0x14u), vm_read32(a+0x18u), vm_read32(a+0x1Cu));
                      }
                      /* Wrap-aware (2026-08-06): scan the FULL ring
                       * [jbase..jend), not just [jbase..jhead) — after a
                       * head wrap, still-live entries sit ABOVE head and
                       * the old scan was blind to them (boot-62 decode). */
                      const uint32_t jend = Sok ? vm_read32(S2 + 0x0Cu) : 0;
                      const uint32_t jtop =
                          (jend > jbase && (jend - jbase) < 0x1000000u)
                              ? jend : jhead;
                      if (jbase && jtop > jbase && (jtop - jbase) < 0x1000000u) {
                          int n7f = 0;
                          fprintf(stderr, "[stop-jrnl] SCAN tag-0x7F entries with word1==0x%08X in [0x%08X..0x%08X) head=0x%08X:\n", ea, jbase, jtop, jhead);
                          for (uint32_t e = jbase; e < jtop; e += 0x20u) {
                              if (vm_read32(e) == 0x7Fu && vm_read32(e + 0x04u) == ea) {
                                  n7f++;
                                  if (n7f <= 8)
                                      fprintf(stderr, "    #%d @0x%08X (%s cursor by %d%s)\n",
                                              n7f, e, e < cur ? "behind" : "ahead of", (int)(e - cur),
                                              jhead && e >= jhead ? ", above-head" : "");
                              }
                          }
                          fprintf(stderr, "[stop-jrnl] SCAN total=%d\n", n7f);
                      }
                      fflush(stderr);
                  }
                  } }
            const int parked3s   = prel && (parked_ms > 3000);
            /* s25: fast tier now UNCONDITIONAL at fast_ms (witness dropped).
             * Measured chain: (a) rides s25ride4-6 ground at 3-5 s/flip
             * because t1 SPINS in the throttle (seq climbs, hop-frozen
             * witness never fires) yet makes no flush call
             * (stopper_drain_re.md Q1); (b) the loading-screen steady state
             * re-parks EVERY frame at the segment-recycle stopper (io
             * 0x200000, 0x258-byte frame batches) because a LATCHED release
             * is only ever applied by the SPU journal consumer (which we
             * lack) or this lever — so fast_ms is the frame-rate governor
             * (fps <= 1000/fast_ms), and RPCS3's consumer does this at
             * sub-ms. Safety was never the witness: the preconditions (GET
             * parked ON the stopper + the release IS the game's own journal
             * entry + PUT committed past it) carry it, and the 3 s tier
             * already fired on exactly those. Lever remains opt-in
             * (YZ_PARK_REL) until the real consumer story lands. park_seq/
             * park_tf kept for the apply log's diagnostics. */
            (void)park_tf;
            /* s28 ROOT FIX (ledger #64, the "1/6" early-boot stall = a LEVER
             * MISFIRE RACE): the fast tier during BOOT-START applies the
             * release before t1 finalizes the following segment — GET runs
             * into the A2000500 placeholder at io 0x1104D00 and t1 parks in
             * its usleep(30) GPU-progress throttle forever (measured: [t1-hb]
             * sc=141 r3=0x1E full-CPU; 7/7 boots separate stalled-vs-clean
             * purely by lever-fire order at the first stopper). Gate the fast
             * tier on the UPDATE LOOP having started (g_yz_updloop_started,
             * set at the first func_00D1E838 entry) — before that, only the
             * 3 s fallback fires, which boot-start timing survives (clean
             * boots' own lever fires were effectively that late). Kill-switch
             * YZ_FASTLEVER_EARLY restores the old behavior. */
            static int fle = -1;
            if (fle < 0) fle = getenv("YZ_FASTLEVER_EARLY") ? 1 : 0;
            const int fast_ok = fle || g_yz_updloop_started;
            const int parkedfast = prel && fast_ms && fast_ok && (parked_ms > fast_ms);
            /* The FIFO ring is 8x1MB = 0x800000 (GET/PUT wrap io 0x7xxxxx -> 0x0; the
             * iomap's 0x1B00000 is the whole io SPAN incl. off-ring buffers, NOT the
             * wrap period). Hard-coded so a future HLE _cellGcmInitBody can't leak the
             * wrong size into the wrap arithmetic. */
            const uint32_t ring  = 0x800000u;
            const uint32_t ahead = (put - get + ring) % ring;   /* PUT distance ahead of GET (ring-wrapped) */
            if (yz_a010_missing_release_try(ea, get, put)) {
                return finish(YZ_RSX_WAIT_ADVANCING);
            }
            if (journal_hle) {
                if (ahead != 0u && ahead < (ring >> 1) &&
                    yz_jrnl_hle_try(ea) == yz_jrnl_hle_park_result::applied) {
                    /* The ordered HLE applied every decoded patch first, then
                     * wrote the faithful jump-forward release word. Advance GET
                     * past the released stopper exactly like the lever does. */
                    vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, get + 4u);
                    return finish(YZ_RSX_WAIT_ADVANCING);
                }
                /* Never fall through to a release-only lever while the HLE owns
                 * the recovery decision. Unsupported entries intentionally stay
                 * parked so a partial journal can never reach RSX. */
                return finish(YZ_RSX_WAIT_SELF_STOPPER);
            }
            const uint32_t rel_entry = ((apply || parked3s || parkedfast) && ahead != 0u && ahead < (ring >> 1))
                                           ? yz_gcm_stopper_release_entry(ea) : 0u;
            if (rel_entry) {
                vm_write32(ea, 0x20000000u | ((get + 4u) & 0x1FFFFFFCu));   /* release: self-jump -> jump-forward +4 */
                vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, get + 4u);     /* GET advances into the committed body */
                /* Faithful consumption mark: GET consuming past this stopper
                 * proves everything journaled up to its release entry retired
                 * -- zero those tags (the game's GPU-progress ledger; see
                 * yz_jrnl_retire_through). */
                yz_jrnl_retire_through(rel_entry);
                /* s31 [park-line] (s31_journal_linefill.md §7): the applied
                 * entry's 128-byte line, slot tags only — discriminates "pure
                 * consumer-dispatch death" (line complete, nobody consumed)
                 * from "completion circle also binds" (line's later slots
                 * still unwritten at apply time). First 8 applies only. */
                { static int pl = 0;
                  if (pl < 8) { pl++;
                    const uint32_t line = rel_entry & ~0x7Fu;
                    fprintf(stderr, "[park-line] entry=0x%08X line=0x%08X tags: %08X %08X %08X %08X\n",
                            rel_entry, line,
                            vm_read32(line), vm_read32(line + 0x20u),
                            vm_read32(line + 0x40u), vm_read32(line + 0x60u));
                  } }
                /* s29 (ledger #65): the n<64 cap made a 900 s boot read as "64
                 * applies total" — a false lever exoneration. Keep the first 64
                 * verbose, then a census line every 256th so the steady-state
                 * fire rate stays measurable at any boot length. */
                { static unsigned long n = 0; n++;
                  if (n <= 64) {
                    fprintf(stderr, "[rsx] applied deferred release @io 0x%06X (PUT ahead 0x%X)%s parked=%llums t1seq=%ld fence0=0x%08X -> GET advances past stopper\n",
                            get, ahead,
                            parkedfast && !parked3s ? " [park-rel FAST]" :
                            parked3s ? " [park-rel 3s]" : "",
                            (unsigned long long)parked_ms, (long)g_yz_t1_sample_seq,
                            vm_read32(0x40C00000u));
                  } else if ((n & 255u) == 0u) {
                    fprintf(stderr, "[park-rel] census: %lu applies total (latest @io 0x%06X%s parked=%llums)\n",
                            n, get,
                            parkedfast && !parked3s ? " FAST" : parked3s ? " 3s" : "",
                            (unsigned long long)parked_ms);
                  }
                  /* s31 W2LIFE (ledger #71): the SPURS wid accounting in the
                   * lever-substitution regime -- every lever apply means the SPU
                   * journal consumer did NOT release this stopper. Sampled so a
                   * long boot can't exhaust the dump's 64-print cap. */
                  if (n == 1 || (n & 63u) == 0u) yz_w2life_dump("lever"); }
                return finish(YZ_RSX_WAIT_ADVANCING);
            }
            if (parked3s) {   /* parked past threshold but NO journal entry: say so once per EA
                               * (discriminates "deferred entry exists" from "immediate release
                               * never executed" -- decides lever vs segment-pin, per the RE map) */
                static uint32_t said = 0;
                if (said != ea) { said = ea;
                    fprintf(stderr, "[park-rel] parked >3s @io 0x%06X (PUT ahead 0x%X) but NO tag-0x7F journal entry matches\n",
                            get, ahead); fflush(stderr); }
            }
            /* not yet committed (no tag-0x7F entry, or PUT not past) -- spin in place */
            return finish(YZ_RSX_WAIT_SELF_STOPPER);
        }
        /* a010 missing generated-link patch.  A clean replay followed
         * 0x205E1BA0 directly into vertex-program payload (0x60405F80);
         * the first complete EDGE command prologue was 0xD0 bytes later at
         * 0x5E1C70.  This is the same missing-patch family as the stale outer
         * stopper, but it is visible only after following the inner link.
         * Repair only an immediate non-command target inside the committed
         * a010 window, and only to the exact measured prologue. */
        const int a010_bad_link =
            yz_a010_fifo_publication_repair_enabled();
        if (a010_root && a010_bad_link) {
            const uint32_t te = yz_rsx_io_to_ea(tgt);
            const uint32_t tw = te ? vm_read32(te) : 0u;
            const int target_flow =
                ((tw & 0xE0000003u) == 0x20000000u) ||
                ((tw & 3u) == 1u) || ((tw & 3u) == 2u) ||
                ((tw & 0xFFFF0003u) == 0x00020000u);
            const int target_method = (tw & 0xA0030003u) == 0u;
            if (te && !target_flow && !target_method) {
                const uint32_t resume =
                    yz_a010_find_pending_chain((tgt + 4u) & 0x7FFFFFu,
                                               put, 1);
                if (resume) {
                    const uint32_t repaired =
                        (cmd & 3u) == 1u
                            ? (resume | 1u)
                            : (0x20000000u | (resume & 0x1FFFFFFCu));
                    vm_write32(ea, repaired);
                    static unsigned long repairs = 0;
                    repairs++;
                    fprintf(stderr,
                            "[a010-bad-link] repaired n=%lu source=0x%06X "
                            "word=0x%08X target=0x%06X head=0x%08X "
                            "-> prologue=0x%06X PUT=0x%06X\n",
                            repairs, get, cmd, tgt, tw, resume, put);
                    fflush(stderr);
                    tgt = resume;
                }
            }
        }
        if (!yz_rsx_io_to_ea(tgt)) {
            const uint32_t pea = yz_rsx_io_to_ea(put);
            if (put != get && pea) {
                vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, put);
                g_fifo_ret = ~0u;
                return finish(YZ_RSX_WAIT_ADVANCING);
            }
            return finish(YZ_RSX_WAIT_BAD_FLOW);
        }
        if (yz_fifo_flowlog()) {
            fprintf(stderr, "[fifo-flow] JUMP io=0x%08X -> 0x%08X (word 0x%08X)\n", get, tgt, cmd);
            fflush(stderr);
        }
        if (a010_diag_root)
            a010_jump_n++;
        if (a010_diag_root && a010_flow_trace) {
            const uint32_t te = yz_rsx_io_to_ea(tgt);
            fprintf(stderr,
                    "[a010-flow] JUMP io=0x%08X ea=0x%08X -> io=0x%08X ea=0x%08X "
                    "head=%08X %08X %08X %08X\n",
                    get, ea, tgt, te,
                    te ? vm_read32(te + 0u) : 0u,
                    te ? vm_read32(te + 4u) : 0u,
                    te ? vm_read32(te + 8u) : 0u,
                    te ? vm_read32(te + 12u) : 0u);
            fflush(stderr);
        }
        g_fifo_last_flow_source = get;
        g_fifo_last_flow_word = cmd;
        g_fifo_last_flow_target = tgt;
        g_fifo_last_flow_kind = 1u;
        vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, tgt);
        return finish(YZ_RSX_WAIT_ADVANCING);
    }
    if ((cmd & 3u) == 2u) {                                          /* call */
        const uint32_t ctgt = cmd & 0x1FFFFFFCu;                     /* CALL offset mask */
        if (!yz_rsx_io_to_ea(ctgt)) {
            return finish(YZ_RSX_WAIT_BAD_FLOW);
        }
        if (g_fifo_ret != ~0u) {
            /* Nested CALL (s29, scratch/s29_terminal_park_re.md Q4a): RPCS3
             * checks fifo_ret_addr != RSX_CALL_STACK_EMPTY here and logs "CALL
             * found inside a subroutine" instead of silently overwriting the
             * pending return -- our one-level slot used to just get clobbered,
             * losing the outer return address with no diagnostic. */
            static unsigned long nc = 0; nc++;
            if (nc <= 64 || (nc & 255u) == 0u)
                fprintf(stderr, "[rsx] CALL inside subroutine at io=0x%08X: live return 0x%08X "
                        "would be clobbered by new return 0x%08X (n=%lu)\n",
                        get, g_fifo_ret, get + 4u, nc);
            if (yz_fifo_recover_ret_enabled()) {
                /* Faithful: don't execute this CALL (don't touch g_fifo_ret or
                 * GET) -- the outer return address survives. Retry/escalate
                 * exactly like the RETURN-without-CALL path below. */
                yz_fifo_ret_recover(get, "CALL-inside-subroutine");
                return finish(YZ_RSX_WAIT_BAD_FLOW);
            }
            /* flag off: preserve the pre-existing default (clobber-and-proceed),
             * now with the loud log above instead of silence. */
        }
        if (yz_fifo_flowlog()) {
            fprintf(stderr, "[fifo-flow] CALL io=0x%08X -> 0x%08X (ret=0x%08X)\n", get, ctgt, get + 4u);
            fflush(stderr);
        }
        if (a010_diag_root)
            a010_call_n++;
        if (a010_diag_root && a010_flow_trace) {
            const uint32_t ce = yz_rsx_io_to_ea(ctgt);
            fprintf(stderr,
                    "[a010-flow] CALL io=0x%08X ea=0x%08X -> io=0x%08X ea=0x%08X "
                    "ret=0x%08X head=%08X %08X %08X %08X\n",
                    get, ea, ctgt, ce, get + 4u,
                    ce ? vm_read32(ce + 0u) : 0u,
                    ce ? vm_read32(ce + 4u) : 0u,
                    ce ? vm_read32(ce + 8u) : 0u,
                    ce ? vm_read32(ce + 12u) : 0u);
            fflush(stderr);
        }
        g_fifo_last_flow_source = get;
        g_fifo_last_flow_word = cmd;
        g_fifo_last_flow_target = ctgt;
        g_fifo_last_flow_kind = 2u;
        g_fifo_ret = get + 4u;                /* one-level return */
        vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, ctgt);
        return finish(YZ_RSX_WAIT_ADVANCING);
    }
    if ((cmd & 0xFFFF0003u) == 0x00020000u) {                        /* return */
        if (g_fifo_ret != ~0u) {
            if (yz_fifo_flowlog()) {
                fprintf(stderr, "[fifo-flow] RET  io=0x%08X -> 0x%08X\n", get, g_fifo_ret);
                fflush(stderr);
            }
            if (a010_diag_root)
                a010_ret_n++;
            if (a010_diag_root && a010_flow_trace) {
                fprintf(stderr,
                        "[a010-flow] RET io=0x%08X ea=0x%08X -> io=0x%08X ea=0x%08X\n",
                        get, ea, g_fifo_ret, yz_rsx_io_to_ea(g_fifo_ret));
                fflush(stderr);
            }
            g_fifo_last_flow_source = get;
            g_fifo_last_flow_word = cmd;
            g_fifo_last_flow_target = g_fifo_ret;
            g_fifo_last_flow_kind = 3u;
            vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, g_fifo_ret);
            g_fifo_ret = ~0u;
            return finish(YZ_RSX_WAIT_ADVANCING);
        }
        /* s29 terminal park (scratch/s29_terminal_park_re.md): RPCS3 logs
         * "RET found without corresponding CALL" and calls recover_fifo() here
         * instead of idling; our one-ever warning below is unchanged (still the
         * MEASURED s28m10/s28m4 receipt), but YZ_FIFO_RECOVER_RET now adds the
         * bounded retry/escalation analog instead of a completely silent
         * forever-idle after the first print. */
        static int warned = 0;
        if (!warned) { warned = 1;
            fprintf(stderr, "[rsx] RETURN without CALL at io=0x%08X; idling\n", get); }
        if (yz_fifo_recover_ret_enabled())
            yz_fifo_ret_recover(get, "RETURN-without-CALL");
        return finish(YZ_RSX_WAIT_BAD_FLOW);
    }

    /* ---- method packet ---- */
    /* RPCS3 RSX_METHOD_NON_METHOD_CMD_MASK 0xA0030003: after the flow-control
     * tests, a finalised method has those bits clear. If set, GET reached the
     * end of finalised commands in this segment -- wait, don't parse data. */
    if (cmd & 0xA0030003u) {
        transition(YZ_RSX_WAIT_UNFINALIZED_HOLE);
        /* s33 FIX (user-confirmed; scratch/s33_fifo_return_audit.md): the s28
         * default here modeled RPCS3's recover_fifo as "skip GET forward one
         * word" for illegal (cmd&3==3) headers. That MIS-MODELED the oracle —
         * RSXThread re-reads the SAME GET (an illegal word is content the
         * producer hasn't finalised yet; recover_fifo aborts in-flight state
         * and RETRIES, it does not advance) — and the skip demonstrably
         * STRANDED GET: s32resur1 walked 22 skips through the unwritten
         * next-phase segment at io 0x8000xx then teleported via a garbage
         * jump to io 0x200BF4 with zero further trace; s28m10's 650 s
         * RETURN-without-CALL park was the same walk ending on a foreign
         * RETURN. Default now = faithful re-read of the same GET forever,
         * with an UNCAPPED time-sampled park print (the old print capped at
         * 24 — terminal parks went invisible, ledger #65 class).
         * YZ_FIFO_SKIP4=1 restores the legacy skip for A/B. */
        static int skip4 = -1;
        if (skip4 < 0) { skip4 = getenv("YZ_FIFO_SKIP4") ? 1 : 0;
            fprintf(stderr, "[fifo-rec] ARMED (%s)\n",
                    skip4 ? "LEGACY skip-4 recovery, YZ_FIFO_SKIP4"
                          : "s33 faithful re-read; illegal words wait for the producer");
            fflush(stderr); }
        static uint32_t stuck_get = 0xFFFFFFFFu;
        static ULONGLONG stuck_t0 = 0, said_t = 0;
        static unsigned long nnc = 0;
        const ULONGLONG now = GetTickCount64();
        if (get != stuck_get) {
            stuck_get = get; stuck_t0 = now; said_t = now; nnc++;
            fprintf(stderr, "[rsx] non-command 0x%08X at io=0x%X (PUT=0x%X) -- segment not "
                    "finalised; waiting for producer (n=%lu)\n", cmd, get, put, nnc);
            fprintf(stderr,
                    "[fifo-arrival] kind=%u source=0x%08X word=0x%08X "
                    "target=0x%08X live-ret=0x%08X\n",
                    g_fifo_last_flow_kind, g_fifo_last_flow_source,
                    g_fifo_last_flow_word, g_fifo_last_flow_target,
                    g_fifo_ret);
            fflush(stderr);
            /* s35 hole-structure dump (env YZ_FIFO_HOLE_DUMP): on first hit of a
             * given non-command park, hexdump [get, get+0x120) so the
             * unfinalised-hole layout AND where real commands resume can be read
             * directly -- the discriminator for "unfinalised Edge hole" (a small
             * data run then valid cmds) vs "GET desynced into a data segment"
             * (all data to PUT). Read-only, one dump per distinct park. */
            static int holedump = -1;
            if (holedump < 0) holedump = getenv("YZ_FIFO_HOLE_DUMP") ? 1 : 0;
            if (holedump) {
                fprintf(stderr, "[fifo-holedump] hole @io 0x%X (PUT=0x%X) g_fifo_ret=0x%08X:\n", get, put, g_fifo_ret);
                for (uint32_t off = 0; off < 0x120u; off += 0x10u) {
                    const uint32_t io0 = get + off;
                    const uint32_t e0 = yz_rsx_io_to_ea(io0);
                    if (!e0) { fprintf(stderr, "  io 0x%X: <off-ring>\n", io0); break; }
                    fprintf(stderr, "  io 0x%X: %08X %08X %08X %08X\n", io0,
                            vm_read32(e0), vm_read32(e0+4u), vm_read32(e0+8u), vm_read32(e0+12u));
                }
                fflush(stderr);
            }
        } else if (now - said_t >= 2000) {
            said_t = now;
            fprintf(stderr, "[rsx] still parked on non-command 0x%08X at io=0x%X (%llus; PUT=0x%X)\n",
                    cmd, get, (unsigned long long)((now - stuck_t0) / 1000u), put);
            fflush(stderr);
        }
        /* s35 STALE-HOLE SKIP (env YZ_FIFO_SKIP_STALE, diagnostic, default OFF):
         * when GET parks on an illegal header (cmd&3==3) for >500 ms with PUT
         * ahead, the SPU (gs_task/EDGE) never finalised this command-buffer hole
         * (the s34 consume-gap: our journal consumer scans but never writes the
         * real commands into the reserved hole). The faithful re-read then waits
         * forever for a producer that has already moved on (PUT past it). This
         * band-aid scans forward from GET for the next VALID command header/jump
         * and resumes there, dropping only the unfinalised region -- which is 3D
         * Edge geometry we cannot render anyway; the 2D loading UI + the CRI
         * preload (both frame-pump-driven) keep flowing. NOT faithful: it drops
         * a draw the same way the park-release lever synthesises a release. A
         * milestone bridge to prove the loading-screen phase is reachable while
         * the SPU-finalisation root is fixed. Kill-switch: the env flag. */
        static int skipstale = -1;
        if (skipstale < 0) { skipstale = getenv("YZ_FIFO_SKIP_STALE") ? 1 : 0;
            if (skipstale) { fprintf(stderr, "[fifo-skipstale] ARMED (YZ_FIFO_SKIP_STALE): resume past unfinalised holes after 500ms park\n"); fflush(stderr); } }
        if (skipstale && (cmd & 3u) == 3u && now - stuck_t0 >= 500) {
            uint32_t scan = get + 4u, found = 0;
            while (scan < put) {
                const uint32_t sea = yz_rsx_io_to_ea(scan);
                if (!sea) break;
                const uint32_t w = vm_read32(sea);
                const int is_jump   = ((w & 0xE0000003u) == 0x20000000u) || ((w & 3u) == 1u);
                const int is_method = (w & 0xA0030003u) == 0u && (w & 0x3FFFCu) != 0u;
                if (is_jump || is_method) { found = scan; break; }
                scan += 4u;
            }
            static unsigned long ssn = 0; ssn++;
            if (found) {
                fprintf(stderr, "[fifo-skipstale] n=%lu skipped hole io[0x%X..0x%X) -> resume at cmd 0x%08X @io 0x%X (PUT=0x%X)\n",
                        ssn, get, found, vm_read32(yz_rsx_io_to_ea(found)), found, put);
                vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, found);
            } else {
                fprintf(stderr, "[fifo-skipstale] n=%lu no valid cmd in io[0x%X,0x%X) -> resync GET to PUT\n",
                        ssn, get, put);
                vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, put);
            }
            g_fifo_ret = ~0u;
            fflush(stderr);
            return finish(YZ_RSX_WAIT_ADVANCING);
        }
        if (skip4 && (cmd & 3u) == 3u && now - stuck_t0 >= 30) {
            static unsigned long recn = 0; recn++;
            fprintf(stderr, "[fifo-rec] n=%lu ILLEGAL header 0x%08X at io=0x%X -- LEGACY skip 4\n",
                    recn, cmd, get);
            fflush(stderr);
            vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, get + 4u);
            return finish(YZ_RSX_WAIT_ADVANCING);
        }
        return finish(YZ_RSX_WAIT_UNFINALIZED_HOLE);
    }

    const uint32_t count   = (cmd >> 18) & 0x7FFu;
    const uint32_t noninc  = cmd & 0x40000000u;
    const uint32_t method  = cmd & 0x3FFFCu;
    const uint32_t pkt_end = get + 4u + count * 4u;

    const int a010_count_packet =
        a010_diag_root && get != a010_last_counted_get;
    if (a010_count_packet) {
        a010_last_counted_get = get;
        a010_packet_n++;
        a010_arg_n += count;
        if (get < a010_src_min) a010_src_min = get;
        if (pkt_end > a010_src_max) a010_src_max = pkt_end;
    }

    /* The whole packet (header + count args) must be committed before we
     * dispatch (RPCS3 inc_get waits for PUT to cover each arg; PUT points one
     * past the last committed word). Linear within a segment; wrap is handled
     * by the producer's JUMP. If PUT is mid-packet, wait. */
    if (count && get < put && pkt_end > put) {
        return finish(YZ_RSX_WAIT_PARTIAL_PACKET);
    }

    int stalled = 0;
    for (uint32_t i = 0; i < count && !stalled; i++) {
        const uint32_t op_ea = yz_rsx_io_to_ea(get + 4u + i * 4u);
        if (!op_ea) break;
        const uint32_t eff = noninc ? method : method + i * 4u;
        const uint32_t val = vm_read32(op_ea);
        const uint32_t canonical = eff & 0x1FFCu;
        const uint32_t subchannel = (eff >> 13) & 7u;
        if (a010_diag_root && canonical == 0x1EFCu)
            a010_const_load[subchannel] = val;
        else if (a010_diag_root && a010_const_trace &&
                 canonical >= 0x1F00u && canonical < 0x2000u) {
            const uint32_t word = (canonical - 0x1F00u) >> 2;
            const uint32_t slot =
                a010_const_load[subchannel] + (word >> 2);
            const int is_nan =
                (val & 0x7F800000u) == 0x7F800000u &&
                (val & 0x007FFFFFu) != 0;
            if (slot >= 108u && slot <= 111u && is_nan &&
                a010_const_trace_n < 128) {
                a010_const_trace_n++;
                fprintf(stderr,
                        "[a010-const] n=%lu io=0x%08X ea=0x%08X "
                        "hdr=0x%08X count=%u i=%u noninc=%u "
                        "raw=0x%05X sub=%u load=%u slot=%u lane=%u "
                        "val=0x%08X ret=0x%08X\n",
                        a010_const_trace_n, get + 4u + i * 4u, op_ea,
                        cmd, count, i, noninc ? 1u : 0u,
                        eff, subchannel, a010_const_load[subchannel],
                        slot, word & 3u, val, g_fifo_ret);
                if (a010_const_trace_n == 1) {
                    fprintf(stderr,
                            "[a010-const] first-NaN packet io=0x%08X "
                            "ea=0x%08X words:",
                            get, ea);
                    for (uint32_t j = 0; j <= count && j < 32u; j++)
                        fprintf(stderr, " %08X", vm_read32(ea + j * 4u));
                    fputc('\n', stderr);
                    fflush(stderr);
                }
            }
        }
        if (a010_count_packet) {
            a010_sub_args[(eff >> 13) & 7u]++;
            if (canonical == 0x1808u) {
                if (val) {
                    if (a010_draw_source_trace &&
                        !a010_draw_source_written &&
                        a010_draw_source_n <
                            sizeof(a010_draw_sources) /
                                sizeof(a010_draw_sources[0])) {
                        yz_a010_draw_source* source =
                            &a010_draw_sources[a010_draw_source_n];
                        source->index = a010_draw_source_n;
                        source->io = get;
                        source->ea = ea;
                        source->ret_io = g_fifo_ret;
                        source->header = cmd;
                        source->primitive = val;
                        ++a010_draw_source_n;
                    }
                    a010_begin_n++;
                } else {
                    a010_end_n++;
                }
            } else if (canonical == 0x1814u) {
                a010_array_n++;
            } else if (canonical == 0x1820u) {
                a010_index_n++;
            }
            if (canonical >= 0x0B80u && canonical < 0x0C00u)
                a010_vp_n++;
            if (canonical >= 0x1F00u && canonical < 0x2000u)
                a010_const_n++;
        }
        /* The shipped a010 capture has no render-target clip dimension above
         * 1280x1024.  A recovered chain has emitted 0x4044 as the vertical
         * extent, which is the high half of a guest pointer rather than an RSX
         * dimension.  Record its exact source packet and keep the last valid
         * clip state; allowing it through makes D3D12 reject the surface and
         * hides the command-link failure behind a host crash. */
        if (a010_root &&
            ((canonical == 0x0200u && (val >> 16) > 8192u) ||
             (canonical == 0x0204u && (val >> 16) > 8192u))) {
            static unsigned long invalid_clip = 0;
            invalid_clip++;
            fprintf(stderr,
                    "[a010-invalid-clip] n=%lu packet-io=0x%06X "
                    "arg-io=0x%06X header=0x%08X count=%u i=%u "
                    "eff=0x%05X val=0x%08X extent=%u ret=0x%08X\n",
                    invalid_clip, get, get + 4u + i * 4u,
                    cmd, count, i, eff, val, val >> 16, g_fifo_ret);
            if (invalid_clip <= 4u) {
                fprintf(stderr, "[a010-invalid-clip] packet words:");
                for (uint32_t j = 0; j <= count && j < 32u; j++)
                    fprintf(stderr, " %08X", vm_read32(ea + j * 4u));
                fputc('\n', stderr);
            }
            fflush(stderr);
            continue;
        }
        if (a010_diag_root && (eff == 0xE920u || eff == 0xE924u)) {
            if (a010_draw_source_trace &&
                !a010_draw_source_written &&
                a010_draw_source_n >= 400u) {
                const char* path = getenv("YZ_A010_DRAW_SOURCE");
                FILE* source_file =
                    path && path[0] ? fopen(path, "w") : nullptr;
                if (source_file) {
                    fprintf(source_file,
                            "draw,io,ea,ret_io,ret_ea,header,primitive\n");
                    for (uint32_t source_i = 0;
                         source_i < a010_draw_source_n; ++source_i) {
                        const yz_a010_draw_source* source =
                            &a010_draw_sources[source_i];
                        fprintf(source_file,
                                "%u,0x%08X,0x%08X,0x%08X,0x%08X,"
                                "0x%08X,%u\n",
                                source->index, source->io, source->ea,
                                source->ret_io,
                                source->ret_io == ~0u
                                    ? 0u
                                    : yz_rsx_io_to_ea(source->ret_io),
                                source->header, source->primitive);
                    }
                    fclose(source_file);
                    fprintf(stderr,
                            "[a010-draw-source] wrote %u draw origins to %s\n",
                            a010_draw_source_n, path);
                } else {
                    fprintf(stderr,
                            "[a010-draw-source] failed to open %s\n",
                            path ? path : "<empty>");
                }
                fflush(stderr);
                a010_draw_source_written = 1;
            }
            const LONG prod_puts =
                InterlockedExchange(&g_yz_a010_spu_puts, 0);
            const LONG prod_put_bytes =
                InterlockedExchange(&g_yz_a010_spu_put_bytes, 0);
            const LONG prod_groups =
                InterlockedExchange(&g_yz_a010_spu_groups, 0);
            const LONG prod_group_bytes =
                InterlockedExchange(&g_yz_a010_spu_group_bytes, 0);
            const LONG prod_headers =
                InterlockedExchange(&g_yz_a010_spu_headers, 0);
            const LONG prod_args =
                InterlockedExchange(&g_yz_a010_spu_args, 0);
            const LONG prod_begin =
                InterlockedExchange(&g_yz_a010_spu_begin, 0);
            const LONG prod_end =
                InterlockedExchange(&g_yz_a010_spu_end, 0);
            const LONG prod_array =
                InterlockedExchange(&g_yz_a010_spu_array, 0);
            const LONG prod_index =
                InterlockedExchange(&g_yz_a010_spu_index, 0);
            const LONG prod_vp =
                InterlockedExchange(&g_yz_a010_spu_vp, 0);
            const LONG prod_const =
                InterlockedExchange(&g_yz_a010_spu_const, 0);
            const LONG prod_unparsed =
                InterlockedExchange(&g_yz_a010_spu_unparsed, 0);
            const LONG prod_ppucmd =
                InterlockedExchange(&g_yz_a010_ppucmd_headers, 0);
            const LONG prod_spucmd =
                InterlockedExchange(&g_yz_a010_spucmd_headers, 0);
            fprintf(stderr,
                    "[a010-fifo] FLIP head=%u buf=%u packets=%lu args=%lu "
                    "flow(j/c/r)=%lu/%lu/%lu io-span=0x%08X..0x%08X "
                    "draw(be/end/a/i)=%lu/%lu/%lu/%lu upload(vp/c)=%lu/%lu "
                    "subargs=%lu/%lu/%lu/%lu/%lu/%lu/%lu/%lu "
                    "GET=0x%08X PUT=0x%08X\n",
                    (eff - 0xE920u) >> 2, val,
                    a010_packet_n, a010_arg_n,
                    a010_jump_n, a010_call_n, a010_ret_n,
                    a010_src_min == 0xFFFFFFFFu ? 0u : a010_src_min,
                    a010_src_max,
                    a010_begin_n, a010_end_n, a010_array_n, a010_index_n,
                    a010_vp_n, a010_const_n,
                    a010_sub_args[0], a010_sub_args[1],
                    a010_sub_args[2], a010_sub_args[3],
                    a010_sub_args[4], a010_sub_args[5],
                    a010_sub_args[6], a010_sub_args[7],
                    get, put);
            fprintf(stderr,
                    "[a010-prod] img0 puts=%ld bytes=%ld "
                    "groups=%ld group_bytes=%ld headers=%ld args=%ld "
                    "draw(be/end/a/i)=%ld/%ld/%ld/%ld "
                    "upload(vp/c)=%ld/%ld unparsed=%ld "
                    "draw-writers(ppu/spu)=%ld/%ld\n",
                    prod_puts, prod_put_bytes,
                    prod_groups, prod_group_bytes, prod_headers, prod_args,
                    prod_begin, prod_end, prod_array, prod_index,
                    prod_vp, prod_const, prod_unparsed,
                    prod_ppucmd, prod_spucmd);
            yz_a010_auth_probe_poll();
            fflush(stderr);
            a010_packet_n = a010_arg_n = 0;
            a010_jump_n = a010_call_n = a010_ret_n = 0;
            memset(a010_sub_args, 0, sizeof(a010_sub_args));
            a010_begin_n = a010_end_n = 0;
            a010_array_n = a010_index_n = 0;
            a010_vp_n = a010_const_n = 0;
            a010_last_counted_get = 0xFFFFFFFFu;
            a010_src_min = 0xFFFFFFFFu;
            a010_src_max = 0;
            if (!a010_draw_source_written)
                a010_draw_source_n = 0;
        }
        if (eff >= 0xE940u && eff <= 0xE95Cu)
            rsx_live_draw_set_fifo_position(get, put);
        const int native_method =
            ((eff == 0x1808u && val == 0u) || eff == 0x1D94u ||
             eff == 0x2328u || eff == 0xC40Cu || eff == 0x0110u ||
             eff == 0x1D70u || eff == 0x1D74u || eff == 0x17C8u ||
             eff == 0x1800u) ?
            yz_nr_vertical_try_method(eff, val, yz_rsx_io_to_ea(get)) : 0;
        if (!native_method)
            yz_nr_vertical_prepare_legacy_method(eff, val);
        const int legacy_graphics_suppressed = !native_method &&
            rsx_live_draw_guest_graphics_suppressed();
        stalled = native_method ? 0 :
            yz_rsx_method(eff, val);   /* 1 => semaphore ACQUIRE not satisfied */
        if (!stalled && !native_method) {
            if (g_yz_nr_shadow_enabled)
                rsx_nr_intercept_shadow_method(&g_yz_nr_shadow, eff, val);
            yz_nr_vertical_observe_method(
                eff, val, yz_rsx_io_to_ea(get),
                legacy_graphics_suppressed);
        }
        if (g_yz_fe0_timeline_enabled && eff == 0x068u) {
            const uint32_t sem_addr =
                yz_rsx_sem_addr(yz_rsx_sem_dma_406e,
                                yz_rsx_sem_off_406e);
            if (sem_addr == 0x10200FE0u) {
                yz_fe0_timeline_rsx_acquire(
                    yz_rsx_sem_dma_406e, yz_rsx_sem_off_406e, sem_addr,
                    val, vm_read32(sem_addr), stalled);
            }
        }
        if constexpr (WaitClassify) {
            ++wait_dispatched_methods;
            if (eff == 0x068u) {
                const uint32_t sem_addr =
                    yz_rsx_sem_addr(yz_rsx_sem_dma_406e,
                                    yz_rsx_sem_off_406e);
                const yz_rsx_semaphore_wait semaphore = {
                    yz_rsx_sem_dma_406e, yz_rsx_sem_off_406e, sem_addr,
                    val, sem_addr ? vm_read32(sem_addr) : 0u
                };
                yz_rsx_wait_classifier_semaphore_attempt(
                    &semaphore, stalled);
            }
        }
    }
    if (stalled) {
        /* Leave GET on this packet header and retry (RPCS3: GET un-advanced on
         * an unsatisfied acquire) so the same method re-issues when ready. */
        return finish(YZ_RSX_WAIT_SEMAPHORE);
    }
    vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, pkt_end);

    /* s37 fix (YZ_FLIP_ON_CONSUMER): retire any flip armed by the method(s)
     * just dispatched (e.g. GCM_FLIP_HEAD 0xE920, or the semaphore-release
     * arm heuristic, both inside yz_rsx_method above) right here on the
     * consumer thread, still under g_rsx_fifo_lock -- GET has now advanced
     * past the flip, matching RPCS3's in-FIFO-order handle_emu_flip. Retire
     * EXACTLY ONCE per arm (InterlockedExchange consumes the pending flag);
     * yz_rsx_vblank_tick's legacy retire is disabled whenever this flag is
     * on (see its "!yz_flip_on_consumer() &&" guard), so there is no
     * double-present (s23 caveat, scratch/s37_render_ordering.md 3). */
    if (yz_flip_on_consumer()) {
        uint64_t flip_ev = 0;
        for (int h = 0; h < 2; h++) {          /* PS3 has 2 active heads */
            if (!InterlockedExchange(&g_rsx_flip_pending[h], 0)) continue;
            uint32_t ha  = yz_rsx_head_addr((uint32_t)h);
            uint32_t buf = vm_read32(ha + 0x14);           /* lastQueuedBufferId */
            uint64_t t   = (uint64_t)GetTickCount() * 1000;
            { static int n = 0; if (n < 12) { n++;
                fprintf(stderr, "[flip-consumer] FLIP COMPLETE head=%d buf=%u -> clear label@0x10200010\n",
                        h, buf); } }
            if (yz_ft_on())
                yz_ft("CONSUMER-RETIRE head=%d buf=%u label-before=0x%08X",
                      h, buf, vm_read32(RSX_REPORTS + 0x10));
            yz_rsx_present(buf);
            vm_write32(ha + 0x10, buf);                    /* flipBufferId */
            vm_write32(ha + 0x08, vm_read32(ha + 0x08) | 0x80000000u); /* flip done */
            vm_write64(ha + 0x00, t);                      /* lastFlipTime */
            vm_write64(RSX_REPORTS + 0x10, 0);              /* flip sema (u128) = 0 */
            vm_write64(RSX_REPORTS + 0x18, 0);
            if (yz_ft_on()) yz_ft("CONSUMER-CLEAR label=0 head=%d", h);
            /* Bump the render-throttle fence exactly once per retired flip,
             * ordered after present+label-clear (same rule as the vblank
             * path's comment at the fence write below). */
            vm_write32(0x40C00000u, vm_read32(0x40C00000u) + 1u);
            flip_ev |= (uint64_t)(0x8u << 1);              /* SYS_RSX_EVENT_FLIP_BASE<<1 */
        }
        if (flip_ev && g_rsx_event_port) {
            uint32_t handlers = vm_read32(RSX_DRIVER_INFO + 0x12C0);
            static int uof = -1; if (uof < 0) uof = getenv("YZ_UCMD_ON_FLIP") ? 1 : 0;
            uint64_t fmask = uof ? (uint64_t)handlers : ((uint64_t)handlers & 0x7Full);
            if (fmask) {
                /* RSX causes are level/coalesced notifications.  A momentarily
                 * full event queue must not discard the flip edge: route it
                 * through the shared pending-mask retry path used by queue and
                 * user-command events. */
                int64_t r = yz_rsx_ev_send(fmask);
                static int n = 0; if (n < 8) { n++;
                    fprintf(stderr, "[flip-consumer] flip event ev=0x%llX -> send=%lld\n",
                            (unsigned long long)fmask, (long long)r); }
            }
        }
    }

    return finish(YZ_RSX_WAIT_ADVANCING);
}

static yz_rsx_wait_category yz_rsx_fifo_step(void)
{
    return g_yz_rsx_wait_classifier_enabled
        ? yz_rsx_fifo_step_impl<true>()
        : yz_rsx_fifo_step_impl<false>();
}

/* Drain the FIFO inline until it stalls/drains (bounded). Called on the PRODUCER
 * thread from the PUT-flush hook (vm_write32 -> yz_rsx_inline_on_put) and from the
 * reserve usleep wait (sys_timer.c via g_yz_usleep_pump) -- the "continuous,
 * synchronous, NOT async" RSX experiment (YZ_RSX_INLINE). */
extern "C" void yz_rsx_fifo_pump(void)
{
    if (!g_rsx_ctx_ready) return;
    int budget = 4096, steps = 0;
    while (budget-- > 0 &&
           yz_rsx_fifo_step() == YZ_RSX_WAIT_ADVANCING) { steps++; }
    /* Lightweight liveness: total pump calls + total advanced steps, every 8192 calls. */
    static long calls = 0, total = 0; total += steps;
    if ((++calls & 0x1FFFu) == 1u)
        fprintf(stderr, "[rsx-inline] pump calls=%ld total_steps=%ld (this=%d) GET=0x%08X PUT=0x%08X\n",
                calls, total, steps,
                vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_GET), vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT));
}

/* PUT-flush hook (vm_write32 on a write to the PUT register 0x10000040). In inline
 * mode, drain GET up to the just-flushed PUT on the producer's own thread, so GET
 * tracks PUT and the producer cannot lap the ring between placing and releasing a
 * stopper. Env-gated; default boot (async consumer) is unchanged. */
extern "C" void yz_rsx_inline_on_put(void)
{
    static int on = -1; if (on < 0) on = getenv("YZ_RSX_INLINE") ? 1 : 0;
    if (on) yz_rsx_fifo_pump();
}

/* Registered into sys_timer.c so the reserve's sub-ms usleep wait pumps the FIFO
 * (GET advances while the producer waits for it -- the faithful "GPU runs while
 * the CPU waits" coupling). Set only in inline mode. */
extern "C" { void (*g_yz_usleep_pump)(void) = 0; }

static DWORD WINAPI yz_rsx_consumer(LPVOID)
{
    yz_thread_adopt_host("rsx-consumer");
    yz_rsx_fifo_lock_ensure();
    fprintf(stderr,
            "[rsx] FIFO consumer up: faithful (RPCS3 run_FIFO model) "
            "host_tid=%lu\n",
            GetCurrentThreadId());
    /* s24 idle heartbeat: wall shape #2 is the consumer idling for minutes with
     * PUT far ahead and NO stopper park (the lever legitimately silent) — and
     * every return-0 path in yz_rsx_fifo_step is print-silent, so the parked
     * state was invisible (scratch/s24ride.err: GET=0x7074 PUT=0x18EF8 wedged,
     * cause unreadable). Every ~10 s of continuous non-advance, print GET/PUT
     * + the raw words at GET so the blocking command is in the log. */
    ULONGLONG idle_t0 = 0; uint32_t idle_get = ~0u;
    for (;;) {
        if (!g_rsx_ctx_ready) { SwitchToThread(); continue; }
        const yz_rsx_wait_category result = yz_rsx_fifo_step();
        if (result == YZ_RSX_WAIT_ADVANCING) {
            idle_t0 = 0;
            continue;
        }
        const uint32_t g = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_GET);
        const ULONGLONG now = GetTickCount64();
        if (g != idle_get || !idle_t0) { idle_get = g; idle_t0 = now; }
        else if (now - idle_t0 >= 10000) {
            idle_t0 = now;
            const uint32_t p  = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT);
            const uint32_t ea = yz_rsx_io_to_ea(g);
            const uint32_t sem_addr =
                yz_rsx_sem_addr(yz_rsx_sem_dma_406e, yz_rsx_sem_off_406e);
            const uint32_t user_cmd =
                vm_read32(RSX_DRIVER_INFO + 0x12CCu);
            const uint32_t handlers =
                vm_read32(RSX_DRIVER_INFO + 0x12C0u);
            const uint32_t handler_arg =
                (uint32_t)_InterlockedCompareExchange(
                    &g_yz_ucmd_handler_arg, 0, 0);
            const uint32_t handler_completed =
                (uint32_t)_InterlockedCompareExchange(
                    &g_yz_ucmd_handler_completed, 0, 0);
            const uint64_t pending_events =
                (uint64_t)_InterlockedCompareExchange64(
                    &g_rsx_ev_pending, 0, 0);
            fprintf(stderr, "[rsx-idle] 10s no-advance GET=0x%08X PUT=0x%08X "
                    "ea=0x%08X words=%08X %08X %08X %08X "
                    "sem406e{dma=%08X off=%08X addr=%08X have=%08X} "
                    "ucmd{latest=%08X entered=%08X completed=%08X "
                    "handlers=%08X "
                    "pending=%016llX}\n",
                    g, p, ea,
                    ea ? vm_read32(ea) : 0, ea ? vm_read32(ea + 4) : 0,
                    ea ? vm_read32(ea + 8) : 0, ea ? vm_read32(ea + 12) : 0,
                    yz_rsx_sem_dma_406e, yz_rsx_sem_off_406e, sem_addr,
                    sem_addr ? vm_read32(sem_addr) : 0,
                    user_cmd, handler_arg, handler_completed, handlers,
                    (unsigned long long)pending_events);
            fflush(stderr);
        }
        SwitchToThread();
    }
    return 0;
}

extern "C" int64_t yz_sys_rsx_context_allocate(ppu_context*);   /* defined below */
extern "C" int32_t cellGcmInit(uint32_t cmd_size, uint32_t io_size,
                                uint32_t io_address);
extern "C" uint32_t cellGcmGetDefaultSegmentWordSize(void);
extern "C" void cellGcmTickVBlank(void);

/* SDK-internal cellGcmSys context helper. */
static constexpr uint64_t g_yz_gcm_system_mode = 0x820u;
static uint32_t g_yz_gcm_cmd_size;
static SRWLOCK g_yz_gcm_callback_lock = SRWLOCK_INIT;
static volatile LONG g_yz_gcm_callback_active;

static int yz_gcm_callback_lock_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        /* Sony's default FIFO callback is not protected by a process-wide
         * host lock.  The old lock was held across the callback's unbounded
         * GET wait, imposing host-only cross-callback serialization on a
         * guest path that has no such primitive.  Follow Sony by default;
         * YZ_GCM_CB_TRACE is the overlap witness and YZ_GCM_CB_LOCK retains
         * the legacy posture for timing/regression A/B. */
        enabled = getenv("YZ_GCM_CB_LOCK") ? 1 : 0;
        fprintf(stderr, "[gcm] callback host serialization: %s\n",
                enabled ? "ON (legacy YZ_GCM_CB_LOCK)" : "OFF (Sony behavior)");
        fflush(stderr);
    }
    return enabled;
}

static int yz_gcm_callback_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("YZ_GCM_CB_TRACE") ? 1 : 0;
    return enabled;
}
extern "C" void yz_ovr__cellGcmFunc15(ppu_context* ctx)
{
    if (getenv("YZ_GCM15_TRACE")) {
        static unsigned calls = 0;
        const unsigned n = ++calls;
        if (n <= 64 || (n & 0x3FFu) == 0u) {
            fprintf(stderr,
                    "[gcm15] n=%u cia=0x%08llX lr=0x%08llX "
                    "r3=0x%08llX r4=0x%08llX r5=0x%08llX r6=0x%08llX\n",
                    n, (unsigned long long)ctx->cia,
                    (unsigned long long)ctx->lr,
                    (unsigned long long)ctx->gpr[3],
                    (unsigned long long)ctx->gpr[4],
                    (unsigned long long)ctx->gpr[5],
                    (unsigned long long)ctx->gpr[6]);
        }
    }
    ctx->gpr[3] = 0;
}

/* Default FIFO callback. Each segment reserves its final word for a jump to the
 * next segment. Publish the jump before advancing PUT, and do not reuse a
 * segment while GET still points into it. */
extern "C" void yz_gcm_fifo_callback(ppu_context* ctx)
{
    const int use_callback_lock = yz_gcm_callback_lock_enabled();
    if (use_callback_lock)
        AcquireSRWLockExclusive(&g_yz_gcm_callback_lock);
    const LONG callback_depth =
        _InterlockedIncrement(&g_yz_gcm_callback_active);
    if (callback_depth > 1 && yz_gcm_callback_trace_enabled()) {
        static volatile LONG overlap_reports;
        const LONG report = _InterlockedIncrement(&overlap_reports);
        if (report <= 64 || (report & 0x3FF) == 0) {
            fprintf(stderr,
                    "[gcm] concurrent callbacks depth=%ld host_tid=%lu "
                    "guest_lr=0x%08llX ctx=0x%08llX\n",
                    callback_depth, (unsigned long)GetCurrentThreadId(),
                    (unsigned long long)ctx->lr,
                    (unsigned long long)ctx->gpr[3]);
            fflush(stderr);
        }
    }
    uint32_t gctx = (uint32_t)ctx->gpr[3];          /* CellGcmContextData* */
    yz_frontier_trace_emit(
        YZ_FT_GCM_CALLBACK, yz_thread_current_id(), 0u,
        gctx,
        gctx ? vm_read32(gctx + 0x0u) : 0u,
        gctx ? vm_read32(gctx + 0x4u) : 0u,
        gctx ? vm_read32(gctx + 0x8u) : 0u,
        vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_GET),
        vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT));
    { static int e = 0; if (e < 16) { e++;
        fprintf(stderr, "[gcm] callback(ctx=0x%08X count=0x%llX) begin=0x%08X end=0x%08X cur=0x%08X\n",
                gctx, (unsigned long long)ctx->gpr[4],
                gctx ? vm_read32(gctx + 0x0) : 0, gctx ? vm_read32(gctx + 0x4) : 0,
                gctx ? vm_read32(gctx + 0x8) : 0); } }
    if (gctx && yz_gcm_io_addr && g_yz_gcm_cmd_size &&
        g_yz_gcm_segment_bytes) {
        const uint32_t begin = vm_read32(gctx + 0x0);
        const uint32_t begin_off = begin - yz_gcm_io_addr;
        const uint32_t cur = vm_read32(gctx + 0x8);
        const uint32_t cur_off = cur - yz_gcm_io_addr;
        const uint32_t segment = cur_off / g_yz_gcm_segment_bytes;
        const uint32_t segment_count =
            g_yz_gcm_cmd_size / g_yz_gcm_segment_bytes;
        const uint32_t next_segment =
            segment_count ? (segment + 1u) % segment_count : 0u;
        uint32_t next_off = next_segment * g_yz_gcm_segment_bytes;
        if (next_segment == 0u)
            next_off += 0x1000u;
        const uint32_t next_begin = yz_gcm_io_addr + next_off;
        const uint32_t next_end = yz_gcm_io_addr +
            (next_segment + 1u) * g_yz_gcm_segment_bytes - 4u;

        vm_write32(cur, 0x20000000u | (next_off & 0x1FFFFFFCu));
        MemoryBarrier();
        vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_PUT, cur_off);
        vm_write32(gctx + 0x0, next_begin);
        vm_write32(gctx + 0x4, next_end);
        vm_write32(gctx + 0x8, next_begin);

        for (;;) {
            uint32_t get;
            uint32_t pending_return;
            /* GET can be in an external display list while the FIFO's pending
             * CALL return still owns a ring segment.  Snapshot both under the
             * consumer lock; recycling either address would overwrite work
             * the consumer has not retired yet. */
            yz_rsx_fifo_acquire();
            get = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_GET) & ~3u;
            pending_return = g_fifo_ret;
            yz_rsx_fifo_release();
            const uint32_t next_end_off = next_end - yz_gcm_io_addr;
            const int get_in_next = get >= next_off && get <= next_end_off;
            const int return_in_next = pending_return != ~0u &&
                pending_return >= next_off && pending_return <= next_end_off;
            const int get_in_initial_window = get < 0x1000u;
            if (!get_in_next && !return_in_next && !get_in_initial_window)
                break;
            if (return_in_next) {
                static unsigned protected_returns = 0;
                if (++protected_returns <= 16u) {
                    fprintf(stderr,
                            "[gcm] segment recycle waiting for pending FIFO "
                            "return=0x%X next=[0x%X,0x%X] GET=0x%X\n",
                            pending_return, next_off, next_end_off, get);
                    fflush(stderr);
                }
            }
            SwitchToThread();
        }
        { static int n = 0; if (n < 8) { n++;
            fprintf(stderr,
                    "[gcm] fifo segment: put=0x%X next=[0x%X,0x%X]\n",
                    cur_off, next_off, next_end - yz_gcm_io_addr); } }
    }
    ctx->gpr[3] = 0;
    yz_frontier_trace_emit(
        YZ_FT_GCM_CALLBACK, yz_thread_current_id(), 1u,
        gctx,
        gctx ? vm_read32(gctx + 0x0u) : 0u,
        gctx ? vm_read32(gctx + 0x4u) : 0u,
        gctx ? vm_read32(gctx + 0x8u) : 0u,
        vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_GET),
        vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT));
    _InterlockedDecrement(&g_yz_gcm_callback_active);
    if (use_callback_lock)
        ReleaseSRWLockExclusive(&g_yz_gcm_callback_lock);
}

/* HLE _cellGcmInitBody sets up the guest-memory contract, maps the io command
 * buffer, and builds the segmented default command context. Args:
 * gpr[3]=CellGcmContextData**, gpr[4]=cmdSize, gpr[5]=ioSize, gpr[6]=ioAddress. */
extern "C" void yz_ovr__cellGcmInitBody(ppu_context* ctx)
{
    uint32_t ctx_slot = (uint32_t)ctx->gpr[3];   /* CellGcmContextData** */
    uint32_t cmd_size = (uint32_t)ctx->gpr[4];
    uint32_t io_size  = (uint32_t)ctx->gpr[5];
    uint32_t io_addr  = (uint32_t)ctx->gpr[6];
    fprintf(stderr, "[gcm-hle] _cellGcmInitBody(ctx**=0x%08X cmdSize=0x%X ioSize=0x%X "
            "ioAddr=0x%08X)\n", ctx_slot, cmd_size, io_size, io_addr);

    const int32_t init_rc = cellGcmInit(cmd_size, io_size, io_addr);
    if (init_rc != 0) {
        ctx->gpr[3] = (uint64_t)(int64_t)init_rc;
        return;
    }

    yz_gcm_io_addr = io_addr;
    yz_gcm_io_size = io_size;
    g_yz_gcm_cmd_size = cmd_size;
    /* The title's Sony-libgcm lane configures this 8 MB FIFO as eight 1 MB
     * segments (measured bufdesc +0x30 == 0x40000 words).  Do not snapshot
     * cellGcmSys's process-wide default here: the title calls
     * cellGcmSetDefaultFifoSize only after _cellGcmInitBody returns, and the
     * conformance HLE currently starts at the SDK's generic 32 KB default.
     * Caching that early value produced 256 tiny segments and eventually
     * deadlocked the producer in the recycle callback. */
    g_yz_gcm_segment_bytes = 0x100000u;
    if (!g_yz_gcm_cmd_size)
        g_yz_gcm_cmd_size = g_yz_gcm_segment_bytes;
    if (g_yz_gcm_segment_bytes > g_yz_gcm_cmd_size)
        g_yz_gcm_segment_bytes = g_yz_gcm_cmd_size;

    /* RSX local memory (0xC0000000): reserved by vm_init, commit it now -- under LLE
     * sys_rsx_memory_allocate did this; under HLE the game addresses local VRAM
     * directly (cellGcmAddressToOffset / MapLocalMemory) right after init. */
    VirtualAlloc(vm_base + YZ_GCM_LOCAL_BASE, YZ_GCM_LOCAL_SIZE, MEM_COMMIT, PAGE_READWRITE);

    /* Guest-memory contract + window + consumer (the setup sys_rsx did under LLE;
     * RPCS3's _cellGcmInitBody likewise drives sys_rsx_context_allocate). */
    {
        static ppu_context sc;
        memset(&sc, 0, sizeof(sc));
        sc.gpr[8] = g_yz_gcm_system_mode;
        yz_sys_rsx_context_allocate(&sc);
    }

    /* Linear io map for the command-buffer ring: io X -> io_addr + X. Display-list
     * buffers add their own entries via cellGcmMapMainMemory/MapEaIoAddress. */
    {
        uint32_t pages = (io_size + 0xFFFFFu) >> 20;
        if (pages > 4096) pages = 4096;
        for (uint32_t i = 0; i < pages; i++)
            g_rsx_iomap_ea[i] = io_addr + (i << 20);
    }

    /* Commit the gcm context page (0x0FF8xxxx) + the synthetic callback OPD whose
     * code word routes to yz_gcm_fifo_callback via YZ_GCM_CB_FAKE_KEY. */
    VirtualAlloc(vm_base + (YZ_GCM_CTX_ADDR & ~0xFFFu), 0x2000, MEM_COMMIT, PAGE_READWRITE);
    vm_write32(YZ_GCM_CB_OPD_ADDR + 0, YZ_GCM_CB_FAKE_KEY);
    vm_write32(YZ_GCM_CB_OPD_ADDR + 4, 0);

    /* The first segment reserves 4 KB at the front.  `end` is the address of
     * the segment's final word; the callback writes its linking jump at the
     * last current position before switching. */
    uint32_t begin = io_addr + 0x1000;
    uint32_t end   = io_addr + g_yz_gcm_segment_bytes - 4u;
    vm_write32(YZ_GCM_CTX_ADDR + 0x0, begin);
    vm_write32(YZ_GCM_CTX_ADDR + 0x4, end);
    vm_write32(YZ_GCM_CTX_ADDR + 0x8, begin);
    vm_write32(YZ_GCM_CTX_ADDR + 0xC, YZ_GCM_CB_OPD_ADDR);

    /* The reserved first 4 KB begins with a jump into the buffer so the consumer
     * starting at GET=0 lands on the first command region. */
    vm_write32(io_addr, 0x20000000u | (0x1000u & 0x1FFFFFFCu));

    vm_write32(ctx_slot, YZ_GCM_CTX_ADDR);    /* hand the context to the game */
    ctx->gpr[3] = 0;
}

/* ---- HLE gcm helper overrides (root-cause fix, 2026-06-14f) ----------------
 * All io/control/label addresses point at the structures the faithful consumer
 * reads (RSX_DMA_CONTROL, RSX_REPORTS, g_rsx_iomap_ea) so the game and the RSX
 * agree by construction. Recovered from git 79869ac + reconciled. */
extern "C" uint32_t cellGcmGetTiledPitchSize(uint32_t size);
extern "C" uint64_t cellGcmGetTimeStampLocation(uint32_t index, uint32_t location);
extern "C" int32_t cellGcmSetDisplayBuffer(uint32_t buffer_id,
                                            uint32_t offset,
                                            uint32_t pitch,
                                            uint32_t width,
                                            uint32_t height);

/* put/get/ref poll pointer -> the GUEST dma_control (put@+0 get@+4 ref@+8). */
extern "C" void yz_ovr_cellGcmGetControlRegister(ppu_context* ctx)
{
    ctx->gpr[3] = RSX_DMA_CONTROL + RSX_DMACTL_PUT;
}

/* Labels live in the reports region the consumer writes (yz_rsx_sem_addr). */
extern "C" void yz_ovr_cellGcmGetLabelAddress(ppu_context* ctx)
{
    ctx->gpr[3] = RSX_REPORTS + ((uint32_t)ctx->gpr[3] & 0xFFu) * 0x10u;
}

/* Guest BE config: localAddr, ioAddr, localSize, ioSize, memFreq, coreFreq. */
extern "C" void yz_ovr_cellGcmGetConfiguration(ppu_context* ctx)
{
    uint32_t cfg = (uint32_t)ctx->gpr[3];
    if (cfg) {
        vm_write32(cfg + 0x00, YZ_GCM_LOCAL_BASE);
        vm_write32(cfg + 0x04, yz_gcm_io_addr);
        vm_write32(cfg + 0x08, YZ_GCM_LOCAL_SIZE);
        vm_write32(cfg + 0x0C, yz_gcm_io_size);
        vm_write32(cfg + 0x10, 650000000u);
        vm_write32(cfg + 0x14, 500000000u);
    }
    ctx->gpr[3] = 0;
}

extern "C" void yz_ovr_cellGcmSetDisplayBuffer(ppu_context* ctx)
{
    const uint32_t id = (uint32_t)ctx->gpr[3];
    const uint32_t offset = (uint32_t)ctx->gpr[4];
    const uint32_t pitch = (uint32_t)ctx->gpr[5];
    const uint32_t width = (uint32_t)ctx->gpr[6];
    const uint32_t height = (uint32_t)ctx->gpr[7];
    const int32_t rc = cellGcmSetDisplayBuffer(
        id, offset, pitch, width, height);
    if (rc == 0 && id < 8u) {
        g_rsx_dispbuf[id].width = width;
        g_rsx_dispbuf[id].height = height;
        g_rsx_dispbuf[id].pitch = pitch;
        g_rsx_dispbuf[id].offset = offset;
        if (id + 1u > g_rsx_dispbuf_count)
            g_rsx_dispbuf_count = id + 1u;
        rsx_live_draw_set_display_buffer(
            id, 0, offset, pitch, width, height);
        yz_nr_vertical_set_display_buffer(id, 0, offset, width, height);
    }
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

/* ea -> io offset via the consumer's iomap (the game computes put=AddressToOffset
 * (current)). Linear fast path for the command-buffer ring. */
extern "C" void yz_ovr_cellGcmAddressToOffset(ppu_context* ctx)
{
    uint32_t ea = (uint32_t)ctx->gpr[3], out = (uint32_t)ctx->gpr[4];
    if (ea >= YZ_GCM_LOCAL_BASE &&
        ea < YZ_GCM_LOCAL_BASE + YZ_GCM_LOCAL_SIZE) {
        if (out) vm_write32(out, ea - YZ_GCM_LOCAL_BASE);
        ctx->gpr[3] = 0; return;
    }
    if (yz_gcm_io_addr && ea >= yz_gcm_io_addr && ea < yz_gcm_io_addr + yz_gcm_io_size) {
        if (out) vm_write32(out, ea - yz_gcm_io_addr);
        ctx->gpr[3] = 0; return;
    }
    for (uint32_t p = 0; p < 4096; p++) {
        uint32_t base = g_rsx_iomap_ea[p];
        if (base != 0xFFFFFFFFu && ea >= base && ea < base + 0x100000u) {
            if (out) vm_write32(out, (p << 20) | (ea - base));
            ctx->gpr[3] = 0; return;
        }
    }
    ctx->gpr[3] = (uint64_t)(int64_t)-1;
}

/* Map a guest EA region to a specific io offset in the consumer's iomap. */
extern "C" void yz_ovr_cellGcmMapEaIoAddress(ppu_context* ctx)
{
    uint32_t ea = (uint32_t)ctx->gpr[3], io = (uint32_t)ctx->gpr[4], size = (uint32_t)ctx->gpr[5];
    yz_rsx_iomap_ensure_init();
    uint32_t pages = (size + 0xFFFFFu) >> 20;
    for (uint32_t i = 0; i < pages && ((io >> 20) + i) < 4096; i++)
        g_rsx_iomap_ea[(io >> 20) + i] = ea + (i << 20);
    fprintf(stderr, "[gcm-hle] MapEaIoAddress ea=0x%08X io=0x%X size=0x%X\n", ea, io, size);
    ctx->gpr[3] = 0;
}

extern "C" void yz_ovr_cellGcmUnmapEaIoAddress(ppu_context* ctx) { ctx->gpr[3] = 0; }

extern "C" void yz_ovr_cellGcmGetTiledPitchSize(ppu_context* ctx)
{
    ctx->gpr[3] = cellGcmGetTiledPitchSize((uint32_t)ctx->gpr[3]);
}

extern "C" void yz_ovr_cellGcmGetTimeStampLocation(ppu_context* ctx)
{
    ctx->gpr[3] = cellGcmGetTimeStampLocation((uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4]);
}

/* Publish a flip through the guest FIFO so draw submission, display queueing,
 * and presentation stay ordered on the one RSX consumer thread.  The public
 * GCM command ABI defines the head-1 queue/flip methods as 0xE944/0xE924; the
 * flip argument's high bit selects the buffer already queued for that head. */
static int32_t yz_gcm_append_flip_commands(uint32_t context,
                                           uint32_t buffer_id,
                                           int wait_for_label,
                                           uint32_t label_index,
                                           uint32_t label_value)
{
    rsx_nr_flip_contract flip = {};
    if (!context || !rsx_nr_flip_contract_init(
            &flip, buffer_id, wait_for_label, label_index, label_value))
        return (int32_t)CELL_GCM_ERROR_INVALID_VALUE;

    const uint32_t current = vm_read32(context + 0x08u);
    const uint32_t end = vm_read32(context + 0x04u);
    const uint32_t bytes = flip.word_count * 4u;
    if (!current || current > UINT32_MAX - bytes || current + bytes > end)
        return (int32_t)CELL_GCM_ERROR_FAILURE;

    for (uint32_t i = 0; i < flip.word_count; ++i)
        vm_write32((uint64_t)current + i * 4u, flip.words[i]);

    MemoryBarrier();
    vm_write32(context + 0x08u, current + bytes);
    return 0;
}

/* These entry points take the GCM context as arg0 (r3) and buffer id in r4. */
extern "C" void yz_ovr__cellGcmSetFlipCommand(ppu_context* ctx)
{
    const uint32_t context = (uint32_t)ctx->gpr[3];
    const uint32_t buffer_id = (uint32_t)ctx->gpr[4];
    int32_t result = 0;
    if (!yz_nr_vertical_try_flip(context, buffer_id, 0, 0, 0, &result))
        result = yz_gcm_append_flip_commands(context, buffer_id, 0, 0, 0);
    if (yz_ft_on())
        yz_ft("HLE-SetFlipCommand ctx=0x%08X buf=%u queued result=0x%08X",
              context, buffer_id, (uint32_t)result);
    ctx->gpr[3] = (uint64_t)(int64_t)result;
}

extern "C" void yz_ovr__cellGcmSetFlipCommandWithWaitLabel(ppu_context* ctx)
{
    const uint32_t context = (uint32_t)ctx->gpr[3];
    const uint32_t buffer_id = (uint32_t)ctx->gpr[4];
    const uint32_t label_index = (uint32_t)ctx->gpr[5];
    const uint32_t label_value = (uint32_t)ctx->gpr[6];
    int32_t result = 0;
    if (!yz_nr_vertical_try_flip(context, buffer_id, 1, label_index,
                                 label_value, &result))
        result = yz_gcm_append_flip_commands(
            context, buffer_id, 1, label_index, label_value);
    if (yz_ft_on())
        yz_ft("HLE-SetFlipCommandWithWaitLabel ctx=0x%08X buf=%u "
              "label=%u value=0x%08X queued result=0x%08X",
              context, buffer_id, label_index, label_value, (uint32_t)result);
    ctx->gpr[3] = (uint64_t)(int64_t)result;
}

/* ===========================================================================
 * sys_rsx syscalls (lv2 668-677, 0x29C-0x2A5) -- registered in shims.cpp.
 * Issued by Sony's libgcm; oracle = Emu/Cell/lv2/sys_rsx.cpp (reimplemented).
 * Args in gpr[3..8], return value in gpr[3] (written by the dispatcher).
 * =========================================================================*/

extern "C" void yz_watch_arm(uint32_t);        /* main.cpp page-guard write-watch (TEMP) */
extern "C" void yz_watch_arm_read(uint32_t);   /* main.cpp page-guard READ-watch (TEMP) */

/* 668 (0x29C) sys_rsx_memory_allocate(mem_handle*, mem_addr*, size, flags,...) */
extern "C" int64_t yz_sys_rsx_memory_allocate(ppu_context* ctx)
{
    uint32_t p_handle = (uint32_t)ctx->gpr[3];
    uint32_t p_addr   = (uint32_t)ctx->gpr[4];   /* u64 out */
    uint32_t size     = (uint32_t)ctx->gpr[5];

    /* RSX local memory: reserved by vm_init, commit it (game addresses it
     * directly via cellGcmAddressToOffset, base 0xC0000000). */
    VirtualAlloc(vm_base + YZ_GCM_LOCAL_BASE, YZ_GCM_LOCAL_SIZE,
                 MEM_COMMIT, PAGE_READWRITE);
    g_rsx_local_mem_size = size ? size : YZ_GCM_LOCAL_SIZE;

    if (p_addr)   vm_write64(p_addr, YZ_GCM_LOCAL_BASE);
    if (p_handle) vm_write32(p_handle, 0x5A5A5A5Bu);
    fprintf(stderr, "[sys_rsx] memory_allocate size=0x%X -> addr=0x%08X handle=0x5A5A5A5B\n",
            size, YZ_GCM_LOCAL_BASE);
    return 0;
}

/* 669 (0x29D) sys_rsx_memory_free(mem_handle) */
extern "C" int64_t yz_sys_rsx_memory_free(ppu_context* ctx) { return 0; }

/* 670 (0x29E) sys_rsx_context_allocate(ctx_id*, dma_ctl*, drv*, reports*,
 *             mem_ctx, system_mode). Sets up the whole guest-memory contract
 *             and starts the window + FIFO consumer (was _cellGcmInitBody's
 *             job under HLE). */
extern "C" int64_t yz_sys_rsx_context_allocate(ppu_context* ctx)
{
    uint32_t p_ctx_id = (uint32_t)ctx->gpr[3];
    uint32_t p_dma    = (uint32_t)ctx->gpr[4];   /* u64 out */
    uint32_t p_drv    = (uint32_t)ctx->gpr[5];   /* u64 out */
    uint32_t p_rep    = (uint32_t)ctx->gpr[6];   /* u64 out */
    uint64_t sys_mode = ctx->gpr[8];

    yz_rsx_iomap_ensure_init();

    /* Commit the context region (dma_control / driver_info / reports / device,
     * 4 MB) in the reserved RSX VM window. */
    VirtualAlloc(vm_base + RSX_CTX_BASE, 0x400000, MEM_COMMIT, PAGE_READWRITE);

    /* driver_info -- RPCS3 sys_rsx.cpp:289. version_driver 0x211 is REQUIRED:
     * Sony's libgcm validates it and bails otherwise (libgcm_sys:770). */
    memset(vm_base + RSX_DRIVER_INFO, 0, 0x12F8);
    vm_write32(RSX_DRIVER_INFO + 0x00, 0x211);                 /* version_driver */
    vm_write32(RSX_DRIVER_INFO + 0x04, 0x5C);                  /* version_gpu */
    vm_write32(RSX_DRIVER_INFO + 0x08, g_rsx_local_mem_size);  /* memory_size */
    vm_write32(RSX_DRIVER_INFO + 0x0C, 1);                     /* hardware_channel */
    vm_write32(RSX_DRIVER_INFO + 0x10, 500000000u);            /* nvcore_frequency */
    vm_write32(RSX_DRIVER_INFO + 0x14, 650000000u);            /* memory_frequency */
    vm_write32(RSX_DRIVER_INFO + 0x2C, 0x1000);               /* reportsNotifyOffset */
    vm_write32(RSX_DRIVER_INFO + 0x30, 0);                    /* reportsOffset */
    vm_write32(RSX_DRIVER_INFO + 0x34, 0x1400);               /* reportsReportOffset */
    vm_write32(RSX_DRIVER_INFO + 0x54, (uint32_t)sys_mode);   /* systemModeFlags */

    /* reports region (RPCS3 init values: semaphore patterns, notify/report
     * timestamps = -1). semaphore[1024]@0, notify[64]@0x1000, report[2048]@0x1400. */
    memset(vm_base + RSX_REPORTS, 0, 0x9400);
    for (uint32_t i = 0; i < 1024; i += 4) {
        vm_write32(RSX_REPORTS + (i + 0) * 4, 0x1337C0D3u);
        vm_write32(RSX_REPORTS + (i + 1) * 4, 0x1337BABEu);
        vm_write32(RSX_REPORTS + (i + 2) * 4, 0x1337BEEFu);
        vm_write32(RSX_REPORTS + (i + 3) * 4, 0x1337F001u);
    }
    for (uint32_t i = 0; i < 64; i++)
        vm_write64(RSX_REPORTS + 0x1000 + i * 16, ~0ull);     /* notify timestamp */
    for (uint32_t i = 0; i < 2048; i++) {
        vm_write64(RSX_REPORTS + 0x1400 + i * 16 + 0, ~0ull); /* report timestamp */
        vm_write32(RSX_REPORTS + 0x1400 + i * 16 + 8, 0);     /* report val */
        vm_write32(RSX_REPORTS + 0x1400 + i * 16 + 12, ~0u);  /* report pad */
    }

    /* dma_control: get/put/ref = 0 (libgcm sets them up). */
    memset(vm_base + RSX_DMA_CONTROL, 0, 0x60);

    if (p_ctx_id) vm_write32(p_ctx_id, 0x55555555u);
    if (p_dma)    vm_write64(p_dma, RSX_DMA_CONTROL);
    if (p_drv)    vm_write64(p_drv, RSX_DRIVER_INFO);
    if (p_rep)    vm_write64(p_rep, RSX_REPORTS);

    /* RSX event port + queue (RPCS3 sys_rsx.cpp:317-325): libgcm spawns an
     * interrupt thread that does sys_event_queue_receive on
     * driver_info.handler_queue (@+0x12D0). Without a real queue it gets ESRCH
     * and the thread dies, leaving libgcm's gcm-handler delivery degenerate.
     * Create the port, create the queue (overwrites handler_queue with the queue
     * id), and connect them. The lv2 handlers read args from gpr[3..6]; drive
     * them with a scratch context (gpr offsets match the runtime layout). */
    {
        uint32_t hq   = RSX_DRIVER_INFO + 0x12D0;   /* driver_info.handler_queue */
        uint32_t attr = RSX_DEVICE_ADDR + 0x1000;   /* committed scratch */
        vm_write32(attr + 0, 1);                    /* SYS_SYNC_PRIORITY */
        vm_write32(attr + 4, 1);                    /* SYS_PPU_QUEUE */
        vm_write64(attr + 8, 0);                    /* name */
        static ppu_context sc;
        memset(&sc, 0, sizeof(sc));
        sc.gpr[3] = hq; sc.gpr[4] = 1 /*SYS_EVENT_PORT_LOCAL*/; sc.gpr[5] = 0;
        sys_event_port_create(&sc);
        g_rsx_event_port = vm_read32(hq);
        sc.gpr[3] = hq; sc.gpr[4] = attr; sc.gpr[5] = 0; sc.gpr[6] = 0x20;
        sys_event_queue_create(&sc);               /* overwrites hq with queue id */
        uint32_t qid = vm_read32(hq);
        sc.gpr[3] = g_rsx_event_port; sc.gpr[4] = qid;
        sys_event_port_connect_local(&sc);
        fprintf(stderr, "[sys_rsx] event port=%u queue=%u (driver_info.handler_queue)\n",
                g_rsx_event_port, qid);
    }

    g_rsx_ctx_ready = 1;
    fprintf(stderr, "[sys_rsx] context_allocate -> dma=0x%08X drv=0x%08X rep=0x%08X "
            "(sys_mode=0x%llX)\n", RSX_DMA_CONTROL, RSX_DRIVER_INFO, RSX_REPORTS,
            (unsigned long long)sys_mode);

    /* Bring up the window + RSX consumer + command-translator state (the
     * LLE-equivalent of the old _cellGcmInitBody startup). */
    static int started = 0;
    if (!started) {
        started = 1;
        yz_nr_vertical_init();
        yz_nr_shadow_init();
        yz_fe0_timeline_init();
        yz_wkl4_cycle_init();
        if (yz_rsx_wait_classifier_init()) {
            yz_rsx_wait_classifier_set_completed_draw_baseline(
                rsx_live_draw_get_completed_draws());
        }
        rsx_state_init(&g_rsx_state);
        CreateThread(NULL, 0, yz_window_thread, NULL, 0, NULL);
        if (getenv("YZ_RSX_INLINE")) {
            /* INLINE/SYNCHRONOUS RSX: no free-running async consumer. The FIFO is
             * drained on the PRODUCER's thread -- on every PUT flush (vm_write32 ->
             * yz_rsx_inline_on_put) and during the reserve's sub-ms usleep wait
             * (sys_timer.c -> g_yz_usleep_pump). Couples GET to PUT so the producer
             * can't lap the ring between placing and releasing a stopper. */
            yz_rsx_fifo_lock_ensure();
            g_yz_usleep_pump = yz_rsx_fifo_pump;
            fprintf(stderr, "[rsx] INLINE mode: async consumer OFF; FIFO drained on producer (flush + reserve usleep)\n");
        } else if (!getenv("YZ_NO_CONSUMER")) {   /* TEMP: isolate consumer vs libgcm */
            CreateThread(NULL, 0, yz_rsx_consumer, NULL, 0, NULL);
        }
        if (getenv("YZ_IMM_REL"))        /* force immediate stopper-release (S[0x1C]=0) */
            CreateThread(NULL, 0, yz_bigseg_monitor, NULL, 0, NULL);
        if (getenv("YZ_TRACE_DEFER"))    /* READ-ONLY: trace the gcm defer decision + op-list drain */
            CreateThread(NULL, 0, yz_defer_trace_mon, NULL, 0, NULL);
        if (getenv("YZ_PHASE"))          /* high-detail producer+consumer timeline (working->broken) */
            CreateThread(NULL, 0, yz_phase_monitor, NULL, 0, NULL);
        if (getenv("YZ_DUMP_BUFDESC"))   /* dump libgcm segment geometry at the deadlock */
            CreateThread(NULL, 0, yz_bufdesc_dump, NULL, 0, NULL);
        if (getenv("YZ_WATCH_DLEA"))     /* direct write-watch on frame-3 display list io 0x1104D00 */
            CreateThread(NULL, 0, yz_watch_dlea_mon, NULL, 0, NULL);
        if (getenv("YZ_SEGBIG"))         /* single big FIFO segment so t1 never recycle-wedges */
            CreateThread(NULL, 0, yz_segsize_mon, NULL, 0, NULL);
        if (getenv("YZ_WATCH_OPLIST"))   /* name the data-patch APPENDER (drain RE, pt25b) */
            CreateThread(NULL, 0, yz_oplist_watch_mon, NULL, 0, NULL);
        /* DIAG: catch who writes the jump-to-self stopper at io 0x300000
         * (ea 0x40700000). Reveals the game function/caller that parks the RSX
         * there -- flush safety-stopper vs deliberate pause vs corruption. */
        if (getenv("YZ_WATCH_300")) yz_watch_arm(0x40700000u);
        /* DIAG (2026-06-14g): catch who writes the flip fence at 0x40C00000 (io
         * 0x800000) that t1 spins on. Tells us if the RSX/consumer (in-stream
         * NV308A), the flip completion, or t1 itself advances it -- and why it
         * sticks at 2. The core question: what control value does the game wait
         * for that we don't produce. */
        if (getenv("YZ_WATCH_FENCE")) yz_watch_arm(0x40C00000u);
        /* DIAG (2026-06-14h): catch the DISPLAY-LIST BUILDER. Arm a write-watch
         * on an arbitrary ea (hex) -- point it at a list region that DOES get
         * written (e.g. frame-3's region 17 ea 0x41500000) so the watch fires
         * when the builder fills it; the (writer-aware) handler then names the
         * builder thread's guest tid + lifted function. Then the wait recorder
         * shows what that thread blocks on for the NEXT frame. */
        if (const char* we = getenv("YZ_WATCH_EA"))
            yz_watch_arm((uint32_t)strtoul(we, nullptr, 16));
        /* READ-watch (2026-06-19): catch what POLLS an address (e.g. the flip
         * fence 0x40C00000) in the reader's own context -> reliable caller chain. */
        if (const char* rd = getenv("YZ_WATCH_READ"))
            yz_watch_arm_read((uint32_t)strtoul(rd, nullptr, 16));
    }
    return 0;
}

/* 671 (0x29F) sys_rsx_context_free(context_id) */
extern "C" int64_t yz_sys_rsx_context_free(ppu_context* ctx) { return 0; }

/* 672 (0x2A0) sys_rsx_context_iomap(context_id, io, ea, size, flags) */
extern "C" int64_t yz_sys_rsx_context_iomap(ppu_context* ctx)
{
    uint32_t io   = (uint32_t)ctx->gpr[4];
    uint32_t ea   = (uint32_t)ctx->gpr[5];
    uint32_t size = (uint32_t)ctx->gpr[6];
    yz_rsx_iomap_ensure_init();
    for (uint32_t off = 0; off < size; off += 0x100000u) {
        uint32_t page = (io + off) >> 20;
        if (page < 4096) g_rsx_iomap_ea[page] = ea + off;
    }
    fprintf(stderr, "[sys_rsx] context_iomap io=0x%X ea=0x%X size=0x%X\n", io, ea, size);
    return 0;
}

/* 673 (0x2A1) sys_rsx_context_iounmap(context_id, io, size) */
extern "C" int64_t yz_sys_rsx_context_iounmap(ppu_context* ctx)
{
    uint32_t io   = (uint32_t)ctx->gpr[4];
    uint32_t size = (uint32_t)ctx->gpr[5];
    yz_rsx_iomap_ensure_init();
    for (uint32_t off = 0; off < size; off += 0x100000u) {
        uint32_t page = (io + off) >> 20;
        if (page < 4096) g_rsx_iomap_ea[page] = 0xFFFFFFFFu;
    }
    return 0;
}

/* 674 (0x2A2) sys_rsx_context_attribute(context_id, package_id, a3,a4,a5,a6).
 * The workhorse: flip / display-buffer / queue / flip-reset / vblank / tile /
 * zcull all funnel through here (RPCS3 sys_rsx.cpp:505). */
extern "C" int64_t yz_sys_rsx_context_attribute(ppu_context* ctx)
{
    uint32_t pkg = (uint32_t)ctx->gpr[4];
    uint64_t a3 = ctx->gpr[5], a4 = ctx->gpr[6], a5 = ctx->gpr[7];
    (void)ctx->gpr[8];   /* a6 (tile/zcull status, unused for now) */

    switch (pkg) {
    case 0x001: {       /* FIFO: set get/put */
        /* DIAG (YZ_LOG_FIFOSET, 2026-06-24): does this syscall STOMP the consumer's
         * live GET (a3 != current GET) and/or is it the (only) PUT writer that makes
         * PUT bounce? RPCS3 serializes this under sys_rsx_mtx; we don't. */
        if (getenv("YZ_LOG_FIFOSET")) {
            uint32_t cur_get = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_GET);
            uint32_t cur_put = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT);
            static int n = 0; if (n < 400) { n++;
                fprintf(stderr, "[fifoset] pkg001 set GET 0x%06X->0x%06X  PUT 0x%06X->0x%06X%s%s\n",
                        cur_get & 0xFFFFFF, (uint32_t)a3 & 0xFFFFFF,
                        cur_put & 0xFFFFFF, (uint32_t)a4 & 0xFFFFFF,
                        ((uint32_t)a3 != cur_get) ? "  <-- GET STOMP" : "",
                        ((uint32_t)a4 < cur_put) ? "  <-- PUT RECEDES" : ""); }
        }
        if (yz_ft_on()) {
            uint32_t cg = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_GET);
            uint32_t cp = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT);
            yz_ft("FIFOSET GET 0x%06X->0x%06X PUT 0x%06X->0x%06X",
                  cg & 0xFFFFFF, (uint32_t)a3 & 0xFFFFFF,
                  cp & 0xFFFFFF, (uint32_t)a4 & 0xFFFFFF);
        }
        /* Serialize the guest GET/PUT set with the consumer's single GET writer
         * (RPCS3 sys_rsx_mtx) so a pkg001 set can't tear a GET the consumer is
         * mid-advance, and the consumer can't clobber a fresh pkg001 set. */
        yz_rsx_fifo_lock_ensure();
        EnterCriticalSection(&g_rsx_fifo_lock);
        vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, (uint32_t)a3);
        vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_PUT, (uint32_t)a4);
        LeaveCriticalSection(&g_rsx_fifo_lock);
        break;
    }
    case 0x100: break;  /* display mode set */
    case 0x101: break;  /* display sync set (vsync pref) */
    case 0x102: {       /* Display flip */
        uint32_t head = (uint32_t)a3 & 7;
        { static int n = 0; if (n < 4) { n++;
            fprintf(stderr, "[sys_rsx] FLIP head=%u a4=0x%llX\n",
                    head, (unsigned long long)a4); } }
        uint32_t flip_idx;
        if (a4 & 0x80000000u) {           /* grab the queued buffer */
            flip_idx = vm_read32(yz_rsx_head_addr(head) + 0x14); /* lastQueuedBufferId */
        } else {                          /* a4 = display buffer offset */
            flip_idx = 0;
            for (uint32_t i = 0; i < g_rsx_dispbuf_count; i++)
                if (g_rsx_dispbuf[i].offset == (uint32_t)a4) { flip_idx = i; break; }
        }
        /* ONE present per flip, done by the vblank retire (s23): presenting here
         * AND arming pending double-presented every immediate flip once the
         * 0xE920 method bridge went live (title-bar flips ran ~2x Track B
         * frames, s23boot1). Record the resolved buffer on the head so the
         * retire presents the right one for the offset form too, then arm; the
         * retire presents + publishes the done bit (which also survives the
         * game's cellGcmResetFlipStatus ordering). */
        vm_write32(yz_rsx_head_addr(head) + 0x14, flip_idx);  /* lastQueuedBufferId */
        InterlockedExchange(&g_rsx_flip_pending[head], 1);
        if (yz_ft_on())
            yz_ft("SYSFLIP head=%u buf=%u a4=0x%llX (arm pending[%u], no label write)",
                  head, flip_idx, (unsigned long long)a4, head);
        break;
    }
    case 0x103: {       /* Display queue */
        uint32_t head = (uint32_t)a3 & 7;
        uint32_t ha = yz_rsx_head_addr(head);
        vm_write32(ha + 0x14, (uint32_t)a4);   /* lastQueuedBufferId */
        vm_write32(ha + 0x08, vm_read32(ha + 0x08) | 0x40000000u
                              | (1u << ((uint32_t)a4 & 31)));
        if (yz_ft_on())
            yz_ft("SYSQUEUE head=%u buf=%u (no arm)", head, (uint32_t)a4);
        /* s23 conformance fix (4th dropped-guest-notification instance, same
         * class as 0xEB00/0xE920): RPCS3's 0x103 also DELIVERS the queue event
         * -- send_event(0, SYS_RSX_EVENT_QUEUE_BASE << head, 0), sys_rsx.cpp:637,
         * sys_rsx.h:74 (1<<5). Gate on the game's registered handler mask like
         * the vblank/flip sends (Sony's intr thread dispatches by cause bits);
         * loud first hits. Kill-switch YZ_NO_QEV. */
        { static int nq = -1; if (nq < 0) { nq = getenv("YZ_NO_QEV") ? 1 : 0;
            fprintf(stderr, "[qev] armed (queue-event dispatch %s)\n",
                    nq ? "DISABLED by YZ_NO_QEV" : "on"); fflush(stderr); }
          if (!nq && g_rsx_event_port) {
            uint64_t qbit = (uint64_t)(0x20u << head);      /* QUEUE_BASE<<head */
            uint32_t handlers = vm_read32(RSX_DRIVER_INFO + 0x12C0);
            if (handlers & qbit) {
                /* s25 audit risk #1: same lossless-latch as the user-cmd —
                 * a queue-full send ORs into the shared pending mask and the
                 * consumer-top retry delivers it (edge event, loss was
                 * permanent). */
                int64_t r = yz_rsx_ev_send(qbit);
                static unsigned long qn = 0; qn++;
                if (qn <= 8 || (qn & 0xFFu) == 0) {
                    fprintf(stderr, "[qev] n=%lu head=%u buf=%u send=%lld\n",
                            qn, head, (uint32_t)a4, (long long)r); fflush(stderr); }
            } else { static int w = 0; if (w < 2) { w++;
                fprintf(stderr, "[qev] game not listening (handlers=0x%08X, qbit=0x%llX) -- benign\n",
                        handlers, (unsigned long long)qbit); fflush(stderr); } }
          } }
        break;
    }
    case 0x104: {       /* Display buffer registration */
        uint32_t id = (uint32_t)a3 & 0xFF;
        if (id < 8) {
            g_rsx_dispbuf[id].width  = (uint32_t)(a4 >> 32);
            g_rsx_dispbuf[id].height = (uint32_t)(a4 & 0xFFFFFFFF);
            g_rsx_dispbuf[id].pitch  = (uint32_t)(a5 >> 32);
            g_rsx_dispbuf[id].offset = (uint32_t)(a5 & 0xFFFFFFFF);
            if (id + 1 > g_rsx_dispbuf_count) g_rsx_dispbuf_count = id + 1;
            rsx_live_draw_set_display_buffer(
                id, 0, g_rsx_dispbuf[id].offset, g_rsx_dispbuf[id].pitch,
                g_rsx_dispbuf[id].width, g_rsx_dispbuf[id].height);
            yz_nr_vertical_set_display_buffer(
                id, 0, g_rsx_dispbuf[id].offset,
                g_rsx_dispbuf[id].width, g_rsx_dispbuf[id].height);
        }
        break;
    }
    case 0x10A: {       /* flip-status reset (cellGcmResetFlipStatus) */
        uint32_t head = (uint32_t)a3 & 7;
        uint32_t ha = yz_rsx_head_addr(head);
        vm_write32(ha + 0x08, (vm_read32(ha + 0x08) & (uint32_t)a4) | (uint32_t)a5);
        if (yz_ft_on())
            yz_ft("RESETSTATUS head=%u and=0x%X or=0x%X", head,
                  (uint32_t)a4, (uint32_t)a5);
        break;
    }
    case 0xFEC: {       /* flip event notification (mark done immediately) */
        uint32_t head = (uint32_t)a3 & 7;
        uint32_t ha = yz_rsx_head_addr(head);
        vm_write32(ha + 0x08, vm_read32(ha + 0x08) | 0x80000000u);
        vm_write64(ha + 0x00, (uint64_t)GetTickCount() * 1000);  /* lastFlipTime */
        if (yz_ft_on()) yz_ft("FEC head=%u (done bit)", head);
        break;
    }
    case 0xFED: break;  /* vblank command (our yz_rsx_vblank_tick drives vblank) */
    default: {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[sys_rsx] context_attribute package 0x%X (nop)\n", pkg);
        }
        break;
    }
    }
    return 0;
}

/* 675 (0x2A3) sys_rsx_device_map(dev_addr*, a2*, dev_id) */
extern "C" int64_t yz_sys_rsx_device_map(ppu_context* ctx)
{
    uint32_t p_dev = (uint32_t)ctx->gpr[3];   /* u64 out */
    VirtualAlloc(vm_base + RSX_DEVICE_ADDR, 0x100000, MEM_COMMIT, PAGE_READWRITE);
    /* device+0x30 = 1: initial HW flip credit set at RSX init (RSXThread.cpp:2487
     * thread::init). The first flip's ACQUIRE device+0x30==1 waits on this. */
    vm_write32(RSX_DEVICE_ADDR + 0x30, 1);
    if (p_dev) vm_write64(p_dev, RSX_DEVICE_ADDR);
    fprintf(stderr, "[sys_rsx] device_map dev_id=0x%X -> 0x%08X\n",
            (uint32_t)ctx->gpr[5], RSX_DEVICE_ADDR);
    return 0;
}

/* 676 (0x2A4) sys_rsx_device_unmap(dev_id) */
extern "C" int64_t yz_sys_rsx_device_unmap(ppu_context* ctx) { return 0; }

/* 677 (0x2A5) sys_rsx_attribute(packageId, a2, a3, a4, a5) */
extern "C" int64_t yz_sys_rsx_attribute(ppu_context* ctx) { return 0; }

/* Host vblank tick (called ~62 Hz by main.cpp's yz_vblank_thread). Bumps the
 * per-head vBlankCount/time and publishes any pending flip's completion (the
 * done bit the game's render loop polls inline). */
extern "C" void yz_rsx_vblank_tick(void)
{
    if (!g_rsx_ctx_ready) return;
#ifdef YZ_NATIVE_GCM
    cellGcmTickVBlank();
#endif
    yz_ft_start();   /* YZ_FLIPTRACE arm banner + label watcher (no-op if off) */

    /* s25: redeliver any latched SPU throw_events (spu_channels.c loss latch,
     * notification-audit risk #3) — ~16 ms retry cadence, no-op when empty. */
    { extern void yz_throw_retry_flush(void);
      yz_throw_retry_flush(); }

    /* s28 t1 host-liveness heartbeat (env YZ_T1_HB — ledger #63, the
     * early-stall root probe): every ~2 s, t1's guest cia/lr/r1 (syscall-
     * boundary stale is fine) + the HOST thread's kernel/user CPU-time DELTAS.
     * du climbing = t1 SPINS in untagged guest code (hypothesis a); both ~0 =
     * the host thread is never rescheduled after sys_ppu_thread_create
     * returns (hypothesis b, runtime scheduling bug). Armed banner so zero
     * output is MEASURED. */
    { static int hb = -1; static ULONGLONG hbms = 0;
      static unsigned long long lk = 0, lu = 0;
      if (hb < 0) { hb = getenv("YZ_T1_HB") ? 1 : 0;
          if (hb) { fprintf(stderr, "[t1-hb] ARMED (2s host-liveness heartbeat)\n"); fflush(stderr); } }
      if (hb) { ULONGLONG now = GetTickCount64();
        if (now - hbms >= 2000) { hbms = now;
            FILETIME c1, e1, kt, ut;
            unsigned long long k = 0, u = 0;
            if (g_yz_t1_handle && GetThreadTimes(g_yz_t1_handle, &c1, &e1, &kt, &ut)) {
                k = ((unsigned long long)kt.dwHighDateTime << 32) | kt.dwLowDateTime;
                u = ((unsigned long long)ut.dwHighDateTime << 32) | ut.dwLowDateTime;
            }
            /* s28m6 answered the fork: the stalled t1 burns FULL cpu (du at
             * the healthy baseline) with zero trace output = a pure guest
             * spin. Sample the spinning RIP (brief suspend, 1/2s — mild per
             * LESSONS #6b, and the boot is already stalled when it matters)
             * and resolve to the guest function. */
            uint32_t grip = 0; unsigned long long hrip = 0;
            if (g_yz_t1_handle && SuspendThread(g_yz_t1_handle) != (DWORD)-1) {
                CONTEXT tc; memset(&tc, 0, sizeof(tc));
                tc.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(g_yz_t1_handle, &tc)) {
                    hrip = tc.Rip;
                    extern uint32_t yz_guest_addr_from_host(const void* rip);
                    grip = yz_guest_addr_from_host((const void*)tc.Rip);
                }
                ResumeThread(g_yz_t1_handle);
            }
            { extern uint32_t g_yz_t1_sc; extern uint64_t g_yz_t1_sc_r3;
              fprintf(stderr, "[t1-hb] rip=%llX guest=0x%08X sc=%u r3=0x%llX r1=0x%08X dk=%llu du=%llu\n",
                    hrip, grip, g_yz_t1_sc, (unsigned long long)g_yz_t1_sc_r3,
                    g_yz_main_ctx ? (uint32_t)g_yz_main_ctx->gpr[1] : 0,
                    k - lk, u - lu); }
            fflush(stderr);
            lk = k; lu = u; } } }

    /* s26: hardware watchpoint arm trigger (env YZ_HWWATCH — main.cpp
     * yz_hwwatch_arm): fire once at tick 2000 (~32 s, all threads live). */
    { static int hw = -1; static int hwdone = 0; static unsigned long hwt = 0;
      if (hw < 0) hw = getenv("YZ_HWWATCH") ? 1 : 0;
      if (hw && !hwdone && ++hwt == 2000) { hwdone = 1;
          yz_hwwatch_arm(); } }

    /* s26: wid4 work-record slot poll (env YZ_W4REC_POLL, diag — ledger #57
     * mode B). The page-guard write-watch on these slots was BOTH invasive
     * (shares the 4 KB page with the pool's ctx save area) and unreliable
     * (s26ride6c: pool ran, ctx saves hit the page, ZERO guard hits) — poll
     * the five 0x40-stride record slots here instead, log word0 (publish
     * value) + word1 (target EA guard) on change. No faults, ~16 ms
     * resolution; records persist between stagings so transitions are
     * caught. Correlate with [w4rec] fetch-time dumps. */
    { static int wp = -1; static int wproot = -1;
      if (wp < 0) { wp = getenv("YZ_W4REC_POLL") ? 1 : 0;
          if (wp) { fprintf(stderr, "[w4poll] ARMED: record-slot poll live\n"); fflush(stderr); } }
      if (wproot < 0) wproot = getenv("YZ_A010_ROOT") ? 1 : 0;
      if (wp && (!wproot ||
          InterlockedCompareExchange(&g_yz_a010_root_active, 0, 0) != 0)) {
          /* s26 ride17 addition: slot[5] = the decode label itself — [fe0]
           * proved publish-8 ISSUED while the acquire read 7 forever; this
           * poll discriminates lost-write (never becomes 8) vs reverted-write
           * (8 flickers then 7 — a stale-snapshot PUTLLC restoring the line). */
          static const uint32_t slots[6] =
              {0x424528A0u,0x424528E0u,0x42452920u,0x42452960u,0x424529A0u,
               0x10200FE0u};
          static uint32_t prev[6][2];
          static int winit = 0;
          for (int i = 0; i < 6; i++) {
              uint32_t w0 = vm_read32(slots[i]);
              uint32_t w1 = vm_read32(slots[i] + 4);
              if (!winit || w0 != prev[i][0] || w1 != prev[i][1]) {
                  fprintf(stderr, "[w4poll] slot=0x%08X val=0x%08X ea=0x%08X\n",
                          slots[i], w0, w1);
                  fflush(stderr);
                  prev[i][0] = w0; prev[i][1] = w1;
              }
          }
          winit = 1;
      } }

    /* s26: redeliver any latched RSX event bits (the s25 ucmd/EBUSY latch,
     * ledger #52) from HERE too. The consumer-top retry site DEADLOCKS when
     * the lost delivery itself parks the FIFO consumer (MEASURED s26ride4:
     * coalesced cause=2 EBUSY-latched, exactly one consumer-side retry, then
     * the consumer parked on the 0xFE0 acquire that the lost handler run
     * caused — the latch never retried again all boot). lv1 redelivers when
     * the queue drains, independent of FIFO flow; this tick is our
     * queue-drain-independent cadence (same pattern as the throw latch
     * above). Same kill-switches (YZ_NO_UCMD_RETRY / YZ_NO_EV_RETRY) gate the
     * latch at its source, so no separate gate here. */
    if (g_rsx_ev_pending) yz_ucmd_retry_pending();

    /* DIAG (one-time, vblank-dispatch hunt): dump Sony's libgcm vblank/flip
     * handler table once the game has registered a handler. The lifted intr-
     * thread dispatcher (libgcm 0x021082D8) reads the table from
     * r29 = *(libgcm_toc - 0x7FB8) = *(0x0210C048), then calls the handler OPDs
     * at [r29 + 0x08/0x0C/0x10/0x14/0x20/0x24/0x28] gated on cause bits. Show
     * which slots are non-null (registered) and their entry points. */
    { static int dumped = 0;
      uint32_t handlers = vm_read32(RSX_DRIVER_INFO + 0x12C0);
      if (!dumped && handlers) {
          dumped = 1;
          uint32_t tbl = vm_read32(0x0210C048u);
          fprintf(stderr, "[diag] libgcm handler-table ptr=0x%08X handlers=0x%X\n", tbl, handlers);
          if (tbl >= 0x02000000u && tbl < 0x02140000u) {
              const uint32_t offs[7] = {0x08,0x0C,0x10,0x14,0x20,0x24,0x28};
              for (int i = 0; i < 7; i++) {
                  uint32_t opd = vm_read32(tbl + offs[i]);
                  uint32_t ent = (opd >= 0x10000u && opd < 0x02140000u) ? vm_read32(opd) : 0;
                  fprintf(stderr, "[diag]   slot+0x%02X opd=0x%08X entry=0x%08X\n",
                          offs[i], opd, ent);
              }
          }
          /* LAYER-1 BISECT (env YZ_GCMCTX_BISECT): dump libgcm's PRIVATE context
           * GCMCTX = *(game_toc-0x5014). func_00EDD15C bctrl's the handler OPDs
           * at GCMCTX[0x00] (arg [0x04]) and GCMCTX[0x08] (arg [0x0C]); the t7
           * stack-overflow recursion runs through here. Dump those OPDs + their
           * code/TOC so we can see whether a handler points back into the
           * dispatch chain (re-entrant) -- and the predicates +0x10/+0x15C/+0x398. */
          if (getenv("YZ_GCMCTX_BISECT") && g_yz_game_toc) {
              uint32_t g = vm_read32(g_yz_game_toc - 0x5014u);
              fprintf(stderr, "[gcmctx] GCMCTX=0x%08X\n", g);
              if (g >= 0x10000u && g < 0xE0000000u) {
                  const uint32_t fo[] = {0x00,0x04,0x08,0x0C,0x10,0x14,0x15C,0x164,0x398};
                  for (size_t i = 0; i < sizeof(fo)/sizeof(fo[0]); i++) {
                      uint32_t v = vm_read32(g + fo[i]);
                      uint32_t code = (v >= 0x10000u && v < 0xE0000000u) ? vm_read32(v) : 0;
                      uint32_t toc  = (v >= 0x10000u && v < 0xE0000000u) ? vm_read32(v + 4u) : 0;
                      fprintf(stderr, "[gcmctx]   +0x%03X = 0x%08X  (opd->code=0x%08X toc=0x%08X)\n",
                              fo[i], v, code, toc);
                  }
              }
          }
          fflush(stderr);
      }
    }

    /* DIAG (TEMP): heartbeat -- proves the vblank thread is live and shows the
     * flip-completion state (pending bits + the flip label the consumer waits
     * on). Once/sec. */
    { static int vbt=-1; static unsigned vt = 0;
      if (vbt < 0) vbt = getenv("YZ_VBL_TRACE") ? 1 : 0;
      if (vbt && (vt++ & 63u) == 0)
          fprintf(stderr, "[vbl] tick=%u pending=[%ld %ld] label@0x10200010=0x%08X qhead=%u\n",
                  vt, g_rsx_flip_pending[0], g_rsx_flip_pending[1],
                  vm_read32(RSX_REPORTS + 0x10), g_rsx_queued_head); }

    /* YZ_JOBPEEK (s21): change-triggered hexdump of the CRI jobchain command
     * stream (0x4019CA80-CB40) + the chain header (0x4019C880-8C0), so the
     * producer's round-N command WRITES are visible independently of the SPU
     * side's fetches -- discriminates "t1 never wrote round 3" from "chain
     * never fetched round 3". Checked once per vblank tick, dumps on change.
     * s23: rows also decode SYMBOLICALLY via yz_jc_dec below -- the raw-hex-only
     * dump cost us two weeks of calling the 0x0000000800000012 park word "END"
     * when it is JTS (jump-to-self stopper, releasable by one store). */
    { static int jp = -1;
      if (jp < 0) { jp = getenv("YZ_JOBPEEK") ? 1 : 0;
          if (jp) fprintf(stderr, "[jobpeek] ARMED (YZ_JOBPEEK): watching 0x4019CA80-CB40 + hdr 0x4019C880\n"); }
      if (jp) {
          static uint64_t lasth = 0;
          uint64_t h = 1469598103934665603ull;   /* FNV-1a over both regions */
          for (uint32_t a = 0x4019CA80u; a < 0x4019CB40u; a += 4)
              { h ^= vm_read32(a); h *= 1099511628211ull; }
          for (uint32_t a = 0x4019C880u; a < 0x4019C8C0u; a += 4)
              { h ^= vm_read32(a); h *= 1099511628211ull; }
          if (h != lasth) {
              lasth = h;
              fprintf(stderr, "[jobpeek] hdr 0x4019C880:");
              for (uint32_t a = 0x4019C880u; a < 0x4019C8C0u; a += 4)
                  fprintf(stderr, " %08X", vm_read32(a));
              fprintf(stderr, "\n");
              for (uint32_t row = 0x4019CA80u; row < 0x4019CB40u; row += 0x20) {
                  char dec[128]; size_t dp = 0; dec[0] = 0;
                  fprintf(stderr, "[jobpeek] cmd 0x%08X:", row);
                  for (uint32_t a = row; a < row + 0x20; a += 8) {
                      uint64_t w = ((uint64_t)vm_read32(a) << 32) | vm_read32(a + 4);
                      fprintf(stderr, " %08X %08X",
                              (uint32_t)(w >> 32), (uint32_t)w);
                      /* SPURS jobchain command decode (s23). Opcode table per
                       * RPCS3 cellSpurs.h:266-281 (CELL_SPURS_JOB_OPCODE_*):
                       * low 3 bits = class; class-2 sub in bits 3-6 (SYNC=0x02,
                       * LWSYNC=0x12, JTS=bit35|LWSYNC -- a STOPPER released by
                       * overwriting the word); class-7 sub in bits 3-6
                       * (GUARD=1, SET_LABEL=2, RET=14, END=15, ABORT=0). */
                      const char* t; char tb[24];
                      uint32_t lo3 = (uint32_t)(w & 7u);
                      if (w == 0)            t = "NOP";
                      else if (w == 5)       t = "FLUSH";
                      else if (lo3 == 2) {
                          uint64_t s = w & ~0x800000000ull;
                          if (s == 0x02)      t = "SYNC";
                          else if (s == 0x12) t = (w & 0x800000000ull) ? "JTS" : "LWSYNC";
                          else                t = "SYNC?";
                      } else if (lo3 == 7) {
                          uint32_t sub = (uint32_t)((w >> 3) & 0xFu);
                          if (sub == 15)      t = "END";
                          else if (sub == 14) t = "RET";
                          else if (sub == 1) { snprintf(tb, sizeof(tb), "GUARD@%04X",
                                                        (uint32_t)(w & ~127ull) & 0xFFFFu); t = tb; }
                          else if (sub == 0)  t = "ABORT";
                          else                t = "CMD7?";
                      } else {
                          static const char* cn[8] =
                              { "JOB", "RESET_PC", "?", "NEXT", "CALL", "?", "JOBLIST", "?" };
                          snprintf(tb, sizeof(tb), "%s@%04X", cn[lo3],
                                   (uint32_t)(w & ~7ull) & 0xFFFFu); t = tb;
                      }
                      dp += (size_t)snprintf(dec + dp, sizeof(dec) - dp, " %s",
                                             t);
                      if (dp >= sizeof(dec) - 1) break;
                  }
                  fprintf(stderr, " |%s\n", dec);
              }
              fflush(stderr);
          }
      }
    }

    /* YZ_CNTGATE (s23): the audio-round COUNT GATE probe. RE (caller-chain map):
     * every frame t1's tick chain calls func_00E5F248, which reads the pending
     * count at G+0x18 (G = *(*(game_toc-0x7B28)+0x20), read at pc 0x00E5F27C)
     * and passes it to func_00E5F094 -- which writes NO JOB and returns when
     * count==0 (early exit pc 0xE5F0D8; a FULL ring would usleep-block instead).
     * Round 3's missing JOB ⇒ count was 0. This probe resolves G, prints the
     * count EA once (for a follow-up YZ_WATCH_WR), and logs count + the round
     * counter @0x015F4410 on change -- one boot names the starved queue and
     * whether the enqueuer ever runs again. */
    { static int cg = -1;
      if (cg < 0) { cg = getenv("YZ_CNTGATE") ? 1 : 0;
          if (cg) fprintf(stderr, "[cntgate] ARMED (YZ_CNTGATE)\n"); }
      if (cg && g_yz_game_toc) {
          static uint32_t lastc = 0xFFFFFFFFu, lastr = 0xFFFFFFFFu;
          static int announced = 0;
          uint32_t O = vm_read32(g_yz_game_toc - 0x7B28u);
          uint32_t G = (O >= 0x10000u && O < 0xE0000000u) ? vm_read32(O + 0x20u) : 0;
          if (G >= 0x10000u && G < 0xE0000000u) {
              if (!announced) { announced = 1;
                  fprintf(stderr, "[cntgate] O=0x%08X G=0x%08X count-EA=0x%08X\n",
                          O, G, G + 0x18u); fflush(stderr); }
              uint32_t c = vm_read32(G + 0x18u);
              uint32_t r = vm_read32(0x015F4410u);
              /* s23 boot5 ADDENDUM: the round counter moved only 1->2 all boot =
               * the DRIVER (func_00A9F8AC) itself ran exactly twice, so the gate
               * is ABOVE the count read. Also watch the driver's own guard field
               * (absolute-EA check that skips its body when 0, per the chain RE)
               * and the fields it clears each round. */
              uint32_t gf = vm_read32(0x014EC864u);
              uint32_t f1c = vm_read32(0x015F441Cu), f20 = vm_read32(0x015F4420u);
              static uint32_t lgf = 0xFFFFFFFFu, lf1c = 0xFFFFFFFFu, lf20 = 0xFFFFFFFFu;
              if (c != lastc || r != lastr || gf != lgf || f1c != lf1c || f20 != lf20) {
                  lastc = c; lastr = r; lgf = gf; lf1c = f1c; lf20 = f20;
                  fprintf(stderr, "[cntgate] round=0x%02X count=0x%08X guard=0x%08X f1C=0x%08X f20=0x%08X\n",
                          r & 0xFFu, c, gf, f1c, f20);
                  fflush(stderr); }
          }
      }
    }

    /* YZ_FLIPTRACE add-on (s21, the phase-2 consumer park): if GET has been
     * FROZEN for ~4 s with PUT ahead (unconsumed commands), dump the FIFO words
     * around GET once per freeze episode -- classifies the silent park (stopper
     * jump-to-self vs CALL vs method packet) that no existing log catches
     * (boot 4: GET=0x4C24 PUT=0x18EF8 frozen, zero consumer prints). */
    if (yz_ft_on()) {
        static uint32_t pg = 0xFFFFFFFFu; static unsigned frozen = 0;
        static uint32_t dumped_at = 0xFFFFFFFFu;
        uint32_t get = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_GET) & 0xFFFFFF;
        uint32_t put = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT) & 0xFFFFFF;
        if (get == pg && get != put) frozen++; else { frozen = 0; pg = get; }
        if (frozen == 256 && dumped_at != get) {   /* ~4 s at 62.5 Hz */
            dumped_at = get;
            yz_ft("FIFOPARK GET=0x%06X PUT=0x%06X frozen ~4s; words around GET:",
                  get, put);
            for (int32_t off = -0x20; off <= 0x3C; off += 4) {
                uint32_t io = get + (uint32_t)off;
                if ((int32_t)(get + off) < 0) continue;
                uint32_t ea = yz_rsx_io_to_ea(io);
                fprintf(stderr, "[ft]   io 0x%06X = %08X%s\n",
                        io, ea ? vm_read32(ea) : 0xDEADDEAD,
                        io == get ? "  <-- GET" : "");
            }
            fflush(stderr);
        }
    }

    /* SPURS DISPATCH DIAG (env YZ_TASK_TRACE, 2026-06-16b): once the game has
     * created its render task, dump the SPURS instance's workload-ready state so
     * we can tell if CreateTask2 set wklReadyCount1 (kernel schedules a workload
     * when wklReadyCount1[wid] > 0). taskset->spurs is at +0x60 (Taskset2) or
     * +0x78 (JobChain-style); try both. CellSpurs: wklReadyCount1[16]@0x00,
     * wklEnabled@0xB0, wklStatus1@0x90. */
    /* COHERENCE TEST (env YZ_FORCE_RC, every vblank): bump the taskset workload's
     * wklReadyCount1 INSIDE the SPU lock-line so it survives the kernel's PUTLLC.
     * The task is already pending_ready; if the kernel then schedules wid -> the
     * policy promotes pending->ready and runs gs_task. Confirms the missing
     * bootstrap step is the (coherent) readyCount bump CreateTask2 should do. */
    if (g_yz_spurs_taskset && getenv("YZ_FORCE_RC")) {
        uint32_t ts = g_yz_spurs_taskset, sp = vm_read32(ts + 0x64u);
        uint32_t wid = vm_read32(ts + 0x74u) & 0xFu;
        if (sp >= 0x10000u && sp < 0xE0000000u) {
            /* wklReadyCount1[wid] = 1 -- the kernel scheduling gate (select needs
             * readyCount > contention; cellSpursSpu.cpp:521). NOTE: use vm_write8
             * directly -- it already serializes through the lock-line in
             * VM_WRITE_COH. Wrapping in spu_lockline_lock() nests the
             * non-recursive lock and DEADLOCKS the vblank thread (the old bug that
             * made this force silently never land -> readyCount stayed 0). */
            uint8_t rc_before = vm_read8(sp + (wid & 0xFu));
            vm_write8(sp + (wid & 0xFu), 1);
            uint8_t rc_after = vm_read8(sp + (wid & 0xFu));
            /* re-kick the system service to re-scan workloads (sysSrvMessage@0x72,
             * sysSrvMsgUpdateWorkload@0xBD): set the low 5 SPU bits. */
            vm_write8(sp + 0x72u, (uint8_t)(vm_read8(sp + 0x72u) | 0x1Fu));
            vm_write8(sp + 0xBDu, (uint8_t)(vm_read8(sp + 0xBDu) | 0x1Fu));
            { static unsigned fc=0; if((fc++ & 63u)==0)
                fprintf(stderr, "[force-rc] sp=0x%08X wid=%u readyCount[wid] %u->%u (then re-read %u)\n",
                        sp, wid, rc_before, rc_after, vm_read8(sp + (wid & 0xFu))); }
        }
    }

    /* pt35 VALIDATION (env YZ_FORCE_CODEC): force the cri_audio codec workload
     * (wid 3) selectable. wid3 is runnable + has priority + readyCount=1, but its
     * wklCurrentContention[3] is pinned at 1 (== maxContention), so the kernel's
     * select gate `maxContention > contention` (cellSpursSpu.cpp:331) fails. Clear
     * contention[3] + pending[3] and (re)assert readyCount[3] each vblank so the
     * kernel selects wid3 -> policy -> StartTask -> spu_task_launch runs cri_audio.
     * Validates the lift+launch+decode chain while the proper contention-accounting
     * fix is pending. Offsets: wklReadyCount1@+0x00, wklCurrentContention@+0x20,
     * wklPendingContention@+0x30 (u8[16], per-wid). */
    if (g_yz_spurs_taskset && getenv("YZ_FORCE_CODEC")) {
        uint32_t sp = vm_read32(g_yz_spurs_taskset + 0x64u);
        if (sp >= 0x10000u && sp < 0xE0000000u) {
            vm_write8(sp + 0x23u, 0);   /* wklCurrentContention[3] = 0 */
            vm_write8(sp + 0x33u, 0);   /* wklPendingContention[3]  = 0 */
            vm_write8(sp + 0x03u, 1);   /* wklReadyCount1[3]        = 1 */
            vm_write8(sp + 0x72u, (uint8_t)(vm_read8(sp + 0x72u) | 0x1Fu));
            vm_write8(sp + 0xBDu, (uint8_t)(vm_read8(sp + 0xBDu) | 0x1Fu));
            { static unsigned fc=0; if((fc++ & 127u)==0)
                fprintf(stderr, "[force-codec] sp=0x%08X cleared wklCurCont[3], rc[3]=1\n", sp); }
        }
    }

    /* pt35: once the codec taskset exists, dump its enabled-bitset + task_info[].elf
     * to settle whether CreateTaskWithAttr populated the codec ELF (0x012B4980) or
     * left task_info empty (-> the elf=0 the policy reads at StartTask). CellSpursTaskset:
     * enabled@0x30, task_info[128]@0x80 (48 bytes each), TaskInfo.elf EA low32 @+0x14. */
    /* pt35 FIX (env YZ_FIXRUN): our lifted cellSpursCreateTask wrongly sets the
     * codec task's `running` bit at creation (RPCS3 sets only enabled + pending_ready;
     * cellSpurs.cpp:4139/4187). With running set, the policy's SELECT_TASK
     * (readyButNotRunning = (signalled|ready|pready) & ~running) never picks it, so
     * the codec is never dispatched (taskId=-1, elf=0 at StartTask). Clear running
     * for the codec taskset ONCE after creation -- safe because nothing reads that
     * taskset until wid3 is selected, which this very bit is blocking. */
    if (g_yz_codec_taskset && getenv("YZ_FIXRUN")) {
        static int fixed = 0;
        if (!fixed) {
            uint32_t ts = g_yz_codec_taskset, run = vm_read32(ts + 0x00u);
            if (run & 0x80000000u) {
                vm_write32(ts + 0x00u, run & 0x7FFFFFFFu);   /* clear running[task0] */
                fixed = 1;
                fprintf(stderr, "[fixrun] codec taskset 0x%08X running 0x%08X -> 0x%08X\n",
                        ts, run, run & 0x7FFFFFFFu); fflush(stderr);
            }
        }
    }

    if (g_yz_codec_taskset && getenv("YZ_TASK_TRACE")) {
        /* pt35e A/B test: the SPURS kernel re-selects a taskset workload iff
         * wklReadyCount1[wid] != 0 OR wklSignal1 bit for wid is set (cellSpursSpu.cpp:333).
         * SPURS instance @ 0x40197C80: wklReadyCount1[0x10]@+0x00 (wid3 = low byte of the
         * +0x00 BE word), wklSignal1 (BE u16)@+0x70 (wid3 bit = 0x8000>>3 = 0x1000). If
         * either is set in MAIN MEMORY -> the PPU DID make wid3 eligible and the idle SPU
         * just missed the wake (gate = B). If neither ever sets -> the create never bumps
         * eligibility (gate = A, the unimplemented readyCount/wklSignal path). */
        const uint32_t SP = 0x40197C80u;
        uint32_t rc1 = vm_read32(SP + 0x00u);    /* wklReadyCount1 wid0..3 (BE) */
        uint32_t sig1w = vm_read32(SP + 0x70u);  /* wklSignal1 (BE u16) in high half */
        unsigned rcWid3 = rc1 & 0xFFu;
        unsigned sigWid3 = ((sig1w >> 16) & 0x1000u) ? 1u : 0u;
        /* pt35e: cellSpursSendWorkloadSignal only sets wklSignal1 if wklState(wid)==RUNNABLE
         * (cellSpurs.cpp:2805). wklState1[0x10]@SPURS+0x80 (wid3 = low byte of +0x80 word);
         * RUNNABLE=2. If wid3 never reaches 2, that guard fails -> signal never sent. */
        uint32_t wkst1 = vm_read32(SP + 0x80u);
        unsigned stWid3 = wkst1 & 0xFFu;
        static int runnable = 0;
        if (!runnable && stWid3 == 2u) { runnable = 1;
            fprintf(stderr, "[codec-runnable] *** wid3 wklState reached RUNNABLE(2) -- SendWorkloadSignal's state guard would pass ***\n"); fflush(stderr); }
        static int elig = 0;
        if (!elig && (rcWid3 || sigWid3)) { elig = 1;
            fprintf(stderr, "[codec-eligible] *** wid3 readyCount=%u wklSignal=%u -> PPU DID make wid3 eligible (gate = B: idle SPU missed the wake) ***\n",
                    rcWid3, sigWid3); fflush(stderr); }
        /* pt35e FIX TEST (env YZ_WKLSIG): the LLE task-creation path sets pending_ready
         * but never lands SendWorkloadSignal -> wklSignal1[wid3] stays 0 -> the kernel
         * never selects the codec taskset. Do the one missing step (RPCS3 cellSpurs.cpp:2812:
         * sig |= 0x8000 >> (wid%16)) once the task is pending. The SPU kernel polls this
         * line via GETLLAR (YZ_FRC proves it), so no wakeup is needed. If this unblocks the
         * codec, the workload-signal gap is confirmed as THE root + replaces YZ_FRC/etc. */
        if (getenv("YZ_WKLSIG") && stWid3 == 2u && !sigWid3 &&
            vm_read32(g_yz_codec_taskset + 0x20u) != 0) {
            /* ROOT FIX TEST (pt35e): the codec's task_start->SendWorkloadSignal(wid3) BAILED
             * because wid3's workload wasn't enabled/RUNNABLE yet when the task was created
             * (the task is created before the SPU kernel processes the workload-add). RE-SEND
             * the missed signal now that the guard would pass (wklState[wid3]==RUNNABLE + a
             * pending task). This is EXACTLY the SendWorkloadSignal that should have fired;
             * the game's real taskset setup is left intact, so the codec should dispatch
             * CLEANLY (unlike the earlier inconsistent ready/readyCount force that crashed). */
            /* The kernel's lifted SELECT acts on readyCount (drove contention before) but
             * not on wklSignal alone. Set BOTH, coherently, leaving the game's taskset
             * bitsets INTACT (no pReady/ready manipulation -> the policy's SELECT_TASK runs
             * normally -> should dispatch cleanly). Re-assert while pending so the workload
             * stays selectable across the multi-step dispatch. */
            static int n = 0;
            if (n < 200) { n++;
                uint32_t sg = vm_read32(SP + 0x70u);
                vm_write32(SP + 0x70u, sg | (0x1000u << 16));      /* wklSignal1[wid3] */
                uint32_t rc = vm_read32(SP + 0x00u);
                vm_write32(SP + 0x00u, (rc & 0xFFFFFF00u) | 1u);   /* wklReadyCount1[wid3]=1 */
                if (n == 1) { fprintf(stderr, "[wklsig] RE-SEND signal+readyCount[wid3] (RUNNABLE+pending, bitsets intact)\n"); fflush(stderr); } }
        }
        static int cd = 0;
        if (cd < 30) { cd++;
            uint32_t ts = g_yz_codec_taskset;
            uint32_t enabled = vm_read32(ts + 0x30u), wid = vm_read32(ts + 0x74u);
            /* Full bitset state (CellSpursTaskset): running@0x00 ready@0x10
             * pending_ready@0x20 enabled@0x30 signalled@0x40 waiting@0x50.
             * RPCS3 after create+start: enabled+pending_ready set, running/ready=0. */
            fprintf(stderr, "[codec-ts] ts=0x%08X wid=%u run=0x%08X rdy=0x%08X pReady=0x%08X en=0x%08X sig=0x%08X wait=0x%08X | SPURS rc1=0x%08X sig1=0x%04X cont=0x%08X rcWid3=%u sigWid3=%u",
                    ts, wid, vm_read32(ts+0x00u), vm_read32(ts+0x10u), vm_read32(ts+0x20u),
                    enabled, vm_read32(ts+0x40u), vm_read32(ts+0x50u),
                    rc1, (sig1w >> 16) & 0xFFFFu, vm_read32(SP+0x20u), rcWid3, sigWid3);
            fprintf(stderr, " wkState1=0x%08X stWid3=%u(2=RUNNABLE)", wkst1, stWid3);
            /* pt35e ROOT test: CellSpursTaskset.spurs @ +0x60 (be64, EA low word @ +0x64).
             * task_start does `spurs = taskset->spurs; cellSpursSendWorkloadSignal(spurs,wid)`.
             * If this isn't 0x40197C80, SendWorkloadSignal gets a bad ptr + bails -> no signal. */
            fprintf(stderr, " tsSpurs=0x%08X(expect 0x40197C80)", vm_read32(ts + 0x64u));
            for (int t = 0; t < 4; t++) {
                uint32_t elf = vm_read32(ts + 0x80u + (uint32_t)t*0x30u + 0x14u);
                fprintf(stderr, " t%d.elf=0x%08X", t, elf);
            }
            fprintf(stderr, "\n"); fflush(stderr);
        }
    }

    if (g_yz_spurs_taskset && getenv("YZ_TASK_TRACE")) {
        static unsigned dt = 0;
        if ((dt++ & 63u) == 0) {
            uint32_t ts = g_yz_spurs_taskset;
            uint32_t sp = vm_read32(ts + 0x64u);            /* CellSpursTaskset2.spurs (+0x60 be64) */
            uint32_t wid = vm_read32(ts + 0x74u);           /* taskset workload id */
            /* CellSpursTaskset2: running_set@0x00 ready_set@0x10 enabled_set@0x30 */
            uint32_t run0 = vm_read32(ts + 0x00u), rdy0 = vm_read32(ts + 0x10u);
            uint32_t pnd0 = vm_read32(ts + 0x20u);   /* pending_ready */
            uint32_t ena0 = vm_read32(ts + 0x30u), sig0 = vm_read32(ts + 0x40u);
            uint32_t rc0 = (sp>=0x10000u&&sp<0xE0000000u) ? vm_read32(sp + 0x00u) : 0;
            uint32_t rc4 = (sp>=0x10000u&&sp<0xE0000000u) ? vm_read32(sp + 0x04u) : 0;
            uint32_t en  = (sp>=0x10000u&&sp<0xE0000000u) ? vm_read32(sp + 0xB0u) : 0;
            uint32_t rcw32 = (sp>=0x10000u&&sp<0xE0000000u) ? vm_read32(sp + ((wid & 0xF) & ~3u)) : 0;
            uint8_t  rcwid = (uint8_t)(rcw32 >> (8u * (3u - ((wid & 0xF) & 3u))));   /* wklReadyCount1[wid], BE */
            /* wklInfo1[wid] @ SPURS+0xB00+wid*0x20: addr(be64)@+0x00, size@+0x10. Is
             * the taskset's WORKLOAD IMAGE (policy module) registered? */
            uint32_t wi = sp + 0xB00u + (wid & 0xF) * 0x20u;
            uint32_t wiAddr = (sp>=0x10000u&&sp<0xE0000000u) ? vm_read32(wi + 0x04u) : 0;
            uint32_t wiSize = (sp>=0x10000u&&sp<0xE0000000u) ? vm_read32(wi + 0x10u) : 0;
            uint8_t  wiUid  = (sp>=0x10000u&&sp<0xE0000000u) ? (uint8_t)(vm_read32(wi + 0x14u) >> 24) : 0;
            fprintf(stderr, "[spurs]   wklInfo1[%u]: addr=0x%08X size=0x%X uniqueId=%u%s\n",
                    wid, wiAddr, wiSize, wiUid, wiAddr ? "" : "  <-- NO IMAGE (policy module not registered)");
            /* pt30c: dump ALL enabled workloads' images -> is the cri_audio SPU codec
             * (0x012B4980) registered as a workload (=> SPURS-scheduling gate) or absent
             * (=> criMana never attached it)? Re-dump only when wklEnabled changes. */
            if (sp>=0x10000u && sp<0xE0000000u) {
                static uint32_t last_wkle = 0xFFFFFFFFu;
                uint32_t wkle = vm_read32(sp + 0xB0u);
                if (wkle != last_wkle) { last_wkle = wkle;
                    fprintf(stderr, "[spurs] WORKLOADS wklEnabled=0x%08X (seeking cri_audio 0x012B4980):\n", wkle);
                    for (int w = 0; w < 16; w++) {
                        if (!(wkle & (1u << (31 - w)))) continue;
                        uint32_t wii = sp + 0xB00u + (uint32_t)w * 0x20u;
                        uint32_t a  = vm_read32(wii + 0x04u);
                        uint32_t sz = vm_read32(wii + 0x10u);
                        const char* tag = (a == 0x012B4980u) ? "  <== cri_audio CODEC" :
                                          (a >= 0x012B0000u && a < 0x012F0000u) ? "  <== CRI image range" : "";
                        fprintf(stderr, "[spurs]   wkl[%2d] image=0x%08X size=0x%X%s\n", w, a, sz, tag);
                    }
                    fflush(stderr);
                }
            }
            /* pt33: readyCount + state per enabled workload. The kernel selects by
             * readyCount; wid 3 (cri_audio taskset) is never selected -> is its
             * readyCount 0 (never ACTIVATED by the service) or set-but-ignored? */
            if (sp>=0x10000u && sp<0xE0000000u) {
                uint32_t wkle2 = vm_read32(sp + 0xB0u);
                char buf[300]; int o = 0; buf[0] = 0;
                for (int w = 0; w < 8 && o < 260; w++) {
                    if (!(wkle2 & (1u << (31 - w)))) continue;
                    uint8_t rc = (uint8_t)(vm_read32(sp + 0x00u + (uint32_t)(w & ~3)) >> (8*(3-(w&3))));
                    uint8_t st = (uint8_t)(vm_read32(sp + 0x80u + (uint32_t)(w & ~3)) >> (8*(3-(w&3))));
                    uint8_t mc = (uint8_t)(vm_read32(sp + 0x50u + (uint32_t)(w & ~3)) >> (8*(3-(w&3))));
                    o += snprintf(buf+o, sizeof(buf)-o, " wid%d[rc=%u st=%02X maxC=%u]", w, rc, st, mc);
                }
                static int rcq = 0;
                if (rcq < 50) { rcq++; fprintf(stderr, "[spurs] readyCounts:%s\n", buf); fflush(stderr); }
            }
            /* pt35 (fresh): dump the SELECTION GATES for EVERY enabled workload, not
             * just gs_task's wid=2. The codec (wid 3) lives in a DIFFERENT taskset,
             * so the per-taskset GATES line below never shows it. Read straight from
             * the shared SPURS struct (sp). Selection (cellSpursSpu.cpp:519) needs:
             * priority>0 (per-SPU) && maxContention>curContention && readyCount>0. */
            if (sp>=0x10000u && sp<0xE0000000u) {
                static int gq = 0;
                if (gq < 30) { gq++;
                    uint32_t wkle3 = vm_read32(sp + 0xB0u);
                    for (int w = 0; w < 8; w++) {
                        if (!(wkle3 & (1u << (31 - w)))) continue;
                        uint8_t cc = (uint8_t)(vm_read32(sp+0x20u+(uint32_t)(w&~3)) >> (8*(3-(w&3))));
                        uint8_t mc = (uint8_t)(vm_read32(sp+0x50u+(uint32_t)(w&~3)) >> (8*(3-(w&3))));
                        uint8_t st = (uint8_t)(vm_read32(sp+0x80u+(uint32_t)(w&~3)) >> (8*(3-(w&3))));
                        uint8_t stat=(uint8_t)(vm_read32(sp+0x90u+(uint32_t)(w&~3)) >> (8*(3-(w&3))));
                        uint8_t rcb = (uint8_t)(vm_read32(sp+0x00u+(uint32_t)(w&~3)) >> (8*(3-(w&3))));
                        uint32_t wiw = sp + 0xB00u + (uint32_t)w*0x20u;
                        uint32_t pHi = vm_read32(wiw + 0x18u), pLo = vm_read32(wiw + 0x1Cu);
                        bool prioNZ = (pHi|pLo) != 0;
                        bool selectable = prioNZ && (mc > cc) && (rcb > cc);
                        fprintf(stderr, "[gate] wid%d: rc=%u cur=%u max=%u state=0x%02X status=0x%02X "
                                "prio=%08X_%08X %s%s\n", w, rcb, cc, mc, st, stat, pHi, pLo,
                                prioNZ ? "" : "PRIO=0 ", selectable ? "<= SELECTABLE" : "(blocked)");
                    }
                    fflush(stderr);
                }
            }
            /* The kernel schedules wid only if runnable && priority>0 && maxContention>
             * contention (cellSpursSpu.cpp:519). wklCurrentContention@0x20, wklMaxContention
             * @0x50, wklStatus1@0x90, wklState1@0x80 (all u8[16]); wklInfo1[wid].priority@+0x18
             * (8 bytes, per-SPU). Find which gate is unset for the taskset. */
            if (sp>=0x10000u && sp<0xE0000000u) {
                uint8_t curCont = (uint8_t)(vm_read32(sp+0x20u+((wid&0xF)&~3u)) >> (8u*(3u-((wid&0xF)&3u))));
                uint8_t maxCont = (uint8_t)(vm_read32(sp+0x50u+((wid&0xF)&~3u)) >> (8u*(3u-((wid&0xF)&3u))));
                uint8_t status  = (uint8_t)(vm_read32(sp+0x90u+((wid&0xF)&~3u)) >> (8u*(3u-((wid&0xF)&3u))));
                uint8_t state   = (uint8_t)(vm_read32(sp+0x80u+((wid&0xF)&~3u)) >> (8u*(3u-((wid&0xF)&3u))));
                uint32_t prioHi = vm_read32(wi + 0x18u), prioLo = vm_read32(wi + 0x1Cu);
                uint8_t sysMsg = (uint8_t)(vm_read32(sp+0x70u) >> 8);       /* sysSrvMessage@0x72 */
                uint8_t sysUpd = (uint8_t)(vm_read32(sp+0xBCu) >> 16);      /* sysSrvMsgUpdateWorkload@0xBD */
                fprintf(stderr, "[spurs]   GATES wid=%u: maxContention=%u curContention=%u status=0x%02X "
                        "state=0x%02X priority=%08X_%08X | sysSrvMessage=0x%02X sysSrvMsgUpdateWorkload=0x%02X\n",
                        wid, maxCont, curCont, status, state, prioHi, prioLo, sysMsg, sysUpd);
            }
            /* What does the idle SPURS kernel busy-wait on? Show the hottest SPU
             * channels + the GETLLAR re-poll rate (delta since last sample). */
            {
                extern unsigned long g_spu_ch_rd[128], g_spu_ch_cnt[128], g_spu_getllar_n;
                extern uint32_t g_spu_getllar_ea;
                static unsigned long pr[128], pc[128], pg;
                const char* nm[31] = {0}; nm[0]="EvStat"; nm[3]="SigN1"; nm[4]="SigN2";
                nm[8]="RdDec"; nm[13]="MachStat"; nm[29]="RdInMbox";
                fprintf(stderr, "[spurs]   SPU idle-poll: GETLLAR +%lu (last ea=0x%08X) |",
                        g_spu_getllar_n - pg, g_spu_getllar_ea); pg = g_spu_getllar_n;
                for (int ch = 0; ch < 31; ch++) {
                    unsigned long dr = g_spu_ch_rd[ch] - pr[ch], dc = g_spu_ch_cnt[ch] - pc[ch];
                    pr[ch] = g_spu_ch_rd[ch]; pc[ch] = g_spu_ch_cnt[ch];
                    if (dr > 1000 || dc > 1000)
                        fprintf(stderr, " ch%d%s%s%s rd+%lu cnt+%lu", ch, nm[ch]?"(":"",
                                nm[ch]?nm[ch]:"", nm[ch]?")":"", dr, dc);
                }
                fprintf(stderr, "\n");
            }
            fprintf(stderr, "[spurs] ts=0x%08X wid=%u spurs=0x%08X | taskset: running=%08X "
                    "ready=%08X pending_ready=%08X enabled=%08X signalled=%08X | SPURS: "
                    "wklReadyCount1[wid]=%u wklEnabled=0x%08X\n",
                    ts, wid, sp, run0, rdy0, pnd0, ena0, sig0, rcwid, en);
            (void)rc0; (void)rc4;
            /* FORCE-READY PROBE (env YZ_FORCE_TASK): mark the enabled-but-not-ready
             * task ready (taskset.ready_set |= enabled_set) and request an SPU for
             * its workload (wklReadyCount1[wid]=1). If the kernel then schedules wid
             * -> it DMAs the taskset POLICY MODULE to LS and branches -> spu_indirect_
             * branch reports the FIRST unknown branch = the policy image to lift (or
             * runs gs_task entry 0x3050 directly). Confirms readyCount is the gate. */
            if (getenv("YZ_FORCE_TASK") && sp >= 0x10000u && sp < 0xE0000000u && ena0) {
                if (rdy0 != ena0) vm_write32(ts + 0x10u, ena0);   /* ready_set = enabled_set */
                if (rcwid == 0) {
                    uint32_t off = sp + ((wid & 0xF) & ~3u);
                    uint32_t cur = vm_read32(off);
                    uint32_t shift = 8u * (3u - ((wid & 0xF) & 3u));
                    cur = (cur & ~(0xFFu << shift)) | (1u << shift);  /* wklReadyCount1[wid]=1 */
                    vm_write32(off, cur);
                    fprintf(stderr, "[spurs] FORCE-READY: ready_set=enabled, wklReadyCount1[%u]=1\n", wid);
                }
            }
            fflush(stderr);
        }
    }

    uint64_t t = (uint64_t)GetTickCount() * 1000;
    uint64_t flip_ev = 0;
    for (int h = 0; h < 2; h++) {          /* PS3 has 2 active heads */
        uint32_t ha = yz_rsx_head_addr((uint32_t)h);
        vm_write64(ha + 0x30, vm_read64(ha + 0x30) + 1);  /* vBlankCount */
        vm_write32(ha + 0x1C, (uint32_t)t);               /* lastVTimeLow */
        vm_write32(ha + 0x3C, (uint32_t)(t >> 32));       /* lastVTimeHigh */

        /* Retire a queued flip on this head: present it, then run the
         * 0xFEC-equivalent completion (RPCS3 sys_rsx.cpp:856-868) -- set the
         * flip-done flag, stamp flipBufferId/lastFlipTime, and clear the flip
         * semaphore at label+0x10 (16 bytes, as real HW does). Clearing it
         * releases the consumer's `ACQUIRE label+0x10 == 0`.
         *
         * s37 fix (YZ_FLIP_ON_CONSUMER): when armed, the FIFO consumer thread
         * (yz_rsx_fifo_step) retires the flip instead, in FIFO order, so this
         * thread does ONLY vBlankCount + the VBLANK event below -- mirroring
         * RPCS3's explicit "wrong thread" 0xFED guard (sys_rsx.cpp:896-900).
         * The "!yz_flip_on_consumer() &&" guard is the ONLY change on this
         * path; default OFF leaves the InterlockedExchange/present/etc. below
         * byte-for-byte as before. Skipping this entirely (rather than just
         * not presenting) also guarantees g_rsx_flip_pending is consumed by
         * exactly one thread -- no double-retire race between the two. */
        if (!yz_flip_on_consumer() && InterlockedExchange(&g_rsx_flip_pending[h], 0)) {
            uint32_t buf = vm_read32(ha + 0x14);          /* lastQueuedBufferId */
            { static int n = 0; if (n < 12) { n++;
                fprintf(stderr, "[vbl] FLIP COMPLETE head=%d buf=%u -> clear label@0x10200010\n", h, buf); } }
            if (yz_ft_on())
                yz_ft("VBL-RETIRE head=%d buf=%u label-before=0x%08X",
                      h, buf, vm_read32(RSX_REPORTS + 0x10));
            yz_rsx_present(buf);
            vm_write32(ha + 0x10, buf);                   /* flipBufferId */
            vm_write32(ha + 0x08, vm_read32(ha + 0x08) | 0x80000000u); /* flip done */
            vm_write64(ha + 0x00, t);                     /* lastFlipTime */
            vm_write64(RSX_REPORTS + 0x10, 0);            /* flip sema (u128) = 0 */
            vm_write64(RSX_REPORTS + 0x18, 0);
            if (yz_ft_on()) yz_ft("VBL-CLEAR label=0 head=%d", h);
            /* Faithful flip-completion (replaces the YZ_FLIPADV band-aid's external
             * fence nudge): advance the counter the game's render throttle
             * func_00EAC46C polls (`while *(0x40C00000)+2 <= target`). Real RSX
             * bumps this once per presented flip; we present right here, so bump it
             * here -- exactly once per retired flip, ordered after present+label-clear. */
            vm_write32(0x40C00000u, vm_read32(0x40C00000u) + 1u);
            flip_ev |= (uint64_t)(0x8u << 1);             /* SYS_RSX_EVENT_FLIP_BASE<<1 */

            /* LAYER-1 ROOT BISECT (env YZ_GCMCTX_BISECT, pt48). Hypothesis: the
             * t7 _gcm_intr_thread stack-overflow recursion is the EVENT-DELIVERY
             * handshake -- libgcm's own private context predicates never clear,
             * so func_00EDC1F0 (flip-complete <=> GCMCTX+0x10==0) and
             * func_00EDD15C (re-arm latch, re-dispatches while GCMCTX+0x398==1)
             * keep re-firing. libgcm clears these ONLY when it fully processes a
             * FLIP event. Here we zero them ourselves on flip completion: if the
             * t7 recursion stops, the root is confirmed as the event handshake
             * (not the FIFO, not a wrong completion EA). GCMCTX = *(game_toc -
             * 0x5014); expected 0x01654130 -- log it once to re-confirm. */
            if (getenv("YZ_GCMCTX_BISECT") && g_yz_game_toc) {
                uint32_t gcmctx = vm_read32(g_yz_game_toc - 0x5014u);
                static int dumped = 0;
                if (!dumped) { dumped = 1;
                    fprintf(stderr, "[gcmctx-late] game_toc=0x%08X GCMCTX=0x%08X (LATE, at flip #1)\n",
                            g_yz_game_toc, gcmctx);
                    if (gcmctx >= 0x10000u && gcmctx < 0xE0000000u) {
                        const uint32_t fo[] = {0x00,0x04,0x08,0x0C,0x10,0x14,0x18,0x28,0x30,0x15C,0x164,0x168,0x398};
                        for (size_t i = 0; i < sizeof(fo)/sizeof(fo[0]); i++)
                            fprintf(stderr, "[gcmctx-late]   +0x%03X = 0x%08X\n", fo[i], vm_read32(gcmctx + fo[i]));
                    }
                    fflush(stderr);
                }
                if (gcmctx >= 0x10000u && gcmctx < 0xE0000000u) {
                    vm_write32(gcmctx + 0x10u, 0);    /* flip no longer pending */
                    vm_write32(gcmctx + 0x398u, 0);   /* clear the re-arm latch  */
                }
            }
        }
    }

    /* Deliver the RSX interrupt to Sony's _gcm_intr_thread via the event queue
     * (RPCS3 sys_rsx.cpp send_event): VBLANK every tick + FLIP on completion,
     * masked by driver_info.handlers (@+0x12C0). The intr thread runs the game's
     * registered vblank/flip handler, which advances its render loop (patches the
     * command-buffer pause it left behind). Without this, the handler never fires
     * and the game deadlocks waiting on its own flip fence. */
    if (g_rsx_event_port) {
        uint32_t handlers = vm_read32(RSX_DRIVER_INFO + 0x12C0);
        { static uint32_t lh = 0xFFFFFFFFu; if (handlers != lh) { lh = handlers;
            fprintf(stderr, "[sys_rsx] driver_info.handlers=0x%08X\n", handlers); } }
        /* On a flip, deliver the flip+vblank bits (Sony's LLE bit assignment is
         * not RPCS3's HLE enum -- handlers=0x6 => vblank bit1 + flip bit2); on a
         * plain vblank, just VBLANK (0x2).
         * s26 ROOT FIX (ledger #57 mode B): the old `flip_ev ? handlers` shape
         * delivered ALL registered bits -- written when handlers was 0x6, it
         * silently started including bit 0x80 (USER_CMD) once the game
         * registered its user handler (mask 0x86 at the movie boundary). Every
         * flip then spuriously dispatched the user handler with the
         * not-yet-written userCmdParam: handler(0) staged a val=0 work record
         * + consumed the pool task's wake -> the real cause's publish desynced
         * (publishes of 0 / stale re-publishes; s26ride8/9 [chain] hit#1
         * r3=0x0 before the first [ucmd]). USER_CMD is delivered EXCLUSIVELY
         * by the 0xEB00 method path. Kill-switch YZ_UCMD_ON_FLIP restores the
         * over-broadcast. */
        static int uof = -1;
        if (uof < 0) uof = getenv("YZ_UCMD_ON_FLIP") ? 1 : 0;
        uint64_t fmask = uof ? (uint64_t)handlers : ((uint64_t)handlers & 0x7Full);
        uint64_t ev = (flip_ev ? fmask : ((uint64_t)0x2 & handlers));
        if (ev) {
            /* Preserve vblank/flip causes across a transient full event queue.
             * Dropping one here can strand the guest's next-frame/title movie
             * transition even though rendering itself remains live. */
            int64_t r = yz_rsx_ev_send(ev);
            /* Log the result periodically: r==EBUSY (-0x... ) means the queue is
             * full -> the _gcm_intr_thread is NOT draining (the real problem). */
            static int n = 0; if (n < 8) { n++;
                fprintf(stderr, "[sys_rsx] vblank event ev=0x%llX -> send=%lld\n",
                        (unsigned long long)ev, (long long)r); }
        }
    }
}

/* ---------------------------------------------------------------------------
 * cellSysutilGetSystemParamInt(id, vm::ptr<s32>)
 *
 * The generic bridge would pass a raw host pointer and the libs HLE stores
 * host-endian (LE); the guest then loads the value with lwz and sees it
 * byte-swapped. Marshal through a host local and store with vm_write32,
 * which writes guest (big-endian) order.
 * -----------------------------------------------------------------------*/
extern "C" int32_t cellSysutilGetSystemParamInt(int32_t id, int32_t* value);

extern "C" void yz_ovr_cellSysutilGetSystemParamInt(ppu_context* ctx)
{
    int32_t  v  = 0;
    int32_t* hp = ctx->gpr[4] ? &v : NULL;
    int32_t  rc = cellSysutilGetSystemParamInt((int32_t)ctx->gpr[3], hp);
    if (hp && rc == 0)
        vm_write32(ctx->gpr[4], (uint32_t)v);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

/* ---------------------------------------------------------------------------
 * cellSaveDataListAutoLoad(version, errDialog, setList, setBuf, funcFixed,
 *                          funcStat, funcFile, container, userdata)
 *
 * This legacy nine-argument entry is the New Game gate used by Yakuza: Dead
 * Souls.  The generic import bridge only forwards the eight register arguments
 * and used to bind this NID to CELL_ENOSYS, so neither callback ran and the
 * title state machine waited forever on a black screen. The ninth argument
 * lives in the caller parameter-save area rather than a GPR.
 * -----------------------------------------------------------------------*/
extern "C" void yz_ovr_cellSaveDataListAutoLoad(ppu_context* ctx)
{
    const uint32_t set_list_ea = (uint32_t)ctx->gpr[5];
    const uint32_t set_buf_ea  = (uint32_t)ctx->gpr[6];
    const uint32_t userdata_ea = (uint32_t)vm_read64(ctx->gpr[1] + 0x70);

    const int32_t rc = cellSaveDataListAutoLoad(
        (uint32_t)ctx->gpr[3],
        (uint32_t)ctx->gpr[4],
        set_list_ea ? (CellSaveDataSetList*)(vm_base + set_list_ea) : NULL,
        set_buf_ea  ? (CellSaveDataSetBuf*)(vm_base + set_buf_ea) : NULL,
        (CellSaveDataFixedCallback)(uintptr_t)(uint32_t)ctx->gpr[7],
        (CellSaveDataStatCallback)(uintptr_t)(uint32_t)ctx->gpr[8],
        (CellSaveDataFileCallback)(uintptr_t)(uint32_t)ctx->gpr[9],
        (uint32_t)ctx->gpr[10],
        (void*)(uintptr_t)userdata_ea);

    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

/* Diagnostic wrapper: log the guest caller (lr) of every lwmutex destroy,
 * then forward to the libs implementation. */
extern "C" int32_t sys_lwmutex_destroy(void* lwmutex);

extern "C" void yz_ovr_sys_lwmutex_destroy(ppu_context* ctx)
{
    fprintf(stderr, "[import] sys_lwmutex_destroy(guest=0x%08X) from lr=0x%08llX\n",
            (uint32_t)ctx->gpr[3], (unsigned long long)ctx->lr);
    void* p = ctx->gpr[3] ? (void*)(vm_base + (uint32_t)ctx->gpr[3]) : NULL;
    ctx->gpr[3] = (uint64_t)(int64_t)sys_lwmutex_destroy(p);
}

/* ---------------------------------------------------------------------------
 * sys_spu_image_import(img*, src, type) -- user-level SPU ELF loader.
 *
 * Sony's libsre (LLE SPURS) calls this to import its embedded SPURS-kernel
 * SPU ELFs (ELF32 BE, EM_SPU; verified at image vaddrs 0x20380/0x20C00,
 * entries 0x818/0x848). Semantics from RPCS3 sys_spu_.cpp
 * sys_spu_image_import (DIRECT path) + sys_spu.h get_nsegs/fill:
 *   - per PT_LOAD: COPY segment {ls=p_vaddr, size=p_filesz, addr=src+p_offset}
 *     plus FILL {ls=p_vaddr+p_filesz, size=p_memsz-p_filesz, value=0} for bss;
 *   - per PT_NOTE (p_type 4): INFO segment {size=0x20, addr=src+p_offset+0x14};
 *   - any other p_type: ENOEXEC.
 * Guest structs (BE): sys_spu_image {type@0=USER, entry@4, segs@8, nsegs@12},
 * sys_spu_segment 0x18 bytes {type@0, ls@4, size@8, addr/value@0x10}.
 * The segment table is consumed at thread-group start when the kernel image
 * is deployed to SPU local store (7d).
 * -----------------------------------------------------------------------*/
extern "C" void yz_ovr_sys_spu_image_import(ppu_context* ctx)
{
    uint32_t img_ea = (uint32_t)ctx->gpr[3];
    uint32_t src    = (uint32_t)ctx->gpr[4];
    uint32_t type   = (uint32_t)ctx->gpr[5];

    const uint8_t* e = vm_base + src;
    if (!img_ea || !src || memcmp(e, "\x7f""ELF", 4) != 0 ||
        e[4] != 1 /*ELF32*/ || e[5] != 2 /*BE*/ ||
        ((e[18] << 8) | e[19]) != 23 /*EM_SPU*/) {
        fprintf(stderr, "[SPU] image_import img=0x%08X src=0x%08X type=%u: "
                "not an ELF32-BE EM_SPU image -> ENOEXEC\n", img_ea, src, type);
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ENOEXEC;
        return;
    }

    uint32_t entry     = vm_read32(src + 0x18);
    uint32_t phoff     = vm_read32(src + 0x1C);
    uint32_t phentsize = (uint32_t)((e[0x2A] << 8) | e[0x2B]);
    uint32_t phnum     = (uint32_t)((e[0x2C] << 8) | e[0x2D]);
    if (!phnum || phentsize < 32) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ENOEXEC;
        return;
    }

    /* Count segments (oracle: get_nsegs) */
    int32_t nsegs = 0;
    for (uint32_t i = 0; i < phnum; i++) {
        uint32_t ph      = src + phoff + i * phentsize;
        uint32_t p_type  = vm_read32(ph + 0x00);
        uint32_t p_filesz= vm_read32(ph + 0x10);
        uint32_t p_memsz = vm_read32(ph + 0x14);
        if (p_type != 1 && p_type != 4) {
            ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ENOEXEC;
            return;
        }
        if (p_type == 1 && p_memsz != p_filesz && p_filesz) nsegs += 2;
        else nsegs += 1;
    }

    /* Batch fixes item 12 (RPCS3 sys_spu.h:107): sys_spu_image segment count
     * is capped at 0x20 -- an image that would overflow that (malformed or
     * adversarial phdr table) must fail ENOMEM instead of driving an
     * oversized heap allocation / segment table. */
    if (nsegs <= 0 || nsegs > 0x20) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_ENOMEM;
        return;
    }

    uint32_t segs_ea = yz_heap_alloc((uint32_t)nsegs * 0x18u, 16);
    if (!segs_ea) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ENOMEM;
        return;
    }

    /* Fill segments (oracle: sys_spu_image::fill) */
    uint32_t s = segs_ea;
    for (uint32_t i = 0; i < phnum; i++) {
        uint32_t ph       = src + phoff + i * phentsize;
        uint32_t p_type   = vm_read32(ph + 0x00);
        uint32_t p_offset = vm_read32(ph + 0x04);
        uint32_t p_vaddr  = vm_read32(ph + 0x08);
        uint32_t p_filesz = vm_read32(ph + 0x10);
        uint32_t p_memsz  = vm_read32(ph + 0x14);
        if (p_type == 1) {
            if (p_filesz) {
                vm_write32(s + 0x00, 1);                 /* COPY */
                vm_write32(s + 0x04, p_vaddr);
                vm_write32(s + 0x08, p_filesz);
                vm_write32(s + 0x10, src + p_offset);
                s += 0x18;
            }
            if (p_memsz > p_filesz) {
                vm_write32(s + 0x00, 2);                 /* FILL */
                vm_write32(s + 0x04, p_vaddr + p_filesz);
                vm_write32(s + 0x08, p_memsz - p_filesz);
                vm_write32(s + 0x10, 0);
                s += 0x18;
            }
        } else { /* p_type == 4 */
            vm_write32(s + 0x00, 4);                     /* INFO */
            vm_write32(s + 0x04, 0);
            vm_write32(s + 0x08, 0x20);
            vm_write32(s + 0x10, src + p_offset + 0x14);
            s += 0x18;
        }
    }

    vm_write32(img_ea + 0x0, 0);                  /* SYS_SPU_IMAGE_TYPE_USER */
    vm_write32(img_ea + 0x4, entry);
    vm_write32(img_ea + 0x8, segs_ea);
    vm_write32(img_ea + 0xC, (uint32_t)nsegs);

    fprintf(stderr, "[SPU] image_import img=0x%08X src=0x%08X type=%u "
            "-> entry=0x%X nsegs=%d segs=0x%08X\n",
            img_ea, src, type, entry, nsegs, segs_ea);
    ctx->gpr[3] = 0;
}

/* sys_spu_image_close: our USER images keep their segment tables in the
 * runner bump heap (never reclaimed), so close is success/no-op. */
extern "C" void yz_ovr_sys_spu_image_close(ppu_context* ctx)
{
    fprintf(stderr, "[SPU] image_close img=0x%08X\n", (uint32_t)ctx->gpr[3]);
    ctx->gpr[3] = 0;
}

/* ---------------------------------------------------------------------------
 * Guest-aware printf family.
 *
 * The generic bridges pass vararg slots raw, so a guest %s pointer reaches
 * host vprintf and is dereferenced as a host address (observed live: Sony''s
 * libsre printed a warning with %s -> fault on guest 0x0202xxxx). This
 * formatter walks the format string itself and translates %s/%p arguments.
 *
 * Vararg slots per the PPC64 ELF ABI: integer args r3..r10, then the
 * caller''s parameter save area at r1+0x30 (8 doubleword home slots for
 * r3..r10, 9th arg onward at r1+0x70). Floats in prototype-less calls are
 * mirrored into the GPR image, so %f can reinterpret the GPR bits.
 * -----------------------------------------------------------------------*/
/* Copy a guest string into `tmp` without letting the host CRT touch
 * unmapped memory. A guest %s arg can point into an uncommitted region of
 * the 4 GB vm reservation (measured 2026-07-03: t10's _sys_printf died in
 * libucrt walking such a string -- the recurring silent long-boot death,
 * import thunk 372, crash rva resolving past CRT `remove`). Probes
 * committed-ness per page; stops at NUL, cap, or the first unreadable
 * page. Returns tmp (always NUL-terminated). */
static const char* yz_guest_str_safe(uint32_t ea, char* tmp, size_t cap)
{
    size_t o = 0;
    const uint8_t* p = (const uint8_t*)(vm_base + ea);
    uintptr_t page = 0;   /* last page verified committed+readable */
    while (o + 1 < cap) {
        uintptr_t cur = (uintptr_t)(p + o) & ~(uintptr_t)0xFFF;
        if (cur != page) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery((const void*)cur, &mbi, sizeof(mbi)) ||
                mbi.State != MEM_COMMIT ||
                (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                if (o == 0) { snprintf(tmp, cap, "(badptr:%08X)", ea); return tmp; }
                break;
            }
            page = cur;
        }
        uint8_t c = p[o];
        if (!c) break;
        tmp[o++] = (char)c;
    }
    tmp[o] = 0;
    return tmp;
}

static int yz_format_guest(char* out, size_t outsz, ppu_context* ctx,
                           uint32_t fmt_ea, int first_vararg /* 0 = r3 */)
{
    char ftmp[1024];   /* format walked byte-by-byte -- same badptr class */
    const char* f = yz_guest_str_safe(fmt_ea, ftmp, sizeof(ftmp));
    size_t o = 0;
    int ai = first_vararg;

    /* fetch integer-arg slot i (0-based from r3) */
    #define YZ_ARG(i) ((i) < 8 ? ctx->gpr[3 + (i)] \
                              : vm_read64(ctx->gpr[1] + 0x30 + (uint64_t)(i) * 8))

    while (*f && o + 1 < outsz) {
        if (*f != '%') { out[o++] = *f++; continue; }

        /* collect the conversion spec; '*' width/precision consumes an
         * integer argument (e.g. SPURS prints names with %.*s) */
        char spec[32];
        size_t sl = 0;
        spec[sl++] = *f++;                      /* '%' */
        while (*f && sl < sizeof(spec) - 16 &&
               (*f == '-' || *f == '+' || *f == ' ' || *f == '#' || *f == '0' ||
                (*f >= '0' && *f <= '9') || *f == '.' || *f == '*')) {
            if (*f == '*') {
                int w = (int)(int32_t)(uint32_t)YZ_ARG(ai++);
                sl += (size_t)snprintf(spec + sl, sizeof(spec) - sl, "%d", w);
                f++;
            } else {
                spec[sl++] = *f++;
            }
        }
        int l_count = 0;
        while (*f == 'l' || *f == 'h' || *f == 'z') {
            if (*f == 'l') l_count++;
            f++;                                /* length mods re-added below */
        }
        char conv = *f ? *f++ : 0;
        if (!conv) break;

        char piece[512];
        piece[0] = 0;
        uint64_t a;
        switch (conv) {
        case '%':
            piece[0] = '%'; piece[1] = 0;
            break;
        case 's': {
            a = YZ_ARG(ai++);
            /* guard: a %s arg below the loaded image range is not a string
             * pointer (mis-indexed vararg or genuine garbage); anything else
             * goes through the page-probing copy so an EA into uncommitted
             * vm space can't fault the host CRT (the t10 silent-death class). */
            char stmp[448];
            const char* s = ((uint32_t)a >= 0x10000u)
                          ? yz_guest_str_safe((uint32_t)a, stmp, sizeof(stmp))
                          : (a ? "(badptr)" : "(null)");
            spec[sl++] = 's'; spec[sl] = 0;
            snprintf(piece, sizeof(piece), spec, s);
            break;
        }
        case 'c':
            a = YZ_ARG(ai++);
            spec[sl++] = 'c'; spec[sl] = 0;
            snprintf(piece, sizeof(piece), spec, (int)a);
            break;
        case 'p':
            a = YZ_ARG(ai++);
            snprintf(piece, sizeof(piece), "0x%08X", (uint32_t)a);
            break;
        case 'd': case 'i':
            a = YZ_ARG(ai++);
            spec[sl++] = 'l'; spec[sl++] = 'l'; spec[sl++] = conv; spec[sl] = 0;
            /* PS3 long = 64-bit; plain int = 32 */
            snprintf(piece, sizeof(piece), spec,
                     l_count ? (long long)a : (long long)(int32_t)(uint32_t)a);
            break;
        case 'u': case 'x': case 'X': case 'o':
            a = YZ_ARG(ai++);
            spec[sl++] = 'l'; spec[sl++] = 'l'; spec[sl++] = conv; spec[sl] = 0;
            snprintf(piece, sizeof(piece), spec,
                     l_count ? (unsigned long long)a
                             : (unsigned long long)(uint32_t)a);
            break;
        case 'f': case 'F': case 'g': case 'G': case 'e': case 'E': {
            a = YZ_ARG(ai++);
            double d;
            memcpy(&d, &a, 8);                  /* GPR image of the double */
            spec[sl++] = conv; spec[sl] = 0;
            snprintf(piece, sizeof(piece), spec, d);
            break;
        }
        default:
            /* unknown conversion: emit it literally, consume one arg */
            a = YZ_ARG(ai++);
            snprintf(piece, sizeof(piece), "%%%c?(0x%llX)", conv,
                     (unsigned long long)a);
            break;
        }
        size_t pl = strlen(piece);
        if (pl > outsz - 1 - o) pl = outsz - 1 - o;
        memcpy(out + o, piece, pl);
        o += pl;
    }
    #undef YZ_ARG
    out[o] = 0;
    return (int)o;
}

extern "C" void yz_ovr__sys_printf(ppu_context* ctx)
{
    char buf[2048];
    int n = yz_format_guest(buf, sizeof(buf), ctx, (uint32_t)ctx->gpr[3], 1);
    printf("[PS3] %s", buf);
    fflush(stdout);
    ctx->gpr[3] = (uint64_t)(int64_t)n;
}

extern "C" void yz_ovr__sys_sprintf(ppu_context* ctx)
{
    char buf[2048];
    int n = yz_format_guest(buf, sizeof(buf), ctx, (uint32_t)ctx->gpr[4], 2);
    uint32_t dst = (uint32_t)ctx->gpr[3];
    if (dst) memcpy(vm_base + dst, buf, (size_t)n + 1);
    ctx->gpr[3] = (uint64_t)(int64_t)n;
}

extern "C" void yz_ovr__sys_snprintf(ppu_context* ctx)
{
    char buf[2048];
    int n = yz_format_guest(buf, sizeof(buf), ctx, (uint32_t)ctx->gpr[5], 3);
    uint32_t dst  = (uint32_t)ctx->gpr[3];
    uint32_t size = (uint32_t)ctx->gpr[4];
    if (dst && size) {
        uint32_t copy = (uint32_t)n < size - 1 ? (uint32_t)n : size - 1;
        memcpy(vm_base + dst, buf, copy);
        *(vm_base + dst + copy) = 0;
    }
    ctx->gpr[3] = (uint64_t)(int64_t)n;
}
