#include "vr/interact/shell_load.h"

#include <algorithm>
#include <cmath>

namespace holevr::vr::interact::shells {
namespace {
double Dot(const FVector& a, const FVector& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
double Len(const FVector& v) { return std::sqrt(Dot(v, v)); }
constexpr double kPi = 3.14159265358979;
}  // namespace

bool TubeLocked(const Settings& s, const Tube& t) { return s.tube_needs_closed_bolt && t.bolt_open; }

bool TubeRoom(const Tube& t) { return t.capacity > 0 && t.in_tube < t.capacity; }

bool PortOpen(const Tube& t) { return t.bolt_open && t.chamber == 0 && t.in_tube == 0 && t.capacity > 0; }

int SmartCount(const Settings& s, const Tube& t) {
    if (t.capacity <= 0) return 0;
    // Bolt locked open: only the ejection port takes a shell, and only one.
    // shotgun_empty_gun_single=0 gives the usual count anyway (held while B
    // closes the bolt, then into the tube).
    if (TubeLocked(s, t) && s.empty_gun_single) return PortOpen(t) ? 1 : 0;
    if (!s.smart) return std::clamp(s.fixed, 1, kMaxHeld);
    const int missing = std::max(0, t.capacity - t.in_tube);
    if (missing >= 4) return 4;
    if (missing >= 2) return 2;
    return missing;  // 1 or 0
}

const char* HoldName(int n) {
    switch (n) {
        case 4: return "quad";
        case 3: return "three";
        case 2: return "twin";
        case 1: return "single";
        default: return "none";
    }
}

FVector HoldBase(int n, int slot, const FVector& nudge) {
    n = std::clamp(n, 1, kMaxHeld);
    slot = std::clamp(slot, 0, n - 1);
    // The rims sit just past the thumb and index ring (X +2.5), so the
    // shells run back through the fist and out past the little finger.
    const double h = kSpacing * 0.5;
    FVector at{2.5, 0, 0};
    if (n == 1) {
        // single: in the middle of the fist
    } else if (n == 2) {
        // twin (owner's photo 2): one above the other
        at.Z = slot == 0 ? h : -h;
    } else {
        // quad (owner's photo 1): two pairs, side by side; the first pair on top
        at.Z = slot < 2 ? h : -h;
        at.Y = (slot % 2 == 0) ? -h : h;
    }
    return at + nudge;
}

FVector HoldNoseDir() { return FVector{-1, 0, 0}; }

Step Advance(Sweep& w, const Settings& s, const StepIn& in) {
    const double along = Dot(in.nose, in.axis);
    const double side = Len(in.nose - in.axis * along);
    if (!w.captured) {
        if (in.held <= 0) return Step::kNone;
        const double c = std::clamp(Dot(in.nose_dir, in.axis), -1.0, 1.0);
        const double angle = std::acos(c) * 180.0 / kPi;
        if (std::fabs(along) <= s.capture_cm && side <= s.capture_cm && angle <= s.capture_deg) {
            w = Sweep{};
            w.captured = true;
            w.offset = along;
            w.pair = std::min(2, in.held);
            return Step::kCaptured;
        }
        return Step::kNone;
    }
    const double d = along - w.offset;
    if (side > s.let_go_side_cm || d < -s.pull_out_cm || in.held <= 0) {
        w.captured = false;
        return Step::kLetGo;
    }
    const double start = w.loaded == 0 ? 0.0 : s.first_cm;
    const double stop = w.loaded == 0 ? s.first_cm : s.second_cm;
    if (!in.room || in.locked) {
        const double limit = start + s.block_cm;
        w.depth = std::min(d, limit);
        if (d >= limit && !w.block_told) {
            w.block_told = true;
            return Step::kBlocked;
        }
        return Step::kNone;
    }
    w.depth = d;
    if (d >= stop) {
        ++w.loaded;
        if (w.loaded >= w.pair) w.captured = false;
        return Step::kCommit;
    }
    return Step::kNone;
}

double ShownDepth(const Sweep& w, const Settings& s) {
    const double start = w.loaded == 0 ? 0.0 : s.first_cm;
    return std::max(0.0, w.depth - start);
}

bool AtEjectionPort(const FVector& shell_middle, const FVector& port, const Settings& s) {
    return Len(shell_middle - port) <= s.port_cm;
}

}  // namespace holevr::vr::interact::shells
