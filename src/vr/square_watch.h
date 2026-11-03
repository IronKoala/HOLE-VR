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
//
// It also counts lone bright dots in each eye (CountDots): what the owner's
// 10:47 screenshot actually shows. A strip with many more in one eye is saved
// as capture-dots-N-strip.bmp, and the whole picture at full size from the
// next strips as capture-dots-N.bmp (at most 3 per run). LatestSweep gives
// the latest whole-picture counts to square_test, which hunts for the cause.
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

// Lone dots per eye over the whole picture, from strips read since
// `since_ms` (perf::NowMs clock). True once every strip of the picture has
// been read since then (a full sweep, about 15 frames).
struct Sweep {
    int dots[2] = {0, 0};
    float grid_share[2] = {0.f, 0.f};
    int strips = 0;  // strips in the picture
    int fresh = 0;   // of them read since since_ms
    int Odd() const { return dots[0] > dots[1] ? dots[0] - dots[1] : dots[1] - dots[0]; }
    int Eye() const { return dots[1] > dots[0] ? 1 : 0; }
};
bool LatestSweep(double since_ms, Sweep* out);

}  // namespace holevr::vr::square_watch
