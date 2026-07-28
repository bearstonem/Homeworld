# Prebuilt APK

Built from `5a96765` plus the working tree, on 2026-07-27.

## One APK, both campaigns

There used to be two, because the demo/full choice is compiled into the
engine: `HW_GAME_DEMO` picks the `.big` file the game opens, the music and
speech filenames, and the mission sequence itself, that last one as a
compile-time array. Installing the wrong one failed at startup with
`Unable to open required .big file`.

It still is compiled in — the APK just carries both builds and chooses at
launch. `HomeworldActivity` looks for `Homeworld.big` in the app's data
directory: present means the full game and `libmain.so`, absent means the demo
and `libmainDemo.so`. The freely redistributable 1.05 demo assets ride inside
the APK and are unpacked on first run, so it plays without anything being
copied to the headset at all.

Switching either way is a matter of what is in that directory. Drop your own
game data in and the next launch is the full campaign; the demo files that
would otherwise shadow it are cleared automatically, `Update.big` included —
it sits first in the engine's `bigFilePrecedence`, so a demo copy left behind
silently overrides full-game content. A retail `Update.big` of a different
size is left alone.

| | Needs | Opens |
|---|---|---|
| Demo | nothing | `HomeworldDL.big`, `DL_Music.wxd`, `DL_Demo.vce` |
| Full game | your own copy of Homeworld | `Homeworld.big`, `HW_Music.wxd`, `HW_comp.vce` |

`install.py` at the repository root installs it, and copies your game data if
you have it. Debug-signed, arm64-v8a, Quest only.

```
4ddfe8762eb78a849c7699767d2d23f7  homeworld-unbound-vr.apk  98M
```

Roughly 65 MB of that is the bundled demo assets and 20 MB the two engine
builds, neither of which is stripped.

## What changed since the last build

- **One APK instead of two**, as above.
- **B is the attack button.** Hold it and let go to attack what you are
  pointing at, or pull the trigger during the hold and sweep to collect
  several targets for a single order. The game's Escape menu moved to a Menu
  wedge on the command wheel, which is where B used to reach it.
- **Drawn flight paths are flown as curves.** The route is followed by one
  destination sliding along it, kept far enough ahead of the fleet to be worth
  full throttle, and the existing move order is steered rather than re-issued —
  so there is no pivot, no engine restart and no flash at every waypoint. The
  path stays drawn while it is flown, dimming behind the fleet.
- **The hands hide during a mission briefing.** When a mission opens the
  Sensors Manager to show you something, the beams and controller gizmos go
  away until it finishes rather than swaying over it.
- **The command wheel's top-level Sensors is now View**, since Sensors is an
  entry on the page it opens.
- **No more two-grip pinch.** It zoomed and orbited the hologram, both of
  which the left stick, the right stick and the wheel's View page already do
  with one hand. The grips now mean one thing each: add to selection on the
  left, step through the fleet on the right.
