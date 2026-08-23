/* spu_njs -- native job scheduler foundation. See spu_njs.h.
 *
 * Every parsing and layout rule below is transcribed from the live jobchain
 * implementation in libs/spurs/cellSpurs.c (run_job / job_alloc /
 * job_layout_fits) with the source line cited at the rule. This file has no
 * live consumers; tests/spu_njs pins the contract.
 */

#include "spu_njs.h"

#include <stdlib.h>
#include <string.h>

#define NJS_LS_SIZE (256 * 1024)
#define NJS_DESCRIPTOR_LS 0x3f000u      /* run_job: descriptor_ls */
#define NJS_CONTEXT_LS 0x4940u          /* run_job: context_ls */

/* big-endian guest field reads (descriptor snapshots are guest bytes) */
static uint16_t rd16be(const uint8_t* p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}
static uint32_t rd32be(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t rd64be(const uint8_t* p)
{
    return ((uint64_t)rd32be(p) << 32) | rd32be(p + 4);
}
static void wr16be(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static void wr32be(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static void wr64be(uint8_t* p, uint64_t v)
{
    wr32be(p, (uint32_t)(v >> 32));
    wr32be(p + 4, (uint32_t)v);
}

/* ---- parse ------------------------------------------------------------- */

spu_njs_status spu_njs_parse_descriptor(const uint8_t* d,
                                        uint32_t descriptor_size,
                                        uint32_t descriptor_ea,
                                        spu_njs_job* out)
{
    if (!d || !out)
        return SPU_NJS_E_DESCRIPTOR;
    /* run_job:5140-5146 */
    if (descriptor_ea > 0xfffffff0u)
        return SPU_NJS_E_DESCRIPTOR;
    if (descriptor_size < 0x30 || descriptor_size > 0x400 ||
        (descriptor_size & 0x0f) ||
        descriptor_ea > 0xffffffffu - descriptor_size)
        return SPU_NJS_E_DESCRIPTOR;
    memset(out, 0, sizeof(*out));
    memcpy(out->descriptor, d, descriptor_size);
    out->descriptor_ea = descriptor_ea;
    out->descriptor_size = descriptor_size;
    /* run_job:5163: jobType at d[0x2C]; nonzero = BINARY2, format unknown */
    if (descriptor_size > 0x2C && d[0x2C] != 0)
        return SPU_NJS_E_BINARY2;
    /* run_job:5177-5195 */
    out->bin_ea = (uint32_t)(rd64be(d + 0x00) & ~1ull);
    out->bin_size = (uint32_t)rd16be(d + 0x08) * 16u;
    if (!out->bin_ea || !out->bin_size)
        return SPU_NJS_E_INVALID_BIN;
    const uint32_t dma_list_size = rd16be(d + 0x0a);
    const uint32_t cache_list_size = rd32be(d + 0x24);
    /* run_job:5198-5202 */
    if ((dma_list_size & 7) || (cache_list_size & 7) ||
        dma_list_size + cache_list_size > descriptor_size - 0x30 ||
        cache_list_size / 8 > 4)
        return SPU_NJS_E_DESCRIPTOR;
    out->kind = rd32be(d + 0x10);
    out->size_io = rd32be(d + 0x14) & 0x3ffffu;
    out->size_out = rd32be(d + 0x18) & 0x3ffffu;
    out->size_stack = rd16be(d + 0x1c) ? (uint32_t)rd16be(d + 0x1c) * 16u
                                       : 8192u;
    out->size_scratch = (uint32_t)rd16be(d + 0x1e) * 16u;

    /* io list (run_job:5336-5372): MFC list elements */
    out->io_count = dma_list_size / 8;
    uint32_t io_offset = 0;
    for (uint32_t i = 0; i < out->io_count; ++i) {
        const uint64_t item = rd64be(d + 0x30 + i * 8);
        const uint32_t item_size = (uint32_t)(item >> 32) & 0x7fffu;
        const uint32_t item_ea = (uint32_t)item;
        const uint32_t item_flags = (uint32_t)(item >> 32);
        const uint32_t item_alignment = item_size < 16u ? item_size : 16u;
        const uint32_t item_slot_size = (item_size + 15u) & ~15u;
        const uint32_t item_ls_offset = io_offset + (item_ea & 15u);
        if (item_size > 0x4000u || (item_flags & 0x7fff8000u) ||
            (item_size && !item_ea) ||
            (item_size < 16u && item_size != 0u && item_size != 1u &&
             item_size != 2u && item_size != 4u && item_size != 8u) ||
            (item_alignment && (item_ea & (item_alignment - 1u))) ||
            io_offset > out->size_io ||
            item_slot_size > out->size_io - io_offset ||
            (item_size && item_ls_offset + item_size >
                              io_offset + item_slot_size))
            return SPU_NJS_E_DESCRIPTOR;
        out->io[i].ea = item_ea;
        out->io[i].size = item_size;
        out->io[i].stall_notify = (item_flags >> 31) & 1u;
        io_offset += item_slot_size;
    }

    /* cache list (run_job:5277-5300): FULL high word is the size */
    out->cache_count = cache_list_size / 8;
    for (uint32_t i = 0; i < out->cache_count; ++i) {
        const uint64_t item = rd64be(d + 0x30 + dma_list_size + i * 8);
        const uint32_t item_size = (uint32_t)(item >> 32);
        if (item_size & 15u)
            return SPU_NJS_E_INVALID_BIN;
        if (item_size > NJS_LS_SIZE)
            return SPU_NJS_E_INVALID_BIN;
        out->cache_ea[i] = (uint32_t)item;
        out->cache_size[i] = item_size;
    }
    return SPU_NJS_OK;
}

/* ---- LS layout --------------------------------------------------------- */

/* job_alloc (cellSpurs.c:4877-4884), verbatim semantics */
static uint32_t njs_alloc(uint32_t* cursor, uint32_t limit, uint32_t size,
                          uint32_t alignment)
{
    if (!size) return 0;
    uint32_t at = (*cursor + alignment - 1) & ~(alignment - 1);
    if (at > limit || size > limit - at) return 0;
    *cursor = at + size;
    return at;
}

/* job_layout_fits (cellSpurs.c:4890-4920), on the typed record */
static int njs_layout_fits(const spu_njs_job* j, uint32_t slot)
{
    if (j->size_stack < 0x30u)
        return 0;
    if (j->bin_size > NJS_DESCRIPTOR_LS - slot)
        return 0;
    uint32_t cursor = (slot + j->bin_size + 1023u) & ~1023u;
    if (j->size_io && !njs_alloc(&cursor, NJS_DESCRIPTOR_LS, j->size_io, 1024))
        return 0;
    if (j->size_out && !njs_alloc(&cursor, NJS_DESCRIPTOR_LS, j->size_out, 1024))
        return 0;
    for (uint32_t i = 0; i < j->cache_count; ++i)
        if (j->cache_size[i] &&
            !njs_alloc(&cursor, NJS_DESCRIPTOR_LS, j->cache_size[i], 1024))
            return 0;
    return njs_alloc(&cursor, NJS_DESCRIPTOR_LS,
                     j->size_scratch + j->size_stack, 1024) != 0;
}

spu_njs_status spu_njs_build_ls(const spu_njs_job* job,
                                uint8_t* ls,
                                uint32_t preferred_slot,
                                const uint32_t dma_tag_pair[2],
                                uint32_t tag_parity,
                                spu_njs_guest_read_fn guest_read,
                                void* guest_user,
                                spu_njs_ls_init* init)
{
    if (!job || !ls || !guest_read || !init)
        return SPU_NJS_E_DESCRIPTOR;
    if (preferred_slot != 0x4c00u && preferred_slot != 0xe400u)
        return SPU_NJS_E_DESCRIPTOR;
    /* run_job:5349-5382: preferred slot, then the other slot as fallback */
    const uint32_t fallback_slot =
        preferred_slot == 0x4c00u ? 0xe400u : 0x4c00u;
    uint32_t slot = preferred_slot;
    if (!njs_layout_fits(job, slot)) {
        if (!njs_layout_fits(job, fallback_slot))
            return SPU_NJS_E_NOMEM;
        slot = fallback_slot;
    }
    memset(init, 0, sizeof(*init));
    init->slot = slot;

    /* run_job:5268-5270: binary */
    if (job->bin_size > NJS_LS_SIZE - slot)
        return SPU_NJS_E_INVALID_BIN;
    if (!guest_read(guest_user, job->bin_ea, ls + slot, job->bin_size))
        return SPU_NJS_E_FAULT;

    /* run_job:5272-5306: allocations in run_job's exact order */
    uint32_t cursor = (slot + job->bin_size + 1023u) & ~1023u;
    init->io_ls = njs_alloc(&cursor, NJS_DESCRIPTOR_LS, job->size_io, 1024);
    init->out_ls = njs_alloc(&cursor, NJS_DESCRIPTOR_LS, job->size_out, 1024);
    for (uint32_t i = 0; i < job->cache_count; ++i) {
        init->cache_ls[i] = njs_alloc(&cursor, NJS_DESCRIPTOR_LS,
                                      job->cache_size[i], 1024);
        if (job->cache_size[i] && !init->cache_ls[i])
            return SPU_NJS_E_NOMEM;
        if (job->cache_size[i] &&
            !guest_read(guest_user, job->cache_ea[i],
                        ls + init->cache_ls[i], job->cache_size[i]))
            return SPU_NJS_E_FAULT;
    }
    init->scratch_stack_ls = njs_alloc(&cursor, NJS_DESCRIPTOR_LS,
                                       job->size_scratch + job->size_stack,
                                       1024);
    if ((job->size_io && !init->io_ls) ||
        (job->size_out && !init->out_ls) || !init->scratch_stack_ls)
        return SPU_NJS_E_NOMEM;

    /* run_job:5336-5392: io gather with sub-quadword low-nibble placement */
    uint32_t io_offset = 0;
    for (uint32_t i = 0; i < job->io_count; ++i) {
        const uint32_t item_size = job->io[i].size;
        const uint32_t item_ea = job->io[i].ea;
        const uint32_t item_slot_size = (item_size + 15u) & ~15u;
        const uint32_t item_ls_offset = io_offset + (item_ea & 15u);
        if (item_size &&
            !guest_read(guest_user, item_ea,
                        ls + init->io_ls + item_ls_offset, item_size))
            return SPU_NJS_E_FAULT;
        io_offset += item_slot_size;
    }

    /* run_job:5394-5410: descriptor copy + context block */
    memcpy(ls + NJS_DESCRIPTOR_LS, job->descriptor, job->descriptor_size);
    memset(ls + NJS_CONTEXT_LS, 0, 48);
    wr32be(ls + NJS_CONTEXT_LS + 0x00, init->io_ls);
    for (uint32_t i = 0; i < 4; ++i)
        wr32be(ls + NJS_CONTEXT_LS + 0x04 + i * 4, init->cache_ls[i]);
    wr32be(ls + NJS_CONTEXT_LS + 0x14,
           job->descriptor_size == 64
               ? 0 : (job->descriptor_size / 128) << 28);
    wr16be(ls + NJS_CONTEXT_LS + 0x18, (uint16_t)job->io_count);
    wr16be(ls + NJS_CONTEXT_LS + 0x1a, (uint16_t)job->cache_count);
    wr32be(ls + NJS_CONTEXT_LS + 0x1c, init->out_ls);
    wr32be(ls + NJS_CONTEXT_LS + 0x20, init->scratch_stack_ls);
    init->dma_tag = dma_tag_pair[tag_parity & 1u];
    wr32be(ls + NJS_CONTEXT_LS + 0x24, init->dma_tag);
    wr64be(ls + NJS_CONTEXT_LS + 0x28, job->descriptor_ea);

    /* run_job:5420-5441: SPU ABI 2.5.1 entry stack + register spec */
    init->r0 = 0x0a70u;
    {
        const uint32_t stack_end = init->scratch_stack_ls +
                                   job->size_scratch + job->size_stack;
        const uint32_t initial_sp = stack_end - 0x30u;
        const uint32_t root_sp = stack_end - 0x10u;
        wr32be(ls + initial_sp, root_sp);
        wr32be(ls + root_sp, 0u);
        init->r1_sp = initial_sp;
        init->r1_avail = job->size_stack;
    }
    init->r3 = NJS_CONTEXT_LS;
    init->r4 = NJS_DESCRIPTOR_LS;
    init->pc = slot;
    return SPU_NJS_OK;
}

/* ---- scheduler --------------------------------------------------------- */

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define NJS_QUEUE_CAP 256

struct spu_njs_sched {
    unsigned width;
    spu_njs_exec_fn exec;
    spu_njs_complete_fn complete;
    void* user;
    int terminal;                 /* 0 live, 1 ended, 2 aborted */
    uint64_t submitted;
    uint64_t completed;
#if defined(_WIN32)
    SRWLOCK lock;
    CONDITION_VARIABLE work_cv;   /* queue not empty / stop */
    CONDITION_VARIABLE idle_cv;   /* queue empty + nothing in flight */
    spu_njs_job queue[NJS_QUEUE_CAP];
    unsigned q_head, q_tail, q_len;
    unsigned in_flight;
    int stop;
    HANDLE threads[32];
    unsigned n_threads;
#endif
};

#if defined(_WIN32)
static DWORD WINAPI njs_worker(LPVOID arg)
{
    spu_njs_sched* s = (spu_njs_sched*)arg;
    AcquireSRWLockExclusive(&s->lock);
    for (;;) {
        while (!s->q_len && !s->stop)
            SleepConditionVariableSRW(&s->work_cv, &s->lock, INFINITE, 0);
        if (!s->q_len && s->stop)
            break;
        spu_njs_job job = s->queue[s->q_head];
        s->q_head = (s->q_head + 1) % NJS_QUEUE_CAP;
        s->q_len--;
        s->in_flight++;
        ReleaseSRWLockExclusive(&s->lock);
        const int ok = s->exec(s->user, &job);
        AcquireSRWLockExclusive(&s->lock);
        s->in_flight--;
        s->completed++;
        if (s->complete) {
            /* completion order == hardware publication order */
            ReleaseSRWLockExclusive(&s->lock);
            s->complete(s->user, &job, ok);
            AcquireSRWLockExclusive(&s->lock);
        }
        if (!s->q_len && !s->in_flight)
            WakeAllConditionVariable(&s->idle_cv);
        WakeConditionVariable(&s->work_cv);
    }
    ReleaseSRWLockExclusive(&s->lock);
    return 0;
}
#endif

spu_njs_sched* spu_njs_sched_create(unsigned width,
                                    spu_njs_exec_fn exec,
                                    spu_njs_complete_fn complete,
                                    void* user)
{
    if (!exec)
        return NULL;
    spu_njs_sched* s = (spu_njs_sched*)calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->width = width;
    s->exec = exec;
    s->complete = complete;
    s->user = user;
#if defined(_WIN32)
    InitializeSRWLock(&s->lock);
    InitializeConditionVariable(&s->work_cv);
    InitializeConditionVariable(&s->idle_cv);
    if (width > 1) {
        if (width > 32)
            width = 32;
        s->width = width;
        for (unsigned i = 0; i < width; ++i) {
            s->threads[i] = CreateThread(NULL, 0, njs_worker, s, 0, NULL);
            if (s->threads[i])
                s->n_threads++;
        }
        if (!s->n_threads)
            s->width = 1;         /* degrade to sequential */
    }
#else
    s->width = 1;
#endif
    return s;
}

static void njs_drain(spu_njs_sched* s)
{
#if defined(_WIN32)
    if (s->width > 1) {
        AcquireSRWLockExclusive(&s->lock);
        while (s->q_len || s->in_flight)
            SleepConditionVariableSRW(&s->idle_cv, &s->lock, INFINITE, 0);
        ReleaseSRWLockExclusive(&s->lock);
    }
#else
    (void)s;
#endif
}

void spu_njs_sched_destroy(spu_njs_sched* s)
{
    if (!s)
        return;
#if defined(_WIN32)
    if (s->width > 1) {
        njs_drain(s);
        AcquireSRWLockExclusive(&s->lock);
        s->stop = 1;
        WakeAllConditionVariable(&s->work_cv);
        ReleaseSRWLockExclusive(&s->lock);
        WaitForMultipleObjects(s->n_threads, s->threads, TRUE, INFINITE);
        for (unsigned i = 0; i < s->n_threads; ++i)
            CloseHandle(s->threads[i]);
    }
#endif
    free(s);
}

spu_njs_status spu_njs_submit(spu_njs_sched* s, const spu_njs_job* job)
{
    if (!s || !job)
        return SPU_NJS_E_DESCRIPTOR;
    if (s->terminal)
        return SPU_NJS_E_STATE;
#if defined(_WIN32)
    if (s->width > 1) {
        AcquireSRWLockExclusive(&s->lock);
        while (s->q_len == NJS_QUEUE_CAP)
            SleepConditionVariableSRW(&s->idle_cv, &s->lock, INFINITE, 0);
        spu_njs_job* slot = &s->queue[s->q_tail];
        *slot = *job;
        slot->sequence = s->submitted++;
        s->q_tail = (s->q_tail + 1) % NJS_QUEUE_CAP;
        s->q_len++;
        WakeConditionVariable(&s->work_cv);
        ReleaseSRWLockExclusive(&s->lock);
        return SPU_NJS_OK;
    }
#endif
    spu_njs_job j = *job;
    j.sequence = s->submitted++;
    const int ok = s->exec(s->user, &j);
    s->completed++;
    if (s->complete)
        s->complete(s->user, &j, ok);
    return SPU_NJS_OK;
}

spu_njs_status spu_njs_sync(spu_njs_sched* s)
{
    if (!s)
        return SPU_NJS_E_DESCRIPTOR;
    if (s->terminal)
        return SPU_NJS_E_STATE;
    njs_drain(s);
    return SPU_NJS_OK;
}

spu_njs_status spu_njs_abort(spu_njs_sched* s)
{
    if (!s)
        return SPU_NJS_E_DESCRIPTOR;
    if (s->terminal)
        return SPU_NJS_E_STATE;
#if defined(_WIN32)
    if (s->width > 1) {
        /* stop issuing: queued-but-unstarted jobs are discarded (the chain
         * consumed them, but ABORT precedes their dispatch); in-flight jobs
         * drain -- their DMA cannot be recalled (hardware contract). */
        AcquireSRWLockExclusive(&s->lock);
        s->q_len = 0;
        s->q_head = s->q_tail = 0;
        ReleaseSRWLockExclusive(&s->lock);
        njs_drain(s);
    }
#endif
    s->terminal = 2;
    return SPU_NJS_OK;
}

spu_njs_status spu_njs_end(spu_njs_sched* s)
{
    if (!s)
        return SPU_NJS_E_DESCRIPTOR;
    if (s->terminal)
        return SPU_NJS_E_STATE;
    njs_drain(s);
    s->terminal = 1;
    return SPU_NJS_OK;
}

uint64_t spu_njs_completed(const spu_njs_sched* s) { return s ? s->completed : 0; }
uint64_t spu_njs_submitted(const spu_njs_sched* s) { return s ? s->submitted : 0; }
int spu_njs_terminal(const spu_njs_sched* s) { return s ? s->terminal : 1; }
