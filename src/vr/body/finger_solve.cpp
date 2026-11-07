#include "vr/body/finger_solve.h"

#include <algorithm>
#include <cmath>

namespace holevr::vr::fingers {

using namespace ik;

Chain Build(const skel::Xf j[3], const FVector& palm, const FVector& pinky_base, bool thumb) {
    Chain c;
    c.thumb = thumb;
    c.ref[0] = j[0];
    c.ref[1] = skel::Relative(j[1], j[0]);
    c.ref[2] = skel::Relative(j[2], j[1]);
    FVector last = j[2].translation - j[1].translation;
    FVector tip_hand = j[2].translation + last * 0.8;
    c.tip = skel::Relative(skel::Xf{FQuat{}, tip_hand}, j[2]).translation;
    FVector toward = Normalize(palm);
    if (thumb) toward = Normalize(Normalize(pinky_base - j[0].translation) + toward * 0.5);
    FVector seg[3] = {j[1].translation - j[0].translation, j[2].translation - j[1].translation,
                      tip_hand - j[2].translation};
    FVector base_axis{0, 0, 0};
    for (int n = 0; n < 3; ++n) {
        FVector axis = Cross(seg[n], toward);
        if (Length(axis) < 1e-6) axis = base_axis;
        axis = Normalize(axis, FVector{0, 0, 1});
        if (n == 0) base_axis = axis;
        c.flex[n] = j[n].rotation.Inverse().Rotate(axis);
    }
    FVector spread = Normalize(Cross(Normalize(seg[0]), base_axis), toward);
    c.spread = j[0].rotation.Inverse().Rotate(spread);
    c.length = Length(seg[0]) + Length(seg[1]) + Length(seg[2]);
    c.valid = c.length > 1.0;
    return c;
}

Limits DefaultLimits(bool thumb) {
    if (thumb) return Limits{{-40, -15, -15}, {55, 65, 75}, -45, 45, 0.8};
    return Limits{{-25, -5, -5}, {90, 105, 80}, -20, 20, 0.7};
}

FQuat LocalRotation(const Chain& c, int j, const Angles& a) {
    FQuat q = AxisAngle(c.flex[j], a.flex[j]);
    if (j == 0) q = AxisAngle(c.spread, a.spread) * q;
    return Normalized(c.ref[j].rotation * q * a.extra[j]);
}

void Fk(const Chain& c, const Angles& a, skel::Xf out[3], FVector* tip) {
    for (int j = 0; j < 3; ++j) {
        skel::Xf l = c.ref[j];
        l.rotation = LocalRotation(c, j, a);
        out[j] = j == 0 ? l : skel::Compose(l, out[j - 1]);
    }
    if (tip) *tip = skel::Apply(out[2], c.tip);
}

namespace {
constexpr double kD = kDegToRad;

struct Params {
    double p[4];  // flex0, flex1, spread, (unused)
};

Angles ToAngles(const Params& p, const Limits& lim) {
    Angles a;
    a.flex[0] = p.p[0];
    a.flex[1] = p.p[1];
    a.flex[2] = std::clamp(p.p[1] * lim.couple, lim.flex_min[2] * kD, lim.flex_max[2] * kD);
    a.spread = p.p[2];
    return a;
}

// Deepest sample of the posed finger inside the object: points along the
// bone line, plus six points `radius` out from each (the finger's skin).
double Depth(const skel::Xf j[3], const FVector& tip, const DepthFn& depth, double radius) {
    FVector pts[4] = {j[0].translation, j[1].translation, j[2].translation, tip};
    double worst = 0;
    for (int s = 0; s < 3; ++s)
        for (int k = 0; k <= 4; ++k) {
            // The first half of the base segment is in the palm / web of the
            // hand, which rests on the object by design.
            if (s == 0 && k < 2) continue;
            FVector p = pts[s] + (pts[s + 1] - pts[s]) * (k / 4.0);
            worst = std::max(worst, depth(p));
            if (radius > 0) {
                static const FVector dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
                for (const FVector& d : dirs) worst = std::max(worst, depth(p + d * radius));
            }
        }
    return worst;
}

double Cost(const Chain& c, const Params& p, const Limits& lim, const FVector& target, const DepthFn* depth,
            double radius) {
    skel::Xf j[3];
    FVector tip;
    Fk(c, ToAngles(p, lim), j, &tip);
    FVector d = tip - target;
    // A little pull toward a relaxed pose so ties resolve naturally.
    double reg = 0.02 * (p.p[0] * p.p[0] + p.p[1] * p.p[1] + 2.0 * p.p[2] * p.p[2]);
    double cost = Dot(d, d) + reg;
    if (depth) {
        double in = Depth(j, tip, *depth, radius);
        cost += 20.0 * in * in + 2.0 * in;
    }
    return cost;
}
}  // namespace

double FingerDepth(const Chain& c, const Angles& a, const DepthFn& depth, double radius) {
    skel::Xf j[3];
    FVector tip;
    Fk(c, a, j, &tip);
    return Depth(j, tip, depth, radius);
}

Angles Solve(const Chain& c, const FVector& target, const Limits& lim, double* error, const DepthFn* depth,
             double radius, double* clip, const Angles* start) {
    const double lo[3] = {lim.flex_min[0] * kD, lim.flex_min[1] * kD, lim.spread_min * kD};
    const double hi[3] = {lim.flex_max[0] * kD, lim.flex_max[1] * kD, lim.spread_max * kD};
    // Coarse grid over the three free angles, then refine around the best.
    Params best{{0, 0, 0, 0}};
    double best_cost = Cost(c, best, lim, target, depth, radius);
    const int n = 9;
    if (start) {
        best = Params{{start->flex[0], start->flex[1], start->spread, 0}};
        best_cost = Cost(c, best, lim, target, depth, radius);
    }
    for (int a = 0; a < (start ? 0 : n); ++a)
        for (int b = 0; b < n; ++b)
            for (int s = 0; s < n; ++s) {
                Params p{{lo[0] + (hi[0] - lo[0]) * a / (n - 1), lo[1] + (hi[1] - lo[1]) * b / (n - 1),
                          lo[2] + (hi[2] - lo[2]) * s / (n - 1), 0}};
                double cost = Cost(c, p, lim, target, depth, radius);
                if (cost < best_cost) {
                    best_cost = cost;
                    best = p;
                }
            }
    const double coarse = start ? 0.25 : 1.0;
    double step[3] = {coarse * (hi[0] - lo[0]) / (n - 1), coarse * (hi[1] - lo[1]) / (n - 1),
                      coarse * (hi[2] - lo[2]) / (n - 1)};
    for (int it = 0; it < 60; ++it) {
        bool moved = false;
        for (int k = 0; k < 3; ++k)
            for (double dir : {1.0, -1.0}) {
                Params p = best;
                p.p[k] = std::clamp(p.p[k] + dir * step[k], lo[k], hi[k]);
                double cost = Cost(c, p, lim, target, depth, radius);
                if (cost < best_cost) {
                    best_cost = cost;
                    best = p;
                    moved = true;
                }
            }
        if (!moved)
            for (double& s : step) s *= 0.5;
        if (step[0] < 1e-4 && step[1] < 1e-4 && step[2] < 1e-4) break;
    }
    Angles out = ToAngles(best, lim);
    if (error) {
        skel::Xf j[3];
        FVector tip;
        Fk(c, out, j, &tip);
        *error = Length(tip - target);
    }
    if (clip) *clip = depth ? FingerDepth(c, out, *depth, radius) : 0.0;
    return out;
}

Angles Curl(const Chain& c, double amount, const Limits& lim) {
    Angles a;
    // Controller scaling and contact drag can request more than a full curl.
    // Generated poses must still respect the solver's configured joint stops.
    if (!std::isfinite(amount)) amount = 0.0;
    amount = std::max(0.0, amount);
    for (int j = 0; j < 3; ++j)
        a.flex[j] = std::clamp(amount * lim.flex_max[j] * kD * (c.thumb ? 0.6 : 0.85),
                               lim.flex_min[j] * kD, lim.flex_max[j] * kD);
    return a;
}

Angles Blend(const Angles& a, const Angles& b, double t) {
    Angles r;
    for (int j = 0; j < 3; ++j) r.flex[j] = a.flex[j] + (b.flex[j] - a.flex[j]) * t;
    r.spread = a.spread + (b.spread - a.spread) * t;
    for (int j = 0; j < 3; ++j) r.extra[j] = Normalized(a.extra[j] * Scale(a.extra[j].Inverse() * b.extra[j], t));
    return r;
}

}  // namespace holevr::vr::fingers
