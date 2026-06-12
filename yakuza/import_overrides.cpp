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

#include <cstdio>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" uint8_t* vm_base;

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
extern "C" void yz_ovr_sys_time_get_system_time(ppu_context* ctx)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    ctx->gpr[3] = (uint64_t)((c.QuadPart * 1000000) / f.QuadPart);
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
