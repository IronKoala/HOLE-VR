// Native unit tests for the rifle held by the left hand alone (no game, no Windows).
//   g++ -std=c++20 -Isrc tests/test_rifle_carry.cpp src/vr/interact/rifle_carry.cpp -o /tmp/test_rifle_carry && /tmp/test_rifle_carry
#include <cmath>
#include <cstdio>

#include "vr/interact/rifle_carry.h"

using namespace holevr::vr;
using namespace holevr::vr::interact;

static int failures = 0;
#define EXPECT(c)                                                     \
    do {                                                              \
        if (!(c)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);  \
            ++failures;                                               \
        }                                                             \
    } while (0)

static bool Near(const FVector& a, const FVector& b, double tol = 1e-6) {
    return std::fabs(a.X - b.X) < tol && std::fabs(a.Y - b.Y) < tol && std::fabs(a.Z - b.Z) < tol;
}
static bool SameRot(const FQuat& a, const FQuat& b, double tol = 1e-6) {
    const double d = std::fabs(a.X * b.X + a.Y * b.Y + a.Z * b.Z + a.W * b.W);
    return d > 1.0 - tol;
}
static FQuat Rot(double p, double y, double r) { return ToQuat(FRotator{p, y, r}); }

// Rifle out in both hands; the typical "transition to the pistol" moments.
static void TestDecideFromRifle() {
    CarryInput in;
    in.have_rifle = true;
    in.rifle_current = true;
    in.right_has_rifle = true;
    in.left_support = true;
    in.left_grip_down = true;

    EXPECT(Decide(in) == CarryAction::kNone);
    // Let go of the right grip: the left hand keeps the rifle.
    in.right_grip_released = true;
    EXPECT(Decide(in) == CarryAction::kRightLetGo);
    // Toggle mode: letting go does nothing, a squeeze lets go.
    in.right_toggle = true;
    EXPECT(Decide(in) == CarryAction::kNone);
    in.right_grip_released = false;
    in.right_grip_pressed = true;
    EXPECT(Decide(in) == CarryAction::kRightLetGo);
    in.right_toggle = false;
    // Never pressed the grip (rifles stay in the hand): squeezing at the
    // pistol holster draws the pistol straight away.
    in.right_at_pistol_holster = true;
    EXPECT(Decide(in) == CarryAction::kDrawPistol);
    // Only one hand on the rifle: a squeeze at the holster still draws the pistol.
    in.left_support = false;
    EXPECT(Decide(in) == CarryAction::kDrawPistol);
    // ...but letting go of a one-handed rifle does nothing (it stays in the hand).
    in.right_at_pistol_holster = false;
    in.right_grip_pressed = false;
    in.right_grip_released = true;
    EXPECT(Decide(in) == CarryAction::kNone);
}

// The left hand holds the rifle alone.
static void TestDecideCarrying() {
    CarryInput in;
    in.have_rifle = true;
    in.rifle_current = true;
    in.carrying = true;
    in.left_grip_down = true;
    EXPECT(Decide(in) == CarryAction::kNone);

    // Right grip at the holster: draw the pistol (the rifle stays in the left).
    in.right_grip_pressed = true;
    in.right_at_pistol_holster = true;
    EXPECT(Decide(in) == CarryAction::kDrawPistol);
    // Right grip on the rifle's grip: take it back.
    in.right_at_pistol_holster = false;
    in.right_on_rifle_grip = true;
    EXPECT(Decide(in) == CarryAction::kRightTakesRifle);
    // Pistol out and in the right hand: the right grip does nothing to the rifle.
    in.rifle_current = false;
    in.right_has_pistol = true;
    EXPECT(Decide(in) == CarryAction::kNone);
    // Pistol holstered, right grip on the rifle grip: switch to the rifle.
    in.right_has_pistol = false;
    EXPECT(Decide(in) == CarryAction::kRightTakesRifle);
    // Pistol current and holstered, right hand at its holster: that is the
    // pistol's own draw, not ours.
    in.right_on_rifle_grip = false;
    in.right_at_pistol_holster = true;
    EXPECT(Decide(in) == CarryAction::kNone);
    // ...even with the rifle's grip in reach too.
    in.right_on_rifle_grip = true;
    EXPECT(Decide(in) == CarryAction::kNone);
    in.right_on_rifle_grip = false;

    // Letting go of the left grip.
    in.right_grip_pressed = false;
    in.right_at_pistol_holster = false;
    in.left_grip_down = false;
    EXPECT(Decide(in) == CarryAction::kLetGo);
    // Grip toggle: only a new squeeze lets go.
    in.toggle = true;
    EXPECT(Decide(in) == CarryAction::kNone);
    in.left_grip_pressed = true;
    EXPECT(Decide(in) == CarryAction::kLetGo);

    // While a switch is pending nothing changes.
    in.switching = true;
    EXPECT(Decide(in) == CarryAction::kNone);
    // No rifle at all: nothing.
    in.switching = false;
    in.have_rifle = false;
    EXPECT(Decide(in) == CarryAction::kNone);
}

