/*
 * ps3recomp - cellGcmSys HLE module implementation
 *
 * Follows the libgcm SDK contract: prototypes and export surface, error
 * values (0x80210xxx), struct layouts, enum values and INIT_STATE_OFFSET.
 *
 * Design: all guest-visible state (control registers, labels, reports,
 * notify, offset tables, packed tile/zcull/display info arrays) lives in
 * GUEST memory so pointer-returning entry points hand back real guest EAs
 * through the bridge, never host pointers. Regions are placed in the
 * RSX-reserved VM window (runtime/memory/vm.h VM_RSX_BASE) at the same
 * offsets the Yakuza runner in yakuza/import_overrides.cpp uses
 * (RSX_DMA_CONTROL / RSX_REPORTS), so the two implementations agree on
 * addresses by construction.
 *
 * The initialization, control-register, io-map, flip and address entries
 * are overridden by the context-aware implementations in
 * yakuza/import_overrides.cpp (gen_imports.py OVERRIDES); three of those
 * (cellGcmSetDisplayBuffer, cellGcmGetTiledPitchSize,
 * cellGcmGetTimeStampLocation) delegate back here, and the remaining
 * imports use this file directly. Guest-memory access is gated on
 * s_gcm_initialized, which this file's cellGcmInit sets. The runner's
 * _cellGcmInitBody calls cellGcmInit and then builds the RSX context, which
 * resets the control registers and the reports block, so the delegated
 * functions run in the initialized state.
 */

#include "cellGcmSys.h"
#include "ps3emu/endian.h"
#include "ps3emu/guest_call.h"
#include "../../runtime/memory/vm.h"        /* vm_base, vm_commit */
#include "../../runtime/ppu/ppu_memory.h"   /* BE guest accessors */

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

/* ---------------------------------------------------------------------------
 * Timestamp helpers (same pattern as sys_timer)
 * -----------------------------------------------------------------------*/

#ifdef _WIN32
static LARGE_INTEGER s_qpc_freq;
static int           s_qpc_init = 0;

static void ensure_qpc_init(void)
{
    if (!s_qpc_init) {
        QueryPerformanceFrequency(&s_qpc_freq);
        s_qpc_init = 1;
    }
}

static u64 get_timestamp_ns(void)
{
    LARGE_INTEGER now;
    ensure_qpc_init();
    QueryPerformanceCounter(&now);
    return (u64)((double)now.QuadPart * 1000000000.0 / (double)s_qpc_freq.QuadPart);
}
#else
static u64 get_timestamp_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}
#endif

static u64 get_timestamp_us(void)
{
    return get_timestamp_ns() / 1000ULL;
}

/* ---------------------------------------------------------------------------
 * Guest memory layout
 *
 * Local memory: EA 0xC0000000, app-visible size 249MB (0x0F900000), the
 * real-hardware value of the 256MB local pool (matches yakuza_runner.h
 * YZ_GCM_LOCAL_BASE/SIZE).
 *
 * HLE state region: inside the RSX VM window (vm.h: 0x10000000, reserved
 * not committed until used). Offsets mirror the Yakuza runner
 * (yakuza/import_overrides.cpp RSX_DMA_CONTROL = base+0, control regs at
 * +0x40; RSX_REPORTS = base+0x200000, labels at idx*0x10).
 * -----------------------------------------------------------------------*/

#define GCM_LOCAL_BASE_EA    0xC0000000u
#define GCM_LOCAL_SIZE       0x0F900000u   /* 249 MB app-visible */
#define GCM_IO_SPACE_SIZE    0x10000000u   /* 256 MB IO address space */
#define GCM_IO_PAGES         (GCM_IO_SPACE_SIZE >> 20)          /* 256 */
#define GCM_EA_PAGES         (GCM_LOCAL_BASE_EA >> 20)          /* 3072 */

#define GCM_STATE_BASE_EA    0x10000000u                 /* == VM_RSX_BASE */
#define GCM_CONTROL_EA       (GCM_STATE_BASE_EA + 0x40u) /* put/get/ref */
/* Reports region layout matches the runner's sys_rsx context_allocate
 * (import_overrides.cpp, matching the driver_info offsets it publishes):
 * semaphores/labels at +0 (256 x 16B),
 * notify at +0x1000 (64 x 16B), report slots at +0x1400 (2048 x 16B). */
#define GCM_REPORTS_EA       (GCM_STATE_BASE_EA + 0x200000u)
#define GCM_LABEL_EA         GCM_REPORTS_EA                  /* 256 x 0x10 */
#define GCM_REPORT_LOCAL_EA  (GCM_REPORTS_EA + 0x1400u)      /* 2048 x 0x10 */
/* HLE-private guest areas: placed above the runner's reports block
 * (0x10200000 + 0x9400) so the two never write over each other when the
 * Yakuza runner calls into this file. */
#define GCM_IOTABLE_EA       (GCM_STATE_BASE_EA + 0x20A000u) /* u16[3072] */
#define GCM_EATABLE_EA       (GCM_STATE_BASE_EA + 0x20B800u) /* u16[256]  */
#define GCM_TILEINFO_EA      (GCM_STATE_BASE_EA + 0x20BC00u) /* 15 x 16B  */
#define GCM_ZCULLINFO_EA     (GCM_STATE_BASE_EA + 0x20BD00u) /* 8 x 24B   */
#define GCM_DISPINFO_EA      (GCM_STATE_BASE_EA + 0x20BE00u) /* 8 x 16B   */
#define GCM_DEFCTX_EA        (GCM_STATE_BASE_EA + 0x20BF00u) /* 16B       */
#define GCM_STATE_COMMIT_SZ  0x210000u

/* Main-memory report window: io 0x0e000000 - 0x0f000000 */
#define GCM_REPORT_MAIN_IO   0x0E000000u

/* ---------------------------------------------------------------------------
 * Internal state (host-side bookkeeping; guest-visible data lives in vm)
 * -----------------------------------------------------------------------*/

static int  s_gcm_initialized = 0;
static u32  s_flip_mode   = CELL_GCM_DISPLAY_VSYNC;
static u32  s_flip_status = CELL_GCM_DISPLAY_FLIP_STATUS_DONE;
static s32  s_debug_level = CELL_GCM_DEBUG_LEVEL0;

/* Configuration mirror (guest copy written on demand by GetConfiguration) */
static u32 s_cfg_io_address = 0;
static u32 s_cfg_io_size    = 0;

/* Display buffers (host mirror of the guest DisplayInfo array) */
static CellGcmDisplayInfo s_display_buffers[CELL_GCM_MAX_DISPLAY_BUFFER_NUM];
static int s_display_buffer_set[CELL_GCM_MAX_DISPLAY_BUFFER_NUM];
static u32 s_current_display_buffer_id = 0;

/* Prepare-flip registry: flip id -> buffer id */
static int s_flip_qid_set[CELL_GCM_MAX_DISPLAY_BUFFER_NUM];
static u32 s_flip_qid_buffer[CELL_GCM_MAX_DISPLAY_BUFFER_NUM];

/* Queued flip (retires at vblank timing; the RSX-side wait-label condition
 * is checked at retire time; the HLE never force-writes the awaited label) */
