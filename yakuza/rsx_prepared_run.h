#ifndef YZ_RSX_PREPARED_RUN_H
#define YZ_RSX_PREPARED_RUN_H
#include <stdint.h>

/* Under the caller's FIFO ownership lock. Admit one complete linear packet
 * at a time, within its mapping page. Never read the next packet before the
 * current action and GET publication: the producer may patch it in response.
 * Unsupported/control-flow/side-effect packets remain at GET for fallback.
 * execute must refuse atomically; publish returns false if a callback changed
 * the cursor or context. Bound ownership latency to 64 headers/4096 arguments. */
template<class Put,class Read,class ReadBlock,class Admit,class Execute,class Publish>
static uint32_t yz_rsx_run_prepared_blocks(uint32_t get,Put put_now,Read read,
    ReadBlock read_block,Admit admit,Execute execute,Publish publish)
{
    uint32_t retired=0,arguments=0;
    uint32_t words[2047];
    while(retired<64u) {
        const uint32_t put=put_now();
        if((get&3u)||(put&3u)||get==put)break;
        uint32_t header=0;
        if(!read(get,&header)||!admit(header))break;
        const uint32_t count=(header>>18)&0x7FFu;
        const uint32_t bytes=4u+count*4u;
        /* Same publication rule as the general FIFO decoder: PUT bounds a
         * linear packet when GET<PUT. An already-followed jump can publish a
         * generated list beyond PUT; its own flow/stopper headers end the run.
         * Recheck the rule with the latest PUT for every packet. */
        if((get<put&&bytes>put-get)||bytes>UINT32_MAX-get||
            bytes>0x100000u-(get&0xFFFFFu)||arguments+count>4096u)break;
        if((count&&!read_block(get+4,words,count))||!execute(header,words,count))break;
        arguments+=count;++retired;get+=bytes;
        if(!publish(get))break;
    }
    return retired;
}
#endif
