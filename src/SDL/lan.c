/*=============================================================================
    lan.c
    Transport for LAN and direct-address multiplayer. See lan.h for why this
    exists rather than the SDL_net implementation it replaces.

    Shape of the thing:

    - One UDP socket bound to LAN_UDP_PORT carries game advertisements. The
      lobby broadcasts one periodically and every listening machine hands it
      to titanReceivedLanBroadcastCB, which is how a game appears in someone
      else's list. Broadcast does not leave the subnet, so this is the LAN
      half only; internet play skips it and calls lanConnect() with a typed
      address.

    - One TCP listener on LAN_TCP_PORT accepts peers, and one outbound
      connection is made per peer we join. Both end up in the same peer table
      and are indistinguishable afterwards. The game addresses peers by IP,
      never by "the connection I opened", so the direction a link was
      established in must not matter.

    - Everything is non-blocking. A peer that stops reading fills its own
      outbound buffer and is dropped; it cannot stall the frame. This is the
      main reason not to reuse the old code, which called blocking send() on
      the game thread and had a discovery routine that spun forever.
=============================================================================*/

#include "lan.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <iphlpapi.h>
    typedef int socklen_t;
    #define LAN_INVALID       INVALID_SOCKET
    #define lanCloseSocket    closesocket
    #define lanWouldBlock()   (WSAGetLastError() == WSAEWOULDBLOCK)
    #define lanInProgress()   (WSAGetLastError() == WSAEWOULDBLOCK)
#else
    #include <arpa/inet.h>
    #include <fcntl.h>
    #include <ifaddrs.h>
    #include <net/if.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <poll.h>
    #include <sys/socket.h>
    #include <unistd.h>
    typedef int SOCKET;
    #define LAN_INVALID       (-1)
    #define lanCloseSocket    close
    #define lanWouldBlock()   (errno == EAGAIN || errno == EWOULDBLOCK)
    #define lanInProgress()   (errno == EINPROGRESS)
#endif

#include "Debug.h"
#include "TitanInterfaceC.h"

/*=============================================================================
    Private types and data
=============================================================================*/

/* Eight players means at most seven peers, but a machine can have a connection
   in flight from someone whose join we have not accepted yet, so leave room
   rather than refuse a legitimate peer for want of a slot. */
#define LAN_MAX_PEERS       16

/* A frame header is three bytes: type, then length little endian. Written and
   read explicitly rather than by struct copy, because a struct would carry
   this platform's padding into the protocol. */
#define LAN_HEADER_SIZE     3

/* Enough for the largest thing the game sends with headroom to spare, and
   small enough that sixteen of them is not worth worrying about. */
#define LAN_RECV_CAPACITY   (64 * 1024)
#define LAN_SEND_CAPACITY   (64 * 1024)

typedef struct
{
    SOCKET  sock;
    udword  ip;                         /* network byte order, as Address holds it */
    bool32  connecting;                 /* outbound connect() has not completed */

    ubyte*  recvBuffer;
    udword  recvUsed;

    ubyte*  sendBuffer;
    udword  sendUsed;
} LanPeer;

static bool32   lanRunning = FALSE;
static SOCKET   lanListenSock = LAN_INVALID;
static SOCKET   lanUdpSock = LAN_INVALID;
static udword   lanMyAddress = 0;
static LanPeer  lanPeers[LAN_MAX_PEERS];
static sdword   lanPeerCount = 0;

/* Addresses that discovery is unicast to as well as broadcast, so peers
   beyond this subnet are reachable. Seeded from configuration and grown from
   whoever sends us discovery.

   With the port, which is not always LAN_UDP_PORT. A peer on mobile data sits
   behind carrier-grade NAT, where thousands of subscribers share one address
   and the source port therefore cannot be preserved: its packets arrive from
   some arbitrary port, and that port is the only way back through the
   mapping. Replying to LAN_UDP_PORT instead reaches nothing, which is exactly
   what happened - the first packet crossed the internet and was answered into
   a void. An address typed by a player keeps LAN_UDP_PORT, since that is what
   the host will have forwarded, and is corrected the moment they answer. */
