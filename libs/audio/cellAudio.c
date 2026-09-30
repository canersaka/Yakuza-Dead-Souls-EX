/*
 * ps3recomp - cellAudio HLE implementation
 *
 * Real audio mixing and output. A background mixing thread reads audio data
 * from each active port's buffer in guest memory, mixes them, and outputs
 * to the host audio device.
 *
 * Output: WASAPI shared mode.
 */

#include "cellAudio.h"
#include <stdio.h>
#include <string.h>
#include "ps3emu/endian.h"
#include "../../runtime/memory/vm.h"   /* vm_commit, vm_to_host */

/* sys_event helpers (C-linkage; defined in runtime/syscalls/sys_event.c).
 * Declared directly to avoid pulling sys_event.h's C++ guards into this C file. */
extern int          sys_event_queue_push_by_id(unsigned int qid, unsigned long long src,
                                               unsigned long long d1, unsigned long long d2,
                                               unsigned long long d3);
extern unsigned int sys_event_find_queue_by_key(unsigned long long key);

/* ---------------------------------------------------------------------------
 * WASAPI / threading setup
 * -----------------------------------------------------------------------*/

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <process.h>

/* Define WASAPI COM GUIDs (avoids needing uuid.lib linkage for these) */
#define DEFINE_AUDIO_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
        const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
DEFINE_AUDIO_GUID(ps3r_CLSID_MMDeviceEnumerator, 0xBCDE0395,0xE52F,0x467C,0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E);
DEFINE_AUDIO_GUID(ps3r_IID_IMMDeviceEnumerator,  0xA95664D2,0x9614,0x4F35,0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6);
DEFINE_AUDIO_GUID(ps3r_IID_IAudioClient,         0x1CB9AD4C,0xDBFA,0x4c32,0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2);
DEFINE_AUDIO_GUID(ps3r_IID_IAudioRenderClient,   0xF294ACFC,0x3146,0x4483,0xA7,0xBF,0xAD,0xDC,0xA7,0xC2,0x60,0xE2);

/* Map to the names used in the code */
#define IID_IMMDeviceEnumerator  ps3r_IID_IMMDeviceEnumerator
#define CLSID_MMDeviceEnumerator ps3r_CLSID_MMDeviceEnumerator
#define IID_IAudioClient         ps3r_IID_IAudioClient
#define IID_IAudioRenderClient   ps3r_IID_IAudioRenderClient

typedef HANDLE thread_t;
typedef CRITICAL_SECTION mutex_t;
#define mutex_init(m)    InitializeCriticalSection(m)
#define mutex_destroy(m) DeleteCriticalSection(m)
#define mutex_lock(m)    EnterCriticalSection(m)
#define mutex_unlock(m)  LeaveCriticalSection(m)

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

typedef struct {
    int                  in_use;
    int                  running;
    CellAudioPortParam   param;
    float*               buffer;        /* host view of the guest-VM ring (big-endian float samples) */
    u32                  buf_size;       /* buffer size in bytes */
    u64                  read_index;     /* current read position (block index) */
    /* For address reporting to guest */
    u64                  port_addr;      /* guest-visible buffer address */
    u64                  read_idx_addr;  /* guest-visible read index address */
} AudioPortSlot;

static int            s_audio_initialized = 0;
static u64            s_audio_start_us = 0;      /* us anchor for GetPortTimestamp */
/* Block tags come from one global counter shared by all ports, not from the
 * per-port read_index that restarts at 0 on every PortOpen; otherwise a port
 * opened T us after init would report every block T us in the past and two
 * ports would contradict each other. Ticks once per mixed block (5.33 ms). */
static u64            s_audio_block_counter = 0;
/* Guest "system time" clock hook (runtime/syscalls/sys_timer.h); registered
 * by the runner before main(). GetPortTimestamp must answer in the same
 * domain the guest reads via sys_time_get_system_time; a raw host-uptime
 * anchor would put every stamp-vs-now delta a guest mixer computes off by
 * hours to days. */
extern u64 (*g_lv2_system_time_us)(void);
static AudioPortSlot  s_ports[CELL_AUDIO_PORT_MAX];

/* Event queue notification */
typedef struct {
    int  in_use;
    u64  key;
} AudioNotifySlot;
static AudioNotifySlot s_notify_queues[CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES];

/* Mixing thread */
static volatile int  s_mix_thread_running = 0;
static thread_t      s_mix_thread;
static mutex_t       s_audio_mutex;

/* ---------------------------------------------------------------------------
 * Guest-VM audio buffers + big-endian I/O
 *
 * The audio port ring buffer and read-index must live in guest VM so the game
 * gets real guest EAs it can read/write (host pointers are unusable to the
 * guest). Dedicated region at 0x50000000,
 * outside the sys_memory window (0x40000000-0x50000000) and the stack
 * (0xD0000000+). Committed on first use; bump-allocated, with closed
 * ports' blocks recycled through the free list below.
 * -----------------------------------------------------------------------*/
