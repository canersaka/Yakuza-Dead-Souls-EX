/*
 * ps3recomp - SPU FLIGHT RECORDER implementation (phase 1)
 *
 * See spu_fltrec.h for the design rationale. This file owns the two ring
 * buffers, the env-gated arm-once init, the per-kind record builders, and
 * the dump-to-disk path (yz_fltrec_dump).
 */
#include "spu_fltrec.h"
#include "spu_context.h"   /* full spu_context (ls[], gpr[], pc, spu_id, image_id) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>

#if defined(_MSC_VER)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <direct.h>
#  define YZ_FR_MKDIR(p) _mkdir(p)
#  include <intrin.h>
#  define YZ_FR_RELAX() _mm_pause()
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  define YZ_FR_MKDIR(p) mkdir((p), 0755)
#  define YZ_FR_RELAX() ((void)0)
#endif

volatile int g_yz_fltrec_on = -1;
volatile int g_yz_fltrec_allctx = 0;

/* ---------------------------------------------------------------------------
 * Ring state
 * -------------------------------------------------------------------------*/
typedef struct {
    yz_fltrec_rec*    buf;
    uint64_t          capacity;   /* records; 0 == not allocated (recorder off) */
    _Atomic(uint64_t) cursor;     /* v2: atomic multi-writer global sequence (was a
                                     plain single-writer counter in phase 1 -- every
                                     lifted-SPU context can write this ring now that
                                     YZ_FLTREC_ALLCTX exists; the interlocked add is
                                     negligible next to this feature's existing cost) */
} yz_fr_main_ring;

typedef struct {
    yz_fltrec_rec*   buf;
    uint64_t         capacity;
    _Atomic(uint64_t) cursor;    /* multiple potential writer threads (foreign) */
} yz_fr_foreign_ring;

static yz_fr_main_ring    g_fr_main;
static yz_fr_foreign_ring g_fr_foreign;

static atomic_flag  s_fr_init_claimed = ATOMIC_FLAG_INIT;
static volatile int s_fr_init_complete = 0;
static atomic_int   s_fr_dump_count = 0;

/* ---------------------------------------------------------------------------
 * Arm-once init
 * -------------------------------------------------------------------------*/
static void fr_init_now(void)
{
    const char* e = getenv("YZ_FLTREC");
    int on = (e && *e && *e != '0') ? 1 : 0;
    int allctx = 0;

    if (on) {
        const char* mbs = getenv("YZ_FLTREC_MB");
        uint64_t mb = (mbs && *mbs) ? (uint64_t)strtoull(mbs, NULL, 10) : 1024ull;
        uint64_t bytes, cap, fcap;
        const char* ac = getenv("YZ_FLTREC_ALLCTX");
        allctx = (ac && *ac && *ac != '0') ? 1 : 0;
        if (mb < 1) mb = 1;
        bytes = mb * 1024ull * 1024ull;
        cap = bytes / sizeof(yz_fltrec_rec);
        if (cap < 1024) cap = 1024;

        g_fr_main.buf = (yz_fltrec_rec*)malloc((size_t)(cap * sizeof(yz_fltrec_rec)));
        if (!g_fr_main.buf) {
            fprintf(stderr, "[fltrec] alloc FAILED (%llu MB main ring) -- recorder stays OFF\n",
                    (unsigned long long)mb);
            fflush(stderr);
            on = 0;
        } else {
            g_fr_main.capacity = cap;
            atomic_store_explicit(&g_fr_main.cursor, 0, memory_order_relaxed);

            /* Foreign ring: fixed 4 MB regardless of YZ_FLTREC_MB -- the ctx
             * save/restore machinery is bursty but bounded (a handful of
             * restores per residency-era switch), not the main event volume. */
            fcap = (4ull * 1024 * 1024) / sizeof(yz_fltrec_rec);
            g_fr_foreign.buf = (yz_fltrec_rec*)malloc((size_t)(fcap * sizeof(yz_fltrec_rec)));
            if (g_fr_foreign.buf) {
                g_fr_foreign.capacity = fcap;
                atomic_store_explicit(&g_fr_foreign.cursor, 0, memory_order_relaxed);
            } else {
                g_fr_foreign.capacity = 0;
            }

            fprintf(stderr,
                    "[fltrec] ARMED v2 main=%lluMB(%llu recs, %d-byte records) foreign=4MB(%llu recs) "
                    "sync_interval=%u allctx=%s consumer_ctx=%p (unset YZ_FLTREC to disable)\n",
                    (unsigned long long)mb, (unsigned long long)cap, (int)sizeof(yz_fltrec_rec),
                    (unsigned long long)g_fr_foreign.capacity, YZ_FR_SYNC_INTERVAL,
                    allctx ? "ON (YZ_FLTREC_ALLCTX)" : "off (consumer ctx only)",
                    (void*)g_yz_consumer_ctx);
            fflush(stderr);
        }
    }
    g_yz_fltrec_allctx = allctx;
    g_yz_fltrec_on = on;
}

