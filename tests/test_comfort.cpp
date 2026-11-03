// Native unit tests for the comfort logic (no game, no Windows).
//   g++ -std=c++20 -Isrc tests/test_comfort.cpp src/vr/comfort_math.cpp -o /tmp/test_comfort && /tmp/test_comfort
#include <cmath>
#include <cstdio>

#include "vr/comfort_math.h"

using namespace holevr::vr;
using namespace holevr::vr::comfort;

static int failures = 0;
#define EXPECT(c)                                                     \
    do {                                                              \
        if (!(c)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);  \
            ++failures;                                               \
        }                                                             \
    } while (0)

static uint32_t Alpha(const std::vector<uint32_t>& px, int x, int y) { return px[static_cast<size_t>(y) * kTexW + x] >> 24; }

static void TestTexture() {
    std::vector<uint32_t> px = TexturePixels();
    EXPECT(px.size() == static_cast<size_t>(kTexW) * kTexH);
    // Colour is always black (premultiplied): only alpha is set.
    bool black = true;
    for (uint32_t p : px) black = black && (p & 0x00FFFFFFu) == 0;
    EXPECT(black);
    for (int level = 1; level <= kVignetteLevels; ++level) {
        Rect r = VignetteTile(level);
        EXPECT(r.w == kTile && r.h == kTile);
        // Clear in the middle, black at the edge of the tile.
        EXPECT(Alpha(px, r.x + kTile / 2, r.y + kTile / 2) == 0);
        EXPECT(Alpha(px, r.x + 1, r.y + kTile / 2) == 255);
    }
    // A stronger vignette is darker part-way out.
    Rect weak = VignetteTile(2), strong = VignetteTile(16);
    int off = kTile / 2 + kTile / 6;
    EXPECT(Alpha(px, strong.x + off, strong.y + kTile / 2) > Alpha(px, weak.x + off, weak.y + kTile / 2));
    // Fade blocks: uniform, rising opacity, inside the texture.
    for (int level = 1; level <= kFadeLevels; ++level) {
        Rect b = FadeBlock(level);
        EXPECT(b.x >= 0 && b.y >= 0 && b.x + b.w <= kTexW && b.y + b.h <= kTexH);
        uint32_t a0 = Alpha(px, b.x, b.y), a1 = Alpha(px, b.x + b.w - 1, b.y + b.h - 1);
        EXPECT(a0 == a1);
        EXPECT(std::abs(static_cast<int>(a0) - static_cast<int>(255.0 * level / kFadeLevels + 0.5)) <= 1);
    }
    EXPECT(Alpha(px, FadeBlock(kFadeLevels).x, FadeBlock(kFadeLevels).y) == 255);
}

static void TestLevels() {
    EXPECT(VignetteLevel(0.0) == 0);
    EXPECT(VignetteLevel(0.01) == 0);
    EXPECT(VignetteLevel(1.0) == kVignetteLevels);
    EXPECT(VignetteLevel(2.0) == kVignetteLevels);
    EXPECT(VignetteLevel(0.5) == kVignetteLevels / 2);
    EXPECT(VignetteLevel(std::nan("")) == 0);
    EXPECT(FadeLevel(0.0) == 0);
    EXPECT(FadeLevel(1.0) == kFadeLevels);
    EXPECT(ClearHalfAngleDeg(0) > ClearHalfAngleDeg(1));
    EXPECT(ClearHalfAngleDeg(1) >= 25.0);
}

static void TestSmoother() {
    Smoother s;
    for (int i = 0; i < 100; ++i) s.Step(1.0, 0.01, 0.1, 0.3);
    EXPECT(s.value > 0.99);
    // Falls slower than it rises.
    Smoother up, down;
    down.value = 1.0;
    up.Step(1.0, 0.1, 0.1, 0.3);
    down.Step(0.0, 0.1, 0.1, 0.3);
    EXPECT(up.value > 1.0 - down.value);
}