static int s_flip_queued = 0;
static u32 s_flip_queued_buffer = 0;
static int s_flip_wait_armed = 0;
static u32 s_flip_wait_index = 0;
static u32 s_flip_wait_value = 0;

/* IO mapping registry. 256 slots = one per 1MB page of the 256MB IO window,
 * so the registry itself can never be the limiting resource. */
typedef struct IoMapping {
    u32 ea;
    u32 io;
    u32 size;
    int active;
} IoMapping;
#define GCM_MAX_IO_MAPPINGS 256
static IoMapping s_io_mappings[GCM_MAX_IO_MAPPINGS];

/* IO map reservation: reserved bytes at the end of the IO space */
static u32 s_io_map_reserved = 0;

/* Callback handlers -- guest OPD addresses (dispatch via g_ps3_guest_caller) */
static u32 s_flip_handler_opd     = 0;
static u32 s_vblank_handler_opd   = 0;
static u32 s_user_handler_opd     = 0;
static u32 s_second_v_handler_opd = 0;
static u32 s_graphics_handler_opd = 0;
static u32 s_queue_handler_opd    = 0;

/* Flip timing (microseconds) */
static u64 s_last_flip_time_us = 0;

/* VBlank counter: u64, incremented by the host vblank tick only (an
 * increment on read would make guest polling loops exit immediately) */
static u64 s_vblank_count = 0;

/* Default FIFO (word sizes). Defaults: 64KB command buffer (the minimum
 * cmdSize) / 32KB segment (the default segment size). */
static u32 s_default_fifo_words    = 0x4000;
static u32 s_default_segment_words = 0x2000;
static s32 s_default_fifo_mode     = CELL_GCM_DEFAULT_FIFO_MODE_TRADITIONAL;

/* Frequencies */
static u32 s_second_v_frequency = 0;
static u32 s_vblank_frequency   = 0;

/* Local-memory map state */
static int s_local_memory_mapped = 0;

/* Tile / zcull host mirrors (raw parameters + bound flags; the packed
 * guest-visible arrays live at GCM_TILEINFO_EA / GCM_ZCULLINFO_EA) */
typedef struct TileRaw {
    u8  location;
    u32 offset, size, pitch;
    u8  comp, bank;
    u16 base;
    int set, bound;
} TileRaw;
static TileRaw s_tiles[CELL_GCM_MAX_TILE_COUNT];
static int s_zcull_bound[CELL_GCM_MAX_ZCULL_COUNT];

/* ---------------------------------------------------------------------------
 * Internal helpers
 * -----------------------------------------------------------------------*/

static int gcm_vm_ready(void)
{
    return vm_base != NULL && s_gcm_initialized;
}

/* --- control register (guest BE u32 put/get/ref at GCM_CONTROL_EA) ------ */

static void gcm_ctrl_write(u32 off, u32 val)
{
    if (gcm_vm_ready())
        vm_write32(GCM_CONTROL_EA + off, val);
}

static u32 gcm_ctrl_read(u32 off)
{
    return gcm_vm_ready() ? vm_read32(GCM_CONTROL_EA + off) : 0;
}

/* get/ref advancement: the control plane belongs to the FIFO consumer. In the
 * Yakuza runner the consumer in yakuza/import_overrides.cpp advances get, so
 * this file must never write it outside cellGcmInit (a forced get=put would
 * skip unconsumed commands out from under that consumer). In a build without
 * the runner the command processor publishes get itself and posts processed
 * SET_REFERENCE values through cellGcmHleSetReference so documented
 * cellGcmFinish-style spins on ctrl->ref terminate. */
void cellGcmHleSetReference(u32 value)
{
    gcm_ctrl_write(8, value);
}

/* --- offset tables (guest BE u16 arrays, 0xFFFF = unmapped) ------------- */

static void gcm_iotable_set(u32 ea_page, u16 v)
{
    if (ea_page < GCM_EA_PAGES)
        vm_write16(GCM_IOTABLE_EA + ea_page * 2, v);
}

static u16 gcm_iotable_get(u32 ea_page)
{
    return (ea_page < GCM_EA_PAGES) ? vm_read16(GCM_IOTABLE_EA + ea_page * 2)
                                    : 0xFFFF;
}

static void gcm_eatable_set(u32 io_page, u16 v)
{
    if (io_page < GCM_IO_PAGES)
        vm_write16(GCM_EATABLE_EA + io_page * 2, v);
}

static u16 gcm_eatable_get(u32 io_page)
{
    return (io_page < GCM_IO_PAGES) ? vm_read16(GCM_EATABLE_EA + io_page * 2)
                                    : 0xFFFF;
}

static void gcm_tables_reset(void)
{
    for (u32 p = 0; p < GCM_EA_PAGES; p++) gcm_iotable_set(p, 0xFFFF);
    for (u32 p = 0; p < GCM_IO_PAGES; p++) gcm_eatable_set(p, 0xFFFF);
}

/* Register one mapping in both tables (EA side is last-writer-wins:
 * multiple IO ranges may map one EA and the EA->IO table keeps the last
 * update). */
static void gcm_tables_map(u32 ea, u32 io, u32 size)
{
    u32 pages = size >> 20;
    for (u32 i = 0; i < pages; i++) {
        gcm_iotable_set((ea >> 20) + i, (u16)((io >> 20) + i));
        gcm_eatable_set((io >> 20) + i, (u16)((ea >> 20) + i));
    }
}

static void gcm_tables_unmap(u32 ea, u32 io, u32 size)
{
    u32 pages = size >> 20;
    for (u32 i = 0; i < pages; i++) {
        /* EA side may have been overwritten by a later mapping of the same
         * EA to another IO range; only clear if it still points here. */
        if (gcm_iotable_get((ea >> 20) + i) == (u16)((io >> 20) + i))
            gcm_iotable_set((ea >> 20) + i, 0xFFFF);
        gcm_eatable_set((io >> 20) + i, 0xFFFF);
    }
}

static IoMapping* gcm_mapping_by_ea(u32 ea)
{
    for (int i = 0; i < GCM_MAX_IO_MAPPINGS; i++)
        if (s_io_mappings[i].active && s_io_mappings[i].ea == ea)
            return &s_io_mappings[i];
    return NULL;
}

static IoMapping* gcm_mapping_by_io(u32 io)
{
    for (int i = 0; i < GCM_MAX_IO_MAPPINGS; i++)
        if (s_io_mappings[i].active && s_io_mappings[i].io == io)
            return &s_io_mappings[i];
    return NULL;
}

static IoMapping* gcm_mapping_alloc(void)
{
    for (int i = 0; i < GCM_MAX_IO_MAPPINGS; i++)
        if (!s_io_mappings[i].active)
            return &s_io_mappings[i];
    return NULL;
}

/* Highest usable IO page (the reserved area sits at the end of the IO space) */
static u32 gcm_io_limit_pages(void)
{
    return (GCM_IO_SPACE_SIZE - s_io_map_reserved) >> 20;
}

static int gcm_io_range_free(u32 io_page, u32 pages)
{
    if (io_page + pages > gcm_io_limit_pages())
        return 0;
    for (u32 i = 0; i < pages; i++)
        if (gcm_eatable_get(io_page + i) != 0xFFFF)
            return 0;
    return 1;
}

