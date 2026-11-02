#include "vr/openxr.h"

#include <openxr/openxr_loader_negotiation.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "core/config.h"
#include "core/log.h"
#include "core/perf.h"

namespace holevr::vr::xr {
namespace {

// ---------------------------------------------------------------- runtime --

PFN_xrGetInstanceProcAddr g_gipa = nullptr;
XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_system = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_space = XR_NULL_HANDLE;
std::atomic<XrSessionState> g_state{XR_SESSION_STATE_UNKNOWN};
std::atomic<bool> g_running{false};
std::atomic<bool> g_instance_ready{false};
bool g_session_failed = false;

// Frame calls (wait/begin/end) and session state changes happen under this
// lock: the game's Present thread drives frames normally, and the keep-alive
// thread fills in when the game stalls.
std::mutex g_frame_mutex;
// Last projection layer we submitted, re-sent by the keep-alive thread. The
// runtime shows each swapchain's most recently released image, so re-sending
// needs no GPU work; head motion is covered by the runtime's reprojection.
XrCompositionLayerProjectionView g_last_views[2];
bool g_have_last_layer = false;
// Timing (QueryPerformanceCounter ticks).
LARGE_INTEGER g_qpc_freq{};
std::atomic<long long> g_last_frame_qpc{0};
double g_max_gap_ms = 0.0;
unsigned long long g_hitches = 0, g_keepalive_frames = 0;
std::atomic<bool> g_keepalive_started{false};

uint32_t g_eye_w = 0, g_eye_h = 0;
std::vector<Listener*>& Listeners() {
    static std::vector<Listener*> l;
    return l;
}

#define XR_CHECK(call)                                                          \
    do {                                                                        \
        XrResult r_ = (call);                                                   \
        if (XR_FAILED(r_)) {                                                    \
            log::Error("OpenXR: %s failed (%d)", #call, static_cast<int>(r_)); \
            return false;                                                       \
        }                                                                       \
    } while (0)

template <typename F>
F Proc(const char* name) {
    return reinterpret_cast<F>(GetProc(name));
}

std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// Path of the active runtime's JSON manifest: XR_RUNTIME_JSON overrides, then
// the registry value every OpenXR runtime (SteamVR, Oculus, VDXR) writes.
std::wstring ActiveRuntimeManifest() {
    wchar_t env[MAX_PATH];
    if (GetEnvironmentVariableW(L"XR_RUNTIME_JSON", env, MAX_PATH)) return env;
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime",
                     RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
        return buf;
    return {};
}

// Pull "library_path" out of the manifest. The format is tiny and fixed, so
// a full JSON parser is not worth bundling.
std::wstring RuntimeLibraryFromManifest(const std::wstring& manifest) {
    FILE* f = _wfopen(manifest.c_str(), L"rb");
    if (!f) return {};
    std::string json;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) json.append(buf, n);
    fclose(f);
    size_t key = json.find("\"library_path\"");
    if (key == std::string::npos) return {};
    size_t q1 = json.find('"', json.find(':', key) + 1);
    if (q1 == std::string::npos) return {};
    std::string value;
    for (size_t i = q1 + 1; i < json.size() && json[i] != '"'; ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            ++i;
            value.push_back(json[i] == '/' ? '/' : json[i]);
        } else {
            value.push_back(json[i]);
        }
    }
    std::wstring lib = Widen(value);
    bool absolute = lib.size() > 2 && (lib[1] == L':' || (lib[0] == L'\\' && lib[1] == L'\\'));
    if (!absolute) lib = manifest.substr(0, manifest.find_last_of(L"\\/") + 1) + lib;
    return lib;
}

bool LoadRuntime() {
    if (g_gipa) return true;
    std::wstring manifest = ActiveRuntimeManifest();
    if (manifest.empty()) {
        log::Error("OpenXR: no active runtime registered. Open SteamVR once and set it as the OpenXR runtime.");
        return false;
    }
    std::wstring lib = RuntimeLibraryFromManifest(manifest);
    HMODULE runtime = lib.empty() ? nullptr : LoadLibraryW(lib.c_str());
    if (!runtime) {
        log::Error("OpenXR: could not load runtime library from manifest %ls", manifest.c_str());
        return false;
    }
    auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
        GetProcAddress(runtime, "xrNegotiateLoaderRuntimeInterface"));
    if (!negotiate) {
        log::Error("OpenXR: runtime has no xrNegotiateLoaderRuntimeInterface");
        return false;
    }
    XrNegotiateLoaderInfo info{};
    info.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
    info.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
    info.structSize = sizeof(info);
    info.minInterfaceVersion = 1;
    info.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    info.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
    info.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);
    XrNegotiateRuntimeRequest req{};
    req.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
    req.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
    req.structSize = sizeof(req);
    if (XR_FAILED(negotiate(&info, &req)) || !req.getInstanceProcAddr) {
        log::Error("OpenXR: runtime negotiation failed");
        return false;
    }
    g_gipa = req.getInstanceProcAddr;
    log::Info("OpenXR: loaded runtime %ls", lib.c_str());
    return true;
}

