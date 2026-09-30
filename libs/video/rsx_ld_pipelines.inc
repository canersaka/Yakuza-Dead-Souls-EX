/* Shader and pipeline-state caches.
 * Part of rsx_live_draw.c, which includes its parts once, in order. */

static int sampler_table_needs_flush(const u32 slots[SMP_TABLE_SIZE])
{
    if (g.smp_ring_used < SMP_RING_TABLES) return 0;
    for (u32 i = 0; i < g.smp_ring_used; i++)
        if (memcmp(g.smp_ring_slots[i], slots,
                   sizeof(g.smp_ring_slots[i])) == 0)
            return 0;
    return 1;
}

static u32 shader_disk_cache_stage_index(u32 stage)
{
    return stage == 'V' ? 0u : 1u;
}


/* On-disk shader cache entry: this header, the HLSL source, then the compiled
 * blob. A hit still compares the exact source bytes, never only the hash. */
#define SHADER_DISK_CACHE_MAGIC 0x43535A59u
/* Bump when generated shader semantics change. */
#define SHADER_DISK_CACHE_VERSION 2u
#define SHADER_DISK_CACHE_MAX_BLOB (4u * 1024u * 1024u)

typedef struct {
    uint32_t magic, version, stage, source_length, blob_length, reserved;
    uint64_t source_hash;
} shader_disk_cache_header;

static int shader_disk_cache_prepare(void)
{
    if (g_ld_shader_disk_ready >= 0)
        return g_ld_shader_disk_ready;
    g_ld_shader_disk_ready = 0;
    {
        CreateDirectoryA("scratch", NULL);
        snprintf(g_ld_shader_disk_dir, sizeof(g_ld_shader_disk_dir),
                 "scratch\\rsx_shader_cache_v%u",
                 SHADER_DISK_CACHE_VERSION);
    }

    if (!CreateDirectoryA(g_ld_shader_disk_dir, NULL) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        fprintf(stderr,
                "[shader-disk-cache] cannot create '%s' error=%lu\n",
                g_ld_shader_disk_dir, (unsigned long)GetLastError());
        return 0;
    }
    const DWORD attributes = GetFileAttributesA(g_ld_shader_disk_dir);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        !(attributes & FILE_ATTRIBUTE_DIRECTORY))
        return 0;

    g_ld_shader_disk_ready = 1;
    fprintf(stderr, "[shader-disk-cache] enabled dir='%s' version=%u\n",
            g_ld_shader_disk_dir, SHADER_DISK_CACHE_VERSION);
    return 1;
}

static int shader_disk_cache_path(
    char path[MAX_PATH], u32 stage, u64 hash, u32 source_length)
{
    if (!shader_disk_cache_prepare())
        return 0;
    const int length = snprintf(
        path, MAX_PATH, "%s\\%c_%016llX_%08X.bin",
        g_ld_shader_disk_dir, (char)stage,
        (unsigned long long)hash, source_length);
    return length > 0 && length < MAX_PATH;
}

static ID3DBlob* shader_disk_cache_load_impl(
    u32 stage, const char* source, u32 source_length, u64 hash)
{
    char path[MAX_PATH];
    const u32 stage_index = shader_disk_cache_stage_index(stage);
    if (!shader_disk_cache_path(path, stage, hash, source_length))
        return NULL;

    FILE* file = fopen(path, "rb");
    if (!file) {
        ++g_ld_shader_disk_misses[stage_index];
        return NULL;
    }

    shader_disk_cache_header header;
    memset(&header, 0, sizeof(header));
    int valid = fread(&header, sizeof(header), 1, file) == 1 &&
        header.magic == SHADER_DISK_CACHE_MAGIC &&
        header.version == SHADER_DISK_CACHE_VERSION &&
        header.stage == stage && header.source_hash == hash &&
        header.source_length == source_length && source_length <= 256u * 1024u &&
        header.blob_length > 0 &&
        header.blob_length <= SHADER_DISK_CACHE_MAX_BLOB;
    char* stored_source = NULL;
    ID3DBlob* blob = NULL;
    if (valid) {
        stored_source = (char*)malloc(source_length ? source_length : 1u);
        valid = stored_source != NULL &&
            fread(stored_source, 1, source_length, file) == source_length &&
            memcmp(stored_source, source, source_length) == 0;
    }
    if (valid) {
        valid = SUCCEEDED(D3DCreateBlob(header.blob_length, &blob)) && blob &&
            fread(blob->lpVtbl->GetBufferPointer(blob), 1,
                  header.blob_length, file) == header.blob_length;
    }
    free(stored_source);
    fclose(file);

    if (!valid) {
        if (blob)
            blob->lpVtbl->Release(blob);
        DeleteFileA(path);
        g_ld_shader_disk_rejects++;
        ++g_ld_shader_disk_misses[stage_index];
        return NULL;
    }

    ++g_ld_shader_disk_hits[stage_index];
    return blob;
}

