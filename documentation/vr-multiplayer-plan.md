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
is the useful part of the finding: most of the work is testable as two
processes on a Linux desktop, and the VR-specific layer is the last one, not
the first.

The recommended first step needs no networking at all. See
[M0](#m0--make-start-game-work-with-no-networking).

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
`MultiplayerGame.c:3925`. The stub returns 0
(`src/SDL/TitanInterfaceC.c:100`), so `mgStartGameCB()` is never reached and
the Start button does nothing at all. The original says what the LAN case
should do (`TitanInterface.cpp:292`): set `GAME_STARTED`, call
`InitPacketList()`, return true.

This is worth emphasising because it means **the Start button is broken for
Skirmish too**, which needs no network whatsoever.

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

### LAN only, and same-ABI peers only, for a first version

WON is gone; internet play needs a lobby or relay service and is out of scope.
Keeping the `Address`-keyed API intact leaves the door open.

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

### M0 — Make Start Game work, with no networking

Fix `titanReadyToStartGame` for the LAN and single-human cases to match
`TitanInterface.cpp:292`, and decide what to do about
`NEED_MIN_TWO_HUMAN_PLAYERS` (`MultiplayerGame.h:426`, currently `1`), which
rejects a one-human Skirmish at `MultiplayerGame.c:3937`.

Then run Skirmish on the Quest.

This is the highest value step by a wide margin. Skirmish walks the entire
lobby-to-game-start path — connection method, scenario picker, options, player
wait, and then the multiplayer *game* rules — without sending a single packet.
It answers the question that sizes everything else: how much of the 1999
multiplayer path still works in a 64-bit build that has never run it. It is
also shippable on its own, as Skirmish against the AI in VR.

### M1 — `src/SDL/lan.c`, validated on the desktop

Implement the `titan*` contract against `TitanInterface.cpp` as the reference:

- UDP broadcast for advertisement and discovery, `titanSendLanBroadcast` out
  and `titanReceivedLanBroadcastCB` in.
- TCP mesh for both lobby and in-game traffic, behind `titanSendPacketTo`,
  `titanBroadcastPacket` and `titanConnectToClient`.
- Non-blocking throughout, length-prefixed framing, real disconnect handling,
  no `exit()`.
- Local address from `getifaddrs`, not from a self-addressed broadcast.

Test as two processes on loopback. Fast iteration, no headset, and the network
log (`/logOn`, `/logOnVerbose`, `main.c:791`) already exists to read.

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