// ---------------------------------------------------------------- frames --

std::mutex g_view_mutex;
EyeView g_latest[2]{};
EyeView g_rendered[2]{};
bool g_have_latest = false;
bool g_have_rendered = false;

std::atomic<ID3D12Resource*> g_eye_texture{nullptr};
// The game window's current back buffer. While the engine draws both eyes
// straight into the window (or before its eye texture is found) we copy from
// here instead: lower resolution, but it gets a picture into the headset.
ID3D12Resource* g_fallback_texture = nullptr;
uint64_t g_frames = 0, g_frames_engine = 0, g_frames_fallback = 0, g_frames_none = 0;
DWORD g_last_stats = 0;

struct Swapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D12KHR> images;
    uint32_t width = 0, height = 0;
};
Swapchain g_swapchains[2];
DXGI_FORMAT g_swapchain_format = DXGI_FORMAT_UNKNOWN;
// Size/format the swapchains were made for; recreated only when these change.
UINT64 g_source_width = 0;
UINT g_source_height = 0;
DXGI_FORMAT g_source_format = DXGI_FORMAT_UNKNOWN;

// Our own copy commands, executed on the game's queue right before its Present
// so they run after the frame's rendering in GPU order.
ID3D12Device* g_device = nullptr;
ID3D12CommandQueue* g_queue = nullptr;
constexpr int kFramesInFlight = 3;
ID3D12CommandAllocator* g_allocators[kFramesInFlight] = {};
UINT64 g_allocator_fence_value[kFramesInFlight] = {};
ID3D12GraphicsCommandList* g_cmd = nullptr;
ID3D12Fence* g_fence = nullptr;
HANDLE g_fence_event = nullptr;
UINT64 g_fence_value = 0;
int g_frame_slot = 0;

bool CreateCopyResources() {
    if (g_cmd) return true;
    for (auto& a : g_allocators) {
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) return false;
    }
    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocators[0], nullptr,
                                           IID_PPV_ARGS(&g_cmd))))
        return false;
    g_cmd->Close();
    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) return false;
    g_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return g_fence_event != nullptr;
}

DXGI_FORMAT SrgbVariant(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        default: return f;
    }
}