#define AUDIO_VM_BASE  0x50000000u
#define AUDIO_VM_SIZE  0x00400000u   /* 4 MB */
static u32 s_audio_vm_bump = AUDIO_VM_BASE;
static int s_audio_vm_ready = 0;

/* PortClose returns the port's ring/read-index blocks to this region, so a
 * title that cycles ports (for example per movie) does not exhaust the 4 MB
 * window. Freed blocks are recorded here and reused exact-size-first by
 * audio_vm_alloc (ports come in a handful of fixed sizes: 2ch/8ch x
 * 2/4/8/16/32 blocks + the 128-byte read-index, so exact-fit reclaims the
 * recycled shapes without fragmentation bookkeeping). Callers hold
 * s_audio_mutex on the open/close paths, which serializes access. */
#define AUDIO_VM_FREE_MAX 32
typedef struct { u32 addr; u32 size; } AudioVmFreeBlock;
static AudioVmFreeBlock s_audio_vm_free[AUDIO_VM_FREE_MAX];

static u32 audio_vm_alloc(u32 size)
{
    size = (size + 0x7Fu) & ~0x7Fu;   /* 128-byte align */
    if (!s_audio_vm_ready) {
        if (vm_commit(AUDIO_VM_BASE, AUDIO_VM_SIZE) != CELL_OK)
            return 0;
        s_audio_vm_ready = 1;
    }
    /* Reuse an exact-size freed block before bumping. */
    for (int i = 0; i < AUDIO_VM_FREE_MAX; i++) {
        if (s_audio_vm_free[i].size == size) {
            u32 a = s_audio_vm_free[i].addr;
            s_audio_vm_free[i].addr = 0;
            s_audio_vm_free[i].size = 0;
            return a;
        }
    }
    if ((u64)s_audio_vm_bump + size > (u64)AUDIO_VM_BASE + AUDIO_VM_SIZE)
        return 0;
    u32 a = s_audio_vm_bump;
    s_audio_vm_bump += size;
    return a;
}

static void audio_vm_free(u32 addr, u32 size)
{
    if (!addr) return;
    size = (size + 0x7Fu) & ~0x7Fu;   /* mirror audio_vm_alloc rounding */
    for (int i = 0; i < AUDIO_VM_FREE_MAX; i++) {
        if (s_audio_vm_free[i].size == 0) {
            s_audio_vm_free[i].addr = addr;
            s_audio_vm_free[i].size = size;
            return;
        }
    }
    /* Free table full: drop the block (bounded leak). */
}

/* Write host->guest in big-endian (the guest is PPC BE). */
static void audio_be_w32(u32 ea, u32 v)
{
    unsigned char* p = (unsigned char*)vm_to_host(ea);
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >>  8); p[3] = (unsigned char)v;
}
static void audio_be_w64(u32 ea, u64 v)
{
    audio_be_w32(ea,     (u32)(v >> 32));
    audio_be_w32(ea + 4, (u32)v);
}

/* Read a big-endian 32-bit float sample from the guest port buffer.
 * The guest (PPC, big-endian) writes float PCM into port->buffer; the host
 * mixer must byteswap each sample to native order before use. Reinterpret
 * via memcpy to avoid strict-aliasing UB. */
static inline float rd_be_f32(const float* p)
{
    u32 w;
    memcpy(&w, p, sizeof(w));
    w = ps3_bswap32(w);
    float f;
    memcpy(&f, &w, sizeof(f));
    return f;
}

/* Output mix buffer (stereo, one block worth) */
static float s_mix_buffer[CELL_AUDIO_BLOCK_SAMPLES * 2];


/* Optional host-native movie stream. It is mixed by the same fixed 48 kHz
 * audio heartbeat as guest ports, making the consumed sample count the movie
 * clock. The movie player owns pcm and stops this stream before freeing it. */
typedef struct {
    const int16_t* pcm;
    u64 frame_count;
    u64 cursor;
    int present;
    int finished;
    int mute_guest_ports;
} HostAudioStream;
static HostAudioStream s_host_audio;

/* ---------------------------------------------------------------------------
 * Host audio output backend
 * -----------------------------------------------------------------------*/

static IAudioClient*        s_wasapi_client = NULL;
static IAudioRenderClient*  s_wasapi_render = NULL;
static HANDLE               s_wasapi_event  = NULL;
static UINT32               s_wasapi_buf_frames = 0;

