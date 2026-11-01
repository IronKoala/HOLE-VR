// Native unit tests for the slide interaction (no game, no Windows).
//   g++ -std=c++20 -Isrc tests/test_slide.cpp src/vr/interact/slide.cpp -o /tmp/test_slide && /tmp/test_slide
#include <cstdio>
#include <cstdlib>

#include "vr/interact/slide.h"

using namespace holevr::vr;
using namespace holevr::vr::interact;

static int failures = 0;
#define EXPECT(c)                                                     \
    do {                                                              \
        if (!(c)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);  \
            ++failures;                                               \
        }                                                             \
    } while (0)

static SlideConfig Pistol() {
    SlideConfig c;
    c.grab_center = FVector{-6, 0, 2};
    c.axis = FVector{-1, 0, 0};
    c.length = 4.7;
    return c;
}

// Pull from the grab point to `cm` back over `steps` frames, then let go.
static SlideEvent Stroke(Slide& s, double cm, bool* reached_back, int steps = 20) {
    FVector start{-6, 0, 2};
    EXPECT(s.Update(start, false, 0.011) == SlideEvent::kNone);
    EXPECT(s.Update(start, true, 0.011) == SlideEvent::kGrabbed);
    *reached_back = false;
    for (int i = 1; i <= steps; ++i) {
        SlideEvent e = s.Update(FVector{-6 - cm * i / steps, 0, 2}, true, 0.011);
        if (e == SlideEvent::kReachedBack) *reached_back = true;
    }
    return s.Update(FVector{-6 - cm, 0, 2}, false, 0.011);
}

int main() {
    {   // full rack
        Slide s(Pistol());
        bool back;
        EXPECT(Stroke(s, 5.0, &back) == SlideEvent::kRacked);
        EXPECT(back);
        EXPECT(s.Peak() == 1.0);
    }
    {   // partial rack / chamber check: about half way, nothing cycles
        Slide s(Pistol());
        bool back;
        EXPECT(Stroke(s, 2.3, &back) == SlideEvent::kPartialRelease);
        EXPECT(!back);
        EXPECT(s.Peak() > 0.45 && s.Peak() < 0.52);
    }
    {   // slide springs closed after release
        Slide s(Pistol());
        bool back;
        Stroke(s, 2.3, &back);
        for (int i = 0; i < 10; ++i) s.Update(FVector{0, 0, 0}, false, 0.011);
        EXPECT(s.Travel() == 0.0);
    }
    {   // grabbing outside the zone does nothing
        Slide s(Pistol());
        s.Update(FVector{20, 0, 0}, false, 0.011);
        EXPECT(s.Update(FVector{20, 0, 0}, true, 0.011) == SlideEvent::kNone);
        EXPECT(!s.Held());
    }
    {   // holding grab before entering the zone does not grab (needs a fresh press)
        Slide s(Pistol());
        s.Update(FVector{20, 0, 0}, true, 0.011);
        EXPECT(s.Update(FVector{-6, 0, 2}, true, 0.011) == SlideEvent::kNone);
    }
    {   // hand pulled off the axis slips
        Slide s(Pistol());
        s.Update(FVector{-6, 0, 2}, false, 0.011);
        s.Update(FVector{-6, 0, 2}, true, 0.011);
        EXPECT(s.Update(FVector{-7, 15, 2}, true, 0.011) == SlideEvent::kSlipped);
        EXPECT(!s.Held());
    }
    {   // pushing forward never goes past closed
        Slide s(Pistol());
        s.Update(FVector{-6, 0, 2}, false, 0.011);
        s.Update(FVector{-6, 0, 2}, true, 0.011);
        s.Update(FVector{0, 0, 2}, true, 0.011);
        EXPECT(s.Travel() == 0.0);
    }
    {   // reached back then pushed forward while held still racks on release
        Slide s(Pistol());
        s.Update(FVector{-6, 0, 2}, false, 0.011);
        s.Update(FVector{-6, 0, 2}, true, 0.011);
        EXPECT(s.Update(FVector{-11, 0, 2}, true, 0.011) == SlideEvent::kReachedBack);
        s.Update(FVector{-6, 0, 2}, true, 0.011);
        EXPECT(s.Update(FVector{-6, 0, 2}, false, 0.011) == SlideEvent::kRacked);
    }
    {   // locked open: a short pull past the catch counts as a rack
        SlideConfig c = Pistol();
        double rest = 3.6 / 4.7;
        c.full_threshold = rest + 0.08;
        Slide s(c);
        s.SetRest(rest);
        s.Update(FVector{-6, 0, 2}, false, 0.011);
        EXPECT(s.Travel() == rest);
        EXPECT(s.Update(FVector{-6, 0, 2}, true, 0.011) == SlideEvent::kGrabbed);
        EXPECT(s.Travel() == rest);
        EXPECT(s.Update(FVector{-6.6, 0, 2}, true, 0.011) == SlideEvent::kReachedBack);
        EXPECT(s.Update(FVector{-6.6, 0, 2}, false, 0.011) == SlideEvent::kRacked);
    }
    {   // locked open and let go without pulling: stays on the catch
        SlideConfig c = Pistol();
        double rest = 3.6 / 4.7;
        c.full_threshold = rest + 0.08;
        Slide s(c);
        s.SetRest(rest);
        s.Update(FVector{-6, 0, 2}, false, 0.011);
        s.Update(FVector{-6, 0, 2}, true, 0.011);
        s.Update(FVector{-3, 0, 2}, true, 0.011);  // pushing forward does nothing
        EXPECT(s.Travel() == rest);
        EXPECT(s.Update(FVector{-3, 0, 2}, false, 0.011) == SlideEvent::kPartialRelease);
        for (int i = 0; i < 10; ++i) s.Update(FVector{0, 0, 0}, false, 0.011);
        EXPECT(s.Travel() == rest);
    }
    std::printf(failures ? "%d failure(s)\n" : "all slide tests passed\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
