// Shotgun smart reload and the two-per-push sweep (vr/interact/shell_load.h).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "vr/interact/shell_load.h"

using namespace holevr::vr;
using namespace holevr::vr::interact::shells;

namespace {
int checks = 0;
void Check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}
bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

Tube T(int capacity, int in_tube, int chamber = 1, bool bolt_open = false) {
    Tube t;
    t.capacity = capacity;
    t.in_tube = in_tube;
    t.chamber = chamber;
    t.bolt_open = bolt_open;
    return t;
}

// The owner's table (2026-11-07): missing 4+ -> 4, 2-3 -> 2, 1 -> 1, 0 -> 0.
void SmartTable() {
    const Settings s;
    Check(SmartCount(s, T(9, 0)) == 4, "9-shell tube empty: quad");
    Check(SmartCount(s, T(9, 5)) == 4, "missing 4: quad");
    Check(SmartCount(s, T(9, 6)) == 2, "missing 3: twin");
    Check(SmartCount(s, T(9, 7)) == 2, "missing 2: twin");
    Check(SmartCount(s, T(9, 8)) == 1, "missing 1: single");
    Check(SmartCount(s, T(9, 9)) == 0, "full: nothing");
    Check(SmartCount(s, T(2, 0)) == 2, "2-shell tube empty: one twin");
    Check(SmartCount(s, T(5, 0)) == 4, "5-shell tube empty: quad");
    Check(SmartCount(s, T(5, 4)) == 1, "5-shell tube, one missing: single");
    // The chamber doesn't count.
    Check(SmartCount(s, T(7, 6, 0)) == 1, "chamber empty, tube missing 1: single");
    Check(SmartCount(s, T(7, 6, 1)) == 1, "chamber full, tube missing 1: single");
    // Unknown capacity: nothing (logged by the caller).
    Check(SmartCount(s, T(0, 0)) == 0, "no capacity read: nothing");
    // More in the tube than its capacity (a bad read): nothing.
    Check(SmartCount(s, T(5, 7)) == 0, "over-full read: nothing");
}

// A 9-shell tube from empty is quad, quad, single.
void NineFromEmpty() {
    const Settings s;
    Tube t = T(9, 0);
    int grabs[3];
    for (int g = 0; g < 3; ++g) {
        grabs[g] = SmartCount(s, t);
        t.in_tube += grabs[g];
    }
    Check(grabs[0] == 4 && grabs[1] == 4 && grabs[2] == 1, "9 from empty: quad, quad, single");
    Check(SmartCount(s, t) == 0, "then full");
}

void SmartOff() {
    Settings s;
    s.smart = false;
    Check(SmartCount(s, T(9, 8)) == 4, "smart off: always 4");
    s.fixed = 9;
    Check(SmartCount(s, T(9, 0)) == 4, "smart off: never more than 4 in a hand");
}

// Bolt locked open on an empty gun: the tube is locked, one shell for the port.
void EmptyGun() {
    Settings s;
    Check(TubeLocked(s, T(7, 0, 0, true)), "bolt open: tube locked");
    Check(!TubeLocked(s, T(7, 0, 0, false)), "bolt closed: tube open");
    Check(SmartCount(s, T(7, 0, 0, true)) == 1, "empty gun, bolt open: one shell for the port");
    Check(PortOpen(T(7, 0, 0, true)), "empty gun, bolt open: port open");
    // Port loaded (waiting on the lifter for B): nothing more until B.
    Check(!PortOpen(T(7, 1, 0, true)), "shell on the lifter: port closed");
    Check(SmartCount(s, T(7, 1, 0, true)) == 0, "shell on the lifter: nothing (press B)");
    // After B: chamber 1, tube 0 -> the table again.
    Check(SmartCount(s, T(7, 0, 1, false)) == 4, "after B: quad");
    // shotgun_empty_gun_single=0: the table's number even with the bolt open.
    s.empty_gun_single = false;
    Check(SmartCount(s, T(7, 0, 0, true)) == 4, "single off: quad on an empty, open gun");
    // shotgun_tube_needs_closed_bolt=0: the tube takes shells with the bolt open.
    s = Settings{};
    s.tube_needs_closed_bolt = false;
    Check(!TubeLocked(s, T(7, 0, 0, true)), "lock off: tube open with the bolt open");
    Check(SmartCount(s, T(7, 0, 0, true)) == 4, "lock off: quad on an empty, open gun");
}

