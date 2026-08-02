/* s50 differential harness: drives ONE lifted SPU twin (instruction-lifted or
 * region-lifted -- selected purely by which generated TU it is linked with)
 * over a deterministic synthetic environment and dumps the complete
 * observable state for cross-twin comparison:
 *
 *   - every channel operation in order (rdch/wrch/rchcnt with channel, value,
 *     and the MFC descriptor + payload hash for DMA commands),
 *   - final GPR file, local-store hash, status/stop_code, terminal kind,
 *   - the dispatch count at the same drain-level event the runtime's
 *     spu_prof_hop / s_prof_hops counts (the quantity region lifting
 *     reduces).
 *
 * Modes:
 *   single:  <exe> code.bin base entry seed evbudget hopmax out
 *            full dump (TERM/EV/EVHASH/GPR/LSHASH lines).
 *   sweep:   <exe> code.bin base sweep seed evbudget hopmax out
 *            one digest line per instruction pc in [base, base+codelen):
 *            the every-entry-PC differential (restart/resume protection).
 *
 * Determinism contract: every environmental response (rdch values, rchcnt
 * counts, GET fill bytes, initial LS + GPR noise) is a pure function of
 * (seed, op index / address), so two twins of the same image produce
 * IDENTICAL streams iff their instruction semantics and control flow agree.
 * A run is cut at the Nth channel event via longjmp FROM THE STUB, so both
 * twins stop at exactly the same architectural point regardless of how
 * their host functions are shaped.
 *
 * pc fidelity note (MEASURED, job_bin_a.c:32): instruction-mode
 * cross-function branches do NOT maintain ctx->pc, so events record pc for
 * the REGION twin's static validation only -- the cross-twin comparison
 * ignores the pc field.
 *
 * A collision driver may reuse the whole environment by defining
 * SPU_DIFF_NO_MAIN and including this file (see tools/test_spu_collision.py):
 * it gets the stubs, registry snapshot/install, env reset, ctx init and the
 * run loop, and provides its own main with per-placement dispatch tables.
 *
 * Build (one exe per twin):
 *   cl ... /I runtime\spu /I <twin dir> /DTWIN_HEADER=<twin.h> \
 *      /DREGISTER_FN=<register fn> spu_diff_harness.c <twin.c>
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <stdint.h>

#define STR_(x) #x
#define XSTR_(x) STR_(x)
#include XSTR_(TWIN_HEADER)   /* pulls spu_context.h via the twin header */
#include "spu_helpers.h"

void REGISTER_FN(void);

/* ---------------- runtime globals the generated code / headers expect ---- */
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*) = 0;
SPU_THREAD_LOCAL spu_context* g_spu_cur_ctx = 0;
int g_spu_prof_on = 1;   /* count dispatches via spu_prof_hop (see below) */
unsigned long g_spu_wrun_log = 0;
volatile unsigned long g_yz_cadv = 0;
volatile int g_yz_lscw_on = 0;
volatile int g_yz_ms_on = 0;
int g_yz_sguard_on = 0;
uint64_t g_tb_counter = 0;
const yz_runtime_config g_yz_runtime_config = {0};
volatile long g_yz_a010_root_active = 0;
volatile long g_yz_a010_stage_generation = 0;
/* fltrec gates: 0 = off and NEVER self-arms (yz_fltrec_hot() only arms when
 * it reads the unarmed value -1), so no recorder is ever invoked. */
volatile int g_yz_fltrec_on = 0;
volatile int g_yz_fltrec_allctx = 0;
volatile void* g_yz_consumer_ctx = 0;
volatile int g_yz_tr_on = 0;
uint32_t g_yz_anch_home = 0;

void yz_tagread_repair_fetch(spu_context* c, uint32_t lsa,
                             unsigned long long ea, uint32_t size)
{ (void)c; (void)lsa; (void)ea; (void)size; }
void yz_tagread_repair_read(spu_context* c, uint32_t lsa, uint32_t* v)
{ (void)c; (void)lsa; (void)v; }
void yz_a010_reltrace_gate(uint32_t spu, uint32_t code, uint32_t key,
                           uint32_t witness, uint32_t descriptor,
                           uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3)
{ (void)spu; (void)code; (void)key; (void)witness; (void)descriptor;
  (void)d0; (void)d1; (void)d2; (void)d3; }
