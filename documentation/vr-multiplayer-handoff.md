# Multiplayer — working notes

Companion to `documentation/vr-multiplayer-plan.md`, which is the plan. This
is the state of it, the things that cost time to find, and what to do next.

State as of the VR keyboard work on `main`, 2026-07-28.

## Where it got to

**LAN play works.** Verified 2026-07-28 between a Quest 3 and a Linux desktop:
both peers open sockets, find each other by UDP broadcast, appear in each
other's lobby, join over TCP, and load into the same game. Both reached
`Cap:I Am 0`/`Cap:I Am 1 in 2-player game` and `Cap:Transition to state
normal`, so `CommandNetwork.c` and `Captaincy.c` ran for the first time in
this fork and agreed with each other about who was captain.

**There is a keyboard.** `src/SDL/vrkeys.c`, drawn on the panel, up whenever
any text entry in the game holds key focus — the player name, the game name,
chat, a save game's name, all of them. It carries the join-by-address field
too, since no such field exists in the game's own screens. See
[Typing in VR](#typing-in-vr).

**Internet play works, across a real WAN.** Verified 2026-07-28: a Quest 3 on
mobile data, behind carrier-grade NAT, joining a Linux desktop behind a home
router with TCP 10500 and UDP 10600 forwarded to it. Both reached
`Cap:I Am 0`/`Cap:I Am 1 in 2-player game` and `Cap:Transition to state
normal`. Three separate bugs stood in the way and all three are fixed:

1. **Nothing could be typed**, so there was no way to name a host from inside
   a headset at all. See [Typing in VR](#typing-in-vr).
2. **Peers were addressed by names the other end had never heard of.** Every
   machine publishes the address it has locally, so behind a router both ends
   name each other by addresses that only mean something on the other's LAN.
   `lanConnect` already substituted the named remote when it dialled; nothing
   else did, so the link came up and every message on it was dropped by
   `lanSendTo` for want of a peer at that address. `lanRouteTo` now applies
   that rule everywhere a peer is named.
3. **Replies went to the wrong port**, and **the captain filed a joining
   player under the wrong address.** Both below, because both are the sort of
   thing that only shows up on a real connection.

Everything short of a WAN passed while all three were broken, which is the
point worth taking from it: a LAN is not a weak version of the internet, it is
a different case, and the two addresses a LAN keeps equal are exactly the ones
that break apart.

**No sync error has been seen**, but nor has a game been played out. A match
that has just loaded has not had the opportunity to desync. `SYNC_CHECK` is 1
(`Switches.h:14`) so a mismatch reports itself with a frame number; that
remains the acceptance test and it has not been run in anger.

## Traps

The expensive part of the session. None of these are in the networking.

- **Android needs `INTERNET` permission before `socket()` does anything.**
  Without it every call fails with `EACCES`, the transport cannot open its
  listener, and the LAN screen reports "no LAN IPX or TCP/IP" exactly as it
  did when there was no transport at all. Indistinguishable from the
  transport being broken.
- **Android's Wi-Fi stack drops packets not addressed to the device**, which
  includes the broadcast that advertises a game. `HomeworldActivity` holds a
  `MulticastLock` for the life of the activity. Without it, hosting and
  joining by address work and nobody ever *appears*, which is a confusing way
  to fail.
- **`Globals.h:41` defines `GAME_STARTED` as `5`**, for the unrelated
  `startingGameState` machine. Any unprefixed enum constant of that name in a
  widely included header gets textually replaced by it. The C++ original was
  safe only because its enum was scoped inside a class. Hence `TITANGAME_*`.
- **`TitanActive` is declared `FALSE` in `TitanNet.c:45` and was never
  assigned anywhere.** `Task.c` gates `titanPumpEngine` on it, so until
  `titanStart` set it the transport was never serviced.
- **`titanBroadcastPacket` used to send only while the game had not started.**
  The original branches three ways and sends in all three. Harmless while
  nothing moved the state; fatal the moment `titanReadyToStartGame` began
  setting `TITANGAME_STARTED` for real, because in-game traffic goes through
  the same function.
- **Two assets the multiplayer screens reference are not in retail or
  Remastered data.** `Feman\TexDecorative\Won_logo_small_fade.lif` (WON
  branding, while the other 205 textures in that directory are present) and
  `ScreenShots/ShotList.script`. Both were fatal. Nobody had reached these
  screens in this fork, so nobody had noticed.
- **An empty filename is not harmless.** `filePathPrepend("")` resolves to the
  data directory, `fopen` succeeds on a directory, so `fileExists` says yes
  and `fileSizeGet` then returns -1. `memAlloc(-1)` is fatal. The tell is an
  allocation named after the working directory.
- **`ferTextureRegister` must never return NULL.** Around 130 call sites
  dereference its result immediately and not one checks. A missing texture
  gets a 1x1 transparent placeholder instead. It has to be a real registry
  element rather than an early return, because `ferDraw` reads a `g_Entry`
  the registry sets.
- **Casting a pointer to `sdword` on the way into a region ID truncates it.**
  `regChildAlloc` takes an `smemsize`, which is 64-bit under `HW_PTR_64`, but
  `GameChat.c` cast to `sdword` first. `feUserRegionDraw` then dereferenced
  half a pointer. Only multiplayer creates that region, which is why it
  shipped. Swept; no such casts remain.
- **Skirmish is not multiplayer.** Its button binds to `StartNewGame`, a
  `utyCallbacks` entry, and `utyNewGameStart` takes its `!multiPlayerGame`
  branch. It is single player with AI opponents borrowing the multiplayer
  screens as a configuration UI, and proves nothing about lockstep.
- **Two processes on one host cannot be peers.** The game compares peers with
  `InternetAddressesAreEqual`, which tests the IP and nothing else, so they
  are the same peer to everything above the socket. Testing needs two hosts.
- **`dbgMessagef` reaches logcat.** `Debug.c:124` mirrors it through
  `SDL_Log` under `__ANDROID__`, unconditionally. Cheapest instrumentation
  available on device.
- **AddressSanitizer cannot see the game's heap.** `memAlloc` is the engine's
  own allocator, so use-after-free inside it presents as a bare SEGV with no
  ASan report. It still catches globals and the real heap, which is how the
  `AliveTimeoutTimers[-1]` write was found.
- **`grep` silently finds nothing in several of these sources.** They carry
  extended-ASCII bytes that are not valid UTF-8 — `UIControls.c` is the worst
  of them — and in a UTF-8 locale GNU grep gives up on the file and reports no
  matches at all rather than an error. It is not "the symbol is not there", it
  is "grep declined". Half an hour went into concluding that `regKeysFocussed`
  was dead code and `uicTextEntryProcess` did not exist, both of which are
  false. Use `LC_ALL=C grep -a`.
- **The screens in `multiplayer_lan_game.fib` are named with an `L` prefix**
  — `LChannel_Chat`, `LLAN_Login`, `LCaptain_Wait` — where the internet ones
  in `multiplayer_game.fib` are not (`mgShowScreen`). Matching on the
  unprefixed name finds the wrong screen, or none.
- **`tools/big_extract.py` reads the retail `.big`,** which is how the screen
  layouts were read at all. A `.fib` is a 12-byte header, then 20 bytes per
  screen, then 76 bytes per atom (the 32-bit on-disk form in `FEFlow.h`), and
  atom rectangles are two corners rather than a corner and a size until
  `feScreensLoad` converts them. Worth rebuilding that dumper before guessing
  where anything is on a screen.

## The transport

`src/SDL/lan.c`, replacing `NetworkInterface.c` (SDL_net, disabled since 2005,
did not compile, assumed a topology the game does not use).

- One UDP socket on 10600 for advertisements, one TCP listener on 10500 for
  peers. Frames are `[type:u8][length:u16 LE][payload]`.
- **No threads.** Serviced entirely from `titanPumpEngine`, which `Task.c`
  calls once per active task per scheduler pass. The old implementation's
  races were with a game whose allocator and front-end queues are not thread
  safe.
- Nothing blocks. A peer that stops reading fills its own outbound buffer and
  is dropped rather than stalling the frame.
- Remote peers (internet play) are addresses that advertisements are unicast
  to as well as broadcast. Learning is symmetric: any non-local address we
  receive discovery from is added, so only the joining side configures
  anything.
- **A peer is not reachable at the address it calls itself.** Everyone
  publishes what `getifaddrs` gave them, so behind a router the lobby names
  the other end by an address that only means something on its own LAN, while
  the link is keyed by the address the socket really uses. `lanRouteTo`
  resolves the one to the other: when exactly one remote is named, that remote
  is necessarily the other end of the internet link. Every path that names a
  peer goes through it — `lanConnect`, `lanSendTo`, `lanPeerConnected` — and
  the first of those was the only one that used to, which is why internet play
  could connect and then do nothing.

  Two consequences worth knowing before relying on it. Over the internet this
  is a **two-player** transport: with several remotes named the resolution is
  ambiguous and is deliberately not guessed. And it **cannot help when both
  ends sit on the same private range** — two 192.168.1.x home networks, which
  is the common case — because the peer's address then looks local, is left
  alone, and reaches somebody else's machine or nothing at all. Fixing that
  properly needs the peers to exchange who they are on connect rather than
  inferring it from addresses; there is no room in the frame header for it
  today and the lobby has nowhere to put it.
- **A reply goes to the port the packet came from, not to `LAN_UDP_PORT`.** A
  peer on mobile data is behind carrier-grade NAT, where thousands of
  subscribers share one address and the source port therefore *cannot* be
  preserved. Discovery used to keep only the address and answer on 10600; the
  advertisement crossed the internet, was answered into a void, and no game
  ever appeared. Remotes now carry the port they were heard on and follow it
  when the carrier reassigns it. An address a player typed keeps 10600, since
  that is what the host forwards, and is corrected the moment they answer.
- **A peer is identified by the address it calls itself, not by the one our
  socket reports.** The captain used to file a joining player under the
  socket address — identical on a LAN, different behind NAT — so every player
  got a lobby naming the joiner by an address the joiner had never heard of,
  and `mgGameStartReceivedCB` called `dbgFatal` when it could not find itself
  in the list. `PlayerJoinInfo` now carries the joiner's own address. Note
  the two questions this separates: *who a peer is* is what it publishes,
  *how to reach it* is what `lanRouteTo` works out. Conflating them is what
  every one of these bugs had in common.

There is a standalone harness for it. It covers startup, address discovery,
accept, the routing rule above (both as a unit and end to end against a plain
socket standing in for the other machine), a frame split across two writes,
two frames in one read, and zero-length payloads. It is not in the repository;
recreate it by `#include "lan.c"` — the routing rule is static — and stubbing
`HandleTCPMessage`, `titanReceivedLanBroadcastCB` and `dbgMessagef`. Build it
against `git show <old>:src/SDL/lan.c` as well as the current file: that is
how the routing change was shown to fix something rather than to look like it
did. Worth doing before touching framing or addressing.

`lan.c` cannot be both ends of a link, so the harness must be the other end
itself: `lanServiceAccept` dedups an inbound connection against an existing
peer at the same address, and on loopback those are the same address.

## Running the two-machine test

Desktop side needs game data next to the binary. It is not in the repository
and the paths used during the session pointed into a temporary directory, so
recreate them:

```sh
D=/sdcard/Android/data/org.homeworldunbound.game/files
adb -s <headset> pull $D/homeworld.big  build/Homeworld.big
adb -s <headset> pull $D/HW_Music.wxd   build/HW_Music.wxd
adb -s <headset> pull $D/HW_Comp.vce    build/HW_comp.vce
ln -sf "$PWD/dist/assets/loading.jpg"   build/loading.jpg
cd build && DISPLAY=:1 ./homeworld /forceLAN /logOn
```

`/forceLAN` skips the version check between peers. The desktop build carries
AddressSanitizer by default, which is worth keeping: it found a bug the Quest
build silently tolerated.

Headset side is the usual deploy loop from `VR_PORT_HANDOFF.md`. Both native
libraries need copying, not just the full-game one.

Then: host on one, join from the other, START GAME on the host. The host's
Setup Game screen needs a **game name of two or more characters** or
`mgInvalidGameName` bounces it back without saying why.

## Typing in VR

`src/SDL/vrkeys.c`. Decided 2026-07-28: a player must never edit a file to
host or join, so this came before everything else.

**It is not a multiplayer feature.** It is up whenever *any* text entry in
the game holds key focus, found by walking the region tree from
`regRootRegion` and matching `uicTextEntryProcess`. That covers the player
name, the game name, lobby and in-game chat, game and room passwords, the
resource-option numbers, and the escape menu's save-game name — every
`FA_TextEntry` in every `.fib`, plus the ones `GameChat.c` and `HorseRace.c`
focus by hand. Matching on the process function and not on the status bit
matters: list windows take `RSF_KeyCapture` too (`uicSetCurrent`), and a
keyboard over a focused list would be wrong.

**It is on the panel, not on a card.** A fourth wrist card would have needed
a new swapchain, a new pose and new ray hit-testing. `vr.pointerX/pointerY`
is already the controller ray mapped onto the panel, so the grid is drawn
into the window framebuffer just before `vrCopyFrame` — the same place the
reticle goes — and hit-tested against that pointer. The card system is there
if a reason to prefer it appears (`vr.card[]`); it was rejected on cost.

Findings behind it, all verified:

- **Synthesized keys already reach text entry.** `main.c` does
  `keyPressDown(keyLanguageTranslate(pEvent->key.keysym.scancode))`, so the
  engine keys off the **scancode**. This is how Escape works in VR today.
- **Never send characters, only scancodes.** But note *how* the engine gets
  from one to the other, because it is not what it looks like:
  `uicTextEntryProcess` does `keycode = SDL_GetKeyFromScancode(data)` and
  then indexes `uicKeyEntryTable` **by ASCII**, not by scancode — entry 65 is
  `{'a','A'}`, entry 46 is `{'.','>'}`. So the whole path depends on SDL's
  default keymap being populated with no physical keyboard attached. If it
  ever is not, every key silently types nothing; `vrKeysPushKey` logs the
  resolved keycode once for exactly that reason.
- **Shift has to be genuinely held, not applied as a transform.**
  `Region.c` reads the key and its shift state together with
  `keyBufferedKeyGet(&bShift)`; `keyPressDown` records it with
  `keyBufferAdd(key, keyIsHit(SHIFTKEY))` at press time. So a shifted glyph
  pushes `LSHIFT` down, the key down and up, then `LSHIFT` up — all in one
  batch, and the release afterwards is harmless because the buffer already
  captured the state.
- **CAPS latches, and applies to letters only.** One ray cannot point at
  shift and a letter at once. A latch that pushed `LSHIFT` for everything
  would turn the number row into `!"#`, and an address is digits and dots, so
  the latch is the keyboard's own flag and the few shifted glyphs worth
  having (`_`, `?`) carry their own shift in the key table.
- **`regKeysFocussed` is not the trigger, though it looked like it.** It is
  set correctly (`UIControls.c`), but it is a single global that says
  *someone* has focus, not who, and the layout needs the focused entry's
  rectangle: the keyboard sits along the bottom and has to move to the top
  when that is where the entry is, which on the lobby it is.

Beyond the letters, `uicTextEntryProcess` handles `ESCKEY`, `RETURNKEY` and
`ENTERKEY`, `BACKSPACEKEY`, `DELETEKEY`, and `ARRLEFT`/`ARRRIGHT`. All are on
the grid. Keys repeat while held, except the ones that commit or toggle —
holding CAPS would otherwise flicker the latch and holding ENTER would submit
the same address over and over.

**In game it behaves like an open manager** (`keysOpen` in `vrUpdateInput`):
the panel is submitted whatever the player toggled with Y, it owns any ray
that crosses it, and the command wheel stands down so the left trigger can
click keys. The escape menu's save-game name and the in-game chat both ask
for text with a world up and the wheel owning the left trigger, and a
keyboard you cannot see or click is worse than none.

### The address field

No address field exists in any of the game's screens, because a LAN never
needed one, and the screen definitions live inside `Homeworld.big`, which
belongs to the player and cannot be edited. So the VR layer draws its own,
on the lobby (`LChannel_Chat`), in the gap the screen leaves in its
right-hand button column between JOIN GAME (ends at y=192) and CHANGE COLORS
(starts at y=377). It takes keys locally rather than pushing them at the
engine, and commits through `lanAddRemote`; the host's game then appears in
the list alongside the LAN ones.

**Both connection buttons now open these screens.** Internet used to open the
WON login, which hands a name and password to `authAuthenticate` — an empty
stub, and it could not be anything else, since the service has not existed
since 2004. The button named after the thing this port can do was the one that
led nowhere. There is only one transport, and a game beyond the subnet is
reached by naming its host, which is a field on the lobby rather than a
different set of screens. The WON screens are still there and simply
unreachable from the connection screen.

Front-end screens are laid out in a **640x480 box centred in the window**
(`feResRepositionCentredX`), which is where those coordinates come from and
what makes the side bands free real estate on a wide panel. The layout was
checked against the real atom rectangles pulled out of the `.fib` at
1032x552, 1024x768 and 640x480 — worth redoing rather than eyeballing if
either the panel or the field moves.

`MultiplayerHost` in the config stays as an override, since it is how the
desktop and any automated test join without a headset. The field seeds itself
from it and writes back to it, so an address typed once survives the session.

## Running a WAN test

The host forwards **TCP 10500** and **UDP 10600** to its machine, and opens
them locally if a firewall is running (`ufw allow 10500/tcp`,
`ufw allow 10600/udp` — ufw being active was worth half an hour on its own).
Nobody else configures anything: the joiner types the host's public address
and the host learns the joiner from the first packet it sends.

The two machines must be on genuinely different networks. Two boxes on one
subnet do not test this even with forwarding in place, because the address
each publishes is directly reachable and none of the substitution engages —
and most routers do not hairpin, so aiming at the public address from inside
fails for reasons that have nothing to do with the game. A phone hotspot is
the cheapest second network.

**Plug the headset in by USB before moving it off Wi-Fi.** Wireless adb dies
with the network, the log buffer rolls in a couple of minutes, and the first
crash of the session was lost that way — reconnecting afterwards found a
buffer that no longer went back far enough.

## What to do next

1. **Play a match to completion** and watch for a sync error. This is now the
   last real unknown, and it is M5 in the plan: VR-issued orders, and
   particularly a drawn flight path, are the one feature that touches the
   command layer unusually.
2. **Two players over the internet is the limit**, and both ends must be on
   different private ranges. Lifting either needs peers to exchange identity
   on connect. `PlayerJoinInfo` now carries an address, so the precedent for
   putting identity in the payload rather than inferring it exists — the
   in-game path would need the same treatment.
3. **The address field writes `MultiplayerHost` back to the config**, so a
   machine that has ever typed an address seeds a remote on every launch
   after. That arms `lanRouteTo`'s single-remote substitution in games that
   have nothing to do with the internet. Harmless as it stands, since a LAN
   peer's address is local and is left alone, but it makes behaviour depend
   on what was typed months ago and is worth scoping to the session.
4. **Player name.** The headset joins as `unnamed_player` unless the player
   types one, which they now can. Prefilling `utyName` the way
   `gpSuggestSaveName` fills a save name would still save them the trouble.

## Seen once, not chased

A SIGSEGV on the SDL audio thread, 2026-07-29, starting a game shortly after
backing out of the multiplayer screens:

```
#04 isoundmixerprocess+732   libmain.so
#05 soundfeedercb+240        libmain.so
#06 libSDL2.so               (audio callback)
```

`code=2` is SEGV_ACCERR, so it is a bad write rather than a null read. The
mixer runs on SDL's callback thread while everything else in this engine is
single threaded, and the crash lands at the point a level load tears the
sound system down and builds it again - which is the shape of a teardown race
rather than anything about the level.

Not a regression from the VR or multiplayer work: the native library was
byte-identical to the one that had just played a saved game through. Recorded
because it happened once with a backtrace in hand, and a second sighting is
worth more than a first. Reproducing it probably means going in and out of the
multiplayer screens a few times before starting a game.

## Known gaps

- Lobby structs (`CaptainGameInfo`, `Address`) go over the wire as raw
  `memcpy` and contain `unsigned long` and `wchar_t`, so Quest and Linux
  interoperate and Windows does not. In-game packets are fine: fixed-width
  types throughout.
- A star topology cannot survive losing the captain, because captaincy
  transfer needs peers to reach each other with no captain to relay through.
  Over the internet, the host leaving ends the game unless every player is
  reachable.
- Everyone runs at the pace of the slowest peer. Inherent to lockstep, not
  something the transport can improve.
- The demo library has multiplayer disabled at compile time
  (`MultiplayerGame.c:1855`), so only `libmain.so` needs any of this.

## Working practice

**Do not `git push` without being asked.** Committing as work lands is wanted;
publishing is the user's call and they will say when. Stated 2026-07-28.
