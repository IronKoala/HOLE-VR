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
#include "core/perf.h"
#include "game/game.h"
#include "ue/call.h"
#include "ue/names.h"
#include "ue/reflection.h"
#include "vr/body/grip_probe.h"
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
    bool finger_contact = true;   // fingers stop where they touch the held object
    double finger_radius = 0.8;   // cm, finger centre to skin on the curl side
    double mag_hold_low = 0.25;
    double mag_index_aim_max = 75.0;  // degrees the index may swing to point up a held magazine
    bool palm_seat = true;        // put the palm on the held object's surface
    double palm_skin = 2.0;       // cm from the hand bone line out to the palm skin
    double palm_along = 0.5;      // where on the palm (0 wrist .. 1 knuckles) touches   // fist below the magazine's middle, share of its height
    bool game_grip = false;  // copy the game's own hand placement on the gun (it does not wrap the grip)
    double arm_scale = 0.0;       // 0 = learn from how far you reach; else fixed multiplier
    double max_arm_scale = 1.25;
    double neck_down = 14.0;      // cm from the eyes down to the base of the neck, in head space
    double neck_back = 8.0;       // cm from the eyes back to the base of the neck
    double grip_angle = 20.0;     // pistol grip slant, degrees
    double gun_hand[3] = {3.5, -3.8, 6.2};  // fallback fist offset from MainHandPosition in the gun frame (barrel, right, up), cm
    double grip_behind_catch = 2.5;  // grip centre behind the magazine catch, cm
    double grip_below_catch = 4.5;   // grip centre down the grip from the magazine catch, cm (no guard found)
    double grip_below_guard = 1.0;   // middle finger centre below the trigger guard, cm
    double thumb_aim_max = 60.0;     // degrees the thumb may swing to point along the barrel
    double grip_side = 0.0;          // sideways shift of the fist on the grip, cm (+ = gun's right)
    double gun_palm_depth = 2.5;     // grip centre out from the palm, cm
    double gun_fist_along = 0.75;    // grip centre along wrist -> middle knuckle (0-1)
    double wrist_twist_share = 0.75;  // how much of the wrist twist the forearm takes
    bool animated_upper_body = false; // false: spine/neck/shoulders ignore the game's animation (recoil etc.)
    // Grip from the scanned gun shape (grip_probe.h).
    bool grip_scan = true;
    double scan_ms = 2.0;             // CPU time per frame for scanning a new gun, ms
    double scan_palm_skin = 1.5;      // cm from the knuckle joints out to the palm skin
    double scan_knuckle_back = 0.5;   // middle knuckle behind the front strap, cm
    double scan_thumb_above = 2.0;    // thumb above the trigger guard bottom, cm
    double scan_clearance = 0.2;      // gap kept between skin and gun, cm
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
    // Renamed from hand_use_game_grip (default 1): the game's hand does not
    // actually wrap the grip.
    S.game_grip = config::GetBool("hand_copy_game_grip", false);
    S.finger_contact = config::GetBool("finger_contact", true);
    S.finger_radius = config::GetFloat("finger_radius_cm", 0.8f);
    // Renamed from hand_mag_hold_low (0.15): reference photos hold it lower,
    // base in the palm.
    S.mag_hold_low = config::GetFloat("hand_mag_grip_low", 0.25f);
    S.mag_index_aim_max = config::GetFloat("hand_mag_index_up_max_deg", 75.f);
    S.palm_seat = config::GetBool("hand_palm_seat", true);
    S.palm_skin = config::GetFloat("hand_palm_skin_cm", 2.f);
    S.palm_along = config::GetFloat("hand_palm_seat_along", 0.5f);
    S.arm_scale = config::GetFloat("body_arm_scale", 0.f);
    S.max_arm_scale = config::GetFloat("body_max_arm_scale", 1.25f);
    S.neck_down = config::GetFloat("body_neck_below_eyes_cm", 14.f);
    S.neck_back = config::GetFloat("body_neck_behind_eyes_cm", 8.f);
    S.grip_angle = config::GetFloat("hand_grip_angle", 20.f);
    S.gun_hand[0] = config::GetFloat("hand_gun_fallback_forward_cm", 3.5f);
    S.gun_hand[1] = config::GetFloat("hand_gun_fallback_right_cm", -3.8f);
    S.gun_hand[2] = config::GetFloat("hand_gun_fallback_up_cm", 6.2f);
    S.grip_behind_catch = config::GetFloat("hand_grip_behind_mag_catch_cm", 2.5f);
    S.grip_below_catch = config::GetFloat("hand_grip_below_mag_catch_cm", 4.5f);
    S.grip_below_guard = config::GetFloat("hand_grip_below_trigger_guard_cm", 1.f);
    S.thumb_aim_max = config::GetFloat("hand_thumb_forward_max_deg", 60.f);
    S.grip_side = config::GetFloat("hand_grip_side_cm", 0.f);
    S.gun_palm_depth = config::GetFloat("hand_grip_palm_depth_cm", 2.5f);
    S.gun_fist_along = config::GetFloat("hand_grip_fist_along", 0.75f);
    S.wrist_twist_share = config::GetFloat("body_wrist_twist_share", 0.75f);
    S.animated_upper_body = config::GetBool("body_animated_upper_body", false);
    S.grip_scan = config::GetBool("hand_grip_from_gun_scan", true);
    S.scan_ms = config::GetFloat("hand_gun_scan_ms_per_frame", 2.f);
    S.scan_palm_skin = config::GetFloat("hand_scan_palm_skin_cm", 1.5f);
    S.scan_knuckle_back = config::GetFloat("hand_scan_knuckle_behind_front_cm", 0.5f);
    S.scan_thumb_above = config::GetFloat("hand_scan_thumb_above_guard_cm", 2.f);
    S.scan_clearance = config::GetFloat("hand_scan_clearance_cm", 0.2f);
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
    // The game's own hand IK on the gun (hidden body), relative to the gun's
    // grip frame; we copy it so the hand holds the gun exactly as the game does.
    bool game_grip_known = false;
    FQuat game_grip_rot;
    FVector game_grip_loc;
    bool have_prev_grip = false;
    hands::Pose prev_grip;
    bool use_game_grip = false;  // this frame
    // Finger contact: share of each joint's curl that is free before the
    // finger touches the held object (1 = curls all the way), smoothed.
    double contact[2][hand_pose::kFingerCount][3];
    bool contact_init[2] = {false, false};
    FVector seat_off[2];  // palm seating correction, hand space
    grip::HandGeo geo[2];  // knuckle positions in hand-bone space
    bool geo_ok[2] = {false, false};
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
    // The posed copy's bounds don't follow our IK (arms reach outside them),
    // so the engine culled it at some view angles. Inflate them.
    {
        ue::Call b(G.pose, "SetBoundsScale");
        b.Set("NewBoundsScale", 4.0f);
        b.Invoke();
    }
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
bool g_catch_known = false;
FVector g_catch_rel;  // magazine catch in the gun frame, from MainHandPosition

