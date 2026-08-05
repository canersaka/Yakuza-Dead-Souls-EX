/*
 * ps3recomp - cellGcmSys HLE module
 *
 * RSX (Reality Synthesizer) graphics system interface: initialization,
 * display buffers, flip control, command-buffer control registers,
 * tile/zcull configuration, IO memory mapping, report/label/notify areas,
 * and address-to-offset translation.
 *
 * Rewritten 2026-08-05 against the real SDK contract (doc-conformance
 * audit 2026-08-04, cellGcm section):
 *   - prototypes follow ORACLE(cell/gcm.h) exactly (names, arity, returns)
 *   - error values follow ORACLE(cell/gcm/gcm_error.h) (0x80210xxx)
 *   - struct layouts follow ORACLE(cell/gcm/gcm_struct.h)
 *   - packed tile/zcull encodings follow ORACLE(libgcm-Reference_e.pdf
 *     pp.36-37)
 *
 * NOTE (gate build): all 29 cellGcmSys imports of the current title bind to
 * the LLE lifted firmware, and the context-aware overrides in
 * yakuza/import_overrides.cpp win over these HLE functions at bridge level
 * (gen_imports.py OVERRIDES). This module is the firmware-free-milestone
 * implementation. The three functions the live overrides DELEGATE to
 * (cellGcmSetDisplayBuffer, cellGcmGetTiledPitchSize,
 * cellGcmGetTimeStampLocation) keep their C signatures and their
 * pre-cellGcmInit fallback behavior so the live path is unchanged.
 *
 * Note: RSX command processing (NV4097 methods, shaders, textures) lives in
 * the RSX command processor module (rsx_commands.c / rsx_dispatch.c), not
 * here.
 */

#ifndef PS3RECOMP_CELL_GCM_SYS_H
#define PS3RECOMP_CELL_GCM_SYS_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Constants -- values ORACLE(cell/gcm/gcm_enum.h, gcm_macros.h)
 * -----------------------------------------------------------------------*/

/* Flip modes (gcm_enum.h:388-390) */
#define CELL_GCM_DISPLAY_HSYNC              1
#define CELL_GCM_DISPLAY_VSYNC              2
#define CELL_GCM_DISPLAY_HSYNC_WITH_NOISE   3

/* Flip status (gcm_enum.h:571-572) */
#define CELL_GCM_DISPLAY_FLIP_STATUS_DONE       0
#define CELL_GCM_DISPLAY_FLIP_STATUS_WAITING    1
/* Legacy short aliases (same values) */
#define CELL_GCM_FLIP_STATUS_DONE       CELL_GCM_DISPLAY_FLIP_STATUS_DONE
#define CELL_GCM_FLIP_STATUS_WAITING    CELL_GCM_DISPLAY_FLIP_STATUS_WAITING

/* Location (gcm_enum.h:15-16) */
#define CELL_GCM_LOCATION_LOCAL         0  /* video memory */
#define CELL_GCM_LOCATION_MAIN          1  /* main memory */

/* Surface color formats (gcm_enum.h) */
#define CELL_GCM_SURFACE_X1R5G5B5_Z1R5G5B5   1
#define CELL_GCM_SURFACE_X1R5G5B5_O1R5G5B5   2
#define CELL_GCM_SURFACE_R5G6B5               3
#define CELL_GCM_SURFACE_X8R8G8B8_Z8R8G8B8   4
#define CELL_GCM_SURFACE_X8R8G8B8             5
#define CELL_GCM_SURFACE_A8R8G8B8             8
#define CELL_GCM_SURFACE_B8                   9
#define CELL_GCM_SURFACE_G8B8                 10
#define CELL_GCM_SURFACE_F_W16Z16Y16X16       11
#define CELL_GCM_SURFACE_F_W32Z32Y32X32       12
#define CELL_GCM_SURFACE_F_X32                13
#define CELL_GCM_SURFACE_X8B8G8R8_Z8B8G8R8   14
#define CELL_GCM_SURFACE_X8B8G8R8_O8B8G8R8   15
#define CELL_GCM_SURFACE_A8B8G8R8             16

