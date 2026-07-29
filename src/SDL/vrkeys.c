/*=============================================================================
    Name    : vrkeys.c
    Purpose : On-panel keyboard and join-by-address field (see vrkeys.h).

    Drawn into the window framebuffer just before that frame is copied into
    the panel swapchain, and hit-tested against vr.pointerX/pointerY, which
    vr.c has already resolved from the controller ray into logical UI pixels.
    That reuses two paths already proven on device - the frame copy and the
    ray-to-panel mapping - instead of adding a fourth wrist card, which would
    have needed its own swapchain, pose and hit test for the same result.

    Created 28/07/2026
=============================================================================*/

#ifdef HW_ENABLE_VR

#include "vrkeys.h"

#include <stdio.h>
#include <string.h>

#include <SDL2/SDL.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#include "glinc.h"

#include "FEFlow.h"
#include "FontReg.h"
#include "font.h"
#include "Key.h"
#include "lan.h"
#include "main.h"
#include "prim2d.h"
#include "Region.h"
#include "Task.h"
#include "UIControls.h"
#include "utility.h"

extern SDL_Window *sdlwindow;

/* Declared in UIControls.c and not in its header; the region tree is the only
   way to find which control is taking keys, and the process function is what
   tells a text entry apart from the list windows that also take key focus. */
extern udword uicTextEntryProcess(regionhandle reg, smemsize ID, udword event, udword data);

/*-----------------------------------------------------------------------------
    Layout
----------------------------------------------------------------------------*/
#define VRK_ROWS            5
#define VRK_UNITS           12      /* every row is this many key units wide */
#define VRK_MAX_KEYS        16      /* per row */

/* Key height as a fraction of a key unit, and the most of the panel's height
   the whole grid may take. Both are about how much of the game it hides. */
#define VRK_KEY_NUM         5
#define VRK_KEY_DEN         8
#define VRK_MAX_HEIGHT_NUM  3
#define VRK_MAX_HEIGHT_DEN  10

/* Between the grid and whatever it is keeping clear of. Small on purpose:
   every pixel of it pushes the grid further up the screen it is covering. */
#define VRK_GAP             6

/* The front end lays every screen out in a 640x480 box centred in the window
   (feResRepositionCentredX), so these are that box's coordinates. */
#define VRK_SCREEN_W        640
#define VRK_SCREEN_H        480

/* Every multiplayer screen keeps its buttons in a column starting at x=507:
   CREATE GAME and JOIN GAME on the lobby, START GAME and LEAVE GAME in a
   waiting room, and the address field, which was put in that column's spare
   room deliberately. The grid stays out of it, so none of them disappears
   behind the keys just because a chat box has focus. */
#define VRK_CONTENT_X0      8
#define VRK_CONTENT_X1      500

/* The gap in the lobby's right-hand button column, between JOIN GAME (ends at
   y=192) and CHANGE COLORS (starts at y=377). Nothing of the game's is drawn
   here, so the address field costs the player none of the screen they had.
   It also stops short of where the keyboard sits along the bottom, so the two
   never fight over the same pixels at the panel's own resolution. */
#define VRK_ADDR_X0         507
#define VRK_ADDR_X1         629
#define VRK_ADDR_Y0         198
#define VRK_ADDR_Y1         288
#define VRK_ADDR_TITLE      3       /* all relative to VRK_ADDR_Y0 */
#define VRK_ADDR_FIELD      17
#define VRK_ADDR_BUTTON     48
#define VRK_ADDR_STATUS     78
#define VRK_ADDR_ROW_H      28

#define VRK_ADDRESS_MAX     40

#define VRK_REPEAT_DELAY    0.42f   /* seconds held before a key repeats */
#define VRK_REPEAT_RATE     0.07f   /* seconds between repeats after that */

/* Hit targets. Keys are 0..n; these are the rest of the overlay. */
#define VRK_HIT_NONE        (-1)
#define VRK_HIT_FIELD       (-2)
#define VRK_HIT_ADD         (-3)

#define VRK_SPECIAL_NONE    0
#define VRK_SPECIAL_CAPS    1

