// clang++ -std=c++20 -Isrc tests/test_menu_input.cpp src/vr/interact/slide.cpp -o test_menu_input
#include <cstdio>
#include "vr/input_edges.h"
#include "vr/interact/slide.h"

using namespace holevr::vr;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

struct Hand {
    input::PendingButtonEdges queue;
    interact::SqueezeGate gate;
    interact::Slide slide;
    bool raw_down = false;

    void Sample(bool down) {
        queue.Observe(0,5,raw_down,down);
        raw_down=down;
    }
    interact::SlideEvent GameTick(bool modal = false) {
        auto edges=queue.Take(0,5);
        bool down=input::GameplayButtonDown(raw_down,edges);
        // controls observes grip-up before its menu/editor early branch.
        if(!down) gate.Update(false,false,false);
        if(modal) return interact::SlideEvent::kNone;
        return slide.Update({0,0,0},gate.Update(edges.pressed,down,true),.011);
    }
};

int main() {
    {
        Hand h;
        h.GameTick();
        h.Sample(true);
        CHECK(h.GameTick()==interact::SlideEvent::kGrabbed);
        CHECK(h.slide.Held());
        // While paused, the old gameplay squeeze ends and a new menu squeeze
        // starts. The UI consumes the new press before resuming the game.
        h.Sample(false);
        h.Sample(true);
        h.queue.DiscardPresses();
        CHECK(h.GameTick()==interact::SlideEvent::kPartialRelease);
        CHECK(!h.slide.Held());
        CHECK(h.raw_down); // Peek/physical input was not rewritten
        for(int i=0;i<5;++i) {
            CHECK(h.GameTick()==interact::SlideEvent::kNone);
            CHECK(!h.slide.Held()); // next tick restores raw down, no new acquisition
        }
        h.Sample(false);
        h.GameTick();
        h.Sample(true);
        CHECK(h.GameTick()==interact::SlideEvent::kGrabbed);
        CHECK(h.slide.Held());
    }
    {
        Hand h;
        // No previous gameplay hold: a grip first pressed in a menu also stays
        // spent while held across resume, then a fresh gameplay squeeze works.
        h.Sample(true);
        h.queue.DiscardPresses();
        for(int i=0;i<5;++i) {
            CHECK(h.GameTick()==interact::SlideEvent::kNone);
            CHECK(!h.slide.Held());
        }
        h.Sample(false);
        h.GameTick();
        h.Sample(true);
        CHECK(h.GameTick()==interact::SlideEvent::kGrabbed);
    }
    {
        Hand h;
        h.Sample(true);
        CHECK(h.GameTick()==interact::SlideEvent::kGrabbed);
        // Some modal menus/editor frames continue ticking the player. The
        // release pulse is consumed during the modal state, before gameplay
        // resumes; it must still invalidate the old squeeze admission.
        h.Sample(false);
        h.Sample(true);
        h.queue.DiscardPresses();
        h.GameTick(true);
        CHECK(h.slide.Held()); // no advance or ammo cycle inside the modal UI
        h.GameTick(true);      // raw level is down again, press remains spent
        CHECK(h.GameTick()==interact::SlideEvent::kPartialRelease);
        CHECK(!h.slide.Held());
        for(int i=0;i<5;++i) {
            CHECK(h.GameTick()==interact::SlideEvent::kNone);
            CHECK(!h.slide.Held());
        }
        h.Sample(false);
        h.GameTick();
        h.Sample(true);
        CHECK(h.GameTick()==interact::SlideEvent::kGrabbed);
    }
    {
        input::PendingButtonEdges q;
        q.Observe(0,5,true,false);
        q.Observe(0,5,false,true);
        q.DiscardPresses();
        auto edges=q.Take(0,5);
        bool held_attachment=true,held_drink=true;
        const bool down=input::GameplayButtonDown(true,edges);
        if(!down) held_attachment=held_drink=false;
        CHECK(!held_attachment && !held_drink);
        // Both acquisition sites require pressed. A still-down next snapshot
        // cannot take an attachment or can after the old hold was cleaned up.
        edges=q.Take(0,5);
        CHECK(input::GameplayButtonDown(true,edges));
        CHECK(!edges.pressed);
    }
    std::printf("menu input/slide boundary: %s (%d failures)\n",failures?"FAILED":"passed",failures);
    return failures?1:0;
}
