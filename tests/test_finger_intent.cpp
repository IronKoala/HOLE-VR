#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include "vr/body/finger_intent.h"
#include "vr/body/thumb_pose.h"
#include "fixtures/white_man_hands.h"

using namespace holevr::vr;
using namespace holevr::vr::ik;
namespace fi=finger_intent;
namespace fc=controller_pose;
namespace jl=joint_limits;
static int failures=0;
#define EXPECT(x) do {if(!(x)){std::printf("FAIL %d: %s\n",__LINE__,#x);++failures;}}while(0)

static jl::Turn Turn(const fingers::Chain& c,const fingers::Angles& a,const fc::Bounds& b,int j) {
    return jl::Decompose(b.axes[j],c.ref[j].rotation.Inverse()*fingers::LocalRotation(c,j,a));
}
static double Anatomical(const fingers::Chain& c,const fingers::Angles& a,const fc::Bounds& b,int j) {
    return (Turn(c,a,b,j).flex+b.limits[j].ref_bend)*kRadToDeg;
}
static void Bounded(const fingers::Chain& c,const fingers::Angles& a,const fc::Bounds& b) {
    for(int j=0;j<3;++j) EXPECT(jl::Within(b.axes[j],b.limits[j],
        c.ref[j].rotation.Inverse()*fingers::LocalRotation(c,j,a),1e-7));
}
static double Difference(const fingers::Chain& c,const fingers::Angles& a,const fingers::Angles& b) {
    double worst=0;
    for(int j=0;j<3;++j) {
        const auto q=Normalized(fingers::LocalRotation(c,j,a)*fingers::LocalRotation(c,j,b).Inverse());
        worst=std::max(worst,2*std::atan2(std::sqrt(q.X*q.X+q.Y*q.Y+q.Z*q.Z),std::fabs(q.W)));
    }
    return worst;
}

