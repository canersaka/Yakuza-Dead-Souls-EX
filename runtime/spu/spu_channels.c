/*
 * ps3recomp - SPU channel + indirect-branch runtime glue
 *
 * Implements the externs the SPU lifter (tools/spu_lifter.py) emits:
 *   - spu_rdch / spu_rchcnt / spu_wrch : SPU channel access. MFC channels are
 *     routed to the DMA engine (spu_dma.h); mailboxes, signal notification,
 *     events and the decrementer use the spu_context channel fields.
 *   - spu_indirect_branch : resolves ctx->pc to a lifted spu_func_* via a
 *     registry that generated code populates by calling spu_recomp_register().
 *
 * The MFC engine state is kept per spu_context here (spu_context.h does not
 * embed one), in a small lazily-populated registry.
 */

#include "spu_dma.h"
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
#if defined(_MSC_VER)
#  include <intrin.h>
#  define SPU_CPU_RELAX() _mm_pause()
#else
#  define SPU_CPU_RELAX() ((void)0)
#endif

/* Per-thread unwind target: a halted SPU (stop instruction, or an indirect
 * branch into unlifted code) must abort the lifted call chain rather than
 * return into it and spin. The host thread proc sets this with setjmp;
 * spu_halt() longjmps back to it. */
#if defined(_MSC_VER)
__declspec(thread) jmp_buf* g_spu_halt_jmp = 0;
#else
_Thread_local jmp_buf* g_spu_halt_jmp = 0;
#endif

/* Tail-call trampoline target (see spu_context.h / SPU_DRAIN). Set by a
 * cross-function tail branch in lifted SPU code; drained iteratively by the
 * enclosing call site or the host-thread driver. */
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*) = 0;
SPU_THREAD_LOCAL spu_context* g_spu_cur_ctx = 0;
int g_spu_dtrace_spu = -1;   /* spu_id whose dispatch tail to hop-trace after gs_task ELF load (-1=off) */

void spu_halt(spu_context* ctx, int status)
{
    ctx->status = (uint32_t)status;
    if (g_spu_halt_jmp)
        longjmp(*g_spu_halt_jmp, 1);
    /* No unwind target (e.g. unit test): fall back to returning. */
}

/* ===========================================================================
 * Global lock-line lock (GETLLAR/PUTLLC transactions across all SPU host
 * threads). A C11 atomic_flag spinlock keeps this portable; the critical
 * sections are tiny (two 128-byte memcpy + memcmp).
 * ===========================================================================*/
#include <stdatomic.h>   /* MSVC: needs /experimental:c11atomics (set by the
                            runtime CMake flags) */
static atomic_flag s_lockline = ATOMIC_FLAG_INIT;
void spu_lockline_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&s_lockline, memory_order_acquire))
        ;
}
void spu_lockline_unlock(void)
{
    atomic_flag_clear_explicit(&s_lockline, memory_order_release);
}

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * PPU<->SPU lock-line coherence (1f SPURS dispatch fix, 2026-06-16b).
 * The SPURS kernel hammers GETLLAR/PUTLLC on its management area (~7.7M/s); the
 * PPU (LLE libsre CreateTask2/AddWorkload) atomically bumps the SAME lines
 * (wklReadyCount1, sysSrvMessage, ...). A PPU write that interleaves with the
 * kernel's PUTLLC memcpy is torn/clobbered, so the kernel never sees the bump
 * -> the taskset is never scheduled. We mark every 128-byte line the SPU
 * reserves (GETLLAR) in a bitmap; a PPU write (shims.cpp vm_write*) to a marked
 * line serializes through the lock-line lock so the kernel's PUTLLC memcmp sees
 * the change and re-reads. PROVEN: forcing wklReadyCount1 under the lock-line
 * sticks; a plain write is clobbered. Scoped to [0x40000000,0x44000000) (SPURS
 * structs + cmd buffer); cmd-buffer lines are never SPU-reserved so they stay
 * unmarked = fast path. Lines are never un-marked (the management area is always
 * reserved; a stale mark only costs an extra locked write to a SPURS struct). */
#define SPU_COH_LO 0x40000000u
#define SPU_COH_HI 0x44000000u
static unsigned char s_coh_bitmap[((SPU_COH_HI - SPU_COH_LO) >> 7) / 8];  /* 64 KB, bit per 128B line */
void spu_coh_reserve(uint32_t ea)
{
    if (ea - SPU_COH_LO >= SPU_COH_HI - SPU_COH_LO) return;
    uint32_t line = (ea - SPU_COH_LO) >> 7;
    s_coh_bitmap[line >> 3] |= (unsigned char)(1u << (line & 7));
}
int spu_coh_is_reserved(uint32_t addr)
{
    if (addr - SPU_COH_LO >= SPU_COH_HI - SPU_COH_LO) return 0;
    uint32_t line = (addr - SPU_COH_LO) >> 7;
    return (s_coh_bitmap[line >> 3] >> (line & 7)) & 1u;
}

/* ===========================================================================
 * Per-context MFC engine registry
 * ===========================================================================*/
#define SPU_MAX_CONTEXTS 8

typedef struct {
    spu_context* ctx;
    mfc_engine   mfc;
} spu_mfc_slot;

static spu_mfc_slot s_mfc_slots[SPU_MAX_CONTEXTS];

static mfc_engine* mfc_for(spu_context* ctx)
{
    spu_mfc_slot* free_slot = NULL;
    for (int i = 0; i < SPU_MAX_CONTEXTS; i++) {
        if (s_mfc_slots[i].ctx == ctx)
            return &s_mfc_slots[i].mfc;
        if (!free_slot && s_mfc_slots[i].ctx == NULL)
            free_slot = &s_mfc_slots[i];
    }
    if (free_slot) {
        free_slot->ctx = ctx;
        mfc_engine_init(&free_slot->mfc);
        return &free_slot->mfc;
    }
    /* Out of slots: fall back to a shared engine (correct for single-SPU). */
    static mfc_engine fallback;
    static int fallback_init = 0;
    if (!fallback_init) { mfc_engine_init(&fallback); fallback_init = 1; }
    return &fallback;
}

/* SPU_EVENT_LR (lock-line reservation lost): a write by another processor to a
 * 128-byte line an SPU has GETLLAR-reserved raises event 0x400 on that SPU and
 * invalidates its reservation. This is the SPURS idle-service wakeup -- when the
 * PPU (LLE libsre CreateTask/AddWorkload) writes the management line
 * (sysSrvMessage/wklState1/readyCount), the service parked in
 * `rdch SPU_RdEventStat` (spu_func_00001178) wakes and re-processes. Called from
 * the PPU coherence write path (shims.cpp VM_WRITE_COH) for reserved lines. */
