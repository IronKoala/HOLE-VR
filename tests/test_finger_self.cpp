#include <algorithm>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <vector>
#include "vr/body/finger_self.h"
#include "vr/body/thumb_pose.h"
#include "fixtures/white_man_hands.h"
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
    // A fitted thick pad is not a uniformly swollen finger. Its measured
    // lateral width permits neighboring digits while padward thickness still
    // blocks overlap after a bone-local roll. Transform scale is applied once.
    {
        fingers::Chain c[5]{};fingers::Angles a[5]{};skel::Xf hand;
        c[1]=Digit(-.8,false,false);c[2]=Digit(.8,false,false);
        for(int f:{1,2})for(auto& shape:c[f].collision) {
            shape.fitted=true;shape.world_radius_cm=1.3;
            shape.side_axis={0,0,1};shape.depth_axis={0,1,0};shape.axial_axis={1,0,0};
            shape.side_half_cm=.6;shape.depth_half_cm=1.3;shape.axial_half_cm=.6;
        }
        EXPECT(fs::Depth(c,a,hand,.5)<1e-8);
        hand.scale={2,2,2};hand.rotation=AxisAngle(Normalize({1,2,3}),.7);
        EXPECT(fs::Depth(c,a,hand,.5)<1e-8);
        hand={};
        for(int f:{1,2})c[f].ref[0].rotation=AxisAngle({1,0,0},90*kDegToRad);
        EXPECT(fs::Depth(c,a,hand,.5)>.9);
    }
    // Only the proximal shared web is excluded from self tests. Fitting does
    // not move the full centerline used by world queries or reduce its radius.
    {
        fingers::Chain c[5]{};fingers::Angles a[5]{};skel::Xf hand;
        c[1]=Digit(-.4,false,false);c[2]=Digit(.4,false,false);
        c[1].ref[0].rotation=Arc({1,0,0},{1,0,-.25});
        c[2].ref[0].rotation=Arc({1,0,0},{1,0,.25});
        for(int f:{1,2})for(auto& shape:c[f].collision) {
            shape.fitted=true;shape.world_radius_cm=1.3;
            shape.side_axis={0,0,1};shape.depth_axis={0,1,0};shape.axial_axis={1,0,0};
            shape.side_half_cm=.6;shape.depth_half_cm=1.3;shape.axial_half_cm=.6;
        }
        EXPECT(fs::Depth(c,a,hand,.5)>.3);
        for(int f:{1,2})c[f].collision[0].self_start_fraction=.5;
        EXPECT(fs::Depth(c,a,hand,.5)<1e-8);
        EXPECT(Length(c[1].ref[0].translation-c[2].ref[0].translation)<.81);
        EXPECT(c[1].collision[0].world_radius_cm==1.3);
    }
    // Pure roll changes a fitted surface even when its bone centerline does
    // not move. Continuous separation must account for that surface travel.
    {
        fingers::Chain c[5]{};fingers::Angles a[5]{},wanted[5]{};fc::Bounds b[5]{};
        fc::State state[5]{};fc::Contacts world[5]{};skel::Xf hand;
        c[1]=Digit(-.65,true,false);c[2]=Digit(.65,true,false);
        for(int f:{1,2}) {
            b[f]=fc::MakeBounds(c[f],0);
            wanted[f].extra[0]=jl::Compose(b[f].axes[0],{0,0,(f==1?20:-20)*kDegToRad});
            for(auto& shape:c[f].collision) {
                shape.fitted=true;shape.world_radius_cm=1.3;
                shape.side_axis={0,0,1};shape.depth_axis={0,1,0};shape.axial_axis={1,0,0};
                shape.side_half_cm=.6;shape.depth_half_cm=1.3;shape.axial_half_cm=.6;
            }
        }
        EXPECT(fs::Depth(c,wanted,hand,.5)>.05);
        bool restricted=false;
        for(int frame=0;frame<70;++frame) {
            for(int f:{1,2})a[f]=fc::Prepare(c[f],wanted[f],state[f],b[f],1.0/35);
            const auto result=fs::Resolve(c,a,state,hand,b,world,.5,1.0/35);
            EXPECT(result.feasible);EXPECT(fs::Depth(c,a,hand,.5)<1e-7);restricted|=result.restricted;
            for(int step=0;step<=10;++step) {
                fingers::Angles path[5]{};
                for(int f:{1,2})for(int j=0;j<3;++j) {
                    const auto q0=jl::Decompose(b[f].axes[j],state[f].frame_start.extra[j]);
                    const auto q1=jl::Decompose(b[f].axes[j],a[f].extra[j]);const double t=step/10.0;
                    path[f].extra[j]=jl::Compose(b[f].axes[j],{q0.flex+(q1.flex-q0.flex)*t,
                        q0.side+(q1.side-q0.side)*t,q0.twist+(q1.twist-q0.twist)*t});
                }
                EXPECT(fs::Depth(c,path,hand,.5)<1e-7);
            }
        }
        EXPECT(restricted);
    }
    for(bool mirror:{false,true})for(bool thumb:{false,true})for(double hz:{35.0,40.0,45.0,72.0,90.0,120.0}) {
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
    // Impossible reference roots must not be labelled feasible/clear.
    {
        fingers::Chain c[5]{};fingers::Angles a[5]{};fc::Bounds b[5]{};fc::State s[5]{};fc::Contacts w[5]{};skel::Xf hand;
        c[1]=Digit(0,false,false);c[2]=Digit(.2,false,false);
        for(int f:{1,2}) {b[f]=fc::MakeBounds(c[f],f);a[f]=fc::Solve(c[f],{},s[f],hand,b[f],w[f],1.0/90).pose;}
        auto& wall=w[1].items[0];w[1].count=1;wall.valid=true;wall.segment=2;wall.fraction=1;wall.radius_cm=.4;wall.patch_radius_cm=20;
        wall.normal={0,-1,0};wall.point={0,.4,0};
        const double before=fc::Depth(c[1],a[1],hand,w[1]);
        const auto result=fs::Resolve(c,a,s,hand,b,w,.4,1.0/90);
        EXPECT(!result.feasible && result.residual_cm>.5);EXPECT(fc::Depth(c[1],a[1],hand,w[1])<=before+1e-6);
    }
    // Clear endpoints can conceal a fast side-swap. Certify the complete
    // path from the reference seed even on the first/reset frame.
    {
        fingers::Chain c[5]{};fingers::Angles a[5]{},wanted[5]{};fc::Bounds b[5]{};
        fc::State state[5]{};fc::Contacts world[5]{};skel::Xf hand;
        c[1]=Digit(-.8,false,false);c[2]=Digit(.8,false,false);
        for(int f:{1,2})b[f]=fc::MakeBounds(c[f],f);
        wanted[1].extra[0]=Arc(b[1].axes[0].bone,{1,0,.35});
        wanted[2].extra[0]=Arc(b[2].axes[0].bone,{1,0,-.35});
        for(int f:{1,2})a[f]=fc::Prepare(c[f],wanted[f],state[f],b[f],1.0/35);
        const auto result=fs::Resolve(c,a,state,hand,b,world,.4,1.0/35);
        EXPECT(result.feasible && result.restricted && result.residual_cm<1e-8);
        EXPECT(fc::SampleWorld(c[1],a[1],hand,2,1).Z<fc::SampleWorld(c[2],a[2],hand,2,1).Z);
    }
    // Self-clear poses take priority over a world constraint that demands a
    // crossing. Expose that debt to the rigid hand root instead of accepting
    // overlap. A common translation clears it without changing separation.
    {
        fingers::Chain c[5]{};fingers::Angles a[5]{},wanted[5]{};fc::Bounds b[5]{};
        fc::State state[5]{};fc::Contacts world[5]{};skel::Xf hand;
        c[1]=Digit(-.8,false,false);c[2]=Digit(.8,false,false);
        for(int f:{1,2}) {
            b[f]=fc::MakeBounds(c[f],f);
            a[f]=fc::Solve(c[f],{},state[f],hand,b[f],world[f],1.0/35).pose;
            wanted[f].extra[0]=Arc(b[f].axes[0].bone,{1,0,f==1?.25:-.25});
        }
        bool exercised=false;
        for(int n=0;n<20 && !exercised;++n) {
            for(int f:{1,2})a[f]=fc::Prepare(c[f],wanted[f],state[f],b[f],1.0/35);
            if(fs::Depth(c,a,hand,.4)>.01) {
                auto& wall=world[1].items[0];world[1].count=1;wall.valid=true;wall.segment=2;wall.fraction=1;
                wall.point_sample_only=true;wall.radius_cm=.4;wall.patch_radius_cm=2;wall.normal={0,0,1};
                wall.point=fc::SampleWorld(c[1],a[1],hand,2,1)-wall.normal*.4;
                const auto result=fs::Resolve(c,a,state,hand,b,world,.4,1.0/35);
                EXPECT(result.feasible && result.residual_cm<1e-8 && result.world_residual_cm>.001);
                hand.translation=wall.normal*result.world_residual_cm;
                EXPECT(fc::Depth(c[1],a[1],hand,world[1])<1e-7);
                EXPECT(fs::Depth(c,a,hand,.4)<1e-8);exercised=true;
            } else {
                fs::Resolve(c,a,state,hand,b,world,.4,1.0/35);
                for(int f:{1,2})state[f].initialized=true;
            }
        }
        EXPECT(exercised);
    }
    // Actual WhiteMan CMC positions/reference rotations, including realistic
    // opposition, at the refresh rates measured in the failing live session.
    for(int gates:{1,2})for(bool fitted:{false,true})for(int side=0;side<2;++side)
      for(double hz:{35.0,40.0,45.0,72.0,90.0,120.0}) {
        fingers::Chain c[5];fc::Bounds b[5];FVector palm;white_man_hands::Build(side,c,b,&palm);
        if(fitted)EXPECT(hand_collision_shape::FitWhiteMan(c,palm));
        fingers::Angles a[5]{},desired[5]{},open[5]{};fc::State state[5]{};fc::Contacts world[5]{};skel::Xf hand;
        for(int f=0;f<5;++f)open[f]=fc::CurlTarget(c[f],0,b[f]);
        open[0]=thumb_pose::Target(c[0],palm,c[1].ref[0].translation,{0,0,true},b[0]);
        double grip_travel[5]{};
        const double reference=fs::Depth(c,a,hand,.5);EXPECT(reference<1e-8);
        double worst=0,microseconds=0,worst_release=0;bool release_ready=false;
        int restricted=0,calls=0;std::vector<double> frame_times;
        for(int frame=0;frame<int(hz*24);++frame) {
            const double t=std::fmod(frame/hz,8.0);
            if(t<6.7)release_ready=false;
            const double grip=std::clamp((t-.5)/1.0,0.0,1.0)*(1-std::clamp((t-2.5)/1.0,0.0,1.0));
            const double trigger=std::clamp((t-4)/.7,0.0,1.0)*(1-std::clamp((t-6)/.7,0.0,1.0));
            for(int f=0;f<5;++f) {
                desired[f]=fc::CurlTarget(c[f],f==1?std::max(grip,trigger):grip,b[f]);
            }
            desired[0]=thumb_pose::Target(c[0],palm,c[1].ref[0].translation,{grip,trigger,true},b[0]);
            for(int f=0;f<5;++f)a[f]=fc::Prepare(c[f],desired[f],state[f],b[f],1/hz);
            const auto started=std::chrono::steady_clock::now();
            const auto pre=fs::Resolve(c,a,state,hand,b,world,.5,1/hz);EXPECT(pre.feasible);restricted+=pre.restricted;
            double frame_time=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-started).count();++calls;
            // A world plane through the proximal hand creates rigid-root
            // debt. It must never authorize actual finger overlap.
            for(int f=0;f<5;++f) {
                world[f]={};
                if(t>1.5 && t<3) {
                    auto& contact=world[f].items[0];world[f].count=1;
                    contact.valid=true;contact.segment=0;contact.fraction=0;contact.point_sample_only=true;
                    contact.normal=palm*-1;contact.point=c[f].ref[0].translation+palm*.25;
                    contact.radius_cm=.5;contact.patch_radius_cm=5;
                }
                a[f]=fc::ResolvePrepared(c[f],a[f],state[f],hand,b[f],world[f]).pose;
            }
            if(gates==2) {
                const auto last=std::chrono::steady_clock::now();
                const auto final=fs::Resolve(c,a,state,hand,b,world,.5,1/hz);EXPECT(final.feasible);
                frame_time+=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-last).count();++calls;
            }
            microseconds+=frame_time;frame_times.push_back(frame_time);
            worst=std::max(worst,fs::Depth(c,a,hand,.5));
            for(int f=0;f<5;++f)Bounded(c[f],a[f],b[f]);
            if(grip>.95)for(int f=0;f<5;++f)grip_travel[f]=std::max(grip_travel[f],
                Length(fc::SampleWorld(c[f],a[f],hand,2,1)-fc::SampleWorld(c[f],open[f],hand,2,1)));
            if(t>=6.7) {
                double remaining=0;
                for(int f=0;f<5;++f)remaining=std::max(remaining,Length(fc::SampleWorld(c[f],a[f],hand,2,1)-fc::SampleWorld(c[f],desired[f],hand,2,1)));
                if(!release_ready && remaining<.01){release_ready=true;worst_release=std::max(worst_release,t-6.7);}
                if(t>7.95)EXPECT(remaining<.002);
            }
        }
        EXPECT(worst<1e-6);
        for(int f=0;f<5;++f)EXPECT(Length(fc::SampleWorld(c[f],a[f],hand,2,1)-fc::SampleWorld(c[f],desired[f],hand,2,1))<.002);
        std::sort(frame_times.begin(),frame_times.end());
        EXPECT(release_ready);
        EXPECT(worst_release<.6);EXPECT(grip_travel[0]>1);
        for(int f=1;f<5;++f)EXPECT(grip_travel[f]>3);
        std::printf("actual %s %s %.0f Hz: reference %.6f, worst %.6f cm, restrictions %d, release %.3f s, grip travel %.2f/%.2f/%.2f/%.2f/%.2f cm, self %.2f us/call; %d-pass frame p95 %.2f/max %.2f us\n",
                    fitted?"fitted":"scalar",side?"right":"left",hz,reference,worst,restricted,worst_release,
                    grip_travel[0],grip_travel[1],grip_travel[2],grip_travel[3],grip_travel[4],microseconds/calls,gates,
                    frame_times[static_cast<std::size_t>(.95*(frame_times.size()-1))],frame_times.back());
    }
    // Valid actual-hand interlocks found by deterministic adversarial search.
    // The previous hard gate left these 10.986 / 9.526 / 14.093 cm from open.
    // Reopen without grabbing, resetting state, shrinking capsules or changing
    // anatomy. The right fixtures mirror side/twist in the rig-local frames.
    const double interlocks[3][2][3][3]={
        {{{.283937580,-.376535806,-.147164118},{.549370409,-.109497046,.009497082},{.446993875,0,0}},
         {{.629628816,.147834831,.035898392},{1.057226437,0,0},{1.208710004,0,0}}},
        {{{.399116801,.058531912,.013441848},{.333829963,-.093859867,.070931981},{1.151689784,0,0}},
         {{.082506755,-.209387953,-.107715295},{1.644761469,0,0},{1.515767255,0,0}}},
        {{{.677975483,.444870345,.015306851},{-.281196880,-.021472673,-.130075745},{1.249341181,0,0}},
         {{1.187556903,-.152051121,.087878553},{1.344939891,0,0},{1.149261679,0,0}}}
    };
    for(int index=0;index<3;++index)for(int side=0;side<2;++side)for(double hz:{35.0,45.0,90.0}) {
        fingers::Chain c[5];fc::Bounds b[5];FVector palm;white_man_hands::Build(side,c,b,&palm);
        fingers::Angles a[5]{},goal[5]{};fc::State state[5]{};fc::Contacts world[5]{};skel::Xf hand;
        for(int f=0;f<2;++f)for(int j=0;j<3;++j) {
            const auto& q=interlocks[index][f][j];const double mirror=side?-1:1;
            a[f].extra[j]=jl::Compose(b[f].axes[j],{q[0],q[1]*mirror,q[2]*mirror});
        }
        EXPECT(fs::Depth(c,a,hand,.5)<1e-7);
        for(int f=0;f<5;++f) {state[f].initialized=true;state[f].pose=a[f];goal[f]=fc::CurlTarget(c[f],0,b[f]);}
        goal[0]=thumb_pose::Target(c[0],palm,c[1].ref[0].translation,{0,0,true},b[0]);
        std::vector<double> timings;double worst=0;int yielded=0;
        for(int frame=0;frame<int(hz*4);++frame) {
            for(int f=0;f<5;++f)a[f]=fc::Prepare(c[f],goal[f],state[f],b[f],1/hz);
            const auto start=std::chrono::steady_clock::now();
            auto result=fs::Resolve(c,a,state,hand,b,world,.5,1/hz);EXPECT(result.feasible);yielded+=result.yielded;
            double time=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();
            for(int f=0;f<5;++f)a[f]=fc::ResolvePrepared(c[f],a[f],state[f],hand,b[f],world[f]).pose;
            const auto again=std::chrono::steady_clock::now();
            result=fs::Resolve(c,a,state,hand,b,world,.5,1/hz);EXPECT(result.feasible);yielded+=result.yielded;
            time+=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-again).count();timings.push_back(time);
            worst=std::max(worst,fs::Depth(c,a,hand,.5));
            for(int f=0;f<5;++f)Bounded(c[f],a[f],b[f]);
        }
        double remaining=0,total=0;
        for(int f=0;f<5;++f)remaining=std::max(remaining,Length(fc::SampleWorld(c[f],a[f],hand,2,1)-fc::SampleWorld(c[f],goal[f],hand,2,1)));
        for(double time:timings)total+=time;std::sort(timings.begin(),timings.end());
        EXPECT(worst<1e-7);EXPECT(remaining<.01);EXPECT(yielded>0);
        std::printf("interlock %d %s %.0f Hz: overlap %.6f cm, open error %.6f cm, yielded %d; 2-pass mean %.2f/p95 %.2f/max %.2f us\n",
            index,side?"right":"left",hz,worst,remaining,yielded,total/timings.size(),
            timings[static_cast<std::size_t>(.95*(timings.size()-1))],timings.back());
    }
    if(failures){std::printf("finger self: %d failures\n",failures);return 1;}
    std::puts("finger self: all tests passed");return 0;
}
