#include "vr/stereo.h"

#include <windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "hooks/hooks.h"
#include "ue/objects.h"
#include "vr/dxgi_hooks.h"
#include "vr/math.h"
#include "vr/openxr.h"

namespace holevr::vr::stereo {
namespace {

// ----------------------------------------------------------- command line --

std::wstring g_cmdline;
using GetCommandLineWFn = LPWSTR(WINAPI*)();
GetCommandLineWFn o_get_command_line_w = nullptr;

LPWSTR WINAPI HookGetCommandLineW() { return g_cmdline.data(); }

// ------------------------------------------------- FFakeStereoRenderingDevice --
//
// Layout in this build (UE 5.5), confirmed from its constructor:
//   0x00 IStereoRendering vtable          0x08 IStereoRenderTargetManager vtable
//   0x10 float FOVInDegrees               0x14 int32 Width   0x18 int32 Height
//   0x1C int32 NumViews (2)               0x20 bool use separate render target
// Both vtables are checked slot by slot at runtime before anything is swapped.
constexpr uintptr_t kFov = 0x10, kWidth = 0x14, kHeight = 0x18, kNumViews = 0x1C, kSeparateRT = 0x20;
constexpr size_t kPrimarySlots = 20;
constexpr size_t kSecondarySlots = 16;
// IStereoRendering slots (UE 5.5 header order).
constexpr size_t kIsStereoEnabled = 1;
constexpr size_t kCalculateStereoViewOffset = 11;
constexpr size_t kGetStereoProjectionMatrix = 12;
constexpr size_t kRenderTextureRenderThread = 14;
// IStereoRenderTargetManager slots.
constexpr size_t kCalculateRenderTargetSize = 2;
constexpr size_t kNeedReAllocateViewportRenderTarget = 3;

using CalcOffsetFn = void (*)(void*, int32_t, FRotator*, float, FVector*);
using ProjectionFn = double* (*)(void*, double*, int32_t);
using RenderTextureFn = void (*)(void*, void*, void*, void*, void*);
using CalcRTSizeFn = void (*)(void*, const void*, uint32_t*, uint32_t*);
using NeedReallocFn = bool (*)(void*, const void*);

uintptr_t g_device = 0;
void* g_primary[kPrimarySlots];
void* g_secondary[kSecondarySlots];
CalcOffsetFn o_calc_offset = nullptr;
ProjectionFn o_projection = nullptr;
RenderTextureFn o_render_texture = nullptr;
CalcRTSizeFn o_calc_rt_size = nullptr;
NeedReallocFn o_need_realloc = nullptr;

const float* g_near_plane = nullptr;  // GNearClippingPlane, read from the original projection code
std::atomic<bool> g_realloc{false};
uint32_t g_eye_w = 0, g_eye_h = 0;

// Views snapshotted for the frame being set up, so both eyes and the
// projection matrices come from the same tracking sample.
std::mutex g_frame_mutex;
xr::EyeView g_frame_views[2];
bool g_have_frame_views = false;

// Recentering, in UE axes but metres (scaled by WorldToMeters on use).
std::atomic<bool> g_recenter{true};
FVector g_origin_m;
double g_origin_yaw = 0.0;

// Logs the first time each hook runs, so a log shows which engine paths are live.
#define HOLEVR_FIRST_CALL(name)                                  \
    do {                                                         \
        static std::atomic<bool> seen{false};                    \
        if (!seen.exchange(true)) log::Info("stereo: first call to %s", name); \
    } while (0)

bool InText(uintptr_t p) {
    const mem::Section* t = mem::Game().Find(".text");
    return t && p >= t->start && p < t->start + t->size;
}

bool VtableLooksValid(uintptr_t vt, size_t slots) {
    for (size_t i = 0; i < slots; ++i)
        if (!InText(mem::ReadOr<uintptr_t>(vt + i * 8))) return false;
    return true;
}

bool LooksLikeFakeStereoDevice(uintptr_t p) {
    if (!mem::LooksLikePointer(p) || !mem::IsReadable(p, 0x28)) return false;
    uintptr_t vt = mem::ReadOr<uintptr_t>(p);
    const mem::Section* rdata = mem::Game().Find(".rdata");
    if (!rdata || vt < rdata->start || vt >= rdata->start + rdata->size) return false;
    // IsStereoEnabled() { return true; } is "mov al, 1; ret".
    uintptr_t is_enabled = mem::ReadOr<uintptr_t>(vt + kIsStereoEnabled * 8);
    unsigned char code[3];
    if (!mem::Read(is_enabled, code) || code[0] != 0xB0 || code[1] != 0x01 || code[2] != 0xC3) return false;
    float fov = mem::ReadOr<float>(p + kFov);
    return mem::ReadOr<int32_t>(p + kNumViews) == 2 && fov > 10.0f && fov < 179.0f &&
           mem::ReadOr<uint8_t>(p + kSeparateRT) <= 1 && VtableLooksValid(vt, kPrimarySlots) &&
           VtableLooksValid(mem::ReadOr<uintptr_t>(p + 8), kSecondarySlots);
}

ue::UObject FindGameEngine() {
    ue::UObject found = 0;
    ue::objects::ForEach([&](int, ue::UObject o) {
        if (found) return;
        if (ue::GetName(ue::GetClass(o)) != "GameEngine") return;
        if (ue::GetName(o).rfind("Default__", 0) == 0) return;
        found = o;
    });
    return found;
}

// GEngine->StereoRenderingDevice is a TSharedPtr; find its object pointer.
uintptr_t FindDevice(ue::UObject engine) {
    for (uintptr_t off = 0x400; off < 0x2000; off += 8) {
        uintptr_t p = mem::ReadOr<uintptr_t>(engine + off);
        if (LooksLikeFakeStereoDevice(p)) {
            log::Info("stereo: emulated stereo device at GEngine+0x%llx", static_cast<unsigned long long>(off));
            return p;
        }
    }
    return 0;
}

// The original projection function loads GNearClippingPlane right before
// storing it into M[3][2]: movss xmm0,[rip+X]; cvtps2pd xmm0,xmm0; movsd [rdi+70h],xmm0
const float* FindNearPlane(uintptr_t projection_fn) {
    static const unsigned char tail[] = {0x0F, 0x5A, 0xC0, 0xF2, 0x0F, 0x11, 0x47, 0x70};
    for (uintptr_t p = projection_fn; p < projection_fn + 0x100; ++p) {
        unsigned char b[16];
        if (!mem::Read(p, b)) return nullptr;
        if (b[0] == 0xF3 && b[1] == 0x0F && b[2] == 0x10 && b[3] == 0x05 && std::memcmp(b + 8, tail, sizeof(tail)) == 0) {
            int32_t disp;
            std::memcpy(&disp, b + 4, 4);
            return reinterpret_cast<const float*>(p + 8 + disp);
        }
    }
    return nullptr;
}

bool FrameViews(bool new_sample, xr::EyeView out[2]) {
    std::lock_guard<std::mutex> lock(g_frame_mutex);
    if (new_sample || !g_have_frame_views) {
        if (!xr::SessionRunning() || !xr::LatestViews(g_frame_views)) return false;
        g_have_frame_views = true;
    }
    out[0] = g_frame_views[0];
    out[1] = g_frame_views[1];
    return true;
}

void HookCalcOffset(void* self, int32_t view_index, FRotator* rotation, float world_to_meters, FVector* location) {
    HOLEVR_FIRST_CALL("CalculateStereoViewOffset");
    xr::EyeView views[2];
    // A left-eye request starts a new frame: take a fresh tracking sample.
    if (view_index < 0 || view_index > 1 || !FrameViews(view_index == 0, views)) {
        o_calc_offset(self, view_index, rotation, world_to_meters, location);
        return;
    }
    if (view_index == 1) xr::SetRenderedViews(views);

    if (g_recenter.exchange(false)) {
        const XrVector3f& a = views[0].pose.position;
        const XrVector3f& b = views[1].pose.position;
        g_origin_m = XrToUePosition((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f, 1.0);
        const XrQuaternionf& q = views[0].pose.orientation;
        g_origin_yaw = YawOf(XrToUeQuat(q.x, q.y, q.z, q.w));
        log::Info("stereo: recentered (yaw %.1f)", g_origin_yaw);
    }

    const XrPosef& pose = views[view_index].pose;
    FQuat eye_q = XrToUeQuat(pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w);
    FVector eye_p = XrToUePosition(pose.position.x, pose.position.y, pose.position.z, 1.0);
    FQuat recenter = YawOnly(-g_origin_yaw);
    FQuat local_q = recenter * eye_q;
    FVector local_p = recenter.Rotate(eye_p - g_origin_m) * world_to_meters;

    // The game keeps steering yaw (mouse/stick turning); the headset owns
    // pitch and roll so the horizon never moves on its own.
    FQuat base = config::GetBool("use_game_pitch", false) ? ToQuat(*rotation) : YawOnly(rotation->Yaw);
    *rotation = ToRotator(base * local_q);
    *location = *location + base.Rotate(local_p);
}

double* HookProjection(void* self, double* out, int32_t view_index) {
    HOLEVR_FIRST_CALL("GetStereoProjectionMatrix");
    xr::EyeView views[2];
    if (view_index < 0 || view_index > 1 || !FrameViews(false, views))
        return o_projection(self, out, view_index);
    const XrFovf& fov = views[view_index].fov;
    double tan_l = std::tan(fov.angleLeft), tan_r = std::tan(fov.angleRight);
    double tan_u = std::tan(fov.angleUp), tan_d = std::tan(fov.angleDown);
    double inv_rl = 1.0 / (tan_r - tan_l), inv_tb = 1.0 / (tan_u - tan_d);
    double z_near = g_near_plane ? *g_near_plane : 10.0;
    // Reversed-Z infinite projection, as Epic's OpenXR plugin builds it.
    double m[16] = {2.0 * inv_rl, 0, 0, 0,
                    0, 2.0 * inv_tb, 0, 0,
                    (tan_r + tan_l) * -inv_rl, (tan_u + tan_d) * -inv_tb, 0, 1,
                    0, 0, z_near, 0};
    std::memcpy(out, m, sizeof(m));
    return out;
}

// --- finding the D3D12 texture behind the engine's FRHITexture ------------

std::vector<uintptr_t> g_resource_vtables;
void* g_last_src = nullptr;
ID3D12Resource* g_last_resource = nullptr;

void LearnResourceVtables() {
    if (!g_resource_vtables.empty()) return;
    ID3D12CommandQueue* queue = dxgi_hooks::GameQueue();
    ID3D12Device* device = nullptr;
    if (!queue || FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))) return;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC tex{};
    tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex.Width = 64;
    tex.Height = 64;
    tex.DepthOrArraySize = 1;
    tex.MipLevels = 1;
    tex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex.SampleDesc.Count = 1;
    tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_RESOURCE_DESC buf{};
    buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf.Width = 256;
    buf.Height = 1;
    buf.DepthOrArraySize = 1;
    buf.MipLevels = 1;
    buf.SampleDesc.Count = 1;
    buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (const D3D12_RESOURCE_DESC* d : {&tex, &buf}) {
        ID3D12Resource* r = nullptr;
        if (SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, d, D3D12_RESOURCE_STATE_COMMON,
                                                      nullptr, IID_PPV_ARGS(&r)))) {
            g_resource_vtables.push_back(*reinterpret_cast<uintptr_t*>(r));
            r->Release();
        }
    }
    device->Release();
}

