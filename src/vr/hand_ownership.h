#pragma once
// One game-frame reservation shared by controls, attachments and drinks.
// A release never hands the same input edge to another feature later in the tick.

namespace holevr::vr::hands {

enum class GripOwner { kNone, kControls, kWorkbench, kDrink, kUi };

struct GripFacts {
    bool controls_held = false;
    bool attachment_held = false;
    bool drink_held = false;
    bool attachment_candidate = false;
    bool drink_candidate = false;
    bool actions_allowed = true;
};

struct HandInteraction {
    GripOwner owner = GripOwner::kNone;
    bool actions_allowed = false;

    bool CanAcquire(GripOwner feature) const { return actions_allowed && owner == feature; }
    bool AllowsControls() const {
        return actions_allowed && (owner == GripOwner::kNone || owner == GripOwner::kControls);
    }
};

inline HandInteraction ResolveGrip(const GripFacts& f) {
    // Existing holds win over proximity, even when tracking temporarily fails.
    // Menu/editor ownership blocks new actions but does not discard held objects:
    // their owning feature still receives raw button-up for release/cleanup.
    GripOwner owner = GripOwner::kNone;
    if (f.controls_held) owner = GripOwner::kControls;
    else if (f.attachment_held) owner = GripOwner::kWorkbench;
    else if (f.drink_held) owner = GripOwner::kDrink;
    else if (!f.actions_allowed) owner = GripOwner::kUi;
    else if (f.attachment_candidate) owner = GripOwner::kWorkbench;
    else if (f.drink_candidate) owner = GripOwner::kDrink;
    return {owner, f.actions_allowed};
}

inline HandInteraction AfterControls(HandInteraction reserved, bool controls_held) {
    // Controls may have acquired/released a gun, slide or magazine since the
    // reservation. Publish that result before downstream features use the edge.
    if (controls_held) reserved.owner = GripOwner::kControls;
    else if (reserved.owner == GripOwner::kControls)
        reserved.owner = reserved.actions_allowed ? GripOwner::kNone : GripOwner::kUi;
    return reserved;
}

}  // namespace holevr::vr::hands