unsigned long g_spu_lr_raise = 0;   /* diag: LR events delivered (YZ_SPU_PROF) */
void spu_coh_notify_write(uint32_t ea)
{
    uint32_t line = ea & ~127u;
    for (int i = 0; i < SPU_MAX_CONTEXTS; i++) {
        spu_context* c = s_mfc_slots[i].ctx;
        if (!c) continue;
        mfc_engine* m = &s_mfc_slots[i].mfc;
        if (m->resv_active && (uint32_t)(m->resv_ea & ~127ull) == line) {
            c->event_status |= 0x400u;   /* SPU_EVENT_LR */
            m->resv_active = 0;          /* reservation lost */
            g_spu_lr_raise++;
        }
    }
}

static int channel_is_mfc(uint32_t ch)
{
    switch (ch) {
    case MFC_WrMSSyncReq: case MFC_RdTagMask:  case MFC_LSA:
    case MFC_EAH:         case MFC_EAL:         case MFC_Size:
    case MFC_TagID:       case MFC_Cmd:         case MFC_WrTagMask:
    case MFC_WrTagUpdate: case MFC_RdTagStat:   case MFC_RdListStallStat:
    case MFC_WrListStallAck: case MFC_RdAtomicStat:
        return 1;
    default:
        return 0;
    }
}

/* ===========================================================================
 * Channel write
 * ===========================================================================*/
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value)
{
    uint32_t v = value._u32[0];  /* channel writes use the preferred slot */

    if (channel_is_mfc(channel)) {
        mfc_channel_write(mfc_for(ctx), ctx, channel, v);
        return;
    }

    switch (channel) {
    case SPU_WrOutMbox:      spu_channel_write(&ctx->ch_out_mbox, v);       break;
    case SPU_WrOutIntrMbox:  spu_channel_write(&ctx->ch_out_intr_mbox, v);  break;
    case SPU_WrDec:          ctx->decrementer = v;                          break;
    case SPU_WrEventMask:    ctx->event_mask = v;                           break;
    case SPU_WrEventAck:     ctx->event_status &= ~v;                       break;
    case SPU_WrSRR0:         ctx->srr0 = v;                                 break;
    default:
        /* Unknown / unhandled channel write -- ignore (matches a no-op SPU). */
        break;
    }
}

/* ===========================================================================
 * Channel read (returns value in the preferred word slot)
 * ===========================================================================*/
/* DIAG (1f SPURS dispatch, env-gated by the reader): per-channel access counters
 * so the PPU side can see what the idle SPURS kernel busy-waits on. */
unsigned long g_spu_ch_rd[128]  = {0};
unsigned long g_spu_ch_cnt[128] = {0};
unsigned long g_spu_getllar_n   = 0;
uint32_t      g_spu_getllar_ea  = 0;
unsigned long g_spu_putllc_log  = 0;   /* spu_dma.h mgmt-CAS diag cap (YZ_SPU_PROF) */
unsigned long g_spu_lsdump_n    = 0;   /* spu_dma.h mgmt-GETLLAR kernel-ctx dump cap */
unsigned long g_spu_wrun_log    = 0;   /* spu_context.h wklRunnable-write diag cap */
unsigned long g_spu_kick_log    = 0;   /* spu_dma.h msgUpd-kick-seen diag cap */
unsigned long g_spu_mgmt_put_ok = 0;   /* mgmt-line PUTLLC successes (livelock test) */
unsigned long g_spu_mgmt_put_fail = 0; /* mgmt-line PUTLLC failures */
unsigned long g_spu_getn2        = 0;  /* steady-state mgmt-struct GET sampler cap */
unsigned long g_spu_evstat_rd    = 0;  /* steady-state SPU_RdEventStat reads (Loop A test) */
/* Distinct GETLLAR line EAs seen in steady state (does the service read
 * wklState1 @ mgmt+0x80 = the ProcessRequests update path?). YZ_SPU_PROF. */
uint32_t      g_spu_llar_eas[32] = {0};
unsigned long g_spu_llar_cnt[32] = {0};
int           g_spu_llar_nea     = 0;
void spu_llar_note(uint32_t ea)
{
    extern unsigned long g_spu_getllar_n;
    if (g_spu_getllar_n < 8000000UL) return;   /* steady state only */
    for (int i = 0; i < g_spu_llar_nea; i++)
        if (g_spu_llar_eas[i] == ea) { g_spu_llar_cnt[i]++; return; }
    if (g_spu_llar_nea < 32) {
        g_spu_llar_eas[g_spu_llar_nea] = ea;
        g_spu_llar_cnt[g_spu_llar_nea] = 1;
        g_spu_llar_nea++;
    }
}

u128 spu_rdch(spu_context* ctx, uint32_t channel)
{
    uint32_t v = 0;
    if (channel < 128) g_spu_ch_rd[channel]++;
    /* DIAG: what channel does gs_task (LS 0x3000..0xBC00) read while it stalls
     * in init? (29=RdInMbox 3/4=RdSigNotify1/2 23=MFC_RdTagStat 0=RdEventStat) */
    if (g_spu_prof_on && (ctx->pc & SPU_LS_MASK) >= 0x3000u && (ctx->pc & SPU_LS_MASK) < 0xBC00u) {
        static int n = 0;
        if (n < 50) { n++; fprintf(stderr, "[gst-ch] rdch ch%u pc=0x%05X\n", channel, ctx->pc & SPU_LS_MASK); }
    }

    if (channel_is_mfc(channel)) {
        v = mfc_channel_read(mfc_for(ctx), ctx, channel);
        return spu_make_preferred_u32(v);
    }

    switch (channel) {
    case SPU_RdInMbox:      v = spu_channel_read(&ctx->ch_in_mbox);     break;
    case SPU_RdSigNotify1:  v = spu_channel_read(&ctx->ch_sig_notify[0]); break;
    case SPU_RdSigNotify2:  v = spu_channel_read(&ctx->ch_sig_notify[1]); break;
    case SPU_RdDec:         v = ctx->decrementer;                       break;
    case SPU_RdEventMask:   v = ctx->event_mask;                        break;
    case SPU_RdEventStat:
        /* NOTE: HW stalls here until an enabled event is pending; a hard block
         * hangs (the reserved-line vs PPU-write-line alignment isn't yet proven
         * -- the service parks but the LR never wakes it). Kept non-blocking; the
         * LR bit raised by spu_coh_notify_write is still visible to this read so
         * the polling service can pick it up. Revisit blocking once the idle
         * reservation line is confirmed to match what the PPU writes. */
        v = *(volatile uint32_t*)&ctx->event_status;
        /* DIAG (YZ_SPU_PROF): does the service reach the event-wait path in
         * STEADY STATE? If RdEventStat fires after 20M GETLLARs, the LR-event
         * (Loop A) is live and worth implementing; if never, the spin is the
         * poll loop (Loop B) and LR is a red herring. */
        if (g_spu_prof_on) {
            extern unsigned long g_spu_getllar_n, g_spu_evstat_rd;
            if (g_spu_getllar_n > 20000000UL) {
                if (g_spu_evstat_rd < 20)
                    fprintf(stderr, "[spu-ev] spu=%X RdEventStat steady: status=0x%X mask=0x%X\n",
                            ctx->spu_id, ctx->event_status, ctx->event_mask);
                g_spu_evstat_rd++;
            }
        }
        break;
    case SPU_RdMachStat:    v = (ctx->status == SPU_STATUS_RUNNING) ? 1 : 0; break;
    case SPU_RdSRR0:        v = ctx->srr0;                              break;
    default:
        v = 0;
        break;
    }
    return spu_make_preferred_u32(v);
}

