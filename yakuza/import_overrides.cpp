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
#include "edge_journal_hle.h"

#include "ps3emu/error_codes.h"
#include "rsx_null_backend.h"   /* pulls rsx_commands.h: rsx_state, processor */
#include "rsx_live_draw.h"      /* Track B: live NV4097 -> D3D12 draw engine */
#include "movie_ffmpeg.h"       /* host FFmpeg movie decode (CRI Sofdec .sfd)   */

#include <cstdio>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" uint8_t* vm_base;
extern "C" void yz_w2life_dump(const char*);   /* s31 W2LIFE probe (spu_channels.c) */
/* s40b v2: the GPU-parked stopper EA, published by the FIFO park tracker below for
 * the SPU-side targeted unstick (gs_task.c YZ_QROT_UNSTICK). 0 = not parked >2s. */
extern "C" { uint32_t g_yz_parked_pub_ea = 0; }

/* lv2 sys_event handlers (runtime/syscalls/sys_event.c) -- driven directly from
 * sys_rsx_context_allocate to set up libgcm's RSX event port/queue. */
extern "C" int64_t sys_event_port_create(ppu_context*);
extern "C" int64_t sys_event_queue_create(ppu_context*);
extern "C" int64_t sys_event_port_connect_local(ppu_context*);
extern "C" int64_t sys_event_port_send(ppu_context*);
extern "C" void yz_hwwatch_arm(void);   /* main.cpp: DR0 watch on the wid4 record slot */
extern HANDLE g_yz_t1_handle;           /* main.cpp: t1 real handle ([t1-hb]) */
extern ppu_context* g_yz_main_ctx;      /* main.cpp: t1's guest context */
extern int g_yz_updloop_started;        /* main.cpp: first func_00D1E838 entry (ledger #64) */
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
    /* U1/U2 fix (2026-07-09): the old code returned microseconds since the
     * QPC counter's own epoch, i.e. leaked this PC's uptime straight into
     * the game -- RPCS3 sys_time.cpp:191-192 calls this out explicitly
     * ("Add an offset to get_timebased_time to avoid leaking PC's uptime
     * into the game / As if PS3 starts at value 0 (base time) when the game
     * boots"). Cache the QPC frequency once and report elapsed time since a
     * first-call anchor instead of raw QPC. Kill-switch YZ_NO_TIMEANCHOR
     * (shared with sys_timer.c's sys_time_get_current_time anchor) restores
     * the old raw-QPC (host-uptime) behaviour for A/B. */
    static LARGE_INTEGER s_freq;
    static LARGE_INTEGER s_anchor;
    static int s_init = 0;
    static int s_no_anchor = -1;

    if (s_no_anchor < 0) {
        s_no_anchor = getenv("YZ_NO_TIMEANCHOR") ? 1 : 0;
        fprintf(stderr, "[yz_time] system_time armed (anchor-to-boot %s)\n",
                s_no_anchor ? "DISABLED by YZ_NO_TIMEANCHOR" : "on");
        fflush(stderr);
    }

    if (!s_init) {
        QueryPerformanceFrequency(&s_freq);
        QueryPerformanceCounter(&s_anchor);
        s_init = 1;
    }

    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    int64_t d = s_no_anchor ? c.QuadPart : (c.QuadPart - s_anchor.QuadPart);
    ctx->gpr[3] = (uint64_t)((d * 1000000) / s_freq.QuadPart);
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
 * sys_prx: runtime loading of the GAME's own engine PRX modules (pt26).
 *
 * The OgreZ engine sys_prx_load_module's its shader module (pxd_shader,
 * data/module/ps3/ogrez_shader_ps3.ppu.sprx) then sys_prx_start_module's it;
 * with both stubbed ENOSYS the shader subsystem never inits and the render
 * thread spin-waits forever (the post-logo stall). We decrypt + relocate +
 * lift the module statically (tools/decrypt_self.py -> lift_prx -> the lifter;
 * image at 0x02200000), so here we just satisfy the prx ABI: load -> a handle,
 * start -> RUN the module's module_start (it sets up the shader subsystem the
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
    if (!handle) {                              /* not LLE'd yet (e.g. dfengine) */
        fprintf(stderr, "[prx] load_module('%s') -> ENOSYS (module not LLE'd yet)\n", path);
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010003; /* CELL_ENOSYS */
        return;
    }
    if (!yz_prx_find(handle) && g_yz_prx_nmods < 8)
        g_yz_prx_mods[g_yz_prx_nmods++] = { handle, start_opd, toc, 0, nm };
    fprintf(stderr, "[prx] load_module('%s') -> handle 0x%08X (%s)\n", path, handle, nm);
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
        fprintf(stderr, "[prx] start_module 0x%08X: running %s module_start(args=0x%llX argp=0x%llX) opd 0x%08X\n",
                handle, m->name, (unsigned long long)a_args, (unsigned long long)a_argp, m->start_opd);
        ctx->gpr[3] = a_args;   /* module_start(size_t args, void* argp) -- pass the game's */
        ctx->gpr[4] = a_argp;
        yz_call_guest_opd(m->start_opd, ctx);   /* sets r2=module TOC, runs it, drains */
        uint32_t res = (uint32_t)ctx->gpr[3];
        fprintf(stderr, "[prx] %s module_start returned 0x%08X\n", m->name, res);
        if (modres >= 0x10000u && modres < 0xE0000000u) vm_write32(modres, res);
    }
    ctx->gpr[3] = 0;   /* CELL_OK */
}

extern "C" void yz_ovr_sys_prx_stop_module(ppu_context* ctx)     { ctx->gpr[3] = 0; }
extern "C" void yz_ovr_sys_prx_unload_module(ppu_context* ctx)   { ctx->gpr[3] = 0; }
extern "C" void yz_ovr_sys_prx_register_library(ppu_context* ctx){ ctx->gpr[3] = 0; }

/* DRM check: disc content has no NPDRM. Returning ENOSYS made the game take its
 * DRM/trophy error path (-> cellMsgDialogOpen2). Report "available" (CELL_OK) so the
 * title sequence proceeds normally. (pt26 stub-fix test for the post-logo stall.) */
extern "C" void yz_ovr_sceNpDrmIsAvailable(ppu_context* ctx) { ctx->gpr[3] = 0; }

/* Audio output (pt26 — THE post-frame-3 black-screen gate). The OgreZ engine's early
 * init spin-polls cellAudioOutGetState until the output reports ENABLED + CELL_OK
 * before it proceeds (proven: t1.ctr = the cellAudioOutGetState import fake-key
 * 0xFE00018C, looping in func_00B1559C -> usleep). Stubbed ENOSYS => never ready =>
 * deadlock. Report a configured stereo/48kHz LPCM primary output so the engine advances.
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
/* pt35: the cri_audio codec taskset (wid 3, e.g. 0x63D22580), captured at
 * CreateTaskWithAttr so we can dump its task_info[] and check whether the codec
 * ELF (0x012B4980) actually got written into the taskset (the elf=0 at StartTask). */
extern "C" { uint32_t g_yz_codec_taskset = 0; }

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

/* ===========================================================================
 * YZ_FLIPTRACE (s21, flip-label stall diagnosis -- STATUS 1a). Uncapped,
 * sequence-stamped event log of the flip-label lifecycle: every arm / clear /
 * acquire / queue event touching the flip semaphore (label 0x10200010 =
 * RSX_REPORTS+0x10) or the HW flip credit (device+0x30), plus a value-
 * transition WATCHER thread that catches writers OUTSIDE the instrumented
 * paths (a plain guest store to the label would otherwise be invisible).
 * All events share one seq counter + QPC clock so cross-thread ORDER is
 * recoverable from the log. Diagnostic only, default OFF.
 * =========================================================================*/
#include <stdarg.h>
static int yz_ft_flag = -1;
static inline int yz_ft_on(void)
{
    if (yz_ft_flag < 0) yz_ft_flag = getenv("YZ_FLIPTRACE") ? 1 : 0;
    return yz_ft_flag;
}
static volatile LONG g_ft_seq = 0;
static double        g_ft_qpf = 0.0;
static LONGLONG      g_ft_t0  = 0;
static void yz_ft(const char* fmt, ...)
{
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (!g_ft_qpf) {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        g_ft_qpf = (double)f.QuadPart; g_ft_t0 = now.QuadPart;
    }
    double ms = (double)(now.QuadPart - g_ft_t0) * 1000.0 / g_ft_qpf;
    char buf[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[ft] #%ld t=%.3f tid=%lu %s\n",
            InterlockedIncrement(&g_ft_seq), ms, GetCurrentThreadId(), buf);
}
/* Label-value watcher: ~ms-grain poll of the flip label; logs every value
 * transition, so an arm/clear from ANY writer (including one no instrumented
 * path covers) appears in the same seq/timestamp stream. Sub-ms pulses can
 * alias between polls -- instrumented writers still log their own events. */
static DWORD WINAPI yz_ft_watch_thread(LPVOID)
{
    uint32_t last = vm_read32(RSX_REPORTS + 0x10);
    yz_ft("WATCHER ARMED label@0x%08X initial=0x%08X", RSX_REPORTS + 0x10, last);
    for (;;) {
        for (int i = 0; i < 64; i++) {
            uint32_t v = vm_read32(RSX_REPORTS + 0x10);
            if (v != last) {
                yz_ft("label 0x%08X -> 0x%08X (watcher) pending=[%ld %ld] qhead=%u",
                      last, v, g_rsx_flip_pending[0], g_rsx_flip_pending[1],
                      g_rsx_queued_head);
                last = v;
            }
            YieldProcessor();
        }
        Sleep(1);
    }
    return 0;
}
static void yz_ft_start(void)   /* called from the vblank tick (ctx is up) */
{
    static int started = 0;
    if (!started && yz_ft_on()) {
        started = 1;
        yz_ft("ARMED (YZ_FLIPTRACE): label=0x%08X devcredit=0x%08X",
              RSX_REPORTS + 0x10, RSX_DEVICE_ADDR + 0x30);
        CreateThread(NULL, 0, yz_ft_watch_thread, NULL, 0, NULL);
    }
}

/* Track B live-draw guest-memory resolver: map an RSX (location, offset) to a
 * host pointer into the guest address space. location 0 = RSX local VRAM (the
 * gcm local carve), 1 = main memory via the gcm io map -- mirrors the DMA cases
 * in yz_rsx_sem_addr. Returns NULL for out-of-range/unmapped regions. */
static const u8* yz_rsx_live_guest_ptr(void* user, u32 location, u32 offset,
                                       u32 min_bytes)
{
    (void)user; (void)min_bytes;
    uint32_t ea;
    if (location == 0) {                         /* RSX local VRAM */
        if (offset >= YZ_GCM_LOCAL_SIZE) return nullptr;
        ea = YZ_GCM_LOCAL_BASE + offset;
    } else {                                     /* main memory via io map */
        ea = yz_rsx_io_to_ea(offset);
        if (!ea) return nullptr;
    }
    return (const u8*)(vm_base + ea);
}

/* Dedicated window thread. A Win32 window must be created AND message-pumped on
 * the same thread, so it lives here rather than on the main/consumer threads.
 * rsx_null_backend_init() opens the window and registers the null backend (GDI
 * clear-color present); the consumer + flip path then drive it. When YZ_RSX_DRAW
 * is set, Track B's live NV4097->D3D12 engine binds a swap chain to that same
 * HWND and takes over presentation (the null GDI present is suppressed). */
