#include "vr/interact/rifle_carry.h"

#include <algorithm>
#include <cmath>

namespace holevr::vr::interact {

namespace {
FQuat Normalized(FQuat q) {
    const double n = std::sqrt(q.X * q.X + q.Y * q.Y + q.Z * q.Z + q.W * q.W);
    if (n < 1e-12) return FQuat{};
    q.X /= n; q.Y /= n; q.Z /= n; q.W /= n;
    return q;
}
}  // namespace

const char* ActionName(CarryAction a) {
    switch (a) {
        case CarryAction::kPickUp: return "pick up";
        case CarryAction::kRightLetGo: return "right hand lets go";
        case CarryAction::kDrawPistol: return "draw pistol";
        case CarryAction::kRightTakesRifle: return "right hand takes the rifle";
        case CarryAction::kLetGo: return "left hand lets go";
        default: return "none";
    }
}

CarryAction Decide(const CarryInput& in) {
    if (!in.have_rifle) return CarryAction::kNone;
    // Until the game has switched, everything stays where it is (the rifle in
    // the left hand, the pistol in its holster). A let-go of the left grip in
    // the meantime is seen right after (hold mode looks at the grip itself).
    if (in.switching) return CarryAction::kNone;

    const bool right_empty = !in.right_has_rifle && !in.right_has_pistol;
    if (in.carrying) {
        if (in.right_grip_pressed && right_empty) {
            // The holster first: it is where the right hand is used to drawing.
            if (in.right_at_pistol_holster && in.rifle_current) return CarryAction::kDrawPistol;
            // Pistol out and holstered, hand at its holster: the pistol's own draw.
            if (in.right_on_rifle_grip && !in.right_at_pistol_holster) return CarryAction::kRightTakesRifle;
        }
        if (in.toggle ? in.left_grip_pressed : !in.left_grip_down) return CarryAction::kLetGo;
        return CarryAction::kNone;
    }
    if (in.right_has_rifle) {
        if (in.right_grip_pressed && in.right_at_pistol_holster) return CarryAction::kDrawPistol;
        if (in.left_support && (in.right_toggle ? in.right_grip_pressed : in.right_grip_released))
            return CarryAction::kRightLetGo;
        return CarryAction::kNone;
    }
    if (in.left_grip_pressed && in.left_on_handguard && !in.left_busy && !in.left_support)
        return CarryAction::kPickUp;
    if (in.right_grip_pressed && right_empty) {
        // Rifle out but on the back (both hands let go of it): the pistol
        // holster still draws the pistol. With the pistol out (holstered) its
        // own holster logic draws it, so the rifle grip only counts away from it.
        if (in.rifle_current && in.right_at_pistol_holster) return CarryAction::kDrawPistol;
        if (in.right_on_rifle_grip && !(in.right_at_pistol_holster && !in.rifle_current))
            return CarryAction::kRightTakesRifle;
    }
    return CarryAction::kNone;
}

bool OnHandguard(const FVector& p, double muzzle_dist, double bore_height) {
    const bool along = p.X > 6.0 && p.X < muzzle_dist - 2.0;
    const bool beside = std::fabs(p.Y) < 10.0;
    const bool height = p.Z > bore_height - 14.0 && p.Z < bore_height + 7.0;
    return along && beside && height;
}

CarryHold HoldAsIs(const hands::Pose& hand, const FQuat& frame_rot, const FVector& grip_world) {
    CarryHold h;
    const FQuat inv_hand = hand.rotation.Inverse();
    h.rot = Normalized(inv_hand * frame_rot);
    h.pos = inv_hand.Rotate(grip_world - hand.location);
    h.support_local = frame_rot.Inverse().Rotate(hand.location - grip_world);
    return h;
}

CarryHold HoldFromGrab(const hands::Pose& hand, const FQuat& frame_rot, const FVector& grip_world, double muzzle_dist,
                       double bore_height, double fist_below_bore, const FQuat& turn) {
    const FVector p = frame_rot.Inverse().Rotate(hand.location - grip_world);
    const double lo = 8.0, hi = std::max(lo, muzzle_dist - 4.0);
    CarryHold h;
    h.support_local = FVector{std::clamp(p.X, lo, hi), 0.0, bore_height - fist_below_bore};
    h.rot = Normalized(turn);
    h.pos = h.rot.Rotate(h.support_local) * -1.0;
    return h;
}

void PlaceFromHand(const hands::Pose& hand, const CarryHold& h, FQuat* frame_rot, FVector* grip_world) {
    *frame_rot = Normalized(hand.rotation * h.rot);
    *grip_world = hand.location + hand.rotation.Rotate(h.pos);
}

void ActorFromFrame(const FQuat& frame_rot, const FVector& grip_world, const FQuat& frame_in_actor,
                    const FVector& grip_rel, FQuat* actor_rot, FVector* actor_loc) {
    *actor_rot = Normalized(frame_rot * frame_in_actor.Inverse());
    *actor_loc = grip_world - actor_rot->Rotate(grip_rel);
}

double Ease(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

FQuat BlendRot(const FQuat& a, FQuat b, double t) {
    if (a.X * b.X + a.Y * b.Y + a.Z * b.Z + a.W * b.W < 0) b = FQuat{-b.X, -b.Y, -b.Z, -b.W};
    return Normalized(FQuat{a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t, a.Z + (b.Z - a.Z) * t,
                            a.W + (b.W - a.W) * t});
}

}  // namespace holevr::vr::interact
