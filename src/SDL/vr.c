/*=============================================================================
    Name    : vr.c
    Purpose : OpenXR presentation layer (Meta Quest / Android).

    See vr.h for the theater-mode overview. The OpenXR session shares the
    EGL context SDL created; the runtime's swapchain textures live in that
    context, so the finished frame can be copied straight out of the window
    framebuffer with glCopyTexSubImage2D (through gl4es).

    Created 24/07/2026
=============================================================================*/

#ifdef HW_ENABLE_VR

#include "vr.h"
#include "vrworld.h"

#include <math.h>
#include <string.h>
#include <dlfcn.h>

#include <SDL2/SDL.h>
#include <jni.h>
#include <EGL/egl.h>

#include "glinc.h"

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

/* GL enums vr.c cares about; not in the GL 1.x headers the game uses. */
#ifndef GL_RGBA8
#define GL_RGBA8        0x8058
#endif
#ifndef GL_SRGB8_ALPHA8
#define GL_SRGB8_ALPHA8 0x8C43
#endif
#define VR_GL_READ_FRAMEBUFFER   0x8CA8
#define VR_GL_DRAW_FRAMEBUFFER   0x8CA9
#define VR_GL_COLOR_ATTACHMENT0  0x8CE0
#define VR_GL_COLOR_BUFFER_BIT   0x00004000
#define VR_GL_NEAREST            0x2600
#define VR_GL_LINEAR             0x2601
#define VR_GL_EXTENSIONS         0x1F03
#define VR_GL_FRAMEBUFFER_SRGB   0x8DB9     /* EXT_sRGB_write_control */

#define VR_MAX_SWAPCHAIN_IMAGES 8

/* Virtual screen: ~2m away, 4:3 like the game's default resolution */
#define VR_SCREEN_DISTANCE  2.0f
#define VR_SCREEN_WIDTH     2.4f

/* Full-screen managers (Build/Launch/Research/Trade) are dense 2D screens
   and the panel is the only way back out of one, so it is placed relative
   to the head at a fixed readable size, biased toward the ship it concerns
   but always well inside the field of view. */
#define VR_MANAGER_DISTANCE      1.80f  /* metres in front of the head      */
#define VR_MANAGER_WIDTH         2.20f  /* ~63 degrees wide: desktop-legible*/
#define VR_MANAGER_CONE_COS      0.940f /* open within 20 deg of the gaze:  */
#define VR_MANAGER_CONE_SIN      0.342f /* far edge still inside the FOV    */
#define VR_MANAGER_LOST_COS      0.64f  /* >~50 deg off gaze: glide back    */
#define VR_MANAGER_HOME_COS      0.985f /* settled in front again           */
#define VR_MANAGER_PITCH_SIN     0.259f /* hold within 15 deg of eye level  */
#define VR_MANAGER_FOLLOW_ALPHA  0.12f  /* ~0.3s glide at 72Hz             */
#define VR_MANAGER_PENDING_FRAMES 200   /* escape hatch if never presentable*/

/* Wrist cards: extra quad layers with content this file draws itself, rather
   than a copy of the game's framebuffer. The controls reference rides
   outboard of the left wrist panel; the status readout rides the right wrist.
   Content is laid out in the game's logical UI pixels (prim2d/font space) and
   blitted out of a scratch corner of the window framebuffer. */
#define VR_CARD_CONTROLS   0
#define VR_CARD_STATUS     1
#define VR_CARD_WHEEL      2
#define VR_CARD_COUNT      3

/* Command wheel. Fixed slots, always the same count, never reordered and
   never hidden when unavailable - direction is what makes a radial memorable,
   and shifting wedges to hide inapplicable entries destroys that exactly when
   the player is under pressure. Unavailable entries draw dim and keep their
   place. */
#define VR_WHEEL_SLOTS       8
#define VR_WHEEL_WIN_W       300    /* layout size, logical UI pixels */
#define VR_WHEEL_WIN_H       300
#define VR_WHEEL_WIDTH       0.26f  /* physical width, metres */
#define VR_WHEEL_DEADZONE    0.45f  /* stick past this picks a wedge */
#define VR_WHEEL_HOLD_NS     260000000LL  /* dwell before a submenu opens */
#define VR_WHEEL_REPEAT_FIRST_NS 200000000LL /* pause before a held wedge repeats */
#define VR_WHEEL_REPEAT_NS   110000000LL  /* interval once it is repeating */

#define VR_CARD_CONTROLS_WIN_W   330    /* layout size, logical UI pixels */
#define VR_CARD_CONTROLS_WIN_H   540
#define VR_CARD_CONTROLS_WIDTH   0.24f  /* physical width, metres */
#define VR_CARD_STATUS_WIN_W     300
#define VR_CARD_STATUS_WIN_H     200
#define VR_CARD_STATUS_WIDTH     0.22f
#define VR_CARD_GAP              0.02f  /* metres between panel and card */
#define VR_CARD_CONTROLS_SIDE   -1.0f   /* which side of the wrist panel */

#define VR_HAND_LEFT   0
#define VR_HAND_RIGHT  1
#define VR_HAND_COUNT  2

#define VR_EYE_COUNT   2

#define VR_DEBUG_INTERVAL 120

#define VR_GESTURE_NONE          0
#define VR_SELECT_PANEL          1
#define VR_SELECT_CLICK          2
#define VR_SELECT_SWEEP          3
#define VR_SELECT_PATH           4
#define VR_SELECT_TARGETS        5
#define VR_CONTEXT_PANEL         1
#define VR_CONTEXT_ORDER         2
#define VR_CONTEXT_MOVE          3

#define VR_DOUBLE_TRIGGER_NS 350000000LL
#define VR_MOVE_DEPTH_DEADZONE 0.20f
#define VR_MOVE_DEPTH_RATE     1.20f   /* ~3.3x cursor depth per second at full stick */
#define VR_MOVE_DEPTH_ACCEL    2.40f   /* rate multiplier gained per second held */
#define VR_MOVE_DEPTH_ACCEL_MAX 6.0f   /* ceiling on that multiplier */

/* How many game-world units one real-world metre of head movement is
   worth. Homeworld ships are hundreds of units long; at 1000 the fleet
   reads as a room-sized hologram. Adjustable at runtime - see
   vrWorldScaleStep - which is what turns the battle between a tabletop model
   and something you stand inside. */
#define VR_WORLD_SCALE 1000.0f
#define VR_WORLD_SCALE_MIN 250.0f
#define VR_WORLD_SCALE_MAX 8000.0f
#define VR_WORLD_SCALE_STEP 1.25f

extern SDL_Window *sdlwindow;

#include "Camera.h"
#include "font.h"
#include "FontReg.h"
#include "main.h"
#include "prim2d.h"
#include "render.h"
#include "Select.h"
#include "ShipSelect.h"
#include "Universe.h"
extern void rndMainViewRenderFunction(Camera *camera);
extern Camera *mrCamera;
extern bool32 gameIsRunning;

/* The frame copy must talk to the driver directly: the game's GL calls go
   through gl4es, which keeps its own texture-id namespace, and the
   swapchain textures belong to the runtime (driver ids). */
typedef void (*rawGlGetIntegerv_t)(unsigned int pname, int* params);
typedef unsigned int (*rawGlGetError_t)(void);
typedef void (*rawGlGenFramebuffers_t)(int n, unsigned int* ids);
typedef void (*rawGlBindFramebuffer_t)(unsigned int target, unsigned int framebuffer);
typedef void (*rawGlFramebufferTexture2D_t)(unsigned int target, unsigned int attachment,
                                            unsigned int textarget, unsigned int texture, int level);
typedef void (*rawGlBlitFramebuffer_t)(int srcX0, int srcY0, int srcX1, int srcY1,
                                       int dstX0, int dstY0, int dstX1, int dstY1,
                                       unsigned int mask, unsigned int filter);
typedef void (*rawGlDeleteFramebuffers_t)(int n, unsigned int const* ids);
typedef void (*rawGlDisable_t)(unsigned int cap);
typedef unsigned char const* (*rawGlGetString_t)(unsigned int name);

/* Manager presentation is tracked separately from the game's own manager
   state. Modal input suppression is gated on VISIBLE - reached only once the
   compositor has accepted a frame containing a valid manager panel - so a
   presentation failure can never turn into a trap the user has to quit out
   of. */
typedef enum
{
    VR_MGR_HIDDEN = 0,
    VR_MGR_PENDING,                 /* open, no presentable panel yet */
    VR_MGR_VISIBLE                  /* a valid panel pose was submitted */
} vrmanagerstate;

typedef struct {
    XrSwapchain  swapchain;
    uint32_t     imageCount;
    XrSwapchainImageOpenGLESKHR images[VR_MAX_SWAPCHAIN_IMAGES];
    sdword       winWidth, winHeight;   /* layout size, logical UI pixels */
    sdword       texWidth, texHeight;   /* swapchain size */
    real32       widthMetres;
    XrPosef      pose;
    bool32       poseValid;
    bool32       failed;                /* swapchain creation gave up */
} vrcard;

typedef struct {
    XrInstance   instance;
    XrSystemId   systemId;
    XrSession    session;
    XrSpace      space;
    XrSpace      viewSpace;
    bool32       quadPlaced;
    XrPosef      quadPose;
    XrPosef      anchorPose;        /* head pose the world is anchored to */
    bool32       quadFollowing;     /* screen gliding back into view */
    XrSwapchain  swapchain;
    XrSwapchain  eyeSwapchain[VR_EYE_COUNT];
    uint32_t     eyeImageCount[VR_EYE_COUNT];
    XrSwapchainImageOpenGLESKHR eyeImages[VR_EYE_COUNT][VR_MAX_SWAPCHAIN_IMAGES];
    sdword       eyeWidth, eyeHeight;
    bool32       eyeActive;         /* inside a per-eye world render pass */
    XrFovf       eyeFov;
    real32       eyeViewMatrix[16];
    XrCompositionLayerProjectionView projViews[VR_EYE_COUNT];
    XrCompositionLayerProjection projLayer;
    uint32_t     imageCount;
    XrSwapchainImageOpenGLESKHR images[VR_MAX_SWAPCHAIN_IMAGES];
    sdword       width, height;
    XrSessionState state;
    bool32       sessionRunning;
    bool32       active;
    udword       frameCount;
    udword       errorsLogged;
    bool32       inFrame;          /* guards re-entry through rndFlush */
    udword       reentryLogged;
    sdword       debugEye;         /* -1 mono, 0 left eye, 1 right eye */
    udword       debugPassFrame[3];
    udword       debugObjectFrame[3][2]; /* first ship/asteroid in each pass */
    XrPosef      debugAimPose[VR_HAND_COUNT];
    unsigned int blitFbo;
    XrActionSet  actionSet;
    XrAction     aimAction;
    XrAction     selectAction;      /* trigger      -> left mouse button        */
    XrAction     contextAction;     /* A/X          -> right mouse (chord: D)   */
    XrAction     backAction;        /* B/Y          -> Escape      (chord: B)   */
    XrAction     stickAction;       /* left stick   -> camera orbit (RMB drag)
                                       right stick  -> zoom (mouse wheel)       */
    XrAction     stickClickAction;  /* left click   -> M, right click -> Space
                                       (chord left  -> R)                       */
    XrAction     gripAction;        /* left grip    -> Shift, right grip = chord */
    XrAction     gripPoseAction;
    XrAction     hapticAction;
    XrPath       handPath[VR_HAND_COUNT];
    XrSpace      aimSpace[VR_HAND_COUNT];
    XrSpace      gripSpace[VR_HAND_COUNT];
    real32       quadWidth;         /* current screen width in metres */
    bool32       prevSelect[VR_HAND_COUNT];
    bool32       prevContext[VR_HAND_COUNT];
    bool32       prevBack[VR_HAND_COUNT];
    bool32       prevGripLeft;
    bool32       prevStickClick[VR_HAND_COUNT];
    sdword       selectGestureHand;
    sdword       selectGestureMode;
    bool32       selectAdditive;
    XrTime       lastSelectTime[VR_HAND_COUNT];
    sdword       contextGestureHand;
    sdword       contextGestureMode;
    sdword       backKey[VR_HAND_COUNT];
    sdword       stickClickKey[VR_HAND_COUNT];
    bool32       stickRotating;     /* left stick currently orbiting the camera */
    real32       wheelAccum;
    sdword       pointerX, pointerY;
    bool32       pointerValid;
    sdword       pointerHand;
    bool32       worldInteractive;  /* game world exists; native 3D interaction on */
    real32       panelHitT;         /* metres to the panel along the pointer ray */
    real32       moveDepthInput;    /* right-stick deflection driving order depth */
    real32       moveDepthHeld;     /* seconds the depth stick has been held */
    XrTime       lastInputTime;
    bool32       panelHidden;       /* wrist panel toggled off in-game */
    vrcard       card[VR_CARD_COUNT];
    fonthandle   cardFont;
    bool32       cardFontTried;
    bool32       cardOverflowLogged;
    bool32       wheelOpen;
    sdword       wheelPage;         /* index into vrWheelPage[] */
    sdword       wheelSlot;         /* highlighted wedge, -1 = none */
    XrTime       wheelSlotSince;    /* when the highlight last changed */
    bool32       wheelSubOpened;    /* dwell already opened a submenu */
    XrTime       wheelRepeatAt;     /* next fire time for a repeating wedge */
    real32       worldScale;        /* game units per metre, runtime */
    vrmanagerstate managerState;
    udword       managerFrames;     /* frames since the manager opened */
    udword       managerPendingFrames; /* consecutive frames not presentable */
    bool32       managerPlaced;     /* panel pose established in space */
    bool32       managerPoseValid;  /* this frame's pose is submittable */
    bool32       managerFollowing;  /* panel gliding back into view */
    bool32       pinchValid;
    real32       pinchPrevDist;
    real32       pinchPrevAzimuth;
    real32       pinchPrevElev;
    sdword       prevCycleDir;      /* right-stick flick edge state */
    XrVector3f   worldOffset;       /* where the hologram sits in the room,
                                       metres. Set by recentring, which is a
                                       presentation concern - it moves the
                                       hologram around the player, not the
                                       game camera through the world. */
    rawGlGetIntegerv_t          rawGetIntegerv;
    rawGlGetError_t             rawGetError;
    rawGlGenFramebuffers_t      rawGenFramebuffers;
    rawGlBindFramebuffer_t      rawBindFramebuffer;
    rawGlFramebufferTexture2D_t rawFramebufferTexture2D;
    rawGlBlitFramebuffer_t      rawBlitFramebuffer;
    rawGlDeleteFramebuffers_t   rawDeleteFramebuffers;
    rawGlDisable_t              rawDisable;
    rawGlGetString_t            rawGetString;
    int64_t      colorFormat;       /* swapchain format, all three chains */
    bool32       srgbWrite;         /* sRGB chain + write conversion off */
} vrstate;

static vrstate vr;

static void vrLogResult(char const* what, XrResult result)
{
    char buffer[XR_MAX_RESULT_STRING_SIZE] = "";
    if (vr.instance != XR_NULL_HANDLE)
    {
        xrResultToString(vr.instance, result, buffer);
    }
    SDL_Log("VR: %s failed: %s (%d)", what, buffer, (int)result);
}

#define VR_CHECK(what, call)                        \
    {                                               \
        XrResult res_ = (call);                     \
        if (XR_FAILED(res_))                        \
        {                                           \
            vrLogResult(what, res_);                \
            return FALSE;                           \
        }                                           \
    }

/*-----------------------------------------------------------------------------
    OpenXR requires the Android loader to know the JavaVM and activity
    before anything else is called.
----------------------------------------------------------------------------*/
static bool32 vrInitLoader(void)
{
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    JavaVM* vm = NULL;
    jobject activity;
    PFN_xrInitializeLoaderKHR initializeLoader = NULL;
    XrLoaderInitInfoAndroidKHR loaderInfo;

    if (env == NULL || (*env)->GetJavaVM(env, &vm) != 0)
    {
        SDL_Log("VR: no JavaVM available");
        return FALSE;
    }
    activity = (jobject)SDL_AndroidGetActivity();
    if (activity == NULL)
    {
        SDL_Log("VR: no activity available");
        return FALSE;
    }

    if (XR_FAILED(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                                        (PFN_xrVoidFunction*)&initializeLoader))
        || initializeLoader == NULL)
    {
        SDL_Log("VR: xrInitializeLoaderKHR unavailable");
        return FALSE;
    }

    memset(&loaderInfo, 0, sizeof(loaderInfo));
    loaderInfo.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR;
    loaderInfo.applicationVM = vm;
    loaderInfo.applicationContext = (*env)->NewGlobalRef(env, activity);
    VR_CHECK("xrInitializeLoaderKHR", initializeLoader((XrLoaderInitInfoBaseHeaderKHR*)&loaderInfo));
    return TRUE;
}

static bool32 vrCreateInstance(void)
{
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    JavaVM* vm = NULL;
    XrInstanceCreateInfoAndroidKHR androidInfo;
    XrInstanceCreateInfo createInfo;
    static char const* extensions[] = {
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
    };
    XrSystemGetInfo systemInfo;

    (*env)->GetJavaVM(env, &vm);

    memset(&androidInfo, 0, sizeof(androidInfo));
    androidInfo.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
    androidInfo.applicationVM = vm;
    androidInfo.applicationActivity = (*env)->NewGlobalRef(env, (jobject)SDL_AndroidGetActivity());

    memset(&createInfo, 0, sizeof(createInfo));
    createInfo.type = XR_TYPE_INSTANCE_CREATE_INFO;
    createInfo.next = &androidInfo;
    strncpy(createInfo.applicationInfo.applicationName, "Homeworld", XR_MAX_APPLICATION_NAME_SIZE - 1);
    strncpy(createInfo.applicationInfo.engineName, "GoK", XR_MAX_ENGINE_NAME_SIZE - 1);
    createInfo.applicationInfo.applicationVersion = 1;
    createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    createInfo.enabledExtensionCount = sizeof(extensions) / sizeof(extensions[0]);
    createInfo.enabledExtensionNames = extensions;

    VR_CHECK("xrCreateInstance", xrCreateInstance(&createInfo, &vr.instance));

    memset(&systemInfo, 0, sizeof(systemInfo));
    systemInfo.type = XR_TYPE_SYSTEM_GET_INFO;
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    VR_CHECK("xrGetSystem", xrGetSystem(vr.instance, &systemInfo, &vr.systemId));

    return TRUE;
}

/*-----------------------------------------------------------------------------
    Find the EGLConfig of the context SDL created (SDL does not expose it,
    but the config id can be queried and matched).
----------------------------------------------------------------------------*/
static EGLConfig vrCurrentEglConfig(EGLDisplay display, EGLContext context)
{
    EGLint configId = 0, count = 0, i;
    EGLConfig configs[256];

    eglQueryContext(display, context, EGL_CONFIG_ID, &configId);
    if (eglGetConfigs(display, configs, 256, &count))
    {
        for (i = 0; i < count; i++)
        {
            EGLint id = 0;
            eglGetConfigAttrib(display, configs[i], EGL_CONFIG_ID, &id);
            if (id == configId)
            {
                return configs[i];
            }
        }
    }
    return NULL;
}

static bool32 vrCreateSession(void)
{
    PFN_xrGetOpenGLESGraphicsRequirementsKHR getRequirements = NULL;
    XrGraphicsRequirementsOpenGLESKHR requirements;
    XrGraphicsBindingOpenGLESAndroidKHR binding;
    XrSessionCreateInfo createInfo;
    XrReferenceSpaceCreateInfo spaceInfo;
    EGLDisplay display = eglGetCurrentDisplay();
    EGLContext context = eglGetCurrentContext();

    if (display == EGL_NO_DISPLAY || context == EGL_NO_CONTEXT)
    {
        SDL_Log("VR: no current EGL context");
        return FALSE;
    }

    /* Mandatory before session creation */
    VR_CHECK("get xrGetOpenGLESGraphicsRequirementsKHR",
             xrGetInstanceProcAddr(vr.instance, "xrGetOpenGLESGraphicsRequirementsKHR",
                                   (PFN_xrVoidFunction*)&getRequirements));
    memset(&requirements, 0, sizeof(requirements));
    requirements.type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR;
    VR_CHECK("xrGetOpenGLESGraphicsRequirementsKHR",
             getRequirements(vr.instance, vr.systemId, &requirements));
    SDL_Log("VR: runtime wants GLES %d.%d - %d.%d",
            XR_VERSION_MAJOR(requirements.minApiVersionSupported),
            XR_VERSION_MINOR(requirements.minApiVersionSupported),
            XR_VERSION_MAJOR(requirements.maxApiVersionSupported),
            XR_VERSION_MINOR(requirements.maxApiVersionSupported));

    memset(&binding, 0, sizeof(binding));
    binding.type = XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR;
    binding.display = display;
    binding.config = vrCurrentEglConfig(display, context);
    binding.context = context;

    memset(&createInfo, 0, sizeof(createInfo));
    createInfo.type = XR_TYPE_SESSION_CREATE_INFO;
    createInfo.next = &binding;
    createInfo.systemId = vr.systemId;
    VR_CHECK("xrCreateSession", xrCreateSession(vr.instance, &createInfo, &vr.session));

    memset(&spaceInfo, 0, sizeof(spaceInfo));
    spaceInfo.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    VR_CHECK("xrCreateReferenceSpace", xrCreateReferenceSpace(vr.session, &spaceInfo, &vr.space));

    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    VR_CHECK("xrCreateReferenceSpace (view)", xrCreateReferenceSpace(vr.session, &spaceInfo, &vr.viewSpace));

    return TRUE;
}

/* v rotated by quaternion q */
static void vrQuatRotate(XrQuaternionf q, XrVector3f v, XrVector3f* out)
{
    XrVector3f t = {2.0f * (q.y * v.z - q.z * v.y),
                    2.0f * (q.z * v.x - q.x * v.z),
                    2.0f * (q.x * v.y - q.y * v.x)};
    XrVector3f c = {q.y * t.z - q.z * t.y,
                    q.z * t.x - q.x * t.z,
                    q.x * t.y - q.y * t.x};

    out->x = v.x + q.w * t.x + c.x;
    out->y = v.y + q.w * t.y + c.y;
    out->z = v.z + q.w * t.z + c.z;
}

static bool32 vrCreateSwapchain(void)
{
    int64_t formats[256];
    uint32_t formatCount = 0, i;
    int64_t chosen = 0, wanted;
    XrSwapchainCreateInfo createInfo;

    /* Two-call idiom: get the count first, then fetch (capped) */
    VR_CHECK("xrEnumerateSwapchainFormats (count)",
             xrEnumerateSwapchainFormats(vr.session, 0, &formatCount, NULL));
    if (formatCount > 256)
    {
        formatCount = 256;
    }
    VR_CHECK("xrEnumerateSwapchainFormats",
             xrEnumerateSwapchainFormats(vr.session, formatCount, &formatCount, formats));

    /* The game's pixels are already display-referred, so the swapchain has to
       say so - see vrConfigureColorSpace. Without the write-control extension
       an sRGB chain would double-darken instead, so fall back to linear. */
    wanted = vr.srgbWrite ? GL_SRGB8_ALPHA8 : GL_RGBA8;
    for (i = 0; i < formatCount; i++)
    {
        if (formats[i] == wanted)
        {
            chosen = formats[i];
            break;
        }
    }
    if (chosen == 0 && vr.srgbWrite)
    {
        /* The runtime does not offer sRGB after all. The disabled write
           conversion is a no-op against a linear target, so nothing needs
           undoing beyond the bookkeeping. */
        for (i = 0; i < formatCount; i++)
        {
            if (formats[i] == GL_RGBA8)
            {
                chosen = formats[i];
                break;
            }
        }
        if (chosen != 0)
        {
            vr.srgbWrite = FALSE;
            SDL_Log("VR: no sRGB swapchain format offered, falling back to linear");
        }
    }
    if (chosen == 0 && formatCount > 0)
    {
        chosen = formats[0];
        SDL_Log("VR: preferred swapchain format unavailable, using 0x%x", (unsigned)chosen);
    }
    vr.colorFormat = chosen;

    memset(&createInfo, 0, sizeof(createInfo));
    createInfo.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.format = chosen;
    createInfo.sampleCount = 1;
    createInfo.width = vr.width;
    createInfo.height = vr.height;
    createInfo.faceCount = 1;
    createInfo.arraySize = 1;
    createInfo.mipCount = 1;
    VR_CHECK("xrCreateSwapchain", xrCreateSwapchain(vr.session, &createInfo, &vr.swapchain));

    for (i = 0; i < VR_MAX_SWAPCHAIN_IMAGES; i++)
    {
        vr.images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
    }
    VR_CHECK("xrEnumerateSwapchainImages",
             xrEnumerateSwapchainImages(vr.swapchain, VR_MAX_SWAPCHAIN_IMAGES, &vr.imageCount,
                                        (XrSwapchainImageBaseHeader*)vr.images));
    SDL_Log("VR: swapchain %dx%d with %u images", (int)vr.width, (int)vr.height, vr.imageCount);
    return TRUE;
}

