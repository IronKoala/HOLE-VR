// Native unit tests for the skeleton pose, fingertip solver and grip records.
//   g++ -std=c++20 -Isrc tests/test_grip.cpp src/vr/body/ik_math.cpp src/vr/body/skeleton.cpp
//       src/vr/body/finger_solve.cpp src/vr/body/grip_record.cpp src/vr/body/grip_editor.cpp src/vr/body/holster_pose.cpp -o /tmp/test_grip && /tmp/test_grip
#include <chrono>
#include <cstdio>
#include <string>

#include "vr/body/finger_solve.h"
#include "vr/body/grip_editor.h"
#include "vr/body/grip_record.h"
#include "vr/body/holster_pose.h"
#include "vr/body/skeleton.h"

using namespace holevr::vr;
using namespace holevr::vr::ik;
using skel::Xf;

static int failures = 0;
#define EXPECT(c)                                                     \
    do {                                                              \
        if (!(c)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);  \
            ++failures;                                               \
        }                                                             \
    } while (0)

static bool Near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) < eps; }
static bool Near(const FVector& a, const FVector& b, double eps = 1e-4) { return Length(a - b) < eps; }

static Xf X(const FVector& t, const FQuat& r = FQuat{}) {
    Xf x;
    x.translation = t;
    x.rotation = r;
    return x;
}

static void TestCompose() {
    Xf p = X({10, 0, 0}, AxisAngle({0, 0, 1}, kPi / 2));
    Xf c = X({5, 0, 0});
    Xf w = skel::Compose(c, p);
    EXPECT(Near(w.translation, FVector{10, 5, 0}));
    Xf back = skel::Relative(w, p);
    EXPECT(Near(back.translation, c.translation));
    // Scaled parent (the hidden head is scaled to almost nothing).
    p.scale = {2, 2, 2};
    w = skel::Compose(c, p);
    EXPECT(Near(w.translation, FVector{10, 10, 0}));
    EXPECT(Near(skel::Relative(w, p).translation, c.translation));
}

static void TestPose() {
    // root -> a -> b, root -> c
    skel::Pose pose;
    EXPECT(pose.Init({-1, 0, 1, 0}));
    EXPECT(!skel::Pose().Init({-1, 2, 0}));
    Xf l[4] = {X({0, 0, 0}), X({10, 0, 0}), X({10, 0, 0}), X({0, 5, 0})};
    pose.SetAllLocal(l, 4);
    EXPECT(Near(pose.C(2).translation, FVector{20, 0, 0}));
    // Rotating a carries b with it, leaves c alone, and changes only a's local.
    pose.ClearChanged();
    pose.RotateC(1, AxisAngle({0, 0, 1}, kPi / 2));
    EXPECT(Near(pose.C(2).translation, FVector{10, 10, 0}));
    EXPECT(Near(pose.C(3).translation, FVector{0, 5, 0}));
    EXPECT(pose.Changed(1) && !pose.Changed(2) && !pose.Changed(3));
    pose.SetTranslationC(2, {0, 0, 7});
    EXPECT(Near(pose.C(2).translation, FVector{0, 0, 7}));
    EXPECT(Near(skel::Compose(pose.L(2), pose.C(1)).translation, FVector{0, 0, 7}));
}

