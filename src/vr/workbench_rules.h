#pragma once
// The hands-on workbench's decisions that need no engine, so they can be
// tested on the host (tests/test_workbench_rules.cpp). vr/workbench.cpp acts
// on them.
#include <vector>

#include "game/weapon_custom.h"

namespace holevr::vr::workbench::rules {

// Is `row` still the gun's own part in `slot`, going by the saved list (what
// the game builds the gun from)? A part a hand took off the gun stays the
// gun's own (only hidden) until it is let go. Once another part has been
// fitted in that slot (with the other hand), the one in the hand is not the
// gun's any more: letting it go must not empty the slot.
inline bool StillFitted(const std::vector<game::custom::SavedPart>& saved, int slot, const ue::FName& row) {
    for (const auto& p : saved)
        if (p.path.size() == 1 && p.path[0] == slot) return game::custom::SameName(p.row, row);
    return false;
}

// What letting go of a held part does.
enum class LetGo {
    kPutBack,    // taken off the gun, let go on its own slot: nothing changes
    kFit,        // let go lined up with a slot: the game fits it there
    kEmptySlot,  // taken off the gun, let go away from it: the game empties the slot
    kBackOnGun,  // taken off a slot the game never leaves empty: it goes back on
    kToTable,    // anything else: it lies on the table
};

struct LetGoFacts {
    bool from_gun = false;           // still the gun's own part (StillFitted)
    int from_slot = -1;              // the slot it was taken from
    int snapped = -1;                // slot it lines up with now, -1 none
    bool have_gun = false;           // a gun to fit it to (on the table or in the right hand)
    bool slot_can_be_empty = false;  // from_slot may be left empty
};

inline LetGo DecideLetGo(const LetGoFacts& f) {
    if (f.snapped >= 0 && f.have_gun)
        return f.from_gun && f.snapped == f.from_slot ? LetGo::kPutBack : LetGo::kFit;
    if (f.from_gun) return f.have_gun && f.slot_can_be_empty ? LetGo::kEmptySlot : LetGo::kBackOnGun;
    return LetGo::kToTable;
}

}  // namespace holevr::vr::workbench::rules