void yz_tr_record(spu_context* c, uint32_t lsa, const uint32_t* v)
{ (void)c; (void)lsa; (void)v; }
void yz_a010_stage_ba68(spu_context* c, const char* phase,
                        uint32_t lsa, const uint32_t* v,
                        uint32_t dma_ea, uint32_t dma_size)
{ (void)c; (void)phase; (void)lsa; (void)v; (void)dma_ea; (void)dma_size; }

int yz_tagread_arm(void) { g_yz_tr_on = 0; return 0; }
void yz_ms_read(spu_context* c, uint32_t lsa, const uint32_t* v)
{ (void)c; (void)lsa; (void)v; }
void yz_tr_read(spu_context* c, uint32_t lsa, const uint32_t* v)
{ (void)c; (void)lsa; (void)v; }
void yz_tr_tag(spu_context* c, uint32_t lsa, const uint32_t* v)
{ (void)c; (void)lsa; (void)v; }
int yz_lscw_arm(void) { g_yz_lscw_on = 0; return 0; }
void yz_lscw_check(spu_context* c, uint32_t a, uint32_t n, const uint8_t* b,
                   unsigned long long x, const char* t)
{ (void)c; (void)a; (void)n; (void)b; (void)x; (void)t; }
int yz_mask_seal_arm(void) { g_yz_ms_on = 0; return 0; }
void yz_ms_write(spu_context* c, uint32_t lsa, const uint32_t* w)
{ (void)c; (void)lsa; (void)w; }
int yz_sguard_check(spu_context* c, void* t) { (void)c; (void)t; return 0; }
void yz_lockstep_tick(struct spu_context* c) { (void)c; }
void spu_task_launch_check(spu_context* c, uint32_t pc) { (void)c; (void)pc; }
void spu_task_launch_behavior_check(spu_context* c, uint32_t pc)
{ (void)c; (void)pc; }
/* Dispatch counter: g_spu_prof_on=1 makes EVERY SPU_DRAIN re-entry (nested
 * drains included) call this -- the exact event the runtime's s_prof_hops
 * counts. The driver loop adds its own top-level dispatches. */
static uint64_t g_hops_total = 0;
/* Sweep mode enforces the dispatch budget IN-FLIGHT (nested drains included):
 * a single region call can otherwise run a multi-second guard loop before the
 * driver-level check gets a chance. The cut lands at mode-dependent
 * architectural points by nature, so the sweep comparer treats HOPCUT pcs as
 * budget-capped (skip-counted), never as comparisons. Single-window mode
 * keeps the driver-level check only (its HOPMAX windows are already classed
 * inconclusive). */
static int g_sweep_cut_on = 0;
static uint64_t g_sweep_hopmax = 0;
static jmp_buf g_cut;
void spu_prof_hop(void* f)
{
    (void)f;
    g_hops_total++;
    if (g_sweep_cut_on && g_hops_total >= g_sweep_hopmax)
        longjmp(g_cut, 5);
}
void spu_img_restore(spu_context* c, int32_t s) { (void)c; (void)s; }
uint64_t ppu_timebase_now(void) { return ++g_tb_counter; }
void spu_ch_wake(spu_context* c) { (void)c; }
void spu_trace_init(const char* p) { (void)p; }
void spu_trace_pc(spu_context* c, uint32_t pc) { (void)c; (void)pc; }
void spu_trace_rt(spu_context* c, uint32_t r) { (void)c; (void)r; }
/* flight-recorder leaf recorders (never called with the gate at 0, but the
 * inline hooks reference them; prototypes match spu_fltrec.h exactly). */