int yz_fltrec_enabled(void)
{
    if (s_fr_init_complete) return g_yz_fltrec_on;
    if (!atomic_flag_test_and_set_explicit(&s_fr_init_claimed, memory_order_acq_rel)) {
        fr_init_now();
        s_fr_init_complete = 1;
    } else {
        while (!s_fr_init_complete) YZ_FR_RELAX();
    }
    return g_yz_fltrec_on;
}

/* ---------------------------------------------------------------------------
 * Main-ring append. v2 (s42): ALWAYS an atomic-cursor multi-writer ring (was
 * single-writer-only in phase 1, back when only g_yz_consumer_ctx's pinned
 * thread ever passed the gate); YZ_FLTREC_ALLCTX now lets every registered
 * lifted-SPU context's own thread call these concurrently. Each record's
 * ring slot is assigned by one shared atomic fetch-add, so physical ring
 * position is global arrival order -- the same guarantee the FOREIGN ring
 * always had.
 * -------------------------------------------------------------------------*/
static uint64_t fr_emit_sync_main(spu_context* ctx)
{
    yz_fltrec_rec r;
    uint64_t seq;
#if defined(_MSC_VER)
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    {
        uint64_t v = (uint64_t)qpc.QuadPart;
        r.addr = (uint32_t)(v & 0xFFFFFFFFu);
        r.value = (uint32_t)(v >> 32);
    }
#else
    {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t v = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        r.addr = (uint32_t)(v & 0xFFFFFFFFu);
        r.value = (uint32_t)(v >> 32);
    }
#endif
    r.pc = 0; r.kind = (uint8_t)YZ_FR_SYNC; r.aux = 0; r.len_or_ch = 0;
    r.spu_id = ctx ? ctx->spu_id : 0;
    seq = atomic_fetch_add_explicit(&g_fr_main.cursor, 1, memory_order_relaxed);
    r.seq = seq;
    g_fr_main.buf[seq % g_fr_main.capacity] = r;
    return seq;
}

/* Assigns *r a ring slot (seq + spu_id filled in here) and writes it. If the
 * assigned seq crosses a YZ_FR_SYNC_INTERVAL boundary (checked on the seq
 * this call itself was given -- interval must stay a power of two, no shared
 * counter needed), also emits a SYNC record via its OWN separate fetch-add.
 * In multi-writer (ALLCTX) mode the SYNC record can therefore land one slot
 * AFTER the record that triggered it (a concurrent writer may claim the
 * intervening slot first) -- documented in the header comment as a harmless
 * approximation; the decoder already treats SYNC-to-time mapping as
 * carry-forward, not exact. */
static inline void fr_push_main(yz_fltrec_rec* r, spu_context* ctx)
{
    uint64_t seq;
    if (g_fr_main.capacity == 0) return;
    seq = atomic_fetch_add_explicit(&g_fr_main.cursor, 1, memory_order_relaxed);
    if ((seq & (YZ_FR_SYNC_INTERVAL - 1)) == 0) fr_emit_sync_main(ctx);
    r->seq = seq;
    r->spu_id = ctx ? ctx->spu_id : 0;
    g_fr_main.buf[seq % g_fr_main.capacity] = *r;
}

/* s42: per-pc mute list (YZ_FLTREC_MUTE_PCS="11B0,5AC", hex, comma-separated,
 * max 8): records emitted AT a muted pc are sampled 1/256 per SPU instead of
 * recorded raw. Rationale: in allctx mode the two spin floods (channel
 * programming at 0x11B0, reservation retry at 0x5AC) were ~95% of ring traffic
 * and shrank the visible window to ~2s (scratch/s42_ordering_diff.md §5).
 * Unset = record everything (behavior unchanged). Sampling counters are
 * per-SPU and deliberately non-atomic — approximate 1/256 is fine for a
 * diagnostic; exact rates are recoverable only to that approximation. */
