// Native unit tests for body IK math and hand poses (no game, no Windows).
//   g++ -std=c++20 -Isrc tests/test_body.cpp src/vr/body/ik_math.cpp src/vr/body/hand_pose.cpp -o /tmp/test_body && /tmp/test_body
#include <cstdio>

#include "vr/body/hand_pose.h"
#include "vr/body/ik_math.h"

using namespace holevr::vr;
using namespace holevr::vr::ik;

static int failures = 0;
#define EXPECT(c)                                                     \
    do {                                                              \
        if (!(c)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);  \
            ++failures;                                               \
        }                                                             \
    } while (0)

static bool Near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) < eps; }
static bool Near(const FVector& a, const FVector& b, double eps = 1e-4) { return Length(a - b) < eps; }

static void TestArc() {
    FVector a{1, 0, 0}, b{0, 1, 0};
    EXPECT(Near(Arc(a, b).Rotate(a), b));
    EXPECT(Near(Arc(b, a).Rotate(b), a));
    FVector c = Normalize(FVector{0.3, -2, 5});
    EXPECT(Near(Arc(a, c).Rotate(a), c));
    // Opposite directions still land.
    EXPECT(Near(Arc(a, a * -1.0).Rotate(a), a * -1.0));
    // Positive angle about Cross(from, to) moves from toward to.
    FVector r = AxisAngle(Cross(a, b), 0.3).Rotate(a);
    EXPECT(r.Y > 0 && r.X > 0);
}

static void TestAlignFrames() {
    FVector a1{1, 0, 0}, a2{0, 0, 1};
    FVector b1 = Normalize(FVector{0, 1, 1}), b2 = Normalize(FVector{1, 0, 0});
    FQuat q = AlignFrames(a1, a2, b1, b2);
    EXPECT(Near(q.Rotate(a1), b1));
    EXPECT(Near(q.Rotate(a2), b2));
}

static void TestScaleClamp() {
    FQuat q = AxisAngle(FVector{0, 0, 1}, 1.0);
    FVector half = Scale(q, 0.5).Rotate(FVector{1, 0, 0});
    EXPECT(Near(half, FVector{std::cos(0.5), std::sin(0.5), 0}));
    FVector c = ClampAngle(q, 0.25).Rotate(FVector{1, 0, 0});
    EXPECT(Near(c, FVector{std::cos(0.25), std::sin(0.25), 0}));
    // Twist part of a swing+twist rotation.
    FQuat twist = AxisAngle(FVector{1, 0, 0}, 0.7);
    FQuat swing = AxisAngle(FVector{0, 0, 1}, 0.4);
    EXPECT(Near(TwistAngle(twist, FVector{1, 0, 0}), 0.7));
    EXPECT(Near(TwistAngle(AxisAngle(FVector{1, 0, 0}, -2.5), FVector{1, 0, 0}), -2.5));
    EXPECT(Near(TwistAngle(swing, FVector{1, 0, 0}), 0.0));
    EXPECT(std::fabs(TwistAngle(swing * twist, FVector{1, 0, 0}) - 0.7) < 0.1);
    EXPECT(Near(AngleBetween(FVector{1, 0, 0}, FVector{0, 3, 0}), kPi / 2));
}

