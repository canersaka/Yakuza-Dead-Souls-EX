/*
 * ps3recomp - cellGcmSys HLE module implementation
 *
 * Rewritten 2026-08-05 to the real SDK contract (doc-conformance audit
 * 2026-08-04, cellGcm section). Oracles:
 *   ORACLE(cell/gcm.h)                    prototypes / export surface
 *   ORACLE(cell/gcm/gcm_error.h)          error values (0x80210xxx)
 *   ORACLE(cell/gcm/gcm_struct.h)         struct layouts
 *   ORACLE(cell/gcm/gcm_enum.h, gcm_macros.h)  enum values, INIT_STATE_OFFSET
 *   ORACLE(libgcm-Reference_e.pdf)        behavior contracts (page-cited)
 *
 * Design: all guest-visible state (control registers, labels, reports,
 * notify, offset tables, packed tile/zcull/display info arrays) lives in
 * GUEST memory so pointer-returning entry points hand back real guest EAs
 * through the bridge (audit: "host pointers returned where guest EAs
 * required"). Regions are placed in the RSX-reserved VM window
 * (runtime/memory/vm.h VM_RSX_BASE) at the same offsets the live core in
 * yakuza/import_overrides.cpp uses (RSX_DMA_CONTROL / RSX_REPORTS), so the
 * two implementations agree on addresses by construction.
 *
 * Gate-build note: all 29 cellGcmSys imports bind to LLE lifted firmware
 * and the gcm entries in gen_imports.py OVERRIDES win over this file, so
 * everything here except the three override-delegated functions
 * (cellGcmSetDisplayBuffer, cellGcmGetTiledPitchSize,
 * cellGcmGetTimeStampLocation) is dead code today. Guest-memory access is
 * gated on s_gcm_initialized (only set by cellGcmInit here, which never
 * runs in the gate build), so the delegated functions keep their exact
 * pre-rewrite live behavior.
 */

#include "cellGcmSys.h"
#include "ps3emu/endian.h"
#include "ps3emu/guest_call.h"
#include "../../runtime/memory/vm.h"        /* vm_base, vm_commit */
#include "../../runtime/ppu/ppu_memory.h"   /* BE guest accessors */

#include <stdio.h>
#include <stdlib.h>
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
 * Local memory: EA 0xC0000000, app-visible size 249MB (0x0F900000) -- the
 * real-hardware value (audit: "localSize 256MB vs app-visible 249MB";
 * matches yakuza_runner.h YZ_GCM_LOCAL_BASE/SIZE).
 *
 * HLE state region: inside the RSX VM window (vm.h: 0x10000000, reserved
 * not committed until used). Offsets mirror the live core
 * (yakuza/import_overrides.cpp RSX_DMA_CONTROL = base+0, control regs at
 * +0x40; RSX_REPORTS = base+0x200000, labels at idx*0x10).
 * -----------------------------------------------------------------------*/

#define GCM_LOCAL_BASE_EA    0xC0000000u
#define GCM_LOCAL_SIZE       0x0F900000u   /* 249 MB app-visible */
#define GCM_IO_SPACE_SIZE    0x10000000u   /* 256 MB IO address space */
#define GCM_IO_PAGES         (GCM_IO_SPACE_SIZE >> 20)          /* 256 */
#define GCM_EA_PAGES         (GCM_LOCAL_BASE_EA >> 20)          /* 3072 (p.17) */

#define GCM_STATE_BASE_EA    0x10000000u                 /* == VM_RSX_BASE */
#define GCM_CONTROL_EA       (GCM_STATE_BASE_EA + 0x40u) /* put/get/ref */
/* Reports region layout matches the live core's sys_rsx context_allocate
 * (import_overrides.cpp:7028-7043, mirroring RPCS3 RsxReports and the
 * driver_info offsets it publishes): semaphores/labels at +0 (256 x 16B),
 * notify at +0x1000 (64 x 16B), report slots at +0x1400 (2048 x 16B). */
#define GCM_REPORTS_EA       (GCM_STATE_BASE_EA + 0x200000u)
#define GCM_LABEL_EA         GCM_REPORTS_EA                  /* 256 x 0x10 */
#define GCM_REPORT_LOCAL_EA  (GCM_REPORTS_EA + 0x1400u)      /* 2048 x 0x10 */
/* HLE-private guest areas: placed ABOVE the live reports block
 * (0x10200000 + 0x9400) so the two cores never write over each other when
 * the live HLE-gcm lane calls into this file. */
