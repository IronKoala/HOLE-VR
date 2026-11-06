#include "vr/watch_math.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace holevr::vr::watch {
using namespace ik;
HandAngles LocalAngles(int hour,int minute,int second,int millisecond) {
    const double s=second+millisecond/1000.0,m=minute+s/60.0,h=hour%12+m/60.0;
    // Original Blender face reflected to UE coordinates: clockwise is +Z.
    return {h*30*kDegToRad,m*6*kDegToRad,s*6*kDegToRad};
}
std::string FormatElapsed(double seconds) {
    if(!std::isfinite(seconds))seconds=0;
    const auto n=static_cast<int>(std::clamp(seconds,0.0,359999.0));
    char out[16];
    if(n>=3600)std::snprintf(out,sizeof(out),"%02d:%02d:%02d",n/3600,n/60%60,n%60);
    else std::snprintf(out,sizeof(out),"%02d:%02d",n/60,n%60);
    return out;
}
void RunClock::Loaded(uint64_t level,bool run,double now) {
    if(!level || !std::isfinite(now) || level==level_)return;
    Exit(now);level_=level;
    if(run){began_=now;last_=0;running_=have_=true;}
}
void RunClock::Exit(double now) {
    if(running_ && std::isfinite(now)){last_=Elapsed(now);running_=false;}
}
double RunClock::Elapsed(double now) const {
    return running_ && std::isfinite(now)?std::max(0.0,now-began_):last_;
}
bool LookingAtRaisedWatch(const TapInput& i) {
    const FVector d=i.centre-i.head;
    const double distance=Length(d);
    return i.tracked && i.actions && std::isfinite(distance) && distance>=15 && distance<=75 &&
        i.centre.Z>=i.head.Z-35 && i.centre.Z>=i.elbow.Z-3 &&
        Dot(Normalize(d),Normalize(i.forward))>=std::cos(18*kDegToRad) &&
        Dot(Normalize(d)*-1,Normalize(i.normal))>=.55;
}
void Tap::Reset(){have_=armed_=false;dwell_=0;}
bool Tap::Update(const TapInput& i) {
    if(!std::isfinite(i.now) || !LookingAtRaisedWatch(i)){Reset();return false;}
    const FVector normal=Normalize(i.normal),delta=i.tip-i.centre;
    const double depth=Dot(delta,normal),radial=Length(delta-normal*depth);
    if(!std::isfinite(depth) || !std::isfinite(radial)){Reset();return false;}
    const double dt=have_?i.now-last_:0;
    const bool continuous=have_ && dt>0 && dt<=.1 && Length(i.tip-previous_tip_)<30 &&
        Length(i.centre-previous_centre_)<30 && Dot(normal,previous_normal_)>=.8;
    if(!continuous){armed_=false;dwell_=0;}
    dwell_+=continuous?dt:0;
    const double skin=std::clamp(i.tip_radius,.2,.8);
    // Re-arm only after lifting away from the face, never on a gaze change
    // with the finger already resting on the glass.
    if(dwell_>=.15 && depth>skin+1.0 && radial<i.radius+2.0 && i.now>=cooldown_)armed_=true;
    // Sweep the fingertip across the face plane: a quick tap may travel
    // through it between two VR frames. Test the crossing, not only the end.
    bool pressed=false;
    if(continuous && armed_ && previous_depth_>skin && depth<=skin) {
        const double t=(previous_depth_-skin)/(previous_depth_-depth);
        const FVector a=previous_tip_-previous_centre_;
        const FVector crossing=a+(delta-a)*t;
        const double radius=Length(crossing-normal*Dot(crossing,normal));
        pressed=radius<=i.radius+.2;
    }
    previous_depth_=depth;previous_tip_=i.tip;previous_centre_=i.centre;previous_normal_=normal;last_=i.now;have_=true;
    if(pressed){armed_=false;cooldown_=i.now+.75;return true;}
    return false;
}
skel::Xf Mount(const Forearm& a,const skel::Xf& local,double weight) {
    FQuat q=a.lower.rotation;
    if(a.has_twist) {
        const FQuat ref_delta=Normalized(a.ref_lower.rotation.Inverse()*a.ref_twist.rotation);
        const FQuat target=Normalized(a.twist.rotation*ref_delta.Inverse());
        q=Normalized(Scale(target*q.Inverse(),std::clamp(weight,0.0,1.0))*q);
    }
    const auto hand_ref=skel::Relative(a.ref_hand,a.ref_lower);
    const FVector back=local.translation-hand_ref.translation;
    skel::Xf result;
    result.rotation=Normalized(q*local.rotation);
    result.scale=a.lower.scale;
    result.translation=a.hand.translation+q.Rotate({back.X*result.scale.X,back.Y*result.scale.Y,back.Z*result.scale.Z});
    return result;
}
} // namespace holevr::vr::watch
