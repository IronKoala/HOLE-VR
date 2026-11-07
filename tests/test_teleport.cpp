// Native unit tests for the teleport logic (no game, no Windows).
//   g++ -std=c++20 -Isrc tests/test_teleport.cpp src/vr/teleport_math.cpp -o /tmp/test_teleport && /tmp/test_teleport
#include <cmath>
#include <cstdio>
#include <string>

#include "vr/teleport_math.h"

using namespace holevr::vr;
using namespace holevr::vr::teleport;

static int failures = 0;
#define EXPECT(c)                                                     \
    do {                                                              \
        if (!(c)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);  \
            ++failures;                                               \
        }                                                             \
    } while (0)

static void TestArc() {
    auto pts = ArcPoints({0, 0, 100}, {1, 0, 1}, 850, 980, 0.06, 30);
    EXPECT(pts.size() == 30);
    EXPECT(std::fabs(pts[0].Z - 100) < 1e-9 && pts[0].X == 0);
    // Goes forward the whole way, rises then falls.
    bool forward = true;
    for (size_t i = 1; i < pts.size(); ++i) forward = forward && pts[i].X > pts[i - 1].X;
    EXPECT(forward);
    EXPECT(pts[5].Z > pts[0].Z);
    EXPECT(pts.back().Z < pts[0].Z);
    // Range on flat ground from 1 m up at 45 deg is several metres, not tens.
    double land_x = 0;
    for (size_t i = 1; i < pts.size(); ++i)
        if (pts[i].Z <= 0 && pts[i - 1].Z > 0) land_x = pts[i].X;
    EXPECT(land_x > 500 && land_x < 1200);
    // A zero direction does not make NaNs.
    auto z = ArcPoints({0, 0, 0}, {0, 0, 0}, 850, 980, 0.06, 3);
    EXPECT(std::isfinite(z[2].X) && std::isfinite(z[2].Z));
    EXPECT(ArcPoints({}, {1, 0, 0}, 1, 1, 1, 0).empty());
}

static void TestGesture() {
    StickGesture g;
    EXPECT(g.Update(0, 0) == Gesture::kIdle);
    EXPECT(g.Update(0.8, 0.1) == Gesture::kIdle);  // sideways does not aim
    EXPECT(g.Update(0, -0.9) == Gesture::kIdle);   // back does not aim
    EXPECT(g.Update(0, 0.9) == Gesture::kAiming);
    EXPECT(g.Aiming());
    EXPECT(g.Update(0.5, 0.5) == Gesture::kAiming);  // steering while aiming is fine
    EXPECT(g.Update(0.1, 0.1) == Gesture::kGo);
    EXPECT(!g.Aiming());
    EXPECT(g.Update(0, 0) == Gesture::kIdle);
    // Cancel by pulling back, then nothing until the stick is centred.
    EXPECT(g.Update(0, 0.9) == Gesture::kAiming);
    EXPECT(g.Update(0, -0.8) == Gesture::kCancel);
    EXPECT(g.Update(0, 0.9) == Gesture::kIdle);
    EXPECT(g.Update(0.1, 0) == Gesture::kIdle);
    EXPECT(g.Update(0, 0.9) == Gesture::kAiming);
}

static void TestLanding() {
    FVector feet{0, 0, 0};
    EXPECT(CheckLanding(feet, {300, 0, 0}, 1.0, 150).ok);
    EXPECT(!CheckLanding(feet, {300, 0, 0}, 0.3, 150).ok);              // a wall
    EXPECT(std::string(CheckLanding(feet, {300, 0, 0}, 0.3, 150).why) == "too steep");
    EXPECT(!CheckLanding(feet, {300, 0, 200}, 1.0, 150).ok);            // a high ledge
    EXPECT(CheckLanding(feet, {300, 0, 100}, 1.0, 150).ok);             // a step up
    EXPECT(CheckLanding(feet, {300, 0, -400}, 1.0, 150).ok);            // down is fine
    EXPECT(!CheckLanding(feet, {10, 10, 0}, 1.0, 150).ok);              // at your feet
    EXPECT(CheckLanding(feet, {10, 10, -200}, 1.0, 150).ok);            // straight down a drop
    EXPECT(!CheckLanding(feet, {300, 0, 0}, std::nan(""), 150).ok);
    FVector d = CapsuleDestination({500, 200, 10}, {20, -5, 0}, 88);
    EXPECT(d.X == 480 && d.Y == 205 && d.Z == 100);
}

int main() {
    TestArc();
    TestGesture();
    TestLanding();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("teleport: all tests passed\n");
    return 0;
}
