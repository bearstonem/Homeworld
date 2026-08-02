/*=============================================================================
    Name    : vrworld.c
    Purpose : VR-native interaction with the game world (see vrworld.h).

    Coordinate pipeline: rendered geometry goes
        game world -> [game camera lookat] -> [anchor, scaled] -> LOCAL -> eye
    so a controller pose in LOCAL space maps into the game world through the
    inverses: world = lookat^-1 * anchor^-1 * (pose * VR_WORLD_SCALE).
    The pure lookat matrix is pushed in from rndMainViewRenderFunction via
    vrWorldCaptureGameCamera. Sampling rndCameraMatrix here instead does not
    work: ShipView and the sensors manager overwrite that global with their
    own cameras, and every full-screen manager clears mrRenderMainScreen so
    the mono main-view render - the only thing that refreshes it - stops.

    Created 24/07/2026
=============================================================================*/

#ifdef HW_ENABLE_VR

#include "vrworld.h"

#include <math.h>
#include <string.h>

#include "Alliance.h"
#include "Blobs.h"
#include "Camera.h"
#include "CameraCommand.h"
#include "ConsMgr.h"
#include "FastMath.h"
#include "LaunchMgr.h"
#include "ResearchGUI.h"
#include "Sensors.h"
#include "SoundEvent.h"
#include "SoundEventDefs.h"
#include "CommandDefs.h"
#include "CommandLayer.h"
#include "Formation.h"
#include "FormationDefs.h"
#include "Undo.h"
#include "CommandWrap.h"
#include "Dock.h"
#include "InfoOverlay.h"
#include "Matrix.h"
#include "ObjTypes.h"
#include "prim2d.h"
#include "prim3d.h"
#include "render.h"
#include "Select.h"
#include "ShipSelect.h"
#include "SpaceObj.h"
#include "Tactics.h"
#include "TradeMgr.h"
#include "Tutor.h"
#include "Universe.h"
#include "Vector.h"
#include "mainrgn.h"
#include "Options.h"

extern Camera *mrCamera;                                    //mainrgn.c
extern Camera smCamera;                                     //Sensors.c
extern bool32 gameIsRunning;                                //Globals.c

#define VRW_HAND_COUNT     2
#define VRW_PICK_MARGIN    1.25f    /* collision sphere inflation for picking */
#define VRW_PICK_MARGIN_MAX 150.0f  /* cap the margin so capitals stay fair */
/* The Mothership is the one hull where the collision sphere is a bad stand-in
   for the ship: it is long and thin, so the sphere containing it reaches far
   out into empty space on every side. That empty space is exactly where you
   work - docking, building, moving escorts around it - and aiming at a
   fighter parked alongside picks the Mothership instead. Both the ring that
   says "selected" and the sphere that decides what the ray hit are scaled by
   this, together, so the two never disagree about how big the ship is. */
#define VRW_MOTHERSHIP_SCALE 0.5f
#define VRW_PICK_TIE_ANGLE  0.004f  /* aims this close in angle count as equal */
#define VRW_PICK_STICKY_ANGLE 0.002f /* angular credit for the previous target */
#define VRW_PICK_CONE_TAN  0.012f   /* ~0.7 degree controller selection cone */
#define VRW_PICK_CONE_MAX  180.0f   /* cap distant-target assistance */
#define VRW_SWEEP_CONE_TAN 0.070f   /* ~4 degree brush: sweeping paints over
                                       a group rather than threading between
                                       ships one aim-cone at a time */
#define VRW_SWEEP_MARGIN   1.60f    /* collision inflation while sweeping */
#define VRW_SWEEP_TRAIL    48       /* samples of the swept path to remember */
#define VRW_MAX_SWEEP_TARGETS 256   /* well under COMMAND_MAX_SHIPS: commit runs
                                       MakeShipMastersIncludeSlaves, which GROWS
                                       the list, so filling it here first would
                                       overflow the array */
#define VRW_RAY_LENGTH     100000.0f
#define VRW_CURSOR_MIN     400.0f   /* nearest the cursor may sit: 50 units is
                                       5cm of hologram, i.e. inside the hand */
#define VRW_DEBUG_INTERVAL 120

/* Freehand path drawing */
#define VRW_PATH_MAX_SAMPLES 64     /* raw swept points before decimation */
#define VRW_PATH_MAX_POINTS  192    /* curve points, evenly spaced by arc length */
#define VRW_PATH_SPLINE_STEPS 16    /* spline evaluations per raw segment */
#define VRW_PATH_SMOOTH_PASSES 2    /* [1 2 1] passes over the raw samples */
#define VRW_PATH_KNOT_MIN   0.001f  /* floor on knot spacing, guards a div0 */

/* How far ahead of the fleet the carrot rides, and why 10 seconds of travel:
   aishipFlyToPointAvoidingObjsFunc asks for a velocity of
   VELOCITY_SCALE_FACTOR (0.1) times the distance left, capped at the ship's
   maximum. Below ten seconds of travel that product is the cap, so the group
   throttles back in proportion to how close the target is. Ten seconds out is
   exactly where full speed is commanded. */
#define VRW_PATH_LEAD_SECONDS 10.0f
/* ...but never more than this fraction of the whole path, or a short curl
   would be flown as a straight line to a point past its own end. Shape wins
   over speed on a small stroke; on a long one the cap never binds. */
#define VRW_PATH_LEAD_MAX_FRAC 0.35f
#define VRW_PATH_LEAD_HULLS   3.0f  /* floor: clear of the group's own bulk */
#define VRW_PATH_SEARCH_LEADS 2.0f  /* forward window when projecting onto the curve */
#define VRW_PATH_REISSUE_FRAC 0.25f /* of the lead, before a fallback re-order */
#define VRW_PATH_STALL_TIME  20.0f  /* seconds of not moving before giving up */

/* Sensor blob shells in the hologram. Dimmer than the map's, because there
   they sit on a black screen and here they are drawn over a lit battle. */
#define VRW_SENSOR_BLOB_COLOR colRGB(90, 120, 220)
#define VRW_SENSOR_GLOW_ALPHA    110    /* additive, so this goes a long way */
#define VRW_SENSOR_GLOW_SEGMENTS 20
/* How much of the room the battlespace is scaled into when the sensor view
   comes up. Slightly less than arm's reach across, so the whole thing can be
   seen at once and still be leaned into. */
#define VRW_SENSOR_VIEW_METRES   1.6f

typedef struct {
    bool32  valid;
    vector  origin;                 /* game world units */
    vector  dir;                    /* unit direction   */
    SpaceObjRotImpTarg *hover;      /* object under this ray, if any */
    real32  hoverT;                 /* ray parameter of the hover hit */
    real32  limitT;                 /* draw clip (panel hit), 0 = none */
    /* Sensors map only. The map's unit of interest is the blob, not the
       ship: from outside one there is nothing else to point at, and the
       ships within are what the blob resolves into once the hand is inside
       it. hover carries the ship in that second case, as everywhere else. */
    blob   *hoverBlob;
    vrworldintent intent;
} vrwray;

static struct {
    bool32  worldValid;
    bool32  sensorsOverlay;         /* sensor representation in the hologram */
    real32  scale;                  /* metres -> game units */
    real32  gameCam[16];            /* pure mono main-view camera matrix */
    bool32  gameCamValid;
    udword  gameCamAge;             /* frames since the last capture */
    real32  lookatInv[16];          /* inverse of the game camera lookat */
    real32  anchorInv[16];          /* inverse of the scaled anchor pose */
    real32  lookatFwd[16];          /* forward transforms (world -> LOCAL) */
    real32  anchorFwd[16];
    vrwray  ray[VRW_HAND_COUNT];

    /* sweep select */
    bool32  sweepActive;
    sdword  sweepHand;
    MaxSelection sweepPreview;
    vector  sweepTrail[VRW_SWEEP_TRAIL];    /* far edge of the brush, recent */
    sdword  sweepTrailCount;
    real32  sweepReach;                     /* how far down the ray to draw */

    /* attack target sweep, during an order preview */
    bool32  targetSweepActive;
    bool32  targetSweepPainting;    /* the trigger is down: brush is live */
    sdword  targetSweepHand;
    MaxAnySelection targetSweep;

    /* move order */
    bool32  moveActive;
    sdword  moveHand;
    vector  moveDestination;        /* point on the ray at cursorDist */
    real32  cursorDist;             /* depth along the ray, game units */
    bool32  cursorManual;           /* stick has overridden the auto depth */

    /* freehand path: raw stroke */
    bool32  pathDrawing;
    sdword  pathHand;
    vector  pathSample[VRW_PATH_MAX_SAMPLES];
    sdword  pathSampleCount;
    real32  pathMinSampleDist;      /* decimation threshold, game units */

    /* freehand path: the curve, resampled to even arc length so that
       "the point s units along" is one divide and one lerp */
    vector  pathPoint[VRW_PATH_MAX_POINTS];
    sdword  pathCount;
    real32  pathSpacing;            /* arc length between adjacent points */
    real32  pathLength;             /* arc length of the whole curve */

    /* freehand path: the follower */
    bool32  pathActive;
    MaxSelection pathSelection;
    real32  pathProgress;           /* arc length the fleet has reached */
    real32  pathLead;               /* how far ahead of that the carrot sits */
    real32  pathArrive;             /* centre this close to the end = done */
    vector  pathIssued;             /* destination the command layer was last given */
    bool32  pathSteering;           /* driving the command directly, not re-ordering */
    vector  pathStallWhere;         /* centre when the stall clock last reset */
    real32  pathStallSince;
    real32  pathLastUpdate;

    /* diagnostics */
    udword  debugFrame;
    vector  dbgLocalPos[VRW_HAND_COUNT];
    vector  dbgLocalDir[VRW_HAND_COUNT];
} vrw;

/*-----------------------------------------------------------------------------
    Small column-major matrix helpers (duplicating vr.c's private ones keeps
    this file free of OpenXR types).
----------------------------------------------------------------------------*/
static void vrwQuatToMat3(real32 const q[4], real32 R[9])   /* q = x,y,z,w */
{
    R[0] = 1.0f - 2.0f * (q[1] * q[1] + q[2] * q[2]);
    R[1] = 2.0f * (q[0] * q[1] - q[2] * q[3]);
    R[2] = 2.0f * (q[0] * q[2] + q[1] * q[3]);
    R[3] = 2.0f * (q[0] * q[1] + q[2] * q[3]);
    R[4] = 1.0f - 2.0f * (q[0] * q[0] + q[2] * q[2]);
    R[5] = 2.0f * (q[1] * q[2] - q[0] * q[3]);
    R[6] = 2.0f * (q[0] * q[2] - q[1] * q[3]);
    R[7] = 2.0f * (q[1] * q[2] + q[0] * q[3]);
    R[8] = 1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]);
}

/* inverse of a rigid transform given rotation R (row-major 3x3) and
   translation t, written as a column-major 4x4 */
static void vrwRigidInverse(real32 const R[9], real32 const t[3], real32 out[16])
{
    sdword r, c;

    for (c = 0; c < 3; c++)
    {
        for (r = 0; r < 3; r++)
        {
            out[c * 4 + r] = R[c * 3 + r];                  /* R^T: element (r,c) = R[c][r] */
        }
        out[c * 4 + 3] = 0.0f;
    }
    for (r = 0; r < 3; r++)
    {
        /* -(R^T t): note the transposed indexing, k*3+r */
        out[12 + r] = -(R[0 * 3 + r] * t[0] + R[1 * 3 + r] * t[1] + R[2 * 3 + r] * t[2]);
    }
    out[15] = 1.0f;
}

static void vrwTransformPoint(real32 const m[16], vector const* in, vector* out)
{
    out->x = m[0] * in->x + m[4] * in->y + m[8]  * in->z + m[12];
    out->y = m[1] * in->x + m[5] * in->y + m[9]  * in->z + m[13];
    out->z = m[2] * in->x + m[6] * in->y + m[10] * in->z + m[14];
}

static void vrwTransformDir(real32 const m[16], vector const* in, vector* out)
{
    out->x = m[0] * in->x + m[4] * in->y + m[8]  * in->z;
    out->y = m[1] * in->x + m[5] * in->y + m[9]  * in->z;
    out->z = m[2] * in->x + m[6] * in->y + m[10] * in->z;
}

/* also rejects NaN: neither comparison holds for it */
static bool32 vrwFinite(real32 v)
{
    return v > -1.0e18f && v < 1.0e18f;
}

static void vrwPathFollow(void);
static real32 vrwDistance(vector const* a, vector const* b);
static bool32 vrwOrdersBlocked(void);
static bool32 vrwPlayerShipSelectable(SpaceObjRotImpTarg const* obj);

/* Place the move cursor on a hand's ray at the current depth. Shared so the
   destination is consistent whether the ray moved or the depth did. */
static void vrwMoveRecompute(sdword hand)
{
    vrwray const* ray = &vrw.ray[hand];

    if (!ray->valid)
    {
        return;
    }
    vrw.moveDestination.x = ray->origin.x + ray->dir.x * vrw.cursorDist;
    vrw.moveDestination.y = ray->origin.y + ray->dir.y * vrw.cursorDist;
    vrw.moveDestination.z = ray->origin.z + ray->dir.z * vrw.cursorDist;
}

void vrWorldCaptureGameCamera(real32 const view[16])
{
    memcpy(vrw.gameCam, view, sizeof(vrw.gameCam));
    vrw.gameCamValid = TRUE;
    vrw.gameCamAge = 0;
}

udword vrWorldGameCameraAge(void)
{
    return vrw.gameCamValid ? vrw.gameCamAge : (udword)-1;
}

bool32 vrWorldFrameBegin(real32 const anchorPos[3], real32 const anchorQuat[4], real32 scale)
{
    real32 R[9], t[3];
    real32 const* look;
    real32 lookR[9], lookT[3];
    sdword r, c;

    vrw.worldValid = FALSE;
    if (!gameIsRunning || mrCamera == NULL)
    {
        return FALSE;
    }
    /* Never sample rndCameraMatrix directly here - see the file header. The
       captured matrix goes stale while a manager holds the main view down,
       which is correct: stale means "the last real main-view camera", and
       camera motion is suppressed for as long as a manager owns input. */
    if (vrw.gameCamValid)
    {
        look = vrw.gameCam;
        if (vrw.gameCamAge < (udword)-1)
        {
            vrw.gameCamAge++;
        }
    }
    else
    {
        look = (real32 const*)&rndCameraMatrix;
    }
    vrw.scale = scale;

    /* anchor inverse (anchor translation is stored in metres; the composed
       pipeline treats LOCAL as metres * scale, so scale it here too) */
    vrwQuatToMat3(anchorQuat, R);
    t[0] = anchorPos[0] * scale;
    t[1] = anchorPos[1] * scale;
    t[2] = anchorPos[2] * scale;
    vrwRigidInverse(R, t, vrw.anchorInv);

    /* lookat inverse: rndCameraMatrix is column-major (from glGetFloatv) */
    for (r = 0; r < 3; r++)
    {
        for (c = 0; c < 3; c++)
        {
            lookR[r * 3 + c] = look[c * 4 + r];             /* to row-major */
        }
        lookT[r] = look[12 + r];
    }
    vrwRigidInverse(lookR, lookT, vrw.lookatInv);

    /* forward transforms for placing panels at world positions */
    memcpy(vrw.lookatFwd, look, sizeof(vrw.lookatFwd));
    for (c = 0; c < 3; c++)
    {
        for (r = 0; r < 3; r++)
        {
            vrw.anchorFwd[c * 4 + r] = R[r * 3 + c];
        }
        vrw.anchorFwd[c * 4 + 3] = 0.0f;
    }
    vrw.anchorFwd[12] = t[0];
    vrw.anchorFwd[13] = t[1];
    vrw.anchorFwd[14] = t[2];
    vrw.anchorFwd[15] = 1.0f;

    vrw.worldValid = TRUE;
    vrwPathFollow();                        //advance any committed flight path

    /* periodic diagnostics: camera matrix health. Ray roundtrips are logged
       in vrWorldSetRay, after the current frame's ray has been transformed. */
    vrw.debugFrame++;
    if (vrw.debugFrame % VRW_DEBUG_INTERVAL == 1)
    {
        real32 lookDet;
        real32 anchorDet;

        lookDet = look[0] * (look[5] * look[10] - look[9] * look[6])
                - look[4] * (look[1] * look[10] - look[9] * look[2])
                + look[8] * (look[1] * look[6] - look[5] * look[2]);
        anchorDet = vrw.anchorFwd[0] * (vrw.anchorFwd[5] * vrw.anchorFwd[10]
                  - vrw.anchorFwd[9] * vrw.anchorFwd[6])
                  - vrw.anchorFwd[4] * (vrw.anchorFwd[1] * vrw.anchorFwd[10]
                  - vrw.anchorFwd[9] * vrw.anchorFwd[2])
                  + vrw.anchorFwd[8] * (vrw.anchorFwd[1] * vrw.anchorFwd[6]
                  - vrw.anchorFwd[5] * vrw.anchorFwd[2]);
        SDL_Log("VRDBG L: rot0=(%.3f %.3f %.3f) t=(%.1f %.1f %.1f) camEye=(%.1f %.1f %.1f)",
                look[0], look[4], look[8], look[12], look[13], look[14],
                mrCamera->eyeposition.x, mrCamera->eyeposition.y, mrCamera->eyeposition.z);
        SDL_Log("VRDBG CHAIN frame=%u lookDet=%.5f anchorDet=%.5f "
                "anchorPosM=(%.4f %.4f %.4f) q=(%.4f %.4f %.4f %.4f) "
                "camAge=%u manager=%s",
                (unsigned)vrw.debugFrame, lookDet, anchorDet,
                anchorPos[0], anchorPos[1], anchorPos[2],
                anchorQuat[0], anchorQuat[1], anchorQuat[2], anchorQuat[3],
                (unsigned)vrWorldGameCameraAge(), vrWorldManagerName());
        SDL_Log("VRDBG CAMERA frame=%u angle=%.6f decl=%.6f distance=%.2f "
                "userFlags=0x%x look=(%.1f %.1f %.1f)",
                (unsigned)vrw.debugFrame, mrCamera->angle,
                mrCamera->declination, mrCamera->distance,
                (unsigned)universe.mainCameraCommand.UserControlled,
                mrCamera->lookatpoint.x, mrCamera->lookatpoint.y,
                mrCamera->lookatpoint.z);
    }
    return TRUE;
}

