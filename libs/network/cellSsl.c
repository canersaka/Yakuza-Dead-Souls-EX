/*
 * ps3recomp - cellSsl HLE implementation
 *
 * Init/End lifecycle works; the certificate loader and cert accessors
 * fail with their error values because no CA certificate store and no TLS
 * engine are shipped (see the TLS policy note in cellHttps.h).  The RNG
 * uses host OS entropy.
 *
 * Error values are in the 0x8074xxxx namespace.
 */

#include "cellSsl.h"
#include "ps3emu/endian.h"   /* ps3_bswap32/64, out-params are guest big-endian */

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

    if (s_ssl_initialized)
        return (s32)CELL_SSL_ERROR_ALREADY_INITIALIZED;

    s_ssl_initialized = 1;
    return CELL_OK;
}

s32 cellSslEnd(void)
{
    if (!s_ssl_initialized)
        return (s32)CELL_SSL_ERROR_NOT_INITIALIZED;

    s_ssl_initialized = 0;
    return CELL_OK;
}

s32 cellSslCertificateLoader(u64 flags, char* buffer, u32 size, u32* required)
{
    (void)buffer; (void)size;

    if (!s_ssl_initialized)
        return (s32)CELL_SSL_ERROR_NOT_INITIALIZED;

    /* The defined CELL_SSL_LOAD_CERT_* selectors occupy bits 0..57; any
     * other bit is an unknown certificate load flag. */
    if (flags == 0 || (flags & ~CELL_SSL_LOAD_CERT_VALID_MASK))
        return (s32)CELL_SSL_ERROR_UNKNOWN_LOAD_CERT;

    /* No CA bundle is shipped, so the loader reports a read failure. */
    if (required)
        *required = ps3_bswap32(0u);  /* out-param is guest memory, big-endian */
    return (s32)CELL_SSL_ERROR_READ_FAILED;
}

/* The loader never produces certificates, so no valid certificate object
 * exists; every accessor rejects its input with INVALID_CERTIFICATE. */

s32 cellSslCertGetSerialNumber(CellSslCertId certId, u8* serial, u32* serialSize)
{
    (void)certId; (void)serial; (void)serialSize;
    return (s32)CELL_SSL_ERROR_INVALID_CERTIFICATE;
}

s32 cellSslCertGetPublicKey(CellSslCertId certId, u8* key, u32* keySize)
{
    (void)certId; (void)key; (void)keySize;
    return (s32)CELL_SSL_ERROR_INVALID_CERTIFICATE;
}

s32 cellSslCertGetNotBefore(CellSslCertId certId, u64* time)
{
    (void)certId; (void)time;
    return (s32)CELL_SSL_ERROR_INVALID_CERTIFICATE;
}

s32 cellSslCertGetNotAfter(CellSslCertId certId, u64* time)
{
    (void)certId; (void)time;
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