static DWORD WINAPI yz_window_thread(LPVOID)
{
    if (rsx_null_backend_init(1280, 720, "Yakuza: Dead Souls (ps3recomp)") != 0) {
        fprintf(stderr, "[rsx] window init failed\n");
        return 1;
    }
    if (rsx_live_draw_enabled()) {
        int r = rsx_live_draw_init(rsx_null_backend_get_hwnd(), 1280, 720,
                                   yz_rsx_live_guest_ptr, nullptr);
        if (r == 0) {
            rsx_null_backend_suppress_present(1);
            fprintf(stderr, "[rsx] live-draw engine up (D3D12); null GDI present suppressed\n");
        } else {
            fprintf(stderr, "[rsx] live-draw init FAILED (%d) -> falling back to null present\n", r);
        }
    }

    /* YZ_MOVIE_TEST=<path.sfd>: standalone proof of the host movie path -- decode
     * the movie with FFmpeg and present it straight to the D3D12 window (movie
     * mode gates the guest's draws off). Not the game hook; just proves
     * decode -> present in-process. */
    const char* mvpath = getenv("YZ_MOVIE_TEST");
    if (mvpath && *mvpath && rsx_live_draw_enabled()) {
        MoviePlayer* mv = movie_open(mvpath);
        if (mv) {
            const uint32_t w = (uint32_t)movie_width(mv), h = (uint32_t)movie_height(mv);
            int fps = (int)(movie_framerate(mv) + 0.5); if (fps <= 0) fps = 30;
            const DWORD frame_ms = (DWORD)(1000 / fps);
            fprintf(stderr, "[movie] YZ_MOVIE_TEST playing %s (%ux%u @ %dfps)\n", mvpath, w, h, fps);
            rsx_live_draw_set_movie_mode(1);
            int n = 0;
            for (;;) {
                const uint8_t* rgba = movie_next_rgba(mv, nullptr);
                if (!rgba) break;                          /* end of stream */
                rsx_live_draw_present_rgba(rgba, w, h);
                if (n == 30) {   /* dump one presented frame as proof (RGB PPM) */
                    FILE* f = fopen("scratch/movie_runtime.ppm", "wb");
                    if (f) { fprintf(f, "P6\n%u %u\n255\n", w, h);
                        for (uint32_t i = 0; i < w * h; i++) fwrite(rgba + i * 4, 1, 3, f);
                        fclose(f); fprintf(stderr, "[movie] wrote scratch/movie_runtime.ppm (frame 30)\n"); }
                }
                if (rsx_null_backend_pump_messages() < 0) break;
                Sleep(frame_ms);
                n++;
            }
            fprintf(stderr, "[movie] done (%d frames presented)\n", n);
            movie_close(mv);
            rsx_live_draw_set_movie_mode(0);
        } else {
            fprintf(stderr, "[movie] movie_open('%s') failed (ffmpeg_available=%d)\n",
                    mvpath, movie_ffmpeg_available());
        }
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
 * The game's inline flush/finish routine writes ctrl->put and spins until
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

/* NV3062 (2D surface) + NV308A (image-from-cpu) state: cellGcmInlineTransfer
 * writes data words into memory through the 2D blit
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

extern "C" int64_t yz_sys_rsx_context_attribute(ppu_context*);   /* defined below */

/* Lossless user-interrupt delivery (s25, the round-25 stall root — the FIFTH
 * dropped-guest-notification instance after 0xEB00/0xE920/flip/0x103).
 * MEASURED (scratch/s25ride.err): the game COALESCES its ucmd cause counter
 * (deliveries 1-21 sequential, then one method carrying cause=25 — a
 * documented rapid-user-command coalescing behavior), and that single coalesced
 * send hit a momentarily FULL RSX event queue: sys_event_port_send returned
 * 0x8001000A (EBUSY) and our fire-and-forget path LOST it. The handler never
 * saw cause 25, the wid4 pool never published rounds 22-25, and the stream
 * parked forever on NV406E SEMAPHORE_ACQUIRE want=25 — WITHOUT the 0x2004
 * death (refutes ledger #49's prime-mover attribution). lv1 keeps ONE pending
 * cause register and re-delivers when the queue drains; model that: latch the
 * undelivered cause, retry from the consumer loop (userCmdParam already
 * carries the latest arg, so a retry delivers correct coalesced state).
 * Consumer-thread-only state, no locking. Kill-switch YZ_NO_UCMD_RETRY. */
/* s25 GENERALIZED (post notification-surface audit, scratch/
 * s25_notification_audit.md risk #1): the latch now covers ANY event bits
 * sent to the RSX port, not just USER_CMD — a failed send ORs its cause
 * bits into one pending mask (exactly how the vblank path already delivers
 * multiple causes in one send) and the consumer-top retry sends the
 * accumulated mask once the queue drains. The user-cmd cause arg still
 * travels via driverInfo.userCmdParam (latest-wins, lv1 coalescing). */
/* 0 = none; else latched cause bits. s26: retried from BOTH the consumer loop
 * top and the vblank tick (the consumer-top-only retry deadlocked when the
 * lost delivery parked the consumer itself — s26ride4), so take-and-retry is
 * atomic: one thread wins the exchange, a failed send re-ORs the bits back.
 * Prevents double-delivery of the same latched cause. */
static volatile long long g_rsx_ev_pending = 0;

static int64_t yz_rsx_ev_send(uint64_t bits)
{
    ppu_context sc; memset(&sc, 0, sizeof(sc));
    sc.gpr[3] = g_rsx_event_port;
    sc.gpr[5] = bits;
    int64_t r = sys_event_port_send(&sc);
    if (r != 0) {
        static int no_retry = -1;
        if (no_retry < 0) no_retry = (getenv("YZ_NO_UCMD_RETRY")
                                      || getenv("YZ_NO_EV_RETRY")) ? 1 : 0;
        if (!no_retry) {
            _InterlockedOr64(&g_rsx_ev_pending, (long long)bits);
            fprintf(stderr, "[rsx-ev] send FAILED r=0x%llX bits=0x%llX -> LATCHED "
                    "for retry (queue full; lossless delivery, s25 fix)\n",
                    (unsigned long long)(uint64_t)r, (unsigned long long)bits);
            fflush(stderr);
        }
    }
    return r;
}

static void yz_ucmd_retry_pending(void)
{
    if (!g_rsx_ev_pending || !g_rsx_event_port) return;
    uint64_t bits = (uint64_t)_InterlockedExchange64(&g_rsx_ev_pending, 0);
    if (!bits) return;                     /* another thread took it */
    ppu_context sc; memset(&sc, 0, sizeof(sc));
    sc.gpr[3] = g_rsx_event_port;
    sc.gpr[5] = bits;
    int64_t r = sys_event_port_send(&sc);
    if (r == 0) {
        fprintf(stderr, "[rsx-ev] RETRY delivered latched bits=0x%llX (queue drained)\n",
                (unsigned long long)bits);
        fflush(stderr);
    } else {
        /* still full — put the bits back; log sparsely so a permanently-
         * wedged intr thread is visible without flooding */
        _InterlockedOr64(&g_rsx_ev_pending, (long long)bits);
        static unsigned long rn = 0; rn++;
        if ((rn & 0x3FFu) == 1) {
            fprintf(stderr, "[rsx-ev] retry still failing bits=0x%llX r=0x%llX (n=%lu)\n",
                    (unsigned long long)bits,
                    (unsigned long long)(uint64_t)r, rn);
            fflush(stderr);
        }
    }
}

static int yz_rsx_method(uint32_t method, uint32_t arg)
{
    /* Deliver any latched (queue-full) RSX event bits before consuming more
     * of the stream — one predicted-not-taken branch when idle. */
    if (g_rsx_ev_pending) yz_ucmd_retry_pending();

    /* GCM_FLIP_HEAD arm banner on the consumer's FIRST method, not the first
     * 0xE920 hit: a "0xE920 never seen" negative is only MEASURED if the log
     * proves the probe ran (docs/LESSONS.md, the s22 honesty rules). */
    static int nf = -1;
    if (nf < 0) { nf = getenv("YZ_NO_FLIPHEAD") ? 1 : 0;
        fprintf(stderr, "[fliphead] armed (immediate-flip dispatch %s)\n",
                nf ? "DISABLED by YZ_NO_FLIPHEAD" : "on"); fflush(stderr); }

    /* Track B live draw: mirror the full method stream into the NV4097->D3D12
     * engine (no-op unless YZ_RSX_DRAW + init succeeded). It accumulates
     * clears/state/geometry and self-presents on the 0xE944 flip method. This
     * runs before the plumbing below so the engine also sees the flip. */
    rsx_live_draw_method(method, arg);

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
        g_rsx_queued_head = head;
        { static int n = 0; if (n < 8) { n++;
            fprintf(stderr, "[rsx] DRIVER_QUEUE head=%u buf=%u (flip queued)\n", head, arg); } }
        if (yz_ft_on()) yz_ft("QUEUE head=%u buf=%u", head, arg);
        /* s23: route through the pkg-0x103 syscall case instead of the old
         * inline duplicate -- RPCS3's gcm::queue_flip method IS that shim
         * (rsx_methods.cpp:101-113 -> sys_rsx 0x103), and the syscall case now
         * also delivers the queue event; boot9 measured the game queues ONLY
         * via this method (the 0x103 syscall itself never fires), so the
         * event delivery was dead until this bridge. */
        ppu_context sc; memset(&sc, 0, sizeof(sc));
        sc.gpr[4] = 0x103;
        sc.gpr[5] = head;
        sc.gpr[6] = arg;
        yz_sys_rsx_context_attribute(&sc);
        return 0;
    }

    /* GCM_FLIP_HEAD (0xE920 + head*4): the IMMEDIATE display flip driven from
     * the GPU command stream (the cellGcmSetFlipImmediate path) -- sibling of
     * the DRIVER_QUEUE path above and the same lv1 driver-method family as the
     * 0xEB00 user interrupt fixed in s22. Our consumer silently dropped it
     * (method coverage audit 2026-07-08, top-ranked gap of blocker #20's
     * missing-mechanism class). RPCS3 binds it as a thin shim onto the SAME
     * syscall our pkg-0x102 case already implements: rsx_methods.cpp:1729
     * bind_range<GCM_FLIP_HEAD,1,2,gcm::driver_flip> ->
     * sys_rsx_context_attribute(0x102, head, arg) (rsx_methods.cpp:93-99,
     * sys_rsx.cpp:574-627: arg bit31 = grab-queued-buffer, else display-buffer
     * offset). Route it there. Kill-switch YZ_NO_FLIPHEAD. */
    if (method == 0xE920 || method == 0xE924) {
        uint32_t head = (method - 0xE920) >> 2;
        static unsigned long fn = 0; fn++;
        if (fn <= 16 || (fn & 0x3FFu) == 0) {
            fprintf(stderr, "[fliphead] n=%lu head=%u arg=0x%08X%s\n",
                    fn, head, arg, nf ? " (DROPPED by YZ_NO_FLIPHEAD)" : "");
            fflush(stderr);
        }
        if (yz_ft_on()) yz_ft("FLIP_HEAD head=%u arg=0x%08X", head, arg);
        if (!nf) {
            ppu_context sc; memset(&sc, 0, sizeof(sc));
            sc.gpr[4] = 0x102;                 /* pkg: display flip */
            sc.gpr[5] = head;
            sc.gpr[6] = arg;
            yz_sys_rsx_context_attribute(&sc);
        }
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
    case 0xEB00:                                  /* GCM_SET_USER_COMMAND (user interrupt) */
    case 0xEB04: {
        /* s22 ROOT FIX (2026-07-08): the RSX USER INTERRUPT — the mechanism the
         * game uses to pump its wid4 SPU decode pool. The game registers
         * func_00E7DB10 (the ONLY _cellSpursSendSignal path in the EBOOT) via
         * cellGcmSetUserHandler (handlers bit 0x80, measured 0x86 live), then
         * queues THIS method in the frame stream; the consumer must deliver it
         * to Sony's _gcm_intr_thread, which calls the handler with the cause
         * arg. We silently dropped it — the pool starved and the movie-boundary
         * bootstrap deadlocked on the 0xFE0 decode label (DONT_RECHASE #29/#31).
         * Oracle: RPCS3 gcm_enums.h:821 (0xEB00 "User interrupt"),
         * rsx_methods.cpp:68/1728 (user_command, bound over 0xEB00-0xEB04),
         * sys_rsx.cpp:931 case 0xFEF (userCmdParam=arg + send_event
         * SYS_RSX_EVENT_USER_CMD=1<<7), sys_rsx.h:46 (userCmdParam @ +0x12CC).
         * Same delivery path as our vblank/flip events. Kill-switch YZ_NO_UCMD. */
        static int nu = -1; if (nu < 0) { nu = getenv("YZ_NO_UCMD") ? 1 : 0;
            fprintf(stderr, "[ucmd] armed (user-interrupt dispatch %s)\n",
                    nu ? "DISABLED by YZ_NO_UCMD" : "on"); fflush(stderr); }
        if (nu) break;
        vm_write32(RSX_DRIVER_INFO + 0x12CC, arg);       /* driverInfo.userCmdParam */
        if (g_rsx_event_port) {
            /* A newer cause supersedes any latched one (lv1 coalescing: ONE
             * pending cause register, latest wins; userCmdParam above already
             * carries it). Failure latches into the shared pending mask. */
            int64_t r = yz_rsx_ev_send(0x80ull);          /* SYS_RSX_EVENT_USER_CMD */
            static unsigned long un = 0; un++;
            if (un <= 40 || (un & 0xFFu) == 0) {
                fprintf(stderr, "[ucmd] n=%lu cause=0x%08X handlers=0x%08X send=%lld\n",
                        un, arg, vm_read32(RSX_DRIVER_INFO + 0x12C0), (long long)r);
                fflush(stderr);
            }
        } else {
            static int w = 0; if (w < 4) { w++;
                fprintf(stderr, "[ucmd] DROPPED cause=0x%08X (no event port yet)\n", arg);
                fflush(stderr); }
        }
        break; }
    case 0x050:                                   /* NV406E SET_REFERENCE */
        /* FAITHFUL FENCE TIMING (env YZ_RSX_FENCE_SYNC, opt-in A/B). RPCS3's
         * nv406e::set_reference calls sync() -- flush + wait for the GPU pipeline
         * -- BEFORE writing REF, so the game's REF poll blocks until the GPU has
         * really caught up. Our async consumer otherwise writes REF instantly and
         * races ahead of real GPU time (measured via the PPU trace-diff at
         * func_00EBBFB4: ours skips the fence wait RPCS3 performs). Flush the
         * D3D12 backend (a real GPU fence wait) first to pace the consumer to
         * actual GPU completion, matching RPCS3. */
        { static int fs = -1; if (fs < 0) fs = getenv("YZ_RSX_FENCE_SYNC") ? 1 : 0;
          if (fs) { extern void rsx_live_draw_flush(void); rsx_live_draw_flush(); } }
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
        /* fliptrace: log the flip-protocol acquires (label / HW credit) on every
         * pass and on stall-episode ENTRY (retries of the same stalled acquire
         * are suppressed so the log stays readable). s21 widened: ANY label-area
         * acquire (0x102000xx) -- the movie phase gates its stream on a decode-
         * sync label at +0xFE0 (boot 9/12 terminal state). */
        if (yz_ft_on() &&
            ((addr >= RSX_REPORTS && addr < RSX_REPORTS + 0x1000u) ||
             addr == RSX_DEVICE_ADDR + 0x30)) {
            uint32_t have = vm_read32(addr);
            int ok = (have == arg);
            static uint32_t fla = 0; static int flr = -1;
            if (addr != fla || ok != flr) { fla = addr; flr = ok;
                yz_ft("ACQ addr=0x%08X want=0x%08X have=0x%08X %s",
                      addr, arg, have, ok ? "pass" : "STALL-enter"); }
        }
        if (addr && vm_read32(addr) != arg) {
            /* s26 ride16 discriminator: the wedge acquire never passed even
             * after the wanted value was published — is this path still being
             * RETRIED (and reading what?), or does the caller stop re-invoking
             * (park upstream)? Heartbeat every 64k retries of the same
             * (addr,want): proves retry liveness + the value actually read
             * (LESSONS #6d: episode-entry dedup hides both). */
            { static uint32_t ha=0, hw=0; static unsigned long hn=0;
              if (addr==ha && arg==hw) { hn++;
                  if ((hn & 0xFFFFu)==0) {
                      fprintf(stderr, "[sem-hb] addr=0x%08X want=0x%08X read=0x%08X retries=%lu\n",
                              addr, arg, vm_read32(addr), hn);
                      fflush(stderr); } }
              else { ha=addr; hw=arg; hn=0; } }
            return 1;                             /* not yet satisfied: stall, retry later */
        }
        break;
    case 0x06C:                                   /* NV406E SEMAPHORE_RELEASE */
        addr = yz_rsx_sem_addr(yz_rsx_sem_dma_406e, yz_rsx_sem_off_406e);
        /* HW flip-sync: a release of 0 to device+0x30 is written as 1 (the RSX
         * never writes 0 there without a display-queue command). nv406e.cpp:130 */
        if (addr == RSX_DEVICE_ADDR + 0x30 && arg == 0) arg = 1;
        { static int sl=0; if (sl<60){ sl++;
            fprintf(stderr, "[sem] RELEASE off=0x%X addr=0x%08X val=0x%08X\n",
                    yz_rsx_sem_off_406e, addr, arg); } }
        if (yz_ft_on() &&
            ((addr >= RSX_REPORTS && addr < RSX_REPORTS + 0x1000u) ||
             addr == RSX_DEVICE_ADDR + 0x30))
            yz_ft("REL addr=0x%08X val=0x%08X qhead=%u pending[qh]=%ld",
                  addr, arg, g_rsx_queued_head,
                  g_rsx_flip_pending[g_rsx_queued_head & 7u]);
        if (addr)
            yz_rsx_w32(addr, arg);
        /* COMPENSATING HEURISTIC (pre-fliphead era, s23 gated): a release of the
         * flip semaphore (label+0x10) to the pending marker arms the queued head
         * so the next vblank presents it. RPCS3's nv406e::semaphore_release does
         * NOT do this -- the real flip is commanded by GCM_FLIP_HEAD (0xE920),
         * which we now deliver (s23). While this stays on, any non-flip label
         * write manufactures a phantom flip (uncommanded present + throttle
         * bump + FLIP event). Kill-switch YZ_NO_SEMARM for the retirement A/B;
         * flip the default to OFF once measured redundant. */
        if (addr == RSX_REPORTS + 0x10 && arg != 0) {
            static int nsa = -1; if (nsa < 0) { nsa = getenv("YZ_NO_SEMARM") ? 1 : 0;
                fprintf(stderr, "[semarm] armed (release-arm heuristic %s)\n",
                        nsa ? "DISABLED by YZ_NO_SEMARM" : "on"); fflush(stderr); }
            if (!nsa) {
                LONG prev = InterlockedExchange(
                    &g_rsx_flip_pending[g_rsx_queued_head & 7u], 1);
                if (yz_ft_on())
                    yz_ft("ARM pending[%u] %ld->1", g_rsx_queued_head & 7u, prev);
            }
        }
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
        if (yz_ft_on() && addr >= RSX_REPORTS && addr < RSX_REPORTS + 0x1000u)
            yz_ft("BE-REL addr=0x%08X arg=0x%08X", addr, arg);
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
 * NOTE (2026-07-02): a "live-stopper guard" (refuse when stopper_ea == S[0x20])
 * was tried here and REFUTED 0/4 -- at GET-park + PUT-ahead the PUT position
 * already proves the guarded content is built, so releasing even the current
 * S[0x20] instance is correct (t1 releases it at its next flush anyway; the
 * early release only skips the wait). The match itself lives in
 * yz_gcm_stopper_release_entry below (returns the entry address so the
 * retirement sweep knows how far consumption has proven progress). */

/* ============================================================================
 * JOURNAL RETIREMENT SWEEP (2026-07-02) -- the faithful consumption contract.
 *
 * MEASURED (instrumented RPCS3 [jrnl-dma]/[jrnl-tags]): on
 * real HW the EDGE SPU task (gs_task) consumes the gcm journal and ZEROES each
 * entry's tag word; the producer polls chunk-head tags == 0 before reusing a
 * journal chunk, i.e. the tags are the game's GPU-PROGRESS LEDGER. Every
 * stand-in that zeroed tags AHEAD of actual FIFO consumption (eager, pending-
 * set, lag-by-one -- 0/8, 0/8, 0/4 boot loops) made the game believe work had
 * retired that our GET had not consumed, and it recycled ring segments under
 * GET (torn-content non-command wedges). The faithful rule that survives:
 * ZERO AN ENTRY ONLY WHEN GET HAS PROVABLY CONSUMED PAST IT. GET applying the
 * deferred release of entry A (it parked on A's stopper with PUT ahead) is
 * that proof for entries BEHIND the released region -- NOTE the caveat: the
 * journal orders a segment's patch entries BEFORE its entry-stopper release,
 * so "through A" retires patches for content GET is only ENTERING (a
 * suspected over-retirement; could not be cleanly validated 2026-07-02
 * because the watchdog instrumentation invalidated the loops). OPT-IN
 * (YZ_JRNL=1) until it can be measured against a clean baseline; the REAL
 * fix in flight is restoring the actual consumer (gs_task residency,
 * trace-diff). Data/sublist payloads (tags 4/8/9/D/10)
 * are EDGE content-generation, not flow -- they stay unapplied until the
 * real consumer era. ===================================================== */
static uint32_t g_jrnl_retire_cursor = 0;

static void yz_jrnl_retire_through(uint32_t entry_addr)
{
    static int on = -1;
    if (on < 0) on = getenv("YZ_JRNL") ? 1 : 0;
    if (!on || !g_yz_game_toc) return;
    const uint32_t S = vm_read32(g_yz_game_toc - 0x7410u);
    if (S < 0x10000u || S >= 0xE0000000u) return;
    const uint32_t base = vm_read32(S + 0x08u);
    const uint32_t aend = vm_read32(S + 0x0Cu);
    if (base < 0x10000u || aend <= base || aend - base > 0x1000000u) return;
    if (entry_addr < base || entry_addr >= aend) return;
    uint32_t cur = g_jrnl_retire_cursor;
    if (cur < base || cur >= aend) cur = base;
    /* linear walk with arena wrap; hard cap for safety */
    unsigned zeroed = 0, guardn = 0;
    while (guardn++ < 0x20000u) {
        if (vm_read32(cur) != 0u) { vm_write32(cur, 0u); zeroed++; }
        const int done = (cur == entry_addr);
        cur += 0x20u;
        if (cur >= aend) cur = base;
        if (done) break;
    }
    g_jrnl_retire_cursor = cur;
    { static unsigned total = 0, logged = 0; total += zeroed;
      if (logged < 12 && zeroed) { logged++;
          fprintf(stderr, "[jrnl] retired %u entries through 0x%08X (%u total)\n",
                  zeroed, entry_addr, total); } }
}

/* Locate the journal entry for a deferred release (same match as
 * yz_gcm_stopper_release_deferred but returns the ENTRY ADDRESS so the
 * retirement sweep knows how far GET's consumption has proven progress). */
static uint32_t yz_gcm_stopper_release_entry(uint32_t stopper_ea)
{
    if (!g_yz_game_toc) return 0;
    uint32_t S = vm_read32(g_yz_game_toc - 0x7410u);
    if (S < 0x10000u || S >= 0xE0000000u) return 0;
    uint32_t base = vm_read32(S + 0x08u);
    uint32_t head = vm_read32(S + 0x00u);
    if (base < 0x10000u || base >= 0xE0000000u) return 0;
    if (head < base || (head - base) > 0x1000000u) return 0;
    for (uint32_t e = base; e < head; e += 0x20u)
        if (vm_read32(e + 0x00u) == 0x7Fu && vm_read32(e + 0x04u) == stopper_ea)
            return e;
    return 0;
}

/* ============================================================================
 * ORDERED EDGE JOURNAL HLE (YZ_JRNL_HLE, opt-in; merged from the 2026-07-14
 * Mac export, docs/EDGE_JOURNAL_HLE.md + scratch/WINDOWS_HANDOFF_2026-07-14.md).
 *
 * A wedge takeover, not an eager second consumer: considered only while RSX
 * GET is parked on a committed jump-to-self, and only after the producer's
 * journal head has been stable for a debounce window. The pure helper
 * (yakuza/edge_journal_hle.cpp) validates the COMPLETE span from the takeover
 * cursor through the release matching the parked stopper before the first
 * guest write, applies patches before releases in journal order, retires each
 * tag only after its operation, and fails CLOSED on any undecoded tag (the
 * stopper stays locked; the full entry is logged for decoding).
 *
 * Two Windows-side adaptations of the Mac design, both measured-evidence:
 *  - release VALUE: the SPU consumer clears a stopper with a gcm jump-FORWARD
 *    word (top byte 0x20; rpcs3clone oracle census + the lever below), never
 *    zero -- so the release goes through yz_jrnl_hle_release, not write32(0).
 *  - takeover CURSOR: bootstrapped from the real consumer's own poll cursor
 *    (g_yz_jrnl_cur_ea, maintained on every GETLLAR poll), sampled at its
 *    episode MINIMUM because the stuck cursor oscillates across a 7-8 line
 *    window. Walking from arena base instead would re-apply entries the
 *    consumer already consumed without zeroing.
 *
 * Decoded tags: 0x10 memcpy{src@+4,size@+8,dst@+0xC}, 0x7F ordered release
 * {stopper EA@+4}. Tags 0x04/08/09/0A/0D/11 intentionally unsupported here
 * until decoded against the consumer's own apply code + the RPCS3 oracle. */
extern "C" volatile uint32_t g_yz_jrnl_cur_ea;   /* spu_channels.c: live consumer cursor */

enum class yz_jrnl_hle_park_result { waiting, applied };

static int yz_jrnl_hle_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        enabled = getenv("YZ_JRNL_HLE") ? 1 : 0;
        if (enabled) {
            fprintf(stderr,
                    "[jrnl-hle] ARMED: frozen-head ordered takeover; known tags=10,11,0A,7F; "
                    "legacy APPLY_REL/PARK_REL disabled\n");
            fflush(stderr);
        }
    }
    return enabled;
}

static uint32_t yz_jrnl_hle_read32(void*, uint32_t address)
{
    return vm_read32(address);
}

static void yz_jrnl_hle_write32(void*, uint32_t address, uint32_t value)
{
    vm_write32(address, value);
}

static bool yz_jrnl_hle_copy(void*, uint32_t destination, uint32_t source,
                             uint32_t size)
{
    if (!vm_base || source + size < source || destination + size < destination)
        return false;
    memmove(vm_base + destination, vm_base + source, size);
    return true;
}

static void yz_jrnl_hle_release(void*, uint32_t stopper_ea)
{
    /* Faithful release value (DONT_RECHASE #83 + the validated lever below):
     * a gcm jump-forward word targeting the io offset just past the stopper.
     * The io map is linear here (io = ea - 0x40400000, the same mapping the
     * segment classifier uses). A stopper outside the io window should not
     * exist; fall back to a NOP clear and say so once. */
    if (stopper_ea >= 0x40400000u && stopper_ea < 0x40C00000u) {
        const uint32_t io_off = stopper_ea - 0x40400000u;
        vm_write32(stopper_ea, 0x20000000u | ((io_off + 4u) & 0x1FFFFFFCu));
        return;
    }
    static int said = 0;
    if (!said) {
        said = 1;
        fprintf(stderr, "[jrnl-hle] release outside io window ea=0x%08X -> NOP clear\n",
                stopper_ea);
        fflush(stderr);
    }
    vm_write32(stopper_ea, 0u);
}

static yz_jrnl_hle_park_result yz_jrnl_hle_try(uint32_t stopper_ea)
{
    static uint32_t arena_base = 0;
    static uint32_t arena_end = 0;
    static uint32_t cursor = 0;          /* persisted past prior HLE applies */
    static uint32_t episode_min = 0;     /* min consumer-cursor EA this episode */
    static uint32_t last_head = 0;
    static uint32_t last_stopper = 0;
    static ULONGLONG stable_since = 0;
    static unsigned stable_ms = 16;
    static int configured = 0;
    static uint32_t last_problem_entry = 0;
    static uint32_t last_problem_tag = 0;

    if (!configured) {
        configured = 1;
        if (const char* value = getenv("YZ_JRNL_HLE_STABLE_MS")) {
            const unsigned parsed = (unsigned)atoi(value);
            stable_ms = parsed > 5000u ? 5000u : parsed;
        }
        fprintf(stderr, "[jrnl-hle] producer-head stability window=%ums\n", stable_ms);
        fflush(stderr);
    }

    if (!g_yz_game_toc) return yz_jrnl_hle_park_result::waiting;
    const uint32_t state = vm_read32(g_yz_game_toc - 0x7410u);
    if (state < 0x10000u || state >= 0xE0000000u)
        return yz_jrnl_hle_park_result::waiting;

    const uint32_t base = vm_read32(state + 0x08u);
    const uint32_t end = vm_read32(state + 0x0cu);
    const uint32_t head = vm_read32(state + 0x00u);
    if (base < 0x10000u || end <= base || end - base > 0x1000000u ||
        head < base || head >= end || ((end - base) & 0x1fu) != 0 ||
        ((head - base) & 0x1fu) != 0) {
        static uint32_t said_state = 0;
        if (said_state != state) {
            said_state = state;
            fprintf(stderr,
                    "[jrnl-hle] invalid arena state S=0x%08X base=0x%08X end=0x%08X head=0x%08X\n",
                    state, base, end, head);
            fflush(stderr);
        }
        return yz_jrnl_hle_park_result::waiting;
    }

    if (base != arena_base || end != arena_end) {
        arena_base = base;
        arena_end = end;
        cursor = 0;
        episode_min = 0;
        last_head = 0;
        stable_since = 0;
    }

    /* Sample the live consumer cursor toward its episode minimum (the stuck
     * window's start line). Maintained on every consumer GETLLAR poll, so
     * repeated stepper calls during the stability window see the whole
     * oscillation range. */
    {
        const uint32_t live = g_yz_jrnl_cur_ea;
        if (live >= base && live < end && ((live - base) & 0x1fu) == 0 &&
            (episode_min == 0 || live < episode_min))
            episode_min = live;
    }

    const ULONGLONG now = GetTickCount64();
    if (head != last_head || stopper_ea != last_stopper) {
        last_head = head;
        last_stopper = stopper_ea;
        stable_since = now;
        episode_min = 0;   /* new episode: resample the consumer window */
        return yz_jrnl_hle_park_result::waiting;
    }
    if (now - stable_since < stable_ms)
        return yz_jrnl_hle_park_result::waiting;

    /* The consumer's own stuck position is ground truth for "everything
     * before this is consumed": prefer it over the persisted cursor and the
     * arena base. Entries the HLE already applied rescan as harmless tag-0. */
    const uint32_t start = episode_min ? episode_min : (cursor ? cursor : base);
    {
        static uint32_t said_start = 0;
        if (said_start != start) {
            said_start = start;
            fprintf(stderr,
                    "[jrnl-hle] takeover cursor=0x%08X (%s) head=0x%08X stopper=0x%08X\n",
                    start,
                    episode_min ? "consumer-window-min" : (cursor ? "persisted" : "arena-base"),
                    head, stopper_ea);
            fflush(stderr);
        }
    }

    const yz::edge_journal::Io io{nullptr, yz_jrnl_hle_read32, yz_jrnl_hle_write32,
                                  yz_jrnl_hle_copy, yz_jrnl_hle_release};
    spu_lockline_lock();
    const yz::edge_journal::Result result = yz::edge_journal::apply_through_release(
        io, base, end, start, head, stopper_ea);
    spu_lockline_unlock();

    if (result.status == yz::edge_journal::Status::applied) {
        cursor = result.next_cursor;
        last_problem_entry = 0;
        last_problem_tag = 0;
        fprintf(stderr,
                "[jrnl-hle] applied %u ordered entries through release=0x%08X; cursor=0x%08X head=0x%08X\n",
                result.applied_entries, stopper_ea, cursor, head);
        fflush(stderr);
        return yz_jrnl_hle_park_result::applied;
    }

    if (result.status == yz::edge_journal::Status::changed_during_validation) {
        /* Producer or consumer activity was observed despite a stable head.
         * Restart the stability proof instead of racing it. */
        stable_since = now;
        return yz_jrnl_hle_park_result::waiting;
    }

    if (result.problem_entry != last_problem_entry ||
        result.problem_tag != last_problem_tag) {
        last_problem_entry = result.problem_entry;
        last_problem_tag = result.problem_tag;
        fprintf(stderr,
                "[jrnl-hle] BLOCKED status=%s entry=0x%08X tag=0x%08X words="
                "%08X %08X %08X %08X %08X %08X %08X %08X\n",
                yz::edge_journal::status_name(result.status),
                result.problem_entry, result.problem_tag,
                result.problem_words[0], result.problem_words[1],
                result.problem_words[2], result.problem_words[3],
                result.problem_words[4], result.problem_words[5],
                result.problem_words[6], result.problem_words[7]);
        fflush(stderr);
    }
    return yz_jrnl_hle_park_result::waiting;
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

/* DEFER-DECISION TRACE (env YZ_TRACE_DEFER, 2026-06-24). Pin WHY the game defers a
 * stopper-release (the LAYER-1 deadlock root) vs releases it immediately. From the
 * static decode of the inline gcm flush func_00E9BC9C / func_00E9BE4C:
 *   S = *(game_toc-0x7410)   = gcm-state struct
 *   C = *(game_toc-0x7414);  P = *C;  ctx_end = P[+0x4]   (current segment end)
 *   S[+0x1C] = defer latch    S[+0x20] = pending stopper cursor
 *   S[+0x24] = segment-end recorded WHEN the stopper was placed
 *   S[+0x00] = op-list write head   S[+0x08] = op-list base   (0x20-byte entries;
 *              entry[+0]=tag (0x7F=deferred release), entry[+4]=stopper EA)
 * DECISION (func_00E9BE4C:E9BE58): release is IMMEDIATE iff S[0x24]==ctx_end, else
 * DEFERRED (cross-segment: the producer crossed a 1 MB boundary between place+release).
 * This monitor is READ-ONLY (no clamp, unlike YZ_IMM_REL) -- it just records the
 * decision state so we can see (a) how often defer fires + the S[0x24]/ctx_end values,
 * (b) whether the op-list backlog of tag-0x7F entries EVER drains (count drops) or only
 * grows, and (c) ctx_end segment advances. Logs every change + a 1s heartbeat. */
static DWORD WINAPI yz_defer_trace_mon(LPVOID)
{
    fprintf(stderr, "[defer] trace monitor up (READ-ONLY): watching S[0x1C] latch, "
                    "op-list backlog, S[0x24] vs ctx_end\n");
    uint32_t last_head = ~0u, last_latch = ~0u, last_end = ~0u, last_pend = ~0u;
    DWORD last_hb = 0;
    for (;;) {
        if (g_yz_game_toc) {
            uint32_t S = vm_read32(g_yz_game_toc - 0x7410u);
            uint32_t C = vm_read32(g_yz_game_toc - 0x7414u);
            if (S >= 0x10000u && S < 0xE0000000u) {
                uint32_t latch = vm_read32(S + 0x1Cu);
                uint32_t pend  = vm_read32(S + 0x20u);
                uint32_t saved = vm_read32(S + 0x24u);
                uint32_t base  = vm_read32(S + 0x08u);
                uint32_t head  = vm_read32(S + 0x00u);
                uint32_t ctx_end = 0;
                if (C >= 0x10000u && C < 0xE0000000u) {
                    uint32_t P = vm_read32(C + 0x0u);
                    if (P >= 0x10000u && P < 0xE0000000u) ctx_end = vm_read32(P + 0x4u);
                }
                /* count un-applied tag-0x7F deferred releases in [base,head) */
                uint32_t pend7f = 0, nent = 0;
                if (base >= 0x10000u && base < 0xE0000000u && head >= base && (head - base) <= 0x8000u) {
                    nent = (head - base) / 0x20u;
                    for (uint32_t e = base; e < head; e += 0x20u)
                        if (vm_read32(e + 0x0u) == 0x7Fu) pend7f++;
                }
                uint32_t get = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_GET);
                uint32_t put = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT);
                DWORD now = GetTickCount();

                if (head != last_head) {
                    int delta = (last_head==~0u) ? 0 : (int)((int32_t)head - (int32_t)last_head)/0x20;
                    const char* dir = (last_head!=~0u && head < last_head) ? "  <<< DRAIN (head shrank)" : "";
                    fprintf(stderr, "[defer] op-list head 0x%08X->0x%08X (%+d entries, %u total, %u pending-0x7F)%s\n",
                            last_head==~0u?head:last_head, head, delta, nent, pend7f, dir);
                    last_head = head;
                }
                if (pend7f != last_pend) {
                    if (last_pend != ~0u && pend7f < last_pend)
                        fprintf(stderr, "[defer] *** BACKLOG DRAINED: pending-0x7F %u -> %u (something applied a release)\n", last_pend, pend7f);
                    last_pend = pend7f;
                }
                if (latch != last_latch) {
                    fprintf(stderr, "[defer] latch S[0x1C] 0x%08X->0x%08X  (%s)  pend=0x%08X saved_segend=0x%08X ctx_end=0x%08X\n",
                            last_latch==~0u?latch:last_latch, latch,
                            latch?"DEFER mode armed":"immediate mode", pend, saved, ctx_end);
                    last_latch = latch;
                }
                if (ctx_end != last_end && ctx_end) {
                    fprintf(stderr, "[defer] ctx_end (segment) 0x%08X->0x%08X  saved_segend=0x%08X  -> next release: %s\n",
                            last_end==~0u?ctx_end:last_end, ctx_end, saved,
                            (saved==ctx_end)?"IMMEDIATE":"DEFER (cross-segment)");
                    last_end = ctx_end;
                }
                if (now - last_hb >= 1000u) { last_hb = now;
                    fprintf(stderr, "[defer hb] latch=0x%X pend=0x%08X saved_segend=0x%08X ctx_end=0x%08X | op-list ent=%u pend7f=%u | GET=0x%06X PUT=0x%06X\n",
                            latch, pend, saved, ctx_end, nent, pend7f, get & 0xFFFFFFu, put & 0xFFFFFFu);
                    fflush(stderr);
                }
            }
        }
        Sleep(0);
    }
}