void yz_fltrec_store32(struct spu_context* ctx, uint32_t lsa, uint32_t val)
{ (void)ctx; (void)lsa; (void)val; }
void yz_fltrec_store128(struct spu_context* ctx, uint32_t lsa,
                        uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3)
{ (void)ctx; (void)lsa; (void)w0; (void)w1; (void)w2; (void)w3; }
void yz_fltrec_branch(struct spu_context* ctx, uint32_t target_pc)
{ (void)ctx; (void)target_pc; }
void yz_fltrec_wrch(struct spu_context* ctx, uint32_t channel, uint32_t val)
{ (void)ctx; (void)channel; (void)val; }
void yz_fltrec_rdch(struct spu_context* ctx, uint32_t channel, uint32_t val)
{ (void)ctx; (void)channel; (void)val; }
void yz_fltrec_dma(struct spu_context* ctx, int is_put, uint32_t lsa,
                   uint32_t ea_lo, uint32_t ea_hi, uint32_t size,
                   uint32_t tag, uint32_t cmd)
{ (void)ctx; (void)is_put; (void)lsa; (void)ea_lo; (void)ea_hi; (void)size;
  (void)tag; (void)cmd; }
int yz_fltrec_enabled(void) { g_yz_fltrec_on = 0; return 0; }
void yz_fltrec_dump(const char* reason) { (void)reason; }

/* ---------------- deterministic environment --------------------------- */
static uint64_t g_seed;
static uint64_t mix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
static uint32_t prng32(uint64_t key) { return (uint32_t)mix64(g_seed ^ key); }

uint64_t spu_diff_fnv64(const uint8_t* p, size_t n, uint64_t h)
{
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}

/* ---------------- event log ------------------------------------------- */
enum { K_RDCH = 1, K_WRCH = 2, K_RCHCNT = 3, K_MFC = 4, K_UNKPC = 5 };
typedef struct { uint32_t kind, ch, a, b, c, d, pc; } ev_t;
#define EV_CAP 65536
static ev_t g_ev[EV_CAP];
static uint32_t g_nev = 0, g_ev_budget = 512;
/* g_cut is declared above spu_prof_hop (the sweep in-flight cut needs it) */
static uint32_t g_last_atomic = 0;
static uint32_t g_ch_idx[64];
static uint64_t g_dec = 0x40000000ull;

static void ev_push(uint32_t kind, uint32_t ch, uint32_t a, uint32_t b,
                    uint32_t c, uint32_t d, uint32_t pc)
{
    if (g_nev < EV_CAP) {
        ev_t* e = &g_ev[g_nev];
        e->kind = kind; e->ch = ch; e->a = a; e->b = b; e->c = c; e->d = d; e->pc = pc;
    }
    g_nev++;
    if (g_nev >= g_ev_budget)
        longjmp(g_cut, 2);   /* cut BOTH twins at exactly this event index */
}

/* ---------------- channel/MFC stub runtime ----------------------------- */
u128 spu_rdch(spu_context* ctx, uint32_t ch)
{
    uint32_t idx = g_ch_idx[ch & 63]++;
    uint32_t v;
    switch (ch) {
    case MFC_RdTagStat:    v = ctx->mfc_tag_mask; break;   /* all requested tags complete */
    case MFC_RdTagMask:    v = ctx->mfc_tag_mask; break;
    case MFC_RdAtomicStat: v = g_last_atomic; break;
    case MFC_RdListStallStat: v = 0; break;
    case SPU_RdEventStat:  v = ctx->event_mask; break;     /* every masked event pending */
    case SPU_RdEventMask:  v = ctx->event_mask; break;
    case SPU_RdMachStat:   v = 0; break;
    case SPU_RdSRR0:       v = ctx->srr0; break;
    case SPU_RdDec:        g_dec -= 64; v = (uint32_t)g_dec; break;
    default:               v = prng32(((uint64_t)ch << 40) | idx); break;
    }
    ev_push(K_RDCH, ch, v, idx, 0, 0, ctx->pc);
    return spu_pref_u32(v);
}

uint32_t spu_rchcnt(spu_context* ctx, uint32_t ch)
{
    ev_push(K_RCHCNT, ch, 1, 0, 0, 0, ctx->pc);
    return 1;   /* channels always ready: poll loops terminate deterministically */
}

