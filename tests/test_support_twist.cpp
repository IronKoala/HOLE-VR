// clang++ -std=c++20 -Isrc tests/test_support_twist.cpp src/vr/body/arm_pose.cpp
// src/vr/body/ik_math.cpp src/vr/body/skeleton.cpp -o test_support_twist
#include "vr/body/arm_pose.h"
#include <algorithm>
#include <cstdio>
#include <limits>
using namespace holevr::vr;
using namespace holevr::vr::ik;
using namespace holevr::vr::arm_pose;
static int failures=0,checks=0;
#define EXPECT(c) do { ++checks; if(!(c)){std::printf("FAIL %d: %s\n",__LINE__,#c);++failures;} }while(0)
static double Difference(double a,double b){return std::fabs(std::remainder(a-b,2*kPi));}
static bool Near(FVector a,FVector b,double e=1e-7){return Length(a-b)<e;}
static double RotationDistance(FQuat a,FQuat b){const auto q=Normalized(a*b.Inverse());return 2*std::acos(std::clamp(std::fabs(q.W),0.,1.));}
struct Rig {Reference ref;FVector palm;};
static Rig ActualRig(int side){
    // WhiteMan reference-pose component frames extracted from the game's
    // skeleton, including its mirrored bone axes and hand bind rotation.
    Rig r;r.ref.valid=true;r.ref.upper_length=30.340;r.ref.forearm_length=26.975;
    if(side==0){
        r.ref.upper_axis={.9999999999999545,-2.538806220392e-7,-1.617786969943e-7};
        r.ref.forearm_axis={.9999999999999732,3.690756485847e-8,-2.289209271866e-7};
        r.ref.upper_normal={6.543667893655e-8,-.3412311163484239,.9399794280917027};
        r.ref.forearm_normal={2.312085493617e-7,-.2336209917554025,.9723277390937528};
        r.ref.forearm_to_hand={-.6178951933079064,-.0193719787901923,.0106534578846181,-.7859495914877991};
        r.palm={-.0998257167249644,-.9897896507963158,-.1017412082533654};
    }else{
        r.ref.upper_axis={-.9999999999993965,6.578517262879e-8,-1.096626905284e-6};
        r.ref.forearm_axis={-.9999999999999498,-2.522205824196e-7,-1.908099863668e-7};
        r.ref.upper_normal={-1.053254554351e-6,-.3412314914067499,.9399792919374428};
        r.ref.forearm_normal={-1.266057763072e-7,-.2336211390111321,.9723277037126556};
        r.ref.forearm_to_hand={-.6178953081959182,-.0193717602569519,.0106531721413063,-.7859495104250313};
        r.palm={.0998369946459101,.9897895735157024,.1017308933396173};
    }
    return r;
}
static void GradualRollAndBoundedHistory(){
 for(int side=0;side<2;++side)for(double share:{0.,.25,.5,1.}){
    const auto r=ActualRig(side);const auto base=ToQuat({23,41,-17});const auto axis=base.Rotate(r.ref.forearm_axis);
    const auto neutral=Normalized(base*r.ref.forearm_to_hand);SupportTwistState state;double previous=0;
    for(int degree=0;degree<=1440;++degree){
        auto hand=Normalized(AxisAngle(axis,degree*kDegToRad)*neutral);
        if(degree%2)hand={-hand.X,-hand.Y,-hand.Z,-hand.W};
        const auto out=FollowSupportTwist(r.ref,base,hand,r.palm,1./90,share,&state);
        EXPECT(out.valid&&out.observable);EXPECT(Difference(out.target_radians,degree*kDegToRad*share)<1e-8);
        EXPECT(std::fabs(out.target_radians)<=kPi+1e-9&&std::fabs(out.forearm_radians)<=kPi+1e-9);
        if(degree)EXPECT(Difference(out.forearm_radians,previous)<=kTwistBoneRate/90+1e-9);
        previous=out.forearm_radians;
    }
    for(int degree=1439;degree>=-720;--degree){
        const auto hand=Normalized(AxisAngle(axis,degree*kDegToRad)*neutral);
        const auto out=FollowSupportTwist(r.ref,base,hand,r.palm,1./90,share,&state);
        EXPECT(out.valid&&out.observable);EXPECT(Difference(out.target_radians,degree*kDegToRad*share)<1e-8);
        EXPECT(Difference(out.forearm_radians,previous)<=kTwistBoneRate/90+1e-9);previous=out.forearm_radians;
    }
    // Releasing/regrabbing gets a natural branch, not the previous attachment's history.
    state={};const auto out=FollowSupportTwist(r.ref,base,AxisAngle(axis,-170*kDegToRad)*neutral,r.palm,1./90,share,&state);
    EXPECT(Difference(out.target_radians,-170*kDegToRad*share)<1e-9);
 }
}
static void SeamNoiseAndProjectionLoss(){
 for(int side=0;side<2;++side){
    const auto r=ActualRig(side);const auto base=ToQuat({-19,44,37});const auto axis=Normalize(base.Rotate(r.ref.forearm_axis));
    const auto neutral=Normalized(base*r.ref.forearm_to_hand);SupportTwistState state;
    FollowSupportTwist(r.ref,base,AxisAngle(axis,170*kDegToRad)*neutral,r.palm,1./90,.5,&state);
    for(int frame=0;frame<900;++frame){
        const double angle=(180+.4*std::sin(frame*.83))*kDegToRad;
        auto hand=Normalized(AxisAngle(axis,angle)*neutral);if(frame%2)hand={-hand.X,-hand.Y,-hand.Z,-hand.W};
        const auto out=FollowSupportTwist(r.ref,base,hand,r.palm,1./90,.5,&state);
        EXPECT(out.valid&&out.observable);EXPECT(Difference(out.target_radians,90*kDegToRad)<.21*kDegToRad);
    }
    const double before=state.target_radians;
    const auto hand_axis=Normalize(r.ref.forearm_to_hand.Inverse().Rotate(r.ref.forearm_axis));
    const auto palm=Normalize(Reject(r.palm,hand_axis));
    const auto world_palm=neutral.Rotate(palm);
    // The hand's palm becomes parallel to the forearm: projected roll is unobservable.
    const auto singular=AlignFrames(hand_axis,palm,world_palm,axis);
    for(int frame=0;frame<40;++frame){
        auto hand=singular;if(frame%2)hand={-hand.X,-hand.Y,-hand.Z,-hand.W};
        const auto out=FollowSupportTwist(r.ref,base,hand,r.palm,1./90,.5,&state);
        EXPECT(out.valid&&!out.observable);EXPECT(Difference(out.target_radians,before)<1e-9);
    }
    auto out=FollowSupportTwist(r.ref,base,AxisAngle(axis,120*kDegToRad)*neutral,r.palm,1./90,.5,&state);
    EXPECT(out.valid&&out.observable);EXPECT(Difference(out.target_radians,before)<1e-9);
    out=FollowSupportTwist(r.ref,base,AxisAngle(axis,130*kDegToRad)*neutral,r.palm,1./90,.5,&state);
    EXPECT(Difference(out.target_radians,before+5*kDegToRad)<1e-9); // following resumes, not permanently frozen
    const double held=out.target_radians;
    out=FollowSupportTwist(r.ref,base,AxisAngle(axis,175*kDegToRad)*neutral,r.palm,.7,.5,&state);
    EXPECT(out.valid&&Difference(out.target_radians,held)<1e-9); // unobserved long gap is rebased
    state={};out=FollowSupportTwist(r.ref,base,singular,r.palm,1./90,.5,&state);EXPECT(!out.valid&&!state.valid);
    out=FollowSupportTwist(r.ref,base,neutral,r.palm,1./90,.5,&state);EXPECT(out.valid);
    out=FollowSupportTwist(r.ref,base,neutral,{0,0,0},1./90,.5,&state);EXPECT(!out.valid&&!state.valid);
 }
}
static void UnobservableAttachmentEntersFromFallback(){
 for(int side=0;side<2;++side)for(double seed:{-179.,-80.,80.,179.}){
    const auto r=ActualRig(side);const auto base=ToQuat({13,-23,31});
    const auto axis=Normalize(base.Rotate(r.ref.forearm_axis));
    const auto hand_axis=Normalize(r.ref.forearm_to_hand.Inverse().Rotate(r.ref.forearm_axis));
    const auto palm=Normalize(Reject(r.palm,hand_axis));
    const auto neutral=Normalized(base*r.ref.forearm_to_hand);
    const auto singular=AlignFrames(hand_axis,palm,neutral.Rotate(palm),axis);
    SupportTwistState state;
    for(int frame=0;frame<20;++frame){
        const auto out=FollowSupportTwist(r.ref,base,singular,r.palm,1./90,.5,&state,seed*kDegToRad);
        EXPECT(!out.valid&&!state.valid); // runtime continues its global fallback
    }
    const auto authored=Normalized(AxisAngle(axis,120*kDegToRad)*neutral);
    auto out=FollowSupportTwist(r.ref,base,authored,r.palm,1./90,.5,&state,seed*kDegToRad);
    EXPECT(out.valid&&out.observable);
    EXPECT(Difference(out.forearm_radians,seed*kDegToRad)<1e-9);
    EXPECT(Difference(out.target_radians,60*kDegToRad)<1e-9);
    auto previous=Normalized(AxisAngle(axis,out.forearm_radians)*base);
    for(int frame=0;frame<300;++frame){
        auto hand=authored;if(frame%2)hand={-hand.X,-hand.Y,-hand.Z,-hand.W};
        // Later fallback values cannot re-seed an established attachment.
        out=FollowSupportTwist(r.ref,base,hand,r.palm,1./90,.5,&state,-seed*kDegToRad);
        const auto current=Normalized(AxisAngle(axis,out.forearm_radians)*base);
        EXPECT(out.valid&&out.observable);EXPECT(RotationDistance(current,previous)<=kTwistBoneRate/90+1e-7);
        EXPECT(std::fabs(out.forearm_radians)<=kPi+1e-9);previous=current;
    }
    EXPECT(Difference(out.forearm_radians,60*kDegToRad)<1e-7);
    state={}; // release: no correction leaks into another grip
    out=FollowSupportTwist(r.ref,base,neutral,r.palm,1./90,.5,&state,-seed*kDegToRad);
    EXPECT(Difference(out.forearm_radians,-seed*kDegToRad)<1e-9);
    EXPECT(Difference(out.target_radians,0)<1e-9);
 }
}
static void RecordedARSeamAndPinnedHand(){
    const auto left=ActualRig(0);const FQuat recorded=Normalized({.2982,.7671,-.4354,-.3646});
    const FVector elbows[]={{26.79,-1.96,-26.45},{26.97,1.27,-26.31}};
    const FVector wrists[]={{33.44,8.13,-8.87},{33.86,11.23,-8.74}};
    for(int side=0;side<2;++side){
        const auto r=ActualRig(side);auto mirror=[&](FVector p){return side?FVector{p.X,-p.Y,p.Z}:p;};
        const auto left_hand_axis=left.ref.forearm_to_hand.Inverse().Rotate(left.ref.forearm_axis);
        const auto right_hand_axis=r.ref.forearm_to_hand.Inverse().Rotate(r.ref.forearm_axis);
        const auto hand=side?AlignFrames(right_hand_axis,r.palm,mirror(recorded.Rotate(left_hand_axis)),mirror(recorded.Rotate(left.palm))):recorded;
        TwistState old;SupportTwistState current;double old_begin=0,new_begin=0,old_end=0,new_end=0,max_step=0,previous=0;
        for(int tick=0;tick<900;++tick){
            const double t=std::clamp((tick-180)/540.,0.,1.);
            const TwoBoneResult solved{mirror(elbows[0]+(elbows[1]-elbows[0])*t),mirror(wrists[0]+(wrists[1]-wrists[0])*t),true};
            const auto rotation=Orient(r.ref,{},solved,{0,0,-1});EXPECT(rotation.valid);
            const auto old_roll=FollowTwist(WristTwist(r.ref,rotation.forearm,hand),1./90,.5,1.,&old);
            const auto now=FollowSupportTwist(r.ref,rotation.forearm,hand,r.palm,1./90,.5,&current,old_roll.forearm_radians);
            EXPECT(now.valid&&now.observable);
            if(tick)EXPECT(Difference(now.forearm_radians,previous)<=kTwistBoneRate/90+1e-9);
            else EXPECT(Difference(now.forearm_radians,old_roll.forearm_radians)<1e-9);
            // Separate the bounded attachment-entry blend from the later
            // constant-hand geometry sweep that triggered the original flip.
            if(tick>=180)max_step=std::max(max_step,Difference(now.forearm_radians,previous));
            previous=now.forearm_radians;
            if(tick==179){old_begin=old_roll.forearm_radians;new_begin=now.forearm_radians;}
            old_end=old_roll.forearm_radians;new_end=now.forearm_radians;
            // Same component-space application order as the arm solve: changing
            // the parent roll cannot move the final authored hand/finger pose.
            skel::Pose pose;pose.Init({-1,0,1});skel::Xf lower,drawn,finger;
            lower.translation=solved.mid;lower.rotation=Normalized(AxisAngle(Normalize(solved.end-solved.mid),now.forearm_radians)*rotation.forearm);
            drawn.translation=solved.end;drawn.rotation=hand;finger.translation={8,1,-2};
            pose.SetC(0,lower);pose.SetC(1,drawn);pose.SetL(2,finger);
            EXPECT(Near(pose.C(1).translation,solved.end));EXPECT(RotationDistance(pose.C(1).rotation,hand)<1e-7);
            EXPECT(Near(pose.C(2).translation,solved.end+hand.Rotate(finger.translation)));
        }
        std::printf("actual AR side%d: old half-roll change %.3f deg; new %.3f deg; new maximum step %.6f deg\n",side,Difference(old_begin,old_end)*kRadToDeg,Difference(new_begin,new_end)*kRadToDeg,max_step*kRadToDeg);
        EXPECT(Difference(old_begin,old_end)>150*kDegToRad);
        EXPECT(Difference(new_begin,new_end)<10*kDegToRad);EXPECT(max_step<.1*kDegToRad);
    }
}
static void ExistingFilterPreservesSupportOnLongFrame(){
    const auto r=ActualRig(0);TwistState state;FollowTwist(0,.01,.5,1.,&state);
    FollowSupportTwist(r.ref,FQuat{},r.ref.forearm_to_hand,r.palm,.01,.5,&state.support);
    EXPECT(state.support.valid);state.support.target_radians=.4;
    FollowTwist(.1,.5,.5,1.,&state);EXPECT(state.support.valid&&std::fabs(state.support.target_radians-.4)<1e-9);
    state={};EXPECT(!state.support.valid); // all existing runtime tracking/origin/rig resets clear it
}
int main(){GradualRollAndBoundedHistory();SeamNoiseAndProjectionLoss();UnobservableAttachmentEntersFromFallback();RecordedARSeamAndPinnedHand();ExistingFilterPreservesSupportOnLongFrame();std::printf("support twist: %d checks, %d failures\n",checks,failures);return failures?1:0;}
