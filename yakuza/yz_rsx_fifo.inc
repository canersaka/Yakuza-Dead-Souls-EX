/* The RSX FIFO consumer thread.
 * Part of import_overrides.cpp, which includes its parts once, in order. */

/* Serializes GET/PUT writes from the consumer and from sys_rsx package 0x001;
 * lazily initialised via InitOnce. */
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

/* Exported for main.cpp, the gcm segment-recycle callback and the movie
 * handoff. GET and the fence race this consumer's read-decide-write window unless written under
 * the lock: GET could be clobbered backward (producer-stall hazard) and the
 * fence could double-advance vs the vblank path. All GET/fence writers must
 * hold g_rsx_fifo_lock. */
extern "C" void yz_rsx_fifo_acquire(void)
{
    yz_rsx_fifo_lock_ensure();
    EnterCriticalSection(&g_rsx_fifo_lock);
}
extern "C" void yz_rsx_fifo_release(void)
{
    LeaveCriticalSection(&g_rsx_fifo_lock);
}

/* Consumer rules:
 *   - GET is re-read on every step and bounded by PUT; get == put means the
 *     ring is drained.
 *   - old jump (cmd & 0xE0000003 == 0x20000000) / new jump (cmd & 3 == 1) are
 *     followed. A jump to itself is the producer's stopper: GET waits on it
 *     until the word is patched or a release is applied.
 *   - call (cmd & 3 == 2) uses a one-level return stack; return
 *     (cmd & 0xFFFF0003 == 0x00020000) pops it.
 *   - a method packet (count = (cmd>>18)&0x7FF, method = cmd&0x3FFFC,
 *     noninc = cmd&0x40000000) is dispatched only once PUT covers the header
 *     and all its arguments. An unsatisfied semaphore acquire leaves GET on
 *     the packet.
 *   - an off-ring GET or unmapped jump target resyncs GET to PUT. */
/* One-level return stack.  Guarded by g_rsx_fifo_lock. */
static uint32_t g_fifo_ret = ~0u;

static volatile LONG g_yz_startup_segment_head_released = 0;

/* The default FIFO buffer starts behind one deliberately published
 * jump-to-self guard.  It is the only self-stopper the consumer may release
 * from PUT alone: segment zero, its reserved 0x1000 head, and a stable PUT
 * snapshot proving a later command boundary.  All recycled/in-stream
 * stoppers retain their journal/SPU publication lifecycle.  The caller owns
 * the final GET write so this same exact rule is usable by both FIFO owners. */
static int yz_rsx_try_release_published_segment_head(
    uint32_t get, uint32_t put, uint32_t command, uint32_t* resume_get)
{
    if (!resume_get || !g_yz_gcm_segment_bytes)
        return 0;
    const uint32_t ring = 0x800000u;
    const int already_released =
        ReadAcquire(&g_yz_startup_segment_head_released) != 0;
    const uint32_t resume = yz_fifo_startup_head_release_resume(
        get, put, command, ring, g_yz_gcm_segment_bytes,
        already_released);
    if (!resume)
        return 0;
    const uint32_t ea = yz_rsx_io_to_ea(get);
    if (!ea)
        return 0;
    MemoryBarrier();
    if (vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT) != put ||
        vm_read32(ea) != command)
        return 0;
    /* Claim the one process-lifetime release before changing the word.  A
     * failed post-claim recheck gives the claim back; once the startup guard
     * is patched, every later self-jump at 0x1000 is a recycled producer
     * stopper and stays under the journal/SPU publication protocol. */
    if (InterlockedCompareExchange(
            &g_yz_startup_segment_head_released, 1, 0) != 0)
        return 0;
    MemoryBarrier();
    if (vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT) != put ||
        vm_read32(ea) != command) {
        InterlockedExchange(&g_yz_startup_segment_head_released, 0);
        return 0;
    }
    vm_write32(ea, 0x20000000u | resume);
    *resume_get = resume;
    return 1;
}

