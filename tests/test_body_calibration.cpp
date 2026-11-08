// Native: clang++ -std=c++20 -Isrc tests/test_body_calibration.cpp
// src/vr/body/body_calibration.cpp src/vr/body/wrist_calibration.cpp src/vr/body/functional_reach.cpp
// src/vr/body/elbow_calibration.cpp src/vr/body/ik_math.cpp -o test_body_calibration
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <sstream>
#include <string>
#include "vr/body/body_calibration.h"
#include "vr/body/ik_math.h"
using namespace holevr::vr;
using namespace holevr::vr::body_calibration;
static int failures=0,checks=0;
#define EXPECT(c) do { ++checks; if(!(c)) { std::printf("FAIL line %d: %s\n",__LINE__,#c); ++failures; } } while(0)
static double Distance(FVector a,FVector b) { return ik::Length(a-b); }
static bool Near(double a,double b,double tolerance=.15) { return std::fabs(a-b)<tolerance; }
// Known asymmetric anatomy, independent of the estimator. A bend changes
// only the forearm direction; palm orientation changes continuously.
struct Rig {
    FVector shoulder[2]={{-5,-20,-25},{-4,21,-24}};
    double upper[2]={31,35},lower[2]={28,30};
    FVector head={500,130,170}; double yaw=73,tilt_down_deg=0;
    FVector Forward(int s) const { const double r=upper[s]+lower[s],a=tilt_down_deg*kDegToRad; return shoulder[s]+FVector{r*std::cos(a),0,-r*std::sin(a)}; }
    FVector Out(int s) const { const double r=upper[s]+lower[s],a=tilt_down_deg*kDegToRad; return shoulder[s]+FVector{0,(s?1.:-1.)*r*std::cos(a),-r*std::sin(a)}; }
    FVector Bent(int s,double progress) const {
        const double elevation=tilt_down_deg*kDegToRad,angle=progress+(progress/(kPi/2)-1)*elevation,sign=s?1.:-1.;
        // Keep the upper arm at the comfortable side-pose elevation; move
        // only the forearm, ending upright, with a neutral wrist throughout.
        return shoulder[s]+FVector{0,sign*(upper[s]*std::cos(elevation)+lower[s]*std::cos(angle)),
            -upper[s]*std::sin(elevation)+lower[s]*std::sin(angle)};
    }
};
struct Run {
    Settings settings; Session session; Rig rig;
    double time=0,dt=1./90.,noise=0,common_sway=0,neck_sway=0,transition_bow=0;
    bool jitter=false,floor_known=true,hand_valid=true,head_valid=true;
    HeightMode height_mode=HeightMode::Real; HeightPosture height_posture=HeightPosture::Standing;
    FVector translation,neck,prior_error; double prior[2]={.44,.52};
    unsigned frame=0,transition_lobes=1; std::uint64_t origin=7;
    explicit Run(Settings s={}) : settings(s),session(s) {}
    Sample Make(FVector left,FVector right,double bend=-1) {
        time+=dt*(jitter?(frame%7==0?1.8:.8666666666667):1.); ++frame;
        Sample sample; sample.seconds=time; sample.valid=hand_valid&&head_valid;
        sample.grip_orientation_valid=hand_valid; sample.head_valid=head_valid;
        sample.floor_known=floor_known; sample.origin_generation=origin;
        sample.height_mode=height_mode; sample.height_posture=height_posture;
        const FQuat yaw=YawOnly(rig.yaw);
        const FVector common=translation+FVector{common_sway*std::sin(time*.8),common_sway*.6*std::cos(time*.7),common_sway*.3*std::sin(time)};
        const FVector neck_motion=neck+FVector{neck_sway*std::sin(time*1.7),neck_sway*.6*std::cos(time*1.3),neck_sway*.3*std::sin(time*1.1)};
        sample.head=rig.head+yaw.Rotate(common+neck_motion); sample.head_rotation=yaw;
        const FVector p[2]={left,right};
        for(int s=0;s<2;++s) {
            const double sign=s?1.:-1.,phase=frame*.71+s*1.1;
            const FVector n{noise*std::sin(phase),noise*std::cos(phase*.83),noise*std::sin(phase*1.29)};
            sample.wrist[s]=rig.head+yaw.Rotate(common+p[s]+n);
            FVector forearm=ik::Normalize(p[s]-rig.shoulder[s]);
            FVector palm=session.GetStatus().phase==Phase::ArmsOut?FVector{0,0,-1}:FVector{0,-sign,0};
            if(bend>=0) {
                const double angle=bend+(bend/(kPi/2)-1)*rig.tilt_down_deg*kDegToRad;
                forearm={0,sign*std::cos(angle),std::sin(angle)};
                palm=ik::AxisAngle(forearm,2*bend).Rotate({0,sign*std::sin(angle),-std::cos(angle)});
            }
            sample.grip_rotation[s]=yaw*ik::AlignFrames({1,0,0},{0,0,1},forearm,palm);
            sample.wrist_offset_grip_cm[s]={-5,sign*2,1}; sample.prior_forearm_fraction[s]=prior[s];
            sample.prior_shoulder[s]=rig.shoulder[s]+prior_error; sample.prior_reach_cm[s]=rig.upper[s]+rig.lower[s];
        }
        return sample;
    }
    Status Step(FVector l,FVector r,double bend=-1) { return session.Update(Make(l,r,bend)); }
    void Confirm(FVector l,FVector r,double bend=-1) { auto s=Make(l,r,bend); s.confirm_pose=true; session.Update(s); }
    void Feed(FVector l,FVector r,double seconds,double bend=-1) { const double end=time+seconds; while(time<end) Step(l,r,bend); }
    void Height() { Confirm({0,-20,-60},{0,20,-60}); Feed({0,-20,-60},{0,20,-60},settings.hold_seconds+.5); }
    void Forward() { Confirm(rig.Forward(0),rig.Forward(1)); Feed(rig.Forward(0),rig.Forward(1),settings.hold_seconds+.5); }
    void TransitionOut() {
        const double begin=time;
        while(time-begin<1.6 && session.GetStatus().phase==Phase::ArmsOut) {
            const double theta=std::min(1.0,(time-begin)/1.6)*kPi/2,elevation=rig.tilt_down_deg*kDegToRad;
            FVector p[2];
            for(int s=0;s<2;++s) {
                const FVector direction=ik::Normalize({std::cos(theta)*std::cos(elevation),(s?1.:-1.)*std::sin(theta)*std::cos(elevation),
                    -std::sin(elevation)+transition_bow*std::sin(2*theta*transition_lobes)});
                p[s]=rig.shoulder[s]+direction*(rig.upper[s]+rig.lower[s]);
            }
            auto sample=Make(p[0],p[1]);
            for(int s=0;s<2;++s) {
                const FVector direction=ik::Normalize(p[s]-rig.shoulder[s]);
                const FVector palm{0,-(s?1.:-1.)*std::cos(theta),-std::sin(theta)};
                sample.grip_rotation[s]=YawOnly(rig.yaw)*ik::AlignFrames({1,0,0},{0,0,1},direction,palm);
            }
            session.Update(sample);
        }
    }
    void Out(bool transition=true) {
        if(transition) TransitionOut();
        Confirm(rig.Out(0),rig.Out(1));
        Feed(rig.Out(0),rig.Out(1),settings.hold_seconds+.5);
    }
    void Bend(bool motion=true) {
        if(motion) { const double begin=time,end=time+3; while(time<end && session.GetStatus().phase==Phase::BendElbows) {
            const double a=(time-begin)/3*kPi/2; Step(rig.Bent(0,a),rig.Bent(1,a),a);
        }}
        Confirm(rig.Bent(0,kPi/2),rig.Bent(1,kPi/2),kPi/2);
        Feed(rig.Bent(0,kPi/2),rig.Bent(1,kPi/2),settings.hold_seconds+.7,kPi/2);
    }
    void Verify() { Forward(); }
    void Full(bool motion=true) { Height(); Forward(); Out(); Bend(motion); Verify(); }
};
static void CheckGeometry(const Run& r,double tolerance=.15) {
    const auto* p=r.session.GetProfile(); EXPECT(p); if(!p) return;
    EXPECT(p->version==Profile::kVersion && p->arms_measured && Validate(*p));
    for(int s=0;s<2;++s) {
        EXPECT(Distance(p->arm[s].shoulder,r.rig.shoulder[s])<tolerance);
        EXPECT(Near(p->arm[s].upper_cm+p->arm[s].forearm_cm,r.rig.upper[s]+r.rig.lower[s],tolerance));
        if(p->arm[s].segments_measured) {
            EXPECT(Near(p->arm[s].upper_cm,r.rig.upper[s],tolerance)); EXPECT(Near(p->arm[s].forearm_cm,r.rig.lower[s],tolerance));
        }
        if(p->arm[s].forearm_measured) EXPECT(Near(p->arm[s].forearm_cm,r.rig.lower[s],tolerance));
    }
}
static void StagesAndRates() {
    for(double hz:{5.,7.,30.,144.}) {
        Run r; r.dt=1/hz;
        r.Height(); EXPECT(r.session.GetStatus().phase==Phase::ArmsForward);
        EXPECT(r.session.GetStatus().height_measured && Near(r.session.GetStatus().measured_eye_cm,170));
        r.Forward(); EXPECT(r.session.GetStatus().phase==Phase::ArmsOut && !r.session.GetProfile());
        r.Out(); EXPECT(r.session.GetStatus().phase==Phase::BendElbows && !r.session.GetProfile());
        r.Bend(); EXPECT(r.session.GetStatus().phase==Phase::VerifyForward && !r.session.GetProfile());
        r.Verify(); EXPECT(r.session.GetStatus().phase==Phase::Complete); CheckGeometry(r);
        if(const auto* p=r.session.GetProfile()) {
            EXPECT(p->arm[0].forearm_measured && p->arm[1].forearm_measured);
            EXPECT(!p->arm[0].segments_measured && !p->arm[1].segments_measured);
            EXPECT(!p->arm[0].shoulder_measured && !p->arm[1].shoulder_measured);
            EXPECT(p->arm[0].shoulder_observed_dimensions==2 && p->arm[1].shoulder_observed_dimensions==2);
            const auto saved=Serialize(*p); ++r.origin; r.Step({},{}); EXPECT(Serialize(*r.session.GetProfile())==saved);
        }
        r.session.Reset(); EXPECT(!r.session.GetProfile());
    }
    Run jitter; jitter.dt=1./7.; jitter.jitter=true; jitter.Full(); CheckGeometry(jitter);
    Run slow; slow.Height(); slow.Feed({0,-20,-80},{0,20,-80},8);
    EXPECT(slow.session.GetStatus().phase==Phase::ArmsForward);
    EXPECT(!slow.session.GetStatus().pose_confirmed && slow.session.GetStatus().hold_fraction==0);
    EXPECT(slow.session.GetStatus().stage_elapsed_seconds>8);
    const double begin=slow.time;
    slow.Confirm(slow.rig.Forward(0),slow.rig.Forward(1));
    while(slow.session.GetStatus().phase==Phase::ArmsForward && slow.time-begin<5) {
        slow.Step(slow.rig.Forward(0),slow.rig.Forward(1));
        if(slow.time-begin<slow.settings.hold_seconds-.01) EXPECT(!slow.session.GetStatus().baseline_locked);
    }
    EXPECT(slow.session.GetStatus().phase==Phase::ArmsOut && slow.time-begin>=slow.settings.hold_seconds);
}
static void MotionAndOutliers() {
    Run sway; sway.common_sway=2; sway.noise=.2; sway.Full(); CheckGeometry(sway,.5);
    Run moved; moved.Height(); moved.Forward(); moved.translation={6,0,0}; moved.Out(); moved.Bend(); moved.Verify(); CheckGeometry(moved);
    EXPECT(moved.session.GetStatus().attempt==1);
    Run neck; neck.neck_sway=.7; neck.Full(); CheckGeometry(neck,2.0);
    Run outlier; outlier.Height(); const double begin=outlier.time; bool injected=false;
    outlier.Confirm(outlier.rig.Forward(0),outlier.rig.Forward(1));
    while(outlier.session.GetStatus().phase==Phase::ArmsForward && outlier.time-begin<7) {
        auto s=outlier.Make(outlier.rig.Forward(0),outlier.rig.Forward(1));
        if(!injected && outlier.time-begin>.8) {
            s.wrist[0]=s.wrist[0]+YawOnly(outlier.rig.yaw).Rotate({0,15,0}); injected=true;
        }
        outlier.session.Update(s);
    }
    EXPECT(injected && outlier.session.GetStatus().phase==Phase::ArmsOut);
    outlier.Out(); EXPECT(outlier.session.GetStatus().phase==Phase::BendElbows);
    outlier.Bend(); outlier.Verify(); CheckGeometry(outlier,.4);
    Run drift; drift.Height(); const double start=drift.time;
    drift.Confirm(drift.rig.Forward(0),drift.rig.Forward(1));
    while(drift.time-start<5) {
        const double d=(drift.time-start)*4;
        drift.Step(drift.rig.Forward(0)+FVector{d,0,0},drift.rig.Forward(1)+FVector{d,0,0});
    }
    // Collection is representative rather than a hidden stillness test. This
    // stage can advance; the later geometric fit still has to validate data.
    EXPECT(drift.session.GetStatus().phase==Phase::ArmsOut && !drift.session.GetProfile());
}
static void WristWindowsAndFrozenMapping() {
    Run spike; spike.Height();
    for(Phase phase:{Phase::ArmsForward,Phase::ArmsOut}) {
        const double begin=spike.time; bool injected=false;
        if(phase==Phase::ArmsForward) spike.Confirm(spike.rig.Forward(0),spike.rig.Forward(1));
        else spike.Confirm(spike.rig.Out(0),spike.rig.Out(1));
        while(spike.session.GetStatus().phase==phase && spike.time-begin<7) {
            auto sample=phase==Phase::ArmsForward?spike.Make(spike.rig.Forward(0),spike.rig.Forward(1)):
                spike.Make(spike.rig.Out(0),spike.rig.Out(1));
            // A brief raw orientation glitch with unchanged wrist positions
            // occurs INSIDE the accepted window, not before preparation.
            const double elapsed=spike.time-begin;
            if(elapsed>.8 && elapsed<.87) {
                for(int side=0;side<2;++side)
                    sample.grip_rotation[side]=sample.grip_rotation[side]*ik::AxisAngle({1,0,0},kPi/2);
                injected=true;
            }
            spike.session.Update(sample);
        }
        EXPECT(injected);
        EXPECT(spike.session.GetStatus().phase==(phase==Phase::ArmsForward?Phase::ArmsOut:Phase::BendElbows));
    }
    spike.Bend(); spike.Verify(); CheckGeometry(spike,.4);
    if(const auto* profile=spike.session.GetProfile()) for(const auto& arm:profile->arm) {
        EXPECT(arm.wrist.measured && arm.wrist.palm_rms_deg<1);
    }
    for(int change=0;change<3;++change) {
        Run r; r.Height(); r.Forward();
        // Let this stage finish its preparation so the mapping is consumed.
        r.Feed(r.rig.Out(0),r.rig.Out(1),r.settings.preparation_seconds+.1);
        auto sample=r.Make(r.rig.Out(0),r.rig.Out(1));
        if(change==0) sample.wrist_offset_grip_cm[0].X+=.2;
        if(change==1) sample.hand_position_setting_game_cm[0].Y+=.2;
        if(change==2) sample.hand_rotation_setting[0]=ik::AxisAngle({0,0,1},.1*kDegToRad);
        r.session.Update(sample);
        EXPECT(r.session.GetStatus().phase==Phase::Failed);
        EXPECT(r.session.GetStatus().reason==Reason::WristPoseMismatch && !r.session.GetProfile());
    }
    Run sign; sign.Height(); sign.Forward();
    sign.Confirm(sign.rig.Out(0),sign.rig.Out(1));
    const double end=sign.time+5;
    while(sign.time<end && sign.session.GetStatus().phase==Phase::ArmsOut) {
        auto sample=sign.Make(sign.rig.Out(0),sign.rig.Out(1));
        // q and -q describe the same frozen manual rotation.
        sample.hand_rotation_setting[0]={0,0,0,-1}; sign.session.Update(sample);
    }
    EXPECT(sign.session.GetStatus().phase==Phase::BendElbows);
}
static void ReachObservabilityAndComfort() {
    // The same true anatomy reaches at different elevations. A lowered
    // wrist is not evidence that the shoulder moved downward.
    for(double tilt:{0.,10.,20.,30.,45.}) for(bool transition:{false,true}) {
        Run r; r.rig.tilt_down_deg=tilt;
        r.Height(); r.Forward(); r.Out(transition); r.Bend(); r.Verify();
        EXPECT(r.session.GetStatus().phase==Phase::Complete); CheckGeometry(r,.2);
        if(const auto* p=r.session.GetProfile()) {
            for(const auto& arm:p->arm) {
                EXPECT(!arm.shoulder_measured);
                EXPECT(arm.shoulder_observed_dimensions==(transition?2u:1u));
            }
            EXPECT(r.session.GetStatus().reach_degraded);
            Profile parsed; EXPECT(Parse(Serialize(*p),&parsed));
            EXPECT(parsed.arm[0].shoulder_measured==p->arm[0].shoulder_measured);
        }
    }
    // A head-only shift is not common torso translation. In this fixture it
    // must not be silently turned into a large shoulder/segment displacement.
    Run neck; neck.Height(); neck.Forward(); neck.neck={4,0,0}; neck.Out(); neck.Bend(); neck.neck={}; neck.Verify();
    if(const auto* p=neck.session.GetProfile()) for(int s=0;s<2;++s) {
        EXPECT(Distance(p->arm[s].shoulder,neck.rig.shoulder[s])<4);
        EXPECT(!p->arm[s].forearm_measured || std::fabs(p->arm[s].forearm_cm-neck.rig.lower[s])<4);
    }
    else EXPECT(neck.session.GetStatus().phase!=Phase::Complete);
}
static void WrongPriorsAndPerSideEvidence() {
    Run planar; planar.prior_error={7,-5,0}; planar.Full(); CheckGeometry(planar,.3);
    if(const auto* p=planar.session.GetProfile()) for(const auto& arm:p->arm) {
        EXPECT(!arm.shoulder_measured && arm.shoulder_observed_dimensions==2);
    }
    Run spatial; spatial.prior_error={5,-4,-6}; spatial.transition_bow=.25; spatial.transition_lobes=2; spatial.Full();
    CheckGeometry(spatial,.3);
    if(const auto* p=spatial.session.GetProfile()) for(const auto& arm:p->arm)
        EXPECT(arm.shoulder_measured && arm.shoulder_observed_dimensions==3 && arm.segments_measured && arm.forearm_measured);
    // A single shallow bow retains a weak-direction prior. It can support
    // the independent forearm but must NOT promote a biased upper/forearm
    // proportion to measured provenance for the next calibration attempt.
    Run weak; weak.prior_error={5,-4,-6}; weak.transition_bow=.4; weak.Full();
    const auto* conditional=weak.session.GetProfile(); EXPECT(conditional);
    if(conditional) {
        Profile parsed; EXPECT(Parse(Serialize(*conditional),&parsed));
        for(int s=0;s<2;++s) {
            EXPECT(!parsed.arm[s].shoulder_measured && parsed.arm[s].shoulder_observed_dimensions==2);
            EXPECT(parsed.arm[s].forearm_measured && !parsed.arm[s].segments_measured);
            EXPECT(Near(parsed.arm[s].forearm_cm,weak.rig.lower[s],.5));
        }
        EXPECT(Distance(conditional->arm[0].shoulder,weak.rig.shoulder[0])>3);
    }
    // Only the left side supplies a real bend. The right result must remain
    // its explicitly supplied estimate without invalidating the useful side.
    Run partial; partial.Height(); partial.Forward(); partial.Out();
    const double start=partial.time;
    while(partial.time-start<3) {
        const double a=(partial.time-start)/3*kPi/2;
        auto sample=partial.Make(partial.rig.Bent(0,a),partial.rig.Bent(1,kPi/2),kPi/2);
        const FVector f{0,-std::cos(a),std::sin(a)};
        const auto palm=ik::AxisAngle(f,2*a).Rotate({0,-std::sin(a),-std::cos(a)});
        sample.grip_rotation[0]=YawOnly(partial.rig.yaw)*ik::AlignFrames({1,0,0},{0,0,1},f,palm);
        partial.session.Update(sample);
    }
    partial.Confirm(partial.rig.Bent(0,kPi/2),partial.rig.Bent(1,kPi/2),kPi/2);
    partial.Feed(partial.rig.Bent(0,kPi/2),partial.rig.Bent(1,kPi/2),3,kPi/2);
    EXPECT(partial.session.GetStatus().phase==Phase::VerifyForward);
    partial.Verify(); EXPECT(partial.session.GetStatus().phase==Phase::Complete);
    if(const auto* p=partial.session.GetProfile()) {
        EXPECT(p->arm[0].forearm_measured && !p->arm[1].forearm_measured);
        EXPECT(!p->arm[0].segments_measured && !p->arm[1].segments_measured);
        EXPECT(Near(p->arm[0].upper_cm,31) && Near(p->arm[0].forearm_cm,28));
        EXPECT(Near(p->arm[1].forearm_cm/(p->arm[1].upper_cm+p->arm[1].forearm_cm),partial.prior[1],1e-8));
        EXPECT(p->arm[1].fit.bend_inliers==0 && partial.session.GetStatus().bend_degraded);
    }
    // Old absolute-X gate rejected these straight low reaches (X<20cm).
    // They are declared comfortable by the user and must be learned as such.
    Run low; low.rig.tilt_down_deg=70; low.Full();
    EXPECT(low.session.GetStatus().phase==Phase::Complete); CheckGeometry(low,.3);
}
static void OneBendSurvivesALongComfortablePause() {
    Run r; r.Height(); r.Forward(); r.Out();
    const double start=r.time;
    while(r.time-start<3) {
        const double a=(r.time-start)/3*kPi/2;
        r.Step(r.rig.Bent(0,a),r.rig.Bent(1,a),a);
    }
    r.Feed(r.rig.Bent(0,kPi/2),r.rig.Bent(1,kPi/2),45,kPi/2);
    EXPECT(r.session.GetStatus().phase==Phase::BendElbows && !r.session.GetStatus().pose_confirmed);
    EXPECT(r.session.GetStatus().bend_samples>=10 && r.session.GetStatus().bend_samples<120);
    r.Confirm(r.rig.Bent(0,kPi/2),r.rig.Bent(1,kPi/2),kPi/2);
    r.Feed(r.rig.Bent(0,kPi/2),r.rig.Bent(1,kPi/2),3,kPi/2);
    EXPECT(r.session.GetStatus().phase==Phase::VerifyForward && !r.session.GetProfile());
    r.Verify(); CheckGeometry(r);
    if(const auto* p=r.session.GetProfile()) EXPECT(p->arm[0].forearm_measured && p->arm[1].forearm_measured);
}
static void TrackingPausesAndTransactions() {
    Run active; active.Height(); active.Confirm(active.rig.Forward(0),active.rig.Forward(1));
    active.Feed(active.rig.Forward(0),active.rig.Forward(1),.75);
    EXPECT(active.session.GetStatus().pose_confirmed && active.session.GetStatus().hold_fraction>0);
    active.hand_valid=false; active.Feed({}, {},.5);
    active.hand_valid=true; active.Feed(active.rig.Forward(0),active.rig.Forward(1),3);
    EXPECT(active.session.GetStatus().phase==Phase::ArmsForward && !active.session.GetStatus().pose_confirmed);
    EXPECT(active.session.GetStatus().hold_fraction==0 && !active.session.GetProfile());
    active.Forward(); EXPECT(active.session.GetStatus().phase==Phase::ArmsOut);
    for(Phase target:{Phase::ArmsForward,Phase::ArmsOut,Phase::BendElbows,Phase::VerifyForward}) {
        Run r; r.Height(); if(target!=Phase::ArmsForward) r.Forward();
        if(target==Phase::BendElbows||target==Phase::VerifyForward) r.Out(); if(target==Phase::VerifyForward) r.Bend();
        EXPECT(r.session.GetStatus().phase==target);
        r.hand_valid=false; r.Feed({}, {},3);
        EXPECT(r.session.GetStatus().phase==target && r.session.GetStatus().reason==Reason::TrackingLost);
        EXPECT(!r.session.GetProfile() && r.session.GetStatus().attempt==1);
        r.hand_valid=true; if(target==Phase::ArmsForward) r.Forward();
        if(target==Phase::ArmsForward||target==Phase::ArmsOut) r.Out();
        if(target!=Phase::VerifyForward) r.Bend(); r.Verify();
        EXPECT(r.session.GetStatus().phase==Phase::Complete);
    }
    Run stale; stale.Height(); auto sample=stale.Make(stale.rig.Forward(0),stale.rig.Forward(1));
    stale.session.Update(sample); const auto before=stale.session.GetStatus();
    for(int i=0;i<500;++i) stale.session.Update(sample);
    EXPECT(stale.session.GetStatus().phase==before.phase && stale.session.GetStatus().hold_fraction==before.hold_fraction);
    auto changed=sample; changed.height_mode=HeightMode::Seated; changed.height_posture=HeightPosture::Seated;
    stale.session.Update(changed); EXPECT(stale.session.GetStatus().reason==Reason::HeightModeChanged && !stale.session.GetProfile());
    Run origin; origin.Height(); sample=origin.Make({},{}); origin.session.Update(sample); ++sample.origin_generation;
    origin.session.Update(sample); EXPECT(origin.session.GetStatus().reason==Reason::OriginChanged);
    Run clock; clock.Height(); sample=clock.Make({},{}); sample.seconds-=2; clock.session.Update(sample);
    EXPECT(clock.session.GetStatus().reason==Reason::TimeWentBackwards);
    Run gap; gap.Height(); gap.Forward(); gap.time+=1.5; gap.Step(gap.rig.Out(0),gap.rig.Out(1));
    EXPECT(gap.session.GetStatus().phase==Phase::ArmsOut && gap.session.GetStatus().hold_fraction==0);
    gap.Out(); gap.Bend(); gap.Verify(); EXPECT(gap.session.GetStatus().phase==Phase::Complete);
    Run invalid; sample=invalid.Make({},{}); sample.head.X=std::numeric_limits<double>::quiet_NaN();
    invalid.session.Update(sample); sample.seconds+=3; invalid.session.Update(sample);
    EXPECT(invalid.session.GetStatus().reason==Reason::InvalidSample && !invalid.session.GetProfile());
    Settings bad; bad.hold_seconds=std::numeric_limits<double>::quiet_NaN(); Session rejected(bad);
    EXPECT(rejected.GetStatus().phase==Phase::Failed);
}
static void BendEvidenceAndIndependentCheck() {
    Run fallback; fallback.Full(false); const auto* p=fallback.session.GetProfile(); EXPECT(p);
    if(p) for(int s=0;s<2;++s) {
        EXPECT(!p->arm[s].segments_measured && !p->arm[s].forearm_measured);
        EXPECT(Near(p->arm[s].forearm_cm/(p->arm[s].upper_cm+p->arm[s].forearm_cm),fallback.prior[s],1e-8));
        EXPECT(p->arm[s].fit.bend_inliers==0);
    }
    EXPECT(fallback.session.GetStatus().bend_degraded);
    Run mismatch; mismatch.Height(); mismatch.Forward(); mismatch.Out(); mismatch.Bend();
    mismatch.Confirm(mismatch.rig.Forward(0)+FVector{12,0,0},mismatch.rig.Forward(1));
    mismatch.Feed(mismatch.rig.Forward(0)+FVector{12,0,0},mismatch.rig.Forward(1),5);
    EXPECT(mismatch.session.GetStatus().phase==Phase::VerifyForward && !mismatch.session.GetProfile());
    EXPECT(mismatch.session.GetStatus().reason==Reason::VerificationMismatch);
    mismatch.Verify(); EXPECT(mismatch.session.GetStatus().phase==Phase::Complete);
    Run neck; neck.Height(); neck.Forward(); neck.Out(); neck.Bend(); neck.neck={8,0,0}; neck.Verify();
    EXPECT(neck.session.GetStatus().phase==Phase::VerifyForward && !neck.session.GetProfile());
    neck.neck={}; neck.Verify(); EXPECT(neck.session.GetStatus().phase==Phase::Complete);
    Run palms; palms.Height(); palms.Forward(); palms.Out(); palms.Bend();
    palms.Confirm(palms.rig.Forward(0),palms.rig.Forward(1));
    const double palm_end=palms.time+5;
    while(palms.time<palm_end) {
        auto sample=palms.Make(palms.rig.Forward(0),palms.rig.Forward(1));
        for(int side=0;side<2;++side)
            sample.grip_rotation[side]=sample.grip_rotation[side]*ik::AxisAngle({1,0,0},(side?1:-1)*kPi/2);
        palms.session.Update(sample);
    }
    EXPECT(palms.session.GetStatus().phase==Phase::VerifyForward && !palms.session.GetProfile());
    EXPECT(palms.session.GetStatus().reason==Reason::VerificationMismatch);
    palms.Verify(); EXPECT(palms.session.GetStatus().phase==Phase::Complete); CheckGeometry(palms);
    Run final_spike; final_spike.Height(); final_spike.Forward(); final_spike.Out(); final_spike.Bend();
    final_spike.Confirm(final_spike.rig.Forward(0),final_spike.rig.Forward(1));
    const double final_begin=final_spike.time,final_wait=final_spike.session.GetStatus().preparation_remaining_seconds; bool injected_final=false;
    while(final_spike.time-final_begin<6 && final_spike.session.GetStatus().phase==Phase::VerifyForward) {
        auto sample=final_spike.Make(final_spike.rig.Forward(0),final_spike.rig.Forward(1));
        const double elapsed=final_spike.time-final_begin-final_wait;
        if(elapsed>.8 && elapsed<.87) {
            sample.grip_rotation[0]=sample.grip_rotation[0]*ik::AxisAngle({1,0,0},kPi/2);
            injected_final=true;
        }
        final_spike.session.Update(sample);
    }
    EXPECT(injected_final && final_spike.session.GetStatus().phase==Phase::Complete); CheckGeometry(final_spike);
    Run source; source.Full(); const auto* measured=source.session.GetProfile(); EXPECT(measured); if(!measured)return;
    for(int side=0;side<2;++side) {
        std::vector<BendObservation> tiny;
        for(int i=0;i<100;++i) {
            const double a=(i%11)*2*kDegToRad,sign=side?1.:-1.;
            const FVector forearm{0,sign*std::cos(a),std::sin(a)};
            const auto q=ik::AlignFrames({1,0,0},{0,0,1},forearm,{1,0,0});
            tiny.push_back({source.rig.Bent(side,a),q});
        }
        SegmentFit unchanged; unchanged.upper_cm=999;
        EXPECT(FitSegments(source.rig.shoulder[side],source.rig.upper[side]+source.rig.lower[side],measured->arm[side].wrist,tiny,20,&unchanged)!=Reason::None);
        EXPECT(unchanged.upper_cm==999);
    }
}
static std::string Legacy(unsigned version) {
    std::ostringstream text;
    text<<"version="<<version<<"\nunits=cm\nmethod="<<(version==1?"frontal_arc_bent_t":version<4?"two_arcs_bent_t":"tracked_body_profile")<<'\n';
    if(version>1) text<<"arms_measured=1\n"; text<<"standing_eye_cm=170\n";
    if(version>=3) text<<"seated_eye_cm=116\nlegacy_eye_cm=0\n";
    for(int s=0;s<2;++s) {
        const std::string p=s?"right_":"left_";
        text<<p<<"shoulder=-5 "<<(s?20:-20)<<" -25\n"<<p<<"upper="<<(s?35:31)<<'\n'<<p<<"forearm="<<(s?30:28)<<'\n'
            <<p<<"radial_rms=.4\n"<<p<<"plane_rms=.1\n"<<p<<"arc_degrees=90\n"<<p<<"bent_error=.5\n"<<p<<"inliers=90\n"<<p<<"samples=95\n";
        if(version>=5) text<<p<<"segments_measured=1\n"<<p<<"shoulder_measured=0\n"<<p<<"verification_error_cm=.7\n";
        if(version>=4) text<<p<<"bend_span=40\n"<<p<<"bend_length_spread=1\n"<<p<<"bend_samples=30\n"<<p<<"bend_inliers=29\n"
            <<p<<"wrist_measured=1\n"<<p<<"forearm_axis_grip=1 0 0\n"<<p<<"palm_axis_grip=0 0 1\n"<<p<<"wrist_offset_grip_cm=-5 0 0\n"
            <<p<<"hand_position_setting_game_cm=1 2 3\n"<<p<<"hand_rotation_setting=0 0 0 1\n"
            <<p<<"forearm_rms_deg=2\n"<<p<<"palm_rms_deg=3\n"<<p<<"wrist_max_error_deg=4\n";
    }
    return text.str();
}
static void PersistenceAndMapping() {
    Run r; r.Full(); const auto* p=r.session.GetProfile(); EXPECT(p); if(!p)return;
    const auto text=Serialize(*p); Profile parsed;
    EXPECT(Parse(text,&parsed) && Serialize(parsed)==text && parsed.version==Profile::kVersion);
    for(unsigned version:{1u,2u,3u,4u,5u}) {
        EXPECT(Parse(Legacy(version),&parsed)); EXPECT(parsed.version==Profile::kVersion && parsed.arms_measured);
        EXPECT(parsed.arm[0].upper_cm==31 && parsed.arm[0].forearm_cm==28);
        EXPECT(parsed.arm[1].upper_cm==35 && parsed.arm[1].forearm_cm==30);
        EXPECT(Distance(parsed.arm[0].shoulder,{-5,-20,-25})<1e-9);
        EXPECT(parsed.arm[0].wrist.measured==(version>=4));
        EXPECT(!parsed.arm[0].shoulder_measured && !parsed.arm[1].shoulder_measured);
        EXPECT(!parsed.arm[0].segments_measured && !parsed.arm[1].segments_measured);
        EXPECT(!parsed.arm[0].forearm_measured && !parsed.arm[1].forearm_measured);
        EXPECT(parsed.arm[0].shoulder_observed_dimensions==0 && parsed.arm[1].shoulder_observed_dimensions==0);
        EXPECT(parsed.standing_eye_cm==(version<3?0:170) && parsed.seated_eye_cm==(version<3?0:116));
        EXPECT(parsed.legacy_eye_cm==(version<3?170:0));
        Profile again; EXPECT(Parse(Serialize(parsed),&again) && Serialize(again)==Serialize(parsed));
        EXPECT(again.arm[0].fit.radial_rms_cm==.4);
    }
    const auto before=Serialize(parsed);
    for(const auto& malformed:{text+"unexpected=1\n",text+"left_upper=999\n",text.substr(0,text.find("right_")),std::string(9000,'x')}) {
        EXPECT(!Parse(malformed,&parsed)); EXPECT(Serialize(parsed)==before);
    }
    for(const std::string field:{"left_upper","left_verification_error_cm","left_segments_measured","left_forearm_measured","left_shoulder_observed_dimensions","standing_eye_cm"}) {
        auto corrupt=text; const auto at=corrupt.find(field+"=");
        corrupt.replace(at,corrupt.find('\n',at)-at,field+"=nan");
        EXPECT(!Parse(corrupt,&parsed) && Serialize(parsed)==before);
    }
    // A self-contradictory v6 record must not activate a trusted-ratio branch
    // merely by toggling one provenance flag while retaining valid geometry.
    for(int contradiction=0;contradiction<3;++contradiction) {
        auto corrupt=text;
        const auto replace=[&](const std::string& field,const char* value) {
            const auto at=corrupt.find(field+"=");
            corrupt.replace(at,corrupt.find('\n',at)-at,field+"="+value);
        };
        if(contradiction==0) replace("left_shoulder_measured","1"); // dimensions remain2
        if(contradiction==1) replace("left_segments_measured","1"); // upper pivot remains partial
        if(contradiction==2) replace("left_wrist_measured","0"); // independent forearm needs its frame
        EXPECT(!Parse(corrupt,&parsed) && Serialize(parsed)==before);
    }
    EXPECT(!Parse(text,nullptr));
    Run fallback; fallback.Full(false); const auto* fp=fallback.session.GetProfile(); EXPECT(fp);
    if(fp) { Profile read; EXPECT(Parse(Serialize(*fp),&read)); EXPECT(!read.arm[0].segments_measured && !read.arm[1].segments_measured); }
    for(double scale:{.65,1.,1.4}) for(double yaw:{-147.,0.,71.}) for(int side=0;side<2;++side) {
        FVector shoulder{9,8,7}; double upper=9,lower=8; const FVector head{200,-90,170}; const auto q=YawOnly(yaw);
        EXPECT(MapArmTarget(*p,side,head,q,scale,&shoulder,&upper,&lower));
        EXPECT(Distance(shoulder,head+q.Rotate(p->arm[side].shoulder*scale))<1e-8);
        EXPECT(Near(upper,p->arm[side].upper_cm*scale,1e-8) && Near(lower,p->arm[side].forearm_cm*scale,1e-8));
        const FVector raw{31,-19,80},offset{4,3,-2}; FVector wrist;
        EXPECT(MapWristTarget(raw,q.Rotate(offset*scale),scale,&wrist));
        EXPECT(Distance(wrist,raw+q.Rotate(offset))<1e-8);
    }
    FVector unchanged{9,8,7}; double upper=9,lower=8;
    EXPECT(!MapArmTarget(*p,2,{},FQuat{},1,&unchanged,&upper,&lower));
    EXPECT(Distance(unchanged,{9,8,7})<1e-8 && upper==9 && lower==8);
    EXPECT(!MapWristTarget({}, {},0,&unchanged) && Distance(unchanged,{9,8,7})<1e-8);
}