static uint32_t g_fr_mute_lo[16], g_fr_mute_hi[16];
static int      g_fr_mute_n = -1;            /* -1 = env not parsed yet */
static int      g_fr_mute_skip_img0 = 0;     /* YZ_FLTREC_MUTE_SKIP_IMG0 */
static uint32_t g_fr_mute_ctr[8][16];        /* [spu_id&7][entry_idx] */

/* Entries are single pcs ("11B0") or inclusive ranges ("1170-1200"). Ranges
 * exist because single-pc muting on an unrolled hot loop just promotes the
 * neighboring instruction to loudest at equal weight (measured, the s42
 * flood census — 0x11B0 muted -> 0x1178 took over at 21% of the ring). */
static int fr_muted(spu_context* ctx, uint32_t pc)
{
    int i;
    if (g_fr_mute_n < 0) {
        const char* s = getenv("YZ_FLTREC_MUTE_PCS");
        g_fr_mute_skip_img0 = getenv("YZ_FLTREC_MUTE_SKIP_IMG0") ? 1 : 0;
        g_fr_mute_n = 0;
        if (s && *s) {
            while (*s && g_fr_mute_n < 16) {
                uint32_t lo = (uint32_t)strtoul(s, (char**)&s, 16), hi = lo;
                if (*s == '-') { s++; hi = (uint32_t)strtoul(s, (char**)&s, 16); }
                g_fr_mute_lo[g_fr_mute_n] = lo;
                g_fr_mute_hi[g_fr_mute_n] = hi;
                g_fr_mute_n++;
                while (*s == ',' || *s == ' ') s++;
            }
            fprintf(stderr, "[fltrec] MUTE armed: %d entr%s, 1/256 sampling%s\n",
                    g_fr_mute_n, g_fr_mute_n == 1 ? "y" : "ies",
                    g_fr_mute_skip_img0 ? ", image-0 exempt" : "");
        }
    }
    /* image-0 exemption: the gs_task/consumer image is the SIGNAL (feed 0x7AC4,
     * sweep 0x352C-0x3568, apply 0xB088) and its pc span overlaps the kernel/
     * policy flood bands — never sample it away when the exemption is armed. */
    if (g_fr_mute_skip_img0 && ctx && ctx->image_id == 0) return 0;
    for (i = 0; i < g_fr_mute_n; i++) {
        if (pc >= g_fr_mute_lo[i] && pc <= g_fr_mute_hi[i]) {
            uint32_t si = (ctx ? ctx->spu_id : 0) & 7u;
            return (++g_fr_mute_ctr[si][i] & 0xFFu) != 0;  /* keep every 256th */
        }
    }
    return 0;
}

void yz_fltrec_store32(spu_context* ctx, uint32_t lsa, uint32_t val)
{
    yz_fltrec_rec r;
    if (g_fr_main.capacity == 0) return;
    if (fr_muted(ctx, ctx->pc)) return;
    r.pc = ctx->pc; r.kind = (uint8_t)YZ_FR_STORE32; r.aux = 0;
    r.len_or_ch = 4; r.addr = lsa; r.value = val;
    fr_push_main(&r, ctx);
}

void yz_fltrec_store128(spu_context* ctx, uint32_t lsa,
                         uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3)
{
    yz_fltrec_rec r; int i;
    uint32_t words[4];
    if (g_fr_main.capacity == 0) return;
    if (fr_muted(ctx, ctx->pc)) return;
    words[0] = w0; words[1] = w1; words[2] = w2; words[3] = w3;
    r.pc = ctx->pc; r.kind = (uint8_t)YZ_FR_STORE128; r.len_or_ch = 16; r.addr = lsa;
    for (i = 0; i < 4; i++) {
        r.aux = (uint8_t)i;
        r.value = words[i];
        fr_push_main(&r, ctx);
    }
}

void yz_fltrec_branch(spu_context* ctx, uint32_t target_pc)
{
    yz_fltrec_rec r;
    if (g_fr_main.capacity == 0) return;
    if (fr_muted(ctx, ctx->pc)) return;
    r.pc = ctx->pc; r.kind = (uint8_t)YZ_FR_BRANCH; r.aux = 0;
    r.len_or_ch = 0; r.addr = target_pc; r.value = 0;
    fr_push_main(&r, ctx);
}

void yz_fltrec_wrch(spu_context* ctx, uint32_t channel, uint32_t val)
{
    yz_fltrec_rec r;
    if (g_fr_main.capacity == 0) return;
    if (fr_muted(ctx, ctx->pc)) return;
    r.pc = ctx->pc; r.kind = (uint8_t)YZ_FR_WRCH; r.aux = 0;
    r.len_or_ch = (uint16_t)channel; r.addr = 0; r.value = val;
    fr_push_main(&r, ctx);
}

