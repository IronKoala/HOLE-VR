// Native unit tests for body IK math and hand poses (no game, no Windows).
//   g++ -std=c++20 -Isrc tests/test_body.cpp src/vr/body/ik_math.cpp src/vr/body/hand_pose.cpp -o /tmp/test_body && /tmp/test_body
#include <cstdio>

#include "vr/body/grip_probe.h"
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

static void TestSegmentBox() {
    Box b{FVector{10, 0, 0}, AxisAngle(FVector{0, 0, 1}, 0.5), FVector{1, 2, 3}};
    double t = -1;
    EXPECT(SegmentBox(FVector{0, 0, 0}, FVector{20, 0, 0}, b, &t));
    EXPECT(t > 0.3 && t < 0.5);
    EXPECT(!SegmentBox(FVector{0, 0, 0}, FVector{5, 0, 0}, b, &t));    // stops short
    EXPECT(!SegmentBox(FVector{0, 10, 0}, FVector{20, 10, 0}, b, &t)); // passes beside
    EXPECT(!SegmentBox(FVector{10, 0, 0}, FVector{20, 0, 0}, b, &t));  // starts inside
    // Hit point lies on the surface.
    Box a{FVector{0, 0, 0}, FQuat{}, FVector{1, 1, 1}};
    EXPECT(SegmentBox(FVector{-5, 0.2, 0.3}, FVector{5, 0.2, 0.3}, a, &t));
    EXPECT(Near(-5 + 10 * t, -1.0));
}

// A model pistol in the gun frame (X barrel, Y right, Z up, cm): slide, dust
// cover, trigger guard loop, and a grip slanted 20 degrees.
static bool PistolSolid(double x, double y, double z) {
    auto in = [](double v, double a, double b) { return v >= a && v <= b; };
    if (in(x, 0, 18) && in(z, 11, 14) && in(y, -5.2, -2.4)) return true;    // slide
    if (in(x, 1, 16) && in(z, 9.5, 11) && in(y, -5.1, -2.5)) return true;    // frame
    if (in(x, 12.5, 17) && in(z, 7.0, 9.5) && in(y, -6.0, -1.6)) return true; // weapon light
    if (in(x, 7, 12.1) && in(z, 7.5, 8.3) && in(y, -5.0, -2.6)) return true; // guard bar
    if (in(x, 11.3, 12.1) && in(z, 7.5, 9.5) && in(y, -5.0, -2.6)) return true;
    double xc = 4 + (z - 8) * std::tan(20 * kDegToRad);
    if (in(z, 0, 9.5) && in(x, xc - 2.5, xc + 2.5) && in(y, -5.3, -2.3)) return true;  // grip
    return false;
}

static grip::Grid ScanModel() {
    grip::Grid g;
    g.Init(-12, 22, -8, 18, 0.5);
    for (int iz = 0; iz < g.nz; ++iz)
        for (int ix = 0; ix < g.nx; ++ix) {
            double x = g.X(ix), z = g.Z(iz);
            size_t k = static_cast<size_t>(iz) * g.nx + ix;
            for (double y = 15; y >= -15; y -= 0.05)
                if (PistolSolid(x, y, z)) { g.yr[k] = y; break; }
            for (double y = -15; y <= 15; y += 0.05)
                if (PistolSolid(x, y, z)) { g.yl[k] = y; break; }
        }
    return g;
}

static void TestGripProbe() {
    grip::Grid g = ScanModel();
    grip::Shape s = grip::Analyze(g);
    EXPECT(s.ok);
    if (!s.ok) { std::printf("  why: %s\n", s.why.c_str()); return; }
    EXPECT(std::fabs(s.z_bottom - 0) < 0.6);
    EXPECT(std::fabs(s.z_guard - 7.5) < 0.6);
    EXPECT(std::fabs(s.slant_deg - 20) < 3);
    EXPECT(std::fabs(s.y_right + 2.3) < 0.2);
    EXPECT(std::fabs(s.y_left + 5.3) < 0.2);
    double xc = 4 + (7.0 - 8) * std::tan(20 * kDegToRad);
    EXPECT(std::fabs(s.front - (xc + 2.5)) < 0.6);
    EXPECT(std::fabs(s.back - (xc - 2.5)) < 0.6);
    EXPECT(std::fabs(s.thumb_y + 5.1) < 0.3);  // frame side, above the guard
    // Inside test.
    double d;
    EXPECT(g.Inside(FVector{4, -3.8, 5}, &d) && d > 1.2);
    EXPECT(!g.Inside(FVector{4, -1.0, 5}));
    EXPECT(!g.Inside(FVector{9, -3.8, 9}));  // inside the trigger guard's hole

    // A right hand (bone space: X wrist->knuckles, Y pinky->index, Z out of the palm).
    grip::HandGeo h;
    h.across = FVector{0, 1, 0};
    h.palm = FVector{0, 0, 1};  // a right hand: Cross(across, palm) = wrist->knuckles
    h.mcp[0] = FVector{2.5, 2.5, 1.5};
    h.mcp[1] = FVector{9.0, 2.2, 0};
    h.mcp[2] = FVector{9.3, 0.2, 0};
    h.mcp[3] = FVector{8.9, -1.7, 0};
    h.mcp[4] = FVector{8.2, -3.4, 0};
    grip::Params p;
    grip::Placement pl = grip::PlaceRightHand(g, s, h, p);
    EXPECT(Near(pl.rot.Rotate(h.palm), FVector{0, -1, 0}, 1e-3));
    EXPECT(Near(pl.rot.Rotate(h.across), s.up, 1e-3));
    FVector mid = pl.pos + pl.rot.Rotate(h.mcp[2]);
    EXPECT(std::fabs(mid.Z - (s.z_guard - p.finger_radius)) < 0.05);
    EXPECT(mid.Y > s.y_right);                      // knuckles outside the right side
    for (const FVector& lp : grip::PalmPoints(h, p.palm_skin))
        EXPECT(!g.Inside(pl.pos + pl.rot.Rotate(lp)));
    FVector wrist = pl.pos;
    EXPECT(wrist.X < s.back);                       // wrist behind the grip, in line with the gun
    EXPECT(pl.thumb.Y < s.y_left);                  // thumb on the gun's LEFT side
    EXPECT(pl.thumb.Z > s.z_guard);
    // A finger through the grip is reported, one beside it is not.
    EXPECT(grip::Penetration(g, {FVector{4, 0, 5}, FVector{4, -8, 5}}, 0.8) > 1.0);
    EXPECT(grip::Penetration(g, {FVector{4, -0.5, 5}, FVector{8, -0.5, 5}}, 0.8) == 0.0);
}

int main() {
    TestGripProbe();
    TestSegmentBox();
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
