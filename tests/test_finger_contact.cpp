#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>

#include "vr/body/finger_contact.h"

using namespace holevr::vr;
using namespace holevr::vr::ik;
namespace fc = holevr::vr::finger_contact;
namespace jl = holevr::vr::joint_limits;
static int failures = 0;
#define EXPECT(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static fingers::Chain Finger(bool mirror = false) {
    skel::Xf j[3];
    j[0].translation = {0, 0, 0};
    j[1].translation = {4, 0, 0};
    j[2].translation = {7, 0, 0};
    return fingers::Build(j, {0, mirror ? -1.0 : 1.0, 0}, {0, 0, -4}, false);
}
static fingers::Angles Wanted() {
    fingers::Angles a;
    a.flex[0] = 35 * kDegToRad;
    a.flex[1] = 65 * kDegToRad;
    a.flex[2] = 45 * kDegToRad;
    return a;
}
static fc::Contact Press(const fingers::Chain& c, const fingers::Angles& a,
                         const skel::Xf& hand, int segment, double fraction,
                         FVector normal, double depth, double patch = 5) {
    fc::Contact contact;
    contact.valid = true;
    contact.segment = segment;
    contact.fraction = fraction;
    contact.radius_cm = 0.4;
    contact.patch_radius_cm = patch;
    contact.normal = Normalize(normal);
    contact.point = fc::SampleWorld(c, a, hand, segment, fraction) + contact.normal * (depth - contact.radius_cm);
    return contact;
}
static void CheckBounds(const fingers::Chain& c, const fingers::Angles& a, const fc::Bounds& b) {
    for (int j = 0; j < 3; ++j) {
        FQuat q = c.ref[j].rotation.Inverse() * fingers::LocalRotation(c, j, a);
        EXPECT(jl::Within(b.axes[j], b.limits[j], q, 1e-7));
        EXPECT(std::isfinite(q.X) && std::isfinite(q.Y) && std::isfinite(q.Z) && std::isfinite(q.W));
    }
}
static jl::Turn Joint(const fingers::Chain& c, const fingers::Angles& a, const fc::Bounds& b, int j) {
    return jl::Decompose(b.axes[j], c.ref[j].rotation.Inverse() * fingers::LocalRotation(c, j, a));
}

