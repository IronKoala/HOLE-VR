// Native unit tests for spotting white squares drawn into one eye only
// (no game, no Windows).
//   g++ -std=c++20 -O2 -Isrc tests/test_square_watch.cpp src/vr/square_watch_math.cpp -o /tmp/test_square_watch && /tmp/test_square_watch
#include <cstdio>

#include "vr/square_watch_math.h"

using namespace holevr::vr::square_watch;

static int failures = 0;
#define EXPECT(c)                                                     \
    do {                                                              \
        if (!(c)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);  \
            ++failures;                                               \
        }                                                             \
    } while (0)

static Band Dark(int eye_w, int h, float v = 0.15f) {
    Band b;
    b.width = eye_w * 2;
    b.height = h;
    b.luma.assign(static_cast<size_t>(b.width) * h, v);
    return b;
}

static void Rect(Band& b, int eye, int x, int y, int w, int h, float v) {
    const int base = eye * (b.width / 2);
    for (int yy = y; yy < y + h && yy < b.height; ++yy)
        for (int xx = x; xx < x + w && xx < b.width / 2; ++xx) b.luma[static_cast<size_t>(yy) * b.width + base + xx] = v;
}

static void TestNothing() {
    Band b = Dark(400, 96);
    Odd o = FindOdd(b);
    EXPECT(o.count[0] == 0 && o.count[1] == 0);
    EXPECT(o.checked[0] == 0);
}

static void TestNearLampSeenByBothEyes() {
    // A bright lamp 40 px further right in the left eye (near) and a far one
    // at the same place in both: nothing odd.
    Band b = Dark(400, 96);
    Rect(b, 0, 140, 20, 30, 30, 0.95f);
    Rect(b, 1, 100, 20, 30, 30, 0.95f);
    Rect(b, 0, 300, 50, 20, 20, 0.9f);
    Rect(b, 1, 300, 50, 20, 20, 0.9f);
    Odd o = FindOdd(b);
    EXPECT(o.checked[0] > 0 && o.checked[1] > 0);
    EXPECT(o.count[0] == 0);
    EXPECT(o.count[1] == 0);
    EXPECT(o.Score() == 0);
}

static void TestWhiteSquaresInRightEye() {
    Band b = Dark(400, 96);
    Rect(b, 1, 64, 32, 16, 16, 1.0f);   // a white square
    Rect(b, 1, 200, 8, 48, 8, 0.95f);   // a white bar
    Odd o = FindOdd(b);
    EXPECT(o.count[0] == 0);
    EXPECT(o.count[1] >= 8);
    EXPECT(o.Eye() == 1);
    EXPECT(o.Score() == o.count[1]);
    EXPECT(o.first_x[1] >= 0 && o.first_y[1] >= 0);
}

static void TestBrightSceneHidesNothingBrokenButStillMatches() {
    // A bright floor in both eyes, slightly shifted: no odd blocks.
    Band b = Dark(400, 96, 0.9f);
    Odd o = FindOdd(b);
    EXPECT(o.checked[0] > 0);
    EXPECT(o.count[0] == 0 && o.count[1] == 0);
}

static void TestLuma() {
    EXPECT(Luma(0xffffffffu, false, false) > 0.999f);
    EXPECT(Luma(0x000000ffu, false, false) > 0.21f && Luma(0x000000ffu, false, false) < 0.22f);  // red
    EXPECT(Luma(0x000000ffu, true, false) > 0.07f && Luma(0x000000ffu, true, false) < 0.08f);    // blue in BGRA
    EXPECT(Luma(0x3fffffffu, false, true) > 0.999f);
    EXPECT(Luma(0u, false, true) == 0.f);
}

static void TestTooSmall() {
    Band b;
    b.width = 4;
    b.height = 4;
    b.luma.assign(16, 1.f);
    Odd o = FindOdd(b);
    EXPECT(o.checked[0] == 0 && o.count[0] == 0);
}

int main() {
    TestNothing();
    TestNearLampSeenByBothEyes();
    TestWhiteSquaresInRightEye();
    TestBrightSceneHidesNothingBrokenButStillMatches();
    TestLuma();
    TestTooSmall();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("square_watch: all tests passed\n");
    return 0;
}
