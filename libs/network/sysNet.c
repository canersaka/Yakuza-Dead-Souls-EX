/*
 * ps3recomp - sys_net module implementation
 *
 * Implements the PS3 BSD socket API by wrapping host OS sockets.
 * On Windows uses Winsock2, on POSIX uses standard sockets.
 *
 * PS3 socket descriptors are mapped 1:1 to a local descriptor table
 * that holds the real host socket handle.
 */

#include "sysNet.h"
#include "../../include/ps3emu/endian.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <winsock2.h>
    #include <ws2tcpip.h>
    typedef SOCKET host_socket_t;
    #define HOST_INVALID_SOCKET INVALID_SOCKET
    #define HOST_SOCKET_ERROR   SOCKET_ERROR
    #define host_closesocket    closesocket
    #define host_errno          WSAGetLastError()
    /* MSG_DONTWAIT is not available on Windows; non-blocking mode is set via
       ioctlsocket(FIONBIO) instead.  Define to 0 so flag masking compiles. */
    #ifndef MSG_DONTWAIT
    #define MSG_DONTWAIT 0
    #endif
#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <sys/select.h>
    #include <netinet/in.h>
    #include <netdb.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <poll.h>
    typedef int host_socket_t;
    #define HOST_INVALID_SOCKET (-1)
    #define HOST_SOCKET_ERROR   (-1)
    #define host_closesocket    close
    #define host_errno          errno
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

typedef struct {
    host_socket_t host_fd;
    int           in_use;
    int           nonblocking;
} net_socket_slot;

static net_socket_slot s_sockets[SYS_NET_MAX_SOCKETS];
static int s_net_initialized = 0;
/* Per-thread errno (libnet-Reference p.117: "Error values are retained
 * separately per thread"; was a process-global). */
#ifdef _MSC_VER
static __declspec(thread) int s_net_errno = 0;
#else
static _Thread_local int s_net_errno = 0;
#endif

/* Static hostent for gethostbyname (PS3 uses a static buffer) */
static sys_net_hostent s_hostent;
static char s_hostent_name[256];
static uint32_t s_hostent_addr;
static uint32_t s_hostent_addr_list[2];  /* addr + NULL */

/* ---------------------------------------------------------------------------
 * Helpers
 * -----------------------------------------------------------------------*/

static int alloc_socket_slot(void)
{
    for (int i = 0; i < SYS_NET_MAX_SOCKETS; i++) {
        if (!s_sockets[i].in_use) {
            s_sockets[i].in_use = 1;
            s_sockets[i].nonblocking = 0;
            return i;
        }
    }
    return -1;
}

static void free_socket_slot(int idx)
{
    if (idx >= 0 && idx < SYS_NET_MAX_SOCKETS) {
        s_sockets[idx].host_fd = HOST_INVALID_SOCKET;
        s_sockets[idx].in_use = 0;
        s_sockets[idx].nonblocking = 0;
    }
}

static int valid_socket(int32_t s)
{
    return (s >= 0 && s < SYS_NET_MAX_SOCKETS && s_sockets[s].in_use);
}

static int translate_host_error(void)
{
    int err = host_errno;
#ifdef _WIN32
    /* Winsock WSAE* codes are WSABASEERR(10000) + the BSD errno numbering
     * that PS3 libnet publishes (ORACLE netex/errno.h: CELL 0x80010200+n
     * with n identical to BSD; e.g. WSAEHOSTUNREACH 10065 -> 65). The
     * named cases document the codes our calls actually produce; the
     * arithmetic fallback covers the rest of the shared numbering. */
    switch (err) {
        case WSAEINTR:         return SYS_NET_EINTR;
        case WSAEBADF:         return SYS_NET_EBADF;
        case WSAEINVAL:        return SYS_NET_EINVAL;
        case WSAEMFILE:        return SYS_NET_EMFILE;
        case WSAEWOULDBLOCK:   return SYS_NET_EWOULDBLOCK;
        case WSAEINPROGRESS:   return SYS_NET_EINPROGRESS;
        case WSAEALREADY:      return SYS_NET_EALREADY;
        case WSAENOPROTOOPT:   return SYS_NET_ENOPROTOOPT;
        case WSAEADDRINUSE:    return SYS_NET_EADDRINUSE;
        case WSAECONNABORTED:  return SYS_NET_ECONNABORTED;
        case WSAECONNRESET:    return SYS_NET_ECONNRESET;
        case WSAENOTCONN:      return SYS_NET_ENOTCONN;
        case WSAETIMEDOUT:     return SYS_NET_ETIMEDOUT;
        case WSAECONNREFUSED:  return SYS_NET_ECONNREFUSED;
        case WSAEHOSTUNREACH:  return SYS_NET_EHOSTUNREACH;
        default:
            if (err > 10000 && err <= 10092) return err - 10000;
            return SYS_NET_EINVAL;
    }
#else
    switch (err) {
        case EWOULDBLOCK:      return SYS_NET_EWOULDBLOCK;
        case EINTR:            return SYS_NET_EINTR;
        case EINPROGRESS:      return SYS_NET_EINPROGRESS;
        case EALREADY:         return SYS_NET_EALREADY;
        case ENOTCONN:         return SYS_NET_ENOTCONN;
        case ECONNREFUSED:     return SYS_NET_ECONNREFUSED;
        case ETIMEDOUT:        return SYS_NET_ETIMEDOUT;
        case ECONNRESET:       return SYS_NET_ECONNRESET;
        case ECONNABORTED:     return SYS_NET_ECONNABORTED;
        case EINVAL:           return SYS_NET_EINVAL;
        case EBADF:            return SYS_NET_EBADF;
        case ENOPROTOOPT:      return SYS_NET_ENOPROTOOPT;
        case EHOSTUNREACH:     return SYS_NET_EHOSTUNREACH;
        case EMFILE:           return SYS_NET_EMFILE;
        default:               return SYS_NET_EINVAL;
    }
#endif
}