static int audio_backend_init(void)
{
    HRESULT hr;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
        printf("[cellAudio] CoInitializeEx failed: 0x%08lX\n", hr);
        return -1;
    }

    IMMDeviceEnumerator* enumerator = NULL;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void**)&enumerator);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to create device enumerator: 0x%08lX\n", hr);
        return -1;
    }

    IMMDevice* device = NULL;
    hr = enumerator->lpVtbl->GetDefaultAudioEndpoint(enumerator, eRender,
                                                      eConsole, &device);
    enumerator->lpVtbl->Release(enumerator);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to get default audio endpoint: 0x%08lX\n", hr);
        return -1;
    }

    hr = device->lpVtbl->Activate(device, &IID_IAudioClient, CLSCTX_ALL,
                                   NULL, (void**)&s_wasapi_client);
    device->lpVtbl->Release(device);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to activate audio client: 0x%08lX\n", hr);
        return -1;
    }

    WAVEFORMATEX wfx;
    wfx.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = CELL_AUDIO_SAMPLE_RATE;
    wfx.wBitsPerSample  = 32;
    wfx.nBlockAlign     = wfx.nChannels * (wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize          = 0;

    /* Request event-driven mode with a ~20ms buffer */
    REFERENCE_TIME buf_duration = 200000; /* 20ms in 100ns units */
    s_wasapi_event = CreateEventW(NULL, FALSE, FALSE, NULL);

    hr = s_wasapi_client->lpVtbl->Initialize(
        s_wasapi_client,
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        buf_duration, 0, &wfx, NULL);

    if (FAILED(hr)) {
        /* Fallback: try without event callback */
        hr = s_wasapi_client->lpVtbl->Initialize(
            s_wasapi_client,
            AUDCLNT_SHAREMODE_SHARED,
            0, buf_duration, 0, &wfx, NULL);
        if (FAILED(hr)) {
            printf("[cellAudio] WASAPI Initialize failed: 0x%08lX\n", hr);
            s_wasapi_client->lpVtbl->Release(s_wasapi_client);
            s_wasapi_client = NULL;
            CloseHandle(s_wasapi_event);
            s_wasapi_event = NULL;
            return -1;
        }
        CloseHandle(s_wasapi_event);
        s_wasapi_event = NULL;
    } else {
        s_wasapi_client->lpVtbl->SetEventHandle(s_wasapi_client, s_wasapi_event);
    }

    s_wasapi_client->lpVtbl->GetBufferSize(s_wasapi_client, &s_wasapi_buf_frames);

    hr = s_wasapi_client->lpVtbl->GetService(
        s_wasapi_client, &IID_IAudioRenderClient, (void**)&s_wasapi_render);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to get render client: 0x%08lX\n", hr);
        s_wasapi_client->lpVtbl->Release(s_wasapi_client);
        s_wasapi_client = NULL;
        return -1;
    }

    hr = s_wasapi_client->lpVtbl->Start(s_wasapi_client);
    if (FAILED(hr)) {
        printf("[cellAudio] WASAPI Start failed: 0x%08lX\n", hr);
        return -1;
    }
    return 0;
}

static void audio_backend_shutdown(void)
{
    if (s_wasapi_client) {
        s_wasapi_client->lpVtbl->Stop(s_wasapi_client);
    }
    if (s_wasapi_render) {
        s_wasapi_render->lpVtbl->Release(s_wasapi_render);
        s_wasapi_render = NULL;
    }
    if (s_wasapi_client) {
        s_wasapi_client->lpVtbl->Release(s_wasapi_client);
        s_wasapi_client = NULL;
    }
    if (s_wasapi_event) {
        CloseHandle(s_wasapi_event);
        s_wasapi_event = NULL;
    }
}

static void audio_backend_submit(const float* stereo_samples, u32 num_samples)
{
    if (!s_wasapi_render) return;

    UINT32 padding = 0;
    HRESULT hr = s_wasapi_client->lpVtbl->GetCurrentPadding(
        s_wasapi_client, &padding);
    if (FAILED(hr))
        return;

    UINT32 available = s_wasapi_buf_frames - padding;
    if (num_samples > available)
        num_samples = available;

    if (num_samples == 0) return;

    BYTE* buf = NULL;
    hr = s_wasapi_render->lpVtbl->GetBuffer(
        s_wasapi_render, num_samples, &buf);
    if (SUCCEEDED(hr) && buf) {
        memcpy(buf, stereo_samples, num_samples * 2 * sizeof(float));
        s_wasapi_render->lpVtbl->ReleaseBuffer(
            s_wasapi_render, num_samples, 0);
    }
}

/* ---------------------------------------------------------------------------
 * Mixing
 * -----------------------------------------------------------------------*/

