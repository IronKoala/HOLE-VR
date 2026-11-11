#include "vr/body/finger_contact.h"

#include <algorithm>
#include <cmath>

namespace holevr::vr::finger_contact {
namespace {
using namespace ik;
namespace jl = joint_limits;
constexpr double kContactAge = 0.075;
constexpr double kClear = 0.001; // cm, numerical convergence tolerance

bool Finite(const FVector& v) { return std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z); }
bool Finite(const FQuat& q) {
    return std::isfinite(q.X) && std::isfinite(q.Y) && std::isfinite(q.Z) && std::isfinite(q.W);
}
double Safe(double v) { return std::isfinite(v) ? v : 0.0; }
bool Usable(const Contact& c) {
    return c.valid && c.segment >= 0 && c.segment < 3 && Finite(c.point) && Finite(c.normal) &&
           Length(c.normal) > 1e-6 && std::isfinite(c.fraction) && c.fraction >= 0 && c.fraction <= 1 &&
           std::isfinite(c.radius_cm) && c.radius_cm >= 0 && c.radius_cm <= 8 &&
           std::isfinite(c.patch_radius_cm) && c.patch_radius_cm > 0 &&
           std::isfinite(c.age_seconds) && c.age_seconds >= 0 && c.age_seconds <= kContactAge;
}
struct Params { jl::Turn joint[3]; };

Params Clamp(Params p, const Bounds& b, bool* limited = nullptr) {
    for (int j = 0; j < 3; ++j) {
        const auto& l = b.limits[j];
        const jl::Turn before = p.joint[j];
        // Generated angular motion is continuous; do not wrap a large input
        // around to the opposite anatomical stop.
        p.joint[j].flex = std::clamp(Safe(before.flex), l.flex_min, l.flex_max);
        const double side = l.SideAt(p.joint[j].flex);
        p.joint[j].side = std::clamp(Safe(before.side), -side, side);
        p.joint[j].twist = std::clamp(Safe(before.twist), -l.twist, l.twist);
        if (limited && (std::fabs(before.flex - p.joint[j].flex) > 1e-6 ||
                        std::fabs(before.side - p.joint[j].side) > 1e-6 ||
                        std::fabs(before.twist - p.joint[j].twist) > 1e-6)) *limited = true;
    }
    return p;
}
Params FromPose(const fingers::Chain& c, const fingers::Angles& a, const Bounds& b, bool* limited = nullptr) {
    Params p;
    for (int j = 0; j < 3; ++j) {
        const FQuat q = c.ref[j].rotation.Inverse() * fingers::LocalRotation(c, j, a);
        p.joint[j] = Finite(q) ? jl::Decompose(b.axes[j], q) : jl::Turn{};
    }
    return Clamp(p, b, limited);
}
fingers::Angles Pose(const Params& p, const Bounds& b) {
    fingers::Angles a;
    for (int j = 0; j < 3; ++j) a.extra[j] = jl::Compose(b.axes[j], p.joint[j]);
    return a;
}
struct Samples { FVector points[4]; skel::Xf joints[3]; };
Samples Points(const fingers::Chain& c, const fingers::Angles& a, const skel::Xf& hand) {
    skel::Xf j[3];
    FVector tip;
    fingers::Fk(c, a, j, &tip);
    Samples out;
    for (int k = 0; k < 3; ++k) {
        out.points[k] = skel::Apply(hand, j[k].translation);
        out.joints[k] = j[k];
    }
    out.points[3] = skel::Apply(hand, tip);
    return out;
}
FVector Sample(const Samples& s, const Contact& c) {
    return s.points[c.segment] + (s.points[c.segment + 1] - s.points[c.segment]) * c.fraction;
}
struct Support { bool active = false; double fraction = 0; FVector point{}; double depth = 0; };
Support LocalSupport(const Samples& s, const Contact& c) {
    Support hit;
    if (!c.valid) return hit; // validated once before the bounded hot loop
    if(c.point_sample_only) {
        const FVector p=Sample(s,c);
        return {true,c.fraction,p,std::max(0.0,c.radius_cm-Dot(p-c.point,c.normal))};
    }
    const FVector n = c.normal, a = s.points[c.segment], delta = s.points[c.segment+1]-a;
    const FVector offset=a-c.point;
    const double dn=Dot(delta,n),on=Dot(offset,n);
    const double aa=std::max(0.0,Dot(delta,delta)-dn*dn),bb=Dot(offset,delta)-on*dn;
    const double cc=Dot(offset,offset)-on*on-c.patch_radius_cm*c.patch_radius_cm;
    double lo=0,hi=1;
    if (aa<1e-12) { if(cc>0) return hit; }
    else if(cc>0 || aa+2*bb+cc>0) {
        const double discriminant=bb*bb-aa*cc;
        if(discriminant<0) return hit;
        const double root=std::sqrt(discriminant);
        lo=std::max(0.0,(-bb-root)/aa); hi=std::min(1.0,(-bb+root)/aa);
        if(lo>hi) return hit;
    }
    hit.active=true;
    hit.fraction=dn>0 ? lo : hi;
    hit.point=a+delta*hit.fraction;
    hit.depth=std::max(0.0,c.radius_cm-on-dn*hit.fraction);
    return hit;
}
Support ConstraintSupport(const Samples& s,const Contact& c,bool finite_patch) {
    Support hit=LocalSupport(s,c);
    if(!finite_patch && c.valid) {
        const FVector point=Sample(s,c);
        const double depth=std::max(0.0,c.radius_cm-Dot(point-c.point,c.normal));
        // Retain the active material sample even if a candidate rotates it
        // beyond the patch boundary. Also cover any newly deeper portion.
        if(!hit.active || depth>hit.depth) hit={true,c.fraction,point,depth};
    }
    return hit;
}
double ContactDepth(const Samples& s, const Contact& c, bool* active = nullptr, bool finite_patch = true) {
    const auto hit=ConstraintSupport(s,c,finite_patch);
    if(active) *active=hit.active;
    return hit.depth;
}
struct Error { double squared = 0, worst = 0; int active = 0; };
Error Measure(const fingers::Chain& c, const Params& p, const Bounds& b, const skel::Xf& hand,
              const Contacts& contacts) {
    const Samples samples = Points(c, Pose(p, b), hand);
    Error e;
    for (int i = 0; i < std::clamp(contacts.count, 0, kMaxContacts); ++i) {
        bool active = false;
        const double d = ContactDepth(samples, contacts.items[i], &active, false);
        e.active += active;
        e.worst = std::max(e.worst, d);
        e.squared += d * d;
    }
    return e;
}
} // namespace