static bool32 vrLoadRawGles(void)
{
    void* handle = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);

    if (handle == NULL)
    {
        SDL_Log("VR: cannot dlopen libGLESv2.so: %s", dlerror());
        return FALSE;
    }
    vr.rawGetIntegerv = (rawGlGetIntegerv_t)dlsym(handle, "glGetIntegerv");
    vr.rawGetError = (rawGlGetError_t)dlsym(handle, "glGetError");
    vr.rawGenFramebuffers = (rawGlGenFramebuffers_t)dlsym(handle, "glGenFramebuffers");
    vr.rawBindFramebuffer = (rawGlBindFramebuffer_t)dlsym(handle, "glBindFramebuffer");
    vr.rawFramebufferTexture2D = (rawGlFramebufferTexture2D_t)dlsym(handle, "glFramebufferTexture2D");
    vr.rawBlitFramebuffer = (rawGlBlitFramebuffer_t)dlsym(handle, "glBlitFramebuffer");
    vr.rawDeleteFramebuffers = (rawGlDeleteFramebuffers_t)dlsym(handle, "glDeleteFramebuffers");
    if (!vr.rawGetIntegerv || !vr.rawGetError || !vr.rawGenFramebuffers || !vr.rawBindFramebuffer
        || !vr.rawFramebufferTexture2D || !vr.rawBlitFramebuffer || !vr.rawDeleteFramebuffers)
    {
        SDL_Log("VR: missing GLES symbols");
        return FALSE;
    }
    /* Only needed to turn the sRGB write conversion off; absence costs
       colour accuracy, not the session, so it must not fail the init. */
    vr.rawDisable = (rawGlDisable_t)dlsym(handle, "glDisable");
    vr.rawGetString = (rawGlGetString_t)dlsym(handle, "glGetString");
    return TRUE;
}

/*-----------------------------------------------------------------------------
    Name        : vrConfigureColorSpace
    Description : Decide what colour space the swapchains live in, and stop
                  the driver from re-encoding what the game writes.

        The compositor reads a swapchain according to the format it was
        created with. A linear format (GL_RGBA8) declares "these are linear
        light values", so the runtime gamma-encodes them on the way to the
        display. But this engine is from 1999: its colours are already
        display-referred, so that encode is a second one, and it lifts every
        midtone. Measured on a Quest 3, a hull side that the shader put at
        0.19 reached the panel at 0.45 - which flattens the sunlit/shadowed
        split the whole art direction rests on.

        The fix is to declare the truth, GL_SRGB8_ALPHA8, so the compositor
        decodes before re-encoding and the two cancel. GLES 3.0 then wants to
        convert on *write* into such a target - also wrong for us, for the
        same reason - and EXT_sRGB_write_control is what turns that off. The
        conversion also applies to glBlitFramebuffer, which is how every one
        of our passes reaches its swapchain, so this is not optional.

        No extension means no way to suppress the write conversion, so we
        stay on the linear format: washed out beats double-dark.
    Inputs      : none (requires a current GL context)
    Outputs     : sets vr.srgbWrite, disables GL_FRAMEBUFFER_SRGB
    Return      : void
----------------------------------------------------------------------------*/
static void vrConfigureColorSpace(void)
{
    unsigned char const* extensions;

    vr.srgbWrite = FALSE;
    if (vr.rawDisable == NULL || vr.rawGetString == NULL)
    {
        SDL_Log("VR: no glDisable/glGetString, keeping the linear swapchain");
        return;
    }
    extensions = vr.rawGetString(VR_GL_EXTENSIONS);
    if (extensions == NULL || strstr((char const*)extensions, "GL_EXT_sRGB_write_control") == NULL)
    {
        SDL_Log("VR: no EXT_sRGB_write_control, keeping the linear swapchain "
                "(midtones will read bright)");
        return;
    }

    /* Enabled by default on an ES 3.0 context - the extension exists purely
       to let us switch it off. */
    vr.rawDisable(VR_GL_FRAMEBUFFER_SRGB);
    vr.srgbWrite = TRUE;
    SDL_Log("VR: sRGB swapchains, write conversion off");
}

/*-----------------------------------------------------------------------------
    Touch controller input: aim ray + trigger/buttons, mapped onto the
    virtual screen as mouse input.
----------------------------------------------------------------------------*/
static bool32 vrCreateActions(void)
{
    XrActionSetCreateInfo setInfo;
    XrActionCreateInfo actionInfo;
    XrActionSuggestedBinding bindings[32];
    XrInteractionProfileSuggestedBinding suggested;
    XrSessionActionSetsAttachInfo attachInfo;
    XrActionSpaceCreateInfo spaceInfo;
    XrPath profilePath;
    uword i, bindingCount;

    xrStringToPath(vr.instance, "/user/hand/left", &vr.handPath[VR_HAND_LEFT]);
    xrStringToPath(vr.instance, "/user/hand/right", &vr.handPath[VR_HAND_RIGHT]);

    memset(&setInfo, 0, sizeof(setInfo));
    setInfo.type = XR_TYPE_ACTION_SET_CREATE_INFO;
    strcpy(setInfo.actionSetName, "gameplay");
    strcpy(setInfo.localizedActionSetName, "Gameplay");
    VR_CHECK("xrCreateActionSet", xrCreateActionSet(vr.instance, &setInfo, &vr.actionSet));

    memset(&actionInfo, 0, sizeof(actionInfo));
    actionInfo.type = XR_TYPE_ACTION_CREATE_INFO;
    actionInfo.countSubactionPaths = VR_HAND_COUNT;
    actionInfo.subactionPaths = vr.handPath;

    actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    strcpy(actionInfo.actionName, "aim");
    strcpy(actionInfo.localizedActionName, "Aim");
    VR_CHECK("xrCreateAction aim", xrCreateAction(vr.actionSet, &actionInfo, &vr.aimAction));

    actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    strcpy(actionInfo.actionName, "select");
    strcpy(actionInfo.localizedActionName, "Select");
    VR_CHECK("xrCreateAction select", xrCreateAction(vr.actionSet, &actionInfo, &vr.selectAction));

    strcpy(actionInfo.actionName, "context_menu");
    strcpy(actionInfo.localizedActionName, "Context menu");
    VR_CHECK("xrCreateAction context", xrCreateAction(vr.actionSet, &actionInfo, &vr.contextAction));

    strcpy(actionInfo.actionName, "back");
    strcpy(actionInfo.localizedActionName, "Back");
    VR_CHECK("xrCreateAction back", xrCreateAction(vr.actionSet, &actionInfo, &vr.backAction));

    strcpy(actionInfo.actionName, "stick_click");
    strcpy(actionInfo.localizedActionName, "Stick click");
    VR_CHECK("xrCreateAction stick_click", xrCreateAction(vr.actionSet, &actionInfo, &vr.stickClickAction));

    strcpy(actionInfo.actionName, "grip");
    strcpy(actionInfo.localizedActionName, "Grip");
    VR_CHECK("xrCreateAction grip", xrCreateAction(vr.actionSet, &actionInfo, &vr.gripAction));

    actionInfo.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
    strcpy(actionInfo.actionName, "thumbstick");
    strcpy(actionInfo.localizedActionName, "Thumbstick");
    VR_CHECK("xrCreateAction thumbstick", xrCreateAction(vr.actionSet, &actionInfo, &vr.stickAction));

    actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    strcpy(actionInfo.actionName, "grip_pose");
    strcpy(actionInfo.localizedActionName, "Grip pose");
    VR_CHECK("xrCreateAction grip_pose", xrCreateAction(vr.actionSet, &actionInfo, &vr.gripPoseAction));

    actionInfo.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
    strcpy(actionInfo.actionName, "haptic");
    strcpy(actionInfo.localizedActionName, "Haptic feedback");
    VR_CHECK("xrCreateAction haptic", xrCreateAction(vr.actionSet, &actionInfo, &vr.hapticAction));

    {
        struct { XrAction action; char const* path; } layout[] = {
            { vr.aimAction,        "/user/hand/left/input/aim/pose" },
            { vr.aimAction,        "/user/hand/right/input/aim/pose" },
            { vr.selectAction,     "/user/hand/left/input/trigger/value" },
            { vr.selectAction,     "/user/hand/right/input/trigger/value" },
            { vr.contextAction,    "/user/hand/left/input/x/click" },
            { vr.contextAction,    "/user/hand/right/input/a/click" },
            { vr.backAction,       "/user/hand/left/input/y/click" },
            { vr.backAction,       "/user/hand/right/input/b/click" },
            { vr.stickAction,      "/user/hand/left/input/thumbstick" },
            { vr.stickAction,      "/user/hand/right/input/thumbstick" },
            { vr.stickClickAction, "/user/hand/left/input/thumbstick/click" },
            { vr.stickClickAction, "/user/hand/right/input/thumbstick/click" },
            { vr.gripAction,       "/user/hand/left/input/squeeze/value" },
            { vr.gripAction,       "/user/hand/right/input/squeeze/value" },
            { vr.gripPoseAction,   "/user/hand/left/input/grip/pose" },
            { vr.gripPoseAction,   "/user/hand/right/input/grip/pose" },
            { vr.hapticAction,     "/user/hand/left/output/haptic" },
            { vr.hapticAction,     "/user/hand/right/output/haptic" },
        };
        /* Derive the count: it used to be the literal 18 written out in four
           separate places, and a single stale copy makes
           xrSuggestInteractionProfileBindings fail and every input die at
           once. */
        bindingCount = (uword)(sizeof(layout) / sizeof(layout[0]));
        if (bindingCount > sizeof(bindings) / sizeof(bindings[0]))
        {
            SDL_Log("VR: %u suggested bindings exceeds capacity %u",
                    (unsigned)bindingCount,
                    (unsigned)(sizeof(bindings) / sizeof(bindings[0])));
            return FALSE;
        }
        for (i = 0; i < bindingCount; i++)
        {
            bindings[i].action = layout[i].action;
            xrStringToPath(vr.instance, layout[i].path, &bindings[i].binding);
        }
    }

    xrStringToPath(vr.instance, "/interaction_profiles/oculus/touch_controller", &profilePath);
    memset(&suggested, 0, sizeof(suggested));
    suggested.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
    suggested.interactionProfile = profilePath;
    suggested.countSuggestedBindings = bindingCount;
    suggested.suggestedBindings = bindings;
    VR_CHECK("xrSuggestInteractionProfileBindings",
             xrSuggestInteractionProfileBindings(vr.instance, &suggested));

    memset(&attachInfo, 0, sizeof(attachInfo));
    attachInfo.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO;
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &vr.actionSet;
    VR_CHECK("xrAttachSessionActionSets", xrAttachSessionActionSets(vr.session, &attachInfo));

    memset(&spaceInfo, 0, sizeof(spaceInfo));
    spaceInfo.type = XR_TYPE_ACTION_SPACE_CREATE_INFO;
    spaceInfo.action = vr.aimAction;
    spaceInfo.poseInActionSpace.orientation.w = 1.0f;
    for (i = 0; i < VR_HAND_COUNT; i++)
    {
        spaceInfo.subactionPath = vr.handPath[i];
        VR_CHECK("xrCreateActionSpace", xrCreateActionSpace(vr.session, &spaceInfo, &vr.aimSpace[i]));
    }
    spaceInfo.action = vr.gripPoseAction;
    for (i = 0; i < VR_HAND_COUNT; i++)
    {
        spaceInfo.subactionPath = vr.handPath[i];
        VR_CHECK("xrCreateActionSpace (grip)", xrCreateActionSpace(vr.session, &spaceInfo, &vr.gripSpace[i]));
    }

    return TRUE;
}

/* Heading-only version of a head pose: the hologram is anchored to it, and
   inheriting the head's pitch and roll tilts the entire battle relative to
   the room for as long as the session lasts. Keep the yaw, drop the rest. */
static void vrAnchorFromHead(XrPosef head, XrPosef* out)
{
    real32 yaw = atan2f(2.0f * (head.orientation.w * head.orientation.y
                              + head.orientation.x * head.orientation.z),
                        1.0f - 2.0f * (head.orientation.y * head.orientation.y
                                     + head.orientation.x * head.orientation.x));

    out->position = head.position;
    out->orientation.x = 0.0f;
    out->orientation.y = sinf(yaw * 0.5f);
    out->orientation.z = 0.0f;
    out->orientation.w = cosf(yaw * 0.5f);
}

/* v rotated by the inverse of q (q assumed unit length) */
static void vrQuatUnrotate(XrQuaternionf q, XrVector3f v, XrVector3f* out)
{
    XrQuaternionf conj = {-q.x, -q.y, -q.z, q.w};
    vrQuatRotate(conj, v, out);
}

/* Lazy-follow screen: stable in space while being read/clicked, and glides
   over when the user turns well away - so it can never be lost either. */

static void vrNlerp(XrQuaternionf a, XrQuaternionf b, real32 t, XrQuaternionf* out)
{
    real32 dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    real32 sign = (dot < 0.0f) ? -1.0f : 1.0f;
    real32 mag;

    out->x = a.x + (b.x * sign - a.x) * t;
    out->y = a.y + (b.y * sign - a.y) * t;
    out->z = a.z + (b.z * sign - a.z) * t;
    out->w = a.w + (b.w * sign - a.w) * t;
    mag = sqrtf(out->x * out->x + out->y * out->y + out->z * out->z + out->w * out->w);
    if (mag > 0.0f)
    {
        out->x /= mag; out->y /= mag; out->z /= mag; out->w /= mag;
    }
}

/* orientation looking along f (unit, LOCAL space) with world up */
static void vrLookOrientation(XrVector3f f, XrQuaternionf* out)
{
    XrVector3f zaxis = {-f.x, -f.y, -f.z};                  /* local -Z = forward */
    XrVector3f xaxis, yaxis;
    real32 mag, trace;

    xaxis.x = 1.0f * zaxis.z - 0.0f * zaxis.y;              /* up(0,1,0) x z */
    xaxis.y = 0.0f * zaxis.x - 0.0f * zaxis.z;
    xaxis.z = 0.0f * zaxis.y - 1.0f * zaxis.x;
    mag = sqrtf(xaxis.x * xaxis.x + xaxis.y * xaxis.y + xaxis.z * xaxis.z);
    if (mag < 1e-4f)
    {
        out->x = out->y = out->z = 0.0f;
        out->w = 1.0f;
        return;
    }
    xaxis.x /= mag; xaxis.y /= mag; xaxis.z /= mag;
    yaxis.x = zaxis.y * xaxis.z - zaxis.z * xaxis.y;
    yaxis.y = zaxis.z * xaxis.x - zaxis.x * xaxis.z;
    yaxis.z = zaxis.x * xaxis.y - zaxis.y * xaxis.x;

    /* rotation matrix (columns x,y,z) -> quaternion */
    trace = xaxis.x + yaxis.y + zaxis.z;
    if (trace > 0.0f)
    {
        real32 s = sqrtf(trace + 1.0f) * 2.0f;

        out->w = 0.25f * s;
        out->x = (yaxis.z - zaxis.y) / s;
        out->y = (zaxis.x - xaxis.z) / s;
        out->z = (xaxis.y - yaxis.x) / s;
    }
    else if (xaxis.x > yaxis.y && xaxis.x > zaxis.z)
    {
        real32 s = sqrtf(1.0f + xaxis.x - yaxis.y - zaxis.z) * 2.0f;

        out->w = (yaxis.z - zaxis.y) / s;
        out->x = 0.25f * s;
        out->y = (yaxis.x + xaxis.y) / s;
        out->z = (zaxis.x + xaxis.z) / s;
    }
    else if (yaxis.y > zaxis.z)
    {
        real32 s = sqrtf(1.0f + yaxis.y - xaxis.x - zaxis.z) * 2.0f;

        out->w = (zaxis.x - xaxis.z) / s;
        out->x = (yaxis.x + xaxis.y) / s;
        out->y = 0.25f * s;
        out->z = (zaxis.y + yaxis.z) / s;
    }
    else
    {
        real32 s = sqrtf(1.0f + zaxis.z - xaxis.x - yaxis.y) * 2.0f;

        out->w = (xaxis.y - yaxis.x) / s;
        out->x = (zaxis.x + xaxis.z) / s;
        out->y = (zaxis.y + yaxis.z) / s;
        out->z = 0.25f * s;
    }
}

#define VR_WRIST_PANEL_WIDTH 0.32f

/* Watch-face pose for a hand: a panel riding just above the controller,
   tilted so it faces the user when the arm is raised naturally. Both the
   left wrist panel and the wrist cards are placed from this. */
