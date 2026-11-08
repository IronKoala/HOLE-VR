// Native unit tests for picking a rear pinch pose on the pistol slide.
//   g++ -std=c++20 -Isrc tests/test_slide_pose.cpp -o /tmp/test_slide_pose && /tmp/test_slide_pose
#include <cstdio>

#include "vr/body/slide_pose.h"

using holevr::vr::body::slide_pose::ChooseRearPinch;

static int failures = 0;
#define EXPECT(c)                                                     \
    do {                                                              \
        if (!(c)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);  \
            ++failures;                                               \
        }                                                             \
    } while (0)

int main() {
    const double max_turn = 60.0;
    {
        // The owner's 08:02 run: only the pose from above is saved. Reaching
        // in from behind turned the hand 80-130 deg onto it; over the top,
        // 36-59 deg.
        const bool saved[2] = {true, false};
        const double behind[2] = {113.0, 0.0}, over_top[2] = {45.0, 0.0};
        EXPECT(ChooseRearPinch(saved, behind, max_turn) == 1);    // built-in from behind, no flip
        EXPECT(ChooseRearPinch(saved, over_top, max_turn) == 0);  // his saved pose
        const double edge[2] = {60.0, 0.0};
        EXPECT(ChooseRearPinch(saved, edge, max_turn) == 0);
    }
    {
        // Both posed: the one the hand turns less to reach.
        const bool saved[2] = {true, true};
        const double a[2] = {20.0, 50.0}, b[2] = {55.0, 15.0};
        EXPECT(ChooseRearPinch(saved, a, max_turn) == 0);
        EXPECT(ChooseRearPinch(saved, b, max_turn) == 1);
        const double one_fits[2] = {90.0, 40.0};
        EXPECT(ChooseRearPinch(saved, one_fits, max_turn) == 1);
        const double neither[2] = {90.0, 120.0};
        EXPECT(ChooseRearPinch(saved, neither, max_turn) == 0);  // the closer
    }
    {
        // Only the pose from behind is saved.
        const bool saved[2] = {false, true};
        const double t[2] = {0.0, 100.0};
        EXPECT(ChooseRearPinch(saved, t, max_turn) == 1);
    }
    {
        // Nothing posed: the built-in from behind.
        const bool saved[2] = {false, false};
        const double t[2] = {0.0, 0.0};
        EXPECT(ChooseRearPinch(saved, t, max_turn) == 1);
    }
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all slide pose tests passed\n");
    return 0;
}
