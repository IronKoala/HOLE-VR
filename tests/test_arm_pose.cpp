// clang++ -std=c++20 -Isrc tests/test_arm_pose.cpp src/vr/body/arm_pose.cpp
//   src/vr/body/ik_math.cpp src/vr/body/skeleton.cpp -o test_arm_pose
#include "vr/body/arm_pose.h"

#include <algorithm>
#include <cstdio>
#include <limits>

using namespace holevr::vr;
using namespace holevr::vr::ik;
using namespace holevr::vr::arm_pose;
using skel::Xf;

static int failures = 0;
#define EXPECT(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static bool Near(const FVector& a, const FVector& b, double e = 1e-6) { return Length(a - b) < e; }
static bool Near(double a, double b, double e = 1e-6) { return std::fabs(a - b) < e; }
static bool SameRotation(const FQuat& a, const FQuat& b) {
    return Near(a.Rotate({1, 0, 0}), b.Rotate({1, 0, 0})) && Near(a.Rotate({0, 1, 0}), b.Rotate({0, 1, 0}));
}
static bool Finite(const FQuat& q) {
    return std::isfinite(q.X) && std::isfinite(q.Y) && std::isfinite(q.Z) && std::isfinite(q.W);
}
static Xf At(const FVector& p, const FQuat& q = FQuat{}) { Xf t; t.translation = p; t.rotation = q; return t; }
static Xf Turn(const Xf& t, const FQuat& q) { return At(q.Rotate(t.translation), Normalized(q * t.rotation)); }

static void AssertAxes(const Reference& r, const Rotations& pose, const FVector& shoulder, const TwoBoneResult& s) {
    EXPECT(pose.valid);
    EXPECT(Finite(pose.upper) && Finite(pose.forearm));
    EXPECT(Near(pose.upper.Rotate(r.upper_axis), Normalize(s.mid - shoulder)));
    EXPECT(Near(pose.forearm.Rotate(r.forearm_axis), Normalize(s.end - s.mid)));
}

static void MirroredAndArbitraryBoneFrames() {
    FVector mids[2], ends[2];
    for (int side = 0; side < 2; ++side) {
        const double sign = side == 0 ? -1.0 : 1.0;
        // Bone axes deliberately do not follow any common UE/Mixamo convention.
        const Xf u = At({0, sign * 18, 140}, ToQuat({17, sign * 31, -52}));
        const Xf f = At(u.translation + FVector{0, sign * 30, 0}, ToQuat({-43, 11, sign * 72}));
        const Xf h = At(f.translation + FVector{0, sign * 28, 0}, ToQuat({21, -90, sign * 45}));
        const Reference r = MeasureReference(u, f, h, {1, 0, 0});
        EXPECT(r.valid && Near(r.upper_length, 30) && Near(r.forearm_length, 28));
        const FVector pole{0, sign * 0.4, -1};
        const TwoBoneResult solved = SolveTwoBone(u.translation, u.translation + FVector{40, sign * 15, -15}, 30, 28, pole);
        const Rotations p = Orient(r, u.translation, solved, pole);
        AssertAxes(r, p, u.translation, solved);
        const FQuat neutral_hand = Normalized(p.forearm * r.forearm_to_hand);
        EXPECT(Near(WristTwist(r, p.forearm, neutral_hand), 0));
        const FQuat rolled_hand = Normalized(p.forearm * AxisAngle(r.forearm_axis, 0.4) * r.forearm_to_hand);
        EXPECT(Near(WristTwist(r, p.forearm, rolled_hand), 0.4));
        mids[side] = solved.mid - u.translation;
        ends[side] = solved.end - u.translation;
    }
    EXPECT(Near(mids[0], {mids[1].X, -mids[1].Y, mids[1].Z}));
    EXPECT(Near(ends[0], {ends[1].X, -ends[1].Y, ends[1].Z}));
}

static void InterveningTwistParent() {
    const Xf u = At({0, 0, 0});
    const Xf f = At({30, 0, 0});
    const FQuat intermediate = AxisAngle({1, 0, 0}, 50 * kDegToRad);
    const FQuat hand_local = AxisAngle({1, 0, 0}, 20 * kDegToRad);
    const Xf h = At({58, 0, 0}, intermediate * hand_local);
    const Reference r = MeasureReference(u, f, h, {0, 0, -1});
    const FQuat current_fore = ToQuat({12, 40, -20});
    const FQuat target = current_fore * h.rotation;
    EXPECT(Near(WristTwist(r, current_fore, target), 0));
    // The former immediate-parent-local subtraction reports a spurious 50 degrees.
    EXPECT(Near(TwistAngle(current_fore.Inverse() * target * hand_local.Inverse(), {1, 0, 0}), 50 * kDegToRad));
}

