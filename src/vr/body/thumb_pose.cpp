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
fingers::Angles Pose(const jl::Turn t[3], const controller_pose::Bounds& b) {
    fingers::Angles a;
    for (int j = 0; j < 3; ++j) a.extra[j] = jl::Compose(b.axes[j], Limit(b.limits[j], t[j]));
    return a;
}
jl::Turn OpenBase(const fingers::Chain& c,const FVector& palm,const controller_pose::Bounds& bounds) {
    jl::Turn open[3];
    open[1].flex=5.0*kDegToRad-bounds.limits[1].ref_bend;
    open[2].flex=3.0*kDegToRad-bounds.limits[2].ref_bend;
    skel::Xf joints[3]; FVector tip;
    fingers::Fk(c,Pose(open,bounds),joints,&tip);
    const FVector ray=tip-joints[0].translation;
    const FVector tangent=Reject(ray,Normalize(palm));
    if(Length(ray)<1e-6 || Length(tangent)<1e-6) return {};
    // The free open thumb belongs in its anatomical palm plane. A baked
    // saddle reference can point the entire chain several centimeters out
    // of that plane even at zero input. Rotate the generated open ray about
    // its fixed CMC origin; do not move the origin or use a world contact.
    const auto q=c.ref[0].rotation.Inverse()*Arc(ray,tangent)*c.ref[0].rotation;
    return Limit(bounds.limits[0],jl::Decompose(bounds.axes[0],q));
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
                       const Input& input, const controller_pose::Bounds& bounds) {
    const double grip = Smooth(input.grip), trigger = Smooth(input.trigger);
    // Trigger without a power grip is a pinch-like intention estimate. It
    // affects only generated appearance, never object ownership or input.
    const double pinch = trigger * (1.0 - grip);
    // Face/stick buttons are only a coarse thumb-pad cue, not a measured
    // saddle rotation. They must not abruptly impose CMC opposition.
    const double opposition = Smooth(std::max(grip, pinch));
    const jl::Turn open_base=OpenBase(c,palm,bounds);
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
    const double closing=std::max(grip,pinch);
    if(closing>0) {
        skel::Xf joints[3]; FVector tip;
        fingers::Fk(c,Pose(turn,bounds),joints,&tip);
        const FVector root=joints[0].translation;
        // A closing free hand rests the thumb outside the curled digits. Aiming at
        // the bare index knuckle puts the thumb across the palm beneath
        // those digits and makes the intended closing motion intersect.
        const double length=Length(c.ref[1].translation)+Length(c.ref[2].translation)+Length(c.tip);
        const FVector normal=Normalize(palm);
        const FVector forward=Normalize(Reject(index_base,normal));
        const FVector radial=Normalize(Reject(Reject(root,normal),forward));
        const FVector outside=index_base+normal*(length*.60)+radial*(length*.25);
        const auto correction=Arc(tip-root,outside-root);
        const auto base=c.ref[0].rotation.Inverse()*correction*c.ref[0].rotation*
            jl::Compose(bounds.axes[0],turn[0]);
        const auto wrapped=Limit(bounds.limits[0],jl::Decompose(bounds.axes[0],base));
        turn[0].flex+=(wrapped.flex-turn[0].flex)*closing;
        turn[0].side+=(wrapped.side-turn[0].side)*closing;
        turn[0].twist+=(wrapped.twist-turn[0].twist)*closing;
        // The index closing into a power grip needs room beside the thenar
        // pad as well as the thumb tip. Reserve two thirds of the available
        // saddle abduction during that combined intention. The joint axis
        // and metacarpal direction choose the sign on either mirrored rig.
        const double outward=Dot(c.ref[0].rotation.Rotate(bounds.axes[0].flex),forward);
        if(std::fabs(outward)>1e-4) {
            const double side=std::copysign(bounds.limits[0].SideAt(turn[0].flex)*(2.0/3.0),outward);
            if(turn[0].side*side<side*side)
                turn[0].side+=(side-turn[0].side)*(grip*trigger);
        }
    }
    // The neutral saddle orientation fades out as opposition takes over.
    if(opposition<1.0) {
        turn[0].flex=open_base.flex*(1.0-opposition)+turn[0].flex*opposition;
        turn[0].side=open_base.side*(1.0-opposition)+turn[0].side;
        turn[0].twist=open_base.twist*(1.0-opposition)+turn[0].twist;
    }
    // The outside guide may swing the metacarpal, but a bent-tip alignment
    // must not roll its skin attachment to the anatomical limit. Keep its
    // side/axial orientation close to the same rig-derived neutral saddle.
    // These are generated-pose guide bounds, not new anatomical limits or
    // restrictions on authored/calibrated object grips.
    constexpr double roll=6*kDegToRad;
    // A pinch may adduct back toward the mesh's saddle reference. Holding
    // that pose ten degrees from the open (abducted) saddle stretches the
    // thumb/index web. Admit that reference-directed travel as pinch grows,
    // without restoring the old sweep past it or the full-grip axial roll.
    const double pinch_adduction=pinch*std::max(0.0,std::fabs(open_base.side)-10*kDegToRad);
    const double side_min=open_base.side-10*kDegToRad-(open_base.side>0?pinch_adduction:0);
    const double side_max=open_base.side+10*kDegToRad+(open_base.side<0?pinch_adduction:0);
    turn[0].side=std::clamp(turn[0].side,side_min,side_max);
    turn[0].twist=std::clamp(turn[0].twist,open_base.twist-roll,open_base.twist+roll);
    if(input.thumb_touch) {
        const double released=1-std::max(grip,trigger);
        // Apply the button fold after the base guide: changing these hinges
        // must not feed back into another base rotation or move its origin.
        turn[1].flex+=6*kDegToRad*released;
        turn[2].flex+=8*kDegToRad*released;
    }
    return Pose(turn, bounds);
}

} // namespace holevr::vr::thumb_pose
