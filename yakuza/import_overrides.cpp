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
#include "rsx_null_backend.h"   /* pulls rsx_commands.h: rsx_state, processor */

#include <cstdio>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" uint8_t* vm_base;

/* lv2 sys_event handlers (runtime/syscalls/sys_event.c) -- driven directly from
 * sys_rsx_context_allocate to set up libgcm's RSX event port/queue. */
extern "C" int64_t sys_event_port_create(ppu_context*);
extern "C" int64_t sys_event_queue_create(ppu_context*);
extern "C" int64_t sys_event_port_connect_local(ppu_context*);
extern "C" int64_t sys_event_port_send(ppu_context*);
extern "C" void spu_lockline_lock(void);    /* SPU GETLLAR/PUTLLC serialization */
extern "C" void spu_lockline_unlock(void);

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
 * RSX driver (LLE): sys_rsx syscalls + FIFO consumer
 *
 * Sony's real libgcm_sys (recomp_prx/libgcm_sys_*) now runs the gcm API; the
 * game's cellGcmSys imports bind to its export OPDs (the former hand-rolled
 * gcm HLE below is retired -- yz_gcm_fifo_callback is a dead stub kept only
 * for the dispatch routing). libgcm drives the RSX through the lv2 sys_rsx
 * syscalls (668-677, 0x29C-0x2A5) implemented at the bottom of this file.
 *
 * The driver<->driver contract is a set of GUEST-memory structures, laid out
 * per RPCS3 Emu/Cell/lv2/sys_rsx.{h,cpp} (the oracle, reimplemented):
 *   - context_allocate hands libgcm the dma_control / driver_info / reports
 *     base addresses and fills driver_info (version_driver 0x211 -- libgcm
 *     validates it, libgcm_sys_recomp_000.cpp:770 -- frequencies, offsets).
 *   - the game writes the FIFO PUT offset into dma_control (+0x40); our
 *     consumer reads PUT/GET there and executes the committed command stream.
 *     libgcm owns the ring, so there is no producer/consumer race.
 *   - context_attribute(package_id) carries flip / display-buffer / vblank;
 *     flips publish completion into driver_info.head[].flipFlags (set on the
 *     vblank tick, async, so it survives the game's ResetFlipStatus ordering),
 *     which the game's render loop polls inline.
 * -----------------------------------------------------------------------*/

/* RsxDmaControl: put/get/ref at +0x40/+0x44/+0x48 (0x40-byte reserved prefix).
 * Sony's cellGcmGetControlRegister returns dma_control + 0x40. */
#define RSX_DMACTL_PUT 0x40u
#define RSX_DMACTL_GET 0x44u
#define RSX_DMACTL_REF 0x48u
/* RsxDriverInfo.head[8] base (sizeof RsxDriverInfo 0x12F8, head[8] 0x200, so
 * head starts at 0x10B8); each head is 0x40 bytes, flipFlags at +0x08. */
#define RSX_DRIVERINFO_HEAD 0x10B8u
#define RSX_HEAD_STRIDE     0x40u

/* Context region (dma_control/driver_info/reports/device): placed in the
 * RSX-reserved VM window (0x10000000, reserved-not-committed by vm_init), so
 * it never collides with the game's main-memory heap -- like RPCS3's separate
 * vm::rsx_context. Committed on demand. */
#define RSX_CTX_BASE      0x10000000u
#define RSX_DMA_CONTROL   (RSX_CTX_BASE + 0x000000u)
#define RSX_DRIVER_INFO   (RSX_CTX_BASE + 0x100000u)
#define RSX_REPORTS       (RSX_CTX_BASE + 0x200000u)
#define RSX_DEVICE_ADDR   (RSX_CTX_BASE + 0x300000u)

static uint32_t g_rsx_local_mem_size = YZ_GCM_LOCAL_SIZE;
static int      g_rsx_ctx_ready      = 0;   /* context_allocate done */
static uint32_t g_rsx_event_port     = 0;   /* RSX event port (libgcm handler) */
/* Captured by dispatch.cpp's YZ_TASK_TRACE on cellSpursCreateTask2WithBinInfo:
 * the taskset ea, so the vblank tick can dump the SPURS workload-ready state
 * (1f dispatch diagnostic). */
extern "C" { uint32_t g_yz_spurs_taskset = 0; }

/* io-offset -> guest EA map, 1 MB granularity (index = io >> 20), built by
 * sys_rsx_context_iomap (replaces the libs cellGcmIoOffsetToAddress the HLE
 * used). 0xFFFFFFFF = unmapped. */
static uint32_t g_rsx_iomap_ea[4096];
static int      g_rsx_iomap_init = 0;

/* HLE gcm (2026-06-14f): EA base of the io command-buffer ring (the ioAddress the
 * game passed to _cellGcmInitBody). The ring maps linearly io X -> yz_gcm_io_addr+X,
 * so ea->io for the ring is `ea - yz_gcm_io_addr`. Set by yz_ovr__cellGcmInitBody. */
static uint32_t yz_gcm_io_addr = 0;
static uint32_t yz_gcm_io_size = 0;

static void yz_rsx_iomap_ensure_init(void)
{
    if (!g_rsx_iomap_init) {
        g_rsx_iomap_init = 1;
        for (int i = 0; i < 4096; i++) g_rsx_iomap_ea[i] = 0xFFFFFFFFu;
    }
}

static uint32_t yz_rsx_io_to_ea(uint32_t io)
{
    uint32_t page = io >> 20;
    if (page >= 4096) return 0;
    uint32_t base = g_rsx_iomap_ea[page];
    if (base == 0xFFFFFFFFu) return 0;
    return base + (io & 0xFFFFFu);
}

static uint32_t yz_rsx_head_addr(uint32_t h)
{
    return RSX_DRIVER_INFO + RSX_DRIVERINFO_HEAD + (h & 7u) * RSX_HEAD_STRIDE;
}

/* TEMP DIAG: the consumer should only ever write RSX regions (local 0xC0..,
 * reports/dma 0x10.., io 0x404..). A write into the game's main memory
 * (0x00010000-0x0FFFFFFF) is corruption -- log it with the target+value. */
static void yz_rsx_w32(uint32_t addr, uint32_t val)
{
    /* Legit consumer targets: local VRAM (0xC0000000+), reports/dma/device
     * (0x10000000-0x10400000), io (0x40400000+). A write into game main memory
     * (0x00010000-0x0FFFFFFF) OR a guest stack (0xD0000000-0xDFFFFFFF) is the
     * consumer derailing -- and a stack hit corrupts a live frame's saved
     * non-volatiles (#19d: r31/r27 came back 0 across _cellGcmInitBody). */
    if ((addr >= 0x00010000u && addr < 0x10000000u) ||
        (addr >= 0xD0000000u && addr < 0xE0000000u)) {
        static int n = 0;
        if (n < 24) { n++;
            fprintf(stderr, "[rsx-corrupt] consumer writes %s 0x%08X = 0x%08X\n",
                    addr >= 0xD0000000u ? "STACK" : "GAME", addr, val);
        }
    }
    vm_write32(addr, val);
}

/* Display buffers registered via context_attribute(0x104). */
struct yz_rsx_dispbuf { uint32_t offset, pitch, width, height; };
static yz_rsx_dispbuf g_rsx_dispbuf[8];
static uint32_t       g_rsx_dispbuf_count;

/* Pending flips per head. A flip is QUEUED by GCM_DRIVER_QUEUE (records the head
 * + buffer) and SUBMITTED by the flip-sema RELEASE (label+0x10 = 0xFFFFFFFF) that
 * follows it; only the RELEASE arms `pending` so the vblank tick can never clear
 * the label before the stream writes 0xFFFFFFFF (avoids a clear/redirty race).
 * The vblank tick then presents the head's buffer + clears the label. */
static volatile long g_rsx_flip_pending[8];
static uint32_t      g_rsx_queued_head = 1;   /* head of the last GCM_DRIVER_QUEUE */

/* Real RSX command translator (libs/video/rsx_commands.c): the consumer
 * delegates 3D rendering methods to it (state tracking + backend clear/draw).
 * State init happens at context_allocate. */
static rsx_state g_rsx_state;

/* Dedicated window thread. A Win32 window must be created AND message-pumped on
 * the same thread, so it lives here rather than on the main/consumer threads.
 * rsx_null_backend_init() opens the window and registers the null backend (GDI
 * clear-color present); the consumer + flip path then drive it. */
static DWORD WINAPI yz_window_thread(LPVOID)
{
    if (rsx_null_backend_init(1280, 720, "Yakuza: Dead Souls (ps3recomp)") != 0) {
        fprintf(stderr, "[rsx] window init failed\n");
        return 1;
    }
    for (;;) {
        if (rsx_null_backend_pump_messages() < 0)
            break;            /* window closed */
        Sleep(8);
    }
    return 0;
}

/* Present the current frame to the window. The clear color was already tracked
 * into the backend as the consumer processed this frame's NV4097 clear methods,
 * so present() just flips it onto the window. */
static void yz_rsx_present(uint32_t buffer_id)
{
    rsx_backend* b = rsx_get_backend();
    if (!b) return;
    if (b->end_frame) b->end_frame(b->userdata);
    if (b->present)   b->present(b->userdata, buffer_id);
}

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
    case 0x66606660u:                            /* SEMAPHORE_RW (report/label, local) */
    case 0x66616661u:                            /* SEMAPHORE_R  (report/label, main) */
        /* labels/report semaphores live in the reports region context_allocate
         * set up (= what Sony's cellGcmGetLabelAddress returns into). */
        return RSX_REPORTS + (offset & 0xFFFFFu);
    case 0x56616660u:                            /* DEVICE_RW (RPCS3 gcm_enums.h:520) */
    case 0x56616661u:                            /* DEVICE_R  -> device_addr + offset.
        * The flip HW-sync semaphore lives at device+0x30 (RSXThread.cpp:240). */
        return (offset < 0x100000u) ? RSX_DEVICE_ADDR + offset : 0;
    case 0xFEED0001u:                            /* main memory via the io map */
        return yz_rsx_io_to_ea(offset);
    case 0xFEED0000u:                            /* RSX local memory */
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
            yz_rsx_w32(addr, arg);
        return 0;
    }

    /* GCM_DRIVER_QUEUE (0xE940 + head*4): the game queues a display flip here
     * (RPCS3 gcm::queue_flip -> sys_rsx 0x103; rsx_methods.cpp:1730). arg = the
     * display-buffer id. Record it on the head; the actual "submit" is the
     * flip-sema RELEASE that follows (which arms `pending`). */
    if (method >= 0xE940 && method <= 0xE95C) {
        uint32_t head = ((method - 0xE940) >> 2) & 7u;
        uint32_t ha = yz_rsx_head_addr(head);
        vm_write32(ha + 0x14, arg);                              /* lastQueuedBufferId */
        vm_write32(ha + 0x08, vm_read32(ha + 0x08) | 0x40000000u | (1u << (arg & 31)));
        g_rsx_queued_head = head;
        { static int n = 0; if (n < 8) { n++;
            fprintf(stderr, "[rsx] DRIVER_QUEUE head=%u buf=%u (flip queued)\n", head, arg); } }
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
        vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_REF, arg);
        break;
    case 0x060:                                   /* NV406E SET_CONTEXT_DMA_SEMAPHORE */
        yz_rsx_sem_dma_406e = arg;
        break;
    case 0x064:                                   /* NV406E SEMAPHORE_OFFSET */
        yz_rsx_sem_off_406e = arg;
        break;
    case 0x068:                                   /* NV406E SEMAPHORE_ACQUIRE */
        addr = yz_rsx_sem_addr(yz_rsx_sem_dma_406e, yz_rsx_sem_off_406e);
        /* Dedup: log only when the (addr,want) pair changes, so a NEW stall
         * surfaces without flooding on the per-poll retries. */
        { static uint32_t la=0xDEAD, lw=0xDEAD;
          if (addr != la || arg != lw) { la = addr; lw = arg;
            fprintf(stderr, "[sem] ACQUIRE off=0x%X addr=0x%08X want=0x%08X have=0x%08X %s\n",
                    yz_rsx_sem_off_406e, addr, arg, addr?vm_read32(addr):0,
                    (addr && vm_read32(addr)!=arg)?"STALL":"pass"); } }
        if (addr && vm_read32(addr) != arg)
            return 1;                             /* not yet satisfied: stall, retry later */
        break;
    case 0x06C:                                   /* NV406E SEMAPHORE_RELEASE */
        addr = yz_rsx_sem_addr(yz_rsx_sem_dma_406e, yz_rsx_sem_off_406e);
        /* HW flip-sync: a release of 0 to device+0x30 is written as 1 (the RSX
         * never writes 0 there without a display-queue command). nv406e.cpp:130 */
        if (addr == RSX_DEVICE_ADDR + 0x30 && arg == 0) arg = 1;
        { static int sl=0; if (sl<60){ sl++;
            fprintf(stderr, "[sem] RELEASE off=0x%X addr=0x%08X val=0x%08X\n",
                    yz_rsx_sem_off_406e, addr, arg); } }
        if (addr)
            yz_rsx_w32(addr, arg);
        /* A release of the flip semaphore (label+0x10) to the pending marker is
         * the "flip submitted" signal -- arm the queued head so the next vblank
         * presents it and clears the label. Arming here (after the 0xFFFFFFFF
         * write) keeps the vblank clear strictly ordered after the dirty. */
        if (addr == RSX_REPORTS + 0x10 && arg != 0)
            InterlockedExchange(&g_rsx_flip_pending[g_rsx_queued_head & 7u], 1);
        break;
    case 0x1A4:                                   /* NV4097 SET_CONTEXT_DMA_SEMAPHORE */
        yz_rsx_sem_dma_4097 = arg;
        break;
    case 0x1D6C:                                  /* NV4097 SET_SEMAPHORE_OFFSET */
        yz_rsx_sem_off_4097 = arg;
        break;
    case 0x1D70:                                  /* BACK_END_WRITE_SEMAPHORE_RELEASE */
        addr = yz_rsx_sem_addr(yz_rsx_sem_dma_4097, yz_rsx_sem_off_4097);
        { static int sl=0; if (sl<60){ sl++;
            fprintf(stderr, "[sem] BE_RELEASE off=0x%X addr=0x%08X val=0x%08X(arg=0x%08X)\n",
                    yz_rsx_sem_off_4097, addr,
                    (arg & 0xFF00FF00u)|((arg&0xFFu)<<16)|((arg>>16)&0xFFu), arg); } }
        if (addr)
            yz_rsx_w32(addr, (arg & 0xFF00FF00u) |
                             ((arg & 0xFFu) << 16) | ((arg >> 16) & 0xFFu));
        break;
    case 0x1D74:                                  /* TEXTURE_READ_SEMAPHORE_RELEASE */
        addr = yz_rsx_sem_addr(yz_rsx_sem_dma_4097, yz_rsx_sem_off_4097);
        if (addr)
            yz_rsx_w32(addr, arg);
        break;
    default:
        /* NV3062/NV308A 2D methods were handled by the first switch above;
         * anything else is a 3D rendering method -> toolkit NV4097 translator
         * (tracks state, drives the backend clear/draw). Unknown methods are
         * ignored there (and self-logged in debug builds). */
        if (!((method >= 0x6000 && method < 0x7000) ||
              (method >= 0xA000 && method < 0xB000)))
            rsx_process_method(&g_rsx_state, method, arg);
        break;
    }
    return 0;
}