typedef struct {
    char const*  label;
    udword       scancode;      /* SDL_SCANCODE_*, 0 for a non-key */
    ubyte        shifted;       /* this glyph needs shift held */
    ubyte        letter;        /* the caps latch applies to it */
    ubyte        special;       /* VRK_SPECIAL_* */
    ubyte        halves;        /* width in half key units */
} vrkeydef;

/* Caps latch rather than a held shift: pointing at shift and at a letter at
   the same time is not possible with one ray. It shifts letters only - a
   latch that shifted everything would turn the number row into !"# and the
   address field is the reason this keyboard exists - so the few shifted
   glyphs that are worth having carry their own shift in the table. */
static vrkeydef const vrkRow0[] = {
    {"1", SDL_SCANCODE_1, 0, 0, 0, 2}, {"2", SDL_SCANCODE_2, 0, 0, 0, 2},
    {"3", SDL_SCANCODE_3, 0, 0, 0, 2}, {"4", SDL_SCANCODE_4, 0, 0, 0, 2},
    {"5", SDL_SCANCODE_5, 0, 0, 0, 2}, {"6", SDL_SCANCODE_6, 0, 0, 0, 2},
    {"7", SDL_SCANCODE_7, 0, 0, 0, 2}, {"8", SDL_SCANCODE_8, 0, 0, 0, 2},
    {"9", SDL_SCANCODE_9, 0, 0, 0, 2}, {"0", SDL_SCANCODE_0, 0, 0, 0, 2},
    {"BACK", SDL_SCANCODE_BACKSPACE, 0, 0, 0, 4},
};
static vrkeydef const vrkRow1[] = {
    {"Q", SDL_SCANCODE_Q, 0, 1, 0, 2}, {"W", SDL_SCANCODE_W, 0, 1, 0, 2},
    {"E", SDL_SCANCODE_E, 0, 1, 0, 2}, {"R", SDL_SCANCODE_R, 0, 1, 0, 2},
    {"T", SDL_SCANCODE_T, 0, 1, 0, 2}, {"Y", SDL_SCANCODE_Y, 0, 1, 0, 2},
    {"U", SDL_SCANCODE_U, 0, 1, 0, 2}, {"I", SDL_SCANCODE_I, 0, 1, 0, 2},
    {"O", SDL_SCANCODE_O, 0, 1, 0, 2}, {"P", SDL_SCANCODE_P, 0, 1, 0, 2},
    {".", SDL_SCANCODE_PERIOD, 0, 0, 0, 2},
    {"-", SDL_SCANCODE_MINUS, 0, 0, 0, 2},
};
static vrkeydef const vrkRow2[] = {
    {"A", SDL_SCANCODE_A, 0, 1, 0, 2}, {"S", SDL_SCANCODE_S, 0, 1, 0, 2},
    {"D", SDL_SCANCODE_D, 0, 1, 0, 2}, {"F", SDL_SCANCODE_F, 0, 1, 0, 2},
    {"G", SDL_SCANCODE_G, 0, 1, 0, 2}, {"H", SDL_SCANCODE_H, 0, 1, 0, 2},
    {"J", SDL_SCANCODE_J, 0, 1, 0, 2}, {"K", SDL_SCANCODE_K, 0, 1, 0, 2},
    {"L", SDL_SCANCODE_L, 0, 1, 0, 2},
    {",", SDL_SCANCODE_COMMA, 0, 0, 0, 2},
    {"/", SDL_SCANCODE_SLASH, 0, 0, 0, 2},
    {"?", SDL_SCANCODE_SLASH, 1, 0, 0, 2},
};
static vrkeydef const vrkRow3[] = {
    {"CAPS", 0, 0, 0, VRK_SPECIAL_CAPS, 3},
    {"Z", SDL_SCANCODE_Z, 0, 1, 0, 2}, {"X", SDL_SCANCODE_X, 0, 1, 0, 2},
    {"C", SDL_SCANCODE_C, 0, 1, 0, 2}, {"V", SDL_SCANCODE_V, 0, 1, 0, 2},
    {"B", SDL_SCANCODE_B, 0, 1, 0, 2}, {"N", SDL_SCANCODE_N, 0, 1, 0, 2},
    {"M", SDL_SCANCODE_M, 0, 1, 0, 2},
    {"_", SDL_SCANCODE_MINUS, 1, 0, 0, 2},
    {"ENTER", SDL_SCANCODE_RETURN, 0, 0, 0, 5},
};
static vrkeydef const vrkRow4[] = {
    {"ESC", SDL_SCANCODE_ESCAPE, 0, 0, 0, 3},
    {"<", SDL_SCANCODE_LEFT, 0, 0, 0, 2},
    {">", SDL_SCANCODE_RIGHT, 0, 0, 0, 2},
    {"SPACE", SDL_SCANCODE_SPACE, 0, 0, 0, 14},
    {"DEL", SDL_SCANCODE_DELETE, 0, 0, 0, 3},
};

