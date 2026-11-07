#include "vr/body/leg_pose.h"
#include "vr/body/stair_pose.h"

#include <algorithm>
#include <cmath>

namespace holevr::vr::body::legs {
namespace {
using namespace ik;
constexpr double kEnterMove = 24.0, kLeaveMove = 12.0;
constexpr double kStepDistance = 16.0, kStepAngle = 32.0 * kDegToRad;
constexpr double kStepDuration = 0.26, kStepLift = 7.0;

bool Finite(const FVector& v) { return std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z); }
bool Finite(const FQuat& q) {
    const double n = q.X*q.X + q.Y*q.Y + q.Z*q.Z + q.W*q.W;
    return std::isfinite(n) && n >= 0.25 && n <= 4.0;
}
bool Finite(const skel::Xf& t) {
    return Finite(t.translation) && Finite(t.rotation) && Finite(t.scale) &&
           std::fabs(t.scale.X) > 1e-6 && std::fabs(t.scale.Y) > 1e-6 && std::fabs(t.scale.Z) > 1e-6;
}
bool ValidFoot(const FootInput& f) {
    return f.valid && Finite(f.hip) && Finite(f.animated_hip) && Finite(f.knee) && Finite(f.ankle) &&
           Finite(f.rotation) && std::isfinite(f.sole_offset) && f.sole_offset >= 0 && f.sole_offset <= 30 &&
            std::isfinite(f.upper_length) && f.upper_length > 1 && f.upper_length < 150 &&
            std::isfinite(f.lower_length) && f.lower_length > 1 && f.lower_length < 150;
}
skel::Xf FootTransform(const FootInput& f, const Ground& g) {
    skel::Xf t;
    t.translation = {f.ankle.X, f.ankle.Y, g.point.Z + f.sole_offset};
    if(g.stair) t.translation=g.point+FVector{0,0,f.sole_offset};
    t.rotation = f.rotation;
    // The source may ALREADY contain floor IK. Align its current sole normal,
    // never add the ground slope a second time. Without a measured foot basis
    // retain the animation rather than assuming the bone's Z axis points up.
    if (Finite(f.sole_up_local) && Length(f.sole_up_local) > 0.5)
        t.rotation = Normalized(Arc(f.rotation.Rotate(Normalize(f.sole_up_local)), Normalize(g.normal)) * f.rotation);
    return t;
}
double Turn(const FQuat& a, const FQuat& b) {
    const FQuat q = Normalized(a * b.Inverse());
    return 2 * std::acos(std::clamp(std::fabs(q.W), 0.0, 1.0));
}
double Horizontal(const FVector& v) { return std::hypot(v.X, v.Y); }
void Release(FootState& f) { f.locked = f.stepping = f.step_on_stairs = false; f.stable = f.phase = 0; }
void Bypass(State& s, const Input& in) {
    for (auto& f : s.foot) f = {};
    s.have_origin = Finite(in.origin);
    s.origin = in.origin;
    s.moving = false;
    s.settle = 0;
    s.action_settle = 0.15; // allow the final animation frame to finish before reacquiring contact
}
} // namespace

FVector ExpectedSole(const FootInput& f) { return f.ankle - FVector{0, 0, f.sole_offset}; }
FVector ProbeSole(const Input& in, int side) {
    if (side < 0 || side > 1) return {};
    const FootInput& f = in.foot[side];
    FVector p = ExpectedSole(f);
    if (in.floor_known && std::isfinite(in.floor_z)) {
        if (in.state_known && in.grounded && !in.kicking && in.speed < kEnterMove &&
            f.stance_valid && Finite(f.stance_ankle) && p.Z - in.floor_z > 32.0)
            p = f.stance_ankle;
        p.Z = in.floor_z;
    }
    return p;
}

double CorrectIdlePelvisHeight(const Input& in, double source_z, double nominal_z) {
    if (!in.state_known || !in.floor_known || !in.grounded || in.kicking ||
        !std::isfinite(in.speed) || in.speed < 0 || in.speed > kLeaveMove ||
        !std::isfinite(in.floor_z) || !std::isfinite(source_z) || !std::isfinite(nominal_z) ||
        source_z <= nominal_z + 12.0) return source_z;
    for (const FootInput& f : in.foot) {
        if (f.valid && Finite(f.ankle) && std::isfinite(f.sole_offset) &&
            f.sole_offset >= 0 && f.sole_offset <= 30 &&
            f.ankle.Z - f.sole_offset - in.floor_z > 32.0) return nominal_z;
    }
    return source_z;
}

