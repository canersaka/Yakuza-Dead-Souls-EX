/* Included after run_job: bounded parallel job execution (two workers, one
 * command owner); disabled by YZ_SPURS_PARALLEL_JOBS=0. */
#ifndef PS3RECOMP_JOB_PARALLEL_POOL_H
#define PS3RECOMP_JOB_PARALLEL_POOL_H

typedef struct JobPoolWorker {
    struct JobPool* pool;
    nthread thread;
    int created, busy;
    u32 descriptor_ea, descriptor_size;
    u8 descriptor[0x400];
    u64 sequence;
    u32 alternate_slot, next_dma_tag, spu_id;
} JobPoolWorker;

typedef struct JobPool {
    JobChainState* chain;
    nmutex mutex;
    ncond changed;
    int stop, error;
    u32 count, active;
    u64 next_sequence, error_sequence;
    JobPoolWorker worker[2];
} JobPool;

static int job_pool_busy(const JobPool* pool)
{
    for (u32 i = 0; i < pool->count; ++i)
        if (pool->worker[i].busy) return 1;
    return 0;
}

static void job_pool_worker_run(JobPoolWorker* worker)
{
    JobPool* pool = worker->pool;
    mx_lock(&pool->mutex);
    for (;;) {
        while (!worker->busy && !pool->stop)
            cv_wait(&pool->changed, &pool->mutex);
        if (pool->stop) break;
        ++pool->active;
        mx_unlock(&pool->mutex);
        const int rc = run_job_with_state(pool->chain, worker->descriptor_ea,
            worker->descriptor_size, worker->descriptor,
            &worker->alternate_slot, &worker->next_dma_tag, worker->spu_id);
        mx_lock(&pool->mutex);
        --pool->active;
        if (rc && (!pool->error || worker->sequence < pool->error_sequence)) {
            pool->error = rc;
            pool->error_sequence = worker->sequence;
        }
        worker->busy = 0;
        cv_wake_all(&pool->changed);
    }
    mx_unlock(&pool->mutex);
}
#if defined(_WIN32)
static DWORD WINAPI job_pool_worker_proc(LPVOID p)
{ job_pool_worker_run((JobPoolWorker*)p); return 0; }
#else
static void* job_pool_worker_proc(void* p)
{ job_pool_worker_run((JobPoolWorker*)p); return NULL; }
#endif

static int job_pool_drain(JobPool* pool)
{
    mx_lock(&pool->mutex);
    while (job_pool_busy(pool)) cv_wait(&pool->changed, &pool->mutex);
    const int rc = pool->error;
    mx_unlock(&pool->mutex);
    return rc;
}

static int job_pool_destroy(JobPool* pool)
{
    const int rc = job_pool_drain(pool);
    mx_lock(&pool->mutex);
    pool->stop = 1;
    cv_wake_all(&pool->changed);
    mx_unlock(&pool->mutex);
    for (u32 i = 0; i < pool->count; ++i) {
        if (!pool->worker[i].created) continue;
#if defined(_WIN32)
        WaitForSingleObject(pool->worker[i].thread, INFINITE);
        CloseHandle(pool->worker[i].thread);
#else
        pthread_join(pool->worker[i].thread, NULL);
#endif
    }
    mx_destroy(&pool->mutex);
#if !defined(_WIN32)
    pthread_cond_destroy(&pool->changed);
#endif
    free(pool);
    return rc;
}

static JobPool* job_pool_create(JobChainState* chain)
{
    /* Two workers unless YZ_SPURS_PARALLEL_JOBS=0 asks for serial jobs. */
    const char* value = getenv("YZ_SPURS_PARALLEL_JOBS");
    if ((value && !strcmp(value, "0")) || chain->parallel_capacity < 2) return NULL;
    JobPool* pool = (JobPool*)calloc(1, sizeof(*pool));
    if (!pool) return NULL;
    pool->chain = chain;
    pool->count = 2;
    mx_init(&pool->mutex);
    cv_init(&pool->changed);
    for (u32 i = 0; i < pool->count; ++i) {
        JobPoolWorker* worker = &pool->worker[i];
        worker->pool = pool;
        worker->alternate_slot = 0x4c00;
        worker->spu_id = chain->parallel_spu_ids[i];
        worker->created = nthread_create_spu(&worker->thread, job_pool_worker_proc, worker);
        if (!worker->created) {
            fprintf(stderr, "[job-parallel] worker creation failed; using serial execution\n");
            job_pool_destroy(pool);
            return NULL;
        }
    }
    return pool;
}

static int job_pool_submit(JobPool* pool, u32 descriptor_ea, u32 descriptor_size,
                           const u8* acquired_descriptor)
{
    u8 owned[0x400];
    if (descriptor_size < 0x30 || descriptor_size > sizeof(owned) ||
        (descriptor_size & 0xf)) return CELL_SPURS_JOB_ERROR_DESCRIPTOR;
    /* Acquire before waiting for capacity: the caller's snapshot storage can
     * be reused as soon as command scanning resumes. No worker reads it. */
    if (acquired_descriptor) memcpy(owned, acquired_descriptor, descriptor_size);
    else if (!job_descriptor_acquire_published(descriptor_ea,
                 (u16)descriptor_size, owned)) return CELL_SPURS_JOB_ERROR_FAULT;
#if defined(YZ_SPURS_DESCRIPTOR_SNAPSHOT_TEST)
    extern void yz_spurs_parallel_acquired_test_hook(u32);
    yz_spurs_parallel_acquired_test_hook(descriptor_ea);
#endif
    mx_lock(&pool->mutex);
    for (;;) {
        if (pool->error || atomic_load(&pool->chain->shutdown)) {
            const int rc = pool->error;
            mx_unlock(&pool->mutex);
            return rc;
        }
        for (u32 i = 0; i < pool->count; ++i) {
            JobPoolWorker* worker = &pool->worker[i];
            if (worker->busy) continue;
            worker->descriptor_ea = descriptor_ea;
            worker->descriptor_size = descriptor_size;
            memcpy(worker->descriptor, owned, descriptor_size);
            worker->sequence = ++pool->next_sequence;
            worker->busy = 1;
            cv_wake_all(&pool->changed);
            mx_unlock(&pool->mutex);
            return CELL_OK;
        }
        cv_wait(&pool->changed, &pool->mutex);
    }
}
#endif
