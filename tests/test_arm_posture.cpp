// clang++ -std=c++20 -Isrc tests/test_arm_posture.cpp src/vr/body/arm_posture.cpp
//   src/vr/body/arm_pose.cpp src/vr/body/ik_math.cpp src/vr/body/skeleton.cpp -o test_arm_posture
#include "vr/body/arm_posture.h"

#include <cstdio>
#include <limits>

using namespace holevr::vr;
using namespace holevr::vr::ik;
using namespace holevr::vr::arm_posture;
using skel::Xf;
static int checks=0, failures=0;
#define EXPECT(c) do { ++checks; if (!(c)) { std::printf("FAIL line%d: %s\n",__LINE__,#c); ++failures; } } while(0)
static bool Near(double a,double b,double e=1e-6) { return std::fabs(a-b)<e; }
static double Square(double x) { return x*x; }
static bool Near(const FVector& a,const FVector& b,double e=1e-6) { return Length(a-b)<e; }
static Xf At(FVector p,FQuat q={}) { Xf x; x.translation=p; x.rotation=q; return x; }
static FVector Mirror(FVector p) { return {p.X,-p.Y,p.Z}; }

static Input Neutral(int side,double ratio=.85) {
    const double sign=side==0?-1:1;
    Input in;
    in.shoulder={0,sign*15,0}; in.wrist=in.shoulder+FVector{58*ratio,0,0};
    in.outward={0,sign,0}; in.forward={1,0,0}; in.down={0,0,-1};
    in.upper_cm=30; in.forearm_cm=28; in.dt=1.0/90;
    in.reference=arm_pose::MeasureReference(At(in.shoulder),At(in.shoulder+in.outward*30),
        At(in.shoulder+in.outward*58),in.down);
    in.forearm_axis_hand=in.outward; in.palm_axis_hand={0,0,-1};
    // Thumb side, as the game's rig builds it (index minus little finger).
    in.radial_axis_hand=side==0 ? Cross(in.forearm_axis_hand,in.palm_axis_hand)
                                : Cross(in.palm_axis_hand,in.forearm_axis_hand);
    in.hand_rotation=AlignFrames(in.forearm_axis_hand,in.palm_axis_hand,{1,0,0},in.outward*-1);
    in.torso_center_valid=true;
    return in;
}
static void Geometry(const Input& in,const Result& r) {
    EXPECT(r.valid && r.rotations.valid);
    if (!r.valid) return;
    EXPECT(Near(Length(r.solved.mid-in.shoulder),in.upper_cm,1e-5));
    EXPECT(Near(Length(r.solved.end-r.solved.mid),in.forearm_cm,1e-5));
    EXPECT(Near(r.solved.end,in.wrist,1e-5));
    EXPECT(Near(r.rotations.upper.Rotate(in.reference.upper_axis),Normalize(r.solved.mid-in.shoulder)));
    EXPECT(Near(r.rotations.forearm.Rotate(in.reference.forearm_axis),Normalize(r.solved.end-r.solved.mid)));
    EXPECT(std::isfinite(r.cost) && r.candidates>0 && r.candidates<=64);
    EXPECT(std::fabs(r.raw_roll)<=kPi+1e-8);
    EXPECT(Near(Length(r.pole),1) && std::fabs(Dot(r.pole,Normalize(in.wrist-in.shoulder)))<1e-6);
}

static void PalmsFacingAndMirroring() {
    for (double ratio:{.50,.80,.90,.98,1.0}) {
        Result result[2];
        for (int side=0;side<2;++side) {
            const Input in=Neutral(side,ratio);
            result[side]=Solve(in,nullptr); Geometry(in,result[side]);
            EXPECT(Dot(result[side].pole,in.outward)>=-1e-8);
            EXPECT(Dot(result[side].solved.mid,in.outward)>=3-1e-6);
        }
        EXPECT(Near(result[0].solved.mid,Mirror(result[1].solved.mid),1e-5));
        EXPECT(Near(result[0].pole,Mirror(result[1].pole),1e-5));
        EXPECT(Near(result[0].cost,result[1].cost,1e-6));
    }
}

