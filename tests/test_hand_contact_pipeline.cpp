#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "vr/body/finger_world_query.h"
#include "vr/body/thumb_pose.h"
#include "vr/body/finger_self.h"

using namespace holevr::vr;
using namespace holevr::vr::ik;
namespace fw=finger_world_query;
namespace fc=finger_contact;
namespace jl=joint_limits;
static int failures=0;
#define EXPECT(x) do { if (!(x)) { std::printf("FAIL %d: %s\n",__LINE__,#x); ++failures; } } while (0)

struct Sphere { FVector center; double radius; };
struct Plane { FVector point,normal; };
// Actual analytical sphere sweeps against test primitives. This backend
// does not fabricate solver contacts or choose their finger sample points.
struct World final : fw::Backend {
    std::vector<Sphere> spheres;
    std::vector<Plane> planes;
    bool available=true;
    fw::QueryStatus Sweep(const FVector& from,const FVector& to,double radius,fw::Hit& hit) override {
        if(!available) return fw::QueryStatus::unavailable;
        double nearest=2;
        const FVector movement=to-from;
        auto record=[&](double t,const FVector& normal,const FVector& point,bool initial) {
            if(t<nearest) { nearest=t; hit={from+movement*t,point,normal,initial}; }
        };
        for(const auto& s:spheres) {
            const FVector m=from-s.center;
            const double expanded=s.radius+radius;
            const double distance=Dot(m,m)-expanded*expanded;
            if(distance<=0) { record(0,Normalize(m,{0,0,1}),from,true); continue; }
            const double a=Dot(movement,movement),b=Dot(m,movement),disc=b*b-a*distance;
            if(a<=1e-12 || b>=0 || disc<0) continue;
            const double t=(-b-std::sqrt(disc))/a;
            if(t<0 || t>1) continue;
            const auto normal=Normalize(from+movement*t-s.center);
            record(t,normal,s.center+normal*s.radius,false);
        }
        for(const auto& p:planes) {
            const double start=Dot(from-p.point,p.normal)-radius;
            const double end=Dot(to-p.point,p.normal)-radius;
            if(start<=0) { record(0,p.normal,from,true); continue; }
            if(end>0 || end>=start) continue;
            const double t=start/(start-end);
            record(t,p.normal,from+movement*t-p.normal*radius,false);
        }
        return nearest<=1 ? fw::QueryStatus::hit : fw::QueryStatus::clear;
    }
};

static fingers::Chain Finger(double z=0) {
    skel::Xf j[3];
    j[0].translation={0,0,z};j[1].translation={4,0,z};j[2].translation={7,0,z};
    return fingers::Build(j,{0,1,0},{0,0,-4},false);
}
struct Pipeline {
    fingers::Chain chains[5]{};
    fingers::Angles desired[5]{},resolved[5]{};
    fc::Bounds bounds[5]{};
    fc::State fingers[5]{};
    fc::Contacts contacts[5]{};
    fc::Result solved[5]{};
    fw::State query;
    skel::Xf hand;
    double radius=.4;
    fw::Stats Step(World& scene,double dt=1.0/90) {
        auto stats=fw::Update(query,fw::MakeGeometry(chains,desired,hand,radius),dt,scene,contacts);
        for(int f=0;f<5;++f) if(chains[f].valid) {
            // Match runtime's unavailable-query policy: retain displayed
            // angles for this frame; a failed observation adds no contact.
            if(!stats.available && fingers[f].initialized) continue;
            solved[f]=fc::Solve(chains[f],desired[f],fingers[f],hand,bounds[f],contacts[f],dt,stats.reset);
            resolved[f]=solved[f].pose;
            for(int j=0;j<3;++j) {
                const FQuat q=chains[f].ref[j].rotation.Inverse()*fingers::LocalRotation(chains[f],j,resolved[f]);
                EXPECT(jl::Within(bounds[f].axes[j],bounds[f].limits[j],q,1e-6));
            }
        }
        if(stats.available) finger_self::Resolve(chains,resolved,fingers,hand,bounds,contacts,radius,dt);
        fw::Commit(query,fw::MakeGeometry(chains,resolved,hand,radius));
        return stats;
    }
    void Add(int f,const fingers::Chain& chain) { chains[f]=chain; bounds[f]=fc::MakeBounds(chain,f); }
    void Reset() { fw::Reset(query); for(auto& s:fingers) fc::Reset(s); }
};
static double PlaneDepth(const Pipeline& p,int finger,const Plane& plane) {
    const auto geometry=fw::MakeGeometry(p.chains,p.resolved,p.hand,p.radius);
    double depth=0;
    for(int n=0;n<4;++n)
        depth=std::max(depth,p.radius-Dot(geometry.point[finger][n]-plane.point,plane.normal));
    return depth;
}
static double SphereDepth(const Pipeline& p,int finger,const Sphere& sphere) {
    const auto geometry=fw::MakeGeometry(p.chains,p.resolved,p.hand,p.radius);
    double depth=0;
    for(int j=0;j<3;++j) {
        const auto a=geometry.point[finger][j],b=geometry.point[finger][j+1],delta=b-a;
        const double l2=Dot(delta,delta);
        const double t=l2>1e-9?std::clamp(Dot(sphere.center-a,delta)/l2,0.0,1.0):0;
        depth=std::max(depth,p.radius+sphere.radius-Length(a+delta*t-sphere.center));
    }
    return depth;
}
static FVector Tip(const Pipeline& p,int f) { return fc::SampleWorld(p.chains[f],p.resolved[f],p.hand,2,1); }