static void HeightPoliciesAndMerge() {
    EXPECT(DefaultHeightPosture(HeightMode::Fixed)==HeightPosture::Unspecified);
    EXPECT(DefaultHeightPosture(HeightMode::Real)==HeightPosture::Standing);
    EXPECT(DefaultHeightPosture(HeightMode::Seated)==HeightPosture::Seated);
    Run known; known.Full(); const auto* previous=known.session.GetProfile();
    EXPECT(previous); if(!previous)return;

    Run seated; seated.height_mode=HeightMode::Seated;
    seated.height_posture=DefaultHeightPosture(seated.height_mode); seated.rig.head.Z=116.48;
    seated.hand_valid=false;
    auto sample=seated.Make({},{}); sample.skip_arms=true; seated.session.Update(sample); seated.Height();
    const auto* sitting=seated.session.GetProfile(); EXPECT(sitting); if(!sitting)return;
    EXPECT(seated.session.GetStatus().height_posture==HeightPosture::Seated);
    EXPECT(seated.session.GetStatus().height_measured && Near(seated.session.GetStatus().measured_eye_cm,116.48));
    EXPECT(seated.session.GetStatus().standing_eye_cm==0);
    EXPECT(sitting->standing_eye_cm==0 && Near(sitting->seated_eye_cm,116.48) && !sitting->arms_measured);
    Profile merged;
    EXPECT(MergeProfile(previous,*sitting,&merged));
    EXPECT(merged.arms_measured && Near(merged.standing_eye_cm,170) && Near(merged.seated_eye_cm,116.48));
    EXPECT(Near(merged.arm[0].upper_cm,previous->arm[0].upper_cm));
    EXPECT(Distance(merged.arm[1].shoulder,previous->arm[1].shoulder)<1e-8);

    Run seated_arms; seated_arms.height_mode=HeightMode::Seated;
    seated_arms.height_posture=HeightPosture::Seated; seated_arms.rig.head.Z=116.48; seated_arms.Full();
    EXPECT(seated_arms.session.GetProfile() && seated_arms.session.GetProfile()->arms_measured);
    if(const auto* fit=seated_arms.session.GetProfile()) {
        EXPECT(fit->standing_eye_cm==0 && Near(fit->seated_eye_cm,116.48));
        EXPECT(Near(fit->arm[1].forearm_cm,seated_arms.rig.lower[1]));
    }

    Run fixed; fixed.height_mode=HeightMode::Fixed; fixed.height_posture=HeightPosture::Unspecified;
    fixed.rig.head.Z=116.48; fixed.Full(); const auto* fixed_fit=fixed.session.GetProfile();
    EXPECT(fixed_fit && fixed_fit->arms_measured);
    if(fixed_fit) {
        EXPECT(fixed_fit->standing_eye_cm==0 && fixed_fit->seated_eye_cm==0);
        EXPECT(!fixed.session.GetStatus().height_measured && fixed.session.GetStatus().measured_eye_cm==0);
        EXPECT(MergeProfile(&merged,*fixed_fit,&merged));
        EXPECT(Near(merged.standing_eye_cm,170) && Near(merged.seated_eye_cm,116.48));
    }
    Run opted_in; opted_in.height_mode=HeightMode::Fixed; opted_in.height_posture=HeightPosture::Standing;
    opted_in.rig.head.Z=176; sample=opted_in.Make({},{}); sample.skip_arms=true;
    opted_in.session.Update(sample); opted_in.Height();
    const auto* standing=opted_in.session.GetProfile(); EXPECT(standing);
    if(standing) {
        EXPECT(Near(standing->standing_eye_cm,176) && standing->seated_eye_cm==0);
        EXPECT(MergeProfile(&merged,*standing,&merged));
        EXPECT(Near(merged.standing_eye_cm,176) && Near(merged.seated_eye_cm,116.48));
        EXPECT(merged.arms_measured);
    }

    for(bool floor_known:{false,true}) {
        Run keep; keep.height_mode=floor_known?HeightMode::Fixed:HeightMode::Seated;
        keep.height_posture=DefaultHeightPosture(keep.height_mode); keep.floor_known=floor_known;
        keep.rig.head.Z=116.48; sample=keep.Make({},{}); sample.skip_arms=true; keep.session.Update(sample); keep.Height();
        const auto* no_op=keep.session.GetProfile(); EXPECT(no_op);
        EXPECT(!keep.session.GetStatus().height_measured);
        if(no_op) {
            EXPECT(Serialize(*no_op).empty());
            const auto before=Serialize(merged);
            EXPECT(MergeProfile(&merged,*no_op,&merged)); EXPECT(Serialize(merged)==before);
        }
    }
    for(const auto mode:{HeightMode::Fixed,HeightMode::Real,HeightMode::Seated}) {
        Run changed; changed.height_mode=mode; changed.height_posture=DefaultHeightPosture(mode); changed.Height();
        changed.height_mode=mode==HeightMode::Fixed?HeightMode::Real:HeightMode::Fixed;
        changed.height_posture=DefaultHeightPosture(changed.height_mode);
        changed.Feed({}, {},1);
        EXPECT(changed.session.GetStatus().phase==Phase::Failed);
        EXPECT(changed.session.GetStatus().reason==Reason::HeightModeChanged && !changed.session.GetProfile());
    }
    Run posture; posture.Height(); posture.height_posture=HeightPosture::Seated; posture.Feed({}, {},1);
    EXPECT(posture.session.GetStatus().reason==Reason::HeightModeChanged && !posture.session.GetProfile());
    Run bad_mode; sample=bad_mode.Make({},{}); sample.height_mode=static_cast<HeightMode>(99);
    bad_mode.session.Update(sample); EXPECT(bad_mode.session.GetStatus().reason==Reason::InvalidSample);
    Profile only_seated;
    EXPECT(MergeProfile(nullptr,*sitting,&only_seated)); EXPECT(!only_seated.arms_measured);
    const auto before=Serialize(merged);
    auto invalid=*sitting; invalid.seated_eye_cm=999;
    EXPECT(!MergeProfile(&merged,invalid,&merged) && Serialize(merged)==before);
    EXPECT(!MergeProfile(&invalid,*sitting,&merged) && Serialize(merged)==before);
    EXPECT(!MergeProfile(previous,*sitting,nullptr));
    Profile reread; EXPECT(Parse(Serialize(merged),&reread));
    EXPECT(Near(reread.standing_eye_cm,176) && Near(reread.seated_eye_cm,116.48));
}

