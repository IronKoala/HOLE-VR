#include "vr/body/controller_pose.h"
#include <algorithm>
#include <cmath>
namespace holevr::vr::controller_pose {
Bounds MakeBounds(const fingers::Chain& c, int finger) {
    Bounds b;
    double ref[3];
    joint_limits::ReferenceBends(c, c.thumb, ref);
    for (int j = 0; j < 3; ++j) {
        b.axes[j] = joint_limits::JointAxes(c, j);
        b.limits[j] = joint_limits::Relative(joint_limits::Human(finger, j), ref[j]);
    }
    return b;
}

fingers::Angles CurlTarget(const fingers::Chain& c,double amount,const Bounds& b) {
    const auto curl=fingers::Curl(c,std::clamp(std::isfinite(amount)?amount:0.0,0.0,1.2),fingers::DefaultLimits(c.thumb));
    fingers::Angles pose;
    for(int j=0;j<3;++j) {
        const double flex=curl.flex[j]-b.limits[j].ref_bend;
        pose.extra[j]=joint_limits::Compose(b.axes[j],{
            std::clamp(std::isfinite(flex)?flex:0.0,b.limits[j].flex_min,b.limits[j].flex_max),0,0});
    }
    return pose;
}
}
