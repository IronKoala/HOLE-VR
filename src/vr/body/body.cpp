#include "vr/body/body.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "game/game.h"
#include "ue/call.h"
#include "ue/names.h"
#include "ue/reflection.h"
#include "vr/body/hand_pose.h"
#include "vr/body/ik_math.h"
#include "vr/hands.h"

namespace holevr::vr::body {
namespace {

using ue::FName;
using ue::UObject;
using namespace ik;

// FTransform (double precision), as the engine passes it to Blueprint calls.
struct alignas(16) Xf {
    FQuat rotation;
    FVector translation;
    double pad0 = 0;
    FVector scale{1, 1, 1};
    double pad1 = 0;
};
static_assert(sizeof(Xf) == 0x60, "FTransform layout");

constexpr uint8_t kWorldSpace = 0;      // EBoneSpaces::WorldSpace
constexpr uint8_t kComponentSpace = 1;  // EBoneSpaces::ComponentSpace
constexpr uint8_t kSnapToTarget = 2;    // EAttachmentRule::SnapToTarget

// ---- settings ----
struct Settings {
    bool enabled = true;
    double yaw_deadzone = 45.0;   // degrees the head may turn before the body follows
    double max_crouch = 70.0;     // cm the pelvis may drop
    double max_lean = 30.0;       // cm the head may lean away from the body before the pelvis follows
    double max_spine_bend = 45.0; // degrees
    double arm_stretch = 30.0;    // cm the hand may still follow past full arm reach
    double hand_offset[3] = {0, 0, 0};  // cm, in the controller's frame (fwd, right, up)
    double hand_rot[3] = {0, 0, 0};     // degrees pitch, yaw, roll applied to the hand
    double palm_depth = 3.0;
    bool hide_head = true;
    int shadow = 1;               // 0 none, 1 game's animated body (with head), 2 the VR body
    bool hand_poses = true;
    double finger_scale = 1.0;
    bool legs = true;
    double arm_scale = 0.0;       // 0 = learn from how far you reach; else fixed multiplier
    double max_arm_scale = 1.25;
    double neck_down = 14.0;      // cm from the eyes down to the base of the neck, in head space
    double neck_back = 8.0;       // cm from the eyes back to the base of the neck
    double grip_angle = 20.0;     // pistol grip slant, degrees
    double gun_hand[3] = {1.5, 0, 7};  // fist offset from MainHandPosition in the gun frame (barrel, right, up), cm
    double wrist_twist_share = 0.75;  // how much of the wrist twist the forearm takes
    bool animated_upper_body = false; // false: spine/neck/shoulders ignore the game's animation (recoil etc.)
} S;

void LoadSettings() {
    S.enabled = config::GetBool("body_ik", true);
    S.yaw_deadzone = config::GetFloat("body_turn_deadzone", 45.f);
    S.max_crouch = config::GetFloat("body_max_crouch_cm", 70.f);
    S.max_lean = config::GetFloat("body_max_lean_cm", 30.f);
    S.max_spine_bend = config::GetFloat("body_max_spine_bend", 45.f);
    // Renamed from body_arm_stretch_cm (8): the hand must stay on the gun.
    S.arm_stretch = config::GetFloat("body_hand_reach_extra_cm", 30.f);
    S.hand_offset[0] = config::GetFloat("hand_offset_forward_cm", 0.f);
    S.hand_offset[1] = config::GetFloat("hand_offset_right_cm", 0.f);
    S.hand_offset[2] = config::GetFloat("hand_offset_up_cm", 0.f);
    S.hand_rot[0] = config::GetFloat("hand_pitch_offset", 0.f);
    S.hand_rot[1] = config::GetFloat("hand_yaw_offset", 0.f);
    S.hand_rot[2] = config::GetFloat("hand_roll_offset", 0.f);
    S.palm_depth = config::GetFloat("hand_palm_depth_cm", 3.f);
    S.hide_head = config::GetBool("body_hide_head", true);
    S.shadow = config::GetInt("body_shadow", 1);
    S.hand_poses = config::GetBool("hand_poses", true);
    S.finger_scale = config::GetFloat("finger_curl_scale", 1.f);
    S.legs = config::GetBool("body_leg_ik", true);
    S.arm_scale = config::GetFloat("body_arm_scale", 0.f);
    S.max_arm_scale = config::GetFloat("body_max_arm_scale", 1.25f);
    S.neck_down = config::GetFloat("body_neck_below_eyes_cm", 14.f);
    S.neck_back = config::GetFloat("body_neck_behind_eyes_cm", 8.f);
    S.grip_angle = config::GetFloat("hand_grip_angle", 20.f);
    S.gun_hand[0] = config::GetFloat("hand_on_gun_forward_cm", 1.5f);
    S.gun_hand[1] = config::GetFloat("hand_on_gun_right_cm", 0.f);
    S.gun_hand[2] = config::GetFloat("hand_on_gun_up_cm", 7.f);
    S.wrist_twist_share = config::GetFloat("body_wrist_twist_share", 0.75f);
    S.animated_upper_body = config::GetBool("body_animated_upper_body", false);
}

// ---- engine helpers ----
void SetVisible(UObject comp, bool visible) {
    ue::Call c(comp, "SetVisibility");
    c.Set("bNewVisibility", visible).Set("bPropagateToChildren", false);
    c.Invoke();
}

void CallBool(UObject comp, const char* fn, const char* param, bool v) {
    ue::Call c(comp, fn);
    c.Set(param, v);
    c.Invoke();
}

std::string Lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const std::string mixamo = "mixamorig:";
    if (s.rfind(mixamo, 0) == 0) s = s.substr(mixamo.size());
    return s;
}

// ---- bones ----
enum Side : int { kL = 0, kR = 1 };

struct Bone {
    bool ok = false;
    FName name;
};

struct Skeleton {
    Bone pelvis, neck, head;
    std::vector<Bone> spine;  // bottom to top
    Bone clavicle[2], upperarm[2], lowerarm[2], hand[2], forearm_twist[2], upperarm_twist[2];
    Bone thigh[2], calf[2], foot[2];
    Bone finger[2][hand_pose::kFingerCount][3];
    bool arms_ok[2] = {false, false};
    bool legs_ok[2] = {false, false};
    bool fingers_ok[2] = {false, false};
};

std::map<std::string, FName> ReadBoneNames(UObject mesh) {
    std::map<std::string, FName> out;
    ue::Call n(mesh, "GetNumBones");
    if (!n.Invoke()) return out;
    int count = n.Result<int32_t>();
    std::string list;
    for (int i = 0; i < count && i < 1024; ++i) {
        ue::Call b(mesh, "GetBoneName");
        b.Set("BoneIndex", static_cast<int32_t>(i));
        if (!b.Invoke()) continue;
        FName name = b.Result<FName>();
        std::string s = ue::names::ToString(name);
        out[Lower(s)] = name;
        list += s;
        list += i + 1 < count ? ", " : "";
    }
    log::Info("body: %d bones: %s", count, list.c_str());
    return out;
}

Bone Pick(const std::map<std::string, FName>& bones, std::initializer_list<std::string> candidates) {
    for (const std::string& c : candidates) {
        auto it = bones.find(c);
        if (it != bones.end()) return Bone{true, it->second};
    }
    return Bone{};
}

Skeleton MapSkeleton(UObject mesh) {
    Skeleton k;
    auto bones = ReadBoneNames(mesh);
    k.pelvis = Pick(bones, {"pelvis", "hips", "hip", "root_pelvis"});
    k.neck = Pick(bones, {"neck_01", "neck", "neck1", "neck_1"});
    k.head = Pick(bones, {"head"});
    for (const char* s : {"spine_01", "spine_02", "spine_03", "spine_04", "spine_05"}) {
        Bone b = Pick(bones, {s});
        if (b.ok) k.spine.push_back(b);
    }
    if (k.spine.empty())
        for (const char* s : {"spine", "spine1", "spine2", "spine3"}) {
            Bone b = Pick(bones, {s});
            if (b.ok) k.spine.push_back(b);
        }
    static const char* kFingers[] = {"thumb", "index", "middle", "ring", "pinky"};
    for (int side = 0; side < 2; ++side) {
        const std::string s = side == kL ? "l" : "r";
        const std::string full = side == kL ? "left" : "right";
        k.clavicle[side] = Pick(bones, {"clavicle_" + s, full + "shoulder"});
        k.upperarm[side] = Pick(bones, {"upperarm_" + s, full + "arm", "upper_arm_" + s});
        k.lowerarm[side] = Pick(bones, {"lowerarm_" + s, full + "forearm", "forearm_" + s});
        k.hand[side] = Pick(bones, {"hand_" + s, full + "hand"});
        k.forearm_twist[side] = Pick(bones, {"lowerarm_twist_01_" + s, "forearm_twist_" + s, full + "forearmroll"});
        k.upperarm_twist[side] = Pick(bones, {"upperarm_twist_01_" + s, full + "armroll"});
        k.thigh[side] = Pick(bones, {"thigh_" + s, full + "upleg"});
        k.calf[side] = Pick(bones, {"calf_" + s, full + "leg"});
        k.foot[side] = Pick(bones, {"foot_" + s, full + "foot"});
        bool all_fingers = true;
        for (int f = 0; f < hand_pose::kFingerCount; ++f)
            for (int j = 0; j < 3; ++j) {
                std::string n = kFingers[f];
                std::string idx = std::to_string(j + 1);
                k.finger[side][f][j] = Pick(bones, {n + "_0" + idx + "_" + s, full + "hand" + n + idx,
                                                    n + idx + "_" + s, n + "_" + idx + "_" + s});
                all_fingers &= k.finger[side][f][j].ok;
            }
        k.arms_ok[side] = k.upperarm[side].ok && k.lowerarm[side].ok && k.hand[side].ok;
        k.legs_ok[side] = k.thigh[side].ok && k.calf[side].ok && k.foot[side].ok;
        k.fingers_ok[side] = all_fingers;
    }
    log::Info("body: skeleton pelvis=%d spine=%zu neck=%d head=%d arms L%d R%d legs L%d R%d fingers L%d R%d",
              k.pelvis.ok, k.spine.size(), k.neck.ok, k.head.ok, k.arms_ok[0], k.arms_ok[1], k.legs_ok[0],
              k.legs_ok[1], k.fingers_ok[0], k.fingers_ok[1]);
    return k;
}

// Bone access on the poseable copy. World space unless noted. The engine
// rebuilds the component-space pose from the local transforms on every
// call, so reads always see earlier writes this frame (parents first).
UObject g_pose = 0;

Xf BoneW(const Bone& b) {
    ue::Call c(g_pose, "GetBoneTransformByName");
    c.Set("BoneName", b.name).Set("BoneSpace", kWorldSpace);
    c.Invoke();
    return c.Result<Xf>();
}

void SetBoneW(const Bone& b, const Xf& t) {
    ue::Call c(g_pose, "SetBoneTransformByName");
    c.Set("BoneName", b.name).Set("InTransform", t).Set("BoneSpace", kWorldSpace);
    c.Invoke();
}

// Pre-multiplies a world-space rotation onto a bone, keeping its position.
void RotateBoneW(const Bone& b, const FQuat& delta) {
    Xf t = BoneW(b);
    t.rotation = Normalized(delta * t.rotation);
    SetBoneW(b, t);
}

void ResetBone(const Bone& b) {
    ue::Call c(g_pose, "ResetBoneTransformByName");
    c.Set("BoneName", b.name);
    c.Invoke();
}

// ---- state ----
struct State {
    UObject player = 0;
    UObject source = 0;  // the game's BodyMesh
    UObject pose = 0;    // our PoseableMeshComponent
    Skeleton skel;
    std::vector<UObject> followers;
    bool active = false;
    double body_yaw = 0;  // body yaw relative to the actor, degrees
    hand_pose::Curl curl[2];
    HandFrame frame[2];
    FVector elbow_hinge_local[2];  // in upper-arm bone space
    bool hinge_known[2] = {false, false};
    double reach_seen[2] = {0, 0};   // smoothed shoulder->hand distance
    double reach_max[2] = {0, 0};    // longest reach seen this session
    double arm_scale[2] = {1, 1};
    FVector knee_hinge_local[2];
    bool knee_known[2] = {false, false};
    bool failed = false;
    int frames = 0;
    // Leg animation check: how much the game's animated feet move (relative
    // to the pelvis) while the character walks.
    double stand_dz = -1e9;  // highest real-neck vs avatar-neck height seen: standing tall
    double diag_time = 0, diag_moving = 0, diag_foot_travel = 0;
    FVector diag_last_foot;
    bool diag_have_foot = false;
} G;

// Gear slots: offsets in cm (forward, right, up) in the body's yawed frame,
// from the pelvis (belt) or the top spine bone (chest/back).
struct SlotDef {
    bool chest;
    FVector offset;
};
const SlotDef kSlots[static_cast<int>(Slot::kCount)] = {
    {false, {2, 20, -6}},    // right hip holster
    {false, {10, -15, 0}},   // left pouch 1 (front)
    {false, {4, -18, 0}},    // left pouch 2
    {false, {-3, -18, 0}},   // left pouch 3
    {true, {14, 0, -2}},     // chest
    {true, {-16, 0, 4}},     // back
};
hands::Pose g_slots[static_cast<int>(Slot::kCount)];
bool g_slots_valid = false;

UObject g_poseable_class = 0;
UObject g_skinned_class = 0;

UObject FindBodyMesh(UObject player) {
    UObject mesh = 0;
    if (ue::GetProp(player, "BodyMesh", mesh) && mesh) return mesh;
    if (ue::GetProp(player, "Mesh", mesh) && mesh) return mesh;
    return 0;
}

struct TArrayRaw {
    uintptr_t data;
    int32_t num;
    int32_t max;
};

// Skinned meshes anywhere under `comp` that use `leader` as their leader
// pose (clothing, hair, gear).
void CollectFollowers(UObject comp, UObject leader, int depth, std::vector<UObject>* out) {
    if (!comp || depth > 5) return;
    int32_t off = ue::PropertyOffset(comp, "AttachChildren");
    TArrayRaw arr{};
    if (off < 0 || !mem::Read(comp + off, arr) || arr.num <= 0 || arr.num > 256) return;
    int32_t leader_index = mem::ReadOr<int32_t>(leader + ue::off::kObjIndex, -1);
    for (int i = 0; i < arr.num; ++i) {
        UObject child = mem::ReadOr<UObject>(arr.data + i * sizeof(UObject), 0);
        if (!child) continue;
        if (g_skinned_class && ue::reflection::IsA(child, g_skinned_class)) {
            int32_t loff = ue::PropertyOffset(child, "LeaderPoseComponent");
            int32_t idx = loff >= 0 ? mem::ReadOr<int32_t>(child + loff, -1) : -1;
            if (idx == leader_index && child != leader) out->push_back(child);
        }
        CollectFollowers(child, leader, depth + 1, out);
    }
}

void SetLeader(UObject follower, UObject leader) {
    ue::Call c(follower, "SetLeaderPoseComponent");
    c.Set("NewLeaderBoneComponent", leader).Set("bForceUpdate", true).Set("bInFollowerShouldTickPose", false);
    c.Invoke();
}

bool CreatePose(UObject player, UObject source) {
    if (!g_poseable_class) g_poseable_class = ue::FindObject("Class /Script/Engine.PoseableMeshComponent");
    if (!g_skinned_class) g_skinned_class = ue::FindObject("Class /Script/Engine.SkinnedMeshComponent");
    if (!g_poseable_class) {
        log::Error("body: PoseableMeshComponent class not found");
        return false;
    }
    ue::Call asset(source, "GetSkinnedAsset");
    UObject mesh = asset.Invoke() ? asset.Result<UObject>() : 0;
    if (!mesh) {
        log::Error("body: BodyMesh has no mesh asset");
        return false;
    }
    ue::Call add(player, "AddComponentByClass");
    add.Set("Class", g_poseable_class).Set("bManualAttachment", true).Set("RelativeTransform", Xf{})
        .Set("bDeferredFinish", false);
    if (!add.Invoke() || !add.Result<UObject>()) {
        log::Error("body: could not add a PoseableMeshComponent to the player");
        return false;
    }
    UObject pose = add.Result<UObject>();

    ue::Call attach(pose, "K2_AttachToComponent");
    attach.Set("Parent", source).Set("SocketName", FName{}).Set("LocationRule", kSnapToTarget)
        .Set("RotationRule", kSnapToTarget).Set("ScaleRule", kSnapToTarget).Set("bWeldSimulatedBodies", false);
    attach.Invoke();

    ue::Call set_mesh(pose, "SetSkinnedAssetAndUpdate");
    set_mesh.Set("NewMesh", mesh).Set("bReinitPose", true);
    set_mesh.Invoke();

    ue::Call nm(source, "GetNumMaterials");
    int materials = nm.Invoke() ? nm.Result<int32_t>() : 0;
    for (int i = 0; i < materials && i < 64; ++i) {
        ue::Call gm(source, "GetMaterial");
        gm.Set("ElementIndex", static_cast<int32_t>(i));
        if (!gm.Invoke()) continue;
        ue::Call sm(pose, "SetMaterial");
        sm.Set("ElementIndex", static_cast<int32_t>(i)).Set("Material", gm.Result<UObject>());
        sm.Invoke();
    }

    // The hidden original must keep animating with full detail: the game
    // camera rides on it and we copy its pose.
    ue::SetProp<uint8_t>(source, "VisibilityBasedAnimTickOption", 0);  // AlwaysTickPoseAndRefreshBones
    ue::Call lod(source, "SetForcedLOD");
    lod.Set("InNewForcedLOD", static_cast<int32_t>(1));
    lod.Invoke();

    G.pose = pose;
    log::Info("body: VR body created (%s, %d materials)", ue::GetName(mesh).c_str(), materials);
    return true;
}

void ApplyShadows() {
    CallBool(G.source, "SetCastHiddenShadow", "NewCastHiddenShadow", S.shadow == 1);
    CallBool(G.pose, "SetCastShadow", "NewCastShadow", S.shadow == 2);
}

void Activate() {
    if (G.active || !G.pose) return;
    G.followers.clear();
    CollectFollowers(G.source, G.source, 0, &G.followers);
    for (UObject f : G.followers) SetLeader(f, G.pose);
    SetVisible(G.pose, true);
    SetVisible(G.source, false);
    ApplyShadows();
    G.active = true;
    G.body_yaw = 0;
    log::Info("body: VR body on (%zu clothing/gear meshes follow it)", G.followers.size());
}

void Deactivate() {
    g_slots_valid = false;
    if (!G.active) return;
    for (UObject f : G.followers) SetLeader(f, G.source);
    G.followers.clear();
    if (G.pose) SetVisible(G.pose, false);
    if (G.source) {
        SetVisible(G.source, true);
        CallBool(G.source, "SetCastHiddenShadow", "NewCastHiddenShadow", false);
    }
    G.active = false;
    log::Info("body: VR body off, game body back");
}

bool Setup(UObject player) {
    UObject source = FindBodyMesh(player);
    if (!source) return false;
    if (player == G.player && source == G.source && G.pose) return true;
    if (G.failed && player == G.player && source == G.source) return false;
    G = State();
    G.player = player;
    G.source = source;
    if (!CreatePose(player, source)) {
        G.failed = true;
        return false;
    }
    G.skel = MapSkeleton(source);
    if (!G.skel.pelvis.ok && !G.skel.arms_ok[0] && !G.skel.arms_ok[1]) {
        log::Error("body: skeleton not recognised, VR body disabled (bone list is in the log)");
        G.failed = true;
        SetVisible(G.pose, false);
        return false;
    }
    return true;
}

// ---- solving ----
FVector YawDir(double yaw_deg) {
    double r = yaw_deg * kDegToRad;
    return {std::cos(r), std::sin(r), 0};
}

// Rotates the root bone of a two-bone chain so its middle joint lands on
// `mid` and the chain bends in the plane of (root, mid, end); then the
// middle bone toward `end`. hinge_local: remembered bend axis in the root
// bone's space, for when the animated limb is straight.
void SolveChain(const Bone& root_b, const Bone& mid_b, const Bone& end_b, const TwoBoneResult& sol,
                FVector* hinge_local, bool* hinge_known, bool place_mid = false) {
    Xf root = BoneW(root_b);
    FVector mid = BoneW(mid_b).translation, end = BoneW(end_b).translation;
    FVector hinge_now = Cross(mid - root.translation, end - mid);
    if (Length(hinge_now) > 0.05 * Length(mid - root.translation) * Length(end - mid)) {
        *hinge_local = root.rotation.Inverse().Rotate(Normalize(hinge_now));
        *hinge_known = true;
    }
    FVector hinge_want = Cross(sol.mid - root.translation, sol.end - sol.mid);
    FQuat q;
    if (*hinge_known && Length(hinge_want) > 1e-3) {
        FVector hinge_cur = root.rotation.Rotate(*hinge_local);
        q = AlignFrames(mid - root.translation, hinge_cur, sol.mid - root.translation, hinge_want);
    } else {
        q = Arc(mid - root.translation, sol.mid - root.translation);
    }
    root.rotation = Normalized(q * root.rotation);
    SetBoneW(root_b, root);

    Xf m = BoneW(mid_b);
    if (place_mid) {
        // Lengthened limb: put the middle joint exactly where the solver did.
        m.translation = sol.mid;
        SetBoneW(mid_b, m);
    }
    FVector e = BoneW(end_b).translation;
    RotateBoneW(mid_b, Arc(e - m.translation, sol.end - m.translation));
}

// Once per gun: where its parts sit in the gun frame relative to
// MainHandPosition, so the hand-on-grip offset can be computed instead of
// eyeballed.
UObject g_logged_gun = 0;
void LogGunGeometry(uintptr_t gun, const hands::Pose& main) {
    if (!gun || gun == g_logged_gun) return;
    g_logged_gun = gun;
    std::string line;
    for (const char* name : {"MagazinePoint", "ChargingHandPoint", "GunParts_Trigger", "GunParts_Bolt(Slide)",
                             "GunParts_MagazineCatch", "MainHandPosition", "SubHandPosition"}) {
        UObject comp = 0;
        if (!ue::GetProp(gun, name, comp) || !comp) continue;
        ue::Call c(comp, "K2_GetComponentToWorld");
        if (!c.Invoke()) continue;
        FVector rel = main.rotation.Inverse().Rotate(c.Result<Xf>().translation - main.location);
        char buf[128];
        std::snprintf(buf, sizeof(buf), " %s=(%.1f %.1f %.1f)", name, rel.X, rel.Y, rel.Z);
        line += buf;
    }
    log::Info("body: gun %s parts in gun frame from MainHandPosition (barrel, right, up cm):%s",
              ue::GetName(gun).c_str(), line.c_str());
}

void SolveArm(int side, const hands::HandState& h, const FVector& body_fwd, const FVector& body_right,
              double dt) {
    const Skeleton& k = G.skel;
    if (!k.arms_ok[side]) return;
    const hands::Hand hs = side == kL ? hands::kLeft : hands::kRight;
    hands::HandGrab grab = hands::GetGrab(hs);

    // Measure the avatar's hand once (finger bases do not move with the pose).
    if (!G.frame[side].valid && k.fingers_ok[side]) {
        Xf hand = BoneW(k.hand[side]);
        G.frame[side] = MeasureHand(hand.translation, hand.rotation,
                                    BoneW(k.finger[side][hand_pose::kIndex][0]).translation,
                                    BoneW(k.finger[side][hand_pose::kMiddle][0]).translation,
                                    BoneW(k.finger[side][hand_pose::kPinky][0]).translation, side == kL,
                                    S.palm_depth);
        if (G.frame[side].valid) log::Info("body: measured the %s hand", side == kL ? "left" : "right");
    }

    FVector fist = h.grip.location;
    if (grab.kind != hands::GrabKind::kNone && grab.kind != hands::GrabKind::kHolster)
        fist = grab.target.location;
    FVector fwd = h.grip.rotation.Rotate(FVector{1, 0, 0});
    FVector palm = h.palm_normal;
    fist = fist + h.grip.rotation.Rotate(FVector{S.hand_offset[0], S.hand_offset[1], S.hand_offset[2]});

    if (grab.kind == hands::GrabKind::kGunGrip) {
        // Wrap the hand around the gun's grip, not the controller: the gun
        // sits pitched and shifted relative to the controller. target.rotation
        // is the gun frame (X barrel, Y right, Z up). The fist axis runs up the
        // grip, which slants back toward the bottom; the palm presses on the
        // side of the grip facing across the gun.
        const FQuat& g = grab.target.rotation;
        FVector barrel = g.Rotate(FVector{1, 0, 0}), right = g.Rotate(FVector{0, 1, 0}), up = g.Rotate(FVector{0, 0, 1});
        double a = S.grip_angle * kDegToRad;
        fwd = Normalize(up * std::cos(a) + barrel * std::sin(a));
        palm = side == kL ? right : right * -1.0;
        // The game's MainHandPosition sits below the middle of the grip
        // (fourth headset test: fist about 7 cm low and 1.5 cm behind), so
        // move the fist up the grip in the gun's own frame.
        fist = fist + barrel * S.gun_hand[0] + right * S.gun_hand[1] + up * S.gun_hand[2];
        LogGunGeometry(grab.object, grab.target);
    }

    FQuat hand_rot;
    FVector hand_pos;
    if (G.frame[side].valid) {
        hand_rot = HandRotation(G.frame[side], fwd, palm);
    } else {
        hand_rot = h.grip.rotation;  // no fingers to measure: trust the controller axes
    }
    if (S.hand_rot[0] || S.hand_rot[1] || S.hand_rot[2]) {
        double mirror = side == kL ? -1.0 : 1.0;
        hand_rot = Normalized(h.grip.rotation *
                              ToQuat(FRotator{S.hand_rot[0], S.hand_rot[1] * mirror, S.hand_rot[2] * mirror}) *
                              h.grip.rotation.Inverse() * hand_rot);
    }
    hand_pos = G.frame[side].valid ? fist - hand_rot.Rotate(G.frame[side].fist) : fist;

    // Shoulder reach: when the hand is far out, the collarbone swings toward
    // it a little, like a real shoulder.
    FVector shoulder = BoneW(k.upperarm[side]).translation;
    FVector elbow = BoneW(k.lowerarm[side]).translation;
    FVector wrist = BoneW(k.hand[side]).translation;
    double upper = Length(elbow - shoulder), lower = Length(wrist - elbow);
    if (upper < 1.0 || lower < 1.0) return;

    // Arm length: learn from how far the player actually reaches, so the
    // avatar's hands can always get to the controllers.
    double dist = Length(hand_pos - shoulder);
    double reach = upper + lower;
    double a = 1.0 - std::exp(-dt / 0.15);
    G.reach_seen[side] += (dist - G.reach_seen[side]) * a;
    if (G.reach_seen[side] < reach * 1.5) G.reach_max[side] = std::max(G.reach_max[side], G.reach_seen[side]);
    double want_scale = S.arm_scale > 0.0 ? S.arm_scale
                                          : std::clamp(G.reach_max[side] / (reach * 0.98), 1.0, S.max_arm_scale);
    if (want_scale > G.arm_scale[side] + 0.01) {
        G.arm_scale[side] = want_scale;
        log::Info("body: %s arm length x%.2f (reach %.0f cm, avatar %.0f cm)", side == kL ? "left" : "right",
                  want_scale, G.reach_max[side], reach);
    } else if (S.arm_scale > 0.0) {
        G.arm_scale[side] = want_scale;
    }
    upper *= G.arm_scale[side];
    lower *= G.arm_scale[side];
    reach = upper + lower;

    if (k.clavicle[side].ok && dist > 0.8 * reach) {
        Xf clav = BoneW(k.clavicle[side]);
        double f = std::clamp((dist / reach - 0.8) / 0.3, 0.0, 1.0);
        FQuat q = Arc(shoulder - clav.translation, hand_pos - clav.translation);
        RotateBoneW(k.clavicle[side], ClampAngle(Scale(q, 0.3 * f), 20.0 * kDegToRad));
        shoulder = BoneW(k.upperarm[side]).translation;
    }

    FVector outward = side == kL ? body_right * -1.0 : body_right;
    // Elbows hang down and a little out and back; raise the hand above the
    // shoulder and they swing further out.
    double above = std::max(0.0, hand_pos.Z - shoulder.Z) / reach;
    FVector pole = FVector{0, 0, -1} * (1.0 - 0.6 * above) + outward * (0.7 + 0.6 * above) - body_fwd * 0.3;
    TwoBoneResult sol = SolveTwoBone(shoulder, hand_pos, upper, lower, pole, S.arm_stretch);
    SolveChain(k.upperarm[side], k.lowerarm[side], k.hand[side], sol, &G.elbow_hinge_local[side],
               &G.hinge_known[side], G.arm_scale[side] > 1.001 || Length(sol.end - shoulder) > reach);

    // Wrist twist: a real forearm rolls along its length when you turn your
    // wrist. Give most of the roll to the forearm (and its twist bone) so
    // the wrist doesn't pinch.
    {
        Xf fore = BoneW(k.lowerarm[side]);
        Xf hand_now = BoneW(k.hand[side]);
        FVector axis = Normalize(hand_now.translation - fore.translation);
        double twist = TwistAngle(hand_rot * hand_now.rotation.Inverse(), axis);
        double share = std::clamp(S.wrist_twist_share, 0.0, 1.0);
        if (k.forearm_twist[side].ok) {
            RotateBoneW(k.lowerarm[side], AxisAngle(axis, twist * share * 0.5));
            RotateBoneW(k.forearm_twist[side], AxisAngle(axis, twist * share * 0.5));
        } else {
            RotateBoneW(k.lowerarm[side], AxisAngle(axis, twist * share));
        }
    }

    Xf hand = BoneW(k.hand[side]);
    hand.rotation = hand_rot;
    hand.translation = sol.end;
    SetBoneW(k.hand[side], hand);

    // Fingers.
    if (!S.hand_poses || !k.fingers_ok[side] || !G.frame[side].valid) return;
    hand_pose::Curl target = hand_pose::Target(grab.kind, hands::GetFingers(hs));
    hand_pose::Smooth(&G.curl[side], target, dt);
    FVector palm_w = hand_rot.Rotate(G.frame[side].palm);
    FVector pinky_base = BoneW(k.finger[side][hand_pose::kPinky][0]).translation;
    for (int f = 0; f < hand_pose::kFingerCount; ++f) {
        const Bone* j = k.finger[side][f];
        FVector a = BoneW(j[0]).translation, b = BoneW(j[1]).translation;
        FVector toward = palm_w;
        if (f == hand_pose::kThumb) toward = Normalize(Normalize(pinky_base - a) + palm_w * 0.5);
        FVector axis = Cross(b - a, toward);
        if (Length(axis) < 1e-4) continue;
        hand_pose::JointLimits lim = hand_pose::Limits(static_cast<hand_pose::Finger>(f));
        double c = std::min(1.2, G.curl[side].finger[f] * S.finger_scale);
        const double deg[3] = {lim.base, lim.middle, lim.tip};
        for (int n = 0; n < 3; ++n) RotateBoneW(j[n], AxisAngle(axis, c * deg[n] * kDegToRad));
    }
}

bool g_toggle_down = false;

void PollToggle() {
    bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (f9 && !ctrl && !g_toggle_down) {
        S.enabled = !S.enabled;
        log::Info("body: F9 -> VR body %s", S.enabled ? "on" : "off");
    }
    g_toggle_down = f9;
}

void OnTick(UObject player, float dt_f) {
    const double dt = dt_f;
    PollToggle();
    hands::Pose head;
    const bool vr = hands::Head(&head);
    if (!S.enabled || !vr) {
        if (G.active && G.player == player) Deactivate();
        return;
    }
    if (!Setup(player)) return;
    Activate();
    if (!G.active) return;
    g_pose = G.pose;
    const Skeleton& k = G.skel;

    ue::Call copy(G.pose, "CopyPoseFromSkeletalComponent");
    copy.Set("InComponentToCopy", G.source);
    if (!copy.Invoke()) return;
    ++G.frames;

    hands::HandState hs[2] = {hands::Get(hands::kLeft), hands::Get(hands::kRight)};

    // Fingers start from the skeleton's straight reference pose; hand poses
    // curl them from there.
    if (S.hand_poses)
        for (int side = 0; side < 2; ++side)
            if (hs[side].valid && k.fingers_ok[side])
                for (auto& f : k.finger[side])
                    for (const Bone& b : f) ResetBone(b);

    // The upper body belongs to the player, not the flat-screen animation:
    // no recoil kick, aim pitch or idle sway in the spine and shoulders.
    // Arms are solved by IK below; the spine only leans toward the real head.
    if (!S.animated_upper_body) {
        for (const Bone& b : k.spine) ResetBone(b);
        if (k.neck.ok) ResetBone(k.neck);
        if (k.head.ok) ResetBone(k.head);
        for (int side = 0; side < 2; ++side) {
            if (!hs[side].valid) continue;
            if (k.clavicle[side].ok) ResetBone(k.clavicle[side]);
            if (k.upperarm_twist[side].ok) ResetBone(k.upperarm_twist[side]);
            if (k.forearm_twist[side].ok) ResetBone(k.forearm_twist[side]);
        }
    }

    // Animated pose, before we change anything.
    ue::Call ar(player, "K2_GetActorRotation");
    ar.Invoke();
    const double actor_yaw = ar.Result<FRotator>().Yaw;
    Xf pelvis = k.pelvis.ok ? BoneW(k.pelvis) : Xf{};
    FVector neck_anim = k.neck.ok ? BoneW(k.neck).translation : FVector{};
    Xf foot_anim[2];
    for (int s = 0; s < 2; ++s)
        if (k.legs_ok[s]) foot_anim[s] = BoneW(k.foot[s]);

    // Body yaw follows the head with a dead zone, and catches up while moving.
    const double head_rel = NormalizeAxis(YawOf(head.rotation) - actor_yaw);
    double diff = NormalizeAxis(head_rel - G.body_yaw);
    if (std::fabs(diff) > S.yaw_deadzone) {
        G.body_yaw += diff > 0 ? diff - S.yaw_deadzone : diff + S.yaw_deadzone;
        diff = NormalizeAxis(head_rel - G.body_yaw);
    }
    ue::Call vel(player, "GetVelocity");
    vel.Invoke();
    FVector v = vel.Result<FVector>();
    if (v.X * v.X + v.Y * v.Y > 30.0 * 30.0) G.body_yaw += diff * (1.0 - std::exp(-5.0 * dt));
    G.body_yaw = NormalizeAxis(G.body_yaw);
    const FQuat yaw_q = YawOnly(G.body_yaw);
    const FVector body_fwd = YawDir(actor_yaw + G.body_yaw);
    const FVector body_right = YawDir(actor_yaw + G.body_yaw + 90.0);

    // Where the player's neck really is: a fixed offset below and behind
    // the eyes, in head space (so nodding pivots around the neck). The body
    // is moved and bent so the avatar's neck sits there, which puts its
    // shoulders where the player's are.
    FVector neck_anim_r = pelvis.translation + yaw_q.Rotate(neck_anim - pelvis.translation);
    FVector neck_real = head.location + head.rotation.Rotate(FVector{-S.neck_back, 0, -S.neck_down});
    FVector delta = k.neck.ok ? neck_real - neck_anim_r : FVector{};
    if (Length(delta) > 150.0) delta = FVector{};  // teleport / recenter in progress
    // Crouch is measured against how high the player's neck has been while
    // standing, not the avatar's height (people and the avatar differ).
    if (delta.Z > G.stand_dz) G.stand_dz = delta.Z;
    const double down = delta.Z - G.stand_dz;
    // With fixed_height the view never drops, so neither does the body.
    const double crouch = config::GetBool("fixed_height", true) ? 0.0 : std::clamp(down, -S.max_crouch, 0.0);
    // Most of a horizontal offset moves the whole body under the head (you
    // stepped, or the game's body stands behind its camera); the spine leans
    // for the rest, up to body_max_lean_cm.
    FVector offset{delta.X, delta.Y, 0};
    FVector pelvis_shift = offset * 0.7;
    if (Length(pelvis_shift) > 60.0) pelvis_shift = Normalize(pelvis_shift) * 60.0;
    FVector lean = offset - pelvis_shift;
    if (Length(lean) > S.max_lean) lean = Normalize(lean) * S.max_lean;
    if (k.pelvis.ok) {
        Xf p = pelvis;
        p.rotation = Normalized(yaw_q * p.rotation);
        p.translation = pelvis.translation + FVector{0, 0, crouch} + pelvis_shift;
        SetBoneW(k.pelvis, p);
    }

    // Spine leans toward the real head position.
    if (k.neck.ok && !k.spine.empty() && k.pelvis.ok) {
        FVector neck_target = neck_anim_r + pelvis_shift + FVector{0, 0, std::max(down, -S.max_crouch - 30.0)} + lean;
        FVector base = BoneW(k.spine[0]).translation;
        FVector neck_now = BoneW(k.neck).translation;
        FQuat bend = ClampAngle(Arc(neck_now - base, neck_target - base), S.max_spine_bend * kDegToRad);
        FQuat step = Scale(bend, 1.0 / k.spine.size());
        for (const Bone& b : k.spine) RotateBoneW(b, step);
    }

    if (S.hide_head && k.head.ok) {
        ue::Call sc(G.pose, "SetBoneScaleByName");
        sc.Set("BoneName", k.head.name).Set("InScale3D", FVector{0.001, 0.001, 0.001})
            .Set("BoneSpace", kComponentSpace);
        sc.Invoke();
    }

    for (int side = 0; side < 2; ++side)
        if (hs[side].valid) SolveArm(side, hs[side], body_fwd, body_right, dt);

    // Legs keep the animated feet where they were.
    if (S.legs && k.pelvis.ok)
        for (int s = 0; s < 2; ++s) {
            if (!k.legs_ok[s]) continue;
            FVector hip = BoneW(k.thigh[s]).translation;
            FVector knee = BoneW(k.calf[s]).translation;
            FVector ankle = BoneW(k.foot[s]).translation;
            double upper = Length(knee - hip), lower = Length(ankle - knee);
            if (upper < 1.0 || lower < 1.0) continue;
            FVector outward = s == kL ? body_right * -1.0 : body_right;
            // The walk animation turns and moves with the body: feet go
            // where the animation puts them, relative to the turned pelvis.
            FVector foot_target = pelvis.translation + pelvis_shift +
                                  yaw_q.Rotate(foot_anim[s].translation - pelvis.translation);
            TwoBoneResult sol = SolveTwoBone(hip, foot_target, upper, lower,
                                             body_fwd + outward * 0.15, 0.0);
            SolveChain(k.thigh[s], k.calf[s], k.foot[s], sol, &G.knee_hinge_local[s], &G.knee_known[s]);
            Xf foot = BoneW(k.foot[s]);
            foot.rotation = Normalized(yaw_q * foot_anim[s].rotation);
            SetBoneW(k.foot[s], foot);
        }

    // Gear slots on the posed body.
    if (k.pelvis.ok) {
        FQuat body_q = YawOnly(actor_yaw + G.body_yaw);
        FVector belt = BoneW(k.pelvis).translation;
        FVector chest = k.spine.empty() ? belt : BoneW(k.spine.back()).translation;
        for (int i = 0; i < static_cast<int>(Slot::kCount); ++i) {
            g_slots[i].location = (kSlots[i].chest ? chest : belt) + body_q.Rotate(kSlots[i].offset);
            g_slots[i].rotation = body_q;
        }
        g_slots_valid = true;
    }

    if (k.legs_ok[0] && k.pelvis.ok) {
        FVector rel = pelvis.translation - foot_anim[0].translation;
        if (G.diag_have_foot) G.diag_foot_travel += Length(rel - G.diag_last_foot);
        G.diag_last_foot = rel;
        G.diag_have_foot = true;
        G.diag_time += dt;
        if (v.X * v.X + v.Y * v.Y > 50.0 * 50.0) G.diag_moving += dt;
        if (G.diag_time >= 2.0) {
            if (G.diag_moving > 1.0)
                log::Info("body: walking %.1f s, animated left foot moved %.0f cm against the pelvis%s", G.diag_moving,
                          G.diag_foot_travel, G.diag_foot_travel < 5.0 ? " (game legs not animating!)" : "");
            G.diag_time = G.diag_moving = G.diag_foot_travel = 0;
        }
    }

    if (G.frames == 1 || G.frames % 3000 == 0)
        log::Info("body: frame %d yaw %.0f crouch %.0f lean %.0f", G.frames, G.body_yaw, crouch, Length(lean));
}

}  // namespace

bool GetSlot(Slot slot, hands::Pose* out) {
    int i = static_cast<int>(slot);
    if (!g_slots_valid || i < 0 || i >= static_cast<int>(Slot::kCount)) return false;
    *out = g_slots[i];
    return true;
}

void Init() {
    LoadSettings();
    game::AddTickListener(&OnTick);
    log::Info("body: ready (%s; F9 toggles)", S.enabled ? "on" : "off");
}

}  // namespace holevr::vr::body
