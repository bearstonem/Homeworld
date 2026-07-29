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
    Remote peers, for play across the internet

    Broadcast does not leave the subnet, so a remote peer has to be named. Once
    named, it is sent the same advertisements a LAN peer would have picked up
    off the broadcast, and it answers with its own. Nothing about the protocol
    changes; only the set of addresses the packets go to.

    Learning is symmetric on purpose. A host cannot know a remote client exists
    until that client speaks, so any address we receive discovery from and do
    not already have is added. That is what lets a configured client reach a
    host which has configured nothing.

    Everyone publishes the address their own machine has, so behind a router
    both ends name each other by addresses that only mean something on the
    other's LAN. Every routing decision therefore goes through lanRouteTo (see
    lan.c), which resolves an unreachable address to the single named remote.
    That makes internet play a two-player affair for now, and it cannot help
    when both ends sit on the same private range.
----------------------------------------------------------------------------*/
void   lanAddRemote(udword ip);
bool32 lanHaveRemotes(void);

/* Record that a peer which calls itself `published` is actually reachable at
   `reachable`, so routing to it stops being a guess.

   The guess - "with exactly one remote named, an unreachable address must be
   that one" - is what held internet play to two players: with two remotes
   there is no way to tell which of them a given private address belongs to.
   Nothing needs guessing, because every advertisement carries the sender's own
   idea of its address while the socket reports where it really came from. Only
   the caller knows how to read the first out of the packet, so it hands both
   down here. Cheap and idempotent: it is called once a second per peer. */
void   lanAddAlias(udword published, udword reachable);

/* Nominate a peer to forward for us: anything with no path of its own is
   wrapped and handed to `ip` instead of being dropped, and that peer passes
   it on. 0 turns it off.

   This is what lets everyone except the host skip configuring a router.
   Homeworld's lockstep is a full mesh - every peer sends to every other peer -
   so without it each pair needs a path between them, which means every player
   forwarding ports, which a player on mobile data cannot do at all.

   The game above is unaware; it still addresses everyone directly. Costs one
   extra hop between clients, on packets carrying commands rather than world
   state, in a game where everyone already runs at the pace of the slowest
   link. Set it to the captain: the captain is the one peer everybody has a
   path to, by construction, since joining required it. */
void   lanSetRelay(udword ip);

/* Whether an address is one we could plausibly reach directly: on a local
   subnet, or a remote we already know. A game advertised from behind NAT
   carries its host's private address, which is meaningless to us. */
bool32 lanAddressIsLocal(udword ip);

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