// Centre of the pistol grip (where the fist closes) in the gun frame, from
// MainHandPosition. From the magazine catch: back to the middle of the grip,
// then down the slanted grip. The game's MainHandPosition is where its wrist
// goes, ~8 cm from the grip itself, which is why the hand used to float.
// Local-space bounding box of a mesh part. The parts' GetLocalBounds
// returned all zeros in the game (seventh grip test), so read the mesh
// asset's own box first.
struct BoxMinMax {
    FVector Min, Max;
};
bool LocalBoxRead(UObject comp, FVector* mn, FVector* mx);
// Cached: asset reads cost real time and a part's mesh does not change.
std::map<UObject, std::pair<bool, BoxMinMax>> g_box_cache;
bool LocalBox(UObject comp, FVector* mn, FVector* mx) {
    auto it = g_box_cache.find(comp);
    if (it == g_box_cache.end()) {
        BoxMinMax b{};
        bool ok = LocalBoxRead(comp, &b.Min, &b.Max);
        if (g_box_cache.size() > 512) g_box_cache.clear();
        it = g_box_cache.emplace(comp, std::make_pair(ok, b)).first;
    }
    if (!it->second.first) return false;
    *mn = it->second.second.Min;
    *mx = it->second.second.Max;
    return true;
}
bool LocalBoxRead(UObject comp, FVector* mn, FVector* mx) {
    UObject mesh = 0;
    {
        ue::Call g(comp, "GetStaticMesh");
        if (g.Valid() && g.Invoke()) mesh = g.Result<UObject>();
    }
    if (!mesh) ue::GetProp(comp, "StaticMesh", mesh);
    if (mesh) {
        ue::Call b(mesh, "GetBoundingBox");
        if (b.Valid() && b.Invoke()) {
            BoxMinMax r = b.Result<BoxMinMax>();
            if (Length(r.Max - r.Min) > 0.1 && Length(r.Max - r.Min) < 500.0) {
                *mn = r.Min;
                *mx = r.Max;
                return true;
            }
        }
    }
    ue::Call lb(comp, "GetLocalBounds");
    if (lb.Valid() && lb.Invoke()) {
        FVector a = lb.Get<FVector>("Min"), b = lb.Get<FVector>("Max");
        if (Length(b - a) > 0.1 && Length(b - a) < 500.0) {
            *mn = a;
            *mx = b;
            return true;
        }
    }
    return false;
}

bool g_guard_known = false;
double g_guard_bottom = 0;  // lowest point of the trigger guard, gun frame Z from MainHandPosition

FVector GripCentre() {
    if (!g_catch_known) return FVector{S.gun_hand[0], S.gun_hand[1], S.gun_hand[2]};
    double a = S.grip_angle * kDegToRad;
    FVector down_grip{-std::sin(a), 0, -std::cos(a)};
    double along = S.grip_below_catch;
    // A high grip (owner's reference photos): the middle finger sits right
    // under the trigger guard, the web of the hand up under the slide. The
    // fist point is on the middle finger's line, so put it one finger radius
    // below the guard.
    if (g_guard_known) {
        double down = g_catch_rel.Z - (g_guard_bottom - S.grip_below_guard);
        if (down > 0.5 && down < 10.0) along = down / std::cos(a);
    }
    return g_catch_rel + FVector{-S.grip_behind_catch, S.grip_side, 0} + down_grip * along;
}

// Lowest point of the gun's trigger guard in the gun frame (from its mesh
// bounds), found by part name.
void FindTriggerGuard(UObject gun, const hands::Pose& main) {
    g_guard_known = false;
    for (UObject cls = ue::GetClass(gun); cls && !g_guard_known; cls = ue::reflection::GetSuper(cls)) {
        for (const auto& p : ue::reflection::GetProperties(cls)) {
            if (p.type != "ObjectProperty" || Lower(p.name).find("triggerguard") == std::string::npos) continue;
            UObject comp = 0;
            if (!mem::Read(gun + p.offset, comp) || !comp) continue;
            FVector mn, mx;
            ue::Call tw(comp, "K2_GetComponentToWorld");
            if (!LocalBox(comp, &mn, &mx)) {
                log::Info("body: trigger guard %s has no readable size, grip height from the magazine catch",
                          p.name.c_str());
                break;
            }
            if (!tw.Invoke()) continue;
            Xf t = tw.Result<Xf>();
            double low = 1e9;
            for (int i = 0; i < 8; ++i) {
                FVector c{(i & 1) ? mx.X : mn.X, (i & 2) ? mx.Y : mn.Y, (i & 4) ? mx.Z : mn.Z};
                FVector w = t.translation + t.rotation.Rotate(FVector{c.X * t.scale.X, c.Y * t.scale.Y, c.Z * t.scale.Z});
                low = std::min(low, main.rotation.Inverse().Rotate(w - main.location).Z);
            }
            // The guard hangs below the magazine catch; anything else is a
            // bad reading.
            if (low > -20.0 && low < 30.0 && (!g_catch_known || low < g_catch_rel.Z - 1.0)) {
                g_guard_bottom = low;
                g_guard_known = true;
                log::Info("body: trigger guard %s bottom at %.1f cm (gun frame)", p.name.c_str(), low);
            }
            break;
        }
    }
}
void LogGunGeometry(uintptr_t gun, const hands::Pose& main) {
    if (!gun || gun == g_logged_gun) return;
    g_logged_gun = gun;
    g_catch_known = false;
    std::string line;
    for (const char* name : {"MagazinePoint", "ChargingHandPoint", "GunParts_Trigger", "GunParts_Bolt(Slide)",
                             "GunParts_MagazineCatch", "MainHandPosition", "SubHandPosition"}) {
        UObject comp = 0;
        if (!ue::GetProp(gun, name, comp) || !comp) continue;
        ue::Call c(comp, "K2_GetComponentToWorld");
        if (!c.Invoke()) continue;
        FVector rel = main.rotation.Inverse().Rotate(c.Result<Xf>().translation - main.location);
        // The magazine catch sits at the top front of the grip, just behind
        // the trigger guard: the landmark the fist is placed from. Only
        // trust it if it is actually near the grip point.
        if (std::string(name) == "GunParts_MagazineCatch" && Length(rel) > 0.5 && Length(rel) < 30.0) {
            g_catch_rel = rel;
            g_catch_known = true;
        }
        char buf[128];
        std::snprintf(buf, sizeof(buf), " %s=(%.1f %.1f %.1f)", name, rel.X, rel.Y, rel.Z);
        line += buf;
    }
    log::Info("body: gun %s parts in gun frame from MainHandPosition (barrel, right, up cm):%s",
              ue::GetName(gun).c_str(), line.c_str());
    FindTriggerGuard(gun, main);
    if (g_catch_known) {
        FVector c = GripCentre();
        log::Info("body: holding %s by the grip at (%.1f %.1f %.1f) in the gun frame", ue::GetName(gun).c_str(), c.X,
                  c.Y, c.Z);
    } else {
        log::Info("body: %s has no magazine catch part, using the fallback grip offset", ue::GetName(gun).c_str());
    }
}

