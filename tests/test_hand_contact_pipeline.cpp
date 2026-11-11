#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

#include "vr/body/finger_world_query.h"
#include "vr/body/thumb_pose.h"
#include "vr/body/finger_self.h"
#include "vr/body/hand_rigid_contact.h"
#include "vr/body/hand_motion.h"
#include "vr/body/hand_contact_staging.h"
#include "fixtures/white_man_hands.h"
#include "fixtures/white_man_shape_samples.h"

using namespace holevr::vr;
using namespace holevr::vr::ik;
namespace fw=finger_world_query;
namespace fc=finger_contact;
namespace jl=joint_limits;
namespace hm=body::hand_motion;
namespace staging=body::hand_contact_staging;
namespace shape=hand_collision_shape;
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
    hm::Sweep Box(const FVector& from,const FVector& to,const FVector& half,const FQuat& rotation) {
        hm::Sweep hit;hit.available=available;if(!available)return hit;
        double nearest=2;
        for(const auto& plane:planes) {
            const double radius=std::fabs(Dot(rotation.Rotate({1,0,0}),plane.normal))*half.X+
                std::fabs(Dot(rotation.Rotate({0,1,0}),plane.normal))*half.Y+
                std::fabs(Dot(rotation.Rotate({0,0,1}),plane.normal))*half.Z;
            const double start=Dot(from-plane.point,plane.normal)-radius,end=Dot(to-plane.point,plane.normal)-radius;
            if(start< -1e-8)return {true,true,true,from,from-plane.normal*radius,plane.normal};
            if(end>=0 || end>=start)continue;
            const double t=start/(start-end);if(t>=nearest)continue;
            nearest=t;hit.hit=true;hit.centre=from+(to-from)*t;hit.normal=plane.normal;hit.point=hit.centre-plane.normal*radius;
        }
        return hit;
    }
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
    skel::Xf hand, requested;
    hm::State motion;
    hm::PalmShape palm_shape;
    hm::RootSupport memory[80]{};
    double expires[80]{};
    int memory_count=0, published_frames=0, staged_frames=0;
    double now=0;
    bool visible=false,pending=false,published=false;
    staging::Intent intention, pending_intention;
    double pending_started=0;
    skel::Xf pending_hand;
    fingers::Angles pending_pose[5]{};
    double radius=.4;
    fw::Stats Step(World& scene,double dt=1.0/90) {
        now+=dt; published=false;
        intention.position=requested.translation; intention.rotation=requested.rotation;
        if(pending && !staging::Compatible(pending_intention,intention,now-pending_started)) pending=false;
        hm::Settings physics; physics.radius_cm=.05; // synthetic fixtures use a wrist anchor
        auto sweep=[&](const FVector&a,const FVector&b,double r) {
            fw::Hit hit; const auto status=scene.Sweep(a,b,r,hit);
            return hm::Sweep{status!=fw::QueryStatus::unavailable,status==fw::QueryStatus::hit,
                             hit.initial_overlap,hit.center,hit.point,hit.normal};
        };
        auto box=[&](const FVector&a,const FVector&b,const FVector&half,const FQuat&rotation) {
            return scene.Box(a,b,half,rotation);
        };
        for(int i=0;i<memory_count;++i) memory[i].remaining_seconds=std::max(0.0,expires[i]-now);
        hm::Input input; input.target=requested.translation; input.rotation=requested.rotation;
        input.anchor=requested.translation; input.dt=dt; input.supports=memory; input.support_count=memory_count;
        input.palm=palm_shape;
        const auto proxy=hm::Step(motion,physics,input,sweep,box);
        EXPECT(proxy.queries<=24);
        skel::Xf candidate=pending ? pending_hand : requested;
        if(!pending && proxy.valid) {candidate.translation=proxy.position;candidate.rotation=proxy.rotation;}
        const auto displayed_state=std::to_array(fingers);
        fingers::Angles candidate_pose[5]{};
        fc::Contacts empty[5]{};
        for(int f=0;f<5;++f) if(chains[f].valid) {
            // Pending already spent its input-easing budget. Retry with the
            // current frame's hard speed/acceleration caps, without shrinking
            // the prepared target exponentially toward the displayed pose.
            candidate_pose[f]=fc::Prepare(chains[f],pending?pending_pose[f]:desired[f],
                                           fingers[f],bounds[f],dt,false,pending);
        }
        auto separation=finger_self::Resolve(chains,candidate_pose,fingers,candidate,bounds,empty,radius,dt);
        EXPECT(separation.feasible);
        const auto queried=fw::MakeGeometry(chains,candidate_pose,candidate,radius);
        auto stats=fw::Update(query,queried,dt,scene,contacts);
        bool recheck_separation=false;
        for(int f=0;f<5;++f) if(chains[f].valid) {
            if(!stats.available || (stats.unresolved_mask & (1u<<f))) {
                recheck_separation=true;
                candidate_pose[f]=fingers[f].frame_start;
                fingers[f].pose=candidate_pose[f]; fingers[f].angular_velocity={}; fingers[f].initialized=true;
            } else if(contacts[f].count) {
                recheck_separation=true;
                solved[f]=fc::ResolvePrepared(chains[f],candidate_pose[f],fingers[f],candidate,bounds[f],contacts[f]);
                candidate_pose[f]=solved[f].pose;
            } else {
                fingers[f].initialized=true;
                solved[f]={};
                solved[f].pose=candidate_pose[f];
            }
            for(int j=0;j<3;++j) {
                const FQuat q=chains[f].ref[j].rotation.Inverse()*fingers::LocalRotation(chains[f],j,candidate_pose[f]);
                EXPECT(jl::Within(bounds[f].axes[j],bounds[f].limits[j],q,1e-6));
            }
        }
        if(recheck_separation)
            separation=finger_self::Resolve(chains,candidate_pose,fingers,candidate,bounds,contacts,radius,dt);
        EXPECT(separation.feasible && separation.residual_cm<=.001);
        body::hand_rigid_contact::Plane supports[80]; double ages[80]{}; int count=0;
        const auto before_correction=candidate;
        bool feasible=stats.available && proxy.valid && separation.feasible;
        if(feasible) {
            for(int f=0;f<5;++f)for(int n=0;n<contacts[f].count;++n) {
                const auto w=fc::EvaluateContact(chains[f],candidate_pose[f],candidate,contacts[f].items[n]);
                if(w.active) {ages[count]=contacts[f].items[n].age_seconds;supports[count++]={w.surface,w.normal,w.center,w.radius_cm};}
            }
            const auto pressure=body::hand_rigid_contact::Resolve(supports,count);
            feasible=pressure.valid && pressure.feasible;
            if(feasible) {
                candidate.translation=candidate.translation+pressure.translation;
                const auto guarded=hm::Constrain(motion,physics,palm_shape,candidate.translation,candidate.rotation,sweep,box,memory,memory_count);
                feasible=guarded.valid;
                if(feasible) {candidate.translation=guarded.position;candidate.rotation=guarded.rotation;}
            }
        }
        const auto actual=fw::MakeGeometry(chains,candidate_pose,candidate,radius);
        bool checked=feasible && stats.geometry_clear && !stats.unresolved_mask && fw::MaxDelta(queried,actual)<=.001;
        if(feasible && !checked) {
            const auto verified=fw::VerifyGeometry(query,actual,scene);
            stats.casts+=verified.casts;
            checked=verified.available && verified.clear;
        }
        EXPECT(stats.casts<=32);
        if(checked) {
            // Commit only the actual drawn wrist and its matching finger
            // angles. Cached supports retain original observation lifetimes.
            hand=candidate; visible=published=true; pending=false; ++published_frames;
            memory_count=0;
            const FQuat turn=Normalized(hand.rotation*before_correction.rotation.Inverse());
            for(int i=0;i<count;++i) if(ages[i]<.05) {
                const auto& p=supports[i];
                const FVector world=hand.translation+turn.Rotate(p.sample-before_correction.translation);
                memory[memory_count]={p.point,p.normal,hand.rotation.Inverse().Rotate(world-hand.translation),p.radius,.05-ages[i]};
                expires[memory_count++]=now+.05-ages[i];
            }
            for(int f=0;f<5;++f) {resolved[f]=candidate_pose[f];fingers[f].pose=resolved[f];}
            fw::Commit(query,actual);
        } else {
            // A corrected but unaudited pose becomes the next query target,
            // never the displayed pose. Otherwise cold overlap recovery can
            // deadlock forever by re-querying the original buried wrist.
            if(feasible) {
                if(!pending) {pending_intention=intention; pending_started=now;}
                pending=true; pending_hand=candidate;
                for(int f=0;f<5;++f) pending_pose[f]=candidate_pose[f];
                ++staged_frames;
            }
            std::copy(displayed_state.begin(),displayed_state.end(),fingers);
            for(auto& digit:fingers) digit.angular_velocity={};
        }
        return stats;
    }
    void Add(int f,const fingers::Chain& chain) { chains[f]=chain; bounds[f]=fc::MakeBounds(chain,f); }
    void Reset() {
        fw::Reset(query); hm::Reset(motion); memory_count=0; pending=visible=false;
        for(auto& s:fingers) fc::Reset(s);
    }
};
static double PlaneDepth(const Pipeline& p,int finger,const Plane& plane) {
    const auto geometry=fw::MakeGeometry(p.chains,p.resolved,p.hand,p.radius);
    skel::Xf joints[3]; fingers::Fk(p.chains[finger],p.resolved[finger],joints,nullptr);
    double depth=0;
    for(int j=0;j<3;++j) {
        const auto section=shape::Transform(p.chains[finger].collision[j],joints[j],p.hand,p.radius);
        for(int n=j;n<=j+1;++n)
            depth=std::max(depth,section.radius_cm-Dot(geometry.point[finger][n]-plane.point,plane.normal));
    }
    return depth;
}
static double SphereDepth(const Pipeline& p,int finger,const Sphere& sphere) {
    const auto geometry=fw::MakeGeometry(p.chains,p.resolved,p.hand,p.radius);
    skel::Xf joints[3]; fingers::Fk(p.chains[finger],p.resolved[finger],joints,nullptr);
    double depth=0;
    for(int j=0;j<3;++j) {
        const auto a=geometry.point[finger][j],b=geometry.point[finger][j+1],delta=b-a;
        const double l2=Dot(delta,delta);
        const double t=l2>1e-9?std::clamp(Dot(sphere.center-a,delta)/l2,0.0,1.0):0;
        const auto section=shape::Transform(p.chains[finger].collision[j],joints[j],p.hand,p.radius);
        depth=std::max(depth,section.radius_cm+sphere.radius-Length(a+delta*t-sphere.center));
    }
    return depth;
}
static FVector Tip(const Pipeline& p,int f) { return fc::SampleWorld(p.chains[f],p.resolved[f],p.hand,2,1); }
static FVector SkinSample(const white_man_shape_samples::Sample& sample,const skel::Xf reference[5][3],
                          const skel::Xf posed[5][3],const skel::Xf& hand) {
    FVector point; double weight=0;
    for(const auto& influence:sample.influence) if(influence.weight>0) {
        FVector moved=sample.point;
        if(influence.finger>=0) {
            const auto& ref=reference[influence.finger][influence.joint];
            const auto& pose=posed[influence.finger][influence.joint];
            skel::Xf original;original.translation=sample.point;
            moved=skel::Apply(pose,skel::Relative(original,ref).translation);
        }
        point=point+moved*influence.weight; weight+=influence.weight;
    }
    EXPECT(std::fabs(weight-1)<1e-5);
    return skel::Apply(hand,point*(1/weight));
}
static double PointSegmentDistance(const FVector& point,const FVector& a,const FVector& b) {
    const auto d=b-a;const double squared=Dot(d,d);
    const double t=squared>1e-12?std::clamp(Dot(point-a,d)/squared,0.0,1.0):0;
    return Length(point-a-d*t);
}
static double PalmOutside(const FVector& point,const shape::Palm& palm) {
    const auto d=point-palm.centre;
    const FVector outside{std::max(0.0,std::fabs(Dot(d,palm.axis[0]))-palm.half.X),
                          std::max(0.0,std::fabs(Dot(d,palm.axis[1]))-palm.half.Y),
                          std::max(0.0,std::fabs(Dot(d,palm.axis[2]))-palm.half.Z)};
    return Length(outside);
}
static hm::PalmShape TransformedPalm(const shape::Palm& fitted,const FVector& scale) {
    skel::Xf hand;
    hand.scale=scale;
    const auto world=shape::TransformPalm(fitted,hand);
    hm::PalmShape palm;
    palm.valid=world.fitted;
    palm.centre=world.centre;
    palm.rotation=AlignFrames({0,0,1},{0,1,0},world.axis[2],world.axis[1]);
    palm.half=world.half;
    return palm;
}

