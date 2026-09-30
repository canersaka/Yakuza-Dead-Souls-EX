/* Descriptor heap helpers.
 * Part of rsx_live_draw.c, which includes its parts once, in order. */

static D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle(u32 idx)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    g.rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.rtv_heap, &h);
    h.ptr += (size_t)idx * g.rtv_step;
    return h;
}
static D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu(u32 slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    g.srv_cpu_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.srv_cpu_heap, &h);
    h.ptr += (size_t)slot * g.srv_step;
    return h;
}
static void srv_write(u32 slot, ID3D12Resource* tex)
{
    ld_material_descriptor_written(slot);
    g.dev->lpVtbl->CreateShaderResourceView(g.dev, tex, NULL, srv_cpu(slot));
}
static void srv_write_zdepth(u32 slot, ID3D12Resource* tex)
{
    ld_material_descriptor_written(slot);
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
    sd.Format = DXGI_FORMAT_R32_FLOAT;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    /* Depth is broadcast to RGB with alpha forced to one. */
    sd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
    sd.Texture2D.MipLevels = 1;
    g.dev->lpVtbl->CreateShaderResourceView(
        g.dev, tex, &sd, srv_cpu(slot));
}
static void srv_write_depth_source(u32 slot, ID3D12Resource* tex)
{
    ld_material_descriptor_written(slot);
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
    sd.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    g.dev->lpVtbl->CreateShaderResourceView(
        g.dev, tex, &sd, srv_cpu(slot));
}
static void uav_write_zdepth(u32 slot, ID3D12Resource* tex)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {0};
    ud.Format = DXGI_FORMAT_R32_FLOAT;
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g.dev->lpVtbl->CreateUnorderedAccessView(
        g.dev, tex, NULL, &ud, srv_cpu(slot));
}
static D3D12_GPU_DESCRIPTOR_HANDLE srv_table(const u32 slots[SRV_TABLE_SIZE])
{
    if (g.srv_ring_used >= SRV_RING_TABLES) {
        D3D12_GPU_DESCRIPTOR_HANDLE invalid = {0};
        return invalid;
    }
    const u32 base = g.srv_ring_used++ * SRV_TABLE_SIZE;
    D3D12_CPU_DESCRIPTOR_HANDLE dst;
    D3D12_CPU_DESCRIPTOR_HANDLE src[SRV_TABLE_SIZE];
    const UINT dst_size = SRV_TABLE_SIZE;
    g.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.srv_heap, &dst);
    dst.ptr += (size_t)base * g.srv_step;
    for (u32 i = 0; i < SRV_TABLE_SIZE; i++)
        src[i] = srv_cpu(slots[i]);
    g.dev->lpVtbl->CopyDescriptors(
        g.dev, 1, &dst, &dst_size, SRV_TABLE_SIZE, src, NULL,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_GPU_DESCRIPTOR_HANDLE h;
    g.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(g.srv_heap, &h);
    h.ptr += (u64)base * g.srv_step;
    return h;
}
static int depth_snapshot_descriptors(
    u32 zindex,
    D3D12_GPU_DESCRIPTOR_HANDLE* out_source,
    D3D12_GPU_DESCRIPTOR_HANDLE* out_destination)
{
    if (zindex >= MAX_SURFACES || g.srv_ring_used >= SRV_RING_TABLES)
        return 0;
    const u32 base = g.srv_ring_used++ * SRV_TABLE_SIZE;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    g.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(
        g.srv_heap, &cpu);
    cpu.ptr += (size_t)base * g.srv_step;
    g.dev->lpVtbl->CopyDescriptorsSimple(
        g.dev, 1, cpu, srv_cpu(SRV_DEPTH_SOURCE_BASE + zindex),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    cpu.ptr += g.srv_step;
    g.dev->lpVtbl->CopyDescriptorsSimple(
        g.dev, 1, cpu, srv_cpu(UAV_ZDEPTH_BASE + zindex),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    g.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(
        g.srv_heap, &gpu);
    gpu.ptr += (u64)base * g.srv_step;
    *out_source = gpu;
    gpu.ptr += g.srv_step;
    *out_destination = gpu;
    return 1;
}
static D3D12_CPU_DESCRIPTOR_HANDLE smp_cpu(u32 slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    g.smp_cpu_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.smp_cpu_heap, &h);
    h.ptr += (size_t)slot * g.smp_step;
    return h;
}
static u32 sampler_slot(const rsx_dsp_texture* t, u32 key)
{
    for (u32 i = 0; i < g.n_samplers; i++)
        if (g.smp_keys[i] == key) return 1 + i;
    if (g.n_samplers >= SMP_CACHE_SLOTS) return SMP_DEFAULT;
    D3D12_SAMPLER_DESC sd = decode_sampler(t);
    const u32 slot = 1 + g.n_samplers;
    g.dev->lpVtbl->CreateSampler(g.dev, &sd, smp_cpu(slot));
    g.smp_keys[g.n_samplers++] = key;
    return slot;
}
static D3D12_GPU_DESCRIPTOR_HANDLE sampler_table(const u32 slots[SMP_TABLE_SIZE])
{
    for (u32 i = 0; i < g.smp_ring_used; i++) {
        if (memcmp(g.smp_ring_slots[i], slots, sizeof(g.smp_ring_slots[i])) == 0) {
            D3D12_GPU_DESCRIPTOR_HANDLE cached;
            g.smp_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(
                g.smp_heap, &cached);
            cached.ptr += (u64)(i * SMP_TABLE_SIZE) * g.smp_step;
            return cached;
        }
    }
    /* The caller preflights capacity before recording render-state commands,
     * because a command-list flush here would discard that state. */
    if (g.smp_ring_used >= SMP_RING_TABLES) {
        D3D12_GPU_DESCRIPTOR_HANDLE invalid = {0};
        return invalid;
    }
    const u32 base = g.smp_ring_used++ * SMP_TABLE_SIZE;
    memcpy(g.smp_ring_slots[base / SMP_TABLE_SIZE], slots,
           sizeof(g.smp_ring_slots[0]));
    D3D12_CPU_DESCRIPTOR_HANDLE dst;
    D3D12_CPU_DESCRIPTOR_HANDLE src[SMP_TABLE_SIZE];
    const UINT dst_size = SMP_TABLE_SIZE;
    g.smp_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.smp_heap, &dst);
    dst.ptr += (size_t)base * g.smp_step;
    for (u32 i = 0; i < SMP_TABLE_SIZE; i++)
        src[i] = smp_cpu(slots[i]);
    g.dev->lpVtbl->CopyDescriptors(
        g.dev, 1, &dst, &dst_size, SMP_TABLE_SIZE, src, NULL,
        D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    D3D12_GPU_DESCRIPTOR_HANDLE h;
    g.smp_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(g.smp_heap, &h);
    h.ptr += (u64)base * g.smp_step;
    return h;
}
