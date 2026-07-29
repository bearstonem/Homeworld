# Homeworld: Unbound

**[Homeworld] (1999) in room-scale VR, running natively on a Meta Quest 3.**

Not streamed from a PC, and not the flat game on a virtual monitor — the engine
renders the battle in tracked stereo around you, and you command the fleet by
pointing at it.

Homeworld's own fiction was already describing this. You play Karan S'jet, a
scientist who had herself surgically integrated into the Mothership's core and
who perceives the fleet through a neural link rather than through a screen.
Every one of the game's interface conventions is a compromise made for a mouse
and a monitor. This port removes the compromise rather than adding a gimmick.

The name comes from the Bentusi, the trader race who gave up planets to live in
the void — *the Unbound*. It cuts the other way too.

## What VR actually changes

- **Orders are placed in real 3D.** Homeworld's mouse "pie plate" exists only
  because a mouse has two degrees of freedom; a tracked controller has six. The
  destination rides your aim ray at a cursor depth you push in and out with the
  stick, so there is no projection plane and therefore no viewing angle at
  which placement degenerates. A shadow ring, a vertical line and a tether back
  to the fleet keep the depth readable.
- **Routes are drawn freehand, and flown as curves.** Sweep the trigger during
  a move preview and the stroke is smoothed into a centripetal Catmull-Rom
  spline. It can climb and dive, because depth is live while you draw.
  Homeworld has no waypoint order — `MoveCommand` is one heading and one
  destination — so the curve is flown by sliding a single destination along it,
  far enough ahead of the fleet to be worth full throttle, and *steering* the
  existing move order rather than re-issuing it. Re-issuing runs
  `InitShipsForAI`, which zeroes `aistate` and drops every hull back into
  point-in-direction: a brake and a pivot at every waypoint. The route stays
  drawn while it is flown, dimming behind the fleet.
- **Attacks are painted.** Hold B and the ray turns hostile; let go and it
  attacks what you are pointing at. Pull the trigger during the hold and sweep,
  and every hostile the brush crosses joins a list that goes out as one
  `clWrapAttack` on release.
- **Ships are their real size.** Homeworld inflates small craft in proportion to
  camera depth (N-LIPS) so they stay visible on a monitor. Nothing cancels that
  through a fixed headset projection, so it is off — a Scout is a speck beside
  a Destroyer, as it should be.
- **The two hands have distinct jobs.** Left is the commander's — wheel, undo,
  orbit, panel. Right is the pointer — select, order, zoom, navigate. No input
  carries more than two meanings.
- **A command wheel carries the rest.** Six buttons cannot hold Homeworld's
  twenty-odd verbs. Formations, tactics, hotkey groups, the Build/Launch/
  Research managers, hyperspace, special abilities, undo and the game's own
  menu live on a radial at the left wrist. It mirrors the game's *own* context
  menu data, so entries the selection cannot perform dim out and the active
  formation and tactic are marked — and it calls the engine's menu callbacks
  rather than synthesizing keystrokes, which would break for any player who
  rebound a key.
- **Sensors is a place, not a screen.** The strategic map is where long-range
  moves and hyperspace happen, so it is navigable in stereo: orbit it, zoom it,
  band-box inside it. When a *mission* opens it to show you something, the
  beams and controller gizmos hide until it finishes rather than swaying over
  the briefing.
- **The hologram is yours to arrange.** The wheel's Closer and Further wedges
  scale the whole battle between a tabletop model you lean over and something
  you stand inside; a left stick click recentres it in front of you. Both are
  wedges you can hold, so it is one gesture rather than a two-handed one.

## Controls

Left hand is the **commander**; right hand is the **pointer**.

| | Left (commander) | Right (pointer) |
|---|---|---|
| Trigger | Hold: command wheel — stick picks, release runs | Select · sweep-select · order the thing you point at · paint targets or draw a route during a held order |
| Grip | Additive select (Shift) | Navigate: +stick X cycles fleet |
| Stick | Orbit camera | Y: zoom · order depth |
| Stick click | Focus selection + recentre hologram | Toggle Sensors |
| A / X | Undo | Hold: preview smart order, release to commit |
| B / Y | Cancel · close manager · wheel back · show/hide panel | Cancel · close manager · **hold to attack** |

The trigger both selects and issues the default order, told apart by what is
under it, exactly as Homeworld's left mouse button does: your own ship selects,
an enemy attacks, an asteroid harvests, a derelict salvages. Ray colour shows
which, and a dim pulse means the order is not legal for that selection.

Two of those buttons are modal, and the trigger is what elaborates them:

- **Hold A** to aim an order before committing it. Over empty space that is a
  move, and sweeping the trigger during it draws the route. Over a target it is
  the smart order, and sweeping the trigger collects more of them.
- **Hold B** to attack, whatever the ray happens to be over — no smart-order
  fallback, so pointing at a rock is a no-op rather than a harvest order. Sweep
  the trigger to build a target list. Y on the other hand throws either away.

Cancelling always takes priority: while anything is being composed, B and Y
abandon it rather than doing their own job. The game's own Escape menu is a
**Menu** wedge on the command wheel, since B is the attack button now.

A control reference rides beside the left wrist and a live fleet readout rides
the right, so none of the above has to be memorised.

