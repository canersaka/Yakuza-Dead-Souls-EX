/* RSX method execution, events and the journal throttle.
 * Part of import_overrides.cpp, which includes its parts once, in order. */

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

/* Semaphore semantics: each class has a context-DMA selector + offset
 * register; the DMA selects where the semaphore lives:
 *   0x66606660/0x66616661 -> label area (what cellGcmGetLabelAddress returns)
 *   0xFEED0001            -> main memory via the io map (offset = io offset)
 *   0xFEED0000            -> RSX local memory
 * back_end release swizzles the value (ARGB shuffle); the others are raw. */
static uint32_t yz_rsx_sem_dma_406e = 0x66616661;  /* power-on values */
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
    case 0x56616660u:                            /* DEVICE_RW */
    case 0x56616661u:                            /* DEVICE_R  -> device_addr + offset.
        * The flip HW-sync semaphore lives at device+0x30. */
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
 * writes data words into memory through the 2D blit engine. Yakuza uses it
 * to publish its flip/vsync counters to a spot in io memory that the PPU
 * then polls. A8R8G8B8/Y32 is a raw word copy to dst_offset + x*4 + y*pitch;
 * operands with index >= SIZE_OUT.x are skipped. */
static uint32_t yz_rsx_blit_dst_dma = 0xFEED0000;
static uint32_t yz_rsx_blit_dst_off;
static uint32_t yz_rsx_blit_pitch   = (64u << 16) | 64u;
static uint32_t yz_rsx_blit_fmt     = 0xB;     /* a8r8g8b8 */
static uint32_t yz_rsx_blit_point;
static uint32_t yz_rsx_blit_size_out = 0x00010001;

/* Cause bits whose event-port send failed because the queue was full (0 =
 * none). Retried from the FIFO consumer and the vblank tick; take-and-retry is
 * an atomic exchange, and a failed send ORs the bits back. */
static volatile long long g_rsx_ev_pending = 0;
extern "C" volatile long g_yz_ucmd_handler_completed_epoch;

static int64_t yz_rsx_ev_send(uint64_t bits)
{
    ppu_context sc; memset(&sc, 0, sizeof(sc));
    sc.gpr[3] = g_rsx_event_port;
    sc.gpr[5] = bits;
    int64_t r = sys_event_port_send(&sc);
    if (r != 0) {
        _InterlockedOr64(&g_rsx_ev_pending, (long long)bits);
        fprintf(stderr, "[rsx-ev] send failed r=0x%llX bits=0x%llX; "
                "latched for retry\n",
                (unsigned long long)(uint64_t)r, (unsigned long long)bits);
        fflush(stderr);
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
        /* still full: put the bits back; log sparsely so a permanently-
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

static void yz_rsx_406e_release(uint32_t dma, uint32_t offset,
                                uint32_t value)
{
    const uint32_t address = yz_rsx_sem_addr(dma, offset);
    /* Hardware flip credit: a zero release to DEVICE_R+0x30 publishes one. */
    if (address == RSX_DEVICE_ADDR + 0x30u && value == 0u)
        value = 1u;
    if (address)
        vm_write32(address, value);
}

/* Semantic actions of the RSX methods that have side effects outside the
 * graphics state (user interrupts, references, flips). */
static void yz_rsx_exec_user_command(uint32_t arg)
{
    /* The RSX user interrupt pumps the title's wid4 SPU decode pool. */
    if (InterlockedCompareExchange(&g_yz_a010_root_active, 0, 0) != 0) {
        const uint32_t have = vm_read32(RSX_REPORTS + 0xFE0u);
        const uint32_t behind = have - arg;
        if (behind < 0x10000u && arg <= have) {
            static unsigned long stale_ucmd = 0;
            stale_ucmd++;
            if (stale_ucmd <= 16 || (stale_ucmd & 0xFFu) == 0u) {
                fprintf(stderr,
                        "[a010-stale-ucmd] skipped n=%lu cause=0x%08X "
                        "completed=0x%08X\n",
                        stale_ucmd, arg, have);
                fflush(stderr);
            }
            return;
        }
    }
    vm_write32(RSX_DRIVER_INFO + 0x12CC, arg);
    cellGcmDispatchUserCommand(arg);
}

static void yz_rsx_exec_set_reference(uint32_t arg)
{
    /* Publish the reference value to the DMA-control REF register. */
    vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_REF, arg);
}

