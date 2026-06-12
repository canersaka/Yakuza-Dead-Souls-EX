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

#include "ps3emu/error_codes.h"

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
 * gcm initialization (_cellGcmInitBody and friends)
 *
 * The game's gcm code is SDK-inline: it walks CellGcmContextData in GUEST
 * memory itself (layout verified from the EBOOT: begin@+0, end@+4,
 * current@+8, callback@+0xC) and writes RSX commands through ctx->current.
 * So init must build guest-visible structures; the libs host-side state
 * (offset table, io mappings, flip machinery) is kept in sync by calling
 * the libs cellGcmInit. Semantics cross-checked against RPCS3
 * _cellGcmInitBody (Emu/Cell/Modules/cellGcmSys.cpp) and the real boot's
 * libgcm trace in RPCS3.log -- reimplemented, not copied.
 * -----------------------------------------------------------------------*/
extern "C" int32_t cellGcmInit(uint32_t cmdSize, uint32_t ioSize, uint32_t ioAddress);
extern "C" int32_t cellGcmIoOffsetToAddress(uint32_t ioOffset, uint32_t* ea);
extern "C" int32_t cellGcmAddressToOffset(uint32_t address, uint32_t* offset);
extern "C" int32_t cellGcmGetTiledPitchSize(uint32_t size, uint32_t* pitch);
extern "C" uint32_t cellGcmGetTimeStampLocation(uint32_t index, uint32_t* location);

static uint32_t yz_gcm_io_addr;
static uint32_t yz_gcm_io_size;

/* ---- Minimal RSX fifo consumer ("mini-RSX") --------------------------------
 * The game's SDK-inline flush/finish writes ctrl->put and spins until
 * ctrl->get (and for SetReference waits, ctrl->ref) catch up — on real
 * hardware the RSX advances them. This host thread walks the command
 * stream from get to put: follows jumps, skips methods by their count
 * field, and executes SET_REFERENCE (NV406E method 0x050; the EBOOT's own
 * inline SetReference writes header 0x00040050) by storing the operand to
 * ctrl->ref. No other method does anything yet — that is the D3D12 tier-1
 * wiring, later. */
/* Execute one method register write. Returns 0 normally, 1 if the fifo
 * must stall (semaphore acquire not yet satisfied).
 * Semaphore semantics mirror RPCS3 (emu/RSX/NV47/HW/nv4097.cpp,
 * rsx_methods.cpp, RSXThread.cpp get_address): each class has a context-DMA
 * selector + offset register; the DMA selects where the semaphore lives:
 *   0x66606660/0x66616661 -> label area (what cellGcmGetLabelAddress returns)
 *   0xFEED0001            -> main memory via the io map (offset = io offset)
 *   0xFEED0000            -> RSX local memory
 * back_end release swizzles the value (ARGB shuffle); the others are raw. */
static uint32_t yz_rsx_sem_dma_406e = 0x66616661;  /* RPCS3 reset values */
static uint32_t yz_rsx_sem_dma_4097 = 0x66606660;
static uint32_t yz_rsx_sem_off_406e;
static uint32_t yz_rsx_sem_off_4097;

static uint32_t yz_rsx_sem_addr(uint32_t dma, uint32_t offset)
{
    switch (dma) {
    case 0x66606660u:
    case 0x66616661u:
        return YZ_GCM_LABELS_ADDR + (offset & 0xFFCu);
    case 0xFEED0001u: {                           /* main memory via io map */
        uint32_t ea = 0;
        if (cellGcmIoOffsetToAddress(offset, &ea) == 0 && ea)
            return ea;
        return 0;
    }
    case 0xFEED0000u:                             /* RSX local memory */
        return (offset < YZ_GCM_LOCAL_SIZE) ? YZ_GCM_LOCAL_BASE + offset : 0;
    default: {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[rsx] unknown semaphore context dma 0x%08X\n", dma);
        }
        return 0;
    }
    }
}