static void RejectInwardZeroWristCandidate() {
    for (int side=0;side<2;++side) {
        Input in=Neutral(side,.69);
        const FVector inward=in.outward*-1;
        const auto old=SolveTwoBone(in.shoulder,in.wrist,in.upper_cm,in.forearm_cm,inward);
        const auto frame=arm_pose::Orient(in.reference,in.shoulder,old,inward);
        in.hand_rotation=Normalized(frame.forearm*in.reference.forearm_to_hand);
        const auto aligned=arm_pose::WristAlignedPole(in.reference,in.shoulder,in.wrist,in.hand_rotation);
        EXPECT(aligned.valid && Dot(aligned.pole,in.outward)<-.99);
        EXPECT(Dot(old.mid,in.outward)<0); // the old zero-wrist-bend answer crosses the torso midline
        const auto r=Solve(in,nullptr); Geometry(in,r);
        EXPECT(Dot(r.pole,in.outward)>=-1e-8);
        EXPECT(Dot(r.solved.mid,in.outward)>=3-1e-6);
        in.straight_wrist=true; // even an explicit authored grip cannot move the elbow through the body
        const auto authored=Solve(in,nullptr); Geometry(in,authored);
        EXPECT(authored.constrained && Dot(authored.pole,in.outward)>=-1e-8);
        // Neither configuration nor a stale unsafe pole can bypass the domain.
        in.preferred_pole=inward;
        State stale{true,{1,0,0},inward};
        const auto safe=Solve(in,&stale); Geometry(in,safe);
        EXPECT(Dot(safe.pole,in.outward)>=-1e-8);
    }
}

static void CrossBodyOverheadAndHanging() {
    for (int side=0;side<2;++side) for (FVector offset:{FVector{30,30,8},FVector{8,7,53},
            FVector{5,-8,-52},FVector{20,45,0},FVector{0,55,0}}) {
        Input in=Neutral(side);
        if (side==1) offset.Y=-offset.Y;
        in.wrist=in.shoulder+offset;
        in.hand_rotation=Normalized(ToQuat({30,side==0?50.0:-50.0,-20})*in.hand_rotation);
        State state;
        const auto r=Solve(in,&state); Geometry(in,r);
        EXPECT(state.valid);
    }
}

static void FullReachRetractionAndRates() {
    for (double hz:{7.0,30.0,72.0,144.0}) for (int side=0;side<2;++side) {
        Input in=Neutral(side,.8); in.dt=1/hz;
        State state;
        FVector previous;
        bool had_previous=false;
        for (int i=0;i<static_cast<int>(hz*2);++i) {
            const double t=i/hz;
            const double ratio=t<1 ? .8+.2*t : 1-.2*(t-1);
            in.wrist=in.shoulder+FVector{58*ratio,0,0};
            const double roll=80*kDegToRad*std::sin(t*2);
            const Input base=Neutral(side,ratio);
            in.hand_rotation=Normalized(AxisAngle({1,0,0},roll)*base.hand_rotation);
            const auto r=Solve(in,&state); Geometry(in,r);
            EXPECT(Dot(r.pole,in.outward)>=-1e-8);
            if (had_previous) EXPECT(AngleBetween(previous,r.pole)<=360*kDegToRad*in.dt+1e-6);
            previous=r.pole; had_previous=true;
        }
    }
    // At full extension neither hand roll nor an optional hand-follow term
    // can choose a different elbow plane; retraction therefore has a reference.
    for (int side=0;side<2;++side) {
        Input in=Neutral(side,1);
        const auto base=Solve(in,nullptr);
        for (double roll:{-179.0,-90.0,0.0,90.0,179.0}) {
            in.hand_rotation=AxisAngle({1,0,0},roll*kDegToRad)*Neutral(side,1).hand_rotation;
            in.preferred_pole=AxisAngle({1,0,0},roll*kDegToRad).Rotate(in.outward);
            const auto r=Solve(in,nullptr); Geometry(in,r);
            EXPECT(Near(r.pole,base.pole));
        }
    }
}