// Learns where the game's own (hidden, animated) right hand sits on the gun.
// The hidden body's pose is one frame old, so compare it with last frame's
// gun frame.
void LearnGameGrip(const hands::HandGrab& grab) {
    const Skeleton& k = G.skel;
    G.use_game_grip = false;
    if (grab.kind != hands::GrabKind::kGunGrip || !k.hand[kR].ok || !S.game_grip) {
        G.have_prev_grip = false;
        return;
    }
    if (G.have_prev_grip) {
        ue::Call c(G.source, "GetBoneTransform");
        c.Set("InBoneName", k.hand[kR].name).Set("TransformSpace", static_cast<uint8_t>(0));  // RTS_World
        if (c.Invoke()) {
            Xf hand = c.Result<Xf>();
            FQuat inv = G.prev_grip.rotation.Inverse();
            FVector loc = inv.Rotate(hand.translation - G.prev_grip.location);
            FQuat rot = Normalized(inv * hand.rotation);
            if (Length(loc) < 25.0) {  // the game's arm actually reached the gun
                if (!G.game_grip_known) {
                    G.game_grip_loc = loc;
                    G.game_grip_rot = rot;
                    G.game_grip_known = true;
                    log::Info("body: copying the game's hand on the gun (%.1f %.1f %.1f cm from its grip point)", loc.X,
                              loc.Y, loc.Z);
                } else {
                    // Slow average: the game's attachment barely moves.
                    G.game_grip_loc = G.game_grip_loc + (loc - G.game_grip_loc) * 0.05;
                    FQuat step = Scale(Normalized(rot * G.game_grip_rot.Inverse()), 0.05);
                    G.game_grip_rot = Normalized(step * G.game_grip_rot);
                }
            }
        }
    }
    G.prev_grip = grab.target;
    G.have_prev_grip = true;
    G.use_game_grip = G.game_grip_known;
}


// ---- finger contact ----
// Fingers curl until they touch what the hand holds, and the palm is seated
// on its surface.
//  - Gun: its parts have no collision at all (log: "collision 0"), so they
//    get query-only collision that ignores every channel: nothing in the
//    game can hit them, but K2_LineTraceComponent (which tests one component
//    and never the world) can.
//  - Magazine: a box from each part's mesh bounds (a magazine is a box).
//    Its collision is left alone so a dropped magazine still lands.
UObject g_prim_class = 0;
struct ContactObject {
    UObject actor = 0;
    std::vector<UObject> comps;
    bool boxes_mode = false;
    std::vector<Box> boxes;  // box mode, refreshed every frame
    int traces = 0, hits = 0;
    bool reported = false;
};
ContactObject g_contacts[2];         // per hand
ContactObject* g_c = &g_contacts[0];  // the hand being solved

bool SkipPart(const std::string& name) {
    std::string n = Lower(name);
    for (const char* bad : {"debug", "hand", "bounding", "eject", "fireline"})
        if (n.find(bad) != std::string::npos) return true;
    return false;
}

void CollectPrimitives(UObject actor, bool make_traceable) {
    *g_c = ContactObject{};
    g_c->actor = actor;
    g_c->boxes_mode = !make_traceable;
    if (!g_prim_class) g_prim_class = ue::FindObject("Class /Script/Engine.PrimitiveComponent");
    if (!g_prim_class || !actor) return;
    std::string names;
    int enabled = 0;
    for (UObject cls = ue::GetClass(actor); cls; cls = ue::reflection::GetSuper(cls)) {
        for (const auto& p : ue::reflection::GetProperties(cls)) {
            if (p.type != "ObjectProperty" || p.array_dim != 1) continue;
            UObject comp = 0;
            if (!mem::Read(actor + p.offset, comp) || !comp) continue;
            if (!ue::reflection::IsA(comp, g_prim_class)) continue;
            if (ue::GetOuter(comp) != actor) continue;  // only the object's own parts
            if (SkipPart(p.name)) continue;
            if (std::find(g_c->comps.begin(), g_c->comps.end(), comp) != g_c->comps.end()) continue;
            if (g_c->comps.size() >= 16) break;
            uint8_t coll = 0;
            ue::Call ce(comp, "GetCollisionEnabled");
            if (ce.Invoke()) coll = ce.Result<uint8_t>();
            if (make_traceable && coll == 0) {
                ue::Call r(comp, "SetCollisionResponseToAllChannels");
                r.Set("NewResponse", static_cast<uint8_t>(0));  // ECR_Ignore
                ue::Call e(comp, "SetCollisionEnabled");
                e.Set("NewType", static_cast<uint8_t>(1));  // QueryOnly
                if (r.Invoke() && e.Invoke()) ++enabled;
            }
            g_c->comps.push_back(comp);
            // Size of the part (local bounds x scale), to compare the gun
            // with a real one and with the avatar's hand.
            FVector size{0, 0, 0};
            FVector bmn, bmx;
            ue::Call tw(comp, "K2_GetComponentToWorld");
            if (LocalBox(comp, &bmn, &bmx) && tw.Invoke()) {
                FVector d = bmx - bmn;
                FVector sc = tw.Result<Xf>().scale;
                size = FVector{d.X * sc.X, d.Y * sc.Y, d.Z * sc.Z};
            }
            char buf[160];
            std::snprintf(buf, sizeof(buf), " %s(collision %d, %.1fx%.1fx%.1f cm)", p.name.c_str(), coll, size.X,
                          size.Y, size.Z);
            names += buf;
        }
    }
    log::Info("body: finger contact on %s (%s): %zu parts, %d made traceable:%s", ue::GetName(actor).c_str(),
              g_c->boxes_mode ? "boxes" : "traces", g_c->comps.size(), enabled, names.c_str());
}

