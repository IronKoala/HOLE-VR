// Native unit tests for the human finger joint ranges (vr/body/joint_limits).
//   g++ -std=c++20 -Isrc tests/test_joint_limits.cpp src/vr/body/ik_math.cpp src/vr/body/skeleton.cpp
//       src/vr/body/finger_solve.cpp src/vr/body/joint_limits.cpp -o /tmp/test_joint_limits && /tmp/test_joint_limits
#include <cstdio>
#include <random>

#include "vr/body/finger_solve.h"
#include "vr/body/joint_limits.h"
#include "vr/body/skeleton.h"

using namespace holevr::vr;
using namespace holevr::vr::ik;
namespace jl = holevr::vr::joint_limits;
using skel::Xf;

static int failures = 0;
#define EXPECT(c)                                                     \
    do {                                                              \
        if (!(c)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);  \
            ++failures;                                               \
        }                                                             \
    } while (0)

static bool Near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) < eps; }
static bool SameRot(const FQuat& a, const FQuat& b, double eps = 1e-6) {
    double d = std::fabs(a.X * b.X + a.Y * b.Y + a.Z * b.Z + a.W * b.W);
    return d > 1.0 - eps;
}
constexpr double D = kDegToRad;

// A finger whose reference pose is already curled (bends b[3], degrees) and
// whose bone frames are rolled about the bone by odd angles, like a real
// skeleton: the limits must not depend on the bone axes. Bones run along
// local -X. Right hand: palm faces +Y, fingers along -X. Left hand: the
// mirror (palm -Y... built with palm +Y but flex measured the same way).
struct Finger {
    fingers::Chain c;
    jl::Axes ax[3];
    jl::Limits lim[3];
    double ref[3];
};
static Finger MakeFinger(int finger, const double bends[3], const double roll[3], const FVector& palm = {0, 1, 0}) {
    const double seg[3] = {4.6, 3.0, 2.4};
    const FVector flex_axis = Normalize(Cross(FVector{-1, 0, 0}, palm));  // curls toward the palm
    Xf j[3];
    FVector pos{-12, 1.5, 0};
    // The knuckle: the wrist->knuckle line is the metacarpal, so the first
    // bend is measured from it.
    FVector dir = Normalize(pos);
    for (int n = 0; n < 3; ++n) {
        dir = AxisAngle(flex_axis, bends[n] * D).Rotate(dir);
        FQuat r = Normalized(Arc(FVector{-1, 0, 0}, dir) * AxisAngle(FVector{-1, 0, 0}, roll[n] * D));
        j[n].translation = pos;
        j[n].rotation = r;
        pos = pos + dir * seg[n];
    }
    Finger f;
    f.c = fingers::Build(j, palm, FVector{-10, 2, -4.6}, finger == 0);
    jl::ReferenceBends(f.c, finger == 0, f.ref);
    for (int n = 0; n < 3; ++n) {
        f.ax[n] = jl::JointAxes(f.c, n);
        f.lim[n] = jl::Relative(jl::Human(finger, n), f.ref[n]);
    }
    return f;
}

static const double kBends[3] = {12, 18, 9};
static const double kRolls[3] = {23, -41, 67};

static void TestAxes() {
    Finger f = MakeFinger(2, kBends, kRolls);
    for (int n = 0; n < 3; ++n) {
        // Orthonormal, and + curl moves the bone toward the palm.
        EXPECT(Near(Length(f.ax[n].bone), 1) && Near(Length(f.ax[n].flex), 1) && Near(Length(f.ax[n].toward), 1));
        EXPECT(Near(Dot(f.ax[n].bone, f.ax[n].flex), 0) && Near(Dot(f.ax[n].bone, f.ax[n].toward), 0));
        fingers::Angles a;
        a.extra[n] = jl::Compose(f.ax[n], jl::Turn{0.4, 0, 0});
        FVector tip0, tip1;
        Xf js[3];
        fingers::Fk(f.c, fingers::Angles{}, js, &tip0);
        fingers::Fk(f.c, a, js, &tip1);
        EXPECT(tip1.Y > tip0.Y + 0.3);
    }
    // The reference bends are measured back from the skeleton. The tip
    // joint has no bone past it (the chain's fingertip carries on along the
    // middle bone), so its bend reads 0.
    EXPECT(Near(f.ref[0] * kRadToDeg, kBends[0], 0.2));
    EXPECT(Near(f.ref[1] * kRadToDeg, kBends[1], 0.2));
    EXPECT(Near(f.ref[2], 0, 1e-6));
    // The thumb's base is ranged about its reference pose.
    Finger t = MakeFinger(0, kBends, kRolls);
    EXPECT(Near(t.ref[0], 0));
}

