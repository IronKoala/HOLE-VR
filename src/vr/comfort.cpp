#include "vr/comfort.h"

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "game/game.h"
#include "game/gun.h"
#include "ue/call.h"
#include "vr/comfort_math.h"
#include "vr/dxgi_hooks.h"
#include "vr/hands.h"
#include "vr/openxr.h"
#include "vr/world_menu.h"

namespace holevr::vr::comfort {
namespace {
using ue::UObject;

template <typename T>
T Proc(const char* name) {
    return reinterpret_cast<T>(xr::GetProc(name));
}

struct Settings {
    double vignette_move = 0.0;  // 0..1
    double vignette_turn = 0.0;
    bool wall_fade = true;
    double wall_start = 15.0;  // cm from a surface where the head-in-wall fade begins
    double wall_full = 2.0;    // cm from it where the view is black
} S;

void LoadSettings() {
    S.vignette_move = std::clamp<double>(config::GetFloat("comfort_vignette_move", 0.f), 0.0, 1.0);
    S.vignette_turn = std::clamp<double>(config::GetFloat("comfort_vignette_turn", 0.f), 0.0, 1.0);
    S.wall_fade = config::GetBool("head_wall_fade", true);
    S.wall_start = std::clamp<double>(config::GetFloat("head_wall_fade_start_cm", 15.f), 3.0, 30.0);
    S.wall_full = std::clamp<double>(config::GetFloat("head_wall_fade_full_cm", 2.f), 0.0, S.wall_start - 1.0);
}

double NowS() {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) / static_cast<double>(freq.QuadPart);
}

// Targets from the game thread, with when they were set.
std::atomic<float> g_move{0.f}, g_turn{0.f}, g_wall{0.f};
std::atomic<double> g_motion_at{0.0}, g_wall_at{0.0};
std::atomic<double> g_blink_until{0.0};

// ---------------------------------------------------------- the layer --

struct Gpu {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    XrSpace view_space = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D12KHR> images;
    bool uploaded = false;
    bool failed = false;
    XrCompositionLayerQuad vignette{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad fade{XR_TYPE_COMPOSITION_LAYER_QUAD};
    Smoother strength, opacity;
    double last = 0.0;
    int logged_level = -1;
} G;

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER t{};
    t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    t.Transition.pResource = r;
    t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    t.Transition.StateBefore = a;
    t.Transition.StateAfter = b;
    return t;
}

// The vignette rings and fade blocks, uploaded once into a static image.
bool Upload(XrSession session) {
    if (G.failed) return false;
    ID3D12Device* dev = xr::Device();
    ID3D12CommandQueue* queue = dxgi_hooks::GameQueue();
    if (!dev || !queue) return false;
    auto enum_formats = Proc<PFN_xrEnumerateSwapchainFormats>("xrEnumerateSwapchainFormats");
    auto create = Proc<PFN_xrCreateSwapchain>("xrCreateSwapchain");
    auto enum_images = Proc<PFN_xrEnumerateSwapchainImages>("xrEnumerateSwapchainImages");
    auto acquire = Proc<PFN_xrAcquireSwapchainImage>("xrAcquireSwapchainImage");
    auto wait = Proc<PFN_xrWaitSwapchainImage>("xrWaitSwapchainImage");
    auto release = Proc<PFN_xrReleaseSwapchainImage>("xrReleaseSwapchainImage");
    if (!enum_formats || !create || !enum_images || !acquire || !wait || !release) return false;
    G.failed = true;  // until it works
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
        log::Error("comfort: no usable image format for the vignette");
        return false;
    }
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = fmt;
    ci.sampleCount = 1;
    ci.width = kTexW;
    ci.height = kTexH;
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    if (XR_FAILED(create(session, &ci, &G.swapchain))) {
        log::Error("comfort: vignette swapchain failed");
        return false;
    }
    enum_images(G.swapchain, 0, &n, nullptr);
    G.images.assign(n, XrSwapchainImageD3D12KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    enum_images(G.swapchain, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(G.images.data()));

    // Black with alpha only, so the byte order of the format doesn't matter.
    std::vector<uint32_t> px = TexturePixels();
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cmd = nullptr;
    ID3D12Fence* fence = nullptr;
    ID3D12Resource* upload = nullptr;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = static_cast<UINT64>(kTexW) * kTexH * 4;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bool ok = SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
              SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cmd))) &&
              SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) &&
              SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                     nullptr, IID_PPV_ARGS(&upload)));
    void* map = nullptr;
    D3D12_RANGE none{0, 0};
    if (ok && SUCCEEDED(upload->Map(0, &none, &map))) {
        std::memcpy(map, px.data(), px.size() * 4);
        upload->Unmap(0, nullptr);
    } else {
        ok = false;
    }
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (ok && XR_SUCCEEDED(acquire(G.swapchain, &ai, &index))) {
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = 100000000;
        ok = XR_SUCCEEDED(wait(G.swapchain, &wi));
        if (ok) {
            ID3D12Resource* dst = G.images[index].texture;
            D3D12_RESOURCE_BARRIER b = Transition(dst, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->ResourceBarrier(1, &b);
            D3D12_TEXTURE_COPY_LOCATION to{};
            to.pResource = dst;
            to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION from{};
            from.pResource = upload;
            from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            from.PlacedFootprint.Footprint.Format = dst->GetDesc().Format;
            from.PlacedFootprint.Footprint.Width = kTexW;
            from.PlacedFootprint.Footprint.Height = kTexH;
            from.PlacedFootprint.Footprint.Depth = 1;
            from.PlacedFootprint.Footprint.RowPitch = kTexW * 4;
            cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
            cmd->ResourceBarrier(1, &b);
            ok = SUCCEEDED(cmd->Close());
            if (ok) {
                ID3D12CommandList* lists[] = {cmd};
                queue->ExecuteCommandLists(1, lists);
                queue->Signal(fence, 1);
                HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                fence->SetEventOnCompletion(1, ev);
                WaitForSingleObject(ev, 500);
                CloseHandle(ev);
            }
        }
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        release(G.swapchain, &ri);
    } else {
        ok = false;
    }
    if (upload) upload->Release();
    if (fence) fence->Release();
    if (cmd) cmd->Release();
    if (alloc) alloc->Release();
    if (!ok) {
        log::Error("comfort: could not upload the vignette image");
        return false;
    }
    G.failed = false;
    G.uploaded = true;
    log::Info("comfort: vignette and fade image ready (%dx%d)", kTexW, kTexH);
    return true;
}