static void YawAndMeasuredLengths() {
    const FQuat turn=YawOnly(137);
    for (int side=0;side<2;++side) {
        Input in=Neutral(side);
        in.upper_cm=side==0?33.6323529389:35.2088780049;
        in.forearm_cm=side==0?26.3728101166:25.0713780835;
        in.wrist=in.shoulder+FVector{(in.upper_cm+in.forearm_cm)*.88,0,0};
        const auto a=Solve(in,nullptr); Geometry(in,a);
        Input spun=in;
        spun.shoulder=turn.Rotate(in.shoulder); spun.wrist=turn.Rotate(in.wrist);
        spun.outward=turn.Rotate(in.outward); spun.forward=turn.Rotate(in.forward);
        spun.down=turn.Rotate(in.down); spun.hand_rotation=turn*in.hand_rotation;
        const auto b=Solve(spun,nullptr); Geometry(spun,b);
        EXPECT(Near(b.solved.mid,turn.Rotate(a.solved.mid),1e-5));
        EXPECT(Near(b.pole,turn.Rotate(a.pole),1e-5));
        EXPECT(Near(b.raw_roll,a.raw_roll,1e-6));
    }
}

static void ExtendedLateralSweep() {
    for (int side=0;side<2;++side) {
        Input in=Neutral(side,1);
        State state;
        FVector previous;
        for (int i=0;i<=160;++i) {
            const double angle=(70+i*.25)*kDegToRad;
            in.wrist=in.shoulder+(in.forward*std::cos(angle)+in.outward*std::sin(angle))*58;
            const auto r=Solve(in,&state); Geometry(in,r);
            if (i) EXPECT(AngleBetween(previous,r.pole)<5*kDegToRad);
            previous=r.pole;
        }
    }
}

static void TransportedPoleRateAcrossBody() {
    // The reviewer's exact reproduction: a0.1-degree sweep across a lateral
    // cross-body direction used to jump45degrees when the outward projection
    // crossed the .05 cutoff, despite a4-degree/frame rate bound.
    for (double ratio:{.60,.85,.98}) for (double elevation:{-60.0,0.0,60.0}) {
        Input in=Neutral(1,ratio);
        State state;
        FVector previous_axis,previous_pole;
        for (int n=-1800;n<=1800;++n) {
            const FQuat facing=ToQuat({elevation,n*.1,0});
            const FVector axis=facing.Rotate({1,0,0});
            in.wrist=in.shoulder+axis*(58*ratio);
            in.hand_rotation=AlignFrames(in.forearm_axis_hand,in.palm_axis_hand,axis,facing.Rotate({0,-1,0}));
            const auto r=Solve(in,&state);
            EXPECT(r.valid && Near(r.solved.end,in.wrist,1e-5));
            if (n>-1800) EXPECT(AngleBetween(Arc(previous_axis,axis).Rotate(previous_pole),r.pole)
                                <=360*kDegToRad*in.dt+1e-7);
            previous_axis=axis; previous_pole=r.pole;
        }
    }
}

static void OptionsAndInvalidState() {
    Input in=Neutral(0,.85);
    in.hand_rotation=Normalized(ToQuat({-55,80,40})*in.hand_rotation);
    in.wrist_relief=false;
    const auto plain=Solve(in,nullptr); Geometry(in,plain);
    in.straight_wrist=true;
    const auto straight=Solve(in,nullptr); Geometry(in,straight);
    EXPECT(Square(straight.flex)+2*Square(straight.deviation)<=Square(plain.flex)+2*Square(plain.deviation)+1e-6);
    State state{true,{0,0,0},{0,0,0}};
    const auto recovered=Solve(in,&state);
    EXPECT(recovered.valid && state.valid);
    const double nan=std::numeric_limits<double>::quiet_NaN();
    in.wrist.X=nan;
    EXPECT(!Solve(in,&state).valid && !state.valid);
    in=Neutral(0); in.dt=nan; EXPECT(!Solve(in,&state).valid);
    in=Neutral(0); in.palm_axis_hand=in.forearm_axis_hand; EXPECT(!Solve(in,&state).valid);
    in=Neutral(0); in.outward=in.forward; EXPECT(!Solve(in,&state).valid);
    in=Neutral(0); in.reference.forearm_axis={}; EXPECT(!Solve(in,&state).valid);
    in=Neutral(0); state={true,{1,0,0},{0,0,-1}}; in.dt=1;
    const auto gap=Solve(in,&state),fresh=Solve(in,nullptr);
    EXPECT(Near(gap.pole,fresh.pole));
}