static void TestRoundTrip() {
    Finger f = MakeFinger(1, kBends, kRolls);
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> u(-1, 1);
    for (int i = 0; i < 200; ++i) {
        FQuat q = AxisAngle(FVector{u(rng), u(rng), u(rng)}, u(rng) * 2.5);
        for (int n = 0; n < 3; ++n) {
            jl::Turn t = jl::Decompose(f.ax[n], q);
            EXPECT(SameRot(jl::Compose(f.ax[n], t), q));
        }
    }
}

// The owner's rule: the middle and tip joints never bend sideways or roll.
static void TestHinges() {
    Finger f = MakeFinger(2, kBends, kRolls);
    for (int n : {1, 2}) {
        const jl::Axes& a = f.ax[n];
        FQuat side = jl::Compose(a, jl::Turn{50 * D, 20 * D, 15 * D});
        bool hit = true;
        FQuat q = jl::Clamp(a, f.lim[n], side, &hit);
        jl::Turn t = jl::Decompose(a, q);
        EXPECT(Near(t.side, 0, 1e-6) && Near(t.twist, 0, 1e-6));
        EXPECT(Near(t.flex, 50 * D, 1e-6));  // the curl is kept
        EXPECT(!hit);                        // sideways on a hinge is not a range end (no buzz)
        // A plain curl in range is untouched.
        FQuat curl = AxisAngle(a.flex, 40 * D);
        EXPECT(jl::Within(a, f.lim[n], curl));
        EXPECT(SameRot(jl::Clamp(a, f.lim[n], curl), curl, 1e-12));
    }
    // Middle joint: 105 deg anatomical at most, so 105 - 18 from this reference.
    FQuat over = AxisAngle(f.ax[1].flex, 120 * D);
    bool hit = false;
    jl::Turn t = jl::Decompose(f.ax[1], jl::Clamp(f.ax[1], f.lim[1], over, &hit));
    EXPECT(hit && Near(t.flex * kRadToDeg, 105 - 18, 0.3));
    // Bent backward: 5 deg past straight at most.
    FQuat back = AxisAngle(f.ax[1].flex, -60 * D);
    t = jl::Decompose(f.ax[1], jl::Clamp(f.ax[1], f.lim[1], back, &hit));
    EXPECT(hit && Near(t.flex * kRadToDeg, -5 - 18, 0.3));
    // Turned far past the end, round past half a turn: stays at the end it
    // ran into instead of snapping to the other.
    FQuat wrap = AxisAngle(f.ax[1].flex, 200 * D);
    t = jl::Decompose(f.ax[1], jl::Clamp(f.ax[1], f.lim[1], wrap));
    EXPECT(Near(t.flex * kRadToDeg, 105 - 18, 0.3));
}

static void TestKnuckle() {
    Finger f = MakeFinger(1, kBends, kRolls);  // index: 22 deg spread
    const jl::Axes& a = f.ax[0];
    // Some spread and a little roll are fine.
    FQuat ok = jl::Compose(a, jl::Turn{20 * D, 15 * D, 6 * D});
    EXPECT(jl::Within(a, f.lim[0], ok));
    // Too far sideways: held at the end, with a buzz.
    bool hit = false;
    jl::Turn t = jl::Decompose(a, jl::Clamp(a, f.lim[0], jl::Compose(a, jl::Turn{0, 45 * D, 30 * D}), &hit));
    EXPECT(hit);
    EXPECT(t.side * kRadToDeg < 22.01 && t.side * kRadToDeg > 18);
    EXPECT(Near(t.twist * kRadToDeg, 10, 0.01));
    // Curled all the way, the knuckle spreads much less.
    jl::Turn curled = jl::Decompose(a, jl::Clamp(a, f.lim[0], jl::Compose(a, jl::Turn{78 * D, 20 * D, 0})));
    EXPECT(curled.side * kRadToDeg < 9.0);
}

// Left hand: + curl still goes toward the palm, and the ranges hold.
static void TestLeftHand() {
    Finger f = MakeFinger(3, kBends, kRolls, FVector{0, -1, 0});
    fingers::Angles a;
    a.extra[1] = jl::Compose(f.ax[1], jl::Turn{0.5, 0, 0});
    FVector tip0, tip1;
    Xf js[3];
    fingers::Fk(f.c, fingers::Angles{}, js, &tip0);
    fingers::Fk(f.c, a, js, &tip1);
    EXPECT(tip1.Y < tip0.Y - 0.3);
    EXPECT(Near(f.ref[1] * kRadToDeg, kBends[1], 0.2));
}