bool CreateSwapchains(ID3D12Resource* source) {
    D3D12_RESOURCE_DESC desc = source->GetDesc();
    auto enum_formats = Proc<PFN_xrEnumerateSwapchainFormats>("xrEnumerateSwapchainFormats");
    auto create = Proc<PFN_xrCreateSwapchain>("xrCreateSwapchain");
    auto destroy = Proc<PFN_xrDestroySwapchain>("xrDestroySwapchain");
    auto enum_images = Proc<PFN_xrEnumerateSwapchainImages>("xrEnumerateSwapchainImages");
    uint32_t count = 0;
    XR_CHECK(enum_formats(g_session, 0, &count, nullptr));
    std::vector<int64_t> formats(count);
    XR_CHECK(enum_formats(g_session, count, &count, formats.data()));
    auto supported = [&](DXGI_FORMAT f) {
        return std::find(formats.begin(), formats.end(), static_cast<int64_t>(f)) != formats.end();
    };
    // The engine's output is already gamma-encoded, so present those bytes as
    // sRGB; copying into a linear format would look washed out.
    DXGI_FORMAT want = SrgbVariant(desc.Format);
    if (supported(want)) g_swapchain_format = want;
    else if (supported(desc.Format)) g_swapchain_format = desc.Format;
    else {
        log::Error("OpenXR: runtime has no swapchain format compatible with eye texture format %d",
                   static_cast<int>(desc.Format));
        return false;
    }
    g_have_last_layer = false;
    for (auto& sc : g_swapchains) {
        if (sc.handle) destroy(sc.handle);
        sc = Swapchain{};
    }
    uint32_t src_eye_w = static_cast<uint32_t>(desc.Width / 2);
    for (int eye = 0; eye < 2; ++eye) {
        Swapchain& sc = g_swapchains[eye];
        sc.width = src_eye_w;
        sc.height = desc.Height;
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        ci.format = g_swapchain_format;
        ci.sampleCount = 1;
        ci.width = sc.width;
        ci.height = sc.height;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        XR_CHECK(create(g_session, &ci, &sc.handle));
        uint32_t n = 0;
        XR_CHECK(enum_images(sc.handle, 0, &n, nullptr));
        sc.images.assign(n, XrSwapchainImageD3D12KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        XR_CHECK(enum_images(sc.handle, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(sc.images.data())));
    }
    g_source_width = desc.Width;
    g_source_height = desc.Height;
    g_source_format = desc.Format;
    log::Info("OpenXR: swapchains %ux%u per eye, format %d (eye texture format %d)", src_eye_w,
              static_cast<unsigned>(desc.Height), static_cast<int>(g_swapchain_format),
              static_cast<int>(desc.Format));
    return true;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

// ------------------------------------------------------------- capture --
// A one-off readback of the picture the headset gets, written as a BMP.

std::mutex g_capture_mutex;
std::string g_capture_tag;  // non-empty: capture the next frame
ID3D12Resource* g_readback = nullptr;
UINT64 g_readback_size = 0;
D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_readback_fp{};
std::string g_readback_tag;  // set while a readback is in this frame's commands

float Channel(uint32_t px, DXGI_FORMAT f, int c) {
    switch (f) {
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            return ((px >> (10 * c)) & 0x3ff) / 1023.f;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return ((px >> (8 * (2 - c))) & 0xff) / 255.f;
        default:  // R8G8B8A8
            return ((px >> (8 * c)) & 0xff) / 255.f;
    }
}

void WriteBmp(std::string tag, std::vector<uint8_t> data, D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp, DXGI_FORMAT f) {
    const uint32_t w = fp.Footprint.Width / 2, h = fp.Footprint.Height / 2;
    const uint32_t row = (w * 3 + 3) & ~3u;
    std::vector<uint8_t> out(54 + static_cast<size_t>(row) * h);
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(&out[at], &v, 4); };
    out[0] = 'B';
    out[1] = 'M';
    put32(2, static_cast<uint32_t>(out.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, h);
    out[26] = 1;
    out[28] = 24;
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* src = data.data() + static_cast<size_t>(y * 2) * fp.Footprint.RowPitch;
        uint8_t* dst = &out[54 + static_cast<size_t>(h - 1 - y) * row];
        for (uint32_t x = 0; x < w; ++x) {
            uint32_t px;
            std::memcpy(&px, src + x * 2 * 4, 4);
            for (int c = 0; c < 3; ++c) {
                float v = std::clamp(Channel(px, f, c), 0.f, 1.f);
                dst[x * 3 + (2 - c)] = static_cast<uint8_t>(v * 255.f + 0.5f);
            }
        }
    }
    std::wstring path = config::Dir() + L"\\capture-" + std::wstring(tag.begin(), tag.end()) + L".bmp";
    FILE* file = _wfopen(path.c_str(), L"wb");
    if (!file) {
        log::Warn("capture: could not write capture-%s.bmp", tag.c_str());
        return;
    }
    fwrite(out.data(), 1, out.size(), file);
    fclose(file);
    log::Info("capture: saved capture-%s.bmp (%ux%u, both eyes side by side)", tag.c_str(), w, h);
}

// Records a copy of `source` into the readback buffer when a capture is due.
void RecordCapture(ID3D12Resource* source) {
    g_readback_tag.clear();
    std::string tag;
    {
        std::lock_guard<std::mutex> lock(g_capture_mutex);
        if (g_capture_tag.empty()) return;
        tag.swap(g_capture_tag);
    }
    D3D12_RESOURCE_DESC sd = source->GetDesc();
    if (sd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || sd.SampleDesc.Count != 1) {
        log::Warn("capture: source isn't a plain 2D texture, skipped");
        return;
    }
    switch (sd.Format) {
        case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            break;
        default:
            log::Warn("capture: format %d not supported, skipped", static_cast<int>(sd.Format));
            return;
    }
    UINT64 total = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    g_device->GetCopyableFootprints(&sd, 0, 1, 0, &fp, nullptr, nullptr, &total);
    if (!g_readback || g_readback_size < total) {
        if (g_readback) g_readback->Release();
        g_readback = nullptr;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                     nullptr, IID_PPV_ARGS(&g_readback)))) {
            log::Warn("capture: could not create the readback buffer");
            return;
        }
        g_readback_size = total;
    }
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = g_readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = source;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    g_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    g_readback_fp = fp;
    g_readback_tag = tag;
}