// Box mode: where each part is this frame.
void RefreshBoxes() {
    g_c->boxes.clear();
    if (!g_c->boxes_mode) return;
    for (UObject comp : g_c->comps) {
        FVector mn, mx;
        ue::Call tw(comp, "K2_GetComponentToWorld");
        if (!LocalBox(comp, &mn, &mx) || !tw.Invoke()) continue;
        Xf t = tw.Result<Xf>();
        FVector c = (mn + mx) * 0.5, h = (mx - mn) * 0.5;
        if (Length(h) < 0.5 || Length(h) > 40.0) continue;
        c = FVector{c.X * t.scale.X, c.Y * t.scale.Y, c.Z * t.scale.Z};
        h = FVector{std::fabs(h.X * t.scale.X), std::fabs(h.Y * t.scale.Y), std::fabs(h.Z * t.scale.Z)};
        g_c->boxes.push_back(Box{t.translation + t.rotation.Rotate(c), t.rotation, h});
    }
}

// ---- gun scan ----
// The gun's parts report no size, so the gun is measured with rays: a grid
// across its side (gun frame X/Z), one ray from the right and one from the
// left per cell, a few hundred per frame until done. grip_probe.cpp turns
// that into the grip's shape and the hand placement.
struct GunScan {
    UObject gun = 0;
    grip::Grid grid;
    int next = 0;
    bool done = false;
    grip::Shape shape;
    int hits = 0;
    int held_frames = 0;
    int reports = 0;
    std::vector<UObject> parts;  // the parts worth scanning
    bool right_missed = false;
    hands::Pose frame;  // where the gun is this frame (gun frame)
    double started = 0, cpu_ms = 0;
} g_scan;
perf::Counter g_perf_tick{"body tick"};
perf::Counter g_perf_scan{"body gun scan"};
perf::Counter g_perf_fingers{"body fingers"};

// Parts that shape the outside of the grip and frame. Internals (barrel,
// spring) and accessories (light, rail) only cost traces.
bool ScanPart(UObject comp) {
    std::string n = Lower(ue::GetName(comp));
    for (const char* skip : {"barrel", "spring", "light", "rail", "laser", "optic", "sight"})
        if (n.find(skip) != std::string::npos) return false;
    return true;
}
const double kScanYCentre = -4.0, kScanYHalf = 16.0;

// Nearest hit along a->b over the given parts, as a fraction of the way.
bool TraceNearest(const std::vector<UObject>& comps, const FVector& a, const FVector& b, double* t_out) {
    double best = 2.0, len = Length(b - a);
    if (len < 1e-6) return false;
    for (UObject comp : comps) {
        ue::Call c(comp, "K2_LineTraceComponent");
        if (!c.Valid()) return false;
        c.Set("TraceStart", a).Set("TraceEnd", b).Set("bTraceComplex", true);
        if (c.Invoke() && c.Result<bool>()) best = std::min(best, Length(c.Get<FVector>("HitLocation") - a) / len);
    }
    if (best > 1.0) return false;
    *t_out = best;
    return true;
}

void StepScan(const hands::HandGrab& grab, const std::vector<UObject>& comps) {
    if (grab.object != g_scan.gun) {
        g_scan = GunScan{};
        g_scan.gun = grab.object;
        g_scan.grid.Init(-8, 22, -4, 17, 0.5);
        for (UObject c : comps)
            if (ScanPart(c)) g_scan.parts.push_back(c);
        g_scan.started = perf::NowMs();
    }
    g_scan.frame = grab.target;
    if (g_scan.done || g_scan.parts.empty()) return;
    perf::Scope timer(g_perf_scan);
    const double t_start = perf::NowMs();
    grip::Grid& g = g_scan.grid;
    const int total = g.nx * g.nz * 2;
    const FQuat& rot = grab.target.rotation;
    const FVector& org = grab.target.location;
    // Time-boxed: a few milliseconds per frame, however slow traces are.
    for (; g_scan.next < total && perf::NowMs() - t_start < S.scan_ms; ++g_scan.next) {
        int cell = g_scan.next / 2, from_left = g_scan.next % 2;
        // Nothing from the right side: nothing from the left either.
        if (from_left && g_scan.right_missed) continue;
        int ix = cell % g.nx, iz = cell / g.nx;
        double y0 = from_left ? kScanYCentre - kScanYHalf : kScanYCentre + kScanYHalf;
        double y1 = from_left ? kScanYCentre + kScanYHalf : kScanYCentre - kScanYHalf;
        FVector a = org + rot.Rotate(FVector{g.X(ix), y0, g.Z(iz)});
        FVector b = org + rot.Rotate(FVector{g.X(ix), y1, g.Z(iz)});
        double t;
        bool hit = TraceNearest(g_scan.parts, a, b, &t);
        if (!from_left) g_scan.right_missed = !hit;
        if (!hit) continue;
        ++g_scan.hits;
        double y = y0 + (y1 - y0) * t;
        (from_left ? g.yl : g.yr)[static_cast<size_t>(cell)] = y;
    }
    g_scan.cpu_ms += perf::NowMs() - t_start;
    if (g_scan.next < total) return;
    g_scan.done = true;
    g_scan.shape = grip::Analyze(g, S.scan_thumb_above);
    const grip::Shape& sh = g_scan.shape;
    if (!sh.ok) {
        log::Info("body: scanned %s with %d rays (%d hit): could not find the grip (%s), using the old grip placement",
                  ue::GetName(grab.object).c_str(), total, g_scan.hits, sh.why.c_str());
        return;
    }
    log::Info("body: scan took %.0f ms of CPU over %.1f s (%zu parts)", g_scan.cpu_ms,
              (perf::NowMs() - g_scan.started) / 1000.0, g_scan.parts.size());
    log::Info("body: scanned %s with %d rays (%d hit): gun %.1f cm long, %.1f cm tall; grip bottom %.1f, trigger guard "
              "bottom %.1f, front strap %.1f, back strap %.1f, grip %.1f cm wide (right side %.1f, left %.1f), slant %.0f "
              "deg; thumb spot (%.1f %.1f %.1f) (gun frame cm)",
              ue::GetName(grab.object).c_str(), total, g_scan.hits, sh.length, sh.height, sh.z_bottom, sh.z_guard,
              sh.front, sh.back, sh.y_right - sh.y_left, sh.y_right, sh.y_left, sh.slant_deg, sh.thumb_x, sh.thumb_y,
              sh.thumb_z);
}

