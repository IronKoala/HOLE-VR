// Native unit tests for fingers resting on a surface (vr/body/finger_rest).
//   g++ -std=c++20 -Isrc tests/test_finger_rest.cpp src/vr/body/finger_rest.cpp src/vr/body/finger_solve.cpp
//       src/vr/body/skeleton.cpp src/vr/body/ik_math.cpp -o /tmp/test_finger_rest && /tmp/test_finger_rest
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
    double depth = -1;

    // Nothing near: the wanted curl stands.
    double curl = fingers::CurlClearOfPlane(c, lim, hand, 0.4, 1.2, FVector{0, 50, 0}, FVector{0, -1, 0}, 0.5, &depth);
    EXPECT(std::fabs(curl - 0.4) < 1e-9);
    EXPECT(depth == 0.0);

    // Palm flat on a wall 2 cm in front of it (wall normal points back at the
    // palm, -Y): curling would push the fingers into it, so the hand opens.
    const FVector wall{0, 3.3, 0}, out{0, -1, 0};
    EXPECT(fingers::PlaneDepth(c, fingers::Curl(c, 0.6, lim), hand, wall, out, 0.5) > 0.0);
    curl = fingers::CurlClearOfPlane(c, lim, hand, 0.6, 1.2, wall, out, 0.5, &depth);
    EXPECT(curl < 0.6);
    EXPECT(depth == 0.0);
    EXPECT(fingers::PlaneDepth(c, fingers::Curl(c, curl, lim), hand, wall, out, 0.5) == 0.0);
    // ...and opens no further than it has to: a little more curl goes in.
    EXPECT(fingers::PlaneDepth(c, fingers::Curl(c, curl + 0.05, lim), hand, wall, out, 0.5) > 0.0);

    // Fingers pointed straight into a wall 6 cm past the knuckles (wall
    // across -X, normal +X): an open hand goes in, so the fingers curl back.
    const FVector front{-18.2, 0, 0}, back{1, 0, 0};
    EXPECT(fingers::PlaneDepth(c, fingers::Curl(c, 0.0, lim), hand, front, back, 0.5) > 0.0);
    curl = fingers::CurlClearOfPlane(c, lim, hand, 0.0, 1.2, front, back, 0.5, &depth);
    EXPECT(curl > 0.0);
    EXPECT(depth == 0.0);

    // A wall the finger can never clear (just past the knuckle): the curl
    // that goes in least is used, and the depth says how far.
    const FVector close{-12.4, 0, 0};
    curl = fingers::CurlClearOfPlane(c, lim, hand, 0.0, 1.2, close, back, 0.5, &depth);
    EXPECT(depth > 0.0);
    EXPECT(curl >= 0.0 && curl <= 1.2);
    for (double other = 0.0; other <= 1.2; other += 0.05)
        EXPECT(fingers::PlaneDepth(c, fingers::Curl(c, other, lim), hand, close, back, 0.5) >= depth - 1e-9);

    // The hand's own world transform is used: the same palm-on-wall case
    // moved and turned in the world gives the same curl.
    Xf moved = X(FVector{100, -40, 90}, ToQuat(FRotator{10, 70, -20}));
    const double here = fingers::CurlClearOfPlane(c, lim, hand, 0.6, 1.2, wall, out, 0.5);
    const double there = fingers::CurlClearOfPlane(c, lim, moved, 0.6, 1.2, skel::Apply(moved, wall),
                                                   moved.rotation.Rotate(out), 0.5);
    EXPECT(std::fabs(here - there) < 1e-6);

    if (failures) {
        std::printf("finger rest: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("finger rest: all tests passed\n");
    return 0;
}
