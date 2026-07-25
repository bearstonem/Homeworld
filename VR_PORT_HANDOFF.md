# Quest 3 VR port — working notes

State as of commit `d3f4f50` on `master-tenhauser` (fork
`github.com/bearstonem/Homeworld`, upstream `GardensOfKadesh/Homeworld`).
Everything described here is committed and pushed; the working tree is clean.

## Build and deploy loop

```sh
cd /home/berkybear/Homeworld          # shell cwd persists - a stale cd into
                                      # android/project deploys the wrong APK
ninja -C build.android-vr
cp build.android-vr/libmain.so \
   android/project/app/src/vr/jniLibs/arm64-v8a/libmain.so
cd android/project && ./gradlew assembleVrDebug && cd ../..
adb -s 192.168.1.92:44159 install -r \
   android/project/app/build/outputs/apk/vr/debug/app-vr-debug.apk
```

Then relaunch — **and wait for the old process to actually die first**:

```sh
adb -s 192.168.1.92:44159 shell am force-stop org.gardensofkadesh.homeworld
# confirm: pidof must come back empty before starting again
adb -s 192.168.1.92:44159 shell pidof org.gardensofkadesh.homeworld
adb -s 192.168.1.92:44159 logcat -c
adb -s 192.168.1.92:44159 shell am start -n \
   org.gardensofkadesh.homeworld/org.libsdl.app.SDLActivity
```

Chaining force-stop straight into `am start` intermittently launches the game
with **no VR at all**: only one app may hold an OpenXR session, so if the old
process still holds it, `vrInit` fails and the game falls back to flat mono
rendering. The tell is a log with `pass=mono` frames and no
`VR: OpenXR initialized` line.

## Device facts worth not rediscovering

- Wireless adb endpoint `192.168.1.92:44159`. The port changes each time
  wireless debugging is re-enabled.
- **The log tag is `SDL/APP`, not `SDL`.** Capture with
  `adb logcat -v time "SDL/APP:V" VrApi:I "*:S"`. `VrApi:I` gives the
  per-second frame report (`FPS`, `Stale`, `App` ms, `LCnt` layer count),
  which is the cheapest way to confirm layers are composing and to spot a
  performance regression.
- The headset must be **awake** or the session never reaches
  `XR_SESSION_STATE_FOCUSED` and a launch appears to hang.
- `adb screencap` cannot capture OpenXR quad or projection layers. The user's
  eyes are the only ground truth for anything in a layer.
- **Two coordinate spaces that do not match, and not by a uniform factor.**
  The framebuffer is 4128x2208 but `prim2d` and the font system draw in the
  game's logical UI resolution, **1024x768** — measured from the card
  swapchain sizes the code derives, not assumed. Anything mixing
  `vr.width`-space pixels with prim2d will be ~4x out horizontally; that is
  what made the panel reticle land off-screen.
  Worse, the two spaces have different aspect ratios: 1024x768 is 4:3 while
  the framebuffer is ~1.87:1, so the scale factors differ — **4.03
  horizontally against 2.87 vertically**. Everything prim2d and the font
  system draw is therefore stretched about 1.4x horizontally, which makes
  circles into ellipses and text too wide. The stereo world escapes this
  because `vrEyeProjection` substitutes the true per-eye headset FOV.
- The build sets `-Wno-implicit-function-declaration`, so a missing header
  compiles silently. Worth re-checking touched files:
  ```sh
  # take the file's command out of build.android-vr/compile_commands.json and
  # swap -Wno-implicit-function-declaration for -Werror=...
  ```
  That check has caught three real latent breakages so far (`vrActive`,
  `mrSetTheFormation`, and a wheel helper).

## Engine seams the VR layer depends on

These are non-obvious and cost real time to find:

- **`rndCameraMatrix` cannot be sampled at `vrFrame` time.** `svShipViewRender`
  (the spinning ship preview in the Construction Manager) overwrites it every
  frame, the sensors manager does too, and every full-screen manager clears
  `mrRenderMainScreen`, which stops the mono main-view render that is the only
  thing refreshing it. `rndMainViewRenderFunction` now pushes the pure matrix
  in via `vrWorldCaptureGameCamera` instead.
- **`vrFrame` re-enters itself.** `mrBuildShips` → `cmConstructionBegin` →
  `rndClear()` → `rndFlush()` → `vrFrame()`, three times, while
  `xrBeginFrame` is outstanding. Guarded; do not remove the guard.
- **Never synthesize keystrokes for commands.** `mrKeyPress` runs mappable keys
  through `kbCheckBindings`, which returns 0 for a key the player has unbound,
  so the command silently vanishes. Call the `mr*` callbacks with
  `(NULL, NULL)` — they all guard `atom != NULL` — or `clWrap*` directly.
  Never bare `cl*`: `CommandWrap.c` is what marshals multiplayer packets and
  recordings.
- **The camera is focus-locked by design.** `NewSetFocusPoint` recomputes
  `remembercam.lookatpoint` from the focus bounding box every tick, so panning
  fights it every frame. There is deliberately no pan; see
  `documentation/vr-interaction-design.md`.
- Camera mutations must touch **both** `remembercam` and `actualcamera` and set
  `UserControlled`, or `CameraChase` tweens them away.
- `vecDivideByScalar(vec, k, tmp)` assigns `1.0f / k` into its **third**
  argument. Passing an integer truncates the reciprocal to zero. This silently
  broke the flight-path follower for any group of two or more.

## Current VR feature set

Documented for players in `android/README.md`; designed in
`documentation/vr-interaction-design.md`.

Stereo world at 72 fps, controller rays with 3D collision-sphere picking,
selection (click / additive / by-type / sweep brush), smart orders, orders
placed freely in 3D via a ray-depth cursor, freehand Catmull-Rom flight paths
flown leg by leg, full-screen managers on a comfortable head-relative panel
with a guaranteed escape, three wrist cards (controls reference, fleet status,
command wheel), and true physical ship scale (N-LIPS disabled).

## Outstanding

- **Not yet validated on device by the user**: the redrawn command wheel and
  its deadzone/dwell/legibility constants, the 3D depth cursor, hologram
  recentring, and multi-ship flight paths since the centroid fix.
- **Known open bug**: zoomed in close to the fleet, move orders only reach a
  short distance even though the ray points further out. The depth cursor is
  seeded from the fleet's depth along the ray, which is small when the camera
  is close, and the multiplicative rate then takes seconds to push out.
- **Planned, not started** (plan approved, see the session plan file): finish
  the button-map restructure so no input carries more than two meanings —
  right grip still holds the Build chord and fleet cycling, left grip is still
  both Shift and half the pinch — and make `VR_WORLD_SCALE` a runtime control
  (three use sites, already a parameter to `vrWorldFrameBegin`) for a
  tabletop-model to life-size range.
- Optional: the residual "X" transform for pitch and zoom beyond the game
  camera's clamps. `vrWorldCameraOrbit` and `vrWorldCameraZoom` already return
  the refused residual; all call sites discard it. Acceptance test already
  exists — `VRDBG ROUNDTRIP ... dot=` must stay 1.0000000 with X non-identity.
