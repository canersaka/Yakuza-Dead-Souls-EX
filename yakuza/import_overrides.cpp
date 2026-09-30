/*
 * Hand-written import bridges that need ppu_context access (the generic
 * generated bridges only marshal gpr[3..10] -> args -> gpr[3]).
 *
 * Guest scratch layout used by the runner (above the game's last ELF segment,
 * which ends near 0x01730000):
 *   0x0D000000 - 0x0FE00000   _sys_heap bump allocator (~46 MB)
 *   0x0FDF0000 - ...          argv/envp block (main.cpp)
 *   0x0FE00000 - 0x0FF00000   main-thread TLS block
 *   0x0FF00000 - ...          synthetic import OPDs (yakuza_runner.h)
 *   0x10000000 - ...          RSX context window
 */

#include "ppu_recomp.h"
#include "yakuza_runner.h"
#include "rsx_wait_category.h"

#include "ps3emu/error_codes.h"
#include "ps3emu/yz_fifo_publication.h"
#include "ps3emu/yz_thread_placement.h"
#include "rsx_null_backend.h"   /* pulls rsx_commands.h: rsx_state, processor */
#include "rsx_prepared_run.h"
#include "rsx_live_draw.h"      /* live NV4097 -> D3D12 draw engine */
#include "movie_ffmpeg.h"       /* host FFmpeg movie decode (CRI Sofdec .sfd)   */
#include "../libs/audio/cellAudio.h"
#include "../libs/filesystem/cellFs.h"
#include "../libs/input/cellPad.h"
#include "../libs/system/cellSaveData.h"
#include "../libs/spurs/cellSpurs.h"

#include <cstdio>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>


extern "C" uint8_t* vm_base;
extern "C" uint32_t g_yz_game_toc;
extern "C" {
/* Set while the orphanage AUTH package (a010) is live: from its open until
 * the following a020 SFD opens.  LONG keeps Interlocked access well-defined. */
volatile LONG g_yz_a010_root_active = 0;
/* Set after an authored character pose has reached its live model palette.
 * The AUTH clock waits for this publication instead of outrunning
 * the asynchronous animation loads with every actor still at Y=10000. */
volatile LONG g_yz_a010_animation_ready = 0;
}

/* lv2 sys_event handlers (runtime/syscalls/sys_event.c); driven directly from
 * sys_rsx_context_allocate to set up libgcm's RSX event port/queue. */
extern "C" int64_t sys_event_port_create(ppu_context*);
extern "C" int64_t sys_event_queue_create(ppu_context*);
extern "C" int64_t sys_event_port_connect_local(ppu_context*);
extern "C" int64_t sys_event_port_send(ppu_context*);
extern "C" void yz_rsx_fifo_acquire(void);
extern "C" void yz_rsx_fifo_release(void);
extern "C" void cellGcmDispatchUserCommand(uint32_t cause);

/* The PPU ABI passes only arguments 1-8 in r3-r10. Arguments 9+ are in the
 * parameter save area at r1+0x30+i*8 (i is zero-based from r3). */
