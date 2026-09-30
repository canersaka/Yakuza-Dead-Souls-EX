/* Job chain creation and native job publication.
 * Part of cellSpurs.c, which includes its parts once, in order. */

s32 _cellSpursJobChainAttributeInitialize(u32 revision, u32 sdk,
                                           CellSpursJobChainAttribute* a,
                                          const u64* entry, u16 size_desc,
                                          u16 max_grab, const u8* priority,
                                          u32 max_cont, u32 auto_request,
                                          u32 tag1, u32 tag2, u32 fixed,
                                          u32 max_desc, u32 initial_request)
{
    if (!a || !entry || !priority) return CELL_SPURS_JOB_ERROR_NULL_POINTER;
    if (!aligned(a, 8) || !aligned(entry, 8)) return CELL_SPURS_JOB_ERROR_ALIGN;
    if ((size_desc != 64 && (size_desc < 128 || (size_desc & 127))) ||
        !max_grab || max_grab > 16 || max_cont > CELL_SPURS_MAX_SPU ||
        tag1 > 30 || tag2 > 30 || max_desc < 256 || max_desc > 1024 ||
        (max_desc & 127) || size_desc > max_desc ||
        (!auto_request && (!initial_request || initial_request > 255)))
        return CELL_SPURS_JOB_ERROR_INVAL;
    for (u32 i = 0; i < CELL_SPURS_MAX_SPU; ++i)
        if (priority[i] > 15) return CELL_SPURS_JOB_ERROR_INVAL;
    memset(a->bytes, 0, sizeof(a->bytes));
    wr32(a->bytes + 0x00, revision);
    wr32(a->bytes + 0x04, sdk);
    wr32(a->bytes + 0x08, guest_ea(entry));
    wr16(a->bytes + 0x0c, size_desc);
    wr16(a->bytes + 0x0e, max_grab);
    memcpy(a->bytes + 0x10, priority, 8);
    wr32(a->bytes + 0x18, max_cont);
    a->bytes[0x1c] = (u8)(auto_request != 0);
    wr32(a->bytes + 0x20, tag1);
    wr32(a->bytes + 0x24, tag2);
    a->bytes[0x28] = (u8)(fixed != 0);
    wr32(a->bytes + 0x2c, max_desc);
    wr32(a->bytes + 0x30, initial_request);
    return CELL_OK;
}
s32 cellSpursJobChainAttributeSetName(CellSpursJobChainAttribute* a, const char* name)
{
    if (!a || !name) return CELL_SPURS_JOB_ERROR_NULL_POINTER;
    wr32(a->bytes + 0x38, guest_ea(name));
    return CELL_OK;
}
s32 cellSpursCreateJobChainWithAttribute(CellSpurs* s, CellSpursJobChain* object,
                                           const CellSpursJobChainAttribute* a)
{
    if (!s || !object || !a) return CELL_SPURS_JOB_ERROR_NULL_POINTER;
    if (!aligned(s, 128) || !aligned(object, 128) || !aligned(a, 8))
        return CELL_SPURS_JOB_ERROR_ALIGN;
    {
        const u32 entry_ea = rd32(a->bytes + 0x08);
        const u16 size_desc = rd16(a->bytes + 0x0c);
        const u16 max_grab = rd16(a->bytes + 0x0e);
        const u32 max_cont = rd32(a->bytes + 0x18);
        const u32 tag1 = rd32(a->bytes + 0x20);
        const u32 tag2 = rd32(a->bytes + 0x24);
        const u32 max_desc = rd32(a->bytes + 0x2c);
        const u32 initial_request = rd32(a->bytes + 0x30);
        if (!entry_ea) return CELL_SPURS_JOB_ERROR_NULL_POINTER;
        if (entry_ea & 7) return CELL_SPURS_JOB_ERROR_ALIGN;
        if (!job_guest_range_valid(entry_ea, sizeof(u64)))
            return CELL_SPURS_JOB_ERROR_FAULT;
        if ((size_desc != 64 && (size_desc < 128 || (size_desc & 127))) ||
            !max_grab || max_grab > 16 || max_cont > CELL_SPURS_MAX_SPU ||
            tag1 > 30 || tag2 > 30 || max_desc < 256 || max_desc > 1024 ||
            (max_desc & 127) || size_desc > max_desc ||
            (!a->bytes[0x1c] && (!initial_request || initial_request > 255)))
            return CELL_SPURS_JOB_ERROR_INVAL;
        for (u32 i = 0; i < CELL_SPURS_MAX_SPU; ++i)
            if (a->bytes[0x10 + i] > 15) return CELL_SPURS_JOB_ERROR_INVAL;
    }
    SpursState* spurs_state = spurs_find(s);
    if (!spurs_state) return CELL_SPURS_JOB_ERROR_STAT;
    JobChainState* jc = jobchain_make(object);
    if (!jc) return CELL_SPURS_JOB_ERROR_NOMEM;
    u32 wid = 0;
    if (!spurs_allocate_workload(spurs_state, &wid)) {
        return CELL_SPURS_JOB_ERROR_NOMEM;
    }
    memset(object->bytes, 0, sizeof(object->bytes));
    jc->spurs = s;
    jc->entry_ea = rd32(a->bytes + 0x08);
    jc->descriptor_size = rd16(a->bytes + 0x0c);
    jc->max_grab = rd16(a->bytes + 0x0e);
    jc->max_contention = rd32(a->bytes + 0x18);
    jc->parallel_capacity = 0;
    jc->pool = NULL;
    const u32 requested = a->bytes[0x1c] ? jc->max_contention :
        rd32(a->bytes + 0x30);
    for (u32 i = 0; i < spurs_state->nspus && i < CELL_SPURS_MAX_SPU &&
         jc->parallel_capacity < 2 && jc->parallel_capacity < requested &&
         jc->parallel_capacity < jc->max_contention; ++i) {
        if (a->bytes[0x10 + i])
            jc->parallel_spu_ids[jc->parallel_capacity++] = i;
    }
    jc->wid = wid;
    jobchain_clear_queued_snapshots(jc);
    jc->watch_lo = jc->entry_ea;
    jc->watch_hi = jc->entry_ea + 8u;
    jc->bridge_lo = UINT32_MAX;
    jc->bridge_hi = 0;
    jc->command_state_count = 0;
    memset(jc->command_state_ea, 0, sizeof(jc->command_state_ea));
    memset(jc->command_last_descriptor, 0,
           sizeof(jc->command_last_descriptor));
    memset(jc->command_last_binary, 0,
           sizeof(jc->command_last_binary));
    memset(jc->command_last_binary_blocks, 0,
           sizeof(jc->command_last_binary_blocks));
    memset(jc->command_last_job_type, 0,
           sizeof(jc->command_last_job_type));
    memset(jc->command_last_identity_valid, 0,
           sizeof(jc->command_last_identity_valid));
    memset(jc->command_last_dispatch_sequence, 0,
           sizeof(jc->command_last_dispatch_sequence));
    jc->command_dispatch_sequence_next = 0;
    jc->command_snapshot_count = jc->command_snapshot_index = 0;
    jc->parked_slot_count = 0;
    for (u32 i = 0; i < MAX_JOB_PARKED_SLOTS; ++i)
        atomic_store_explicit(&jc->command_consumed[i], 0,
                              memory_order_relaxed);
    memset(jc->command_publication_sequence, 0,
           sizeof(jc->command_publication_sequence));
    memset(jc->command_acquired_publication_sequence, 0,
           sizeof(jc->command_acquired_publication_sequence));
    memset(jc->command_polled_unacknowledged, 0,
           sizeof(jc->command_polled_unacknowledged));
    memset(jc->deferred_resume_sequence, 0,
           sizeof(jc->deferred_resume_sequence));
    memset(jc->deferred_publication_sequence, 0,
           sizeof(jc->deferred_publication_sequence));
    jc->deferred_sequence_next = 0;
    jc->waiting_command = 0;
    jc->current_ea = jc->entry_ea;
    jc->complete = jc->running = jc->error = 0;
    atomic_store(&jc->shutdown, 0);
    jc->alternate_slot = 0x4c00;
    jc->dma_tag[0] = rd32(a->bytes + 0x20);
    jc->dma_tag[1] = rd32(a->bytes + 0x24);
    jc->next_dma_tag = 0;
    wr64(object->bytes + 0x00, jc->entry_ea);
    object->bytes[0x23] = 0;
    object->bytes[0x24] = a->bytes[0x1c] ? 1 : 0;
    object->bytes[0x28] = (u8)rd32(a->bytes + 0x30);
    object->bytes[0x2a] = (u8)rd32(a->bytes + 0x20);
    object->bytes[0x2b] = (u8)rd32(a->bytes + 0x24);
    {
        const u32 max_desc = rd32(a->bytes + 0x2c);
        object->bytes[0x2c] = (a->bytes[0x28] ? 0x80u : 0u) |
            (u8)((max_desc >= 0x100 ? (max_desc - 0x100) / 128 : 0) << 4);
    }
    object->bytes[0x2d] = (u8)rd32(a->bytes + 0x00);
    wr16(object->bytes + 0x70, jc->max_grab);
    wr16(object->bytes + 0x72, jc->descriptor_size);
    wr32(object->bytes + 0x74, wid);
    wr64(object->bytes + 0x78, guest_ea(s));
    wr32(object->bytes + 0x90, rd32(a->bytes + 0x04));
    object->bytes[0x94] = a->bytes[0x3c] ? 2 : 0;
    {
        const u32 index = (u32)(jc - g_jobchains);
        guest_write_route_begin_target(
            GUEST_WRITE_ROUTE_JOBCHAIN_COMMAND, index);
        guest_write_route_begin_target(
            GUEST_WRITE_ROUTE_JOBCHAIN_BRIDGE, index);
    }
    jobchain_watch_ea_locked(jc, jc->entry_ea);
    return CELL_OK;
}

