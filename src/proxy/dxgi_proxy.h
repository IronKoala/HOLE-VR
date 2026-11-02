#pragma once
#include <windows.h>

namespace holevr::proxy {
// Loads System32\dxgi.dll and fills the forwarding table. Called from DllMain.
bool LoadRealDxgi();
HMODULE RealDxgi();

// Called with every DXGI factory the process creates (riid as requested).
using FactoryCallback = void (*)(void* factory, REFIID riid);
void SetFactoryCallback(FactoryCallback cb);
}  // namespace holevr::proxy