void SetQuad(XrCompositionLayerQuad& q, const Rect& r) {
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    q.space = G.view_space;
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = G.swapchain;
    q.subImage.imageRect = {{r.x, r.y}, {r.w, r.h}};
    q.pose = {{0, 0, 0, 1}, {0, 0, -static_cast<float>(kQuadDistM)}};
    q.size = {static_cast<float>(kQuadHalfM * 2), static_cast<float>(kQuadHalfM * 2)};
}

struct Listener : xr::Listener {
    void OnSessionCreated(XrSession session, std::vector<XrActionSet>&) override {
        auto make = Proc<PFN_xrCreateReferenceSpace>("xrCreateReferenceSpace");
        if (!make) return;
        XrReferenceSpaceCreateInfo rci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        rci.poseInReferenceSpace.orientation.w = 1.0f;
        if (XR_FAILED(make(session, &rci, &G.view_space))) {
            G.view_space = XR_NULL_HANDLE;
            log::Error("comfort: no head-locked space; vignette and fades are off");
        }
    }

    void AddLayers(XrSession session, XrSpace, XrTime,
                   std::vector<const XrCompositionLayerBaseHeader*>& layers) override {
        if (!G.view_space) return;
        const double now = NowS();
        double dt = G.last > 0 ? std::clamp(now - G.last, 0.0, 0.1) : 0.0;
        G.last = now;
        // Stale targets (game paused, menu up, loading) count as zero.
        const bool motion_fresh = now - g_motion_at.load() < 0.15;
        double want = 0;
        if (motion_fresh)
            want = std::max(g_move.load() * S.vignette_move, g_turn.load() * S.vignette_turn);
        double strength = G.strength.Step(want, dt, 0.10, 0.30);
        const bool wall_fresh = now - g_wall_at.load() < 0.15;
        double dark = wall_fresh && S.wall_fade ? g_wall.load() : 0.0;
        if (now < g_blink_until.load()) dark = 1.0;
        double opacity = G.opacity.Step(dark, dt, 0.04, 0.18);
        const int vl = VignetteLevel(strength), fl = FadeLevel(opacity);
        if (vl == 0 && fl == 0) return;
        if (!G.uploaded && !Upload(session)) return;
        if (vl > 0) {
            SetQuad(G.vignette, VignetteTile(vl));
            layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&G.vignette));
        }
        if (fl > 0) {
            SetQuad(G.fade, FadeBlock(fl));
            layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&G.fade));
        }
    }

    void OnSessionEnding() override {
        if (G.swapchain)
            if (auto destroy = Proc<PFN_xrDestroySwapchain>("xrDestroySwapchain")) destroy(G.swapchain);
        if (G.view_space)
            if (auto destroy = Proc<PFN_xrDestroySpace>("xrDestroySpace")) destroy(G.view_space);
        G = Gpu{};
    }
} g_listener;

