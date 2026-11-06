#include "vr/kick.h"
#include <algorithm>

namespace holevr::vr::kick {
namespace {
bool Finite(const FVector& p) { return std::isfinite(p.X) && std::isfinite(p.Y) && std::isfinite(p.Z); }
bool Unit(const FQuat& q, FQuat* out) {
    const double n = q.X*q.X + q.Y*q.Y + q.Z*q.Z + q.W*q.W;
    if (!out || !std::isfinite(n) || n < 1e-12) return false;
    const double s = 1.0/std::sqrt(n);
    *out = {q.X*s, q.Y*s, q.Z*s, q.W*s};
    return true;
}
FVector Reflect(const FVector& p, const FQuat& frame) {
    FVector local = frame.Inverse().Rotate(p);
    local.Y = -local.Y;
    return frame.Rotate(local);
}
FQuat ReflectDelta(const FQuat& delta, const FQuat& frame) {
    const FQuat local = frame.Inverse()*delta*frame;
    // M R M for sagittal reflection M=diag(1,-1,1). Quaternion vector is axial.
    return frame*FQuat{-local.X, local.Y, -local.Z, local.W}*frame.Inverse();
}
FQuat Fraction(FQuat q, double t) {
    if (q.W < 0) q = {-q.X,-q.Y,-q.Z,-q.W};
    const double half = std::acos(std::clamp(q.W,-1.0,1.0));
    if (half < 1e-9) return {};
    const double s = std::sin(half*t)/std::sin(half);
    return {q.X*s,q.Y*s,q.Z*s,std::cos(half*t)};
}
}  // namespace

Plan MakePlan(const FQuat& head, const FQuat& body, bool right_available) {
    FQuat h, b;
    if (!Unit(head, &h) || !Unit(body, &b)) return {};
    Plan p;
    p.valid = true;
    p.aim = ToRotator(h);
    p.aim.Pitch = std::clamp(p.aim.Pitch, -50.0, 30.0);
    p.aim.Yaw = NormalizeAxis(p.aim.Yaw);
    p.aim.Roll = 0;
    // Small centre dead band avoids head jitter choosing the right leg.
    p.side = right_available && NormalizeAxis(p.aim.Yaw-YawOf(b)) > 5.0 ? 1 : 0;
    p.control = p.aim;
    p.control.Pitch -= kNativePitch;
    p.direction = ToQuat(p.aim).Rotate({1,0,0});
    return p;
}

bool MapVisual(const VisualLeg& source, const FVector& hip, bool right,
               const FQuat& body_frame, const FQuat& aim_frame,
               const FQuat& source_ref, const FQuat& target_ref, VisualLeg* out, double correction_blend) {
    FQuat b, a, anim, sr, tr;
    if (!out || !std::isfinite(correction_blend) || !Finite(source.hip) || !Finite(source.knee) || !Finite(source.ankle) || !Finite(hip) ||
        !Unit(body_frame, &b) || !Unit(aim_frame, &a) || !Unit(source.foot, &anim) ||
        !Unit(source_ref, &sr) || !Unit(target_ref, &tr)) return false;
    const FQuat turn = Fraction(a*(b*ToQuat({kNativePitch,0,0})).Inverse(),
                               std::clamp(correction_blend,0.0,1.0));
    const auto point = [&](const FVector& p) {
        const FVector d = p-source.hip;
        return hip+turn.Rotate(right ? Reflect(d,b) : d);
    };
    FQuat delta = anim*sr.Inverse();
    if (right) delta = ReflectDelta(delta,b);
    VisualLeg mapped{hip, point(source.knee), point(source.ankle), {}};
    if (!Unit(turn*delta*tr, &mapped.foot)) return false;
    *out = mapped;
    return true;
}

void OriginOverride::Arm(uintptr_t owner, const FVector& left, const FVector& selected) {
    Clear();
    if (!owner || !Finite(left) || !Finite(selected)) return;
    mesh = owner; expected = left; replacement = selected; armed = true;
}
bool OriginOverride::Apply(uintptr_t context, FVector* result) {
    if (!armed || context != mesh || !result) return false;
    // Never consume some later socket call after the expected first read failed.
    armed = false;
    if (!Finite(*result)) return false;
    const FVector d = *result-expected;
    if (d.X*d.X+d.Y*d.Y+d.Z*d.Z > 0.01) return false;
    *result = replacement;
    consumed = true;
    return true;
}
}  // namespace holevr::vr::kick
