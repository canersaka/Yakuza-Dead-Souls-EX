/*
 * ps3recomp - cellAudio HLE implementation
 *
 * Real audio mixing and output. A background mixing thread reads audio data
 * from each active port's buffer in guest memory, mixes them, and outputs
 * to the host audio device.
 *
 * Backend selection:
 *   - Windows default: WASAPI (via mmdeviceapi)
 *   - Everywhere else / if PS3RECOMP_AUDIO_USE_SDL2 defined: SDL2 audio
 *
 * Define PS3RECOMP_AUDIO_USE_SDL2 to force SDL2 backend on Windows.
 */

#include "cellAudio.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "../../runtime/memory/vm.h"   /* vm_commit, vm_to_host, vm_base */

/* sys_event helpers (C-linkage; defined in runtime/syscalls/sys_event.c).
 * Declared directly to avoid pulling sys_event.h's C++ guards into this C file. */
extern int          sys_event_queue_push_by_id(unsigned int qid, unsigned long long src,
                                               unsigned long long d1, unsigned long long d2,
                                               unsigned long long d3);
extern unsigned int sys_event_find_queue_by_key(unsigned long long key);

/* ---------------------------------------------------------------------------
 * Backend selection
 * -----------------------------------------------------------------------*/

#if defined(PS3RECOMP_AUDIO_USE_SDL2)
  #define AUDIO_BACKEND_SDL2    1
  #define AUDIO_BACKEND_WASAPI  0
#elif defined(_WIN32)
  #define AUDIO_BACKEND_SDL2    0
  #define AUDIO_BACKEND_WASAPI  1
#else
  #define AUDIO_BACKEND_SDL2    1
  #define AUDIO_BACKEND_WASAPI  0
#endif

#if AUDIO_BACKEND_WASAPI
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <mmdeviceapi.h>
  #include <audioclient.h>
  #include <process.h>

  /* Define WASAPI COM GUIDs (avoids needing uuid.lib linkage for these) */
  #ifdef __cplusplus
    #define GUID_SECT
  #else
    #define GUID_SECT
  #endif
  #ifndef DEFINE_AUDIO_GUID
    #ifdef INITGUID
      #define DEFINE_AUDIO_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
              const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
    #else
      #define DEFINE_AUDIO_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
              const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
    #endif
  #endif
  DEFINE_AUDIO_GUID(ps3r_CLSID_MMDeviceEnumerator, 0xBCDE0395,0xE52F,0x467C,0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E);
  DEFINE_AUDIO_GUID(ps3r_IID_IMMDeviceEnumerator,  0xA95664D2,0x9614,0x4F35,0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6);
  DEFINE_AUDIO_GUID(ps3r_IID_IAudioClient,         0x1CB9AD4C,0xDBFA,0x4c32,0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2);
  DEFINE_AUDIO_GUID(ps3r_IID_IAudioRenderClient,   0xF294ACFC,0x3146,0x4483,0xA7,0xBF,0xAD,0xDC,0xA7,0xC2,0x60,0xE2);

  /* Map to the names used in the code */
  #define IID_IMMDeviceEnumerator  ps3r_IID_IMMDeviceEnumerator
  #define CLSID_MMDeviceEnumerator ps3r_CLSID_MMDeviceEnumerator
  #define IID_IAudioClient         ps3r_IID_IAudioClient
  #define IID_IAudioRenderClient   ps3r_IID_IAudioRenderClient
#endif

#if AUDIO_BACKEND_SDL2
  #include <SDL2/SDL.h>
#endif

/* Portable threading */
#ifdef _WIN32
  #include <windows.h>
  typedef HANDLE thread_t;
  typedef CRITICAL_SECTION mutex_t;
  #define mutex_init(m)    InitializeCriticalSection(m)
  #define mutex_destroy(m) DeleteCriticalSection(m)
  #define mutex_lock(m)    EnterCriticalSection(m)
  #define mutex_unlock(m)  LeaveCriticalSection(m)