static void ExplicitConfirmationOwnsReadiness() {
    Run r;
    r.Feed({0,-20,-60},{0,20,-60},8);
    EXPECT(r.session.GetStatus().phase==Phase::MeasureHeight);
    EXPECT(!r.session.GetStatus().pose_confirmed && r.session.GetStatus().hold_fraction==0);
    r.Height(); EXPECT(r.session.GetStatus().phase==Phase::ArmsForward);
    // Waiting/moving through any comfortable geometry cannot masquerade as a
    // declaration of readiness. There are no hardcoded XYZ rejection cards.
    for(const auto p:{FVector{0,-20,-60},FVector{55,-20,-100},FVector{55,-20,25}}) {
        r.Feed(p,{p.X,-p.Y,p.Z},1);
        EXPECT(r.session.GetStatus().phase==Phase::ArmsForward);
        EXPECT(!r.session.GetStatus().pose_confirmed && r.session.GetStatus().hold_fraction==0);
        EXPECT(r.session.GetStatus().reason!=Reason::HandsTooLow && r.session.GetStatus().reason!=Reason::ArmDirectionMismatch);
    }
    r.Forward(); EXPECT(r.session.GetStatus().phase==Phase::ArmsOut);
    r.Feed(r.rig.Out(0),r.rig.Out(1),5);
    EXPECT(r.session.GetStatus().phase==Phase::ArmsOut && !r.session.GetStatus().pose_confirmed);
    r.Out(); EXPECT(r.session.GetStatus().phase==Phase::BendElbows);
    r.Feed(r.rig.Bent(0,kPi/2),r.rig.Bent(1,kPi/2),5,kPi/2);
    EXPECT(r.session.GetStatus().phase==Phase::BendElbows && !r.session.GetProfile());
    r.Bend(); EXPECT(r.session.GetStatus().phase==Phase::VerifyForward);
    r.Feed(r.rig.Forward(0),r.rig.Forward(1),5);
    EXPECT(r.session.GetStatus().phase==Phase::VerifyForward && !r.session.GetProfile());
    r.Verify(); EXPECT(r.session.GetStatus().phase==Phase::Complete);
    // One confirmation starts one window. Replaying an identical timestamp
    // (including its true edge) cannot complete that window or a later stage.
    Run replay; auto sample=replay.Make({},{}); sample.confirm_pose=true;
    replay.session.Update(sample);
    for(int i=0;i<500;++i) replay.session.Update(sample);
    EXPECT(replay.session.GetStatus().phase==Phase::MeasureHeight && !replay.session.GetProfile());
    replay.Feed({}, {},3);
    EXPECT(replay.session.GetStatus().phase==Phase::ArmsForward);
    replay.Feed(replay.rig.Forward(0),replay.rig.Forward(1),5);
    EXPECT(replay.session.GetStatus().phase==Phase::ArmsForward && !replay.session.GetStatus().pose_confirmed);
}

