// Native unit tests for touch interact maths (no game, no Windows).
//   g++ -std=c++20 -Isrc tests/test_touch.cpp src/vr/interact/touch_math.cpp -o /tmp/test_touch && /tmp/test_touch
#include <cmath>
#include <cstdio>

#include "vr/interact/touch_math.h"

using namespace holevr::vr;
using namespace holevr::vr::touch;

static int failures = 0;
#define EXPECT(c)                                                     \
    do {                                                              \
        if (!(c)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);  \
            ++failures;                                               \
        }                                                             \
    } while (0)

static bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

static void TestClassify() {
    const std::vector<std::string> none;
    EXPECT(Classify({"BP_HomeDoor_B_C", "BP_DoorBase_C", "Actor"}, none, none) == Kind::kDoor);
    EXPECT(Classify({"BP_ItemBoxBase_C", "Actor"}, none, none) == Kind::kContainer);
    EXPECT(Classify({"LCBP_Fridge_C", "Actor"}, none, none) == Kind::kContainer);
    EXPECT(Classify({"TItem0005_CD_C", "BP_ItemBase_C", "Actor"}, none, none) == Kind::kLoot);
    EXPECT(Classify({"LCBP_Hole_C", "Actor"}, none, none) == Kind::kOther);
    // The drink machine and its parts are the drinks code's.
    EXPECT(Classify({"LCBP_SkillUpgradeSwitchNew_C", "Actor"}, none, none) == Kind::kSkip);
    EXPECT(Classify({"LCBP_SkillUpgradeNew_C", "Actor"}, none, none) == Kind::kSkip);
    // ini lists.
    EXPECT(Classify({"BP_Locker_C", "Actor"}, {"Locker"}, none) == Kind::kContainer);
    EXPECT(Classify({"TItem0005_CD_C", "BP_ItemBase_C"}, none, {"TItem0005"}) == Kind::kSkip);
    auto l = SplitList(" a, b ,,c ");
    EXPECT(l.size() == 3 && l[0] == "a" && l[1] == "b" && l[2] == "c");
    EXPECT(SplitList("").empty());
}

static void TestBox() {
    Box b{{-1, -2, -3}, {1, 2, 3}};
    EXPECT(Near(DistToBox({0, 0, 0}, b), 0));
    EXPECT(Near(DistToBox({4, 0, 0}, b), 3));
    EXPECT(Near(DistToBox({4, 6, 3}, b), 5));
    EXPECT(Near(BoxVolume(b), 2 * 4 * 6));
    Box g = Grow(b, 1);
    EXPECT(Near(g.lo.X, -2) && Near(g.hi.Z, 4));
    Box s = ScaledBox({-1, 0, 2}, {3, 1, 4}, {2, -1, 0.5});
    EXPECT(Near(s.lo.X, -2) && Near(s.hi.X, 6));
    EXPECT(Near(s.lo.Y, -1) && Near(s.hi.Y, 0));
    EXPECT(Near(s.lo.Z, 1) && Near(s.hi.Z, 2));
    // A frame turned 90 deg about Z: world +Y is the frame's +X.
    const double h = std::sqrt(0.5);
    FQuat rot{0, 0, h, h};
    FVector l = ToFrame(rot, {10, 0, 0}, {10, 5, 0});
    EXPECT(Near(l.X, 5, 1e-9) && Near(l.Y, 0, 1e-9));
}