#define LAN_MAX_REMOTES 8
typedef struct
{
    udword ip;
    uword  port;                        /* network byte order, as sockaddr holds it */
} LanRemote;
static LanRemote lanRemotes[LAN_MAX_REMOTES];
static sdword   lanRemoteCount = 0;

/* Our own address and netmask, for deciding whether an advertised address is
   one we could actually dial. */
static udword   lanMyNetmask = 0;

/* What a peer calls itself, against where it can actually be reached.
   Everybody publishes the address their own machine has, so behind a router
   the lobby names peers by addresses that mean nothing here - and guessing
   which one is which only works when there is exactly one candidate, which is
   what limited internet play to two players.

   No guessing is needed, because every discovery packet carries both halves:
   the advertisement says who the sender is, and recvfrom says where it came
   from. The lobby knows that packet's shape and hands the pair down through
   lanAddAlias, once a second, for every peer that can reach us. */
#define LAN_MAX_ALIASES 16
typedef struct
{
    udword published;                   /* what the peer calls itself */
    udword reachable;                   /* where its packets really come from */
} LanAlias;
static LanAlias lanAliases[LAN_MAX_ALIASES];
static sdword   lanAliasCount = 0;

/*=============================================================================
    Socket helpers
=============================================================================*/

static void lanSetNonBlocking(SOCKET sock)
{
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(sock, FIONBIO, &on);
#else
    sdword flags = fcntl(sock, F_GETFL, 0);

    if (flags >= 0)
    {
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }
#endif
}

/* Lockstep sends small packets constantly and waits on every one of them, so
   Nagle's algorithm is exactly wrong here: it would hold a command back for up
   to 200ms looking for company, and every player waits out that delay. */
static void lanSetNoDelay(SOCKET sock)
{
    sdword on = 1;

    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char*)&on, sizeof(on));
}

/*=============================================================================
    Peer table
=============================================================================*/

static LanPeer* lanFindPeer(udword ip)
{
    sdword i;

    for (i = 0; i < lanPeerCount; i++)
    {
        if (lanPeers[i].ip == ip)
        {
            return &lanPeers[i];
        }
    }
    return NULL;
}

/* The address a peer calls itself is not always the address we reach it on.
   Everyone publishes the address getifaddrs gave them, so a player behind a
   router publishes a private one: the game's lobby structures, and therefore
   every call into this file, name the other end by an address that means
   nothing out here. The link itself is keyed by the address the socket really
   uses - the one we dialled, or the one accept() reported.

   With exactly one remote named, that remote is necessarily the other end of
   the internet link, so an address we cannot otherwise reach resolves to it.
   That is the same rule lanConnect already used to dial; without it here, the
   connection came up and then every message on it was dropped for want of a
   peer, which is what made internet play fail after appearing to work.

   Ambiguous with several remotes named, so it is not attempted then: over the
   internet this transport is a two-player one until the protocol carries the
   sender's identity. Deliberately not guessed - see the note in lanConnect.

   It cannot help when both ends sit on the same private range (two 192.168.1.x
   networks, which is the common case for home routers): the peer's address
   then looks local, is left alone, and reaches somebody else's machine or
   nothing at all. Fixing that needs the peers to exchange who they are rather
   than inferring it from addresses. */
static udword lanRouteTo(udword ip)
{
    sdword i;

    if (lanFindPeer(ip) != NULL)
    {
        return ip;                      /* reachable under the name given */
    }
    /* Told, rather than guessed. Works with any number of peers, which the
       fallback below cannot: with two remotes named there is no way to know
       which of them an unreachable address belongs to. */
    for (i = 0; i < lanAliasCount; i++)
    {
        if (lanAliases[i].published == ip)
        {
            return lanAliases[i].reachable;
        }
    }
    if (!lanAddressIsLocal(ip) && lanRemoteCount == 1)
    {
        /* Nothing has been heard from this peer yet, but with exactly one
           remote named there is only one thing it could be. Keeps a two
           player game working during the gap before the first advertisement
           arrives. */
        return lanRemotes[0].ip;
    }
    return ip;
}