/* PHASE TIMELINE (env YZ_PHASE, 2026-06-24): high-detail producer+consumer timeline to
 * catch the working->broken transition. Frames 1-2 work, frame 3 (first ring wrap)
 * deadlocks; this logs EVERY change to GET/PUT (consumer/commit heads) + the producer's
 * bufdesc cursor (cur/begin/end) + flip count, timestamped, so we can see the exact step
 * where "producer ahead of consumer" flips to "phase-locked one segment apart". Pairs with
 * the reserve-call log (dispatch.cpp, each func_02103AAC) and YZ_FIFO_TRACE (consumer steps). */
static DWORD WINAPI yz_phase_monitor(LPVOID)
{
    fprintf(stderr, "[phase] timeline monitor up (logs every GET/PUT/cursor/flip change)\n");
    const uint32_t pbd = 0x02114000u - 0x7FD8u;
    uint32_t lg=~0u, lp=~0u, lc=~0u, lb=~0u, le=~0u, lf=~0u;
    DWORD t0 = GetTickCount();
    for (;;) {
        uint32_t bd  = vm_read32(pbd);
        uint32_t get = vm_read32(0x10000044u) & ~3u;
        uint32_t put = vm_read32(0x10000040u) & ~3u;
        uint32_t cur=0, beg=0, end=0;
        if (bd >= 0x10000u && bd < 0xE0000000u) {
            cur = vm_read32(bd + 0x8); beg = vm_read32(bd + 0x0); end = vm_read32(bd + 0x4);
        }
        uint32_t fl = vm_read32(0x40C00000u);
        if (get!=lg || put!=lp || cur!=lc || beg!=lb || end!=le || fl!=lf) {
            /* seg# of GET vs cur (producer write head) -> the phase gap */
            int gseg = (int)((get & 0x7FFFFF) >> 20);
            int cseg = (cur >= 0x40400000u && cur < 0x40C00000u) ? (int)((cur - 0x40400000u) >> 20) : -1;
            fprintf(stderr, "[phase] t=%5lu GET=%06X(s%d) PUT=%06X | cur=%08X(s%d) beg=%08X end=%08X | flips=%u | gap=%d\n",
                    GetTickCount()-t0, get, gseg, put, cur, cseg, beg, end, fl,
                    (cseg>=0)? cseg-gseg : 99);
            fflush(stderr);
            lg=get; lp=put; lc=cur; lb=beg; le=end; lf=fl;
        }
        Sleep(0);
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

/* ===========================================================================
 * Faithful RSX FIFO consumer (clean-room reimplementation of RPCS3's
 * FIFO_control + rsx::thread::run_FIFO, Emu/RSX/RSXFIFO.cpp). Replaces ~10
 * sessions of band-aids. Sony's libgcm owns the ring; the guest (t1) produces
 * commands and advances PUT; this thread is the RSX side that consumes the
 * committed [GET, PUT) and advances GET.
 * ===========================================================================*/

/* GET serialization (RPCS3 sys_rsx_mtx analogue). The two writers of the
 * DMA-control GET register -- this consumer loop and the guest sys_rsx pkg001
 * (FIFO set get/put) path -- serialize here so a pkg001 set never tears a GET
 * the consumer is advancing, and the consumer never overwrites a pkg001 set
 * with a stale-derived GET. Race-safe lazy init via InitOnce. */
static CRITICAL_SECTION g_rsx_fifo_lock;
static INIT_ONCE        g_rsx_fifo_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK yz_rsx_fifo_init_cb(PINIT_ONCE, PVOID, PVOID*)
{
    InitializeCriticalSection(&g_rsx_fifo_lock);
    return TRUE;
}
static void yz_rsx_fifo_lock_ensure(void)
{
    InitOnceExecuteOnce(&g_rsx_fifo_once, yz_rsx_fifo_init_cb, NULL, NULL);
}

/* Exported for the flow-control band-aid (main.cpp yz_flip_advance). Audit
 * 2026-07-01: the band-aid wrote GET and the fence with NO lock, racing this
 * consumer's read-decide-write window -- GET could be clobbered BACKWARD
 * (producer-stall hazard) and the fence could double-advance vs the faithful
 * vblank path. All GET/fence writers must hold g_rsx_fifo_lock. */
extern "C" void yz_rsx_fifo_acquire(void)
{
    yz_rsx_fifo_lock_ensure();
    EnterCriticalSection(&g_rsx_fifo_lock);
}
extern "C" void yz_rsx_fifo_release(void)
{
    LeaveCriticalSection(&g_rsx_fifo_lock);
}
extern "C" int yz_rsx_flip_pending_any(void)
{
    for (int h = 0; h < 8; ++h)
        if (g_rsx_flip_pending[h]) return 1;
    return 0;
}

/* s37 fix (scratch/s37_render_ordering.md, ledger #82 candidate): RPCS3
 * retires a flip -- present + flip-done bit + label clear + the throttle
 * fence bump + FLIP event -- ON THE RSX/FIFO-CONSUMER THREAD, in FIFO order,
 * when GET reaches the flip (ORACLE RSXThread.cpp:3383 handle_emu_flip,
 * sys_rsx.cpp:880-892 0xFEC clears label+0x10). The vblank handler explicitly
 * REFUSES to run on a ppu/timer thread (ORACLE sys_rsx.cpp:896-900, "wrong
 * thread"). Our default retires on the free-running 60 Hz vblank timer
 * instead (yz_rsx_vblank_tick below), decoupling the render throttle's fence
 * from actual consumer progress -- t1 can arm a flip and outrun the consumer
 * because the fence bumps on wall-clock, not on drain.
 *
 * YZ_FLIP_ON_CONSUMER (default OFF) moves the retire into yz_rsx_fifo_step
 * (below), gated on g_rsx_fifo_lock, so it fires exactly once per arm right
 * after GET has drained past the flip -- restoring the RPCS3 lockstep. When
 * ON, yz_rsx_vblank_tick's per-head retire is skipped entirely (it does only
 * vBlankCount + the VBLANK event, mirroring RPCS3's 0xFED). Default OFF is a
 * one-token change at each call site: zero behavior change from the
 * pre-existing vblank-thread retire. */
static int yz_flip_on_consumer(void)
{
    static int on = -1;
    if (on < 0) { on = getenv("YZ_FLIP_ON_CONSUMER") ? 1 : 0;
        fprintf(stderr, "[flip-consumer] armed: flip completion runs on %s (YZ_FLIP_ON_CONSUMER=%s)\n",
                on ? "the FIFO consumer thread" : "the vblank timer (default, unchanged)",
                on ? "1" : "0");
        fflush(stderr);
    }
    return on;
}

/* Faithful rules (NO heuristics, NO deferred-release, NO GET-forcing):
 *   - GET re-read every iteration; PUT bounds us to [GET, PUT). get == put =>
 *     drained: yield and re-poll. GET NEVER reaches or passes PUT.
 *   - old jump (cmd & 0xE0000003 == 0x20000000) / new jump (cmd & 3 == 1):
 *     follow. A jump-to-self (target == get) is the producer's stopper: spin in
 *     place (memwatch) until t1 patches the word; never force past it.
 *   - call (cmd & 3 == 2): one-level return stack; jump to cmd & 0x1FFFFFFC.
 *   - return (cmd & 0xFFFF0003 == 0x00020000): pop the return.
 *   - method packet: count=(cmd>>18)&0x7FF, method=cmd&0x3FFFC,
 *     noninc=cmd&0x40000000. Require the whole packet committed (PUT covers
 *     header+args) before dispatching (RPCS3 inc_get waits for PUT per arg).
 *     Dispatch each arg via yz_rsx_method; an unsatisfied semaphore ACQUIRE
 *     stalls (leave GET on the packet, retry).
 *   - off-ring GET / unmapped jump target / malformed word: recover (resync
 *     GET=PUT, log once, clear the return stack) -- RPCS3 recover_fifo. */
/* One-level return stack, shared between the async consumer thread and any
 * inline pump (only ONE drains at a time -- YZ_RSX_INLINE disables the async
 * thread). Guarded by g_rsx_fifo_lock. */
static uint32_t g_fifo_ret = ~0u;

/* s29 (scratch/s29_terminal_park_re.md, Q4): RPCS3 (RSXFIFO.cpp) treats BOTH a
 * nested CALL (a second CALL before the pending one RETURNs) and a RETURN with
 * no pending CALL as FIFO_ERROR and calls recover_fifo() -- checkpoint/retry,
 * escalating to a fatal abort after 20 recoveries inside a 2 s window. Our port
 * used to (a) silently clobber the one-level g_fifo_ret slot on a nested CALL
 * with zero diagnostic, and (b) idle forever, completely silently after one
 * one-ever warning, on a RETURN-without-CALL (the s28m10/s28m4 terminal park at
 * GET=0x011001EC / 0x0000098C). The loud detection log below fires
 * UNCONDITIONALLY (zero behavior change, always-on diagnostic per the report's
 * "smallest diagnostic first" recommendation). The checkpoint/retry+escalation
 * recovery itself is a behavior change and stays behind YZ_FIFO_RECOVER_RET
 * (default OFF -- unvalidated against a live boot; kill-switch semantics
 * inverted from YZ_NO_FIFO_RECOVER's sibling non-command path because this one
 * hasn't been through an A/B boot yet). Our step function has no valid
 * "rewind" target for either case (GET never advanced into the bad word), so
 * "restore to checkpoint" is a no-op position-wise -- the recovery's value is
 * the bounded, loud retry/escalation cadence instead of a truly-silent
 * infinite idle. Retirement: fold into the default path once a live boot
 * confirms neither state corrupts anything worse than idling. */
static int g_fifo_recover_ret_fatal = 0;
static ULONGLONG g_fifo_recover_ret_last_ms = 0;
static ULONGLONG g_fifo_recover_ret_window0 = 0;
static int g_fifo_recover_ret_n = 0;

static int yz_fifo_recover_ret_enabled(void)
{
    static int rr = -1;
    if (rr < 0) { rr = getenv("YZ_FIFO_RECOVER_RET") ? 1 : 0;
        if (rr) fprintf(stderr, "[fifo-rec-ret] ARMED (YZ_FIFO_RECOVER_RET): RETURN-without-CALL "
                "and CALL-inside-subroutine get the RPCS3 recover_fifo checkpoint-retry analog "
                "(20 strikes / 2s -> fatal) instead of silent-forever-idle / silent-clobber\n"); }
    return rr;
}

/* s33 [fifo-flow] (env YZ_FIFO_FLOWLOG): log every SUCCESSFUL flow-control
 * transfer. The s33 audit's discriminator: JUMP/CALL were silent on success,
 * so a stranded GET's arrival path (jump vs call vs walked) was never in the
 * log — s32resur1's teleport from io 0x8007C to 0x200BF4 had zero trace. */
static int yz_fifo_flowlog(void)
{
    static int fl = -1;
    if (fl < 0) { fl = getenv("YZ_FIFO_FLOWLOG") ? 1 : 0;
        if (fl) { fprintf(stderr, "[fifo-flow] ARMED (jump/call/return transition log)\n"); fflush(stderr); } }
    return fl;
}

/* One recovery attempt for the RETURN-without-CALL / CALL-inside-subroutine
 * states. Rate-limited to ~1 attempt/50ms (our poll loop has no RPCS3-style
 * blocking 2ms sleep between retries, so this substitutes the existing
 * SwitchToThread poll cadence -- the strike count and 2s window are faithful
 * to the RPCS3 shape). After 20 strikes inside a rolling 2s window, latches
 * fatal permanently: one loud print, then this and all further calls are a
 * cheap no-op so the FIFO consumer's existing idle/heartbeat machinery takes
 * over (matches "kills RSX" in spirit without tearing down the process --
 * the consumer parks, loudly, instead of RPCS3's hard exception). */
static void yz_fifo_ret_recover(uint32_t get, const char* what)
{
    if (g_fifo_recover_ret_fatal) return;
    const ULONGLONG now = GetTickCount64();
    if (g_fifo_recover_ret_last_ms && now - g_fifo_recover_ret_last_ms < 50) return;
    g_fifo_recover_ret_last_ms = now;
    if (!g_fifo_recover_ret_window0 || now - g_fifo_recover_ret_window0 > 2000) {
        g_fifo_recover_ret_window0 = now; g_fifo_recover_ret_n = 0;
    }
    g_fifo_recover_ret_n++;
    if (g_fifo_recover_ret_n >= 20) {
        g_fifo_recover_ret_fatal = 1;
        fprintf(stderr, "[fifo-rec-ret] FATAL: %s struck %d recoveries within 2s at io=0x%08X -- "
                "giving up (RPCS3 recover_fifo analog); FIFO consumer parks permanently\n",
                what, g_fifo_recover_ret_n, get);
        fflush(stderr);
        return;
    }
    fprintf(stderr, "[fifo-rec-ret] n=%d %s at io=0x%08X -- retry (RPCS3 recover_fifo analog)\n",
            g_fifo_recover_ret_n, what, get);
    fflush(stderr);
}

/* t1 hop counter from dispatch.cpp — the park-rel fast path's wedge witness:
 * a t1 that makes no hops while the consumer sits parked cannot be on its way
 * to drain the release journal (the drain runs on t1). */
extern "C" volatile long g_yz_t1_sample_seq;
extern "C" volatile void* g_yz_t1_last_tf;   /* s25 spin-witness feed (dispatch.cpp) */
extern "C" volatile uint32_t g_yz_jrnl_cur_ea;  /* s34 live journal-consumer cursor EA (spu_channels.c / spu_dma.h) */

/* Process exactly ONE FIFO command at GET (self-locked). Returns 1 if it made
 * progress (GET advanced / a method dispatched), 0 if idle or stalled (drained,
 * parked on a stopper, segment not finalised, off-ring, or an unsatisfied
 * semaphore). The RPCS3 run_FIFO step, factored out of the old consumer loop so
 * it can run EITHER on the free-running async thread OR inline on the producer
 * thread (YZ_RSX_INLINE: drain coupled to the producer's PUT flush so it can't lap). */
static int yz_rsx_fifo_step(void)
{
    EnterCriticalSection(&g_rsx_fifo_lock);
    uint32_t       get = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_GET) & ~3u;
    const uint32_t put = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT) & ~3u;

    /* s33 [fifo-hb] (env YZ_FIFO_HB): uncapped 5 s GET/PUT heartbeat. Every
     * deep-boot terminal FIFO state so far was invisible because the apply/
     * park prints are count-capped (ledger #65 class); this always answers
     * "where is the FIFO right now". Placed BEFORE the empty check so a
     * drained ring (get==put, producer stopped) is a visible state too. */
    { static int hb = -1; static ULONGLONG hb_t = 0;
      if (hb < 0) { hb = getenv("YZ_FIFO_HB") ? 1 : 0;
          if (hb) { fprintf(stderr, "[fifo-hb] ARMED (5s GET/PUT heartbeat)\n"); fflush(stderr); } }
      if (hb) { const ULONGLONG now = GetTickCount64();
          if (now - hb_t >= 5000) { hb_t = now;
              const uint32_t hea = yz_rsx_io_to_ea(get);
              /* s38 reframe discriminator: log the gcm-journal PRODUCER HEAD
               * (S+0x00) + BASE (S+0x08), S=vm[game_toc-0x7410], alongside the
               * SPU consumer cursor (g_yz_jrnl_cur_ea). If the head keeps
               * ADVANCING while the cursor stays stuck in its small window =>
               * the consumer's window LIMIT never refreshes to the head (the
               * fix locus). If the head is FROZEN during the wedge => the
               * PRODUCER (t1) stalled and the consumer is exonerated (root
               * upstream). Distance head-cursor = how far behind the consumer is. */
              uint32_t jS = g_yz_game_toc ? vm_read32(g_yz_game_toc - 0x7410u) : 0u;
              uint32_t jhead = (jS >= 0x10000u && jS < 0xE0000000u) ? vm_read32(jS + 0x00u) : 0u;
              uint32_t jbase = (jS >= 0x10000u && jS < 0xE0000000u) ? vm_read32(jS + 0x08u) : 0u;
              uint32_t jcur  = g_yz_jrnl_cur_ea;
              fprintf(stderr, "[fifo-hb] get=0x%08X put=0x%08X word=0x%08X ret=0x%08X | jhead=0x%08X jcur=0x%08X behind=0x%X jbase=0x%08X\n",
                      get, put, hea ? vm_read32(hea) : 0xDEADDEADu, g_fifo_ret,
                      jhead, jcur, (jhead > jcur) ? (jhead - jcur) : 0u, jbase);
              fflush(stderr); } } }

    /* FIFO_EMPTY: ring drained. Never reach/pass PUT. (RPCS3 read(): put==get) */
    if (get == put) { LeaveCriticalSection(&g_rsx_fifo_lock); return 0; }

    const uint32_t ea = yz_rsx_io_to_ea(get);
    if (!ea) {
        const uint32_t pea = yz_rsx_io_to_ea(put);
        if (put != get && pea) {
            static int n = 0; if (n < 12) { n++;
                fprintf(stderr, "[rsx] GET=0x%08X off-ring -> resync to PUT 0x%08X (recover)\n", get, put); }
            vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, put);
            g_fifo_ret = ~0u;
            LeaveCriticalSection(&g_rsx_fifo_lock);
            return 1;
        }
        static int warned = 0;
        if (!warned) { warned = 1;
            fprintf(stderr, "[rsx] GET=0x%08X (PUT=0x%08X) not io-mapped; idling\n", get, put); }
        LeaveCriticalSection(&g_rsx_fifo_lock);
        return 0;
    }

    const uint32_t cmd = vm_read32(ea);

    /* ---- control transfer ---- */
    if ((cmd & 0xE0000003u) == 0x20000000u || (cmd & 3u) == 1u) {   /* old | new jump */
        const uint32_t tgt = (cmd & 3u) == 1u ? (cmd & 0xFFFFFFFCu)   /* NEW offset mask */
                                              : (cmd & 0x1FFFFFFCu);  /* OLD offset mask */
        if (tgt == get) {
            /* Jump-to-self stopper. DEFERRED-RELEASE APPLY -- RETIRED (default
             * OFF 2026-07-02, layer-1 root-cause session; opt back in with
             * YZ_APPLY_REL=1 for A/B). This stand-in was built (2026-06-28)
             * when nothing released journaled stoppers because the REAL
             * consumer couldn't run. Post il/SPU_RET/backoff fixes, gs_task
             * (EDGE) demonstrably does the whole job itself: applies the
             * tag-0x04/08/09/10 patches (plain PUTs, LS pc 0xB60C) THEN
             * releases the stopper with a FENCED 4-byte PUT (pc 0x5F00) --
             * measured via the [gs-put] probe; with this path off, GET never
             * once met an unpatched stopper across 12 boots. Leaving it ON
             * races Sony's consumer: it releases WITHOUT the preceding
             * patches, handing GET unpatched content -- 3/3 applier-on boots
             * wedged t1 at ~+6 s at an identical site; 0/12 with it off.
             * Evidence: scratch/{bad1,cfgA*,cfgB*,val*}.err. The default is
             * now a faithful memwatch: spin at the stopper until the real
             * consumer patches it. Delete after quiet sessions. */
            const int journal_hle = yz_jrnl_hle_enabled();
            static int apply = -1;
            if (apply < 0) apply = (!journal_hle && getenv("YZ_APPLY_REL")) ? 1 : 0;
            /* YZ_PARK_REL (s21, the movie-phase deadlock triangle -- full map in
             * scratch/stopper_drain_re.md): at the logo->movie boundary a commit
             * crosses a segment recycle, so the game DEFERS this stopper's
             * release into its tag-0x7F op-list; the drain that would execute it
             * runs only after t1's flip throttle passes -- but the throttled
             * flips sit BEHIND this stopper. Permanent triangle. gs_task is
             * measured idle here (no geometry -> no journal work), so the June
             * race partner (double-apply during the geometry stream, the reason
             * YZ_APPLY_REL was retired) cannot exist. This narrow variant
             * applies the game's OWN journaled release ONLY after parking on
             * the SAME stopper for 3 s with PUT ahead -- a state that is
             * otherwise a permanent deadlock. Opt-in for A/B validation. */
            static int prel = -1; static unsigned fast_ms = 250;
            if (prel < 0) { prel = (!journal_hle && getenv("YZ_PARK_REL")) ? 1 : 0;
                const char* fm = getenv("YZ_PARKREL_FAST_MS");
                if (fm) fast_ms = (unsigned)atoi(fm);   /* 0 = fast path off (3 s tier only) */
                if (prel) fprintf(stderr, "[park-rel] ARMED (YZ_PARK_REL): deadlock-only deferred-release apply, fast=%ums+t1-frozen witness, fallback=3000ms\n", fast_ms); }
            /* s24 FAST PATH: the 3 s tier alone cost 16x3 s = ~48 s/boot at the
             * movie boundary (scratch/s24pr1.err). Fire early ONLY when the
             * deadlock is witnessed, not merely suspected: (a) parked on this
             * stopper > fast_ms, (b) t1 made ZERO hops since the park began
             * (the journal drain runs on t1 — a frozen t1 cannot be coming),
             * (c) the release is in the game's journal (checked below — we
             * only ever deliver the game's own queued write). The 3 s tier
             * stays as the unconditional fallback for shapes the witness
             * misses (e.g. t1 busy in a long direct-call stretch). */
            static uint32_t park_ea = 0; static ULONGLONG park_t0 = 0;
            static long park_seq = 0; static void* park_tf = 0;
            if (ea != park_ea) { park_ea = ea; park_t0 = GetTickCount64();
                                 park_seq = g_yz_t1_sample_seq;
                                 park_tf  = (void*)g_yz_t1_last_tf; }
            const ULONGLONG parked_ms = GetTickCount64() - park_t0;
            /* s40b v2 (the adversarial review's targeting requirement,
             * scratch/s40b_refute_unstick.md): publish the stopper EA the GPU is
             * provably parked on, so the SPU-side YZ_QROT_UNSTICK can fire ONLY
             * for the item carrying THIS release (not a blind lottery). >2s parked
             * = published; resets to 0 on any new park until it matures. */
            g_yz_parked_pub_ea = (parked_ms > 2000) ? ea : 0;
            /* s33 0x4C24 discriminator (STATUS ⚡ #1, always-on, low-volume):
             * at any >=5 s stopper park, LEVER ON OR OFF, say whether the
             * game's tag-0x7F journal holds this stopper's release entry.
             * PRESENT -> the consumer has a consume-gap (it applied dozens of
             * others, s33retA/B); ABSENT -> the producer never journaled it
             * (t1 wedged pre-append). Resamples every 10 s while parked. */
            { static uint32_t sj_ea = 0; static ULONGLONG sj_t = 0;
              if (parked_ms > 5000 && (sj_ea != ea || GetTickCount64() - sj_t > 10000)) {
                  sj_ea = ea; sj_t = GetTickCount64();
                  const uint32_t je = yz_gcm_stopper_release_entry(ea);
                  fprintf(stderr, "[stop-jrnl] parked %llums @io 0x%06X ea=0x%08X journal-entry=%s (0x%08X)\n",
                          (unsigned long long)parked_ms, get, ea, je ? "PRESENT" : "ABSENT", je);
                  fflush(stderr);
                  /* s34 CONSUME-GAP HEXDUMP — fires ONCE at the 2nd park
                   * sample (>15s), anchored on the STABLE release entry (NOT
                   * the cursor). The consumer cursor has two measured steady
                   * shapes: LOCKED at one value past the entry (s34gapA:
                   * 0x41F1E300) or CYCLING a ~6-line window that CONTAINS the
                   * entry (s34frzA: sweeps [0x41F2EF80..0x41F2F280] forever,
                   * entry 0x41F2F200 inside it). A cursor-stable gate misses
                   * the cycling flavor entirely, so anchor on the entry and
                   * dump WIDE (6 lines back + entry line + 3 forward) to
                   * capture the whole active window in both. cur is printed so
                   * we see where in the cycle it was. Adds the live RING word
                   * (still jump-to-self?) + a [base,head) scan for EVERY
                   * tag-0x7F entry naming this stopper. Entry stride 0x20:
                   * word0=tag (0x7F release / 04,05,09,0D,10 patch-data /
                   * 0=hole), word1=target EA; consumer accepts 128B lines. */
                  static int sj_dumped = 0;
                  if (je && !sj_dumped && parked_ms > 15000) {
                      sj_dumped = 1;
                      const uint32_t S2    = vm_read32(g_yz_game_toc - 0x7410u);
                      const int Sok        = (S2 >= 0x10000u && S2 < 0xE0000000u);
                      const uint32_t jbase = Sok ? vm_read32(S2 + 0x08u) : 0;
                      const uint32_t jhead = Sok ? vm_read32(S2 + 0x00u) : 0;
                      const uint32_t cur   = g_yz_jrnl_cur_ea;
                      const uint32_t ringword = vm_read32(ea);
                      uint32_t lo = (je & ~127u) - 0x300u;          /* 6 lines back */
                      uint32_t hi = (je & ~127u) + 0x180u;          /* entry line + 3 forward */
                      if (jbase && lo < jbase) lo = jbase;
                      if (hi - lo > 0x800u) hi = lo + 0x800u;
                      fprintf(stderr, "[stop-jrnl] HEXDUMP [0x%08X..0x%08X) cursor=0x%08X entry=0x%08X base=0x%08X head=0x%08X ringword@0x%08X=0x%08X\n",
                              lo, hi, cur, je, jbase, jhead, ea, ringword);
                      for (uint32_t a = lo; a < hi; a += 0x20u) {
                          const char* mark = "    ";
                          if (jhead && a >= jhead) mark = "UNW>";
                          if (cur && (a & ~127u) == (cur & ~127u)) mark = "CUR>";
                          if (a == je) mark = "ENT>";
                          fprintf(stderr, "%s 0x%08X (%+5d): %08X %08X %08X %08X %08X %08X %08X %08X\n",
                                  mark, a, (int)(a - cur),
                                  vm_read32(a+0x00u), vm_read32(a+0x04u), vm_read32(a+0x08u), vm_read32(a+0x0Cu),
                                  vm_read32(a+0x10u), vm_read32(a+0x14u), vm_read32(a+0x18u), vm_read32(a+0x1Cu));
                      }
                      if (jbase && jhead > jbase && (jhead - jbase) < 0x1000000u) {
                          int n7f = 0;
                          fprintf(stderr, "[stop-jrnl] SCAN tag-0x7F entries with word1==0x%08X in [0x%08X..0x%08X):\n", ea, jbase, jhead);
                          for (uint32_t e = jbase; e < jhead; e += 0x20u) {
                              if (vm_read32(e) == 0x7Fu && vm_read32(e + 0x04u) == ea) {
                                  n7f++;
                                  if (n7f <= 8)
                                      fprintf(stderr, "    #%d @0x%08X (%s cursor by %d)\n",
                                              n7f, e, e < cur ? "behind" : "ahead of", (int)(e - cur));
                              }
                          }
                          fprintf(stderr, "[stop-jrnl] SCAN total=%d\n", n7f);
                      }
                      fflush(stderr);
                  }
                  } }
            const int parked3s   = prel && (parked_ms > 3000);
            /* s25: fast tier now UNCONDITIONAL at fast_ms (witness dropped).
             * Measured chain: (a) rides s25ride4-6 ground at 3-5 s/flip
             * because t1 SPINS in the throttle (seq climbs, hop-frozen
             * witness never fires) yet makes no flush call
             * (stopper_drain_re.md Q1); (b) the loading-screen steady state
             * re-parks EVERY frame at the segment-recycle stopper (io
             * 0x200000, 0x258-byte frame batches) because a LATCHED release
             * is only ever applied by the SPU journal consumer (which we
             * lack) or this lever — so fast_ms is the frame-rate governor
             * (fps <= 1000/fast_ms), and RPCS3's consumer does this at
             * sub-ms. Safety was never the witness: the preconditions (GET
             * parked ON the stopper + the release IS the game's own journal
             * entry + PUT committed past it) carry it, and the 3 s tier
             * already fired on exactly those. Lever remains opt-in
             * (YZ_PARK_REL) until the real consumer story lands. park_seq/
             * park_tf kept for the apply log's diagnostics. */
            (void)park_tf;
            /* s28 ROOT FIX (ledger #64, the "1/6" early-boot stall = a LEVER
             * MISFIRE RACE): the fast tier during BOOT-START applies the
             * release before t1 finalizes the following segment — GET runs
             * into the A2000500 placeholder at io 0x1104D00 and t1 parks in
             * its usleep(30) GPU-progress throttle forever (measured: [t1-hb]
             * sc=141 r3=0x1E full-CPU; 7/7 boots separate stalled-vs-clean
             * purely by lever-fire order at the first stopper). Gate the fast
             * tier on the UPDATE LOOP having started (g_yz_updloop_started,
             * set at the first func_00D1E838 entry) — before that, only the
             * 3 s fallback fires, which boot-start timing survives (clean
             * boots' own lever fires were effectively that late). Kill-switch
             * YZ_FASTLEVER_EARLY restores the old behavior. */
            static int fle = -1;
            if (fle < 0) fle = getenv("YZ_FASTLEVER_EARLY") ? 1 : 0;
            const int fast_ok = fle || g_yz_updloop_started;
            const int parkedfast = prel && fast_ms && fast_ok && (parked_ms > fast_ms);
            /* The FIFO ring is 8x1MB = 0x800000 (GET/PUT wrap io 0x7xxxxx -> 0x0; the
             * iomap's 0x1B00000 is the whole io SPAN incl. off-ring buffers, NOT the
             * wrap period). Hard-coded so a future HLE _cellGcmInitBody can't leak the
             * wrong size into the wrap arithmetic. */
            const uint32_t ring  = 0x800000u;
            const uint32_t ahead = (put - get + ring) % ring;   /* PUT distance ahead of GET (ring-wrapped) */
            if (journal_hle) {
                if (ahead != 0u && ahead < (ring >> 1) &&
                    yz_jrnl_hle_try(ea) == yz_jrnl_hle_park_result::applied) {
                    /* The ordered HLE applied every decoded patch first, then
                     * wrote the faithful jump-forward release word. Advance GET
                     * past the released stopper exactly like the lever does. */
                    vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, get + 4u);
                    LeaveCriticalSection(&g_rsx_fifo_lock);
                    return 1;
                }
                /* Never fall through to a release-only lever while the HLE owns
                 * the recovery decision. Unsupported entries intentionally stay
                 * parked so a partial journal can never reach RSX. */
                LeaveCriticalSection(&g_rsx_fifo_lock);
                return 0;
            }
            const uint32_t rel_entry = ((apply || parked3s || parkedfast) && ahead != 0u && ahead < (ring >> 1))
                                           ? yz_gcm_stopper_release_entry(ea) : 0u;
            if (rel_entry) {
                vm_write32(ea, 0x20000000u | ((get + 4u) & 0x1FFFFFFCu));   /* release: self-jump -> jump-forward +4 */
                vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, get + 4u);     /* GET advances into the committed body */
                /* Faithful consumption mark: GET consuming past this stopper
                 * proves everything journaled up to its release entry retired
                 * -- zero those tags (the game's GPU-progress ledger; see
                 * yz_jrnl_retire_through). */
                yz_jrnl_retire_through(rel_entry);
                /* s31 [park-line] (s31_journal_linefill.md §7): the applied
                 * entry's 128-byte line, slot tags only — discriminates "pure
                 * consumer-dispatch death" (line complete, nobody consumed)
                 * from "completion circle also binds" (line's later slots
                 * still unwritten at apply time). First 8 applies only. */
                { static int pl = 0;
                  if (pl < 8) { pl++;
                    const uint32_t line = rel_entry & ~0x7Fu;
                    fprintf(stderr, "[park-line] entry=0x%08X line=0x%08X tags: %08X %08X %08X %08X\n",
                            rel_entry, line,
                            vm_read32(line), vm_read32(line + 0x20u),
                            vm_read32(line + 0x40u), vm_read32(line + 0x60u));
                  } }
                /* s29 (ledger #65): the n<64 cap made a 900 s boot read as "64
                 * applies total" — a false lever exoneration. Keep the first 64
                 * verbose, then a census line every 256th so the steady-state
                 * fire rate stays measurable at any boot length. */
                { static unsigned long n = 0; n++;
                  if (n <= 64) {
                    fprintf(stderr, "[rsx] applied deferred release @io 0x%06X (PUT ahead 0x%X)%s parked=%llums t1seq=%ld fence0=0x%08X -> GET advances past stopper\n",
                            get, ahead,
                            parkedfast && !parked3s ? " [park-rel FAST]" :
                            parked3s ? " [park-rel 3s]" : "",
                            (unsigned long long)parked_ms, (long)g_yz_t1_sample_seq,
                            vm_read32(0x40C00000u));
                  } else if ((n & 255u) == 0u) {
                    fprintf(stderr, "[park-rel] census: %lu applies total (latest @io 0x%06X%s parked=%llums)\n",
                            n, get,
                            parkedfast && !parked3s ? " FAST" : parked3s ? " 3s" : "",
                            (unsigned long long)parked_ms);
                  }
                  /* s31 W2LIFE (ledger #71): the SPURS wid accounting in the
                   * lever-substitution regime -- every lever apply means the SPU
                   * journal consumer did NOT release this stopper. Sampled so a
                   * long boot can't exhaust the dump's 64-print cap. */
                  if (n == 1 || (n & 63u) == 0u) yz_w2life_dump("lever"); }
                LeaveCriticalSection(&g_rsx_fifo_lock);
                return 1;
            }
            if (parked3s) {   /* parked past threshold but NO journal entry: say so once per EA
                               * (discriminates "deferred entry exists" from "immediate release
                               * never executed" -- decides lever vs segment-pin, per the RE map) */
                static uint32_t said = 0;
                if (said != ea) { said = ea;
                    fprintf(stderr, "[park-rel] parked >3s @io 0x%06X (PUT ahead 0x%X) but NO tag-0x7F journal entry matches\n",
                            get, ahead); fflush(stderr); }
            }
            /* not yet committed (no tag-0x7F entry, or PUT not past) -- spin in place */
            LeaveCriticalSection(&g_rsx_fifo_lock);
            return 0;
        }
        if (!yz_rsx_io_to_ea(tgt)) {
            const uint32_t pea = yz_rsx_io_to_ea(put);
            if (put != get && pea) {
                vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, put);
                g_fifo_ret = ~0u;
                LeaveCriticalSection(&g_rsx_fifo_lock);
                return 1;
            }
            LeaveCriticalSection(&g_rsx_fifo_lock);
            return 0;
        }
        if (yz_fifo_flowlog()) {
            fprintf(stderr, "[fifo-flow] JUMP io=0x%08X -> 0x%08X (word 0x%08X)\n", get, tgt, cmd);
            fflush(stderr);
        }
        vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, tgt);
        LeaveCriticalSection(&g_rsx_fifo_lock);
        return 1;
    }
    if ((cmd & 3u) == 2u) {                                          /* call */
        const uint32_t ctgt = cmd & 0x1FFFFFFCu;                     /* CALL offset mask */
        if (!yz_rsx_io_to_ea(ctgt)) {
            LeaveCriticalSection(&g_rsx_fifo_lock);
            return 0;
        }
        if (g_fifo_ret != ~0u) {
            /* Nested CALL (s29, scratch/s29_terminal_park_re.md Q4a): RPCS3
             * checks fifo_ret_addr != RSX_CALL_STACK_EMPTY here and logs "CALL
             * found inside a subroutine" instead of silently overwriting the
             * pending return -- our one-level slot used to just get clobbered,
             * losing the outer return address with no diagnostic. */
            static unsigned long nc = 0; nc++;
            if (nc <= 64 || (nc & 255u) == 0u)
                fprintf(stderr, "[rsx] CALL inside subroutine at io=0x%08X: live return 0x%08X "
                        "would be clobbered by new return 0x%08X (n=%lu)\n",
                        get, g_fifo_ret, get + 4u, nc);
            if (yz_fifo_recover_ret_enabled()) {
                /* Faithful: don't execute this CALL (don't touch g_fifo_ret or
                 * GET) -- the outer return address survives. Retry/escalate
                 * exactly like the RETURN-without-CALL path below. */
                yz_fifo_ret_recover(get, "CALL-inside-subroutine");
                LeaveCriticalSection(&g_rsx_fifo_lock);
                return 0;
            }
            /* flag off: preserve the pre-existing default (clobber-and-proceed),
             * now with the loud log above instead of silence. */
        }
        if (yz_fifo_flowlog()) {
            fprintf(stderr, "[fifo-flow] CALL io=0x%08X -> 0x%08X (ret=0x%08X)\n", get, ctgt, get + 4u);
            fflush(stderr);
        }
        g_fifo_ret = get + 4u;                /* one-level return */
        vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, ctgt);
        LeaveCriticalSection(&g_rsx_fifo_lock);
        return 1;
    }
    if ((cmd & 0xFFFF0003u) == 0x00020000u) {                        /* return */
        if (g_fifo_ret != ~0u) {
            if (yz_fifo_flowlog()) {
                fprintf(stderr, "[fifo-flow] RET  io=0x%08X -> 0x%08X\n", get, g_fifo_ret);
                fflush(stderr);
            }
            vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, g_fifo_ret);
            g_fifo_ret = ~0u;
            LeaveCriticalSection(&g_rsx_fifo_lock);
            return 1;
        }
        /* s29 terminal park (scratch/s29_terminal_park_re.md): RPCS3 logs
         * "RET found without corresponding CALL" and calls recover_fifo() here
         * instead of idling; our one-ever warning below is unchanged (still the
         * MEASURED s28m10/s28m4 receipt), but YZ_FIFO_RECOVER_RET now adds the
         * bounded retry/escalation analog instead of a completely silent
         * forever-idle after the first print. */
        static int warned = 0;
        if (!warned) { warned = 1;
            fprintf(stderr, "[rsx] RETURN without CALL at io=0x%08X; idling\n", get); }
        if (yz_fifo_recover_ret_enabled())
            yz_fifo_ret_recover(get, "RETURN-without-CALL");
        LeaveCriticalSection(&g_rsx_fifo_lock);
        return 0;
    }

    /* ---- method packet ---- */
    /* RPCS3 RSX_METHOD_NON_METHOD_CMD_MASK 0xA0030003: after the flow-control
     * tests, a finalised method has those bits clear. If set, GET reached the
     * end of finalised commands in this segment -- wait, don't parse data. */
    if (cmd & 0xA0030003u) {
        /* s33 FIX (user-confirmed; scratch/s33_fifo_return_audit.md): the s28
         * default here modeled RPCS3's recover_fifo as "skip GET forward one
         * word" for illegal (cmd&3==3) headers. That MIS-MODELED the oracle —
         * RSXThread re-reads the SAME GET (an illegal word is content the
         * producer hasn't finalised yet; recover_fifo aborts in-flight state
         * and RETRIES, it does not advance) — and the skip demonstrably
         * STRANDED GET: s32resur1 walked 22 skips through the unwritten
         * next-phase segment at io 0x8000xx then teleported via a garbage
         * jump to io 0x200BF4 with zero further trace; s28m10's 650 s
         * RETURN-without-CALL park was the same walk ending on a foreign
         * RETURN. Default now = faithful re-read of the same GET forever,
         * with an UNCAPPED time-sampled park print (the old print capped at
         * 24 — terminal parks went invisible, ledger #65 class).
         * YZ_FIFO_SKIP4=1 restores the legacy skip for A/B. */
        static int skip4 = -1;
        if (skip4 < 0) { skip4 = getenv("YZ_FIFO_SKIP4") ? 1 : 0;
            fprintf(stderr, "[fifo-rec] ARMED (%s)\n",
                    skip4 ? "LEGACY skip-4 recovery, YZ_FIFO_SKIP4"
                          : "s33 faithful re-read; illegal words wait for the producer");
            fflush(stderr); }
        static uint32_t stuck_get = 0xFFFFFFFFu;
        static ULONGLONG stuck_t0 = 0, said_t = 0;
        static unsigned long nnc = 0;
        const ULONGLONG now = GetTickCount64();
        if (get != stuck_get) {
            stuck_get = get; stuck_t0 = now; said_t = now; nnc++;
            fprintf(stderr, "[rsx] non-command 0x%08X at io=0x%X (PUT=0x%X) -- segment not "
                    "finalised; waiting for producer (n=%lu)\n", cmd, get, put, nnc);
            fflush(stderr);
            /* s35 hole-structure dump (env YZ_FIFO_HOLE_DUMP): on first hit of a
             * given non-command park, hexdump [get, get+0x120) so the
             * unfinalised-hole layout AND where real commands resume can be read
             * directly -- the discriminator for "unfinalised Edge hole" (a small
             * data run then valid cmds) vs "GET desynced into a data segment"
             * (all data to PUT). Read-only, one dump per distinct park. */
            static int holedump = -1;
            if (holedump < 0) holedump = getenv("YZ_FIFO_HOLE_DUMP") ? 1 : 0;
            if (holedump) {
                fprintf(stderr, "[fifo-holedump] hole @io 0x%X (PUT=0x%X) g_fifo_ret=0x%08X:\n", get, put, g_fifo_ret);
                for (uint32_t off = 0; off < 0x120u; off += 0x10u) {
                    const uint32_t io0 = get + off;
                    const uint32_t e0 = yz_rsx_io_to_ea(io0);
                    if (!e0) { fprintf(stderr, "  io 0x%X: <off-ring>\n", io0); break; }
                    fprintf(stderr, "  io 0x%X: %08X %08X %08X %08X\n", io0,
                            vm_read32(e0), vm_read32(e0+4u), vm_read32(e0+8u), vm_read32(e0+12u));
                }
                fflush(stderr);
            }
        } else if (now - said_t >= 2000) {
            said_t = now;
            fprintf(stderr, "[rsx] still parked on non-command 0x%08X at io=0x%X (%llus; PUT=0x%X)\n",
                    cmd, get, (unsigned long long)((now - stuck_t0) / 1000u), put);
            fflush(stderr);
        }
        /* s35 STALE-HOLE SKIP (env YZ_FIFO_SKIP_STALE, diagnostic, default OFF):
         * when GET parks on an illegal header (cmd&3==3) for >500 ms with PUT
         * ahead, the SPU (gs_task/EDGE) never finalised this command-buffer hole
         * (the s34 consume-gap: our journal consumer scans but never writes the
         * real commands into the reserved hole). The faithful re-read then waits
         * forever for a producer that has already moved on (PUT past it). This
         * band-aid scans forward from GET for the next VALID command header/jump
         * and resumes there, dropping only the unfinalised region -- which is 3D
         * Edge geometry we cannot render anyway; the 2D loading UI + the CRI
         * preload (both frame-pump-driven) keep flowing. NOT faithful: it drops
         * a draw the same way the park-release lever synthesises a release. A
         * milestone bridge to prove the loading-screen phase is reachable while
         * the SPU-finalisation root is fixed. Kill-switch: the env flag. */
        static int skipstale = -1;
        if (skipstale < 0) { skipstale = getenv("YZ_FIFO_SKIP_STALE") ? 1 : 0;
            if (skipstale) { fprintf(stderr, "[fifo-skipstale] ARMED (YZ_FIFO_SKIP_STALE): resume past unfinalised holes after 500ms park\n"); fflush(stderr); } }
        if (skipstale && (cmd & 3u) == 3u && now - stuck_t0 >= 500) {
            uint32_t scan = get + 4u, found = 0;
            while (scan < put) {
                const uint32_t sea = yz_rsx_io_to_ea(scan);
                if (!sea) break;
                const uint32_t w = vm_read32(sea);
                const int is_jump   = ((w & 0xE0000003u) == 0x20000000u) || ((w & 3u) == 1u);
                const int is_method = (w & 0xA0030003u) == 0u && (w & 0x3FFFCu) != 0u;
                if (is_jump || is_method) { found = scan; break; }
                scan += 4u;
            }
            static unsigned long ssn = 0; ssn++;
            if (found) {
                fprintf(stderr, "[fifo-skipstale] n=%lu skipped hole io[0x%X..0x%X) -> resume at cmd 0x%08X @io 0x%X (PUT=0x%X)\n",
                        ssn, get, found, vm_read32(yz_rsx_io_to_ea(found)), found, put);
                vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, found);
            } else {
                fprintf(stderr, "[fifo-skipstale] n=%lu no valid cmd in io[0x%X,0x%X) -> resync GET to PUT\n",
                        ssn, get, put);
                vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, put);
            }
            g_fifo_ret = ~0u;
            fflush(stderr);
            LeaveCriticalSection(&g_rsx_fifo_lock);
            return 1;
        }
        if (skip4 && (cmd & 3u) == 3u && now - stuck_t0 >= 30) {
            static unsigned long recn = 0; recn++;
            fprintf(stderr, "[fifo-rec] n=%lu ILLEGAL header 0x%08X at io=0x%X -- LEGACY skip 4\n",
                    recn, cmd, get);
            fflush(stderr);
            vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, get + 4u);
            LeaveCriticalSection(&g_rsx_fifo_lock);
            return 1;
        }
        LeaveCriticalSection(&g_rsx_fifo_lock);
        return 0;
    }

    const uint32_t count   = (cmd >> 18) & 0x7FFu;
    const uint32_t noninc  = cmd & 0x40000000u;
    const uint32_t method  = cmd & 0x3FFFCu;
    const uint32_t pkt_end = get + 4u + count * 4u;

    /* The whole packet (header + count args) must be committed before we
     * dispatch (RPCS3 inc_get waits for PUT to cover each arg; PUT points one
     * past the last committed word). Linear within a segment; wrap is handled
     * by the producer's JUMP. If PUT is mid-packet, wait. */
    if (count && get < put && pkt_end > put) {
        LeaveCriticalSection(&g_rsx_fifo_lock);
        return 0;
    }

    int stalled = 0;
    for (uint32_t i = 0; i < count && !stalled; i++) {
        const uint32_t op_ea = yz_rsx_io_to_ea(get + 4u + i * 4u);
        if (!op_ea) break;
        const uint32_t eff = noninc ? method : method + i * 4u;
        const uint32_t val = vm_read32(op_ea);
        stalled = yz_rsx_method(eff, val);   /* 1 => semaphore ACQUIRE not satisfied */
    }
    if (stalled) {
        /* Leave GET on this packet header and retry (RPCS3: GET un-advanced on
         * an unsatisfied acquire) so the same method re-issues when ready. */
        LeaveCriticalSection(&g_rsx_fifo_lock);
        return 0;
    }
    vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, pkt_end);

    /* s37 fix (YZ_FLIP_ON_CONSUMER): retire any flip armed by the method(s)
     * just dispatched (e.g. GCM_FLIP_HEAD 0xE920, or the semaphore-release
     * arm heuristic, both inside yz_rsx_method above) right here on the
     * consumer thread, still under g_rsx_fifo_lock -- GET has now advanced
     * past the flip, matching RPCS3's in-FIFO-order handle_emu_flip. Retire
     * EXACTLY ONCE per arm (InterlockedExchange consumes the pending flag);
     * yz_rsx_vblank_tick's legacy retire is disabled whenever this flag is
     * on (see its "!yz_flip_on_consumer() &&" guard), so there is no
     * double-present (s23 caveat, scratch/s37_render_ordering.md 3). */
    if (yz_flip_on_consumer()) {
        uint64_t flip_ev = 0;
        for (int h = 0; h < 2; h++) {          /* PS3 has 2 active heads */
            if (!InterlockedExchange(&g_rsx_flip_pending[h], 0)) continue;
            uint32_t ha  = yz_rsx_head_addr((uint32_t)h);
            uint32_t buf = vm_read32(ha + 0x14);           /* lastQueuedBufferId */
            uint64_t t   = (uint64_t)GetTickCount() * 1000;
            { static int n = 0; if (n < 12) { n++;
                fprintf(stderr, "[flip-consumer] FLIP COMPLETE head=%d buf=%u -> clear label@0x10200010\n",
                        h, buf); } }
            if (yz_ft_on())
                yz_ft("CONSUMER-RETIRE head=%d buf=%u label-before=0x%08X",
                      h, buf, vm_read32(RSX_REPORTS + 0x10));
            yz_rsx_present(buf);
            vm_write32(ha + 0x10, buf);                    /* flipBufferId */
            vm_write32(ha + 0x08, vm_read32(ha + 0x08) | 0x80000000u); /* flip done */
            vm_write64(ha + 0x00, t);                      /* lastFlipTime */
            vm_write64(RSX_REPORTS + 0x10, 0);              /* flip sema (u128) = 0 */
            vm_write64(RSX_REPORTS + 0x18, 0);
            if (yz_ft_on()) yz_ft("CONSUMER-CLEAR label=0 head=%d", h);
            /* Bump the render-throttle fence exactly once per retired flip,
             * ordered after present+label-clear (same rule as the vblank
             * path's comment at the fence write below). */
            vm_write32(0x40C00000u, vm_read32(0x40C00000u) + 1u);
            flip_ev |= (uint64_t)(0x8u << 1);              /* SYS_RSX_EVENT_FLIP_BASE<<1 */
        }
        if (flip_ev && g_rsx_event_port) {
            uint32_t handlers = vm_read32(RSX_DRIVER_INFO + 0x12C0);
            static int uof = -1; if (uof < 0) uof = getenv("YZ_UCMD_ON_FLIP") ? 1 : 0;
            uint64_t fmask = uof ? (uint64_t)handlers : ((uint64_t)handlers & 0x7Full);
            if (fmask) {
                ppu_context sc; memset(&sc, 0, sizeof(sc));
                sc.gpr[3] = g_rsx_event_port; sc.gpr[5] = fmask;
                int64_t r = sys_event_port_send(&sc);
                static int n = 0; if (n < 8) { n++;
                    fprintf(stderr, "[flip-consumer] flip event ev=0x%llX -> send=%lld\n",
                            (unsigned long long)fmask, (long long)r); }
            }
        }
    }

    LeaveCriticalSection(&g_rsx_fifo_lock);
    return 1;
}

