#include <algorithm>
#include <cmath>
#include <cstdio>

#include "vr/body/thumb_pose.h"

using namespace holevr::vr;
using namespace holevr::vr::ik;
namespace fc = holevr::vr::finger_contact;
namespace tp = holevr::vr::thumb_pose;
namespace jl = holevr::vr::joint_limits;
static int failures = 0;
#define EXPECT(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static fingers::Chain Thumb(bool left = false, bool rolled = false) {
    const double sign = left ? -1 : 1;
    FVector dir = Normalize(FVector{1, 0.4*sign, 0.7});
    skel::Xf j[3];
    const FQuat align = Arc({1,0,0}, dir);
    j[0].translation = {4.8,2.4*sign,2.5};
    j[1].translation = j[0].translation + dir*3.5;
    j[2].translation = j[1].translation + dir*3.2;
    for (int i = 0; i < 3; ++i)
        j[i].rotation = align * AxisAngle({1,0,0}, rolled ? (i*37-21)*kDegToRad : 0);
    return fingers::Build(j, {0,sign,0}, {10.1,2.3*sign,-4.6}, true);
}
static void Bounded(const fingers::Chain& c, const fingers::Angles& a, const fc::Bounds& b) {
    for (int j = 0; j < 3; ++j) {
        const FQuat q = c.ref[j].rotation.Inverse()*fingers::LocalRotation(c,j,a);
        EXPECT(jl::Within(b.axes[j],b.limits[j],q,1e-7));
    }
}
static double Alignment(const fingers::Chain& c,const fingers::Angles& a,const FVector& palm,const FVector& index) {
    skel::Xf j[3]; FVector tip;
    fingers::Fk(c,a,j,&tip);
    return Dot(tp::PadDirection(c,a,palm),Normalize(index-tip));
}
int main() {
    const auto c=Thumb(); const auto b=fc::MakeBounds(c,0);
    const FVector palm{0,1,0},index{12.1,1.8,2.1};
    const auto open=tp::Target(c,palm,index,{},b);
    const auto wrap=tp::Target(c,palm,index,{1,0,false},b);
    const auto pinch=tp::Target(c,palm,index,{0,1,false},b);
    Bounded(c,open,b); Bounded(c,wrap,b); Bounded(c,pinch,b);
    const auto base=jl::Decompose(b.axes[0],pinch.extra[0]);
    EXPECT(std::fabs(base.side)>0.02);
    EXPECT(std::fabs(base.twist)>0.02);
    auto no_opposition=pinch;
    auto plain=base; plain.twist=0;
    no_opposition.extra[0]=jl::Compose(b.axes[0],plain);
    const double before=Alignment(c,no_opposition,palm,index),after=Alignment(c,pinch,palm,index);
    EXPECT(after>before+0.01);
    std::printf("thumb pad toward index: without base axial rotation %.3f, with opposition %.3f\n",before,after);
    // Pinch and power wrapping use distinct MCP/IP configurations.
    EXPECT(std::fabs(jl::Decompose(b.axes[1],pinch.extra[1]).flex-jl::Decompose(b.axes[1],wrap.extra[1]).flex)>0.1);

    // Curves remain continuous and bounded as trigger/grip vary.
    auto last=open;
    for(int i=0;i<=200;++i) {
        const auto a=tp::Target(c,palm,index,{i/200.0,1-i/200.0,false},b);
        Bounded(c,a,b);
        if(i>0) for(int j=0;j<3;++j) {
            const double d=std::fabs(a.extra[j].X*last.extra[j].X+a.extra[j].Y*last.extra[j].Y+
                                      a.extra[j].Z*last.extra[j].Z+a.extra[j].W*last.extra[j].W);
            EXPECT(d>0.999);
        }
        last=a;
    }
    // Mirrored geometry produces mirrored pad directions and fingertip paths.
    {
        const auto left=Thumb(true); const auto lb=fc::MakeBounds(left,0);
        const FVector lpalm{0,-1,0},lindex{index.X,-index.Y,index.Z};
        const auto pose=tp::Target(left,lpalm,lindex,{0,1,false},lb);
        Bounded(left,pose,lb);
        const FVector rp=tp::PadDirection(c,pinch,palm),lp=tp::PadDirection(left,pose,lpalm);
        EXPECT(Length(lp-FVector{rp.X,-rp.Y,rp.Z})<1e-7);
        skel::Xf jr[3],jlft[3]; FVector rt,lt;
        fingers::Fk(c,pinch,jr,&rt); fingers::Fk(left,pose,jlft,&lt);
        EXPECT(Length(lt-FVector{rt.X,-rt.Y,rt.Z})<1e-7);
    }
    // Arbitrary bone roll should not change the hand-space solution.
    {
        const auto rolled=Thumb(false,true); const auto rb=fc::MakeBounds(rolled,0);
        const auto pose=tp::Target(rolled,palm,index,{0,1,false},rb);
        Bounded(rolled,pose,rb);
        skel::Xf jr[3],jb[3]; FVector rt,bt;
        fingers::Fk(c,pinch,jr,&rt); fingers::Fk(rolled,pose,jb,&bt);
        EXPECT(Length(rt-bt)<1e-7);
        EXPECT(Length(tp::PadDirection(c,pinch,palm)-tp::PadDirection(rolled,pose,palm))<1e-7);
    }
    // Existing reference curl must not be added a second time to MCP/IP
    // intentions. The same configured anatomical open angle is requested.
    {
        auto curled=c;
        curled.ref[1].rotation=curled.ref[1].rotation*AxisAngle(curled.flex[1],20*kDegToRad);
        const auto cb=fc::MakeBounds(curled,0);
        EXPECT(cb.limits[1].ref_bend>15*kDegToRad);
        const auto pose=tp::Target(curled,palm,index,{},cb);
        Bounded(curled,pose,cb);
        const double anatomical=jl::Decompose(cb.axes[1],pose.extra[1]).flex+cb.limits[1].ref_bend;
        EXPECT(std::fabs(anatomical-5*kDegToRad)<1e-7);
    }
    if(failures) { std::printf("thumb pose: %d failures\n",failures); return 1; }
    std::puts("thumb pose: all tests passed"); return 0;
}
