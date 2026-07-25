# Building GoK for Android and Meta Quest ("Homeworld: Unbound")

The VR flavour ships under the name **Homeworld: Unbound** — that is the label
you will see in the Quest library. The name lives in
`app/src/vr/res/values/strings.xml`, so the flat Android build keeps the plain
"Homeworld" label. The package id stays `org.gardensofkadesh.homeworld`
deliberately: it is also the asset path under
`/sdcard/Android/data/`, and changing it would orphan an existing install, its
settings and its assets.

The Android port reuses the OpenGL ES 1.1 renderer (`-Dgles=true`).
SDL's Java activity (`org.libsdl.app.SDLActivity`) loads the game as
`libmain.so` and calls its `SDL_main` entry point.

## Prerequisites

- Android SDK with platform 34 and build-tools
- Android NDK (tested with r26)
- JDK 17
- The usual GoK build environment for the build machine (meson, flex,
  bison, ...) — `nix develop ./Linux` provides it

## 1. Build SDL2 for Android

Download and unpack an SDL2 release (tested with 2.32.8) to `android/SDL`,
then:

```sh
cd android
cmake -S SDL -B build-sdl-arm64 -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_HOME/ndk/<version>/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PWD/sdl-prefix-arm64
cmake --build build-sdl-arm64 -j$(nproc)
cmake --install build-sdl-arm64
```

## 2. Cross-compile the game

Adjust the NDK paths in `android/aarch64-android.meson-cross-build-definition.txt`
to your installation, then from the repository root:

```sh
meson setup --cross-file android/aarch64-android.meson-cross-build-definition.txt \
    --buildtype=release -Db_sanitize=none -Dgles=true -Dmovies=false -Ddemo=true \
    build.android
meson compile -C build.android
```

This produces `build.android/libmain.so`.

> The cross file points pkg-config at `android/sdl-prefix-arm64` so the
> build machine's own SDL2 can not leak into the cross build.

## 3. Package the APK

The gradle project in `android/project` packages prebuilt native libraries
(no NDK build happens inside gradle):

```sh
cp sdl-prefix-arm64/lib/libSDL2.so ../build.android/libmain.so \
    project/app/src/main/jniLibs/arm64-v8a/
cd project
./gradlew assembleDebug
```

The APK lands in `project/app/build/outputs/apk/debug/`.

## 4. Install and provide game data

```sh
adb install app/build/outputs/apk/debug/app-debug.apk
```

The game runs out of its external-storage directory and expects the game
data (.big files etc.) there. For the demo assets:

```sh
adb shell mkdir -p /sdcard/Android/data/org.gardensofkadesh.homeworld/files
adb push subprojects/demo-assets-1.05/assets/. \
    /sdcard/Android/data/org.gardensofkadesh.homeworld/files/
```

(For the full game, build with `-Ddemo=false` and push the original
Homeworld data files instead.)

Settings, saves and screenshots are written to the same directory.

### Running the full game instead of the demo

If you own the Homeworld Remastered Collection, its `Homeworld1Classic/Data`
directory is the original 1999 game and this port reads it directly —
`BigFile.c` already recognises the Remastered packaging of `homeworld.big`
(it detects the TOC by its 13887-file count and reads the wider entry struct).
That gets you the full 16-mission campaign instead of the demo's handful.

Build with `-Ddemo=false`, then push, **renaming as you go** — the binary opens
`Homeworld.big`, `HW_Music.wxd` and `HW_comp.vce`, and note the lowercase `c`
in the last one, which does not match the shipped `HW_Comp.vce`:

```sh
SRC=".../steamapps/common/Homeworld/Homeworld1Classic/Data"
DST=/sdcard/Android/data/org.gardensofkadesh.homeworld/files
adb push "$SRC/homeworld.big" $DST/Homeworld.big
adb push "$SRC/HW_Music.wxd"  $DST/HW_Music.wxd
adb push "$SRC/HW_Comp.vce"   $DST/HW_comp.vce
```

**Delete the demo assets first**, `Update.big` above all. It sits *first* in
`bigFilePrecedence`, so leaving the demo's copy in place silently overrides
files from the full `Homeworld.big` with demo content:

```sh
adb shell "cd $DST && rm -f HomeworldDL.big DL_Music.wxd DL_demo.vce Update.big"
```

The movies are `.bik` and are not needed unless you build with `-Dmovies=true`.

Nothing from the Remastered Collection is redistributed here; this reads the
copy you already own. `tools/sga_extract.py` unpacks the Remastered
`_ARCHIVE` (Relic SGA v2) `.big` files for the same reason.

> **Do not restore a backup of that directory with `adb push` alone.** Push
> writes files as the *shell* user (uid 2000, mode 644); the game runs as its
> own uid and is only in the group, so it can read them but not write them.
> The four asset files above are read-only and so are fine, but `Homeworld.cfg`
> and `SavedGames` are files the game creates and rewrites — restoring those
> from a backup makes the game crash at startup, before it logs anything.
> Push only the assets and let the game create the rest; to restore saves,
> `chmod 666` them afterwards.

