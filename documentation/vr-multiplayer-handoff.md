# Multiplayer — working notes

Companion to `documentation/vr-multiplayer-plan.md`, which is the plan. This
is the state of it, the things that cost time to find, and what to do next.

State as of `c6e6385` on `main`. Everything described is committed and pushed.

## Where it got to

**LAN play works.** Verified 2026-07-28 between a Quest 3 and a Linux desktop:
both peers open sockets, find each other by UDP broadcast, appear in each
other's lobby, join over TCP, and load into the same game. Both reached
`Cap:I Am 0`/`Cap:I Am 1 in 2-player game` and `Cap:Transition to state
normal`, so `CommandNetwork.c` and `Captaincy.c` ran for the first time in
this fork and agreed with each other about who was captain.

**Internet play is written but unverified.** Same transport, reached by naming
the host instead of broadcasting. Never tested against a genuinely remote
endpoint — both machines here are on one subnet, so nothing has yet crossed a
router, let alone NAT.

**No sync error has been seen**, but nor has a game been played out. A match
that has just loaded has not had the opportunity to desync. `SYNC_CHECK` is 1
(`Switches.h:14`) so a mismatch reports itself with a frame number; that
remains the acceptance test and it has not been run in anger.

**The keyboard does not exist**, and it now blocks everything else. See
[What to do next](#what-to-do-next).

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
- A game hosted behind a router advertises its *private* address. When that
  address is unreachable and exactly one remote is named, that remote is
  necessarily where the advertisement came from, so it is dialled instead.
  With several named it is ambiguous and deliberately not guessed.

There is a standalone harness for it. It covers startup, address discovery,
broadcast round-trip, accept, a frame split across two writes, two frames in
one read, zero-length payloads, peer drop and restart — everything short of
two-host routing. It is not in the repository; recreate it by stubbing
`HandleTCPMessage`, `titanReceivedLanBroadcastCB` and `dbgMessagef` and
linking `lan.c` directly. Worth doing before touching framing.

## Running the two-machine test

Desktop side needs game data next to the binary. It is not in the repository
and the paths used during the session pointed into a temporary directory, so
recreate them:

```sh
D=/sdcard/Android/data/org.gardensofkadesh.homeworld/files
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

## What to do next

### A keyboard, in VR

Decided 2026-07-28: a player must never edit a file to host or join. That
makes this the critical path, ahead of everything else, because there is
currently no way to enter an address or a name from inside the headset.

The design is de-risked but unwritten. All three unknowns are resolved:

- **Synthesized keys already reach text entry.** `main.c:1791` does
  `keyPressDown(keyLanguageTranslate(pEvent->key.keysym.scancode))`, so the
  engine keys off the **scancode**, and `vrPushKey` already sets it. This is
  how Escape works in VR today. No engine changes are needed.
- **`regKeysFocussed` (`Region.h:174`) is TRUE exactly while a text entry
  holds key capture**, which is when the keyboard should appear.
- **It belongs on the panel, not on a card.** A fourth wrist card would need a
  new swapchain, a new pose and new ray hit-testing. `vr.pointerX/pointerY`
  already holds the controller ray mapped onto the panel in logical UI
  pixels, and `vrPushMouseButton` fires clicks at exactly those coordinates
  (`vr.c:1493`). Draw the key grid as an overlay, hit-test against the
  pointer that is already computed, and push a scancode on trigger instead of
  a mouse click. Roughly a quarter of the code and it reuses paths already
  proven on device.

`MultiplayerHost` in the config stays as an override, since it is how the
desktop and any automated test join without a headset. It must not be the
route a player takes.

### After that

1. **Play a match to completion** and watch for a sync error. This is the
   last real unknown before LAN play is usable, and it is M5 in the plan:
   VR-issued orders, and particularly a drawn flight path, are the one
   feature that touches the command layer unusually.
2. **Test internet play across a real WAN.** Nothing has crossed a router.
   The host forwards TCP 10500 and UDP 10600.
3. **Player name.** The headset joins as `unnamed_player` because `utyName`
   falls back to its default. Prefilling it the way `gpSuggestSaveName` fills
   a save name is the cheap fix; the keyboard is the real one.

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
