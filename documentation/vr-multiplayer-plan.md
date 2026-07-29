# Multiplayer, and what it would take to have any

## Summary

Homeworld's multiplayer logic is all here and intact — the lockstep command
engine, the captain model, the sync checksums, the lobby and its front-end
screens, roughly 18,000 lines of it. What is missing is the **transport
underneath**: every `titan*` entry point is a stub that logs its own name and
returns zero. One attempt at a real transport exists, from HomeworldSDL around
2005, and it is disabled, uncompilable, and built on a topology the game does
not actually use.

So multiplayer is dead on *every* platform in this fork, not only in VR. That
is the useful part of the finding: almost none of the work is VR work, and the
VR-specific layer is the last one rather than the first.

Neither LAN nor internet play works today, and no amount of front-end fixing
changes that: the work is the transport. See
[M1](#m1--srcsdllanc-validated-on-the-desktop).

## What is already here

| File | Lines | What it does |
|---|---|---|
| `src/Game/CommandNetwork.c` | 2825 | Lockstep command/sync packets, captain server, lag and drop handling |
| `src/Game/MultiplayerGame.c` | 7820 | Lobby front end: connection method, options, player wait, chat |
| `src/Game/MultiplayerLANGame.c` | 3097 | The LAN-specific half of those screens |
| `src/Game/NetCheck.c` | 967 | Per-frame sync checksums |
| `src/Game/TitanNet.c` | 919 | Lobby protocol on top of the transport |
| `src/Game/Captaincy.c` | — | Captain transfer, i.e. host migration |

`SYNC_CHECK` is `1` (`src/Game/Switches.h:14`), so the desync detector is
compiled in and will report a checksum mismatch with the frame it happened on.
That is the acceptance test for everything in
[M5](#m5--determinism-audit-of-the-vr-layer), free of charge.

There is also `src/SDL/TitanInterface.cpp` — 6563 lines of the original Relic
WON implementation, wrapped in `#if 0` from its first line and absent from
`src/SDL/meson.build`. It is dead code, but it is an exact specification of
what each `titan*` function is supposed to do, which removes most of the
guesswork from writing a replacement.

## What is missing, precisely

### The transport is a stub

`src/SDL/TitanInterfaceC.c` is `dbgMessagef` and `return 0` throughout. Its
one live consequence is that `titanStart` returns 0
(`src/SDL/TitanInterfaceC.c:51`), so pressing Launch on the LAN login screen
falls through both protocol attempts at `MultiplayerLANGame.c:782` and puts up
"no LAN IPX or TCP/IP".

### The one real transport does not compile

`src/SDL/NetworkInterface.c` and the `HW_ENABLE_NETWORK` half of
`TitanInterfaceC.c` are a partial SDL_net LAN implementation. Nothing defines
`HW_ENABLE_NETWORK` — the only place that ever did is the abandoned autotools
build (`Linux/stuff/configure.ac:218`). `NetworkInterface.c` is in
`src/SDL/meson.build:7` and compiles to an empty object file.

Forcing the flag on, against a hand-written stub of `SDL_net.h`, gives:

```
TitanInterfaceC.c:165  error: 'mGameCreationState' undeclared
TitanInterfaceC.c:165  error: 'GAME_NOT_STARTED' undeclared
TitanInterfaceC.c:382  error: (the same two again)
TitanInterfaceC.c:407  error: conflicting types for 'HandleJoinReject'
NetworkInterface.c:6   fatal: SDL2/SDL_net.h: No such file or directory
```

`mGameCreationState` and `GAME_NOT_STARTED` only exist as members of a C++
class (`src/SDL/TitanInterface.h:175`), so C cannot see them. The two headers
also disagree about how to include SDL_net: `NetworkInterface.c:6` says
`<SDL2/SDL_net.h>` and `NetworkInterface.h:7` says `"SDL_net.h"`.

### And even fixed, no game could start

`mgStartGame` → `mgReallyStartGame` → `if (titanReadyToStartGame(...))` at
`MultiplayerGame.c:3925`. The stub returns 0, so `mgStartGameCB()` is never
reached and the Start button does nothing at all. The original says what the
LAN case should do (`TitanInterface.cpp:292`): mark the game started and
return true. Porting that needs an enum, and the obvious names are a trap:
`Globals.h:41` already defines `GAME_STARTED` as `5` for the unrelated
`startingGameState` machine, and an unprefixed enum constant in a header this
widely included would be textually replaced by that macro. The C++ original
was safe only because its enum was scoped inside the class.

`MG_StartGame` is bound on exactly one screen, **Captain_Wait**, which is the
LAN and internet flow. Since `titanStart` fails before a player can ever reach
that screen, this gate is currently unreachable and untestable; it is fixed
here on the reference implementation's authority, not on a passing test.

> **Skirmish does not go through any of this.** Verified on device
> 2026-07-28: a skirmish started fresh from the menu logs no call to
> `titanReadyToStartGame` at all. The SKIRMISH button binds to
> `StartNewGame`, a `utyCallbacks` entry (`src/SDL/utility.c:451`) rather than
> an `mgCallBack` one, and `utyNewGameStart` takes its `!multiPlayerGame`
> branch: `numPlayers = 1 + tpGameCreated.numComputers` with
> `ComputerPlayerEnabled[i] = TRUE`. Skirmish is a **single-player game with
> AI opponents** that borrows the multiplayer screens as a configuration UI.
> No lockstep, no captain, no sync checksums. It therefore proves nothing
> about `CommandNetwork.c`, and an earlier draft of this plan was wrong to
> propose it as the way to find out.

## Four decisions to take before writing code

### Sockets, not SDL2_net

SDL2_net is not installed on the build host, not vendored under `android/`
(SDL2 itself is, as `android/SDL` built into `sdl-prefix-arm64`), and has no
pkg-config entry. Using it means adding a cross-compiled dependency to the
Quest build for what is a thin wrapper over BSD sockets, which Android, Linux
and macOS all have natively. Windows needs a small Winsock shim; `mingw/`
already exists for that kind of thing.

The rewrite also sheds `NetworkInterface.c`'s real defects rather than
inheriting them: `exit()` on twelve separate error paths, a `getMyAddress()`
that discovers the local IP by broadcasting a ping to itself and then blocks
its caller forever waiting for it (`NetworkInterface.c:203`), and hardcoded
ports (`TCPPORT 10500`, `UDPPORT 10600`).

Keep the old file as a reference for the intended call graph. Do not keep it
as a base.

### Full mesh, not a star

`NetworkInterface.c` assumes non-captains only ever talk to the captain: once
`clientActive` is set, `putPacket` ignores its address argument entirely and
writes to `clientSock` (`NetworkInterface.c:513`).

That is *almost* what the game does, which is presumably why the assumption
survived. Command packets go to the captain (`CommandNetwork.c:633`), sync
packets come back only from it (`:655`), and even chat is point-to-point
relayed through the captain rather than broadcast
(`SendChatPacketPacket`, `CommandNetwork.c:2364`).

But three paths break it, and any player can hit all three:
`SendDroppingOutOfLoad` (`CommandNetwork.c:2255`), `SendCheatDetect`
(`:2274`) and `SendInGameQuittingPacket` (`:2294`) each call
`titanSendBroadcastMessage` with no `IAmCaptain` guard. That fans out over
every player in `tpGameCreated.playerInfo[]` (`TitanNet.c:737`, whose comment
already admits "Fix later for non-captain broadcasting stuff"), so on the star
topology the captain receives N copies and nobody else receives any — meaning
a player who quits or drops mid-load is invisible to every peer except the
captain.

Eight players is 28 TCP connections, which is nothing on a LAN, and a mesh
makes `titanSendPacketTo(any peer)` and `titanBroadcastPacket` both correct
without special cases.

**Internet play pulls the other way, so the transport must do both.** A mesh
needs every player reachable, which means every player port-forwards. A star
needs only the captain reachable, which is the difference between one person
configuring a router and eight. So the transport should expose "send to
player N" and "broadcast" as *logical* operations and route them over
whatever links exist: direct where there is one, relayed by the captain where
there is not. LAN then runs as a mesh because it costs nothing, and internet
runs as a star with one forwarded port.

One thing a star cannot do is survive losing the captain. Host migration is
real in this engine — `TransferCaptaincyPacket` and the `titanAnyone*`
variants exist precisely for the window where there is no captain to relay
through (`CommandNetwork.c:2200`) — and if the captain was the only reachable
peer, the survivors cannot elect a replacement. Losing the host therefore ends
an internet game unless every player is reachable. That is a normal
limitation to ship with, but it should be a decision rather than a surprise.

### LAN first, internet second, and same-ABI peers throughout

Both are wanted (stated 2026-07-28). They are mostly the *same* work: one
transport carries both, and internet play is that transport plus a way to
reach a peer that broadcast cannot find.

The staging follows from what each adds:

1. **LAN.** UDP broadcast discovers games, TCP carries the rest. Everything
   in M1.
2. **Internet, host-reachable.** The same transport, joined by typed address
   instead of discovered, with the captain's TCP port forwarded. Broadcast
   does not cross the internet, so the join-by-IP path stops being a
   fallback and becomes the primary route. No new protocol.
3. **Internet, both peers behind NAT.** The hard case, and the only one
   needing infrastructure. WON solved it with routing servers, which is what
   `titanBehindFirewall`, `userBehindFirewall` and the Choose_Server screens
   are vestiges of. Options are a relay we host, or hole punching. Defer
   until 1 and 2 work.

Lockstep over the internet is not the obstacle it might sound like: the
original shipped this exact scheme in 1999 over dial-up, and
`CAPTAINSERVER_PERIOD` is 1/8s. Every player still runs at the pace of the
slowest peer, so a distant player slows the whole game — that is inherent to
the design and not something the transport can fix.

Peer compatibility splits cleanly in two. **In-game packets are portable**:
`HWPacketHeader` and every command body use fixed-width `uword` / `udword` /
`real32` (`CommandNetwork.h:232`). **Lobby structs are not**: `Address`
(`TitanInterfaceC.h:47`) and `CaptainGameInfo` (`:152`) travel as raw `memcpy`
and contain `unsigned long` and `wchar_t`.

| Pairing | `unsigned long` | `wchar_t` | Verdict |
|---|---|---|---|
| Quest ↔ Quest (aarch64 LP64) | 8 | 4 | works |
| Quest ↔ Linux x86_64 | 8 | 4 | works |
| Quest ↔ Windows (LLP64) | 8 vs 4 | 4 vs 2 | different layout, does not work |

So version one is Quest-to-Quest and Quest-to-Linux, stated plainly in the
player documentation. `networkVersion` (`src/SDL/main.c:260`, still
`"HomeworldSDL"` by deliberate decision — see `VR_PORT_HANDOFF.md`) is the
gate that keeps incompatible builds from finding each other
(`TitanNet.c:915`), and `/forceLAN` (`main.c:797`) is the override for
testing. Giving the lobby structs an explicit wire format is a later and
separate job.

### Prefill, do not build a keyboard

The precedent is already set: `gpSuggestSaveName` in `src/Game/GamePick.c`
fills an empty save-name field rather than asking a headset for typing.

- **Player name** — `utyName` is already settable from the config script
  (`src/SDL/utility.c:763`) and already has a default
  (`utility.c:3893`). Prefill it the same way.
- **Game name** — prefill from the player name.
- **Passwords** — skip. Nothing on a home LAN needs them.
- **Chat** — out of scope for a first version, or a short preset list.
- **Join by IP** — the one place that genuinely needs input, and it is ten
  digits and a dot. A small keypad, not a keyboard.

## Milestones

### M0 — Done, and it answered less than intended

`titanReadyToStartGame` now implements the LAN and single-human cases from
`TitanInterface.cpp:292`, with `mGameCreationState` ported to C under
`TITANGAME_*` names to clear the `GAME_STARTED` macro collision. Internet
games still return 0, so they fail visibly rather than starting into a
transport that cannot carry them. The same enum work also took
`TitanInterfaceC.c` from 5 compile errors to 0 under `HW_ENABLE_NETWORK`,
which was owed to M2.

The intent was to use Skirmish as a no-network rehearsal of the multiplayer
game path. **That premise was wrong**, for the reason recorded above: Skirmish
never touches this code, and is a single-player game with AI opponents. It ran
on device before any of these changes and still does.

What the device run did establish, which is not nothing: the multiplayer
*screens* — connection method, scenario picker, basic and advanced options,
CPU opponent count — are all reachable, legible and clickable on the VR panel,
and the game they configure loads and plays. So M4's front-end concern is
smaller than feared, and the remaining question there is text entry.

What it did **not** establish is anything at all about `CommandNetwork.c`,
the captain model, or the sync checksums. Nothing exercises those without a
transport, so that question moves wholly into M1. `NEED_MIN_TWO_HUMAN_PLAYERS`
was left alone: it guards `mgStartGame`, which only Captain_Wait reaches, so
it is a LAN concern and not a Skirmish one.

### M1 — `src/SDL/lan.c`, validated on the desktop

Implement the `titan*` contract against `TitanInterface.cpp` as the reference:

- UDP broadcast for advertisement and discovery, `titanSendLanBroadcast` out
  and `titanReceivedLanBroadcastCB` in.
- TCP mesh for both lobby and in-game traffic, behind `titanSendPacketTo`,
  `titanBroadcastPacket` and `titanConnectToClient`.
- Non-blocking throughout, length-prefixed framing, real disconnect handling,
  no `exit()`.
- Local address from `getifaddrs`, not from a self-addressed broadcast.

Two processes on one machine will **not** work as peers, so do not plan around
it. The game compares peers with `InternetAddressesAreEqual`, which tests the
IP and nothing else, so two instances behind one address are the same peer as
far as every layer above the socket is concerned. Real testing needs two hosts.

What is testable without a second machine is the transport by itself, driven
from a harness that stubs `HandleTCPMessage` and
`titanReceivedLanBroadcastCB`: startup, address discovery, broadcast
round-trip, accept, frame reassembly across a split write, batched frames in
one read, zero-length payloads, peer drop and restart. That is the whole of
lan.c short of two-host routing, and it catches the framing bugs that are
otherwise found by watching a game desync.

### M2 — Build and Android wiring

A `network` option in `meson_options.txt` defining `HW_ENABLE_NETWORK`, and
the manifest permissions, which are currently absent entirely: `INTERNET`,
`ACCESS_NETWORK_STATE`, and `CHANGE_WIFI_MULTICAST_STATE` for broadcast
receipt.

Note the APK carries two engine libraries and the demo one keeps multiplayer
off regardless — `mgLANIPX` disables its own button under `HW_GAME_DEMO`
(`MultiplayerGame.c:1855`). Only `libmain.so` needs any of this.

### M3 — On device

Quest against Linux first, since that isolates the transport from the headset.
Then Quest against Quest.

The unknowns here are all device-level and none can be settled by reading
code: whether the access point passes broadcast between wireless clients (many
isolate them, which is what join-by-IP is for), and what a headset going to
sleep or backgrounding does to a lockstep game, where one stalled peer stalls
everybody.

### M4 — The VR front end

The lobby screens need no new presentation layer. Out of game
`vr.worldInteractive` is false (`src/SDL/vr.c:4692`) and the whole 1024x768 UI
already goes to a pointer-driven panel; `vrWorldManagerActive`
(`src/SDL/vrworld.c:384`) is a whitelist of *in-game* managers and the lobby
is not one of them, so nothing there needs touching.

What is needed is the prefilling from the previous section, the join-by-IP
keypad, and a legibility pass: these are dense screens, and everything prim2d
draws is stretched about 1.4x horizontally (see `VR_PORT_HANDOFF.md` on the
two coordinate spaces).

### M5 — Determinism audit of the VR layer

Mostly reassuring already:

- **No RNG use.** Neither `vr.c` nor `vrworld.c` calls `ranRandom`, so the VR
  layer cannot pull the simulation's random stream out of step.
- **Orders replicate by construction.** Everything goes out through
  `clWrap*`, which is the layer that marshals multiplayer packets.
- **The flight-path follower already anticipated this.**
  `vrw.pathSteering = !multiPlayerGame` (`vrworld.c:2231`); in a network game
  it stops writing `move.destination` and `ship->aistatecommand` behind the
  command layer's back and falls back to a rate-limited `clWrapMove`
  (`vrworld.c:2495`).

Still to check: whether `VRW_PATH_REISSUE_FRAC` throttles that fallback enough
not to flood the command stream; that runtime world scale, the 3x draw
distance and disabled N-LIPS are strictly render-side; and that no command
wheel entry reaches a bare `cl*` instead of `clWrap*`.

### M6 — In-game multiplayer, in VR

Multiplayer does not pause — `utyInGameCancel` (`src/SDL/utility.c:3124`)
gates `universePause` on `!multiPlayerGame` — which makes the wrist panel the
only safe way to open a menu mid-battle. Beyond that: chat display, the
player-dropped dialog, and the sync-error message, all of which currently
assume a monitor.

## Risks

- The lockstep code has never run in this fork. There is unknown bit-rot
  behind the `-Wno-*` list in `meson.build`, and 64-bit was not a thing it was
  written for.
- Wi-Fi jitter against an engine designed for a wired 1999 LAN.
- Headset sleep stalling the game for every player, which has no desktop
  equivalent and therefore no existing handling.

## Verification

1. Skirmish reaches a running game on device, and the mission plays.
2. Two desktop processes complete a LAN game start to finish with no sync
   error reported by `NetCheck`.
3. A Quest and a desktop do the same.
4. Two Quests do the same.
5. VR-issued orders — including a drawn flight path, which is the one feature
   that touches the command layer unusually — produce no sync error over a
   full game.
6. Dropping one peer mid-game leaves the others playing, and captain transfer
   works when the dropped peer was the captain.