void Holds() {
    const FVector none{};
    // Single: in the middle of the fist, rim towards the thumb.
    FVector b = HoldBase(1, 0, none);
    Check(Near(b.Y, 0) && Near(b.Z, 0) && b.X > 0, "single: middle of the fist, rim forward");
    Check(Near(HoldNoseDir().X, -1), "noses point out past the little finger");
    // Twin: one above the other, the first on top.
    Check(HoldBase(2, 0, none).Z > HoldBase(2, 1, none).Z, "twin: first shell on top");
    Check(Near(HoldBase(2, 0, none).Y, HoldBase(2, 1, none).Y), "twin: stacked");
    // Quad: two pairs, the first pair (slots 0 and 1) on top, side by side.
    for (int i = 0; i < 2; ++i)
        Check(HoldBase(4, i, none).Z > HoldBase(4, i + 2, none).Z, "quad: first pair on top");
    Check(Near(HoldBase(4, 0, none).Z, HoldBase(4, 1, none).Z), "quad: a pair side by side");
    Check(Near(std::fabs(HoldBase(4, 0, none).Y - HoldBase(4, 1, none).Y), kSpacing), "quad: a rim apart");
    // Every pair of slots a shell width apart at least (no overlap).
    for (int i = 0; i < 4; ++i)
        for (int j = i + 1; j < 4; ++j) {
            const FVector d = HoldBase(4, i, none) - HoldBase(4, j, none);
            Check(std::sqrt(d.X * d.X + d.Y * d.Y + d.Z * d.Z) >= kSpacing - 1e-9, "quad: shells don't overlap");
        }
    // The nudge moves the hold.
    const FVector n{1, 2, 3};
    const FVector m = HoldBase(4, 3, n) - HoldBase(4, 3, none);
    Check(Near(m.X, 1) && Near(m.Y, 2) && Near(m.Z, 3), "nudge moves the hold");
    // Out of range slots clamp.
    Check(Near(HoldBase(2, 5, none).Z, HoldBase(2, 1, none).Z), "slot clamps");
    Check(std::string_view(HoldName(4)) == "quad" && std::string_view(HoldName(2)) == "twin" &&
              std::string_view(HoldName(1)) == "single",
          "hold names");
}

// One sweep along the tube (X), nose starting `start` cm before the mouth.
StepIn At(double along, double side, int held, bool room = true, bool locked = false) {
    StepIn in;
    in.nose = FVector{along, side, 0};
    in.nose_dir = FVector{1, 0, 0};
    in.axis = FVector{1, 0, 0};
    in.held = held;
    in.room = room;
    in.locked = locked;
    return in;
}

void Capture() {
    const Settings s;
    Sweep w;
    Check(Advance(w, s, At(-10, 0, 4)) == Step::kNone, "far behind the mouth: nothing");
    Check(Advance(w, s, At(-1, 6, 4)) == Step::kNone, "off to the side: nothing");
    StepIn bad = At(-1, 0, 4);
    bad.nose_dir = FVector{0, 0, 1};
    Check(Advance(w, s, bad) == Step::kNone, "pointing up, not along the tube: nothing");
    StepIn tilted = At(-1, 0, 4);
    tilted.nose_dir = FVector{std::cos(30 * 3.14159265358979 / 180), 0, std::sin(30 * 3.14159265358979 / 180)};
    Check(Advance(w, s, tilted) == Step::kCaptured, "30 deg off the tube: caught");
    Check(w.captured && w.pair == 2 && Near(w.offset, -1), "caught: a pair, from where its nose was");
    Sweep one;
    Check(Advance(one, s, At(0, 0, 1)) == Step::kCaptured && one.pair == 1, "a single is a pair of one");
    Sweep none;
    Check(Advance(none, s, At(0, 0, 0)) == Step::kNone, "no shells: nothing to catch");
}

