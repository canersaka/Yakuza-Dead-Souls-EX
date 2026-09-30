/*
 * ps3recomp - cellHttps HLE implementation
 *
 * Lifecycle only: the caller's CA list is accepted and discarded.  No TLS
 * exists here, so https transactions fail at cellHttpSendRequest with
 * CELL_HTTPS_ERROR_HANDSHAKE (cellHttp.c) instead of silently downgrading
 * to plaintext.
 *
 * Signatures:
 *   int cellHttpsInit(size_t caCertNum, const CellHttpsData *caList);
 *   int cellHttpsEnd(void);
 * CellHttpsData is a guest struct of two BE 4-byte fields {ptr, size};
 * caList arrives as a host pointer to that guest array.
 *
 * Error values: the SDK defines no https-specific init errors; libhttp's
 * init pair applies.
 */

#include "cellHttps.h"

static int s_https_initialized = 0;

s32 cellHttpsInit(u32 caCertNum, const CellHttpsData* caList)
{
    if (s_https_initialized)
        return (s32)CELL_HTTP_ERROR_ALREADY_INITIALIZED;
    if (caCertNum > 0 && !caList)
        return (s32)CELL_HTTP_ERROR_NO_BUFFER;

    s_https_initialized = 1;
    return CELL_OK;
}

s32 cellHttpsEnd(void)
{
    if (!s_https_initialized)
        return (s32)CELL_HTTP_ERROR_NOT_INITIALIZED;

    s_https_initialized = 0;
    return CELL_OK;
}