void yz_fltrec_rdch(spu_context* ctx, uint32_t channel, uint32_t val)
{
    yz_fltrec_rec r;
    if (g_fr_main.capacity == 0) return;
    if (fr_muted(ctx, ctx->pc)) return;
    r.pc = ctx->pc; r.kind = (uint8_t)YZ_FR_RDCH; r.aux = 0;
    r.len_or_ch = (uint16_t)channel; r.addr = 0; r.value = val;
    fr_push_main(&r, ctx);
}

void yz_fltrec_dma(spu_context* ctx, int is_put, uint32_t lsa,
                    uint32_t ea_lo, uint32_t ea_hi, uint32_t size,
                    uint32_t tag, uint32_t cmd)
{
    yz_fltrec_rec r1, r2;
    if (g_fr_main.capacity == 0) return;
    if (fr_muted(ctx, ctx->pc)) return;
    r1.pc = ctx->pc; r1.kind = (uint8_t)(is_put ? YZ_FR_DMA_PUT : YZ_FR_DMA_GET);
    r1.aux = 0; r1.len_or_ch = 0; r1.addr = lsa; r1.value = ea_lo;
    fr_push_main(&r1, ctx);
    r2.pc = ctx->pc; r2.kind = (uint8_t)YZ_FR_DMA_META;
    r2.aux = (uint8_t)(tag & 0x1Fu);
    r2.len_or_ch = (uint16_t)(size > 0xFFFFu ? 0xFFFFu : size);
    r2.addr = ea_hi; r2.value = cmd;
    fr_push_main(&r2, ctx);
}

void yz_fltrec_xadopt(spu_context* ctx, uint32_t addr, int from_img, int to_img)
{
    yz_fltrec_rec r;
    if (g_fr_main.capacity == 0) return;
    r.pc = ctx->pc; r.kind = (uint8_t)YZ_FR_XIMG;
    r.aux = (uint8_t)(from_img & 0xFF); r.len_or_ch = (uint16_t)(to_img & 0xFFFF);
    r.addr = addr; r.value = 0;
    fr_push_main(&r, ctx);
}

void yz_fltrec_call_ret(spu_context* ctx, int32_t from_img, int32_t to_img)
{
    yz_fltrec_rec r;
    if (g_fr_main.capacity == 0) return;
    r.pc = ctx->pc; r.kind = (uint8_t)YZ_FR_CALL_RET;
    r.aux = (uint8_t)(from_img & 0xFF); r.len_or_ch = (uint16_t)(to_img & 0xFFFF);
    r.addr = ctx->pc; r.value = 0;
    fr_push_main(&r, ctx);
}

/* ---------------------------------------------------------------------------
 * Foreign-write ring: atomic fetch_add cursor, since the writer thread is
 * whichever host thread is running the ctx save/restore machinery.
 * -------------------------------------------------------------------------*/
void yz_fltrec_foreign_write(spu_context* ctx, uint32_t lsa, uint32_t len, int site)
{
    yz_fltrec_rec r;
    uint32_t sum = 0;
    uint32_t n, i;
    const uint8_t* p;
    uint64_t seq;
    if (g_fr_foreign.capacity == 0) return;

    /* Cheap fold of the written region so the decoder can flag whether the
     * foreign write actually changed content, without dumping the payload. */
    n = len; p = ctx->ls + (lsa & SPU_LS_MASK);
    for (i = 0; i + 4 <= n; i += 4)
        sum ^= ((uint32_t)p[i] << 24) | ((uint32_t)p[i+1] << 16) |
               ((uint32_t)p[i+2] << 8) | (uint32_t)p[i+3];

    r.pc = ctx->pc; r.kind = (uint8_t)YZ_FR_FOREIGN_WRITE;
    r.aux = (uint8_t)site; r.len_or_ch = (uint16_t)(len > 0xFFFFu ? 0xFFFFu : len);
    r.addr = lsa; r.value = sum;
    /* v2: source identity is the ctx being written into, which in every
     * current call site is also the ctx whose own host thread is performing
     * the restore during its own dispatch -- see the header comment. */
    r.spu_id = ctx->spu_id;

    seq = atomic_fetch_add_explicit(&g_fr_foreign.cursor, 1, memory_order_relaxed);
    r.seq = seq;
    g_fr_foreign.buf[seq % g_fr_foreign.capacity] = r;
}

