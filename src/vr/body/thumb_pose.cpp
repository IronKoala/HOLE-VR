#include "vr/body/thumb_pose.h"

#include <algorithm>
#include <cmath>

namespace holevr::vr::thumb_pose {
namespace {
using namespace ik;
namespace jl = joint_limits;
double Unit(double x) { return std::isfinite(x) ? std::clamp(x, 0.0, 1.0) : 0.0; }
double Smooth(double x) { x = Unit(x); return x * x * (3.0 - 2.0 * x); }
jl::Turn Limit(const jl::Limits& l, jl::Turn t) {
    t.flex = std::clamp(t.flex, l.flex_min, l.flex_max);
    t.side = std::clamp(t.side, -l.SideAt(t.flex), l.SideAt(t.flex));
    t.twist = std::clamp(t.twist, -l.twist, l.twist);
    return t;
}
fingers::Angles Pose(const jl::Turn t[3], const finger_contact::Bounds& b) {
    fingers::Angles a;
    for (int j = 0; j < 3; ++j) a.extra[j] = jl::Compose(b.axes[j], Limit(b.limits[j], t[j]));
    return a;
}
} // namespace

FVector PadDirection(const fingers::Chain& c, const fingers::Angles& pose, const FVector& palm) {
    skel::Xf reference[3], posed[3];
    fingers::Fk(c, fingers::Angles{}, reference, nullptr);
    fingers::Fk(c, pose, posed, nullptr);
    // The reference palm normal is transported by the distal bone. This
    // avoids assuming a mesh-specific local X/Y/Z is the pad direction.
    const FVector local_pad = reference[2].rotation.Inverse().Rotate(Normalize(palm));
    return Normalize(posed[2].rotation.Rotate(local_pad));
}

fingers::Angles Target(const fingers::Chain& c, const FVector& palm, const FVector& index_base,
                       const Input& input, const finger_contact::Bounds& bounds) {
    const double grip = Smooth(input.grip), trigger = Smooth(input.trigger);
    // Trigger without a power grip is a pinch-like intention estimate. It
    // affects only generated appearance, never object ownership or input.
    const double pinch = trigger * (1.0 - grip);
    const double opposition = Smooth(std::max({grip, pinch, input.thumb_touch ? 0.45 : 0.0}));
    const FVector local_aim = c.ref[0].rotation.Inverse().Rotate(index_base - c.ref[0].translation);
    const jl::Turn direction = jl::Decompose(bounds.axes[0], Arc(bounds.axes[0].bone, local_aim));
    jl::Turn turn[3];
    turn[0].flex = (8.0 + 27.0 * grip + 17.0 * pinch) * kDegToRad;
    turn[0].side = direction.side * opposition;
    // CMC is measured from its saddle reference; MCP/IP curves describe
    // anatomical flex and must account for an already-curled reference rig.
    turn[1].flex = (5.0 + 45.0 * grip + 24.0 * pinch) * kDegToRad - bounds.limits[1].ref_bend;
    turn[2].flex = (3.0 + 42.0 * grip + 22.0 * pinch) * kDegToRad - bounds.limits[2].ref_bend;
    auto alignment = [&](double twist) {
        jl::Turn posed[3] = {turn[0], turn[1], turn[2]};
        posed[0].twist = twist;
        const auto candidate = Pose(posed, bounds);
        skel::Xf joints[3]; FVector tip;
        fingers::Fk(c, candidate, joints, &tip);
        return Dot(PadDirection(c, candidate, palm), Normalize(index_base - tip));
    };
    // Rotating the base also moves the already-bent tip, so align against
    // the resulting tip-to-index direction rather than a frozen normal.
    // A finite derivative and smooth saturation choose handedness without
    // a discontinuous +/- twist switch or mesh-specific axis convention.
    constexpr double h = 0.02;
    const double slope = (alignment(h) - alignment(-h)) / (2 * h);
    double twist = bounds.limits[0].twist * opposition * slope / std::sqrt(slope*slope + 0.01);
    const double initial = alignment(0);
    for (int trial = 0; trial < 5 && alignment(twist) < initial; ++trial) twist *= 0.5;
    if (alignment(twist) >= initial) turn[0].twist = twist;
    return Pose(turn, bounds);
}

} // namespace holevr::vr::thumb_pose
