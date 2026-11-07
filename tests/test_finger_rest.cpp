// Native unit tests for fingers giving way against a surface (vr/body/finger_rest).
//   g++ -std=c++20 -Isrc tests/test_finger_rest.cpp src/vr/body/finger_rest.cpp src/vr/body/finger_solve.cpp
//       src/vr/body/skeleton.cpp src/vr/body/ik_math.cpp -o /tmp/test_finger_rest && /tmp/test_finger_rest
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "vr/body/finger_rest.h"
#include "vr/body/ik_math.h"

using namespace holevr::vr;
using namespace holevr::vr::ik;
using skel::Xf;

static int failures = 0;
#define EXPECT(c)                                                     \
    do {                                                              \
        if (!(c)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                               \
        }                                                             \
    } while (0)

static Xf X(const FVector& t, const FQuat& r = FQuat{}) {
    Xf x;
    x.rotation = r;
    x.translation = t;
    return x;
}

// A middle finger like the game's right hand (UE4 mannequin): the finger runs
// along -X from a knuckle 12 cm out, the palm faces +Y.
static fingers::Chain Finger() {
    const FVector palm{0, 1, 0}, knuckle{-12.2, 1.3, -0.6}, pinky{-10.1, 2.3, -4.6};
    const FVector dir{-1, 0, 0};
    Xf j[3];
    j[0] = X(knuckle);
    j[1] = X(knuckle + dir * 4.6);
    j[2] = X(j[1].translation + dir * 3.4);
    return fingers::Build(j, palm, pinky, false);
}

