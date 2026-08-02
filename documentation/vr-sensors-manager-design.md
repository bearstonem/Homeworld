# The Sensors Manager in VR

Written 2026-08-01 against `bd947ab` as a proposal. Items 1-3 landed on
2026-08-02; see "Progress" at the end for what was built, what it cost, and
what the work turned up. Items 4-6 are not started.

The Sensors Manager currently rides the left wrist as a 46cm quad, like every
other full-screen manager. That is the correct conservative port and it works.
It is also the one screen in Homeworld where the choice costs something real,
because the Sensors Manager is not a screen — it is a 3D scene with its own
camera, and it is where the game expects long-range movement and hyperspace to
be issued.

This document records what the map is supposed to do, what the port does with
it today, what the engine already provides, and what a VR-native version would
be. Every claim about the code was checked against the source at that commit.

## What the map is for

From the 1999 manual, §4.2.3:

> The sensors manager gives you a general view of the entire battlespace, and a
> detailed view of the space surrounding any of your vessels. Detailed areas
> exist within the blue spheres, and represent where your scanners are giving
> accurate information on what lies in the region. The black areas represent
> space outside of your scanning range, and so they are without detail.

What the player can do from inside it, which is more than the name suggests:

- **Move ships, over long distances.** `[M]` or the `<MOVE>` button raises the
  movement disk and orders are issued as they are in the normal view. The quick
  reference card lists this as its own command category — *long-distance
  movement* — because the disk in the main view is range-limited.
  `MAX_MOVE_DISTANCE` (`src/Game/Tweak.c:206`) is 196000000, i.e. 14,000 units;
  `smSensorsClose` (`src/Game/Sensors.c:3846`) cancels a pending move when the
  selection is further than that from the camera on the way out.
- **Hyperspace.** Capital ships, multiplayer only. The RU cost follows the
  cursor and turns red when it is unaffordable. Issuing `<HYPERSPACE>` from the
  normal view raises the Sensors Manager and the movement disk automatically.
- **Tactical Overlay**, toggled from the bottom edge, which in multiplayer is
  also how alliances are formed or broken and how RUs are transferred.
- **Selection and every order that follows from it** — formations, tactics,
  attack, guard, harvest. Hotkey groups work natively; `Sensors.c:4239`
  registers the number keys against the manager's own region.
- **Camera.** Rotate and zoom stay live; `<PAN>` is a press-drag-release mode.
- **Leaving.** Clicking a blob, or band-boxing one, closes the map and takes the
  main camera there. It is the game's fast-travel as much as its map.

Sensor levels are 0, 1 and 2. The Sensors Array ship sets 2 while it lives and
drops back to 0 when it dies (`src/Ships/SensorArray.c:35,65`), which reveals
all uncloaked enemies and resources everywhere. Nothing in this tree grants
level 1 outside the debug `smToggleSensorsLevel` cycle, so in normal play the
value is 0 or 2, even though the renderer has distinct branches for 1
(`Sensors.c:2796,2806`). A dead player is also set to 2 and put into
`smGhostMode` (`UnivUpdate.c:7683`, read at `Sensors.c:3105`), so eliminated
players watch the rest of the match with full vision.

The callback table at `Sensors.c:3917` wires two toggles the manual never
documents: `SM_Resource` and `SM_NonResource`, flipping `smResources` and
`smNonResources`, which gate rendering at `Sensors.c:868,1037,1073,1545` —
show/hide resources and show/hide everything else, independently.

## What the VR build does today

`vrWorldManagerActive` (`src/SDL/vrworld.c:386`) includes `smSensorsActive`
deliberately, with the reason in the comment: the map is full-screen and clears
`mrRenderMainScreen` like the rest, so without it the map would land on the
32cm menu panel instead of the 46cm manager panel (`VR_WRIST_PANEL_WIDTH` and
`VR_WRIST_MANAGER_WIDTH`, `src/SDL/vr.c:914,922`). 46cm at the wrist's ~45cm
subtends about what the old 2.2m head-relative panel did at 2m, so the type
stays the size it was.

While it is open:

- The ray drives the panel pointer, so **band-box selection works** — dragging
  the ray is dragging a mouse.
- The left stick orbits `smCamera` and the right stick's Y zooms it
  (`vrWorldSensorsOrbit` / `vrWorldSensorsZoom`, called from `vr.c:2992-3001`).
  These drive `smCamera` directly rather than through the `camJoy*` globals,
  which the main camera's `cameraControl` would otherwise consume first. They
  exist because the sticks are suppressed under any manager, which had left a
  strategic view that could be pointed at but never moved.
- **Everything else is off.** `managerActive` gates select (`vr.c:3234,3279`),
  sweep (`3290`), the 3D move (`3394,3409`), freehand paths (`3187`), grip
  traversal (`3768`) and even the hover haptic tick (`3130`).

So: a 3D strategic map, flattened to a monoscopic rectangle on the forearm,
driven by a simulated mouse, with the entire VR interaction vocabulary
suppressed at the moment the player is doing the most spatial thinking in the
game.

## What the engine already provides

This is the finding that makes the work worth proposing rather than filing. The
map was expected to be a projected 2D diagram. It mostly is not. Counting the
primitives `smBlobsDraw` and its callees actually emit:

| Element | Drawn with | Space |
|---|---|---|
| Ship dots | `primPoint3` (`Sensors.c:1031,1065,1149`) | world |
| Asteroids, dust | `primPoint3` (`1991,2107`) | world |
| World plane, spokes, ticks | `primLine3` (`602,624,641,713,763`) | world |
| Drop lines to the plane | `primLine3` (`2048`) | world |
| Blob spheres | `primCircleSolid2` (`1860`) | screen |
| Tactical overlay icons | `primLineLoopStart2` (`1204`) | screen |
| Picking | `mouseInRectGeneral` (`1867`) | screen |

The comment at `Sensors.c:1031` says it outright: *everything is rendered as a
point*. `smCamera` is a real camera with a real lookat. What is flat is the
blob circles, the overlay icons, and the hit test — and the hit test is the one
we least want to keep, because `vrWorldSetRay` already does ray-vs-sphere
picking in world space and does it better than nearest-to-cursor in 2D ever
could.

This is replacing a projection layer, not writing a renderer.

## The design

**Do not open a screen. Change what the hologram is showing.**

The world is already an anchored, grabbable, scalable hologram — `vr.worldScale`
in game units per metre, default 1000, clamped to 250–8000 (`vr.c:133-136`),
with a free LOCAL-space translation in `vr.worldOffset` for grip drags. Pressing
Sensors should leave all of that in place and change the representation and the
scale: the fleet dissolves into the sensor abstraction and the view pulls back
to the whole battlespace. The player stays where they are standing; the room
goes from one engagement to the whole war.

**Scale and framing.** Fit the universe extents (`smUniverseSizeX/Y/Z`) into
roughly 1.2–1.5m on entry. Existing scale stepping (`VRW_CMD_SCALE_UP/DOWN`) and
the grip drag keep working unchanged, so "push it away and see all of it" and
"pull it to your chest and put your head inside the sphere" are gestures that
already exist.

**The blue spheres become spheres.** The manual has called them spheres since
1999 and they have always been drawn as flat discs. As translucent shells they
are something the player can stand inside or outside of, which turns the
fog-of-war from a legend entry into a position. One primitive.

**Selection, two-tier.** Ray hover picks the blob from outside and individual
ships once the hand is inside one. `vrWorldSweepBegin`/`Commit` already replaces
the band box with a swept volume; at sensor scale that selects a battle group.
It is strictly better than a 2D band box, which can only describe a frustum
wedge whose depth the player cannot see.

**Movement, which is the reason to do any of this.**
`vrWorldMoveBegin`/`Update`/`Commit` places a destination in free 3D at a cursor
depth, and `vrworld.h:116-121` gives the reason it matters: *there is no
projection plane, so unlike the mouse pie plate there is no viewing angle at
which placement degenerates.* The movement disk, `[SHIFT]`-drag for elevation
and `[CTRL]+[SHIFT]` to cancel it exist only because a mouse cannot say "there"
about a point in space. That problem is already solved in the main view. At
sensor scale a 14,000-unit move is about 14cm of reach, so the long-range order
the Sensors Manager was built to make possible becomes a thing the player points
at.

