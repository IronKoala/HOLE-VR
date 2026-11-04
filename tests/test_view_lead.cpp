// Native unit tests for predicting the camera's move during the player's tick
// (no game, no Windows).
//   g++ -std=c++20 -Isrc tests/test_view_lead.cpp src/vr/view_lead_math.cpp -o /tmp/test_view_lead && /tmp/test_view_lead
#include <cmath>
#include <cstdio>

#include "vr/view_lead_math.h"

using namespace holevr::vr;
using namespace holevr::vr::view_lead;

static int failures = 0;
#define EXPECT(c)                                                     \
    do {                                                              \
        if (!(c)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);  \
            ++failures;                                               \
        }                                                             \
    } while (0)

static bool Near(const FVector& a, const FVector& b, double eps = 1e-6) { return Length(a - b) < eps; }

static FQuat Yaw(double deg) {
    const double h = deg * kDegToRad * 0.5;
    return {0, 0, std::sin(h), std::cos(h)};
}

static Anchors At(uintptr_t who, const FVector& p, double yaw_deg) {
    Anchors a;
    a.who = who;
    a.camera = true;
    a.camera_at = p + FVector{20, 0, 150};
    a.mesh = true;
    a.mesh_at = p + FVector{0, 0, -90};
    a.mesh_rot = Yaw(yaw_deg);
    a.capsule = true;
    a.capsule_at = p;
    return a;
}

static void TestWalking() {
    // Walking 3.5 cm along X in one frame: every anchor says the same move.
    const Anchors then = At(1, {100, 0, 90}, 0), now = At(1, {103.5, 0, 90}, 0);
    const FVector cam_then{120, 0, 240};
    Leads l = Predict(then, now, cam_then, 120);
    for (int k = 0; k < kKinds; ++k) {
        EXPECT(l.valid[k]);
        EXPECT(Near(l.move[k], {3.5, 0, 0}));
    }
    EXPECT(Pick(l, -1) == kCamera);
    EXPECT(Pick(l, 0) == -1);
    EXPECT(Pick(l, 2) == kMesh);
    EXPECT(Pick(l, 3) == kCapsule);
}

static void TestTurning() {
    // The body turns 90 degrees in place; the camera sits 20 cm in front of
    // its middle, so it swings from +X to +Y. Only the body anchor sees that.
    const Anchors then = At(1, {0, 0, 90}, 0), now = At(1, {0, 0, 90}, 90);
    const FVector cam_then{20, 0, 240};
    Leads l = Predict(then, now, cam_then, 120);
    EXPECT(l.valid[kMesh]);
    EXPECT(Near(l.move[kMesh], {-20, 20, 0}));
    EXPECT(Near(l.move[kCapsule], {0, 0, 0}));
}

static void TestTeleportAndNewPlayer() {
    const Anchors then = At(1, {0, 0, 90}, 0);
    Leads far = Predict(then, At(1, {500, 0, 90}, 0), {20, 0, 240}, 120);
    for (int k = 0; k < kKinds; ++k) EXPECT(!far.valid[k]);
    EXPECT(Pick(far, -1) == -1);
    Leads other = Predict(then, At(2, {1, 0, 90}, 0), {20, 0, 240}, 120);
    for (int k = 0; k < kKinds; ++k) EXPECT(!other.valid[k]);
    Anchors none;
    Leads empty = Predict(none, none, {}, 120);
    EXPECT(Pick(empty, -1) == -1);
}

static void TestMissingCamera() {
    Anchors then = At(1, {0, 0, 90}, 0), now = At(1, {2, 0, 90}, 0);
    then.camera = false;
    Leads l = Predict(then, now, {20, 0, 240}, 120);
    EXPECT(!l.valid[kCamera]);
    EXPECT(Pick(l, -1) == kMesh);
    EXPECT(Pick(l, 1) == -1);
}