/* NV3062 (2D surface) + NV308A (image-from-cpu) state: the SDK's
 * cellGcmInlineTransfer writes data words into memory through the 2D blit
 * engine — Yakuza uses it to publish its flip/vsync counters to a spot in
 * io memory that the PPU then polls. Semantics per RPCS3 nv308a.cpp:
 * A8R8G8B8/Y32 is a raw word copy to dst_offset + x*4 + y*pitch; operands
 * with index >= SIZE_OUT.x are skipped. */
static uint32_t yz_rsx_blit_dst_dma = 0xFEED0000;
static uint32_t yz_rsx_blit_dst_off;
static uint32_t yz_rsx_blit_pitch   = 64;
static uint32_t yz_rsx_blit_fmt     = 0xB;     /* a8r8g8b8 */
static uint32_t yz_rsx_blit_point;
static uint32_t yz_rsx_blit_size_out = 0x00010001;

static int yz_rsx_method(uint32_t method, uint32_t arg)
{
    if (method >= 0xA400 && method < 0xAB00) {     /* NV308A_COLOR window */
        uint32_t index = (method - 0xA400) >> 2;
        uint32_t out_x = yz_rsx_blit_size_out & 0xFFFFu;
        if (index >= out_x)
            return 0;                              /* clipped: skip */
        if (yz_rsx_blit_fmt != 0xB && yz_rsx_blit_fmt != 0x8 /* y32 */) {
            static int warned = 0;
            if (!warned) {
                warned = 1;
                fprintf(stderr, "[rsx] NV308A color format 0x%X unsupported\n",
                        yz_rsx_blit_fmt);
            }
            return 0;
        }
        uint32_t x = (yz_rsx_blit_point & 0xFFFFu) + index;
        uint32_t y = yz_rsx_blit_point >> 16;
        uint32_t pitch = yz_rsx_blit_pitch & 0xFFFFu;
        uint32_t addr = yz_rsx_sem_addr(yz_rsx_blit_dst_dma,
                                        yz_rsx_blit_dst_off + x * 4 + y * pitch);
        if (addr)
            vm_write32(addr, arg);
        return 0;
    }

    uint32_t addr;
    switch (method) {
    case 0x6184:                                  /* NV3062 DMA_IMAGE_SOURCE */
        break;
    case 0x6188:                                  /* NV3062 DMA_IMAGE_DESTIN */
        yz_rsx_blit_dst_dma = arg;
        break;
    case 0x6300:                                  /* NV3062 SET_COLOR_FORMAT */
        yz_rsx_blit_fmt = arg & 0xFFFFu;
        break;
    case 0x6304:                                  /* NV3062 SET_PITCH (src<<16|dst) */
        yz_rsx_blit_pitch = arg;
        break;
    case 0x630C:                                  /* NV3062 SET_OFFSET_DESTIN */
        yz_rsx_blit_dst_off = arg;
        break;
    case 0xA304:                                  /* NV308A_POINT (y<<16|x) */
        yz_rsx_blit_point = arg;
        break;
    case 0xA308:                                  /* NV308A_SIZE_OUT */
        yz_rsx_blit_size_out = arg;
        break;
    }
    switch (method) {
    case 0x050:                                   /* NV406E SET_REFERENCE */
        vm_write32(YZ_GCM_CTRL_ADDR + 8, arg);
        break;
    case 0x060:                                   /* NV406E SET_CONTEXT_DMA_SEMAPHORE */
        yz_rsx_sem_dma_406e = arg;
        break;
    case 0x064:                                   /* NV406E SEMAPHORE_OFFSET */
        yz_rsx_sem_off_406e = arg;
        break;
    case 0x068:                                   /* NV406E SEMAPHORE_ACQUIRE */
        addr = yz_rsx_sem_addr(yz_rsx_sem_dma_406e, yz_rsx_sem_off_406e);
        if (addr && vm_read32(addr) != arg)
            return 1;                             /* stall, retry later */
        break;
    case 0x06C:                                   /* NV406E SEMAPHORE_RELEASE */
        addr = yz_rsx_sem_addr(yz_rsx_sem_dma_406e, yz_rsx_sem_off_406e);
        if (addr)
            vm_write32(addr, arg);
        break;
    case 0x1A4:                                   /* NV4097 SET_CONTEXT_DMA_SEMAPHORE */
        yz_rsx_sem_dma_4097 = arg;
        break;
    case 0x1D6C:                                  /* NV4097 SET_SEMAPHORE_OFFSET */
        yz_rsx_sem_off_4097 = arg;
        break;
    case 0x1D70:                                  /* BACK_END_WRITE_SEMAPHORE_RELEASE */
        addr = yz_rsx_sem_addr(yz_rsx_sem_dma_4097, yz_rsx_sem_off_4097);
        if (addr)
            vm_write32(addr, (arg & 0xFF00FF00u) |
                             ((arg & 0xFFu) << 16) | ((arg >> 16) & 0xFFu));
        break;
    case 0x1D74:                                  /* TEXTURE_READ_SEMAPHORE_RELEASE */
        addr = yz_rsx_sem_addr(yz_rsx_sem_dma_4097, yz_rsx_sem_off_4097);
        if (addr)
            vm_write32(addr, arg);
        break;
    default:
        break;                                    /* rendering methods: later */
    }
    return 0;
}

