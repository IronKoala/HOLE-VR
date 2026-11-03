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
        {   // rear sight pinch sphere
            SlideConfig q = c;
            q.sight_center = FVector{-9, 0, 5};
            q.sight_radius = 7.0;
            EXPECT(Slide(q).InZone(FVector{-12, 0, 10}));
            EXPECT(!Slide(c).InZone(FVector{-12, 0, 10}));
        }
        {   // measured slide span: rear end at x -12, front end at x +8, top z 4
            SlideConfig m = Pistol();
            m.use_span = true;
            m.span_rear = FVector{-12, 0, 4};
            m.span_front = FVector{8, 0, 4};
            m.sight_center = FVector{-10, 0, 5};
            m.sight_radius = 6.5;
            Slide z(m);
            EXPECT(z.InZone(FVector{-11.4, -4.0, 0.5}));   // owner's rear overhand attempts
            EXPECT(z.InZone(FVector{-13.1, -3.8, 0.2}));
            EXPECT(z.InZone(FVector{6, 3, 6}));           // front
            EXPECT(!z.InZone(FVector{-18, 0, 4}));         // well behind the rear end and sight
            EXPECT(!z.InZone(FVector{-26.5, 0, 4.1}));     // the old ADS eye point
            EXPECT(!z.InZone(FVector{-6, 0, -6}));         // under the slide (support hand)
            EXPECT(!z.InZone(FVector{11, 0, 4}));          // past the muzzle
            EXPECT(z.InZone(FVector{-14, 0, 8}));          // pinch at the rear sight
        }
        {   // pinch capsule from the owner's 20:58 run: sight (-3.1 -0.8 3.7), 6.5 cm, 0.64 cm back
            SlideConfig m = Pistol();
            m.use_span = true;
            m.span_rear = FVector{-6.8, -1.3, 2.4};
            m.span_front = FVector{8.2, 1.2, 3.3};
            m.sight_center = FVector{-3.1, -0.8, 3.7};
            m.sight_radius = 6.5;
            m.sight_back = 0.64;
            Slide z(m);
            EXPECT(z.InZone(FVector{-10.0, 0.5, 4.1}));    // missed in that run, now inside
            EXPECT(z.InZone(FVector{-9.7, -1.7, 1.6}));
            EXPECT(!z.InZone(FVector{-13.4, -0.1, 0.5}));  // well behind: still outside
        }
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
        Slide s(Pistol());   // (grabbed on the line, so it is also far from where it grabbed)
        s.Update(FVector{-6, 0, 2}, false, 0.011);
        s.Update(FVector{-6, 0, 2}, true, 0.011);
        EXPECT(s.Update(FVector{-7, 15, 2}, true, 0.011) == SlideEvent::kSlipped);
        EXPECT(!s.Held());
    }
    {   // a grab at the edge of the zone, 7 cm beside the slide, keeps hold
        // through a few cm of sideways wobble while it pulls (before
        // 2026-11-03 it slipped once it was 9 cm from the slide's line)
        SlideConfig c = Pistol();
        c.grab_radius = 8.0;
        Slide s(c);
        s.Update(FVector{-6, -7, 2}, false, 0.011);
        EXPECT(s.Update(FVector{-6, -7, 2}, true, 0.011) == SlideEvent::kGrabbed);
        EXPECT(s.Stats().start_off_axis > 6.9 && s.Stats().start_off_axis < 7.1);
        bool back = false;
        for (int i = 1; i <= 20; ++i) {
            SlideEvent e = s.Update(FVector{-6 - 5.0 * i / 20, -7 - 3.0 * i / 20, 2}, true, 0.011);
            EXPECT(e != SlideEvent::kSlipped);
            if (e == SlideEvent::kReachedBack) back = true;
        }
        EXPECT(back);
        EXPECT(s.Stats().max_pull > 4.9 && s.Stats().max_pull < 5.1);
        EXPECT(s.Stats().max_drift > 2.9 && s.Stats().max_drift < 3.1);
        EXPECT(!s.Stats().slipped);
        EXPECT(s.Update(FVector{-11, -10, 2}, false, 0.011) == SlideEvent::kRacked);
    }
    {   // ...but a hand that really leaves (far from the slide and from where
        // it grabbed) still slips, and the stats say so
        Slide s(Pistol());
        s.Update(FVector{-6, 0, 2}, false, 0.011);
        s.Update(FVector{-6, 0, 2}, true, 0.011);
        s.Update(FVector{-6.5, 0, 2}, true, 0.011);
        EXPECT(s.Update(FVector{-7, 15, 2}, true, 0.011) == SlideEvent::kSlipped);
        EXPECT(s.Stats().slipped);
        EXPECT(s.Stats().max_pull > 0.9 && s.Stats().max_pull < 1.1);   // includes the frame it slipped on
        EXPECT(s.Stats().frames == 2);
    }
    {   // hold stats: pushed forward, then pulled back a little, let go
        Slide s(Pistol());
        s.Update(FVector{-6, 0, 2}, false, 0.011);
        s.Update(FVector{-6, 0, 2}, true, 0.011);
        s.Update(FVector{-4, 0, 2}, true, 0.011);
        s.Update(FVector{-7, 0, 2}, true, 0.011);
        EXPECT(s.Update(FVector{-7, 0, 2}, false, 0.011) == SlideEvent::kPartialRelease);
        EXPECT(s.Stats().min_pull < -1.9 && s.Stats().min_pull > -2.1);
        EXPECT(s.Stats().max_pull > 0.9 && s.Stats().max_pull < 1.1);
        EXPECT(!s.Stats().slipped);
    }
    {   // the caller's own zone (visible fingers at the rear sight) accepts a
        // press that the slide's zone alone would not, for that press only
        Slide s(Pistol());
        s.Update(FVector{-20, 0, 2}, false, 0.011);
        s.AcceptNextPress(true);
        EXPECT(s.Update(FVector{-20, 0, 2}, true, 0.011) == SlideEvent::kGrabbed);
        EXPECT(s.Update(FVector{-20, 0, 2}, false, 0.011) == SlideEvent::kPartialRelease);
        EXPECT(s.Update(FVector{-20, 0, 2}, true, 0.011) == SlideEvent::kNone);
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
