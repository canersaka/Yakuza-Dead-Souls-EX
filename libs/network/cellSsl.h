/*
 * ps3recomp - cellSsl HLE
 *
 * SSL support module for cellHttp's https surface.  This firmware-free
 * build ships no TLS engine and no CA certificate store, so the
 * certificate loader and the cert accessors fail with their DOCUMENTED
 * error values (the game's https path fail-softs on them).
 *
 * Oracles:
 *   ORACLE(cell/ssl/ssl.h)    - cellSslInit/cellSslEnd prototypes
 *   ORACLE(cell/ssl/cert.h)   - cellSslCertificateLoader prototype + the
 *                               CELL_SSL_LOAD_CERT_* flag bits (bit0..bit57)
 *   ORACLE(cell/ssl/error.h)  - libssl error namespace 0x8074_0001-0x8074_0fff
 *                               (the previous 0x8072xxxx values here were
 *                               fabricated -- audit 2026-08-04).
 */

#ifndef PS3RECOMP_CELL_SSL_H
#define PS3RECOMP_CELL_SSL_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes -- values verbatim from ORACLE(cell/ssl/error.h)
 * (note: NOT_INITIALIZED really is 0x80740001 and ALREADY_INITIALIZED
 *  0x80740002 -- the SDK header's comments are swapped, the values and
 *  names are authoritative)
 * -----------------------------------------------------------------------*/
#define CELL_SSL_ERROR_NOT_INITIALIZED          0x80740001  /* error.h:25 */
#define CELL_SSL_ERROR_ALREADY_INITIALIZED      0x80740002  /* error.h:29 */
#define CELL_SSL_ERROR_INITIALIZATION_FAILED    0x80740003  /* error.h:33 */
#define CELL_SSL_ERROR_NO_BUFFER                0x80740004  /* error.h:37 */
#define CELL_SSL_ERROR_INVALID_CERTIFICATE      0x80740005  /* error.h:41 */
#define CELL_SSL_ERROR_UNRETRIEVABLE            0x80740006  /* error.h:45 */
#define CELL_SSL_ERROR_INVALID_FORMAT           0x80740007  /* error.h:48 */
#define CELL_SSL_ERROR_NOT_FOUND                0x80740008  /* error.h:51 */
#define CELL_SSL_ERROR_READ_FAILED              0x80740009  /* error.h:54 */
#define CELL_SSL_ERROR_NO_MEMORY                0x80740035  /* error.h:74 */
#define CELL_SSL_ERROR_NO_STRING                0x80740036  /* error.h:78 */
#define CELL_SSL_ERROR_UNKNOWN_LOAD_CERT        0x80740037  /* error.h:82 */

/* Valid cellSslCertificateLoader flag mask: CELL_SSL_LOAD_CERT_* occupies
 * bits 0..57 (ORACLE(cell/ssl/cert.h:80-146), SCE01=bit0 .. SECOM_RCA2=bit57). */
#define CELL_SSL_LOAD_CERT_VALID_MASK  0x03FFFFFFFFFFFFFFull

/* ---------------------------------------------------------------------------
 * Types
 * -----------------------------------------------------------------------*/
typedef u32 CellSslCertId;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 cellSslInit(void* pool, u32 poolSize);
s32 cellSslEnd(void);

/* Certificate loader + accessors.  Signature CONFIRMED CORRECT by the
 * 2026-08-04 audit.  With no CA store on disk the loader reports the
 * documented read failure; the accessors can therefore never see a valid
 * certificate and reject every handle. */
s32 cellSslCertificateLoader(u64 flags, char* buffer, u32 size, u32* required);
s32 cellSslCertGetSerialNumber(CellSslCertId certId, u8* serial, u32* serialSize);
s32 cellSslCertGetPublicKey(CellSslCertId certId, u8* key, u32* keySize);
s32 cellSslCertGetNotBefore(CellSslCertId certId, u64* time);
s32 cellSslCertGetNotAfter(CellSslCertId certId, u64* time);

/* Entropy / RNG (not an SDK libssl export; host-side helper kept for
 * internal use). */
s32 cellSslGetRandomNumber(u8* buf, u32 size);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_SSL_H */
