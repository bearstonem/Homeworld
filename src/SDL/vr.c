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

#define VR_MAX_SWAPCHAIN_IMAGES 8

/* Virtual screen: ~2m away, 4:3 like the game's default resolution */
#define VR_SCREEN_DISTANCE  2.0f
#define VR_SCREEN_WIDTH     2.4f

#define VR_HAND_LEFT   0
#define VR_HAND_RIGHT  1
#define VR_HAND_COUNT  2

#define VR_EYE_COUNT   2

/* How many game-world units one real-world metre of head movement is
   worth. Homeworld ships are hundreds of units long; at 1000 the fleet
   reads as a room-sized hologram. */
#define VR_WORLD_SCALE 1000.0f

extern SDL_Window *sdlwindow;

#include "Camera.h"
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
    XrPath       handPath[VR_HAND_COUNT];
    XrSpace      aimSpace[VR_HAND_COUNT];
    XrSpace      gripSpace[VR_HAND_COUNT];
    real32       quadWidth;         /* current screen width in metres */
    bool32       prevSelect;
    bool32       prevContext;
    bool32       prevBack;
    bool32       prevGripLeft;
    bool32       prevStickClick[VR_HAND_COUNT];
    sdword       contextKey;        /* key sent at press, released on release (0 = mouse) */
    sdword       backKey;
    sdword       stickClickKey[VR_HAND_COUNT];
    bool32       stickRotating;     /* left stick currently orbiting the camera */
    real32       wheelAccum;
    sdword       pointerX, pointerY;
    bool32       pointerValid;
    bool32       worldInteractive;  /* game world exists; native 3D interaction on */
    sdword       activeHand;
    real32       moveHeight;        /* metres of vertical offset in a move order */
    bool32       panelHidden;       /* wrist panel toggled off in-game */
    bool32       grabValid[VR_HAND_COUNT];
    XrVector3f   grabPrev[VR_HAND_COUNT];
    bool32       pinchValid;
    real32       pinchPrevDist;
    real32       pinchPrevAzimuth;
    real32       pinchPrevElev;
    XrVector3f   crawlVel;          /* throw-glide velocity, metres/frame */
    sdword       prevCycleDir;      /* right-stick flick edge state */
    XrVector3f   worldOffset;       /* free hologram translation, metres (the
                                       game camera cannot pan: it is focus-
                                       locked, so drags translate the world
                                       in LOCAL space instead) */
    rawGlGetIntegerv_t          rawGetIntegerv;
    rawGlGetError_t             rawGetError;
    rawGlGenFramebuffers_t      rawGenFramebuffers;
    rawGlBindFramebuffer_t      rawBindFramebuffer;
    rawGlFramebufferTexture2D_t rawFramebufferTexture2D;
    rawGlBlitFramebuffer_t      rawBlitFramebuffer;
    rawGlDeleteFramebuffers_t   rawDeleteFramebuffers;
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
    int64_t chosen = 0;
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

    /* Prefer a linear format: the game's output is not sRGB-encoded, and
       copying linear pixels into an sRGB swapchain would double-correct. */
    for (i = 0; i < formatCount; i++)
    {
        if (formats[i] == GL_RGBA8)
        {
            chosen = formats[i];
            break;
        }
    }
    if (chosen == 0 && formatCount > 0)
    {
        chosen = formats[0];
        SDL_Log("VR: GL_RGBA8 unavailable, using swapchain format 0x%x", (unsigned)chosen);
    }

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
    return TRUE;
}