/* Depth buffer formats */
#define CELL_GCM_SURFACE_Z16                  1
#define CELL_GCM_SURFACE_Z24S8                2

/* Multisample modes (gcm_enum.h:41-44) */
#define CELL_GCM_SURFACE_CENTER_1             0
#define CELL_GCM_SURFACE_DIAGONAL_CENTERED_2  3
#define CELL_GCM_SURFACE_SQUARE_CENTERED_4    4
#define CELL_GCM_SURFACE_SQUARE_ROTATED_4     5

/* Tile compression modes (gcm_enum.h:361-368) */
#define CELL_GCM_COMPMODE_DISABLED                  0
#define CELL_GCM_COMPMODE_C32_2X1                   7
#define CELL_GCM_COMPMODE_C32_2X2                   8
#define CELL_GCM_COMPMODE_Z32_SEPSTENCIL            9
#define CELL_GCM_COMPMODE_Z32_SEPSTENCIL_REGULAR    10
#define CELL_GCM_COMPMODE_Z32_SEPSTENCIL_DIAGONAL   11
#define CELL_GCM_COMPMODE_Z32_SEPSTENCIL_ROTATED    12

/* Zcull formats (gcm_enum.h:371-376) */
#define CELL_GCM_ZCULL_Z16          1
#define CELL_GCM_ZCULL_Z24S8        2
#define CELL_GCM_ZCULL_MSB          0
#define CELL_GCM_ZCULL_LONES        1
#define CELL_GCM_ZCULL_LESS         0
#define CELL_GCM_ZCULL_GREATER      1

/* Stencil cull functions (gcm_enum.h:378-385) */
#define CELL_GCM_SCULL_SFUNC_NEVER      0
#define CELL_GCM_SCULL_SFUNC_LESS       1
#define CELL_GCM_SCULL_SFUNC_EQUAL      2
#define CELL_GCM_SCULL_SFUNC_LEQUAL     3
#define CELL_GCM_SCULL_SFUNC_GREATER    4
#define CELL_GCM_SCULL_SFUNC_NOTEQUAL   5
#define CELL_GCM_SCULL_SFUNC_GEQUAL     6
#define CELL_GCM_SCULL_SFUNC_ALWAYS     7

/* Default FIFO modes (gcm_enum.h:618-620, sequential enum from 0) */
#define CELL_GCM_DEFAULT_FIFO_MODE_TRADITIONAL  0
#define CELL_GCM_DEFAULT_FIFO_MODE_OPTIMIZE     1
#define CELL_GCM_DEFAULT_FIFO_MODE_CONDITIONAL  2

/* Max display buffers (Reference p.419: ids 0-7) */
#define CELL_GCM_MAX_DISPLAY_BUFFER_NUM 8

/* Tile / Zcull limits (gcm_macros.h:366-367) */
#define CELL_GCM_MAX_TILE_COUNT         15
#define CELL_GCM_MAX_ZCULL_COUNT        8

/* Labels: 256 x 16-byte slots. Indices 0-63 are system-reserved; the
 * flip-wait label index range is 64-255 (Reference p.429). */
#define CELL_GCM_MAX_LABEL_COUNT        256
#define CELL_GCM_LABEL_STRIDE           0x10

/* Report slots: local report area has 2048 indices (Reference pp.326-332);
 * the main-memory report area (io 0x0e000000-0x0f000000) has 1M indices
 * (Reference p.327). Each report is 16 bytes. */
#define CELL_GCM_MAX_REPORT_COUNT       2048
#define CELL_GCM_MAX_MAIN_REPORT_COUNT  (1024 * 1024)
#define CELL_GCM_REPORT_DATA_SIZE       16

/* Notify data: 8 slots, 64-byte stride, io window 0x0f100000 +0x200
 * (gcm_macros.h:376-381, Reference p.16/p.323) */