Full detail, including the multi-target attack grammar and the sweep brush, is
in [`android/README.md`](/android/README.md#vr-build-openxr-theater-mode); the
reasoning behind the scheme is in
[`documentation/vr-interaction-design.md`](/documentation/vr-interaction-design.md).

## Status

Playable and actively worked on. Stereo world at a locked 72 fps on Quest 3,
with selection, orders, flight paths, all the managers, and the command wheel
in place.

Rough edges worth knowing before you build it:

- Tested on **Quest 3 only**, though the manifest also lists Quest 2 and Pro.
- The world is still rendered three times a frame (mono pass plus two eyes) and
  the stereo passes render into the window rather than their own FBOs, which is
  what currently blocks MSAA. There is headroom; it has not been spent yet.
- Multiplayer is untouched by the VR work and unexercised in it.

## Building and installing

The VR build is an OpenXR Android app: the engine's desktop GL 1.x path
compiles against [gl4es] on an ES 3.0 context, the world is drawn in tracked
stereo, and the 2D managers are presented on OpenXR quad layers
(see [`src/SDL/vr.c`](/src/SDL/vr.c)).

Step-by-step instructions — SDL2, gl4es and the OpenXR loader for arm64, the
meson cross build, and the gradle flavours — are in
[`android/README.md`](/android/README.md). In short:

One APK plays both campaigns, so the build produces both engines: `HW_GAME_DEMO`
picks the `.big` file, the music and speech filenames and the mission sequence
itself, the last of them as a compile-time array, so it cannot be a runtime
switch. Two build trees, two libraries, one package.

```sh
XC=android/aarch64-android.meson-cross-build-definition.txt
OPTS="--buildtype=release -Db_sanitize=none -Dvr=true -Dmovies=false"
meson setup --cross-file $XC $OPTS -Ddemo=false build.android-vr
meson setup --cross-file $XC $OPTS -Ddemo=true  build.android-vr-demo
meson compile -C build.android-vr
meson compile -C build.android-vr-demo

JNI=android/project/app/src/vr/jniLibs/arm64-v8a
cp build.android-vr/libmain.so      $JNI/libmain.so
cp build.android-vr-demo/libmain.so $JNI/libmainDemo.so
cd android/project && ./gradlew assembleVrDebug
adb install -r app/build/outputs/apk/vr/debug/app-vr-debug.apk
```

`install.py` at the repository root does all of that, plus finding your game
data and copying it across. The same tree still builds the flat 2D Android app
(`assembleFlatDebug`) and the desktop game ([Linux](/Linux/BUILD.md),
[Mac](/Mac/BUILD.md)).

### Game data

**No commercial game assets are distributed here.** The freely redistributable
1.05 demo assets ride inside the VR APK and are unpacked on first run, so it is
playable as installed. For the full sixteen-mission campaign, put your own
Homeworld data files in
`/sdcard/Android/data/org.homeworldunbound.game/files` — the next launch
picks the full engine, and clears the demo files that would otherwise shadow
it. Settings, saves and screenshots are written to the same place.

## Lineage

This is a Quest VR fork of [Gardens of Kadesh][gok], which is itself the
continuation of a long chain of work by other people:

- **1999** — [Relic Entertainment] releases [Homeworld].
- **2003** — Relic [releases the source code][source code release] alongside
  Homeworld 2. A community effort ports it to [SDL] as "Homeworld SDL",
  reaching Mac, Linux and eventually browsers.
- **2012** — Homeworld SDL is [saved to GitHub][tim detering's fork]; by 2016
  the effort has [gone dark][homeworldsdl.org].
- **2019** — Volunteers pick it up from that export and rename it *Gardens of
  Kadesh*, after [Mission 07][Mission 07].
- **2026** — This fork adds the OpenXR VR port and a control scheme designed
  for 3D rather than translated from a mouse.

The VR work is confined almost entirely to `src/SDL/vr.c` (OpenXR, layers,
input) and `src/SDL/vrworld.c` (the bridge to the game's own command layer),
so it stays mergeable with upstream. Engine-side changes are small and mostly
guarded by `HW_ENABLE_VR`.

## License

**This project is not open source.** The 2003 source release came with
restrictions that are still in force — see the
[license agreement](/documentation/contributors/license-agreement.md#this-project-is-not-open-source)
before doing anything with this code. Contribution guidance is in
[CONTRIBUTING.md](/CONTRIBUTING.md), and the full upstream documentation is at
[`documentation/`](/documentation/README.md).

Homeworld is a trademark of its respective owners. This is an unaffiliated fan
project.

[Homeworld]: https://en.wikipedia.org/wiki/Homeworld
[Relic Entertainment]: https://www.relic.com/
[source code release]: https://web.archive.org/web/20051101125632/http://www.insidemacgames.com:80/news/story.php?ArticleID=8516
[SDL]: https://en.wikipedia.org/wiki/Simple_DirectMedia_Layer
[tim detering's fork]: https://github.com/timdetering/HomeworldSDL
[homeworldsdl.org]: https://web.archive.org/web/20160912032212/http://www.homeworldsdl.org/
[Mission 07]: https://homeworld.fandom.com/wiki/HW_Campaign:_Gardens_of_Kadesh
[gok]: https://gitlab.com/gardens-of-kadesh/gardens-of-kadesh
[gl4es]: https://github.com/ptitSeb/gl4es