/* ===========================================================================
 * Channel count (rchcnt) -- how many entries can be read/written right now
 * ===========================================================================*/
uint32_t spu_rchcnt(spu_context* ctx, uint32_t channel)
{
    if (channel < 128) g_spu_ch_cnt[channel]++;
    if (g_spu_prof_on && (ctx->pc & SPU_LS_MASK) >= 0x3000u && (ctx->pc & SPU_LS_MASK) < 0xBC00u) {
        static int n = 0;
        if (n < 50) { n++; fprintf(stderr, "[gst-ch] rchcnt ch%u pc=0x%05X\n", channel, ctx->pc & SPU_LS_MASK); }
    }
    switch (channel) {
    case SPU_RdInMbox:       return ctx->ch_in_mbox.count;                 /* readable */
    case SPU_WrOutMbox:      return SPU_MBOX_DEPTH - ctx->ch_out_mbox.count; /* free slots */
    case SPU_WrOutIntrMbox:  return SPU_INTR_MBOX_DEPTH - ctx->ch_out_intr_mbox.count;
    case SPU_RdSigNotify1:   return ctx->ch_sig_notify[0].count;
    case SPU_RdSigNotify2:   return ctx->ch_sig_notify[1].count;
    case MFC_Cmd:            return MFC_QUEUE_DEPTH - mfc_for(ctx)->queue_count;
    case MFC_RdTagStat:      return 1;  /* synchronous: status always ready */
    default:                 return 1;  /* default: channel ready */
    }
}

/* ===========================================================================
 * Indirect-branch dispatch + function registry
 * ===========================================================================*/
typedef void (*spu_fn)(spu_context*);

typedef struct {
    uint32_t addr;
    spu_fn   fn;
    int      image_id;   /* which recompiled image this function belongs to */
} spu_reg_entry;

#define SPU_FN_REGISTRY_MAX 65536
static spu_reg_entry s_registry[SPU_FN_REGISTRY_MAX];
static uint32_t s_registry_count = 0;

/* Image currently being registered. SPURS images (kernel/policy/job) overlap in
 * LS, so each registers under a distinct id via spu_begin_image() before calling
 * its (prefixed) spu_recomp_register(). Single-image callers leave it 0. */
static int s_reg_image = 0;
void spu_begin_image(int image_id) { s_reg_image = image_id; }

/* ===========================================================================
 * Function-level SPU spin profiler (diagnostic, env YZ_SPU_PROF)
 *
 * The SPURS scheduler loops via tail-call trampolines (g_spu_trampoline_fn),
 * NOT register-indirect branches -- so spu_indirect_branch never sees its hot
 * loop. SPU_DRAIN (spu_context.h) calls spu_prof_hop() on each trampoline
 * re-entry when g_spu_prof_on is set; we histogram the target LS address and
 * periodically dump the hottest functions, pinning exactly which lifted SPURS
 * functions the SPU threads spin in. Off by default (one branch per hop).
 * ===========================================================================*/
int g_spu_prof_on = 0;   /* set from YZ_SPU_PROF at startup (main.cpp) */
int g_yz_watch_dlist = 0;        /* YZ_WATCH_DLIST: watch PPU writes to io 0x1104D00 */
unsigned long g_yz_dlist_w = 0;  /* cap for the [dlist-w] log */
uint32_t g_yz_taskset_ea = 0;    /* EA of the gs_task CellSpursTaskset (bitset line); captured from the task_info DMA */

#define SPU_PROF_HSZ   8192            /* fn-pointer hash slots (>> registry) */
static void*    s_p_fn[SPU_PROF_HSZ];
static uint32_t s_p_addr[SPU_PROF_HSZ];

#define SPU_PROF_SLOTS 0x10000UL       /* one per (LS addr >> 2), 256 KB LS */
static unsigned long s_prof_hist[SPU_PROF_SLOTS];
static unsigned long s_prof_hops = 0;

static unsigned p_hash(void* fn)
{
    uintptr_t p = (uintptr_t)fn;
    p ^= p >> 17; p *= 0x9E3779B1u; p ^= p >> 13;
    return (unsigned)p & (SPU_PROF_HSZ - 1);
}

static void spu_prof_insert(uint32_t addr, spu_fn fn)
{
    unsigned h = p_hash((void*)fn);
    while (s_p_fn[h]) {
        if (s_p_fn[h] == (void*)fn) { s_p_addr[h] = addr; return; }
        h = (h + 1) & (SPU_PROF_HSZ - 1);
    }
    s_p_fn[h]   = (void*)fn;
    s_p_addr[h] = addr;
}

static uint32_t spu_prof_addr_of(void* fn)
{
    unsigned h = p_hash(fn);
    while (s_p_fn[h]) {
        if (s_p_fn[h] == fn) return s_p_addr[h];
        h = (h + 1) & (SPU_PROF_HSZ - 1);
    }
    return 0xFFFFFFFFu;
}

static void spu_prof_dump(void)
{
    enum { TOP = 24 };
    uint32_t bi[TOP]; unsigned long bc[TOP];
    for (int i = 0; i < TOP; i++) { bi[i] = 0; bc[i] = 0; }
    for (unsigned long s = 0; s < SPU_PROF_SLOTS; s++) {
        unsigned long c = s_prof_hist[s];
        if (c <= bc[TOP - 1]) continue;
        int j = TOP - 1;
        while (j > 0 && bc[j - 1] < c) { bc[j] = bc[j - 1]; bi[j] = bi[j - 1]; j--; }
        bc[j] = c; bi[j] = (uint32_t)(s << 2);
    }
    fprintf(stderr, "[spu-prof] %lu trampoline hops; hottest LS funcs:\n", s_prof_hops);
    for (int i = 0; i < TOP && bc[i]; i++)
        fprintf(stderr, "  LS 0x%05X : %lu\n", bi[i], bc[i]);
    fprintf(stderr, "[spu-prof] mgmt-line(0x40197C80) PUTLLC: ok=%lu fail=%lu\n",
            g_spu_mgmt_put_ok, g_spu_mgmt_put_fail);
    fprintf(stderr, "[spu-prof] distinct GETLLAR line EAs (steady state):\n");
    for (int i = 0; i < g_spu_llar_nea; i++)
        fprintf(stderr, "    ea=0x%08X  x%lu\n", g_spu_llar_eas[i], g_spu_llar_cnt[i]);
    fflush(stderr);
}

