#pragma once
// The hands-on workbench's decisions that need no engine, so they can be
// tested on the host (tests/test_workbench_rules.cpp). vr/workbench.cpp acts
// on them.
#include <vector>
#include <utility>

#include "game/weapon_custom.h"
#include "vr/hands.h"

namespace holevr::vr::workbench::rules {

// InstalledParts is authoritative after a rebuild. The old actor may still
// have a live UObject slot while waiting for garbage collection.
inline ue::UObject FittedActor(const std::vector<game::custom::Installed>& installed, int slot,
                               const std::string& row) {
    for (const auto& p:installed)
        if (p.path.size()==1 && p.path[0]==slot && p.row==row) return p.actor;
    return 0;
}

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

// Controls resolves both candidates before either hand actually grabs.
// Recheck the current actor and logical source slot at acquisition time.
template<class Holds>
bool CanTakeFittedPart(const std::vector<game::custom::Installed>& installed, int slot,
                      const std::string& row, ue::UObject actor, const Holds& holds) {
    if (!actor || FittedActor(installed,slot,row) != actor) return false;
    for (const auto& held : holds)
        if (held.active && held.from_gun && held.from_slot == slot) return false;
    return true;
}
template<class Items>
int LooseIndex(const Items& items, ue::UObject actor) {
    if (!actor) return -1;
    for (size_t i=0;i<items.size();++i) if (items[i].actor == actor) return static_cast<int>(i);
    return -1;
}
inline bool HoldsTargetGun(ue::UObject gun, const hands::HandGrab& grab) {
    const auto kind = hands::HeldGrabKind(grab);
    return gun && grab.object == gun &&
           (kind == hands::GrabKind::kGunGrip || kind == hands::GrabKind::kSupportGrip);
}

// Moving one held attachment between mounts is one saved-loadout edit:
// fill the destination AND empty its old mount. A failed rebuild can then
// restore both slots through the existing transaction. Ammo paths stay intact.
inline bool SlotEdit(const std::vector<game::custom::SavedPart>& before, int target, const ue::FName& row,
                     int source, const ue::FName& empty, std::vector<game::custom::SavedPart>* out) {
    if (target <= 1 || target > 255 || source < -1 || source > 255 || (source >= 0 && source <= 1)) return false;
    const bool moving = source >= 0 && source != target;
    if (moving && (!empty.comparison_index || !StillFitted(before,source,row))) return false;
    auto edited = before;
    auto put = [&](int slot,const ue::FName& name) {
        for (auto& p:edited) if (p.path.size()==1 && p.path[0]==slot) { p.row=name; return; }
        edited.push_back({name,{static_cast<uint8_t>(slot)}});
    };
    put(target,row);
    if (moving) put(source,empty);
    *out = std::move(edited);
    return true;
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
    if (f.snapped >= 0 && f.have_gun) {
        if (f.from_gun && f.snapped == f.from_slot) return LetGo::kPutBack;
        if (f.from_gun && !f.slot_can_be_empty) return LetGo::kBackOnGun;
        return LetGo::kFit;
    }
    if (f.from_gun) return f.have_gun && f.slot_can_be_empty ? LetGo::kEmptySlot : LetGo::kBackOnGun;
    return LetGo::kToTable;
}

// The green box's frame (owner 2026-11-04: solid edge lines, the colour
// fading out quickly toward the middle of each side, the middle clear). In
// the box's own frame, cm, centred on the box: axis 0 = width w, 1 = depth d,
// 2 = height h. Level 0 = the 12 edge lines (`line` thick, centred on the
// edges); levels 1..levels-1 = rings of thin strips on each face stepping in
// from the lines, `fade` / (levels - 1) wide each, no two strips of one ring
// overlapping.
struct BoxPiece {
    int level = 0;
    double at[3] = {0, 0, 0};
    double size[3] = {0, 0, 0};
};

inline std::vector<BoxPiece> BoxFrame(double w, double d, double h, double line, double fade, int levels,
                                      double thin = 0.1) {
    std::vector<BoxPiece> out;
    const double dim[3] = {w, d, h};
    auto add = [&](int level, double a0, double a1, double a2, double s0, double s1, double s2) {
        if (s0 <= 0.01 || s1 <= 0.01 || s2 <= 0.01) return;
        BoxPiece p;
        p.level = level;
        p.at[0] = a0, p.at[1] = a1, p.at[2] = a2;
        p.size[0] = s0, p.size[1] = s1, p.size[2] = s2;
        out.push_back(p);
    };
    // The 12 edges: for each axis, the 4 edges running along it.
    for (int ax = 0; ax < 3; ++ax) {
        const int b = (ax + 1) % 3, c = (ax + 2) % 3;
        for (int i = 0; i < 4; ++i) {
            double at[3], sz[3];
            at[ax] = 0;
            at[b] = ((i & 1) ? 0.5 : -0.5) * dim[b];
            at[c] = ((i & 2) ? 0.5 : -0.5) * dim[c];
            sz[ax] = dim[ax] + line;
            sz[b] = sz[c] = line;
            add(0, at[0], at[1], at[2], sz[0], sz[1], sz[2]);
        }
    }
    if (levels < 2) return out;
    const double band = fade / (levels - 1);
    // Each face: normal axis n, in-plane axes a and b.
    for (int n = 0; n < 3; ++n) {
        const int a = (n + 1) % 3, b = (n + 2) % 3;
        for (int side = -1; side <= 1; side += 2) {
            for (int k = 1; k < levels; ++k) {
                const double r0 = line * 0.5 + (k - 1) * band, r1 = r0 + band;
                for (int e = -1; e <= 1; e += 2) {
                    double at[3], sz[3];
                    // Strip along a, the ring's full width.
                    at[n] = side * dim[n] * 0.5, at[a] = 0, at[b] = e * (dim[b] * 0.5 - r0 - band * 0.5);
                    sz[n] = thin, sz[a] = dim[a] - 2 * r0, sz[b] = band;
                    add(k, at[0], at[1], at[2], sz[0], sz[1], sz[2]);
                    // Strip along b, between those two.
                    at[n] = side * dim[n] * 0.5, at[b] = 0, at[a] = e * (dim[a] * 0.5 - r0 - band * 0.5);
                    sz[n] = thin, sz[b] = dim[b] - 2 * r1, sz[a] = band;
                    add(k, at[0], at[1], at[2], sz[0], sz[1], sz[2]);
                }
            }
        }
    }
    return out;
}

}  // namespace holevr::vr::workbench::rules