typedef struct {
    vrkeydef const* keys;
    sdword          count;
} vrkeyrow;

static vrkeyrow const vrkRows[VRK_ROWS] = {
    {vrkRow0, (sdword)(sizeof(vrkRow0) / sizeof(vrkRow0[0]))},
    {vrkRow1, (sdword)(sizeof(vrkRow1) / sizeof(vrkRow1[0]))},
    {vrkRow2, (sdword)(sizeof(vrkRow2) / sizeof(vrkRow2[0]))},
    {vrkRow3, (sdword)(sizeof(vrkRow3) / sizeof(vrkRow3[0]))},
    {vrkRow4, (sdword)(sizeof(vrkRow4) / sizeof(vrkRow4[0]))},
};

/*-----------------------------------------------------------------------------
    State
----------------------------------------------------------------------------*/
typedef struct {
    rectangle       rect;
    vrkeydef const* def;
} vrkeyslot;

static struct {
    bool32     boardUp;                 /* key grid drawn and hittable */
    bool32     addressUp;               /* address field on this screen */
    bool32     addressFocus;            /* our field owns the keys, not the game's */
    bool32     capsLatched;

    vrkeyslot  slot[VRK_ROWS * VRK_MAX_KEYS];
    sdword     slotCount;
    rectangle  board;                   /* the whole key grid, for the backdrop */
    rectangle  fieldRect;
    rectangle  addRect;

    sdword     hover;                   /* VRK_HIT_* or a slot index */
    sdword     held;                    /* slot held by the trigger, else -1 */
    real32     nextRepeat;

    char       address[VRK_ADDRESS_MAX + 1];
    sdword     addressLen;
    bool32     addressSeeded;           /* prefilled from the config once */
    char       status[80];

    fonthandle font;
    bool32     fontTried;
    bool32     loggedUp;
    bool32     loggedKey;
} vrk;

/*-----------------------------------------------------------------------------
    Where the game is
----------------------------------------------------------------------------*/
static char const* vrKeysScreenName(void)
{
    if (feStackIndex < 0 || feStack[feStackIndex].screen == NULL)
    {
        return NULL;
    }
    return feStack[feStackIndex].screen->name;
}

/* The lobby: a list of games, and no way at all to reach one that is not on
   this subnet. Both spellings because the LAN screens carry an L prefix and
   the internet ones do not (mgShowScreen). */
static bool32 vrKeysOnLobbyScreen(void)
{
    char const* name = vrKeysScreenName();

    return name != NULL
        && (strcmp(name, "LChannel_Chat") == 0 || strcmp(name, "Channel_Chat") == 0);
}

/* The region currently taking keystrokes, if it is a text entry. List windows
   take key focus too (uicSetCurrent), and a keyboard over a focused list would
   be wrong, so the process function is what decides. */
static regionhandle vrKeysFocusedEntry(regionhandle reg)
{
    regionhandle child, found;

    for (child = reg->child; child != NULL; child = child->next)
    {
        if (bitTest(child->status, RSF_KeyCapture)
            && child->processFunction == uicTextEntryProcess)
        {
            return child;
        }
        found = vrKeysFocusedEntry(child);
        if (found != NULL)
        {
            return found;
        }
    }
    return NULL;
}

