/* cellGcmSys overrides.
 * Part of import_overrides.cpp, which includes its parts once, in order. */

extern "C" int32_t cellGcmInit(uint32_t cmd_size, uint32_t io_size,
                                uint32_t io_address);
extern "C" void cellGcmTickVBlank(void);

/* _cellGcmFunc15: no-op, returns 0. */
static constexpr uint64_t g_yz_gcm_system_mode = 0x820u;
static uint32_t g_yz_gcm_cmd_size;
extern "C" void yz_ovr__cellGcmFunc15(ppu_context* ctx)
{
    ctx->gpr[3] = 0;
}

/* Default FIFO callback. Each segment reserves its final word for a jump to the
 * next segment. Publish the jump before advancing PUT, and do not reuse a
 * segment while GET still points into it. */
extern "C" void yz_gcm_fifo_callback(ppu_context* ctx)
{
    uint32_t gctx = (uint32_t)ctx->gpr[3];          /* CellGcmContextData* */
    if (gctx && yz_gcm_io_addr && g_yz_gcm_cmd_size &&
        g_yz_gcm_segment_bytes) {
        const uint32_t begin = vm_read32(gctx + 0x0);
        const uint32_t begin_off = begin - yz_gcm_io_addr;
        const uint32_t cur = vm_read32(gctx + 0x8);
        const uint32_t cur_off = cur - yz_gcm_io_addr;
        const uint32_t segment = cur_off / g_yz_gcm_segment_bytes;
        const uint32_t segment_count =
            g_yz_gcm_cmd_size / g_yz_gcm_segment_bytes;
        const uint32_t next_segment =
            segment_count ? (segment + 1u) % segment_count : 0u;
        uint32_t next_off = next_segment * g_yz_gcm_segment_bytes;
        if (next_segment == 0u)
            next_off += 0x1000u;
        const uint32_t next_begin = yz_gcm_io_addr + next_off;
        const uint32_t next_end = yz_gcm_io_addr +
            (next_segment + 1u) * g_yz_gcm_segment_bytes - 4u;

        vm_write32(cur, 0x20000000u | (next_off & 0x1FFFFFFCu));
        MemoryBarrier();
        {
            vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_PUT, cur_off);
        }
        vm_write32(gctx + 0x0, next_begin);
        vm_write32(gctx + 0x4, next_end);
        vm_write32(gctx + 0x8, next_begin);

        for (;;) {
            uint32_t get;
            uint32_t pending_return;
            /* GET can be in an external display list while the FIFO's pending
             * CALL return still owns a ring segment.  Snapshot both under the
             * consumer lock; recycling either address would overwrite work
             * the consumer has not retired yet. */
            yz_rsx_fifo_acquire();
            get = vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_GET) & ~3u;
            pending_return = g_fifo_ret;
            yz_rsx_fifo_release();
            const uint32_t next_end_off = next_end - yz_gcm_io_addr;
            const int get_in_next = get >= next_off && get <= next_end_off;
            const int return_in_next = pending_return != ~0u &&
                pending_return >= next_off && pending_return <= next_end_off;
            const int get_in_initial_window = get < 0x1000u;
            if (!get_in_next && !return_in_next && !get_in_initial_window)
                break;
            if (return_in_next) {
                static unsigned protected_returns = 0;
                if (++protected_returns <= 16u) {
                    fprintf(stderr,
                            "[gcm] segment recycle waiting for pending FIFO "
                            "return=0x%X next=[0x%X,0x%X] GET=0x%X\n",
                            pending_return, next_off, next_end_off, get);
                    fflush(stderr);
                }
            }
            SwitchToThread();
        }
    }
    ctx->gpr[3] = 0;
}

/* HLE _cellGcmInitBody sets up the guest-memory contract, maps the io command
 * buffer, and builds the segmented default command context. Args:
 * gpr[3]=CellGcmContextData**, gpr[4]=cmdSize, gpr[5]=ioSize, gpr[6]=ioAddress. */