// After the frame's commands are submitted: waits for them and saves the picture.
void FinishCapture(DXGI_FORMAT format) {
    if (g_readback_tag.empty()) return;
    std::string tag;
    tag.swap(g_readback_tag);
    if (g_fence->GetCompletedValue() < g_fence_value) {
        g_fence->SetEventOnCompletion(g_fence_value, g_fence_event);
        WaitForSingleObject(g_fence_event, 500);
    }
    void* mapped = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(g_readback_size)};
    if (FAILED(g_readback->Map(0, &range, &mapped)) || !mapped) {
        log::Warn("capture: could not read the picture back");
        return;
    }
    std::vector<uint8_t> data(static_cast<const uint8_t*>(mapped),
                              static_cast<const uint8_t*>(mapped) + g_readback_size);
    D3D12_RANGE none{0, 0};
    g_readback->Unmap(0, &none);
    std::thread(WriteBmp, tag, std::move(data), g_readback_fp, format).detach();
}

// Copies both halves of the eye texture into the acquired swapchain images.
bool CopyEyes(ID3D12Resource* source, D3D12_RESOURCE_STATES src_state, const uint32_t image_index[2]) {
    int slot = g_frame_slot;
    g_frame_slot = (g_frame_slot + 1) % kFramesInFlight;
    if (g_fence->GetCompletedValue() < g_allocator_fence_value[slot]) {
        g_fence->SetEventOnCompletion(g_allocator_fence_value[slot], g_fence_event);
        WaitForSingleObject(g_fence_event, 100);
    }
    g_allocators[slot]->Reset();
    g_cmd->Reset(g_allocators[slot], nullptr);

    D3D12_RESOURCE_BARRIER before[3] = {
        Transition(source, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE),
        Transition(g_swapchains[0].images[image_index[0]].texture, D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_COPY_DEST),
        Transition(g_swapchains[1].images[image_index[1]].texture, D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_COPY_DEST),
    };
    g_cmd->ResourceBarrier(3, before);
    RecordCapture(source);
    for (int eye = 0; eye < 2; ++eye) {
        const Swapchain& sc = g_swapchains[eye];
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = sc.images[image_index[eye]].texture;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = source;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        D3D12_BOX box{eye * sc.width, 0, 0, eye * sc.width + sc.width, sc.height, 1};
        g_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    }
    D3D12_RESOURCE_BARRIER after[3];
    for (int i = 0; i < 3; ++i) {
        after[i] = before[i];
        std::swap(after[i].Transition.StateBefore, after[i].Transition.StateAfter);
    }
    g_cmd->ResourceBarrier(3, after);
    if (FAILED(g_cmd->Close())) return false;
    ID3D12CommandList* lists[] = {g_cmd};
    g_queue->ExecuteCommandLists(1, lists);
    g_queue->Signal(g_fence, ++g_fence_value);
    g_allocator_fence_value[slot] = g_fence_value;
    FinishCapture(source->GetDesc().Format);
    return true;
}

// ---------------------------------------------------------------- session --