// Pistol out, rifle on the back.
static void TestDecideFromPistol() {
    CarryInput in;
    in.have_rifle = true;
    in.right_has_pistol = true;
    in.left_grip_pressed = true;
    in.left_grip_down = true;
    EXPECT(Decide(in) == CarryAction::kNone);   // not on the handguard
    in.left_on_handguard = true;
    EXPECT(Decide(in) == CarryAction::kPickUp);
    // The left grip took the slide or a magazine first: no rifle.
    in.left_busy = true;
    EXPECT(Decide(in) == CarryAction::kNone);
    in.left_busy = false;

    // Pistol holstered, right hand on the rifle's grip on the back: rifle out.
    in = CarryInput{};
    in.have_rifle = true;
    in.right_grip_pressed = true;
    in.right_on_rifle_grip = true;
    EXPECT(Decide(in) == CarryAction::kRightTakesRifle);
    // Also at the pistol holster: the pistol's draw wins.
    in.right_at_pistol_holster = true;
    EXPECT(Decide(in) == CarryAction::kNone);
    // Rifle current but on the back (both hands let go), right at the holster:
    // draw the pistol.
    in.rifle_current = true;
    EXPECT(Decide(in) == CarryAction::kDrawPistol);
    in.right_at_pistol_holster = false;
    EXPECT(Decide(in) == CarryAction::kRightTakesRifle);
}

static void TestZone() {
    // AR-15: muzzle about 40 cm ahead of the grip, bore 5 cm above it.
    const double muzzle = 40, bore = 5;
    EXPECT(OnHandguard(FVector{20, 0, 1}, muzzle, bore));    // fist under the handguard
    EXPECT(OnHandguard(FVector{8, 3, -6}, muzzle, bore));    // magwell
    EXPECT(!OnHandguard(FVector{2, 0, 0}, muzzle, bore));    // the pistol grip itself
    EXPECT(!OnHandguard(FVector{39, 0, 0}, muzzle, bore));   // past the end
    EXPECT(!OnHandguard(FVector{20, 12, 0}, muzzle, bore));  // beside it
    EXPECT(!OnHandguard(FVector{20, 0, -12}, muzzle, bore)); // well below
}

