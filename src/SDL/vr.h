/*=============================================================================
    Name    : vr.h
    Purpose : OpenXR presentation layer (Meta Quest / Android).

    "Theater mode": the game renders its normal mono frame into the window
    framebuffer; each frame is copied into an OpenXR swapchain and shown on
    a large virtual screen (composition quad layer) in the headset.

    Created 24/07/2026
=============================================================================*/

#ifndef ___VR_H
#define ___VR_H

#include "Types.h"

#ifdef HW_ENABLE_VR

/* Initialize OpenXR against the current EGL context. width/height is the
   size of the frames the game renders (and of the swapchain). Returns TRUE
   on success; on failure the game simply keeps running flat. */
bool32 vrInit(sdword width, sdword height);

/* TRUE when an OpenXR session is up (init succeeded and not shut down). */
bool32 vrActive(void);

/* Present the frame currently in the window framebuffer to the headset.
   Called from rndFlush() before SDL_GL_SwapWindow. Also pumps OpenXR
   events; no-op while inactive. */
void vrFrame(void);

void vrShutdown(void);

#endif /* HW_ENABLE_VR */

#endif