/* FIFO consumer (RPCS3 FIFO_control model -- Emu/RSX/RSXFIFO.cpp).
 *
 * Under LLE, Sony's libgcm owns the ring and the game (t1) produces commands +
 * advances PUT; this thread is the RSX side that consumes [GET, PUT) and
 * advances GET. The two invariants that keep it from racing t1's live writes:
 *   1. RE-READ PUT before every command -- never read past the committed
 *      boundary into a segment t1 is still filling.
 *   2. WRITE GET BACK on every advance -- t1's get-based flow control reuses
 *      ring space only after GET passes it; a laggy/bogus GET makes t1 rewrite
 *      memory we're mid-read (the old hand-rolled consumer's bug).
 * Process ONE command per iteration (no aggressive drain with a stale PUT), and
 * idle at the jump-to-self stopper t1 places at PUT (re-poll until t1 patches
 * it). On an unsatisfied semaphore acquire, leave GET in place and retry. */

/* DIAG (TEMP, strip before commit): control-transfer ring. Records the last 64
 * jumps/calls/returns the consumer FOLLOWED so that when it parks on a STALE
 * jump-to-self (one with PUT ahead of GET -- the deadlock signature) we can dump
 * the exact path GET took into the island, plus the ring topology. Pins whether
 * the consumer took a wrong jump (path divergence) or the stopper is an unpatched
 * barrier. (blocker #21) */
struct yz_ct_ent { uint32_t from, cmd, to; const char* kind; };
static yz_ct_ent g_ct_ring[64];
static unsigned  g_ct_head = 0;
static inline void yz_ct_push(uint32_t from, uint32_t cmd, uint32_t to, const char* kind) {
    yz_ct_ent& e = g_ct_ring[g_ct_head & 63u];
    e.from = from; e.cmd = cmd; e.to = to; e.kind = kind; g_ct_head++;
}
static void yz_ct_dump(uint32_t parkget, uint32_t put) {
    fprintf(stderr, "[ct] === consumer parked on STALE jump-to-self io=0x%06X (PUT=0x%06X ahead) ===\n",
            parkget, put);
    fprintf(stderr, "[ct] last %u control transfers (oldest first):\n",
            g_ct_head < 64u ? g_ct_head : 64u);
    unsigned start = g_ct_head >= 64u ? g_ct_head - 64u : 0u;
    for (unsigned i = start; i < g_ct_head; i++) {
        yz_ct_ent& e = g_ct_ring[i & 63u];
        fprintf(stderr, "    %-4s io=0x%06X cmd=0x%08X -> io=0x%06X\n",
                e.kind, e.from, e.cmd, e.to);
    }
    fprintf(stderr, "[ct] fragment heads (io F -> [F],[F+4],[F+8]) and tails ([F+0xFFFF8],[F+0xFFFFC]):\n");
    for (uint32_t F = 0; F <= 0x700000u; F += 0x100000u) {
        uint32_t h0 = yz_rsx_io_to_ea(F), h1 = yz_rsx_io_to_ea(F + 4), h2 = yz_rsx_io_to_ea(F + 8);
        uint32_t t0 = yz_rsx_io_to_ea(F + 0xFFFF8u), t1 = yz_rsx_io_to_ea(F + 0xFFFFCu);
        fprintf(stderr, "    io 0x%06X head %08X %08X %08X | tail %08X %08X\n", F,
                h0 ? vm_read32(h0) : 0, h1 ? vm_read32(h1) : 0, h2 ? vm_read32(h2) : 0,
                t0 ? vm_read32(t0) : 0, t1 ? vm_read32(t1) : 0);
    }
    /* what does frame 3 (the live region up to PUT) look like at its head + just below PUT? */
    fprintf(stderr, "[ct] live region: words around PUT=0x%06X:\n", put);
    for (uint32_t off = (put >= 0x10u ? put - 0x10u : 0u); off <= put + 0x8u; off += 4u) {
        uint32_t e = yz_rsx_io_to_ea(off);
        fprintf(stderr, "    io 0x%06X = %08X%s\n", off, e ? vm_read32(e) : 0,
                off == put ? "  <- PUT" : "");
    }
}

extern "C" void yz_watch_arm(uint32_t);   /* main.cpp page-guard write-watch (TEMP) */
extern "C" uint32_t g_yz_game_toc;        /* dispatch.cpp: the game module's TOC */

/* PARSE-TRACE ring (TEMP DIAG, blocker #21): the last method packets the consumer
 * actually parsed (with their counts), so a CALL-not-ready park can be checked for
 * ALIGNMENT -- is GET on a real packet boundary (the CALL is genuine -> blocked
 * producer) or did a mis-counted method land us mid-packet on an ARG misread as a
 * CALL (a phantom list = a consumer parse bug, not a producer block)? */
struct yz_mt_ent { uint32_t get, cmd; uint16_t count, method; };
static yz_mt_ent g_mt_ring[64];
static unsigned  g_mt_head = 0;
static inline void yz_mt_push(uint32_t g, uint32_t c, uint32_t cnt, uint32_t m) {
    yz_mt_ent& e = g_mt_ring[g_mt_head & 63u];
    e.get = g; e.cmd = c; e.count = (uint16_t)cnt; e.method = (uint16_t)m; g_mt_head++;
}
static void yz_mt_dump(uint32_t parkget, uint32_t parkcmd) {
    static int done = 0; if (done) return; done = 1;
    fprintf(stderr, "[mt] parked at CALL io=0x%06X cmd=0x%08X; last %u method packets parsed "
            "(next == this.get+4+count*4 should chain to the CALL if aligned):\n",
            parkget, parkcmd, g_mt_head < 64u ? g_mt_head : 64u);
    unsigned start = g_mt_head >= 64u ? g_mt_head - 64u : 0u;
    for (unsigned i = start; i < g_mt_head; i++) {
        yz_mt_ent& e = g_mt_ring[i & 63u];
        fprintf(stderr, "    io=0x%06X cmd=0x%08X m=0x%04X cnt=%-3u -> next io=0x%06X\n",
                e.get, e.cmd, e.method, e.count, e.get + 4u + (uint32_t)e.count * 4u);
    }
}

/* The game's gcm flush/reserve (func_00E9BC9C / func_00E9B630 / func_00E9BF14)
 * places a self-jump stopper at every commit and RELEASES the previous one. A
 * same-fragment release is IMMEDIATE -- it writes 0 (NOP) over the stopper word
 * (func_00E9BC9C @0xE9BE60: `stw r11,0(r9)` with r11=S[0x1C]=0). A CROSS-fragment
 * release (the commit spanned a gcm buffer recycle, so S[0x24]!=ctx->end) is
 * DEFERRED into the game's gcm op-list and applied later:
 *     S    = *(game_toc - 0x7410)                 (the gcm-state struct)
 *     list = [ S[+8] (base) .. S[+0] (write head) ), 0x20-byte entries
 *     entry[+0] = op tag (0x7F == deferred stopper-release), entry[+4] = stopper EA
 * The @0x300000 deadlock (blocker #21) is exactly this: io 0x300000's release was
 * deferred (verified live: list entry tag=0x7F ea=0x40700000) but the game never
 * drains the list -- it's stuck spinning on the flip fence that needs frame 3 to
 * execute, which needs this very stopper released. Since PUT is already past the
 * stopper, the body IS committed and the game HAS committed to releasing it, so
 * applying the deferred release the moment GET reaches the stopper is faithful to
 * the game's own intent (the accurate form of the 14c timer heuristic).
 * Returns true iff stopper_ea is queued for release in the op-list. */
static bool yz_gcm_stopper_release_deferred(uint32_t stopper_ea)
{
    if (!g_yz_game_toc) return false;
    uint32_t S = vm_read32(g_yz_game_toc - 0x7410u);
    if (S < 0x10000u || S >= 0xE0000000u) return false;
    uint32_t base = vm_read32(S + 0x08u);
    uint32_t head = vm_read32(S + 0x00u);
    if (base < 0x10000u || base >= 0xE0000000u) return false;
    if (head < base || (head - base) > 0x4000u) return false;   /* sane, un-wrapped */
    for (uint32_t e = base; e < head; e += 0x20u)
        if (vm_read32(e + 0x00u) == 0x7Fu && vm_read32(e + 0x04u) == stopper_ea)
            return true;
    return false;
}

/* SINGLE-SEGMENT regime (env YZ_BIG_SEG, 2026-06-16): pin ctx->end (gcm_ctx+4) to the
 * io-buffer end so the game's same-segment release check (S[0x24]==ctx->end) ALWAYS
 * passes -> every stopper-release is IMMEDIATE (no cross-segment defer, no S[0x1C] latch,
 * no drain needed) -> GET flows. Reproduces the 2026-06-14f single-segment behaviour that
 * reached fence-advance + no deadlock, but via the LIVE context (no _cellGcmInitBody
 * replacement). Tight monitor so it beats the game's stopper-placement rate. (A real ring
 * WRAP needs handling once current nears the buffer end -- many seconds of frames away;
 * fine for the test window. If this flows, add wrap handling + make it faithful.) */
static DWORD WINAPI yz_bigseg_monitor(LPVOID)
{
    /* FORCE-IMMEDIATE gcm stopper-release (2026-06-19): the inline gcm flush
     * func_00E9BC9C @0xE9BCF8 reads S[0x1C] (S = *(game_toc-0x7410)); S[0x1C]!=0 ->
     * DEFER the release into the op-list (tag 0x7F) that never drains while t1 is
     * flip-wedged (blocker #21); S[0x1C]==0 -> release IMMEDIATELY (NOP the stopper
     * word so the RSX flows past it). Clamp S[0x1C] to 0 so every release is
     * immediate -> the op-list never accumulates -> GET is never stranded behind a
     * deferred stopper. (Reliable: same S pointer our deferred-release reader uses.) */
    /* IMMEDIATE STOPPER-RELEASE (env YZ_IMM_REL, 2026-06-19): the inline gcm flush
     * func_00E9BC9C @0xE9BCF8 reads S[0x1C] (S = *(game_toc-0x7410)); S[0x1C]!=0 ->
     * DEFER the stopper-release into the op-list (tag 0x7F) that only drains AFTER
     * the libgcm reserve (func_02103AAC) returns -- but the reserve never returns
     * because it is waiting on GET, which is waiting on this very release. Clamp
     * S[0x1C]=0 so every release is IMMEDIATE (the same-fragment NOP-patch path),
     * matching RPCS3 where releases are consumed promptly. Pair with YZ_NO_DEFER
     * (faithful consumer that spins on the stopper like RPCS3's run_FIFO). */
    fprintf(stderr, "[immrel] monitor up: clamping gcm defer latch S[0x1C]=0 (force immediate release)\n");
    int cleared = 0;
    for (;;) {
        if (g_yz_game_toc) {
            uint32_t S = vm_read32(g_yz_game_toc - 0x7410u);
            if (S >= 0x10000u && S < 0xE0000000u) {
                if (vm_read32(S + 0x1Cu) != 0u) {
                    vm_write32(S + 0x1Cu, 0u);
                    if (cleared < 12) { cleared++;
                        fprintf(stderr, "[immrel] cleared S[0x1C] latch (S=0x%08X)\n", S); }
                }
            }
        }
        Sleep(0);   /* yield, tight -- beat func_00E9BC9C's S[0x1C] read */
    }
}

/* FENCE-KICK probe (env YZ_FENCE_KICK, 2026-06-19): read-watch proved t1 spins
 * polling the flip fence 0x40C00000 (stuck at 2); the next flip needs frame-3's
 * commands, which t1 only builds AFTER the throttle releases -> circular wait.
 * When GET is parked (the deadlock), bump the fence so t1's throttle releases,
 * and observe (via YZ_DIAG_PARK word@GET) whether t1 then FILLS io 0x1104D00 and
 * the loop self-sustains. Decides root: self-sustain => our fence advance is
 * under-modelled (real fix = drive it from the FIFO flip/label, not DRIVER_QUEUE);
 * re-deadlock => structural ordering inversion in t1's frame build. */
static DWORD WINAPI yz_fencekick_monitor(LPVOID)
{
    fprintf(stderr, "[fkick] monitor up: bumping flip fence 0x40C00000 while GET is parked\n");
    uint32_t last_get = ~0u; DWORD stuck_since = 0;
    for (;;) {
        uint32_t get = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_GET);
        DWORD now = GetTickCount();
        if (get != last_get) { last_get = get; stuck_since = now; }
        else if (now - stuck_since > 150u) {           /* GET parked = the deadlock */
            uint32_t f = vm_read32(0x40C00000u);
            vm_write32(0x40C00000u, f + 1u);
            static int n = 0; if (n < 60) { n++;
                fprintf(stderr, "[fkick] GET parked at 0x%X -> fence %u -> %u\n", get, f, f + 1u); }
            stuck_since = now;                          /* re-arm */
        }
        Sleep(50);
    }
}

/* BUFDESC GEOMETRY DUMP (env YZ_DUMP_BUFDESC, 2026-06-20): resolve libgcm's buffer
 * descriptor (bufdesc = *(libgcm_toc 0x02114000 - 0x7FD8) = *0x0210C028) and dump the
 * segment-geometry fields the reserve func_02103AAC waits on (+0x10 dma-ctrl, +0x14 base,
 * +0x18, +0x1C, +0x20, +0x28, +0x30 seg-size, +0x38 guard, +0x4C type). Resolves the
 * conflict: init func_021036D4 writes +0x30=0x2000 but pt22 measured 0x40000 at runtime
 * + called it "dynamic". Three passes (after deadlock latch ~32s, then +8s twice) to
 * catch any dynamic change. Read-only; default boot unaffected. */
static DWORD WINAPI yz_bufdesc_dump(LPVOID)
{
    const uint32_t pbd = 0x02114000u - 0x7FD8u;     /* libgcm_toc - 0x7FD8 = 0x0210C028 */
    for (int pass = 0; pass < 3; pass++) {
        Sleep(pass == 0 ? 32000 : 8000);
        uint32_t bd  = vm_read32(pbd);
        uint32_t get = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_GET);
        uint32_t put = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT);
        fprintf(stderr, "[bufdesc pass%d] ptr@0x%08X -> bd=0x%08X | GET=0x%08X PUT=0x%08X\n",
                pass, pbd, bd, get, put);
        if (bd >= 0x10000u && bd < 0xE0000000u) {
            static const uint32_t offs[] = {0x10,0x14,0x18,0x1C,0x20,0x24,0x28,0x2C,
                                            0x30,0x34,0x38,0x3C,0x40,0x44,0x48,0x4C};
            for (uint32_t i = 0; i < sizeof(offs)/sizeof(offs[0]); i++)
                fprintf(stderr, "    bd+0x%02X = 0x%08X\n", offs[i], vm_read32(bd + offs[i]));
            uint32_t segsz = vm_read32(bd + 0x30);
            uint32_t base  = vm_read32(bd + 0x14);
            fprintf(stderr, "    => base(+0x14)=0x%08X  seg-size(+0x30)<<2=0x%X (%u KB)  guard(+0x38)<<2=0x%X\n",
                    base, segsz << 2, (segsz << 2) >> 10, vm_read32(bd + 0x38) << 2);
        }
        fflush(stderr);
    }
    return 0;
}