/* MSG_DONTWAIT poll-then-call helper (Windows has no per-call nonblocking
 * flag and FIONBIO can't emulate it per-call): zero-timeout readiness check
 * on one socket. dir 0 = readable, 1 = writable. Returns 1 ready / 0 not. */
static int sock_ready(host_socket_t fd, int dir)
{
    fd_set set;
    struct timeval tv;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    int r = select((int)fd + 1, dir ? NULL : &set, dir ? &set : NULL,
                   NULL, &tv);
    return r == 1;
}

static void to_host_sockaddr(struct sockaddr_in* dst, const sys_net_sockaddr* src)
{
    const sys_net_sockaddr_in* ps3 = (const sys_net_sockaddr_in*)src;
    memset(dst, 0, sizeof(*dst));
    dst->sin_family = AF_INET;
    dst->sin_port = ps3->sin_port;        /* already network byte order */
    dst->sin_addr.s_addr = ps3->sin_addr; /* already network byte order */
}

static void from_host_sockaddr(sys_net_sockaddr* dst, const struct sockaddr_in* src)
{
    sys_net_sockaddr_in* ps3 = (sys_net_sockaddr_in*)dst;
    memset(ps3, 0, sizeof(*ps3));
    ps3->sin_len = sizeof(sys_net_sockaddr_in);
    ps3->sin_family = (uint8_t)SYS_NET_AF_INET;
    ps3->sin_port = src->sin_port;
    ps3->sin_addr = src->sin_addr.s_addr;
}

/* ---------------------------------------------------------------------------
 * Network init / shutdown
 * -----------------------------------------------------------------------*/

int32_t sys_net_initialize_network_ex(void* param)
{
    (void)param;

    if (s_net_initialized) {
        printf("[sys_net] Already initialized\n");
        return CELL_OK;
    }

#ifdef _WIN32
    WSADATA wsa;
    int err = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (err != 0) {
        printf("[sys_net] WSAStartup failed: %d\n", err);
        /* Non-BSD module API surface: CELL code, not -1+errno
         * (libnet-Reference p.28: nonzero CELL value = error). */
        return SYS_NET_ERROR_ENOMEM;
    }
#endif

    for (int i = 0; i < SYS_NET_MAX_SOCKETS; i++) {
        s_sockets[i].host_fd = HOST_INVALID_SOCKET;
        s_sockets[i].in_use = 0;
        s_sockets[i].nonblocking = 0;
    }

    s_net_initialized = 1;
    printf("[sys_net] Network initialized\n");
    return CELL_OK;
}