static void mfc_execute(spu_context* ctx, uint32_t cmdw, uint32_t pc)
{
    uint32_t cmd = cmdw & 0xFF;
    uint32_t lsa = ctx->mfc_lsa & SPU_LS_MASK;
    uint32_t size = ctx->mfc_size;
    uint64_t ea = ((uint64_t)ctx->mfc_eah << 32) | ctx->mfc_eal;
    uint64_t payload = 0;
    int is_get = (cmd == MFC_GET_CMD || cmd == MFC_GETB_CMD || cmd == MFC_GETF_CMD);
    int is_put = (cmd == MFC_PUT_CMD || cmd == MFC_PUTB_CMD || cmd == MFC_PUTF_CMD);

    if (cmd == MFC_GETLLAR_CMD) { lsa &= ~127u; size = 128; is_get = 1; }
    if (cmd == MFC_PUTLLC_CMD || cmd == MFC_PUTLLUC_CMD || cmd == MFC_PUTQLLUC_CMD) {
        lsa &= ~127u; size = 128; is_put = 1;
    }
    if (size > SPU_LS_SIZE - lsa) size = SPU_LS_SIZE - lsa;   /* clamp, deterministic */

    if (is_get) {
        for (uint32_t i = 0; i < size; i++)
            ctx->ls[lsa + i] = (uint8_t)prng32(0xD0000000ull + ea + i);
        payload = spu_diff_fnv64(&ctx->ls[lsa], size, 0xCBF29CE484222325ull);
    } else if (is_put) {
        payload = spu_diff_fnv64(&ctx->ls[lsa], size, 0xCBF29CE484222325ull);
    }
    /* list forms (GETL/PUTL family) are recorded with their descriptor but not
     * transferred: the PRNG LS initialization stands in for the data, which is
     * identical for both twins -- documented harness simplification. */

    switch (cmd) {
    case MFC_PUTLLC_CMD:  g_last_atomic = MFC_PUTLLC_SUCCESS;  break;
    case MFC_PUTLLUC_CMD: g_last_atomic = MFC_PUTLLUC_SUCCESS; break;
    case MFC_GETLLAR_CMD: g_last_atomic = MFC_GETLLAR_SUCCESS; break;
    default: break;
    }
    ev_push(K_MFC, cmdw & 0xFFFF, lsa, ctx->mfc_eal, (ctx->mfc_tag << 16) | (size & 0xFFFF),
            (uint32_t)(payload ^ (payload >> 32)), pc);
}

void spu_wrch(spu_context* ctx, uint32_t ch, u128 value)
{
    uint32_t v = value._u32[0];
    switch (ch) {
    case MFC_LSA:   ctx->mfc_lsa = v; break;
    case MFC_EAH:   ctx->mfc_eah = v; break;
    case MFC_EAL:   ctx->mfc_eal = v; break;
    case MFC_Size:  ctx->mfc_size = v & 0xFFFF; break;
    case MFC_TagID: ctx->mfc_tag = v & 0x1F; break;
    case MFC_Cmd:   mfc_execute(ctx, v, ctx->pc); return;
    case MFC_WrTagMask:   ctx->mfc_tag_mask = v; break;
    case MFC_WrTagUpdate: break;
    case SPU_WrEventMask: ctx->event_mask = v; break;
    case SPU_WrEventAck:
        atomic_fetch_and_explicit(&ctx->event_status, ~v, memory_order_seq_cst);
        break;
    case SPU_WrSRR0: ctx->srr0 = v; break;
    case SPU_WrDec:  g_dec = v; break;
    default: break;
    }
    /* SPU channel writes consume only the preferred word.  Recording the
     * three ignored lanes would make a scalar compact helper look different
     * from an instruction write even though the channel observes the same
     * value. */
    ev_push(K_WRCH, ch, v, 0, 0, 0, ctx->pc);
}

/* Keep hand-written compact MFC sequences observable to this differential
 * harness exactly like the six architectural channel writes they replace in
 * the production runtime.  Generated fast twins may use this helper, while
 * their instruction twins still issue the channels one at a time. */
void spu_mfc_issue_compact(spu_context* ctx,
                           uint32_t lsa, uint32_t eah, uint32_t eal,
                           uint32_t size, uint32_t tag, uint32_t cmd,
                           const uint32_t channel_pcs[6])
{
    static const uint32_t channels[6] = {
        MFC_LSA, MFC_EAH, MFC_EAL, MFC_Size, MFC_TagID, MFC_Cmd
    };
    const uint32_t values[6] = { lsa, eah, eal, size, tag, cmd };
    const uint32_t saved_pc = ctx->pc;

    for (unsigned i = 0; i < 6; ++i) {
        if (channel_pcs)
            ctx->pc = channel_pcs[i];
        spu_wrch(ctx, channels[i], spu_pref_u32(values[i]));
    }
    ctx->pc = saved_pc;
}