bool CreateSession(ID3D12CommandQueue* queue) {
    g_queue = queue;
    g_queue->AddRef();
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&g_device)))) return false;

    // Required by the spec before creating a D3D12 session.
    auto reqs_fn = Proc<PFN_xrGetD3D12GraphicsRequirementsKHR>("xrGetD3D12GraphicsRequirementsKHR");
    XrGraphicsRequirementsD3D12KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    XR_CHECK(reqs_fn(g_instance, g_system, &reqs));
    LUID luid = g_device->GetAdapterLuid();
    if (std::memcmp(&luid, &reqs.adapterLuid, sizeof(LUID)) != 0)
        log::Warn("OpenXR: the game is rendering on a different GPU than the headset is connected to");

    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding.device = g_device;
    binding.queue = g_queue;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = g_system;
    XR_CHECK(Proc<PFN_xrCreateSession>("xrCreateSession")(g_instance, &sci, &g_session));

    // Standing/room-scale space if the runtime has one, seated otherwise.
    auto enum_spaces = Proc<PFN_xrEnumerateReferenceSpaces>("xrEnumerateReferenceSpaces");
    uint32_t n = 0;
    enum_spaces(g_session, 0, &n, nullptr);
    std::vector<XrReferenceSpaceType> spaces(n);
    enum_spaces(g_session, n, &n, spaces.data());
    bool stage = std::find(spaces.begin(), spaces.end(), XR_REFERENCE_SPACE_TYPE_STAGE) != spaces.end() &&
                 !config::GetBool("seated_space", false);
    XrReferenceSpaceCreateInfo rci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rci.referenceSpaceType = stage ? XR_REFERENCE_SPACE_TYPE_STAGE : XR_REFERENCE_SPACE_TYPE_LOCAL;
    rci.poseInReferenceSpace.orientation.w = 1.0f;
    XR_CHECK(Proc<PFN_xrCreateReferenceSpace>("xrCreateReferenceSpace")(g_session, &rci, &g_space));

    std::vector<XrActionSet> sets;
    for (Listener* l : Listeners()) l->OnSessionCreated(g_session, sets);
    if (!sets.empty()) {
        XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        ai.countActionSets = static_cast<uint32_t>(sets.size());
        ai.actionSets = sets.data();
        XR_CHECK(Proc<PFN_xrAttachSessionActionSets>("xrAttachSessionActionSets")(g_session, &ai));
    }
    if (!CreateCopyResources()) {
        log::Error("OpenXR: could not create D3D12 copy resources");
        return false;
    }
    log::Info("OpenXR: session created (%s space)", stage ? "standing" : "seated");
    return true;
}

void PollEvents() {
    auto poll = Proc<PFN_xrPollEvent>("xrPollEvent");
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (poll(g_instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* sc = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            g_state = sc->state;
            log::Info("OpenXR: session state %d", static_cast<int>(g_state.load()));
            if (g_state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(Proc<PFN_xrBeginSession>("xrBeginSession")(g_session, &bi))) g_running = true;
            } else if (g_state == XR_SESSION_STATE_STOPPING) {
                for (Listener* l : Listeners()) l->OnSessionEnding();
                g_running = false;
                g_have_last_layer = false;
                Proc<PFN_xrEndSession>("xrEndSession")(g_session);
            } else if (g_state == XR_SESSION_STATE_EXITING || g_state == XR_SESSION_STATE_LOSS_PENDING) {
                g_running = false;
            }
        }
        ev = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    }
}

void LocateViews(XrTime time) {
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = time;
    li.space = g_space;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t n = 0;
    if (XR_FAILED(Proc<PFN_xrLocateViews>("xrLocateViews")(g_session, &li, &vs, 2, &n, views)) || n != 2) return;
    if (!(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) return;
    std::lock_guard<std::mutex> lock(g_view_mutex);
    for (int i = 0; i < 2; ++i) g_latest[i] = {views[i].pose, views[i].fov};
    g_have_latest = true;
}

double MsSince(long long qpc) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (now.QuadPart - qpc) * 1000.0 / static_cast<double>(g_qpc_freq.QuadPart);
}

void MarkFrameDone() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_last_frame_qpc = now.QuadPart;
}

perf::Counter g_perf_game{"game frame (between submits)"};
perf::Counter g_perf_wait{"xrWaitFrame"};
perf::Counter g_perf_submit{"copy+submit"};