static void TestPoses() {
    hands::Pose hand{FVector{10, 20, 130}, Rot(10, 70, -15)};
    const FQuat frame = Rot(-5, 60, 3);
    const FVector grip{-12, 5, 125};

    // As is: placing from the same hand gives the same rifle.
    CarryHold h = HoldAsIs(hand, frame, grip);
    FQuat f;
    FVector g;
    PlaceFromHand(hand, h, &f, &g);
    EXPECT(SameRot(f, frame));
    EXPECT(Near(g, grip, 1e-6));
    // The support point is where the hand is, in the gun frame.
    EXPECT(Near(grip + frame.Rotate(h.support_local), hand.location, 1e-6));

    // The hand moves and turns: the rifle moves rigidly with it.
    hands::Pose moved{FVector{40, -10, 120}, Rot(30, -20, 5) * hand.rotation};
    PlaceFromHand(moved, h, &f, &g);
    EXPECT(Near(g + f.Rotate(h.support_local), moved.location, 1e-6));

    // From a grab: the rifle lines up with the hand, the bore 4 cm over the
    // fist, at the point grabbed (clamped to the handguard).
    hands::Pose at{grip + frame.Rotate(FVector{22, 1, 2}), Rot(0, 90, 0)};
    CarryHold hg = HoldFromGrab(at, frame, grip, 40, 5, 4, FQuat{});
    EXPECT(Near(hg.support_local, FVector{22, 0, 1}, 1e-6));
    PlaceFromHand(at, hg, &f, &g);
    EXPECT(SameRot(f, at.rotation));
    EXPECT(Near(g + f.Rotate(hg.support_local), at.location, 1e-6));
    // The bore line passes 4 cm above the fist: the point straight above the
    // fist, at bore height, is on the barrel axis from the grip point.
    const FVector bore_pt = at.location + f.Rotate(FVector{0, 0, 4});
    const FVector rel = f.Inverse().Rotate(bore_pt - g);
    EXPECT(std::fabs(rel.Y) < 1e-6 && std::fabs(rel.Z - 5.0) < 1e-6);
    // Grabbed past the front: kept on the handguard.
    hands::Pose far{grip + frame.Rotate(FVector{60, 0, 0}), Rot(0, 0, 0)};
    EXPECT(std::fabs(HoldFromGrab(far, frame, grip, 40, 5, 4, FQuat{}).support_local.X - 36.0) < 1e-9);
    // A turn: the barrel is turned by it relative to the hand.
    const FQuat turn = Rot(-20, 0, 0);
    hg = HoldFromGrab(at, frame, grip, 40, 5, 4, turn);
    PlaceFromHand(at, hg, &f, &g);
    EXPECT(SameRot(f, at.rotation * turn));
    EXPECT(Near(g + f.Rotate(hg.support_local), at.location, 1e-6));

    // At a grip point: that point is at the hand, the rifle lies along the
    // hand turned by `turn`.
    const FVector pt{25.5, -5, 10};
    CarryHold hp = HoldAtPoint(pt, turn);
    EXPECT(Near(hp.support_local, pt, 1e-12));
    PlaceFromHand(at, hp, &f, &g);
    EXPECT(SameRot(f, at.rotation * turn));
    EXPECT(Near(g + f.Rotate(pt), at.location, 1e-6));

    // Actor from the gun frame: the frame and grip point come back out.
    const FQuat frame_in_actor = Rot(0, -90, 0);
    const FVector grip_rel{0, 3, -4};
    FQuat ar;
    FVector al;
    ActorFromFrame(frame, grip, frame_in_actor, grip_rel, &ar, &al);
    EXPECT(SameRot(ar * frame_in_actor, frame));
    EXPECT(Near(al + ar.Rotate(grip_rel), grip, 1e-6));
}

static void TestBlend() {
    EXPECT(Ease(-1) == 0.0 && Ease(0) == 0.0 && Ease(1) == 1.0 && Ease(2) == 1.0);
    EXPECT(std::fabs(Ease(0.5) - 0.5) < 1e-12);
    const FQuat a = Rot(0, 0, 0), b = Rot(0, 90, 0);
    EXPECT(SameRot(BlendRot(a, b, 0), a));
    EXPECT(SameRot(BlendRot(a, b, 1), b));
    // The short way round, even when b is given with the other sign.
    const FQuat nb{-b.X, -b.Y, -b.Z, -b.W};
    EXPECT(SameRot(BlendRot(a, nb, 0.5), Rot(0, 45, 0), 1e-9));
}

int main() {
    TestDecideFromRifle();
    TestDecideCarrying();
    TestDecideFromPistol();
    TestZone();
    TestPoses();
    TestBlend();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("rifle carry: all tests passed\n");
    return 0;
}
