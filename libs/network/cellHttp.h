/*
 * ps3recomp - cellHttp HLE
 *
 * HTTP client: clients, transactions, request/response, auto-redirect,
 * chunked transfer decoding.
 *
 * Guest-facing layout, export names and error values follow the PS3 libhttp
 * ABI. Error namespaces:
 *       libhttp      0x8071_0001 - 0x8071_0fff
 *       https        0x8071_0a01 - 0x8071_0aff
 *       libhttp_util 0x8071_1001 - 0x8071_10ff
 * Auto-redirect and auto-authentication both default to enabled, with at
 * most 5 attempts each.
 *
 * ABI note: every pointer parameter arrives as a HOST pointer into guest
 * memory (the import bridge translates guest EA -> host).  Anything
 * multi-byte read or written through such a pointer is guest BIG-ENDIAN;
 * use ps3_bswap16/32/64 (include/ps3emu/endian.h), same convention as
 * libs/video/cellVideoOut.c.
 */

#ifndef PS3RECOMP_CELL_HTTP_H
#define PS3RECOMP_CELL_HTTP_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes -- values as used by the PS3 libhttp ABI
 * -----------------------------------------------------------------------*/
#define CELL_HTTP_ERROR_ALREADY_INITIALIZED     0x80710001
#define CELL_HTTP_ERROR_NOT_INITIALIZED         0x80710002
#define CELL_HTTP_ERROR_NO_MEMORY               0x80710003
#define CELL_HTTP_ERROR_NO_BUFFER               0x80710004
#define CELL_HTTP_ERROR_NO_STRING               0x80710005
#define CELL_HTTP_ERROR_INSUFFICIENT            0x80710006
#define CELL_HTTP_ERROR_INVALID_URI             0x80710007
#define CELL_HTTP_ERROR_INVALID_HEADER          0x80710008
#define CELL_HTTP_ERROR_BAD_METHOD              0x80710009
#define CELL_HTTP_ERROR_BAD_CLIENT              0x80710010
#define CELL_HTTP_ERROR_BAD_TRANS               0x80710011
#define CELL_HTTP_ERROR_NO_CONNECTION           0x80710012
#define CELL_HTTP_ERROR_NO_REQUEST_SENT         0x80710013
#define CELL_HTTP_ERROR_ALREADY_BUILT           0x80710014
#define CELL_HTTP_ERROR_ALREADY_SENT            0x80710015
#define CELL_HTTP_ERROR_NO_HEADER               0x80710016
#define CELL_HTTP_ERROR_NO_CONTENT_LENGTH       0x80710017
#define CELL_HTTP_ERROR_TOO_MANY_REDIRECTS      0x80710018
#define CELL_HTTP_ERROR_TOO_MANY_AUTHS          0x80710019
#define CELL_HTTP_ERROR_TRANS_NO_CONNECTION     0x80710020
#define CELL_HTTP_ERROR_CB_FAILED               0x80710021
#define CELL_HTTP_ERROR_TRANS_ABORTED           0x80710024
#define CELL_HTTP_ERROR_UNAVAILABLE             0x80710026
#define CELL_HTTP_ERROR_INVALID_VALUE           0x80710027
#define CELL_HTTP_ERROR_CANNOT_AUTHENTICATE     0x80710028
#define CELL_HTTP_ERROR_CACHE_ALREADY_INITIALIZED 0x80710043
#define CELL_HTTP_ERROR_CACHE_NOT_INITIALIZED   0x80710044
#define CELL_HTTP_ERROR_LINE_EXCEEDS_MAX        0x80710045
#define CELL_HTTP_ERROR_UNKNOWN                 0x80710051
#define CELL_HTTP_ERROR_INTERNAL                0x80710052
#define CELL_HTTP_ERROR_BROKEN_CHUNK            0x8071005a

/* Network errors: low byte carries the socket errno/h_errno in the real
 * library; we report the class value. */