/*-----------------------------------------------------------------------------
    Touch controller input: aim ray + trigger/buttons, mapped onto the
    virtual screen as mouse input.
----------------------------------------------------------------------------*/
static bool32 vrCreateActions(void)
{
    XrActionSetCreateInfo setInfo;
    XrActionCreateInfo actionInfo;
    XrActionSuggestedBinding bindings[14];
    XrInteractionProfileSuggestedBinding suggested;
    XrSessionActionSetsAttachInfo attachInfo;
    XrActionSpaceCreateInfo spaceInfo;
    XrPath profilePath;
    uword i;

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

    {
        struct { XrAction action; char const* path; } layout[16] = {
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
        };
        for (i = 0; i < 16; i++)
        {
            bindings[i].action = layout[i].action;
            xrStringToPath(vr.instance, layout[i].path, &bindings[i].binding);
        }
    }

    xrStringToPath(vr.instance, "/interaction_profiles/oculus/touch_controller", &profilePath);
    memset(&suggested, 0, sizeof(suggested));
    suggested.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
    suggested.interactionProfile = profilePath;
    suggested.countSuggestedBindings = 14;
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

/* In-game the screen becomes a small panel riding the left wrist */
static bool32 vrUpdateWristPanel(XrTime displayTime)
{
    XrSpaceLocation grip, head;
    XrVector3f offset = {0.0f, 0.06f, -0.05f}, worldOffset, toPanel;
    real32 mag;

    memset(&grip, 0, sizeof(grip));
    grip.type = XR_TYPE_SPACE_LOCATION;
    memset(&head, 0, sizeof(head));
    head.type = XR_TYPE_SPACE_LOCATION;
    /* the aim pose is used (grip pose returns no tracking on this runtime) */
    if (XR_FAILED(xrLocateSpace(vr.aimSpace[VR_HAND_LEFT], vr.space, displayTime, &grip))
        || !(grip.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
        || !(grip.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)
        || XR_FAILED(xrLocateSpace(vr.viewSpace, vr.space, displayTime, &head))
        || !(head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
    {
        if (vr.frameCount % 300 == 1)
        {
            SDL_Log("VR: wrist panel: left aim not tracked (flags 0x%x)",
                    (unsigned)grip.locationFlags);
        }
        return FALSE;
    }

    offset.x = 0.0f;
    offset.y = 0.10f;                                       //above the controller
    offset.z = 0.06f;                                       //slightly toward the wrist
    vrQuatRotate(grip.pose.orientation, offset, &worldOffset);
    vr.quadPose.position.x = grip.pose.position.x + worldOffset.x;
    vr.quadPose.position.y = grip.pose.position.y + worldOffset.y;
    vr.quadPose.position.z = grip.pose.position.z + worldOffset.z;

    /* watch-face orientation: locked to the wrist, tilted so the panel
       faces the user when the arm is raised naturally */
    {
        XrQuaternionf tilt = {0.0f, -0.7071068f, 0.7071068f, 0.0f};  /* +90degX then 180degZ roll */
        XrQuaternionf q = grip.pose.orientation;

        vr.quadPose.orientation.w = q.w * tilt.w - q.x * tilt.x - q.y * tilt.y - q.z * tilt.z;
        vr.quadPose.orientation.x = q.w * tilt.x + q.x * tilt.w + q.y * tilt.z - q.z * tilt.y;
        vr.quadPose.orientation.y = q.w * tilt.y - q.x * tilt.z + q.y * tilt.w + q.z * tilt.x;
        vr.quadPose.orientation.z = q.w * tilt.z + q.x * tilt.y - q.y * tilt.x + q.z * tilt.w;
    }
    (void)head;
    (void)toPanel;
    (void)mag;
    vr.quadWidth = VR_WRIST_PANEL_WIDTH;
    vr.quadPlaced = TRUE;
    return TRUE;
}

static void vrUpdateScreenPose(XrTime displayTime)
{
    XrSpaceLocation location;
    XrVector3f forward = {0.0f, 0.0f, -VR_SCREEN_DISTANCE}, offset;
    XrVector3f gaze = {0.0f, 0.0f, -1.0f}, facing, toQuad;
    XrPosef desired;
    real32 mag, dot;

    if (vr.worldInteractive)
    {
        real32 panelPos[3];

        if (vrWorldManagerPanelAnchor(panelPos))
        {
            /* Build/Launch/Research manager open: the game frame floats
               beside the ship it concerns, billboarded to the head */
            XrSpaceLocation head;
            XrVector3f toPanel;
            real32 mag;

            vr.quadPose.position.x = panelPos[0];
            vr.quadPose.position.y = panelPos[1];
            vr.quadPose.position.z = panelPos[2];
            memset(&head, 0, sizeof(head));
            head.type = XR_TYPE_SPACE_LOCATION;
            if (XR_SUCCEEDED(xrLocateSpace(vr.viewSpace, vr.space, displayTime, &head))
                && (head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
            {
                toPanel.x = vr.quadPose.position.x - head.pose.position.x;
                toPanel.y = vr.quadPose.position.y - head.pose.position.y;
                toPanel.z = vr.quadPose.position.z - head.pose.position.z;
                mag = sqrtf(toPanel.x * toPanel.x + toPanel.y * toPanel.y + toPanel.z * toPanel.z);
                if (mag > 1e-4f)
                {
                    toPanel.x /= mag; toPanel.y /= mag; toPanel.z /= mag;
                    vrLookOrientation(toPanel, &vr.quadPose.orientation);
                }
            }
            vr.quadWidth = 0.85f;
            vr.quadPlaced = TRUE;
            return;
        }

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
        vr.anchorPose = location.pose;                      //world anchor
        vr.quadPlaced = TRUE;
        vr.quadFollowing = FALSE;
        SDL_Log("VR: screen anchored at (%.2f, %.2f, %.2f)",
                desired.position.x, desired.position.y, desired.position.z);
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

/* Intersect a hand's aim ray with the virtual screen; returns TRUE and the
   game-window pixel coordinates on hit. */
static bool32 vrPointerFromHand(uword hand, XrTime time, sdword* px, sdword* py)
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

    quadHeight = vr.quadWidth * (real32)vr.height / (real32)vr.width;
    u = (local.x + t * localDir.x) / vr.quadWidth + 0.5f;
    v = 0.5f - (local.y + t * localDir.y) / quadHeight;
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
    {
        return FALSE;
    }

    *px = (sdword)(u * (real32)vr.width);
    *py = (sdword)(v * (real32)vr.height);
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

static bool32 vrActionPressed(XrAction action)
{
    return vrActionPressedHand(action, VR_HAND_LEFT) || vrActionPressedHand(action, VR_HAND_RIGHT);
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
    vrQuatRotate(location.pose.orientation, forward, &d);
    dir[0] = d.x;
    dir[1] = d.y;
    dir[2] = d.z;
    return TRUE;
}

static void vrUpdateInput(XrTime time)
{
    XrActiveActionSet activeSet;
    XrActionsSyncInfo syncInfo;
    sdword px, py;
    bool32 select, context, back;

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

    /* feed both aim rays into the world-interaction layer (in-game only;
       vrWorldFrameBegin gates on a live game world) */
    if (vr.worldInteractive)
    {
        uword hand;
        real32 pos[VR_HAND_COUNT][3], dir[VR_HAND_COUNT][3];
        bool32 tracked[VR_HAND_COUNT];
        bool32 grip[VR_HAND_COUNT];

        for (hand = 0; hand < VR_HAND_COUNT; hand++)
        {
            tracked[hand] = vrHandAimLocal(hand, time, pos[hand], dir[hand]);
            grip[hand] = vrActionPressedHand(vr.gripAction, hand);
            vrWorldSetRay((sdword)hand, pos[hand], dir[hand], tracked[hand]);
        }
        vr.activeHand = vrWorldHandHasTarget(VR_HAND_RIGHT) ? VR_HAND_RIGHT
                      : vrWorldHandHasTarget(VR_HAND_LEFT) ? VR_HAND_LEFT
                      : VR_HAND_RIGHT;

        /* grab-the-space gestures. Two grips: pinch zoom + pair rotation.
           One grip: 1:1 pan (your hand drags the hologram). */
        if (grip[VR_HAND_LEFT] && grip[VR_HAND_RIGHT]
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

                if (dAz > 3.14159f)  dAz -= 6.28318f;
                if (dAz < -3.14159f) dAz += 6.28318f;
                vrWorldCameraZoom(vr.pinchPrevDist / dist);
                vrWorldCameraOrbit(dAz, elev - vr.pinchPrevElev);
            }
            vr.pinchPrevDist = dist;
            vr.pinchPrevAzimuth = azimuth;
            vr.pinchPrevElev = elev;
            vr.pinchValid = TRUE;
            vr.grabValid[VR_HAND_LEFT] = vr.grabValid[VR_HAND_RIGHT] = FALSE;
            vr.crawlVel.x = vr.crawlVel.y = vr.crawlVel.z = 0.0f;
        }
        else
        {
            /* Traversal follows Homeworld's own mechanic: select a ship and
               focus on it (left stick click). Single-grip drags do nothing;
               two-grip pinch handles orbit/zoom above. */
            vr.pinchValid = FALSE;
            vr.grabValid[VR_HAND_LEFT] = vr.grabValid[VR_HAND_RIGHT] = FALSE;
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
        /* in-game: left stick orbits, right stick Y zooms, right stick X
           flicks cycle the camera focus through the fleet */
        real32 lx, ly, rx, ry;
        sdword cycleDir;

        vrActionStick(VR_HAND_LEFT, &lx, &ly);
        if (lx * lx + ly * ly > 0.04f)
        {
            vrWorldCameraOrbit(-lx * 0.035f, ly * 0.025f);
        }
        vrActionStick(VR_HAND_RIGHT, &rx, &ry);
        if ((ry > 0.25f || ry < -0.25f) && !vrWorldMoveActive())
        {
            vrWorldCameraZoom(1.0f - ry * 0.02f);
        }
        cycleDir = (rx > 0.7f) ? 1 : (rx < -0.7f) ? -1 : 0;
        if (cycleDir != 0 && vr.prevCycleDir == 0)
        {
            vrWorldFocusCycle(cycleDir);
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

    /* panel pointer from the aim ray (right hand wins). In-game the panel
       only receives the pointer when no ship is hovered (wizard clicks),
       and never while hidden. */
    if (!vr.stickRotating
        && !(vr.worldInteractive && !vrWorldManagerActive()
             && (vrWorldHandHasTarget(vr.activeHand) || vr.panelHidden)))
    {
        if (vrPointerFromHand(VR_HAND_RIGHT, time, &px, &py)
            || vrPointerFromHand(VR_HAND_LEFT, time, &px, &py))
        {
            vr.pointerX = px;
            vr.pointerY = py;
            vr.pointerValid = TRUE;
            SDL_WarpMouseInWindow(sdlwindow, (int)px, (int)py);
        }
        else
        {
            vr.pointerValid = FALSE;
        }
    }
    else if (vr.worldInteractive)
    {
        vr.pointerValid = FALSE;
    }

    select = vrActionPressed(vr.selectAction);
    context = vrActionPressed(vr.contextAction);
    back = vrActionPressed(vr.backAction);

    if (select != vr.prevSelect)
    {
        if (vr.worldInteractive && !vr.pointerValid)
        {
            /* native selection */
            bool32 additive = vrActionPressedHand(vr.gripAction, VR_HAND_LEFT);

            if (select)
            {
                if (vrWorldHandHasTarget(vr.activeHand))
                {
                    vrWorldSelectClick(vr.activeHand, additive);
                }
                else
                {
                    vrWorldSweepBegin(vr.activeHand);
                }
            }
            else
            {
                vrWorldSweepCommit(vr.activeHand, additive);
            }
            vr.prevSelect = select;
        }
        else if (vr.pointerValid || !select)
        {
            vrPushMouseButton(SDL_BUTTON_LEFT, select);
            vr.prevSelect = select;
        }
    }

    /* in-game native context orders / move on A-X */
    if (vr.worldInteractive && !vr.pointerValid)
    {
        if (context != vr.prevContext)
        {
            if (context)
            {
                if (!vrWorldContextOrder(vr.activeHand))
                {
                    vrWorldMoveBegin(vr.activeHand);
                }
            }
            else
            {
                vrWorldMoveCommit();
            }
            vr.prevContext = context;
        }
        if (vrWorldMoveActive())
        {
            real32 rx, ry;

            vrActionStick(VR_HAND_RIGHT, &rx, &ry);
            vr.moveHeight += ry * 0.004f;
            vrWorldMoveUpdate(vr.activeHand, vr.moveHeight);
        }
        else
        {
            vr.moveHeight = 0.0f;
        }
        if (back && !vr.prevBack && vrWorldMoveActive())
        {
            vrWorldMoveCancel();
            vr.prevBack = back;                             //swallow this ESC
        }
        else if (back && !vr.prevBack
                 && vrActionPressedHand(vr.gripAction, VR_HAND_LEFT))
        {
            /* left grip + B/Y: toggle the wrist panel */
            vr.panelHidden = !vr.panelHidden;
            SDL_Log("VR: wrist panel %s", vr.panelHidden ? "hidden" : "shown");
            vr.prevBack = back;                             //swallow this ESC
        }
    }

    /* A/X: right mouse button, or Dock with the right-grip chord */
    if (context != vr.prevContext)
    {
        if (context)
        {
            vr.contextKey = vrActionPressedHand(vr.gripAction, VR_HAND_RIGHT) ? SDLK_d : 0;
            if (vr.contextKey)
            {
                vrPushKey(vr.contextKey, SDL_SCANCODE_D, TRUE);
            }
            else if (vr.pointerValid)
            {
                vrPushMouseButton(SDL_BUTTON_RIGHT, TRUE);
            }
        }
        else
        {
            if (vr.contextKey)
            {
                vrPushKey(vr.contextKey, SDL_SCANCODE_D, FALSE);
                vr.contextKey = 0;
            }
            else
            {
                vrPushMouseButton(SDL_BUTTON_RIGHT, FALSE);
            }
        }
        vr.prevContext = context;
    }

    /* B/Y: Escape, or Build manager with the right-grip chord */
    if (back != vr.prevBack)
    {
        if (back)
        {
            vr.backKey = vrActionPressedHand(vr.gripAction, VR_HAND_RIGHT) ? SDLK_b : SDLK_ESCAPE;
            vrPushKey(vr.backKey, (vr.backKey == SDLK_b) ? SDL_SCANCODE_B : SDL_SCANCODE_ESCAPE, TRUE);
        }
        else
        {
            vrPushKey(vr.backKey, (vr.backKey == SDLK_b) ? SDL_SCANCODE_B : SDL_SCANCODE_ESCAPE, FALSE);
        }
        vr.prevBack = back;
    }

    /* left grip: Shift (additive selection / band-box) */
    {
        bool32 gripLeft = vrActionPressedHand(vr.gripAction, VR_HAND_LEFT);

        if (gripLeft != vr.prevGripLeft)
        {
            vrPushKey(SDLK_LSHIFT, SDL_SCANCODE_LSHIFT, gripLeft);
            vr.prevGripLeft = gripLeft;
        }
    }

    /* stick clicks: left = Move order (chord: Research), right = Sensors */
    {
        uword hand;

        for (hand = 0; hand < VR_HAND_COUNT; hand++)
        {
            bool32 pressed = vrActionPressedHand(vr.stickClickAction, hand);

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
                        vrWorldCameraFocusSelection();
                        vr.worldOffset.x = vr.worldOffset.y = vr.worldOffset.z = 0.0f;
                        vr.crawlVel.x = vr.crawlVel.y = vr.crawlVel.z = 0.0f;
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
    createInfo.format = GL_RGBA8;
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
    GLint savedViewport[4];
    GLboolean hadScissor;
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
        vrPoseToModelMatrix(adjusted, VR_WORLD_SCALE, anchorModel);
    }

    glGetIntegerv(GL_VIEWPORT, savedViewport);
    hadScissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);

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
            return FALSE;
        }

        /* eye view = inverse(eye pose) * anchor pose: identity when the
           head sits exactly where the world was anchored */
        vrPoseToViewMatrix(views[eye].pose, VR_WORLD_SCALE, eyeView);
        vrMatMul(eyeView, anchorModel, vr.eyeViewMatrix);
        vr.eyeFov = views[eye].fov;

        glViewport(0, 0, vr.eyeWidth, vr.eyeHeight);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        vr.eyeActive = TRUE;
        rndMainViewRenderFunction(mrCamera);
        vrWorldDrawOverlays();                              //rays, rings, move disc
        vr.eyeActive = FALSE;
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

    glViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);
    if (hadScissor) glEnable(GL_SCISSOR_TEST);

    memset(&vr.projLayer, 0, sizeof(vr.projLayer));
    vr.projLayer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
    vr.projLayer.space = vr.space;
    vr.projLayer.viewCount = VR_EYE_COUNT;
    vr.projLayer.views = vr.projViews;
    return TRUE;
}

bool32 vrInit(sdword width, sdword height)
{
    memset(&vr, 0, sizeof(vr));
    vr.width = width;
    vr.height = height;
    vr.quadWidth = VR_SCREEN_WIDTH;
    vr.state = XR_SESSION_STATE_UNKNOWN;

    if (!vrLoadRawGles() || !vrInitLoader() || !vrCreateInstance() || !vrCreateSession()
        || !vrCreateSwapchain() || !vrCreateActions())
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
            /* User (re)entered the app - put the screen in front of them */
            vr.quadPlaced = FALSE;
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

void vrFrame(void)
{
    XrFrameState frameState;
    XrFrameWaitInfo waitInfo;
    XrFrameBeginInfo beginInfo;
    XrFrameEndInfo endInfo;
    XrCompositionLayerQuad quad;
    XrCompositionLayerBaseHeader const* layers[2];
    uint32_t layerCount = 0;

    if (!vr.active)
    {
        return;
    }

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

            vr.worldInteractive = vrWorldFrameBegin(anchorPos, anchorQuat, VR_WORLD_SCALE);
            if (vr.worldInteractive != was)
            {
                SDL_Log("VR: world interactive -> %d", (int)vr.worldInteractive);
            }
        }

        vrUpdateScreenPose(frameState.predictedDisplayTime);
        vrUpdateInput(frameState.predictedDisplayTime);

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

            /* stereo world behind the UI screen (in-game only; the window
               framebuffer is reused per eye, which is why the quad blit
               above must happen first) */
            if (vrRenderEyes(frameState.predictedDisplayTime))
            {
                layers[layerCount++] = (XrCompositionLayerBaseHeader const*)&vr.projLayer;
            }

            if (!(vr.worldInteractive && vr.panelHidden))
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
                quad.size.height = vr.quadWidth * (real32)vr.height / (real32)vr.width;
                layers[layerCount++] = (XrCompositionLayerBaseHeader const*)&quad;
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
        if (XR_FAILED(result) && vr.errorsLogged < 8)
        {
            vr.errorsLogged++;
            vrLogResult("xrEndFrame", result);
        }
    }

    vr.frameCount++;
    if (vr.frameCount % 300 == 1)
    {
        SDL_Log("VR: frame %u, state %d, shouldRender %d, layers %u",
                (unsigned)vr.frameCount, (int)vr.state,
                (int)frameState.shouldRender, (unsigned)layerCount);
    }
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
