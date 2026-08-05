/*
 * ps3recomp - cellHttpUtil HLE
 *
 * URI parse/build, percent-encoding, form-url encoding, Base64.
 *
 * Guest-facing contract follows the SDK 4.75 oracles:
 *   ORACLE(cell/http/util.h)  - prototypes, CellHttpUri layout, URI flags
 *   ORACLE(cell/http/error.h:468-499) - libhttp_util namespace 0x80711001-100a
 *   ORACLE(libhttp_util-Reference_e.pdf) - two-pass pool/required idiom
 *
 * All out-parameters live in guest memory: multi-byte writes are
 * big-endian (ps3_bswap*), pointers stored into guest structs are BE
 * guest EAs.
 */

#ifndef PS3RECOMP_CELL_HTTP_UTIL_H
#define PS3RECOMP_CELL_HTTP_UTIL_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"
#include "cellHttp.h"   /* CellHttpUri / CellHttpHeader guest structs */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes -- values verbatim from ORACLE(cell/http/error.h:471-499)
 * -----------------------------------------------------------------------*/
#define CELL_HTTP_UTIL_ERROR_NO_MEMORY          0x80711001
#define CELL_HTTP_UTIL_ERROR_NO_BUFFER          0x80711002
#define CELL_HTTP_UTIL_ERROR_NO_STRING          0x80711003
#define CELL_HTTP_UTIL_ERROR_INSUFFICIENT       0x80711004
#define CELL_HTTP_UTIL_ERROR_INVALID_URI        0x80711005
#define CELL_HTTP_UTIL_ERROR_INVALID_HEADER     0x80711006
#define CELL_HTTP_UTIL_ERROR_INVALID_REQUEST    0x80711007
#define CELL_HTTP_UTIL_ERROR_INVALID_RESPONSE   0x80711008
#define CELL_HTTP_UTIL_ERROR_INVALID_LENGTH     0x80711009
#define CELL_HTTP_UTIL_ERROR_INVALID_CHARACTER  0x8071100a

/* BuildUri option flags -- ORACLE(cell/http/util.h:93-97) */
#define CELL_HTTP_UTIL_URI_FLAG_FULL_URI        0x00000000
#define CELL_HTTP_UTIL_URI_FLAG_NO_SCHEME       0x00000001
#define CELL_HTTP_UTIL_URI_FLAG_NO_CREDENTIALS  0x00000002
#define CELL_HTTP_UTIL_URI_FLAG_NO_PASSWORD     0x00000004
#define CELL_HTTP_UTIL_URI_FLAG_NO_PATH         0x00000008

/* Base64 sizing -- ORACLE(cell/http/util.h:58-59) */
#define CELL_HTTP_UTIL_BASE64_ENC_BUF_SIZE(_size) (((_size) + 2) / 3 * 4)
#define CELL_HTTP_UTIL_BASE64_DEC_BUF_SIZE(_size) ((_size) / 4 * 3)

/* ---------------------------------------------------------------------------
 * Functions -- names spelled exactly as the SDK exports them
 * (ORACLE(cell/http/util.h)); NIDs are hashed from these C symbol names.
 * -----------------------------------------------------------------------*/

/* Parser: two-pass pool idiom (pool==NULL -> only *required is written). */
s32 cellHttpUtilParseUri(CellHttpUri* uri, const char* str,
                         void* pool, u32 size, u32* required);

/* Builder: real SDK argument order (uri first) + option flags. */
s32 cellHttpUtilBuildUri(const CellHttpUri* uri, char* buf, u32 len,
                         u32* required, s32 flags);

/* Percent-encoding (two-pass: out==NULL -> only *required is written). */
s32 cellHttpUtilEscapeUri(char* out, u32 outSize,
                          const u8* in, u32 inSize, u32* required);
s32 cellHttpUtilUnescapeUri(u8* out, u32 size, const char* in, u32* required);

/* application/x-www-form-urlencoded (buffer codec, not key=value). */
s32 cellHttpUtilFormUrlEncode(char* out, u32 outSize,
                              const u8* in, u32 inSize, u32* required);
s32 cellHttpUtilFormUrlDecode(u8* out, u32 size, const char* in, u32* required);

/* Base64: fixed-size output per the sizing macros, returns output byte
 * count (>0) or a negative error (ORACLE(libhttp_util-Reference p.36-37)). */
s32 cellHttpUtilBase64Encoder(char* out, const void* input, u32 len);
s32 cellHttpUtilBase64Decoder(char* output, const void* in, u32 len);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_HTTP_UTIL_H */
