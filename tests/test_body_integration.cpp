// Pure integration: persisted physical fit -> runtime target mapping ->
// measured bone frames -> IK -> actual component/local skeleton writes.
// Link body_calibration.cpp, arm_pose.cpp, arm_posture.cpp, ik_math.cpp and skeleton.cpp.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

#include "vr/body/body_calibration.h"
#include "vr/body/arm_pose.h"
#include "vr/body/arm_posture.h"

using namespace holevr::vr;
using namespace holevr::vr::ik;
namespace calibration = holevr::vr::body_calibration;
using holevr::vr::skel::Xf;
static int failures = 0;
#define EXPECT(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
static bool Near(double a, double b, double epsilon = 1e-7) { return std::fabs(a-b) < epsilon; }
static bool Near(FVector a, FVector b, double epsilon = 1e-7) { return Length(a-b) < epsilon; }
static bool SameRotation(FQuat a, FQuat b) {
    a=Normalized(a); b=Normalized(b);
    return std::fabs(a.X*b.X+a.Y*b.Y+a.Z*b.Z+a.W*b.W) > 1-1e-9;
}
static bool GoodRotation(FQuat q) {
    return std::isfinite(q.X) && std::isfinite(q.Y) && std::isfinite(q.Z) && std::isfinite(q.W) &&
        Near(q.X*q.X+q.Y*q.Y+q.Z*q.Z+q.W*q.W, 1);
}
static Xf At(FVector p, FQuat q = {}) { Xf t; t.translation=p; t.rotation=q; return t; }

// A versioned profile as read from disk, deliberately asymmetric and with
// lengths different from the avatar fixture below.
static const char* saved_profile =
    "version=1\nunits=cm\nmethod=frontal_arc_bent_t\nstanding_eye_cm=173.5\n"
    "left_shoulder=-5 -20 -25\nleft_upper=31\nleft_forearm=28\n"
    "left_radial_rms=0.4\nleft_plane_rms=0.5\nleft_arc_degrees=87\nleft_bent_error=0.5\n"
    "left_inliers=90\nleft_samples=95\n"
    "right_shoulder=-4 21 -24\nright_upper=35\nright_forearm=30\n"
    "right_radial_rms=0.3\nright_plane_rms=0.6\nright_arc_degrees=89\nright_bent_error=0.7\n"
    "right_inliers=92\nright_samples=96\n";
static const FVector anatomical_shoulder[2]={{-5,-20,-25},{-4,21,-24}};
static const double anatomical_upper[2]={31,35}, anatomical_forearm[2]={28,30};

struct Avatar {
    // root, then clavicle/upper/forearm/twist/hand for each side. The twist
    // is an intervening hand parent, rather than only an isolated skin branch.
    const std::vector<int> parent={-1,0,1,2,3,4,0,6,7,8,9};
    Xf local[11];
    arm_pose::Reference reference[2];
    arm_pose::Attachment twist_reference[2];
    Avatar() {
        Xf component[11];
        component[0]=At({0,0,90});
        for (int side=0; side<2; ++side) {
            const double sign=side==0 ? -1.0 : 1.0;
            const int c=1+5*side, u=c+1, f=u+1, twist=u+2, h=u+3;
            component[c]=At({-3,sign*9,132},ToQuat({15,sign*24,-17}));
            component[u]=At({0,sign*18,135}, ToQuat({12,sign*78,31}));
            component[f]=At({0,sign*42,132}, ToQuat({-17,sign*69,22}));
            component[twist]=At({0,sign*47,131.8}, ToQuat({-17,sign*69,62}));
            component[h]=At({0,sign*71,131}, ToQuat({29,sign*54,13}));
        }
        for (int i=0; i<11; ++i) local[i]=parent[i]<0 ? component[i] : skel::Relative(component[i],component[parent[i]]);
        skel::Pose rest;
        EXPECT(rest.Init(parent)); rest.SetAllLocal(local,11);
        for (int side=0; side<2; ++side) {
            const int u=2+5*side;
            reference[side]=arm_pose::MeasureReference(rest.C(u),rest.C(u+1),rest.C(u+3),{0,0,-1});
            twist_reference[side]=arm_pose::MeasureAttachment(rest.C(u+1),rest.C(u+3),rest.C(u+2));
            EXPECT(reference[side].valid);
            EXPECT(twist_reference[side].valid);
            EXPECT(!Near(reference[side].upper_length,anatomical_upper[side]));
        }
    }
    void Animated(skel::Pose& pose, int variant) const {
        EXPECT(pose.Init(parent));
        Xf animation[11];
        for (int i=0;i<11;++i) {
            animation[i]=local[i];
            // Root, shoulders, elbows and the intervening twist all carry
            // unrelated animation rotations. The final target must win.
            animation[i].rotation=ToQuat({double(variant*19+i*5),double(variant*41-i*11),double(i*17-variant*9)});
        }
        pose.SetAllLocal(animation,11);
    }
};

