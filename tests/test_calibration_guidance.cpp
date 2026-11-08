#include "vr/body/calibration_guidance.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
using namespace holevr::vr::calibration_guidance;
using namespace holevr::vr::body_calibration;
namespace {
int checks=0,failures=0;
#define EXPECT(c) do { ++checks; if(!(c)) { std::printf("FAIL line %d: %s\n",__LINE__,#c); ++failures; } } while(0)
std::string Text(const Guide& g) { return std::string(g.title)+"\n"+g.line[0]+"\n"+g.line[1]+"\n"+g.line[2]; }
bool Has(const Guide& g,const char* phrase) { return Text(g).find(phrase)!=std::string::npos; }
Status At(Phase p) { Status s; s.phase=p; return s; }
void Bounded(const Guide& g) {
    EXPECT(g.valid && g.title[0]);
    for(const auto& line:g.line) EXPECT(line[0] && std::strlen(line)<=46);
    EXPECT(g.progress>=0 && g.progress<=100);
    if(g.action!=Action::Capture && g.action!=Action::Complete) EXPECT(g.progress==0);
    for(const char* phrase:{"Hold still","hold still","motionless","Lower forearms","raise again",
                           "Repeat elbow","overhead","shoulder height","shoulder level","arms level"}) EXPECT(!Has(g,phrase));
    if(g.action==Action::Paused||g.action==Action::Failed) EXPECT(g.illustration==Illustration::None);
}
void UserControlsCapture() {
    for(Phase p:{Phase::MeasureHeight,Phase::ArmsForward,Phase::ArmsOut,Phase::BendElbows,Phase::VerifyForward}) {
        auto s=At(p); s.pose_ready=true; s.bend_pose_bent=true; s.hold_fraction=.59;
        // Neither an inferred pose flag nor an elapsed timer authorizes capture.
        for(double elapsed:{0.,2.,10.,100.}) {
            s.stage_elapsed_seconds=elapsed; s.preparation_remaining_seconds=elapsed;
            const auto g=ForStatus(s); Bounded(g);
            EXPECT(g.action==Action::Position && g.progress==0 && Has(g,"Hold both index triggers"));
        }
        s.pose_confirmed=true;
        auto g=ForStatus(s); Bounded(g);
        EXPECT(g.action==Action::Capture && g.progress==50 && Has(g,"small moves are OK"));
        s.pose_confirmed=false;
        g=ForStatus(s); EXPECT(g.action==Action::Position && Has(g,"Hold both index triggers"));
    }
}
void PoseInstructions() {
    auto s=At(Phase::MeasureHeight);
    s.height_posture=HeightPosture::Seated; EXPECT(Has(ForStatus(s),"Sit comfortably"));
    s.height_posture=HeightPosture::Standing; EXPECT(Has(ForStatus(s),"Stand comfortably"));
    s.height_posture=HeightPosture::Unspecified; EXPECT(Has(ForStatus(s),"seated or standing"));
    for(Phase p:{Phase::ArmsForward,Phase::VerifyForward}) {
        s=At(p); const auto g=ForStatus(s);
        EXPECT(g.illustration==Illustration::Forward && Has(g,"comfy height") && Has(g,"Palms inward"));
        EXPECT(Has(g,"wrists in line with forearms"));
    }
    s=At(Phase::ArmsOut); auto g=ForStatus(s);
    EXPECT(g.illustration==Illustration::Out && Has(g,"comfortably to the sides") && Has(g,"Palms down"));
    s=At(Phase::BendElbows);
    for(bool inferred:{false,true}) {
        s.bend_pose_bent=inferred; s.pose_ready=inferred; g=ForStatus(s);
        EXPECT(g.illustration==Illustration::Raising && Has(g,"hands rise comfortably"));
        EXPECT(Has(g,"Keep elbows in place") && Has(g,"wrists in line"));
        EXPECT(!Has(g,"Palms inward") && !Has(g,"down") && Has(g,"Hold both index triggers for 2 seconds"));
    }
    s.pose_confirmed=true; g=ForStatus(s);
    EXPECT(g.illustration==Illustration::Bent && Has(g,"comfortable bent pose") && !Has(g,"hands rise"));
}
void RecoveryAndResults() {
    for(Phase p:{Phase::MeasureHeight,Phase::ArmsForward,Phase::ArmsOut,Phase::BendElbows,Phase::VerifyForward}) {
        auto s=At(p); s.pose_confirmed=s.pose_ready=true; s.hold_fraction=1;
        s.reason=Reason::TrackingLost; auto g=ForStatus(s);
        EXPECT(g.action==Action::Paused && Has(g,"Wait for tracking") && g.progress==0);
        s.reason=Reason::LookForward; g=ForStatus(s);
        EXPECT(g.action==Action::Position && Has(g,"face forward") && g.progress==0);
    }
    for(Reason r:{Reason::BadArcFit,Reason::WristPoseMismatch}) {
        auto s=At(Phase::ArmsOut); s.reason=r; s.pose_confirmed=true; s.hold_fraction=1;
        const auto g=ForStatus(s);
        EXPECT(g.action==Action::Position && Has(g,"Palms down") && Has(g,"Hold both triggers again") && Has(g,"X: keep"));
        EXPECT(!Has(g,"Palms inward"));
    }
    auto s=At(Phase::VerifyForward); s.reason=Reason::VerificationMismatch;
    auto g=ForStatus(s); EXPECT(Has(g,"earlier straight-arm reach") && Has(g,"Hold both index triggers"));
    for(bool shoulder:{false,true}) for(bool elbow:{false,true}) {
        s=At(Phase::Complete); s.arms_measured=true; s.reach_degraded=shoulder; s.bend_degraded=elbow;
        s.reason=Reason::TrackingLost; g=ForStatus(s); Bounded(g);
        EXPECT(g.action==Action::Complete && g.progress==100);
        EXPECT(Has(g,"shoulder directions remain estimated")==shoulder);
        EXPECT(Has(g,"arm proportions remain estimated")==elbow);
        EXPECT(!Has(g,"prior shoulder") && !Has(g,"measured your elbows"));
    }
    s=At(Phase::Complete); g=ForStatus(s); EXPECT(Has(g,"arm settings are unchanged"));
    s=At(Phase::Failed); g=ForStatus(s); EXPECT(Has(g,"saved fit is unchanged"));
}
void EveryStateFits() {
    for(Phase p:{Phase::MeasureHeight,Phase::ArmsForward,Phase::ArmsOut,Phase::BendElbows,Phase::VerifyForward,Phase::Complete,Phase::Failed})
        for(int r=0;r<=int(Reason::ArmDirectionMismatch);++r)
            for(bool confirm:{false,true}) for(bool ready:{false,true}) for(bool bent:{false,true}) {
                auto s=At(p); s.reason=Reason(r); s.pose_confirmed=confirm; s.pose_ready=ready; s.bend_pose_bent=bent;
                s.hold_fraction=.7; s.reach_degraded=s.bend_degraded=true; Bounded(ForStatus(s));
            }
    auto s=At(Phase::ArmsForward); s.pose_confirmed=true;
    for(double f:{-1.,0.,.99,1.,9.,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()}) {
        s.hold_fraction=f; const auto g=ForStatus(s); Bounded(g); if(!std::isfinite(f)) EXPECT(g.progress==0);
    }
    s.preparation_remaining_seconds=std::numeric_limits<double>::infinity(); EXPECT(ForStatus(s).action==Action::Paused);
    s.phase=Phase(999); EXPECT(!ForStatus(s).valid);
}
}
int main() {
    UserControlsCapture(); PoseInstructions(); RecoveryAndResults(); EveryStateFits();
    std::printf("calibration guidance: %s (%d checks, %d failures)\n",failures?"FAIL":"PASS",checks,failures);
    return failures?1:0;
}