bool ValidGround(const Ground& g, const FVector& sole) {
    if (!g.hit || g.initial_overlap || !Finite(sole) || !Finite(g.point) || !Finite(g.normal) ||
        !Finite(g.support_frame)) return false;
    const double n = Length(g.normal);
    if (n < 0.5 || n > 1.5 || g.normal.Z / n < 0.70) return false;
    const FVector d = g.point - sole;
    if(g.stair) return g.support_id && std::isfinite(g.clearance_z) &&
        g.clearance_z>=g.point.Z-2 && g.clearance_z-g.point.Z<=stairs::kMaxRiser+2 &&
        g.normal.Z/n>=.94 && Horizontal(d)<=10 && std::fabs(d.Z)<=stairs::kMaxFloorDifference;
    return Horizontal(d) <= 2.0 && d.Z >= -18.0 && d.Z <= 12.0;
}
void Reset(State& s) { s = {}; }

Output Plan(State& s, const Input& in, const Ground (&ground)[2]) {
    Output out;
    out.steps_taken = s.steps_taken;
    for (int i = 0; i < 2; ++i) {
        const FootInput& f = in.foot[i];
        out.foot[i].ankle = f.ankle;
        out.foot[i].rotation = f.rotation;
        // Preserve the animation's knee bend plane, with an anatomical fallback
        // only for a nearly straight source leg.
        FVector pole = Reject(f.knee - f.animated_hip, Normalize(f.ankle - f.animated_hip, {0,0,-1}));
        if (Length(pole) < 0.1) pole = in.forward;
        out.foot[i].pole = Normalize(pole, {1,0,0});
    }
    if (!std::isfinite(in.dt) || in.dt <= 0 || in.dt > 0.15 || !Finite(in.origin) ||
        !std::isfinite(in.speed) || !in.state_known || !in.floor_known || !std::isfinite(in.floor_z) ||
        !in.grounded || in.kicking ||
        (s.have_origin && Length(in.origin - s.origin) > 100.0)) {
        Bypass(s, in);
        return out;
    }
    s.have_origin = true;
    s.origin = in.origin;
    s.action_settle = std::max(0.0, s.action_settle - in.dt);
    if (in.speed >= kEnterMove) s.moving = true;
    else if (in.speed <= kLeaveMove) s.moving = false;
    s.settle = s.moving ? 0 : std::min(1.0, s.settle + in.dt);
    const bool idle = !s.moving && s.settle >= 0.12 && s.action_settle <= 0;

    bool contact[2]{}, desired_valid[2]{};
    skel::Xf desired[2], planted[2];
    double need[2]{};
    for (int i = 0; i < 2; ++i) {
        FootState& st = s.foot[i];
        const FootInput& source = in.foot[i];
        FootInput f = source;
        FootOutput& o = out.foot[i];
        if (!ValidFoot(f)) { st = {}; continue; }
        o.solve = true;
        o.mode = Mode::Animation;
        desired_valid[i] = ValidGround(ground[i], ProbeSole(in, i));
        const bool stair=desired_valid[i]&&ground[i].stair;
        const FVector source_local = source.ankle - in.origin;
        const FVector source_velocity=st.source_previous?(source_local-st.previous_source_local)*(1/in.dt):FVector{};
        const FVector travel=Normalize(FVector{in.velocity.X,in.velocity.Y,0},in.forward);
        const double forward_rate=Dot(source_velocity,travel);
        const double source_vz = st.source_previous ?
            std::fabs(source_local.Z - st.previous_source_local.Z) / in.dt : 1e9;
        st.source_previous = true;
        st.previous_source_local = source_local;
        const bool high_idle_foot = idle && source.stance_valid && Finite(source.stance_ankle) &&
            Finite(source.stance_rotation) && ExpectedSole(source).Z - in.floor_z > 32.0 && source_vz <= 20.0;
        if (!idle) st.repairing = false;
        if (high_idle_foot && !st.repairing) {
            Release(st);
            st.repairing = true;
            st.repair.translation = source.ankle;
            st.repair.rotation = source.rotation;
        }
        if (st.repairing) {
            const bool source_recovered = ExpectedSole(source).Z - in.floor_z <= 10.0;
            FVector target = source_recovered ? source.ankle : source.stance_ankle;
            FQuat rotation = source_recovered ? source.rotation : source.stance_rotation;
            if (!source_recovered)
                target.Z = (desired_valid[i] ? ground[i].point.Z : in.floor_z) + f.sole_offset;
            const FVector change = target - st.repair.translation;
            const double distance = Length(change), limit = 160.0 * in.dt;
            st.repair.translation = distance <= limit ? target : st.repair.translation + change * (limit / distance);
            st.repair.rotation = Normalized(Scale(Normalized(rotation * st.repair.rotation.Inverse()),
                                                  1-std::exp(-12*in.dt)) * st.repair.rotation);
            f.ankle = st.repair.translation;
            f.rotation = st.repair.rotation;
            o.ankle = f.ankle;
            o.rotation = f.rotation;
            // Terrain IK has also bent the source knee toward the table.
            // The measured anatomical plane belongs to this repaired stance.
            o.pole = Normalize(source.stance_pole, in.forward);
            o.mode = Mode::Recovery;
            if (source_recovered && distance < 0.05) st.repairing = false;
        }
        bool stair_swing=false;
        if(stair && !st.repairing) {
            const double source_sole=ExpectedSole(source).Z;
            const bool raised=source_sole-in.floor_z>7 && source_sole-ground[i].point.Z>7;
            stair_swing=s.moving && (source_velocity.Z>20 || forward_rate>std::max(30.0,in.speed*.25) || raised);
            const double target_z=stair_swing?
                std::max(source.ankle.Z,ground[i].clearance_z+f.sole_offset+6):ground[i].point.Z+f.sole_offset;
            const double correction=target_z-source.ankle.Z;
            const double limit=240*in.dt;
            st.stair_adjust_z+=std::clamp(correction-st.stair_adjust_z,-limit,limit);
            f.ankle.Z=source.ankle.Z+st.stair_adjust_z;
            o.ankle=f.ankle; o.mode=Mode::StairSwing;
        } else if(!stair) st.stair_adjust_z=0;
        const FVector local = f.ankle - in.origin;
        const double vz = st.previous ? std::fabs(local.Z - st.previous_local.Z) / in.dt : 1e9;
        st.previous = true;
        st.previous_local = local;
        // A foot in a swing pose cannot become a planted foot merely because
        // actor velocity reached zero. Require it to settle close to its sole.
        contact[i] = desired_valid[i] && std::fabs(ExpectedSole(f).Z - ground[i].point.Z) <= 3.0 && vz <= 20.0;
        if(stair && s.moving) {
            if(st.stepping) Release(st); // locomotion resumes ownership of an unfinished room step
            desired[i]=FootTransform(f,ground[i]);
            if(st.locked && (st.support_id!=ground[i].support_id || stair_swing ||
                source_local.Z-st.contact_source_z>5)) Release(st);
            if(st.locked) {
                planted[i]=skel::Compose(st.planted_local,ground[i].support_frame);
                if(Horizontal(desired[i].translation-planted[i].translation)>45 ||
                   std::fabs(planted[i].translation.Z-f.sole_offset-in.floor_z)>stairs::kMaxFloorDifference)
                    Release(st);
            }
            if(!st.locked) {
                st.stable=contact[i]&&!stair_swing?st.stable+in.dt:0;
                if(st.stable>=.035) {
                    st.locked=true; st.stepping=false; st.support_id=ground[i].support_id;
                    st.contact_source_z=source_local.Z;
                    st.planted_local=skel::Relative(desired[i],ground[i].support_frame);
                    planted[i]=desired[i];
                }
            }
            if(st.locked) {
                o.ankle=planted[i].translation; o.rotation=planted[i].rotation; o.mode=Mode::StairSupport;
            }
            // Animation chooses lift, swing and landing timing. Only confirmed
            // support is locked; no procedural gait replaces kicks or jumps.
            continue;
        }
        if (!idle || !desired_valid[i]) { Release(st); continue; }
        desired[i] = FootTransform(f, ground[i]);
        if ((st.locked || st.stepping) && st.support_id != ground[i].support_id) {
            Release(st); // lost support / component changed: reacquire, never reuse a stale platform anchor
        }
        if (st.locked || st.stepping) {
            planted[i] = skel::Compose(st.planted_local, ground[i].support_frame);
            if (Length(desired[i].translation - planted[i].translation) > 60.0) Release(st);
        }
        if (!st.locked && !st.stepping) {
            st.stable = contact[i] ? st.stable + in.dt : 0;
            if (st.stable < 0.06) continue;
            st.locked = true;
            st.support_id = ground[i].support_id;
            st.planted_local = skel::Relative(desired[i], ground[i].support_frame);
            planted[i] = desired[i];
        }
        if (st.stepping) {
            st.phase = std::min(1.0, st.phase + in.dt / kStepDuration);
            const skel::Xf to = skel::Compose(st.to_local, ground[i].support_frame);
            const double t = st.phase, e = t*t*(3-2*t);
            o.ankle = st.from.translation + (to.translation - st.from.translation) * e;
            const double lift=kStepLift+(st.step_on_stairs?.5*std::fabs(to.translation.Z-st.from.translation.Z):0);
            o.ankle.Z += lift * std::sin(kPi*t);
            o.rotation = Normalized(Scale(Normalized(to.rotation * st.from.rotation.Inverse()), e) * st.from.rotation);
            o.mode = Mode::Step;
            if (st.phase >= 1) {
                st.planted_local = st.to_local;
                st.stepping = false;
                st.locked = true;
                ++s.steps_taken;
                s.next_foot = 1-i;
                o.mode = Mode::Planted;
            }
        } else {
            o.ankle = planted[i].translation;
            o.rotation = planted[i].rotation;
            o.mode = Mode::Planted;
            const double distance = Horizontal(desired[i].translation - planted[i].translation);
            const double turn = Turn(desired[i].rotation, planted[i].rotation);
            need[i] = std::max(distance / kStepDistance, turn / kStepAngle);
            if(stair) need[i]=std::max(need[i],std::fabs(desired[i].translation.Z-planted[i].translation.Z)/8);
        }
    }

    // One support foot remains planted. Both landing endpoints are immutable
    // within their supporting component for the entire step.
    if (!s.foot[0].stepping && !s.foot[1].stepping) {
        int first = need[s.next_foot] >= need[1-s.next_foot] ? s.next_foot : 1-s.next_foot;
        if (need[first] > 1 && s.foot[first].locked && s.foot[1-first].locked && contact[first]) {
            FootState& st = s.foot[first];
            st.stepping = true;
            st.phase = 0;
            st.from.translation = out.foot[first].ankle;
            st.from.rotation = out.foot[first].rotation;
            st.to_local = skel::Relative(desired[first], ground[first].support_frame);
            st.step_on_stairs=ground[first].stair;
            out.foot[first].mode = Mode::Step;
        }
    }

    // Only planted support feet may lower the pelvis to regain reach; swing
    // poses, kicks and the headset never pull the pelvis toward the floor.
    double lower_by = 0;
    bool stair_support=false;
    for (int i = 0; i < 2; ++i) {
        const FootInput& f = in.foot[i];
        const FootOutput& o = out.foot[i];
        if (!o.solve || (o.mode != Mode::Planted && o.mode != Mode::StairSupport)) continue;
        stair_support |= ground[i].stair;
        const double reach = (f.upper_length + f.lower_length) * 0.99;
        const FVector d = f.hip - o.ankle;
        const double horizontal = Horizontal(d);
        if (d.Z > 0 && horizontal < reach) {
            const double max_height = std::sqrt(std::max(0.0, reach*reach-horizontal*horizontal));
            lower_by = std::max(lower_by, d.Z-max_height);
        }
    }
    out.pelvis_offset.Z = -std::clamp(lower_by, 0.0, stair_support?stairs::kMaxFloorDifference:12.0);
    out.steps_taken = s.steps_taken;
    return out;
}
} // namespace holevr::vr::body::legs