// The owner's "two per push": one sweep loads the pair, first at 5.5 cm,
// second at 12.5 cm.
void TwoPerPush() {
    const Settings s;
    Sweep w;
    Check(Advance(w, s, At(-2, 0, 4)) == Step::kCaptured, "caught 2 cm before the mouth");
    Check(Advance(w, s, At(2, 0, 4)) == Step::kNone, "4 cm in: nothing yet");
    Check(Near(ShownDepth(w, s), 4), "drawn 4 cm in");
    Check(Advance(w, s, At(3.5, 0, 4)) == Step::kCommit, "5.5 cm: first shell in");
    Check(w.captured && w.loaded == 1, "still pushing the pair");
    Check(Near(ShownDepth(w, s), 0), "second shell at the mouth");
    Check(Advance(w, s, At(6, 0, 3)) == Step::kNone, "8 cm: second on its way");
    Check(Near(ShownDepth(w, s), 2.5), "second drawn 2.5 cm in");
    Check(Advance(w, s, At(10.5, 0, 3)) == Step::kCommit, "12.5 cm: second shell in");
    Check(!w.captured, "pair done: the next pair needs its own push");
    // The next pair is caught again only at the mouth.
    Check(Advance(w, s, At(10.5, 0, 2)) == Step::kNone, "hand still forward: not caught");
    Check(Advance(w, s, At(0.5, 0, 2)) == Step::kCaptured, "back at the mouth: caught");
}

// A single loads on its own and ends the sweep.
void SingleAlone() {
    const Settings s;
    Sweep w;
    Advance(w, s, At(0, 0, 1));
    Check(Advance(w, s, At(5.5, 0, 1)) == Step::kCommit && !w.captured, "single: in at 5.5 cm, sweep over");
}

// Stopping after the first shell: the second stays in the hand, ready.
void StopHalfway() {
    const Settings s;
    Sweep w;
    Advance(w, s, At(0, 0, 2));
    Check(Advance(w, s, At(6, 0, 2)) == Step::kCommit, "first in");
    // The hand comes back a bit: still on the line, nothing more goes in.
    Check(Advance(w, s, At(4, 0, 1)) == Step::kNone && w.captured, "back a bit: still caught");
    Check(Near(ShownDepth(w, s), 0), "second waits at the mouth");
    // and pushes again: the second goes in.
    Check(Advance(w, s, At(12.5, 0, 1)) == Step::kCommit && !w.captured, "pushed again: second in");
    // Pulling right back out lets go with the shell still in the hand.
    Sweep v;
    Advance(v, s, At(0, 0, 2));
    Check(Advance(v, s, At(-3.5, 0, 2)) == Step::kLetGo && !v.captured, "pulled back out: let go");
}

void LeaveTheLine() {
    const Settings s;
    Sweep w;
    Advance(w, s, At(0, 0, 4));
    Check(Advance(w, s, At(2, 4.9, 4)) == Step::kNone, "4.9 cm off the line: still caught");
    Check(Advance(w, s, At(2, 5.1, 4)) == Step::kLetGo, "5.1 cm off the line: let go");
}

// A full tube (or one locked by the open bolt) stops the shell 1 cm in, once.
void Blocked() {
    const Settings s;
    Sweep w;
    Advance(w, s, At(0, 0, 4, false));
    Check(Advance(w, s, At(0.5, 0, 4, false)) == Step::kNone, "half a cm: not blocked yet");
    Check(Advance(w, s, At(3, 0, 4, false)) == Step::kBlocked, "full tube: blocked at 1 cm");
    Check(Near(ShownDepth(w, s), 1.0), "held at 1 cm");
    Check(Advance(w, s, At(8, 0, 4, false)) == Step::kNone, "told once a sweep");
    Check(Near(ShownDepth(w, s), 1.0), "still held at 1 cm");
    // One slot left during a pair: the first goes in, the second blocks.
    Sweep v;
    Advance(v, s, At(0, 0, 2, true));
    Check(Advance(v, s, At(6, 0, 2, true)) == Step::kCommit, "last slot: first in");
    Check(Advance(v, s, At(9, 0, 1, false)) == Step::kBlocked, "then the tube is full: second blocks");
    Check(Near(ShownDepth(v, s), 1.0), "second stops 1 cm in");
    // Bolt open: locked.
    Sweep u;
    Advance(u, s, At(0, 0, 1, true, true));
    Check(Advance(u, s, At(2, 0, 1, true, true)) == Step::kBlocked, "bolt open: tube locked");
}

void Port() {
    const Settings s;
    Check(AtEjectionPort({1, 2, 3}, {1, 2, 6.9}, s), "3.9 cm from the ejection port: drops in");
    Check(!AtEjectionPort({1, 2, 3}, {1, 2, 7.1}, s), "4.1 cm: not yet");
}
}  // namespace

int main() {
    SmartTable();
    NineFromEmpty();
    SmartOff();
    EmptyGun();
    Holds();
    Capture();
    TwoPerPush();
    SingleAlone();
    StopHalfway();
    LeaveTheLine();
    Blocked();
    Port();
    std::printf("shell_load: %d checks passed\n", checks);
    return 0;
}