static Xf World(const skel::Pose& pose, int bone, const Xf& mesh) { return skel::Compose(pose.C(bone),mesh); }
static void SetWorld(skel::Pose& pose, int bone, const Xf& mesh, const Xf& world) {
    pose.SetC(bone,skel::Relative(world,mesh));
}

static void ProfileThroughPose() {
    calibration::Profile profile;
    EXPECT(calibration::Parse(saved_profile,&profile));
    EXPECT(calibration::Validate(profile));
    EXPECT(profile.standing_eye_cm==0);
    EXPECT(profile.seated_eye_cm==0);
    EXPECT(profile.legacy_eye_cm==173.5);
    const std::string persisted=calibration::Serialize(profile);
    // A rejected newer save must leave the usable previous fit intact.
    EXPECT(!calibration::Parse(persisted+"left_upper=999\n",&profile));
    EXPECT(calibration::Serialize(profile)==persisted);

    Avatar avatar;
    for (double tracking_scale : {.8,1.,1.25}) for (double mesh_scale : {.65,1.,1.6})
        for (int variant=0;variant<2;++variant) {
        const FQuat actor=YawOnly(variant==0?37:-81);
        const FQuat body=Normalized(actor*YawOnly(variant==0?-21:49));
        // The avatar's component is rotated relative to both actor and body.
        Xf mesh=At({200,-300,25},Normalized(actor*ToQuat({11,-62,13})));
        mesh.scale={mesh_scale,mesh_scale,mesh_scale};
        const FVector head={315,-190,173.5*tracking_scale};
        skel::Pose pose; avatar.Animated(pose,variant);
        for (int side=0;side<2;++side) {
            const int u=2+5*side, f=u+1, twist=u+2, h=u+3;
            FVector shoulder;
            double upper=0,forearm=0;
            EXPECT(calibration::MapArmTarget(profile,side,head,body,tracking_scale,&shoulder,&upper,&forearm));

            // Construct a physically possible target independently from the
            // persisted fit: fixed known shoulder, two known segment vectors.
            const double sign=side==0?-1.:1.;
            const FVector upper_direction=Normalize({.4,sign*.7,-.3});
            const FVector forearm_direction=Normalize({.8,sign*.2,.2});
            const FVector elbow_local=anatomical_shoulder[side]+upper_direction*anatomical_upper[side];
            const FVector wrist_local=elbow_local+forearm_direction*anatomical_forearm[side];
            const FVector expected_shoulder=head+body.Rotate(anatomical_shoulder[side]*tracking_scale);
            const FVector expected_elbow=head+body.Rotate(elbow_local*tracking_scale);
            const FVector expected_target=head+body.Rotate(wrist_local*tracking_scale);
            const FVector raw_head{30,-40,173.5};
            const FQuat controller=ToQuat({23,sign*58+variant*27,-36});
            const FVector controller_to_wrist{6,-3,2};
            const FVector raw_controller=raw_head+wrist_local-controller.Rotate(controller_to_wrist);
            FVector raw_wrist;
            EXPECT(calibration::MapWristTarget(raw_controller,controller.Rotate(controller_to_wrist*tracking_scale),
                                               tracking_scale,&raw_wrist));
            const FVector target=head+body.Rotate((raw_wrist-raw_head)*tracking_scale);
            const FVector target_axis=Normalize(target-expected_shoulder);
            const FVector wanted_pole=Reject(expected_elbow-expected_shoulder,target_axis);
            const FVector pole=Normalize(wanted_pole);
            const auto solved=SolveTwoBone(shoulder,target,upper,forearm,pole);
            const auto mapped=arm_pose::Orient(avatar.reference[side],shoulder,solved,pole);
            EXPECT(mapped.valid && solved.reached);
            const FQuat target_hand=Normalized(body*controller);

            Xf bone=World(pose,u,mesh); bone.translation=shoulder; bone.rotation=mapped.upper; SetWorld(pose,u,mesh,bone);
            bone=World(pose,f,mesh); bone.translation=solved.mid; bone.rotation=mapped.forearm; SetWorld(pose,f,mesh,bone);
            // Exercise the same roll distribution topology as the runtime.
            // Roll must not change segment endpoints, even with a twist parent.
            const FVector axis=Normalize(solved.end-solved.mid);
            arm_pose::TwistState roll_state;
            const auto roll=arm_pose::FollowTwist(arm_pose::WristTwist(avatar.reference[side],mapped.forearm,target_hand),
                                                  1./90,.5,.2,&roll_state);
            EXPECT(roll.valid);
            Xf unrolled=World(pose,f,mesh);
            bone=unrolled; bone.rotation=Normalized(AxisAngle(axis,roll.forearm_radians)*unrolled.rotation);
            SetWorld(pose,f,mesh,bone);
            Xf twist_frame=unrolled;
            twist_frame.rotation=Normalized(AxisAngle(axis,roll.twist_radians)*unrolled.rotation);
            EXPECT(arm_pose::PlaceAttachment(avatar.twist_reference[side],twist_frame,solved.end,&bone));
            SetWorld(pose,twist,mesh,bone);
            bone=World(pose,h,mesh); bone.translation=solved.end; bone.rotation=target_hand; SetWorld(pose,h,mesh,bone);

            const Xf actual_upper=World(pose,u,mesh), actual_forearm=World(pose,f,mesh), actual_hand=World(pose,h,mesh);
            EXPECT(Near(actual_upper.translation,expected_shoulder));
            EXPECT(Near(actual_forearm.translation,expected_elbow));
            EXPECT(Near(actual_hand.translation,expected_target));
            EXPECT(Near(Length(actual_forearm.translation-actual_upper.translation)/tracking_scale,anatomical_upper[side]));
            EXPECT(Near(Length(actual_hand.translation-actual_forearm.translation)/tracking_scale,anatomical_forearm[side]));
            EXPECT(SameRotation(actual_hand.rotation,target_hand));
            EXPECT(Near(Dot(World(pose,twist,mesh).translation-solved.mid,axis)/forearm,
                        avatar.twist_reference[side].along));
            EXPECT(GoodRotation(actual_upper.rotation) && GoodRotation(actual_forearm.rotation) && GoodRotation(actual_hand.rotation));
            EXPECT(Near(actual_upper.rotation.Rotate(avatar.reference[side].upper_axis),Normalize(expected_elbow-expected_shoulder)));
            EXPECT(Near(actual_forearm.rotation.Rotate(avatar.reference[side].forearm_axis),Normalize(target-expected_elbow)));
            EXPECT(pose.Changed(u) && pose.Changed(f) && pose.Changed(h));
            // The engine receives local transforms, not this cache. Rebuild
            // from those locals and verify the same final world-space goal.
            skel::Pose rebuilt; EXPECT(rebuilt.Init(avatar.parent));
            rebuilt.SetAllLocal(pose.Local().data(),pose.Count());
            EXPECT(Near(World(rebuilt,h,mesh).translation,expected_target));
            EXPECT(SameRotation(World(rebuilt,h,mesh).rotation,target_hand));
        }
    }
    EXPECT(calibration::Serialize(profile)==persisted);
}

