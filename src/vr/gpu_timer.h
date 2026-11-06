#pragma once
#include <d3d12.h>

// How long the GPU really spends on each game frame (2026-11-06).
//
// The older "GPU done after Present" figure is stamped by our headset copy,
// which is queued only after xrWaitFrame has returned, so it can't be shorter
// than that wait and can't say whether the GPU is the limit. This puts two
// GPU timestamps of its own on the game's queue: one right at the game's
// Present (the frame's last work is done), one in front of the first command
// lists the game submits after it (the next frame's work starts). Per frame:
//   GPU frame = its start stamp to its end stamp,
//   GPU idle before it = the last frame's end to this frame's start (time the
//     GPU sat waiting for the CPU; near 0 every frame means GPU-bound),
//   GPU done after Present = how long after the CPU's Present call the GPU
//     finished, measured at the Present itself.
// gpu_timer=0 in hole-vr.ini turns it off.
namespace holevr::vr::gpu_timer {

// The game's swap chain and queue were found: hooks the queue's
// ExecuteCommandLists. Once.
void Init(ID3D12CommandQueue* queue);

// In the game's Present hook, before anything else is queued: end stamp.
void PresentBegin();
// After the game's real Present returned: the next submit gets a start stamp.
void PresentEnd();

// Whether the timer runs.
bool On();

// GPU frames measured since the last call (any thread).
struct Window {
    double frame_ms = 0.0;  // summed
    long frames = 0;
    double idle_ms = 0.0;
    long idles = 0;
};
Window TakeWindow();

}  // namespace holevr::vr::gpu_timer
