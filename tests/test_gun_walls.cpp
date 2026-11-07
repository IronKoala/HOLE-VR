// Native unit tests for the held gun's whole shape against walls (no game,
// no Windows).
//   g++ -std=c++20 -Isrc tests/test_gun_walls.cpp src/vr/gun_walls_math.cpp -o /tmp/test_gun_walls && /tmp/test_gun_walls
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <vector>

#include "vr/gun_walls_math.h"

using namespace holevr::vr;
using namespace holevr::vr::gun_walls;

static int failures = 0;
#define EXPECT(c)                                                     \
    do {                                                              \
        if (!(c)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);  \
            ++failures;                                               \
        }                                                             \
    } while (0)

static double Len(const FVector& v) { return std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z); }
static bool Near(const FVector& a, const FVector& b, double eps = 1e-6) { return Len(a - b) < eps; }

// A Glock-sized pistol in the gun frame (from the grip point): slide and
// barrel on top, frame and magazine down through the grip.
static std::vector<PartBox> Pistol() {
    return {
        {Box{{-6, -1.3, 0}, {18, 1.3, 3.5}}, Part::kGun},     // slide
        {Box{{-5, -1.5, -11}, {3, 1.5, 0}}, Part::kGun},      // frame and grip
        {Box{{-2, -1.2, -12}, {2, 1.2, -11}}, Part::kMagazine},
        {FiringHandBox(), Part::kFiringHand},
    };
}

// An AR-15-sized rifle: stock, receiver, handguard and barrel along X, the
// pistol grip and magazine below.
static std::vector<PartBox> Rifle() {
    return {
        {Box{{-30, -2, -6}, {-8, 2, 3}}, Part::kGun},     // stock
        {Box{{-8, -1.8, -3}, {14, 1.8, 5}}, Part::kGun},  // receiver
        {Box{{14, -2.2, -2}, {40, 2.2, 4}}, Part::kGun},  // handguard
        {Box{{40, -0.8, 0}, {55, 0.8, 1.6}}, Part::kGun}, // barrel
        {Box{{-2, -1.4, -10}, {3, 1.4, -3}}, Part::kGun}, // pistol grip
        {Box{{4, -1.3, -22}, {12, 1.3, -3}}, Part::kMagazine},
        {FiringHandBox(), Part::kFiringHand},
        {SupportHandBox({28, 0, -2}), Part::kSupportHand},
    };
}

// A flat wall: points with dot(p - on, n) < 0 are inside it; n faces the eyes.
static LineFn Wall(const FVector& on, const FVector& n, std::uintptr_t id = 7) {
    return [=](const FVector& from, const FVector& to) {
        Hit h;
        const double a = (from.X - on.X) * n.X + (from.Y - on.Y) * n.Y + (from.Z - on.Z) * n.Z;
        const double b = (to.X - on.X) * n.X + (to.Y - on.Y) * n.Y + (to.Z - on.Z) * n.Z;
        if (a < 0) { h.eyes_inside = true; return h; }
        if (b >= 0) return h;
        const double t = a / (a - b);
        h.hit = true;
        h.impact = from + (to - from) * t;
        h.normal = n;
        h.what = id;
        return h;
    };
}

// Two walls: whichever the line meets first.
static LineFn Corner(const LineFn& a, const LineFn& b) {
    return [=](const FVector& from, const FVector& to) {
        Hit ha = a(from, to), hb = b(from, to);
        if (ha.eyes_inside || hb.eyes_inside) { Hit h; h.eyes_inside = true; return h; }
        if (!ha.hit) return hb;
        if (!hb.hit) return ha;
        return Len(ha.impact - from) <= Len(hb.impact - from) ? ha : hb;
    };
}

static void World(const std::vector<Point>& outline, std::vector<FVector>* pts, std::vector<Part>* parts) {
    pts->clear();
    parts->clear();
    for (const Point& p : outline) pts->push_back(p.at), parts->push_back(p.part);
}

static void TestPistolOutline() {
    const auto outline = Outline(Pistol(), 1.5, 20.0, 40);
    EXPECT(!outline.empty());
    EXPECT(outline.size() <= 40);
    bool muzzle = false, phantom = false, hand_right = false, hand_left = false, mag_bottom = false;
    for (const Point& p : outline) {
        if (Near(p.at, {18, 0, 1.75}, 1e-6)) muzzle = true;
        // Under the barrel in front of the grip is empty: no point there.
        if (p.at.X > 8 && p.at.Z < -1) phantom = true;
        if (p.at.Y > 4.4 && p.part == Part::kFiringHand) hand_right = true;
        if (p.at.Y < -4.4 && p.part == Part::kFiringHand) hand_left = true;
        if (p.at.Z < -11.9 && p.part == Part::kMagazine) mag_bottom = true;
    }
    EXPECT(muzzle);
    EXPECT(!phantom);
    EXPECT(hand_right);
    EXPECT(hand_left);
    EXPECT(mag_bottom);
    // A pistol is short: no slices.
    for (size_t i = 0; i < outline.size(); ++i)
        for (size_t j = i + 1; j < outline.size(); ++j) EXPECT(Len(outline[i].at - outline[j].at) >= 1.5);
}