static void TestStats() {
    Stats s;
    Leads l;
    l.valid[kCamera] = true;
    l.move[kCamera] = {3, 0, 0};
    s.Add({4, 0, 0}, l, kCamera);  // 4 cm off without, 1 cm with
    s.Add({0, 0, 0}, Leads{}, -1);  // standing still: not counted
    EXPECT(s.frames == 1);
    EXPECT(std::fabs(s.none_sum - 4) < 1e-9);
    EXPECT(std::fabs(s.used_sum - 1) < 1e-9);
    EXPECT(s.n[kCamera] == 1 && s.n[kMesh] == 0);
    // The 1 cm miss is along world X; facing 90 degrees (world +Y) that is
    // sideways, not forward. Under 1 cm or more is not one of the worst.
    Stats t;
    t.Add({4, 0, 0}, l, kCamera, 90.0);
    EXPECT(std::fabs(t.axis_sum[0]) < 1e-9);
    EXPECT(std::fabs(t.axis_sum[1] - 1) < 1e-9);
    EXPECT(t.worst_count == 0);
    // Worst frames: kept largest first, at most kWorst.
    const double misses[] = {2, 5, 1.5, 4, 3};
    for (double m : misses) t.Add({3 + m, 0, 0}, l, kCamera, 0.0);
    EXPECT(t.worst_count == Stats::kWorst);
    EXPECT(std::fabs(t.worst[0].cm - 5) < 1e-9);
    EXPECT(std::fabs(t.worst[1].cm - 4) < 1e-9);
    EXPECT(std::fabs(t.worst[2].cm - 3) < 1e-9);
    EXPECT(std::fabs(t.worst[0].miss.X - 5) < 1e-9 && t.worst[0].used == kCamera);
}

static void TestGunShake() {
    GunShake g;
    // Smooth motion (constant velocity, constant turn): no shake.
    for (int i = 0; i < 10; ++i) g.Add(true, FVector{1.0 * i, 0, 0}, Yaw(2.0 * i), true);
    EXPECT(g.n[1] == 8);
    EXPECT(g.cm_sum[1] < 1e-9);
    EXPECT(g.deg_sum[1] < 1e-6);
    // A one-frame 1 cm jump: shake 1, then 2, then 1.
    GunShake j;
    const double xs[] = {0, 0, 0, 1, 0, 0};
    for (double x : xs) j.Add(true, FVector{x, 0, 0}, FQuat{}, false);
    EXPECT(j.n[0] == 4);
    EXPECT(std::fabs(j.cm_sum[0] - 4.0) < 1e-9);
    EXPECT(std::fabs(j.cm_max[0] - 2.0) < 1e-9);
    // A gap restarts the history.
    j.Add(false, {}, {}, false);
    j.Add(true, FVector{50, 0, 0}, FQuat{}, false);
    j.Add(true, FVector{51, 0, 0}, FQuat{}, false);
    EXPECT(j.n[0] == 4);
    j.AddMovedAfter(0.0);
    j.AddMovedAfter(2.0);
    EXPECT(j.checked_frames == 2 && j.moved_frames == 1 && std::fabs(j.moved_max - 2.0) < 1e-9);
    EXPECT(std::fabs(AngleDeg(Yaw(30)) - 30.0) < 1e-6);

    // The gun against the hand: a fixed offset gives no wobble; a 0.5 cm
    // twitch shows as 0.5 then 0.5 back; a re-grip (big jump) is left out;
    // a frame with no grip restarts it.
    GunShake p;
    for (int i = 0; i < 5; ++i) p.AddPlacement(true, FVector{10, 2, 3}, Yaw(5), true);
    EXPECT(p.place_n[1] == 4 && p.place_cm_sum[1] < 1e-9 && p.place_deg_sum[1] < 1e-6);
    p.AddPlacement(true, FVector{10.5, 2, 3}, Yaw(5), true);
    p.AddPlacement(true, FVector{10, 2, 3}, Yaw(7), true);
    EXPECT(p.place_n[1] == 6);
    EXPECT(std::fabs(p.place_cm_sum[1] - 1.0) < 1e-9);
    EXPECT(std::fabs(p.place_deg_max[1] - 2.0) < 1e-6);
    p.AddPlacement(true, FVector{40, 2, 3}, Yaw(7), false);
    EXPECT(p.place_n[0] == 0 && p.place_n[1] == 6);
    p.AddPlacement(false, {}, {}, false);
    p.AddPlacement(true, FVector{0, 0, 0}, FQuat{}, false);
    EXPECT(p.place_n[0] == 0);
    p.AddPlacement(true, FVector{0.1, 0, 0}, FQuat{}, false);
    EXPECT(p.place_n[0] == 1 && std::fabs(p.place_cm_max[0] - 0.1) < 1e-9);
    p.ResetTotals();
    EXPECT(p.place_n[0] == 0 && p.place_n[1] == 0 && p.place_cm_max[0] == 0);
}