/*-----------------------------------------------------------------------------
    Layout
----------------------------------------------------------------------------*/
static void vrKeysLayout(regionhandle focused)
{
    sdword const originX = feResRepositionCentredX(0);
    sdword const originY = feResRepositionCentredY(0);
    sdword width = VRK_CONTENT_X1 - VRK_CONTENT_X0;
    sdword unit, keyHeight, boardHeight, x0, y0, row;
    sdword keepClearFrom = 0, keepClearTo = 0;

    /* Even, so the half-unit keys land on whole pixels and the rows stay
       flush with each other. Capped by height as well as width: the keyboard
       may take at most VRK_MAX_HEIGHT of the panel. A ray is a precise
       pointer, so the keys can be a good deal shorter than they are wide
       without becoming hard to hit, and every pixel of height is one taken
       off the screen behind. */
    {
        sdword byWidth = (width / VRK_UNITS) & ~1;
        sdword byHeight = (MAIN_WindowHeight * VRK_MAX_HEIGHT_NUM
                           / VRK_MAX_HEIGHT_DEN) * VRK_KEY_DEN
                        / (VRK_KEY_NUM * VRK_ROWS);

        unit = byWidth < (byHeight & ~1) ? byWidth : (byHeight & ~1);
    }
    if (unit < 16) unit = 16;
    width = unit * VRK_UNITS;
    keyHeight = unit * VRK_KEY_NUM / VRK_KEY_DEN;
    boardHeight = keyHeight * VRK_ROWS;

    x0 = originX + VRK_CONTENT_X0
       + (VRK_CONTENT_X1 - VRK_CONTENT_X0 - width) / 2;
    y0 = MAIN_WindowHeight - boardHeight - VRK_GAP;

    /* Never over whatever is being typed into. On the lobby the game's chat
       entry sits along the bottom, which is exactly where the keyboard wants
       to be. When it has to move, it goes *just above* that field rather than
       to the top of the panel: the lobby's list of games is up there and is
       the one thing a player is in the lobby to read. Only the two rows the
       keyboard actually needs get covered, and it reads as belonging to the
       field it is attached to. */
    if (vrk.addressFocus)
    {
        keepClearFrom = originY + VRK_ADDR_Y0;
        keepClearTo = originY + VRK_ADDR_Y1;
    }
    else if (focused != NULL)
    {
        keepClearFrom = focused->rect.y0;
        keepClearTo = focused->rect.y1;
    }
    if (keepClearTo > y0 - VRK_GAP)
    {
        y0 = keepClearFrom - boardHeight - VRK_GAP;
    }
    if (y0 < 4)
    {
        y0 = 4;
    }

    vrk.board.x0 = x0;
    vrk.board.y0 = y0;
    vrk.board.x1 = x0 + width;
    vrk.board.y1 = y0 + boardHeight;

    vrk.slotCount = 0;
    for (row = 0; row < VRK_ROWS; row++)
    {
        sdword x = x0;
        sdword i;

        for (i = 0; i < vrkRows[row].count && i < VRK_MAX_KEYS; i++)
        {
            vrkeydef const* def = &vrkRows[row].keys[i];
            vrkeyslot* slot = &vrk.slot[vrk.slotCount++];
            sdword keyWidth = unit * def->halves / 2;

            slot->def = def;
            slot->rect.x0 = x + 1;
            slot->rect.y0 = y0 + row * keyHeight + 1;
            slot->rect.x1 = x + keyWidth - 1;
            slot->rect.y1 = y0 + (row + 1) * keyHeight - 1;
            x += keyWidth;
        }
    }

    vrk.fieldRect.x0 = originX + VRK_ADDR_X0 + 4;
    vrk.fieldRect.x1 = originX + VRK_ADDR_X1 - 4;
    vrk.fieldRect.y0 = originY + VRK_ADDR_Y0 + VRK_ADDR_FIELD;
    vrk.fieldRect.y1 = vrk.fieldRect.y0 + VRK_ADDR_ROW_H;
    vrk.addRect.x0 = vrk.fieldRect.x0;
    vrk.addRect.x1 = vrk.fieldRect.x1;
    vrk.addRect.y0 = originY + VRK_ADDR_Y0 + VRK_ADDR_BUTTON;
    vrk.addRect.y1 = vrk.addRect.y0 + VRK_ADDR_ROW_H;

    /* Typing into one of the game's own fields near the bottom puts the grid
       over the right-hand column, address panel included. Stand it down for
       the duration rather than drawing half of it under the keys - it is not
       what the player is doing, and it comes straight back. It can never be
       the field being typed into: that case places the grid above it. */
    if (vrk.boardUp && vrk.addressUp
        && originX + VRK_ADDR_X0 < vrk.board.x1
        && vrk.board.x0 < originX + VRK_ADDR_X1
        && originY + VRK_ADDR_Y0 < vrk.board.y1 + 4
        && vrk.board.y0 - 4 < originY + VRK_ADDR_Y1)
    {
        vrk.addressUp = FALSE;
    }
}

