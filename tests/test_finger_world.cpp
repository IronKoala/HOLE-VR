#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include "vr/body/finger_world_query.h"

using namespace holevr::vr;
using namespace holevr::vr::ik;
namespace w = finger_world_query;
static int failures=0;
#define EXPECT(x) do { if (!(x)) { std::printf("FAIL %d: %s\n",__LINE__,#x); ++failures; } } while (0)

struct Sphere { FVector center; double radius; std::uint64_t id=0; };
struct Plane { FVector point, normal; std::uint64_t id=0; };
struct Slab { double lo,hi; std::uint64_t id; };
struct World final : w::Backend {
    std::vector<Sphere> spheres;
    std::vector<Plane> planes;
    std::vector<Slab> slabs;
    int calls=0,engine_calls=0,frame_calls=0;
    int trace_passes=1; // counting fixture for coincident simple/visible surfaces
    bool rigid=false,invalid_frame=false;
    skel::Xf frame{};
    bool fail=false, overlap_only=false, miss_broad=false;
    w::QueryStatus Sweep(const FVector& from,const FVector& to,double radius,w::Hit& hit) override {
        ++calls;
        engine_calls+=trace_passes;
        if (fail) return w::QueryStatus::unavailable;
        if (miss_broad && radius>3) return w::QueryStatus::clear;
        if (overlap_only) {
            engine_calls+=trace_passes;
            hit={from,from,FVector{0,0,1},true};
            return w::QueryStatus::hit;
        }
        double best=2;
        const FVector move=to-from;
        auto take=[&](double time,const FVector& normal,const FVector& surface,bool overlap,std::uint64_t id=0) {
            if (time<best) { best=time; hit={from+move*time,surface,normal,overlap,id}; }
        };
        for (const auto& s:spheres) {
            const FVector m=from-s.center;
            const double expanded=s.radius+radius;
            const double c=Dot(m,m)-expanded*expanded;
            if (c<=0) {
                take(0,Normalize(m,FVector{0,0,1}),from,true,s.id);
                continue;
            }
            const double a=Dot(move,move),b=Dot(m,move);
            const double disc=b*b-a*c;
            if (a<1e-12 || b>=0 || disc<0) continue;
            const double t=(-b-std::sqrt(disc))/a;
            if (t<0 || t>1) continue;
            const FVector n=Normalize(from+move*t-s.center);
            take(t,n,s.center+n*s.radius,false,s.id);
        }
        for (const auto& p:planes) {
            const double from_d=Dot(from-p.point,p.normal)-radius;
            const double to_d=Dot(to-p.point,p.normal)-radius;
            if (from_d<=0) { take(0,p.normal,from,true,p.id); continue; }
            if (to_d>=from_d || to_d>0) continue;
            const double t=from_d/(from_d-to_d);
            take(t,p.normal,from+move*t-p.normal*radius,false,p.id);
        }
        for(const auto& slab:slabs) {
            const double lo=slab.lo-radius,hi=slab.hi+radius;
            if(from.Y>=lo && from.Y<=hi) {
                take(0,{0,from.Y<(slab.lo+slab.hi)*.5?-1.0:1.0,0},from,true,slab.id);
            } else if(from.Y<lo && to.Y>=lo) {
                const double t=(lo-from.Y)/move.Y;
                const auto center=from+move*t;
                take(t,{0,-1,0},{center.X,slab.lo,center.Z},false,slab.id);
            } else if(from.Y>hi && to.Y<=hi) {
                const double t=(hi-from.Y)/move.Y;
                const auto center=from+move*t;
                take(t,{0,1,0},{center.X,slab.hi,center.Z},false,slab.id);
            }
        }
        if(best<=1) engine_calls+=trace_passes;
        return best<=1 ? w::QueryStatus::hit : w::QueryStatus::clear;
    }
#ifndef QUERY_BASELINE
    bool SurfaceTransform(std::uint64_t id,skel::Xf& out) override {
        if(!rigid || !id) return false;
        ++frame_calls; ++engine_calls; out=frame;
        if(invalid_frame) out.scale.X=0;
        return true;
    }
    int InvocationCount() const override { return engine_calls; }
#endif
};

static w::Geometry Hand(bool all=true) {
    w::Geometry g;
    for (int f=0;f<5;++f) {
        g.valid[f]=all || f==0;
        for (int j=0;j<4;++j) g.point[f][j]={1.0+2*j,2.0*f,0};
    }
    return g;
}
static int Count(const finger_contact::Contacts c[5]) {
    int n=0; for (int f=0;f<5;++f) n+=c[f].count; return n;
}