**Freehand paths become fleet-scale.** `vrWorldPathBegin`/`Sample` — the
centripetal Catmull-Rom routes — are a tactical flourish in the main view. On
the strategic map they express a curve that takes a strike group around the far
side of a dust cloud and in from behind, which is an intent Homeworld has never
had a way to state.

**Hyperspace.** The same gesture, with the RU cost at the fingertip and red when
unaffordable. The desktop already raises the movement disk for this; the
substitution is like for like.

**The wrist keeps the switches.** Tactical Overlay, the resource and
non-resource filters, close. Map in the room, controls on the forearm. This is a
better use of the wrist panel than showing the map on it.

**Two-handed.** Left grip holds and turns the battlespace, right hand commands
into it. Mostly falls out of the existing grip handling.

## Work, in order

1. **Done.** `smBlobsDraw` — a 3D shell primitive in place of
   `primCircleSolid2`, with the existing 2D path kept behind the panel branch.
2. **Done.** Tactical overlay icons — billboard `primLineLoopStart2` into world
   space.
3. **Done, unverified.** A `vrWorldSensorsRay` picker: ray-vs-blob, then
   ray-vs-ship within the hit blob. Mirrors the existing hover path in
   `vrWorldSetRay`.
4. Un-gate the world verbs when `vrWorldSensorsActive()`. The `!managerActive`
   tests at `vr.c:3130,3234,3279,3290,3394,3768` need to mean "not a *panel*
   manager" rather than "not any manager".
5. Entry and exit: bind `VRW_CMD_SENSORS` to a scale-and-representation
   transition rather than to `smSensorsBegin` raising a quad.
6. Keep the panel path alive — see below.

## What must not change

**Mission briefings.** `smFleetIntel` is the game's marker for the map having
been raised by a script rather than by the player (`kasfOpenSensors` /
`kasfCloseSensors`), and `Sensors.c` reads it throughout to lock the player out
while the briefing runs. `vrWorldSensorsBriefing()` already hides the beams and
gizmos during one, and the pointer resolution in `vr.c:3068` excludes the panel
for the same reason. Briefings are authored against a framed 2D presentation and
should keep getting exactly that. Only the player-opened map becomes volumetric.

**The escape path.** `vrWorldCloseManagers` is the guaranteed way out of a
manager and must keep working regardless of whether a panel ever became
presentable — the timeout at `vr.c` that closes an unpresentable manager after
`VR_MANAGER_PENDING_FRAMES` exists because a manager the user cannot see must
never also be modal. A volumetric map has no panel to fail, but it still needs a
close that cannot be lost.

## Risks and unknowns

- **Picking density.** Blobs overlap, and a ray through a crowded battlespace
  will have several candidates. The two-tier scheme and the existing hover
  haptic tick mitigate it; whether that is enough is not known without trying.
- **Per-frame cost.** Blob rendering is already throttled by `smBlobUpdateRate`.
  Shells cost more than discs and there are two eyes. Measure on device; do not
  reason about it. Item 5 of the improvement backlog — the third redundant mono
  pass — is the obvious place to find the headroom if it is needed.
- **The 2D path has to stay compiled and working** for briefings, which means
  the blob draw grows a branch rather than losing one.
- **No multiplayer risk.** This is entirely local presentation and input; no
  simulation state is touched and no packet changes. Given how much of the
  port's fragility lives in networking, that is worth stating plainly — this is
  a large feature that cannot desync anything.

## Progress, 2026-08-02

Items 1-3, on device against a first-mission fleet.

**Cost is not the constraint.** `SMSHELL` reports blob count, shells drawn and
draw calls per eye every 120 frames: `blobs=6 shells=3 step=60 calls/eye=18`,
steady. Eighteen calls is noise, so the ring step stays at 60 and there is room
to make the shells denser rather than thinner. That is a small fleet; the line
is still there to read on a big one.