static void ReferenceRoundTripAndExtension() {
    const Xf u = At({2, 3, 4}, ToQuat({37, -80, 19}));
    const Xf f = At({30, 5, 8}, ToQuat({-14, 47, 91}));
    const Xf h = At({51, 17, -2}, ToQuat({31, -73, 8}));
    const Reference bent = MeasureReference(u, f, h, {0, 0, -1});
    const Rotations original = Orient(bent, u.translation, {f.translation, h.translation, true}, {0, 0, -1});
    EXPECT(SameRotation(original.upper, u.rotation));
    EXPECT(SameRotation(original.forearm, f.rotation));
    const Reference straight = MeasureReference(At({}), At({30, 0, 0}), At({58, 0, 0}), {0, 0, -1});
    const FVector pole{0, 0, -1};
    const Rotations before = Orient(straight, {}, SolveTwoBone({}, {58 - 1e-8, 0, 0}, 30, 28, pole), pole);
    const Rotations at = Orient(straight, {}, SolveTwoBone({}, {58, 0, 0}, 30, 28, pole), pole);
    EXPECT(AngleBetween(before.upper.Rotate({0, 1, 0}), at.upper.Rotate({0, 1, 0})) < 1e-3);
    EXPECT(AngleBetween(before.forearm.Rotate({0, 1, 0}), at.forearm.Rotate({0, 1, 0})) < 1e-3);
}

static void DegenerateAndStraightTargets() {
    const Xf u = At({0, 0, 0}, ToQuat({11, 22, 33}));
    const Xf f = At({30, 0, 0}, ToQuat({-11, 37, 3}));
    const Xf h = At({58, 0, 0}, ToQuat({8, 9, -70}));
    const Reference r = MeasureReference(u, f, h, {1, 0, 0}); // parallel anatomical hint
    EXPECT(r.valid);
    const Reference repeat = MeasureReference(u, f, h, {1, 0, 0});
    EXPECT(Near(r.upper_normal, repeat.upper_normal));
    for (FVector target : {FVector{58, 0, 0}, FVector{100, 0, 0}, FVector{0, 0, 0}, FVector{1e-10, 0, 0}}) {
        const FVector pole{0,0,-1};
        const TwoBoneResult solved = SolveTwoBone({}, target, 30, 28, pole);
        AssertAxes(r, Orient(r, {}, solved, pole), {}, solved);
        EXPECT(Near(Length(solved.mid), 30));
        EXPECT(Near(Length(solved.end - solved.mid), 28));
    }
    // A tiny opposite reference bend is export noise, not a 180-degree roll instruction.
    const Reference tiny = MeasureReference(u, At({30, 0, 1e-7}, f.rotation), h, {0, 0, -1});
    const Reference flat = MeasureReference(u, f, h, {0, 0, -1});
    EXPECT(Near(tiny.upper_normal, flat.upper_normal));
    EXPECT(!MeasureReference(u, u, h, {0, 0, -1}).valid);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT(!MeasureReference(At({nan, 0, 0}), f, h, {0, 0, -1}).valid);
    EXPECT(!Orient(r, {}, {{nan, 0, 0}, {1, 0, 0}, false}, {0, 0, -1}).valid);
}

static void YawCovariance() {
    const FQuat yaw = YawOnly(137);
    const Xf u = At({3, -18, 130}, ToQuat({10, 20, 30}));
    const Xf f = At({3, -48, 130}, ToQuat({40, 50, 60}));
    const Xf h = At({3, -76, 130}, ToQuat({70, 80, 90}));
    const Reference a = MeasureReference(u, f, h, {1, 0, 0});
    const Reference b = MeasureReference(Turn(u, yaw), Turn(f, yaw), Turn(h, yaw), yaw.Rotate({1, 0, 0}));
    EXPECT(Near(a.upper_axis, b.upper_axis) && Near(a.upper_normal, b.upper_normal));
    EXPECT(SameRotation(a.forearm_to_hand, b.forearm_to_hand));
    const FVector target = u.translation + FVector{35, -15, -10};
    const FVector pa = Normalize(Reject({0.1,-0.4,-1},Normalize(target-u.translation)));
    const FVector pb = yaw.Rotate(pa);
    const TwoBoneResult solved = SolveTwoBone(u.translation, target, 30, 28, pa);
    const TwoBoneResult turned{yaw.Rotate(solved.mid), yaw.Rotate(solved.end), solved.reached};
    const Rotations ra = Orient(a, u.translation, solved, pa), rb = Orient(b, yaw.Rotate(u.translation), turned, pb);
    EXPECT(SameRotation(rb.upper, yaw * ra.upper));
    EXPECT(SameRotation(rb.forearm, yaw * ra.forearm));
}