static bool32 vrKeysInside(rectangle const* rect, sdword x, sdword y)
{
    return x >= rect->x0 && x < rect->x1 && y >= rect->y0 && y < rect->y1;
}

static sdword vrKeysHitTest(sdword x, sdword y)
{
    sdword i;

    if (vrk.addressUp)
    {
        if (vrKeysInside(&vrk.fieldRect, x, y)) return VRK_HIT_FIELD;
        if (vrKeysInside(&vrk.addRect, x, y))   return VRK_HIT_ADD;
    }
    if (vrk.boardUp)
    {
        for (i = 0; i < vrk.slotCount; i++)
        {
            if (vrKeysInside(&vrk.slot[i].rect, x, y))
            {
                return i;
            }
        }
    }
    return VRK_HIT_NONE;
}

/*-----------------------------------------------------------------------------
    Our own field
----------------------------------------------------------------------------*/
static void vrKeysAddressCommit(void)
{
    udword ip;

    /* Short, because the column this is reported in is 112 pixels wide. */
    if (vrk.addressLen == 0)
    {
        strcpy(vrk.status, "type it first");
        return;
    }
    ip = inet_addr(vrk.address);
    if (ip == INADDR_NONE)
    {
        strcpy(vrk.status, "not an address");
        return;
    }
#ifdef HW_ENABLE_NETWORK
    lanAddRemote(ip);
    /* Kept for the next session too. The config is the desktop's and any test
       harness's way in, and a player who typed an address once should not have
       to type it again. */
    strncpy(utyMultiplayerHost, vrk.address, sizeof(utyMultiplayerHost) - 1);
    utyMultiplayerHost[sizeof(utyMultiplayerHost) - 1] = '\0';
    strcpy(vrk.status, "added, waiting");
    SDL_Log("VR: joining by address %s", vrk.address);
#else
    strcpy(vrk.status, "no networking");
#endif
}

/* The character a key stands for, applied to our own field. The game's own
   entries never come through here: they get scancodes and derive the
   character themselves. */
static char vrKeysCharOf(vrkeydef const* def)
{
    char c = def->label[0];

    if (def->label[1] != '\0')
    {
        return '\0';                                        /* BACK, ENTER, ... */
    }
    if (def->letter && !vrk.capsLatched)
    {
        c = (char)(c - 'A' + 'a');
    }
    return c;
}

static void vrKeysAddressKey(vrkeydef const* def)
{
    char c;

    switch (def->scancode)
    {
        case SDL_SCANCODE_BACKSPACE:
        case SDL_SCANCODE_DELETE:
            if (vrk.addressLen > 0)
            {
                vrk.address[--vrk.addressLen] = '\0';
            }
            return;
        case SDL_SCANCODE_RETURN:
            vrKeysAddressCommit();
            return;
        case SDL_SCANCODE_ESCAPE:
            vrk.addressFocus = FALSE;
            return;
        case SDL_SCANCODE_LEFT:
        case SDL_SCANCODE_RIGHT:
            return;
        default:
            break;
    }
    c = vrKeysCharOf(def);
    if (c != '\0' && vrk.addressLen < VRK_ADDRESS_MAX)
    {
        vrk.address[vrk.addressLen++] = c;
        vrk.address[vrk.addressLen] = '\0';
    }
}

/*-----------------------------------------------------------------------------
    Into the engine

    Only ever scancodes. Region.c reads the key and the shift state together
    (keyBufferedKeyGet) at the moment the key is buffered, so a shifted glyph
    needs LSHIFT genuinely down across the press rather than a transform
    applied afterwards.
----------------------------------------------------------------------------*/
static void vrKeysPushRaw(udword scancode, bool32 down)
{
    SDL_Event event;

    memset(&event, 0, sizeof(event));
    event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    event.key.windowID = SDL_GetWindowID(sdlwindow);
    event.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    event.key.keysym.scancode = (SDL_Scancode)scancode;
    event.key.keysym.sym = SDL_GetKeyFromScancode((SDL_Scancode)scancode);
    SDL_PushEvent(&event);
}

