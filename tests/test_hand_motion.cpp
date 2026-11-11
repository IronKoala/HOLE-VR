#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>
#include "vr/body/hand_motion.h"
#include "vr/body/hand_rigid_contact.h"

using namespace holevr::vr;
using namespace holevr::vr::ik;
namespace hm = holevr::vr::body::hand_motion;
namespace rigid = holevr::vr::body::hand_rigid_contact;
static int failures = 0;
#define EXPECT(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

struct Plane { FVector point, normal; };
struct World {
    std::vector<Plane> planes;
    bool available = true;
    int queries = 0;
    int fail_query = 0;
    hm::Sweep operator()(const FVector& from, const FVector& to, double radius) {
        ++queries;
        hm::Sweep result;
        result.available = available && queries != fail_query;
        if (!result.available) return result;
        double earliest = 2;
        for (const auto& p : planes) {
            const double d0 = Dot(from-p.point,p.normal), d1 = Dot(to-p.point,p.normal);
            if (d0 < radius-1e-8) {
                result.hit = result.initial_overlap = true;
                result.centre = from; result.normal = p.normal;
                result.point = from - p.normal*d0;
                return result;
            }
            if (d1 >= radius-1e-10 || d0-d1 < 1e-12) continue;
            const double t = std::clamp((d0-radius)/(d0-d1), 0.0, 1.0);
            if (t >= earliest) continue;
            earliest = t;
            result.hit = true; result.centre = from+(to-from)*t;
            result.normal = p.normal; result.point = result.centre-p.normal*radius;
        }
        return result;
    }
    static double Support(const FVector& half, const FQuat& rotation, const FVector& normal) {
        return std::fabs(Dot(rotation.Rotate({1,0,0}), normal))*half.X +
               std::fabs(Dot(rotation.Rotate({0,1,0}), normal))*half.Y +
               std::fabs(Dot(rotation.Rotate({0,0,1}), normal))*half.Z;
    }
    hm::Sweep Box(const FVector& from, const FVector& to, const FVector& half, const FQuat& rotation) {
        ++queries;
        hm::Sweep result; result.available=available && queries!=fail_query;
        if (!result.available) return result;
        double earliest=2;
        for (const auto& p:planes) {
            const double radius=Support(half,rotation,p.normal);
            const double d0=Dot(from-p.point,p.normal), d1=Dot(to-p.point,p.normal);
            if (d0<radius-1e-8) {
                result.hit=result.initial_overlap=true; result.centre=from;
                result.normal=p.normal; result.point=from-p.normal*d0; return result;
            }
            if (d1>=radius-1e-10 || d0-d1<1e-12) continue;
            const double t=std::clamp((d0-radius)/(d0-d1),0.0,1.0);
            if (t>=earliest) continue;
            earliest=t; result.hit=true; result.centre=from+(to-from)*t;
            result.normal=p.normal; result.point=result.centre-p.normal*radius;
        }
        return result;
    }
};
static hm::Result Step(hm::State& s, const hm::Settings& cfg, hm::Input& input, World& w) {
    return hm::Step(s,cfg,input,[&](const FVector& a,const FVector& b,double r){ return w(a,b,r); },
        [&](const FVector& a,const FVector& b,const FVector& half,const FQuat& q){ return w.Box(a,b,half,q); });
}
static void Initialize(hm::State& s, const hm::Settings& cfg, hm::Input& input, World& w) {
    input.target = input.anchor = {};
    input.rotation = {};
    input.dt = 1.0/90;
    EXPECT(Step(s,cfg,input,w).valid);
}
static hm::PalmShape Palm() {
    hm::PalmShape palm; palm.valid=true;
    palm.centre={-1.5,0,0};
    palm.rotation=AlignFrames({0,0,1},{0,1,0},{1,0,0},{0,0,1});
    palm.half={3.5,5.5,1.5};
    return palm;
}
static double PalmGap(const hm::Result& r,const hm::PalmShape& palm,const Plane& plane) {
    const FVector centre=r.position+r.rotation.Rotate(palm.centre);
    return Dot(centre-plane.point,plane.normal)-World::Support(palm.half,r.rotation*palm.rotation,plane.normal);
}
int main() {
    hm::Settings cfg;
    // A continuously swept proxy stops at the sphere's skin, even for a
    // rapid target displacement. Sustained disagreement never snaps it in.
    {
        World w{{{{10,0,0},{-1,0,0}}}};
        hm::State s; hm::Input input;
        Initialize(s,cfg,input,w);
        input.target = {75,0,0};
        hm::Result r;
        for (int n=0;n<180;++n) {
            r=Step(s,cfg,input,w);
            EXPECT(r.valid && r.position.X<=6.0+1e-7);
            EXPECT(r.peak_force_n<=cfg.max_force_n+1e-9);
            EXPECT(r.speed_cm_s<=cfg.max_speed_cm_s+1e-9);
            EXPECT(r.queries<=3);
        }
        EXPECT(r.touching && r.recovering && r.error_cm>60);
        input.target={2,0,0};
        for(int n=0;n<60;++n) r=Step(s,cfg,input,w);
        EXPECT(!r.touching && !r.recovering && std::fabs(r.position.X-2)<0.01);
    }
    // Oblique pressure slides along a wall; a second wall stops the slide.
    {
        World w{{{{10,0,0},{-1,0,0}}}};
        hm::State s; hm::Input input; Initialize(s,cfg,input,w);
        input.target={20,25,0};
        bool slid=false;
        for(int n=0;n<100;++n) { auto r=Step(s,cfg,input,w); slid|=r.sliding; EXPECT(r.position.X<=6.0+1e-7); }
        EXPECT(slid && s.position.Y>20);
        hm::Reset(s); Initialize(s,cfg,input,w);
        w.planes.push_back({{0,10,0},{0,-1,0}});
        input.target={20,20,0};
        for(int n=0;n<100;++n) { auto r=Step(s,cfg,input,w); EXPECT(r.position.X<=6.0+1e-6 && r.position.Y<=6.0+1e-6); }
    }
    // Stationary tracking cannot preserve an old "clear" cache: a moving
    // wall overtakes it, forcing fresh swept recovery on the very next step.
    {
        World w; hm::State s; hm::Input input;
        input.anchor={0,0,0}; input.target={8,0,0}; input.dt=1.0/90;
        EXPECT(Step(s,cfg,input,w).position.X==8);
        const int before=w.queries;
        for(int n=0;n<20;++n) Step(s,cfg,input,w);
        EXPECT(w.queries>=before+20);
        w.planes.push_back({{6,0,0},{-1,0,0}});
        const auto r=Step(s,cfg,input,w);
        EXPECT(r.valid && r.touching && r.recovering && r.position.X<2.001);
        w.planes.clear();
        EXPECT(!Step(s,cfg,input,w).touching);
    }
    // Tracking reset and teleport recovery are swept from the tracking
    // anchor. A huge target jump cannot put the proxy across the blocker.
    {
        World w{{{{10,0,0},{-1,0,0}}}};
        hm::State s; hm::Input input; Initialize(s,cfg,input,w);
        input.target={100,0,0};
        auto r=Step(s,cfg,input,w);
        EXPECT(r.reset && r.touching && r.position.X<6.001);
        input.tracked=false;
        r=Step(s,cfg,input,w);
        EXPECT(!r.valid && !s.valid && Length(s.velocity)==0);
        input.tracked=true;
        r=Step(s,cfg,input,w);
        EXPECT(r.valid && r.reset && r.touching && r.position.X<6.001);
        input.anchor={20,0,0}; input.discontinuity=true;
        r=Step(s,cfg,input,w);
        EXPECT(!r.valid); // an overlapping anchor provides no verified placement
    }
    // Failed geometry queries hold the last valid position and discard
    // motion/contact history instead of treating query failure as empty space.
    {
        World w; hm::State s; hm::Input input; Initialize(s,cfg,input,w);
        input.target={20,0,0}; w.available=false;
        const auto r=Step(s,cfg,input,w);
        EXPECT(r.query_failed && r.position.X==0 && !r.touching && Length(s.velocity)==0);
        hm::Reset(s);
        EXPECT(!Step(s,cfg,input,w).valid);
    }
    // Stable time integration at common refresh rates with a moving target.
    FVector ends[3]; int k=0;
    for(double hz:{72.0,90.0,120.0}) {
        World w; hm::State s; hm::Input input; Initialize(s,cfg,input,w);
        input.dt=1.0/hz;
        for(int n=1;n<=static_cast<int>(hz);++n) {
            input.target={100*n/hz,0,0}; input.rotation=ToQuat({0,90,0});
            auto r=Step(s,cfg,input,w);
            EXPECT(r.valid && r.peak_force_n<=cfg.max_force_n+1e-8 && r.peak_torque_nm<=cfg.max_torque_nm+1e-8);
            EXPECT(Length(s.angular_velocity)<=cfg.max_angular_speed+1e-8 && r.substeps<=36);
        }
        ends[k++]=s.position;
        EXPECT(Length(input.target-s.position)<5.0);
        EXPECT(AngleBetween(s.rotation.Rotate({1,0,0}),input.rotation.Rotate({1,0,0}))<0.01);
    }
    EXPECT(Length(ends[0]-ends[1])<0.75 && Length(ends[1]-ends[2])<0.75);
    // Invalid/zero frame times and long pauses remain finite. A zero-time
    // update still observes a moving object; it does not advance the spring.
    {
        World w; hm::State s; hm::Input input; Initialize(s,cfg,input,w);
        input.target={20,0,0}; input.dt=0;
        auto r=Step(s,cfg,input,w);
        EXPECT(r.position.X==0 && r.queries>0);
        input.dt=std::numeric_limits<double>::quiet_NaN();
        EXPECT(Step(s,cfg,input,w).position.X==0);
        input.dt=2.0; w.planes.push_back({{10,0,0},{-1,0,0}});
        r=Step(s,cfg,input,w);
        EXPECT(r.reset && r.valid && r.position.X<6.001 && Length(s.velocity)==0);
        input.target.X=std::numeric_limits<double>::infinity();
        EXPECT(!Step(s,cfg,input,w).valid && !s.valid);
    }
    // Rotating/translating the whole setup gives the same contact motion.
    {
        const FQuat q=ToQuat({20,110,-15}); const FVector offset{90,-30,42};
        World a, b;
        a.planes.push_back({{10,0,0},{-1,0,0}});
        b.planes.push_back({offset+q.Rotate({10,0,0}),q.Rotate({-1,0,0})});
        hm::State sa,sb; hm::Input ia,ib;
        ia.dt=ib.dt=1.0/90;
        ib.anchor=ib.target=offset;
        Step(sa,cfg,ia,a); Step(sb,cfg,ib,b);
        ia.target={20,10,0}; ib.target=offset+q.Rotate(ia.target);
        for(int n=0;n<90;++n) { Step(sa,cfg,ia,a); Step(sb,cfg,ib,b); }
        EXPECT(Length(sb.position-(offset+q.Rotate(sa.position)))<1e-5);
    }
    // The actual calibrated palm pad reaches a wall. A grip-centred 4 cm
    // sphere would incorrectly leave this palm four centimetres away.
    {
        World w{{{{10,0,0},{-1,0,0}}}};
        hm::State s; hm::Input in; in.palm=Palm(); Initialize(s,cfg,in,w);
        in.target={20,0,0}; hm::Result r;
        for (int i=0;i<90;++i) {
            r=Step(s,cfg,in,w);
            EXPECT(r.valid && PalmGap(r,in.palm,w.planes[0])>=-1e-7);
        }
        EXPECT(r.touching && r.flatness>.99);
        EXPECT(PalmGap(r,in.palm,w.planes[0])<0.04);
        EXPECT(r.position.X>9.9);
        in.target={20,20,0}; bool slid=false;
        for(int i=0;i<90;++i) { r=Step(s,cfg,in,w); slid|=r.sliding; EXPECT(PalmGap(r,in.palm,w.planes[0])>=-1e-7); }
        EXPECT(slid && r.position.Y>19);
        in.target={0,20,0};
        for(int i=0;i<90;++i) r=Step(s,cfg,in,w);
        EXPECT(!r.touching && r.flatness==0 && std::fabs(r.position.X)<0.01);
    }
    // Rotation checks the whole palm volume, not just its grip point. The
    // hand can seat against a wall at a new angle and cannot turn through a
    // second wall at a corner. Intermediate angular clearance is swept too.
    {
        World w{{{{10,0,0},{-1,0,0}},{{0,8,0},{0,-1,0}}}};
        hm::State s; hm::Input in; in.palm=Palm(); in.palm.flatten=false;
        Initialize(s,cfg,in,w); in.target={20,20,0}; hm::Result r;
        for(int i=0;i<45;++i) r=Step(s,cfg,in,w);
        in.rotation=ToQuat({20,45,30});
        for(int i=0;i<180;++i) {
            r=Step(s,cfg,in,w);
            EXPECT(r.valid);
            for(const auto& plane:w.planes) EXPECT(PalmGap(r,in.palm,plane)>=-1e-6);
        }
        EXPECT(AngleBetween(r.rotation.Rotate({1,0,0}),in.rotation.Rotate({1,0,0}))<0.03);
        EXPECT(r.flatness==0);
        in.target={-15,-15,0};
        for(int i=0;i<90;++i) r=Step(s,cfg,in,w);
        EXPECT(!r.touching && Length(r.position-in.target)<0.02);
    }
    // Pressure aligns the palm toward a fresh wall, then seats its face;
    // removing the wall withdraws flattening immediately, without a cache.
    {
        World w{{{{10,0,0},{-1,0,0}}}};
        hm::State s; hm::Input in; in.palm=Palm(); Initialize(s,cfg,in,w);
        in.target={20,0,0}; in.rotation=ToQuat({0,35,0}); hm::Result r;
        for(int i=0;i<180;++i) {
            r=Step(s,cfg,in,w); EXPECT(r.valid && PalmGap(r,in.palm,w.planes[0])>=-1e-6);
        }
        EXPECT(r.flatness>.99 && Dot((r.rotation*in.palm.rotation).Rotate({0,0,1}),{1,0,0})>.999);
        EXPECT(PalmGap(r,in.palm,w.planes[0])<0.04);
        w.planes.clear(); r=Step(s,cfg,in,w); EXPECT(!r.touching && r.flatness==0);
        w.available=false;
        const auto prior=s.rotation; const auto at=s.position;
        in.rotation=ToQuat({0,90,0}); r=Step(s,cfg,in,w);
        EXPECT(r.query_failed && Length(r.position-at)<1e-9);
        EXPECT(AngleBetween(r.rotation.Rotate({1,0,0}),prior.Rotate({1,0,0}))<1e-8);
    }
    // Long-frame angular motion cannot spend an unbounded number of world
    // queries. Partial turns stop at an already checked pose when exhausted.
    {
        World w{{{{10,0,0},{-1,0,0}}}};
        hm::State s; hm::Input in; in.palm=Palm(); in.palm.flatten=false;
        Initialize(s,cfg,in,w); in.target={20,0,0};
        for(int i=0;i<60;++i) Step(s,cfg,in,w);
        auto limited=cfg; limited.max_queries=8;
        in.dt=.1; in.rotation=ToQuat({0,80,30});
        const auto r=Step(s,limited,in,w);
        EXPECT(r.valid && r.palm_shape && r.budget_limited && r.rotation_blocked && r.queries==8);
        EXPECT(PalmGap(r,in.palm,w.planes[0])>=-1e-6);
    }
    // Fail each observation around a wall turn: a failed clearance/seating
    // query cannot commit the tentative transform or fall back to the sphere.
    for(int failed=1;failed<=12;++failed) {
        World w{{{{10,0,0},{-1,0,0}}}};
        hm::State s; hm::Input in; in.palm=Palm(); in.palm.flatten=false;
        Initialize(s,cfg,in,w); in.target={20,0,0};
        for(int i=0;i<60;++i) Step(s,cfg,in,w);
        w.queries=0; w.fail_query=failed;
        in.dt=.1; in.rotation=ToQuat({0,80,30});
        const auto r=Step(s,cfg,in,w);
        EXPECT(r.valid && r.palm_shape && r.queries<=cfg.max_queries);
        EXPECT(PalmGap(r,in.palm,w.planes[0])>=-1e-6);
    }
    {
        World w; hm::State s; hm::Input in; in.palm=Palm();
        in.palm.press_cm=std::numeric_limits<double>::quiet_NaN();
        in.dt=1.0/90;
        // A configured box without the box backend is unavailable, not a
        // smaller or wrongly located sphere that might report clear space.
        const auto missing=hm::Step(s,cfg,in,[&](const FVector& a,const FVector& b,double r){ return w(a,b,r); });
        EXPECT(!missing.valid && missing.query_failed && missing.palm_shape);
        EXPECT(Step(s,cfg,in,w).valid);
        w.planes.push_back({{10,0,0},{-1,0,0}}); in.target={20,0,0};
        hm::Result r;
        for(int i=0;i<90;++i) r=Step(s,cfg,in,w);
        EXPECT(std::isfinite(r.flatness) && r.flatness>.99);
    }
    // Calibrated grip-local palm geometry and its checked seating transform
    // together with the world; a world origin/yaw change cannot alter its gap.
    {
        const FQuat turn=ToQuat({15,80,-20}); const FVector origin={120,-40,30};
        World a{{{{10,0,0},{-1,0,0}}}}, b;
        b.planes.push_back({origin+turn.Rotate({10,0,0}),turn.Rotate({-1,0,0})});
        hm::State sa,sb; hm::Input ia,ib; ia.palm=ib.palm=Palm();
        ia.dt=ib.dt=1.0/90; ib.anchor=ib.target=origin; ib.rotation=turn;
        Step(sa,cfg,ia,a); Step(sb,cfg,ib,b);
        ia.target={20,15,0}; ia.rotation=ToQuat({0,30,0});
        ib.target=origin+turn.Rotate(ia.target); ib.rotation=Normalized(turn*ia.rotation);
        hm::Result ra,rb;
        for(int i=0;i<120;++i) { ra=Step(sa,cfg,ia,a); rb=Step(sb,cfg,ib,b); }
        EXPECT(Length(rb.position-(origin+turn.Rotate(ra.position)))<1e-5);
        EXPECT(std::fabs(ra.flatness-rb.flatness)<1e-6);
        EXPECT(std::fabs(PalmGap(ra,ia.palm,a.planes[0])-PalmGap(rb,ib.palm,b.planes[0]))<1e-5);
    }
    // The observed 1.104 cm plateau is an immovable thumb base, not missing
    // finger curl: root palm station2.1335 - boxface1.5 - skin.03 + radius.5.
    // All points translate together, preserving the requested rigid shape.
    for(double mirror:{-1.0,1.0}) {
        const FVector normal{0,0,1};
        const FVector root{mirror*3,2,-0.6035};
        rigid::Plane plane[]{{{},normal,{0,0,1.5},1.5},{{},normal,root,0.5}};
        const auto r=rigid::Resolve(plane,2);
        EXPECT(r.valid && r.feasible && r.planes==1);
        EXPECT(std::fabs(r.translation.Z-1.1335)<1e-6 && r.residual_cm<1e-6);
        const FVector tip=root+FVector{0,5,0};
        EXPECT(Length((tip+r.translation)-(root+r.translation)-(tip-root))<1e-9);
        EXPECT(Dot(root+r.translation,normal)-0.5>=0.02999);
    }
    // Palm, CMC and deep finger contacts push the whole hand back; a corner
    // contributes independent normals without forcing arbitrary joint curls.
    {
        rigid::Plane planes[]{{{},{0,0,1},{0,0,1.5},1.5},
                              {{},{0,0,1},{0,0,-.6035},.5},
                              {{},{0,0,1},{0,0,-2},.5},
                              {{},{1,0,0},{-1,0,0},.5}};
        const auto r=rigid::Resolve(planes,4);
        EXPECT(r.valid && r.feasible && r.planes==2);
        EXPECT(std::fabs(r.translation.X-1.53)<1e-6 && std::fabs(r.translation.Z-2.53)<1e-6);
        for(const auto& p:planes) EXPECT(Dot(p.sample+r.translation-p.point,p.normal)-p.radius>=.02999);
        rigid::Settings limited; limited.maximum_translation_cm=1;
        const auto short_step=rigid::Resolve(planes,4,limited);
        EXPECT(short_step.valid && !short_step.feasible && Length(short_step.translation)<=1.000001);
        rigid::Plane impossible[]{{{2,0,0},{1,0,0},{},0},{{1,0,0},{-1,0,0},{},0}};
        EXPECT(!rigid::Resolve(impossible,2).feasible);
    }
    // Reach-aware projection can move sideways as needed to retain both
    // world clearance and valid arm length; incompatible constraints reject.
    {
        rigid::Settings settings; settings.skin_cm=0;
        rigid::Plane plane{{0,4,0},{0,1,0},{},0};
        const auto reachable=rigid::Resolve(&plane,1,settings,{{4,0,0},{},5});
        EXPECT(reachable.valid && reachable.feasible && reachable.residual_cm<.015);
        EXPECT(Length(FVector{4,0,0}+reachable.translation)<=5.00001);
        EXPECT(reachable.translation.Y>3.985 && reachable.translation.X<-.98);
        plane.point={0,6,0};
        EXPECT(!rigid::Resolve(&plane,1,settings,{{},{},5}).feasible);
        EXPECT(!rigid::Resolve(nullptr,0,settings,{{1e308,0,0},{-1e308,0,0},5}).valid);
    }
    // Post-IK/backpressure correction checks the full palm from the acquired
    // proxy, immediately arrests blocked motion, and keeps raw history intact.
    {
        World w{{{{10,0,0},{-1,0,0}}}};
        hm::State s; hm::Input in; in.palm=Palm(); Initialize(s,cfg,in,w);
        in.target={9,0,0}; for(int i=0;i<60;++i) Step(s,cfg,in,w);
        const FVector raw=s.last_target, anchor=s.last_anchor;
        auto guard=[&](const FVector& target,const FQuat& rotation,const hm::Settings& settings) {
            return hm::Constrain(s,settings,in.palm,target,rotation,
                [&](const FVector&a,const FVector&b,double radius){return w(a,b,radius);},
                [&](const FVector&a,const FVector&b,const FVector&half,const FQuat&q){return w.Box(a,b,half,q);});
        };
        const auto arrested=guard(s.position+FVector{2.304,0,0},s.rotation,cfg);
        EXPECT(arrested.valid && arrested.touching && PalmGap(arrested,in.palm,w.planes[0])>=0.02999);
        EXPECT(Length(s.last_target-raw)<1e-9 && Length(s.last_anchor-anchor)<1e-9);
        const auto retracted=guard({5,0,0},s.rotation,cfg);
        EXPECT(retracted.valid && std::fabs(s.position.X-5)<1e-7);
        const auto saved=s.position; w.available=false;
        EXPECT(!guard({7,0,0},ToQuat({0,35,0}),cfg).valid && Length(s.position-saved)<1e-9);
        w.available=true; auto none=cfg; none.max_queries=0;
        const auto no_budget=guard({7,0,0},s.rotation,none);
        EXPECT(!no_budget.valid && no_budget.budget_limited && no_budget.queries==0 && Length(s.position-saved)<1e-9);
    }
    // At a low35Hz game tick, world cost stays bounded during a turn; after
    // settling, tiny tracking jitter needs only the fresh contact queries.
    {
        World w{{{{10,0,0},{-1,0,0}}}};
        hm::State s; hm::Input in; in.palm=Palm(); Initialize(s,cfg,in,w);
        in.dt=1.0/35; in.target={20,0,0}; in.rotation=ToQuat({0,35,0});
        hm::Result r;
        for(int i=0;i<70;++i) {r=Step(s,cfg,in,w); EXPECT(r.queries<=24 && PalmGap(r,in.palm,w.planes[0])>=-1e-6);}
        for(int i=0;i<70;++i) {
            in.rotation=ToQuat({0,35+(i%2?.05:-.05),0}); r=Step(s,cfg,in,w);
            EXPECT(r.queries<=3 && r.flatness>.99);
        }
    }
    // Fresh rigid-root memory prevents spring/retraction/cache invalidation
    // on every frame: the thumbbase itself becomes a bounded contact support.
    // Expiry or a clear re-observation releases it; it cannot hide query loss.
    {
        World w{{{{10,0,0},{-1,0,0}}}};
        hm::State s; hm::Input in; in.palm=Palm(); Initialize(s,cfg,in,w);
        hm::RootSupport root{{10,0,0},{-1,0,0},{.6335,0,0},.5,.05};
        in.supports=&root; in.support_count=1; in.dt=1.0/35; in.target={20,0,0};
        hm::Result r;
        for(int i=0;i<35;++i) {
            // A stationary surface is re-observed at least every50ms.
            root.remaining_seconds=(i%2==0)?.05:.05-1.0/35;
            r=Step(s,cfg,in,w);
            EXPECT(r.valid && r.queries<=3 && r.position.X<=8.83651);
        }
        EXPECT(std::fabs(r.position.X-8.8365)<1e-5 && r.touching);
        const auto old=s.position;
        const auto blocked=hm::Constrain(s,cfg,in.palm,{9.5,0,0},s.rotation,
            [&](const FVector&a,const FVector&b,double radius){return w(a,b,radius);},
            [&](const FVector&a,const FVector&b,const FVector&half,const FQuat&q){return w.Box(a,b,half,q);},&root,1);
        EXPECT(blocked.valid && std::fabs(s.position.X-old.X)<1e-6);
        w.available=false; r=Step(s,cfg,in,w);
        EXPECT(r.query_failed && !r.touching && Length(s.position-old)<1e-6);
        w.available=true; w.planes.clear(); root.remaining_seconds=0;
        r=Step(s,cfg,in,w);
        EXPECT(r.valid && !r.touching && r.position.X>old.X && r.flatness==0);
    }
    std::printf("hand motion: %d failure(s); positions at 72/90/120 Hz %.3f / %.3f / %.3f cm\n",failures,ends[0].X,ends[1].X,ends[2].X);
    return failures?1:0;
}