/* Execute one method register write. Returns 0 normally, 1 if the fifo
 * must stall (semaphore acquire not yet satisfied). */
static int yz_rsx_method(uint32_t method, uint32_t arg)
{
    /* Deliver any latched (queue-full) RSX event bits before consuming more
     * of the stream; one predicted-not-taken branch when idle. */
    if (g_rsx_ev_pending) yz_ucmd_retry_pending();

    /* Mirror the full method stream into the live NV4097 -> D3D12 renderer. It
     * accumulates clears/state/geometry and presents on the 0xE944 flip method,
     * so it runs before the plumbing below. */
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
        uint32_t pitch = yz_rsx_blit_pitch >> 16;
        uint32_t addr = yz_rsx_sem_addr(yz_rsx_blit_dst_dma,
                                        yz_rsx_blit_dst_off + x * 4 + y * pitch);
        if (addr) {
            vm_write32(addr, arg);
        }
        return 0;
    }

    /* GCM_DRIVER_QUEUE (0xE940 + head*4): the game queues a display flip here
     * (queue_flip -> sys_rsx 0x103). arg = the
     * display-buffer id. Record it on the head; the actual "submit" is the
     * flip-sema RELEASE that follows (which arms `pending`). */
    if (method >= 0xE940 && method <= 0xE95C) {
        uint32_t head = ((method - 0xE940) >> 2) & 7u;
        g_rsx_queued_head = head;
        /* Route through the pkg-0x103 syscall case: the gcm queue_flip method
         * is that same shim, and the syscall case also delivers the queue
         * event. The game queues only via this method (the 0x103 syscall
         * itself never fires), so the event delivery depends on this bridge. */
        ppu_context sc; memset(&sc, 0, sizeof(sc));
        sc.gpr[4] = 0x103;
        sc.gpr[5] = head;
        sc.gpr[6] = arg;
        yz_sys_rsx_context_attribute(&sc);
        return 0;
    }

    /* GCM_FLIP_HEAD (0xE920 + head*4): the immediate display flip driven from
     * the GPU command stream (the cellGcmSetFlipImmediate path), sibling of
     * the DRIVER_QUEUE path above and the same lv1 driver-method family as the
     * 0xEB00 user interrupt. It is a thin shim onto the same syscall the
     * pkg-0x102 case implements (arg bit31 = grab-queued-buffer, else
     * display-buffer offset). Route it there. */
    if (method == 0xE920 || method == 0xE924) {
        uint32_t head = (method - 0xE920) >> 2;
        ppu_context sc; memset(&sc, 0, sizeof(sc));
        sc.gpr[4] = 0x102;                     /* pkg: display flip */
        sc.gpr[5] = head;
        sc.gpr[6] = arg;
        yz_sys_rsx_context_attribute(&sc);
        return 0;
    }

    uint32_t addr;
    switch (method) {
    case 0x6188:                                  /* NV3062 DMA_IMAGE_DESTIN */
        yz_rsx_blit_dst_dma = arg;
        break;
    case 0x6300:                                  /* NV3062 SET_COLOR_FORMAT */
        yz_rsx_blit_fmt = arg & 0xFFFFu;
        break;
    case 0x6304:                                  /* NV3062 SET_PITCH (pitch<<16|alignment) */
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
    case 0xEB04:
        yz_rsx_exec_user_command(arg);
        break;
    case 0x050:                                   /* NV406E SET_REFERENCE */
        yz_rsx_exec_set_reference(arg);
        break;
    case 0x060:                                   /* NV406E SET_CONTEXT_DMA_SEMAPHORE */
        yz_rsx_sem_dma_406e = arg;
        break;
    case 0x064:                                   /* NV406E SEMAPHORE_OFFSET */
        yz_rsx_sem_off_406e = arg;
        break;
    case 0x068:                                   /* NV406E SEMAPHORE_ACQUIRE */
        /*
         * The a010 missing-release recovery resumes a linked display-list
         * chain after an old self-stop.  That chain's FE0 completion wait can
         * be reached before its usual SET_CONTEXT_DMA_SEMAPHORE packet, while
         * the inherited selector still names DEVICE_R from the preceding
         * flip-credit wait.  FE0 is the monotonic reports-label counter; the
         * only legitimate device-memory semaphore in this protocol is +0x30.
         * Restore the reports selector at this exact recovered boundary.
         */
        if (yz_rsx_sem_off_406e == 0xFE0u &&
            (yz_rsx_sem_dma_406e == 0x56616660u ||
             yz_rsx_sem_dma_406e == 0x56616661u) &&
            InterlockedCompareExchange(
                &g_yz_a010_root_active, 0, 0) != 0) {
            static unsigned long context_repairs = 0;
            yz_rsx_sem_dma_406e = 0x66616661u;
            context_repairs++;
            if (context_repairs <= 16 ||
                (context_repairs & (context_repairs - 1u)) == 0u) {
                fprintf(stderr,
                        "[a010-sem-context-repair] n=%lu "
                        "off=0x%X DEVICE_R -> SEMAPHORE_R\n",
                        context_repairs, yz_rsx_sem_off_406e);
                fflush(stderr);
            }
        }
        addr = yz_rsx_sem_addr(yz_rsx_sem_dma_406e, yz_rsx_sem_off_406e);
        if (addr && vm_read32(addr) != arg) {
            /* 0xFE0 is a monotonic completion counter.  During a010 recovery,
             * a wait older than the already-completed value is satisfied by
             * definition; requiring exact equality would deadlock forever on
             * stale ring history. */
            if (addr == RSX_REPORTS + 0xFE0u) {
                const uint32_t have = vm_read32(addr);
                const uint32_t ahead = have - arg;
                if (InterlockedCompareExchange(
                        &g_yz_a010_root_active, 0, 0) != 0 &&
                    ahead != 0u && ahead < 0x10000u) {
                    static unsigned long stale_acquire = 0;
                    stale_acquire++;
                    if (stale_acquire <= 16 ||
                        (stale_acquire & 0xFFu) == 0u) {
                        fprintf(stderr,
                                "[a010-stale-acquire] passed n=%lu "
                                "want=0x%08X completed=0x%08X\n",
                                stale_acquire, arg, have);
                        fflush(stderr);
                    }
                    break;
                }
            }
            /* The acquire cannot block here: the vblank/event publisher that
             * would satisfy it may be serialized behind this consumer's lock.
             * Leave GET on the packet and retry. */
            return 1;                             /* not yet satisfied: stall, retry later */
        }
        break;
    case 0x06C:                                   /* NV406E SEMAPHORE_RELEASE */
        yz_rsx_406e_release(yz_rsx_sem_dma_406e, yz_rsx_sem_off_406e, arg);
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
        /* NV3062/NV308A 2D methods were handled by the first switch above;
         * anything else is a 3D rendering method, passed to the method
         * translator in libs/video/rsx_commands.c (tracks state, drives the
         * backend clear/draw). Unknown methods are ignored there. */
        if (!((method >= 0x6000 && method < 0x7000) ||
              (method >= 0xA000 && method < 0xB000)))
            /* Reports remain guest-visible even when the diagnostic graphics
             * backend is inactive. Its raster state/callbacks have no consumer
             * while the live renderer owns the window. */
            if(!ReadAcquire(&g_yz_command_owner_enabled) ||
                !rsx_null_backend_graphics_suppressed() ||
                method==NV4097_SET_CONTEXT_DMA_REPORT || method==NV4097_GET_REPORT ||
                method==NV4097_CLEAR_REPORT_VALUE)
                rsx_process_method(&g_rsx_state, method, arg);
        break;
    }
    return 0;
}

