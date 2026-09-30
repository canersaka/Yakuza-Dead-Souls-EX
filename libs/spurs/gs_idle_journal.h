/* Dead Souls GS image 0, polling YIELD return 0x7C0C. This predicate only
 * identifies a fully quiescent worker; it never consumes or retires a record.
 * 0x6550 fetches 128-byte groups and uses the fourth tag to advance its fetch
 * cursor. 0x5A40 can consume earlier records before that group is complete, so
 * the wait must watch the current record, not unconditionally the fourth tag.
 * 0x4F00 services the pending table; 0x6200 and 0x6990 service the two auxiliary
 * stream arrays, whose state is +8. Every one must be idle before parking. */
#ifndef PS3RECOMP_GS_IDLE_JOURNAL_H
#define PS3RECOMP_GS_IDLE_JOURNAL_H
#include <stdint.h>
#include <stddef.h>
static uint32_t gs_idle_be32(const uint8_t* p)
{ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static uint32_t gs_idle_journal_ready_word(const uint8_t* ls,size_t bytes)
{
    if(!ls||bytes<0x40000)return 0;
    const uint8_t* m=ls+0xbc90;
    const uint32_t read=gs_idle_be32(m),write=gs_idle_be32(m+4);
    const uint32_t record=gs_idle_be32(m+0x28);
    if(read>=32||read!=write||gs_idle_be32(m+8)!=32||
       gs_idle_be32(m+12)!=0x36780||gs_idle_be32(m+0x10)||
       gs_idle_be32(m+0xb8)||gs_idle_be32(m+0xc0)||record>=4)return 0;
    if(gs_idle_be32(ls+0x36780+read*128+record*32))return 0;
    for(unsigned i=0;i<4;++i)if(gs_idle_be32(ls+0x1c900+i*44+8))return 0;
    for(unsigned i=0;i<7;++i)if(gs_idle_be32(ls+0x26a00+i*40+8))return 0;
    const uint32_t first=gs_idle_be32(m+0x14),end=gs_idle_be32(m+0x18);
    const uint32_t cursor=gs_idle_be32(m+0x1c);
    if(!first||((first|end|cursor)&127)||end<=first||cursor<first||
       (uint64_t)cursor+128>end)return 0;
    return cursor+record*32;
}
#endif
