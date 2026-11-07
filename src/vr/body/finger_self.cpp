#include "vr/body/finger_self.h"
#include <algorithm>
#include <cmath>

namespace holevr::vr::finger_self {
namespace {
using namespace ik;
namespace fc=finger_contact;
namespace jl=joint_limits;
struct Geometry { FVector p[5][4]; };
Geometry Make(const fingers::Chain c[5],const fingers::Angles a[5],const skel::Xf& hand) {
    Geometry g;
    for(int f=0;f<5;++f)if(c[f].valid) {
        skel::Xf j[3];FVector tip;fingers::Fk(c[f],a[f],j,&tip);
        for(int n=0;n<3;++n)g.p[f][n]=skel::Apply(hand,j[n].translation);
        g.p[f][3]=skel::Apply(hand,tip);
    }
    return g;
}
struct Pair { int a=0,b=0,sa=0,sb=0;double fa=0,fb=0,depth=0;FVector pa{},pb{},normal{}; };
Pair Closest(const Geometry& g,int a,int sa,int b,int sb,double radius) {
    Pair p;p.a=a;p.b=b;p.sa=sa;p.sb=sb;
    const FVector x=g.p[a][sa],y=g.p[b][sb],u=g.p[a][sa+1]-x,v=g.p[b][sb+1]-y,w=x-y;
    const double uu=Dot(u,u),vv=Dot(v,v),uv=Dot(u,v),uw=Dot(u,w),vw=Dot(v,w);
    if(uu<=1e-12) {p.fa=0;p.fb=vv>1e-12?std::clamp(vw/vv,0.0,1.0):0;}
    else if(vv<=1e-12) {p.fb=0;p.fa=std::clamp(-uw/uu,0.0,1.0);}
    else {
        const double den=uu*vv-uv*uv;
        p.fa=den>1e-12?std::clamp((uv*vw-vv*uw)/den,0.0,1.0):0;
        p.fb=(uv*p.fa+vw)/vv;
        if(p.fb<0){p.fb=0;p.fa=std::clamp(-uw/uu,0.0,1.0);}
        else if(p.fb>1){p.fb=1;p.fa=std::clamp((uv-uw)/uu,0.0,1.0);}
    }
    p.pa=x+u*p.fa;p.pb=y+v*p.fb;
    const FVector difference=p.pa-p.pb;
    p.normal=Normalize(difference,Normalize(g.p[a][0]-g.p[b][0],{0,0,1}));
    p.depth=std::max(0.0,2*radius-Length(difference));return p;
}
struct Error {double worst=0,squared=0;int pairs=0;};
Error Measure(const fingers::Chain c[5],const Geometry& g,double radius) {
    Error e;
    for(int a=0;a<5;++a)if(c[a].valid)for(int b=a+1;b<5;++b)if(c[b].valid)
        for(int sa=0;sa<3;++sa)for(int sb=0;sb<3;++sb) {
            const double d=Closest(g,a,sa,b,sb,radius).depth;
            e.worst=std::max(e.worst,d);e.squared+=d*d;e.pairs+=d>0.001;
        }
    return e;
}
void Participants(const fingers::Chain c[5],const Geometry& g,double radius,bool involved[5]) {
    for(int a=0;a<5;++a)if(c[a].valid)for(int b=a+1;b<5;++b)if(c[b].valid)
        for(int sa=0;sa<3;++sa)for(int sb=0;sb<3;++sb)
            if(Closest(g,a,sa,b,sb,radius).depth>0.001)involved[a]=involved[b]=true;
}
bool Improves(const Error& a,const Error& b) {
    return a.worst<b.worst-1e-7 || (a.worst<=b.worst+1e-7 && a.squared<b.squared-1e-9);
}
fingers::Angles Blend(const fingers::Chain& c,const fingers::Angles& a,const fingers::Angles& b,
                      const fc::Bounds& bounds,double t) {
    fingers::Angles out;
    for(int j=0;j<3;++j) {
        const auto from=jl::Decompose(bounds.axes[j],c.ref[j].rotation.Inverse()*fingers::LocalRotation(c,j,a));
        const auto to=jl::Decompose(bounds.axes[j],c.ref[j].rotation.Inverse()*fingers::LocalRotation(c,j,b));
        jl::Turn q{from.flex+(to.flex-from.flex)*t,from.side+(to.side-from.side)*t,from.twist+(to.twist-from.twist)*t};
        out.extra[j]=jl::Compose(bounds.axes[j],jl::ClampTurn(bounds.limits[j],q));
    }
    return out;
}
void Synchronize(const fingers::Chain& c,const fingers::Angles& pose,fc::State& state,const fc::Bounds& bounds) {
    state.pose=pose;
    if(state.frame_seconds<=0)return;
    for(int j=0;j<3;++j) {
        const auto before=jl::Decompose(bounds.axes[j],c.ref[j].rotation.Inverse()*fingers::LocalRotation(c,j,state.frame_start));
        const auto after=jl::Decompose(bounds.axes[j],c.ref[j].rotation.Inverse()*fingers::LocalRotation(c,j,pose));
        state.angular_velocity[j*3]=(after.flex-before.flex)/state.frame_seconds;
        state.angular_velocity[j*3+1]=(after.side-before.side)/state.frame_seconds;
        state.angular_velocity[j*3+2]=(after.twist-before.twist)/state.frame_seconds;
    }
}
} // namespace
double Depth(const fingers::Chain c[5],const fingers::Angles a[5],const skel::Xf& hand,double radius,int* pairs) {
    const auto e=Measure(c,Make(c,a,hand),std::clamp(radius,0.0,3.0));if(pairs)*pairs=e.pairs;return e.worst;
}
Result Resolve(const fingers::Chain c[5],fingers::Angles poses[5],fc::State states[5],const skel::Xf& hand,
               const fc::Bounds bounds[5],const fc::Contacts world[5],double radius,double dt) {
    (void)dt;Result out;radius=std::isfinite(radius)?std::clamp(radius,0.0,3.0):0;
    auto geometry=Make(c,poses,hand);Error best=Measure(c,geometry,radius);
    double world_depth[5]{};
    for(int f=0;f<5;++f)if(c[f].valid)world_depth[f]=fc::Depth(c[f],poses[f],hand,world[f]);
    // A verified prior full-hand pose is preferable to an interpenetrating
    // candidate. Test it at THIS wrist transform and current world evidence.
    fingers::Angles previous[5];bool prior_available=true,prior_world_ok=true;
    for(int f=0;f<5;++f) {
        previous[f]=states[f].frame_start;
        if(c[f].valid) {
            prior_available&=states[f].frame_has_prior;
            prior_world_ok&=fc::Depth(c[f],previous[f],hand,world[f])<=world_depth[f]+1e-6;
        }
    }
    if(prior_available && prior_world_ok) {
        const auto prior_error=Measure(c,Make(c,previous,hand),radius);
        if(Improves(prior_error,best)) {
            bool involved[5]{};Participants(c,geometry,radius,involved);
            fingers::Angles candidate[5];
            for(int f=0;f<5;++f)candidate[f]=involved[f]?previous[f]:poses[f];
            const auto held=Make(c,candidate,hand);const auto error=Measure(c,held,radius);
            if(Improves(error,best)) {
                for(int f=0;f<5;++f)poses[f]=candidate[f];
                geometry=held;best=error;out.restricted=true;
            }
        }
        // Check intermediate capsule poses as well: two fingertips must not
        // swap sides in a single step while ending mutually clear.
        if(prior_error.worst<=0.001 && best.worst<=0.001) {
            fingers::Angles candidate[5],last_clear[5];
            for(int f=0;f<5;++f)last_clear[f]=previous[f];
            for(int step=1;step<=8;++step) {
                bool clear=true;
                for(int f=0;f<5;++f)if(c[f].valid) {
                    candidate[f]=Blend(c[f],previous[f],poses[f],bounds[f],step/8.0);
                    clear&=fc::Depth(c[f],candidate[f],hand,world[f])<=world_depth[f]+1e-6;
                }
                const auto midway=Make(c,candidate,hand);
                if(Measure(c,midway,radius).worst>0.001)clear=false;
                if(!clear) {
                    bool involved[5]{};Participants(c,midway,radius,involved);
                    for(int f=0;f<5;++f)if(c[f].valid && fc::Depth(c[f],candidate[f],hand,world[f])>world_depth[f]+1e-6)involved[f]=true;
                    for(int f=0;f<5;++f)candidate[f]=involved[f]?last_clear[f]:poses[f];
                    const auto held=Make(c,candidate,hand);const auto error=Measure(c,held,radius);
                    if(error.worst<=best.worst+1e-6) {
                        for(int f=0;f<5;++f)poses[f]=candidate[f];
                        geometry=held;best=error;out.restricted=true;
                    }
                    break;
                }
                for(int f=0;f<5;++f)last_clear[f]=candidate[f];
            }
        }
    }
    for(int pass=0;pass<3 && best.worst>0.001;++pass) {
        out.passes=pass+1;bool moved=false;
        for(int f=0;f<5;++f)if(c[f].valid) {
            // Keep all observed world constraints. Remaining slots contain
            // the deepest actual capsule pair for each neighboring digit.
            fc::Contacts combined=world[f];combined.count=std::clamp(combined.count,0,fc::kMaxContacts);
            Pair nearest[4];int count=0;
            for(int other=0;other<5;++other)if(other!=f && c[other].valid) {
                Pair deepest;
                for(int a=0;a<3;++a)for(int b=0;b<3;++b) {
                    const auto pair=Closest(geometry,f,a,other,b,radius);
                    if(pair.depth>deepest.depth)deepest=pair;
                }
                if(deepest.depth>0.001)nearest[count++]=deepest;
            }
            std::sort(nearest,nearest+count,[](const Pair& a,const Pair& b){return a.depth>b.depth;});
            for(int i=0;i<count && combined.count<fc::kMaxContacts;++i) {
                const auto& pair=nearest[i];fc::Contact contact;contact.valid=true;contact.source=fc::Contact::Source::self;
                contact.segment=pair.sa;contact.fraction=pair.fa;contact.point=pair.pb+pair.normal*radius;
                contact.normal=pair.normal;contact.radius_cm=radius;contact.patch_radius_cm=2*radius+0.1;
                contact.point_sample_only=true;combined.items[combined.count++]=contact;
            }
            if(combined.count==world[f].count)continue;
            fc::State trial=states[f];trial.pose=poses[f];
            const auto result=fc::ResolveContacts(c[f],trial,hand,bounds[f],combined);
            const double new_world=fc::Depth(c[f],result.pose,hand,world[f]);
            if(new_world>world_depth[f]+1e-6)continue;
            const auto original=poses[f];poses[f]=result.pose;
            const auto candidate=Make(c,poses,hand);const auto error=Measure(c,candidate,radius);
            if(Improves(error,best)) {
                states[f]=trial;geometry=candidate;best=error;world_depth[f]=new_world;moved=true;
            } else poses[f]=original;
        }
        if(!moved)break;
    }
    for(int f=0;f<5;++f)if(c[f].valid) {
        Synchronize(c[f],poses[f],states[f],bounds[f]);
        out.world_residual_cm=std::max(out.world_residual_cm,fc::Depth(c[f],poses[f],hand,world[f]));
    }
    out.residual_cm=best.worst;out.active_pairs=best.pairs;return out;
}
} // namespace holevr::vr::finger_self