static void TestRifleSlices() {
    const auto outline = Outline(Rifle(), 1.5, 20.0, 48);
    // The handguard's right side halfway along gets a point (a wall edge
    // meeting the middle of the rifle).
    bool middle_right = false, stock_back = false, mag_bottom = false;
    for (const Point& p : outline) {
        if (p.at.X > 15 && p.at.X < 39 && p.at.Y > 2.1) middle_right = true;
        if (p.at.X < -29.9) stock_back = true;
        if (p.at.Z < -21.9 && p.part == Part::kMagazine) mag_bottom = true;
    }
    EXPECT(middle_right);
    EXPECT(stock_back);
    EXPECT(mag_bottom);
    EXPECT(outline.size() <= 48);
    // The cap is kept.
    EXPECT(Outline(Rifle(), 1.5, 20.0, 10).size() == 10);
}

static void TestPushOut() {
    // 3 cm behind a wall facing -X: out by 3 + 1.
    EXPECT(Near(PushOut({18, 0, 0}, {15, 0, 0}, {-1, 0, 0}, 1.0, 40.0), {-4, 0, 0}));
    // In front: nothing.
    EXPECT(Near(PushOut({14, 0, 0}, {15, 0, 0}, {-1, 0, 0}, 1.0, 40.0), {0, 0, 0}));
    // Capped.
    EXPECT(Near(PushOut({100, 0, 0}, {15, 0, 0}, {-1, 0, 0}, 1.0, 40.0), {-40, 0, 0}));
    // No normal: nothing.
    EXPECT(Near(PushOut({18, 0, 0}, {15, 0, 0}, {0, 0, 0}, 1.0, 40.0), {0, 0, 0}));
}

static void TestBounds() {
    FVector c;
    double r = 0;
    Bounds({{0, 0, 0}, {10, 0, 0}}, &c, &r);
    EXPECT(Near(c, {5, 0, 0}));
    EXPECT(std::fabs(r - 5) < 1e-9);
}

// The owner's case: the gun laid along a wall. The muzzle line never meets
// it, but the hand on the grip (and the slide's side) are in it.
static void TestPistolAlongWall() {
    std::vector<FVector> pts;
    std::vector<Part> parts;
    World(Outline(Pistol(), 1.5, 20.0, 40), &pts, &parts);
    const FVector eyes{-25, -12, 15};
    const LineFn wall = Wall({0, 2.0, 0}, {0, -1, 0});
    // Old check: only the muzzle (bore at Z 1.75) -> no push.
    const Result old = Solve(eyes, {FVector{18, 0, 1.75}}, {Part::kGun}, wall, 1.0, 40.0, 2);
    EXPECT(Len(old.push) < 1e-9);
    // Whole gun: the hand's knuckles 2.5 cm into the wall -> out 3.5 cm.
    const Result r = Solve(eyes, pts, parts, wall, 1.0, 40.0, 2);
    EXPECT(Near(r.push, {0, -3.5, 0}, 1e-6));
    EXPECT(r.deepest_part == Part::kFiringHand);
    EXPECT(r.what == 7);
    EXPECT(r.pushed_points > 0);
    EXPECT(!r.eyes_inside);
    // After the push nothing is behind the wall.
    for (const FVector& p : pts) EXPECT((p + r.push).Y <= 2.0 - 1.0 + 1e-6);
}

// Pointing into a wall: the muzzle goes back out (as before).
static void TestMuzzleIntoWall() {
    std::vector<FVector> pts;
    std::vector<Part> parts;
    World(Outline(Pistol(), 1.5, 20.0, 40), &pts, &parts);
    const Result r = Solve({-25, 0, 10}, pts, parts, Wall({15, 0, 0}, {-1, 0, 0}), 1.0, 40.0, 2);
    EXPECT(Near(r.push, {-4, 0, 0}, 1e-6));
    EXPECT(r.deepest_part == Part::kGun);
}

// A rifle turned so its stock meets a wall behind: the stock comes out.
static void TestRifleStock() {
    std::vector<FVector> pts;
    std::vector<Part> parts;
    World(Outline(Rifle(), 1.5, 20.0, 48), &pts, &parts);
    const Result r = Solve({-5, -10, 12}, pts, parts, Wall({-25, 0, 0}, {1, 0, 0}), 1.0, 40.0, 2);
    EXPECT(Near(r.push, {6, 0, 0}, 1e-6));
}

