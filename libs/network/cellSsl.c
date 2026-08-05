/*
 * ps3recomp - cellSsl HLE implementation
 *
 * Init/End lifecycle works; the certificate loader and cert accessors
 * fail with their DOCUMENTED error values because this firmware-free
 * build ships no CA certificate store and no TLS engine (see the TLS
 * policy note in cellHttps.h).  RNG uses host OS entropy.
 *
 * Error values: ORACLE(cell/ssl/error.h), 0x8074xxxx namespace.
 */

#include "cellSsl.h"
#include "ps3emu/endian.h"   /* ps3_bswap32/64, out-params are guest big-endian */
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <fcntl.h>
#include <unistd.h>
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int s_ssl_initialized = 0;

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellSslInit(void* pool, u32 poolSize)
{
    (void)pool;
    (void)poolSize;
    printf("[cellSsl] Init(poolSize=%u)\n", poolSize);

    if (s_ssl_initialized)
        return (s32)CELL_SSL_ERROR_ALREADY_INITIALIZED;

    s_ssl_initialized = 1;
    return CELL_OK;
}

s32 cellSslEnd(void)
{
    printf("[cellSsl] End()\n");

    if (!s_ssl_initialized)
        return (s32)CELL_SSL_ERROR_NOT_INITIALIZED;

    s_ssl_initialized = 0;
    return CELL_OK;
}

s32 cellSslCertificateLoader(u64 flags, char* buffer, u32 size, u32* required)
{
    (void)buffer; (void)size;
    printf("[cellSsl] CertificateLoader(flags=0x%llX)\n", (unsigned long long)flags);

    if (!s_ssl_initialized)
        return (s32)CELL_SSL_ERROR_NOT_INITIALIZED;

    /* Flag validation per ORACLE(cell/ssl/cert.h:80-146): the defined
     * CELL_SSL_LOAD_CERT_* selectors occupy bits 0..57.
     * ORACLE(cell/ssl/error.h:80-82): "unknown certificate load flag". */
    if (flags == 0 || (flags & ~CELL_SSL_LOAD_CERT_VALID_MASK))
        return (s32)CELL_SSL_ERROR_UNKNOWN_LOAD_CERT;

    /* No CA bundle exists in this firmware-free build.  Report the
     * documented read failure (ORACLE(cell/ssl/error.h:53-54)) instead of
     * fabricating an empty success -- the old required=0 + CELL_OK made
     * the game hand an empty CA list to cellHttpsInit (audit 2026-08-04
     * MEDIUM).  The https path fail-softs on this error. */
    if (required)
        *required = ps3_bswap32(0u);  /* out-param is guest memory, big-endian */
    return (s32)CELL_SSL_ERROR_READ_FAILED;
}

/* With the loader unable to produce certificates, no valid certificate
 * object can exist; every accessor rejects its input with the documented
 * "invalid certificate pointer" error (ORACLE(cell/ssl/error.h:39-41))
 * instead of fabricating data. */

s32 cellSslCertGetSerialNumber(CellSslCertId certId, u8* serial, u32* serialSize)
{
    (void)certId; (void)serial; (void)serialSize;
    printf("[cellSsl] CertGetSerialNumber()\n");
    return (s32)CELL_SSL_ERROR_INVALID_CERTIFICATE;
}

s32 cellSslCertGetPublicKey(CellSslCertId certId, u8* key, u32* keySize)
{
    (void)certId; (void)key; (void)keySize;
    printf("[cellSsl] CertGetPublicKey()\n");
    return (s32)CELL_SSL_ERROR_INVALID_CERTIFICATE;
}

s32 cellSslCertGetNotBefore(CellSslCertId certId, u64* time)
{
    (void)certId; (void)time;
    printf("[cellSsl] CertGetNotBefore()\n");
    return (s32)CELL_SSL_ERROR_INVALID_CERTIFICATE;
}

s32 cellSslCertGetNotAfter(CellSslCertId certId, u64* time)
{
    (void)certId; (void)time;
    printf("[cellSsl] CertGetNotAfter()\n");
    return (s32)CELL_SSL_ERROR_INVALID_CERTIFICATE;
}

s32 cellSslGetRandomNumber(u8* buf, u32 size)
{
    if (!buf || size == 0)
        return (s32)CELL_SSL_ERROR_NO_BUFFER;

#ifdef _WIN32
    NTSTATUS status = BCryptGenRandom(NULL, buf, size,
                                       BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status != 0) {
        /* Fallback: fill with pseudo-random */
        for (u32 i = 0; i < size; i++)
            buf[i] = (u8)(i * 37 + 13);
    }
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        read(fd, buf, size);
        close(fd);
    } else {
        for (u32 i = 0; i < size; i++)
            buf[i] = (u8)(i * 37 + 13);
    }
#endif

    return CELL_OK;
}