/* SINGLE-SEGMENT reserve override (env YZ_ONESEG, 2026-06-20). libgcm's segment-recycle
 * reserve func_02103AAC (r3 = bufdesc 0x0210C3FC: begin@+0/end@+4/current@+8; geometry
 * +0x14 base, +0x18 buffer-end, +0x28 reserve-off, +0x30 seg-size words, +0x34 seg-count)
 * carves the 8 MB FIFO into 8x1 MB segments and advances one per call. MEASURED wedge: when
 * GET follows a CALL OUT of the ring into an unfinalized display list (io 0x1104D00) and the
 * producer then needs to recycle a segment, the reserve waits for GET to return to the ring
 * bounds -> never -> deadlock (scratch/reserve2.txt #36).
 *
 * FIX: promote the ring to ONE segment spanning the whole buffer. While end != buffer-end,
 * set end = buffer-end (+ seg-size=0x200000 words/<<2=8MB, count=1) and SKIP the real reserve
 * -- the producer keeps writing linearly (no recycle) so it never wedges mid-frame; by the
 * time cur reaches buffer-end it has emitted many frames + finalized its display lists, GET
 * has drained, and the real reserve's WRAP path (end==bd+0x18) recreates the full segment and
 * waits on a drained GET. Returns 1 = handled (skip real reserve), 0 = run the real reserve. */
extern "C" int yz_gcm_reserve_oneseg(ppu_context* ctx)
{
    /* OBSERVE-ONLY (the skip-based single-segment was PROVEN structurally wrong, 2026-06-20:
     * func_02103AAC is the per-fragment FIFO COMMIT -- it writes the fragment JUMP, advances
     * the segment AND flushes PUT. Skipping it froze PUT at 0x3544 while cur advanced =
     * starved GET, scratch/oneseg2.txt. So the reserve cannot be bypassed; a correct
     * single-segment would have to REPLICATE its commit minus the recycle-wait. The wedge's
     * true cause: the reserve's GET-wait never clears because GET is parked OUTSIDE the ring
     * (display list io 0x1104D00, > bd+0x20=0x7FFFFC) -- a producer-finalization root.)
     * Always returns 0 = run the real reserve; just trace the call state. */
    uint32_t bd = (uint32_t)ctx->gpr[3];
    if (!bd) return 0;
    static int logn = 0;
    if (logn < 40) { logn++;
        fprintf(stderr, "[reserve #%d t%u] bd=0x%08X begin=0x%08X end=0x%08X cur=0x%08X | "
                "GET=0x%06X PUT=0x%06X | seg=0x%X cnt=0x%X\n",
                logn, yz_thread_current_id(), bd, vm_read32(bd + 0x0), vm_read32(bd + 0x4),
                vm_read32(bd + 0x8), vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_GET),
                vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT),
                vm_read32(bd + 0x30), vm_read32(bd + 0x34)); fflush(stderr); }
    return 0;
}

/* SINGLE-BIG-SEGMENT (env YZ_SEGBIG, 2026-06-20, the principled LAYER-1 fix). libgcm
 * carves the 8 MB FIFO into 1 MB segments (bufdesc+0x30 = 0x40000 words); t1 wedges in
 * the reserve recycling a segment whose GET is parked (proven: resync past it -> frame 3
 * flips). Make the segment size = the rest of the buffer so t1 builds whole frames without
 * recycling. We poke bufdesc+0x30 AS SOON AS init sets it (while ctx is still at segment 0,
 * end=base+1MB), so the next reserve-advance creates a ~7 MB segment ending exactly at the
 * 8 MB buffer end (no overrun). bufdesc = *(libgcm_toc 0x02114000 - 0x7FD8) = *0x0210C028. */
static DWORD WINAPI yz_segsize_mon(LPVOID)
{
    const uint32_t pbd = 0x02114000u - 0x7FD8u;   /* 0x0210C028 */
    for (int i = 0; i < 100000; i++) {
        uint32_t bd = vm_read32(pbd);
        if (bd >= 0x10000u && bd < 0xE0000000u && vm_read32(bd + 0x30u) == 0x40000u) {
            vm_write32(bd + 0x30u, 0x1C0000u);    /* ~7 MB: next segment = rest of buffer */
            fprintf(stderr, "[segbig] bd=0x%08X: seg-size 0x40000 -> 0x1C0000 (single big segment)\n", bd);
            fflush(stderr);
            return 0;
        }
        Sleep(1);
    }
    fprintf(stderr, "[segbig] bufdesc seg-size never read 0x40000; not poked\n");
    return 0;
}

/* Direct write-watch on the frame-3 display list io 0x1104D00 (EA 0x41504D00), armed
 * independent of the consumer path (YZ_WATCH_LIST only arms when GET reaches the CALL,
 * which never happens in faithful YZ_NO_DEFER mode). Waits for the io map + display-list
 * region to commit (~6s), then arms. Answers: does the PRODUCER ever build this list? */
static DWORD WINAPI yz_watch_dlea_mon(LPVOID)
{
    const char* s = getenv("YZ_WATCH_DLEA");
    uint32_t ea = (s && s[0] && s[1] != '\0') ? (uint32_t)strtoul(s, nullptr, 16) : 0x41504D00u;
    if (ea < 0x10000u) ea = 0x41504D00u;   /* "1" etc -> default frame-3 list */
    /* Poll until the page commits, then arm immediately -- catches the FIRST write
     * (e.g. frames 1-2 building io 0x1100100 at ea 0x41500100) to name the producer. */
    for (int i = 0; i < 600; i++) {
        MEMORY_BASIC_INFORMATION mbi;
        if (vm_base && VirtualQuery(vm_base + ea, &mbi, sizeof(mbi)) && (mbi.State & MEM_COMMIT)) {
            fprintf(stderr, "[watch-dlea] arming write-watch on ea 0x%08X (committed @ ~%dms)\n", ea, i * 20);
            yz_watch_arm(ea);
            return 0;
        }
        Sleep(20);
    }
    fprintf(stderr, "[watch-dlea] ea 0x%08X never committed; not armed\n", ea);
    return 0;
}

/* OP-LIST APPEND TRACE (env YZ_WATCH_OPLIST, 2026-06-20 pt25b): the data-patch APPLY
 * never runs (the deadlock is before frame-finalize), but the APPEND does -- t1 builds
 * the frame, deferring data patches (tag 0x04/0x08/0x09) into the op-list. Arm a
 * write-watch on the op-list BASE (entry[0]'s tag field, which the YZ_DUMP_SEG dump
 * showed is a tag-0x08 data patch); the watch fires in the appender's context and dumps
 * its reliable caller chain -> NAMES the function that defers a data patch. That function
 * (+ its apply mirror, usually the same module) is the key to the drain. base = *(S+8),
 * S = *(game_toc-0x7410). Arms once the op-list is allocated. */
static DWORD WINAPI yz_oplist_watch_mon(LPVOID)
{
    for (int i = 0; i < 4000; i++) {
        if (g_yz_game_toc) {
            uint32_t S = vm_read32(g_yz_game_toc - 0x7410u);
            if (S >= 0x10000u && S < 0xE0000000u) {
                uint32_t base = vm_read32(S + 0x08u);
                if (base >= 0x10000u && base < 0xE0000000u) {
                    fprintf(stderr, "[oplist-watch] arming write-watch on op-list base 0x%08X "
                            "(S=0x%08X) -- names the data-patch APPENDER\n", base, S);
                    fflush(stderr);
                    yz_watch_arm(base);   /* entry[0] tag field */
                    return 0;
                }
            }
        }
        Sleep(5);
    }
    fprintf(stderr, "[oplist-watch] op-list base never resolved; not armed\n");
    return 0;
}

/* Pacing A/B (2026-06-20, LAYER-1). The consumer is a background CreateThread that
 * Sleep(1)s at every idle/park point. On Windows Sleep(1) rounds up to the timer
 * quantum (~1 ms with timeBeginPeriod, else ~15 ms), so when GET catches up to PUT
 * or parks on a stopper the consumer NAPS while t1 keeps filling the ring -> t1
 * laps GET and wedges in libgcm's reserve usleep (func_02103AAC). YZ_TIGHT swaps the
 * idle naps for a continuous, FAIR spin (YieldProcessor pause + SwitchToThread) so
 * the consumer drains like a hardware pipeline -- testing whether pacing alone keeps
 * t1 out of the reserve. Default boot path unchanged (tight off). */
static inline void rsx_idle(int tight)
{
    if (tight) {
        for (int i = 0; i < 64; i++) YieldProcessor();
        SwitchToThread();   /* yield to t1 if it's ready on this core; return now if not */
    } else {
        Sleep(1);
    }
}