/* Mix one block from all active ports into s_mix_buffer (stereo float) */
static void audio_mix_one_block(void)
{
    memset(s_mix_buffer, 0, sizeof(s_mix_buffer));

    mutex_lock(&s_audio_mutex);

    /* One global block tick per mixed period (all ports advance together;
     * update notifications are issued at the same time for all ports).
     * Tag/timestamp queries key off this counter. */
    s_audio_block_counter++;

    const int mix_guest_ports =
        !(s_host_audio.present && s_host_audio.mute_guest_ports);
    for (int p = 0; p < CELL_AUDIO_PORT_MAX; p++) {
        AudioPortSlot* port = &s_ports[p];
        if (!port->in_use || !port->running || !port->buffer)
            continue;

        u32 nch    = (u32)port->param.nChannel;
        u32 nblock = (u32)port->param.nBlock;
        /* cellAudioPortOpen resolves the port's level once (honoring
         * CELL_AUDIO_PORTATTR_INITLEVEL), and an intentional level=0 (mute)
         * stays muted through the mixer. */
        float level = port->param.level;
        if (level > 1.0f) level = 1.0f;
        if (level < 0.0f) level = 0.0f;

        /* Read one block at the current read_index */
        u32 block_idx = (u32)(port->read_index % nblock);
        u32 block_offset = block_idx * CELL_AUDIO_BLOCK_SAMPLES * nch;
        float* src = port->buffer + block_offset;


        for (u32 s = 0; s < CELL_AUDIO_BLOCK_SAMPLES; s++) {
            float left  = rd_be_f32(&src[s * nch + 0]) * level;
            float right = rd_be_f32(&src[s * nch + 1]) * level;

            /* If 7.1, mix center and other channels into stereo */
            if (nch == 8) {
                float center = rd_be_f32(&src[s * 8 + 2]) * level * 0.707f;
                float lfe    = rd_be_f32(&src[s * 8 + 3]) * level * 0.5f;
                float rl     = rd_be_f32(&src[s * 8 + 4]) * level * 0.5f;
                float rr     = rd_be_f32(&src[s * 8 + 5]) * level * 0.5f;
                float sl     = rd_be_f32(&src[s * 8 + 6]) * level * 0.3f;
                float sr     = rd_be_f32(&src[s * 8 + 7]) * level * 0.3f;
                left  += center + lfe + rl + sl;
                right += center + lfe + rr + sr;
            }

            if (mix_guest_ports) {
                s_mix_buffer[s * 2 + 0] += left;
                s_mix_buffer[s * 2 + 1] += right;
            }
        }

        /* Zero the consumed block: the audio server clears the blocks it has
         * finished reading. Besides fidelity (a stalled producer must go
         * silent, not loop stale PCM), block-clearing is a documented
         * consumption signal a producer can poll. */
        memset(src, 0, CELL_AUDIO_BLOCK_SAMPLES * nch * sizeof(float));

        /* Advance read index + publish the block index to the guest (BE u64 at
         * read_idx_addr, 0..nBlock-1) so the game's audio loop sees blocks being
         * consumed and keeps feeding the next one. */
        port->read_index++;
        if (port->read_idx_addr)
            audio_be_w64((u32)port->read_idx_addr, port->read_index % nblock);
    }

    if (s_host_audio.present) {
        for (u32 s = 0; s < CELL_AUDIO_BLOCK_SAMPLES; s++) {
            if (s_host_audio.cursor >= s_host_audio.frame_count) {
                s_host_audio.finished = 1;
                break;
            }
            const int16_t* src = s_host_audio.pcm +
                s_host_audio.cursor * 2;
            s_mix_buffer[s * 2 + 0] += (float)src[0] / 32768.0f;
            s_mix_buffer[s * 2 + 1] += (float)src[1] / 32768.0f;
            s_host_audio.cursor++;
        }
    }

    mutex_unlock(&s_audio_mutex);

    /* Clip to [-1.0, 1.0] */
    for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2; i++) {
        if (s_mix_buffer[i] > 1.0f) s_mix_buffer[i] = 1.0f;
        if (s_mix_buffer[i] < -1.0f) s_mix_buffer[i] = -1.0f;
    }

}

/* ---------------------------------------------------------------------------
 * Mixing thread
 * -----------------------------------------------------------------------*/

static void audio_notify_event_queues(void)
{
    /* Push the audio-period event (data1 = CELL_AUDIO_EVENT_MIX = 0) to each
     * registered notify queue, so the game's audio loop (blocked on
     * sys_event_queue_receive) wakes once per block and writes the next one.
     * Resolve the queue by its ipc_key (set via cellAudioSetNotifyEventQueue). */
    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) continue;
        unsigned int qid = sys_event_find_queue_by_key(s_notify_queues[i].key);
        if (qid)
            sys_event_queue_push_by_id(qid, s_notify_queues[i].key,
                                       0 /*CELL_AUDIO_EVENT_MIX*/, 0, 0);
    }
}

/* ---------------------------------------------------------------------------
 * Guest block-progression pacing
 *
 * The console's audio server runs a fixed hardware heartbeat: one block is
 * exactly 256 samples / 5.3 ms (5 1/3 ms), and both the ring-buffer
 * read-index advance and the per-block notify event fire on that fixed
 * cadence, independent of how the host sink is draining. Guest-visible
 * progress therefore never depends on host backend buffer occupancy.
 *
 * Pacing math: the block period (16000/3 us) is not an integer, so rather
 * than accumulate a per-iteration sleep duration (which would accumulate
 * rounding error), every wait target is an absolute deadline computed fresh
 * from a thread-start anchor and the monotonic block sequence number via
 * 64-bit integer division: deadline = anchor + n * period. Truncation of
 * that division alternates by at most one low-order tick and cancels out
 * exactly every 3 blocks (== 16 ms, an exact integer), so there is no
 * running remainder to leak drift into over an arbitrarily long run. */

