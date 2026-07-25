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
#include "Camera.h"
#include "CameraCommand.h"
#include "ConsMgr.h"
#include "FastMath.h"
#include "LaunchMgr.h"
#include "ResearchGUI.h"
#include "Sensors.h"
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
#include "TradeMgr.h"
#include "Tutor.h"
#include "Universe.h"
#include "Vector.h"
#include "mainrgn.h"
#include "Options.h"

extern Camera *mrCamera;                                    //mainrgn.c
extern bool32 gameIsRunning;                                //Globals.c

#define VRW_HAND_COUNT     2
#define VRW_PICK_MARGIN    1.25f    /* collision sphere inflation for picking */
#define VRW_PICK_STICKY    0.92f    /* score discount for the previous target */
#define VRW_PICK_MARGIN_MAX 150.0f  /* cap the margin so capitals stay fair */
#define VRW_PICK_MISS_BIAS  2.5f    /* how much an off-centre aim is punished */
#define VRW_PICK_CONE_TAN  0.012f   /* ~0.7 degree controller selection cone */
#define VRW_PICK_CONE_MAX  180.0f   /* cap distant-target assistance */
#define VRW_SWEEP_CONE_TAN 0.070f   /* ~4 degree brush: sweeping paints over
                                       a group rather than threading between
                                       ships one aim-cone at a time */
#define VRW_SWEEP_MARGIN   1.60f    /* collision inflation while sweeping */
#define VRW_RAY_LENGTH     100000.0f
#define VRW_CURSOR_MIN     400.0f   /* nearest the cursor may sit: 50 units is
                                       5cm of hologram, i.e. inside the hand */
#define VRW_DEBUG_INTERVAL 120

/* Freehand path drawing */
#define VRW_PATH_MAX_SAMPLES 64     /* raw swept points before decimation */
#define VRW_PATH_MAX_POINTS  12     /* waypoints handed to the follower */
#define VRW_PATH_SPLINE_STEPS 12    /* spline evaluations per raw segment */
#define VRW_PATH_LEG_TIMEOUT 45.0f  /* seconds before a stuck leg is skipped */

typedef struct {
    bool32  valid;
    vector  origin;                 /* game world units */
    vector  dir;                    /* unit direction   */
    SpaceObjRotImpTarg *hover;      /* object under this ray, if any */
    real32  hoverT;                 /* ray parameter of the hover hit */
    real32  limitT;                 /* draw clip (panel hit), 0 = none */
    vrworldintent intent;
} vrwray;

static struct {
    bool32  worldValid;
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
    real32  pathSpacing;            /* arc length between waypoints */