static DWORD WINAPI yz_rsx_consumer(LPVOID)
{
    uint32_t fifo_ret    = ~0u;         /* 1-level call stack */
    uint32_t g_call_entry = ~0u;        /* entry of the current sub (self-loop detect) */
    /* DEADLOCK-BREAK state: the game RELEASES a stopper by patching it
     * (func_00E9BC9C@0xE9BE60 writes a NOP; cross-fragment -> JUMP; display-list
     * self-loop -> RET) and we normally WAIT for that. But the @0x300000-class wall
     * is a TRUE mutual deadlock (write-watch: the game writes the stopper, then goes
     * silent -- it parks in the flip-throttle/recycle waiting for the RSX while the
     * RSX waits for the patch). So if PUT stays FROZEN >80 ms (the game is stuck, not
     * building) we force the release ourselves, mirroring the patch. A frozen-PUT gate
     * (not a timer) is essential: forcing mid-build races into un-committed data. */
    uint32_t stall_put   = ~1u;
    DWORD    stall_since  = 0;
    /* COHERENCE-SAFE deferred-release (LAYER 1, 2026-06-15e). When GET enters a
     * segment via the game's queued stopper-release, we DON'T zero the stopper word
     * (that satisfies the producer's recycle-wait `while [stopper]!=0` and lets it
     * reuse/rewrite the segment under us = the corruption we are removing). We hold
     * the stopper ea here and zero it only once GET has LEFT the segment -- so the
     * producer can reuse it only after GET demonstrably passed. */
    uint32_t def_release_ea  = 0;
    uint32_t def_release_seg = 0;
    /* FAITHFUL by default (Layer 1, STATUS.md 2026-06-14d): a jump-to-self
     * stopper is a MEMWATCH -- park GET on it and wait for the game to patch the
     * word, mirroring real HW (RPCS3 FIFO_control::set_get/read memwatch). The
     * pre-14d deadlock-break TIMERS force GET past an un-patched stopper, which
     * corrupts the GET we report to libgcm's recycle/flip flow-control -- the
     * suspected cause of the handshake deadlocks. YZ_RSX_HEURISTIC=1 restores the
     * old behavior so the two can be A/B-compared in one build. */
    const int heur = getenv("YZ_RSX_HEURISTIC") != nullptr;
    const int tight = getenv("YZ_TIGHT") != nullptr;
    /* CALL-into-stale guard (2026-06-20): a committed CALL to a per-frame display list
     * (io 0x1104D00) whose target is NOT yet finalized -- the first word is a non-command
     * (0xA2000500 stale shader, 0xA0030003 bits set) -- must NOT be followed. Following it
     * parks GET at io 0x1104D00 which is OUTSIDE the 8 MB ring, wedging libgcm's reserve
     * (it waits forever for GET to return to ring bounds) -> t1 never finalizes the list.
     * WAIT at the CALL instead (GET stays in the ring) so the reserve keeps working and t1
     * can finalize the list, then follow. Faithful: RPCS3's GET never executes stale data. */
    const int callwait = getenv("YZ_CALLWAIT") != nullptr;
    /* CALL-skip (2026-06-20, LAYER-1 angle): when a committed CALL targets an unbuilt
     * (stale, non-command first word) display list, SKIP it -- advance GET past the CALL
     * and keep processing the main ring -- instead of waiting/following. Rationale: the
     * deferred-release zooms GET to the CALL before the producer builds the list; parking
     * there (callwait) makes the producer lap+wedge so it never builds it. Skipping keeps
     * GET advancing (producer never wedged) at the cost of frame N's geometry, to see if
     * the loop self-advances (fence climbs, the NEXT frame's list gets built). */
    const int callskip = getenv("YZ_CALLSKIP") != nullptr;
    /* FAITHFUL-STOPPER + WRAP-AWARE SKIP (env YZ_FAITHSKIP, 2026-06-20) -- the
     * RPCS3-grounded LAYER-1 fix. GROUND TRUTH (user RSX-debug, EA 0x40700000 = io
     * 0x300000): a segment-entry stopper's RELEASED form is 0x20N00004 (a jump fwd
     * one word to the real method header at io 0xN00004); its NOT-READY form is
     * 0x20N00000 (jump-to-self). RPCS3's GET never parks on the jump-to-self -- the
     * producer patches it BEFORE GET arrives. Our deferred-release instead force-
     * advances past the UNpatched 0x20N00000 into the STALE io 0xN00004 (a leftover
     * shader-bind arg like 0x01104D02), misreads it as a CALL, and derails into shader
     * microcode at io 0x1104D00 -> GET freezes -> t1's libgcm reserve wedges -> it
     * never builds/patches the segment. FIX: (1) at a jump-to-self, WAIT for the word
     * to become a non-self jump (0x20N00004) -- never read stale io+4; (2) if it stays
     * a stopper > 120ms (t1 reserve-wedged), advance GET to the NEXT segment boundary
     * (wrap-aware: ring io 0x1000..0x800000) -- always a jump word, never stale data --
     * so GET leaves t1's recycle region and t1 can build + patch the segment. */
    const int faithskip = getenv("YZ_FAITHSKIP") != nullptr;
    uint32_t fs_stop_get = ~0u; DWORD fs_stop_since = 0;
    /* SKIP-GARBAGE consumer (env YZ_SKIPGARBAGE, 2026-06-20 pt25b): the segment is
     * built with deferred-patch PLACEHOLDERS (op-list journal, applied only on ring-
     * wrap which never comes). Rather than apply the full drain (deep, machinery spread
     * across many funcs), exploit that our parser RELIABLY flags non-commands (the
     * 0xA0030003 mask: real methods incl. draws 0x00041808 / flip 0x0004E944 pass it;
     * every placeholder/data-block fails it). On a jump-to-self stopper OR a non-command
     * word OR a CALL into a stale list, SKIP one word and re-check (stay within [GET,PUT))
     * until the next valid command. Walks past the un-drained placeholders to the real
     * draws + the in-stream flip command -> breaks the deadlock, flips flow. LOSSY (the
     * deferred-patch values are missing -> some geometry wrong) but it gets us PAST the
     * LAYER-1 deadlock; the faithful drain refines it later. */
    const int skipgarbage = getenv("YZ_SKIPGARBAGE") != nullptr;
    fprintf(stderr, "[rsx] FIFO consumer up: %s%s\n",
            heur ? "HEURISTIC mode (deadlock-break timers ON)"
                 : "FAITHFUL mode (memwatch stopper, no GET-forcing heuristics)",
            tight ? " + TIGHT drain (no Sleep idles)" : "");
    /* FIFO STEP TRACE (env YZ_FIFO_TRACE=<path>, 2026-06-20): a canonical, machine-
     * parseable play-by-play of EXACTLY what the consumer does, one line per step:
     *   M  <midx> <io_get> <method> <value>     -- each dispatched NV method (method &
     *                                              0x3FFFC, same decode as RPCS3's .rrc)
     *   J  <step> <io_get> <io_put> <word> <tgt>            -- jump taken
     *   C  <step> <io_get> <io_put> <word> <ctgt> FOLLOW|SKIP|WAIT  -- call
     *   R  <step> <io_get> <io_put> <ret>                   -- return
     *   S  <step> <io_get> <io_put> <word> PARK|ADV         -- jump-to-self stopper
     * The 'M' stream is directly diffable against RPCS3's working-frame .rrc (emit it
     * with tools/rrc_methods.py; align with tools/cmp_fifo.py) -> shows precisely where
     * our executed stream diverges from / stops short of RPCS3's. */
    const char* ft_path = getenv("YZ_FIFO_TRACE");
    FILE* ft = ft_path ? fopen(ft_path, "w") : nullptr;
    uint32_t ft_midx = 0, ft_step = 0, ft_park = ~0u, ft_cwait = ~0u;
    if (ft) fprintf(ft, "# YZ FIFO TRACE  kinds: M(method) J(jump) C(call) R(ret) S(stopper)\n");
    for (;;) {
        if (!g_rsx_ctx_ready) { Sleep(1); continue; }
        /* Re-read GET and PUT every iteration. We are the only writer of GET in
         * steady state; PUT is t1's and bounds us to the committed [GET,PUT). */
        uint32_t get = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_GET);
        const uint32_t put = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT) & ~3u;

        /* Coherence: once GET has LEFT (moved to a later main-ring segment than) the
         * segment we entered via a deferred-release, zero that segment's entry stopper
         * -- only now is reuse safe (GET passed it). Skip while GET is inside a called
         * display list (io >= 0x01000000); it will RET back into the segment. */
        if (def_release_ea && get < 0x01000000u && (get & ~0xFFFFFu) != def_release_seg) {
            vm_write32(def_release_ea, 0);
            def_release_ea = 0;
        }

        if (get == put) { rsx_idle(tight); continue; }   /* committed boundary: idle */

        const uint32_t ea = yz_rsx_io_to_ea(get);
        if (!ea) {
            static int warned = 0;
            if (!warned) { warned = 1;
                fprintf(stderr, "[rsx] GET=0x%08X (PUT=0x%08X) not io-mapped; idling\n", get, put); }
            rsx_idle(tight);
            continue;
        }
        const uint32_t cmd = vm_read32(ea);

        /* ---- flow control (jump / call / return) ---- */
        if ((cmd & 0xE0000003u) == 0x20000000u || (cmd & 3u) == 1u) {   /* old|new jump */
            const uint32_t tgt = (cmd & 3u) == 1u ? (cmd & 0xFFFFFFFCu) : (cmd & 0x1FFFFFFCu);
            if (tgt == get) {                          /* jump-to-self stopper */
                if (skipgarbage) {                     /* skip the un-released stopper word */
                    if (ft) { fprintf(ft, "S\t%u\t0x%06X\t0x%06X\t0x%08X\tGSKIP\n", ft_step++, get, put, cmd); fflush(ft); }
                    get += 4; vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, get);
                    continue;
                }
                if (faithskip) {
                    /* Faithful: NEVER advance into stale io+4. Wait for the game to
                     * patch this word to 0x20N00004 (released); if it stays a stopper
                     * too long (t1 reserve-wedged), skip to the next segment boundary
                     * so GET leaves t1's recycle region. */
                    DWORD now = GetTickCount();
                    if (get != fs_stop_get) { fs_stop_get = get; fs_stop_since = now; }
                    if (now - fs_stop_since > 120u && put != get) {
                        /* t1 reserve/flip-wedged: catch GET up to the committed write
                         * head (PUT) to free it -- the proven pt24 resync lever, but now
                         * (a) inside the consumer (no racing monitor) and (b) protected
                         * by the faithful stopper above, so the next read can't derail
                         * into stale data the way the bare resync did. */
                        if (ft) { fprintf(ft, "S\t%u\t0x%06X\t0x%06X\t0x%08X\tCATCHUP->0x%06X\n",
                                          ft_step++, get, put, cmd, put); fflush(ft); }
                        { static int sk = 0; if (sk < 24) { sk++;
                            fprintf(stderr, "[rsx] FAITHSKIP: stopper io=0x%X unpatched %lums -> GET=PUT 0x%X\n",
                                    get, now - fs_stop_since, put); } }
                        /* ONE-TIME committed-segment dump (is [stopper,PUT) real frame-3
                         * content with a flip command, or stale?). Decode each word. */
                        { static int dumped = 0;
                          if (!dumped && getenv("YZ_DUMP_SEG")) { dumped = 1;
                            uint32_t lo = get & ~0xFFFFFu, hi = (put > lo && put < lo + 0x100000u) ? put + 0x10u : lo + 0x120u;
                            fprintf(stderr, "[segdump] committed io 0x%X..0x%X (GET=0x%X PUT=0x%X):\n", lo, hi, get, put);
                            for (uint32_t o = lo; o < hi; o += 4u) {
                                uint32_t e2 = yz_rsx_io_to_ea(o); uint32_t w = e2 ? vm_read32(e2) : 0;
                                const char* k = ((w&0xE0000003u)==0x20000000u)?"JUMP":((w&3u)==1u)?"jmpN":
                                                ((w&3u)==2u)?"CALL":(w==0x20000u)?"RET ":
                                                ((w&0xA0030003u)?"DATA":"meth");
                                fprintf(stderr, "    io 0x%06X = 0x%08X %s m=0x%05X cnt=%u%s\n",
                                        o, w, k, w&0x3FFFCu, (w>>18)&0x7FFu, o==get?" <-GET":(o==put?" <-PUT":"")); }
                            /* Dump the gcm op-list entries (full 0x20 bytes each) so we can
                             * see the VALUE the game's drain would write for each tag-0x7F
                             * stopper-release (esp. io 0x300000's first-method header). */
                            if (g_yz_game_toc) {
                                uint32_t S = vm_read32(g_yz_game_toc - 0x7410u);
                                uint32_t b = (S>=0x10000u&&S<0xE0000000u) ? vm_read32(S+0x08u) : 0;
                                uint32_t h = (S>=0x10000u&&S<0xE0000000u) ? vm_read32(S+0x00u) : 0;
                                fprintf(stderr, "[oplist] S=0x%08X base=0x%08X head=0x%08X\n", S, b, h);
                                int n = 0;
                                for (uint32_t e = b; b && e < h && n < 32; e += 0x20u, n++) {
                                    fprintf(stderr, "  [%2d] tag=0x%02X ea=0x%08X  +08=0x%08X +0C=0x%08X +10=0x%08X +14=0x%08X +18=0x%08X +1C=0x%08X\n",
                                            n, vm_read32(e+0x00u), vm_read32(e+0x04u), vm_read32(e+0x08u),
                                            vm_read32(e+0x0Cu), vm_read32(e+0x10u), vm_read32(e+0x14u),
                                            vm_read32(e+0x18u), vm_read32(e+0x1Cu)); }
                            }
                            fflush(stderr); } }
                        get = put; vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, get);
                        fs_stop_get = ~0u;
                        continue;
                    }
                    if (ft && ft_park != get) { ft_park = get;
                        fprintf(ft, "S\t%u\t0x%06X\t0x%06X\t0x%08X\tWAIT\n", ft_step++, get, put, cmd); fflush(ft); }
                    rsx_idle(tight);
                    continue;
                }
                /* HLE (default = PURE FAITHFUL): a jump-to-self is a memwatch -- park
                 * and wait for the game to patch it. Under single-segment HLE gcm the
                 * game's threads don't wedge (no fragment-recycle), so they release
                 * their own stoppers and no consumer-side break is needed. The LLE
                 * band-aids below (op-list deferred-release + frozen-PUT NOP break)
                 * are gated behind YZ_RSX_HEURISTIC for A/B comparison only. */
                /* COHERENCE-SAFE deferred-release (LAYER 1, 2026-06-15e -- replaces the
                 * unsound NOP). The game queues a cross-segment stopper-release into its
                 * op-list (tag 0x7F) and applies it from its own drain-shepherd, which
                 * never runs while it is flip-blocked -> GET wedges here. We honour the
                 * game's OWN queued intent, advancing GET past the stopper but WITHOUT
                 * zeroing the word (zeroing satisfies the producer's recycle-wait and
                 * lets it reuse the segment under us). The held stopper is zeroed -- above
                 * -- once GET leaves the segment. No frozen-PUT NOP-force any more: that
                 * shoved GET past un-finalised data (exactly the corruption removed here). */
                if (!getenv("YZ_NO_DEFER") && yz_gcm_stopper_release_deferred(ea)) {
                    uint32_t seg = get & ~0xFFFFFu;
                    if (def_release_ea && def_release_seg != seg) vm_write32(def_release_ea, 0);
                    def_release_ea = ea; def_release_seg = seg;
                    if (ft) { fprintf(ft, "S\t%u\t0x%06X\t0x%06X\t0x%08X\tADV\n", ft_step++, get, put, cmd); fflush(ft); }
                    get += 4; vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, get);
                    continue;
                }
                (void)stall_put; (void)stall_since;
                if (ft && ft_park != get) { ft_park = get;
                    fprintf(ft, "S\t%u\t0x%06X\t0x%06X\t0x%08X\tPARK\n", ft_step++, get, put, cmd); fflush(ft); }
                rsx_idle(tight);
                continue;
            }
            yz_ct_push(get, cmd, tgt, "jmp");
            if (ft) { fprintf(ft, "J\t%u\t0x%06X\t0x%06X\t0x%08X\t0x%06X\n", ft_step++, get, put, cmd, tgt); fflush(ft); }
            get = tgt;
            vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, get);
            continue;
        }
        if ((cmd & 3u) == 2u) {                                          /* call */
            const uint32_t ctgt = cmd & 0xFFFFFFFCu;
            /* Self-loop CALL (back to the entry of the sub we are inside) = display-list
             * "park here" stopper. Wait for the game to patch it to a RET; force the
             * RET on a frozen-PUT deadlock. Never re-enter (clobbers the 1-level ret). */
            if (ctgt == g_call_entry && fifo_ret != ~0u) {
                /* Display-list "park here" self-loop CALL: the game patches it to a
                 * RET when it finalizes the list. Unlike a main-ring stopper this
                 * release is NOT pre-queued in the op-list (the game wedges in
                 * libgcm's reserve -- waiting for GET -- BEFORE it can finalize +
                 * queue it: the true producer/consumer interlock, blocker #21). We
                 * have already executed the list's committed content to reach this
                 * terminator, so once PUT stays frozen >80 ms (producer wedged) force
                 * the RET the game would write -- the only break for this interlock.
                 * HLE (default): PURE FAITHFUL -- wait for the game's own RET patch. */
                if (!heur) { rsx_idle(tight); continue; }
                if (put != stall_put) { stall_put = put; stall_since = GetTickCount();
                                        Sleep(1); continue; }
                if (GetTickCount() - stall_since < 80u) { Sleep(1); continue; }
                { static int n = 0; if (n < 8) { n++;
                    fprintf(stderr, "[rsx] interlock-break self-loop io=0x%X -> RET to io=0x%X\n",
                            get, fifo_ret); } }
                stall_since = GetTickCount();
                get = fifo_ret; fifo_ret = ~0u; g_call_entry = ~0u;
                vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, get);
                continue;
            }
            /* CALL-readiness: the main ring commits the [stopper][CALL display-list]
             * pair (PUT) BEFORE the game fills the CALLed list in its own io region.
             * WAIT (don't follow into a not-yet-written / unmapped list = walking
             * zeros). No skip-on-deadlock: skipping derails into the next un-ready
             * region; better to stall cleanly here than corrupt the stream. */
            const uint32_t ctgt_ea = yz_rsx_io_to_ea(ctgt);
            const uint32_t ctgt_w0 = ctgt_ea ? vm_read32(ctgt_ea) : 0u;
            const int stale_word = (ctgt_w0 & 0xA0030003u) != 0u;
            const int ctgt_stale = (callwait || callskip) && stale_word;
            if ((callskip || skipgarbage) && ctgt_ea && stale_word) {
                /* SKIP the unbuilt list: advance GET past the CALL, stay in the ring. */
                if (ft) { fprintf(ft, "C\t%u\t0x%06X\t0x%06X\t0x%08X\t0x%06X\tSKIP\n", ft_step++, get, put, cmd, ctgt); fflush(ft); }
                get += 4; vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, get);
                static int sk = 0; if (sk < 16) { sk++;
                    fprintf(stderr, "[rsx] CALL-SKIP unbuilt list io=0x%X (w0=0x%08X) -> GET=0x%X\n",
                            ctgt, ctgt_w0, get); }
                continue;
            }
            if (!ctgt_ea || ctgt_w0 == 0u || ctgt_stale) {
                if (ft && ft_cwait != get) { ft_cwait = get;
                    fprintf(ft, "C\t%u\t0x%06X\t0x%06X\t0x%08X\t0x%06X\tWAIT\n", ft_step++, get, put, cmd, ctgt); fflush(ft); }
                static int w = 0; if (w < 8) { w++;
                    fprintf(stderr, "[rsx] CALL target io=0x%X not ready (ea=0x%08X w0=0x%08X)%s; waiting\n",
                            ctgt, ctgt_ea, ctgt_w0, ctgt_stale ? " [stale]" : ""); }
                /* PRODUCER TRACE (env YZ_WATCH_LIST): arm the write-watch on the
                 * un-written display list's ea so the next write to it reveals the
                 * producer (tid + guest fn via the watch handler's back-chain dump).
                 * If it never fires, the producer is fully blocked before touching
                 * it -> that's the thread to chase. */
                if (ctgt_ea) { static int armed = 0;
                    if (!armed && getenv("YZ_WATCH_LIST")) { armed = 1;
                        fprintf(stderr, "[rsx] arming producer-watch on display-list ea=0x%08X (io 0x%X)\n",
                                ctgt_ea, ctgt);
                        yz_watch_arm(ctgt_ea); } }
                /* ALIGNMENT CHECK: decode the fragment we walked into this CALL
                 * (io frag-start .. GET+8) ONCE, to tell whether GET is on a real
                 * packet boundary (the CALL is genuine) or the deadlock-break
                 * fall-through misaligned us (the "CALL" is a method arg). */
                { static int fd = 0; if (!fd) { fd = 1;
                    uint32_t fs = get & ~0xFFFFFu;          /* 1 MB segment start */
                    fprintf(stderr, "[align] segment dump io 0x%X.. (GET=0x%X):\n", fs, get);
                    for (uint32_t o = fs; o < fs + 0x200u; o += 4u) {
                        uint32_t e2 = yz_rsx_io_to_ea(o);
                        uint32_t wv = e2 ? vm_read32(e2) : 0;
                        /* faithful classification (RPCS3 RSXFIFO.cpp: RSX_METHOD_NON_METHOD_CMD_MASK
                         * 0xa0030003 -- a real method has those bits clear; else DATA/flow). */
                        const char* k = ((wv&0xE0000003u)==0x20000000u)?"JUMP":((wv&3u)==1u)?"jmpN":
                                        ((wv&3u)==2u)?"CALL":(wv==0x20000u)?"RET ":
                                        ((wv&0xA0030003u)?"DATA":"meth");
                        fprintf(stderr, "    io 0x%06X = 0x%08X %s m=0x%X c=%u%s\n",
                                o, wv, k, wv&0xFFFCu, (wv>>18)&0x7FFu, o==get?"  <-GET":""); }
                    /* TERMINATOR SCAN: a VALID frame ends in a jump to the next segment
                     * (0x20?00000) -- find the first flow-control word after GET. If the
                     * region GET..GET+0x800 is continuous DATA with no flow-control, GET
                     * overran the frame into stale/uninitialised memory (coherence), not a
                     * parser misparse on a live frame. */
                    int found = 0;
                    for (uint32_t o = get + 4u; o < fs + 0x100000u; o += 4u) {
                        uint32_t e2 = yz_rsx_io_to_ea(o);
                        uint32_t wv = e2 ? vm_read32(e2) : 0;
                        if ((wv&0xE0000003u)==0x20000000u || (wv&3u)==1u || (wv&3u)==2u || wv==0x20000u) {
                            fprintf(stderr, "    [scan] first flow-control after GET: io 0x%06X = 0x%08X "
                                    "(+%d bytes)\n", o, wv, (int)(o - get)); found = 1; break; }
                        if (o >= get + 0x800u) {
                            fprintf(stderr, "    [scan] NO flow-control in GET..GET+0x800 -> continuous "
                                    "DATA = GET overran frame into stale memory (coherence, not parse)\n");
                            found = 1; break; } }
                    (void)found; } }
                /* PARSE TRACE: dump the last methods the consumer actually parsed to
                 * reach this CALL, so we can tell if GET is on a real packet boundary
                 * (the CALL is genuine -> blocked producer) or a method-count misparse
                 * landed us mid-packet on an ARG misread as a CALL (a phantom list). */
                yz_mt_dump(get, cmd);
                { static int cd = 0; if (!cd) { cd = 1; yz_ct_dump(get, put); } }
                /* DECISIVE TEST (env YZ_GET_RELIEF): is io 0x15D3134 unbuilt because t1 is
                 * BLOCKED (out of ring space) before its build, or because it NEVER builds
                 * it? When t1 is wedged here (PUT frozen >150ms), report the ring fully
                 * drained (GET=PUT) to free t1's gcm reserve, then watch io 0x15D3134:
                 *   - if t1 now BUILDS it  -> purely the circular space block (fixable by
                 *     freeing the ring at the right moment);
                 *   - if it STAYS empty    -> deeper: t1's control flow never reaches the
                 *     build, or builds at the wrong address.
                 * Corrupts the consumer stream (loses position) -- DIAGNOSTIC ONLY. */
                if (getenv("YZ_GET_RELIEF")) {
                    static uint32_t rput = ~0u; static DWORD rsince = 0;
                    if (put != rput) { rput = put; rsince = GetTickCount(); }
                    else if (GetTickCount() - rsince > 150u) {
                        static int n = 0; if (n < 40) { n++;
                            fprintf(stderr, "[rsx] GET-RELIEF: GET=PUT=0x%X to free t1's reserve "
                                    "(was parked at empty CALL io 0x%X)\n", put, ctgt); }
                        vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, put);
                        rsince = GetTickCount();
                        Sleep(1);
                        continue;
                    }
                }
                rsx_idle(tight);
                continue;
            }
            /* DIAG (2026-06-14h): log the display-list CALL targets the consumer
             * actually FOLLOWS (the WRITTEN lists) with their ea, so we know the
             * real per-frame list regions for this run -- then a YZ_WATCH_EA on one
             * of them catches the BUILDER filling it (the never-written io 0x15D3134
             * can't be watched; a region the game reuses each frame can). */
            { static int cl = 0; if (cl < 32) { cl++;
                fprintf(stderr, "[rsx] follow CALL -> list io 0x%X (ea 0x%08X) from io 0x%X\n",
                        ctgt, ctgt_ea, get); } }
            if (ft) { fprintf(ft, "C\t%u\t0x%06X\t0x%06X\t0x%08X\t0x%06X\tFOLLOW\n", ft_step++, get, put, cmd, ctgt); fflush(ft); }
            fifo_ret = get + 4;
            g_call_entry = ctgt;
            yz_ct_push(get, cmd, ctgt, "call");
            get = ctgt;
            vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, get);
            continue;
        }
        if (cmd == 0x00020000u) {                                        /* return */
            if (fifo_ret != ~0u) {
                yz_ct_push(get, cmd, fifo_ret, "ret");
                if (ft) { fprintf(ft, "R\t%u\t0x%06X\t0x%06X\t0x%06X\n", ft_step++, get, put, fifo_ret); fflush(ft); }
                get = fifo_ret; fifo_ret = ~0u; g_call_entry = ~0u;
                vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, get);
                continue;
            }
            static int warned = 0;
            if (!warned) { warned = 1;
                fprintf(stderr, "[rsx] RETURN without CALL at io=0x%08X; idling\n", get); }
            rsx_idle(tight);
            continue;
        }

        /* ---- method(s) ---- */
        /* Faithful NV4097 validation (RPCS3 RSXFIFO.cpp RSX_METHOD_NON_METHOD_CMD_MASK
         * 0xa0030003). After the jump/call/ret checks above, a real method has those
         * bits clear. If set, the word is NOT a command -- GET reached the end of the
         * commands the producer has finalised in this segment (the rest is data/stale
         * being written). WAIT instead of parsing data as a command and drifting.
         * Coherence-safe: we never zeroed this segment's entry stopper, so the producer
         * cannot reuse it under us while we wait for it to finish writing. */
        if (cmd & 0xA0030003u) {
            if (skipgarbage) {                         /* skip the un-drained placeholder word */
                if (ft) { fprintf(ft, "G\t%u\t0x%06X\t0x%06X\t0x%08X\tGSKIP\n", ft_step++, get, put, cmd); }
                get += 4; vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, get);
                continue;
            }
            static int w = 0; if (w < 24) { w++;
                fprintf(stderr, "[rsx] non-command word 0x%08X at io=0x%X (PUT=0x%X) -- segment "
                        "not finalised; waiting for producer\n", cmd, get, put); }
            /* DIAG (YZ_DIAG_PARK, 2026-06-19): decide LAYER-1 fix A vs B. At the park,
             * is PUT past GET (producer committed beyond us)? Does the window GET would
             * execute hold real NV4097 methods, or stay stale -- and does it transition
             * stale->real over time (finalisation timing)? Re-sampled ~400ms x12. */
            if (getenv("YZ_DIAG_PARK")) {
                static uint32_t dp_ea = 0; static DWORD dp_t0 = 0, dp_last = 0; static int dp_n = 0;
                DWORD now = GetTickCount();
                if (ea != dp_ea) { dp_ea = ea; dp_t0 = now; dp_last = 0; dp_n = 0; }
                if (dp_n < 12 && (dp_last == 0 || now - dp_last >= 400u)) {
                    dp_last = now;
                    int meth = 0, flow = 0, data = 0; uint32_t first = 0;
                    for (uint32_t o = get; o < get + 0x200u; o += 4u) {
                        uint32_t e2 = yz_rsx_io_to_ea(o); if (!e2) break;
                        uint32_t wv = vm_read32(e2);
                        if (o == get) first = wv;
                        if (((wv & 0xE0000003u) == 0x20000000u) || ((wv & 3u) == 1u) ||
                            ((wv & 3u) == 2u) || (wv == 0x00020000u)) flow++;
                        else if (wv & 0xA0030003u) data++;
                        else meth++;
                    }
                    fprintf(stderr, "[diag-park] t+%5lums GET=0x%X PUT=0x%X put-get=%d "
                            "word@GET=0x%08X | window[+0x200]: meth=%d flow=%d stale=%d\n",
                            now - dp_t0, get, put, (int)(put - get), first, meth, flow, data);
                    fflush(stderr);
                    dp_n++;
                }
            }
            /* CIRCULAR-WAIT BREAK TEST (env YZ_DLIST_BREAK, 2026-06-16): the wedge is
             * a true producer/consumer circle -- t1 wrote this CALLed display list's
             * HEAD (0xA2000500), then poll-waits (vblank-pump + usleep) for GET to
             * advance, BEFORE finalising the list; GET is parked HERE on that head.
             * If we RETURN out of the unfinished list (skip frame N's draws, resume
             * the rest of the segment = the label/fence writes t1 is waiting on),
             * does t1's wait clear so it finalises the list + the fence advances past
             * 2? Decisive test of "breakable circle". Only when inside a CALL and PUT
             * frozen; loses frame N's draws -- DIAGNOSTIC, not a render fix. */
            if (getenv("YZ_DLIST_BREAK") && fifo_ret != ~0u && get >= 0x01000000u) {
                static uint32_t bput = ~0u; static DWORD bsince = 0;
                if (put != bput) { bput = put; bsince = GetTickCount(); }
                else if (GetTickCount() - bsince > 250u) {
                    static int n = 0; if (n < 16) { n++;
                        fprintf(stderr, "[rsx] DLIST-BREAK: RET out of unfinished list io=0x%X "
                                "-> io=0x%X (PUT frozen, freeing t1's GET-wait)\n", get, fifo_ret); }
                    get = fifo_ret; fifo_ret = ~0u; g_call_entry = ~0u;
                    vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, get);
                    bsince = GetTickCount();
                    continue;
                }
            }
            rsx_idle(tight);
            continue;
        }
        const uint32_t count  = (cmd >> 18) & 0x7FFu;
        const uint32_t noninc = cmd & 0x40000000u;
        const uint32_t method = cmd & 0x3FFFCu;
        yz_mt_push(get, cmd, count, method);   /* TEMP DIAG: parse-trace ring */
        int stalled = 0;
        for (uint32_t i = 0; i < count && !stalled; i++) {
            const uint32_t op_ea = yz_rsx_io_to_ea(get + 4 + i * 4);
            if (!op_ea) break;
            const uint32_t eff = noninc ? method : method + i * 4;
            const uint32_t val = vm_read32(op_ea);
            if (ft) { fprintf(ft, "M\t%u\t0x%06X\t0x%05X\t0x%08X\n", ft_midx++, get, eff & 0x3FFFCu, val);
                      if ((ft_midx & 0x1FFu) == 0) fflush(ft); }
            stalled = yz_rsx_method(eff, val);   /* unmasked: byte-identical to prior behavior */
        }
        if (stalled) { rsx_idle(tight); continue; }   /* semaphore acquire not yet satisfied */
        get += 4 + count * 4;
        vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, get);
    }
    return 0;
}