// A right hand like the game's (UE4 mannequin, from the 2026-11-01 log):
// fingers run along -X, palm faces +Y, index side +Z.
struct TestHand {
    fingers::Chain chain[5];
    grip_record::HandGeo geo;
    FVector palm{0, 1, 0};
    FVector mcp[5] = {{-4.8, 2.4, 2.5}, {-12.1, 1.8, 2.1}, {-12.2, 1.3, -0.6}, {-11.6, 1.8, -2.6}, {-10.1, 2.3, -4.6}};
};
static TestHand MakeHand() {
    TestHand h;
    const double seg[5][2] = {{3.5, 3.2}, {4.4, 2.6}, {4.6, 3.4}, {4.3, 3.0}, {3.4, 2.2}};
    for (int f = 0; f < 5; ++f) {
        FVector dir = f == 0 ? Normalize(FVector{-1, 0.4, 0.7}) : FVector{-1, 0, 0};
        FQuat r = Arc(FVector{-1, 0, 0}, dir);  // bone -X along the finger, like the mannequin's right side
        Xf j[3];
        j[0] = X(h.mcp[f], r);
        j[1] = X(h.mcp[f] + dir * seg[f][0], r);
        j[2] = X(j[1].translation + dir * seg[f][1], r);
        h.chain[f] = fingers::Build(j, h.palm, h.mcp[4], f == 0);
    }
    h.geo.knuckle = h.mcp[2];
    h.geo.fingers = Normalize(h.mcp[2]);
    h.geo.across = Normalize(h.mcp[1] - h.mcp[4]);
    return h;
}

static void TestFingerSolve() {
    TestHand h = MakeHand();
    const fingers::Chain& c = h.chain[2];
    EXPECT(c.valid);
    fingers::Limits lim = fingers::DefaultLimits(false);
    // Straight finger: tip ~10.5 cm out along -X.
    Xf j[3];
    FVector tip;
    fingers::Fk(c, fingers::Angles{}, j, &tip);
    EXPECT(Near(tip, h.mcp[2] + FVector{-(4.6 + 3.4 + 3.4 * 0.8), 0, 0}, 1e-3));
    // Curling moves the tip toward the palm (+Y).
    fingers::Fk(c, fingers::Curl(c, 0.5, lim), j, &tip);
    EXPECT(tip.Y > h.mcp[2].Y + 2.0);
    // A reachable target is reached.
    fingers::Angles want;
    want.flex[0] = 40 * kDegToRad;
    want.flex[1] = 50 * kDegToRad;
    want.flex[2] = 35 * kDegToRad;
    FVector target;
    fingers::Fk(c, want, j, &target);
    double err = 9;
    fingers::Solve(c, target, lim, &err);
    EXPECT(err < 0.3);
    // A target behind the hand bends nothing backward past the limit.
    fingers::Angles back = fingers::Solve(c, h.mcp[2] + FVector{-3, -8, 0}, lim);
    for (int n = 0; n < 3; ++n) EXPECT(back.flex[n] >= lim.flex_min[n] * kDegToRad - 1e-9);
}

static void TestRecord() {
    grip_record::Record r = grip_record::DefaultPistol();
    std::string text = grip_record::Serialize(r);
    grip_record::Record p;
    EXPECT(grip_record::Parse(text, &p));
    EXPECT(Near(p.knuckle, r.knuckle, 0.01));
    EXPECT(Near(p.tip[grip_record::kThumb], r.tip[grip_record::kThumb], 0.01));
    EXPECT(p.outline.size() == r.outline.size());
    grip_record::Record m = grip_record::Mirror(r);
    EXPECT(!m.right_hand && Near(m.tip[grip_record::kThumb].Y, 5.9, 1e-9));
    // Hand transform round trip.
    TestHand h = MakeHand();
    FQuat rot;
    FVector pos;
    grip_record::HandInObject(r, h.geo, &rot, &pos);
    grip_record::Record s = r;
    grip_record::StoreHand(&s, h.geo, rot, pos);
    EXPECT(Near(s.knuckle, r.knuckle, 1e-6));
    EXPECT(Near(s.across, r.across, 1e-6));
    // Outline: the grip centre is inside, a point beside it is not.
    EXPECT(grip_record::Depth(r, {4.0, -3.45, 6.0}) > 1.0);
    EXPECT(grip_record::Depth(r, {4.0, 0.5, 6.0}) == 0.0);
}