bool IsD3D12Resource(uintptr_t p) {
    if (!mem::LooksLikePointer(p)) return false;
    uintptr_t vt;
    if (!mem::Read(p, vt)) return false;
    for (uintptr_t known : g_resource_vtables)
        if (vt == known) return true;
    return false;
}

// The engine hands us an RDG or RHI texture wrapper. The ID3D12Resource sits a
// few pointers below it (FRDGTexture -> pooled RHI texture -> FD3D12Resource ->
// ID3D12Resource), at offsets that differ between engine builds. Search up to
// three levels for an object with the D3D12 runtime's resource vtable that is
// a side-by-side 2D texture.
ID3D12Resource* SearchResource(uintptr_t obj, int depth, std::vector<uintptr_t>& path) {
    const uintptr_t span = depth == 0 ? 0x200 : 0x100;
    for (uintptr_t off = 0; off < span; off += 8) {
        uintptr_t p = mem::ReadOr<uintptr_t>(obj + off);
        if (!mem::LooksLikePointer(p) || p == obj) continue;
        path.push_back(off);
        if (IsD3D12Resource(p)) {
            auto* res = reinterpret_cast<ID3D12Resource*>(p);
            D3D12_RESOURCE_DESC d = res->GetDesc();
            if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width >= d.Height) return res;
        } else if (depth < 2) {
            if (ID3D12Resource* r = SearchResource(p, depth + 1, path)) return r;
        }
        path.pop_back();
    }
    return nullptr;
}