int main() {
    const auto c = Finger();
    const auto b = fc::MakeBounds(c, 2);
    const auto wanted = Wanted();
    skel::Xf hand;
    fc::Contacts contacts;
    contacts.count = 1;
    contacts.items[0] = Press(c, wanted, hand, 2, 1, {1, 0, 0}, 0.8);
    EXPECT(std::fabs(fc::Depth(c, wanted, hand, contacts) - 0.8) < 1e-9);
    fc::State state;
    const auto solved = fc::Solve(c, wanted, state, hand, b, contacts, 1.0 / 90);
    CheckBounds(c, solved.pose, b);
    EXPECT(solved.residual_cm < 0.005);
    EXPECT(solved.active_contacts == 1 && solved.iterations <= 6);
    const auto mcp = Joint(c, solved.pose, b, 0), pip = Joint(c, solved.pose, b, 1), dip = Joint(c, solved.pose, b, 2);
    EXPECT(pip.flex > 50 * kDegToRad);
    EXPECT(std::fabs(mcp.flex - wanted.flex[0]) < 8 * kDegToRad);
    EXPECT(dip.flex < wanted.flex[2] - 10 * kDegToRad);
    EXPECT(std::fabs(dip.flex - 0.7 * pip.flex) > 10 * kDegToRad);
    std::printf("distal contact: MCP/PIP/DIP %.2f/%.2f/%.2f deg, residual %.6f cm\n",
                mcp.flex*kRadToDeg, pip.flex*kRadToDeg, dip.flex*kRadToDeg, solved.residual_cm);
    // Easing into an obstructed requested pose must not accumulate residual
    // penetration across frames after a clear solution has been found.
    {
        double depth=solved.residual_cm;
        for(int frame=0;frame<50;++frame) {
            const auto result=fc::Solve(c,wanted,state,hand,b,contacts,1.0/90);
            EXPECT(result.residual_cm<=depth+1e-7);
            depth=result.residual_cm;
        }
        const double before=std::fabs(Joint(c,state.pose,b,2).flex-wanted.flex[2]);
        const auto release=fc::Solve(c,wanted,state,hand,b,{},1.0/90);
        EXPECT(std::fabs(Joint(c,release.pose,b,2).flex-wanted.flex[2])<before);
    }

    // Changing the world frame cannot change the local joint solution.
    {
        skel::Xf moved;
        moved.translation = {80, -30, 120};
        moved.rotation = ToQuat({25, 72, -38});
        auto transformed = contacts;
        transformed.items[0].point = skel::Apply(moved, contacts.items[0].point);
        transformed.items[0].normal = moved.rotation.Rotate(contacts.items[0].normal);
        fc::State s;
        const auto result = fc::Solve(c, wanted, s, moved, b, transformed, 1.0 / 90);
        EXPECT(result.residual_cm < 0.005);
        for (int j = 0; j < 3; ++j)
            EXPECT(std::fabs(Joint(c, result.pose, b, j).flex - Joint(c, solved.pose, b, j).flex) < 1e-7);
    }
    // Mirroring a hand and its contacts yields the same anatomical motion.
    {
        const auto left = Finger(true);
        const auto limits = fc::MakeBounds(left, 2);
        auto mirrored = contacts;
        mirrored.items[0].point.Y *= -1;
        mirrored.items[0].normal.Y *= -1;
        fc::State s;
        const auto result = fc::Solve(left, wanted, s, hand, limits, mirrored, 1.0/90);
        EXPECT(result.residual_cm < 0.005);
        for (int j = 0; j < 3; ++j)
            EXPECT(std::fabs(Joint(left, result.pose, limits, j).flex - Joint(c, solved.pose, b, j).flex) < 1e-7);
    }
    // Separate local patches can describe a corner; a single whole-hand
    // plane cannot represent these different contact normals together.
    {
        auto corner = contacts;
        corner.count = 2;
        corner.items[1] = Press(c, wanted, hand, 2, 0.5, {0, 0, 1}, 0.5);
        fc::State s;
        const auto result = fc::Solve(c, wanted, s, hand, b, corner, 1.0/90);
        CheckBounds(c, result.pose, b);
        EXPECT(result.active_contacts == 2);
        EXPECT(result.residual_cm < 0.02);
    }
    // Nonuniform reference scale uses the bounded numerical derivative;
    // ordinary rigid-axis derivatives are not valid for this rig shape.
    {
        auto scaled=c;
        scaled.ref[1].scale={1.15,0.8,1.0};
        const auto limits=fc::MakeBounds(scaled,2);
        auto surface=contacts;
        surface.items[0]=Press(scaled,wanted,hand,2,1,{1,0,0},0.8);
        fc::State s;
        const auto result=fc::Solve(scaled,wanted,s,hand,limits,surface,1.0/90);
        CheckBounds(scaled,result.pose,limits);
        EXPECT(result.residual_cm<0.005 && result.iterations<=6);
    }
    // Contact patches expire and do not become infinite walls around edges.
    // A neighboring finger has its own state and need not stop with this one.
    {
        auto stale = contacts;
        stale.items[0].age_seconds = 0.076;
        fc::State s;
        const auto result = fc::Solve(c, wanted, s, hand, b, stale, 1.0/90);
        EXPECT(result.active_contacts == 0 && result.residual_cm == 0);
        EXPECT(std::fabs(Joint(c, result.pose, b, 2).flex - wanted.flex[2]) < 1e-9);
        auto finite = contacts;
        finite.items[0].point.Z += 20;
        fc::State other;
        const auto clear = fc::Solve(c, wanted, other, hand, b, finite, 1.0/90);
        EXPECT(clear.active_contacts == 0 && clear.residual_cm == 0);
        EXPECT(std::fabs(Joint(c, clear.pose, b, 2).flex - wanted.flex[2]) < 1e-9);
    }
    // A candidate must not escape an active tiny patch sideways while it is
    // still behind the observed surface. Patch validity is frozen per solve.
    {
        auto tiny = contacts;
        tiny.items[0].patch_radius_cm = 0.0001;
        fc::State s;
        const auto result = fc::Solve(c, wanted, s, hand, b, tiny, 1.0/90);
        const auto& patch = tiny.items[0];
        const auto point = fc::SampleWorld(c, result.pose, hand, patch.segment, patch.fraction);
        const double raw_depth = patch.radius_cm-Dot(point-patch.point,patch.normal);
        EXPECT(raw_depth < 0.005);
        EXPECT(result.residual_cm < 0.005);
    }
    // A contact at the fixed base cannot be resolved by finger angles. It
    // reports the residual without inventing extra degrees of freedom.
    {
        fc::Contacts stuck;
        stuck.count = 1;
        stuck.items[0] = Press(c, wanted, hand, 0, 0, {1, 0, 0}, 3);
        fc::State s;
        const auto result = fc::Solve(c, wanted, s, hand, b, stuck, 1.0/90);
        EXPECT(std::fabs(result.residual_cm - 3) < 1e-9);
        EXPECT(result.iterations <= 6);
        CheckBounds(c, result.pose, b);
    }
    // Zero time preserves prior free motion. Reset discards it. Release
    // returns toward the requested pose and does not retain stale constraints.
    {
        const auto before = state.pose;
        const auto paused = fc::Solve(c, fingers::Angles{}, state, hand, b, {}, 0);
        for (int j = 0; j < 3; ++j)
            EXPECT(std::fabs(Joint(c, paused.pose, b, j).flex - Joint(c, before, b, j).flex) < 1e-9);
        const auto reset = fc::Solve(c, wanted, state, hand, b, {}, 0, true);
        EXPECT(std::fabs(Joint(c, reset.pose, b, 2).flex - wanted.flex[2]) < 1e-9);
        auto initial = fc::Solve(c, wanted, state, hand, b, contacts, 1.0/90, true);
        (void)initial;
        for (int i = 0; i < 60; ++i) fc::Solve(c, wanted, state, hand, b, {}, 1.0/90);
        std::printf("release after 60 frames: DIP error %.9f rad\n",std::fabs(Joint(c,state.pose,b,2).flex-wanted.flex[2]));
        EXPECT(std::fabs(Joint(c, state.pose, b, 2).flex - wanted.flex[2]) < 1e-6);
    }
    // Joint limits are applied to the entire requested quaternion pose.
    {
        auto over = wanted;
        over.flex[1] = 150*kDegToRad;
        over.extra[2] = AxisAngle({1,0,0}, 0.8);
        fc::State s;
        const auto result = fc::Solve(c, over, s, hand, b, {}, 1.0/90);
        EXPECT(result.limited);
        CheckBounds(c, result.pose, b);
        auto invalid = contacts;
        invalid.items[0].normal.X = std::numeric_limits<double>::quiet_NaN();
        EXPECT(fc::Solve(c, wanted, s, hand, b, invalid, 1.0/90).active_contacts == 0);
    }
    // Real playtest dimensions/reference bends, with rapidly alternating
    // observed normals and contact loss. Every rendered angular increment,
    // including a same-frame extra contact pass, has one shared rate bound.
    for(double hz:{72.0,90.0,120.0}) {
        auto long_finger=c;
        long_finger.ref[0].translation={12.3,0,0};
        long_finger.ref[0].rotation=AxisAngle({0,0,1},16*kDegToRad);
        long_finger.ref[1].translation={4.7,0,0};
        long_finger.ref[1].rotation=AxisAngle({0,0,1},12*kDegToRad);
        long_finger.ref[2].translation={3.6,0,0};long_finger.tip={2.9,0,0};
        const auto limits=fc::MakeBounds(long_finger,2);
        fc::State s;
        auto previous=fc::Solve(long_finger,wanted,s,hand,limits,{},1/hz).pose;
        double maximum_speed=0,maximum_depth=0;
        for(int frame=0;frame<static_cast<int>(hz*6);++frame) {
            fc::Contacts noisy;noisy.count=frame%7==0?0:1;
            const double side=frame%2?1.0:-1.0;
            noisy.items[0]=Press(long_finger,wanted,hand,2,1,{1,.08*side,.05*side},.25+.02*side);
            auto result=fc::Solve(long_finger,wanted,s,hand,limits,noisy,1/hz);
            result=fc::ResolveContacts(long_finger,s,hand,limits,noisy);
            CheckBounds(long_finger,result.pose,limits);
            for(int j=0;j<3;++j) {
                const auto before=Joint(long_finger,previous,limits,j),after=Joint(long_finger,result.pose,limits,j);
                maximum_speed=std::max({maximum_speed,std::fabs(after.flex-before.flex)*hz,
                    std::fabs(after.side-before.side)*hz,std::fabs(after.twist-before.twist)*hz});
            }
            maximum_depth=std::max(maximum_depth,result.residual_cm);previous=result.pose;
        }
        EXPECT(maximum_speed<=360*kDegToRad+1e-6);
        std::printf("noisy contacts %.0f Hz: peak %.3f deg/s, max residual %.5f cm\n",hz,maximum_speed*kRadToDeg,maximum_depth);
        // A sudden deep obstacle cannot demand an unlimited angular snap.
        fc::Contacts intrusion;intrusion.count=1;intrusion.items[0]=Press(long_finger,s.pose,hand,0,0,{1,0,0},3.7);
        const auto residual=fc::Solve(long_finger,wanted,s,hand,limits,intrusion,1/hz);
        EXPECT(residual.residual_cm>=3.69);
    }
    // Time common contact counts and adverse configurations. This measures
    // only CPU math, excludes world queries, and makes no frame-time promise.
    {
        auto benchmark = [&](const char* label, const fc::Contacts& set, int iterations) {
            const auto begin=std::chrono::steady_clock::now();
            double residual=0;
            for(int i=0;i<300;++i) {
                fc::State s;
                residual+=fc::Solve(c,wanted,s,hand,b,set,1.0/90).residual_cm;
            }
            const double us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-begin).count()/300;
            std::printf("%s contact solve: %d contacts, %d iterations, %.2f us/finger (%.3f ms/10), residual sum %.2f\n",
                        label,set.count,iterations,us,us*0.01,residual);
        };
        benchmark("free", {}, 0);
        benchmark("ordinary", contacts, solved.iterations);
        unsigned random = 7;
        auto unit = [&]() { random=random*1664525u+1013904223u; return (random>>8)/16777216.0; };
        for(int count : {6,9,fc::kMaxContacts}) {
            fc::Contacts hardest;
            int maximum_iterations = 0;
            for (int trial=0;trial<80;++trial) {
                fc::Contacts set; set.count=count;
                for (int i=0;i<set.count;++i)
                    set.items[i]=Press(c,wanted,hand,i%3,0.4+0.6*unit(),
                                      {unit()*2-1,unit()*2-1,unit()*2-1},0.1+unit()*3,4);
                fc::State s;
                const auto result=fc::Solve(c,wanted,s,hand,b,set,1.0/90);
                CheckBounds(c,result.pose,b);
                EXPECT(result.iterations<=6 && result.residual_cm<=fc::Depth(c,wanted,hand,set)+1e-7);
                if (result.iterations>maximum_iterations) { maximum_iterations=result.iterations; hardest=set; }
            }
            benchmark("adverse",hardest,maximum_iterations);
        }
    }
    if (failures) { std::printf("finger contact: %d failures\n", failures); return 1; }
    std::puts("finger contact: all tests passed");
    return 0;
}