#define GCM_IOTABLE_EA       (GCM_STATE_BASE_EA + 0x20A000u) /* u16[3072] */
#define GCM_EATABLE_EA       (GCM_STATE_BASE_EA + 0x20B800u) /* u16[256]  */
#define GCM_TILEINFO_EA      (GCM_STATE_BASE_EA + 0x20BC00u) /* 15 x 16B  */
#define GCM_ZCULLINFO_EA     (GCM_STATE_BASE_EA + 0x20BD00u) /* 8 x 24B   */
#define GCM_DISPINFO_EA      (GCM_STATE_BASE_EA + 0x20BE00u) /* 8 x 16B   */
#define GCM_DEFCTX_EA        (GCM_STATE_BASE_EA + 0x20BF00u) /* 16B       */
#define GCM_STATE_COMMIT_SZ  0x210000u

/* Main-memory report window: io 0x0e000000 - 0x0f000000 (Reference p.328) */
#define GCM_REPORT_MAIN_IO   0x0E000000u

/* ---------------------------------------------------------------------------
 * Internal state (host-side bookkeeping; guest-visible data lives in vm)
 * -----------------------------------------------------------------------*/

static int  s_gcm_initialized = 0;
static u32  s_flip_mode   = CELL_GCM_DISPLAY_VSYNC;
static u32  s_flip_status = CELL_GCM_DISPLAY_FLIP_STATUS_DONE; /* p.408 */
static s32  s_debug_level = CELL_GCM_DEBUG_LEVEL0;

/* Configuration mirror (guest copy written on demand by GetConfiguration) */
static u32 s_cfg_io_address = 0;
static u32 s_cfg_io_size    = 0;

/* Display buffers (host mirror of the guest DisplayInfo array) */
static CellGcmDisplayInfo s_display_buffers[CELL_GCM_MAX_DISPLAY_BUFFER_NUM];
static int s_display_buffer_set[CELL_GCM_MAX_DISPLAY_BUFFER_NUM];
static u32 s_current_display_buffer_id = 0;

/* Prepare-flip registry: flip id -> buffer id (Reference p.407) */
static int s_flip_qid_set[CELL_GCM_MAX_DISPLAY_BUFFER_NUM];
static u32 s_flip_qid_buffer[CELL_GCM_MAX_DISPLAY_BUFFER_NUM];

/* Queued flip (retires at vblank timing; the RSX-side wait-label condition
 * is checked at retire time -- the HLE never force-writes the awaited label
 * (audit: the old force-write was the "force stuck flag" anti-pattern)) */
static int s_flip_queued = 0;
static u32 s_flip_queued_buffer = 0;
static int s_flip_wait_armed = 0;
static u32 s_flip_wait_index = 0;
static u32 s_flip_wait_value = 0;

/* IO mapping registry. 256 slots = one per 1MB page of the 256MB IO window,
 * so the registry itself can never be the limiting resource (audit: the old
 * 16-mapping cap was fabricated). */
typedef struct IoMapping {
    u32 ea;
    u32 io;
    u32 size;
    int active;
} IoMapping;
#define GCM_MAX_IO_MAPPINGS 256
static IoMapping s_io_mappings[GCM_MAX_IO_MAPPINGS];

/* IO map reservation: reserved bytes at the END of the IO space
 * (Reference p.471: "The area at the end of the IO address space ... will
 * be reserved") */
static u32 s_io_map_reserved = 0;

/* Callback handlers -- guest OPD addresses (dispatch via g_ps3_guest_caller) */
static u32 s_flip_handler_opd     = 0;
static u32 s_vblank_handler_opd   = 0;
static u32 s_user_handler_opd     = 0;
static u32 s_second_v_handler_opd = 0;
static u32 s_graphics_handler_opd = 0;
static u32 s_queue_handler_opd    = 0;

/* Flip timing (MICROSECONDS -- Reference p.409) */
static u64 s_last_flip_time_us = 0;

/* VBlank counter: u64, incremented by the host vblank tick only (audit: the
 * old ++-on-read made guest polling loops exit immediately) */
static u64 s_vblank_count = 0;

/* Default FIFO (word sizes). Defaults: 64KB command buffer / 32KB segment
 * (Reference p.414 minimum cmdSize 64KB; p.380 "default size of 32KB"
 * segments; audit pinned the old 0x400/0x40000 as off by 16x/32x). */
static u32 s_default_fifo_words    = 0x4000;
static u32 s_default_segment_words = 0x2000;
static s32 s_default_fifo_mode     = CELL_GCM_DEFAULT_FIFO_MODE_TRADITIONAL;

/* Frequencies */
static u32 s_second_v_frequency = 0;
static u32 s_vblank_frequency   = 0;