/* Resolve an io offset to a guest EA via the io->ea table (1MB pages) */
static u32 gcm_io_to_ea(u32 io)
{
    u16 ea_page = gcm_eatable_get(io >> 20);
    if (ea_page == 0xFFFF)
        return 0;
    return ((u32)ea_page << 20) | (io & 0xFFFFF);
}

/* --- valid tiled pitches ------------------------------------------------ */

static const u32 s_valid_pitches[] = {
    0x0200, 0x0300, 0x0400, 0x0500, 0x0600, 0x0700, 0x0800,
    0x0A00, 0x0C00, 0x0D00, 0x0E00, 0x1000, 0x1400, 0x1800,
    0x1A00, 0x1C00, 0x2000, 0x2800, 0x3000, 0x3400, 0x3800,
    0x4000, 0x5000, 0x6000, 0x6800, 0x7000, 0x8000, 0xA000,
    0xC000, 0xD000, 0xE000, 0x10000
};
static const int s_valid_pitch_count =
    (int)(sizeof(s_valid_pitches) / sizeof(s_valid_pitches[0]));

/* --- flip execution ----------------------------------------------------- */

static void gcm_fire_flip_handler(void)
{
    if (s_flip_handler_opd && g_ps3_guest_caller)
        g_ps3_guest_caller(s_flip_handler_opd, 0, 0, 0, 0, 0, 0, 0, 0);
}

/* Retire the queued flip if its wait-label condition is satisfied. This
 * never force-writes the label: if nothing ever writes it, the flip stays queued
 * exactly like the real RSX would keep waiting. */
static void gcm_try_retire_flip(void)
{
    if (!s_flip_queued)
        return;

    if (s_flip_wait_armed && gcm_vm_ready()) {
        u32 cur = vm_read32(GCM_LABEL_EA +
                            (s_flip_wait_index & 0xFF) * CELL_GCM_LABEL_STRIDE);
        if (cur != s_flip_wait_value)
            return;                      /* label not written yet: keep waiting */
    }

    s_flip_queued = 0;
    s_flip_wait_armed = 0;
    s_current_display_buffer_id = s_flip_queued_buffer;
    s_flip_status = CELL_GCM_DISPLAY_FLIP_STATUS_DONE;
    s_last_flip_time_us = get_timestamp_us();
    gcm_fire_flip_handler();
}

