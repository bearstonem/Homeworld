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

/* Stereo world rendering hooks, called from rglu.c during the per-eye
   passes. Outside an eye pass both are no-ops. */

/* Replaces the symmetric game frustum with the eye's asymmetric one.
   Returns TRUE when it did (caller returns without building its own). */
bool32 vrEyeProjection(real32 zNear, real32 zFar);

/* Pre-multiplies the head-tracking eye view onto the modelview stack,
   so the game's own lookat applies on top of it. */
void vrEyeApplyView(void);

/* TRUE while a per-eye world render pass is running. Frame statistics
   (AutoLOD polygon counts etc.) must only accumulate in the mono pass. */
bool32 vrEyePassActive(void);

#endif /* HW_ENABLE_VR */

#endif
