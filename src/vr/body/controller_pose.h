#pragma once
// Controller animation limits and targets, independent of contact physics.
#include "vr/body/joint_limits.h"
namespace holevr::vr::controller_pose {
struct Bounds { joint_limits::Axes axes[3]; joint_limits::Limits limits[3]; };
Bounds MakeBounds(const fingers::Chain& chain,int finger);
fingers::Angles CurlTarget(const fingers::Chain& chain,double amount,const Bounds& bounds);
}
