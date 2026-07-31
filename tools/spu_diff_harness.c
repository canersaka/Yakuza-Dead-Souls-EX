/* s50 differential harness: drives ONE lifted SPU twin (instruction-lifted or
 * region-lifted -- selected purely by which generated TU it is linked with)
 * from a chosen entry pc over a deterministic synthetic environment, and dumps
 * the complete observable state for cross-twin comparison:
 *
 *   - every channel operation in order (rdch/wrch/rchcnt with channel, value,
 *     and the MFC descriptor + payload hash for DMA commands),
 *   - final GPR file, local-store hash, status/stop_code, terminal kind,
 *   - the driver-level dispatch (trampoline hop) count -- the same event the
 *     runtime's s_prof_hops counts, which is what the region lift is meant to
 *     reduce.
 *
 * Determinism contract: every environmental response (rdch values, rchcnt
 * counts, GET fill bytes, initial LS + GPR noise) is a pure function of
 * (seed, op index / address), so two twins of the same image produce
 * IDENTICAL streams iff their instruction semantics and control flow agree.
 * The run is cut at the Nth channel event via longjmp FROM THE STUB, so both
 * twins stop at exactly the same architectural point regardless of how their
 * host functions are shaped.
 *
 * pc fidelity note (MEASURED, job_bin_a.c:32): instruction-mode cross-function
 * branches do NOT maintain ctx->pc, so events record pc for the REGION twin's
 * static validation only -- the cross-twin comparison ignores the pc field.
 *
 * Build (one exe per twin):
 *   cl ... /I runtime\spu /I <twin dir> /DTWIN_HEADER=<twin.h> \
 *      /DREGISTER_FN=<register fn> spu_diff_harness.c <twin.c>
 * Run:
 *   <exe> <code.bin> <base-hex> <entry-hex> <seed> <ev-budget> <hop-max> <out>
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
/* fltrec gates: 0 = off and NEVER self-arms (yz_fltrec_hot() only arms when
 * it reads the unarmed value -1), so no recorder is ever invoked. */
volatile int g_yz_fltrec_on = 0;
volatile int g_yz_fltrec_allctx = 0;
volatile void* g_yz_consumer_ctx = 0;
volatile int g_yz_tr_on = 0;
uint32_t g_yz_anch_home = 0;

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
void spu_task_launch_check(spu_context* c, void* f) { (void)c; (void)f; }
/* Dispatch counter: g_spu_prof_on=1 makes EVERY SPU_DRAIN re-entry (nested
 * drains included) call this -- the exact event the runtime's s_prof_hops
 * counts, i.e. the quantity the region lift exists to reduce. The driver
 * loop adds its own top-level dispatches. */
static uint64_t g_hops_total = 0;
void spu_prof_hop(void* f) { (void)f; g_hops_total++; }
void spu_img_restore(spu_context* c, int32_t s) { (void)c; (void)s; }
uint64_t ppu_timebase_now(void) { return ++g_tb_counter; }
void spu_ch_wake(spu_context* c) { (void)c; }
void spu_trace_init(const char* p) { (void)p; }
void spu_trace_pc(spu_context* c, uint32_t pc) { (void)c; (void)pc; }
void spu_trace_rt(spu_context* c, uint32_t r) { (void)c; (void)r; }
/* flight-recorder leaf recorders (yz_fltrec_hot() is a header inline whose
 * gate g_yz_fltrec_on stays 0 here so these are never CALLED, but the inline
 * LS/branch hooks reference them, so the linker needs definitions with the
 * EXACT spu_fltrec.h prototypes). */
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

/* ---------------- event log ------------------------------------------- */
enum { K_RDCH = 1, K_WRCH = 2, K_RCHCNT = 3, K_MFC = 4, K_UNKPC = 5 };
typedef struct { uint32_t kind, ch, a, b, c, d, pc; } ev_t;
#define EV_CAP 65536
static ev_t g_ev[EV_CAP];
static uint32_t g_nev = 0, g_ev_budget = 512;
static jmp_buf g_cut;
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

static uint64_t fnv64(const uint8_t* p, size_t n, uint64_t h)
{
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
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
        payload = fnv64(&ctx->ls[lsa], size, 0xCBF29CE484222325ull);
    } else if (is_put) {
        payload = fnv64(&ctx->ls[lsa], size, 0xCBF29CE484222325ull);
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
    ev_push(K_WRCH, ch, v, value._u32[1], value._u32[2], value._u32[3], ctx->pc);
}

/* ---------------- dispatch table --------------------------------------- */
static void (*g_fn[SPU_LS_SIZE / 4])(spu_context*);
void spu_register_function(uint32_t addr, void (*fn)(spu_context*))
{
    g_fn[(addr & SPU_LS_MASK) >> 2] = fn;
}
void spu_begin_image(int image_id) { (void)image_id; }

#define RET_SENTINEL 0x3FFF0u
static int g_sentinel_hit = 0;

