/*=============================================================================
    Name    : vrworld.c
    Purpose : VR-native interaction with the game world (see vrworld.h).

    Coordinate pipeline: rendered geometry goes
        game world -> [game camera lookat] -> [anchor, scaled] -> LOCAL -> eye
    so a controller pose in LOCAL space maps into the game world through the
    inverses: world = lookat^-1 * anchor^-1 * (pose * VR_WORLD_SCALE).
    The pure lookat matrix is captured from rndCameraMatrix, which the mono
    render pass fills before vrFrame runs (the eye passes overwrite it later
    with per-eye composites, so the capture happens in vrWorldFrameBegin).

    Created 24/07/2026
=============================================================================*/

#ifdef HW_ENABLE_VR

#include "vrworld.h"

#include <math.h>
#include <string.h>

#include "Camera.h"
#include "CameraCommand.h"
#include "FastMath.h"
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
#include "Tutor.h"
#include "Universe.h"
#include "Vector.h"
#include "mainrgn.h"
#include "Options.h"

extern Camera *mrCamera;                                    //mainrgn.c
extern bool32 gameIsRunning;                                //Globals.c

#define VRW_HAND_COUNT     2
#define VRW_PICK_MARGIN    1.4f     /* collision sphere inflation for picking */
#define VRW_SWEEP_RADIUS   0.12f    /* LOCAL-space metres around the ray */
#define VRW_RAY_LENGTH     100000.0f

typedef struct {
    bool32  valid;
    vector  origin;                 /* game world units */
    vector  dir;                    /* unit direction   */
    SpaceObjRotImpTarg *hover;      /* object under this ray, if any */
    real32  hoverT;                 /* ray parameter of the hover hit */
} vrwray;

static struct {
    bool32  worldValid;
    real32  scale;                  /* metres -> game units */
    real32  lookatInv[16];          /* inverse of the game camera lookat */
    real32  anchorInv[16];          /* inverse of the scaled anchor pose */
    vrwray  ray[VRW_HAND_COUNT];

    /* sweep select */
    bool32  sweepActive;
    sdword  sweepHand;
    MaxSelection sweepPreview;