static HANDLE          s_pace_timer;
static ULARGE_INTEGER  s_pace_anchor;      /* FILETIME 100ns units, thread-start */
static u64             s_pace_block_seq;

static void audio_pace_reset(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    s_pace_anchor.LowPart  = ft.dwLowDateTime;
    s_pace_anchor.HighPart = ft.dwHighDateTime;
    s_pace_block_seq = 1;   /* block 0 already ran (right at the anchor) */
    if (!s_pace_timer)
        s_pace_timer = CreateWaitableTimerW(NULL, FALSE, NULL);
}

/* Block until the next fixed-cadence deadline, then advance the sequence. */
static void audio_pace_wait_block(void)
{
    /* period, in 100ns units: 160000/3 (== 5.3333... ms). Integer division
     * against the running sequence number, not an accumulator. */
    ULONGLONG target = s_pace_anchor.QuadPart + (s_pace_block_seq * 160000ULL) / 3ULL;
    s_pace_block_seq++;

    if (s_pace_timer) {
        LARGE_INTEGER due;
        due.QuadPart = (LONGLONG)target;   /* positive => absolute FILETIME */
        if (SetWaitableTimer(s_pace_timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(s_pace_timer, INFINITE);
            return;
        }
    }

    /* Fallback if timer creation/arming failed: poll the same absolute
     * clock. Still zero-drift (deadline is unaffected), just coarser wake
     * granularity. */
    for (;;) {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER now;
        now.LowPart = ft.dwLowDateTime; now.HighPart = ft.dwHighDateTime;
        if (now.QuadPart >= target) break;
        Sleep(1);
    }
}

static unsigned __stdcall audio_mix_thread_func(void* arg)
{
    (void)arg;
    audio_pace_reset();

    while (s_mix_thread_running) {
        /* Mix and submit one block. audio_backend_submit() still clips to
         * whatever the host WASAPI ring can currently accept (occupancy
         * governs the host sink only, not guest block progression below). */
        audio_mix_one_block();
        audio_backend_submit(s_mix_buffer, CELL_AUDIO_BLOCK_SAMPLES);

        /* Notify event queues */
        audio_notify_event_queues();

        audio_pace_wait_block();
    }

    return 0;
}

static int audio_start_mix_thread(void)
{
    s_mix_thread_running = 1;

    s_mix_thread = (HANDLE)_beginthreadex(NULL, 0, audio_mix_thread_func,
                                           NULL, 0, NULL);
    if (!s_mix_thread) {
        s_mix_thread_running = 0;
        return -1;
    }
    return 0;
}

static void audio_stop_mix_thread(void)
{
    s_mix_thread_running = 0;
    if (s_mix_thread) {
        WaitForSingleObject(s_mix_thread, 2000);
        CloseHandle(s_mix_thread);
        s_mix_thread = NULL;
    }
    if (s_pace_timer) {
        CloseHandle(s_pace_timer);
        s_pace_timer = NULL;
    }
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellAudioInit(void)
{
    if (s_audio_initialized)
        return CELL_AUDIO_ERROR_ALREADY_INIT;

    memset(s_ports, 0, sizeof(s_ports));
    memset(s_notify_queues, 0, sizeof(s_notify_queues));
    mutex_init(&s_audio_mutex);

    if (audio_backend_init() < 0) {
        printf("[cellAudio] WARNING: Audio backend init failed, continuing silently\n");
        /* Don't fail -- games should still run without audio */
    }

    if (audio_start_mix_thread() < 0) {
        printf("[cellAudio] WARNING: Could not start mixing thread\n");
    }

    if (g_lv2_system_time_us) {
        /* Guest boot-anchored domain (see the hook comment above). */
        s_audio_start_us = g_lv2_system_time_us();
    } else {
        LARGE_INTEGER f, c;
        QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
        s_audio_start_us = (u64)((c.QuadPart * 1000000LL) / f.QuadPart);
    }
    s_audio_block_counter = 0;
    s_audio_initialized = 1;
    return CELL_OK;
}

s32 cellAudioQuit(void)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    audio_stop_mix_thread();
    audio_backend_shutdown();
    memset(&s_host_audio, 0, sizeof(s_host_audio));

    /* Free all port buffers */
    for (int i = 0; i < CELL_AUDIO_PORT_MAX; i++) {
        s_ports[i].buffer = NULL;   /* guest-VM bump region; not malloc'd */
        s_ports[i].in_use = 0;
        s_ports[i].running = 0;
    }

    mutex_destroy(&s_audio_mutex);
    s_audio_initialized = 0;
    return CELL_OK;
}

int cellAudioHostStreamStart(const int16_t* pcm, uint64_t frames,
                             int mute_guest_ports)
{
    if (!s_audio_initialized || !pcm || !frames)
        return -1;
    mutex_lock(&s_audio_mutex);
    s_host_audio.pcm = pcm;
    s_host_audio.frame_count = frames;
    s_host_audio.cursor = 0;
    s_host_audio.present = 1;
    s_host_audio.finished = 0;
    s_host_audio.mute_guest_ports = mute_guest_ports ? 1 : 0;
    mutex_unlock(&s_audio_mutex);
    return 0;
}

void cellAudioHostStreamStop(void)
{
    if (!s_audio_initialized)
        return;
    mutex_lock(&s_audio_mutex);
    memset(&s_host_audio, 0, sizeof(s_host_audio));
    mutex_unlock(&s_audio_mutex);
}

uint64_t cellAudioHostStreamPositionFrames(void)
{
    if (!s_audio_initialized)
        return 0;
    mutex_lock(&s_audio_mutex);
    u64 position = s_host_audio.cursor;
    mutex_unlock(&s_audio_mutex);
    return position;
}

int cellAudioHostStreamFinished(void)
{
    if (!s_audio_initialized)
        return 0;
    mutex_lock(&s_audio_mutex);
    int finished = s_host_audio.present && s_host_audio.finished;
    mutex_unlock(&s_audio_mutex);
    return finished;
}

/* Guest structs are big-endian (PPC); the import bridge hands us a host pointer
 * into guest VM, so multi-byte fields must be byteswapped to host order before
 * use (a raw nChannel of 8 reads as 0x0800000000000000 and fails validation). */

s32 cellAudioPortOpen(const CellAudioPortParam* param, u32* portNum)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (!param || !portNum)
        return CELL_AUDIO_ERROR_PARAM;

    /* Byteswap the big-endian guest param into a host-native copy. */
    CellAudioPortParam hp;
    hp.nChannel = ps3_bswap64(param->nChannel);
    hp.nBlock   = ps3_bswap64(param->nBlock);
    hp.attr     = ps3_bswap64(param->attr);
    { union { u32 u; float f; } lv; lv.u = ps3_bswap32(*(const u32*)&param->level); hp.level = lv.f; }

    /* Validate parameters */
    u64 nch = hp.nChannel;
    u64 nblk = hp.nBlock;
    if (nch != CELL_AUDIO_PORT_2CH && nch != CELL_AUDIO_PORT_8CH)
        return CELL_AUDIO_ERROR_PARAM;
    /* Firmware accepts nBlock of exactly {2,4,8,16,32}; 2 and 4 are used by
     * CRI/libmixer ports, so rejecting them would kill audio at init.
     * Anything else is CELL_AUDIO_ERROR_PARAM. */
    if (nblk != 2 && nblk != 4 &&
        nblk != CELL_AUDIO_BLOCK_8 && nblk != CELL_AUDIO_BLOCK_16 &&
        nblk != CELL_AUDIO_BLOCK_32)
        return CELL_AUDIO_ERROR_PARAM;

    /* Only honor the game's requested level when
     * CELL_AUDIO_PORTATTR_INITLEVEL is set; otherwise default to 1.0f.
     * A negative level is applied as its absolute value, not as silence
     * (hardware plays at |level|). The >1.0 clamp is our own safety margin:
     * levels above 1.0 are allowed but degrade quality. */
    {
        float resolved_level = 1.0f;
        if (hp.attr & CELL_AUDIO_PORTATTR_INITLEVEL) {
            resolved_level = hp.level;
            if (resolved_level < 0.0f) resolved_level = -resolved_level;
            if (resolved_level > 1.0f) resolved_level = 1.0f;
        }
        hp.level = resolved_level;
    }

    mutex_lock(&s_audio_mutex);

    /* Find a free port slot */
    s32 found = -1;
    for (u32 i = 0; i < CELL_AUDIO_PORT_MAX; i++) {
        if (!s_ports[i].in_use) {
            found = (s32)i;
            break;
        }
    }

    if (found < 0) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PORT_FULL;
    }

    AudioPortSlot* port = &s_ports[found];
    port->in_use  = 1;
    port->running = 0;
    port->param   = hp;          /* host-native (byteswapped) copy */
    port->read_index  = 0;

    /* Allocate the audio ring buffer + read-index in GUEST VM, so the game can
     * read/write them via guest EAs (cellAudioGetPortConfig hands these back). */
    u32 buf_samples = (u32)(nblk * CELL_AUDIO_BLOCK_SAMPLES * nch);
    port->buf_size  = buf_samples * (u32)sizeof(float);
    u32 g_buf  = audio_vm_alloc(port->buf_size);
    u32 g_ridx = audio_vm_alloc(16);   /* read-index counter (u64) */

    if (!g_buf || !g_ridx) {
        port->in_use = 0;
        mutex_unlock(&s_audio_mutex);
        return CELL_ENOMEM;
    }

    port->buffer        = (float*)vm_to_host(g_buf);  /* host view for the mixer */
    port->port_addr     = (u64)g_buf;                 /* guest EA for the game    */
    port->read_idx_addr = (u64)g_ridx;                /* guest EA: read index     */
    audio_be_w64(g_ridx, 0);                          /* read index starts at 0   */

    /* portNum is a host pointer aliasing guest BE memory, so write big-endian. */
    { unsigned char* pn = (unsigned char*)portNum;
      pn[0]=(unsigned char)(found>>24); pn[1]=(unsigned char)(found>>16);
      pn[2]=(unsigned char)(found>>8);  pn[3]=(unsigned char)found; }

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortClose(u32 portNum)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    mutex_lock(&s_audio_mutex);

    if (!s_ports[portNum].in_use) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    }

    /* Return the port's guest-VM blocks (ring buffer + read-index) to the
     * audio VM free list so reopened ports reuse them instead of exhausting
     * the 4 MB window (see audio_vm_free above). Real firmware releases port
     * memory on close. */
    audio_vm_free((u32)s_ports[portNum].port_addr, s_ports[portNum].buf_size);
    audio_vm_free((u32)s_ports[portNum].read_idx_addr, 16);
    s_ports[portNum].port_addr     = 0;
    s_ports[portNum].read_idx_addr = 0;
    s_ports[portNum].buffer  = NULL;
    s_ports[portNum].in_use  = 0;
    s_ports[portNum].running = 0;

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortStart(u32 portNum)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (s_ports[portNum].running)
        return CELL_AUDIO_ERROR_PORT_ALREADY_RUN;

    mutex_lock(&s_audio_mutex);
    s_ports[portNum].running = 1;
    mutex_unlock(&s_audio_mutex);

    return CELL_OK;
}