bool ScanReady(const hands::HandGrab& grab) {
    return g_scan.gun == grab.object && g_scan.done && g_scan.shape.ok;
}
FVector GunLocal(const hands::Pose& frame, const FVector& w) {
    return frame.rotation.Inverse().Rotate(w - frame.location);
}
FVector GunLocal(const hands::HandGrab& grab, const FVector& w) { return GunLocal(grab.target, w); }

// While set, finger contact tests against the scanned gun instead of
// tracing its parts (same answer, far cheaper, and the same model the clip
// report checks).
const hands::Pose* g_grid_frame = nullptr;

// First hit along a->b, as a fraction of the way (0..1). `first_only`: any
// hit will do (cheaper), the fraction is then not the nearest.
bool RayHit(const FVector& a, const FVector& b, double* t_out, bool first_only) {
    double best = 2.0;
    double len = Length(b - a);
    if (len < 1e-6) return false;
    ++g_c->traces;
    if (g_c->boxes_mode) {
        for (const Box& box : g_c->boxes) {
            double t;
            if (SegmentBox(a, b, box, &t) && t < best) {
                best = t;
                if (first_only) break;
            }
        }
    } else {
        for (UObject comp : g_c->comps) {
            ue::Call c(comp, "K2_LineTraceComponent");
            if (!c.Valid()) return false;
            c.Set("TraceStart", a).Set("TraceEnd", b).Set("bTraceComplex", true);
            if (c.Invoke() && c.Result<bool>()) {
                double t = Length(c.Get<FVector>("HitLocation") - a) / len;
                if (t < best) best = t;
                if (first_only) break;
            }
        }
    }
    if (best > 1.0) return false;
    ++g_c->hits;
    if (t_out) *t_out = best;
    return true;
}

bool SegmentHits(const FVector& a, const FVector& b) {
    if (g_grid_frame)
        return grip::Penetration(g_scan.grid, {GunLocal(*g_grid_frame, a), GunLocal(*g_grid_frame, b)}, 0.0) > 0.0;
    return RayHit(a, b, nullptr, true);
}

// Largest angle (0..max) joint `pivot` may turn about `axis` before the
// segment pivot->end (offset by the finger's radius toward the curl side)
// touches the object. Returns max when nothing is in the way, or when the
// segment already starts inside (nothing better to do).
double FreeAngle(const FVector& pivot, const FVector& end, const FVector& axis, double max, double radius) {
    auto blocked = [&](double ang) {
        FQuat q = AxisAngle(axis, ang);
        FVector seg = q.Rotate(end - pivot);
        FVector side = Normalize(Cross(axis, seg)) * radius;
        return SegmentHits(pivot + side, pivot + seg + side);
    };
    if (max <= 0.0 || !blocked(max)) return max;
    if (blocked(0.0)) return max;
    double lo = 0.0, hi = max;
    for (int i = 0; i < 5; ++i) {
        double mid = 0.5 * (lo + hi);
        if (blocked(mid)) hi = mid; else lo = mid;
    }
    return lo;
}

// Where and how to hold a magazine: around its lower body, the hand's
// pinky->index axis up the magazine toward the feed lips, palm on the back
// of the magazine facing its front. Measured from the magazine's own mesh
// (largest part's local bounds), so the hand follows the magazine wherever
// controls carries it. Needs CollectPrimitives(mag) first.
bool MagHold(FVector* fist, FVector* up, FVector* front) {
    UObject best = 0;
    double best_size = 0;
    FVector bmin, bmax;
    for (UObject comp : g_c->comps) {
        FVector mn, mx;
        if (!LocalBox(comp, &mn, &mx)) continue;
        double size = Length(mx - mn);
        if (size > best_size && size < 80.0) {
            best = comp;
            best_size = size;
            bmin = mn;
            bmax = mx;
        }
    }
    if (!best || best_size < 3.0) return false;
    ue::Call c(best, "K2_GetComponentToWorld");
    if (!c.Invoke()) return false;
    Xf t = c.Result<Xf>();
    FVector centre = (bmin + bmax) * 0.5;
    double height = (bmax.Z - bmin.Z) * t.scale.Z;
    FVector u = t.rotation.Rotate(FVector{0, 0, 1});
    FVector world_centre = t.translation + t.rotation.Rotate(FVector{centre.X * t.scale.X, centre.Y * t.scale.Y,
                                                                         centre.Z * t.scale.Z});
    *fist = world_centre - u * (height * S.mag_hold_low);
    *up = u;
    *front = t.rotation.Rotate(FVector{1, 0, 0});
    static UObject logged = 0;
    if (logged != best) {
        logged = best;
        log::Info("body: holding magazine %s by %s (%.1f x %.1f x %.1f cm)", ue::GetName(g_c->actor).c_str(),
                  ue::GetName(best).c_str(), (bmax.X - bmin.X) * t.scale.X, (bmax.Y - bmin.Y) * t.scale.Y, height);
    }
    return true;
}

