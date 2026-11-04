#include "vr/interact/touch_math.h"

#include <algorithm>
#include <cmath>

namespace holevr::vr::touch {

const char* KindName(Kind k) {
    switch (k) {
        case Kind::kSkip: return "left to the drinks code";
        case Kind::kOther: return "trigger only";
        case Kind::kLoot: return "loot";
        case Kind::kContainer: return "container";
        case Kind::kDoor: return "door";
    }
    return "?";
}

Kind Classify(const std::vector<std::string>& chain, const std::vector<std::string>& extra_containers,
              const std::vector<std::string>& skip) {
    auto has = [&](const char* name) { return std::find(chain.begin(), chain.end(), name) != chain.end(); };
    for (const std::string& c : chain) {
        // The hub drink machine, its buttons and can slot (drinks thread,
        // 2026-11-03): pressed and drunk by hand there, never by touch here.
        if (c.rfind("LCBP_SkillUpgrade", 0) == 0) return Kind::kSkip;
        for (const std::string& s : skip)
            if (!s.empty() && c.find(s) != std::string::npos) return Kind::kSkip;
    }
    if (has("BP_DoorBase_C")) return Kind::kDoor;
    if (has("BP_ItemBoxBase_C") || has("LCBP_Fridge_C")) return Kind::kContainer;
    for (const std::string& c : chain)
        for (const std::string& e : extra_containers)
            if (!e.empty() && c.find(e) != std::string::npos) return Kind::kContainer;
    if (has("BP_ItemBase_C")) return Kind::kLoot;
    return Kind::kOther;
}

std::vector<std::string> SplitList(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t comma = s.find(',', start);
        if (comma == std::string::npos) comma = s.size();
        std::string w = s.substr(start, comma - start);
        while (!w.empty() && (w.front() == ' ' || w.front() == '\t')) w.erase(w.begin());
        while (!w.empty() && (w.back() == ' ' || w.back() == '\t')) w.pop_back();
        if (!w.empty()) out.push_back(w);
        start = comma + 1;
    }
    return out;
}