**Relic had already written the sphere.** `smBlobSphereDraw` existed — a
ten-circle globe, hard-coded white, drawn for the one blob under the movement
cursor. It was generalised to take a colour, a ring step and a segment count
rather than adding a second sphere beside it, and the movement-cursor call
passes the values it always used. It also settles the aesthetic question:
a wireframe globe is what Relic thought a blob looked like, they just never
drew more than one at a time.

Two things constrain retuning it:

- The segment count must come from `pieCircleSegmentsCompute`, which returns
  one of seven values. `primCircleOutline3` caches a unit circle per
  (slice count, axis) in a linear list that is never freed, so a freely derived
  count grows that list without bound and lengthens the walk on every call.
- A ring step that divides 90 draws one circle twice: an `X_AXIS` circle turned
  90° about Z and a `Z_AXIS` circle turned 90° about X are the same great
  circle. Relic's 40 never lands there; 90, 45 and 30 all do. The duplicate is
  skipped explicitly so this cannot be reintroduced by retuning.

**Three defects, all found by looking at the thing.** Worth recording because
only the first announced itself:

- `colMultiplyClamped` clamps its factor at 1.0 and then computes
  `(c * 255) >> 8`, so it can only darken — at the top of its range it still
  takes half a percent off. It is not a brightness control. Blending toward
  white is.
- Moving the overlay icons out of `primModeSet2` took away the depth-test-off
  that the 2D path had been giving them for free, so they z-fought the mesh
  they mark and vanished behind blob shells. An overlay is an overlay.
- `toVertexAdd` bakes a 4:3 term into every icon vertex's y
  (`TO_VertexScanFactorY`) that x does not get, because the 2D path writes NDC
  directly and NDC y and x do not cover the same number of pixels. A billboard
  needs none of it: equal offsets along the camera's right and up project to
  equal pixel distances, since the projection's own `P11/P00` is that ratio.
  Carried through, it stretched every icon vertically by a third. Undoing it
  also makes the glyphs correct at any aspect, which the 2D path is not.

The last two hid behind class glyphs that are legitimately tall and narrow;
neither was visible in a screenshot, and both fell out of checking the
arithmetic against what the old path did.

**The picker is written and cannot yet be verified.** `vrwSensorsPick` walks
`universe.collBlobList` with the same visibility test the shells use, has no
aim assist (a blob is already a sphere thousands of units across), and prefers
a blob the hand is standing in over any it merely points through. Ships resolve
only from inside a blob — from across the map a blob is a region, and picking
one of the ships stacked along the line of sight would be a guess the player
cannot see well enough to correct.

What it cannot do yet is mean anything. The ray reaching it has been
transformed through the main view's camera, and a full-screen manager freezes
that camera — `vrWorldGameCameraAge()` goes non-zero precisely because the mono
pass stops. Item 5 is what gives the map a presentation with its own camera,
and until then the picker aims through a stale transform at geometry that only
exists on a wrist panel.

**Open: does panel selection on the map work at all?** Reported on device as
"no way to select the blobs or ships". The code path looks complete from both
ends — `vr.c` presses the left button on a panel hit, keeps warping the pointer
to the tracked hand for as long as the gesture is locked, and releases; and
`smViewportProcess` turns that into `smSelectHold` for a band box and
`selSelectionClick` / `selRectDragAnybodyAnywhere` on release. A click does not
close the map.

The map's own FE buttons definitely do work: the tactical overlay has been
toggled from the panel. So this is about the viewport region
(`smViewportRegion`, `SM_ViewportFilter`) rather than about panel input in
general. One suspect is `mouseClipToRect(&smViewRectangle)` on `RPE_PressLeft`
against `SDL_WarpMouseInWindow` running every frame from the VR side.

Worth ten minutes before item 4 only to rule out the viewport region receiving
no input at all — because the map's *own* controls live there too, and unlike
selection those cannot be replaced by world verbs. If it is only selection,
leave it: items 4 and 5 replace panel selection with the picker and the ray,
and any fix here is thrown away.