bool32 vrWorldManagerActive(void)
{
    /* Sensors belongs here too: it is full-screen and clears
       mrRenderMainScreen like the rest, so without it the sensors map would
       be shown on the 32cm wrist panel. */
    return cmActive || lmActive || rmGUIActive || tmTraderGUIActive()
        || smSensorsActive;
}

char const* vrWorldManagerName(void)
{
    if (cmActive)            return "construction";
    if (lmActive)            return "launch";
    if (rmGUIActive)         return "research";
    if (tmTraderGUIActive()) return "trade";
    if (smSensorsActive)     return "sensors";
    return "none";
}

bool32 vrWorldCloseManagers(void)
{
    if (cmActive)        cmCloseIfOpen();
    if (lmActive)        lmCloseIfOpen();
    if (rmGUIActive)     rmCloseIfOpen();
    if (smSensorsActive) smSensorsCloseForGood();
    /* The trader GUI has no close entry point; its own panel button and the
       Escape key are the way out, so it is reported as still open here. */
    return !vrWorldManagerActive();
}

bool32 vrWorldManagerPanelAnchor(real32 outPosMetres[3], real32* outRadiusMetres)
{
    vector world, cam, local;
    Ship* anchorShip = NULL;
    real32 radius;

    if (!vrw.worldValid || !vrWorldManagerActive() || vrw.scale <= 0.0f)
    {
        return FALSE;
    }
    if (selSelected.numShips > 0
        && !(selSelected.ShipPtr[0]->flags & SOF_Dead))
    {
        anchorShip = selSelected.ShipPtr[0];
    }
    else if (cmActive && universe.curPlayerPtr != NULL
             && universe.curPlayerPtr->PlayerMothership != NULL)
    {
        anchorShip = universe.curPlayerPtr->PlayerMothership;
    }

    if (anchorShip != NULL)
    {
        world = anchorShip->collInfo.collPosition;
        radius = anchorShip->staticinfo->staticheader.staticCollInfo.collspheresize
               / vrw.scale;
    }
    else
    {
        world = selCentrePoint;
        radius = 0.10f;
    }
    vrwTransformPoint(vrw.lookatFwd, &world, &cam);
    vrwTransformPoint(vrw.anchorFwd, &cam, &local);
    if (!vrwFinite(local.x) || !vrwFinite(local.y) || !vrwFinite(local.z)
        || !vrwFinite(radius))
    {
        SDL_Log("VR: manager anchor rejected: world=(%.1f %.1f %.1f) "
                "local=(%.1f %.1f %.1f) radius=%.3f camAge=%u",
                world.x, world.y, world.z, local.x, local.y, local.z,
                radius, (unsigned)vrWorldGameCameraAge());
        return FALSE;
    }
    outPosMetres[0] = local.x / vrw.scale;
    outPosMetres[1] = local.y / vrw.scale;
    outPosMetres[2] = local.z / vrw.scale;
    *outRadiusMetres = radius < 0.0f ? 0.0f : (radius > 20.0f ? 20.0f : radius);
    return TRUE;
}

/*-----------------------------------------------------------------------------
    Command wheel dispatch

    Every leaf here goes through a seam the mouse already uses: the mr*
    right-click-menu callbacks (all of which guard atom != NULL, so (NULL,
    NULL) is safe), or clWrap* for the orders with no menu entry. Never a
    synthesized keystroke, and never a bare cl* - CommandWrap.c is what
    marshals multiplayer packets and recordings.
----------------------------------------------------------------------------*/
static TypeOfFormation const vrwCommandFormation[] = {
    DELTA_FORMATION, BROAD_FORMATION, DELTA3D_FORMATION, CLAW_FORMATION,
    WALL_FORMATION, SPHERE_FORMATION, CUSTOM_FORMATION
};

static bool32 vrwCommandIsFormation(vrworldcommand cmd)
{
    return cmd >= VRW_CMD_FORM_DELTA && cmd <= VRW_CMD_FORM_CUSTOM;
}

static TacticsType vrwCommandTactic(vrworldcommand cmd)
{
    return (TacticsType)(Evasive + (cmd - VRW_CMD_TACTIC_EVASIVE));
}

sdword vrWorldGroupSize(sdword group)
{
    if (group < 0 || group >= SEL_NumberHotKeyGroups)
    {
        return 0;
    }
    return selHotKeyGroup[group].numShips;
}

/* The wheel's special-ability wedge fires a target-less special, which a
   Salvage Corvette cannot perform - salvaging needs something to salvage.
   clWrapSpecial now drops them rather than dereferencing the NULL target
   list, so this is no longer a crash, but a selection of nothing but
   salcaps would still be a wedge that lights up and does nothing. Ask the
   same question the order does, so it dims instead. */
static void vrwDropSalvageCorvettes(SelectCommand* selection)
{
    sdword i;

    for (i = 0; i < selection->numShips;)
    {
        if (selection->ShipPtr[i]->shiptype == SalCapCorvette)
        {
            selection->numShips--;
            selection->ShipPtr[i] = selection->ShipPtr[selection->numShips];
            continue;
        }
        i++;
    }
}

bool32 vrWorldCommandEnabled(vrworldcommand cmd, sdword arg)
{
    udword mask;
    MaxSelection capable;

    if (!vrw.worldValid)
    {
        return FALSE;
    }
    /* Navigation, and the full-screen managers, stay available at all times.
       The managers are deliberately NOT gated on the MAM_* action mask: those
       bits describe what a right-click on one particular ship should offer,
       where that ship is the context. A global wheel has no such context, and
       the keyboard equivalents prove the point - mrResearch takes no selection
       at all, and mrBuildShips and mrLaunch both fall back to the player's own
       mothership. Masking them meant Research simply could not be opened with
       a fighter selected. */
    switch (cmd)
    {
        case VRW_CMD_SELECT_ALL:
        case VRW_CMD_MOTHERSHIP:
        case VRW_CMD_FOCUS_NEXT:
        case VRW_CMD_FOCUS_PREV:
        case VRW_CMD_SENSORS:
        case VRW_CMD_UNDO:
        case VRW_CMD_SCALE_UP:
        case VRW_CMD_SCALE_DOWN:
        /* never gated: this is how the player reaches save, options and quit,
           and it is the wheel's job to always offer a way out */
        case VRW_CMD_MENU:
            return TRUE;
        case VRW_CMD_BUILD:
        case VRW_CMD_LAUNCH:
        case VRW_CMD_RESEARCH:
            return !vrwOrdersBlocked();
        case VRW_CMD_GROUP_RECALL:
            return vrWorldGroupSize(arg) > 0;
        case VRW_CMD_GROUP_STORE:
            return selSelected.numShips > 0;
        default:
            break;
    }
    if (selSelected.numShips == 0)
    {
        return FALSE;
    }

    mask = mrSelectionActionMask();
    if (vrwCommandIsFormation(cmd))
    {
        return (mask & MAM_Formations) != 0
            && selSelected.numShips >= MIN_SHIPS_IN_FORMATION;
    }
    switch (cmd)
    {
        case VRW_CMD_TACTIC_EVASIVE:
        case VRW_CMD_TACTIC_NEUTRAL:
        case VRW_CMD_TACTIC_AGGRESSIVE: return (mask & MAM_Tactics) != 0;
        /* Ask the same question the order itself asks, rather than trusting
           the ship-type menu bits as a proxy. MAM_Harvest is set for exactly
           one hull in mrMenuActionsByShipType, so anything else that can
           collect would have been dimmed despite the order being legal. */
        case VRW_CMD_HARVEST:
            return MakeShipsHarvestCapable((SelectCommand*)&capable,
                                           (SelectCommand*)&selSelected) != 0;
        case VRW_CMD_DOCK:
            capable = selSelected;
            makeShipsDockCapable((SelectCommand*)&capable);
            return capable.numShips > 0;
        case VRW_CMD_RETIRE:            return (mask & MAM_Retire) != 0;
        case VRW_CMD_SCUTTLE:           return (mask & MAM_Scuttle) != 0;
        case VRW_CMD_HYPERSPACE:        return (mask & MAM_Hyperspace) != 0;
        case VRW_CMD_KAMIKAZE:
            if (vrwOrdersBlocked())
            {
                return FALSE;
            }
            capable = selSelected;
            return MakeSelectionKamikazeCapable((SelectCommand*)&capable) != 0;
        case VRW_CMD_SPECIAL:
            if (vrwOrdersBlocked())
            {
                return FALSE;
            }
            capable = selSelected;
            MakeShipsSpecialActivateCapable((SelectCommand*)&capable);
            vrwDropSalvageCorvettes((SelectCommand*)&capable);
            return capable.numShips > 0;
        case VRW_CMD_HALT:              return !vrwOrdersBlocked();
        default:                        return FALSE;
    }
}

bool32 vrWorldCommandActive(vrworldcommand cmd)
{
    if (!vrw.worldValid || selSelected.numShips == 0)
    {
        return FALSE;
    }
    if (vrwCommandIsFormation(cmd))
    {
        return clSelectionAlreadyInFormation(&universe.mainCommandLayer,
                                            (SelectCommand*)&selSelected)
            == vrwCommandFormation[cmd - VRW_CMD_FORM_DELTA];
    }
    if (cmd >= VRW_CMD_TACTIC_EVASIVE && cmd <= VRW_CMD_TACTIC_AGGRESSIVE)
    {
        return selSelected.ShipPtr[0]->tacticstype == vrwCommandTactic(cmd);
    }
    return FALSE;
}

/* "Select all visible" cannot borrow the keyboard path: mainrgn's E key runs
   selRectSelect over a full-screen rect against the mono camera, which in VR
   is an invisible frustum unrelated to what the player sees. Select the
   player's selectable ships out of the render list instead. */
static bool32 vrwSelectAllVisible(void)
{
    Node* node;
    bool32 changed = FALSE;

    for (node = universe.RenderList.head;
         node != NULL && selSelected.numShips < COMMAND_MAX_SHIPS;
         node = node->next)
    {
        SpaceObjRotImpTarg* obj = (SpaceObjRotImpTarg*)listGetStructOfNode(node);

        if (vrwPlayerShipSelectable(obj)
            && !selShipInSelection(selSelected.ShipPtr, selSelected.numShips,
                                   (Ship*)obj))
        {
            selSelectionAddSingleShip(&selSelected, (Ship*)obj);
            changed = TRUE;
        }
    }
    if (changed)
    {
        ioUpdateShipTotals();
        tutGameMessage("Game_SelectingRect");
    }
    return changed;
}

static bool32 vrwGroupStore(sdword group)
{
    if (group < 0 || group >= SEL_NumberHotKeyGroups
        || selSelected.numShips == 0)
    {
        return FALSE;
    }
    selHotKeyGroupRemoveReferences(group);
    selSelectionCopy((MaxAnySelection*)&selHotKeyGroup[group],
                     (MaxAnySelection*)&selSelected);
    selHotKeyNumbersSet(group);
    return TRUE;
}

static bool32 vrwGroupRecall(sdword group)
{
    if (vrWorldGroupSize(group) == 0)
    {
        return FALSE;
    }
    selSelectHotKeyGroup(&selHotKeyGroup[group]);
    selHotKeyNumbersSet(group);
    ioUpdateShipTotals();
    return TRUE;
}

bool32 vrWorldCommand(vrworldcommand cmd, sdword arg)
{
    MaxSelection capable;

    if (!vrWorldCommandEnabled(cmd, arg))
    {
        SDL_Log("VR: wheel command %d rejected (arg=%d)", (int)cmd, (int)arg);
        return FALSE;
    }
    if (vrwCommandIsFormation(cmd))
    {
        /* mrSetTheFormation, not the TAB path: TAB only stages a name and a
           timer, committing later from mrRegionDraw after MR_FormationDelay.
           This applies it now, parade special-case and speech included. */
        mrSetTheFormation(vrwCommandFormation[cmd - VRW_CMD_FORM_DELTA]);
        return TRUE;
    }

    switch (cmd)
    {
        case VRW_CMD_TACTIC_EVASIVE:    mrEvasiveTactics(NULL, NULL);   break;
        case VRW_CMD_TACTIC_NEUTRAL:    mrNeutralTactics(NULL, NULL);   break;
        case VRW_CMD_TACTIC_AGGRESSIVE: mrAgressiveTactics(NULL, NULL); break;

        /* Both of these run makeShipsControllable on the selection they are
           given, which TRIMS IT IN PLACE - so they get a copy, or issuing the
           order would quietly delete ships from the player's own selection. */
        case VRW_CMD_HALT:
            capable = selSelected;
            clWrapHalt(&universe.mainCommandLayer, (SelectCommand*)&capable);
            break;
        case VRW_CMD_SPECIAL:
            /* Self-activate, as Z-release does - and exactly as it does it.
               Passing the raw selection with NULL targets segfaulted inside
               clWrapSpecial: ships with no special ability reached
               ability-specific code. The keyboard path copies and then filters
               with MakeShipsSpecialActivateCapable for precisely that reason,
               and checks again afterwards because the filter can empty it.
               Salvage Corvettes survive that filter - they do have a special
               activate - but theirs is salvage, which needs a target, so they
               come out too. See vrwDropSalvageCorvettes. */
            capable = selSelected;
            MakeShipsSpecialActivateCapable((SelectCommand*)&capable);
            vrwDropSalvageCorvettes((SelectCommand*)&capable);
            if (capable.numShips == 0)
            {
                return FALSE;
            }
            tutGameMessage("KB_Special");
            clWrapSpecial(&universe.mainCommandLayer, (SelectCommand*)&capable,
                          NULL);
            break;
        case VRW_CMD_KAMIKAZE:
            capable = selSelected;
            if (!MakeSelectionKamikazeCapable((SelectCommand*)&capable))
            {
                return FALSE;
            }
            clWrapSetKamikaze(&universe.mainCommandLayer,
                              (SelectCommand*)&capable);
            break;

        case VRW_CMD_HARVEST:    mrHarvestResources(NULL, NULL); break;
        case VRW_CMD_DOCK:       mrDockingOrders(NULL, NULL);    break;
        case VRW_CMD_RETIRE:     mrRetire(NULL, NULL);           break;
        case VRW_CMD_SCUTTLE:    mrScuttle(NULL, NULL);          break;

        /* Managers are mutually exclusive, and the mr* entry points do not
           close each other, so switching straight from one to another has to
           close the incumbent first - otherwise the request is swallowed and
           the player is stuck in whichever manager they already had open. */
        case VRW_CMD_BUILD:
            vrWorldCloseManagers();
            mrBuildShips(NULL, NULL);
            break;
        case VRW_CMD_LAUNCH:
            vrWorldCloseManagers();
            mrLaunch(NULL, NULL);
            break;
        case VRW_CMD_RESEARCH:
            vrWorldCloseManagers();
            mrResearch(NULL, NULL);
            break;
        case VRW_CMD_HYPERSPACE: mrHyperspace(NULL, NULL);       break;

        case VRW_CMD_SELECT_ALL: return vrwSelectAllVisible();
        case VRW_CMD_MOTHERSHIP:
            ccFocusOnMyMothership(&universe.mainCameraCommand);
            break;
        case VRW_CMD_FOCUS_NEXT:
            ccForwardFocus(&universe.mainCameraCommand);
            break;
        case VRW_CMD_FOCUS_PREV:
            ccCancelFocus(&universe.mainCameraCommand);
            break;
        case VRW_CMD_SENSORS:
            /* Not the manager. The map's whole content is already geometry in
               game-world space, and the world is already a hologram in the
               room, so the sensor view is a change of representation rather
               than a screen to open - which is what the design document
               argues for and what leaves every world verb working, since
               smSensorsActive is what gates them off.

               The manager still exists and still runs for mission briefings,
               which KAS opens directly through kasfOpenSensors. */
            vrWorldCloseManagers();
            vrWorldToggleSensors();
            break;
        case VRW_CMD_UNDO:       udLatestThingUndo();            break;

        case VRW_CMD_GROUP_RECALL: return vrwGroupRecall(arg);
        case VRW_CMD_GROUP_STORE:  return vrwGroupStore(arg);

        default:
            return FALSE;
    }
    SDL_Log("VR: wheel command %d issued (arg=%d, selection=%d)", (int)cmd,
            (int)arg, (int)selSelected.numShips);
    return TRUE;
}

