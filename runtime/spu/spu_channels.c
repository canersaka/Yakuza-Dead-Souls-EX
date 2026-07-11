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
#include "spu_helpers.h"   /* spu_rotqbyi: the real spursTasksetStartTask gpr4 seed */
#include "../../include/ps3emu/error_codes.h"   /* CELL_EBUSY: send_event ack mapping */
/* Generated EBOOT SPU image registry (tools/gen_spu_images.py): elf EA ->
 * image id / entry / BSS spans. Generated into recomp_prx like the lifted
 * kernels the build already requires. */
#include "../../recomp_prx/spu_image_table.h"
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

/* Host-call-depth guard restart target + stack base. The lifter emits SPU
 * `brsl`/`bisl` (CALL) as NESTED host calls; a SPURS coroutine/poll yield that
 * tail-branches away from a CALL without ever returning (e.g. gs_task's idle
 * poll at LS 0xB6C0) leaks the host frame each iteration -> the 64MB SPU thread
 * stack overflows (the documented "brsl call/return nesting imbalance"). When
 * the host stack grows past a safe threshold, spu_indirect_branch longjmps to
 * g_spu_restart_jmp; the driver (lv2_register.c) re-dispatches from ctx->pc on a
 * fresh stack. All SPU state is in the heap `ctx`, so it survives -- this bounds
 * the host stack the way the SPU's own 256KB LS stack bounds it on hardware. */
#if defined(_MSC_VER)
__declspec(thread) jmp_buf* g_spu_restart_jmp = 0;
__declspec(thread) char*    g_spu_stack_base  = 0;
#else
_Thread_local jmp_buf* g_spu_restart_jmp = 0;
_Thread_local char*    g_spu_stack_base  = 0;
#endif

/* YZ_CTXWATCH (2026-07-10 s26 ⚡1, diag — DONT_RECHASE #53/#54): taskset
 * context-save EA watch table. s25ride12 published the decode counter val=1
 * TWICE ([fe0]) — the pool task re-ran with STALE STATE between WAIT_SIGNAL
 * cycles. The task context round-trips main memory through the taskInfo's
 * ctxsave EA (be64 @LS 0x2798): the yield's plain-PUT saves LS 0x2C80.. to
 * EA+0, the resume restores from it (host memcpy in spu_task_launch below,
 * plus optionally Sony's own GET). spu_task_launch registers each observed
 * ctx block here; the spu_dma.h watch logs every DMA touching one, so
 * "no SAVE between two RESUMEs" (double-resume) separates mechanically from
 * "SAVE landed but the restore read stale bytes". Registration is benign-race
 * append-only (diag; worst case a duplicate row). */
uint32_t g_yz_ctxw_ea[32];
uint32_t g_yz_ctxw_len[32];
uint32_t g_yz_ctxw_ts[32];
uint32_t g_yz_ctxw_tid[32];
volatile int g_yz_ctxw_n = 0;

static uint32_t yz_ctxw_hash(const uint8_t* p, uint32_t n)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static void yz_ctxw_register(uint32_t ea, uint32_t len, uint32_t ts, uint32_t tid)
{
    for (int i = 0; i < g_yz_ctxw_n && i < 32; i++)
        if (g_yz_ctxw_ea[i] == ea) return;
    if (g_yz_ctxw_n >= 32) return;
    int i = g_yz_ctxw_n;
    g_yz_ctxw_ea[i] = ea; g_yz_ctxw_len[i] = len;
    g_yz_ctxw_ts[i] = ts; g_yz_ctxw_tid[i] = tid;
    g_yz_ctxw_n = i + 1;
    fprintf(stderr, "[ctxw] REG ctx=0x%08X len=0x%X taskset=0x%08X taskId=%u\n",
            ea, len, ts, tid);
    fflush(stderr);
}

/* One task launch/resume observation: register the ctxsave block + log the
 * cycle with the register-block hash AS READ FROM MAIN MEMORY (comparable
 * with the spu_dma.h LOAD hashes). Called from BOTH launch paths — the legacy
 * spu_task_launch (assisted launch) and spu_indirect_branch's natural
 * StartTask image-switch (the path the wid4 pool tasks actually take;
 * s26ride2 measured zero registrations from the legacy hook alone). Returns
 * quietly when YZ_CTXWATCH is off. */
static int g_yz_ctxw_armed = -1;
static void yz_ctxw_cycle(spu_context* c, int image, uint32_t tpc, int is_resume)
{
    if (g_yz_ctxw_armed < 0) { g_yz_ctxw_armed = getenv("YZ_CTXWATCH") ? 1 : 0;
        if (g_yz_ctxw_armed) { fprintf(stderr, "[ctxw] ARMED: taskset ctx save/restore watch live\n"); fflush(stderr); } }
    if (!g_yz_ctxw_armed) return;
    const unsigned char* ls = c->ls;
    const unsigned char* ti = ls + 0x2798u;
    uint64_t css = ((uint64_t)ti[0]<<56)|((uint64_t)ti[1]<<48)
                 |((uint64_t)ti[2]<<40)|((uint64_t)ti[3]<<32)
                 |((uint64_t)ti[4]<<24)|((uint64_t)ti[5]<<16)
                 |((uint64_t)ti[6]<<8)|ti[7];
    uint32_t ctx_ea = (uint32_t)(css & ~0x7Full);
    uint32_t ts  = ((uint32_t)ls[0x27BC]<<24)|((uint32_t)ls[0x27BD]<<16)
                 |((uint32_t)ls[0x27BE]<<8)|ls[0x27BF];
    uint32_t tid = ((uint32_t)ls[0x27D4]<<24)|((uint32_t)ls[0x27D5]<<16)
                 |((uint32_t)ls[0x27D6]<<8)|ls[0x27D7];
    if (ctx_ea)
        yz_ctxw_register(ctx_ea, 0x400u + (uint32_t)(css & 0x7Fu) * 0x800u, ts, tid);
    /* Per-image budgets: gs_task (image 0) re-enters its same-SPU idle poll
     * thousands of times and ate the whole print budget in s26ride3,
     * suppressing exactly the pool-task (image 4) cycles the instrument
     * exists for (LESSONS #21). */
    static unsigned long crn_img0 = 0, crn = 0;
    if (image == 0) { crn_img0++;
        if (!(crn_img0 <= 50 || (crn_img0 & 0x3FFu) == 0)) return; }
    else crn++;
    if (image == 0 || crn <= 4000 || (crn & 0xFFu) == 0) {
        uint32_t h = 0;
        if (ctx_ea) { extern uint8_t* vm_base;
                      h = yz_ctxw_hash(vm_base + ctx_ea, 0x380u); }
        fprintf(stderr, "[ctxw] %s n=%lu spu=%X img=%d taskset=0x%08X taskId=%u "
                "scl=0x%05X ctx=0x%08X h=%08X gpr0=%08X sp=%08X g80=%08X g81=%08X\n",
                is_resume ? "RESUME" : "START",
                image == 0 ? crn_img0 : crn, c->spu_id, image, ts, tid,
                tpc, ctx_ea, h,
                c->gpr[0]._u32[0], c->gpr[1]._u32[0],
                c->gpr[80]._u32[0], c->gpr[81]._u32[0]);
        fflush(stderr);
    }
}

/* YZ_DEFER_PROBE handler (s26, ledger #48 value hunt — injected into
 * func_00E9BE4C by scratch/patch_defer_probe.py): logs both operands of the
 * gcm flush's defer-vs-immediate release decision + the branch taken. The
 * healthy oracle takes IMMEDIATE (ledger #54①); ours chronically DEFERs. */
void yz_defer_probe(unsigned s24, unsigned cend, unsigned s, unsigned p, int imm)
{
    static int dp = -1;
    if (dp < 0) { dp = getenv("YZ_DEFER_PROBE") ? 1 : 0;
        if (dp) { fprintf(stderr, "[defer] ARMED (E9BE4C compare probe)\n"); fflush(stderr); } }
    if (!dp) return;
    static unsigned long dn = 0; dn++;
    if (dn <= 60 || (dn & 0x3FFu) == 0) {
        fprintf(stderr, "[defer] n=%lu S24=0x%08X end=0x%08X -> %s (S=0x%08X P=0x%08X)\n",
                dn, s24, cend, imm ? "IMMEDIATE" : "DEFER", s, p);
        fflush(stderr);
    }
}

/* ---- s31 W2LIFE probe (ledger #70/#71): SPURS wid accounting + per-SPU liveness.
 * The journal-consumer death (scratch/s31_consumer_death.md) leaves wid2 (the
 * gs_task taskset hosting the gcm journal consumer) permanently undispatched
 * after its first contended workload switch at the CRI bring-up. This dump
 * captures, from PPU-visible main memory, everything the kernel's SelectWorkload
 * eligibility test reads (RPCS3 cellSpursSpu.cpp:283-346 semantics; CellSpurs
 * field offsets from cellSpurs.h:667-740): readyCount1/current/pending/max
 * contention, wklState1, wklSignal1, sysSrvMsgUpdateWorkload, wid2's per-SPU
 * priorities -- plus the gs_task taskset bitsets and a per-SPU host-liveness
 * census (hops/lastpc/lastimg, maintained in spu_indirect_branch). Env
 * YZ_W2LIFE, armed banner, hard 64-dump cap. Callers: the coret poll-yield
 * site, the exit-unwind site (below), the park-rel lever apply
 * (import_overrides.cpp), the stall watchdog (main.cpp). */
volatile unsigned long long g_yz_spu_hops[8];
volatile unsigned int       g_yz_spu_lastpc[8];
volatile int                g_yz_spu_lastimg[8];

void yz_w2life_dump(const char* tag)
{
    static int w2 = -1;
    if (w2 < 0) { w2 = getenv("YZ_W2LIFE") ? 1 : 0;
        if (w2) { fprintf(stderr, "[w2life] ARMED (SPURS wid accounting + per-SPU liveness)\n");
                  fflush(stderr); } }
    if (!w2) return;
    static int wn = 0;
    if (wn >= 64) return;
    wn++;
    {
        extern uint8_t* vm_base;
        const uint8_t* S = vm_base + 0x40197C80u;  /* CellSpurs (measured constant:
                                                    * wklSignal1@+0x70 = 0x40197CF0) */
        const uint8_t* T = vm_base + 0x42100080u;  /* gs_task taskset (bitsets +0x00..+0x50) */
        const uint8_t* P = S + 0xB00u + 2u * 0x20u + 0x18u;  /* wklInfo1[2].priority[8] */
        fprintf(stderr,
            "[w2life:%s] rdy=%02X%02X%02X%02X%02X cur=%02X%02X%02X%02X%02X "
            "pnd=%02X%02X%02X%02X%02X max=%02X%02X%02X%02X%02X st=%02X%02X%02X%02X%02X "
            "sig1=%02X%02X srvmsg=%02X pri2=%02X%02X%02X%02X%02X%02X%02X%02X\n",
            tag,
            S[0x00],S[0x01],S[0x02],S[0x03],S[0x04],
            S[0x20],S[0x21],S[0x22],S[0x23],S[0x24],
            S[0x30],S[0x31],S[0x32],S[0x33],S[0x34],
            S[0x50],S[0x51],S[0x52],S[0x53],S[0x54],
            S[0x80],S[0x81],S[0x82],S[0x83],S[0x84],
            S[0x70],S[0x71], S[0xBD],
            P[0],P[1],P[2],P[3],P[4],P[5],P[6],P[7]);
        fprintf(stderr,
            "[w2life:%s] ts run=%02X%02X%02X%02X rdy=%02X%02X%02X%02X prdy=%02X%02X%02X%02X "
            "en=%02X%02X%02X%02X sig=%02X%02X%02X%02X wait=%02X%02X%02X%02X wid=%02X |"
            " hops 0-5: %llu/%04X/%d %llu/%04X/%d %llu/%04X/%d %llu/%04X/%d %llu/%04X/%d %llu/%04X/%d\n",
            tag,
            T[0x00],T[0x01],T[0x02],T[0x03], T[0x10],T[0x11],T[0x12],T[0x13],
            T[0x20],T[0x21],T[0x22],T[0x23], T[0x30],T[0x31],T[0x32],T[0x33],
            T[0x40],T[0x41],T[0x42],T[0x43], T[0x50],T[0x51],T[0x52],T[0x53],
            T[0x74],
            g_yz_spu_hops[0], g_yz_spu_lastpc[0], g_yz_spu_lastimg[0],
            g_yz_spu_hops[1], g_yz_spu_lastpc[1], g_yz_spu_lastimg[1],
            g_yz_spu_hops[2], g_yz_spu_lastpc[2], g_yz_spu_lastimg[2],
            g_yz_spu_hops[3], g_yz_spu_lastpc[3], g_yz_spu_lastimg[3],
            g_yz_spu_hops[4], g_yz_spu_lastpc[4], g_yz_spu_lastimg[4],
            g_yz_spu_hops[5], g_yz_spu_lastpc[5], g_yz_spu_lastimg[5]);
        fflush(stderr);
    }
}

/* Tail-call trampoline target (see spu_context.h / SPU_DRAIN). Set by a
 * cross-function tail branch in lifted SPU code; drained iteratively by the
 * enclosing call site or the host-thread driver. */
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*) = 0;
SPU_THREAD_LOCAL spu_context* g_spu_cur_ctx = 0;
int g_spu_dtrace_spu = -1;   /* spu_id whose dispatch tail to hop-trace after gs_task ELF load (-1=off) */
int g_yz_codec_dispatch_spu = -1;  /* pt35: spu_id running the codec policy dispatch (armed at codec ELF load) */

void spu_halt(spu_context* ctx, int status)
{
    ctx->status = (uint32_t)status;
    /* pt35 (env YZ_HALT_LOG): log SPU halts -- spursTasksetLoadElf/dispatch failures
     * call spursHalt(spu), which lands here. Shows whether the codec policy SPU halts
     * after LoadElf (the stuck-running symptom) and at what PC. */
    { static int hl = -1; if (hl < 0) hl = getenv("YZ_HALT_LOG") ? 1 : 0;
      if (hl) { static int n = 0; if (n < 40) { n++;
          fprintf(stderr, "[spu-halt] spu=%X image=%d pc=0x%05X status=%d\n",
                  ctx->spu_id, ctx->image_id, ctx->pc & SPU_LS_MASK, status);
          fflush(stderr); } } }
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
        SPU_CPU_RELAX();   /* don't saturate the cache line while spinning */
}
void spu_lockline_unlock(void)
{
    atomic_flag_clear_explicit(&s_lockline, memory_order_release);
}

/* Host-core yield for SPU idle-poll loops (the spu_dma.h GETLLAR backoff;
 * kill-switch YZ_NO_SPUBACKOFF lives at the call site). level 0 = cpu pause
 * (stay hot, just stop hammering the lock-line lock at full rate); level 1 =
 * hand the core to the OS scheduler for a quantum (still hot when cores are
 * idle -- SwitchToThread returns immediately with no waiter); level 2 = a
 * real 1 ms sleep, the only rung that actually idles the core (laptop
 * heat/throttling; up to ~15 ms wall with the default timer resolution --
 * acceptable reaction latency for an SPU that has been idle this long).
 * Measured before the backoff: five SPURS kernel SPUs each burned ~97% of a
 * host core in their GETLLAR poll loops, and boot pacing collapsed under the
 * lock-line contention (see STATUS 2026-07-03); pacing recovered with the
 * pause/yield rungs alone (lock RATE, not occupancy, was the poison). */