void spu_prof_hop(void* fn)
{
    uint32_t a = spu_prof_addr_of(fn);
    if (a < SPU_PROF_SLOTS * 4u) s_prof_hist[a >> 2]++;
    /* DIAG: gs_task executes via the trampoline (not spu_indirect_branch), so
     * log its trampoline-target sequence here -- the entry (should be 0x3050)
     * and the flow into the spin -- to see where/why it stalls. */
    if (a >= 0x3000u && a < 0xBC00u) {
        static int gst = 0;
        if (gst < 6000) { gst++; fprintf(stderr, "[gstask-hop] LS 0x%05X\n", a); }
    }
    /* DIAG: trace the policy spu's hop path through the dispatch tail after gs_task
     * loads -- to see exactly where it branches to the kernel (0x838) instead of
     * StartTask (bi to entry 0x3050). Armed by the ELF-load DMA (spu_dma.h). */
    /* Arm the dispatch trace on the FIRST policy (image 2) hop, so we capture
     * 0x1F18 (StartTask/dispatch) + the launch-vs-yield decision from the start
     * (the ELF-GET arming in spu_dma.h fires too late -- mid-0x1F18). */
    if (g_spu_dtrace_spu < 0 && g_spu_cur_ctx && g_spu_cur_ctx->image_id == 2)
        g_spu_dtrace_spu = (int)g_spu_cur_ctx->spu_id;
    if (g_spu_dtrace_spu >= 0 && g_spu_cur_ctx
            && (int)g_spu_cur_ctx->spu_id == g_spu_dtrace_spu) {
        /* FOCUSED decision-path trace (2026-06-18): log only the launch-vs-yield
         * decision blocks so a single run shows the flow across MANY cycles
         * (the old blanket 4000-hop cap cut off before the kernel<->workload
         * resumes fired, so 0x231C/0x290/0x3050 never appeared -- ambiguous).
         * Ranges: StartTask 0x1C00-0x1CD0, GUID+poll helper 0x13B0-0x1440,
         * cellSpursModulePollStatus 0x2300-0x2380, SelectWorkload 0x290-0x300,
         * kernel workload-exit 0x838, gs_task launch 0x3050-0x3060. */
        /* Focused decision-path trace (no per-hop flush, skip the 0x2200-0x22FF
         * BSS/task-region memset loop which floods): StartTask, the GUID+poll
         * helper, the poll, SelectWorkload, kernel-exit, and the gs_task launch
         * window 0x3040-0x30A0. Shows whether StartTask reaches bi 0x3050. */
        int key = ((a >= 0x1C00u && a <= 0x1CD0u)
                || (a >= 0x13B0u && a <= 0x14D0u)
                || (a >= 0x2300u && a <= 0x2380u)
                || (a >= 0x0290u && a <= 0x0300u)
                || (a == 0x0838u)
                || (a >= 0x3040u && a <= 0x30A0u))
               && !(a >= 0x2238u && a <= 0x2264u);
        if (key) {
            static int dt = 0;
            if (dt < 6000) { dt++;
                fprintf(stderr, "[dtrace] LS 0x%05X img%d\n", a, g_spu_cur_ctx->image_id);
            }
        }
    }
    /* GS_TASK LAUNCH (2026-06-19). StartTask now reaches its launch `bi gpr2`(0x1CC0)
     * = spu.pc = savedContextLr (RPCS3 spursTasksetStartTask cellSpursSpu.cpp:1409),
     * but the band-aid coret/helper-return path left the launch registers corrupted,
     * so gpr2 != 0x3050 and gs_task never runs. The dispatch HAS loaded gs_task
     * (LoadElf DMA'd its code to LS 0x3000+ and set savedContextLr@0x2C80=0x3050),
     * so establish the exact launch ABI here (RPCS3 capture: gpr3=taskArgs, gpr4
     * word3=spurs, gpr2=entry for the `bi gpr2`, gpr5..127=0, sp=0x2c30, image=gs_task)
     * and let the bi transfer to 0x3050. Off-switch: YZ_NOLAUNCH. */
    if (a == 0x1CC0u && g_spu_cur_ctx && g_spu_cur_ctx->image_id == 2 && !getenv("YZ_NOLAUNCH")) {
        spu_context* c = g_spu_cur_ctx;
        const unsigned char* ls = c->ls;
        uint32_t scl = ((uint32_t)ls[0x2C80]<<24)|((uint32_t)ls[0x2C81]<<16)
                     |((uint32_t)ls[0x2C82]<<8)|ls[0x2C83];
        static int launched = 0;
        if (scl == 0x3050u && !launched) {
            launched = 1;
            memset(c->gpr, 0, 128 * sizeof(c->gpr[0]));
            c->gpr[3]._u32[0]=0x40197180u; c->gpr[3]._u32[1]=0x40176B10u;
            c->gpr[3]._u32[2]=0x000067F0u; c->gpr[3]._u32[3]=0;     /* taskArgs */
            c->gpr[4]._u32[3]=0x40197C80u;                          /* spurs */
            c->gpr[1]._u32[0]=0x2C30u;                              /* sp */
            c->gpr[2]._u32[0]=0x3050u;                              /* bi gpr2 -> gs_task entry */
            c->image_id = 0;                                        /* gs_task image */
            /* pt18 FIX: the START-path context setup (RPCS3 spursTasksetDispatch
             * cellSpursSpu.cpp:1787 sets ctxt->tasksetMgmtAddr=0x2700) is skipped by
             * our bandaid launch, leaving SpursTasksetContext.tasksetMgmtAddr (LS 0x2FB8)
             * = 0. gs_task 0xB6C0 derefs it as a syscall-handler vtable base
             * (call *(tasksetMgmtAddr+0xC4) = LS[0x27C4]=syscallAddr) -> base=0 -> bi 0
             * -> halt. Write the authoritative 0x2700 so gs_task's dispatch resolves.
             * Off-switch: YZ_NO_MGMT. */
            if (!getenv("YZ_NO_MGMT")) {
                c->ls[0x2FB8]=0x00; c->ls[0x2FB9]=0x00; c->ls[0x2FBA]=0x27; c->ls[0x2FBB]=0x00;
            }
        }
    }
    /* DIAG: the policy's post-select gate at LS 0x2468 polls a byte in the kernel
     * context tempArea (LS 0x100..0x180) and bails unless ==1. Dump that region
     * + the taskset bitmaps so we can see the value it waits on vs expected. */
    if (a == 0x2468u && g_spu_cur_ctx) {
        static int n = 0;
        if (n < 12) { n++;
            const unsigned char* ls = g_spu_cur_ctx->ls;
            /* 0x2468 = cellSpursModulePutTrace's trace gate, which runs AFTER
             * spursTasksetDispatch sets savedContextLr (LS 0x2C80 = entry) and
             * before StartTask. Dump savedContextLr + task_info (LS 0x2780, holds
             * the elf addr+low-bit flags the dispatch branches on) + taskId. */
            fprintf(stderr, "[pol-gate] spu=%X savedCtxLr@2C80:", g_spu_cur_ctx->spu_id);
            for (int i = 0; i < 8; i++) fprintf(stderr, " %02X", ls[(0x2C80 + i) & SPU_LS_MASK]);
            fprintf(stderr, " | elf@2790 (TaskInfo.elf, low bits=flags):");
            for (int i = 0; i < 8; i++) fprintf(stderr, " %02X", ls[(0x2790 + i) & SPU_LS_MASK]);
            fprintf(stderr, " | ctxsave@2798:");
            for (int i = 0; i < 8; i++) fprintf(stderr, " %02X", ls[(0x2798 + i) & SPU_LS_MASK]);
            fprintf(stderr, "\n");
        }
    }
    /* DIAG (YZ_SPU_PROF): the dispatch ELF-decision probe. The policy extracts
     * elfAddr (gpr81 = shlqbyi(gpr8,4) @ 0x186C), tests elfAddr&2 (gpr93 @ 0x1878,
     * the escape: RPCS3 cellSpursSpu.cpp:1803), masks+stores it to LS 0x2790, and
     * branches on gpr83 (0x1890). If elfAddr != 0x0127A580 it's mis-extracted
     * (byte-order in shlqbyi) and the whole start/escape decision is corrupted.
     * Dump the actual values on the policy spu (image 2). */
    if (g_spu_cur_ctx && g_spu_cur_ctx->image_id == 2
            && (a == 0x1870u || a == 0x187Cu || a == 0x1890u || a == 0x18B0u)) {
        static int de = 0;
        if (de < 20) { de++;
            spu_context* c = g_spu_cur_ctx;
            fprintf(stderr, "[elf-dec] pc=0x%04X gpr8=%08X_%08X gpr81(elfAddr)=%08X_%08X "
                    "gpr93(&2)=%08X gpr83=%08X gpr33=%04X\n", a,
                    c->gpr[8]._u32[0], c->gpr[8]._u32[1],
                    c->gpr[81]._u32[0], c->gpr[81]._u32[1],
                    c->gpr[93]._u32[0], c->gpr[83]._u32[0], c->gpr[33]._u16[0]);
            fflush(stderr);
        }
    }
    if ((++s_prof_hops % 8000000UL) == 0) spu_prof_dump();
}