static void vrKeysPushKey(vrkeydef const* def)
{
    bool32 shift = def->shifted || (def->letter && vrk.capsLatched);

    /* uicTextEntryProcess turns the scancode back into a character through
       SDL_GetKeyFromScancode and an ASCII-indexed table, so a headset with no
       physical keyboard attached is relying on SDL's default keymap being
       populated. Say what it resolved to once: if that is ever 0, every key
       silently types nothing and there is otherwise no way to tell. */
    if (!vrk.loggedKey)
    {
        vrk.loggedKey = TRUE;
        SDL_Log("VR: keyboard '%s' scancode %u -> keycode %d, shift %d",
                def->label, (unsigned)def->scancode,
                (int)SDL_GetKeyFromScancode((SDL_Scancode)def->scancode),
                (int)shift);
    }
    if (shift)
    {
        vrKeysPushRaw(SDL_SCANCODE_LSHIFT, TRUE);
    }
    vrKeysPushRaw(def->scancode, TRUE);
    vrKeysPushRaw(def->scancode, FALSE);
    if (shift)
    {
        vrKeysPushRaw(SDL_SCANCODE_LSHIFT, FALSE);
    }
}

/* Which keys repeat while held. Typing and correcting do; the ones that
   commit or toggle do not, or holding CAPS would flicker the latch and
   holding ENTER would submit the same address over and over. */
static bool32 vrKeysRepeats(vrkeydef const* def)
{
    return def->special == VRK_SPECIAL_NONE
        && def->scancode != SDL_SCANCODE_RETURN
        && def->scancode != SDL_SCANCODE_ESCAPE;
}

static void vrKeysFire(sdword index)
{
    vrkeydef const* def;

    if (index < 0 || index >= vrk.slotCount)
    {
        return;
    }
    def = vrk.slot[index].def;

    if (def->special == VRK_SPECIAL_CAPS)
    {
        vrk.capsLatched = !vrk.capsLatched;
        return;
    }
    if (vrk.addressFocus)
    {
        vrKeysAddressKey(def);
        return;
    }
    vrKeysPushKey(def);
}

/*-----------------------------------------------------------------------------
    Frame
----------------------------------------------------------------------------*/
void vrKeysUpdate(bool32 pointerValid, sdword x, sdword y)
{
    regionhandle focused = vrKeysFocusedEntry(&regRootRegion);
    bool32 wasUp = vrk.boardUp;

    vrk.addressUp = vrKeysOnLobbyScreen();
    if (!vrk.addressUp)
    {
        vrk.addressFocus = FALSE;
        vrk.status[0] = '\0';
    }
    else if (!vrk.addressSeeded)
    {
        /* Whatever was used last, from the config the desktop and any test
           harness set through MultiplayerHost. Typing an address once should
           be enough. */
        vrk.addressSeeded = TRUE;
        strncpy(vrk.address, utyMultiplayerHost, VRK_ADDRESS_MAX);
        vrk.address[VRK_ADDRESS_MAX] = '\0';
        vrk.addressLen = (sdword)strlen(vrk.address);
    }
    /* Our own field wins while it has focus, and the game's entry does not
       take it back by itself. The lobby's chat box is created with
       UICTE_NoLossOfFocus (lgChatTextEntry) and so holds key capture the
       whole time that screen is up: deferring to whatever the region tree
       says has focus meant the address field lost it again on the very next
       frame, every frame, and could never be typed into at all. A press
       somewhere else on the panel is what gives it up - see vrKeysPress. */
    vrk.boardUp = (focused != NULL) || vrk.addressFocus;

    if (!vrk.boardUp && !vrk.addressUp)
    {
        vrk.hover = VRK_HIT_NONE;
        vrk.held = -1;
        return;
    }
    if (vrk.boardUp && !wasUp)
    {
        vrk.capsLatched = FALSE;
        if (!vrk.loggedUp)
        {
            vrk.loggedUp = TRUE;
            SDL_Log("VR: keyboard up (screen '%s')",
                    vrKeysScreenName() != NULL ? vrKeysScreenName() : "?");
        }
    }

    vrKeysLayout(focused);
    vrk.hover = pointerValid ? vrKeysHitTest(x, y) : VRK_HIT_NONE;

    /* Held keys repeat, which is the difference between deleting a mistyped
       address and deleting it one deliberate tap at a time. */
    if (vrk.held >= 0)
    {
        if (vrk.hover != vrk.held)
        {
            vrk.held = -1;                                  /* slid off the key */
        }
        else if (taskTimeElapsed >= vrk.nextRepeat
                 && vrKeysRepeats(vrk.slot[vrk.held].def))
        {
            vrKeysFire(vrk.held);
            vrk.nextRepeat = taskTimeElapsed + VRK_REPEAT_RATE;
        }
    }
}