uint64_t spu_mfc_read_atomic_status_compact(spu_context* ctx)
{
    if (!spu_rchcnt(ctx, MFC_RdAtomicStat))
        return 0;
    return (1ull << 32) |
           (uint64_t)spu_rdch(ctx, MFC_RdAtomicStat)._u32[0];
}

/* ---------------- dispatch table --------------------------------------- */
static void (*g_fn[SPU_LS_SIZE / 4])(spu_context*);
void spu_register_function(uint32_t addr, void (*fn)(spu_context*))
{
    g_fn[(addr & SPU_LS_MASK) >> 2] = fn;
}
void spu_begin_image(int image_id) { (void)image_id; }

/* registry helpers for multi-placement (collision) drivers */
void spu_diff_registry_clear(void) { memset(g_fn, 0, sizeof g_fn); }
void spu_diff_registry_snapshot(void (**dst)(spu_context*))
{ memcpy(dst, g_fn, sizeof g_fn); }
void spu_diff_registry_install(void (**src)(spu_context*))
{ memcpy(g_fn, src, sizeof g_fn); }
void* spu_diff_registry_lookup(uint32_t pc)
{ return (pc & 3) ? 0 : (void*)g_fn[(pc & SPU_LS_MASK) >> 2]; }

#define RET_SENTINEL 0x3FFF0u
static int g_sentinel_hit = 0;

void spu_indirect_branch(spu_context* ctx)
{
    /* FAITHFUL to the runtime dispatcher (spu_channels.c:4712): the lookup
     * takes the RAW pc and an out-of-LS or unregistered target is a LOUD
     * unknown-branch failure in BOTH modes. Masking here (an earlier harness
     * version) resurrected valid code for the instruction twin while the
     * region twin's entry switch correctly refused the raw value -- a
     * harness-only divergence the real system never produces. */
    uint32_t pc = ctx->pc;
    if (pc == RET_SENTINEL) {
        /* Terminate via longjmp, not status+return: a status set inside a
         * NESTED drain does not stop the enclosing lifted caller, so whether
         * the run ends would depend on host nesting depth, which legitimately
         * differs between the twins (region gotos collapse frames). */
        g_sentinel_hit = 1;
        ctx->status = SPU_STATUS_STOPPED;
        longjmp(g_cut, 4);
    }
    void (*fn)(spu_context*) =
        (pc >= SPU_LS_SIZE || (pc & 3)) ? 0 : g_fn[pc >> 2];
    if (!fn) {
        ev_push(K_UNKPC, 0, pc, 0, 0, 0, pc);
        /* Faithful to the runtime: an unknown branch ends the SPU's thread of
         * control via spu_halt's longjmp (spu_channels.c:1410), unwinding all
         * nesting identically in both modes. */
        spu_halt(ctx, SPU_STATUS_STOPPED_BY_HALT);
    }
    g_spu_trampoline_fn = fn;   /* enclosing SPU_DRAIN / driver loop runs it */
}

void spu_halt(spu_context* ctx, int status)
{
    /* The real runtime longjmps to the host driver here, unwinding every
     * lifted frame; mirror that so nested halts terminate the run instead of
     * silently continuing in the caller's continuation. */
    ctx->status = (uint32_t)status;
    longjmp(g_cut, 3);
}

/* ---------------- run core (shared by modes and collision drivers) ----- */
static uint8_t g_code[SPU_LS_SIZE];
static size_t g_codelen = 0;
static uint32_t g_base = 0;
static uint8_t g_ls_image[SPU_LS_SIZE];

int spu_diff_load_code(const char* path, uint32_t base)
{
    FILE* cf = fopen(path, "rb");
    if (!cf) return -1;
    g_codelen = fread(g_code, 1, sizeof g_code, cf);
    fclose(cf);
    g_base = base;
    return 0;
}

