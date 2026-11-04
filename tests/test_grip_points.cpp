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

// The owner's AR as measured in the second run (2026-11-04 04:04): handguard
// 16.3..52.0 ahead of the grip, Y -4.4..0.2, Z 8.7..15.5; its rails end at
// 52.1; lower receiver front 17.0; a long light from 42.7. Not measured then
// (guessed for the test): a 10-round magazine X 5..11.5, Z -1..9.4, and the
// scope's top rail ending at 27.
static GunParts Ar() {
    GunParts p;
    p.have_handguard = true;
    p.hg_lo = FVector{16.3, -4.4, 8.7};
    p.hg_hi = FVector{52.0, 0.2, 15.5};
    p.have_rails = true;
    p.rail_front = 52.1;
    p.have_scope_rail = true;
    p.scope_rail_front = 27.0;
    p.have_mag = true;
    p.mag_lo = FVector{5.0, -3.4, -1.0};
    p.mag_hi = FVector{11.5, -0.8, 9.4};
    p.have_magwell = true;
    p.magwell_front = 17.0;
    p.have_light = true;
    p.light_rear = 42.7;
    return p;
}

static void TestThreePoints() {
    const GripPoints g = MakeGripPoints(Ar(), 58.4);
    EXPECT(g.ok);
    EXPECT(g.have[kPointMag] && g.have[kPointScopeRail] && g.have[kPointLight]);
    EXPECT(g.light_found && g.scope_found && g.mag_found);
    // Just in front of the magazine (3 cm), at the handguard's left face,
    // 8 cm under the handguard: where the owner reached (X 13-17, Z ~0).
    EXPECT(Near(g.p[kPointMag], FVector{14.5, -4.4, 0.7}));
    // On the handguard's left side (2.5 cm out), level with its middle.
    EXPECT(Near(g.p[kPointScopeRail], FVector{27.0, -6.9, 12.1}));
    // The hand's front edge at the light's back end.
    EXPECT(Near(g.p[kPointLight], FVector{38.2, -6.9, 12.1}));
    // Back to front: magazine, scope rail end, light.
    EXPECT(g.p[kPointMag].X < g.p[kPointScopeRail].X && g.p[kPointScopeRail].X < g.p[kPointLight].X);
}

static void TestFallbacks() {
    // No light: the front of the handguard.
    GunParts p = Ar();
    p.have_light = false;
    GripPoints g = MakeGripPoints(p, 58.4);
    EXPECT(g.have[kPointLight] && !g.light_found);
    EXPECT(std::fabs(g.p[kPointLight].X - 47.6) < 1e-9);
    // A light on the receiver (too far back) doesn't count.
    p = Ar();
    p.light_rear = 18;
    g = MakeGripPoints(p, 58.4);
    EXPECT(!g.light_found && std::fabs(g.p[kPointLight].X - 47.6) < 1e-9);
    // No scope rail found: halfway between the handguard's back end and the
    // light point.
    p = Ar();
    p.have_scope_rail = false;
    g = MakeGripPoints(p, 58.4);
    EXPECT(g.have[kPointScopeRail] && !g.scope_found);
    EXPECT(std::fabs(g.p[kPointScopeRail].X - 0.5 * (21.5 + 38.2)) < 1e-9);
    // A scope rail that ends on the receiver: the hand at the handguard's
    // back end, still above the magazine point.
    p = Ar();
    p.scope_rail_front = 10;
    g = MakeGripPoints(p, 58.4);
    EXPECT(g.have[kPointScopeRail] && std::fabs(g.p[kPointScopeRail].X - 21.5) < 1e-9);
    EXPECT(g.p[kPointScopeRail].Z > g.p[kPointMag].Z + 5);
    // No magazine in the gun: just behind the lower receiver's front end.
    p = Ar();
    p.have_mag = false;
    g = MakeGripPoints(p, 58.4);
    EXPECT(!g.mag_found && Near(g.p[kPointMag], FVector{15.0, -4.4, 0.7}));
    // A "magazine" box that isn't below the handguard is ignored.
    p = Ar();
    p.mag_lo.Z = 10;
    g = MakeGripPoints(p, 58.4);
    EXPECT(!g.mag_found);
    // Nothing but the handguard: its back end.
    p = Ar();
    p.have_mag = p.have_magwell = false;
    g = MakeGripPoints(p, 58.4);
    EXPECT(Near(g.p[kPointMag], FVector{20.8, -6.9, 12.1}));
}

