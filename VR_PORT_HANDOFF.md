# Homeworld: Unbound — Quest 3 VR port, working notes

The VR build ships as **Homeworld: Unbound** (the Bentusi are "the Unbound";
also unbound from the monitor). Three things carry a name and only two of them
were renamed:

- `app/src/vr/res/values/strings.xml` → `app_name`, the Quest library label.
  It lives in the **vr flavour**, so the flat Android build keeps "Homeworld".
- `HW_WINDOW_TITLE` in `src/SDL/main.h` → the SDL window title, `#ifdef
  HW_ENABLE_VR`. Used by `main.c`'s `windowTitle` and both `SDL_CreateWindow`
  sites in `render.c`.
- **`networkVersion` in `main.c` is deliberately still `"HomeworldSDL"`.** It is
  the multiplayer protocol version string, matched against other clients;
  renaming it would silently make this build unable to play with anything else.

`applicationId` also stays `org.gardensofkadesh.homeworld`: it is the asset path
under `/sdcard/Android/data/`, so changing it orphans the install, its settings
and its assets. The meson project name (`homeworld`) is likewise internal.

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

- Wireless adb endpoint `192.168.1.105:34721` (the IP was `.92` for a long
  time; the port has been 40561, 44159, 45027, 38229). **Both halves move** —
  the port each time wireless debugging is re-enabled, and the IP whenever the
  DHCP lease turns over — and `adb mdns services` can keep advertising a dead
  one, which makes a stale endpoint look authoritative. Read them off the
  headset's own Wireless Debugging screen, not from mDNS and not from this
  file. Two symptoms worth telling apart: "Connection refused" means the right
  host with no adbd (wireless debugging is off, or the port moved), while "No
  route to host" means nothing is at that IP at all — the lease moved. A
  `nmap -sn` sweep of the /24 finds it again.
- **The log tag is `SDL/APP`, not `SDL`.** Capture with
  `adb logcat -v time "SDL/APP:V" VrApi:I "*:S"`. `VrApi:I` gives the
  per-second frame report (`FPS`, `Stale`, `App` ms, `LCnt` layer count),
  which is the cheapest way to confirm layers are composing and to spot a
  performance regression.
- The headset must be **awake** or the session never reaches
  `XR_SESSION_STATE_FOCUSED` and a launch appears to hang.
- **`adb screencap` does capture OpenXR layers** — this note used to say the
  opposite. `adb exec-out screencap -p` returns the composited stereo frame at
  4128x2208: both eyes, lens distortion, passthrough, quad layers and the
  world projection layer. That makes it ground truth for what the player sees,
  and it is measurable — it is how the washed-out lighting below was pinned
  down to a number rather than argued about.
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
- **The Remastered background cubemaps are worse than HW1's own backgrounds
  in VR. Tried, measured, reverted.** The remaster ships each background as
  six 1024x1024 DXT1 cube faces (`tools/rm_backgrounds.py` extracts them, and
  the mapping is exact - the remaster's campaign data maps missionNN to ezNN,
  and HW1 uses the same names for its .btg files). Rendering them as a skybox
  works and looks noticeably worse. Two reasons, only one of them fixable:
  - **1024 per face is not enough for a headset.** A face spans 90 degrees
    and each eye renders ~2064px across roughly that, so every texel is
    magnified about 2x. There is no higher-res source: the `_hq_` variants
    are also 1024. These were authored for a 2015 monitor at ~60 degrees.
  - **A cubemap is flat, and it reads as flat.** HW1's `.btg` is a
    vertex-coloured mesh - smooth interpolated gradients with no texels at
    all - plus separate star points. In stereo that reads as depth and
    atmosphere; a textured cube reads as a picture pasted on a box. This is
    the part that cannot be fixed with a bigger texture.

  If backgrounds are revisited, the direction is to improve what `.btg`
  already does (denser mesh, better star rendering) rather than replace it.
  Two traps worth keeping if the skybox is ever rebuilt: a unit cube is
  entirely inside the near clip plane and silently draws nothing (btgRender
  uses `CAMERA_CLIP_FAR - 500`), and `screenshot.c` already defines
  `STB_IMAGE_IMPLEMENTATION`.
- **The game runs in one fixed heap, and running out of it is a silent
  segfault.** `memAllocFunctionANV` returns NULL when no block is big enough;
  its diagnostic is behind `MEM_VERBOSE_LEVEL`, compiled out in a distribution
  build. Callers do not check - `etgEffectCreate` dereferences the result on
  the very next line. So exhaustion presents as an unexplained SIGSEGV with no
  tombstone, no message, and nothing in the crash buffer. It is deterministic
  after enough play and absent on a freshly loaded save, which is the
  fingerprint to recognise. The heap is sized once at startup from physical
  RAM and then clamped to `MEM_HeapDefaultMax` - 128MB in the original, now
  256MB under `HW_ENABLE_VR`, because forcing full mesh detail and 3x render
  distance asks more of it than the 1999 game ever did. If
  `HWMEM etgEffectCreate: heap exhausted` ever appears, 256MB is not enough
  either and something is leaking.