static DWORD WINAPI yz_rsx_consumer(LPVOID)
{
    static int warned_offmap = 0, warned_badret = 0;
    uint32_t call_return = ~0u;   /* RSX has a single-level call stack */
    for (;;) {
        uint32_t put = vm_read32(YZ_GCM_CTRL_ADDR + 0);
        uint32_t get = vm_read32(YZ_GCM_CTRL_ADDR + 4);
        int budget = 1 << 20;   /* bail out of garbage/loops, resync below */
        while (get != put && budget-- > 0) {
            uint32_t ea = 0;
            if (cellGcmIoOffsetToAddress(get, &ea) != 0 || ea == 0) {
                if (!warned_offmap) {
                    warned_offmap = 1;
                    fprintf(stderr, "[rsx] get=0x%08X not io-mapped; resync to put\n", get);
                }
                get = put;
                break;
            }
            uint32_t cmd = vm_read32(ea);
            if ((cmd & 0xE0000003u) == 0x20000000u) {       /* old jump */
                uint32_t tgt = cmd & 0x1FFFFFFCu;
                if (tgt == get) break;   /* self-jump: idle until overwritten */
                get = tgt;
                continue;
            }
            if ((cmd & 3u) == 1u) {                          /* new jump */
                uint32_t tgt = cmd & 0xFFFFFFFCu;
                if (tgt == get) break;
                get = tgt;
                continue;
            }
            if ((cmd & 3u) == 2u) {                          /* call */
                call_return = get + 4;
                get = cmd & 0xFFFFFFFCu;
                continue;
            }
            if (cmd == 0x00020000u) {                        /* return */
                if (call_return != ~0u) {
                    get = call_return;
                    call_return = ~0u;
                    continue;
                }
                if (!warned_badret) {
                    warned_badret = 1;
                    fprintf(stderr, "[rsx] RETURN without CALL at get=0x%08X\n", get);
                }
                get = put;
                break;
            }
            uint32_t count  = (cmd >> 18) & 0x7FF;
            uint32_t noninc = cmd & 0x40000000u;
            uint32_t method = cmd & 0x3FFFCu;
            int stalled = 0;
            for (uint32_t i = 0; i < count && !stalled; i++) {
                uint32_t op_ea = 0;
                if (cellGcmIoOffsetToAddress(get + 4 + i * 4, &op_ea) != 0 || !op_ea)
                    break;
                stalled = yz_rsx_method(noninc ? method : method + i * 4,
                                        vm_read32(op_ea));
            }
            if (stalled)
                break;   /* leave get at this packet; re-poll the semaphore */
            get += 4 + count * 4;
        }
        vm_write32(YZ_GCM_CTRL_ADDR + 4, get);
        Sleep(1);
    }
    return 0;
}

/* Fifo-overflow callback: the game calls ctx->callback(ctx, count) when
 * current+4 > end and retries while it returns 0. Wrap the buffer: flush
 * what's pending, let the consumer drain it (so the rewind can't overwrite
 * unconsumed commands), then append a jump-to-begin and rewind. */