#define CELL_GCM_NOTIFY_MAIN_MAX_COUNT  8
#define CELL_GCM_NOTIFY_MAIN_ALIGN_SIZE 64
#define CELL_GCM_NOTIFY_IO_ADDRESS_BASE 0x0F100000u
#define CELL_GCM_NOTIFY_IO_ADDRESS_SIZE 0x00000200u

/* Idle/init position of the default command buffer: the first 4KB of the
 * command buffer holds the RSX state-init commands; put/get sit at 0x1000
 * when idle (gcm_macros.h:373 CELL_GCM_INIT_STATE_OFFSET, Reference
 * pp.384/414). */
#define CELL_GCM_INIT_STATE_OFFSET      0x1000u

/* Debug output levels */
#define CELL_GCM_DEBUG_LEVEL0           0
#define CELL_GCM_DEBUG_LEVEL1           1
#define CELL_GCM_DEBUG_LEVEL2           2

/* ---------------------------------------------------------------------------
 * Structures -- layouts ORACLE(cell/gcm/gcm_struct.h)
 *
 * Pointer-typed SDK members are guest EAs in the recomp and are declared u32
 * here. All guest-resident instances are stored big-endian; HLE functions
 * that receive a host pointer to a guest struct byte-swap explicitly
 * (convention: libs/video/cellVideoOut.c).
 * -----------------------------------------------------------------------*/

/* Command-buffer control registers (gcm_struct.h:16-20). ref initial value
 * after cellGcmInit is 0xFFFFFFFF (Reference pp.14/374). */
typedef struct CellGcmControl {
    volatile u32 put;       /* end of command string (put register) */
    volatile u32 get;       /* beginning of command string (get register) */
    volatile u32 ref;       /* reference register */
} CellGcmControl;

/* GCM context data (gcm_struct.h:74-79). Fields are guest EAs (BE in guest
 * memory). Layout also verified against compiled SDK inline code (Yakuza:
 * Dead Souls EBOOT command-write helper at 0xEBC0C8): begin/end/current/
 * callback, callback last at offset 0xC. */
typedef struct CellGcmContextData {
    u32 begin;      /* start of command buffer (guest EA) */
    u32 end;        /* end of command buffer (guest EA) */
    u32 current;    /* current write position (guest EA) */
    u32 callback;   /* overflow callback OPD (guest EA) */
} CellGcmContextData;

/* Display buffer info (gcm_struct.h:157-162) */
typedef struct CellGcmDisplayInfo {
    u32 offset;         /* offset in local memory */
    u32 pitch;          /* pitch in bytes */
    u32 width;
    u32 height;
} CellGcmDisplayInfo;

/* Address conversion tables (gcm_struct.h:81-84). The two members are guest
 * EAs of u16 tables (1MB pages, BE entries, 0xFFFF = unmapped): ioAddress
 * has valid subscripts 0-0xBFF (EA 0..0xC0000000), eaAddress 0-255 for the
 * 256MB IO space (Reference p.17). */
typedef struct CellGcmOffsetTable {
    u32 ioAddress;      /* guest EA of u16[]: ea page -> io page */
    u32 eaAddress;      /* guest EA of u16[]: io page -> ea page */
} CellGcmOffsetTable;

/* GCM configuration (gcm_struct.h:22-29; localAddress/ioAddress are guest
 * EAs) */
typedef struct CellGcmConfig {
    u32 localAddress;       /* guest EA of local (video) memory */
    u32 ioAddress;          /* guest EA of the IO-mapped main memory */
    u32 localSize;          /* size of local memory */
    u32 ioSize;             /* size of IO-mapped region */
    u32 memoryFrequency;    /* RSX memory clock */
    u32 coreFrequency;      /* RSX core clock */
} CellGcmConfig;

/* Tile info, PACKED format (gcm_struct.h:141-146; packing formulas
 * Reference p.36):
 *   tile   = (location+1) | (bank<<4) | ((offset/0x10000)<<16) | location<<31
 *   limit  = ((offset+size-1)/0x10000)<<16 | location<<31
 *   pitch  = (pitch/0x100)<<8
 *   format = base | (base+((size-1)/0x10000))<<13 | comp<<26 | 1<<30 */
