#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include "vr/body/finger_contact.h"
#include "fixtures/white_man_hands.h"
using namespace holevr::vr;
using namespace holevr::vr::ik;
namespace fc=finger_contact;
namespace jl=joint_limits;
static int failures=0;
#define EXPECT(x) do {if(!(x)){std::printf("FAIL %d: %s\n",__LINE__,#x);++failures;}}while(0)
static fingers::Chain Finger(bool mirror=false) {
    skel::Xf j[3];j[1].translation={4,0,0};j[2].translation={7,0,0};
    return fingers::Build(j,{0,mirror?-1.0:1.0,0},{0,0,-4},false);
}
static fingers::Angles Wanted(){fingers::Angles a;a.flex[0]=35*kDegToRad;a.flex[1]=65*kDegToRad;a.flex[2]=45*kDegToRad;return a;}
static fc::Contact Press(const fingers::Chain& c,const fingers::Angles& a,const skel::Xf& hand,
                         int segment,double fraction,FVector normal,double depth,double patch=5) {
    fc::Contact contact;contact.valid=true;contact.segment=segment;contact.fraction=fraction;
    contact.radius_cm=.4;contact.patch_radius_cm=patch;contact.normal=Normalize(normal);
    contact.point=fc::SampleWorld(c,a,hand,segment,fraction)+contact.normal*(depth-contact.radius_cm);return contact;
}
static jl::Turn Joint(const fingers::Chain& c,const fingers::Angles& a,const fc::Bounds& b,int j) {
    return jl::Decompose(b.axes[j],c.ref[j].rotation.Inverse()*fingers::LocalRotation(c,j,a));
}
static double PoseDifference(const fingers::Chain& c,const fingers::Angles& a,const fingers::Angles& b) {
    double worst=0;for(int j=0;j<3;++j) {
        const auto q=Normalized(fingers::LocalRotation(c,j,a)*fingers::LocalRotation(c,j,b).Inverse());
        worst=std::max(worst,2*std::atan2(std::sqrt(q.X*q.X+q.Y*q.Y+q.Z*q.Z),std::fabs(q.W)));
    }return worst;
}
static void Bounds(const fingers::Chain& c,const fingers::Angles& a,const fc::Bounds& b) {
    for(int j=0;j<3;++j)EXPECT(jl::Within(b.axes[j],b.limits[j],c.ref[j].rotation.Inverse()*fingers::LocalRotation(c,j,a),1e-7));
}
int main() {
    const auto c=Finger();const auto b=fc::MakeBounds(c,2);const auto wanted=Wanted();skel::Xf hand;
    fc::Contacts contact;contact.count=1;contact.items[0]=Press(c,wanted,hand,2,1,{1,0,0},.8);
    // World intrusion cannot manufacture curl. Keep the requested mechanical
    // shape and expose its real root debt; translating the root must clear it.
    for(double hz:{35.0,40.0,45.0,72.0,90.0,120.0}) {
        fc::State state;fc::Solve(c,wanted,state,hand,b,{},1/hz);
        double largest_angle=0;
        for(int frame=0;frame<int(hz*5);++frame) {
            const auto result=fc::Solve(c,wanted,state,hand,b,contact,1/hz);
            EXPECT(result.root_required && result.blocked);EXPECT(std::fabs(result.residual_cm-.8)<1e-8);
            largest_angle=std::max(largest_angle,PoseDifference(c,wanted,result.pose));Bounds(c,result.pose,b);
        }
        EXPECT(largest_angle<1e-8);
        auto corrected=hand;const auto witness=fc::EvaluateContact(c,state.pose,hand,contact.items[0]);
        EXPECT(witness.active && std::fabs(witness.depth_cm-.8)<1e-8);
        corrected.translation=corrected.translation+witness.normal*witness.depth_cm;
        const auto clear=fc::ResolvePrepared(c,state.pose,state,corrected,b,contact);
        EXPECT(clear.residual_cm<1e-7 && !clear.root_required);
        EXPECT(PoseDifference(c,wanted,clear.pose)<1e-8);
        std::printf("firm pressure %.0f Hz: invented curl %.8f deg, cleared root debt %.3f cm\n",hz,largest_angle*kRadToDeg,witness.depth_cm);
    }
    // Arrest only intended obstructed motion: the distal joint stops while
    // proximal input changes remain independent. No compulsory DIP:PIP ratio.
    {
        fc::State state;auto open=wanted;open.flex[2]=20*kDegToRad;
        fc::Solve(c,open,state,hand,b,{},1.0/35);
        auto target=wanted;target.flex[0]+=2*kDegToRad;target.flex[1]+=2*kDegToRad;
        auto plane=contact;plane.items[0]=Press(c,target,hand,2,1,{1,0,0},.4);
        for(int n=0;n<140;++n) {
            const auto result=fc::Solve(c,target,state,hand,b,plane,1.0/35);
            EXPECT(result.residual_cm<=.0011);Bounds(c,result.pose,b);
        }
        const auto base=Joint(c,state.pose,b,0),middle=Joint(c,state.pose,b,1),tip=Joint(c,state.pose,b,2);
        EXPECT(base.flex>36*kDegToRad && middle.flex>66*kDegToRad);
        EXPECT(tip.flex<44*kDegToRad && tip.flex>=20*kDegToRad);
        EXPECT(std::fabs(tip.flex-.7*middle.flex)>4*kDegToRad);
        std::printf("arrested input: %.3f/%.3f/%.3f deg\n",base.flex*kRadToDeg,middle.flex*kRadToDeg,tip.flex*kRadToDeg);
        for(int n=0;n<70;++n)fc::Solve(c,target,state,hand,b,{},1.0/35);
        EXPECT(PoseDifference(c,state.pose,target)<1e-5);
    }
    // Preview and Prepare produce precisely one budget/easing step; a later
    // contact resolution cannot consume a second allowance.
    {
        fc::State state;fc::Solve(c,{},state,hand,b,{},1.0/35);
        const auto before=state.pose;const auto preview=fc::Preview(c,wanted,state,b,1.0/35);
        EXPECT(PoseDifference(c,before,state.pose)<1e-9);
        const auto prepared=fc::Prepare(c,wanted,state,b,1.0/35);
        EXPECT(PoseDifference(c,preview,prepared)<1e-9);
        for(int retry=0;retry<8;++retry) {
            const auto pending=fc::Prepare(c,prepared,state,b,1.0/35,false,true);
            EXPECT(PoseDifference(c,pending,prepared)<1e-9);
        }
        const auto resolved=fc::ResolvePrepared(c,prepared,state,hand,b,{});
        EXPECT(PoseDifference(c,preview,resolved.pose)<1e-9);
        EXPECT(PoseDifference(c,before,resolved.pose)<=240*kDegToRad/35+1e-8);
        const auto extra=fc::ResolveContacts(c,state,hand,b,{});
        EXPECT(PoseDifference(c,resolved.pose,extra.pose)<1e-9);
        EXPECT(PoseDifference(c,fc::Solve(c,{},state,hand,b,{},0).pose,extra.pose)<1e-9);
    }
    // Local geometry is invariant under a world rotation, mirror and scale;
    // stale/invalid/distant patches create no invented obstacle.
    for(bool mirror:{false,true}) {
        const auto digit=Finger(mirror);const auto limits=fc::MakeBounds(digit,2);
        skel::Xf transform;transform.rotation=ToQuat({25,72,-38});transform.translation={80,-30,120};
        const auto normal=transform.rotation.Rotate({1,0,0});auto observed=contact;
        observed.items[0]=Press(digit,wanted,transform,2,1,normal,.8);
        fc::State state;fc::Solve(digit,wanted,state,transform,limits,{},1.0/35);
        EXPECT(std::fabs(fc::Solve(digit,wanted,state,transform,limits,observed,1.0/35).residual_cm-.8)<1e-8);
        observed.items[0].age_seconds=.076;
        EXPECT(fc::Solve(digit,wanted,state,transform,limits,observed,1.0/35).active_contacts==0);
        observed.items[0].age_seconds=0;observed.items[0].normal.X=std::numeric_limits<double>::quiet_NaN();
        EXPECT(!fc::EvaluateContact(digit,wanted,transform,observed.items[0]).active);
    }
    // Real WhiteMan CMC roots protrude beyond the old 1.5 cm palm face. The
    // measured 1.104 cm debt must move the root, not the thumb's joint angles.
    for(int side=0;side<2;++side) {
        fingers::Chain chains[5];fc::Bounds limits[5];FVector palm;
        white_man_hands::Build(side,chains,limits,&palm);
        fc::Contact root;root.valid=true;root.segment=0;root.fraction=0;root.point_sample_only=true;
        root.point=palm*1.53;root.normal=palm*-1;root.radius_cm=.5;root.patch_radius_cm=30;
        const auto witness=fc::EvaluateContact(chains[0],{},hand,root);
        EXPECT(witness.active && witness.depth_cm>1.09 && witness.depth_cm<1.12);
        auto moved=hand;moved.translation=root.normal*witness.depth_cm;
        EXPECT(fc::EvaluateContact(chains[0],{},moved,root).depth_cm<1e-8);
        std::printf("actual %s CMC root debt %.6f cm\n",side?"right":"left",witness.depth_cm);
        for(int f=1;f<5;++f) {
            const auto open=fc::CurlTarget(chains[f],0,limits[f]);Bounds(chains[f],open,limits[f]);
            for(int j=0;j<3;++j)EXPECT(std::fabs(Joint(chains[f],open,limits[f],j).flex+limits[f].limits[j].ref_bend)<1e-7);
            const double station=Dot(fc::SampleWorld(chains[f],open,hand,2,1),palm);
            EXPECT(station<1.5);
        }
    }
    // Input still respects all anatomical bounds, including full quaternions.
    {
        auto over=wanted;over.flex[1]=150*kDegToRad;over.extra[2]=AxisAngle({1,0,0},.8);
        fc::State state;const auto result=fc::Solve(c,over,state,hand,b,{},1.0/35);Bounds(c,result.pose,b);
    }
    // Stationary pressure cost: production functions, no query backend.
    for(int count:{0,1,9,16}) {
        fc::Contacts contacts;contacts.count=count;
        for(int i=0;i<count;++i)contacts.items[i]=contact.items[0];
        fc::State state;fc::Solve(c,wanted,state,hand,b,{},1.0/35);
        const auto start=std::chrono::steady_clock::now();double debt=0;
        for(int n=0;n<2000;++n)debt+=fc::Solve(c,wanted,state,hand,b,contacts,1.0/35).residual_cm;
        const double us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count()/2000;
        std::printf("firm solver %d contacts: %.3f us/finger, accumulated debt %.3f\n",count,us,debt);
    }
    if(failures){std::printf("finger contact: %d failures\n",failures);return 1;}
    std::puts("finger contact: all tests passed");return 0;
}