extern "C" void yz_ovr__cellSpursJobChainAttributeInitialize(ppu_context* ctx)
{
    auto stack_arg = [ctx](unsigned i) -> uint64_t {
        return vm_read64((uint32_t)ctx->gpr[1] + 0x30u + i * 8u);
    };
    const int32_t rc = _cellSpursJobChainAttributeInitialize(
        (uint32_t)ctx->gpr[3],
        (uint32_t)ctx->gpr[4],
        (CellSpursJobChainAttribute*)(vm_base + (uint32_t)ctx->gpr[5]),
        (const uint64_t*)(vm_base + (uint32_t)ctx->gpr[6]),
        (uint16_t)ctx->gpr[7],
        (uint16_t)ctx->gpr[8],
        (const uint8_t*)(vm_base + (uint32_t)ctx->gpr[9]),
        (uint32_t)ctx->gpr[10],
        (uint32_t)stack_arg(8),
        (uint32_t)stack_arg(9),
        (uint32_t)stack_arg(10),
        (uint32_t)stack_arg(11),
        (uint32_t)stack_arg(12),
        (uint32_t)stack_arg(13));
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

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
/* System time starts at 0 when the game boots, as on the console; the host
 * counter's own epoch would leak the PC's uptime into the game.  The lv2
 * timer subsystem (runtime/syscalls/sys_timer.c) uses the same clock for
 * timer bases and event stamps. */
extern "C" uint64_t yz_guest_system_time_us(void)
{
    static LARGE_INTEGER s_freq;
    static LARGE_INTEGER s_anchor;
    static int s_init = 0;

    if (!s_init) {
        QueryPerformanceFrequency(&s_freq);
        QueryPerformanceCounter(&s_anchor);
        s_init = 1;
    }

    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const int64_t d = c.QuadPart - s_anchor.QuadPart;
    return (uint64_t)((d * 1000000) / s_freq.QuadPart);
}

/* Register this clock with runtime/syscalls/sys_timer.c before main() runs
 * (dynamic initializer; import_overrides.cpp is always linked into the
 * runner). */
extern "C" uint64_t (*g_lv2_system_time_us)(void);
static const int s_yz_timer_clock_registered =
    (g_lv2_system_time_us = &yz_guest_system_time_us, 0);

extern "C" void yz_ovr_sys_time_get_system_time(ppu_context* ctx)
{
    ctx->gpr[3] = yz_guest_system_time_us();
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
 * sys_prx: runtime loading of the game's own engine PRX modules.
 *
 * The OgreZ engine sys_prx_load_module's its shader module (pxd_shader,
 * data/module/ps3/ogrez_shader_ps3.ppu.sprx) then sys_prx_start_module's it;
 * if both returned ENOSYS the shader subsystem would never init and the render
 * thread would spin-wait forever. The module is decrypted, relocated and lifted
 * statically (tools/lift_prx.py and the lifter; image at
 * 0x02200000), so here we just satisfy the prx ABI: load -> a handle,
 * start -> run the module's module_start (it sets up the shader subsystem the
 * engine waits on). pxd_shader exports only module_start/stop and imports 0,
 * so module_start is self-contained and safe to run inline on the caller. */
struct yz_prx_mod { uint32_t handle, start_opd, toc; int started; const char* name; };
static yz_prx_mod g_yz_prx_mods[8];
static int        g_yz_prx_nmods = 0;

static yz_prx_mod* yz_prx_find(uint32_t handle)
{
    for (int i = 0; i < g_yz_prx_nmods; i++)
        if (g_yz_prx_mods[i].handle == handle) return &g_yz_prx_mods[i];
    return nullptr;
}

static const char* yz_guest_cstr(uint32_t gaddr, char* buf, int n)
{
    int i = 0;
    if (gaddr >= 0x10000u && gaddr < 0xE0000000u)
        for (; i < n - 1; i++) { uint8_t c = vm_read8(gaddr + (uint32_t)i); buf[i] = (char)c; if (!c) break; }
    buf[i] = 0;
    return buf;
}

extern "C" void yz_ovr_sys_prx_load_module(ppu_context* ctx)
{
    char path[256];
    yz_guest_cstr((uint32_t)ctx->gpr[3], path, sizeof(path));
    uint32_t handle = 0, start_opd = 0, toc = 0; const char* nm = "?";
    if (strstr(path, "ogrez_shader")) {        /* pxd_shader: lifted + loaded */
        handle = 0x23000001u; start_opd = 0x0266AFD8u; toc = 0x02673020u; nm = "pxd_shader";
    }
    if (!handle) {                              /* module is not supported */
        fprintf(stderr, "[prx] load_module('%s') -> ENOSYS (module is not supported)\n", path);
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010003; /* CELL_ENOSYS */
        return;
    }
    if (!yz_prx_find(handle) && g_yz_prx_nmods < 8)
        g_yz_prx_mods[g_yz_prx_nmods++] = { handle, start_opd, toc, 0, nm };
    ctx->gpr[3] = handle;
}

extern "C" void yz_ovr_sys_prx_start_module(ppu_context* ctx)
{
    uint32_t handle = (uint32_t)ctx->gpr[3];
    uint32_t modres = (uint32_t)ctx->gpr[6];   /* int* out: module_start result */
    yz_prx_mod* m = yz_prx_find(handle);
    if (!m) { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010002; return; } /* EINVAL */
    if (!m->started) {
        m->started = 1;
        uint64_t a_args = ctx->gpr[4], a_argp = ctx->gpr[5];   /* start_module(id,args,argp,...) */
        ctx->gpr[3] = a_args;   /* module_start(size_t args, void* argp): pass the game's */
        ctx->gpr[4] = a_argp;
        yz_call_guest_opd(m->start_opd, ctx);   /* sets r2=module TOC, runs it, drains */
        uint32_t res = (uint32_t)ctx->gpr[3];
        if (modres >= 0x10000u && modres < 0xE0000000u) vm_write32(modres, res);
    }
    ctx->gpr[3] = 0;   /* CELL_OK */
}

