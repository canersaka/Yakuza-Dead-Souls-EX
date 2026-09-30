/*
 * ps3recomp - cellHttps HLE
 *
 * The SDK https surface inside libhttp is just the init/end pair plus the
 * CellHttpsData CA-list element; everything else (SSL callback, cipher
 * getters) lives under the cellHttp module.
 *
 * TLS policy: no TLS is implemented.  cellHttpsInit
 * accepts and discards the CA list; https transactions fail cleanly at
 * cellHttpSendRequest with CELL_HTTPS_ERROR_HANDSHAKE (see cellHttp.c).
 * Error values come from cellHttp.h.
 */

#ifndef PS3RECOMP_CELL_HTTPS_H
#define PS3RECOMP_CELL_HTTPS_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"
#include "cellHttp.h"   /* CellHttpsData guest struct + 0x80710a0x errors */

#ifdef __cplusplus
extern "C" {
#endif

s32 cellHttpsInit(u32 caCertNum, const CellHttpsData* caList);
s32 cellHttpsEnd(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_HTTPS_H */
