// Native tests for the hands-on workbench's release rules (no game, no Windows).
//   g++ -std=c++20 -Isrc tests/test_workbench_rules.cpp -o /tmp/test_workbench_rules && /tmp/test_workbench_rules
//
// Outside review 2026-11-03, finding 1: take light A off the gun with one
// hand, fit light B in the same slot with the other, then let go of A away
// from the gun. A was still treated as the gun's own part, so letting it go
// emptied the slot and took B off again.
#include <algorithm>
#include <cmath>
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

    // The green box (owner 2026-11-04 07:57): twice as tall (52 cm), solid
    // edge lines, colour fading out within 4 cm, the middle of each side clear.
    {
        const double w = 34, d = 14, h = 52, line = 0.6, fade = 4, thin = 0.1;
        const int levels = 5;
        const auto frame = BoxFrame(w, d, h, line, fade, levels, thin);
        int lines = 0, strips = 0;
        bool inside = true, ring_ok = true;
        const double dim[3] = {w, d, h};
        for (const auto& p : frame) {
            (p.level == 0 ? lines : strips)++;
            for (int a = 0; a < 3; ++a)
                if (std::fabs(p.at[a]) + p.size[a] * 0.5 > dim[a] * 0.5 + line * 0.5 + 1e-9) inside = false;
            if (p.level == 0) continue;
            // A strip lies on one face (its thin axis at the face), between
            // r0 and r1 in from that face's edges on one in-plane axis.
            int n = -1;
            for (int a = 0; a < 3; ++a)
                if (std::fabs(p.size[a] - thin) < 1e-9 && std::fabs(std::fabs(p.at[a]) - dim[a] * 0.5) < 1e-9) n = a;
            if (n < 0) {
                ring_ok = false;
                continue;
            }
            const double band = fade / (levels - 1), r0 = line * 0.5 + (p.level - 1) * band;
            bool on_ring = false;
            for (int a = 0; a < 3; ++a) {
                if (a == n) continue;
                const double from_edge = dim[a] * 0.5 - (std::fabs(p.at[a]) + p.size[a] * 0.5);
                if (std::fabs(p.size[a] - band) < 1e-9 && std::fabs(from_edge - r0) < 1e-9) on_ring = true;
            }
            if (!on_ring) ring_ok = false;
        }
        Check(lines == 12, "box: 12 solid edge lines");
        Check(strips == 6 * (levels - 1) * 4, "box: 4 fading rings of 4 strips on each of the 6 sides");
        Check(inside, "box: every piece stays on the box");
        Check(ring_ok, "box: each fading strip sits on its ring, the fainter ones further in");
        // No two strips of one ring on one side overlap (the corners are not
        // drawn twice, which would show as brighter squares).
        bool overlap = false;
        for (size_t i = 0; i < frame.size(); ++i)
            for (size_t j = i + 1; j < frame.size(); ++j) {
                const auto &p = frame[i], &q = frame[j];
                if (p.level == 0 || p.level != q.level) continue;
                double vol = 1;
                for (int a = 0; a < 3; ++a) {
                    const double lo = std::max(p.at[a] - p.size[a] * 0.5, q.at[a] - q.size[a] * 0.5);
                    const double hi = std::min(p.at[a] + p.size[a] * 0.5, q.at[a] + q.size[a] * 0.5);
                    vol *= std::max(0.0, hi - lo);
                }
                if (vol > 1e-6) overlap = true;
            }
        Check(!overlap, "box: strips of one ring never overlap");
        // The middle of every side is clear: nothing but the lines comes
        // further in than line/2 + fade from a side's edges.
        bool clear = true;
        for (const auto& p : frame) {
            if (p.level == 0) continue;
            for (int a = 0; a < 3; ++a) {
                if (p.size[a] <= thin + 1e-9) continue;
                const double reach = dim[a] * 0.5 - (std::fabs(p.at[a]) - p.size[a] * 0.5);
                // A strip that runs along this axis spans it; one across it
                // must stay within the fade band.
                if (p.size[a] < dim[a] - 2 * (line * 0.5 + fade) - 1e-9 && reach > line * 0.5 + fade + 1e-9) clear = false;
            }
        }
        // Points on each side just past the fade (and its very middle) are
        // covered by no piece; points inside the fade are.
        auto covered = [&](const double pt[3]) {
            for (const auto& p : frame) {
                bool in = true;
                for (int a = 0; a < 3; ++a)
                    if (std::fabs(pt[a] - p.at[a]) > p.size[a] * 0.5 + 1e-9) in = false;
                if (in) return true;
            }
            return false;
        };
        bool past_clear = true, fade_drawn = true;
        for (int n = 0; n < 3; ++n) {
            const int a = (n + 1) % 3, b = (n + 2) % 3;
            for (int side = -1; side <= 1; side += 2) {
                for (double in_cm : {line * 0.5 + fade + 0.2, dim[a] * 0.5}) {
                    double pt[3];
                    pt[n] = side * dim[n] * 0.5, pt[b] = 0;
                    pt[a] = dim[a] * 0.5 - std::min(in_cm, dim[a] * 0.5);
                    if (covered(pt)) past_clear = false;
                }
                double pt[3];
                pt[n] = side * dim[n] * 0.5, pt[b] = 0, pt[a] = dim[a] * 0.5 - (line * 0.5 + fade * 0.5);
                if (!covered(pt)) fade_drawn = false;
            }
        }
        Check(clear && past_clear, "box: the middle of every side is clear (the colour is gone within 4 cm of the lines)");
        Check(fade_drawn, "box: inside the fade band every side is drawn");
        Check(BoxFrame(w, d, 26, line, fade, levels, thin).size() == frame.size(), "box: a shorter box has the same pieces");
    }

    std::printf(g_fail ? "%d workbench rule test(s) FAILED\n" : "all workbench rule tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