// ---------------------------------------------------- head in a wall --
// A ball is swept from the middle of the character's collision capsule (at
// head height) to the eyes. Its radius is where the fade begins
// (head_wall_fade_start_cm, 15): where it stops, the eyes are that close to
// a wall or object, and the further they go on past that point, the darker:
// black head_wall_fade_full_cm (2) before the surface. The first version
// only began fading 1 cm inside the wall and was black 9 cm in, so you saw
// through first (owner 2026-11-03 10:07: "the wall fade needs to kick in a
// lot sooner"; the near clip plane is 1 cm, so the wall vanishes as soon as
// the eyes cross it).
struct TArrayRaw {
    const UObject* data;
    int32_t num, max;
};

struct WallState {
    UObject ksl = 0, gps = 0;
    int lookups = 0;
    int32_t radius_size = -1;  // SphereTraceSingle's Radius: float (4) or double (8)
    bool was_inside = false;
    double inside_since = 0;
    double closest = 1e9;      // nearest the eyes got to the surface while faded, cm
} WS;

double HeadInWall(UObject player, const hands::Pose& head, double* gap) {
    if ((!WS.ksl || !WS.gps) && WS.lookups < 3) {
        ++WS.lookups;
        if (!WS.ksl) WS.ksl = ue::FindObject("KismetSystemLibrary /Script/Engine.Default__KismetSystemLibrary");
        if (!WS.gps) WS.gps = ue::FindObject("GameplayStatics /Script/Engine.Default__GameplayStatics");
        if (!WS.ksl || !WS.gps) log::Warn("comfort: head-in-wall check unavailable (trace library not found)");
    }
    if (!WS.ksl || !WS.gps) return 0;
    ue::Call loc(player, "K2_GetActorLocation");
    if (!loc.Invoke()) return 0;
    FVector cap = loc.Result<FVector>();
    double half = 88.0;
    UObject capsule = 0;
    if (ue::GetProp(player, "CapsuleComponent", capsule) && capsule) {
        ue::Call h(capsule, "GetScaledCapsuleHalfHeight");
        if (h.Invoke() && h.Result<float>() > 10.f) half = h.Result<float>();
    }
    // Far from the character, the game has moved its camera (the results
    // screen after an extract or a death puts it elsewhere): the line would
    // cross whatever lies between and fade a menu to black (owner 2026-11-03:
    // "if i successfully extract i cant see the menu tho i can still use it";
    // log: faded for the whole 31 s the results were up).
    const double hx = head.location.X - cap.X, hy = head.location.Y - cap.Y;
    if (hx * hx + hy * hy > 100.0 * 100.0) return 0;
    FVector start{cap.X, cap.Y, std::clamp(head.location.Z, cap.Z - half + 20.0, cap.Z + half - 5.0)};
    FVector d = head.location - start;
    double len = std::sqrt(d.X * d.X + d.Y * d.Y + d.Z * d.Z);
    if (len < 4.0) return 0;  // head right over the capsule: nothing to cross
    // The gun and whatever the hands hold (a magazine brought up to the
    // face) never fade the view.
    UObject ignore[4] = {player, 0, 0, 0};
    int32_t n = 1;
    game::Gun gun = game::Gun::Current(player);
    if (gun.Valid()) ignore[n++] = gun.Object();
    for (hands::Hand h : {hands::kLeft, hands::kRight}) {
        const UObject held = hands::GetGrab(h).object;
        if (held && held != ignore[1]) ignore[n++] = held;
    }
    TArrayRaw arr{ignore, n, n};
    ue::Call t(WS.ksl, "SphereTraceSingle");
    if (!t.Valid()) return 0;
    if (WS.radius_size < 0) {
        int32_t sz = 0;
        t.Raw("Radius", &sz);
        WS.radius_size = sz;
        if (sz != 4 && sz != 8) log::Warn("comfort: sphere trace radius is %d bytes; head-in-wall fade is off", sz);
    }
    if (WS.radius_size == 4)
        t.Set("Radius", static_cast<float>(S.wall_start));
    else if (WS.radius_size == 8)
        t.Set("Radius", S.wall_start);
    else
        return 0;
    t.Set("WorldContextObject", player).Set("Start", start).Set("End", head.location)
        .Set("TraceChannel", uint8_t{0}).Set("bTraceComplex", false).Set("ActorsToIgnore", arr)
        .Set("bIgnoreSelf", true);
    if (!t.Invoke() || !t.Result<bool>()) return 0;
    int32_t hit_size = 0;
    uint8_t* hit = t.Raw("OutHit", &hit_size);
    if (!hit) return 0;
    ue::Call b(WS.gps, "BreakHitResult");
    int32_t in_size = 0;
    uint8_t* in = b.Raw("Hit", &in_size);
    if (!in || in_size != hit_size) return 0;
    std::memcpy(in, hit, static_cast<size_t>(hit_size));
    if (!b.Invoke() || !b.Get<bool>("bBlockingHit") || b.Get<bool>("bInitialOverlap")) return 0;
    // The ball's centre where it touched: the eyes are wall_start from the
    // surface there (head-on), and each cm further along brings them a cm
    // closer.
    FVector at = b.Get<FVector>("Location");
    FVector past = head.location - at;
    double depth = std::sqrt(past.X * past.X + past.Y * past.Y + past.Z * past.Z);
    if (gap) *gap = S.wall_start - depth;
    return std::clamp(depth / (S.wall_start - S.wall_full), 0.0, 1.0);
}