/* ---------------------------------------------------------------------------
 * Dump
 * -------------------------------------------------------------------------*/
static void fr_write_ring_file(const char* dir)
{
    char path[600]; FILE* f;
    uint64_t cap, cur;
    snprintf(path, sizeof(path), "%s/ring.bin", dir);
    f = fopen(path, "wb");
    if (!f) return;
    cap = g_fr_main.capacity;
    cur = atomic_load_explicit(&g_fr_main.cursor, memory_order_relaxed);
    if (cap == 0) { fclose(f); return; }
    if (cur <= cap) {
        fwrite(g_fr_main.buf, sizeof(yz_fltrec_rec), (size_t)cur, f);
    } else {
        uint64_t start = cur % cap;
        fwrite(g_fr_main.buf + start, sizeof(yz_fltrec_rec), (size_t)(cap - start), f);
        fwrite(g_fr_main.buf, sizeof(yz_fltrec_rec), (size_t)start, f);
    }
    fclose(f);
}

static void fr_write_foreign_file(const char* dir)
{
    char path[600]; FILE* f;
    uint64_t cap, cur;
    snprintf(path, sizeof(path), "%s/foreign.bin", dir);
    f = fopen(path, "wb");
    if (!f) return;
    cap = g_fr_foreign.capacity;
    cur = atomic_load_explicit(&g_fr_foreign.cursor, memory_order_relaxed);
    if (cap == 0) { fclose(f); return; }
    if (cur <= cap) {
        fwrite(g_fr_foreign.buf, sizeof(yz_fltrec_rec), (size_t)cur, f);
    } else {
        uint64_t start = cur % cap;
        fwrite(g_fr_foreign.buf + start, sizeof(yz_fltrec_rec), (size_t)(cap - start), f);
        fwrite(g_fr_foreign.buf, sizeof(yz_fltrec_rec), (size_t)start, f);
    }
    fclose(f);
}

static void fr_write_ls_file(const char* dir, spu_context* c)
{
    char path[600]; FILE* f;
    snprintf(path, sizeof(path), "%s/ls.bin", dir);
    f = fopen(path, "wb");
    if (!f) return;
    if (c) fwrite(c->ls, 1, SPU_LS_SIZE, f);
    fclose(f);
}

static void fr_write_ctx_file(const char* dir, spu_context* c)
{
    char path[600]; FILE* f; int i;
    snprintf(path, sizeof(path), "%s/ctx.txt", dir);
    f = fopen(path, "w");
    if (!f) return;
    if (!c) {
        fprintf(f, "consumer ctx = NULL (g_yz_consumer_ctx never published this boot)\n");
        fclose(f);
        return;
    }
    fprintf(f, "spu_id=0x%X image_id=%d pc=0x%05X status=%d\n",
            c->spu_id, c->image_id, c->pc, c->status);
    for (i = 0; i < 128; i++) {
        fprintf(f, "gpr[%3d] = %08X %08X %08X %08X\n", i,
                c->gpr[i]._u32[0], c->gpr[i]._u32[1], c->gpr[i]._u32[2], c->gpr[i]._u32[3]);
    }
    fclose(f);
}

static void fr_write_arena_file(const char* dir)
{
    extern uint8_t* vm_base;
    char path[600]; FILE* f;
    snprintf(path, sizeof(path), "%s/arena.bin", dir);
    f = fopen(path, "wb");
    if (!f) return;
    if (vm_base) fwrite(vm_base + 0x41F00000u, 1, 0x300000u, f);
    fclose(f);
}

