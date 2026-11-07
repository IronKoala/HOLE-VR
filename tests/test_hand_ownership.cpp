// Native tests: clang++ -std=c++20 -Isrc tests/test_hand_ownership.cpp -o test_hand_ownership
#include <cstdio>
#include <initializer_list>
#include "vr/hands.h"

using namespace holevr::vr::hands;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

// Model the production order: controls reserves the raw input, publishes its
// resulting hold, workbench runs, then drinks. The same edge must not acquire
// two objects even when both proximity queries return true.
struct Frame {
    bool weapon = false, attachment = false, drink = false;
    bool near_attachment = false, near_drink = false;
    bool allowed = true, pose_valid = true;
    int workbench_grabs = 0, drink_grabs = 0;
    HandInteraction Run(bool pressed, bool down, bool controls_acquires = false) {
        GripFacts f{weapon, attachment, drink, near_attachment && pose_valid,
                    near_drink && pose_valid, allowed};
        HandInteraction access = ResolveGrip(f);
        if (controls_acquires && access.AllowsControls()) weapon = true;
        access = AfterControls(access, weapon);
        if (attachment) {
            if (!down || access.owner != GripOwner::kWorkbench) attachment = false;
        } else if (pose_valid && pressed && access.CanAcquire(GripOwner::kWorkbench)) {
            attachment = true;
            ++workbench_grabs;
        }
        if (drink) {
            if (!down || access.owner != GripOwner::kDrink) drink = false;
        } else if (pose_valid && pressed && access.CanAcquire(GripOwner::kDrink)) {
            drink = true;
            ++drink_grabs;
        }
        CHECK(int(weapon) + int(attachment) + int(drink) <= 1);
        return access;
    }
};

