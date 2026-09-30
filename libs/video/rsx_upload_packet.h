#ifndef PS3RECOMP_RSX_UPLOAD_PACKET_H
#define PS3RECOMP_RSX_UPLOAD_PACKET_H

#include "ps3emu/ps3types.h"

/* Only raw subchannel-zero, data-only upload windows. Refuse the entire
 * packet before touching state if it could cross into another method. */
static inline int rsx_upload_packet_supported(u32 method, u32 count, u32 noninc)
{
    if (!count || count > 32u || (method & 3u) || noninc > 1u)
        return 0;
    const u32 end = method < 0x0C00u ? 0x0C00u : 0x1F80u;
    if (!((method >= 0x0B80u && method < 0x0C00u) ||
          (method >= 0x1F00u && method < 0x1F80u)))
        return 0;
    return noninc || count <= (end - method) / 4u;
}

#endif