static s32 gcm_queue_flip(u32 id, int wait, u32 labelindex, u32 labelvalue)
{
    /* Invalid buffer ID -> CELL_GCM_ERROR_FAILURE */
    if (id >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM || !s_display_buffer_set[id])
        return CELL_GCM_ERROR_FAILURE;

    s_flip_queued = 1;
    s_flip_queued_buffer = id;
    s_flip_wait_armed = wait;
    s_flip_wait_index = labelindex;
    s_flip_wait_value = labelvalue;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Initialization
 * -----------------------------------------------------------------------*/

s32 cellGcmInit(u32 cmdSize, u32 ioSize, u32 ioAddress)
{
    if (s_gcm_initialized)
        return CELL_GCM_ERROR_FAILURE;
    if (!vm_base)
        return CELL_GCM_ERROR_FAILURE;

    /* Validation: ioAddress 1MB-aligned, ioSize a
     * nonzero multiple of 1MB, cmdSize at least 64KB and at least 1KB
     * smaller than ioSize (RSX prefetch); FAILURE otherwise. */
    if (ioAddress == 0 || (ioAddress & 0xFFFFF) != 0)
        return CELL_GCM_ERROR_FAILURE;
    if (ioSize == 0 || (ioSize & 0xFFFFF) != 0)
        return CELL_GCM_ERROR_FAILURE;
    if (cmdSize < 0x10000 || cmdSize > ioSize - 0x400)
        return CELL_GCM_ERROR_FAILURE;
    if (ioAddress >= GCM_LOCAL_BASE_EA ||
        ioAddress + ioSize > GCM_LOCAL_BASE_EA)
        return CELL_GCM_ERROR_FAILURE;

    /* Commit the guest-side HLE state region and local memory */
    if (vm_commit(GCM_STATE_BASE_EA, GCM_STATE_COMMIT_SZ) != CELL_OK)
        return CELL_GCM_ERROR_FAILURE;
    if (vm_commit(GCM_LOCAL_BASE_EA, GCM_LOCAL_SIZE) != CELL_OK)
        return CELL_GCM_ERROR_FAILURE;
    memset(vm_base + GCM_STATE_BASE_EA, 0, GCM_STATE_COMMIT_SZ);

    memset(s_display_buffers, 0, sizeof(s_display_buffers));
    memset(s_display_buffer_set, 0, sizeof(s_display_buffer_set));
    memset(s_flip_qid_set, 0, sizeof(s_flip_qid_set));
    memset(s_flip_qid_buffer, 0, sizeof(s_flip_qid_buffer));
    memset(s_io_mappings, 0, sizeof(s_io_mappings));
    memset(s_tiles, 0, sizeof(s_tiles));
    memset(s_zcull_bound, 0, sizeof(s_zcull_bound));

    s_cfg_io_address = ioAddress;
    s_cfg_io_size    = ioSize;

    s_current_display_buffer_id = 0;
    s_flip_queued = 0;
    s_flip_wait_armed = 0;
    /* Flip status immediately after cellGcmInit is DONE */
    s_flip_status = CELL_GCM_DISPLAY_FLIP_STATUS_DONE;
    s_flip_mode = CELL_GCM_DISPLAY_VSYNC;
    s_debug_level = CELL_GCM_DEBUG_LEVEL0;
    s_vblank_count = 0;
    s_last_flip_time_us = 0;
    s_io_map_reserved = 0;
    s_second_v_frequency = 0;
    s_vblank_frequency = 0;
    s_flip_handler_opd = 0;
    s_vblank_handler_opd = 0;
    s_user_handler_opd = 0;
    s_second_v_handler_opd = 0;
    s_graphics_handler_opd = 0;
    s_queue_handler_opd = 0;

    /* The default command buffer occupies io offsets 0..cmdSize of the
     * mapped region; word sizes reported by GetDefaultCommandWordSize /
     * GetDefaultSegmentWordSize (segment default 32KB). */
    s_default_fifo_words = cmdSize >> 2;

    s_gcm_initialized = 1;   /* vm helpers live from here on */

    /* Offset tables + the initial IO mapping (io 0 = command buffer region,
     * the buffer is mapped from position 0) */
    gcm_tables_reset();
    s_io_mappings[0].ea = ioAddress;
    s_io_mappings[0].io = 0;
    s_io_mappings[0].size = ioSize;
    s_io_mappings[0].active = 1;
    gcm_tables_map(ioAddress, 0, ioSize);

    /* Control registers: put/get idle at the init-state offset 0x1000; ref
     * starts at 0xFFFFFFFF so the documented Finish spin "while (ref != 0)"
     * does not exit before any command has run. */
    gcm_ctrl_write(0, CELL_GCM_INIT_STATE_OFFSET);
    gcm_ctrl_write(4, CELL_GCM_INIT_STATE_OFFSET);
    gcm_ctrl_write(8, 0xFFFFFFFFu);

    /* Default context (guest BE): begin after the fixed 4KB init area;
     * end/current are simplified to the whole default command buffer minus
     * the segment-link word (a simplification of the segment ring). */
    {
        u32 begin = ioAddress + CELL_GCM_INIT_STATE_OFFSET;
        u32 end   = ioAddress + cmdSize - 4;
        vm_write32(GCM_DEFCTX_EA + 0x0, begin);
        vm_write32(GCM_DEFCTX_EA + 0x4, end);
        vm_write32(GCM_DEFCTX_EA + 0x8, begin);
        vm_write32(GCM_DEFCTX_EA + 0xC, 0);
    }

    return CELL_OK;
}

void cellGcmGetConfiguration(CellGcmConfig* config)
{
    if (!config)
        return;

    /* Out-struct lives in guest memory: write big-endian
     * (convention: libs/video/cellVideoOut.c) */
    config->localAddress    = ps3_bswap32(GCM_LOCAL_BASE_EA);
    config->ioAddress       = ps3_bswap32(s_cfg_io_address);
    config->localSize       = ps3_bswap32(GCM_LOCAL_SIZE);
    config->ioSize          = ps3_bswap32(s_cfg_io_size);
    config->memoryFrequency = ps3_bswap32(650000000u);
    config->coreFrequency   = ps3_bswap32(500000000u);
}

/* Returns the guest control-register struct; the bridge
 * converts the host pointer back to a guest EA. The Yakuza runner overrides
 * it (yakuza/yz_gcm_hle.inc). */
CellGcmControl* cellGcmGetControlRegister(void)
{
    if (!gcm_vm_ready())
        return NULL;
    return (CellGcmControl*)(vm_base + GCM_CONTROL_EA);
}

/* ---------------------------------------------------------------------------
 * Display / flip
 * -----------------------------------------------------------------------*/

u32 cellGcmGetCurrentField(void)
{
    return 0;   /* progressive */
}

void cellGcmSetFlipMode(u32 mode)
{
    if (mode >= CELL_GCM_DISPLAY_HSYNC && mode <= CELL_GCM_DISPLAY_HSYNC_WITH_NOISE)
        s_flip_mode = mode;
}

/* HLE model: the wait executes the pending flip if its label condition is
 * satisfied. It does not force the status. */
void cellGcmSetWaitFlip(CellGcmContextData* ctx)
{
    (void)ctx;
    gcm_try_retire_flip();
}

/* Reset to WAITING */
void cellGcmResetFlipStatus(void)
{
    s_flip_status = CELL_GCM_DISPLAY_FLIP_STATUS_WAITING;
}

/* No-arg, sets DONE */
void cellGcmSetFlipStatus(void)
{
    s_flip_status = CELL_GCM_DISPLAY_FLIP_STATUS_DONE;
}

/* Pure read: completion happens at flip retire time, so WAITING can be
 * observed */
u32 cellGcmGetFlipStatus(void)
{
    return s_flip_status;
}

/* Host-side tick: retires queued flips and fires the guest vblank handler.
 * Called once per frame by yz_rsx_vblank_tick (import_overrides.cpp). It must
 * not touch the control registers (owned by the FIFO consumer). */
void cellGcmTickVBlank(void)
{
    s_vblank_count++;
    gcm_try_retire_flip();
    if (s_vblank_handler_opd && g_ps3_guest_caller)
        g_ps3_guest_caller(s_vblank_handler_opd, 0, 0, 0, 0, 0, 0, 0, 0);
}

/* User commands. On hardware the 0xEB00 method raises an interrupt and a
 * separate PPU thread runs the handler while the RSX keeps consuming; lv1
 * keeps one pending cause register, so rapid commands coalesce and the latest
 * wins. The game's user handler tolerates that coalescing. A dedicated host interrupt
 * thread with a latest-wins cause latch models this, so the FIFO consumer
 * never runs guest callback code or blocks on the handler. */
#ifdef _WIN32
static HANDLE s_ucmd_intr_event;
static HANDLE s_ucmd_intr_thread;
static volatile LONG s_ucmd_cause;       /* latest-wins (lv1 coalescing) */
static volatile LONG s_ucmd_has_pending;

static DWORD WINAPI gcm_intr_thread_proc(LPVOID opaque)
{
    (void)opaque;
    for (;;) {
        WaitForSingleObject(s_ucmd_intr_event, INFINITE);
        while (InterlockedExchange(&s_ucmd_has_pending, 0)) {
            const u32 cause =
                (u32)InterlockedCompareExchange(&s_ucmd_cause, 0, 0);
            if (s_user_handler_opd && g_ps3_guest_caller)
                g_ps3_guest_caller(s_user_handler_opd, cause,
                                   0, 0, 0, 0, 0, 0, 0);
        }
    }
    return 0;
}
#endif

void cellGcmDispatchUserCommand(u32 cause)
{
#ifdef _WIN32
    /* Synchronous dispatch is only the fallback when the interrupt thread
     * cannot be created. */
    static int sync_mode = 0;
    if (!sync_mode) {
        if (!s_ucmd_intr_thread) {
            s_ucmd_intr_event = CreateEventA(NULL, FALSE, FALSE, NULL);
            s_ucmd_intr_thread = CreateThread(
                NULL, 0, gcm_intr_thread_proc, NULL, 0, NULL);
            if (!s_ucmd_intr_event || !s_ucmd_intr_thread) {
                fprintf(stderr, "[gcm-intr] thread setup failed; falling "
                        "back to synchronous dispatch\n");
                fflush(stderr);
                sync_mode = 1;
            }
        }
        if (!sync_mode) {
            InterlockedExchange(&s_ucmd_cause, (LONG)cause);
            InterlockedExchange(&s_ucmd_has_pending, 1);
            SetEvent(s_ucmd_intr_event);
            return;
        }
    }
#endif
    if (s_user_handler_opd && g_ps3_guest_caller)
        g_ps3_guest_caller(s_user_handler_opd, cause, 0, 0, 0, 0, 0, 0, 0);
}

/* FAILURE for invalid argument values. Called by the override in
 * yakuza/import_overrides.cpp, so the signature must stay (u32 x5). */
s32 cellGcmSetDisplayBuffer(u32 bufferId, u32 offset, u32 pitch,
                            u32 width, u32 height)
{
    if (bufferId >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM)
        return CELL_GCM_ERROR_FAILURE;

    s_display_buffers[bufferId].offset = offset;
    s_display_buffers[bufferId].pitch  = pitch;
    s_display_buffers[bufferId].width  = width;
    s_display_buffers[bufferId].height = height;
    s_display_buffer_set[bufferId] = 1;

    /* Mirror into the guest DisplayInfo array (values are stored directly).
     * Skipped until cellGcmInit has run. */
    if (gcm_vm_ready()) {
        u32 e = GCM_DISPINFO_EA + bufferId * 16;
        vm_write32(e + 0x0, offset);
        vm_write32(e + 0x4, pitch);
        vm_write32(e + 0x8, width);
        vm_write32(e + 0xC, height);
    }
    return CELL_OK;
}

/* Flip commands take the GCM context as arg0. The context is unused by
 * this HLE (no FIFO here; flips retire via the vblank tick). */
s32 cellGcmSetFlipCommand(CellGcmContextData* ctx, u32 id)
{
    (void)ctx;
    return gcm_queue_flip(id, 0, 0, 0);
}

s32 cellGcmSetFlipCommandWithWaitLabel(CellGcmContextData* ctx, u32 id,
                                       u32 labelindex, u32 labelvalue)
{
    (void)ctx;
    /* Label index range is 64-255 (0-63 system-reserved); the documented
     * error contract only covers the buffer id. */
    return gcm_queue_flip(id, 1, labelindex, labelvalue);
}

s32 _cellGcmSetFlipCommand(CellGcmContextData* ctx, u32 id)
{
    return cellGcmSetFlipCommand(ctx, id);
}

s32 _cellGcmSetFlipCommandWithWaitLabel(CellGcmContextData* ctx, u32 id,
                                        u32 labelindex, u32 labelvalue)
{
    return cellGcmSetFlipCommandWithWaitLabel(ctx, id, labelindex, labelvalue);
}

/* Registers the buffer for a flip id and returns the flip id; the flip
 * itself happens later. */
s32 cellGcmSetPrepareFlip(CellGcmContextData* ctx, u32 id)
{
    (void)ctx;
    if (id >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM || !s_display_buffer_set[id])
        return CELL_GCM_ERROR_FAILURE;

    s_flip_qid_set[id] = 1;
    s_flip_qid_buffer[id] = id;
    return (s32)id;
}

/* Immediate flip (no vsync wait) */
s32 cellGcmSetFlipImmediate(u32 id)
{
    s32 rc = gcm_queue_flip(id, 0, 0, 0);
    if (rc != CELL_OK)
        return rc;
    gcm_try_retire_flip();
    return CELL_OK;
}

/* Returns the display buffer ID for a flip id, or -1 */
s32 cellGcmGetDisplayBufferByFlipIndex(u32 qid)
{
    if (qid >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM || !s_flip_qid_set[qid])
        return -1;
    return (s32)s_flip_qid_buffer[qid];
}

/* Out-param + status return */
s32 cellGcmGetCurrentDisplayBufferId(u8* id)
{
    if (!id)
        return CELL_GCM_ERROR_FAILURE;
    *id = (u8)s_current_display_buffer_id;   /* single byte: no swap */
    return CELL_OK;
}

void cellGcmSetFlipHandler(CellGcmFlipHandler handler)
{
    s_flip_handler_opd = (u32)(size_t)handler;
}

void cellGcmSetVBlankHandler(CellGcmVBlankHandler handler)
{
    s_vblank_handler_opd = (u32)(size_t)handler;
}

void cellGcmSetSecondVHandler(CellGcmSecondVHandler handler)
{
    s_second_v_handler_opd = (u32)(size_t)handler;
}

void cellGcmSetUserHandler(CellGcmUserHandler handler)
{
    s_user_handler_opd = (u32)(size_t)handler;
}

void cellGcmSetGraphicsHandler(CellGcmGraphicsHandler handler)
{
    s_graphics_handler_opd = (u32)(size_t)handler;
}

void cellGcmSetQueueHandler(CellGcmQueueHandler handler)
{
    s_queue_handler_opd = (u32)(size_t)handler;
}

/* Microseconds */
u64 cellGcmGetLastFlipTime(void)
{
    return s_last_flip_time_us;
}

/* u64; counts only at the vblank tick */
u64 cellGcmGetVBlankCount(void)
{
    return s_vblank_count;
}

void cellGcmSetSecondVFrequency(u32 freq)
{
    s_second_v_frequency = freq;
}

void cellGcmSetVBlankFrequency(u32 freq)
{
    s_vblank_frequency = freq;
}

/* Generates a user-command interrupt; the HLE
 * dispatches the registered user handler directly. */
void cellGcmSetUserCommand(CellGcmContextData* ctx, u32 cause)
{
    (void)ctx;
    cellGcmDispatchUserCommand(cause);
}

/* ---------------------------------------------------------------------------
 * Address translation / IO mapping
 * -----------------------------------------------------------------------*/

/* Members are guest EAs of the u16 tables */
void cellGcmGetOffsetTable(CellGcmOffsetTable* table)
{
    if (!table)
        return;
    table->ioAddress = ps3_bswap32(s_gcm_initialized ? GCM_IOTABLE_EA : 0);
    table->eaAddress = ps3_bswap32(s_gcm_initialized ? GCM_EATABLE_EA : 0);
}

/* Local memory or a mapped IO EA; FAILURE otherwise */
s32 cellGcmAddressToOffset(u32 address, u32* offset)
{
    if (!offset)
        return CELL_GCM_ERROR_FAILURE;

    if (address >= GCM_LOCAL_BASE_EA &&
        address < GCM_LOCAL_BASE_EA + GCM_LOCAL_SIZE) {
        *offset = ps3_bswap32(address - GCM_LOCAL_BASE_EA);
        return CELL_OK;
    }

    if (gcm_vm_ready()) {
        u16 io_page = gcm_iotable_get(address >> 20);
        if (io_page != 0xFFFF) {
            *offset = ps3_bswap32(((u32)io_page << 20) | (address & 0xFFFFF));
            return CELL_OK;
        }
    }

    *offset = 0;
    return CELL_GCM_ERROR_FAILURE;
}

/* The allocator picks the io offset; FAILURE on bad
 * alignment, EINVAL on RSX-prohibited areas, NO_IO_PAGE_TABLE when the page
 * table is full. */
s32 cellGcmMapMainMemory(u32 ea, u32 size, u32* offset)
{
    if (!offset || !s_gcm_initialized)
        return CELL_GCM_ERROR_FAILURE;

    if ((ea & 0xFFFFF) != 0 || size == 0 || (size & 0xFFFFF) != 0)
        return CELL_GCM_ERROR_FAILURE;    /* FAILURE, not ALIGNMENT */

    if (ea >= GCM_LOCAL_BASE_EA || ea + size > GCM_LOCAL_BASE_EA)
        return CELL_EINVAL;               /* RSX-prohibited area */

    u32 pages = size >> 20;
    u32 limit = gcm_io_limit_pages();
    u32 found = 0xFFFFFFFFu;
    for (u32 p = 0; p + pages <= limit; p++) {
        if (gcm_io_range_free(p, pages)) {
            found = p;
            break;
        }
    }
    if (found == 0xFFFFFFFFu)
        return CELL_GCM_ERROR_NO_IO_PAGE_TABLE;

    IoMapping* m = gcm_mapping_alloc();
    if (!m)
        return CELL_GCM_ERROR_NO_IO_PAGE_TABLE;

    m->ea = ea;
    m->io = found << 20;
    m->size = size;
    m->active = 1;
    gcm_tables_map(ea, m->io, size);

    *offset = ps3_bswap32(m->io);
    return CELL_OK;
}

/* The caller picks the io offset; FAILURE on bad alignment, EINVAL on
 * prohibited areas. Multiple IO ranges may map the same EA, so there is no
 * EA-overlap error. */
s32 cellGcmMapEaIoAddress(u32 ea, u32 io, u32 size)
{
    if (!s_gcm_initialized)
        return CELL_GCM_ERROR_FAILURE;

    if ((ea & 0xFFFFF) != 0 || (io & 0xFFFFF) != 0 ||
        size == 0 || (size & 0xFFFFF) != 0)
        return CELL_GCM_ERROR_FAILURE;    /* FAILURE, not ALIGNMENT */

    if (ea >= GCM_LOCAL_BASE_EA || ea + size > GCM_LOCAL_BASE_EA)
        return CELL_EINVAL;

    /* Target io range must be inside the unreserved window and unused
     * (reserved areas refuse mapping). */
    if (!gcm_io_range_free(io >> 20, size >> 20))
        return CELL_GCM_ERROR_FAILURE;

    IoMapping* m = gcm_mapping_alloc();
    if (!m)
        return CELL_GCM_ERROR_NO_IO_PAGE_TABLE;

    m->ea = ea;
    m->io = io;
    m->size = size;
    m->active = 1;
    gcm_tables_map(ea, io, size);

    return CELL_OK;
}

/* Must be the mapping's beginning EA; FAILURE otherwise */
s32 cellGcmUnmapEaIoAddress(u32 ea)
{
    IoMapping* m = gcm_mapping_by_ea(ea);
    if (!m || !s_gcm_initialized)
        return CELL_GCM_ERROR_FAILURE;

    gcm_tables_unmap(m->ea, m->io, m->size);
    m->active = 0;
    return CELL_OK;
}

/* Must be the mapping's beginning io offset */
s32 cellGcmUnmapIoAddress(u32 io)
{
    IoMapping* m = gcm_mapping_by_io(io);
    if (!m || !s_gcm_initialized)
        return CELL_GCM_ERROR_FAILURE;

    gcm_tables_unmap(m->ea, m->io, m->size);
    m->active = 0;
    return CELL_OK;
}

/* io -> EA (local memory is not covered); FAILURE if unmapped */
s32 cellGcmIoOffsetToAddress(u32 ioOffset, u32* address)
{
    if (!address)
        return CELL_GCM_ERROR_FAILURE;

    if (gcm_vm_ready()) {
        u32 ea = gcm_io_to_ea(ioOffset);
        if (ea) {
            *address = ps3_bswap32(ea);
            return CELL_OK;
        }
    }
    *address = 0;
    return CELL_GCM_ERROR_FAILURE;
}

/* Compact the IO page table. The HLE reassigns every
 * active mapping a fresh sequential io range; callers re-derive offsets via
 * cellGcmAddressToOffset afterwards (as documented). */
s32 cellGcmSortRemapEaIoAddress(void)
{
    if (!s_gcm_initialized)
        return CELL_GCM_ERROR_FAILURE;

    gcm_tables_reset();

    /* Stable ascending-io order, then repack from io 0 */
    u32 next_io = 0;
    for (;;) {
        IoMapping* best = NULL;
        for (int i = 0; i < GCM_MAX_IO_MAPPINGS; i++) {
            IoMapping* m = &s_io_mappings[i];
            if (!m->active || m->io < next_io)
                continue;
            if (!best || m->io < best->io)
                best = m;
        }
        if (!best)
            break;
        u32 new_io = next_io;
        next_io += best->size;
        best->io = new_io;
        gcm_tables_map(best->ea, best->io, best->size);
    }
    return CELL_OK;
}

s32 cellGcmMapLocalMemory(u32* address, u32* size)
{
    if (!address || !size)
        return CELL_GCM_ERROR_FAILURE;

    if (s_gcm_initialized) {
        /* After cellGcmInit local memory is already mapped: ENOMEM */
        return CELL_ENOMEM;
    }
    if (s_local_memory_mapped)
        return CELL_GCM_ERROR_FAILURE;

    if (vm_base)
        vm_commit(GCM_LOCAL_BASE_EA, GCM_LOCAL_SIZE);

    s_local_memory_mapped = 1;
    *address = ps3_bswap32(GCM_LOCAL_BASE_EA);
    *size    = ps3_bswap32(GCM_LOCAL_SIZE);
    return CELL_OK;
}

/* IO space size minus the reserved area (mapped areas do not reduce it) */
u32 cellGcmGetMaxIoMapSize(void)
{
    return GCM_IO_SPACE_SIZE - s_io_map_reserved;
}

s32 cellGcmReserveIoMapSize(u32 size)
{
    if ((size & 0xFFFFF) != 0)
        return CELL_GCM_ERROR_INVALID_ALIGNMENT;
    if (size > cellGcmGetMaxIoMapSize())
        return CELL_GCM_ERROR_INVALID_VALUE;

    s_io_map_reserved += size;
    return CELL_OK;
}

s32 cellGcmUnreserveIoMapSize(u32 size)
{
    if ((size & 0xFFFFF) != 0)
        return CELL_GCM_ERROR_INVALID_ALIGNMENT;
    if (size > s_io_map_reserved)
        return CELL_GCM_ERROR_INVALID_VALUE;

    s_io_map_reserved -= size;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Label / report / timestamp / notify
 * -----------------------------------------------------------------------*/

/* Labels: 256 x 16B in the reports region (indices 0-63 system-reserved;
 * same base+stride as the runner's cellGcmGetLabelAddress). */
u32* cellGcmGetLabelAddress(u8 index)
{
    if (!gcm_vm_ready())
        return NULL;
    return (u32*)(vm_base + GCM_LABEL_EA + (u32)index * CELL_GCM_LABEL_STRIDE);
}

/* Local report area, indices 0-2047 */
CellGcmReportData* cellGcmGetReportDataAddress(u32 index)
{
    if (index >= CELL_GCM_MAX_REPORT_COUNT || !gcm_vm_ready())
        return NULL;
    return (CellGcmReportData*)(vm_base + GCM_REPORT_LOCAL_EA +
                                index * CELL_GCM_REPORT_DATA_SIZE);
}

/* Resolve a report slot EA for (index, location); 0 when unresolvable.
 * MAIN reports live at io 0x0e000000 + index*16 and reach guest memory only
 * through the caller's io mapping. */
static u32 gcm_report_ea(u32 index, u32 location)
{
    if (!gcm_vm_ready())
        return 0;
    if (location == CELL_GCM_LOCATION_LOCAL) {
        if (index >= CELL_GCM_MAX_REPORT_COUNT)
            return 0;
        return GCM_REPORT_LOCAL_EA + index * CELL_GCM_REPORT_DATA_SIZE;
    }
    if (location == CELL_GCM_LOCATION_MAIN) {
        if (index >= CELL_GCM_MAX_MAIN_REPORT_COUNT)
            return 0;
        return gcm_io_to_ea(GCM_REPORT_MAIN_IO +
                            index * CELL_GCM_REPORT_DATA_SIZE);
    }
    return 0;
}

CellGcmReportData* cellGcmGetReportDataAddressLocation(u32 index, u32 location)
{
    u32 ea = gcm_report_ea(index, location);
    return ea ? (CellGcmReportData*)(vm_base + ea) : NULL;
}

/* Reads the report's value member */
u32 cellGcmGetReportDataLocation(u32 index, u32 location)
{
    u32 ea = gcm_report_ea(index, location);
    return ea ? vm_read32(ea + 8) : 0;
}

/* Local report area only; the HLE keeps one value per slot
 * so type does not select a separate bank */
u32 cellGcmGetReport(u32 type, u32 index)
{
    (void)type;
    return cellGcmGetReportDataLocation(index, CELL_GCM_LOCATION_LOCAL);
}

/* This is a PPU read of the timer the RSX wrote into the report slot, not a
 * request for a fresh host timestamp; the read goes through vm_read64 so the
 * report slot is current. */
u64 cellGcmGetTimeStamp(u32 index)
{
    const u32 ea = gcm_report_ea(index, CELL_GCM_LOCATION_LOCAL);
    return ea ? vm_read64(ea) : 0;
}

/* Same PPU-memory-read contract with a location-dependent slot address. The
 * caller remains responsible for ordering. */
u64 cellGcmGetTimeStampLocation(u32 index, u32 location)
{
    const u32 ea = gcm_report_ea(index, location);
    return ea ? vm_read64(ea) : 0;
}

/* Shared monotonic timebase (ns) for report timers (consumer:
 * rsx_commands.c NV4097_GET_REPORT handler). */
u64 cellGcmReportTimestampNs(void)
{
    return get_timestamp_ns();
}

/* Notify data lives in MAIN memory
 * at io 0x0f100000, 8 slots with a 64-byte stride; NULL when not mapped. */
CellGcmNotifyData* cellGcmGetNotifyDataAddress(u32 index)
{
    if (index >= CELL_GCM_NOTIFY_MAIN_MAX_COUNT || !gcm_vm_ready())
        return NULL;
    u32 ea = gcm_io_to_ea(CELL_GCM_NOTIFY_IO_ADDRESS_BASE +
                          index * CELL_GCM_NOTIFY_MAIN_ALIGN_SIZE);
    return ea ? (CellGcmNotifyData*)(vm_base + ea) : NULL;
}

/* ---------------------------------------------------------------------------
 * Tile / Zcull
 * -----------------------------------------------------------------------*/

/* Write one packed TileInfo entry (formulas in cellGcmSys.h) into the guest
 * array. */
static void gcm_tile_write_packed(u8 index, const TileRaw* t)
{
    if (!gcm_vm_ready())
        return;
    u32 e = GCM_TILEINFO_EA + (u32)index * 16;
    u32 loc = t->location;
    vm_write32(e + 0x0, (loc + 1) | ((u32)t->bank << 4) |
                        ((t->offset / 0x10000u) << 16) | (loc << 31));
    vm_write32(e + 0x4, (((t->offset + t->size - 1) / 0x10000u) << 16) |
                        (loc << 31));
    vm_write32(e + 0x8, (t->pitch / 0x100u) << 8);
    vm_write32(e + 0xC, (u32)t->base |
                        (((u32)t->base + ((t->size - 1) / 0x10000u)) << 13) |
                        ((u32)t->comp << 26) | (1u << 30));
}

static void gcm_tile_clear_packed(u8 index)
{
    if (!gcm_vm_ready())
        return;
    u32 e = GCM_TILEINFO_EA + (u32)index * 16;
    vm_write32(e + 0x0, 0);
    vm_write32(e + 0x4, 0);
    vm_write32(e + 0x8, 0);
    vm_write32(e + 0xC, 0);
}

s32 cellGcmSetTileInfo(u8 index, u8 location, u32 offset, u32 size,
                       u32 pitch, u8 comp, u16 base, u8 bank)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;      /* index 15+ */
    if (base >= 0x800 || bank >= 4)
        return CELL_GCM_ERROR_INVALID_VALUE;
    if ((offset & 0xFFFF) != 0 || (size & 0xFFFF) != 0 || size == 0)
        return CELL_GCM_ERROR_INVALID_ALIGNMENT;  /* 64KB alignment */
    if ((pitch & 0xFF) != 0 || pitch == 0)
        return CELL_GCM_ERROR_INVALID_ALIGNMENT;  /* 256B pitch */
    if (location != CELL_GCM_LOCATION_LOCAL && location != CELL_GCM_LOCATION_MAIN)
        return CELL_GCM_ERROR_INVALID_ENUM;
    if (comp != CELL_GCM_COMPMODE_DISABLED &&
        (comp < CELL_GCM_COMPMODE_C32_2X1 ||
         comp > CELL_GCM_COMPMODE_Z32_SEPSTENCIL_ROTATED))
        return CELL_GCM_ERROR_INVALID_ENUM;

    TileRaw* t = &s_tiles[index];
    t->location = location;
    t->offset = offset;
    t->size = size;
    t->pitch = pitch;
    t->comp = comp;
    t->base = base;
    t->bank = bank;
    t->set = 1;
    /* SetTileInfo only holds the settings; the packed guest entry is what
     * GetTileInfo exposes, so keep it current. */
    gcm_tile_write_packed(index, t);
    return CELL_OK;
}

/* Only the index is validated: this title binds tile 1 [0,0x3C0000) inside
 * already-bound tile 0 [0,0x780000) and runs on real hardware, so overlapping
 * bound tiles are accepted rather than reported as ADDRESS_OVERWRAP. */
s32 cellGcmBindTile(u8 index)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    s_tiles[index].bound = 1;
    return CELL_OK;
}