s32 cellAudioPortStop(u32 portNum)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (!s_ports[portNum].running)
        return CELL_AUDIO_ERROR_PORT_NOT_RUN;

    mutex_lock(&s_audio_mutex);
    s_ports[portNum].running = 0;
    mutex_unlock(&s_audio_mutex);

    return CELL_OK;
}

s32 cellAudioSetNotifyEventQueue(u64 key)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    /* The key must resolve to an existing event queue and must not already
     * be registered; both cases are CELL_AUDIO_ERROR_TRANS_EVENT. Titles
     * poll SetNotifyEventQueue until the surmixer queue appears, so answering
     * CELL_OK for a nonexistent queue would break that retry loop, and a
     * duplicate registration would double the 5.3 ms event rate into one
     * queue (overrun). */
    if (!sys_event_find_queue_by_key(key))
        return CELL_AUDIO_ERROR_TRANS_EVENT;

    mutex_lock(&s_audio_mutex);

    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (s_notify_queues[i].in_use && s_notify_queues[i].key == key) {
            mutex_unlock(&s_audio_mutex);
            return CELL_AUDIO_ERROR_TRANS_EVENT; /* already registered */
        }
    }

    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) {
            s_notify_queues[i].in_use = 1;
            s_notify_queues[i].key = key;
            mutex_unlock(&s_audio_mutex);
            return CELL_OK;
        }
    }

    mutex_unlock(&s_audio_mutex);
    return CELL_AUDIO_ERROR_TRANS_EVENT; /* table full */
}