void spu_diff_env_reset(uint64_t seed)
{
    g_seed = seed;
    g_nev = 0;
    memset(g_ch_idx, 0, sizeof g_ch_idx);
    g_dec = 0x40000000ull;
    g_last_atomic = 0;
    g_sentinel_hit = 0;
    g_hops_total = 0;
    g_spu_trampoline_fn = 0;
    /* precompute the LS image for this seed: PRNG fill + code overlay */
    for (uint32_t i = 0; i < SPU_LS_SIZE; i++)
        g_ls_image[i] = (uint8_t)prng32(0xA0000000ull + i);
    memcpy(&g_ls_image[g_base & SPU_LS_MASK], g_code, g_codelen);
}

void spu_diff_ctx_init(spu_context* ctx)
{
    memset(ctx, 0, sizeof *ctx);
    ctx->status = SPU_STATUS_RUNNING;
    ctx->image_id = 0;
    memcpy(ctx->ls, g_ls_image, SPU_LS_SIZE);
    for (int r = 0; r < 128; r++)
        for (int w = 0; w < 4; w++)
            ctx->gpr[r]._u32[w] = prng32(0xB0000000ull + (uint64_t)r * 8 + w);
    /* Preferred words masked: loop trip counts and addresses come from the
     * preferred slot, and unbounded 32-bit noise makes synthetic windows spin
     * for billions of iterations (seen at gs_task 0x30C8, the s42 counted
     * loop). Single-window mode uses 16-bit trips. Sweep mode uses 8-bit
     * trips: a region twin's in-region loops execute with ZERO dispatches, so
     * no dispatch budget can bound them, and 16-bit NESTED loop products
     * (64k x 64k) ran single pcs for hours; 8-bit products (<=64k total
     * iterations) bound every pc architecturally in both twins identically.
     * Lanes 1-3 keep full-width entropy either way. */
    {
        uint32_t pref_mask = g_sweep_cut_on ? 0xFFu : 0xFFFFu;
        for (int r = 2; r < 128; r++)
            ctx->gpr[r]._u32[0] &= pref_mask;
    }
    ctx->gpr[0] = spu_pref_u32(RET_SENTINEL);   /* link: clean natural-return terminal */
    ctx->gpr[1] = spu_pref_u32(0x3FF80u);       /* plausible stack top */
    g_spu_cur_ctx = ctx;
    /* per-run event/dispatch state that env_reset also covers, repeated here
     * so a sweep can reuse one env per pc while resetting these cheaply */
    g_nev = 0;
    memset(g_ch_idx, 0, sizeof g_ch_idx);
    g_dec = 0x40000000ull;
    g_last_atomic = 0;
    g_sentinel_hit = 0;
    g_hops_total = 0;
    g_spu_trampoline_fn = 0;
}

const char* spu_diff_run(spu_context* ctx, uint32_t entry, uint64_t hopmax)
{
    const char* term = "?";
    int cutrc = setjmp(g_cut);
    if (cutrc == 0) {
        ctx->pc = entry;
        void (*fn)(spu_context*) = (entry & 3) ? 0 : g_fn[(entry & SPU_LS_MASK) >> 2];
        if (!fn)
            return "NOENTRY";
        for (;;) {
            g_hops_total++;              /* top-level dispatch */
            fn(ctx);
            if (ctx->status != SPU_STATUS_RUNNING)
                return g_sentinel_hit ? "RETURNED" : "STOPPED";
            if (!g_spu_trampoline_fn) return "DRAINED";
            if (g_hops_total >= hopmax) return "HOPMAX";
            fn = g_spu_trampoline_fn;
            g_spu_trampoline_fn = 0;
        }
    }
    term = (cutrc == 5) ? "HOPCUT"
         : (cutrc == 4) ? "RETURNED"
         : (cutrc == 3) ? "HALTED" : "EVCUT";
    return term;
}

uint64_t spu_diff_hops(void) { return g_hops_total; }
uint32_t spu_diff_nev(void) { return g_nev; }

uint64_t spu_diff_evhash(void)
{
    uint64_t evh = 0xCBF29CE484222325ull;
    uint32_t nev = g_nev < EV_CAP ? g_nev : EV_CAP;
    for (uint32_t i = 0; i < nev; i++) {
        ev_t* e = &g_ev[i];
        uint32_t key[6] = { e->kind, e->ch, e->a, e->b, e->c, e->d };  /* pc excluded */
        evh = spu_diff_fnv64((const uint8_t*)key, sizeof key, evh);
    }
    return evh;
}