Bounds MakeBounds(const fingers::Chain& c, int finger) {
    Bounds b;
    double ref[3];
    joint_limits::ReferenceBends(c, c.thumb, ref);
    for (int j = 0; j < 3; ++j) {
        b.axes[j] = joint_limits::JointAxes(c, j);
        b.limits[j] = joint_limits::Relative(joint_limits::Human(finger, j), ref[j]);
    }
    return b;
}
void Reset(State& state) { state = State{}; }

fingers::Angles CurlTarget(const fingers::Chain& c,double amount,const Bounds& b) {
    const auto curl=fingers::Curl(c,std::clamp(Safe(amount),0.0,1.2),fingers::DefaultLimits(c.thumb));
    Params p;
    for(int j=0;j<3;++j)p.joint[j].flex=curl.flex[j]-b.limits[j].ref_bend;
    return Pose(Clamp(p,b),b);
}

FVector SampleWorld(const fingers::Chain& c, const fingers::Angles& a, const skel::Xf& hand,
                    int segment, double fraction) {
    Contact sample;
    sample.segment = std::clamp(segment, 0, 2);
    sample.fraction = std::clamp(Safe(fraction), 0.0, 1.0);
    return Sample(Points(c, a, hand), sample);
}
double Depth(const fingers::Chain& c, const fingers::Angles& a, const skel::Xf& hand,
             const Contacts& contacts, int* active) {
    const Samples samples = Points(c, a, hand);
    double worst = 0;
    int count = 0;
    for (int i = 0; i < std::clamp(contacts.count, 0, kMaxContacts); ++i) {
        Contact contact=contacts.items[i];
        if(!Usable(contact)) continue;
        contact.normal=Normalize(contact.normal);
        bool used = false;
        worst = std::max(worst, ContactDepth(samples, contact, &used));
        count += used;
    }
    if (active) *active = count;
    return worst;
}