bool32 vrKeysActive(void)
{
    return vrk.boardUp || vrk.addressUp;
}

bool32 vrKeysCovers(sdword x, sdword y)
{
    return vrKeysActive() && vrKeysHitTest(x, y) != VRK_HIT_NONE;
}

bool32 vrKeysPress(sdword x, sdword y)
{
    sdword hit;

    if (!vrKeysActive())
    {
        return FALSE;
    }
    hit = vrKeysHitTest(x, y);
    vrk.hover = hit;

    switch (hit)
    {
        case VRK_HIT_NONE:
            /* Clicked past the overlay: whatever that was - one of the game's
               own fields, a button, empty space - the address field is no
               longer what is being typed into. This is the only thing that
               gives it up, short of leaving the screen, because the game's
               entry never asks for it back. */
            vrk.addressFocus = FALSE;
            return FALSE;
        case VRK_HIT_FIELD:
            vrk.addressFocus = TRUE;
            vrk.status[0] = '\0';
            return TRUE;
        case VRK_HIT_ADD:
            vrKeysAddressCommit();
            return TRUE;
        default:
            break;
    }
    vrk.held = hit;
    vrk.nextRepeat = taskTimeElapsed + VRK_REPEAT_DELAY;
    vrKeysFire(hit);
    return TRUE;
}

void vrKeysRelease(void)
{
    vrk.held = -1;
}

/*-----------------------------------------------------------------------------
    Drawing
----------------------------------------------------------------------------*/
static fonthandle vrKeysFont(void)
{
    if (!vrk.fontTried)
    {
        vrk.fontTried = TRUE;
        vrk.font = frFontRegister("default.hff");
    }
    return vrk.font;
}

static void vrKeysCentredText(rectangle const* rect, char const* text, color c)
{
    sdword lineHeight = fontHeight("Ay");
    sdword textWidth = fontWidth((char*)text);

    if (lineHeight <= 0)
    {
        lineHeight = 12;
    }
    fontPrint(rect->x0 + (rect->x1 - rect->x0 - textWidth) / 2,
              rect->y0 + (rect->y1 - rect->y0 - lineHeight) / 2, c, (char*)text);
}

static void vrKeysDrawBoard(void)
{
    /* The backdrop is nearly clear and the key faces are not: what the grid
       hides is then only the keys themselves, and the screen behind reads
       through everything between them. */
    color const backdrop = colRGBA(6, 12, 20, 120);
    color const edge = colRGB(60, 140, 180);
    color const face = colRGBA(22, 40, 56, 225);
    color const faceHover = colRGBA(40, 90, 120, 240);
    color const faceHeld = colRGBA(70, 160, 200, 250);
    color const faceLatched = colRGBA(30, 90, 70, 240);
    color const capLabel = colRGB(215, 232, 240);
    color const capActive = colRGB(255, 240, 190);
    rectangle frame = vrk.board;
    sdword i;

    frame.x0 -= 4;
    frame.y0 -= 4;
    frame.x1 += 4;
    frame.y1 += 4;
    primRectTranslucent2(&frame, backdrop);
    primRectOutline2(&frame, 2, edge);

    for (i = 0; i < vrk.slotCount; i++)
    {
        vrkeyslot* slot = &vrk.slot[i];
        bool32 latched = slot->def->special == VRK_SPECIAL_CAPS && vrk.capsLatched;
        color fill = face;

        if (vrk.held == i)
        {
            fill = faceHeld;
        }
        else if (latched)
        {
            fill = faceLatched;
        }
        else if (vrk.hover == i)
        {
            fill = faceHover;
        }
        primRectTranslucent2(&slot->rect, fill);
        vrKeysCentredText(&slot->rect, slot->def->label,
                          latched ? capActive : capLabel);
    }
}