#define CELL_HTTP_ERROR_NET_FIN                 0x80710091
#define CELL_HTTP_ERROR_NET_CONNECT_TIMEOUT     0x80710092
#define CELL_HTTP_ERROR_NET_SELECT_TIMEOUT      0x80710093
#define CELL_HTTP_ERROR_NET_SEND_TIMEOUT        0x80710094
#define CELL_HTTP_ERROR_NET_RESOLVER            0x80710100
#define CELL_HTTP_ERROR_NET_ABORT               0x80710200
#define CELL_HTTP_ERROR_NET_OPTION              0x80710300
#define CELL_HTTP_ERROR_NET_SOCKET              0x80710400
#define CELL_HTTP_ERROR_NET_CONNECT             0x80710500
#define CELL_HTTP_ERROR_NET_SEND                0x80710600
#define CELL_HTTP_ERROR_NET_RECV                0x80710700

/* HTTPS errors (0x80710a0x block) */
#define CELL_HTTPS_ERROR_CERTIFICATE_LOAD       0x80710a01
#define CELL_HTTPS_ERROR_BAD_MEMORY             0x80710a02
#define CELL_HTTPS_ERROR_CONTEXT_CREATION       0x80710a03
#define CELL_HTTPS_ERROR_CONNECTION_CREATION    0x80710a04
#define CELL_HTTPS_ERROR_SOCKET_ASSOCIATION     0x80710a05
#define CELL_HTTPS_ERROR_HANDSHAKE              0x80710a06
#define CELL_HTTPS_ERROR_LOOKUP_CERTIFICATE     0x80710a07
#define CELL_HTTPS_ERROR_NO_SSL                 0x80710a08
#define CELL_HTTPS_ERROR_KEY_LOAD               0x80710a09

/* ---------------------------------------------------------------------------
 * Guest structures
 *
 * These are declared with the exact GUEST layout: 4-byte big-endian guest
 * EAs where the SDK declares pointers (PPU pointers are 32-bit), big-endian
 * scalars elsewhere.  All producers/consumers must bswap on access.
 * -----------------------------------------------------------------------*/

/* scheme, hostname, username, password, path, port, reserved[4]:
 * 28 bytes on the guest. */
typedef struct CellHttpUri {
    u32 scheme;      /* BE guest EA -> const char* */
    u32 hostname;    /* BE guest EA -> const char* */
    u32 username;    /* BE guest EA -> const char* (0 if absent) */
    u32 password;    /* BE guest EA -> const char* (0 if absent) */
    u32 path;        /* BE guest EA -> const char* */
    u32 port;        /* BE u32, port number (value in host byte order) */
    u8  reserved[4];
} CellHttpUri;
typedef char cellhttp_assert_uri_28[(sizeof(CellHttpUri) == 28) ? 1 : -1];

/* { name, value }: 8 bytes on the guest. */
typedef struct CellHttpHeader {
    u32 name;        /* BE guest EA -> const char* */
    u32 value;       /* BE guest EA -> const char* */
} CellHttpHeader;
typedef char cellhttp_assert_hdr_8[(sizeof(CellHttpHeader) == 8) ? 1 : -1];

/* protocol, majorVersion, minorVersion, reasonPhrase, statusCode,
 * reserved[4]: 24 bytes on the guest. */
typedef struct CellHttpStatusLine {
    u32 protocol;      /* BE guest EA -> const char* */
    u32 majorVersion;  /* BE u32 */
    u32 minorVersion;  /* BE u32 */
    u32 reasonPhrase;  /* BE guest EA -> const char* */
    s32 statusCode;    /* BE s32 */
    u8  reserved[4];
} CellHttpStatusLine;
typedef char cellhttp_assert_sl_24[(sizeof(CellHttpStatusLine) == 24) ? 1 : -1];