static void AnimationPoseInvariance() {
    // The pose has an extra roll parent between lowerarm and hand. Apply
    // the integration's absolute-bone writes to two conflicting animations.
    skel::Pose ref;
    EXPECT(ref.Init({-1, 0, 1, 2, 3}));
    const Xf locals[] = {At({0, 0, 100}), At({0, -18, 30}, ToQuat({0, -90, 20})),
                        At({30, 0, 0}, ToQuat({0, 0, 10})), At({0, 0, 0}, ToQuat({0, 0, 15})), At({28, 0, 0})};
    ref.SetAllLocal(locals, 5);
    const Reference r = MeasureReference(ref.C(1), ref.C(2), ref.C(4), {1, 0, 0});
    const FVector shoulder = ref.C(1).translation;
    const FVector pole{0, -0.4, -1};
    const TwoBoneResult solved = SolveTwoBone(shoulder, shoulder + FVector{35, -10, -15}, r.upper_length, r.forearm_length, pole);
    const Rotations mapped = Orient(r, shoulder, solved, pole);
    const FQuat target_hand = ToQuat({20, 30, 40});
    Xf result[2][3];
    for (int run = 0; run < 2; ++run) {
        skel::Pose p;
        p.Init({-1, 0, 1, 2, 3});
        Xf animated[5];
        for (int i = 0; i < 5; ++i) animated[i] = locals[i];
        animated[1].rotation = ToQuat({run * 43.0, run * 110.0, run * -75.0});
        animated[2].rotation = ToQuat({run * 14.0, run * -67.0, run * 33.0});
        p.SetAllLocal(animated, 5);
        p.SetRotationC(1, mapped.upper);
        p.SetTranslationC(2, solved.mid);
        p.SetRotationC(2, mapped.forearm);
        p.SetTranslationC(4, solved.end);
        p.SetRotationC(4, target_hand);
        result[run][0] = p.C(1); result[run][1] = p.C(2); result[run][2] = p.C(4);
    }
    for (int i = 0; i < 3; ++i) {
        EXPECT(Near(result[0][i].translation, result[1][i].translation));
        EXPECT(SameRotation(result[0][i].rotation, result[1][i].rotation));
    }
}

static void WristAlignedElbowOutsideOldWindow() {
    for (const double sign:{-1.0,1.0}) {
        const FQuat world=ToQuat({12,sign*37,23});
        const Xf u=Turn(At({},ToQuat({10,20,30})),world);
        const Xf f=Turn(At({31,0,0},ToQuat({-40,25,15})),world);
        const Xf h=Turn(At({59,0,0},ToQuat({15,70,-20})),world);
        const Reference ref=MeasureReference(u,f,h,world.Rotate({0,0,-1}));
        const FVector wrist=u.translation+world.Rotate({20,0,0});
        const FVector preferred=world.Rotate({0,0,-1});
        const FVector desired=world.Rotate({0,0,1});
        const auto natural=SolveTwoBone(u.translation,wrist,31,28,desired);
        const FVector wanted=Normalize(natural.end-natural.mid);
        const FQuat hand=Normalized(Arc(Normalize(h.translation-f.translation),wanted)*h.rotation);
        const auto hint=WristAlignedPole(ref,u.translation,wrist,hand);
        EXPECT(hint.valid && hint.strength>.5 && Near(hint.pole,desired));
        const auto solved=SolveTwoBone(u.translation,wrist,31,28,hint.pole);
        EXPECT(Near(solved.mid,natural.mid) && Near(Normalize(solved.end-solved.mid),wanted));
        double old_best=kPi;
        for(int step=-6;step<=6;++step) {
            const FVector pole=AxisAngle(world.Rotate({1,0,0}),step*10*kDegToRad).Rotate(preferred);
            const auto old=SolveTwoBone(u.translation,wrist,31,28,pole);
            old_best=std::min(old_best,AngleBetween(old.end-old.mid,wanted));
        }
        EXPECT(old_best>100*kDegToRad); // actual straight wrist, old elbow-window cannot represent it
        EXPECT(Near(Length(solved.mid-u.translation),31) && Near(Length(solved.end-solved.mid),28));
        const FQuat opposite{-hand.X,-hand.Y,-hand.Z,-hand.W};
        EXPECT(Near(WristAlignedPole(ref,u.translation,wrist,opposite).pole,hint.pole));
        const FQuat parallel=Normalized(Arc(Normalize(h.translation-f.translation),world.Rotate({1,0,0}))*h.rotation);
        EXPECT(!WristAlignedPole(ref,u.translation,wrist,parallel).valid);
        EXPECT(!WristAlignedPole(ref,u.translation,u.translation,hand).valid);
    }
}