static void lanDropPeerAt(sdword index, char const* why)
{
    LanPeer* peer = &lanPeers[index];
    struct in_addr shown;

    shown.s_addr = peer->ip;
    dbgMessagef("lan: dropping peer %s (%s)", inet_ntoa(shown), why);

    if (peer->sock != LAN_INVALID)
    {
        lanCloseSocket(peer->sock);
    }
    free(peer->recvBuffer);
    free(peer->sendBuffer);

    lanPeerCount--;
    if (index < lanPeerCount)
    {
        lanPeers[index] = lanPeers[lanPeerCount];
    }
    memset(&lanPeers[lanPeerCount], 0, sizeof(LanPeer));
}

static LanPeer* lanAddPeer(SOCKET sock, udword ip, bool32 connecting)
{
    LanPeer* peer;

    if (lanPeerCount >= LAN_MAX_PEERS)
    {
        dbgMessagef("lan: refusing peer, table full");
        lanCloseSocket(sock);
        return NULL;
    }

    peer = &lanPeers[lanPeerCount];
    memset(peer, 0, sizeof(LanPeer));
    peer->recvBuffer = (ubyte*)malloc(LAN_RECV_CAPACITY);
    peer->sendBuffer = (ubyte*)malloc(LAN_SEND_CAPACITY);
    if (peer->recvBuffer == NULL || peer->sendBuffer == NULL)
    {
        free(peer->recvBuffer);
        free(peer->sendBuffer);
        lanCloseSocket(sock);
        return NULL;
    }

    peer->sock = sock;
    peer->ip = ip;
    peer->connecting = connecting;
    lanPeerCount++;

    lanSetNonBlocking(sock);
    lanSetNoDelay(sock);
    return peer;
}

bool32 lanPeerConnected(udword ip)
{
    LanPeer* peer = lanFindPeer(lanRouteTo(ip));

    return peer != NULL && !peer->connecting;
}

/*=============================================================================
    Remote peers
=============================================================================*/

/* port in network byte order. Learned from a packet that reached us, so it is
   the way back through whatever translated it; 0 means "the standard one",
   which is what an address a player typed gets. */
static void lanAddRemoteAt(udword ip, uword port)
{
    struct in_addr shown;
    sdword i;

    if (ip == 0 || ip == lanMyAddress)
    {
        return;
    }
    if (port == 0)
    {
        port = htons(LAN_UDP_PORT);
    }
    shown.s_addr = ip;
    for (i = 0; i < lanRemoteCount; i++)
    {
        if (lanRemotes[i].ip == ip)
        {
            /* A mapping that moved - the peer's NAT rebound, or we had only
               guessed the port from configuration and have now heard from
               them. Follow it, or every reply after this goes nowhere. */
            if (lanRemotes[i].port != port)
            {
                dbgMessagef("lan: remote peer %s moved from port %u to %u",
                            inet_ntoa(shown), (unsigned)ntohs(lanRemotes[i].port),
                            (unsigned)ntohs(port));
                lanRemotes[i].port = port;
            }
            return;
        }
    }
    if (lanRemoteCount >= LAN_MAX_REMOTES)
    {
        return;
    }
    lanRemotes[lanRemoteCount].ip = ip;
    lanRemotes[lanRemoteCount].port = port;
    lanRemoteCount++;
    dbgMessagef("lan: remote peer %s port %u added, discovery will be sent to it",
                inet_ntoa(shown), (unsigned)ntohs(port));
}

void lanAddRemote(udword ip)
{
    lanAddRemoteAt(ip, 0);
}

