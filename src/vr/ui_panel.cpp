#include "vr/ui_panel.h"

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "game/game.h"
#include "ue/call.h"
#include "ue/objects.h"
#include "ue/reflection.h"
#include "vr/dxgi_hooks.h"
#include "vr/input.h"
#include "vr/openxr.h"
#include "vr/world_menu.h"

namespace holevr::vr::ui_panel {
namespace {
using ue::UObject;

template <typename T>
T Proc(const char* name) {
    return reinterpret_cast<T>(xr::GetProc(name));
}

// ------------------------------------------------------------- settings --

struct Settings {
    bool enabled = true;
    float width_m = 1.6f;      // panel width; height follows the window's shape
    float distance_m = 1.6f;   // from your head when the menu opens
    float drop_m = 0.1f;       // centre this far below eye height
    float follow_deg = 60.f;   // look further away than this and the panel comes back in front
    bool pause_button = true;  // controller menu button sends Escape
} S;

// ----------------------------------------------------- menu detection --
// The game shows the mouse cursor whenever a menu wants mouse input. We read
// APlayerController::bShowMouseCursor every frame from the present thread;
// the controller pointer and its class are refreshed once a second.

std::atomic<uintptr_t> g_pc{0};
std::atomic<uintptr_t> g_pc_class{0};
int32_t g_cursor_offset = -1;
uint8_t g_cursor_mask = 0;
UObject g_pc_base_class = 0;
std::atomic<bool> g_menu_open{false};
DWORD g_opened_at = 0;

bool ReadMenuOpen() {
    uintptr_t pc = g_pc.load();
    if (!pc || g_cursor_offset < 0) return false;
    if (mem::ReadOr<uintptr_t>(pc + ue::off::kObjClass) != g_pc_class.load()) return false;
    return (mem::ReadOr<uint8_t>(pc + g_cursor_offset) & g_cursor_mask) != 0;
}

// --------------------------------------------------------------- window --

HWND g_hwnd = nullptr;

BOOL CALLBACK FindWindowProc(HWND w, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
    wchar_t cls[64];
    GetClassNameW(w, cls, 64);
    if (wcscmp(cls, L"UnrealWindow") != 0) return TRUE;
    g_hwnd = w;
    return FALSE;
}

HWND GameWindow() {
    if (!g_hwnd || !IsWindow(g_hwnd)) {
        g_hwnd = nullptr;
        EnumWindows(FindWindowProc, 0);
    }
    return g_hwnd;
}

// ------------------------------------------------------------ the layer --

struct Panel {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D12KHR> images;
    uint32_t width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN, source_format = DXGI_FORMAT_UNKNOWN;
    XrPosef pose{{0, 0, 0, 1}, {0, 0, 0}};
    bool placed = false;
    XrCompositionLayerQuad layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
} P;

ID3D12CommandQueue* g_queue = nullptr;
ID3D12CommandAllocator* g_alloc[3] = {};
ID3D12GraphicsCommandList* g_cmd = nullptr;
ID3D12Fence* g_fence = nullptr;
HANDLE g_fence_event = nullptr;
uint64_t g_fence_value = 0, g_alloc_value[3] = {};
int g_slot = 0;
ID3D12DescriptorHeap* g_rtv_heap = nullptr;
UINT g_rtv_size = 0;
bool g_gpu_failed = false;

DXGI_FORMAT Srgb(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        default: return f;
    }
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER t{};
    t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    t.Transition.pResource = r;
    t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    t.Transition.StateBefore = a;
    t.Transition.StateAfter = b;
    return t;
}

bool InitGpu() {
    if (g_cmd) return true;
    if (g_gpu_failed) return false;
    ID3D12Device* dev = xr::Device();
    g_queue = dxgi_hooks::GameQueue();
    if (!dev || !g_queue) return false;
    bool ok = true;
    for (auto& a : g_alloc)
        ok = ok && SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)));
    ok = ok && SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr,
                                                IID_PPV_ARGS(&g_cmd)));
    if (ok) g_cmd->Close();
    ok = ok && SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)));
    g_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ok) {
        g_gpu_failed = true;
        log::Error("menu panel: could not create GPU objects, menus stay on the monitor");
    }
    return ok;
}