/*-----------------------------------------------------------------------------
    Rays and picking
----------------------------------------------------------------------------*/
static void vrwLocalToWorld(real32 const posMetres[3], vector* out)
{
    vector local, cam;

    local.x = posMetres[0] * vrw.scale;
    local.y = posMetres[1] * vrw.scale;
    local.z = posMetres[2] * vrw.scale;
    vrwTransformPoint(vrw.anchorInv, &local, &cam);
    vrwTransformPoint(vrw.lookatInv, &cam, out);
}

static void vrwLocalDirToWorld(real32 const dir[3], vector* out)
{
    vector local, cam;
    real32 mag;

    local.x = dir[0];
    local.y = dir[1];
    local.z = dir[2];
    vrwTransformDir(vrw.anchorInv, &local, &cam);
    vrwTransformDir(vrw.lookatInv, &cam, out);
    mag = fsqrt(out->x * out->x + out->y * out->y + out->z * out->z);
    if (mag > 0.0f)
    {
        out->x /= mag; out->y /= mag; out->z /= mag;
    }
}

/* Something this player may be ordered to shoot at. Mirrors the enemy branch
   of vrWorldContextIntent; the game's own MakeTargetsOnlyNonForceAttackTargets
   does the final pruning at commit. */
static bool32 vrwAttackable(SpaceObjRotImpTarg const* obj)
{
    if (obj == NULL || (obj->flags & SOF_Dead))
    {
        return FALSE;
    }
    if (obj->objtype == OBJ_DerelictType)
    {
        return TRUE;
    }
    return obj->objtype == OBJ_ShipType
        && ((Ship const*)obj)->playerowner != universe.curPlayerPtr
        && !allianceIsShipAlly((Ship*)obj, universe.curPlayerPtr);
}

static bool32 vrwPlayerShipSelectable(SpaceObjRotImpTarg const* obj)
{
    udword const blocked = SOF_Dead | SOF_Hide | SOF_Disabled
                         | SOF_Crazy | SOF_Hyperspace;

    return obj != NULL && obj->objtype == OBJ_ShipType
        && ((Ship const*)obj)->playerowner == universe.curPlayerPtr
        && ((Ship const*)obj)->shiptype != Drone
        && (obj->flags & SOF_Selectable)
        && !(obj->flags & blocked);
}

/* Best ray/sphere hit along the render list. selectableOnly restricts to the
   current player's selectable ships.

   Candidates are ranked by how directly the beam points at their centre, in
   plain angle, with depth only breaking near-ties. Two earlier attempts got
   this wrong in instructive ways:

   Ranking by the depth at which the ray ENTERS the sphere fails because a
   large sphere's surface begins far ahead of its centre - the Mothership's
   hull starts some 2600 units in front of its own middle, so it beat every
   fighter parked ahead of it however precisely the beam was on the fighter.

   Ranking by centre depth with a penalty for miss distance expressed as a
   FRACTION OF THE OBJECT'S OWN RADIUS fails for the same underlying reason,
   just less obviously. A Mothership's effective radius is around 2800 units,
   so nearly any aim is "well centred" for it, while a fighter's is under 100
   and a 30-unit tremor already reads as badly off. Normalising by size
   therefore favours whatever is biggest - the precise opposite of what is
   needed. Absolute angle does not care how big the target is, which is what
   makes it match the player's intent: the ship they are pointing most
   directly at is the ship they mean. */
/* How big this object should be treated as, for anything that answers "did
   you mean this one" - the ray test, the sweep brush, and every ring drawn to
   show what is hovered, targeted or selected. All of them go through here so
   they cannot disagree with each other, which is what happened when the
   Mothership's selection ring was shrunk on its own and the hover ring around
   it stayed full size. See VRW_MOTHERSHIP_SCALE for why it is special.

   Not for anything about the ship's actual extent: waypoint arrival distance
   and panel anchoring want the real sphere. */
static real32 vrwHullRadius(SpaceObjRotImpTarg const* obj)
{
    real32 hull = obj->staticinfo->staticheader.staticCollInfo.collspheresize;

    if (obj->objtype == OBJ_ShipType
        && ((Ship const*)obj)->shiptype == Mothership)
    {
        hull *= VRW_MOTHERSHIP_SCALE;
    }
    return hull;
}

static SpaceObjRotImpTarg* vrwPick(vrwray const* ray, bool32 selectableOnly,
                                   SpaceObjRotImpTarg const* preferred, real32* hitT)
{
    Node* node;
    SpaceObjRotImpTarg* best = NULL;
    real32 bestAngle = REALlyBig;
    real32 bestT = REALlyBig;
    real32 bestSurface = 0.0f;

    for (node = universe.RenderList.head; node != NULL; node = node->next)
    {
        SpaceObjRotImpTarg* obj = (SpaceObjRotImpTarg*)listGetStructOfNode(node);
        vector toObj;
        real32 radius, tCentre, distSqr, discr, root;
        real32 hull, margin, assist, angle;
        bool32 better;

        if (obj->objtype != OBJ_ShipType && obj->objtype != OBJ_AsteroidType
            && obj->objtype != OBJ_DustType && obj->objtype != OBJ_GasType
            && obj->objtype != OBJ_DerelictType)
        {
            continue;
        }
        if (selectableOnly)
        {
            if (!vrwPlayerShipSelectable(obj))
            {
                continue;
            }
        }
        if (obj->flags & SOF_Dead)
        {
            continue;
        }

        vecSub(toObj, obj->collInfo.collPosition, ray->origin);
        tCentre = vecDotProduct(toObj, ray->dir);
        if (tCentre < 0.0f)
        {
            continue;                                       //behind the controller
        }

        /* Aim assists are added, not multiplied, and capped in absolute
           units. A 25% margin is a handful of units on a fighter and a
           650-unit dead zone around the Mothership, which is precisely how
           a capital ship ends up swallowing its own escorts. */
        hull = vrwHullRadius(obj);
        margin = hull * (VRW_PICK_MARGIN - 1.0f);
        if (margin > VRW_PICK_MARGIN_MAX)
        {
            margin = VRW_PICK_MARGIN_MAX;
        }
        assist = tCentre * VRW_PICK_CONE_TAN;
        if (assist > VRW_PICK_CONE_MAX)
        {
            assist = VRW_PICK_CONE_MAX;
        }
        radius = hull + margin + assist;

        distSqr = vecMagnitudeSquared(toObj) - tCentre * tCentre;
        discr = radius * radius - distSqr;
        if (discr < 0.0f)
        {
            continue;
        }
        root = fsqrt(discr);

        /* angle between the beam and this object's centre, in radians */
        angle = fsqrt(distSqr) / (tCentre > 1.0f ? tCentre : 1.0f);
        if (obj == preferred)
        {
            /* hysteresis in the ranking rather than the geometry: inflating
               the previous target's sphere made big ships stickier than small
               ones, which is backwards */
            angle -= VRW_PICK_STICKY_ANGLE;
            if (angle < 0.0f)
            {
                angle = 0.0f;
            }
        }
        /* clearly better aim wins; among aims too close to tell apart, the
           nearer centre wins, so a ship in front beats one behind it */
        better = angle < bestAngle - VRW_PICK_TIE_ANGLE
              || (angle < bestAngle + VRW_PICK_TIE_ANGLE && tCentre < bestT);
        if (better)
        {
            if (angle < bestAngle)
            {
                bestAngle = angle;
            }
            bestT = tCentre;
            best = obj;
            /* the drawn beam still wants the surface, not the centre. The
               controller can sit inside a capital ship's sphere, so fall
               through to the forward exit rather than a negative distance
               that would draw the ray back through the hand. */
            bestSurface = tCentre - root;
            if (bestSurface < 0.0f)
            {
                bestSurface = tCentre + root;
            }
        }
    }

    if (best != NULL && hitT != NULL)
    {
        *hitT = bestSurface;
    }
    return best;
}

/*-----------------------------------------------------------------------------
    Name        : vrwSensorsPick
    Description : Ray against the sensors map: blobs first, then the ships
                  inside whichever blob was hit.

                  The map is not the render list. Its objects are the
                  collision blobs, most of which the player cannot see into,
                  and a blob is a sphere already - so unlike vrwPick there is
                  no hull to inflate and no aim assist to apply. Blobs are
                  thousands of units across; the beam either enters one or it
                  does not.

                  Second tier: once the ray is inside the winning blob, the
                  ships in it are what the player means. That is the whole
                  reason the desktop map has a band box and a click - one
                  picks the region, the other picks what is in it.
    Inputs      : ray - a ray already in game-world space
                  outShip - filled with the ship under the ray inside the
                      chosen blob, or NULL when the ray is outside it
                  hitT - ray parameter of the surface hit, for the drawn beam
    Outputs     :
    Return      : the blob under the ray, or NULL
----------------------------------------------------------------------------*/
static blob* vrwSensorsPick(vrwray const* ray, SpaceObjRotImpTarg** outShip,
                            real32* hitT)
{
    Node* node;
    blob* best = NULL;
    real32 bestAngle = REALlyBig;
    real32 bestT = REALlyBig;
    real32 bestSurface = 0.0f;
    bool32 bestInside = FALSE;
    sdword sensorLevel = universe.curPlayerPtr->sensorLevel;

    if (outShip != NULL)
    {
        *outShip = NULL;
    }

    for (node = universe.collBlobList.head; node != NULL; node = node->next)
    {
        blob* thisBlob = (blob*)listGetStructOfNode(node);
        vector toBlob;
        real32 tCentre, distSqr, discr, root, angle, surface;
        bool32 inside, better;

        /* Exactly what smBlobsDraw draws a shell for. Pointing at a blob
           that is not on the map would select through the fog. */
        if (!((thisBlob->flags & (BTF_Explored | BTF_ProbeDroid))
              || (sensorLevel == 2
                  && bitTest(thisBlob->flags, BTF_UncloakedEnemies))))
        {
            continue;
        }

        vecSub(toBlob, thisBlob->centre, ray->origin);
        distSqr = vecMagnitudeSquared(toBlob);
        inside = (distSqr <= thisBlob->radius * thisBlob->radius);
        tCentre = vecDotProduct(toBlob, ray->dir);
        if (tCentre < 0.0f && !inside)
        {
            continue;                                       //behind the hand
        }

        distSqr -= tCentre * tCentre;
        discr = thisBlob->radius * thisBlob->radius - distSqr;
        if (discr < 0.0f)
        {
            continue;
        }
        root = fsqrt(discr);
        surface = tCentre - root;
        if (surface < 0.0f)
        {
            surface = tCentre + root;                       //hand is within
        }

        angle = fsqrt(distSqr > 0.0f ? distSqr : 0.0f)
              / (tCentre > 1.0f ? tCentre : 1.0f);

        /* A blob the hand is standing in beats every blob it merely points
           through, however well aimed. Otherwise walking into a sphere would
           hand the pick to whatever lay beyond it. */
        better = (inside && !bestInside)
              || (inside == bestInside
                  && (angle < bestAngle - VRW_PICK_TIE_ANGLE
                      || (angle < bestAngle + VRW_PICK_TIE_ANGLE
                          && tCentre < bestT)));
        if (better)
        {
            bestAngle = angle;
            bestT = tCentre;
            best = thisBlob;
            bestSurface = surface;
            bestInside = inside;
        }
    }

    if (best == NULL)
    {
        return NULL;
    }
    if (hitT != NULL)
    {
        *hitT = bestSurface;
    }

    /* Second tier. Only from inside: a blob seen from across the map is a
       region, and resolving it to one of the ships stacked along the line of
       sight would be a guess the player cannot see well enough to correct. */
    if (bestInside && outShip != NULL && best->blobObjects != NULL)
    {
        SpaceObjRotImpTarg* bestShip = NULL;
        real32 shipAngle = REALlyBig;
        real32 shipT = REALlyBig;
        sdword index;

        for (index = 0; index < best->blobObjects->numSpaceObjs; index++)
        {
            SpaceObj* obj = best->blobObjects->SpaceObjPtr[index];
            SpaceObjRotImpTarg* targ = (SpaceObjRotImpTarg*)obj;
            vector toObj;
            real32 radius, tc, dSqr, ang;

            if (obj->objtype != OBJ_ShipType && obj->objtype != OBJ_AsteroidType
                && obj->objtype != OBJ_DustType && obj->objtype != OBJ_GasType
                && obj->objtype != OBJ_DerelictType)
            {
                continue;
            }
            if (obj->flags & (SOF_Dead | SOF_Hide))
            {
                continue;
            }

            vecSub(toObj, targ->collInfo.collPosition, ray->origin);
            tc = vecDotProduct(toObj, ray->dir);
            if (tc < 0.0f)
            {
                continue;
            }
            /* The map draws ships as points, so there is no hull on screen
               to aim at and the pick has to be a cone about the beam. */
            radius = vrwHullRadius(targ);
            radius += tc * VRW_PICK_CONE_TAN;
            dSqr = vecMagnitudeSquared(toObj) - tc * tc;
            if (radius * radius - dSqr < 0.0f)
            {
                continue;
            }
            ang = fsqrt(dSqr > 0.0f ? dSqr : 0.0f) / (tc > 1.0f ? tc : 1.0f);
            if (ang < shipAngle - VRW_PICK_TIE_ANGLE
                || (ang < shipAngle + VRW_PICK_TIE_ANGLE && tc < shipT))
            {
                shipAngle = ang;
                shipT = tc;
                bestShip = targ;
            }
        }
        *outShip = bestShip;
    }

    return best;
}

/* Remember where the brush has been, so the overlay can show the region
   actually swept rather than only which objects it happened to catch. Shared
   by both sweeps - selecting ships and picking attack targets use the same
   brush, so they should look the same while in progress. */
static void vrwSweepTrailRecord(vrwray const* ray)
{
    vector far;

    if (!(vrw.sweepReach > 0.0f))
    {
        return;
    }
    far.x = ray->origin.x + ray->dir.x * vrw.sweepReach;
    far.y = ray->origin.y + ray->dir.y * vrw.sweepReach;
    far.z = ray->origin.z + ray->dir.z * vrw.sweepReach;
    if (vrw.sweepTrailCount == 0
        || vrwDistance(&far, &vrw.sweepTrail[vrw.sweepTrailCount - 1])
           > vrw.sweepReach * 0.02f)
    {
        if (vrw.sweepTrailCount >= VRW_SWEEP_TRAIL)
        {
            sdword i;

            /* keep the whole stroke by halving resolution, as the path
               sampler does, rather than dropping the start of it */
            for (i = 0; i * 2 < vrw.sweepTrailCount; i++)
            {
                vrw.sweepTrail[i] = vrw.sweepTrail[i * 2];
            }
            vrw.sweepTrailCount = i;
        }
        vrw.sweepTrail[vrw.sweepTrailCount++] = far;
    }
}