/* Drain the FIFO inline until it stalls/drains (bounded). Called on the PRODUCER
 * thread from the PUT-flush hook (vm_write32 -> yz_rsx_inline_on_put) and from the
 * reserve usleep wait (sys_timer.c via g_yz_usleep_pump) -- the "continuous,
 * synchronous, NOT async" RSX experiment (YZ_RSX_INLINE). */
extern "C" void yz_rsx_fifo_pump(void)
{
    if (!g_rsx_ctx_ready) return;
    int budget = 4096, steps = 0;
    while (budget-- > 0 && yz_rsx_fifo_step()) { steps++; }
    /* Lightweight liveness: total pump calls + total advanced steps, every 8192 calls. */
    static long calls = 0, total = 0; total += steps;
    if ((++calls & 0x1FFFu) == 1u)
        fprintf(stderr, "[rsx-inline] pump calls=%ld total_steps=%ld (this=%d) GET=0x%08X PUT=0x%08X\n",
                calls, total, steps,
                vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_GET), vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT));
}

/* PUT-flush hook (vm_write32 on a write to the PUT register 0x10000040). In inline
 * mode, drain GET up to the just-flushed PUT on the producer's own thread, so GET
 * tracks PUT and the producer cannot lap the ring between placing and releasing a
 * stopper. Env-gated; default boot (async consumer) is unchanged. */