#else
  #include <pthread.h>
  #include <unistd.h>
  typedef pthread_t thread_t;
  typedef pthread_mutex_t mutex_t;
  #define mutex_init(m)    pthread_mutex_init(m, NULL)
  #define mutex_destroy(m) pthread_mutex_destroy(m)
  #define mutex_lock(m)    pthread_mutex_lock(m)
  #define mutex_unlock(m)  pthread_mutex_unlock(m)
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

/* Per-port audio buffer allocated on the host side.
 * In a full emulator these would be in guest VM memory; here we allocate
 * host buffers and expose their addresses through portAddr/readIndexAddr. */
#define AUDIO_PORT_BUF_MAX  (CELL_AUDIO_BLOCK_32 * CELL_AUDIO_BLOCK_SAMPLES * CELL_AUDIO_PORT_8CH)

typedef struct {
    int                  in_use;
    int                  running;
    CellAudioPortParam   param;
    float*               buffer;        /* host audio buffer (float samples) */
    u32                  buf_size;       /* buffer size in bytes */
    u64                  read_index;     /* current read position (block index) */
    u64                  write_index;    /* where the game is writing */
    /* For address reporting to guest */
    u64                  port_addr;      /* guest-visible buffer address */
    u64                  read_idx_addr;  /* guest-visible read index address */
} AudioPortSlot;

static int            s_audio_initialized = 0;
static u64            s_audio_start_us = 0;      /* us anchor for GetPortTimestamp */
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
 * Guest-VM audio buffers + big-endian I/O (2026-06-21)
 *
 * The audio port ring buffer and read-index must live in GUEST VM so the game
 * gets real guest EAs it can read/write (the old code handed out host calloc
 * pointers -> the game couldn't use them). Dedicated region at 0x50000000,
 * outside the sys_memory window (0x40000000-0x50000000) and the stack
 * (0xD0000000+). Committed on first use; bump-allocated (audio buffers are
 * never individually freed during a session).
 * -----------------------------------------------------------------------*/
#define AUDIO_VM_BASE  0x50000000u
#define AUDIO_VM_SIZE  0x00400000u   /* 4 MB */
static u32 s_audio_vm_bump = AUDIO_VM_BASE;
static int s_audio_vm_ready = 0;

static u32 audio_vm_alloc(u32 size)
{
    size = (size + 0x7Fu) & ~0x7Fu;   /* 128-byte align */
    if (!s_audio_vm_ready) {
        if (vm_commit(AUDIO_VM_BASE, AUDIO_VM_SIZE) != CELL_OK)
            return 0;
        s_audio_vm_ready = 1;
    }
    if ((u64)s_audio_vm_bump + size > (u64)AUDIO_VM_BASE + AUDIO_VM_SIZE)
        return 0;
    u32 a = s_audio_vm_bump;
    s_audio_vm_bump += size;
    return a;
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

/* Big-endian scalar helpers (guest is PPC BE). Also used by cellAudioPortOpen
 * to byteswap the guest CellAudioPortParam into host-native order. */
static inline u64 ca_be64(u64 v) {
#if defined(_MSC_VER)
    return _byteswap_uint64(v);
#else
    return __builtin_bswap64(v);
#endif
}
static inline u32 ca_be32(u32 v) {
#if defined(_MSC_VER)
    return _byteswap_ulong(v);
#else
    return __builtin_bswap32(v);
#endif
}

/* Read a big-endian 32-bit float sample from the guest port buffer.
 * The guest (PPC, big-endian) writes float PCM into port->buffer; the host
 * mixer must byteswap each sample to native order before use (RPCS3 does the
 * same: its port buffer is typed be_t<f32>*, auto-swapped on read). Reinterpret
 * via memcpy to avoid strict-aliasing UB. */
static inline float rd_be_f32(const float* p)
{
    u32 w;
    memcpy(&w, p, sizeof(w));
    w = ca_be32(w);
    float f;
    memcpy(&f, &w, sizeof(f));
    return f;
}

/* Output mix buffer (stereo, one block worth) */
static float s_mix_buffer[CELL_AUDIO_BLOCK_SAMPLES * 2];

/* ---------------------------------------------------------------------------
 * Host audio output backend
 * -----------------------------------------------------------------------*/

#if AUDIO_BACKEND_SDL2

static SDL_AudioDeviceID s_sdl_audio_dev = 0;

static int audio_backend_init(void)
{
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            printf("[cellAudio] SDL audio init failed: %s\n", SDL_GetError());
            return -1;
        }
    }

    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq     = CELL_AUDIO_SAMPLE_RATE;
    want.format   = AUDIO_F32SYS;
    want.channels = 2;
    want.samples  = CELL_AUDIO_BLOCK_SAMPLES;
    want.callback = NULL; /* We'll use SDL_QueueAudio */

    s_sdl_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (s_sdl_audio_dev == 0) {
        printf("[cellAudio] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return -1;
    }

    SDL_PauseAudioDevice(s_sdl_audio_dev, 0); /* Start playback */
    return 0;
}

static void audio_backend_shutdown(void)
{
    if (s_sdl_audio_dev) {
        SDL_CloseAudioDevice(s_sdl_audio_dev);
        s_sdl_audio_dev = 0;
    }
}

static void audio_backend_submit(const float* stereo_samples, u32 num_samples)
{
    if (s_sdl_audio_dev) {
        SDL_QueueAudio(s_sdl_audio_dev, stereo_samples,
                       num_samples * 2 * sizeof(float));
    }
}

static u32 audio_backend_queued_samples(void)
{
    if (s_sdl_audio_dev) {
        return SDL_GetQueuedAudioSize(s_sdl_audio_dev) / (2 * sizeof(float));
    }
    return 0;
}

#endif /* AUDIO_BACKEND_SDL2 */

#if AUDIO_BACKEND_WASAPI

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

    s_wasapi_client->lpVtbl->Start(s_wasapi_client);
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
    s_wasapi_client->lpVtbl->GetCurrentPadding(s_wasapi_client, &padding);

    UINT32 available = s_wasapi_buf_frames - padding;
    if (num_samples > available)
        num_samples = available;

    if (num_samples == 0) return;

    BYTE* buf = NULL;
    HRESULT hr = s_wasapi_render->lpVtbl->GetBuffer(s_wasapi_render, num_samples, &buf);
    if (SUCCEEDED(hr) && buf) {
        memcpy(buf, stereo_samples, num_samples * 2 * sizeof(float));
        s_wasapi_render->lpVtbl->ReleaseBuffer(s_wasapi_render, num_samples, 0);
    }
}

