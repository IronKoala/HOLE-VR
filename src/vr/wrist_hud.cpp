#include "vr/wrist_hud.h"

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "game/game.h"
#include "game/gun.h"
#include "ue/call.h"
#include "ue/objects.h"
#include "ue/reflection.h"
#include "vr/dxgi_hooks.h"
#include "vr/input.h"
#include "vr/openxr.h"
#include "vr/ui_panel.h"

namespace holevr::vr::wrist_hud {
namespace {
using ue::UObject;

template <typename T>
T Proc(const char* name) {
    return reinterpret_cast<T>(xr::GetProc(name));
}

// ------------------------------------------------------------- settings --

struct Settings {
    bool enabled = true;
    int hand = input::kLeft;
    bool always = false;          // show without looking
    float show_distance_m = 0.5f; // wrist this close to the head...
    float show_angle_deg = 40.f;  // ...and the display facing you within this angle
    float width_m = 0.10f;
    float offset_back_m = 0.035f; // from the grip centre toward the back of the hand
    float offset_elbow_m = 0.10f; // and toward the elbow
    bool ammo = true;             // ammo count on the display (off = harder)
    bool hide_flat_hud = true;
    bool heartbeat = true;
    float heartbeat_below = 0.3f; // fraction of max health
} S;

// --------------------------------------------- game state (game thread) --

struct Status {
    bool valid = false;           // there is a player
    bool has_gun = false;
    int chamber = 0, magazine = -1, capacity = 0;
    bool has_magazine = false, jammed = false, slide_locked = false;
    double hp = 0, max_hp = 0;
    bool dead = false;
    bool operator==(const Status& o) const {
        return valid == o.valid && has_gun == o.has_gun && chamber == o.chamber && magazine == o.magazine &&
               capacity == o.capacity && has_magazine == o.has_magazine && jammed == o.jammed &&
               slide_locked == o.slide_locked && std::lround(hp) == std::lround(o.hp) &&
               std::lround(max_hp) == std::lround(o.max_hp) && dead == o.dead;
    }
};

std::mutex g_status_mutex;
Status g_status;

// The component holding health (BPC_StateManager_C), found by type among
// the player's properties so no offset is hardcoded.
UObject StateManager(UObject player) {
    static UObject s_class = 0;
    static int32_t s_offset = -1;
    UObject cls = ue::GetClass(player);
    if (cls != s_class) {
        s_class = cls;
        s_offset = -1;
        for (UObject c = cls; c && s_offset < 0; c = ue::reflection::GetSuper(c))
            for (const auto& p : ue::reflection::GetProperties(c))
                if (p.type == "ObjectProperty" && p.inner_type == "BPC_StateManager_C") {
                    s_offset = p.offset;
                    log::Info("wrist: health from %s.%s", ue::GetName(c).c_str(), p.name.c_str());
                    break;
                }
        if (s_offset < 0) log::Warn("wrist: no health component found on %s", ue::GetName(cls).c_str());
    }
    return s_offset >= 0 ? mem::ReadOr<UObject>(player + s_offset) : 0;
}

// ESlateVisibility
constexpr uint8_t kVisible = 0, kCollapsed = 1;
const char* const kHudParts[] = {"HUDP_Ammo",     "HUDP_BarrelHeat", "HUDP_CashData", "HUDP_Crosshair",
                                 "HUDP_DebugHP",  "HUDP_Fridge",     "HUDP_Interact", "HUDP_Light",
                                 "HUDP_NavIcon",  "HUDP_OBJ",        "HUDP_Popup",    "HUDP_ReloadHint",
                                 "HUDP_SaveProgress"};
// Parts the wrist display replaces.
bool Replaced(const std::string& part) {
    return part == "HUDP_Ammo" || part == "HUDP_BarrelHeat" || part == "HUDP_Crosshair" ||
           part == "HUDP_DebugHP" || part == "HUDP_ReloadHint";
}

// Hides the replaced parts and logs every part's visibility changes, so we
// learn in play when the game uses each one. Game thread.
void ManageFlatHud(UObject player) {
    static std::map<std::string, int> s_seen;  // part -> last logged visibility
    UObject hud = 0;
    if (!ue::GetProp(player, "HUD", hud) || !hud) return;
    for (const char* name : kHudParts) {
        UObject part = 0;
        if (!ue::GetProp(hud, name, part) || !part) continue;
        uint8_t vis = kVisible;
        ue::GetProp(part, "Visibility", vis);
        bool hide = S.hide_flat_hud && Replaced(name);
        // The game's own state is logged before we change it.
        auto it = s_seen.find(name);
        if (!hide && (it == s_seen.end() || it->second != vis)) {
            log::Info("hud: %s %s", name, vis == kCollapsed ? "collapsed" : vis == 2 ? "hidden" : "showing");
            s_seen[name] = vis;
        }
        if (hide && vis != kCollapsed) {
            ue::Call c(part, "SetVisibility");
            c.Set("InVisibility", kCollapsed);
            if (c.Invoke() && (it == s_seen.end() || it->second != kCollapsed)) {
                log::Info("hud: %s hidden (on the wrist instead)", name);
                s_seen[name] = kCollapsed;
            }
        }
    }
}

void OnTick(UObject player, float dt) {
    static float s_acc = 0.f, s_hud_acc = 1.f;
    s_acc += dt;
    s_hud_acc += dt;
    if (s_acc < 0.05f) return;  // twenty reads a second
    s_acc = 0.f;
    Status st;
    st.valid = player != 0;
    if (player) {
        game::Gun gun = game::Gun::Current(player);
        if (gun.Valid()) {
            game::GunState g = gun.State();
            st.has_gun = true;
            st.chamber = g.chamber;
            st.magazine = g.magazine;
            st.capacity = g.capacity;
            st.has_magazine = g.has_magazine;
            st.jammed = g.jammed;
            st.slide_locked = g.slide_locked;
        }
        if (UObject sm = StateManager(player)) {
            ue::GetProp(sm, "HP", st.hp);
            ue::GetProp(sm, "MaxHP", st.max_hp);
            ue::GetProp(sm, "IsDead", st.dead);
        }
        if (s_hud_acc >= 0.3f) {
            s_hud_acc = 0.f;
            ManageFlatHud(player);
        }
    }
    // Each change of the count is logged (the first 120), so a run's log
    // shows the display followed every shot (owner 2026-11-03: "the ammo
    // counter doesn't keep a live feed").
    static int s_logs = 0;
    static Status s_prev;
    if (s_logs < 120 && st.has_gun &&
        (st.chamber != s_prev.chamber || st.magazine != s_prev.magazine || st.has_magazine != s_prev.has_magazine ||
         st.has_gun != s_prev.has_gun)) {
        ++s_logs;
        log::Info("wrist: the game's ammo is now %s%d+%d (magazine+chamber)", st.has_magazine ? "" : "no magazine, ",
                  std::max(0, st.magazine), st.chamber);
    }
    s_prev = st;
    std::lock_guard<std::mutex> lock(g_status_mutex);
    g_status = st;
}

// -------------------------------------------------------------- drawing --
// The display is drawn with GDI into a small bitmap, then uploaded. It only
// changes when the numbers do.

constexpr int kTexW = 512, kTexH = 256;

struct Canvas {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    uint32_t* pixels = nullptr;  // BGRA, top-down
    HFONT big = nullptr, small_font = nullptr;
} C;

bool InitCanvas() {
    if (C.dc) return true;
    C.dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = kTexW;
    bi.bmiHeader.biHeight = -kTexH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    C.bmp = CreateDIBSection(C.dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!C.bmp) return false;
    C.pixels = static_cast<uint32_t*>(bits);
    SelectObject(C.dc, C.bmp);
    C.big = CreateFontW(-120, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0,
                        L"Segoe UI");
    C.small_font = CreateFontW(-44, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0,
                          L"Segoe UI");
    SetBkMode(C.dc, TRANSPARENT);
    return true;
}

void Text(const std::wstring& s, HFONT font, COLORREF color, RECT r, UINT flags) {
    SelectObject(C.dc, font);
    SetTextColor(C.dc, color);
    DrawTextW(C.dc, s.c_str(), -1, &r, flags | DT_SINGLELINE | DT_NOPREFIX);
}

// Renders the status and converts it into premultiplied BGRA: GDI draws
// coloured text on black, and each pixel's brightest channel becomes its
// coverage over a dark translucent backing.
void Draw(const Status& st, std::vector<uint32_t>& out) {
    std::memset(C.pixels, 0, kTexW * kTexH * 4);
    const COLORREF white = RGB(240, 240, 240), amber = RGB(255, 190, 40), red = RGB(255, 70, 60),
                   green = RGB(90, 230, 120), grey = RGB(150, 150, 150);
    // Ammo: magazine + chamber, the way you'd count it.
    std::wstring hint;
    if (st.has_gun && S.ammo) {
        wchar_t buf[32];
        if (st.has_magazine) swprintf(buf, 32, L"%d+%d", std::max(0, st.magazine), st.chamber);
        else swprintf(buf, 32, L"–+%d", st.chamber);
        bool empty = (!st.has_magazine || st.magazine <= 0) && st.chamber == 0;
        Text(buf, C.big, st.jammed ? red : empty ? amber : white, RECT{16, 0, 330, 150}, DT_LEFT | DT_VCENTER);
        if (st.capacity > 0 && st.has_magazine) {
            swprintf(buf, 32, L"/ %d", st.capacity);
            Text(buf, C.small_font, grey, RECT{330, 60, 500, 130}, DT_LEFT | DT_VCENTER);
        }
    }
    if (st.has_gun) {
        if (st.jammed) hint = L"JAMMED – rack the slide";
        else if (st.slide_locked && (!st.has_magazine || st.magazine <= 0)) hint = L"Empty – new magazine";
        else if (st.slide_locked) hint = L"Slide locked – release it";
        else if (!st.has_magazine && st.chamber == 0) hint = L"No magazine";
        else if (st.chamber == 0) hint = L"Chamber empty – rack it";
    }
    if (!hint.empty())
        Text(hint, C.small_font, st.jammed ? red : amber, RECT{16, 140, 500, 190}, DT_LEFT | DT_VCENTER);
    // Health bar.
    if (st.max_hp > 0) {
        double f = std::clamp(st.hp / st.max_hp, 0.0, 1.0);
        COLORREF col = f < S.heartbeat_below ? red : f < 0.6 ? amber : green;
        RECT back{16, 206, 496, 240};
        HBRUSH bg = CreateSolidBrush(RGB(60, 60, 60));
        FillRect(C.dc, &back, bg);
        DeleteObject(bg);
        RECT bar{16, 206, 16 + static_cast<LONG>(480 * f), 240};
        HBRUSH fg = CreateSolidBrush(col);
        FillRect(C.dc, &bar, fg);
        DeleteObject(fg);
    }
    GdiFlush();
    out.resize(kTexW * kTexH);
    // No box behind it: each pixel's brightest channel is its coverage, and a
    // soft dark outline around the text and bar keeps it readable over
    // anything.
    std::vector<float> cov(kTexW * kTexH), tmp(kTexW * kTexH), halo(kTexW * kTexH);
    for (int i = 0; i < kTexW * kTexH; ++i) {
        uint32_t p = C.pixels[i];
        cov[i] = std::max({(p & 0xFF), (p >> 8) & 0xFF, (p >> 16) & 0xFF}) / 255.f;
    }
    const int r = 5;
    for (int y = 0; y < kTexH; ++y)
        for (int x = 0; x < kTexW; ++x) {
            float m = 0;
            for (int k = std::max(0, x - r); k <= std::min(kTexW - 1, x + r); ++k) m = std::max(m, cov[y * kTexW + k]);
            tmp[y * kTexW + x] = m;
        }
    for (int y = 0; y < kTexH; ++y)
        for (int x = 0; x < kTexW; ++x) {
            float m = 0;
            for (int k = std::max(0, y - r); k <= std::min(kTexH - 1, y + r); ++k) m = std::max(m, tmp[k * kTexW + x]);
            halo[y * kTexW + x] = m;
        }
    for (int i = 0; i < kTexW * kTexH; ++i) {
        uint32_t p = C.pixels[i];
        float b = (p & 0xFF) / 255.f, g = ((p >> 8) & 0xFF) / 255.f, rr = ((p >> 16) & 0xFF) / 255.f;
        float a = std::max(cov[i], 0.6f * halo[i]);
        // Premultiplied: colour already carries its coverage; the outline is black.
        auto ch = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
        out[i] = ch(b) | (ch(g) << 8) | (ch(rr) << 16) | (ch(a) << 24);
    }
}

// ---------------------------------------------------------- the layer --

struct Layer {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D12KHR> images;
    bool bgra = true;
    bool has_image = false;
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
} L;

ID3D12CommandQueue* g_queue = nullptr;
ID3D12CommandAllocator* g_alloc = nullptr;
ID3D12GraphicsCommandList* g_cmd = nullptr;
ID3D12Fence* g_fence = nullptr;
HANDLE g_event = nullptr;
uint64_t g_fence_value = 0;
ID3D12Resource* g_upload = nullptr;
bool g_failed = false;
Status g_drawn;
bool g_drawn_any = false;

bool InitGpu() {
    if (g_cmd) return true;
    if (g_failed) return false;
    ID3D12Device* dev = xr::Device();
    g_queue = dxgi_hooks::GameQueue();
    if (!dev || !g_queue) return false;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = kTexW * kTexH * 4;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bool ok = SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc))) &&
              SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, nullptr,
                                               IID_PPV_ARGS(&g_cmd))) &&
              SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence))) &&
              SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&g_upload)));
    if (ok) g_cmd->Close();
    g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ok || !InitCanvas()) {
        g_failed = true;
        log::Error("wrist: could not create the display's GPU objects");
        return false;
    }
    return true;
}

