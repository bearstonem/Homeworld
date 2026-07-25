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

/* Update the LOCAL->game-world transform state. Called once per vrFrame
   (before the eye renders overwrite rndCameraMatrix) with the current
   world anchor. Returns TRUE when a game world exists to interact with. */
bool32 vrWorldFrameBegin(real32 const anchorPos[3], real32 const anchorQuat[4], real32 scale);

/* Controller aim ray (LOCAL metres) -> game world ray + hover update.
   hand: 0 = left, 1 = right. valid FALSE clears that hand's ray. */
void vrWorldSetRay(sdword hand, real32 const origin[3], real32 const dir[3], bool32 valid);

/* TRUE when the given hand's ray currently points at a targetable object */
bool32 vrWorldHandHasTarget(sdword hand);

/* Clip the drawn ray at this distance (metres) - set when the pointer is
   on the wrist panel so the beam visually ends at the panel surface. */
void vrWorldSetRayLimit(sdword hand, real32 metres);

/* Selection: click select the hovered ship of this hand (additive with
   shift semantics when additive is TRUE). */
void vrWorldSelectClick(sdword hand, bool32 additive);

/* Sweep (band-box replacement): while active, player ships near the
   hand's ray accumulate into a preview; commit applies to selSelected. */
void vrWorldSweepBegin(sdword hand);
void vrWorldSweepCommit(sdword hand, bool32 additive);
void vrWorldSweepCancel(void);

/* Context order with the hand's hovered target: attack enemy, harvest
   resource, dock at friendly. Returns TRUE if an order was issued. */
bool32 vrWorldContextOrder(sdword hand);

/* Move order, pie-plate semantics in 3D: begin on empty space, update
   follows ray/plane intersection with heightMetres vertical offset,
   commit issues clWrapMove. Returns FALSE from Begin when no selection. */
bool32 vrWorldMoveBegin(sdword hand);
void vrWorldMoveUpdate(sdword hand, real32 heightMetres);
void vrWorldMoveCommit(void);
void vrWorldMoveCancel(void);
bool32 vrWorldMoveActive(void);

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

/* When a full-screen manager (Build/Launch/Research) is open, the game
   frame should be shown as a panel floating beside the ship it concerns:
   returns TRUE and the LOCAL-space anchor position (metres). */
bool32 vrWorldManagerPanelAnchor(real32 outPosMetres[3]);

/* TRUE while a full-screen manager is open (panel should own the pointer) */
bool32 vrWorldManagerActive(void);

#endif /* HW_ENABLE_VR */

#endif
