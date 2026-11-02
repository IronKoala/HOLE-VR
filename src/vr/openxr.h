#pragma once
// OpenXR runtime connection: loads the active runtime (SteamVR etc.) without
// the Khronos loader, owns the instance/session/swapchains and runs the frame
// loop from the game's swapchain Present.

#include <windows.h>
#include <d3d12.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <string>
#include <vector>

namespace holevr::vr::xr {

// Other systems (input, body tracking) plug in here instead of editing this file.
struct Listener {
    virtual ~Listener() = default;
    virtual void ExtraInstanceExtensions(std::vector<const char*>& extensions) {}
    virtual void OnInstanceCreated(XrInstance instance) {}
    virtual void OnSessionCreated(XrSession session, std::vector<XrActionSet>& sets_to_attach) {}
    // Once per frame after xrWaitFrame, on the thread that presents (render side).
    virtual void OnFrame(XrSession session, XrSpace tracking_space, XrTime predicted_display_time) {}
    // Extra composition layers (e.g. quad layers for menus), submitted on top
    // of the eye images in xrEndFrame. Pointers must stay valid until it returns.
    virtual void AddLayers(XrSession session, XrSpace tracking_space, XrTime display_time,
                           std::vector<const XrCompositionLayerBaseHeader*>& layers) {}
    virtual void OnSessionEnding() {}
};
void RegisterListener(Listener* listener);

// Any OpenXR function, resolved through the runtime. nullptr if unavailable.
PFN_xrVoidFunction GetProc(const char* name);

// Step 1, from the init thread: load the runtime, create the instance and
// find the headset. Returns false if no runtime/headset yet (call again later).
bool InitInstance();
bool InstanceReady();

// Library path of the active OpenXR runtime (from its registered manifest),
// e.g. <SteamVR>\bin\vrclient_x64.dll. "" if none is registered. Does not load it.
std::wstring ActiveRuntimeLibrary();

// Per-eye render size the runtime recommends (already scaled by settings).
void RecommendedEyeSize(uint32_t* width, uint32_t* height);

// Step 2, from the Present hook once the game's D3D12 queue is known.
// back_buffer is the window image about to be presented (fallback source).
void OnPresent(ID3D12CommandQueue* queue, ID3D12Resource* back_buffer);

// The texture the engine rendered both eyes into (left half / right half).
void SetEyeTexture(ID3D12Resource* texture);
void SetFallbackTexture(ID3D12Resource* texture);
// The game window's back buffer for the frame being presented (state
// PRESENT), with the desktop mirror and the game's menus/HUD on it. Only
// valid during AddLayers/OnFrame on the present thread.
ID3D12Resource* WindowTexture();

struct EyeView {
    XrPosef pose;  // in tracking space
    XrFovf fov;
};
// Latest predicted eye views. Returns false until tracking is running.
bool LatestViews(EyeView out[2]);
// Tell the frame loop which views the engine actually rendered with, so the
// headset reprojects from the right pose.
void SetRenderedViews(const EyeView views[2]);

bool SessionRunning();
// True once the headset is showing this game (session VISIBLE or FOCUSED).
bool SessionVisible();
XrInstance Instance();
XrSession Session();
XrSpace TrackingSpace();
// The game's D3D12 device (valid once the session exists).
ID3D12Device* Device();

// Saves the next picture sent to the headset (both eyes, half size) as
// <mod folder>/capture-<tag>.bmp. For checking what the player sees.
void RequestCapture(const char* tag);

}  // namespace holevr::vr::xr