/* Sweep-select brush. This is the VR stand-in for the desktop band-box, so
   it behaves like one: every selectable player ship the beam passes over
   joins the preview - not just the nearest - and the capture radius widens
   with distance exactly as a screen-space box does with depth. Painting
   across a cluster therefore takes the whole cluster in one pass. */
static void vrwSweepAccumulate(vrwray const* ray)
{
    Node* node;

    vrwSweepTrailRecord(ray);

    for (node = universe.RenderList.head;
         node != NULL && vrw.sweepPreview.numShips < COMMAND_MAX_SHIPS;
         node = node->next)
    {
        SpaceObjRotImpTarg* obj = (SpaceObjRotImpTarg*)listGetStructOfNode(node);
        vector toObj;
        real32 radius, tCentre, distSqr;

        if (!vrwPlayerShipSelectable(obj))
        {
            continue;
        }
        vecSub(toObj, obj->collInfo.collPosition, ray->origin);
        tCentre = vecDotProduct(toObj, ray->dir);
        if (tCentre < 0.0f)
        {
            continue;                                       //behind the controller
        }
        radius = vrwHullRadius(obj);
        {
            real32 margin = radius * (VRW_SWEEP_MARGIN - 1.0f);

            /* same absolute cap as aiming: otherwise brushing anywhere near a
               capital ship always drags it into the group */
            radius += margin > VRW_PICK_MARGIN_MAX ? VRW_PICK_MARGIN_MAX : margin;
        }
        radius += tCentre * VRW_SWEEP_CONE_TAN;
        distSqr = vecMagnitudeSquared(toObj) - tCentre * tCentre;
        if (distSqr > radius * radius)
        {
            continue;
        }
        if (!selShipInSelection(vrw.sweepPreview.ShipPtr,
                                vrw.sweepPreview.numShips, (Ship*)obj))
        {
            vrw.sweepPreview.ShipPtr[vrw.sweepPreview.numShips++] = (Ship*)obj;
        }
    }
}

/* Same brush as selection sweeping, aimed at things to shoot rather than
   things to command, so "paint over them" feels identical either way. */
static void vrwTargetSweepAccumulate(vrwray const* ray)
{
    Node* node;
    sdword i;

    vrwSweepTrailRecord(ray);
    for (node = universe.RenderList.head;
         node != NULL && vrw.targetSweep.numTargets < VRW_MAX_SWEEP_TARGETS;
         node = node->next)
    {
        SpaceObjRotImpTarg* obj = (SpaceObjRotImpTarg*)listGetStructOfNode(node);
        vector toObj;
        real32 radius, tCentre, distSqr, margin;
        bool32 already = FALSE;

        if (!vrwAttackable(obj))
        {
            continue;
        }
        vecSub(toObj, obj->collInfo.collPosition, ray->origin);
        tCentre = vecDotProduct(toObj, ray->dir);
        if (tCentre < 0.0f)
        {
            continue;
        }
        radius = vrwHullRadius(obj);
        margin = radius * (VRW_SWEEP_MARGIN - 1.0f);
        radius += margin > VRW_PICK_MARGIN_MAX ? VRW_PICK_MARGIN_MAX : margin;
        radius += tCentre * VRW_SWEEP_CONE_TAN;
        distSqr = vecMagnitudeSquared(toObj) - tCentre * tCentre;
        if (distSqr > radius * radius)
        {
            continue;
        }
        for (i = 0; i < vrw.targetSweep.numTargets; i++)
        {
            if (vrw.targetSweep.TargetPtr[i] == obj)
            {
                already = TRUE;
                break;
            }
        }
        if (!already)
        {
            vrw.targetSweep.TargetPtr[vrw.targetSweep.numTargets++] = obj;
        }
    }
}

bool32 vrWorldSetRay(sdword hand, real32 const origin[3], real32 const dir[3], bool32 valid)
{
    vrwray* ray = &vrw.ray[hand];
    SpaceObjRotImpTarg* oldHover = ray->hover;

    /* One choke point for hiding the hands during a mission briefing: an
       invalid ray draws nothing, picks nothing, and so pulses nothing. */
    ray->valid = valid && vrw.worldValid && !vrWorldSensorsBriefing();
    ray->hover = NULL;
    ray->hoverBlob = NULL;
    ray->limitT = 0.0f;
    if (!ray->valid)
    {
        return FALSE;
    }
    vrw.dbgLocalPos[hand].x = origin[0];
    vrw.dbgLocalPos[hand].y = origin[1];
    vrw.dbgLocalPos[hand].z = origin[2];
    vrw.dbgLocalDir[hand].x = dir[0];
    vrw.dbgLocalDir[hand].y = dir[1];
    vrw.dbgLocalDir[hand].z = dir[2];
    vrwLocalToWorld(origin, &ray->origin);
    vrwLocalDirToWorld(dir, &ray->dir);
    if (vrWorldSensorsActive())
    {
        /* The map has its own contents. Picking the render list here would
           aim at ships drawn nowhere the player is looking - the main view
           is not even rendering while a manager holds it down. */
        ray->hoverBlob = vrwSensorsPick(ray, &ray->hover, &ray->hoverT);
        if (ray->hoverBlob == NULL)
        {
            ray->hoverT = 0.0f;
        }
    }
    else
    {
        ray->hover = vrwPick(ray, FALSE, oldHover, &ray->hoverT);
        /* With the overlay up the ships are still drawn and still the thing
           orders act on, so ship picking stays exactly as it was. The blob
           is picked alongside it, for the shell highlight and for region
           work - it never displaces a ship the player is aiming at. */
        if (vrw.sensorsOverlay)
        {
            vrw.ray[hand].hoverBlob = vrwSensorsPick(ray, NULL, NULL);
        }
    }
    if (hand == 1 && vrw.debugFrame % VRW_DEBUG_INTERVAL == 1)
    {
        vector backCam, backLocal, backCamDir, backLocalDir;
        real32 backMag, dirDot;

        vrwTransformPoint(vrw.lookatFwd, &ray->origin, &backCam);
        vrwTransformPoint(vrw.anchorFwd, &backCam, &backLocal);
        vrwTransformDir(vrw.lookatFwd, &ray->dir, &backCamDir);
        vrwTransformDir(vrw.anchorFwd, &backCamDir, &backLocalDir);
        backMag = fsqrt(backLocalDir.x * backLocalDir.x
                      + backLocalDir.y * backLocalDir.y
                      + backLocalDir.z * backLocalDir.z);
        if (backMag > 0.0f)
        {
            backLocalDir.x /= backMag;
            backLocalDir.y /= backMag;
            backLocalDir.z /= backMag;
        }
        dirDot = backLocalDir.x * vrw.dbgLocalDir[1].x
               + backLocalDir.y * vrw.dbgLocalDir[1].y
               + backLocalDir.z * vrw.dbgLocalDir[1].z;
        SDL_Log("VRDBG RAY frame=%u local=(%.4f %.4f %.4f) "
                "minusZ=(%.4f %.4f %.4f) world=(%.1f %.1f %.1f) "
                "dirW=(%.4f %.4f %.4f) hover=%d hitT=%.2f",
                (unsigned)vrw.debugFrame, origin[0], origin[1], origin[2],
                dir[0], dir[1], dir[2], ray->origin.x, ray->origin.y,
                ray->origin.z, ray->dir.x, ray->dir.y, ray->dir.z,
                ray->hover != NULL, ray->hover != NULL ? ray->hoverT : -1.0f);
        SDL_Log("VRDBG ROUNDTRIP frame=%u local*S=(%.2f %.2f %.2f) "
                "back=(%.2f %.2f %.2f) dirBack=(%.5f %.5f %.5f) dot=%.7f",
                (unsigned)vrw.debugFrame, origin[0] * vrw.scale,
                origin[1] * vrw.scale, origin[2] * vrw.scale,
                backLocal.x, backLocal.y, backLocal.z,
                backLocalDir.x, backLocalDir.y, backLocalDir.z, dirDot);
    }

    if (vrw.sweepActive && hand == vrw.sweepHand)
    {
        vrwSweepAccumulate(ray);
    }
    if (vrw.targetSweepActive && vrw.targetSweepPainting
        && hand == vrw.targetSweepHand)
    {
        vrwTargetSweepAccumulate(ray);
    }

    /* Move order rides the ray at the cursor depth. A controller has six
       degrees of freedom, so unlike the mouse pie plate this needs no
       horizontal plane to project onto - and therefore has no degenerate
       angle where the projection flies off across the map. */
    if (vrw.moveActive && hand == vrw.moveHand)
    {
        /* Point at something and the order goes there, whatever the zoom.
           Without this the reach is stuck near the seeded depth, which tracks
           camera distance - so close in, a beam pointing at a distant rock
           would drop the order a few hundred units from the hand. Drawing a
           path is exempt: a curve that snapped to passing ships would kink. */
        if (!vrw.cursorManual && !vrw.pathDrawing
            && ray->hover != NULL && ray->hoverT > VRW_CURSOR_MIN)
        {
            vrw.cursorDist = ray->hoverT;
        }
        vrwMoveRecompute(hand);
    }
    return ray->hover != NULL && ray->hover != oldHover;
}

bool32 vrWorldHandHasTarget(sdword hand)
{
    /* A blob counts. On the sensors map it is often the only thing there is
       to point at, and without it the beam would draw to infinity and the
       haptic tick would never fire over a sphere the size of a battle.
       Outside the map hoverBlob is always NULL, so nothing else changes. */
    return vrw.ray[hand].valid
        && (vrw.ray[hand].hover != NULL || vrw.ray[hand].hoverBlob != NULL);
}

bool32 vrWorldSensorsOverlayActive(void)
{
    return vrw.sensorsOverlay;
}

blob* vrWorldSensorsHoverBlob(void)
{
    sdword hand;

    if (!vrWorldSensorsActive() && !vrw.sensorsOverlay)
    {
        return NULL;
    }
    for (hand = 0; hand < VRW_HAND_COUNT; hand++)
    {
        if (vrw.ray[hand].valid && vrw.ray[hand].hoverBlob != NULL)
        {
            return vrw.ray[hand].hoverBlob;
        }
    }
    return NULL;
}

bool32 vrWorldHandHasSelectable(sdword hand)
{
    SpaceObjRotImpTarg* obj = vrw.ray[hand].valid ? vrw.ray[hand].hover : NULL;

    return vrwPlayerShipSelectable(obj);
}

real32 vrWorldHandHitDistance(sdword hand)
{
    if (!vrWorldHandHasTarget(hand) || vrw.scale <= 0.0f)
    {
        return -1.0f;
    }
    return vrw.ray[hand].hoverT / vrw.scale;
}

void vrWorldSetRayLimit(sdword hand, real32 metres)
{
    /* The game world and wrist screen are separate OpenXR layers. Stop the
       projection-layer beam just in front of the quad, then let the exact
       panel-space reticle drawn by vr.c finish the visual connection. */
    real32 visibleMetres = metres > 0.012f ? metres - 0.012f : metres;

    vrw.ray[hand].limitT = visibleMetres * vrw.scale;
}

void vrWorldSetIntent(sdword hand, vrworldintent intent)
{
    if (hand >= 0 && hand < VRW_HAND_COUNT
        && intent >= VRW_INTENT_IDLE && intent <= VRW_INTENT_INVALID)
    {
        vrw.ray[hand].intent = intent;
    }
}

/*-----------------------------------------------------------------------------
    Selection
----------------------------------------------------------------------------*/
static bool32 vrwOrdersBlocked(void)
{
    return (universePause && !opPauseOrders) || mrDisabled;
}

bool32 vrWorldSelectClick(sdword hand, bool32 additive)
{
    SpaceObjRotImpTarg* obj = vrw.ray[hand].valid ? vrw.ray[hand].hover : NULL;
    bool32 changed = FALSE;

    if (!vrwPlayerShipSelectable(obj))
    {
        /* Only genuinely empty space clears the selection. Pointing at an
           enemy or a resource and pulling the trigger used to deselect the
           whole fleet, which is the opposite of what the mouse path does -
           there, clicking a hostile target orders the selection to act on it. */
        if (obj == NULL && !additive && selSelected.numShips > 0)
        {
            selSelectNone();
            ioUpdateShipTotals();
            changed = TRUE;
        }
        return changed;
    }

    if (additive)
    {
        if (selShipInSelection(selSelected.ShipPtr, selSelected.numShips, (Ship*)obj))
        {
            selSelectionRemoveSingleShip(&selSelected, (Ship*)obj);
        }
        else
        {
            selSelectionAddSingleShip(&selSelected, (Ship*)obj);
        }
        changed = TRUE;
    }
    else
    {
        changed = selSelected.numShips != 1 || selSelected.ShipPtr[0] != (Ship*)obj;
        selSelectionSetSingleShip((Ship*)obj);
    }
    ioUpdateShipTotals();
    return changed;
}

bool32 vrWorldSelectType(sdword hand, bool32 additive)
{
    SpaceObjRotImpTarg* obj = vrw.ray[hand].valid ? vrw.ray[hand].hover : NULL;
    Ship* target;
    Node* node;
    bool32 changed = FALSE;

    if (!vrwPlayerShipSelectable(obj))
    {
        return FALSE;
    }
    target = (Ship*)obj;
    if (!additive && selSelected.numShips > 0)
    {
        selSelectNone();
        changed = TRUE;
    }
    /* Match the desktop double-click feel by limiting the expansion to the
       current render list rather than selecting that type across the map. */
    for (node = universe.RenderList.head;
         node != NULL && selSelected.numShips < COMMAND_MAX_SHIPS;
         node = node->next)
    {
        SpaceObjRotImpTarg* candidate =
            (SpaceObjRotImpTarg*)listGetStructOfNode(node);

        if (vrwPlayerShipSelectable(candidate)
            && ((Ship*)candidate)->shiptype == target->shiptype
            && !selShipInSelection(selSelected.ShipPtr, selSelected.numShips,
                                   (Ship*)candidate))
        {
            selSelectionAddSingleShip(&selSelected, (Ship*)candidate);
            changed = TRUE;
        }
    }
    if (changed)
    {
        ioUpdateShipTotals();
        tutGameMessage("Game_SelectingRect");
    }
    return changed;
}

/* How far down the beam to draw the brush. The capture cone is unbounded, so
   pick a reach the player can judge: the fleet's depth along the ray, since
   that is where their ships are and therefore where the width matters. */
static real32 vrwSweepReachFor(sdword hand)
{
    vrwray const* ray = &vrw.ray[hand];
    vector toFleet;
    real32 reach;

    if (selSelected.numShips > 0)
    {
        selCentrePointCompute();
    }
    vecSub(toFleet, selCentrePoint, ray->origin);
    reach = vecDotProduct(toFleet, ray->dir);
    return reach > 1000.0f ? reach : 3.0f * vrw.scale;      //else 3m of hologram
}

void vrWorldSweepBegin(sdword hand)
{
    vrw.sweepActive = TRUE;
    vrw.sweepHand = hand;
    vrw.sweepPreview.numShips = 0;
    vrw.sweepTrailCount = 0;

    vrw.sweepReach = vrwSweepReachFor(hand);
}

bool32 vrWorldSweepCommit(sdword hand, bool32 additive)
{
    sdword i;
    bool32 changed = FALSE;

    if (!vrw.sweepActive || hand != vrw.sweepHand)
    {
        return FALSE;
    }
    vrw.sweepActive = FALSE;
    if (!additive)
    {
        if (selSelected.numShips > 0)
        {
            selSelectNone();
            changed = TRUE;
        }
    }
    for (i = 0; i < vrw.sweepPreview.numShips; i++)
    {
        if (!selShipInSelection(selSelected.ShipPtr, selSelected.numShips, vrw.sweepPreview.ShipPtr[i]))
        {
            selSelectionAddSingleShip(&selSelected, vrw.sweepPreview.ShipPtr[i]);
            changed = TRUE;
        }
    }
    if (changed)
    {
        ioUpdateShipTotals();
        tutGameMessage("Game_SelectingRect");
    }
    SDL_Log("VR: sweep brush caught %d ship(s), selection now %d (additive=%d)",
            (int)vrw.sweepPreview.numShips, (int)selSelected.numShips,
            (int)additive);
    vrw.sweepPreview.numShips = 0;
    vrw.sweepTrailCount = 0;
    return changed;
}

