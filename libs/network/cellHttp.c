/*
 * ps3recomp - cellHttp HLE implementation
 *
 * Real HTTP client using native sockets (Winsock2 on Windows, POSIX
 * elsewhere).  Resolves hostnames, connects via TCP, sends HTTP/1.1
 * requests, receives responses with header parsing, chunked transfer
 * decoding, and documented auto-redirect/auto-authentication behavior.
 *
 * Conformance notes (2026-08-05 rework against the SDK 4.75 oracles):
 *  - Export names/prototypes: ORACLE(cell/http/client.h).
 *  - Error values: ORACLE(cell/http/error.h) -- see cellHttp.h.
 *  - CellHttpUri/CellHttpHeader are GUEST structs of big-endian 4-byte EAs;
 *    every access goes through ps3_bswap32 + vm_to_host.
 *  - Every multi-byte out-parameter write is big-endian (guest memory).
 *  - Auto-redirect: default enabled, max 5, 304/306 never redirected,
 *    301/302/303/305/307 always, other 3xx only for GET/HEAD, performed
 *    inside the library during request sending
 *    (ORACLE(libhttp-Reference_e.pdf p.47)).
 *  - Auto-authentication: default enabled (ORACLE(libhttp-Reference p.51));
 *    we can honor it only with URI-supplied Basic credentials -- guest
 *    authentication callbacks cannot be invoked from HLE, so exhausting
 *    the URI credentials yields CELL_HTTP_ERROR_CANNOT_AUTHENTICATE
 *    ("not enough info", ORACLE(cell/http/error.h:138-140)).
 *  - TLS is NOT implemented (firmware-free build, no crypto stack).
 *    https transactions fail at cellHttpSendRequest with the documented
 *    CELL_HTTPS_ERROR_HANDSHAKE instead of silently talking plaintext.
 *  - Preserved from the audited version (CONFIRMED CORRECT): timeout
 *    defaults connect 30s / send 120s / recv 120s (microseconds),
 *    race-free bounded resolve/connect helpers, method passed as string,
 *    recvd==0 EOF convention, no pipelining (obsolete API).
 */

#include "cellHttp.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Platform socket abstraction
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET http_socket_t;
#define HTTP_INVALID_SOCKET INVALID_SOCKET
#define http_closesocket closesocket
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
typedef int http_socket_t;
#define HTTP_INVALID_SOCKET (-1)
#define http_closesocket close
#endif

#include "ps3emu/endian.h"            /* ps3_bswap16/32/64 */
#include "../../runtime/memory/vm.h"  /* vm_to_host / vm_to_guest */

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int s_http_initialized = 0;
static int s_cookie_initialized = 0;
#ifdef _WIN32
static int s_wsa_initialized = 0;
#endif

typedef struct {
    int in_use;
    /* Timeouts, microseconds, client-scoped 64-bit per the SDK
     * (cellHttpClientSet{Conn,Send,Recv}Timeout).  Defaults CONFIRMED
     * CORRECT by the 2026-08-04 audit: connect 30s, send 120s, recv 120s. */
    s64 conn_timeout_usec;
    s64 send_timeout_usec;
    s64 recv_timeout_usec;
    /* ORACLE(libhttp-Reference_e.pdf p.47/p.51): both default ENABLED. */
    int auto_redirect;
    int auto_auth;
    int cookie_enabled;
    /* Raw guest callback descriptors + args; stored, never invoked (no
     * cookie engine / no TLS verify path in this HLE). */
    u32 cookie_send_cb, cookie_send_cb_arg;
    u32 cookie_recv_cb, cookie_recv_cb_arg;
    u32 ssl_cb, ssl_cb_arg;
} HttpClientSlot;

/* Custom header key/value pair */
typedef struct {
    char name[128];
    char value[512];
} HttpCustomHeader;

/* Header receive buffer size - enough for typical HTTP response headers */
#define HTTP_HDR_BUF_SIZE 8192

/* Chunked decode phases */
enum {
    HTTP_CHUNK_SIZE_LINE = 0,
    HTTP_CHUNK_DATA,
    HTTP_CHUNK_DATA_CRLF,
    HTTP_CHUNK_TRAILER,
    HTTP_CHUNK_DONE
};

typedef struct {
    int  in_use;
    u32  client;            /* owning client handle (1-based) */
    char method[16];
    char scheme[16];
    char hostname[256];
    char path[1024];
    char username[256];
    char password[256];
    u32  port;
    int  is_https;
    int  aborted;
    int  request_sent;

    u64  request_content_length;
    int  request_content_length_set;
    char auth_header[700];  /* "Authorization: Basic <b64>" resend header */
    int  auth_applied;

    HttpCustomHeader custom_headers[CELL_HTTP_MAX_CUSTOM_HEADERS];
    u32  custom_header_count;

    /* Socket and response state */
    http_socket_t sock;
    int  headers_parsed;
    s32  status_code;
    u64  content_length;
    int  content_length_known;
    int  conn_close;        /* server sent Connection: close */
    int  eof_reached;
    u32  redirect_count;
    u32  auth_count;

    /* chunked transfer decoding (Transfer-Encoding: chunked) */
    int  chunked;
    int  chunk_phase;
    u64  chunk_remaining;

    u64  body_received;

    /* Raw response header block. Kept after parsing so the Response*
     * getters keep working for the life of the transaction. */
    char hdr_buf[HTTP_HDR_BUF_SIZE];
    u32  hdr_buf_len;       /* total bytes accumulated during header read */
    u32  hdr_len;           /* length of the header section incl. \r\n\r\n */

    /* Leftover body bytes that arrived with the header read */
    char* body_overflow;
    u32   body_overflow_len;
} HttpTransSlot;

static HttpClientSlot s_clients[CELL_HTTP_MAX_CLIENTS];
static HttpTransSlot  s_transactions[CELL_HTTP_MAX_TRANSACTIONS];

/* ---------------------------------------------------------------------------
 * Handle helpers -- guest handles are 1-based so a valid id is never 0
 * (real library hands out pointers; NULL must stay invalid).
 * -----------------------------------------------------------------------*/

static HttpClientSlot* http_client_from_handle(u32 clientId)
{
    if (clientId == 0 || clientId > CELL_HTTP_MAX_CLIENTS)
        return NULL;
    if (!s_clients[clientId - 1].in_use)
        return NULL;
    return &s_clients[clientId - 1];
}

static HttpTransSlot* http_trans_from_handle(u32 transId)
{
    if (transId == 0 || transId > CELL_HTTP_MAX_TRANSACTIONS)
        return NULL;
    if (!s_transactions[transId - 1].in_use)
        return NULL;
    return &s_transactions[transId - 1];
}

/* ---------------------------------------------------------------------------
 * Small helpers
 * -----------------------------------------------------------------------*/

/* Apply a timeout (microseconds) to a socket direction. */
static void http_apply_timeout(http_socket_t sock, int snd, s64 usec)
{
    if (sock == HTTP_INVALID_SOCKET || usec < 0)
        return;

#ifdef _WIN32
    /* Winsock SO_RCVTIMEO/SO_SNDTIMEO takes milliseconds as DWORD */
    DWORD ms = (DWORD)(usec / 1000);
    if (ms == 0 && usec > 0)
        ms = 1;
    setsockopt(sock, SOL_SOCKET, snd ? SO_SNDTIMEO : SO_RCVTIMEO,
               (const char*)&ms, sizeof(ms));
#else
    struct timeval tv;
    tv.tv_sec  = (long)(usec / 1000000);
    tv.tv_usec = (long)(usec % 1000000);
    setsockopt(sock, SOL_SOCKET, snd ? SO_SNDTIMEO : SO_RCVTIMEO,
               &tv, sizeof(tv));
#endif
}

/* Reset per-response state (does NOT touch the request definition). */
static void http_reset_response_state(HttpTransSlot* t)
{
    t->headers_parsed       = 0;
    t->status_code          = 0;
    t->content_length       = 0;
    t->content_length_known = 0;
    t->conn_close           = 0;
    t->eof_reached          = 0;
    t->chunked              = 0;
    t->chunk_phase          = HTTP_CHUNK_SIZE_LINE;
    t->chunk_remaining      = 0;
    t->body_received        = 0;
    t->hdr_buf_len          = 0;
    t->hdr_len              = 0;
    if (t->body_overflow) {
        free(t->body_overflow);
        t->body_overflow = NULL;
    }
    t->body_overflow_len = 0;
}

/* Close the socket in a transaction slot if open.  Response metadata is
 * kept: the Response* getters must stay valid after connection close. */
static void http_close_slot_socket(HttpTransSlot* t)
{
    if (t->sock != HTTP_INVALID_SOCKET) {
        http_closesocket(t->sock);
        t->sock = HTTP_INVALID_SOCKET;
    }
}

