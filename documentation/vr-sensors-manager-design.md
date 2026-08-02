# The Sensors Manager in VR

A proposal, written 2026-08-01 against `bd947ab`. Nothing here is in progress.

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

1. `smBlobsDraw` — a 3D shell primitive in place of `primCircleSolid2`, with the
   existing 2D path kept behind the panel branch.
2. Tactical overlay icons — billboard `primLineLoopStart2` into world space.
3. A `vrWorldSensorsRay` picker: ray-vs-blob, then ray-vs-ship within the hit
   blob. Mirrors the existing hover path in `vrWorldSetRay`.
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

## Sources

The 1999 manual and the quick reference card, both of which defeat a plain HTTP
fetch — the PDFs return as compressed streams and have to go through
`pdftotext -layout` to be read at all.

- Manual: `https://dn790009.ca.archive.org/0/items/Homeworld_Manual/Homeworld_Manual.pdf`
  — §4.2.3 Sensors Manager, §3.4 Movement, §3.8 Other Commands, §7.2 Video
  Options (Blob Alpha, Instant SM Transition).
- Quick reference card: `https://homeworld.neocities.org/download/pdf/referenc.pdf`
  — §4.0 Movement, §6.0 Manager Screens, §10.0 Multiplayer Controls.
