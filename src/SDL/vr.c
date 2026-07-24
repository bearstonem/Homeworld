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

extern SDL_Window *sdlwindow;

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
    XrSwapchain  swapchain;
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
    XrAction     selectAction;      /* trigger  -> left mouse button  */
    XrAction     contextAction;     /* A/X      -> right mouse button */
    XrAction     backAction;        /* B/Y      -> Escape key         */
    XrPath       handPath[VR_HAND_COUNT];
    XrSpace      aimSpace[VR_HAND_COUNT];
    bool32       prevSelect;
    bool32       prevContext;
    bool32       prevBack;
    sdword       pointerX, pointerY;
    bool32       pointerValid;
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

/* Anchor the virtual screen in front of wherever the user is actually
   looking on the first tracked frame (the LOCAL origin is wherever the
   headset happened to be at app start, which may be nowhere useful). */
static void vrPlaceQuad(XrTime displayTime)
{
    XrSpaceLocation location;
    XrVector3f forward = {0.0f, 0.0f, -VR_SCREEN_DISTANCE};
    XrVector3f offset;

    memset(&location, 0, sizeof(location));
    location.type = XR_TYPE_SPACE_LOCATION;
    if (XR_FAILED(xrLocateSpace(vr.viewSpace, vr.space, displayTime, &location))
        || !(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
        || !(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
    {
        return;                                             //try again next frame
    }

    vrQuatRotate(location.pose.orientation, forward, &offset);
    vr.quadPose.position.x = location.pose.position.x + offset.x;
    vr.quadPose.position.y = location.pose.position.y + offset.y;
    vr.quadPose.position.z = location.pose.position.z + offset.z;
    vr.quadPose.orientation = location.pose.orientation;
    vr.quadPlaced = TRUE;
    SDL_Log("VR: screen anchored at (%.2f, %.2f, %.2f)",
            vr.quadPose.position.x, vr.quadPose.position.y, vr.quadPose.position.z);
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
    XrActionSuggestedBinding bindings[8];
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

    {
        struct { XrAction action; char const* path; } layout[8] = {
            { vr.aimAction,     "/user/hand/left/input/aim/pose" },
            { vr.aimAction,     "/user/hand/right/input/aim/pose" },
            { vr.selectAction,  "/user/hand/left/input/trigger/value" },
            { vr.selectAction,  "/user/hand/right/input/trigger/value" },
            { vr.contextAction, "/user/hand/left/input/x/click" },
            { vr.contextAction, "/user/hand/right/input/a/click" },
            { vr.backAction,    "/user/hand/left/input/y/click" },
            { vr.backAction,    "/user/hand/right/input/b/click" },
        };
        for (i = 0; i < 8; i++)
        {
            bindings[i].action = layout[i].action;
            xrStringToPath(vr.instance, layout[i].path, &bindings[i].binding);
        }
    }

    xrStringToPath(vr.instance, "/interaction_profiles/oculus/touch_controller", &profilePath);
    memset(&suggested, 0, sizeof(suggested));
    suggested.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
    suggested.interactionProfile = profilePath;
    suggested.countSuggestedBindings = 8;
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

    return TRUE;
}

/* v rotated by the inverse of q (q assumed unit length) */
static void vrQuatUnrotate(XrQuaternionf q, XrVector3f v, XrVector3f* out)
{
    XrQuaternionf conj = {-q.x, -q.y, -q.z, q.w};
    vrQuatRotate(conj, v, out);
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

    quadHeight = VR_SCREEN_WIDTH * (real32)vr.height / (real32)vr.width;
    u = (local.x + t * localDir.x) / VR_SCREEN_WIDTH + 0.5f;
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

static bool32 vrActionPressed(XrAction action)
{
    XrActionStateGetInfo getInfo;
    XrActionStateBoolean state;
    uword hand;

    memset(&getInfo, 0, sizeof(getInfo));
    getInfo.type = XR_TYPE_ACTION_STATE_GET_INFO;
    getInfo.action = action;
    for (hand = 0; hand < VR_HAND_COUNT; hand++)
    {
        getInfo.subactionPath = vr.handPath[hand];
        memset(&state, 0, sizeof(state));
        state.type = XR_TYPE_ACTION_STATE_BOOLEAN;
        if (XR_SUCCEEDED(xrGetActionStateBoolean(vr.session, &getInfo, &state))
            && state.isActive && state.currentState)
        {
            return TRUE;
        }
    }
    return FALSE;
}

static void vrUpdateInput(XrTime time)
{
    XrActiveActionSet activeSet;
    XrActionsSyncInfo syncInfo;
    sdword px, py;
    bool32 select, context, back;

    if (vr.state != XR_SESSION_STATE_FOCUSED || !vr.quadPlaced)
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

    /* right hand wins when both point at the screen */
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

    select = vrActionPressed(vr.selectAction);
    context = vrActionPressed(vr.contextAction);
    back = vrActionPressed(vr.backAction);

    if (select != vr.prevSelect && (vr.pointerValid || !select))
    {
        vrPushMouseButton(SDL_BUTTON_LEFT, select);
        vr.prevSelect = select;
    }
    if (context != vr.prevContext && (vr.pointerValid || !context))
    {
        vrPushMouseButton(SDL_BUTTON_RIGHT, context);
        vr.prevContext = context;
    }
    if (back != vr.prevBack)
    {
        vrPushKey(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, back);
        vr.prevBack = back;
    }
}

bool32 vrInit(sdword width, sdword height)
{
    memset(&vr, 0, sizeof(vr));
    vr.width = width;
    vr.height = height;
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
    XrCompositionLayerBaseHeader const* layers[1];
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

        if (!vr.quadPlaced)
        {
            vrPlaceQuad(frameState.predictedDisplayTime);
        }
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

            memset(&quad, 0, sizeof(quad));
            quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
            quad.space = vr.space;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = vr.swapchain;
            quad.subImage.imageRect.extent.width = vr.width;
            quad.subImage.imageRect.extent.height = vr.height;
            if (vr.quadPlaced)
            {
                quad.pose = vr.quadPose;
            }
            else
            {
                quad.pose.orientation.w = 1.0f;
                quad.pose.position.z = -VR_SCREEN_DISTANCE;
            }
            quad.size.width = VR_SCREEN_WIDTH;
            quad.size.height = VR_SCREEN_WIDTH * (real32)vr.height / (real32)vr.width;
            layers[0] = (XrCompositionLayerBaseHeader const*)&quad;
            layerCount = 1;
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
    }
    if (vr.actionSet != XR_NULL_HANDLE)
    {
        xrDestroyActionSet(vr.actionSet);
    }
    if (vr.swapchain != XR_NULL_HANDLE)
    {
        xrDestroySwapchain(vr.swapchain);
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