void spu_register_function(uint32_t addr, spu_fn fn)
{
    if (s_registry_count < SPU_FN_REGISTRY_MAX) {
        s_registry[s_registry_count].addr = addr;
        s_registry[s_registry_count].fn = fn;
        s_registry[s_registry_count].image_id = s_reg_image;
        s_registry_count++;
    }
    spu_prof_insert(addr, fn);
}

static spu_fn spu_lookup(uint32_t addr, int image_id)
{
    /* Linear scan is fine for the small per-image tables. Match the context's
     * active image; image_id 0 (context or entry) matches any, for back-compat
     * with single-image contexts. */
    for (uint32_t i = 0; i < s_registry_count; i++)
        if (s_registry[i].addr == addr &&
            (image_id == 0 || s_registry[i].image_id == 0 ||
             s_registry[i].image_id == image_id))
            return s_registry[i].fn;
    return NULL;
}

/* Does a lifted function exist at this LS address (any image)? Lets the
 * lv2 layer decide between real SPU execution and PPU fallbacks. */
int spu_have_function(uint32_t addr)
{
    return spu_lookup(addr, 0) != NULL;
}

/* Classify an LS address by which lifted SPURS image owns it (diagnostic). */
static const char* spu_img_class(uint32_t pc)
{
    uint32_t a = pc & SPU_LS_MASK;
    if (a < 0xA00)   return "KERNEL";
    if (a < 0x3000)  return "SERVICE";
    if (a < 0xC000)  return "GSTASK";
    return "?";
}

