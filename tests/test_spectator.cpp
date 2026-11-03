// Native unit tests for the monitor view layout (one eye on the monitor).
//   g++ -std=c++20 -Isrc tests/test_spectator.cpp src/vr/spectator_math.cpp -o /tmp/test_spectator && /tmp/test_spectator
#include <cmath>
#include <cstdio>

#include "vr/spectator_math.h"

using namespace holevr::vr::spectator;

static int failures = 0;
#define EXPECT(c)                                                     \
    do {                                                              \
        if (!(c)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);  \
            ++failures;                                               \
        }                                                             \
    } while (0)

static bool Inside(const PixelRect& r, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h) {
    return r.x >= x0 && r.y >= y0 && r.x + r.w <= x0 + w && r.y + r.h <= y0 + h;
}

static void TestParse() {
    EXPECT(ParseView("right", View::kBothEyes) == View::kRightEye);
    EXPECT(ParseView(" Left ", View::kBothEyes) == View::kLeftEye);
    EXPECT(ParseView("BOTH", View::kRightEye) == View::kBothEyes);
    EXPECT(ParseView("", View::kRightEye) == View::kRightEye);
    EXPECT(ParseView("sideways", View::kRightEye) == View::kRightEye);
    EXPECT(ParseView(nullptr, View::kLeftEye) == View::kLeftEye);
}

// The owner's ultrawide: window 3440x1440, each eye 1720x1440.
static void TestUltrawideRightEye() {
    Layout l = ComputeLayout(3440, 1440, 1, 0.5f, 0.5f, 16.0f / 9.0f);
    EXPECT(l.valid);
    // Full eye width, 16:9 tall, from the right half only.
    EXPECT(l.crop.w == 1720);
    EXPECT(l.crop.h == 967 || l.crop.h == 968);
    EXPECT(Inside(l.crop, 1720, 0, 1720, 1440));
    EXPECT(l.crop.y + l.crop.h / 2 >= 719 && l.crop.y + l.crop.h / 2 <= 721);  // centred
    // Fills the window's height, centred left to right, still 16:9.
    EXPECT(l.dest.h == 1440);
    EXPECT(l.dest.w == 2561 || l.dest.w == 2560);
    EXPECT(l.dest.y == 0);
    EXPECT(l.dest.x + l.dest.w / 2 >= 1719 && l.dest.x + l.dest.w / 2 <= 1721);
}

static void TestLeftEyeUsesLeftHalf() {
    Layout l = ComputeLayout(3440, 1440, 0, 0.5f, 0.5f, 16.0f / 9.0f);
    EXPECT(l.valid);
    EXPECT(Inside(l.crop, 0, 0, 1720, 1440));
}

// Straight ahead off-centre: the crop follows it but stays inside the eye.
static void TestFollowsStraightAhead() {
    Layout up = ComputeLayout(3440, 1440, 1, 0.5f, 0.3f, 16.0f / 9.0f);
    Layout mid = ComputeLayout(3440, 1440, 1, 0.5f, 0.5f, 16.0f / 9.0f);
    EXPECT(up.crop.y < mid.crop.y);
    Layout top = ComputeLayout(3440, 1440, 1, 0.5f, 0.0f, 16.0f / 9.0f);
    EXPECT(top.crop.y == 0);
    Layout bottom = ComputeLayout(3440, 1440, 1, 0.5f, 1.0f, 16.0f / 9.0f);
    EXPECT(bottom.crop.y + bottom.crop.h == 1440);
}

// A 16:9 monitor (1920x1080, each eye 960x1080): the view fills the window.
static void TestSixteenNineMonitor() {
    Layout l = ComputeLayout(1920, 1080, 1, 0.5f, 0.5f, 16.0f / 9.0f);
    EXPECT(l.valid);
    EXPECT(l.crop.w == 960 && l.crop.h == 540);
    EXPECT(l.dest.x == 0 && l.dest.y == 0 && l.dest.w == 1920 && l.dest.h == 1080);
}

// A wide eye picture with a narrow shape: crop the sides, not the height.
static void TestNarrowShape() {
    Layout l = ComputeLayout(3440, 1440, 1, 0.5f, 0.5f, 1.0f);
    EXPECT(l.valid);
    EXPECT(l.crop.w == 1440 && l.crop.h == 1440);
    EXPECT(Inside(l.crop, 1720, 0, 1720, 1440));
    EXPECT(l.dest.w == 1440 && l.dest.h == 1440 && l.dest.x == 1000);
}

static void TestBadInput() {
    EXPECT(!ComputeLayout(0, 0, 1, 0.5f, 0.5f, 16.0f / 9.0f).valid);
    EXPECT(!ComputeLayout(3440, 1440, 1, 0.5f, 0.5f, 0.0f).valid);
    EXPECT(!ComputeLayout(3440, 1440, 1, 0.5f, 0.5f, std::nanf("")).valid);
}

static void TestStraightAhead() {
    float u, v;
    // Symmetric field of view: the middle.
    StraightAhead(-0.8f, 0.8f, 0.8f, -0.8f, &u, &v);
    EXPECT(std::fabs(u - 0.5f) < 1e-4f && std::fabs(v - 0.5f) < 1e-4f);
    // A right eye that sees further right: straight ahead is left of centre.
    StraightAhead(-0.7f, 0.9f, 0.8f, -0.8f, &u, &v);
    EXPECT(u < 0.5f && u > 0.3f);
    // Sees further down than up: straight ahead is above the middle.
    StraightAhead(-0.8f, 0.8f, 0.7f, -0.9f, &u, &v);
    EXPECT(v < 0.5f);
    // Degenerate: the middle.
    StraightAhead(0.0f, 0.0f, 0.0f, 0.0f, &u, &v);
    EXPECT(u == 0.5f && v == 0.5f);
}

int main() {
    TestParse();
    TestUltrawideRightEye();
    TestLeftEyeUsesLeftHalf();
    TestFollowsStraightAhead();
    TestSixteenNineMonitor();
    TestNarrowShape();
    TestBadInput();
    TestStraightAhead();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all spectator tests passed\n");
    return 0;
}
