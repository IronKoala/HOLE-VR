#include "vr/body/joint_limits.h"

#include <algorithm>
#include <cmath>

namespace holevr::vr::joint_limits {

using namespace ik;

Range Human(int finger, int joint) {
    Range r;
    if (finger == 0) {
        switch (joint) {
            case 0:  // CMC, a saddle: curls across the palm, swings, rolls toward the fingers
                r.flex_min = -30, r.flex_max = 50, r.side = 35, r.twist = 20;
                r.from_reference = true;
                break;
            case 1:  // MCP: mostly a hinge, a little side play that goes when curled
                r.flex_min = -15, r.flex_max = 65, r.side = 10, r.side_curled = 0.5, r.twist = 8;
                break;
            default:  // IP: a hinge (many people bend it well back)
                r.flex_min = -25, r.flex_max = 80;
                break;
        }
        return r;
    }
    switch (joint) {
        case 0: {  // MCP: curls (the outer fingers further), spreads (less when curled), barely rolls
            static const double spread[5] = {0, 22, 18, 18, 28};
            static const double curl[5] = {0, 90, 95, 95, 100};
            const int i = std::clamp(finger, 1, 4);
            r.flex_min = -30;
            r.flex_max = curl[i];
            r.side = spread[i];
            r.side_curled = 0.35;
            r.twist = 10;
            break;
        }
        case 1:  // PIP: a hinge
            r.flex_min = -5, r.flex_max = 105;
            break;
        default:  // DIP: a hinge
            r.flex_min = -15, r.flex_max = 90;
            break;
    }
    return r;
}

namespace {
constexpr double kD = kDegToRad;

double Wrap(double a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}

// Signed angle from `from` to `to` about `axis` (both taken perpendicular to it).
double SignedAngle(const FVector& from, const FVector& to, const FVector& axis) {
    FVector n = Normalize(axis);
    FVector f = Normalize(Reject(from, n)), t = Normalize(Reject(to, n));
    return std::atan2(Dot(Cross(f, t), n), Dot(f, t));
}

// The nearer end of [lo, hi] for an angle outside it (angles wrap).
double ClampWrapped(double x, double lo, double hi) {
    if (x >= lo && x <= hi) return x;
    return std::fabs(Wrap(x - lo)) < std::fabs(Wrap(x - hi)) ? lo : hi;
}

FVector BoneOf(const fingers::Chain& c, int j) {
    return Normalize(j < 2 ? c.ref[j + 1].translation : c.tip, FVector{1, 0, 0});
}
}  // namespace

Axes JointAxes(const fingers::Chain& c, int j) {
    Axes a;
    a.bone = BoneOf(c, j);
    FVector perp = Reject(c.flex[j], a.bone);
    if (Length(perp) < 1e-6) perp = Reject(FVector{0, 0, 1}, a.bone);
    if (Length(perp) < 1e-6) perp = Reject(FVector{0, 1, 0}, a.bone);
    a.flex = Normalize(perp);
    a.toward = Normalize(Cross(a.flex, a.bone));
    return a;
}

Turn Decompose(const Axes& a, const FQuat& q_in) {
    FQuat q = Normalized(q_in);
    FVector d = Normalize(q.Rotate(a.bone), a.bone);
    Turn t;
    t.flex = std::atan2(Dot(d, a.toward), Dot(d, a.bone));
    t.side = std::asin(std::clamp(Dot(d, a.flex), -1.0, 1.0));
    FQuat nat = Normalized(AxisAngle(a.flex, t.flex) * AxisAngle(a.toward, -t.side));
    t.twist = TwistAngle(Normalized(nat.Inverse() * q), a.bone);
    return t;
}

FQuat Compose(const Axes& a, const Turn& t) {
    return Normalized(AxisAngle(a.flex, t.flex) * AxisAngle(a.toward, -t.side) * AxisAngle(a.bone, t.twist));
}

void ReferenceBends(const fingers::Chain& c, bool thumb, double out[3]) {
    for (int j = 0; j < 3; ++j) {
        double bend = 0;
        if (j == 0) {
            // Knuckle: from the wrist->knuckle line (the metacarpal) to the
            // first bone, in hand space. The thumb's base has no such line.
            if (!thumb) {
                FVector meta = c.ref[0].translation;
                FVector seg = c.ref[0].rotation.Rotate(BoneOf(c, 0));
                FVector axis = c.ref[0].rotation.Rotate(c.flex[0]);
                if (Length(meta) > 1e-3) bend = SignedAngle(meta, seg, axis);
            }
        } else {
            // From the previous bone to this one, in the previous joint's frame.
            FVector prev = BoneOf(c, j - 1);
            FVector seg = c.ref[j].rotation.Rotate(BoneOf(c, j));
            FVector axis = c.ref[j].rotation.Rotate(c.flex[j]);
            bend = SignedAngle(prev, seg, axis);
        }
        // A relaxed reference hand is curled a little; a reading far outside
        // that is a measuring problem, not a pose.
        out[j] = std::clamp(bend, -10.0 * kD, 30.0 * kD);
    }
}

double Limits::SideAt(double flex) const {
    if (side <= 0) return 0;
    double frac = anat_max > 1e-6 ? std::clamp((flex + ref_bend) / anat_max, 0.0, 1.0) : 0.0;
    return side * (1.0 - (1.0 - side_curled) * frac);
}

Limits Relative(const Range& r, double ref_bend) {
    Limits l;
    l.ref_bend = r.from_reference ? 0.0 : ref_bend;
    l.flex_min = r.flex_min * kD - l.ref_bend;
    l.flex_max = r.flex_max * kD - l.ref_bend;
    l.side = r.side * kD;
    l.side_curled = std::clamp(r.side_curled, 0.0, 1.0);
    l.twist = r.twist * kD;
    l.anat_max = std::max(r.flex_max * kD, 1e-3);
    return l;
}

Turn ClampTurn(const Limits& l, const Turn& t, bool* at_limit) {
    const double slack = 0.3 * kD;
    Turn o;
    o.flex = ClampWrapped(t.flex, l.flex_min, l.flex_max);
    const double side = l.SideAt(o.flex);
    o.side = std::clamp(t.side, -side, side);
    o.twist = std::clamp(t.twist, -l.twist, l.twist);
    if (at_limit) {
        bool hit = std::fabs(Wrap(t.flex - o.flex)) > slack;
        if (l.side > 0 && std::fabs(t.side - o.side) > slack) hit = true;
        *at_limit = hit;
    }
    return o;
}

bool Within(const Axes& a, const Limits& l, const FQuat& q, double tol) {
    Turn t = Decompose(a, q);
    if (t.flex < l.flex_min - tol || t.flex > l.flex_max + tol) return false;
    if (std::fabs(t.side) > l.SideAt(t.flex) + tol) return false;
    return std::fabs(t.twist) <= l.twist + tol;
}

FQuat Clamp(const Axes& a, const Limits& l, const FQuat& q, bool* at_limit) {
    Turn t = Decompose(a, q);
    Turn c = ClampTurn(l, t, at_limit);
    if (std::fabs(Wrap(c.flex - t.flex)) < 1e-9 && std::fabs(c.side - t.side) < 1e-9 &&
        std::fabs(c.twist - t.twist) < 1e-9)
        return Normalized(q);
    return Compose(a, c);
}

namespace {
// What the fit keeps: the middle and tip joints, the fingertip, and a point
// on the fingertip's pad (its roll).
struct Shape {
    FVector p[4];
};
constexpr double kWeight[4] = {1.0, 1.0, 2.0, 0.5};

Shape ShapeOf(const fingers::Chain& c, const Axes ax[3], const FQuat q[3]) {
    fingers::Angles a;
    for (int j = 0; j < 3; ++j) a.extra[j] = q[j];
    skel::Xf js[3];
    FVector tip;
    fingers::Fk(c, a, js, &tip);
    Shape s;
    s.p[0] = js[1].translation;
    s.p[1] = js[2].translation;
    s.p[2] = tip;
    s.p[3] = tip + js[2].rotation.Rotate(ax[2].toward) * 1.0;
    return s;
}

double ShapeCost(const Shape& a, const Shape& b) {
    double cost = 0;
    for (int k = 0; k < 4; ++k) {
        FVector d = a.p[k] - b.p[k];
        cost += kWeight[k] * Dot(d, d);
    }
    return cost;
}
}  // namespace

FitReport Fit(const fingers::Chain& c, const Axes ax[3], const Limits lim[3], FQuat q[3]) {
    FitReport rep;
    if (!c.valid) return rep;
    bool inside = true;
    for (int j = 0; j < 3; ++j) inside = inside && Within(ax[j], lim[j], q[j]);
    if (inside) return rep;

    const Shape want = ShapeOf(c, ax, q);
    Turn orig[3], cur[3];
    for (int j = 0; j < 3; ++j) {
        orig[j] = Decompose(ax[j], q[j]);
        cur[j] = ClampTurn(lim[j], orig[j]);
        // What had to go: the part of each turn past its range.
        rep.flex_deg[j] = std::fabs(Wrap(orig[j].flex - cur[j].flex)) * kRadToDeg;
        rep.side_deg[j] = std::fabs(orig[j].side - cur[j].side) * kRadToDeg;
        rep.twist_deg[j] = std::fabs(orig[j].twist - cur[j].twist) * kRadToDeg;
    }
    auto build = [&](const Turn t[3], FQuat out[3]) {
        for (int j = 0; j < 3; ++j) out[j] = Compose(ax[j], t[j]);
    };
    auto cost_of = [&](const Turn t[3]) {
        FQuat qq[3];
        build(t, qq);
        return ShapeCost(ShapeOf(c, ax, qq), want);
    };
    // Coordinate descent over every free part of every joint, kept in range.
    double best = cost_of(cur);
    double step = 4.0 * kD;
    for (int it = 0; it < 200 && step > 0.05 * kD; ++it) {
        bool moved = false;
        for (int j = 0; j < 3; ++j)
            for (int part = 0; part < 3; ++part) {
                if (part == 1 && lim[j].side <= 0) continue;
                if (part == 2 && lim[j].twist <= 0) continue;
                for (double dir : {1.0, -1.0}) {
                    Turn trial[3] = {cur[0], cur[1], cur[2]};
                    double* v = part == 0 ? &trial[j].flex : part == 1 ? &trial[j].side : &trial[j].twist;
                    *v += dir * step;
                    trial[j] = ClampTurn(lim[j], trial[j]);
                    double cost = cost_of(trial);
                    if (cost < best - 1e-12) {
                        best = cost;
                        for (int k = 0; k < 3; ++k) cur[k] = trial[k];
                        moved = true;
                    }
                }
            }
        if (!moved) step *= 0.5;
    }
    FQuat out[3];
    build(cur, out);
    const Shape got = ShapeOf(c, ax, out);
    for (int k = 0; k < 3; ++k) rep.moved_cm = std::max(rep.moved_cm, Length(got.p[k] - want.p[k]));
    for (int j = 0; j < 3; ++j) q[j] = out[j];
    rep.changed = true;
    return rep;
}

}  // namespace holevr::vr::joint_limits