void spu_indirect_branch(spu_context* ctx)
{
    /* DIAG (YZ_SPU_PROF): the StartTask divergence probe. After gs_task's ELF
     * loads, the policy splices a target into the LS 0x1E0 resume slot then
     * `bisl`s through it (policy 0x2308: lqa r2,0x1E0; bisl r2). RPCS3's HLE
     * (cellSpursSpu.cpp:1395 StartTask) proves the required target is the task
     * entry savedContextLr=0x3050. Dump, on every policy indirect branch, the
     * resolved target + the 16-byte LS 0x1E0 slot + LS 0x2C80 (savedContextLr):
     *   - target!=0x3050 & 0x1E0[0..3]!=0x3050  => the splice mis-computed the
     *     resume addr (byte-order class, chd/shufb/gbb) -- a value bug.
     *   - 0x1E0[0..3]==0x3050 but target still kernel => bisl/branch resolution.
     * This disambiguates lead (a) vs (b) in one run. */
    /* Fire only on the dtrace-armed spu (set on the gs_task ELF GET). The broad
     * image-2 0x838/0xA00 gate was removed: its flushed per-hop fprintf added
     * latency during the busy SPURS phase and shifted the boot thread-race. */
    if (g_spu_dtrace_spu >= 0 && (int)ctx->spu_id == g_spu_dtrace_spu) {
        static int st = 0;
        if (st < 120) { st++;
            const unsigned char* ls = ctx->ls;
            uint32_t tgt = ctx->pc & SPU_LS_MASK;
            fprintf(stderr, "[start-div] img%d -> 0x%05X lr=0x%05X | LS 0x1E0:",
                    ctx->image_id, tgt, ctx->gpr[0]._u32[0] & SPU_LS_MASK);
            for (int i = 0; i < 16; i++) fprintf(stderr, " %02X", ls[(0x1E0 + i) & SPU_LS_MASK]);
            /* Taskset bitsets (LS tempAreaTaskset 0x2700, CellSpursTaskset layout
             * cellSpurs.h:1060-1073) for task 0 = gs_task, whose bit is 1<<127 =
             * byte0 0x80. RESUME requires waiting&0x80 set + running&0x80 clear.
             * Word0 of each bitset is enough to see task 0. */
            fprintf(stderr, " | scl=%02X%02X%02X%02X run=%02X%02X%02X%02X rdy=%02X%02X%02X%02X "
                    "pry=%02X%02X%02X%02X sig=%02X%02X%02X%02X wait=%02X%02X%02X%02X lst=%02X",
                    ls[0x2C80],ls[0x2C81],ls[0x2C82],ls[0x2C83],
                    ls[0x2700],ls[0x2701],ls[0x2702],ls[0x2703],
                    ls[0x2710],ls[0x2711],ls[0x2712],ls[0x2713],
                    ls[0x2720],ls[0x2721],ls[0x2722],ls[0x2723],
                    ls[0x2740],ls[0x2741],ls[0x2742],ls[0x2743],
                    ls[0x2750],ls[0x2751],ls[0x2752],ls[0x2753],
                    ls[0x2773]);
            fprintf(stderr, "\n"); fflush(stderr);
        }
    }
    /* EXPERIMENT (env YZ_FORCE_START): the policy fully loads gs_task and sets
     * savedContextLr (LS 0x2C80 = entry 0x3050) but skips the StartTask `bi r0`
     * to the entry, returning to the kernel (pc=0x838) instead. When the policy
     * spu reaches that return with gs_task loaded, force the SPURS task-start ABI
     * (oracle cellSpursSpu.cpp:1395 spursTasksetStartTask: gpr3=taskArgs, gpr4 hi=
     * spurs, gpr2/5..127=0) and jump to the entry under the gs_task image -- to
     * drive the downstream pipeline (gs_task -> geometry @ io 0x1104D00 -> frame)
     * while the StartTask divergence is pinned for a proper fix. */
    if (getenv("YZ_FORCE_START") && (ctx->pc & SPU_LS_MASK) == 0x838u) {
        const unsigned char* ls = ctx->ls;
        uint32_t entry = ((uint32_t)ls[0x2C80] << 24) | ((uint32_t)ls[0x2C81] << 16)
                       | ((uint32_t)ls[0x2C82] << 8) | ls[0x2C83];
        static int launched = 0;
        if (entry == 0x3050u && !launched) {
            launched = 1;
            memset(ctx->gpr, 0, 128 * sizeof(ctx->gpr[0]));
            /* The LS-derived ABI was WRONG (2026-06-18 capture): our LS 0x2780 held
             * 41F00080_40C00100_00800100 (op-list base, not taskArgs) and LS 0x2C90 held
             * 0x3FFF0 (kernel-stack top) -- because our spursTasksetStartTask never runs to
             * completion to populate them. Use the EXACT register state RPCS3 launches gs_task
             * with (SPU debugger, pxd::CellSpursKernel2, PC=0x3050): these EAs are deterministic
             * (spurs is at 0x40197c80 in both). If gs_task now produces geometry, the launch
             * path is proven and the remaining work is sourcing these args from the taskset. */
            ctx->gpr[3]._u32[0] = 0x40197180u;   /* taskArgs (RPCS3 r3) */
            ctx->gpr[3]._u32[1] = 0x40176B10u;
            ctx->gpr[3]._u32[2] = 0x000067F0u;
            ctx->gpr[3]._u32[3] = 0x00000000u;
            ctx->gpr[4]._u32[3] = 0x40197C80u;   /* gpr4 word3 = spurs addr (RPCS3 r4) */
            uint32_t sp = 0x2C30u;               /* RPCS3 sp (taskset context stack) */
            ctx->gpr[1]._u32[0] = sp;
            ctx->image_id = 0;                                /* gs_task image */
            ctx->pc = 0x3050u;
            fprintf(stderr, "[force-start] gs_task @0x3050 (RPCS3 ABI) sp=0x%05X gpr3=%08X_%08X_%08X_%08X\n",
                    sp, ctx->gpr[3]._u32[0], ctx->gpr[3]._u32[1], ctx->gpr[3]._u32[2], ctx->gpr[3]._u32[3]);
            fflush(stderr);
        }
    }
    /* === SPURS taskset policy kernel<->workload RESUME (the gs_task launch fix) ===
     *
     * The policy's cellSpursModulePollStatus (0x2308) yields to the kernel at
     * 0x2318 via `bisl exitToKernelAddr`(0x838) (link 0x231C). In real SPURS /
     * RPCS3 LLE the workload is RESUMED at 0x231C after the kernel re-selects it,
     * so the poll completes -> StartTask -> `bi savedContextLr`(0x3050) launches
     * gs_task. OUR 0x838 hard-redispatches to 0xA00 (a fresh spursTasksetEntry),
     * so the policy re-STARTs the dispatch every cycle and NEVER reaches 0x231C /
     * the launch. RPCS3-capture confirmed (2026-06-18): RPCS3 reaches 0x231C and
     * runs the policy on its own taskset stack via ResumeTask(0xB60); ours loops
     * 0x2318->0xA00->0x2318 on the kernel stack. Bitset dump: gs_task is stuck
     * running, never waiting -- because it never launches even once.
     *
     * Model the resume: snapshot the policy's full register context at the yield;
     * when the kernel next dispatches the taskset workload (image 2) to 0xA00,
     * restore that context and resume at 0x231C instead of the fresh entry. The
     * kernel's SelectWorkload (run during the intervening yield) has updated
     * wklCurrentId, so the policy's 2nd poll call (0x290) returns the real status.
     * Kill-switch: YZ_NORESUME. */
    {
        /* GENERAL kernel<->workload coroutine resume. The taskset policy yields
         * to the kernel at MULTIPLE points (cellSpursModuleExit / poll-status),
         * each a `bisl exitToKernelAddr`(0x838) whose link (gpr0) is the policy
         * resume point. Real SPURS / RPCS3 LLE saves the full SPU context on the
         * yield and restores it when the kernel re-selects the workload, resuming
         * at the saved PC. Our 0x838 hard-redispatches to 0xA00 (fresh entry), so
         * the policy never resumes -> never reaches StartTask's bi savedContextLr.
         * Model it: on any image-2 yield to 0x838 with a policy-range link, snapshot
         * the context (resume PC = gpr0); on the next taskset re-dispatch to 0xA00
         * (kernel wklCurrentId==2), restore it and resume at that PC. The yield's
         * "return value" gpr3 is 0 on a clean yield (the policy does gpr80=gpr3
         * then `if (gpr80==0)` proceeds). Kill-switch: YZ_NORESUME. */
        /* Full workload context switch: save the policy's registers AND its LS
         * footprint (code+stack+context 0xA00..0x3000) at the yield, restore both
         * on re-dispatch, resume at the saved PC -- bypassing the fresh dispatch so
         * the system service's intervening LS clobber (kernel/service/taskset share
         * LS 0xA00+) can't leak in. RPCS3 (pt11) does this via the kernel<->workload
         * context switch (reaches ResumeTask 0xB60). */
        static int off = -1; if (off < 0) off = getenv("YZ_NORESUME") ? 1 : 0;
        if (!off && ctx->image_id == 2) {
            uint32_t lpc = ctx->pc & SPU_LS_MASK;
            uint32_t link = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
            uint32_t wcl = ((uint32_t)ctx->ls[0x1DC] << 24) | ((uint32_t)ctx->ls[0x1DD] << 16)
                         | ((uint32_t)ctx->ls[0x1DE] << 8) | ctx->ls[0x1DF];
            /* CLEAN COROUTINE RETURN (2026-06-19, replaces the snapshot-resume).
             * cellSpursModulePollStatus yields with `bisl exitToKernelAddr`(0x838)
             * at 0x2318 (link 0x231C). Our lifted 0x838 turns that into a
             * NON-RETURNING re-dispatch (bi 0xA00): (1) it destroys the poll's C
             * call stack so the poll's `bi lr`(0x2380) can't return up to StartTask;
             * (2) each re-dispatch reaches another bisl 0x838 and nests another
             * SPU_DRAIN -> the host stack grows until it overflows (the segfault).
             * The old snapshot-resume re-injected on a FRESH stack -> the poll
             * returned to the wrong frame (loop) and corrupted state over cycles.
             * FIX: for the poll yield, just RETURN from spu_indirect_branch without
             * running 0x838. The caller (0x2318) then continues at its hardcoded
             * link 0x231C on the SAME C stack with SP intact; SelectWorkload still
             * runs at 0x2340 (0x290) so the verdict is unchanged. The poll then
             * returns up the C stack to StartTask -> bi savedContextLr=0x3050.
             * gpr3=0 = the clean-yield result the poll reads (gpr80=gpr3, then
             * `if (gpr80==0)` proceeds). Kill-switch: YZ_NORESUME (re-dispatch). */
            if (lpc == 0x838u && wcl == 2u && link == 0x231Cu) {
                ctx->gpr[3]._u32[0] = ctx->gpr[3]._u32[1] = 0;
                ctx->gpr[3]._u32[2] = ctx->gpr[3]._u32[3] = 0;
                g_spu_trampoline_fn = 0;   /* no chain -> caller resumes at 0x231C */
                static int rl = 0;
                if (rl < 32) { rl++;
                    fprintf(stderr, "[yz-coret] poll yield 0x838 link=0x231C -> "
                            "return up C stack (skip re-dispatch)\n");
                    fflush(stderr);
                }
                return;
            }
            /* Force the poll-status 2nd call (0x2340 `bisl selectWorkloadAddr`,
             * link 0x2344) to report "current workload selected" when the taskset
             * (wid=2) is the running workload. cellSpursModulePollStatus computes
             * its verdict (policy 0x2374) as `selectedWid == wklCurrentId ? 0 : 1`;
             * gpr5 = the selected wid. Set gpr3.word0 = wklCurrentId so the verdict
             * is 0 (proceed -> StartTask -> bi savedContextLr=0x3050 launches
             * gs_task), and return to 0x2344 without re-running SelectWorkload (its
             * pick would yield to a higher-weight workload and loop forever). */
            else if (getenv("YZ_POLLFORCE") && lpc == 0x290u
                     && (ctx->gpr[0]._u32[0] & SPU_LS_MASK) == 0x2344u) {
                /* OFF by default: forcing the poll to proceed does NOT launch
                 * gs_task (verified -- the policy loops regardless of the poll
                 * result, i.e. it idle-waits for gs_task work that isn't ready
                 * during the pre-menu movie). gs_task is not the producer of the
                 * io 0x1104D00 display list at this phase; the game's PPU render
                 * thread is, stuck in a producer/consumer circular wait. */
                uint32_t wcl = ((uint32_t)ctx->ls[0x1DC] << 24) | ((uint32_t)ctx->ls[0x1DD] << 16)
                             | ((uint32_t)ctx->ls[0x1DE] << 8) | ctx->ls[0x1DF];
                if (wcl == 2u) {
                    ctx->gpr[3]._u32[0] = wcl;             /* selectedWid == wklCurrentId */
                    ctx->gpr[3]._u32[1] = ctx->gpr[3]._u32[2] = ctx->gpr[3]._u32[3] = 0;
                    ctx->pc = 0x2344u;                     /* return past the bisl */
                    static int pl = 0;
                    if (pl < 8) { pl++;
                        fprintf(stderr, "[yz-poll] force poll proceed (wid=2 == current) -> launch\n");
                        fflush(stderr);
                    }
                }
            }
        }
    }
    /* DIAG (YZ_SPU_PROF): gs_task launch + flow. gs_task loads at LS 0x3000
     * (entry 0x3050, code .. 0xBC00); it gets its EDGE-job pointer in gpr3 and
     * taskset args/spurs in gpr4 (SPURS task-start ABI, cellSpursSpu.cpp:1395).
     * Log its indirect branches (uncapped by the 160-entry [spu-ib] cap) so we
     * can see the arg it launched with and where it spins. */
    if (g_spu_prof_on && (ctx->pc & SPU_LS_MASK) >= 0x3000u
            && (ctx->pc & SPU_LS_MASK) < 0xBC00u) {
        static int gst = 0;
        if (gst < 60) { gst++;
            fprintf(stderr, "[gstask] ib pc=0x%05X arg(gpr3)=%08X_%08X_%08X_%08X "
                    "gpr4=%08X_%08X_%08X_%08X lr=0x%05X\n", ctx->pc & SPU_LS_MASK,
                    ctx->gpr[3]._u32[0], ctx->gpr[3]._u32[1], ctx->gpr[3]._u32[2], ctx->gpr[3]._u32[3],
                    ctx->gpr[4]._u32[0], ctx->gpr[4]._u32[1], ctx->gpr[4]._u32[2], ctx->gpr[4]._u32[3],
                    ctx->gpr[0]._u32[0] & SPU_LS_MASK);
        }
    }
    /* STARTTASK HELPER RETURN (2026-06-19). The taskset StartTask C-calls the
     * GUID-trace+poll helper at 0x1C50 (`spu_polfunc_000013B0(ctx); SPU_DRAIN`).
     * The helper ends `bi gpr7`(0x14CC), but gpr7 was clobbered to the trace LSA
     * 0x10000 by the trace block at 0x14A0 (the lifted epilogue never restores it;
     * our coret skip of the 0x838 kernel coroutine left the return linkage there).
     * 0x10000 is NOT valid policy code (policy spans 0xA00-0x2840), so a bi there
     * in image 2 IS that clobbered return: stop the trampoline chain so SPU_DRAIN
     * unwinds back to the C caller (StartTask 0x1C50 -> 0x1C54 -> ... -> 0x1CC0
     * `bi savedContextLr=0x3050` = the gs_task launch). Without this the policy SPU
     * halts at 0x10000 and gs_task never executes. Env off-switch: YZ_NOHELPRET. */
    if ((ctx->pc & SPU_LS_MASK) == 0x10000u && ctx->image_id == 2 && !getenv("YZ_NOHELPRET")) {
        g_spu_trampoline_fn = 0;   /* C-unwind: SPU_DRAIN returns to StartTask 0x1C54 */
        static int rr = 0; if (rr < 16) { rr++;
            fprintf(stderr, "[yz-helpret] StartTask helper bi gpr7=0x10000 (clobbered) "
                    "-> C-unwind to 0x1C54 -> launch path\n");
            fflush(stderr); }
        return;
    }
    spu_fn fn = spu_lookup(ctx->pc, ctx->image_id);
    if (fn) {
        /* DIAG (YZ_SPU_PROF): trace the cross-image dispatch cycle. The SPURS
         * scheduler loop indirect-branches between the kernel, system service
         * and (unexpectedly) the geometry task -- log target + apparent caller
         * (SPU lr = gpr0) + image class to pin where it crosses images. */
        if (g_spu_prof_on) {
            static unsigned long ib_n = 0;
            unsigned long n = ib_n++;
            if (n < 160 || (n % 2000000UL) == 0)
                fprintf(stderr, "[spu-ib] spu=%X -> 0x%05X (%s) lr=0x%05X\n",
                        ctx->spu_id, ctx->pc & SPU_LS_MASK, spu_img_class(ctx->pc),
                        ctx->gpr[0]._u32[0] & SPU_LS_MASK);
        }
        fn(ctx);
        return;
    }
    /* TEMP DEBUG (7d): on the first few unknown branches, dump what's at the
     * target LS — distinguishes "real code DMA'd in at runtime" (must lift)
     * from "zero/garbage" (bad dispatch). Strip once workloads run. */
    {
        static int unk_log = 6;
        if (unk_log > 0) {
            unk_log--;
            uint32_t a = ctx->pc & SPU_LS_MASK;
            fprintf(stderr, "[SPU] unknown branch full_pc=0x%08X LS 0x%05X (image %d) "
                    "gpr0=%08X_%08X gpr1=%08X_%08X gpr2=%08X_%08X bytes:",
                    ctx->pc, a, ctx->image_id,
                    ctx->gpr[0]._u32[0], ctx->gpr[0]._u32[1],
                    ctx->gpr[1]._u32[0], ctx->gpr[1]._u32[1],
                    ctx->gpr[2]._u32[0], ctx->gpr[2]._u32[1]);
            for (int i = 0; i < 32; i++) fprintf(stderr, " %02X", ctx->ls[(a + i) & SPU_LS_MASK]);
            fprintf(stderr, "\n");
            fflush(stderr);
        }
    }
    spu_halt(ctx, SPU_STATUS_STOPPED_BY_HALT);   /* unwinds; does not return */
}