double DistToBox(const FVector& l, const Box& b) {
    const double dx = l.X - std::clamp(l.X, b.lo.X, b.hi.X);
    const double dy = l.Y - std::clamp(l.Y, b.lo.Y, b.hi.Y);
    const double dz = l.Z - std::clamp(l.Z, b.lo.Z, b.hi.Z);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

double BoxVolume(const Box& b) {
    return std::fmax(0.0, b.hi.X - b.lo.X) * std::fmax(0.0, b.hi.Y - b.lo.Y) * std::fmax(0.0, b.hi.Z - b.lo.Z);
}

Box Grow(const Box& b, double cm) {
    return {FVector{b.lo.X - cm, b.lo.Y - cm, b.lo.Z - cm}, FVector{b.hi.X + cm, b.hi.Y + cm, b.hi.Z + cm}};
}

double SegmentBoxGap(const FVector& o, const FVector& d, double len, const Box& b, double* t_at) {
    // Through the box: the slab test gives where the segment enters.
    double t0 = 0.0, t1 = len;
    const double oo[3] = {o.X, o.Y, o.Z}, dd[3] = {d.X, d.Y, d.Z};
    const double lo[3] = {b.lo.X, b.lo.Y, b.lo.Z}, hi[3] = {b.hi.X, b.hi.Y, b.hi.Z};
    bool through = true;
    for (int k = 0; k < 3 && through; ++k) {
        if (std::fabs(dd[k]) < 1e-12) {
            if (oo[k] < lo[k] || oo[k] > hi[k]) through = false;
            continue;
        }
        double a = (lo[k] - oo[k]) / dd[k], c = (hi[k] - oo[k]) / dd[k];
        if (a > c) std::swap(a, c);
        t0 = std::fmax(t0, a);
        t1 = std::fmin(t1, c);
        if (t0 > t1) through = false;
    }
    if (through) {
        if (t_at) *t_at = t0;
        return 0.0;
    }
    // Beside it: the distance to a box along a line is convex, so a golden
    // section search finds the closest point.
    auto at = [&](double t) { return DistToBox(FVector{o.X + d.X * t, o.Y + d.Y * t, o.Z + d.Z * t}, b); };
    const double g = 0.6180339887498949;
    double a = 0.0, c = len;
    double x1 = c - g * (c - a), x2 = a + g * (c - a);
    double f1 = at(x1), f2 = at(x2);
    for (int i = 0; i < 60 && c - a > 0.01; ++i) {
        if (f1 <= f2) {
            c = x2, x2 = x1, f2 = f1;
            x1 = c - g * (c - a), f1 = at(x1);
        } else {
            a = x1, x1 = x2, f1 = f2;
            x2 = a + g * (c - a), f2 = at(x2);
        }
    }
    double t = 0.5 * (a + c), best = at(t);
    for (const double e : {0.0, len}) {
        const double f = at(e);
        if (f < best) best = f, t = e;
    }
    if (t_at) *t_at = t;
    return best;
}

Box ScaledBox(const FVector& mn, const FVector& mx, const FVector& s) {
    auto axis = [](double a, double b, double k, double* lo, double* hi) {
        *lo = std::fmin(a * k, b * k);
        *hi = std::fmax(a * k, b * k);
    };
    Box out;
    axis(mn.X, mx.X, s.X, &out.lo.X, &out.hi.X);
    axis(mn.Y, mx.Y, s.Y, &out.lo.Y, &out.hi.Y);
    axis(mn.Z, mx.Z, s.Z, &out.lo.Z, &out.hi.Z);
    return out;
}

namespace {
double& At(FVector& v, int i) { return i == 0 ? v.X : i == 1 ? v.Y : v.Z; }
double At(const FVector& v, int i) { return i == 0 ? v.X : i == 1 ? v.Y : v.Z; }
}  // namespace

int DoorHandleZones(const Box& leaf, const FVector& hinge, bool hinge_known, const FVector& up_in_leaf,
                    const HandleZoneSettings& s, Box zones[2]) {
    // Vertical: the leaf axis closest to the world's up. Thickness: the
    // thinnest of the other two. Width: the one left.
    int v = 0;
    for (int i = 1; i < 3; ++i)
        if (std::fabs(At(up_in_leaf, i)) > std::fabs(At(up_in_leaf, v))) v = i;
    const int a = (v + 1) % 3, b = (v + 2) % 3;
    const double ea = At(leaf.hi, a) - At(leaf.lo, a), eb = At(leaf.hi, b) - At(leaf.lo, b);
    const int t = ea < eb ? a : b, w = t == a ? b : a;
    const double height = At(leaf.hi, v) - At(leaf.lo, v), width = At(leaf.hi, w) - At(leaf.lo, w),
                 thick = At(leaf.hi, t) - At(leaf.lo, t);
    // A door leaf: taller than wide, wider than thick, door-like sizes.
    if (height < 120.0 || width < 40.0 || thick > 30.0 || width > height) return 0;
    const bool up_pos = At(up_in_leaf, v) > 0;
    const double bottom = up_pos ? At(leaf.lo, v) : At(leaf.hi, v);
    const double dir = up_pos ? 1.0 : -1.0;
    const double mid = bottom + dir * std::fmin(s.height, height - s.half_height);
    Box z;
    At(z.lo, v) = std::fmax(std::fmin(mid - s.half_height, mid + s.half_height), At(leaf.lo, v));
    At(z.hi, v) = std::fmin(std::fmax(mid - s.half_height, mid + s.half_height), At(leaf.hi, v));
    At(z.lo, t) = At(leaf.lo, t) - s.out;
    At(z.hi, t) = At(leaf.hi, t) + s.out;
    const double in = std::fmin(s.from_edge, width * 0.5);
    auto edge_zone = [&](bool high_edge) {
        Box e = z;
        if (high_edge) {
            At(e.lo, w) = At(leaf.hi, w) - in;
            At(e.hi, w) = At(leaf.hi, w) + s.past;
        } else {
            At(e.lo, w) = At(leaf.lo, w) - s.past;
            At(e.hi, w) = At(leaf.lo, w) + in;
        }
        return e;
    };
    if (!hinge_known) {
        zones[0] = edge_zone(false);
        zones[1] = edge_zone(true);
        return 2;
    }
    // The handle is on the edge away from the hinge.
    const double to_lo = std::fabs(At(hinge, w) - At(leaf.lo, w)), to_hi = std::fabs(At(hinge, w) - At(leaf.hi, w));
    zones[0] = edge_zone(to_hi > to_lo);
    return 1;
}

bool StepTouch(TouchState& s, double dist, double now, double touch_cm, double leave_cm, double hold_s) {
    if (!s.seen) {
        s.seen = true;
        s.armed = dist > touch_cm;
    }
    if (dist > leave_cm) {
        s.armed = true;
        s.inside = false;
        return false;
    }
    if (!s.inside) {
        if (dist > touch_cm) return false;
        s.inside = true;
        s.since = now;
    }
    if (s.armed && now - s.since >= hold_s) {
        s.armed = false;
        return true;
    }
    return false;
}

}  // namespace holevr::vr::touch