- **A target-less `clWrapSpecial` must not include Salvage Corvettes.** The
  function NULL-checks `targets` for the ship-flash and then dereferences it
  unguarded in the salvage branch (`targets->TargetPtr[0]->objtype`), as do
  `isThereAnotherTargetForMe` (`targets->numTargets`) and `clSpecial` (hands
  `targets` to `ShipInSelection`). A salcap's special *is* salvage, so it needs
  a target. This is an original 1999 bug, not a port regression: the desktop Z
  key (`mainrgn.c:1375`), `mouse.c:669` and `KASFunc.c:2933` all pass NULL, so
  pressing Z with salcaps selected crashes the original game too. It survived
  because on desktop you salvage by ctrl-clicking a target; the VR command
  wheel put a target-less special one flick away. Fixed in `clWrapSpecial`
  itself so every caller is covered — and note `MakeShipsSpecialActivateCapable`
  does **not** filter salcaps out, since they genuinely have a
  `CustShipSpecialActivate`, so filtering the selection is not a defence.

## Colour space: why the fleet looked flatly lit

Reported by a player as "it seems to have a sort of lighting from all sides,
where the original only had parallel lighting from one distant source". It was
real, it was VR-only, and it was **not** a lighting bug — the lighting is
untouched by the port.

The engine's hull shading is `shColourSet0` (`src/Game/Shader.c:698`), a
per-vertex `N·L` against the light direction rotated into the ship's own object
space by `shPushLightMatrix`. Nothing in that path reads the camera, the head
pose or the world scale, so it is bit-identical between the flat and VR builds
and cannot be skewed by stereo. The mission data is intact too: every `.hsf` in
`Homeworld.big` carries one ambient plus two distant lights — a warm key at
intensity 1.5-3.0 and a dim blue fill from roughly the opposite side — and the
`HSF/` lookup resolves (`fileExists` searches the .big first, and
`bigTOCFileExists` lowercases and back-slashes the name to match how entries
are stored).

The defect was in presentation. `vrCreateSwapchain` used to prefer linear
`GL_RGBA8`, reasoning that the game's output "is not sRGB-encoded" so an sRGB
chain would double-correct. That is backwards: a 1999 engine's colours are
already display-referred, and declaring the buffer linear makes the compositor
gamma-encode them a second time.

Measured from a `screencap` of the Mothership, hull pixels only:

| | measured | expected from the data |
|---|---|---|
| darkest decile | 115/255 (0.45) | 0.19 |
| brightest decile | 189/255 (0.74) | ~1.0, clipping |
| bright:dark | **1.76 : 1** | ~5 : 1 |

0.19 is the ambient floor for a standard hull material (ambient 0.196, diffuse
0.784 — the constants hardcoded at `Shader.c:262`) under `default.hsf`. And
sRGB-encoding 0.19 gives 0.473, against a measured 0.45. Two controls rule out
a brightness or contrast offset: the region outside the lens barrel stays
exactly `(0,0,0)`, and white still reaches 255 — only the midtones moved, which
is a gamma curve. SurfaceFlinger confirms the panel is `dataspace=V0_SRGB` with
an identity `colorTransform`.

The fix is `vrConfigureColorSpace` (`src/SDL/vr.c`). Declaring
`GL_SRGB8_ALPHA8` alone is not enough and would over-darken instead: GLES 3.0
also converts on *write* into an sRGB target, including through
`glBlitFramebuffer`, which is how all three passes reach their swapchains.
`GL_EXT_sRGB_write_control` is what turns that off, so the sequence is
extension check → `glDisable(GL_FRAMEBUFFER_SRGB)` → sRGB format. Without the
extension it stays on the linear format, because washed out beats double-dark.
All three swapchains (world, eyes, cards) take the format from
`vr.colorFormat`; the cards especially must match, or the wrist panels drift
away from the scene behind them.

Expect the corrected build to look **considerably darker**. That is the point.

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

- **Validated on device by the user**: the command wheel, the 3D depth cursor
  and its zoomed-in reach, hologram recentring, multi-ship flight paths since
  the centroid fix, the button-map restructure, runtime world scale, sensors
  navigation and its toggle, the 3x draw distance, multi-target attack, and the
  skybox hole fix. Nothing in the interaction layer is currently unverified.
- **Parked by decision, not blocked** (2026-07-25): the graphics work. MSAA
  needs the eye passes to render into their own FBOs first — 4x MSAA on the
  window blacked out every wrist card, because an ES multisample *resolve* blit
  requires identical source and destination rectangles and the card blit reads
  `srcY0` while writing 0. The world is also still drawn three times a frame
  (mono pass plus two eyes). There is headroom for all of it; the user chose to
  spend it elsewhere for now.
- Optional: the residual "X" transform for pitch and zoom beyond the game
  camera's clamps. `vrWorldCameraOrbit` and `vrWorldCameraZoom` already return
  the refused residual; all call sites discard it. Acceptance test already
  exists — `VRDBG ROUNDTRIP ... dot=` must stay 1.0000000 with X non-identity.
