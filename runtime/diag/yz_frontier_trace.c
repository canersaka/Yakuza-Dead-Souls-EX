#include "ps3emu/yz_frontier_trace.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <stdatomic.h>
#include <time.h>
#endif

#define YZ_FRONTIER_CAPACITY 65536u
#define YZ_FRONTIER_PATH_MAX 1024u

typedef struct yz_frontier_record {
    uint64_t sequence;  /* published last; zero means incomplete */
    uint64_t qpc;
    uint32_t type;
    uint32_t actor;
    uint32_t pc;
    uint32_t arg[6];
} yz_frontier_record;

typedef struct yz_frontier_file_header {
    char magic[8];
    uint32_t version;
    uint32_t record_size;
    uint32_t capacity;
    uint32_t reason;
    uint64_t write_count;
    uint64_t retained_begin;
    uint64_t qpc_frequency;
} yz_frontier_file_header;

static yz_frontier_record s_records[YZ_FRONTIER_CAPACITY];
static char s_dump_prefix[YZ_FRONTIER_PATH_MAX] = "scratch/frontier_ring";
static uint64_t s_qpc_frequency;

#ifdef _WIN32
static volatile LONG64 s_write_count;
static volatile LONG s_enabled;
static volatile LONG s_armed;
static volatile LONG s_dumped;

static uint64_t yz_frontier_now(void)
{
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return (uint64_t)value.QuadPart;
}

static uint64_t yz_frontier_next(void)
{
    return (uint64_t)InterlockedIncrement64(&s_write_count) - 1u;
}

static uint64_t yz_frontier_count(void)
{
    return (uint64_t)InterlockedCompareExchange64(&s_write_count, 0, 0);
}

static void yz_frontier_publish(volatile uint64_t* sequence, uint64_t value)
{
    InterlockedExchange64((volatile LONG64*)sequence, (LONG64)value);
}
#else
static _Atomic uint64_t s_write_count;
static _Atomic int s_enabled;
static _Atomic int s_armed;
static _Atomic int s_dumped;

static uint64_t yz_frontier_now(void)
{
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000000000ull + (uint64_t)value.tv_nsec;
}

static uint64_t yz_frontier_next(void)
{
    return atomic_fetch_add_explicit(&s_write_count, 1u, memory_order_relaxed);
}

static uint64_t yz_frontier_count(void)
{
    return atomic_load_explicit(&s_write_count, memory_order_acquire);
}

static void yz_frontier_publish(volatile uint64_t* sequence, uint64_t value)
{
    atomic_store_explicit((_Atomic uint64_t*)sequence, value,
                          memory_order_release);
}
#endif

static const char* yz_frontier_event_name(uint32_t type)
{
    switch (type) {
    case YZ_FT_ARM:              return "arm";
    case YZ_FT_PPU_SITE:         return "ppu-site";
    case YZ_FT_PPU_VALUE:        return "ppu-value";
    case YZ_FT_PPU_STORE:        return "ppu-store";
    case YZ_FT_SPU_JOB_SELECT:   return "spu-job-select";
    case YZ_FT_SPU_DMA_CMD:      return "spu-dma";
    case YZ_FT_SPU_OUT_MBOX:     return "spu-out-mbox";
    case YZ_FT_SPU_INTR_MBOX:    return "spu-intr-mbox";
    case YZ_FT_EVENT_WAIT:       return "event-wait";
    case YZ_FT_EVENT_SET:        return "event-set";
    case YZ_FT_EVENT_QUEUE:      return "event-queue";
    case YZ_FT_SPU_HALT:         return "spu-halt";
    case YZ_FT_RSX_STATE:        return "rsx-state";
    case YZ_FT_STALL:            return "stall";
    default:                     return "unknown";
    }
}

int yz_frontier_trace_init(void)
{
    const char* enabled = getenv("YZ_FRONTIER_RING");
    const char* path = getenv("YZ_FRONTIER_RING_PATH");
    if (!enabled || !*enabled || *enabled == '0')
        return 0;

    if (path && *path) {
        snprintf(s_dump_prefix, sizeof(s_dump_prefix), "%s", path);
        s_dump_prefix[sizeof(s_dump_prefix) - 1u] = '\0';
    }
#ifdef _WIN32
    {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        s_qpc_frequency = (uint64_t)frequency.QuadPart;
        InterlockedExchange(&s_enabled, 1);
    }
#else
    s_qpc_frequency = 1000000000ull;
    atomic_store_explicit(&s_enabled, 1, memory_order_release);
#endif
    fprintf(stderr,
            "[frontier-ring] configured dormant capacity=%u bytes=%zu\n",
            YZ_FRONTIER_CAPACITY, sizeof(s_records));
    fflush(stderr);
    return 1;
}

int yz_frontier_trace_enabled(void)
{
#ifdef _WIN32
    return InterlockedCompareExchange(&s_enabled, 0, 0) != 0;
#else
    return atomic_load_explicit(&s_enabled, memory_order_acquire) != 0;
#endif
}

