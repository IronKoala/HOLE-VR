// dxgi.dll proxy.
//
// HOLE-Win64-Shipping.exe imports dxgi.dll directly, and Windows looks in the
// exe's folder before System32 for it. We ship as dxgi.dll next to the exe,
// forward every export to the real System32 dxgi.dll, and use the load as our
// entry point into the process. No separate injector is needed.

#include "proxy/dxgi_proxy.h"

#include <windows.h>

#include <cstdint>

// Every export the system dxgi.dll has on Windows 10/11. Order matters: it is
// the index into g_real[] used by the stubs below.
#define HOLEVR_DXGI_EXPORTS(X)            \
    X(ApplyCompatResolutionQuirking, 0)   \
    X(CompatString, 1)                    \
    X(CompatValue, 2)                     \
    X(CreateDXGIFactory, 3)               \
    X(CreateDXGIFactory1, 4)              \
    X(CreateDXGIFactory2, 5)              \
    X(DXGID3D10CreateDevice, 6)           \
    X(DXGID3D10CreateLayeredDevice, 7)    \
    X(DXGID3D10GetLayeredDeviceSize, 8)   \
    X(DXGID3D10RegisterLayers, 9)         \
    X(DXGIDeclareAdapterRemovalSupport, 10) \
    X(DXGIDisableVBlankVirtualization, 11) \
    X(DXGIDumpJournal, 12)                \
    X(DXGIGetDebugInterface1, 13)         \
    X(DXGIReportAdapterConfiguration, 14) \
    X(PIXBeginCapture, 15)                \
    X(PIXEndCapture, 16)                  \
    X(PIXGetCaptureState, 17)             \
    X(SetAppCompatStringPointer, 18)      \
    X(UpdateHMDEmulationStatus, 19)

namespace {
constexpr int kExportCount = 20;
const char* const kExportNames[kExportCount] = {
#define HOLEVR_NAME(name, idx) #name,
    HOLEVR_DXGI_EXPORTS(HOLEVR_NAME)
#undef HOLEVR_NAME
};
HMODULE g_real_module = nullptr;
}  // namespace

// Read by the stubs, so it needs C linkage and a stable symbol name.
extern "C" {
void* holevr_dxgi_real[kExportCount] = {};
}

// Each stub is a bare tail jump through the table, so arguments and return
// values pass through untouched whatever the real signature is.
#define HOLEVR_STUB(name, idx)                                              \
    extern "C" __attribute__((naked)) void holevr_##name() {                \
        __asm__ volatile("jmp *(holevr_dxgi_real + " #idx " * 8)(%rip)");   \
    }
HOLEVR_DXGI_EXPORTS(HOLEVR_STUB)
#undef HOLEVR_STUB

namespace holevr::proxy {

bool LoadRealDxgi() {
    if (g_real_module) return true;
    wchar_t path[MAX_PATH];
    UINT len = GetSystemDirectoryW(path, MAX_PATH);
    if (len == 0 || len > MAX_PATH - 12) return false;
    lstrcatW(path, L"\\dxgi.dll");
    g_real_module = LoadLibraryW(path);
    if (!g_real_module) return false;
    for (int i = 0; i < kExportCount; ++i) {
        holevr_dxgi_real[i] =
            reinterpret_cast<void*>(GetProcAddress(g_real_module, kExportNames[i]));
    }
    // CreateDXGIFactory* are the ones the engine actually needs.
    return holevr_dxgi_real[3] && holevr_dxgi_real[4];
}

HMODULE RealDxgi() { return g_real_module; }

}  // namespace holevr::proxy