/* Send all bytes on a socket, handling partial sends. Returns 0 on success. */
static int http_send_all(http_socket_t sock, const char* data, u32 len)
{
    u32 total = 0;
    while (total < len) {
        int n = send(sock, data + total, (int)(len - total), 0);
        if (n <= 0)
            return -1;
        total += (u32)n;
    }
    return 0;
}

static int http_tolower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int http_strieq_n(const char* a, const char* b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (http_tolower((unsigned char)a[i]) != http_tolower((unsigned char)b[i]))
            return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 * Bounded resolve / connect
 *
 * The actual getaddrinfo()/connect() calls run bounded by the client's
 * connection timeout so a stalled DNS server or an unreachable host cannot
 * hang the transaction forever.  (Race-free helper design CONFIRMED
 * CORRECT by the 2026-08-04 audit -- do not restructure casually.)
 * -----------------------------------------------------------------------*/

/* getaddrinfo() has no portable cancel, so a bounded wait needs a helper
 * thread. state coordinates ownership of the heap-allocated context between
 * the worker and the (possibly timed-out) caller via a single CAS: whichever
 * side observes state==0 first wins and either takes the result (caller) or
 * takes over cleanup (worker). */
typedef struct {
    char                hostname[256];
    char                port_str[8];
    struct addrinfo     hints;
    struct addrinfo*    result;
    int                 gai_ret;
    volatile long       state; /* 0=running, 1=done (caller may claim), 2=abandoned (worker frees) */
} HttpResolveCtx;

#ifdef _WIN32
static unsigned __stdcall http_resolve_worker(void* arg)
#else
static void* http_resolve_worker(void* arg)
#endif
{
    HttpResolveCtx* ctx = (HttpResolveCtx*)arg;
    ctx->gai_ret = getaddrinfo(ctx->hostname, ctx->port_str, &ctx->hints, &ctx->result);

#ifdef _WIN32
    long prev = InterlockedCompareExchange(&ctx->state, 1, 0);
#else
    long prev = __sync_val_compare_and_swap(&ctx->state, 0, 1);
#endif
    if (prev == 2) {
        /* The caller already gave up and marked us abandoned; it can't see
         * ctx->result anymore, so we own the cleanup. */
        if (ctx->result)
            freeaddrinfo(ctx->result);
        free(ctx);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* Resolve hostname/port_str with a bound of timeout_usec microseconds.
 * Returns the getaddrinfo() return code (0 on success) with out_result set;
 * a timeout reports -1 with out_result == NULL, without leaking the
 * in-flight resolution (the worker cleans itself up once it eventually
 * completes). timeout_usec == 0 means "no bound configured" -> resolve
 * inline as before. */
static int http_resolve_with_timeout(const char* hostname, const char* port_str,
                                     const struct addrinfo* hints, u32 timeout_usec,
                                     struct addrinfo** out_result)
{
    *out_result = NULL;
    if (timeout_usec == 0)
        return getaddrinfo(hostname, port_str, hints, out_result);

    HttpResolveCtx* ctx = (HttpResolveCtx*)calloc(1, sizeof(HttpResolveCtx));
    if (!ctx)
        return getaddrinfo(hostname, port_str, hints, out_result);

    strncpy(ctx->hostname, hostname, sizeof(ctx->hostname) - 1);
    strncpy(ctx->port_str, port_str, sizeof(ctx->port_str) - 1);
    ctx->hints = *hints;
    ctx->state = 0;

#ifdef _WIN32
    HANDLE th = (HANDLE)_beginthreadex(NULL, 0, http_resolve_worker, ctx, 0, NULL);
    if (!th) {
        free(ctx);
        return getaddrinfo(hostname, port_str, hints, out_result);
    }
    DWORD ms = timeout_usec / 1000;
    if (ms == 0)
        ms = 1;
    DWORD wr = WaitForSingleObject(th, ms);
    CloseHandle(th);

    if (wr == WAIT_OBJECT_0) {
        /* Worker's CAS already ran before its thread function returned. */
        *out_result = ctx->result;
        int ret = ctx->gai_ret;
        free(ctx);
        return ret;
    }

    long prev = InterlockedCompareExchange(&ctx->state, 2, 0);
    if (prev == 0)
        return -1; /* abandoned; worker frees ctx when it finishes */
    *out_result = ctx->result;
    int ret = ctx->gai_ret;
    free(ctx);
    return ret;
#else
    pthread_t th;
    if (pthread_create(&th, NULL, http_resolve_worker, ctx) != 0) {
        free(ctx);
        return getaddrinfo(hostname, port_str, hints, out_result);
    }
    pthread_detach(th);

    uint64_t waited = 0;
    while (waited < timeout_usec && ctx->state == 0) {
        useconds_t step = 5000;
        uint64_t remain = timeout_usec - waited;
        if ((uint64_t)step > remain)
            step = (useconds_t)remain;
        usleep(step);
        waited += step;
    }

    if (ctx->state != 0) {
        *out_result = ctx->result;
        int ret = ctx->gai_ret;
        free(ctx);
        return ret;
    }

    long prev = __sync_val_compare_and_swap(&ctx->state, 0, 2);
    if (prev == 0)
        return -1; /* abandoned; worker frees ctx when it finishes */
    *out_result = ctx->result;
    int ret = ctx->gai_ret;
    free(ctx);
    return ret;
#endif
}

/* Connect with a bound of *timeout_usec* microseconds: switch the socket to
 * non-blocking, kick off the connect, and wait for writability via select().
 * Restores blocking mode before returning either way -- the rest of the
 * transaction path assumes a blocking socket bounded by SO_SNDTIMEO/
 * SO_RCVTIMEO instead (see http_apply_timeout()).
 * timeout_usec == 0 means "no bound configured" -> connect inline as before.
 * Returns 0 ok, -1 connect failure, -2 timeout. */
static int http_connect_with_timeout(http_socket_t sock, const struct sockaddr* addr,
                                     int addrlen, u32 timeout_usec)
{
    if (timeout_usec == 0)
        return connect(sock, addr, addrlen) == 0 ? 0 : -1;

#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(sock, FIONBIO, &nb);
#else
    int orig_flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, orig_flags | O_NONBLOCK);
#endif

    int failed = 0;
    int timed_out = 0;
    int ret = connect(sock, addr, addrlen);
    if (ret != 0) {
#ifdef _WIN32
        int in_progress = (WSAGetLastError() == WSAEWOULDBLOCK);
#else
        int in_progress = (errno == EINPROGRESS);
#endif
        if (!in_progress) {
            failed = 1;
        } else {
            fd_set wfds, efds;
            FD_ZERO(&wfds);
            FD_ZERO(&efds);
            FD_SET(sock, &wfds);
            FD_SET(sock, &efds);
            struct timeval tv;
            tv.tv_sec  = (long)(timeout_usec / 1000000);
            tv.tv_usec = (long)(timeout_usec % 1000000);

            int sel = select((int)(sock + 1), NULL, &wfds, &efds, &tv);
            if (sel == 0) {
                failed = 1;
                timed_out = 1;
            } else if (sel < 0) {
                failed = 1;
            } else {
                int soerr = 0;
#ifdef _WIN32
                int soerr_len = sizeof(soerr);
#else
                socklen_t soerr_len = sizeof(soerr);
#endif
                getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&soerr, &soerr_len);
                if (soerr != 0)
                    failed = 1;
            }
        }
    }

#ifdef _WIN32
    nb = 0;
    ioctlsocket(sock, FIONBIO, &nb);
#else
    fcntl(sock, F_SETFL, orig_flags);
#endif
    if (!failed)
        return 0;
    return timed_out ? -2 : -1;
}

/* ---------------------------------------------------------------------------
 * Response header parsing
 * -----------------------------------------------------------------------*/

/* Find "\r\n\r\n" in a buffer, return pointer to start of body or NULL. */
static const char* http_find_header_end(const char* buf, u32 len)
{
    if (len < 4)
        return NULL;
    for (u32 i = 0; i <= len - 4; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' &&
            buf[i+2] == '\r' && buf[i+3] == '\n')
            return buf + i + 4;
    }
    return NULL;
}

/* ANCHORED header lookup in the stored raw header block: the name must
 * start a line and be followed immediately by ':'.  (The old substring
 * search let "X-Original-Content-Length" satisfy a "Content-Length"
 * query -- audit 2026-08-04 MEDIUM.)  Returns 1 with *out_val/*out_len
 * pointing at the trimmed value bytes (NOT NUL-terminated), and, if
 * out_name_len is non-NULL, the on-wire name length. */