static double RotationDistance(double a,double b) {
    const FQuat qa=AxisAngle({1,0,0},a),qb=AxisAngle({1,0,0},b);
    const FQuat delta=Normalized(qa*qb.Inverse());
    return 2*std::acos(std::clamp(std::fabs(delta.W),0.0,1.0));
}
static bool BoundedRoll(const TwistResult& r) {
    return r.valid && std::isfinite(r.raw_radians) && std::fabs(r.raw_radians)<=kPi+1e-8 &&
        std::isfinite(r.target_radians) && std::fabs(r.target_radians)<=kTwistTargetLimit+1e-8 &&
        std::isfinite(r.forearm_radians) && std::fabs(r.forearm_radians)<=kPi+1e-8 &&
        std::isfinite(r.twist_radians) && std::fabs(r.twist_radians)<=kPi+1e-8;
}
static void BoundedDistributedRoll() {
    constexpr double dt=1.0/90;
    TwistState state;
    auto out=FollowTwist(179*kDegToRad,dt,.5,1,&state);
    EXPECT(BoundedRoll(out));
    EXPECT(Near(out.forearm_radians,.5*kTwistTargetLimit) && Near(out.twist_radians,kTwistTargetLimit));
    const auto begin=out;
    bool guarded=false;
    double full_motion=0;
    for(int n=0;n<180;++n) {
        const auto old=out;
        out=FollowTwist(-179*kDegToRad,dt,.5,1,&state);
        guarded|=out.seam_guarded;
        EXPECT(BoundedRoll(out));
        EXPECT(RotationDistance(old.forearm_radians,out.forearm_radians)<=kTwistBoneRate*dt+1e-7);
        EXPECT(RotationDistance(old.twist_radians,out.twist_radians)<=kTwistBoneRate*dt+1e-7);
        full_motion+=RotationDistance(old.twist_radians,out.twist_radians);
    }
    EXPECT(guarded);
    // The full-share bone crosses the small seam arc, never spins through zero.
    EXPECT(full_motion<11*kDegToRad);
    EXPECT(RotationDistance(begin.forearm_radians,out.forearm_radians)>170*kDegToRad);
    TwistState fresh;
    const auto same=FollowTwist(-179*kDegToRad,dt,.5,1,&fresh);
    EXPECT(RotationDistance(out.forearm_radians,same.forearm_radians)<1e-6);
    EXPECT(RotationDistance(out.twist_radians,same.twist_radians)<1e-6);

    // Exact live IK5 failure: after crossing the seam, holding -112 used to
    // retain +248; half share then differed by 180 degrees from a fresh pose.
    for(int n=0;n<180;++n) out=FollowTwist(-112*kDegToRad,dt,.5,1,&state);
    const auto reset=FollowTwist(-112*kDegToRad,dt,.5,1,nullptr);
    EXPECT(Near(out.forearm_radians,-56*kDegToRad));
    EXPECT(SameRotation(AxisAngle({1,0,0},out.forearm_radians),AxisAngle({1,0,0},reset.forearm_radians)));
    EXPECT(SameRotation(AxisAngle({1,0,0},out.twist_radians),AxisAngle({1,0,0},reset.twist_radians)));

    // Repeated turns never accumulate integer winding, and a held pose always
    // converges to the same two actual bone rotations, for fractional shares.
    for(double share:{0.0,.25,.5,.9,1.0}) {
        state={};
        out=FollowTwist(0,dt,share,1-share,&state);
        for(int n=1;n<=1800;++n) {
            const auto old=out;
            out=FollowTwist(n*3*kDegToRad,dt,share,1-share,&state);
            EXPECT(BoundedRoll(out));
            EXPECT(RotationDistance(old.forearm_radians,out.forearm_radians)<=kTwistBoneRate*dt+1e-7);
            EXPECT(RotationDistance(old.twist_radians,out.twist_radians)<=kTwistBoneRate*dt+1e-7);
        }
        for(int n=0;n<180;++n) out=FollowTwist(-112*kDegToRad,dt,share,1-share,&state);
        const auto direct=FollowTwist(-112*kDegToRad,dt,share,1-share,nullptr);
        EXPECT(RotationDistance(out.forearm_radians,direct.forearm_radians)<1e-6);
        EXPECT(RotationDistance(out.twist_radians,direct.twist_radians)<1e-6);
    }
    // Noisy signs must not reset a settle timer forever or preserve whichever
    // arbitrary branch happened to arrive first. Both histories select one
    // deterministic branch and remain rate-limited throughout.
    TwistState positive,negative;
    auto a=FollowTwist(179*kDegToRad,dt,.5,1,&positive);
    auto b=FollowTwist(-179*kDegToRad,dt,.5,1,&negative);
    for(int n=0;n<360;++n) {
        const auto old_a=a,old_b=b;
        const double noisy=(n%2?179:-179)*kDegToRad;
        a=FollowTwist(noisy,dt,.5,1,&positive);
        b=FollowTwist(noisy,dt,.5,1,&negative);
        EXPECT(RotationDistance(old_a.forearm_radians,a.forearm_radians)<=kTwistBoneRate*dt+1e-7);
        EXPECT(RotationDistance(old_b.forearm_radians,b.forearm_radians)<=kTwistBoneRate*dt+1e-7);
    }
    EXPECT(RotationDistance(a.forearm_radians,b.forearm_radians)<1e-6);
    EXPECT(RotationDistance(a.twist_radians,b.twist_radians)<1e-6);
    for(int n=0;n<180;++n) b=FollowTwist(-179*kDegToRad,dt,.5,1,&negative);
    EXPECT(RotationDistance(b.forearm_radians,same.forearm_radians)<1e-6);

    const Xf u=At({}),f=At({31,0,0}),h=At({59,0,0});
    const Reference ref=MeasureReference(u,f,h,{0,0,-1});
    const FQuat hand=AxisAngle({1,0,0},-112*kDegToRad),opposite{-hand.X,-hand.Y,-hand.Z,-hand.W};
    const auto sign_a=FollowTwist(WristTwist(ref,{},hand),dt,.5,1,nullptr);
    const auto sign_b=FollowTwist(WristTwist(ref,{},opposite),dt,.5,1,nullptr);
    EXPECT(SameRotation(AxisAngle({1,0,0},sign_a.forearm_radians),AxisAngle({1,0,0},sign_b.forearm_radians)));
    EXPECT(SameRotation(AxisAngle({1,0,0},sign_a.twist_radians),AxisAngle({1,0,0},sign_b.twist_radians)));

    const double nan=std::numeric_limits<double>::quiet_NaN();
    EXPECT(!FollowTwist(nan,dt,.5,1,&state).valid && !state.valid);
    EXPECT(!FollowTwist(0,dt,nan,1,&state).valid && !state.valid);
    EXPECT(!FollowTwist(0,dt,.5,nan,&state).valid && !state.valid);
    state.valid=true; state.forearm_radians=248*kDegToRad;
    out=FollowTwist(-112*kDegToRad,dt,.5,1,&state);
    EXPECT(Near(out.forearm_radians,-56*kDegToRad)); // corrupt/legacy winding cannot survive
    state.twist_radians=nan;
    EXPECT(BoundedRoll(FollowTwist(.3,dt,.5,1,&state)));
    state.seam_seconds=nan;
    EXPECT(BoundedRoll(FollowTwist(.3,dt,.5,1,&state)));
    for(double bad_dt:{0.0,-1.0,1.0,nan}) {
        out=FollowTwist(-30*kDegToRad,bad_dt,.5,1,&state);
        EXPECT(BoundedRoll(out) && Near(out.forearm_radians,-15*kDegToRad));
    }
    out=FollowTwist(.4,dt,-3,8,nullptr);
    EXPECT(Near(out.forearm_radians,0) && Near(out.twist_radians,.4));
}

