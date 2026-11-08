#include "vr/interact/holster_feedback.h"
#include <algorithm>
#include <cmath>

namespace holevr::vr::interact {
HolsterCues HolsterFeedback::Update(const HolsterFeedbackInput& in) {
    if (!in.enabled || !in.pistol || !std::isfinite(in.hood_open)) { Reset(); return 0; }
    HolsterCues out = 0;
    // Initial load, new pistol/player, and re-enabling are quiet.
    if (ready_ && last_.pistol == in.pistol) {
        if (in.hood_wants_open != last_.hood_wants_open) {
            if (in.hood_wants_open && last_.hood_open < 1.0 - 1e-6)
                out |= unsigned(HolsterCue::kHoodOpen);
            if (!in.hood_wants_open && last_.hood_open > 1e-6)
                out |= unsigned(HolsterCue::kHoodClose);
        }
        using O = PistolOccupancy;
        // Picking up a parked gun is not a draw from this holster.
        if (in.occupancy == O::kInHand && (last_.occupancy == O::kSeated || last_.occupancy == O::kSettling))
            out |= unsigned(HolsterCue::kDraw);
        // Sound on arrival, not release. Returning/settling can take many frames.
        if (in.occupancy == O::kSeated && (last_.occupancy == O::kInHand ||
            last_.occupancy == O::kReturning || last_.occupancy == O::kSettling))
            out |= unsigned(HolsterCue::kHolster);
    }
    last_ = in;
    last_.hood_open = std::clamp(in.hood_open, 0.0, 1.0);
    ready_ = true;
    return out;
}

const char* HolsterCueName(HolsterCue cue) {
    switch (cue) {
        case HolsterCue::kHoodOpen: return "hood_open";
        case HolsterCue::kHoodClose: return "hood_close";
        case HolsterCue::kDraw: return "draw";
        case HolsterCue::kHolster: return "holster";
    }
    return "unknown";
}
namespace { HolsterCueSink g_sink = nullptr; }
void SetHolsterCueSink(HolsterCueSink sink) { g_sink = sink; }
void DispatchHolsterCues(HolsterCues cues, uintptr_t player, const FVector& world_cm) {
    if (!g_sink || !player || !std::isfinite(world_cm.X) ||
        !std::isfinite(world_cm.Y) || !std::isfinite(world_cm.Z)) return;
    for (HolsterCue cue : {HolsterCue::kHoodOpen, HolsterCue::kHoodClose, HolsterCue::kDraw, HolsterCue::kHolster})
        if (cues & unsigned(cue)) g_sink(cue, player, world_cm);
}
}  // namespace holevr::vr::interact