extern "C" void yz_rsx_inline_on_put(void)
{
    static int on = -1; if (on < 0) on = getenv("YZ_RSX_INLINE") ? 1 : 0;
    if (on) yz_rsx_fifo_pump();
}

/* Registered into sys_timer.c so the reserve's sub-ms usleep wait pumps the FIFO
 * (GET advances while the producer waits for it -- the faithful "GPU runs while
 * the CPU waits" coupling). Set only in inline mode. */
extern "C" { void (*g_yz_usleep_pump)(void) = 0; }

static DWORD WINAPI yz_rsx_consumer(LPVOID)
{
    yz_rsx_fifo_lock_ensure();
    fprintf(stderr, "[rsx] FIFO consumer up: faithful (RPCS3 run_FIFO model)\n");
    /* s24 idle heartbeat: wall shape #2 is the consumer idling for minutes with
     * PUT far ahead and NO stopper park (the lever legitimately silent) — and
     * every return-0 path in yz_rsx_fifo_step is print-silent, so the parked
     * state was invisible (scratch/s24ride.err: GET=0x7074 PUT=0x18EF8 wedged,
     * cause unreadable). Every ~10 s of continuous non-advance, print GET/PUT
     * + the raw words at GET so the blocking command is in the log. */
    ULONGLONG idle_t0 = 0; uint32_t idle_get = ~0u;
    for (;;) {
        if (!g_rsx_ctx_ready) { SwitchToThread(); continue; }
        if (yz_rsx_fifo_step()) { idle_t0 = 0; continue; }
        const uint32_t g = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_GET);
        const ULONGLONG now = GetTickCount64();
        if (g != idle_get || !idle_t0) { idle_get = g; idle_t0 = now; }
        else if (now - idle_t0 >= 10000) {
            idle_t0 = now;
            const uint32_t p  = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT);
            const uint32_t ea = yz_rsx_io_to_ea(g);
            fprintf(stderr, "[rsx-idle] 10s no-advance GET=0x%08X PUT=0x%08X ea=0x%08X words=%08X %08X %08X %08X\n",
                    g, p, ea,
                    ea ? vm_read32(ea) : 0, ea ? vm_read32(ea + 4) : 0,
                    ea ? vm_read32(ea + 8) : 0, ea ? vm_read32(ea + 12) : 0);
            fflush(stderr);
        }
        SwitchToThread();
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