## Meta Quest

The Quest runs standard Android APKs as flat 2D panel apps: enable
developer mode, connect via adb, then install/push as above.

## VR build (OpenXR theater mode)

The `-Dvr=true` meson option builds an immersive Quest variant: the game
compiles its desktop GL 1.x path against [gl4es] (GL over ES2) on an
ES 3.0 context. The game world is rendered in tracked stereo, while menus
and managers are presented through an OpenXR quad layer (see
`src/SDL/vr.c`). Bluetooth mouse/keyboard input remains available alongside
the Touch controllers.

Touch controls in the 3D world:

- Aim at one of your ships and pull the right trigger to select it. Hold the
  left grip for additive/toggle selection. A quick second pull selects visible
  ships of the same type. (The left trigger opens the command wheel instead,
  see below.)
- Pull the right trigger on empty space, sweep the ray across ships, then
  release to commit the group selection. The sweep is a brush, not a hairline:
  its capture radius widens with distance the way a screen-space band box does
  with depth, and it takes every ship the beam passes over. While sweeping,
  rings down the beam show the capture cone at the fleet's depth and a trail
  shows the region already swept - drawn as a cone rather than a box, because
  a cone is the shape that actually decides which ships are caught.
- The trigger both selects and issues the default order, told apart by what is
  under it, exactly as Homeworld's left mouse button does in `mrObjectClick`:
  one of your own ships selects it, while a hostile ship, a resource or a
  derelict orders the current selection to act on it. So point at an asteroid
  with harvesters selected and pull to harvest, point at a derelict with a
  Salvage Corvette selected and pull to salvage, point at an enemy with
  warships selected and pull to attack. Ray colour shows which it will be, and
  a dim pulse means the order is not legal for that selection. Empty space
  still starts a sweep selection, and only empty space clears the selection.
- Hold A/X to preview a context order and release to issue it: the same attack,
  harvest, dock and salvage verbs, plus move when aimed at empty space. A/X is
  the path for orders needing a position previewed in 3D; the trigger is the
  fast path for orders that only need a target.
- A move destination is placed freely in 3D. It rides your aim ray at a cursor
  depth that starts level with your ships, snaps to whatever you point at, and
  right-stick Y moves it toward or away from you, accelerating the longer you
  hold. There is no projection plane, so unlike the mouse pie plate there is no
  viewing angle at which placement breaks down. A shadow ring on the fleet's
  plane, a vertical line and a line back to the fleet make the depth readable,
  the beam ends exactly at the destination, and the right wrist card shows the
  depth as a number. B/Y cancels.
- To attack several things at once, hold A on one enemy and then, still
  holding it, pull the trigger and sweep across the others. Each one picked up
  is ringed in red, and releasing A sends a single attack order against all of
  them - the same order the desktop's ctrl-drag band box issues. It is the same
  grammar as drawing a route: A previews an order, and sweeping the trigger
  during that preview elaborates it.
- To fly a route rather than a straight line, hold the trigger during a move
  preview and sweep: the swept points are smoothed into a Catmull-Rom spline
  and resampled by arc length into evenly spaced waypoints, drawn as a curve
  with a ring on each. Releasing A/X sends the selection along them, ring by
  ring. Right-stick Y still sets depth while drawing, so a route is a genuine
  space curve that can climb and dive. Homeworld has no waypoint order of its
  own, so each leg is issued as a plain move once the previous one is reached.
- Ray colors show the pending action; short controller pulses confirm
  target acquisition, selection, and issued orders.
- The left stick orbits and right-stick Y zooms; squeezing both grips at once
  pinches the hologram to zoom and rotate it. Hold the right grip to turn the
  right stick into a fleet traversal control: flick it left or right to step
  the camera through your ships. The grip is required so that cycling cannot
  fire by accident while zooming, and it suppresses zoom for as long as it is
  held. Left-stick click means "bring me back to my fleet": it focuses the
  camera on your selection and re-places the hologram in front of you, which
  is also how you recover if you have walked away from it or it ended up at
  the wrong height. Right-stick click opens Sensors, and B closes it again.
- The Sensors Manager is a 3D strategic map rather than a flat screen, and it
  is where long-range moves and hyperspace are issued - the game opens it by
  itself for a move beyond its maximum range - so it is navigable rather than
  merely visible. While it is open the left stick orbits its camera and
  right-stick Y zooms it, driving that view's own camera directly. Selection
  works as it does on any panel: drag the ray to band-box ships, exactly as
  dragging a mouse would.
- Ships render at true physical scale. Homeworld's N-LIPS scaling, which
  inflates a ship in proportion to its camera depth to keep small craft
  readable on a flat monitor, is disabled in VR: nothing cancels it through
  the fixed headset projection, so it only made ships swell as the fleet was
  pushed away.