int main() {
    // Acquire a wall while already requesting a penetrating pose. There is
    // no earlier clear render pose to conceal a broken spatial-contact path.
    {
        World scene; scene.planes.push_back({{0,4.5,0},{0,-1,0}});
        Pipeline p; p.Add(2,Finger());
        p.desired[2]=fingers::Curl(p.chains[2],.65,fingers::DefaultLimits(false));
        double first=0;
        for(int frame=0;frame<20;++frame) {
            const auto stats=p.Step(scene);
            EXPECT(stats.available && stats.casts<=32);
            if(frame==0) first=PlaneDepth(p,2,scene.planes[0]);
        }
        const double last=PlaneDepth(p,2,scene.planes[0]);
        std::printf("pipeline wall acquisition: first %.5f cm, settled %.5f cm\n",first,last);
        EXPECT(last<.05);
    }
    // A small curved obstacle stops only the finger that reaches it. The
    // measured capsule/sphere distance is independent of solver residuals.
    {
        World scene; scene.spheres.push_back({{8,0,0},.7});
        Pipeline p; p.Add(2,Finger());p.Add(3,Finger(4));
        for(int frame=0;frame<25;++frame) p.Step(scene);
        const double depth=SphereDepth(p,2,scene.spheres[0]);
        std::printf("pipeline curved acquisition: settled %.5f cm\n",depth);
        EXPECT(depth<.08);
        EXPECT(p.contacts[2].count>0);
        EXPECT(p.contacts[3].count==0);
        EXPECT(Length(Tip(p,3)-FVector{9.4,0,4})<1e-6);
    }
    // A narrow object intersects the middle of a phalanx while BOTH joints
    // are outside it. The spatial sweep's first-touch fraction has zero
    // depth by construction; the remaining segment must still be resolved.
    {
        World scene; scene.spheres.push_back({{5.4,0,0},.3});
        Pipeline p;p.Add(2,Finger());
        for(int frame=0;frame<20;++frame) p.Step(scene);
        const double depth=SphereDepth(p,2,scene.spheres[0]);
        std::printf("pipeline middle-of-phalanx obstacle: %.5f cm\n",depth);
        EXPECT(p.contacts[2].count>0);
        EXPECT(depth<.08);
    }
    // A previously empty scene can move into a stationary posed hand. The
    // coarse-clear cache must expire, then contact response must occur.
    {
        World scene; Pipeline p; p.Add(2,Finger());
        p.Step(scene); const FVector initial=Tip(p,2);
        scene.spheres.push_back({{8,0,0},.7});
        int detected=0;
        for(int frame=1;frame<=25;++frame) {
            p.Step(scene);
            if(!detected && p.contacts[2].count) detected=frame;
        }
        EXPECT(detected>0 && detected<=5);
        EXPECT(Length(Tip(p,2)-initial)>.1);
        EXPECT(SphereDepth(p,2,scene.spheres[0])<.08);
        // Removing the obstacle and asking the hand to open must converge
        // back to the requested pose, with no stale constraint holding it.
        scene.spheres.clear();
        // Bounded acceleration/speed now applies during release too.
        for(int frame=0;frame<90;++frame) p.Step(scene);
        EXPECT(p.contacts[2].count==0 && p.solved[2].active_contacts==0);
        EXPECT(Length(Tip(p,2)-initial)<.002);
    }
    // Query failure gives no invented contact and the next valid scene can
    // reacquire. Teleport/tracking reset discards motion and finger history.
    {
        World scene; scene.spheres.push_back({{8,0,0},.7});
        Pipeline p;p.Add(2,Finger());
        for(int i=0;i<20;++i) p.Step(scene);
        const auto before_failure=Tip(p,2);
        scene.available=false;
        const auto failed=p.Step(scene);
        EXPECT(!failed.available && p.contacts[2].count==0);
        EXPECT(Length(Tip(p,2)-before_failure)<1e-9);
        scene.available=true;
        EXPECT(p.Step(scene).available);
        p.Reset(); scene.spheres.clear(); p.hand.translation={100,20,80};
        const auto reset=p.Step(scene);
        EXPECT(reset.temporal_casts==0 && p.contacts[2].count==0);
        EXPECT(Length(Tip(p,2)-FVector{109.4,20,80})<1e-6);
    }
    // Long, pre-bent digits matching the scale/reference bends recorded in
    // the headset log. Sustain wall pressure and slide for ten seconds at
    // each supported refresh rate, then withdraw and open the hand.
    for (double hz : {72.0, 90.0, 120.0}) {
        World scene; scene.planes.push_back({{0,5.2,0},{0,-1,0}});
        Pipeline p;
        for (int f=1;f<5;++f) {
            skel::Xf joint[3];
            const double z=(f-1)*1.6;
            joint[0].translation={12.3,0,z};
            joint[0].rotation=AxisAngle({0,0,1},16*kDegToRad);
            joint[1].translation=joint[0].translation+joint[0].rotation.Rotate({4.8,0,0});
            joint[1].rotation=AxisAngle({0,0,1},28*kDegToRad);
            joint[2].translation=joint[1].translation+joint[1].rotation.Rotate({3.55,0,0});
            joint[2].rotation=joint[1].rotation;
            p.Add(f,fingers::Build(joint,{0,1,0},{12.3,0,6},false));
            p.desired[f]=fingers::Curl(p.chains[f],.55,fingers::DefaultLimits(false));
        }
        double worst_settled=0,max_step=0;
        fingers::Angles previous[5]{};
        for(int frame=0;frame<int(10*hz);++frame) {
            const double t=frame/hz;
            p.hand.translation={0,.4*std::sin(t*.8),.8*std::sin(t*.7)};
            const auto stats=p.Step(scene,1/hz);
            EXPECT(stats.available && stats.casts<=32);
            for(int f=1;f<5;++f) {
                if(t>1) worst_settled=std::max(worst_settled,PlaneDepth(p,f,scene.planes[0]));
                if(frame && !stats.reset) for(int j=0;j<3;++j) {
                    const auto a=fingers::LocalRotation(p.chains[f],j,previous[f]);
                    const auto b=fingers::LocalRotation(p.chains[f],j,p.resolved[f]);
                    const auto delta=Normalized(b*a.Inverse());
                    max_step=std::max(max_step,2*std::acos(std::clamp(std::fabs(delta.W),0.0,1.0)));
                }
                previous[f]=p.resolved[f];
            }
        }
        std::printf("pipeline sustained wall %.0f Hz: worst settled %.5f cm, joint step %.3f deg\n",
                    hz,worst_settled,max_step*kRadToDeg);
        EXPECT(worst_settled<.15);
        EXPECT(max_step<=420*kDegToRad/hz+1e-5);
        scene.planes.clear();
        for(int f=1;f<5;++f) p.desired[f]={};
        for(int frame=0;frame<int(hz);++frame) p.Step(scene,1/hz);
        for(int f=1;f<5;++f) {
            EXPECT(p.contacts[f].count==0);
            EXPECT(Length(Tip(p,f)-fc::SampleWorld(p.chains[f],p.desired[f],p.hand,2,1))<.03);
        }
    }
    if(failures) {std::printf("hand contact pipeline: %d failures\n",failures);return 1;}
    std::printf("hand contact pipeline: all tests passed\n");
    return 0;
}