int main() {
    // Pending retries retain one raw-intent snapshot. Controller withdrawal,
    // opening, touch changes, rotation, and expiry all preempt old work.
    {
        staging::Intent a,b;
        EXPECT(staging::Compatible(a,b,.10));
        b.position={.751,0,0}; EXPECT(!staging::Compatible(a,b,.01));
        b={}; b.rotation=AxisAngle({0,1,0},8.1*kDegToRad); EXPECT(!staging::Compatible(a,b,.01));
        b={}; b.rotation={0,0,0,-1}; EXPECT(staging::Compatible(a,b,.01));
        b={}; b.grip=.121; EXPECT(!staging::Compatible(a,b,.01));
        b={}; b.trigger=.121; EXPECT(!staging::Compatible(a,b,.01));
        b={}; b.thumb_touch=true; EXPECT(!staging::Compatible(a,b,.01));
        b={}; b.trigger_touch=true; EXPECT(!staging::Compatible(a,b,.01));
        b={}; EXPECT(!staging::Compatible(a,b,.151));
        EXPECT(!staging::Compatible(a,b,-.01));
        b.rotation={0,0,0,0}; EXPECT(!staging::Compatible(a,b,.01));
    }
    // The fitted union includes mixed thumb/palm web. Its checked box seats,
    // slides, tilts and withdraws without unchecked post-proxy movement.
    // Measure the actual neutral-open skin gap separately: conservative
    // enclosure is not a claim that every palm vertex touches the wall.
    for(int side=0;side<2;++side)for(FVector scale:{FVector{.75,.75,.75},FVector{1,1,1},
                                                 FVector{1.25,1.25,1.25},FVector{.8,1.1,.9}}) {
        fingers::Chain chain[5];fc::Bounds bounds[5];FVector normal;
        white_man_hands::Build(side,chain,bounds,&normal);shape::Palm fitted;
        EXPECT(shape::FitWhiteMan(chain,normal,&fitted) && fitted.fitted);
        const auto palm=TransformedPalm(fitted,scale);
        const auto out=palm.rotation.Rotate({0,0,1}),forward=palm.rotation.Rotate({0,1,0});
        World scene;scene.planes.push_back({out*20,out*-1});
        hm::Settings settings;hm::State state;hm::Input input;input.palm=palm;input.dt=1.0/35;
        auto step=[&]() {return hm::Step(state,settings,input,
            [](const FVector&,const FVector&,double){EXPECT(false);return hm::Sweep{};},
            [&](const FVector& a,const FVector& b,const FVector& h,const FQuat& q){return scene.Box(a,b,h,q);});};
        EXPECT(step().valid);input.target=out*30;
        hm::Result result;
        for(int frame=0;frame<70;++frame){result=step();EXPECT(result.valid && result.queries<=24);}
        double closest=1e9;
        auto skin_clearance=[&]() {
            for(const auto& sample:white_man_shape_samples::palm)if(sample.side==side) {
                const FVector local{sample.point.X*scale.X,sample.point.Y*scale.Y,sample.point.Z*scale.Z};
                const auto skin=result.position+result.rotation.Rotate(local);
                const double gap=Dot(skin-scene.planes[0].point,scene.planes[0].normal);
                EXPECT(gap>=-.001);closest=std::min(closest,gap);
            }
        };
        skin_clearance();EXPECT(result.touching && closest>.02);
        const auto face=result.position+result.rotation.Rotate(palm.centre)+out*palm.half.Z;
        EXPECT(std::fabs(Dot(face-scene.planes[0].point,scene.planes[0].normal)-settings.skin_cm)<.001);
        skel::Xf reference[5][3],joints[5][3],hand;
        hand.scale=scale;hand.rotation=result.rotation;hand.translation=result.position;
        for(int f=0;f<5;++f) {
            fingers::Fk(chain[f],{},reference[f],nullptr);
            const auto pose=f?fc::CurlTarget(chain[f],0,bounds[f]):
                thumb_pose::Target(chain[f],normal,chain[1].ref[0].translation,{},bounds[0]);
            fingers::Fk(chain[f],pose,joints[f],nullptr);
        }
        const auto front=SkinSample(white_man_shape_samples::open_front[side],reference,joints,hand);
        const double front_gap=Dot(front-scene.planes[0].point,scene.planes[0].normal);
        EXPECT(front_gap>.02);
        if(scale.X==1 && scale.Y==1 && scale.Z==1) {
            const double measured_gap=side?.385896:.437183;
            std::printf("open visible skin seating side%d: %.3f mm including %.2f mm solver skin\n",
                        side,front_gap*10,settings.skin_cm*10);
            EXPECT(std::fabs(front_gap-settings.skin_cm-measured_gap)<.002);
        }
        for(int frame=0;frame<35;++frame) {
            input.target=out*30+forward*(frame*.2);
            input.rotation=AxisAngle(palm.rotation.Rotate({1,0,0}),10*kDegToRad*frame/35);
            result=step();EXPECT(result.valid && result.queries<=24);skin_clearance();
        }
        input.target=out*-5;
        for(int frame=0;frame<70;++frame)result=step();
        EXPECT(result.valid && !result.touching && Length(result.position-input.target)<.03);
    }
    // Independently extracted skin support vertices retain their original
    // blend weights. Check reference/open/half/full curl after deforming them
    // with the same joint transforms as the visible mesh, including scale.
    {
        double missed=0,legacy_missed=0,palm_missed=0;
        for(int side=0;side<2;++side) {
            Pipeline p; FVector palm;
            white_man_hands::Build(side,p.chains,p.bounds,&palm);
            shape::Palm fitted_palm;
            EXPECT(shape::FitWhiteMan(p.chains,palm,&fitted_palm));
            skel::Xf reference[5][3];
            for(int f=0;f<5;++f)fingers::Fk(p.chains[f],{},reference[f],nullptr);
            const auto middle=p.chains[2].ref[0].translation;
            const FVector forward=Normalize(Reject(middle,palm)),across=Normalize(Cross(forward,palm));
            const FVector old_centre=middle*.55;
            const FVector old_half={Length(p.chains[1].ref[0].translation-p.chains[4].ref[0].translation)*.5+.8,
                                    Length(middle)*.45,1.5};
            for(const auto& sample:white_man_shape_samples::palm)if(sample.side==side) {
                const auto delta=sample.point-old_centre;
                palm_missed=std::max({palm_missed,std::fabs(Dot(delta,across))-old_half.X,
                    std::fabs(Dot(delta,forward))-old_half.Y,std::fabs(Dot(delta,palm))-old_half.Z});
            }
            for(double amount:{-1.0,0.0,.5,1.0}) {
                fingers::Angles pose[5]; skel::Xf joints[5][3];
                for(int f=0;f<5;++f) {
                    pose[f]=amount<0?fingers::Angles{}:fc::CurlTarget(p.chains[f],amount,p.bounds[f]);
                    if(f==0 && amount>=0)pose[f]=thumb_pose::Target(p.chains[f],palm,p.chains[1].ref[0].translation,
                        {amount,amount,false},p.bounds[f]);
                    fingers::Fk(p.chains[f],pose[f],joints[f],nullptr);
                }
                for(FVector scale:{FVector{.75,.75,.75},FVector{1,1,1},FVector{1.25,1.25,1.25},FVector{.8,1.1,.9}}) {
                    skel::Xf hand;hand.scale=scale;hand.rotation=AxisAngle({1,2,3},.7);hand.translation={17,-8,3};
                    const auto geometry=fw::MakeGeometry(p.chains,pose,hand,.5);
                    const auto world_palm=shape::TransformPalm(fitted_palm,hand);
                    auto check_sample=[&](const white_man_shape_samples::Sample& sample) {
                        if(sample.side!=side)return;
                        const auto point=SkinSample(sample,reference,joints,hand);
                        double fitted=PalmOutside(point,world_palm),legacy=1e9;
                        for(int f=0;f<5;++f)for(int j=0;j<3;++j) {
                            const double distance=PointSegmentDistance(point,geometry.point[f][j],geometry.point[f][j+1]);
                            const auto section=shape::Transform(p.chains[f].collision[j],joints[f][j],hand,.5);
                            fitted=std::min(fitted,distance-section.radius_cm);legacy=std::min(legacy,distance-.5);
                        }
                        missed=std::max(missed,fitted);legacy_missed=std::max(legacy_missed,legacy);
                    };
                    for(const auto& sample:white_man_shape_samples::finger)check_sample(sample);
                    for(const auto& sample:white_man_shape_samples::web)check_sample(sample);
                    for(const auto& sample:white_man_shape_samples::open_front)check_sample(sample);
                }
            }
            p.chains[2].ref[1].translation.X+=.2;
            EXPECT(!shape::FitWhiteMan(p.chains,palm));
            for(const auto& chain:p.chains)for(const auto& section:chain.collision)EXPECT(!section.fitted);
        }
        std::printf("independent skin samples: fitted missed %.5f cm, old .5cm capsules missed %.3f cm; old palm box missed %.3f cm\n",
            missed,legacy_missed,palm_missed);
        EXPECT(missed<.02 && legacy_missed>.25);
    }
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
        EXPECT(p.visible && p.published_frames>0 && last<.05);
    }
    // A small curved obstacle stops only the finger that reaches it. The
    // measured capsule/sphere distance is independent of solver residuals.
    {
        World scene; scene.spheres.push_back({{8,0,0},.7});
        Pipeline p; p.Add(2,Finger());p.Add(3,Finger(4));
        for(int frame=0;frame<25;++frame) p.Step(scene);
        const double depth=SphereDepth(p,2,scene.spheres[0]);
        std::printf("pipeline curved acquisition: settled %.5f cm\n",depth);
        EXPECT(p.visible && p.published_frames>0 && depth<.08);
        EXPECT(p.contacts[2].count>0);
        EXPECT(p.contacts[3].count==0);
        EXPECT(Length((Tip(p,3)-p.hand.translation)-FVector{9.4,0,4})<1e-6);
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
        EXPECT(p.visible && p.published_frames>0 && depth<.08);
    }
    // Root backpressure that clears one digit can move a DIFFERENT digit
    // into an obstacle outside the original contacts. Final geometry must
    // reject that frame, stage the candidate, then query its new location.
    {
        World scene; scene.spheres={{{8,0,0},.7},{{-1.8,0,4},.3}};
        Pipeline p; p.Add(2,Finger()); p.Add(3,Finger(4));
        const auto first=p.Step(scene);
        EXPECT(first.available && first.casts<=32 && !p.published && p.pending);
        for(int i=0;i<30;++i) {
            const auto stats=p.Step(scene);
            EXPECT(stats.casts<=32);
            if(p.published) {
                EXPECT(SphereDepth(p,2,scene.spheres[0])<.02);
                EXPECT(SphereDepth(p,3,scene.spheres[1])<.02);
            }
        }
        EXPECT(p.visible && p.published_frames>10 && p.staged_frames>0);
        EXPECT(SphereDepth(p,2,scene.spheres[0])<.02 && SphereDepth(p,3,scene.spheres[1])<.02);
    }
    // Start with a verified displayed hand, then push all five roots toward
    // a wall. A cold overlap frame can spend30of32casts on acquisition, so
    // holding the old wrist must not destroy the staged query observations.
    {
        World scene; scene.planes.push_back({{0,4.5,0},{0,-1,0}});
        Pipeline p; for(int f=0;f<5;++f) p.Add(f,Finger(f*2.0));
        p.Step(scene); EXPECT(p.visible);
        p.requested.translation={0,6,0};
        bool budgeted=false;
        for(int i=0;i<80;++i) {
            const auto stats=p.Step(scene);
            budgeted|=stats.casts>=30;
            EXPECT(stats.casts<=32 && p.visible);
            for(int f=0;f<5;++f) EXPECT(PlaneDepth(p,f,scene.planes[0])<.02);
        }
        EXPECT(budgeted && p.staged_frames>0 && p.published_frames>20);
        EXPECT(p.hand.translation.Y>3.8 && p.hand.translation.Y<4.101);
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
    // A cold finger can begin too deeply overlapped to recover a trustworthy
    // surface even though the wrist is clear. Withdrawing the controller must
    // replace that staged work; retrying its old world pose forever is wrong.
    {
        World scene; scene.planes.push_back({{10,0,0},{-1,0,0}});
        Pipeline p; auto chain=Finger(); chain.ref[0].translation.X=14; p.Add(2,chain);
        p.intention.grip=.8;
        p.Step(scene); const double started=p.pending_started;
        for(int i=0;i<4;++i) p.Step(scene);
        EXPECT(!p.visible && p.pending);
        EXPECT(p.pending_started==started); // stationary retries do not renew the deadline
        p.intention.grip=0; p.Step(scene);
        EXPECT(p.pending && p.pending_intention.grip==0 && p.pending_started>started);
        p.requested.translation={-20,0,0};
        for(int i=0;i<90;++i) p.Step(scene);
        EXPECT(p.visible && !p.pending && PlaneDepth(p,2,scene.planes[0])<.02);
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
        p.Reset(); scene.spheres.clear(); p.requested.translation={100,20,80};
        const auto reset=p.Step(scene);
        EXPECT(reset.temporal_casts==0 && p.contacts[2].count==0);
        EXPECT(Length(Tip(p,2)-FVector{109.4,20,80})<1e-6);
    }
    // Long, pre-bent digits matching the scale/reference bends recorded in
    // the headset log. Sustain wall pressure and slide for ten seconds at
    // each supported refresh rate, then withdraw and open the hand.
    for (double hz : {25.0, 35.0, 45.0, 72.0, 90.0, 120.0}) {
        World scene; scene.planes.push_back({{0,5.2,0},{0,-1,0}});
        Pipeline p; p.intention.grip=.55;
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
            p.desired[f]=fc::CurlTarget(p.chains[f],.55,p.bounds[f]);
        }
        double worst_settled=0,max_step=0;
        fingers::Angles previous[5]{};
        for(int frame=0;frame<int(10*hz);++frame) {
            const double t=frame/hz;
            p.requested.translation={0,-4*std::max(0.0,1-t*2)+.4*std::sin(t*.8),.8*std::sin(t*.7)};
            const auto stats=p.Step(scene,1/hz);
            EXPECT(stats.available && stats.casts<=32);
            for(int f=1;f<5;++f) {
                if(t>1) { EXPECT(p.visible); worst_settled=std::max(worst_settled,PlaneDepth(p,f,scene.planes[0])); }
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
        EXPECT(worst_settled<.02 && p.published_frames>int(hz));
        EXPECT(max_step<=240*kDegToRad/hz+1e-5);
        scene.planes.clear();
        p.intention.grip=0;
        for(int f=1;f<5;++f) p.desired[f]=fc::CurlTarget(p.chains[f],0,p.bounds[f]);
        const int before_release=p.published_frames, staged_release=p.staged_frames;
        for(int frame=0;frame<int(hz);++frame) p.Step(scene,1/hz);
        std::printf("pipeline release %.0f Hz: tip error %.5f cm, published %d staged %d pending %d\n",hz,
            Length(Tip(p,1)-fc::SampleWorld(p.chains[1],p.desired[1],p.hand,2,1)),
            p.published_frames-before_release,p.staged_frames-staged_release,p.pending?1:0);
        // One cold refresh may hold a frame, but redundant old-pose casts
        // must not exhaust the budget and halve the ongoing motion rate.
        EXPECT(p.published_frames-before_release>=int(hz)-1);
        for(int f=1;f<5;++f) {
            EXPECT(p.contacts[f].count==0);
            EXPECT(Length(Tip(p,f)-fc::SampleWorld(p.chains[f],p.desired[f],p.hand,2,1))<.03);
        }
    }
    // Actual game rig: the thumb's fixed root is buried 1.104 cm when the
    // previous palm proxy stops at its nominal face. Reproduce the measured
    // pressure at the observed low frame rates; a wall must move the whole
    // hand back without creating a progressively more curled finger pose.
    for(int side=0;side<2;++side)for(double hz:{25.0,35.0,45.0,72.0}) {
        Pipeline p; p.radius=.5; FVector palm;
        white_man_hands::Build(side,p.chains,p.bounds,&palm);
        for(int f=1;f<5;++f) p.desired[f]=fc::CurlTarget(p.chains[f],0,p.bounds[f]);
        p.desired[0]=thumb_pose::Target(p.chains[0],palm,p.chains[1].ref[0].translation,{},p.bounds[0]);
        World scene;scene.planes.push_back({palm*1.53,palm*-1});
        const FVector thumb_root=fc::SampleWorld(p.chains[0],{},skel::Xf{},0,0);
        EXPECT(std::fabs((Dot(thumb_root,palm)-1.53+.5)-1.1035)<.001);
        if(side==0 && hz==25) {
            const auto wanted=fw::MakeGeometry(p.chains,p.desired,{},p.radius);
            for(int f=0;f<5;++f) std::printf("open rig digit%d palm stations %.3f %.3f %.3f %.3f; ref bends %.2f %.2f %.2f deg\n",f,
                Dot(wanted.point[f][0],palm),Dot(wanted.point[f][1],palm),Dot(wanted.point[f][2],palm),Dot(wanted.point[f][3],palm),
                p.bounds[f].limits[0].ref_bend*kRadToDeg,p.bounds[f].limits[1].ref_bend*kRadToDeg,p.bounds[f].limits[2].ref_bend*kRadToDeg);
        }
        double worst=0,self=0,root=0,joint_drift=0;
        fingers::Angles first[5]{};
        for(int frame=0;frame<180;++frame) {
            // Start with an observable outside pose, then approach. The game
            // does not begin a tracked palm seven centimetres inside a wall.
            p.requested.translation=palm*(frame<45?-12.0*(1.0-frame/45.0):(frame<90?0:1.0));
            const double dt=frame%3==0?.05:1/hz;
            const auto stats=p.Step(scene,dt);
            EXPECT(stats.available && stats.casts<=32);
            for(int f=0;f<5;++f) {
                if(frame>60) {EXPECT(p.visible);worst=std::max(worst,PlaneDepth(p,f,scene.planes[0]));}
                if(frame==60)first[f]=p.resolved[f];
                if(frame>60)for(int j=0;j<3;++j) {
                    const auto delta=Normalized(fingers::LocalRotation(p.chains[f],j,p.resolved[f])*
                        fingers::LocalRotation(p.chains[f],j,first[f]).Inverse());
                    joint_drift=std::max(joint_drift,2*std::acos(std::clamp(std::fabs(delta.W),0.0,1.0)));
                }
            }
            if(p.visible)self=std::max(self,finger_self::Depth(p.chains,p.resolved,p.hand,p.radius));
            if(frame>60)root=std::max(root,Length(p.hand.translation-p.requested.translation));
        }
        std::printf("actual rig %s %.0f Hz+jitter: world %.5f self %.5f root %.3f drift %.5f deg\n",
            side?"right":"left",hz,worst,self,root,joint_drift*kRadToDeg);
        EXPECT(worst<.02 && self<.001 && root>1.1 && root<3.0 && joint_drift<.001 && p.published_frames>30);
    }
    // Fitted source-skin dimensions are transformed once by the drawn hand
    // scale. All world segments retain their full roots; only connected web
    // regions may be excluded from same-hand separation.
    for(int side=0;side<2;++side)for(FVector scale:{FVector{.75,.75,.75},FVector{1,1,1},
                                                 FVector{1.25,1.25,1.25},FVector{.8,1.1,.9}}) {
        Pipeline p; p.radius=.5; FVector palm;
        white_man_hands::Build(side,p.chains,p.bounds,&palm);
        shape::Palm fitted_palm;
        EXPECT(shape::FitWhiteMan(p.chains,palm,&fitted_palm));
        p.palm_shape=TransformedPalm(fitted_palm,scale);
        p.requested.scale=scale; p.requested.rotation=AxisAngle({0,0,1},.4);
        for(int f=1;f<5;++f) p.desired[f]=fc::CurlTarget(p.chains[f],0,p.bounds[f]);
        p.desired[0]=thumb_pose::Target(p.chains[0],palm,p.chains[1].ref[0].translation,{},p.bounds[0]);
        fingers::Angles reference[5]{};
        const double reference_overlap=finger_self::Depth(p.chains,reference,p.requested,p.radius);
        const double open_overlap=finger_self::Depth(p.chains,p.desired,p.requested,p.radius);
        std::printf("fitted rig %s scale %.2f/%.2f/%.2f: reference %.5f open %.5f cm\n",
            side?"right":"left",scale.X,scale.Y,scale.Z,reference_overlap,open_overlap);
        EXPECT(reference_overlap<.001 && open_overlap<.001);
        if(reference_overlap>=.001 || open_overlap>=.001) continue;
        const FVector normal=Normalize(p.requested.rotation.Rotate({palm.X/scale.X,palm.Y/scale.Y,palm.Z/scale.Z}));
        const FVector wall=skel::Apply(p.requested,palm*1.53);
        World scene; scene.planes.push_back({wall,normal*-1});
        const auto world_palm=shape::TransformPalm(fitted_palm,p.requested);
        const double palm_support=std::fabs(Dot(world_palm.axis[0],normal))*world_palm.half.X+
            std::fabs(Dot(world_palm.axis[1],normal))*world_palm.half.Y+
            std::fabs(Dot(world_palm.axis[2],normal))*world_palm.half.Z;
        double expected_shift=Dot(world_palm.centre-wall,normal)+palm_support+.03;
        const auto target=fw::MakeGeometry(p.chains,p.desired,p.requested,p.radius);
        for(int f=0;f<5;++f) {
            skel::Xf joints[3]; fingers::Fk(p.chains[f],p.desired[f],joints,nullptr);
            for(int j=0;j<3;++j) {
                const auto section=shape::Transform(p.chains[f].collision[j],joints[j],p.requested,p.radius);
                EXPECT(section.fitted && section.radius_cm>.65);
                for(int n=j;n<=j+1;++n) expected_shift=std::max(expected_shift,
                    Dot(target.point[f][n]-wall,normal)+section.radius_cm+.03);
            }
        }
        double worst_world=0,worst_self=0,root=0;
        for(int frame=0;frame<140;++frame) {
            p.requested.translation=normal*(frame<40?-14.0*(1-frame/40.0):(frame<80?0:1));
            const auto stats=p.Step(scene,1.0/35);
            EXPECT(stats.available && stats.casts<=32);
            if(frame>60) {
                EXPECT(p.visible);
                for(int f=0;f<5;++f) worst_world=std::max(worst_world,PlaneDepth(p,f,scene.planes[0]));
                const auto actual_palm=shape::TransformPalm(fitted_palm,p.hand);
                const double support=std::fabs(Dot(actual_palm.axis[0],normal))*actual_palm.half.X+
                    std::fabs(Dot(actual_palm.axis[1],normal))*actual_palm.half.Y+
                    std::fabs(Dot(actual_palm.axis[2],normal))*actual_palm.half.Z;
                worst_world=std::max(worst_world,Dot(actual_palm.centre-wall,normal)+support);
                worst_self=std::max(worst_self,finger_self::Depth(p.chains,p.resolved,p.hand,p.radius));
            }
            if(frame>110)root=std::max(root,Length(p.hand.translation-p.requested.translation));
        }
        double open_error=0;
        for(int f=0;f<5;++f)open_error=std::max(open_error,Length(Tip(p,f)-fc::SampleWorld(p.chains[f],p.desired[f],p.hand,2,1)));
        std::printf("fitted rig contact: world %.5f self %.5f setback %.3f expected %.3f cm, open error %.5f cm\n",
            worst_world,worst_self,root,expected_shift+1,open_error);
        EXPECT(worst_world<.02 && worst_self<.001 && std::fabs(root-expected_shift-1)<.05);
        EXPECT(open_error<.03);
        EXPECT(p.published_frames>50);
        if(scale.X==1 && scale.Y==1 && scale.Z==1) {
            scene.planes.clear();
            p.intention.grip=p.intention.trigger=.7;
            for(int f=1;f<5;++f)p.desired[f]=fc::CurlTarget(p.chains[f],.7,p.bounds[f]);
            p.desired[0]=thumb_pose::Target(p.chains[0],palm,p.chains[1].ref[0].translation,{.7,.7,false},p.bounds[0]);
            for(int frame=0;frame<35;++frame) {
                const auto stats=p.Step(scene,1.0/35);
                EXPECT(stats.available && stats.casts<=32);
                EXPECT(finger_self::Depth(p.chains,p.resolved,p.hand,p.radius)<.001);
            }
            // Self contact may arrest a requested fist. Releasing controls
            // must still escape it and return to the anatomical open target.
            p.intention.grip=p.intention.trigger=0;
            for(int f=1;f<5;++f)p.desired[f]=fc::CurlTarget(p.chains[f],0,p.bounds[f]);
            p.desired[0]=thumb_pose::Target(p.chains[0],palm,p.chains[1].ref[0].translation,{},p.bounds[0]);
            for(int frame=0;frame<53;++frame) {
                const auto stats=p.Step(scene,1.0/35);
                EXPECT(stats.available && stats.casts<=32);
                EXPECT(finger_self::Depth(p.chains,p.resolved,p.hand,p.radius)<.001);
            }
            double error=0;
            for(int f=0;f<5;++f)error=std::max(error,Length(Tip(p,f)-fc::SampleWorld(p.chains[f],p.desired[f],p.hand,2,1)));
            std::printf("fitted rig close/release %s: error %.5f cm\n",side?"right":"left",error);
            EXPECT(error<.03);
        }
    }
    if(failures) {std::printf("hand contact pipeline: %d failures\n",failures);return 1;}
    std::printf("hand contact pipeline: all tests passed\n");
    return 0;
}