// The built-in pistol grip on the game's hand: how close each fingertip gets
// to its target, which way the thumb ends up, and how deep anything sits
// inside the pistol's outline. Printed so the numbers can be compared with
// the headset.
static void TestDefaultPistolOnHand() {
    TestHand h = MakeHand();
    grip_record::Record r = grip_record::DefaultPistol();
    FQuat rot;
    FVector pos;
    grip_record::HandInObject(r, h.geo, &rot, &pos);
    std::printf("default pistol grip: wrist at (%.1f %.1f %.1f) gun frame\n", pos.X, pos.Y, pos.Z);
    const int tip_of[5] = {grip_record::kThumb, grip_record::kIndexOff, grip_record::kMiddle, grip_record::kRing,
                           grip_record::kPinky};
    double worst = 0;
    for (int f = 0; f < 5; ++f) {
        const fingers::Chain& c = h.chain[f];
        FVector target = rot.Inverse().Rotate(r.tip[tip_of[f]] - pos);
        double err = 0, depth = 0;
        fingers::DepthFn fn = [&](const FVector& p) { return grip_record::Depth(r, pos + rot.Rotate(p)); };
        fingers::Angles a = fingers::Solve(c, target, fingers::DefaultLimits(f == 0), &err, &fn, 0.5, &depth);
        Xf j[3];
        FVector tip;
        fingers::Fk(c, a, j, &tip);
        std::string box = "-";
        FVector tip_gun = pos + rot.Rotate(tip);
        std::printf("  %-6s tip error %.1f cm, tip at (%.1f %.1f %.1f), deepest %.1f cm inside %s; flex %.0f %.0f %.0f "
                    "spread %.0f deg\n",
                    grip_record::TipName(tip_of[f]), err, tip_gun.X, tip_gun.Y, tip_gun.Z, depth,
                    box.c_str(), a.flex[0] * kRadToDeg, a.flex[1] * kRadToDeg,
                    a.flex[2] * kRadToDeg, a.spread * kRadToDeg);
        worst = std::max(worst, depth);
        if (f == 0) EXPECT(tip_gun.Y < -4.5);  // thumb on the LEFT side of the gun
        if (f == 1) EXPECT(tip_gun.Y > -2.0 && tip_gun.X > 12.0);  // index along the right of the frame
    }
    // Palm skin points (between wrist and knuckles, out of the palm).
    double palm = 0;
    for (double along : {0.3, 0.5, 0.7, 0.9})
        for (double side : {-0.4, 0.0, 0.4}) {
            FVector p = h.geo.knuckle * along + h.palm * 1.5 + h.geo.across * (side * 5.0);
            palm = std::max(palm, grip_record::Depth(r, pos + rot.Rotate(p)));
        }
    std::printf("  palm deepest %.1f cm inside the gun\n", palm);
    EXPECT(palm < 0.5);
    EXPECT(worst < 0.5);
}

static void TestEditor() {
    grip_editor::Editor ed;
    grip_record::Record rec = grip_record::DefaultPistol();
    FQuat hr;
    FVector hp{1, 2, 3};
    grip_editor::Scene sc;
    for (int m = 0; m < grip_editor::kJointItems; ++m) sc.joint[m] = {m * 3.0, 0, 0};
    sc.palm = {0, 0, 8};
    grip_editor::Input in;
    in.dt = 0.1;
    // Short Y taps do nothing while off; a 1 s hold turns it on.
    in.y_down = true;
    grip_editor::Output o;
    for (int i = 0; i < 5; ++i) o = ed.Update(in, sc, &rec, &hr, &hp);
    EXPECT(!ed.Active());
    for (int i = 0; i < 6; ++i) o = ed.Update(in, sc, &rec, &hr, &hp);
    EXPECT(ed.Active());
    in.y_down = false;
    o = ed.Update(in, sc, &rec, &hr, &hp);
    EXPECT(!o.save);  // releasing after the long hold is not a tap
    // Squeeze next to joint 4 (index middle joint): it is grabbed, once.
    in.pointer = sc.joint[4] + FVector{0, -1, 0};
    in.grab_down = true;
    o = ed.Update(in, sc, &rec, &hr, &hp);
    EXPECT(o.grabbed == 4 && o.started);
    o = ed.Update(in, sc, &rec, &hr, &hp);
    EXPECT(o.grabbed == 4 && !o.started);
    // The object ball, when there is one, is a bigger target than a joint.
    sc.has_object = true;
    sc.object = sc.joint[4] + FVector{0, 0, 2.0};
    EXPECT(grip_editor::Pick(sc.joint[4] + FVector{0, 0, 1.2}, sc) == grip_editor::kObjectItem);
    sc.has_object = false;
    in.grab_down = false;
    ed.Update(in, sc, &rec, &hr, &hp);
    // Grab the palm and turn: the hand turns with the controller.
    in.pointer = sc.palm;
    in.grab_down = true;
    ed.Update(in, sc, &rec, &hr, &hp);
    in.pointer_rot = AxisAngle({0, 0, 1}, 0.2);
    o = ed.Update(in, sc, &rec, &hr, &hp);
    EXPECT(o.grabbed == grip_editor::kHandItem && Near(hr.Z, std::sin(0.1), 1e-6));
    in.grab_down = false;
    // A tap saves; a long hold saves and stops.
    in.y_down = true;
    ed.Update(in, sc, &rec, &hr, &hp);
    in.y_down = false;
    o = ed.Update(in, sc, &rec, &hr, &hp);
    EXPECT(o.save && ed.Active());
    in.y_down = true;
    bool saved = false;
    for (int i = 0; i < 11; ++i) saved |= ed.Update(in, sc, &rec, &hr, &hp).exited;
    EXPECT(saved && !ed.Active());
}