static int http_find_response_header(const HttpTransSlot* t, const char* name,
                                     const char** out_val, u32* out_len,
                                     const char** out_name, u32* out_name_len)
{
    size_t name_len = strlen(name);
    const char* p   = t->hdr_buf;
    const char* end = t->hdr_buf + t->hdr_len;

    /* skip the status line */
    while (p < end && *p != '\n')
        p++;
    if (p < end)
        p++;

    while (p < end) {
        const char* line = p;
        const char* eol  = line;
        while (eol < end && *eol != '\n')
            eol++;
        /* line spans [line, eol), typically ending in \r */
        size_t avail = (size_t)(eol - line);
        if (avail > name_len && line[name_len] == ':' &&
            http_strieq_n(line, name, name_len)) {
            const char* v = line + name_len + 1;
            while (v < eol && (*v == ' ' || *v == '\t'))
                v++;
            const char* ve = eol;
            if (ve > v && ve[-1] == '\r')
                ve--;
            while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t'))
                ve--;
            *out_val = v;
            *out_len = (u32)(ve - v);
            if (out_name)     *out_name = line;
            if (out_name_len) *out_name_len = (u32)name_len;
            return 1;
        }
        p = (eol < end) ? eol + 1 : eol;
    }
    return 0;
}

/* Parse the response headers accumulated in hdr_buf.  Extracts status code,
 * Content-Length, Connection: close, Transfer-Encoding: chunked.
 * Returns CELL_OK or a documented error. */
static s32 http_parse_response_headers(HttpTransSlot* t, const char* hdr_end)
{
    t->hdr_len = (u32)(hdr_end - t->hdr_buf);

    /* Parse status line: "HTTP/1.x SSS reason\r\n" */
    int major = 0, minor = 0, status = 0;
    if (sscanf(t->hdr_buf, "HTTP/%d.%d %d", &major, &minor, &status) < 3) {
        printf("[cellHttp]   Failed to parse status line\n");
        return (s32)CELL_HTTP_ERROR_INVALID_VALUE;
    }
    t->status_code = (s32)status;
    printf("[cellHttp]   Response status: %d\n", status);

    const char* val;
    u32 vlen;

    /* Transfer-Encoding: chunked (checked before Content-Length; a chunked
     * response has no usable Content-Length per RFC2616 4.4). */
    t->chunked = 0;
    if (http_find_response_header(t, "Transfer-Encoding", &val, &vlen, NULL, NULL)) {
        for (u32 i = 0; i + 7 <= vlen; i++) {
            if (http_strieq_n(val + i, "chunked", 7)) {
                t->chunked = 1;
                t->chunk_phase = HTTP_CHUNK_SIZE_LINE;
                t->chunk_remaining = 0;
                break;
            }
        }
    }

    /* Content-Length */
    t->content_length_known = 0;
    if (!t->chunked &&
        http_find_response_header(t, "Content-Length", &val, &vlen, NULL, NULL)) {
        char tmp[32];
        u32 n = vlen < sizeof(tmp) - 1 ? vlen : (u32)sizeof(tmp) - 1;
        memcpy(tmp, val, n);
        tmp[n] = '\0';
        t->content_length = (u64)strtoull(tmp, NULL, 10);
        t->content_length_known = 1;
        printf("[cellHttp]   Content-Length: %llu\n",
               (unsigned long long)t->content_length);
    }

    /* Connection: close */
    t->conn_close = 0;
    if (http_find_response_header(t, "Connection", &val, &vlen, NULL, NULL)) {
        for (u32 i = 0; i + 5 <= vlen; i++) {
            if (http_strieq_n(val + i, "close", 5)) {
                t->conn_close = 1;
                break;
            }
        }
    }

    /* Store leftover body bytes that arrived with the header read */
    u32 body_start = t->hdr_len;
    u32 leftover   = t->hdr_buf_len - body_start;
    if (leftover > 0) {
        t->body_overflow = (char*)malloc(leftover);
        if (t->body_overflow) {
            memcpy(t->body_overflow, t->hdr_buf + body_start, leftover);
            t->body_overflow_len = leftover;
        }
    }

    t->headers_parsed = 1;
    return CELL_OK;
}

/* Read from the socket until the full header section is buffered, then
 * parse it.  Returns CELL_OK or a documented error. */
static s32 http_read_response_headers(HttpTransSlot* t)
{
    if (t->headers_parsed)
        return CELL_OK;
    if (t->sock == HTTP_INVALID_SOCKET)
        return (s32)CELL_HTTP_ERROR_NO_CONNECTION;

    while (t->hdr_buf_len < HTTP_HDR_BUF_SIZE - 1) {
        int n = recv(t->sock, t->hdr_buf + t->hdr_buf_len,
                     (int)(HTTP_HDR_BUF_SIZE - 1 - t->hdr_buf_len), 0);
        if (n < 0) {
            printf("[cellHttp]   recv() failed during header read\n");
            http_close_slot_socket(t);
            return (s32)CELL_HTTP_ERROR_NET_RECV;
        }
        if (n == 0) {
            /* EOF before the header section completed.
             * ORACLE(cell/http/error.h:345-347): NET_FIN. */
            printf("[cellHttp]   EOF during header read\n");
            http_close_slot_socket(t);
            return (s32)CELL_HTTP_ERROR_NET_FIN;
        }
        t->hdr_buf_len += (u32)n;

        const char* hdr_end = http_find_header_end(t->hdr_buf, t->hdr_buf_len);
        if (hdr_end) {
            s32 rc = http_parse_response_headers(t, hdr_end);
            if (rc != CELL_OK)
                http_close_slot_socket(t);
            return rc;
        }
    }

    /* Header section exceeds our buffer.
     * ORACLE(cell/http/error.h:158-160): LINE_EXCEEDS_MAX. */
    printf("[cellHttp]   Header section exceeds %u bytes\n",
           (unsigned)HTTP_HDR_BUF_SIZE);
    http_close_slot_socket(t);
    return (s32)CELL_HTTP_ERROR_LINE_EXCEEDS_MAX;
}

/* ---------------------------------------------------------------------------
 * Raw body byte source (overflow-then-socket)
 * -----------------------------------------------------------------------*/

/* Returns >0 bytes read, 0 on EOF, -1 on socket error. */
static int http_body_read_raw(HttpTransSlot* t, char* dst, u32 want)
{
    if (want == 0)
        return 0;

    if (t->body_overflow && t->body_overflow_len > 0) {
        u32 copy = t->body_overflow_len < want ? t->body_overflow_len : want;
        memcpy(dst, t->body_overflow, copy);
        u32 remain = t->body_overflow_len - copy;
        if (remain > 0)
            memmove(t->body_overflow, t->body_overflow + copy, remain);
        else {
            free(t->body_overflow);
            t->body_overflow = NULL;
        }
        t->body_overflow_len = remain;
        return (int)copy;
    }

    if (t->sock == HTTP_INVALID_SOCKET)
        return 0; /* connection already gone -> EOF */

    return recv(t->sock, dst, (int)want, 0);
}

/* ---------------------------------------------------------------------------
 * Chunked transfer decoding (audit 2026-08-04 HIGH: previously absent while
 * advertising HTTP/1.1).  Delivers decoded body bytes; recvd==0 signals
 * "end of chunks" per ORACLE(libhttp-Reference_e.pdf p.121).
 * -----------------------------------------------------------------------*/

/* Read one text line (through '\n') from the body source. Returns 0 on
 * success with line (CR/LF stripped) in buf, -1 on socket error, -2 on
 * EOF, -3 on oversize. */
static int http_read_chunk_line(HttpTransSlot* t, char* buf, u32 bufsz)
{
    u32 pos = 0;
    for (;;) {
        char c;
        int n = http_body_read_raw(t, &c, 1);
        if (n < 0)  return -1;
        if (n == 0) return -2;
        if (c == '\n') {
            while (pos > 0 && (buf[pos-1] == '\r'))
                pos--;
            buf[pos] = '\0';
            return 0;
        }
        if (pos + 1 >= bufsz)
            return -3;
        buf[pos++] = c;
    }
}

/* Decode chunked body bytes into out (up to size). Returns CELL_OK with
 * *filled set; end-of-chunks yields *filled possibly 0. */
