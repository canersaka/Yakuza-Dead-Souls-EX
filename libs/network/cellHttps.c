/*
 * ps3recomp - cellHttps HLE implementation
 *
 * Lifecycle only: registers (and discards) the caller's CA list.  No TLS
 * exists in this firmware-free build, so https transactions fail at
 * cellHttpSendRequest with the documented CELL_HTTPS_ERROR_HANDSHAKE
 * (cellHttp.c) instead of silently downgrading to plaintext.
 *
 * Signatures: ORACLE(cell/http/client.h:72-73):
 *   int cellHttpsInit(size_t caCertNum, const CellHttpsData *caList);
 *   int cellHttpsEnd(void);
 * CellHttpsData is a guest struct of two BE 4-byte fields {ptr, size}
 * (ORACLE(cell/http/client.h:19-22)); caList arrives as a HOST pointer to
 * that guest array.
 *
 * Error values: the SDK defines no https-specific init errors; libhttp's
 * init pair applies (ORACLE(cell/http/error.h:31-37)).
 */

#include "cellHttps.h"
#include "ps3emu/endian.h"

#include <stdio.h>

static int s_https_initialized = 0;

s32 cellHttpsInit(u32 caCertNum, const CellHttpsData* caList)
{
    printf("[cellHttps] Init(caCertNum=%u)\n", caCertNum);

    if (s_https_initialized)
        return (s32)CELL_HTTP_ERROR_ALREADY_INITIALIZED;
    if (caCertNum > 0 && !caList)
        return (s32)CELL_HTTP_ERROR_NO_BUFFER;

    /* Walk the guest CA list (BE fields) for the log only -- there is no
     * TLS stack to hand the certificates to. */
    for (u32 i = 0; i < caCertNum && i < 64; i++) {
        u32 ptr_ea = ps3_bswap32(caList[i].ptr);
        u32 sz     = ps3_bswap32(caList[i].size);
        printf("[cellHttps]   caCert[%u]: ea=0x%08X size=%u (ignored, no TLS)\n",
               i, ptr_ea, sz);
    }

    s_https_initialized = 1;
    return CELL_OK;
}

s32 cellHttpsEnd(void)
{
    printf("[cellHttps] End()\n");

    if (!s_https_initialized)
        return (s32)CELL_HTTP_ERROR_NOT_INITIALIZED;

    s_https_initialized = 0;
    return CELL_OK;
}