void OnTick(UObject player, float) {
    if (!S.wall_fade || !player) return;
    hands::Pose head;
    if (!hands::Head(&head)) return;
    // A menu on the forearm is never faded out: it has to be readable even
    // with your head in something (the fade is drawn over everything).
    double gap = 1e9;
    double dark = world_menu::MenuUp() ? 0.0 : HeadInWall(player, head, &gap);
    g_wall.store(static_cast<float>(dark));
    g_wall_at.store(NowS());
    bool inside = dark > 0.5;
    if (inside) WS.closest = std::min(WS.closest, gap);
    if (inside != WS.was_inside) {
        WS.was_inside = inside;
        if (inside) {
            WS.inside_since = NowS();
            WS.closest = gap;
            log::Info("comfort: head close to a wall or object (%.0f cm from it), view fading", gap);
        } else {
            log::Info("comfort: head away from the wall again after %.1f s (it got to %.0f cm %s the surface)",
                      NowS() - WS.inside_since, std::fabs(WS.closest), WS.closest < 0 ? "past" : "from");
        }
    }
}

}  // namespace

void SetMotion(double move01, double turn01) {
    g_move.store(static_cast<float>(std::clamp(move01, 0.0, 1.0)));
    g_turn.store(static_cast<float>(std::clamp(turn01, 0.0, 1.0)));
    g_motion_at.store(NowS());
}

void Blink(double seconds) { g_blink_until.store(NowS() + seconds); }

void Init() {
    LoadSettings();
    config::AddChangeListener(&LoadSettings);
    xr::RegisterListener(&g_listener);
    log::Info("comfort: vignette moving %.2f, turning %.2f; head-in-wall fade %s (begins %.0f cm from a surface, "
              "black at %.0f cm)", S.vignette_move, S.vignette_turn, S.wall_fade ? "on" : "off", S.wall_start,
              S.wall_full);
}

void InitGame() { game::AddTickListener(&OnTick); }

}  // namespace holevr::vr::comfort