static s32 http_chunked_read(HttpTransSlot* t, char* out, u32 size, u32* filled)
{
    *filled = 0;
    char line[128];

    while (*filled < size) {
        switch (t->chunk_phase) {
        case HTTP_CHUNK_SIZE_LINE: {
            int rc = http_read_chunk_line(t, line, sizeof(line));
            if (rc == -2) {
                /* connection lost mid-chunk-framing.
                 * ORACLE(cell/http/error.h:203-205): BROKEN_CHUNK. */
                return (s32)CELL_HTTP_ERROR_BROKEN_CHUNK;
            }
            if (rc == -1) return (s32)CELL_HTTP_ERROR_NET_RECV;
            if (rc == -3) return (s32)CELL_HTTP_ERROR_LINE_EXCEEDS_MAX;
            /* chunk-size [;chunk-ext] */
            char* endp = NULL;
            unsigned long long sz = strtoull(line, &endp, 16);
            if (endp == line)
                return (s32)CELL_HTTP_ERROR_BROKEN_CHUNK;
            if (sz == 0) {
                t->chunk_phase = HTTP_CHUNK_TRAILER;
            } else {
                t->chunk_remaining = (u64)sz;
                t->chunk_phase = HTTP_CHUNK_DATA;
            }
            break;
        }
        case HTTP_CHUNK_DATA: {
            u32 want = size - *filled;
            if ((u64)want > t->chunk_remaining)
                want = (u32)t->chunk_remaining;
            int n = http_body_read_raw(t, out + *filled, want);
            if (n < 0)  return (s32)CELL_HTTP_ERROR_NET_RECV;
            if (n == 0) return (s32)CELL_HTTP_ERROR_BROKEN_CHUNK;
            *filled += (u32)n;
            t->body_received += (u64)n;
            t->chunk_remaining -= (u64)n;
            if (t->chunk_remaining == 0)
                t->chunk_phase = HTTP_CHUNK_DATA_CRLF;
            break;
        }
        case HTTP_CHUNK_DATA_CRLF: {
            int rc = http_read_chunk_line(t, line, sizeof(line));
            if (rc == -2) return (s32)CELL_HTTP_ERROR_BROKEN_CHUNK;
            if (rc == -1) return (s32)CELL_HTTP_ERROR_NET_RECV;
            if (rc == -3) return (s32)CELL_HTTP_ERROR_LINE_EXCEEDS_MAX;
            t->chunk_phase = HTTP_CHUNK_SIZE_LINE;
            break;
        }
        case HTTP_CHUNK_TRAILER: {
            int rc = http_read_chunk_line(t, line, sizeof(line));
            if (rc == -1) return (s32)CELL_HTTP_ERROR_NET_RECV;
            if (rc == -3) return (s32)CELL_HTTP_ERROR_LINE_EXCEEDS_MAX;
            if (rc == -2 || line[0] == '\0') {
                /* blank line (or EOF right at the end) terminates trailers */
                t->chunk_phase = HTTP_CHUNK_DONE;
                t->eof_reached = 1;
                return CELL_OK;
            }
            break;
        }
        case HTTP_CHUNK_DONE:
        default:
            return CELL_OK; /* *filled stays 0 -> "end of chunks" */
        }
    }
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Request build + send
 * -----------------------------------------------------------------------*/

#define HTTP_REQ_BUF_SIZE 8192

/* Append to the request buffer with hard bounds checking (the old code
 * accumulated snprintf() would-have-written counts -- audit MEDIUM). */
static int http_req_append(char* buf, int* len, const char* fmt, ...)
{
    if (*len < 0 || *len >= HTTP_REQ_BUF_SIZE)
        return -1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *len, (size_t)(HTTP_REQ_BUF_SIZE - *len), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= HTTP_REQ_BUF_SIZE - *len)
        return -1;
    *len += n;
    return 0;
}

/* Resolve + connect + send the request (headers and optional body).
 * On success the socket is stored in t->sock ready for response reading. */
static s32 http_perform_request(HttpTransSlot* t, const HttpClientSlot* c,
                                const void* body, u32 size)
{
    /* Close any previously open socket (re-send / redirect scenario) */
    http_close_slot_socket(t);
    http_reset_response_state(t);

    /* ---- Resolve hostname (bounded by the connection timeout) ---- */
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", t->port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    u32 conn_usec = (c->conn_timeout_usec > 0 && c->conn_timeout_usec < 0xFFFFFFFFll)
                        ? (u32)c->conn_timeout_usec : 0;

    struct addrinfo* result = NULL;
    int gai = http_resolve_with_timeout(t->hostname, port_str, &hints,
                                        conn_usec, &result);
    if (gai != 0 || !result) {
        printf("[cellHttp]   getaddrinfo failed for '%s': %d\n", t->hostname, gai);
        if (result) freeaddrinfo(result);
        /* ORACLE(cell/http/error.h:361-366): resolver failures are the
         * NET_RESOLVER class (low bits carry h_errno in the real lib). */
        return (s32)CELL_HTTP_ERROR_NET_RESOLVER;
    }

    /* ---- Create socket and connect ---- */
    http_socket_t sock = socket(result->ai_family, result->ai_socktype,
                                result->ai_protocol);
    if (sock == HTTP_INVALID_SOCKET) {
        printf("[cellHttp]   socket() failed\n");
        freeaddrinfo(result);
        return (s32)CELL_HTTP_ERROR_NET_SOCKET;
    }

    /* Apply timeouts from the owning client */
    http_apply_timeout(sock, 1, c->send_timeout_usec);
    http_apply_timeout(sock, 0, c->recv_timeout_usec);

    int crc = http_connect_with_timeout(sock, result->ai_addr,
                                        (int)result->ai_addrlen, conn_usec);
    if (crc != 0) {
        printf("[cellHttp]   connect() %s to %s:%u\n",
               crc == -2 ? "timed out" : "failed", t->hostname, t->port);
        http_closesocket(sock);
        freeaddrinfo(result);
        return crc == -2 ? (s32)CELL_HTTP_ERROR_NET_CONNECT_TIMEOUT
                         : (s32)CELL_HTTP_ERROR_NET_CONNECT;
    }

    freeaddrinfo(result);
    printf("[cellHttp]   Connected to %s:%u\n", t->hostname, t->port);

    /* ---- Format HTTP request (bounds-checked) ---- */
    char req_buf[HTTP_REQ_BUF_SIZE];
    int  req_len = 0;
    int  bad = 0;

    bad |= http_req_append(req_buf, &req_len, "%s %s HTTP/1.1\r\nHost: %s\r\n",
                           t->method, t->path, t->hostname);

    /* Content-Length header if body provided or explicitly set */
    if (size > 0) {
        bad |= http_req_append(req_buf, &req_len, "Content-Length: %u\r\n", size);
    } else if (t->request_content_length_set) {
        bad |= http_req_append(req_buf, &req_len, "Content-Length: %llu\r\n",
                               (unsigned long long)t->request_content_length);
    }

    /* Basic credentials on an auth retry */
    if (t->auth_applied)
        bad |= http_req_append(req_buf, &req_len, "%s\r\n", t->auth_header);

    /* Custom request headers */
    for (u32 i = 0; i < t->custom_header_count; i++) {
        bad |= http_req_append(req_buf, &req_len, "%s: %s\r\n",
                               t->custom_headers[i].name,
                               t->custom_headers[i].value);
    }

    bad |= http_req_append(req_buf, &req_len, "\r\n");

    if (bad) {
        printf("[cellHttp]   Request exceeds %u-byte build buffer\n",
               (unsigned)HTTP_REQ_BUF_SIZE);
        http_closesocket(sock);
        return (s32)CELL_HTTP_ERROR_NO_MEMORY;
    }

    /* ---- Send header ---- */
    if (http_send_all(sock, req_buf, (u32)req_len) != 0) {
        printf("[cellHttp]   Failed to send request headers\n");
        http_closesocket(sock);
        return (s32)CELL_HTTP_ERROR_NET_SEND;
    }

    /* ---- Send body if provided ---- */
    if (body && size > 0) {
        if (http_send_all(sock, (const char*)body, size) != 0) {
            printf("[cellHttp]   Failed to send request body\n");
            http_closesocket(sock);
            return (s32)CELL_HTTP_ERROR_NET_SEND;
        }
    }

    printf("[cellHttp]   Request sent (%d header bytes + %u body bytes)\n",
           req_len, size);

    t->sock = sock;
    return CELL_OK;
}

/* Base64 for the Basic-auth header (RFC4648 alphabet). */
static void http_base64(const char* in, size_t inlen, char* out, size_t outsz)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t pos = 0;
    for (size_t i = 0; i < inlen && pos + 4 < outsz; i += 3) {
        u32 v = ((u32)(u8)in[i]) << 16;
        if (i + 1 < inlen) v |= ((u32)(u8)in[i+1]) << 8;
        if (i + 2 < inlen) v |= (u32)(u8)in[i+2];
        out[pos++] = tbl[(v >> 18) & 0x3F];
        out[pos++] = tbl[(v >> 12) & 0x3F];
        out[pos++] = (i + 1 < inlen) ? tbl[(v >> 6) & 0x3F] : '=';
        out[pos++] = (i + 2 < inlen) ? tbl[v & 0x3F] : '=';
    }
    out[pos] = '\0';
}

/* Parse a Location value (absolute http URI or absolute path) into the
 * transaction target.  Returns 1 on success, 0 if unusable (https target,
 * malformed, or unsupported scheme). */