    /* freehand path: resampled waypoints and the follower flying them */
    vector  pathPoint[VRW_PATH_MAX_POINTS];
    sdword  pathCount;
    bool32  pathActive;
    MaxSelection pathSelection;
    sdword  pathLeg;
    real32  pathArriveDist;
    real32  pathLegStart;           /* universe time the leg was issued */
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

bool32 vrWorldToggleBuildManager(void)
{
    if (!vrw.worldValid)
    {
        return FALSE;
    }
    if (cmActive)
    {
        cmCloseIfOpen();
        return !cmActive;
    }

    /* Manager modes are mutually exclusive. Match the taskbar behavior by
       closing another full-screen manager before opening Construction. */
    lmCloseIfOpen();
    rmCloseIfOpen();
    tutGameMessage("KB_Build");
    mrBuildShips(NULL, NULL);
    return cmActive;
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
        case VRW_CMD_HALT:
        case VRW_CMD_SPECIAL:           return !vrwOrdersBlocked();
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

        case VRW_CMD_HALT:
            clWrapHalt(&universe.mainCommandLayer,
                       (SelectCommand*)&selSelected);
            break;
        case VRW_CMD_SPECIAL:
            /* NULL targets is the self-activate form, matching Z-release */
            clWrapSpecial(&universe.mainCommandLayer,
                          (SelectCommand*)&selSelected, NULL);
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
        case VRW_CMD_BUILD:      mrBuildShips(NULL, NULL);       break;
        case VRW_CMD_LAUNCH:     mrLaunch(NULL, NULL);           break;
        case VRW_CMD_RESEARCH:   mrResearch(NULL, NULL);         break;
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
        case VRW_CMD_SENSORS:    smSensorsBegin(NULL, NULL);     break;
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

   Candidates are ranked by the depth of their CENTRE, penalised for how far
   off-centre the aim is - not by the depth at which the ray enters the
   sphere. Entry depth sounds right and is badly wrong when sizes differ by
   two orders of magnitude: the Mothership's hull begins some 2600 units
   ahead of its centre, so ranking by entry made it beat every fighter parked
   in front of it, no matter how precisely the beam was on the fighter. */
static SpaceObjRotImpTarg* vrwPick(vrwray const* ray, bool32 selectableOnly,
                                   SpaceObjRotImpTarg const* preferred, real32* hitT)
{
    Node* node;
    SpaceObjRotImpTarg* best = NULL;
    real32 bestScore = REALlyBig;
    real32 bestSurface = 0.0f;

    for (node = universe.RenderList.head; node != NULL; node = node->next)
    {
        SpaceObjRotImpTarg* obj = (SpaceObjRotImpTarg*)listGetStructOfNode(node);
        vector toObj;
        real32 radius, tCentre, distSqr, discr, root;
        real32 hull, margin, assist, missFrac, score;

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
        hull = obj->staticinfo->staticheader.staticCollInfo.collspheresize;
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

        /* 0 when the beam is dead on the centre, 1 when it just grazes. A
           grazing hit has to be much nearer to win than a centred one. */
        missFrac = radius > 0.0f ? distSqr / (radius * radius) : 1.0f;
        score = tCentre * (1.0f + missFrac * VRW_PICK_MISS_BIAS);
        if (obj == preferred)
        {
            /* hysteresis belongs in the ranking, not the geometry: inflating
               the previous target's sphere made big ships stickier than small
               ones, which is backwards */
            score *= VRW_PICK_STICKY;
        }
        if (score < bestScore)
        {
            bestScore = score;
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

/* Sweep-select brush. This is the VR stand-in for the desktop band-box, so
   it behaves like one: every selectable player ship the beam passes over
   joins the preview - not just the nearest - and the capture radius widens
   with distance exactly as a screen-space box does with depth. Painting
   across a cluster therefore takes the whole cluster in one pass. */
static void vrwSweepAccumulate(vrwray const* ray)
{
    Node* node;

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
        radius = obj->staticinfo->staticheader.staticCollInfo.collspheresize;
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

bool32 vrWorldSetRay(sdword hand, real32 const origin[3], real32 const dir[3], bool32 valid)
{
    vrwray* ray = &vrw.ray[hand];
    SpaceObjRotImpTarg* oldHover = ray->hover;

    ray->valid = valid && vrw.worldValid;
    ray->hover = NULL;
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
    ray->hover = vrwPick(ray, FALSE, oldHover, &ray->hoverT);
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
    return vrw.ray[hand].valid && vrw.ray[hand].hover != NULL;
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
        if (!additive && selSelected.numShips > 0)
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

void vrWorldSweepBegin(sdword hand)
{
    vrw.sweepActive = TRUE;
    vrw.sweepHand = hand;
    vrw.sweepPreview.numShips = 0;
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
    return changed;
}

void vrWorldSweepCancel(void)
{
    vrw.sweepActive = FALSE;
    vrw.sweepPreview.numShips = 0;
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

    Homeworld has no waypoint order: clWrapMove takes a single destination and
    replaces whatever the ships were doing. So a drawn path is kept here and
    flown leg by leg, re-issuing a plain move each time the previous leg is
    reached. The stroke itself is a Catmull-Rom spline through the swept
    points, resampled by arc length so waypoint spacing is even regardless of
    how fast the hand moved.
----------------------------------------------------------------------------*/
static real32 vrwDistance(vector const* a, vector const* b)
{
    vector d;

    vecSub(d, *a, *b);
    return fsqrt(vecMagnitudeSquared(d));
}

/* Catmull-Rom through p1..p2, with p0/p3 setting the tangents */
static void vrwSpline(vector const* p0, vector const* p1, vector const* p2,
                      vector const* p3, real32 t, vector* out)
{
    real32 t2 = t * t;
    real32 t3 = t2 * t;
    real32 a = -0.5f * t3 +  1.0f * t2 - 0.5f * t;
    real32 b =  1.5f * t3 -  2.5f * t2            + 1.0f;
    real32 c = -1.5f * t3 +  2.0f * t2 + 0.5f * t;
    real32 d =  0.5f * t3 -  0.5f * t2;

    out->x = a * p0->x + b * p1->x + c * p2->x + d * p3->x;
    out->y = a * p0->y + b * p1->y + c * p2->y + d * p3->y;
    out->z = a * p0->z + b * p1->z + c * p2->z + d * p3->z;
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

/* Trigger released: smooth the stroke and lay waypoints along it evenly. */
bool32 vrWorldPathFinishStroke(void)
{
    sdword segment, step;
    real32 total = 0.0f, spacing, travelled = 0.0f;
    vector previous, current;

    if (!vrw.pathDrawing)
    {
        return FALSE;
    }
    vrw.pathDrawing = FALSE;
    vrw.pathCount = 0;
    if (vrw.pathSampleCount < 2)
    {
        SDL_Log("VR: path stroke too short (%d samples)",
                (int)vrw.pathSampleCount);
        return FALSE;
    }

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
    if (total <= 0.0f)
    {
        return FALSE;
    }

    /* Spacing follows the path length so the waypoint budget is never
       exceeded and a long sweep does not become a dense stutter of orders. */
    spacing = total / (real32)VRW_PATH_MAX_POINTS;
    vrw.pathSpacing = spacing;
    previous = vrw.pathSample[0];
    for (segment = 0; segment + 1 < vrw.pathSampleCount; segment++)
    {
        for (step = 1; step <= VRW_PATH_SPLINE_STEPS; step++)
        {
            vrwSplineAt(segment, (real32)step / (real32)VRW_PATH_SPLINE_STEPS,
                        &current);
            travelled += vrwDistance(&previous, &current);
            previous = current;
            if (travelled >= spacing && vrw.pathCount < VRW_PATH_MAX_POINTS - 1)
            {
                vrw.pathPoint[vrw.pathCount++] = current;
                travelled = 0.0f;
            }
        }
    }
    /* finish exactly where the hand stopped */
    if (vrw.pathCount < VRW_PATH_MAX_POINTS)
    {
        vrw.pathPoint[vrw.pathCount++] =
            vrw.pathSample[vrw.pathSampleCount - 1];
    }
    SDL_Log("VR: path stroke done: %d samples, length=%.0f spacing=%.0f "
            "-> %d waypoints", (int)vrw.pathSampleCount, total, spacing,
            (int)vrw.pathCount);
    return vrw.pathCount > 0;
}

bool32 vrWorldPathCommit(void)
{
    sdword i;
    real32 maxColl = 0.0f;

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
        real32 coll = vrw.pathSelection.ShipPtr[i]->staticinfo
                      ->staticheader.staticCollInfo.collspheresize;

        if (coll > maxColl)
        {
            maxColl = coll;
        }
    }
    /* Arrive generously: the follower only needs to know the leg is done, and
       a tight radius on a spread-out group would stall the whole path until
       the timeout rescued it. Scale with leg length as well as group size. */
    vrw.pathArriveDist = maxColl * 3.0f + 0.10f * vrw.scale;
    if (vrw.pathArriveDist < vrw.pathSpacing * 0.25f)
    {
        vrw.pathArriveDist = vrw.pathSpacing * 0.25f;
    }

    selCentrePointCompute();
    vrw.pathActive = TRUE;
    vrw.pathSampleCount = 0;            //the drawn stroke has served its purpose
    vrw.pathLeg = 0;
    vrw.pathLegStart = universe.totaltimeelapsed;
    vrw.pathLastUpdate = universe.totaltimeelapsed;
    clWrapMove(&universe.mainCommandLayer, (SelectCommand*)&vrw.pathSelection,
               selCentrePoint, vrw.pathPoint[0]);
    tutGameMessage("Game_MoveIssued");
    SDL_Log("VR: path commit: %d ships, %d legs, arrive=%.0f, leg 0 -> "
            "(%.0f %.0f %.0f)", (int)vrw.pathSelection.numShips,
            (int)vrw.pathCount, vrw.pathArriveDist, vrw.pathPoint[0].x,
            vrw.pathPoint[0].y, vrw.pathPoint[0].z);
    return TRUE;
}

void vrWorldPathCancel(void)
{
    if (vrw.pathDrawing || vrw.pathActive || vrw.pathCount > 0)
    {
        SDL_Log("VR: path cancelled (drawing=%d active=%d leg=%d/%d)",
                (int)vrw.pathDrawing, (int)vrw.pathActive, (int)vrw.pathLeg,
                (int)vrw.pathCount);
    }
    vrw.pathDrawing = FALSE;
    vrw.pathActive = FALSE;
    vrw.pathSampleCount = 0;
    vrw.pathCount = 0;
    vrw.pathLeg = 0;
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

/* Advance the committed path. Called once per frame from vrWorldFrameBegin;
   gated on universe time so the extra rndFlush passes a manager triggers
   cannot re-issue the same leg several times and stall the ships. */
static void vrwPathFollow(void)
{
    vector centre;
    sdword i, alive = 0;
    real32 invAlive, toLeg;
    bool32 reached, timedOut;

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

    toLeg = vrwDistance(&centre, &vrw.pathPoint[vrw.pathLeg]);
    reached = toLeg <= vrw.pathArriveDist;
    timedOut = (universe.totaltimeelapsed - vrw.pathLegStart)
               > VRW_PATH_LEG_TIMEOUT;
    if (!reached && !timedOut)
    {
        if (vrw.debugFrame % VRW_DEBUG_INTERVAL == 1)
        {
            SDL_Log("VR: path leg %d/%d, %d ship(s) %.0f away, arrive at %.0f",
                    (int)vrw.pathLeg, (int)vrw.pathCount, (int)alive, toLeg,
                    vrw.pathArriveDist);
        }
        return;
    }
    if (timedOut && !reached)
    {
        SDL_Log("VR: path leg %d timed out, skipping", (int)vrw.pathLeg);
    }

    vrw.pathLeg++;
    if (vrw.pathLeg >= vrw.pathCount)
    {
        SDL_Log("VR: path complete (%d legs)", (int)vrw.pathCount);
        vrw.pathActive = FALSE;
        vrw.pathCount = 0;
        vrw.pathSelection.numShips = 0;
        return;
    }
    vrw.pathLegStart = universe.totaltimeelapsed;
    clWrapMove(&universe.mainCommandLayer, (SelectCommand*)&vrw.pathSelection,
               centre, vrw.pathPoint[vrw.pathLeg]);
    SDL_Log("VR: path leg %d/%d -> (%.0f %.0f %.0f)", (int)vrw.pathLeg,
            (int)vrw.pathCount, vrw.pathPoint[vrw.pathLeg].x,
            vrw.pathPoint[vrw.pathLeg].y, vrw.pathPoint[vrw.pathLeg].z);
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
    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_FOG);

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
                               ray->hover->staticinfo->staticheader.staticCollInfo.collspheresize,
                               24, 0, rayColor, Z_AXIS);
        }
    }

    for (i = 0; i < selSelected.numShips; i++)
    {
        Ship* ship = selSelected.ShipPtr[i];

        primCircleOutline3(&ship->collInfo.collPosition,
                           ship->staticinfo->staticheader.staticCollInfo.collspheresize * 1.1f,
                           24, 0, selColor, Z_AXIS);
    }

    for (i = 0; i < vrw.sweepPreview.numShips; i++)
    {
        Ship* ship = vrw.sweepPreview.ShipPtr[i];

        primCircleOutline3(&ship->collInfo.collPosition,
                           ship->staticinfo->staticheader.staticCollInfo.collspheresize * 1.2f,
                           16, 0, hoverColor, Z_AXIS);
    }

    /* Freehand path: the smoothed spline as a fine polyline, with a ring at
       each waypoint the ships will be ordered to. This is purely a planning
       aid - once the path is committed and the ships are flying it, it has
       served its purpose and stops drawing. */
    if (!vrw.pathActive && (vrw.pathDrawing || vrw.pathCount > 0))
    {
        color const strokeColor = colRGB(120, 200, 255);
        color const pointColor = colRGB(255, 235, 140);
        real32 ringSize = selAverageSize > 0.0f ? selAverageSize : 150.0f;
        sdword segment, step;

        if (vrw.pathSampleCount >= 2)
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
        for (i = 0; i < vrw.pathCount; i++)
        {
            primCircleOutline3(&vrw.pathPoint[i], ringSize, 12, 0, pointColor,
                               Z_AXIS);
            if (i > 0)
            {
                primLine3(&vrw.pathPoint[i - 1], &vrw.pathPoint[i],
                          strokeColor);
            }
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
