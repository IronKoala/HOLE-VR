// Native unit tests for the support hand's grip points (no game, no Windows).
//   g++ -std=c++20 -Isrc tests/test_grip_points.cpp src/vr/interact/grip_points.cpp -o /tmp/test_grip_points && /tmp/test_grip_points
#include <cmath>
#include <cstdio>

#include "vr/interact/grip_points.h"

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

static bool Near(const FVector& a, const FVector& b, double tol = 1e-9) {
    return std::fabs(a.X - b.X) < tol && std::fabs(a.Y - b.Y) < tol && std::fabs(a.Z - b.Z) < tol;
}

// An AR-15-like rifle: handguard 12..42 cm ahead of the grip, 5 cm wide,
// its middle 10 cm above the grip; rails end at 41; the lower receiver ends
// at 8 (behind the handguard); a light from 30 to 36.
static GunParts Ar() {
    GunParts p;
    p.have_handguard = true;
    p.hg_lo = FVector{12, -2.5, 7};
    p.hg_hi = FVector{42, 2.5, 13};
    p.have_rails = true;
    p.rail_front = 41;
    p.have_magwell = true;
    p.magwell_front = 8;
    p.have_light = true;
    p.light_rear = 30;
    return p;
}

static void TestThreePoints() {
    const GripPoints g = MakeGripPoints(Ar(), 57);
    EXPECT(g.ok);
    EXPECT(g.have[kPointMagwell] && g.have[kPointLight] && g.have[kPointRailEnd]);
    EXPECT(g.light_found);
    // Half a hand (4.5 cm) in from each edge; on the left side (2.5 cm
    // outside a 2.5 cm half width), level with the handguard's middle.
    EXPECT(Near(g.p[kPointMagwell], FVector{16.5, -5, 10}));
    EXPECT(Near(g.p[kPointLight], FVector{25.5, -5, 10}));
    EXPECT(Near(g.p[kPointRailEnd], FVector{36.5, -5, 10}));
}

static void TestNoLight() {
    GunParts p = Ar();
    p.have_light = false;
    GripPoints g = MakeGripPoints(p, 57);
    EXPECT(g.have[kPointLight] && !g.light_found);
    EXPECT(std::fabs(g.p[kPointLight].X - 26.5) < 1e-9);   // halfway
    // A light right at the front leaves no room behind it: halfway again.
    p.have_light = true;
    p.light_rear = 39;
    g = MakeGripPoints(p, 57);
    EXPECT(g.have[kPointLight] && !g.light_found);
    EXPECT(std::fabs(g.p[kPointLight].X - 26.5) < 1e-9);
    // ...and one too far back (on the receiver) as well.
    p.light_rear = 14;
    g = MakeGripPoints(p, 57);
    EXPECT(!g.light_found);
}

static void TestEdges() {
    // The magwell reaches further forward than the handguard's back end.
    GunParts p = Ar();
    p.magwell_front = 14;
    GripPoints g = MakeGripPoints(p, 57);
    EXPECT(std::fabs(g.p[kPointMagwell].X - 18.5) < 1e-9);
    // No rails found: the handguard's own front end.
    p = Ar();
    p.have_rails = false;
    g = MakeGripPoints(p, 57);
    EXPECT(std::fabs(g.p[kPointRailEnd].X - 37.5) < 1e-9);
    // Rails past the muzzle (a bad measurement): kept 2 cm behind it.
    p = Ar();
    p.rail_front = 60;
    p.hg_hi.X = 60;
    g = MakeGripPoints(p, 50);
    EXPECT(std::fabs(g.p[kPointRailEnd].X - 43.5) < 1e-9);
    // A short handguard: one point, in its middle; no middle point.
    p = Ar();
    p.hg_hi.X = 20;
    p.rail_front = 20;
    p.have_light = false;
    g = MakeGripPoints(p, 57);
    EXPECT(g.ok && g.have[kPointMagwell] && !g.have[kPointLight] && !g.have[kPointRailEnd]);
    EXPECT(std::fabs(g.p[kPointMagwell].X - 16.0) < 1e-9);
    // No handguard found: no points (the hand holds where it grabs).
    p = Ar();
    p.have_handguard = false;
    g = MakeGripPoints(p, 57);
    EXPECT(!g.ok);
    EXPECT(NearestPoint(g, FVector{20, -5, 10}) == -1);
}

static void TestNearest() {
    const GripPoints g = MakeGripPoints(Ar(), 57);
    EXPECT(NearestPoint(g, FVector{20, -5, 10}) == kPointMagwell);
    EXPECT(NearestPoint(g, FVector{8, -4, 2}) == kPointMagwell);     // on the magwell itself
    EXPECT(NearestPoint(g, FVector{27, -3, 12}) == kPointLight);
    EXPECT(NearestPoint(g, FVector{33, -5, 11}) == kPointRailEnd);   // where the owner grabbed most
    EXPECT(NearestPoint(g, FVector{44, -5, 11}) == kPointRailEnd);   // just past the rail
    // Owner's grabs from the first run (gun-frame X 14..38, Y -0.1..-7.8,
    // Z 0.3..15.2) are all in the zone.
    EXPECT(NearestPoint(g, FVector{14.3, -4.5, 4.2}) >= 0);
    EXPECT(NearestPoint(g, FVector{38.4, -6.6, 9.4}) >= 0);
    EXPECT(NearestPoint(g, FVector{35.7, -4.3, 15.2}) >= 0);
    EXPECT(NearestPoint(g, FVector{22.7, -0.1, 0.3}) >= 0);
    // Out on the bare barrel, beside the gun, under it, at the pistol grip.
    EXPECT(NearestPoint(g, FVector{50, -5, 10}) == -1);
    EXPECT(NearestPoint(g, FVector{25, -20, 10}) == -1);
    EXPECT(NearestPoint(g, FVector{25, -5, -10}) == -1);
    EXPECT(NearestPoint(g, FVector{0, 0, 0}) == -1);
}

static void TestLongSqueeze() {
    LongSqueeze s;
    s.Start();
    const double dt = 1.0 / 72.0;
    EXPECT(s.Update(0.9f, dt));
    // Relaxed to 0.3 (under the button's 0.35) while aiming: still held.
    for (int i = 0; i < 200; ++i) EXPECT(s.Update(0.3f, dt));
    // A dip under 0.2 for a few frames: still held.
    for (int i = 0; i < 5; ++i) EXPECT(s.Update(0.1f, dt));
    EXPECT(s.Update(0.5f, dt));
    // An open hand: let go within the grace (0.12 s, 9 frames).
    bool held = true;
    int frames = 0;
    while (held && frames < 100) {
        held = s.Update(0.0f, dt);
        ++frames;
    }
    EXPECT(!held && frames <= 9);
    EXPECT(s.lowest == 0.0f);
    s.Start();
    EXPECT(s.lowest == 1.0f && s.below_for == 0);
}

int main() {
    TestThreePoints();
    TestNoLight();
    TestEdges();
    TestNearest();
    TestLongSqueeze();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("grip points: all tests passed\n");
    return 0;
}