extern "C" void yz_gcm_fifo_callback(ppu_context* ctx)
{
    uint32_t gctx = (uint32_t)ctx->gpr[3];
    if (gctx) {
        uint32_t begin = vm_read32(gctx);
        uint32_t cur   = vm_read32(gctx + 8);
        uint32_t begin_off = begin - yz_gcm_io_addr;
        uint32_t cur_off   = cur - yz_gcm_io_addr;
        vm_write32(YZ_GCM_CTRL_ADDR, cur_off);             /* flush pending */
        for (int spin = 0; spin < 1000; spin++) {          /* drain (<=1s) */
            if (vm_read32(YZ_GCM_CTRL_ADDR + 4) == cur_off)
                break;
            Sleep(1);
        }
        vm_write32(cur, 0x20000000u | begin_off);          /* jump to begin */
        vm_write32(gctx + 8, begin);                       /* current = begin */
        vm_write32(YZ_GCM_CTRL_ADDR, begin_off);           /* put = begin */
    }
    ctx->gpr[3] = 0;
}

extern "C" void yz_ovr__cellGcmInitBody(ppu_context* ctx)
{
    uint32_t ctx_slot = (uint32_t)ctx->gpr[3];   /* CellGcmContextData** */
    uint32_t cmd_size = (uint32_t)ctx->gpr[4];
    uint32_t io_size  = (uint32_t)ctx->gpr[5];
    uint32_t io_addr  = (uint32_t)ctx->gpr[6];

    printf("[gcm] _cellGcmInitBody(ctx**=0x%08X, cmdSize=0x%X, ioSize=0x%X, "
           "ioAddr=0x%08X)\n", ctx_slot, cmd_size, io_size, io_addr);

    /* Host-side state: offset table, io mapping, config, flip machinery. */
    cellGcmInit(cmd_size, io_size, io_addr);

    /* RSX local memory: reserved by vm_init, commit it now (the game may
     * address it via config.localAddress / cellGcmAddressToOffset). */
    VirtualAlloc(vm_base + YZ_GCM_LOCAL_BASE, YZ_GCM_LOCAL_SIZE,
                 MEM_COMMIT, PAGE_READWRITE);

    yz_gcm_io_addr = io_addr;
    yz_gcm_io_size = io_size;

    /* Synthetic OPD for the fifo callback (code word = runner fake key). */
    vm_write32(YZ_GCM_CB_OPD_ADDR,     YZ_GCM_CB_FAKE_KEY);
    vm_write32(YZ_GCM_CB_OPD_ADDR + 4, 0);

    /* Guest context over the game's io buffer. First 4 KB reserved (matches
     * libgcm); use the WHOLE default command buffer as one segment (no RSX
     * consumer yet, so no need for libgcm's 32 KB fragment rotation). */
    uint32_t begin = io_addr + 0x1000;
    uint32_t end   = io_addr + (cmd_size ? cmd_size : 0x10000) - 4;
    vm_write32(YZ_GCM_CTX_ADDR + 0x0, begin);
    vm_write32(YZ_GCM_CTX_ADDR + 0x4, end);
    vm_write32(YZ_GCM_CTX_ADDR + 0x8, begin);
    vm_write32(YZ_GCM_CTX_ADDR + 0xC, YZ_GCM_CB_OPD_ADDR);

    /* Control register block (put/get/ref). */
    vm_write32(YZ_GCM_CTRL_ADDR + 0, 0);
    vm_write32(YZ_GCM_CTRL_ADDR + 4, 0);
    vm_write32(YZ_GCM_CTRL_ADDR + 8, 0);

    /* Label area returned by cellGcmGetLabelAddress (16 B stride). */
    memset(vm_base + YZ_GCM_LABELS_ADDR, 0, 256 * 16);

    /* The reserved first 4 KB of io space starts with a jump into the
     * buffer, so the consumer can start from get=0. */
    vm_write32(io_addr, 0x20000000u | 0x1000u);

    static int rsx_started = 0;
    if (!rsx_started) {
        rsx_started = 1;
        CreateThread(NULL, 0, yz_rsx_consumer, NULL, 0, NULL);
    }

    /* Hand the context to the game. */
    vm_write32(ctx_slot, YZ_GCM_CTX_ADDR);
    ctx->gpr[3] = 0;
}

