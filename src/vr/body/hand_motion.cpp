#include "vr/body/hand_motion.h"
#include <algorithm>
#include <cmath>

namespace holevr::vr::body::hand_motion {
namespace {
using namespace ik;
bool Finite(const FVector& v) { return std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z); }
bool Finite(const FQuat& q) {
    return std::isfinite(q.X) && std::isfinite(q.Y) && std::isfinite(q.Z) && std::isfinite(q.W) &&
           q.X*q.X + q.Y*q.Y + q.Z*q.Z + q.W*q.W > 1e-12;
}
FVector Bounded(FVector v, double maximum) {
    const double length = Length(v);
    return length > maximum && length > 0 ? v * (maximum / length) : v;
}
FVector RotationError(const FQuat& to, const FQuat& from) {
    FQuat q = Normalized(to * from.Inverse());
    if (q.W < 0) q = {-q.X,-q.Y,-q.Z,-q.W};
    const FVector axis{q.X,q.Y,q.Z};
    const double length = Length(axis);
    return length > 1e-9 ? axis * (2.0 * std::atan2(length, std::clamp(q.W, 0.0, 1.0)) / length)
                         : axis * 2.0;
}
bool ValidHit(const Sweep& hit) {
    return hit.available && (!hit.hit ||
           (Finite(hit.centre) && Finite(hit.point) && Finite(hit.normal) && Length(hit.normal) > 0.5));
}
}

void Reset(State& state) { state = {}; }