static void Benchmark(int trace_passes) {
    const double rates[]={25,35,45,72};
    for(double rate:rates) for(int blocked_candidate=0;blocked_candidate<2;++blocked_candidate) {
        World scene; scene.trace_passes=trace_passes; scene.rigid=true;
        scene.planes.push_back({{0,0,0},{0,0,1},31});
        auto candidate=Hand(),displayed=candidate;
        for(auto& digit:candidate.point) for(auto& p:digit) p.Z=blocked_candidate ? -.15 : .49;
        for(auto& digit:displayed.point) for(auto& p:digit) p.Z=blocked_candidate ? .55 : .49;
        w::State state; finger_contact::Contacts contacts[5];
        int casts=0,calls=0,expired=0,missing=0,unresolved=0;
        for(int frame=0;frame<360;++frame) {
            w::Commit(state,displayed);
            const int before=scene.engine_calls;
            const auto stats=w::Update(state,candidate,1/rate,scene,contacts);
            if(frame>=20) {
                casts+=stats.casts; calls+=scene.engine_calls-before; expired+=stats.expired;
#ifndef QUERY_BASELINE
                unresolved+=stats.unresolved_mask!=0;
#endif
                for(int f=0;f<5;++f) for(int j=0;j<3;++j) {
                    bool found=false;
                    for(int c=0;c<contacts[f].count;++c) found|=contacts[f].items[c].segment==j;
                    missing+=!found;
                }
            }
        }
        std::printf("BENCH rate=%.0f pose=%s casts/hand/frame=%.3f engine_invocations=%.3f expired=%d missing_segments=%d unresolved_frames=%d\n",
                    rate,blocked_candidate?"blocked_preview":"seated",casts/340.0,calls/340.0,expired,missing,unresolved);
    }
}