static void RotatedControllerOffset() {
    const FVector raw={120,-40,150}, local_offset={6,-3,2};
    const FQuat world_yaw=YawOnly(71);
    const FVector moved_origin={400,-170,30};
    for (double scale : {.8,1.,1.25}) {
        FVector wrists[2];
        for (int turn=0;turn<2;++turn) {
            const FQuat controller=YawOnly(turn*90.);
            // FreeHand has already rotated its game-space wrist displacement.
            const FVector game_delta=controller.Rotate(local_offset*scale);
            EXPECT(calibration::MapWristTarget(raw,game_delta,scale,&wrists[turn]));
            const FVector displacement=wrists[turn]-raw;
            EXPECT(Near(Length(displacement),7));
            EXPECT(Near(controller.Inverse().Rotate(displacement),local_offset));
            FVector moved;
            EXPECT(calibration::MapWristTarget(moved_origin+world_yaw.Rotate(raw),world_yaw.Rotate(game_delta),scale,&moved));
            EXPECT(Near(moved,moved_origin+world_yaw.Rotate(wrists[turn])));
            EXPECT(Near(raw,FVector{120,-40,150}));
        }
        // Equal-length offsets with different orientations are different wrist
        // targets; subtracting a scalar fist length would collapse this case.
        EXPECT(Length(wrists[0]-wrists[1])>9);
    }
}

