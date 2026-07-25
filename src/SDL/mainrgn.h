/*=============================================================================
    Name    : mainrgn.h
    Purpose : Logic for handling region events for the main game screen.

    Created 6/27/1997 by lmoloney
    Copyright Relic Entertainment, Inc.  All rights reserved.
=============================================================================*/

#ifndef ___MAINRGN_H
#define ___MAINRGN_H

#include "Camera.h"
#include "FEFlow.h"
#include "SpaceObj.h"
#include "Types.h"

/*=============================================================================
    Switches:
=============================================================================*/
#define MR_GUI_SINGLECLICK          1

#ifdef HW_BUILD_FOR_DEBUGGING

#define MR_ERROR_CHECKING           1           //general error checking
#define MR_VERBOSE_LEVEL            2           //control specific output code
#define MR_TEST_HPB                 0           //test heading/pitch/bank interactively
#define MR_RELEASE_MOUSE            1           //allow the mouse to be freed from the window
#define MR_TEST_GUNS                1           //allow the game testing mode
#define MR_SOUND_RELOAD_VOLUMES     1           //permits reloading of volume tables
#define MR_CAN_FOCUS_ROIDS          1           //can focus on asteroids,dust clouds and derelicts with an alt-click
#define MR_KEYBOARD_CHEATS          1           //enable typing in cheats on the keyboard

#else

#define MR_ERROR_CHECKING           0           //general error checking
#define MR_VERBOSE_LEVEL            0           //control specific output code
#define MR_TEST_PING                0           //test ping time
#define MR_TEST_HPB                 0           //test heading/pitch/bank interactively
#define MR_RELEASE_MOUSE            0           //allow the mouse to be freed from the window
#define MR_TEST_GUNS                0           //allow the game testing mode
#define MR_SOUND_RELOAD_VOLUMES     0           //permits reloading of volume tables
#define MR_CAN_FOCUS_ROIDS          0           //can focus on asteroids,dust clouds and derelicts with an alt-click
#define MR_KEYBOARD_CHEATS          0           //enable typing in cheats on the keyboard

#endif

/*=============================================================================
    Definitions:
=============================================================================*/
//message display definitions
#define MR_MESSAGE_LINE_SPACING     12
#define MR_MESSAGE_LINE_START       400
#define MR_MouseMovementClickLimit  12
#define MR_FormationDelay           1.0f
#define MR_NumberDoublePressTime    0.5
#define MR_FastMoveShipClickTime    0.5
#define MR_FastMoveShipClickX       5
#define MR_FastMoveShipClickY       5

#define MR_LetterboxGrey            0.5f, 0.5f, 0.5f

/*=============================================================================
    Data:
=============================================================================*/
//data exported for the benefit of other modules with similar functionality
extern bool32 mrWhiteOut;
extern real32 mrWhiteOutT;
extern sdword mrRenderMainScreen;
extern sdword mrOldMouseX, mrOldMouseY;
extern sdword mrMouseHasMoved;
extern sdword mrMouseMovementClickLimit;

//extern ShipPtr mrLastClosestShip;
extern real32 MAX_INGAME_MOVECOMMAND_DISTANCE;
extern real32 mrClosestDistance;

//camera rendering in the main region
extern Camera *mrCamera;

extern void (*mrHoldLeft)(void);
extern void (*mrHoldRight)(void);

extern char *mrMenuItemByTactic[];

extern bool32 helpinfoactive;

extern real32 mrNumberDoublePressTime;
extern sdword mrLastKeyPressed;
extern real32 mrLastKeyTime;

extern bool32 mrDisabled;

#if MR_CAN_FOCUS_ROIDS
extern bool32 mrCanFocusRoids;
#endif

/*=============================================================================
    Functions:
=============================================================================*/
//startup/shutdown of main region code
void mrStartup(void);
void mrShutdown(void);
void mrReset(void);

void mrShipDied(struct Ship *ship);

//region-process routine for the main region
udword mrRegionProcess(struct tagRegion *reg, sdword ID, udword event, udword data);
//region-draw routine for the main region
void mrRegionDraw(regionhandle reg);

//bit fields for the action mask
#define MAM_Info                0x00000001
#define MAM_Formations          0x00000002
#define MAM_Tactics             0x00000004
#define MAM_Move                0x00000008
#define MAM_Scuttle             0x00000010
#define MAM_Retire              0x00000020
#define MAM_Divider             0x00000040
#define MAM_Dock                0x00000080
#define MAM_Launch              0x00000100
#define MAM_Harvest             0x00000200
#define MAM_Build               0x00000400
#define MAM_Research            0x00000800
#define MAM_Trade               0x00001000
#define MAM_Hyperspace          0x00002000
#define MAM_BasicSet            (MAM_Info | MAM_Formations | MAM_Tactics | MAM_Move)
#define MAM_MedSet              MAM_BasicSet | MAM_Scuttle
#define MAM_ExtSet              MAM_BasicSet | MAM_Scuttle | MAM_Retire

//OR of the action bits for the current selection - shared with the VR
//command wheel so it offers exactly what the right-click menu would
udword mrSelectionActionMask(void);

//right-click callback functions
void mrDockingOrders(char *string, featom *atom);
void mrDeltaFormation(char *string, featom *atom);
void mrBroadFormation(char *string, featom *atom);
void mrXFormation(char *string, featom *atom);
void mrClawFormation(char *string, featom *atom);
void mrWallFormation(char *string, featom *atom);
void mrSphereFormation(char *string, featom *atom);
void mrCustomFormation(char *string, featom *atom);
void mrHarvestResources(char *string, featom *atom);
void mrBuildShips(char *string, featom *atom);
void mrTradeStuff(char *string, featom *atom);
void mrMoveShips(char *string, featom *atom);
void mrInfo(char *string, featom *atom);
void mrCancel(char *string, featom *atom);
void mrScuttle(char *string, featom *atom);
void mrRetire(char *string, featom *atom);
void mrUpdateHyperspaceStatus(bool32 goForLaunch);
void mrHyperspace(char *string, featom *atom);
void mrLaunch(char *string, featom *atom);
void mrResearch(char *string, featom *atom);
void mrEvasiveTactics(char *string, featom *atom);
void mrNeutralTactics(char *string, featom *atom);
void mrAgressiveTactics(char *string, featom *atom);
void mrFormAlliance(char *string, featom *atom);
void mrBreakAlliance(char *string, featom *atom);
void mrTransferRUS(char *string, featom *atom);

//misc functions
void mrCameraMotion(void);
void mrPlayerNameDraw(sdword playerIndex, sdword x, sdword y);
void mrCommandMessageDraw(void);
void mrSelectRectBuild(rectangle *dest, sdword anchorX, sdword anchorY);
sdword mrCursorText(SpaceObj *cursorobj);
void mrEnable(void);
void mrDisable(void);
void mrNULL(void);
void mrSelectHold(void);
void mrShipDied(Ship *ship);
void mrTacticalOverlayState(bool32 bTactical);

void bigmessageDisplay(char *msg,sdword position);
void bigmessageErase(sdword position);

//probe hack functions
bool32 mrNeedProbeHack(void);
void mrProbeHack();
void mrRemoveAllProbesFromSelection();

//nis strangeness prevention:
void mrNISStarting(void);
void mrNISStopping(void);

#endif