void lanAddAlias(udword published, udword reachable)
{
    struct in_addr shownPub, shownReach;
    sdword i;

    if (published == 0 || reachable == 0 || published == reachable
        || published == lanMyAddress)
    {
        return;                         /* nothing to learn */
    }
    for (i = 0; i < lanAliasCount; i++)
    {
        if (lanAliases[i].published == published)
        {
            /* A peer that moved: a NAT rebound, a reconnection from a new
               port, or the same player on a different network. Follow it,
               since the old one is no longer where they are. */
            if (lanAliases[i].reachable != reachable)
            {
                shownPub.s_addr = published;
                dbgMessagef("lan: %s has moved", inet_ntoa(shownPub));
                lanAliases[i].reachable = reachable;
            }
            return;
        }
    }
    if (lanAliasCount >= LAN_MAX_ALIASES)
    {
        return;
    }
    lanAliases[lanAliasCount].published = published;
    lanAliases[lanAliasCount].reachable = reachable;
    lanAliasCount++;

    shownPub.s_addr = published;
    shownReach.s_addr = reachable;
    /* inet_ntoa returns a static buffer, so the first has to be copied out
       before the second call overwrites it. */
    {
        char pub[24];

        strncpy(pub, inet_ntoa(shownPub), sizeof(pub) - 1);
        pub[sizeof(pub) - 1] = '\0';
        dbgMessagef("lan: %s is reachable at %s", pub, inet_ntoa(shownReach));
    }
}

bool32 lanHaveRemotes(void)
{
    return lanRemoteCount > 0;
}

bool32 lanAddressIsLocal(udword ip)
{
    sdword i;

    if (lanMyNetmask != 0
        && (ip & lanMyNetmask) == (lanMyAddress & lanMyNetmask))
    {
        return TRUE;
    }
    for (i = 0; i < lanRemoteCount; i++)
    {
        if (lanRemotes[i].ip == ip)
        {
            return TRUE;
        }
    }
    return FALSE;
}

/*=============================================================================
    Local address

    Picked from the interface list: the first address that is up, is not
    loopback, and is IPv4. Wanted because Address.AddrPart.IP is how every
    other player will refer to us, so it has to be an address they can reach,
    not 127.0.0.1.
=============================================================================*/

udword lanLocalAddress(void)
{
    return lanMyAddress;
}

#ifndef _WIN32
static udword lanDiscoverLocalAddress(void)
{
    struct ifaddrs* list = NULL;
    struct ifaddrs* entry;
    udword found = 0;

    if (getifaddrs(&list) != 0)
    {
        dbgMessagef("lan: getifaddrs failed (%s)", strerror(errno));
        return 0;
    }

    for (entry = list; entry != NULL; entry = entry->ifa_next)
    {
        struct sockaddr_in* in;

        if (entry->ifa_addr == NULL || entry->ifa_addr->sa_family != AF_INET)
        {
            continue;
        }
        if (!(entry->ifa_flags & IFF_UP) || (entry->ifa_flags & IFF_LOOPBACK))
        {
            continue;
        }
        in = (struct sockaddr_in*)entry->ifa_addr;
        found = in->sin_addr.s_addr;
        if (entry->ifa_netmask != NULL)
        {
            lanMyNetmask = ((struct sockaddr_in*)entry->ifa_netmask)->sin_addr.s_addr;
        }
        dbgMessagef("lan: local address %s on %s", inet_ntoa(in->sin_addr),
                    entry->ifa_name ? entry->ifa_name : "?");
        break;
    }

    freeifaddrs(list);
    return found;
}
#else
static udword lanDiscoverLocalAddress(void)
{
    char name[256];
    struct addrinfo hints;
    struct addrinfo* result = NULL;
    udword found = 0;

    if (gethostname(name, sizeof(name)) != 0)
    {
        return 0;
    }
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    if (getaddrinfo(name, NULL, &hints, &result) != 0)
    {
        return 0;
    }
    if (result != NULL)
    {
        found = ((struct sockaddr_in*)result->ai_addr)->sin_addr.s_addr;
    }
    freeaddrinfo(result);
    return found;
}
#endif

/*=============================================================================
    Startup and shutdown
=============================================================================*/