typedef struct CellGcmTileInfo {
    u32 tile;
    u32 limit;
    u32 pitch;
    u32 format;
} CellGcmTileInfo;

/* Zcull info, PACKED format (gcm_struct.h:148-155; packing formulas
 * Reference p.37):
 *   region  = (1<<0) | (zFormat<<4) | (aaFormat<<8)
 *   size    = ((width>>6)<<22) | ((height>>6)<<6)
 *   start   = cullStart & ~0xFFF
 *   offset  = offset
 *   status0 = (zcullDir<<1) | (zcullFormat<<2) | ((sFunc&0xF)<<12)
 *             | (sRef<<16) | (sMask<<24)
 *   status1 = (0x2000<<0) | (0x20<<16)   (fixed value) */
typedef struct CellGcmZcullInfo {
    u32 region;
    u32 size;
    u32 start;
    u32 offset;
    u32 status0;
    u32 status1;
} CellGcmZcullInfo;

/* Report data (gcm_struct.h:91-95): timer is written by the RSX in
 * NANOSECONDS; zero always written 0. */
typedef struct CellGcmReportData {
    u64 timer;
    u32 value;
    u32 zero;
} CellGcmReportData;

/* Notify data (gcm_struct.h:97-100) */
typedef struct CellGcmNotifyData {
    u64 timer;
    u64 zero;
} CellGcmNotifyData;

/* Callback typedefs. In the recomp these carry guest OPD addresses, not
 * host function pointers; setters store the OPD and dispatch goes through
 * g_ps3_guest_caller (ps3emu/guest_call.h). */
typedef void (*CellGcmFlipHandler)(u32 head);
typedef void (*CellGcmVBlankHandler)(u32 head);
typedef void (*CellGcmUserHandler)(u32 cause);
typedef void (*CellGcmSecondVHandler)(u32 head);
typedef s32  (*CellGcmContextCallback)(struct CellGcmContextData* context, u32 count);
typedef void (*CellGcmGraphicsHandler)(u32 val);
typedef void (*CellGcmQueueHandler)(u32 head);

/* ---------------------------------------------------------------------------
 * Functions -- prototypes ORACLE(cell/gcm.h). NIDs derive from these names
 * (tools/nid_database.py computes them; per-function NID comments removed --
 * several were stale duplicates).
 * -----------------------------------------------------------------------*/

/* --- system functions (cell/gcm.h:51-79) --- */

/* cell/gcm.h:52: cellGcmInit(cmdSize, ioSize, const void* ioAddress).
 * ioAddress is the raw guest EA of the 1MB-aligned main-memory buffer.
 * Note: the real PRX export behind the SDK inline is _cellGcmInitBody,
 * which is a context-aware override in yakuza/import_overrides.cpp; this
 * entry is the standalone HLE initializer. */
s32 cellGcmInit(u32 cmdSize, u32 ioSize, u32 ioAddress);

/* cell/gcm.h:55: returns void */
void cellGcmGetConfiguration(CellGcmConfig* config);

/* cell/gcm.h:56: returns the control-register struct (guest EA through the
 * bridge). OVERRIDE in the gate build. */
CellGcmControl* cellGcmGetControlRegister(void);

/* cell/gcm.h:57. OVERRIDE-delegated: keep signature. */
u32 cellGcmGetTiledPitchSize(u32 size);

/* cell/gcm.h:59-65 -- handler setters (guest OPDs) */
void cellGcmSetVBlankHandler(CellGcmVBlankHandler handler);
void cellGcmSetSecondVHandler(CellGcmSecondVHandler handler);
void cellGcmSetGraphicsHandler(CellGcmGraphicsHandler handler);
void cellGcmSetFlipHandler(CellGcmFlipHandler handler);
void cellGcmSetQueueHandler(CellGcmQueueHandler handler);
void cellGcmSetUserHandler(CellGcmUserHandler handler);
void cellGcmSetDebugOutputLevel(s32 level);

