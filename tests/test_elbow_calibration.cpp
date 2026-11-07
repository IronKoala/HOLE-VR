// clang++ -std=c++20 -Isrc tests/test_elbow_calibration.cpp
// src/vr/body/elbow_calibration.cpp src/vr/body/ik_math.cpp
#include "vr/body/elbow_calibration.h"
#include "vr/body/ik_math.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace holevr::vr;
namespace elbow=holevr::vr::elbow_calibration;
static unsigned checks=0,failures=0;
#define EXPECT(x) do { ++checks; if(!(x)) { ++failures; std::printf("FAIL %d: %s\n",__LINE__,#x); } } while(0)
static bool Near(double a,double b,double e=1e-6) { return std::fabs(a-b)<=e; }
static bool Near(FVector a,FVector b,double e=1e-6) { return ik::Length(a-b)<=e; }
struct Fixture {
    FVector center{6,37,-21};
    double lower=28;
    std::vector<elbow::Observation> samples;
    Fixture(double span=65,int count=61,double noise=0) {
        for(int i=0;i<count;++i) {
            const double a=(15+span*double(i)/(count-1))*kDegToRad;
            const FVector direction{std::cos(a),0,std::sin(a)};
            const FVector n{noise*std::sin(i*1.71),noise*std::cos(i*.83),noise*std::sin(i*.67)};
            samples.push_back({center+direction*lower+n,direction});
        }
    }
};
static void ShoulderIndependentGeometry() {
    // The earlier split formula could inherit a 10cm-low shoulder. This fit
    // has no shoulder input: a tilted/dropped upper arm changes only E.
    const FVector shoulder{-6,18,-22};
    const auto upper_dir=ik::Normalize(FVector{std::sin(25*kDegToRad),std::cos(25*kDegToRad),-std::sin(15*kDegToRad)});
    const auto center=shoulder+upper_dir*31;
    for(double sign:{-1.,1.}) for(double yaw:{-155.,0.,73.}) {
        Fixture f; const auto q=YawOnly(yaw);
        for(auto& p:f.samples) {
            p.wrist=p.wrist-f.center+center;
            p.wrist.Y*=sign; p.forearm_direction.Y*=sign;
            p.wrist=q.Rotate(p.wrist); p.forearm_direction=q.Rotate(p.forearm_direction);
        }
        FVector want=center; want.Y*=sign; want=q.Rotate(want);
        const auto r=elbow::Fit(f.samples);
        EXPECT(r.valid); EXPECT(Near(r.forearm_cm,28)); EXPECT(Near(r.elbow,want));
        EXPECT(r.rms_cm<1e-8); EXPECT(r.inliers==f.samples.size()); EXPECT(Near(r.span_degrees,65));
        FVector transformed_shoulder=shoulder; transformed_shoulder.Y*=sign;
        EXPECT(Near(ik::Length(r.elbow-q.Rotate(transformed_shoulder)),31));
    }
}
static void LimitedMotionAndNoise() {
    // No exact 20-degree posture threshold: a short, clean sweep is useful;
    // a numerically identical stationary jitter trace is not.
    Fixture small(12,31,.04);
    const auto r=elbow::Fit(small.samples);
    EXPECT(r.valid); EXPECT(Near(r.forearm_cm,28,.5)); EXPECT(Near(r.elbow,small.center,.5));
    EXPECT(r.uncertainty_cm<=5); EXPECT(r.span_degrees<20);
    Fixture jitter(.4,61,0); EXPECT(!elbow::Fit(jitter.samples).valid);
    Fixture small_bad(12,61,.65); EXPECT(!elbow::Fit(small_bad.samples).valid);
    Fixture normal(70,61,.25); const auto n=elbow::Fit(normal.samples);
    EXPECT(n.valid); EXPECT(Near(n.forearm_cm,28,.5)); EXPECT(Near(n.elbow,normal.center,.5));
}
static void IsolatedSpikesAndContradictoryMotion() {
    Fixture f(70,61,.12);
    f.samples[12].wrist=f.samples[12].wrist+FVector{25,-12,30};
    f.samples[41].wrist=f.samples[41].wrist+FVector{-20,25,-15};
    const auto r=elbow::Fit(f.samples);
    EXPECT(r.valid); EXPECT(r.inliers==59); EXPECT(Near(r.forearm_cm,28,.4)); EXPECT(Near(r.elbow,f.center,.4));
    Fixture many(70,61,.12);
    for(std::size_t i=0;i<12;++i) many.samples[i*5].wrist=many.samples[i*5].wrist+FVector{20,-10,25};
    EXPECT(!elbow::Fit(many.samples).valid);
    Fixture moving_elbow(70,61,.1);
    for(std::size_t i=0;i<moving_elbow.samples.size();++i)
        moving_elbow.samples[i].wrist.Y+=5*std::sin(double(i)*.15);
    EXPECT(!elbow::Fit(moving_elbow.samples).valid);
    Fixture wrist_only;
    for(auto& p:wrist_only.samples) p.wrist={10,20,30};
    EXPECT(!elbow::Fit(wrist_only.samples).valid);
}
static void EndpointPauseDoesNotCreateInformation() {
    Fixture base(60,61,.08);
    const auto a=elbow::Fit(base.samples); EXPECT(a.valid);
    auto pause=base.samples;
    for(int i=0;i<75;++i) pause.push_back(base.samples.back());
    const auto b=elbow::Fit(pause); EXPECT(b.valid);
    EXPECT(Near(b.forearm_cm,a.forearm_cm,.1)); EXPECT(Near(b.elbow,a.elbow,.1));
    // A tiny number of moving frames in a long hold cannot manufacture a
    // reliable measurement. The caller retains a labelled prior instead.
    for(int i=0;i<200;++i) pause.push_back(base.samples.back());
    EXPECT(!elbow::Fit(pause).valid);
    auto two=base.samples;
    for(std::size_t i=0;i<two.size();++i) two[i]=i%2?base.samples.front():base.samples.back();
    EXPECT(!elbow::Fit(two).valid);
    auto one=base.samples;
    for(auto& p:one) p=base.samples.back();
    EXPECT(!elbow::Fit(one).valid);
    // A dense good endpoint must not hide a coherently displaced moving arc.
    auto contaminated=base.samples;
    for(std::size_t i=15;i<45;++i) contaminated[i].wrist=contaminated[i].wrist+FVector{0,8,0};
    for(int i=0;i<75;++i) contaminated.push_back(base.samples.back());
    EXPECT(!elbow::Fit(contaminated).valid);
}
static void CovarianceAndBoundaries() {
    Fixture f(65,61,.12);
    f.samples[20].wrist=f.samples[20].wrist+FVector{20,20,-15};
    const auto original=elbow::Fit(f.samples); EXPECT(original.valid);
    const auto q=ik::AxisAngle(ik::Normalize({.3,-.8,.6}),1.1);
    const FVector shift{105,-72,39};
    for(auto& p:f.samples) { p.wrist=shift+q.Rotate(p.wrist); p.forearm_direction=q.Rotate(p.forearm_direction); }
    std::reverse(f.samples.begin(),f.samples.end());
    const auto changed=elbow::Fit(f.samples); EXPECT(changed.valid);
    EXPECT(Near(changed.forearm_cm,original.forearm_cm)); EXPECT(Near(changed.elbow,shift+q.Rotate(original.elbow)));
    EXPECT(Near(changed.rms_cm,original.rms_cm)); EXPECT(changed.inliers==original.inliers);
    const auto good=f.samples;
    f.samples[0].wrist.X=std::numeric_limits<double>::quiet_NaN(); EXPECT(!elbow::Fit(f.samples).valid);
    f.samples=good; f.samples[0].forearm_direction={}; EXPECT(!elbow::Fit(f.samples).valid);
    f.samples=good; f.samples[0].forearm_direction.Z=std::numeric_limits<double>::infinity(); EXPECT(!elbow::Fit(f.samples).valid);
    EXPECT(!elbow::Fit({}).valid); f.samples.resize(9); EXPECT(!elbow::Fit(f.samples).valid);
    f.samples.assign(513,good[0]); EXPECT(!elbow::Fit(f.samples).valid);
    Fixture negative;
    for(auto& p:negative.samples) p.forearm_direction=p.forearm_direction*-1;
    EXPECT(!elbow::Fit(negative.samples).valid);
}
static void IdentifiabilityLimitIsExplicit() {
    // Correlated elbow movement E_i = E + delta*u_i is indistinguishable from
    // a longer forearm using these observations alone. The helper certifies a
    // conditional model fit, not a directly tracked joint. The caller must
    // independently check total reach and must not relabel a prior as measured.
    Fixture f;
    for(auto& p:f.samples) p.wrist=p.wrist+p.forearm_direction*4;
    const auto r=elbow::Fit(f.samples);
    EXPECT(r.valid); EXPECT(Near(r.forearm_cm,32)); EXPECT(Near(r.elbow,f.center));
}
int main() {
    ShoulderIndependentGeometry(); LimitedMotionAndNoise(); IsolatedSpikesAndContradictoryMotion();
    EndpointPauseDoesNotCreateInformation(); CovarianceAndBoundaries(); IdentifiabilityLimitIsExplicit();
    std::printf("elbow calibration: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