static ID3DBlob* shader_disk_cache_load(
    u32 stage, const char* source, u32 source_length, u64 hash)
{
    ID3DBlob* result = shader_disk_cache_load_impl(stage, source, source_length, hash);
    return result;
}

static void shader_disk_cache_store(
    u32 stage, const char* source, u32 source_length, u64 hash,
    ID3DBlob* blob)
{
    if (!blob || source_length > 256u * 1024u)
        return;
    const size_t blob_length = blob->lpVtbl->GetBufferSize(blob);
    if (!blob_length || blob_length > SHADER_DISK_CACHE_MAX_BLOB)
        return;

    char path[MAX_PATH], temporary[MAX_PATH];
    if (!shader_disk_cache_path(path, stage, hash, source_length))
        return;
    const int temporary_length = snprintf(
        temporary, sizeof(temporary), "%s.tmp.%lu", path,
        (unsigned long)GetCurrentProcessId());
    if (temporary_length <= 0 || temporary_length >= sizeof(temporary))
        return;

    shader_disk_cache_header header;
    memset(&header, 0, sizeof(header));
    header.magic = SHADER_DISK_CACHE_MAGIC;
    header.version = SHADER_DISK_CACHE_VERSION;
    header.stage = stage;
    header.source_length = source_length;
    header.blob_length = (u32)blob_length;
    header.source_hash = hash;

    FILE* file = fopen(temporary, "wb");
    if (!file)
        return;
    const int wrote = fwrite(&header, sizeof(header), 1, file) == 1 &&
        fwrite(source, 1, source_length, file) == source_length &&
        fwrite(blob->lpVtbl->GetBufferPointer(blob), 1,
               blob_length, file) == blob_length &&
        fflush(file) == 0;
    fclose(file);
    if (!wrote || !MoveFileExA(
            temporary, path,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(temporary);
        return;
    }

    const u32 stage_index = shader_disk_cache_stage_index(stage);
    ++g_ld_shader_disk_writes[stage_index];
}

static u64 ld_hash_structural_render_state(
    const render_state_t* state, u64 hash)
{
#define LD_HASH_RENDER_FIELD(name) \
    hash = rsx_fnv1a(&state->name, sizeof(state->name), hash)
    /* Alpha payload is deliberately absent. Enable and compare mode still
     * select distinct shader/PSO behavior. Every other D3D12 render-state
     * field is listed explicitly so struct padding can never become identity. */
    LD_HASH_RENDER_FIELD(alpha_test_enable);
    LD_HASH_RENDER_FIELD(alpha_func);
    LD_HASH_RENDER_FIELD(blend_enable);
    LD_HASH_RENDER_FIELD(sf_rgb);
    LD_HASH_RENDER_FIELD(df_rgb);
    LD_HASH_RENDER_FIELD(sf_a);
    LD_HASH_RENDER_FIELD(df_a);
    LD_HASH_RENDER_FIELD(eq_rgb);
    LD_HASH_RENDER_FIELD(eq_a);
    LD_HASH_RENDER_FIELD(depth_test);
    LD_HASH_RENDER_FIELD(depth_write);
    LD_HASH_RENDER_FIELD(depth_func);
    LD_HASH_RENDER_FIELD(cull_enable);
    LD_HASH_RENDER_FIELD(cull_face);
    LD_HASH_RENDER_FIELD(front_face);
    LD_HASH_RENDER_FIELD(color_mask);
    LD_HASH_RENDER_FIELD(stencil_enable);
    LD_HASH_RENDER_FIELD(stencil_two_sided);
    LD_HASH_RENDER_FIELD(s_func);
    LD_HASH_RENDER_FIELD(s_func_mask);
    LD_HASH_RENDER_FIELD(s_write_mask);
    LD_HASH_RENDER_FIELD(s_fail);
    LD_HASH_RENDER_FIELD(s_zfail);
    LD_HASH_RENDER_FIELD(s_zpass);
    LD_HASH_RENDER_FIELD(bs_func);
    LD_HASH_RENDER_FIELD(bs_fail);
    LD_HASH_RENDER_FIELD(bs_zfail);
    LD_HASH_RENDER_FIELD(bs_zpass);
#undef LD_HASH_RENDER_FIELD
    return hash;
}

/*
 * PSO identity includes render state, but D3DCompile only consumes the final
 * generated shader text.  Keep those cache boundaries separate: one exact
 * HLSL program is compiled once and its bytecode is reused by every PSO state
 * variant that references it.  The source bytes are retained for collision-
 * safe equality; the 64-bit hash is only an index accelerator.
 */
static ID3DBlob* shader_blob_cache_find(
    shader_blob_cache_t* cache, const char* source, u32 source_length,
    u64 hash)
{
    for (u32 i = 0; i < cache->count; i++) {
        shader_blob_t* entry = &cache->entries[i];
        if (entry->hash != hash ||
            entry->source_length != source_length ||
            memcmp(entry->source, source, source_length) != 0)
            continue;
        entry->blob->lpVtbl->AddRef(entry->blob);
        return entry->blob;
    }
    return NULL;
}

typedef enum {
    SHADER_BLOB_INSERT_SKIPPED = 0,
    SHADER_BLOB_INSERTED,
    SHADER_BLOB_INSERT_FULL
} shader_blob_insert_result;

static shader_blob_insert_result shader_blob_cache_insert(
    shader_blob_cache_t* cache, const char* source, u32 source_length,
    u64 hash, ID3DBlob* blob)
{
    if (!blob)
        return SHADER_BLOB_INSERT_SKIPPED;
    if (cache->count >= MAX_SHADER_BLOBS)
        return SHADER_BLOB_INSERT_FULL;
    char* source_copy = (char*)malloc((size_t)source_length + 1u);
    if (!source_copy)
        return SHADER_BLOB_INSERT_SKIPPED;
    memcpy(source_copy, source, source_length);
    source_copy[source_length] = '\0';
    shader_blob_t* entry = &cache->entries[cache->count++];
    entry->hash = hash;
    entry->source_length = source_length;
    entry->source = source_copy;
    entry->blob = blob;
    blob->lpVtbl->AddRef(blob);
    cache->retained_source_bytes += source_length;
    cache->retained_blob_bytes += blob->lpVtbl->GetBufferSize(blob);
    return SHADER_BLOB_INSERTED;
}


static void shader_blob_cache_release(shader_blob_cache_t* cache)
{
    for (u32 i = 0; i < cache->count; i++) {
        shader_blob_t* entry = &cache->entries[i];
        if (entry->blob)
            entry->blob->lpVtbl->Release(entry->blob);
        free(entry->source);
    }
    memset(cache, 0, sizeof(*cache));
}


static u32 vp_txl_unit_mask(const u8* ucode, u32 instrs)
{
    u32 mask = 0;
    for (u32 i = 0; i < instrs; i++) {
        const u8* p = ucode + i * 16;
        const u32 d1 = (u32)p[4] | ((u32)p[5] << 8) |
                       ((u32)p[6] << 16) | ((u32)p[7] << 24);
        if (((d1 >> 22) & 0x1Fu) != 0x19u) continue;
        const u32 d2 = (u32)p[8] | ((u32)p[9] << 8) |
                       ((u32)p[10] << 16) | ((u32)p[11] << 24);
        mask |= 1u << ((d2 >> 8) & 3u);
    }
    return mask;
}

static ID3D12PipelineState* build_pso_impl(
    const char* vs_hlsl, const char* ps_hlsl, const render_state_t* rs,
    const rsx_vertex_layout_plan* masked_layout, int packed_offsets)
{
    ID3DBlob *vs = NULL, *ps = NULL, *err = NULL;
    static u32 compile_fail_logs = 0;
    static u32 create_fail_logs = 0;
    const u32 vs_length = (u32)strlen(vs_hlsl);
    const u32 ps_length = (u32)strlen(ps_hlsl);
    const u64 vs_hash = rsx_fnv1a(
        vs_hlsl, vs_length, RSX_FNV64_OFFSET);
    const u64 ps_hash = rsx_fnv1a(
        ps_hlsl, ps_length, RSX_FNV64_OFFSET);
    vs = shader_blob_cache_find(
        &g.vs_blobs, vs_hlsl, vs_length, vs_hash);
    HRESULT vs_hr = S_OK;
    if (!vs) {
        vs = shader_disk_cache_load('V', vs_hlsl, vs_length, vs_hash);
        if (vs) {
            shader_blob_cache_insert(
                &g.vs_blobs, vs_hlsl, vs_length, vs_hash, vs);
        } else {
            vs_hr = D3DCompile(
                vs_hlsl, vs_length, "xvs", NULL, NULL, "main",
                "vs_5_0", 0, 0, &vs, &err);
            if (SUCCEEDED(vs_hr)) {
                shader_disk_cache_store(
                    'V', vs_hlsl, vs_length, vs_hash, vs);
                shader_blob_cache_insert(
                    &g.vs_blobs, vs_hlsl, vs_length, vs_hash, vs);
            }
        }
    }
    if (FAILED(vs_hr)) {
        if (compile_fail_logs++ < 32) {
            const char* msg = err ? (const char*)err->lpVtbl->GetBufferPointer(err)
                                  : "no compiler diagnostic";
            fprintf(stderr, "[pso-fail] vertex compile: %.768s\n", msg);
        }
        if (err) err->lpVtbl->Release(err); return NULL;
    }
    if (err) {
        err->lpVtbl->Release(err);
        err = NULL;
    }
    ps = shader_blob_cache_find(
        &g.ps_blobs, ps_hlsl, ps_length, ps_hash);
    HRESULT ps_hr = S_OK;
    if (!ps) {
        ps = shader_disk_cache_load('P', ps_hlsl, ps_length, ps_hash);
        if (ps) {
            shader_blob_cache_insert(
                &g.ps_blobs, ps_hlsl, ps_length, ps_hash, ps);
        } else {
            ps_hr = D3DCompile(
                ps_hlsl, ps_length, "xps", NULL, NULL, "main",
                "ps_5_0", 0, 0, &ps, &err);
            if (SUCCEEDED(ps_hr)) {
                shader_disk_cache_store(
                    'P', ps_hlsl, ps_length, ps_hash, ps);
                shader_blob_cache_insert(
                    &g.ps_blobs, ps_hlsl, ps_length, ps_hash, ps);
            }
        }
    }
    if (FAILED(ps_hr)) {
        if (compile_fail_logs++ < 32) {
            const char* msg = err ? (const char*)err->lpVtbl->GetBufferPointer(err)
                                  : "no compiler diagnostic";
            fprintf(stderr, "[pso-fail] pixel compile: %.768s\n", msg);
        }
        if (err) err->lpVtbl->Release(err);
        vs->lpVtbl->Release(vs); return NULL;
    }
    if (err) {
        err->lpVtbl->Release(err);
        err = NULL;
    }
    D3D12_INPUT_ELEMENT_DESC il[16];
    const u32 input_count =
        masked_layout ? masked_layout->count : RSX_DSP_NUM_VERTEX_ATTR;
    for (u32 slot = 0; slot < input_count; slot++) {
        const u32 attr =
            masked_layout ? masked_layout->attrs[slot] : slot;
        D3D12_INPUT_ELEMENT_DESC e = {
            "ATTR", attr, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
            masked_layout && !packed_offsets ? attr * 16u : slot * 16u,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0
        };
        il[slot] = e;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = g.rootsig_x;
    pd.VS.pShaderBytecode = vs->lpVtbl->GetBufferPointer(vs);
    pd.VS.BytecodeLength = vs->lpVtbl->GetBufferSize(vs);
    pd.PS.pShaderBytecode = ps->lpVtbl->GetBufferPointer(ps);
    pd.PS.BytecodeLength = ps->lpVtbl->GetBufferSize(ps);
    pd.InputLayout.pInputElementDescs = input_count ? il : NULL;
    pd.InputLayout.NumElements = input_count;
    apply_render_state(&pd, rs);
    pd.SampleMask = 0xFFFFFFFFu;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = NULL;
    HRESULT hr = g.dev->lpVtbl->CreateGraphicsPipelineState(
        g.dev, &pd, &IID_ID3D12PipelineState, (void**)&pso);
    if (FAILED(hr)) {
        const HRESULT removed = g.dev->lpVtbl->GetDeviceRemovedReason(g.dev);
        if (create_fail_logs++ < 32)
            fprintf(stderr,
                    "[pso-fail] create hr=0x%08lX removed=0x%08lX "
                    "depth{test=%u write=%u func=%u} blend=%u "
                    "cull{enable=%u face=%u}\n",
                    (unsigned long)hr, (unsigned long)removed,
                    rs->depth_test, rs->depth_write, rs->depth_func,
                    rs->blend_enable, rs->cull_enable, rs->cull_face);
        if (removed != S_OK)
            ld_dump_dred("CreateGraphicsPipelineState", hr);
    }
    vs->lpVtbl->Release(vs); ps->lpVtbl->Release(ps);
    return SUCCEEDED(hr) ? pso : NULL;
}

static ID3D12PipelineState* build_pso(
    const char* vs_hlsl, const char* ps_hlsl, const render_state_t* rs,
    const rsx_vertex_layout_plan* masked_layout, int packed_offsets)
{
    ID3D12PipelineState* result = build_pso_impl(
        vs_hlsl, ps_hlsl, rs, masked_layout, packed_offsets);
    return result;
}

static int ld_current_vertex_layout(rsx_vertex_layout_plan* layout)
{
    const u32 start = rsx_dsp_vp_start(&g.rsx);
    if (start >= RSX_DSP_VP_INSTR) {
        ld_layout_plan_get(0xFFFFu, layout);
        return 0;
    }
    const u8* vp_uc = (const u8*)(g.rsx.vp + start * 4);
    if (g_ld_use_vp_metadata) {
        const vp_metadata metadata = vp_metadata_lookup(
            &g_ld_vp_metadata, vp_uc, (RSX_DSP_VP_INSTR - start) * 16);
        ld_layout_plan_get(metadata.instructions ? metadata.input_mask : 0xFFFFu, layout);
        return metadata.instructions != 0;
    }
    const u32 vp_instrs = rsx_vp_program_size_instrs(
        vp_uc, (RSX_DSP_VP_INSTR - start) * 16);
    rsx_vp_input_analysis analysis = {0xFFFFu, 0};
    if (!vp_instrs ||
        rsx_vp_analyze_inputs(
            vp_uc, vp_instrs * 16, &analysis) != (int)vp_instrs ||
        !analysis.exact)
        analysis.input_mask = 0xFFFFu;
    ld_layout_plan_get(analysis.input_mask, layout);
    return vp_instrs != 0;
}

static ID3D12PipelineState* get_pso_uncached(
    const rsx_vertex_layout_plan* masked_layout, int packed_payload)
{
    memset(&g_ld_current_pso, 0, sizeof(g_ld_current_pso));
    const u32 start = rsx_dsp_vp_start(&g.rsx);
    if (start >= RSX_DSP_VP_INSTR) return NULL;
    const u8* vp_uc = (const u8*)(g.rsx.vp + start * 4);
    vp_metadata metadata = {0};
    if (g_ld_use_vp_metadata)
        metadata = vp_metadata_lookup(
            &g_ld_vp_metadata, vp_uc, (RSX_DSP_VP_INSTR - start) * 16);
    const u32 vp_instrs = g_ld_use_vp_metadata ? metadata.instructions :
        rsx_vp_program_size_instrs(vp_uc, (RSX_DSP_VP_INSTR - start) * 16);
    if (!vp_instrs) return NULL;

    u32 fp_loc = 0;
    const u32 fp_off = rsx_dsp_fragment_program(&g.rsx, &fp_loc);
    const u8* fp_uc = ld_fragment_bytes(fp_loc, fp_off, 16);
    if (!fp_uc) return NULL;
    const u32 fp_size = rsx_fp_program_size(fp_uc, 0x10000);
    if (!fp_size) return NULL;
    /* re-resolve with the true size to validate the whole program is mapped */
    fp_uc = ld_fragment_bytes(fp_loc, fp_off, fp_size);
    if (!fp_uc) return NULL;

    /* The fragment output register file (fp16 h0 vs fp32 r0) is chosen by
     * SHADER_CONTROL bit 0x40, so that bit is part of the PSO cache key: a
     * program reused under a different export mode gets its own PSO. */
    const u32 fp_ctrl = rsx_dsp_shader_control(&g.rsx);
    u32 cube_mask = 0;
    for (u32 unit = 0; unit < RSX_DSP_NUM_TEXTURES; unit++) {
        rsx_dsp_texture texture;
        rsx_dsp_get_texture(&g.rsx, unit, &texture);
        if (texture.enabled && texture.cubemap)
            cube_mask |= 1u << unit;
    }
    const u32 vtex_mask = vertex_texture_mask();
    const u32 txl_mask = g_ld_use_vp_metadata ? metadata.txl_mask :
        vp_txl_unit_mask(vp_uc, vp_instrs);

    render_state_t rs;
    decode_render_state(&rs);

    if (rsx_fp_collect_constants(
            fp_uc, fp_size, &g.fp_constants) < 0)
        return NULL;
    g.fp_alpha_ref = rsx_fp_alpha_ref(
        rs.alpha_ref_raw, rs.alpha_ref_format);

    const u64 vp_hash = g_ld_use_vp_metadata ? metadata.hash : rsx_fnv1a(
        vp_uc, vp_instrs * 16, RSX_FNV64_OFFSET);
    u64 key = vp_hash;
    /* Branch destinations are absolute instruction slots, so the same bytes
     * loaded at another start slot may generate different control flow. */
    key = rsx_fnv1a(&start, sizeof(start), key);
    key = rsx_fp_structural_hash(fp_uc, fp_size, key);
    if (!key)
        return NULL;
    const u32 fp_ctrl_key = fp_ctrl & 0x40u;
    key = rsx_fnv1a(&fp_ctrl_key, sizeof(fp_ctrl_key), key);
    key = rsx_fnv1a(&cube_mask, sizeof(cube_mask), key);
    key = rsx_fnv1a(&vtex_mask, sizeof(vtex_mask), key);
    if (masked_layout) {
        static const u32 masked_layout_tag = 0x314B534Du; /* "MSK1" */
        const u32 payload_stride =
            packed_payload ? masked_layout->stride : VERT_STRIDE;
        key = rsx_fnv1a(
            &masked_layout_tag, sizeof(masked_layout_tag), key);
        key = rsx_fnv1a(
            &masked_layout->mask, sizeof(masked_layout->mask), key);
        key = rsx_fnv1a(
            &payload_stride, sizeof(payload_stride), key);
        key = rsx_fnv1a(
            &packed_payload, sizeof(packed_payload), key);
    }
    key = ld_hash_structural_render_state(&rs, key);
    g_ld_current_pso.valid = 1;
    g_ld_current_pso.key = key;
    /* vp_uc is unchanged while this serialized draw prepares its key. */
    g_ld_current_pso.vp_hash = (g_ld_reuse_vp_hash || g_ld_use_vp_metadata) ? vp_hash : rsx_fnv1a(
        vp_uc, vp_instrs * 16, RSX_FNV64_OFFSET);
    g_ld_current_pso.vp_start = start;
    g_ld_current_pso.vp_instrs = vp_instrs;
    g_ld_current_pso.fp_location = fp_loc;
    g_ld_current_pso.fp_offset = fp_off;
    g_ld_current_pso.fp_size = fp_size;
    g_ld_current_pso.fp_control = fp_ctrl;
    g_ld_current_pso.cube_mask = cube_mask;
    g_ld_current_pso.vtex_mask = vtex_mask;
    g_ld_current_pso.txl_mask = txl_mask;
    g_ld_current_pso.input_mask =
        masked_layout ? masked_layout->mask : 0xFFFFu;
    g_ld_current_pso.input_stride =
        masked_layout && packed_payload
            ? masked_layout->stride : VERT_STRIDE;
    g_ld_current_pso.packed_offsets =
        masked_layout && packed_payload;

    for (u32 i = 0; i < g.n_psos; i++) {
        if (g.psos[i].key == key) {
            return g.psos[i].pso;
        }
    }
    if (g.n_psos >= MAX_PSOS) {
        return NULL;
    }

    static char vs_hlsl[256 * 1024];
    static char ps_hlsl[256 * 1024];
    ID3D12PipelineState* pso = NULL;
    const int vi = masked_layout
        ? rsx_vp_decompile_compact_control_ex(
            vp_uc, vp_instrs * 16, vtex_mask, masked_layout->mask,
            start, vs_hlsl, sizeof(vs_hlsl))
        : rsx_vp_decompile_control_ex(
            vp_uc, vp_instrs * 16, vtex_mask, start, vs_hlsl, sizeof(vs_hlsl));
    u32 decompiled_constant_count = 0;
    int fi;
    fi = rsx_fp_decompile_buffered_ex(
        fp_uc, fp_size, fp_ctrl, cube_mask, ps_hlsl,
        sizeof(ps_hlsl), &decompiled_constant_count);
    if (fi > 0 &&
        decompiled_constant_count != g.fp_constants.count)
        fi = -1;
    if (fi > 0 && rs.alpha_test_enable &&
        rsx_fp_apply_alpha_test_buffered(
            ps_hlsl, sizeof(ps_hlsl), rs.alpha_func) < 0)
        fi = -1;
    if (vi > 0 && fi > 0)
        pso = build_pso(
            vs_hlsl, ps_hlsl, &rs, masked_layout, packed_payload);
    else {
        static u32 decompile_fail_logs;
        if (decompile_fail_logs++ < 16u)
            fprintf(stderr,
                    "[pso-fail] shader translation: vertex=%d pixel=%d "
                    "vp_start=%u vp_instructions=%u txl=0x%X bound_vtex=0x%X\n",
                    vi, fi, start, vp_instrs, txl_mask, vtex_mask);
    }

    g.psos[g.n_psos].key = key;
    g.psos[g.n_psos].pso = pso;
    g.n_psos++;
    return pso;
}

#include "rsx_pipeline_records.h"

static int ld_upload_pixel_constants(u32* out_offset)
{
    if (out_offset)
        *out_offset = 0;
    if (!g.ps_cb || !g.ps_cb_mapped)
        return 0;

    /* HLSL uses a one-element placeholder array for programs with no inline
     * constants so fp_alpha retains a valid 16-byte slot. */
    const u32 value_slots =
        g.fp_constants.count ? g.fp_constants.count : 1u;
    const u32 data_bytes = (value_slots + 1u) * 16u;
    u32 offset = 0;
    u32 allocation_bytes = 0;
    int plan = rsx_fp_constant_ring_plan(
        g.ps_cb_used, PS_CB_RING_BYTES, data_bytes,
        &offset, &allocation_bytes);
    if (plan < 0) {
        return 0;
    }
    if (plan == 0) {
        ld_flush(LD_FLUSH_PIXEL_CONSTANT_RING);
        if (!g.ready)
            return 0;
        g.ps_cb_used = 0;
        plan = rsx_fp_constant_ring_plan(
            g.ps_cb_used, PS_CB_RING_BYTES, data_bytes,
            &offset, &allocation_bytes);
        if (plan != 1) {
            return 0;
        }
    }

    u8* destination = g.ps_cb_mapped + offset;
    memset(destination, 0, data_bytes);
    if (g.fp_constants.count)
        memcpy(
            destination, g.fp_constants.values,
            (size_t)g.fp_constants.count * 16u);
    memcpy(
        destination + value_slots * 16u,
        &g.fp_alpha_ref, sizeof(g.fp_alpha_ref));
    g.ps_cb_used += allocation_bytes;
    if (out_offset)
        *out_offset = offset;
    return 1;
}
