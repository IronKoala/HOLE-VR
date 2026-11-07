#include "vr/spectator.h"

#include <d3d12.h>
#include <d3dcompiler.h>
#include <windows.h>

#include <cstring>
#include <string>

#include "core/config.h"
#include "core/log.h"
#include "core/perf.h"
#include "vr/openxr.h"
#include "vr/spectator_math.h"

namespace holevr::vr::spectator {
namespace {

// Full-window triangle; uv 0..1 over the viewport (the destination rectangle).
const char kShader[] = R"(
Texture2D eye_picture : register(t0);
SamplerState linear_clamp : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V VS(uint id : SV_VertexID) {
    V o;
    float2 q = float2((id << 1) & 2, id & 2);
    o.uv = q;
    o.pos = float4(q.x * 2.0 - 1.0, 1.0 - q.y * 2.0, 0.0, 1.0);
    return o;
}
float4 PS(V i) : SV_Target { return float4(eye_picture.Sample(linear_clamp, i.uv).rgb, 1.0); }
)";

constexpr int kFramesInFlight = 3;

ID3D12Device* g_device = nullptr;
ID3D12CommandQueue* g_queue = nullptr;
ID3D12RootSignature* g_root = nullptr;
ID3D12PipelineState* g_pso = nullptr;
DXGI_FORMAT g_pso_format = DXGI_FORMAT_UNKNOWN;
ID3DBlob* g_vs = nullptr;
ID3DBlob* g_ps = nullptr;
ID3D12DescriptorHeap* g_rtv_heap = nullptr;
ID3D12DescriptorHeap* g_srv_heap = nullptr;
ID3D12CommandAllocator* g_allocators[kFramesInFlight] = {};
UINT64 g_allocator_fence[kFramesInFlight] = {};
ID3D12GraphicsCommandList* g_cmd = nullptr;
ID3D12Fence* g_fence = nullptr;
HANDLE g_fence_event = nullptr;
UINT64 g_fence_value = 0;
int g_slot = 0;

// The cropped eye picture, copied out of the window first (a draw can't read
// the window picture while writing it).
ID3D12Resource* g_crop = nullptr;
uint32_t g_crop_w = 0, g_crop_h = 0;
DXGI_FORMAT g_crop_format = DXGI_FORMAT_UNKNOWN;

bool g_failed = false;
View g_logged_view = View::kBothEyes;
bool g_logged_once = false;

perf::Counter g_perf{"monitor view (one eye on the monitor)"};

template <typename T>
void SafeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    return b;
}

bool Fail(const char* what, HRESULT hr) {
    log::Warn("monitor view: %s failed (0x%08lx); the monitor keeps showing both eyes", what,
              static_cast<unsigned long>(hr));
    g_failed = true;
    return false;
}

bool CompileShaders() {
    // Ships with Windows 10 and 11 (System32).
    HMODULE lib = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = lib ? reinterpret_cast<pD3DCompile>(GetProcAddress(lib, "D3DCompile")) : nullptr;
    if (!compile) {
        log::Warn("monitor view: d3dcompiler_47.dll not found; the monitor keeps showing both eyes");
        g_failed = true;
        return false;
    }
    ID3DBlob* errors = nullptr;
    HRESULT hr = compile(kShader, sizeof(kShader) - 1, "monitor_view", nullptr, nullptr, "VS", "vs_5_0", 0, 0,
                         &g_vs, &errors);
    if (SUCCEEDED(hr))
        hr = compile(kShader, sizeof(kShader) - 1, "monitor_view", nullptr, nullptr, "PS", "ps_5_0", 0, 0, &g_ps,
                     &errors);
    if (FAILED(hr)) {
        if (errors) log::Warn("monitor view: shader: %s", static_cast<const char*>(errors->GetBufferPointer()));
        SafeRelease(errors);
        return Fail("compiling the shader", hr);
    }
    SafeRelease(errors);
    return true;
}

bool CreateRootSignature() {
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 1;
    param.DescriptorTable.pDescriptorRanges = &range;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 1;
    desc.pParameters = &param;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    SafeRelease(errors);
    if (FAILED(hr)) return Fail("building the root signature", hr);
    hr = g_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_root));
    SafeRelease(blob);
    if (FAILED(hr)) return Fail("creating the root signature", hr);
    return true;
}