void spu_indirect_branch(spu_context* ctx)
{
    uint32_t pc = ctx->pc & SPU_LS_MASK;
    if (pc == RET_SENTINEL) { g_sentinel_hit = 1; ctx->status = SPU_STATUS_STOPPED; return; }
    void (*fn)(spu_context*) = (pc & 3) ? 0 : g_fn[pc >> 2];
    if (!fn) {
        ev_push(K_UNKPC, 0, pc, 0, 0, 0, pc);
        ctx->status = SPU_STATUS_STOPPED_BY_HALT;
        return;
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

/* ---------------- driver ------------------------------------------------ */
static spu_context g_ctx;

int main(int argc, char** argv)
{
    if (argc != 8) {
        fprintf(stderr, "usage: %s code.bin base entry seed evbudget hopmax out\n", argv[0]);
        return 2;
    }
    const char* codep = argv[1];
    uint32_t base = (uint32_t)strtoul(argv[2], 0, 16);
    uint32_t entry = (uint32_t)strtoul(argv[3], 0, 16);
    g_seed = strtoull(argv[4], 0, 0);
    g_ev_budget = (uint32_t)strtoul(argv[5], 0, 0);
    uint64_t hopmax = strtoull(argv[6], 0, 0);
    const char* outp = argv[7];

    FILE* cf = fopen(codep, "rb");
    if (!cf) { fprintf(stderr, "cannot open %s\n", codep); return 2; }
    static uint8_t code[SPU_LS_SIZE];
    size_t codelen = fread(code, 1, sizeof code, cf);
    fclose(cf);

    spu_context* ctx = &g_ctx;
    memset(ctx, 0, sizeof *ctx);
    ctx->status = SPU_STATUS_RUNNING;
    ctx->image_id = 0;
    for (uint32_t i = 0; i < SPU_LS_SIZE; i++)
        ctx->ls[i] = (uint8_t)prng32(0xA0000000ull + i);
    memcpy(&ctx->ls[base & SPU_LS_MASK], code, codelen);
    for (int r = 0; r < 128; r++)
        for (int w = 0; w < 4; w++)
            ctx->gpr[r]._u32[w] = prng32(0xB0000000ull + (uint64_t)r * 8 + w);
    /* Preferred words masked to 16 bits: loop trip counts and addresses come
     * from the preferred slot, and unbounded 32-bit noise makes synthetic
     * windows spin for billions of iterations (seen at gs_task 0x30C8, the
     * s42 counted loop). 16-bit trips keep every window terminating in BOTH
     * twins while lanes 1-3 keep full-width entropy. */
    for (int r = 2; r < 128; r++)
        ctx->gpr[r]._u32[0] &= 0xFFFF;
    ctx->gpr[0] = spu_pref_u32(RET_SENTINEL);   /* link: clean natural-return terminal */
    ctx->gpr[1] = spu_pref_u32(0x3FF80u);       /* plausible stack top */
    ctx->mfc_tag_mask = 0;
    g_spu_cur_ctx = ctx;

    REGISTER_FN();

    const char* term = "?";
    int cutrc = setjmp(g_cut);
    if (cutrc == 0) {
        ctx->pc = entry;
        void (*fn)(spu_context*) = (entry & 3) ? 0 : g_fn[(entry & SPU_LS_MASK) >> 2];
        if (!fn) {
            term = "NOENTRY";
        } else {
            for (;;) {
                g_hops_total++;              /* top-level dispatch */
                fn(ctx);
                if (ctx->status != SPU_STATUS_RUNNING) {
                    term = g_sentinel_hit ? "RETURNED" : "STOPPED";
                    break;
                }
                if (!g_spu_trampoline_fn) { term = "DRAINED"; break; }
                if (g_hops_total >= hopmax) { term = "HOPMAX"; break; }
                fn = g_spu_trampoline_fn;
                g_spu_trampoline_fn = 0;
            }
        }
    } else {
        term = (cutrc == 3) ? "HALTED" : "EVCUT";
    }

    FILE* out = fopen(outp, "w");
    if (!out) { fprintf(stderr, "cannot open %s\n", outp); return 2; }
    fprintf(out, "TERM %s status=%u stop=0x%X hops=%llu events=%u pc=0x%X\n",
            term, ctx->status, ctx->stop_code, (unsigned long long)g_hops_total,
            g_nev, ctx->pc);
    uint64_t evh = 0xCBF29CE484222325ull;
    uint32_t nev = g_nev < EV_CAP ? g_nev : EV_CAP;
    for (uint32_t i = 0; i < nev; i++) {
        ev_t* e = &g_ev[i];
        fprintf(out, "EV %u k=%u ch=%u a=0x%08X b=0x%08X c=0x%08X d=0x%08X pc=0x%X\n",
                i, e->kind, e->ch, e->a, e->b, e->c, e->d, e->pc);
        uint32_t key[6] = { e->kind, e->ch, e->a, e->b, e->c, e->d };  /* pc excluded */
        evh = fnv64((const uint8_t*)key, sizeof key, evh);
    }
    fprintf(out, "EVHASH 0x%016llX\n", (unsigned long long)evh);
    for (int r = 0; r < 128; r++)
        fprintf(out, "GPR %d %08X %08X %08X %08X\n", r,
                ctx->gpr[r]._u32[0], ctx->gpr[r]._u32[1],
                ctx->gpr[r]._u32[2], ctx->gpr[r]._u32[3]);
    fprintf(out, "LSHASH 0x%016llX\n",
            (unsigned long long)fnv64(ctx->ls, SPU_LS_SIZE, 0xCBF29CE484222325ull));
    fclose(out);
    return 0;
}
