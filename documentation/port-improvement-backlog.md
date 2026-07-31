# Port improvement backlog

A review of the port at `50dc7a3`, 2026-07-30. Nothing here is in progress;
this is a list to pick from, ordered by argument rather than by date.

Every claim below was checked against the source rather than carried over from
the other handoff documents, and the file and line references are from that
commit. Where the reasoning in an existing document is contradicted, that is
called out — the older document is the one that is out of date.

The interaction layer is deliberately absent. Every item under "Outstanding"
in `VR_PORT_HANDOFF.md` is validated on device, and the gap now is between
"works when the author drives it" and "survives someone else playing it".
That is what this list is about.

## 1. Multiplayer cannot reach Windows players

`src/Game/MultiplayerGame.h:112-124,248` puts `wchar_t` in `CaptainGameInfo`
and the lobby structs, and those go over the wire as a raw `memcpy`. `wchar_t`
is 2 bytes on Windows and 4 on Linux and Android, so the struct is a different
size and a different layout on the two platforms and neither can read the
other's. `unsigned long` in the same structs has the same problem for a
different reason.

In-game packets are unaffected — fixed-width types throughout — so this is
confined to lobby discovery and joining.

The reason to put this first is not difficulty, it is reach. The surviving
Homeworld community is overwhelmingly on Windows, and today a headset can play
a Linux desktop and nothing else. Serialising those few structs field by field
to fixed-width types is bounded and self-contained, and `net-harness` exercises
it without needing two machines.

Recorded as a known gap in `documentation/vr-multiplayer-handoff.md`; the point
here is that it deserves to be near the top rather than at the bottom.

## 2. Heap exhaustion is silent, and 256MB is a guess

`memAllocFunctionANV` (`src/Game/Memory.c:753`) returns its `newPointer`
unconditionally, and that pointer is NULL when no block is big enough. Every
diagnostic in the function sits behind `MEM_VERBOSE_LEVEL`, which is compiled
out of a distribution build. Callers do not check — `etgEffectCreate`
dereferences the result on the next line.

So exhaustion presents as an unexplained SIGSEGV with no tombstone and nothing
in the crash buffer. It is deterministic after enough play and absent on a
freshly loaded save, which is the only fingerprint there is.

Two lines of always-compiled logging at the NULL return turn that into one log
line. This matters more here than it would upstream because the VR build
*raised* the clamp to 256MB (`MEM_HeapDefaultMax`, `src/Game/Memory.h:124`) to
pay for forced mesh detail and 3x draw distance, and there is currently no way
to learn whether that was enough except from a crash report.

Cheapest large win on this list.

## 3. The sound mixer race, and why it should not wait for a reproduction

`documentation/vr-multiplayer-handoff.md` diagnoses this fully and parks it for
want of a reproduction. That is right for the full teardown rework and wrong
for the parts that are unambiguous.

The race is provable by inspection, not by sighting. There is no
`SDL_LockAudioDevice` anywhere in `smixer.c`, `soundlow.c` or `sstream.c`;
`soundclose()` sets a flag and returns while the audio thread keeps running for
several more buffers, over memory the caller is about to free; and
`soundrestore()` waits by spinning on a global that is not `volatile`, which
the compiler is entitled to hoist out of the loop.

`volatile` on the flag the existing spin-wait depends on is nearly free and
strictly correct, and does not need a reproduction to justify. The
`SDL_LockAudioDevice` rework around teardown and re-init is the part worth
holding until there is something to test a fix against.

## 4. There is no CI

No `.github/workflows` at all. Three configurations have to keep compiling —
`build`, `build.android-vr`, `build.android-vr-demo` — and the build sets
`-Wno-implicit-function-declaration`, so a missing header compiles silently.
`VR_PORT_HANDOFF.md` records three real latent breakages already found that
way, by hand.

A matrix that builds all three on push closes exactly that class, and can run
the `-Werror=implicit-function-declaration` check the handoff currently
describes as a manual chore. Worth doing before the items above rather than
after, because it protects them.

## 5. The world is drawn three times a frame

A mono pass plus two eyes. The mono pass existed because `rndCameraMatrix`
could not be sampled at `vrFrame` time, but that is now pushed in through
`vrWorldCaptureGameCamera` instead, so the original reason may no longer hold.

If it can be skipped under `HW_ENABLE_VR`, that is roughly a third of render
time back — which is what would fund the MSAA work parked on 2026-07-25 for
want of headroom. Verify before assuming: other state may be riding on the
mono pass, and this needs measuring on device rather than reasoning about.

## 6. Smaller items

- **Player name.** The headset joins as `unnamed_player` unless someone types
  one. Prefilling `utyName` the way `gpSuggestSaveName` fills a save name is
  the same trick against a different field, and that one is proven on device.
- **`MultiplayerHost` persists in the config**, so a machine that has ever
  typed an address seeds a remote on every launch afterwards, arming
  `lanRouteTo`'s single-remote substitution in games with nothing to do with
  the internet. Harmless as it stands — a LAN peer's address is local and is
  left alone — but it makes behaviour depend on what was typed months ago.
  Scope it to the session.
- **The relay has no timeout** and is a single point of failure. Losing the
  captain was always fatal over the internet; it is now also fatal to ordinary
  traffic between two clients who could have reached each other directly.

## 7. The overwrite prompt on desktop

`src/Game/GamePick.c:869` tests `fileExists(filename, 0)` to decide whether to
show the overwrite confirmation, but `SaveGame()` opens the same name with
`FF_UserSettingsPath`. Flag `0` sends `filePathPrepend` to `fileOverrideBigPath`
instead.

On the Quest there is no `$HOME`, so the user-settings path and the data path
are the same directory and it happens to work. On desktop they differ
(`~/.homeworld` against the data directory), so the prompt never fires and a
save silently overwrites an existing one. Desktop-only, small.

## Documentation that is now wrong

`VR_PORT_HANDOFF.md` is the file that gets trusted, and two things in it are
out of date:

- **It tells you to `am start`** (the relaunch block under "Build and deploy
  loop"), which `CLAUDE.md` and the `vr-deploy` skill both forbid, because
  launching over adb strands the headset in Meta's loading environment. The
  handoff is the one that is stale.
- **Stale specifics**: "State as of commit `d3f4f50`", and a wireless adb
  endpoint that has moved twice since it was written. The document already
  warns against trusting its own endpoints, which is an argument for removing
  them rather than updating them.

A third — it recommended `chmod 2775` on the data directory — was corrected at
`50dc7a3`, because that is the advice that broke saving on 2026-07-30. See
`android/README.md` for the full account.

## Suggested order

CI first, because it protects everything after it. Then the heap logging, as
the cheapest large win. Then Windows interoperability, as the change that most
widens who can actually play. The `volatile` half of the audio fix can ride
along with any of them.