static SOCKET lanOpenListener(void)
{
    SOCKET sock;
    struct sockaddr_in addr;
    sdword reuse = 1;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == LAN_INVALID)
    {
        dbgMessagef("lan: TCP socket failed");
        return LAN_INVALID;
    }
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(LAN_TCP_PORT);

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0)
    {
        dbgMessagef("lan: TCP bind to %d failed (%s)", LAN_TCP_PORT,
                    strerror(errno));
        lanCloseSocket(sock);
        return LAN_INVALID;
    }
    if (listen(sock, LAN_MAX_PEERS) != 0)
    {
        dbgMessagef("lan: listen failed (%s)", strerror(errno));
        lanCloseSocket(sock);
        return LAN_INVALID;
    }

    lanSetNonBlocking(sock);
    return sock;
}

static SOCKET lanOpenDiscovery(void)
{
    SOCKET sock;
    struct sockaddr_in addr;
    sdword on = 1;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == LAN_INVALID)
    {
        dbgMessagef("lan: UDP socket failed");
        return LAN_INVALID;
    }
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&on, sizeof(on));
    /* Sending to 255.255.255.255 needs this, and receiving needs the bind
       below to be to INADDR_ANY rather than to our own address. */
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (char*)&on, sizeof(on));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(LAN_UDP_PORT);

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0)
    {
        dbgMessagef("lan: UDP bind to %d failed (%s)", LAN_UDP_PORT,
                    strerror(errno));
        lanCloseSocket(sock);
        return LAN_INVALID;
    }

    lanSetNonBlocking(sock);
    return sock;
}

bool32 lanStart(void)
{
    if (lanRunning)
    {
        return TRUE;
    }

#ifdef _WIN32
    {
        WSADATA wsa;

        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        {
            dbgMessagef("lan: WSAStartup failed");
            return FALSE;
        }
    }
#endif

    memset(lanPeers, 0, sizeof(lanPeers));
    lanPeerCount = 0;

    lanMyAddress = lanDiscoverLocalAddress();
    if (lanMyAddress == 0)
    {
        dbgMessagef("lan: no usable local address, refusing to start");
        return FALSE;
    }

    lanUdpSock = lanOpenDiscovery();
    if (lanUdpSock == LAN_INVALID)
    {
        return FALSE;
    }

    lanListenSock = lanOpenListener();
    if (lanListenSock == LAN_INVALID)
    {
        lanCloseSocket(lanUdpSock);
        lanUdpSock = LAN_INVALID;
        return FALSE;
    }

    lanRunning = TRUE;
    dbgMessagef("lan: started, TCP %d UDP %d", LAN_TCP_PORT, LAN_UDP_PORT);
    return TRUE;
}

void lanShutdown(void)
{
    if (!lanRunning)
    {
        return;
    }
    while (lanPeerCount > 0)
    {
        lanDropPeerAt(lanPeerCount - 1, "shutdown");
    }
    if (lanListenSock != LAN_INVALID)
    {
        lanCloseSocket(lanListenSock);
        lanListenSock = LAN_INVALID;
    }
    if (lanUdpSock != LAN_INVALID)
    {
        lanCloseSocket(lanUdpSock);
        lanUdpSock = LAN_INVALID;
    }
    lanRunning = FALSE;
    lanMyAddress = 0;
    lanMyNetmask = 0;
    lanRemoteCount = 0;
#ifdef _WIN32
    WSACleanup();
#endif
    dbgMessagef("lan: shut down");
}

/*=============================================================================
    Discovery
=============================================================================*/

