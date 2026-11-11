#include "vr/body/finger_self.h"
#include <algorithm>
#include <cmath>

namespace holevr::vr::finger_self {
namespace {
using namespace ik;
namespace fc=finger_contact;
namespace jl=joint_limits;
struct Envelope {
    FVector half_axis[3]{};
    FVector dual_axis[3]{};
    FVector offset{};
    double enclosing_radius=0;
    double start_fraction=0;
    bool fitted=false;
};
struct Geometry {
    FVector p[5][4];Envelope envelope[5][3];
    FQuat rotation{},inverse_rotation{};FVector inverse_scale{1,1,1};bool anisotropic=false;
};
FVector Unscaled(const Geometry& g,const FVector& v) {
    const auto local=g.inverse_rotation.Rotate(v);
    return {local.X*g.inverse_scale.X,local.Y*g.inverse_scale.Y,local.Z*g.inverse_scale.Z};
}
FVector NormalToWorld(const Geometry& g,const FVector& v) {
    return g.rotation.Rotate({v.X*g.inverse_scale.X,v.Y*g.inverse_scale.Y,v.Z*g.inverse_scale.Z});
}
void SetEnvelope(Geometry& g,int f,const fingers::Chain& c,const skel::Xf joints[3],const skel::Xf& hand) {
    g.rotation=hand.rotation;g.inverse_rotation=hand.rotation.Inverse();
    g.inverse_scale={std::fabs(hand.scale.X)>1e-9?1/hand.scale.X:0,std::fabs(hand.scale.Y)>1e-9?1/hand.scale.Y:0,
                     std::fabs(hand.scale.Z)>1e-9?1/hand.scale.Z:0};
    g.anisotropic=std::fabs(std::fabs(hand.scale.X)-std::fabs(hand.scale.Y))+
                  std::fabs(std::fabs(hand.scale.X)-std::fabs(hand.scale.Z))>1e-8;
    for(int n=0;n<3;++n) {
        const auto transformed=hand_collision_shape::Transform(c.collision[n],joints[n],hand,0);
        auto& shape=g.envelope[f][n];shape.fitted=transformed.fitted;
        shape.half_axis[0]=transformed.side_half;shape.half_axis[1]=transformed.depth_half;
        shape.half_axis[2]=transformed.axial_half;
        shape.offset=transformed.self_offset;
        shape.enclosing_radius=std::sqrt(Dot(shape.half_axis[0],shape.half_axis[0])+
            Dot(shape.half_axis[1],shape.half_axis[1])+Dot(shape.half_axis[2],shape.half_axis[2]));
        if(g.anisotropic)for(int axis=0;axis<3;++axis)shape.dual_axis[axis]=NormalToWorld(g,Unscaled(g,shape.half_axis[axis]));
        shape.start_fraction=std::clamp(transformed.self_start_fraction,0.0,1.0);
    }
}
Geometry Make(const fingers::Chain c[5],const fingers::Angles a[5],const skel::Xf& hand) {
    Geometry g;
    for(int f=0;f<5;++f)if(c[f].valid) {
        skel::Xf j[3];FVector tip;fingers::Fk(c[f],a[f],j,&tip);
        for(int n=0;n<3;++n)g.p[f][n]=skel::Apply(hand,j[n].translation);
        g.p[f][3]=skel::Apply(hand,tip);
        SetEnvelope(g,f,c[f],j,hand);
    }
    return g;
}
struct Pair { int a=0,b=0,sa=0,sb=0;double fa=0,fb=0,depth=0;FVector pa{},pb{},normal{}; };
Pair ClosestPoints(const FVector& x,const FVector& end_x,const FVector& y,const FVector& end_y) {
    Pair p;
    const FVector u=end_x-x,v=end_y-y,w=x-y;
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
    p.normal=Normalize(difference,Normalize(x-y,{0,0,1}));return p;
}
double Support(const Envelope& shape,const FVector& normal,double radius) {
    if(!shape.fitted)return radius;
    double squared=0;
    for(const auto& axis:shape.half_axis){const double v=Dot(axis,normal);squared+=v*v;}
    return std::sqrt(squared);
}
// A positive result is a separating-plane certificate for the complete
// segment plus oriented ellipsoid, not a reduced isotropic finger radius.
// Negative values are conservative unresolved overlap, not exact mesh depth.
double Separation(const Geometry& g,int a,int sa,int b,int sb,double radius) {
    const auto& A=g.envelope[a][sa];const auto& B=g.envelope[b][sb];
    // A whole segment can belong to the connected palm/thumb web. Excluding
    // it from self tests must not leave an artificial colliding endpoint cap.
    if(A.start_fraction>=1 || B.start_fraction>=1)return 1e9;
    const FVector a0=g.p[a][sa]+(g.p[a][sa+1]-g.p[a][sa])*A.start_fraction+A.offset,a1=g.p[a][sa+1]+A.offset;
    const FVector b0=g.p[b][sb]+(g.p[b][sb+1]-g.p[b][sb])*B.start_fraction+B.offset,b1=g.p[b][sb+1]+B.offset;
    const auto closest=ClosestPoints(a0,a1,b0,b1);
    const double centre_distance=Length(closest.pa-closest.pb);
    if(!A.fitted && !B.fitted)return centre_distance-2*radius;
    const double spherical=centre_distance-(A.fitted?A.enclosing_radius:radius)-(B.fitted?B.enclosing_radius:radius);
    if(spherical>.1)return spherical;
    auto along=[&](const FVector& direction) {
        const double length=Length(direction);if(length<1e-9)return -1e9;
        const FVector n=direction*(1.0/length);
        const double amin=std::min(Dot(a0,n),Dot(a1,n)),amax=std::max(Dot(a0,n),Dot(a1,n));
        const double bmin=std::min(Dot(b0,n),Dot(b1,n)),bmax=std::max(Dot(b0,n),Dot(b1,n));
        return std::max(amin-bmax,bmin-amax)-Support(A,n,radius)-Support(B,n,radius);
    };
    double clearance=along(closest.normal);
    if(clearance>0)return clearance;
    for(const auto& axis:A.half_axis)clearance=std::max(clearance,along(axis));
    for(const auto& axis:B.half_axis)clearance=std::max(clearance,along(axis));
    clearance=std::max(clearance,along(Cross(a1-a0,b1-b0)));
    if(clearance<=0 && g.anisotropic) {
        // Common affine hand scale cannot turn disjoint shapes into an
        // intersection. Transport the source-space separator as a normal,
        // using inverse transpose rather than the surface-vector transform.
        const auto local=ClosestPoints(Unscaled(g,a0-b0),Unscaled(g,a1-b0),{},Unscaled(g,b1-b0));
        clearance=std::max(clearance,along(NormalToWorld(g,local.normal)));
        for(const auto& axis:A.dual_axis)clearance=std::max(clearance,along(axis));
        for(const auto& axis:B.dual_axis)clearance=std::max(clearance,along(axis));
    }
    return clearance;
}
struct Error {double worst=0,squared=0;int pairs=0;};
Error Measure(const fingers::Chain c[5],const Geometry& g,double radius) {
    Error e;
    for(int a=0;a<5;++a)if(c[a].valid)for(int b=a+1;b<5;++b)if(c[b].valid)
        for(int sa=0;sa<3;++sa)for(int sb=0;sb<3;++sb) {
            const double d=std::max(0.0,-Separation(g,a,sa,b,sb,radius));
            e.worst=std::max(e.worst,d);e.squared+=d*d;e.pairs+=d>0.001;
        }
    return e;
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
bool LocalMetric(const fingers::Chain c[5],const skel::Xf& hand) {
    if(std::min({std::fabs(hand.scale.X),std::fabs(hand.scale.Y),std::fabs(hand.scale.Z)})<1e-6)return false;
    bool any=false;
    for(int f=0;f<5;++f)if(c[f].valid)for(const auto& shape:c[f].collision){any=true;if(!shape.fitted)return false;}
    return any;
}
} // namespace
double Depth(const fingers::Chain c[5],const fingers::Angles a[5],const skel::Xf& hand,double radius,int* pairs) {
    const bool local=LocalMetric(c,hand);
    const auto e=Measure(c,Make(c,a,local?skel::Xf{}:hand),std::clamp(radius,0.0,3.0));if(pairs)*pairs=e.pairs;
    return e.worst*(local?std::max({std::fabs(hand.scale.X),std::fabs(hand.scale.Y),std::fabs(hand.scale.Z)}):1.0);
}

namespace {
double MaxScale(const FVector& v){return std::max({std::fabs(v.X),std::fabs(v.Y),std::fabs(v.Z)});}
bool SamePose(const fingers::Angles& a,const fingers::Angles& b) {
    if(std::fabs(a.spread-b.spread)>1e-10)return false;
    for(int j=0;j<3;++j)if(std::fabs(a.flex[j]-b.flex[j])+std::fabs(a.extra[j].X-b.extra[j].X)+
        std::fabs(a.extra[j].Y-b.extra[j].Y)+std::fabs(a.extra[j].Z-b.extra[j].Z)+std::fabs(a.extra[j].W-b.extra[j].W)>1e-10)return false;
    return true;
}
double TravelBound(const fingers::Chain& c,const fingers::Angles& a,const fingers::Angles& z,
                   const fc::Bounds& b,const skel::Xf& hand) {
    if(SamePose(a,z))return 0;
    double scale=MaxScale(hand.scale),length[3],surface[3]{};
    for(int j=0;j<3;++j) {
        scale*=MaxScale(c.ref[j].scale);length[j]=scale*Length(j<2?c.ref[j+1].translation:c.tip);
        const auto& s=c.collision[j];
        if(s.fitted) surface[j]=scale*(Length(s.self_offset)+std::sqrt(Dot(s.side_axis,s.side_axis)*s.side_half_cm*s.side_half_cm+
            Dot(s.depth_axis,s.depth_axis)*s.depth_half_cm*s.depth_half_cm+
            Dot(s.axial_axis,s.axial_axis)*s.axial_half_cm*s.axial_half_cm));
    }
    double bound=0,remaining=0,extent=0;
    for(int j=2;j>=0;--j) {
        remaining+=length[j];extent=std::max(extent,surface[j]);
        const auto from=jl::Decompose(b.axes[j],c.ref[j].rotation.Inverse()*fingers::LocalRotation(c,j,a));
        const auto to=jl::Decompose(b.axes[j],c.ref[j].rotation.Inverse()*fingers::LocalRotation(c,j,z));
        bound+=(remaining+extent)*(std::fabs(to.flex-from.flex)+std::fabs(to.side-from.side)+std::fabs(to.twist-from.twist));
    }
    // Conservative scale/turn bound, including flex-dependent side clamping.
    return 2*bound;
}
void MakeDigit(Geometry& g,int f,const fingers::Chain& c,const fingers::Angles& pose,const skel::Xf& hand) {
    skel::Xf j[3];FVector tip;fingers::Fk(c,pose,j,&tip);
    for(int n=0;n<3;++n)g.p[f][n]=skel::Apply(hand,j[n].translation);
    g.p[f][3]=skel::Apply(hand,tip);
    SetEnvelope(g,f,c,j,hand);
}
double Clearance(const Geometry& g,int a,int b,double radius) {
    double clearance=1e9;
    for(int x=0;x<3;++x)for(int y=0;y<3;++y) {
        clearance=std::min(clearance,Separation(g,a,x,b,y,radius));
    }
    return clearance;
}
std::array<double,3> SegmentTravel(const fingers::Chain& c,const fingers::Angles& before,
                                   const fingers::Angles& after,const fc::Bounds& b,const skel::Xf& hand) {
    // A distal joint cannot move an earlier phalanx. Keeping these bounds
    // separate avoids charging a close proximal pair for the fingertip's
    // longer path, which otherwise falsely arrests opening on the fitted rig.
    std::array<double,3> bound{};
    if(SamePose(before,after))return bound;
    double angular[3]{},length[3]{},scale=MaxScale(hand.scale);
    for(int s=0;s<3;++s) {
        const auto start=jl::Decompose(b.axes[s],c.ref[s].rotation.Inverse()*fingers::LocalRotation(c,s,before));
        const auto end=jl::Decompose(b.axes[s],c.ref[s].rotation.Inverse()*fingers::LocalRotation(c,s,after));
        angular[s]=std::fabs(end.flex-start.flex)+std::fabs(end.side-start.side)+std::fabs(end.twist-start.twist);
        scale*=MaxScale(c.ref[s].scale);
        length[s]=scale*Length(s<2?c.ref[s+1].translation:c.tip);
        const auto& shape=c.collision[s];
        double reach=scale*(Length(shape.self_offset)+std::sqrt(shape.side_half_cm*shape.side_half_cm+
            shape.depth_half_cm*shape.depth_half_cm+shape.axial_half_cm*shape.axial_half_cm));
        for(int j=s;j>=0;--j){reach+=length[j];bound[s]+=2*reach*angular[j];}
    }
    return bound;
}
double FittedFraction(const fingers::Chain c[5],const fingers::Angles before[5],const fingers::Angles after[5],
                      const fc::Bounds b[5],const skel::Xf& hand,int a,int z,double radius,int* shared_budget) {
    const auto A=SegmentTravel(c[a],before[a],after[a],b[a],hand),B=SegmentTravel(c[z],before[z],after[z],b[z],hand);
    std::array<double,9> travel{},skin{};bool moving=false;
    for(int x=0;x<3;++x)for(int y=0;y<3;++y){travel[x*3+y]=A[x]+B[y];moving|=travel[x*3+y]>1e-9;}
    if(!moving)return 1;
    if(shared_budget && *shared_budget<2)return 0;
    int evaluations=0;
    auto sample=[&](double t) {
        ++evaluations;if(shared_budget)--*shared_budget;
        Geometry g;std::array<double,9> gaps{};
        for(int f:{a,z})MakeDigit(g,f,c[f],t==0?before[f]:t==1?after[f]:Blend(c[f],before[f],after[f],b[f],t),hand);
        for(int x=0;x<3;++x)for(int y=0;y<3;++y)if(travel[x*3+y]>1e-9)gaps[x*3+y]=Separation(g,a,x,z,y,radius);
        return gaps;
    };
    const auto first=sample(0),last=sample(1);
    // Keep a small available surface gap through repeated pressure frames.
    // A narrower initial gap may open freely but cannot shrink cumulatively.
    for(int i=0;i<9;++i)skin[i]=std::min(.03,std::max(0.0,first[i]));
    double safe=0;
    auto visit=[&](auto&& recurse,double lo,double hi,const auto& left,const auto& right,int depth)->bool {
        bool clear=true;
        for(int i=0;i<9;++i)if(travel[i]>1e-9 && (left[i]<-1e-9 || right[i]+1e-9<skin[i] ||
            left[i]+right[i]<=travel[i]*(hi-lo)+1e-9)){clear=false;break;}
        if(clear){safe=hi;return true;}
        if(depth>=12 || evaluations>=64 || (shared_budget && *shared_budget<=0))return false;
        const double middle=(lo+hi)*.5;const auto mid=sample(middle);
        return recurse(recurse,lo,middle,left,mid,depth+1) && recurse(recurse,middle,hi,mid,right,depth+1);
    };
    return visit(visit,0.0,1.0,first,last,0)?1.0:safe;
}
// The distance between two capsules cannot decrease faster than the sum of
// their endpoint travel bounds. Advance only through a certified clear part
// of the input path; do not trust just clear endpoints of crossing fingers.
double SafeFraction(const fingers::Chain c[5],const fingers::Angles before[5],
                    const fingers::Angles after[5],const fc::Bounds b[5],const skel::Xf& hand,
                    int a,int z,double radius,int* shared_budget=nullptr,double travel_override=-1) {
    if(c[a].collision[0].fitted && c[z].collision[0].fitted)
        return FittedFraction(c,before,after,b,hand,a,z,radius,shared_budget);
    const double travel=travel_override>=0?travel_override:
        TravelBound(c[a],before[a],after[a],b[a],hand)+TravelBound(c[z],before[z],after[z],b[z],hand);
    if(travel<1e-9)return 1;
    if(shared_budget && *shared_budget<2)return 0;
    auto clearance_at=[&](double t) {
        if(shared_budget)--*shared_budget;
        Geometry g;
        MakeDigit(g,a,c[a],Blend(c[a],before[a],after[a],b[a],t),hand);
        MakeDigit(g,z,c[z],Blend(c[z],before[z],after[z],b[z],t),hand);
        return Clearance(g,a,z,radius);
    };
    const double initial=clearance_at(0),last=clearance_at(1);
    double safe=0;int evaluations=2;
    // A previously valid near-touching pose must remain releasable; requiring
    // a larger new gap would freeze it before it could move out of contact.
    const double skin=std::min(.005,.5*std::max(0.0,initial));
    auto visit=[&](auto&& self,double lo,double hi,double left,double right,int depth)->bool {
        // Endpoint distance lower bounds overlap throughout this interval.
        if(left>=0 && right>=skin && left+right>travel*(hi-lo)+1e-9) {
            safe=hi;return true;
        }
        if(depth>=12 || evaluations>=64 || (shared_budget && *shared_budget<=0))return false;
        const double middle=(lo+hi)*.5,value=clearance_at(middle);++evaluations;
        if(!self(self,lo,middle,left,value,depth+1))return false;
        return self(self,middle,hi,value,right,depth+1);
    };
    return visit(visit,0.0,1.0,initial,last,0)?1.0:safe;
}
jl::Turn Turn(const fingers::Chain& c,const fingers::Angles& pose,const fc::Bounds& bounds,int joint) {
    if(pose.flex[joint]==0 && (joint!=0 || pose.spread==0))return jl::Decompose(bounds.axes[joint],pose.extra[joint]);
    return jl::Decompose(bounds.axes[joint],c.ref[joint].rotation.Inverse()*fingers::LocalRotation(c,joint,pose));
}
double IntentionError(const fingers::Chain c[5],const fingers::Angles pose[5],const fc::State state[5],const fc::Bounds b[5]) {
    double score=0;
    for(int f=0;f<5;++f)if(c[f].valid)for(int j=0;j<3;++j) {
        const auto a=Turn(c[f],pose[f],b[f],j),goal=Turn(c[f],state[f].frame_intent,b[f],j);
        const double flex=a.flex-goal.flex,side=a.side-goal.side,twist=a.twist-goal.twist;
        score+=flex*flex+.35*side*side+.15*twist*twist;
    }
    return score;
}
fingers::Angles YieldPose(const fingers::Chain& c,const fingers::Angles& requested,const fc::State& state,
                         const fc::Bounds& b,double side,double twist,int flex_joint=-1,double flex_delta=0) {
    fingers::Angles pose;
    for(int j=0;j<3;++j) {
        const auto start=Turn(c,state.frame_start,b,j),goal=Turn(c,state.frame_intent,b,j);
        const auto anchor=Turn(c,state.self_yield_active?state.self_anchor:state.frame_start,b,j);
        auto q=Turn(c,requested,b,j);
        if(j==flex_joint)q.flex=std::clamp(q.flex+flex_delta,std::min(goal.flex,anchor.flex)-12*kDegToRad,
                                          std::max(goal.flex,anchor.flex)+12*kDegToRad);
        if(j==0) {
            // Absolute bounds around controller intention prevent compliance
            // from accumulating each frame into an unrelated finger pose.
            q.side=std::clamp(q.side+side,std::min(goal.side,anchor.side)-12*kDegToRad,std::max(goal.side,anchor.side)+12*kDegToRad);
            q.twist=std::clamp(q.twist+twist,std::min(goal.twist,anchor.twist)-8*kDegToRad,std::max(goal.twist,anchor.twist)+8*kDegToRad);
        }
        q=jl::ClampTurn(b.limits[j],q);
        const double df=q.flex-start.flex,ds=q.side-start.side,dr=q.twist-start.twist;
        const double total=std::fabs(df)+std::fabs(ds)+std::fabs(dr);
        if(state.frame_has_prior && total>state.frame_max_step && total>1e-12) {
            const double t=state.frame_max_step/total;
            q={start.flex+df*t,start.side+ds*t,start.twist+dr*t};
        }
        pose.extra[j]=jl::Compose(b.axes[j],jl::ClampTurn(b.limits[j],q));
    }
    return pose;
}
bool Yield(const fingers::Chain c[5],const fingers::Angles before[5],const fingers::Angles requested[5],
           fingers::Angles accepted[5],fc::State state[5],const fc::Bounds b[5],const skel::Xf& hand,
           double radius,const bool blocked[5]) {
    const double score=IntentionError(c,accepted,state,b);
    jl::Turn goal[5][3];double joint_cost[5][3]{},base_travel[5]{};
    auto cost=[](const jl::Turn& a,const jl::Turn& z) {
        const double f=a.flex-z.flex,s=a.side-z.side,r=a.twist-z.twist;return f*f+.35*s*s+.15*r*r;
    };
    for(int f=0;f<5;++f)if(c[f].valid) {
        for(int j=0;j<3;++j) {
            goal[f][j]=Turn(c[f],state[f].frame_intent,b[f],j);
            joint_cost[f][j]=cost(Turn(c[f],accepted[f],b[f],j),goal[f][j]);
        }
        base_travel[f]=TravelBound(c[f],before[f],accepted[f],b[f],hand);
    }
    const Geometry base_geometry=Make(c,accepted,hand);
    int trials=0,path_budget=128,proposal=0,next_untried=-1;
    const unsigned cursor=state[0].self_search_cursor;
    const int first=0;
    auto contact_load=[&](const Geometry& g) {
        double load=0;
        for(int a=0;a<5;++a)if(c[a].valid)for(int z=a+1;z<5;++z)if(c[z].valid)
            load+=std::exp(-std::max(0.0,Clearance(g,a,z,radius))/.25);
        return load;
    };
    const double initial_load=contact_load(base_geometry);
    auto attempt=[&](const fingers::Angles candidate[5],bool escape=false) {
        // Fixed CPU ceiling; resume untried proposals on successive frames
        // instead of repeatedly searching the same prefix of candidates.
        const int index=proposal++;
        if(index<static_cast<int>(cursor))return false;
        if(++trials>24 || path_budget<=0) {
            if(next_untried<0)next_untried=index;
            return false;
        }
        double candidate_score=score,travel[5]{};bool changed[5]{};Geometry geometry=base_geometry;
        for(int f=0;f<5;++f)if(c[f].valid) {
            changed[f]=!SamePose(candidate[f],accepted[f]);travel[f]=base_travel[f];
            if(!changed[f])continue;
            for(int j=0;j<3;++j)candidate_score+=cost(Turn(c[f],candidate[f],b[f],j),goal[f][j])-joint_cost[f][j];
            MakeDigit(geometry,f,c[f],candidate[f],hand);
            travel[f]=TravelBound(c[f],before[f],candidate[f],b[f],hand);
        }
        if(candidate_score>=score-1e-7 &&
           (!escape || candidate_score>score+.15 || contact_load(geometry)>initial_load-.005))return false;
        for(int a=0;a<5;++a)if(c[a].valid)for(int z=a+1;z<5;++z)if(c[z].valid && (changed[a]||changed[z]))
            if(Clearance(geometry,a,z,radius)<-1e-8)return false;
        for(int a=0;a<5;++a)if(c[a].valid)for(int z=a+1;z<5;++z)if(c[z].valid)
            if((changed[a]||changed[z]) && SafeFraction(c,before,candidate,b,hand,a,z,radius,&path_budget,travel[a]+travel[z])<1-1e-9)return false;
        for(int f=0;f<5;++f)accepted[f]=candidate[f];state[0].self_search_cursor=0;return true;
    };
    fingers::Angles candidate[5];
    // Often only coordination is needed: let one digit open before its
    // neighbor, instead of freezing both on their simultaneous input path.
    for(int order=0;order<5;++order) { const int f=(first+order)%5;if(!c[f].valid || !blocked[f])continue;
        for(int k=0;k<5;++k)candidate[k]=accepted[k];candidate[f]=requested[f];
        if(attempt(candidate))return true;
        // Unhook the tip/middle before moving the knuckle when a simultaneous
        // opening sweep would cross the neighboring digit.
        for(int j:{2,1,0}) {
            for(int k=0;k<5;++k)candidate[k]=accepted[k];
            auto local=Turn(c[f],requested[f],b[f],j);
            candidate[f].flex[j]=0;
            if(j==0)candidate[f].spread=0;
            candidate[f].extra[j]=jl::Compose(b[f].axes[j],local);
            if(attempt(candidate))return true;
        }
    }
    // Small anatomical lateral/axial compliance supplies a release detour.
    // World debt is handled by the hand root, never by violating self contact.
    for(int order=0;order<5;++order) { const int f=(first+order)%5;if(!c[f].valid || !blocked[f])continue;
      for(double direction:{-1.0,1.0}) {
        for(int k=0;k<5;++k)candidate[k]=accepted[k];
        candidate[f]=YieldPose(c[f],requested[f],state[f],b[f],direction*3*kDegToRad,0);
        if(attempt(candidate,true))return true;
        if(c[f].thumb) {
            candidate[f]=YieldPose(c[f],requested[f],state[f],b[f],0,direction*3*kDegToRad);
            if(attempt(candidate,true))return true;
        }
        for(int j:{0,1}) {
            candidate[f]=YieldPose(c[f],requested[f],state[f],b[f],0,0,j,direction*3*kDegToRad);
            if(attempt(candidate,true))return true;
        }
        // A neighboring digit can yield sideways while the hooked digit
        // opens. Both changes still share their original frame budgets.
        for(int other=0;other<5;++other)if(other!=f && c[other].valid && blocked[other]) {
            for(int k=0;k<5;++k)candidate[k]=accepted[k];
            candidate[f]=YieldPose(c[f],requested[f],state[f],b[f],direction*3*kDegToRad,0);
            candidate[other]=requested[other];
            if(attempt(candidate,true))return true;
        }
        // First create a little clearance while holding the hooked flexion;
        // mixing the full opening step into this detour can still collide.
        for(int k=0;k<5;++k)candidate[k]=accepted[k];
        candidate[f]=YieldPose(c[f],accepted[f],state[f],b[f],direction*3*kDegToRad,0);
        if(attempt(candidate,true))return true;
        if(c[f].thumb) {
            candidate[f]=YieldPose(c[f],accepted[f],state[f],b[f],0,direction*3*kDegToRad);
            if(attempt(candidate,true))return true;
        }
        for(int j:{0,1,2}) {
            candidate[f]=YieldPose(c[f],accepted[f],state[f],b[f],0,0,j,direction*3*kDegToRad);
            if(attempt(candidate,true))return true;
        }
    }
    }
    state[0].self_search_cursor=next_untried>=0?static_cast<unsigned>(next_untried):0;
    return false;
}
}
Result Resolve(const fingers::Chain c[5],fingers::Angles poses[5],fc::State states[5],const skel::Xf& world_hand,
               const fc::Bounds bounds[5],const fc::Contacts world[5],double radius,double dt) {
    (void)dt;Result out;radius=std::isfinite(radius)?std::clamp(radius,0.0,3.0):0;
    // A common invertible wrist transform preserves all self intersections.
    // Certify fitted surfaces in one hand-local metric so nonuniform avatar
    // scale cannot change the accepted angular path or create a false lock.
    const bool local=LocalMetric(c,world_hand);
    const skel::Xf hand=local?skel::Xf{}:world_hand;
    const double reported_scale=local?MaxScale(world_hand.scale):1.0;
    fingers::Angles before[5],reference[5],requested[5];bool blocked[5]{};
    for(int f=0;f<5;++f) {
        before[f]=states[f].frame_start;
        reference[f]=Blend(c[f],{}, {},bounds[f],0);
        requested[f]=poses[f];
    }
    auto seed=Measure(c,Make(c,before,hand),radius);
    if(seed.worst>1e-6) {
        const auto ref=Measure(c,Make(c,reference,hand),radius);
        if(ref.worst<=1e-6) {
            for(int f=0;f<5;++f)before[f]=reference[f];
            out.restricted=true;
        } else {
            // A collision radius larger than immutable root spacing is not
            // solvable by bending. Refuse to certify an overlapping pose.
            out.feasible=false;out.residual_cm=seed.worst*reported_scale;out.active_pairs=seed.pairs;
            return out;
        }
    }
    // Local self-separation has priority. Any world debt introduced by an
    // arrested digit is handed to the rigid hand-root response afterwards.
    for(int pass=0;pass<3;++pass) {
        bool changed=false;out.passes=pass+1;
        for(int a=0;a<5;++a)if(c[a].valid)for(int z=a+1;z<5;++z)if(c[z].valid) {
            const double t=SafeFraction(c,before,poses,bounds,hand,a,z,radius);
            if(t>=1-1e-9)continue;
            blocked[a]=blocked[z]=true;
            poses[a]=Blend(c[a],before[a],poses[a],bounds[a],t);
            poses[z]=Blend(c[z],before[z],poses[z],bounds[z],t);
            changed=out.restricted=true;
        }
        if(!changed)break;
    }
    // Reverting one interacting digit may alter its relation to another.
    // Close that dependency set, retaining unaffected digits' input motion.
    bool held[5]{};
    for(int pass=0;pass<5;++pass) {
        bool changed=false;
        for(int a=0;a<5;++a)if(c[a].valid)for(int z=a+1;z<5;++z)if(c[z].valid) {
            if(SafeFraction(c,before,poses,bounds,hand,a,z,radius)>=1-1e-9)continue;
            if(!held[a]||!held[z]) {
                blocked[a]=blocked[z]=true;
                poses[a]=before[a];poses[z]=before[z];held[a]=held[z]=true;
                changed=out.restricted=true;
            }
        }
        if(!changed)break;
    }
    for(int f=0;f<5;++f)if(c[f].valid && blocked[f]) {
        bool new_goal=false;
        for(int j=0;j<3;++j) {
            const auto goal=Turn(c[f],states[f].frame_intent,bounds[f],j);
            const auto old=Turn(c[f],states[f].self_intent,bounds[f],j);
            new_goal|=std::fabs(goal.flex-old.flex)+std::fabs(goal.side-old.side)+std::fabs(goal.twist-old.twist)>20*kDegToRad;
        }
        if(!states[f].self_yield_active || new_goal) {
            states[f].self_anchor=before[f];states[f].self_intent=states[f].frame_intent;states[f].self_yield_active=true;
        }
    }
    bool opening=false;
    for(int f=0;f<5;++f)if(c[f].valid && blocked[f])for(int j=0;j<3;++j) {
        const auto current=Turn(c[f],before[f],bounds[f],j),goal=Turn(c[f],states[f].frame_intent,bounds[f],j);
        opening|=goal.flex<current.flex-2*kDegToRad || std::fabs(goal.side)<std::fabs(current.side)-2*kDegToRad ||
                 std::fabs(goal.twist)<std::fabs(current.twist)-2*kDegToRad;
    }
    // Opening is serviced immediately. A stationary closing intention gets
    // occasional soft adjustment rather than paying for a search every draw.
    if(out.restricted && (opening || states[0].self_search_tick++%4==0))
        out.yielded=Yield(c,before,requested,poses,states,bounds,hand,radius,blocked);
    const auto final=Measure(c,Make(c,poses,hand),radius);
    out.residual_cm=final.worst*reported_scale;out.active_pairs=final.pairs;out.feasible=final.worst<=1e-6;
    for(int f=0;f<5;++f)if(c[f].valid) {
        double remaining=0;
        for(int j=0;j<3;++j) {
            const auto pose=Turn(c[f],poses[f],bounds[f],j),goal=Turn(c[f],states[f].frame_intent,bounds[f],j);
            remaining+=std::fabs(pose.flex-goal.flex)+std::fabs(pose.side-goal.side)+std::fabs(pose.twist-goal.twist);
        }
        if(remaining<1*kDegToRad)states[f].self_yield_active=false;
        Synchronize(c[f],poses[f],states[f],bounds[f]);
        out.world_residual_cm=std::max(out.world_residual_cm,fc::Depth(c[f],poses[f],world_hand,world[f]));
    }
    return out;
}
} // namespace holevr::vr::finger_self