ID3D12Resource* FindNativeTexture(void* engine_texture) {
    std::vector<uintptr_t> path;
    ID3D12Resource* res = SearchResource(reinterpret_cast<uintptr_t>(engine_texture), 0, path);
    if (!res) return nullptr;
    D3D12_RESOURCE_DESC d = res->GetDesc();
    std::string chain;
    char hex[24];
    for (uintptr_t o : path) {
        snprintf(hex, sizeof(hex), " +0x%llx", static_cast<unsigned long long>(o));
        chain += hex;
    }
    log::Info("stereo: eye texture %llux%u format %d (pointer path%s)", static_cast<unsigned long long>(d.Width),
              d.Height, static_cast<int>(d.Format), chain.c_str());
    return res;
}

void HookRenderTexture(void* self, void* cmdlist, void* back_buffer, void* src_texture, void* window_size) {
    HOLEVR_FIRST_CALL("RenderTexture_RenderThread");
    if (!src_texture) {
        static bool told = false;
        if (!told) log::Warn("stereo: engine passed no eye texture to RenderTexture");
        told = true;
    }
    if (src_texture && src_texture != g_last_src) {
        LearnResourceVtables();
        g_last_resource = FindNativeTexture(src_texture);
        g_last_src = src_texture;
        if (!g_last_resource) log::Warn("stereo: could not find the D3D12 texture for the eye image");
    }
    xr::SetEyeTexture(g_last_resource);
    if (config::GetBool("desktop_mirror", true))
        o_render_texture(self, cmdlist, back_buffer, src_texture, window_size);
}