/* ===========================================================================
 * Execution trace (for §3 validation: diff vs RPCS3 SPU interpreter)
 *
 * When the lifter is invoked with --trace, every emitted instruction is
 * surrounded by spu_trace_pc(ctx, PC) before execution and spu_trace_rt(
 * ctx, RT) after, for instructions whose destination is the rt slot. The
 * output is one line per event:
 *
 *     <PC-5hex>                          - PC about to execute
 *       r<rt> <hi-64hex> <lo-64hex>      - register written, post-state
 *
 * Direct to stderr by default; call spu_trace_init(path) once at startup
 * to redirect to a file. The format is intentionally minimal and stable
 * so a small converter can line it up against an RPCS3.log SPU trace.
 * ===========================================================================*/
static FILE* s_trace_fp = NULL;
/* Bounded trace gating (YZ_SPU_PROF): trace ONE SPU (0x2000), only in steady
 * state (after the deadlock forms), for a bounded window -- otherwise a
 * per-instruction trace of 5 SPUs spinning millions/sec is unusable. */
static int   s_trace_armed  = 0;
static long  s_trace_budget = 0;

void spu_trace_init(const char* path)
{
    if (!path || !*path) { s_trace_fp = stderr; return; }
    s_trace_fp = fopen(path, "w");
    if (!s_trace_fp) s_trace_fp = stderr;
}