static void CalibratedTwistBranch() {
    const Xf start=At({31,0,0}),end=At({58,0,0});
    const Xf skin=At({52.6,2,0},AxisAngle({1,0,0},.2));
    const Attachment measured=MeasureAttachment(start,end,skin);
    EXPECT(measured.valid && Near(measured.along,.8));
    for(const double length:{22.0,37.0}) for(const double sign:{-1.0,1.0}) {
        const FQuat frame=ToQuat({17,sign*53,-21});
        Xf unrolled=At({10,-20,100},frame);
        const FVector axis=frame.Rotate({1,0,0}),target=unrolled.translation+axis*length;
        Xf total_roll=unrolled; total_roll.rotation=AxisAngle(axis,sign*1.7)*frame;
        Xf placed;
        EXPECT(PlaceAttachment(measured,total_roll,target,&placed));
        EXPECT(Near(Dot(placed.translation-unrolled.translation,axis),.8*length));
        EXPECT(Near(placed.translation,unrolled.translation+axis*(.8*length)+total_roll.rotation.Rotate({0,2,0})));
        EXPECT(SameRotation(placed.rotation,total_roll.rotation*skin.rotation));

        // Hand precedes twist in the skeleton, matching HOLE's bone ordering:
        // twist is a skin branch, so moving only the hand cannot retarget it.
        skel::Pose pose; EXPECT(pose.Init({-1,0,1,1}));
        const Xf local[]={At({}),start,skel::Relative(end,start),skel::Relative(skin,start)};
        pose.SetAllLocal(local,4);
        Xf partial=unrolled; partial.rotation=AxisAngle(axis,sign*.85)*frame;
        pose.SetC(1,partial);
        const FVector old_skin=pose.C(3).translation;
        Xf hand=pose.C(2); hand.translation=target; hand.rotation=frame; pose.SetC(2,hand);
        EXPECT(Near(pose.C(3).translation,old_skin));
        EXPECT(!Near(Dot(old_skin-unrolled.translation,axis),.8*length));
        pose.SetC(3,placed);
        EXPECT(Near(pose.C(2).translation,target) && SameRotation(pose.C(2).rotation,frame));
        EXPECT(SameRotation(pose.C(3).rotation,total_roll.rotation*skin.rotation)); // total, not twice-inherited roll
    }
    Xf scaled=At({10,20,30},ToQuat({20,40,60})); scaled.scale={1.4,1.4,1.4};
    Xf out;
    const FVector endpoint=scaled.translation+scaled.rotation.Rotate({22*1.4,0,0});
    EXPECT(PlaceAttachment(measured,scaled,endpoint,&out));
    EXPECT(Near(out.translation,scaled.translation+scaled.rotation.Rotate({22*1.4*.8,2*1.4,0})));
    EXPECT(Near(out.scale,FVector{1.4,1.4,1.4}));
    EXPECT(!MeasureAttachment(start,end,At({80,0,0})).valid);
    EXPECT(!MeasureAttachment(start,start,skin).valid);
    const Xf untouched=At({1,2,3}); out=untouched;
    EXPECT(!PlaceAttachment({},scaled,endpoint,&out) && Near(out.translation,untouched.translation));
}