#if defined(_WIN32)
__declspec(dllimport) int __stdcall SwitchToThread(void);
__declspec(dllimport) void __stdcall Sleep(unsigned long ms);
#else
#include <sched.h>
#include <unistd.h>
#endif
void spu_idle_yield(int level)
{
    if (level == 0) {
        SPU_CPU_RELAX(); SPU_CPU_RELAX(); SPU_CPU_RELAX(); SPU_CPU_RELAX();
        return;
    }
#if defined(_WIN32)
    if (level >= 2) Sleep(1);
    else SwitchToThread();
#else
    if (level >= 2) usleep(1000);
    else sched_yield();
#endif
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

/* Per-128B-line write GENERATION (mirrors RPCS3 vm::reservation_acquire). Bumped on
 * every PPU coherence write to a reserved line; GETLLAR snapshots it and LR is
 * RE-DERIVED at the next GETLLAR by comparison -- so a write that lands while no SPU
 * reservation is active (the window between PUTLLC and the next GETLLAR) is NOT lost.
 * This closes the lost-wakeup that made the SPURS codec dispatch a boot coin-flip. */
static uint32_t s_coh_gen[(SPU_COH_HI - SPU_COH_LO) >> 7];
uint32_t spu_coh_gen(uint32_t addr)
{
    if (addr - SPU_COH_LO >= SPU_COH_HI - SPU_COH_LO) return 0;
    return s_coh_gen[(addr - SPU_COH_LO) >> 7];
}

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
    if (line - SPU_COH_LO < SPU_COH_HI - SPU_COH_LO)
        s_coh_gen[(line - SPU_COH_LO) >> 7]++;   /* generation bump -- the missed-edge fix */
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
    case SPU_WrOutIntrMbox:
        /* DIAG (env YZ_INTRMBOX_LOG, 2026-07-04 -- pure diagnosis, log every
         * class-2 doorbell write UNCONDITIONALLY before any routing decision,
         * answering: does the SPU ever target port 17 / queue 2 (t1's SPURS
         * event-flag queue, measured via group_connect_event_all_threads
         * group=0x1000 queue=0x2 -> port=17), and if so is it routed or
         * dropped. Decodes the same bit layout the routing code below uses
         * (RPCS3 SPUThread.cpp:5969-6090: code = v>>24; code<64 send_event,
         * 64..127 throw_event, 128/192 sys_event_flag set_bit); peeks
         * ch_out_mbox without popping it (spu_group_spup_queue is pure/
         * read-only, safe to call speculatively here even for codes the
         * routing logic below won't treat as a queue doorbell). */
        { static int intrmbox_log = -1;
          if (intrmbox_log < 0) intrmbox_log = getenv("YZ_INTRMBOX_LOG") ? 1 : 0;
          if (intrmbox_log) {
              static unsigned long n = 0;
              if (n < 200) {
                  n++;
                  uint32_t dcode = v >> 24;
                  uint32_t dspup = dcode & 63;
                  extern uint32_t spu_group_spup_queue(uint32_t group_id, uint32_t spup);
                  uint32_t dqid = (dcode < 128) ? spu_group_spup_queue(ctx->spu_group_id, dspup) : 0;
                  const char* path;
                  if (dcode < 128)
                      path = dqid ? (dcode < 64 ? "send_event(routed)" : "throw_event(routed)")
                                   : "DROPPED(no queue bound to port)";
                  else if (dcode == 128 || dcode == 192)
                      path = "eflag_set_bit";
                  else
                      path = "unrouted(buffered)";
                  fprintf(stderr,
                      "[intrmbox-log] #%lu spu_id=0x%X group=0x%X v=0x%08X code=%u "
                      "port(spup)=%u -> queue=%u path=%s\n",
                      n, ctx->spu_id, ctx->spu_group_id, v, dcode, dspup, dqid, path);
                  fflush(stderr);
              }
          } }
        /* sys_spu_thread_send_event / throw_event delivery (2026-07-02; closes
         * the pt29 "NOT forwarded yet" gap). RPCS3 SPUThread.cpp
         * SPU_WrOutIntrMbox: code = v>>24; code<64 = send_event (acks CELL_OK
         * into the SPU InMbox), 64..127 = throw_event (no ack); both pop data
         * from the OutMbox (written just before) and deliver
         * {SYS_SPU_THREAD_EVENT_USER_KEY, spu_id, (spup<<32)|(v&0xFFFFFF), data}
         * to the event queue bound to the group's port (connect_event_all_
         * threads / cellSpursQueueAttachLv2EventQueue). MEASURED need: Sony's
         * SPU-side SpursQueue push wakes a BLOCKING PPU popper (t1 parked in
         * sys_event_queue_receive inside cellSpursQueuePopBody) by throwing on
         * the queue's attached port (v=0x53000000 -> port 0x13) -- dropping it
         * left the CRI voice-init response unconsumed forever. */
        { uint32_t code = v >> 24;
          if (code < 128 && ctx->ch_out_mbox.count) {
              extern int yz_throw_latch_add(uint32_t qid, uint64_t src,
                                            uint64_t d1, uint64_t d2, uint64_t d3);
              uint32_t spup = code & 63;
              extern uint32_t spu_group_spup_queue(uint32_t group_id, uint32_t spup);
              extern int sys_event_queue_push_by_id(uint32_t queue_id, uint64_t source,
                                                    uint64_t d1, uint64_t d2, uint64_t d3);
              uint32_t qid = spu_group_spup_queue(ctx->spu_group_id, spup);
              if (qid) {
                  uint32_t data = spu_channel_read(&ctx->ch_out_mbox);
                  int rc = sys_event_queue_push_by_id(qid,
                              0xFFFFFFFF53505501ULL /* SYS_SPU_THREAD_EVENT_USER_KEY */,
                              ctx->spu_id,
                              ((uint64_t)spup << 32) | (v & 0xFFFFFFu),
                              data);
                  /* runtime_cbea_audit.md sys_event slice item 1 (this file,
                   * originally L294-295): the ack MUST reflect the push result,
                   * not a hardcoded CELL_OK -- RPCS3 SPUThread.cpp:6052
                   * (`atomic_storage<u32>::release(ch_in_mbox.values..., res)`)
                   * writes the queue->send() RESULT into the SPU's InMbox for
                   * sys_spu_thread_send_event, not a fixed success code, so a
                   * full/dead destination queue is visible to the guest instead
                   * of being silently reported as delivered. Our push helper's
                   * only failure mode past this point (qid already resolved to
                   * a live queue by spu_group_spup_queue above) is a full ring
                   * buffer (sys_event.c event_queue_push returns -1 there) --
                   * this file's OWN existing convention for that same -1 (see
                   * sys_event_port_send, sys_event.c:642-643) is CELL_EBUSY, so
                   * reuse it rather than inventing a new mapping. */
                  if (code < 64)                       /* send_event acks the SPU */
                      spu_channel_write(&ctx->ch_in_mbox,
                          rc == 0 ? 0u /* CELL_OK */ : (uint32_t)(int32_t)CELL_EBUSY);
                  /* s25 (notification-surface audit risk #3): throw_event
                   * (code>=64) has NO ack path by protocol — on real HW a
                   * full destination queue means the event is simply lost,
                   * and the game only survives because its consumer keeps
                   * up. Our timing widens the full-queue window, so latch
                   * the failed throw and redeliver when the queue drains
                   * (vblank-tick flush). The event WAS thrown; only its
                   * delivery timing shifts. Loud per-latch log so it is
                   * visible whenever this actually fires. Kill-switch
                   * YZ_NO_THROW_RETRY restores the faithful drop. */
                  if (code >= 64 && rc != 0) {
                      static int ntr = -1;
                      if (ntr < 0) ntr = getenv("YZ_NO_THROW_RETRY") ? 1 : 0;
                      if (!ntr) {
                          int ok = yz_throw_latch_add(qid,
                                      0xFFFFFFFF53505501ULL, ctx->spu_id,
                                      ((uint64_t)spup << 32) | (v & 0xFFFFFFu), data);
                          fprintf(stderr, "[throw-lat] spup=%u queue %u FULL -> "
                                  "%s (rc=%d)\n", spup, qid,
                                  ok ? "LATCHED for redelivery" : "latch FULL, LOST",
                                  rc);
                          fflush(stderr);
                      }
                  }
                  { static unsigned long n = 0;
                    if (n < 24) { n++;
                        fprintf(stderr, "[SPU] %s spup=%u data0=0x%X data1=0x%X -> queue %u (rc=%d)\n",
                                code < 64 ? "send_event" : "throw_event",
                                spup, v & 0xFFFFFFu, data, qid, rc);
                        fflush(stderr); } }
                  break;
              }
          }
          /* sys_event_flag set_bit protocol (codes 128 strict / 192 impatient
           * -- RPCS3 SPUThread.cpp :6090-6160): the SPU sets bit (v&0xFFFFFF)
           * of the lv2 EVENT FLAG whose id it wrote to OutMbox just before.
           * MEASURED need (2026-07-03): Sony's SPURS taskset EXIT HANDLER
           * (libsre blob at LS 0x10000) fires code 192 as its task-exit
           * completion doorbell -- dropping it left the pxd job layer
           * thinking its service task never finished (the entry-7 shader-
           * stream gate). Strict form acks the setter's return code into the
           * SPU InMbox; impatient form doesn't ack. */
          if ((code == 128 || code == 192) && ctx->ch_out_mbox.count) {
              extern int64_t sys_event_flag_set_by_id(uint32_t flag_id, uint64_t bitpat);
              uint32_t bit  = v & 0xFFFFFFu;
              uint32_t fid  = spu_channel_read(&ctx->ch_out_mbox);
              int64_t rc = (bit < 64)
                  ? sys_event_flag_set_by_id(fid, 1ull << bit)
                  : (int64_t)(int32_t)0x80010002 /* CELL_EINVAL */;
              if (code == 128)
                  spu_channel_write(&ctx->ch_in_mbox, (uint32_t)rc);
              { static unsigned long n = 0;
                if (n < 24) { n++;
                    fprintf(stderr, "[SPU] eflag_set_bit%s id=%u bit=%u (rc=0x%X)\n",
                            code == 192 ? "_impatient" : "", fid, bit, (uint32_t)rc);
                    fflush(stderr); } }
              break;
          }
          { static unsigned long n = 0;
            if (n < 12) { n++;
                fprintf(stderr, "[SPU] WrOutIntrMbox v=0x%08X (no bound queue -- buffered)\n", v);
                fflush(stderr); } } }
        spu_channel_write(&ctx->ch_out_intr_mbox, v);
        break;
    case SPU_WrDec:
        /* Decrementer write (CBEA v1.02 Section 9.7 p145-146; F9/F21). Re-anchors
         * the count: the written value becomes the new count and the guest
         * timebase at this instant becomes the new zero point, mirroring RPCS3
         * SPUThread.cpp:6309-6310 (`ch_dec_start_timestamp = get_timebased_time();
         * ch_dec_value = value;`). Also re-arms counting if a prior WrEventAck
         * had frozen it (CBEA Section 9.11.2 p158's ack-while-Tm-disabled stop
         * rule, see SPU_WrEventAck below) -- "the decrementer... starts... when a
         * wrch instruction targets SPU_WrDec" (p145).
         *
         * NOT implemented here: the decrementer EVENT (status bit 0x20, Tm, on an
         * MSb 0->1 transition -- CBEA Section 9.12.5 p166), including the
         * immediate-signal case where the written value's own MSb is already 1
         * while the previous count's MSb was 0. Zero measured `wrch SPU_WrDec`
         * sites exist in any lifted image today (F21) and only 1 WrEventMask + 1
         * WrEventAck site total, so no guest code currently arms/consumes Tm --
         * this is a documented remaining gap (specaudit_spu.md F9), not silently
         * dropped: implementing the edge-detector would require persisting the
         * pre-write MSb across writes/reads with no live case to validate it
         * against, so it is left for when a real Tm consumer is measured. */
        ctx->dec_value    = v;
        ctx->dec_start_tb = ppu_timebase_now();
        ctx->dec_running  = 1;
        break;
    case SPU_WrEventMask:    ctx->event_mask = v;                           break;
    case SPU_WrEventAck:
        /* Lost-wakeup fix: serialize this read-modify-write against the PPU
         * coherence write (spu_coh_notify_write, |= SPU_EVENT_LR) and the GETLLAR
         * LR-raise (spu_dma.h), both of which set event_status under
         * spu_lockline_lock. A bare `&= ~v` here could read event_status, then have
         * a concurrent `|= 0x400` land, then write back the stale value -> the LR
         * edge is dropped and the idle SPURS kernel never wakes. */
        spu_lockline_lock();
        ctx->event_status &= ~v;
        spu_lockline_unlock();
        /* Decrementer stop rule (CBEA v1.02 Section 9.11.2 p158 implementation
         * note): acking the Tm event (bit 0x20) while Tm is DISABLED in the
         * event mask stops the decrementer ("must stop... regardless of the
         * status of the SPU decrementer event"; must NOT stop if Tm is enabled
         * at ack time). F9's fix sketch. Freeze is inert today (no measured
         * WrEventAck site ever acks 0x20 -- only 1 WrEventMask + 1 WrEventAck
         * site total per F9/F21's live-exposure sweep) but is implemented
         * faithfully since it composes for free with the SPU_WrDec re-arm above. */
        if ((v & 0x20u) && !(ctx->event_mask & 0x20u))
            ctx->dec_running = 0;
        break;
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
    case SPU_RdDec:
        /* Decrementer read (CBEA v1.02 Section 9.7 p146; F9/F21). Nonblocking;
         * "successive reads can return the same value" (p146) -- true here too
         * when called back-to-back within the same guest timebase tick.
         * Overflow-safe: elapsed ticks can exceed dec_value (the count wraps
         * past 0, which is architecturally correct -- it is a free-running
         * counter), so the subtraction is done in 32-bit wrapping arithmetic
         * exactly like a real 32-bit decrementer register. When dec_running is
         * 0 (frozen by WrEventAck while Tm was disabled, CBEA Section 9.11.2
         * p158) the count does not advance -- mirrors RPCS3's
         * `is_dec_frozen ? 0 : (get_timebased_time() - ch_dec_start_timestamp)`
         * (SPUThread.cpp:5183). */
        {
            uint64_t elapsed = ctx->dec_running
                              ? (ppu_timebase_now() - ctx->dec_start_tb)
                              : 0;
            v = ctx->dec_value - (uint32_t)elapsed;
        }
        break;
    case SPU_RdEventMask:   v = ctx->event_mask;                        break;
    case SPU_RdEventStat:
        /* NOTE: HW stalls here until an enabled event is pending; a hard block
         * hangs (the reserved-line vs PPU-write-line alignment isn't yet proven
         * -- the service parks but the LR never wakes it). Kept non-blocking; the
         * LR bit raised by spu_coh_notify_write is still visible to this read so
         * the polling service can pick it up. Revisit blocking once the idle
         * reservation line is confirmed to match what the PPU writes. */
        /* F10 (CBEA v1.02 Section 9.11.1 p153): "Reading the SPU Read Event
         * Status Channel... returns the value of the SPU Pending Event
         * Register logically ANDed with the value in the SPU Write Event Mask
         * Channel". The lifter's bisled emission already tests
         * `(event_status & event_mask)` (spu_lifter.py) -- this makes the
         * runtime channel-read path consistent with that, so code that masks
         * an event to defer it does not still observe it here. */
        v = (*(volatile uint32_t*)&ctx->event_status) & ctx->event_mask;
        /* DIAG (YZ_SPU_PROF): does the service reach the event-wait path in
         * STEADY STATE? If RdEventStat fires after 20M GETLLARs, the LR-event
         * (Loop A) is live and worth implementing; if never, the spin is the
         * poll loop (Loop B) and LR is a red herring. */
        if (g_spu_prof_on) {
            extern unsigned long g_spu_getllar_n, g_spu_evstat_rd;
            if (g_spu_getllar_n > 20000000UL) {
                if (g_spu_evstat_rd < 20)
                    fprintf(stderr, "[spu-ev] spu=%X RdEventStat steady: status=0x%X mask=0x%X lr_raised=%lu\n",
                            ctx->spu_id, ctx->event_status, ctx->event_mask, g_spu_lr_raise);
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
    case SPU_RdEventStat:
        /* F10 (CBEA v1.02 Section 9.11.1 p153): "If no enabled events have
         * occurred, a rchcnt... returns zeros" -- i.e. rchcnt(0) must be
         * (pending & mask) ? 1 : 0, matching the same masking SPU_RdEventStat's
         * read now applies (see spu_rdch above) and what the lifter's bisled
         * emission already assumes. Zero measured rchcnt(ch0) sites in the
         * current lifted images (specaudit_spu.md F10/F22's live sweep), so
         * this closes a real spec gap without a currently-exercised regression
         * risk. */
        return ((*(volatile uint32_t*)&ctx->event_status) & ctx->event_mask) ? 1u : 0u;
    case SPU_RdInMbox:       return ctx->ch_in_mbox.count;                 /* readable */
    case SPU_WrOutMbox:      return SPU_MBOX_DEPTH - ctx->ch_out_mbox.count; /* free slots */
    case SPU_WrOutIntrMbox:  return SPU_INTR_MBOX_DEPTH - ctx->ch_out_intr_mbox.count;
    case SPU_RdSigNotify1:   return ctx->ch_sig_notify[0].count;
    case SPU_RdSigNotify2:   return ctx->ch_sig_notify[1].count;
    case MFC_Cmd:            return MFC_QUEUE_DEPTH - mfc_for(ctx)->queue_count;
    case MFC_RdTagStat:      return 1;  /* synchronous: status always ready */
    case MFC_RdListStallStat:
        /* F11/P6: nonzero (readable) iff at least one tag is currently
         * stalled, mirroring RPCS3 SPUThread.cpp:5298
         * (`case MFC_RdListStallStat: return ch_stall_stat.get_count();`,
         * where get_count() on the underlying channel is the popcount of
         * stalled tags -- collapsed here to the architected 0/1 rchcnt
         * result, matching every other channel's rchcnt encoding in this
         * function). An un-stalled list therefore reads the same 0 it
         * always would have (this channel was never in the old default-1
         * path -- MFC_RdListStallStat had no rchcnt case before this change
         * and fell through to the generic `default: return 1`, which was
         * spec-wrong for the common "nothing stalled" case). */
        return mfc_for(ctx)->stall_mask ? 1u : 0u;
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

/* All 10 EBOOT task images + kernel/service/policy total ~150k lifted
 * functions (cri_audio alone is 35k); the old 64k cap silently dropped
 * whoever registered last. ~3 MB at 262144 -- cheap. */
#define SPU_FN_REGISTRY_MAX 262144
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
int g_yz_slotstore = 0;          /* YZ_SLOTSTORE: name the wid4 work-record writers (s26 #57B) */

/* Compare-on-store logger for the wid4 work-record slots (called from the
 * vm_write32/vm_write64 hot-path branch in ppu_memory.h when YZ_SLOTSTORE=1).
 * ra = the call site inside the lifted chunk (resolve vs yakuza_recomp.map,
 * or via yz_guest_addr_from_host when available). Uncapped: staging writes
 * are rare (~2/round). */
void yz_slotstore_log(uint32_t addr, unsigned long long val, int width, void* ra)
{
    extern uint32_t yz_guest_addr_from_host(const void* rip);   /* main.cpp helper */
    extern uint32_t yz_thread_current_id(void);
    uint32_t g = yz_guest_addr_from_host(ra);
    fprintf(stderr, "[slotstore] tid=%u ea=0x%08X val=0x%0*llX w%d guest=0x%08X host_ra=%p\n",
            yz_thread_current_id(), addr, width / 4, val, width, g, ra);
    fflush(stderr);
}
unsigned long g_yz_dlist_w = 0;  /* cap for the [dlist-w] log */
uint32_t g_yz_taskset_ea = 0;    /* EA of the gs_task CellSpursTaskset (bitset line); captured from the task_info DMA */

/* fn-pointer hash slots. MUST exceed the total number of lifted SPU functions
 * across ALL images, else spu_prof_insert()'s open-addressing probe never finds
 * an empty slot and spins forever. With --seed-all on the policy + task modules
 * the total reaches ~45k (gs_task ~9k, cri_audio ~32k), so size generously. */
#define SPU_PROF_HSZ   (1u << 17)      /* 131072 slots */
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
    /* Guard against a full table: bounded probe so a future overflow degrades
     * spu_prof_addr_of() (a diagnostic) instead of hanging registration. */
    for (unsigned probes = 0; probes < SPU_PROF_HSZ; probes++) {
        if (!s_p_fn[h]) { s_p_fn[h] = (void*)fn; s_p_addr[h] = addr; return; }
        if (s_p_fn[h] == (void*)fn) { s_p_addr[h] = addr; return; }
        h = (h + 1) & (SPU_PROF_HSZ - 1);
    }
}

static uint32_t spu_prof_addr_of(void* fn)
{
    unsigned h = p_hash(fn);
    for (unsigned probes = 0; probes < SPU_PROF_HSZ && s_p_fn[h]; probes++) {
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

/* Generalized SPU taskset task launch. Invoked when the taskset POLICY module
 * (image 2) is about to run StartTask (LS 0x1CC0): the dispatch has DMA'd the
 * selected task's TaskInfo to LS 0x2780 (args@0x2780, elf@0x2790) and set
 * savedContextLr@0x2C80 = the task entry. Establish the launch ABI (RPCS3
 * spursTasksetStartTask, cellSpursSpu.cpp:1395) and switch to the task's lifted
 * IMAGE so the policy's `bi gpr2` transfers into the lifted task code.
 *
 * Discriminates the task by its TaskInfo ELF EA (the generic key):
 *   0x0127A580 -> gs_task   (Edge geometry, image 0)
 *   0x012B4980 -> cri_audio (SOFDEC/ADX codec, image 3)
 * Replaces the gs_task-only hardcode (which also required YZ_SPU_PROF). gpr3 is
 * read from the DMA'd TaskInfo (not hardcoded) so any task's args are correct.
 * Off-switch: YZ_NOLAUNCH. */
void spu_task_launch(spu_context* c)
{
    /* NOTE: tasks LAUNCH fine without this (the bi $r0 lifter fix), but disabling it
     * REGRESSES movie frame progress (fence 28->14 measured) -- it still helps the
     * ongoing dispatch/execution that produces frames, so it stays default-ON.
     * Off-switch: YZ_NOLAUNCH. */
    static int nolaunch = -1;
    if (nolaunch < 0) nolaunch = getenv("YZ_NOLAUNCH") ? 1 : 0;
    if (nolaunch) return;
    const unsigned char* ls = c->ls;
    uint32_t scl = ((uint32_t)ls[0x2C80]<<24)|((uint32_t)ls[0x2C81]<<16)
                 |((uint32_t)ls[0x2C82]<<8)|ls[0x2C83];      /* savedContextLr = task entry */
    uint32_t elf = ((uint32_t)ls[0x2794]<<24)|((uint32_t)ls[0x2795]<<16)
                 |((uint32_t)ls[0x2796]<<8)|ls[0x2797];      /* TaskInfo.elf EA (low 32) */
    elf &= 0xFFFFFFF8u;                                       /* dispatch masks 3 flag bits */
    const spu_image_desc* imd = spu_image_for_elf(elf);
    if (!imd) return;                         /* unknown task -- don't launch */
    int image = imd->image_id;

    /* START vs RESUME (RPCS3 spursTasksetDispatch, cellSpursSpu.cpp:1740-1864):
     * savedContextLr == the ELF entry => a FRESH START (seed the task ABI). Any
     * other (mid-function) savedContextLr => the task YIELDED (its 0xA70
     * context-save wrote gpr0->LS 0x2C80, gpr1->0x2C90, gpr80+i->0x2CA0+i*0x10)
     * and is being RESUMED -> RESTORE that saved non-volatile context instead of
     * memset-ing it. Memset on resume wipes gpr80-127 (notably gpr81, the codec's
     * work-object base, set only at the ELF entry 0x3084) -> null vtable bisl at
     * LS 0x3EB4 -> branch to LS 0 -> halt. The context-save area (0x2C80-0x3000)
     * sits between the policy (<=0x2840) and the task (>=0x3000), so it persists
     * in LS across the yield -- restore straight from LS. */
    /* (2026-07-02: entry from the generated image table -- the old ternary
     * assumed 0x3070 for every non-gs_task image, misclassifying a fresh wkl4
     * launch, entry 0x3050, as a resume on this A/B path.) */
    uint32_t elf_entry = imd->entry;
    int resume = (scl != elf_entry);
    memset(c->gpr, 0, 128 * sizeof(c->gpr[0]));
    if (resume) {
        /* FAITHFUL CONTEXT RESTORE FROM MAIN MEMORY (RPCS3 cellSpursSpu.cpp
         * spursTasksetDispatch, resume path :1832): the yield SAVED the task
         * context to the taskInfo's context-save EA (be64 @LS 0x2798, low 7
         * bits = allocLsBlocks): the register block [LS 0x2C80,0x3000) at
         * EA+0, the ls_pattern-selected 2 KB LS blocks at EA+0x400 (pattern
         * @LS 0x27A0, MSB-first bit i = block i, task area starts block 6 =
         * LS 0x3000). Restoring from LOCAL LS only worked when the task
         * resumed on the SAME SPU it yielded on; SPURS migrates tasks freely
         * (MEASURED 2026-07-02: cri_audio yielded on one SPU, startTask
         * resumed it on another -> local restore read garbage -> gpr81=0 ->
         * the bisl @LS 0x3EB4 null-vtable branch to LS 0 -> kernel group
         * death -> Sony's spurs_handler_entry assert; not on real HW).
         * Idempotent if Sony's lifted policy already restored: same bytes. */
        {
            const unsigned char* ti = c->ls + 0x2798u;
            uint64_t css = ((uint64_t)ti[0]<<56)|((uint64_t)ti[1]<<48)
                         |((uint64_t)ti[2]<<40)|((uint64_t)ti[3]<<32)
                         |((uint64_t)ti[4]<<24)|((uint64_t)ti[5]<<16)
                         |((uint64_t)ti[6]<<8)|ti[7];
            uint32_t ea = (uint32_t)(css & ~0x7Full);
            if (ea) {
                extern uint8_t* vm_base;
                memcpy(c->ls + 0x2C80u, vm_base + ea, 0x380u);
                const unsigned char* lp = c->ls + 0x27A0u;
                for (int i = 6; i < 128; i++)
                    if (lp[i >> 3] & (0x80u >> (i & 7)))
                        memcpy(c->ls + 0x3000u + (uint32_t)(i - 6) * 0x800u,
                               vm_base + ea + 0x400u + (uint32_t)(i - 6) * 0x800u,
                               0x800u);
            }
        }
        c->gpr[0] = spu_ls_read128(c, 0x2C80u);              /* LR / savedContextLr */
        c->gpr[1] = spu_ls_read128(c, 0x2C90u);              /* SP */
        for (int i = 0; i < 48; i++)                          /* gpr80..127 (non-volatile) */
            c->gpr[80 + i] = spu_ls_read128(c, 0x2CA0u + (uint32_t)i * 0x10u);
        /* gpr3 left 0 on resume (RPCS3 clears it) */
    } else {
        c->gpr[3] = spu_ls_read128(c, 0x2780u);              /* gpr3 = taskInfo->args */
        c->gpr[4] = spu_rotqbyi(spu_ls_read128(c, 0x2760u), 8); /* gpr4 = {spurs,args} */
        c->gpr[1]._u32[0] = 0x2C30u;                          /* sp = taskset ctx stack */
        /* FRESH START: zero the task image's BSS ([filesz,memsz) spans), per
         * the ELF contract Sony's LoadElf honors -- our image deploy copies
         * only filesz, leaving whatever a prior image/dispatch wrote in the
         * BSS span. Spans come from the generated image table (auto-derived
         * from the ELF headers; reproduces the old hand-derived bounds
         * byte-exactly). RESUME must NOT zero (LS persists across yields). */
        spu_image_zero_bss(c->ls, imd);
    }
    /* YZ_CTXWATCH: register + log this cycle (see yz_ctxw_cycle — the
     * discriminator for the s26 ⚡1 stale-republish signature). */
    yz_ctxw_cycle(c, image, scl, resume);
    c->gpr[2]._u32[0] = scl;                  /* bi gpr2 -> task entry / resume PC */
    c->image_id = image;
    { static int rn = 0; if (resume && rn < 6) { rn++;
        fprintf(stderr, "[task-resume] %s scl=0x%04X restored gpr81=%08X gpr80=%08X\n",
                image==3?"cri_audio":"gs_task", scl,
                c->gpr[81]._u32[0], c->gpr[80]._u32[0]); fflush(stderr); } }
    if (!getenv("YZ_NO_MGMT")) {              /* SpursTasksetContext.tasksetMgmtAddr@0x2FB8=0x2700 */
        c->ls[0x2FB8]=0x00; c->ls[0x2FB9]=0x00; c->ls[0x2FBA]=0x27; c->ls[0x2FBB]=0x00;
    }
    { static int n[8] = {0};
      if (image < 8 && n[image] < 6) { n[image]++;
        uint32_t tsPtr = ((uint32_t)ls[0x27BC]<<24)|((uint32_t)ls[0x27BD]<<16)
                       |((uint32_t)ls[0x27BE]<<8)|ls[0x27BF];          /* taskset ptr (low32) @0x27B8 be64 */
        uint32_t tId   = ((uint32_t)ls[0x27D4]<<24)|((uint32_t)ls[0x27D5]<<16)
                       |((uint32_t)ls[0x27D6]<<8)|ls[0x27D7];          /* SpursTasksetContext.taskId */
        fprintf(stderr, "[task-launch] %s entry=0x%04X elf=0x%08X image=%d args=%08X_%08X_%08X_%08X "
                "| taskset=0x%08X taskId=%u\n",
                image==3?"cri_audio":"gs_task", scl, elf, image,
                c->gpr[3]._u32[0], c->gpr[3]._u32[1], c->gpr[3]._u32[2], c->gpr[3]._u32[3], tsPtr, tId);
        fflush(stderr); } }
}

/* DEFAULT-boot launch interception (replaces the prof-gated path). Called from
 * SPU_DRAIN on each trampoline hop when profiling is OFF; cheap early-out unless
 * this SPU is running the taskset policy (image 2) about to enter StartTask. */
void spu_task_launch_check(spu_context* ctx, void* fn)
{
    if (ctx->image_id != 2) return;
    /* FIX TEST (env YZ_FIXEXIT): the policy poll (0x2318: bisl LS[0x1E0]) yields via the
     * kernel-context exitToKernelAddr@LS 0x1E0, which MUST be 0x838 (CELL_SPURS_KERNEL2_
     * EXIT_ADDR) + selectWorkloadAddr@0x1E4 = 0x290. Reliable trace shows ours is 0xA00 (the
     * policy ENTRY) so the poll re-enters the policy instead of yielding -> infinite poll,
     * never reaching the kernel. Force the correct addrs to test if THAT is the blocker. */
    { static int fe = -1; if (fe < 0) fe = getenv("YZ_FIXEXIT") ? 1 : 0;
      if (fe) { unsigned char* L = ctx->ls;
          uint32_t e = ((uint32_t)L[0x1E0]<<24)|((uint32_t)L[0x1E1]<<16)|((uint32_t)L[0x1E2]<<8)|L[0x1E3];
          if (e != 0x838u) { L[0x1E0]=0;L[0x1E1]=0;L[0x1E2]=0x08;L[0x1E3]=0x38;
              static int n=0; if(n<8){n++; fprintf(stderr,"[fixexit] LS0x1E0 0x%08X -> 0x838\n", e); fflush(stderr);} }
          uint32_t s = ((uint32_t)L[0x1E4]<<24)|((uint32_t)L[0x1E5]<<16)|((uint32_t)L[0x1E6]<<8)|L[0x1E7];
          if (s != 0x290u) { L[0x1E4]=0;L[0x1E5]=0;L[0x1E6]=0x02;L[0x1E7]=0x90; } } }
    uint32_t a = spu_prof_addr_of(fn);
    /* THROWAWAY DIAG (env YZ_POLHOP): full control-flow path of the taskset POLICY
     * (image 2). Locks onto the first image-2 SPU and logs its NON-SEQUENTIAL hops
     * (real branches, a != prev+4) from entry 0xA00 onward -- shows exactly where the
     * policy halts/loops before reaching the poll(0x2308)/exitToKernel(0x838)/SELECT_TASK
     * /StartTask(0x1CC0) chain. Default boot unaffected (env-gated). */
    { static int ph = -1; if (ph < 0) ph = getenv("YZ_POLHOP") ? 1 : 0;
      if (ph) { static int lock = -1; if (lock < 0) lock = (int)ctx->spu_id;
          if ((int)ctx->spu_id == lock) {
              /* Log EVERY image-2 hop for this SPU (cap 2500): the exact sequential
               * path so we can see precisely where the policy halts/loops/exits.
               * Mark key SPURS addrs inline. */
              static int n = 0;
              if (n < 2500) { n++;
                  const char* tag = a==0xA00u?" <ENTRY>":a==0x2308u?" <POLL>":a==0x1CC0u?" <STARTTASK>"
                                  :a==0x290u?" <SELWKL>":a==0x838u?" <exit?>":a==0x231Cu?" <RESUME>":"";
                  fprintf(stderr, "[polhop] 0x%04X%s\n", a, tag); fflush(stderr); }
              /* At the entry flag-check (0x1AD4 ceqi / 0x1AD8 biz), dump the LS copy
               * of the SPURS header it reads (LS 0x170) vs the live main-mem header
               * (0x40197CF0) -- shows the flag value + whether the LS copy is stale. */
              if (a == 0x1AD4u) { static int d = 0; if (d < 20) { d++;
                  extern uint8_t* vm_base; const uint8_t* L = ctx->ls;
                  fprintf(stderr, "[polflag] LS0x170:");
                  for (int i=0;i<16;i++) fprintf(stderr," %02X", L[0x170+i]);
                  fprintf(stderr, " | MMhdr0x70:");
                  for (int i=0;i<16;i++) fprintf(stderr," %02X", vm_base[0x40197C80+0x70+i]);
                  fprintf(stderr, " gpr75=%08X\n", ctx->gpr[75]._u32[0]); fflush(stderr); } }
          } } }
    /* pt35e (env YZ_TRACE_CODEC): arm the dispatch trace EARLY -- the pre-LoadElf crash
     * happens before the ELF-load arming. Detect a policy SPU running the CODEC taskset by
     * its EA in the SpursTasksetContext (LS 0x27BC == 0x63D22580), so we trace the StartTask
     * path that crashes before LoadElf. Default boot unaffected (env-gated). */
    if (g_yz_codec_dispatch_spu < 0) {
        static int tc = -1; if (tc < 0) tc = getenv("YZ_TRACE_CODEC") ? 1 : 0;
        if (tc) { const unsigned char* l = ctx->ls;
            uint32_t tsEA = ((uint32_t)l[0x27BC]<<24)|((uint32_t)l[0x27BD]<<16)
                          |((uint32_t)l[0x27BE]<<8)|l[0x27BF];
            if (tsEA == 0x63D22580u) { g_yz_codec_dispatch_spu = (int)ctx->spu_id;
                fprintf(stderr, "[codec-arm] spu=%X armed at LS 0x%04X (codec taskset present)\n",
                        ctx->spu_id, a); fflush(stderr); } }
    }
    /* pt35 (env YZ_DISP_TRACE): trace the codec policy SPU's dispatch-tail hops
     * after LoadElf, to see where it diverges from StartTask (0x1CC0). Armed at the
     * codec ELF load (spu_dma.h sets g_yz_codec_dispatch_spu). */
    if ((int)ctx->spu_id == g_yz_codec_dispatch_spu) {
        static int dt = -1; if (dt < 0) dt = getenv("YZ_DISP_TRACE") ? 1 : 0;
        /* Log only NON-SEQUENTIAL hops (real branches, a != prev+4) to cut the
         * per-instruction fallthrough noise + avoid the fprintf-per-hop Heisenbug
         * that slows the memset/DMA loops. Shows the dispatch's control-flow path. */
        if (dt) { static uint32_t prev = 0; static int n = 0;
            if (a != prev + 4u && n < 4000) { n++;
                fprintf(stderr, "[disp] spu=%X LS 0x%04X\n", ctx->spu_id, a); fflush(stderr); }
            /* pt35b: pin the instruction that produces the garbage clear count.
             * gpr8 (per-hop = value AFTER the function at `prev` ran). When it first
             * becomes 0x28730 (the file-cursor count) or 0x3C130 (dest), the function
             * at `prev` is the assigning instruction -> audit that op vs the SPU ISA. */
            { static uint32_t pg8 = 0, pg16 = 0; static int w8 = 0, w16 = 0;
              uint32_t g8 = ctx->gpr[8]._u32[0], g16 = ctx->gpr[16]._u32[0];
              static int pathlog = 0;
              if (g8 == 0x28730u && pg8 != 0x28730u && w8 < 4) { w8++; pathlog = 1;
                  fprintf(stderr, "[watch8] gpr8 0x%08X->0x28730 set at LS 0x%04X (now hop->0x%04X) | gpr4=%08X gpr6=%08X gpr7=%08X\n",
                          pg8, prev, a, ctx->gpr[4]._u32[0], ctx->gpr[6]._u32[0], ctx->gpr[7]._u32[0]); fflush(stderr); }
              /* pt35b: once gpr8=0x28730, log EVERY hop until the clear loop (0x2238)
               * is entered -- the exact path by which the clear reuses the stale gpr8. */
              if (pathlog) { static int pl = 0;
                  if (pl < 120) { pl++;
                      fprintf(stderr, "[path] 0x%04X gpr8=%08X\n", a, g8); fflush(stderr); }
                  if (a == 0x2238u) pathlog = 0; }
              if (g16 == 0x3C130u && pg16 != 0x3C130u && w16 < 4) { w16++;
                  fprintf(stderr, "[watch16] gpr16 0x%08X->0x3C130 set at LS 0x%04X (now hop->0x%04X)\n",
                          pg16, prev, a); fflush(stderr); }
              pg8 = g8; pg16 = g16; }
            prev = a;
            /* pt35: dump the clear-loop count registers at setup (0x2204) -- gpr[8]
             * (count) should be 128-aligned & reach 0; it iterates 3855x (>1952), so
             * the count is wrong. gpr[8]=gpr[4]-128+gpr[6]; show gpr[4]/gpr[6] source. */
            if (a == 0x2204u) { static int rd = 0; if (rd < 4) { rd++;
                fprintf(stderr, "[clearloop] spu=%X @0x2204 gpr4=%08X_%08X gpr6=%08X_%08X gpr80=%08X gpr82=%08X\n",
                        ctx->spu_id, ctx->gpr[4]._u32[0], ctx->gpr[4]._u32[3],
                        ctx->gpr[6]._u32[0], ctx->gpr[6]._u32[3],
                        ctx->gpr[80]._u32[3], ctx->gpr[82]._u32[3]); fflush(stderr); } }
            /* observe the loop counter gpr8 + the write ptr gpr16 at the check */
            if (a == 0x2264u) { static int cn = 0; if (cn < 4) { cn++;
                fprintf(stderr, "[clearck] gpr8=%08X_%08X gpr16=%08X_%08X\n",
                        ctx->gpr[8]._u32[0], ctx->gpr[8]._u32[3],
                        ctx->gpr[16]._u32[0], ctx->gpr[16]._u32[3]); fflush(stderr); } }
            /* the Duff's-device computed jump (pc=gpr5._u32[0]=0x223C+(gpr6>>2)).
             * [-1] in the trace => target outside the lifted range. Dump inputs to
             * find which (gpr6 andi / gpr7 rotmi) is mis-lifted. */
            if (a == 0x2234u) { static int jn = 0; if (jn < 8) { jn++;
                fprintf(stderr, "[duff] tgt(gpr5)=%08X gpr6=%08X gpr7=%08X gpr8=%08X_%08X gpr16=%08X\n",
                        ctx->gpr[5]._u32[0], ctx->gpr[6]._u32[0], ctx->gpr[7]._u32[0],
                        ctx->gpr[8]._u32[0], ctx->gpr[8]._u32[3], ctx->gpr[16]._u32[0]); fflush(stderr); } }
            /* segment-loop index (gpr90) at the loop increment (0x2268): if it reaches
             * 2, the loop is processing PH[2]=PT_NOTE (should be skipped) -> garbage clear. */
            if (a == 0x2268u) { static int sn = 0; if (sn < 8) { sn++;
                fprintf(stderr, "[segidx] gpr90=%u (segments processed)\n", ctx->gpr[90]._u32[0]);
                fflush(stderr); } }
            /* p_type extraction check (0x2190 ceqi gpr12,1): dump the p_type our code
             * read per segment. PH0/PH1=1 (PT_LOAD), PH2 should be 4 (PT_NOTE). If PH2
             * reads !=4 (esp. 1), the rotqby/ls_read p_type extraction is mislifted. */
            if (a == 0x2190u) { static int tn = 0; if (tn < 8) { tn++;
                fprintf(stderr, "[ptype] gpr12(p_type)=%08X gpr80(p_vaddr)=%08X gpr8(p_off)=%08X gpr87(p_memsz)=%08X\n",
                        ctx->gpr[12]._u32[0], ctx->gpr[80]._u32[0], ctx->gpr[8]._u32[0], ctx->gpr[87]._u32[0]);
                fflush(stderr); } }
            /* 0x22B4 restores gpr0 from stack (gpr1+0x120) then the epilogue jumps to it.
             * The trace shows it landing at 0x2218 (the garbage clear). Is gpr0 a corrupted
             * return addr (=0x2218, stack smashed) or a sane dispatch addr (reached 0x2218
             * another way)? Dump gpr0 AFTER 0x22B4 runs -- so capture at 0x22B8. */
            if (a == 0x22B8u) { static int en = 0; if (en < 6) { en++;
                fprintf(stderr, "[ret] @0x22B8 gpr0(retaddr)=%08X_%08X gpr1(sp)=%08X gpr91=%08X gpr89(highLS)=%08X\n",
                        ctx->gpr[0]._u32[0], ctx->gpr[0]._u32[3], ctx->gpr[1]._u32[0],
                        ctx->gpr[91]._u32[0], ctx->gpr[89]._u32[0]); fflush(stderr); } }
            /* @0x1B6C (just after the no-op'd `hlgti r3,0` at 0x1B68 = LoadElf's
             * "halt if return!=CELL_OK"): dump gpr3 (LoadElf return code). If !=0,
             * LoadElf ERRORED and the no-op'd assert is masking it -> the dispatch
             * runs into garbage. THE key check. */
            if (a == 0x1B6Cu) { static int ln = 0; if (ln < 6) { ln++;
                fprintf(stderr, "[loadelf-ret] gpr3(retcode)=%08X_%08X (0=CELL_OK; !=0 => LoadElf FAILED, assert masked)\n",
                        ctx->gpr[3]._u32[0], ctx->gpr[3]._u32[3]); fflush(stderr); } } }
    }
    if (a != 0x1CC0u) return;
    /* DIAG (pt35): every time the policy reaches StartTask (0x1CC0), log the
     * TaskInfo ELF + wklCurrentId, so we can see whether dispatch reaches the
     * launch point for the codec (elf 0x012B4980) or only ever for gs_task. If
     * this never fires with the codec elf, the gate is UPSTREAM of StartTask
     * (selection / policy dispatch), NOT the launch -- and NOT contention alone. */
    { static int st = 0;
      if (st < 40) { st++;
        const unsigned char* l = ctx->ls;
        uint32_t e = (((uint32_t)l[0x2794]<<24)|((uint32_t)l[0x2795]<<16)
                    |((uint32_t)l[0x2796]<<8)|l[0x2797]) & 0xFFFFFFF8u;
        uint32_t wcl = ((uint32_t)l[0x1DC]<<24)|((uint32_t)l[0x1DD]<<16)
                     |((uint32_t)l[0x1DE]<<8)|l[0x1DF];
        /* SpursTasksetContext @ LS 0x2700: taskset ptr (low32)@0x27BC, taskId@0x27D4.
         * Shows whether the policy computed the right taskId (0 for the codec) and
         * whether the task_info DMA brought the codec elf to LS 0x2790. */
        uint32_t tsp = ((uint32_t)l[0x27BC]<<24)|((uint32_t)l[0x27BD]<<16)|((uint32_t)l[0x27BE]<<8)|l[0x27BF];
        uint32_t tid = ((uint32_t)l[0x27D4]<<24)|((uint32_t)l[0x27D5]<<16)|((uint32_t)l[0x27D6]<<8)|l[0x27D7];
        uint32_t a0 = ((uint32_t)l[0x2780]<<24)|((uint32_t)l[0x2781]<<16)|((uint32_t)l[0x2782]<<8)|l[0x2783];
        fprintf(stderr, "[startTask] spu=%X elf=0x%08X wklCurId=0x%X taskset=0x%08X taskId=%u argsHi=0x%08X\n",
                ctx->spu_id, e, wcl, tsp, tid, a0);
        fflush(stderr); } }
    /* DEFAULT boot path: launch ONLY the cri_audio codec (elf 0x012B4980).
     * gs_task launching here wedges an SPU and diverges the boot away from the
     * movie (measured: .cvm stream stops at 0xB800 vs 0x1D800), so gs_task stays
     * on the YZ_SPU_PROF path. Discriminate by the TaskInfo ELF EA at LS 0x2790. */
    const unsigned char* ls = ctx->ls;
    uint32_t elf = (((uint32_t)ls[0x2794]<<24)|((uint32_t)ls[0x2795]<<16)
                  |((uint32_t)ls[0x2796]<<8)|ls[0x2797]) & 0xFFFFFFF8u;
    /* RETIRED DEFAULT-OFF (2026-07-02, opt back in with YZ_STARTTASK_HOOK=1):
     * LS 0x1CC0 is NOT StartTask -- the policy ELF disasm shows it is the
     * taskset-SYSCALL switch (`bi $r2` with the case jump-table at 0x1CC4:
     * 0=EXIT 0x1CD8, 1=YIELD 0x1D34, 2=WAIT_SIGNAL 0x1D80...; RPCS3
     * spursTasksetProcessSyscall matches case-for-case). This hook therefore
     * hijacked EVERY taskset syscall of the matched elfs into an immediate
     * re-launch: a WAIT_SIGNAL became a bogus instant "resume" (measured: the
     * codec's wait at scl=0x2095C relaunched with a zeroed/garbage context ->
     * the bisl @0x3EB4 null-vtable crash), and Sony's save-task-context
     * (case handlers) never ran -- the context-save EA stayed all-zero while
     * gs_task's NATURAL-path yields saved fine (PUT 0x380 -> ctxsave EA).
     * With the hook off, the real case handlers + dispatch run lifted, and
     * task entries/resumes are caught by the natural-launch interception in
     * spu_indirect_branch (bi savedContextLr, pc>=0x3000 under image 2). */
    { static int sth = -1; if (sth < 0) sth = getenv("YZ_STARTTASK_HOOK") ? 1 : 0;
      if (sth && (elf == 0x012B4980u || elf == 0x0127A580u)) spu_task_launch(ctx); }
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
    /* RETIRED DEFAULT-OFF (2026-07-02): LS 0x1CC0 is the taskset-SYSCALL switch,
     * not StartTask -- see spu_task_launch_check. YZ_STARTTASK_HOOK=1 re-enables
     * the legacy hijack for A/B. */
    { static int sth = -1; if (sth < 0) sth = getenv("YZ_STARTTASK_HOOK") ? 1 : 0;
      if (sth && a == 0x1CC0u && g_spu_cur_ctx && g_spu_cur_ctx->image_id == 2)
        spu_task_launch(g_spu_cur_ctx); }
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
    } else {
        /* NEVER silent: an overflow drops whole images (whoever registers
         * last) and the boot degrades mysteriously -- exactly what happened
         * when the 2026-07-02 batch lift pushed the total past the old 64k
         * cap and gs_task (registered last) vanished. */
        static int warned = 0;
        if (!warned) { warned = 1;
            fprintf(stderr, "[SPU] FATAL: function registry FULL (%u) -- raise "
                    "SPU_FN_REGISTRY_MAX; later images are DROPPED\n",
                    s_registry_count);
            fflush(stderr); }
    }
    spu_prof_insert(addr, fn);
}

/* Shared exact/wildcard decision, applied identically after either the sorted
 * index binary search or the O(n) fallback scan below have found (a) an exact
 * (addr,image_id) hit -- returned immediately by the caller before this is
 * even reached -- or (b) the best wildcard candidate. Kept as one function so
 * the guard's behavior can't drift between the fast and fallback paths. */
static spu_fn spu_lookup_apply_job_guard(uint32_t addr, int image_id,
                                          spu_fn wildcard, int wildcard_img)
{
    /* Jobchain-family wildcard guard (2026-07-08). A query from images 13-15
     * at a JOB-SPAN address (>= 0x4880, past the module's end; kernel-yield
     * targets are all below 0xA00 and stay wildcard-served by design) that
     * misses its exact image would run ANOTHER image's code at the job site —
     * measured: the round-1 notify job (jobB loaded at LS 0x4C00) dispatched
     * gs_task's spu_func_00004C00 via this fallback and the job never did its
     * real work (the spup17 wall). Fail LOUD (NULL -> unknown-branch halt)
     * instead; the job binaries are now lifted at both observed slot bases so
     * a legitimate dispatch always has an exact match. Kill-switch:
     * YZ_JOB_WILDCARD_OK=1 restores the old silent substitution. */
    if (image_id >= 13 && image_id <= 15 && addr >= 0x4880u && wildcard) {
        static int ok = -1;
        if (ok < 0) ok = getenv("YZ_JOB_WILDCARD_OK") ? 1 : 0;
        static int wl = 0; if (wl < 32) { wl++;
            fprintf(stderr, "[job-wildcard] query img=%d addr=0x%05X exact-miss; wildcard img=%d %s\n",
                    image_id, addr, wildcard_img,
                    ok ? "SERVED (YZ_JOB_WILDCARD_OK)" : "REFUSED (would run wrong image's code)");
            fflush(stderr); }
        if (!ok) return NULL;
    }
    /* Kernel-context wildcard guard (s24). Contexts running the SPURS kernel
     * (image 16, adopted at group start) must resolve ONLY against the
     * kernel's own registrations — a kernel-era wild branch that fell to the
     * image-0 wildcard executed gs_task's code and mis-attributed the
     * tid-0x2004 death class for weeks (ledger #34/#49). Legitimate kernel
     * exits (workload dispatch at 0xA00, task launch, job slots) all SWITCH
     * image_id before entering foreign code, so a kernel-tagged query that
     * misses exact is a bug by definition. Fail loud; kill-switch
     * YZ_KERN_WILDCARD_OK restores the old masking. */
    if (image_id == 16 && wildcard) {
        static int kok = -1;
        if (kok < 0) kok = getenv("YZ_KERN_WILDCARD_OK") ? 1 : 0;
        static int kl = 0; if (kl < 32) { kl++;
            fprintf(stderr, "[kern-wildcard] KERNEL ctx query addr=0x%05X exact-miss; wildcard img=%d %s\n",
                    addr, wildcard_img,
                    kok ? "SERVED (YZ_KERN_WILDCARD_OK)" : "REFUSED (wild kernel branch, failing loud)");
            fflush(stderr); }
        if (!kok) return NULL;
    }
    return wildcard;
}

/* ===========================================================================
 * spu_lookup fast index (SPU-SPEED item 1). The registry holds O(10^5)
 * entries and every SPU computed branch (bi/bisl/brasl-return) queries it --
 * with ~150k+ functions registered, the plain forward scan was the hottest
 * O(n) cost in the whole dispatch loop. Replace it with a copy of the
 * registry sorted by addr (ties broken by original registration order, so
 * the exact/wildcard tie-break below matches the old scan byte-for-byte),
 * binary-searched to the first entry with a matching addr, then scanned
 * across just the (small, bounded by how many images alias that LS address)
 * run of same-addr entries -- O(log n + k) instead of O(n).
 *
 * Registration is append-only and, in this runtime, 100% complete (all
 * spu_begin_image()/spu_recomp_register*() calls in main()) before any SPU
 * host thread starts calling spu_lookup (threads are spawned later, from
 * sys_spu_thread_group_start) -- so the index only ever needs to be built
 * ONCE, with no reader racing the build. It's built lazily on the first
 * lookup after registration (rather than eagerly per spu_register_function
 * call) so the O(n log n) sort cost is paid once, not once per registration
 * in a ~150k-call registration burst.
 *
 * Thread safety: builds are serialized by a spinlock (same atomic_flag
 * pattern as spu_lockline above); the built array is published through an
 * atomic pointer + count so a lookup on another thread either sees the
 * fully-built index or the prior (possibly NULL) one, never a partial write.
 * A rebuilt index is never freed (only ever superseded) -- registration is
 * rare (once at boot in practice) and a stale reader could still be scanning
 * it, so the bounded, one-time leak is the safe trade against a use-after-
 * free. If a build ever fails (OOM), spu_lookup transparently falls back to
 * the original O(n) scan, so correctness never depends on the index. */
typedef struct {
    uint32_t addr;
    uint32_t order;    /* original registration index; preserves the old
                        * linear scan's "first registered wins" tie-break */
    spu_fn   fn;
    int      image_id;
} spu_idx_entry;

static int spu_idx_cmp(const void* pa, const void* pb)
{
    const spu_idx_entry* a = (const spu_idx_entry*)pa;
    const spu_idx_entry* b = (const spu_idx_entry*)pb;
    if (a->addr != b->addr) return (a->addr < b->addr) ? -1 : 1;
    return (a->order < b->order) ? -1 : (a->order > b->order ? 1 : 0);
}

static atomic_flag              s_index_build_lock = ATOMIC_FLAG_INIT;
static _Atomic(spu_idx_entry*)  s_index = 0;         /* published sorted snapshot */
static _Atomic(uint32_t)        s_index_count = 0;   /* entries in that snapshot */
static uint32_t                 s_index_built_from = 0; /* lock-protected */

static void spu_lookup_rebuild_index(void)
{
    while (atomic_flag_test_and_set_explicit(&s_index_build_lock, memory_order_acquire))
        SPU_CPU_RELAX();
    uint32_t n = s_registry_count;
    if (n != s_index_built_from) {
        spu_idx_entry* idx = (spu_idx_entry*)malloc((n ? (size_t)n : 1) * sizeof(spu_idx_entry));
        if (idx) {
            for (uint32_t i = 0; i < n; i++) {
                idx[i].addr = s_registry[i].addr;
                idx[i].fn = s_registry[i].fn;
                idx[i].image_id = s_registry[i].image_id;
                idx[i].order = i;
            }
            qsort(idx, n, sizeof(spu_idx_entry), spu_idx_cmp);
            /* Publish count BEFORE the pointer isn't safe (a reader could load
             * the new pointer with the old count); publish the pointer LAST
             * with a release store so a reader that observes the new pointer
             * also observes fully-initialized entries and the matching count
             * (read together below, count first, pointer second, matching
             * this store order in reverse -- see spu_lookup). */
            atomic_store_explicit(&s_index_count, n, memory_order_relaxed);
            atomic_store_explicit(&s_index, idx, memory_order_release);
            s_index_built_from = n;
        }
        /* malloc failure: leave whatever was previously published (possibly
         * NULL) in place -- spu_lookup's fallback path covers this. */
    }
    atomic_flag_clear_explicit(&s_index_build_lock, memory_order_release);
}

/* serve_img (may be NULL): which image's registration actually served the
 * lookup — image_id itself on an exact hit, the wildcard owner's id on a
 * substituted serve. Feeds adopt-on-serve in spu_indirect_branch (s24 image
 * model, ledger #51) so the context is tagged with the code it truly runs. */
static spu_fn spu_lookup(uint32_t addr, int image_id, int* serve_img)
{
    if (serve_img) *serve_img = image_id;
    spu_idx_entry* idx = atomic_load_explicit(&s_index, memory_order_acquire);
    uint32_t idx_n = atomic_load_explicit(&s_index_count, memory_order_acquire);
    if (!idx || idx_n != s_registry_count) {
        spu_lookup_rebuild_index();
        idx = atomic_load_explicit(&s_index, memory_order_acquire);
        idx_n = atomic_load_explicit(&s_index_count, memory_order_acquire);
    }
    if (idx && idx_n == s_registry_count) {
        /* Binary search: first index with addr >= target. */
        uint32_t lo = 0, hi = idx_n;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (idx[mid].addr < addr) lo = mid + 1; else hi = mid;
        }
        spu_fn wildcard = NULL;
        int wildcard_img = -1;
        for (uint32_t i = lo; i < idx_n && idx[i].addr == addr; i++) {
            if (idx[i].image_id == image_id) return idx[i].fn; /* exact (incl 0==0) */
            /* s24: kernel entries (16) are universal servers -- every context
             * may branch INTO the resident scheduler (workload exits at 0x290/
             * 0x838). The kern-guard below still refuses img-0 substitution FOR
             * kernel contexts. */
            if ((idx[i].image_id == 0 || idx[i].image_id == 16 || image_id == 0) && !wildcard) {
                wildcard = idx[i].fn;
                wildcard_img = idx[i].image_id;
            }
        }
        if (serve_img && wildcard) *serve_img = wildcard_img;
        return spu_lookup_apply_job_guard(addr, image_id, wildcard, wildcard_img);
    }
    /* Fallback (index unavailable, e.g. OOM on the very first build): the
     * original O(n) scan. Correctness never depends on the index existing. */
    spu_fn wildcard = NULL;
    int wildcard_img = -1;
    for (uint32_t i = 0; i < s_registry_count; i++) {
        if (s_registry[i].addr != addr) continue;
        if (s_registry[i].image_id == image_id) return s_registry[i].fn; /* exact (incl 0==0) */
        if ((s_registry[i].image_id == 0 || s_registry[i].image_id == 16 || image_id == 0) && !wildcard) {
            wildcard = s_registry[i].fn;
            wildcard_img = s_registry[i].image_id;
        }
    }
    if (serve_img && wildcard) *serve_img = wildcard_img;
    return spu_lookup_apply_job_guard(addr, image_id, wildcard, wildcard_img);
}

/* Does a lifted function exist at this LS address (any image)? Lets the
 * lv2 layer decide between real SPU execution and PPU fallbacks. */
int spu_have_function(uint32_t addr)
{
    return spu_lookup(addr, 0, NULL) != NULL;
}

/* ===========================================================================
 * Restore-on-host-return (s24 adopt-on-serve image model, ledger #51).
 *
 * The lifted brsl/bisl emissions bracket every nested host call with
 *     { int32_t _si = ctx->image_id;  <call + SPU_DRAIN + depth-->;
 *       spu_img_restore(ctx, _si); }
 * so any image adopted INSIDE the call (dispatcher serve, foreign-resident
 * adoption, drain tail-chain hops) is scoped to the call and cannot leak
 * into the caller's continuation — the failure mode that let kernel-era
 * contexts wander to image 0 and mis-attribute the tid-0x2004 death for
 * weeks (ledger #34/#49/#51). The ledger-#51 trap (a returning bisl into
 * kernel 0x290 leaving the policy mislabeled 16 for the kern-guard to
 * false-kill) is exactly what this closes: the return restores the caller's
 * image unconditionally. Persistent switches that must survive a return are
 * re-applied at dispatch time (module_img_a00 residency; job_bin_base spans;
 * task-launch keying) — dispatch-time residency, not return-time state, is
 * the ground truth. longjmp unwinds (restack/halt/task-launch replacement)
 * discard the bracket locals with their frames; those paths set image_id
 * explicitly. Kill-switch: YZ_NO_IMGSTACK=1 reverts to the old sticky-image
 * behavior. */
/* ===========================================================================
 * throw_event loss latch (s25, notification-surface audit risk #3).
 *
 * SPU WrOutIntrMbox codes 64-127 (throw_event) are fire-and-forget by
 * protocol: no ack channel exists, so a full destination lv2 queue loses the
 * event with no guest-visible signal. Real HW tolerates this because its
 * consumers keep pace; our host timing widens the full-queue window. Failed
 * throws are latched here and redelivered from the vblank tick once the
 * queue has drained — the throw happened, only delivery timing shifts.
 * Small fixed ring; overflow drops (the faithful behavior) with a loud log
 * at the latch site. Writers = SPU threads, flusher = the vblank thread;
 * serialized by a spinlock. */
#define YZ_THROW_LAT_MAX 16
typedef struct { uint32_t qid; uint64_t src, d1, d2, d3; int used; } yz_throw_lat;
static yz_throw_lat g_throw_lat[YZ_THROW_LAT_MAX];
static atomic_flag  g_throw_lat_lock = ATOMIC_FLAG_INIT;

int yz_throw_latch_add(uint32_t qid, uint64_t src,
                       uint64_t d1, uint64_t d2, uint64_t d3)
{
    int ok = 0;
    while (atomic_flag_test_and_set_explicit(&g_throw_lat_lock, memory_order_acquire))
        SPU_CPU_RELAX();
    for (int i = 0; i < YZ_THROW_LAT_MAX; i++)
        if (!g_throw_lat[i].used) {
            g_throw_lat[i].qid = qid; g_throw_lat[i].src = src;
            g_throw_lat[i].d1 = d1; g_throw_lat[i].d2 = d2; g_throw_lat[i].d3 = d3;
            g_throw_lat[i].used = 1; ok = 1; break;
        }
    atomic_flag_clear_explicit(&g_throw_lat_lock, memory_order_release);
    return ok;
}

/* Called from the vblank tick (~16 ms cadence). Retries every latched throw
 * in order; entries that still fail stay latched. */
void yz_throw_retry_flush(void)
{
    extern int sys_event_queue_push_by_id(uint32_t queue_id, uint64_t source,
                                          uint64_t d1, uint64_t d2, uint64_t d3);
    while (atomic_flag_test_and_set_explicit(&g_throw_lat_lock, memory_order_acquire))
        SPU_CPU_RELAX();
    for (int i = 0; i < YZ_THROW_LAT_MAX; i++) {
        if (!g_throw_lat[i].used) continue;
        int rc = sys_event_queue_push_by_id(g_throw_lat[i].qid, g_throw_lat[i].src,
                                            g_throw_lat[i].d1, g_throw_lat[i].d2,
                                            g_throw_lat[i].d3);
        if (rc == 0) {
            fprintf(stderr, "[throw-lat] REDELIVERED latched throw -> queue %u\n",
                    g_throw_lat[i].qid);
            fflush(stderr);
            g_throw_lat[i].used = 0;
        }
    }
    atomic_flag_clear_explicit(&g_throw_lat_lock, memory_order_release);
}

void spu_img_restore(spu_context* ctx, int32_t saved_img)
{
    static int off = -1;
    if (off < 0) {
        off = getenv("YZ_NO_IMGSTACK") ? 1 : 0;
        fprintf(stderr, "[img-ret] %s: restore-on-host-return image model %s\n",
                off ? "OFF (YZ_NO_IMGSTACK)" : "ARMED",
                off ? "disabled" : "live");
        fflush(stderr);
    }
    if (off) return;
    if (ctx->image_id != saved_img) {
        static int rl = 0; if (rl < 24) { rl++;
            fprintf(stderr, "[img-ret] spu=%X host-return restore image %d -> %d (pc=0x%05X)\n",
                    ctx->spu_id, ctx->image_id, saved_img, ctx->pc & SPU_LS_MASK);
            fflush(stderr); }
        ctx->image_id = saved_img;
    }
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

/* Event-armed tracing (YZ_SPU_TRACE_EVARM, 2026-07-02): when set alongside
 * YZ_SPU_TRACE, arming is HELD until an event site sets this flag (currently
 * the 0xA70 taskset-syscall probe below) — so the budget captures the code of
 * interest instead of boot-time dispatch-loop noise. */
int g_spu_trace_evarm = 0;

void spu_indirect_branch(spu_context* ctx)
{
    /* HOST-CALL-DEPTH GUARD (2026-06-27): a never-returning SPU coroutine/poll
     * yield (gs_task's idle poll, etc.) leaks a host frame per iteration because
     * the lifter models SPU `brsl`/`bisl` as nested host calls. Once the host
     * stack passes the threshold, unwind to the driver and re-dispatch from
     * ctx->pc on a fresh stack (SPU state is all in heap `ctx`). Stack grows
     * down, so base - sp = depth; 32MB of the 64MB SPU thread stack. Legitimate
     * SPU recursion is bounded by the 256KB LS, far below this. */
    if (g_spu_restart_jmp && g_spu_stack_base) {
        char probe;
        if ((size_t)(g_spu_stack_base - &probe) > (size_t)(32u * 1024u * 1024u)) {
            static int n = 0; if (n < 8) { n++;
                fprintf(stderr, "[spu-restack] host stack ~%zuMB at pc=0x%05X image=%d -> unwind + re-dispatch\n",
                        (size_t)(g_spu_stack_base - &probe) >> 20,
                        ctx->pc & SPU_LS_MASK, ctx->image_id);
                fflush(stderr); }
            g_spu_trampoline_fn = 0;
            longjmp(*g_spu_restart_jmp, 1);
        }
    }

    /* s31 W2LIFE: per-SPU host-liveness census (always on -- three stores; read
     * by yz_w2life_dump). Discriminates "SPU thread wedged/frozen" (hops static,
     * lastpc names the site) from "alive but the workload is never re-selected"
     * (hops climb under other images) -- the H-wedge/H-accounting fork of
     * scratch/s31_consumer_death.md §6. */
    { unsigned _wi = ctx->spu_id & 7u;
      g_yz_spu_hops[_wi]++;
      g_yz_spu_lastpc[_wi] = ctx->pc & SPU_LS_MASK;
      g_yz_spu_lastimg[_wi] = (int)ctx->image_id; }

    /* RESIDENCY RE-ADOPTION at the workload-module entry (s24 image model).
     * The DMA layer records which module image is resident at LS 0xA00
     * (module_img_a00, spu_dma.h); the kernel may perform that load inside a
     * subroutine whose host-return bracket restores the kernel image before
     * the dispatch branch, so the DMA-time image_id switch alone is not
     * durable. The module ENTRY is unambiguous: whoever branches to 0xA00 is
     * entering the resident module — adopt its recorded image here, keeping
     * every image-keyed heuristic below (coret, resume, diags) working. */
    if ((ctx->pc & SPU_LS_MASK) == 0xA00u && ctx->module_img_a00 > 0
            && ctx->image_id != ctx->module_img_a00) {
        static int ra = 0; if (ra < 16) { ra++;
            fprintf(stderr, "[img-res] spu=%X entry 0xA00: image %d -> %d (resident module)\n",
                    ctx->spu_id, ctx->image_id, ctx->module_img_a00);
            fflush(stderr); }
        ctx->image_id = ctx->module_img_a00;
    }

    /* SPURS TASK LAUNCH (image switch). The taskset policy (image 2) launches an
     * SPU task by running Sony's real spursTasksetStartTask, which ends in
     * `bi savedContextLr` -> the task entry. That entry is in the task region
     * (LS >= CELL_SPURS_TASK_TOP 0x3000), OUTSIDE the policy image (0xA00-0x2840).
     * The lifted StartTask has already set the SPURS task ABI registers; we only
     * need to switch image_id to the task's lifted image so its code resolves
     * (else the task runs under image 2 and the first computed branch faults as
     * an "unknown branch"). Task image is keyed off TaskInfo.elf EA @ LS 0x2794
     * (same mapping as spu_task_launch): gs_task 0x0127A580 -> image 0,
     * cri_audio 0x012B4980 -> image 3. */
    if (ctx->image_id == 2) {
        uint32_t tpc = ctx->pc & SPU_LS_MASK;
        if (tpc >= 0x3000u && tpc < 0x40000u) {
            const unsigned char* ls = ctx->ls;
            uint32_t elf = (((uint32_t)ls[0x2794]<<24)|((uint32_t)ls[0x2795]<<16)
                           |((uint32_t)ls[0x2796]<<8)|ls[0x2797]) & 0xFFFFFFF8u;
            const spu_image_desc* imd = spu_image_for_elf(elf);
            int img = imd ? imd->image_id : -1;
            if (img >= 0) {
                ctx->image_id = img;
                /* FRESH START (branch target == ELF entry, not a mid-body resume):
                 * zero the task image's BSS ([filesz,memsz) spans), per the ELF
                 * contract Sony's LoadElf honors -- our image deploy copies only
                 * filesz, leaving prior-image bytes in the BSS span. Spans and
                 * entries come from the generated image table (auto-derived from
                 * the ELF headers). Resume paths must NOT zero (LS persists
                 * across yields by design). */
                if (tpc == imd->entry)
                    spu_image_zero_bss(ctx->ls, imd);
                /* YZ_CTXWATCH: this natural-launch path is the one the wid4
                 * pool tasks take (s26ride2: zero registrations from the
                 * legacy spu_task_launch hook while [fe0] published twice) —
                 * register the ctxsave block + log the cycle here too. */
                yz_ctxw_cycle(ctx, img, tpc, tpc != imd->entry);
                /* The lifted policy StartTask does NOT propagate the SPURS task-ABI
                 * registers on this natural-launch path -- measured (pt47): the
                 * cri_audio task enters with gpr3=0, so its context base is null and
                 * the first vtable dispatch (`bisl $r31` @LS 0x3EB4) reads a null
                 * pointer -> branch to LS 0 -> halt. Inject the task ABI the same way
                 * spu_task_launch does: gpr3 = taskInfo->args (LS 0x2780), gpr4 =
                 * spurs. Only when gpr3 is actually unset (don't clobber a good value),
                 * and only for the codec image (gs_task carries its args already). */
                /* (2026-07-02: FRESH entry only — tpc==0x3070. On a dispatch
                 * RESUME, r3 carries the taskset-syscall result to the resumed
                 * task (RPCS3 spursTasksetSyscallEntry:1366); injecting args
                 * there would clobber it.) */
                if (img == 3 && tpc == 0x3070u && ctx->gpr[3]._u32[0] == 0) {
                    ctx->gpr[3] = spu_ls_read128(ctx, 0x2780u);
                    ctx->gpr[4]._u32[3] = 0x40197C80u;
                    static int gi = 0; if (gi < 4) { gi++;
                        fprintf(stderr, "[task-abi] injected cri_audio gpr3=%08X_%08X_%08X_%08X\n",
                                ctx->gpr[3]._u32[0], ctx->gpr[3]._u32[1],
                                ctx->gpr[3]._u32[2], ctx->gpr[3]._u32[3]);
                        fflush(stderr); }
                }
                static int n = 0; if (n < 8) { n++;
                    fprintf(stderr, "[task-launch] policy bi savedContextLr -> %s entry=0x%05X image=%d "
                            "(natural StartTask launch)\n", imd->name, tpc, img);
                    fflush(stderr); }
                /* SPURS context-replacement (2026-06-28). The policy's StartTask
                 * `bi savedContextLr` reaches here DEEP inside the policy's nested
                 * StartTask-helper SPU_DRAINs (policy 0x1C50 C-calls helper 0x13B0
                 * with a nested drain). Falling through to the in-line fn(ctx)
                 * dispatch runs the task prologue on that nested drain; when an
                 * enclosing helper C-frame returns mid-prologue, the helper loop-back
                 * (policy_module.c:5884) clobbers the task's trampoline (gs_task:
                 * 0x305C -> policy 0x1C50 under image 0) and the body never runs.
                 * cri_audio only survives by reaching its 0xA70 yield before the
                 * frame pops -- luck of depth. Per the SPURS model the launch is a
                 * CONTEXT REPLACEMENT: the policy PM frame is gone and the task owns
                 * the SPU until it yields (LS 0xA70). ctx->pc = task entry and
                 * image_id is switched, so unwind to the driver (lv2_register.c:458)
                 * which re-dispatches spu_indirect_branch from ctx->pc on a fresh
                 * top-level stack -- the task body then drains at the top level (the
                 * same footing cri_audio's spu_task_launch resume already runs on).
                 * The policy re-enters on the task's 0xA70 cross-image yield. Env
                 * kill-switch YZ_NO_LAUNCH_UNWIND. */
                static int no_unwind = -1;
                if (no_unwind < 0) no_unwind = getenv("YZ_NO_LAUNCH_UNWIND") ? 1 : 0;
                if (g_spu_restart_jmp && !no_unwind) {
                    g_spu_trampoline_fn = 0;
                    longjmp(*g_spu_restart_jmp, 1);
                }
            }
        }
    }
    /* SPURS JOBCHAIN job dispatch (2026-07-03, images 14/15). The job module
     * (image 13) loads each descriptor's binary into free LS past its own end
     * (module spans LS [0xA00,0x4880)) and `bisl`s to its entry. The DMA
     * recorder (spu_dma.h) captured WHERE each known binary is resident in
     * this context; switch to that lifted image so the job's code resolves.
     * Spans are the measured descriptor sizeBinary values (jobA 0x9540 = the
     * 14-way bulk worker, jobB 0x14C0 = the event-flag notify job). */
    {
        static const uint32_t job_span[2] = { 0x9540u, 0x14C0u };
        uint32_t jpc = ctx->pc & SPU_LS_MASK;
        int family = (ctx->image_id >= 13 && ctx->image_id <= 15);
        int jimg = -1;
        if (family && jpc >= 0xA00u && jpc < 0x4880u) {
            jimg = 13;                       /* back into the resident module */
        } else {
            for (int i = 0; i < 2; i++)
                if (ctx->job_bin_base[i]
                        && jpc >= ctx->job_bin_base[i]
                        && jpc <  ctx->job_bin_base[i] + job_span[i])
                    { jimg = 14 + i; break; }
            /* A span hit from OUTSIDE the jobchain family: an SPU that lost
             * its image to a mid-cycle kernel adoption (module code runs as
             * straight-line C, so a stale image_id only surfaces at the next
             * computed branch = the job entry; measured as the LS-0 death
             * under image 0, first jobval boot). Content-verify before
             * switching: the resident job module at LS 0xA00 is the ground
             * truth that this LS really is a jobchain world. */
            if (jimg > 0 && !family) {
                extern uint8_t* vm_base;
                if (memcmp(ctx->ls + 0xA00u, vm_base + 0x0202A180u, 16) != 0)
                    jimg = -1;
            }
        }
        if (jimg > 0 && jimg != ctx->image_id) {
            static int jl = 0; if (jl < 16) { jl++;
                fprintf(stderr, "[job-launch] spu=%X image %d -> %d at LS 0x%05X (lr=0x%05X)\n",
                        ctx->spu_id, ctx->image_id, jimg, jpc,
                        ctx->gpr[0]._u32[0] & SPU_LS_MASK);
                /* Identity forensics (2026-07-08): the recorded per-context binary
                 * bases + the RESIDENT head bytes at the branch target, so the log
                 * proves WHICH binary is loaded there (jobA head 43F79802..., jobB
                 * head 43494E02..., from the EBOOT at 0x01254500/0x01275A00). */
                fprintf(stderr, "[job-launch]   jobbase A=0x%05X B=0x%05X ls@0x%05X:",
                        ctx->job_bin_base[0], ctx->job_bin_base[1], jpc);
                for (int bi = 0; bi < 16; bi++)
                    fprintf(stderr, " %02X", ctx->ls[(jpc + bi) & SPU_LS_MASK]);
                fprintf(stderr, "\n");
                fflush(stderr); }
            ctx->image_id = jimg;
        }
    }
    /* DIAG (env YZ_JOBTRACE, 2026-07-08, capped): computed-branch trail of the
     * jobchain JOB binaries (images 14/15) — pc, link, r3 — the coarse round-1
     * execution path of jobA/jobB now that the dual-base dispatch fix runs
     * their real code. Shows how deep the job gets and through which exit it
     * returns to the module without setting the IWL flag. */
    { static int jt = -1;
      if (jt < 0) { jt = getenv("YZ_JOBTRACE") ? 1 : 0;
          if (jt) { fprintf(stderr, "[jobtrace] armed\n"); fflush(stderr); } }
      if (jt && (ctx->image_id == 14 || ctx->image_id == 15)) {
          static int jn = 0; if (jn < 400) { jn++;
              fprintf(stderr, "[jobtrace] spu=%X img=%d -> 0x%05X lr=0x%05X r3=%08X_%08X\n",
                      ctx->spu_id, ctx->image_id, ctx->pc & SPU_LS_MASK,
                      ctx->gpr[0]._u32[0] & SPU_LS_MASK,
                      ctx->gpr[3]._u32[0], ctx->gpr[3]._u32[1]);
              fflush(stderr); } } }
    /* THROWAWAY DIAG (env YZ_POLTRACE): raw PC path of the taskset POLICY (image 2)
     * on every indirect branch -- pc, link(gpr0), wklCurrentId(LS 0x1DC). Shows where
     * the dispatched policy goes and where it yields (vs the oracle: 0xA00 entry ->
     * 0x2308 pollStatus -> 0x838 exitToKernel/0x290 selectWorkload -> resume 0x231C ->
     * SELECT_TASK -> LoadElf -> 0x1CC0 StartTask -> bi 0x3050). Non-prof, non-diverging. */
    { static int pt = -1; if (pt < 0) pt = getenv("YZ_POLTRACE") ? 1 : 0;
      if (pt && ctx->image_id == 2) {
          static int n = 0; if (n < 600) { n++;
              uint32_t lpc = ctx->pc & SPU_LS_MASK;
              uint32_t link = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
              uint32_t wcl = ((uint32_t)ctx->ls[0x1DC]<<24)|((uint32_t)ctx->ls[0x1DD]<<16)
                           |((uint32_t)ctx->ls[0x1DE]<<8)|ctx->ls[0x1DF];
              fprintf(stderr, "[poltrace] pc=0x%05X link=0x%05X wcl=0x%X gpr3=%08X\n",
                      lpc, link, wcl, ctx->gpr[3]._u32[0]);
              fflush(stderr); } } }
    /* DIAG (env YZ_WID0_REQ -- wid0 vs wid2 request-path
     * comparison, RETIRE with the wid0-dispatch frontier): the policy's
     * syscall re-entry is LS 0xA70 (module entry -- saves LR@0x2C80/SP@0x2C90,
     * the taskset-syscall handler every task/self dispatch funnels through;
     * LS 0xA70 = the taskset syscall handler). On EVERY re-entry to 0xA70
     * under image 2, dump: the RESIDENT taskset EA this policy instance has
     * loaded (LS 0x27B8 taskset bptr, low32 @0x27BC -- same field [task-launch]
     * already decodes), the request code (gpr3.word0 -- the ABI arg
     * spursTasksetProcessRequest's `s32 request` per cellSpursSpu.cpp:1413),
     * and the 6 taskset bitset words from the LOCAL COPY the policy just
     * operated on (tempAreaTaskset @ LS 0x2700, cellSpurs.h:1237 layout,
     * word0 of each = task 0's bit at 0x80000000): running@2700 ready@2710
     * pending_ready@2720 enabled@2730 signalled@2740 waiting@2750. Filtered to
     * the wid0/pxd taskset (0x40199D00) and wid2/gs_task taskset (captured via
     * g_yz_taskset_ea) for a side-by-side compare -- does wid0 ever issue
     * SELECT_TASK(5), and if not, what code does it loop on. */
    { static int wr = -1; if (wr < 0) wr = getenv("YZ_WID0_REQ") ? 1 : 0;
      if (wr && ctx->image_id == 2 && (ctx->pc & SPU_LS_MASK) == 0xA70u) {
          const unsigned char* l = ctx->ls;
          uint32_t tsPtr = ((uint32_t)l[0x27BC]<<24)|((uint32_t)l[0x27BD]<<16)
                         |((uint32_t)l[0x27BE]<<8)|l[0x27BF];
          extern uint32_t g_yz_taskset_ea;
          int is_wid0 = (tsPtr == 0x40199D00u);
          int is_wid2 = (g_yz_taskset_ea && tsPtr == g_yz_taskset_ea);
          /* bring-up: unconditional hit counter + first-32 any-taskset dump,
             so a zero-match run can distinguish "0xA70 never reached under
             image 2" from "reached, but tsPtr never matches either taskset" */
          { static unsigned long any = 0; any++;
            static int an = 0; if (an < 32) { an++;
                fprintf(stderr, "[wid0-req-any] spu=%X req=%d taskset=0x%08X g_yz_taskset_ea=0x%08X hitnum=%lu\n",
                        ctx->spu_id, ctx->gpr[3]._s32[0], tsPtr, g_yz_taskset_ea, any);
                fflush(stderr); } }
          if (is_wid0 || is_wid2) {
              uint32_t run  = ((uint32_t)l[0x2700]<<24)|((uint32_t)l[0x2701]<<16)|((uint32_t)l[0x2702]<<8)|l[0x2703];
              uint32_t rdy  = ((uint32_t)l[0x2710]<<24)|((uint32_t)l[0x2711]<<16)|((uint32_t)l[0x2712]<<8)|l[0x2713];
              uint32_t pend = ((uint32_t)l[0x2720]<<24)|((uint32_t)l[0x2721]<<16)|((uint32_t)l[0x2722]<<8)|l[0x2723];
              uint32_t enb  = ((uint32_t)l[0x2730]<<24)|((uint32_t)l[0x2731]<<16)|((uint32_t)l[0x2732]<<8)|l[0x2733];
              uint32_t sig  = ((uint32_t)l[0x2740]<<24)|((uint32_t)l[0x2741]<<16)|((uint32_t)l[0x2742]<<8)|l[0x2743];
              uint32_t wait = ((uint32_t)l[0x2750]<<24)|((uint32_t)l[0x2751]<<16)|((uint32_t)l[0x2752]<<8)|l[0x2753];
              static int n0 = 0, n2 = 0;
              int* cnt = is_wid0 ? &n0 : &n2;
              if (*cnt < 60) { (*cnt)++;
                  fprintf(stderr, "[wid0-req] %s spu=%X req=%d(0x%X) taskset=0x%08X "
                          "run=%08X rdy=%08X pend=%08X enb=%08X sig=%08X wait=%08X\n",
                          is_wid0 ? "wid0" : "wid2",
                          ctx->spu_id, ctx->gpr[3]._s32[0], ctx->gpr[3]._u32[0], tsPtr,
                          run, rdy, pend, enb, sig, wait);
                  fflush(stderr); }
          }
      } }
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
        /* image 13 = the JOB policy module (jobchain, wid 1) -- it shares the
         * kernel2 poll/exit convention (bisl 0x838, poll link 0x231C). Without
         * the clean coroutine return its idle poll-yields hit the destructive
         * fresh-redispatch path and the workload rotation DIES ~+5 s (measured
         * 2026-07-03 jobfix1: last [spu-img] switch at line 1953 of 860k, all
         * tasksets starved). Image-2 semantics unchanged. */
        if (!off && (ctx->image_id == 2 || ctx->image_id == 13)) {
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
            /* The poll yield is the SAME for every taskset (the policy module at
             * LS 0xA00 is shared; link 0x231C is its poll resume point). wcl==2
             * is the gs_task-only restriction; YZ_CORET_GEN drops it so ANY
             * taskset's poll resumes (the cri_audio codec taskset = wid 3).
             * Default unchanged (gs_task only) until verified. */
            static int coret_gen = -1;
            if (coret_gen < 0) coret_gen = getenv("YZ_CORET_GEN") ? 1 : 0;
            if (lpc == 0x838u && link == 0x231Cu
                    && (wcl == 2u || ctx->image_id == 13 || coret_gen)) {
                /* s31 iteration 2 (ledger #71; MEASURED scratch/s31cure1.err): for
                 * wcl==2 the synchronous fake is WRONG whenever the kernel would
                 * SWITCH AWAY. The validation boot showed: readyCount1[2]=1 (the
                 * task correctly parked+requeued) but wklCurrentContention[2]
                 * stuck == maxContention[2] == 1 for the whole boot, SPU4 alive
                 * spinning the policy's select-verify loop (lastpc=0x290, img=2,
                 * ~150k hops/30s) -- the LLE policy loops until its own select
                 * pick equals wklCurrentId, i.e. it WAITS for the workload switch
                 * that only the real kernel 0x838 performs (kernel-mode select:
                 * subtract the yielder's wklLocContention claim, commit
                 * wklCurrentContention, update wklCurrentId, dispatch -- RPCS3
                 * cellSpursSpu.cpp:283,384-405; the exact fields the fake left
                 * poisoned). And wid2 is PINNED to this SPU (wklInfo1[2].priority
                 * nonzero on slot 4 only, [w2life:coret]) so no other kernel can
                 * ever rescue it. FIX: run the REAL lifted kernel 0x838 for the
                 * wcl==2 poll -- unwind to the driver and re-dispatch at depth 0,
                 * the identical path the s31 exit-unwind below exercised 11,520+
                 * times (wcl=1/3/32, depths 4-8) in the same boot with zero
                 * regressions. Nothing of wid2 is lost on a switch-away: at the
                 * poll instant the task is already OFF-CPU with its context saved
                 * (taskset bitsets run=0/ready/waiting, readyCount=1 -- measured),
                 * and its later resume is the acfccf6 cross-context class. The
                 * fake is retained for image 13/coret_gen (measured ZERO hits in
                 * current boots) and under YZ_CORET_LEGACY=1. */
                static int legacy2 = -1;
                if (legacy2 < 0) legacy2 = getenv("YZ_CORET_LEGACY") ? 1 : 0;
                if (!legacy2 && wcl == 2u) {
                    static int rn = 0;
                    if (rn < 16) { rn++;
                        fprintf(stderr, "[yz-coret] REAL kernel poll 0x838 wcl=%u depth=%u%s\n",
                                wcl, ctx->host_depth,
                                ctx->host_depth ? " -> unwind to depth 0" : " (at depth 0, dispatching)");
                        fflush(stderr);
                        yz_w2life_dump("poll-real");
                    }
                    if (ctx->host_depth > 0 && g_spu_restart_jmp) {
                        g_spu_trampoline_fn = 0;
                        longjmp(*g_spu_restart_jmp, 1);   /* driver re-enters from
                                                           * ctx->pc==0x838, depth 0 */
                    }
                    /* Already at depth 0: fall through -- the normal dispatch
                     * below serves the real lifted spu_func_00000838 (kernel
                     * entries are universal servers). */
                } else {
                    ctx->gpr[3]._u32[0] = ctx->gpr[3]._u32[1] = 0;
                    ctx->gpr[3]._u32[2] = ctx->gpr[3]._u32[3] = 0;
                    g_spu_trampoline_fn = 0;   /* no chain -> caller resumes at 0x231C */
                    static int rl = 0;
                    if (rl < 64) { rl++;
                        fprintf(stderr, "[yz-coret] poll yield 0x838 wcl=%u -> resume at 0x231C (fake)\n", wcl);
                        fflush(stderr);
                        yz_w2life_dump("coret");   /* s31: the wid accounting at the yield */
                    }
                    return;
                }
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

    /* ---- s31 FIX (ledger #71, scratch/s31_consumer_death.md): REAL module-exit
     * -> kernel transition. cellSpursModuleExit is a ONE-WAY jump to
     * exitToKernelAddr (LS 0x838): the kernel entry immediately resets the SPU
     * stack pointer to 0x3FFD0 (spurs_kernel2.c spu_func_00000838), so no lifted
     * host C frame above this point can ever be returned into -- the nested
     * frames mirroring the exited workload are dead by construction. Running the
     * kernel's select/contention-commit/dispatch chain NESTED inside them is the
     * seam that killed wid2 (the gcm journal consumer's taskset) at the CRI
     * bring-up: the first-ever CONTENDED workload switch on that SPU (85/85
     * boots, exactly one wcl=2 poll-yield each, always the last wid2 event; the
     * SPU thread never exits -- no [SPU] stopped line -- it just never runs wid2
     * again on any SPU). Model the transition the way the hardware does: unwind
     * the host stack to the driver (the same context-replacement mechanism as
     * the task-launch unwind above and the acfccf6 depth-aware-return class) and
     * re-dispatch the lifted kernel at 0x838 on a fresh top-level stack. SPU
     * semantics are identical (all state lives in the heap ctx); host execution
     * becomes the depth-0 kernel entry the healthy SPUs already use. NOTE the
     * POLL yield (bisl with link 0x231C, special-cased above) is NOT an exit:
     * the oracle implements it as a synchronous select-and-return (RPCS3
     * cellSpursSpu.cpp:97-119 cellSpursModulePollStatus returns
     * `wklId == wklCurrentId ? 0 : 1` to the caller, no context switch), so the
     * clean-coroutine return above is the faithful poll model and stays.
     * Kill-switch: YZ_CORET_LEGACY=1 restores the pre-s31 nested-exit behavior
     * exactly. */
    {
        static int legacy = -1;
        if (legacy < 0) legacy = getenv("YZ_CORET_LEGACY") ? 1 : 0;
        if (!legacy && (ctx->pc & SPU_LS_MASK) == 0x838u
                && ctx->host_depth > 0 && g_spu_restart_jmp) {
            uint32_t wcl = ((uint32_t)ctx->ls[0x1DC] << 24) | ((uint32_t)ctx->ls[0x1DD] << 16)
                         | ((uint32_t)ctx->ls[0x1DE] << 8) | ctx->ls[0x1DF];
            static unsigned long xn = 0; xn++;
            if (xn <= 8 || (xn & 0xFFu) == 0) {
                fprintf(stderr, "[exit-unwind] n=%lu spu=%X img=%d wcl=%u depth=%u "
                        "-> kernel 0x838 on a fresh stack\n",
                        xn, ctx->spu_id, (int)ctx->image_id, wcl, ctx->host_depth);
                fflush(stderr);
            }
            if (wcl == 2u) yz_w2life_dump("exit");   /* the wid2 switch-away snapshot */
            g_spu_trampoline_fn = 0;
            longjmp(*g_spu_restart_jmp, 1);   /* driver re-enters spu_indirect_branch
                                               * from ctx->pc == 0x838, host_depth = 0 */
        }
    }

    /* DIAG (YZ_SPU_PROF): gs_task launch + flow. gs_task loads at LS 0x3000
     * (entry 0x3050, code .. 0xBC00); it gets its EDGE-job pointer in gpr3 and
     * taskset args/spurs in gpr4 (SPURS task-start ABI, cellSpursSpu.cpp:1395).
     * Log its indirect branches (uncapped by the 160-entry [spu-ib] cap) so we
     * can see the arg it launched with and where it spins. */
    /* 2026-07-03 s8: launch-arg capture — ENTRY branch only (pc 0x3050, the
     * `bi savedContextLr` task launch) so the cap survives to the pxd freeze
     * window; every task launch logs exactly once. Own lean flag (YZ_TASKARG)
     * because YZ_SPU_PROF's per-branch overhead crawls the whole boot. */
    { static int ta = -1; if (ta < 0) ta = getenv("YZ_TASKARG") ? 1 : 0;
    if ((g_spu_prof_on || ta) && (ctx->pc & SPU_LS_MASK) == 0x3050u) {
        static int gst = 0;
        if (gst < 400) { gst++;
            fprintf(stderr, "[gstask] ib pc=0x%05X arg(gpr3)=%08X_%08X_%08X_%08X "
                    "gpr4=%08X_%08X_%08X_%08X lr=0x%05X\n", ctx->pc & SPU_LS_MASK,
                    ctx->gpr[3]._u32[0], ctx->gpr[3]._u32[1], ctx->gpr[3]._u32[2], ctx->gpr[3]._u32[3],
                    ctx->gpr[4]._u32[0], ctx->gpr[4]._u32[1], ctx->gpr[4]._u32[2], ctx->gpr[4]._u32[3],
                    ctx->gpr[0]._u32[0] & SPU_LS_MASK);
        }
    } }
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
    int serve_img = ctx->image_id;
    spu_fn fn = spu_lookup(ctx->pc, ctx->image_id, &serve_img);
    if (fn) {
        /* ADOPT-ON-SERVE (s24 image model, ledger #51): tag the context with
         * the image whose registration actually serves this dispatch (kernel
         * universal serves, image-0 wildcards), so attribution is true while
         * the foreign code runs — a wild branch inside kernel-served code now
         * reports image 16, not whatever the context last wore. Safe because
         * the caller's bracket (spu_img_restore) restores its image on the
         * host return; without the brackets this adoption would be the
         * ledger-#51 mislabel trap, so it shares the YZ_NO_IMGSTACK switch. */
        if (serve_img != ctx->image_id) {
            static int noadopt = -1;
            if (noadopt < 0) noadopt = getenv("YZ_NO_IMGSTACK") ? 1 : 0;
            if (!noadopt) {
                static int al = 0; if (al < 16) { al++;
                    fprintf(stderr, "[img-serve] spu=%X 0x%05X served by image %d (ctx was %d)\n",
                            ctx->spu_id, ctx->pc & SPU_LS_MASK, serve_img, ctx->image_id);
                    fflush(stderr); }
                ctx->image_id = serve_img;
            }
        }
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
    /* REVERSE image-switch (task -> resident SPURS runtime). The forward
     * policy(2)->task switch above has no mirror: a running SPURS task
     * (cri_audio image 3, gs_task) calls back into the resident taskset
     * policy/runtime, which lives at low LS under a DIFFERENT image. The
     * scoped lookup then misses a perfectly-lifted function and we would halt
     * on a legitimate cross-image call (measured: cri_audio @0x3070 calls
     * policy spu_polfunc_00000A70 -> "unknown branch (image 3)" -> codec dies
     * on its first runtime call -> no ADX decode -> t1 cond-9 livelock).
     * Resolve faithfully: the SPU LS is ONE shared address space; if EXACTLY
     * ONE other lifted image owns this LS address, dispatch it and adopt its
     * image_id (the forward switch restores the task image when the runtime
     * branches back to LS>=0x3000). Genuine overlap (>1 foreign owner, the
     * kernel/service/policy 0xA00+ aliasing) stays ambiguous -> falls through
     * to the halt; a truly missing/garbage target has 0 owners -> also halts,
     * so real dispatch bugs are not masked. */
    {
        uint32_t la = ctx->pc & SPU_LS_MASK;
        int foreign = 0, foreign_img = 0; spu_fn ffn = 0;
        int pol_seen = 0; spu_fn pol_fn = 0;
        /* The jobchain job binaries (images 14/15) are only ever entered from
         * the job module via the recorded-DMA switch above -- exclude them as
         * foreign owners for every other context so their addresses can't
         * push a previously exactly-one adoption into ambiguity. */
        int from_jobworld = (ctx->image_id >= 13 && ctx->image_id <= 15);
        for (uint32_t i = 0; i < s_registry_count; i++)
            if (s_registry[i].addr == la && s_registry[i].image_id != ctx->image_id) {
                if (!from_jobworld
                        && (s_registry[i].image_id == 14 || s_registry[i].image_id == 15))
                    continue;
                foreign++; foreign_img = s_registry[i].image_id; ffn = s_registry[i].fn;
                if (s_registry[i].image_id == 2) { pol_seen = 1; pol_fn = s_registry[i].fn; }
            }
        /* With ALL EBOOT task images registered (2026-07-02 batch lift), a
         * low-LS address can have several foreign owners (spuimg6 loads at LS
         * 0xF0 and shadows the policy region), which broke the exactly-one
         * rule and killed the task->policy 0xA70 syscall adoption. Faithful
         * disambiguation: a TASK image calling into the resident-runtime
         * region (< CELL_SPURS_TASK_TOP 0x3000) is calling the taskset POLICY
         * -- that is what is resident under a running task by protocol. */
        if (foreign > 1 && pol_seen && la < 0x3000u && ctx->image_id != 2) {
            foreign = 1; foreign_img = 2; ffn = pol_fn;
        }
        /* Taskset EXIT-HANDLER overlay disambiguation: on a task's EXIT
         * syscall Sony's policy DMAs the libsre exit blob (ea 0x02025500,
         * 0x680 B) to LS 0x10000 and calls it (RPCS3 spursTasksetOnTaskExit,
         * cellSpursSpu.cpp:1669/1673). Big EBOOT task images (spuimg6/9) own
         * static code at 0x10000 too, so the exactly-one rule is ambiguous
         * there. Disambiguate by CONTENT: if the bytes RESIDENT at LS 0x10000
         * are the exit blob's (the DMA that just loaded them is the ground
         * truth), the call is the exit-handler invocation -> image 12. A task
         * image running its OWN 0x10000 code never reaches this scan (exact
         * image match dispatches first). */
        if (foreign > 1 && la >= 0x10000u && la < 0x10680u) {
            extern uint8_t* vm_base;
            if (memcmp(ctx->ls + 0x10000u, vm_base + 0x02025500u, 16) == 0) {
                for (uint32_t i = 0; i < s_registry_count; i++)
                    if (s_registry[i].addr == la && s_registry[i].image_id == 12) {
                        foreign = 1; foreign_img = 12; ffn = s_registry[i].fn;
                        break;
                    }
            }
        }
        /* Kernel-context refusal (s24, closes the kern-guard bypass): a
         * KERNEL-tagged context (image 16) whose branch missed every kernel
         * registration must not silently adopt another image's code here —
         * this adopter was the unlogged "later something sets 0" leg of the
         * tid-0x2004 wander (ledger #51). Legitimate kernel-era transfers
         * into foreign code all go through explicit switches (module entry
         * residency, task launch, job spans) BEFORE the branch; a kernel
         * query landing here is a wild branch by definition — fall through
         * to the loud halt. Shares the kern-guard kill-switch. */
        if (foreign == 1 && ffn && ctx->image_id == 16) {
            static int kok = -1;
            if (kok < 0) kok = getenv("YZ_KERN_WILDCARD_OK") ? 1 : 0;
            if (!kok) {
                static int kf = 0; if (kf < 16) { kf++;
                    fprintf(stderr, "[spu-ximg] KERNEL ctx branch LS 0x%05X owned only by image %d "
                            "REFUSED (wild kernel branch, failing loud)\n", la, foreign_img);
                    fflush(stderr); }
                foreign = 0; ffn = 0;
            }
        }
        if (foreign == 1 && ffn) {
            static int xn = 0;
            if (xn < 16) { xn++;
                fprintf(stderr, "[spu-ximg] cross-image call LS 0x%05X: image %d -> %d "
                        "(resident runtime, adopting)\n", la, ctx->image_id, foreign_img);
                fflush(stderr); }
            /* TASKSET-SYSCALL probe (env YZ_CTXSAVE_WATCH, 2026-07-02, diag —
             * REMOVE when settled): at the task->policy 0xA70 syscall entry, log
             * the syscall + pre-evaluate the three bail checks Sony's
             * save-task-context performs (RPCS3 spursTasketSaveTaskContext
             * :1690-1712) — cri_audio's wait-yield never produces the save PUT
             * that gs_task's does; this names the failing check (or clears all
             * three, pointing at a lift bug in the save code itself). */
            { static int scw = -1; if (scw < 0) scw = getenv("YZ_CTXSAVE_WATCH") ? 1 : 0;
              if (scw && la == 0xA70u && foreign_img == 2) {
                  /* 2026-07-03 s8 pxd fork: the IO-service task (caller img 5)
                   * launches then instantly exits instead of parking in
                   * WAIT_SIGNAL — this names its actual request code per cycle
                   * (codes: -1 POLL_SIGNAL 0 DESTROY 1 YIELD 2 WAIT_SIGNAL
                   * 3 POLL 4 WAIT_WKL_FLAG 5 SELECT_TASK).
                   * REMOVE with the pxd-dispatch frontier. */
                  { static int s5 = 0;
                    if (ctx->image_id == 5 && s5 < 40) { s5++;
                        fprintf(stderr, "[tsyscall-pxd] spu=%X num=0x%X r4=0x%08X r5=0x%08X sp=0x%05X\n",
                                ctx->spu_id, ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0],
                                ctx->gpr[5]._u32[0], ctx->gpr[1]._u32[0]);
                        fflush(stderr); } }
                  const unsigned char* l = ctx->ls;
                  uint64_t css = 0; for (int k = 0; k < 8; k++) css = (css << 8) | l[0x2798 + k];
                  uint32_t alloc = (uint32_t)(css & 0x7Fu);
                  int lsblk = 0, stackok = 1, firstmiss = -1;
                  uint32_t sp = ctx->gpr[1]._u32[0];
                  for (int i = 0; i < 128; i++)
                      if (l[0x27A0 + (i >> 3)] & (0x80u >> (i & 7))) lsblk++;
                  for (int i = (int)(sp >> 11); i < 128; i++)
                      if (!(l[0x27A0 + (i >> 3)] & (0x80u >> (i & 7)))) { stackok = 0; firstmiss = i; break; }
                  static int sn = 0; if (sn < 12) { sn++;
                      fprintf(stderr, "[tsyscall] spu=%X num=0x%X args=0x%08X | css=0x%016llX (ea=0x%08X alloc=%u) "
                              "lsblk=%d sp=0x%05X | bail: css0=%d blk>alloc=%d stackmiss=%d(blk %d)\n",
                              ctx->spu_id, ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0],
                              (unsigned long long)css, (uint32_t)(css & ~0x7Full), alloc,
                              lsblk, sp, css == 0, lsblk > (int)alloc, !stackok, firstmiss);
                      fflush(stderr); }
                  g_spu_trace_evarm = 1;   /* release event-armed tracing at the syscall */
              } }
            ctx->image_id = foreign_img;
            ffn(ctx);
            return;
        }
    }
    /* CRI-CODEC vtable-dispatch diagnostic (pt47): the cri_audio task halts on a
     * `bisl $r31` (LS 0x3EB4) where $r31 -- a function pointer read from an LS
     * struct at gpr33 = base+0x1C -- is 0, so it branches to LS 0. Dump the source:
     * gpr33 + the LS quadword ls_read128 actually fetched + the base regs.
     *   quadword all-zero            => the table is empty/un-DMA'd (setup bug)
     *   non-zero word at gpr33&0xF   => our rotqby/ls_read128 extraction is wrong. */
    if (ctx->image_id == 3 && (ctx->pc & SPU_LS_MASK) == 0) {
        static int v = 0;
        if (v < 4) { v++;
            uint32_t a33 = ctx->gpr[33]._u32[0] & SPU_LS_MASK;
            uint32_t qw  = a33 & ~0xFu;        /* the 16B-aligned quadword that was loaded */
            fprintf(stderr, "[cri-vtbl] target(gpr31)=0x%08X gpr33=0x%05X (off %u) "
                    "gpr35=0x%08X gpr81=0x%08X | LS[qw 0x%05X]:",
                    ctx->gpr[31]._u32[0], a33, a33 & 0xFu,
                    ctx->gpr[35]._u32[0], ctx->gpr[81]._u32[0], qw);
            for (int i = 0; i < 16; i++) fprintf(stderr, " %02X", ctx->ls[(qw + i) & SPU_LS_MASK]);
            fprintf(stderr, "\n"); fflush(stderr);
        }
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
            /* s24 SOURCE ATTRIBUTION (the 0x2004 phantom-death race, DONT_RECHASE
             * #34, measured load-bearing in scratch/s24ride2.err: the dead kernel
             * thread beheads the wid4 round publisher at ~round 24 and the whole
             * boot wedges on SEMAPHORE_ACQUIRE). The TARGET (0) never named the
             * bug; the host return address is inside the lifted SPU function that
             * executed the `bi` -- resolve the RVA against yakuza_recomp.map
             * (crash-symbolization workflow, LESSONS #12). spu_id names the
             * victim thread. */
#if defined(_MSC_VER) && defined(_WIN32)
            { void* ra = _ReturnAddress();
              /* print the module-relative RVA too: raw pointers are useless
               * post-mortem under ASLR (learned from s24dw.err's dead catch). */
              extern void* __stdcall GetModuleHandleA(const char*);
              uintptr_t mod = (uintptr_t)GetModuleHandleA(0);
              fprintf(stderr, "[SPU] unknown-branch SOURCE: spu=%X host_ra=%p "
                      "rva=0x%llX (map preferred base 0x140000000: symbol at "
                      "0x140000000+rva in yakuza_recomp.map)\n",
                      ctx->spu_id, ra,
                      (unsigned long long)((uintptr_t)ra - mod)); }
#endif
            /* s31 iteration 3 (ledger #34/#42/#71; MEASURED scratch/s31cure2.err:
             * 4746-4780): the long-standing 0x2004 null-branch death is now
             * PINNED to gs_task's first-work-item vtable dispatch -- LS 0x3CA4
             * `bi $r6` (via 0x4F5C, gpr0=0x4F60 in every historical sighting):
             * object = *(gs_ctx+0xE0), method = (*(*(obj+12)))[+8] read as ZERO
             * from LS, fired at the first CRI-phase work item (a wid4 signal
             * raise landed the same instant). At the halt the registers still
             * hold the whole chain (0x3C88 ori r3,r2: gpr3 = the object LS addr,
             * gpr9 = the vtable LS addr, gpr8 = vtable+8, gpr6 = the null
             * method). Dump gpr3-9 + the object/vtable quads so the next
             * sighting names the un-initialized structure outright. */
            fprintf(stderr, "[SPU] unknown-branch REGS: g3=%08X g4=%08X g5=%08X "
                    "g6=%08X g7=%08X g8=%08X g9=%08X\n",
                    ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0], ctx->gpr[5]._u32[0],
                    ctx->gpr[6]._u32[0], ctx->gpr[7]._u32[0], ctx->gpr[8]._u32[0],
                    ctx->gpr[9]._u32[0]);
            { uint32_t obj = ctx->gpr[3]._u32[0] & SPU_LS_MASK;
              uint32_t vtb = ctx->gpr[9]._u32[0] & SPU_LS_MASK;
              fprintf(stderr, "[SPU] unknown-branch LS[obj 0x%05X]:", obj);
              for (int i = 0; i < 16; i++) fprintf(stderr, " %02X", ctx->ls[(obj + i) & SPU_LS_MASK]);
              fprintf(stderr, "  LS[vtbl 0x%05X]:", vtb);
              for (int i = 0; i < 16; i++) fprintf(stderr, " %02X", ctx->ls[(vtb + i) & SPU_LS_MASK]);
              fprintf(stderr, "\n"); }
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
/* Which SPU/image spu_trace_rt should emit for. Defaults match the legacy prof
 * path (spu 0x2000, any image); the YZ_SPU_TRACE arm below locks them to the
 * traced SPU+image so register lines pair with the (image-gated) PC lines. */
static uint32_t s_trace_spuid = 0x2000u;
static int      s_trace_img   = -1;

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
    /* RELIABLE POLICY TRACE (env YZ_SPU_TRACE) -- decoupled from YZ_SPU_PROF (which
     * force-launches gs_task + diverges off the movie). Traces the taskset POLICY
     * (image 2) on the DEFAULT movie boot: locks to the first image-2 SPU, arms once
     * the policy is actually running, logs every instruction PC (exact literal -- no
     * fn->addr inversion) + a |K/|P marker for the kernel(<0xA00)/policy boundary.
     * This is the lasting reliable SPU tracer: relift the module with --trace, run
     * with YZ_SPU_TRACE=1, read scratch/spu_trace.txt. */
    {
        static int tmode = -1;
        if (tmode < 0) tmode = getenv("YZ_SPU_TRACE") ? 1 : 0;
        if (tmode) {
            { static int timg = -1;
              if (timg < 0) { const char* e = getenv("YZ_SPU_TRACE_IMG");
                              timg = e ? (int)strtol(e, 0, 0) : 2; }
              if (ctx->image_id != timg) return; }   /* env-selected image (default 2) */
            { static int evarm = -1;
              if (evarm < 0) evarm = getenv("YZ_SPU_TRACE_EVARM") ? 1 : 0;
              extern int g_spu_trace_evarm;
              if (evarm && !g_spu_trace_evarm) return; }   /* hold for the event site */
            /* One SPU, no interleave. Default: lock to the FIRST SPU seen running
             * the traced image. YZ_SPU_TRACE_SPU=<id> targets a specific SPU
             * instead (e.g. 0x2002) -- needed when several SPUs run the same
             * image and the interesting one isn't first (the cri_audio
             * startTask instance vs the healthy natural-launch instance). */
            static int tlock = -1;
            if (tlock < 0) { const char* e = getenv("YZ_SPU_TRACE_SPU");
                             tlock = e ? ((e[0]=='a') ? -3 : (int)strtol(e, 0, 0)) : -2; }
            if (tlock == -2) tlock = (int)ctx->spu_id;
            if (tlock != -3 && (int)ctx->spu_id != tlock) return;  /* "any" = -3 */
            if (!s_trace_armed) {
                s_trace_armed = 1;
                { const char* n = getenv("YZ_SPU_TRACE_N");   /* instruction budget */
                  s_trace_budget = n ? strtol(n, 0, 0) : 600000; }
                if (!s_trace_fp) s_trace_fp = fopen("scratch/spu_trace.txt", "w");
                if (!s_trace_fp) s_trace_fp = stderr;
                /* Unbuffered: the traced SPU can halt (crash) with the tail of the
                 * trace -- the approach into the fault -- stuck in the stdio buffer
                 * when the host process is killed. Fidelity over speed here. */
                setvbuf(s_trace_fp, NULL, _IONBF, 0);
                uint32_t wcl = ((uint32_t)ctx->ls[0x1DC]<<24)|((uint32_t)ctx->ls[0x1DD]<<16)
                             |((uint32_t)ctx->ls[0x1DE]<<8)|ctx->ls[0x1DF];
                { const char* e = getenv("YZ_SPU_TRACE_SPU");
                  s_trace_spuid = (e && e[0] == 'a') ? 0xFFFFFFFFu   /* any */
                                : (uint32_t)ctx->spu_id; }  /* pair rt-lines to this SPU */
                s_trace_img   = (int)ctx->image_id;       /* ...and this image */
                fprintf(s_trace_fp, "# YZ_SPU_TRACE armed spu=%X img=%d wklCurrentId=%u budget=%ld\n",
                        ctx->spu_id, ctx->image_id, wcl, s_trace_budget);
            }
            if (s_trace_budget <= 0) {
                if (s_trace_budget == 0) {
                    fprintf(s_trace_fp, "# trace budget exhausted\n");
                    s_trace_budget = -1;
                }
                return;
            }
            s_trace_budget--;
            uint32_t lp = pc & SPU_LS_MASK;
            /* dump the clear-loop count regs at setup so we can see the garbage count
             * + where gpr4 came from (the LS-clear size must be 128-aligned). */
            if (lp == 0x2318u) {
                static int p18 = 0; if (p18 < 20) { p18++;
                const unsigned char* L = ctx->ls;
                fprintf(s_trace_fp, "%05X |P  gpr2._u32[0]=%08X  rawLS[0x1E0..3]=%02X%02X%02X%02X  rawLS[0x1E4..7]=%02X%02X%02X%02X\n",
                        lp, ctx->gpr[2]._u32[0], L[0x1E0],L[0x1E1],L[0x1E2],L[0x1E3], L[0x1E4],L[0x1E5],L[0x1E6],L[0x1E7]);
                } else fprintf(s_trace_fp, "%05X |P\n", lp);
            } else if (lp == 0x1CB4u) {
                static int rl = 0; if (rl < 30) { rl++;
                const unsigned char* L = ctx->ls;
                #define TSW(o) (((uint32_t)L[(o)]<<24)|((uint32_t)L[(o)+1]<<16)|((uint32_t)L[(o)+2]<<8)|L[(o)+3])
                fprintf(s_trace_fp, "%05X |P  taskInfo: args@2780=%08X_%08X elf@2790=%08X_%08X ctxsave@2798=%08X_%08X lspat@27A0=%08X | TS run@2700=%08X rdy@2710=%08X | idx(gpr5)=%08X\n",
                        lp, TSW(0x2780), TSW(0x2784), TSW(0x2790), TSW(0x2794), TSW(0x2798), TSW(0x279C), TSW(0x27A0),
                        TSW(0x2700), TSW(0x2710), ctx->gpr[5]._u32[0]);
                #undef TSW
                } else fprintf(s_trace_fp, "%05X |P\n", lp);
            } else if (lp == 0x1CBCu || lp == 0x1CC0u || lp == 0x1C40u || lp == 0x2308u) {
                static int rl2 = 0; if (rl2 < 40) { rl2++;
                fprintf(s_trace_fp, "%05X %s  gpr2=%08X gpr3=%08X gpr5=%08X gpr8=%08X_%08X gpr9=%08X\n",
                        lp, lp < 0xA00u ? "|K" : "|P",
                        ctx->gpr[2]._u32[0], ctx->gpr[3]._u32[0], ctx->gpr[5]._u32[0],
                        ctx->gpr[8]._u32[0], ctx->gpr[8]._u32[3], ctx->gpr[9]._u32[0]); }
                else fprintf(s_trace_fp, "%05X %s\n", lp, lp < 0xA00u ? "|K" : "|P");
            } else {
                fprintf(s_trace_fp, "%05X %s\n", lp, lp < 0xA00u ? "|K" : "|P");
            }
            return;
        }
    }
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
    if (s_trace_spuid != 0xFFFFFFFFu && ctx->spu_id != s_trace_spuid) return;
    if (s_trace_img >= 0 && (int)ctx->image_id != s_trace_img) return;
    u128 v = ctx->gpr[rt & 0x7F];
    fprintf(s_trace_fp, "  r%-3u %016llX %016llX\n",
            (unsigned)(rt & 0x7F),
            (unsigned long long)v._u64[0],
            (unsigned long long)v._u64[1]);
}

#ifdef __cplusplus
}
#endif