static void ComfortableOpeningUsesFittedSpan() {
    auto at_angle=[](const Run& r,int side,double angle) {
        const double a=angle*kDegToRad,reach=r.rig.upper[side]+r.rig.lower[side];
        return r.rig.shoulder[side]+FVector{reach*std::cos(a),(side?1.:-1.)*reach*std::sin(a),0};
    };
    for(double duration:{0.0,25.0}) {
        Run r;
        r.rig.upper[0]=22; r.rig.lower[0]=18; // 40 cm total
        r.rig.upper[1]=48; r.rig.lower[1]=37; // 85 cm total
        r.Height(); r.Forward();
        const double begin=r.time;
        while(r.time-begin<duration && r.session.GetStatus().phase==Phase::ArmsOut) {
            const double angle=45*(r.time-begin)/duration;
            r.Step(at_angle(r,0,angle),at_angle(r,1,angle));
            if(angle<30) EXPECT(r.session.GetStatus().phase==Phase::ArmsOut);
        }
        r.Confirm(at_angle(r,0,45),at_angle(r,1,45));
        r.Feed(at_angle(r,0,45),at_angle(r,1,45),5);
        EXPECT(r.session.GetStatus().phase==Phase::BendElbows);
        r.Bend(); r.Verify();
        EXPECT(r.session.GetStatus().phase==Phase::Complete); CheckGeometry(r);
    }
    Run pause; pause.rig.upper[0]=pause.rig.upper[1]=45;
    pause.rig.lower[0]=pause.rig.lower[1]=40;
    pause.Height(); pause.Forward();
    // A small opening may provide only a partial pivot. It still cannot be
    // accepted before the user confirms, regardless of elapsed time.
    pause.Feed(at_angle(pause,0,10),at_angle(pause,1,10),8);
    EXPECT(pause.session.GetStatus().phase==Phase::ArmsOut);
    EXPECT(!pause.session.GetStatus().pose_confirmed && !pause.session.GetProfile());
    pause.Confirm(at_angle(pause,0,45),at_angle(pause,1,65));
    pause.Feed(at_angle(pause,0,45),at_angle(pause,1,65),5);
    EXPECT(pause.session.GetStatus().phase==Phase::BendElbows);
    pause.Bend(); pause.Verify();
    EXPECT(pause.session.GetStatus().phase==Phase::Complete); CheckGeometry(pause);
}