extern "C" void yz_ovr__cellGcmInitBody(ppu_context* ctx)
{
    uint32_t ctx_slot = (uint32_t)ctx->gpr[3];   /* CellGcmContextData** */
    uint32_t cmd_size = (uint32_t)ctx->gpr[4];
    uint32_t io_size  = (uint32_t)ctx->gpr[5];
    uint32_t io_addr  = (uint32_t)ctx->gpr[6];

    const int32_t init_rc = cellGcmInit(cmd_size, io_size, io_addr);
    if (init_rc != 0) {
        ctx->gpr[3] = (uint64_t)(int64_t)init_rc;
        return;
    }

    yz_gcm_io_addr = io_addr;
    yz_gcm_io_size = io_size;
    g_yz_gcm_cmd_size = cmd_size;
    /* The title configures this 8 MB FIFO as eight 1 MB segments (bufdesc +0x30
     * == 0x40000 words).  Do not snapshot cellGcmSys's process-wide default
     * here: the title calls cellGcmSetDefaultFifoSize only after
     * _cellGcmInitBody returns, and cellGcmSys (libs/video/cellGcmSys.c)
     * starts at a 32 KB default until then. Caching that early value would
     * produce 256 tiny segments and eventually deadlock the producer in the
     * recycle callback. */
    g_yz_gcm_segment_bytes = 0x100000u;
    if (!g_yz_gcm_cmd_size)
        g_yz_gcm_cmd_size = g_yz_gcm_segment_bytes;
    if (g_yz_gcm_segment_bytes > g_yz_gcm_cmd_size)
        g_yz_gcm_segment_bytes = g_yz_gcm_cmd_size;

    /* RSX local memory (0xC0000000): reserved by vm_init, commit it now. The
     * game addresses local VRAM directly (cellGcmAddressToOffset /
     * MapLocalMemory) right after init. If the title calls
     * sys_rsx_memory_allocate itself, that path does the same. */
    VirtualAlloc(vm_base + YZ_GCM_LOCAL_BASE, YZ_GCM_LOCAL_SIZE, MEM_COMMIT, PAGE_READWRITE);

    /* Guest-memory contract + window + consumer, set up through the sys_rsx
     * context allocation. */
    {
        static ppu_context sc;
        memset(&sc, 0, sizeof(sc));
        sc.gpr[8] = g_yz_gcm_system_mode;
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

    /* The first segment reserves 4 KB at the front.  `end` is the address of
     * the segment's final word; the callback writes its linking jump at the
     * last current position before switching. */
    uint32_t begin = io_addr + 0x1000;
    uint32_t end   = io_addr + g_yz_gcm_segment_bytes - 4u;
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

/* ---- HLE gcm helper overrides ----------------------------------------------
 * All io/control/label addresses point at the structures the FIFO consumer
 * reads (RSX_DMA_CONTROL, RSX_REPORTS, g_rsx_iomap_ea) so the game and the RSX
 * agree by construction. */
extern "C" uint32_t cellGcmGetTiledPitchSize(uint32_t size);
extern "C" uint64_t cellGcmGetTimeStampLocation(uint32_t index, uint32_t location);
extern "C" int32_t cellGcmSetDisplayBuffer(uint32_t buffer_id,
                                            uint32_t offset,
                                            uint32_t pitch,
                                            uint32_t width,
                                            uint32_t height);

/* put/get/ref poll pointer -> the guest dma_control (put@+0 get@+4 ref@+8). */
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

extern "C" void yz_ovr_cellGcmSetDisplayBuffer(ppu_context* ctx)
{
    const uint32_t id = (uint32_t)ctx->gpr[3];
    const uint32_t offset = (uint32_t)ctx->gpr[4];
    const uint32_t pitch = (uint32_t)ctx->gpr[5];
    const uint32_t width = (uint32_t)ctx->gpr[6];
    const uint32_t height = (uint32_t)ctx->gpr[7];
    const int32_t rc = cellGcmSetDisplayBuffer(
        id, offset, pitch, width, height);
    if (rc == 0 && id < 8u) {
        g_rsx_dispbuf[id].width = width;
        g_rsx_dispbuf[id].height = height;
        g_rsx_dispbuf[id].pitch = pitch;
        g_rsx_dispbuf[id].offset = offset;
        if (id + 1u > g_rsx_dispbuf_count)
            g_rsx_dispbuf_count = id + 1u;
        rsx_live_draw_set_display_buffer(
            id, 0, offset, pitch, width, height);
    }
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

/* ea -> io offset via the consumer's iomap (the game computes put=AddressToOffset
 * (current)). Linear fast path for the command-buffer ring. */
extern "C" void yz_ovr_cellGcmAddressToOffset(ppu_context* ctx)
{
    uint32_t ea = (uint32_t)ctx->gpr[3], out = (uint32_t)ctx->gpr[4];
    if (ea >= YZ_GCM_LOCAL_BASE &&
        ea < YZ_GCM_LOCAL_BASE + YZ_GCM_LOCAL_SIZE) {
        if (out) vm_write32(out, ea - YZ_GCM_LOCAL_BASE);
        ctx->gpr[3] = 0; return;
    }
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
    ctx->gpr[3] = 0;
}

extern "C" void yz_ovr_cellGcmUnmapEaIoAddress(ppu_context* ctx) { ctx->gpr[3] = 0; }

extern "C" void yz_ovr_cellGcmGetTiledPitchSize(ppu_context* ctx)
{
    ctx->gpr[3] = cellGcmGetTiledPitchSize((uint32_t)ctx->gpr[3]);
}

extern "C" void yz_ovr_cellGcmGetTimeStampLocation(ppu_context* ctx)
{
    ctx->gpr[3] = cellGcmGetTimeStampLocation((uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4]);
}

/* Publish a flip through the guest FIFO so draw submission, display queueing,
 * and presentation stay ordered on the one RSX consumer thread.  The public
 * GCM command ABI defines the head-1 queue/flip methods as 0xE944/0xE924; the
 * flip argument's high bit selects the buffer already queued for that head. */
static int32_t yz_gcm_append_flip_commands(uint32_t context,
                                           uint32_t buffer_id,
                                           int wait_for_label,
                                           uint32_t label_index,
                                           uint32_t label_value)
{
    if (!context || buffer_id >= 8u)
        return (int32_t)CELL_GCM_ERROR_INVALID_VALUE;

    uint32_t words[10];
    uint32_t count = 0;
    if (wait_for_label) {
        words[count++] = 0x00040060u;              /* NV406E semaphore context */
        words[count++] = 0x66616661u;
        words[count++] = 0x00040064u;              /* NV406E semaphore offset */
        words[count++] = (label_index & 0xFFu) * 0x10u;
        words[count++] = 0x00040068u;              /* NV406E semaphore acquire */
        words[count++] = label_value;
    }
    words[count++] = 0x0004E944u;                  /* queue buffer on head 1 */
    words[count++] = buffer_id;
    words[count++] = 0x0004E924u;                  /* flip head 1 */
    words[count++] = 0x8000010Fu;

    const uint32_t current = vm_read32(context + 0x08u);
    const uint32_t end = vm_read32(context + 0x04u);
    const uint32_t bytes = count * 4u;
    if (!current || current > UINT32_MAX - bytes || current + bytes > end)
        return (int32_t)CELL_GCM_ERROR_FAILURE;

    for (uint32_t i = 0; i < count; ++i)
        vm_write32((uint64_t)current + i * 4u, words[i]);

    MemoryBarrier();
    vm_write32(context + 0x08u, current + bytes);
    return 0;
}

/* These entry points take the GCM context as arg0 (r3) and buffer id in r4. */
extern "C" void yz_ovr__cellGcmSetFlipCommand(ppu_context* ctx)
{
    const uint32_t context = (uint32_t)ctx->gpr[3];
    const uint32_t buffer_id = (uint32_t)ctx->gpr[4];
    const int32_t result =
        yz_gcm_append_flip_commands(context, buffer_id, 0, 0, 0);
    ctx->gpr[3] = (uint64_t)(int64_t)result;
}

extern "C" void yz_ovr__cellGcmSetFlipCommandWithWaitLabel(ppu_context* ctx)
{
    const uint32_t context = (uint32_t)ctx->gpr[3];
    const uint32_t buffer_id = (uint32_t)ctx->gpr[4];
    const uint32_t label_index = (uint32_t)ctx->gpr[5];
    const uint32_t label_value = (uint32_t)ctx->gpr[6];
    const int32_t result = yz_gcm_append_flip_commands(
        context, buffer_id, 1, label_index, label_value);
    ctx->gpr[3] = (uint64_t)(int64_t)result;
}