// Secondary-interface methods receive this = device + 8.
void HookCalcRTSize(void* self, const void* viewport, uint32_t* x, uint32_t* y) {
    HOLEVR_FIRST_CALL("CalculateRenderTargetSize");
    if (g_eye_w && g_eye_h) {
        *x = g_eye_w * 2;
        *y = g_eye_h;
        g_realloc = false;
        return;
    }
    o_calc_rt_size(self, viewport, x, y);
}

bool HookNeedRealloc(void* self, const void* viewport) {
    HOLEVR_FIRST_CALL("NeedReAllocateViewportRenderTarget");
    return g_realloc.load() || o_need_realloc(self, viewport);
}

}  // namespace

void InstallCommandLineHook() {
    if (!config::GetBool("vr_enabled", true)) return;
    std::wstring original = GetCommandLineW();
    if (original.find(L"-emulatestereo") != std::wstring::npos) return;
    g_cmdline = original + L" -emulatestereo";
    o_get_command_line_w = reinterpret_cast<GetCommandLineWFn>(
        hooks::PatchExeImport("KERNEL32.dll", "GetCommandLineW", reinterpret_cast<void*>(&HookGetCommandLineW)));
    log::Info(o_get_command_line_w ? "Added -emulatestereo to the game's command line"
                                   : "Could not hook GetCommandLineW; add -emulatestereo to the launch options");
}

bool Install() {
    if (g_device) return true;
    ue::UObject engine = FindGameEngine();
    if (!engine) return false;
    uintptr_t dev = FindDevice(engine);
    if (!dev) {
        log::Error("stereo: the engine's emulated stereo device is missing (was -emulatestereo applied?)");
        return false;
    }
    uintptr_t vt1 = mem::ReadOr<uintptr_t>(dev);
    uintptr_t vt2 = mem::ReadOr<uintptr_t>(dev + 8);
    std::memcpy(g_primary, reinterpret_cast<void*>(vt1), sizeof(g_primary));
    std::memcpy(g_secondary, reinterpret_cast<void*>(vt2), sizeof(g_secondary));

    o_calc_offset = reinterpret_cast<CalcOffsetFn>(g_primary[kCalculateStereoViewOffset]);
    o_projection = reinterpret_cast<ProjectionFn>(g_primary[kGetStereoProjectionMatrix]);
    o_render_texture = reinterpret_cast<RenderTextureFn>(g_primary[kRenderTextureRenderThread]);
    o_calc_rt_size = reinterpret_cast<CalcRTSizeFn>(g_secondary[kCalculateRenderTargetSize]);
    o_need_realloc = reinterpret_cast<NeedReallocFn>(g_secondary[kNeedReAllocateViewportRenderTarget]);
    g_near_plane = FindNearPlane(reinterpret_cast<uintptr_t>(o_projection));
    if (!g_near_plane) log::Warn("stereo: near clip plane not found, using 10 cm");

    g_primary[kCalculateStereoViewOffset] = reinterpret_cast<void*>(&HookCalcOffset);
    g_primary[kGetStereoProjectionMatrix] = reinterpret_cast<void*>(&HookProjection);
    g_primary[kRenderTextureRenderThread] = reinterpret_cast<void*>(&HookRenderTexture);
    g_secondary[kCalculateRenderTargetSize] = reinterpret_cast<void*>(&HookCalcRTSize);
    g_secondary[kNeedReAllocateViewportRenderTarget] = reinterpret_cast<void*>(&HookNeedRealloc);

    xr::RecommendedEyeSize(&g_eye_w, &g_eye_h);
    if (g_eye_w && g_eye_h) {
        *reinterpret_cast<int32_t*>(dev + kWidth) = static_cast<int32_t>(g_eye_w * 2);
        *reinterpret_cast<int32_t*>(dev + kHeight) = static_cast<int32_t>(g_eye_h);
        g_realloc = true;
    }
    // Swap this one object's vtables; the engine's shared tables stay untouched.
    *reinterpret_cast<void* volatile*>(dev) = g_primary;
    *reinterpret_cast<void* volatile*>(dev + 8) = g_secondary;
    g_device = dev;
    log::Info("stereo: hooked (render target %ux%u, near plane %.1f)", g_eye_w * 2, g_eye_h,
              g_near_plane ? *g_near_plane : 10.0f);
    return true;
}

void Recenter() { g_recenter = true; }

}  // namespace holevr::vr::stereo