static void ConsistentReachWithSmallWristVariation() {
    // Real recordings had near-identical radial reach in both straight poses
    // but a 5-6 degree change of the controller-local forearm direction.
    // That does not identify a new shoulder. It can support conditional reach
    // if the unchanged wrist-quality and independent verification gates pass.
    Run r; r.Height(); r.Forward();
    r.Confirm(r.rig.Out(0),r.rig.Out(1));
    const double finish=r.time+5;
    while(r.time<finish && r.session.GetStatus().phase==Phase::ArmsOut) {
        auto sample=r.Make(r.rig.Out(0),r.rig.Out(1));
        for(int s=0;s<2;++s) sample.grip_rotation[s]=sample.grip_rotation[s]*ik::AxisAngle({0,1,0},6*kDegToRad);
        r.session.Update(sample);
    }
    EXPECT(r.session.GetStatus().phase==Phase::BendElbows);
    EXPECT(r.session.GetStatus().reach_degraded && !r.session.GetProfile());
    r.Bend(); r.Verify(); EXPECT(r.session.GetStatus().phase==Phase::Complete);
    if(const auto* p=r.session.GetProfile()) {
        for(int s=0;s<2;++s) {
            EXPECT(!p->arm[s].shoulder_measured);
            EXPECT(Distance(p->arm[s].shoulder,r.rig.shoulder[s])<1e-6);
            // A small learned frame difference affects the independently fitted
            // elbow, so this is a sub-cm consistency bound, not exact anatomy.
            EXPECT(Near(p->arm[s].upper_cm+p->arm[s].forearm_cm,r.rig.upper[s]+r.rig.lower[s],.6));
            EXPECT(p->arm[s].wrist.measured && p->arm[s].wrist.forearm_rms_deg<=8);
        }
    }
    // Consistent length cannot excuse a grossly contradictory wrist frame.
    Run bad; bad.Height(); bad.Forward(); const double end=bad.time+6;
    bad.Confirm(bad.rig.Out(0),bad.rig.Out(1));
    while(bad.time<end) {
        auto sample=bad.Make(bad.rig.Out(0),bad.rig.Out(1));
        for(int s=0;s<2;++s) sample.grip_rotation[s]=sample.grip_rotation[s]*ik::AxisAngle({0,1,0},50*kDegToRad);
        bad.session.Update(sample);
    }
    EXPECT(bad.session.GetStatus().phase==Phase::ArmsForward);
    EXPECT(bad.session.GetStatus().reason==Reason::WristPoseMismatch && !bad.session.GetProfile());
}
static void BadForwardCanBeReplacedWithoutRepeatingHeight() {
    Run r; r.Height(); const double begin=r.time;
    bool confirmed=false;
    while(r.time-begin<3 && r.session.GetStatus().phase==Phase::ArmsForward) {
        auto sample=r.Make(r.rig.Forward(0),r.rig.Forward(1));
        for(int s=0;s<2;++s) sample.grip_rotation[s]=sample.grip_rotation[s]*ik::AxisAngle({1,0,0},kPi/2);
        sample.confirm_pose=!confirmed; confirmed=true; r.session.Update(sample);
    }
    EXPECT(r.session.GetStatus().phase==Phase::ArmsOut);
    r.Out();
    EXPECT(r.session.GetStatus().phase==Phase::ArmsForward && !r.session.GetStatus().pose_confirmed);
    EXPECT(r.session.GetStatus().attempt==2 && r.session.GetStatus().height_measured);
    EXPECT(r.session.GetStatus().reason==Reason::WristPoseMismatch && !r.session.GetProfile());
    r.Feed(r.rig.Forward(0),r.rig.Forward(1),3);
    EXPECT(r.session.GetStatus().phase==Phase::ArmsForward); // no hidden retry confirmation
    r.Forward(); r.Out(); r.Bend(); r.Verify();
    EXPECT(r.session.GetStatus().phase==Phase::Complete); CheckGeometry(r);
}