static void FreeWristDoesNotMeasureElbow() {
    // A tracked hand may flex, deviate, or pronate while its position stays
    // fixed. None of those observations measures a new elbow location.
    // Previously even wrist_relief=false allowed a hand roll to move the
    // elbow28.3cm at80% reach because a separate roll cost remained active.
    // Without joint limits (body_arm_joint_limits=0). With them, a turn the
    // forearm or wrist cannot make moves the elbow: JointLimitsMoveElbow.
    for(int side=0;side<2;++side) for(double ratio:{.5,.8,.95,.999}) {
        Input in=Neutral(side,ratio);
        in.joint_limits=false;
        EXPECT(!in.straight_wrist && !in.wrist_relief);
        const Input fixed=in;
        const auto base=Solve(in,nullptr);
        State moving_state,stationary_state;
        bool saw_wrist_bend=false;
        for(const FVector axis:{FVector{1,0,0},FVector{0,1,0},FVector{0,0,1}})
            for(int degrees=-180;degrees<=180;degrees+=5) {
                in.hand_rotation=AxisAngle(axis,degrees*kDegToRad)*fixed.hand_rotation;
                const auto out=Solve(in,nullptr); Geometry(in,out);
                EXPECT(Near(out.solved.mid,base.solved.mid));
                EXPECT(Near(out.pole,base.pole));
                const auto moving=Solve(in,&moving_state),stationary=Solve(fixed,&stationary_state);
                EXPECT(Near(moving.solved.mid,stationary.solved.mid));
                EXPECT(Near(moving.pole,stationary.pole));
                saw_wrist_bend |= std::fabs(out.flex)>55*kDegToRad || std::fabs(out.deviation)>25*kDegToRad;
            }
        EXPECT(saw_wrist_bend); // diagnostics report motion instead of silently correcting it through the elbow
    }
}

static void ExtensionKeepsTransportedPlane() {
    for(double hz:{7.0,30.0,90.0,144.0}) for(int side=0;side<2;++side) {
        Input in=Neutral(side,1); in.dt=1/hz;
        State state{true,{1,0,0},Normalize(in.outward+FVector{0,0,-.1})};
        const FVector original=state.pole;
        for(int i=0;i<static_cast<int>(2*hz);++i) {
            const auto out=Solve(in,&state); Geometry(in,out);
            EXPECT(state.extension_locked && Near(out.pole,original));
        }
        // A2cm change in inferred shoulder location can move a nearly straight
        // triangle considerably. Here small direction/reach noise must not
        // invent an elbow-plane rotation; compare with parallel transport.
        for(int i=0;i<static_cast<int>(2*hz);++i) {
            const FVector axis=Normalize(FVector{1,.004*std::sin(i*.7),.004*std::cos(i*.9)});
            const FVector expected=Arc(state.axis,axis).Rotate(state.pole);
            const double ratio=.991+.007*std::sin(i*.6); // repeatedly crosses entry threshold without unlocking
            in.wrist=in.shoulder+axis*(58*ratio);
            in.hand_rotation=ToQuat({i*13.0,i*-11.0,i*17.0});
            const auto out=Solve(in,&state); Geometry(in,out);
            EXPECT(state.extension_locked && Near(out.pole,expected,1e-6));
        }
        const FVector previous=state.pole,previous_axis=state.axis;
        in.wrist=in.shoulder+FVector{58*.96,0,0};
        const auto retracted=Solve(in,&state); Geometry(in,retracted);
        EXPECT(!state.extension_locked);
        EXPECT(AngleBetween(Arc(previous_axis,state.axis).Rotate(previous),retracted.pole)<=360*kDegToRad*in.dt+1e-6);
        // Explicit state reset remains the authority after tracking recovery.
        state={}; const auto recovered=Solve(in,&state),fresh=Solve(in,nullptr);
        EXPECT(Near(recovered.pole,fresh.pole));
    }
}