// A corner: two walls, two passes.
static void TestCorner() {
    std::vector<FVector> pts;
    std::vector<Part> parts;
    World(Outline(Pistol(), 1.5, 20.0, 40), &pts, &parts);
    const LineFn corner = Corner(Wall({15, 0, 0}, {-1, 0, 0}, 1), Wall({0, 2.0, 0}, {0, -1, 0}, 2));
    const Result r = Solve({-25, -12, 15}, pts, parts, corner, 1.0, 40.0, 2);
    for (const FVector& p : pts) {
        EXPECT((p + r.push).X <= 15.0 + 1e-6);
        EXPECT((p + r.push).Y <= 2.0 + 1e-6);
    }
    // One pass leaves one of the two.
    const Result one = Solve({-25, -12, 15}, pts, parts, corner, 1.0, 40.0, 1);
    bool left_in = false;
    for (const FVector& p : pts) left_in = left_in || (p + one.push).X > 15.0 + 1e-6 || (p + one.push).Y > 2.0 + 1e-6;
    EXPECT(left_in);
}

// Eyes inside something: no push, as before.
static void TestEyesInside() {
    std::vector<FVector> pts;
    std::vector<Part> parts;
    World(Outline(Pistol(), 1.5, 20.0, 40), &pts, &parts);
    const Result r = Solve({20, 0, 10}, pts, parts, Wall({15, 0, 0}, {-1, 0, 0}), 1.0, 40.0, 2);
    EXPECT(r.eyes_inside);
    EXPECT(Len(r.push) < 1e-9);
}

// The push never goes past max_cm.
static void TestMaxPush() {
    std::vector<FVector> pts;
    std::vector<Part> parts;
    World(Outline(Rifle(), 1.5, 20.0, 48), &pts, &parts);
    const Result r = Solve({-40, 0, 10}, pts, parts, Wall({-35, 0, 0}, {-1, 0, 0}), 1.0, 40.0, 2);
    EXPECT(Len(r.push) <= 40.0 + 1e-9);
    EXPECT(Len(r.push) > 39.0);
}

// Clear space: one look at each point, no push.
static void TestClear() {
    std::vector<FVector> pts;
    std::vector<Part> parts;
    World(Outline(Pistol(), 1.5, 20.0, 40), &pts, &parts);
    const Result r = Solve({-25, 0, 10}, pts, parts, Wall({100, 0, 0}, {-1, 0, 0}), 1.0, 40.0, 2);
    EXPECT(Len(r.push) < 1e-9);
    EXPECT(r.queries == static_cast<int>(pts.size()));
    EXPECT(r.deepest == -1);
}

// A wall by the rifle's muzzle end: with the near check per group, only the
// front group's points are looked at, and the push is the same as looking at
// every point.
static void TestGroups() {
    const auto outline = Outline(Rifle(), 1.0, 20.0, 48);
    std::vector<FVector> pts;
    std::vector<Part> parts;
    World(outline, &pts, &parts);
    const std::vector<int> groups = GroupsAlongBarrel(outline, 30.0, 3);
    EXPECT(groups.size() == outline.size());
    int most = 0;
    for (int g : groups) most = std::max(most, g);
    EXPECT(most == 2);
    const FVector eyes{-5, -10, 12};
    const FVector on{50, 0, 0}, n{-1, 0, 0};
    const LineFn wall = Wall(on, n);
    // Something is near when the path's ball reaches the wall plane.
    int asked = 0;
    const CloseFn close = [&](const FVector& from, const FVector& centre, double radius) {
        ++asked;
        const double a = (from.X - on.X) * n.X + (from.Y - on.Y) * n.Y + (from.Z - on.Z) * n.Z;
        const double b = (centre.X - on.X) * n.X + (centre.Y - on.Y) * n.Y + (centre.Z - on.Z) * n.Z;
        return std::min(a, b) < radius;
    };
    const Result all = Solve(eyes, pts, parts, wall, 1.0, 40.0, 2);
    const Result some = Solve(eyes, pts, parts, wall, 1.0, 40.0, 2, groups, close);
    EXPECT(Near(all.push, some.push, 1e-9));
    EXPECT(Near(some.push, {-6, 0, 0}, 1e-6));
    EXPECT(some.queries < all.queries);
    EXPECT(some.groups_close == 1);
    EXPECT(asked >= 3);
    // Nothing near: no lines at all.
    const Result none = Solve(eyes, pts, parts, Wall({200, 0, 0}, n), 1.0, 40.0, 2, groups,
                              [](const FVector&, const FVector&, double) { return false; });
    EXPECT(none.queries == 0);
    EXPECT(Len(none.push) < 1e-9);
    // A pistol is one group.
    const auto pistol = Outline(Pistol(), 1.0, 20.0, 48);
    for (int g : GroupsAlongBarrel(pistol, 30.0, 3)) EXPECT(g == 0);
}

int main() {
    TestGroups();
    TestPistolOutline();
    TestRifleSlices();
    TestPushOut();
    TestBounds();
    TestPistolAlongWall();
    TestMuzzleIntoWall();
    TestRifleStock();
    TestCorner();
    TestEyesInside();
    TestMaxPush();
    TestClear();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("gun walls: all tests passed\n");
    return 0;
}
