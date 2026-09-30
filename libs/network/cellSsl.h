/*
 * ps3recomp - cellSsl HLE
 *
 * SSL support module for cellHttp's https surface.  No TLS engine and no
 * CA certificate store are shipped, so the certificate loader and the cert
 * accessors always fail with their error values.
 *
 * The libssl error namespace is 0x80740001-0x80740fff. The certificate
 * loader flags CELL_SSL_LOAD_CERT_* occupy bits 0..57.
 */

#ifndef PS3RECOMP_CELL_SSL_H
#define PS3RECOMP_CELL_SSL_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * (NOT_INITIALIZED is 0x80740001 and ALREADY_INITIALIZED is 0x80740002; the
 *  SDK header's comments for these two are swapped, the values and names
 *  are authoritative)
 * -----------------------------------------------------------------------*/
#define CELL_SSL_ERROR_NOT_INITIALIZED          0x80740001
#define CELL_SSL_ERROR_ALREADY_INITIALIZED      0x80740002
#define CELL_SSL_ERROR_INITIALIZATION_FAILED    0x80740003
#define CELL_SSL_ERROR_NO_BUFFER                0x80740004
#define CELL_SSL_ERROR_INVALID_CERTIFICATE      0x80740005
#define CELL_SSL_ERROR_UNRETRIEVABLE            0x80740006
#define CELL_SSL_ERROR_INVALID_FORMAT           0x80740007
#define CELL_SSL_ERROR_NOT_FOUND                0x80740008
#define CELL_SSL_ERROR_READ_FAILED              0x80740009
#define CELL_SSL_ERROR_NO_MEMORY                0x80740035
#define CELL_SSL_ERROR_NO_STRING                0x80740036
#define CELL_SSL_ERROR_UNKNOWN_LOAD_CERT        0x80740037

/* Valid cellSslCertificateLoader flag mask: CELL_SSL_LOAD_CERT_* occupies
 * bits 0..57 (SCE01=bit0 .. SECOM_RCA2=bit57). */
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

/* Certificate loader + accessors.  With no CA store the loader reports a
 * read failure; the accessors therefore never see a valid certificate and
 * reject every handle. */
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