/* HLE flip: present the buffer + signal the flip label the game polls. These
 * entry points take the gcm CONTEXT as arg0 (r3), the buffer id in r4. */
extern "C" void yz_ovr__cellGcmSetFlipCommand(ppu_context* ctx)
{
    if (yz_ft_on()) yz_ft("HLE-SetFlipCommand buf=%u (label clear!)",
                          (uint32_t)ctx->gpr[4]);
    yz_rsx_present((uint32_t)ctx->gpr[4]);
    vm_write32(RSX_REPORTS + 0x10, 0);             /* flip label -> done */
    ctx->gpr[3] = 0;
}

extern "C" void yz_ovr__cellGcmSetFlipCommandWithWaitLabel(ppu_context* ctx)
{
    if (yz_ft_on()) yz_ft("HLE-SetFlipCommandWithWaitLabel buf=%u (label clear!)",
                          (uint32_t)ctx->gpr[4]);
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
        if (getenv("YZ_RSX_INLINE")) {
            /* INLINE/SYNCHRONOUS RSX: no free-running async consumer. The FIFO is
             * drained on the PRODUCER's thread -- on every PUT flush (vm_write32 ->
             * yz_rsx_inline_on_put) and during the reserve's sub-ms usleep wait
             * (sys_timer.c -> g_yz_usleep_pump). Couples GET to PUT so the producer
             * can't lap the ring between placing and releasing a stopper. */
            yz_rsx_fifo_lock_ensure();
            g_yz_usleep_pump = yz_rsx_fifo_pump;
            fprintf(stderr, "[rsx] INLINE mode: async consumer OFF; FIFO drained on producer (flush + reserve usleep)\n");
        } else if (!getenv("YZ_NO_CONSUMER")) {   /* TEMP: isolate consumer vs libgcm */
            CreateThread(NULL, 0, yz_rsx_consumer, NULL, 0, NULL);
        }
        if (getenv("YZ_IMM_REL"))        /* force immediate stopper-release (S[0x1C]=0) */
            CreateThread(NULL, 0, yz_bigseg_monitor, NULL, 0, NULL);
        if (getenv("YZ_TRACE_DEFER"))    /* READ-ONLY: trace the gcm defer decision + op-list drain */
            CreateThread(NULL, 0, yz_defer_trace_mon, NULL, 0, NULL);
        if (getenv("YZ_PHASE"))          /* high-detail producer+consumer timeline (working->broken) */
            CreateThread(NULL, 0, yz_phase_monitor, NULL, 0, NULL);
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
        /* DIAG (YZ_LOG_FIFOSET, 2026-06-24): does this syscall STOMP the consumer's
         * live GET (a3 != current GET) and/or is it the (only) PUT writer that makes
         * PUT bounce? RPCS3 serializes this under sys_rsx_mtx; we don't. */
        if (getenv("YZ_LOG_FIFOSET")) {
            uint32_t cur_get = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_GET);
            uint32_t cur_put = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT);
            static int n = 0; if (n < 400) { n++;
                fprintf(stderr, "[fifoset] pkg001 set GET 0x%06X->0x%06X  PUT 0x%06X->0x%06X%s%s\n",
                        cur_get & 0xFFFFFF, (uint32_t)a3 & 0xFFFFFF,
                        cur_put & 0xFFFFFF, (uint32_t)a4 & 0xFFFFFF,
                        ((uint32_t)a3 != cur_get) ? "  <-- GET STOMP" : "",
                        ((uint32_t)a4 < cur_put) ? "  <-- PUT RECEDES" : ""); }
        }
        if (yz_ft_on()) {
            uint32_t cg = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_GET);
            uint32_t cp = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT);
            yz_ft("FIFOSET GET 0x%06X->0x%06X PUT 0x%06X->0x%06X",
                  cg & 0xFFFFFF, (uint32_t)a3 & 0xFFFFFF,
                  cp & 0xFFFFFF, (uint32_t)a4 & 0xFFFFFF);
        }
        /* Serialize the guest GET/PUT set with the consumer's single GET writer
         * (RPCS3 sys_rsx_mtx) so a pkg001 set can't tear a GET the consumer is
         * mid-advance, and the consumer can't clobber a fresh pkg001 set. */
        yz_rsx_fifo_lock_ensure();
        EnterCriticalSection(&g_rsx_fifo_lock);
        vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, (uint32_t)a3);
        vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_PUT, (uint32_t)a4);
        LeaveCriticalSection(&g_rsx_fifo_lock);
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
        /* ONE present per flip, done by the vblank retire (s23): presenting here
         * AND arming pending double-presented every immediate flip once the
         * 0xE920 method bridge went live (title-bar flips ran ~2x Track B
         * frames, s23boot1). Record the resolved buffer on the head so the
         * retire presents the right one for the offset form too, then arm; the
         * retire presents + publishes the done bit (which also survives the
         * game's cellGcmResetFlipStatus ordering). */
        vm_write32(yz_rsx_head_addr(head) + 0x14, flip_idx);  /* lastQueuedBufferId */
        InterlockedExchange(&g_rsx_flip_pending[head], 1);
        if (yz_ft_on())
            yz_ft("SYSFLIP head=%u buf=%u a4=0x%llX (arm pending[%u], no label write)",
                  head, flip_idx, (unsigned long long)a4, head);
        break;
    }
    case 0x103: {       /* Display queue */
        uint32_t head = (uint32_t)a3 & 7;
        uint32_t ha = yz_rsx_head_addr(head);
        vm_write32(ha + 0x14, (uint32_t)a4);   /* lastQueuedBufferId */
        vm_write32(ha + 0x08, vm_read32(ha + 0x08) | 0x40000000u
                              | (1u << ((uint32_t)a4 & 31)));
        if (yz_ft_on())
            yz_ft("SYSQUEUE head=%u buf=%u (no arm)", head, (uint32_t)a4);
        /* s23 conformance fix (4th dropped-guest-notification instance, same
         * class as 0xEB00/0xE920): RPCS3's 0x103 also DELIVERS the queue event
         * -- send_event(0, SYS_RSX_EVENT_QUEUE_BASE << head, 0), sys_rsx.cpp:637,
         * sys_rsx.h:74 (1<<5). Gate on the game's registered handler mask like
         * the vblank/flip sends (Sony's intr thread dispatches by cause bits);
         * loud first hits. Kill-switch YZ_NO_QEV. */
        { static int nq = -1; if (nq < 0) { nq = getenv("YZ_NO_QEV") ? 1 : 0;
            fprintf(stderr, "[qev] armed (queue-event dispatch %s)\n",
                    nq ? "DISABLED by YZ_NO_QEV" : "on"); fflush(stderr); }
          if (!nq && g_rsx_event_port) {
            uint64_t qbit = (uint64_t)(0x20u << head);      /* QUEUE_BASE<<head */
            uint32_t handlers = vm_read32(RSX_DRIVER_INFO + 0x12C0);
            if (handlers & qbit) {
                /* s25 audit risk #1: same lossless-latch as the user-cmd —
                 * a queue-full send ORs into the shared pending mask and the
                 * consumer-top retry delivers it (edge event, loss was
                 * permanent). */
                int64_t r = yz_rsx_ev_send(qbit);
                static unsigned long qn = 0; qn++;
                if (qn <= 8 || (qn & 0xFFu) == 0) {
                    fprintf(stderr, "[qev] n=%lu head=%u buf=%u send=%lld\n",
                            qn, head, (uint32_t)a4, (long long)r); fflush(stderr); }
            } else { static int w = 0; if (w < 2) { w++;
                fprintf(stderr, "[qev] game not listening (handlers=0x%08X, qbit=0x%llX) -- benign\n",
                        handlers, (unsigned long long)qbit); fflush(stderr); } }
          } }
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
        if (yz_ft_on())
            yz_ft("RESETSTATUS head=%u and=0x%X or=0x%X", head,
                  (uint32_t)a4, (uint32_t)a5);
        break;
    }
    case 0xFEC: {       /* flip event notification (mark done immediately) */
        uint32_t head = (uint32_t)a3 & 7;
        uint32_t ha = yz_rsx_head_addr(head);
        vm_write32(ha + 0x08, vm_read32(ha + 0x08) | 0x80000000u);
        vm_write64(ha + 0x00, (uint64_t)GetTickCount() * 1000);  /* lastFlipTime */
        if (yz_ft_on()) yz_ft("FEC head=%u (done bit)", head);
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
    yz_ft_start();   /* YZ_FLIPTRACE arm banner + label watcher (no-op if off) */

    /* s25: redeliver any latched SPU throw_events (spu_channels.c loss latch,
     * notification-audit risk #3) — ~16 ms retry cadence, no-op when empty. */
    { extern void yz_throw_retry_flush(void);
      yz_throw_retry_flush(); }

    /* s28 t1 host-liveness heartbeat (env YZ_T1_HB — ledger #63, the
     * early-stall root probe): every ~2 s, t1's guest cia/lr/r1 (syscall-
     * boundary stale is fine) + the HOST thread's kernel/user CPU-time DELTAS.
     * du climbing = t1 SPINS in untagged guest code (hypothesis a); both ~0 =
     * the host thread is never rescheduled after sys_ppu_thread_create
     * returns (hypothesis b, runtime scheduling bug). Armed banner so zero
     * output is MEASURED. */
    { static int hb = -1; static ULONGLONG hbms = 0;
      static unsigned long long lk = 0, lu = 0;
      if (hb < 0) { hb = getenv("YZ_T1_HB") ? 1 : 0;
          if (hb) { fprintf(stderr, "[t1-hb] ARMED (2s host-liveness heartbeat)\n"); fflush(stderr); } }
      if (hb) { ULONGLONG now = GetTickCount64();
        if (now - hbms >= 2000) { hbms = now;
            FILETIME c1, e1, kt, ut;
            unsigned long long k = 0, u = 0;
            if (g_yz_t1_handle && GetThreadTimes(g_yz_t1_handle, &c1, &e1, &kt, &ut)) {
                k = ((unsigned long long)kt.dwHighDateTime << 32) | kt.dwLowDateTime;
                u = ((unsigned long long)ut.dwHighDateTime << 32) | ut.dwLowDateTime;
            }
            /* s28m6 answered the fork: the stalled t1 burns FULL cpu (du at
             * the healthy baseline) with zero trace output = a pure guest
             * spin. Sample the spinning RIP (brief suspend, 1/2s — mild per
             * LESSONS #6b, and the boot is already stalled when it matters)
             * and resolve to the guest function. */
            uint32_t grip = 0; unsigned long long hrip = 0;
            if (g_yz_t1_handle && SuspendThread(g_yz_t1_handle) != (DWORD)-1) {
                CONTEXT tc; memset(&tc, 0, sizeof(tc));
                tc.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(g_yz_t1_handle, &tc)) {
                    hrip = tc.Rip;
                    extern uint32_t yz_guest_addr_from_host(const void* rip);
                    grip = yz_guest_addr_from_host((const void*)tc.Rip);
                }
                ResumeThread(g_yz_t1_handle);
            }
            { extern uint32_t g_yz_t1_sc; extern uint64_t g_yz_t1_sc_r3;
              fprintf(stderr, "[t1-hb] rip=%llX guest=0x%08X sc=%u r3=0x%llX r1=0x%08X dk=%llu du=%llu\n",
                    hrip, grip, g_yz_t1_sc, (unsigned long long)g_yz_t1_sc_r3,
                    g_yz_main_ctx ? (uint32_t)g_yz_main_ctx->gpr[1] : 0,
                    k - lk, u - lu); }
            fflush(stderr);
            lk = k; lu = u; } } }

    /* s26: hardware watchpoint arm trigger (env YZ_HWWATCH — main.cpp
     * yz_hwwatch_arm): fire once at tick 2000 (~32 s, all threads live). */
    { static int hw = -1; static int hwdone = 0; static unsigned long hwt = 0;
      if (hw < 0) hw = getenv("YZ_HWWATCH") ? 1 : 0;
      if (hw && !hwdone && ++hwt == 2000) { hwdone = 1;
          yz_hwwatch_arm(); } }

    /* s26: wid4 work-record slot poll (env YZ_W4REC_POLL, diag — ledger #57
     * mode B). The page-guard write-watch on these slots was BOTH invasive
     * (shares the 4 KB page with the pool's ctx save area) and unreliable
     * (s26ride6c: pool ran, ctx saves hit the page, ZERO guard hits) — poll
     * the five 0x40-stride record slots here instead, log word0 (publish
     * value) + word1 (target EA guard) on change. No faults, ~16 ms
     * resolution; records persist between stagings so transitions are
     * caught. Correlate with [w4rec] fetch-time dumps. */
    { static int wp = -1;
      if (wp < 0) { wp = getenv("YZ_W4REC_POLL") ? 1 : 0;
          if (wp) { fprintf(stderr, "[w4poll] ARMED: record-slot poll live\n"); fflush(stderr); } }
      if (wp) {
          /* s26 ride17 addition: slot[5] = the decode label itself — [fe0]
           * proved publish-8 ISSUED while the acquire read 7 forever; this
           * poll discriminates lost-write (never becomes 8) vs reverted-write
           * (8 flickers then 7 — a stale-snapshot PUTLLC restoring the line). */
          static const uint32_t slots[6] =
              {0x424528A0u,0x424528E0u,0x42452920u,0x42452960u,0x424529A0u,
               0x10200FE0u};
          static uint32_t prev[6][2];
          static int winit = 0;
          for (int i = 0; i < 6; i++) {
              uint32_t w0 = vm_read32(slots[i]);
              uint32_t w1 = vm_read32(slots[i] + 4);
              if (!winit || w0 != prev[i][0] || w1 != prev[i][1]) {
                  fprintf(stderr, "[w4poll] slot=0x%08X val=0x%08X ea=0x%08X\n",
                          slots[i], w0, w1);
                  fflush(stderr);
                  prev[i][0] = w0; prev[i][1] = w1;
              }
          }
          winit = 1;
      } }

    /* s26: redeliver any latched RSX event bits (the s25 ucmd/EBUSY latch,
     * ledger #52) from HERE too. The consumer-top retry site DEADLOCKS when
     * the lost delivery itself parks the FIFO consumer (MEASURED s26ride4:
     * coalesced cause=2 EBUSY-latched, exactly one consumer-side retry, then
     * the consumer parked on the 0xFE0 acquire that the lost handler run
     * caused — the latch never retried again all boot). lv1 redelivers when
     * the queue drains, independent of FIFO flow; this tick is our
     * queue-drain-independent cadence (same pattern as the throw latch
     * above). Same kill-switches (YZ_NO_UCMD_RETRY / YZ_NO_EV_RETRY) gate the
     * latch at its source, so no separate gate here. */
    if (g_rsx_ev_pending) yz_ucmd_retry_pending();

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
          /* LAYER-1 BISECT (env YZ_GCMCTX_BISECT): dump libgcm's PRIVATE context
           * GCMCTX = *(game_toc-0x5014). func_00EDD15C bctrl's the handler OPDs
           * at GCMCTX[0x00] (arg [0x04]) and GCMCTX[0x08] (arg [0x0C]); the t7
           * stack-overflow recursion runs through here. Dump those OPDs + their
           * code/TOC so we can see whether a handler points back into the
           * dispatch chain (re-entrant) -- and the predicates +0x10/+0x15C/+0x398. */
          if (getenv("YZ_GCMCTX_BISECT") && g_yz_game_toc) {
              uint32_t g = vm_read32(g_yz_game_toc - 0x5014u);
              fprintf(stderr, "[gcmctx] GCMCTX=0x%08X\n", g);
              if (g >= 0x10000u && g < 0xE0000000u) {
                  const uint32_t fo[] = {0x00,0x04,0x08,0x0C,0x10,0x14,0x15C,0x164,0x398};
                  for (size_t i = 0; i < sizeof(fo)/sizeof(fo[0]); i++) {
                      uint32_t v = vm_read32(g + fo[i]);
                      uint32_t code = (v >= 0x10000u && v < 0xE0000000u) ? vm_read32(v) : 0;
                      uint32_t toc  = (v >= 0x10000u && v < 0xE0000000u) ? vm_read32(v + 4u) : 0;
                      fprintf(stderr, "[gcmctx]   +0x%03X = 0x%08X  (opd->code=0x%08X toc=0x%08X)\n",
                              fo[i], v, code, toc);
                  }
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

    /* YZ_JOBPEEK (s21): change-triggered hexdump of the CRI jobchain command
     * stream (0x4019CA80-CB40) + the chain header (0x4019C880-8C0), so the
     * producer's round-N command WRITES are visible independently of the SPU
     * side's fetches -- discriminates "t1 never wrote round 3" from "chain
     * never fetched round 3". Checked once per vblank tick, dumps on change.
     * s23: rows also decode SYMBOLICALLY via yz_jc_dec below -- the raw-hex-only
     * dump cost us two weeks of calling the 0x0000000800000012 park word "END"
     * when it is JTS (jump-to-self stopper, releasable by one store). */
    { static int jp = -1;
      if (jp < 0) { jp = getenv("YZ_JOBPEEK") ? 1 : 0;
          if (jp) fprintf(stderr, "[jobpeek] ARMED (YZ_JOBPEEK): watching 0x4019CA80-CB40 + hdr 0x4019C880\n"); }
      if (jp) {
          static uint64_t lasth = 0;
          uint64_t h = 1469598103934665603ull;   /* FNV-1a over both regions */
          for (uint32_t a = 0x4019CA80u; a < 0x4019CB40u; a += 4)
              { h ^= vm_read32(a); h *= 1099511628211ull; }
          for (uint32_t a = 0x4019C880u; a < 0x4019C8C0u; a += 4)
              { h ^= vm_read32(a); h *= 1099511628211ull; }
          if (h != lasth) {
              lasth = h;
              fprintf(stderr, "[jobpeek] hdr 0x4019C880:");
              for (uint32_t a = 0x4019C880u; a < 0x4019C8C0u; a += 4)
                  fprintf(stderr, " %08X", vm_read32(a));
              fprintf(stderr, "\n");
              for (uint32_t row = 0x4019CA80u; row < 0x4019CB40u; row += 0x20) {
                  char dec[128]; size_t dp = 0; dec[0] = 0;
                  fprintf(stderr, "[jobpeek] cmd 0x%08X:", row);
                  for (uint32_t a = row; a < row + 0x20; a += 8) {
                      uint64_t w = ((uint64_t)vm_read32(a) << 32) | vm_read32(a + 4);
                      fprintf(stderr, " %08X %08X",
                              (uint32_t)(w >> 32), (uint32_t)w);
                      /* SPURS jobchain command decode (s23). Opcode table per
                       * RPCS3 cellSpurs.h:266-281 (CELL_SPURS_JOB_OPCODE_*):
                       * low 3 bits = class; class-2 sub in bits 3-6 (SYNC=0x02,
                       * LWSYNC=0x12, JTS=bit35|LWSYNC -- a STOPPER released by
                       * overwriting the word); class-7 sub in bits 3-6
                       * (GUARD=1, SET_LABEL=2, RET=14, END=15, ABORT=0). */
                      const char* t; char tb[24];
                      uint32_t lo3 = (uint32_t)(w & 7u);
                      if (w == 0)            t = "NOP";
                      else if (w == 5)       t = "FLUSH";
                      else if (lo3 == 2) {
                          uint64_t s = w & ~0x800000000ull;
                          if (s == 0x02)      t = "SYNC";
                          else if (s == 0x12) t = (w & 0x800000000ull) ? "JTS" : "LWSYNC";
                          else                t = "SYNC?";
                      } else if (lo3 == 7) {
                          uint32_t sub = (uint32_t)((w >> 3) & 0xFu);
                          if (sub == 15)      t = "END";
                          else if (sub == 14) t = "RET";
                          else if (sub == 1) { snprintf(tb, sizeof(tb), "GUARD@%04X",
                                                        (uint32_t)(w & ~127ull) & 0xFFFFu); t = tb; }
                          else if (sub == 0)  t = "ABORT";
                          else                t = "CMD7?";
                      } else {
                          static const char* cn[8] =
                              { "JOB", "RESET_PC", "?", "NEXT", "CALL", "?", "JOBLIST", "?" };
                          snprintf(tb, sizeof(tb), "%s@%04X", cn[lo3],
                                   (uint32_t)(w & ~7ull) & 0xFFFFu); t = tb;
                      }
                      dp += (size_t)snprintf(dec + dp, sizeof(dec) - dp, " %s",
                                             t);
                      if (dp >= sizeof(dec) - 1) break;
                  }
                  fprintf(stderr, " |%s\n", dec);
              }
              fflush(stderr);
          }
      }
    }

    /* YZ_CNTGATE (s23): the audio-round COUNT GATE probe. RE (caller-chain map):
     * every frame t1's tick chain calls func_00E5F248, which reads the pending
     * count at G+0x18 (G = *(*(game_toc-0x7B28)+0x20), read at pc 0x00E5F27C)
     * and passes it to func_00E5F094 -- which writes NO JOB and returns when
     * count==0 (early exit pc 0xE5F0D8; a FULL ring would usleep-block instead).
     * Round 3's missing JOB ⇒ count was 0. This probe resolves G, prints the
     * count EA once (for a follow-up YZ_WATCH_WR), and logs count + the round
     * counter @0x015F4410 on change -- one boot names the starved queue and
     * whether the enqueuer ever runs again. */
    { static int cg = -1;
      if (cg < 0) { cg = getenv("YZ_CNTGATE") ? 1 : 0;
          if (cg) fprintf(stderr, "[cntgate] ARMED (YZ_CNTGATE)\n"); }
      if (cg && g_yz_game_toc) {
          static uint32_t lastc = 0xFFFFFFFFu, lastr = 0xFFFFFFFFu;
          static int announced = 0;
          uint32_t O = vm_read32(g_yz_game_toc - 0x7B28u);
          uint32_t G = (O >= 0x10000u && O < 0xE0000000u) ? vm_read32(O + 0x20u) : 0;
          if (G >= 0x10000u && G < 0xE0000000u) {
              if (!announced) { announced = 1;
                  fprintf(stderr, "[cntgate] O=0x%08X G=0x%08X count-EA=0x%08X\n",
                          O, G, G + 0x18u); fflush(stderr); }
              uint32_t c = vm_read32(G + 0x18u);
              uint32_t r = vm_read32(0x015F4410u);
              /* s23 boot5 ADDENDUM: the round counter moved only 1->2 all boot =
               * the DRIVER (func_00A9F8AC) itself ran exactly twice, so the gate
               * is ABOVE the count read. Also watch the driver's own guard field
               * (absolute-EA check that skips its body when 0, per the chain RE)
               * and the fields it clears each round. */
              uint32_t gf = vm_read32(0x014EC864u);
              uint32_t f1c = vm_read32(0x015F441Cu), f20 = vm_read32(0x015F4420u);
              static uint32_t lgf = 0xFFFFFFFFu, lf1c = 0xFFFFFFFFu, lf20 = 0xFFFFFFFFu;
              if (c != lastc || r != lastr || gf != lgf || f1c != lf1c || f20 != lf20) {
                  lastc = c; lastr = r; lgf = gf; lf1c = f1c; lf20 = f20;
                  fprintf(stderr, "[cntgate] round=0x%02X count=0x%08X guard=0x%08X f1C=0x%08X f20=0x%08X\n",
                          r & 0xFFu, c, gf, f1c, f20);
                  fflush(stderr); }
          }
      }
    }

    /* YZ_FLIPTRACE add-on (s21, the phase-2 consumer park): if GET has been
     * FROZEN for ~4 s with PUT ahead (unconsumed commands), dump the FIFO words
     * around GET once per freeze episode -- classifies the silent park (stopper
     * jump-to-self vs CALL vs method packet) that no existing log catches
     * (boot 4: GET=0x4C24 PUT=0x18EF8 frozen, zero consumer prints). */
    if (yz_ft_on()) {
        static uint32_t pg = 0xFFFFFFFFu; static unsigned frozen = 0;
        static uint32_t dumped_at = 0xFFFFFFFFu;
        uint32_t get = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_GET) & 0xFFFFFF;
        uint32_t put = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT) & 0xFFFFFF;
        if (get == pg && get != put) frozen++; else { frozen = 0; pg = get; }
        if (frozen == 256 && dumped_at != get) {   /* ~4 s at 62.5 Hz */
            dumped_at = get;
            yz_ft("FIFOPARK GET=0x%06X PUT=0x%06X frozen ~4s; words around GET:",
                  get, put);
            for (int32_t off = -0x20; off <= 0x3C; off += 4) {
                uint32_t io = get + (uint32_t)off;
                if ((int32_t)(get + off) < 0) continue;
                uint32_t ea = yz_rsx_io_to_ea(io);
                fprintf(stderr, "[ft]   io 0x%06X = %08X%s\n",
                        io, ea ? vm_read32(ea) : 0xDEADDEAD,
                        io == get ? "  <-- GET" : "");
            }
            fflush(stderr);
        }
    }

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

    /* pt35 VALIDATION (env YZ_FORCE_CODEC): force the cri_audio codec workload
     * (wid 3) selectable. wid3 is runnable + has priority + readyCount=1, but its
     * wklCurrentContention[3] is pinned at 1 (== maxContention), so the kernel's
     * select gate `maxContention > contention` (cellSpursSpu.cpp:331) fails. Clear
     * contention[3] + pending[3] and (re)assert readyCount[3] each vblank so the
     * kernel selects wid3 -> policy -> StartTask -> spu_task_launch runs cri_audio.
     * Validates the lift+launch+decode chain while the proper contention-accounting
     * fix is pending. Offsets: wklReadyCount1@+0x00, wklCurrentContention@+0x20,
     * wklPendingContention@+0x30 (u8[16], per-wid). */
    if (g_yz_spurs_taskset && getenv("YZ_FORCE_CODEC")) {
        uint32_t sp = vm_read32(g_yz_spurs_taskset + 0x64u);
        if (sp >= 0x10000u && sp < 0xE0000000u) {
            vm_write8(sp + 0x23u, 0);   /* wklCurrentContention[3] = 0 */
            vm_write8(sp + 0x33u, 0);   /* wklPendingContention[3]  = 0 */
            vm_write8(sp + 0x03u, 1);   /* wklReadyCount1[3]        = 1 */
            vm_write8(sp + 0x72u, (uint8_t)(vm_read8(sp + 0x72u) | 0x1Fu));
            vm_write8(sp + 0xBDu, (uint8_t)(vm_read8(sp + 0xBDu) | 0x1Fu));
            { static unsigned fc=0; if((fc++ & 127u)==0)
                fprintf(stderr, "[force-codec] sp=0x%08X cleared wklCurCont[3], rc[3]=1\n", sp); }
        }
    }

    /* pt35: once the codec taskset exists, dump its enabled-bitset + task_info[].elf
     * to settle whether CreateTaskWithAttr populated the codec ELF (0x012B4980) or
     * left task_info empty (-> the elf=0 the policy reads at StartTask). CellSpursTaskset:
     * enabled@0x30, task_info[128]@0x80 (48 bytes each), TaskInfo.elf EA low32 @+0x14. */
    /* pt35 FIX (env YZ_FIXRUN): our lifted cellSpursCreateTask wrongly sets the
     * codec task's `running` bit at creation (RPCS3 sets only enabled + pending_ready;
     * cellSpurs.cpp:4139/4187). With running set, the policy's SELECT_TASK
     * (readyButNotRunning = (signalled|ready|pready) & ~running) never picks it, so
     * the codec is never dispatched (taskId=-1, elf=0 at StartTask). Clear running
     * for the codec taskset ONCE after creation -- safe because nothing reads that
     * taskset until wid3 is selected, which this very bit is blocking. */
    if (g_yz_codec_taskset && getenv("YZ_FIXRUN")) {
        static int fixed = 0;
        if (!fixed) {
            uint32_t ts = g_yz_codec_taskset, run = vm_read32(ts + 0x00u);
            if (run & 0x80000000u) {
                vm_write32(ts + 0x00u, run & 0x7FFFFFFFu);   /* clear running[task0] */
                fixed = 1;
                fprintf(stderr, "[fixrun] codec taskset 0x%08X running 0x%08X -> 0x%08X\n",
                        ts, run, run & 0x7FFFFFFFu); fflush(stderr);
            }
        }
    }

    if (g_yz_codec_taskset && getenv("YZ_TASK_TRACE")) {
        /* pt35e A/B test: the SPURS kernel re-selects a taskset workload iff
         * wklReadyCount1[wid] != 0 OR wklSignal1 bit for wid is set (cellSpursSpu.cpp:333).
         * SPURS instance @ 0x40197C80: wklReadyCount1[0x10]@+0x00 (wid3 = low byte of the
         * +0x00 BE word), wklSignal1 (BE u16)@+0x70 (wid3 bit = 0x8000>>3 = 0x1000). If
         * either is set in MAIN MEMORY -> the PPU DID make wid3 eligible and the idle SPU
         * just missed the wake (gate = B). If neither ever sets -> the create never bumps
         * eligibility (gate = A, the unimplemented readyCount/wklSignal path). */
        const uint32_t SP = 0x40197C80u;
        uint32_t rc1 = vm_read32(SP + 0x00u);    /* wklReadyCount1 wid0..3 (BE) */
        uint32_t sig1w = vm_read32(SP + 0x70u);  /* wklSignal1 (BE u16) in high half */
        unsigned rcWid3 = rc1 & 0xFFu;
        unsigned sigWid3 = ((sig1w >> 16) & 0x1000u) ? 1u : 0u;
        /* pt35e: cellSpursSendWorkloadSignal only sets wklSignal1 if wklState(wid)==RUNNABLE
         * (cellSpurs.cpp:2805). wklState1[0x10]@SPURS+0x80 (wid3 = low byte of +0x80 word);
         * RUNNABLE=2. If wid3 never reaches 2, that guard fails -> signal never sent. */
        uint32_t wkst1 = vm_read32(SP + 0x80u);
        unsigned stWid3 = wkst1 & 0xFFu;
        static int runnable = 0;
        if (!runnable && stWid3 == 2u) { runnable = 1;
            fprintf(stderr, "[codec-runnable] *** wid3 wklState reached RUNNABLE(2) -- SendWorkloadSignal's state guard would pass ***\n"); fflush(stderr); }
        static int elig = 0;
        if (!elig && (rcWid3 || sigWid3)) { elig = 1;
            fprintf(stderr, "[codec-eligible] *** wid3 readyCount=%u wklSignal=%u -> PPU DID make wid3 eligible (gate = B: idle SPU missed the wake) ***\n",
                    rcWid3, sigWid3); fflush(stderr); }
        /* pt35e FIX TEST (env YZ_WKLSIG): the LLE task-creation path sets pending_ready
         * but never lands SendWorkloadSignal -> wklSignal1[wid3] stays 0 -> the kernel
         * never selects the codec taskset. Do the one missing step (RPCS3 cellSpurs.cpp:2812:
         * sig |= 0x8000 >> (wid%16)) once the task is pending. The SPU kernel polls this
         * line via GETLLAR (YZ_FRC proves it), so no wakeup is needed. If this unblocks the
         * codec, the workload-signal gap is confirmed as THE root + replaces YZ_FRC/etc. */
        if (getenv("YZ_WKLSIG") && stWid3 == 2u && !sigWid3 &&
            vm_read32(g_yz_codec_taskset + 0x20u) != 0) {
            /* ROOT FIX TEST (pt35e): the codec's task_start->SendWorkloadSignal(wid3) BAILED
             * because wid3's workload wasn't enabled/RUNNABLE yet when the task was created
             * (the task is created before the SPU kernel processes the workload-add). RE-SEND
             * the missed signal now that the guard would pass (wklState[wid3]==RUNNABLE + a
             * pending task). This is EXACTLY the SendWorkloadSignal that should have fired;
             * the game's real taskset setup is left intact, so the codec should dispatch
             * CLEANLY (unlike the earlier inconsistent ready/readyCount force that crashed). */
            /* The kernel's lifted SELECT acts on readyCount (drove contention before) but
             * not on wklSignal alone. Set BOTH, coherently, leaving the game's taskset
             * bitsets INTACT (no pReady/ready manipulation -> the policy's SELECT_TASK runs
             * normally -> should dispatch cleanly). Re-assert while pending so the workload
             * stays selectable across the multi-step dispatch. */
            static int n = 0;
            if (n < 200) { n++;
                uint32_t sg = vm_read32(SP + 0x70u);
                vm_write32(SP + 0x70u, sg | (0x1000u << 16));      /* wklSignal1[wid3] */
                uint32_t rc = vm_read32(SP + 0x00u);
                vm_write32(SP + 0x00u, (rc & 0xFFFFFF00u) | 1u);   /* wklReadyCount1[wid3]=1 */
                if (n == 1) { fprintf(stderr, "[wklsig] RE-SEND signal+readyCount[wid3] (RUNNABLE+pending, bitsets intact)\n"); fflush(stderr); } }
        }
        static int cd = 0;
        if (cd < 30) { cd++;
            uint32_t ts = g_yz_codec_taskset;
            uint32_t enabled = vm_read32(ts + 0x30u), wid = vm_read32(ts + 0x74u);
            /* Full bitset state (CellSpursTaskset): running@0x00 ready@0x10
             * pending_ready@0x20 enabled@0x30 signalled@0x40 waiting@0x50.
             * RPCS3 after create+start: enabled+pending_ready set, running/ready=0. */
            fprintf(stderr, "[codec-ts] ts=0x%08X wid=%u run=0x%08X rdy=0x%08X pReady=0x%08X en=0x%08X sig=0x%08X wait=0x%08X | SPURS rc1=0x%08X sig1=0x%04X cont=0x%08X rcWid3=%u sigWid3=%u",
                    ts, wid, vm_read32(ts+0x00u), vm_read32(ts+0x10u), vm_read32(ts+0x20u),
                    enabled, vm_read32(ts+0x40u), vm_read32(ts+0x50u),
                    rc1, (sig1w >> 16) & 0xFFFFu, vm_read32(SP+0x20u), rcWid3, sigWid3);
            fprintf(stderr, " wkState1=0x%08X stWid3=%u(2=RUNNABLE)", wkst1, stWid3);
            /* pt35e ROOT test: CellSpursTaskset.spurs @ +0x60 (be64, EA low word @ +0x64).
             * task_start does `spurs = taskset->spurs; cellSpursSendWorkloadSignal(spurs,wid)`.
             * If this isn't 0x40197C80, SendWorkloadSignal gets a bad ptr + bails -> no signal. */
            fprintf(stderr, " tsSpurs=0x%08X(expect 0x40197C80)", vm_read32(ts + 0x64u));
            for (int t = 0; t < 4; t++) {
                uint32_t elf = vm_read32(ts + 0x80u + (uint32_t)t*0x30u + 0x14u);
                fprintf(stderr, " t%d.elf=0x%08X", t, elf);
            }
            fprintf(stderr, "\n"); fflush(stderr);
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
            /* pt30c: dump ALL enabled workloads' images -> is the cri_audio SPU codec
             * (0x012B4980) registered as a workload (=> SPURS-scheduling gate) or absent
             * (=> criMana never attached it)? Re-dump only when wklEnabled changes. */
            if (sp>=0x10000u && sp<0xE0000000u) {
                static uint32_t last_wkle = 0xFFFFFFFFu;
                uint32_t wkle = vm_read32(sp + 0xB0u);
                if (wkle != last_wkle) { last_wkle = wkle;
                    fprintf(stderr, "[spurs] WORKLOADS wklEnabled=0x%08X (seeking cri_audio 0x012B4980):\n", wkle);
                    for (int w = 0; w < 16; w++) {
                        if (!(wkle & (1u << (31 - w)))) continue;
                        uint32_t wii = sp + 0xB00u + (uint32_t)w * 0x20u;
                        uint32_t a  = vm_read32(wii + 0x04u);
                        uint32_t sz = vm_read32(wii + 0x10u);
                        const char* tag = (a == 0x012B4980u) ? "  <== cri_audio CODEC" :
                                          (a >= 0x012B0000u && a < 0x012F0000u) ? "  <== CRI image range" : "";
                        fprintf(stderr, "[spurs]   wkl[%2d] image=0x%08X size=0x%X%s\n", w, a, sz, tag);
                    }
                    fflush(stderr);
                }
            }
            /* pt33: readyCount + state per enabled workload. The kernel selects by
             * readyCount; wid 3 (cri_audio taskset) is never selected -> is its
             * readyCount 0 (never ACTIVATED by the service) or set-but-ignored? */
            if (sp>=0x10000u && sp<0xE0000000u) {
                uint32_t wkle2 = vm_read32(sp + 0xB0u);
                char buf[300]; int o = 0; buf[0] = 0;
                for (int w = 0; w < 8 && o < 260; w++) {
                    if (!(wkle2 & (1u << (31 - w)))) continue;
                    uint8_t rc = (uint8_t)(vm_read32(sp + 0x00u + (uint32_t)(w & ~3)) >> (8*(3-(w&3))));
                    uint8_t st = (uint8_t)(vm_read32(sp + 0x80u + (uint32_t)(w & ~3)) >> (8*(3-(w&3))));
                    uint8_t mc = (uint8_t)(vm_read32(sp + 0x50u + (uint32_t)(w & ~3)) >> (8*(3-(w&3))));
                    o += snprintf(buf+o, sizeof(buf)-o, " wid%d[rc=%u st=%02X maxC=%u]", w, rc, st, mc);
                }
                static int rcq = 0;
                if (rcq < 50) { rcq++; fprintf(stderr, "[spurs] readyCounts:%s\n", buf); fflush(stderr); }
            }
            /* pt35 (fresh): dump the SELECTION GATES for EVERY enabled workload, not
             * just gs_task's wid=2. The codec (wid 3) lives in a DIFFERENT taskset,
             * so the per-taskset GATES line below never shows it. Read straight from
             * the shared SPURS struct (sp). Selection (cellSpursSpu.cpp:519) needs:
             * priority>0 (per-SPU) && maxContention>curContention && readyCount>0. */
            if (sp>=0x10000u && sp<0xE0000000u) {
                static int gq = 0;
                if (gq < 30) { gq++;
                    uint32_t wkle3 = vm_read32(sp + 0xB0u);
                    for (int w = 0; w < 8; w++) {
                        if (!(wkle3 & (1u << (31 - w)))) continue;
                        uint8_t cc = (uint8_t)(vm_read32(sp+0x20u+(uint32_t)(w&~3)) >> (8*(3-(w&3))));
                        uint8_t mc = (uint8_t)(vm_read32(sp+0x50u+(uint32_t)(w&~3)) >> (8*(3-(w&3))));
                        uint8_t st = (uint8_t)(vm_read32(sp+0x80u+(uint32_t)(w&~3)) >> (8*(3-(w&3))));
                        uint8_t stat=(uint8_t)(vm_read32(sp+0x90u+(uint32_t)(w&~3)) >> (8*(3-(w&3))));
                        uint8_t rcb = (uint8_t)(vm_read32(sp+0x00u+(uint32_t)(w&~3)) >> (8*(3-(w&3))));
                        uint32_t wiw = sp + 0xB00u + (uint32_t)w*0x20u;
                        uint32_t pHi = vm_read32(wiw + 0x18u), pLo = vm_read32(wiw + 0x1Cu);
                        bool prioNZ = (pHi|pLo) != 0;
                        bool selectable = prioNZ && (mc > cc) && (rcb > cc);
                        fprintf(stderr, "[gate] wid%d: rc=%u cur=%u max=%u state=0x%02X status=0x%02X "
                                "prio=%08X_%08X %s%s\n", w, rcb, cc, mc, st, stat, pHi, pLo,
                                prioNZ ? "" : "PRIO=0 ", selectable ? "<= SELECTABLE" : "(blocked)");
                    }
                    fflush(stderr);
                }
            }
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
         * releases the consumer's `ACQUIRE label+0x10 == 0`.
         *
         * s37 fix (YZ_FLIP_ON_CONSUMER): when armed, the FIFO consumer thread
         * (yz_rsx_fifo_step) retires the flip instead, in FIFO order, so this
         * thread does ONLY vBlankCount + the VBLANK event below -- mirroring
         * RPCS3's explicit "wrong thread" 0xFED guard (sys_rsx.cpp:896-900).
         * The "!yz_flip_on_consumer() &&" guard is the ONLY change on this
         * path; default OFF leaves the InterlockedExchange/present/etc. below
         * byte-for-byte as before. Skipping this entirely (rather than just
         * not presenting) also guarantees g_rsx_flip_pending is consumed by
         * exactly one thread -- no double-retire race between the two. */
        if (!yz_flip_on_consumer() && InterlockedExchange(&g_rsx_flip_pending[h], 0)) {
            uint32_t buf = vm_read32(ha + 0x14);          /* lastQueuedBufferId */
            { static int n = 0; if (n < 12) { n++;
                fprintf(stderr, "[vbl] FLIP COMPLETE head=%d buf=%u -> clear label@0x10200010\n", h, buf); } }
            if (yz_ft_on())
                yz_ft("VBL-RETIRE head=%d buf=%u label-before=0x%08X",
                      h, buf, vm_read32(RSX_REPORTS + 0x10));
            yz_rsx_present(buf);
            vm_write32(ha + 0x10, buf);                   /* flipBufferId */
            vm_write32(ha + 0x08, vm_read32(ha + 0x08) | 0x80000000u); /* flip done */
            vm_write64(ha + 0x00, t);                     /* lastFlipTime */
            vm_write64(RSX_REPORTS + 0x10, 0);            /* flip sema (u128) = 0 */
            vm_write64(RSX_REPORTS + 0x18, 0);
            if (yz_ft_on()) yz_ft("VBL-CLEAR label=0 head=%d", h);
            /* Faithful flip-completion (replaces the YZ_FLIPADV band-aid's external
             * fence nudge): advance the counter the game's render throttle
             * func_00EAC46C polls (`while *(0x40C00000)+2 <= target`). Real RSX
             * bumps this once per presented flip; we present right here, so bump it
             * here -- exactly once per retired flip, ordered after present+label-clear. */
            vm_write32(0x40C00000u, vm_read32(0x40C00000u) + 1u);
            flip_ev |= (uint64_t)(0x8u << 1);             /* SYS_RSX_EVENT_FLIP_BASE<<1 */

            /* LAYER-1 ROOT BISECT (env YZ_GCMCTX_BISECT, pt48). Hypothesis: the
             * t7 _gcm_intr_thread stack-overflow recursion is the EVENT-DELIVERY
             * handshake -- libgcm's own private context predicates never clear,
             * so func_00EDC1F0 (flip-complete <=> GCMCTX+0x10==0) and
             * func_00EDD15C (re-arm latch, re-dispatches while GCMCTX+0x398==1)
             * keep re-firing. libgcm clears these ONLY when it fully processes a
             * FLIP event. Here we zero them ourselves on flip completion: if the
             * t7 recursion stops, the root is confirmed as the event handshake
             * (not the FIFO, not a wrong completion EA). GCMCTX = *(game_toc -
             * 0x5014); expected 0x01654130 -- log it once to re-confirm. */
            if (getenv("YZ_GCMCTX_BISECT") && g_yz_game_toc) {
                uint32_t gcmctx = vm_read32(g_yz_game_toc - 0x5014u);
                static int dumped = 0;
                if (!dumped) { dumped = 1;
                    fprintf(stderr, "[gcmctx-late] game_toc=0x%08X GCMCTX=0x%08X (LATE, at flip #1)\n",
                            g_yz_game_toc, gcmctx);
                    if (gcmctx >= 0x10000u && gcmctx < 0xE0000000u) {
                        const uint32_t fo[] = {0x00,0x04,0x08,0x0C,0x10,0x14,0x18,0x28,0x30,0x15C,0x164,0x168,0x398};
                        for (size_t i = 0; i < sizeof(fo)/sizeof(fo[0]); i++)
                            fprintf(stderr, "[gcmctx-late]   +0x%03X = 0x%08X\n", fo[i], vm_read32(gcmctx + fo[i]));
                    }
                    fflush(stderr);
                }
                if (gcmctx >= 0x10000u && gcmctx < 0xE0000000u) {
                    vm_write32(gcmctx + 0x10u, 0);    /* flip no longer pending */
                    vm_write32(gcmctx + 0x398u, 0);   /* clear the re-arm latch  */
                }
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
        /* On a flip, deliver the flip+vblank bits (Sony's LLE bit assignment is
         * not RPCS3's HLE enum -- handlers=0x6 => vblank bit1 + flip bit2); on a
         * plain vblank, just VBLANK (0x2).
         * s26 ROOT FIX (ledger #57 mode B): the old `flip_ev ? handlers` shape
         * delivered ALL registered bits -- written when handlers was 0x6, it
         * silently started including bit 0x80 (USER_CMD) once the game
         * registered its user handler (mask 0x86 at the movie boundary). Every
         * flip then spuriously dispatched the user handler with the
         * not-yet-written userCmdParam: handler(0) staged a val=0 work record
         * + consumed the pool task's wake -> the real cause's publish desynced
         * (publishes of 0 / stale re-publishes; s26ride8/9 [chain] hit#1
         * r3=0x0 before the first [ucmd]). USER_CMD is delivered EXCLUSIVELY
         * by the 0xEB00 method path. Kill-switch YZ_UCMD_ON_FLIP restores the
         * over-broadcast. */
        static int uof = -1;
        if (uof < 0) uof = getenv("YZ_UCMD_ON_FLIP") ? 1 : 0;
        uint64_t fmask = uof ? (uint64_t)handlers : ((uint64_t)handlers & 0x7Full);
        uint64_t ev = (flip_ev ? fmask : ((uint64_t)0x2 & handlers));
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

    /* Batch fixes item 12 (RPCS3 sys_spu.h:107): sys_spu_image segment count
     * is capped at 0x20 -- an image that would overflow that (malformed or
     * adversarial phdr table) must fail ENOMEM instead of driving an
     * oversized heap allocation / segment table. */
    if (nsegs <= 0 || nsegs > 0x20) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_ENOMEM;
        return;
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
/* Copy a guest string into `tmp` without letting the host CRT touch
 * unmapped memory. A guest %s arg can point into an uncommitted region of
 * the 4 GB vm reservation (measured 2026-07-03: t10's _sys_printf died in
 * libucrt walking such a string -- the recurring silent long-boot death,
 * import thunk 372, crash rva resolving past CRT `remove`). Probes
 * committed-ness per page; stops at NUL, cap, or the first unreadable
 * page. Returns tmp (always NUL-terminated). */
static const char* yz_guest_str_safe(uint32_t ea, char* tmp, size_t cap)
{
    size_t o = 0;
    const uint8_t* p = (const uint8_t*)(vm_base + ea);
    uintptr_t page = 0;   /* last page verified committed+readable */
    while (o + 1 < cap) {
        uintptr_t cur = (uintptr_t)(p + o) & ~(uintptr_t)0xFFF;
        if (cur != page) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery((const void*)cur, &mbi, sizeof(mbi)) ||
                mbi.State != MEM_COMMIT ||
                (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                if (o == 0) { snprintf(tmp, cap, "(badptr:%08X)", ea); return tmp; }
                break;
            }
            page = cur;
        }
        uint8_t c = p[o];
        if (!c) break;
        tmp[o++] = (char)c;
    }
    tmp[o] = 0;
    return tmp;
}

static int yz_format_guest(char* out, size_t outsz, ppu_context* ctx,
                           uint32_t fmt_ea, int first_vararg /* 0 = r3 */)
{
    char ftmp[1024];   /* format walked byte-by-byte -- same badptr class */
    const char* f = yz_guest_str_safe(fmt_ea, ftmp, sizeof(ftmp));
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
             * pointer (mis-indexed vararg or genuine garbage); anything else
             * goes through the page-probing copy so an EA into uncommitted
             * vm space can't fault the host CRT (the t10 silent-death class). */
            char stmp[448];
            const char* s = ((uint32_t)a >= 0x10000u)
                          ? yz_guest_str_safe((uint32_t)a, stmp, sizeof(stmp))
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