/* Unbind + clear the packed entry to zeros */
s32 cellGcmUnbindTile(u8 index)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    s_tiles[index].bound = 0;
    s_tiles[index].set = 0;
    gcm_tile_clear_packed(index);
    return CELL_OK;
}

/* Legacy combined setter (void) */
void cellGcmSetTile(u8 index, u8 location, u32 offset, u32 size,
                    u32 pitch, u8 comp, u16 base, u8 bank)
{
    if (cellGcmSetTileInfo(index, location, offset, size,
                           pitch, comp, base, bank) == CELL_OK)
        (void)cellGcmBindTile(index);
}

void cellGcmSetInvalidateTile(u8 index)
{
    (void)cellGcmUnbindTile(index);
}

/* The documented INVALID_VALUE/ALIGNMENT/ENUM checks apply only to the
 * debug-version PRX; the release PRX validates nothing but the bind slot.
 * This matters twice over: (a) release fidelity, and (b) the generated import
 * bridge forwards only 8 GPR args to this 12-parameter export (PPU args 9-12
 * live in the guest stack save area), so the tail parameters may be garbage;
 * rejecting on them would break the call. */
s32 cellGcmBindZcull(u32 index, u32 offset, u32 width, u32 height,
                     u32 cullStart, u32 zFormat, u32 aaFormat,
                     u32 zcullDir, u32 zcullFormat,
                     u32 sFunc, u32 sRef, u32 sMask)
{
    if (index >= CELL_GCM_MAX_ZCULL_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    if (gcm_vm_ready()) {
        u32 e = GCM_ZCULLINFO_EA + index * 24;
        /* Packing formulas: see cellGcmSys.h */
        vm_write32(e + 0x00, (1u << 0) | (zFormat << 4) | (aaFormat << 8));
        vm_write32(e + 0x04, ((width >> 6) << 22) | ((height >> 6) << 6));
        vm_write32(e + 0x08, cullStart & ~0xFFFu);
        vm_write32(e + 0x0C, offset);
        vm_write32(e + 0x10, (zcullDir << 1) | (zcullFormat << 2) |
                             ((sFunc & 0xF) << 12) | (sRef << 16) |
                             (sMask << 24));
        vm_write32(e + 0x14, (0x2000u << 0) | (0x20u << 16)); /* fixed */
    }
    s_zcull_bound[index] = 1;
    return CELL_OK;
}

/* Legacy combined setter (void) */
void cellGcmSetZcull(u8 index, u32 offset, u32 width, u32 height,
                     u32 cullStart, u32 zFormat, u32 aaFormat,
                     u32 zcullDir, u32 zcullFormat,
                     u32 sFunc, u32 sRef, u32 sMask)
{
    (void)cellGcmBindZcull(index, offset, width, height, cullStart,
                           zFormat, aaFormat, zcullDir, zcullFormat,
                           sFunc, sRef, sMask);
}

/* Unbind + clear the packed entry */
s32 cellGcmUnbindZcull(u8 index)
{
    if (index >= CELL_GCM_MAX_ZCULL_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    s_zcull_bound[index] = 0;
    if (gcm_vm_ready()) {
        u32 e = GCM_ZCULLINFO_EA + (u32)index * 24;
        for (u32 off = 0; off < 24; off += 4)
            vm_write32(e + off, 0);
    }
    return CELL_OK;
}

/* No-arg array getters: return the guest packed
 * arrays (15 / 8 / 8 entries). */
const CellGcmTileInfo* cellGcmGetTileInfo(void)
{
    if (!gcm_vm_ready())
        return NULL;
    return (const CellGcmTileInfo*)(vm_base + GCM_TILEINFO_EA);
}

const CellGcmZcullInfo* cellGcmGetZcullInfo(void)
{
    if (!gcm_vm_ready())
        return NULL;
    return (const CellGcmZcullInfo*)(vm_base + GCM_ZCULLINFO_EA);
}

const CellGcmDisplayInfo* cellGcmGetDisplayInfo(void)
{
    if (!gcm_vm_ready())
        return NULL;
    return (const CellGcmDisplayInfo*)(vm_base + GCM_DISPINFO_EA);
}

/* ---------------------------------------------------------------------------
 * Misc
 * -----------------------------------------------------------------------*/

/* Smallest documented tiled pitch that holds size. OVERRIDE-delegated: keep
 * the signature. */
u32 cellGcmGetTiledPitchSize(u32 size)
{
    for (int i = 0; i < s_valid_pitch_count; i++) {
        if (s_valid_pitches[i] >= size)
            return s_valid_pitches[i];
    }
    return 0;
}

void cellGcmSetDebugOutputLevel(s32 level)
{
    if (level >= CELL_GCM_DEBUG_LEVEL0 && level <= CELL_GCM_DEBUG_LEVEL2)
        s_debug_level = level;
}

/* Release-PRX behavior is a no-op returning CELL_OK */
s32 cellGcmDumpGraphicsError(void)
{
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Default command buffer / FIFO
 * -----------------------------------------------------------------------*/

/* Must precede cellGcmInit; INVALID_ENUM on a bad mode */
s32 cellGcmInitDefaultFifoMode(s32 mode)
{
    if (s_gcm_initialized)
        return CELL_GCM_ERROR_FAILURE;
    if (mode < CELL_GCM_DEFAULT_FIFO_MODE_TRADITIONAL ||
        mode > CELL_GCM_DEFAULT_FIFO_MODE_CONDITIONAL)
        return CELL_GCM_ERROR_INVALID_ENUM;

    s_default_fifo_mode = mode;
    return CELL_OK;
}

/* Word sizes; the RSX must be idling at the start of the default command
 * buffer; the buffer must hold at least two segments and is rounded to a
 * segment multiple. Idle check: put==get within the fixed init window, which
 * accepts the documented 0x1000 idle position and the Yakuza runner's
 * post-init put=get=0 (its consumer starts at GET=0, see
 * import_overrides.cpp; the title calls this right after _cellGcmInitBody).
 * A ring in use (put!=get or beyond the init window) fails per the documented
 * contract. */
s32 cellGcmSetDefaultFifoSize(u32 bufferSize, u32 segmentSize)
{
    if (!s_gcm_initialized)
        return CELL_GCM_ERROR_FAILURE;
    {
        u32 put = gcm_ctrl_read(0), get = gcm_ctrl_read(4);
        if (put != get || put > CELL_GCM_INIT_STATE_OFFSET)
            return CELL_GCM_ERROR_FAILURE;
    }
    if (segmentSize == 0 || bufferSize < segmentSize * 2)
        return CELL_GCM_ERROR_FAILURE;

    bufferSize -= bufferSize % segmentSize;   /* adjust to multiple */
    s_default_fifo_words = bufferSize;
    s_default_segment_words = segmentSize;

    /* Default context members are re-initialized; current points at the
     * beginning position (0x1000) of the default command buffer. */
    if (gcm_vm_ready()) {
        u32 begin = s_cfg_io_address + CELL_GCM_INIT_STATE_OFFSET;
        u32 end   = s_cfg_io_address + bufferSize * 4;
        if (end > s_cfg_io_address + s_cfg_io_size)
            end = s_cfg_io_address + s_cfg_io_size;
        vm_write32(GCM_DEFCTX_EA + 0x0, begin);
        vm_write32(GCM_DEFCTX_EA + 0x4, end - 4);
        vm_write32(GCM_DEFCTX_EA + 0x8, begin);
    }
    return CELL_OK;
}

/* Re-selects the default command buffer as the current buffer. Moving the
 * RSX registers is ResetDefaultCommandBuffer's job (their idle position is
 * 0x1000, not 0); the control plane belongs to the FIFO consumer and must
 * not be touched here. */
void cellGcmSetDefaultCommandBuffer(void)
{
    if (gcm_vm_ready())
        vm_write32(GCM_DEFCTX_EA + 0x8,
                   vm_read32(GCM_DEFCTX_EA + 0x0));
}

u32 cellGcmGetDefaultCommandWordSize(void)
{
    return s_default_fifo_words;
}

u32 cellGcmGetDefaultSegmentWordSize(void)
{
    return s_default_segment_words;
}

/* Default-context overflow callback: flush (publish put) and wrap current
 * back to begin. `context` is a host pointer to the guest struct (BE
 * fields). Only operates on this module's default context: in the Yakuza
 * runner the game's default context uses the segmented callback in
 * yakuza/import_overrides.cpp (yz_gcm_fifo_callback), and writing the
 * control registers for a foreign context would corrupt its ring. */
s32 cellGcmCallback(CellGcmContextData* context, u32 count)
{
    (void)count;
    if (!context || !gcm_vm_ready())
        return CELL_OK;
    if ((u32)((u8*)context - vm_base) != GCM_DEFCTX_EA)
        return CELL_OK;

    u32 cur   = ps3_bswap32(context->current);
    u32 begin = ps3_bswap32(context->begin);

    if (cur >= s_cfg_io_address && cur < s_cfg_io_address + s_cfg_io_size)
        gcm_ctrl_write(0, cur - s_cfg_io_address);   /* put = flushed offset */

    context->current = ps3_bswap32(begin);           /* wrap */
    return CELL_OK;
}
