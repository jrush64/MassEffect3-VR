#pragma once

// Hand-coded OpenXR 1.0 types (XR_KHR_D3D11_enable), lifted VERBATIM from the research tree
// where they are PROVEN to submit frames to a real headset. No OpenXR SDK dependency - the
// loader (openxr_loader.dll) is loaded at runtime. Do not "tidy" these: the layouts must match
// the OpenXR ABI exactly. Source: research MELEVR/openxr_probe.cpp (tag research-archive-2026-06-15).

#include <Windows.h>
#include <d3d11.h>

#include <cstdint>

namespace MELEVR::Xr
{
// ---- constants / enum values -------------------------------------------------------------
constexpr int32_t XR_SUCCESS_VALUE = 0;
constexpr int32_t XR_EVENT_UNAVAILABLE_VALUE = 4;
constexpr int32_t XR_ERROR_FORM_FACTOR_UNAVAILABLE_VALUE = -35;
constexpr int32_t XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED_VALUE = -41;
constexpr int32_t XR_ERROR_RUNTIME_UNAVAILABLE_VALUE = -51;
constexpr uint32_t XR_TYPE_EXTENSION_PROPERTIES_VALUE = 2;
constexpr uint32_t XR_TYPE_INSTANCE_CREATE_INFO_VALUE = 3;
constexpr uint32_t XR_TYPE_SYSTEM_GET_INFO_VALUE = 4;
constexpr uint32_t XR_TYPE_VIEW_LOCATE_INFO_VALUE = 6;
constexpr uint32_t XR_TYPE_VIEW_VALUE = 7;
constexpr uint32_t XR_TYPE_SESSION_CREATE_INFO_VALUE = 8;
constexpr uint32_t XR_TYPE_SWAPCHAIN_CREATE_INFO_VALUE = 9;
constexpr uint32_t XR_TYPE_SESSION_BEGIN_INFO_VALUE = 10;
constexpr uint32_t XR_TYPE_VIEW_STATE_VALUE = 11;
constexpr uint32_t XR_TYPE_FRAME_END_INFO_VALUE = 12;
constexpr uint32_t XR_TYPE_EVENT_DATA_BUFFER_VALUE = 16;
constexpr uint32_t XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED_VALUE = 18;
constexpr uint32_t XR_TYPE_INSTANCE_PROPERTIES_VALUE = 32;
constexpr uint32_t XR_TYPE_FRAME_WAIT_INFO_VALUE = 33;
constexpr uint32_t XR_TYPE_COMPOSITION_LAYER_PROJECTION_VALUE = 35;
constexpr uint32_t XR_TYPE_REFERENCE_SPACE_CREATE_INFO_VALUE = 37;
constexpr uint32_t XR_TYPE_VIEW_CONFIGURATION_VIEW_VALUE = 41;
constexpr uint32_t XR_TYPE_FRAME_STATE_VALUE = 44;
constexpr uint32_t XR_TYPE_FRAME_BEGIN_INFO_VALUE = 46;
constexpr uint32_t XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW_VALUE = 48;
constexpr uint32_t XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO_VALUE = 55;
constexpr uint32_t XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO_VALUE = 56;
constexpr uint32_t XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE = 57;
constexpr uint32_t XR_TYPE_GRAPHICS_BINDING_D3D11_KHR_VALUE = 1000027000;
constexpr uint32_t XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR_VALUE = 1000027001;
constexpr uint32_t XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR_VALUE = 1000027002;
constexpr uint32_t XR_MAX_APPLICATION_NAME_SIZE_VALUE = 128;
constexpr uint32_t XR_MAX_ENGINE_NAME_SIZE_VALUE = 128;
constexpr uint32_t XR_MAX_EXTENSION_NAME_SIZE_VALUE = 128;
constexpr uint32_t XR_MAX_RUNTIME_NAME_SIZE_VALUE = 128;
constexpr uint32_t XR_MAX_EVENT_DATA_SIZE_VALUE = 4000;
constexpr int32_t XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY_VALUE = 1;
constexpr int32_t XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_VALUE = 2;
constexpr int32_t XR_REFERENCE_SPACE_TYPE_VIEW_VALUE = 1;
constexpr int32_t XR_REFERENCE_SPACE_TYPE_LOCAL_VALUE = 2;
constexpr int32_t XR_ENVIRONMENT_BLEND_MODE_OPAQUE_VALUE = 1;
constexpr int32_t XR_EYE_VISIBILITY_BOTH_VALUE = 0;
constexpr int32_t XR_EYE_VISIBILITY_LEFT_VALUE = 1;
constexpr int32_t XR_EYE_VISIBILITY_RIGHT_VALUE = 2;
constexpr uint32_t XR_TYPE_COMPOSITION_LAYER_QUAD_VALUE = 36;
constexpr int32_t XR_SESSION_STATE_READY_VALUE = 2;
constexpr int32_t XR_SESSION_STATE_STOPPING_VALUE = 6;
constexpr int32_t XR_SESSION_STATE_LOSS_PENDING_VALUE = 7;
constexpr int32_t XR_SESSION_STATE_EXITING_VALUE = 8;
constexpr int64_t XR_INFINITE_DURATION_VALUE = 0x7fffffffffffffffLL;
constexpr uint64_t XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT_VALUE = 0x00000001;
constexpr uint64_t XR_SWAPCHAIN_USAGE_SAMPLED_BIT_VALUE = 0x00000020;
constexpr uint64_t XR_VIEW_STATE_ORIENTATION_VALID_BIT_VALUE = 0x00000001;
constexpr uint64_t XR_VIEW_STATE_POSITION_VALID_BIT_VALUE = 0x00000002;

constexpr char kD3D11ExtensionName[] = "XR_KHR_D3D11_enable";
constexpr char kDisplayRefreshRateExtensionName[] = "XR_FB_display_refresh_rate";

// ---- handle / scalar aliases -------------------------------------------------------------
using XrVersion = uint64_t;
using XrFlags64 = uint64_t;
using XrResult = int32_t;
using XrStructureType = uint32_t;
using XrBool32 = uint32_t;
using XrTime = int64_t;
using XrDuration = int64_t;
using XrFormFactor = int32_t;
using XrEnvironmentBlendMode = int32_t;
using XrReferenceSpaceType = int32_t;
using XrSessionState = int32_t;
using XrViewStateFlags = XrFlags64;
using XrInstanceCreateFlags = XrFlags64;
using XrSessionCreateFlags = XrFlags64;
using XrSwapchainCreateFlags = XrFlags64;
using XrSwapchainUsageFlags = XrFlags64;
using XrInstance = struct XrInstance_T*;
using XrSession = struct XrSession_T*;
using XrSpace = struct XrSpace_T*;
using XrSwapchain = struct XrSwapchain_T*;
using XrSystemId = uint64_t;
using XrViewConfigurationType = int32_t;
using XrEyeVisibility = int32_t;
using PFN_xrVoidFunction = void (*)();

// ---- structures (ABI-exact) --------------------------------------------------------------
struct XrExtensionProperties { XrStructureType type; void* next; char extensionName[XR_MAX_EXTENSION_NAME_SIZE_VALUE]; uint32_t extensionVersion; };
struct XrApplicationInfo { char applicationName[XR_MAX_APPLICATION_NAME_SIZE_VALUE]; uint32_t applicationVersion; char engineName[XR_MAX_ENGINE_NAME_SIZE_VALUE]; uint32_t engineVersion; XrVersion apiVersion; };
struct XrInstanceCreateInfo { XrStructureType type; const void* next; XrInstanceCreateFlags createFlags; XrApplicationInfo applicationInfo; uint32_t enabledApiLayerCount; const char* const* enabledApiLayerNames; uint32_t enabledExtensionCount; const char* const* enabledExtensionNames; };
struct XrInstanceProperties { XrStructureType type; void* next; XrVersion runtimeVersion; char runtimeName[XR_MAX_RUNTIME_NAME_SIZE_VALUE]; };
using PFN_xrGetInstanceProperties = XrResult (*)(XrInstance, XrInstanceProperties*);
struct XrSystemGetInfo { XrStructureType type; const void* next; XrFormFactor formFactor; };
struct XrVector3f { float x; float y; float z; };
struct XrQuaternionf { float x; float y; float z; float w; };
struct XrPosef { XrQuaternionf orientation; XrVector3f position; };
struct XrReferenceSpaceCreateInfo { XrStructureType type; const void* next; XrReferenceSpaceType referenceSpaceType; XrPosef poseInReferenceSpace; };
struct XrViewConfigurationView { XrStructureType type; void* next; uint32_t recommendedImageRectWidth; uint32_t maxImageRectWidth; uint32_t recommendedImageRectHeight; uint32_t maxImageRectHeight; uint32_t recommendedSwapchainSampleCount; uint32_t maxSwapchainSampleCount; };
struct XrGraphicsRequirementsD3D11KHR { XrStructureType type; void* next; LUID adapterLuid; D3D_FEATURE_LEVEL minFeatureLevel; };
struct XrGraphicsBindingD3D11KHR { XrStructureType type; const void* next; ID3D11Device* device; };
struct XrSessionCreateInfo { XrStructureType type; const void* next; XrSessionCreateFlags createFlags; XrSystemId systemId; };
struct XrSwapchainCreateInfo { XrStructureType type; const void* next; XrSwapchainCreateFlags createFlags; XrSwapchainUsageFlags usageFlags; int64_t format; uint32_t sampleCount; uint32_t width; uint32_t height; uint32_t faceCount; uint32_t arraySize; uint32_t mipCount; };
struct XrSwapchainImageBaseHeader { XrStructureType type; void* next; };
struct XrSwapchainImageD3D11KHR { XrStructureType type; void* next; ID3D11Texture2D* texture; };
struct XrEventDataBuffer { XrStructureType type; const void* next; uint8_t varying[XR_MAX_EVENT_DATA_SIZE_VALUE]; };
struct XrEventDataSessionStateChanged { XrStructureType type; const void* next; XrSession session; XrSessionState state; XrTime time; };
struct XrSessionBeginInfo { XrStructureType type; const void* next; XrViewConfigurationType primaryViewConfigurationType; };
struct XrFrameWaitInfo { XrStructureType type; const void* next; };
struct XrFrameState { XrStructureType type; void* next; XrTime predictedDisplayTime; XrDuration predictedDisplayPeriod; XrBool32 shouldRender; };
struct XrFrameBeginInfo { XrStructureType type; const void* next; };
struct XrCompositionLayerBaseHeader { XrStructureType type; const void* next; XrFlags64 layerFlags; XrSpace space; };
struct XrFrameEndInfo { XrStructureType type; const void* next; XrTime displayTime; XrEnvironmentBlendMode environmentBlendMode; uint32_t layerCount; const XrCompositionLayerBaseHeader* const* layers; };
struct XrSwapchainImageAcquireInfo { XrStructureType type; const void* next; };
struct XrSwapchainImageWaitInfo { XrStructureType type; const void* next; XrDuration timeout; };
struct XrSwapchainImageReleaseInfo { XrStructureType type; const void* next; };
struct XrViewLocateInfo { XrStructureType type; const void* next; XrViewConfigurationType viewConfigurationType; XrTime displayTime; XrSpace space; };
struct XrViewState { XrStructureType type; void* next; XrViewStateFlags viewStateFlags; };
struct XrFovf { float angleLeft; float angleRight; float angleUp; float angleDown; };
struct XrView { XrStructureType type; void* next; XrPosef pose; XrFovf fov; };
struct XrOffset2Di { int32_t x; int32_t y; };
struct XrExtent2Di { int32_t width; int32_t height; };
struct XrRect2Di { XrOffset2Di offset; XrExtent2Di extent; };
struct XrSwapchainSubImage { XrSwapchain swapchain; XrRect2Di imageRect; uint32_t imageArrayIndex; };
struct XrCompositionLayerProjectionView { XrStructureType type; const void* next; XrPosef pose; XrFovf fov; XrSwapchainSubImage subImage; };
struct XrCompositionLayerProjection { XrStructureType type; const void* next; XrFlags64 layerFlags; XrSpace space; uint32_t viewCount; const XrCompositionLayerProjectionView* views; };
struct XrExtent2Df { float width; float height; };
struct XrCompositionLayerQuad { XrStructureType type; const void* next; XrFlags64 layerFlags; XrSpace space; XrEyeVisibility eyeVisibility; XrSwapchainSubImage subImage; XrPosef pose; XrExtent2Df size; };

// ---- function-pointer typedefs -----------------------------------------------------------
using PFN_xrGetInstanceProcAddr = XrResult (*)(XrInstance, const char*, PFN_xrVoidFunction*);
using PFN_xrEnumerateInstanceExtensionProperties = XrResult (*)(const char*, uint32_t, uint32_t*, XrExtensionProperties*);
using PFN_xrCreateInstance = XrResult (*)(const XrInstanceCreateInfo*, XrInstance*);
using PFN_xrDestroyInstance = XrResult (*)(XrInstance);
using PFN_xrPollEvent = XrResult (*)(XrInstance, XrEventDataBuffer*);
using PFN_xrGetSystem = XrResult (*)(XrInstance, const XrSystemGetInfo*, XrSystemId*);
using PFN_xrEnumerateViewConfigurationViews = XrResult (*)(XrInstance, XrSystemId, XrViewConfigurationType, uint32_t, uint32_t*, XrViewConfigurationView*);
using PFN_xrGetD3D11GraphicsRequirementsKHR = XrResult (*)(XrInstance, XrSystemId, XrGraphicsRequirementsD3D11KHR*);
using PFN_xrCreateSession = XrResult (*)(XrInstance, const XrSessionCreateInfo*, XrSession*);
using PFN_xrDestroySession = XrResult (*)(XrSession);
using PFN_xrEnumerateSwapchainFormats = XrResult (*)(XrSession, uint32_t, uint32_t*, int64_t*);
using PFN_xrCreateSwapchain = XrResult (*)(XrSession, const XrSwapchainCreateInfo*, XrSwapchain*);
using PFN_xrDestroySwapchain = XrResult (*)(XrSwapchain);
using PFN_xrEnumerateSwapchainImages = XrResult (*)(XrSwapchain, uint32_t, uint32_t*, XrSwapchainImageBaseHeader*);
using PFN_xrAcquireSwapchainImage = XrResult (*)(XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t*);
using PFN_xrWaitSwapchainImage = XrResult (*)(XrSwapchain, const XrSwapchainImageWaitInfo*);
using PFN_xrReleaseSwapchainImage = XrResult (*)(XrSwapchain, const XrSwapchainImageReleaseInfo*);
using PFN_xrBeginSession = XrResult (*)(XrSession, const XrSessionBeginInfo*);
using PFN_xrEndSession = XrResult (*)(XrSession);
using PFN_xrWaitFrame = XrResult (*)(XrSession, const XrFrameWaitInfo*, XrFrameState*);
using PFN_xrBeginFrame = XrResult (*)(XrSession, const XrFrameBeginInfo*);
using PFN_xrEndFrame = XrResult (*)(XrSession, const XrFrameEndInfo*);
using PFN_xrCreateReferenceSpace = XrResult (*)(XrSession, const XrReferenceSpaceCreateInfo*, XrSpace*);
using PFN_xrDestroySpace = XrResult (*)(XrSpace);
using PFN_xrLocateViews = XrResult (*)(XrSession, const XrViewLocateInfo*, XrViewState*, uint32_t, uint32_t*, XrView*);
using PFN_xrGetDisplayRefreshRateFB = XrResult (*)(XrSession, float*);

// Resolved per-instance/session function pointers (filled once after xrCreateInstance).
struct Functions
{
    PFN_xrDestroyInstance destroyInstance = nullptr;
    PFN_xrPollEvent pollEvent = nullptr;
    PFN_xrGetSystem getSystem = nullptr;
    PFN_xrEnumerateViewConfigurationViews enumerateViewConfigurationViews = nullptr;
    PFN_xrGetD3D11GraphicsRequirementsKHR getD3D11GraphicsRequirements = nullptr;
    PFN_xrCreateSession createSession = nullptr;
    PFN_xrDestroySession destroySession = nullptr;
    PFN_xrEnumerateSwapchainFormats enumerateSwapchainFormats = nullptr;
    PFN_xrCreateSwapchain createSwapchain = nullptr;
    PFN_xrDestroySwapchain destroySwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages enumerateSwapchainImages = nullptr;
    PFN_xrAcquireSwapchainImage acquireSwapchainImage = nullptr;
    PFN_xrWaitSwapchainImage waitSwapchainImage = nullptr;
    PFN_xrReleaseSwapchainImage releaseSwapchainImage = nullptr;
    PFN_xrBeginSession beginSession = nullptr;
    PFN_xrEndSession endSession = nullptr;
    PFN_xrWaitFrame waitFrame = nullptr;
    PFN_xrBeginFrame beginFrame = nullptr;
    PFN_xrEndFrame endFrame = nullptr;
    PFN_xrCreateReferenceSpace createReferenceSpace = nullptr;
    PFN_xrDestroySpace destroySpace = nullptr;
    PFN_xrLocateViews locateViews = nullptr;
    PFN_xrGetDisplayRefreshRateFB getDisplayRefreshRate = nullptr;
};

inline bool XrSucceeded(XrResult r) noexcept { return r >= 0; }
inline XrVersion MakeXrVersion(uint16_t major, uint16_t minor, uint32_t patch) noexcept
{
    return (static_cast<XrVersion>(major) << 48) | (static_cast<XrVersion>(minor) << 32) | static_cast<XrVersion>(patch);
}
inline XrPosef IdentityPose() noexcept { XrPosef p = {}; p.orientation.w = 1.0f; return p; }
}
