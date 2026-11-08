#pragma once
#include <cstdint>
#include <string>
#include "vr/body/skeleton.h"

namespace holevr::vr::watch {
struct HandAngles { double hour=0,minute=0,second=0; };
HandAngles LocalAngles(int hour,int minute,int second,int millisecond);
std::string FormatElapsed(double seconds);
// Elapsed wall time in the loaded run level, including pause menus. Loading
// transitions and the hub retain the completed time until a new run loads.
class RunClock {
public:
    void Loaded(uint64_t level,bool run,double now);
    void Exit(double now);
    double Elapsed(double now) const;
    bool Running() const {return running_;}
    bool HasTime() const {return have_;}
    uint64_t Level() const {return level_;}
private:
    uint64_t level_=0;
    double began_=0,last_=0;
    bool running_=false,have_=false;
};

struct TapInput {
    double now=0;
    bool tracked=false,actions=false;
    FVector head,forward{1,0,0},centre,normal{0,0,1},elbow,tip;
    double radius=2.0,tip_radius=.5;
};
// Head gaze (no eye tracker required), lifted forearm and facing watch.
bool LookingAtRaisedWatch(const TapInput& in);
class Tap {
public:
    bool Update(const TapInput& in);
    void Reset();
private:
    bool have_=false,armed_=false;
    double last_=0,dwell_=0,cooldown_=0,previous_depth_=0;
    FVector previous_tip_,previous_centre_,previous_normal_;
};

// Skin-following frame with an explicit wrist position. IK changes segment
// lengths through joint positions, so anchor distance is measured back from
// the wrist instead of assuming a fixed distance forward from the elbow.
struct Forearm {
    skel::Xf lower,twist,hand;
    skel::Xf ref_lower,ref_twist,ref_hand;
    bool has_twist=false;
};
skel::Xf Mount(const Forearm& arm,const skel::Xf& mount_in_reference_lower,
               double twist_skin_weight);
} // namespace holevr::vr::watch
