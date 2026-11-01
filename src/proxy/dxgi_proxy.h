#pragma once
#include <windows.h>

namespace holevr::proxy {
// Loads System32\dxgi.dll and fills the forwarding table. Called from DllMain.
bool LoadRealDxgi();
HMODULE RealDxgi();
}  // namespace holevr::proxy