/* BE out-param marshals for the gcm helpers the fifo path depends on
 * (the game's inline flush does AddressToOffset(current) -> ctrl->put;
 * a host-endian result is a byte-swapped put). */
extern "C" void yz_ovr_cellGcmAddressToOffset(ppu_context* ctx)
{
    uint32_t off = 0;
    int32_t  rc  = cellGcmAddressToOffset((uint32_t)ctx->gpr[3], &off);
    if (rc == 0 && ctx->gpr[4])
        vm_write32(ctx->gpr[4], off);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

extern "C" void yz_ovr_cellGcmGetTiledPitchSize(ppu_context* ctx)
{
    uint32_t pitch = 0;
    int32_t  rc    = cellGcmGetTiledPitchSize((uint32_t)ctx->gpr[3], &pitch);
    if (rc == 0 && ctx->gpr[4])
        vm_write32(ctx->gpr[4], pitch);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

extern "C" void yz_ovr_cellGcmGetTimeStampLocation(ppu_context* ctx)
{
    uint32_t loc = 0;
    uint32_t rc  = cellGcmGetTimeStampLocation((uint32_t)ctx->gpr[3], &loc);
    if (ctx->gpr[4])
        vm_write32(ctx->gpr[4], loc);
    ctx->gpr[3] = rc;
}

/* Returns a pointer the game reads/polls -> must be a guest address
 * (libs returns a host static; same class as GetControlRegister). */
extern "C" void yz_ovr_cellGcmGetLabelAddress(ppu_context* ctx)
{
    ctx->gpr[3] = YZ_GCM_LABELS_ADDR + ((uint32_t)ctx->gpr[3] & 0xFF) * 0x10;
}

/* SDK-internal flip entry points take the gcm CONTEXT as the first arg
 * (RPCS3: _cellGcmSetFlipCommand(ctx, id)); the libs single-arg versions
 * would receive the context pointer as the buffer id and silently fail,
 * leaving flip status WAITING forever. */
extern "C" int32_t cellGcmSetFlipCommand(uint32_t bufferId);
extern "C" int32_t cellGcmSetFlipCommandWithWaitLabel(uint32_t bufferId,
                                                      uint32_t labelIndex,
                                                      uint32_t labelValue);

extern "C" void yz_ovr__cellGcmSetFlipCommand(ppu_context* ctx)
{
    ctx->gpr[3] = (uint64_t)(int64_t)
        cellGcmSetFlipCommand((uint32_t)ctx->gpr[4]);
}

extern "C" void yz_ovr__cellGcmSetFlipCommandWithWaitLabel(ppu_context* ctx)
{
    ctx->gpr[3] = (uint64_t)(int64_t)
        cellGcmSetFlipCommandWithWaitLabel((uint32_t)ctx->gpr[4],
                                           (uint32_t)ctx->gpr[5],
                                           (uint32_t)ctx->gpr[6]);
}

/* The game polls put/get/ref through this pointer (SDK-inline flush/finish
 * write it directly), so it must be a GUEST address -- the libs version
 * returns a host static, which the pointer-return bridge mangles. */
extern "C" void yz_ovr_cellGcmGetControlRegister(ppu_context* ctx)
{
    ctx->gpr[3] = YZ_GCM_CTRL_ADDR;
}

/* Guest struct fields are big-endian; the libs version memcpy's host-endian
 * (same class as cellSysutilGetSystemParamInt). Field order per SDK:
 * localAddress, ioAddress, localSize, ioSize, memoryFrequency, coreFrequency. */
extern "C" void yz_ovr_cellGcmGetConfiguration(ppu_context* ctx)
{
    uint32_t cfg = (uint32_t)ctx->gpr[3];
    if (!cfg) { ctx->gpr[3] = (uint64_t)(int64_t)-1; return; }
    vm_write32(cfg + 0x00, YZ_GCM_LOCAL_BASE);
    vm_write32(cfg + 0x04, yz_gcm_io_addr);
    vm_write32(cfg + 0x08, YZ_GCM_LOCAL_SIZE);
    vm_write32(cfg + 0x0C, yz_gcm_io_size);
    vm_write32(cfg + 0x10, 650000000);
    vm_write32(cfg + 0x14, 500000000);
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

/* ---------------------------------------------------------------------------
 * sys_spu_image_import(img*, src, type) -- user-level SPU ELF loader.
 *
 * Sony's libsre (LLE SPURS) calls this to import its embedded SPURS-kernel
 * SPU ELFs (ELF32 BE, EM_SPU; verified at image vaddrs 0x20380/0x20C00,
 * entries 0x818/0x848). Semantics from RPCS3 sys_spu_.cpp
 * sys_spu_image_import (DIRECT path) + sys_spu.h get_nsegs/fill:
 *   - per PT_LOAD: COPY segment {ls=p_vaddr, size=p_filesz, addr=src+p_offset}
 *     plus FILL {ls=p_vaddr+p_filesz, size=p_memsz-p_filesz, value=0} for bss;
 *   - per PT_NOTE (p_type 4): INFO segment {size=0x20, addr=src+p_offset+0x14};
 *   - any other p_type: ENOEXEC.
 * Guest structs (BE): sys_spu_image {type@0=USER, entry@4, segs@8, nsegs@12},
 * sys_spu_segment 0x18 bytes {type@0, ls@4, size@8, addr/value@0x10}.
 * The segment table is consumed at thread-group start when the kernel image
 * is deployed to SPU local store (7d).
 * -----------------------------------------------------------------------*/
extern "C" void yz_ovr_sys_spu_image_import(ppu_context* ctx)
{
    uint32_t img_ea = (uint32_t)ctx->gpr[3];
    uint32_t src    = (uint32_t)ctx->gpr[4];
    uint32_t type   = (uint32_t)ctx->gpr[5];

    const uint8_t* e = vm_base + src;
    if (!img_ea || !src || memcmp(e, "\x7f""ELF", 4) != 0 ||
        e[4] != 1 /*ELF32*/ || e[5] != 2 /*BE*/ ||
        ((e[18] << 8) | e[19]) != 23 /*EM_SPU*/) {
        fprintf(stderr, "[SPU] image_import img=0x%08X src=0x%08X type=%u: "
                "not an ELF32-BE EM_SPU image -> ENOEXEC\n", img_ea, src, type);
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ENOEXEC;
        return;
    }

    uint32_t entry     = vm_read32(src + 0x18);
    uint32_t phoff     = vm_read32(src + 0x1C);
    uint32_t phentsize = (uint32_t)((e[0x2A] << 8) | e[0x2B]);
    uint32_t phnum     = (uint32_t)((e[0x2C] << 8) | e[0x2D]);
    if (!phnum || phentsize < 32) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ENOEXEC;
        return;
    }

    /* Count segments (oracle: get_nsegs) */
    int32_t nsegs = 0;
    for (uint32_t i = 0; i < phnum; i++) {
        uint32_t ph      = src + phoff + i * phentsize;
        uint32_t p_type  = vm_read32(ph + 0x00);
        uint32_t p_filesz= vm_read32(ph + 0x10);
        uint32_t p_memsz = vm_read32(ph + 0x14);
        if (p_type != 1 && p_type != 4) {
            ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ENOEXEC;
            return;
        }
        if (p_type == 1 && p_memsz != p_filesz && p_filesz) nsegs += 2;
        else nsegs += 1;
    }

    uint32_t segs_ea = yz_heap_alloc((uint32_t)nsegs * 0x18u, 16);
    if (!segs_ea) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ENOMEM;
        return;
    }

    /* Fill segments (oracle: sys_spu_image::fill) */
    uint32_t s = segs_ea;
    for (uint32_t i = 0; i < phnum; i++) {
        uint32_t ph       = src + phoff + i * phentsize;
        uint32_t p_type   = vm_read32(ph + 0x00);
        uint32_t p_offset = vm_read32(ph + 0x04);
        uint32_t p_vaddr  = vm_read32(ph + 0x08);
        uint32_t p_filesz = vm_read32(ph + 0x10);
        uint32_t p_memsz  = vm_read32(ph + 0x14);
        if (p_type == 1) {
            if (p_filesz) {
                vm_write32(s + 0x00, 1);                 /* COPY */
                vm_write32(s + 0x04, p_vaddr);
                vm_write32(s + 0x08, p_filesz);
                vm_write32(s + 0x10, src + p_offset);
                s += 0x18;
            }
            if (p_memsz > p_filesz) {
                vm_write32(s + 0x00, 2);                 /* FILL */
                vm_write32(s + 0x04, p_vaddr + p_filesz);
                vm_write32(s + 0x08, p_memsz - p_filesz);
                vm_write32(s + 0x10, 0);
                s += 0x18;
            }
        } else { /* p_type == 4 */
            vm_write32(s + 0x00, 4);                     /* INFO */
            vm_write32(s + 0x04, 0);
            vm_write32(s + 0x08, 0x20);
            vm_write32(s + 0x10, src + p_offset + 0x14);
            s += 0x18;
        }
    }

    vm_write32(img_ea + 0x0, 0);                  /* SYS_SPU_IMAGE_TYPE_USER */
    vm_write32(img_ea + 0x4, entry);
    vm_write32(img_ea + 0x8, segs_ea);
    vm_write32(img_ea + 0xC, (uint32_t)nsegs);

    fprintf(stderr, "[SPU] image_import img=0x%08X src=0x%08X type=%u "
            "-> entry=0x%X nsegs=%d segs=0x%08X\n",
            img_ea, src, type, entry, nsegs, segs_ea);
    ctx->gpr[3] = 0;
}

/* sys_spu_image_close: our USER images keep their segment tables in the
 * runner bump heap (never reclaimed), so close is success/no-op. */
extern "C" void yz_ovr_sys_spu_image_close(ppu_context* ctx)
{
    fprintf(stderr, "[SPU] image_close img=0x%08X\n", (uint32_t)ctx->gpr[3]);
    ctx->gpr[3] = 0;
}

/* ---------------------------------------------------------------------------
 * Guest-aware printf family.
 *
 * The generic bridges pass vararg slots raw, so a guest %s pointer reaches
 * host vprintf and is dereferenced as a host address (observed live: Sony''s
 * libsre printed a warning with %s -> fault on guest 0x0202xxxx). This
 * formatter walks the format string itself and translates %s/%p arguments.
 *
 * Vararg slots per the PPC64 ELF ABI: integer args r3..r10, then the
 * caller''s parameter save area at r1+0x30 (8 doubleword home slots for
 * r3..r10, 9th arg onward at r1+0x70). Floats in prototype-less calls are
 * mirrored into the GPR image, so %f can reinterpret the GPR bits.
 * -----------------------------------------------------------------------*/
static int yz_format_guest(char* out, size_t outsz, ppu_context* ctx,
                           uint32_t fmt_ea, int first_vararg /* 0 = r3 */)
{
    const char* f = (const char*)(vm_base + fmt_ea);
    size_t o = 0;
    int ai = first_vararg;

    /* fetch integer-arg slot i (0-based from r3) */
    #define YZ_ARG(i) ((i) < 8 ? ctx->gpr[3 + (i)] \
                              : vm_read64(ctx->gpr[1] + 0x30 + (uint64_t)(i) * 8))

    while (*f && o + 1 < outsz) {
        if (*f != '%') { out[o++] = *f++; continue; }

        /* collect the conversion spec; '*' width/precision consumes an
         * integer argument (e.g. SPURS prints names with %.*s) */
        char spec[32];
        size_t sl = 0;
        spec[sl++] = *f++;                      /* '%' */
        while (*f && sl < sizeof(spec) - 16 &&
               (*f == '-' || *f == '+' || *f == ' ' || *f == '#' || *f == '0' ||
                (*f >= '0' && *f <= '9') || *f == '.' || *f == '*')) {
            if (*f == '*') {
                int w = (int)(int32_t)(uint32_t)YZ_ARG(ai++);
                sl += (size_t)snprintf(spec + sl, sizeof(spec) - sl, "%d", w);
                f++;
            } else {
                spec[sl++] = *f++;
            }
        }
        int l_count = 0;
        while (*f == 'l' || *f == 'h' || *f == 'z') {
            if (*f == 'l') l_count++;
            f++;                                /* length mods re-added below */
        }
        char conv = *f ? *f++ : 0;
        if (!conv) break;

        char piece[512];
        piece[0] = 0;
        uint64_t a;
        switch (conv) {
        case '%':
            piece[0] = '%'; piece[1] = 0;
            break;
        case 's': {
            a = YZ_ARG(ai++);
            /* guard: a %s arg below the loaded image range is not a string
             * pointer (mis-indexed vararg or genuine garbage) */
            const char* s = ((uint32_t)a >= 0x10000u)
                          ? (const char*)(vm_base + (uint32_t)a)
                          : (a ? "(badptr)" : "(null)");
            spec[sl++] = 's'; spec[sl] = 0;
            snprintf(piece, sizeof(piece), spec, s);
            break;
        }
        case 'c':
            a = YZ_ARG(ai++);
            spec[sl++] = 'c'; spec[sl] = 0;
            snprintf(piece, sizeof(piece), spec, (int)a);
            break;
        case 'p':
            a = YZ_ARG(ai++);
            snprintf(piece, sizeof(piece), "0x%08X", (uint32_t)a);
            break;
        case 'd': case 'i':
            a = YZ_ARG(ai++);
            spec[sl++] = 'l'; spec[sl++] = 'l'; spec[sl++] = conv; spec[sl] = 0;
            /* PS3 long = 64-bit; plain int = 32 */
            snprintf(piece, sizeof(piece), spec,
                     l_count ? (long long)a : (long long)(int32_t)(uint32_t)a);
            break;
        case 'u': case 'x': case 'X': case 'o':
            a = YZ_ARG(ai++);
            spec[sl++] = 'l'; spec[sl++] = 'l'; spec[sl++] = conv; spec[sl] = 0;
            snprintf(piece, sizeof(piece), spec,
                     l_count ? (unsigned long long)a
                             : (unsigned long long)(uint32_t)a);
            break;
        case 'f': case 'F': case 'g': case 'G': case 'e': case 'E': {
            a = YZ_ARG(ai++);
            double d;
            memcpy(&d, &a, 8);                  /* GPR image of the double */
            spec[sl++] = conv; spec[sl] = 0;
            snprintf(piece, sizeof(piece), spec, d);
            break;
        }
        default:
            /* unknown conversion: emit it literally, consume one arg */
            a = YZ_ARG(ai++);
            snprintf(piece, sizeof(piece), "%%%c?(0x%llX)", conv,
                     (unsigned long long)a);
            break;
        }
        size_t pl = strlen(piece);
        if (pl > outsz - 1 - o) pl = outsz - 1 - o;
        memcpy(out + o, piece, pl);
        o += pl;
    }
    #undef YZ_ARG
    out[o] = 0;
    return (int)o;
}

