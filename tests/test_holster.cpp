// Native unit tests for the body holsters (no game, no Windows).
//   g++ -std=c++20 -Isrc tests/test_holster.cpp src/vr/interact/holster.cpp -o /tmp/test_holster && /tmp/test_holster
#include <cmath>
#include <cstdio>
#include <initializer_list>

#include "vr/interact/holster.h"

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
static double Dot(const FVector& a, const FVector& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
static FQuat Yaw(double deg) { return ToQuat(FRotator{0, deg, 0}); }

static void TestSpots() {
    // Body turned 90 degrees (facing +Y): "right" is then -X in the world.
    const FQuat body = Yaw(90);
    const FVector belt{100, 200, 95};
    HolsterSpot s = DefaultSpot(HolsterId::kPistol);
    EXPECT(s.enabled && s.hand == 1 && s.anchor == HolsterAnchor::kBelt);
    EXPECT(s.offset.Y > 15);                         // right hip
    FVector w = SpotWorld(s, belt, body);
    EXPECT(w.X < belt.X - 15);                       // to the body's right = world -X
    EXPECT(Near(SpotOffset(w, belt, body), s.offset));
    HolsterSpot m = DefaultSpot(HolsterId::kMag);
    EXPECT(m.enabled && m.hand == 0 && m.offset.Y < -15);  // left hip
    EXPECT(!DefaultSpot(HolsterId::kRifle).enabled);
    EXPECT(!DefaultSpot(HolsterId::kRifleMag).enabled);
    // Calibration round trip from an arbitrary hand position.
    FVector hand{90, 230, 80};
    s.offset = SpotOffset(hand, belt, body);
    EXPECT(Near(SpotWorld(s, belt, body), hand));
}

static void TestPistolFrame() {
    for (double yaw : {0.0, 90.0, -135.0}) {
        const FQuat body = Yaw(yaw);
        const FVector fwd = body.Rotate(FVector{1, 0, 0}), right = body.Rotate(FVector{0, 1, 0});
        const FQuat f = PistolHolsterFrame(body, 0);
        // Muzzle straight down, slide toward the front, right side out.
        EXPECT(Near(f.Rotate(FVector{1, 0, 0}), FVector{0, 0, -1}, 1e-9));
        EXPECT(Near(f.Rotate(FVector{0, 0, 1}), fwd, 1e-9));
        EXPECT(Near(f.Rotate(FVector{0, 1, 0}), right, 1e-9));
        // Forward cant: the muzzle swings back, the grip (gun -Z, behind and
        // above the bore) tips forward and up.
        const FQuat c = PistolHolsterFrame(body, 10);
        const FVector barrel = c.Rotate(FVector{1, 0, 0});
        EXPECT(Dot(barrel, fwd) < -0.15 && Dot(barrel, fwd) > -0.2);
        EXPECT(barrel.Z < -0.95);
        const FVector grip_dir = c.Rotate(FVector{-0.3, 0, -1});  // down the grip from the bore
        EXPECT(Dot(grip_dir, fwd) < 0);    // grip sticks out behind the slide
        EXPECT(grip_dir.Z > 0);            // and above it (it is the part you grab)
        EXPECT(Near(c.Rotate(FVector{0, 1, 0}), right, 1e-9));
        // Proper rotation (unit length).
        EXPECT(std::fabs(c.X * c.X + c.Y * c.Y + c.Z * c.Z + c.W * c.W - 1.0) < 1e-9);
    }
}

static void TestMagFrame() {
    const double rake = 18.0;
    const double r = rake * kDegToRad;
    const FVector in_g{std::sin(r), 0, std::cos(r)};  // into the magwell, gun frame
    for (double yaw : {0.0, 45.0, 180.0}) {
        const FQuat body = Yaw(yaw);
        const FQuat f = MagPouchGunFrame(body, rake);
        EXPECT(Near(f.Rotate(in_g), FVector{0, 0, 1}, 1e-9));                // magwell straight up
        EXPECT(Near(f.Rotate(FVector{0, 1, 0}), body.Rotate(FVector{0, 1, 0}), 1e-9));
        EXPECT(Dot(f.Rotate(FVector{1, 0, 0}), body.Rotate(FVector{1, 0, 0})) > 0.9);  // front faces forward
    }
    // No rake (rifle): the gun is level.
    EXPECT(Near(MagPouchGunFrame(Yaw(0), 0).Rotate(FVector{1, 0, 0}), FVector{1, 0, 0}, 1e-9));
}

// Feeds a hand position and grip state; spot at the origin.
static HolsterEvent Step(HolsterDraw& h, FVector hand, bool down, bool pressed, bool freeze = false,
                         double dt = 1.0 / 72) {
    DrawInput in;
    in.hand = hand;
    in.spot = FVector{0, 0, 0};
    in.grip_down = down;
    in.grip_pressed = pressed;
    in.freeze = freeze;
    in.dt = dt;
    return h.Update(in);
}

static void TestHoldMode() {
    HolsterDraw h;
    h.Configure(12.0, DrawMode::kHold, 0.2, 0.08);
    EXPECT(h.State() == HolsterState::kHolstered && h.Blend() == 1.0);
    // Squeezing away from the holster does nothing.
    EXPECT(Step(h, {40, 0, 0}, true, true) == HolsterEvent::kNone);
    EXPECT(!h.InHand());
    Step(h, {40, 0, 0}, false, false);
    // Reaching in: hover tick once, not every frame.
    EXPECT(Step(h, {10, 0, 0}, false, false) == HolsterEvent::kHoverEnter);
    EXPECT(Step(h, {9, 0, 0}, false, false) == HolsterEvent::kNone);
    EXPECT(h.Hovering());
    // Just outside the radius but inside the hysteresis: still hovering.
    EXPECT(Step(h, {13.5, 0, 0}, false, false) == HolsterEvent::kNone && h.Hovering());
    // Squeeze in the zone: drawn.
    EXPECT(Step(h, {5, 0, 0}, true, true) == HolsterEvent::kDrawn);
    EXPECT(h.InHand() && !h.Hovering());
    EXPECT(std::fabs(h.LastDistance() - 5) < 1e-9);
    // Held while the grip is down, wherever the hand goes.
    for (int i = 0; i < 30; ++i) Step(h, {50.0 + i, 20, 30}, true, false);
    EXPECT(h.InHand());
    // Let go far away: it flies back by itself.
    EXPECT(Step(h, {80, 20, 30}, false, false) == HolsterEvent::kReturnStarted);
    EXPECT(h.State() == HolsterState::kReturning && h.Blend() == 0.0);
    HolsterEvent last = HolsterEvent::kNone;
    int frames = 0;
    while (h.State() == HolsterState::kReturning && frames < 100) {
        last = Step(h, {80, 20, 30}, false, false);
        ++frames;
    }
    EXPECT(last == HolsterEvent::kReturned);
    EXPECT(frames >= 13 && frames <= 16);  // 0.2 s at 72 fps
    EXPECT(h.State() == HolsterState::kHolstered && h.Blend() == 1.0);
    // Draw again and put it away in the zone: stowed, slides in quickly.
    Step(h, {3, 0, 0}, true, true);
    EXPECT(h.InHand());
    Step(h, {6, 2, 0}, true, false);
    EXPECT(Step(h, {6, 2, 0}, false, false) == HolsterEvent::kStowed);
    EXPECT(h.State() == HolsterState::kHolstered && h.Blend() == 0.0);
    for (int i = 0; i < 10; ++i) Step(h, {6, 2, 0}, false, false);
    EXPECT(h.Blend() == 1.0);
}

static void TestGrabWhileReturning() {
    HolsterDraw h;
    h.Configure(12.0, DrawMode::kHold);
    Step(h, {0, 0, 0}, true, true);
    Step(h, {60, 0, 0}, false, false);
    EXPECT(h.State() == HolsterState::kReturning);
    // Hand back at the holster before it lands: grab it again.
    EXPECT(Step(h, {2, 0, 0}, true, true) == HolsterEvent::kDrawn);
    EXPECT(h.InHand());
}

static void TestFreeze() {
    HolsterDraw h;
    h.Configure(12.0, DrawMode::kHold);
    Step(h, {0, 0, 0}, true, true);
    EXPECT(h.InHand());
    // Grip editor borrows the controller: its grip reads as released, but
    // the gun must stay in the hand.
    for (int i = 0; i < 50; ++i) Step(h, {60, 0, 0}, false, false, true);
    EXPECT(h.InHand());
    // Lost tracking: also no change.
    DrawInput lost;
    lost.hand_valid = false;
    lost.dt = 0.1;
    EXPECT(h.Update(lost) == HolsterEvent::kNone && h.InHand());
    // Holster position unknown (no body, no head): no change either.
    DrawInput nospot;
    nospot.spot_valid = false;
    nospot.hand = FVector{60, 0, 0};
    nospot.dt = 0.1;
    EXPECT(h.Update(nospot) == HolsterEvent::kNone && h.InHand());
    // Frozen while holstered: squeezing at the holster does not draw.
    Step(h, {60, 0, 0}, false, false);
    for (int i = 0; i < 20; ++i) Step(h, {60, 0, 0}, false, false);
    EXPECT(h.State() == HolsterState::kHolstered);
    EXPECT(Step(h, {1, 0, 0}, true, true, true) != HolsterEvent::kDrawn);
    EXPECT(!h.InHand());
}

static void TestToggleMode() {
    HolsterDraw h;
    h.Configure(12.0, DrawMode::kToggle);
    EXPECT(Step(h, {4, 0, 0}, true, true) == HolsterEvent::kDrawn);
    // Letting go of the grip keeps it in the hand.
    Step(h, {40, 0, 0}, false, false);
    EXPECT(h.InHand());
    // Squeezing away from the holster keeps it too.
    Step(h, {40, 0, 0}, true, true);
    Step(h, {40, 0, 0}, false, false);
    EXPECT(h.InHand());
    // Squeeze at the holster: put away.
    EXPECT(Step(h, {5, 0, 0}, true, true) == HolsterEvent::kStowed);
    EXPECT(!h.InHand());
    // The same press must not draw it again on the next frame.
    Step(h, {5, 0, 0}, true, false);
    EXPECT(!h.InHand());
}

static void TestZone() {
    HolsterZone z;
    EXPECT(!z.Update(20, 14, true));
    EXPECT(z.Update(13, 14, true));    // enter: one tick
    EXPECT(!z.Update(12, 14, true));
    EXPECT(!z.Update(16, 14, true) && z.Inside());  // hysteresis
    EXPECT(!z.Update(18, 14, true) && !z.Inside());
    EXPECT(z.Update(10, 14, true));
    EXPECT(!z.Update(10, 14, false) && !z.Inside());  // inactive (hand full): out
}

int main() {
    TestSpots();
    TestPistolFrame();
    TestMagFrame();
    TestHoldMode();
    TestGrabWhileReturning();
    TestFreeze();
    TestToggleMode();
    TestZone();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("holster tests passed\n");
    return 0;
}