/* { char *ptr; size_t size; }: two 4-byte BE fields on the guest. */
typedef struct CellHttpsData {
    u32 ptr;         /* BE guest EA */
    u32 size;        /* BE u32 */
} CellHttpsData;
typedef char cellhttp_assert_hd_8[(sizeof(CellHttpsData) == 8) ? 1 : -1];

/* Handles are opaque 32-bit values on the guest (real lib: pointers).
 * We hand out 1-based slot handles so a valid handle is never 0/NULL. */
typedef u32 CellHttpClientId;
typedef u32 CellHttpTransId;

/* Implementation caps (the real library sizes these from the cellHttpInit
 * pool; we use fixed caps and ignore the pool). */
#define CELL_HTTP_MAX_CLIENTS        8
#define CELL_HTTP_MAX_TRANSACTIONS   32
#define CELL_HTTP_MAX_CUSTOM_HEADERS 16

/* ---------------------------------------------------------------------------
 * Functions -- names spelled exactly as the library exports them; the
 * import generator hashes these C symbol names into NIDs, so the spelling
 * is load-bearing.
 * -----------------------------------------------------------------------*/

/* lifecycle */
s32 cellHttpInit(void* pool, u32 poolSize);
s32 cellHttpEnd(void);
s32 cellHttpInitCookie(void* pool, u32 poolSize);
s32 cellHttpEndCookie(void);

/* clients */
s32 cellHttpCreateClient(u32* clientId);
s32 cellHttpDestroyClient(u32 clientId);
s32 cellHttpClientSetAutoRedirect(u32 clientId, u32 enable);
s32 cellHttpClientGetAutoRedirect(u32 clientId, u8* enable);
s32 cellHttpClientSetAutoAuthentication(u32 clientId, u32 enable);
s32 cellHttpClientGetAutoAuthentication(u32 clientId, u8* enable);
s32 cellHttpClientSetCookieStatus(u32 clientId, u32 enable);
s32 cellHttpClientSetCookieSendCallback(u32 clientId, u32 cbfunc, u32 userArg);
s32 cellHttpClientSetCookieRecvCallback(u32 clientId, u32 cbfunc, u32 userArg);
s32 cellHttpClientSetSslCallback(u32 clientId, u32 cbfunc, u32 userArg);
s32 cellHttpClientSetConnTimeout(u32 clientId, s64 usec);
s32 cellHttpClientGetConnTimeout(u32 clientId, s64* usec);
s32 cellHttpClientSetSendTimeout(u32 clientId, s64 usec);
s32 cellHttpClientGetSendTimeout(u32 clientId, s64* usec);
s32 cellHttpClientSetRecvTimeout(u32 clientId, s64 usec);
s32 cellHttpClientGetRecvTimeout(u32 clientId, s64* usec);

/* transactions */
s32 cellHttpCreateTransaction(u32* transId, u32 clientId, const char* method,
                              const CellHttpUri* uri);
s32 cellHttpDestroyTransaction(u32 transId);
s32 cellHttpSendRequest(u32 transId, const void* buf, u32 size, u32* sent);
s32 cellHttpRecvResponse(u32 transId, void* buf, u32 size, u32* recvd);
s32 cellHttpTransactionCloseConnection(u32 transId);
s32 cellHttpTransactionAbortConnection(u32 transId);

/* request */
s32 cellHttpRequestSetContentLength(u32 transId, u64 totalSize);
s32 cellHttpRequestGetContentLength(u32 transId, u64* totalSize);
s32 cellHttpRequestAddHeader(u32 transId, const CellHttpHeader* header);
s32 cellHttpRequestSetHeader(u32 transId, const CellHttpHeader* header);

/* response */
s32 cellHttpResponseGetStatusCode(u32 transId, s32* code);
s32 cellHttpResponseGetContentLength(u32 transId, u64* length);
s32 cellHttpResponseGetHeader(u32 transId, CellHttpHeader* header,
                              const char* name, void* pool, u32 poolSize,
                              u32* required);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_HTTP_H */
