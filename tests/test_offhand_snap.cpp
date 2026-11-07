#include <cmath>
#include <cstdio>
#include <limits>
#include "vr/interact/offhand_snap.h"
using namespace holevr::vr::interact;
int failures = 0, checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

void Run(OffhandSnap& s, OffhandSnapInput& in, int frames) {
    for (int f = 0; f < frames; ++f) s.Update(in);
}

// Snaps on when the controllers nearly touch, lets go a little further out.
void SnapAndRelease() {
    OffhandSnap s;
    OffhandSnapInput in;
    in.allowed = true;
    in.dt = 1.0 / 72;
    in.hands_apart = 30;
    Run(s, in, 20);
    CHECK(!s.Near() && !s.TwoHands() && s.Blend() == 0);
    in.hands_apart = 10.5;
    s.Update(in);
    CHECK(!s.Near());
    in.hands_apart = 9.5;
    s.Update(in);
    CHECK(s.Near() && s.TwoHands() && s.Blend() > 0 && s.Blend() < 1);
    Run(s, in, 8);          // 0.10 s at 72 fps
    CHECK(s.Blend() == 1);
    in.hands_apart = 13.9;  // inside the release distance: stays on
    Run(s, in, 10);
    CHECK(s.Near() && s.Blend() == 1);
    in.hands_apart = 14.1;
    s.Update(in);
    CHECK(!s.Near() && !s.TwoHands() && s.Blend() < 1 && s.Blend() > 0);
    Run(s, in, 8);          // eases back to the controller
    CHECK(s.Blend() == 0);
}

// The chest pose within 35 cm, back to the other beyond 40.
void ChestPose() {
    OffhandSnap s;
    OffhandSnapInput in;
    in.allowed = true;
    in.dt = 1.0 / 72;
    in.hands_apart = 5;
    in.gun_to_chest = 50;
    Run(s, in, 10);
    CHECK(s.Near() && !s.Chest());
    in.gun_to_chest = 34;
    s.Update(in);
    CHECK(s.Chest());
    in.gun_to_chest = 39;
    s.Update(in);
    CHECK(s.Chest());
    in.gun_to_chest = 41;
    s.Update(in);
    CHECK(!s.Chest());
    in.gun_to_chest = 1e9;  // no chest (body not ready): the other pose
    s.Update(in);
    CHECK(!s.Chest());
    in.gun_to_chest = std::numeric_limits<double>::quiet_NaN();
    s.Update(in);
    CHECK(!s.Chest() && s.Near());
}

// A menu, a held item or tracking loss lets go at once; bad numbers never
// trap the hand.
void LetsGo() {
    OffhandSnap s;
    OffhandSnapInput in;
    in.allowed = true;
    in.dt = 1.0 / 72;
    in.hands_apart = 3;
    Run(s, in, 20);
    CHECK(s.Blend() == 1);
    in.allowed = false;
    s.Update(in);
    CHECK(!s.Near() && s.Blend() == 0);
    in.allowed = true;
    Run(s, in, 20);
    in.hands_apart = std::numeric_limits<double>::infinity();
    s.Update(in);
    CHECK(!s.Near() && s.Blend() == 0);
    in.hands_apart = std::numeric_limits<double>::quiet_NaN();
    s.Update(in);
    CHECK(!s.Near() && s.Blend() == 0);
    // A long frame steps at most 0.1 s.
    in.hands_apart = 3;
    in.dt = 5.0;
    s.Update(in);
    CHECK(s.Blend() == 1);
    in.dt = std::numeric_limits<double>::quiet_NaN();
    s.Update(in);
    CHECK(s.Blend() == 1);
    // Settings out of range are kept sane.
    s.Reset();
    in.dt = 1.0 / 72;
    in.snap_cm = -5;        // at least 1 cm
    in.release_cm = 0;      // never inside the snap distance
    in.hands_apart = 0.5;
    s.Update(in);
    CHECK(s.Near());
    in.hands_apart = 1.5;
    s.Update(in);
    CHECK(!s.Near());
}

int main() {
    SnapAndRelease();
    ChestPose();
    LetsGo();
    std::printf("offhand_snap: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
