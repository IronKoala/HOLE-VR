// clang++ -std=c++20 -Isrc tests/test_functional_reach.cpp
// src/vr/body/functional_reach.cpp src/vr/body/ik_math.cpp
#include "vr/body/functional_reach.h"
#include "vr/body/ik_math.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
using namespace holevr::vr;
namespace reach=holevr::vr::functional_reach;
static unsigned checks=0,failures=0;
#define EXPECT(x) do { ++checks; if(!(x)) { ++failures; std::printf("FAIL %d: %s\n",__LINE__,#x); } } while(0)
static bool Near(double a,double b,double e=1e-6) { return std::fabs(a-b)<=e; }
static bool Near(FVector a,FVector b,double e=1e-6) { return ik::Length(a-b)<=e; }
struct Fixture {
    FVector shoulder{-5,20,-25},local{59,0,0};
    std::vector<reach::Group> groups{3};
    FQuat device=ik::AxisAngle({.3,-.2,.7},.83);
    explicit Fixture(double down=10,double sign=1,double yaw=0,double bow=14) {
        shoulder.Y*=sign; const FQuat world=YawOnly(yaw); shoulder=world.Rotate(shoulder);
        local=device.Inverse().Rotate(local);
        for(int group=0;group<3;++group) for(int i=0;i<(group==1?45:15);++i) {
            const double t=group==0?0:group==2?1:double(i)/44;
            const double a=t*kPi/2,tilt=(down+bow*std::sin(t*kPi))*kDegToRad;
            const FVector f{std::cos(tilt)*std::cos(a),sign*std::cos(tilt)*std::sin(a),-std::sin(tilt)};
            const FVector palm=ik::AxisAngle(f,sign*(1-t)*kPi/2).Rotate(ik::Normalize(ik::Reject({0,0,-1},f)));
            const auto q=world*ik::AlignFrames({1,0,0},{0,0,1},f,palm)*device;
            groups[group].push_back({shoulder+q.Rotate(local),q});
        }
    }
};
static void ObservedAndTilt() {
    for(double down:{-15.,0.,5.,10.,15.}) for(double sign:{-1.,1.}) for(double yaw:{0.,73.,-155.}) {
        Fixture f(down,sign,yaw);
        const auto r=reach::Fit(f.groups,f.shoulder+YawOnly(yaw).Rotate({4,sign*3,5}),65);
        EXPECT(r.valid); EXPECT(r.observed);
        if(r.valid) { EXPECT(Near(r.shoulder,f.shoulder)); EXPECT(Near(r.reach,59)); EXPECT(r.rms<1e-8); EXPECT(r.condition>.025); }
    }
}
static void DeficientFallback() {
    for(double down:{0.,5.,10.,15.}) for(double sign:{-1.,1.}) {
        Fixture f(down,sign); f.groups.erase(f.groups.begin()+1);
        const auto r=reach::Fit(f.groups,f.shoulder,65);
        EXPECT(r.valid && !r.observed); EXPECT(Near(r.shoulder,f.shoulder)); EXPECT(Near(r.reach,59)); EXPECT(r.rms<1e-8);
    }
    // Even 90 distinct samples about ONE fixed rotation axis leave one
    // direction unobservable. More samples must not fabricate identifiability.
    Fixture f; f.groups.assign(3,{});
    for(int g=0;g<3;++g) for(int i=0;i<30;++i) {
        const auto q=ik::AxisAngle({0,0,1},(g*30+i)*kDegToRad)*f.device;
        f.groups[g].push_back({f.shoulder+q.Rotate(f.local),q});
    }
    auto r=reach::Fit(f.groups,f.shoulder,59); EXPECT(r.valid&&!r.observed); EXPECT(Near(r.reach,59));
    // Tiny multiaxis jitter is not sufficient excitation either.
    for(auto& group:f.groups) for(std::size_t i=0;i<group.size();++i) {
        const auto q=ik::AxisAngle({double(i%3==0),double(i%3==1),double(i%3==2)},.002)*f.device;
        group[i]={f.shoulder+q.Rotate(f.local),q};
    }
    r=reach::Fit(f.groups,f.shoulder,59); EXPECT(r.valid&&!r.observed);
}
static void RobustAndWeighted() {
    Fixture f;
    for(std::size_t g=0;g<f.groups.size();++g) for(std::size_t i=0;i<f.groups[g].size();++i) {
        const double a=double(i)*.73+double(g);
        f.groups[g][i].wrist=f.groups[g][i].wrist+FVector{.25*std::sin(a),.2*std::cos(a),.2*std::sin(a*1.3)};
    }
    f.groups[0][3].wrist=f.groups[0][3].wrist+FVector{0,20,15};
    f.groups[1][6].wrist=f.groups[1][6].wrist+FVector{25,0,-15};
    f.groups[2][7].wrist=f.groups[2][7].wrist+FVector{-25,10,0};
    const auto a=reach::Fit(f.groups,f.shoulder+FVector{3,0,0},59);
    EXPECT(a.valid&&a.observed); EXPECT(a.inliers+3==a.samples); EXPECT(Near(a.shoulder,f.shoulder,1)); EXPECT(Near(a.reach,59,1));
    auto dense=f.groups;
    for(int repeat=0;repeat<8;++repeat) dense[0].insert(dense[0].end(),f.groups[0].begin(),f.groups[0].end());
    std::reverse(dense[1].begin(),dense[1].end());
    const auto b=reach::Fit(dense,f.shoulder+FVector{3,0,0},59);
    EXPECT(b.valid&&b.observed); EXPECT(Near(a.shoulder,b.shoulder,1e-7)); EXPECT(Near(a.reach,b.reach,1e-7));
    // A coherent bad stage cannot be hidden behind a much denser good one.
    for(auto& p:dense[2]) p.wrist=p.wrist+FVector{0,0,16};
    EXPECT(!reach::Fit(dense,f.shoulder,59).valid);
    Fixture too_many;
    for(int i=0;i<3;++i) too_many.groups[0][i].wrist=too_many.groups[0][i].wrist+FVector{25,0,0};
    EXPECT(!reach::Fit(too_many.groups,too_many.shoulder,59).valid);
}
static void FrameCovarianceAndBadData() {
    Fixture f(10,-1);
    const auto a=reach::Fit(f.groups,f.shoulder,59); EXPECT(a.valid&&a.observed);
    const auto turn=ik::AxisAngle({.1,.7,-.4},1.1),device=ik::AxisAngle({-.3,.1,.5},.9);
    const FVector shift{8,13,-3}; auto transformed=f.groups;
    for(auto& group:transformed) for(auto& p:group) { p.wrist=shift+turn.Rotate(p.wrist); p.grip=turn*p.grip*device; }
    const auto b=reach::Fit(transformed,shift+turn.Rotate(f.shoulder),59);
    EXPECT(b.valid&&b.observed); EXPECT(Near(b.shoulder,shift+turn.Rotate(a.shoulder))); EXPECT(Near(a.reach,b.reach));
    EXPECT(Near(a.condition,b.condition)); EXPECT(Near(a.rms,b.rms));
    transformed[0][0].grip={0,0,0,0}; EXPECT(!reach::Fit(transformed,f.shoulder,59).valid);
    transformed=f.groups; transformed[0][0].wrist.X=std::numeric_limits<double>::quiet_NaN(); EXPECT(!reach::Fit(transformed,f.shoulder,59).valid);
    EXPECT(!reach::Fit(f.groups,f.shoulder,0).valid); EXPECT(!reach::Fit({},f.shoulder,59).valid);
    transformed=f.groups; transformed[0].resize(2); EXPECT(!reach::Fit(transformed,f.shoulder,59).valid);
}
static void ModelUncertainty() {
    Fixture noisy;
    for(auto& group:noisy.groups) for(std::size_t i=0;i<group.size();++i) {
        const double a=double(i)*1.97;
        group[i].wrist=group[i].wrist+FVector{1.1*std::sin(a),1.1*std::cos(a),1.1*std::sin(a*1.7)};
    }
    auto r=reach::Fit(noisy.groups,noisy.shoulder,59);
    EXPECT(r.valid&&!r.observed); EXPECT(Near(r.shoulder,noisy.shoulder)); EXPECT(Near(r.reach,59,.3));
    // A previous shoulder that cannot explain otherwise consistent endpoints
    // must fail; fallback is not a license to certify inconsistent geometry.
    Fixture endpoint; endpoint.groups.erase(endpoint.groups.begin()+1);
    EXPECT(!reach::Fit(endpoint.groups,endpoint.shoulder+FVector{15,-8,0},59).valid);
    // Independent shoulder movement over the transition breaks the rigid
    // model. A small subset of those motions cannot be trimmed as a whole.
    Fixture moving;
    for(std::size_t i=0;i<moving.groups[1].size();++i) {
        const double a=double(i)/double(moving.groups[1].size()-1);
        moving.groups[1][i].wrist=moving.groups[1][i].wrist+FVector{0,0,12*std::sin(2*kPi*a)};
    }
    EXPECT(!reach::Fit(moving.groups,moving.shoulder,59).valid);
}
static void FlatMovement() {
    for(double amplitude:{0.,.25,.5,2.}) for(double sign:{-1.,1.}) {
        Fixture f(10,sign,73,0);
        for(std::size_t g=0;g<f.groups.size();++g) for(std::size_t i=0;i<f.groups[g].size();++i) {
            const double a=double(i)*1.97+double(g)*.5;
            // amplitude is the positional noise magnitude, not per axis.
            const FVector n=ik::Normalize({std::sin(a),std::cos(a),std::sin(a*1.7)});
            f.groups[g][i].wrist=f.groups[g][i].wrist+n*amplitude;
        }
        const auto r=reach::Fit(f.groups,f.shoulder,59);
        if(amplitude<2) EXPECT(r.valid);
        if(amplitude<=.5) EXPECT(r.observed);
        if(amplitude>=2) EXPECT(!r.observed);
        if(r.valid) { EXPECT(Near(r.shoulder,f.shoulder,.6)); EXPECT(Near(r.reach,59,.3)); }
        if(amplitude<=.5) {
            const auto fresh=reach::Fit(f.groups,f.shoulder+FVector{4,3,0},65);
            EXPECT(fresh.valid&&fresh.observed); EXPECT(Near(fresh.shoulder,f.shoulder,.6)); EXPECT(Near(fresh.reach,59,.3));
        }
    }
}
static void ConditionalRadialReach() {
    for(double sign:{-1.,1.}) for(double yaw:{0.,73.}) {
        Fixture f(10,sign,yaw,0);f.groups.erase(f.groups.begin()+1);
        // Stable endpoint radii with six degrees of inter-stage grip-basis
        // change reproduce the real recording's ~3cm Cartesian residual.
        const FQuat change=ik::AxisAngle(YawOnly(yaw).Rotate({0,0,1}),6*kDegToRad);
        for(auto& p:f.groups[1]) p.grip=change*p.grip;
        EXPECT(!reach::Fit(f.groups,f.shoulder,59).valid);
        auto r=reach::FitPriorReach(f.groups,f.shoulder);
        EXPECT(r.valid&&!r.observed&&r.condition==0); EXPECT(Near(r.shoulder,f.shoulder));
        EXPECT(Near(r.reach,59)); EXPECT(r.rms<1e-8);
        // A bent/shortened out hold cannot pass only because grip orientation
        // was removed from this conditional distance estimate.
        for(auto& p:f.groups[1]) p.wrist=f.shoulder+(p.wrist-f.shoulder)*(49./59.);
        EXPECT(!reach::FitPriorReach(f.groups,f.shoulder).valid);
    }
    Fixture f;f.groups.erase(f.groups.begin()+1);
    for(std::size_t i=0;i<f.groups[0].size();++i) {
        const double radius=58.7+.2*std::sin(i*.8);
        f.groups[0][i].wrist=f.shoulder+ik::Normalize(f.groups[0][i].wrist-f.shoulder)*radius;
    }
    for(std::size_t i=0;i<f.groups[1].size();++i) {
        const double radius=59.3+.2*std::cos(i*.7);
        f.groups[1][i].wrist=f.shoulder+ik::Normalize(f.groups[1][i].wrist-f.shoulder)*radius;
    }
    f.groups[0][2].wrist=f.groups[0][2].wrist+ik::Normalize(f.groups[0][2].wrist-f.shoulder)*20;
    f.groups[1][3].wrist=f.groups[1][3].wrist-ik::Normalize(f.groups[1][3].wrist-f.shoulder)*15;
    const auto a=reach::FitPriorReach(f.groups,f.shoulder);
    EXPECT(a.valid&&!a.observed&&a.inliers+2==a.samples); EXPECT(Near(a.reach,59,.1));
    auto dense=f.groups;
    for(int repeat=0;repeat<8;++repeat) dense[0].insert(dense[0].end(),f.groups[0].begin(),f.groups[0].end());
    std::reverse(dense[1].begin(),dense[1].end());
    const auto b=reach::FitPriorReach(dense,f.shoulder);
    EXPECT(b.valid);EXPECT(Near(a.reach,b.reach));EXPECT(Near(a.rms,b.rms));
    // Exactly15% discarded outliers are permitted;20% are not.
    Fixture budget;budget.groups.erase(budget.groups.begin()+1);
    for(auto& group:budget.groups) group.resize(20,group.front());
    for(int i=0;i<3;++i) budget.groups[0][i].wrist=budget.shoulder+ik::Normalize(budget.groups[0][i].wrist-budget.shoulder)*80;
    EXPECT(reach::FitPriorReach(budget.groups,budget.shoulder).valid);
    budget.groups[0][3].wrist=budget.groups[0][0].wrist;
    EXPECT(!reach::FitPriorReach(budget.groups,budget.shoulder).valid);
    Fixture three;EXPECT(!reach::FitPriorReach(three.groups,three.shoulder).valid);
    three.groups.erase(three.groups.begin()+1);three.groups[0].resize(2);
    EXPECT(!reach::FitPriorReach(three.groups,three.shoulder).valid);
    Fixture mismatch;mismatch.groups.erase(mismatch.groups.begin()+1);
    //4cm inter-stage change is below either standalone RMS cap but exceeds
    //the explicit cross-stage limit and must be rejected.
    for(auto& p:mismatch.groups[1]) p.wrist=mismatch.shoulder+ik::Normalize(p.wrist-mismatch.shoulder)*63;
    EXPECT(!reach::FitPriorReach(mismatch.groups,mismatch.shoulder).valid);
    for(int stage=0;stage<2;++stage) for(auto& p:mismatch.groups[stage])
        p.wrist=mismatch.shoulder+ik::Normalize(p.wrist-mismatch.shoulder)*(stage==0?40:42.2);
    EXPECT(!reach::FitPriorReach(mismatch.groups,mismatch.shoulder).valid); //5% stricter than3cm
    Fixture noisy;noisy.groups.erase(noisy.groups.begin()+1);
    for(auto& group:noisy.groups) for(std::size_t i=0;i<group.size();++i)
        group[i].wrist=noisy.shoulder+ik::Normalize(group[i].wrist-noisy.shoulder)*(59+(double(i%3)-1)*2.5);
    EXPECT(!reach::FitPriorReach(noisy.groups,noisy.shoulder).valid); //RMS, not just max error
    mismatch.groups[0][0].wrist.X=std::numeric_limits<double>::quiet_NaN();
    EXPECT(!reach::FitPriorReach(mismatch.groups,mismatch.shoulder).valid);
}
struct PositionFixture {
    FVector centre{-7,22,-24};
    FVector x{1,0,0},y{0,1,0},normal{0,0,1};
    double radius=60;
    std::vector<reach::Group> groups{3}; // first two endpoint holds, then opening motion
    PositionFixture(double angle=90,double tilt=0,bool spatial=false) {
        const auto turn=ik::AxisAngle({.3,.7,-.2},tilt*kDegToRad);
        x=turn.Rotate(x); y=turn.Rotate(y); normal=turn.Rotate(normal);
        for(int group=0;group<3;++group) for(int i=0;i<(group==2?61:20);++i) {
            const double t=group==0?0:group==1?1:double(i)/60;
            const double a=angle*kDegToRad*t;
            const double elevation=spatial?.35*std::sin(2*kPi*t):0;
            const auto direction=(x*std::cos(a)+y*std::sin(a))*std::cos(elevation)+normal*std::sin(elevation);
            groups[group].push_back({centre+direction*radius,FQuat{}});
        }
    }
    void Noise(double magnitude) {
        for(std::size_t g=0;g<groups.size();++g) for(std::size_t i=0;i<groups[g].size();++i) {
            const double a=double(i)*1.97+double(g)*.9;
            groups[g][i].wrist=groups[g][i].wrist+ik::Normalize({std::sin(a),std::cos(a),std::sin(a*1.7)})*magnitude;
        }
    }
};
static void PositionObservableComponents() {
    for(double opening:{45.,60.,90.}) for(double tilt:{0.,37.,121.}) for(double offset:{0.,8.,-12.}) {
        PositionFixture f(opening,tilt);
        const FVector prior=f.centre+f.x*18-f.y*14+f.normal*offset;
        const auto r=reach::FitPositions(f.groups,prior,65);
        EXPECT(r.valid && !r.observed && r.observed_dimensions==2);
        if(r.valid) {
            EXPECT(Near(r.shoulder,f.centre+f.normal*offset,1e-5));
            EXPECT(Near(r.reach,std::sqrt(f.radius*f.radius+offset*offset),1e-5));
            EXPECT(r.rms<1e-6);
            // A prior shift in an observable direction must not move the fit.
            const auto another=reach::FitPositions(f.groups,prior+f.x*10-f.y*9,45);
            EXPECT(another.valid && Near(another.shoulder,r.shoulder,1e-5));
        }
    }
    for(double tilt:{0.,51.,127.}) {
        PositionFixture f(90,tilt,true);
        const auto r=reach::FitPositions(f.groups,f.centre+FVector{20,-13,11},65);
        EXPECT(r.valid && r.observed && r.observed_dimensions==3);
        if(r.valid) { EXPECT(Near(r.shoulder,f.centre,1e-5)); EXPECT(Near(r.reach,60,1e-5)); }
        f.Noise(.25);
        const auto noisy=reach::FitPositions(f.groups,f.centre+FVector{20,-13,11},65);
        EXPECT(noisy.valid && noisy.observed && noisy.observed_dimensions==3);
        if(noisy.valid) { EXPECT(Near(noisy.shoulder,f.centre,1.5)); EXPECT(Near(noisy.reach,60,1)); }
    }
}
static void PositionDeficientMotion() {
    for(double tilt:{0.,37.,121.}) {
        PositionFixture f(90,tilt); f.groups.resize(2);
        const auto first=f.groups[0][0].wrist,last=f.groups[1][0].wrist;
        const FVector prior=f.centre+f.x*18-f.y*14+f.normal*8;
        const auto chord=ik::Normalize(last-first);
        const auto expected=prior+chord*ik::Dot((first+last)*.5-prior,chord);
        const auto r=reach::FitPositions(f.groups,prior,60);
        EXPECT(r.valid && !r.observed && r.observed_dimensions==1);
        if(r.valid) { EXPECT(Near(r.shoulder,expected,1e-6)); EXPECT(Near(r.reach,ik::Length(first-expected),1e-6)); }
        f.groups[1]=f.groups[0];
        const auto still=reach::FitPositions(f.groups,f.centre,60);
        EXPECT(still.valid && !still.observed && still.observed_dimensions==0);
        EXPECT(Near(still.shoulder,f.centre) && Near(still.reach,60));
    }
    for(double opening:{5.,15.,30.}) {
        PositionFixture short_arc(opening);
        const auto r=reach::FitPositions(short_arc.groups,short_arc.centre,60);
        EXPECT(r.valid && !r.observed && r.observed_dimensions<=1);
    }
    PositionFixture line; line.groups.assign(3,{});
    for(int group=0;group<3;++group) for(int i=0;i<20;++i) {
        const double t=group==0?-8:group==1?8:-8+16.*i/19;
        line.groups[group].push_back({line.centre+FVector{t,0,60},FQuat{}});
    }
    const auto r=reach::FitPositions(line.groups,line.centre,60);
    EXPECT(r.valid && !r.observed && r.observed_dimensions<=1);
}
static void PositionNoiseAndRobustness() {
    for(double noise:{.25,.5}) for(double angle:{60.,90.}) for(double tilt:{0.,68.}) {
        PositionFixture f(angle,tilt); f.Noise(noise);
        const auto prior=f.centre+f.x*15-f.y*10;
        const auto r=reach::FitPositions(f.groups,prior,60);
        EXPECT(r.valid && !r.observed && r.observed_dimensions==2);
        if(r.valid) { EXPECT(Near(r.shoulder,f.centre,2)); EXPECT(Near(r.reach,60,1.5)); EXPECT(r.pivot_sensitivity_cm<=8.01); }
    }
    PositionFixture f(90,42,true); f.Noise(.25);
    for(auto& group:f.groups) group[3].wrist=group[3].wrist+FVector{25,-25,20};
    const auto a=reach::FitPositions(f.groups,f.centre+FVector{15,-12,8},60);
    EXPECT(a.valid && a.observed && a.inliers+3==a.samples);
    if(a.valid) { EXPECT(Near(a.shoulder,f.centre,1.5)); EXPECT(Near(a.reach,60,1)); }
    auto dense=f.groups;
    for(int i=0;i<8;++i) dense[0].insert(dense[0].end(),f.groups[0].begin(),f.groups[0].end());
    std::reverse(dense[2].begin(),dense[2].end());
    const auto b=reach::FitPositions(dense,f.centre+FVector{15,-12,8},60);
    EXPECT(b.valid && b.observed);
    if(a.valid&&b.valid) { EXPECT(Near(a.shoulder,b.shoulder,1e-5)); EXPECT(Near(a.reach,b.reach,1e-5)); }
    PositionFixture budget;
    for(int i=0;i<3;++i) budget.groups[0][i].wrist=budget.groups[0][i].wrist+FVector{0,0,40};
    const auto accepted=reach::FitPositions(budget.groups,budget.centre,60);
    EXPECT(accepted.valid && !accepted.observed && accepted.observed_dimensions==2);
    EXPECT(accepted.inliers+3==accepted.samples);
    budget.groups[0][3].wrist=budget.groups[0][0].wrist;
    EXPECT(!reach::FitPositions(budget.groups,budget.centre,60).valid);
    // These displaced transition points lie on a different, exact sphere.
    // Zero radial error must not let an isolated spike invent an observed axis.
    for(double displacement:{40.,85.}) for(int spikes:{1,4,8}) {
        PositionFixture ghost(90,37);
        for(int i=0;i<spikes;++i) ghost.groups[2][20+i].wrist=ghost.groups[2][20+i].wrist+ghost.normal*displacement;
        const auto r=reach::FitPositions(ghost.groups,ghost.centre,60);
        EXPECT(r.valid && !r.observed && r.observed_dimensions==2);
        if(r.valid) { EXPECT(Near(r.shoulder,ghost.centre,1e-5)); EXPECT(Near(r.reach,60,1e-5)); }
    }
    // A dense flat hold cloud cannot turn tracking jitter into a third axis.
    PositionFixture planar; planar.Noise(.75);
    const auto noisy_planar=reach::FitPositions(planar.groups,planar.centre,60);
    EXPECT(!noisy_planar.observed && noisy_planar.observed_dimensions<=2);
}
static void PositionFramesAndInvalidData() {
    PositionFixture f(90,36,true);
    const auto first=reach::FitPositions(f.groups,f.centre,60); EXPECT(first.valid && first.observed);
    const auto turn=ik::AxisAngle({.4,-.7,.1},.9); const FVector shift{13,-9,5};
    auto moved=f.groups;
    for(auto& group:moved) for(auto& p:group) { p.wrist=shift+turn.Rotate(p.wrist); p.grip={0,0,0,0}; }
    const auto second=reach::FitPositions(moved,shift+turn.Rotate(f.centre),60);
    EXPECT(second.valid && second.observed); // controller roll is deliberately irrelevant
    EXPECT(Near(second.shoulder,shift+turn.Rotate(first.shoulder),1e-5));
    EXPECT(Near(first.reach,second.reach,1e-5));
    moved[0][0].wrist.X=std::numeric_limits<double>::quiet_NaN();
    EXPECT(!reach::FitPositions(moved,f.centre,60).valid);
    EXPECT(!reach::FitPositions(f.groups,f.centre,0).valid);
    EXPECT(!reach::FitPositions({},f.centre,60).valid);
    moved=f.groups; moved[0].resize(2); EXPECT(!reach::FitPositions(moved,f.centre,60).valid);
    moved=f.groups; moved[0].resize(1025,moved[0].front()); EXPECT(!reach::FitPositions(moved,f.centre,60).valid);
}

int main() {
    PositionObservableComponents(); PositionDeficientMotion(); PositionNoiseAndRobustness(); PositionFramesAndInvalidData();
    ObservedAndTilt(); DeficientFallback(); RobustAndWeighted(); FrameCovarianceAndBadData(); ModelUncertainty(); FlatMovement(); ConditionalRadialReach();
    std::printf("functional reach: %u checks, %u failures\n",checks,failures); return failures?1:0;
}