static u32 job_alloc(u32* cursor, u32 limit, u32 size, u32 alignment)
{
    if (!size) return 0;
    u32 at = (*cursor + alignment - 1) & ~(alignment - 1);
    if (at > limit || size > limit - at) return 0;
    *cursor = at + size;
    return at;
}

/* Mirror the native runner's LS allocation without writing anything. SPURS
 * jobchains have two standard binary slots; large jobs may fit at only one of
 * them once their input, cache, scratch and stack reservations are included.
 * Blind alternation is therefore not a valid slot-selection rule. */
static int job_layout_fits(const u8* d, u32 bin_size,
                           u32 dma_list_size, u32 cache_list_size, u32 slot)
{
    const u32 descriptor_ls = 0x3f000;
    const u32 size_io = rd32(d + 0x14) & 0x3ffffu;
    const u32 size_out = rd32(d + 0x18) & 0x3ffffu;
    const u32 size_stack = rd16(d + 0x1c) ?
                           (u32)rd16(d + 0x1c) * 16u : 8192u;
    const u32 size_scratch = (u32)rd16(d + 0x1e) * 16u;
    /* The SPU ABI's initial stack consists of a minimal caller frame, a
     * link-register save area and the entry frame's backchain. */
    if (size_stack < 0x30u)
        return 0;
    if (bin_size > descriptor_ls - slot)
        return 0;

    u32 cursor = (slot + bin_size + 1023u) & ~1023u;
    if (size_io && !job_alloc(&cursor, descriptor_ls, size_io, 1024))
        return 0;
    if (size_out && !job_alloc(&cursor, descriptor_ls, size_out, 1024))
        return 0;
    for (u32 i = 0; i < cache_list_size / 8; ++i) {
        const u64 item = rd64(d + 0x30 + dma_list_size + i * 8);
        const u32 item_size = (u32)(item >> 32);
        if (item_size && !job_alloc(
                &cursor, descriptor_ls, item_size, 1024))
            return 0;
    }
    return job_alloc(&cursor, descriptor_ls,
                     size_scratch + size_stack, 1024) != 0;
}

