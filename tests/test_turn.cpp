// Native unit tests for the turn stick (vr/turn_math.h): every Turn style
// on the VR tab does what its description says.
//   g++ -std=c++20 -Isrc tests/test_turn.cpp -o /tmp/test_turn && /tmp/test_turn
#include <cmath>
#include <cstdio>
#include <initializer_list>

#include "vr/turn_math.h"

using namespace holevr::vr::turn;

static int failures = 0;
#define EXPECT(c)                                                     \
    do {                                                              \
        if (!(c)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);  \
            ++failures;                                               \
        }                                                             \
    } while (0)

static bool Near(double a, double b) { return std::fabs(a - b) < 1e-9; }

int main() {
    EXPECT(StyleFromMode(1) == Style::kSmooth);
    EXPECT(StyleFromMode(0) == Style::kSnap);
    EXPECT(StyleFromMode(2) == Style::kOff);
    EXPECT(StyleFromMode(7) == Style::kSmooth);

    const double dt = 1.0 / 72.0;
    bool armed = true;
    // Smooth: full stick for one second turns the set speed; both ways;
    // inside the dead zone nothing.
    Settings smooth{Style::kSmooth, 30.0, 120.0, 0.15};
    double sum = 0;
    for (int i = 0; i < 72; ++i) sum += Step(smooth, 1.0, dt, &armed);
    EXPECT(Near(sum, 120.0));
    EXPECT(Step(smooth, -1.0, dt, &armed) < 0);
    EXPECT(Step(smooth, 0.1, dt, &armed) == 0.0);
    EXPECT(Near(Step(smooth, 0.5, 1.0, &armed), 60.0));  // half stick, half speed

    // Snap: one flick, one step, however long it is held; again only after
    // the stick comes back; left turns left.
    Settings snap{Style::kSnap, 45.0, 90.0, 0.15};
    armed = true;
    sum = 0;
    for (int i = 0; i < 30; ++i) sum += Step(snap, 0.9, dt, &armed);
    EXPECT(Near(sum, 45.0));
    EXPECT(Step(snap, 0.5, dt, &armed) == 0.0);  // between 0.4 and 0.7: still not armed
    EXPECT(Step(snap, 0.9, dt, &armed) == 0.0);
    EXPECT(Step(snap, 0.0, dt, &armed) == 0.0);  // back to centre: armed
    EXPECT(Near(Step(snap, -0.9, dt, &armed), -45.0));
    EXPECT(Step(snap, 0.6, dt, &armed) == 0.0);  // a small push never snaps

    // Off: no turn at any push, and a snap is ready if the style changes.
    Settings off{Style::kOff, 30.0, 240.0, 0.15};
    armed = false;
    for (double x : {-1.0, -0.8, 0.3, 0.9, 1.0}) EXPECT(Step(off, x, dt, &armed) == 0.0);
    EXPECT(armed);

    // A broken stick reading never turns.
    EXPECT(Step(smooth, NAN, dt, &armed) == 0.0);

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("turn: all tests passed\n");
    return 0;
}