void vrWorldTargetSweepBegin(sdword hand)
{
    SpaceObjRotImpTarg* obj = vrw.ray[hand].valid ? vrw.ray[hand].hover : NULL;

    vrw.targetSweepActive = TRUE;
    vrw.targetSweepPainting = TRUE;
    vrw.targetSweepHand = hand;
    vrw.targetSweep.numTargets = 0;
    /* the two sweeps are mutually exclusive - one needs the trigger free, the
       other needs it during an order preview - so they share the brush state
       the gizmo draws from */
    vrw.sweepHand = hand;
    vrw.sweepTrailCount = 0;
    vrw.sweepReach = vrwSweepReachFor(hand);
    /* seed with whatever the order preview is already aimed at, so the first
       target does not have to be swept over a second time */
    if (vrwAttackable(obj))
    {
        vrw.targetSweep.TargetPtr[vrw.targetSweep.numTargets++] = obj;
    }
    SDL_Log("VR: attack target sweep begin, seeded %d",
            (int)vrw.targetSweep.numTargets);
}

void vrWorldTargetSweepPaint(bool32 painting)
{
    if (vrw.targetSweepActive)
    {
        vrw.targetSweepPainting = painting;
    }
}

sdword vrWorldTargetSweepCount(void)
{
    return vrw.targetSweepActive ? vrw.targetSweep.numTargets : 0;
}

bool32 vrWorldTargetSweepCommit(void)
{
    MaxSelection attackers;
    MaxAnySelection targets;

    if (!vrw.targetSweepActive)
    {
        return FALSE;
    }
    vrw.targetSweepActive = FALSE;
    vrw.targetSweepPainting = FALSE;
    targets = vrw.targetSweep;
    vrw.targetSweep.numTargets = 0;
    if (targets.numTargets == 0 || selSelected.numShips == 0
        || vrwOrdersBlocked())
    {
        return FALSE;
    }
    if (!MakeShipsAttackCapable((SelectCommand*)&attackers,
                                (SelectCommand*)&selSelected))
    {
        return FALSE;
    }
    /* the game's own filter decides what is legitimately shootable without a
       force-attack, exactly as the ctrl-band-box path does */
    MakeTargetsOnlyNonForceAttackTargets((SelectAnyCommand*)&targets,
                                         universe.curPlayerPtr);
    if (targets.numTargets == 0)
    {
        return FALSE;
    }
    MakeShipMastersIncludeSlaves((SelectCommand*)&targets);
    /* as for any fresh order: a follower still feeding move legs to these
       ships would otherwise override the attack a fraction of a second later */
    vrWorldPathCancel();
    clWrapAttack(&universe.mainCommandLayer, (SelectCommand*)&attackers,
                 (SelectAnyCommand*)&targets);
    tutGameMessage("Game_ClickAttack");
    SDL_Log("VR: multi-attack issued: %d ships -> %d targets",
            (int)attackers.numShips, (int)targets.numTargets);
    return TRUE;
}

void vrWorldTargetSweepCancel(void)
{
    vrw.targetSweepActive = FALSE;
    vrw.targetSweepPainting = FALSE;
    vrw.targetSweep.numTargets = 0;
}

void vrWorldSweepCancel(void)
{
    vrw.sweepActive = FALSE;
    vrw.sweepPreview.numShips = 0;
    vrw.sweepTrailCount = 0;
}

/*-----------------------------------------------------------------------------
    Orders
----------------------------------------------------------------------------*/
vrworldintent vrWorldContextIntent(sdword hand)
{
    SpaceObjRotImpTarg* obj = vrw.ray[hand].valid ? vrw.ray[hand].hover : NULL;
    MaxSelection capable;

    if (obj == NULL)
    {
        return VRW_INTENT_MOVE;
    }
    if (selSelected.numShips == 0 || vrwOrdersBlocked())
    {
        return VRW_INTENT_INVALID;
    }
    /* Single-click-special ships take precedence, as in mrObjectClick: a
       Salvage Corvette pointed at a derelict salvages it, a Repair Corvette
       at a damaged ship repairs it. Checked before the plain harvest and
       attack cases because the same target means something different when
       these ships are in the selection. */
    if (obj->objtype == OBJ_ShipType || obj->objtype == OBJ_DerelictType)
    {
        if (MakeShipsSingleClickSpecialCapable((SelectCommand*)&capable,
                                               (SelectCommand*)&selSelected))
        {
            MaxAnySelection targets;

            targets.numTargets = 1;
            targets.TargetPtr[0] = obj;
            if (ShiptypeInSelection((SelectCommand*)&capable, SalCapCorvette))
            {
                MakeTargetsSalvageable((SelectAnyCommand*)&targets,
                                       universe.curPlayerPtr);
            }
            else
            {
                MakeTargetsOnlyNonForceAttackTargets((SelectAnyCommand*)&targets,
                                                     universe.curPlayerPtr);
            }
            if (targets.numTargets > 0)
            {
                return VRW_INTENT_SPECIAL;
            }
        }
    }
    if (obj->objtype == OBJ_AsteroidType || obj->objtype == OBJ_DustType
        || obj->objtype == OBJ_GasType)
    {
        return MakeShipsHarvestCapable((SelectCommand*)&capable,
                                       (SelectCommand*)&selSelected)
             ? VRW_INTENT_HARVEST : VRW_INTENT_INVALID;
    }
    if (obj->objtype != OBJ_ShipType)
    {
        return VRW_INTENT_INVALID;
    }
    if (((Ship*)obj)->playerowner == universe.curPlayerPtr)
    {
        if (!((Ship*)obj)->staticinfo->canReceiveSomething)
        {
            return VRW_INTENT_INVALID;
        }
        capable = selSelected;
        makeShipsDockCapable((SelectCommand*)&capable);
        return capable.numShips > 0 ? VRW_INTENT_DOCK : VRW_INTENT_INVALID;
    }
    if (allianceIsShipAlly((Ship*)obj, universe.curPlayerPtr))
    {
        return VRW_INTENT_INVALID;
    }
    return MakeShipsAttackCapable((SelectCommand*)&capable,
                                  (SelectCommand*)&selSelected)
         ? VRW_INTENT_ATTACK : VRW_INTENT_INVALID;
}

bool32 vrWorldContextOrder(sdword hand)
{
    SpaceObjRotImpTarg* obj = vrw.ray[hand].valid ? vrw.ray[hand].hover : NULL;
    MaxSelection tempSelection;
    vrworldintent intent = vrWorldContextIntent(hand);

    if (obj == NULL || intent == VRW_INTENT_MOVE || intent == VRW_INTENT_INVALID)
    {
        return FALSE;
    }
    /* a fresh order supersedes whatever path was being flown */
    vrWorldPathCancel();

    if (intent == VRW_INTENT_HARVEST)
    {                                                       //harvest
        if (MakeShipsHarvestCapable((SelectCommand*)&tempSelection, (SelectCommand*)&selSelected))
        {
            clWrapCollectResource(&universe.mainCommandLayer, (SelectCommand*)&tempSelection,
                                  (Resource*)obj);
            tutGameMessage("Game_ClickHarvest");
            return TRUE;
        }
        return FALSE;
    }

    if (intent == VRW_INTENT_SPECIAL)
    {                                                       //salvage / repair / support
        MaxAnySelection targets;

        if (!MakeShipsSingleClickSpecialCapable((SelectCommand*)&tempSelection,
                                                (SelectCommand*)&selSelected))
        {
            return FALSE;
        }
        targets.numTargets = 1;
        targets.TargetPtr[0] = obj;
        if (ShiptypeInSelection((SelectCommand*)&tempSelection, SalCapCorvette))
        {
            MakeTargetsSalvageable((SelectAnyCommand*)&targets,
                                   universe.curPlayerPtr);
        }
        else
        {
            MakeTargetsOnlyNonForceAttackTargets((SelectAnyCommand*)&targets,
                                                 universe.curPlayerPtr);
        }
        if (targets.numTargets == 0)
        {
            return FALSE;
        }
        clWrapSpecial(&universe.mainCommandLayer, (SelectCommand*)&tempSelection,
                      (SpecialCommand*)&targets);
        return TRUE;
    }

    if (intent == VRW_INTENT_DOCK)
    {                                                       //own ship: dock at it
        tempSelection = selSelected;
        makeShipsDockCapable((SelectCommand*)&tempSelection);
        if (tempSelection.numShips > 0)
        {
            clWrapDock(&universe.mainCommandLayer, (SelectCommand*)&tempSelection,
                       DOCK_AT_SPECIFIC_SHIP, (Ship*)obj);
            tutGameMessage("Game_DoubleClickDock");
            return TRUE;
        }
        return FALSE;
    }

    /* non-allied enemy: attack */
    if (intent == VRW_INTENT_ATTACK
        && MakeShipsAttackCapable((SelectCommand*)&tempSelection,
                                  (SelectCommand*)&selSelected))
    {
        MaxAnySelection attackOne;

        attackOne.numTargets = 1;
        attackOne.TargetPtr[0] = obj;
        MakeShipMastersIncludeSlaves((SelectCommand*)&attackOne);
        clWrapAttack(&universe.mainCommandLayer, (SelectCommand*)&tempSelection,
                     (SelectAnyCommand*)&attackOne);
        tutGameMessage("Game_ClickAttack");
        return TRUE;
    }
    return FALSE;
}

bool32 vrWorldHandAttackable(sdword hand)
{
    SpaceObjRotImpTarg* obj = vrw.ray[hand].valid ? vrw.ray[hand].hover : NULL;
    MaxSelection capable;

    return vrwAttackable(obj) && selSelected.numShips > 0
        && !vrwOrdersBlocked()
        && MakeShipsAttackCapable((SelectCommand*)&capable,
                                  (SelectCommand*)&selSelected) != 0;
}

/* Deliberately built on the same two filters the swept multi-attack uses -
   vrwAttackable to decide what the ray may pick up, then the game's own
   MakeTargetsOnlyNonForceAttackTargets to decide what may be shot without a
   force-attack. One target or twenty, the button means the same thing. */
bool32 vrWorldAttackOrder(sdword hand)
{
    SpaceObjRotImpTarg* obj = vrw.ray[hand].valid ? vrw.ray[hand].hover : NULL;
    MaxSelection attackers;
    MaxAnySelection targets;

    if (!vrwAttackable(obj) || selSelected.numShips == 0 || vrwOrdersBlocked())
    {
        return FALSE;
    }
    if (!MakeShipsAttackCapable((SelectCommand*)&attackers,
                                (SelectCommand*)&selSelected))
    {
        return FALSE;
    }
    targets.numTargets = 1;
    targets.TargetPtr[0] = obj;
    MakeTargetsOnlyNonForceAttackTargets((SelectAnyCommand*)&targets,
                                         universe.curPlayerPtr);
    if (targets.numTargets == 0)
    {
        return FALSE;
    }
    MakeShipMastersIncludeSlaves((SelectCommand*)&targets);
    /* a fresh order supersedes whatever path was being flown */
    vrWorldPathCancel();
    clWrapAttack(&universe.mainCommandLayer, (SelectCommand*)&attackers,
                 (SelectAnyCommand*)&targets);
    tutGameMessage("Game_ClickAttack");
    SDL_Log("VR: attack issued: %d ship(s) -> 1 target",
            (int)attackers.numShips);
    return TRUE;
}

bool32 vrWorldMoveBegin(sdword hand)
{
    vrwray const* ray = &vrw.ray[hand];
    vector toSelection;

    if (selSelected.numShips == 0 || vrwOrdersBlocked() || !ray->valid)
    {
        return FALSE;
    }
    selCentrePointCompute();
    vrw.moveActive = TRUE;
    vrw.moveHand = hand;
    vrw.cursorManual = FALSE;

    /* Seed the depth at the fleet's own, measured ALONG the ray rather than
       straight-line to the centroid. Straight-line puts the cursor on a
       sphere around the hand, so aiming away from the fleet drops the cursor
       off the fleet's depth and the destination appears to lunge. Projecting
       onto the ray instead starts it level with the ships wherever you point.

       Note there is deliberately no ray-pick here: this is only reached when
       the beam is on empty space (vrWorldContextIntent only yields
       VRW_INTENT_MOVE for a NULL hover), so a pick could only ever miss. */
    vecSub(toSelection, selCentrePoint, ray->origin);
    vrw.cursorDist = vecDotProduct(toSelection, ray->dir);
    if (!(vrw.cursorDist > VRW_CURSOR_MIN))
    {
        /* pointing away from the fleet: fall back to its true distance */
        vrw.cursorDist = fsqrt(vecMagnitudeSquared(toSelection));
        if (!(vrw.cursorDist > VRW_CURSOR_MIN))
        {
            vrw.cursorDist = VRW_CURSOR_MIN;
        }
    }
    vrwMoveRecompute(hand);
    SDL_Log("VR: move cursor seeded at %.0f units (fleet %.0f away)",
            vrw.cursorDist, fsqrt(vecMagnitudeSquared(toSelection)));
    tutGameMessage("Game_Move");
    return TRUE;
}

/* depthDelta is a fractional change for this frame: it scales the cursor
   distance rather than adding a height, so the same flick feels equally
   responsive whether the fleet is 2000 or 60000 units out. */
void vrWorldMoveUpdate(sdword hand, real32 depthDelta)
{
    if (!vrw.moveActive || hand != vrw.moveHand)
    {
        return;
    }
    if (depthDelta != 0.0f)
    {
        vrw.cursorManual = TRUE;                //manual beats snapping now
        vrw.cursorDist *= 1.0f + depthDelta;
        if (vrw.cursorDist < VRW_CURSOR_MIN)
        {
            vrw.cursorDist = VRW_CURSOR_MIN;
        }
        else if (vrw.cursorDist > VRW_RAY_LENGTH)
        {
            vrw.cursorDist = VRW_RAY_LENGTH;
        }
        vrwMoveRecompute(hand);
        tutGameMessage("Game_MoveZ");
    }
    else
    {
        tutGameMessage("Game_MoveXY");
    }
}

bool32 vrWorldMoveCommit(void)
{
    if (!vrw.moveActive)
    {
        return FALSE;
    }
    vrw.moveActive = FALSE;
    if (selSelected.numShips == 0 || vrwOrdersBlocked())
    {
        return FALSE;
    }
    vrWorldPathCancel();                //a plain move replaces a flown path
    MakeShipsMobile((SelectCommand*)&selSelected);
    if (selSelected.numShips > 0)
    {
        clWrapMove(&universe.mainCommandLayer, (SelectCommand*)&selSelected,
                   selCentrePoint, vrw.moveDestination);
        tutGameMessage("Game_MoveIssued");
        return TRUE;
    }
    return FALSE;
}

void vrWorldMoveCancel(void)
{
    if (vrw.moveActive)
    {
        vrw.moveActive = FALSE;
        tutGameMessage("Game_MoveQuit");
    }
}

bool32 vrWorldMoveActive(void)
{
    return vrw.moveActive;
}

real32 vrWorldCursorDist(void)
{
    return vrw.cursorDist;
}

/*-----------------------------------------------------------------------------
    Freehand flight paths

    Homeworld has no waypoint order. MoveCommand is one heading and one
    destination, and clMove either overwrites them or replaces the command
    outright; there is no list and no "and then". So a drawn curve has to be
    flown from out here.

    Not as a chain of destinations, though. Handing the fleet each point in
    turn and waiting for it to arrive fails twice over: ChangeOrderToMove runs
    InitShipsForAI, which zeroes ship->aistate and drops every hull back into
    STATE_POINT_IN_DIRECTION - a brake and a pivot at every point - and the
    arrival radius needed to make a spread-out group ever count as "there"
    is wider than the gap between the points, so they are consumed as fast as
    the clock ticks and the curve evaporates.

    Instead the curve is a carrot. One destination slides along it, kept a
    lead distance ahead of where the fleet has actually got to, and the
    command layer is steered rather than re-ordered: move.destination, the
    heading and each ship's moveTo are written directly, leaving aistate
    alone, so nothing pivots, no engine restarts and nothing flashes. The
    ships are always chasing a point far enough ahead to be worth full
    throttle, and always one that lies on the drawn curve.

    Steering the command layer behind its own back is a single-player liberty:
    in a network game orders have to travel as orders, so there the follower
    falls back to a rate-limited clWrapMove and accepts the pivots.

    The stroke itself is a centripetal Catmull-Rom spline through the swept
    points, resampled to even arc length so that "the point s units along"
    costs one divide and one lerp however fast the hand moved.
----------------------------------------------------------------------------*/
static real32 vrwDistance(vector const* a, vector const* b)
{
    vector d;

    vecSub(d, *a, *b);
    return fsqrt(vecMagnitudeSquared(d));
}