Witness EvaluateContact(const fingers::Chain& c,const fingers::Angles& a,
                        const skel::Xf& hand,const Contact& input) {
    Witness out;Contact contact=input;if(!Usable(contact))return out;
    contact.normal=Normalize(contact.normal);
    const auto support=LocalSupport(Points(c,a,hand),contact);
    if(!support.active)return out;
    out.active=true;out.center=support.point;out.surface=contact.point;out.normal=contact.normal;
    out.radius_cm=contact.radius_cm;out.depth_cm=support.depth;out.segment=contact.segment;out.fraction=support.fraction;
    return out;
}

fingers::Angles Prepare(const fingers::Chain& c,const fingers::Angles& desired,State& state,
                        const Bounds& b,double dt,bool reset,bool already_prepared) {
    if(reset)Reset(state);
    const Params target=FromPose(c,desired,b);
    // A reference pose is the first local safety seed, not an already-closing
    // desired fist. The hand-wide gate verifies that seed before rendering.
    const Params previous=FromPose(c,state.initialized?state.pose:fingers::Angles{},b);
    Params p=previous;
    const double seconds=std::clamp(Safe(dt),0.0,0.05);
    state.frame_start=Pose(previous,b);state.frame_intent=Pose(target,b);state.frame_seconds=seconds;
    state.frame_has_prior=state.initialized;state.frame_max_step=240*kDegToRad*seconds;
    const double alpha=state.initialized && !already_prepared?1-std::exp(-seconds/.035):1;
    for(int j=0;j<3;++j) {
        double* value[3]={&p.joint[j].flex,&p.joint[j].side,&p.joint[j].twist};
        const double goal[3]={target.joint[j].flex,target.joint[j].side,target.joint[j].twist};
        const double before[3]={previous.joint[j].flex,previous.joint[j].side,previous.joint[j].twist};
        for(int axis=0;axis<3;++axis) {
            double delta=(goal[axis]-*value[axis])*alpha;
            if(std::fabs(goal[axis]-*value[axis])<1e-5)delta=goal[axis]-*value[axis];
            if(state.initialized) {
                if(seconds<=0)delta=0;
                else {
                    const double speed=240*kDegToRad,acceleration=7200*kDegToRad;
                    const double velocity=std::clamp(std::clamp(delta/seconds,-speed,speed),
                        state.angular_velocity[j*3+axis]-acceleration*seconds,
                        state.angular_velocity[j*3+axis]+acceleration*seconds);
                    delta=std::clamp(velocity*seconds,std::min(0.0,goal[axis]-*value[axis]),std::max(0.0,goal[axis]-*value[axis]));
                }
            }
            *value[axis]+=delta;
        }
        const double total=std::fabs(*value[0]-before[0])+std::fabs(*value[1]-before[1])+std::fabs(*value[2]-before[2]);
        if(state.initialized && total>state.frame_max_step && total>1e-12)
            for(int axis=0;axis<3;++axis)*value[axis]=before[axis]+(*value[axis]-before[axis])*state.frame_max_step/total;
    }
    return Pose(Clamp(p,b),b);
}
fingers::Angles Preview(const fingers::Chain& c,const fingers::Angles& desired,const State& state,
                        const Bounds& b,double dt) {
    State copy=state;return Prepare(c,desired,copy,b,dt);
}