/* Process exactly one FIFO command at GET (self-locked). Returns an observation
 * category: ADVANCING if GET advanced / a method dispatched, otherwise the
 * precise reason it remained idle or stalled. */
static yz_rsx_wait_category yz_rsx_fifo_step_impl(void)
{
    if (!g_rsx_ctx_ready) {
        return YZ_RSX_WAIT_NO_CONTEXT;
    }
    EnterCriticalSection(&g_rsx_fifo_lock);
    uint32_t       get = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_GET) & ~3u;
    const uint32_t put = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT) & ~3u;
    const auto finish = [&](yz_rsx_wait_category category) {
        LeaveCriticalSection(&g_rsx_fifo_lock);
        return category;
    };
    /* ReadAcquire avoids a locked read-modify-write on this per-command
     * path. */
    const int a010_root = ReadAcquire(&g_yz_a010_root_active) != 0;

    /* One graphics-state owner, above per-word observation/translation. The
     * FIFO lock also owns movie handoff. Publish each packet before resolving
     * the next one; range notifications run once before reading its payload. */
#if defined(NDEBUG)
    {
        if(ReadAcquire(&g_yz_command_owner_enabled) &&
            !g_rsx_ev_pending &&
            rsx_null_backend_graphics_suppressed()) {
            const uint32_t retired=yz_rsx_run_prepared_blocks(get,
                [](){return vm_read32(RSX_DMA_CONTROL+RSX_DMACTL_PUT)&~3u;},
                [](uint32_t io,uint32_t* word){
                    const uint32_t address=yz_rsx_io_to_ea(io);
                    if(!address)return false;*word=vm_read32(address);return true;
                },
                [](uint32_t io,uint32_t* words,uint32_t count){
                    const uint32_t address=yz_rsx_io_to_ea(io),bytes=count*4;
                    if(!address || bytes>UINT32_MAX-address ||
                        yz_rsx_io_to_ea(io+bytes-1)!=address+bytes-1)return false;
                    const uint8_t* source=vm_base+address;
                    for(uint32_t i=0;i<count;i++){
                        uint32_t raw;memcpy(&raw,source+i*4,4);words[i]=ps3_bswap32(raw);
                    }
                    return true;
                },
                [](uint32_t header){return rsx_live_draw_prepared_packet_supported(header)!=0;},
                [&](uint32_t header,const uint32_t* words,uint32_t count){
                    if(g_rsx_ev_pending || !ReadAcquire(&g_yz_command_owner_enabled))return false;
                    if(ReadAcquire(&g_yz_a010_root_active)) {
                        const uint32_t method=header&0x3FFFCu,step=(header&0x40000000u)?0:4;
                        for(uint32_t i=0;i<count;i++)
                            if((method+i*step==0x200u||method+i*step==0x204u) && (words[i]>>16)>8192u)return false;
                    }
                    const bool executed=rsx_live_draw_graphics_packet(header,words,&g_rsx_state)!=0;
                    return executed;
                },
                [&](uint32_t next){
                    vm_write32(RSX_DMA_CONTROL+RSX_DMACTL_GET,next);
                    return g_rsx_ctx_ready && (vm_read32(RSX_DMA_CONTROL+RSX_DMACTL_GET)&~3u)==next;
                });
            if(retired)return finish(YZ_RSX_WAIT_ADVANCING);
        }
    }
