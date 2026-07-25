/*=============================================================================
    Name    : vrworld.h
    Purpose : VR-native interaction with the game world (Quest/OpenXR).

    Bridges the OpenXR layer (vr.c, poses in LOCAL metres) and the game:
    controller rays are transformed into game-world space and drive the
    engine's own selection/command/camera seams directly - no synthesized
    mouse input.

    Created 24/07/2026
=============================================================================*/

#ifndef ___VRWORLD_H
#define ___VRWORLD_H

#include "Types.h"

#ifdef HW_ENABLE_VR

/* Visual/interaction intent for a controller ray.  vr.c owns the input
   gesture; vrworld uses this to provide consistent world-space feedback. */
typedef enum
{
    VRW_INTENT_IDLE = 0,
    VRW_INTENT_SELECT,
    VRW_INTENT_ADD_SELECT,
    VRW_INTENT_ATTACK,
    VRW_INTENT_HARVEST,
    VRW_INTENT_DOCK,
    VRW_INTENT_MOVE,
    VRW_INTENT_PANEL,
    VRW_INTENT_INVALID
} vrworldintent;

/* Update the LOCAL->game-world transform state. Called once per vrFrame
   (before the eye renders overwrite rndCameraMatrix) with the current
   world anchor. Returns TRUE when a game world exists to interact with. */
bool32 vrWorldFrameBegin(real32 const anchorPos[3], real32 const anchorQuat[4], real32 scale);

/* Hand this layer the pure mono main-view camera matrix, straight from
   rndMainViewRenderFunction. rndCameraMatrix cannot be sampled at vrFrame
   time: every full-screen manager draws a spinning ship preview that
   overwrites the global with its own close-up camera, and suppresses the
   mono main-view render entirely (mrRenderMainScreen), so the global is
   both wrong and never refreshed while a manager is open. */
void vrWorldCaptureGameCamera(real32 const view[16]);

/* Frames since the game camera was last captured; -1 when never captured.
   Non-zero means a manager is holding the main view down. */
udword vrWorldGameCameraAge(void);

/* Controller aim ray (LOCAL metres) -> game world ray + hover update.
   hand: 0 = left, 1 = right. valid FALSE clears that hand's ray. */
/* Returns TRUE when this hand acquired a different non-null hover target,
   allowing the OpenXR layer to provide a small haptic tick. */
bool32 vrWorldSetRay(sdword hand, real32 const origin[3], real32 const dir[3], bool32 valid);

/* TRUE when the given hand's ray currently points at a targetable object */
bool32 vrWorldHandHasTarget(sdword hand);
bool32 vrWorldHandHasSelectable(sdword hand);

/* Distance to the current world hit in metres, or -1 when there is none.
   Used to route input to whichever surface (world or wrist panel) is nearer. */
real32 vrWorldHandHitDistance(sdword hand);

/* Clip the drawn ray at this distance (metres) - set when the pointer is
   on the wrist panel so the beam visually ends at the panel surface. */
void vrWorldSetRayLimit(sdword hand, real32 metres);
void vrWorldSetIntent(sdword hand, vrworldintent intent);

/* Selection: click select the hovered ship of this hand (additive with
   shift semantics when additive is TRUE). Returns TRUE if it changed. */
bool32 vrWorldSelectClick(sdword hand, bool32 additive);

/* Double-trigger convenience: select visible player ships matching the
   hovered ship's type. */
bool32 vrWorldSelectType(sdword hand, bool32 additive);

/* Sweep (band-box replacement): while active, player ships near the
   hand's ray accumulate into a preview; commit applies to selSelected. */
void vrWorldSweepBegin(sdword hand);
bool32 vrWorldSweepCommit(sdword hand, bool32 additive);
void vrWorldSweepCancel(void);

/* Context order with the hand's hovered target: attack enemy, harvest
   resource, dock at friendly. Returns TRUE if an order was issued. */
vrworldintent vrWorldContextIntent(sdword hand);
bool32 vrWorldContextOrder(sdword hand);

/* Move order placed freely in 3D. The destination rides the hand's ray at a
   cursor depth, seeded from whatever the beam is touching (or the fleet's
   own distance) and then scaled by depthDelta, a fractional change applied
   for this frame. There is no projection plane, so unlike the mouse pie
   plate there is no viewing angle at which placement degenerates. Returns
   FALSE from Begin when there is no selection. */
bool32 vrWorldMoveBegin(sdword hand);
void vrWorldMoveUpdate(sdword hand, real32 depthDelta);
bool32 vrWorldMoveCommit(void);
void vrWorldMoveCancel(void);
bool32 vrWorldMoveActive(void);

/* Current cursor depth in game units, for diagnostics */
real32 vrWorldCursorDist(void);

/* Freehand flight paths. While a move preview is running, holding the
   trigger draws a path through space: the swept points are smoothed into a
   Catmull-Rom spline, then resampled by arc length into a bounded set of
   waypoints. Committing hands the waypoints to a follower that issues each
   leg with clWrapMove as the previous one is reached, since Homeworld's
   command layer has no waypoint order of its own. */
void   vrWorldPathBegin(sdword hand);
void   vrWorldPathSample(sdword hand);
bool32 vrWorldPathFinishStroke(void);   /* trigger released: build waypoints */
bool32 vrWorldPathCommit(void);         /* A/X released: fly it */
void   vrWorldPathCancel(void);
bool32 vrWorldPathDrawing(void);
bool32 vrWorldPathActive(void);         /* a committed path is being flown */
sdword vrWorldPathPointCount(void);

/* Camera manipulation (grab gestures). Deltas in LOCAL metres / radians;
   conversion to game units happens inside. Returns the residual fraction
   the camera clamps refused (0 = fully applied), for folding into the
   free VR transform. */
void vrWorldCameraPan(real32 const deltaMetres[3]);
real32 vrWorldCameraOrbit(real32 deltaYaw, real32 deltaPitch);
real32 vrWorldCameraZoom(real32 ratio);
void vrWorldCameraFocusSelection(void);

/* Cycle the camera focus through the player's fleet (+1 next / -1 prev) */
void vrWorldFocusCycle(sdword step);

/* Draw the VR overlays (aim rays, hover ring, selection rings, move disc)
   in game-world space. Called inside each per-eye render pass, after
   rndMainViewRenderFunction (matrices still current). */
void vrWorldDrawOverlays(void);

/* LOCAL-space anchor (metres) and collision radius of the ship a
   full-screen manager concerns. Used only to bias which way the manager
   panel is placed - the panel itself is positioned relative to the head,
   because it is the only way back out of a manager. Returns FALSE when no
   finite anchor can be derived. */
bool32 vrWorldManagerPanelAnchor(real32 outPosMetres[3], real32* outRadiusMetres);

/* TRUE while a full-screen manager is open (panel should own the pointer) */
bool32 vrWorldManagerActive(void);

/* Which manager is open, for diagnostics ("none" when none is) */
char const* vrWorldManagerName(void);

/* Close every full-screen manager that can be closed. This is the
   guaranteed VR escape path, so it must work regardless of whether the
   manager's panel ever became presentable. Returns TRUE when none remain. */
bool32 vrWorldCloseManagers(void);

/* Right-grip+B: open the Build Manager, or close it when already open.
   Returns TRUE when the state was toggled. */
bool32 vrWorldToggleBuildManager(void);

#endif /* HW_ENABLE_VR */

#endif