// Rough cost of a frame's pose math: a 68-bone tree, ~60 bone edits with
// subtree updates, as the body tick does. Printed, not asserted.
static void BenchPose() {
    std::vector<int> parent(68);
    parent[0] = -1;
    for (int i = 1; i < 68; ++i) parent[i] = (i % 4 == 1) ? 0 : i - 1;
    skel::Pose pose;
    pose.Init(parent);
    std::vector<Xf> local(68, X({5, 1, 0}, AxisAngle({0, 1, 1}, 0.1)));
    auto t0 = std::chrono::steady_clock::now();
    const int frames = 2000;
    for (int f = 0; f < frames; ++f) {
        pose.SetAllLocal(local.data(), 68);
        for (int k = 0; k < 60; ++k) pose.RotateC(1 + (k * 7) % 67, AxisAngle({1, 0, 0}, 0.01));
    }
    double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / frames;
    std::printf("pose math: %.0f us per frame (68 bones, 60 edits)\n", us);
}

// Joint poses set in the editor survive a save/load and drive the finger.
static void TestPosedJoints() {
    grip_record::Record r = grip_record::DefaultPistol();
    r.posed[grip_record::kMiddle] = true;
    r.joint[grip_record::kMiddle][1] = AxisAngle(Normalize(FVector{0.2, 1, 0.3}), 0.7);
    grip_record::Record back;
    EXPECT(grip_record::Parse(grip_record::Serialize(r), &back));
    EXPECT(back.posed[grip_record::kMiddle] && !back.posed[grip_record::kThumb]);
    const FQuat& q = back.joint[grip_record::kMiddle][1];
    const FQuat& w = r.joint[grip_record::kMiddle][1];
    EXPECT(Near(q.X, w.X, 1e-4) && Near(q.Y, w.Y, 1e-4) && Near(q.Z, w.Z, 1e-4) && Near(q.W, w.W, 1e-4));
    // extra[j] turns joint j in its own frame, after flex.
    fingers::Chain c;
    for (int j = 0; j < 3; ++j) {
        c.ref[j].translation = {j ? 4.0 : 0.0, 0, 0};
        c.flex[j] = {0, 1, 0};
    }
    c.spread = {0, 0, 1};
    c.tip = {3, 0, 0};
    fingers::Angles a;
    a.extra[0] = AxisAngle({0, 0, 1}, kPi / 2);
    FVector tip;
    skel::Xf jx[3];
    fingers::Fk(c, a, jx, &tip);
    EXPECT(Near(tip, FVector{0, 11, 0}));
    fingers::Angles h = fingers::Blend(fingers::Angles{}, a, 0.5);
    EXPECT(Near(h.extra[0].Z, std::sin(kPi / 8), 1e-6));
}