static void TestEdges() {
    // Rails past the muzzle (a bad measurement): kept 2 cm behind it.
    GunParts p = Ar();
    p.have_light = false;
    p.rail_front = 60;
    p.hg_hi.X = 60;
    GripPoints g = MakeGripPoints(p, 50);
    EXPECT(std::fabs(g.p[kPointLight].X - 43.5) < 1e-9);
    // A short handguard: room for fewer points, none on top of another.
    p = Ar();
    p.hg_hi.X = 24;
    p.rail_front = 24;
    p.have_light = false;
    p.scope_rail_front = 30;
    g = MakeGripPoints(p, 58.4);
    EXPECT(g.ok && g.have[kPointMag] && g.have[kPointLight] && !g.have[kPointScopeRail]);
    // No handguard found: no points (the hand holds where it grabs).
    p = Ar();
    p.have_handguard = false;
    g = MakeGripPoints(p, 58.4);
    EXPECT(!g.ok);
    EXPECT(NearestPoint(g, FVector{20, -5, 10}) == -1);
}

static void TestNearest() {
    const GripPoints g = MakeGripPoints(Ar(), 58.4);
    // The owner's reaches in the second run for the point in front of the
    // magazine (X 13-17, Z -3..5) and behind the light (X 32-40, Z 8-14).
    const FVector mag_reaches[] = {{16.1, -3.4, 2.1}, {14.7, -2.5, 1.2}, {13.1, -2.6, -2.9}, {9.4, -1.1, -2.6},
                                   {15.4, -3.5, -2.0}, {13.7, -6.4, 4.7}, {18.3, -5.1, -2.4}, {17.0, -6.3, 0.5}};
    for (const FVector& r : mag_reaches) EXPECT(NearestPoint(g, r) == kPointMag);
    const FVector light_reaches[] = {{38.4, -6.8, 10.0}, {35.8, -7.8, 7.6}, {39.3, -6.3, 8.8},
                                     {35.9, -6.7, 13.6}, {42.0, -6.3, 12.1}, {36.2, -8.1, 14.4}};
    for (const FVector& r : light_reaches) EXPECT(NearestPoint(g, r) == kPointLight);
    // On the handguard between them: the scope rail's end.
    EXPECT(NearestPoint(g, FVector{25.7, -5.6, 9.5}) == kPointScopeRail);
    EXPECT(NearestPoint(g, FVector{28.8, -4.6, 13.6}) == kPointScopeRail);
    // Under the back of the handguard, closer to it than to the magazine.
    EXPECT(NearestPoint(g, FVector{21.0, -4.0, 8.0}) == kPointScopeRail);
    // Just past the end of the handguard: still the light.
    EXPECT(NearestPoint(g, FVector{54.1, -6.9, 15.0}) == kPointLight);
    // Out on the bare barrel, beside the gun, far under it, at the pistol grip.
    EXPECT(NearestPoint(g, FVector{58, -5, 10}) == -1);
    EXPECT(NearestPoint(g, FVector{25, -20, 10}) == -1);
    EXPECT(NearestPoint(g, FVector{25, -5, -12}) == -1);
    EXPECT(NearestPoint(g, FVector{0, 0, 0}) == -1);
    EXPECT(NearestPoint(g, FVector{11.5, -9.8, -33.4}) == -1);
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
    TestFallbacks();
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
