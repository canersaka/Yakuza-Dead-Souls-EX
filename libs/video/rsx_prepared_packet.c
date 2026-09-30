#include "rsx_prepared_packet.h"
#include "rsx_upload_packet.h"
#include <stdlib.h>
#include <string.h>

enum { R_REGISTER, R_PROGRAM, R_CONSTANT, R_ACTION };
typedef struct { u8 allowed, retained; } method_program;
typedef struct { u32 header; u16 first, count; u8 step, valid, supported, action; } packet_program;
#define PACKET_SLOTS 1024u
struct rsx_prepared_packet_cache {
    method_program methods[0x2000/4];
    packet_program packets[PACKET_SLOTS];
    rsx_prepared_packet_stats stats;
};
/* Register-state methods a packet may carry although the dispatcher itself
 * treats them as plain register writes. */
static const u32 k_state_methods[] = {
    NV4097_SET_SURFACE_FORMAT, NV4097_SET_SURFACE_CLIP_HORIZONTAL,
    NV4097_SET_SURFACE_CLIP_VERTICAL, NV4097_SET_SURFACE_COLOR_AOFFSET,
    NV4097_SET_SURFACE_COLOR_BOFFSET, NV4097_SET_SURFACE_COLOR_COFFSET,
    NV4097_SET_SURFACE_COLOR_DOFFSET, NV4097_SET_SURFACE_ZETA_OFFSET,
    NV4097_SET_SURFACE_COLOR_TARGET, NV4097_SET_SURFACE_PITCH_A,
    NV4097_SET_SURFACE_PITCH_B, NV4097_SET_SURFACE_PITCH_C,
    NV4097_SET_SURFACE_PITCH_D, NV4097_SET_SURFACE_PITCH_Z,
    NV4097_SET_VIEWPORT_HORIZONTAL, NV4097_SET_VIEWPORT_VERTICAL,
    NV4097_SET_SCISSOR_HORIZONTAL, NV4097_SET_SCISSOR_VERTICAL,
    NV4097_SET_COLOR_CLEAR_VALUE, NV4097_SET_ZSTENCIL_CLEAR_VALUE,
    NV4097_SET_COLOR_MASK, NV4097_SET_ALPHA_TEST_ENABLE, NV4097_SET_ALPHA_FUNC,
    NV4097_SET_ALPHA_REF, NV4097_SET_BLEND_ENABLE, NV4097_SET_BLEND_FUNC_SFACTOR,
    NV4097_SET_BLEND_FUNC_DFACTOR, NV4097_SET_BLEND_EQUATION,
    NV4097_SET_DEPTH_TEST_ENABLE, NV4097_SET_DEPTH_FUNC, NV4097_SET_DEPTH_MASK,
    NV4097_SET_STENCIL_TEST_ENABLE, NV4097_SET_STENCIL_FUNC,
    NV4097_SET_STENCIL_FUNC_REF, NV4097_SET_STENCIL_FUNC_MASK,
    NV4097_SET_STENCIL_OP_FAIL, NV4097_SET_STENCIL_OP_ZFAIL,
    NV4097_SET_STENCIL_OP_ZPASS, NV4097_SET_CULL_FACE_ENABLE, NV4097_SET_CULL_FACE,
    NV4097_SET_FRONT_FACE, NV4097_SET_CONTEXT_DMA_REPORT,
    NV4097_SET_TRANSFORM_PROGRAM_LOAD, NV4097_SET_TRANSFORM_CONSTANT_LOAD,
    NV4097_SET_SHADER_PROGRAM, NV4097_SET_VERTEX_ATTRIB_OUTPUT_MASK,
};
rsx_prepared_packet_cache* rsx_prepared_packet_create(void)
{
    rsx_prepared_packet_cache* c=calloc(1,sizeof(*c));
    rsx_dispatch* classification=malloc(sizeof(*classification));
    if (!c || !classification) {free(c);free(classification);return NULL;}
    rsx_dispatch_init(classification,NULL);
    for(u32 i=0;i<0x2000/4;i++)
        c->methods[i].allowed=classification->klass[i]!=RSX_DSP_CLASS_TODO;
    free(classification);
    for(u32 i=0;i<sizeof(k_state_methods)/sizeof(k_state_methods[0]);i++)
        c->methods[k_state_methods[i]/4].allowed=1;
    for(u32 m=0x1F00;m<0x2000;m+=4)c->methods[m/4].allowed=1;          /* transform constants */
    for(u32 i=0;i<16;i++) {
        c->methods[(0x1740+i*4)/4].allowed=1;                           /* vertex attribute format */
        c->methods[(0x1680+i*4)/4].allowed=1;                           /* vertex attribute offset */
        for(u32 j=0;j<8;j++)c->methods[(0x1A00+i*32+j*4)/4].allowed=1;  /* texture state */
    }
    for(u32 m=0xB80;m<0xC00;m+=4)c->methods[m/4].retained=R_PROGRAM;
    for(u32 m=0x1F00;m<0x1F80;m+=4)c->methods[m/4].retained=R_CONSTANT;
    const u32 actions[]={0x1808,0x1814,0x1824,0x1D94};
    for(u32 i=0;i<sizeof(actions)/sizeof(actions[0]);i++)c->methods[actions[i]/4].retained=R_ACTION;
    return c;
}
void rsx_prepared_packet_destroy(rsx_prepared_packet_cache* c){free(c);}
void rsx_prepared_packet_get_stats(const rsx_prepared_packet_cache* c,rsx_prepared_packet_stats* out)
{if(out){if(c)*out=c->stats;else memset(out,0,sizeof(*out));}}
static packet_program* prepare(rsx_prepared_packet_cache* c,u32 header)
{
    if(!c)return NULL;
    packet_program* p=&c->packets[((header^(header>>18))*2654435761u)&(PACKET_SLOTS-1)];
    if(p->valid&&p->header==header){++c->stats.hits;return p->supported?p:NULL;}
    *p=(packet_program){0};p->header=header;p->valid=1;++c->stats.compiled;
    /* Include the full subchannel/reserved field in validation; a header match
     * is exact identity, never a guest-address or content-hash assumption. */
    if(header&0xA0030003u)return NULL;
    const u32 count=(header>>18)&0x7FFu,method=header&0x3FFFCu;
    const u32 step=(header&0x40000000u)?0:1;
    if(!count){p->supported=1;return p;}
    if(method>=0x2000u||method/4+(count-1)*step>=0x2000/4)return NULL;
    for(u32 i=0;i<count;i++) {
        const method_program* m=&c->methods[method/4+i*step];
        if(!m->allowed)return NULL;
        if(m->retained==R_ACTION){if(count!=1)return NULL;p->action=1;}
    }
    p->first=(u16)(method/4);p->count=(u16)count;p->step=(u8)step;p->supported=1;
    return p;
}
int rsx_prepared_packet_supported(rsx_prepared_packet_cache* c,u32 header)
{return prepare(c,header)!=NULL;}
int rsx_prepared_packet_execute_graphics(rsx_prepared_packet_cache* c,
    rsx_dispatch* r,rsx_state* protocol,u32 header,const u32* words,u64* counter)
{
    const packet_program* p=prepare(c,header);
    if(!p||!r||(p->count&&!words)){if(c)++c->stats.refused;return 0;}
    ++c->stats.packets;c->stats.methods+=p->count;
    if(counter)*counter+=p->count;
    if(!p->count)return 1;
    const u32 method=p->first*4;
    if(rsx_upload_packet_supported(method,p->count,!p->step))
        return rsx_dispatch_upload_packet(r,method,words,p->count,!p->step);
    if(p->action){
        ++c->stats.actions;rsx_dispatch_method(r,method,words[0]);return 1;
    }
    /* This is the authoritative register file, not a second command decoder.
     * Mixed load/upload packets preserve intermediate load-pointer effects. */
    for(u32 i=0,reg=p->first;i<p->count;i++,reg+=p->step){
        const method_program* m=&c->methods[reg];const u32 value=words[i];
        ++r->seen[reg];r->regs[reg]=value;
        if(reg==0x1e98u/4)rsx_dispatch_render_condition(r,value);
        if(m->retained==R_PROGRAM){
            u32* load=&r->regs[0x1E9C/4];const u32 lane=reg&3,pos=*load*4+lane;
            if(pos<RSX_DSP_VP_WORDS)r->vp[pos]=value;
            if(lane==3)++*load;
        }else if(m->retained==R_CONSTANT){
            const u32 word=reg-0x1F00/4,slot=r->regs[0x1EFC/4]+word/4;
            if(slot<RSX_DSP_NUM_CONSTANTS)r->constants[slot][word&3]=value;
        }
        if(protocol&&reg==NV4097_SET_CONTEXT_DMA_REPORT/4)
            protocol->context_dma_report=value;
    }
    return 1;
}