static void TestPhysicalBody() {
    PhysicalBody b;
    b.Configure(true, 35, false);
    double h = 1.70;
    int crouches = 0, stands = 0, jumps = 0;
    auto run = [&](double target, double seconds) {
        for (double t = 0; t < seconds; t += 1.0 / 72) {
            h += (target - h) * 0.1;
            BodyEvent e = b.Update(h, 1.0 / 72);
            crouches += e == BodyEvent::kCrouch;
            stands += e == BodyEvent::kStand;
            jumps += e == BodyEvent::kJump;
        }
    };
    run(1.70, 2.0);  // warm up standing
    EXPECT(std::fabs(b.Standing() - 1.70) < 0.01);
    run(1.50, 1.0);  // 20 cm: a lean, not a crouch
    EXPECT(crouches == 0);
    run(1.20, 1.0);  // 50 cm down
    EXPECT(crouches == 1 && b.Crouched());
    run(1.40, 1.0);  // 30 cm down: still crouched (hysteresis)
    EXPECT(stands == 0 && b.Crouched());
    run(1.70, 1.0);
    EXPECT(stands == 1 && !b.Crouched());
    EXPECT(jumps == 0);  // jump is off
    // Standing never creeps up from a hop.
    b.Configure(true, 35, true);
    for (int i = 0; i < 20; ++i) {
        h += 0.025;  // 1.8 m/s up
        BodyEvent e = b.Update(h, 1.0 / 72);
        jumps += e == BodyEvent::kJump;
    }
    EXPECT(jumps == 1);
    EXPECT(b.Standing() < 1.72);
    // Crouch off: no events.
    PhysicalBody off;
    off.Configure(false, 35, false);
    int events = 0;
    for (int i = 0; i < 300; ++i) events += off.Update(i < 150 ? 1.7 : 1.0, 1.0 / 72) != BodyEvent::kNone;
    EXPECT(events == 0);
    // A measured standing height is kept.
    PhysicalBody m;
    m.SetStanding(1.80);
    m.Configure(true, 35, false);
    int mc = 0;
    for (int i = 0; i < 300; ++i) mc += m.Update(1.40, 1.0 / 72) == BodyEvent::kCrouch;
    EXPECT(mc == 1);
}

static FQuat Yaw(double deg) { return ToQuat(FRotator{0, deg, 0}); }

static void TestAimSmoother() {
    AimSmoother a;
    // Off: passes straight through.
    EXPECT(AngleDeg(a.Step(Yaw(10), 1.0 / 72, 0.0), Yaw(10)) < 1e-6);
    // Small fast tremor (+-0.3 deg at 10 Hz) is reduced when on.
    AimSmoother on;
    double worst = 0;
    on.Step(Yaw(0), 1.0 / 72, 1.0);
    for (int i = 1; i < 200; ++i) {
        double t = i / 72.0;
        FQuat target = Yaw(0.3 * std::sin(t * 2 * kPi * 2));
        FQuat out = on.Step(target, 1.0 / 72, 1.0);
        if (i > 50) worst = std::fmax(worst, AngleDeg(out, Yaw(0)));
    }
    EXPECT(worst < 0.3);
    // A fast swing (180 deg/s) is followed closely.
    AimSmoother swing;
    swing.Step(Yaw(0), 1.0 / 72, 1.0);
    FQuat out;
    for (int i = 1; i <= 36; ++i) out = swing.Step(Yaw(i * 2.5), 1.0 / 72, 1.0);
    EXPECT(AngleDeg(out, Yaw(90)) < 1.0);
    // A big jump snaps.
    EXPECT(AngleDeg(swing.Step(Yaw(-90), 1.0 / 72, 1.0), Yaw(-90)) < 1e-6);
}

