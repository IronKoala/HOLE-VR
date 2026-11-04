// Native tests for the hands-on workbench's release rules (no game, no Windows).
//   g++ -std=c++20 -Isrc tests/test_workbench_rules.cpp -o /tmp/test_workbench_rules && /tmp/test_workbench_rules
//
// Outside review 2026-11-03, finding 1: take light A off the gun with one
// hand, fit light B in the same slot with the other, then let go of A away
// from the gun. A was still treated as the gun's own part, so letting it go
// emptied the slot and took B off again.
#include <cstdio>
#include <vector>

#include "vr/workbench_rules.h"

using holevr::game::custom::SavedPart;
using holevr::ue::FName;
using namespace holevr::vr::workbench::rules;

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

const FName kSight{11, 0}, kLightA{21, 0}, kLightB{22, 0}, kEmpty{5, 0}, kAmmo{1, 0}, kMag{2, 0};
constexpr int kSightSlot = 3, kLightSlot = 7;

std::vector<SavedPart> Loadout(const FName& light) {
    return {{kAmmo, {0}}, {kMag, {1}}, {kSight, {kSightSlot}}, {light, {kLightSlot}}};
}

// What letting go does, the way vr/workbench.cpp's Release asks.
LetGo LetGoOf(const std::vector<SavedPart>& saved, bool taken_off_gun, const FName& held, int from_slot, int snapped,
              bool slot_can_be_empty) {
    LetGoFacts f;
    f.from_gun = taken_off_gun && StillFitted(saved, from_slot, held);
    f.from_slot = f.from_gun ? from_slot : -1;
    f.snapped = snapped;
    f.have_gun = true;
    f.slot_can_be_empty = f.from_gun && slot_can_be_empty;
    return DecideLetGo(f);
}
}  // namespace

int main() {
    // The light taken off and let go away from the gun: the slot is emptied.
    Check(LetGoOf(Loadout(kLightA), true, kLightA, kLightSlot, -1, true) == LetGo::kEmptySlot,
          "light taken off, let go away: the game empties its slot");
    // Taken off and let go on its own slot: nothing changes.
    Check(LetGoOf(Loadout(kLightA), true, kLightA, kLightSlot, kLightSlot, true) == LetGo::kPutBack,
          "light taken off, let go on its slot: put back, nothing changes");
    // The sight can't be left empty: it goes back on.
    Check(LetGoOf(Loadout(kLightA), true, kSight, kSightSlot, -1, false) == LetGo::kBackOnGun,
          "sight taken off, let go away: back on the gun (never empty)");
    // A part picked up from the table.
    Check(LetGoOf(Loadout(kEmpty), false, kLightA, -1, -1, false) == LetGo::kToTable,
          "part from the table, let go away: back on the table");
    Check(LetGoOf(Loadout(kEmpty), false, kLightA, -1, kLightSlot, false) == LetGo::kFit,
          "part from the table, let go on the slot: fitted");

    // Finding 1: A taken off (left hand), B fitted in the same slot (right
    // hand). The saved list now has B there.
    const std::vector<SavedPart> after_b = Loadout(kLightB);
    Check(!StillFitted(after_b, kLightSlot, kLightA), "after B is fitted, A is not the gun's part any more");
    Check(StillFitted(after_b, kLightSlot, kLightB), "B is the gun's part in the light slot");
    const LetGo away = LetGoOf(after_b, true, kLightA, kLightSlot, -1, true);
    Check(away == LetGo::kToTable, "then A let go away from the gun: A goes on the table (B stays fitted)");
    Check(away != LetGo::kEmptySlot, "then A let go away from the gun: the slot is NOT emptied");
    Check(LetGoOf(after_b, true, kLightA, kLightSlot, kLightSlot, true) == LetGo::kFit,
          "then A let go on the light slot: A is fitted for real (not a silent no-op)");

    // A slot that is in the list twice never happens; one that is missing
    // means not fitted.
    Check(!StillFitted({{kAmmo, {0}}}, kLightSlot, kLightA), "slot missing from the list: not fitted");
    Check(!StillFitted({{kLightA, {kLightSlot, 2}}}, kLightSlot, kLightA), "sub-slot paths are not the top slot");

    std::printf(g_fail ? "%d workbench rule test(s) FAILED\n" : "all workbench rule tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
