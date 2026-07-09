/*
 * ps3recomp - Memory management syscalls (implementation)
 */

#include "sys_memory.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * Globals
 * -----------------------------------------------------------------------*/
sys_mem_alloc_info       g_sys_mem_allocs[SYS_MEMORY_ALLOC_MAX];
sys_mem_container_info   g_sys_mem_containers[SYS_MEMORY_CONTAINER_MAX];
sys_mmapper_shared_info  g_sys_mmapper_shared[SYS_MMAPPER_SHARED_MAX];

/* Bump allocator for main memory pool.
 * Starts after a reasonable offset to avoid the low addresses used by ELF. */
uint32_t g_sys_mem_bump_ptr = 0;

/* Total user memory size (default 213 MB, after kernel reservation) */
#define SYS_MEM_USER_TOTAL  (213 * 1024 * 1024)

/* Guest window handed out by sys_memory_allocate / sys_mmapper.
 * 0x40000000+ matches where the real lv2 places these allocations (verified
 * against an RPCS3 boot log of Yakuza: Dead Souls); the window is outside
 * the pre-committed main region, so pages are committed on demand. */
#define SYS_MEM_ALLOC_BASE  0x40000000u
#define SYS_MEM_ALLOC_END   0x50000000u

static uint32_t s_total_allocated = 0;

/* ---------------------------------------------------------------------------
 * M1/M7 (2026-07-09): user-memory accounting.
 *
 * sys_memory_get_user_memory_size(352) reported total/avail computed only
 * from OUR OWN sys_memory_allocate bookkeeping (s_total_allocated); it never
 * billed the ELF's own footprint (segments + primary stack, charged by real
 * lv2 before the game's first instruction even runs -- RPCS3
 * Emu/Cell/PPUModule.cpp:2798) or sys_vm's psize (billed against the same
 * lv2_memory_container on real HW -- RPCS3 Emu/Cell/lv2/sys_vm.cpp:91,
 * `ct->take(psize)`). Measured against this exact title's RPCS3.log:
 *   line 61476: get_user_memory_size() -> Avail=0xBD20000, Total=0xD500000
 *     (i.e. 0x17E0000 already consumed before the first guest instruction --
 *     ELF segments + primary stack)
 *   line 61484-61486: sys_vm_memory_map(psize=0x7a50000) follows
 *   line 62755: get_user_memory_size() -> Avail=0x17fc000 (down sharply,
 *     consistent with psize + other allocations being billed against Total)
 * s_external_consumed is the charge ledger fed by sys_memory_account();
 * s_total_allocated already covers our own sys_memory_allocate() bookkeeping.
 *
 * Kill-switch YZ_NO_MEMACCT: this changes a value the game reads (Avail),
 * so it's gated for A/B safety -- when set, get_user_memory_size reports the
 * old numbers (ignores s_external_consumed and skips the ELF charge) and
 * sys_memory_allocate's ENOMEM ceiling reverts to address-window-only.
 * -----------------------------------------------------------------------*/
static uint64_t s_external_consumed = 0;

static int memacct_disabled(void)
{
    static int d = -1;
    if (d < 0) {
        d = getenv("YZ_NO_MEMACCT") ? 1 : 0;
        fprintf(stderr, "[sys_memory] memacct armed (user-memory accounting %s)\n",
                d ? "DISABLED by YZ_NO_MEMACCT" : "on");
        fflush(stderr);
    }
    return d;
}

void sys_memory_account(uint32_t bytes)
{
    if (memacct_disabled())
        return;
    s_external_consumed += bytes;
}

/* ELF footprint charge (M1), applied lazily the first time the game queries
 * get_user_memory_size. Documented constant rather than walking the loader's
 * segment table (yakuza/main.cpp) -- cites a direct measurement of this
 * exact title: real HW reports Avail=0xBD20000 of Total=0xD500000 before the
 * first guest instruction runs, i.e. 0xD500000-0xBD20000 = 0x17E0000 consumed
 * by ELF segments + primary stack (RPCS3.log:61476). */
#define SYS_MEM_ELF_FOOTPRINT  0x17E0000u
static int s_elf_charged = 0;