static void TestDoor() {
    // A 90 x 4 x 210 cm leaf: X across the door from the hinge at X=0,
    // Y through it, Z up.
    Box leaf{{0, -2, 0}, {90, 2, 210}};
    HandleZoneSettings s;
    Box z[2];
    int n = DoorHandleZones(leaf, {0, 0, 100}, true, {0, 0, 1}, s, z);
    EXPECT(n == 1);
    // Handle side: the far edge (X = 90).
    EXPECT(Near(z[0].lo.X, 68) && Near(z[0].hi.X, 94));
    EXPECT(Near(z[0].lo.Z, 80) && Near(z[0].hi.Z, 120));
    EXPECT(Near(z[0].lo.Y, -14) && Near(z[0].hi.Y, 14));
    // A hand resting on the handle (8 cm in from the edge, 6 cm off the face).
    EXPECT(Near(DistToBox({82, 8, 98}, z[0]), 0));
    // The hinge side and the top of the door are not the handle.
    EXPECT(DistToBox({5, 4, 100}, z[0]) > 50);
    EXPECT(DistToBox({80, 4, 190}, z[0]) > 60);
    // Hinge on the other side: the zone moves to X = 0.
    n = DoorHandleZones(leaf, {90, 0, 100}, true, {0, 0, 1}, s, z);
    EXPECT(n == 1 && Near(z[0].lo.X, -4) && Near(z[0].hi.X, 22));
    // Upside-down leaf frame (world up is the leaf's -Z): height from Z = 210.
    n = DoorHandleZones(leaf, {0, 0, 0}, true, {0, 0, -1}, s, z);
    EXPECT(n == 1 && Near(z[0].lo.Z, 90) && Near(z[0].hi.Z, 130));
    // Leaf lying along Y (thickness in X), hinge unknown: both edges.
    Box leaf2{{-2, -45, 0}, {2, 45, 205}};
    n = DoorHandleZones(leaf2, {}, false, {0, 0, 1}, s, z);
    EXPECT(n == 2);
    EXPECT(Near(z[0].lo.Y, -49) && Near(z[0].hi.Y, -23));
    EXPECT(Near(z[1].lo.Y, 23) && Near(z[1].hi.Y, 49));
    EXPECT(Near(z[1].lo.X, -14) && Near(z[1].hi.X, 14));
    // Not a door leaf: a box.
    EXPECT(DoorHandleZones(Box{{0, 0, 0}, {50, 50, 50}}, {}, false, {0, 0, 1}, s, z) == 0);
}

static void TestTouch() {
    TouchState s;
    const double touch = 1.5, leave = 4.5, hold = 0.1;
    // Comes from outside, rests 0.1 s: fires once.
    EXPECT(!StepTouch(s, 20, 0.00, touch, leave, hold));
    EXPECT(!StepTouch(s, 1.0, 0.01, touch, leave, hold));
    EXPECT(!StepTouch(s, 0.0, 0.05, touch, leave, hold));
    EXPECT(StepTouch(s, 0.0, 0.12, touch, leave, hold));
    EXPECT(!StepTouch(s, 0.0, 0.50, touch, leave, hold));
    // Inside the leave band it stays used; past it, it re-arms.
    EXPECT(!StepTouch(s, 3.0, 0.60, touch, leave, hold));
    EXPECT(!StepTouch(s, 0.5, 0.80, touch, leave, hold));
    EXPECT(!StepTouch(s, 9.0, 0.90, touch, leave, hold));
    EXPECT(!StepTouch(s, 0.5, 1.00, touch, leave, hold));
    EXPECT(StepTouch(s, 0.5, 1.11, touch, leave, hold));
    // Brushing past (shorter than the hold) does nothing.
    TouchState b;
    EXPECT(!StepTouch(b, 30, 0.0, touch, leave, hold));
    EXPECT(!StepTouch(b, 0.5, 0.01, touch, leave, hold));
    EXPECT(!StepTouch(b, 8.0, 0.05, touch, leave, hold));
    EXPECT(!StepTouch(b, 0.5, 0.06, touch, leave, hold));
    EXPECT(!StepTouch(b, 0.5, 0.15, touch, leave, hold));
    EXPECT(StepTouch(b, 0.5, 0.17, touch, leave, hold));
    // First seen with the hand already on it: waits until the hand leaves.
    TouchState p;
    EXPECT(!StepTouch(p, 0.0, 0.0, touch, leave, hold));
    EXPECT(!StepTouch(p, 0.0, 1.0, touch, leave, hold));
    EXPECT(!StepTouch(p, 10.0, 1.1, touch, leave, hold));
    EXPECT(!StepTouch(p, 0.0, 1.2, touch, leave, hold));
    EXPECT(StepTouch(p, 0.0, 1.31, touch, leave, hold));
    // Disarmed by a touch elsewhere: needs to leave first.
    TouchState d;
    EXPECT(!StepTouch(d, 20, 0.0, touch, leave, hold));
    EXPECT(!StepTouch(d, 0.0, 0.1, touch, leave, hold));
    Disarm(d);
    EXPECT(!StepTouch(d, 0.0, 0.5, touch, leave, hold));
    EXPECT(!StepTouch(d, 6.0, 0.6, touch, leave, hold));
    EXPECT(!StepTouch(d, 0.0, 0.7, touch, leave, hold));
    EXPECT(StepTouch(d, 0.0, 0.81, touch, leave, hold));
}

