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
    {   // front of the slide: grabbable along its length, and a pull from there racks
        SlideConfig c = Pistol();
        c.grab_radius = 8.0;
        c.grab_forward = 16.0;
        c.grab_radius_forward = 6.0;
        Slide s(c);
        EXPECT(s.InZone(FVector{8, 0, 3}));      // 14 cm forward of the rear point, on the slide
        EXPECT(s.InZone(FVector{-6, 7, 2}));     // rear sphere still 8 cm
        EXPECT(!s.InZone(FVector{8, 0, -6}));    // 8 cm below the front: not the slide
        EXPECT(!s.InZone(FVector{17, 0, 2}));    // well past the muzzle end
        FVector f{8, 0, 2};
        s.Update(f, false, 0.011);
        EXPECT(s.Update(f, true, 0.011) == SlideEvent::kGrabbed);
        bool back = false;
        for (int i = 1; i <= 20; ++i)
            if (s.Update(FVector{8 - 5.0 * i / 20, 0, 2}, true, 0.011) == SlideEvent::kReachedBack) back = true;
        EXPECT(back);
        EXPECT(s.Update(FVector{3, 0, 2}, false, 0.011) == SlideEvent::kRacked);
        SlideConfig r = Pistol();   // rear sphere only (default)
        EXPECT(!Slide(r).InZone(FVector{-20, 0, 2}));
        {   // pinch from behind (owner's log: hands 10-15 cm behind the rear point)
            SlideConfig b = c;
            b.grab_back = 15.0;
            Slide p(b);
            EXPECT(p.InZone(FVector{-17.6, -0.8, 0.5}));
            EXPECT(p.InZone(FVector{-21.0, 1.0, 2.0}));
            EXPECT(!p.InZone(FVector{-14, 0, -8}));   // support hand under the grip: not the slide
            EXPECT(!p.InZone(FVector{-28, 0, 2}));
            FVector h{-18, 0, 2};
            p.Update(h, false, 0.011);
            EXPECT(p.Update(h, true, 0.011) == SlideEvent::kGrabbed);
            bool back = false;
            for (int i = 1; i <= 20; ++i)
                if (p.Update(FVector{-18 - 5.0 * i / 20, 0, 2}, true, 0.011) == SlideEvent::kReachedBack) back = true;
            EXPECT(back);
            EXPECT(p.Update(FVector{-23, 0, 2}, false, 0.011) == SlideEvent::kRacked);
        }
        EXPECT(!Slide(r).InZone(FVector{8, 0, 2}));
    }
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
    {
        // AR charging handle with the bolt locked back at 70%: the handle is
        // grabbed forward and must travel to the bolt before the bolt moves.
        SlideConfig c = Pistol();
        c.length = 10.0;
        c.grab_at_closed = true;
        c.full_threshold = 0.78;
        Slide s(c);
        s.SetRest(0.7);
        s.Update(FVector{-6, 0, 2}, false, 0.011);
        EXPECT(s.Travel() == 0.7);
        EXPECT(s.Update(FVector{-6, 0, 2}, true, 0.011) == SlideEvent::kGrabbed);
        EXPECT(s.HandleTravel() == 0.0);
        s.Update(FVector{-9, 0, 2}, true, 0.011);   // handle 30%, bolt still on the catch
        EXPECT(s.HandleTravel() > 0.29 && s.HandleTravel() < 0.31);
        EXPECT(s.Travel() == 0.7);
        EXPECT(s.Update(FVector{-14, 0, 2}, true, 0.011) == SlideEvent::kReachedBack);
        EXPECT(s.Travel() > 0.79);
        EXPECT(s.Update(FVector{-14, 0, 2}, false, 0.011) == SlideEvent::kRacked);
        EXPECT(s.HandleTravel() == 0.0);
    }
    std::printf(failures ? "%d failure(s)\n" : "all slide tests passed\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