bool Init(ID3D12CommandQueue* queue) {
    if (g_failed) return false;
    if (g_device) return queue == g_queue;
    HRESULT hr = queue->GetDevice(IID_PPV_ARGS(&g_device));
    if (FAILED(hr)) return Fail("finding the device", hr);
    g_queue = queue;
    if (!CompileShaders() || !CreateRootSignature()) return false;
    D3D12_DESCRIPTOR_HEAP_DESC rtv{};
    rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv.NumDescriptors = 1;
    if (FAILED(hr = g_device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&g_rtv_heap)))) return Fail("RTV heap", hr);
    D3D12_DESCRIPTOR_HEAP_DESC srv{};
    srv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv.NumDescriptors = 1;
    srv.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(hr = g_device->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&g_srv_heap)))) return Fail("SRV heap", hr);
    for (auto& a : g_allocators)
        if (FAILED(hr = g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))))
            return Fail("command allocator", hr);
    if (FAILED(hr = g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocators[0], nullptr,
                                                IID_PPV_ARGS(&g_cmd))))
        return Fail("command list", hr);
    g_cmd->Close();
    if (FAILED(hr = g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) return Fail("fence", hr);
    g_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return true;
}

void WaitIdle() {
    if (!g_fence || g_fence->GetCompletedValue() >= g_fence_value) return;
    g_fence->SetEventOnCompletion(g_fence_value, g_fence_event);
    WaitForSingleObject(g_fence_event, 500);
}

bool EnsurePipeline(DXGI_FORMAT format) {
    if (g_pso && g_pso_format == format) return true;
    WaitIdle();
    SafeRelease(g_pso);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = g_root;
    d.VS = {g_vs->GetBufferPointer(), g_vs->GetBufferSize()};
    d.PS = {g_ps->GetBufferPointer(), g_ps->GetBufferSize()};
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = format;
    d.SampleDesc.Count = 1;
    HRESULT hr = g_device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&g_pso));
    if (FAILED(hr)) return Fail("creating the pipeline", hr);
    g_pso_format = format;
    return true;
}

bool EnsureCrop(uint32_t w, uint32_t h, DXGI_FORMAT format) {
    if (g_crop && g_crop_w == w && g_crop_h == h && g_crop_format == format) return true;
    WaitIdle();
    SafeRelease(g_crop);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = format;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    HRESULT hr = g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                   IID_PPV_ARGS(&g_crop));
    if (FAILED(hr)) return Fail("creating the eye picture copy", hr);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = format;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(g_crop, &sv, g_srv_heap->GetCPUDescriptorHandleForHeapStart());
    g_crop_w = w;
    g_crop_h = h;
    g_crop_format = format;
    return true;
}

// Straight ahead inside the shown eye's picture, from the headset's field of
// view for that eye (the engine draws each eye with exactly that view).
void EyeCenter(int eye, float* u, float* v) {
    *u = *v = 0.5f;
    xr::EyeView views[2];
    if (!xr::LatestViews(views)) return;
    const XrFovf& f = views[eye].fov;
    StraightAhead(f.angleLeft, f.angleRight, f.angleUp, f.angleDown, u, v);
}

}  // namespace