s32 cellAudioRemoveNotifyEventQueue(u64 key)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    mutex_lock(&s_audio_mutex);

    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (s_notify_queues[i].in_use && s_notify_queues[i].key == key) {
            s_notify_queues[i].in_use = 0;
            mutex_unlock(&s_audio_mutex);
            return CELL_OK;
        }
    }

    mutex_unlock(&s_audio_mutex);
    /* Unknown key: TRANS_EVENT, not PARAM. */
    return CELL_AUDIO_ERROR_TRANS_EVENT;
}

s32 cellAudioGetPortConfig(u32 portNum, CellAudioPortConfig* config)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    /* Validation order: a NULL config or out-of-range portNum is
     * CELL_AUDIO_ERROR_PARAM (checked first). A valid but never-opened or
     * closed port is not an error; it reports CELL_OK with
     * status = CELL_AUDIO_STATUS_CLOSE (0x1010), so a status poll can observe
     * the CLOSE state. */
    if (!config || portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PARAM;

    if (!s_ports[portNum].in_use) {
        unsigned char* cc = (unsigned char*)config;
        memset(cc, 0, 32);
        u32 v = (u32)CELL_AUDIO_STATUS_CLOSE;
        cc[4] = (unsigned char)(v >> 24); cc[5] = (unsigned char)(v >> 16);
        cc[6] = (unsigned char)(v >> 8);  cc[7] = (unsigned char)v;
        return CELL_OK;
    }

    mutex_lock(&s_audio_mutex);

    AudioPortSlot* port = &s_ports[portNum];

    /* Write the guest CellAudioPortConfig (32 bytes, big-endian) at its real PS3
     * offsets: readIndexAddr@0(u32) status@4(u32)
     * nChannel@8(u64) nBlock@16(u64) portSize@24(u32) portAddr@28(u32).
     * (The host struct layout differs; write the guest bytes explicitly.) */
    unsigned char* c = (unsigned char*)config;
    memset(c, 0, 32);
    u32 status = port->running ? (u32)CELL_AUDIO_STATUS_RUN
                               : (u32)CELL_AUDIO_STATUS_READY;
    #define WBE32(o, val) do { u32 v_ = (u32)(val); \
        c[(o)+0]=(unsigned char)(v_>>24); c[(o)+1]=(unsigned char)(v_>>16); \
        c[(o)+2]=(unsigned char)(v_>>8);  c[(o)+3]=(unsigned char)v_; } while(0)
    WBE32(0,  (u32)port->read_idx_addr);
    WBE32(4,  status);
    WBE32(8,  (u32)(port->param.nChannel >> 32)); WBE32(12, (u32)port->param.nChannel);
    WBE32(16, (u32)(port->param.nBlock   >> 32)); WBE32(20, (u32)port->param.nBlock);
    WBE32(24, port->buf_size);
    WBE32(28, (u32)port->port_addr);
    #undef WBE32

    mutex_unlock(&s_audio_mutex);

    return CELL_OK;
}

