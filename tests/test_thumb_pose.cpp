#include <algorithm>
#include <cmath>
#include <cstdio>

#include "vr/body/thumb_pose.h"
#include "fixtures/white_man_hands.h"

using namespace holevr::vr;
using namespace holevr::vr::ik;
namespace fc = holevr::vr::controller_pose;
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
    // A reference-aligned pinch can have zero side angle. Its functional
    // opposition is verified by pad alignment, not a mandatory side offset.
    EXPECT(std::fabs(base.twist)>0.02);
    auto no_opposition=pinch;
    auto plain=base; plain.twist=0;
    no_opposition.extra[0]=jl::Compose(b.axes[0],plain);
    const double before=Alignment(c,no_opposition,palm,index),after=Alignment(c,pinch,palm,index);
    EXPECT(after>before+0.01);
    std::printf("thumb pad toward index: without base axial rotation %.3f, with opposition %.3f\n",before,after);
    // Pinch and power wrapping use distinct MCP/IP configurations.
    EXPECT(std::fabs(jl::Decompose(b.axes[1],pinch.extra[1]).flex-jl::Decompose(b.axes[1],wrap.extra[1]).flex)>0.1);
    // Power grip keeps its anatomical MCP/IP bend while its CMC aims
    // outside the closed fingers. Pinch retains its separate joint bend.
    EXPECT(std::fabs(jl::Decompose(b.axes[1],wrap.extra[1]).flex+b.limits[1].ref_bend-50*kDegToRad)<1e-7);
    EXPECT(std::fabs(jl::Decompose(b.axes[2],wrap.extra[2]).flex+b.limits[2].ref_bend-45*kDegToRad)<1e-7);
    EXPECT(std::fabs(jl::Decompose(b.axes[1],pinch.extra[1]).flex+b.limits[1].ref_bend-29*kDegToRad)<1e-7);
    EXPECT(std::fabs(jl::Decompose(b.axes[2],pinch.extra[2]).flex+b.limits[2].ref_bend-25*kDegToRad)<1e-7);

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
    // The actual game's baked thumb reference points out through the palm
    // plane. Neutral input rotates the generated CMC orientation without
    // moving its fixed origin or changing the MCP/IP anatomical open bend.
    for(int side=0;side<2;++side) {
        fingers::Chain chains[5]; fc::Bounds bounds[5]; FVector normal;
        white_man_hands::Build(side,chains,bounds,&normal);
        const auto pose=tp::Target(chains[0],normal,chains[1].ref[0].translation,{},bounds[0]);
        Bounded(chains[0],pose,bounds[0]);
        skel::Xf joints[3]; FVector tip;
        fingers::Fk(chains[0],pose,joints,&tip);
        const double root=Dot(joints[0].translation,normal);
        const double stations[4]={root,Dot(joints[1].translation,normal),Dot(joints[2].translation,normal),Dot(tip,normal)};
        const auto base=jl::Decompose(bounds[0].axes[0],pose.extra[0]);
        std::printf("actual thumb side%d open palm stations %.3f %.3f %.3f %.3f cm; CMC %.2f %.2f %.2f deg\n",
                    side,stations[0],stations[1],stations[2],stations[3],base.flex*kRadToDeg,base.side*kRadToDeg,base.twist*kRadToDeg);
        EXPECT(Length(joints[0].translation-chains[0].ref[0].translation)<1e-9);
        EXPECT(std::fabs(root-2.1335)<.001);
        EXPECT(*std::max_element(stations,stations+4)<=root+.30);
        const auto fist=tp::Target(chains[0],normal,chains[1].ref[0].translation,{1,1,false},bounds[0]);
        const auto grip_only=tp::Target(chains[0],normal,chains[1].ref[0].translation,{1,0,false},bounds[0]);
        const auto fist_base=jl::Decompose(bounds[0].axes[0],fist.extra[0]);
        const auto grip_base=jl::Decompose(bounds[0].axes[0],grip_only.extra[0]);
        // Original weighted thenar triangles require at least 22 degrees
        // on the left / 20 on the right beside the closing index. A tall
        // thumb tip alone did not prevent the base pad from intersecting.
        EXPECT((side==0 ? -1 : 1)*fist_base.side>22*kDegToRad);
        // Grip-only now also retains more of the outside neutral saddle.
        // The combined grip must keep its absolute thenar clearance, without
        // requiring an artificial twelve-degree gap from that safer grip.
        EXPECT(std::fabs(fist_base.side)>=std::fabs(grip_base.side));
        Bounded(chains[0],fist,bounds[0]);
        // All generated closing routes retain a modest saddle side/roll
        // excursion; broad anatomical limits alone allowed the old guide to
        // saturate axial roll and reverse the side across the palm.
        for(int mode=0;mode<3;++mode)for(int step=0;step<=100;++step) {
            const double amount=step/100.;
            const auto released=tp::Target(chains[0],normal,chains[1].ref[0].translation,
                {mode!=1?amount:0,mode!=0?amount:0,false},bounds[0]);
            const auto pressed=tp::Target(chains[0],normal,chains[1].ref[0].translation,
                {mode!=1?amount:0,mode!=0?amount:0,true},bounds[0]);
            const auto actual=jl::Decompose(bounds[0].axes[0],released.extra[0]);
            const double side_allowance=mode==0?10*kDegToRad:std::max(10*kDegToRad,std::fabs(base.side));
            EXPECT(std::fabs(actual.side-base.side)<=side_allowance+1e-8);
            EXPECT(actual.side*base.side>=-1e-8); // pinch may reach, not cross, reference
            if(base.side<0)EXPECT(actual.side>=base.side-10*kDegToRad-1e-8);
            else EXPECT(actual.side<=base.side+10*kDegToRad+1e-8);
            EXPECT(std::fabs(actual.twist-base.twist)<=6*kDegToRad+1e-8);
            const auto delta=Normalized(pressed.extra[0]*released.extra[0].Inverse());
            EXPECT(std::fabs(delta.W)>1-1e-12); // button never steers the saddle
            Bounded(chains[0],released,bounds[0]);Bounded(chains[0],pressed,bounds[0]);
        }
        const auto full_pinch=tp::Target(chains[0],normal,chains[1].ref[0].translation,{0,1,false},bounds[0]);
        const auto pinch_base=jl::Decompose(bounds[0].axes[0],full_pinch.extra[0]);
        EXPECT(std::fabs(pinch_base.side)<1e-8); // do not force the web outward during pinch
        EXPECT(std::fabs(jl::Decompose(bounds[0].axes[1],pose.extra[1]).flex+bounds[0].limits[1].ref_bend-5*kDegToRad)<1e-7);
        EXPECT(std::fabs(jl::Decompose(bounds[0].axes[2],pose.extra[2]).flex+bounds[0].limits[2].ref_bend-3*kDegToRad)<1e-7);
        // Changing thumb intention cannot mutate the other chains or their
        // independent generated open targets.
        skel::Xf other_before[3],other_after[3]; FVector before_tip,after_tip;
        const auto other=fc::CurlTarget(chains[1],0,bounds[1]);
        fingers::Fk(chains[1],other,other_before,&before_tip);
        for(int mode=0;mode<3;++mode) {
            auto previous=pose;
            for(int step=1;step<=200;++step) {
                const double amount=step/200.0;
                const auto current=tp::Target(chains[0],normal,chains[1].ref[0].translation,
                    {mode!=1?amount:0,mode!=0?amount:0,false},bounds[0]);
                Bounded(chains[0],current,bounds[0]);
                for(int j=0;j<3;++j) {
                    const auto delta=Normalized(current.extra[j]*previous.extra[j].Inverse());
                    EXPECT(std::fabs(delta.W)>.999);
                }
                previous=current;
            }
        }
        fingers::Fk(chains[1],other,other_after,&after_tip);
        EXPECT(Length(after_tip-before_tip)<1e-12);
    }
    // The neutral correction is defined in anatomical hand space, so a
    // different bone roll or mirrored rig cannot change its geometry.
    {
        const auto left=Thumb(true),rolled=Thumb(false,true);
        const auto lb=fc::MakeBounds(left,0),rb=fc::MakeBounds(rolled,0);
        const auto lpose=tp::Target(left,{0,-1,0},{index.X,-index.Y,index.Z},{},lb);
        const auto rpose=tp::Target(rolled,palm,index,{},rb);
        skel::Xf joints[3]; FVector tip,ltip,rtip;
        fingers::Fk(c,open,joints,&tip);
        fingers::Fk(left,lpose,joints,&ltip); fingers::Fk(rolled,rpose,joints,&rtip);
        EXPECT(Length(ltip-FVector{tip.X,-tip.Y,tip.Z})<1e-7);
        EXPECT(Length(rtip-tip)<1e-7);
        Bounded(left,lpose,lb); Bounded(rolled,rpose,rb);
    }
    if(failures) { std::printf("thumb pose: %d failures\n",failures); return 1; }
    std::puts("thumb pose: all tests passed"); return 0;
}
