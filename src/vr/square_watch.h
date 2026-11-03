#pragma once
// Watches the game's picture for white squares and bars drawn into one eye
// only, and saves a copy when they show, so what draws them can be seen.
//
// Each headset frame one strip of the picture (both eyes, 96 rows; the whole
// picture every 15 frames) is copied back from the GPU without waiting for
// it, and a background thread compares the two eyes (square_watch_math.h).
// When one eye has many bright blocks the other has nowhere near, the strip
// is saved as capture-squares-N-strip.bmp and the next strips are put
// together into capture-squares-N.bmp (at most 5 per run). The oddest strip
// of the run is kept as capture-squares-worst-strip.bmp. Only with the
// player in the world and no forearm menu up (the owner sees the squares
// during runs). square_watch=0 in hole-vr.ini turns it off.
#include <windows.h>
#include <d3d12.h>

namespace holevr::vr::square_watch {

void Init();

// Render thread, in the copy to the headset, while `source` (the game's
// picture, both eyes side by side) is in the copy-source state.
void Record(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* source);

// Render thread, once those commands are submitted: `fence` reaches `value`
// when the GPU is done with them.
void Submitted(ID3D12Fence* fence, UINT64 value);

}  // namespace holevr::vr::square_watch