static int http_apply_redirect_location(HttpTransSlot* t, const char* loc, u32 len)
{
    char buf[1024];
    if (len >= sizeof(buf))
        return 0;
    memcpy(buf, loc, len);
    buf[len] = '\0';

    if (http_strieq_n(buf, "https://", 8)) {
        /* No TLS in this build: do not follow into https -- surface the
         * 3xx response to the guest instead of downgrading. */
        return 0;
    }
    if (http_strieq_n(buf, "http://", 7)) {
        const char* p = buf + 7;
        const char* host_end = p;
        while (*host_end && *host_end != '/' && *host_end != ':' && *host_end != '?')
            host_end++;
        size_t hlen = (size_t)(host_end - p);
        if (hlen == 0 || hlen >= sizeof(t->hostname))
            return 0;
        memcpy(t->hostname, p, hlen);
        t->hostname[hlen] = '\0';
        u32 port = 80;
        const char* rest = host_end;
        if (*rest == ':') {
            port = 0;
            rest++;
            while (*rest >= '0' && *rest <= '9')
                port = port * 10 + (u32)(*rest++ - '0');
            if (port == 0 || port > 65535)
                return 0;
        }
        t->port = port;
        if (*rest == '\0')
            strcpy(t->path, "/");
        else {
            if (strlen(rest) >= sizeof(t->path))
                return 0;
            strcpy(t->path, rest);
        }
        strcpy(t->scheme, "http");
        t->is_https = 0;
        return 1;
    }
    if (buf[0] == '/') {
        if (len >= sizeof(t->path))
            return 0;
        strcpy(t->path, buf);
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellHttpInit(void* pool, u32 poolSize)
{
    /* The real library carves clients/transactions out of this pool; we use
     * fixed caps and only require the argument shape. */
    printf("[cellHttp] Init(pool=%p, poolSize=%u)\n", pool, poolSize);

    if (s_http_initialized)
        return (s32)CELL_HTTP_ERROR_ALREADY_INITIALIZED;

#ifdef _WIN32
    if (!s_wsa_initialized) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            printf("[cellHttp] WSAStartup failed\n");
            return (s32)CELL_HTTP_ERROR_INTERNAL;
        }
        s_wsa_initialized = 1;
    }
#endif

    memset(s_clients, 0, sizeof(s_clients));
    memset(s_transactions, 0, sizeof(s_transactions));

    /* Ensure all sockets are marked invalid */
    for (u32 i = 0; i < CELL_HTTP_MAX_TRANSACTIONS; i++) {
        s_transactions[i].sock = HTTP_INVALID_SOCKET;
    }

    s_http_initialized = 1;
    return CELL_OK;
}

s32 cellHttpEnd(void)
{
    printf("[cellHttp] End()\n");

    if (!s_http_initialized)
        return (s32)CELL_HTTP_ERROR_NOT_INITIALIZED;

    /* Close any open sockets and free overflow buffers */
    for (u32 i = 0; i < CELL_HTTP_MAX_TRANSACTIONS; i++) {
        if (s_transactions[i].in_use) {
            http_close_slot_socket(&s_transactions[i]);
            http_reset_response_state(&s_transactions[i]);
        }
    }

    memset(s_clients, 0, sizeof(s_clients));
    memset(s_transactions, 0, sizeof(s_transactions));

    /* Re-invalidate sockets after memset */
    for (u32 i = 0; i < CELL_HTTP_MAX_TRANSACTIONS; i++) {
        s_transactions[i].sock = HTTP_INVALID_SOCKET;
    }

    s_http_initialized = 0;

#ifdef _WIN32
    if (s_wsa_initialized) {
        WSACleanup();
        s_wsa_initialized = 0;
    }
#endif

    return CELL_OK;
}

s32 cellHttpInitCookie(void* pool, u32 poolSize)
{
    (void)pool;
    printf("[cellHttp] InitCookie(poolSize=%u)\n", poolSize);
    if (!s_http_initialized)
        return (s32)CELL_HTTP_ERROR_NOT_INITIALIZED;
    if (s_cookie_initialized)
        return (s32)CELL_HTTP_ERROR_CACHE_ALREADY_INITIALIZED;
    s_cookie_initialized = 1;
    return CELL_OK;
}

s32 cellHttpEndCookie(void)
{
    printf("[cellHttp] EndCookie()\n");
    if (!s_cookie_initialized)
        return (s32)CELL_HTTP_ERROR_CACHE_NOT_INITIALIZED;
    s_cookie_initialized = 0;
    return CELL_OK;
}

s32 cellHttpCreateClient(u32* clientId)
{
    if (!s_http_initialized)
        return (s32)CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (!clientId)
        return (s32)CELL_HTTP_ERROR_NO_BUFFER;

    for (u32 i = 0; i < CELL_HTTP_MAX_CLIENTS; i++) {
        if (!s_clients[i].in_use) {
            HttpClientSlot* c = &s_clients[i];
            memset(c, 0, sizeof(*c));
            c->in_use = 1;
            /* Defaults CONFIRMED CORRECT by the 2026-08-04 audit */
            c->conn_timeout_usec = 30000000ll;   /* 30s  */
            c->send_timeout_usec = 120000000ll;  /* 120s */
            c->recv_timeout_usec = 120000000ll;  /* 120s */
            /* ORACLE(libhttp-Reference_e.pdf p.47/p.51): both enabled */
            c->auto_redirect = 1;
            c->auto_auth     = 1;
            *clientId = ps3_bswap32(i + 1);      /* guest BE; handles 1-based */
            printf("[cellHttp] CreateClient(id=%u)\n", i + 1);
            return CELL_OK;
        }
    }

    return (s32)CELL_HTTP_ERROR_NO_MEMORY;
}

s32 cellHttpDestroyClient(u32 clientId)
{
    if (!s_http_initialized)
        return (s32)CELL_HTTP_ERROR_NOT_INITIALIZED;

    HttpClientSlot* c = http_client_from_handle(clientId);
    if (!c)
        return (s32)CELL_HTTP_ERROR_BAD_CLIENT;

    /* Destroy all transactions belonging to this client */
    for (u32 i = 0; i < CELL_HTTP_MAX_TRANSACTIONS; i++) {
        if (s_transactions[i].in_use && s_transactions[i].client == clientId) {
            http_close_slot_socket(&s_transactions[i]);
            http_reset_response_state(&s_transactions[i]);
            s_transactions[i].in_use = 0;
        }
    }

    c->in_use = 0;
    printf("[cellHttp] DestroyClient(id=%u)\n", clientId);
    return CELL_OK;
}

/* ---- client setters/getters -------------------------------------------- */

s32 cellHttpClientSetAutoRedirect(u32 clientId, u32 enable)
{
    if (!s_http_initialized) return (s32)CELL_HTTP_ERROR_NOT_INITIALIZED;
    HttpClientSlot* c = http_client_from_handle(clientId);
    if (!c) return (s32)CELL_HTTP_ERROR_BAD_CLIENT;
    c->auto_redirect = enable ? 1 : 0;
    printf("[cellHttp] ClientSetAutoRedirect(id=%u, %u)\n", clientId, enable ? 1u : 0u);
    return CELL_OK;
}

s32 cellHttpClientGetAutoRedirect(u32 clientId, u8* enable)
{
    if (!s_http_initialized) return (s32)CELL_HTTP_ERROR_NOT_INITIALIZED;
    HttpClientSlot* c = http_client_from_handle(clientId);
    if (!c) return (s32)CELL_HTTP_ERROR_BAD_CLIENT;
    if (!enable) return (s32)CELL_HTTP_ERROR_NO_BUFFER;
    *enable = c->auto_redirect ? 1 : 0;  /* single byte, no swap needed */
    return CELL_OK;
}

s32 cellHttpClientSetAutoAuthentication(u32 clientId, u32 enable)
{
    if (!s_http_initialized) return (s32)CELL_HTTP_ERROR_NOT_INITIALIZED;
    HttpClientSlot* c = http_client_from_handle(clientId);
    if (!c) return (s32)CELL_HTTP_ERROR_BAD_CLIENT;
    c->auto_auth = enable ? 1 : 0;
    printf("[cellHttp] ClientSetAutoAuthentication(id=%u, %u)\n", clientId, enable ? 1u : 0u);
    return CELL_OK;
}

s32 cellHttpClientGetAutoAuthentication(u32 clientId, u8* enable)
{
    if (!s_http_initialized) return (s32)CELL_HTTP_ERROR_NOT_INITIALIZED;
    HttpClientSlot* c = http_client_from_handle(clientId);
    if (!c) return (s32)CELL_HTTP_ERROR_BAD_CLIENT;
    if (!enable) return (s32)CELL_HTTP_ERROR_NO_BUFFER;
    *enable = c->auto_auth ? 1 : 0;
    return CELL_OK;
}

s32 cellHttpClientSetCookieStatus(u32 clientId, u32 enable)
{
    if (!s_http_initialized) return (s32)CELL_HTTP_ERROR_NOT_INITIALIZED;
    HttpClientSlot* c = http_client_from_handle(clientId);
    if (!c) return (s32)CELL_HTTP_ERROR_BAD_CLIENT;
    c->cookie_enabled = enable ? 1 : 0;
    printf("[cellHttp] ClientSetCookieStatus(id=%u, %u)\n", clientId, enable ? 1u : 0u);
    return CELL_OK;
}