#endif
    /* FIFO_EMPTY: ring drained. Never reach/pass PUT (put==get). */
    if (get == put)
        return finish(YZ_RSX_WAIT_EMPTY);

    const uint32_t ea = yz_rsx_io_to_ea(get);
    if (!ea) {
        const uint32_t pea = yz_rsx_io_to_ea(put);
        if (pea) {
            static int n = 0; if (n < 12) { n++;
                fprintf(stderr, "[rsx] GET=0x%08X off-ring -> resync to PUT 0x%08X (recover)\n", get, put); }
            vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, put);
            g_fifo_ret = ~0u;
            return finish(YZ_RSX_WAIT_ADVANCING);
        }
        static int warned = 0;
        if (!warned) { warned = 1;
            fprintf(stderr, "[rsx] GET=0x%08X (PUT=0x%08X) not io-mapped; idling\n", get, put); }
        return finish(YZ_RSX_WAIT_BAD_FLOW);
    }

    const uint32_t cmd = vm_read32(ea);

    /* ---- control transfer ---- */
    if ((cmd & 0xE0000003u) == 0x20000000u || (cmd & 3u) == 1u) {   /* old | new jump */
        uint32_t tgt = (cmd & 3u) == 1u ? (cmd & 0xFFFFFFFCu)   /* NEW offset mask */
                                       : (cmd & 0x1FFFFFFCu);  /* OLD offset mask */
        if (tgt == get) {
            /* The reserved default-buffer head is initially guarded by a
             * self-jump.  Release only that startup guard from the consumer
             * side, after PUT proves a later command boundary was published.
             * Recycled and in-stream stoppers keep their journal lifecycle. */
            uint32_t published_resume = get;
            if (yz_rsx_try_release_published_segment_head(
                    get, put, cmd, &published_resume)) {
                vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET,
                           published_resume);
                static unsigned released_heads = 0;
                if (++released_heads <= 16u) {
                    fprintf(stderr,
                            "[gcm] released published segment head "
                            "io=0x%06X PUT=0x%06X\n",
                            get, put);
                    fflush(stderr);
                }
                return finish(YZ_RSX_WAIT_ADVANCING);
            }
            /* Jump-to-self stopper.  At a segment recycle the game defers
             * the previous stopper's release into its journal (tag 0x7F), and
             * the drain that applies it runs only after the flip throttle
             * passes, while the throttled flips sit behind this stopper.
             * After GET has sat on the same stopper for three seconds with PUT
             * past it, apply the game's own journaled release. */
            static uint32_t park_ea = 0; static ULONGLONG park_t0 = 0;
            if (ea != park_ea) { park_ea = ea; park_t0 = GetTickCount64(); }
            const ULONGLONG parked_ms = GetTickCount64() - park_t0;
            const int parked3s = parked_ms > 3000;
            /* The FIFO ring is 8x1MB = 0x800000: GET/PUT wrap from io 0x7xxxxx
             * to 0x0. The iomap's 0x1B00000 spans all io buffers including
             * off-ring ones, so it is not the wrap period. */
            const uint32_t ring  = 0x800000u;
            const uint32_t ahead = (put - get + ring) % ring;   /* PUT distance ahead of GET (ring-wrapped) */
            if (yz_a010_missing_release_try(ea, get, put)) {
                return finish(YZ_RSX_WAIT_ADVANCING);
            }
            const uint32_t rel_entry = (parked3s && ahead != 0u && ahead < (ring >> 1))
                                           ? yz_gcm_stopper_release_entry(ea) : 0u;
            if (rel_entry) {
                vm_write32(ea, 0x20000000u | ((get + 4u) & 0x1FFFFFFCu));   /* release: self-jump -> jump-forward +4 */
                vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, get + 4u);     /* GET advances into the committed body */
                static unsigned long applied = 0;
                if (++applied <= 64 || (applied & 255u) == 0u) {
                    fprintf(stderr, "[park-rel] applied deferred release @io 0x%06X "
                            "(PUT ahead 0x%X) parked=%llums n=%lu\n",
                            get, ahead, (unsigned long long)parked_ms, applied);
                    fflush(stderr);
                }
                return finish(YZ_RSX_WAIT_ADVANCING);
            }
            /* not yet committed (no tag-0x7F entry, or PUT not past); spin in place */
            return finish(YZ_RSX_WAIT_SELF_STOPPER);
        }
        /* a010 missing generated-link patch: an inner link can point into
         * vertex-program payload instead of the EDGE command prologue that
         * follows it. Repair only an immediate non-command target inside the
         * committed a010 window, and only to the exact prologue. */
        if (a010_root) {
            const uint32_t te = yz_rsx_io_to_ea(tgt);
            const uint32_t tw = te ? vm_read32(te) : 0u;
            const int target_flow =
                ((tw & 0xE0000003u) == 0x20000000u) ||
                ((tw & 3u) == 1u) || ((tw & 3u) == 2u) ||
                ((tw & 0xFFFF0003u) == 0x00020000u);
            const int target_method = (tw & 0xA0030003u) == 0u;
            if (te && !target_flow && !target_method) {
                const uint32_t resume =
                    yz_a010_find_pending_chain((tgt + 4u) & 0x7FFFFFu,
                                               put);
                if (resume) {
                    const uint32_t repaired =
                        (cmd & 3u) == 1u
                            ? (resume | 1u)
                            : (0x20000000u | (resume & 0x1FFFFFFCu));
                    vm_write32(ea, repaired);
                    static unsigned long repairs = 0;
                    repairs++;
                    if (yz_a010_log_sparse(repairs)) {
                        fprintf(stderr,
                                "[a010-bad-link] repaired n=%lu source=0x%06X "
                                "word=0x%08X target=0x%06X head=0x%08X "
                                "-> prologue=0x%06X PUT=0x%06X\n",
                                repairs, get, cmd, tgt, tw, resume, put);
                        fflush(stderr);
                    }
                    tgt = resume;
                }
            }
        }
        if (!yz_rsx_io_to_ea(tgt)) {
            const uint32_t pea = yz_rsx_io_to_ea(put);
            if (pea) {
                vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, put);
                g_fifo_ret = ~0u;
                return finish(YZ_RSX_WAIT_ADVANCING);
            }
            return finish(YZ_RSX_WAIT_BAD_FLOW);
        }
        vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, tgt);
        return finish(YZ_RSX_WAIT_ADVANCING);
    }
    if ((cmd & 3u) == 2u) {                                          /* call */
        const uint32_t ctgt = cmd & 0x1FFFFFFCu;                     /* CALL offset mask */
        if (!yz_rsx_io_to_ea(ctgt)) {
            return finish(YZ_RSX_WAIT_BAD_FLOW);
        }
        if (g_fifo_ret != ~0u) {
            /* Nested CALL: the one-level return slot would be clobbered,
             * losing the outer return address, so log it rather than
             * silently overwriting the pending return. */
            static unsigned long nc = 0; nc++;
            if (nc <= 64 || (nc & 255u) == 0u)
                fprintf(stderr, "[rsx] CALL inside subroutine at io=0x%08X: live return 0x%08X "
                        "would be clobbered by new return 0x%08X (n=%lu)\n",
                        get, g_fifo_ret, get + 4u, nc);
        }
        g_fifo_ret = get + 4u;                /* one-level return */
        vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, ctgt);
        return finish(YZ_RSX_WAIT_ADVANCING);
    }
    if ((cmd & 0xFFFF0003u) == 0x00020000u) {                        /* return */
        if (g_fifo_ret != ~0u) {
            vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, g_fifo_ret);
            g_fifo_ret = ~0u;
            return finish(YZ_RSX_WAIT_ADVANCING);
        }
        /* A RETURN with no pending CALL: the stream is inconsistent.
         * Say so once and idle; GET stays on the word. */
        static int warned = 0;
        if (!warned) { warned = 1;
            fprintf(stderr, "[rsx] RETURN without CALL at io=0x%08X; idling\n", get); }
        return finish(YZ_RSX_WAIT_BAD_FLOW);
    }

    /* ---- method packet ---- */
    /* After the flow-control tests, a finalised method header has the
     * 0xA0030003 bits clear. If any is set, GET reached the end of finalised
     * commands in this segment: wait, don't parse data. */
    if (cmd & 0xA0030003u) {
        /* An illegal header is content the producer has not finalised
         * yet: re-read the same GET until it is, logging the park
         * sparsely. */
        static uint32_t stuck_get = 0xFFFFFFFFu;
        static unsigned long nnc = 0;
        if (get != stuck_get) {
            stuck_get = get; nnc++;
            fprintf(stderr, "[rsx] non-command 0x%08X at io=0x%X (PUT=0x%X) -- segment not "
                    "finalised; waiting for producer (n=%lu)\n", cmd, get, put, nnc);
            fflush(stderr);
        }
        return finish(YZ_RSX_WAIT_UNFINALIZED_HOLE);
    }

    const uint32_t count   = (cmd >> 18) & 0x7FFu;
    const uint32_t noninc  = cmd & 0x40000000u;
    const uint32_t method  = cmd & 0x3FFFCu;
    const uint32_t pkt_end = get + 4u + count * 4u;

    /* The whole packet (header + count args) must be committed before it is
     * dispatched; PUT points one past the last committed word. Linear within
     * a segment; wrap is handled by the producer's JUMP. If PUT is
     * mid-packet, wait. */
    if (count && get < put && pkt_end > put) {
        return finish(YZ_RSX_WAIT_PARTIAL_PACKET);
    }

    int stalled = 0;
    for (uint32_t i = 0; i < count && !stalled; i++) {
        const uint32_t op_ea = yz_rsx_io_to_ea(get + 4u + i * 4u);
        if (!op_ea) break;
        const uint32_t eff = noninc ? method : method + i * 4u;
        const uint32_t val = vm_read32(op_ea);
        const uint32_t canonical = eff & 0x1FFCu;
        /* Extents above 8192 come from a mis-linked chain (a guest pointer
         * read as a clip size); skip them and keep the previous clip. */
        if (a010_root &&
            ((canonical == 0x0200u && (val >> 16) > 8192u) ||
             (canonical == 0x0204u && (val >> 16) > 8192u))) {
            static unsigned long invalid_clip = 0;
            invalid_clip++;
            if (yz_a010_log_sparse(invalid_clip)) {
                fprintf(stderr,
                        "[a010-invalid-clip] n=%lu packet-io=0x%06X "
                        "arg-io=0x%06X header=0x%08X count=%u i=%u "
                        "eff=0x%05X val=0x%08X extent=%u ret=0x%08X\n",
                        invalid_clip, get, get + 4u + i * 4u,
                        cmd, count, i, eff, val, val >> 16, g_fifo_ret);
                fflush(stderr);
            }
            continue;
        }
        stalled = yz_rsx_method(eff, val);   /* 1 => semaphore ACQUIRE not satisfied */
    }
    if (stalled) {
        /* Leave GET on this packet header and retry (GET stays un-advanced on
         * an unsatisfied acquire) so the same method re-issues when ready. */
        return finish(YZ_RSX_WAIT_SEMAPHORE);
    }
    vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, pkt_end);

    return finish(YZ_RSX_WAIT_ADVANCING);
}

static yz_rsx_wait_category yz_rsx_fifo_step(void)
{
    return yz_rsx_fifo_step_impl();
}

static DWORD WINAPI yz_rsx_consumer(LPVOID)
{
    yz_place_critical_thread(0);
    yz_thread_adopt_host("rsx-consumer");
    yz_rsx_fifo_lock_ensure();
    fprintf(stderr, "[rsx] FIFO consumer up host_tid=%lu\n",
            GetCurrentThreadId());
    for (;;) {
        if (!g_rsx_ctx_ready) { SwitchToThread(); continue; }
        const yz_rsx_wait_category result = yz_rsx_fifo_step();
        if (result == YZ_RSX_WAIT_ADVANCING)
            continue;
        SwitchToThread();
    }
    return 0;
}
