# Native VR Interaction Design (Meta Quest)

## Goal

No screen during gameplay: selection, orders and camera control happen
directly in the 3D holographic battle space. Orbit with the left stick, zoom
with the right, scale the hologram from the wheel, point at ships to select,
and issue orders with the controller ray.

There is deliberately **no pan**. Homeworld's camera is focus-locked by
design: every camera verb is a focus verb (`ccFocus`, `ccFocusFar`,
`ccFocusOnMyMothership`, `ccForwardFocus`, `ccCancelFocus`), and
`NewSetFocusPoint` recomputes `remembercam.lookatpoint` from the focus
bounding box every tick, so a pan would be overwritten every frame. Looking
elsewhere means focusing on something, which the command wheel, hotkey group
recall and right-grip fleet cycling all expose. Recentring the hologram
(left-stick click) is a separate, presentation-only concern: it moves where
the hologram sits in the room via `vr.worldOffset`, not the game camera
through the world. Dense 2D interfaces
(Build/Research/Sensors managers, menus, tutorial wizard) live on a small
panel attached to the left wrist; out of game the panel is a lazy-follow
floating screen.

## Architecture: hybrid free-transform + camera-follow

Gestures produce a desired view delta each frame. It is routed into the
**real game camera** first (tutorial validation, LOD, audio and the focus
stack all stay coherent); only what the orbit camera's clamps refuse
(pitch past ±85°, zoom past limits, true scaling) accumulates in a small
"excess" similarity transform **X** (uniform scale + pitch, applied in
camera space) composed into the eye views. X is normally identity; an
*unwind-first* policy drains it back whenever the user gestures back into
the camera's legal range. A reset gesture (both stick-clicks) decays X to
identity and refocuses on the selection.

Key discipline (verified in CameraCommand.c): camera mutations must be
applied to **both** `currentCameraStackEntry(&universe.mainCameraCommand)
->remembercam` (the tween target) and `actualcamera`, and must set
`UserControlled |= CAM_USER_MOVED/ZOOMED` — otherwise `CameraChase`
tweens the change away. Respect `CCMODE_LOCK_OUT_USER_INPUT` and NIS
cutscenes.

### Matrix pipeline

- `L` = game lookat (world→camera, game units) — captured once per frame
  from `rndCameraMatrix` right after the mono pass (the eye passes
  overwrite it).
- `A` = VR anchor pose (LOCAL metres), `S0 = VR_WORLD_SCALE` (GU/metre).
- Per-eye view: `E = Dscale(S0/σ) · View(eyePose) · A · X · Dscale(1/S0)`
  — pure rotation in the linear part, so game near/far planes stay valid,
  and stereo separation automatically equals `IPD · S_eff` at every scale
  (`S_eff = S0/σ`).
- Controller ray into game world: `world = L⁻¹ · Dscale(S0) · X⁻¹ · A⁻¹ ·
  pose` (directions use the rotation part + normalize).

## Interaction (src/SDL/vrworld.c + vr.c input state machine)

- **Hover/pick**: ray-vs-collision-sphere over `universe.RenderList`
  (`collInfo.collPosition`, `staticCollInfo.collspheresize`, inflated;
  nearest positive t). No screen-space picking involved.
- **Trigger** on a friendly ship: select (`selSelectionSetSingleShip`);
  with left grip held: additive toggle (`selSelectionAdd/RemoveSingleShip`);
  on empty space: sweep select — ships the ray passes over accumulate into
  a preview committed on release (band-box replacement). Always
  `ioUpdateShipTotals()` after mutations.
- **A/X** on a target: context order mirroring `mrRightClickMenu` —
  resource → `clWrapCollectResource` (+`Game_ClickHarvest`), enemy →
  `MakeShipsAttackCapable` + `clWrapAttack` (+`Game_ClickAttack`), own
  ship → `clWrapDock(DOCK_AT_SPECIFIC_SHIP)`.
