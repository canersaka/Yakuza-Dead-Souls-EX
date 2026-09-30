/* Cache of resolved pipeline states keyed by exact register state; each entry
 * snapshots the vertex and fragment program bytes and is validated
 * byte-for-byte on every hit. */
#define LD_PIPELINE_RECORD_COUNT 4096u
#define LD_PIPELINE_RECORD_BUDGET (64u*1024u*1024u)
typedef struct {
    render_state_t state;
    u32 vp_start,fp_location,fp_offset,fp_control,cube_mask,vtex_mask;
    u32 masked,input_mask,input_stride,packed;
} ld_pipeline_key;
typedef struct {
    ld_pipeline_key key;
    ld_pso_metadata metadata;
    ID3D12PipelineState* pso;
    rsx_fp_constant_layout* payload;
    u64 hash,age;
    u32 allocation;
} ld_pipeline_record;
static ld_pipeline_record g_ld_pipeline_records[LD_PIPELINE_RECORD_COUNT];
static u64 g_ld_pipeline_age;
static u32 g_ld_pipeline_bytes;
static void ld_pipeline_records_reset(void)
{
    for(u32 i=0;i<LD_PIPELINE_RECORD_COUNT;i++)free(g_ld_pipeline_records[i].payload);
    memset(g_ld_pipeline_records,0,sizeof(g_ld_pipeline_records));
    g_ld_pipeline_bytes=0;g_ld_pipeline_age=0;
}
static ID3D12PipelineState* get_pso(const rsx_vertex_layout_plan* layout,int packed)
{
    ld_pipeline_key key;memset(&key,0,sizeof(key));
    decode_render_state(&key.state);
    const float alpha=rsx_fp_alpha_ref(key.state.alpha_ref_raw,key.state.alpha_ref_format);
    key.state.alpha_ref_raw=key.state.alpha_ref_format=0;
    key.vp_start=rsx_dsp_vp_start(&g.rsx);
    key.fp_offset=rsx_dsp_fragment_program(&g.rsx,&key.fp_location);
    key.fp_control=rsx_dsp_shader_control(&g.rsx);
    for(u32 u=0;u<RSX_DSP_NUM_TEXTURES;u++){
        rsx_dsp_texture t;rsx_dsp_get_texture(&g.rsx,u,&t);
        if(t.enabled&&t.cubemap)key.cube_mask|=1u<<u;
    }
    key.vtex_mask=vertex_texture_mask();
    key.masked=layout!=NULL;key.input_mask=layout?layout->mask:0xFFFFu;
    key.input_stride=layout&&packed?layout->stride:VERT_STRIDE;
    key.packed=layout&&packed;
    if(key.vp_start>=RSX_DSP_VP_INSTR)return get_pso_uncached(layout,packed);
    const u64 hash=rsx_fnv1a(&key,sizeof(key),RSX_FNV64_OFFSET);
    const u32 base=(u32)(hash^(hash>>32))&((LD_PIPELINE_RECORD_COUNT/4)-1u);
    ld_pipeline_record* chosen=&g_ld_pipeline_records[base*4];
    for(u32 way=0;way<4;way++){
        ld_pipeline_record* r=&g_ld_pipeline_records[base*4+way];
        if(!r->payload||r->age<chosen->age)chosen=r;
        if(!r->payload||r->hash!=hash||memcmp(&key,&r->key,sizeof(key)))continue;
        const u32 vp_bytes=r->metadata.vp_instrs*16;
        const u8* vp_snapshot=(const u8*)(r->payload+1);
        const u8* fp_snapshot=vp_snapshot+vp_bytes;
        const u8* vp_source=(const u8*)(g.rsx.vp+key.vp_start*4);
        const u8* fp_source=guest_ptr(key.fp_location,key.fp_offset,r->metadata.fp_size);
        if(fp_source && !memcmp(vp_source,vp_snapshot,vp_bytes) &&
            rsx_fp_structure_matches(fp_source,fp_snapshot,r->metadata.fp_size,r->payload) &&
            rsx_fp_refresh_constants(fp_source,r->metadata.fp_size,r->payload,&g.fp_constants)>=0){
            r->age=++g_ld_pipeline_age;
            g_ld_current_pso=r->metadata;g.fp_alpha_ref=alpha;
            return r->pso;
        }
        chosen=r;break;
    }
    ID3D12PipelineState* pso=get_pso_uncached(layout,packed);
    if(!pso||!g_ld_current_pso.valid)return pso;
    const ld_pso_metadata metadata=g_ld_current_pso;
    const u32 vp_bytes=metadata.vp_instrs*16,fp_bytes=metadata.fp_size;
    const u8* source=guest_ptr(metadata.fp_location,metadata.fp_offset,fp_bytes);
    if(!source||vp_bytes>(RSX_DSP_VP_INSTR-key.vp_start)*16u||fp_bytes>0x10000u)return pso;
    const u32 allocation=(u32)sizeof(rsx_fp_constant_layout)+vp_bytes+fp_bytes;
    if(allocation>LD_PIPELINE_RECORD_BUDGET-(g_ld_pipeline_bytes-chosen->allocation))return pso;
    rsx_fp_constant_layout* payload=malloc(allocation);
    if(!payload)return pso;
    u8* vp_snapshot=(u8*)(payload+1);u8* fp_snapshot=vp_snapshot+vp_bytes;
    memcpy(vp_snapshot,g.rsx.vp+key.vp_start*4,vp_bytes);memcpy(fp_snapshot,source,fp_bytes);
    if(rsx_fp_plan_constants(fp_snapshot,fp_bytes,payload)<0 || payload->bytes!=fp_bytes ||
        payload->count!=g.fp_constants.count){free(payload);return pso;}
    g_ld_pipeline_bytes-=chosen->allocation;free(chosen->payload);
    chosen->key=key;chosen->metadata=metadata;chosen->pso=pso;
    chosen->payload=payload;chosen->allocation=allocation;
    chosen->hash=hash;chosen->age=++g_ld_pipeline_age;g_ld_pipeline_bytes+=allocation;
    return pso;
}