void spu_trace_pc(spu_context* ctx, uint32_t pc)
{
    extern int g_spu_prof_on;
    extern unsigned long g_spu_getllar_n;
    extern uint8_t* vm_base;
    if (!g_spu_prof_on) return;
    if (ctx->spu_id != 0x2000u) return;          /* one SPU, no interleave */
    if (!s_trace_armed) {
        if (g_spu_getllar_n < 8000000UL) return; /* wait for post-CreateTask2 steady state */
        s_trace_armed  = 1;
        s_trace_budget = 6000;
        if (!s_trace_fp) s_trace_fp = fopen("scratch/spu_trace.txt", "w");
        if (!s_trace_fp) s_trace_fp = stderr;
        uint8_t ssm = vm_base[(uint32_t)(0x40197C80u + 0x72u)];
        fprintf(s_trace_fp, "# armed getllar=%lu sysSrvMessage=0x%02X spu=%X\n",
                g_spu_getllar_n, ssm, ctx->spu_id);
    }
    if (s_trace_budget <= 0) { if (s_trace_fp) fflush(s_trace_fp); return; }
    s_trace_budget--;
    fprintf(s_trace_fp, "%05X\n", pc & SPU_LS_MASK);
}

void spu_trace_rt(spu_context* ctx, uint32_t rt)
{
    if (!s_trace_armed || s_trace_budget <= 0 || !s_trace_fp) return;
    if (ctx->spu_id != 0x2000u) return;
    u128 v = ctx->gpr[rt & 0x7F];
    fprintf(s_trace_fp, "  r%-3u %016llX %016llX\n",
            (unsigned)(rt & 0x7F),
            (unsigned long long)v._u64[0],
            (unsigned long long)v._u64[1]);
}

#ifdef __cplusplus
}
#endif
