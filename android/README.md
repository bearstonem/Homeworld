# Building GoK for Android (and Meta Quest)

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

- Aim at one of your ships and pull either trigger to select it. Hold the
  left grip for additive/toggle selection. A quick second trigger pull
  selects visible ships of the same type.
- Pull a trigger on empty space, sweep the ray across ships, then release
  to commit the group selection. The sweep is a brush, not a hairline: its
  capture radius widens with distance the way a screen-space band box does
  with depth, and it takes every ship the beam passes over.
- Hold A/X to preview a context order and release to issue it: attack an
  enemy, harvest a resource, dock at one of your ships, or move when aimed
  at empty space. The destination is placed freely in 3D: it rides your aim
  ray at a cursor depth that starts level with your ships, and right-stick Y
  moves it toward or away from you. There is no projection plane, so unlike
  the mouse pie plate there is no viewing angle at which placement breaks
  down. A shadow ring on the fleet's plane, a vertical line and a line back
  to the fleet make the depth readable, and the right wrist card shows it as
  a number. B/Y cancels.
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
  the wrong height. Right-stick click opens Sensors, and B/Y closes it again.
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
- Right-grip+B/Y opens the Build Manager, and an unmodified B/Y closes any
  open manager - Construction, Launch, Research, Trade or Sensors. Sensors is
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