static void TestHolsterApproach() {
    TestHand h = MakeHand();
    auto r = grip_record::DefaultPistol();
    // Real saved targets were far forward/up; the approach must not inherit
    // those targets or the trigger's bent resting/pulled pose.
    r.tip[grip_record::kThumb] = {21.90,-9.45,12.66};
    r.tip[grip_record::kIndexOff] = {28.47,-7.15,21.30};
    FQuat rot; FVector pos;
    grip_record::HandInObject(r,h.geo,&rot,&pos);
    const auto p = holster_pose::Approach(h.chain[0],h.chain[1],rot);
    for (int f=0;f<2;++f) {
        const auto& a = f==0 ? p.thumb : p.index;
        Xf joint[3]; FVector tip;
        fingers::Fk(h.chain[f],a,joint,&tip);
        const FVector aim = f==0 ? Normalize(FVector{-.4,0,1}) : Normalize(FVector{1,-.04,.25});
        const FVector points[4] = {joint[0].translation,joint[1].translation,joint[2].translation,tip};
        EXPECT(Near(points[0],h.chain[f].ref[0].translation)); // knuckle stays on saved grip
        EXPECT(Near(Length(tip-points[0]),h.chain[f].length,1e-5));
        if (f==1) {
            const FVector in_gun=rot.Rotate(tip-points[0]);
            EXPECT(in_gun.Y < -.2 && in_gun.Y > -.6); // modest inward adjustment
            EXPECT(in_gun.X > .96*h.chain[f].length); // still points down barrel
            EXPECT(in_gun.Z > .24*h.chain[f].length); // reaches the side above the lower rim
        }
        for (int j=0;j<3;++j) {
            EXPECT(Dot(Normalize(rot.Rotate(points[j+1]-points[j])),aim) > .999999);
            EXPECT(std::isfinite(a.extra[j].W));
        }
        // A tilted/calibrated holster carries the pose along with the gun.
        const FQuat holster = Normalized(FQuat{-.07713,-.73441,-.03793,-.67324});
        EXPECT(Dot(Normalize(holster.Rotate(rot.Rotate(tip-points[0]))),holster.Rotate(aim)) > .999999);
    }
    // Also straighten reference chains whose joints are not already collinear.
    Xf curved[3] = {X({0,0,0}),X({4,1,0},AxisAngle({0,0,1},.3)),X({7,2,1},AxisAngle({0,1,0},.4))};
    auto chain = fingers::Build(curved,{0,1,0},{0,0,-3},false);
    auto a = holster_pose::Extended(chain,{0,0,-1});
    Xf joint[3]; FVector tip;
    fingers::Fk(chain,a,joint,&tip);
    EXPECT(Near(tip,{0,0,-chain.length},1e-5));
    auto invalid = holster_pose::Extended(fingers::Chain{}, {0,0,1});
    EXPECT(Near(invalid.extra[0].W,1));
    const auto before = grip_record::Serialize(r);
    const auto fit = holster_pose::Fit(r,h.chain,rot,pos,.5);
    EXPECT(Near(fit.hand_position-pos,FVector{0,1.8,0}));
    EXPECT(grip_record::Serialize(r)==before); // held grip is not changed
    for (int f=2;f<5;++f) {
        Xf joint[3];FVector tip;
        fingers::Fk(h.chain[f],fit.finger[f],joint,&tip);
        const FVector p=fit.hand_position+rot.Rotate(tip);
        EXPECT(Length(p-r.tip[grip_record::kMiddle+f-2]) < 2.0);
    }
}

int main() {
    TestHolsterApproach();
    TestPosedJoints();
    TestEditor();
    BenchPose();
    TestCompose();
    TestPose();
    TestFingerSolve();
    TestRecord();
    TestDefaultPistolOnHand();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all grip tests passed\n");
    return 0;
}
