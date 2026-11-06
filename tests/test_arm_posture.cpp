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
    for(int side=0;side<2;++side) for(double ratio:{.5,.8,.95,.999}) {
        Input in=Neutral(side,ratio);
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

int main() {
    PalmsFacingAndMirroring(); RejectInwardZeroWristCandidate(); CrossBodyOverheadAndHanging();
    FullReachRetractionAndRates(); YawAndMeasuredLengths(); ExtendedLateralSweep();
    TransportedPoleRateAcrossBody(); OptionsAndInvalidState();
    FreeWristDoesNotMeasureElbow(); ExtensionKeepsTransportedPlane();
    TurningExtendedArmThenRetracting();
    std::printf("arm posture: %s (%d checks, %d failures)\n",failures?"FAIL":"PASS",checks,failures);
    return failures?1:0;
}