extern "C" int64_t yz_sys_rsx_context_allocate(ppu_context*);   /* defined below */

/* HLE gcm out-of-space callback (root-cause fix, 2026-06-14f). The game's gcm
 * wrapper calls ctx->callback(ctx, count) when current+4 > end. We treat the whole
 * io command buffer as ONE segment and WRAP to begin when full -- CO-DESIGNED with
 * the faithful consumer:
 *   1. flush PUT = current (let the consumer drain what's pending);
 *   2. SYNCHRONOUSLY DRAIN -- wait until GET == PUT, so rewinding can't overwrite
 *      commands the consumer hasn't passed (this is the synchronization the old
 *      hand-rolled consumer lacked = the race that drove the LLE detour);
 *   3. write a jump-to-begin at the old current, rewind current=begin, PUT=begin.
 * One segment => ctx->end never changes => the game's cross-fragment release-defer
 * (the @0x300000 op-list deadlock) never triggers and nothing fragment-recycles, so
 * the game's threads never wedge. Routed here via YZ_GCM_CB_FAKE_KEY (dispatch.cpp). */
extern "C" void yz_gcm_fifo_callback(ppu_context* ctx)
{
    uint32_t gctx = (uint32_t)ctx->gpr[3];          /* CellGcmContextData* */
    { static int e = 0; if (e < 16) { e++;
        fprintf(stderr, "[gcm] callback(ctx=0x%08X count=0x%llX) begin=0x%08X end=0x%08X cur=0x%08X\n",
                gctx, (unsigned long long)ctx->gpr[4],
                gctx ? vm_read32(gctx + 0x0) : 0, gctx ? vm_read32(gctx + 0x4) : 0,
                gctx ? vm_read32(gctx + 0x8) : 0); } }
    if (gctx && yz_gcm_io_addr) {
        uint32_t begin = vm_read32(gctx + 0x0);     /* EA: segment start */
        uint32_t cur   = vm_read32(gctx + 0x8);     /* EA: current write head */
        uint32_t begin_off = begin - yz_gcm_io_addr;   /* io offset (linear ring) */
        uint32_t cur_off   = cur   - yz_gcm_io_addr;
        vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_PUT, cur_off);     /* flush pending */
        for (int spin = 0; spin < 4000; spin++) {                 /* drain (<=4 s) */
            if ((vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_GET) & ~3u) == cur_off) break;
            Sleep(1);
        }
        vm_write32(cur, 0x20000000u | (begin_off & 0x1FFFFFFCu));  /* jump-to-begin */
        vm_write32(gctx + 0x8, begin);                            /* current = begin */
        vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_PUT, begin_off);  /* PUT = begin */
        { static int n = 0; if (n < 8) { n++;
            fprintf(stderr, "[gcm] fifo wrap: drained to io 0x%X, rewound to io 0x%X\n",
                    cur_off, begin_off); } }
    }
    ctx->gpr[3] = 0;
}

/* HLE _cellGcmInitBody (root-cause fix, 2026-06-14f): replaces Sony's LLE libgcm
 * init. Sets up the guest-memory contract (reusing the sys_rsx setup, like RPCS3's
 * _cellGcmInitBody calls sys_rsx_context_allocate), maps the io command buffer, and
 * builds the gcm CONTEXT (single segment + our wrap callback). Args (RPCS3 ABI):
 * gpr[3]=CellGcmContextData**, gpr[4]=cmdSize, gpr[5]=ioSize, gpr[6]=ioAddress. */
extern "C" void yz_ovr__cellGcmInitBody(ppu_context* ctx)
{
    uint32_t ctx_slot = (uint32_t)ctx->gpr[3];   /* CellGcmContextData** */
    uint32_t cmd_size = (uint32_t)ctx->gpr[4];
    uint32_t io_size  = (uint32_t)ctx->gpr[5];
    uint32_t io_addr  = (uint32_t)ctx->gpr[6];
    fprintf(stderr, "[gcm-hle] _cellGcmInitBody(ctx**=0x%08X cmdSize=0x%X ioSize=0x%X "
            "ioAddr=0x%08X)\n", ctx_slot, cmd_size, io_size, io_addr);

    yz_gcm_io_addr = io_addr;
    yz_gcm_io_size = io_size;

    /* RSX local memory (0xC0000000): reserved by vm_init, commit it now -- under LLE
     * sys_rsx_memory_allocate did this; under HLE the game addresses local VRAM
     * directly (cellGcmAddressToOffset / MapLocalMemory) right after init. */
    VirtualAlloc(vm_base + YZ_GCM_LOCAL_BASE, YZ_GCM_LOCAL_SIZE, MEM_COMMIT, PAGE_READWRITE);

    /* Guest-memory contract + window + consumer (the setup sys_rsx did under LLE;
     * RPCS3's _cellGcmInitBody likewise drives sys_rsx_context_allocate). */
    {
        static ppu_context sc;
        memset(&sc, 0, sizeof(sc));
        yz_sys_rsx_context_allocate(&sc);
    }

    /* Linear io map for the command-buffer ring: io X -> io_addr + X. Display-list
     * buffers add their own entries via cellGcmMapMainMemory/MapEaIoAddress. */
    {
        uint32_t pages = (io_size + 0xFFFFFu) >> 20;
        if (pages > 4096) pages = 4096;
        for (uint32_t i = 0; i < pages; i++)
            g_rsx_iomap_ea[i] = io_addr + (i << 20);
    }

    /* Commit the gcm context page (0x0FF8xxxx) + the synthetic callback OPD whose
     * code word routes to yz_gcm_fifo_callback via YZ_GCM_CB_FAKE_KEY. */
    VirtualAlloc(vm_base + (YZ_GCM_CTX_ADDR & ~0xFFFu), 0x2000, MEM_COMMIT, PAGE_READWRITE);
    vm_write32(YZ_GCM_CB_OPD_ADDR + 0, YZ_GCM_CB_FAKE_KEY);
    vm_write32(YZ_GCM_CB_OPD_ADDR + 4, 0);

    /* CellGcmContextData: ONE segment over the whole io buffer (4 KB reserved at
     * the front), callback @+0xC = our wrap callback. begin/end/current are EAs. */
    uint32_t begin = io_addr + 0x1000;
    uint32_t end   = io_addr + (cmd_size ? cmd_size : 0x10000) - 4;
    vm_write32(YZ_GCM_CTX_ADDR + 0x0, begin);
    vm_write32(YZ_GCM_CTX_ADDR + 0x4, end);
    vm_write32(YZ_GCM_CTX_ADDR + 0x8, begin);
    vm_write32(YZ_GCM_CTX_ADDR + 0xC, YZ_GCM_CB_OPD_ADDR);

    /* The reserved first 4 KB begins with a jump into the buffer so the consumer
     * starting at GET=0 lands on the first command region. */
    vm_write32(io_addr, 0x20000000u | (0x1000u & 0x1FFFFFFCu));

    vm_write32(ctx_slot, YZ_GCM_CTX_ADDR);    /* hand the context to the game */
    ctx->gpr[3] = 0;
}