static void fr_write_meta_file(const char* dir, const char* reason, int dump_no)
{
    char path[600]; FILE* f;
    uint64_t main_cap, main_cur, main_wraps, for_cap, for_cur, for_wraps;
    uint64_t time_unit_hz;
    snprintf(path, sizeof(path), "%s/meta.json", dir);
    f = fopen(path, "w");
    if (!f) return;

    main_cap = g_fr_main.capacity;
    main_cur = atomic_load_explicit(&g_fr_main.cursor, memory_order_relaxed);
    main_wraps = main_cap ? main_cur / main_cap : 0;
    for_cap = g_fr_foreign.capacity;
    for_cur = atomic_load_explicit(&g_fr_foreign.cursor, memory_order_relaxed);
    for_wraps = for_cap ? for_cur / for_cap : 0;

    /* SYNC records store addr=low32/value=high32 of this same clock, so the
     * decoder can turn any two SYNC ticks into elapsed seconds. */
#if defined(_MSC_VER)
    { LARGE_INTEGER f_; QueryPerformanceFrequency(&f_); time_unit_hz = (uint64_t)f_.QuadPart; }
#else
    time_unit_hz = 1000000000ull;   /* fr_emit_sync's POSIX leg uses raw nanoseconds */
#endif

    fprintf(f,
        "{\n"
        "  \"format_version\": 2,\n"
        "  \"reason\": \"%s\",\n"
        "  \"dump_index\": %d,\n"
        "  \"record_size_bytes\": %d,\n"
        "  \"sync_interval_records\": %u,\n"
        "  \"allctx\": %s,\n"
        "  \"main_ring\": { \"capacity_records\": %llu, \"emitted_records\": %llu, "
                "\"wraps\": %llu, \"overflowed\": %s },\n"
        "  \"foreign_ring\": { \"capacity_records\": %llu, \"emitted_records\": %llu, "
                "\"wraps\": %llu, \"overflowed\": %s },\n"
        "  \"arena_range\": { \"ea_start\": \"0x41F00000\", \"ea_end\": \"0x42200000\" },\n"
        "  \"time_unit_hz\": %llu,\n"
        "  \"build\": \"%s %s\",\n"
        "  \"note\": \"v2 (s42): records carry seq+spu_id and gained YZ_FR_XIMG/"
                "YZ_FR_CALL_RET; format_version absent = a pre-s42 v1 dump (16-byte "
                "records, no seq/spu_id) -- see tools/fltrec_dump.py. foreign.bin is "
                "an addition beyond the originally listed dump files, added to "
                "preserve the FOREIGN_WRITE history -- see scratch/s41_fltrec_report.md\"\n"
        "}\n",
        reason, dump_no, (int)sizeof(yz_fltrec_rec), YZ_FR_SYNC_INTERVAL,
        g_yz_fltrec_allctx ? "true" : "false",
        (unsigned long long)main_cap, (unsigned long long)main_cur,
        (unsigned long long)main_wraps, main_cur > main_cap ? "true" : "false",
        (unsigned long long)for_cap, (unsigned long long)for_cur,
        (unsigned long long)for_wraps, for_cur > for_cap ? "true" : "false",
        (unsigned long long)time_unit_hz,
        __DATE__, __TIME__);
    fclose(f);
}

void yz_fltrec_dump(const char* reason)
{
    char ts[32];
    char dir[300];
    time_t t;
    struct tm tmv;
    spu_context* c;
    int prev;

    if (g_yz_fltrec_on != 1) return;   /* recorder never armed -- nothing to dump */

    prev = atomic_fetch_add_explicit(&s_fr_dump_count, 1, memory_order_acq_rel);
    if (prev >= 2) {
        fprintf(stderr, "[fltrec] dump SKIPPED (max 2/boot already used): %s\n", reason);
        fflush(stderr);
        return;
    }

    t = time(NULL);
#if defined(_MSC_VER)
    localtime_s(&tmv, &t);
#else
    { struct tm* tp = localtime(&t); if (tp) tmv = *tp; else memset(&tmv, 0, sizeof(tmv)); }
#endif
    snprintf(ts, sizeof(ts), "%04d%02d%02d_%02d%02d%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

    YZ_FR_MKDIR("scratch/fltrec");
    snprintf(dir, sizeof(dir), "scratch/fltrec/%s_d%d", ts, prev + 1);
    YZ_FR_MKDIR(dir);

    c = (spu_context*)g_yz_consumer_ctx;

    fr_write_ring_file(dir);
    fr_write_foreign_file(dir);
    fr_write_ls_file(dir, c);
    fr_write_ctx_file(dir, c);
    fr_write_arena_file(dir);
    fr_write_meta_file(dir, reason, prev + 1);

    { uint64_t main_cur_now = atomic_load_explicit(&g_fr_main.cursor, memory_order_relaxed);
      uint64_t for_cur_now = atomic_load_explicit(&g_fr_foreign.cursor, memory_order_relaxed);
      fprintf(stderr,
              "[fltrec] DUMP #%d -> %s (reason=%s, main=%llu recs%s, foreign=%llu recs%s)\n",
              prev + 1, dir, reason,
              (unsigned long long)main_cur_now,
              main_cur_now > g_fr_main.capacity ? " OVERFLOWED" : "",
              (unsigned long long)for_cur_now,
              for_cur_now > g_fr_foreign.capacity ? " OVERFLOWED" : ""); }
    fflush(stderr);
}