Result ResolvePrepared(const fingers::Chain& c,const fingers::Angles& candidate,State& state,
                       const skel::Xf& hand,const Bounds& b,const Contacts& contacts) {
    Result out;
    const Params target=FromPose(c,candidate,b,&out.limited);
    const Params previous=FromPose(c,state.frame_start,b);
    Contacts effective=contacts;
    const Samples before=Points(c,Pose(previous,b),hand),after=Points(c,Pose(target,b),hand);
    for(int i=0;i<std::clamp(effective.count,0,kMaxContacts);++i) {
        auto& contact=effective.items[i];contact.valid=Usable(contact);if(!contact.valid)continue;
        contact.normal=Normalize(contact.normal);
        const auto a=LocalSupport(before,contact),z=LocalSupport(after,contact);
        const auto deepest=z.active&&(!a.active||z.depth>a.depth)?z:a;
        contact.valid=deepest.active;if(deepest.active)contact.fraction=deepest.fraction;
    }
    Params p=target;
    const Error requested=Measure(c,target,b,hand,effective),prior=Measure(c,previous,b,hand,effective);
    // Desired movement is admitted only along the input trajectory. A moving
    // obstacle or buried root creates hand-level debt instead of progressively
    // folding all three joints into an artificial escape pose.
    if(requested.worst>kClear) {
        out.blocked=true;p=previous;
        Error accepted=prior;
        auto allow=[&](Params trial) {
            trial=Clamp(trial,b);
            const auto error=Measure(c,trial,b,hand,effective);
            const bool clear=error.worst<=kClear && accepted.worst<=kClear;
            const bool improves=error.worst<accepted.worst-1e-8 ||
                (error.worst<=accepted.worst+1e-8 && error.squared<=accepted.squared+1e-10);
            if(clear||improves){p=trial;accepted=error;return true;}return false;
        };
        if(!allow(target)) {
            // Arrest only the obstructed components. Each trial moves toward
            // the prepared intention, never sideways or beyond it.
            for(int j:{2,1,0})for(int axis=0;axis<3;++axis) {
                const Params start=p;
                auto component=[](Params& v,int joint,int a)->double& {
                    return a==0?v.joint[joint].flex:a==1?v.joint[joint].side:v.joint[joint].twist;
                };
                Params goal=target;
                const double from=component(p,j,axis),to=component(goal,j,axis);
                if(std::fabs(to-from)<1e-8)continue;
                Params trial=p;component(trial,j,axis)=to;
                if(allow(trial))continue;
                double lo=0,hi=1;
                for(int step=0;step<5;++step) {
                    out.iterations=std::max(out.iterations,step+1);
                    const double t=(lo+hi)*.5;trial=start;component(trial,j,axis)=from+(to-from)*t;
                    if(allow(trial))lo=t;else hi=t;
                }
            }
        }
    }
    p=Clamp(p,b,&out.limited);out.pose=Pose(p,b);
    const auto final=Measure(c,p,b,hand,effective);
    out.residual_cm=final.worst;out.active_contacts=final.active;out.root_required=final.worst>kClear;
    if(state.frame_seconds>0)for(int j=0;j<3;++j) {
        state.angular_velocity[j*3]=(p.joint[j].flex-previous.joint[j].flex)/state.frame_seconds;
        state.angular_velocity[j*3+1]=(p.joint[j].side-previous.joint[j].side)/state.frame_seconds;
        state.angular_velocity[j*3+2]=(p.joint[j].twist-previous.joint[j].twist)/state.frame_seconds;
    }
    state.pose=out.pose;state.initialized=true;return out;
}
Result Solve(const fingers::Chain& c,const fingers::Angles& desired,State& state,
             const skel::Xf& hand,const Bounds& b,const Contacts& contacts,double dt,bool reset) {
    const auto candidate=Prepare(c,desired,state,b,dt,reset);
    return ResolvePrepared(c,candidate,state,hand,b,contacts);
}
Result ResolveContacts(const fingers::Chain& c,State& state,const skel::Xf& hand,
                       const Bounds& b,const Contacts& contacts) {
    return ResolvePrepared(c,state.pose,state,hand,b,contacts);
}

} // namespace holevr::vr::finger_contact