static void vrKeysDrawAddress(void)
{
    color const backdrop = colRGBA(6, 12, 20, 225);
    color const edge = colRGB(60, 140, 180);
    color const edgeFocus = colRGB(120, 230, 255);
    color const heading = colRGB(120, 230, 255);
    color const text = colRGB(230, 240, 246);
    color const dim = colRGB(130, 148, 160);
    color const button = colRGBA(26, 52, 70, 235);
    color const buttonHover = colRGBA(46, 100, 132, 245);
    sdword const originX = feResRepositionCentredX(0);
    sdword const originY = feResRepositionCentredY(0);
    sdword lineHeight = fontHeight("Ay");
    sdword textY;
    rectangle frame;

    if (lineHeight <= 0)
    {
        lineHeight = 12;
    }
    frame.x0 = originX + VRK_ADDR_X0;
    frame.y0 = originY + VRK_ADDR_Y0;
    frame.x1 = originX + VRK_ADDR_X1;
    frame.y1 = originY + VRK_ADDR_Y1;
    primRectTranslucent2(&frame, backdrop);
    primRectOutline2(&frame, 1, edge);

    /* One line, because the column is 122 pixels wide. This is the whole of
       internet play as far as a player is concerned: a game that is not on
       this subnet cannot advertise itself here, so its host has to be named
       before it can appear in the list alongside the LAN ones. */
    fontPrint(frame.x0 + 5, frame.y0 + VRK_ADDR_TITLE, heading, "JOIN BY ADDRESS");

    primRectTranslucent2(&vrk.fieldRect, colRGBA(12, 24, 34, 240));
    primRectOutline2(&vrk.fieldRect, vrk.addressFocus ? 2 : 1,
                     vrk.addressFocus ? edgeFocus : edge);
    textY = vrk.fieldRect.y0 + (vrk.fieldRect.y1 - vrk.fieldRect.y0 - lineHeight) / 2;
    if (vrk.addressLen > 0)
    {
        sdword room = vrk.fieldRect.x1 - vrk.fieldRect.x0 - 10;
        sdword from = 0;

        /* Show the tail: what matters while typing is the character that just
           arrived, not the start of an address already scrolled past. */
        while (from < vrk.addressLen
               && fontWidthN(vrk.address + from, vrk.addressLen - from) > room)
        {
            from++;
        }
        fontPrintN(vrk.fieldRect.x0 + 5, textY, text, vrk.address + from,
                   vrk.addressLen - from);
    }
    else
    {
        fontPrint(vrk.fieldRect.x0 + 5, textY, dim, "host address");
    }

    primRectTranslucent2(&vrk.addRect,
                         vrk.hover == VRK_HIT_ADD ? buttonHover : button);
    primRectOutline2(&vrk.addRect, 1, edge);
    vrKeysCentredText(&vrk.addRect, "ADD HOST", text);

    if (vrk.status[0] != '\0')
    {
        fontPrint(frame.x0 + 5, frame.y0 + VRK_ADDR_STATUS, dim, vrk.status);
    }
}

void vrKeysDraw(void)
{
    bool32 const wasPrimMode = primModeEnabled;
    fonthandle previousFont;

    if (!vrKeysActive() || MAIN_WindowWidth <= 0 || MAIN_WindowHeight <= 0)
    {
        return;
    }
    primModeSet2();
    previousFont = fontMakeCurrent(vrKeysFont());
    if (vrk.addressUp)
    {
        vrKeysDrawAddress();
    }
    if (vrk.boardUp)
    {
        vrKeysDrawBoard();
    }
    fontMakeCurrent(previousFont);
    if (!wasPrimMode)
    {
        primModeClear2();
    }
    glFlush();
}

#endif /* HW_ENABLE_VR */