// Thumb on a pistol (owner's reference photos): from the web of the hand
// around the back of the grip to the gun's LEFT side, then straight forward
// along the frame under the slide. Aimed in the gun frame and checked against
// the scanned gun so it never goes through the grip or over the slide.
void PlaceGunThumb(const hands::HandGrab& grab, const Bone* j, const grip::Placement& place) {
    FVector p0 = GunLocal(grab, BoneW(j[0]).translation);
    FVector p1 = GunLocal(grab, BoneW(j[1]).translation);
    FVector p2 = GunLocal(grab, BoneW(j[2]).translation);
    double l1 = Length(p1 - p0), l2 = Length(p2 - p1), l3 = l2 * 0.8;
    if (l1 < 0.5 || l2 < 0.5) return;
    const FVector fwd = place.thumb_dir;
    FVector target = place.thumb;
    FVector k1, k2, tip;
    for (int it = 0; it < 10; ++it) {
        k1 = p0 + Normalize(target - p0) * l1;
        k2 = k1 + fwd * l2;
        tip = k2 + fwd * l3;
        int at = -1;
        double d = grip::Penetration(g_scan.grid, {p0, k1, k2, tip}, S.finger_radius, &at);
        if (d <= 0.0) break;
        // Through the back of the grip: swing further back; along the side:
        // further out to the left.
        if (at == 0) target.X -= 0.4;
        target.Y -= 0.3;
    }
    const FQuat& r = grab.target.rotation;
    RotateBoneW(j[0], Arc(r.Rotate(p1 - p0), r.Rotate(k1 - p0)));
    FVector n1 = GunLocal(grab, BoneW(j[1]).translation), n2 = GunLocal(grab, BoneW(j[2]).translation);
    RotateBoneW(j[1], Arc(r.Rotate(n2 - n1), r.Rotate(k2 - k1)));
    // Tip joint straight: same orientation as the bone before it.
    Xf t2 = BoneW(j[2]);
    t2.rotation = BoneW(j[1]).rotation;
    SetBoneW(j[2], t2);
}

// How deep any finger or palm point sits inside the scanned gun, logged a
// few times per gun so a playtest log shows whether the hand clips.
void ClipReport(const hands::HandGrab& grab, int side) {
    ++g_scan.held_frames;
    if (g_scan.reports >= 3 || (g_scan.held_frames != 90 && g_scan.held_frames != 900 &&
                                g_scan.held_frames != 3600))
        return;
    ++g_scan.reports;
    const Skeleton& k = G.skel;
    static const char* names[] = {"thumb", "index", "middle", "ring", "pinky"};
    std::string line;
    double worst = 0;
    for (int f = 0; f < hand_pose::kFingerCount; ++f) {
        const Bone* j = k.finger[side][f];
        FVector a = BoneW(j[0]).translation, b = BoneW(j[1]).translation, c = BoneW(j[2]).translation;
        FVector tip = c + (c - b) * 0.8;
        double d = grip::Penetration(g_scan.grid, {GunLocal(grab, a), GunLocal(grab, b), GunLocal(grab, c),
                                                   GunLocal(grab, tip)}, S.finger_radius);
        worst = std::max(worst, d);
        char buf[48];
        std::snprintf(buf, sizeof(buf), " %s %.1f", names[f], d);
        line += buf;
    }
    Xf hand = BoneW(k.hand[side]);
    double palm = 0;
    for (const FVector& lp : grip::PalmPoints(G.geo[side], S.scan_palm_skin)) {
        double d;
        if (g_scan.grid.Inside(GunLocal(grab, hand.translation + hand.rotation.Rotate(lp)), &d))
            palm = std::max(palm, d);
    }
    worst = std::max(worst, palm);
    FVector wrist = GunLocal(grab, hand.translation);
    log::Info("body: clip check on %s after %d frames: deepest %.1f cm inside the gun (fingers:%s, palm %.1f); wrist at "
              "(%.1f %.1f %.1f) gun frame%s",
              ue::GetName(grab.object).c_str(), g_scan.held_frames, worst, line.c_str(), palm, wrist.X, wrist.Y,
              wrist.Z, worst < 0.3 ? " - clean" : "");
}