/* Guest threads are real host threads; serialize the bump allocator. */
#ifdef _WIN32
static SRWLOCK s_bump_lock = SRWLOCK_INIT;
static void bump_lock(void)   { AcquireSRWLockExclusive(&s_bump_lock); }
static void bump_unlock(void) { ReleaseSRWLockExclusive(&s_bump_lock); }
#else
#include <pthread.h>
static pthread_mutex_t s_bump_mtx = PTHREAD_MUTEX_INITIALIZER;
static void bump_lock(void)   { pthread_mutex_lock(&s_bump_mtx); }
static void bump_unlock(void) { pthread_mutex_unlock(&s_bump_mtx); }
#endif

static void write_be32(uint32_t addr, uint32_t val)
{
    uint32_t* p = (uint32_t*)vm_to_host(addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
    val = ((val >> 24) & 0xFF) | ((val >> 8) & 0xFF00) |
          ((val <<  8) & 0xFF0000) | ((val << 24) & 0xFF000000u);
#endif
    *p = val;
}

/* ---------------------------------------------------------------------------
 * sys_memory_allocate
 *
 * r3 = size
 * r4 = flags (page size)
 * r5 = pointer to receive allocated address (u32*)
 * -----------------------------------------------------------------------*/
int64_t sys_memory_allocate(ppu_context* ctx)
{
    uint32_t size      = LV2_ARG_U32(ctx, 0);
    uint32_t flags     = LV2_ARG_U32(ctx, 1);
    uint32_t addr_out  = LV2_ARG_PTR(ctx, 2);

    fprintf(stderr, "[sys_memory] allocate(size=0x%X, flags=0x%X)\n",
            size, flags);

    /* M5 fix (2026-07-09): flags must be exactly one of the two page-size
     * bits, or 0 (defaults to 1M, NOT 64K -- RPCS3 sys_memory.cpp:123-126:
     * `flags == SYS_MEMORY_PAGE_SIZE_1M ? 0x100000 : flags ==
     * SYS_MEMORY_PAGE_SIZE_64K ? 0x10000 : flags == 0 ? 0x100000 : 0`, and
     * `if (!align) return CELL_EINVAL`). */
    uint32_t alignment;
    if (flags == 0) {
        alignment = 0x100000; /* 1 MB (spec default) */
    } else if (flags == SYS_MEMORY_PAGE_SIZE_1M) {
        alignment = 0x100000; /* 1 MB */
    } else if (flags == SYS_MEMORY_PAGE_SIZE_64K) {
        alignment = 0x10000;  /* 64 KB */
    } else {
        return (int64_t)(int32_t)CELL_EINVAL;
    }

    size = VM_ALIGN_UP(size, alignment);

    if (size == 0)
        return (int64_t)(int32_t)CELL_EINVAL;

    bump_lock();

    /* Initialize bump pointer on first call */
    if (g_sys_mem_bump_ptr == 0)
        g_sys_mem_bump_ptr = SYS_MEM_ALLOC_BASE;

    /* Align bump pointer */
    g_sys_mem_bump_ptr = VM_ALIGN_UP(g_sys_mem_bump_ptr, alignment);

    /* Check if we have room in the address window */
    if (g_sys_mem_bump_ptr + size > SYS_MEM_ALLOC_END) {
        bump_unlock();
        return (int64_t)(int32_t)CELL_ENOMEM;
    }

    /* M1 fix: also enforce the real 213MB user-memory budget, not just the
     * 256MB bump-allocator address window (sys_memory.c:27-28 vs :21) --
     * YZ_NO_MEMACCT restores the old address-window-only ceiling for A/B. */
    if (!memacct_disabled()) {
        uint64_t projected = (uint64_t)s_total_allocated + s_external_consumed + (uint64_t)size;
        if (projected > (uint64_t)SYS_MEM_USER_TOTAL) {
            bump_unlock();
            return (int64_t)(int32_t)CELL_ENOMEM;
        }
    }

    /* Find a free allocation slot */
    int slot = -1;
    for (int i = 0; i < SYS_MEMORY_ALLOC_MAX; i++) {
        if (!g_sys_mem_allocs[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        bump_unlock();
        return (int64_t)(int32_t)CELL_ENOMEM;
    }

    uint32_t alloc_addr = g_sys_mem_bump_ptr;
    g_sys_mem_bump_ptr += size;
    s_total_allocated += size;

    sys_mem_alloc_info* a = &g_sys_mem_allocs[slot];
    a->active       = 1;       /* claim the slot before unlocking */
    a->addr         = alloc_addr;
    a->size         = size;
    a->container_id = 0;
    a->page_size    = alignment;  /* 0x100000 (1M) or 0x10000 (64K) */

    bump_unlock();

    /* Commit the pages (window is outside the pre-committed main region);
     * fresh commits are already zeroed by the OS */
    if (vm_commit(alloc_addr, size) != CELL_OK) {
        a->active = 0;
        return (int64_t)(int32_t)CELL_ENOMEM;
    }

    fprintf(stderr, "[sys_memory] allocate -> 0x%08X\n", alloc_addr);

    if (addr_out != 0) {
        write_be32(addr_out, alloc_addr);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_memory_free
 *
 * r3 = address
 * -----------------------------------------------------------------------*/
int64_t sys_memory_free(ppu_context* ctx)
{
    uint32_t addr = LV2_ARG_U32(ctx, 0);

    for (int i = 0; i < SYS_MEMORY_ALLOC_MAX; i++) {
        if (g_sys_mem_allocs[i].active && g_sys_mem_allocs[i].addr == addr) {
            s_total_allocated -= g_sys_mem_allocs[i].size;
            g_sys_mem_allocs[i].active = 0;
            return CELL_OK;
        }
    }

    return (int64_t)(int32_t)CELL_EINVAL;
}

/* ---------------------------------------------------------------------------
 * sys_memory_get_user_memory_size
 *
 * r3 = pointer to output struct: { u32 total, u32 available }
 * -----------------------------------------------------------------------*/
int64_t sys_memory_get_user_memory_size(ppu_context* ctx)
{
    uint32_t out_addr = LV2_ARG_PTR(ctx, 0);

    fprintf(stderr, "[sys_memory] get_user_memory_size()\n");

    /* M1: charge the ELF footprint exactly once, lazily, on first query
     * (see SYS_MEM_ELF_FOOTPRINT above). No-op when accounting is disabled. */
    if (!s_elf_charged) {
        s_elf_charged = 1;
        sys_memory_account(SYS_MEM_ELF_FOOTPRINT);
    }

    if (out_addr != 0) {
        uint32_t total = SYS_MEM_USER_TOTAL;
        /* M7: floor at 0 instead of underflowing a uint32 subtraction when
         * consumed > total (can happen transiently with padding/rounding). */
        uint64_t consumed = memacct_disabled()
            ? (uint64_t)s_total_allocated
            : (uint64_t)s_total_allocated + s_external_consumed;
        uint32_t avail = (consumed >= total) ? 0 : (uint32_t)(total - consumed);
        write_be32(out_addr + 0, total);
        write_be32(out_addr + 4, avail);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_memory_get_page_attribute (syscall 351 / 0x15F)
 *
 * r3 = address to query
 * r4 = pointer to sys_page_attr_t (rpcs3/rpcs3/Emu/Cell/lv2/sys_memory.h):
 *        +0x00 attribute    (u64)
 *        +0x08 access_right (u64)
 *        +0x10 page_size    (u32)  , the field titles actually check
 *        +0x14 pad          (u32)
 *
 * Was misnumbered 358 and unregistered, so it fell through to the return-0
 * stub without ever filling the output struct, callers reading page_size
 * got stack garbage. Values below mirror RPCS3's fast-path constants
 * (SYS_MEMORY_PROT_READ_WRITE / SYS_MEMORY_ACCESS_RIGHT_PPU_THR); page_size
 * reports the queried region's actual allocation granularity.
 * -----------------------------------------------------------------------*/
int64_t sys_memory_get_page_attribute(ppu_context* ctx)
{
    uint32_t addr     = LV2_ARG_U32(ctx, 0);
    uint32_t attr_out = LV2_ARG_PTR(ctx, 1);

    /* Report the page size of the allocation containing addr. */
    uint32_t page_size = 0x10000;  /* default 64K */
    for (int i = 0; i < SYS_MEMORY_ALLOC_MAX; i++) {
        if (g_sys_mem_allocs[i].active &&
            addr >= g_sys_mem_allocs[i].addr &&
            addr <  g_sys_mem_allocs[i].addr + g_sys_mem_allocs[i].size) {
            page_size = g_sys_mem_allocs[i].page_size ? g_sys_mem_allocs[i].page_size : 0x10000;
            break;
        }
    }

    if (attr_out != 0) {
        write_be32(attr_out + 0x00, 0);          /* attribute    (hi) */
        write_be32(attr_out + 0x04, 0x00040000); /* attribute    (lo) SYS_MEMORY_PROT_READ_WRITE */
        write_be32(attr_out + 0x08, 0);          /* access_right (hi) */
        write_be32(attr_out + 0x0C, 0x00000008); /* access_right (lo) SYS_MEMORY_ACCESS_RIGHT_PPU_THR */
        write_be32(attr_out + 0x10, page_size);  /* page_size */
        write_be32(attr_out + 0x14, 0);          /* pad */
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_memory_container_create
 *
 * r3 = pointer to receive container ID (u32*)
 * r4 = size
 * -----------------------------------------------------------------------*/
int64_t sys_memory_container_create(ppu_context* ctx)
{
    uint32_t id_out_addr = LV2_ARG_PTR(ctx, 0);
    uint32_t size        = LV2_ARG_U32(ctx, 1);

    int slot = -1;
    for (int i = 0; i < SYS_MEMORY_CONTAINER_MAX; i++) {
        if (!g_sys_mem_containers[i].active) { slot = i; break; }
    }
    if (slot < 0)
        return (int64_t)(int32_t)CELL_EAGAIN;

    sys_mem_container_info* c = &g_sys_mem_containers[slot];
    c->active     = 1;
    c->total_size = size;
    c->used_size  = 0;

    uint32_t container_id = (uint32_t)(slot + 1);
    if (id_out_addr != 0) {
        write_be32(id_out_addr, container_id);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_memory_container_destroy
 *
 * r3 = container_id
 * -----------------------------------------------------------------------*/
int64_t sys_memory_container_destroy(ppu_context* ctx)
{
    uint32_t container_id = LV2_ARG_U32(ctx, 0);

    if (container_id == 0 || container_id > SYS_MEMORY_CONTAINER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_mem_container_info* c = &g_sys_mem_containers[container_id - 1];
    if (!c->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    c->active = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_memory_container_get_size
 *
 * r3 = container_id
 * r4 = pointer to output struct: { u32 total, u32 used }
 * -----------------------------------------------------------------------*/
int64_t sys_memory_container_get_size(ppu_context* ctx)
{
    uint32_t container_id = LV2_ARG_U32(ctx, 0);
    uint32_t out_addr     = LV2_ARG_PTR(ctx, 1);

    if (container_id == 0 || container_id > SYS_MEMORY_CONTAINER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_mem_container_info* c = &g_sys_mem_containers[container_id - 1];
    if (!c->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    if (out_addr != 0) {
        write_be32(out_addr + 0, c->total_size);
        write_be32(out_addr + 4, c->used_size);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_mmapper_allocate_address
 *
 * r3 = size
 * r4 = flags
 * r5 = alignment
 * r6 = pointer to receive address (u32*)
 * -----------------------------------------------------------------------*/
int64_t sys_mmapper_allocate_address(ppu_context* ctx)
{
    uint32_t size      = LV2_ARG_U32(ctx, 0);
    /* uint32_t flags  = LV2_ARG_U32(ctx, 1); */
    uint32_t alignment = LV2_ARG_U32(ctx, 2);
    uint32_t addr_out  = LV2_ARG_PTR(ctx, 3);

    if (alignment == 0) alignment = 0x10000;
    size = VM_ALIGN_UP(size, alignment);

    bump_lock();

    if (g_sys_mem_bump_ptr == 0)
        g_sys_mem_bump_ptr = SYS_MEM_ALLOC_BASE;

    g_sys_mem_bump_ptr = VM_ALIGN_UP(g_sys_mem_bump_ptr, alignment);

    if (g_sys_mem_bump_ptr + size > SYS_MEM_ALLOC_END) {
        bump_unlock();
        return (int64_t)(int32_t)CELL_ENOMEM;
    }

    uint32_t alloc_addr = g_sys_mem_bump_ptr;
    g_sys_mem_bump_ptr += size;

    bump_unlock();

    /* Commit the reserved region */
    vm_commit(alloc_addr, size);

    if (addr_out != 0) {
        write_be32(addr_out, alloc_addr);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_mmapper_free_address
 *
 * r3 = address
 * -----------------------------------------------------------------------*/
int64_t sys_mmapper_free_address(ppu_context* ctx)
{
    /* uint32_t addr = LV2_ARG_U32(ctx, 0); */
    (void)ctx;
    /* In our bump allocator we don't actually free, just acknowledge */
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_mmapper_allocate_shared_memory
 *
 * r3 = key
 * r4 = size
 * r5 = flags
 * r6 = pointer to receive shared mem ID (u32*)
 * -----------------------------------------------------------------------*/
int64_t sys_mmapper_allocate_shared_memory(ppu_context* ctx)
{
    uint64_t key      = LV2_ARG_U64(ctx, 0);
    uint32_t size     = LV2_ARG_U32(ctx, 1);
    /* uint32_t flags = LV2_ARG_U32(ctx, 2); */
    uint32_t id_out   = LV2_ARG_PTR(ctx, 3);

    int slot = -1;
    for (int i = 0; i < SYS_MMAPPER_SHARED_MAX; i++) {
        if (!g_sys_mmapper_shared[i].active) { slot = i; break; }
    }
    if (slot < 0)
        return (int64_t)(int32_t)CELL_EAGAIN;

    sys_mmapper_shared_info* s = &g_sys_mmapper_shared[slot];
    s->active = 1;
    s->size   = VM_ALIGN_UP(size, VM_PAGE_SIZE);
    s->key    = key;
    s->addr   = 0;

    uint32_t shm_id = (uint32_t)(slot + 1);
    if (id_out != 0) {
        write_be32(id_out, shm_id);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_mmapper_map_shared_memory
 *
 * r3 = address (where to map)
 * r4 = shared mem ID
 * r5 = flags
 * -----------------------------------------------------------------------*/
int64_t sys_mmapper_map_shared_memory(ppu_context* ctx)
{
    uint32_t addr   = LV2_ARG_U32(ctx, 0);
    uint32_t shm_id = LV2_ARG_U32(ctx, 1);
    /* uint32_t flags = LV2_ARG_U32(ctx, 2); */

    if (shm_id == 0 || shm_id > SYS_MMAPPER_SHARED_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_mmapper_shared_info* s = &g_sys_mmapper_shared[shm_id - 1];
    if (!s->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    /* Commit the memory at the specified address */
    int32_t rc = vm_commit(addr, s->size);
    if (rc != CELL_OK)
        return (int64_t)(int32_t)rc;

    s->addr = addr;
    memset(vm_to_host(addr), 0, s->size);

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Registration
 * -----------------------------------------------------------------------*/
void sys_memory_init(lv2_syscall_table* tbl)
{
    memset(g_sys_mem_allocs,     0, sizeof(g_sys_mem_allocs));
    memset(g_sys_mem_containers, 0, sizeof(g_sys_mem_containers));
    memset(g_sys_mmapper_shared, 0, sizeof(g_sys_mmapper_shared));
    g_sys_mem_bump_ptr = 0;
    s_total_allocated  = 0;

    /* Commit the whole window up front. On real hardware the user lpar is
     * mapped beyond the game's current allocations, and Sony's own code
     * relies on it: the SPURS kernel's boot code GETs a resume-context
     * probe from spurs+0xD0000 — memory the game only sys_memory-allocates
     * a few milliseconds later (a benign race on real HW; the kernel
     * validates the contents and treats garbage as a cold boot). Host
     * pages still materialize lazily on first touch. */
    vm_commit(SYS_MEM_ALLOC_BASE, SYS_MEM_ALLOC_END - SYS_MEM_ALLOC_BASE);

    lv2_syscall_register(tbl, SYS_MEMORY_ALLOCATE,             sys_memory_allocate);
    lv2_syscall_register(tbl, SYS_MEMORY_FREE,                 sys_memory_free);
    lv2_syscall_register(tbl, SYS_MEMORY_GET_USER_MEMORY_SIZE, sys_memory_get_user_memory_size);
    lv2_syscall_register(tbl, SYS_MEMORY_GET_PAGE_ATTRIBUTE,   sys_memory_get_page_attribute);
    lv2_syscall_register(tbl, SYS_MEMORY_CONTAINER_CREATE,     sys_memory_container_create);
    lv2_syscall_register(tbl, SYS_MEMORY_CONTAINER_DESTROY,    sys_memory_container_destroy);
    lv2_syscall_register(tbl, SYS_MEMORY_CONTAINER_GET_SIZE,   sys_memory_container_get_size);
    lv2_syscall_register(tbl, SYS_MMAPPER_ALLOCATE_ADDRESS,    sys_mmapper_allocate_address);
    lv2_syscall_register(tbl, SYS_MMAPPER_FREE_ADDRESS,        sys_mmapper_free_address);
    lv2_syscall_register(tbl, SYS_MMAPPER_ALLOCATE_SHARED_MEMORY, sys_mmapper_allocate_shared_memory);
    lv2_syscall_register(tbl, SYS_MMAPPER_MAP_SHARED_MEMORY,  sys_mmapper_map_shared_memory);
}