static void TurningExtendedArmThenRetracting() {
    // A plane transported while fully straight can legitimately leave the
    // body's preferred interval. A hard clamp on retraction caused a35degree
    // snap in one frame; rejoin gradually instead of inventing an observation.
    for(int side=0;side<2;++side) for(int sweep=-180;sweep<=180;sweep+=10) {
        Input in=Neutral(side,1); State state;
        for(int n=0;n<=90;++n) {
            in.wrist=in.shoulder+YawOnly(sweep*n/90.0).Rotate({58,0,0});
            Geometry(in,Solve(in,&state));
        }
        for(int n=1;n<=225;++n) {
            const FVector previous=state.pole,axis=state.axis;
            in.wrist=in.shoulder+YawOnly(sweep).Rotate({58*(1-.2*std::min(n,45)/45),0,0});
            const auto out=Solve(in,&state); Geometry(in,out);
            EXPECT(AngleBetween(Arc(axis,state.axis).Rotate(previous),out.pole)<=360*kDegToRad*in.dt+1e-6);
        }
        EXPECT(!state.extension_locked && !state.extension_recovering);
    }
}

// Owner 2026-11-07: "the only time humans elbows go above our head is when
// we reach straight up. right now the elbows loving going up when I'm
// holding the AR and I'm aiming it to shoot." Positions from that run's arm
// samples (07:46:14-29, body yaw frame, cm from the shoulder), with a
// synthetic hand frame: no controller here measures the real palm axes.
static Input Shouldered(int side,FVector wrist_local,double upper,double forearm) {
    Input in=Neutral(side);
    const double sign=side==0?-1:1;
    in.upper_cm=upper; in.forearm_cm=forearm;
    in.wrist=in.shoulder+FVector{wrist_local.X,sign*wrist_local.Y,wrist_local.Z};
    in.reference=arm_pose::MeasureReference(At(in.shoulder),At(in.shoulder+in.outward*upper),
        At(in.shoulder+in.outward*(upper+forearm)),in.down);
    // Gun hand: knuckles forward, palm toward the gun (medial), thumb up.
    in.hand_rotation=AlignFrames(in.forearm_axis_hand,in.palm_axis_hand,{1,0,0},in.outward*-1);
    return in;
}
static double Up(const Input& in,const Result& r) { return Dot(r.solved.mid-in.shoulder,in.down*-1); }
static Result Settled(Input in) { State state; Result r; for(int i=0;i<200;++i) r=Solve(in,&state); return r; }

static void RifleAimKeepsElbowsDown() {
    for(int side=0;side<2;++side) {
        // Trigger hand at the face: 13 cm ahead of and 6 cm above the shoulder.
        Input trigger=Shouldered(side,{13.3,-.3,6.4},33.11,29.44);
        // Support hand on the handguard, low and across toward the midline.
        Input support=Shouldered(side,{32.6,-17.9,-.8},38.62,20.82);
        support.hand_rotation=AlignFrames(support.forearm_axis_hand,support.palm_axis_hand,
            {1,0,0},Normalize(support.outward*-.5+FVector{0,0,1}));
        for(Input in:{trigger,support}) for(int mode=0;mode<3;++mode) {
            in.wrist_relief=mode>=1; in.straight_wrist=mode==2;
            Input old=in; old.joint_limits=false;
            const auto before=Settled(old), after=Settled(in);
            Geometry(in,after);
            EXPECT(after.elbow_over_cm<1.0);
            EXPECT(Up(in,after)<=after.elbow_cap_cm+1.0);
            EXPECT(Up(in,after)<.15*in.upper_cm+1.0);
            // The trigger elbow stays outside the shoulder; the support
            // hand reaches across, so only compare it with the old solver.
            if(in.upper_cm<35) EXPECT(Dot(after.solved.mid-in.shoulder,in.outward)>=-1e-6);
            else EXPECT(after.torso_clearance_cm>=std::min(3.0,before.torso_clearance_cm)-1e-5);
            if(side==1) std::printf("rifle %s, %s: elbow %.1f cm above the shoulder (without limits %.1f), cap %.1f; out %.1f (without %.1f)\n",
                in.upper_cm>35?"support":"trigger",mode==0?"free":mode==1?"wrist relief":"straight wrist",
                Up(in,after),Up(old,before),after.elbow_cap_cm,Dot(after.solved.mid-in.shoulder,in.outward),
                Dot(before.solved.mid-in.shoulder,in.outward));
        }
    }
}

