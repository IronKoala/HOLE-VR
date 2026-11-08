#include <cstdio>
#include "vr/input_edges.h"
#include "vr/interact/slide.h"

using holevr::vr::input::PendingButtonEdges;
using holevr::vr::input::GameplayButtonDown;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

int main() {
    CHECK(GameplayButtonDown(true, {true, true}));   // ordinary release/repress remains held
    CHECK(GameplayButtonDown(true, {true, false}));  // new gameplay press
    CHECK(GameplayButtonDown(true, {false, false})); // unchanged physical hold
    CHECK(!GameplayButtonDown(true, {false, true})); // UI retired the new press
    CHECK(!GameplayButtonDown(false, {true, true})); // complete gameplay tap is up
    CHECK(!GameplayButtonDown(false, {false, true}));
    for (int hand = 0; hand < 2; ++hand) for (int button = 0; button < 6; ++button) {
        PendingButtonEdges q;
        // A complete click occurs while the game is paused. UI closes the menu
        // before the next game tick: only the cleanup release may reach gameplay.
        q.Observe(hand, button, false, true);
        q.Observe(hand, button, true, false);
        q.DiscardPresses();
        const auto resumed = q.Take(hand, button);
        CHECK(!resumed.pressed && resumed.released);
        const auto idle = q.Take(hand, button);
        CHECK(!idle.pressed && !idle.released);

        // A UI click still held on resume must not become a fresh gameplay press.
        q.Observe(hand, button, false, true);
        q.DiscardPresses();
        q.Observe(hand, button, true, true);
        const auto held = q.Take(hand, button);
        CHECK(!held.pressed && !held.released);
        q.Observe(hand, button, true, false);
        const auto released = q.Take(hand, button);
        CHECK(!released.pressed && released.released);

        // A genuinely new press after closing the menu works normally.
        q.Observe(hand, button, false, true);
        CHECK(q.Take(hand, button).pressed);

        // Outside menus, quick taps still preserve both edges (existing contract).
        q.Observe(hand, button, true, false);
        q.Observe(hand, button, false, true);
        const auto quick = q.Take(hand, button);
        CHECK(quick.pressed && quick.released);
    }
    {
        PendingButtonEdges q;
        // All six buttons on both hands are drained together, not only the
        // pointer trigger: queued grip/weapon-switch/menu presses also belong to UI.
        for (int h=0;h<2;++h) for(int b=0;b<6;++b) {
            q.Observe(h,b,false,true);
            if (b%2) q.Observe(h,b,true,false);
        }
        q.DiscardPresses();
        q.DiscardPresses(); // both menu readers may observe the same snapshot
        for (int h=0;h<2;++h) for(int b=0;b<6;++b) {
            const auto e=q.Take(h,b);
            CHECK(!e.pressed && e.released == bool(b%2));
        }
    }
    {
        // Integration with the current slide admission gate: holding a grip
        // that began in the menu is not a fresh slide squeeze on resume.
        PendingButtonEdges q;
        holevr::vr::interact::SqueezeGate slide;
        q.Observe(0,5,false,true);
        q.DiscardPresses();
        const auto menu_grip=q.Take(0,5);
        CHECK(!slide.Update(menu_grip.pressed,true,true));
        CHECK(slide.Blocked());
        q.Observe(0,5,true,false);
        const auto release=q.Take(0,5);
        CHECK(release.released && !slide.Update(release.pressed,false,true));
        q.Observe(0,5,false,true);
        CHECK(slide.Update(q.Take(0,5).pressed,true,true));
    }
    std::printf("input menu edges: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}