- **A/X on empty space** with a selection: move order, pie-plate semantics
  natively — ray ∩ horizontal plane at `selCentrePoint.z`
  (`vecLineIntersectWithXYPlane`), height offset in a second stage,
  commit via `clWrapMove` (+`Game_Move*` tutorial messages), ghost disc
  and vertical line drawn as overlays.
- **Grips**: no camera gesture on either. The left adds to the selection, the
  right modifies the right stick into fleet traversal. One grip does not pan -
  see the note above - and the two-grip pinch that used to zoom and orbit is
  gone, since the left stick, the right stick and the wheel's View page
  already carry every camera verb it reached.
- **Overlays** (aim rays, hover ring, selection rings, sweep preview,
  move disc) drawn in game-world space with `primLine3` /
  `primCircleOutline3` inside each eye pass right after
  `rndMainViewRenderFunction` — the 2D selection feedback never appears
  in eye views, these replace it.
- All order paths fire the same `tutGameMessage` strings as the mouse
  paths and respect `playPackets/universePause/mrDisabled` guards, so the
  tutorial and multiplayer wrappers behave identically.

## Typing (src/SDL/vrkeys.c)

Menus need text - a player name, a game name, chat, and the address of an
internet host - and a headset has no keyboard. Both the key grid and the
address field are drawn by the VR layer into the window framebuffer just
before that frame is copied to the panel quad, and hit-tested against
`vr.pointerX/pointerY`, which is already the controller ray mapped into
logical UI pixels. No new swapchain, pose or hit test: a fourth wrist card
would have needed all three for the same result.

- **Up exactly while a text entry holds `RSF_KeyCapture`**, found by walking
  the region tree from `regRootRegion` and matching on
  `uicTextEntryProcess`. List windows take key focus too, so the process
  function rather than the status bit is what decides.
- **Scancodes only.** The engine derives the character itself, through
  `SDL_GetKeyFromScancode` into the ASCII-indexed `uicKeyEntryTable`
  (`UIControls.c`). Injecting letters would be fighting it.
- **Shift is genuinely held.** `keyPressDown` records the shift state into
  the key buffer at press time (`keyBufferAdd(key, keyIsHit(SHIFTKEY))`), so
  a shifted glyph pushes `LSHIFT` down, the key, and `LSHIFT` up.
- **CAPS latches and applies to letters only.** One ray cannot point at
  shift and a letter at once. A latch that shifted everything would turn the
  number row into `!"#`, and an address is digits and dots.
- **It never covers what is being typed into.** The grid sits along the
  bottom and moves to the top when the focused entry is down there, which is
  where the lobby keeps its chat.
- **The address field is the VR layer's own**, drawn in the gap the lobby
  leaves in its right-hand button column. It takes keys locally rather than
  pushing them at the engine, and commits through `lanAddRemote`. The screen
  definitions live in `Homeworld.big`, which belongs to the player, so a
  field the game does not have cannot be added where its own fields are.

## Files

- `src/SDL/vrworld.c/.h` (new): ray transforms, picking, selection,
  orders, move plane, camera routers, overlays. Game-coupled; no OpenXR
  types.
- `src/SDL/vrkeys.c/.h` (new): the on-panel keyboard and the join-by-address
  field. Game-coupled; no OpenXR types.
- `src/SDL/vr.c`: L/anchor capture per frame, X state + composition,
  grip pose actions, gesture decomposition, input state machine, wrist
  panel pose, overlay call in the eye pass.
- `src/SDL/meson.build`: add vrworld.c, vrkeys.c.

## Verification (on Quest 3 via wireless adb)

1. Identity refactor: image identical to previous build.
2. Pinch zoom/orbit: 1:1, no rubber-banding (CameraChase converged),
   persists on release; residual X engages only past clamps and unwinds
   first; recentre recovers.
3. Hover ring lands on pointed ships in both eyes at multiple scales
   (validates the whole inverse chain).
4. Select/additive/sweep; orders (move with disc ghost, attack, harvest,
   dock); tutorial mission progresses through its steps end to end.
5. Wrist panel: managers usable, wizard clickable; menus float when not
   in game.