int main() {
    for(int side=0;side<2;++side) {
        fingers::Chain c[5];fc::Bounds b[5];FVector palm;
        white_man_hands::Build(side,c,b,&palm);
        for(int f=1;f<5;++f) {
            const auto neutral=fi::FreeTarget(c[f],f,{},b[f]);
            Bounded(c[f],neutral,b[f]);
            // Archived Hands1 Curl(.12) adds these angles onto the curved
            // source reference. An absolute 12% target must fail this check.
            const double historical_extra[3]={9.18,10.71,8.16};
            const auto historical=fingers::Curl(c[f],.12,fingers::DefaultLimits(false));
            std::printf("rest match side%d finger%d delta %.12g deg\n",side,f,Difference(c[f],neutral,historical)*kRadToDeg);
            // Anatomical-axis orthogonalization removes ~1e-7 degree of
            // source floating-point skew; it must not change the rest pose.
            EXPECT(Difference(c[f],neutral,historical)<1e-8);
            for(int j=0;j<3;++j) {
                EXPECT(std::fabs(Turn(c[f],neutral,b[f],j).flex*kRadToDeg-historical_extra[j])<1e-7);
                EXPECT(std::fabs(Anatomical(c[f],neutral,b[f],j)-
                    (historical_extra[j]+b[f].limits[j].ref_bend*kRadToDeg))<1e-7);
            }

            // Absolute opening and the original full-close endpoints retain
            // their contracts; relaxation is only the free-hand baseline.
            const auto open=fc::CurlTarget(c[f],0,b[f]);
            EXPECT(Difference(c[f],fi::FreeTarget(c[f],f,{},b[f],1,0),open)<1e-10);
            EXPECT(Difference(c[f],fi::FreeTarget(c[f],f,{},b[f],0),open)<1e-10);
            hands::FingerInput closed;closed.grip=closed.trigger=1;
            for(double scale:{.5,1.,1.2}) {
                const auto target=fi::FreeTarget(c[f],f,closed,b[f],scale);
                Bounded(c[f],target,b[f]);
                EXPECT(Difference(c[f],target,fc::CurlTarget(c[f],scale,b[f]))<1e-10);
                EXPECT(Difference(c[f],fi::FreeTarget(c[f],f,closed,b[f],scale,0),target)<1e-10);
            }

            // Approaching an object removes only passive rest, smoothly and
            // monotonically. It cannot override a deliberate full squeeze.
            double prior_open[3]={1e9,1e9,1e9};
            for(int step=0;step<=100;++step) {
                const auto target=fi::FreeTarget(c[f],f,{},b[f],1,1-step/100.0);
                Bounded(c[f],target,b[f]);
                for(int j=0;j<3;++j) {
                    const double angle=Anatomical(c[f],target,b[f],j);
                    EXPECT(angle<=prior_open[j]+1e-7);prior_open[j]=angle;
                }
            }

            // Every joint advances monotonically with its own control.
            // This also verifies that added rest bend cannot overflow limits.
            for(double gain:{.5,1.,1.2}) {
                double prior[3]={};
                for(int step=0;step<=100;++step) {
                    hands::FingerInput input;
                    if(f==1) input.trigger=step/100.f;else input.grip=step/100.f;
                    const auto target=fi::FreeTarget(c[f],f,input,b[f],gain);
                    Bounded(c[f],target,b[f]);
                    for(int j=0;j<3;++j) {
                        const auto angle=Anatomical(c[f],target,b[f],j);
                        EXPECT(std::isfinite(angle) && (step==0 || angle>=prior[j]-1e-7));
                        if(step) EXPECT(angle-prior[j]<gain);
                        prior[j]=angle;
                    }
                }
            }
            auto restricted=b[f];restricted.limits[1].flex_max=10*kDegToRad-restricted.limits[1].ref_bend;
            const auto limited=fi::FreeTarget(c[f],f,{},restricted);
            Bounded(c[f],limited,restricted);
            EXPECT(Anatomical(c[f],limited,restricted,1)<=10+1e-7);
            hands::FingerInput invalid;invalid.grip=invalid.trigger=std::numeric_limits<float>::quiet_NaN();
            const auto sanitized=fi::FreeTarget(c[f],f,invalid,b[f],std::numeric_limits<double>::infinity());
            EXPECT(Difference(c[f],sanitized,neutral)<1e-10);
        }

        // Grip-only keeps a straight index for pointing. That extension is
        // continuous, while trigger touch still places its pad on the trigger.
        double previous=1e9;
        for(int step=0;step<=100;++step) {
            hands::FingerInput input;input.grip=step/100.f;
            const auto target=fi::FreeTarget(c[1],1,input,b[1]);
            const double angle=Anatomical(c[1],target,b[1],1);
            EXPECT(angle<=previous+1e-7);previous=angle;
        }
        EXPECT(std::fabs(previous)<1e-7);
        hands::FingerInput touch;touch.grip=1;touch.trigger_touch=true;
        EXPECT(Anatomical(c[1],fi::FreeTarget(c[1],1,touch,b[1]),b[1],1)>25);
        hands::FingerInput closing;closing.grip=1;closing.trigger=.5f;
        EXPECT(Anatomical(c[1],fi::FreeTarget(c[1],1,closing,b[1]),b[1],1)>
            Anatomical(c[1],fc::CurlTarget(c[1],.5,b[1]),b[1],1)+1);

        const auto thumb_rest=fi::FreeThumbTarget(c[0],palm,c[1].ref[0].translation,{},b[0]);
        const auto historical_thumb=fingers::Curl(c[0],.15,fingers::DefaultLimits(true));
        std::printf("rest match side%d thumb delta %.12g deg\n",side,Difference(c[0],thumb_rest,historical_thumb)*kRadToDeg);
        EXPECT(Difference(c[0],thumb_rest,historical_thumb)<1e-8);
        const double thumb_extra[3]={4.95,5.85,6.75};
        for(int j=0;j<3;++j) {
            const auto rest_turn=Turn(c[0],thumb_rest,b[0],j);
            std::printf("thumb rest side%d joint%d %.9f/%.12g/%.12g deg\n",side,j,rest_turn.flex*kRadToDeg,rest_turn.side*kRadToDeg,rest_turn.twist*kRadToDeg);
            EXPECT(std::fabs(rest_turn.flex*kRadToDeg-thumb_extra[j])<1e-7);
            EXPECT(std::fabs(rest_turn.side)<1e-8 && std::fabs(rest_turn.twist)<1e-8);
        }
        for(int mode=0;mode<3;++mode) {
            hands::FingerInput input;input.grip=mode!=1;input.trigger=mode!=0;
            const auto raw=thumb_pose::Target(c[0],palm,c[1].ref[0].translation,
                {input.grip,input.trigger,input.thumb_touch},b[0]);
            for(double weight:{0.,.5,1.}) EXPECT(Difference(c[0],raw,
                fi::FreeThumbTarget(c[0],palm,c[1].ref[0].translation,input,b[0],weight))<1e-10);
        }
        const auto thumb_open=thumb_pose::Target(c[0],palm,c[1].ref[0].translation,{},b[0]);
        EXPECT(Difference(c[0],thumb_open,fi::FreeThumbTarget(c[0],palm,c[1].ref[0].translation,{},b[0],0))<1e-10);
        // A new resting frame cannot introduce a threshold snap as actual
        // grip/trigger input takes over, or as the environment opens it.
        for(double weight:{0.,.5,1.})for(int mode=0;mode<3;++mode) {
            auto prior=fi::FreeThumbTarget(c[0],palm,c[1].ref[0].translation,{},b[0],weight);
            for(int step=1;step<=100;++step) {
                hands::FingerInput input;
                input.grip=mode==1?0:step/100.f;input.trigger=mode==0?0:step/100.f;
                const auto target=fi::FreeThumbTarget(c[0],palm,c[1].ref[0].translation,input,b[0],weight);
                EXPECT(Difference(c[0],target,prior)<2.5*kDegToRad);
                Bounded(c[0],target,b[0]);prior=target;
            }
        }
        auto prior_approach=thumb_rest;
        for(int step=1;step<=100;++step) {
            const auto target=fi::FreeThumbTarget(c[0],palm,c[1].ref[0].translation,{},b[0],1-step/100.0);
            EXPECT(Difference(c[0],target,prior_approach)<.5*kDegToRad);
            Bounded(c[0],target,b[0]);prior_approach=target;
        }
        const auto neutral_base=Turn(c[0],thumb_open,b[0],0);
        // Controller face/stick presses are binary proxies, not CMC tracking.
        // Every environmental/input corner must fold only the outer hinges,
        // retaining both full-control endpoints and the fixed root frame.
        for(double weight:{0.,.5,1.})for(double grip:{0.,.1,.5,1.})for(double trigger:{0.,.1,.5,1.}) {
            hands::FingerInput input;input.grip=grip;input.trigger=trigger;
            const auto released=fi::FreeThumbTarget(c[0],palm,c[1].ref[0].translation,input,b[0],weight);
            input.thumb_touch=true;
            const auto pressed=fi::FreeThumbTarget(c[0],palm,c[1].ref[0].translation,input,b[0],weight);
            Bounded(c[0],released,b[0]);Bounded(c[0],pressed,b[0]);
            const auto base=Turn(c[0],released,b[0],0),button_base=Turn(c[0],pressed,b[0],0);
            EXPECT(std::fabs(base.flex-button_base.flex)<1e-9);
            EXPECT(std::fabs(base.side-button_base.side)<1e-9);
            EXPECT(std::fabs(base.twist-button_base.twist)<1e-9);
            const double side_allowance=std::max(weight*std::fabs(neutral_base.side),
                trigger==0?10*kDegToRad:std::max(10*kDegToRad,std::fabs(neutral_base.side)));
            EXPECT(std::fabs(base.side-neutral_base.side)<=side_allowance+1e-8);
            EXPECT(base.side*neutral_base.side>=-1e-8);
            if(neutral_base.side<0)EXPECT(base.side>=neutral_base.side-10*kDegToRad-1e-8);
            else EXPECT(base.side<=neutral_base.side+10*kDegToRad+1e-8);
            EXPECT(std::fabs(base.twist-neutral_base.twist)<=6*kDegToRad+1e-8);
            for(int j=1;j<3;++j) {
                const auto a=Turn(c[0],released,b[0],j),z=Turn(c[0],pressed,b[0],j);
                EXPECT(z.flex>=a.flex-1e-9 && z.flex-a.flex<=(j==1?6:8)*kDegToRad+1e-8);
                EXPECT(std::fabs(z.side-a.side)<1e-9 && std::fabs(z.twist-a.twist)<1e-9);
                if(grip==0 && trigger==0)EXPECT(z.flex-a.flex>(j==1?2:3)*kDegToRad);
            }
            if(grip==1 || trigger==1)EXPECT(Difference(c[0],released,pressed)<1e-9);
            skel::Xf joints[3];fingers::Fk(c[0],pressed,joints,nullptr);
            EXPECT(Length(joints[0].translation-c[0].ref[0].translation)<1e-10);
        }

    }
    return failures ? 1 : 0;
}