extern "C" void yz_ovr_sys_prx_stop_module(ppu_context* ctx)     { ctx->gpr[3] = 0; }
extern "C" void yz_ovr_sys_prx_unload_module(ppu_context* ctx)   { ctx->gpr[3] = 0; }
extern "C" void yz_ovr_sys_prx_register_library(ppu_context* ctx){ ctx->gpr[3] = 0; }

/* DRM check: disc content has no NPDRM. Returning ENOSYS would send the game down its
 * DRM/trophy error path (-> cellMsgDialogOpen2). Report "available" (CELL_OK) so the
 * title sequence proceeds normally. */
extern "C" void yz_ovr_sceNpDrmIsAvailable(ppu_context* ctx) { ctx->gpr[3] = 0; }

/* Audio output. The OgreZ engine's early init spin-polls cellAudioOutGetState
 * until the output reports ENABLED + CELL_OK before it proceeds (the loop is in
 * func_00B1559C -> usleep). Stubbed ENOSYS => never ready => deadlock. Report a
 * configured stereo/48kHz LPCM primary output so the engine advances.
 * CellAudioOutState: state u8@0, encoder u8@1, reserved[6], downMixer be32@8, soundMode
 * {type u8@C, channel u8@D, fs u8@E, rsvd u8@F, layout be32@10}. */
extern "C" void yz_ovr_cellAudioOutGetState(ppu_context* ctx)
{
    uint32_t s = (uint32_t)ctx->gpr[5];   /* CellAudioOutState* */
    if (s >= 0x10000u && s < 0xE0000000u) {
        vm_write8(s + 0x0u, 0);      /* state   = CELL_AUDIO_OUT_OUTPUT_STATE_ENABLED (0) */
        vm_write8(s + 0x1u, 0);      /* encoder = CELL_AUDIO_OUT_CODING_TYPE_LPCM (0) */
        for (uint32_t i = 2; i < 8; i++) vm_write8(s + i, 0);   /* reserved[6] */
        vm_write32(s + 0x8u, 0);     /* downMixer = NONE */
        vm_write8(s + 0xCu, 0);      /* soundMode.type    = LPCM */
        vm_write8(s + 0xDu, 2);      /* soundMode.channel = 2 (stereo) */
        vm_write8(s + 0xEu, 0x04u);  /* soundMode.fs      = 48KHz */
        vm_write8(s + 0xFu, 0);      /* soundMode.reserved */
        vm_write32(s + 0x10u, 1);    /* soundMode.layout  = 2CH */
    }
    ctx->gpr[3] = 0;   /* CELL_OK */
}
extern "C" void yz_ovr_cellAudioOutConfigure(ppu_context* ctx) { ctx->gpr[3] = 0; }   /* CELL_OK */

/* ---------------------------------------------------------------------------
 * RSX driver: gcm HLE, sys_rsx syscalls and the FIFO consumer
 *
 * The game's cellGcmSys imports are implemented natively in yz_gcm_hle.inc
 * (yz_ovr_cellGcm*, with yz_gcm_fifo_callback as the command-buffer
 * callback). They set up the RSX through the lv2 sys_rsx syscalls
 * (668-677, 0x29C-0x2A5) in yz_sys_rsx.inc.
 *
 * The driver contract is a set of guest-memory structures laid out like the
 * lv2 sys_rsx ones:
 *   - context_allocate returns the dma_control / driver_info / reports
 *     base addresses and fills driver_info (version_driver 0x211,
 *     frequencies, offsets).
 *   - the game writes the FIFO PUT offset into dma_control (+0x40); our
 *     consumer reads PUT/GET there and executes the committed command stream.
 *     The game owns the ring, so there is no producer/consumer race.
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
 * it never collides with the game's main-memory heap. Committed on demand. */
#define RSX_CTX_BASE      0x10000000u
#define RSX_DMA_CONTROL   (RSX_CTX_BASE + 0x000000u)
#define RSX_DRIVER_INFO   (RSX_CTX_BASE + 0x100000u)
#define RSX_REPORTS       (RSX_CTX_BASE + 0x200000u)
#define RSX_DEVICE_ADDR   (RSX_CTX_BASE + 0x300000u)

