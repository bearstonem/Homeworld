/*=============================================================================
    Name    : vrkeys.h
    Purpose : On-panel keyboard and join-by-address field for VR.

    A player in a headset has no keyboard, and every multiplayer screen asks
    for typing: a player name, a game name, chat, and - for play across the
    internet - the address of the host. The first three exist in the game's
    own screens; the fourth does not exist anywhere, because a LAN never
    needed one and the screen definitions live inside Homeworld.big, which
    belongs to the player and cannot be edited. Both are drawn here, over the
    game's frame, and hit-tested against the controller ray that vr.c has
    already mapped onto the panel.

    Keys are synthesized as scancodes, never as characters: the engine derives
    the character itself through uicKeyEntryTable (UIControls.c) and a
    keyboard that injected letters directly would be fighting it.

    Created 28/07/2026
=============================================================================*/

#ifndef ___VRKEYS_H
#define ___VRKEYS_H

#include "Types.h"

#ifdef HW_ENABLE_VR

/* Once per frame, before vrKeysDraw. x/y are the panel pointer in logical UI
   pixels; valid FALSE means no ray is on the panel. Decides whether the
   keyboard is up, tracks what is under the pointer, and fires key repeats. */
void vrKeysUpdate(bool32 pointerValid, sdword x, sdword y);

/* TRUE while the overlay is drawing something the pointer can hit. vr.c uses
   this to keep the ray from also reaching the game underneath. */
bool32 vrKeysActive(void);

/* TRUE when these logical UI pixels are on the overlay. For the buttons that
   are not the trigger: what is under a key must never be clicked. */
bool32 vrKeysCovers(sdword x, sdword y);

/* A trigger press at these logical UI pixels. TRUE when the overlay took it,
   in which case no mouse click must be pushed for the same press. */
bool32 vrKeysPress(sdword x, sdword y);

/* The matching release. Safe to call when the press was not consumed. */
void vrKeysRelease(void);

/* Draw into the window framebuffer. Must run after the game has finished its
   own frame and before that frame is copied into the panel swapchain. */
void vrKeysDraw(void);

#endif /* HW_ENABLE_VR */

#endif