extern "C" void yz_ovr__sys_printf(ppu_context* ctx)
{
    char buf[2048];
    int n = yz_format_guest(buf, sizeof(buf), ctx, (uint32_t)ctx->gpr[3], 1);
    printf("[PS3] %s", buf);
    fflush(stdout);
    ctx->gpr[3] = (uint64_t)(int64_t)n;
}

extern "C" void yz_ovr__sys_sprintf(ppu_context* ctx)
{
    char buf[2048];
    int n = yz_format_guest(buf, sizeof(buf), ctx, (uint32_t)ctx->gpr[4], 2);
    uint32_t dst = (uint32_t)ctx->gpr[3];
    if (dst) memcpy(vm_base + dst, buf, (size_t)n + 1);
    ctx->gpr[3] = (uint64_t)(int64_t)n;
}

extern "C" void yz_ovr__sys_snprintf(ppu_context* ctx)
{
    char buf[2048];
    int n = yz_format_guest(buf, sizeof(buf), ctx, (uint32_t)ctx->gpr[5], 3);
    uint32_t dst  = (uint32_t)ctx->gpr[3];
    uint32_t size = (uint32_t)ctx->gpr[4];
    if (dst && size) {
        uint32_t copy = (uint32_t)n < size - 1 ? (uint32_t)n : size - 1;
        memcpy(vm_base + dst, buf, copy);
        *(vm_base + dst + copy) = 0;
    }
    ctx->gpr[3] = (uint64_t)(int64_t)n;
}