static void HeldCaptureReleaseAndAcceptance() {
    Run r;
    auto sample=r.Make({},{}); sample.confirm_pose=true; sample.capture_held=true;
    EXPECT(r.session.Update(sample).pose_confirmed);
    r.Feed({},{},1.4);
    EXPECT(r.session.GetStatus().phase==Phase::MeasureHeight && !r.session.GetStatus().capture_accepted);
    // Release is authoritative even when the pose timestamp is unchanged.
    sample.seconds=r.time; sample.confirm_pose=false; sample.capture_held=false;
    auto status=r.session.Update(sample);
    EXPECT(!status.pose_confirmed && status.hold_fraction==0 && !status.capture_accepted);
    const double end=r.time+3;
    while(r.time<end) {
        sample=r.Make({},{}); sample.capture_held=false;
        status=r.session.Update(sample);
        EXPECT(status.phase==Phase::MeasureHeight && !status.capture_accepted);
    }
    // A start edge with no held level cannot begin a hidden recording.
    sample=r.Make({},{}); sample.confirm_pose=true; sample.capture_held=false;
    EXPECT(!r.session.Update(sample).pose_confirmed);
    sample=r.Make({},{}); sample.confirm_pose=true; sample.capture_held=true;
    const double began=sample.seconds;
    status=r.session.Update(sample);
    unsigned accepted=0;
    while(status.phase==Phase::MeasureHeight && r.time-began<3) {
        sample=r.Make({},{}); sample.capture_held=true;
        status=r.session.Update(sample); accepted+=status.capture_accepted;
        if(r.time-began<2) EXPECT(status.phase==Phase::MeasureHeight && !status.capture_accepted);
    }
    EXPECT(status.phase==Phase::ArmsForward && accepted==1 && status.capture_accepted);
    EXPECT(r.time-began>=2 && status.height_measured);
    // Even a duplicate Update retires the completion edge. Holding through
    // a phase transition does not confirm the next pose.
    status=r.session.Update(sample);
    EXPECT(!status.capture_accepted && !status.pose_confirmed);
    r.Feed(r.rig.Forward(0),r.rig.Forward(1),3);
    EXPECT(r.session.GetStatus().phase==Phase::ArmsForward && !r.session.GetStatus().capture_accepted);
    sample=r.Make(r.rig.Forward(0),r.rig.Forward(1)); sample.skip_arms=true;
    status=r.session.Update(sample);
    EXPECT(status.phase==Phase::Complete && !status.capture_accepted); // skip is not a capture
    EXPECT(!r.session.Update(sample).capture_accepted);

    // Cancelling a forward window must not discard an accepted height.
    Run arm; arm.Height(); arm.Confirm(arm.rig.Forward(0),arm.rig.Forward(1));
    arm.Feed(arm.rig.Forward(0),arm.rig.Forward(1),1.6);
    sample=arm.Make(arm.rig.Forward(0),arm.rig.Forward(1)); sample.capture_held=false;
    status=arm.session.Update(sample);
    EXPECT(status.phase==Phase::ArmsForward && status.height_measured && !status.pose_confirmed);
    arm.Forward(); EXPECT(arm.session.GetStatus().phase==Phase::ArmsOut);
    arm.Out(); arm.Bend(); arm.Verify(); EXPECT(arm.session.GetProfile());

    // Fit rejection does not emit success.
    Run wrong; wrong.Height(); wrong.Forward();
    wrong.Confirm(wrong.rig.Out(0),wrong.rig.Out(1));
    const double deadline=wrong.time+4;
    while(wrong.session.GetStatus().phase==Phase::ArmsOut && wrong.time<deadline) {
        sample=wrong.Make(wrong.rig.Out(0),wrong.rig.Out(1));
        for(int s=0;s<2;++s) sample.grip_rotation[s]=sample.grip_rotation[s]*ik::AxisAngle({1,0,0},kPi/2);
        status=wrong.session.Update(sample); EXPECT(!status.capture_accepted);
    }
    EXPECT(status.phase==Phase::ArmsForward && !status.capture_accepted);

    Run verify; verify.Height(); verify.Forward(); verify.Out(); verify.Bend();
    verify.Confirm(verify.rig.Forward(0)+FVector{20,0,0},verify.rig.Forward(1)+FVector{20,0,0});
    const double verification_deadline=verify.time+3;
    while(verify.time<verification_deadline) {
        status=verify.Step(verify.rig.Forward(0)+FVector{20,0,0},verify.rig.Forward(1)+FVector{20,0,0});
        EXPECT(!status.capture_accepted);
    }
    EXPECT(status.phase==Phase::VerifyForward && status.reason==Reason::VerificationMismatch && !status.pose_confirmed);
}