s32 cellHttpClientSetCookieSendCallback(u32 clientId, u32 cbfunc, u32 userArg)
{
    if (!s_http_initialized) return (s32)CELL_HTTP_ERROR_NOT_INITIALIZED;
    HttpClientSlot* c = http_client_from_handle(clientId);
    if (!c) return (s32)CELL_HTTP_ERROR_BAD_CLIENT;
    c->cookie_send_cb     = cbfunc;   /* raw guest values; never invoked */
    c->cookie_send_cb_arg = userArg;
    printf("[cellHttp] ClientSetCookieSendCallback(id=%u)\n", clientId);
    return CELL_OK;
}

s32 cellHttpClientSetCookieRecvCallback(u32 clientId, u32 cbfunc, u32 userArg)
{
    if (!s_http_initialized) return (s32)CELL_HTTP_ERROR_NOT_INITIALIZED;
    HttpClientSlot* c = http_client_from_handle(clientId);
    if (!c) return (s32)CELL_HTTP_ERROR_BAD_CLIENT;
    c->cookie_recv_cb     = cbfunc;
    c->cookie_recv_cb_arg = userArg;
    printf("[cellHttp] ClientSetCookieRecvCallback(id=%u)\n", clientId);
    return CELL_OK;
}

s32 cellHttpClientSetSslCallback(u32 clientId, u32 cbfunc, u32 userArg)
{
    if (!s_http_initialized) return (s32)CELL_HTTP_ERROR_NOT_INITIALIZED;
    HttpClientSlot* c = http_client_from_handle(clientId);
    if (!c) return (s32)CELL_HTTP_ERROR_BAD_CLIENT;
    c->ssl_cb     = cbfunc;   /* stored only: no TLS verify path exists */
    c->ssl_cb_arg = userArg;
    printf("[cellHttp] ClientSetSslCallback(id=%u)\n", clientId);
    return CELL_OK;
}

s32 cellHttpClientSetConnTimeout(u32 clientId, s64 usec)
{
    if (!s_http_initialized) return (s32)CELL_HTTP_ERROR_NOT_INITIALIZED;
    HttpClientSlot* c = http_client_from_handle(clientId);
    if (!c) return (s32)CELL_HTTP_ERROR_BAD_CLIENT;
    c->conn_timeout_usec = usec;
    printf("[cellHttp] ClientSetConnTimeout(id=%u, %lld us)\n", clientId,
           (long long)usec);
    return CELL_OK;
}

s32 cellHttpClientGetConnTimeout(u32 clientId, s64* usec)
{
    if (!s_http_initialized) return (s32)CELL_HTTP_ERROR_NOT_INITIALIZED;
    HttpClientSlot* c = http_client_from_handle(clientId);
    if (!c) return (s32)CELL_HTTP_ERROR_BAD_CLIENT;
    if (!usec) return (s32)CELL_HTTP_ERROR_NO_BUFFER;
    *usec = (s64)ps3_bswap64((u64)c->conn_timeout_usec);  /* guest BE */
    return CELL_OK;
}

s32 cellHttpClientSetSendTimeout(u32 clientId, s64 usec)
{
    if (!s_http_initialized) return (s32)CELL_HTTP_ERROR_NOT_INITIALIZED;
    HttpClientSlot* c = http_client_from_handle(clientId);
    if (!c) return (s32)CELL_HTTP_ERROR_BAD_CLIENT;
    c->send_timeout_usec = usec;
    printf("[cellHttp] ClientSetSendTimeout(id=%u, %lld us)\n", clientId,
           (long long)usec);
    return CELL_OK;
}

s32 cellHttpClientGetSendTimeout(u32 clientId, s64* usec)
{
    if (!s_http_initialized) return (s32)CELL_HTTP_ERROR_NOT_INITIALIZED;
    HttpClientSlot* c = http_client_from_handle(clientId);
    if (!c) return (s32)CELL_HTTP_ERROR_BAD_CLIENT;
    if (!usec) return (s32)CELL_HTTP_ERROR_NO_BUFFER;
    *usec = (s64)ps3_bswap64((u64)c->send_timeout_usec);
    return CELL_OK;
}

s32 cellHttpClientSetRecvTimeout(u32 clientId, s64 usec)
{
    if (!s_http_initialized) return (s32)CELL_HTTP_ERROR_NOT_INITIALIZED;
    HttpClientSlot* c = http_client_from_handle(clientId);
    if (!c) return (s32)CELL_HTTP_ERROR_BAD_CLIENT;
    c->recv_timeout_usec = usec;
    printf("[cellHttp] ClientSetRecvTimeout(id=%u, %lld us)\n", clientId,
           (long long)usec);
    return CELL_OK;
}

s32 cellHttpClientGetRecvTimeout(u32 clientId, s64* usec)
{
    if (!s_http_initialized) return (s32)CELL_HTTP_ERROR_NOT_INITIALIZED;
    HttpClientSlot* c = http_client_from_handle(clientId);
    if (!c) return (s32)CELL_HTTP_ERROR_BAD_CLIENT;
    if (!usec) return (s32)CELL_HTTP_ERROR_NO_BUFFER;
    *usec = (s64)ps3_bswap64((u64)c->recv_timeout_usec);
    return CELL_OK;
}

/* ---- transactions ------------------------------------------------------- */

s32 cellHttpCreateTransaction(u32* transId, u32 clientId, const char* method,
                              const CellHttpUri* uri)
{
    if (!s_http_initialized)
        return (s32)CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (!transId || !uri)
        return (s32)CELL_HTTP_ERROR_NO_BUFFER;
    if (!method || !method[0])
        return (s32)CELL_HTTP_ERROR_BAD_METHOD;

    if (!http_client_from_handle(clientId))
        return (s32)CELL_HTTP_ERROR_BAD_CLIENT;

    /* CellHttpUri is a GUEST struct: five big-endian 4-byte EAs plus a
     * big-endian port (ORACLE(cell/http/util.h:12-20)).  Translate each EA
     * separately -- the old code declared host pointers and fused adjacent
     * fields into wild 64-bit pointers (audit 2026-08-04 HIGH). */
    u32 scheme_ea   = ps3_bswap32(uri->scheme);
    u32 hostname_ea = ps3_bswap32(uri->hostname);
    u32 username_ea = ps3_bswap32(uri->username);
    u32 password_ea = ps3_bswap32(uri->password);
    u32 path_ea     = ps3_bswap32(uri->path);
    u32 port        = ps3_bswap32(uri->port);

    const char* scheme   = scheme_ea   ? (const char*)vm_to_host(scheme_ea)   : NULL;
    const char* hostname = hostname_ea ? (const char*)vm_to_host(hostname_ea) : NULL;
    const char* username = username_ea ? (const char*)vm_to_host(username_ea) : NULL;
    const char* password = password_ea ? (const char*)vm_to_host(password_ea) : NULL;
    const char* path     = path_ea     ? (const char*)vm_to_host(path_ea)     : NULL;

    if (!hostname || !hostname[0])
        return (s32)CELL_HTTP_ERROR_INVALID_URI;

    for (u32 i = 0; i < CELL_HTTP_MAX_TRANSACTIONS; i++) {
        if (!s_transactions[i].in_use) {
            HttpTransSlot* t = &s_transactions[i];
            memset(t, 0, sizeof(*t));

            t->in_use  = 1;
            t->client  = clientId;
            t->sock    = HTTP_INVALID_SOCKET;

            strncpy(t->method, method, sizeof(t->method) - 1);
            strncpy(t->scheme, scheme ? scheme : "http", sizeof(t->scheme) - 1);
            strncpy(t->hostname, hostname, sizeof(t->hostname) - 1);
            strncpy(t->path, (path && path[0]) ? path : "/", sizeof(t->path) - 1);
            if (username) strncpy(t->username, username, sizeof(t->username) - 1);
            if (password) strncpy(t->password, password, sizeof(t->password) - 1);

            t->is_https = http_strieq_n(t->scheme, "https", 5) &&
                          t->scheme[5] == '\0';
            t->port = port ? port : (t->is_https ? 443u : 80u);

            *transId = ps3_bswap32(i + 1);   /* guest BE; handles 1-based */
            printf("[cellHttp] CreateTransaction(client=%u, %s %s://%s:%u%s) -> trans=%u\n",
                   clientId, t->method, t->scheme, t->hostname, t->port,
                   t->path, i + 1);
            return CELL_OK;
        }
    }

    return (s32)CELL_HTTP_ERROR_NO_MEMORY;
}

s32 cellHttpDestroyTransaction(u32 transId)
{
    if (!s_http_initialized)
        return (s32)CELL_HTTP_ERROR_NOT_INITIALIZED;

    HttpTransSlot* t = http_trans_from_handle(transId);
    if (!t)
        return (s32)CELL_HTTP_ERROR_BAD_TRANS;

    http_close_slot_socket(t);
    http_reset_response_state(t);
    t->in_use = 0;
    printf("[cellHttp] DestroyTransaction(trans=%u)\n", transId);
    return CELL_OK;
}