/* ---- HLE gcm helper overrides (root-cause fix, 2026-06-14f) ----------------
 * All io/control/label addresses point at the structures the faithful consumer
 * reads (RSX_DMA_CONTROL, RSX_REPORTS, g_rsx_iomap_ea) so the game and the RSX
 * agree by construction. Recovered from git 79869ac + reconciled. */
extern "C" int32_t  cellGcmGetTiledPitchSize(uint32_t size, uint32_t* pitch);
extern "C" uint64_t cellGcmGetTimeStampLocation(uint32_t index, uint32_t location);

/* put/get/ref poll pointer -> the GUEST dma_control (put@+0 get@+4 ref@+8). */
extern "C" void yz_ovr_cellGcmGetControlRegister(ppu_context* ctx)
{
    ctx->gpr[3] = RSX_DMA_CONTROL + RSX_DMACTL_PUT;
}

/* Labels live in the reports region the consumer writes (yz_rsx_sem_addr). */
extern "C" void yz_ovr_cellGcmGetLabelAddress(ppu_context* ctx)
{
    ctx->gpr[3] = RSX_REPORTS + ((uint32_t)ctx->gpr[3] & 0xFFu) * 0x10u;
}

/* Guest BE config: localAddr, ioAddr, localSize, ioSize, memFreq, coreFreq. */
extern "C" void yz_ovr_cellGcmGetConfiguration(ppu_context* ctx)
{
    uint32_t cfg = (uint32_t)ctx->gpr[3];
    if (cfg) {
        vm_write32(cfg + 0x00, YZ_GCM_LOCAL_BASE);
        vm_write32(cfg + 0x04, yz_gcm_io_addr);
        vm_write32(cfg + 0x08, YZ_GCM_LOCAL_SIZE);
        vm_write32(cfg + 0x0C, yz_gcm_io_size);
        vm_write32(cfg + 0x10, 650000000u);
        vm_write32(cfg + 0x14, 500000000u);
    }
    ctx->gpr[3] = 0;
}

/* ea -> io offset via the consumer's iomap (the game computes put=AddressToOffset
 * (current)). Linear fast path for the command-buffer ring. */
extern "C" void yz_ovr_cellGcmAddressToOffset(ppu_context* ctx)
{
    uint32_t ea = (uint32_t)ctx->gpr[3], out = (uint32_t)ctx->gpr[4];
    if (yz_gcm_io_addr && ea >= yz_gcm_io_addr && ea < yz_gcm_io_addr + yz_gcm_io_size) {
        if (out) vm_write32(out, ea - yz_gcm_io_addr);
        ctx->gpr[3] = 0; return;
    }
    for (uint32_t p = 0; p < 4096; p++) {
        uint32_t base = g_rsx_iomap_ea[p];
        if (base != 0xFFFFFFFFu && ea >= base && ea < base + 0x100000u) {
            if (out) vm_write32(out, (p << 20) | (ea - base));
            ctx->gpr[3] = 0; return;
        }
    }
    ctx->gpr[3] = (uint64_t)(int64_t)-1;
}

/* Map a guest EA region to a specific io offset in the consumer's iomap. */
extern "C" void yz_ovr_cellGcmMapEaIoAddress(ppu_context* ctx)
{
    uint32_t ea = (uint32_t)ctx->gpr[3], io = (uint32_t)ctx->gpr[4], size = (uint32_t)ctx->gpr[5];
    yz_rsx_iomap_ensure_init();
    uint32_t pages = (size + 0xFFFFFu) >> 20;
    for (uint32_t i = 0; i < pages && ((io >> 20) + i) < 4096; i++)
        g_rsx_iomap_ea[(io >> 20) + i] = ea + (i << 20);
    fprintf(stderr, "[gcm-hle] MapEaIoAddress ea=0x%08X io=0x%X size=0x%X\n", ea, io, size);
    ctx->gpr[3] = 0;
}

extern "C" void yz_ovr_cellGcmUnmapEaIoAddress(ppu_context* ctx) { ctx->gpr[3] = 0; }

extern "C" void yz_ovr_cellGcmGetTiledPitchSize(ppu_context* ctx)
{
    uint32_t pitch = 0;
    int32_t rc = cellGcmGetTiledPitchSize((uint32_t)ctx->gpr[3], &pitch);
    if (ctx->gpr[4]) vm_write32((uint32_t)ctx->gpr[4], pitch);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

extern "C" void yz_ovr_cellGcmGetTimeStampLocation(ppu_context* ctx)
{
    ctx->gpr[3] = cellGcmGetTimeStampLocation((uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4]);
}

/* HLE flip: present the buffer + signal the flip label the game polls. The SDK
 * entry points take the gcm CONTEXT as arg0 (r3), the buffer id in r4. */
extern "C" void yz_ovr__cellGcmSetFlipCommand(ppu_context* ctx)
{
    yz_rsx_present((uint32_t)ctx->gpr[4]);
    vm_write32(RSX_REPORTS + 0x10, 0);             /* flip label -> done */
    ctx->gpr[3] = 0;
}

extern "C" void yz_ovr__cellGcmSetFlipCommandWithWaitLabel(ppu_context* ctx)
{
    yz_rsx_present((uint32_t)ctx->gpr[4]);
    vm_write32(RSX_REPORTS + ((uint32_t)ctx->gpr[5] & 0xFFu) * 0x10u, (uint32_t)ctx->gpr[6]);
    vm_write32(RSX_REPORTS + 0x10, 0);
    ctx->gpr[3] = 0;
}

/* ===========================================================================
 * sys_rsx syscalls (lv2 668-677, 0x29C-0x2A5) -- registered in shims.cpp.
 * Issued by Sony's libgcm; oracle = Emu/Cell/lv2/sys_rsx.cpp (reimplemented).
 * Args in gpr[3..8], return value in gpr[3] (written by the dispatcher).
 * =========================================================================*/

extern "C" void yz_watch_arm(uint32_t);        /* main.cpp page-guard write-watch (TEMP) */
extern "C" void yz_watch_arm_read(uint32_t);   /* main.cpp page-guard READ-watch (TEMP) */

/* 668 (0x29C) sys_rsx_memory_allocate(mem_handle*, mem_addr*, size, flags,...) */
extern "C" int64_t yz_sys_rsx_memory_allocate(ppu_context* ctx)
{
    uint32_t p_handle = (uint32_t)ctx->gpr[3];
    uint32_t p_addr   = (uint32_t)ctx->gpr[4];   /* u64 out */
    uint32_t size     = (uint32_t)ctx->gpr[5];

    /* RSX local memory: reserved by vm_init, commit it (game addresses it
     * directly via cellGcmAddressToOffset, base 0xC0000000). */
    VirtualAlloc(vm_base + YZ_GCM_LOCAL_BASE, YZ_GCM_LOCAL_SIZE,
                 MEM_COMMIT, PAGE_READWRITE);
    g_rsx_local_mem_size = size ? size : YZ_GCM_LOCAL_SIZE;

    if (p_addr)   vm_write64(p_addr, YZ_GCM_LOCAL_BASE);
    if (p_handle) vm_write32(p_handle, 0x5A5A5A5Bu);
    fprintf(stderr, "[sys_rsx] memory_allocate size=0x%X -> addr=0x%08X handle=0x5A5A5A5B\n",
            size, YZ_GCM_LOCAL_BASE);
    return 0;
}

/* 669 (0x29D) sys_rsx_memory_free(mem_handle) */
extern "C" int64_t yz_sys_rsx_memory_free(ppu_context* ctx) { return 0; }

/* 670 (0x29E) sys_rsx_context_allocate(ctx_id*, dma_ctl*, drv*, reports*,
 *             mem_ctx, system_mode). Sets up the whole guest-memory contract
 *             and starts the window + FIFO consumer (was _cellGcmInitBody's
 *             job under HLE). */
extern "C" int64_t yz_sys_rsx_context_allocate(ppu_context* ctx)
{
    uint32_t p_ctx_id = (uint32_t)ctx->gpr[3];
    uint32_t p_dma    = (uint32_t)ctx->gpr[4];   /* u64 out */
    uint32_t p_drv    = (uint32_t)ctx->gpr[5];   /* u64 out */
    uint32_t p_rep    = (uint32_t)ctx->gpr[6];   /* u64 out */
    uint64_t sys_mode = ctx->gpr[8];

    yz_rsx_iomap_ensure_init();

    /* Commit the context region (dma_control / driver_info / reports / device,
     * 4 MB) in the reserved RSX VM window. */
    VirtualAlloc(vm_base + RSX_CTX_BASE, 0x400000, MEM_COMMIT, PAGE_READWRITE);

    /* driver_info -- RPCS3 sys_rsx.cpp:289. version_driver 0x211 is REQUIRED:
     * Sony's libgcm validates it and bails otherwise (libgcm_sys:770). */
    memset(vm_base + RSX_DRIVER_INFO, 0, 0x12F8);
    vm_write32(RSX_DRIVER_INFO + 0x00, 0x211);                 /* version_driver */
    vm_write32(RSX_DRIVER_INFO + 0x04, 0x5C);                  /* version_gpu */
    vm_write32(RSX_DRIVER_INFO + 0x08, g_rsx_local_mem_size);  /* memory_size */
    vm_write32(RSX_DRIVER_INFO + 0x0C, 1);                     /* hardware_channel */
    vm_write32(RSX_DRIVER_INFO + 0x10, 500000000u);            /* nvcore_frequency */
    vm_write32(RSX_DRIVER_INFO + 0x14, 650000000u);            /* memory_frequency */
    vm_write32(RSX_DRIVER_INFO + 0x2C, 0x1000);               /* reportsNotifyOffset */
    vm_write32(RSX_DRIVER_INFO + 0x30, 0);                    /* reportsOffset */
    vm_write32(RSX_DRIVER_INFO + 0x34, 0x1400);               /* reportsReportOffset */
    vm_write32(RSX_DRIVER_INFO + 0x54, (uint32_t)sys_mode);   /* systemModeFlags */

    /* reports region (RPCS3 init values: semaphore patterns, notify/report
     * timestamps = -1). semaphore[1024]@0, notify[64]@0x1000, report[2048]@0x1400. */
    memset(vm_base + RSX_REPORTS, 0, 0x9400);
    for (uint32_t i = 0; i < 1024; i += 4) {
        vm_write32(RSX_REPORTS + (i + 0) * 4, 0x1337C0D3u);
        vm_write32(RSX_REPORTS + (i + 1) * 4, 0x1337BABEu);
        vm_write32(RSX_REPORTS + (i + 2) * 4, 0x1337BEEFu);
        vm_write32(RSX_REPORTS + (i + 3) * 4, 0x1337F001u);
    }
    for (uint32_t i = 0; i < 64; i++)
        vm_write64(RSX_REPORTS + 0x1000 + i * 16, ~0ull);     /* notify timestamp */
    for (uint32_t i = 0; i < 2048; i++) {
        vm_write64(RSX_REPORTS + 0x1400 + i * 16 + 0, ~0ull); /* report timestamp */
        vm_write32(RSX_REPORTS + 0x1400 + i * 16 + 8, 0);     /* report val */
        vm_write32(RSX_REPORTS + 0x1400 + i * 16 + 12, ~0u);  /* report pad */
    }

    /* dma_control: get/put/ref = 0 (libgcm sets them up). */
    memset(vm_base + RSX_DMA_CONTROL, 0, 0x60);

    if (p_ctx_id) vm_write32(p_ctx_id, 0x55555555u);
    if (p_dma)    vm_write64(p_dma, RSX_DMA_CONTROL);
    if (p_drv)    vm_write64(p_drv, RSX_DRIVER_INFO);
    if (p_rep)    vm_write64(p_rep, RSX_REPORTS);

    /* RSX event port + queue (RPCS3 sys_rsx.cpp:317-325): libgcm spawns an
     * interrupt thread that does sys_event_queue_receive on
     * driver_info.handler_queue (@+0x12D0). Without a real queue it gets ESRCH
     * and the thread dies, leaving libgcm's gcm-handler delivery degenerate.
     * Create the port, create the queue (overwrites handler_queue with the queue
     * id), and connect them. The lv2 handlers read args from gpr[3..6]; drive
     * them with a scratch context (gpr offsets match the runtime layout). */
    {
        uint32_t hq   = RSX_DRIVER_INFO + 0x12D0;   /* driver_info.handler_queue */
        uint32_t attr = RSX_DEVICE_ADDR + 0x1000;   /* committed scratch */
        vm_write32(attr + 0, 1);                    /* SYS_SYNC_PRIORITY */
        vm_write32(attr + 4, 1);                    /* SYS_PPU_QUEUE */
        vm_write64(attr + 8, 0);                    /* name */
        static ppu_context sc;
        memset(&sc, 0, sizeof(sc));
        sc.gpr[3] = hq; sc.gpr[4] = 1 /*SYS_EVENT_PORT_LOCAL*/; sc.gpr[5] = 0;
        sys_event_port_create(&sc);
        g_rsx_event_port = vm_read32(hq);
        sc.gpr[3] = hq; sc.gpr[4] = attr; sc.gpr[5] = 0; sc.gpr[6] = 0x20;
        sys_event_queue_create(&sc);               /* overwrites hq with queue id */
        uint32_t qid = vm_read32(hq);
        sc.gpr[3] = g_rsx_event_port; sc.gpr[4] = qid;
        sys_event_port_connect_local(&sc);
        fprintf(stderr, "[sys_rsx] event port=%u queue=%u (driver_info.handler_queue)\n",
                g_rsx_event_port, qid);
    }

    g_rsx_ctx_ready = 1;
    fprintf(stderr, "[sys_rsx] context_allocate -> dma=0x%08X drv=0x%08X rep=0x%08X "
            "(sys_mode=0x%llX)\n", RSX_DMA_CONTROL, RSX_DRIVER_INFO, RSX_REPORTS,
            (unsigned long long)sys_mode);

    /* Bring up the window + RSX consumer + command-translator state (the
     * LLE-equivalent of the old _cellGcmInitBody startup). */
    static int started = 0;
    if (!started) {
        started = 1;
        rsx_state_init(&g_rsx_state);
        CreateThread(NULL, 0, yz_window_thread, NULL, 0, NULL);
        if (!getenv("YZ_NO_CONSUMER"))   /* TEMP: isolate consumer vs libgcm */
            CreateThread(NULL, 0, yz_rsx_consumer, NULL, 0, NULL);
        if (getenv("YZ_IMM_REL"))        /* force immediate stopper-release (S[0x1C]=0) */
            CreateThread(NULL, 0, yz_bigseg_monitor, NULL, 0, NULL);
        if (getenv("YZ_FENCE_KICK"))     /* bump flip fence when GET parks (root-diagnostic) */
            CreateThread(NULL, 0, yz_fencekick_monitor, NULL, 0, NULL);
        if (getenv("YZ_DUMP_BUFDESC"))   /* dump libgcm segment geometry at the deadlock */
            CreateThread(NULL, 0, yz_bufdesc_dump, NULL, 0, NULL);
        if (getenv("YZ_WATCH_DLEA"))     /* direct write-watch on frame-3 display list io 0x1104D00 */
            CreateThread(NULL, 0, yz_watch_dlea_mon, NULL, 0, NULL);
        if (getenv("YZ_SEGBIG"))         /* single big FIFO segment so t1 never recycle-wedges */
            CreateThread(NULL, 0, yz_segsize_mon, NULL, 0, NULL);
        if (getenv("YZ_WATCH_OPLIST"))   /* name the data-patch APPENDER (drain RE, pt25b) */
            CreateThread(NULL, 0, yz_oplist_watch_mon, NULL, 0, NULL);
        /* DIAG: catch who writes the jump-to-self stopper at io 0x300000
         * (ea 0x40700000). Reveals the game function/caller that parks the RSX
         * there -- flush safety-stopper vs deliberate pause vs corruption. */
        if (getenv("YZ_WATCH_300")) yz_watch_arm(0x40700000u);
        /* DIAG (2026-06-14g): catch who writes the flip fence at 0x40C00000 (io
         * 0x800000) that t1 spins on. Tells us if the RSX/consumer (in-stream
         * NV308A), the flip completion, or t1 itself advances it -- and why it
         * sticks at 2. The core question: what control value does the game wait
         * for that we don't produce. */
        if (getenv("YZ_WATCH_FENCE")) yz_watch_arm(0x40C00000u);
        /* DIAG (2026-06-14h): catch the DISPLAY-LIST BUILDER. Arm a write-watch
         * on an arbitrary ea (hex) -- point it at a list region that DOES get
         * written (e.g. frame-3's region 17 ea 0x41500000) so the watch fires
         * when the builder fills it; the (writer-aware) handler then names the
         * builder thread's guest tid + lifted function. Then the wait recorder
         * shows what that thread blocks on for the NEXT frame. */
        if (const char* we = getenv("YZ_WATCH_EA"))
            yz_watch_arm((uint32_t)strtoul(we, nullptr, 16));
        /* READ-watch (2026-06-19): catch what POLLS an address (e.g. the flip
         * fence 0x40C00000) in the reader's own context -> reliable caller chain. */
        if (const char* rd = getenv("YZ_WATCH_READ"))
            yz_watch_arm_read((uint32_t)strtoul(rd, nullptr, 16));
    }
    return 0;
}

/* 671 (0x29F) sys_rsx_context_free(context_id) */
extern "C" int64_t yz_sys_rsx_context_free(ppu_context* ctx) { return 0; }

/* 672 (0x2A0) sys_rsx_context_iomap(context_id, io, ea, size, flags) */
extern "C" int64_t yz_sys_rsx_context_iomap(ppu_context* ctx)
{
    uint32_t io   = (uint32_t)ctx->gpr[4];
    uint32_t ea   = (uint32_t)ctx->gpr[5];
    uint32_t size = (uint32_t)ctx->gpr[6];
    yz_rsx_iomap_ensure_init();
    for (uint32_t off = 0; off < size; off += 0x100000u) {
        uint32_t page = (io + off) >> 20;
        if (page < 4096) g_rsx_iomap_ea[page] = ea + off;
    }
    fprintf(stderr, "[sys_rsx] context_iomap io=0x%X ea=0x%X size=0x%X\n", io, ea, size);
    return 0;
}

/* 673 (0x2A1) sys_rsx_context_iounmap(context_id, io, size) */
extern "C" int64_t yz_sys_rsx_context_iounmap(ppu_context* ctx)
{
    uint32_t io   = (uint32_t)ctx->gpr[4];
    uint32_t size = (uint32_t)ctx->gpr[5];
    yz_rsx_iomap_ensure_init();
    for (uint32_t off = 0; off < size; off += 0x100000u) {
        uint32_t page = (io + off) >> 20;
        if (page < 4096) g_rsx_iomap_ea[page] = 0xFFFFFFFFu;
    }
    return 0;
}

/* 674 (0x2A2) sys_rsx_context_attribute(context_id, package_id, a3,a4,a5,a6).
 * The workhorse: flip / display-buffer / queue / flip-reset / vblank / tile /
 * zcull all funnel through here (RPCS3 sys_rsx.cpp:505). */
extern "C" int64_t yz_sys_rsx_context_attribute(ppu_context* ctx)
{
    uint32_t pkg = (uint32_t)ctx->gpr[4];
    uint64_t a3 = ctx->gpr[5], a4 = ctx->gpr[6], a5 = ctx->gpr[7];
    (void)ctx->gpr[8];   /* a6 (tile/zcull status, unused for now) */

    switch (pkg) {
    case 0x001: {       /* FIFO: set get/put */
        vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, (uint32_t)a3);
        vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_PUT, (uint32_t)a4);
        break;
    }
    case 0x100: break;  /* display mode set */
    case 0x101: break;  /* display sync set (vsync pref) */
    case 0x102: {       /* Display flip */
        uint32_t head = (uint32_t)a3 & 7;
        { static int n = 0; if (n < 4) { n++;
            fprintf(stderr, "[sys_rsx] FLIP head=%u a4=0x%llX\n",
                    head, (unsigned long long)a4); } }
        uint32_t flip_idx;
        if (a4 & 0x80000000u) {           /* grab the queued buffer */
            flip_idx = vm_read32(yz_rsx_head_addr(head) + 0x14); /* lastQueuedBufferId */
        } else {                          /* a4 = display buffer offset */
            flip_idx = 0;
            for (uint32_t i = 0; i < g_rsx_dispbuf_count; i++)
                if (g_rsx_dispbuf[i].offset == (uint32_t)a4) { flip_idx = i; break; }
        }
        yz_rsx_present(flip_idx);         /* show the frame in the window */
        /* Mark the flip pending; the vblank tick publishes the done bit so it
         * survives the game's cellGcmResetFlipStatus ordering. */
        InterlockedExchange(&g_rsx_flip_pending[head], 1);
        break;
    }
    case 0x103: {       /* Display queue */
        uint32_t head = (uint32_t)a3 & 7;
        uint32_t ha = yz_rsx_head_addr(head);
        vm_write32(ha + 0x14, (uint32_t)a4);   /* lastQueuedBufferId */
        vm_write32(ha + 0x08, vm_read32(ha + 0x08) | 0x40000000u
                              | (1u << ((uint32_t)a4 & 31)));
        break;
    }
    case 0x104: {       /* Display buffer registration */
        uint32_t id = (uint32_t)a3 & 0xFF;
        if (id < 8) {
            g_rsx_dispbuf[id].width  = (uint32_t)(a4 >> 32);
            g_rsx_dispbuf[id].height = (uint32_t)(a4 & 0xFFFFFFFF);
            g_rsx_dispbuf[id].pitch  = (uint32_t)(a5 >> 32);
            g_rsx_dispbuf[id].offset = (uint32_t)(a5 & 0xFFFFFFFF);
            if (id + 1 > g_rsx_dispbuf_count) g_rsx_dispbuf_count = id + 1;
        }
        break;
    }
    case 0x10A: {       /* flip-status reset (cellGcmResetFlipStatus) */
        uint32_t head = (uint32_t)a3 & 7;
        uint32_t ha = yz_rsx_head_addr(head);
        vm_write32(ha + 0x08, (vm_read32(ha + 0x08) & (uint32_t)a4) | (uint32_t)a5);
        break;
    }
    case 0xFEC: {       /* flip event notification (mark done immediately) */
        uint32_t head = (uint32_t)a3 & 7;
        uint32_t ha = yz_rsx_head_addr(head);
        vm_write32(ha + 0x08, vm_read32(ha + 0x08) | 0x80000000u);
        vm_write64(ha + 0x00, (uint64_t)GetTickCount() * 1000);  /* lastFlipTime */
        break;
    }
    case 0xFED: break;  /* vblank command (our yz_rsx_vblank_tick drives vblank) */
    default: {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[sys_rsx] context_attribute package 0x%X (nop)\n", pkg);
        }
        break;
    }
    }
    return 0;
}