**The desktop build cannot catch errors in `vrworld.c`.** The whole file is
inside `HW_ENABLE_VR`, so on desktop it compiles to nothing: a missing
`Blobs.h` passed `ninja -C build` clean and failed both Quest builds. Any
change to a VR-only file has to be built for the Quest before it means
anything, which is a concrete argument for item 4 of the improvement backlog.

## Sources

The 1999 manual and the quick reference card, both of which defeat a plain HTTP
fetch — the PDFs return as compressed streams and have to go through
`pdftotext -layout` to be read at all.

- Manual: `https://dn790009.ca.archive.org/0/items/Homeworld_Manual/Homeworld_Manual.pdf`
  — §4.2.3 Sensors Manager, §3.4 Movement, §3.8 Other Commands, §7.2 Video
  Options (Blob Alpha, Instant SM Transition).
- Quick reference card: `https://homeworld.neocities.org/download/pdf/referenc.pdf`
  — §4.0 Movement, §6.0 Manager Screens, §10.0 Multiplayer Controls.

## Camera ownership, and four ways it bites

The sensor view is the first thing in the port to take the game camera away
from the engine for a while, and every one of these cost real time on
2026-08-02. None is visible from the code.

**The focus stack recomputes distance, not just the lookat.**
`NewSetFocusPoint` rebuilds `remembercam` from the focus bounding box every
tick, so a `ccFocus` left running will fight anything else driving the camera
and win. Using `ccFocus` to aim a zoom made zooming *in* appear to work - it
pulled the same way - while zooming out did nothing at all. Cancel focus on
entry and own the lookat outright.

**Do not move the camera between the capture and the render.** Controller rays
are resolved through `lookatInv`, built from the camera matrix captured during
the *previous* frame's render (`vrWorldCaptureGameCamera`, from
`rndMainViewRenderFunction`). Setting `distance` and `lookatpoint` is safe -
`cameraControl` derives the eye position on its own schedule. Calling
`cameraSetEyePosition` directly is not: it moves the camera inside that gap and
every ray in the frame solves against a camera that no longer exists.

**Do not hold the camera every render frame.** `CameraChase` eases
`actualcamera` toward `remembercam` on the universe tick, which runs at a
different rate from the render. Writing both every frame leaves them never
quite agreed and the whole view shimmers. `ccCancelFocus` plus
`CAM_USER_MOVED|CAM_USER_ZOOMED` is what actually keeps a camera still.

**`ccCancelFocus` pops the stack, so resolve camera pointers after it.**
`vrwActualCamera` and `vrwRememberCamera` go through
`currentCameraStackEntry`. Pointers taken before the cancel address the entry
that was just popped, and everything written through them lands on a camera
nobody is looking out of - which opened the view at whatever close-up the pop
had restored.

## Known, not fixed

**Rays drift while the zoom is held.** Same one-frame lag as above: during a
continuous zoom the camera moves every frame, so the rays trail it by a frame's
worth of movement and snap back on release. The zoom rate is halved in the
sensor view to halve the drift, which is a mitigation and not a fix. The fix is
to re-capture the lookat from the current camera at the top of
`vrWorldFrameBegin` rather than relying on the previous frame's capture -
`render.c` builds it with `rgluLookAt` from `eyeposition + scaledOffset`,
`lookatpoint` and `upvector`, and that `scaledOffset` term is the part to get
right.

**The view jitters while ships are moving.** Not the camera - the collision
blobs re-form under the drawing. `bobListCreate` reclusters every tick and a
ship crossing a cluster boundary changes a blob's centre and radius
discontinuously, so shells and glow jump rather than slide. The map hides this
by throttling its own blob work with `smBlobUpdateRate`; the overlay reads the
live list every frame. Either sample on a slower cadence or smooth each blob's
centre and radius - the latter needs blob identity to survive a rebuild, which
has not been checked.

**Class glyphs are drawn for every ship.** The desktop map draws them only for
Resource Collectors and Capital Ships (`smShipTypeRenderFlags`, `SM_TO`). Worth
narrowing once there is a real fleet to judge it against.
