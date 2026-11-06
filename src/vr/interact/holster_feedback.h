#pragma once
// One-shot, spatial sound events shared by the holster and its audio player.
#include <cstdint>
#include "vr/math.h"

namespace holevr::vr::interact {
enum class HolsterCue : unsigned { kHoodOpen = 1, kHoodClose = 2, kDraw = 4, kHolster = 8 };
using HolsterCues = unsigned;
enum class PistolOccupancy { kSeated, kSettling, kInHand, kReturning, kParked };
struct HolsterFeedbackInput {
    bool enabled = false;
    uintptr_t pistol = 0;
    PistolOccupancy occupancy = PistolOccupancy::kParked;
    bool hood_wants_open = true;
    double hood_open = 1.0;
};

class HolsterFeedback {
public:
    HolsterCues Update(const HolsterFeedbackInput& in);
    void Reset() { *this = HolsterFeedback{}; }
private:
    bool ready_ = false;
    HolsterFeedbackInput last_;
};

const char* HolsterCueName(HolsterCue cue);
// Register before ticks begin; dispatch on the game thread. Missing clips or
// backend remain silent; no global UI sounds.
using HolsterCueSink = void (*)(HolsterCue cue, uintptr_t player, const FVector& world_cm);
void SetHolsterCueSink(HolsterCueSink sink);
void DispatchHolsterCues(HolsterCues cues, uintptr_t player, const FVector& world_cm);
}  // namespace holevr::vr::interact