/* Local-memory map state (Reference p.467) */
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

/* get/ref advancement (audit: "control reg never advances get/ref"): the
 * control plane is CONSUMER-owned. In the live HLE-gcm lane the FIFO
 * consumer in yakuza/import_overrides.cpp advances get, so this file must
 * never write it outside cellGcmInit (a forced get=put would skip
 * unconsumed commands out from under that consumer). In a pure-libs build
 * the command processor publishes get itself and posts processed
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
 * Reference p.459 notes multiple IO ranges may map one EA and the EA->IO
 * table keeps the last update). */
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
         * EA to another IO range -- only clear if it still points here. */
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

/* Highest usable IO page (reserved area sits at the END of the IO space --
 * Reference pp.471-472) */
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

/* --- valid tiled pitches (audit: table verified correct; keep) ---------- */

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

/* Retire the queued flip if its wait-label condition is satisfied. Never
 * force-writes the label: if nothing ever writes it, the flip stays queued
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
    /* Reference p.429: invalid buffer ID -> CELL_GCM_ERROR_FAILURE */
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
    printf("[cellGcmSys] Init(cmdSize=0x%X, ioSize=0x%X, ioAddr=0x%08X)\n",
           cmdSize, ioSize, ioAddress);

    if (s_gcm_initialized) {
        printf("[cellGcmSys] WARNING: already initialized\n");
        return CELL_GCM_ERROR_FAILURE;
    }
    if (!vm_base)
        return CELL_GCM_ERROR_FAILURE;

    /* Validation (Reference pp.413-414): ioAddress 1MB-aligned, ioSize a
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
    /* Flip status immediately after cellGcmInit is DONE (Reference p.408) */
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
     * GetDefaultSegmentWordSize (segment default 32KB, Reference p.380). */
    s_default_fifo_words = cmdSize >> 2;
    if (s_default_segment_words == 0)
        s_default_segment_words = 0x2000;

    s_gcm_initialized = 1;   /* vm helpers live from here on */

    /* Offset tables + the initial IO mapping (io 0 = command buffer region,
     * Reference p.413: the buffer is mapped from position 0) */
    gcm_tables_reset();
    s_io_mappings[0].ea = ioAddress;
    s_io_mappings[0].io = 0;
    s_io_mappings[0].size = ioSize;
    s_io_mappings[0].active = 1;
    gcm_tables_map(ioAddress, 0, ioSize);

    /* Control registers: put/get idle at the init-state offset 0x1000
     * (gcm_macros.h:373, Reference p.384); ref initial value 0xFFFFFFFF
     * (Reference pp.14/374 -- the old 0 made the documented Finish spin
     * "while (ref != 0)" exit before any command ran). */
    gcm_ctrl_write(0, CELL_GCM_INIT_STATE_OFFSET);
    gcm_ctrl_write(4, CELL_GCM_INIT_STATE_OFFSET);
    gcm_ctrl_write(8, 0xFFFFFFFFu);

    /* Default context (guest BE): begin after the fixed 4KB init area
     * (Reference p.414); end/current simplified to the whole default
     * command buffer minus the segment-link word (INFERRED simplification
     * of the segment ring, Reference p.380). */
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

/* Reference p.406: void return */
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

/* Returns the guest control-register struct (Reference p.376); the bridge
 * converts the host pointer back to a guest EA. OVERRIDE in the gate build. */
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
 * satisfied. Does NOT force the status (audit: old code force-completed). */
void cellGcmSetWaitFlip(CellGcmContextData* ctx)
{
    (void)ctx;
    gcm_try_retire_flip();
}

/* Reference p.417: reset to WAITING */
void cellGcmResetFlipStatus(void)
{
    s_flip_status = CELL_GCM_DISPLAY_FLIP_STATUS_WAITING;
}

/* Reference p.428: no-arg, sets DONE */
void cellGcmSetFlipStatus(void)
{
    s_flip_status = CELL_GCM_DISPLAY_FLIP_STATUS_DONE;
}

/* Reference p.408: pure read -- completion happens at flip retire time
 * (audit: old version self-completed and could never report WAITING) */
u32 cellGcmGetFlipStatus(void)
{
    return s_flip_status;
}

/* Host-side ticks: retire queued flips + fire guest handlers at vblank
 * timing. Called once per frame -- in the live HLE-gcm lane by
 * yz_rsx_vblank_tick (import_overrides.cpp), in a pure-libs build by the
 * flow driver. Must not touch the control registers (consumer-owned). */