/* cell/gcm.h:67-79 -- IO mapping */
void cellGcmGetOffsetTable(CellGcmOffsetTable* table);
s32  cellGcmIoOffsetToAddress(u32 ioOffset, u32* address);   /* *address = BE guest EA */
s32  cellGcmSortRemapEaIoAddress(void);
s32  cellGcmMapEaIoAddress(u32 ea, u32 io, u32 size);
s32  cellGcmUnmapIoAddress(u32 io);
s32  cellGcmUnmapEaIoAddress(u32 ea);
s32  cellGcmMapLocalMemory(u32* address, u32* size);         /* BE out-params */
u32  cellGcmGetMaxIoMapSize(void);
s32  cellGcmReserveIoMapSize(u32 size);
s32  cellGcmUnreserveIoMapSize(u32 size);
s32  cellGcmAddressToOffset(u32 address, u32* offset);       /* *offset = BE */
s32  cellGcmMapMainMemory(u32 ea, u32 size, u32* offset);    /* *offset = BE */

/* --- display functions (cell/gcm.h:82-95) --- */

s32  cellGcmSetDisplayBuffer(u32 bufferId, u32 offset, u32 pitch,
                             u32 width, u32 height);         /* OVERRIDE-delegated */
u64  cellGcmGetLastFlipTime(void);      /* MICROSECONDS (Reference p.409) */
void cellGcmSetFlipMode(u32 mode);
s32  cellGcmSetFlipImmediate(u32 id);
void cellGcmResetFlipStatus(void);      /* -> WAITING (Reference p.417) */
void cellGcmSetFlipStatus(void);        /* -> DONE, no args (Reference p.428) */
u32  cellGcmGetFlipStatus(void);
void cellGcmSetSecondVFrequency(u32 freq);
s32  cellGcmGetDisplayBufferByFlipIndex(u32 qid);  /* buffer id, or -1 (p.407) */
u64  cellGcmGetVBlankCount(void);
u32  cellGcmGetCurrentField(void);
void cellGcmSetVBlankFrequency(u32 freq);
s32  cellGcmGetCurrentDisplayBufferId(u8* id);     /* (p.457) */

/* --- flip command entry points ---
 * The exports take the GCM context as arg0 (gcm_prototypes_ppu.h:12-29;
 * audit: "_cellGcmSetFlipCommand ctx+id"). Underscore forms are the real
 * PRX exports (OVERRIDES in the gate build). */
s32 cellGcmSetFlipCommand(CellGcmContextData* ctx, u32 id);
s32 cellGcmSetFlipCommandWithWaitLabel(CellGcmContextData* ctx, u32 id,
                                       u32 labelindex, u32 labelvalue);
s32 _cellGcmSetFlipCommand(CellGcmContextData* ctx, u32 id);
s32 _cellGcmSetFlipCommandWithWaitLabel(CellGcmContextData* ctx, u32 id,
                                        u32 labelindex, u32 labelvalue);
/* Returns the flip id (= buffer id) on success (Reference p.407) */
s32 cellGcmSetPrepareFlip(CellGcmContextData* ctx, u32 id);
/* Waits for a queued flip to retire (gcm_prototypes_sub.h:136; HLE executes
 * the pending flip if its wait-label condition is satisfied) */
void cellGcmSetWaitFlip(CellGcmContextData* ctx);
/* Generates a user-command interrupt (gcm_prototypes_sub.h:112) */
void cellGcmSetUserCommand(CellGcmContextData* ctx, u32 cause);

/* --- flow control (cell/gcm.h:108-117) --- */

void cellGcmSetDefaultCommandBuffer(void);
u32  cellGcmGetDefaultCommandWordSize(void);
u32  cellGcmGetDefaultSegmentWordSize(void);
s32  cellGcmInitDefaultFifoMode(s32 mode);
s32  cellGcmSetDefaultFifoSize(u32 bufferSize, u32 segmentSize);

/* Default-context overflow callback (cell/gcm.h:74 cellGcmCallbackForSnc is
 * the SNC alias; the PRX export is cellGcmCallback) */