static void job_publish_copy_internal(u32 ea, const void* source, u32 size)
{
    if (!size) return;
    const u32 first = ea & ~127u;
    const u32 last = (ea + size - 1) & ~127u;
    spu_lockline_lock();
    memcpy(vm_base + ea, source, size);
    for (u32 line = first; ; line += 128) {
        if (spu_coh_is_reserved(line)) spu_coh_notify_write(line);
        if (line == last) break;
    }
    spu_lockline_unlock();
}


static void job_publish_u32(u32 ea, u32 value)
{
    u8 encoded[4];
    wr32(encoded, value);
    job_publish_copy_internal(ea, encoded, sizeof(encoded));
}

static int job_return_syscall(spu_context* ctx, void* opaque)
{
    (void)opaque;
    /* Drain the job's outstanding MFC transfers before run_job publishes
     * the output areas and frees the context. The task-side scheduler
     * handoffs do exactly this so a wait cannot expose a partial context;
     * job exit needs the same barrier or a late PUT races publication. */
    spu_task_wait_all_dma(ctx);
    return -1;
}

cellSpursNativeJobPrepare g_cell_spurs_native_job_prepare;
int cellSpursNativeJobRangeValid(u32 ea, u32 size)
{
    return size == 0 || (ea <= UINT32_MAX - size && job_guest_range_valid(ea, size));
}