static void TestTwoBone() {
    FVector root{0, 0, 0};
    // Target in reach: lengths are kept and the elbow bends toward the pole.
    FVector target{40, 0, -10};
    TwoBoneResult r = SolveTwoBone(root, target, 30, 28, FVector{0, 0, -1});
    EXPECT(r.reached);
    EXPECT(Near(Length(r.mid - root), 30));
    EXPECT(Near(Length(r.end - r.mid), 28));
    EXPECT(Near(r.end, target));
    // The elbow sits below the root->target line.
    FVector dir = Normalize(target);
    EXPECT(Dot(Reject(r.mid, dir), FVector{0, 0, -1}) > 0);

    // Out of reach without stretch: straight arm, stops short.
    TwoBoneResult far = SolveTwoBone(root, FVector{100, 0, 0}, 30, 28, FVector{0, 0, -1});
    EXPECT(!far.reached);
    EXPECT(Near(far.end, FVector{58, 0, 0}));
    // With stretch it follows a little further.
    TwoBoneResult s = SolveTwoBone(root, FVector{62, 0, 0}, 30, 28, FVector{0, 0, -1}, 5);
    EXPECT(s.reached);
    EXPECT(Near(s.end, FVector{62, 0, 0}));

    // Pole along the limb falls back to a sane bend.
    TwoBoneResult p = SolveTwoBone(root, FVector{0, 0, -40}, 30, 28, FVector{0, 0, -1});
    EXPECT(Near(p.end, FVector{0, 0, -40}));
    EXPECT(Near(Length(p.mid), 30));
}

static void TestHandFrame() {
    // Left hand lying palm down, fingers along +X, thumb side +Y.
    FVector hand{10, 0, 0};
    FQuat rot = AxisAngle(FVector{0, 0, 1}, 0.4);  // any bone orientation
    FVector idx = hand + FVector{9, 2, 0}, mid = hand + FVector{9.5, 0, 0}, pink = hand + FVector{9, -3, 0};
    HandFrame f = MeasureHand(hand, rot, idx, mid, pink, true);
    EXPECT(f.valid);
    EXPECT(Near(rot.Rotate(f.palm), FVector{0, 0, -1}, 1e-3));
    EXPECT(Near(rot.Rotate(f.across), FVector{0, 1, 0}, 1e-3));
    // Right hand palm down: thumb side -Y.
    FVector ridx = hand + FVector{9, -2, 0}, rpink = hand + FVector{9, 3, 0};
    HandFrame rf = MeasureHand(hand, rot, ridx, mid, rpink, false);
    EXPECT(Near(rot.Rotate(rf.palm), FVector{0, 0, -1}, 1e-3));

    // Gripping a controller held upright and pointing forward: the grip axis
    // is +X, the left palm faces +Y (toward the body's right).
    FQuat target = HandRotation(f, FVector{1, 0, 0}, FVector{0, 1, 0});
    EXPECT(Near(target.Rotate(f.across), FVector{1, 0, 0}, 1e-3));
    EXPECT(Near(target.Rotate(f.palm), FVector{0, 1, 0}, 1e-3));
}

static void TestHandPose() {
    using namespace holevr::vr::hand_pose;
    using holevr::vr::hands::FingerInput;
    using holevr::vr::hands::GrabKind;
    FingerInput none;
    Curl gun = Target(GrabKind::kGunGrip, none);
    EXPECT(gun.finger[kIndex] < 0.1f);  // finger off the trigger
    EXPECT(gun.finger[kMiddle] > 0.7f);
    FingerInput pull;
    pull.trigger_touch = true;
    pull.trigger = 1.f;
    Curl fire = Target(GrabKind::kGunGrip, pull);
    EXPECT(fire.finger[kIndex] > 0.6f);
    FingerInput fist;
    fist.grip = 1.f;
    fist.trigger = 1.f;
    Curl closed = Target(GrabKind::kNone, fist);
    for (int i = kIndex; i <= kPinky; ++i) EXPECT(closed.finger[i] > 0.9f);
    Curl open = Target(GrabKind::kNone, none);
    for (int i = kIndex; i <= kPinky; ++i) EXPECT(open.finger[i] < 0.2f);

    Curl c;
    for (int i = 0; i < 60; ++i) Smooth(&c, closed, 1.0 / 60);
    EXPECT(c.finger[kMiddle] > 0.9f);
}

int main() {
    TestArc();
    TestAlignFrames();
    TestScaleClamp();
    TestTwoBone();
    TestHandFrame();
    TestHandPose();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all body tests passed\n");
    return 0;
}