void lanSendDiscovery(const void* data, uword length)
{
    struct sockaddr_in addr;
    sdword i;

    if (!lanRunning || length == 0)
    {
        return;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_BROADCAST;
    addr.sin_port = htons(LAN_UDP_PORT);

    if (sendto(lanUdpSock, (const char*)data, length, 0,
               (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        if (!lanWouldBlock())
        {
            dbgMessagef("lan: discovery send failed (%s)", strerror(errno));
        }
    }

    /* The same advertisement, addressed rather than broadcast, so it crosses
       a router. */
    for (i = 0; i < lanRemoteCount; i++)
    {
        addr.sin_addr.s_addr = lanRemotes[i].ip;
        addr.sin_port = lanRemotes[i].port;
        sendto(lanUdpSock, (const char*)data, length, 0,
               (struct sockaddr*)&addr, sizeof(addr));
    }
}

static void lanServiceDiscovery(void)
{
    ubyte packet[LAN_MAX_MESSAGE];
    struct sockaddr_in from;
    socklen_t fromLen;
    sdword got;

    for (;;)
    {
        fromLen = sizeof(from);
        got = recvfrom(lanUdpSock, (char*)packet, sizeof(packet), 0,
                       (struct sockaddr*)&from, &fromLen);
        if (got <= 0)
        {
            return;
        }
        /* Anyone off-subnet who reaches us has to be answered directly,
           since our broadcast will never get back to them. This is what lets
           a host that configured nothing still talk to a client that named
           it. */
        if (from.sin_addr.s_addr != lanMyAddress
            && !lanAddressIsLocal(from.sin_addr.s_addr))
        {
            lanAddRemoteAt(from.sin_addr.s_addr, from.sin_port);
        }
        /* Already known, but the port it reaches us on can move under us:
           behind carrier-grade NAT it is whatever the carrier assigned, and
           it is re-assigned freely. Track it on every packet, not only the
           first, or the link goes quiet the moment the mapping changes. */
        else if (from.sin_addr.s_addr != lanMyAddress)
        {
            sdword r;

            for (r = 0; r < lanRemoteCount; r++)
            {
                if (lanRemotes[r].ip == from.sin_addr.s_addr)
                {
                    lanAddRemoteAt(from.sin_addr.s_addr, from.sin_port);
                    break;
                }
            }
        }

        /* Our own broadcast comes back to us. The lobby is written expecting
           to see its own advertisement (titanSendLanBroadcast used to call
           the callback directly), so this is passed up rather than filtered:
           dropping it would remove the host's own game from its list. */
        /* The source address goes up with it: the advertisement says who the
           sender calls itself and this says where it actually came from, and
           only the lobby knows how to read the first out of the packet. That
           pair is what lanAddAlias needs. */
        titanReceivedLanBroadcastCB(from.sin_addr.s_addr, packet,
                                    (unsigned short)got);
    }
}

/*=============================================================================
    Sending
=============================================================================*/

static void lanFlush(sdword index)
{
    LanPeer* peer = &lanPeers[index];
    sdword sent;

    while (peer->sendUsed > 0)
    {
        sent = send(peer->sock, (const char*)peer->sendBuffer, peer->sendUsed, 0);
        if (sent > 0)
        {
            peer->sendUsed -= (udword)sent;
            if (peer->sendUsed > 0)
            {
                memmove(peer->sendBuffer, peer->sendBuffer + sent, peer->sendUsed);
            }
            continue;
        }
        if (sent < 0 && lanWouldBlock())
        {
            return;                     /* socket full, try again next tick */
        }
        lanDropPeerAt(index, "send failed");
        return;
    }
}

static void lanQueue(LanPeer* peer, ubyte messageType, const void* data,
                     uword length)
{
    udword needed = LAN_HEADER_SIZE + (udword)length;

    if (peer->sendUsed + needed > LAN_SEND_CAPACITY)
    {
        /* Not a transient condition: it means this peer has not drained a
           full buffer's worth. Marking the socket dead lets the game's own
           player-dropped handling take over, which is better than silently
           losing a command and desyncing everyone. */
        dbgMessagef("lan: peer send buffer overflow, marking dead");
        if (peer->sock != LAN_INVALID)
        {
            lanCloseSocket(peer->sock);
            peer->sock = LAN_INVALID;
        }
        return;
    }

    peer->sendBuffer[peer->sendUsed++] = messageType;
    peer->sendBuffer[peer->sendUsed++] = (ubyte)(length & 0xff);
    peer->sendBuffer[peer->sendUsed++] = (ubyte)((length >> 8) & 0xff);
    if (length > 0)
    {
        memcpy(peer->sendBuffer + peer->sendUsed, data, length);
        peer->sendUsed += length;
    }
}

void lanSendTo(udword ip, ubyte messageType, const void* data, uword length)
{
    LanPeer* peer;

    if (!lanRunning)
    {
        return;
    }
    peer = lanFindPeer(lanRouteTo(ip));
    if (peer == NULL)
    {
        struct in_addr shown;

        shown.s_addr = ip;
        dbgMessagef("lan: no link to %s, message type %u dropped",
                    inet_ntoa(shown), (unsigned)messageType);
        return;
    }
    lanQueue(peer, messageType, data, length);
}

void lanSendToAll(ubyte messageType, const void* data, uword length)
{
    sdword i;

    if (!lanRunning)
    {
        return;
    }
    for (i = 0; i < lanPeerCount; i++)
    {
        lanQueue(&lanPeers[i], messageType, data, length);
    }
}

/*=============================================================================
    Connecting and accepting
=============================================================================*/

bool32 lanConnect(udword ip)
{
    SOCKET sock;
    struct sockaddr_in addr;
    struct in_addr shown;

    if (!lanRunning)
    {
        return FALSE;
    }
    /* A game hosted behind a router advertises the host's address on its own
       LAN, which is meaningless from out here: dialling 192.168.x.y would
       reach nothing, or worse, somebody else's machine on our subnet. See
       lanRouteTo, which every path that names a peer goes through. */
    {
        udword routed = lanRouteTo(ip);

        if (routed != ip)
        {
            struct in_addr shown;
            char was[24];

            /* Two inet_ntoa calls in one printf both read the same static
               buffer, so the line came out naming one address twice - which
               reads as a no-op substitution and sent a real diagnosis down
               the wrong path for a while. */
            shown.s_addr = ip;
            strncpy(was, inet_ntoa(shown), sizeof(was) - 1);
            was[sizeof(was) - 1] = '\0';
            shown.s_addr = routed;
            dbgMessagef("lan: %s is not reachable from here, dialling the named "
                        "remote %s instead", was, inet_ntoa(shown));
            ip = routed;
        }
    }
    if (lanFindPeer(ip) != NULL)
    {
        return TRUE;                    /* already linked or linking */
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == LAN_INVALID)
    {
        return FALSE;
    }
    lanSetNonBlocking(sock);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ip;
    addr.sin_port = htons(LAN_TCP_PORT);

    shown.s_addr = ip;
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0
        && !lanInProgress())
    {
        dbgMessagef("lan: connect to %s failed (%s)", inet_ntoa(shown),
                    strerror(errno));
        lanCloseSocket(sock);
        return FALSE;
    }

    dbgMessagef("lan: connecting to %s", inet_ntoa(shown));
    return lanAddPeer(sock, ip, TRUE) != NULL;
}

static void lanServiceAccept(void)
{
    for (;;)
    {
        struct sockaddr_in from;
        socklen_t fromLen = sizeof(from);
        SOCKET sock = accept(lanListenSock, (struct sockaddr*)&from, &fromLen);

        if (sock == LAN_INVALID)
        {
            return;
        }
        /* A peer we already have an outbound connection to can also be
           connecting to us; both sides joining at once is normal on a mesh.
           Keeping the existing entry and closing the new socket makes the
           result independent of who dialled first. */
        if (lanFindPeer(from.sin_addr.s_addr) != NULL)
        {
            lanCloseSocket(sock);
            continue;
        }
        dbgMessagef("lan: accepted %s", inet_ntoa(from.sin_addr));
        lanAddPeer(sock, from.sin_addr.s_addr, FALSE);
    }
}

/*=============================================================================
    Receiving
=============================================================================*/

/* Pulls whole frames out of the peer's buffer and dispatches them. Returns
   FALSE if the peer went away, in which case the caller must not touch it
   again: dispatching can re-enter the transport and reshuffle the table. */
static bool32 lanDispatch(sdword index)
{
    for (;;)
    {
        LanPeer* peer = &lanPeers[index];
        udword length;
        ubyte type;
        udword frameSize;
        ubyte payload[LAN_MAX_MESSAGE];
        udword peerIp;

        if (peer->recvUsed < LAN_HEADER_SIZE)
        {
            return TRUE;
        }
        type = peer->recvBuffer[0];
        length = (udword)peer->recvBuffer[1] | ((udword)peer->recvBuffer[2] << 8);
        frameSize = LAN_HEADER_SIZE + length;

        if (peer->recvUsed < frameSize)
        {
            return TRUE;                /* rest of it has not arrived yet */
        }

        /* Copied out before dispatching. The callback runs game code that can
           call back into here and move this peer's buffer, or free it. */
        if (length > 0)
        {
            memcpy(payload, peer->recvBuffer + LAN_HEADER_SIZE, length);
        }
        peerIp = peer->ip;
        peer->recvUsed -= frameSize;
        if (peer->recvUsed > 0)
        {
            memmove(peer->recvBuffer, peer->recvBuffer + frameSize,
                    peer->recvUsed);
        }

        HandleTCPMessage(peerIp, type, payload, (uword)length);

        /* The dispatch may have dropped this peer, in which case the slot now
           holds somebody else entirely. */
        if (index >= lanPeerCount || lanPeers[index].ip != peerIp)
        {
            return FALSE;
        }
    }
}

static void lanServicePeer(sdword index)
{
    LanPeer* peer = &lanPeers[index];
    sdword got;

    for (;;)
    {
        udword space = LAN_RECV_CAPACITY - peer->recvUsed;

        if (space == 0)
        {
            /* A single frame cannot exceed the buffer, so a full buffer with
               no complete frame in it means the peer is not speaking this
               protocol. */
            lanDropPeerAt(index, "receive buffer full");
            return;
        }
        got = recv(peer->sock, (char*)peer->recvBuffer + peer->recvUsed, space, 0);
        if (got > 0)
        {
            peer->recvUsed += (udword)got;
            if (!lanDispatch(index))
            {
                return;
            }
            peer = &lanPeers[index];
            continue;
        }
        if (got == 0)
        {
            lanDropPeerAt(index, "peer closed");
            return;
        }
        if (lanWouldBlock())
        {
            return;
        }
        lanDropPeerAt(index, "receive failed");
        return;
    }
}

/*=============================================================================
    The service tick
=============================================================================*/

void lanService(void)
{
    sdword i;

    if (!lanRunning)
    {
        return;
    }

    lanServiceAccept();
    lanServiceDiscovery();

    /* Backwards, so dropping a peer (which moves the last entry into the hole)
       cannot skip the entry that was moved. */
    for (i = lanPeerCount - 1; i >= 0; i--)
    {
        LanPeer* peer = &lanPeers[i];

        if (peer->sock == LAN_INVALID)
        {
            lanDropPeerAt(i, "marked dead");
            continue;
        }
        if (peer->connecting)
        {
            struct pollfd pfd;

            pfd.fd = peer->sock;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            if (poll(&pfd, 1, 0) > 0)
            {
                sdword err = 0;
                socklen_t errLen = sizeof(err);

                getsockopt(peer->sock, SOL_SOCKET, SO_ERROR, (char*)&err, &errLen);
                if (err != 0)
                {
                    lanDropPeerAt(i, "connect refused");
                    continue;
                }
                peer->connecting = FALSE;
                lanSetNoDelay(peer->sock);
                dbgMessagef("lan: connection established");
            }
            continue;                   /* nothing to read or send until then */
        }

        lanServicePeer(i);
        if (i < lanPeerCount && lanPeers[i].sock != LAN_INVALID)
        {
            lanFlush(i);
        }
    }
}
