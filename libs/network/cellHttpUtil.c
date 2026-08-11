/*
 * ps3recomp - cellHttpUtil HLE implementation
 *
 * URI parsing/building, percent-encoding, form-url encoding, Base64.
 * Pure C, no external dependencies.
 *
 * Conformance notes (2026-08-05 rework against the SDK 4.75 oracles):
 *  - cellHttpUtilParseUri writes the documented 28-byte guest CellHttpUri
 *    (five BE guest EAs + BE port, ORACLE(cell/http/util.h:12-20)) using
 *    the documented two-pass pool idiom; the old code discarded
 *    pool/size/required and wrote a 2704-byte host struct over the guest
 *    28-byte struct (audit 2026-08-04 HIGH: guest heap corruption).
 *  - Default ports per scheme: ORACLE(libhttp_util-Reference_e.pdf p.10):
 *    http 80, https 443, ftp 21, ssh 22, gopher 70, imap 143, rtsp 554.
 *  - BuildUri takes the SDK argument order (uri, buf, len, required,
 *    flags) and honors the four NO_* flags (ORACLE(cell/http/util.h:93-98,
 *    Reference p.22)).
 *  - Escape/Unescape/FormUrl* always write *required when supplied
 *    (two-pass sizing; audit MEDIUM: previously NO_BUFFER without
 *    *required).
 *  - Base64Encoder/Decoder use the SDK 3-argument prototypes and return
 *    the output byte count (ORACLE(cell/http/util.h:60-61, Reference
 *    p.36-37)).
 *  - Error values from ORACLE(cell/http/error.h:471-499) (0x80711001-100a).
 */

#include "cellHttpUtil.h"
#include "ps3emu/endian.h"
#include "../../runtime/memory/vm.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal helpers
 * -----------------------------------------------------------------------*/

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static const char s_hex[] = "0123456789ABCDEF";

/* Is a character "unreserved" per RFC 3986? */
static int is_unreserved(u8 c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
        return 1;
    return (c == '-' || c == '_' || c == '.' || c == '~');
}

/* Default port for a scheme.
 * ORACLE(libhttp_util-Reference_e.pdf p.10). Unknown scheme -> 0. */