static void ImplausibleSplitIsNotUsed() {
    // 2026-11-06 owner profile: the left elbow fit read 20.8 cm of a 59.4 cm
    // arm (35%), the right arm kept the avatar's 47%. Human arms are ~41-47%.
    EXPECT(!PlausibleSplit(38.619820529954083,20.822743817518582));
    EXPECT(PlausibleSplit(33.110188379277069,29.438200558590854));
    // Every proportion the other tests use stays a human arm.
    EXPECT(PlausibleSplit(31,28) && PlausibleSplit(35,30) && PlausibleSplit(22,18) && PlausibleSplit(48,37) && PlausibleSplit(45,40));
    EXPECT(!PlausibleSplit(0,20) && !PlausibleSplit(30,-1) && !PlausibleSplit(std::nan(""),20) && !PlausibleSplit(30,40));
    Arm owner_left; owner_left.upper_cm=38.619820529954083; owner_left.forearm_cm=20.822743817518582;
    double upper=0,forearm=0;
    EXPECT(!UsableSegments(owner_left,&upper,&forearm));
    EXPECT(Near(upper+forearm,owner_left.upper_cm+owner_left.forearm_cm,1e-9));
    EXPECT(Near(forearm/(upper+forearm),kDefaultForearmShare,1e-12) && forearm>27.9 && forearm<28);
    Arm normal; normal.upper_cm=33.11; normal.forearm_cm=29.44;
    EXPECT(UsableSegments(normal,&upper,&forearm) && upper==33.11 && forearm==29.44);

    // A saved profile with that split draws the arm with the usual split on
    // the same reach; a plausible arm is untouched. The file is not changed.
    Run saved; saved.Full(); const auto* sp=saved.session.GetProfile(); EXPECT(sp);
    if(sp) {
        Profile profile=*sp; profile.arm[0].upper_cm=38.619820529954083; profile.arm[0].forearm_cm=20.822743817518582;
        EXPECT(Validate(profile));
        const std::string text=Serialize(profile); Profile parsed; EXPECT(Parse(text,&parsed));
        EXPECT(parsed.arm[0].upper_cm==profile.arm[0].upper_cm && parsed.arm[0].forearm_cm==profile.arm[0].forearm_cm);
        for(double scale:{.8,1.,1.3}) {
            FVector shoulder; double u=0,f=0; const FVector head{200,-90,170}; const auto q=YawOnly(31);
            EXPECT(MapArmTarget(parsed,0,head,q,scale,&shoulder,&u,&f));
            EXPECT(Near(u+f,(38.619820529954083+20.822743817518582)*scale,1e-8) && Near(f/(u+f),kDefaultForearmShare,1e-12));
            EXPECT(MapArmTarget(parsed,1,head,q,scale,&shoulder,&u,&f));
            EXPECT(Near(u,parsed.arm[1].upper_cm*scale,1e-8) && Near(f,parsed.arm[1].forearm_cm*scale,1e-8));
        }
    }

    // A new setup no longer saves such a bend fit: that side keeps its
    // straight-arm reach with the prior split; the other side's fit stands.
    Run odd; odd.rig.upper[0]=40; odd.rig.lower[0]=20; // 33%: what a moving elbow reads
    odd.Full(); EXPECT(odd.session.GetStatus().phase==Phase::Complete);
    if(const auto* p=odd.session.GetProfile()) {
        EXPECT(!p->arm[0].forearm_measured && !p->arm[0].segments_measured);
        EXPECT(odd.session.GetStatus().bend_reason[0]==Reason::UncertainBend);
        EXPECT(Near(p->arm[0].upper_cm+p->arm[0].forearm_cm,60,.15) && Near(p->arm[0].forearm_cm/(p->arm[0].upper_cm+p->arm[0].forearm_cm),odd.prior[0],1e-9));
        EXPECT(PlausibleSplit(p->arm[0].upper_cm,p->arm[0].forearm_cm));
        EXPECT(p->arm[1].forearm_measured && Near(p->arm[1].forearm_cm,odd.rig.lower[1],.15));
    } else EXPECT(false);
}

int main() {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    ImplausibleSplitIsNotUsed();
    StagesAndRates(); MotionAndOutliers(); WristWindowsAndFrozenMapping(); ReachObservabilityAndComfort(); TrackingPausesAndTransactions();
    WrongPriorsAndPerSideEvidence();
    OneBendSurvivesALongComfortablePause();
    BendEvidenceAndIndependentCheck(); HeightPoliciesAndMerge(); PersistenceAndMapping(); ExplicitConfirmationOwnsReadiness();
    ComfortableOpeningUsesFittedSpan();
    ConsistentReachWithSmallWristVariation();
    BadForwardCanBeReplacedWithoutRepeatingHeight();
    HeldCaptureReleaseAndAcceptance();
    std::printf("body_calibration: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