int main(int argc,char** argv) {
    if(argc>1) { Benchmark(std::strcmp(argv[1],"--bench-nearest")==0 ? 2 : 1); return 0; }
#ifdef QUERY_BASELINE
    return 0;
#else
    constexpr double dt=1.0/90;
    const double nan=std::numeric_limits<double>::quiet_NaN();
    w::Settings invalid_settings;
    invalid_settings.clear_refresh_seconds=nan;
    invalid_settings.max_contact_age_seconds=nan;
    invalid_settings.movement_threshold_cm=nan;
    invalid_settings.teleport_distance_cm=nan;
    finger_contact::Contacts contacts[5];
    // A stationary hand must notice an object moving into it within the
    // refresh deadline, despite a prior clear coarse query.
    {
        World scene; w::State state; const auto g=Hand(false);
        auto first=w::Update(state,g,dt,scene,contacts,invalid_settings);
        EXPECT(first.available && first.casts==3 && !first.nearby);
        w::Commit(state,g);
        scene.spheres.push_back({{5,0,0},0.7});
        int found=0;
        for (int frame=1;frame<=5;++frame) {
            const auto stats=w::Update(state,g,dt,scene,contacts,invalid_settings);
            if (Count(contacts)) { found=frame; EXPECT(stats.nearby); break; }
        }
        EXPECT(found>0 && found<=5);
        EXPECT(contacts[0].count>0);
        for (int f=1;f<5;++f) EXPECT(contacts[f].count==0);
    }
    // A broad sphere's missed initial overlap must not veto actual finger
    // sweeps. This models mesh-query paths absent from the earlier tests.
    {
        World scene; scene.miss_broad=true;
        scene.spheres.push_back({{4,0,0},.6,42});
        w::State state;
        const auto stats=w::Update(state,Hand(false),dt,scene,contacts);
        EXPECT(stats.available && stats.primary_hits>0 && stats.broad_false_clear==0);
        EXPECT(Count(contacts)>0 && !state.broad_clear && stats.casts<=32);
    }
    // A requested phalanx enters the back half of a small object. The raw
    // overlap normal flips, but a fresh verified recast retains its outside
    // approach side instead of flipping the contact normal every frame.
    {
        World scene;scene.spheres.push_back({{3,0,0},1,77});
        auto g=Hand(false);g.point[0][0]={0,0,0};g.point[0][1]={2.9,0,0};
        g.point[0][2]={5.5,0,0};g.point[0][3]={8,0,0};
        w::State state;
        w::Update(state,g,dt,scene,contacts);w::Commit(state,g);
        EXPECT(state.segment[0][1].contacts[0].normal.X<-.9);
        g.point[0][1].X=3.1;
        const auto stats=w::Update(state,g,dt,scene,contacts);
        const auto& c=state.segment[0][1].contacts[0];
        EXPECT(c.valid && c.recovered_overlap && c.surface_id==77 && c.normal.X<-.9);
        EXPECT(stats.recovered_overlaps>0 && stats.casts<=32);
    }
    // Desired and temporal traces can see opposite sides of one thin solid.
    // Keep the approach-side contact rather than feed an impossible pair of
    // half-spaces into the joint solver.
    {
        World scene;scene.slabs.push_back({-.1,.1,91});
        auto before=Hand(false),desired=before;
        for(auto& p:before.point[0]) p.Y=-.7;
        for(auto& p:desired.point[0]) p.Y=.2;
        w::State state;w::Commit(state,before);
        const auto stats=w::Update(state,desired,dt,scene,contacts);
        EXPECT(stats.available && Count(contacts)>0);
        for(int n=0;n<contacts[0].count;++n) EXPECT(contacts[0].items[n].normal.Y<-.9);
    }
    // Opposing planes on a shared mesh remain valid when there is enough
    // room for the finger between them (a corridor, not one solid slab).
    {
        World scene;
        scene.planes.push_back({{0,-.7,0},{0,1,0},92});
        scene.planes.push_back({{0,.7,0},{0,-1,0},92});
        auto before=Hand(false),desired=before;
        for(auto& p:before.point[0]) p.Y=-.4;
        for(auto& p:desired.point[0]) p.Y=.4;
        w::State state;w::Commit(state,before);
        const auto stats=w::Update(state,desired,dt,scene,contacts);
        bool upper=false,lower=false;
        for(int n=0;n<contacts[0].count;++n) {
            upper|=contacts[0].items[n].normal.Y<-.9;
            lower|=contacts[0].items[n].normal.Y>.9;
        }
        EXPECT(upper && lower && stats.conflicting_contacts==0);
    }
    // Curl motion invalidates a clear result even with an unmoved wrist.
    {
        World scene; w::State state; auto g=Hand(false);
        w::Update(state,g,dt,scene,contacts); w::Commit(state,g);
        g.point[0][3].Y=1;
        scene.spheres.push_back({{7,1,0},0.3});
        const auto stats=w::Update(state,g,dt,scene,contacts,invalid_settings);
        EXPECT(stats.casts>=1 && stats.primary_hits>0 && Count(contacts)>0);
    }
    // Curved geometry supplies local normals and finite contact patches.
    // An adjacent finger that does not intersect the sphere is unaffected.
    {
        // A later self/contact adjustment can change what is displayed while
        // the requested curl stays fixed. A clear-cache hit must validate
        // both geometries, not wait fifty milliseconds to see that motion.
        World scene; w::State state;
        const auto desired=Hand(false);
        w::Update(state,desired,dt,scene,contacts);
        auto displayed=desired; displayed.point[0][3].Y=1;
        w::Commit(state,displayed);
        scene.spheres.push_back({{7,1,0},.3});
        const auto stats=w::Update(state,desired,dt,scene,contacts);
        EXPECT(stats.casts>0 && stats.displayed_casts>0 && Count(contacts)>0);
    }
    {
        World scene; w::State state; const auto g=Hand();
        scene.spheres.push_back({{4,0,0},0.6});
        const auto stats=w::Update(state,g,dt,scene,contacts);
        EXPECT(stats.available && stats.casts<=32 && contacts[0].count>0);
        EXPECT(contacts[1].count==0 && contacts[4].count==0);
        for (int i=0;i<contacts[0].count;++i) {
            const auto& c=contacts[0].items[i];
            EXPECT(c.valid && c.segment>=0 && c.segment<3);
            EXPECT(std::fabs(Length(c.point-scene.spheres[0].center)-0.6)<1e-6);
            EXPECT(Dot(c.normal,Normalize(c.point-scene.spheres[0].center))>0.9999);
            EXPECT(c.patch_radius_cm>0 && c.patch_radius_cm<=2.5);
        }
    }
    // Temporal endpoint sweeps catch a thin object crossed between poses
    // even when every segment at the requested pose has passed beyond it.
    {
        World scene; w::State state; auto before=Hand(false);
        for (auto& p:before.point[0]) p.Y=-2;
        w::Update(state,before,dt,scene,contacts); w::Commit(state,before);
        auto after=before;
        for (auto& p:after.point[0]) p.Y=2;
        scene.spheres.push_back({{7,0,0},0.2});
        const auto stats=w::Update(state,after,dt,scene,contacts);
        EXPECT(stats.temporal_casts==3 && contacts[0].count>0);
        bool distal=false;
        for (int n=0;n<contacts[0].count;++n)
            if (contacts[0].items[n].segment==2) distal=true;
        EXPECT(distal);
    }
    // A failed query removes contact evidence instead of freezing its last
    // plane; moving away/removing an object likewise drops the old contacts.
    {
        // Requested fingers point along X while the displayed fingers are
        // bent toward Y. A sphere can touch the middle displayed phalanx
        // while missing BOTH requested segments and all endpoint paths.
        // Continuous controller translation must not starve this category.
        World scene; w::State state;
        auto desired=Hand(),displayed=desired;
        for(int f=0;f<5;++f) for(int j=0;j<4;++j) {
            const double length[4]={0,4,7,9.4};
            desired.point[f][j]={length[j],0,3.0*f};
            displayed.point[f][j]={0,length[j],3.0*f};
        }
        // Nearby geometry keeps the coarse gate active without touching a
        // desired/displayed segment or its temporal endpoint paths.
        scene.spheres.push_back({{4,4,0},.3});
        w::Update(state,desired,dt,scene,contacts);
        w::Commit(state,displayed);
        scene.spheres.push_back({{0,5.5,12},.3});
        bool found=false;
        int display_casts=0,temporal_casts=0;
        for(int frame=0;frame<2;++frame) {
            desired.wrist.X+=.2; displayed.wrist.X+=.2;
            for(auto& digit:desired.point) for(auto& p:digit) p.X+=.2;
            for(auto& digit:displayed.point) for(auto& p:digit) p.X+=.2;
            const auto stats=w::Update(state,desired,dt,scene,contacts);
            EXPECT(stats.casts<=32 && stats.budget_limited);
            display_casts+=stats.displayed_casts;
            temporal_casts+=stats.temporal_casts;
            for(int n=0;n<contacts[4].count;++n)
                if(contacts[4].items[n].segment==1) found=true;
            w::Commit(state,displayed);
        }
        EXPECT(found && display_casts>=15 && temporal_casts>=15);
    }
    {
        // A newly refreshed desired-pose contact must neither erase nor
        // rejuvenate a distinct displayed-pose contact that lacked budget.
        World scene; w::State state;
        auto desired=Hand(),displayed=desired;
        for(int f=0;f<5;++f) for(int j=0;j<4;++j) {
            const double length[4]={0,4,7,9.4};
            desired.point[f][j]={length[j],0,3.0*f};
            displayed.point[f][j]={0,length[j],3.0*f};
        }
        scene.spheres.push_back({{5.5,0,12},.3});
        scene.spheres.push_back({{0,5.5,12},.3});
        w::Commit(state,displayed);
        w::Settings options=invalid_settings; options.max_casts=64;
        w::Update(state,desired,dt,scene,contacts,options);
        EXPECT(state.segment[4][1].contacts[0].valid && state.segment[4][1].contacts[1].valid);
        options.max_casts=16;
        // Make every new primary observation ambiguous and expensive. The
        // displayed contact receives no successful refresh during this run.
        scene.overlap_only=true;
        w::InvalidateGeometry(state);
        for(int frame=1;frame<=5;++frame) {
            const auto stats=w::Update(state,desired,dt,scene,contacts,options);
            const auto& shown=state.segment[4][1].contacts[1];
            EXPECT(stats.casts<=16 && stats.unresolved_mask);
            if(frame<5) EXPECT(shown.valid && std::fabs(shown.age_seconds-frame*dt)<1e-9);
            else EXPECT(!shown.valid && stats.expired>0);
        }
    }
    {
        World scene; w::State state; const auto g=Hand(false);
        scene.spheres.push_back({{5,0,0},0.7});
        w::Update(state,g,dt,scene,contacts); w::Commit(state,g);
        EXPECT(Count(contacts)>0);
        scene.fail=true;
        auto stats=w::Update(state,g,dt,scene,contacts);
        EXPECT(!stats.available && Count(contacts)==0 && stats.failures==1);
        scene.fail=false;
        w::Update(state,g,dt,scene,contacts);
        EXPECT(Count(contacts)>0);
        scene.spheres.clear();
        w::Update(state,g,dt,scene,contacts);
        EXPECT(Count(contacts)==0);
    }
    // Initial overlaps are not treated as real impact points. When the
    // recovery sweep cannot obtain a surface, no tangent plane is invented.
    {
        World scene; scene.overlap_only=true;
        w::State state;
        const auto stats=w::Update(state,Hand(),dt,scene,contacts);
        EXPECT(stats.casts<=32 && Count(contacts)==0);
    }
    // Budget pressure is explicit and bounded. Previous evidence expires
    // even if a low budget repeatedly prevents querying the same segment.
    {
        World scene; scene.overlap_only=true;
        w::State state; auto g=Hand();
        w::Settings options; options.max_casts=16;
        for (int frame=0;frame<30;++frame) {
            g.wrist.X+=0.3;
            for (auto& digit:g.point) for (auto& p:digit) p.X+=0.3;
            const auto stats=w::Update(state,g,dt,scene,contacts,options);
            EXPECT(stats.casts<=16 && stats.budget_limited && Count(contacts)==0);
            w::Commit(state,g);
        }
    }
    {
        World scene; w::State state; auto before=Hand();
        scene.spheres.push_back({{5,0,0},0.7});
        w::Update(state,before,dt,scene,contacts);
        EXPECT(Count(contacts)>0);
        w::Commit(state,before);
        auto moved=before;
        for (auto& digit:moved.point) for (auto& p:digit) p.Y+=4;
        // The displayed hand still touches the object while its candidate
        // has moved. Fresh displayed contact must remain until Commit says
        // the actual hand moved; cache starvation must not erase it.
        w::Settings options; options.max_casts=16;
        int last_count=0;
        for (int frame=0;frame<6;++frame) {
            const auto stats=w::Update(state,moved,dt,scene,contacts,options);
            EXPECT(stats.casts<=16);
            for (int f=0;f<5;++f) for (int n=0;n<contacts[f].count;++n)
                EXPECT(contacts[f].items[n].age_seconds<0.05);
            last_count=Count(contacts);
        }
        EXPECT(last_count>0);
        w::Commit(state,moved);
        w::Update(state,moved,dt,scene,contacts,options);
        EXPECT(Count(contacts)==0);
    }
    // Teleport/large rotation and invalid time reset history; there is no
    // long sweep through the level following a discontinuity.
    {
        World scene; w::State state; auto g=Hand(false);
        w::Update(state,g,dt,scene,contacts); w::Commit(state,g);
        auto teleported=g; teleported.wrist.X=100;
        for (auto& p:teleported.point[0]) p.X+=100;
        auto stats=w::Update(state,teleported,dt,scene,contacts,invalid_settings);
        EXPECT(stats.reset && stats.temporal_casts==0);
        w::Commit(state,teleported);
        teleported.orientation=AxisAngle({0,0,1},kPi);
        stats=w::Update(state,teleported,dt,scene,contacts);
        EXPECT(stats.reset && !state.have_rendered);
        stats=w::Update(state,teleported,0,scene,contacts);
        EXPECT(stats.reset && stats.casts==0 && !state.initialized && Count(contacts)==0);
        stats=w::Update(state,teleported,std::numeric_limits<double>::quiet_NaN(),scene,contacts);
        EXPECT(stats.reset && stats.casts==0);
    }
    // All15 overlapping phalanges keep a real, unexpired observation at
    // headset rates and recorded slow/jittered frame times. No cap increase
    // and no longer contact lifetime is needed to refresh this steady wall.
    for(double step: {1.0/25,1.0/35,1.0/45,1.0/72,0.0}) for(bool rigid:{false,true}) {
        World scene; scene.rigid=rigid; scene.planes.push_back({{0,0,0},{0,0,1},101});
        w::State state; auto g=Hand();
        for(auto& digit:g.point) for(auto& point:digit) point.Z=.49;
        for(int frame=0;frame<120;++frame) {
            const double seconds=step>0 ? step : frame%2 ? .05 : .02;
            const auto stats=w::Update(state,g,seconds,scene,contacts);
            EXPECT(stats.available && stats.casts<=32 && !stats.unresolved_mask);
            EXPECT(stats.expired==0 && stats.surface_queries<=4);
            EXPECT(Count(contacts)==15);
            for(const auto& digit:contacts) for(int n=0;n<digit.count;++n)
                EXPECT(digit.items[n].age_seconds<.05 && digit.items[n].normal.Z>.99);
            w::Commit(state,g);
        }
    }
    // A rigid surface moves during a cache hit: transform its finite feature
    // now, without resetting the original geometry observation's age.
    {
        World scene; scene.rigid=true; scene.planes.push_back({{0,0,0},{0,0,1},102});
        w::State state; auto g=Hand();
        for(auto& digit:g.point) for(auto& point:digit) point.Z=.49;
        w::Update(state,g,.02,scene,contacts); w::Commit(state,g);
        scene.frame.translation.Z=.2; scene.planes[0].point.Z=.2;
        const auto moved=w::Update(state,g,.02,scene,contacts);
        EXPECT(moved.casts==0 && moved.surface_queries==1 && moved.cache_hits==15);
        for(const auto& digit:contacts) for(int n=0;n<digit.count;++n) {
            EXPECT(std::fabs(digit.items[n].point.Z-.2)<1e-9);
            EXPECT(std::fabs(digit.items[n].age_seconds-.02)<1e-9);
        }
        scene.planes.clear();
        const auto removed=w::Update(state,g,.05,scene,contacts);
        EXPECT(removed.casts==15 && Count(contacts)==0);
    }
    // Previously clear observations still audit all15 segments on schedule
    // when independent objects approach an entirely stationary hand.
    for(double step: {1.0/25,1.0/35,1.0/45,1.0/72,0.0}) {
        World scene; w::State state; const auto g=Hand();
        w::Update(state,g,.02,scene,contacts); w::Commit(state,g);
        for(int f=0;f<5;++f) for(int j=0;j<3;++j)
            scene.spheres.push_back({{2.0+2*j,2.0*f,0},.1,static_cast<std::uint64_t>(110+f)});
        double elapsed=0,last_step=0;
        int found=0;
        for(int frame=0;frame<12;++frame) {
            last_step=step>0 ? step : frame%2 ? .05 : .02;
            elapsed+=last_step;
            const auto stats=w::Update(state,g,last_step,scene,contacts);
            EXPECT(stats.casts<=32);
            if(Count(contacts)==15) { found=frame+1; break; }
        }
        EXPECT(found && elapsed<=.05+last_step+1e-9);
    }
    // Rigid feature normals use inverse-transpose scaling, including a
    // nonuniform component scale, and retain the original query deadline.
    {
        World scene; scene.rigid=true;
        scene.planes.push_back({{0,0,0},Normalize({1,0,1}),121});
        w::State state; auto g=Hand(false);
        for(int j=0;j<4;++j) g.point[0][j]={.1*j,0,.6-.1*j};
        w::Update(state,g,.02,scene,contacts); w::Commit(state,g);
        scene.frame.scale={2,1,.5};
        scene.planes[0].normal=Normalize({.5,0,2});
        const auto stats=w::Update(state,g,.02,scene,contacts);
        EXPECT(stats.casts==0 && stats.cache_hits==3 && stats.surface_queries==1);
        for(int n=0;n<contacts[0].count;++n) {
            EXPECT(Dot(contacts[0].items[n].normal,scene.planes[0].normal)>.99999);
            EXPECT(std::fabs(contacts[0].items[n].age_seconds-.02)<1e-9);
        }
        // A component that stops supporting rigid tracking must be queried
        // now, even before its previous50ms observation would have expired.
        scene.rigid=false; scene.planes.clear();
        const auto gone=w::Update(state,g,.01,scene,contacts);
        EXPECT(gone.casts==6 && gone.feature_casts==3 && Count(contacts)==0);
    }
    // Bad/unsupported rigid transforms use fresh geometry rather than turn
    // an invalid transform into a permanent surface or a false clear.
    {
        World scene; scene.rigid=true; scene.planes.push_back({{0,0,0},{0,0,1},103});
        w::State state; auto g=Hand();
        for(auto& digit:g.point) for(auto& point:digit) point.Z=.49;
        w::Update(state,g,.02,scene,contacts); w::Commit(state,g);
        scene.invalid_frame=true;
        const auto stats=w::Update(state,g,.02,scene,contacts);
        EXPECT(stats.casts==15 && stats.feature_casts==15 && Count(contacts)==15);
        EXPECT(!stats.unresolved_mask && stats.surface_queries==1);
        w::InvalidateGeometry(state);
        EXPECT(!state.segment[0][0].observation[0].valid);
        EXPECT(state.segment[0][0].contacts[0].valid);
        const auto audit=w::Update(state,g,.02,scene,contacts);
        EXPECT(audit.casts==30 && audit.feature_casts==0 && Count(contacts)==15);
    }
    // The minimum budget cannot recover15 overlaps; disclose exactly that
    // missing evidence so integration can hold affected digits this frame.
    {
        World scene; scene.planes.push_back({{0,0,0},{0,0,1},104});
        w::State state; auto g=Hand(); w::Settings options; options.max_casts=16;
        for(auto& digit:g.point) for(auto& point:digit) point.Z=.49;
        const auto stats=w::Update(state,g,.04,scene,contacts,options);
        EXPECT(stats.available && stats.casts==16 && stats.budget_limited);
        EXPECT(stats.unresolved_mask && stats.unresolved_segments && Count(contacts)==8);
    }
    // Whole-hand pressure correction can clear its known wall yet enter a
    // different object. Endpoint verification catches that new obstacle
    // using only the remaining frame budget, without aging any observation.
    {
        World scene; scene.rigid=true;
        scene.planes.push_back({{0,0,0},{0,0,1},131});
        scene.spheres.push_back({{3,0,1.49},.1,132});
        w::State state; auto g=Hand();
        for(auto& digit:g.point) for(auto& point:digit) point.Z=.49;
        const auto query=w::Update(state,g,.02,scene,contacts);
        EXPECT(query.casts==30);
        auto corrected=g;
        for(auto& digit:corrected.point) for(auto& point:digit) point.Z+=1;
        corrected.wrist.Z+=1;
        const double age=state.segment[0][0].contacts[0].age_seconds;
        const auto blocked=w::VerifyGeometry(state,corrected,scene);
        EXPECT(blocked.available && !blocked.clear && blocked.blocked_mask==1);
        EXPECT(query.casts+blocked.casts<=32);
        EXPECT(state.segment[0][0].contacts[0].age_seconds==age);
        scene.spheres.clear();
        const auto unknown=w::VerifyGeometry(state,corrected,scene);
        EXPECT(!unknown.available && !unknown.clear && unknown.budget_limited);
        EXPECT(state.frame_casts==32);
        EXPECT(state.segment[0][0].contacts[0].age_seconds==age);
        // Begin another frame with the staged corrected geometry. A query
        // which has15clear primary capsules leaves enough budget to verify
        // that same final geometry, rather than permanently freeze recovery.
        const auto next=w::Update(state,corrected,.02,scene,contacts);
        const auto clear=w::VerifyGeometry(state,corrected,scene);
        EXPECT(next.casts==15 && clear.available && clear.clear && clear.casts==15);
        EXPECT(state.frame_casts==30 && w::MaxDelta(g,corrected)==1);
    }
    // A small solved skin gap retains a freshly measured nearby support.
    // Otherwise each clear audit drops support and the following spring
    // step pushes the same fingers back through the wall.
    for(bool rigid:{false,true}) {
        World scene; scene.rigid=rigid; scene.planes.push_back({{0,0,0},{0,0,1},141});
        w::State state; auto g=Hand();
        for(auto& digit:g.point) for(auto& point:digit) point.Z=.49;
        w::Update(state,g,.03,scene,contacts); w::Commit(state,g);
        for(auto& digit:g.point) for(auto& point:digit) point.Z=.53;
        bool certified_clear=false;
        for(int frame=0;frame<40;++frame) {
            const auto stats=w::Update(state,g,1.0/35,scene,contacts);
            EXPECT(stats.available && !stats.unresolved_mask && stats.casts<=32);
            EXPECT(Count(contacts)==15 && stats.expired==0);
            certified_clear|=stats.geometry_clear;
            w::Commit(state,g);
        }
        EXPECT(certified_clear);
        scene.frame.translation.Z=.08; scene.planes[0].point.Z=.08;
        const auto intruding=w::Update(state,g,.01,scene,contacts);
        EXPECT(!intruding.geometry_clear && Count(contacts)==15);
        scene.planes.clear();
        const auto removed=w::Update(state,g,.05,scene,contacts);
        EXPECT(removed.geometry_clear && Count(contacts)==0 && removed.casts<=32);
    }
    // Constraint reuse's motion tolerance is not exact final clearance. A
    // small changed capsule must still pass endpoint verification before it
    // is displayed, even when the earlier clear query can be reused.
    {
        World scene; scene.spheres.push_back({{4,.6,0},.02,145});
        w::State state; auto g=Hand(false);
        const auto clear=w::Update(state,g,.01,scene,contacts);
        EXPECT(clear.geometry_clear); w::Commit(state,g);
        for(auto& p:g.point[0]) p.Y+=.12;
        const auto reused=w::Update(state,g,.01,scene,contacts);
        EXPECT(reused.casts==0 && reused.cache_hits==3 && !reused.geometry_clear);
        const auto verified=w::VerifyGeometry(state,g,scene);
        EXPECT(verified.available && !verified.clear && verified.blocked_mask==1);
    }
    // Committing exactly queried full-radius clear geometry supplies the
    // next frame's displayed observation. Continuous free movement keeps all
    //15 primary and15 temporal checks inside32casts at120Hz, with no redundant
    //15 displayed checks that previously starved alternate release frames.
    {
        World scene; w::State state; auto g=Hand();
        auto first=w::Update(state,g,1.0/120,scene,contacts);
        EXPECT(first.geometry_clear && first.casts==15); w::Commit(state,g);
        for(int frame=0;frame<40;++frame) {
            g.wrist.Y+=.3;
            for(auto& digit:g.point) for(auto& p:digit) p.Y+=.3;
            const auto stats=w::Update(state,g,1.0/120,scene,contacts);
            EXPECT(stats.geometry_clear && !stats.unresolved_mask && stats.casts==30);
            EXPECT(stats.temporal_casts==15 && stats.displayed_casts==0);
            w::Commit(state,g);
        }
        w::Update(state,g,.02,scene,contacts);
        const auto original_age=state.segment[0][0].observation[0].age_seconds;
        w::Commit(state,g);
        EXPECT(original_age>.019 && state.segment[0][0].observation[1].age_seconds==original_age);
        auto altered=g; altered.point[0][1].Z+=.01;
        w::Commit(state,altered);
        EXPECT(Length(state.segment[0][0].observation[1].to-altered.point[0][1])>.009);
    }
    // Each phalanx uses its fitted world radius, including final verification
    // and the contact data returned to rigid backpressure. The fallback
    // radius cannot silently replace a larger measured mesh envelope.
    {
        World scene; scene.planes.push_back({{0,0,0},{0,0,1},148});
        w::State state; auto g=Hand(false);
        for(auto& p:g.point[0]) p.Z=.8;
        g.segment_radius_cm[0][0]=.3; g.segment_radius_cm[0][1]=.9; g.segment_radius_cm[0][2]=1.2;
        const auto stats=w::Update(state,g,.02,scene,contacts);
        EXPECT(stats.available && !stats.geometry_clear && stats.casts<=32);
        bool middle=false,distal=false;
        for(int n=0;n<contacts[0].count;++n) {
            const auto& c=contacts[0].items[n];
            EXPECT(c.segment!=0);
            if(c.segment==1) { middle=true; EXPECT(c.radius_cm==.9); }
            if(c.segment==2) { distal=true; EXPECT(c.radius_cm==1.2); }
        }
        EXPECT(middle && distal);
        const auto verified=w::VerifyGeometry(state,g,scene);
        EXPECT(verified.available && !verified.clear);
        w::Commit(state,g);
        g.segment_radius_cm[0][1]=.2;
        EXPECT(w::Update(state,g,.02,scene,contacts).reset);
        g.segment_radius_cm[0][2]=std::numeric_limits<double>::quiet_NaN();
        const auto invalid=w::Update(state,g,.02,scene,contacts);
        EXPECT(invalid.reset && !invalid.available && invalid.casts==0);
    }
    // The first-touch patch can miss a deeper endpoint far along the wall.
    // Keep the real base contact and obtain an additional real endpoint
    // plane, with its own finite patch, instead of widening the first plane.
    {
        World scene; scene.planes.push_back({{0,0,0},{0,0,1},151});
        w::State state; auto g=Hand(false);
        g.point[0][0]={0,0,.7}; g.point[0][1]={4,0,-1};
        g.point[0][2]={7,0,-2}; g.point[0][3]={9,0,-3};
        const auto stats=w::Update(state,g,.03,scene,contacts);
        int endpoint_contacts=0;
        for(int i=0;i<contacts[0].count;++i) {
            const auto& c=contacts[0].items[i];
            if(c.source==finger_contact::Contact::Source::endpoint) {
                ++endpoint_contacts;
                EXPECT(c.fraction==1 && std::fabs(c.point.Z)<1e-8 && c.normal.Z>.999);
                EXPECT(std::fabs(c.point.X-g.point[0][c.segment+1].X)<1e-8);
            }
            EXPECT(c.patch_radius_cm<=2.5);
        }
        EXPECT(stats.available && !stats.geometry_clear && !stats.unresolved_mask);
        EXPECT(stats.casts<=32 && endpoint_contacts==3 && contacts[0].count==6);
        const auto verification=w::VerifyGeometry(state,g,scene);
        EXPECT(verification.available && !verification.clear);
    }
    // A recovery ray can meet a different object before reaching the
    // original overlap. That hit does not establish the original wall's
    // surface and must not silently replace its unresolved constraint.
    {
        World scene; scene.planes.push_back({{0,0,0},{0,0,1},161});
        auto g=Hand(false);
        for(auto& p:g.point[0]) p.Z=.1;
        for(int j=0;j<3;++j) scene.spheres.push_back({{1.0+2*j,0,1.1},.2,162});
        w::State state;
        const auto stats=w::Update(state,g,.03,scene,contacts);
        EXPECT(stats.unresolved_mask && !stats.geometry_clear);
        for(int i=0;i<contacts[0].count;++i) EXPECT(contacts[0].items[i].surface_id!=162);
    }
    if (failures) { std::printf("finger world: %d failures\n",failures); return 1; }
    std::printf("finger world: all tests passed\n");
    return 0;
#endif
}