s32 cellHttpSendRequest(u32 transId, const void* buf, u32 size, u32* sent)
{
    if (!s_http_initialized)
        return (s32)CELL_HTTP_ERROR_NOT_INITIALIZED;

    HttpTransSlot* t = http_trans_from_handle(transId);
    if (!t)
        return (s32)CELL_HTTP_ERROR_BAD_TRANS;

    if (t->aborted) {
        printf("[cellHttp] SendRequest(trans=%u) - transaction aborted\n", transId);
        return (s32)CELL_HTTP_ERROR_TRANS_ABORTED;
    }

    if (t->is_https) {
        /* TLS is not implemented in this firmware-free build.  Fail the
         * transaction cleanly with the documented handshake error instead
         * of silently talking plaintext (audit 2026-08-04 HIGH).
         * ORACLE(cell/http/error.h:413-415): CELL_HTTPS_ERROR_HANDSHAKE. */
        printf("[cellHttp] SendRequest(trans=%u) - https unsupported (no TLS)\n",
               transId);
        if (sent) *sent = ps3_bswap32(0u);
        return (s32)CELL_HTTPS_ERROR_HANDSHAKE;
    }

    HttpClientSlot* c = http_client_from_handle(t->client);
    if (!c)
        return (s32)CELL_HTTP_ERROR_BAD_CLIENT;

    printf("[cellHttp] SendRequest(trans=%u, %s %s, bodySize=%u)\n",
           transId, t->method, t->path, size);

    /* Send + read response headers, following redirects/auth inside the
     * library per ORACLE(libhttp-Reference_e.pdf p.47): "Request sending/
     * response receiving for automatic redirection is performed within the
     * library" and TOO_MANY_REDIRECTS surfaces from the send function. */
    const void* body      = buf;
    u32         body_size = size;
    t->redirect_count = 0;
    t->auth_count     = 0;

    for (;;) {
        s32 rc = http_perform_request(t, c, body, body_size);
        if (rc != CELL_OK) {
            if (sent) *sent = ps3_bswap32(0u);
            return rc;
        }
        t->request_sent = 1;

        rc = http_read_response_headers(t);
        if (rc != CELL_OK) {
            if (sent) *sent = ps3_bswap32(0u);
            return rc;
        }

        s32 st = t->status_code;

        /* --- documented auto-redirect rules (Reference p.47):
         *     304/306 never; 301/302/303/305/307 always; other 3xx only
         *     for GET/HEAD; at most 5 attempts. --- */
        if (c->auto_redirect && st >= 300 && st <= 399 &&
            st != 304 && st != 306) {
            int always = (st == 301 || st == 302 || st == 303 || st == 305 ||
                          st == 307);
            int get_head = (strcmp(t->method, "GET") == 0 ||
                            strcmp(t->method, "HEAD") == 0);
            if (always || get_head) {
                const char* loc; u32 loclen;
                if (http_find_response_header(t, "Location", &loc, &loclen,
                                              NULL, NULL) && loclen > 0) {
                    if (t->redirect_count >= 5) {
                        if (sent) *sent = ps3_bswap32(0u);
                        return (s32)CELL_HTTP_ERROR_TOO_MANY_REDIRECTS;
                    }
                    /* Copy Location out before the retarget resets state */
                    char locbuf[1024];
                    u32 n = loclen < sizeof(locbuf) - 1 ? loclen
                                                        : (u32)sizeof(locbuf) - 1;
                    memcpy(locbuf, loc, n);
                    if (http_apply_redirect_location(t, locbuf, n)) {
                        t->redirect_count++;
                        if (st == 303) {
                            /* RFC2616: 303 continues with GET, no body */
                            strncpy(t->method, "GET", sizeof(t->method) - 1);
                            body      = NULL;
                            body_size = 0;
                        }
                        printf("[cellHttp]   redirect %u -> %s:%u%s\n",
                               t->redirect_count, t->hostname, t->port, t->path);
                        continue;
                    }
                    /* Unfollowable target (e.g. https): surface the 3xx */
                }
            }
        }

        /* --- documented auto-authentication (Reference p.51): use URI
         *     credentials first; guest callbacks are unreachable from HLE,
         *     so without usable credentials this is the documented
         *     "not enough info" failure. --- */
        if (c->auto_auth && st == 401) {
            const char* ch; u32 chlen;
            int basic = 0;
            if (http_find_response_header(t, "WWW-Authenticate", &ch, &chlen,
                                          NULL, NULL)) {
                for (u32 k = 0; k + 5 <= chlen; k++) {
                    if (http_strieq_n(ch + k, "Basic", 5)) { basic = 1; break; }
                }
            }
            if (basic && t->username[0] && !t->auth_applied) {
                char creds[520];
                snprintf(creds, sizeof(creds), "%s:%s", t->username, t->password);
                char b64[700];
                http_base64(creds, strlen(creds), b64, sizeof(b64));
                snprintf(t->auth_header, sizeof(t->auth_header),
                         "Authorization: Basic %s", b64);
                t->auth_applied = 1;
                if (++t->auth_count > 5) {
                    if (sent) *sent = ps3_bswap32(0u);
                    return (s32)CELL_HTTP_ERROR_TOO_MANY_AUTHS;
                }
                printf("[cellHttp]   retrying with Basic credentials\n");
                continue;
            }
            if (sent) *sent = ps3_bswap32(0u);
            return (s32)CELL_HTTP_ERROR_CANNOT_AUTHENTICATE;
        }

        break;
    }

    if (sent)
        *sent = ps3_bswap32(size);   /* guest BE out-param */

    return CELL_OK;
}

s32 cellHttpRecvResponse(u32 transId, void* buf, u32 size, u32* recvd)
{
    if (!s_http_initialized)
        return (s32)CELL_HTTP_ERROR_NOT_INITIALIZED;

    HttpTransSlot* t = http_trans_from_handle(transId);
    if (!t)
        return (s32)CELL_HTTP_ERROR_BAD_TRANS;

    if (t->aborted) {
        printf("[cellHttp] RecvResponse(trans=%u) - transaction aborted\n", transId);
        if (recvd) *recvd = ps3_bswap32(0u);
        return (s32)CELL_HTTP_ERROR_TRANS_ABORTED;
    }

    if (!t->request_sent) {
        /* ORACLE(cell/http/error.h:76-78): "no request has been sent yet" */
        printf("[cellHttp] RecvResponse(trans=%u) - no request sent\n", transId);
        if (recvd) *recvd = ps3_bswap32(0u);
        return (s32)CELL_HTTP_ERROR_NO_REQUEST_SENT;
    }

    /* Headers are normally parsed inside SendRequest already; fall back for
     * completeness (doc: getters/Recv read headers first if needed). */
    if (!t->headers_parsed) {
        s32 rc = http_read_response_headers(t);
        if (rc != CELL_OK) {
            if (recvd) *recvd = ps3_bswap32(0u);
            return rc;
        }
    }

    /* ---- Return body data ---- */
    if (!buf || size == 0) {
        if (recvd) *recvd = ps3_bswap32(0u);
        return CELL_OK;
    }

    u32 filled = 0;

    if (t->chunked) {
        s32 rc = http_chunked_read(t, (char*)buf, size, &filled);
        if (rc != CELL_OK) {
            if (recvd) *recvd = ps3_bswap32(0u);
            return rc;
        }
        if (recvd) *recvd = ps3_bswap32(filled);
        return CELL_OK;
    }

    /* Identity transfer: bounded by Content-Length when known */
    if (t->content_length_known && t->body_received >= t->content_length) {
        if (recvd) *recvd = ps3_bswap32(0u);
        return CELL_OK;
    }
    if (t->eof_reached) {
        if (recvd) *recvd = ps3_bswap32(0u);
        return CELL_OK;
    }

    while (filled < size) {
        u32 want = size - filled;
        if (t->content_length_known) {
            u64 remaining = t->content_length - t->body_received;
            if (remaining == 0)
                break;
            if ((u64)want > remaining)
                want = (u32)remaining;
        }
        if (want == 0)
            break;

        int n = http_body_read_raw(t, (char*)buf + filled, want);
        if (n < 0) {
            /* Error - if we already have some data, return it */
            if (filled > 0)
                break;
            printf("[cellHttp]   recv() error during body read\n");
            if (recvd) *recvd = ps3_bswap32(0u);
            return (s32)CELL_HTTP_ERROR_NET_RECV;
        }
        if (n == 0) {
            /* FIN/RST: per ORACLE(libhttp-Reference p.121) recvd goes to 0
             * regardless of Content-Length completeness -- EOF is not an
             * error here. */
            t->eof_reached = 1;
            if (!t->content_length_known) {
                t->content_length = t->body_received;
                t->content_length_known = 1;
            }
            break;
        }

        filled += (u32)n;
        t->body_received += (u64)n;
    }

    if (recvd)
        *recvd = ps3_bswap32(filled);   /* guest BE out-param */

    return CELL_OK;
}