void RunFrame() {
    static bool registered = [] {
        perf::Register(&g_perf_game);
        perf::Register(&g_perf_wait);
        perf::Register(&g_perf_submit);
        return true;
    }();
    (void)registered;
    if (long long last = g_last_frame_qpc.load()) g_perf_game.Add(MsSince(last));
    double t_wait = perf::NowMs();
    // A long gap since the last frame means the game stalled; SteamVR hides
    // the app (black screen) if no frame arrives for too long.
    if (long long last = g_last_frame_qpc.load()) {
        double gap = MsSince(last);
        if (gap > g_max_gap_ms) g_max_gap_ms = gap;
        if (gap > config::GetFloat("hitch_log_ms", 100.0f)) {
            ++g_hitches;
            static DWORD last_log = 0;
            if (GetTickCount() - last_log > 1000) {
                last_log = GetTickCount();
                log::Info("OpenXR: game frame gap %.0f ms (session state %d)", gap, static_cast<int>(g_state.load()));
            }
        }
    }
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    if (XR_FAILED(Proc<PFN_xrWaitFrame>("xrWaitFrame")(g_session, &wi, &fs))) return;
    g_perf_wait.Add(perf::NowMs() - t_wait);
    perf::Scope submit_scope(g_perf_submit);
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(Proc<PFN_xrBeginFrame>("xrBeginFrame")(g_session, &bi))) return;

    LocateViews(fs.predictedDisplayTime);
    for (Listener* l : Listeners()) l->OnFrame(g_session, g_space, fs.predictedDisplayTime);

    XrCompositionLayerProjectionView proj_views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                      {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::vector<const XrCompositionLayerBaseHeader*> layers;

    // The engine leaves its eye texture readable by shaders after drawing the
    // desktop mirror from it; a back buffer is in the PRESENT state.
    ID3D12Resource* source = g_eye_texture.load();
    auto src_state = static_cast<D3D12_RESOURCE_STATES>(config::GetInt(
        "eye_texture_state", D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    const bool from_engine = source != nullptr;
    if (!source && config::GetBool("backbuffer_fallback", true)) {
        source = g_fallback_texture;
        src_state = D3D12_RESOURCE_STATE_PRESENT;
    }
    EyeView rendered[2];
    bool have_rendered;
    {
        std::lock_guard<std::mutex> lock(g_view_mutex);
        have_rendered = g_have_rendered;
        std::memcpy(rendered, g_rendered, sizeof(rendered));
    }
    if (fs.shouldRender && source && have_rendered) {
        D3D12_RESOURCE_DESC sd = source->GetDesc();
        bool ok = (sd.Width == g_source_width && sd.Height == g_source_height && sd.Format == g_source_format) ||
                  CreateSwapchains(source);
        uint32_t index[2] = {};
        auto acquire = Proc<PFN_xrAcquireSwapchainImage>("xrAcquireSwapchainImage");
        auto wait = Proc<PFN_xrWaitSwapchainImage>("xrWaitSwapchainImage");
        auto release = Proc<PFN_xrReleaseSwapchainImage>("xrReleaseSwapchainImage");
        int acquired = 0;
        for (int eye = 0; ok && eye < 2; ++eye) {
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            ok = XR_SUCCEEDED(acquire(g_swapchains[eye].handle, &ai, &index[eye]));
            if (!ok) break;
            ++acquired;
            XrSwapchainImageWaitInfo wti{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wti.timeout = 100000000;  // 100 ms
            ok = XR_SUCCEEDED(wait(g_swapchains[eye].handle, &wti));
        }
        if (ok) ok = CopyEyes(source, src_state, index);
        for (int eye = 0; eye < acquired; ++eye) {
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            release(g_swapchains[eye].handle, &ri);
        }
        if (ok) {
            for (int eye = 0; eye < 2; ++eye) {
                proj_views[eye].pose = rendered[eye].pose;
                proj_views[eye].fov = rendered[eye].fov;
                proj_views[eye].subImage.swapchain = g_swapchains[eye].handle;
                proj_views[eye].subImage.imageRect.extent = {static_cast<int32_t>(g_swapchains[eye].width),
                                                             static_cast<int32_t>(g_swapchains[eye].height)};
            }
            layer.space = g_space;
            layer.viewCount = 2;
            layer.views = proj_views;
            layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer));
            ++(from_engine ? g_frames_engine : g_frames_fallback);
            std::memcpy(g_last_views, proj_views, sizeof(g_last_views));
            g_have_last_layer = true;
        }
    }
    ++g_frames;
    if (layers.empty()) ++g_frames_none;
    DWORD now = GetTickCount();
    if (now - g_last_stats > 10000) {
        log::Info("OpenXR: %llu frames (engine eye texture %llu, window copy %llu, nothing %llu, keep-alive %llu); "
                  "eye texture %s, rendered views %s; longest gap %.0f ms, %llu gaps over limit",
                  (unsigned long long)g_frames, (unsigned long long)g_frames_engine,
                  (unsigned long long)g_frames_fallback, (unsigned long long)g_frames_none,
                  g_keepalive_frames, g_eye_texture.load() ? "found" : "missing", have_rendered ? "yes" : "no",
                  g_max_gap_ms, g_hitches);
        g_max_gap_ms = 0.0;
        g_last_stats = now;
    }
    if (fs.shouldRender)
        for (Listener* l : Listeners()) l->AddLayers(g_session, g_space, fs.predictedDisplayTime, layers);
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = static_cast<uint32_t>(layers.size());
    ei.layers = layers.data();
    Proc<PFN_xrEndFrame>("xrEndFrame")(g_session, &ei);
    MarkFrameDone();
    perf::MaybeReport();
}

// Re-sends the last frame while the game is stalled (shader compiles, level
// streaming, loading), so SteamVR keeps showing the game instead of timing
// it out and going black. Only for real stalls: each keep-alive frame waits
// for a display slot, and the game's next frame then waits for another, so
// a short threshold (40 ms) fed on itself and dragged the game to ~20 fps.
void KeepAliveFrame() {
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    if (XR_FAILED(Proc<PFN_xrWaitFrame>("xrWaitFrame")(g_session, &wi, &fs))) return;
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(Proc<PFN_xrBeginFrame>("xrBeginFrame")(g_session, &bi))) return;
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader* layers[1] = {reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)};
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    if (fs.shouldRender && g_have_last_layer) {
        layer.space = g_space;
        layer.viewCount = 2;
        layer.views = g_last_views;
        ei.layerCount = 1;
        ei.layers = layers;
    }
    Proc<PFN_xrEndFrame>("xrEndFrame")(g_session, &ei);
    ++g_keepalive_frames;
    MarkFrameDone();
}