static void RaisedHandsStillRaiseElbows() {
    for(int side=0;side<2;++side) {
        // Hand on top of the head and behind the head (reaching over the
        // shoulder): elbows point up there, as a person's do.
        for(FVector local:{FVector{0,-12,30},FVector{-10,-5,18},FVector{5,5,52}}) {
            Input in=Shouldered(side,local,30,28);
            in.hand_rotation=AlignFrames(in.forearm_axis_hand,in.palm_axis_hand,{0,0,1},in.forward*-1);
            const auto r=Settled(in); Geometry(in,r);
            EXPECT(r.elbow_cap_cm>10);
            EXPECT(r.elbow_over_cm<1.0);
            if(side==1) std::printf("raised hand (%.0f %.0f %.0f): elbow %.1f cm up, cap %.1f\n",
                local.X,local.Y,local.Z,Up(in,r),r.elbow_cap_cm);
        }
    }
}

static void ExtensionLockWithLimits() {
    // A straight arm aimed a little above the shoulder (a pistol at head
    // height) has its elbow over the height limit on every plane: the
    // carried plane must still hold through noise, not chase it.
    for(double hz:{30.0,90.0}) for(int side=0;side<2;++side) {
        Input in=Neutral(side,1); in.dt=1/hz;
        const FVector raised=Normalize(FVector{1,0,.25});
        in.wrist=in.shoulder+raised*58;
        State state; Solve(in,&state);
        for(int i=0;i<static_cast<int>(2*hz);++i) {
            const FVector axis=Normalize(raised+FVector{0,.004*std::sin(i*.7),.004*std::cos(i*.9)});
            const FVector expected=Arc(state.axis,axis).Rotate(state.pole);
            in.wrist=in.shoulder+axis*(58*(.991+.007*std::sin(i*.6)));
            const auto out=Solve(in,&state); Geometry(in,out);
            EXPECT(state.extension_locked && Near(out.pole,expected,1e-6));
        }
    }
    // A plane carried straight with the elbow pointing up is let go when the
    // arm bends, at the usual rate, and the elbow comes back under its limit.
    for(double hz:{30.0,90.0}) for(int side=0;side<2;++side) {
        Input in=Neutral(side,1); in.dt=1/hz;
        State state{true,{1,0,0},{0,0,1}};
        Solve(in,&state);
        Result out;
        for(int n=1;n<=static_cast<int>(hz);++n) {
            const FVector previous=state.pole,previous_axis=state.axis;
            in.wrist=in.shoulder+FVector{58*(1-.35*std::min(1.0,n/(.3*hz))),0,0};
            out=Solve(in,&state); Geometry(in,out);
            EXPECT(AngleBetween(Arc(previous_axis,state.axis).Rotate(previous),out.pole)<=360*kDegToRad*in.dt+1e-6);
        }
        EXPECT(out.elbow_over_cm<.5 && Up(in,out)<0);
    }
}