static void TestToggleGrip() {
    // Hold mode: the button as it is.
    GripView v = ToggleGrip(false, true, false, false, true);
    EXPECT(!v.down && v.released);
    // Toggle, not holding: a squeeze grabs.
    v = ToggleGrip(true, false, true, true, false);
    EXPECT(v.down && v.pressed && !v.released);
    // Toggle, holding: letting go of the button keeps it held.
    v = ToggleGrip(true, true, false, false, true);
    EXPECT(v.down && !v.released);
    // Next squeeze lets go.
    v = ToggleGrip(true, true, true, true, false);
    EXPECT(!v.down && !v.pressed && v.released);
}

static void TestCameraHeight() {
    const double dt = 1.0 / 72;
    auto run = [&](CameraHeight& c, double rel, double seconds, bool crouched = false, bool real = false,
                   bool still = true) {
        double r = 0;
        for (double t = 0; t < seconds; t += dt) r = c.Update(rel, crouched, false, still, real, dt);
        return r;
    };
    // Learns standing height once the camera holds still.
    CameraHeight c;
    run(c, 152, 1.5);
    EXPECT(c.HaveStanding() && std::fabs(c.Standing() - 152) < 0.01);
    // A short dip (a vault, a hit) is left alone.
    EXPECT(run(c, 130, 0.8) == 0.0);
    EXPECT(run(c, 152, 0.5) == 0.0);
    // A drop with no crouch that stays while standing still: after 2 s the
    // view is held up.
    run(c, 127, 1.9);
    EXPECT(!c.Holding());
    double r = run(c, 127, 1.5);
    EXPECT(c.Holding() && std::fabs(r - 25) < 1.0);
    // Once held, moving keeps it held while the camera stays low.
    EXPECT(run(c, 127, 1.0, false, false, false) > 24.0);
    // Back to normal: the raise goes away.
    EXPECT(run(c, 152, 2.0) < 0.1);
    // A stick crouch is a real crouch: never undone in character height.
    CameraHeight k;
    run(k, 152, 1.5);
    k.Update(152, false, true, true, false, dt);  // crouch pressed
    EXPECT(run(k, 112, 3.0) == 0.0);
    // Standing up again (second press), back at standing: presses forgotten.
    k.Update(112, false, true, true, false, dt);
    run(k, 152, 1.0);
    EXPECT(k.CrouchPresses() == 0);
    // Crouch then stand pressed but the camera stays down: held up.
    k.Update(152, false, true, true, false, dt);
    run(k, 120, 0.2);
    k.Update(120, false, true, true, false, dt);
    EXPECT(run(k, 120, 3.5) > 25.0);
    // The character's crouch flag: no raise in character height...
    CameraHeight f;
    run(f, 152, 1.5);
    EXPECT(run(f, 112, 3.0, true) == 0.0);
    // ...but in real height every drop is given back (your head crouches).
    CameraHeight g;
    run(g, 152, 1.5, false, true);
    EXPECT(std::fabs(run(g, 112, 2.0, true, true) - 40) < 1.0);
    // The standing height never learns a low camera.
    EXPECT(std::fabs(c.Standing() - 152) < 0.01);
    // Sprinting lowers the game camera on purpose: never held while moving,
    // and the moment the character stops the camera comes back up.
    CameraHeight s;
    run(s, 152, 1.5);
    EXPECT(run(s, 128, 6.0, false, false, false) == 0.0);
    EXPECT(!s.Holding());
    EXPECT(run(s, 152, 0.5) == 0.0);
    // A brief lift (an animation) is not learned as standing.
    CameraHeight h;
    run(h, 152, 1.5);
    run(h, 175, 0.5);
    EXPECT(std::fabs(h.Standing() - 152) < 0.01);
}

int main() {
    TestTexture();
    TestLevels();
    TestSmoother();
    TestPhysicalBody();
    TestAimSmoother();
    TestToggleGrip();
    TestCameraHeight();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("comfort: all tests passed\n");
    return 0;
}