int yz_frontier_trace_is_armed(void)
{
#ifdef _WIN32
    return InterlockedCompareExchange(&s_armed, 0, 0) != 0;
#else
    return atomic_load_explicit(&s_armed, memory_order_acquire) != 0;
#endif
}

void yz_frontier_trace_emit(uint32_t type, uint32_t actor, uint32_t pc,
                            uint32_t a0, uint32_t a1, uint32_t a2,
                            uint32_t a3, uint32_t a4, uint32_t a5)
{
    yz_frontier_record* record;
    uint64_t sequence;
    if (!yz_frontier_trace_is_armed())
        return;

    sequence = yz_frontier_next();
    record = &s_records[sequence & (YZ_FRONTIER_CAPACITY - 1u)];
    yz_frontier_publish(&record->sequence, 0);
    record->qpc = yz_frontier_now();
    record->type = type;
    record->actor = actor;
    record->pc = pc;
    record->arg[0] = a0;
    record->arg[1] = a1;
    record->arg[2] = a2;
    record->arg[3] = a3;
    record->arg[4] = a4;
    record->arg[5] = a5;
    yz_frontier_publish(&record->sequence, sequence + 1u);
}

void yz_frontier_trace_arm(uint32_t actor, uint32_t pc,
                           uint32_t source_ea, uint32_t ls,
                           uint32_t image, uint32_t descriptor)
{
    if (!yz_frontier_trace_enabled())
        return;
#ifdef _WIN32
    if (InterlockedCompareExchange(&s_armed, 1, 0) != 0)
        return;
#else
    {
        int expected = 0;
        if (!atomic_compare_exchange_strong_explicit(
                &s_armed, &expected, 1,
                memory_order_acq_rel, memory_order_acquire))
            return;
    }
#endif
    yz_frontier_trace_emit(YZ_FT_ARM, actor, pc, source_ea, ls,
                           image, descriptor, 0, 0);
}

int yz_frontier_trace_dump(uint32_t reason)
{
    yz_frontier_file_header header;
    char binary_path[YZ_FRONTIER_PATH_MAX + 8u];
    char text_path[YZ_FRONTIER_PATH_MAX + 8u];
    FILE* binary;
    FILE* text_file;
    uint64_t end;
    uint64_t begin;

#ifdef _WIN32
    if (InterlockedCompareExchange(&s_dumped, 1, 0) != 0)
        return 0;
    InterlockedExchange(&s_armed, 0);
#else
    {
        int expected = 0;
        if (!atomic_compare_exchange_strong_explicit(
                &s_dumped, &expected, 1,
                memory_order_acq_rel, memory_order_acquire))
            return 0;
        atomic_store_explicit(&s_armed, 0, memory_order_release);
    }
#endif

    end = yz_frontier_count();
    begin = end > YZ_FRONTIER_CAPACITY ? end - YZ_FRONTIER_CAPACITY : 0u;
    snprintf(binary_path, sizeof(binary_path), "%s.bin", s_dump_prefix);
    snprintf(text_path, sizeof(text_path), "%s.tsv", s_dump_prefix);

    memset(&header, 0, sizeof(header));
    memcpy(header.magic, "YZFTRC1", 7);
    header.version = 1;
    header.record_size = (uint32_t)sizeof(yz_frontier_record);
    header.capacity = YZ_FRONTIER_CAPACITY;
    header.reason = reason;
    header.write_count = end;
    header.retained_begin = begin;
    header.qpc_frequency = s_qpc_frequency;

    binary = fopen(binary_path, "wb");
    if (binary) {
        fwrite(&header, sizeof(header), 1u, binary);
        fwrite(s_records, sizeof(s_records), 1u, binary);
        fclose(binary);
    }

    text_file = fopen(text_path, "wb");
    if (text_file) {
        uint64_t sequence;
        fprintf(text_file,
                "sequence\tqpc\ttype\tname\tactor\tpc"
                "\ta0\ta1\ta2\ta3\ta4\ta5\n");
        for (sequence = begin; sequence < end; ++sequence) {
            const yz_frontier_record* record =
                &s_records[sequence & (YZ_FRONTIER_CAPACITY - 1u)];
            if (record->sequence != sequence + 1u)
                continue;
            fprintf(text_file,
                    "%" PRIu64 "\t%" PRIu64 "\t%u\t%s"
                    "\t%08" PRIX32 "\t%08" PRIX32
                    "\t%08" PRIX32 "\t%08" PRIX32
                    "\t%08" PRIX32 "\t%08" PRIX32
                    "\t%08" PRIX32 "\t%08" PRIX32 "\n",
                    sequence, record->qpc, record->type,
                    yz_frontier_event_name(record->type),
                    record->actor, record->pc,
                    record->arg[0], record->arg[1], record->arg[2],
                    record->arg[3], record->arg[4], record->arg[5]);
        }
        fclose(text_file);
    }

    fprintf(stderr,
            "[frontier-ring] dumped reason=%u records=%" PRIu64
            " retained=%" PRIu64 " binary=%s text=%s\n",
            reason, end, end - begin, binary_path, text_path);
    fflush(stderr);
    return 1;
}