bool CreateSwapchain(ID3D12Resource* source) {
    D3D12_RESOURCE_DESC d = source->GetDesc();
    XrSession session = xr::Session();
    auto enum_formats = Proc<PFN_xrEnumerateSwapchainFormats>("xrEnumerateSwapchainFormats");
    auto create = Proc<PFN_xrCreateSwapchain>("xrCreateSwapchain");
    auto destroy = Proc<PFN_xrDestroySwapchain>("xrDestroySwapchain");
    auto enum_images = Proc<PFN_xrEnumerateSwapchainImages>("xrEnumerateSwapchainImages");
    if (!enum_formats || !create || !destroy || !enum_images) return false;
    uint32_t n = 0;
    enum_formats(session, 0, &n, nullptr);
    std::vector<int64_t> formats(n);
    enum_formats(session, n, &n, formats.data());
    auto supported = [&](DXGI_FORMAT f) {
        return std::find(formats.begin(), formats.end(), static_cast<int64_t>(f)) != formats.end();
    };
    // The window image is gamma-encoded; an sRGB swapchain shows it as-is.
    DXGI_FORMAT want = Srgb(d.Format);
    DXGI_FORMAT fmt = supported(want) ? want : supported(d.Format) ? d.Format : DXGI_FORMAT_UNKNOWN;
    if (fmt == DXGI_FORMAT_UNKNOWN) {
        log::Error("menu panel: runtime can't take the window's format %d", static_cast<int>(d.Format));
        g_gpu_failed = true;
        return false;
    }
    if (P.swapchain) destroy(P.swapchain);
    P = Panel{};
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = fmt;
    ci.sampleCount = 1;
    ci.width = static_cast<uint32_t>(d.Width);
    ci.height = d.Height;
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    if (XR_FAILED(create(session, &ci, &P.swapchain))) {
        log::Error("menu panel: xrCreateSwapchain failed");
        g_gpu_failed = true;
        return false;
    }
    enum_images(P.swapchain, 0, &n, nullptr);
    P.images.assign(n, XrSwapchainImageD3D12KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    enum_images(P.swapchain, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(P.images.data()));
    P.width = ci.width;
    P.height = ci.height;
    P.format = fmt;
    P.source_format = d.Format;

    // One render-target view per image, used only to draw the pointer dot.
    ID3D12Device* dev = xr::Device();
    if (g_rtv_heap) g_rtv_heap->Release();
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, n, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    g_rtv_heap = nullptr;
    if (SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtv_heap)))) {
        g_rtv_size = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = fmt;
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
        for (uint32_t i = 0; i < n; ++i, h.ptr += g_rtv_size) dev->CreateRenderTargetView(P.images[i].texture, &rd, h);
    }
    log::Info("menu panel: %ux%u swapchain, format %d (window format %d), %u images", P.width, P.height,
              static_cast<int>(fmt), static_cast<int>(d.Format), n);
    return true;
}

// Copies the window into the panel and draws the pointer dot (u,v in 0..1,
// negative = no dot).
bool Fill(ID3D12Resource* source, uint32_t image, float u, float v, bool pressed) {
    int slot = g_slot;
    g_slot = (g_slot + 1) % 3;
    if (g_fence->GetCompletedValue() < g_alloc_value[slot]) {
        g_fence->SetEventOnCompletion(g_alloc_value[slot], g_fence_event);
        WaitForSingleObject(g_fence_event, 100);
    }
    g_alloc[slot]->Reset();
    g_cmd->Reset(g_alloc[slot], nullptr);
    ID3D12Resource* dst = P.images[image].texture;
    D3D12_RESOURCE_BARRIER before[2] = {
        Transition(source, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE),
        Transition(dst, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST)};
    g_cmd->ResourceBarrier(2, before);
    g_cmd->CopyResource(dst, source);
    D3D12_RESOURCE_BARRIER after[2] = {
        Transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT),
        Transition(dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET)};
    g_cmd->ResourceBarrier(2, after);
    if (u >= 0.f && g_rtv_heap) {
        // A ring: a dark square with a bright centre, readable over any menu.
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += image * g_rtv_size;
        LONG cx = static_cast<LONG>(u * P.width), cy = static_cast<LONG>(v * P.height);
        LONG r = std::max<LONG>(4, static_cast<LONG>(P.height / 140));
        D3D12_RECT outer{cx - r - 2, cy - r - 2, cx + r + 2, cy + r + 2};
        D3D12_RECT inner{cx - r, cy - r, cx + r, cy + r};
        const float dark[4] = {0.f, 0.f, 0.f, 1.f};
        const float lit[4] = {pressed ? 1.f : 0.2f, pressed ? 0.6f : 0.9f, pressed ? 0.1f : 1.f, 1.f};
        g_cmd->ClearRenderTargetView(h, dark, 1, &outer);
        g_cmd->ClearRenderTargetView(h, lit, 1, &inner);
    }
    if (FAILED(g_cmd->Close())) return false;
    ID3D12CommandList* lists[] = {g_cmd};
    g_queue->ExecuteCommandLists(1, lists);
    g_queue->Signal(g_fence, ++g_fence_value);
    g_alloc_value[slot] = g_fence_value;
    return true;
}

