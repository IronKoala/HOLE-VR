#include "vr/body/holster_pose.h"

namespace holevr::vr::holster_pose {
using namespace ik;

fingers::Angles Extended(const fingers::Chain& c, const FVector& direction) {
    fingers::Angles a;
    if (!c.valid || !std::isfinite(Length(direction)) || Length(direction) < 1e-6) return a;
    const FVector aim = Normalize(direction);
    for (int j = 0; j < 3; ++j) {
        skel::Xf joint[3];
        FVector tip;
        fingers::Fk(c, a, joint, &tip);
        const FVector end = j < 2 ? joint[j + 1].translation : tip;
        const FQuat world = Arc(end - joint[j].translation, aim) * joint[j].rotation;
        const FQuat parent = j ? joint[j - 1].rotation : FQuat{};
        a.extra[j] = Normalized(c.ref[j].rotation.Inverse() * parent.Inverse() * world);
    }
    return a;
}

Pose Approach(const fingers::Chain& thumb, const fingers::Chain& index, const FQuat& hand_in_gun) {
    const FQuat inv = hand_in_gun.Inverse();
    // A slight rearward lean lifts the thumb clear of the beavertail/slide.
    return {Extended(thumb, inv.Rotate({-.4, 0, 1})), Extended(index, inv.Rotate({1, -kIndexInward, kIndexRise}))};
}

Grip Fit(const grip_record::Record& rec, const fingers::Chain* chains,
         const FQuat& rot, const FVector& pos, double radius) {
    Grip out;
    out.hand_position = pos + FVector{0, kPalmOut, 0};
    const Pose extended = Approach(chains[0], chains[1], rot);
    out.finger[0] = extended.thumb;
    out.finger[1] = extended.index;
    fingers::DepthFn depth = [&](const FVector& p) {
        return grip_record::Depth(rec, out.hand_position + rot.Rotate(p));
    };
    for (int f = 2; f < 5; ++f) {
        const int t = grip_record::kMiddle + f - 2;
        FVector target = rec.tip[t];
        if (rec.posed[t]) {
            // A manually posed finger keeps its existing contact point.
            fingers::Angles original;
            for (int j = 0; j < 3; ++j) original.extra[j] = rec.joint[t][j];
            skel::Xf joints[3]; FVector tip;
            fingers::Fk(chains[f], original, joints, &tip);
            target = pos + rot.Rotate(tip);
        }
        out.finger[f] = fingers::Solve(chains[f], rot.Inverse().Rotate(target - out.hand_position),
                                      fingers::DefaultLimits(false), nullptr,
                                      rec.outline.empty() ? nullptr : &depth, radius);
    }
    return out;
}
}  // namespace holevr::vr::holster_pose
