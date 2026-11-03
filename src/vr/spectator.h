#pragma once
// Monitor view (spectator view). The engine draws both eyes side by side into
// the game window, and the headset picture is copied from there. After that
// copy, right before the window is shown, this redraws the window as one
// eye's picture cropped to 16:9 and centred on straight ahead, so the monitor
// and recordings of it (OBS, NVIDIA app) show a normal-looking game view.
// ini: monitor_view = right (default) / left / both (the old side-by-side).

struct ID3D12CommandQueue;
struct ID3D12Resource;

namespace holevr::vr::spectator {

// Present thread, after the headset copy for this picture has been queued on
// `queue`. back_buffer is the window picture about to be presented.
void Draw(ID3D12CommandQueue* queue, ID3D12Resource* back_buffer);

}  // namespace holevr::vr::spectator