// ----------------------------------------------------------- placement --

struct V3 {
    float x, y, z;
};
V3 Add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 Mul(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 FromXr(XrVector3f v) { return {v.x, v.y, v.z}; }
// Rotate v by unit quaternion q.
V3 Rotate(XrQuaternionf q, V3 v) {
    V3 u{q.x, q.y, q.z};
    V3 t = Mul(V3{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x}, 2.f);
    return Add(Add(v, Mul(t, q.w)), V3{u.y * t.z - u.z * t.y, u.z * t.x - u.x * t.z, u.x * t.y - u.y * t.x});
}

bool HeadPose(V3* pos, float* yaw) {
    xr::EyeView eyes[2];
    if (!xr::LatestViews(eyes)) return false;
    *pos = Mul(Add(FromXr(eyes[0].pose.position), FromXr(eyes[1].pose.position)), 0.5f);
    // OpenXR looks down -Z; yaw about +Y.
    V3 fwd = Rotate(eyes[0].pose.orientation, V3{0, 0, -1});
    *yaw = std::atan2(-fwd.x, -fwd.z);
    return true;
}

void PlaceInFront(V3 head, float yaw) {
    V3 fwd{-std::sin(yaw), 0.f, -std::cos(yaw)};
    V3 c = Add(head, Mul(fwd, S.distance_m));
    c.y -= S.drop_m;
    P.pose.position = {c.x, c.y, c.z};
    // Quad faces +Z; turn it by yaw so it faces the head.
    P.pose.orientation = {0.f, std::sin(yaw * 0.5f), 0.f, std::cos(yaw * 0.5f)};
    P.placed = true;
}

// ------------------------------------------------------------- pointer --

struct Pointer {
    bool trigger_down[2] = {};
    bool back_down[2] = {};
    bool menu_down[2] = {};
    int active_hand = input::kRight;
    float scroll_accum = 0.f;
    bool mouse_down = false;
} PT;

void PostKey(HWND w, WPARAM vk) {
    PostMessageW(w, WM_KEYDOWN, vk, 1);
    PostMessageW(w, WM_KEYUP, vk, (1u << 31) | (1u << 30) | 1);
}

// Ray from a hand against the panel. Returns UV in 0..1 when it hits.
bool HitPanel(const input::HandInput& h, float* u, float* v) {
    if (!h.aim_valid || !P.placed) return false;
    V3 origin = FromXr(h.aim.position);
    V3 dir = Rotate(h.aim.orientation, V3{0, 0, -1});
    V3 normal = Rotate(P.pose.orientation, V3{0, 0, 1});
    V3 centre = FromXr(P.pose.position);
    float denom = Dot(dir, normal);
    if (denom > -1e-4f) return false;  // pointing away or edge-on
    float t = Dot(Sub(centre, origin), normal) / denom;
    if (t <= 0.f || t > 20.f) return false;
    V3 local = Sub(Add(origin, Mul(dir, t)), centre);
    V3 right = Rotate(P.pose.orientation, V3{1, 0, 0});
    V3 up = Rotate(P.pose.orientation, V3{0, 1, 0});
    float w = P.layer.size.width, hgt = P.layer.size.height;
    *u = Dot(local, right) / w + 0.5f;
    *v = 0.5f - Dot(local, up) / hgt;
    return *u >= 0.f && *u <= 1.f && *v >= 0.f && *v <= 1.f;
}

// Moves the Windows cursor and posts mouse messages to the game window.
void DriveMouse(HWND w, float u, float v, bool down, float scroll) {
    RECT rc;
    if (!GetClientRect(w, &rc)) return;
    POINT p{static_cast<LONG>(u * (rc.right - rc.left)), static_cast<LONG>(v * (rc.bottom - rc.top))};
    LPARAM client = MAKELPARAM(p.x, p.y);
    POINT screen = p;
    ClientToScreen(w, &screen);
    SetCursorPos(screen.x, screen.y);
    PostMessageW(w, WM_MOUSEMOVE, down ? MK_LBUTTON : 0, client);
    if (down != PT.mouse_down) {
        PostMessageW(w, down ? WM_LBUTTONDOWN : WM_LBUTTONUP, down ? MK_LBUTTON : 0, client);
        PT.mouse_down = down;
    }
    if (scroll != 0.f) PostMessageW(w, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<short>(scroll)),
                                    MAKELPARAM(screen.x, screen.y));
}

