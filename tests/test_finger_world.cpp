#include <algorithm>
#include <cmath>
#include <cstdio>
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
    int calls=0;
    bool fail=false, overlap_only=false, miss_broad=false;
    w::QueryStatus Sweep(const FVector& from,const FVector& to,double radius,w::Hit& hit) override {
        ++calls;
        if (fail) return w::QueryStatus::unavailable;
        if (miss_broad && radius>3) return w::QueryStatus::clear;
        if (overlap_only) {
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
        return best<=1 ? w::QueryStatus::hit : w::QueryStatus::clear;
    }
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

int main() {
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
        EXPECT(first.available && first.casts==4 && !first.nearby);
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
        EXPECT(stats.available && stats.primary_hits>0 && stats.broad_false_clear==1);
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
        EXPECT(stats.conflicting_contacts>0 && Count(contacts)>0);
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
        EXPECT(stats.casts>1 && Count(contacts)>0);
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
        for(int frame=1;frame<=5;++frame) {
            const auto stats=w::Update(state,desired,dt,scene,contacts,options);
            const auto& requested=state.segment[4][1].contacts[0];
            const auto& shown=state.segment[4][1].contacts[1];
            EXPECT(stats.casts<=16 && requested.valid && requested.age_seconds==0);
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
        // Coarse proximity remains true, but the requested phalanges are
        // clear. With only sixteen casts, no temporal query can replace the
        // old contact. It may bridge that brief gap, never persist forever.
        w::Settings options; options.max_casts=16;
        int last_count=0;
        for (int frame=0;frame<6;++frame) {
            const auto stats=w::Update(state,moved,dt,scene,contacts,options);
            EXPECT(stats.casts<=16 && stats.budget_limited);
            for (int f=0;f<5;++f) for (int n=0;n<contacts[f].count;++n)
                EXPECT(contacts[f].items[n].age_seconds<0.05);
            last_count=Count(contacts);
        }
        EXPECT(last_count==0);
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
    if (failures) { std::printf("finger world: %d failures\n",failures); return 1; }
    std::printf("finger world: all tests passed\n");
    return 0;
}