static void ForearmRotationMeasure() {
    // Right arm hanging, forearm forward: palm up = supinated (0), palm
    // toward the body = thumb up (90), palm down = pronated (180). Mirrored
    // for the left arm.
    for(int side=0;side<2;++side) {
        const double s=side==0?-1:1;
        const FVector upper{0,0,-30},forearm{28,0,0},pole{0,0,-1};
        EXPECT(Near(arm_pose::ForearmRotation(upper,forearm,{0,0,1},pole,s),0,1e-9));
        EXPECT(Near(arm_pose::ForearmRotation(upper,forearm,{0,-s,0},pole,s),kPi/2,1e-9));
        EXPECT(Near(std::fabs(arm_pose::ForearmRotation(upper,forearm,{0,0,-1},pole,s)),kPi,1e-9));
        EXPECT(Near(arm_pose::ForearmRotation(upper,forearm,{0,s,0},pole,s),-kPi/2,1e-9));
        // A straight arm uses the pole: hanging straight, elbow pointing back.
        EXPECT(Near(arm_pose::ForearmRotation({0,0,-30},{0,0,-28},{1,0,0},{-1,0,0},s),0,1e-9));
        EXPECT(!std::isfinite(arm_pose::ForearmRotation(upper,forearm,{1,0,0},pole,s)));
    }
    // The raw IK roll and the anatomical turn agree: raw = s*(rho - rho_ref).
    for(int side=0;side<2;++side) for(double ratio:{.6,.9}) for(double roll:{-120.0,-45.0,0.0,60.0,150.0}) {
        Input in=Neutral(side,ratio);
        const double s=side==0?-1:1;
        const double rest=arm_pose::ReferenceForearmRotation(in.reference,in.palm_axis_hand,s);
        EXPECT(std::isfinite(rest));
        const auto solved=SolveTwoBone(in.shoulder,in.wrist,in.upper_cm,in.forearm_cm,in.down);
        const auto frames=arm_pose::Orient(in.reference,in.shoulder,solved,in.down);
        const FVector f=Normalize(solved.end-solved.mid);
        const FQuat hand=Normalized(AxisAngle(f,roll*kDegToRad)*frames.forearm*in.reference.forearm_to_hand);
        const double raw=arm_pose::WristTwist(in.reference,frames.forearm,hand);
        const double rho=arm_pose::ForearmRotation(solved.mid-in.shoulder,solved.end-solved.mid,
            hand.Rotate(in.palm_axis_hand),in.down,s);
        EXPECT(Near(std::remainder(raw-roll*kDegToRad,2*kPi),0,1e-6));
        EXPECT(Near(std::remainder(s*(rho-rest)-raw,2*kPi),0,1e-6));
    }
}

static double TurnOutside(double rho) {
    // Radians outside the forearm's own range [-10, 180] deg.
    const double end=-10*kDegToRad;
    return rho<end ? std::min(end-rho,rho+kPi) : 0;
}

// A hand in line with the forearm the solver chose without limits, thumb
// up (palm toward the body's midline), then turned roll_degrees about the
// forearm: positive turns toward palm-down for either arm.
static Input NaturalHand(int side,double ratio,double roll_degrees,Result* free_base=nullptr) {
    Input in=Neutral(side,ratio);
    Input off=in; off.joint_limits=false;
    const auto base=Settled(off);
    if(free_base) *free_base=base;
    const double s=side==0?-1:1;
    const FVector upper=base.solved.mid-in.shoulder, f=Normalize(base.solved.end-base.solved.mid);
    const FVector medial=Normalize(Cross(upper,f))*s;
    const FQuat straight=AlignFrames(in.forearm_axis_hand,in.palm_axis_hand,f,medial);
    in.hand_rotation=Normalized(AxisAngle(f,s*roll_degrees*kDegToRad)*straight);
    return in;
}