// The camera moves v a frame, which the lead sees, and jumps j once at frame
// `at` after the tick, which the lead only sees at the next tick (and then
// counts again). Returns the largest |mismatch| between what the mod placed
// and the drawn view, and the view's distance from the game camera at the end.
static void RunSettle(double follow, double v, double j, int at, double* worst_mismatch, double* mismatch_at,
                      double* mismatch_next, double* off_end) {
    FVector game{0, 0, 0}, view{0, 0, 0};
    *worst_mismatch = *mismatch_at = *mismatch_next = 0;
    for (int n = 1; n <= 40; ++n) {
        game = game + FVector{v + (n == at ? j : 0.0), 0, 0};
        const FVector lead{v + (n == at + 1 ? j : 0.0), 0, 0};
        const FVector placed = view + lead;
        const Settle s = SettleView(game, placed, follow, true, 30.0);
        const double m = Length(s.view - placed);
        if (m > *worst_mismatch) *worst_mismatch = m;
        if (n == at) *mismatch_at = m;
        if (n == at + 1) *mismatch_next = m;
        view = s.view;
    }
    *off_end = Length(view - game);
}

static void TestSettle() {
    double worst, at, next, off;
    // Walking, all seen by the lead: the view is the game camera, nothing off.
    RunSettle(0.15, 4.0, 0.0, 10, &worst, &at, &next, &off);
    EXPECT(worst < 1e-9 && off < 1e-9);
    // As before (follow 1): a 9 cm jump after the tick puts the gun 9 cm off,
    // then 9 cm off the other way the next frame.
    RunSettle(1.0, 4.0, 9.0, 10, &worst, &at, &next, &off);
    EXPECT(std::fabs(at - 9.0) < 1e-9 && std::fabs(next - 9.0) < 1e-9 && off < 1e-9);
    // Settled: off by 0.15 of it, then much less, and the view catches up.
    RunSettle(0.15, 4.0, 9.0, 10, &worst, &at, &next, &off);
    EXPECT(std::fabs(at - 1.35) < 1e-9);
    EXPECT(next < 0.25);
    EXPECT(worst < 1.36);
    EXPECT(off < 0.05);
    // No lead (no tick since the last views) or a teleport: the game camera.
    Settle s = SettleView({100, 0, 0}, {95, 0, 0}, 0.15, false, 30.0);
    EXPECT(s.snapped && std::fabs(s.view.X - 100) < 1e-9);
    s = SettleView({100, 0, 0}, {50, 0, 0}, 0.15, true, 30.0);
    EXPECT(s.snapped && std::fabs(s.view.X - 100) < 1e-9);
    s = SettleView({10, 0, 0}, {0, 0, 0}, 0.15, true, 30.0);
    EXPECT(!s.snapped && std::fabs(s.view.X - 1.5) < 1e-9 && std::fabs(s.held_off.X + 8.5) < 1e-9);
    SettleStats st;
    st.Add(s);
    st.Add(SettleView({100, 0, 0}, {95, 0, 0}, 0.15, false, 30.0));
    EXPECT(st.frames == 2 && st.snaps == 1 && std::fabs(st.mismatch_sum - 1.5) < 1e-9 &&
           std::fabs(st.off_max - 8.5) < 1e-9);
}