s32 cellAudioPortGetStatus(u32 portNum, u32* status)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !status)
        return CELL_AUDIO_ERROR_PARAM;

    {   /* out-param is a raw guest pointer: write guest-BE */
        u32 v = !s_ports[portNum].in_use ? (u32)CELL_AUDIO_STATUS_CLOSE
              : s_ports[portNum].running ? (u32)CELL_AUDIO_STATUS_RUN
                                         : (u32)CELL_AUDIO_STATUS_READY;
        unsigned char* c = (unsigned char*)status;
        c[0] = (unsigned char)(v >> 24); c[1] = (unsigned char)(v >> 16);
        c[2] = (unsigned char)(v >> 8);  c[3] = (unsigned char)v;
    }

    return CELL_OK;
}

/* Block tag of slot blockNo: tag = global_counter + blockNo - cur_pos, where
 * read_index % nBlock is the current slot. libmixer's sur-mixer pump paces
 * itself with this + GetPortTimestamp. Out-param written guest-BE (guest is
 * big-endian; every HLE write of a multi-byte out-param must match). */
s32 cellAudioGetPortBlockTag(u32 portNum, u64 blockNo, u64* tag)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PARAM;

    if (!s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (!tag)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);
    AudioPortSlot* port = &s_ports[portNum];
    u64 nblk = port->param.nBlock ? port->param.nBlock : 1;
    if (blockNo >= nblk) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PARAM;
    }
    /* Global-counter form: while running, read_index and the global counter
     * advance in lockstep, so this stays consistent per-port while giving
     * all ports one shared tag timeline. */
    u64 t = s_audio_block_counter + blockNo - (port->read_index % nblk);
    mutex_unlock(&s_audio_mutex);

    unsigned char* c = (unsigned char*)tag;
    for (int i = 0; i < 8; i++)
        c[i] = (unsigned char)(t >> (56 - i * 8));
    return CELL_OK;
}

/* Microsecond timestamp at which block `tag` was mixed: start_time +
 * tag * 256 * 1000000 / 48000. Anchored to the mixer's own block clock:
 * one block = 256 samples @48kHz = 16000/3 us. */
s32 cellAudioGetPortTimestamp(u32 portNum, u64 tag, u64* stamp)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PARAM;

    if (!s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (!stamp)
        return CELL_AUDIO_ERROR_PARAM;

    /* A tag the mixer has not reached yet is CELL_AUDIO_ERROR_TAG_NOT_FOUND.
     * Tags come from the shared s_audio_block_counter (see GetPortBlockTag),
     * so compare against that; the stamp below is anchored on the guest
     * system-time clock. */
    mutex_lock(&s_audio_mutex);
    u64 global_counter = s_audio_block_counter;
    mutex_unlock(&s_audio_mutex);
    if (tag > global_counter)
        return CELL_AUDIO_ERROR_TAG_NOT_FOUND;

    u64 t = s_audio_start_us + tag * 256000000ULL / 48000ULL;
    unsigned char* c = (unsigned char*)stamp;
    for (int i = 0; i < 8; i++)
        c[i] = (unsigned char)(t >> (56 - i * 8));
    return CELL_OK;
}

s32 cellAudioSetPersonalDevice(s32 iPersonalStream, s32 iDevice)
{
    (void)iPersonalStream;
    (void)iDevice;
    return CELL_OK;
}

s32 cellAudioUnsetPersonalDevice(s32 iPersonalStream)
{
    (void)iPersonalStream;
    return CELL_OK;
}