extern "C" uint32_t g_yz_game_toc;        /* dispatch.cpp: the game module's TOC */

/* Journal producer throttle.  Every journal append of the release records
 * goes through one guest function, which the lifter rewrites to call this
 * first.  Release records that survive the game's phase-boundary reinit
 * (which zero-sweeps and rebuilds the dispatch vtables) corrupt the rebuilt
 * state; on hardware the releases complete in microseconds and the engine is
 * empty by then.  Holding the producer until the consumer's poll cursor is
 * within K lines of the head restores that invariant by backpressure.
 *
 * Every path fails open: no consumer yet, a wiped anchor, a cursor outside
 * the arena or an unparsable arena all pass straight through.  Each append
 * waits at most 200 ms, and 64 consecutive timeouts disarm the throttle for
 * the rest of the run.  The consumer legitimately polls at or slightly past
 * the head between appends, so a small forward window reads as gap 0 rather
 * than as a wrapped full ring. */
extern "C" uint32_t yz_consumer_cursor(void);   /* runtime/spu/spu_channels.c */

extern "C" void yz_jrnl_throttle(void)
{
    const uint32_t K = 8;             /* lines the consumer may lag */
    const uint32_t budget_ms = 200;   /* per-append wait before failing open */
    static int disarmed = 0;
    static unsigned long timeouts = 0, consec_to = 0;
    if (disarmed || !g_yz_game_toc) return;

    const uint32_t S = vm_read32(g_yz_game_toc - 0x7410u);
    if (S < 0x10000u || S >= 0xE0000000u) return;
    const uint32_t base = vm_read32(S + 0x08u);
    const uint32_t aend = vm_read32(S + 0x0Cu);
    if (base < 0x10000u || aend <= base || aend - base > 0x1000000u) return;
    const uint32_t span = aend - base;
    const uint32_t bound = K * 0x80u;
    if (bound >= span / 2u) {
        static int warned = 0;
        if (!warned) { warned = 1;
            fprintf(stderr, "[jthr] K=%u out of range for span=0x%X; throttle inert\n", K, span);
            fflush(stderr); }
        return;
    }

    unsigned long long t0 = 0; unsigned long spins = 0;
    for (;;) {
        const uint32_t cur = yz_consumer_cursor();
        if (cur < base || cur >= aend) return;              /* fail-open (incl. 0) */
        const uint32_t head = vm_read32(S + 0x00u);
        if (head < base || head >= aend) return;            /* fail-open */
        /* Caught-up slack: the consumer's cursor may legitimately poll at or
         * a few lines past head between appends; a cursor up to 8 lines ahead
         * reads as gap 0. A cursor "ahead" by more than that with a near-full
         * ring behind it means the consumer lapped, which pacing cannot
         * recover anyway, so failing open there is the safe direction (no lap
         * counter exists to disambiguate). */
        const uint32_t ahead = (cur - head + span) % span;
        const uint32_t gap = (ahead <= 0x400u) ? 0 : (head - cur + span) % span;
        if (gap <= bound) {
            consec_to = 0;      /* any pass (fast or waited) breaks the streak */
            return;
        }
        const unsigned long long now = GetTickCount64();
        if (!t0) t0 = now;
        else if (now - t0 >= budget_ms) {                   /* timeout escape */
            timeouts++; consec_to++;
            if (timeouts <= 8 || (timeouts & 255u) == 0) {
                fprintf(stderr, "[jthr] TIMEOUT #%lu (consec %lu) gap=%u lines head=0x%08X cur=0x%08X\n",
                        timeouts, consec_to, gap / 0x80u, head, cur);
                fflush(stderr);
            }
            if (consec_to >= 64) {
                disarmed = 1;
                fprintf(stderr, "[jthr] DISARMED after %lu consecutive timeouts (consumer gone)\n",
                        consec_to);
                fflush(stderr);
            }
            return;                                         /* fail-open */
        }
        /* backoff by iteration count (GetTickCount64 is ~15.6ms-granular;
         * time-based thresholds below one tick would hot-spin a core) */
        spins++;
        if (spins < 256) YieldProcessor();
        else Sleep(spins < 2048 ? 0 : 1);
    }
}
