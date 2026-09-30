/*
 * ps3recomp - cellNetCtl HLE implementation
 *
 * Provides network control state and info queries. The runtime has no live
 * PSN session, so connection state defaults to disconnected.
 */

#include "cellNetCtl.h"
#include "ps3emu/endian.h"   /* CellNetCtlInfo numeric fields are guest big-endian */
#include "cellSysutil.h"     /* cellSysutilQueueEvent, CELL_SYSUTIL_MAX_CALLBACKS */
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int s_netctl_initialized = 0;

/* Handlers are registered but never invoked, so only the slot usage is
 * tracked. */
typedef struct {
    int in_use;
} NetCtlHandlerSlot;

static NetCtlHandlerSlot s_handlers[CELL_NET_CTL_HANDLER_MAX];

/* Try to get the host machine's IP address */
static int netctl_get_host_ip(char* buf, size_t buflen)
{
#ifdef _WIN32
    WSADATA wsa;
    char hostname[256];
    struct addrinfo hints, *res;
    int ret;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        goto fallback;

    if (gethostname(hostname, sizeof(hostname)) != 0) {
        WSACleanup();
        goto fallback;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    ret = getaddrinfo(hostname, NULL, &hints, &res);
    if (ret != 0 || !res) {
        WSACleanup();
        goto fallback;
    }

    {
        struct sockaddr_in* addr = (struct sockaddr_in*)res->ai_addr;
        u8* ip = (u8*)&addr->sin_addr;
        snprintf(buf, buflen, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    }
    freeaddrinfo(res);
    /* Don't WSACleanup here -- we may need it later */
    return 1;
#else
    char hostname[256];
    struct addrinfo hints, *res;
    int ret;

    if (gethostname(hostname, sizeof(hostname)) != 0)
        goto fallback;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    ret = getaddrinfo(hostname, NULL, &hints, &res);
    if (ret != 0 || !res)
        goto fallback;

    {
        struct sockaddr_in* addr = (struct sockaddr_in*)res->ai_addr;
        u8* ip = (u8*)&addr->sin_addr;
        snprintf(buf, buflen, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    }
    freeaddrinfo(res);
    return 1;
#endif

fallback:
    snprintf(buf, buflen, "192.168.1.100");
    return 0;
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellNetCtlInit(void)
{
    if (s_netctl_initialized)
        return CELL_NET_CTL_ERROR_NOT_TERMINATED;

    memset(s_handlers, 0, sizeof(s_handlers));
    s_netctl_initialized = 1;
    return CELL_OK;
}

void cellNetCtlTerm(void)
{
    memset(s_handlers, 0, sizeof(s_handlers));
    s_netctl_initialized = 0;
}

s32 cellNetCtlGetState(s32* state)
{
    if (!s_netctl_initialized)
        return CELL_NET_CTL_ERROR_NOT_INITIALIZED;

    if (!state)
        return CELL_NET_CTL_ERROR_INVALID_ADDR;

    /* The state is always Disconnected because no PSN backend exists.
     * Reporting a connected state makes the title open a "sign in to
     * PlayStation Network" dialog and wait for a sign-in that can never
     * complete. GetInfo still returns plausible wired-link values. */
    *state = (s32)ps3_bswap32((u32)CELL_NET_CTL_STATE_Disconnected);
    return CELL_OK;
}

s32 cellNetCtlGetInfo(s32 code, CellNetCtlInfo* info)
{
    if (!s_netctl_initialized)
        return CELL_NET_CTL_ERROR_NOT_INITIALIZED;

    if (!info)
        return CELL_NET_CTL_ERROR_INVALID_ADDR;

    memset(info, 0, sizeof(CellNetCtlInfo));

    switch (code)
    {
    case CELL_NET_CTL_INFO_DEVICE:
        info->device = ps3_bswap32((u32)CELL_NET_CTL_DEVICE_WIRED);
        break;

    case CELL_NET_CTL_INFO_ETHER_ADDR:
        /* Fake PS3-like MAC: 00:04:1F:xx:xx:xx (Sony OUI) */
        info->ether_addr.data[0] = 0x00;
        info->ether_addr.data[1] = 0x04;
        info->ether_addr.data[2] = 0x1F;
        info->ether_addr.data[3] = 0xAB;
        info->ether_addr.data[4] = 0xCD;
        info->ether_addr.data[5] = 0xEF;
        break;

    case CELL_NET_CTL_INFO_MTU:
        info->mtu = ps3_bswap32(1500u);
        break;

    case CELL_NET_CTL_INFO_LINK:
        info->link = ps3_bswap32((u32)CELL_NET_CTL_LINK_CONNECTED);
        break;

    case CELL_NET_CTL_INFO_LINK_TYPE:
        info->link_type = ps3_bswap32((u32)CELL_NET_CTL_LINK_TYPE_1000BASE_FULL);
        break;

    case CELL_NET_CTL_INFO_IP_ADDRESS:
        netctl_get_host_ip(info->ip_address, sizeof(info->ip_address));
        break;

    case CELL_NET_CTL_INFO_NETMASK:
        strncpy(info->netmask, "255.255.255.0", sizeof(info->netmask) - 1);
        break;

    case CELL_NET_CTL_INFO_DEFAULT_ROUTE:
        strncpy(info->default_route, "192.168.1.1",
                sizeof(info->default_route) - 1);
        break;

    case CELL_NET_CTL_INFO_PRIMARY_DNS:
        strncpy(info->primary_dns, "8.8.8.8", sizeof(info->primary_dns) - 1);
        break;

    case CELL_NET_CTL_INFO_SECONDARY_DNS:
        strncpy(info->secondary_dns, "8.8.4.4",
                sizeof(info->secondary_dns) - 1);
        break;

    case CELL_NET_CTL_INFO_IP_CONFIG:
        info->ip_config = 0; /* DHCP */
        break;

    case CELL_NET_CTL_INFO_HTTP_PROXY_CONFIG:
        info->http_proxy_config = 0; /* disabled */
        break;

    case CELL_NET_CTL_INFO_UPNP_CONFIG:
        info->upnp_config = ps3_bswap32(1u); /* enabled */
        break;

    default:
        return CELL_NET_CTL_ERROR_INVALID_CODE;
    }

    return CELL_OK;
}

s32 cellNetCtlGetNatInfo(CellNetCtlNatInfo* natInfo)
{
    if (!s_netctl_initialized)
        return CELL_NET_CTL_ERROR_NOT_INITIALIZED;

    if (!natInfo)
        return CELL_NET_CTL_ERROR_INVALID_ADDR;

    natInfo->size        = sizeof(CellNetCtlNatInfo);
    natInfo->nat_type    = CELL_NET_CTL_NATINFO_NAT_TYPE_2; /* moderate */
    natInfo->stun_status = 0;
    natInfo->upnp_status = 0;

    return CELL_OK;
}

s32 cellNetCtlAddHandler(cellNetCtlHandler handler, void* arg, s32* hid)
{
    if (!s_netctl_initialized)
        return CELL_NET_CTL_ERROR_NOT_INITIALIZED;

    if (!handler || !hid)
        return CELL_NET_CTL_ERROR_INVALID_ADDR;

    (void)arg;   /* handlers are never invoked */

    for (s32 i = 0; i < CELL_NET_CTL_HANDLER_MAX; i++) {
        if (!s_handlers[i].in_use) {
            s_handlers[i].in_use = 1;
            /* hid aliases guest BE memory: store big-endian like every other
             * numeric out-param in this module (a raw host-endian store would
             * hand the guest a byte-swapped id). Ids 0-2 are endian-visible
             * on PPC. */
            *hid = (s32)ps3_bswap32((u32)i);
            return CELL_OK;
        }
    }

    return CELL_NET_CTL_ERROR_HANDLER_MAX;
}

s32 cellNetCtlDelHandler(s32 hid)
{
    if (!s_netctl_initialized)
        return CELL_NET_CTL_ERROR_NOT_INITIALIZED;

    if (hid < 0 || hid >= CELL_NET_CTL_HANDLER_MAX)
        return CELL_NET_CTL_ERROR_INVALID_ID;

    if (!s_handlers[hid].in_use)
        return CELL_NET_CTL_ERROR_ID_NOT_FOUND;

    s_handlers[hid].in_use = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Net-start ("connect to PSN") dialog
 *
 * Real hardware shows a system dialog and reports progress asynchronously
 * through the cellSysutil callback queue (LOADED -> FINISHED), after which
 * the game calls UnloadAsync to read the result. We have no
 * PSN session to show a dialog for. The explicit connect-dialog path remains
 * an immediate success even though ordinary state queries default to
 * Disconnected: LoadAsync posts LOADED + FINISHED to every registered
 * sysutil callback slot (the game may have registered on any of them), and
 * UnloadAsync reports a successful connect.
 * -----------------------------------------------------------------------*/

static void netctl_netstart_broadcast(u32 status)
{
    for (int slot = 0; slot < CELL_SYSUTIL_MAX_CALLBACKS; slot++)
        cellSysutilQueueEvent(slot, status, 0);
}

s32 cellNetCtlNetStartDialogLoadAsync(const CellNetCtlNetStartDialogParam* param)
{
    if (!s_netctl_initialized)
        return CELL_NET_CTL_ERROR_NOT_INITIALIZED;

    if (!param)
        return CELL_NET_CTL_ERROR_INVALID_ADDR;

    netctl_netstart_broadcast(CELL_SYSUTIL_NET_CTL_NETSTART_LOADED);
    netctl_netstart_broadcast(CELL_SYSUTIL_NET_CTL_NETSTART_FINISHED);
    return CELL_OK;
}

s32 cellNetCtlNetStartDialogUnloadAsync(CellNetCtlNetStartDialogResult* result)
{
    if (!s_netctl_initialized)
        return CELL_NET_CTL_ERROR_NOT_INITIALIZED;

    if (result)
        result->result = (s32)ps3_bswap32(0u); /* 0 = connected (guest big-endian) */

    netctl_netstart_broadcast(CELL_SYSUTIL_NET_CTL_NETSTART_UNLOADED);
    return CELL_OK;
}
