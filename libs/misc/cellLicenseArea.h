/*
 * ps3recomp - cellLicenseArea HLE
 *
 * License area verification for region-locked content.
 * Stub — always reports valid license, no restrictions.
 */

#ifndef PS3RECOMP_CELL_LICENSE_AREA_H
#define PS3RECOMP_CELL_LICENSE_AREA_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
/* The real cellSysutilGetLicenseArea API has NO error facility
 * (sysutil_licensearea.h defines zero error constants). These stub-local
 * guard values previously sat on 0x80410B01/02 == the LIVE swcache
 * namespace (cell/swcache/error.h) -- diagnostic poison. Map them to the
 * real generic sysutil-common codes instead (sysutil_common.h 0x8002b1xx):
 * bad pointer arg -> ERROR_VALUE, lookup failure -> ERROR_STATUS. */
#define CELL_LICENSE_AREA_ERROR_INVALID_ARGUMENT  0x8002b102 /* SYSUTIL VALUE */
#define CELL_LICENSE_AREA_ERROR_NOT_FOUND         0x8002b106 /* SYSUTIL STATUS */

/* License area IDs */
#define CELL_LICENSE_AREA_J    1   /* Japan */
#define CELL_LICENSE_AREA_A    2   /* Americas */
#define CELL_LICENSE_AREA_E    3   /* Europe */
#define CELL_LICENSE_AREA_H    4   /* Asia (Hong Kong) */
#define CELL_LICENSE_AREA_K    5   /* Korea */
#define CELL_LICENSE_AREA_C    6   /* China */

/* Functions */
s32 cellLicenseAreaCheck(s32* areaCode);
s32 cellLicenseAreaGetAreaCode(s32* areaCode);
s32 cellLicenseAreaIsValid(s32 areaCode, s32* isValid);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_LICENSE_AREA_H */
