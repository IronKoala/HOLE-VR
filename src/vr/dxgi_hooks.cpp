#include "vr/dxgi_hooks.h"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <atomic>

#include "core/config.h"
#include "core/log.h"
#include "hooks/hooks.h"
#include "vr/openxr.h"

namespace holevr::vr::dxgi_hooks {
namespace {

// vtable slots (IUnknown 0-2, IDXGIObject 3-6, IDXGIFactory 7-11, IDXGIFactory1 12-13, IDXGIFactory2 14+)
constexpr size_t kCreateSwapChain = 10;
constexpr size_t kCreateSwapChainForHwnd = 15;
// IDXGISwapChain::Present and IDXGISwapChain1::Present1
constexpr size_t kPresent = 8;
constexpr size_t kPresent1 = 22;

using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
                                                             const DXGI_SWAP_CHAIN_DESC1*,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
                                                             IDXGIOutput*, IDXGISwapChain1**);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

CreateSwapChainFn o_create_swapchain = nullptr;
CreateSwapChainForHwndFn o_create_swapchain_for_hwnd = nullptr;
PresentFn o_present = nullptr;
Present1Fn o_present1 = nullptr;

std::atomic<ID3D12CommandQueue*> g_queue{nullptr};
std::atomic<IDXGISwapChain*> g_game_swapchain{nullptr};
thread_local bool t_in_present = false;

// Runs the VR frame for the game's own swap chain only, never for any swap
// chain another component (or the VR runtime) creates in this process.
UINT BeforePresent(IDXGISwapChain* sc, UINT sync_interval) {
    if (t_in_present || sc != g_game_swapchain.load()) return sync_interval;
    t_in_present = true;
    ID3D12Resource* back_buffer = nullptr;
    IDXGISwapChain3* sc3 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) {
        sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back_buffer));
        sc3->Release();
    }
    xr::OnPresent(g_queue.load(), back_buffer);
    // The swap chain keeps its buffers alive; we only borrow this one for the frame.
    if (back_buffer) back_buffer->Release();
    t_in_present = false;
    // The headset sets the pace now; waiting for the monitor's refresh as well
    // would cap the headset at the monitor's rate.
    if (xr::SessionRunning() && !config::GetBool("desktop_vsync", false)) return 0;
    return sync_interval;
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    return o_present(sc, BeforePresent(sc, sync), flags);
}

HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    return o_present1(sc, BeforePresent(sc, sync), flags, p);
}

void OnSwapChainCreated(IUnknown* device, IDXGISwapChain* sc) {
    ID3D12CommandQueue* queue = nullptr;
    if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&queue)))) return;  // not D3D12
    if (g_game_swapchain.load()) {
        queue->Release();
        return;  // keep the first (the game window's) swap chain
    }
    g_queue = queue;  // keep the reference for the life of the process
    g_game_swapchain = sc;
    void** vt = *reinterpret_cast<void***>(sc);
    if (!o_present) o_present = reinterpret_cast<PresentFn>(hooks::PatchVtableSlot(vt, kPresent, reinterpret_cast<void*>(&HookPresent)));
    IDXGISwapChain1* sc1 = nullptr;
    if (!o_present1 && SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1)))) {
        void** vt1 = *reinterpret_cast<void***>(sc1);
        o_present1 = reinterpret_cast<Present1Fn>(hooks::PatchVtableSlot(vt1, kPresent1, reinterpret_cast<void*>(&HookPresent1)));
        sc1->Release();
    }
    log::Info("Game swap chain hooked (D3D12 queue %p)", static_cast<void*>(queue));
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChain(IDXGIFactory* f, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                              IDXGISwapChain** out) {
    HRESULT hr = o_create_swapchain(f, device, desc, out);
    if (SUCCEEDED(hr) && out && *out) OnSwapChainCreated(device, *out);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* device, HWND hwnd,
                                                     const DXGI_SWAP_CHAIN_DESC1* desc,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* out_target,
                                                     IDXGISwapChain1** out) {
    HRESULT hr = o_create_swapchain_for_hwnd(f, device, hwnd, desc, fs, out_target, out);
    if (SUCCEEDED(hr) && out && *out) OnSwapChainCreated(device, *out);
    return hr;
}

}  // namespace

void OnFactoryCreated(void* factory, REFIID riid) {
    if (!factory || o_create_swapchain_for_hwnd) return;
    auto* unk = static_cast<IUnknown*>(factory);
    IDXGIFactory2* f2 = nullptr;
    if (FAILED(unk->QueryInterface(IID_PPV_ARGS(&f2)))) return;
    void** vt = *reinterpret_cast<void***>(f2);
    o_create_swapchain = reinterpret_cast<CreateSwapChainFn>(
        hooks::PatchVtableSlot(vt, kCreateSwapChain, reinterpret_cast<void*>(&HookCreateSwapChain)));
    o_create_swapchain_for_hwnd = reinterpret_cast<CreateSwapChainForHwndFn>(
        hooks::PatchVtableSlot(vt, kCreateSwapChainForHwnd, reinterpret_cast<void*>(&HookCreateSwapChainForHwnd)));
    f2->Release();
    log::Info("DXGI factory hooked");
}

ID3D12CommandQueue* GameQueue() { return g_queue.load(); }

}  // namespace holevr::vr::dxgi_hooks