static void TinyBendDoesNotFlipBoneFrame() {
    for(const FQuat frame:{FQuat{},ToQuat({23,-71,35})}) {
        const FVector shoulder{100000,200000,300000};
        const auto world=[&](FVector p){ return shoulder+frame.Rotate(p); };
        const Reference ref=MeasureReference(At(world({0,0,0}),frame),At(world({30,0,0}),frame),
            At(world({58,0,0}),frame),frame.Rotate({0,0,-1}));
        for(double noise:{.0001,.001,.005}) {
            Rotations result[2];
            for(int side=0;side<2;++side) {
                const TwoBoneResult tiny{world({30,side?noise:-noise,0}),world({58,0,0}),true};
                result[side]=Orient(ref,shoulder,tiny,frame.Rotate({0,0,-1}));
                EXPECT(result[side].valid);
                EXPECT(Near(result[side].upper.Rotate(ref.upper_axis),Normalize(tiny.mid-shoulder)));
                EXPECT(Near(result[side].forearm.Rotate(ref.forearm_axis),Normalize(tiny.end-tiny.mid)));
                EXPECT(Dot(result[side].forearm.Rotate(ref.forearm_normal),frame.Rotate({0,-1,0}))>.999999);
            }
            const FQuat difference=Normalized(result[0].forearm.Inverse()*result[1].forearm);
            EXPECT(2*std::acos(std::clamp(std::fabs(difference.W),0.0,1.0))<.05*kDegToRad);
        }
    }
}

static FVector InversePoint(const Xf& frame, const FVector& point) {
    const FVector v = frame.rotation.Inverse().Rotate(point-frame.translation);
    return {v.X/frame.scale.X,v.Y/frame.scale.Y,v.Z/frame.scale.Z};
}

