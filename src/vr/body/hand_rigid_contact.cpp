#include "vr/body/hand_rigid_contact.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace holevr::vr::body::hand_rigid_contact {
namespace {
using namespace ik;
bool Finite(const FVector& v) { return std::isfinite(v.X)&&std::isfinite(v.Y)&&std::isfinite(v.Z); }
FVector Ball(const FVector& v, double radius) {
    const double length=Length(v);
    return length>radius ? v*(radius/length) : v;
}
struct Halfspace { FVector normal; double distance=0; };
}
Result Resolve(const Plane* planes, int count, const Settings& settings, const Reach& reach) {
    Result result;
    if(count<0 || count>128 || (count && !planes) || !std::isfinite(settings.skin_cm) ||
       !std::isfinite(settings.maximum_translation_cm) || !std::isfinite(settings.tolerance_cm)) return result;
    const double skin=std::clamp(settings.skin_cm,0.0,0.2);
    const double maximum=std::clamp(settings.maximum_translation_cm,0.1,50.0);
    const double tolerance=std::clamp(settings.tolerance_cm,0.0001,0.1);
    const bool limited_reach=reach.maximum_cm>0;
    if(!std::isfinite(reach.maximum_cm) || (limited_reach &&
       (!Finite(reach.wrist)||!Finite(reach.shoulder)||reach.maximum_cm>1000))) return result;
    std::array<Halfspace,128> constraint{};
    for(int i=0;i<count;++i) {
        const auto& p=planes[i];
        if(!Finite(p.point)||!Finite(p.sample)||!Finite(p.normal)||!std::isfinite(p.radius)||
           p.radius<0 || p.radius>20 || !std::isfinite(Length(p.normal)) || Length(p.normal)<0.5) return result;
        const FVector normal=Normalize(p.normal);
        const double distance=p.radius+skin-Dot(p.sample-p.point,normal);
        if(!std::isfinite(distance)) return result;
        // Planar walls often produce dozens of identical witnesses. Merge
        // only numerically identical directions, keeping the deepest root.
        int same=-1;
        for(int k=0;k<result.planes;++k) if(Dot(normal,constraint[k].normal)>1.0-1e-10) {same=k;break;}
        if(same>=0) constraint[same].distance=std::max(constraint[same].distance,distance);
        else constraint[result.planes++]={normal,distance};
    }
    result.valid=true;
    // Dykstra projects onto the intersection, keeping correction terms so
    // ordering does not turn a slide into gratuitous sideways displacement.
    std::array<FVector,130> correction{};
    FVector shift;
    const int passes=std::clamp(settings.iterations,1,48);
    for(int pass=0;pass<passes;++pass) {
        const FVector before=shift;
        for(int i=0;i<result.planes;++i) {
            const auto& plane=constraint[i];
            const FVector candidate=shift+correction[i];
            const double penetration=plane.distance-Dot(candidate,plane.normal);
            shift=candidate+plane.normal*std::max(0.0,penetration);
            correction[i]=candidate-shift;
        }
        const FVector bounded=shift+correction[128];
        shift=Ball(bounded,maximum); correction[128]=bounded-shift;
        if(limited_reach) {
            const FVector candidate=shift+correction[129];
            shift=reach.shoulder-reach.wrist+Ball(reach.wrist+candidate-reach.shoulder,reach.maximum_cm);
            correction[129]=candidate-shift;
        }
        if(!Finite(shift)) return {};
        result.iterations=pass+1;
        if(pass>0 && Length(shift-before)<1e-8) break;
    }
    result.translation=Ball(shift,maximum);
    for(int i=0;i<result.planes;++i)
        result.residual_cm=std::max(result.residual_cm,constraint[i].distance-Dot(result.translation,constraint[i].normal));
    if(limited_reach) result.residual_cm=std::max(result.residual_cm,
        Length(reach.wrist+result.translation-reach.shoulder)-reach.maximum_cm);
    result.feasible=result.residual_cm<=tolerance;
    return result;
}
}