static u32 audio_backend_queued_samples(void)
{
    if (!s_wasapi_client) return 0;
    UINT32 padding = 0;
    s_wasapi_client->lpVtbl->GetCurrentPadding(s_wasapi_client, &padding);
    return padding;
}

#endif /* AUDIO_BACKEND_WASAPI */

/* ---------------------------------------------------------------------------
 * Mixing
 * -----------------------------------------------------------------------*/

/* Mix one block from all active ports into s_mix_buffer (stereo float) */
static void audio_mix_one_block(void)
{
    memset(s_mix_buffer, 0, sizeof(s_mix_buffer));

    mutex_lock(&s_audio_mutex);

    for (int p = 0; p < CELL_AUDIO_PORT_MAX; p++) {
        AudioPortSlot* port = &s_ports[p];
        if (!port->in_use || !port->running || !port->buffer)
            continue;

        u32 nch    = (u32)port->param.nChannel;
        u32 nblock = (u32)port->param.nBlock;
        float level = port->param.level;
        if (level <= 0.0f) level = 1.0f;
        if (level > 1.0f) level = 1.0f;

        /* Read one block at the current read_index */
        u32 block_idx = (u32)(port->read_index % nblock);
        u32 block_offset = block_idx * CELL_AUDIO_BLOCK_SAMPLES * nch;
        float* src = port->buffer + block_offset;

        for (u32 s = 0; s < CELL_AUDIO_BLOCK_SAMPLES; s++) {
            float left, right;
            if (nch >= 2) {
                left  = rd_be_f32(&src[s * nch + 0]) * level;
                right = rd_be_f32(&src[s * nch + 1]) * level;
            } else {
                /* Mono: duplicate to both channels */
                left = right = rd_be_f32(&src[s]) * level;
            }

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

            s_mix_buffer[s * 2 + 0] += left;
            s_mix_buffer[s * 2 + 1] += right;
        }

        /* Advance read index + publish the block index to the guest (BE u64 at
         * read_idx_addr) so the game's audio loop sees blocks being consumed and
         * keeps feeding the next one (matches RPCS3: *index = cur_pos, 0..nBlock-1). */
        port->read_index++;
        if (port->read_idx_addr)
            audio_be_w64((u32)port->read_idx_addr, port->read_index % nblock);
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

#ifdef _WIN32
static unsigned __stdcall audio_mix_thread_func(void* arg)
{
    (void)arg;
    printf("[cellAudio] Mixing thread started\n");

    while (s_mix_thread_running) {
        /* Mix and submit one block */
        audio_mix_one_block();
        audio_backend_submit(s_mix_buffer, CELL_AUDIO_BLOCK_SAMPLES);

        /* Notify event queues */
        audio_notify_event_queues();

        /* Wait approximately one audio period (~5.333ms).
         * Adjust based on how much is queued to avoid buffer overrun. */
        u32 queued = audio_backend_queued_samples();
        if (queued > CELL_AUDIO_BLOCK_SAMPLES * 4) {
            Sleep(5);
        } else {
            Sleep(2);
        }
    }

    printf("[cellAudio] Mixing thread stopped\n");
    return 0;
}
#else
static void* audio_mix_thread_func(void* arg)
{
    (void)arg;
    printf("[cellAudio] Mixing thread started\n");

    while (s_mix_thread_running) {
        audio_mix_one_block();
        audio_backend_submit(s_mix_buffer, CELL_AUDIO_BLOCK_SAMPLES);
        audio_notify_event_queues();

        u32 queued = audio_backend_queued_samples();
        if (queued > CELL_AUDIO_BLOCK_SAMPLES * 4) {
            usleep(5000);
        } else {
            usleep(2000);
        }
    }

    printf("[cellAudio] Mixing thread stopped\n");
    return NULL;
}
#endif

static int audio_start_mix_thread(void)
{
    s_mix_thread_running = 1;

#ifdef _WIN32
    s_mix_thread = (HANDLE)_beginthreadex(NULL, 0, audio_mix_thread_func,
                                           NULL, 0, NULL);
    if (!s_mix_thread) {
        s_mix_thread_running = 0;
        return -1;
    }
#else
    if (pthread_create(&s_mix_thread, NULL, audio_mix_thread_func, NULL) != 0) {
        s_mix_thread_running = 0;
        return -1;
    }
#endif
    return 0;
}

static void audio_stop_mix_thread(void)
{
    s_mix_thread_running = 0;
#ifdef _WIN32
    if (s_mix_thread) {
        WaitForSingleObject(s_mix_thread, 2000);
        CloseHandle(s_mix_thread);
        s_mix_thread = NULL;
    }
#else
    pthread_join(s_mix_thread, NULL);
#endif
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellAudioInit(void)
{
    printf("[cellAudio] Init()\n");

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

#ifdef _WIN32
    { LARGE_INTEGER f, c;
      QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
      s_audio_start_us = (u64)((c.QuadPart * 1000000LL) / f.QuadPart); }
#endif
    s_audio_initialized = 1;
    return CELL_OK;
}

s32 cellAudioQuit(void)
{
    printf("[cellAudio] Quit()\n");

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    audio_stop_mix_thread();
    audio_backend_shutdown();

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

/* Guest structs are big-endian (PPC); the import bridge hands us a HOST pointer
 * into guest VM, so multi-byte fields must be byteswapped to host order before
 * use. (Bug found 2026-06-20: nChannel was read raw -> 0x0800000000000000 instead
 * of 8 -> CELL_AUDIO_ERROR_PARAM -> the game aborted audio init -> the surround
 * mixer + the CRI movie pipeline collapsed. RPCS3 reads 8 and proceeds.)
 * The ca_be64/ca_be32 helpers are defined above (near the mixer). */

s32 cellAudioPortOpen(const CellAudioPortParam* param, u32* portNum)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (!param || !portNum)
        return CELL_AUDIO_ERROR_PARAM;

    /* Byteswap the big-endian guest param into a host-native copy. */
    CellAudioPortParam hp;
    hp.nChannel = ca_be64(param->nChannel);
    hp.nBlock   = ca_be64(param->nBlock);
    hp.attr     = ca_be64(param->attr);
    { union { u32 u; float f; } lv; lv.u = ca_be32(*(const u32*)&param->level); hp.level = lv.f; }

    printf("[cellAudio] PortOpen(nChannel=%llu, nBlock=%llu)\n",
           (unsigned long long)hp.nChannel, (unsigned long long)hp.nBlock);

    /* Validate parameters */
    u64 nch = hp.nChannel;
    u64 nblk = hp.nBlock;
    if (nch != CELL_AUDIO_PORT_2CH && nch != CELL_AUDIO_PORT_8CH)
        return CELL_AUDIO_ERROR_PARAM;
    if (nblk != CELL_AUDIO_BLOCK_8 && nblk != CELL_AUDIO_BLOCK_16 && nblk != CELL_AUDIO_BLOCK_32)
        return CELL_AUDIO_ERROR_PARAM;

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
    port->write_index = 0;

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

    /* portNum is a host pointer aliasing guest BE memory -- write big-endian
     * (was host-endian: the game read a byte-swapped port index). */
    { unsigned char* pn = (unsigned char*)portNum;
      pn[0]=(unsigned char)(found>>24); pn[1]=(unsigned char)(found>>16);
      pn[2]=(unsigned char)(found>>8);  pn[3]=(unsigned char)found; }

    /* CRI-gate TEST (pt30, env YZ_AUDIO_FORCE): the game never calls cellAudioPortStart
     * (libmixer init never completes). HYPOTHESIS: the criMana player won't decode the
     * voice until the audio OUTPUT is running (somewhere to drain PCM). Simulate the
     * missing PortStart (port->running=1) + SetNotifyEventQueue (register the surround
     * mixer's queue key so our mixing thread feeds it period events). If the player then
     * decodes + the boot progresses, the audio-output-ready handshake IS the gate. */
    if (getenv("YZ_AUDIO_FORCE")) {
        s_ports[found].running = 1;
        printf("[cellAudio] YZ_AUDIO_FORCE: port %d forced running\n", found);
    }

    mutex_unlock(&s_audio_mutex);

    if (getenv("YZ_AUDIO_FORCE"))
        cellAudioSetNotifyEventQueue(0x8000CAFE02460300ULL);  /* register mixer queue */
    return CELL_OK;
}

s32 cellAudioPortClose(u32 portNum)
{
    printf("[cellAudio] PortClose(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    mutex_lock(&s_audio_mutex);

    if (!s_ports[portNum].in_use) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    }

    /* Buffer lives in the guest-VM bump region (not malloc'd) -- don't free it. */
    s_ports[portNum].buffer  = NULL;
    s_ports[portNum].in_use  = 0;
    s_ports[portNum].running = 0;

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortStart(u32 portNum)
{
    printf("[cellAudio] PortStart(port=%u)\n", portNum);

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
    printf("[cellAudio] PortStop(port=%u)\n", portNum);

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

/* Surmixer synchronization (mirrors RPCS3 AudioSetNotifyEventQueue,
 * cellAudio.cpp:1671-1719 + the merged #18857 fix). The CRI surmixer's notify
 * queue (key c_mxr000) is CREATED by the _cellsurMixerMain thread; RPCS3 BLOCKS
 * the registration until that queue exists so the surmixer init cannot race
 * ahead of its own thread (RPCS3 issue #18852). Gated by YZ_SURMIXER_SYNC. */
extern void yz_for_each_thread(void (*cb)(unsigned int tid, const char* name, void* handle));
static volatile int s_surmixer_thread_seen;
static void surmixer_name_cb(unsigned int tid, const char* name, void* handle)
{ (void)tid; (void)handle; if (name && strcmp(name, "_cellsurMixerMain") == 0) s_surmixer_thread_seen = 1; }
static int has_sur_mixer_thread(void)
{ s_surmixer_thread_seen = 0; yz_for_each_thread(surmixer_name_cb); return s_surmixer_thread_seen; }

s32 cellAudioSetNotifyEventQueue(u64 key)
{
    printf("[cellAudio] SetNotifyEventQueue(key=0x%llX)\n",
           (unsigned long long)key);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    { static int surmix_sync = -1;
      if (surmix_sync < 0) surmix_sync = getenv("YZ_SURMIXER_SYNC") ? 1 : 0;
      const unsigned long long c_mxr000 = 0x8000CAFE02460300ULL;
      if (surmix_sync && (key == c_mxr000 || key == 0) && has_sur_mixer_thread()) {
          int found = 0;
          for (int count = 0; count < 100; count++) {
              if (sys_event_find_queue_by_key(c_mxr000)) { found = 1; break; }
#ifdef _WIN32
              Sleep(50);
#else
              usleep(50000);
#endif
          }
          if (key == 0 && found) key = c_mxr000;   /* correct key (#18857) */
          printf("[cellAudio] surmixer-sync: c_mxr000 queue %s after wait\n",
                 found ? "found" : "TIMEOUT");
      } }

    mutex_lock(&s_audio_mutex);

    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) {
            s_notify_queues[i].in_use = 1;
            s_notify_queues[i].key = key;
            mutex_unlock(&s_audio_mutex);
            return CELL_OK;
        }
    }

    mutex_unlock(&s_audio_mutex);
    return CELL_AUDIO_ERROR_PARAM; /* no free slots */
}

s32 cellAudioRemoveNotifyEventQueue(u64 key)
{
    printf("[cellAudio] RemoveNotifyEventQueue(key=0x%llX)\n",
           (unsigned long long)key);

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
    return CELL_AUDIO_ERROR_PARAM;
}

s32 cellAudioGetPortConfig(u32 portNum, CellAudioPortConfig* config)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (!config)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);

    AudioPortSlot* port = &s_ports[portNum];

    /* Write the guest CellAudioPortConfig (32 bytes, BIG-ENDIAN) at its real PS3
     * offsets (RPCS3 cellAudio.h): readIndexAddr@0(u32) status@4(u32)
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

    {   /* out-param is a raw guest pointer: write guest-BE (audit 2026-07-01) */
        u32 v = !s_ports[portNum].in_use ? (u32)CELL_AUDIO_STATUS_CLOSE
              : s_ports[portNum].running ? (u32)CELL_AUDIO_STATUS_RUN
                                         : (u32)CELL_AUDIO_STATUS_READY;
        unsigned char* c = (unsigned char*)status;
        c[0] = (unsigned char)(v >> 24); c[1] = (unsigned char)(v >> 16);
        c[2] = (unsigned char)(v >> 8);  c[3] = (unsigned char)v;
    }

    return CELL_OK;
}