static void CalibratedSkinLength() {
    // WhiteMan's native forearm is 26.975 cm on both sides. The live profile
    // kept L=26.937, R=22.991. Its right wrist crease is 89.3% lowerarm skin:
    // merely translating the hand leaves that influence 3.56 cm too distal.
    constexpr double native_lower = 26.97525577172432;
    constexpr double lower_weight = .8930555979410807;
    const double lower_lengths[] = {26.937004911886032,22.990692710225463};
    for (double scale : {.8,1.,1.25}) for (int side = 0; side < 2; ++side)
        for (double flex : {0.,80.,-80.}) for (bool nonuniform : {false,true}) {
        const double sign = side ? -1. : 1.;
        const FQuat frame = ToQuat({21,-63,14});
        Xf source[5];
        source[0] = At({3,7,130},frame);
        source[1] = At(source[0].translation+frame.Rotate({sign*30.34,0,0}),
                       frame*AxisAngle({1,0,0},.4));
        source[3] = At(source[1].translation+source[1].rotation.Rotate({sign*native_lower,0,0}),
                       source[1].rotation*ToQuat({12,-8,41}));
        source[2] = At(source[1].translation+(source[3].translation-source[1].translation)*.519,
                       source[1].rotation*ToQuat({0,0,23}));
        source[4] = skel::Compose(At({4,1,2},ToQuat({20,5,7})),source[3]);
        const Reference ref = MeasureReference(source[0],source[1],source[3],{0,0,-1});
        const Attachment skin = MeasureAttachment(source[1],source[3],source[2]);
        const std::vector<int> parents = {-1,0,1,1,3};
        Xf local[5];
        for (int i=0;i<5;++i) local[i]=parents[i]<0?source[i]:skel::Relative(source[i],source[parents[i]]);
        skel::Pose pose; EXPECT(pose.Init(parents));
        Xf world; world.translation={200,-100,50};world.rotation=YawOnly(71);
        world.scale={scale,scale*(nonuniform?1.2:1.),scale*(nonuniform?.9:1.)};
        const auto to_world=[&](int bone){return skel::Compose(pose.C(bone),world);};
        const auto set_world=[&](int bone,const Xf& value){pose.SetC(bone,skel::Relative(value,world));};
        const FVector wrist_bind=source[3].translation;
        const FVector lower_bind=InversePoint(source[1],wrist_bind),hand_bind=InversePoint(source[3],wrist_bind);
        FVector last_wrist,last_skin;
        for (int tick=0;tick<2;++tick) {
            pose.SetAllLocal(local,5); // the production animation read resets prior skin scale
            Xf upper=to_world(0),lower=to_world(1),hand=to_world(3);
            const auto upper_scale=upper.scale,lower_scale=lower.scale,hand_scale=hand.scale;
            const FVector shoulder=upper.translation;
            const double upper_cm=(side?33.625352328533552:31.532127224694452)*scale;
            const double lower_cm=lower_lengths[side]*scale;
            const FVector elbow=shoulder+upper.rotation.Rotate(ref.upper_axis)*upper_cm;
            lower.translation=elbow;
            const FVector wrist=elbow+lower.rotation.Rotate(ref.forearm_axis)*lower_cm;
            const FQuat bend=AxisAngle(lower.rotation.Rotate({0,1,0}),flex*kDegToRad);
            hand.translation=wrist;hand.rotation=Normalized(bend*hand.rotation);
            // Control pose: the same authoritative hand transform without
            // changing either arm's scale. Finger size/position must match
            // this under the engine's existing component/world composition.
            skel::Pose hand_baseline=pose;
            hand_baseline.SetC(3,skel::Relative(hand,world));
            const Xf expected_finger=skel::Compose(hand_baseline.C(4),world);
            const FVector old_skin=skel::Apply(lower,lower_bind)*lower_weight+
                                   skel::Apply(hand,hand_bind)*(1-lower_weight);
            if(side && scale==1) EXPECT(Length(old_skin-wrist)>3.55);
            EXPECT(MatchSegmentScale(ref.upper_axis,ref.upper_scale,ref.upper_length,upper_cm,upper_scale,&upper.scale));
            set_world(0,upper);
            EXPECT(MatchSegmentScale(ref.forearm_axis,ref.forearm_scale,ref.forearm_length,lower_cm,lower_scale,&lower.scale));
            set_world(1,lower);
            Xf twist; EXPECT(PlaceAttachment(skin,lower,wrist,&twist));set_world(2,twist);
            hand.scale=hand_scale;set_world(3,hand);
            const FVector skinned=skel::Apply(to_world(1),lower_bind)*lower_weight+
                                   skel::Apply(to_world(3),hand_bind)*(1-lower_weight);
            EXPECT(Near(skinned,wrist)); // both skin influences meet at the actual wrist pivot
            EXPECT(Near(skel::Apply(upper,InversePoint(source[0],source[1].translation)),elbow));
            EXPECT(Near(to_world(3).translation,wrist));
            EXPECT(Near(to_world(1).scale.Y,lower_scale.Y)&&Near(to_world(1).scale.Z,lower_scale.Z));
            EXPECT(Near(to_world(3).scale,hand_scale));
            EXPECT(Near(to_world(4).translation,expected_finger.translation));
            EXPECT(Near(to_world(4).scale,expected_finger.scale));
            EXPECT(SameRotation(to_world(4).rotation,expected_finger.rotation));
            EXPECT(Near(to_world(2).translation,elbow+(wrist-elbow)*.519));
            if(tick) { EXPECT(Near(last_wrist,wrist));EXPECT(Near(last_skin,skinned)); }
            last_wrist=wrist;last_skin=skinned;
        }
    }
    FVector unchanged{2,3,4};
    EXPECT(!MatchSegmentScale({1,1,0},{1,1,1},27,23,{1,1,1},&unchanged));
    EXPECT(Near(unchanged,{2,3,4}));
    EXPECT(!MatchSegmentScale({1,0,0},{1,1,1},0,23,{1,1,1},&unchanged));
    EXPECT(!MatchSegmentScale({1,0,0},{1,1,1},27,23,{0,1,1},&unchanged));
    EXPECT(MatchSegmentScale({0,-1,0},{1.1,1.3,.9},26,22,{2.2,2.6,1.8},&unchanged));
    EXPECT(Near(unchanged,{2.2,1.1,1.8}));
    EXPECT(MatchSegmentScale({0,0,1},{1.1,1.3,.9},18,22,{2.2,2.6,1.8},&unchanged));
    EXPECT(Near(unchanged,{2.2,2.6,1.1}));
}