// ------------------------------------------------------------- listener --

struct UiListener : xr::Listener {
    void AddLayers(XrSession session, XrSpace space, XrTime,
                   std::vector<const XrCompositionLayerBaseHeader*>& layers) override {
        bool open = ReadMenuOpen();
        if (open != g_menu_open.load()) {
            g_menu_open = open;
            log::Info("menu panel: menu %s", open ? "opened" : "closed");
            if (open) {
                P.placed = false;
                g_opened_at = GetTickCount();
            }
            if (!open && PT.mouse_down) {
                if (HWND w = GameWindow()) PostMessageW(w, WM_LBUTTONUP, 0, 0);
                PT.mouse_down = false;
            }
        }
        input::Snapshot in = input::Peek();
        HWND w = GameWindow();

        // Menu button on either controller toggles the pause menu (Escape).
        // A half-second guard so a double press doesn't open and close it.
        static DWORD s_last_menu = 0;
        for (int hnd = 0; hnd < 2; ++hnd) {
            bool m = in.valid && in.hand[hnd].menu.down;
            if (S.pause_button && m && !PT.menu_down[hnd] && w && GetTickCount() - s_last_menu > 500) {
                PostKey(w, VK_ESCAPE);
                s_last_menu = GetTickCount();
            }
            PT.menu_down[hnd] = m;
        }
        if (!open || !S.enabled) return;

        // B/Y goes back (Escape).
        if (in.valid) {
            for (int hnd = 0; hnd < 2; ++hnd) {
                bool back = in.hand[hnd].secondary.down;
                if (back && !PT.back_down[hnd] && w) PostKey(w, VK_ESCAPE);
                PT.back_down[hnd] = back;
            }
        }

        // Menus on their own panels in the world (vr/world_menu) replace this
        // flat copy of the screen. It only comes back if no menu could be
        // moved there.
        // Only the title screen still uses it; every other menu goes on the
        // forearm and never gets the flat box.
        if (world_menu::Enabled()) {
            if (!world_menu::TitleUp() || world_menu::Active() || GetTickCount() - g_opened_at < 300) {
                if (PT.mouse_down && w) PostMessageW(w, WM_LBUTTONUP, 0, 0);
                PT.mouse_down = false;
                return;
            }
            static bool s_said = false;
            if (!s_said) {
                s_said = true;
                log::Info("menu panel: title screen on the flat panel");
            }
        }

        ID3D12Resource* source = xr::WindowTexture();
        if (!source || !InitGpu()) return;
        D3D12_RESOURCE_DESC d = source->GetDesc();
        if ((d.Width != P.width || d.Height != P.height || d.Format != P.source_format) && !CreateSwapchain(source))
            return;

        V3 head;
        float yaw;
        if (HeadPose(&head, &yaw)) {
            if (!P.placed) PlaceInFront(head, yaw);
            else {
                // Came back in front if you turn well away from it.
                V3 to = Sub(FromXr(P.pose.position), head);
                float panel_yaw = std::atan2(-to.x, -to.z);
                float diff = std::fabs(std::remainder(panel_yaw - yaw, 6.2831853f));
                if (diff > S.follow_deg * 0.0174533f) PlaceInFront(head, yaw);
            }
        }
        P.layer.size.width = S.width_m;
        P.layer.size.height = S.width_m * static_cast<float>(P.height) / static_cast<float>(P.width);

        // Pointer: the hand whose trigger was pulled last.
        float u = -1.f, v = -1.f;
        bool down = false;
        if (in.valid) {
            for (int hnd = 0; hnd < 2; ++hnd) {
                const auto& h = in.hand[hnd];
                bool t = h.trigger > (PT.trigger_down[hnd] ? 0.35f : 0.6f);
                if (t && !PT.trigger_down[hnd]) PT.active_hand = hnd;
                PT.trigger_down[hnd] = t;
            }
            const auto& h = in.hand[PT.active_hand];
            if (HitPanel(h, &u, &v)) {
                down = PT.trigger_down[PT.active_hand];
                // A tick in the hand on each click, so you feel it land.
                if (down && !PT.mouse_down) input::Haptic(static_cast<input::Hand>(PT.active_hand), 0.3f, 0.02f, 200.f);
                // Thumbstick up/down scrolls lists, 3 notches a second at full tilt.
                PT.scroll_accum += h.stick_y * 120.f * 3.f / 90.f;
                float notch = 0.f;
                if (std::fabs(PT.scroll_accum) >= 120.f) {
                    notch = PT.scroll_accum > 0 ? 120.f : -120.f;
                    PT.scroll_accum -= notch;
                }
                if (std::fabs(h.stick_y) < 0.2f) PT.scroll_accum = 0.f;
                if (w) DriveMouse(w, u, v, down, notch);
            } else {
                u = v = -1.f;
                if (PT.mouse_down && w) DriveMouse(w, 0.5f, 0.5f, false, 0.f);
            }
        }

        auto acquire = Proc<PFN_xrAcquireSwapchainImage>("xrAcquireSwapchainImage");
        auto wait = Proc<PFN_xrWaitSwapchainImage>("xrWaitSwapchainImage");
        auto release = Proc<PFN_xrReleaseSwapchainImage>("xrReleaseSwapchainImage");
        uint32_t index = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(acquire(P.swapchain, &ai, &index))) return;
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = 100000000;
        bool ok = XR_SUCCEEDED(wait(P.swapchain, &wi)) && Fill(source, index, u, v, down);
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        release(P.swapchain, &ri);
        if (!ok) return;