static void vrwLerp(vector* out, vector const* a, vector const* b, real32 s)
{
    out->x = a->x + (b->x - a->x) * s;
    out->y = a->y + (b->y - a->y) * s;
    out->z = a->z + (b->z - a->z) * s;
}

/* Centripetal Catmull-Rom through p1..p2, with p0/p3 setting the tangents.

   The uniform form this replaced spaces its knots evenly no matter how far
   apart the points actually are. A hand that slows through a turn and speeds
   up out of it lays down a short segment next to a long one, and uniform
   knots answer that with an overshoot: the curve bulges past the point it is
   meant to pass through, and where the stroke doubles back it ties a small
   loop. Both read as the path "wobbling" rather than curving.

   Spacing the knots by the square root of chord length (alpha = 0.5, the
   centripetal case) is the standard cure and is provably free of cusps and
   self-intersections, whatever the sampling does. Evaluated with the
   Barry-Goldman pyramid, which stays well behaved as intervals get small. */
static void vrwSpline(vector const* p0, vector const* p1, vector const* p2,
                      vector const* p3, real32 t, vector* out)
{
    real32 t0, t1, t2, t3, tt;
    vector a1, a2, a3, b1, b2;

    /* knot spacing: sqrt of chord length, floored so coincident samples
       cannot divide by zero */
    t0 = 0.0f;
    t1 = t0 + fsqrt(vrwDistance(p0, p1));
    t2 = t1 + fsqrt(vrwDistance(p1, p2));
    t3 = t2 + fsqrt(vrwDistance(p2, p3));
    if (t1 - t0 < VRW_PATH_KNOT_MIN) t1 = t0 + VRW_PATH_KNOT_MIN;
    if (t2 - t1 < VRW_PATH_KNOT_MIN) t2 = t1 + VRW_PATH_KNOT_MIN;
    if (t3 - t2 < VRW_PATH_KNOT_MIN) t3 = t2 + VRW_PATH_KNOT_MIN;

    tt = t1 + t * (t2 - t1);

    vrwLerp(&a1, p0, p1, (tt - t0) / (t1 - t0));
    vrwLerp(&a2, p1, p2, (tt - t1) / (t2 - t1));
    vrwLerp(&a3, p2, p3, (tt - t2) / (t3 - t2));
    vrwLerp(&b1, &a1, &a2, (tt - t0) / (t2 - t0));
    vrwLerp(&b2, &a2, &a3, (tt - t1) / (t3 - t1));
    vrwLerp(out, &b1, &b2, (tt - t1) / (t2 - t1));
}

/* Hand tremor rides on the samples at a far higher frequency than the shape
   being drawn, and an interpolating spline is obliged to reproduce every
   wobble of it faithfully. A couple of [1 2 1] passes first, with the ends
   pinned so the stroke still starts and finishes exactly where the hand did,
   means the spline draws the stroke that was intended. */
static void vrwPathSmoothSamples(void)
{
    vector smoothed[VRW_PATH_MAX_SAMPLES];
    sdword i, pass;

    if (vrw.pathSampleCount < 3)
    {
        return;
    }
    for (pass = 0; pass < VRW_PATH_SMOOTH_PASSES; pass++)
    {
        smoothed[0] = vrw.pathSample[0];
        smoothed[vrw.pathSampleCount - 1] = vrw.pathSample[vrw.pathSampleCount - 1];
        for (i = 1; i + 1 < vrw.pathSampleCount; i++)
        {
            smoothed[i].x = (vrw.pathSample[i - 1].x
                             + 2.0f * vrw.pathSample[i].x
                             + vrw.pathSample[i + 1].x) * 0.25f;
            smoothed[i].y = (vrw.pathSample[i - 1].y
                             + 2.0f * vrw.pathSample[i].y
                             + vrw.pathSample[i + 1].y) * 0.25f;
            smoothed[i].z = (vrw.pathSample[i - 1].z
                             + 2.0f * vrw.pathSample[i].z
                             + vrw.pathSample[i + 1].z) * 0.25f;
        }
        memcpy(vrw.pathSample, smoothed,
               sizeof(vector) * (size_t)vrw.pathSampleCount);
    }
}

/* Point at parameter t within raw segment index, ends clamped */
static void vrwSplineAt(sdword segment, real32 t, vector* out)
{
    sdword last = vrw.pathSampleCount - 1;
    sdword i0 = segment - 1 < 0 ? 0 : segment - 1;
    sdword i1 = segment;
    sdword i2 = segment + 1 > last ? last : segment + 1;
    sdword i3 = segment + 2 > last ? last : segment + 2;

    vrwSpline(&vrw.pathSample[i0], &vrw.pathSample[i1], &vrw.pathSample[i2],
              &vrw.pathSample[i3], t, out);
}

/* Halve the stroke in place when it fills up, so an arbitrarily long sweep
   keeps being recorded at progressively coarser resolution. */
static void vrwPathDecimate(void)
{
    sdword i, kept = 0;

    for (i = 0; i < vrw.pathSampleCount; i += 2)
    {
        vrw.pathSample[kept++] = vrw.pathSample[i];
    }
    vrw.pathSampleCount = kept;
    vrw.pathMinSampleDist *= 2.0f;
}

void vrWorldPathBegin(sdword hand)
{
    if (!vrw.moveActive || hand != vrw.moveHand)
    {
        return;
    }
    /* A new stroke replaces whatever was being flown. They share pathPoint,
       so leaving the follower running would have it indexing a curve that is
       being overwritten - and drawing a fresh route is the player saying the
       old one is finished with anyway. */
    vrWorldPathCancel();
    vrw.pathDrawing = TRUE;
    vrw.pathHand = hand;
    vrw.pathSampleCount = 0;
    vrw.pathCount = 0;
    vrw.pathSpacing = 0.0f;
    /* start fine: a short stroke should keep its shape, and decimation
       coarsens this on demand for long ones */
    vrw.pathMinSampleDist = 0.03f * vrw.scale;
    vrw.pathSample[vrw.pathSampleCount++] = vrw.moveDestination;
    SDL_Log("VR: path draw begin hand=%d minSample=%.1f", (int)hand,
            vrw.pathMinSampleDist);
}

void vrWorldPathSample(sdword hand)
{
    vector const* point = &vrw.moveDestination;

    if (!vrw.pathDrawing || hand != vrw.pathHand)
    {
        return;
    }
    if (vrw.pathSampleCount > 0
        && vrwDistance(point, &vrw.pathSample[vrw.pathSampleCount - 1])
           < vrw.pathMinSampleDist)
    {
        return;
    }
    if (vrw.pathSampleCount >= VRW_PATH_MAX_SAMPLES)
    {
        vrwPathDecimate();
    }
    vrw.pathSample[vrw.pathSampleCount++] = *point;
}

/* The point a given arc length along the curve. Even spacing is what buys
   this: no table search, just an index and a fraction. */
static void vrwPathPointAt(real32 s, vector* out)
{
    sdword i;
    real32 f;

    if (vrw.pathCount <= 0)
    {
        vecZeroVector(*out);
        return;
    }
    if (s <= 0.0f || vrw.pathSpacing <= 0.0f)
    {
        *out = vrw.pathPoint[0];
        return;
    }
    i = (sdword)(s / vrw.pathSpacing);
    if (i >= vrw.pathCount - 1)
    {
        *out = vrw.pathPoint[vrw.pathCount - 1];
        return;
    }
    f = (s - (real32)i * vrw.pathSpacing) / vrw.pathSpacing;
    vrwLerp(out, &vrw.pathPoint[i], &vrw.pathPoint[i + 1], f);
}

/* Trigger released: smooth the stroke and resample it to even arc length. */
bool32 vrWorldPathFinishStroke(void)
{
    sdword segment, step, emitted;
    real32 total = 0.0f, spacing, acc = 0.0f;
    vector previous, current;

    if (!vrw.pathDrawing)
    {
        return FALSE;
    }
    vrw.pathDrawing = FALSE;
    vrw.pathCount = 0;
    vrw.pathLength = 0.0f;
    vrw.pathSpacing = 0.0f;
    if (vrw.pathSampleCount < 2)
    {
        SDL_Log("VR: path stroke too short (%d samples)",
                (int)vrw.pathSampleCount);
        return FALSE;
    }

    vrwPathSmoothSamples();

    /* pass one: how long the smoothed curve actually is */
    previous = vrw.pathSample[0];
    for (segment = 0; segment + 1 < vrw.pathSampleCount; segment++)
    {
        for (step = 1; step <= VRW_PATH_SPLINE_STEPS; step++)
        {
            vrwSplineAt(segment, (real32)step / (real32)VRW_PATH_SPLINE_STEPS,
                        &current);
            total += vrwDistance(&previous, &current);
            previous = current;
        }
    }
    spacing = total / (real32)(VRW_PATH_MAX_POINTS - 1);
    if (total <= 0.0f || spacing <= 0.0f)
    {
        return FALSE;
    }

    /* Pass two: emit a point at every multiple of the spacing, interpolating
       within whichever spline step straddles it. Walking the curve a second
       time rather than keeping the thousand-odd fine points around - they are
       only wanted for their lengths, and this is not a hot path. */
    vrw.pathPoint[0] = vrw.pathSample[0];
    emitted = 1;
    previous = vrw.pathSample[0];
    for (segment = 0; segment + 1 < vrw.pathSampleCount; segment++)
    {
        for (step = 1; step <= VRW_PATH_SPLINE_STEPS; step++)
        {
            real32 segLen;

            vrwSplineAt(segment, (real32)step / (real32)VRW_PATH_SPLINE_STEPS,
                        &current);
            segLen = vrwDistance(&previous, &current);
            while (emitted < VRW_PATH_MAX_POINTS - 1 && segLen > 0.0f
                   && (real32)emitted * spacing <= acc + segLen)
            {
                real32 f = ((real32)emitted * spacing - acc) / segLen;

                vrwLerp(&vrw.pathPoint[emitted], &previous, &current, f);
                emitted++;
            }
            acc += segLen;
            previous = current;
        }
    }
    /* rounding can leave the tail a point or two short; pad, then finish
       exactly where the hand stopped */
    while (emitted < VRW_PATH_MAX_POINTS)
    {
        vrw.pathPoint[emitted++] = previous;
    }
    vrw.pathPoint[VRW_PATH_MAX_POINTS - 1] =
        vrw.pathSample[vrw.pathSampleCount - 1];
    vrw.pathCount = VRW_PATH_MAX_POINTS;
    vrw.pathSpacing = spacing;
    vrw.pathLength = total;

    SDL_Log("VR: path stroke done: %d samples, length=%.0f -> %d points "
            "spaced %.0f", (int)vrw.pathSampleCount, total,
            (int)vrw.pathCount, spacing);
    return TRUE;
}

bool32 vrWorldPathCommit(void)
{
    sdword i;
    real32 maxColl = 0.0f, paceVel = 0.0f, leadCap;
    vector first;

    if (vrw.pathCount <= 0)
    {
        return FALSE;
    }
    if (selSelected.numShips == 0 || vrwOrdersBlocked())
    {
        vrWorldPathCancel();
        return FALSE;
    }

    vrw.pathSelection = selSelected;
    MakeShipsMobile((SelectCommand*)&vrw.pathSelection);
    if (vrw.pathSelection.numShips == 0)
    {
        vrWorldPathCancel();
        return FALSE;
    }
    for (i = 0; i < vrw.pathSelection.numShips; i++)
    {
        Ship* ship = vrw.pathSelection.ShipPtr[i];
        real32 coll = ship->staticinfo->staticheader.staticCollInfo.collspheresize;
        real32 vel = tacticsGetShipsMaxVelocity(ship);

        if (coll > maxColl)
        {
            maxColl = coll;
        }
        /* the slowest hull sets the pace: lead the group by more than its
           laggard can cover and the fast ships simply run off the front */
        if (paceVel <= 0.0f || vel < paceVel)
        {
            paceVel = vel;
        }
    }

    /* Ten seconds of travel is where the game's own velocity controller stops
       throttling back (see VRW_PATH_LEAD_SECONDS), capped so a short stroke
       keeps its shape and floored so the carrot is always clear of the
       group's own hulls and of MOVE_ARRIVE_TOLERANCE. */
    vrw.pathLead = paceVel * VRW_PATH_LEAD_SECONDS;
    leadCap = vrw.pathLength * VRW_PATH_LEAD_MAX_FRAC;
    if (vrw.pathLead > leadCap)
    {
        vrw.pathLead = leadCap;
    }
    if (vrw.pathLead < maxColl * VRW_PATH_LEAD_HULLS + vrw.pathSpacing)
    {
        vrw.pathLead = maxColl * VRW_PATH_LEAD_HULLS + vrw.pathSpacing;
    }
    vrw.pathArrive = maxColl * 3.0f + vrw.pathSpacing;

    selCentrePointCompute();
    vrw.pathActive = TRUE;
    vrw.pathSampleCount = 0;            //the drawn stroke has served its purpose
    vrw.pathProgress = 0.0f;
    vrw.pathStallWhere = selCentrePoint;
    vrw.pathStallSince = universe.totaltimeelapsed;
    vrw.pathLastUpdate = universe.totaltimeelapsed;
    vrw.pathSteering = !multiPlayerGame;

    /* One real order, so the command layer, formation and speech all see a
       move being given. Everything after this steers that same command. */
    vrwPathPointAt(vrw.pathLead, &first);
    vrw.pathIssued = first;
    clWrapMove(&universe.mainCommandLayer, (SelectCommand*)&vrw.pathSelection,
               selCentrePoint, first);
    tutGameMessage("Game_MoveIssued");
    SDL_Log("VR: path commit: %d ships, length=%.0f lead=%.0f arrive=%.0f "
            "steering=%d, first target (%.0f %.0f %.0f)",
            (int)vrw.pathSelection.numShips, vrw.pathLength, vrw.pathLead,
            vrw.pathArrive, (int)vrw.pathSteering, first.x, first.y, first.z);
    return TRUE;
}

void vrWorldPathCancel(void)
{
    if (vrw.pathDrawing || vrw.pathActive || vrw.pathCount > 0)
    {
        SDL_Log("VR: path cancelled (drawing=%d active=%d %.0f/%.0f flown)",
                (int)vrw.pathDrawing, (int)vrw.pathActive, vrw.pathProgress,
                vrw.pathLength);
    }
    vrw.pathDrawing = FALSE;
    vrw.pathActive = FALSE;
    vrw.pathSampleCount = 0;
    vrw.pathCount = 0;
    vrw.pathLength = 0.0f;
    vrw.pathProgress = 0.0f;
    vrw.pathSelection.numShips = 0;
}

bool32 vrWorldPathDrawing(void)
{
    return vrw.pathDrawing;
}

bool32 vrWorldPathActive(void)
{
    return vrw.pathActive;
}

sdword vrWorldPathPointCount(void)
{
    return vrw.pathCount;
}

bool32 vrWorldPathPending(void)
{
    return vrw.pathCount > 0 && !vrw.pathActive;
}

/* How far along the curve the fleet has actually got. Searched forward only,
   and only within a window a couple of leads wide: nearest-point over the
   whole remaining curve would let a stroke that doubles back teleport the
   carrot across the loop, and searching backwards would let a group that
   overshoots a corner undo its own progress. */
static void vrwPathAdvance(vector const* centre)
{
    sdword first = (sdword)(vrw.pathProgress / vrw.pathSpacing);
    sdword window = (sdword)((vrw.pathLead * VRW_PATH_SEARCH_LEADS)
                             / vrw.pathSpacing) + 2;
    sdword last = first + window;
    sdword i, best = first;
    real32 bestSqr = -1.0f;

    if (last >= vrw.pathCount)
    {
        last = vrw.pathCount - 1;
    }
    for (i = first; i <= last; i++)
    {
        vector d;
        real32 sqr;

        vecSub(d, vrw.pathPoint[i], *centre);
        sqr = vecMagnitudeSquared(d);
        if (bestSqr < 0.0f || sqr < bestSqr)
        {
            bestSqr = sqr;
            best = i;
        }
    }
    if ((real32)best * vrw.pathSpacing > vrw.pathProgress)
    {
        vrw.pathProgress = (real32)best * vrw.pathSpacing;
    }
}