static void CenteredTwistSeam() {
    // The seam sits opposite the center (owner 2026-11-07 skin twist: the
    // seam goes where the palm faces outward, opposite a thumb-up forearm).
    // A sweep through the old seam at +/-180 is continuous; shares still
    // multiply the whole roll from the reference.
    const double dt=1.0/90;
    const double center=-90*kDegToRad;
    TwistState state;
    TwistResult out=FollowTwist(center,dt,0,.5,&state,center,.5);
    EXPECT(out.valid && Near(out.twist_radians,center*.5) && Near(out.forearm_radians,0) &&
           Near(out.watch_radians,center*.5));
    double previous=out.twist_radians;
    for(int n=0;n<=160;++n) {
        // From the center to 160 deg past it: through raw -180/+180.
        const double raw=std::remainder(center-n*kDegToRad,2*kPi);
        out=FollowTwist(raw,dt,0,.5,&state,center,.5);
        EXPECT(out.valid && !out.seam_guarded);
        EXPECT(std::fabs(std::remainder(out.twist_radians-previous,2*kPi))<=kTwistBoneRate*dt+1e-9);
        EXPECT(std::fabs(out.twist_radians)<=kPi+1e-9 && std::fabs(out.watch_radians)<=kPi+1e-9);
        previous=out.twist_radians;
    }
    // Settled 160 deg past the center: half of (center - 160 deg), not the
    // half of a principal angle on the other side.
    for(int n=0;n<200;++n) out=FollowTwist(std::remainder(center-160*kDegToRad,2*kPi),dt,0,.5,&state,center,.5);
    EXPECT(Near(std::remainder(out.twist_radians-(center-160*kDegToRad)*.5,2*kPi),0,1e-6));
    EXPECT(Near(out.forearm_radians,0));
    // The default center keeps the old behavior exactly.
    TwistState a,b;
    for(double raw:{-179.0,-120.0,30.0,179.0}) {
        const auto old=FollowTwist(raw*kDegToRad,dt,.5,1,&a);
        const auto centered=FollowTwist(raw*kDegToRad,dt,.5,1,&b,0,-1);
        EXPECT(Near(old.forearm_radians,centered.forearm_radians) && Near(old.twist_radians,centered.twist_radians) &&
               Near(centered.watch_radians,centered.forearm_radians));
    }
    EXPECT(!FollowTwist(0,dt,.5,1,&state,std::numeric_limits<double>::quiet_NaN()).valid && !state.valid);
}

int main() {
    CalibratedSkinLength();
    CenteredTwistSeam();
    MirroredAndArbitraryBoneFrames();
    InterveningTwistParent();
    ReferenceRoundTripAndExtension();
    DegenerateAndStraightTargets();
    YawCovariance();
    AnimationPoseInvariance();
    WristAlignedElbowOutsideOldWindow();
    BoundedDistributedRoll();
    CalibratedTwistBranch();
    TinyBendDoesNotFlipBoneFrame();
    std::printf("arm pose: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