int cellSpursNativeJobAcquire(const cellSpursNativeJobRequest* job,
                              u32 ea, void* output, u32 size)
{
    if (!cellSpursNativeJobRangeValid(ea, size)) return 0;
    if (size) memcpy(output, vm_base + ea, size);
    return 1;
}

void cellSpursNativeJobPublish(const cellSpursNativeJobRequest* job, u32 ea,
                              const void* source, u32 size, u32 pc, u32 command)
{
    if (!size) return;
    job_publish_copy_internal(ea, source, size);
    cellSpursNotifyGuestWrite(ea, size);
}

static int run_job_with_state(JobChainState* jc, u32 descriptor_ea,
                   u32 descriptor_size, const u8* acquired_descriptor,
                   u32* alternate_slot, u32* next_dma_tag, u32 spu_id)
{
    if (!vm_base || descriptor_ea > 0xfffffff0u)
        return CELL_SPURS_JOB_ERROR_DESCRIPTOR;
    if (descriptor_size < 0x30 || descriptor_size > 0x400 ||
        (descriptor_size & 0x0f) ||
        descriptor_ea > 0xffffffffu - descriptor_size)
        return CELL_SPURS_JOB_ERROR_DESCRIPTOR;
    if (!job_guest_range_valid(descriptor_ea, descriptor_size))
        return CELL_SPURS_JOB_ERROR_FAULT;
    u8 descriptor_copy[0x400];
    if (acquired_descriptor)
        memcpy(descriptor_copy, acquired_descriptor, descriptor_size);
    else if (!job_descriptor_acquire_published(
                 descriptor_ea, (u16)descriptor_size, descriptor_copy))
        return CELL_SPURS_JOB_ERROR_FAULT;
    const u8* const d = descriptor_copy;
    /* jobType (d[0x2C]) selects the descriptor format.  A nonzero type is
     * the BINARY2 family, whose binaryInfo layout differs from the legacy
     * eaBinary/sizeBinary pair; refuse it rather than misparse it. */
    if (descriptor_size > 0x2C && d[0x2C] != 0)
        return CELL_SPURS_JOB_ERROR_INVALID_BIN;
    u32 bin_ea = (u32)(rd64(d + 0x00) & ~1ull);
    u32 bin_size = (u32)rd16(d + 0x08) * 16;
    if (!bin_ea || !bin_size) {
        fprintf(stderr,
                "[native-spurs] incomplete job descriptor publication: "
                "descriptor=0x%08X bin=0x%08X blocks=0x%X "
                "kind=0x%08X ready=0x%08X\n",
                descriptor_ea, bin_ea, bin_size / 16u,
                rd32(d + 0x10), rd32(d + 0x14));
        fflush(stderr);
        return CELL_SPURS_JOB_ERROR_INVALID_BIN;
    }
    if (!job_guest_range_valid(bin_ea, bin_size))
        return CELL_SPURS_JOB_ERROR_FAULT;
    const u32 dma_list_size = rd16(d + 0x0a);
    const u32 cache_list_size = rd32(d + 0x24);
    if ((dma_list_size & 7) || (cache_list_size & 7) ||
        dma_list_size + cache_list_size > descriptor_size - 0x30 ||
        cache_list_size / 8 > 4)
        return CELL_SPURS_JOB_ERROR_DESCRIPTOR;
    spu_workload_image image;
    if (!spu_workload_resolve(vm_base + bin_ea, bin_size, &image)) {
        const u64 fingerprint = spu_workload_fingerprint(
            vm_base + bin_ea, bin_size);
        fprintf(stderr,
                "[native-spurs] unregistered job image: descriptor=0x%08X "
                "bin=0x%08X size=0x%X fingerprint=0x%016llX "
                "next-slot=0x%05X; add this binary and its placements to "
                "the game's SPU manifest\n",
                descriptor_ea, bin_ea, bin_size,
                (unsigned long long)fingerprint,
                *alternate_slot);
        fflush(stderr);
        return CELL_SPURS_JOB_ERROR_INVALID_BIN;
    }
    const u32 size_io = rd32(d + 0x14) & 0x3ffffu;
    const u32 size_out = rd32(d + 0x18) & 0x3ffffu;
    const u32 size_stack = rd16(d + 0x1c) ?
                           (u32)rd16(d + 0x1c) * 16u : 8192u;
    const u32 size_scratch = (u32)rd16(d + 0x1e) * 16u;
    const u32 preferred_slot = *alternate_slot;
    const u32 fallback_slot =
        preferred_slot == 0x4c00u ? 0xe400u : 0x4c00u;
    u32 slot = preferred_slot;
    if (!job_layout_fits(
            d, bin_size, dma_list_size, cache_list_size, slot)) {
        if (!job_layout_fits(
                d, bin_size, dma_list_size, cache_list_size,
                fallback_slot)) {
            fprintf(stderr,
                    "[native-spurs] job LS layout does not fit either slot: "
                    "descriptor=0x%08X bin=0x%08X/0x%X io=0x%X "
                    "out=0x%X stack=0x%X scratch=0x%X cache-list=0x%X\n",
                    descriptor_ea, bin_ea, bin_size, size_io, size_out,
                    size_stack, size_scratch, cache_list_size);
            return CELL_SPURS_JOB_ERROR_NOMEM;
        }
        slot = fallback_slot;
    }
    /* Keep scheduler ownership and completion ordering, but retire the entire
     * emulated job context for a title operation with a native implementation. */
    const int native_image = spu_job_placement_image(bin_ea, slot);
    if (g_cell_spurs_native_job_prepare) {
        const cellSpursNativeJobRequest request = {d, descriptor_ea, descriptor_size,
            bin_ea, bin_size, native_image >= 0 ? (u32)native_image : (u32)image.image_id,
            slot, spu_id};
        cellSpursNativeOperation operation = {0};
        if (g_cell_spurs_native_job_prepare(&request, &operation) && operation.execute) {
            *alternate_slot = slot == 0x4c00u ? 0xe400u : 0x4c00u;
            *next_dma_tag ^= 1u;
            operation.execute(&request, operation.user);
            return CELL_OK;
        }
    }
    spu_context* ctx = (spu_context*)malloc(sizeof(*ctx));
    if (!ctx) return CELL_SPURS_JOB_ERROR_NOMEM;
    spu_context_init(ctx, spu_id);
    *alternate_slot = slot == 0x4c00 ? 0xe400 : 0x4c00;
    /* Overlapping placements have distinct image ids. Select from the exact
     * descriptor identity and the native scheduler's chosen LS slot before
     * entering the lifted job. */
    if (native_image >= 0) image.image_id = native_image;
    if (bin_size > SPU_LS_SIZE - slot) { native_spu_context_free(ctx); return CELL_SPURS_JOB_ERROR_INVALID_BIN; }
    memcpy(ctx->ls + slot, vm_base + bin_ea, bin_size);

    const u32 context_ls = 0x4940;
    const u32 descriptor_ls = 0x3f000;
    u32 cursor = (slot + bin_size + 1023u) & ~1023u;
    const u32 io_ls = job_alloc(&cursor, descriptor_ls, size_io, 1024);
    const u32 out_ls = job_alloc(&cursor, descriptor_ls, size_out, 1024);
    u32 cache_ls[4] = {0, 0, 0, 0};
    u32 cache_count = cache_list_size / 8;
    for (u32 i = 0; i < cache_count; ++i) {
        u64 item = rd64(d + 0x30 + dma_list_size + i * 8);
        /* Read-only cache elements carry the full high word as their size
         * (an MFC-list-style 0x7fff mask would silently truncate >=32KB
         * caches). Validate: 16-multiple, fits in LS. (The io list below
         * keeps its 0x7fff mask; those are MFC list elements with a <=16KB
         * contract enforced there.) */
        u32 item_size = (u32)(item >> 32);
        if (item_size & 15u) {
            native_spu_context_free(ctx);
            return CELL_SPURS_JOB_ERROR_INVALID_BIN;
        }
        if (item_size > SPU_LS_SIZE) {
            native_spu_context_free(ctx);
            return CELL_SPURS_JOB_ERROR_INVALID_BIN;
        }
        cache_ls[i] = job_alloc(&cursor, descriptor_ls, item_size, 1024);
        if (item_size && !cache_ls[i]) { native_spu_context_free(ctx); return CELL_SPURS_JOB_ERROR_NOMEM; }
        if (item_size && !job_guest_range_valid((u32)item, item_size)) {
            native_spu_context_free(ctx);
            return CELL_SPURS_JOB_ERROR_FAULT;
        }
        if (item_size) memcpy(ctx->ls + cache_ls[i], vm_base + (u32)item, item_size);
    }
    const u32 scratch_stack_ls =
        job_alloc(&cursor, descriptor_ls, size_scratch + size_stack, 1024);
    if ((size_io && !io_ls) || (size_out && !out_ls) || !scratch_stack_ls) {
        native_spu_context_free(ctx);
        return CELL_SPURS_JOB_ERROR_NOMEM;
    }

    u32 io_offset = 0;
    const u32 io_count = dma_list_size / 8;
    for (u32 i = 0; i < io_count; ++i) {
        u64 item = rd64(d + 0x30 + i * 8);
        u32 item_size = (u32)(item >> 32) & 0x7fffu;
        u32 item_ea = (u32)item;
        const u32 item_flags = (u32)(item >> 32);
        const u32 item_alignment = item_size < 16u ? item_size : 16u;
        const u32 item_slot_size = (item_size + 15u) & ~15u;
        const u32 item_ls_offset = io_offset + (item_ea & 15u);
        /* A DMA-list element's top bit is the MFC stall-and-notify flag,
         * not part of its 15-bit transfer size.  The flag is legal on a
         * zero-length final element (the title uses an exact
         * 0x8000000000000001 sentinel); synchronous host acquisition
         * has already completed by job entry, and hardware ignores a stall
         * on the final element.  Keep rejecting the genuinely reserved bits
         * between that flag and the transfer-size field. */
        if (item_size > 0x4000u || (item_flags & 0x7fff8000u) ||
            (item_size && !item_ea) ||
            (item_size < 16u && item_size != 0u && item_size != 1u &&
             item_size != 2u && item_size != 4u && item_size != 8u) ||
            (item_alignment && (item_ea & (item_alignment - 1u))) ||
            io_offset > size_io || item_slot_size > size_io - io_offset ||
            (item_size && item_ls_offset + item_size >
                              io_offset + item_slot_size)) {
            native_spu_context_free(ctx);
            return CELL_SPURS_JOB_ERROR_DESCRIPTOR;
        }
        if (item_size && !job_guest_range_valid(item_ea, item_size)) {
            native_spu_context_free(ctx);
            return CELL_SPURS_JOB_ERROR_FAULT;
        }
        /* For sub-qword list elements, SPURS preserves the effective
         * address low nibble in the element's 16-byte LS slot.  Jobs use
         * that placement when resolving list pointers. */
        if (item_size) memcpy(ctx->ls + io_ls + item_ls_offset,
                              vm_base + item_ea, item_size);
        io_offset += item_slot_size;
    }

    memcpy(ctx->ls + descriptor_ls, d, descriptor_size);
    memset(ctx->ls + context_ls, 0, 48);
    wr32(ctx->ls + context_ls + 0x00, io_ls);
    for (u32 i = 0; i < 4; ++i)
        wr32(ctx->ls + context_ls + 0x04 + i * 4, cache_ls[i]);
    wr32(ctx->ls + context_ls + 0x14,
         descriptor_size == 64 ? 0 : (descriptor_size / 128) << 28);
    wr16(ctx->ls + context_ls + 0x18, (u16)io_count);
    wr16(ctx->ls + context_ls + 0x1a, (u16)cache_count);
    wr32(ctx->ls + context_ls + 0x1c, out_ls);
    wr32(ctx->ls + context_ls + 0x20, scratch_stack_ls);
    wr32(ctx->ls + context_ls + 0x24,
         jc->dma_tag[*next_dma_tag & 1u]);
    *next_dma_tag ^= 1u;
    wr64(ctx->ls + context_ls + 0x28, descriptor_ea);

    ctx->gpr[0]._u32[0] = 0x0a70;
    {
        /* SPU ABI: entry starts 0x30 below the stack boundary.  The
         * initial frame points to the minimal root frame at end-0x10, whose
         * backchain is NULL; end-0x20 is reserved for the entry LR save.
         * Placing r1 at stack_end would let an ordinary stqd r0,0x10(r1)
         * overwrite the following LS allocation (e.g. when a job layout
         * ends exactly at 0x3f000). */
        const u32 stack_end =
            scratch_stack_ls + size_scratch + size_stack;
        const u32 initial_sp = stack_end - 0x30u;
        const u32 root_sp = stack_end - 0x10u;
        wr32(ctx->ls + initial_sp, root_sp);
        wr32(ctx->ls + root_sp, 0u);
        ctx->gpr[1]._u32[0] = initial_sp;
        /* ABI word element 1 is Available Stack Space. */
        ctx->gpr[1]._u32[1] = size_stack;
    }
    ctx->gpr[3]._u32[0] = context_ls;
    ctx->gpr[4]._u32[0] = descriptor_ls;
    ctx->native_spurs_syscall = job_return_syscall;
    ctx->pc = slot;
    image.entry_pc = slot;
    ctx->image_id = image.image_id;
    const int ok = spu_workload_execute(&image, ctx);
    /* No inout write-back.  The job module does not copy the input-output
     * buffer back to main memory: useInOutBuffer only selects a writable
     * gather buffer, and output is the job's own DMA over the context DMA
     * tag, which the chain fences.  Republishing the LS working copy would
     * overwrite guest pointers with their LS-relocated forms.  The job's
     * output DMAs ran inside spu_workload_execute and job_return_syscall
     * drained them before this point. */
    native_spu_context_free(ctx);
    return ok ? CELL_OK : CELL_SPURS_JOB_ERROR_NOEXEC;
}