int main() {
    const fingers::Chain c = Finger();
    EXPECT(c.valid);
    const fingers::Limits lim = fingers::DefaultLimits(false);
    const Xf hand = X(FVector{0, 0, 0});
    auto depth_at = [&](double b, const FVector& p, const FVector& n) {
        return fingers::PlaneDepth(c, fingers::Bend(c, b, lim), hand, p, n, 0.5);
    };

    // The bend path: 0 is straight, positive is Curl, negative bends back
    // (mostly at the knuckle) and moves the fingertip to the back of the hand.
    {
        const fingers::Angles s = fingers::Bend(c, 0.0, lim);
        EXPECT(s.flex[0] == 0.0 && s.flex[1] == 0.0 && s.flex[2] == 0.0);
        const fingers::Angles b = fingers::Bend(c, -1.0, lim);
        EXPECT(b.flex[0] < 0.0 && b.flex[0] < b.flex[1] && b.flex[0] < b.flex[2]);
        EXPECT(std::fabs(b.flex[0] - lim.flex_min[0] * kDegToRad) < 1e-9);
        const fingers::Angles k = fingers::Bend(c, 0.4, lim), want = fingers::Curl(c, 0.4, lim);
        EXPECT(std::fabs(k.flex[1] - want.flex[1]) < 1e-12);
        const FVector tip0 = fingers::TipWorld(c, s, hand), tipb = fingers::TipWorld(c, b, hand);
        EXPECT(tipb.Y < tip0.Y - 2.0);  // the palm faces +Y: bent back goes -Y
    }

    // Nothing near: the finger goes all the way to the wanted bend.
    {
        const fingers::Push p =
            fingers::PushFromPlane(c, lim, hand, 0.1, 0.4, 1.2, FVector{0, 50, 0}, FVector{0, -1, 0}, 0.5);
        EXPECT(std::fabs(p.bend - 0.4) < 1e-9 && p.depth == 0.0 && p.dir == 0 && !p.pushed);
    }

    // Strong curl input and fingertip drag must not bypass joint stops.
    // This also covers reverse drag beyond the backward end of the range.
    for (bool thumb : {false, true}) {
        fingers::Chain joint = c;
        joint.thumb = thumb;
        const auto limits = fingers::DefaultLimits(thumb);
        for (double b : {-3.0, -1.3, 0.0, 1.2, 1.5, 3.0}) {
            const auto a = fingers::Bend(joint, b, limits);
            for (int j = 0; j < 3; ++j) {
                EXPECT(a.flex[j] >= limits.flex_min[j] * kDegToRad - 1e-12);
                EXPECT(a.flex[j] <= limits.flex_max[j] * kDegToRad + 1e-12);
            }
        }
    }

    // Palm 2 cm from a wall: an open finger asked to curl (pad toward the
    // wall) curls until it touches the wall and rests there.
    const FVector wall{0, 3.3, 0}, out{0, -1, 0};
    {
        EXPECT(depth_at(0.0, wall, out) == 0.0 && depth_at(0.6, wall, out) > 0.0);
        const fingers::Push p = fingers::PushFromPlane(c, lim, hand, 0.0, 0.6, 1.2, wall, out, 0.5);
        EXPECT(!p.pushed && p.dir == -1);
        EXPECT(p.bend > 0.0 && p.bend < 0.15);
        EXPECT(p.depth == 0.0 && depth_at(p.bend, wall, out) == 0.0);
        EXPECT(depth_at(p.bend + 0.02, wall, out) > 0.0);
        // Asked to open again, it follows at once.
        const fingers::Push q = fingers::PushFromPlane(c, lim, hand, p.bend, 0.0, 1.2, wall, out, 0.5);
        EXPECT(q.bend == 0.0 && q.dir == 0);
    }

    // The hand pushed onto the wall: the relaxed fingers are in it on their
    // pads, so they bend back past straight (a real hand pushed flat), not
    // into a fist.
    const FVector flat{0, 1.5, 0};
    {
        EXPECT(depth_at(0.15, flat, out) > 0.0 && depth_at(0.0, flat, out) > 0.0);
        const fingers::Push p = fingers::PushFromPlane(c, lim, hand, 0.15, 0.15, 1.2, flat, out, 0.5);
        EXPECT(p.pushed && p.dir == -1);
        EXPECT(p.bend < 0.0);
        EXPECT(p.depth == 0.0);
    }

    // A wall on the back of the fingers (pressed on the nails): they curl.
    const FVector behind{0, 1.0, 0}, back_out{0, 1, 0};
    {
        EXPECT(depth_at(0.0, behind, back_out) > 0.0);
        const fingers::Push p = fingers::PushFromPlane(c, lim, hand, 0.0, 0.0, 1.2, behind, back_out, 0.5);
        EXPECT(p.pushed && p.dir == +1);
        EXPECT(p.bend > 0.0);
        EXPECT(p.depth == 0.0);
    }

    // A non-grid starting bend must still reach the exact backward stop.
    // Place the plane so the final fraction of a 0.05 step is required.
    {
        Xf joints[3];
        FVector tip;
        fingers::Fk(c, fingers::Bend(c, -1.0, lim), joints, &tip);
        const double highest = std::max({joints[1].translation.Y, joints[2].translation.Y, tip.Y});
        const FVector edge{0, highest + 0.5 + 1e-6, 0};
        EXPECT(depth_at(-1.0, edge, out) == 0.0);
        EXPECT(depth_at(-0.97, edge, out) > 0.0);
        const auto p = fingers::PushFromPlane(c, lim, hand, 0.13, 0.13, 1.2, edge, out, 0.5);
        EXPECT(p.pushed && p.dir == -1 && p.bend < -0.99 && p.depth == 0.0);
    }

    // The forward stop also need not be aligned to the sampling grid.
    {
        Xf joints[3];
        FVector tip;
        fingers::Fk(c, fingers::Bend(c, 0.83, lim), joints, &tip);
        const double lowest = std::min({joints[1].translation.Y, joints[2].translation.Y, tip.Y});
        const FVector edge{0, lowest - 0.5 - 1e-6, 0};
        EXPECT(depth_at(0.83, edge, back_out) == 0.0);
        EXPECT(depth_at(0.81, edge, back_out) > 0.0);
        const auto p = fingers::PushFromPlane(c, lim, hand, 0.11, 0.11, 0.83, edge, back_out, 0.5);
        EXPECT(p.pushed && p.dir == +1 && p.bend > 0.82 && p.depth == 0.0);
    }

    // A finger bent back to its limit with the wall on its nails: it can't
    // go further back, so it is pushed the other way, out of the wall.
    {
        EXPECT(depth_at(-1.0, behind, back_out) > 0.0);
        const fingers::Push p = fingers::PushFromPlane(c, lim, hand, -1.0, 0.0, 1.2, behind, back_out, 0.5);
        EXPECT(p.pushed && p.dir == +1 && p.depth == 0.0 && p.bend > -1.0);
    }

    // Fingertips straight into a wall 6 cm past the knuckle. A straight
    // finger bends back; it can't clear a wall that close, so it takes the
    // bend that goes in least on that side.
    const FVector front{-18.2, 0, 0}, front_out{1, 0, 0};
    {
        const double before = depth_at(0.0, front, front_out);
        EXPECT(before > 0.0);
        const fingers::Push p = fingers::PushFromPlane(c, lim, hand, 0.0, 0.0, 1.2, front, front_out, 0.5);
        EXPECT(p.pushed && p.dir == -1);
        EXPECT(p.depth > 0.0 && p.depth <= before);
        for (double b = -1.0; b <= 1e-9; b += 0.05) EXPECT(depth_at(b, front, front_out) >= p.depth - 1e-9);
        // Last frame's side wins a near tie: told it was curling, it curls.
        const fingers::Push q = fingers::PushFromPlane(c, lim, hand, 0.0, 0.0, 1.2, front, front_out, 0.5, +1);
        EXPECT(q.dir == +1);
    }

    // A relaxed (slightly curled) finger pushed tip first buckles further
    // until it clears, as a sprung finger would.
    {
        EXPECT(depth_at(0.12, front, front_out) > 0.0);
        const fingers::Push p = fingers::PushFromPlane(c, lim, hand, 0.12, 0.12, 1.2, front, front_out, 0.5);
        EXPECT(p.pushed && p.dir == +1 && p.bend > 0.12 && p.depth == 0.0);
        // Pushed toward a curl the controller asks for anyway, it carries on
        // to it.
        const fingers::Push q = fingers::PushFromPlane(c, lim, hand, 0.12, 0.9, 1.2, front, front_out, 0.5);
        EXPECT(q.depth == 0.0 && std::fabs(q.bend - 0.9) < 1e-9);
    }

    // The hand's own world transform is used: the same cases moved and
    // turned in the world give the same bends.
    {
        const Xf moved = X(FVector{100, -40, 90}, ToQuat(FRotator{10, 70, -20}));
        for (const FVector* w : {&wall, &flat}) {
            const fingers::Push here = fingers::PushFromPlane(c, lim, hand, 0.15, 0.6, 1.2, *w, out, 0.5);
            const fingers::Push there = fingers::PushFromPlane(c, lim, moved, 0.15, 0.6, 1.2, skel::Apply(moved, *w),
                                                               moved.rotation.Rotate(out), 0.5);
            EXPECT(std::fabs(here.bend - there.bend) < 1e-6 && here.dir == there.dir);
        }
    }

    // Contact must survive the final render-pose easing. A newly pressed
    // palm has a clear solved pose but ordinary blending puts it back in.
    // Exercise common headset frame rates and transformed contact planes.
    for (double hz : {72.0, 90.0, 120.0}) {
        const auto p = fingers::PushFromPlane(c, lim, hand, 0.15, 0.15, 1.2, flat, out, 0.5);
        const auto current = fingers::Bend(c, 0.15, lim);
        const auto solved = fingers::Bend(c, p.bend, lim);
        const double ease = 1.0 - std::exp(-1.0 / (hz * 0.04));
        const auto legacy = fingers::Blend(current, solved, ease);
        const double legacy_depth = fingers::PlaneDepth(c, legacy, hand, flat, out, 0.5);
        EXPECT(legacy_depth > 0.5);
        const auto safe = fingers::BlendOnPlane(c, current, solved, ease, hand, flat, out, 0.5);
        EXPECT(fingers::PlaneDepth(c, safe, hand, flat, out, 0.5) <= 1e-6);
        const Xf moved = X(FVector{20, -10, 60}, ToQuat(FRotator{20, 70, -30}));
        const auto rotated = fingers::BlendOnPlane(c, current, solved, ease, moved,
                            skel::Apply(moved, flat), moved.rotation.Rotate(out), 0.5);
        for (int j = 0; j < 3; ++j) EXPECT(std::fabs(safe.flex[j] - rotated.flex[j]) < 1e-9);
        std::printf("contact at %.0f Hz: old easing %.3f cm inside, corrected %.6f cm\n", hz,
                    legacy_depth, fingers::PlaneDepth(c, safe, hand, flat, out, 0.5));

        // An unreachable contact keeps the solver's best residual instead
        // of worsening it for visual smoothness.
        const auto hard = fingers::PushFromPlane(c, lim, hand, 0.0, 0.0, 1.2, front, front_out, 0.5);
        const auto hard_pose = fingers::Bend(c, hard.bend, lim);
        const auto partial = fingers::BlendOnPlane(c, fingers::Bend(c, 0.0, lim), hard_pose, ease,
                                                   hand, front, front_out, 0.5);
        EXPECT(fingers::PlaneDepth(c, partial, hand, front, front_out, 0.5) <= hard.depth + 1e-6);

        // Clear movement still eases, including release away from the wall.
        const FVector far{0, 50, 0};
        const auto smooth = fingers::BlendOnPlane(c, current, solved, ease, hand, far, out, 0.5);
        for (int j = 0; j < 3; ++j) EXPECT(std::fabs(smooth.flex[j] - legacy.flex[j]) < 1e-12);
    }

    // Drag: asked to move the fingertip toward the palm, the finger curls;
    // sideways, it spreads; the tip ends up moved the asked way; big asks are
    // clamped.
    {
        double db = 0, ds = 0;
        fingers::DragTip(c, lim, hand, 0.1, FVector{0, 0.5, 0}, &db, &ds);
        EXPECT(db > 0.0);
        const FVector side = Normalize(Cross(FVector{-1, 0, 0}, FVector{0, 1, 0}));  // across the finger
        fingers::DragTip(c, lim, hand, 0.1, side * 0.8, &db, &ds);
        EXPECT(std::fabs(ds) > 0.01);
        fingers::Angles a = fingers::Bend(c, 0.1 + db, lim);
        a.spread = ds;
        const FVector t0 = fingers::TipWorld(c, fingers::Bend(c, 0.1, lim), hand), t1 = fingers::TipWorld(c, a, hand);
        EXPECT(Dot(t1 - t0, side) > 0.4);
        fingers::DragTip(c, lim, hand, 0.1, FVector{0, 50, 0}, &db, &ds);
        EXPECT(std::fabs(db) <= 0.3 + 1e-12);
    }

    if (failures) {
        std::printf("finger rest: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("finger rest: all tests passed\n");
    return 0;
}
