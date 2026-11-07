// clang++ -std=c++20 -Isrc tests/test_wrist_calibration.cpp
//   src/vr/body/wrist_calibration.cpp src/vr/body/ik_math.cpp -o test_wrist_calibration
#include "vr/body/wrist_calibration.h"
#include "vr/body/ik_math.h"
#include <algorithm>
#include <cstdio>
#include <limits>

using namespace holevr::vr;
using namespace holevr::vr::ik;
using namespace holevr::vr::wrist_calibration;
namespace {
int checks=0,failures=0;
#define EXPECT(c) do { ++checks; if(!(c)) { std::printf("FAIL line %d: %s\n",__LINE__,#c); ++failures; } } while(0)
bool Near(FVector a,FVector b,double e=1e-6) { return Length(a-b)<e; }
bool Same(FQuat a,FQuat b) { return Near(a.Rotate({1,0,0}),b.Rotate({1,0,0}))&&Near(a.Rotate({0,1,0}),b.Rotate({0,1,0})); }
const FVector local_forearm=Normalize(FVector{.23,.87,-.44});
const FVector local_palm=Normalize(Reject(FVector{-.4,-.3,-.84},local_forearm));
const FVector offset{1.4,-6.2,2.1};
std::vector<Observation> Observations(int side,int forward_count=9,int out_count=9,FQuat world={}) {
    std::vector<Observation> result;
    const double sign=side==0?-1:1;
    for(int stage=0;stage<2;++stage) {
        const FVector f=world.Rotate(stage?FVector{0,sign,0}:FVector{1,0,0});
        const FVector p=world.Rotate(stage?FVector{0,0,-1}:FVector{0,-sign,0});
        const FQuat q=AlignFrames(local_forearm,local_palm,f,p);
        for(int i=0;i<(stage?out_count:forward_count);++i) {
            const double quaternion_scale=i%2?-2.0:3.0;
            result.push_back({stage?Stage::Out:Stage::Forward,
                {q.X*quaternion_scale,q.Y*quaternion_scale,q.Z*quaternion_scale,q.W*quaternion_scale},f,p,true});
        }
    }
    return result;
}
void FitKnownBasis() {
    for(int side=0;side<2;++side) for(FQuat world:{FQuat{},ToQuat({13,137,-11})}) {
        auto samples=Observations(side,9,9,world);
        const auto a=Fit(samples,offset);
        EXPECT(a.reason==Reason::None&&Validate(a.calibration));
        EXPECT(Near(a.calibration.forearm_axis_grip,local_forearm));
        EXPECT(Near(a.calibration.palm_axis_grip,local_palm));
        EXPECT(Near(a.calibration.wrist_offset_cm,offset));
        EXPECT(a.calibration.forearm_rms_deg<1e-5&&a.calibration.palm_rms_deg<1e-5);
        // Overhead roll is comfortable motion, not a specified neutral pose.
        // Even a different wrist-flex pattern must not warp the held-pose fit.
        for(int i=0;i<100;++i)
            samples.push_back({Stage::Overhead,ToQuat({i*3.0,17.0,-31.0}),{0,0,1},{0,1,0},true});
        const auto b=Fit(samples,offset);
        EXPECT(Validate(b.calibration));
        EXPECT(Near(a.calibration.forearm_axis_grip,b.calibration.forearm_axis_grip));
        EXPECT(Near(a.calibration.palm_axis_grip,b.calibration.palm_axis_grip));
        const auto dense=Fit(Observations(side,200,3,world),offset);
        EXPECT(Validate(dense.calibration));
        EXPECT(Near(dense.calibration.forearm_axis_grip,a.calibration.forearm_axis_grip));
        EXPECT(Near(dense.calibration.palm_axis_grip,a.calibration.palm_axis_grip));
    }
}
void RejectInconsistentStages() {
    auto samples=Observations(0,200,3);
    for(auto& s:samples) if(s.stage==Stage::Out)
        s.raw_grip_rotation=AxisAngle(s.forearm_direction,30*kDegToRad)*Normalized(s.raw_grip_rotation);
    const auto palm=Fit(samples,offset);
    EXPECT(!palm.calibration.measured&&palm.reason==Reason::InconsistentPalm);
    samples=Observations(0,200,3);
    for(auto& s:samples) if(s.stage==Stage::Out)
        s.raw_grip_rotation=AxisAngle(s.palm_direction,30*kDegToRad)*Normalized(s.raw_grip_rotation);
    const auto forearm=Fit(samples,offset);
    EXPECT(!forearm.calibration.measured&&forearm.reason==Reason::InconsistentForearm);
    samples=Observations(0,200,2);
    EXPECT(Fit(samples,offset).reason==Reason::NotEnoughSamples);
    samples=Observations(0); samples[0].palm_known=false;
    EXPECT(Fit(samples,offset).reason==Reason::InvalidSample);
    samples=Observations(0); samples[0].raw_grip_rotation={0,0,0,0};
    EXPECT(Fit(samples,offset).reason==Reason::InvalidSample);
    samples=Observations(0);
    for(int i=0;i<3;++i) samples[i].palm_direction=samples[i].forearm_direction;
    EXPECT(Fit(samples,offset).reason==Reason::InconsistentPalm);
    EXPECT(Fit(Observations(0),{100,0,0}).reason==Reason::InvalidOffset);
    // Small measured hold variation is allowed and reported, not forced to
    // zero by assigning a different correction to each observation.
    samples=Observations(1);
    for(unsigned i=0;i<samples.size();++i) {
        auto& s=samples[i];
        s.raw_grip_rotation=AxisAngle(s.forearm_direction,(int(i%3)-1)*2*kDegToRad)*Normalized(s.raw_grip_rotation);
    }
    const auto noisy=Fit(samples,offset);
    EXPECT(Validate(noisy.calibration));
    EXPECT(noisy.calibration.palm_rms_deg>1&&noisy.calibration.palm_rms_deg<3);
    // Permitted arm tilt must not consume the wrist-roll error budget. The
    // geometric nonorthogonality gate is still inclusive and remains bounded.
    for(double tilt:{10.0,10.01,19.99,20.0,20.01}) {
        samples=Observations(0);
        for(auto& s:samples) if(s.stage==Stage::Out) {
            s.forearm_direction={0,-std::cos(tilt*kDegToRad),-std::sin(tilt*kDegToRad)};
            s.raw_grip_rotation=AlignFrames(local_forearm,local_palm,s.forearm_direction,s.palm_direction);
        }
        const auto boundary=Fit(samples,offset);
        EXPECT(tilt<=20 ? Validate(boundary.calibration) : boundary.reason==Reason::InconsistentPalm);
    }
    // Separately exercise the inclusive 10-degree per-stage roll residual:
    // two holds differing by20 degrees fit midway; any larger disagreement
    // fails instead of receiving a separate correction for each stage.
    for(double disagreement:{20.0,20.01}) {
        samples=Observations(0);
        for(auto& s:samples) if(s.stage==Stage::Out)
            s.raw_grip_rotation=AxisAngle(s.forearm_direction,disagreement*kDegToRad)*Normalized(s.raw_grip_rotation);
        const auto boundary=Fit(samples,offset);
        EXPECT(disagreement==20 ? Validate(boundary.calibration) : boundary.reason==Reason::InconsistentPalm);
    }
}
void RuntimeMapping() {
    const Calibration c=Fit(Observations(0),offset).calibration;
    for(double scale:{.8,1.0,1.25}) for(double yaw:{0.0,137.0,-53.0}) for(double mirror:{-1.0,1.0}) {
        Mapping m;
        m.controller_world={123,-48,150}; m.grip_world=Normalized(YawOnly(yaw)*ToQuat({17,-23,31}));
        m.tracking_scale=scale;
        const FQuat arbitrary_bone=ToQuat({34,mirror*67,-29});
        m.forearm_axis_hand=arbitrary_bone.Rotate({1,0,0});
        m.palm_axis_hand=arbitrary_bone.Rotate({0,0,mirror});
        FVector wrist; FQuat hand;
        EXPECT(Map(c,m,&wrist,&hand));
        EXPECT(Near(wrist,m.controller_world+m.grip_world.Rotate(offset*scale)));
        EXPECT(Near(hand.Rotate(m.forearm_axis_hand),m.grip_world.Rotate(local_forearm)));
        EXPECT(Near(hand.Rotate(m.palm_axis_hand),m.grip_world.Rotate(local_palm)));
        const FVector before=wrist;
        m.rotation_delta_grip=ToQuat({23,-31,17});
        EXPECT(Map(c,m,&wrist,&hand));
        EXPECT(Near(wrist,before)); // orientation correction cannot invalidate arm-position samples
        EXPECT(Near(hand.Rotate(m.forearm_axis_hand),(m.grip_world*m.rotation_delta_grip).Rotate(local_forearm)));
        EXPECT(Near(hand.Rotate(m.palm_axis_hand),(m.grip_world*m.rotation_delta_grip).Rotate(local_palm)));
        m.position_delta_cm={.4,-.2,.3};
        EXPECT(Map(c,m,&wrist,&hand));
        EXPECT(Near(wrist,before+m.grip_world.Rotate(m.position_delta_cm*scale)));
    }
}
void InvalidMappingIsTransactional() {
    Calibration c=Fit(Observations(0),offset).calibration;
    Mapping m; m.forearm_axis_hand={1,0,0}; m.palm_axis_hand={0,0,-1};
    const FVector untouched{7,8,9}; const FQuat rotation=ToQuat({11,22,33});
    auto fails=[&](Calibration bad,Mapping input) {
        FVector p=untouched; FQuat q=rotation;
        EXPECT(!Map(bad,input,&p,&q)); EXPECT(Near(p,untouched)&&Same(q,rotation));
    };
    auto bad=c; bad.measured=false; fails(bad,m); // legacy profile stays on caller's existing path
    bad=c; bad.forearm_axis_grip=local_forearm*2; fails(bad,m);
    bad=c; bad.palm_axis_grip=local_forearm; fails(bad,m);
    const double nan=std::numeric_limits<double>::quiet_NaN();
    bad=c; bad.wrist_offset_cm.X=nan; fails(bad,m);
    auto input=m; input.grip_world={0,0,0,0}; fails(c,input);
    input=m; input.rotation_delta_grip={nan,0,0,1}; fails(c,input);
    input=m; input.palm_axis_hand=input.forearm_axis_hand; fails(c,input);
    input=m; input.tracking_scale=0; fails(c,input);
    input=m; input.position_delta_cm={nan,0,0}; fails(c,input);
    input=m; input.controller_world={nan,0,0}; fails(c,input);
}

void RepresentativeWindowsWithOneSpike() {
    // Physiological variation is spread across the whole window; one bad
    // valid orientation must not veto all of its consistent neighbours.
    for(int side=0;side<2;++side) for(int count:{7,9,14,90})
        for(FQuat world:{FQuat{},ToQuat({13,137,-11})}) {
            auto samples=Observations(side,count,count,world);
            for(int stage=0;stage<2;++stage) for(int n=0;n<count;++n) {
                auto& s=samples[stage*count+n];
                const double t=2*kPi*n/count;
                s.raw_grip_rotation=AxisAngle(s.forearm_direction,5*std::sin(t)*kDegToRad)*
                    AxisAngle(s.palm_direction,3*std::sin(2*t)*kDegToRad)*Normalized(s.raw_grip_rotation);
            }
            const auto clean=Fit(samples,offset);
            EXPECT(Validate(clean.calibration));
            for(int stage=0;stage<2;++stage) {
                auto& s=samples[stage*count]; // simultaneous isolated tracking spikes in both holds
                s.raw_grip_rotation=AxisAngle(Normalize(s.forearm_direction+s.palm_direction),85*kDegToRad)*
                    Normalized(s.raw_grip_rotation);
            }
            const auto robust=Fit(samples,offset);
            EXPECT(robust.reason==Reason::None && Validate(robust.calibration));
            if(!robust.calibration.measured) continue;
            EXPECT(AngleBetween(robust.calibration.forearm_axis_grip,clean.calibration.forearm_axis_grip)<1*kDegToRad);
            EXPECT(AngleBetween(robust.calibration.palm_axis_grip,clean.calibration.palm_axis_grip)<1*kDegToRad);
            EXPECT(robust.calibration.forearm_rms_deg>0 && robust.calibration.palm_rms_deg>0);
            EXPECT(robust.calibration.max_error_deg<8);
            EXPECT(Near(robust.calibration.wrist_offset_cm,offset));
        }
    // A finite positional spike can make an otherwise valid sample's stated
    // palm axis nonorthogonal. It counts against the same per-stage budget.
    auto samples=Observations(0);
    samples[0].palm_direction=samples[0].forearm_direction;
    EXPECT(Validate(Fit(samples,offset).calibration));
}

void OutlierBudgetCannotHideWrongStage() {
    for(int corrupt:{3,4}) {
        auto samples=Observations(0,200,20);
        for(int i=0;i<corrupt;++i) {
            auto& s=samples[200+i];
            s.raw_grip_rotation=AxisAngle(s.forearm_direction,80*kDegToRad)*Normalized(s.raw_grip_rotation);
        }
        const auto result=Fit(samples,offset);
        EXPECT(corrupt==3 ? Validate(result.calibration) : result.reason==Reason::InconsistentPalm);
    }
    // A tiny window cannot discard one of six observations under85% coverage.
    auto samples=Observations(0,200,6);
    auto& bad=samples.back();
    bad.raw_grip_rotation=AxisAngle(bad.forearm_direction,80*kDegToRad)*Normalized(bad.raw_grip_rotation);
    EXPECT(Fit(samples,offset).reason==Reason::InconsistentPalm);

    for(bool flex:{false,true}) {
        samples=Observations(0,200,20);
        for(auto& s:samples) if(s.stage==Stage::Out)
            s.raw_grip_rotation=AxisAngle(flex?s.palm_direction:s.forearm_direction,30*kDegToRad)*Normalized(s.raw_grip_rotation);
        const auto result=Fit(samples,offset);
        EXPECT(!result.calibration.measured);
        EXPECT(result.reason==(flex?Reason::InconsistentForearm:Reason::InconsistentPalm));
    }
    // Equal stage weight survives different sampling rates and spike removal.
    Calibration result[2];
    for(int run=0;run<2;++run) {
        samples=Observations(1,run?200:7,7);
        for(auto& s:samples) if(s.stage==Stage::Out)
            s.raw_grip_rotation=AxisAngle(s.forearm_direction,6*kDegToRad)*Normalized(s.raw_grip_rotation);
        samples[0].raw_grip_rotation=AxisAngle(samples[0].forearm_direction,80*kDegToRad)*Normalized(samples[0].raw_grip_rotation);
        auto& spike=samples.back();
        spike.raw_grip_rotation=AxisAngle(spike.forearm_direction,80*kDegToRad)*Normalized(spike.raw_grip_rotation);
        result[run]=Fit(samples,offset).calibration;
        EXPECT(Validate(result[run]));
    }
    EXPECT(Near(result[0].forearm_axis_grip,result[1].forearm_axis_grip));
    EXPECT(Near(result[0].palm_axis_grip,result[1].palm_axis_grip));

    // The robust centre must not depend on an arbitrary device-coordinate
    // convention. Re-express the same noisy measurements in another frame.
    samples=Observations(0,21,21);
    for(std::size_t i=0;i<samples.size();++i) {
        auto& s=samples[i];
        s.raw_grip_rotation=AxisAngle(s.forearm_direction,4*std::sin(double(i))*kDegToRad)*Normalized(s.raw_grip_rotation);
    }
    samples[0].raw_grip_rotation=AxisAngle(samples[0].forearm_direction,90*kDegToRad)*Normalized(samples[0].raw_grip_rotation);
    const auto original=Fit(samples,offset);
    const FQuat reframe=ToQuat({31,-79,121});
    for(auto& s:samples) s.raw_grip_rotation=Normalized(s.raw_grip_rotation)*reframe.Inverse();
    std::reverse(samples.begin(),samples.end());
    const auto transformed=Fit(samples,reframe.Rotate(offset));
    EXPECT(Validate(original.calibration) && Validate(transformed.calibration));
    EXPECT(Near(transformed.calibration.forearm_axis_grip,reframe.Rotate(original.calibration.forearm_axis_grip)));
    EXPECT(Near(transformed.calibration.palm_axis_grip,reframe.Rotate(original.calibration.palm_axis_grip)));
    EXPECT(std::fabs(transformed.calibration.max_error_deg-original.calibration.max_error_deg)<1e-6);
}
}
int main() {
    FitKnownBasis(); RejectInconsistentStages(); RuntimeMapping(); InvalidMappingIsTransactional();
    RepresentativeWindowsWithOneSpike(); OutlierBudgetCannotHideWrongStage();
    std::printf("wrist calibration: %s (%d checks, %d failures)\n",failures?"FAIL":"PASS",checks,failures);
    return failures?1:0;
}