static u32 uri_default_port(const char* scheme, u32 scheme_len)
{
    struct { const char* name; u32 port; } table[] = {
        { "http",   80 }, { "https", 443 }, { "ftp",   21 }, { "ssh",  22 },
        { "gopher", 70 }, { "imap",  143 }, { "rtsp", 554 },
    };
    for (u32 i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strlen(table[i].name) == scheme_len &&
            memcmp(table[i].name, scheme, scheme_len) == 0)
            return table[i].port;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * URI parsing
 * -----------------------------------------------------------------------*/

s32 cellHttpUtilParseUri(CellHttpUri* uri, const char* str,
                         void* pool, u32 size, u32* required)
{
    if (!str)
        return (s32)CELL_HTTP_UTIL_ERROR_NO_STRING;

    /* ---- locate the components (scheme required for ParseUri; the
     * schemeless variant is cellHttpUtilParseProxy) ---- */
    const char* scheme_start = str;
    const char* p = strstr(str, "://");
    if (!p || p == str)
        return (s32)CELL_HTTP_UTIL_ERROR_INVALID_URI;
    u32 scheme_len = (u32)(p - scheme_start);
    p += 3;

    /* user[:password]@ */
    const char* user_start = NULL; u32 user_len = 0;
    const char* pass_start = NULL; u32 pass_len = 0;
    {
        const char* slash = strchr(p, '/');
        const char* at    = strchr(p, '@');
        if (at && (!slash || at < slash)) {
            const char* colon = memchr(p, ':', (size_t)(at - p));
            if (colon) {
                user_start = p;             user_len = (u32)(colon - p);
                pass_start = colon + 1;     pass_len = (u32)(at - colon - 1);
            } else {
                user_start = p;             user_len = (u32)(at - p);
            }
            p = at + 1;
        }
    }

    /* hostname[:port] */
    const char* host_start = p;
    const char* host_end = p;
    while (*host_end && *host_end != '/' && *host_end != ':' &&
           *host_end != '?' && *host_end != '#')
        host_end++;
    u32 host_len = (u32)(host_end - host_start);
    if (host_len == 0)
        return (s32)CELL_HTTP_UTIL_ERROR_INVALID_URI;

    u32 port = 0;
    p = host_end;
    if (*p == ':') {
        p++;
        if (*p < '0' || *p > '9')
            return (s32)CELL_HTTP_UTIL_ERROR_INVALID_URI;
        while (*p >= '0' && *p <= '9')
            port = port * 10 + (u32)(*p++ - '0');
        if (port > 65535)
            return (s32)CELL_HTTP_UTIL_ERROR_INVALID_URI;
    }
    if (port == 0)
        port = uri_default_port(scheme_start, scheme_len);

    /* Path includes query/fragment: ORACLE(libhttp_util-Reference p.4):
     * "path: Path name, including the filename query string and its
     * components". Empty path is stored as "/" (INFERRED). */
    const char* path_start = (*p != '\0') ? p : "/";
    u32 path_len = (u32)strlen(path_start);

    /* ---- two-pass pool sizing ---- */
    u32 need = (scheme_len + 1) + (host_len + 1) + (path_len + 1);
    if (user_start) need += user_len + 1;
    if (pass_start) need += pass_len + 1;

    if (required)
        *required = ps3_bswap32(need);   /* guest BE; written on both passes */

    if (!pool || !uri)
        return CELL_OK;                  /* first pass: sizing only */

    if (size < need)
        return (s32)CELL_HTTP_UTIL_ERROR_INSUFFICIENT;

    /* ---- copy strings into the caller's pool, point the guest struct at
     * them (BE guest EAs) ---- */
    char* dst   = (char*)pool;
    u32 pool_ea = vm_to_guest(pool);
    u32 off     = 0;

    u32 scheme_ea = pool_ea + off;
    memcpy(dst + off, scheme_start, scheme_len);
    dst[off + scheme_len] = '\0';
    off += scheme_len + 1;

    u32 host_ea = pool_ea + off;
    memcpy(dst + off, host_start, host_len);
    dst[off + host_len] = '\0';
    off += host_len + 1;

    u32 user_ea = 0;
    if (user_start) {
        user_ea = pool_ea + off;
        memcpy(dst + off, user_start, user_len);
        dst[off + user_len] = '\0';
        off += user_len + 1;
    }

    u32 pass_ea = 0;
    if (pass_start) {
        pass_ea = pool_ea + off;
        memcpy(dst + off, pass_start, pass_len);
        dst[off + pass_len] = '\0';
        off += pass_len + 1;
    }

    u32 path_ea = pool_ea + off;
    memcpy(dst + off, path_start, path_len);
    dst[off + path_len] = '\0';

    uri->scheme   = ps3_bswap32(scheme_ea);
    uri->hostname = ps3_bswap32(host_ea);
    uri->username = ps3_bswap32(user_ea);
    uri->password = ps3_bswap32(pass_ea);
    uri->path     = ps3_bswap32(path_ea);
    uri->port     = ps3_bswap32(port);
    memset(uri->reserved, 0, sizeof(uri->reserved));

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * URI building
 * -----------------------------------------------------------------------*/

/* Bounded append helper for BuildUri: always tracks the exact needed size,
 * copies only what fits when a buffer is present. */
typedef struct {
    char* buf;
    u32   cap;
    u32   used;   /* bytes that WOULD be used (excl. NUL) */
} UriBuild;

static void ub_append(UriBuild* b, const char* s, u32 n)
{
    if (b->buf && b->used < b->cap) {
        u32 space = b->cap - b->used;
        u32 copy  = n < space ? n : space;
        memcpy(b->buf + b->used, s, copy);
    }
    b->used += n;
}

static void ub_append_str(UriBuild* b, const char* s)
{
    ub_append(b, s, (u32)strlen(s));
}

s32 cellHttpUtilBuildUri(const CellHttpUri* uri, char* buf, u32 len,
                         u32* required, s32 flags)
{
    if (!uri)
        return (s32)CELL_HTTP_UTIL_ERROR_NO_BUFFER;

    /* Guest struct fields: BE EAs / BE port (ORACLE(cell/http/util.h)). */
    u32 scheme_ea = ps3_bswap32(uri->scheme);
    u32 host_ea   = ps3_bswap32(uri->hostname);
    u32 user_ea   = ps3_bswap32(uri->username);
    u32 pass_ea   = ps3_bswap32(uri->password);
    u32 path_ea   = ps3_bswap32(uri->path);
    u32 port      = ps3_bswap32(uri->port);

    const char* scheme = scheme_ea ? (const char*)vm_to_host(scheme_ea) : NULL;
    const char* host   = host_ea   ? (const char*)vm_to_host(host_ea)   : NULL;
    const char* user   = user_ea   ? (const char*)vm_to_host(user_ea)   : NULL;
    const char* pass   = pass_ea   ? (const char*)vm_to_host(pass_ea)   : NULL;
    const char* path   = path_ea   ? (const char*)vm_to_host(path_ea)   : NULL;

    if (!host || !host[0])
        return (s32)CELL_HTTP_UTIL_ERROR_INVALID_URI;

    UriBuild b;
    b.buf  = buf;
    b.cap  = buf ? len : 0;
    b.used = 0;

    if (!(flags & CELL_HTTP_UTIL_URI_FLAG_NO_SCHEME) && scheme && scheme[0]) {
        ub_append_str(&b, scheme);
        ub_append_str(&b, "://");
    }
    if (!(flags & CELL_HTTP_UTIL_URI_FLAG_NO_CREDENTIALS) && user && user[0]) {
        ub_append_str(&b, user);
        if (!(flags & CELL_HTTP_UTIL_URI_FLAG_NO_PASSWORD) && pass && pass[0]) {
            ub_append_str(&b, ":");
            ub_append_str(&b, pass);
        }
        ub_append_str(&b, "@");
    }
    ub_append_str(&b, host);
    if (port != 0) {
        /* Reference p.22 example includes the port even at its scheme
         * default ("http://...:80/...") -> include whenever set (INFERRED). */
        char pbuf[12];
        snprintf(pbuf, sizeof(pbuf), ":%u", port);
        ub_append_str(&b, pbuf);
    }
    if (!(flags & CELL_HTTP_UTIL_URI_FLAG_NO_PATH) && path && path[0])
        ub_append_str(&b, path);

    u32 need = b.used + 1;   /* documented: required includes the NUL */
    if (required)
        *required = ps3_bswap32(need);   /* guest BE; written on both passes */

    if (!buf)
        return CELL_OK;                  /* first pass: sizing only */

    if (len < need)
        return (s32)CELL_HTTP_UTIL_ERROR_INSUFFICIENT;

    buf[b.used] = '\0';
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Percent-encoding / decoding (two-pass, *required always reported)
 * -----------------------------------------------------------------------*/

s32 cellHttpUtilEscapeUri(char* out, u32 outSize,
                          const u8* in, u32 inSize, u32* required)
{
    if (!in)
        return (s32)CELL_HTTP_UTIL_ERROR_NO_STRING;

    /* sizing pass */
    u32 need = 0;
    for (u32 i = 0; i < inSize; i++)
        need += is_unreserved(in[i]) ? 1 : 3;
    need += 1;   /* NUL terminator (output is a string) */

    if (required)
        *required = ps3_bswap32(need);   /* guest BE; written on both passes */

    if (!out)
        return CELL_OK;                  /* first pass of the two-pass idiom */

    if (outSize < need)
        return (s32)CELL_HTTP_UTIL_ERROR_INSUFFICIENT;

    u32 pos = 0;
    for (u32 i = 0; i < inSize; i++) {
        if (is_unreserved(in[i])) {
            out[pos++] = (char)in[i];
        } else {
            out[pos++] = '%';
            out[pos++] = s_hex[(in[i] >> 4) & 0xF];
            out[pos++] = s_hex[in[i] & 0xF];
        }
    }
    out[pos] = '\0';
    return CELL_OK;
}

s32 cellHttpUtilUnescapeUri(u8* out, u32 size, const char* in, u32* required)
{
    if (!in)
        return (s32)CELL_HTTP_UTIL_ERROR_NO_STRING;

    u32 in_len = (u32)strlen(in);

    /* sizing pass (unescaped characters pass through unchanged --
     * ORACLE(libhttp_util-Reference p.33 Notes)) */
    u32 need = 0;
    for (u32 i = 0; i < in_len; i++) {
        if (in[i] == '%' && i + 2 < in_len &&
            hex_digit(in[i+1]) >= 0 && hex_digit(in[i+2]) >= 0)
            i += 2;
        need++;
    }

    if (required)
        *required = ps3_bswap32(need);   /* guest BE; written on both passes */

    if (!out)
        return CELL_OK;

    if (size < need)
        return (s32)CELL_HTTP_UTIL_ERROR_INSUFFICIENT;

    u32 pos = 0;
    for (u32 i = 0; i < in_len; i++) {
        u8 decoded;
        if (in[i] == '%' && i + 2 < in_len) {
            int hi = hex_digit(in[i + 1]);
            int lo = hex_digit(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                decoded = (u8)((hi << 4) | lo);
                i += 2;
            } else {
                decoded = (u8)in[i];
            }
        } else {
            decoded = (u8)in[i];
        }
        out[pos++] = decoded;
    }
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * application/x-www-form-urlencoded codec
 *
 * SDK prototype is a plain buffer codec (ORACLE(cell/http/util.h:55-56)):
 * space -> '+', unreserved passthrough, everything else %XX.
 * -----------------------------------------------------------------------*/

s32 cellHttpUtilFormUrlEncode(char* out, u32 outSize,
                              const u8* in, u32 inSize, u32* required)
{
    if (!in)
        return (s32)CELL_HTTP_UTIL_ERROR_NO_STRING;

    u32 need = 0;
    for (u32 i = 0; i < inSize; i++)
        need += (in[i] == ' ' || is_unreserved(in[i])) ? 1 : 3;
    need += 1;   /* NUL */

    if (required)
        *required = ps3_bswap32(need);

    if (!out)
        return CELL_OK;

    if (outSize < need)
        return (s32)CELL_HTTP_UTIL_ERROR_INSUFFICIENT;

    u32 pos = 0;
    for (u32 i = 0; i < inSize; i++) {
        if (in[i] == ' ') {
            out[pos++] = '+';
        } else if (is_unreserved(in[i])) {
            out[pos++] = (char)in[i];
        } else {
            out[pos++] = '%';
            out[pos++] = s_hex[(in[i] >> 4) & 0xF];
            out[pos++] = s_hex[in[i] & 0xF];
        }
    }
    out[pos] = '\0';
    return CELL_OK;
}

s32 cellHttpUtilFormUrlDecode(u8* out, u32 size, const char* in, u32* required)
{
    if (!in)
        return (s32)CELL_HTTP_UTIL_ERROR_NO_STRING;

    u32 in_len = (u32)strlen(in);

    u32 need = 0;
    for (u32 i = 0; i < in_len; i++) {
        if (in[i] == '%' && i + 2 < in_len &&
            hex_digit(in[i+1]) >= 0 && hex_digit(in[i+2]) >= 0)
            i += 2;
        need++;
    }

    if (required)
        *required = ps3_bswap32(need);

    if (!out)
        return CELL_OK;

    if (size < need)
        return (s32)CELL_HTTP_UTIL_ERROR_INSUFFICIENT;

    u32 pos = 0;
    for (u32 i = 0; i < in_len; i++) {
        if (in[i] == '+') {
            out[pos++] = ' ';
        } else if (in[i] == '%' && i + 2 < in_len &&
                   hex_digit(in[i+1]) >= 0 && hex_digit(in[i+2]) >= 0) {
            out[pos++] = (u8)((hex_digit(in[i+1]) << 4) | hex_digit(in[i+2]));
            i += 2;
        } else {
            out[pos++] = (u8)in[i];
        }
    }
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Base64 -- SDK 3-argument prototypes, return output byte count
 * (ORACLE(cell/http/util.h:60-61), Reference p.36-37: caller allocates
 * exactly CELL_HTTP_UTIL_BASE64_ENC/DEC_BUF_SIZE bytes; no NUL is written).
 * -----------------------------------------------------------------------*/

static const char s_b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

s32 cellHttpUtilBase64Encoder(char* out, const void* input, u32 len)
{
    if (!out)
        return (s32)CELL_HTTP_UTIL_ERROR_NO_BUFFER;
    if (!input)
        return (s32)CELL_HTTP_UTIL_ERROR_NO_STRING;

    const u8* data = (const u8*)input;
    u32 pos = 0;
    for (u32 i = 0; i < len; i += 3) {
        u32 n = ((u32)data[i]) << 16;
        if (i + 1 < len) n |= ((u32)data[i + 1]) << 8;
        if (i + 2 < len) n |= data[i + 2];

        out[pos++] = s_b64_table[(n >> 18) & 0x3F];
        out[pos++] = s_b64_table[(n >> 12) & 0x3F];
        out[pos++] = (i + 1 < len) ? s_b64_table[(n >> 6) & 0x3F] : '=';
        out[pos++] = (i + 2 < len) ? s_b64_table[n & 0x3F] : '=';
    }

    return (s32)pos;   /* documented: output byte count */
}

static int b64_decode_char(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

s32 cellHttpUtilBase64Decoder(char* output, const void* in, u32 len)
{
    if (!output)
        return (s32)CELL_HTTP_UTIL_ERROR_NO_BUFFER;
    if (!in)
        return (s32)CELL_HTTP_UTIL_ERROR_NO_STRING;

    const char* enc = (const char*)in;
    u8* out = (u8*)output;
    u32 pos = 0;

    for (u32 i = 0; i < len; i += 4) {
        int a = b64_decode_char(enc[i]);
        int b = (i + 1 < len) ? b64_decode_char(enc[i + 1]) : -1;
        int c = (i + 2 < len && enc[i + 2] != '=') ? b64_decode_char(enc[i + 2]) : -2;
        int d = (i + 3 < len && enc[i + 3] != '=') ? b64_decode_char(enc[i + 3]) : -2;

        if (a < 0 || b < 0 || c == -1 || d == -1)
            return (s32)CELL_HTTP_UTIL_ERROR_INVALID_CHARACTER;

        u32 triple = ((u32)a << 18) | ((u32)b << 12);
        if (c >= 0) triple |= ((u32)c << 6);
        if (d >= 0) triple |= (u32)d;

        out[pos++] = (u8)((triple >> 16) & 0xFF);
        if (c >= 0)
            out[pos++] = (u8)((triple >> 8) & 0xFF);
        if (d >= 0)
            out[pos++] = (u8)(triple & 0xFF);
    }

    return (s32)pos;   /* documented: output byte count */
}
