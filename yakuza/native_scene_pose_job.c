#include "native_scene_pose_job.h"
#include "native_pose_generate.h"
#include <string.h>

enum { MAX_BONES=123 };
typedef struct NativePose {
    uint8_t world[64],bones[MAX_BONES*64],model[MAX_BONES*64];
    uint8_t result[MAX_BONES*48],completed[128];
    uint32_t count,output_ea;
} NativePose;
static _Thread_local NativePose inputs;
static uint32_t be32(const uint8_t* p)
{ return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3]; }
static uint32_t be16(const uint8_t* p) { return (uint32_t)p[0]<<8|p[1]; }

static void execute_pose(const cellSpursNativeJobRequest* job,void* user)
{
    NativePose* data=user;
    /* Prepare has validated all numerical bounds and the private storage is
     * disjoint. No guest input is reread after preparation and no fallible
     * work remains. */
    yz_pose_generate_spu48(data->result,sizeof(data->result),data->world,64,
        data->bones,sizeof(data->bones),data->model,sizeof(data->model),data->count);
    const uint32_t relocation=job->slot-0x4c00;
    cellSpursNativeJobPublish(job,data->output_ea,data->result,data->count*48,relocation+0xd32c,0x20);
    cellSpursNativeJobPublish(job,job->descriptor_ea,data->completed,128,relocation+0xcf70,0x22);
}

int yz_native_scene_pose_prepare(const cellSpursNativeJobRequest* job,cellSpursNativeOperation* operation)
{
    if(!job || !operation || !job->descriptor || job->binary_ea!=0x01254500 ||
       job->binary_size!=0x9540 || job->image_id!=14 || job->descriptor_size!=128 ||
       (job->slot!=0x4c00 && job->slot!=0xe400)) return 0;
    const uint8_t* d=job->descriptor;
    if(be32(d)!=0 || be32(d+4)!=job->binary_ea || be16(d+8)!=0x954 ||
       be16(d+10)!=24 || be32(d+12)!=0 || be32(d+16)!=4 || be32(d+20)!=0x8040 ||
       be32(d+48)!=64) return 0;
    /* Only this exact descriptor layout is handled. Additional cached inputs,
     * output/scratch allocations or user fields require a new contract. */
    for(unsigned at=24;at<48;at+=4) if(be32(d+at)) return 0;
    if(be32(d+72) || be32(d+76)) return 0;
    for(unsigned at=88;at<128;at+=4) if(be32(d+at)) return 0;
    const uint32_t count=be32(d+84),world=be32(d+52),bones=be32(d+60),model=be32(d+68),output=be32(d+80);
    if(count<8 || count>MAX_BONES || be32(d+56)!=count*64 || be32(d+64)!=count*64 ||
       !world || !bones || !model || !output || !job->descriptor_ea ||
       ((world|bones|model|output|job->descriptor_ea)&15) ||
       !cellSpursNativeJobRangeValid(world,64) || !cellSpursNativeJobRangeValid(bones,count*64) ||
       !cellSpursNativeJobRangeValid(model,count*64) || !cellSpursNativeJobRangeValid(output,count*48) ||
       !cellSpursNativeJobRangeValid(job->descriptor_ea,128)) return 0;
    if((uint64_t)output<(uint64_t)job->descriptor_ea+128 &&
       (uint64_t)job->descriptor_ea<(uint64_t)output+count*48) return 0;
    NativePose* data=&inputs;
    /* Preserve the original DMA-list acquisition order. Aliased guest inputs
     * or input/output storage is safe because all inputs are snapshotted first. */
    if(!cellSpursNativeJobAcquire(job,world,data->world,64) ||
       !cellSpursNativeJobAcquire(job,bones,data->bones,count*64) ||
       !cellSpursNativeJobAcquire(job,model,data->model,count*64)) return 0;
    data->count=count;data->output_ea=output;
    memcpy(data->completed,d,128);memset(data->completed+16,0,4);
    operation->execute=execute_pose;operation->user=data;return 1;
}