void Draw(ID3D12CommandQueue* queue, ID3D12Resource* back_buffer) {
    if (!queue || !back_buffer || g_failed) return;
    if (!config::GetBool("vr_enabled", true)) return;
    const std::string text = config::GetString("monitor_view", "right");
    const View view = ParseView(text.c_str(), View::kRightEye);
    if (!g_logged_once || view != g_logged_view) {
        log::Info("monitor view: %s", view == View::kBothEyes  ? "both eyes side by side"
                                      : view == View::kRightEye ? "right eye, 16:9"
                                                                : "left eye, 16:9");
        g_logged_view = view;
        g_logged_once = true;
    }
    if (view == View::kBothEyes) return;
    perf::Scope timer(g_perf);
    static bool registered = (perf::Register(&g_perf), true);
    (void)registered;
    if (!Init(queue)) return;

    const D3D12_RESOURCE_DESC bd = back_buffer->GetDesc();
    if (bd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || bd.SampleDesc.Count != 1) return;
    const int eye = view == View::kLeftEye ? 0 : 1;
    float cu, cv;
    EyeCenter(eye, &cu, &cv);
    const float aspect = config::GetFloat("monitor_view_aspect", 16.0f / 9.0f);
    const Layout lay = ComputeLayout(static_cast<uint32_t>(bd.Width), bd.Height, eye, cu, cv, aspect);
    if (!lay.valid) return;
    if (!EnsurePipeline(bd.Format) || !EnsureCrop(lay.crop.w, lay.crop.h, bd.Format)) return;

    const int slot = g_slot;
    g_slot = (g_slot + 1) % kFramesInFlight;
    if (g_fence->GetCompletedValue() < g_allocator_fence[slot]) {
        g_fence->SetEventOnCompletion(g_allocator_fence[slot], g_fence_event);
        WaitForSingleObject(g_fence_event, 100);
        if (g_fence->GetCompletedValue() < g_allocator_fence[slot]) return;  // GPU far behind; skip this picture
    }
    g_allocators[slot]->Reset();
    g_cmd->Reset(g_allocators[slot], nullptr);

    // 1. Copy the cropped eye picture out of the window picture.
    D3D12_RESOURCE_BARRIER to_copy[2] = {
        Transition(back_buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE),
        Transition(g_crop, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
    };
    g_cmd->ResourceBarrier(2, to_copy);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = g_crop;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = back_buffer;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box{lay.crop.x, lay.crop.y, 0, lay.crop.x + lay.crop.w, lay.crop.y + lay.crop.h, 1};
    g_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

    // 2. Black window, then the eye picture scaled into the middle.
    D3D12_RESOURCE_BARRIER to_draw[2] = {
        Transition(back_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        Transition(g_crop, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
    };
    g_cmd->ResourceBarrier(2, to_draw);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    g_device->CreateRenderTargetView(back_buffer, nullptr, rtv);
    g_cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    g_cmd->ClearRenderTargetView(rtv, black, 0, nullptr);
    D3D12_VIEWPORT vp{static_cast<float>(lay.dest.x), static_cast<float>(lay.dest.y),
                      static_cast<float>(lay.dest.w), static_cast<float>(lay.dest.h), 0.0f, 1.0f};
    D3D12_RECT sc{static_cast<LONG>(lay.dest.x), static_cast<LONG>(lay.dest.y),
                  static_cast<LONG>(lay.dest.x + lay.dest.w), static_cast<LONG>(lay.dest.y + lay.dest.h)};
    g_cmd->RSSetViewports(1, &vp);
    g_cmd->RSSetScissorRects(1, &sc);
    g_cmd->SetGraphicsRootSignature(g_root);
    g_cmd->SetPipelineState(g_pso);
    ID3D12DescriptorHeap* heaps[] = {g_srv_heap};
    g_cmd->SetDescriptorHeaps(1, heaps);
    g_cmd->SetGraphicsRootDescriptorTable(0, g_srv_heap->GetGPUDescriptorHandleForHeapStart());
    g_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_cmd->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER to_present =
        Transition(back_buffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    g_cmd->ResourceBarrier(1, &to_present);
    if (FAILED(g_cmd->Close())) {
        Fail("recording the draw", E_FAIL);
        return;
    }
    ID3D12CommandList* lists[] = {g_cmd};
    g_queue->ExecuteCommandLists(1, lists);
    g_queue->Signal(g_fence, ++g_fence_value);
    g_allocator_fence[slot] = g_fence_value;

    static bool first = true;
    if (first) {
        first = false;
        log::Info("monitor view: showing %s eye %ux%u from (%u,%u) as %ux%u at (%u,%u) in the %llux%u window",
                  eye ? "right" : "left", lay.crop.w, lay.crop.h, lay.crop.x, lay.crop.y, lay.dest.w, lay.dest.h,
                  lay.dest.x, lay.dest.y, static_cast<unsigned long long>(bd.Width), bd.Height);
    }
}

}  // namespace holevr::vr::spectator
