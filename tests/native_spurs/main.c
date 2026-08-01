#include "cellSpurs.h"
#include "cellSync.h"
#include "spu_workload.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

#define MEM_SIZE (2u * 1024u * 1024u)
static _Alignas(128) uint8_t guest[MEM_SIZE];
uint8_t* vm_base = guest;
static volatile int task_phase;
static volatile int task_abi_ok;
static volatile int job_runs;
static volatile int job_abi_ok;

typedef struct EventWaitArgs {
    CellSpursEventFlag* flag;
    uint16_t* bits;
    int32_t rc;
} EventWaitArgs;

#if defined(_WIN32)
static DWORD WINAPI event_wait_thread(LPVOID raw)
#else
static void* event_wait_thread(void* raw)
#endif
{
    EventWaitArgs* args = (EventWaitArgs*)raw;
    args->rc = cellSpursEventFlagWait(
        args->flag, args->bits, CELL_SPURS_EVENT_FLAG_OR);
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

uint64_t ppu_timebase_now(void) { return (uint64_t)clock(); }
int spu_coh_is_reserved(uint32_t ea) { (void)ea; return 0; }
void spu_coh_notify_write(uint32_t ea) { (void)ea; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
int spu_native_image_executor(spu_context* ctx, int image_id, uint32_t pc)
{
    (void)ctx; (void)image_id; (void)pc;
    return 0;
}

static uint32_t be32(const void* v)
{
    const uint8_t* p = (const uint8_t*)v;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static void put16(void* v, uint16_t x)
{
    uint8_t* p = (uint8_t*)v; p[0] = (uint8_t)(x >> 8); p[1] = (uint8_t)x;
}
static void put32(void* v, uint32_t x)
{
    uint8_t* p = (uint8_t*)v;
    p[0] = (uint8_t)(x >> 24); p[1] = (uint8_t)(x >> 16);
    p[2] = (uint8_t)(x >> 8); p[3] = (uint8_t)x;
}
static void put64(void* v, uint64_t x)
{
    put32(v, (uint32_t)(x >> 32)); put32((uint8_t*)v + 4, (uint32_t)x);
}
static int fail(const char* what, int line)
{
    fprintf(stderr, "FAIL line %d: %s\n", line, what);
    return 1;
}
#define CHECK(x) do { if (!(x)) return fail(#x, __LINE__); } while (0)

static void make_elf(uint8_t* e)
{
    memset(e, 0, 0x104);
    e[0]=0x7f; e[1]='E'; e[2]='L'; e[3]='F'; e[4]=1; e[5]=2;
    put32(e + 0x18, 0x3000);
    put32(e + 0x1c, 0x34);
    put16(e + 0x2a, 0x20);
    put16(e + 0x2c, 1);
    put32(e + 0x34, 1);
    put32(e + 0x38, 0x100);
    put32(e + 0x3c, 0x3000);
    put32(e + 0x44, 4);
    put32(e + 0x48, 4);
    e[0x100]=0x12; e[0x101]=0x34; e[0x102]=0x56; e[0x103]=0x78;
}

static void synthetic_task(spu_context* ctx)
{
    task_abi_ok =
        ctx->gpr[3]._u32[0] == 0x01020304 &&
        ctx->gpr[3]._u32[1] == 0x11121314 &&
        ctx->gpr[3]._u32[2] == 0x21222324 &&
        ctx->gpr[3]._u32[3] == 0x31323334 &&
        ctx->gpr[4]._u32[0] == 0x11223344 &&
        ctx->gpr[4]._u32[1] == 0x55667788 &&
        ctx->gpr[4]._u32[2] == 0 &&
        ctx->gpr[4]._u32[3] == 0x1000 &&
        be32(ctx->ls + 0x2794) == 0x8000 &&
        be32(ctx->ls + 0x2fb8) == 0x2700;
    ctx->gpr[80] = spu_make_preferred_u32(0xdeadbeef);
    task_phase = 1;
    ctx->gpr[3]._u32[0] = 2; /* WAIT_SIGNAL */
    if (ctx->native_spurs_syscall(ctx, ctx->native_spurs_opaque) <= 0) return;
    task_abi_ok = task_abi_ok && ctx->gpr[80]._u32[0] == 0xdeadbeef;
    task_phase = 2;
    ctx->gpr[3]._u32[0] = 0; /* EXIT */
    /* The SDK task exit wrapper saves the public exit code here before it
     * repurposes r4 for the native syscall entry. */
    put32(ctx->ls + 0x2fd0, 0x2468);
    (void)ctx->native_spurs_syscall(ctx, ctx->native_spurs_opaque);
}
static uint16_t be16(const void* v)
{
    const uint8_t* p = (const uint8_t*)v;
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static void synthetic_job(spu_context* ctx)
{
    const uint32_t context_ls = ctx->gpr[3]._u32[0];
    const uint32_t descriptor_ls = ctx->gpr[4]._u32[0];
    const uint32_t io_ls = be32(ctx->ls + context_ls);
    job_abi_ok =
        ctx->gpr[0]._u32[0] == 0x0a70 &&
        ctx->gpr[1]._u32[0] == 0x7400 &&
        context_ls == 0x4940 &&
        descriptor_ls == 0x3f000 &&
        io_ls == 0x5000 &&
        ctx->ls[context_ls + 0x18] == 0 &&
        ctx->ls[context_ls + 0x19] == 1 &&
        be32(ctx->ls + context_ls + 0x2c) == 0xe000 &&
        be32(ctx->ls + descriptor_ls + 4) == 0xf000;
    for (uint32_t i = 0; i < 16; ++i) ctx->ls[io_ls + i] ^= 0xff;
    ++job_runs;
}

static int wait_value(volatile int* value, int expected)
{
    for (unsigned i = 0; i < 1000000; ++i) {
        if (*value == expected) return 1;
#if defined(_WIN32)
        SwitchToThread();
#else
        sched_yield();
#endif
    }
    return 0;
}
static int wait_phase(int phase) { return wait_value(&task_phase, phase); }
static int wait_be32_value(const void* value, uint32_t expected)
{
    for (unsigned i = 0; i < 1000000; ++i) {
        if (be32(value) == expected) return 1;
#if defined(_WIN32)
        SwitchToThread();
#else
        sched_yield();
#endif
    }
    return 0;
}

static int test_layouts(void)
{
    CHECK(sizeof(CellSpurs) == 4096);
    CHECK(sizeof(CellSpurs2) == 8192);
    CHECK(sizeof(CellSpursTaskset) == 0x1900);
    CHECK(sizeof(CellSpursTaskset2) == 0x2900);
    CHECK(sizeof(CellSpursEventFlag) == 128);
    CHECK(sizeof(CellSpursJobChain) == 272);
    return 0;
}

static int test_multiple_instances(void)
{
    CellSpurs* first = (CellSpurs*)(guest + 0x12000);
    CellSpurs* second = (CellSpurs*)(guest + 0x14000);
    CellSpursAttribute* first_attr =
        (CellSpursAttribute*)(guest + 0x16000);
    CellSpursAttribute* second_attr =
        (CellSpursAttribute*)(guest + 0x16800);
    CHECK(cellSpursAttributeInitialize(first_attr, 1, 100, 1000, 0) == 0);
    CHECK(cellSpursAttributeInitialize(second_attr, 3, 200, 2000, 0) == 0);
    CHECK(cellSpursInitializeWithAttribute(first, first_attr) == 0);
    CHECK(cellSpursInitializeWithAttribute(second, second_attr) == 0);
    CHECK(first->bytes[0x76] == 1);
    CHECK(second->bytes[0x76] == 3);
    CHECK(cellSpursFinalize(first) == 0);
    CHECK(cellSpursFinalize(second) == 0);
    return 0;
}

static int test_task_event_queue(void)
{
    CellSpurs* spurs = (CellSpurs*)(guest + 0x1000);
    CellSpursTaskset2* ts = (CellSpursTaskset2*)(guest + 0x3000);
    CellSpursAttribute* attr = (CellSpursAttribute*)(guest + 0x6000);
    CellSpursTasksetAttribute2* taskset_attr =
        (CellSpursTasksetAttribute2*)(guest + 0x6400);
    CellSpursEventFlag* ef = (CellSpursEventFlag*)(guest + 0x6800);
    CellSpursQueue* push_queue = (CellSpursQueue*)(guest + 0x6900);
    CellSpursQueue* pop_queue = (CellSpursQueue*)(guest + 0x6a00);
    CellSpursLFQueue* lf_queue = (CellSpursLFQueue*)(guest + 0x6b00);
    uint8_t* push_data = guest + 0x7000;
    uint8_t* pop_data = guest + 0x7100;
    uint8_t* lf_data = guest + 0x7200;
    uint8_t* elf = guest + 0x8000;
    uint8_t* task_context = guest + 0x18000;
    CellSpursTaskId* id = (CellSpursTaskId*)(guest + 0x9000);
    int32_t* exit_code = (int32_t*)(guest + 0x9004);
    uint16_t* bits = (uint16_t*)(guest + 0x9010);
    uint8_t priority[8] = {1,1,1,1,1,1,1,1};

    make_elf(elf);
    spu_workload_reset();
    CHECK(spu_workload_register_direct(
              spu_workload_fingerprint(elf, 0x104), 0x104,
              synthetic_task, "synthetic-task"));
    CHECK(cellSpursAttributeInitialize(attr, 2, 100, 1000, 0) == 0);
    CHECK(cellSpursInitializeWithAttribute(spurs, attr) == 0);
    _cellSpursTasksetAttribute2Initialize(taskset_attr, 1);
    put64(taskset_attr->bytes + 0x08, 0x1122334455667788ull);
    CHECK(cellSpursCreateTaskset2(spurs, ts, taskset_attr) == 0);
    task_phase = 0;
    task_abi_ok = 0;
    {
        CellSpursTaskBinInfo bin;
        CellSpursTaskArgument arg;
        memset(&bin, 0, sizeof(bin)); memset(&arg, 0, sizeof(arg));
        put64((uint8_t*)&bin + 0x00, 0x8000);
        put32((uint8_t*)&bin + 0x08, 1024);
        put32(arg.bytes + 0x00, 0x01020304);
        put32(arg.bytes + 0x04, 0x11121314);
        put32(arg.bytes + 0x08, 0x21222324);
        put32(arg.bytes + 0x0c, 0x31323334);
        CHECK(cellSpursCreateTask2WithBinInfo(ts, id, &bin, &arg,
                                             task_context, "task", NULL) == 0);
    }
    CHECK(wait_phase(1));
    CHECK(task_abi_ok);
    CHECK(cellSpursTryJoinTask2(ts, be32(id), exit_code) == CELL_SPURS_TASK_ERROR_BUSY);
    CHECK(_cellSpursSendSignal((CellSpursTaskset*)ts, be32(id)) == 0);
    CHECK(cellSpursJoinTask2(ts, be32(id), exit_code) == 0);
    CHECK(task_phase == 2);
    CHECK(be32(exit_code) == 0x2468);
    CHECK(be32(task_context + 0x20) == 0xdeadbeef);

    CHECK(_cellSpursEventFlagInitialize(NULL, (CellSpursTaskset*)ts, ef,
                                        CELL_SPURS_EVENT_FLAG_CLEAR_AUTO,
                                        CELL_SPURS_EVENT_FLAG_ANY2ANY) == 0);
    put16(bits, 0x0001);
    CHECK(cellSpursEventFlagWait(ef, bits, CELL_SPURS_EVENT_FLAG_OR) ==
          CELL_SPURS_TASK_ERROR_STAT);
    CHECK(cellSpursEventFlagAttachLv2EventQueue(ef) == 0);
    CHECK(cellSpursEventFlagSet(ef, 0x00a0) == 0);
    put16(bits, 0x0020);
    CHECK(cellSpursEventFlagWait(ef, bits, CELL_SPURS_EVENT_FLAG_OR) == 0);
    CHECK((guest[0x9010] == 0 && guest[0x9011] == 0x20));
    CHECK((ef->bytes[0] == 0 && ef->bytes[1] == 0x80));
    for (unsigned iteration = 0; iteration < 100; ++iteration) {
        EventWaitArgs args = {ef, bits, -1};
        put16(bits, 0x0040);
#if defined(_WIN32)
        HANDLE thread = CreateThread(NULL, 0, event_wait_thread, &args, 0, NULL);
        CHECK(thread != NULL);
        CHECK(cellSpursEventFlagSet(ef, 0x0040) == 0);
        CHECK(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0);
        CloseHandle(thread);
#else
        pthread_t thread;
        CHECK(pthread_create(&thread, NULL, event_wait_thread, &args) == 0);
        CHECK(cellSpursEventFlagSet(ef, 0x0040) == 0);
        CHECK(pthread_join(thread, NULL) == 0);
#endif
        CHECK(args.rc == 0);
        CHECK(guest[0x9010] == 0 && guest[0x9011] == 0x40);
    }
    {
        EventWaitArgs args = {ef, bits, -1};
        put16(bits, 0x0001);
#if defined(_WIN32)
        HANDLE thread = CreateThread(NULL, 0, event_wait_thread, &args, 0, NULL);
        CHECK(thread != NULL);
#else
        pthread_t thread;
        CHECK(pthread_create(&thread, NULL, event_wait_thread, &args) == 0);
#endif
        for (unsigned spin = 0; spin < 1000 && be16(ef->bytes + 0x04) != 0x0001; ++spin) {
#if defined(_WIN32)
            Sleep(1);
#else
            sched_yield();
#endif
        }
        CHECK(be16(ef->bytes + 0x04) == 0x0001);
        const uint32_t slot = ef->bytes[0x06] >> 4;
        put16(ef->bytes + 0x30 + slot * 2, 0x0001);
        put16(ef->bytes + 0x04, 0);
        ef->bytes[0x07] = 1;
        cellSpursNotifyGuestWrite(0x6800, 128);
#if defined(_WIN32)
        CHECK(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0);
        CloseHandle(thread);
#else
        CHECK(pthread_join(thread, NULL) == 0);
#endif
        CHECK(args.rc == 0);
        CHECK(be16(bits) == 0x0001);
        CHECK(ef->bytes[0x07] == 0);
    }
    CHECK(cellSpursEventFlagDetachLv2EventQueue(ef) == 0);

    CHECK(_cellSpursQueueInitialize(NULL, (CellSpursTaskset*)ts, push_queue,
                                    push_data, 16, 2, 2) == 0);
    {
        _Alignas(16) uint32_t a[4] = {
            0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00
        };
        _Alignas(16) uint32_t b[4] = {0};
        CHECK(cellSpursQueuePushBody(push_queue, a, 1) == 0);
        CHECK(be32(push_queue->bytes + 0x04) == 1);
        CHECK(!memcmp(push_data, a, sizeof(a)));

        CHECK(_cellSpursQueueInitialize(NULL, (CellSpursTaskset*)ts, pop_queue,
                                        pop_data, 16, 2, 1) == 0);
        memcpy(pop_data, a, sizeof(a));
        put32(pop_queue->bytes + 0x04, 1); /* completed SPU producer */
        cellSpursNotifyGuestWrite(0x6a00, 128);
        CHECK(cellSpursQueuePopBody(pop_queue, b, 0, 1) == 0);
        CHECK(!memcmp(b, a, sizeof(a)));
        CHECK(be32(pop_queue->bytes + 0x00) == 1);

        CHECK(_cellSpursLFQueueInitialize(ts, lf_queue, lf_data,
                                          16, 2, 2) == 0);
        CHECK(be32(lf_queue->bytes + 0x10) == 16);
        CHECK(be32(lf_queue->bytes + 0x14) == 2);
        CHECK(cellSpursQueuePushBody(push_queue, a, 0) == 0);
        CHECK(_cellSpursLFQueuePushBody(lf_queue, a, 1) == 0);
        CHECK(lf_queue->bytes[0x08] == 0 && lf_queue->bytes[0x09] == 1);
        CHECK(!memcmp(lf_data, a, sizeof(a)));
    }
    CHECK(cellSpursShutdownTaskset((CellSpursTaskset*)ts) == 0);
    CHECK(cellSpursJoinTaskset((CellSpursTaskset*)ts) == 0);
    CHECK(cellSpursFinalize(spurs) == 0);
    (void)priority;
    return 0;
}

static int test_job_chain(void)
{
    CellSpurs* spurs = (CellSpurs*)(guest + 0xa000);
    CellSpursAttribute* sa = (CellSpursAttribute*)(guest + 0xb000);
    CellSpursJobChainAttribute* ja = (CellSpursJobChainAttribute*)(guest + 0xb800);
    CellSpursJobChain* jc = (CellSpursJobChain*)(guest + 0xc000);
    uint64_t* commands = (uint64_t*)(guest + 0xd000);
    CellSpursJobGuard* guard = (CellSpursJobGuard*)(guest + 0xd080);
    uint8_t* descriptor = guest + 0xe000;
    uint8_t* binary = guest + 0xf000;
    uint8_t* inout = guest + 0x10000;
    uint8_t priority[8] = {1,1,1,1,1,1,1,1};
    memset(binary, 0x5a, 16);
    memset(descriptor, 0, 0x40);
    for (uint32_t i = 0; i < 16; ++i) inout[i] = (uint8_t)i;
    put64(descriptor, 0xf000);
    put16(descriptor + 8, 1);
    put16(descriptor + 0x0a, 8);
    put32(descriptor + 0x10, 1);  /* write back input/output DMA list */
    put32(descriptor + 0x14, 16);
    put64(descriptor + 0x30, ((uint64_t)16 << 32) | 0x10000);
    put64(commands + 0, 0x000000000000d003ull); /* circular empty NEXT */
    put64(commands + 1, 0x000000000000d08full); /* GUARD */
    put64(commands + 2, 0x000000000000d003ull); /* wrap to the head */
    spu_workload_reset();
    CHECK(spu_workload_register_direct(
              spu_workload_fingerprint(binary, 16), 16,
              synthetic_job, "synthetic-job"));
    {
        uint8_t unknown[16];
        spu_workload_image image;
        memset(unknown, 0xa5, sizeof(unknown));
        CHECK(!spu_workload_resolve(unknown, sizeof(unknown), &image));
    }
    CHECK(cellSpursAttributeInitialize(sa, 2, 100, 1000, 0) == 0);
    CHECK(cellSpursInitializeWithAttribute(spurs, sa) == 0);
    CHECK(_cellSpursJobChainAttributeInitialize(3, 0x475001, ja, commands,
                                                0x30, 1, priority, 1, 0,
                                                0, 0, 0, 0x100, 1) ==
          CELL_SPURS_JOB_ERROR_INVAL);
    CHECK(_cellSpursJobChainAttributeInitialize(3, 0x475001, ja, commands,
                                                0x40, 1, priority, 1, 0,
                                                0, 0, 0, 0x100, 1) == 0);
    CHECK(cellSpursCreateJobChainWithAttribute(spurs, jc, ja) == 0);
    CHECK(cellSpursJobGuardInitialize(jc, guard, 1, 1, 1) == 0);
    CHECK(cellSpursJobGuardNotify(guard) == 0);
    CHECK(be32(guard->bytes) == 0);
    CHECK(cellSpursJobGuardNotify(guard) == CELL_SPURS_JOB_ERROR_STAT);
    CHECK(cellSpursJobGuardReset(guard) == 0);
    CHECK(be32(guard->bytes) == 1);
    job_runs = 0;
    job_abi_ok = 0;
    CHECK(cellSpursRunJobChain(jc) == 0);
    put64(commands + 0, 0xe000);
    cellSpursNotifyPpuGuestWrite(0xd000, 8);
    CHECK(wait_value(&job_runs, 1));
    put64(commands + 0, 0x000000000000d003ull);
    cellSpursNotifyPpuGuestWrite(0xd000, 8);
    CHECK(cellSpursJobGuardNotify(guard) == 0);
    CHECK(wait_be32_value(guard->bytes, 1));
    put64(commands + 0, 0x0000000800000012ull);
    cellSpursNotifyPpuGuestWrite(0xd000, 8);
    CHECK(cellSpursShutdownJobChain(jc) == 0);
    CHECK(cellSpursJoinJobChain(jc) == 0);
    CHECK(job_runs == 1);
    CHECK(job_abi_ok);
    for (uint32_t i = 0; i < 16; ++i)
        CHECK(inout[i] == (uint8_t)(i ^ 0xff));
    CHECK(cellSpursFinalize(spurs) == 0);
    return 0;
}

static int test_ticket_mutex(void)
{
    CellSyncMutex* m = (CellSyncMutex*)(guest + 0x11000);
    CHECK(cellSyncMutexInitialize(m) == 0);
    CHECK(cellSyncMutexTryLock(m) == 0);
    CHECK(guest[0x11000] == 0 && guest[0x11001] == 0 &&
          guest[0x11002] == 0 && guest[0x11003] == 1);
    CHECK(cellSyncMutexTryLock(m) == CELL_SYNC_ERROR_BUSY);
    CHECK(cellSyncMutexUnlock(m) == 0);
    CHECK(guest[0x11000] == 0 && guest[0x11001] == 1 &&
          guest[0x11002] == 0 && guest[0x11003] == 1);
    return 0;
}

int main(void)
{
    if (test_layouts() || test_multiple_instances() ||
        test_task_event_queue() ||
        test_job_chain() || test_ticket_mutex()) return 1;
    puts("native_spurs_tests: PASS");
    return 0;
}