int main() {
    // Calibration next to a seated pistol must ignore the approach pose,
    // while genuine holds (including another owner's stale visual pose)
    // still block setup. Candidate reservations are not actual item holds.
    {
        HandGrab approach;
        approach.kind = GrabKind::kGunGrip;
        approach.holster_preview = true;
        for (const auto owner : {GripOwner::kNone, GripOwner::kWorkbench, GripOwner::kDrink}) {
            const HandInteraction access{owner, true};
            CHECK(!HasHeldObject(approach, access, false, false));
            CHECK(HasHeldObject(approach, access, true, false));
            CHECK(HasHeldObject(approach, access, false, true));
        }
        CHECK(HasHeldObject(approach, {GripOwner::kControls, true}, false, false));
        approach.holster_preview = false;
        for (const auto kind : {GrabKind::kGunGrip, GrabKind::kSlideGrab, GrabKind::kMagHeld,
                               GrabKind::kMagInsert, GrabKind::kSupportGrip, GrabKind::kHolster}) {
            approach.kind = kind;
            CHECK(HasHeldObject(approach, {GripOwner::kNone, true}, false, false));
        }
        CHECK(!HasHeldObject({}, {GripOwner::kNone, true}, false, false));
    }
    // A visual approach can overlap a can/attachment. Reserving the input
    // still chooses one real feature; the preview cannot block it or become
    // a held gun for the grip editor/workbench. Raw pose data stays intact.
    {
        HandGrab approach;
        approach.kind = GrabKind::kGunGrip;
        approach.object = 123;
        approach.holster_preview = true;
        approach.pose_blend = 0.75;
        CHECK(HeldGrabKind(approach) == GrabKind::kNone);
        CHECK(HeldGrabKind(approach) != GrabKind::kGunGrip);
        Frame both;
        both.near_attachment = both.near_drink = HeldGrabKind(approach) == GrabKind::kNone;
        auto access = both.Run(true, true);
        CHECK(both.attachment && !both.drink && !both.weapon);
        CHECK(access.owner == GripOwner::kWorkbench);
        CHECK(!access.AllowsControls());
        Frame can;
        can.near_drink = HeldGrabKind(approach) == GrabKind::kNone;
        access = can.Run(true, true);
        CHECK(can.drink && !can.weapon && access.owner == GripOwner::kDrink);
        // A true controls hold wins even if a visual snapshot still previews.
        Frame actual;
        actual.weapon = true;
        actual.near_attachment = actual.near_drink = true;
        access = actual.Run(true, true);
        CHECK(actual.weapon && !actual.attachment && !actual.drink);
        CHECK(HasHeldObject(approach, access, false, false));
        CHECK(approach.kind == GrabKind::kGunGrip && approach.object == 123 && approach.pose_blend == 0.75);
        for (const auto kind : {GrabKind::kNone, GrabKind::kGunGrip, GrabKind::kSlideGrab,
                               GrabKind::kMagHeld, GrabKind::kMagInsert, GrabKind::kHolster,
                               GrabKind::kSupportGrip}) {
            HandGrab held;
            held.kind = kind;
            CHECK(HeldGrabKind(held) == kind); // retain every non-preview contract
        }
    }
    struct Case { const char* name; GripFacts facts; GripOwner expected; };
    const Case cases[] = {
        {"empty", {}, GripOwner::kNone},
        {"attachment", {false,false,false,true,false,true}, GripOwner::kWorkbench},
        {"drink", {false,false,false,false,true,true}, GripOwner::kDrink},
        {"overlapping candidates", {false,false,false,true,true,true}, GripOwner::kWorkbench},
        {"held can over attachment", {false,false,true,true,true,true}, GripOwner::kDrink},
        {"held attachment over can", {false,true,false,true,true,true}, GripOwner::kWorkbench},
        {"reload over both", {true,false,false,true,true,true}, GripOwner::kControls},
        {"menu owns empty hand", {false,false,false,true,true,false}, GripOwner::kUi},
        {"menu preserves can for cleanup", {false,false,true,true,true,false}, GripOwner::kDrink},
        {"editor preserves attachment", {false,true,false,true,true,false}, GripOwner::kWorkbench},
        {"lost pose keeps weapon hold", {true,false,false,false,false,true}, GripOwner::kControls},
    };
    for (const Case& c : cases) {
        const auto a = ResolveGrip(c.facts);
        if (a.owner != c.expected) { std::printf("FAIL case: %s\n", c.name); ++failures; }
        CHECK(a.actions_allowed == c.facts.actions_allowed);
        CHECK(!(a.CanAcquire(GripOwner::kWorkbench) && a.CanAcquire(GripOwner::kDrink)));
        if (!c.facts.actions_allowed) {
            CHECK(!a.CanAcquire(GripOwner::kWorkbench));
            CHECK(!a.CanAcquire(GripOwner::kDrink));
            CHECK(!a.AllowsControls());
        }
    }
    // Every supported single-existing-owner x proximity x menu combination.
    for (int held = 0; held < 4; ++held) for (int near = 0; near < 4; ++near)
        for (int allowed = 0; allowed < 2; ++allowed) {
            Frame f;
            f.weapon = held == 1; f.attachment = held == 2; f.drink = held == 3;
            f.near_attachment = near & 1; f.near_drink = near & 2; f.allowed = allowed;
            f.Run(true, true);
            if (held == 1) CHECK(f.weapon);
            if (held == 2) CHECK(f.attachment);
            if (held == 3) CHECK(f.drink);
            if (!allowed) CHECK(f.workbench_grabs == 0 && f.drink_grabs == 0);
        }
    {
        Frame f; f.drink = true; f.near_attachment = true;
        auto access = f.Run(true, true);
        CHECK(f.drink && !f.attachment && !access.AllowsControls());
        f.pose_valid = false;
        f.Run(false, true); // tracking loss must not donate the grip
        CHECK(f.drink);
        f.Run(false, false); // release still frees it
        CHECK(!f.drink);
        f.pose_valid = true;
        f.Run(false, false); // recovery cannot replay a past press
        CHECK(!f.attachment);
        f.Run(true, true);
        CHECK(f.attachment);
    }
    {
        Frame f; f.attachment = true; f.near_drink = true; f.allowed = false;
        f.Run(false, false); // release/cleanup while operating a menu
        CHECK(!f.attachment && !f.drink);
        f.allowed = true;
        f.Run(false, false);
        CHECK(!f.drink);
        f.Run(true, true);
        CHECK(f.drink);
    }
    {
        Frame f; auto access = f.Run(true, true, true);
        CHECK(f.weapon && access.owner == GripOwner::kControls);
        CHECK(!access.CanAcquire(GripOwner::kWorkbench) && !access.CanAcquire(GripOwner::kDrink));
        access = AfterControls(access, false); // same-frame weapon release
        CHECK(access.owner == GripOwner::kNone);
        CHECK(!access.CanAcquire(GripOwner::kWorkbench) && !access.CanAcquire(GripOwner::kDrink));
    }
    {
        // A game-driven weapon switch can put a gun in a formerly busy hand.
        // Downstream holders must clean up rather than keep a second object.
        Frame f; f.attachment = true; f.weapon = true;
        f.Run(false, true);
        CHECK(f.weapon && !f.attachment);
        f.attachment = false; f.drink = true;
        f.Run(false, true);
        CHECK(f.weapon && !f.drink);
    }
    std::printf("hand ownership: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}