- Raise the left controller to use the wrist panel. The nearer of the panel
  and 3D target receives input, and the pointer remains captured until the
  button is released. Left-grip+B/Y hides or shows the wrist panel.
- A control reference card rides beside the left wrist panel, and a fleet
  status readout (resource units, ship and selection counts, sensors level,
  class totals) rides the right wrist. Both are their own compositor layers
  drawn from live game state, and both follow the wrist panel's visibility.
- Six buttons cannot carry Homeworld's twenty-odd verbs, so the rest live on a
  command wheel: hold the left trigger and a radial appears at that hand, the
  left stick picks a wedge, and releasing the trigger runs it. Dwelling on a
  category wedge descends into it; B/Y steps back up a level, then closes.
  The View wedge also scales the hologram: hold Closer or Further and the whole
  battle draws toward a tabletop model you can lean over, or out to something
  you stand inside. Scale does not change anything's apparent size - angular
  size is hull over distance, both in game units - it changes how far away the
  fleet physically feels and therefore how strong the stereo depth cue is. It
  scales about whatever the camera is looking at, so the fleet does not slide
  past you as it changes.
  Formations, tactics, hotkey groups, halt, harvest, dock, special ability,
  kamikaze, retire, scuttle, the Build/Launch/Research managers, hyperspace,
  focus and undo are all reachable there. Entries the current selection cannot
  perform are dimmed rather than hidden - the wedge positions stay fixed so
  the wheel becomes a flick once learned - and the formation and tactic
  already in effect are marked. In the hotkey group wheel, holding the left
  grip turns recall into store, and each wedge shows its ship count.
  The wheel takes its position from the left wrist but its orientation from
  your head, so stick direction always maps to the same wedge no matter how
  your wrist is turned.
- Wheel entries call the game's own right-click-menu callbacks rather than
  synthesizing keystrokes, because `mrKeyPress` routes mappable keys through
  `kbCheckBindings`, which discards keys the player has unbound.
- The command wheel stays available while a manager is open, which is how you
  move between them. Homeworld's own taskbar cannot do that job: opening a
  manager detaches the taskbar regions from the region tree and modal
  front-end screens stop events propagating to it, so those buttons are inert
  by the original game's design, not by omission here.
- The two hands have distinct jobs. The **left** is the commander's: the
  trigger opens the command wheel, X is undo, Y shows and hides the wrist
  panel and its cards, the stick orbits and its click focuses the selection
  and recentres the hologram. The **right** is the pointer: the trigger
  selects and issues default orders, A previews and commits a smart order, B
  cancels or closes, the stick zooms and sets order depth, and the grip means
  navigate and nothing else - flick the right stick under it to cycle the
  fleet, or squeeze both grips to pinch the hologram. The right grip used to
  carry four unrelated meanings; Build and the dock chord moved to the wheel
  and the smart order respectively.
- An unmodified B closes any - Construction, Launch, Research, Trade or Sensors. Sensors is
  full-screen and suppresses the main view like the rest, so it is treated as
  a manager too rather than being squeezed onto the wrist panel. Manager
  screens are dense 2D UI, so they are presented as a
  panel a fixed readable distance in front of you, leaning toward the ship
  they concern but clamped into a cone around your gaze. The panel is the
  only way back out of a manager, so input only becomes modal once the
  compositor has accepted a frame containing it, and a manager that never
  becomes visible is closed automatically.

[gl4es]: https://github.com/ptitSeb/gl4es

Build the two extra native dependencies once:

```sh
cd android
git clone --depth 1 https://github.com/ptitSeb/gl4es.git
cmake -S gl4es -B build-gl4es-arm64 -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_HOME/ndk/<version>/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 \
    -DNOX11=ON -DDEFAULT_ES=2 -DSTATICLIB=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-gl4es-arm64 -j$(nproc)   # produces gl4es/lib/libGL.a

git clone --depth 1 https://github.com/KhronosGroup/OpenXR-SDK.git
cmake -S OpenXR-SDK -B build-openxr-arm64 -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_HOME/ndk/<version>/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DCMAKE_BUILD_TYPE=Release
cmake --build build-openxr-arm64 -j$(nproc)  # produces libopenxr_loader.so
```

Then from the repository root:

```sh
meson setup --cross-file android/aarch64-android.meson-cross-build-definition.txt \
    --buildtype=release -Db_sanitize=none -Dvr=true -Dmovies=false -Ddemo=true \
    build.android-vr
meson compile -C build.android-vr
```

The gradle project has `flat` and `vr` product flavors (same application
id, so they share the sideloaded game data but cannot be installed at
the same time):

```sh
cp build.android-vr/libmain.so android/sdl-prefix-arm64/lib/libSDL2.so \
    android/build-openxr-arm64/src/loader/libopenxr_loader.so \
    android/project/app/src/vr/jniLibs/arm64-v8a/
cd android/project
./gradlew assembleVrDebug     # or assembleFlatDebug for the 2D app
adb install app/build/outputs/apk/vr/debug/app-vr-debug.apk
```