static void JointLimitsMoveElbow() {
    for(int side=0;side<2;++side) for(double ratio:{.55,.8}) {
        Result free_base;
        const auto natural=Settled(NaturalHand(side,ratio,0,&free_base));
        // A natural hand changes nothing.
        EXPECT(Length(natural.solved.mid-free_base.solved.mid)<1.0);
        EXPECT(Near(natural.forearm_rotation,kPi/2,2*kDegToRad));
        double most_inside=0;
        for(int degrees=-180;degrees<=180;degrees+=15) {
            const Input in=NaturalHand(side,ratio,degrees);
            Input free=in; free.joint_limits=false;
            const auto limited=Settled(in), unlimited=Settled(free);
            Geometry(in,limited);
            EXPECT(limited.elbow_over_cm<1.0);
            EXPECT(std::isfinite(limited.forearm_rotation) && std::isfinite(unlimited.forearm_rotation));
            if(!std::isfinite(limited.forearm_rotation) || !std::isfinite(unlimited.forearm_rotation)) continue;
            const double outside=TurnOutside(unlimited.forearm_rotation);
            const double moved=Length(limited.solved.mid-unlimited.solved.mid);
            if(outside<1e-9) {
                // In range the shoulder takes only a small share.
                most_inside=std::max(most_inside,moved);
                EXPECT(moved<.2*in.upper_cm);
            } else {
                // Never worse. Past the palm-down end the shoulder takes most
                // of the rest (the elbow rises and swings out; the wrist's own
                // range shares the work with a folded arm). Past the
                // palm-up end it would have to tuck across the chest, which
                // the torso guard keeps out, or swing round the long way.
                EXPECT(TurnOutside(limited.forearm_rotation)<=outside+1e-6);
                // Unless raising the elbow further would pass its height limit
                // (the hand here is no higher than the shoulder).
                if(unlimited.forearm_rotation<-kPi/2 && limited.elbow_height_cm<limited.elbow_cap_cm-2) {
                    EXPECT(TurnOutside(limited.forearm_rotation)<outside*.7+1e-6);
                    if(TurnOutside(limited.forearm_rotation)>=outside*.7+1e-6)
                        std::printf("  side %d reach %.2f turn %d: alone %.0f, limited %.0f, elbow %.1f cap %.1f\n",side,ratio,degrees,
                            unlimited.forearm_rotation*kRadToDeg,limited.forearm_rotation*kRadToDeg,
                            limited.elbow_height_cm,limited.elbow_cap_cm);
                }
            }
            if(side==1 && ratio==.8) std::printf("turn %+4d: forearm %+4.0f deg (alone %+4.0f), elbow moved %.1f cm\n",
                degrees,limited.forearm_rotation*kRadToDeg,unlimited.forearm_rotation*kRadToDeg,moved);
        }
        if(side==1) std::printf("hand turns within the forearm's range move the elbow at most %.1f cm (reach %.2f)\n",
                                most_inside,ratio);
    }
    // Continuity under a full roll sweep with limits on.
    for(double hz:{30.0,90.0}) for(int side=0;side<2;++side) {
        Input in=NaturalHand(side,.75,0); in.dt=1/hz;
        const FQuat start=in.hand_rotation;
        State state; FVector previous; bool have=false;
        const FVector axis=Normalize(in.wrist-in.shoulder);
        for(int i=0;i<static_cast<int>(4*hz);++i) {
            in.hand_rotation=Normalized(AxisAngle(axis,170*kDegToRad*std::sin(i/hz*1.7))*start);
            const auto r=Solve(in,&state); Geometry(in,r);
            if(have) EXPECT(AngleBetween(previous,r.pole)<=360*kDegToRad*in.dt+1e-6);
            previous=r.pole; have=true;
        }
    }
    // Wrist bent past its range: the elbow helps.
    for(int side=0;side<2;++side) for(double degrees:{-110.0,-95.0,95.0,110.0}) {
        Result free_base;
        const Input natural=NaturalHand(side,.7,0,&free_base);
        const FVector f=Normalize(free_base.solved.end-free_base.solved.mid);
        const FVector palm=natural.hand_rotation.Rotate(natural.palm_axis_hand);
        Input bent=natural;
        bent.hand_rotation=Normalized(AxisAngle(Cross(f,palm),degrees*kDegToRad)*natural.hand_rotation);
        Input free=bent; free.joint_limits=false;
        const auto limited=Settled(bent), unlimited=Settled(free);
        Geometry(bent,limited);
        const auto over=[](const Result& r) {
            return std::max(0.0,r.flex-70*kDegToRad)+std::max(0.0,-r.flex-80*kDegToRad); };
        EXPECT(over(unlimited)>0);
        EXPECT(over(limited)<over(unlimited)-1e-3);
        if(side==1) std::printf("wrist bent %+.0f: past its range by %.0f deg (alone %.0f)\n",degrees,
                                over(limited)*kRadToDeg,over(unlimited)*kRadToDeg);
    }
}

int main() {
    PalmsFacingAndMirroring(); RejectInwardZeroWristCandidate(); CrossBodyOverheadAndHanging();
    FullReachRetractionAndRates(); YawAndMeasuredLengths(); ExtendedLateralSweep();
    TransportedPoleRateAcrossBody(); OptionsAndInvalidState();
    FreeWristDoesNotMeasureElbow(); ExtensionKeepsTransportedPlane();
    TurningExtendedArmThenRetracting();
    RifleAimKeepsElbowsDown(); RaisedHandsStillRaiseElbows(); ExtensionLockWithLimits(); ForearmRotationMeasure();
    JointLimitsMoveElbow();
    std::printf("arm posture: %s (%d checks, %d failures)\n",failures?"FAIL":"PASS",checks,failures);
    return failures?1:0;
}