s32 cellGcmCallback(CellGcmContextData* context, u32 count);

/* --- label / report / timestamp (cell/gcm.h:126-145) --- */

u32* cellGcmGetLabelAddress(u8 index);
u64  cellGcmGetTimeStamp(u32 index);
u32  cellGcmGetReport(u32 type, u32 index);
CellGcmReportData* cellGcmGetReportDataAddress(u32 index);
CellGcmReportData* cellGcmGetReportDataAddressLocation(u32 index, u32 location);
u64  cellGcmGetTimeStampLocation(u32 index, u32 location);   /* OVERRIDE-delegated */
u32  cellGcmGetReportDataLocation(u32 index, u32 location);
CellGcmNotifyData* cellGcmGetNotifyDataAddress(u32 index);

/* --- tile / zcull (cell/gcm.h:129-137, 161-163) --- */

/* Legacy combined setters: void returns (cell/gcm.h:129-131) */
void cellGcmSetTile(u8 index, u8 location, u32 offset, u32 size,
                    u32 pitch, u8 comp, u16 base, u8 bank);
void cellGcmSetInvalidateTile(u8 index);
void cellGcmSetZcull(u8 index, u32 offset, u32 width, u32 height,
                     u32 cullStart, u32 zFormat, u32 aaFormat,
                     u32 zcullDir, u32 zcullFormat,
                     u32 sFunc, u32 sRef, u32 sMask);

s32 cellGcmSetTileInfo(u8 index, u8 location, u32 offset, u32 size,
                       u32 pitch, u8 comp, u16 base, u8 bank);
s32 cellGcmBindTile(u8 index);
s32 cellGcmUnbindTile(u8 index);
/* 12-parameter form (cell/gcm.h:136). NOTE: >8 args -- if this is ever
 * imported in a firmware-free build it needs a context-aware bridge (args
 * 9-12 live in the guest parameter save area), same class as
 * _cellGcmFunc15. */
s32 cellGcmBindZcull(u32 index, u32 offset, u32 width, u32 height,
                     u32 cullStart, u32 zFormat, u32 aaFormat,
                     u32 zcullDir, u32 zcullFormat,
                     u32 sFunc, u32 sRef, u32 sMask);
s32 cellGcmUnbindZcull(u8 index);

/* No-arg array-pointer getters (cell/gcm.h:161-163; Reference pp.453-455).
 * Return the guest arrays (15 / 8 / 8 packed entries). */
const CellGcmTileInfo*    cellGcmGetTileInfo(void);
const CellGcmZcullInfo*   cellGcmGetZcullInfo(void);
const CellGcmDisplayInfo* cellGcmGetDisplayInfo(void);

/* --- misc (cell/gcm.h:48) --- */
s32 cellGcmDumpGraphicsError(void);

/* ---------------------------------------------------------------------------
 * Host-side helpers (NOT SDK exports)
 * -----------------------------------------------------------------------*/

/* Host-side ticks. The game's host driver calls these periodically so
 * registered guest VBlank/Flip handlers fire via g_ps3_guest_caller and so
 * queued flips retire at vblank timing (many titles drive their title-screen
 * state machine from the VBlank handler). */
void cellGcmTickVBlank(void);
void cellGcmTickFlip(void);
void cellGcmDispatchUserCommand(u32 cause);

/* Shared monotonic timebase (ns) backing cellGcmGetTimeStamp/report timers.
 * Also used by rsx_commands.c's NV4097_GET_REPORT handler so a report timer
 * written via the command buffer and one read back via the API above are the
 * same clock. */
u64 cellGcmReportTimestampNs(void);

/* FIFO-consumer hook: publish a processed SET_REFERENCE value into the HLE
 * control register (ref). The HLE itself cannot see reference commands (they
 * travel in the FIFO), so the command processor calls this when it executes
 * one; cellGcmFinish-style guest spins on ctrl->ref then terminate. */
void cellGcmHleSetReference(u32 value);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_GCM_SYS_H */