        P.layer.space = space;
        P.layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        P.layer.subImage.swapchain = P.swapchain;
        P.layer.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(P.width), static_cast<int32_t>(P.height)}};
        P.layer.pose = P.pose;
        layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&P.layer));
    }

    void OnSessionEnding() override {
        if (P.swapchain) {
            if (auto destroy = Proc<PFN_xrDestroySwapchain>("xrDestroySwapchain")) destroy(P.swapchain);
        }
        P = Panel{};
    }
} g_listener;

}  // namespace

void Init() {
    S.enabled = config::GetBool("menu_panel", true);
    S.width_m = config::GetFloat("menu_panel_width_m", 1.6f);
    S.distance_m = config::GetFloat("menu_panel_distance_m", 1.6f);
    S.drop_m = config::GetFloat("menu_panel_drop_m", 0.1f);
    S.follow_deg = config::GetFloat("menu_panel_follow_deg", 60.f);
    S.pause_button = config::GetBool("menu_button_pauses", true);
    xr::RegisterListener(&g_listener);
    log::Info("menu panel: %s, %.2f m wide at %.2f m", S.enabled ? "on" : "off", S.width_m, S.distance_m);
}

void Maintain() {
    if (!ue::reflection::Ready()) return;
    if (!g_pc_base_class) {
        g_pc_base_class = ue::FindObject("Class /Script/Engine.PlayerController");
        if (!g_pc_base_class) return;
        auto prop = ue::reflection::FindProperty(g_pc_base_class, "bShowMouseCursor");
        if (!prop.valid() || !prop.bool_mask) {
            log::Warn("menu panel: PlayerController.bShowMouseCursor not found; menus stay on the monitor");
            g_pc_base_class = 0;
            return;
        }
        g_cursor_offset = prop.offset;
        g_cursor_mask = prop.bool_mask;
    }
    // The player's controller when there is a player, else any live one (title screen).
    UObject pc = 0;
    if (UObject player = game::Player()) ue::GetProp(player, "Controller", pc);
    uintptr_t cur = g_pc.load();
    if (!pc && cur && mem::ReadOr<uintptr_t>(cur + ue::off::kObjClass) == g_pc_class.load()) return;
    if (!pc) pc = ue::FindFirstInstance(g_pc_base_class);
    if (pc != cur) {
        g_pc_class = pc ? ue::GetClass(pc) : 0;
        g_pc = pc;
        if (pc) log::Info("menu panel: watching %s", ue::GetName(ue::GetClass(pc)).c_str());
    }
}

bool MenuOpen() { return g_menu_open.load(); }

}  // namespace holevr::vr::ui_panel