// Outside review, finding 6: losing tracking (or a menu, or a busy hand) with
// the hand still on a door must not count as leaving it.
static void TestUnknown() {
    const double touch = 1.5, leave = 5.5, hold = 0.2;
    const double tick = 1.0 / 72.0;
    // Rest on the handle, fire once, keep resting past the 1 s cooldown, lose
    // tracking for one tick, come back at the same place: no second fire.
    TouchState s;
    int fires = 0;
    double t = 0.0;
    fires += StepTouch(s, 20, t, touch, leave, hold);
    for (t = tick; t < 3.0; t += tick) fires += StepTouch(s, 0.0, t, touch, leave, hold);
    EXPECT(fires == 1);
    StepUnknown(s);
    t += tick;
    for (const double end = t + 2.0; t < end; t += tick) fires += StepTouch(s, 0.0, t, touch, leave, hold);
    EXPECT(fires == 1);
    // Longer gaps (menu open for seconds) do not either.
    for (int i = 0; i < 300; ++i) StepUnknown(s);
    for (const double end = t + 2.0; t < end; t += tick) fires += StepTouch(s, 0.0, t, touch, leave, hold);
    EXPECT(fires == 1);
    // A real leave and return still works.
    fires += StepTouch(s, 10.0, t, touch, leave, hold);
    t += tick;
    for (const double end = t + 0.5; t < end; t += tick) fires += StepTouch(s, 0.0, t, touch, leave, hold);
    EXPECT(fires == 2);

    // A rest in progress when tracking drops starts again: no fire on the
    // first tick back, a full rest is needed.
    TouchState r;
    EXPECT(!StepTouch(r, 20, 0.00, touch, leave, hold));
    EXPECT(!StepTouch(r, 0.0, 0.10, touch, leave, hold));
    StepUnknown(r);
    EXPECT(!StepTouch(r, 0.0, 0.40, touch, leave, hold));
    EXPECT(!StepTouch(r, 0.0, 0.55, touch, leave, hold));
    EXPECT(StepTouch(r, 0.0, 0.61, touch, leave, hold));

    // Unknown before the thing was ever seen: the first real sample decides,
    // so a hand already on it when it is first seen still waits for a leave.
    TouchState f;
    StepUnknown(f);
    EXPECT(!StepTouch(f, 0.0, 0.0, touch, leave, hold));
    EXPECT(!StepTouch(f, 0.0, 1.0, touch, leave, hold));
    EXPECT(!StepTouch(f, 10.0, 1.1, touch, leave, hold));
    EXPECT(!StepTouch(f, 0.0, 1.2, touch, leave, hold));
    EXPECT(StepTouch(f, 0.0, 1.41, touch, leave, hold));
}

int main() {
    TestClassify();
    TestBox();
    TestDoor();
    TestTouch();
    TestUnknown();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_touch: all passed\n");
    return 0;
}
