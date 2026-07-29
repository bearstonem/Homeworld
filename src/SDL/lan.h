/*=============================================================================
    lan.h
    Transport for LAN and direct-address multiplayer.

    Replaces NetworkInterface.c, which was built on SDL_net, exit()ed on every
    error path, blocked its caller forever discovering the local address, and
    assumed a star topology the game does not actually use.

    Everything here is non-blocking and serviced from lanService(), which
    titanPumpEngine() calls on the main thread. There are deliberately no
    threads: the old implementation's races were with a game whose allocator
    and front-end queues are not thread safe, and the task scheduler already
    hands us a tick often enough that we do not need one.
=============================================================================*/

#ifndef ___LAN_H
#define ___LAN_H

#include "Types.h"

/* Chosen to match what NetworkInterface.c used, so anything on a network
   still speaking the old protocol lands on the same ports. */
#define LAN_TCP_PORT 10500
#define LAN_UDP_PORT 10600

/* A frame is [type:u8][length:u16 little endian][payload]. The length field
   caps a single message at 64K, which is far above the largest thing the game
   sends: CaptainGameInfo, the whole lobby state, is 896 bytes. */
#define LAN_MAX_MESSAGE 65535

/*-----------------------------------------------------------------------------
    Lifecycle
----------------------------------------------------------------------------*/
/* Opens the UDP discovery socket and the TCP listener and works out which
   local address peers should reach us on. Returns FALSE and leaves nothing
   open if any of that fails, so titanStart can report failure rather than
   half-starting. */
bool32 lanStart(void);
void   lanShutdown(void);

/* Accept, read, dispatch and flush. Cheap when idle. Safe to call when
   lanStart() failed or was never called. */
void   lanService(void);

/*-----------------------------------------------------------------------------
    Addressing
----------------------------------------------------------------------------*/
/* Our own address in network byte order, matching Address.AddrPart.IP.
   Discovered from the interface list, not by broadcasting a packet to
   ourselves and blocking until it arrives, which is what this replaced. */
udword lanLocalAddress(void);

/*-----------------------------------------------------------------------------
    Discovery

    UDP broadcast, so it reaches a LAN and nothing beyond it. Internet play
    does not use this at all: there the player types an address and
    lanConnect() is called directly.
----------------------------------------------------------------------------*/
void   lanSendDiscovery(const void* data, uword length);

/*-----------------------------------------------------------------------------
    Connections and messages

    lanSendTo() addresses a peer by IP whether or not a direct link exists.
    On a LAN every peer connects to every other, so there always is one. Over
    the internet only the captain is reachable, and the rest is relayed; see
    the topology section of documentation/vr-multiplayer-plan.md.
----------------------------------------------------------------------------*/
bool32 lanConnect(udword ip);
void   lanSendTo(udword ip, ubyte messageType, const void* data, uword length);
void   lanSendToAll(ubyte messageType, const void* data, uword length);

/* Whether a usable link to this peer exists right now. */
bool32 lanPeerConnected(udword ip);

/*-----------------------------------------------------------------------------
    Delivered upward by lanService(): this one to TitanInterfaceC.c, which
    already knows how to dispatch a TITANMSGTYPE_*, and discovery packets to
    titanReceivedLanBroadcastCB, declared in TitanInterfaceC.h.
----------------------------------------------------------------------------*/
void HandleTCPMessage(udword address, ubyte messageType, const void* data, uword length);

#endif