static uint32_t g_rsx_local_mem_size = YZ_GCM_LOCAL_SIZE;
static int      g_rsx_ctx_ready      = 0;   /* context_allocate done */
static uint32_t g_rsx_event_port     = 0;   /* RSX event port (libgcm handler) */

/* io-offset -> guest EA map, 1 MB granularity (index = io >> 20), built by
 * sys_rsx_context_iomap. 0xFFFFFFFF = unmapped. */
static uint32_t g_rsx_iomap_ea[4096];
static int      g_rsx_iomap_init = 0;

/* EA base of the io command-buffer ring (the ioAddress the
 * game passed to _cellGcmInitBody). The ring maps linearly io X -> yz_gcm_io_addr+X,
 * so ea->io for the ring is `ea - yz_gcm_io_addr`. Set by yz_ovr__cellGcmInitBody. */
static uint32_t yz_gcm_io_addr = 0;
static uint32_t yz_gcm_io_size = 0;
static uint32_t g_yz_gcm_segment_bytes = 0;

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

/* Bump the flip counter the game's render throttle polls (guest 0x40C00000). */
static void yz_rsx_advance_flip_progress(uint32_t source)
{
    const uint32_t value = vm_read32(0x40C00000u) + 1u;
    vm_write32(0x40C00000u, value);
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
static volatile LONG g_yz_command_owner_enabled;
static int yz_rsx_command_owner_control(int enabled)
{
    InterlockedExchange(&g_yz_command_owner_enabled,enabled!=0);
    return enabled!=0;
}

/* Live-draw guest-memory resolver: map an RSX (location, offset) to a
 * host pointer into the guest address space. location 0 = RSX local VRAM (the
 * gcm local carve), 1 = main memory via the gcm io map; mirrors the DMA cases
 * in yz_rsx_sem_addr. Returns NULL for out-of-range/unmapped regions. */
static const u8* yz_rsx_live_guest_ptr(void* user, u32 location, u32 offset,
                                       u32 min_bytes)
{
    (void)user;
    /*
     * The live renderer promises its callers that the returned host pointer
     * covers min_bytes, so validate the whole interval rather than only its
     * first byte.  A mip span can begin inside local VRAM and end exactly at
     * 0xCF900000, the first uncommitted byte after the console's 249 MB
     * local-memory carve.
     */
    const uint64_t end = (uint64_t)offset + (uint64_t)min_bytes;
    if (end > 0x100000000ull)
        return nullptr;

    uint32_t ea;
    if (location == 0) {                         /* RSX local VRAM */
        const uint32_t local_size =
            g_rsx_local_mem_size < YZ_GCM_LOCAL_SIZE
                ? g_rsx_local_mem_size : YZ_GCM_LOCAL_SIZE;
        if (offset >= local_size || end > local_size)
            return nullptr;
        ea = YZ_GCM_LOCAL_BASE + offset;
    } else if (location == 1) {                  /* main memory via io map */
        ea = yz_rsx_io_to_ea(offset);
        if (!ea) return nullptr;
        if (min_bytes) {
            const uint64_t last_io64 = end - 1u;
            if (last_io64 > UINT32_MAX)
                return nullptr;
            const uint32_t last_ea =
                yz_rsx_io_to_ea((uint32_t)last_io64);
            if (!last_ea ||
                (uint64_t)last_ea !=
                    (uint64_t)ea + (uint64_t)min_bytes - 1u)
                return nullptr;

            /* IO pages are independently mapped.  Matching endpoints are not
             * sufficient when a multi-page texture crosses a discontiguous or
             * unmapped page in between. */
            uint64_t boundary =
                ((uint64_t)offset + 0x100000ull) & ~0xFFFFFull;
            for (; boundary < end; boundary += 0x100000ull) {
                const uint32_t boundary_ea =
                    yz_rsx_io_to_ea((uint32_t)boundary);
                if (!boundary_ea ||
                    (uint64_t)boundary_ea !=
                        (uint64_t)ea + boundary - offset)
                    return nullptr;
            }
        }
    } else {
        return nullptr;
    }
    return (const u8*)(vm_base + ea);
}

/* The overrides are one translation unit: the parts below share the state
 * above and are included once, in dependency order. */
#include "yz_movie_host.inc"
#include "yz_rsx_methods.inc"
#include "yz_rsx_stopper_repair.inc"
#include "yz_rsx_fifo.inc"
#include "yz_gcm_hle.inc"
#include "yz_sys_rsx.inc"

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

/* ---------------------------------------------------------------------------
 * cellSaveDataListAutoLoad(version, errDialog, setList, setBuf, funcFixed,
 *                          funcStat, funcFile, container, userdata)
 *
 * This legacy nine-argument entry is the New Game gate used by Yakuza: Dead
 * Souls.  The generic import bridge only forwards the eight register arguments
 * and would bind this NID to CELL_ENOSYS, so neither callback would run and
 * the title state machine would wait forever on a black screen. The ninth argument
 * lives in the caller parameter-save area rather than a GPR.
 * -----------------------------------------------------------------------*/
extern "C" void yz_ovr_cellSaveDataListAutoLoad(ppu_context* ctx)
{
    const uint32_t set_list_ea = (uint32_t)ctx->gpr[5];
    const uint32_t set_buf_ea  = (uint32_t)ctx->gpr[6];
    const uint32_t userdata_ea = (uint32_t)vm_read64(ctx->gpr[1] + 0x70);

    const int32_t rc = cellSaveDataListAutoLoad(
        (uint32_t)ctx->gpr[3],
        (uint32_t)ctx->gpr[4],
        set_list_ea ? (CellSaveDataSetList*)(vm_base + set_list_ea) : NULL,
        set_buf_ea  ? (CellSaveDataSetBuf*)(vm_base + set_buf_ea) : NULL,
        (CellSaveDataFixedCallback)(uintptr_t)(uint32_t)ctx->gpr[7],
        (CellSaveDataStatCallback)(uintptr_t)(uint32_t)ctx->gpr[8],
        (CellSaveDataFileCallback)(uintptr_t)(uint32_t)ctx->gpr[9],
        (uint32_t)ctx->gpr[10],
        (void*)(uintptr_t)userdata_ea);

    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

extern "C" int32_t sys_lwcond_wait(void*, uint64_t);
extern "C" void yz_ovr_sys_lwcond_wait(ppu_context* ctx)
{
    const uint32_t cond = (uint32_t)ctx->gpr[3];
    const int32_t rc = sys_lwcond_wait(cond ? vm_base + cond : nullptr, ctx->gpr[4]);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

/* sys_lwmutex_destroy takes a guest pointer; translate it for the libs
 * implementation. */
extern "C" int32_t sys_lwmutex_destroy(void* lwmutex);

extern "C" void yz_ovr_sys_lwmutex_destroy(ppu_context* ctx)
{
    void* p = ctx->gpr[3] ? (void*)(vm_base + (uint32_t)ctx->gpr[3]) : NULL;
    ctx->gpr[3] = (uint64_t)(int64_t)sys_lwmutex_destroy(p);
}

/* ---------------------------------------------------------------------------
 * sys_spu_image_import(img*, src, type): user-level SPU ELF loader.
 *
 * Imports an SPU ELF (ELF32 BE, EM_SPU) held in guest memory into a
 * sys_spu_image. Semantics (direct import path; get_nsegs/fill):
 *   - per PT_LOAD: COPY segment {ls=p_vaddr, size=p_filesz, addr=src+p_offset}
 *     plus FILL {ls=p_vaddr+p_filesz, size=p_memsz-p_filesz, value=0} for bss;
 *   - per PT_NOTE (p_type 4): INFO segment {size=0x20, addr=src+p_offset+0x14};
 *   - any other p_type: ENOEXEC.
 * Guest structs (BE): sys_spu_image {type@0=USER, entry@4, segs@8, nsegs@12},
 * sys_spu_segment 0x18 bytes {type@0, ls@4, size@8, addr/value@0x10}.
 * The segment table is consumed at thread-group start when the kernel image
 * is deployed to SPU local store.
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

    /* Count segments */
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

    /* The segment count is capped at 0x20: a malformed or adversarial phdr
     * table must fail with ENOMEM instead of driving an oversized heap
     * allocation. */
    if (nsegs <= 0 || nsegs > 0x20) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_ENOMEM;
        return;
    }

    uint32_t segs_ea = yz_heap_alloc((uint32_t)nsegs * 0x18u, 16);
    if (!segs_ea) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ENOMEM;
        return;
    }

    /* Fill segments */
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

    ctx->gpr[3] = 0;
}

/* sys_spu_image_close: our USER images keep their segment tables in the
 * runner bump heap (never reclaimed), so close is success/no-op. */
extern "C" void yz_ovr_sys_spu_image_close(ppu_context* ctx)
{
    ctx->gpr[3] = 0;
}