static FVector Tip(const fingers::Chain& c, const FQuat q[3]) {
    fingers::Angles a;
    for (int j = 0; j < 3; ++j) a.extra[j] = q[j];
    Xf js[3];
    FVector tip;
    fingers::Fk(c, a, js, &tip);
    return tip;
}

// Saved poses: made human, as close to how they were posed as it can.
static void TestFit() {
    Finger f = MakeFinger(2, kBends, kRolls);
    // A pose the editor allowed before: the middle joint bent sideways.
    FQuat q[3] = {jl::Compose(f.ax[0], jl::Turn{40 * D, 0, 0}), jl::Compose(f.ax[1], jl::Turn{50 * D, 14 * D, 8 * D}),
                  jl::Compose(f.ax[2], jl::Turn{30 * D, 0, 0})};
    const FVector tip0 = Tip(f.c, q);
    // Plain clamping (each joint alone) for comparison.
    FQuat plain[3];
    for (int j = 0; j < 3; ++j) plain[j] = jl::Clamp(f.ax[j], f.lim[j], q[j]);
    const double plain_moved = Length(Tip(f.c, plain) - tip0);
    jl::FitReport rep = jl::Fit(f.c, f.ax, f.lim, q);
    EXPECT(rep.changed);
    for (int j = 0; j < 3; ++j) EXPECT(jl::Within(f.ax[j], f.lim[j], q[j], 1e-6));
    const double moved = Length(Tip(f.c, q) - tip0);
    EXPECT(moved < plain_moved * 0.6);
    EXPECT(rep.moved_cm >= moved - 1e-9 && rep.moved_cm < 1.0);
    EXPECT(Near(rep.side_deg[1], 14, 0.01) && Near(rep.twist_deg[1], 8, 0.01));
    std::printf("fit: fingertip moved %.2f cm (each joint clamped alone: %.2f cm)\n", moved, plain_moved);
    // Fitting again changes nothing.
    FQuat again[3] = {q[0], q[1], q[2]};
    EXPECT(!jl::Fit(f.c, f.ax, f.lim, again).changed);
    for (int j = 0; j < 3; ++j) EXPECT(SameRot(again[j], q[j], 1e-12));
    // A human pose is left exactly as it was.
    FQuat good[3] = {jl::Compose(f.ax[0], jl::Turn{30 * D, 8 * D, 3 * D}), AxisAngle(f.ax[1].flex, 60 * D),
                     AxisAngle(f.ax[2].flex, 40 * D)};
    FQuat copy[3] = {good[0], good[1], good[2]};
    EXPECT(!jl::Fit(f.c, f.ax, f.lim, copy).changed);
    for (int j = 0; j < 3; ++j) EXPECT(copy[j].X == good[j].X && copy[j].W == good[j].W);
    // The solver's own poses (curl, plus spread at the knuckle) are human.
    fingers::Angles s;
    s.flex[0] = 50 * D;
    s.flex[1] = 60 * D;
    s.flex[2] = 42 * D;
    s.spread = 8 * D;
    FQuat solved[3];
    for (int j = 0; j < 3; ++j)
        solved[j] = Normalized(f.c.ref[j].rotation.Inverse() * fingers::LocalRotation(f.c, j, s));
    for (int j = 0; j < 3; ++j) EXPECT(jl::Within(f.ax[j], f.lim[j], solved[j]));
}

// The thumb: a big side bend at its middle joint goes to its base instead.
static void TestThumb() {
    Finger t = MakeFinger(0, kBends, kRolls);
    FQuat q[3] = {jl::Compose(t.ax[0], jl::Turn{10 * D, 0, 0}), jl::Compose(t.ax[1], jl::Turn{20 * D, 35 * D, 0}),
                  jl::Compose(t.ax[2], jl::Turn{30 * D, 0, 0})};
    const FVector tip0 = Tip(t.c, q);
    jl::FitReport rep = jl::Fit(t.c, t.ax, t.lim, q);
    EXPECT(rep.changed);
    for (int j = 0; j < 3; ++j) EXPECT(jl::Within(t.ax[j], t.lim[j], q[j], 1e-6));
    jl::Turn base = jl::Decompose(t.ax[0], q[0]);
    EXPECT(std::fabs(base.side) > 10 * D || std::fabs(base.twist) > 5 * D);
    std::printf("thumb fit: fingertip moved %.2f cm\n", Length(Tip(t.c, q) - tip0));
    EXPECT(Length(Tip(t.c, q) - tip0) < 1.0);
}

int main() {
    TestAxes();
    TestRoundTrip();
    TestHinges();
    TestKnuckle();
    TestLeftHand();
    TestFit();
    TestThumb();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all joint limit tests passed\n");
    return 0;
}
