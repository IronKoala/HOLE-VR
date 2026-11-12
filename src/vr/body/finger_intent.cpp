#include "vr/body/finger_intent.h"
#include "vr/body/thumb_pose.h"

#include <algorithm>
#include <cmath>

namespace holevr::vr::finger_intent {
namespace {
double Unit(double value) {
    return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0;
}
double Smooth(double value) { return value*value*(3.0-2.0*value); }
}

fingers::Angles FreeTarget(const fingers::Chain& chain, int finger,
                          const hands::FingerInput& input,
                          const controller_pose::Bounds& bounds,
                          double curl_scale, double relaxed_weight) {
    const double grip = Unit(input.grip), trigger = Unit(input.trigger);
    const double scale = std::isfinite(curl_scale) ? std::max(0.0, curl_scale) : 1.0;
    const double control = finger == 1 ? std::max(trigger, input.trigger_touch ? .22 : 0.0) : grip;
    const double amount = std::clamp(control*scale, 0.0, 1.2);
    const auto absolute = controller_pose::CurlTarget(chain, amount, bounds);
    if (chain.thumb || finger < 1 || finger > 4 || amount >= 1.0) return absolute;

    // Full controller input retains its configured endpoint even when the
    // curl gain is below one. Higher gains fade the rest term before their
    // scaled endpoint, keeping that transition continuous too.
    const double released = 1.0-std::min(1.0,std::max(control,amount));
    double rest = Unit(relaxed_weight)*std::min(1.0, scale)*released;
    // A released trigger while the other fingers close is the existing
    // pointing gesture. Fade its rest curl smoothly instead of removing
    // the index's independent trigger control or inventing another button.
    if (finger == 1 && !input.trigger_touch) rest *= 1.0-Smooth(grip)*(1.0-Smooth(trigger));
    if (rest <= 0.0) return absolute;

    // Hands1 rests on the mesh's already-curved reference pose, with a
    // further 12% curl. Preserve that pose rather than treating 12% as an
    // absolute anatomical angle (which would subtract the baked curvature).
    const auto relaxed = fingers::Curl(chain, .12, fingers::DefaultLimits(false));
    fingers::Angles result;
    for (int joint = 0; joint < 3; ++joint) {
        auto turn = joint_limits::Decompose(bounds.axes[joint],
            chain.ref[joint].rotation.Inverse()*fingers::LocalRotation(chain,joint,absolute));
        const auto reference_rest = joint_limits::Decompose(bounds.axes[joint],
            chain.ref[joint].rotation.Inverse()*fingers::LocalRotation(chain,joint,relaxed));
        const double anatomical = std::max(0.0,reference_rest.flex+bounds.limits[joint].ref_bend);
        turn.flex = std::clamp(turn.flex + anatomical*rest,
                              bounds.limits[joint].flex_min, bounds.limits[joint].flex_max);
        result.extra[joint] = joint_limits::Compose(bounds.axes[joint], turn);
    }
    return result;
}

fingers::Angles FreeThumbTarget(const fingers::Chain& chain, const FVector& palm,
                               const FVector& index_base, const hands::FingerInput& input,
                               const controller_pose::Bounds& bounds, double relaxed_weight) {
    const double grip=Unit(input.grip),trigger=Unit(input.trigger);
    // Restore Hands1's small extra bend on the reference thumb at rest.
    // Active controls retain the current outside-thumb guide and its safe
    // closing endpoints; this does not restore the old full-curl thumb path.
    const double rest=Unit(relaxed_weight)*(1.0-Smooth(std::max(grip,trigger)));
    const auto active=thumb_pose::Target(chain,palm,index_base,{grip,trigger,false},bounds);
    if(rest<=0.0) return input.thumb_touch ?
        thumb_pose::Target(chain,palm,index_base,{grip,trigger,true},bounds) : active;
    const auto relaxed=fingers::Curl(chain,.15,fingers::DefaultLimits(true));
    const auto pressed=input.thumb_touch ?
        thumb_pose::Target(chain,palm,index_base,{grip,trigger,true},bounds) : active;
    fingers::Angles result;
    for(int joint=0;joint<3;++joint) {
        const auto a=joint_limits::Decompose(bounds.axes[joint],
            chain.ref[joint].rotation.Inverse()*fingers::LocalRotation(chain,joint,active));
        const auto r=joint_limits::Decompose(bounds.axes[joint],
            chain.ref[joint].rotation.Inverse()*fingers::LocalRotation(chain,joint,relaxed));
        joint_limits::Turn turn{a.flex+(r.flex-a.flex)*rest,
                               a.side+(r.side-a.side)*rest,
                               a.twist+(r.twist-a.twist)*rest};
        // Keep Hands8's small button fold independent of the base blend.
        // A face/stick press cannot move the CMC or change its side/roll.
        if(input.thumb_touch && joint>0) {
            const auto p=joint_limits::Decompose(bounds.axes[joint],
                chain.ref[joint].rotation.Inverse()*fingers::LocalRotation(chain,joint,pressed));
            turn.flex+=p.flex-a.flex;
        }
        result.extra[joint]=joint_limits::Compose(bounds.axes[joint],
            joint_limits::ClampTurn(bounds.limits[joint],turn));
    }
    return result;
}

} // namespace holevr::vr::finger_intent