/* Has something else given these ships orders? A fresh order of any kind
   supersedes the route: the player pointing somewhere new means it, and going
   on steering would drag the fleet back a tick later. This catches the orders
   that never pass through here at all - the Sensors map's own move, the
   wheel's Halt and Dock, a mission script - as well as the ones that do.

   A missing command is not a takeover. It is what arriving looks like, and the
   follower's job at that point is to push the group on to the next stretch. */
static bool32 vrwPathTakenOver(void)
{
    CommandToDo* command =
        IsSelectionAlreadyDoingSomething(&universe.mainCommandLayer,
                                         (SelectCommand*)&vrw.pathSelection);

    if (command == NULL)
    {
        return FALSE;
    }
    if (command->ordertype.order != COMMAND_MOVE)
    {
        return TRUE;                    //attack, halt, dock, harvest...
    }
    /* A move destination that is not the one we last wrote. Compared with a
       tolerance rather than exactly: in a network game it has been through a
       packet, and the carrot never moves this far in a single tick anyway. */
    return vrwDistance(&command->move.destination, &vrw.pathIssued)
         > vrw.pathArrive;
}

/* Point the fleet's existing move command at a new destination without
   re-issuing it. ChangeOrderToMove would do the same three writes and then
   call InitShipsForAI, whose ship->aistate = 0 is exactly the brake-and-pivot
   this whole approach exists to avoid; aistatecommand is reset because that
   one is the outer "have I arrived" latch, and a ship that tripped it would
   otherwise sit steady while the carrot moved on without it.

   The command is looked up fresh every tick rather than cached: clProcess
   frees a move command the moment processMoveToDo reports everyone done, and
   the player can replace it from the wheel at any time. */
static bool32 vrwPathSteer(vector const* from, vector const* to)
{
    CommandToDo* command;
    bool32 formation;
    sdword i;

    if (!vrw.pathSteering)
    {
        return FALSE;
    }
    command = IsSelectionAlreadyDoingSomething(&universe.mainCommandLayer,
                                               (SelectCommand*)&vrw.pathSelection);
    if (command == NULL || command->ordertype.order != COMMAND_MOVE
        || (command->ordertype.attributes & COMMAND_MASK_ATTACKING_AND_MOVING))
    {
        return FALSE;                   //not ours to steer any more
    }
    command->move.destination = *to;
    vecSub(command->move.heading, *to, *from);
    vecNormalize(&command->move.heading);
    CalculateMoveToPoints(command->selection, *from, *to);

    /* In formation only the leader is flown - processMoveLeaderToDo reads
       move.destination directly and the rest hold station around it. */
    formation = (command->ordertype.attributes & COMMAND_MASK_FORMATION) != 0;
    for (i = 0; i < command->selection->numShips; i++)
    {
        Ship* ship = command->selection->ShipPtr[i];

        ship->moveFrom = ship->posinfo.position;
        if (i == 0 || !formation)
        {
            ship->aistatecommand = 0;
        }
    }
    return TRUE;
}

/* Advance the committed path. Called once per frame from vrWorldFrameBegin;
   gated on universe time so the extra rndFlush passes a manager triggers
   cannot run the follower several times against one tick of simulation. */
static void vrwPathFollow(void)
{
    vector centre, target;
    sdword i, alive = 0;
    real32 invAlive, toEnd;

    if (!vrw.pathActive)
    {
        return;
    }
    if (!(universe.totaltimeelapsed > vrw.pathLastUpdate))
    {
        return;
    }
    vrw.pathLastUpdate = universe.totaltimeelapsed;

    /* drop casualties; abandon the path if the group is gone */
    vecZeroVector(centre);
    for (i = 0; i < vrw.pathSelection.numShips; i++)
    {
        Ship* ship = vrw.pathSelection.ShipPtr[i];

        if (ship->flags & SOF_Dead)
        {
            continue;
        }
        vrw.pathSelection.ShipPtr[alive++] = ship;
        vecAddTo(centre, ship->collInfo.collPosition);
    }
    vrw.pathSelection.numShips = alive;
    if (alive == 0)
    {
        SDL_Log("VR: path abandoned, no ships left");
        vrWorldPathCancel();
        return;
    }
    vecDivideByScalar(centre, (real32)alive, invAlive);

    if (vrwPathTakenOver())
    {
        SDL_Log("VR: path released at %.0f/%.0f, the fleet has new orders",
                vrw.pathProgress, vrw.pathLength);
        vrw.pathActive = FALSE;
        vrw.pathCount = 0;
        vrw.pathSelection.numShips = 0;
        return;
    }

    vrwPathAdvance(&centre);

    /* Done when the group is at the far end. Progress has to agree, or a
       curve that loops back past its own start would finish on the spot. */
    toEnd = vrwDistance(&centre, &vrw.pathPoint[vrw.pathCount - 1]);
    if (vrw.pathProgress >= vrw.pathLength - vrw.pathSpacing
        && toEnd <= vrw.pathArrive)
    {
        SDL_Log("VR: path complete (%.0f units flown)", vrw.pathLength);
        vrw.pathActive = FALSE;
        vrw.pathCount = 0;
        vrw.pathSelection.numShips = 0;
        return;
    }

    /* Stuck means the ships have stopped moving, not that the curve has
       stopped being consumed: the run in to the start of a long path can eat
       a lot of clock without advancing any arc length at all, and measuring
       progress along the curve would call that a stall and give up on it. */
    if (vrwDistance(&centre, &vrw.pathStallWhere) > vrw.pathArrive)
    {
        vrw.pathStallWhere = centre;
        vrw.pathStallSince = universe.totaltimeelapsed;
    }
    else if (universe.totaltimeelapsed - vrw.pathStallSince
             > VRW_PATH_STALL_TIME)
    {
        SDL_Log("VR: path stalled at %.0f/%.0f, releasing the fleet",
                vrw.pathProgress, vrw.pathLength);
        vrw.pathActive = FALSE;
        vrw.pathCount = 0;
        vrw.pathSelection.numShips = 0;
        return;
    }

    vrwPathPointAt(vrw.pathProgress + vrw.pathLead, &target);

    /* Steering is free, so it happens every tick and the carrot slides
       smoothly. Re-ordering is not - it flashes the ships, restarts their
       engines and pivots them - so the fallback waits until the target has
       moved a worthwhile fraction of the lead. */
    if (!vrwPathSteer(&centre, &target))
    {
        if (vrwDistance(&target, &vrw.pathIssued)
            > vrw.pathLead * VRW_PATH_REISSUE_FRAC)
        {
            clWrapMove(&universe.mainCommandLayer,
                       (SelectCommand*)&vrw.pathSelection, centre, target);
            vrw.pathIssued = target;
            SDL_Log("VR: path re-ordered at %.0f/%.0f -> (%.0f %.0f %.0f)",
                    vrw.pathProgress, vrw.pathLength, target.x, target.y,
                    target.z);
        }
    }
    else
    {
        vrw.pathIssued = target;
    }

    if (vrw.debugFrame % VRW_DEBUG_INTERVAL == 1)
    {
        SDL_Log("VR: path %.0f/%.0f flown, %d ship(s), lead=%.0f, %.0f to end",
                vrw.pathProgress, vrw.pathLength, (int)alive, vrw.pathLead,
                toEnd);
    }
}

/*-----------------------------------------------------------------------------
    Camera (grab gestures land here; residuals return to vr.c for folding
    into the free transform)
----------------------------------------------------------------------------*/
/* Camera gestures must mutate BOTH the focus stack's remembercam (the
   tween target CameraChase eases actualcamera toward) and actualcamera
   itself, and flag the motion as user-controlled - exactly what the mouse
   path in ccControl does. Mutating actualcamera alone gets tweened away. */
static bool32 vrwCameraLocked(void)
{
    extern bool32 nisIsRunning;

    return !vrw.worldValid
        || nisIsRunning
        || (universe.mainCameraCommand.ccMode & CCMODE_LOCK_OUT_USER_INPUT) != 0;
}

static Camera* vrwActualCamera(void)
{
    return &universe.mainCameraCommand.actualcamera;
}

static Camera* vrwRememberCamera(void)
{
    return &currentCameraStackEntry(&universe.mainCameraCommand)->remembercam;
}

static void vrwCameraTouched(sdword flags)
{
    universe.mainCameraCommand.UserControlled |= flags;
    if (flags & CAM_USER_ZOOMED)
    {
        universe.mainCameraCommand.zoomInCloseAsPossible = FALSE;
    }
}

real32 vrWorldCameraOrbit(real32 deltaYaw, real32 deltaPitch)
{
    real32 beforeDecl, applied;

    if (vrwCameraLocked())
    {
        return deltaPitch;
    }
    cameraRotAngle(vrwRememberCamera(), deltaYaw);          //yaw never clamps
    cameraRotAngle(vrwActualCamera(), deltaYaw);

    beforeDecl = vrwActualCamera()->declination;
    cameraRotDeclination(vrwRememberCamera(), deltaPitch);
    cameraRotDeclination(vrwActualCamera(), deltaPitch);
    applied = vrwActualCamera()->declination - beforeDecl;
    vrwCameraTouched(CAM_USER_MOVED);
    return deltaPitch - applied;                            //clamp residual
}

real32 vrWorldCameraZoom(real32 ratio)
{
    real32 before, appliedRatio;

    if (vrwCameraLocked() || ratio <= 0.0f)
    {
        return 1.0f;
    }
    before = vrwActualCamera()->distance;
    cameraZoom(vrwRememberCamera(), ratio, TRUE);
    cameraZoom(vrwActualCamera(), ratio, TRUE);
    appliedRatio = vrwActualCamera()->distance / before;
    vrwCameraTouched(CAM_USER_MOVED | CAM_USER_ZOOMED);
    return ratio / appliedRatio;                            //residual ratio
}

void vrWorldCameraFocusSelection(void)
{
    if (vrw.worldValid && selSelected.numShips > 0)
    {
        ccFocus(&universe.mainCameraCommand, (FocusCommand*)&selSelected);
        tutGameMessage("KB_Focus");
    }
}

bool32 vrWorldSensorsActive(void)
{
    /* Not during the zoom transition either way - smViewportProcess ignores
       input then, and steering a camera mid-flight fights the animation. Nor
       during a mission briefing: that map is the mission's to drive, and the
       sticks bypass the smFleetIntel guards Sensors.c uses on the mouse. */
    return smSensorsActive && !smZoomingIn && !smZoomingOut && !smFleetIntel;
}

bool32 vrWorldSensorsBriefing(void)
{
    return smFleetIntel && (smSensorsActive || smZoomingIn || smZoomingOut);
}

bool32 vrWorldToggleSensors(void)
{
    if (smZoomingIn || smZoomingOut)
    {
        return smSensorsActive;                             //mid-transition
    }
    /* A briefing can leave the engine's own manager up. Close it gracefully,
       the way the screen's own Close button does - it plays the zoom back to
       the fleet rather than cutting out - and treat that as the toggle. */
    if (smSensorsActive)
    {
        smSensorsClose(NULL, NULL);
        return FALSE;
    }
    vrw.sensorsOverlay ^= TRUE;
    /* The map's own sounds. Pulling back to the sensor view is a moment in
       Homeworld and the audio is most of why - the SFX duck away, the intro
       sting plays over the zoom, and the sound of space comes back when the
       fleet does. */
    if (vrw.sensorsOverlay)
    {
        soundEventStopSFX(0.5f);
        soundEvent(NULL, UI_SensorsIntro);
    }
    else
    {
        /* Leave the view where the player was working. The map has always
           behaved this way - clicking a blob closes it and takes the camera
           there - and having pulled back to pick something out, coming down
           anywhere else would throw away the only thing the trip was for.
           Falls back to whatever the ray was on when nothing is selected. */
        if (selSelected.numShips > 0)
        {
            vrWorldCameraFocusSelection();
        }
        else
        {
            blob* hovered = vrWorldSensorsHoverBlob();

            if (hovered != NULL && hovered->blobShips != NULL
                && hovered->blobShips->numShips > 0)
            {
                ccFocus(&universe.mainCameraCommand,
                        (FocusCommand*)hovered->blobShips);
            }
        }
        soundEvent(NULL, UI_SensorsExit);
        soundEvent(NULL, UI_SoundOfSpace);
    }
    SDL_Log("VR: sensor overlay %s", vrw.sensorsOverlay ? "on" : "off");
    return vrw.sensorsOverlay;
}

/*-----------------------------------------------------------------------------
    Name        : vrWorldSensorsSpan
    Description : How wide the sensor view needs to be, in game units, for
                  everything worth seeing to fit.

                  The navigation disc is a fixed ring, so that is the floor;
                  blobs outside it push it wider. Returned as a radius about
                  the hologram's anchor, which is the game camera's lookat.
    Inputs      : void
    Outputs     :
    Return      : radius in game units, always positive
----------------------------------------------------------------------------*/
real32 vrWorldSensorsSpan(void)
{
    Node* node;
    real32 span = (smZoomMin + smZoomMax) / 2.0f * smWorldPlaneDistanceFactor;
    sdword sensorLevel = universe.curPlayerPtr != NULL
                       ? universe.curPlayerPtr->sensorLevel : 0;
    vector const* look = &mrCamera->lookatpoint;

    for (node = universe.collBlobList.head; node != NULL; node = node->next)
    {
        blob* thisBlob = (blob*)listGetStructOfNode(node);
        vector d;
        real32 reach;

        if (!((thisBlob->flags & (BTF_Explored | BTF_ProbeDroid))
              || (sensorLevel == 2
                  && bitTest(thisBlob->flags, BTF_UncloakedEnemies))))
        {
            continue;
        }
        vecSub(d, thisBlob->centre, *look);
        reach = fsqrt(vecMagnitudeSquared(d)) + thisBlob->radius;
        if (reach > span)
        {
            span = reach;
        }
    }
    return span > 1.0f ? span : 1.0f;
}

real32 vrWorldSensorsViewMetres(void)
{
    return VRW_SENSOR_VIEW_METRES;
}

void vrWorldSensorsOrbit(real32 deltaYaw, real32 deltaPitch)
{
    if (!vrWorldSensorsActive())
    {
        return;
    }
    cameraRotAngle(&smCamera, deltaYaw);
    cameraRotDeclination(&smCamera, deltaPitch);
}

void vrWorldSensorsZoom(real32 ratio)
{
    if (!vrWorldSensorsActive() || ratio <= 0.0f)
    {
        return;
    }
    /* FALSE: the strategic view is not bound by ship distances the way the
       main camera is, which is the whole point of it */
    cameraZoom(&smCamera, ratio, FALSE);
}

/* Cycle the camera focus through the player's fleet (step = +1 / -1).
   Selection is untouched; this is pure traversal. */
void vrWorldFocusCycle(sdword step)
{
    static sdword cycleIndex = 0;
    Ship* ships[256];
    sdword count = 0;
    Node* node;
    MaxSelection one;

    if (!vrw.worldValid)
    {
        return;
    }
    for (node = universe.ShipList.head; node != NULL && count < 256; node = node->next)
    {
        Ship* ship = (Ship*)listGetStructOfNode(node);

        if (ship->playerowner == universe.curPlayerPtr && !(ship->flags & SOF_Dead)
            && ship->shiptype != Drone)
        {
            ships[count++] = ship;
        }
    }
    if (count == 0)
    {
        return;
    }
    cycleIndex = (cycleIndex + step % count + count) % count;
    one.numShips = 1;
    one.ShipPtr[0] = ships[cycleIndex];
    ccFocus(&universe.mainCameraCommand, (FocusCommand*)&one);
    tutGameMessage("KB_Focus");
}