static bool32 vrWristPose(uword hand, XrTime displayTime, XrPosef* out)
{
    XrSpaceLocation grip;
    XrVector3f offset, worldOffset;
    XrQuaternionf tilt = {0.0f, -0.7071068f, 0.7071068f, 0.0f};  /* +90degX then 180degZ roll */
    XrQuaternionf q;

    memset(&grip, 0, sizeof(grip));
    grip.type = XR_TYPE_SPACE_LOCATION;
    /* the aim pose is used (grip pose returns no tracking on this runtime) */
    if (XR_FAILED(xrLocateSpace(vr.aimSpace[hand], vr.space, displayTime, &grip))
        || !(grip.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
        || !(grip.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
    {
        if (vr.frameCount % 3600 == 1)
        {
            SDL_Log("VR: wrist pose: hand %u aim not tracked (flags 0x%x)",
                    (unsigned)hand, (unsigned)grip.locationFlags);
        }
        return FALSE;
    }

    offset.x = 0.0f;
    offset.y = 0.10f;                                       //above the controller
    offset.z = 0.06f;                                       //slightly toward the wrist
    vrQuatRotate(grip.pose.orientation, offset, &worldOffset);
    out->position.x = grip.pose.position.x + worldOffset.x;
    out->position.y = grip.pose.position.y + worldOffset.y;
    out->position.z = grip.pose.position.z + worldOffset.z;

    q = grip.pose.orientation;
    out->orientation.w = q.w * tilt.w - q.x * tilt.x - q.y * tilt.y - q.z * tilt.z;
    out->orientation.x = q.w * tilt.x + q.x * tilt.w + q.y * tilt.z - q.z * tilt.y;
    out->orientation.y = q.w * tilt.y - q.x * tilt.z + q.y * tilt.w + q.z * tilt.x;
    out->orientation.z = q.w * tilt.z + q.x * tilt.y - q.y * tilt.x + q.z * tilt.w;
    return TRUE;
}

/* In-game the screen becomes a small panel riding the left wrist */
static bool32 vrUpdateWristPanel(XrTime displayTime)
{
    XrPosef pose;

    if (!vrWristPose(VR_HAND_LEFT, displayTime, &pose))
    {
        return FALSE;
    }
    vr.quadPose = pose;
    vr.quadWidth = VR_WRIST_PANEL_WIDTH;
    vr.quadPlaced = TRUE;
    return TRUE;
}

static void vrPushKey(SDL_Keycode sym, SDL_Scancode scancode, bool32 down);

/* also rejects NaN: neither comparison holds for it */
static bool32 vrFinite(real32 v)
{
    return v > -1.0e9f && v < 1.0e9f;
}

/* A pose the compositor will accept: finite position, unit quaternion. */
static bool32 vrPoseSubmittable(XrPosef const* pose)
{
    real32 norm = pose->orientation.x * pose->orientation.x
                + pose->orientation.y * pose->orientation.y
                + pose->orientation.z * pose->orientation.z
                + pose->orientation.w * pose->orientation.w;

    return vrFinite(pose->position.x) && vrFinite(pose->position.y)
        && vrFinite(pose->position.z)
        && norm > 0.99f && norm < 1.01f;
}

/* Hold a direction within VR_MANAGER_PITCH_SIN of eye level so a manager
   never opens at the user's feet or above their head. */
static void vrClampPitch(XrVector3f* dir, XrVector3f fallback)
{
    real32 limit = VR_MANAGER_PITCH_SIN;
    real32 horizontal, want, wantHorizontal, rescale;

    if (dir->y <= limit && dir->y >= -limit)
    {
        return;
    }
    want = dir->y > 0.0f ? limit : -limit;
    wantHorizontal = sqrtf(1.0f - want * want);
    horizontal = sqrtf(dir->x * dir->x + dir->z * dir->z);
    if (horizontal < 1e-4f)
    {                                                       //straight up/down
        horizontal = sqrtf(fallback.x * fallback.x + fallback.z * fallback.z);
        if (horizontal < 1e-4f)
        {
            dir->x = 0.0f;
            dir->y = want;
            dir->z = -wantHorizontal;
            return;
        }
        dir->x = fallback.x;
        dir->z = fallback.z;
    }
    rescale = wantHorizontal / horizontal;
    dir->x *= rescale;
    dir->z *= rescale;
    dir->y = want;
}

/* Panel pose VR_MANAGER_DISTANCE along dir from the head, facing back at it.
   Returns FALSE when the result is not submittable. */
static bool32 vrManagerPoseAlong(XrVector3f headPos, XrVector3f dir, XrPosef* out)
{
    real32 mag = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);

    if (!(mag > 1e-4f) || !vrFinite(mag))
    {
        return FALSE;
    }
    dir.x /= mag;
    dir.y /= mag;
    dir.z /= mag;
    out->position.x = headPos.x + dir.x * VR_MANAGER_DISTANCE;
    out->position.y = headPos.y + dir.y * VR_MANAGER_DISTANCE;
    out->position.z = headPos.z + dir.z * VR_MANAGER_DISTANCE;
    vrLookOrientation(dir, &out->orientation);
    return vrPoseSubmittable(out);
}

/* Manager panel placement. On the opening frame the panel is aimed at the
   ship the manager concerns, clamped into a cone around the current gaze so
   it is always substantially in view. After that it stays put in space (it
   is a screen being read and clicked) and only glides back if the user turns
   well away from it. Returns TRUE once a submittable pose exists. */
static bool32 vrUpdateManagerPanel(XrTime displayTime)
{
    XrSpaceLocation head;
    XrVector3f gaze = {0.0f, 0.0f, -1.0f};
    XrVector3f forward, desired, toPanel;
    XrPosef pose;
    real32 mag, dot;

    memset(&head, 0, sizeof(head));
    head.type = XR_TYPE_SPACE_LOCATION;
    if (XR_FAILED(xrLocateSpace(vr.viewSpace, vr.space, displayTime, &head))
        || !(head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
        || !(head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
    {
        if (vr.managerFrames % VR_DEBUG_INTERVAL == 1)
        {
            SDL_Log("VR: manager '%s' panel: head not located (flags 0x%x)",
                    vrWorldManagerName(), (unsigned)head.locationFlags);
        }
        return FALSE;
    }
    vrQuatRotate(head.pose.orientation, gaze, &forward);
    vr.quadWidth = VR_MANAGER_WIDTH;

    if (!vr.managerPlaced)
    {
        real32 shipPos[3], shipRadius = 0.0f;
        bool32 biased = FALSE;

        desired = forward;
        if (vrWorldManagerPanelAnchor(shipPos, &shipRadius))
        {
            XrVector3f toShip;
            real32 distance;

            toShip.x = shipPos[0] - head.pose.position.x;
            toShip.y = shipPos[1] - head.pose.position.y;
            toShip.z = shipPos[2] - head.pose.position.z;
            distance = sqrtf(toShip.x * toShip.x + toShip.y * toShip.y
                           + toShip.z * toShip.z);
            if (distance > 1e-3f && vrFinite(distance))
            {
                toShip.x /= distance;
                toShip.y /= distance;
                toShip.z /= distance;
                dot = toShip.x * forward.x + toShip.y * forward.y
                    + toShip.z * forward.z;
                if (dot >= VR_MANAGER_CONE_COS)
                {                                           //already in view
                    desired = toShip;
                    biased = TRUE;
                }
                else
                {
                    /* rotate the gaze exactly to the cone edge, in the
                       plane containing the ship - the panel leans toward
                       the ship without ever leaving the field of view */
                    XrVector3f perp;

                    perp.x = toShip.x - forward.x * dot;
                    perp.y = toShip.y - forward.y * dot;
                    perp.z = toShip.z - forward.z * dot;
                    mag = sqrtf(perp.x * perp.x + perp.y * perp.y
                              + perp.z * perp.z);
                    if (mag > 1e-4f)
                    {
                        desired.x = forward.x * VR_MANAGER_CONE_COS
                                  + (perp.x / mag) * VR_MANAGER_CONE_SIN;
                        desired.y = forward.y * VR_MANAGER_CONE_COS
                                  + (perp.y / mag) * VR_MANAGER_CONE_SIN;
                        desired.z = forward.z * VR_MANAGER_CONE_COS
                                  + (perp.z / mag) * VR_MANAGER_CONE_SIN;
                        biased = TRUE;
                    }
                }
            }
        }
        vrClampPitch(&desired, forward);
        if (!vrManagerPoseAlong(head.pose.position, desired, &pose))
        {
            SDL_Log("VR: manager '%s' panel pose rejected: head=(%.2f %.2f %.2f) "
                    "dir=(%.3f %.3f %.3f) biased=%d",
                    vrWorldManagerName(), head.pose.position.x,
                    head.pose.position.y, head.pose.position.z,
                    desired.x, desired.y, desired.z, (int)biased);
            return FALSE;
        }
        vr.quadPose = pose;
        vr.managerPlaced = TRUE;
        vr.managerFollowing = FALSE;
        SDL_Log("VR: manager '%s' panel placed at (%.2f %.2f %.2f) "
                "dir=(%.3f %.3f %.3f) width=%.2f biased=%d shipRadius=%.2f "
                "camAge=%u frame=%u",
                vrWorldManagerName(), vr.quadPose.position.x,
                vr.quadPose.position.y, vr.quadPose.position.z,
                desired.x, desired.y, desired.z, vr.quadWidth, (int)biased,
                shipRadius, (unsigned)vrWorldGameCameraAge(),
                (unsigned)vr.frameCount);
        return TRUE;
    }

    /* established: keep the panel where it is, but never let it be lost */
    toPanel.x = vr.quadPose.position.x - head.pose.position.x;
    toPanel.y = vr.quadPose.position.y - head.pose.position.y;
    toPanel.z = vr.quadPose.position.z - head.pose.position.z;
    mag = sqrtf(toPanel.x * toPanel.x + toPanel.y * toPanel.y
              + toPanel.z * toPanel.z);
    if (!(mag > 0.20f) || !vrFinite(mag))
    {                                                       //walked into it
        vr.managerPlaced = FALSE;
        return FALSE;
    }
    dot = (forward.x * toPanel.x + forward.y * toPanel.y
         + forward.z * toPanel.z) / mag;
    if (dot < VR_MANAGER_LOST_COS)
    {
        vr.managerFollowing = TRUE;
    }
    if (vr.managerFollowing)
    {
        desired = forward;
        vrClampPitch(&desired, forward);
        if (vrManagerPoseAlong(head.pose.position, desired, &pose))
        {
            real32 const alpha = VR_MANAGER_FOLLOW_ALPHA;

            vr.quadPose.position.x += (pose.position.x - vr.quadPose.position.x) * alpha;
            vr.quadPose.position.y += (pose.position.y - vr.quadPose.position.y) * alpha;
            vr.quadPose.position.z += (pose.position.z - vr.quadPose.position.z) * alpha;
            vrNlerp(vr.quadPose.orientation, pose.orientation, alpha,
                    &vr.quadPose.orientation);
        }
        if (dot > VR_MANAGER_HOME_COS)
        {
            vr.managerFollowing = FALSE;
        }
    }
    else
    {
        /* re-aim only: the panel turns to face the user as they move
           around it, without drifting from where it was placed */
        XrQuaternionf facing;
        XrVector3f unit;

        unit.x = toPanel.x / mag;
        unit.y = toPanel.y / mag;
        unit.z = toPanel.z / mag;
        vrLookOrientation(unit, &facing);
        vr.quadPose.orientation = facing;
    }
    if (!vrPoseSubmittable(&vr.quadPose))
    {
        SDL_Log("VR: manager '%s' panel pose went bad: pos=(%.2f %.2f %.2f) "
                "q=(%.4f %.4f %.4f %.4f) - re-placing",
                vrWorldManagerName(), vr.quadPose.position.x,
                vr.quadPose.position.y, vr.quadPose.position.z,
                vr.quadPose.orientation.x, vr.quadPose.orientation.y,
                vr.quadPose.orientation.z, vr.quadPose.orientation.w);
        vr.managerPlaced = FALSE;
        return FALSE;
    }
    if (vr.managerFrames % VR_DEBUG_INTERVAL == 1)
    {
        SDL_Log("VR: manager '%s' panel pos=(%.2f %.2f %.2f) distance=%.2f "
                "gazeDot=%.3f following=%d state=%d frames=%u",
                vrWorldManagerName(), vr.quadPose.position.x,
                vr.quadPose.position.y, vr.quadPose.position.z, mag, dot,
                (int)vr.managerFollowing, (int)vr.managerState,
                (unsigned)vr.managerFrames);
    }
    return TRUE;
}

/* Re-place the hologram in front of the player, here and now.
   vr.anchorPose is only ever assigned outside gameplay, so in-game the battle
   stays anchored to wherever the head happened to be on the main menu - which
   for a standing player means it can end up at the wrong height, or behind
   them, with no way back. This shifts vr.worldOffset (already composed into
   both the eye anchor and vrWorldFrameBegin) so the anchor lands at the
   current head position, keeping the anchor's own heading. */
static void vrRecentreHologram(XrTime displayTime)
{
    XrSpaceLocation head;
    XrPosef wanted;

    memset(&head, 0, sizeof(head));
    head.type = XR_TYPE_SPACE_LOCATION;
    if (XR_FAILED(xrLocateSpace(vr.viewSpace, vr.space, displayTime, &head))
        || !(head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
        || !(head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
    {
        SDL_Log("VR: recentre skipped, head not located (flags 0x%x)",
                (unsigned)head.locationFlags);
        return;
    }
    vrAnchorFromHead(head.pose, &wanted);
    vr.worldOffset.x = wanted.position.x - vr.anchorPose.position.x;
    vr.worldOffset.y = wanted.position.y - vr.anchorPose.position.y;
    vr.worldOffset.z = wanted.position.z - vr.anchorPose.position.z;
    /* adopt the current heading too, so the fleet faces the way you do */
    vr.anchorPose.orientation = wanted.orientation;
    SDL_Log("VR: hologram recentred, offset=(%.2f %.2f %.2f) yaw q=(%.3f %.3f)",
            vr.worldOffset.x, vr.worldOffset.y, vr.worldOffset.z,
            vr.anchorPose.orientation.y, vr.anchorPose.orientation.w);
}

static void vrManagerPanelReset(void)
{
    if (vr.managerState != VR_MGR_HIDDEN)
    {
        SDL_Log("VR: manager panel closed after %u frames (state=%d)",
                (unsigned)vr.managerFrames, (int)vr.managerState);
    }
    vr.managerState = VR_MGR_HIDDEN;
    vr.managerFrames = 0;
    vr.managerPendingFrames = 0;
    vr.managerPlaced = FALSE;
    vr.managerPoseValid = FALSE;
    vr.managerFollowing = FALSE;
}

static void vrUpdateScreenPose(XrTime displayTime)
{
    XrSpaceLocation location;
    XrVector3f forward = {0.0f, 0.0f, -VR_SCREEN_DISTANCE}, offset;
    XrVector3f gaze = {0.0f, 0.0f, -1.0f}, facing, toQuad;
    XrPosef desired;
    real32 mag, dot;

    if (vr.worldInteractive && vrWorldManagerActive())
    {
        if (vr.managerState == VR_MGR_HIDDEN)
        {
            vr.managerState = VR_MGR_PENDING;
            vr.managerFrames = 0;
            vr.managerPendingFrames = 0;
            vr.managerPlaced = FALSE;
            vr.managerFollowing = FALSE;
            SDL_Log("VR: manager '%s' opened: presentation pending, camAge=%u",
                    vrWorldManagerName(), (unsigned)vrWorldGameCameraAge());
        }
        vr.managerFrames++;
        vr.managerPoseValid = vrUpdateManagerPanel(displayTime);
        if (vr.managerPoseValid)
        {
            vr.quadPlaced = TRUE;
            vr.managerPendingFrames = 0;
            return;
        }

        vr.managerPendingFrames++;
        if (vr.managerState == VR_MGR_VISIBLE)
        {
            /* The panel stopped being presentable - tracking dropout, or a
               pose that went bad. Hand the world controls straight back:
               a manager the user cannot see must never also be modal. */
            SDL_Log("VR: manager '%s' panel no longer presentable at frame %u "
                    "- input no longer modal", vrWorldManagerName(),
                    (unsigned)vr.managerFrames);
            vr.managerState = VR_MGR_PENDING;
        }
        if (vr.managerPendingFrames > VR_MANAGER_PENDING_FRAMES)
        {
            /* The panel is the only way out of a manager. If one never
               becomes presentable, close it rather than stranding the user
               in a screen they can neither see nor dismiss. */
            SDL_Log("VR: manager '%s' not presentable for %u frames - closing",
                    vrWorldManagerName(), (unsigned)vr.managerPendingFrames);
            if (!vrWorldCloseManagers())
            {
                vrPushKey(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, TRUE);
                vrPushKey(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, FALSE);
            }
            vrManagerPanelReset();
        }
        return;
    }

    vrManagerPanelReset();
    if (vr.worldInteractive)
    {
        /* if the controller loses tracking, the panel holds its last wrist
           pose instead of reverting to the big floating screen */
        vrUpdateWristPanel(displayTime);
        return;
    }
    else
    {
        vr.quadWidth = VR_SCREEN_WIDTH;
    }

    memset(&location, 0, sizeof(location));
    location.type = XR_TYPE_SPACE_LOCATION;
    if (XR_FAILED(xrLocateSpace(vr.viewSpace, vr.space, displayTime, &location))
        || !(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
        || !(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
    {
        return;
    }

    vrQuatRotate(location.pose.orientation, forward, &offset);
    desired.position.x = location.pose.position.x + offset.x;
    desired.position.y = location.pose.position.y + offset.y;
    desired.position.z = location.pose.position.z + offset.z;
    desired.orientation = location.pose.orientation;

    if (!vr.quadPlaced)
    {
        vr.quadPose = desired;
        vrAnchorFromHead(location.pose, &vr.anchorPose);    //world anchor
        vr.quadPlaced = TRUE;
        vr.quadFollowing = FALSE;
        SDL_Log("VR: screen anchored at (%.2f, %.2f, %.2f), anchor yaw q=(%.3f %.3f)",
                desired.position.x, desired.position.y, desired.position.z,
                vr.anchorPose.orientation.y, vr.anchorPose.orientation.w);
        return;
    }

    /* how far off gaze is the screen? */
    vrQuatRotate(location.pose.orientation, gaze, &facing);
    toQuad.x = vr.quadPose.position.x - location.pose.position.x;
    toQuad.y = vr.quadPose.position.y - location.pose.position.y;
    toQuad.z = vr.quadPose.position.z - location.pose.position.z;
    mag = sqrtf(toQuad.x * toQuad.x + toQuad.y * toQuad.y + toQuad.z * toQuad.z);
    if (mag < 0.25f)
    {
        return;
    }
    dot = (facing.x * toQuad.x + facing.y * toQuad.y + facing.z * toQuad.z) / mag;

    if (dot < 0.55f)                                        //> ~57 degrees off gaze
    {
        vr.quadFollowing = TRUE;
    }
    if (vr.quadFollowing)
    {
        real32 const alpha = 0.08f;                         //~0.4s glide at 72Hz

        vr.quadPose.position.x += (desired.position.x - vr.quadPose.position.x) * alpha;
        vr.quadPose.position.y += (desired.position.y - vr.quadPose.position.y) * alpha;
        vr.quadPose.position.z += (desired.position.z - vr.quadPose.position.z) * alpha;
        vrNlerp(vr.quadPose.orientation, desired.orientation, alpha, &vr.quadPose.orientation);
        if (dot > 0.996f)                                   //settled in front again
        {
            vr.quadFollowing = FALSE;
        }
    }
}

/* Height of the main UI quad for a given width.

   This is the LOGICAL aspect, not the framebuffer's. The game lays its UI and
   its mono world out for rndAspectRatio, which is
   MAIN_WindowWidth/MAIN_WindowHeight - 4:3 - and then rasterises that into a
   4128x2208 framebuffer, roughly 1.87:1. Sizing the quad by the framebuffer
   aspect therefore displayed 4:3 content in a 1.87:1 rectangle and stretched
   everything about 1.4x horizontally. Sizing it by the logical aspect undoes
   the rasterisation stretch exactly, with nothing cropped or letterboxed.

   The pointer intersection has to use the same figure or clicks land at the
   wrong height, which is why both go through here. */
static real32 vrQuadHeightFor(real32 width)
{
    if (MAIN_WindowWidth <= 0)
    {
        return width * (real32)vr.height / (real32)vr.width;
    }
    return width * (real32)MAIN_WindowHeight / (real32)MAIN_WindowWidth;
}

/* Intersect a hand's aim ray with the virtual screen; returns TRUE and the
   game-window pixel coordinates on hit. */
static bool32 vrPointerFromHand(uword hand, XrTime time, sdword* px, sdword* py,
                                real32* hitT)
{
    XrSpaceLocation location;
    XrVector3f origin, forward = {0.0f, 0.0f, -1.0f}, direction, local, localDir;
    real32 t, quadHeight, u, v;

    memset(&location, 0, sizeof(location));
    location.type = XR_TYPE_SPACE_LOCATION;
    if (XR_FAILED(xrLocateSpace(vr.aimSpace[hand], vr.space, time, &location))
        || !(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
        || !(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
    {
        return FALSE;
    }

    /* ray into the quad's local frame (quad plane is z=0) */
    vrQuatRotate(location.pose.orientation, forward, &direction);
    origin.x = location.pose.position.x - vr.quadPose.position.x;
    origin.y = location.pose.position.y - vr.quadPose.position.y;
    origin.z = location.pose.position.z - vr.quadPose.position.z;
    vrQuatUnrotate(vr.quadPose.orientation, origin, &local);
    vrQuatUnrotate(vr.quadPose.orientation, direction, &localDir);

    if (localDir.z > -1e-5f)                                //parallel or pointing away
    {
        return FALSE;
    }
    t = -local.z / localDir.z;
    if (t <= 0.0f)
    {
        return FALSE;
    }

    quadHeight = vrQuadHeightFor(vr.quadWidth);
    u = (local.x + t * localDir.x) / vr.quadWidth + 0.5f;
    v = 0.5f - (local.y + t * localDir.y) / quadHeight;
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
    {
        return FALSE;
    }

    *px = (sdword)(u * (real32)vr.width);
    *py = (sdword)(v * (real32)vr.height);
    *hitT = t;                                              //ray clip distance, metres
    return TRUE;
}

static void vrPushMouseButton(ubyte button, bool32 down)
{
    SDL_Event event;

    memset(&event, 0, sizeof(event));
    event.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    event.button.windowID = SDL_GetWindowID(sdlwindow);
    event.button.button = button;
    event.button.state = down ? SDL_PRESSED : SDL_RELEASED;
    event.button.clicks = 1;
    event.button.x = vr.pointerX;
    event.button.y = vr.pointerY;
    SDL_PushEvent(&event);
}

static void vrPushKey(SDL_Keycode sym, SDL_Scancode scancode, bool32 down)
{
    SDL_Event event;

    memset(&event, 0, sizeof(event));
    event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    event.key.windowID = SDL_GetWindowID(sdlwindow);
    event.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    event.key.keysym.sym = sym;
    event.key.keysym.scancode = scancode;
    SDL_PushEvent(&event);
}

static void vrHapticPulse(uword hand, real32 amplitude, XrDuration duration)
{
    XrHapticActionInfo info;
    XrHapticVibration vibration;

    if (vr.hapticAction == XR_NULL_HANDLE || vr.state != XR_SESSION_STATE_FOCUSED)
    {
        return;
    }
    memset(&info, 0, sizeof(info));
    info.type = XR_TYPE_HAPTIC_ACTION_INFO;
    info.action = vr.hapticAction;
    info.subactionPath = vr.handPath[hand];
    memset(&vibration, 0, sizeof(vibration));
    vibration.type = XR_TYPE_HAPTIC_VIBRATION;
    vibration.duration = duration;
    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
    vibration.amplitude = amplitude;
    xrApplyHapticFeedback(vr.session, &info, (XrHapticBaseHeader const*)&vibration);
}

/* The game cursor is drawn before vrFrame updates the OpenXR pointer. Draw a
   small current-frame reticle into the same framebuffer copied to the wrist
   quad, making the visual hit exact even though the beam is in another
   compositor layer. */
static void vrDrawPanelReticle(void)
{
    color const shadow = colRGB(8, 18, 24);
    color const cyan = colRGB(70, 230, 255);
    bool32 const wasPrimMode = primModeEnabled;
    sdword x, y;

    if (!vr.pointerValid || vr.width <= 0 || vr.height <= 0)
    {
        return;
    }
    /* The pointer is in framebuffer pixels, which on Quest is an integer
       multiple of the logical UI resolution prim2d draws in (4128x2208 vs
       1032x552). Drawing it unscaled put the reticle off the panel entirely. */
    x = vr.pointerX * MAIN_WindowWidth / vr.width;
    y = vr.pointerY * MAIN_WindowHeight / vr.height;
    primModeSet2();
    primLineThick2(x - 11, y, x - 4, y, 4, shadow);
    primLineThick2(x + 4, y, x + 11, y, 4, shadow);
    primLineThick2(x, y - 11, x, y - 4, 4, shadow);
    primLineThick2(x, y + 4, x, y + 11, 4, shadow);
    primLineThick2(x - 10, y, x - 4, y, 2, cyan);
    primLineThick2(x + 4, y, x + 10, y, 2, cyan);
    primLineThick2(x, y - 10, x, y - 4, 2, cyan);
    primLineThick2(x, y + 4, x, y + 10, 2, cyan);
    if (!wasPrimMode)
    {
        primModeClear2();
    }
    glFlush();
}

/* Bright border around the panel while a manager owns the screen. On device
   this separates the two ways a manager can look wrong: no border means the
   quad is not where the user is looking (or was not submitted), a border
   around empty space means the game drew nothing into the framebuffer. */
static void vrDrawManagerBorder(void)
{
    color const edge = colRGB(255, 140, 20);
    bool32 const wasPrimMode = primModeEnabled;
    /* prim2d works in logical window space, which maps to the whole
       framebuffer - and so to the whole quad - however it is scaled */
    sdword right = MAIN_WindowWidth - 1;
    sdword bottom = MAIN_WindowHeight - 1;

    if (!vr.worldInteractive || !vrWorldManagerActive())
    {
        return;
    }
    primModeSet2();
    primLineThick2(0, 2, right, 2, 5, edge);
    primLineThick2(0, bottom - 2, right, bottom - 2, 5, edge);
    primLineThick2(2, 0, 2, bottom, 5, edge);
    primLineThick2(right - 2, 0, right - 2, bottom, 5, edge);
    if (!wasPrimMode)
    {
        primModeClear2();
    }
    glFlush();
}

/*-----------------------------------------------------------------------------
    Wrist cards
----------------------------------------------------------------------------*/
static bool32 vrActionPressedHand(XrAction action, uword hand);
static void vrActionStick(uword hand, real32* x, real32* y);
static void vrHapticPulse(uword hand, real32 amplitude, XrDuration duration);

static void vrCardInit(void)
{
    vr.card[VR_CARD_CONTROLS].winWidth = VR_CARD_CONTROLS_WIN_W;
    vr.card[VR_CARD_CONTROLS].winHeight = VR_CARD_CONTROLS_WIN_H;
    vr.card[VR_CARD_CONTROLS].widthMetres = VR_CARD_CONTROLS_WIDTH;
    vr.card[VR_CARD_STATUS].winWidth = VR_CARD_STATUS_WIN_W;
    vr.card[VR_CARD_STATUS].winHeight = VR_CARD_STATUS_WIN_H;
    vr.card[VR_CARD_STATUS].widthMetres = VR_CARD_STATUS_WIDTH;
    vr.card[VR_CARD_WHEEL].winWidth = VR_WHEEL_WIN_W;
    vr.card[VR_CARD_WHEEL].winHeight = VR_WHEEL_WIN_H;
    vr.card[VR_CARD_WHEEL].widthMetres = VR_WHEEL_WIDTH;
    vr.wheelSlot = -1;
}

/* Created on first use, like the eye buffers: a card is a convenience, so a
   failure here must never take the session down with it. */
static bool32 vrCardSwapchain(sdword index)
{
    vrcard* card = &vr.card[index];
    XrSwapchainCreateInfo createInfo;
    uint32_t i;

    if (card->swapchain != XR_NULL_HANDLE)
    {
        return TRUE;
    }
    if (card->failed)
    {
        return FALSE;
    }
    /* Size the swapchain to the framebuffer-space footprint of the layout so
       the blit out of it is exactly 1:1. It used to downscale (4x source into
       a 2x texture) with a LINEAR filter, which GLES forbids when the read
       buffer is multisampled - so MSAA would have silently broken every card. */
    if (MAIN_WindowWidth <= 0 || MAIN_WindowHeight <= 0)
    {
        card->failed = TRUE;
        return FALSE;
    }
    card->texWidth = card->winWidth * vr.width / MAIN_WindowWidth;
    card->texHeight = card->winHeight * vr.height / MAIN_WindowHeight;

    memset(&createInfo, 0, sizeof(createInfo));
    createInfo.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT
                          | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    /* Cards must share the world's colour space or the wrist panels drift
       away from the scene they sit in front of. */
    createInfo.format = vr.colorFormat;
    createInfo.sampleCount = 1;
    createInfo.width = card->texWidth;
    createInfo.height = card->texHeight;
    createInfo.faceCount = 1;
    createInfo.arraySize = 1;
    createInfo.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(vr.session, &createInfo, &card->swapchain)))
    {
        SDL_Log("VR: card %d swapchain %dx%d failed", (int)index,
                (int)card->texWidth, (int)card->texHeight);
        card->swapchain = XR_NULL_HANDLE;
        card->failed = TRUE;
        return FALSE;
    }
    for (i = 0; i < VR_MAX_SWAPCHAIN_IMAGES; i++)
    {
        card->images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
    }
    if (XR_FAILED(xrEnumerateSwapchainImages(card->swapchain,
                                             VR_MAX_SWAPCHAIN_IMAGES,
                                             &card->imageCount,
                                             (XrSwapchainImageBaseHeader*)card->images)))
    {
        SDL_Log("VR: card %d image enumeration failed", (int)index);
        card->failed = TRUE;
        return FALSE;
    }
    SDL_Log("VR: card %d swapchain %dx%d with %u images, %.2fm wide",
            (int)index, (int)card->texWidth, (int)card->texHeight,
            card->imageCount, card->widthMetres);
    return TRUE;
}

static fonthandle vrCardFont(void)
{
    if (!vr.cardFontTried)
    {
        vr.cardFontTried = TRUE;
        vr.cardFont = frFontRegister("default.hff");
        SDL_Log("VR: card font handle %u", (unsigned)vr.cardFont);
    }
    return vr.cardFont;
}

/* One row of "label ... value" inside the card, in logical UI pixels */
static void vrCardRow(sdword x, sdword y, sdword width, char const* label,
                      char const* value, color labelColor, color valueColor)
{
    fontPrint(x, y, labelColor, (char*)label);
    if (value != NULL)
    {
        fontPrint(x + width - fontWidth((char*)value), y, valueColor,
                  (char*)value);
    }
}

static void vrCardDrawControls(vrcard const* card)
{
    static char const* rows[][2] = {
        {"COMMAND WHEEL",       NULL},
        {"hold L-trigger",      "open wheel"},
        {"L-stick",             "pick wedge"},
        {"dwell on wedge",      "submenu"},
        {"release L-trigger",   "do it"},
        {"B/Y",                 "back / close"},
        {"",                    NULL},
        {"SELECT",              NULL},
        {"R-trig own ship",     "select"},
        {"L-grip + trigger",    "add / toggle"},
        {"trigger x2",          "all of type"},
        {"empty + sweep",       "group brush"},
        {"",                    NULL},
        {"R-TRIGGER A TARGET",  NULL},
        {"enemy",               "attack"},
        {"resource",            "harvest"},
        {"derelict + salvcorv", "salvage"},
        {"damaged + repcorv",   "repair"},
        {"",                    NULL},
        {"ORDERS  hold A",      NULL},
        {"on enemy",            "attack"},
        {"on resource",         "harvest"},
        {"on own ship",         "dock"},
        {"on empty",            "move"},
        {"+ trigger on foes",   "multi-attack"},
        {"R-stick Y",           "order depth"},
        {"+ trigger, sweep",    "draw path"},
        {"release A",           "fly path"},
        {"B",                   "cancel"},
        {"",                    NULL},
        {"CAMERA",              NULL},
        {"L-stick",             "orbit"},
        {"R-stick Y",           "zoom"},
        {"both grips",          "pinch zoom"},
        {"R-grip + R-stick X",  "cycle fleet"},
        {"L-stick click",       "focus + recentre"},
        {"R-stick click",       "sensors"},
        {"",                    NULL},
        {"LEFT = COMMANDER",    NULL},
        {"X",                   "undo"},
        {"Y",                   "hide panel"},
        {"B (right)",           "close mgr"},
    };
    sdword const count = (sdword)(sizeof(rows) / sizeof(rows[0]));
    color const heading = colRGB(120, 230, 255);
    color const label = colRGB(210, 225, 235);
    color const value = colRGB(255, 205, 110);
    sdword lineHeight = fontHeight("Ay");
    sdword inner = 10;
    sdword y = inner + 2;
    sdword i;

    if (lineHeight <= 0)
    {
        lineHeight = 12;
    }
    for (i = 0; i < count && y + lineHeight < card->winHeight - inner; i++)
    {
        /* the tail of this list is how the user escapes a manager, so a
           silent overflow is not acceptable: say so once and loudly */
        if (i + 1 == count && !vr.cardOverflowLogged)
        {
            vr.cardOverflowLogged = TRUE;
            SDL_Log("VR: controls card fits all %d rows (lineHeight=%d y=%d "
                    "of %d)", (int)count, (int)lineHeight, (int)y,
                    (int)card->winHeight);
        }
        if (rows[i][1] == NULL)
        {
            if (rows[i][0][0] != '\0')
            {
                fontPrint(inner, y, heading, (char*)rows[i][0]);
                y += lineHeight;
            }
            else
            {
                y += lineHeight / 2;
            }
            continue;
        }
        vrCardRow(inner, y, card->winWidth - inner * 2, rows[i][0], rows[i][1],
                  label, value);
        y += lineHeight;
    }
    if (i < count && !vr.cardOverflowLogged)
    {
        vr.cardOverflowLogged = TRUE;
        SDL_Log("VR: controls card OVERFLOW: only %d of %d rows fit "
                "(lineHeight=%d, need ~%d px, have %d) - raise "
                "VR_CARD_CONTROLS_WIN_H", (int)i, (int)count, (int)lineHeight,
                (int)(count * lineHeight + inner * 2), (int)card->winHeight);
    }
}

/*-----------------------------------------------------------------------------
    Command wheel pages
----------------------------------------------------------------------------*/
#define VR_WHEEL_PAGE_ROOT       0
#define VR_WHEEL_PAGE_FORMATIONS 1
#define VR_WHEEL_PAGE_TACTICS    2
#define VR_WHEEL_PAGE_ORDERS     3
#define VR_WHEEL_PAGE_MANAGERS   4
#define VR_WHEEL_PAGE_VIEW       5
#define VR_WHEEL_PAGE_GROUPS     6
#define VR_WHEEL_PAGE_COUNT      7

typedef struct {
    char const*    label;
    vrworldcommand cmd;         /* VRW_CMD_NONE when this only opens a page */
    sdword         arg;
    sdword         page;        /* submenu to open, -1 for none */
    bool32         repeat;      /* fires while held, not on release */
} vrwheelslot;

typedef struct {
    char const*  title;
    sdword       slotCount;
    vrwheelslot  slot[10];
} vrwheelpage;

/* A wedge with both a cmd and a page does the cmd on a flick and opens the
   page on a dwell, so the common case stays a single gesture. */
static vrwheelpage const vrWheelPage[VR_WHEEL_PAGE_COUNT] = {
    { "COMMAND", 8, {
        { "Formation",  VRW_CMD_NONE,        0, VR_WHEEL_PAGE_FORMATIONS },
        { "Groups",     VRW_CMD_NONE,        0, VR_WHEEL_PAGE_GROUPS },
        { "Tactics",    VRW_CMD_NONE,        0, VR_WHEEL_PAGE_TACTICS },
        { "Build",      VRW_CMD_BUILD,       0, -1 },
        { "Undo",       VRW_CMD_UNDO,        0, -1 },
        { "Research",   VRW_CMD_RESEARCH,    0, -1 },
        { "View",       VRW_CMD_NONE,        0, VR_WHEEL_PAGE_VIEW },
        { "Orders",     VRW_CMD_NONE,        0, VR_WHEEL_PAGE_ORDERS },
    }},
    { "FORMATION", 7, {
        { "Delta",      VRW_CMD_FORM_DELTA,  0, -1 },
        { "Broad",      VRW_CMD_FORM_BROAD,  0, -1 },
        { "X",          VRW_CMD_FORM_X,      0, -1 },
        { "Claw",       VRW_CMD_FORM_CLAW,   0, -1 },
        { "Wall",       VRW_CMD_FORM_WALL,   0, -1 },
        { "Sphere",     VRW_CMD_FORM_SPHERE, 0, -1 },
        { "Custom",     VRW_CMD_FORM_CUSTOM, 0, -1 },
    }},
    { "TACTICS", 3, {
        { "Evasive",    VRW_CMD_TACTIC_EVASIVE,    0, -1 },
        { "Neutral",    VRW_CMD_TACTIC_NEUTRAL,    0, -1 },
        { "Aggressive", VRW_CMD_TACTIC_AGGRESSIVE, 0, -1 },
    }},
    { "ORDERS", 7, {
        { "Halt",       VRW_CMD_HALT,      0, -1 },
        { "Harvest",    VRW_CMD_HARVEST,   0, -1 },
        { "Dock",       VRW_CMD_DOCK,      0, -1 },
        { "Special",    VRW_CMD_SPECIAL,   0, -1 },
        { "Kamikaze",   VRW_CMD_KAMIKAZE,  0, -1 },
        { "Retire",     VRW_CMD_RETIRE,    0, -1 },
        { "Scuttle",    VRW_CMD_SCUTTLE,   0, -1 },
    }},
    { "MANAGERS", 4, {
        { "Build",      VRW_CMD_BUILD,      0, -1 },
        { "Launch",     VRW_CMD_LAUNCH,     0, -1 },
        { "Research",   VRW_CMD_RESEARCH,   0, -1 },
        { "Hyperspace", VRW_CMD_HYPERSPACE, 0, -1 },
    }},
    { "VIEW", 7, {
        { "Mothership", VRW_CMD_MOTHERSHIP, 0, -1, FALSE },
        { "Next focus", VRW_CMD_FOCUS_NEXT, 0, -1, FALSE },
        { "Closer",     VRW_CMD_SCALE_UP,   0, -1, TRUE },
        { "Select all", VRW_CMD_SELECT_ALL, 0, -1, FALSE },
        { "Sensors",    VRW_CMD_SENSORS,    0, -1, FALSE },
        { "Further",    VRW_CMD_SCALE_DOWN, 0, -1, TRUE },
        { "Prev focus", VRW_CMD_FOCUS_PREV, 0, -1, FALSE },
    }},
    /* group labels are rewritten per frame with their ship counts */
    { "GROUPS", 10, {
        { "1", VRW_CMD_GROUP_RECALL, 1, -1 },
        { "2", VRW_CMD_GROUP_RECALL, 2, -1 },
        { "3", VRW_CMD_GROUP_RECALL, 3, -1 },
        { "4", VRW_CMD_GROUP_RECALL, 4, -1 },
        { "5", VRW_CMD_GROUP_RECALL, 5, -1 },
        { "6", VRW_CMD_GROUP_RECALL, 6, -1 },
        { "7", VRW_CMD_GROUP_RECALL, 7, -1 },
        { "8", VRW_CMD_GROUP_RECALL, 8, -1 },
        { "9", VRW_CMD_GROUP_RECALL, 9, -1 },
        { "0", VRW_CMD_GROUP_RECALL, 0, -1 },
    }},
};

static vrwheelpage const* vrWheelCurrentPage(void)
{
    sdword page = vr.wheelPage;

    if (page < 0 || page >= VR_WHEEL_PAGE_COUNT)
    {
        page = VR_WHEEL_PAGE_ROOT;
    }
    return &vrWheelPage[page];
}

/* Wedge under the stick, or -1 inside the deadzone. Slot 0 is straight up and
   they run clockwise, so a slot's direction never depends on how many slots
   the page happens to have... except in count, which is why every page keeps
   its slots in a fixed order. */
static sdword vrWheelSlotFromStick(real32 x, real32 y, sdword slotCount)
{
    real32 angle;
    sdword slot;

    if (slotCount <= 0 || x * x + y * y < VR_WHEEL_DEADZONE * VR_WHEEL_DEADZONE)
    {
        return -1;
    }
    angle = atan2f(x, y);                                   //0 = up, cw
    if (angle < 0.0f)
    {
        angle += 6.2831853f;
    }
    slot = (sdword)((angle / 6.2831853f) * (real32)slotCount + 0.5f);
    return slot % slotCount;
}

/* Is this slot's command available right now? Store-modifier aware. */
static bool32 vrWheelSlotEnabled(vrwheelslot const* slot, bool32 storeModifier)
{
    if (slot->cmd == VRW_CMD_NONE)
    {
        return slot->page >= 0;
    }
    if (slot->cmd == VRW_CMD_GROUP_RECALL && storeModifier)
    {
        return vrWorldCommandEnabled(VRW_CMD_GROUP_STORE, slot->arg);
    }
    return vrWorldCommandEnabled(slot->cmd, slot->arg);
}

/* Radial helpers. Everything is built from filled triangles and thick lines
   in plain logical-UI coordinates, which are the primitives whose conventions
   are already proven by the other cards - no guessing at arc angle bases.
   Slot 0 is straight up and they run clockwise, matching
   vrWheelSlotFromStick. */
static void vrWheelPointAt(sdword cx, sdword cy, real32 angle, real32 radius,
                           sdword* x, sdword* y)
{
    *x = cx + (sdword)(sinf(angle) * radius);
    *y = cy - (sdword)(cosf(angle) * radius);
}

/* Filled annular sector, as a strip of triangles */
static void vrWheelSector(sdword cx, sdword cy, real32 from, real32 to,
                          real32 innerR, real32 outerR, color c)
{
    sdword const steps = 10;
    sdword step;

    for (step = 0; step < steps; step++)
    {
        real32 a0 = from + (to - from) * (real32)step / (real32)steps;
        real32 a1 = from + (to - from) * (real32)(step + 1) / (real32)steps;
        triangle tri;
        sdword i0x, i0y, o0x, o0y, i1x, i1y, o1x, o1y;

        vrWheelPointAt(cx, cy, a0, innerR, &i0x, &i0y);
        vrWheelPointAt(cx, cy, a0, outerR, &o0x, &o0y);
        vrWheelPointAt(cx, cy, a1, innerR, &i1x, &i1y);
        vrWheelPointAt(cx, cy, a1, outerR, &o1x, &o1y);

        tri.x0 = i0x; tri.y0 = i0y;
        tri.x1 = o0x; tri.y1 = o0y;
        tri.x2 = o1x; tri.y2 = o1y;
        primTriSolid2(&tri, c);
        tri.x0 = i0x; tri.y0 = i0y;
        tri.x1 = o1x; tri.y1 = o1y;
        tri.x2 = i1x; tri.y2 = i1y;
        primTriSolid2(&tri, c);
    }
}

/* Arc drawn as short chords, so no arc-primitive angle convention is assumed */
static void vrWheelArc(sdword cx, sdword cy, real32 from, real32 to,
                       real32 radius, sdword thickness, color c)
{
    sdword const steps = 14;
    sdword step, px = 0, py = 0;

    for (step = 0; step <= steps; step++)
    {
        real32 a = from + (to - from) * (real32)step / (real32)steps;
        sdword x, y;

        vrWheelPointAt(cx, cy, a, radius, &x, &y);
        if (step > 0)
        {
            primLineThick2(px, py, x, y, thickness, c);
        }
        px = x;
        py = y;
    }
}

static void vrCardDrawWheel(vrcard const* card)
{
    vrwheelpage const* page = vrWheelCurrentPage();
    bool32 storeModifier = vrActionPressedHand(vr.gripAction, VR_HAND_LEFT);
    color const band     = colRGB(17, 27, 38);
    color const bandEdge = colRGB(38, 66, 86);
    color const spoke    = colRGB(30, 52, 68);
    color const selFill  = colRGB(30, 74, 104);
    color const selEdge  = colRGB(120, 214, 255);
    color const hubFill  = colRGB(12, 20, 29);
    color const needle   = colRGB(90, 190, 235);
    color const textOn   = colRGB(228, 238, 246);
    color const textOff  = colRGB(78, 90, 102);
    color const textSel  = colRGB(255, 238, 165);
    color const textMark = colRGB(120, 255, 170);
    color const titleCol = colRGB(120, 214, 255);
    color const hint     = colRGB(96, 112, 126);
    color const shadowC  = colRGB(3, 6, 10);
    sdword cx = card->winWidth / 2;
    sdword cy = card->winHeight / 2;
    sdword span = (card->winWidth < card->winHeight ? card->winWidth
                                                   : card->winHeight);
    real32 outerR = (real32)span * 0.44f;
    real32 innerR = (real32)span * 0.30f;
    real32 labelR = (innerR + outerR) * 0.5f;
    sdword hubR = (sdword)((real32)span * 0.155f);
    sdword lineHeight = fontHeight("Ay");
    real32 half;
    real32 lx, ly, stickMag;
    FontShadowType oldShadow = fontShadowGet();
    sdword i;

    if (lineHeight <= 0)
    {
        lineHeight = 12;
    }
    if (page->slotCount <= 0)
    {
        return;
    }
    half = 3.14159265f / (real32)page->slotCount;

    /* the band the labels live on, then the selected sector on top of it */
    primCircleBorder(cx, cy, (sdword)innerR, (sdword)outerR, 64, band);
    if (vr.wheelSlot >= 0 && vr.wheelSlot < page->slotCount)
    {
        real32 centre = 6.2831853f * (real32)vr.wheelSlot
                      / (real32)page->slotCount;

        vrWheelSector(cx, cy, centre - half, centre + half, innerR, outerR,
                      selFill);
        vrWheelArc(cx, cy, centre - half, centre + half, outerR, 3, selEdge);
        vrWheelArc(cx, cy, centre - half, centre + half, innerR, 2, selEdge);
    }

    /* spokes on the wedge boundaries make the angular mapping visible, which
       is what lets the wheel become a flick rather than a read */
    for (i = 0; i < page->slotCount; i++)
    {
        real32 edge = 6.2831853f * ((real32)i + 0.5f) / (real32)page->slotCount;
        sdword ix, iy, ox, oy;

        vrWheelPointAt(cx, cy, edge, innerR, &ix, &iy);
        vrWheelPointAt(cx, cy, edge, outerR, &ox, &oy);
        primLineThick2(ix, iy, ox, oy, 1, spoke);
    }
    vrWheelArc(cx, cy, 0.0f, 6.2831853f, outerR, 2, bandEdge);
    vrWheelArc(cx, cy, 0.0f, 6.2831853f, innerR, 1, bandEdge);

    /* live stick needle: proves the stick is registering even in the deadzone */
    vrActionStick(VR_HAND_LEFT, &lx, &ly);
    stickMag = sqrtf(lx * lx + ly * ly);
    if (stickMag > 0.08f)
    {
        real32 angle = atan2f(lx, ly);
        real32 reach = (real32)hubR + 4.0f
                     + (innerR - (real32)hubR - 8.0f)
                       * (stickMag > 1.0f ? 1.0f : stickMag);
        sdword nx, ny;

        vrWheelPointAt(cx, cy, angle, reach, &nx, &ny);
        primLineThick2(cx, cy, nx, ny, 3,
                       stickMag >= VR_WHEEL_DEADZONE ? selEdge : needle);
    }

    /* hub: page title, and the group wheel's live recall/store mode */
    primCircleSolid2(cx, cy, hubR, 28, hubFill);
    vrWheelArc(cx, cy, 0.0f, 6.2831853f, (real32)hubR, 2, bandEdge);
    fontShadowSet(FS_SE, shadowC);
    fontPrint(cx - fontWidth((char*)page->title) / 2, cy - lineHeight,
              titleCol, (char*)page->title);
    if (page->slot[0].cmd == VRW_CMD_GROUP_RECALL)
    {
        char const* mode = storeModifier ? "STORE" : "recall";

        fontPrint(cx - fontWidth((char*)mode) / 2, cy + 2,
                  storeModifier ? textMark : hint, (char*)mode);
    }

    for (i = 0; i < page->slotCount; i++)
    {
        vrwheelslot const* slot = &page->slot[i];
        real32 angle = 6.2831853f * (real32)i / (real32)page->slotCount;
        bool32 enabled = vrWheelSlotEnabled(slot, storeModifier);
        bool32 active = vrWorldCommandActive(slot->cmd);
        char const* label = slot->label;
        char text[32];
        sdword x, y;
        color c;

        if (slot->cmd == VRW_CMD_GROUP_RECALL)
        {
            sdword size = vrWorldGroupSize(slot->arg);

            if (size > 0)
            {
                sprintf(text, "%s\xb7%d", slot->label, (int)size);
            }
            else
            {
                sprintf(text, "%s", slot->label);
            }
            label = text;
        }
        vrWheelPointAt(cx, cy, angle, labelR, &x, &y);
        c = !enabled ? textOff
                     : (i == vr.wheelSlot ? textSel
                                          : (active ? textMark : textOn));
        fontPrint(x - fontWidth((char*)label) / 2, y - lineHeight / 2, c,
                  (char*)label);
        /* a category wedge says so, so nobody waits for a command that is
           really a submenu */
        if (slot->page >= 0 && enabled)
        {
            fontPrint(x - fontWidth("...") / 2, y + lineHeight / 2 + 1,
                      i == vr.wheelSlot ? textSel : hint, "...");
        }
        else if (active)
        {
            primCircleSolid2(x - fontWidth((char*)label) / 2 - 7, y, 3, 10,
                             textMark);
        }
    }

    /* what the buttons do from here */
    {
        char const* back = vr.wheelPage != VR_WHEEL_PAGE_ROOT
                         ? "B/Y back" : "B/Y close";

        fontPrint(cx - fontWidth((char*)back) / 2,
                  card->winHeight - lineHeight - 4, hint, (char*)back);
    }
    fontShadowSet(oldShadow, shadowC);
}

static void vrCardDrawStatus(vrcard const* card)
{
    color const heading = colRGB(120, 230, 255);
    color const label = colRGB(210, 225, 235);
    color const value = colRGB(255, 205, 110);
    sdword lineHeight = fontHeight("Ay");
    sdword inner = 10;
    sdword width = card->winWidth - inner * 2;
    sdword y = inner + 2;
    char text[64];

    if (lineHeight <= 0)
    {
        lineHeight = 12;
    }
    fontPrint(inner, y, heading, "FLEET STATUS");
    y += lineHeight + lineHeight / 3;

    if (universe.curPlayerPtr == NULL)
    {
        fontPrint(inner, y, label, "no player");
        return;
    }
    sprintf(text, "%d", (int)universe.curPlayerPtr->resourceUnits);
    vrCardRow(inner, y, width, "resource units", text, label, value);
    y += lineHeight;

    sprintf(text, "%d", (int)universe.curPlayerPtr->totalships);
    vrCardRow(inner, y, width, "ships", text, label, value);
    y += lineHeight;

    sprintf(text, "%d", (int)selSelected.numShips);
    vrCardRow(inner, y, width, "selected", text, label, value);
    y += lineHeight;

    sprintf(text, "%d", (int)universe.curPlayerPtr->sensorLevel);
    vrCardRow(inner, y, width, "sensors level", text, label, value);
    y += lineHeight;

    sprintf(text, "%d", (int)universe.curPlayerPtr->classtotals[CLASS_Fighter]);
    vrCardRow(inner, y, width, "fighters", text, label, value);
    y += lineHeight;

    sprintf(text, "%d", (int)universe.curPlayerPtr->classtotals[CLASS_Corvette]);
    vrCardRow(inner, y, width, "corvettes", text, label, value);
    y += lineHeight;

    sprintf(text, "%d", (int)universe.curPlayerPtr->classtotals[CLASS_Frigate]);
    vrCardRow(inner, y, width, "frigates", text, label, value);
    y += lineHeight;

    /* While an order is being placed, depth is the one thing stereo cannot
       convey: past ~10m of hologram the disparity is indistinguishable from
       infinity, so the number is the only real feedback the player gets. */
    if (vrWorldMoveActive())
    {
        color const active = colRGB(120, 255, 170);

        y += lineHeight / 3;
        sprintf(text, "%d", (int)vrWorldCursorDist());
        vrCardRow(inner, y, width, "order depth", text, active, active);
    }
}

/* Lay a card out in the top-left of the window framebuffer and blit it into
   its swapchain. Runs after the game frame has already been copied to the UI
   quad and before the eye passes clear the framebuffer, so the scratch
   drawing is never seen and never survives. */
static void vrCardRender(sdword index, uint32_t imageIndex)
{
    vrcard const* card = &vr.card[index];
    color const backdrop = colRGB(10, 16, 24);
    color const edge = colRGB(60, 140, 180);
    bool32 const wasPrimMode = primModeEnabled;
    fonthandle previousFont;
    rectangle rect;
    sdword srcX1, srcY0;

    rect.x0 = 0;
    rect.y0 = 0;
    rect.x1 = card->winWidth;
    rect.y1 = card->winHeight;

    primModeSet2();
    primRectSolid2(&rect, backdrop);
    primRectOutline2(&rect, 2, edge);
    previousFont = fontMakeCurrent(vrCardFont());
    if (index == VR_CARD_CONTROLS)
    {
        vrCardDrawControls(card);
    }
    else if (index == VR_CARD_WHEEL)
    {
        vrCardDrawWheel(card);
    }
    else
    {
        vrCardDrawStatus(card);
    }
    fontMakeCurrent(previousFont);
    if (!wasPrimMode)
    {
        primModeClear2();
    }
    glFlush();

    /* prim2d's origin is top-left in logical UI pixels; GL row 0 is the
       bottom of the framebuffer, so the laid-out block is the top-left
       corner scaled up by the framebuffer/UI ratio. */
    /* 1:1 and NEAREST - see vrCardSwapchain.

       Note this is still NOT legal from a multisampled read buffer, even at
       1:1: a resolve requires the source and destination rectangles to be
       identical, not merely the same size, and this reads from srcY0 while
       writing to 0 because the layout is drawn at the top of the framebuffer
       whereas GL row 0 is the bottom. Enabling window MSAA therefore turns
       every card black - the blit fails with INVALID_OPERATION and nothing is
       written - which is exactly what happened. vrCopyFrame and vrBlitEye
       survive it because their rectangles genuinely are identical.

       This is moot if antialiasing arrives via multisampled eye framebuffers
       rather than a multisampled window, which is the direction worth taking
       anyway for cost reasons: the window stays single-sampled and these
       blits stay legal. */
    srcX1 = card->texWidth;
    srcY0 = vr.height - card->texHeight;
    vr.rawBindFramebuffer(VR_GL_DRAW_FRAMEBUFFER, vr.blitFbo);
    vr.rawFramebufferTexture2D(VR_GL_DRAW_FRAMEBUFFER, VR_GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, card->images[imageIndex].image, 0);
    vr.rawBindFramebuffer(VR_GL_READ_FRAMEBUFFER, 0);
    vr.rawBlitFramebuffer(0, srcY0, srcX1, vr.height,
                          0, 0, card->texWidth, card->texHeight,
                          VR_GL_COLOR_BUFFER_BIT, VR_GL_NEAREST);
    vr.rawBindFramebuffer(VR_GL_DRAW_FRAMEBUFFER, 0);
}

/* Cards ride the wrists alongside the panel, so they follow the same
   visibility rule: in-game, panel not hidden, no manager owning the screen. */
static void vrUpdateCardPoses(XrTime displayTime)
{
    vrcard* controls = &vr.card[VR_CARD_CONTROLS];
    vrcard* status = &vr.card[VR_CARD_STATUS];
    bool32 wanted = vr.worldInteractive && !vr.panelHidden
                 && !vrWorldManagerActive();
    XrPosef pose;

    controls->poseValid = FALSE;
    status->poseValid = FALSE;
    if (!wanted)
    {
        return;
    }

    /* controls card: outboard of the left wrist panel, in its own plane */
    if (vrWristPose(VR_HAND_LEFT, displayTime, &pose))
    {
        XrVector3f right = {1.0f, 0.0f, 0.0f}, offset;
        real32 shift = (VR_WRIST_PANEL_WIDTH + controls->widthMetres) * 0.5f
                     + VR_CARD_GAP;

        vrQuatRotate(pose.orientation, right, &offset);
        controls->pose.orientation = pose.orientation;
        controls->pose.position.x = pose.position.x
            + offset.x * shift * VR_CARD_CONTROLS_SIDE;
        controls->pose.position.y = pose.position.y
            + offset.y * shift * VR_CARD_CONTROLS_SIDE;
        controls->pose.position.z = pose.position.z
            + offset.z * shift * VR_CARD_CONTROLS_SIDE;
        controls->poseValid = vrPoseSubmittable(&controls->pose);
    }

    /* status card: watch face on the right wrist */
    if (vrWristPose(VR_HAND_RIGHT, displayTime, &pose))
    {
        status->pose = pose;
        status->poseValid = vrPoseSubmittable(&status->pose);
    }
}

/* The wheel takes its position from the left wrist so it feels attached to
   the commanding hand, but its orientation from the head. A watch-face pose
   would roll with the wrist, and a radial whose "up" rotates has no stable
   mapping from stick direction to wedge - which is the whole point of it. */
static void vrUpdateWheelPose(XrTime displayTime)
{
    vrcard* wheel = &vr.card[VR_CARD_WHEEL];
    XrSpaceLocation head;
    XrPosef wrist;
    XrVector3f toWheel;
    real32 mag;

    wheel->poseValid = FALSE;
    if (!vr.wheelOpen)
    {
        return;
    }
    memset(&head, 0, sizeof(head));
    head.type = XR_TYPE_SPACE_LOCATION;
    if (!vrWristPose(VR_HAND_LEFT, displayTime, &wrist)
        || XR_FAILED(xrLocateSpace(vr.viewSpace, vr.space, displayTime, &head))
        || !(head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
    {
        return;
    }
    wheel->pose.position = wrist.position;
    toWheel.x = wrist.position.x - head.pose.position.x;
    toWheel.y = wrist.position.y - head.pose.position.y;
    toWheel.z = wrist.position.z - head.pose.position.z;
    mag = sqrtf(toWheel.x * toWheel.x + toWheel.y * toWheel.y
              + toWheel.z * toWheel.z);
    if (!(mag > 0.05f) || !vrFinite(mag))
    {
        return;
    }
    toWheel.x /= mag;
    toWheel.y /= mag;
    toWheel.z /= mag;
    vrLookOrientation(toWheel, &wheel->pose.orientation);
    wheel->poseValid = vrPoseSubmittable(&wheel->pose);
}

/* Draw, blit and describe one card as a quad layer. Returns FALSE when the
   card should not be submitted this frame. */
static bool32 vrCardSubmit(sdword index, XrCompositionLayerQuad* quad)
{
    vrcard const* card = &vr.card[index];
    XrSwapchainImageAcquireInfo acquireInfo;
    XrSwapchainImageWaitInfo waitInfo;
    XrSwapchainImageReleaseInfo releaseInfo;
    uint32_t imageIndex = 0;

    if (!card->poseValid || !vrCardSwapchain(index) || vr.blitFbo == 0)
    {
        return FALSE;
    }
    memset(&acquireInfo, 0, sizeof(acquireInfo));
    acquireInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
    memset(&waitInfo, 0, sizeof(waitInfo));
    waitInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
    waitInfo.timeout = XR_INFINITE_DURATION;
    memset(&releaseInfo, 0, sizeof(releaseInfo));
    releaseInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;

    if (XR_FAILED(xrAcquireSwapchainImage(card->swapchain, &acquireInfo, &imageIndex))
        || XR_FAILED(xrWaitSwapchainImage(card->swapchain, &waitInfo)))
    {
        return FALSE;
    }
    vrCardRender(index, imageIndex);
    xrReleaseSwapchainImage(card->swapchain, &releaseInfo);

    memset(quad, 0, sizeof(*quad));
    quad->type = XR_TYPE_COMPOSITION_LAYER_QUAD;
    quad->space = vr.space;
    quad->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad->subImage.swapchain = card->swapchain;
    quad->subImage.imageRect.extent.width = card->texWidth;
    quad->subImage.imageRect.extent.height = card->texHeight;
    quad->pose = card->pose;
    quad->size.width = card->widthMetres;
    quad->size.height = card->widthMetres * (real32)card->winHeight
                      / (real32)card->winWidth;
    return TRUE;
}

static bool32 vrActionPressedHand(XrAction action, uword hand)
{
    XrActionStateGetInfo getInfo;
    XrActionStateBoolean state;

    memset(&getInfo, 0, sizeof(getInfo));
    getInfo.type = XR_TYPE_ACTION_STATE_GET_INFO;
    getInfo.action = action;
    getInfo.subactionPath = vr.handPath[hand];
    memset(&state, 0, sizeof(state));
    state.type = XR_TYPE_ACTION_STATE_BOOLEAN;
    return XR_SUCCEEDED(xrGetActionStateBoolean(vr.session, &getInfo, &state))
           && state.isActive && state.currentState;
}

static void vrActionStick(uword hand, real32* x, real32* y)
{
    XrActionStateGetInfo getInfo;
    XrActionStateVector2f state;

    *x = *y = 0.0f;
    memset(&getInfo, 0, sizeof(getInfo));
    getInfo.type = XR_TYPE_ACTION_STATE_GET_INFO;
    getInfo.action = vr.stickAction;
    getInfo.subactionPath = vr.handPath[hand];
    memset(&state, 0, sizeof(state));
    state.type = XR_TYPE_ACTION_STATE_VECTOR2F;
    if (XR_SUCCEEDED(xrGetActionStateVector2f(vr.session, &getInfo, &state)) && state.isActive)
    {
        *x = state.currentState.x;
        *y = state.currentState.y;
    }
}

/* Aim pose of a hand in LOCAL space: position in metres + forward dir */
static bool32 vrHandAimLocal(uword hand, XrTime time, real32 pos[3], real32 dir[3])
{
    XrSpaceLocation location;
    XrVector3f forward = {0.0f, 0.0f, -1.0f}, d;

    memset(&location, 0, sizeof(location));
    location.type = XR_TYPE_SPACE_LOCATION;
    if (XR_FAILED(xrLocateSpace(vr.aimSpace[hand], vr.space, time, &location))
        || !(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
        || !(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
    {
        return FALSE;
    }
    pos[0] = location.pose.position.x;
    pos[1] = location.pose.position.y;
    pos[2] = location.pose.position.z;
    vr.debugAimPose[hand] = location.pose;
    vrQuatRotate(location.pose.orientation, forward, &d);
    dir[0] = d.x;
    dir[1] = d.y;
    dir[2] = d.z;
    return TRUE;
}

/* Change how many game units a metre is worth, keeping the point the camera
   is looking at where it already is.

   Scale alone does not change anything's apparent SIZE - angular size is
   hull/distance, and both are in game units - so what it changes is how far
   away the fleet physically feels, and with it the stereo depth cue. Raising
   it pulls the battle onto a tabletop you can lean over; lowering it pushes it
   out to something you stand inside.

   Naively changing it slides the whole hologram, because the lookat point sits
   at anchorPos + R_anchor . (0,0,-cameraDistance) / scale. Shifting the anchor
   by cameraDistance * (1/old - 1/new) along the anchor's forward axis cancels
   that exactly, so the fleet grows and shrinks about the thing being looked at
   rather than lurching past the player. */
static bool32 vrWorldScaleStep(real32 factor)
{
    real32 wanted = vr.worldScale * factor;
    XrVector3f forward = {0.0f, 0.0f, -1.0f}, along;
    real32 shift;

    if (wanted < VR_WORLD_SCALE_MIN)
    {
        wanted = VR_WORLD_SCALE_MIN;
    }
    else if (wanted > VR_WORLD_SCALE_MAX)
    {
        wanted = VR_WORLD_SCALE_MAX;
    }
    if (!vrFinite(wanted) || wanted <= 0.0f
        || wanted == vr.worldScale)                         //already clamped
    {
        return FALSE;
    }
    if (mrCamera != NULL)
    {
        shift = mrCamera->distance * (1.0f / vr.worldScale - 1.0f / wanted);
        vrQuatRotate(vr.anchorPose.orientation, forward, &along);
        vr.worldOffset.x += along.x * shift;
        vr.worldOffset.y += along.y * shift;
        vr.worldOffset.z += along.z * shift;
    }
    SDL_Log("VR: world scale %.0f -> %.0f units/metre (fleet at %.2fm)",
            vr.worldScale, wanted,
            mrCamera != NULL ? mrCamera->distance / wanted : 0.0f);
    vr.worldScale = wanted;
    return TRUE;
}

/* Close the wheel, optionally running whatever wedge is highlighted. */
/* Run one wedge. Shared by release-to-commit and by repeating wedges, so a
   held wedge and a released one can never mean different things. */
static void vrWheelFire(vrwheelslot const* slot, real32 haptic)
{
    vrworldcommand cmd = slot->cmd;

    if (cmd == VRW_CMD_NONE)
    {
        return;
    }
    /* the group page is the one place a modifier changes the verb */
    if (cmd == VRW_CMD_GROUP_RECALL
        && vrActionPressedHand(vr.gripAction, VR_HAND_LEFT))
    {
        cmd = VRW_CMD_GROUP_STORE;
    }
    /* scale is presentation, so the OpenXR layer owns it rather than vrworld */
    if (cmd == VRW_CMD_SCALE_UP || cmd == VRW_CMD_SCALE_DOWN)
    {
        if (vrWorldScaleStep(cmd == VRW_CMD_SCALE_UP
                             ? VR_WORLD_SCALE_STEP : 1.0f / VR_WORLD_SCALE_STEP))
        {
            vrHapticPulse(VR_HAND_LEFT, haptic * 0.5f, 18000000);
        }
        return;
    }
    if (vrWorldCommand(cmd, slot->arg))
    {
        vrHapticPulse(VR_HAND_LEFT, haptic, 40000000);
    }
}

static void vrWheelClose(bool32 commit)
{
    if (!vr.wheelOpen)
    {
        return;
    }
    if (commit && vr.wheelSlot >= 0)
    {
        vrwheelpage const* page = vrWheelCurrentPage();

        if (vr.wheelSlot < page->slotCount
            && !page->slot[vr.wheelSlot].repeat)
        {
            vrWheelFire(&page->slot[vr.wheelSlot], 0.50f);
        }
    }
    SDL_Log("VR: wheel closed (commit=%d page=%d slot=%d)", (int)commit,
            (int)vr.wheelPage, (int)vr.wheelSlot);
    vr.wheelOpen = FALSE;
    vr.wheelPage = VR_WHEEL_PAGE_ROOT;
    vr.wheelSlot = -1;
    vr.wheelSubOpened = FALSE;
}

/* Left trigger holds the wheel open, the left stick picks a wedge, release
   commits. Dwelling on a category wedge descends into it. */
static void vrUpdateWheelInput(XrTime time, bool32 held)
{
    vrwheelpage const* page;
    real32 lx, ly;
    sdword slot;

    if (held && !vr.wheelOpen)
    {
        vr.wheelOpen = TRUE;
        vr.wheelPage = VR_WHEEL_PAGE_ROOT;
        vr.wheelSlot = -1;
        vr.wheelSlotSince = time;
        vr.wheelSubOpened = FALSE;
        vrHapticPulse(VR_HAND_LEFT, 0.25f, 20000000);
        SDL_Log("VR: wheel opened");
        return;
    }
    if (!vr.wheelOpen)
    {
        return;
    }
    if (!held)
    {
        vrWheelClose(TRUE);
        return;
    }

    page = vrWheelCurrentPage();
    vrActionStick(VR_HAND_LEFT, &lx, &ly);
    slot = vrWheelSlotFromStick(lx, ly, page->slotCount);
    if (slot != vr.wheelSlot)
    {
        vr.wheelSlot = slot;
        vr.wheelSlotSince = time;
        vr.wheelRepeatAt = time + VR_WHEEL_REPEAT_FIRST_NS;
        vr.wheelSubOpened = FALSE;
        if (slot >= 0)
        {
            vrHapticPulse(VR_HAND_LEFT, 0.16f, 10000000);
        }
        return;
    }
    /* A wedge that adjusts a continuous quantity - hologram scale - would be
       useless at one step per open-and-release, so those fire on an interval
       while held instead, and consume the release so it does not fire twice. */
    if (slot >= 0 && slot < page->slotCount && page->slot[slot].repeat)
    {
        if (time >= vr.wheelRepeatAt)
        {
            vrWheelFire(&page->slot[slot], 0.30f);
            vr.wheelRepeatAt = time + VR_WHEEL_REPEAT_NS;
        }
        return;
    }
    /* dwell on a category descends. Reset the highlight on the way in so the
       stick's current direction cannot immediately commit something on the
       new page - the player has to aim again, which is what they expect. */
    if (slot >= 0 && !vr.wheelSubOpened && slot < page->slotCount
        && page->slot[slot].page >= 0
        && time - vr.wheelSlotSince >= VR_WHEEL_HOLD_NS)
    {
        vr.wheelPage = page->slot[slot].page;
        vr.wheelSlot = -1;
        vr.wheelSlotSince = time;
        vr.wheelSubOpened = TRUE;
        vrHapticPulse(VR_HAND_LEFT, 0.30f, 22000000);
        SDL_Log("VR: wheel descended to page %d", (int)vr.wheelPage);
    }
}

static void vrUpdateInput(XrTime time)
{
    XrActiveActionSet activeSet;
    XrActionsSyncInfo syncInfo;
    real32 pos[VR_HAND_COUNT][3], dir[VR_HAND_COUNT][3];
    real32 panelT[VR_HAND_COUNT] = {0.0f, 0.0f};
    sdword panelX[VR_HAND_COUNT] = {0, 0};
    sdword panelY[VR_HAND_COUNT] = {0, 0};
    bool32 tracked[VR_HAND_COUNT] = {FALSE, FALSE};
    bool32 grip[VR_HAND_COUNT];
    bool32 hoverChanged[VR_HAND_COUNT] = {FALSE, FALSE};
    bool32 panelHit[VR_HAND_COUNT] = {FALSE, FALSE};
    bool32 panelOwns[VR_HAND_COUNT] = {FALSE, FALSE};
    bool32 select[VR_HAND_COUNT], context[VR_HAND_COUNT], back[VR_HAND_COUNT];
    bool32 managerOpen, managerActive;
    uword const handOrder[VR_HAND_COUNT] = {VR_HAND_RIGHT, VR_HAND_LEFT};
    real32 deltaSeconds = 1.0f / 72.0f;
    uword hand, orderIndex;

    if (vr.state != XR_SESSION_STATE_FOCUSED)
    {
        return;
    }

    activeSet.actionSet = vr.actionSet;
    activeSet.subactionPath = XR_NULL_PATH;
    memset(&syncInfo, 0, sizeof(syncInfo));
    syncInfo.type = XR_TYPE_ACTIONS_SYNC_INFO;
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &activeSet;
    if (XR_FAILED(xrSyncActions(vr.session, &syncInfo)))
    {
        return;
    }
    if (vr.lastInputTime != 0 && time > vr.lastInputTime)
    {
        deltaSeconds = (real32)((double)(time - vr.lastInputTime) / 1000000000.0);
        if (deltaSeconds > 0.05f)
        {
            deltaSeconds = 0.05f;
        }
    }
    vr.lastInputTime = time;
    for (hand = 0; hand < VR_HAND_COUNT; hand++)
    {
        grip[hand] = vrActionPressedHand(vr.gripAction, hand);
        select[hand] = vrActionPressedHand(vr.selectAction, hand);
        context[hand] = vrActionPressedHand(vr.contextAction, hand);
        back[hand] = vrActionPressedHand(vr.backAction, hand);
    }
    /* A manager owns the pointer as soon as the game opens it, but it only
       takes input away from the world once its panel has actually reached
       the compositor. Otherwise a presentation failure locks the user out
       of every control they could use to recover. */
    managerOpen = vr.worldInteractive && vrWorldManagerActive();
    managerActive = managerOpen && vr.managerState == VR_MGR_VISIBLE;

    /* The command wheel owns the left hand while it is up. In menus the left
       trigger keeps its ordinary click, so this is in-game only.

       Deliberately available with a manager open: the wheel is the only
       VR-native route to the managers, and Homeworld's own taskbar cannot
       serve that purpose because opening a manager detaches the taskbar
       regions from the region tree (tbDisable) and modal front-end screens
       set RPE_ModalBreak, which stops events propagating to it. Without this
       there is no way to go from one manager to another. */
    if (vr.worldInteractive)
    {
        vrUpdateWheelInput(time, select[VR_HAND_LEFT]);
    }
    else if (vr.wheelOpen)
    {
        vrWheelClose(FALSE);
    }

    /* feed both aim rays into the world-interaction layer (in-game only;
       vrWorldFrameBegin gates on a live game world) */
    if (vr.worldInteractive)
    {
        memset(pos, 0, sizeof(pos));
        memset(dir, 0, sizeof(dir));
        for (hand = 0; hand < VR_HAND_COUNT; hand++)
        {
            tracked[hand] = vrHandAimLocal(hand, time, pos[hand], dir[hand]);
            hoverChanged[hand] =
                vrWorldSetRay((sdword)hand, pos[hand], dir[hand], tracked[hand]);
        }
        if (vr.frameCount % VR_DEBUG_INTERVAL == 1)
        {
            XrPosef const* aim = &vr.debugAimPose[VR_HAND_RIGHT];

            SDL_Log("VRDBG AIM frame=%u R tracked=%d pos=(%.4f %.4f %.4f) "
                    "q=(%.4f %.4f %.4f %.4f) minusZ=(%.4f %.4f %.4f)",
                    (unsigned)vr.frameCount, (int)tracked[VR_HAND_RIGHT],
                    pos[VR_HAND_RIGHT][0], pos[VR_HAND_RIGHT][1], pos[VR_HAND_RIGHT][2],
                    aim->orientation.x, aim->orientation.y,
                    aim->orientation.z, aim->orientation.w,
                    dir[VR_HAND_RIGHT][0], dir[VR_HAND_RIGHT][1],
                    dir[VR_HAND_RIGHT][2]);
        }
        /* grab-the-space gestures. Two grips: pinch zoom + pair rotation.
           One grip: 1:1 pan (your hand drags the hologram). */
        if (!managerActive
            && grip[VR_HAND_LEFT] && grip[VR_HAND_RIGHT]
            && tracked[VR_HAND_LEFT] && tracked[VR_HAND_RIGHT])
        {
            real32 dx = pos[VR_HAND_RIGHT][0] - pos[VR_HAND_LEFT][0];
            real32 dy = pos[VR_HAND_RIGHT][1] - pos[VR_HAND_LEFT][1];
            real32 dz = pos[VR_HAND_RIGHT][2] - pos[VR_HAND_LEFT][2];
            real32 dist = sqrtf(dx * dx + dy * dy + dz * dz);
            real32 azimuth = atan2f(dz, dx);
            real32 elev = (dist > 1e-4f) ? asinf(dy / dist) : 0.0f;

            if (vr.pinchValid && dist > 0.05f && vr.pinchPrevDist > 0.05f)
            {
                real32 dAz = azimuth - vr.pinchPrevAzimuth;
                real32 dElev = elev - vr.pinchPrevElev;

                if (dAz > 3.14159f)  dAz -= 6.28318f;
                if (dAz < -3.14159f) dAz += 6.28318f;
                if (vr.frameCount % VR_DEBUG_INTERVAL == 1)
                {
                    SDL_Log("VRDBG INPUT frame=%u source=pinch grips=1/1 "
                            "distance=%.4f zoom=%.6f orbit=(%.6f %.6f)",
                            (unsigned)vr.frameCount, dist,
                            vr.pinchPrevDist / dist, dAz, dElev);
                }
                vrWorldCameraZoom(vr.pinchPrevDist / dist);
                vrWorldCameraOrbit(dAz, dElev);
            }
            vr.pinchPrevDist = dist;
            vr.pinchPrevAzimuth = azimuth;
            vr.pinchPrevElev = elev;
            vr.pinchValid = TRUE;
        }
        else
        {
            /* Traversal follows Homeworld's own mechanic: select a ship and
               focus on it (left stick click). Single-grip drags do nothing;
               two-grip pinch handles orbit/zoom above. */
            vr.pinchValid = FALSE;
        }
    }

    if (!vr.worldInteractive)
    {
        /* menu mode: left stick synthesizes Homeworld's right-button drag */
        real32 lx, ly;

        vrActionStick(VR_HAND_LEFT, &lx, &ly);
        if (lx * lx + ly * ly > 0.04f)
        {
            if (!vr.stickRotating)
            {
                vrPushMouseButton(SDL_BUTTON_RIGHT, TRUE);
                vr.stickRotating = TRUE;
            }
            vr.pointerX += (sdword)(lx * 22.0f);
            vr.pointerY -= (sdword)(ly * 22.0f);
            if (vr.pointerX < 0) vr.pointerX = 0;
            if (vr.pointerY < 0) vr.pointerY = 0;
            if (vr.pointerX >= vr.width)  vr.pointerX = vr.width - 1;
            if (vr.pointerY >= vr.height) vr.pointerY = vr.height - 1;
            SDL_WarpMouseInWindow(sdlwindow, (int)vr.pointerX, (int)vr.pointerY);
        }
        else if (vr.stickRotating)
        {
            vrPushMouseButton(SDL_BUTTON_RIGHT, FALSE);
            vr.stickRotating = FALSE;
        }
    }
    else
    {
        /* in-game: left stick orbits, right stick Y zooms, and right grip
           turns the right stick into a fleet traversal control - flick it
           left/right to step the camera through the fleet. Cycling used to
           sit on a bare flick, which fired by accident while zooming. */
        real32 lx, ly, rx, ry;
        sdword cycleDir;
        bool32 traversing;

        vrActionStick(VR_HAND_LEFT, &lx, &ly);
        vrActionStick(VR_HAND_RIGHT, &rx, &ry);
        if (vr.frameCount % VR_DEBUG_INTERVAL == 1)
        {
            SDL_Log("VRDBG INPUT frame=%u source=sticks L=(%.5f %.5f) "
                    "R=(%.5f %.5f) deadzoneL=%d",
                    (unsigned)vr.frameCount, lx, ly, rx, ry,
                    (int)(lx * lx + ly * ly > 0.04f));
        }
        traversing = grip[VR_HAND_RIGHT] && !managerActive;

        /* The Sensors Manager is a 3D strategic map, not a flat screen, and
           it is where long-range moves and hyperspace are issued - so it has
           to be navigable. Its own camera takes the sticks while it is open;
           without this the sticks were simply suppressed along with every
           other manager's, leaving a strategic view that could be pointed at
           but never moved. Selection there already works, since dragging the
           ray band-boxes exactly as dragging a mouse does. */
        if (vrWorldSensorsActive() && !vr.wheelOpen)
        {
            if (lx * lx + ly * ly > 0.04f)
            {
                vrWorldSensorsOrbit(-lx * 0.030f, ly * 0.022f);
            }
            if (ry > 0.25f || ry < -0.25f)
            {
                vrWorldSensorsZoom(1.0f - ry * 0.030f);
            }
        }
        else if (!managerActive && !vr.wheelOpen && lx * lx + ly * ly > 0.04f)
        {
            vrWorldCameraOrbit(-lx * 0.035f, ly * 0.025f);
        }
        /* while the grip claims the right stick, it steers the fleet and
           nothing else: a diagonal flick must not also drive the zoom */
        if (!managerActive && !traversing && !vrWorldSensorsActive()
            && (ry > 0.25f || ry < -0.25f)
            && !vrWorldMoveActive())
        {
            vrWorldCameraZoom(1.0f - ry * 0.02f);
        }
        cycleDir = traversing
                 ? ((rx > 0.7f) ? 1 : (rx < -0.7f) ? -1 : 0) : 0;
        if (cycleDir != 0 && vr.prevCycleDir == 0)
        {
            vrWorldFocusCycle(cycleDir);
            SDL_Log("VR: fleet cycle %+d (right grip + right stick)",
                    (int)cycleDir);
        }
        vr.prevCycleDir = cycleDir;
        if (vr.stickRotating)
        {
            vrPushMouseButton(SDL_BUTTON_RIGHT, FALSE);
            vr.stickRotating = FALSE;
        }
    }

    if (!vr.worldInteractive)
    {
        /* zoom: right stick Y accumulates into mouse wheel ticks */
        real32 rx, ry;

        vrActionStick(VR_HAND_RIGHT, &rx, &ry);
        if (ry > 0.25f || ry < -0.25f)
        {
            vr.wheelAccum += ry * 0.12f;
        }
        while (vr.wheelAccum >= 1.0f || vr.wheelAccum <= -1.0f)
        {
            SDL_Event event;
            sdword step = (vr.wheelAccum > 0.0f) ? 1 : -1;

            memset(&event, 0, sizeof(event));
            event.type = SDL_MOUSEWHEEL;
            event.wheel.windowID = SDL_GetWindowID(sdlwindow);
            event.wheel.y = step;
            SDL_PushEvent(&event);
            vr.wheelAccum -= (real32)step;
        }
    }

    /* Resolve each hand against both surfaces. The nearest physical hit owns
       input; manager screens explicitly take priority. A gesture that began
       on the panel remains locked to that hand until its button is released. */
    vr.pointerValid = FALSE;
    vr.pointerHand = -1;
    if (!vr.stickRotating)
    {
        bool32 panelAvailable = !vr.worldInteractive || !vr.panelHidden || managerOpen;
        sdword lockedPanelHand = -1;
        sdword pointerCandidate = -1;

        for (hand = 0; hand < VR_HAND_COUNT; hand++)
        {
            real32 worldT;

            if (!panelAvailable)
            {
                continue;
            }
            panelHit[hand] = vrPointerFromHand(hand, time, &panelX[hand],
                                               &panelY[hand], &panelT[hand]);
            if (!panelHit[hand])
            {
                continue;
            }
            worldT = vr.worldInteractive ? vrWorldHandHitDistance((sdword)hand) : -1.0f;
            panelOwns[hand] = !vr.worldInteractive || managerOpen || worldT < 0.0f
                           || panelT[hand] <= worldT;
            if (panelOwns[hand] && vr.worldInteractive)
            {
                vrWorldSetRayLimit((sdword)hand, panelT[hand]);
            }
        }

        if (vr.selectGestureMode == VR_SELECT_PANEL)
        {
            lockedPanelHand = vr.selectGestureHand;
        }
        else if (vr.contextGestureMode == VR_CONTEXT_PANEL)
        {
            lockedPanelHand = vr.contextGestureHand;
        }
        if (lockedPanelHand >= 0 && panelHit[lockedPanelHand])
        {
            pointerCandidate = lockedPanelHand;
        }
        else if (panelOwns[VR_HAND_RIGHT])
        {
            pointerCandidate = VR_HAND_RIGHT;
        }
        else if (panelOwns[VR_HAND_LEFT])
        {
            pointerCandidate = VR_HAND_LEFT;
        }

        if (pointerCandidate >= 0)
        {
            vr.pointerX = panelX[pointerCandidate];
            vr.pointerY = panelY[pointerCandidate];
            vr.panelHitT = panelT[pointerCandidate];
            vr.pointerHand = pointerCandidate;
            vr.pointerValid = TRUE;
            SDL_WarpMouseInWindow(sdlwindow, vr.pointerX, vr.pointerY);
        }
    }
    if (vr.worldInteractive)
    {
        for (hand = 0; hand < VR_HAND_COUNT; hand++)
        {
            if (!managerActive && hoverChanged[hand] && !panelOwns[hand])
            {
                vrHapticPulse(hand, 0.16f, 12000000);
            }
        }
    }

    /* Trigger selection is captured by the hand and surface on press. This
       prevents the other controller, or a ray crossing the wrist panel,
       from stealing the release. */
    for (orderIndex = 0; orderIndex < VR_HAND_COUNT; orderIndex++)
    {
        hand = handOrder[orderIndex];
        if (hand == VR_HAND_LEFT && vr.worldInteractive)
        {
            vr.prevSelect[hand] = select[hand];             //wheel owns it
            continue;
        }
        if (select[hand] && !vr.prevSelect[hand])
        {
            /* The trigger is otherwise unbound during a move preview, so it
               is what draws a freehand flight path: sweep with both held and
               the swept curve becomes the waypoints. */
            if (vr.contextGestureMode == VR_CONTEXT_MOVE
                && vr.contextGestureHand == (sdword)hand
                && vrWorldMoveActive())
            {
                vrWorldPathBegin((sdword)hand);
                vr.selectGestureHand = hand;
                vr.selectGestureMode = VR_SELECT_PATH;
                vrHapticPulse(hand, 0.30f, 22000000);
            }
            /* Same grammar one step across: A previews an order, and sweeping
               the trigger during it elaborates that order. During a move it
               draws a route; during an attack it collects more targets, which
               all go out as one clWrapAttack when A is released. */
            else if (vr.contextGestureMode == VR_CONTEXT_ORDER
                     && vr.contextGestureHand == (sdword)hand
                     && vrWorldContextIntent((sdword)hand) == VRW_INTENT_ATTACK)
            {
                vrWorldTargetSweepBegin((sdword)hand);
                vr.selectGestureHand = hand;
                vr.selectGestureMode = VR_SELECT_TARGETS;
                vrHapticPulse(hand, 0.30f, 22000000);
            }
            else if (vr.selectGestureHand < 0 && vr.contextGestureHand < 0)
            {
                if (panelOwns[hand])
                {
                    vr.pointerX = panelX[hand];
                    vr.pointerY = panelY[hand];
                    vr.panelHitT = panelT[hand];
                    vr.pointerHand = hand;
                    vr.pointerValid = TRUE;
                    SDL_WarpMouseInWindow(sdlwindow, vr.pointerX, vr.pointerY);
                    vrPushMouseButton(SDL_BUTTON_LEFT, TRUE);
                    vr.selectGestureHand = hand;
                    vr.selectGestureMode = VR_SELECT_PANEL;
                }
                else if (vr.worldInteractive && !managerActive)
                {
                    bool32 changed = FALSE;

                    vr.selectAdditive = grip[VR_HAND_LEFT];
                    vr.selectGestureHand = hand;
                    /* Homeworld's left button both selects and issues the
                       default order, told apart by what is under it: one of
                       your own ships selects, a hostile or a resource orders
                       the current selection to act on it (mrObjectClick).
                       Mirror that - point at a rock with harvesters up and
                       pull, point at a derelict with a Salvage Corvette and
                       pull. The trigger is the fast path; A/X stays for orders
                       that need a previewed position in 3D. */
                    if (vrWorldHandHasTarget((sdword)hand)
                        && !vrWorldHandHasSelectable((sdword)hand)
                        && selSelected.numShips > 0)
                    {
                        vrworldintent intent = vrWorldContextIntent((sdword)hand);
                        bool32 issued = vrWorldContextOrder((sdword)hand);

                        vr.selectGestureMode = VR_SELECT_CLICK;
                        vr.lastSelectTime[hand] = 0;
                        vrHapticPulse(hand, issued ? 0.46f : 0.12f,
                                      issued ? 38000000 : 16000000);
                        SDL_Log("VR: hand %u trigger order intent=%d issued=%d",
                                (unsigned)hand, (int)intent, (int)issued);
                    }
                    else if (vrWorldHandHasTarget((sdword)hand))
                    {
                        bool32 doubleTrigger =
                            vrWorldHandHasSelectable((sdword)hand)
                            && vr.lastSelectTime[hand] != 0
                            && time > vr.lastSelectTime[hand]
                            && time - vr.lastSelectTime[hand] <= VR_DOUBLE_TRIGGER_NS;

                        if (doubleTrigger)
                        {
                            changed = vrWorldSelectType((sdword)hand, vr.selectAdditive);
                            vr.lastSelectTime[hand] = 0;
                            SDL_Log("VR: hand %u double-trigger type select additive=%d changed=%d",
                                    (unsigned)hand, (int)vr.selectAdditive, (int)changed);
                        }
                        else
                        {
                            changed = vrWorldSelectClick((sdword)hand, vr.selectAdditive);
                            vr.lastSelectTime[hand] =
                                vrWorldHandHasSelectable((sdword)hand) ? time : 0;
                            SDL_Log("VR: hand %u trigger select additive=%d changed=%d",
                                    (unsigned)hand, (int)vr.selectAdditive, (int)changed);
                        }
                        vr.selectGestureMode = VR_SELECT_CLICK;
                    }
                    else
                    {
                        vr.lastSelectTime[hand] = 0;
                        vrWorldSweepBegin((sdword)hand);
                        vr.selectGestureMode = VR_SELECT_SWEEP;
                        SDL_Log("VR: hand %u sweep selection begin additive=%d",
                                (unsigned)hand, (int)vr.selectAdditive);
                    }
                    if (changed)
                    {
                        vrHapticPulse(hand, 0.38f, 30000000);
                    }
                }
            }
        }
        else if (!select[hand] && vr.prevSelect[hand]
                 && vr.selectGestureHand == (sdword)hand)
        {
            bool32 changed = FALSE;

            if (vr.selectGestureMode == VR_SELECT_PANEL)
            {
                vrPushMouseButton(SDL_BUTTON_LEFT, FALSE);
            }
            else if (vr.selectGestureMode == VR_SELECT_SWEEP)
            {
                changed = vrWorldSweepCommit((sdword)hand, vr.selectAdditive);
                SDL_Log("VR: hand %u sweep selection commit additive=%d changed=%d",
                        (unsigned)hand, (int)vr.selectAdditive, (int)changed);
            }
            else if (vr.selectGestureMode == VR_SELECT_PATH)
            {
                changed = vrWorldPathFinishStroke();
                SDL_Log("VR: hand %u path stroke committed=%d points=%d",
                        (unsigned)hand, (int)changed,
                        (int)vrWorldPathPointCount());
            }
            else if (vr.selectGestureMode == VR_SELECT_TARGETS)
            {
                /* keep the list; A/X release is what issues the order */
                SDL_Log("VR: hand %u target sweep holding %d target(s)",
                        (unsigned)hand, (int)vrWorldTargetSweepCount());
            }
            if (changed)
            {
                vrHapticPulse(hand, 0.42f, 35000000);
            }
            vr.selectGestureHand = -1;
            vr.selectGestureMode = VR_GESTURE_NONE;
        }
        vr.prevSelect[hand] = select[hand];
    }

    /* A/X previews a native smart order while held and commits it on
       release. Empty space starts a move preview; an invalid target never
       falls through into an accidental move. */
    for (orderIndex = 0; orderIndex < VR_HAND_COUNT; orderIndex++)
    {
        hand = handOrder[orderIndex];
        if (context[hand] && !vr.prevContext[hand])
        {
            if (vr.contextGestureHand < 0 && vr.selectGestureHand < 0)
            {
                if (hand == VR_HAND_LEFT && vr.worldInteractive && !managerActive)
                {
                    /* Left hand is the commander's: X is undo, not a mirror of
                       the right hand's order button. The old right-grip+A/X
                       chord that pushed the Dock key is gone - the smart order
                       already docks when aimed at a friendly ship, so it was a
                       third meaning on the right grip for nothing. */
                    bool32 undone = vrWorldCommand(VRW_CMD_UNDO, 0);

                    vrHapticPulse(hand, undone ? 0.34f : 0.12f,
                                  undone ? 30000000 : 16000000);
                    SDL_Log("VR: undo via left X, applied=%d", (int)undone);
                }
                else if (panelOwns[hand])
                {
                    vr.pointerX = panelX[hand];
                    vr.pointerY = panelY[hand];
                    vr.panelHitT = panelT[hand];
                    vr.pointerHand = hand;
                    vr.pointerValid = TRUE;
                    SDL_WarpMouseInWindow(sdlwindow, vr.pointerX, vr.pointerY);
                    vrPushMouseButton(SDL_BUTTON_RIGHT, TRUE);
                    vr.contextGestureHand = hand;
                    vr.contextGestureMode = VR_CONTEXT_PANEL;
                }
                else if (vr.worldInteractive && !managerActive)
                {
                    vrworldintent intent = vrWorldContextIntent((sdword)hand);

                    vr.contextGestureHand = hand;
                    if (vrWorldHandHasTarget((sdword)hand))
                    {
                        vr.contextGestureMode = VR_CONTEXT_ORDER;
                        vrHapticPulse(hand,
                                      intent == VRW_INTENT_INVALID ? 0.10f : 0.26f,
                                      intent == VRW_INTENT_INVALID ? 18000000 : 28000000);
                        SDL_Log("VR: hand %u smart-order preview intent=%d",
                                (unsigned)hand, (int)intent);
                    }
                    else if (intent == VRW_INTENT_MOVE
                             && vrWorldMoveBegin((sdword)hand))
                    {
                        vr.contextGestureMode = VR_CONTEXT_MOVE;
                        vr.moveDepthInput = 0.0f;
                        vr.moveDepthHeld = 0.0f;
            vr.moveDepthHeld = 0.0f;
                        vrHapticPulse(hand, 0.24f, 26000000);
                        SDL_Log("VR: hand %u move preview begin", (unsigned)hand);
                    }
                    else
                    {
                        vr.contextGestureHand = -1;
                    }
                }
            }
        }
        else if (!context[hand] && vr.prevContext[hand]
                 && vr.contextGestureHand == (sdword)hand)
        {
            bool32 issued = FALSE;

            if (vr.contextGestureMode == VR_CONTEXT_PANEL)
            {
                vrPushMouseButton(SDL_BUTTON_RIGHT, FALSE);
            }
            else if (vr.contextGestureMode == VR_CONTEXT_ORDER)
            {
                if (vrWorldTargetSweepCount() > 0)
                {
                    sdword targets = vrWorldTargetSweepCount();

                    issued = vrWorldTargetSweepCommit();
                    SDL_Log("VR: hand %u multi-attack commit issued=%d targets=%d",
                            (unsigned)hand, (int)issued, (int)targets);
                }
                else
                {
                    vrWorldTargetSweepCancel();
                    issued = vrWorldContextOrder((sdword)hand);
                    SDL_Log("VR: hand %u smart-order commit issued=%d",
                            (unsigned)hand, (int)issued);
                }
                if (vr.selectGestureMode == VR_SELECT_TARGETS)
                {
                    vr.selectGestureHand = -1;
                    vr.selectGestureMode = VR_GESTURE_NONE;
                }
            }
            else if (vr.contextGestureMode == VR_CONTEXT_MOVE)
            {
                /* A drawn path replaces the single destination: finish an
                   in-progress stroke, then fly whatever curve was laid down. */
                if (vrWorldPathDrawing())
                {
                    vrWorldPathFinishStroke();
                }
                if (vrWorldPathPointCount() > 0)
                {
                    vrWorldMoveCancel();
                    issued = vrWorldPathCommit();
                    SDL_Log("VR: hand %u path commit issued=%d legs=%d",
                            (unsigned)hand, (int)issued,
                            (int)vrWorldPathPointCount());
                }
                else
                {
                    issued = vrWorldMoveCommit();
                    SDL_Log("VR: hand %u move commit issued=%d depth=%.0f",
                            (unsigned)hand, (int)issued, vrWorldCursorDist());
                }
                if (vr.selectGestureMode == VR_SELECT_PATH)
                {
                    vr.selectGestureHand = -1;
                    vr.selectGestureMode = VR_GESTURE_NONE;
                }
            }
            if (issued)
            {
                vrHapticPulse(hand, 0.52f, 45000000);
            }
            vr.contextGestureHand = -1;
            vr.contextGestureMode = VR_GESTURE_NONE;
            vr.moveDepthInput = 0.0f;
            vr.moveDepthHeld = 0.0f;
        }
        vr.prevContext[hand] = context[hand];
    }

    if (vr.contextGestureMode == VR_CONTEXT_MOVE
        && vr.contextGestureHand >= 0 && vrWorldMoveActive())
    {
        real32 rx, ry;

        /* Raw deflection, deadzoned. vrworld scales the cursor's distance by
           it, so no accumulator is needed here and the depth cannot drift. */
        vrActionStick(VR_HAND_RIGHT, &rx, &ry);
        vr.moveDepthInput = 0.0f;
        if (fabsf(ry) > VR_MOVE_DEPTH_DEADZONE)
        {
            vr.moveDepthInput = (fabsf(ry) - VR_MOVE_DEPTH_DEADZONE)
                              / (1.0f - VR_MOVE_DEPTH_DEADZONE);
            if (ry < 0.0f)
            {
                vr.moveDepthInput = -vr.moveDepthInput;
            }
        }
        /* Time-based so depth speed does not follow the frame rate, and
           accelerating while held: the useful depth range spans two orders of
           magnitude, so a single rate is either too coarse to aim with or too
           slow to cross it. */
        if (vr.moveDepthInput != 0.0f)
        {
            vr.moveDepthHeld += deltaSeconds;
        }
        else
        {
            vr.moveDepthHeld = 0.0f;
        }
        {
            real32 ramp = 1.0f + vr.moveDepthHeld * VR_MOVE_DEPTH_ACCEL;

            if (ramp > VR_MOVE_DEPTH_ACCEL_MAX)
            {
                ramp = VR_MOVE_DEPTH_ACCEL_MAX;
            }
            vrWorldMoveUpdate(vr.contextGestureHand, vr.moveDepthInput
                              * VR_MOVE_DEPTH_RATE * ramp * deltaSeconds);
        }
        /* after the destination has been recomputed for this frame, so the
           stroke records the depth the stick is currently dialling in */
        vrWorldPathSample(vr.contextGestureHand);
    }

    /* B/Y cancels a move first, then closes any open manager - that path is
       unconditional, so a manager is always escapable no matter what its
       panel is doing. Otherwise left-grip+B/Y toggles the wrist panel,
       right-grip+B/Y opens Build, and an unmodified press is Escape. */
    for (orderIndex = 0; orderIndex < VR_HAND_COUNT; orderIndex++)
    {
        hand = handOrder[orderIndex];
        if (back[hand] && !vr.prevBack[hand])
        {
            SDL_Log("VR: hand %u B/Y press grips=%d/%d move=%d manager=%s state=%d",
                    (unsigned)hand, (int)grip[VR_HAND_LEFT],
                    (int)grip[VR_HAND_RIGHT], (int)vrWorldMoveActive(),
                    vrWorldManagerName(), (int)vr.managerState);
            if (vr.wheelOpen)
            {
                if (vr.wheelPage != VR_WHEEL_PAGE_ROOT)
                {
                    vr.wheelPage = VR_WHEEL_PAGE_ROOT;      //back up a level
                    vr.wheelSlot = -1;
                    vr.wheelSlotSince = time;
                    vr.wheelSubOpened = FALSE;
                    SDL_Log("VR: wheel back to root");
                }
                else
                {
                    vrWheelClose(FALSE);
                }
                vrHapticPulse(hand, 0.22f, 18000000);
            }
            else if (vrWorldMoveActive() || vrWorldPathDrawing()
                || vrWorldPathActive() || vrWorldTargetSweepCount() > 0)
            {
                vrWorldMoveCancel();
                vrWorldPathCancel();
                vrWorldTargetSweepCancel();
                if (vr.contextGestureMode == VR_CONTEXT_MOVE)
                {
                    vr.contextGestureHand = -1;
                    vr.contextGestureMode = VR_GESTURE_NONE;
                    vr.moveDepthInput = 0.0f;
                }
                if (vr.selectGestureMode == VR_SELECT_PATH
                    || vr.selectGestureMode == VR_SELECT_TARGETS)
                {
                    vr.selectGestureHand = -1;
                    vr.selectGestureMode = VR_GESTURE_NONE;
                }
                vrHapticPulse(hand, 0.30f, 30000000);
                SDL_Log("VR: order preview cancelled");
            }
            else if (vrWorldManagerActive())
            {
                bool32 closed = vrWorldCloseManagers();

                SDL_Log("VR: manager close requested, remaining=%s closed=%d",
                        vrWorldManagerName(), (int)closed);
                if (!closed)
                {                                           //trader GUI: Escape is its exit
                    vr.backKey[hand] = SDLK_ESCAPE;
                    vrPushKey(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, TRUE);
                }
                else
                {
                    /* hand the quad straight back to the wrist so the panel
                       does not linger a frame at manager size and distance */
                    vrUpdateScreenPose(time);
                }
                vrHapticPulse(hand, 0.38f, 35000000);
            }
            else if (hand == VR_HAND_LEFT)
            {
                /* Y on the commander's hand shows and hides the wrist panel
                   and its cards. This used to need left-grip+B/Y, and Build
                   used to need right-grip+B/Y - both chords are gone, so the
                   right grip now means one thing only: navigate. Build lives
                   on the wheel, which reaches every manager. */
                vr.panelHidden = !vr.panelHidden;
                vrHapticPulse(hand, 0.25f, 25000000);
                SDL_Log("VR: wrist panel %s", vr.panelHidden ? "hidden" : "shown");
            }
            else
            {
                vr.backKey[hand] = SDLK_ESCAPE;
                SDL_Log("VR: hand %u dispatching Escape", (unsigned)hand);
                vrPushKey(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, TRUE);
            }
        }
        else if (!back[hand] && vr.prevBack[hand] && vr.backKey[hand] != 0)
        {
            vrPushKey(vr.backKey[hand], SDL_SCANCODE_ESCAPE, FALSE);
            vr.backKey[hand] = 0;
        }
        vr.prevBack[hand] = back[hand];
    }

    /* Ray color communicates what the next trigger/A-X action will do. */
    if (vr.worldInteractive)
    {
        for (hand = 0; hand < VR_HAND_COUNT; hand++)
        {
            vrworldintent intent = VRW_INTENT_IDLE;

            if (panelOwns[hand]
                || (vr.selectGestureHand == (sdword)hand
                    && vr.selectGestureMode == VR_SELECT_PANEL)
                || (vr.contextGestureHand == (sdword)hand
                    && vr.contextGestureMode == VR_CONTEXT_PANEL))
            {
                intent = VRW_INTENT_PANEL;
            }
            else if (managerActive)
            {
                intent = VRW_INTENT_INVALID;
            }
            else if (vr.selectGestureHand == (sdword)hand)
            {
                intent = vr.selectAdditive ? VRW_INTENT_ADD_SELECT : VRW_INTENT_SELECT;
            }
            else if (vr.contextGestureHand == (sdword)hand)
            {
                if (vr.contextGestureMode == VR_CONTEXT_MOVE)
                {
                    intent = VRW_INTENT_MOVE;
                }
                else
                {
                    intent = vrWorldContextIntent((sdword)hand);
                }
            }
            else if (vrWorldHandHasSelectable((sdword)hand))
            {
                intent = grip[VR_HAND_LEFT] ? VRW_INTENT_ADD_SELECT : VRW_INTENT_SELECT;
            }
            else if (vrWorldHandHasTarget((sdword)hand))
            {
                intent = vrWorldContextIntent((sdword)hand);
            }
            vrWorldSetIntent((sdword)hand, intent);
        }
    }

    /* left grip: Shift for world additive selection, never leak a modifier
       into a modal manager panel. */
    {
        bool32 gripLeft = !managerActive
                        && vrActionPressedHand(vr.gripAction, VR_HAND_LEFT);

        if (gripLeft != vr.prevGripLeft)
        {
            vrPushKey(SDLK_LSHIFT, SDL_SCANCODE_LSHIFT, gripLeft);
            vr.prevGripLeft = gripLeft;
        }
    }

    /* In-game stick clicks focus the selection / toggle Sensors. Suppress
       both while a manager owns interaction. */
    {
        uword hand;

        for (hand = 0; hand < VR_HAND_COUNT; hand++)
        {
            bool32 pressed = vrActionPressedHand(vr.stickClickAction, hand);

            /* Right stick click toggles Sensors, so the same control both
               enters and leaves it. This has to run before the manager
               suppression below, or the only way out of Sensors would be a
               different control from the way in. */
            if (hand == VR_HAND_RIGHT && vr.worldInteractive
                && pressed != vr.prevStickClick[hand])
            {
                vr.prevStickClick[hand] = pressed;
                if (pressed)
                {
                    bool32 open = vrWorldToggleSensors();

                    vrHapticPulse(hand, 0.34f, 30000000);
                    SDL_Log("VR: sensors toggled -> %s", open ? "open" : "closed");
                }
                continue;
            }

            if (managerActive)
            {
                if (vr.stickClickKey[hand] != 0)
                {
                    vrPushKey(vr.stickClickKey[hand],
                              (vr.stickClickKey[hand] == SDLK_r) ? SDL_SCANCODE_R :
                              (vr.stickClickKey[hand] == SDLK_m) ? SDL_SCANCODE_M :
                                                                  SDL_SCANCODE_SPACE,
                              FALSE);
                    vr.stickClickKey[hand] = 0;
                }
                vr.prevStickClick[hand] = pressed;
                continue;
            }
            if (pressed == vr.prevStickClick[hand])
            {
                continue;
            }
            if (pressed)
            {
                if (hand == VR_HAND_LEFT)
                {
                    if (vr.worldInteractive)
                    {
                        /* F equivalent: focus the camera on the selection,
                           and bring the hologram back home */
                        /* One intent, "bring me back to my fleet": focus the
                           game camera on the selection and put the hologram
                           back in front of the player. Zeroing worldOffset
                           was the old behaviour and did the opposite of
                           useful - it snapped back to the anchor captured on
                           the main menu. */
                        vrWorldCameraFocusSelection();
                        vrRecentreHologram(time);
                        vr.prevStickClick[hand] = pressed;
                        vr.stickClickKey[hand] = 0;
                        continue;
                    }
                    vr.stickClickKey[hand] =
                        vrActionPressedHand(vr.gripAction, VR_HAND_RIGHT) ? SDLK_r : SDLK_m;
                }
                else
                {
                    vr.stickClickKey[hand] = SDLK_SPACE;
                }
                vrPushKey(vr.stickClickKey[hand],
                          (vr.stickClickKey[hand] == SDLK_r) ? SDL_SCANCODE_R :
                          (vr.stickClickKey[hand] == SDLK_m) ? SDL_SCANCODE_M : SDL_SCANCODE_SPACE,
                          TRUE);
            }
            else if (vr.stickClickKey[hand] != 0)
            {
                vrPushKey(vr.stickClickKey[hand],
                          (vr.stickClickKey[hand] == SDLK_r) ? SDL_SCANCODE_R :
                          (vr.stickClickKey[hand] == SDLK_m) ? SDL_SCANCODE_M : SDL_SCANCODE_SPACE,
                          FALSE);
            }
            vr.prevStickClick[hand] = pressed;
        }
    }
}

/* OpenXR can stop delivering action updates while the app is unfocused.
   Release every synthesized hold and cancel previews at that boundary so
   returning from the system UI cannot leave a mouse button or key stuck. */
static void vrReleaseInputCapture(void)
{
    uword hand;

    if (vr.selectGestureMode == VR_SELECT_PANEL)
    {
        vrPushMouseButton(SDL_BUTTON_LEFT, FALSE);
    }
    else if (vr.selectGestureMode == VR_SELECT_SWEEP)
    {
        vrWorldSweepCancel();
    }
    else if (vr.selectGestureMode == VR_SELECT_PATH)
    {
        /* an unfinished stroke is meaningless; a committed path keeps flying */
        if (vrWorldPathDrawing())
        {
            vrWorldPathCancel();
        }
    }
    else if (vr.selectGestureMode == VR_SELECT_TARGETS)
    {
        vrWorldTargetSweepCancel();
    }
    if (vr.contextGestureMode == VR_CONTEXT_PANEL)
    {
        vrPushMouseButton(SDL_BUTTON_RIGHT, FALSE);
    }
    if (vrWorldMoveActive())
    {
        vrWorldMoveCancel();
    }
    if (vr.stickRotating)
    {
        vrPushMouseButton(SDL_BUTTON_RIGHT, FALSE);
    }
    if (vr.prevGripLeft)
    {
        vrPushKey(SDLK_LSHIFT, SDL_SCANCODE_LSHIFT, FALSE);
    }
    for (hand = 0; hand < VR_HAND_COUNT; hand++)
    {
        if (vr.backKey[hand] != 0)
        {
            vrPushKey(vr.backKey[hand], SDL_SCANCODE_ESCAPE, FALSE);
        }
        if (vr.stickClickKey[hand] != 0)
        {
            vrPushKey(vr.stickClickKey[hand],
                      vr.stickClickKey[hand] == SDLK_r ? SDL_SCANCODE_R :
                      vr.stickClickKey[hand] == SDLK_m ? SDL_SCANCODE_M :
                                                        SDL_SCANCODE_SPACE,
                      FALSE);
        }
    }

    if (vr.wheelOpen)
    {
        vrWheelClose(FALSE);
    }

    memset(vr.prevSelect, 0, sizeof(vr.prevSelect));
    memset(vr.prevContext, 0, sizeof(vr.prevContext));
    memset(vr.prevBack, 0, sizeof(vr.prevBack));
    memset(vr.prevStickClick, 0, sizeof(vr.prevStickClick));
    memset(vr.backKey, 0, sizeof(vr.backKey));
    memset(vr.stickClickKey, 0, sizeof(vr.stickClickKey));
    vr.prevGripLeft = FALSE;
    vr.selectGestureHand = -1;
    vr.selectGestureMode = VR_GESTURE_NONE;
    vr.contextGestureHand = -1;
    vr.contextGestureMode = VR_GESTURE_NONE;
    vr.stickRotating = FALSE;
    vr.pointerValid = FALSE;
    vr.pointerHand = -1;
    vr.moveDepthInput = 0.0f;
    vr.moveDepthHeld = 0.0f;
    vr.lastInputTime = 0;
}

/*-----------------------------------------------------------------------------
    Stereo world rendering: the game's world is drawn once per eye with the
    headset's tracked pose layered on top of the game camera, and submitted
    as an OpenXR projection layer behind the UI quad.
----------------------------------------------------------------------------*/

/* q as a 3x3 rotation, R[row][col] flattened row-major */
static void vrQuatToMat3(XrQuaternionf q, real32 R[9])
{
    R[0] = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    R[1] = 2.0f * (q.x * q.y - q.z * q.w);
    R[2] = 2.0f * (q.x * q.z + q.y * q.w);
    R[3] = 2.0f * (q.x * q.y + q.z * q.w);
    R[4] = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
    R[5] = 2.0f * (q.y * q.z - q.x * q.w);
    R[6] = 2.0f * (q.x * q.z - q.y * q.w);
    R[7] = 2.0f * (q.y * q.z + q.x * q.w);
    R[8] = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
}

/* Column-major model matrix of a pose, translation scaled into game units */
static void vrPoseToModelMatrix(XrPosef pose, real32 scale, real32 out[16])
{
    real32 R[9];
    sdword r, c;

    vrQuatToMat3(pose.orientation, R);
    for (c = 0; c < 3; c++)
    {
        for (r = 0; r < 3; r++)
        {
            out[c * 4 + r] = R[r * 3 + c];
        }
        out[c * 4 + 3] = 0.0f;
    }
    out[12] = pose.position.x * scale;
    out[13] = pose.position.y * scale;
    out[14] = pose.position.z * scale;
    out[15] = 1.0f;
}

/* Column-major inverse (view) matrix of a rigid pose */
static void vrPoseToViewMatrix(XrPosef pose, real32 scale, real32 out[16])
{
    real32 R[9], t[3];
    sdword r, c;

    vrQuatToMat3(pose.orientation, R);
    t[0] = pose.position.x * scale;
    t[1] = pose.position.y * scale;
    t[2] = pose.position.z * scale;
    for (c = 0; c < 3; c++)
    {
        for (r = 0; r < 3; r++)
        {
            out[c * 4 + r] = R[c * 3 + r];                  /* R transposed */
        }
        out[c * 4 + 3] = 0.0f;
    }
    for (r = 0; r < 3; r++)
    {
        out[12 + r] = -(R[0 * 3 + r] * t[0] + R[1 * 3 + r] * t[1] + R[2 * 3 + r] * t[2]);
    }
    out[15] = 1.0f;
}

/* C = A * B, all column-major */
static void vrMatMul(real32 const A[16], real32 const B[16], real32 C[16])
{
    sdword r, c, k;

    for (c = 0; c < 4; c++)
    {
        for (r = 0; r < 4; r++)
        {
            real32 sum = 0.0f;
            for (k = 0; k < 4; k++)
            {
                sum += A[k * 4 + r] * B[c * 4 + k];
            }
            C[c * 4 + r] = sum;
        }
    }
}

bool32 vrEyeProjection(real32 zNear, real32 zFar)
{
    real32 m[16];
    real32 l, r, b, t;

    if (!vr.eyeActive)
    {
        return FALSE;
    }

    l = zNear * tanf(vr.eyeFov.angleLeft);
    r = zNear * tanf(vr.eyeFov.angleRight);
    b = zNear * tanf(vr.eyeFov.angleDown);
    t = zNear * tanf(vr.eyeFov.angleUp);

    memset(m, 0, sizeof(m));
    m[0] = 2.0f * zNear / (r - l);
    m[5] = 2.0f * zNear / (t - b);
    m[8] = (r + l) / (r - l);
    m[9] = (t + b) / (t - b);
    m[10] = -(zFar + zNear) / (zFar - zNear);
    m[11] = -1.0f;
    m[14] = -2.0f * zFar * zNear / (zFar - zNear);
    glMultMatrixf(m);
    return TRUE;
}

void vrEyeApplyView(void)
{
    if (vr.eyeActive)
    {
        glMultMatrixf(vr.eyeViewMatrix);
    }
}

bool32 vrEyePassActive(void)
{
    return vr.eyeActive;
}

static sdword vrDebugPassIndex(void)
{
    return vr.eyeActive ? vr.debugEye + 1 : 0;
}

static char const* vrDebugPassName(sdword pass)
{
    static char const* names[3] = {"mono", "left", "right"};

    return (pass >= 0 && pass < 3) ? names[pass] : "unknown";
}

static real32 vrDebugRotationDeterminant(real32 const m[16])
{
    return m[0] * (m[5] * m[10] - m[9] * m[6])
         - m[4] * (m[1] * m[10] - m[9] * m[2])
         + m[8] * (m[1] * m[6] - m[5] * m[2]);
}

void vrDebugRenderPass(real32 const view[16], real32 const projection[16],
                       real32 const eye[3], real32 const lookat[3])
{
    sdword pass = vrDebugPassIndex();
    GLint matrixMode = 0, modelDepth = 0, projectionDepth = 0;

    if (!vr.active || vr.frameCount % VR_DEBUG_INTERVAL != 1
        || vr.debugPassFrame[pass] == vr.frameCount)
    {
        return;
    }
    vr.debugPassFrame[pass] = vr.frameCount;
    glGetIntegerv(GL_MATRIX_MODE, &matrixMode);
    glGetIntegerv(GL_MODELVIEW_STACK_DEPTH, &modelDepth);
    glGetIntegerv(GL_PROJECTION_STACK_DEPTH, &projectionDepth);
    SDL_Log("VRDBG PASS frame=%u pass=%s eye=(%.1f %.1f %.1f) "
            "look=(%.1f %.1f %.1f) det=%.5f mode=0x%x stacks=%d/%d",
            (unsigned)vr.frameCount, vrDebugPassName(pass),
            eye[0], eye[1], eye[2], lookat[0], lookat[1], lookat[2],
            vrDebugRotationDeterminant(view), (unsigned)matrixMode,
            (int)modelDepth, (int)projectionDepth);
    SDL_Log("VRDBG VIEW %s r0=(%.4f %.4f %.4f %.2f) "
            "r1=(%.4f %.4f %.4f %.2f) r2=(%.4f %.4f %.4f %.2f)",
            vrDebugPassName(pass),
            view[0], view[4], view[8], view[12],
            view[1], view[5], view[9], view[13],
            view[2], view[6], view[10], view[14]);
    SDL_Log("VRDBG PROJ %s diag=(%.4f %.4f %.4f %.4f) offset=(%.4f %.4f)",
            vrDebugPassName(pass), projection[0], projection[5],
            projection[10], projection[14], projection[8], projection[9]);
}

void vrDebugRenderObject(sdword objtype, real32 const worldPos[3], sdword lod)
{
    sdword pass = vrDebugPassIndex();
    sdword kind;
    GLfloat actual[16];
    real32 camera[4], clip[4];
    real32 ndcX = 0.0f, ndcY = 0.0f, ndcZ = 0.0f;
    real32 delta;
    real32 const* view = (real32 const*)&rndCameraMatrix;
    real32 const* projection = (real32 const*)&rndProjectionMatrix;

    if (objtype == OBJ_ShipType)
    {
        kind = 0;
    }
    else if (objtype == OBJ_AsteroidType)
    {
        kind = 1;
    }
    else
    {
        return;
    }
    if (!vr.active || vr.frameCount % VR_DEBUG_INTERVAL != 1
        || vr.debugObjectFrame[pass][kind] == vr.frameCount)
    {
        return;
    }
    vr.debugObjectFrame[pass][kind] = vr.frameCount;

    camera[0] = view[0] * worldPos[0] + view[4] * worldPos[1]
              + view[8] * worldPos[2] + view[12];
    camera[1] = view[1] * worldPos[0] + view[5] * worldPos[1]
              + view[9] * worldPos[2] + view[13];
    camera[2] = view[2] * worldPos[0] + view[6] * worldPos[1]
              + view[10] * worldPos[2] + view[14];
    camera[3] = 1.0f;
    clip[0] = projection[0] * camera[0] + projection[4] * camera[1]
            + projection[8] * camera[2] + projection[12];
    clip[1] = projection[1] * camera[0] + projection[5] * camera[1]
            + projection[9] * camera[2] + projection[13];
    clip[2] = projection[2] * camera[0] + projection[6] * camera[1]
            + projection[10] * camera[2] + projection[14];
    clip[3] = projection[3] * camera[0] + projection[7] * camera[1]
            + projection[11] * camera[2] + projection[15];
    if (fabsf(clip[3]) > 1e-6f)
    {
        ndcX = clip[0] / clip[3];
        ndcY = clip[1] / clip[3];
        ndcZ = clip[2] / clip[3];
    }

    /* Called immediately after glMultMatrixf(objectTransform): the model
       origin must land at the same camera-space point as view * worldPos. */
    glGetFloatv(GL_MODELVIEW_MATRIX, actual);
    delta = fabsf(actual[12] - camera[0]) + fabsf(actual[13] - camera[1])
          + fabsf(actual[14] - camera[2]);
    SDL_Log("VRDBG OBJ frame=%u pass=%s kind=%s lod=%d "
            "world=(%.1f %.1f %.1f) cam=(%.1f %.1f %.1f) "
            "actual=(%.1f %.1f %.1f) delta=%.4f ndc=(%.3f %.3f %.3f)",
            (unsigned)vr.frameCount, vrDebugPassName(pass),
            kind == 0 ? "ship" : "asteroid", (int)lod,
            worldPos[0], worldPos[1], worldPos[2],
            camera[0], camera[1], camera[2],
            actual[12], actual[13], actual[14], delta, ndcX, ndcY, ndcZ);
}

static bool32 vrCreateStereoSwapchains(void)
{
    XrViewConfigurationView views[VR_EYE_COUNT];
    uint32_t viewCount = 0, i, j;
    XrSwapchainCreateInfo createInfo;

    for (i = 0; i < VR_EYE_COUNT; i++)
    {
        memset(&views[i], 0, sizeof(views[i]));
        views[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    }
    VR_CHECK("xrEnumerateViewConfigurationViews",
             xrEnumerateViewConfigurationViews(vr.instance, vr.systemId,
                                               XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                               VR_EYE_COUNT, &viewCount, views));

    /* The eye views render through the window framebuffer, so they cannot
       exceed its size */
    vr.eyeWidth = (sdword)views[0].recommendedImageRectWidth;
    vr.eyeHeight = (sdword)views[0].recommendedImageRectHeight;
    if (vr.eyeWidth > vr.width)   vr.eyeWidth = vr.width;
    if (vr.eyeHeight > vr.height) vr.eyeHeight = vr.height;

    memset(&createInfo, 0, sizeof(createInfo));
    createInfo.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.format = vr.colorFormat;
    createInfo.sampleCount = 1;
    createInfo.width = vr.eyeWidth;
    createInfo.height = vr.eyeHeight;
    createInfo.faceCount = 1;
    createInfo.arraySize = 1;
    createInfo.mipCount = 1;
    for (i = 0; i < VR_EYE_COUNT; i++)
    {
        VR_CHECK("xrCreateSwapchain (eye)",
                 xrCreateSwapchain(vr.session, &createInfo, &vr.eyeSwapchain[i]));
        for (j = 0; j < VR_MAX_SWAPCHAIN_IMAGES; j++)
        {
            vr.eyeImages[i][j].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        }
        VR_CHECK("xrEnumerateSwapchainImages (eye)",
                 xrEnumerateSwapchainImages(vr.eyeSwapchain[i], VR_MAX_SWAPCHAIN_IMAGES,
                                            &vr.eyeImageCount[i],
                                            (XrSwapchainImageBaseHeader*)vr.eyeImages[i]));
    }
    SDL_Log("VR: stereo eye buffers %dx%d", (int)vr.eyeWidth, (int)vr.eyeHeight);
    return TRUE;
}

/* Blit the freshly rendered eye view (bottom-left of the window
   framebuffer) into the eye's swapchain image. */
static void vrBlitEye(uword eye, uint32_t imageIndex)
{
    vr.rawBindFramebuffer(VR_GL_DRAW_FRAMEBUFFER, vr.blitFbo);
    vr.rawFramebufferTexture2D(VR_GL_DRAW_FRAMEBUFFER, VR_GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, vr.eyeImages[eye][imageIndex].image, 0);
    vr.rawBindFramebuffer(VR_GL_READ_FRAMEBUFFER, 0);
    vr.rawBlitFramebuffer(0, 0, vr.eyeWidth, vr.eyeHeight,
                          0, 0, vr.eyeWidth, vr.eyeHeight,
                          VR_GL_COLOR_BUFFER_BIT, VR_GL_NEAREST);
    vr.rawBindFramebuffer(VR_GL_DRAW_FRAMEBUFFER, 0);
}

/* Render the game world once per eye and fill in the projection layer.
   Returns TRUE when the layer should be submitted this frame. */
static bool32 vrRenderEyes(XrTime displayTime)
{
    XrViewLocateInfo locateInfo;
    XrViewState viewState;
    XrView views[VR_EYE_COUNT];
    uint32_t viewCount = 0;
    real32 anchorModel[16], eyeView[16];
    real32 savedCameraMatrix[16], savedProjectionMatrix[16];
    GLfloat savedGlModelView[16], savedGlProjection[16];
    GLint savedViewport[4], savedMatrixMode;
    GLint savedModelDepth, savedProjectionDepth;
    GLboolean hadScissor, hadLighting, hadTexture, hadFog;
    GLboolean hadDepth, hadBlend, hadCull;
    uword eye;

    static bool32 stereoFailed = FALSE;

    if (!gameIsRunning || mrCamera == NULL || !vr.quadPlaced || stereoFailed)
    {
        return FALSE;
    }

    /* Eye buffers are created on first use: swapchains that exist but are
       never presented keep some runtimes stuck on the loading screen. */
    if (vr.eyeSwapchain[0] == XR_NULL_HANDLE)
    {
        if (!vrCreateStereoSwapchains())
        {
            stereoFailed = TRUE;
            return FALSE;
        }
    }

    memset(&locateInfo, 0, sizeof(locateInfo));
    locateInfo.type = XR_TYPE_VIEW_LOCATE_INFO;
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = displayTime;
    locateInfo.space = vr.space;
    memset(&viewState, 0, sizeof(viewState));
    viewState.type = XR_TYPE_VIEW_STATE;
    for (eye = 0; eye < VR_EYE_COUNT; eye++)
    {
        memset(&views[eye], 0, sizeof(views[eye]));
        views[eye].type = XR_TYPE_VIEW;
    }
    if (XR_FAILED(xrLocateViews(vr.session, &locateInfo, &viewState,
                                VR_EYE_COUNT, &viewCount, views))
        || viewCount != VR_EYE_COUNT
        || !(viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
    {
        return FALSE;
    }

    {
        XrPosef adjusted = vr.anchorPose;

        /* free hologram translation composes as a LOCAL-space shift of
           the anchor (grip drags / crawl glide) */
        adjusted.position.x += vr.worldOffset.x;
        adjusted.position.y += vr.worldOffset.y;
        adjusted.position.z += vr.worldOffset.z;
        vrPoseToModelMatrix(adjusted, vr.worldScale, anchorModel);
    }

    /* the eye renders overwrite the rndCamera/ProjectionMatrix globals with
       head-composed matrices; the rest of the engine (billboards, selection
       circles, tutorial pointers) must keep seeing the pure mono camera */
    memcpy(savedCameraMatrix, (void const*)&rndCameraMatrix, sizeof(savedCameraMatrix));
    memcpy(savedProjectionMatrix, (void const*)&rndProjectionMatrix, sizeof(savedProjectionMatrix));

    glGetIntegerv(GL_MATRIX_MODE, &savedMatrixMode);
    glGetIntegerv(GL_MODELVIEW_STACK_DEPTH, &savedModelDepth);
    glGetIntegerv(GL_PROJECTION_STACK_DEPTH, &savedProjectionDepth);
    glMatrixMode(GL_MODELVIEW);
    glGetFloatv(GL_MODELVIEW_MATRIX, savedGlModelView);
    glMatrixMode(GL_PROJECTION);
    glGetFloatv(GL_PROJECTION_MATRIX, savedGlProjection);
    glMatrixMode((GLenum)savedMatrixMode);
    glGetIntegerv(GL_VIEWPORT, savedViewport);
    hadScissor = glIsEnabled(GL_SCISSOR_TEST);
    hadLighting = glIsEnabled(GL_LIGHTING);
    hadTexture = glIsEnabled(GL_TEXTURE_2D);
    hadFog = glIsEnabled(GL_FOG);
    hadDepth = glIsEnabled(GL_DEPTH_TEST);
    hadBlend = glIsEnabled(GL_BLEND);
    hadCull = glIsEnabled(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);

    if (vr.frameCount % VR_DEBUG_INTERVAL == 1)
    {
        SDL_Log("VRDBG XR frame=%u anchor pos=(%.4f %.4f %.4f) "
                "q=(%.4f %.4f %.4f %.4f) offset=(%.4f %.4f %.4f) "
                "entry stacks=%d/%d",
                (unsigned)vr.frameCount,
                vr.anchorPose.position.x, vr.anchorPose.position.y, vr.anchorPose.position.z,
                vr.anchorPose.orientation.x, vr.anchorPose.orientation.y,
                vr.anchorPose.orientation.z, vr.anchorPose.orientation.w,
                vr.worldOffset.x, vr.worldOffset.y, vr.worldOffset.z,
                (int)savedModelDepth, (int)savedProjectionDepth);
    }

    for (eye = 0; eye < VR_EYE_COUNT; eye++)
    {
        XrSwapchainImageAcquireInfo acquireInfo;
        XrSwapchainImageWaitInfo waitInfo;
        XrSwapchainImageReleaseInfo releaseInfo;
        uint32_t imageIndex = 0;

        memset(&acquireInfo, 0, sizeof(acquireInfo));
        acquireInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
        memset(&waitInfo, 0, sizeof(waitInfo));
        waitInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
        waitInfo.timeout = XR_INFINITE_DURATION;
        memset(&releaseInfo, 0, sizeof(releaseInfo));
        releaseInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;

        if (XR_FAILED(xrAcquireSwapchainImage(vr.eyeSwapchain[eye], &acquireInfo, &imageIndex))
            || XR_FAILED(xrWaitSwapchainImage(vr.eyeSwapchain[eye], &waitInfo)))
        {
            glViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);
            if (hadScissor) glEnable(GL_SCISSOR_TEST);
            memcpy((void*)&rndCameraMatrix, savedCameraMatrix, sizeof(savedCameraMatrix));
            memcpy((void*)&rndProjectionMatrix, savedProjectionMatrix, sizeof(savedProjectionMatrix));
            return FALSE;
        }

        /* eye view = inverse(eye pose) * anchor pose: identity when the
           head sits exactly where the world was anchored */
        vrPoseToViewMatrix(views[eye].pose, vr.worldScale, eyeView);
        vrMatMul(eyeView, anchorModel, vr.eyeViewMatrix);
        vr.eyeFov = views[eye].fov;
        vr.debugEye = (sdword)eye;
        if (vr.frameCount % VR_DEBUG_INTERVAL == 1)
        {
            SDL_Log("VRDBG EYE frame=%u eye=%u pos=(%.4f %.4f %.4f) "
                    "q=(%.4f %.4f %.4f %.4f) relT=(%.2f %.2f %.2f) det=%.5f",
                    (unsigned)vr.frameCount, (unsigned)eye,
                    views[eye].pose.position.x, views[eye].pose.position.y,
                    views[eye].pose.position.z, views[eye].pose.orientation.x,
                    views[eye].pose.orientation.y, views[eye].pose.orientation.z,
                    views[eye].pose.orientation.w, vr.eyeViewMatrix[12],
                    vr.eyeViewMatrix[13], vr.eyeViewMatrix[14],
                    vrDebugRotationDeterminant(vr.eyeViewMatrix));
        }

        glViewport(0, 0, vr.eyeWidth, vr.eyeHeight);
        /* The game's own background colour, not black. The backdrop dome does
           not reach the poles, and on a flat screen the camera's declination
           clamp means nobody ever looks there - in VR the head can, and a
           black clear made the gap read as a hole punched in the sky. Filling
           it with the colour the game itself clears to leaves the tint of the
           nebula instead. */
        glClearColor((real32)colRed(universe.backgroundColor) / 255.0f,
                     (real32)colGreen(universe.backgroundColor) / 255.0f,
                     (real32)colBlue(universe.backgroundColor) / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        vr.eyeActive = TRUE;
        rndMainViewRenderFunction(mrCamera);
        vrWorldDrawOverlays();                              //rays, rings, move disc
        vr.eyeActive = FALSE;
        vr.debugEye = -1;
        glFlush();

        vrBlitEye(eye, imageIndex);
        xrReleaseSwapchainImage(vr.eyeSwapchain[eye], &releaseInfo);

        memset(&vr.projViews[eye], 0, sizeof(vr.projViews[eye]));
        vr.projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        vr.projViews[eye].pose = views[eye].pose;
        vr.projViews[eye].fov = views[eye].fov;
        vr.projViews[eye].subImage.swapchain = vr.eyeSwapchain[eye];
        vr.projViews[eye].subImage.imageRect.extent.width = vr.eyeWidth;
        vr.projViews[eye].subImage.imageRect.extent.height = vr.eyeHeight;
    }

    {
        GLint endModelDepth = 0, endProjectionDepth = 0;

        glGetIntegerv(GL_MODELVIEW_STACK_DEPTH, &endModelDepth);
        glGetIntegerv(GL_PROJECTION_STACK_DEPTH, &endProjectionDepth);
        if (vr.frameCount % VR_DEBUG_INTERVAL == 1
            || endModelDepth != savedModelDepth || endProjectionDepth != savedProjectionDepth)
        {
            SDL_Log("VRDBG GL frame=%u eye-pass stacks before=%d/%d after=%d/%d "
                    "error=0x%x",
                    (unsigned)vr.frameCount, (int)savedModelDepth,
                    (int)savedProjectionDepth, (int)endModelDepth,
                    (int)endProjectionDepth, (unsigned)glGetError());
        }
    }

    /* The eye passes render through the game's global fixed-function
       context. Restore both actual GL state and the globals so no head pose
       can leak into the next mono frame copied to the wrist quad. */
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(savedGlProjection);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(savedGlModelView);
    glMatrixMode((GLenum)savedMatrixMode);
    glViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);
    if (hadScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    rndLightingEnable(hadLighting);
    rndTextureEnable(hadTexture);
    if (hadFog) glEnable(GL_FOG); else glDisable(GL_FOG);
    if (hadDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (hadBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (hadCull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    memcpy((void*)&rndCameraMatrix, savedCameraMatrix, sizeof(savedCameraMatrix));
    memcpy((void*)&rndProjectionMatrix, savedProjectionMatrix, sizeof(savedProjectionMatrix));

    memset(&vr.projLayer, 0, sizeof(vr.projLayer));
    vr.projLayer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
    vr.projLayer.space = vr.space;
    vr.projLayer.viewCount = VR_EYE_COUNT;
    vr.projLayer.views = vr.projViews;
    return TRUE;
}

bool32 vrInit(sdword width, sdword height)
{
    sdword pass, kind;

    memset(&vr, 0, sizeof(vr));
    vr.width = width;
    vr.height = height;
    vr.quadWidth = VR_SCREEN_WIDTH;
    vr.worldScale = VR_WORLD_SCALE;
    vr.debugEye = -1;
    vr.selectGestureHand = -1;
    vr.contextGestureHand = -1;
    vr.pointerHand = -1;
    vrCardInit();
    for (pass = 0; pass < 3; pass++)
    {
        vr.debugPassFrame[pass] = (udword)-1;
        for (kind = 0; kind < 2; kind++)
        {
            vr.debugObjectFrame[pass][kind] = (udword)-1;
        }
    }
    vr.state = XR_SESSION_STATE_UNKNOWN;

    if (!vrLoadRawGles() || !vrInitLoader() || !vrCreateInstance() || !vrCreateSession())
    {
        vrShutdown();
        return FALSE;
    }

    /* Between the session and the first swapchain: it decides the format
       every swapchain is then created with. */
    vrConfigureColorSpace();

    if (!vrCreateSwapchain() || !vrCreateActions())
    {
        vrShutdown();
        return FALSE;
    }

    vr.active = TRUE;
    SDL_Log("VR: OpenXR initialized");
    return TRUE;
}

bool32 vrActive(void)
{
    return vr.active;
}

static void vrHandleSessionState(XrEventDataSessionStateChanged const* event)
{
    XrSessionBeginInfo beginInfo;

    if (vr.state == XR_SESSION_STATE_FOCUSED
        && event->state != XR_SESSION_STATE_FOCUSED)
    {
        vrReleaseInputCapture();
    }
    vr.state = event->state;
    SDL_Log("VR: session state -> %d", (int)event->state);
    switch (event->state)
    {
        case XR_SESSION_STATE_READY:
            memset(&beginInfo, 0, sizeof(beginInfo));
            beginInfo.type = XR_TYPE_SESSION_BEGIN_INFO;
            beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            if (XR_SUCCEEDED(xrBeginSession(vr.session, &beginInfo)))
            {
                vr.sessionRunning = TRUE;
                SDL_Log("VR: session running");
            }
            break;
        case XR_SESSION_STATE_FOCUSED:
            /* User (re)entered the app - put the screen in front of them,
               including an open manager's panel */
            vr.quadPlaced = FALSE;
            vr.managerPlaced = FALSE;
            vr.managerFollowing = FALSE;
            break;
        case XR_SESSION_STATE_STOPPING:
            xrEndSession(vr.session);
            vr.sessionRunning = FALSE;
            SDL_Log("VR: session stopped");
            break;
        case XR_SESSION_STATE_LOSS_PENDING:
        case XR_SESSION_STATE_EXITING:
            vr.sessionRunning = FALSE;
            vr.active = FALSE;
            break;
        default:
            break;
    }
}

static void vrPollEvents(void)
{
    XrEventDataBuffer event;

    for (;;)
    {
        memset(&event, 0, sizeof(event));
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
        if (xrPollEvent(vr.instance, &event) != XR_SUCCESS)
        {
            break;
        }
        switch (event.type)
        {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
                vrHandleSessionState((XrEventDataSessionStateChanged const*)&event);
                break;
            case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
                /* Tracking space recentered (headset donned, Meta-button
                   recenter, ...) - the old anchor is meaningless now */
                vr.quadPlaced = FALSE;
                vr.managerPlaced = FALSE;
                vr.managerFollowing = FALSE;
                SDL_Log("VR: reference space changed, re-anchoring");
                break;
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                vr.sessionRunning = FALSE;
                vr.active = FALSE;
                break;
            default:
                break;
        }
    }
}

/*-----------------------------------------------------------------------------
    Copy the finished frame (window framebuffer, pre-swap) into the
    acquired swapchain texture.
----------------------------------------------------------------------------*/
static void vrCopyFrame(uint32_t imageIndex)
{
    unsigned int error;

    /* Blit rather than glCopyTexSubImage2D: the window surface (RGBX) and
       the swapchain texture (RGBA8) are not copy-compatible under ES rules.
       No Y-flip: the compositor samples GL swapchain textures with the
       usual GL orientation. */
    if (vr.blitFbo == 0)
    {
        vr.rawGenFramebuffers(1, &vr.blitFbo);
    }
    vr.rawBindFramebuffer(VR_GL_DRAW_FRAMEBUFFER, vr.blitFbo);
    vr.rawFramebufferTexture2D(VR_GL_DRAW_FRAMEBUFFER, VR_GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, vr.images[imageIndex].image, 0);
    vr.rawBindFramebuffer(VR_GL_READ_FRAMEBUFFER, 0);
    vr.rawBlitFramebuffer(0, 0, vr.width, vr.height,
                          0, 0, vr.width, vr.height,
                          VR_GL_COLOR_BUFFER_BIT, VR_GL_NEAREST);
    error = vr.rawGetError();
    vr.rawBindFramebuffer(VR_GL_DRAW_FRAMEBUFFER, 0);

    if (error != 0 && vr.errorsLogged < 8)
    {
        vr.errorsLogged++;
        SDL_Log("VR: frame blit GL error 0x%x (tex %u, %dx%d)",
                error, vr.images[imageIndex].image, (int)vr.width, (int)vr.height);
    }
}

static void vrFrameInner(void)
{
    XrFrameState frameState;
    XrFrameWaitInfo waitInfo;
    XrFrameBeginInfo beginInfo;
    XrFrameEndInfo endInfo;
    XrCompositionLayerQuad quad;
    XrCompositionLayerQuad cardQuad[VR_CARD_COUNT];
    bool32 cardReady[VR_CARD_COUNT] = {FALSE, FALSE, FALSE};
    XrCompositionLayerBaseHeader const* layers[2 + VR_CARD_COUNT];
    uint32_t layerCount = 0;
    bool32 managerQuadSubmitted = FALSE;

    if (!vr.active)
    {
        return;
    }

#ifdef __ANDROID__
    /* Take SIGSEGV back if the OpenXR runtime or SDL has claimed it. Done
       here because a crash handler that has been silently displaced reports
       nothing, which is exactly how a whole debugging session was lost. */
    mainCrashHandlerKeep();
#endif

    vrPollEvents();
    if (!vr.sessionRunning)
    {
        return;
    }

    memset(&waitInfo, 0, sizeof(waitInfo));
    waitInfo.type = XR_TYPE_FRAME_WAIT_INFO;
    memset(&frameState, 0, sizeof(frameState));
    frameState.type = XR_TYPE_FRAME_STATE;
    if (XR_FAILED(xrWaitFrame(vr.session, &waitInfo, &frameState)))
    {
        return;
    }

    memset(&beginInfo, 0, sizeof(beginInfo));
    beginInfo.type = XR_TYPE_FRAME_BEGIN_INFO;
    if (XR_FAILED(xrBeginFrame(vr.session, &beginInfo)))
    {
        return;
    }

    if (frameState.shouldRender)
    {
        XrSwapchainImageAcquireInfo acquireInfo;
        XrSwapchainImageWaitInfo imageWaitInfo;
        XrSwapchainImageReleaseInfo releaseInfo;
        uint32_t imageIndex = 0;

        /* capture the pure game camera matrix (the eye passes overwrite
           rndCameraMatrix later) and refresh the LOCAL->world transforms */
        {
            real32 anchorPos[3] = {vr.anchorPose.position.x + vr.worldOffset.x,
                                   vr.anchorPose.position.y + vr.worldOffset.y,
                                   vr.anchorPose.position.z + vr.worldOffset.z};
            real32 anchorQuat[4] = {vr.anchorPose.orientation.x,
                                    vr.anchorPose.orientation.y,
                                    vr.anchorPose.orientation.z,
                                    vr.anchorPose.orientation.w};

            bool32 was = vr.worldInteractive;

            vr.worldInteractive = vrWorldFrameBegin(anchorPos, anchorQuat, vr.worldScale);
            if (vr.worldInteractive != was)
            {
                SDL_Log("VR: world interactive -> %d", (int)vr.worldInteractive);
            }
        }

        vrUpdateScreenPose(frameState.predictedDisplayTime);
        vrUpdateInput(frameState.predictedDisplayTime);
        vrUpdateCardPoses(frameState.predictedDisplayTime);
        vrUpdateWheelPose(frameState.predictedDisplayTime);
        vrDrawManagerBorder();
        vrDrawPanelReticle();

        memset(&acquireInfo, 0, sizeof(acquireInfo));
        acquireInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
        memset(&imageWaitInfo, 0, sizeof(imageWaitInfo));
        imageWaitInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
        imageWaitInfo.timeout = XR_INFINITE_DURATION;
        memset(&releaseInfo, 0, sizeof(releaseInfo));
        releaseInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;

        if (XR_SUCCEEDED(xrAcquireSwapchainImage(vr.swapchain, &acquireInfo, &imageIndex))
            && XR_SUCCEEDED(xrWaitSwapchainImage(vr.swapchain, &imageWaitInfo)))
        {
            vrCopyFrame(imageIndex);
            xrReleaseSwapchainImage(vr.swapchain, &releaseInfo);

            /* Cards scribble their layout into the framebuffer, so they must
               come after the game frame has been copied out of it and before
               the eye passes clear it. */
            {
                sdword card;

                for (card = 0; card < VR_CARD_COUNT; card++)
                {
                    cardReady[card] = vrCardSubmit(card, &cardQuad[card]);
                }
            }

            /* stereo world behind the UI screen (in-game only; the window
               framebuffer is reused per eye, which is why the quad blit
               above must happen first) */
            if (vrRenderEyes(frameState.predictedDisplayTime))
            {
                layers[layerCount++] = (XrCompositionLayerBaseHeader const*)&vr.projLayer;
            }

            /* While a manager is open the quad IS the manager screen, so it
               is submitted only with a pose this frame validated as
               submittable - never at a stale wrist pose. */
            {
                bool32 managerOpen = vr.worldInteractive && vrWorldManagerActive();
                bool32 showQuad = managerOpen
                                ? vr.managerPoseValid
                                : !(vr.worldInteractive && vr.panelHidden);

                if (showQuad && vrPoseSubmittable(&vr.quadPose))
                {
                    memset(&quad, 0, sizeof(quad));
                    quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
                    quad.space = vr.space;
                    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    quad.subImage.swapchain = vr.swapchain;
                    quad.subImage.imageRect.extent.width = vr.width;
                    quad.subImage.imageRect.extent.height = vr.height;
                    quad.pose = vr.quadPose;
                    quad.size.width = vr.quadWidth;
                    quad.size.height = vrQuadHeightFor(vr.quadWidth);
                    layers[layerCount++] = (XrCompositionLayerBaseHeader const*)&quad;
                    managerQuadSubmitted = managerOpen;
                }
            }

            {
                sdword card;

                for (card = 0; card < VR_CARD_COUNT; card++)
                {
                    if (cardReady[card])
                    {
                        layers[layerCount++] =
                            (XrCompositionLayerBaseHeader const*)&cardQuad[card];
                    }
                }
            }
        }
    }

    memset(&endInfo, 0, sizeof(endInfo));
    endInfo.type = XR_TYPE_FRAME_END_INFO;
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = layerCount;
    endInfo.layers = layers;
    {
        XrResult result = xrEndFrame(vr.session, &endInfo);

        if (XR_FAILED(result))
        {
            if (vr.errorsLogged < 8)
            {
                vr.errorsLogged++;
                vrLogResult("xrEndFrame", result);
            }
            if (managerQuadSubmitted)
            {
                /* the compositor rejected the frame the manager panel was
                   in: keep presentation pending so input stays escapable */
                SDL_Log("VR: manager '%s' frame rejected, pose=(%.2f %.2f %.2f) "
                        "q=(%.4f %.4f %.4f %.4f) width=%.2f layers=%u",
                        vrWorldManagerName(), vr.quadPose.position.x,
                        vr.quadPose.position.y, vr.quadPose.position.z,
                        vr.quadPose.orientation.x, vr.quadPose.orientation.y,
                        vr.quadPose.orientation.z, vr.quadPose.orientation.w,
                        vr.quadWidth, (unsigned)layerCount);
            }
        }
        else if (managerQuadSubmitted && vr.managerState != VR_MGR_VISIBLE)
        {
            SDL_Log("VR: manager '%s' panel visible after %u frames "
                    "(pose=(%.2f %.2f %.2f) width=%.2f) - input now modal",
                    vrWorldManagerName(), (unsigned)vr.managerFrames,
                    vr.quadPose.position.x, vr.quadPose.position.y,
                    vr.quadPose.position.z, vr.quadWidth);
            vr.managerState = VR_MGR_VISIBLE;
        }
    }

    vr.frameCount++;
    if (vr.frameCount % 3600 == 1)
    {
        SDL_Log("VR: frame %u, state %d, shouldRender %d, layers %u",
                (unsigned)vr.frameCount, (int)vr.state,
                (int)frameState.shouldRender, (unsigned)layerCount);
    }
}

void vrFrame(void)
{
    /* The game re-enters the render flush from inside our own frame: opening
       a manager runs rndClear(), and every rndFlush() in it lands back here
       while xrBeginFrame is still outstanding. Nested OpenXR frames break
       the frame sequence the compositor relies on, and the nested
       vrUpdateInput would re-fire the still-held button that opened the
       manager - recursing until the stack gives out. Drop inner calls. */
    if (vr.inFrame)
    {
        if (vr.reentryLogged < 8)
        {
            vr.reentryLogged++;
            SDL_Log("VR: dropped re-entrant vrFrame at frame %u (manager=%s)",
                    (unsigned)vr.frameCount, vrWorldManagerName());
        }
        return;
    }
    vr.inFrame = TRUE;
    vrFrameInner();
    vr.inFrame = FALSE;
}

void vrShutdown(void)
{
    uword i;

    if (vr.blitFbo != 0 && vr.rawDeleteFramebuffers != NULL)
    {
        vr.rawDeleteFramebuffers(1, &vr.blitFbo);
        vr.blitFbo = 0;
    }
    for (i = 0; i < VR_HAND_COUNT; i++)
    {
        if (vr.aimSpace[i] != XR_NULL_HANDLE)
        {
            xrDestroySpace(vr.aimSpace[i]);
        }
        if (vr.gripSpace[i] != XR_NULL_HANDLE)
        {
            xrDestroySpace(vr.gripSpace[i]);
        }
    }
    if (vr.actionSet != XR_NULL_HANDLE)
    {
        xrDestroyActionSet(vr.actionSet);
    }
    if (vr.swapchain != XR_NULL_HANDLE)
    {
        xrDestroySwapchain(vr.swapchain);
    }
    for (i = 0; i < VR_EYE_COUNT; i++)
    {
        if (vr.eyeSwapchain[i] != XR_NULL_HANDLE)
        {
            xrDestroySwapchain(vr.eyeSwapchain[i]);
        }
    }
    for (i = 0; i < VR_CARD_COUNT; i++)
    {
        if (vr.card[i].swapchain != XR_NULL_HANDLE)
        {
            xrDestroySwapchain(vr.card[i].swapchain);
        }
    }
    if (vr.viewSpace != XR_NULL_HANDLE)
    {
        xrDestroySpace(vr.viewSpace);
    }
    if (vr.space != XR_NULL_HANDLE)
    {
        xrDestroySpace(vr.space);
    }
    if (vr.session != XR_NULL_HANDLE)
    {
        xrDestroySession(vr.session);
    }
    if (vr.instance != XR_NULL_HANDLE)
    {
        xrDestroyInstance(vr.instance);
    }
    memset(&vr, 0, sizeof(vr));
}

#endif /* HW_ENABLE_VR */