/* 675 (0x2A3) sys_rsx_device_map(dev_addr*, a2*, dev_id) */
extern "C" int64_t yz_sys_rsx_device_map(ppu_context* ctx)
{
    uint32_t p_dev = (uint32_t)ctx->gpr[3];   /* u64 out */
    VirtualAlloc(vm_base + RSX_DEVICE_ADDR, 0x100000, MEM_COMMIT, PAGE_READWRITE);
    /* device+0x30 = 1: initial HW flip credit set at RSX init (RSXThread.cpp:2487
     * thread::init). The first flip's ACQUIRE device+0x30==1 waits on this. */
    vm_write32(RSX_DEVICE_ADDR + 0x30, 1);
    if (p_dev) vm_write64(p_dev, RSX_DEVICE_ADDR);
    fprintf(stderr, "[sys_rsx] device_map dev_id=0x%X -> 0x%08X\n",
            (uint32_t)ctx->gpr[5], RSX_DEVICE_ADDR);
    return 0;
}

/* 676 (0x2A4) sys_rsx_device_unmap(dev_id) */
extern "C" int64_t yz_sys_rsx_device_unmap(ppu_context* ctx) { return 0; }

/* 677 (0x2A5) sys_rsx_attribute(packageId, a2, a3, a4, a5) */
extern "C" int64_t yz_sys_rsx_attribute(ppu_context* ctx) { return 0; }

/* Host vblank tick (called ~62 Hz by main.cpp's yz_vblank_thread). Bumps the
 * per-head vBlankCount/time and publishes any pending flip's completion (the
 * done bit the game's render loop polls inline). */