/*-----------------------------------------------------------------------------
    Overlays (drawn per eye in game-world space)
----------------------------------------------------------------------------*/
void vrWorldDrawOverlays(void)
{
    sdword hand, i;
    sdword oldLighting, oldTexture;
    GLint oldMatrixMode, modelDepthBefore, projectionDepthBefore;
    GLint modelDepthAfter, projectionDepthAfter;
    GLboolean oldFog;
    GLenum entryError, exitError;
    color const hoverColor = colRGB(255, 200, 60);
    color const selColor = colRGB(90, 255, 120);
    color const moveColor = colRGB(255, 255, 255);

    if (!vrw.worldValid)
    {
        return;
    }

    /* Trust nothing about the state the world render ended in: load the
       per-eye matrices captured during this eye's render explicitly, and
       force the fixed-function state the primitives need. */
    entryError = glGetError();
    glGetIntegerv(GL_MATRIX_MODE, &oldMatrixMode);
    glGetIntegerv(GL_MODELVIEW_STACK_DEPTH, &modelDepthBefore);
    glGetIntegerv(GL_PROJECTION_STACK_DEPTH, &projectionDepthBefore);
    oldFog = glIsEnabled(GL_FOG);
    oldLighting = rndLightingEnable(FALSE);
    oldTexture = rndTextureEnable(FALSE);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadMatrixf((GLfloat const*)&rndProjectionMatrix);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadMatrixf((GLfloat const*)&rndCameraMatrix);

    /* The one pair of numbers that settles where a misplaced ray comes from.
       Overlays are drawn with whatever rndCameraMatrix holds at this instant,
       while ray->origin was placed with lookatInv - and the two only cancel
       if this matrix really is (eye view * the captured game lookat).

       Read the translation. Composed with the game lookat it is game-scale,
       tens of thousands of units. A bare eye view is head-scale, a metre or
       two: that would mean the world was never drawn this pass and the rays
       are being drawn in head space, which is exactly what "stuck to my head"
       looks like. */
    if (vrw.debugFrame % VRW_DEBUG_INTERVAL == 1)
    {
        real32 const* m = (real32 const*)&rndCameraMatrix;

        SDL_Log("VRDBG OVERLAY frame=%u drawMatrix row0=(%.3f %.3f %.3f) "
                "t=(%.1f %.1f %.1f) |t|=%.1f manager=%s",
                (unsigned)vrw.debugFrame, m[0], m[4], m[8],
                m[12], m[13], m[14],
                fsqrt(m[12] * m[12] + m[13] * m[13] + m[14] * m[14]),
                vrWorldManagerName());
    }
    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_FOG);

    /* Sensor representation, drawn into the world the player is standing in
       rather than onto a panel. Same blobs the map draws, same visibility
       test, same primitive - only the camera differs, and here it is the
       player's own head. The blob the ray is on is drawn at Relic's full
       density in white, which is what the map has always done for the blob
       under the movement cursor. */
    if (vrw.sensorsOverlay)
    {
        blob const* hovered = vrWorldSensorsHoverBlob();
        Node* node;
        sdword sensorLevel = universe.curPlayerPtr->sensorLevel;
        vector planeCentre;
        vector camRight, camUp;
        real32 const* vm = (real32 const*)&rndCameraMatrix;

        /* Camera basis for the region glow, taken from this eye's own view
           matrix rather than the game camera: a billboard built from the
           mono camera would sit at a different depth in each eye and read as
           a smear rather than a sphere. Rows of the rotation part are the
           camera's world axes. */
        camRight.x = vm[0]; camRight.y = vm[4]; camRight.z = vm[8];
        camUp.x    = vm[1]; camUp.y    = vm[5]; camUp.z    = vm[9];

        /* The navigation disc, at the world plane the map uses. Its ring is a
           fixed radius, so this is the same frame of reference for direction
           and distance the map has always drawn - only now the player can
           walk around it. */
        planeCentre.x = planeCentre.y = planeCentre.z = 0.0f;
        smWorldPlaneDraw(&planeCentre, TRUE, smWorldPlaneColor);

        /* Regions first, then shells over them: the glow is additive and
           would wash the wireframe out if it went on top. */
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        for (node = universe.collBlobList.head; node != NULL; node = node->next)
        {
            blob* thisBlob = (blob*)listGetStructOfNode(node);
            color c;
            sdword seg;

            if (!((thisBlob->flags & (BTF_Explored | BTF_ProbeDroid))
                  || (sensorLevel == 2
                      && bitTest(thisBlob->flags, BTF_UncloakedEnemies))))
            {
                continue;
            }
            c = (thisBlob == hovered) ? colWhite : VRW_SENSOR_BLOB_COLOR;

            /* Camera-facing disc, bright at the centre and transparent at the
               rim, which is what the map's filled blob reads as. A real
               translucent sphere would be the honest shape, but it costs a
               shell per blob per eye for something the eye reads as a haze
               either way. */
            glBegin(GL_TRIANGLE_FAN);
            glColor4ub(colRed(c), colGreen(c), colBlue(c), VRW_SENSOR_GLOW_ALPHA);
            glVertex3fv((GLfloat const*)&thisBlob->centre);
            for (seg = 0; seg <= VRW_SENSOR_GLOW_SEGMENTS; seg++)
            {
                real32 a = (real32)seg * (2.0f * PI) / (real32)VRW_SENSOR_GLOW_SEGMENTS;
                real32 ca = (real32)cos((double)a) * thisBlob->radius;
                real32 sa = (real32)sin((double)a) * thisBlob->radius;
                vector rim;

                rim.x = thisBlob->centre.x + camRight.x * ca + camUp.x * sa;
                rim.y = thisBlob->centre.y + camRight.y * ca + camUp.y * sa;
                rim.z = thisBlob->centre.z + camRight.z * ca + camUp.z * sa;
                glColor4ub(colRed(c), colGreen(c), colBlue(c), 0);
                glVertex3fv((GLfloat const*)&rim);
            }
            glEnd();
        }
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        for (node = universe.collBlobList.head; node != NULL; node = node->next)
        {
            blob* thisBlob = (blob*)listGetStructOfNode(node);

            if (!((thisBlob->flags & (BTF_Explored | BTF_ProbeDroid))
                  || (sensorLevel == 2
                      && bitTest(thisBlob->flags, BTF_UncloakedEnemies))))
            {
                continue;
            }
            /* screenRadius is the map's own LOD input and is stale or zero
               while the map is closed, so the shell is drawn at a fixed
               tessellation here rather than reusing it. */
            if (thisBlob == hovered)
            {
                smBlobSphereDraw(thisBlob, colWhite, 40, 12);
            }
            else
            {
                smBlobSphereDraw(thisBlob, VRW_SENSOR_BLOB_COLOR, 60, 10);
            }
        }
    }

    for (hand = 0; hand < VRW_HAND_COUNT; hand++)
    {
        vrwray const* ray = &vrw.ray[hand];
        vector end;
        real32 length;
        color rayColor;

        if (!ray->valid)
        {
            continue;
        }
        switch (ray->intent)
        {
            case VRW_INTENT_SELECT:     rayColor = colRGB(90, 255, 120); break;
            case VRW_INTENT_ADD_SELECT: rayColor = colRGB(80, 255, 230); break;
            case VRW_INTENT_ATTACK:     rayColor = colRGB(255, 70, 70); break;
            case VRW_INTENT_HARVEST:    rayColor = colRGB(255, 190, 55); break;
            case VRW_INTENT_DOCK:       rayColor = colRGB(80, 220, 255); break;
            case VRW_INTENT_SPECIAL:    rayColor = colRGB(190, 130, 255); break;
            case VRW_INTENT_MOVE:       rayColor = colRGB(255, 255, 255); break;
            case VRW_INTENT_PANEL:      rayColor = colRGB(70, 220, 255); break;
            case VRW_INTENT_INVALID:    rayColor = colRGB(150, 150, 160); break;
            default:                    rayColor = colRGB(80, 160, 255); break;
        }
        /* A panel can be farther away than the normal free-space beam,
           especially when a manager is attached to a distant ship. */
        if (ray->limitT > 0.0f)
        {
            /* Input routing has already decided that the panel owns this
               ray. Do not let a projection-layer world pick shorten it. */
            length = ray->limitT;
        }
        else if (vrw.moveActive && hand == vrw.moveHand)
        {
            /* End the beam at the destination. Otherwise it runs to a fixed
               2.5m and, zoomed in closer than that, visibly overshoots where
               the order would actually land. */
            length = vrw.cursorDist;
        }
        else
        {
            length = ray->hover != NULL && ray->hoverT > 0.0f
                   ? ray->hoverT : (2.5f * vrw.scale);
        }
        end.x = ray->origin.x + ray->dir.x * length;
        end.y = ray->origin.y + ray->dir.y * length;
        end.z = ray->origin.z + ray->dir.z * length;
        primLine3(&ray->origin, &end, rayColor);

        /* controller gizmo: three axis rings around the hand position */
        {
            real32 gizmoRadius = 0.02f * vrw.scale;

            primCircleOutline3(&ray->origin, gizmoRadius, 12, 0, rayColor, X_AXIS);
            primCircleOutline3(&ray->origin, gizmoRadius, 12, 0, rayColor, 1);
            primCircleOutline3(&ray->origin, gizmoRadius, 12, 0, rayColor, Z_AXIS);
        }

        if (ray->hover != NULL && ray->intent != VRW_INTENT_PANEL)
        {
            primCircleOutline3(&ray->hover->collInfo.collPosition,
                               vrwHullRadius(ray->hover),
                               24, 0, rayColor, Z_AXIS);
        }
    }

    for (i = 0; i < selSelected.numShips; i++)
    {
        Ship* ship = selSelected.ShipPtr[i];

        primCircleOutline3(&ship->collInfo.collPosition,
                           vrwHullRadius((SpaceObjRotImpTarg*)ship) * 1.1f,
                           24, 0, selColor, Z_AXIS);
    }

    /* Sweep gizmo. The brush is a cone widening from the hand, not a box, so
       that is what gets drawn - a box would misstate which ships are actually
       inside it. Rings down the ray show the capture volume, and the trail of
       recent far-edge positions shows the region already swept. */
    if ((vrw.sweepActive || (vrw.targetSweepActive && vrw.targetSweepPainting))
        && vrw.sweepHand >= 0
        && vrw.ray[vrw.sweepHand].valid && vrw.sweepReach > 0.0f)
    {
        vrwray const* ray = &vrw.ray[vrw.sweepHand];
        color const brush = colRGB(90, 220, 255);
        color const trailColor = colRGB(50, 130, 170);
        sdword const rings = 4;
        sdword step;

        for (step = 1; step <= rings; step++)
        {
            real32 t = vrw.sweepReach * (real32)step / (real32)rings;
            vector centre;

            centre.x = ray->origin.x + ray->dir.x * t;
            centre.y = ray->origin.y + ray->dir.y * t;
            centre.z = ray->origin.z + ray->dir.z * t;
            /* same radius the accumulator uses at this depth, so the gizmo
               cannot promise a reach the picker does not honour */
            primCircleOutline3(&centre, t * VRW_SWEEP_CONE_TAN, 20, 0, brush,
                               Z_AXIS);
        }
        for (i = 1; i < vrw.sweepTrailCount; i++)
        {
            primLine3(&vrw.sweepTrail[i - 1], &vrw.sweepTrail[i], trailColor);
        }
        if (vrw.sweepTrailCount > 0)
        {
            primLine3(&ray->origin, &vrw.sweepTrail[0], trailColor);
            primLine3(&ray->origin,
                      &vrw.sweepTrail[vrw.sweepTrailCount - 1], trailColor);
        }
    }

    if (vrw.targetSweepActive)
    {
        color const attackColor = colRGB(255, 80, 70);

        for (i = 0; i < vrw.targetSweep.numTargets; i++)
        {
            SpaceObjRotImpTarg* obj = vrw.targetSweep.TargetPtr[i];

            primCircleOutline3(&obj->collInfo.collPosition,
                               vrwHullRadius(obj) * 1.3f,
                               20, 0, attackColor, Z_AXIS);
        }
    }

    for (i = 0; i < vrw.sweepPreview.numShips; i++)
    {
        Ship* ship = vrw.sweepPreview.ShipPtr[i];

        primCircleOutline3(&ship->collInfo.collPosition,
                           vrwHullRadius((SpaceObjRotImpTarg*)ship) * 1.2f,
                           16, 0, hoverColor, Z_AXIS);
    }

    /* Freehand path. While the hand is still drawing, the live spline through
       the raw samples; after that, the resampled curve the follower is
       actually working from. It keeps drawing once committed - dimmed behind
       the fleet, lit ahead of it, with a ring on the carrot - because seeing
       the route being tracked is the whole point of having drawn one. */
    if (vrw.pathDrawing || vrw.pathCount > 0)
    {
        color const strokeColor = colRGB(120, 200, 255);
        color const flownColor = colRGB(45, 75, 100);
        color const pointColor = colRGB(255, 235, 140);
        real32 ringSize = selAverageSize > 0.0f ? selAverageSize : 150.0f;
        sdword segment, step;

        /* only while the hand is still moving: once the stroke is finished the
           resampled curve below is the same shape, and drawing both doubles
           every line */
        if (vrw.pathDrawing && vrw.pathSampleCount >= 2)
        {
            vector previous = vrw.pathSample[0];

            for (segment = 0; segment + 1 < vrw.pathSampleCount; segment++)
            {
                for (step = 1; step <= VRW_PATH_SPLINE_STEPS; step++)
                {
                    vector current;

                    vrwSplineAt(segment,
                                (real32)step / (real32)VRW_PATH_SPLINE_STEPS,
                                &current);
                    primLine3(&previous, &current, strokeColor);
                    previous = current;
                }
            }
        }
        for (i = 1; i < vrw.pathCount; i++)
        {
            bool32 flown = vrw.pathActive
                && (real32)i * vrw.pathSpacing <= vrw.pathProgress;

            primLine3(&vrw.pathPoint[i - 1], &vrw.pathPoint[i],
                      flown ? flownColor : strokeColor);
        }
        /* Ends only. At this resolution a ring per point would be a tube. */
        if (vrw.pathCount > 0)
        {
            primCircleOutline3(&vrw.pathPoint[0], ringSize, 12, 0, pointColor,
                               Z_AXIS);
            primCircleOutline3(&vrw.pathPoint[vrw.pathCount - 1], ringSize, 12,
                               0, pointColor, Z_AXIS);
        }
        if (vrw.pathActive)
        {
            vector carrot;

            vrwPathPointAt(vrw.pathProgress + vrw.pathLead, &carrot);
            primCircleOutline3(&carrot, ringSize * 1.4f, 16, 0, moveColor,
                               Z_AXIS);
        }
    }

    if (vrw.moveActive)
    {
        /* The cursor is free in space now, so depth has to be readable some
           other way: drop it onto the selection's plane with a vertical line
           and ring the shadow, then join the shadow to the fleet. That trio
           is what tells the eye how far out and how high the point is. */
        vector shadow = vrw.moveDestination;
        real32 ring = selAverageSize > 0.0f ? selAverageSize : 100.0f;

        shadow.z = selCentrePoint.z;
        primCircleOutline3(&shadow, ring * 2.0f, 32, 0, moveColor, Z_AXIS);
        primLine3(&shadow, &vrw.moveDestination, moveColor);
        primLine3(&selCentrePoint, &shadow, moveColor);
        primCircleOutline3(&vrw.moveDestination, ring, 16, 0, hoverColor, Z_AXIS);
    }

    rndTextureEnable(oldTexture);
    rndLightingEnable(oldLighting);
    if (oldFog) glEnable(GL_FOG); else glDisable(GL_FOG);
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode((GLenum)oldMatrixMode);

    glGetIntegerv(GL_MODELVIEW_STACK_DEPTH, &modelDepthAfter);
    glGetIntegerv(GL_PROJECTION_STACK_DEPTH, &projectionDepthAfter);
    exitError = glGetError();
    if (vrw.debugFrame % VRW_DEBUG_INTERVAL == 1
        || modelDepthBefore != modelDepthAfter
        || projectionDepthBefore != projectionDepthAfter
        || entryError != GL_NO_ERROR || exitError != GL_NO_ERROR)
    {
        SDL_Log("VRDBG OVERLAY frame=%u stacks=%d/%d->%d/%d "
                "mode=0x%x entryErr=0x%x exitErr=0x%x blue=-Z",
                (unsigned)vrw.debugFrame, (int)modelDepthBefore,
                (int)projectionDepthBefore, (int)modelDepthAfter,
                (int)projectionDepthAfter, (unsigned)oldMatrixMode,
                (unsigned)entryError, (unsigned)exitError);
    }
}

#endif /* HW_ENABLE_VR */