static void PostureKeepsCalibration() {
    calibration::Profile profile; EXPECT(calibration::Parse(saved_profile,&profile));
    const std::string saved=calibration::Serialize(profile);
    Avatar avatar;
    for (double scale : {.8,1.,1.25}) for (int variant=0;variant<2;++variant) {
        const FQuat body=YawOnly(variant?117:-42);
        const FVector head{230,-170,173.5*scale};
        Xf mesh=At({180,-215,31},ToQuat({13,variant?-81.:36.,-17}));
        mesh.scale={variant?.7:1.4,variant?.7:1.4,variant?.7:1.4};
        for (int side=0;side<2;++side) {
            const int u=2+5*side, f=u+1, twist=u+2, h=u+3;
            const double sign=side?1.:-1.;
            FVector shoulder; double upper=0,lower=0;
            EXPECT(calibration::MapArmTarget(profile,side,head,body,scale,&shoulder,&upper,&lower));
            const double reach=upper+lower;
            const FVector offsets[]={{reach*.65,0,-reach*.1},
                {reach*.35,-sign*reach*.60,reach*.1}, // crossed hand can need a medial elbow
                {reach*.08,sign*reach*.08,reach*.82}, // overhead
                {std::fabs(upper-lower)+1,0,0},      // nearly folded, still reachable
                {reach*.99,0,0}};                    // near extension
            arm_posture::State continuity;
            arm_pose::TwistState roll_state;
            for (int position=0;position<5;++position) {
                skel::Pose pose; avatar.Animated(pose,variant+position);
                const FVector wrist=shoulder+body.Rotate(offsets[position]);
                const FQuat hand=Normalized(body*ToQuat({position*12.,sign*37,position%2?179.:-179.}));
                arm_posture::Input in;
                in.reference=avatar.reference[side];
                in.shoulder=shoulder; in.wrist=wrist; in.hand_rotation=hand;
                in.upper_cm=upper; in.forearm_cm=lower;
                in.outward=body.Rotate({0,sign,0}); in.forward=body.Rotate({1,0,0}); in.down={0,0,-1};
                in.forearm_axis_hand=in.reference.forearm_to_hand.Inverse().Rotate(in.reference.forearm_axis);
                in.palm_axis_hand=in.reference.forearm_to_hand.Inverse().Rotate(in.reference.forearm_normal);
                in.torso_center=head; in.torso_center_valid=true;
                in.straight_wrist=position==0; in.dt=1./90;
                const auto result=arm_posture::Solve(in,&continuity);
                EXPECT(result.valid && result.solved.reached);
                if (!result.valid) continue;
                EXPECT(Near(result.solved.end,wrist));
                EXPECT(Near(Length(result.solved.mid-shoulder),upper));
                EXPECT(Near(Length(result.solved.end-result.solved.mid),lower));
                if (position==0) EXPECT(Dot(result.solved.mid-head,in.outward)>0);

                // Write the measured anchor through an arbitrarily animated
                // clavicle, then apply the actual posture and bounded skin
                // helpers. Absolute hand placement must win over parent roll.
                Xf upper_bone=World(pose,u,mesh);
                upper_bone.translation=shoulder; upper_bone.rotation=result.rotations.upper;
                SetWorld(pose,u,mesh,upper_bone);
                Xf forearm=World(pose,f,mesh);
                forearm.translation=result.solved.mid; forearm.rotation=result.rotations.forearm;
                SetWorld(pose,f,mesh,forearm);
                const FVector axis=Normalize(result.solved.end-result.solved.mid);
                const auto roll=arm_pose::FollowTwist(result.raw_roll,in.dt,.5,.2,&roll_state);
                EXPECT(roll.valid);
                Xf rolled=forearm;
                rolled.rotation=Normalized(AxisAngle(axis,roll.forearm_radians)*forearm.rotation);
                SetWorld(pose,f,mesh,rolled);
                Xf skin_frame=forearm,skin;
                skin_frame.rotation=Normalized(AxisAngle(axis,roll.twist_radians)*forearm.rotation);
                EXPECT(arm_pose::PlaceAttachment(avatar.twist_reference[side],skin_frame,result.solved.end,&skin));
                SetWorld(pose,twist,mesh,skin);
                Xf hand_bone=World(pose,h,mesh); hand_bone.translation=result.solved.end; hand_bone.rotation=hand;
                SetWorld(pose,h,mesh,hand_bone);
                skel::Pose rebuilt; EXPECT(rebuilt.Init(avatar.parent));
                rebuilt.SetAllLocal(pose.Local().data(),pose.Count());
                EXPECT(Near(World(rebuilt,u,mesh).translation,head+body.Rotate(anatomical_shoulder[side]*scale)));
                EXPECT(Near(World(rebuilt,h,mesh).translation,wrist));
                EXPECT(SameRotation(World(rebuilt,h,mesh).rotation,hand));
                EXPECT(Near(Length(World(rebuilt,f,mesh).translation-World(rebuilt,u,mesh).translation),upper));
                EXPECT(Near(Length(World(rebuilt,h,mesh).translation-World(rebuilt,f,mesh).translation),lower));
                EXPECT(Near(Dot(World(rebuilt,twist,mesh).translation-result.solved.mid,axis)/lower,
                            avatar.twist_reference[side].along));
            }
        }
    }
    EXPECT(calibration::Serialize(profile)==saved);
}

