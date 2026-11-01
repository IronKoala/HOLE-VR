#pragma once
// Hooks the game's swap chain through the dxgi.dll proxy: captures the D3D12
// command queue the game presents on and runs the OpenXR frame on Present.

#include <windows.h>

namespace holevr::vr::dxgi_hooks {
// Called by the proxy with every factory the game creates.
void OnFactoryCreated(void* factory, REFIID riid);
}  // namespace holevr::vr::dxgi_hooks

struct ID3D12CommandQueue;
namespace holevr::vr::dxgi_hooks {
// The D3D12 queue the game presents on, once its swap chain exists.
ID3D12CommandQueue* GameQueue();
// Resize the game window by one pixel and back, so the engine re-creates its
// viewport render targets.
void NudgeWindow();
}  // namespace holevr::vr::dxgi_hooks
