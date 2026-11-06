#include "vr/body/stair_pose.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace holevr::vr;
using namespace holevr::vr::body::stairs;
static int checks=0;
static void Check(bool ok,const char* text) { ++checks; if(!ok) { std::printf("FAIL: %s\n",text); std::exit(1); } }
static bool Near(double a,double b) { return std::fabs(a-b)<1e-6; }
static std::array<Sample,kSamples> Flight(int sign=1) {
    std::array<Sample,kSamples> out{};
    for(int i=0;i<kSamples;++i) {
        const double at=(i-kSamples/2)*kSpacing;
        out[i]={true,at,sign*18*std::floor(at/30),{0,0,1},1};
    }
    return out;
}
int main() {
    for(int sign:{-1,1}) {
        auto samples=Flight(sign);
        const auto profile=Recognize(samples,{}, {1,0,0},0);
        Check(profile.valid&&profile.count>=3&&profile.rise_sign==sign,"recognize ascent and descent from multiple treads");
        const auto upper=Select(profile,{35,10,0},0,15);
        const auto lower=Select(profile,{5,-10,0},0,15);
        Check(upper.valid&&lower.valid&&Near(upper.sole.Z,sign*18)&&Near(lower.sole.Z,0),"each foot selects its own tread");
        const auto edge=Select(profile,{22,0,0},0,15);
        Check(edge.valid&&edge.sole.X<=edge.end-8,"heel and toe are seated inside the observed tread");
        Check(!Select(profile,{35,40,0},0).valid,"unobserved surfaces far beside staircase are not authorized");
        Check(!Select(profile,{35,0,0},100).valid,"remote tread cannot become a foothold from stale floor height");
        Check(!Select(profile,{100,0,0},0).valid,"do not extrapolate beyond observed treads");
    }
    auto samples=Flight();
    for(double depth:{24.,26.,28.,30.}) for(int phase=0;phase<int(depth);++phase) {
        samples=Flight();
        for(auto& s:samples) s.z=18*std::floor((s.along+phase)/depth);
        Check(Recognize(samples,{}, {1,0,0},0).valid,"ordinary tread depth remains recognizable across scan-grid phases");
    }
    samples=Flight();
    for(auto& s:samples) s.z=s.along>0?70:0;
    Check(!Recognize(samples,{}, {1,0,0},0).valid,"shooting-range table is a single tall edge, not stairs");
    for(auto& s:samples) s.z=s.along>0?18:0;
    Check(!Recognize(samples,{}, {1,0,0},0).valid,"single low ledge does not prove a staircase");
    for(auto& s:samples) s.z=.3*s.along;
    Check(!Recognize(samples,{}, {1,0,0},0).valid,"ramp cannot pass as flat treads");
    samples=Flight(); samples[4].hit=false;
    Check(!Recognize(samples,{}, {1,0,0},0).valid,"missing trace does not bridge an unseen tread");
    samples=Flight(); samples[4].normal={1,0,0};
    Check(!Recognize(samples,{}, {1,0,0},0).valid,"riser wall is not support");
    samples=Flight(); samples[4].z=std::numeric_limits<double>::quiet_NaN();
    Check(!Recognize(samples,{}, {1,0,0},0).valid,"non-finite scan is rejected");
    samples=Flight(); for(auto& s:samples) s.z*=2;
    Check(!Recognize(samples,{}, {1,0,0},0).valid,"oversized risers cannot authorize furniture climbing");
    samples=Flight(); for(auto& s:samples) s.z*=.2;
    Check(!Recognize(samples,{}, {1,0,0},0).valid,"tiny unevenness is not stairs");
    samples=Flight(); for(int i=0;i<kSamples;++i) samples[i].support_id=0;
    Check(!Recognize(samples,{}, {1,0,0},0).valid,"support identity is required");
    samples=Flight();
    const auto rotated=Recognize(samples,{100,200,30},{0,1,0},0);
    const auto landing=Select(rotated,{110,235,0},0);
    Check(landing.valid&&Near(landing.sole.X,110)&&Near(landing.sole.Y,235)&&Near(landing.sole.Z,18),
          "recognition and selection are invariant to horizontal world rotation and translation");
    std::printf("stair_pose: %d checks passed\n",checks);
}