void spu_diff_dump_single(FILE* out, const char* term, spu_context* ctx)
{
    fprintf(out, "TERM %s status=%u stop=0x%X hops=%llu events=%u pc=0x%X\n",
            term, ctx->status, ctx->stop_code,
            (unsigned long long)g_hops_total, g_nev, ctx->pc);
    uint32_t nev = g_nev < EV_CAP ? g_nev : EV_CAP;
    for (uint32_t i = 0; i < nev; i++) {
        ev_t* e = &g_ev[i];
        fprintf(out, "EV %u k=%u ch=%u a=0x%08X b=0x%08X c=0x%08X d=0x%08X pc=0x%X\n",
                i, e->kind, e->ch, e->a, e->b, e->c, e->d, e->pc);
    }
    fprintf(out, "EVHASH 0x%016llX\n", (unsigned long long)spu_diff_evhash());
    for (int r = 0; r < 128; r++)
        fprintf(out, "GPR %d %08X %08X %08X %08X\n", r,
                ctx->gpr[r]._u32[0], ctx->gpr[r]._u32[1],
                ctx->gpr[r]._u32[2], ctx->gpr[r]._u32[3]);
    fprintf(out, "LSHASH 0x%016llX\n",
            (unsigned long long)spu_diff_fnv64(ctx->ls, SPU_LS_SIZE, 0xCBF29CE484222325ull));
}

void spu_diff_dump_digest(FILE* out, uint32_t pc, const char* term, spu_context* ctx)
{
    fprintf(out, "PC %05X %s st=%u stop=0x%X ev=%u hops=%llu "
                 "g=%016llX l=%016llX e=%016llX\n",
            pc, term, ctx->status, ctx->stop_code, g_nev,
            (unsigned long long)g_hops_total,
            (unsigned long long)spu_diff_fnv64((const uint8_t*)ctx->gpr,
                                               sizeof ctx->gpr, 0xCBF29CE484222325ull),
            (unsigned long long)spu_diff_fnv64(ctx->ls, SPU_LS_SIZE,
                                               0xCBF29CE484222325ull),
            (unsigned long long)spu_diff_evhash());
}

#ifndef SPU_DIFF_NO_MAIN
static spu_context g_ctx;

int main(int argc, char** argv)
{
    if (argc != 8) {
        fprintf(stderr, "usage: %s code.bin base <entry|sweep> seed evbudget hopmax out\n",
                argv[0]);
        return 2;
    }
    uint32_t base = (uint32_t)strtoul(argv[2], 0, 16);
    int sweep = (strcmp(argv[3], "sweep") == 0);
    uint32_t entry = sweep ? 0 : (uint32_t)strtoul(argv[3], 0, 16);
    uint64_t seed = strtoull(argv[4], 0, 0);
    g_ev_budget = (uint32_t)strtoul(argv[5], 0, 0);
    uint64_t hopmax = strtoull(argv[6], 0, 0);
    const char* outp = argv[7];

    if (spu_diff_load_code(argv[1], base) != 0) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    REGISTER_FN();
    FILE* out = fopen(outp, "w");
    if (!out) { fprintf(stderr, "cannot open %s\n", outp); return 2; }

    if (!sweep) {
        spu_diff_env_reset(seed);
        spu_diff_ctx_init(&g_ctx);
        const char* term = spu_diff_run(&g_ctx, entry, hopmax);
        spu_diff_dump_single(out, term, &g_ctx);
    } else {
        g_sweep_cut_on = 1;
        g_sweep_hopmax = hopmax;
        spu_diff_env_reset(seed);
        for (uint32_t pc = base; pc < base + (uint32_t)g_codelen; pc += 4) {
            spu_diff_ctx_init(&g_ctx);
            const char* term = spu_diff_run(&g_ctx, pc, hopmax);
            spu_diff_dump_digest(out, pc, term, &g_ctx);
            if ((pc & 0x3FCu) == 0)
                fflush(out);   /* usable partial file if a backstop timeout fires */
        }
    }
    fclose(out);
    return 0;
}
#endif /* SPU_DIFF_NO_MAIN */