void SolveArm(int side, const hands::HandState& h, const FVector& body_fwd, const FVector& body_right,
              double dt) {
    const Skeleton& k = G.skel;
    if (!k.arms_ok[side]) return;
    const hands::Hand hs = side == kL ? hands::kLeft : hands::kRight;
    hands::HandGrab grab = hands::GetGrab(hs);
    g_c = &g_contacts[side];

    // Measure the avatar's hand once (finger bases do not move with the pose).
    if (!G.frame[side].valid && k.fingers_ok[side]) {
        Xf hand = BoneW(k.hand[side]);
        G.frame[side] = MeasureHand(hand.translation, hand.rotation,
                                    BoneW(k.finger[side][hand_pose::kIndex][0]).translation,
                                    BoneW(k.finger[side][hand_pose::kMiddle][0]).translation,
                                    BoneW(k.finger[side][hand_pose::kPinky][0]).translation, side == kL,
                                    S.palm_depth);
        if (G.frame[side].valid) {
            Xf hs_now = BoneW(k.hand[side]);
            FVector tip = BoneW(k.finger[side][hand_pose::kMiddle][2]).translation;
            double scale = BoneW(k.hand[side]).scale.X;
            log::Info("body: measured the %s hand (wrist to middle fingertip joint %.1f cm, bone scale %.2f)",
                      side == kL ? "left" : "right", Length(tip - hs_now.translation), scale);
            // Knuckles in hand-bone space, for placing the hand on a scanned gun.
            grip::HandGeo& geo = G.geo[side];
            geo.across = G.frame[side].across;
            geo.palm = G.frame[side].palm;
            FQuat inv = hs_now.rotation.Inverse();
            for (int f = 0; f < hand_pose::kFingerCount; ++f)
                geo.mcp[f] = inv.Rotate(BoneW(k.finger[side][f][0]).translation - hs_now.translation);
            // A right hand has Cross(pinky->index, out of palm) pointing from
            // the wrist to the knuckles; anything else means a bad reading.
            double hand_ok = Dot(Normalize(Cross(geo.across, geo.palm)), Normalize(geo.mcp[hand_pose::kMiddle]));
            G.geo_ok[side] = side == kL ? hand_ok < -0.5 : hand_ok > 0.5;
            log::Info("body: %s hand knuckles (hand space cm): thumb (%.1f %.1f %.1f) index (%.1f %.1f %.1f) middle "
                      "(%.1f %.1f %.1f) pinky (%.1f %.1f %.1f), handedness check %.2f%s",
                      side == kL ? "left" : "right", geo.mcp[0].X, geo.mcp[0].Y, geo.mcp[0].Z, geo.mcp[1].X,
                      geo.mcp[1].Y, geo.mcp[1].Z, geo.mcp[2].X, geo.mcp[2].Y, geo.mcp[2].Z, geo.mcp[4].X,
                      geo.mcp[4].Y, geo.mcp[4].Z, hand_ok, G.geo_ok[side] ? "" : " (FAILED: no scanned grip)");
        }
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
        // The fist closes on the grip's centre, found from the gun's own
        // parts (see GripCentre), not on MainHandPosition.
        LogGunGeometry(grab.object, grab.target);
        fist = grab.target.location + g.Rotate(GripCentre());
    }

    const bool on_mag = (grab.kind == hands::GrabKind::kMagHeld || grab.kind == hands::GrabKind::kMagInsert) &&
                        grab.object;
    if (on_mag && grab.object != g_c->actor) CollectPrimitives(grab.object, false);
    FVector mag_up{0, 0, 1};
    bool have_mag_up = false;
    if (on_mag && grab.object == g_c->actor) {
        FVector mag_fist, mag_front;
        if (MagHold(&mag_fist, &mag_up, &mag_front)) {
            have_mag_up = true;
            fist = mag_fist;
            fwd = mag_up;
            palm = mag_front;
        }
    }

    FQuat hand_rot;
    FVector hand_pos;
    // Point inside the hand that goes on the target. On a gun grip it sits
    // further toward the knuckles and closer to the palm than a controller
    // handle, so the curled fingers land on the grip instead of in it.
    FVector fist_local = G.frame[side].fist;
    if (G.frame[side].valid && (grab.kind == hands::GrabKind::kGunGrip || on_mag))
        fist_local = G.frame[side].middle * std::clamp(S.gun_fist_along, 0.0, 1.2) +
                     G.frame[side].palm * S.gun_palm_depth;
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
    hand_pos = G.frame[side].valid ? fist - hand_rot.Rotate(fist_local) : fist;

    // Pistol grip from the scanned gun: hand placed from the gun's measured
    // grip, trigger guard and sides (grip_probe.h).
    bool scanned = false;
    grip::Placement place;
    if (side == kR && grab.kind == hands::GrabKind::kGunGrip && grab.object && S.grip_scan && G.geo_ok[side]) {
        if (grab.object != g_c->actor) CollectPrimitives(grab.object, true);
        StepScan(grab, g_c->comps);
        if (ScanReady(grab)) {
            grip::Params gp;
            gp.palm_skin = S.scan_palm_skin;
            gp.finger_radius = S.finger_radius + S.scan_clearance;
            gp.knuckle_back = S.scan_knuckle_back;
            gp.clearance = S.scan_clearance;
            place = grip::PlaceRightHand(g_scan.grid, g_scan.shape, G.geo[side], gp);
            hand_rot = Normalized(grab.target.rotation * place.rot);
            hand_pos = grab.target.location + grab.target.rotation.Rotate(place.pos);
            scanned = true;
            if (g_scan.held_frames == 0) {
                FVector mid = place.pos + place.rot.Rotate(G.geo[side].mcp[hand_pose::kMiddle]);
                log::Info("body: right hand on the scanned grip: wrist (%.1f %.1f %.1f), middle knuckle (%.1f %.1f "
                          "%.1f), palm moved out %.1f cm to clear the grip (gun frame cm)",
                          place.pos.X, place.pos.Y, place.pos.Z, mid.X, mid.Y, mid.Z, place.pushed);
            }
        }
    }

    // Seat the palm on the held object's surface: look from outside the palm
    // toward the grip centre, and put the palm's skin where the surface is.
    // Without it a big hand ends up with the grip inside its palm.
    if (S.palm_seat && !scanned && G.frame[side].valid && grab.object &&
        (grab.kind == hands::GrabKind::kGunGrip || on_mag)) {
        if (grab.object != g_c->actor) CollectPrimitives(grab.object, !on_mag);
        RefreshBoxes();
        FVector palm_w = hand_rot.Rotate(G.frame[side].palm);
        FVector from = fist - palm_w * 10.0;
        double t;
        FVector want{0, 0, 0};
        if (RayHit(from, fist, &t, false)) {
            FVector surface = from + (fist - from) * t;
            FVector skin = G.frame[side].middle * S.palm_along + G.frame[side].palm * S.palm_skin;
            FVector moved = surface - hand_rot.Rotate(skin);
            want = hand_rot.Inverse().Rotate(moved - hand_pos);
            if (Length(want) > 8.0) want = want * (8.0 / Length(want));
        }
        double a = 1.0 - std::exp(-dt / 0.08);
        G.seat_off[side] = G.seat_off[side] + (want - G.seat_off[side]) * a;
        hand_pos = hand_pos + hand_rot.Rotate(G.seat_off[side]);
    } else {
        G.seat_off[side] = FVector{0, 0, 0};
    }
    const bool game_grip = side == kR && grab.kind == hands::GrabKind::kGunGrip && G.use_game_grip;
    if (game_grip) {
        hand_rot = Normalized(grab.target.rotation * G.game_grip_rot);
        hand_pos = grab.target.location + grab.target.rotation.Rotate(G.game_grip_loc);
    }

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

    // Fingers. On the gun with the game's grip copied, keep the game's own
    // finger pose (it was made for this gun).
    if (game_grip) return;
    if (!S.hand_poses || !k.fingers_ok[side] || !G.frame[side].valid) return;
    hand_pose::Curl target = hand_pose::Target(grab.kind, hands::GetFingers(hs));
    hand_pose::Smooth(&G.curl[side], target, dt);
    FVector palm_w = hand_rot.Rotate(G.frame[side].palm);
    FVector pinky_base = BoneW(k.finger[side][hand_pose::kPinky][0]).translation;
    // Fingers wrap until they touch what the hand holds.
    perf::Scope finger_timer(g_perf_fingers);
    // On the gun, fingers test the scanned shape (no traces: tracing every
    // finger joint against every part cost ~25 ms a hand per frame). Until
    // the scan is done they keep their full pose. Magazines use boxes.
    const bool on_gun = grab.kind == hands::GrabKind::kGunGrip || grab.kind == hands::GrabKind::kSlideGrab;
    const bool gun_grid = on_gun && grab.object && grab.object == g_scan.gun && g_scan.done && g_scan.shape.ok;
    const bool touch = S.finger_contact && grab.object &&
                       (gun_grid || grab.kind == hands::GrabKind::kMagHeld || grab.kind == hands::GrabKind::kMagInsert);
    if (touch && !on_gun && grab.object != g_c->actor) CollectPrimitives(grab.object, false);
    if (gun_grid) g_grid_frame = &g_scan.frame;
    if (!G.contact_init[side] || !touch) {
        for (auto& f : G.contact[side])
            for (double& v : f) v = 1.0;
        G.contact_init[side] = true;
    }
    const double follow = 1.0 - std::exp(-dt / 0.05);
    if (touch && !on_gun) RefreshBoxes();
    const bool index_straight = grab.kind == hands::GrabKind::kGunGrip && G.curl[side].finger[hand_pose::kIndex] < 0.15f;
    const FVector barrel_w = grab.target.rotation.Rotate(FVector{1, 0, 0});
    if (scanned) {
        PlaceGunThumb(grab, k.finger[side][hand_pose::kThumb], place);
    }
    for (int f = 0; f < hand_pose::kFingerCount; ++f) {
        if (scanned && f == hand_pose::kThumb) continue;
        const Bone* j = k.finger[side][f];
        FVector a = BoneW(j[0]).translation, b = BoneW(j[1]).translation;
        FVector toward = palm_w;
        if (f == hand_pose::kThumb) toward = Normalize(Normalize(pinky_base - a) + palm_w * 0.5);
        FVector axis = Cross(b - a, toward);
        if (Length(axis) < 1e-4) continue;
        axis = Normalize(axis);
        hand_pose::JointLimits lim = hand_pose::Limits(static_cast<hand_pose::Finger>(f));
        double c = std::min(1.2, G.curl[side].finger[f] * S.finger_scale);
        const double deg[3] = {lim.base, lim.middle, lim.tip};
        for (int n = 0; n < 3; ++n) {
            double want = c * deg[n] * kDegToRad;
            // Trigger discipline: off the trigger the index finger lies along
            // the frame, pointing where the barrel points.
            // Thumb on a pistol grip (reference photos): straight, high on the
            // weak side of the frame, pointing forward like the barrel.
            // Magazine (owner's reference photos): base in the palm, index
            // finger straight up the front, thumb up the side, both pointing
            // toward the feed lips.
            const bool mag_point = have_mag_up && (f == hand_pose::kIndex || f == hand_pose::kThumb);
            const bool gun_thumb = grab.kind == hands::GrabKind::kGunGrip && f == hand_pose::kThumb;
            if (mag_point || gun_thumb) {
                if (n == 0) {
                    FVector dir = BoneW(j[1]).translation - BoneW(j[0]).translation;
                    double lim = f == hand_pose::kIndex ? S.mag_index_aim_max : S.thumb_aim_max;
                    RotateBoneW(j[0], ClampAngle(Arc(dir, mag_point ? mag_up : barrel_w), lim * kDegToRad));
                    G.contact[side][f][0] = 1.0;
                    continue;
                }
                want = 6.0 * kDegToRad;
            }
            if (index_straight && f == hand_pose::kIndex) {
                if (n == 0) {
                    FVector dir = Normalize(b - a);
                    FVector tgt = Reject(barrel_w, axis);
                    double ang = Length(tgt) > 1e-3
                                     ? std::atan2(Dot(Cross(dir, Normalize(tgt)), axis), Dot(dir, Normalize(tgt)))
                                     : 0.0;
                    want = std::clamp(ang, -15.0 * kDegToRad, 45.0 * kDegToRad);
                } else {
                    want = 4.0 * kDegToRad;
                }
            }
            if (touch && (gun_grid || !g_c->comps.empty()) && want > 1e-3) {
                // Joint n turns the segment from bone n to bone n+1 (the tip
                // segment is extended from the last bone's direction).
                FVector p = BoneW(j[n]).translation;
                FVector e;
                if (n < 2) {
                    e = BoneW(j[n + 1]).translation;
                } else {
                    FVector prev = BoneW(j[1]).translation;
                    e = p + (p - prev) * 0.8;
                }
                double free = FreeAngle(p, e, axis, want, S.finger_radius) / want;
                double& sm = G.contact[side][f][n];
                sm += (free - sm) * follow;
            } else {
                G.contact[side][f][n] = 1.0;
            }
            RotateBoneW(j[n], AxisAngle(axis, want * G.contact[side][f][n]));
        }
    }
    g_grid_frame = nullptr;
    if (scanned) ClipReport(grab, side);
    if (touch && !on_gun && !g_c->reported && g_c->traces > 2000) {
        g_c->reported = true;
        log::Info("body: finger contact on %s: %d of %d traces touched%s", ue::GetName(g_c->actor).c_str(),
                  g_c->hits, g_c->traces,
                  g_c->hits ? "" : " (no collision on it: fingers use their full pose)");
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
    static bool registered = [] {
        perf::Register(&g_perf_tick);
        perf::Register(&g_perf_scan);
        perf::Register(&g_perf_fingers);
        return true;
    }();
    (void)registered;
    perf::Scope tick_timer(g_perf_tick);
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
    LearnGameGrip(hands::GetGrab(hands::kRight));

    // Fingers start from the skeleton's straight reference pose; hand poses
    // curl them from there.
    if (S.hand_poses)
        for (int side = 0; side < 2; ++side)
            if (hs[side].valid && k.fingers_ok[side] && !(side == kR && G.use_game_grip))
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
