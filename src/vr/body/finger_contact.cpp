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
           std::isfinite(c.radius_cm) && c.radius_cm >= 0 && c.radius_cm <= 3 &&
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
double& Variable(Params& p, int k) {
    if (k < 3) return p.joint[k].flex;
    return k == 3 ? p.joint[0].side : p.joint[0].twist;
}
double Variable(const Params& p, int k) {
    if (k < 3) return p.joint[k].flex;
    return k == 3 ? p.joint[0].side : p.joint[0].twist;
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
double Preference(const Params& p, const Params& desired, const Params& previous) {
    double cost = 0;
    // Distal response is deliberately less expensive than moving the whole
    // chain. This is finite compliance, not a compulsory DIP:PIP relation.
    const double weights[5] = {4, 2, 0.35, 2, 2};
    for (int k = 0; k < 5; ++k) {
        const double d = Variable(p, k) - Variable(desired, k);
        const double prior = Variable(p, k) - Variable(previous, k);
        cost += weights[k] * d * d + 0.15 * prior * prior;
    }
    const double coupling = (p.joint[2].flex - 0.7 * p.joint[1].flex) -
                            (desired.joint[2].flex - 0.7 * desired.joint[1].flex);
    return cost + 0.03 * coupling * coupling;
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

static Result SolveImpl(const fingers::Chain& c, const fingers::Angles& desired, State& state,
                        const skel::Xf& hand, const Bounds& b, const Contacts& contacts,
                        double dt, bool reset, bool continuation) {
    Result out;
    if (reset) Reset(state);
    const Params target = FromPose(c, desired, b, &out.limited);
    const Params previous = state.initialized ? FromPose(c, state.pose, b) : target;
    Params p = previous;
    const double seconds = continuation ? state.frame_seconds : std::clamp(Safe(dt), 0.0, 0.05);
    if(!continuation) {
        state.frame_start=Pose(previous,b);
        state.frame_seconds=seconds;
        state.frame_has_prior=state.initialized;
        state.frame_max_step=360*kDegToRad*seconds;
        const double alpha=state.initialized ? 1-std::exp(-seconds/0.040) : 1;
        for(int j=0;j<3;++j) {
            double* values[3]={&p.joint[j].flex,&p.joint[j].side,&p.joint[j].twist};
            const double targets[3]={target.joint[j].flex,target.joint[j].side,target.joint[j].twist};
            for(int axis=0;axis<3;++axis) {
                double delta=(targets[axis]-*values[axis])*alpha;
                if(std::fabs(targets[axis]-*values[axis])<1e-5)delta=targets[axis]-*values[axis];
                if(state.initialized && seconds>0) {
                    const double speed=240*kDegToRad,acceleration=7200*kDegToRad;
                    const double prior_speed=state.angular_velocity[j*3+axis];
                    const double wanted_speed=std::clamp(delta/seconds,-speed,speed);
                    const double velocity=std::clamp(wanted_speed,prior_speed-acceleration*seconds,prior_speed+acceleration*seconds);
                    // Input motion cannot coast past the target on a reversal.
                    delta=std::clamp(velocity*seconds,std::min(0.0,targets[axis]-*values[axis]),std::max(0.0,targets[axis]-*values[axis]));
                }
                *values[axis]+=delta;
            }
        }
    }
    const Params frame_start=FromPose(c,state.frame_start,b);
    auto bounded=[&](Params value) {
        if(state.frame_has_prior) {
            for(int j=0;j<3;++j) {
                value.joint[j].flex=std::clamp(value.joint[j].flex,frame_start.joint[j].flex-state.frame_max_step,frame_start.joint[j].flex+state.frame_max_step);
                value.joint[j].side=std::clamp(value.joint[j].side,frame_start.joint[j].side-state.frame_max_step,frame_start.joint[j].side+state.frame_max_step);
                value.joint[j].twist=std::clamp(value.joint[j].twist,frame_start.joint[j].twist-state.frame_max_step,frame_start.joint[j].twist+state.frame_max_step);
                // Compose is a product of three rotations. Limiting the sum
                // of their absolute changes conservatively limits the true
                // quaternion angle too, unlike three independent box bounds.
                const double df=value.joint[j].flex-frame_start.joint[j].flex;
                const double ds=value.joint[j].side-frame_start.joint[j].side;
                const double dr=value.joint[j].twist-frame_start.joint[j].twist;
                const double total=std::fabs(df)+std::fabs(ds)+std::fabs(dr);
                if(total>state.frame_max_step && total>1e-12) {
                    const double scale=state.frame_max_step/total;
                    value.joint[j].flex=frame_start.joint[j].flex+df*scale;
                    value.joint[j].side=frame_start.joint[j].side+ds*scale;
                    value.joint[j].twist=frame_start.joint[j].twist+dr*scale;
                }
            }
        }
        return Clamp(value,b,&out.limited);
    };
    p = bounded(p);
    // Decide local patch validity once for this solve. A numerical candidate
    // must not "solve" a penetration by leaving the patch sideways while
    // staying behind its surface. The world layer refreshes patch geometry
    // on later frames; this freeze is not a cached infinite world plane.
    Contacts effective = contacts;
    const Samples initial_samples = Points(c, Pose(p, b), hand);
    const Samples target_samples = Points(c, Pose(target, b), hand);
    const Samples previous_samples = Points(c, Pose(previous, b), hand);
    for (int i = 0; i < std::clamp(effective.count, 0, kMaxContacts); ++i) {
        auto& contact=effective.items[i];
        contact.valid=Usable(contact);
        if(!contact.valid) continue;
        contact.normal=Normalize(contact.normal);
        const auto initial=LocalSupport(initial_samples,effective.items[i]);
        const auto requested=LocalSupport(target_samples,effective.items[i]);
        const auto preceding=LocalSupport(previous_samples,effective.items[i]);
        Support deepest=initial;
        for(const auto& hit : {requested,preceding})
            if(hit.active && (!deepest.active || hit.depth>deepest.depth)) deepest=hit;
        effective.items[i].valid=deepest.active;
        if(deepest.active) effective.items[i].fraction=deepest.fraction;
    }
    Error best = Measure(c, p, b, hand, effective);
    double preference = Preference(p, target, previous);
    auto consider = [&](Params candidate) {
        candidate = bounded(candidate);
        const Error e = Measure(c, candidate, b, hand, effective);
        const double pref = Preference(candidate, target, previous);
        // Contact feasibility wins; preference only resolves near-equal
        // solutions. No final animation blend follows this comparison.
        if (e.worst < best.worst - 1e-8 ||
            (std::fabs(e.worst - best.worst) <= 1e-8 &&
             (e.squared < best.squared - 1e-10 ||
              (std::fabs(e.squared - best.squared) <= 1e-10 && pref < preference)))) {
            p = candidate; best = e; preference = pref; return true;
        }
        return false;
    };
    constexpr int kIterations = 6;
    const double mobility[5] = {0.02, 0.08, 1.0, 0.02, 0.02};
    bool analytic=true;
    for(const auto& joint:c.ref)
        if(std::fabs(joint.scale.X-joint.scale.Y)>1e-6 || std::fabs(joint.scale.X-joint.scale.Z)>1e-6)
            analytic=false;
    for (int iteration = 0; iteration < kIterations && best.worst > kClear; ++iteration) {
        out.iterations = iteration + 1;
        bool moved = false;
        for (int i = 0; i < std::clamp(effective.count, 0, kMaxContacts); ++i) {
            const Contact& contact = effective.items[i];
            const Samples samples = Points(c, Pose(p, b), hand);
            const auto support=ConstraintSupport(samples,contact,false);
            const double depth = support.depth;
            if (depth <= kClear) continue;
            const FVector normal = contact.normal;
            const int segment=contact.segment;
            const FVector tip_local=segment<2 ? samples.joints[segment+1].translation
                                              : skel::Apply(samples.joints[2],c.tip);
            const FVector point_local=samples.joints[segment].translation+
                        (tip_local-samples.joints[segment].translation)*support.fraction;
            double jacobian[5]{}, denominator = 1e-8;
            auto available_derivative=[&](int k,double derivative) {
                if(std::fabs(derivative)<1e-10)return 0.0;
                const double direction=derivative>0?1.0:-1.0;
                Params probe=p;Variable(probe,k)+=direction*1e-4;probe=bounded(probe);
                // A joint already at its anatomical or per-frame stop has
                // no mobility in that direction. Including it in the
                // denominator starves joints that can still clear contact.
                return direction*(Variable(probe,k)-Variable(p,k))>1e-7?derivative:0.0;
            };
            for (int k = 0; k < 5; ++k) {
                if(k<3 && k>segment) continue;
                if(!analytic) {
                    Params probe=p;
                    Variable(probe,k)+=0.003; probe=Clamp(probe,b);
                    double delta=Variable(probe,k)-Variable(p,k);
                    if(std::fabs(delta)<1e-7) {
                        probe=p; Variable(probe,k)-=0.003; probe=Clamp(probe,b);
                        delta=Variable(probe,k)-Variable(p,k);
                    }
                    if(std::fabs(delta)>1e-7) {
                        Contact sample=contact; sample.fraction=support.fraction;
                        jacobian[k]=available_derivative(k,Dot(Sample(Points(c,Pose(probe,b),hand),sample)-support.point,normal)/delta);
                        denominator+=mobility[k]*jacobian[k]*jacobian[k];
                    }
                    continue;
                }
                const int joint=k<3 ? k : 0;
                const FQuat parent=joint==0 ? FQuat{} : samples.joints[joint-1].rotation;
                const FQuat reference=parent*c.ref[joint].rotation;
                FVector axis;
                if(k<3) axis=reference.Rotate(b.axes[joint].flex);
                else if(k==3) axis=(reference*AxisAngle(b.axes[0].flex,p.joint[0].flex)).Rotate(b.axes[0].toward)*-1.0;
                else axis=samples.joints[0].rotation.Rotate(b.axes[0].bone);
                const FVector velocity=Cross(axis,point_local-samples.joints[joint].translation);
                const FVector world=hand.rotation.Rotate({velocity.X*hand.scale.X,velocity.Y*hand.scale.Y,
                                                          velocity.Z*hand.scale.Z});
                jacobian[k]=available_derivative(k,Dot(world,normal));
                denominator += mobility[k] * jacobian[k] * jacobian[k];
            }
            const Params start = p;
            for (double scale : {1.0, 0.5, 0.25}) {
                Params candidate = start;
                for (int k = 0; k < 5; ++k)
                    Variable(candidate, k) += std::clamp(scale * depth * mobility[k] * jacobian[k] / denominator,
                                                        -0.22, 0.22);
                if (consider(candidate)) { moved = true; break; }
            }
        }
        // Escape a straight-on singularity or a clamped Jacobian without
        // an unbounded global search. Distal choices are considered first.
        if (!moved && best.worst > kClear) {
            const double step = iteration < 4 ? 0.12 : iteration < 8 ? 0.06 : 0.025;
            for (int k : {2, 1, 0, 3, 4})
                for (double direction : {-1.0, 1.0}) {
                    Params probe = p;
                    Variable(probe, k) += direction * step;
                    moved = consider(probe) || moved;
                }
        }
        if (!moved) break;
    }
    // A bounded optimizer must not gradually worsen an already-clear
    // rendered pose. Re-evaluate it against CURRENT evidence, not yesterday's
    // contact set, after trying the desired independent-joint response.
    if(state.initialized) consider(previous);
    p = Clamp(p, b, &out.limited);
    out.pose = Pose(p, b);
    const Error final = Measure(c, p, b, hand, effective);
    out.residual_cm = final.worst;
    out.active_contacts = final.active;
    if(seconds>0)for(int j=0;j<3;++j) {
        state.angular_velocity[j*3]=(p.joint[j].flex-frame_start.joint[j].flex)/seconds;
        state.angular_velocity[j*3+1]=(p.joint[j].side-frame_start.joint[j].side)/seconds;
        state.angular_velocity[j*3+2]=(p.joint[j].twist-frame_start.joint[j].twist)/seconds;
    }
    state.pose = out.pose;
    state.initialized = true;
    return out;
}

Result Solve(const fingers::Chain& c,const fingers::Angles& desired,State& state,
             const skel::Xf& hand,const Bounds& b,const Contacts& contacts,double dt,bool reset) {
    return SolveImpl(c,desired,state,hand,b,contacts,dt,reset,false);
}
Result ResolveContacts(const fingers::Chain& c,State& state,const skel::Xf& hand,
                       const Bounds& b,const Contacts& contacts) {
    const auto desired=state.pose;
    return SolveImpl(c,desired,state,hand,b,contacts,state.frame_seconds,false,true);
}

} // namespace holevr::vr::finger_contact
