#include "native_scene_jobs.h"
#include "native_scene_pose_job.h"
#include "spu_xfloat_simd.h"
#include "native_visibility.h"
#include "../libs/spurs/cellSpurs.h"
#include <string.h>

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static uint32_t be16(const uint8_t* p) { return ((uint32_t)p[0] << 8) | p[1]; }
static void write_be32(uint8_t* p,uint32_t value) {
    p[0]=(uint8_t)(value>>24);p[1]=(uint8_t)(value>>16);p[2]=(uint8_t)(value>>8);p[3]=(uint8_t)value;
}


typedef struct NativeVisibility {
    uint8_t camera[0x3100], matrix[64], bounds[128*64];
    yz_visibility_result result;
    uint32_t count, far_bits, flags_ea, depth_ea;
} NativeVisibility;
static _Thread_local NativeVisibility native_inputs;

static void execute_visibility(const cellSpursNativeJobRequest* job,void* user) {
    NativeVisibility* data=(NativeVisibility*)user;
    const yz_visibility_request request={data->matrix,data->bounds,data->camera,
        sizeof(data->camera),data->count,data->far_bits};
    spu_xfloat_simd_thread_enter();
    /* Prepare has validated every condition checked by the pure operation.
     * Inputs are private snapshots; no failure or fallback is possible after
     * publication. */
    yz_visibility_oriented(&request,&data->result);
    const uint32_t pc=job->slot-0x4c00;
    cellSpursNativeJobPublish(job,data->flags_ea,data->result.visible,
        (data->count+15)&~15u,pc+0xc36c,0x20);
    if(data->depth_ea) {
        uint8_t depths[512];
        for(unsigned i=0;i<128;++i)write_be32(depths+4*i,data->result.depth_bits[i]);
        cellSpursNativeJobPublish(job,data->depth_ea,depths,
            (data->count*4+15)&~15u,pc+0xc464,0x20);
    }
    uint8_t completed[128];memcpy(completed,job->descriptor,128);write_be32(completed+16,0);
    /* Synchronous result publications precede descriptor completion. */
    cellSpursNativeJobPublish(job,job->descriptor_ea,completed,128,pc+0xcf70,0x22);
}

static int prepare_visibility(const cellSpursNativeJobRequest* job,cellSpursNativeOperation* operation) {
    if(job->binary_ea!=0x01254500u ||
       job->binary_size!=0x9540 || job->image_id!=14 || job->descriptor_size!=128 ||
       (job->slot!=0x4c00 && job->slot!=0xe400)) return 0;
    const uint8_t* d=job->descriptor;
    if(be32(d+16)!=1||be32(d+20)!=0x4000||be16(d+10)!=16||be32(d+36)!=8||
       be32(d+80)!=9||be32(d+48)!=64||be32(d+64)!=0x3100) return 0;
    const uint32_t n=be32(d+84),bytes=be32(d+56);
    const uint32_t matrix_ea=be32(d+52),bounds_ea=be32(d+60),camera_ea=be32(d+68);
    const uint32_t flags_ea=be32(d+88),depth_ea=be32(d+112);
    if(n>128||bytes<n*64||bytes>0x3fc0||(bytes&15)||
       !matrix_ea||!bounds_ea||!camera_ea||((matrix_ea|bounds_ea|flags_ea|depth_ea)&15)||
       !cellSpursNativeJobRangeValid(bounds_ea,bytes)||
       !cellSpursNativeJobRangeValid(flags_ea,(n+15)&~15u)||
       (depth_ea&&!cellSpursNativeJobRangeValid(depth_ea,(n*4+15)&~15u))) return 0;
    NativeVisibility* data=&native_inputs;
    /* Acquire inputs in the job's original order, without a 256KB local
     * store. Only the operation's compact data is copied. */
    if(!cellSpursNativeJobAcquire(job,camera_ea,data->camera,sizeof(data->camera))||
       !cellSpursNativeJobAcquire(job,matrix_ea,data->matrix,sizeof(data->matrix))||
       !cellSpursNativeJobAcquire(job,bounds_ea,data->bounds,n*64)||
       be32(data->camera)>128) return 0;
    data->count=n;data->far_bits=be32(d+92);data->flags_ea=flags_ea;data->depth_ea=depth_ea;
    operation->execute=execute_visibility;operation->user=data;
    return 1;
}

static int prepare_scene_job(const cellSpursNativeJobRequest* job,cellSpursNativeOperation* operation) {
    if(yz_native_scene_pose_prepare(job,operation)) return 1;
    return prepare_visibility(job,operation);
}

void yz_native_scene_jobs_init(void) {
    g_cell_spurs_native_job_prepare=prepare_scene_job;
}