int32_t sys_net_finalize_network(void)
{
    if (!s_net_initialized)
        return CELL_OK;

    /* Close any remaining sockets */
    for (int i = 0; i < SYS_NET_MAX_SOCKETS; i++) {
        if (s_sockets[i].in_use && s_sockets[i].host_fd != HOST_INVALID_SOCKET) {
            host_closesocket(s_sockets[i].host_fd);
            s_sockets[i].host_fd = HOST_INVALID_SOCKET;
            s_sockets[i].in_use = 0;
        }
    }

#ifdef _WIN32
    WSACleanup();
#endif

    s_net_initialized = 0;
    printf("[sys_net] Network finalized\n");
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Socket creation / destruction
 * -----------------------------------------------------------------------*/

int32_t sys_net_bnet_socket(int32_t domain, int32_t type, int32_t protocol)
{
    if (!s_net_initialized) {
        /* BSD shape like every other guard here: -1 + errno (was a raw
         * CELL-ish value returned as a "descriptor"). */
        s_net_errno = SYS_NET_EINVAL;
        return -1;
    }

    int host_domain = (domain == SYS_NET_AF_INET) ? AF_INET : domain;
    int host_type = 0;
    switch (type) {
        case SYS_NET_SOCK_STREAM: host_type = SOCK_STREAM; break;
        case SYS_NET_SOCK_DGRAM:  host_type = SOCK_DGRAM;  break;
        default: host_type = type; break;
    }

    int slot = alloc_socket_slot();
    if (slot < 0) {
        s_net_errno = SYS_NET_ENOMEM;
        return -1;
    }

    host_socket_t fd = socket(host_domain, host_type, protocol);
    if (fd == HOST_INVALID_SOCKET) {
        s_net_errno = translate_host_error();
        free_socket_slot(slot);
        return -1;
    }

    s_sockets[slot].host_fd = fd;
    printf("[sys_net] socket(%d, %d, %d) -> fd %d\n", domain, type, protocol, slot);
    return slot;
}

int32_t sys_net_bnet_close(int32_t s)
{
    if (!valid_socket(s)) {
        s_net_errno = SYS_NET_EBADF;
        return -1;
    }

    host_closesocket(s_sockets[s].host_fd);
    free_socket_slot(s);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Connection management
 * -----------------------------------------------------------------------*/

int32_t sys_net_bnet_bind(int32_t s, const sys_net_sockaddr* addr, uint32_t addrlen)
{
    (void)addrlen;
    if (!valid_socket(s)) { s_net_errno = SYS_NET_EBADF; return -1; }

    struct sockaddr_in host_addr;
    to_host_sockaddr(&host_addr, addr);

    int ret = bind(s_sockets[s].host_fd, (struct sockaddr*)&host_addr, sizeof(host_addr));
    if (ret == HOST_SOCKET_ERROR) {
        s_net_errno = translate_host_error();
        return -1;
    }
    return 0;
}

int32_t sys_net_bnet_listen(int32_t s, int32_t backlog)
{
    if (!valid_socket(s)) { s_net_errno = SYS_NET_EBADF; return -1; }

    int ret = listen(s_sockets[s].host_fd, backlog);
    if (ret == HOST_SOCKET_ERROR) {
        s_net_errno = translate_host_error();
        return -1;
    }
    return 0;
}

int32_t sys_net_bnet_accept(int32_t s, sys_net_sockaddr* addr, uint32_t* addrlen)
{
    if (!valid_socket(s)) { s_net_errno = SYS_NET_EBADF; return -1; }

    struct sockaddr_in host_addr;
    int host_len = sizeof(host_addr);
#ifdef _WIN32
    host_socket_t fd = accept(s_sockets[s].host_fd, (struct sockaddr*)&host_addr, &host_len);
#else
    socklen_t sl = (socklen_t)host_len;
    host_socket_t fd = accept(s_sockets[s].host_fd, (struct sockaddr*)&host_addr, &sl);
    host_len = (int)sl;
#endif

    if (fd == HOST_INVALID_SOCKET) {
        s_net_errno = translate_host_error();
        return -1;
    }

    int slot = alloc_socket_slot();
    if (slot < 0) {
        host_closesocket(fd);
        s_net_errno = SYS_NET_ENOMEM;
        return -1;
    }

    s_sockets[slot].host_fd = fd;

    if (addr)
        from_host_sockaddr(addr, &host_addr);
    if (addrlen)
        *addrlen = sizeof(sys_net_sockaddr_in);

    return slot;
}

int32_t sys_net_bnet_connect(int32_t s, const sys_net_sockaddr* addr, uint32_t addrlen)
{
    (void)addrlen;
    if (!valid_socket(s)) { s_net_errno = SYS_NET_EBADF; return -1; }

    struct sockaddr_in host_addr;
    to_host_sockaddr(&host_addr, addr);

    int ret = connect(s_sockets[s].host_fd, (struct sockaddr*)&host_addr, sizeof(host_addr));
    if (ret == HOST_SOCKET_ERROR) {
        int err = host_errno;
#ifdef _WIN32
        if (err == WSAEWOULDBLOCK) {
#else
        if (err == EINPROGRESS) {
#endif
            s_net_errno = SYS_NET_EINPROGRESS;
            return -1;
        }
        s_net_errno = translate_host_error();
        return -1;
    }
    return 0;
}

int32_t sys_net_bnet_shutdown(int32_t s, int32_t how)
{
    if (!valid_socket(s)) { s_net_errno = SYS_NET_EBADF; return -1; }

    int ret = shutdown(s_sockets[s].host_fd, how);
    if (ret == HOST_SOCKET_ERROR) {
        s_net_errno = translate_host_error();
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Data transfer
 * -----------------------------------------------------------------------*/

int32_t sys_net_bnet_send(int32_t s, const void* buf, uint32_t len, int32_t flags)
{
    if (!valid_socket(s)) { s_net_errno = SYS_NET_EBADF; return -1; }

    int host_flags = 0;
    if (flags & SYS_NET_MSG_DONTWAIT) host_flags |= MSG_DONTWAIT;
#ifndef _WIN32
    if (flags & SYS_NET_MSG_PEEK)     host_flags |= MSG_PEEK;
#endif
#ifdef _WIN32
    /* 2026-08-05 item D4: Windows has no per-call MSG_DONTWAIT (the macro
     * is 0 above) -- poll-then-call so a blocking socket returns EAGAIN
     * instead of blocking (libnet-Reference p.58 error contract). */
    if ((flags & SYS_NET_MSG_DONTWAIT) && !s_sockets[s].nonblocking &&
        !sock_ready(s_sockets[s].host_fd, 1)) {
        s_net_errno = SYS_NET_EWOULDBLOCK;
        return -1;
    }
#endif

    int ret = send(s_sockets[s].host_fd, (const char*)buf, (int)len, host_flags);
    if (ret == HOST_SOCKET_ERROR) {
        s_net_errno = translate_host_error();
        return -1;
    }
    return ret;
}

int32_t sys_net_bnet_sendto(int32_t s, const void* buf, uint32_t len, int32_t flags,
                            const sys_net_sockaddr* to, uint32_t tolen)
{
    (void)tolen;
    if (!valid_socket(s)) { s_net_errno = SYS_NET_EBADF; return -1; }

    struct sockaddr_in host_to;
    to_host_sockaddr(&host_to, to);

    int host_flags = 0;
    if (flags & SYS_NET_MSG_DONTWAIT) host_flags |= MSG_DONTWAIT;
#ifdef _WIN32
    if ((flags & SYS_NET_MSG_DONTWAIT) && !s_sockets[s].nonblocking &&
        !sock_ready(s_sockets[s].host_fd, 1)) {
        s_net_errno = SYS_NET_EWOULDBLOCK;
        return -1;
    }
#endif

    int ret = sendto(s_sockets[s].host_fd, (const char*)buf, (int)len, host_flags,
                     (struct sockaddr*)&host_to, sizeof(host_to));
    if (ret == HOST_SOCKET_ERROR) {
        s_net_errno = translate_host_error();
        return -1;
    }
    return ret;
}

int32_t sys_net_bnet_recv(int32_t s, void* buf, uint32_t len, int32_t flags)
{
    if (!valid_socket(s)) { s_net_errno = SYS_NET_EBADF; return -1; }

    int host_flags = 0;
    if (flags & SYS_NET_MSG_PEEK)     host_flags |= MSG_PEEK;
    if (flags & SYS_NET_MSG_DONTWAIT) host_flags |= MSG_DONTWAIT;
    if (flags & SYS_NET_MSG_WAITALL)  host_flags |= MSG_WAITALL;
#ifdef _WIN32
    if ((flags & SYS_NET_MSG_DONTWAIT) && !s_sockets[s].nonblocking &&
        !sock_ready(s_sockets[s].host_fd, 0)) {
        s_net_errno = SYS_NET_EWOULDBLOCK;
        return -1;
    }
#endif

    int ret = recv(s_sockets[s].host_fd, (char*)buf, (int)len, host_flags);
    if (ret == HOST_SOCKET_ERROR) {
        s_net_errno = translate_host_error();
        return -1;
    }
    return ret;
}

int32_t sys_net_bnet_recvfrom(int32_t s, void* buf, uint32_t len, int32_t flags,
                              sys_net_sockaddr* from, uint32_t* fromlen)
{
    if (!valid_socket(s)) { s_net_errno = SYS_NET_EBADF; return -1; }

    struct sockaddr_in host_from;
    int host_fromlen = sizeof(host_from);
    int host_flags = 0;
    if (flags & SYS_NET_MSG_PEEK)     host_flags |= MSG_PEEK;
    if (flags & SYS_NET_MSG_DONTWAIT) host_flags |= MSG_DONTWAIT;

#ifdef _WIN32
    if ((flags & SYS_NET_MSG_DONTWAIT) && !s_sockets[s].nonblocking &&
        !sock_ready(s_sockets[s].host_fd, 0)) {
        s_net_errno = SYS_NET_EWOULDBLOCK;
        return -1;
    }
    int ret = recvfrom(s_sockets[s].host_fd, (char*)buf, (int)len, host_flags,
                       (struct sockaddr*)&host_from, &host_fromlen);
#else
    socklen_t sl = (socklen_t)host_fromlen;
    int ret = recvfrom(s_sockets[s].host_fd, buf, (size_t)len, host_flags,
                       (struct sockaddr*)&host_from, &sl);
#endif

    if (ret == HOST_SOCKET_ERROR) {
        s_net_errno = translate_host_error();
        return -1;
    }

    if (from)
        from_host_sockaddr(from, &host_from);
    if (fromlen)
        *fromlen = sizeof(sys_net_sockaddr_in);

    return ret;
}

/* ---------------------------------------------------------------------------
 * Socket options
 * -----------------------------------------------------------------------*/

int32_t sys_net_bnet_setsockopt(int32_t s, int32_t level, int32_t optname,
                                const void* optval, uint32_t optlen)
{
    if (!valid_socket(s)) { s_net_errno = SYS_NET_EBADF; return -1; }

    /* Handle PS3-specific SO_NBIO */
    if (level == SYS_NET_SOL_SOCKET && optname == SYS_NET_SO_NBIO) {
        int val = optval ? *(const int32_t*)optval : 0;
        s_sockets[s].nonblocking = val;
#ifdef _WIN32
        u_long mode = val ? 1 : 0;
        ioctlsocket(s_sockets[s].host_fd, FIONBIO, &mode);
#else
        int flags = fcntl(s_sockets[s].host_fd, F_GETFL, 0);
        if (val)
            fcntl(s_sockets[s].host_fd, F_SETFL, flags | O_NONBLOCK);
        else
            fcntl(s_sockets[s].host_fd, F_SETFL, flags & ~O_NONBLOCK);
#endif
        return 0;
    }

    /* 2026-08-05 doc-conformance item D3: translate the caller's real
     * level (the old code hardcoded SOL_SOCKET, turning TCP_NODELAY(1)
     * into Winsock SO_DEBUG(1)); marshal guest big-endian option values
     * explicitly; convert the SND/RCVTIMEO 16-byte BE timeval to the
     * host form; unknown (level,optname) -> ENOPROTOOPT. PS3
     * IPPROTO_TCP/IP numbers equal the host's, so those pass through. */
    if (!optval || optlen < 4) { s_net_errno = SYS_NET_EINVAL; return -1; }

    if (level == SYS_NET_SOL_SOCKET &&
        (optname == SYS_NET_SO_SNDTIMEO || optname == SYS_NET_SO_RCVTIMEO)) {
        /* Guest: struct timeval { s64 sec; s64 usec; } big-endian. */
        if (optlen < 16) { s_net_errno = SYS_NET_EINVAL; return -1; }
        const uint64_t sec  = ps3_bswap64(((const uint64_t*)optval)[0]);
        const uint64_t usec = ps3_bswap64(((const uint64_t*)optval)[1]);
        const int host_opt = (optname == SYS_NET_SO_SNDTIMEO)
                                 ? SO_SNDTIMEO : SO_RCVTIMEO;
#ifdef _WIN32
        /* Win32 optval is a DWORD of milliseconds. Round a nonzero
         * sub-ms request up so it never becomes 0 (= infinite). */
        uint64_t ms64 = sec * 1000ull + (usec + 999ull) / 1000ull;
        DWORD ms = (ms64 > 0xFFFFFFFEull) ? 0xFFFFFFFEu : (DWORD)ms64;
        int ret = setsockopt(s_sockets[s].host_fd, SOL_SOCKET, host_opt,
                             (const char*)&ms, sizeof(ms));
#else
        struct timeval tv;
        tv.tv_sec = (time_t)sec;
        tv.tv_usec = (suseconds_t)usec;
        int ret = setsockopt(s_sockets[s].host_fd, SOL_SOCKET, host_opt,
                             (const char*)&tv, sizeof(tv));
#endif
        if (ret == HOST_SOCKET_ERROR) {
            s_net_errno = translate_host_error();
            return -1;
        }
        return 0;
    }

    int host_level, host_optname;
    if (level == SYS_NET_SOL_SOCKET) {
        host_level = SOL_SOCKET;
        switch (optname) {
            case SYS_NET_SO_REUSEADDR: host_optname = SO_REUSEADDR; break;
            case SYS_NET_SO_KEEPALIVE: host_optname = SO_KEEPALIVE; break;
            case SYS_NET_SO_BROADCAST: host_optname = SO_BROADCAST; break;
            case SYS_NET_SO_LINGER:    host_optname = SO_LINGER;    break;
            case SYS_NET_SO_SNDBUF:    host_optname = SO_SNDBUF;    break;
            case SYS_NET_SO_RCVBUF:    host_optname = SO_RCVBUF;    break;
            default:
                s_net_errno = SYS_NET_ENOPROTOOPT;
                return -1;
        }
    } else if (level == SYS_NET_IPPROTO_TCP || level == SYS_NET_IPPROTO_IP) {
        host_level = level;      /* numeric values match the host's */
        host_optname = optname;  /* TCP_NODELAY=1, IP_* match 1:1 */
    } else {
        s_net_errno = SYS_NET_ENOPROTOOPT;
        return -1;
    }

    if (host_level == SOL_SOCKET && host_optname == SO_LINGER) {
        /* Guest: { s32 l_onoff; s32 l_linger; } big-endian. */
        if (optlen < 8) { s_net_errno = SYS_NET_EINVAL; return -1; }
        struct linger lg;
        lg.l_onoff  = (u_short)ps3_bswap32(((const uint32_t*)optval)[0]);
        lg.l_linger = (u_short)ps3_bswap32(((const uint32_t*)optval)[1]);
        int ret = setsockopt(s_sockets[s].host_fd, SOL_SOCKET, SO_LINGER,
                             (const char*)&lg, sizeof(lg));
        if (ret == HOST_SOCKET_ERROR) {
            s_net_errno = translate_host_error();
            return -1;
        }
        return 0;
    }

    /* Remaining options carry one 32-bit int (guest big-endian). */
    int hv = (int)ps3_bswap32(*(const uint32_t*)optval);
    int ret = setsockopt(s_sockets[s].host_fd, host_level, host_optname,
                         (const char*)&hv, sizeof(hv));
    if (ret == HOST_SOCKET_ERROR) {
        s_net_errno = translate_host_error();
        return -1;
    }
    return 0;
}

int32_t sys_net_bnet_getsockopt(int32_t s, int32_t level, int32_t optname,
                                void* optval, uint32_t* optlen)
{
    if (!valid_socket(s)) { s_net_errno = SYS_NET_EBADF; return -1; }

    /* PS3-specific SO_NBIO (guest out-param is big-endian) */
    if (level == SYS_NET_SOL_SOCKET && optname == SYS_NET_SO_NBIO) {
        if (optval) *(uint32_t*)optval =
            ps3_bswap32((uint32_t)s_sockets[s].nonblocking);
        if (optlen) *optlen = 4;
        return 0;
    }
    if (!optval || !optlen) { s_net_errno = SYS_NET_EINVAL; return -1; }

    /* 2026-08-05 item D3 (mirror of setsockopt): honor the real level,
     * marshal BE out-values, convert the TIMEO DWORD back to a guest
     * 16-byte BE timeval, ENOPROTOOPT for unknown combos. */
    if (level == SYS_NET_SOL_SOCKET &&
        (optname == SYS_NET_SO_SNDTIMEO || optname == SYS_NET_SO_RCVTIMEO)) {
        if (*optlen < 16) { s_net_errno = SYS_NET_EINVAL; return -1; }
        const int host_opt = (optname == SYS_NET_SO_SNDTIMEO)
                                 ? SO_SNDTIMEO : SO_RCVTIMEO;
        uint64_t sec, usec;
#ifdef _WIN32
        DWORD ms = 0;
        int ol = sizeof(ms);
        int ret = getsockopt(s_sockets[s].host_fd, SOL_SOCKET, host_opt,
                             (char*)&ms, &ol);
        if (ret == HOST_SOCKET_ERROR) {
            s_net_errno = translate_host_error();
            return -1;
        }
        sec = ms / 1000u;
        usec = (uint64_t)(ms % 1000u) * 1000u;
#else
        struct timeval tv;
        socklen_t sl = sizeof(tv);
        int ret = getsockopt(s_sockets[s].host_fd, SOL_SOCKET, host_opt,
                             &tv, &sl);
        if (ret == HOST_SOCKET_ERROR) {
            s_net_errno = translate_host_error();
            return -1;
        }
        sec = (uint64_t)tv.tv_sec;
        usec = (uint64_t)tv.tv_usec;
#endif
        ((uint64_t*)optval)[0] = ps3_bswap64(sec);
        ((uint64_t*)optval)[1] = ps3_bswap64(usec);
        *optlen = 16;
        return 0;
    }

    int host_level, host_optname;
    if (level == SYS_NET_SOL_SOCKET) {
        host_level = SOL_SOCKET;
        switch (optname) {
            case SYS_NET_SO_REUSEADDR: host_optname = SO_REUSEADDR; break;
            case SYS_NET_SO_KEEPALIVE: host_optname = SO_KEEPALIVE; break;
            case SYS_NET_SO_BROADCAST: host_optname = SO_BROADCAST; break;
            case SYS_NET_SO_SNDBUF:    host_optname = SO_SNDBUF;    break;
            case SYS_NET_SO_RCVBUF:    host_optname = SO_RCVBUF;    break;
            case SYS_NET_SO_ERROR:     host_optname = SO_ERROR;     break;
            default:
                s_net_errno = SYS_NET_ENOPROTOOPT;
                return -1;
        }
    } else if (level == SYS_NET_IPPROTO_TCP || level == SYS_NET_IPPROTO_IP) {
        host_level = level;
        host_optname = optname;
    } else {
        s_net_errno = SYS_NET_ENOPROTOOPT;
        return -1;
    }

    int hv = 0;
#ifdef _WIN32
    int host_optlen = sizeof(hv);
    int ret = getsockopt(s_sockets[s].host_fd, host_level, host_optname,
                         (char*)&hv, &host_optlen);
#else
    socklen_t sl = sizeof(hv);
    int ret = getsockopt(s_sockets[s].host_fd, host_level, host_optname,
                         &hv, &sl);
#endif
    if (ret == HOST_SOCKET_ERROR) {
        s_net_errno = translate_host_error();
        return -1;
    }
    if (*optlen < 4) { s_net_errno = SYS_NET_EINVAL; return -1; }
    /* SO_ERROR's host value is a Winsock/host errno: publish the guest
     * numbering, not the raw host constant. */
    if (host_level == SOL_SOCKET && host_optname == SO_ERROR && hv != 0) {
#ifdef _WIN32
        hv = (hv > 10000 && hv <= 10092) ? hv - 10000 : SYS_NET_EINVAL;
#endif
    }
    *(uint32_t*)optval = ps3_bswap32((uint32_t)hv);
    *optlen = 4;
    return 0;
}

int32_t sys_net_bnet_getsockname(int32_t s, sys_net_sockaddr* addr, uint32_t* addrlen)
{
    if (!valid_socket(s)) { s_net_errno = SYS_NET_EBADF; return -1; }

    struct sockaddr_in host_addr;
#ifdef _WIN32
    int host_len = sizeof(host_addr);
    int ret = getsockname(s_sockets[s].host_fd, (struct sockaddr*)&host_addr, &host_len);
#else
    socklen_t sl = sizeof(host_addr);
    int ret = getsockname(s_sockets[s].host_fd, (struct sockaddr*)&host_addr, &sl);
#endif

    if (ret == HOST_SOCKET_ERROR) {
        s_net_errno = translate_host_error();
        return -1;
    }

    if (addr)
        from_host_sockaddr(addr, &host_addr);
    if (addrlen)
        *addrlen = sizeof(sys_net_sockaddr_in);

    return 0;
}

/* ---------------------------------------------------------------------------
 * I/O multiplexing
 * -----------------------------------------------------------------------*/

int32_t sys_net_bnet_poll(sys_net_pollfd* fds, uint32_t nfds, int32_t timeout_ms)
{
    if (!fds || nfds == 0) return 0;

#ifdef _WIN32
    /* Use WSAPoll on Windows */
    WSAPOLLFD* host_fds = (WSAPOLLFD*)calloc(nfds, sizeof(WSAPOLLFD));
    if (!host_fds) return -1;

    for (uint32_t i = 0; i < nfds; i++) {
        if (valid_socket(fds[i].fd)) {
            host_fds[i].fd = s_sockets[fds[i].fd].host_fd;
        } else {
            host_fds[i].fd = INVALID_SOCKET;
        }
        host_fds[i].events = 0;
        if (fds[i].events & SYS_NET_POLLIN)  host_fds[i].events |= POLLIN;
        if (fds[i].events & SYS_NET_POLLOUT) host_fds[i].events |= POLLOUT;
    }

    int ret = WSAPoll(host_fds, nfds, timeout_ms);

    for (uint32_t i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        if (host_fds[i].revents & POLLIN)  fds[i].revents |= SYS_NET_POLLIN;
        if (host_fds[i].revents & POLLOUT) fds[i].revents |= SYS_NET_POLLOUT;
        if (host_fds[i].revents & POLLERR) fds[i].revents |= SYS_NET_POLLERR;
        if (host_fds[i].revents & POLLHUP) fds[i].revents |= SYS_NET_POLLHUP;
        if (host_fds[i].revents & POLLNVAL) fds[i].revents |= SYS_NET_POLLNVAL;
    }

    free(host_fds);
#else
    struct pollfd* host_fds = (struct pollfd*)calloc(nfds, sizeof(struct pollfd));
    if (!host_fds) return -1;

    for (uint32_t i = 0; i < nfds; i++) {
        if (valid_socket(fds[i].fd)) {
            host_fds[i].fd = s_sockets[fds[i].fd].host_fd;
        } else {
            host_fds[i].fd = -1;
        }
        host_fds[i].events = 0;
        if (fds[i].events & SYS_NET_POLLIN)  host_fds[i].events |= POLLIN;
        if (fds[i].events & SYS_NET_POLLOUT) host_fds[i].events |= POLLOUT;
    }

    int ret = poll(host_fds, nfds, timeout_ms);

    for (uint32_t i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        if (host_fds[i].revents & POLLIN)  fds[i].revents |= SYS_NET_POLLIN;
        if (host_fds[i].revents & POLLOUT) fds[i].revents |= SYS_NET_POLLOUT;
        if (host_fds[i].revents & POLLERR) fds[i].revents |= SYS_NET_POLLERR;
        if (host_fds[i].revents & POLLHUP) fds[i].revents |= SYS_NET_POLLHUP;
        if (host_fds[i].revents & POLLNVAL) fds[i].revents |= SYS_NET_POLLNVAL;
    }

    free(host_fds);
#endif

    if (ret == HOST_SOCKET_ERROR) {
        s_net_errno = translate_host_error();
        return -1;
    }
    return ret;
}

/* Guest fd_set: 32 big-endian u32 words = 1024 fd bits, bit (1<<(fd&31))
 * within word fd/32 (ORACLE sys/select.h). Guest timeval: { s64 sec;
 * s64 usec; } big-endian, 16 bytes (ORACLE sys/time.h). */
#define SYS_NET_FDSET_WORDS 32

static int guest_fd_isset(const uint32_t* set, int fd)
{
    return (ps3_bswap32(set[fd >> 5]) >> (fd & 31)) & 1u;
}
static void guest_fd_set_bit(uint32_t* set, int fd)
{
    set[fd >> 5] |= ps3_bswap32(1u << (fd & 31));
}

int32_t sys_net_bnet_select(int32_t nfds, void* readfds, void* writefds,
                            void* exceptfds, void* timeout)
{
    /* 2026-08-05 doc-conformance item D2: was an unconditional
     * return-0-"timeout" no-op -- readiness loops spun forever
     * (libnet-Reference p.80-81: real select semantics; NULL timeout
     * blocks, NULL set = direction unchecked, return = ready count). */
    if (nfds <= 0 || nfds > SYS_NET_MAX_SOCKETS) {
        s_net_errno = SYS_NET_EINVAL;
        return -1;
    }

    fd_set hr, hw, he;
    FD_ZERO(&hr); FD_ZERO(&hw); FD_ZERO(&he);
    host_socket_t maxfd = 0;
    int any = 0;
    for (int i = 0; i < nfds; i++) {
        if (!s_sockets[i].in_use) continue;
        const host_socket_t hfd = s_sockets[i].host_fd;
        int used = 0;
        if (readfds && guest_fd_isset((const uint32_t*)readfds, i)) {
            FD_SET(hfd, &hr); used = 1;
        }
        if (writefds && guest_fd_isset((const uint32_t*)writefds, i)) {
            FD_SET(hfd, &hw); used = 1;
        }
        if (exceptfds && guest_fd_isset((const uint32_t*)exceptfds, i)) {
            FD_SET(hfd, &he); used = 1;
        }
        if (used) { any = 1; if (hfd > maxfd) maxfd = hfd; }
    }

    struct timeval htv;
    struct timeval* ptv = NULL;
    if (timeout) {
        const uint64_t sec  = ps3_bswap64(((const uint64_t*)timeout)[0]);
        const uint64_t usec = ps3_bswap64(((const uint64_t*)timeout)[1]);
        htv.tv_sec  = (long)((sec > 0x7FFFFFFFull) ? 0x7FFFFFFFul : sec);
        htv.tv_usec = (long)usec;
        ptv = &htv;
    }

    int ready;
    if (!any) {
        /* No live sockets selected: sleep out the timeout (a host select
         * with empty sets errors on Winsock). NULL timeout would block
         * forever -- treat as EINVAL rather than hang. */
        if (!ptv) { s_net_errno = SYS_NET_EINVAL; return -1; }
#ifdef _WIN32
        Sleep((DWORD)(htv.tv_sec * 1000 + htv.tv_usec / 1000));
#else
        select(0, NULL, NULL, NULL, ptv);
#endif
        ready = 0;
    } else {
        ready = select((int)maxfd + 1, readfds ? &hr : NULL,
                       writefds ? &hw : NULL, exceptfds ? &he : NULL, ptv);
    }
    if (ready == HOST_SOCKET_ERROR) {
        s_net_errno = translate_host_error();
        return -1;
    }

    /* Write back BE result sets over the guest words. */
    uint32_t out_r[SYS_NET_FDSET_WORDS] = {0};
    uint32_t out_w[SYS_NET_FDSET_WORDS] = {0};
    uint32_t out_e[SYS_NET_FDSET_WORDS] = {0};
    for (int i = 0; i < nfds; i++) {
        if (!s_sockets[i].in_use) continue;
        const host_socket_t hfd = s_sockets[i].host_fd;
        if (readfds && guest_fd_isset((const uint32_t*)readfds, i) &&
            FD_ISSET(hfd, &hr))
            guest_fd_set_bit(out_r, i);
        if (writefds && guest_fd_isset((const uint32_t*)writefds, i) &&
            FD_ISSET(hfd, &hw))
            guest_fd_set_bit(out_w, i);
        if (exceptfds && guest_fd_isset((const uint32_t*)exceptfds, i) &&
            FD_ISSET(hfd, &he))
            guest_fd_set_bit(out_e, i);
    }
    if (readfds)   memcpy(readfds,   out_r, sizeof(out_r));
    if (writefds)  memcpy(writefds,  out_w, sizeof(out_w));
    if (exceptfds) memcpy(exceptfds, out_e, sizeof(out_e));
    return ready;
}

/* ---------------------------------------------------------------------------
 * DNS
 * -----------------------------------------------------------------------*/

int32_t sys_net_bnet_inet_aton(const char* cp, uint32_t* inp)
{
    if (!cp || !inp) return 0;

    struct in_addr addr;
#ifdef _WIN32
    addr.s_addr = inet_addr(cp);
    if (addr.s_addr == INADDR_NONE && strcmp(cp, "255.255.255.255") != 0)
        return 0;
#else
    if (inet_aton(cp, &addr) == 0)
        return 0;
#endif

    *inp = addr.s_addr;
    return 1;
}

uint32_t sys_net_bnet_gethostbyname(const char* name)
{
    if (!name) return 0;

    struct hostent* he = gethostbyname(name);
    if (!he) {
        printf("[sys_net] gethostbyname('%s') failed\n", name);
        return 0;
    }

    /* Copy into our static buffer */
    strncpy(s_hostent_name, he->h_name, sizeof(s_hostent_name) - 1);
    s_hostent_name[sizeof(s_hostent_name) - 1] = '\0';

    if (he->h_addr_list && he->h_addr_list[0]) {
        memcpy(&s_hostent_addr, he->h_addr_list[0], 4);
    }

    /*
     * Note: In a real implementation, these would be guest VM pointers.
     * For now we return a host pointer cast. The recompiled code will
     * need adapter functions to handle this properly.
     */
    s_hostent_addr_list[0] = s_hostent_addr;
    s_hostent_addr_list[1] = 0;

    s_hostent.h_addrtype = he->h_addrtype;
    s_hostent.h_length = he->h_length;

    printf("[sys_net] gethostbyname('%s') -> %u.%u.%u.%u\n", name,
           (s_hostent_addr >> 0) & 0xFF, (s_hostent_addr >> 8) & 0xFF,
           (s_hostent_addr >> 16) & 0xFF, (s_hostent_addr >> 24) & 0xFF);

    /* Return non-zero to indicate success (PS3 returns pointer) */
    return 1;
}

/* ---------------------------------------------------------------------------
 * Thread-local errno
 * -----------------------------------------------------------------------*/

/* Real export name carries the leading underscore (libnet-Reference
 * p.117); renamed 2026-08-05 so the NID scan binds it when imported. */
int32_t* _sys_net_errno_loc(void)
{
    return &s_net_errno;
}