bool CreateSwapchain(XrSession session) {
    auto enum_formats = Proc<PFN_xrEnumerateSwapchainFormats>("xrEnumerateSwapchainFormats");
    auto create = Proc<PFN_xrCreateSwapchain>("xrCreateSwapchain");
    auto enum_images = Proc<PFN_xrEnumerateSwapchainImages>("xrEnumerateSwapchainImages");
    uint32_t n = 0;
    enum_formats(session, 0, &n, nullptr);
    std::vector<int64_t> formats(n);
    enum_formats(session, n, &n, formats.data());
    auto has = [&](DXGI_FORMAT f) {
        return std::find(formats.begin(), formats.end(), static_cast<int64_t>(f)) != formats.end();
    };
    DXGI_FORMAT fmt = has(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
                      : has(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                                             : DXGI_FORMAT_UNKNOWN;
    if (fmt == DXGI_FORMAT_UNKNOWN) {
        log::Error("wrist: no 8-bit sRGB swapchain format available");
        g_failed = true;
        return false;
    }
    L.bgra = fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = fmt;
    ci.sampleCount = 1;
    ci.width = kTexW;
    ci.height = kTexH;
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    if (XR_FAILED(create(session, &ci, &L.swapchain))) {
        log::Error("wrist: xrCreateSwapchain failed");
        g_failed = true;
        return false;
    }
    enum_images(L.swapchain, 0, &n, nullptr);
    L.images.assign(n, XrSwapchainImageD3D12KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    enum_images(L.swapchain, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(L.images.data()));
    log::Info("wrist: display swapchain %dx%d format %d", kTexW, kTexH, static_cast<int>(fmt));
    return true;
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

bool Upload(const std::vector<uint32_t>& px) {
    // Wait for the previous upload before reusing the buffer and allocator;
    // if the GPU is still busy with it, try again next frame.
    if (g_fence->GetCompletedValue() < g_fence_value) {
        g_fence->SetEventOnCompletion(g_fence_value, g_event);
        WaitForSingleObject(g_event, 100);
        if (g_fence->GetCompletedValue() < g_fence_value) return false;
    }
    void* map = nullptr;
    D3D12_RANGE none{0, 0};
    if (FAILED(g_upload->Map(0, &none, &map))) return false;
    if (L.bgra) std::memcpy(map, px.data(), px.size() * 4);
    else {
        auto* dst = static_cast<uint32_t*>(map);
        for (size_t i = 0; i < px.size(); ++i) {
            uint32_t p = px[i];
            dst[i] = (p & 0xFF00FF00u) | ((p & 0xFF) << 16) | ((p >> 16) & 0xFF);
        }
    }
    g_upload->Unmap(0, nullptr);

    auto acquire = Proc<PFN_xrAcquireSwapchainImage>("xrAcquireSwapchainImage");
    auto wait = Proc<PFN_xrWaitSwapchainImage>("xrWaitSwapchainImage");
    auto release = Proc<PFN_xrReleaseSwapchainImage>("xrReleaseSwapchainImage");
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(acquire(L.swapchain, &ai, &index))) return false;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = 100000000;
    // XR_TIMEOUT_EXPIRED counts as success to XR_SUCCEEDED, but the image
    // isn't ours yet: wait again (the spec wants a successful wait first).
    XrResult wr = wait(L.swapchain, &wi);
    for (int i = 0; i < 5 && wr == XR_TIMEOUT_EXPIRED; ++i) wr = wait(L.swapchain, &wi);
    bool ok = wr == XR_SUCCESS;
    if (ok) {
        g_alloc->Reset();
        g_cmd->Reset(g_alloc, nullptr);
        ID3D12Resource* dst = L.images[index].texture;
        D3D12_RESOURCE_BARRIER b = Transition(dst, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
        g_cmd->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION to{};
        to.pResource = dst;
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION from{};
        from.pResource = g_upload;
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint.Footprint.Format = dst->GetDesc().Format;
        // sRGB/typeless resources: the footprint wants the resource's own format.
        from.PlacedFootprint.Footprint.Width = kTexW;
        from.PlacedFootprint.Footprint.Height = kTexH;
        from.PlacedFootprint.Footprint.Depth = 1;
        from.PlacedFootprint.Footprint.RowPitch = kTexW * 4;
        g_cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        g_cmd->ResourceBarrier(1, &b);
        ok = SUCCEEDED(g_cmd->Close());
        if (ok) {
            ID3D12CommandList* lists[] = {g_cmd};
            g_queue->ExecuteCommandLists(1, lists);
            g_queue->Signal(g_fence, ++g_fence_value);
        }
    }
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    release(L.swapchain, &ri);
    return ok;
}

// --------------------------------------------------------- placement --

struct V3 {
    float x, y, z;
};
V3 Add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 Mul(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float Len(V3 a) { return std::sqrt(Dot(a, a)); }
V3 Rotate(XrQuaternionf q, V3 v) {
    V3 u{q.x, q.y, q.z};
    V3 t = Mul(V3{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x}, 2.f);
    return Add(Add(v, Mul(t, q.w)), V3{u.y * t.z - u.z * t.y, u.z * t.x - u.x * t.z, u.x * t.y - u.y * t.x});
}

V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

// Rotation whose columns are the given orthonormal axes.
XrQuaternionf FromAxes(V3 x, V3 y, V3 z) {
    float tr = x.x + y.y + z.z;
    if (tr > 0) {
        float s = std::sqrt(tr + 1.f) * 2.f;
        return {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, 0.25f * s};
    }
    if (x.x > y.y && x.x > z.z) {
        float s = std::sqrt(1.f + x.x - y.y - z.z) * 2.f;
        return {0.25f * s, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
    }
    if (y.y > z.z) {
        float s = std::sqrt(1.f + y.y - x.x - z.z) * 2.f;
        return {(y.x + x.y) / s, 0.25f * s, (z.y + y.z) / s, (z.x - x.z) / s};
    }
    float s = std::sqrt(1.f + z.z - x.x - y.y) * 2.f;
    return {(z.x + x.z) / s, (z.y + y.z) / s, 0.25f * s, (x.y - y.x) / s};
}

// OpenXR grip space: -Z through the fist, +Y along the thumb, +X out of the
// back of the left hand (into the palm of the right). The display lies flat
// on the back of the hand. Its long side runs along the hand, and its top is
// whichever edge (thumb or little finger side) is higher in your view, so it
// always reads the right way up like a watch.
bool WristPose(const input::HandInput& h, XrPosef* pose) {
    if (!h.grip_valid) return false;
    // The forearm menu, placed with this same +X-is-back rule, showed up on the
    // owner's PALM side (2026-11-02, Steam Frame via SteamVR), so on this
    // runtime +X points out of the left palm: the back of the hand is -X.
    float side = S.hand == input::kLeft ? -1.f : 1.f;
    V3 local{side * S.offset_back_m, 0.f, S.offset_elbow_m};
    V3 p = Add(V3{h.grip.position.x, h.grip.position.y, h.grip.position.z}, Rotate(h.grip.orientation, local));
    V3 face = Rotate(h.grip.orientation, V3{side, 0, 0});
    V3 thumb = Rotate(h.grip.orientation, V3{0, 1, 0});
    xr::EyeView eyes[2];
    static float s_up_sign = 1.f;
    if (xr::LatestViews(eyes)) {
        V3 head_up = Rotate(eyes[0].pose.orientation, V3{0, 1, 0});
        float d = Dot(thumb, head_up) * s_up_sign;
        if (d < -0.25f) s_up_sign = -s_up_sign;  // a little hysteresis so it doesn't flip back and forth
    }
    V3 up = Mul(thumb, s_up_sign);
    // Read level, like a watch you've turned toward you: the display's up is
    // your head's up laid flat onto the back of the hand. Following the thumb
    // left it standing on end (owner, 2026-11-02: "vertical not horizontal").
    if (xr::LatestViews(eyes)) {
        V3 head_up = Rotate(eyes[0].pose.orientation, V3{0, 1, 0});
        V3 flat = Sub(head_up, Mul(face, Dot(head_up, face)));
        float len = Len(flat);
        if (len > 0.3f) up = Mul(flat, 1.f / len);
    }
    V3 right = Cross(up, face);
    pose->orientation = FromAxes(right, up, face);
    pose->position = {p.x, p.y, p.z};
    return true;
}

bool LookingAtWrist(const XrPosef& wrist) {
    if (S.always) return true;
    xr::EyeView eyes[2];
    if (!xr::LatestViews(eyes)) return false;
    V3 head = Mul(Add(V3{eyes[0].pose.position.x, eyes[0].pose.position.y, eyes[0].pose.position.z},
                      V3{eyes[1].pose.position.x, eyes[1].pose.position.y, eyes[1].pose.position.z}),
                  0.5f);
    V3 to_head = Sub(head, V3{wrist.position.x, wrist.position.y, wrist.position.z});
    float d = Len(to_head);
    if (d > S.show_distance_m || d < 1e-3f) return false;
    V3 face = Rotate(wrist.orientation, V3{0, 0, 1});
    float cos_face = Dot(face, Mul(to_head, 1.f / d));
    // And it has to be roughly where you're looking.
    V3 fwd = Rotate(eyes[0].pose.orientation, V3{0, 0, -1});
    float cos_gaze = Dot(fwd, Mul(to_head, -1.f / d));
    return cos_face > std::cos(S.show_angle_deg * 0.0174533f) && cos_gaze > std::cos(50.f * 0.0174533f);
}

// ------------------------------------------------------- heartbeat --

void Heartbeat(const Status& st) {
    static DWORD s_next = 0, s_second = 0;
    DWORD now = GetTickCount();
    if (s_second && now >= s_second) {
        for (int h = 0; h < 2; ++h) input::Haptic(static_cast<input::Hand>(h), 0.35f, 0.04f, 60.f);
        s_second = 0;
    }
    if (!S.heartbeat || !st.valid || st.dead || st.max_hp <= 0 || st.hp / st.max_hp >= S.heartbeat_below) return;
    if (now < s_next) return;
    // A double pulse, faster as it gets worse: 1.4 s down to 0.7 s apart.
    double f = std::clamp(st.hp / st.max_hp / S.heartbeat_below, 0.0, 1.0);
    s_next = now + static_cast<DWORD>(700 + 700 * f);
    s_second = now + 180;
    for (int h = 0; h < 2; ++h) input::Haptic(static_cast<input::Hand>(h), 0.6f, 0.05f, 60.f);
}

// --------------------------------------------------------- listener --

struct WristListener : xr::Listener {
    bool shown = false;
    DWORD hide_after = 0;

    void AddLayers(XrSession session, XrSpace space, XrTime,
                   std::vector<const XrCompositionLayerBaseHeader*>& layers) override {
        if (!S.enabled) return;
        Status st;
        {
            std::lock_guard<std::mutex> lock(g_status_mutex);
            st = g_status;
        }
        Heartbeat(st);
        if (!st.valid) return;
        input::Snapshot in = input::Peek();
        XrPosef pose;
        if (!in.valid || !WristPose(in.hand[S.hand], &pose)) return;
        // A short hold-off so it doesn't flicker at the edge of the angle.
        DWORD now = GetTickCount();
        if (LookingAtWrist(pose)) {
            if (!shown) log::Info("wrist: display shown");
            shown = true;
            hide_after = now + 250;
        } else if (shown && now > hide_after) {
            shown = false;
        }
        if (!shown) return;
        if (!InitGpu()) return;
        if (!L.swapchain && !CreateSwapchain(session)) return;
        if (!g_drawn_any || !(st == g_drawn)) {
            std::vector<uint32_t> px;
            Draw(st, px);
            if (Upload(px)) {
                static int s_logs = 0;
                if (s_logs < 120 && st.has_gun &&
                    (!g_drawn_any || st.chamber != g_drawn.chamber || st.magazine != g_drawn.magazine ||
                     st.has_magazine != g_drawn.has_magazine)) {
                    ++s_logs;
                    log::Info("wrist: display drawn with %s%d+%d", st.has_magazine ? "" : "no magazine, ",
                              std::max(0, st.magazine), st.chamber);
                }
                g_drawn = st;
                g_drawn_any = true;
                L.has_image = true;
            }
        }
        if (!L.has_image) return;
        L.quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        L.quad.space = space;
        L.quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        L.quad.subImage.swapchain = L.swapchain;
        L.quad.subImage.imageRect = {{0, 0}, {kTexW, kTexH}};
        L.quad.pose = pose;
        L.quad.size = {S.width_m, S.width_m * kTexH / kTexW};
        layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&L.quad));
    }

    void OnSessionEnding() override {
        if (L.swapchain)
            if (auto destroy = Proc<PFN_xrDestroySwapchain>("xrDestroySwapchain")) destroy(L.swapchain);
        L = Layer{};
        g_drawn_any = false;
    }
} g_listener;

}  // namespace

void Init() {
    S.enabled = config::GetBool("wrist_hud", true);
    S.hand = config::GetString("wrist_hud_hand", "left") == "right" ? input::kRight : input::kLeft;
    S.always = config::GetBool("wrist_hud_always", false);
    S.show_distance_m = config::GetFloat("wrist_hud_show_distance_m", 0.5f);
    S.show_angle_deg = config::GetFloat("wrist_hud_show_angle_deg", 40.f);
    S.width_m = config::GetFloat("wrist_hud_width_m", 0.10f);
    // Over the back of the hand (new keys; the old wrist_hud_offset_* put it on the wrist).
    S.offset_back_m = config::GetFloat("hand_hud_offset_back_m", 0.045f);
    S.offset_elbow_m = config::GetFloat("hand_hud_offset_along_m", 0.0f);
    S.ammo = config::GetBool("wrist_hud_ammo", true);
    S.hide_flat_hud = config::GetBool("hud_hide_flat_parts", true);
    S.heartbeat = config::GetBool("low_health_heartbeat", true);
    S.heartbeat_below = config::GetFloat("low_health_below", 0.3f);
    xr::RegisterListener(&g_listener);
    log::Info("wrist: display %s on the %s wrist, ammo %s, flat HUD parts %s, heartbeat %s", S.enabled ? "on" : "off",
              S.hand == input::kLeft ? "left" : "right", S.ammo ? "shown" : "hidden",
              S.hide_flat_hud ? "hidden" : "kept", S.heartbeat ? "on" : "off");
}

void InitGame() { game::AddTickListener(&OnTick); }

}  // namespace holevr::vr::wrist_hud
