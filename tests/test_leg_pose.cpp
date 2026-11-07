#include "vr/body/leg_pose.h"
#include "vr/body/arm_pose.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace holevr::vr;
using namespace holevr::vr::body::legs;
namespace {
int checks = 0;
void Check(bool ok, const char* what) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a-b) < eps; }
bool Near(FVector a, FVector b) { return ik::Length(a-b) < 1e-6; }
struct Scene {
    State state;
    Input in;
    Ground ground[2];
    Scene() {
        in.dt = 1.0/90;
        in.state_known = in.grounded = true;
        in.floor_known = true;
        for (int i=0; i<2; ++i) {
            const double y = i ? 10 : -10;
            in.foot[i].valid = true;
            in.foot[i].hip = in.foot[i].animated_hip = {0,y,95};
            in.foot[i].knee = {5,y,52};
            in.foot[i].ankle = {0,y,8};
            in.foot[i].upper_length = ik::Length(in.foot[i].knee - in.foot[i].animated_hip);
            in.foot[i].lower_length = ik::Length(in.foot[i].ankle - in.foot[i].knee);
            in.foot[i].sole_offset = 8;
            in.foot[i].stance_valid = true;
            in.foot[i].stance_ankle = in.foot[i].ankle;
            ground[i].hit = true;
            ground[i].point = {0,y,0};
            ground[i].support_id = 1;
        }
    }
    Output Tick(int n=1) { Output o; while (n--) o=Plan(state,in,ground); return o; }
    void MoveDesired(double x) {
        for(int i=0; i<2; ++i) {
            in.foot[i].hip.X = in.foot[i].animated_hip.X = in.foot[i].ankle.X = x;
            in.foot[i].knee.X = x+5;
            in.foot[i].stance_ankle.X = x;
            ground[i].point.X = x;
        }
    }
};
void BypassActions() {
    Scene s;
    Check(s.Tick(30).foot[0].mode == Mode::Planted, "idle reaches contact-qualified plant");
    s.in.kicking = true;
    s.in.foot[0].ankle = {70,-10,55};
    s.in.foot[0].knee = {38,-10,78};
    auto o=s.Tick();
    Check(!o.foot[0].solve && !o.foot[1].solve, "stationary kick bypasses BOTH leg writes");
    Check(Near(o.foot[0].ankle,s.in.foot[0].ankle), "kick ankle is unmodified");
    Check(!s.state.foot[0].locked && !s.state.foot[1].locked, "kick releases stale contacts");
    s.in.kicking=false;
    Check(s.Tick().foot[0].mode == Mode::Animation, "kick completion has animation settle period");
    s.in.grounded=false;
    Check(!s.Tick().foot[0].solve, "vertical airborne motion bypasses even with zero horizontal speed");
    s.in.grounded=true; s.in.state_known=false;
    Check(!s.Tick().foot[0].solve, "missing movement/kick query fails open to animation");
}
void LocomotionAndContact() {
    Scene s;
    s.in.speed=400;
    s.in.foot[0].ankle.Z=28;
    s.in.foot[0].knee.X=-12;
    auto o=s.Tick(30);
    Check(o.foot[0].mode == Mode::Animation && Near(o.foot[0].ankle.Z,28), "sprint preserves swing height");
    Check(o.foot[0].pole.X<0, "knee plane follows source animation, not forced body forward");
    Check(Near(o.pelvis_offset.Z,0), "gait swing does not pull pelvis down");
    s.in.speed=0;
    Check(s.Tick(30).foot[0].mode == Mode::Animation, "stopping never plants a foot held above floor");
    s.in.foot[0].ankle.Z=8;
    Check(s.Tick().foot[0].mode == Mode::Animation, "landing must settle before planting");
    Check(s.Tick(10).foot[0].mode == Mode::Planted, "settled landing acquires contact");
    s.in.speed=25;
    Check(s.Tick().foot[0].mode == Mode::Animation, "movement releases plant immediately");
    s.in.speed=18;
    Check(s.Tick(30).foot[0].mode == Mode::Animation, "speed hysteresis prevents threshold flicker");
    s.in.speed=10;
    Check(s.Tick(30).foot[0].mode == Mode::Planted, "below lower threshold permits idle plant");
}
void RoomSteps() {
    Scene s; s.Tick(30); s.MoveDesired(22);
    auto o=s.Tick();
    Check(o.foot[0].mode==Mode::Step && o.foot[1].mode==Mode::Planted, "one idle foot steps while other supports");
    Check(Near(o.foot[0].ankle.X,0), "step starts at prior plant without snapping");
    s.MoveDesired(30);
    o=s.Tick(10);
    Check(o.foot[0].ankle.Z>8 && o.foot[0].ankle.X<22, "room step lifts in a bounded arc");
    o=s.Tick(14);
    Check(Near(o.foot[0].ankle.X,22), "step lands at captured endpoint despite changing desired pose");
    Check(o.foot[1].mode==Mode::Step, "other foot follows after first lands");
    Check(o.steps_taken==1, "completed step counted once");
    // Real pivoting moves the desired foot orientation. Detect a turn without
    // relying on drift in the actor's horizontal velocity.
    Scene turn; turn.Tick(30);
    for(auto& f:turn.in.foot) f.rotation=ik::AxisAngle({0,0,1},45*kDegToRad);
    Check(turn.Tick().foot[0].mode==Mode::Step, "room rotation can initiate one grounded step");
}
void GroundSafety() {
    Scene s;
    Ground g=s.ground[0];
    Check(ValidGround(g,ExpectedSole(s.in.foot[0])), "flat sole hit accepted");
    g.point.Z=65;
    Check(!ValidGround(g,ExpectedSole(s.in.foot[0])), "tabletop far above sole rejected");
    g=s.ground[0]; g.normal={1,0,0};
    Check(!ValidGround(g,ExpectedSole(s.in.foot[0])), "vertical wall rejected");
    g=s.ground[0]; g.initial_overlap=true;
    Check(!ValidGround(g,ExpectedSole(s.in.foot[0])), "overlap rejected");
    g=s.ground[0]; g.point.Z=-25;
    Check(!ValidGround(g,ExpectedSole(s.in.foot[0])), "unsupported drop rejected");
    g=s.ground[0]; g.normal.Z=std::numeric_limits<double>::quiet_NaN();
    Check(!ValidGround(g,ExpectedSole(s.in.foot[0])), "non-finite hit rejected");
    s.Tick(30); s.ground[0].hit=false;
    Check(s.Tick().foot[0].mode==Mode::Animation && !s.state.foot[0].locked, "lost ground releases foot");
    Scene uneven;
    uneven.ground[0].point.Z=2;
    auto o=uneven.Tick(30);
    Check(Near(o.foot[0].ankle.Z,10) && Near(o.foot[1].ankle.Z,8), "independent near-floor contacts preserve uneven heights");
    Check(Near(uneven.in.foot[0].ankle.Z,8), "planner never mutates source animation");
    Scene slope;
    const FQuat tilt=ik::AxisAngle({0,1,0},20*kDegToRad);
    for(int i=0;i<2;++i) {
        slope.in.foot[i].rotation=tilt;
        slope.in.foot[i].sole_up_local={0,0,1};
        slope.ground[i].normal=tilt.Rotate({0,0,1});
    }
    o=slope.Tick(30);
    Check(Near(o.foot[0].rotation.Rotate({0,0,1}),slope.ground[0].normal), "existing animation floor tilt is not applied twice");
}
void PlatformsAndResets() {
    Scene s; s.Tick(30);
    s.MoveDesired(50); s.in.origin.X=50;
    for(auto& g:s.ground) g.support_frame.translation.X=50;
    auto o=s.Tick();
    Check(Near(o.foot[0].ankle.X,50) && o.foot[0].mode==Mode::Planted, "planted foot follows supporting platform transform");
    s.ground[0].support_id=2;
    Check(s.Tick().foot[0].mode==Mode::Animation, "new support cannot reuse old local anchor");
    Scene jump; jump.Tick(30); jump.in.origin.X=150; jump.MoveDesired(150);
    Check(!jump.Tick().foot[0].solve && !jump.state.foot[0].locked, "teleport clears world foot locks");
    Scene pause; pause.Tick(30); pause.in.dt=1;
    Check(!pause.Tick().foot[0].solve, "long frame clears lock instead of completing a giant step");
    pause.in.dt=0;
    Check(!pause.Tick().foot[0].solve, "zero dt is safe");
    pause.in.dt=std::numeric_limits<double>::quiet_NaN();
    Check(!pause.Tick().foot[0].solve, "non-finite dt is safe");
    Scene invalid; invalid.in.foot[0].rotation={0,0,0,0};
    Check(!invalid.Tick().foot[0].solve, "invalid limb frame is left untouched");
}
void PelvisReach() {
    Scene s;
    s.in.foot[0].hip.Z=s.in.foot[1].hip.Z=120;
    auto o=s.Tick(30);
    Check(o.pelvis_offset.Z<0 && o.pelvis_offset.Z>=-12, "support reach correction is downward and bounded");
    s.in.kicking=true;
    Check(Near(s.Tick().pelvis_offset.Z,0), "kick never requests pelvis contact correction");
    Scene source_stretched;
    for (auto& f:source_stretched.in.foot) {
        f.hip.Z=120;
        f.knee.X=80; // corrupt source geometry appears to double the available reach
    }
    const auto repaired=source_stretched.Tick(30);
    Check(Near(repaired.pelvis_offset.Z,o.pelvis_offset.Z) && repaired.pelvis_offset.Z<0,
          "source joint displacement cannot enlarge reference reach or suppress pelvis correction");
    Scene invalid;
    invalid.in.foot[0].upper_length=0;
    invalid.in.foot[1].lower_length=std::numeric_limits<double>::quiet_NaN();
    const auto bypass=invalid.Tick(30);
    Check(!bypass.foot[0].solve && !bypass.foot[1].solve,
          "missing or invalid reference segment lengths cannot use source geometry as fallback");
}
void TableCannotBecomeGround() {
    Scene s;
    s.in.foot[0].ankle={12,-10,78}; // already corrupted by source ALS foot placement
    s.in.foot[0].knee={-40,-10,70};
    s.in.foot[0].stance_pole={1,0,0};
    Check(Near(ProbeSole(s.in,0),FVector{0,-10,0}), "raised source foot probes reference stance at capsule floor");
    Ground table=s.ground[0]; table.point.Z=70;
    Check(!ValidGround(table,ProbeSole(s.in,0)), "raised source ankle cannot authorize a tabletop hit");
    auto o=s.Tick(15);
    Check(o.foot[0].ankle.Z<78 && o.foot[0].ankle.Z>8, "bad idle foot lowers smoothly rather than teleporting");
    o=s.Tick(90);
    Check(Near(o.foot[0].ankle,FVector{0,-10,8}), "idle tabletop foot recovers to physical floor stance");
    Check(o.foot[0].pole.X>0.9, "table-corrupted knee plane is replaced with anatomical stance plane");
    Scene no_hit;
    no_hit.in.foot[0]=s.in.foot[0]; no_hit.ground[0].hit=false;
    o=no_hit.Tick(110);
    Check(Near(o.foot[0].ankle.Z,8) && o.foot[0].mode==Mode::Recovery,
          "failed floor probe still removes known elevated idle stance without fabricating a contact");
    Scene run;
    run.in.foot[0]=s.in.foot[0]; run.in.speed=400;
    o=run.Tick(110);
    Check(Near(o.foot[0].ankle.Z,78) && o.foot[0].mode==Mode::Animation, "high moving swing is not mistaken for idle terrain error");
    Scene kick;
    kick.in.foot[0]=s.in.foot[0]; kick.in.kicking=true;
    o=kick.Tick(110);
    Check(!o.foot[0].solve && Near(o.foot[0].ankle.Z,78), "high kick remains owned by animation");
    Scene stairs;
    stairs.in.speed=100; stairs.in.floor_z=40;
    for(int i=0;i<2;++i) {
        stairs.in.foot[i].ankle.Z+=40; stairs.in.foot[i].knee.Z+=40;
        stairs.in.foot[i].animated_hip.Z+=40; stairs.in.foot[i].hip.Z+=40;
        stairs.ground[i].point.Z=40;
    }
    o=stairs.Tick(30);
    Check(Near(ProbeSole(stairs.in,0).Z,40) && Near(o.foot[0].ankle.Z,48), "floor reference follows character ascending stairs, not a hard world-height cap");
    stairs.in.floor_known=false;
    Check(!stairs.Tick().foot[0].solve, "missing independent floor never falls back to raised-ankle floor");
}
void IdlePelvisGuard() {
    Scene s;
    s.in.foot[0].ankle.Z+=70;
    Check(Near(CorrectIdlePelvisHeight(s.in,165,95),95),
          "pelvis and source foot lifted 70cm are rebased to independent nominal height");
    Check(Near(CorrectIdlePelvisHeight(s.in,75,95),75),
          "upward-only pelvis guard preserves ordinary crouch");
    Check(Near(CorrectIdlePelvisHeight(s.in,107,95),107),
          "small source pelvis movement remains animation-owned");
    const Input high=s.in;
    s.in.kicking=true;
    Check(Near(CorrectIdlePelvisHeight(s.in,165,95),165),"pelvis repair never modifies a kick");
    s.in=high; s.in.grounded=false;
    Check(Near(CorrectIdlePelvisHeight(s.in,165,95),165),"pelvis repair never modifies airborne animation");
    s.in=high; s.in.speed=24;
    Check(Near(CorrectIdlePelvisHeight(s.in,165,95),165),"pelvis repair preserves moving gait");
    s.in=high; s.in.state_known=false;
    Check(Near(CorrectIdlePelvisHeight(s.in,165,95),165),"unknown movement state cannot authorize pelvis correction");
    s.in=high; s.in.floor_known=false;
    Check(Near(CorrectIdlePelvisHeight(s.in,165,95),165),"unknown floor cannot authorize pelvis correction");
    s.in=high; s.in.foot[0].ankle.Z=8;
    Check(Near(CorrectIdlePelvisHeight(s.in,165,95),165),"normal low feet cannot trigger pelvis correction");
    s.in=high; s.in.foot[0].valid=false;
    Check(Near(CorrectIdlePelvisHeight(s.in,165,95),165),"missing source leg cannot trigger pelvis correction");
    const double nan=std::numeric_limits<double>::quiet_NaN();
    s.in=high; s.in.speed=nan;
    Check(Near(CorrectIdlePelvisHeight(s.in,165,95),165),"non-finite speed cannot authorize correction");
    s.in=high; s.in.floor_z=nan;
    Check(Near(CorrectIdlePelvisHeight(s.in,165,95),165),"non-finite physical floor cannot authorize correction");
    s.in=high; s.in.foot[0].ankle.Z=nan;
    Check(Near(CorrectIdlePelvisHeight(s.in,165,95),165),"non-finite source ankle cannot authorize correction");
    s.in=high; s.in.foot[0].sole_offset=nan;
    Check(Near(CorrectIdlePelvisHeight(s.in,165,95),165),"non-finite sole offset cannot authorize correction");
    Check(Near(CorrectIdlePelvisHeight(high,165,nan),165) && std::isnan(CorrectIdlePelvisHeight(high,nan,95)),
          "invalid pelvis heights are left untouched rather than replaced by fabricated geometry");
}
void StairFeet() {
    auto qualify=[](Scene& s,int side,double z) {
        s.ground[side].stair=true; s.ground[side].point.Z=z; s.ground[side].clearance_z=z;
    };
    Scene ascent; ascent.in.speed=120; ascent.in.velocity={120,0,0};
    qualify(ascent,0,18); qualify(ascent,1,0);
    auto o=ascent.Tick(30);
    Check(o.foot[0].mode==Mode::StairSupport&&o.foot[1].mode==Mode::StairSupport,
          "moving staircase acquires independent support instead of bypassing all gait contacts");
    Check(Near(o.foot[0].ankle.Z,26)&&Near(o.foot[1].ankle.Z,8),"ascent feet occupy distinct real tread heights");
    const FVector plant=o.foot[0].ankle;
    for(int n=0;n<5;++n) {
        ascent.in.origin.X+=120*ascent.in.dt;
        for(auto& f:ascent.in.foot) f.hip.X+=120*ascent.in.dt;
        o=ascent.Tick();
    }
    Check(Near(o.foot[0].ankle,plant),"support foot remains planted in world space during weight transfer");
    ascent.in.foot[0].ankle.X=6; ascent.in.foot[0].ankle.Z=18; ascent.ground[0].point.X=6;
    o=ascent.Tick();
    Check(o.foot[0].mode==Mode::StairSwing&&!ascent.state.foot[0].locked,
          "native forward/lifting swing releases the stair support");
    Check(o.foot[0].ankle.Z>=32,"ascending swing clears the tread rather than cutting through its riser");
    Check(o.foot[1].mode==Mode::StairSupport,"other support stays planted while one foot swings");
    ascent.in.foot[0].ankle={30,-10,8}; ascent.ground[0].point.X=30;
    o=ascent.Tick(30);
    Check(o.foot[0].mode==Mode::StairSupport&&Near(o.foot[0].ankle,FVector{30,-10,26}),
          "landing reacquires a fresh per-foot tread endpoint");
    Scene native; native.in.speed=120; native.in.velocity={120,0,0};
    qualify(native,0,18); qualify(native,1,0); native.in.foot[0].ankle.Z=26;
    o=native.Tick(30);
    Check(Near(o.foot[0].ankle.Z,26),"source animation already on a tread is not raised twice");
    Scene descent; descent.in.speed=100; descent.in.velocity={100,0,0};
    qualify(descent,0,-18); qualify(descent,1,0); o=descent.Tick(30);
    Check(o.foot[0].mode==Mode::StairSupport&&Near(o.foot[0].ankle.Z,-10),"descending foot reaches its lower tread");
    Check(o.pelvis_offset.Z<-16&&o.pelvis_offset.Z>=-30,
          "only real stair support permits the bounded extra pelvis descent needed for leg reach");
    Scene idle; qualify(idle,0,18); qualify(idle,1,0); o=idle.Tick(45);
    Check(o.foot[0].mode==Mode::Planted&&Near(o.foot[0].ankle.Z,26)&&Near(o.foot[1].ankle.Z,8),
          "stopping keeps separate tread heights");
    for(auto& f:idle.in.foot) f.rotation=ik::AxisAngle({0,0,1},45*kDegToRad);
    o=idle.Tick();
    Check(o.foot[0].mode==Mode::Step&&o.foot[1].mode==Mode::Planted,"turning on stairs releases one support at a time");
    Scene step; qualify(step,0,0); qualify(step,1,0); step.Tick(30); step.MoveDesired(22);
    qualify(step,0,18);
    double high=0;
    for(int n=0;n<45;++n) { o=step.Tick(); high=std::max(high,o.foot[0].ankle.Z); }
    Check(high>30&&Near(o.foot[0].ankle.Z,26),"idle stair step lifts over the riser before settling on its captured tread");
    ascent.in.kicking=true; o=ascent.Tick();
    Check(!o.foot[0].solve&&!o.foot[1].solve&&!ascent.state.foot[1].locked,"stair contacts cannot override a native kick");
    descent.in.grounded=false; o=descent.Tick();
    Check(!o.foot[0].solve&&!o.foot[1].solve,"stair qualification never suppresses airborne animation");
    Scene furniture; qualify(furniture,0,65);
    Check(!ValidGround(furniture.ground[0],ProbeSole(furniture.in,0)),"even a stair-marked hit cannot authorize a tabletop height");
    for(double rate:{7.,30.,90.}) {
        Scene slow; slow.in.dt=1/rate; slow.in.speed=120; slow.in.velocity={120,0,0};
        qualify(slow,0,18); qualify(slow,1,-18);
        o=slow.Tick(int(rate));
        Check(o.foot[0].mode==Mode::StairSupport&&o.foot[1].mode==Mode::StairSupport&&
              Near(o.foot[0].ankle.Z,26)&&Near(o.foot[1].ankle.Z,-10),
              "per-foot stair support settles at low and high game update rates");
    }
}
void LegReferenceFrames() {
    // Straight vertical rest legs have no geometric knee plane. Exported
    // thigh/calf axes may be arbitrary and mirrored; forward supplies the
    // anatomical fallback without assuming either bone's local X axis.
    for (int side=0; side<2; ++side) {
        const double sign=side ? 1.0 : -1.0;
        skel::Xf thigh,calf,foot;
        thigh.translation={0,sign*10,95};
        calf.translation={0,sign*10,52};
        foot.translation={0,sign*10,8};
        thigh.rotation=ik::AxisAngle({1,2,3},sign*1.2);
        calf.rotation=ik::AxisAngle({-2,3,1},sign*0.9);
        foot.rotation=ik::AxisAngle({0,0,1},sign*0.2);
        const auto ref=arm_pose::MeasureReference(thigh,calf,foot,{1,0,0});
        Check(ref.valid && Near(ref.upper_length,43) && Near(ref.forearm_length,44),
              "vertical rest leg yields measured arbitrary-axis reference");
        const FVector targets[]={{0,sign*10,8},{18,sign*10,17},{-10,sign*10,28},{0,sign*10,-10}};
        for (const auto& target:targets) {
            const auto solved=ik::SolveTwoBone(thigh.translation,target,43,44,{1,0,0});
            const auto q=arm_pose::Orient(ref,thigh.translation,solved,{1,0,0});
            Check(q.valid && Near(q.upper.Rotate(ref.upper_axis),ik::Normalize(solved.mid-thigh.translation)) &&
                  Near(q.forearm.Rotate(ref.forearm_axis),ik::Normalize(solved.end-solved.mid)),
                  "leg frame mapper aligns measured bone axes through bend and extension");
            Check(Near(ik::Length(solved.mid-thigh.translation),43) && Near(ik::Length(solved.end-solved.mid),44),
                  "leg solution retains reference lengths for stance and swing");
        }

        // Exercise the exact absolute-write order needed by body.cpp, with
        // an intervening calf twist bone and two conflicting source poses.
        // The final foot orientation belongs to the foot planner, independently
        // of calf rotation and the arm mapper's forearm_to_hand relationship.
        skel::Xf twist=calf;
        twist.translation={0,sign*10,30};
        twist.rotation=ik::AxisAngle({2,1,-1},sign*0.4);
        skel::Xf locals[]={thigh,skel::Relative(calf,thigh),skel::Relative(twist,calf),skel::Relative(foot,twist)};
        skel::Pose poses[2];
        const FVector target{20,sign*10,20};
        const auto solved=ik::SolveTwoBone(thigh.translation,target,43,44,{1,0,0});
        const auto q=arm_pose::Orient(ref,thigh.translation,solved,{1,0,0});
        const FQuat foot_q=ik::AxisAngle({0,0,1},sign*0.5)*ik::AxisAngle({0,1,0},0.25);
        for (int animation=0; animation<2; ++animation) {
            auto& pose=poses[animation];
            Check(pose.Init({-1,0,1,2}),"leg integration fixture accepts calf twist hierarchy");
            pose.SetAllLocal(locals,4);
            pose.SetRotationC(0,ik::AxisAngle({0,1,0},animation ? -1.1 : 0.7));
            pose.SetRotationC(1,ik::AxisAngle({1,0,0},animation ? 0.8 : -1.2));
            pose.SetRotationC(2,ik::AxisAngle({0,0,1},animation ? -0.9 : 1.3));
            pose.SetRotationC(0,q.upper);
            pose.SetTranslationC(1,solved.mid);
            pose.SetRotationC(1,q.forearm);
            pose.SetTranslationC(3,solved.end);
            pose.SetRotationC(3,foot_q);
            Check(Near(pose.C(1).translation,solved.mid) && Near(pose.C(3).translation,solved.end),
                  "absolute leg writes preserve solved endpoints through a twist bone");
            Check(Near(pose.C(3).rotation.Rotate({1,0,0}),foot_q.Rotate({1,0,0})) &&
                  Near(pose.C(3).rotation.Rotate({0,0,1}),foot_q.Rotate({0,0,1})),
                  "foot world orientation is independent of calf and twist animation");
        }
        Check(Near(poses[0].C(0).rotation.Rotate({1,0,0}),poses[1].C(0).rotation.Rotate({1,0,0})) &&
              Near(poses[0].C(1).rotation.Rotate({0,0,1}),poses[1].C(1).rotation.Rotate({0,0,1})),
              "source terrain knee rotations cannot leak into the mapped thigh and calf");
    }
}
} // namespace
int main() {
    BypassActions(); LocomotionAndContact(); RoomSteps(); GroundSafety(); PlatformsAndResets(); PelvisReach();
    TableCannotBecomeGround(); IdlePelvisGuard(); StairFeet(); LegReferenceFrames();
    std::printf("leg_pose: %d checks passed\n",checks);
}