void cellGcmTickVBlank(void)
{
    s_vblank_count++;
    gcm_try_retire_flip();
    if (s_vblank_handler_opd && g_ps3_guest_caller)
        g_ps3_guest_caller(s_vblank_handler_opd, 0, 0, 0, 0, 0, 0, 0, 0);
}

void cellGcmTickFlip(void)
{
    gcm_try_retire_flip();
    gcm_fire_flip_handler();
}

/* 2026-08-06 (boots 50-60, the dialogue-load face-B decode): the user-command
 * handler (game func_00E7DB10 — the EBOOT's ONLY _cellSpursSendSignal path,
 * the wid-4 pool's only wake) used to run SYNCHRONOUSLY on the RSX FIFO
 * consumer thread. Real HW order is different: the 0xEB00 method raises an
 * interrupt; Sony's _gcm_intr_thread (a separate PPU thread) runs the handler
 * asynchronously while the RSX keeps consuming, and lv1 keeps ONE pending
 * cause register — rapid commands COALESCE, latest wins (measured game-side:
 * its cause counter is coalescing-tolerant, s25 note in import_overrides).
 * Model that faithfully: a dedicated host intr thread + latest-wins cause
 * latch. The consumer never executes guest callback code and never blocks on
 * the handler. Kill-switch YZ_UCMD_SYNC=1 restores the old inline dispatch. */
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
    static int sync_mode = -1;
    if (sync_mode < 0) {
        sync_mode = getenv("YZ_UCMD_SYNC") ? 1 : 0;
        fprintf(stderr, "[gcm-intr] user-command dispatch: %s\n",
                sync_mode ? "SYNCHRONOUS on consumer (YZ_UCMD_SYNC=1, "
                            "legacy)" :
                            "async intr thread (real-HW order, lv1 "
                            "latest-wins coalescing)");
        fflush(stderr);
    }
    if (!sync_mode) {
        if (!s_ucmd_intr_thread) {
            s_ucmd_intr_event = CreateEventA(NULL, FALSE, FALSE, NULL);
            s_ucmd_intr_thread = CreateThread(
                NULL, 0, gcm_intr_thread_proc, NULL, 0, NULL);
            if (!s_ucmd_intr_event || !s_ucmd_intr_thread) {
                fprintf(stderr, "[gcm-intr] thread setup FAILED — falling "
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

/* Reference p.419: FAILURE for invalid argument values. OVERRIDE-delegated
 * from the live core -- signature must stay (u32 x5). */
s32 cellGcmSetDisplayBuffer(u32 bufferId, u32 offset, u32 pitch,
                            u32 width, u32 height)
{
    printf("[cellGcmSys] SetDisplayBuffer(id=%u, offset=0x%X, pitch=%u, %ux%u)\n",
           bufferId, offset, pitch, width, height);

    if (bufferId >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM)
        return CELL_GCM_ERROR_FAILURE;

    s_display_buffers[bufferId].offset = offset;
    s_display_buffers[bufferId].pitch  = pitch;
    s_display_buffers[bufferId].width  = width;
    s_display_buffers[bufferId].height = height;
    s_display_buffer_set[bufferId] = 1;

    /* Mirror into the guest DisplayInfo array (Reference p.39: values are
     * stored directly). Skipped pre-init so the live delegation path is
     * unchanged. */
    if (gcm_vm_ready()) {
        u32 e = GCM_DISPINFO_EA + bufferId * 16;
        vm_write32(e + 0x0, offset);
        vm_write32(e + 0x4, pitch);
        vm_write32(e + 0x8, width);
        vm_write32(e + 0xC, height);
    }
    return CELL_OK;
}

/* Flip commands take the GCM context as arg0 (gcm_prototypes_ppu.h; audit:
 * "_cellGcmSetFlipCommand ctx+id"). The context is unused by this HLE (no
 * FIFO here -- flips retire via the vblank tick). */
s32 cellGcmSetFlipCommand(CellGcmContextData* ctx, u32 id)
{
    (void)ctx;
    return gcm_queue_flip(id, 0, 0, 0);
}

s32 cellGcmSetFlipCommandWithWaitLabel(CellGcmContextData* ctx, u32 id,
                                       u32 labelindex, u32 labelvalue)
{
    (void)ctx;
    /* Reference p.429: label index range 64-255 (0-63 system-reserved);
     * the documented error contract only covers the buffer id. */
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

/* Reference p.407: registers the buffer for a flip id and returns the flip
 * id; the flip itself happens later. */
s32 cellGcmSetPrepareFlip(CellGcmContextData* ctx, u32 id)
{
    (void)ctx;
    if (id >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM || !s_display_buffer_set[id])
        return CELL_GCM_ERROR_FAILURE;

    s_flip_qid_set[id] = 1;
    s_flip_qid_buffer[id] = id;
    return (s32)id;
}

/* Reference p.85/p.407: immediate flip (no vsync wait) */
s32 cellGcmSetFlipImmediate(u32 id)
{
    s32 rc = gcm_queue_flip(id, 0, 0, 0);
    if (rc != CELL_OK)
        return rc;
    gcm_try_retire_flip();
    return CELL_OK;
}

/* Reference p.407: returns the display buffer ID for a flip id, or -1 */
s32 cellGcmGetDisplayBufferByFlipIndex(u32 qid)
{
    if (qid >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM || !s_flip_qid_set[qid])
        return -1;
    return (s32)s_flip_qid_buffer[qid];
}

/* Reference p.457: out-param + status return */
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

/* Reference p.409: MICROSECONDS (the old ns value was 1000x off) */
u64 cellGcmGetLastFlipTime(void)
{
    return s_last_flip_time_us;
}

/* Reference p.412: u64; counts only at the vblank tick */
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

/* Generates a user-command interrupt (gcm_prototypes_sub.h:112); the HLE
 * dispatches the registered user handler directly. */
void cellGcmSetUserCommand(CellGcmContextData* ctx, u32 cause)
{
    (void)ctx;
    cellGcmDispatchUserCommand(cause);
}

/* ---------------------------------------------------------------------------
 * Address translation / IO mapping
 * -----------------------------------------------------------------------*/

/* Reference p.461: void return; members are guest EAs of the u16 tables */
void cellGcmGetOffsetTable(CellGcmOffsetTable* table)
{
    if (!table)
        return;
    table->ioAddress = ps3_bswap32(s_gcm_initialized ? GCM_IOTABLE_EA : 0);
    table->eaAddress = ps3_bswap32(s_gcm_initialized ? GCM_EATABLE_EA : 0);
}

/* Reference p.459: local memory or a mapped IO EA; FAILURE otherwise */
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

/* Reference pp.469-470: allocator picks the io offset; FAILURE on bad
 * alignment, EINVAL on RSX-prohibited areas, NO_IO_PAGE_TABLE when the page
 * table is full. */
s32 cellGcmMapMainMemory(u32 ea, u32 size, u32* offset)
{
    if (!offset || !s_gcm_initialized)
        return CELL_GCM_ERROR_FAILURE;

    if ((ea & 0xFFFFF) != 0 || size == 0 || (size & 0xFFFFF) != 0)
        return CELL_GCM_ERROR_FAILURE;    /* p.469: FAILURE, not ALIGNMENT */

    if (ea >= GCM_LOCAL_BASE_EA || ea + size > GCM_LOCAL_BASE_EA)
        return CELL_EINVAL;               /* p.469: RSX-prohibited area */

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
        return CELL_GCM_ERROR_NO_IO_PAGE_TABLE;   /* p.469 */

    IoMapping* m = gcm_mapping_alloc();
    if (!m)
        return CELL_GCM_ERROR_NO_IO_PAGE_TABLE;

    m->ea = ea;
    m->io = found << 20;
    m->size = size;
    m->active = 1;
    gcm_tables_map(ea, m->io, size);

    printf("[cellGcmSys] MapMainMemory(ea=0x%08X, size=0x%X) -> io=0x%X\n",
           ea, size, m->io);
    *offset = ps3_bswap32(m->io);
    return CELL_OK;
}

/* Reference pp.463-464: caller picks the io offset; FAILURE on bad
 * alignment, EINVAL on prohibited areas. Multiple IO ranges may map the
 * same EA (p.459 note): no EA-overlap error. */
s32 cellGcmMapEaIoAddress(u32 ea, u32 io, u32 size)
{
    if (!s_gcm_initialized)
        return CELL_GCM_ERROR_FAILURE;

    if ((ea & 0xFFFFF) != 0 || (io & 0xFFFFF) != 0 ||
        size == 0 || (size & 0xFFFFF) != 0)
        return CELL_GCM_ERROR_FAILURE;    /* p.463: FAILURE, not ALIGNMENT */

    if (ea >= GCM_LOCAL_BASE_EA || ea + size > GCM_LOCAL_BASE_EA)
        return CELL_EINVAL;

    /* Target io range must be inside the unreserved window and unused
     * (reserved areas refuse mapping, p.472). */
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

    printf("[cellGcmSys] MapEaIoAddress(ea=0x%08X, io=0x%08X, size=0x%X)\n",
           ea, io, size);
    return CELL_OK;
}

/* Reference p.473: must be the mapping's beginning EA; FAILURE otherwise */
s32 cellGcmUnmapEaIoAddress(u32 ea)
{
    IoMapping* m = gcm_mapping_by_ea(ea);
    if (!m || !s_gcm_initialized)
        return CELL_GCM_ERROR_FAILURE;

    gcm_tables_unmap(m->ea, m->io, m->size);
    m->active = 0;
    return CELL_OK;
}

/* Reference p.474: must be the mapping's beginning io offset */
s32 cellGcmUnmapIoAddress(u32 io)
{
    IoMapping* m = gcm_mapping_by_io(io);
    if (!m || !s_gcm_initialized)
        return CELL_GCM_ERROR_FAILURE;

    gcm_tables_unmap(m->ea, m->io, m->size);
    m->active = 0;
    return CELL_OK;
}

/* Reference p.462: io -> EA (local memory NOT covered); FAILURE if unmapped */
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

/* Reference p.450: compact the IO page table. The HLE reassigns every
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

/* Reference pp.467-468 */
s32 cellGcmMapLocalMemory(u32* address, u32* size)
{
    if (!address || !size)
        return CELL_GCM_ERROR_FAILURE;

    if (s_gcm_initialized) {
        /* p.467: after cellGcmInit local memory is already mapped: ENOMEM */
        return CELL_ENOMEM;
    }
    if (s_local_memory_mapped) {
        printf("[cellGcmSys] RSX local memory already mapped\n");
        return CELL_GCM_ERROR_FAILURE;
    }

    if (vm_base)
        vm_commit(GCM_LOCAL_BASE_EA, GCM_LOCAL_SIZE);

    s_local_memory_mapped = 1;
    *address = ps3_bswap32(GCM_LOCAL_BASE_EA);
    *size    = ps3_bswap32(GCM_LOCAL_SIZE);
    return CELL_OK;
}

/* Reference p.460: IO space size minus the RESERVED area (mapped areas do
 * not reduce it -- the old subtract-mapped math was wrong and underflowed) */
u32 cellGcmGetMaxIoMapSize(void)
{
    return GCM_IO_SPACE_SIZE - s_io_map_reserved;
}

/* Reference p.471 */
s32 cellGcmReserveIoMapSize(u32 size)
{
    if ((size & 0xFFFFF) != 0)
        return CELL_GCM_ERROR_INVALID_ALIGNMENT;
    if (size > cellGcmGetMaxIoMapSize())
        return CELL_GCM_ERROR_INVALID_VALUE;

    s_io_map_reserved += size;
    return CELL_OK;
}

/* Reference p.475 */
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
 * same base+stride as the live core's cellGcmGetLabelAddress). */
u32* cellGcmGetLabelAddress(u8 index)
{
    if (!gcm_vm_ready())
        return NULL;
    return (u32*)(vm_base + GCM_LABEL_EA + (u32)index * CELL_GCM_LABEL_STRIDE);
}

/* Reference p.326: local report area, indices 0-2047 */
CellGcmReportData* cellGcmGetReportDataAddress(u32 index)
{
    if (index >= CELL_GCM_MAX_REPORT_COUNT || !gcm_vm_ready())
        return NULL;
    return (CellGcmReportData*)(vm_base + GCM_REPORT_LOCAL_EA +
                                index * CELL_GCM_REPORT_DATA_SIZE);
}

/* Resolve a report slot EA for (index, location); 0 when unresolvable.
 * MAIN reports live at io 0x0e000000 + index*16 and reach guest memory only
 * through the caller's io mapping (Reference pp.327-328). */
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

/* Reference p.327 */
CellGcmReportData* cellGcmGetReportDataAddressLocation(u32 index, u32 location)
{
    u32 ea = gcm_report_ea(index, location);
    return ea ? (CellGcmReportData*)(vm_base + ea) : NULL;
}

/* Reference p.329: reads the report's value member */
u32 cellGcmGetReportDataLocation(u32 index, u32 location)
{
    u32 ea = gcm_report_ea(index, location);
    return ea ? vm_read32(ea + 8) : 0;
}

/* Reference p.324: local report area only; the HLE keeps one value per slot
 * so type does not select a separate bank */
u32 cellGcmGetReport(u32 type, u32 index)
{
    (void)type;
    return cellGcmGetReportDataLocation(index, CELL_GCM_LOCATION_LOCAL);
}

/* Reference p.331: returns the report timer in NANOSECONDS for the given
 * slot. HLE model (audit-endorsed: "GetTimeStamp ns correct"): the shared
 * host monotonic ns clock -- the same clock rsx_commands.c stamps into
 * report slots via cellGcmReportTimestampNs -- is returned directly, so a
 * timer written through the FIFO and one read here agree by construction
 * and the value advances even before any GET_REPORT lands (guest code that
 * follows p.331's completion-guarantee rule cannot tell the difference). */
u64 cellGcmGetTimeStamp(u32 index)
{
    if (index >= CELL_GCM_MAX_REPORT_COUNT)
        return 0;
    return get_timestamp_ns();
}

/* Reference p.332: same contract with a location-dependent index range.
 * OVERRIDE-delegated from the live core -- behavior identical to the
 * pre-rewrite version for valid indices. */
u64 cellGcmGetTimeStampLocation(u32 index, u32 location)
{
    if (location == CELL_GCM_LOCATION_LOCAL) {
        if (index >= CELL_GCM_MAX_REPORT_COUNT)
            return 0;
    } else if (location == CELL_GCM_LOCATION_MAIN) {
        if (index >= CELL_GCM_MAX_MAIN_REPORT_COUNT)
            return 0;
    } else {
        return 0;
    }
    return get_timestamp_ns();
}

/* Shared monotonic timebase (ns) for report timers (consumer:
 * rsx_commands.c NV4097_GET_REPORT handler). */
u64 cellGcmReportTimestampNs(void)
{
    return get_timestamp_ns();
}

/* Reference p.323 + gcm_macros.h:376-381: notify data lives in MAIN memory
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

/* Write one packed TileInfo entry (Reference p.36 formulas, quoted in the
 * header) into the guest array. */
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

/* Reference pp.438-440 validation contract */
s32 cellGcmSetTileInfo(u8 index, u8 location, u32 offset, u32 size,
                       u32 pitch, u8 comp, u16 base, u8 bank)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;      /* index 15+ */
    if (base >= 0x800 || bank >= 4)
        return CELL_GCM_ERROR_INVALID_VALUE;      /* p.438 */
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
    /* SetTileInfo only holds the settings (p.439); the packed guest entry
     * is what GetTileInfo exposes (p.36/p.453), so keep it current. */
    gcm_tile_write_packed(index, t);
    return CELL_OK;
}

/* Reference pp.400-401. Only the index is validated hard: this title
 * MEASURED(native-fifo-fix-visible.stdout.log:186-189) binds tile 1
 * [0,0x3C0000) inside already-bound tile 0 [0,0x780000) and runs on real
 * hardware, so the p.400 ADDRESS_OVERWRAP table cannot describe the release
 * PRX for this pattern (RPCS3's cellGcmBindTile likewise checks only the
 * index). The documented conditions are logged once for diagnostics. */
s32 cellGcmBindTile(u8 index)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    TileRaw* t = &s_tiles[index];

    for (int i = 0; i < CELL_GCM_MAX_TILE_COUNT; i++) {
        if (i == index || !s_tiles[i].bound || s_tiles[i].location != t->location)
            continue;
        if (t->offset < s_tiles[i].offset + s_tiles[i].size &&
            s_tiles[i].offset < t->offset + t->size) {
            static int warned = 0;
            if (!warned) {
                warned = 1;
                printf("[cellGcmSys] note: BindTile(%u) overlaps bound tile %d "
                       "(documented ADDRESS_OVERWRAP case; accepted per "
                       "measured title behavior)\n", index, i);
            }
            break;
        }
    }

    t->bound = 1;
    return CELL_OK;
}

/* Reference p.451: unbind + clear the packed entry to zeros (p.453) */
s32 cellGcmUnbindTile(u8 index)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    s_tiles[index].bound = 0;
    s_tiles[index].set = 0;
    gcm_tile_clear_packed(index);
    return CELL_OK;
}

/* Legacy combined setter (cell/gcm.h:129: void) */
void cellGcmSetTile(u8 index, u8 location, u32 offset, u32 size,
                    u32 pitch, u8 comp, u16 base, u8 bank)
{
    if (cellGcmSetTileInfo(index, location, offset, size,
                           pitch, comp, base, bank) == CELL_OK)
        (void)cellGcmBindTile(index);
}

/* cell/gcm.h:130: void */
void cellGcmSetInvalidateTile(u8 index)
{
    (void)cellGcmUnbindTile(index);
}

/* Reference pp.402-404: the INVALID_VALUE/ALIGNMENT/ENUM table is
 * explicitly "when the debug-version PRX is used" (p.402); the release PRX
 * validates nothing but the bind slot. This matters twice over: (a) release
 * fidelity, and (b) the currently-generated import bridge still forwards
 * only 8 GPR args to this 12-parameter export (PPU args 9-12 live in the
 * guest stack save area), so until it gets a context-aware bridge (same
 * class as _cellGcmFunc15 -- see report) the tail parameters may be
 * garbage; rejecting on them would break the measured live call. */
s32 cellGcmBindZcull(u32 index, u32 offset, u32 width, u32 height,
                     u32 cullStart, u32 zFormat, u32 aaFormat,
                     u32 zcullDir, u32 zcullFormat,
                     u32 sFunc, u32 sRef, u32 sMask)
{
    if (index >= CELL_GCM_MAX_ZCULL_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    if (gcm_vm_ready()) {
        u32 e = GCM_ZCULLINFO_EA + index * 24;
        /* Packing formulas: Reference p.37 (quoted in cellGcmSys.h) */
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

/* Legacy combined setter (cell/gcm.h:131: void) */
void cellGcmSetZcull(u8 index, u32 offset, u32 width, u32 height,
                     u32 cullStart, u32 zFormat, u32 aaFormat,
                     u32 zcullDir, u32 zcullFormat,
                     u32 sFunc, u32 sRef, u32 sMask)
{
    (void)cellGcmBindZcull(index, offset, width, height, cullStart,
                           zFormat, aaFormat, zcullDir, zcullFormat,
                           sFunc, sRef, sMask);
}

/* Reference p.452: unbind + clear the packed entry (p.454) */
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

/* No-arg array getters (Reference pp.453-455): return the guest packed
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

/* Reference p.411. Table verified against the documented pitch set (audit:
 * "pitch table itself correct"). OVERRIDE-delegated: keep signature. */
u32 cellGcmGetTiledPitchSize(u32 size)
{
    for (int i = 0; i < s_valid_pitch_count; i++) {
        if (s_valid_pitches[i] >= size)
            return s_valid_pitches[i];
    }
    return 0;
}

/* cell/gcm.h:65: void, s32 level */
void cellGcmSetDebugOutputLevel(s32 level)
{
    if (level >= CELL_GCM_DEBUG_LEVEL0 && level <= CELL_GCM_DEBUG_LEVEL2)
        s_debug_level = level;
}

/* Reference p.405: release-PRX behavior is a no-op returning CELL_OK */
s32 cellGcmDumpGraphicsError(void)
{
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Default command buffer / FIFO
 * -----------------------------------------------------------------------*/

/* Reference p.379: must precede cellGcmInit; INVALID_ENUM on a bad mode */
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

/* Reference pp.388-389: word sizes; the RSX must be idling at the start of
 * the default command buffer; buffer must hold at least two segments;
 * buffer rounded to a segment multiple. Idle check: put==get within the
 * fixed init window -- accepts the documented 0x1000 idle position AND the
 * live HLE-gcm lane's post-init put=get=0 (its consumer starts at GET=0,
 * import_overrides.cpp:6780; the measured boot calls this right after
 * _cellGcmInitBody). A live ring (put!=get or beyond the init window)
 * fails per the documented contract. */
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

    bufferSize -= bufferSize % segmentSize;   /* p.389 adjust to multiple */
    s_default_fifo_words = bufferSize;
    s_default_segment_words = segmentSize;

    /* p.389: default context members re-initialized; current points at the
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

/* cell/gcm.h:110: re-selects the default command buffer as the current
 * buffer (audit LOW: "it's a re-select shim" -- the old version zeroed
 * put/get/ref, but moving the RSX registers is ResetDefaultCommandBuffer's
 * job (pp.384-385, and their idle position is 0x1000, not 0); the control
 * plane is consumer-owned and must not be touched here). */
void cellGcmSetDefaultCommandBuffer(void)
{
    if (gcm_vm_ready())
        vm_write32(GCM_DEFCTX_EA + 0x8,
                   vm_read32(GCM_DEFCTX_EA + 0x0));
}

/* Reference p.377 */
u32 cellGcmGetDefaultCommandWordSize(void)
{
    return s_default_fifo_words;
}

/* Reference p.378 */
u32 cellGcmGetDefaultSegmentWordSize(void)
{
    return s_default_segment_words;
}

/* Default-context overflow callback: flush (publish put) and wrap current
 * back to begin. `context` is a host pointer to the guest struct (BE
 * fields). Only operates on THIS module's default context -- in the live
 * HLE-gcm lane the game's default context uses the segmented callback in
 * yakuza/import_overrides.cpp (yz_gcm_fifo_callback), and writing the
 * consumer-owned registers for a foreign context would corrupt its ring. */
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