s32 cellHttpTransactionCloseConnection(u32 transId)
{
    if (!s_http_initialized)
        return (s32)CELL_HTTP_ERROR_NOT_INITIALIZED;

    HttpTransSlot* t = http_trans_from_handle(transId);
    if (!t)
        return (s32)CELL_HTTP_ERROR_BAD_TRANS;

    http_close_slot_socket(t);
    printf("[cellHttp] TransactionCloseConnection(trans=%u)\n", transId);
    return CELL_OK;
}

s32 cellHttpTransactionAbortConnection(u32 transId)
{
    if (!s_http_initialized)
        return (s32)CELL_HTTP_ERROR_NOT_INITIALIZED;

    HttpTransSlot* t = http_trans_from_handle(transId);
    if (!t)
        return (s32)CELL_HTTP_ERROR_BAD_TRANS;

    t->aborted = 1;
    http_close_slot_socket(t);
    printf("[cellHttp] TransactionAbortConnection(trans=%u)\n", transId);
    return CELL_OK;
}

/* ---- request ------------------------------------------------------------ */

s32 cellHttpRequestSetContentLength(u32 transId, u64 totalSize)
{
    if (!s_http_initialized)
        return (s32)CELL_HTTP_ERROR_NOT_INITIALIZED;

    HttpTransSlot* t = http_trans_from_handle(transId);
    if (!t)
        return (s32)CELL_HTTP_ERROR_BAD_TRANS;

    t->request_content_length     = totalSize;
    t->request_content_length_set = 1;
    printf("[cellHttp] RequestSetContentLength(trans=%u, %llu)\n",
           transId, (unsigned long long)totalSize);
    return CELL_OK;
}

s32 cellHttpRequestGetContentLength(u32 transId, u64* totalSize)
{
    if (!s_http_initialized)
        return (s32)CELL_HTTP_ERROR_NOT_INITIALIZED;

    HttpTransSlot* t = http_trans_from_handle(transId);
    if (!t)
        return (s32)CELL_HTTP_ERROR_BAD_TRANS;
    if (!totalSize)
        return (s32)CELL_HTTP_ERROR_NO_BUFFER;
    if (!t->request_content_length_set)
        return (s32)CELL_HTTP_ERROR_NO_CONTENT_LENGTH;

    *totalSize = ps3_bswap64(t->request_content_length);  /* guest BE */
    return CELL_OK;
}

/* Shared body of Add/SetHeader.  header is a HOST pointer to the guest
 * CellHttpHeader (two BE EAs, ORACLE(cell/http/util.h:45-48)). */
static s32 http_request_put_header(u32 transId, const CellHttpHeader* header,
                                   int replace)
{
    if (!s_http_initialized)
        return (s32)CELL_HTTP_ERROR_NOT_INITIALIZED;

    HttpTransSlot* t = http_trans_from_handle(transId);
    if (!t)
        return (s32)CELL_HTTP_ERROR_BAD_TRANS;
    if (!header)
        return (s32)CELL_HTTP_ERROR_INVALID_HEADER;

    u32 name_ea  = ps3_bswap32(header->name);
    u32 value_ea = ps3_bswap32(header->value);
    const char* name  = name_ea  ? (const char*)vm_to_host(name_ea)  : NULL;
    const char* value = value_ea ? (const char*)vm_to_host(value_ea) : NULL;

    if (!name || !name[0])
        return (s32)CELL_HTTP_ERROR_INVALID_HEADER;
    if (!value)
        value = "";

    if (replace) {
        for (u32 i = 0; i < t->custom_header_count; i++) {
            if (http_strieq_n(t->custom_headers[i].name, name, strlen(name) + 1)) {
                strncpy(t->custom_headers[i].value, value,
                        sizeof(t->custom_headers[i].value) - 1);
                t->custom_headers[i].value[sizeof(t->custom_headers[i].value) - 1] = '\0';
                return CELL_OK;
            }
        }
    }

    if (t->custom_header_count >= CELL_HTTP_MAX_CUSTOM_HEADERS) {
        printf("[cellHttp] Request%sHeader - too many custom headers\n",
               replace ? "Set" : "Add");
        return (s32)CELL_HTTP_ERROR_NO_MEMORY;
    }

    HttpCustomHeader* h = &t->custom_headers[t->custom_header_count];
    strncpy(h->name, name, sizeof(h->name) - 1);
    h->name[sizeof(h->name) - 1] = '\0';
    strncpy(h->value, value, sizeof(h->value) - 1);
    h->value[sizeof(h->value) - 1] = '\0';
    t->custom_header_count++;

    printf("[cellHttp] Request%sHeader(trans=%u, '%s: %s')\n",
           replace ? "Set" : "Add", transId, h->name, h->value);
    return CELL_OK;
}

s32 cellHttpRequestAddHeader(u32 transId, const CellHttpHeader* header)
{
    return http_request_put_header(transId, header, 0);
}

s32 cellHttpRequestSetHeader(u32 transId, const CellHttpHeader* header)
{
    return http_request_put_header(transId, header, 1);
}

/* ---- response ----------------------------------------------------------- */

/* Doc contract for all Response* getters: "If the response headers are not
 * yet processed by the time this function is called, it will first read the
 * response headers" (ORACLE(libhttp-Reference p.121-125)). */
static s32 http_ensure_response_headers(HttpTransSlot* t)
{
    if (t->headers_parsed)
        return CELL_OK;
    if (!t->request_sent)
        return (s32)CELL_HTTP_ERROR_NO_REQUEST_SENT;
    return http_read_response_headers(t);
}

s32 cellHttpResponseGetStatusCode(u32 transId, s32* code)
{
    if (!s_http_initialized)
        return (s32)CELL_HTTP_ERROR_NOT_INITIALIZED;

    HttpTransSlot* t = http_trans_from_handle(transId);
    if (!t)
        return (s32)CELL_HTTP_ERROR_BAD_TRANS;
    if (!code)
        return (s32)CELL_HTTP_ERROR_NO_BUFFER;

    s32 rc = http_ensure_response_headers(t);
    if (rc != CELL_OK)
        return rc;

    *code = (s32)ps3_bswap32((u32)t->status_code);   /* guest BE */
    return CELL_OK;
}

s32 cellHttpResponseGetContentLength(u32 transId, u64* length)
{
    if (!s_http_initialized)
        return (s32)CELL_HTTP_ERROR_NOT_INITIALIZED;

    HttpTransSlot* t = http_trans_from_handle(transId);
    if (!t)
        return (s32)CELL_HTTP_ERROR_BAD_TRANS;
    if (!length)
        return (s32)CELL_HTTP_ERROR_NO_BUFFER;

    s32 rc = http_ensure_response_headers(t);
    if (rc != CELL_OK)
        return rc;

    if (!t->content_length_known) {
        /* ORACLE(libhttp-Reference p.124): "If no Content-Length field was
         * received ... it will return the CELL_HTTP_ERROR_NO_CONTENT_LENGTH
         * error." (chunked responses land here too) */
        return (s32)CELL_HTTP_ERROR_NO_CONTENT_LENGTH;
    }

    *length = ps3_bswap64(t->content_length);   /* guest BE */
    return CELL_OK;
}

s32 cellHttpResponseGetHeader(u32 transId, CellHttpHeader* header,
                              const char* name, void* pool, u32 poolSize,
                              u32* required)
{
    if (!s_http_initialized)
        return (s32)CELL_HTTP_ERROR_NOT_INITIALIZED;

    HttpTransSlot* t = http_trans_from_handle(transId);
    if (!t)
        return (s32)CELL_HTTP_ERROR_BAD_TRANS;
    if (!name || !name[0])
        return (s32)CELL_HTTP_ERROR_NO_STRING;

    s32 rc = http_ensure_response_headers(t);
    if (rc != CELL_OK)
        return rc;

    const char* val; u32 vlen;
    const char* wire_name; u32 wire_name_len;
    if (!http_find_response_header(t, name, &val, &vlen,
                                   &wire_name, &wire_name_len)) {
        /* ORACLE(cell/http/error.h:88-90): "that header doesn't exist" */
        return (s32)CELL_HTTP_ERROR_NO_HEADER;
    }

    u32 need = wire_name_len + 1 + vlen + 1;
    if (required)
        *required = ps3_bswap32(need);   /* guest BE; both passes */

    if (!pool)
        return CELL_OK;                  /* first pass of the two-pass idiom */

    if (poolSize < need)
        return (s32)CELL_HTTP_ERROR_INSUFFICIENT;
    if (!header)
        return (s32)CELL_HTTP_ERROR_NO_BUFFER;

    /* Copy the strings into the caller's pool and point the guest header
     * struct at them (BE guest EAs). */
    char* dst = (char*)pool;
    memcpy(dst, wire_name, wire_name_len);
    dst[wire_name_len] = '\0';
    memcpy(dst + wire_name_len + 1, val, vlen);
    dst[wire_name_len + 1 + vlen] = '\0';

    u32 pool_ea = vm_to_guest(pool);
    header->name  = ps3_bswap32(pool_ea);
    header->value = ps3_bswap32(pool_ea + wire_name_len + 1);
    return CELL_OK;
}
