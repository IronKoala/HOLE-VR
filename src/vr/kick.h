#pragma once
// Kick geometry shared by the controls' native trace adapter and body posing.
// No engine access: the original game's timeline and hit processing still run.
#include <cstdint>
#include "vr/math.h"

namespace holevr::vr::kick {
constexpr double kNativePitch = 30.0;
constexpr double kNativeReach = 160.0;
struct Plan {
    bool valid = false;
    int side = 0;                 // 0 left (also centre/default), 1 right
    FRotator aim;                 // desired world direction, without head roll
    FRotator control;             // cancels the native trace's extra +30 pitch
    FVector direction;
    uint64_t serial = 0;          // assigned by controls once per accepted start
    double blend = 0;            // current native KickAnimValue.BlendAlpha, unknown=0
};
Plan MakePlan(const FQuat& head, const FQuat& body, bool right_available = true);

struct VisualLeg {
    FVector hip, knee, ankle;
    FQuat foot;
};
// All inputs/output are in one space. body_frame has anatomical forward/right/up
// axes; aim_frame points along Plan.aim transformed into that same space.
// References include the current body frame but no kick animation. Mirroring a
// rotation delta then applying the target reference preserves different left/
// right bone axes. The selected hip is the pivot; it must also be published as
// the native trace origin. Native +30 pitch is removed in both paths. Pass
// native BlendAlpha as correction_blend: only the added aim rotation is
// blended; the input leg already contains the game's attack/recovery blend.
bool MapVisual(const VisualLeg& native_left, const FVector& selected_hip, bool right,
               const FQuat& body_frame, const FQuat& aim_frame,
               const FQuat& left_foot_reference, const FQuat& selected_foot_reference,
               VisualLeg* out, double correction_blend = 1.0);

// A scoped, one-shot native GetSocketLocation result replacement. The adapter
// calls the original exec first, then this; no FFrame parsing or guessed offsets.
// The expected original hip prevents an unrelated socket read being replaced.
struct OriginOverride {
    uintptr_t mesh = 0;
    FVector expected, replacement;
    bool armed = false;
    bool consumed = false;
    void Arm(uintptr_t owner, const FVector& left_hip, const FVector& selected_hip);
    bool Apply(uintptr_t context, FVector* result);
    void Clear() { *this = {}; }
};
}  // namespace holevr::vr::kick