// The extraction hole: the character falls 55.6 cm a frame while the game's
// view holds still. The lead is applied once, found 55.6 cm off, and stopped,
// so the hands stay with the view; it comes back once the fall ends.
static void TestLeadTrust() {
    LeadTrust t;
    auto frame = [&](const FVector& predicted, const FVector& actual) {
        Leads l;
        l.valid[kCamera] = true;
        l.move[kCamera] = predicted;
        const FVector applied = t.trusted ? predicted : FVector{};
        t.Update(actual, l, Pick(l, -1), 30.0, 10.0);
        return Length(applied - actual);  // how far the hands end up off the view
    };
    // Walking and sprinting, misses up to 15 cm: never stopped.
    for (int i = 0; i < 20; ++i) EXPECT(frame({4, 0, 0}, {4, 0, 0}) < 1e-9);
    EXPECT(frame({7, 0, 0}, {7, 0, -15}) < 15.01);
    EXPECT(t.trusted && t.stops == 0);
    // The fall: off once, then with the view every frame.
    EXPECT(std::fabs(frame({0, 0, -55.6}, {0, 0, 0}) - 55.6) < 1e-9);
    EXPECT(!t.trusted && t.stops == 1 && std::fabs(t.worst_miss - 55.6) < 1e-9);
    for (int i = 0; i < 50; ++i) EXPECT(frame({0, 0, -55.6}, {0, 0, 0}) < 1e-9);
    EXPECT(!t.trusted && t.off_frames == 50);
    // Landed: the prediction agrees again, and the lead is back the frame after.
    EXPECT(frame({0, 0, 0}, {0, 0, 0}) < 1e-9);
    EXPECT(t.trusted);
    EXPECT(frame({4, 0, 0}, {4, 0, 0}) < 1e-9);
    // A one-frame jump of the view (a teleport) stops it for one frame only.
    frame({4, 0, 0}, {50, 0, 0});
    EXPECT(!t.trusted);
    frame({4, 0, 0}, {4, 0, 0});
    EXPECT(t.trusted && t.stops == 2);
    // No prediction at all: nothing learned.
    Leads none;
    t.Update({100, 0, 0}, none, -1, 30.0, 10.0);
    EXPECT(t.trusted && t.stops == 2);
    t.ResetTotals();
    EXPECT(t.trusted && t.stops == 0 && t.off_frames == 0 && t.worst_miss == 0);
}

// The old push: back along the eyes-to-muzzle line to 1 cm short of where it
// meets the wall plane y = wall_y (eyes on the y < wall_y side).
static double OldPush(const FVector& eyes, const FVector& muzzle, double wall_y) {
    const double t = (wall_y - eyes.Y) / (muzzle.Y - eyes.Y);
    const FVector at = eyes + (muzzle - eyes) * t;
    return Length(muzzle - at) + 1.0;
}

static void TestWallPush() {
    // A wall straight ahead, the muzzle 3 cm in: back 4 cm, as before.
    FVector p = WallPushOut({103, 0, 0}, {100, 0, 0}, {-1, 0, 0}, 1.0, 40.0);
    EXPECT(Near(p, {-4, 0, 0}));
    // Sprinting along a wall 6 cm beside the head, the muzzle 3 cm into it:
    // out 4 cm sideways, where the old push came back almost 20 cm.
    const FVector reach{55, 9, 0};
    FVector eyes{0, 0, 0};
    p = WallPushOut(eyes + reach, {36.7, 6, 0}, {0, -1, 0}, 1.0, 40.0);
    EXPECT(Near(p, {0, -4, 0}));
    EXPECT(OldPush(eyes, eyes + reach, 6.0) > 19.0);
    // The head bobs 1 cm toward the wall: the push grows 1 cm, not 6.
    const FVector eyes2{0, 1, 0};
    const FVector p2 = WallPushOut(eyes2 + reach, {30, 6, 0}, {0, -1, 0}, 1.0, 40.0);
    EXPECT(std::fabs(Length(p2 - p) - 1.0) < 1e-9);
    EXPECT(OldPush(eyes2, eyes2 + reach, 6.0) - OldPush(eyes, eyes + reach, 6.0) > 6.0);
    // Muzzle in front of the surface: nothing. A normal that isn't one: nothing.
    EXPECT(Length(WallPushOut({99, 0, 0}, {100, 0, 0}, {-1, 0, 0}, 1.0, 40.0)) < 1e-12);
    EXPECT(Length(WallPushOut({103, 0, 0}, {100, 0, 0}, {0, 0, 0}, 1.0, 40.0)) < 1e-12);
    // At most max_cm; a normal of any length counts as its direction.
    p = WallPushOut({200, 0, 0}, {100, 0, 0}, {-2, 0, 0}, 1.0, 40.0);
    EXPECT(Near(p, {-40, 0, 0}));
}

int main() {
    TestLeadTrust();
    TestWallPush();
    TestSettle();
    TestGunShake();
    TestWalking();
    TestTurning();
    TestTeleportAndNewPlayer();
    TestMissingCamera();
    TestStats();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("view_lead: all tests passed\n");
    return 0;
}