Result Step(State& state, const Settings& settings, const Input& input, const SweepFn& sweep,
            const BoxSweepFn& box_sweep) {
    using namespace ik;
    Result result;
    if (!input.tracked || !Finite(input.target) || !Finite(input.anchor) || !Finite(input.rotation) || !sweep) {
        Reset(state);
        result.reset = true;
        return result;
    }
    const auto& palm = input.palm;
    result.palm_shape = palm.valid;
    if (palm.valid && (!Finite(palm.centre) || !Finite(palm.rotation) || !Finite(palm.half) ||
        palm.half.X < 0.05 || palm.half.Y < 0.05 || palm.half.Z < 0.05 || Length(palm.half) > 50 || Length(palm.centre) > 100)) {
        Reset(state); result.reset = true; return result;
    }
    const double dt = std::isfinite(input.dt) ? std::clamp(input.dt, 0.0, settings.max_frame_seconds) : 0.0;
    const bool jump = state.valid && (Length(input.target - state.last_target) > settings.discontinuity_cm ||
                                     Length(input.anchor - state.last_anchor) > settings.discontinuity_cm);
    if (input.discontinuity || jump || input.dt > settings.max_frame_seconds) {
        Reset(state);
        result.reset = true;
    }
    state.last_target = input.target;
    state.last_anchor = input.anchor;
    auto query = [&](const FVector& from, const FVector& to, const FQuat& rotation, double padding = 0.0) {
        if (result.queries >= std::clamp(settings.max_queries, 8, 128)) {
            result.budget_limited = true;
            return Sweep{};
        }
        ++result.queries;
        Sweep hit;
        if (palm.valid) {
            if (box_sweep) {
                const FVector offset = rotation.Rotate(palm.centre);
                hit = box_sweep(from + offset, to + offset, palm.half + FVector{padding,padding,padding},
                                Normalized(rotation * palm.rotation));
                // The rest of the solver operates on the grip root, not the
                // box centre. This preserves the calibrated wrist offset.
                hit.centre = hit.centre - offset;
            }
        } else hit = sweep(from, to, settings.radius_cm);
        if (!ValidHit(hit)) {
            hit.available = false;
            result.query_failed = true;
        }
        if (hit.hit && hit.available) hit.normal = Normalize(hit.normal);
        return hit;
    };
    auto contact = [&](const Sweep& hit) {
        result.touching = true;
        result.point = hit.point;
        result.normal = hit.normal;
    };
    auto reacquire = [&]() {
        const FQuat rotation = Normalized(input.rotation);
        const Sweep path = query(input.anchor, input.target, rotation);
        if (!path.available || path.initial_overlap) {
            state.valid = false;
            state.velocity = state.angular_velocity = {};
            return false;
        }
        state.position = path.hit ? path.centre + path.normal * settings.skin_cm : input.target;
        state.rotation = rotation;
        state.velocity = state.angular_velocity = {};
        state.valid = true;
        if (path.hit) contact(path);
        return true;
    };
    if (!state.valid) {
        result.reset = true;
        if (!reacquire()) return result;
    } else {
        const FVector start = state.position;
        const bool was_touching = state.touching;
        const FVector old_normal = state.normal;
        FVector proposed = start;
        const double omega = 2.0 * kPi * settings.frequency_hz;
        const double mass = std::max(0.01, settings.virtual_mass_kg);
        const double max_acceleration = std::max(0.0, settings.max_force_n) * 100.0 / mass;
        // Small math steps keep the capped PD controller stable. World
        // queries stay bounded: sweep the whole travelled segment afterwards.
        const int steps = dt > 0 ? std::clamp(static_cast<int>(std::ceil(dt * 360.0)), 1, 36) : 0;
        const double h = steps ? dt / steps : 0;
        result.substeps = steps;
        for (int i = 0; i < steps; ++i) {
            FVector acceleration = (input.target - proposed) * (omega * omega) -
                                   state.velocity * (2.0 * settings.damping_ratio * omega);
            acceleration = Bounded(acceleration, max_acceleration);
            result.peak_force_n = std::max(result.peak_force_n, Length(acceleration) * mass / 100.0);
            state.velocity = Bounded(state.velocity + acceleration * h, settings.max_speed_cm_s);
            proposed = proposed + state.velocity * h;
        }
        // Even a stationary hand queries, so moving objects cannot inherit
        // an old clear result. Engine adapters may use a tiny nonzero sweep.
        const Sweep move = query(start, proposed, state.rotation);
        if (!move.available) {
            state.velocity = state.angular_velocity = {};
        } else if (move.initial_overlap) {
            // A moving object overtook the previous proxy. Reacquire only
            // along a verified anchor path; never teleport to the raw target.
            result.recovering = true;
            if (!reacquire()) return result;
        } else if (move.hit) {
            contact(move);
            state.position = move.centre + move.normal * settings.skin_cm;
            const double inward = Dot(state.velocity, move.normal);
            if (inward < 0) state.velocity = state.velocity - move.normal * inward;
            const double friction = std::exp(-std::max(0.0, settings.friction_per_s) * dt);
            const FVector tangent = Reject(proposed - move.centre, move.normal) * friction;
            state.velocity = state.velocity * friction;
            if (Length(tangent) > 1e-5) {
                const FVector slide_from = state.position;
                const Sweep slide = query(slide_from, slide_from + tangent, state.rotation);
                if (slide.available && !slide.initial_overlap) {
                    state.position = slide.hit ? slide.centre + slide.normal * settings.skin_cm : slide_from + tangent;
                    result.sliding = Length(state.position - slide_from) > 1e-4;
                    if (slide.hit) {
                        contact(slide);
                        const double into_second = Dot(state.velocity, slide.normal);
                        if (into_second < 0) state.velocity = state.velocity - slide.normal * into_second;
                    }
                } else state.velocity = {};
            }
        } else {
            state.position = proposed;
            // Contact at rest needs fresh evidence too; only probe a very
            // small gap, so moving away immediately releases the surface.
            if (was_touching) {
                const Sweep nearby = query(proposed, proposed - old_normal * (settings.skin_cm + 0.08), state.rotation);
                if (nearby.available && nearby.hit && !nearby.initial_overlap) contact(nearby);
            }
        }

        // Flatten only against this frame's verified contact. Palm/back/chop
        // orientation gates keep incidental side impacts from turning a hand.
        FQuat wanted = Normalized(input.rotation);
        double pressure = 0;
        if (palm.valid && palm.flatten && result.touching) {
            const FQuat raw_box = Normalized(wanted * palm.rotation);
            const FVector into = result.normal * -1.0;
            const double facing = Dot(raw_box.Rotate({0,0,1}), into);
            const double side = std::fabs(Dot(raw_box.Rotate({1,0,0}), into));
            const double press_cm = std::isfinite(palm.press_cm) ? std::clamp(palm.press_cm, 0.5, 20.0) : 4.0;
            pressure = std::clamp(Dot(state.position - input.target, result.normal) / press_cm, 0.0, 1.0);
            pressure *= std::clamp((facing + 0.35) / 0.55, 0.0, 1.0) *
                        std::clamp((0.85 - side) / 0.25, 0.0, 1.0);
            wanted = Normalized(Scale(Arc(raw_box.Rotate({0,0,1}), into), pressure) * wanted);
        }
        FQuat proposed_rotation = state.rotation;
        const double angular_omega = 2.0 * kPi * settings.angular_frequency_hz;
        const double inertia = std::max(1e-6, settings.virtual_inertia);
        const double max_angular_acceleration = std::max(0.0, settings.max_torque_nm) / inertia;
        if (move.available && !move.initial_overlap) for (int i = 0; i < steps; ++i) {
            FVector acceleration = RotationError(wanted, proposed_rotation) * (angular_omega * angular_omega) -
                                   state.angular_velocity * (2.0 * settings.damping_ratio * angular_omega);
            acceleration = Bounded(acceleration, max_angular_acceleration);
            result.peak_torque_nm = std::max(result.peak_torque_nm, Length(acceleration) * inertia);
            state.angular_velocity = Bounded(state.angular_velocity + acceleration * h, settings.max_angular_speed);
            const double turn = Length(state.angular_velocity) * h;
            if (turn > 1e-12)
                proposed_rotation = Normalized(AxisAngle(state.angular_velocity, turn) * proposed_rotation);
        }
        if (!palm.valid) state.rotation = proposed_rotation;
        else {
            // A sphere cannot observe a rotating palm. Bound every <=5-degree
            // arc by a conservatively expanded, fixed-orientation box. Near a
            // wall, first sweep the real palm out to verified rotational
            // clearance, rotate within that empty envelope, then sweep it back
            // toward the spring target. Every intermediate motion is checked;
            // corners can reject rotation instead of accepting a buried palm.
            const double angle = Length(RotationError(proposed_rotation, state.rotation));
            const int turns = angle > 1e-7 ? static_cast<int>(std::ceil(angle / (5.0*kDegToRad))) : 0;
            const FQuat begin_rotation = state.rotation;
            const FQuat delta = Normalized(proposed_rotation * begin_rotation.Inverse());
            for (int turn = 1; turn <= turns; ++turn) {
                const FQuat next = Normalized(Scale(delta, static_cast<double>(turn)/turns) * begin_rotation);
                const double part = Length(RotationError(next, state.rotation));
                const double padding = 2.0 * (Length(palm.centre) + Length(palm.half)) * std::sin(part * 0.5) + 1e-5;
                FVector clearance = state.position;
                bool clear = false;
                for (int attempt = 0; attempt < 4; ++attempt) {
                    const Sweep guard = query(clearance, clearance, state.rotation, padding);
                    if (!guard.available) break;
                    if (!guard.hit) { clear = true; break; }
                    // Initial-overlap ImpactPoint is not a reliable surface
                    // point. Recover the actual plane with the unpadded box.
                    const Sweep plane = query(clearance, clearance - guard.normal * (padding*3.0 + 0.2), state.rotation);
                    if (!plane.available || !plane.hit || plane.initial_overlap) break;
                    const FQuat box = Normalized(state.rotation * palm.rotation);
                    const double added = padding * (std::fabs(Dot(box.Rotate({1,0,0}), plane.normal)) +
                        std::fabs(Dot(box.Rotate({0,1,0}), plane.normal)) + std::fabs(Dot(box.Rotate({0,0,1}), plane.normal)));
                    const double gap = Dot(clearance - plane.centre, plane.normal);
                    const FVector out = clearance + plane.normal * std::max(settings.skin_cm, added + settings.skin_cm - gap);
                    const Sweep retreat = query(clearance, out, state.rotation);
                    if (!retreat.available || retreat.hit) break;
                    clearance = out;
                }
                if (!clear) { state.angular_velocity = {}; result.rotation_blocked = true; break; }
                const Sweep seated = query(clearance, proposed, next);
                if (!seated.available || seated.initial_overlap) { state.angular_velocity = {}; result.rotation_blocked = true; break; }
                state.position = seated.hit ? seated.centre + seated.normal * settings.skin_cm : proposed;
                state.rotation = next;
                result.touching = false;
                if (seated.hit) {
                    contact(seated);
                    const double inward = Dot(state.velocity, seated.normal);
                    if (inward < 0) state.velocity = state.velocity - seated.normal * inward;
                }
            }
            if (result.touching && pressure > 0) {
                const FVector surface = Normalized(state.rotation * palm.rotation).Rotate({0,0,1});
                result.flatness = pressure * std::clamp((Dot(surface, result.normal * -1.0) - 0.5) / 0.5, 0.0, 1.0);
            }
        }
    }
    result.valid = state.valid;
    result.position = state.position;
    result.rotation = state.rotation;
    result.error_cm = Length(input.target - state.position);
    result.speed_cm_s = Length(state.velocity);
    state.touching = result.touching;
    state.normal = result.normal;
    state.blocked_seconds = result.touching && result.error_cm > settings.recovery_distance_cm
                                ? state.blocked_seconds + dt : 0.0;
    // Persistent disagreement changes recovery state, never the positional
    // collision rule. Force is already capped; releasing is always possible.
    result.recovering = result.recovering || state.blocked_seconds >= settings.recovery_seconds;
    return result;
}

} // namespace holevr::vr::body::hand_motion
