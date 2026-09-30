/* lv2 sys_rsx system calls.
 * Part of import_overrides.cpp, which includes its parts once, in order. */

/* ===========================================================================
 * sys_rsx syscalls (lv2 668-677, 0x29C-0x2A5), registered in shims.cpp and
 * called by the gcm HLE (yz_gcm_hle.inc).
 * Args in gpr[3..8], return value in gpr[3] (written by the dispatcher).
 * =========================================================================*/

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
 *             and starts the window + FIFO consumer (the job of
 *             _cellGcmInitBody under HLE). */
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

    /* driver_info. gcm validates version_driver 0x211. */
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

    /* reports region: semaphore[1024]@0, notify[64]@0x1000, report[2048]@0x1400.
     * Semaphores the game has not written hold a distinct non-zero pattern, so
     * a wait on one never matches by accident; notify and report timestamps
     * start at -1. */
    memset(vm_base + RSX_REPORTS, 0, 0x9400);
    for (uint32_t i = 0; i < 1024; i++)
        vm_write32(RSX_REPORTS + i * 4, 0xA5A50000u | i);
    for (uint32_t i = 0; i < 64; i++)
        vm_write64(RSX_REPORTS + 0x1000 + i * 16, ~0ull);     /* notify timestamp */
    for (uint32_t i = 0; i < 2048; i++) {
        vm_write64(RSX_REPORTS + 0x1400 + i * 16 + 0, ~0ull); /* report timestamp */
        vm_write32(RSX_REPORTS + 0x1400 + i * 16 + 8, 0);     /* report val */
        vm_write32(RSX_REPORTS + 0x1400 + i * 16 + 12, ~0u);  /* report pad */
    }

    /* dma_control: get/put/ref = 0 (libgcm sets them up). */
    memset(vm_base + RSX_DMA_CONTROL, 0, 0x60);

    /* One RSX context per process; the other sys_rsx calls do not check it. */
    if (p_ctx_id) vm_write32(p_ctx_id, 1u);
    if (p_dma)    vm_write64(p_dma, RSX_DMA_CONTROL);
    if (p_drv)    vm_write64(p_drv, RSX_DRIVER_INFO);
    if (p_rep)    vm_write64(p_rep, RSX_REPORTS);

    /* RSX event port + queue: libgcm spawns an
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

    /* Bring up the command-translator state, the window thread and the FIFO
     * consumer (once per process). */
    static int started = 0;
    if (!started) {
        started = 1;
        yz_rsx_command_owner_control(1);
        rsx_state_init(&g_rsx_state);
        CreateThread(NULL, 0, yz_window_thread, NULL, 0, NULL);
        CreateThread(NULL, 0, yz_rsx_consumer, NULL, 0, NULL);
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
    static unsigned long iomaps = 0;
    if (++iomaps <= 16 || (iomaps & 255) == 0)
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
 * zcull all funnel through here. */
extern "C" int64_t yz_sys_rsx_context_attribute(ppu_context* ctx)
{
    uint32_t pkg = (uint32_t)ctx->gpr[4];
    uint64_t a3 = ctx->gpr[5], a4 = ctx->gpr[6], a5 = ctx->gpr[7];

    switch (pkg) {
    case 0x001: {       /* FIFO: set get/put */
        /* Serialize the guest GET/PUT set with the consumer's single GET writer
         * so a pkg001 set can't tear a GET the consumer is mid-advance, and the
         * consumer can't clobber a fresh pkg001 set. */
        yz_rsx_fifo_acquire();
        vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, (uint32_t)a3);
        vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_PUT, (uint32_t)a4);
        yz_rsx_fifo_release();
        break;
    }
    case 0x100: break;  /* display mode set */
    case 0x101: break;  /* display sync set (vsync pref) */
    case 0x102: {       /* Display flip */
        uint32_t head = (uint32_t)a3 & 7;
        uint32_t flip_idx;
        if (a4 & 0x80000000u) {           /* grab the queued buffer */
            flip_idx = vm_read32(yz_rsx_head_addr(head) + 0x14); /* lastQueuedBufferId */
        } else {                          /* a4 = display buffer offset */
            flip_idx = 0;
            for (uint32_t i = 0; i < g_rsx_dispbuf_count; i++)
                if (g_rsx_dispbuf[i].offset == (uint32_t)a4) { flip_idx = i; break; }
        }
        /* One present per flip, done by the vblank retire: presenting here
         * and also arming pending would double-present every immediate flip.
         * Record the resolved buffer on the head so the retire presents the
         * right one for the offset form too, then arm; the retire presents +
         * publishes the done bit (which also survives the game's
         * cellGcmResetFlipStatus ordering). */
        vm_write32(yz_rsx_head_addr(head) + 0x14, flip_idx);  /* lastQueuedBufferId */
        InterlockedExchange(&g_rsx_flip_pending[head], 1);
        break;
    }
    case 0x103: {       /* Display queue */
        uint32_t head = (uint32_t)a3 & 7;
        uint32_t ha = yz_rsx_head_addr(head);
        vm_write32(ha + 0x14, (uint32_t)a4);   /* lastQueuedBufferId */
        vm_write32(ha + 0x08, vm_read32(ha + 0x08) | 0x40000000u
                              | (1u << ((uint32_t)a4 & 31)));
        /* A queued flip also raises the per-head QUEUE event when the game
         * registered a handler for it.  A send that finds the queue full is
         * latched and redelivered, like the other RSX causes. */
        if (g_rsx_event_port) {
            const uint64_t qbit = (uint64_t)(0x20u << head);
            if (vm_read32(RSX_DRIVER_INFO + 0x12C0) & qbit)
                yz_rsx_ev_send(qbit);
        }
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
            rsx_live_draw_set_display_buffer(
                id, 0, g_rsx_dispbuf[id].offset, g_rsx_dispbuf[id].pitch,
                g_rsx_dispbuf[id].width, g_rsx_dispbuf[id].height);
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
    /* device+0x30 = 1: initial HW flip credit set at RSX init.
     * The first flip's ACQUIRE device+0x30==1 waits on this. */
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

/* Host vblank tick (called at 59.94 Hz from main.cpp's vblank thread). Bumps the
 * per-head vBlankCount/time and publishes any pending flip's completion (the
 * done bit the game's render loop polls inline). */
extern "C" void yz_rsx_vblank_tick(void)
{
    if (!g_rsx_ctx_ready) return;
    cellGcmTickVBlank();

    /* Redeliver SPU throw events and RSX event bits that found a full queue.
     * The FIFO consumer also retries RSX events, but it cannot when the lost
     * delivery is what parked it; this tick runs regardless of FIFO flow. */
    { extern void yz_throw_retry_flush(void);
      yz_throw_retry_flush(); }
    if (g_rsx_ev_pending) yz_ucmd_retry_pending();

    uint64_t t = (uint64_t)GetTickCount() * 1000;
    uint64_t flip_ev = 0;
    for (int h = 0; h < 2; h++) {          /* PS3 has 2 active heads */
        uint32_t ha = yz_rsx_head_addr((uint32_t)h);
        const uint64_t vblank_count = vm_read64(ha + 0x30) + 1;
        vm_write64(ha + 0x30, vblank_count);              /* vBlankCount */
        vm_write32(ha + 0x1C, (uint32_t)t);               /* lastVTimeLow */
        vm_write32(ha + 0x3C, (uint32_t)(t >> 32));       /* lastVTimeHigh */

        /* Retire a queued flip on this head: present it, then complete it
         * the way a flip-event package does: set the flip-done flag, stamp
         * flipBufferId/lastFlipTime, and clear the 16-byte flip semaphore at
         * label+0x10, which releases the consumer's ACQUIRE on it. */
        if (InterlockedExchange(&g_rsx_flip_pending[h], 0)) {
            uint32_t buf = vm_read32(ha + 0x14);          /* lastQueuedBufferId */
            yz_rsx_present(buf);
            vm_write32(ha + 0x10, buf);                   /* flipBufferId */
            vm_write32(ha + 0x08, vm_read32(ha + 0x08) | 0x80000000u); /* flip done */
            vm_write64(ha + 0x00, t);                     /* lastFlipTime */
            vm_write64(RSX_REPORTS + 0x10, 0);            /* flip sema (u128) = 0 */
            vm_write64(RSX_REPORTS + 0x18, 0);
            /* Advance the counter the game's render throttle polls.  The RSX
             * bumps it once per presented flip, so do it here, after the
             * present and the label clear. */
            yz_rsx_advance_flip_progress(0x10000u);
            flip_ev |= (uint64_t)(0x8u << 1);             /* SYS_RSX_EVENT_FLIP_BASE<<1 */

        }
    }

    /* Deliver the RSX interrupt to Sony's _gcm_intr_thread via the event queue
     * (send_event): VBLANK every tick + FLIP on completion,
     * masked by driver_info.handlers (@+0x12C0). The intr thread runs the game's
     * registered vblank/flip handler, which advances its render loop (patches the
     * command-buffer pause it left behind). Without this, the handler never fires
     * and the game deadlocks waiting on its own flip fence. */
    if (g_rsx_event_port) {
        uint32_t handlers = vm_read32(RSX_DRIVER_INFO + 0x12C0);
        /* On a flip deliver every registered cause except USER_CMD (0x80),
         * which only the user-command method raises: sending it here would run
         * the user handler with a parameter the game has not written yet.  On
         * a plain vblank deliver VBLANK (0x2) alone. */
        const uint64_t ev = flip_ev ? ((uint64_t)handlers & 0x7Full)
                                    : ((uint64_t)0x2 & handlers);
        if (ev) {
            /* Preserve vblank/flip causes across a transient full event queue.
             * Dropping one here can strand the guest's next-frame/title movie
             * transition even though rendering itself remains live. */
            yz_rsx_ev_send(ev);
        }
    }
}