/* Block tag of slot blockNo (RPCS3 cellAudio.cpp cellAudioGetPortBlockTag:
 * tag = global_counter + blockNo - cur_pos). Our read_index is the monotonic
 * global counter and read_index % nBlock the current slot, so the tag of slot
 * blockNo in the current lap is read_index - cur_pos + blockNo. libmixer's
 * sur-mixer pump paces itself with this + GetPortTimestamp; the ENOSYS stub
 * starved the mixer (2026-07-02). Out-param written guest-BE (guest is
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
    u64 t = port->read_index + blockNo - (port->read_index % nblk);
    mutex_unlock(&s_audio_mutex);

    unsigned char* c = (unsigned char*)tag;
    for (int i = 0; i < 8; i++)
        c[i] = (unsigned char)(t >> (56 - i * 8));
    return CELL_OK;
}

/* Microsecond timestamp at which block `tag` was mixed (RPCS3: start_time +
 * tag * 256 * 1000000 / 48000). Anchored to the mixer's own block clock:
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
    printf("[cellAudio] SetPersonalDevice(stream=%d, device=%d) - stub\n",
           iPersonalStream, iDevice);
    return CELL_OK;
}

s32 cellAudioUnsetPersonalDevice(s32 iPersonalStream)
{
    (void)iPersonalStream;
    printf("[cellAudio] UnsetPersonalDevice(stream=%d) - stub\n", iPersonalStream);
    return CELL_OK;
}