extern "C" void yz_rsx_vblank_tick(void)
{
    if (!g_rsx_ctx_ready) return;

    /* DIAG (one-time, vblank-dispatch hunt): dump Sony's libgcm vblank/flip
     * handler table once the game has registered a handler. The lifted intr-
     * thread dispatcher (libgcm 0x021082D8) reads the table from
     * r29 = *(libgcm_toc - 0x7FB8) = *(0x0210C048), then calls the handler OPDs
     * at [r29 + 0x08/0x0C/0x10/0x14/0x20/0x24/0x28] gated on cause bits. Show
     * which slots are non-null (registered) and their entry points. */
    { static int dumped = 0;
      uint32_t handlers = vm_read32(RSX_DRIVER_INFO + 0x12C0);
      if (!dumped && handlers) {
          dumped = 1;
          uint32_t tbl = vm_read32(0x0210C048u);
          fprintf(stderr, "[diag] libgcm handler-table ptr=0x%08X handlers=0x%X\n", tbl, handlers);
          if (tbl >= 0x02000000u && tbl < 0x02140000u) {
              const uint32_t offs[7] = {0x08,0x0C,0x10,0x14,0x20,0x24,0x28};
              for (int i = 0; i < 7; i++) {
                  uint32_t opd = vm_read32(tbl + offs[i]);
                  uint32_t ent = (opd >= 0x10000u && opd < 0x02140000u) ? vm_read32(opd) : 0;
                  fprintf(stderr, "[diag]   slot+0x%02X opd=0x%08X entry=0x%08X\n",
                          offs[i], opd, ent);
              }
          }
          fflush(stderr);
      }
    }

    /* DIAG (TEMP): heartbeat -- proves the vblank thread is live and shows the
     * flip-completion state (pending bits + the flip label the consumer waits
     * on). Once/sec. */
    { static unsigned vt = 0;
      if ((vt++ & 63u) == 0)
          fprintf(stderr, "[vbl] tick=%u pending=[%ld %ld] label@0x10200010=0x%08X qhead=%u\n",
                  vt, g_rsx_flip_pending[0], g_rsx_flip_pending[1],
                  vm_read32(RSX_REPORTS + 0x10), g_rsx_queued_head); }

    /* SPURS DISPATCH DIAG (env YZ_TASK_TRACE, 2026-06-16b): once the game has
     * created its render task, dump the SPURS instance's workload-ready state so
     * we can tell if CreateTask2 set wklReadyCount1 (kernel schedules a workload
     * when wklReadyCount1[wid] > 0). taskset->spurs is at +0x60 (Taskset2) or
     * +0x78 (JobChain-style); try both. CellSpurs: wklReadyCount1[16]@0x00,
     * wklEnabled@0xB0, wklStatus1@0x90. */
    /* COHERENCE TEST (env YZ_FORCE_RC, every vblank): bump the taskset workload's
     * wklReadyCount1 INSIDE the SPU lock-line so it survives the kernel's PUTLLC.
     * The task is already pending_ready; if the kernel then schedules wid -> the
     * policy promotes pending->ready and runs gs_task. Confirms the missing
     * bootstrap step is the (coherent) readyCount bump CreateTask2 should do. */
    if (g_yz_spurs_taskset && getenv("YZ_FORCE_RC")) {
        uint32_t ts = g_yz_spurs_taskset, sp = vm_read32(ts + 0x64u);
        uint32_t wid = vm_read32(ts + 0x74u) & 0xFu;
        if (sp >= 0x10000u && sp < 0xE0000000u) {
            /* wklReadyCount1[wid] = 1 -- the kernel scheduling gate (select needs
             * readyCount > contention; cellSpursSpu.cpp:521). NOTE: use vm_write8
             * directly -- it already serializes through the lock-line in
             * VM_WRITE_COH. Wrapping in spu_lockline_lock() nests the
             * non-recursive lock and DEADLOCKS the vblank thread (the old bug that
             * made this force silently never land -> readyCount stayed 0). */
            uint8_t rc_before = vm_read8(sp + (wid & 0xFu));
            vm_write8(sp + (wid & 0xFu), 1);
            uint8_t rc_after = vm_read8(sp + (wid & 0xFu));
            /* re-kick the system service to re-scan workloads (sysSrvMessage@0x72,
             * sysSrvMsgUpdateWorkload@0xBD): set the low 5 SPU bits. */
            vm_write8(sp + 0x72u, (uint8_t)(vm_read8(sp + 0x72u) | 0x1Fu));
            vm_write8(sp + 0xBDu, (uint8_t)(vm_read8(sp + 0xBDu) | 0x1Fu));
            { static unsigned fc=0; if((fc++ & 63u)==0)
                fprintf(stderr, "[force-rc] sp=0x%08X wid=%u readyCount[wid] %u->%u (then re-read %u)\n",
                        sp, wid, rc_before, rc_after, vm_read8(sp + (wid & 0xFu))); }
        }
    }

    if (g_yz_spurs_taskset && getenv("YZ_TASK_TRACE")) {
        static unsigned dt = 0;
        if ((dt++ & 63u) == 0) {
            uint32_t ts = g_yz_spurs_taskset;
            uint32_t sp = vm_read32(ts + 0x64u);            /* CellSpursTaskset2.spurs (+0x60 be64) */
            uint32_t wid = vm_read32(ts + 0x74u);           /* taskset workload id */
            /* CellSpursTaskset2: running_set@0x00 ready_set@0x10 enabled_set@0x30 */
            uint32_t run0 = vm_read32(ts + 0x00u), rdy0 = vm_read32(ts + 0x10u);
            uint32_t pnd0 = vm_read32(ts + 0x20u);   /* pending_ready */
            uint32_t ena0 = vm_read32(ts + 0x30u), sig0 = vm_read32(ts + 0x40u);
            uint32_t rc0 = (sp>=0x10000u&&sp<0xE0000000u) ? vm_read32(sp + 0x00u) : 0;
            uint32_t rc4 = (sp>=0x10000u&&sp<0xE0000000u) ? vm_read32(sp + 0x04u) : 0;
            uint32_t en  = (sp>=0x10000u&&sp<0xE0000000u) ? vm_read32(sp + 0xB0u) : 0;
            uint32_t rcw32 = (sp>=0x10000u&&sp<0xE0000000u) ? vm_read32(sp + ((wid & 0xF) & ~3u)) : 0;
            uint8_t  rcwid = (uint8_t)(rcw32 >> (8u * (3u - ((wid & 0xF) & 3u))));   /* wklReadyCount1[wid], BE */
            /* wklInfo1[wid] @ SPURS+0xB00+wid*0x20: addr(be64)@+0x00, size@+0x10. Is
             * the taskset's WORKLOAD IMAGE (policy module) registered? */
            uint32_t wi = sp + 0xB00u + (wid & 0xF) * 0x20u;
            uint32_t wiAddr = (sp>=0x10000u&&sp<0xE0000000u) ? vm_read32(wi + 0x04u) : 0;
            uint32_t wiSize = (sp>=0x10000u&&sp<0xE0000000u) ? vm_read32(wi + 0x10u) : 0;
            uint8_t  wiUid  = (sp>=0x10000u&&sp<0xE0000000u) ? (uint8_t)(vm_read32(wi + 0x14u) >> 24) : 0;
            fprintf(stderr, "[spurs]   wklInfo1[%u]: addr=0x%08X size=0x%X uniqueId=%u%s\n",
                    wid, wiAddr, wiSize, wiUid, wiAddr ? "" : "  <-- NO IMAGE (policy module not registered)");
            /* The kernel schedules wid only if runnable && priority>0 && maxContention>
             * contention (cellSpursSpu.cpp:519). wklCurrentContention@0x20, wklMaxContention
             * @0x50, wklStatus1@0x90, wklState1@0x80 (all u8[16]); wklInfo1[wid].priority@+0x18
             * (8 bytes, per-SPU). Find which gate is unset for the taskset. */
            if (sp>=0x10000u && sp<0xE0000000u) {
                uint8_t curCont = (uint8_t)(vm_read32(sp+0x20u+((wid&0xF)&~3u)) >> (8u*(3u-((wid&0xF)&3u))));
                uint8_t maxCont = (uint8_t)(vm_read32(sp+0x50u+((wid&0xF)&~3u)) >> (8u*(3u-((wid&0xF)&3u))));
                uint8_t status  = (uint8_t)(vm_read32(sp+0x90u+((wid&0xF)&~3u)) >> (8u*(3u-((wid&0xF)&3u))));
                uint8_t state   = (uint8_t)(vm_read32(sp+0x80u+((wid&0xF)&~3u)) >> (8u*(3u-((wid&0xF)&3u))));
                uint32_t prioHi = vm_read32(wi + 0x18u), prioLo = vm_read32(wi + 0x1Cu);
                uint8_t sysMsg = (uint8_t)(vm_read32(sp+0x70u) >> 8);       /* sysSrvMessage@0x72 */
                uint8_t sysUpd = (uint8_t)(vm_read32(sp+0xBCu) >> 16);      /* sysSrvMsgUpdateWorkload@0xBD */
                fprintf(stderr, "[spurs]   GATES wid=%u: maxContention=%u curContention=%u status=0x%02X "
                        "state=0x%02X priority=%08X_%08X | sysSrvMessage=0x%02X sysSrvMsgUpdateWorkload=0x%02X\n",
                        wid, maxCont, curCont, status, state, prioHi, prioLo, sysMsg, sysUpd);
            }
            /* What does the idle SPURS kernel busy-wait on? Show the hottest SPU
             * channels + the GETLLAR re-poll rate (delta since last sample). */
            {
                extern unsigned long g_spu_ch_rd[128], g_spu_ch_cnt[128], g_spu_getllar_n;
                extern uint32_t g_spu_getllar_ea;
                static unsigned long pr[128], pc[128], pg;
                const char* nm[31] = {0}; nm[0]="EvStat"; nm[3]="SigN1"; nm[4]="SigN2";
                nm[8]="RdDec"; nm[13]="MachStat"; nm[29]="RdInMbox";
                fprintf(stderr, "[spurs]   SPU idle-poll: GETLLAR +%lu (last ea=0x%08X) |",
                        g_spu_getllar_n - pg, g_spu_getllar_ea); pg = g_spu_getllar_n;
                for (int ch = 0; ch < 31; ch++) {
                    unsigned long dr = g_spu_ch_rd[ch] - pr[ch], dc = g_spu_ch_cnt[ch] - pc[ch];
                    pr[ch] = g_spu_ch_rd[ch]; pc[ch] = g_spu_ch_cnt[ch];
                    if (dr > 1000 || dc > 1000)
                        fprintf(stderr, " ch%d%s%s%s rd+%lu cnt+%lu", ch, nm[ch]?"(":"",
                                nm[ch]?nm[ch]:"", nm[ch]?")":"", dr, dc);
                }
                fprintf(stderr, "\n");
            }
            fprintf(stderr, "[spurs] ts=0x%08X wid=%u spurs=0x%08X | taskset: running=%08X "
                    "ready=%08X pending_ready=%08X enabled=%08X signalled=%08X | SPURS: "
                    "wklReadyCount1[wid]=%u wklEnabled=0x%08X\n",
                    ts, wid, sp, run0, rdy0, pnd0, ena0, sig0, rcwid, en);
            (void)rc0; (void)rc4;
            /* FORCE-READY PROBE (env YZ_FORCE_TASK): mark the enabled-but-not-ready
             * task ready (taskset.ready_set |= enabled_set) and request an SPU for
             * its workload (wklReadyCount1[wid]=1). If the kernel then schedules wid
             * -> it DMAs the taskset POLICY MODULE to LS and branches -> spu_indirect_
             * branch reports the FIRST unknown branch = the policy image to lift (or
             * runs gs_task entry 0x3050 directly). Confirms readyCount is the gate. */
            if (getenv("YZ_FORCE_TASK") && sp >= 0x10000u && sp < 0xE0000000u && ena0) {
                if (rdy0 != ena0) vm_write32(ts + 0x10u, ena0);   /* ready_set = enabled_set */
                if (rcwid == 0) {
                    uint32_t off = sp + ((wid & 0xF) & ~3u);
                    uint32_t cur = vm_read32(off);
                    uint32_t shift = 8u * (3u - ((wid & 0xF) & 3u));
                    cur = (cur & ~(0xFFu << shift)) | (1u << shift);  /* wklReadyCount1[wid]=1 */
                    vm_write32(off, cur);
                    fprintf(stderr, "[spurs] FORCE-READY: ready_set=enabled, wklReadyCount1[%u]=1\n", wid);
                }
            }
            fflush(stderr);
        }
    }

    uint64_t t = (uint64_t)GetTickCount() * 1000;
    uint64_t flip_ev = 0;
    for (int h = 0; h < 2; h++) {          /* PS3 has 2 active heads */
        uint32_t ha = yz_rsx_head_addr((uint32_t)h);
        vm_write64(ha + 0x30, vm_read64(ha + 0x30) + 1);  /* vBlankCount */
        vm_write32(ha + 0x1C, (uint32_t)t);               /* lastVTimeLow */
        vm_write32(ha + 0x3C, (uint32_t)(t >> 32));       /* lastVTimeHigh */

        /* Retire a queued flip on this head: present it, then run the
         * 0xFEC-equivalent completion (RPCS3 sys_rsx.cpp:856-868) -- set the
         * flip-done flag, stamp flipBufferId/lastFlipTime, and clear the flip
         * semaphore at label+0x10 (16 bytes, as real HW does). Clearing it
         * releases the consumer's `ACQUIRE label+0x10 == 0`. */
        if (InterlockedExchange(&g_rsx_flip_pending[h], 0)) {
            uint32_t buf = vm_read32(ha + 0x14);          /* lastQueuedBufferId */
            { static int n = 0; if (n < 12) { n++;
                fprintf(stderr, "[vbl] FLIP COMPLETE head=%d buf=%u -> clear label@0x10200010\n", h, buf); } }
            yz_rsx_present(buf);
            vm_write32(ha + 0x10, buf);                   /* flipBufferId */
            vm_write32(ha + 0x08, vm_read32(ha + 0x08) | 0x80000000u); /* flip done */
            vm_write64(ha + 0x00, t);                     /* lastFlipTime */
            vm_write64(RSX_REPORTS + 0x10, 0);            /* flip sema (u128) = 0 */
            vm_write64(RSX_REPORTS + 0x18, 0);
            flip_ev |= (uint64_t)(0x8u << 1);             /* SYS_RSX_EVENT_FLIP_BASE<<1 */
        }
    }

    /* PACING EXPERIMENT (env YZ_FORCE_FLIP): the game throttles its render loop on
     * the flip-COMPLETION counter @0x40C00000 (t1/libgcm increments it per real
     * flip; PROVEN 14h: each [vbl] FLIP COMPLETE -> one increment). When the
     * consumer is parked (it can't reach the in-stream DRIVER_QUEUE/flip command
     * past a stopper or an unbuilt list) the counter stalls and t1 wedges in its
     * flip-wait -> it never builds/releases the next frame -> the consumer stays
     * parked: a mutual-timing deadlock. Force the counter forward when the consumer
     * is provably parked (GET frozen, committed work pending, counter stalled) so
     * t1 unblocks and produces the next frame, letting the consumer advance. This
     * tests whether the flip-throttle is THE gate, and (band-aid) advances the boot
     * to the next wall. */
    if (getenv("YZ_FORCE_FLIP")) {
        static uint32_t lf = ~0u, lget = ~0u; static DWORD lf_t = 0, lget_t = 0;
        const DWORD now = GetTickCount();
        const uint32_t fnc = vm_read32(0x40C00000u);
        const uint32_t get = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_GET);
        const uint32_t put = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT) & ~3u;
        if (get != lget) { lget = get; lget_t = now; }
        if (fnc != lf)   { lf  = fnc; lf_t  = now; }
        if (put != get && (now - lget_t) > 150u && (now - lf_t) > 150u) {
            vm_write32(0x40C00000u, fnc + 1);
            lf = fnc + 1; lf_t = now;
            static int n = 0; if (n < 60) { n++;
                fprintf(stderr, "[vbl] FORCE-FLIP fence %u->%u (consumer parked GET=0x%X PUT=0x%X)\n",
                        fnc, fnc + 1, get, put); }
        }
    }

    /* EXPERIMENT (env YZ_FORCE_REF, 2026-06-16): t1's ACTUAL render-loop throttle is
     * func_00EAC46C, which spins `while [[flip_obj+0x398]] + 2 <= target` -- it polls the
     * counter at [[0x0164FE78]] (flip_obj=0x0164FAE0, a deterministic game-heap object),
     * NOT the fence @0x40C00000 that YZ_FORCE_FLIP forced (which is why force-flip never
     * unblocked t1). Probe + force the REAL counter when the consumer is parked, to test
     * whether advancing it unblocks t1 -> it builds io 0x1104D00 -> reaches the DRAIN
     * (clears S[0x1C], applies io 0x300000's queued release) -> GET advances. If it just
     * relocates the wedge, the throttle is GET-circular (deep pacing); if t1 flows, our
     * runtime is failing to advance the flip/ref-completion counter (a fixable gap). */
    if (getenv("YZ_FORCE_REF")) {
        const uint32_t cptr = vm_read32(0x0164FE78u);          /* [flip_obj+0x398] = counter ptr */
        if (cptr >= 0x10000u && cptr < 0xE0000000u) {
            const uint32_t cnt = vm_read32(cptr);
            { static uint32_t lc = 0xFFFFFFFFu; if (cnt != lc) { lc = cnt;
                fprintf(stderr, "[vbl] ref-counter [0x%08X]=%u\n", cptr, cnt); } }
            static uint32_t lg = ~0u; static DWORD lg_t = 0;
            const DWORD now = GetTickCount();
            const uint32_t get = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_GET);
            const uint32_t put = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT) & ~3u;
            if (get != lg) { lg = get; lg_t = now; }
            if (put != get && (now - lg_t) > 150u) {           /* consumer parked >150ms */
                vm_write32(cptr, cnt + 1);
                lg_t = now;
                static int n = 0; if (n < 80) { n++;
                    fprintf(stderr, "[vbl] FORCE-REF [0x%08X] %u->%u (consumer parked GET=0x%X PUT=0x%X)\n",
                            cptr, cnt, cnt + 1, get, put); }
            }
        }
    }

    /* Deliver the RSX interrupt to Sony's _gcm_intr_thread via the event queue
     * (RPCS3 sys_rsx.cpp send_event): VBLANK every tick + FLIP on completion,
     * masked by driver_info.handlers (@+0x12C0). The intr thread runs the game's
     * registered vblank/flip handler, which advances its render loop (patches the
     * command-buffer pause it left behind). Without this, the handler never fires
     * and the game deadlocks waiting on its own flip fence. */
    if (g_rsx_event_port) {
        uint32_t handlers = vm_read32(RSX_DRIVER_INFO + 0x12C0);
        { static uint32_t lh = 0xFFFFFFFFu; if (handlers != lh) { lh = handlers;
            fprintf(stderr, "[sys_rsx] driver_info.handlers=0x%08X\n", handlers); } }
        /* On a flip, deliver ALL registered bits (Sony's LLE bit assignment is
         * not RPCS3's HLE enum -- handlers=0x6 => vblank bit1 + flip bit2); on a
         * plain vblank, just VBLANK (0x2). */
        uint64_t ev = (flip_ev ? (uint64_t)handlers : ((uint64_t)0x2 & handlers));
        if (ev) {
            ppu_context sc; memset(&sc, 0, sizeof(sc));
            sc.gpr[3] = g_rsx_event_port; sc.gpr[5] = ev;
            int64_t r = sys_event_port_send(&sc);
            /* Log the result periodically: r==EBUSY (-0x... ) means the queue is
             * full -> the _gcm_intr_thread is NOT draining (the real problem). */
            static int n = 0; if (n < 8) { n++;
                fprintf(stderr, "[sys_rsx] vblank event ev=0x%llX -> send=%lld\n",
                        (unsigned long long)ev, (long long)r); }
        }
    }
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