    /* move order */
    bool32  moveActive;
    sdword  moveHand;
    vector  movePlanePoint;         /* ray/plane intersection (XY at height) */
    vector  moveDestination;        /* plane point + vertical offset */
    real32  movePlaneZ;
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
            out[c * 4 + r] = R[r * 3 + c];                  /* transpose */
        }
        out[c * 4 + 3] = 0.0f;
    }
    for (r = 0; r < 3; r++)
    {
        out[12 + r] = -(R[r * 3 + 0] * t[0] + R[r * 3 + 1] * t[1] + R[r * 3 + 2] * t[2]);
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

bool32 vrWorldFrameBegin(real32 const anchorPos[3], real32 const anchorQuat[4], real32 scale)
{
    real32 R[9], t[3];
    real32 const* look = (real32 const*)&rndCameraMatrix;
    real32 lookR[9], lookT[3];
    sdword r, c;

    vrw.worldValid = FALSE;
    if (!gameIsRunning || mrCamera == NULL)
    {
        return FALSE;
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

    vrw.worldValid = TRUE;
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

/* nearest ray/sphere hit along the render list. selectableOnly restricts to
   the current player's selectable ships. */
static SpaceObjRotImpTarg* vrwPick(vrwray const* ray, bool32 selectableOnly, real32* hitT)
{
    Node* node;
    SpaceObjRotImpTarg* best = NULL;
    real32 bestT = REALlyBig;

    for (node = universe.RenderList.head; node != NULL; node = node->next)
    {
        SpaceObjRotImpTarg* obj = (SpaceObjRotImpTarg*)listGetStructOfNode(node);
        vector toObj;
        real32 radius, tCentre, distSqr, discr;

        if (obj->objtype != OBJ_ShipType && obj->objtype != OBJ_AsteroidType
            && obj->objtype != OBJ_DustType && obj->objtype != OBJ_GasType
            && obj->objtype != OBJ_DerelictType)
        {
            continue;
        }
        if (selectableOnly)
        {
            if (obj->objtype != OBJ_ShipType
                || ((Ship*)obj)->playerowner != universe.curPlayerPtr
                || ((Ship*)obj)->shiptype == Drone)
            {
                continue;
            }
        }
        if (obj->flags & SOF_Dead)
        {
            continue;
        }

        radius = obj->staticinfo->staticheader.staticCollInfo.collspheresize * VRW_PICK_MARGIN;
        vecSub(toObj, obj->collInfo.collPosition, ray->origin);
        tCentre = vecDotProduct(toObj, ray->dir);
        if (tCentre < 0.0f)
        {
            continue;                                       //behind the controller
        }
        distSqr = vecMagnitudeSquared(toObj) - tCentre * tCentre;
        discr = radius * radius - distSqr;
        if (discr < 0.0f)
        {
            continue;
        }
        if (tCentre - fsqrt(discr) < bestT)
        {
            bestT = tCentre - fsqrt(discr);
            best = obj;
        }
    }

    if (best != NULL && hitT != NULL)
    {
        *hitT = bestT;
    }
    return best;
}

void vrWorldSetRay(sdword hand, real32 const origin[3], real32 const dir[3], bool32 valid)
{
    vrwray* ray = &vrw.ray[hand];

    ray->valid = valid && vrw.worldValid;
    ray->hover = NULL;
    if (!ray->valid)
    {
        return;
    }
    vrwLocalToWorld(origin, &ray->origin);
    vrwLocalDirToWorld(dir, &ray->dir);
    ray->hover = vrwPick(ray, FALSE, &ray->hoverT);

    /* while sweeping, accumulate player ships near the ray */
    if (vrw.sweepActive && hand == vrw.sweepHand)
    {
        real32 t;
        SpaceObjRotImpTarg* obj = vrwPick(ray, TRUE, &t);

        if (obj != NULL && vrw.sweepPreview.numShips < COMMAND_MAX_SHIPS
            && !selShipInSelection(vrw.sweepPreview.ShipPtr, vrw.sweepPreview.numShips, (Ship*)obj))
        {
            vrw.sweepPreview.ShipPtr[vrw.sweepPreview.numShips++] = (Ship*)obj;
        }
    }

    /* move order follows the ray */
    if (vrw.moveActive && hand == vrw.moveHand)
    {
        vector far;

        far.x = ray->origin.x + ray->dir.x * VRW_RAY_LENGTH;
        far.y = ray->origin.y + ray->dir.y * VRW_RAY_LENGTH;
        far.z = ray->origin.z + ray->dir.z * VRW_RAY_LENGTH;
        vecLineIntersectWithXYPlane(&vrw.movePlanePoint, &ray->origin, &far, vrw.movePlaneZ);
    }
}

bool32 vrWorldHandHasTarget(sdword hand)
{
    return vrw.ray[hand].valid && vrw.ray[hand].hover != NULL;
}

/*-----------------------------------------------------------------------------
    Selection
----------------------------------------------------------------------------*/
static bool32 vrwOrdersBlocked(void)
{
    return (universePause && !opPauseOrders) || mrDisabled;
}

void vrWorldSelectClick(sdword hand, bool32 additive)
{
    SpaceObjRotImpTarg* obj = vrw.ray[hand].valid ? vrw.ray[hand].hover : NULL;

    if (obj == NULL || obj->objtype != OBJ_ShipType
        || ((Ship*)obj)->playerowner != universe.curPlayerPtr)
    {
        if (!additive)
        {
            selSelectNone();
            ioUpdateShipTotals();
        }
        return;
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
    }
    else
    {
        selSelectionSetSingleShip((Ship*)obj);
    }
    ioUpdateShipTotals();
}

void vrWorldSweepBegin(sdword hand)
{
    vrw.sweepActive = TRUE;
    vrw.sweepHand = hand;
    vrw.sweepPreview.numShips = 0;
}

void vrWorldSweepCommit(sdword hand, bool32 additive)
{
    sdword i;

    if (!vrw.sweepActive)
    {
        return;
    }
    vrw.sweepActive = FALSE;
    if (vrw.sweepPreview.numShips == 0)
    {
        return;
    }
    if (!additive)
    {
        selSelectNone();
    }
    for (i = 0; i < vrw.sweepPreview.numShips; i++)
    {
        if (!selShipInSelection(selSelected.ShipPtr, selSelected.numShips, vrw.sweepPreview.ShipPtr[i]))
        {
            selSelectionAddSingleShip(&selSelected, vrw.sweepPreview.ShipPtr[i]);
        }
    }
    ioUpdateShipTotals();
    tutGameMessage("Game_SelectingRect");
    vrw.sweepPreview.numShips = 0;
}

void vrWorldSweepCancel(void)
{
    vrw.sweepActive = FALSE;
    vrw.sweepPreview.numShips = 0;
}

/*-----------------------------------------------------------------------------
    Orders
----------------------------------------------------------------------------*/
bool32 vrWorldContextOrder(sdword hand)
{
    SpaceObjRotImpTarg* obj = vrw.ray[hand].valid ? vrw.ray[hand].hover : NULL;
    MaxSelection tempSelection;

    if (obj == NULL || selSelected.numShips == 0 || vrwOrdersBlocked())
    {
        return FALSE;
    }

    if (obj->objtype == OBJ_AsteroidType || obj->objtype == OBJ_DustType
        || obj->objtype == OBJ_GasType)
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

    if (obj->objtype != OBJ_ShipType)
    {
        return FALSE;
    }

    if (((Ship*)obj)->playerowner == universe.curPlayerPtr)
    {                                                       //own ship: dock at it
        clWrapDock(&universe.mainCommandLayer, (SelectCommand*)&selSelected,
                   DOCK_AT_SPECIFIC_SHIP, (Ship*)obj);
        tutGameMessage("Game_DoubleClickDock");
        return TRUE;
    }

    /* enemy: attack */
    if (MakeShipsAttackCapable((SelectCommand*)&tempSelection, (SelectCommand*)&selSelected))
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
    if (selSelected.numShips == 0 || vrwOrdersBlocked() || !vrw.ray[hand].valid)
    {
        return FALSE;
    }
    selCentrePointCompute();
    vrw.moveActive = TRUE;
    vrw.moveHand = hand;
    vrw.movePlaneZ = selCentrePoint.z;
    vrw.movePlanePoint = selCentrePoint;
    vrw.moveDestination = selCentrePoint;
    tutGameMessage("Game_Move");
    return TRUE;
}

void vrWorldMoveUpdate(sdword hand, real32 heightMetres)
{
    if (!vrw.moveActive || hand != vrw.moveHand)
    {
        return;
    }
    vrw.moveDestination = vrw.movePlanePoint;
    vrw.moveDestination.z += heightMetres * vrw.scale;
    if (heightMetres != 0.0f)
    {
        tutGameMessage("Game_MoveZ");
    }
    else
    {
        tutGameMessage("Game_MoveXY");
    }
}

void vrWorldMoveCommit(void)
{
    if (!vrw.moveActive)
    {
        return;
    }
    vrw.moveActive = FALSE;
    if (selSelected.numShips == 0 || vrwOrdersBlocked())
    {
        return;
    }
    MakeShipsMobile((SelectCommand*)&selSelected);
    if (selSelected.numShips > 0)
    {
        clWrapMove(&universe.mainCommandLayer, (SelectCommand*)&selSelected,
                   selCentrePoint, vrw.moveDestination);
        tutGameMessage("Game_MoveIssued");
    }
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

void vrWorldCameraPan(real32 const deltaMetres[3])
{
    vector deltaCam, deltaWorld, newLookat;

    if (vrwCameraLocked())
    {
        return;
    }
    /* hand delta is in LOCAL space = (scaled) camera space; rotate into the
       world with the lookat inverse so "drag right" pans consistently */
    deltaCam.x = -deltaMetres[0] * vrw.scale;
    deltaCam.y = -deltaMetres[1] * vrw.scale;
    deltaCam.z = -deltaMetres[2] * vrw.scale;
    vrwTransformDir(vrw.lookatInv, &deltaCam, &deltaWorld);

    vecAdd(newLookat, vrwRememberCamera()->lookatpoint, deltaWorld);
    cameraChangeLookatpoint(vrwRememberCamera(), &newLookat);
    vecAdd(newLookat, vrwActualCamera()->lookatpoint, deltaWorld);
    cameraChangeLookatpoint(vrwActualCamera(), &newLookat);
    vrwCameraTouched(CAM_USER_MOVED);
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

/*-----------------------------------------------------------------------------
    Overlays (drawn per eye in game-world space)
----------------------------------------------------------------------------*/
void vrWorldDrawOverlays(void)
{
    sdword hand, i;
    color const rayColor = colRGB(80, 160, 255);
    color const hoverColor = colRGB(255, 200, 60);
    color const selColor = colRGB(90, 255, 120);
    color const moveColor = colRGB(255, 255, 255);

    if (!vrw.worldValid)
    {
        return;
    }

    /* the world render leaves lighting/texturing on, which blacks out
       unlit primitives - same preamble as the pie plate drawing */
    primModeClear2();
    rndLightingEnable(FALSE);
    rndTextureEnable(FALSE);

    for (hand = 0; hand < VRW_HAND_COUNT; hand++)
    {
        vrwray const* ray = &vrw.ray[hand];
        vector end;
        real32 length;

        if (!ray->valid)
        {
            continue;
        }
        length = (ray->hover != NULL) ? ray->hoverT : (2.5f * vrw.scale);
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

        if (ray->hover != NULL)
        {
            primCircleOutline3(&ray->hover->collInfo.collPosition,
                               ray->hover->staticinfo->staticheader.staticCollInfo.collspheresize,
                               24, 0, hoverColor, Z_AXIS);
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

    if (vrw.moveActive)
    {
        vector vertical = vrw.movePlanePoint;

        primCircleOutline3(&vrw.movePlanePoint, selAverageSize > 0.0f ? selAverageSize * 2.0f : 200.0f,
                           32, 0, moveColor, Z_AXIS);
        primLine3(&vrw.movePlanePoint, &vrw.moveDestination, moveColor);
        vertical.z = selCentrePoint.z;
        primLine3(&selCentrePoint, &vrw.movePlanePoint, moveColor);
        primCircleOutline3(&vrw.moveDestination, selAverageSize > 0.0f ? selAverageSize : 100.0f,
                           16, 0, hoverColor, Z_AXIS);
    }
}

#endif /* HW_ENABLE_VR */
