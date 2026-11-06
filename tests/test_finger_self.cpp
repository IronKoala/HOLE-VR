#include <algorithm>
#include <cmath>
#include <cstdio>
#include "vr/body/finger_self.h"
using namespace holevr::vr;
using namespace holevr::vr::ik;
namespace fc=finger_contact;
namespace fs=finger_self;
namespace jl=joint_limits;
static int failures=0;
#define EXPECT(x) do {if(!(x)){std::printf("FAIL %d: %s\n",__LINE__,#x);++failures;}}while(0)
static fingers::Chain Digit(double z,bool thumb,bool mirror) {
    const double sign=mirror?-1:1;
    skel::Xf j[3];j[0].translation={12.3,0,z*sign};j[1].translation={17.0,0,z*sign};j[2].translation={20.6,0,z*sign};
    return fingers::Build(j,{0,1,0},{12.3,0,-5*sign},thumb);
}
static void Bounded(const fingers::Chain& c,const fingers::Angles& a,const fc::Bounds& b) {
    for(int j=0;j<3;++j)EXPECT(jl::Within(b.axes[j],b.limits[j],c.ref[j].rotation.Inverse()*fingers::LocalRotation(c,j,a),1e-6));
}
int main() {
    for(bool mirror:{false,true})for(bool thumb:{false,true})for(double hz:{72.0,90.0,120.0}) {
        fingers::Chain chains[5]{};fingers::Angles poses[5]{},wanted[5]{};
        fc::Bounds bounds[5]{};fc::State states[5]{};fc::Contacts world[5]{};skel::Xf hand;
        const int a=thumb?0:1,b=2;const double sign=mirror?-1:1;
        chains[a]=Digit(-.8,thumb,mirror);chains[b]=Digit(.8,false,mirror);chains[4]=Digit(5,false,mirror);
        for(int f=0;f<5;++f)if(chains[f].valid)bounds[f]=fc::MakeBounds(chains[f],f);
        for(int f=0;f<5;++f)if(chains[f].valid)poses[f]=fc::Solve(chains[f],wanted[f],states[f],hand,bounds[f],world[f],1/hz).pose;
        const auto initial_other=fc::SampleWorld(chains[4],poses[4],hand,2,1);
        wanted[a].extra[0]=Arc(bounds[a].axes[0].bone,{1,0,.25*sign});
        wanted[b].extra[0]=Arc(bounds[b].axes[0].bone,{1,0,-.25*sign});
        wanted[4].flex[0]=.35;wanted[4].flex[1]=.45;wanted[4].flex[2]=.3;
        double maximum=0;bool restricted=false;
        for(int frame=0;frame<static_cast<int>(hz*2);++frame) {
            for(int f=0;f<5;++f)if(chains[f].valid)poses[f]=fc::Solve(chains[f],wanted[f],states[f],hand,bounds[f],world[f],1/hz).pose;
            const auto result=fs::Resolve(chains,poses,states,hand,bounds,world,.4,1/hz);
            maximum=std::max(maximum,fs::Depth(chains,poses,hand,.4));restricted|=result.restricted;
            for(int f=0;f<5;++f)if(chains[f].valid) {
                Bounded(chains[f],poses[f],bounds[f]);
                for(double velocity:states[f].angular_velocity)EXPECT(std::fabs(velocity)<=360*kDegToRad+1e-6);
            }
        }
        EXPECT(maximum<.002);EXPECT(restricted);
        EXPECT(Length(fc::SampleWorld(chains[4],poses[4],hand,2,1)-initial_other)>2);
        std::printf("self %s %s %.0f Hz: maximum %.6f cm\n",thumb?"thumb/index":"adjacent",mirror?"mirrored":"right",hz,maximum);
        // Opening after blocked splay must release both involved digits.
        for(int f=0;f<5;++f)wanted[f]={};
        for(int frame=0;frame<static_cast<int>(hz);++frame) {
            for(int f=0;f<5;++f)if(chains[f].valid)poses[f]=fc::Solve(chains[f],wanted[f],states[f],hand,bounds[f],world[f],1/hz).pose;
            fs::Resolve(chains,poses,states,hand,bounds,world,.4,1/hz);
        }
        EXPECT(Length(fc::SampleWorld(chains[a],poses[a],hand,2,1)-fc::SampleWorld(chains[a],{},hand,2,1))<.002);
    }
    // World evidence must never be sacrificed to fix an impossible self
    // overlap at immovable roots. Nonzero residual is reported honestly.
    {
        fingers::Chain c[5]{};fingers::Angles a[5]{};fc::Bounds b[5]{};fc::State s[5]{};fc::Contacts w[5]{};skel::Xf hand;
        c[1]=Digit(0,false,false);c[2]=Digit(.2,false,false);
        for(int f:{1,2}) {b[f]=fc::MakeBounds(c[f],f);a[f]=fc::Solve(c[f],{},s[f],hand,b[f],w[f],1.0/90).pose;}
        auto& wall=w[1].items[0];w[1].count=1;wall.valid=true;wall.segment=2;wall.fraction=1;wall.radius_cm=.4;wall.patch_radius_cm=20;
        wall.normal={0,-1,0};wall.point={0,.4,0};
        const double before=fc::Depth(c[1],a[1],hand,w[1]);
        const auto result=fs::Resolve(c,a,s,hand,b,w,.4,1.0/90);
        EXPECT(result.residual_cm>.5);EXPECT(fc::Depth(c[1],a[1],hand,w[1])<=before+1e-6);
    }
    if(failures){std::printf("finger self: %d failures\n",failures);return 1;}
    std::puts("finger self: all tests passed");return 0;
}
