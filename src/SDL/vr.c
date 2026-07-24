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

#define VR_MAX_SWAPCHAIN_IMAGES 8

/* Virtual screen: ~2m away, 4:3 like the game's default resolution */
#define VR_SCREEN_DISTANCE  2.0f
#define VR_SCREEN_WIDTH     2.4f

typedef struct {
    XrInstance   instance;
    XrSystemId   systemId;
    XrSession    session;
    XrSpace      space;
    XrSwapchain  swapchain;
    uint32_t     imageCount;
    XrSwapchainImageOpenGLESKHR images[VR_MAX_SWAPCHAIN_IMAGES];
    sdword       width, height;
    XrSessionState state;
    bool32       sessionRunning;
    bool32       active;
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

    return TRUE;
}

static bool32 vrCreateSwapchain(void)
{
    int64_t formats[64];
    uint32_t formatCount = 0, i;
    int64_t chosen = 0;
    XrSwapchainCreateInfo createInfo;

    VR_CHECK("xrEnumerateSwapchainFormats",
             xrEnumerateSwapchainFormats(vr.session, 64, &formatCount, formats));

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

bool32 vrInit(sdword width, sdword height)
{
    memset(&vr, 0, sizeof(vr));
    vr.width = width;
    vr.height = height;
    vr.state = XR_SESSION_STATE_UNKNOWN;

    if (!vrInitLoader() || !vrCreateInstance() || !vrCreateSession() || !vrCreateSwapchain())
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
    GLint previous = 0;

    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
    glBindTexture(GL_TEXTURE_2D, vr.images[imageIndex].image);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, vr.width, vr.height);
    glBindTexture(GL_TEXTURE_2D, (GLuint)previous);
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
            quad.pose.orientation.w = 1.0f;
            quad.pose.position.z = -VR_SCREEN_DISTANCE;
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
    xrEndFrame(vr.session, &endInfo);
}

void vrShutdown(void)
{
    if (vr.swapchain != XR_NULL_HANDLE)
    {
        xrDestroySwapchain(vr.swapchain);
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