static void InvalidMappingIsTransactional() {
    calibration::Profile profile; EXPECT(calibration::Parse(saved_profile,&profile));
    FVector shoulder{1,2,3}; double upper=4,forearm=5;
    const double nan=std::numeric_limits<double>::quiet_NaN();
    for (double bad : {0.,-1.,1001.,nan,std::numeric_limits<double>::infinity()}) {
        EXPECT(!calibration::MapArmTarget(profile,0,{},FQuat{},bad,&shoulder,&upper,&forearm));
        EXPECT(Near(shoulder,FVector{1,2,3}) && upper==4 && forearm==5);
        EXPECT(!calibration::MapWristTarget({}, {}, bad, &shoulder));
        EXPECT(Near(shoulder,FVector{1,2,3}));
    }
    EXPECT(!calibration::MapArmTarget(profile,2,{},FQuat{},1,&shoulder,&upper,&forearm));
    EXPECT(!calibration::MapArmTarget(profile,0,{nan,0,0},FQuat{},1,&shoulder,&upper,&forearm));
    EXPECT(!calibration::MapArmTarget(profile,0,{},FQuat{0,0,0,0},1,&shoulder,&upper,&forearm));
    EXPECT(!calibration::MapWristTarget({nan,0,0},{},1,&shoulder));
    EXPECT(!calibration::MapWristTarget({}, {nan,0,0},1,&shoulder));
    EXPECT(Near(shoulder,FVector{1,2,3}) && upper==4 && forearm==5);
    profile.arm[0].upper_cm=999;
    EXPECT(!calibration::MapArmTarget(profile,0,{},FQuat{},1,&shoulder,&upper,&forearm));
}

int main() {
    ProfileThroughPose(); RotatedControllerOffset(); PostureKeepsCalibration(); InvalidMappingIsTransactional();
    std::printf("body integration: %s (%d failures)\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