void KeepAliveLoop() {
    for (;;) {
        Sleep(5);
        long long last = g_last_frame_qpc.load();
        if (!g_running || !last) continue;
        if (MsSince(last) < config::GetFloat("keep_alive_after_ms", 250.0f)) continue;
        std::lock_guard<std::mutex> lock(g_frame_mutex);
        // Re-check under the lock: the game may have submitted meanwhile.
        if (!g_running || MsSince(g_last_frame_qpc.load()) < config::GetFloat("keep_alive_after_ms", 250.0f))
            continue;
        KeepAliveFrame();
    }
}

}  // namespace

void RegisterListener(Listener* listener) { Listeners().push_back(listener); }

PFN_xrVoidFunction GetProc(const char* name) {
    if (!g_gipa) return nullptr;
    PFN_xrVoidFunction fn = nullptr;
    g_gipa(g_instance, name, &fn);
    return fn;
}

bool InitInstance() {
    if (g_instance_ready) return true;
    if (!LoadRuntime()) return false;

    if (!g_instance) {
        auto enum_ext = Proc<PFN_xrEnumerateInstanceExtensionProperties>("xrEnumerateInstanceExtensionProperties");
        uint32_t n = 0;
        XR_CHECK(enum_ext(nullptr, 0, &n, nullptr));
        std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
        XR_CHECK(enum_ext(nullptr, n, &n, props.data()));
        auto available = [&](const char* name) {
            for (const auto& p : props)
                if (std::strcmp(p.extensionName, name) == 0) return true;
            return false;
        };
        std::vector<const char*> wanted = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
        for (Listener* l : Listeners()) l->ExtraInstanceExtensions(wanted);
        std::vector<const char*> enabled;
        for (const char* e : wanted) {
            if (available(e)) enabled.push_back(e);
            else log::Warn("OpenXR: runtime does not offer extension %s", e);
        }
        if (!available(XR_KHR_D3D12_ENABLE_EXTENSION_NAME)) {
            log::Error("OpenXR: runtime has no Direct3D 12 support");
            return false;
        }
        XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
        std::strncpy(ci.applicationInfo.applicationName, "HOLE VR", XR_MAX_APPLICATION_NAME_SIZE - 1);
        std::strncpy(ci.applicationInfo.engineName, "Unreal Engine", XR_MAX_ENGINE_NAME_SIZE - 1);
        ci.applicationInfo.engineVersion = 5;
        ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        ci.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
        ci.enabledExtensionNames = enabled.data();
        XR_CHECK(Proc<PFN_xrCreateInstance>("xrCreateInstance")(&ci, &g_instance));
        XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
        if (XR_SUCCEEDED(Proc<PFN_xrGetInstanceProperties>("xrGetInstanceProperties")(g_instance, &ip)))
            log::Info("OpenXR: runtime %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
                      XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
        for (Listener* l : Listeners()) l->OnInstanceCreated(g_instance);
    }

    XrSystemGetInfo gi{XR_TYPE_SYSTEM_GET_INFO};
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (XR_FAILED(Proc<PFN_xrGetSystem>("xrGetSystem")(g_instance, &gi, &g_system))) {
        return false;  // Headset not connected yet; caller retries quietly.
    }
    auto enum_views = Proc<PFN_xrEnumerateViewConfigurationViews>("xrEnumerateViewConfigurationViews");
    XrViewConfigurationView views[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    uint32_t n = 0;
    XR_CHECK(enum_views(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, views));
    float scale = std::clamp(config::GetFloat("resolution_scale", 1.0f), 0.3f, 2.0f);
    g_eye_w = static_cast<uint32_t>(views[0].recommendedImageRectWidth * scale) & ~3u;
    g_eye_h = static_cast<uint32_t>(views[0].recommendedImageRectHeight * scale) & ~3u;
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    Proc<PFN_xrGetSystemProperties>("xrGetSystemProperties")(g_instance, g_system, &sp);
    log::Info("OpenXR: headset \"%s\", eye render size %ux%u (scale %.2f)", sp.systemName, g_eye_w, g_eye_h, scale);
    g_instance_ready = true;
    return true;
}

bool InstanceReady() { return g_instance_ready; }

std::wstring ActiveRuntimeLibrary() {
    std::wstring manifest = ActiveRuntimeManifest();
    return manifest.empty() ? std::wstring() : RuntimeLibraryFromManifest(manifest);
}

void RecommendedEyeSize(uint32_t* width, uint32_t* height) {
    *width = g_eye_w;
    *height = g_eye_h;
}

void OnPresent(ID3D12CommandQueue* queue, ID3D12Resource* back_buffer) {
    g_fallback_texture = back_buffer;
    if (!g_instance_ready || g_session_failed || !queue) return;
    if (!g_session) {
        if (!CreateSession(queue)) {
            g_session_failed = true;  // Logged; don't retry every frame.
            return;
        }
    }
    if (!g_qpc_freq.QuadPart) QueryPerformanceFrequency(&g_qpc_freq);
    if (!g_keepalive_started.exchange(true) && config::GetBool("keep_alive", true))
        std::thread(KeepAliveLoop).detach();
    std::lock_guard<std::mutex> lock(g_frame_mutex);
    PollEvents();
    if (g_running) RunFrame();
}

void SetEyeTexture(ID3D12Resource* texture) { g_eye_texture = texture; }
void SetFallbackTexture(ID3D12Resource* texture) { g_fallback_texture = texture; }
ID3D12Resource* WindowTexture() { return g_fallback_texture; }

bool LatestViews(EyeView out[2]) {
    std::lock_guard<std::mutex> lock(g_view_mutex);
    if (!g_have_latest) return false;
    out[0] = g_latest[0];
    out[1] = g_latest[1];
    return true;
}

void SetRenderedViews(const EyeView views[2]) {
    std::lock_guard<std::mutex> lock(g_view_mutex);
    g_rendered[0] = views[0];
    g_rendered[1] = views[1];
    g_have_rendered = true;
}

bool SessionRunning() { return g_running; }
bool SessionVisible() {
    XrSessionState s = g_state.load();
    return g_running && (s == XR_SESSION_STATE_VISIBLE || s == XR_SESSION_STATE_FOCUSED);
}
XrInstance Instance() { return g_instance; }
XrSession Session() { return g_session; }
XrSpace TrackingSpace() { return g_space; }
ID3D12Device* Device() { return g_device; }

void RequestCapture(const char* tag) {
    std::lock_guard<std::mutex> lock(g_capture_mutex);
    g_capture_tag = tag ? tag : "frame";
    log::Info("capture: next headset picture will be saved as capture-%s.bmp", g_capture_tag.c_str());
}

}  // namespace holevr::vr::xr
