#include "vr/body/body.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "core/perf.h"
#include "game/game.h"
#include "ue/call.h"
#include "ue/names.h"
#include "ue/process_event.h"
#include "ue/reflection.h"
#include "vr/body/finger_solve.h"
#include "vr/body/grip_editor.h"
#include "vr/body/grip_record.h"
#include "vr/body/finger_rest.h"
#include "vr/body/finger_contact.h"
#include "vr/body/finger_self.h"
#include "vr/body/finger_world.h"
#include "vr/body/thumb_pose.h"
#include "vr/body/hand_walls.h"
#include "vr/body/hand_pose.h"
#include "vr/body/holster_pose.h"
#include "vr/body/ik_math.h"
#include "vr/body/joint_limits.h"
#include "vr/body/arm_pose.h"
#include "vr/body/arm_posture.h"
#include "vr/body/body_calibration.h"
#include "vr/body/calibration_guidance.h"
#include "vr/body/calibration_hold.h"
#include "vr/body/leg_pose.h"
#include "vr/body/leg_world.h"
#include "vr/body/reload_anim.h"
#include "vr/body/torso_pose.h"
#include "vr/body/skeleton.h"
#include "vr/interact/grip_points.h"
#include "vr/body/slide_pose.h"
#include "vr/hands.h"
#include "vr/controls.h"
#include "vr/input.h"
#include "vr/openxr.h"
#include "vr/stereo.h"
#include "vr/interact/holster.h"
#include "vr/ui_panel.h"
#include "vr/drinks.h"
#include "vr/workbench.h"

namespace holevr::vr::body {
namespace {

using ue::FName;
using ue::UObject;
using namespace ik;
using skel::Xf;
namespace gr = grip_record;
namespace jl = joint_limits;

constexpr uint8_t kComponentSpace = 1;  // EBoneSpaces::ComponentSpace
constexpr uint8_t kSnapToTarget = 2;    // EAttachmentRule::SnapToTarget

// ---- settings ----
struct Settings {
    bool enabled = true;
    double turn_threshold = 25.0;  // degrees the head may turn before the body follows
    double turn_toward_hands = 0.3;
    double max_crouch = 70.0;
    double max_lean = 30.0;
    double max_spine_bend = 45.0;
    bool animated_upper_body = false;
    bool legs = true;
    bool hide_head = true;
    int shadow = 1;
    bool hand_poses = true;
    double finger_scale = 1.0;
    double hand_offset[3] = {0, 0, 0};
    double hand_rot[3] = {0, 0, 0};
    double palm_depth = 3.0;
    double arm_scale_override = 0.0;
    double max_stretch = 1.04;
    double max_stretch_unmeasured = 1.15;
    double shoulder_below = 0.0;   // 0 = calibrated shoulder offset or stable fallback
    double shoulder_behind = 0.0;
    double forearm_roll_share = 0.5;   // the wrist display's share of the hand's turn
    // Skin: share of the hand's turn at the elbow end of the forearm and at
    // its middle twist joint (owner 2026-11-07: it looked like all the twist
    // was in the elbow). A real forearm turns from nothing at the elbow to
    // all of it at the wrist.
    double twist_at_elbow = 0.0;
    double twist_at_middle = 0.5;
    bool arm_joint_limits = true;      // human shoulder, forearm and wrist ranges (arm_posture.h)
    double shoulder_twist_share = 0.15;
    bool elbow_relieves_wrist = true;
    bool grip_records = true;
    bool joint_limits = true;      // posed finger joints keep to human ranges (joint_limits.h)
    double finger_radius = 0.5;
    bool editor_markers = true;
    double joint_ball = 2.6;       // joint grab ball diameter, cm
    double reach_gun = 22.0;       // pointer ahead of a gun-holding editing hand, cm
    bool gun_wrist_straight = true;
    bool fit_log = true;
    bool fingers_rest = true;  // fingers give way against walls an empty hand is against
    bool thumb_opposition = true;
    bool press_flat = true;    // an empty hand pushed against a wall settles flat on it
    double press_push = 4.0;   // cm the real hand goes on past where it touched until the hand is flat
    double finger_drag = 1.0;  // fingertips trail along a surface the hand slides on (0 = off, 1 = normal)
    // A magazine lined up with the magwell (owner 2026-11-06): its own hand
    // pose (stage 2), the game's reload hold until it is posed, and the
    // game's own left-hand reload animation played by how far it is in.
    // Pistol magazines play the game's pistol reload; AR magazines only take
    // the two poses (owner 2026-11-06 23:29 UTC: "reuse the games pistol
    // reload for the pistol, not the AR. For the AR i want the multi-pose").
    bool mag_insert_anim = true;
    int mag_insert_ar_clip = -1;  // for AR magazines: -1 (poses only), kPistol or kRifle
    bool mag_insert_game_hold = true;
} S;

void LoadSettings() {
    S.enabled = config::GetBool("body_ik", true);
    S.turn_threshold = config::GetFloat("body_turn_threshold_deg", 25.f);
    S.turn_toward_hands = std::clamp<double>(config::GetFloat("body_turn_toward_hands", 0.3f), 0.0, 1.0);
    S.max_crouch = config::GetFloat("body_max_crouch_cm", 70.f);
    S.max_lean = config::GetFloat("body_max_lean_cm", 30.f);
    S.max_spine_bend = config::GetFloat("body_max_spine_bend", 45.f);
    S.animated_upper_body = config::GetBool("body_animated_upper_body", false);
    S.legs = config::GetBool("body_leg_ik", true);
    S.hide_head = config::GetBool("body_hide_head", true);
    S.shadow = config::GetInt("body_shadow_source", 2);
    S.hand_poses = config::GetBool("hand_poses", true);
    S.finger_scale = config::GetFloat("finger_curl_scale", 1.f);
    S.hand_offset[0] = config::GetFloat("hand_offset_forward_cm", 0.f);
    S.hand_offset[1] = config::GetFloat("hand_offset_right_cm", 0.f);
    S.hand_offset[2] = config::GetFloat("hand_offset_up_cm", 0.f);
    S.hand_rot[0] = config::GetFloat("hand_pitch_offset", 0.f);
    S.hand_rot[1] = config::GetFloat("hand_yaw_offset", 0.f);
    S.hand_rot[2] = config::GetFloat("hand_roll_offset", 0.f);
    S.palm_depth = config::GetFloat("hand_palm_depth_cm", 3.f);
    S.arm_scale_override = config::GetFloat("body_arm_scale_override", 0.f);
    S.max_stretch = std::clamp<double>(config::GetFloat("body_max_arm_stretch", 1.04f), 1.0, 1.5);
    S.max_stretch_unmeasured =
        std::clamp<double>(config::GetFloat("body_max_arm_stretch_unmeasured", 1.15f), 1.0, 1.5);
    S.shoulder_below = config::GetFloat("body_shoulder_below_eyes_cm", 0.f);
    S.shoulder_behind = config::GetFloat("body_shoulder_behind_eyes_cm", 0.f);
    S.forearm_roll_share = std::clamp<double>(config::GetFloat("body_forearm_roll_share", 0.5f), 0.0, 1.0);
    // body_forearm_twist_bone_share (old: 1.0 on the middle twist joint) is
    // replaced by body_forearm_twist_at_elbow / _at_middle.
    S.twist_at_elbow = std::clamp<double>(config::GetFloat("body_forearm_twist_at_elbow", 0.0f), 0.0, 1.0);
    S.twist_at_middle = std::clamp<double>(config::GetFloat("body_forearm_twist_at_middle", 0.5f), 0.0, 1.0);
    S.arm_joint_limits = config::GetBool("body_arm_joint_limits", true);
    S.shoulder_twist_share = std::clamp<double>(config::GetFloat("body_shoulder_twist_share", 0.15f), 0.0, 0.9);
    S.elbow_relieves_wrist = config::GetBool("body_elbow_relieves_wrist", true);
    S.grip_records = config::GetBool("hand_grip_records", true);
    S.joint_limits = config::GetBool("hand_joint_limits", true);
    S.finger_radius = config::GetFloat("hand_finger_radius_cm", 0.5f);
    S.finger_radius = std::isfinite(S.finger_radius) ? std::clamp(S.finger_radius, 0.1, 2.0) : 0.5;
    S.editor_markers = config::GetBool("hand_editor_show_grab_points", true);
    S.joint_ball = std::clamp<double>(config::GetFloat("hand_editor_joint_ball_cm", 2.6f), 0.5, 6.0);
    S.reach_gun = std::clamp<double>(config::GetFloat("hand_editor_gun_hand_reach_cm", 22.f), 3.0, 60.0);
    S.gun_wrist_straight = config::GetBool("body_gun_wrist_straight", true);
    S.fit_log = config::GetBool("body_fit_log", true);
    S.fingers_rest = config::GetBool("hand_fingers_rest_on_walls", true);
    S.thumb_opposition = config::GetBool("hand_thumb_opposition", true);
    S.press_flat = config::GetBool("hand_press_flat_on_walls", true);
    S.press_push = std::clamp<double>(config::GetFloat("hand_press_flat_push_cm", 4.f), 0.5, 20.0);
    S.finger_drag = std::clamp<double>(config::GetFloat("hand_finger_drag", 1.f), 0.0, 3.0);
    S.mag_insert_anim = config::GetBool("mag_insert_anim", true);
    {
        // A new key: mag_insert_anim_rifle (pistol by default) was the first
        // reading of the owner's ask, and ini files keep what they were
        // given.
        std::string r = config::GetString("mag_insert_anim_ar", "off");
        for (char& ch : r) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        S.mag_insert_ar_clip = r == "rifle" || r == "ar" ? reload_anim::kRifle
                               : r == "pistol" || r == "hg" ? reload_anim::kPistol
                                                            : -1;
    }
    S.mag_insert_game_hold = config::GetBool("mag_insert_game_hold", true);
}

// ---- small files beside the ini ----
std::wstring ModPath(const std::wstring& rel) { return config::Dir() + L"\\" + rel; }
std::wstring Widen(const std::string& s) {
    std::wstring w;
    for (char c : s) w += static_cast<wchar_t>(static_cast<unsigned char>(c));
    return w;
}
bool ReadText(const std::wstring& path, std::string* out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    out->clear();
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
    fclose(f);
    return true;
}
bool WriteText(const std::wstring& path, const std::string& text) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    return fclose(f) == 0 && ok;
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

Xf ComponentToWorld(UObject comp) {
    ue::Call c(comp, "K2_GetComponentToWorld");
    c.Invoke();
    return c.Result<Xf>();
}

// ---- bones ----
enum Side : int { kL = 0, kR = 1 };
constexpr int kNoBone = -1;

struct Skeleton {
    int pelvis = kNoBone, neck = kNoBone, head = kNoBone;
    std::vector<int> spine;  // bottom to top
    int clavicle[2] = {kNoBone, kNoBone}, upperarm[2] = {kNoBone, kNoBone}, lowerarm[2] = {kNoBone, kNoBone},
        hand[2] = {kNoBone, kNoBone}, forearm_twist[2] = {kNoBone, kNoBone}, upperarm_twist[2] = {kNoBone, kNoBone};
    int thigh[2] = {kNoBone, kNoBone}, calf[2] = {kNoBone, kNoBone}, foot[2] = {kNoBone, kNoBone};
    int toe[2] = {kNoBone, kNoBone};
    int finger[2][hand_pose::kFingerCount][3];
    bool arms_ok[2] = {false, false};
    bool legs_ok[2] = {false, false};
    bool fingers_ok[2] = {false, false};
};

int Pick(const std::map<std::string, int>& bones, std::initializer_list<std::string> candidates) {
    for (const std::string& c : candidates) {
        auto it = bones.find(c);
        if (it != bones.end()) return it->second;
    }
    return kNoBone;
}

Skeleton MapSkeleton(const std::vector<std::string>& names) {
    Skeleton k;
    std::map<std::string, int> bones;
    for (size_t i = 0; i < names.size(); ++i) bones[Lower(names[i])] = static_cast<int>(i);
    k.pelvis = Pick(bones, {"pelvis", "hips", "hip", "root_pelvis"});
    k.neck = Pick(bones, {"neck_01", "neck", "neck1", "neck_1"});
    k.head = Pick(bones, {"head"});
    for (const char* s : {"spine_01", "spine_02", "spine_03", "spine_04", "spine_05"}) {
        int b = Pick(bones, {s});
        if (b != kNoBone) k.spine.push_back(b);
    }
    if (k.spine.empty())
        for (const char* s : {"spine", "spine1", "spine2", "spine3"}) {
            int b = Pick(bones, {s});
            if (b != kNoBone) k.spine.push_back(b);
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
        k.toe[side] = Pick(bones, {"ball_" + s, "toe_" + s, full + "toebase"});
        bool all_fingers = true;
        for (int f = 0; f < hand_pose::kFingerCount; ++f)
            for (int j = 0; j < 3; ++j) {
                std::string n = kFingers[f];
                std::string idx = std::to_string(j + 1);
                k.finger[side][f][j] = Pick(bones, {n + "_0" + idx + "_" + s, full + "hand" + n + idx,
                                                    n + idx + "_" + s, n + "_" + idx + "_" + s});
                all_fingers &= k.finger[side][f][j] != kNoBone;
            }
        k.arms_ok[side] = k.upperarm[side] != kNoBone && k.lowerarm[side] != kNoBone && k.hand[side] != kNoBone;
        k.legs_ok[side] = k.thigh[side] != kNoBone && k.calf[side] != kNoBone && k.foot[side] != kNoBone;
        k.fingers_ok[side] = all_fingers;
    }
    log::Info("body: skeleton pelvis=%d spine=%zu neck=%d head=%d arms L%d R%d legs L%d R%d fingers L%d R%d",
              k.pelvis >= 0, k.spine.size(), k.neck >= 0, k.head >= 0, k.arms_ok[0], k.arms_ok[1], k.legs_ok[0],
              k.legs_ok[1], k.fingers_ok[0], k.fingers_ok[1]);
    return k;
}

// ---- the pose inside the engine ----
// The PoseableMeshComponent keeps its pose as an array of parent-relative
// bone transforms (BoneSpaceTransforms, not exposed to reflection). We find
// that array once, prove it is the right one, and then read and write it
// directly: one copy each way per frame. Every Get/SetBoneTransformByName
// call rebuilt the whole skeleton (~40 us each, ~300 a frame = 13 ms).
// If the array can't be found, the slow per-bone calls are used instead.
struct TArrayRaw {
    uintptr_t data;
    int32_t num;
    int32_t max;
};

class EnginePose {
public:
    UObject comp = 0;
    std::vector<FName> names;
    std::vector<std::string> name_str;
    std::vector<int> parent;
    int32_t local_off = -1;

    bool Init(UObject c) {
        comp = c;
        names.clear();
        name_str.clear();
        parent.clear();
        local_off = -1;
        ue::Call n(comp, "GetNumBones");
        if (!n.Invoke()) return false;
        int count = n.Result<int32_t>();
        if (count <= 0 || count > 1024) return false;
        std::map<std::string, int> index;
        std::string list;
        for (int i = 0; i < count; ++i) {
            ue::Call b(comp, "GetBoneName");
            b.Set("BoneIndex", static_cast<int32_t>(i));
            if (!b.Invoke()) return false;
            names.push_back(b.Result<FName>());
            name_str.push_back(ue::names::ToString(names.back()));
            index[name_str.back()] = i;
            list += name_str.back();
            list += i + 1 < count ? ", " : "";
        }
        log::Info("body: %d bones: %s", count, list.c_str());
        for (int i = 0; i < count; ++i) {
            ue::Call p(comp, "GetParentBone");
            p.Set("BoneName", names[i]);
            int pi = -1;
            if (p.Invoke()) {
                auto it = index.find(ue::names::ToString(p.Result<FName>()));
                if (it != index.end()) pi = it->second;
            }
            if (pi >= i || (i > 0 && pi < 0)) {
                log::Error("body: bone %s has parent index %d, skeleton order not usable", name_str[i].c_str(), pi);
                return false;
            }
            parent.push_back(pi);
        }
        return true;
    }

    // Looks for the bone array; call after the component has a pose. Gives
    // up (slow path) after a few tries.
    void TryLocate() {
        if (Fast() || locate_tries >= 10) return;
        ++locate_tries;
        Locate(locate_tries == 10);
    }
    int locate_tries = 0;

    bool Fast() const { return local_off >= 0; }
    int Count() const { return static_cast<int>(names.size()); }

    bool Read(std::vector<Xf>* local) {
        const int n = Count();
        local->resize(n);
        if (Fast()) {
            TArrayRaw arr{};
            if (!mem::Read(comp + local_off, arr) || arr.num != n || !mem::IsReadable(arr.data, n * sizeof(Xf)))
                return false;
            std::memcpy(local->data(), reinterpret_cast<const void*>(arr.data), n * sizeof(Xf));
            return true;
        }
        std::vector<Xf> cs(n);
        for (int i = 0; i < n; ++i) {
            cs[i] = ApiCS(i);
            (*local)[i] = parent[i] < 0 ? cs[i] : skel::Relative(cs[i], cs[parent[i]]);
        }
        return true;
    }

    // Writes every bone whose local transform changed this frame.
    int Write(const skel::Pose& p) {
        const int n = Count();
        int written = 0;
        if (Fast()) {
            TArrayRaw arr{};
            if (!mem::Read(comp + local_off, arr) || arr.num != n || !mem::IsWritable(arr.data, n * sizeof(Xf)))
                return 0;
            Xf* dst = reinterpret_cast<Xf*>(arr.data);
            for (int i = 0; i < n; ++i)
                if (p.Changed(i)) {
                    std::memcpy(&dst[i], &p.L(i), sizeof(Xf));
                    ++written;
                }
            // One engine call so it sends the new pose to the renderer (it
            // rebuilds the bones from the array we just wrote).
            SetCS(0, p.C(0));
            return written;
        }
        for (int i = 0; i < n; ++i)
            if (p.Changed(i)) {
                SetCS(i, p.C(i));
                ++written;
            }
        return written;
    }

    void ResetAll() {
        for (const FName& name : names) {
            ue::Call c(comp, "ResetBoneTransformByName");
            c.Set("BoneName", name);
            c.Invoke();
        }
    }

private:
    Xf ApiCS(int i) {
        ue::Call c(comp, "GetBoneTransformByName");
        c.Set("BoneName", names[i]).Set("BoneSpace", kComponentSpace);
        c.Invoke();
        return c.Result<Xf>();
    }
    void SetCS(int i, const Xf& t) {
        ue::Call c(comp, "SetBoneTransformByName");
        c.Set("BoneName", names[i]).Set("InTransform", t).Set("BoneSpace", kComponentSpace);
        c.Invoke();
    }

    static bool SameXf(const Xf& a, const Xf& b) {
        double dot = std::fabs(a.rotation.X * b.rotation.X + a.rotation.Y * b.rotation.Y +
                               a.rotation.Z * b.rotation.Z + a.rotation.W * b.rotation.W);
        return Length(a.translation - b.translation) < 0.05 && dot > 0.9999;
    }

    void Locate(bool last_try) {
        const int n = Count();
        // Bones deep in the tree make the test strict.
        std::vector<int> probe = {n - 1, n / 2, n / 3};
        for (int i = n - 1; i >= 0 && probe.size() < 6; --i)
            if (parent[i] >= 0 && parent[parent[i]] >= 0) probe.push_back(i);
        std::vector<Xf> want;
        for (int i : probe) want.push_back(ApiCS(i));
        UObject cls = ue::GetClass(comp);
        int32_t size = cls ? ue::reflection::GetPropertiesSize(cls) : 0;
        if (size < 0x200 || size > 0x4000) size = 0x2000;
        std::vector<Xf> local(n);
        skel::Pose pose;
        if (!pose.Init(parent)) return;
        for (int32_t off = 0x100; off + 16 <= size; off += 8) {
            TArrayRaw arr{};
            if (!mem::Read(comp + off, arr) || arr.num != n || arr.max < n || arr.max > n * 4 ||
                !mem::LooksLikePointer(arr.data) || !mem::IsReadable(arr.data, n * sizeof(Xf)))
                continue;
            std::memcpy(local.data(), reinterpret_cast<const void*>(arr.data), n * sizeof(Xf));
            bool sane = true;
            for (const Xf& x : local) {
                double q = x.rotation.X * x.rotation.X + x.rotation.Y * x.rotation.Y + x.rotation.Z * x.rotation.Z +
                           x.rotation.W * x.rotation.W;
                if (!(std::fabs(q - 1.0) < 1e-3) || !std::isfinite(x.translation.X)) sane = false;
            }
            if (!sane) continue;
            pose.SetAllLocal(local.data(), n);
            bool match = true;
            for (size_t k = 0; k < probe.size() && match; ++k) match = SameXf(pose.C(probe[k]), want[k]);
            if (!match) continue;
            // Prove the engine reads this array: nudge one bone and ask it.
            const int t = probe[0];
            Xf* entry = reinterpret_cast<Xf*>(arr.data) + t;
            Xf saved = *entry;
            if (!mem::IsWritable(reinterpret_cast<uintptr_t>(entry), sizeof(Xf))) continue;
            entry->translation.Z += 3.0;
            Xf moved = ApiCS(t);
            *entry = saved;
            if (std::fabs((moved.translation - want[0].translation).Z) + std::fabs((moved.translation - want[0].translation).X) +
                    std::fabs((moved.translation - want[0].translation).Y) < 0.5) {
                log::Info("body: pose array at +0x%x matches but the engine does not read it", off);
                continue;
            }
            local_off = off;
            log::Info("body: fast pose path on (bone array at +0x%x, %d bones)", off, n);
            return;
        }
        if (last_try)
            log::Error("body: bone array not found, using the slow per-bone engine calls (expect several ms per frame)");
    }
};

// ---- state ----
struct HandRig {
    bool ok = false;
    fingers::Chain chain[hand_pose::kFingerCount];
    gr::HandGeo geo;
    HandFrame frame;      // for empty hands on the controller
    FVector palm_centre;  // hand space
    FVector forearm_dir;  // reference: elbow -> wrist, in hand space
    FVector palm_h, across_h;
    // Human joint ranges for posed fingers, per finger and joint (joint_limits.h).
    jl::Axes axes[hand_pose::kFingerCount][3];
    jl::Limits limits[hand_pose::kFingerCount][3];
};

// The grip a hand is using right now.
struct ActiveGrip {
    std::string key;            // file name without extension
    UObject object = 0;
    hands::GrabKind kind = hands::GrabKind::kNone;
    int point = -1;             // support grip: which grip point (hands::HandGrab::point)
    bool have = false;          // a record is in use
    bool pending = false;       // record found, its solve is running off the game thread
    bool from_file = false;
    gr::Record rec;
    FQuat hand_rot;             // hand bone in the object frame
    FVector hand_pos;
    fingers::Angles tip_angles[gr::kTipCount];
    holster_pose::Grip approach;
    bool approach_ready = false;
    double err[gr::kTipCount] = {};
    double clip[gr::kTipCount] = {};
    double palm_clip = 0;
    double web_clip = 0;  // web between thumb and index (meets the tang)
};

struct State {
    UObject player = 0;
    UObject source = 0;  // the game's BodyMesh
    UObject pose_comp = 0;
    UObject anim_src = 0;     // where the animation is copied from (BodyMesh or its leader)
    int mesh_hidden_shadow = -1;  // character Mesh's own hidden-shadow flag, to restore
    EnginePose engine;
    skel::Pose pose;
    std::vector<Xf> ref;    // reference (rest) pose, local
    std::vector<Xf> frame_local;
    std::vector<double> spine_source_z; // reused snapshots; no per-frame torso allocation
    Skeleton skel;
    HandRig rig[2];
    ActiveGrip grip[2];
    fingers::Angles cur[2][hand_pose::kFingerCount];
    bool cur_init[2] = {false, false};
    double index_touch[2] = {0, 0};
    double index_pull[2] = {0, 0};
    std::vector<UObject> followers;
    bool active = false;
    bool failed = false;
    bool ref_pending = true;
    double body_yaw = 0;
    arm_pose::Reference arm_ref[2];
    watch::Forearm watch_arm[2];
    double watch_arm_ms[2] = {};
    arm_pose::Attachment upper_twist_ref[2], forearm_twist_ref[2];
    arm_posture::State posture_state[2];
    bool arm_tracked[2] = {false, false};
    arm_pose::Reference leg_ref[2];
    double clav_amount[2] = {0, 0};
    legs::State legs;
    leg_world::ProbeState leg_probe;
    double sole_offset[2] = {};
    FVector sole_up_local[2];
    FVector stance_from_pelvis[2];
    FVector leg_upper_cs[2], leg_lower_cs[2];
    double reference_pelvis_height = 0;
    FQuat stance_rotation[2];
    FQuat toe_reference[2];
    std::uint64_t origin_generation = 0;
    double last_tick_ms = 0;
    FVector last_actor;
    bool have_actor = false;
    int steps_taken = 0;
    arm_pose::TwistState twist_state[2];
    double rest_turn_logged[2] = {1e9, 1e9};
    double arm_sample_ms[2] = {}; // signed pose snapshots, at most one per five seconds/hand
    double body_dz = 0;  // extra tracked crouch; fixed/seated preserve the game's pelvis
    double torso_spine_scale = 1, torso_shoulder_residual = 0;
    int frames = 0;
    // fit log
    double fit_time = 0;
    double fit_reach[2] = {0, 0}, fit_gap[2] = {0, 0}, fit_flex[2] = {0, 0}, fit_dev[2] = {0, 0};
    double fit_swivel[2] = {0, 0}, fit_twist[2] = {0, 0};
    double fit_controller_cm[2] = {}, fit_controller_deg[2] = {};
    unsigned fit_leg_frames = 0, fit_leg_bypass = 0, fit_leg_recovery[2] = {};
    int movement_known = -1;
    int fit_n[2] = {0, 0};
    double fit_yaw_lag = 0;
    int fit_yaw_n = 0;
    double max_gap[2] = {0, 0};
} G;

// Arm scale from the one-time measurement (hole-vr/body-measure.ini).
struct Measure {
    bool loaded = false;
    bool have = false;
    double arm_scale = 1.0;
    bool calibrated = false;
    body_calibration::Profile profile;
    double standing_eye_cm = 165.0;
} M;

body_calibration::HeightMode CurrentHeightMode() {
    if (!config::GetBool("fixed_height", true)) return body_calibration::HeightMode::Real;
    return config::GetString("height_mode", "fixed") == "seated"
        ? body_calibration::HeightMode::Seated : body_calibration::HeightMode::Fixed;
}

void LoadMeasure() {
    M.loaded = true;
    double configured_standing = config::GetFloat("standing_height_cm", 0.f);
    if (!(configured_standing >= 80 && configured_standing <= 230)) configured_standing = 0;
    M.standing_eye_cm = configured_standing > 0 ? configured_standing : 165;
    std::string text;
    if (ReadText(ModPath(L"body-calibration.ini"), &text)) {
        if (body_calibration::Parse(text, &M.profile)) {
            M.calibrated = M.profile.arms_measured;
            // Older profiles labeled every floor-relative measurement as
            // standing, even in seated mode. Do not re-import that same value
            // from the ini after the parser has quarantined its provenance.
            if (M.profile.legacy_eye_cm > 0 && M.profile.standing_eye_cm <= 0 &&
                std::fabs(configured_standing - M.profile.legacy_eye_cm) < 0.5) {
                config::Set("standing_height_cm", "0");
                config::NotifyChanged();
                stereo::SetStandingEyeHeight(0);
                configured_standing = 0;
                M.standing_eye_cm = 165;
                log::Info("body: old %.1f cm eye measurement has unknown posture; not using it as standing height", M.profile.legacy_eye_cm);
            }
            log::Info("body: %s loaded (profile v%u; standing %.1f, seated %.1f, unclassified %.1f cm)",
                      M.calibrated ? "calibrated arms" : "height-only profile, keeping legacy/default arms",
                      M.profile.version, M.profile.standing_eye_cm, M.profile.seated_eye_cm, M.profile.legacy_eye_cm);
            for (int s = 0; M.calibrated && s < 2; ++s) {
                const auto& arm = M.profile.arm[s];
                double upper = 0, forearm = 0;
                if (!body_calibration::UsableSegments(arm, &upper, &forearm))
                    log::Info("body: %s arm's saved split (upper %.1f, forearm %.1f cm, forearm %.0f%% of the arm) is "
                              "outside a human arm's %.0f-%.0f%%: the elbow moved during setup's bend. Using upper %.1f, "
                              "forearm %.1f cm (same %.1f cm reach); the saved file is unchanged",
                              s == kL ? "left" : "right", arm.upper_cm, arm.forearm_cm,
                              100.0 * arm.forearm_cm / (arm.upper_cm + arm.forearm_cm),
                              100.0 * body_calibration::kMinForearmShare, 100.0 * body_calibration::kMaxForearmShare,
                              upper, forearm, upper + forearm);
            }
            if (M.profile.standing_eye_cm > 0) {
                M.standing_eye_cm = M.profile.standing_eye_cm;
                if (xr::StandingSpace()) stereo::SetStandingEyeHeight(M.standing_eye_cm / 100.0);
                char height[32]; std::snprintf(height, sizeof(height), "%.2f", M.standing_eye_cm);
                config::Set("standing_height_cm", height);
                config::NotifyChanged();
            }
            if (M.calibrated) return;
            // Height-only calibration intentionally leaves the old arm scale.
        } else {
            log::Warn("body: calibration profile is invalid; using the previous legacy/default fit");
        }
    }
    if (!ReadText(ModPath(L"body-measure.ini"), &text)) {
        if (!M.calibrated) log::Info("body: use VR Options > Calibrate body for height / overhead / forward / T / bent-elbow setup");
        return;
    }
    double s = 0;
    const char* p = std::strstr(text.c_str(), "arm_scale=");
    if (p && std::sscanf(p + 10, "%lf", &s) == 1 && s > 0.7 && s < 1.5) {
        M.have = true;
        M.arm_scale = s;
        log::Info("body: arm scale %.3f from your measurement", s);
    }
    // Old one-pose height has no physical-posture provenance. Retain only
    // its arm scale; a new standing-mode calibration can establish height.
}

double ArmScale() {
    if (S.arm_scale_override > 0.0) return S.arm_scale_override;
    return M.have ? M.arm_scale : 1.0;
}
double MaxStretch() { return M.calibrated ? 1.0 : M.have || S.arm_scale_override > 0.0 ? S.max_stretch : S.max_stretch_unmeasured; }

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
hands::Pose g_anchors[static_cast<int>(Anchor::kCount)];  // holsters (body.h Anchor)
bool g_slots_valid = false;

UObject g_poseable_class = 0;
UObject g_skinned_class = 0;

perf::Counter g_perf_tick{"body tick"};
perf::Counter g_perf_engine{"body engine read+write"};
perf::Counter g_perf_solve{"body solve"};
perf::Counter g_perf_grip{"body grip solve (once per grip)"};
perf::Counter g_perf_legs{"body legs and ground"};
perf::Counter g_perf_arms{"body arms and fingers"};
perf::Counter g_perf_calibration{"body calibration"};

// ---- setup ----
UObject FindBodyMesh(UObject player) {
    UObject mesh = 0;
    if (ue::GetProp(player, "BodyMesh", mesh) && mesh) return mesh;
    if (ue::GetProp(player, "Mesh", mesh) && mesh) return mesh;
    return 0;
}

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

    G.pose_comp = pose;
    log::Info("body: VR body created (%s, %d materials)", ue::GetName(mesh).c_str(), materials);
    return true;
}

// Reads a (possibly bitfield) bool property. -1 if it can't be read.
int ReadBoolProp(UObject obj, const char* name) {
    auto p = ue::reflection::FindProperty(ue::GetClass(obj), name);
    if (!p.valid() || p.type != "BoolProperty") return -1;
    uint8_t b = 0;
    if (!mem::Read(obj + p.offset, b)) return -1;
    return (b & (p.bool_mask ? p.bool_mask : 0xFF)) ? 1 : 0;
}

// The game's hidden character Mesh casts the player's shadow on its own
// (that's the walking shadow the owner saw, out of step with the VR body).
// One shadow only: by default the VR body's, so it moves with you.
void ApplyShadows() {
    if (G.anim_src && G.anim_src != G.source) {
        if (G.mesh_hidden_shadow < 0) G.mesh_hidden_shadow = ReadBoolProp(G.anim_src, "bCastHiddenShadow");
        CallBool(G.anim_src, "SetCastHiddenShadow", "NewCastHiddenShadow", S.shadow == 1);
    }
    CallBool(G.source, "SetCastHiddenShadow", "NewCastHiddenShadow", S.shadow == 1 && !G.anim_src);
    CallBool(G.pose_comp, "SetCastShadow", "NewCastShadow", S.shadow == 2);
}

void StopEditor();
void ResetHandPhysics(int side = -1);

void Activate() {
    if (G.active || !G.pose_comp) return;
    G.followers.clear();
    CollectFollowers(G.source, G.source, 0, &G.followers);
    for (UObject f : G.followers) SetLeader(f, G.pose_comp);
    SetVisible(G.pose_comp, true);
    // The posed copy's bounds don't follow our IK (arms reach outside them),
    // so the engine culled it at some view angles. Inflate them.
    {
        ue::Call b(G.pose_comp, "SetBoundsScale");
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
    ResetHandPhysics();
    StopEditor();
    if (!G.active) return;
    for (UObject f : G.followers) SetLeader(f, G.source);
    G.followers.clear();
    if (G.pose_comp) SetVisible(G.pose_comp, false);
    if (G.source) {
        SetVisible(G.source, true);
        CallBool(G.source, "SetCastHiddenShadow", "NewCastHiddenShadow", false);
    }
    if (G.anim_src && G.anim_src != G.source && G.mesh_hidden_shadow >= 0) {
        CallBool(G.anim_src, "SetCastHiddenShadow", "NewCastHiddenShadow", G.mesh_hidden_shadow == 1);
    }
    G.active = false;
    log::Info("body: VR body off, game body back");
}

bool Setup(UObject player) {
    UObject source = FindBodyMesh(player);
    if (!source) return false;
    if (player == G.player && source == G.source && G.pose_comp) return true;
    if (G.failed && player == G.player && source == G.source) return false;
    StopEditor();
    ResetHandPhysics();
    G = State();
    G.player = player;
    G.source = source;
    if (!CreatePose(player, source)) {
        G.failed = true;
        return false;
    }
    if (!G.engine.Init(G.pose_comp) || !G.pose.Init(G.engine.parent)) {
        log::Error("body: could not read the skeleton, VR body disabled");
        G.failed = true;
        SetVisible(G.pose_comp, false);
        return false;
    }
    G.skel = MapSkeleton(G.engine.name_str);
    if (G.skel.pelvis < 0 && !G.skel.arms_ok[0] && !G.skel.arms_ok[1]) {
        log::Error("body: skeleton not recognised, VR body disabled (bone list is in the log)");
        G.failed = true;
        SetVisible(G.pose_comp, false);
        return false;
    }
    // BodyMesh may not animate itself: it can follow the character Mesh's
    // pose (LeaderPoseComponent), and then its own bone array never moves,
    // so a copy of it has still legs. Copy from the Mesh that animates.
    {
        UObject mesh = 0;
        int32_t loff = ue::PropertyOffset(source, "LeaderPoseComponent");
        int32_t leader = loff >= 0 ? mem::ReadOr<int32_t>(source + loff, -1) : -1;
        if (ue::GetProp(player, "Mesh", mesh) && mesh && mesh != source && leader >= 0 &&
            leader == mem::ReadOr<int32_t>(mesh + ue::off::kObjIndex, -2)) {
            G.anim_src = mesh;
            ue::SetProp<uint8_t>(mesh, "VisibilityBasedAnimTickOption", 0);  // AlwaysTickPoseAndRefreshBones
            // Hidden and (now) casting no shadow, the engine would drop it
            // to its lowest detail, which can filter out animation notifies
            // such as footsteps. Keep it at full detail.
            ue::Call lod(mesh, "SetForcedLOD");
            lod.Set("InNewForcedLOD", static_cast<int32_t>(1));
            lod.Invoke();
            log::Info("body: BodyMesh follows %s's animation, so the VR body copies from %s",
                      ue::GetName(mesh).c_str(), ue::GetName(mesh).c_str());
        } else {
            log::Info("body: BodyMesh animates itself (leader %d), the VR body copies it", leader);
        }
    }
    G.ref_pending = true;
    return true;
}

// ---- walk check: which of the player's meshes actually animate walking ----
// The owner saw the shadow walk but not the VR body's legs. The player has
// both "BodyMesh" (our source) and the character "Mesh"; this logs, while the
// character moves, how far the left foot swings in each and in the VR body.
struct WalkCheck {
    UObject comps[3] = {0, 0, 0};  // BodyMesh, Mesh (if different), VR body
    double lo[3][3], hi[3][3];
    bool have[3] = {false, false, false};
    double t = 0, speed = 0;
    int n = 0, reports = 0;
    bool listed = false;
    UObject player = 0;
} g_walk;

double ReadFootSwing(int i) {
    double r = 0;
    for (int a = 0; a < 3; ++a) r = std::max(r, g_walk.hi[i][a] - g_walk.lo[i][a]);
    return r;
}

void WalkCheckTick(UObject player, double speed, double dt) {
    static ue::FName foot{};
    static bool foot_ok = ue::names::Find("foot_l", &foot);
    if (!foot_ok) return;
    if (g_walk.player != player || g_walk.comps[2] != G.pose_comp) {
        g_walk = WalkCheck{};
        g_walk.player = player;
        UObject mesh = 0;
        g_walk.comps[0] = G.source;
        if (ue::GetProp(player, "Mesh", mesh) && mesh && mesh != G.source) g_walk.comps[1] = mesh;
        g_walk.comps[2] = G.pose_comp;
    }
    if (!g_walk.listed) {
        g_walk.listed = true;
        const char* label[2] = {"BodyMesh", "character Mesh"};
        for (int i = 0; i < 2; ++i) {
            UObject c = g_walk.comps[i];
            if (!c) {
                if (i == 1) log::Info("body walk check: the character Mesh is the same as BodyMesh (or missing)");
                continue;
            }
            ue::Call a(c, "GetSkinnedAsset");
            UObject asset = a.Invoke() ? a.Result<UObject>() : 0;
            ue::Call v(c, "IsVisible");
            bool vis = v.Invoke() && v.Result<bool>();
            log::Info("body walk check: %s = %s, mesh %s, %s", label[i], ue::GetName(c).c_str(),
                      asset ? ue::GetName(asset).c_str() : "none", vis ? "visible" : "hidden");
        }
    }
    if (speed < 30.0 || g_walk.reports >= 6) return;
    for (int i = 0; i < 3; ++i) {
        UObject c = g_walk.comps[i];
        if (!c) continue;
        ue::Call st(c, "GetSocketTransform");
        st.Set("InSocketName", foot).Set("TransformSpace", static_cast<uint8_t>(2));  // RTS_Component
        if (!st.Invoke()) continue;
        FVector p = st.Result<Xf>().translation;
        double v[3] = {p.X, p.Y, p.Z};
        for (int a = 0; a < 3; ++a) {
            if (!g_walk.have[i]) g_walk.lo[i][a] = g_walk.hi[i][a] = v[a];
            g_walk.lo[i][a] = std::min(g_walk.lo[i][a], v[a]);
            g_walk.hi[i][a] = std::max(g_walk.hi[i][a], v[a]);
        }
        g_walk.have[i] = true;
    }
    g_walk.t += dt;
    g_walk.speed += speed;
    ++g_walk.n;
    if (g_walk.t < 2.0) return;
    char mesh_part[64] = "same as BodyMesh";
    if (g_walk.comps[1]) std::snprintf(mesh_part, sizeof(mesh_part), "%.0f cm", ReadFootSwing(1));
    log::Info("body walk check (moving %.0f cm/s for 2 s): left foot swung %.0f cm in BodyMesh, %s in the "
              "character Mesh, %.0f cm in the VR body",
              g_walk.speed / g_walk.n, ReadFootSwing(0), mesh_part, ReadFootSwing(2));
    ++g_walk.reports;
    g_walk.t = g_walk.speed = 0;
    g_walk.n = 0;
    for (bool& h : g_walk.have) h = false;
}



// ---- footstep study: which animation notifies fire (owner: no footstep sounds) ----
struct NotifyStudy {
    std::mutex m;
    std::unordered_map<UObject, int> fn_kind;  // 0 ignore, 1 log
    std::map<std::string, int> counts;
    int lines = 0;
} g_ns;
bool g_ns_on = false;

void OnAnyNotify(UObject self, UObject function, void* params) {
    if (!self || !function) return;
    std::lock_guard<std::mutex> lock(g_ns.m);
    auto it = g_ns.fn_kind.find(function);
    if (it == g_ns.fn_kind.end()) {
        std::string fn = ue::GetName(function);
        std::string lo = Lower(fn);
        bool want = fn.rfind("AnimNotify_", 0) == 0 || fn == "Received_Notify" || fn == "Received_NotifyBegin" ||
                    lo.find("foot") != std::string::npos || lo.find("step") != std::string::npos ||
                    lo.find("sound") != std::string::npos;
        it = g_ns.fn_kind.emplace(function, want ? 1 : 0).first;
    }
    if (!it->second) return;
    std::string key = ue::GetName(ue::GetClass(self)) + "." + ue::GetName(function);
    int& n = ++g_ns.counts[key];
    if (n <= 2 && g_ns.lines < 120) {
        ++g_ns.lines;
        std::string mesh;
        if (ue::GetName(function).rfind("Received_Notify", 0) == 0 && params)
            mesh = " on " + ue::GetName(mem::ReadOr<UObject>(reinterpret_cast<uintptr_t>(params), 0));
        log::Info("body footstep study: %s%s", key.c_str(), mesh.c_str());
    }
}

void NotifyStudyInit() {
    // Off unless asked for: footsteps work (owner, 2026-11-02), and the study
    // put every animation update of every character through our event hook.
    if (!config::GetBool("body_footstep_study", false)) return;
    g_ns_on = true;
    const char* cdos[] = {"AnimNotify /Script/Engine.Default__AnimNotify",
                          "AnimNotifyState /Script/Engine.Default__AnimNotifyState",
                          "AnimInstance /Script/Engine.Default__AnimInstance"};
    for (const char* name : cdos) {
        UObject cdo = ue::FindObject(name);
        bool ok = cdo && ue::process_event::HookVtableOf(cdo);
        log::Info("body footstep study: %s %s", name, ok ? "followed" : "not found");
    }
    ue::process_event::OnAny(&OnAnyNotify);
}

void NotifyStudyReport(double dt) {
    if (!g_ns_on) return;
    static double t = 0;
    static int reports = 0;
    t += dt;
    if (t < 15.0 || reports >= 8) return;
    t = 0;
    std::lock_guard<std::mutex> lock(g_ns.m);
    if (g_ns.counts.empty()) {
        log::Info("body footstep study: no animation notifies in the last 15 s");
        ++reports;
        return;
    }
    std::string line;
    for (auto& [k, n] : g_ns.counts) line += " | " + k + " x" + std::to_string(n);
    log::Info("body footstep study (15 s):%s", line.c_str());
    g_ns.counts.clear();
    ++reports;
}

// ---- world <-> component space (this frame) ----
Xf g_w;  // the pose component's transform
Xf WX(int i) { return skel::Compose(G.pose.C(i), g_w); }
FVector WP(int i) { return skel::Apply(g_w, G.pose.C(i).translation); }
void SetWX(int i, const Xf& t) { G.pose.SetC(i, skel::Relative(t, g_w)); }
void RotW(int i, const FQuat& q) {
    Xf t = WX(i);
    t.rotation = Normalized(q * t.rotation);
    SetWX(i, t);
}

// ---- the hands' own geometry, from the reference pose ----
void BuildRig(int side, const skel::Pose& refp) {
    const Skeleton& k = G.skel;
    HandRig& r = G.rig[side];
    r = HandRig{};
    if (!k.arms_ok[side] || !k.fingers_ok[side]) return;
    const Xf hand = refp.C(k.hand[side]);
    Xf j[hand_pose::kFingerCount][3];
    for (int f = 0; f < hand_pose::kFingerCount; ++f)
        for (int n = 0; n < 3; ++n) j[f][n] = skel::Relative(refp.C(k.finger[side][f][n]), hand);
    const FVector mcp_i = j[hand_pose::kIndex][0].translation, mcp_m = j[hand_pose::kMiddle][0].translation,
                  mcp_p = j[hand_pose::kPinky][0].translation;
    r.geo.knuckle = mcp_m;
    r.geo.fingers = Normalize(mcp_m);
    r.geo.across = Normalize(mcp_i - mcp_p);
    r.across_h = r.geo.across;
    // Right hand: across x palm points from the wrist to the knuckles.
    r.palm_h = side == kR ? Normalize(Cross(r.geo.fingers, r.geo.across)) : Normalize(Cross(r.geo.across, r.geo.fingers));
    r.frame = MeasureHand(FVector{}, FQuat{}, mcp_i, mcp_m, mcp_p, side == kL, S.palm_depth);
    r.palm_centre = mcp_m * 0.55 + r.palm_h * 1.5;
    r.forearm_dir = hand.rotation.Inverse().Rotate(Normalize(hand.translation - refp.C(k.lowerarm[side]).translation));
    double lengths[5];
    for (int f = 0; f < hand_pose::kFingerCount; ++f) {
        r.chain[f] = fingers::Build(j[f], r.palm_h, mcp_p, f == hand_pose::kThumb);
        lengths[f] = r.chain[f].length;
    }
    double check = Dot(Normalize(Cross(r.geo.across, r.palm_h)), r.geo.fingers);
    r.ok = r.frame.valid && (side == kR ? check > 0.5 : check < -0.5);
    // Joint ranges: anatomical, shifted by how bent each joint already is in
    // the reference pose.
    std::string bends;
    for (int f = 0; f < hand_pose::kFingerCount; ++f) {
        double ref[3];
        jl::ReferenceBends(r.chain[f], f == hand_pose::kThumb, ref);
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%s%.0f/%.0f/%.0f", f ? ", " : "", ref[0] * kRadToDeg, ref[1] * kRadToDeg,
                      ref[2] * kRadToDeg);
        bends += buf;
        for (int n = 0; n < 3; ++n) {
            r.axes[f][n] = jl::JointAxes(r.chain[f], n);
            r.limits[f][n] = jl::Relative(jl::Human(f, n), ref[n]);
        }
    }
    log::Info("body: %s hand joint limits %s: reference pose bends (deg, base/middle/tip, thumb..pinky) %s",
              side == kL ? "left" : "right", S.joint_limits ? "on" : "OFF (hand_joint_limits=0)", bends.c_str());
    log::Info("body: %s hand: middle knuckle %.1f cm from the wrist, fingers %.1f %.1f %.1f %.1f %.1f cm "
              "(thumb..pinky), handedness %.2f%s",
              side == kL ? "left" : "right", Length(mcp_m), lengths[0], lengths[1], lengths[2], lengths[3], lengths[4],
              check, r.ok ? "" : " (FAILED: no stored grips for this hand)");
}

void PrewarmGrips();  // below

void CaptureReference() {
    G.engine.ResetAll();
    if (!G.engine.Read(&G.ref)) {
        log::Error("body: could not read the reference pose");
        G.failed = true;
        return;
    }
    skel::Pose refp;
    refp.Init(G.engine.parent);
    refp.SetAllLocal(G.ref.data(), static_cast<int>(G.ref.size()));
    for (int side = 0; side < 2; ++side) BuildRig(side, refp);
    PrewarmGrips();
    const Skeleton& k = G.skel;
    const Xf reference_world = ComponentToWorld(G.pose_comp);
    ue::Call actor_rotation(G.player, "K2_GetActorRotation");
    const double reference_yaw = actor_rotation.Invoke() ? actor_rotation.Result<FRotator>().Yaw : 0;
    const FVector forward_cs = reference_world.rotation.Inverse().Rotate(YawOnly(reference_yaw).Rotate(FVector{1,0,0}));
    const FVector down_cs = reference_world.rotation.Inverse().Rotate(FVector{0,0,-1});
    if (k.pelvis >= 0) G.reference_pelvis_height = std::max(0.0, refp.C(k.pelvis).translation.Z - refp.C(0).translation.Z);
    for (int s = 0; s < 2; ++s) {
        if (k.arms_ok[s]) {
            const auto& upper = refp.C(k.upperarm[s]);
            const auto& lower = refp.C(k.lowerarm[s]);
            const auto& hand = refp.C(k.hand[s]);
            auto& accessory = G.watch_arm[s];
            accessory.ref_lower = lower;
            accessory.ref_hand = hand;
            accessory.has_twist = k.forearm_twist[s] >= 0;
            accessory.ref_twist = accessory.has_twist ? refp.C(k.forearm_twist[s]) : lower;
            G.arm_ref[s] = arm_pose::MeasureReference(upper, lower, hand, down_cs);
            if (k.upperarm_twist[s] >= 0)
                G.upper_twist_ref[s] = arm_pose::MeasureAttachment(upper, lower, refp.C(k.upperarm_twist[s]));
            if (k.forearm_twist[s] >= 0)
                G.forearm_twist_ref[s] = arm_pose::MeasureAttachment(lower, hand, refp.C(k.forearm_twist[s]));
            log::Info("body: %s twist skin joints: upper %d parent %d along %.3f valid %d; forearm %d parent %d along %.3f valid %d; hand %d parent %d",
                      s == kL ? "left" : "right", k.upperarm_twist[s], k.upperarm_twist[s] >= 0 ? G.pose.Parent(k.upperarm_twist[s]) : -1,
                      G.upper_twist_ref[s].along, G.upper_twist_ref[s].valid,
                      k.forearm_twist[s], k.forearm_twist[s] >= 0 ? G.pose.Parent(k.forearm_twist[s]) : -1,
                      G.forearm_twist_ref[s].along, G.forearm_twist_ref[s].valid, k.hand[s], G.pose.Parent(k.hand[s]));
        }
        if (k.legs_ok[s] && k.pelvis >= 0) {
            G.leg_ref[s] = arm_pose::MeasureReference(refp.C(k.thigh[s]), refp.C(k.calf[s]), refp.C(k.foot[s]), forward_cs);
            G.leg_upper_cs[s] = refp.C(k.calf[s]).translation - refp.C(k.thigh[s]).translation;
            G.leg_lower_cs[s] = refp.C(k.foot[s]).translation - refp.C(k.calf[s]).translation;
            G.sole_offset[s] = std::clamp(refp.C(k.foot[s]).translation.Z - refp.C(0).translation.Z, 0.0, 30.0);
            G.sole_up_local[s] = refp.C(k.foot[s]).rotation.Inverse().Rotate(FVector{0, 0, 1});
            G.stance_from_pelvis[s] = refp.C(k.foot[s]).translation - refp.C(k.pelvis).translation;
            G.stance_rotation[s] = refp.C(k.foot[s]).rotation;
            if (k.toe[s] >= 0) G.toe_reference[s] = refp.C(k.toe[s]).rotation;
        }
    }
    if (k.arms_ok[kR]) {
        double upper = Length(refp.C(k.lowerarm[kR]).translation - refp.C(k.upperarm[kR]).translation);
        double lower = Length(refp.C(k.hand[kR]).translation - refp.C(k.lowerarm[kR]).translation);
        log::Info("body: avatar arm %.1f cm (upper %.1f, forearm %.1f)", upper + lower, upper, lower);
    }
    G.ref_pending = false;
}

// ---- stored grips ----
int TipFinger(int tip) {
    switch (tip) {
        case gr::kThumb: return hand_pose::kThumb;
        case gr::kIndexOff:
        case gr::kIndexRest:
        case gr::kIndexPull: return hand_pose::kIndex;
        case gr::kMiddle: return hand_pose::kMiddle;
        case gr::kRing: return hand_pose::kRing;
        default: return hand_pose::kPinky;
    }
}

// Keeps posed finger t of a record to human joint ranges, as close to its
// shape as they allow (joint_limits.h: the middle and tip joints only curl,
// the knuckles spread a little and barely roll).
jl::FitReport LimitPosedFinger(const HandRig& r, gr::Record* rec, int t) {
    if (!S.joint_limits || !r.ok || !rec->posed[t]) return {};
    const int f = TipFinger(t);
    return jl::Fit(r.chain[f], r.axes[f], r.limits[f], rec->joint[t]);
}

bool IsGrab(hands::GrabKind k) {
    return k == hands::GrabKind::kGunGrip || k == hands::GrabKind::kSlideGrab || k == hands::GrabKind::kMagHeld ||
           k == hands::GrabKind::kMagInsert || k == hands::GrabKind::kSupportGrip;
}

// The frame a grip is stored in. Gun: the gun frame at its grip point.
// Slide / charging handle / rifle handguard: the gun frame at the point the
// hand holds. Magazine: the magazine itself.
// While a magazine grip is edited, the hand stays where it was on the
// controller and the magazine is placed from the hand (so moving the
// magazine in the editor moves the magazine, not the hand).
// While the grip editor runs, both hands and the held objects stay where
// they were when it started (owner: "when i initiate the pose editor i want
// my vr hands to freeze in place"); only the editing controller's grab point
// follows the real controller.
struct EditFreeze {
    bool on = false;
    hands::HandState hs[2];
    hands::HandGrab grab[2];
} g_freeze;

struct MagPin {
    bool on = false;
    int side = -1;
    UObject object = 0;
    Xf hand_in_ctrl;  // hand bone relative to the holding controller's grip
} g_pin;

Xf PoseXf(const hands::Pose& p) {
    Xf x;
    x.translation = p.location;
    x.rotation = p.rotation;
    return x;
}

bool PinnedObject(const hands::HandGrab& grab, hands::Pose* out) {
    if (!g_pin.on || grab.object != g_pin.object || G.grip[g_pin.side].object != grab.object) return false;
    hands::HandState h = g_freeze.on ? g_freeze.hs[g_pin.side] : hands::Get(g_pin.side == kL ? hands::kLeft : hands::kRight);
    if (!h.valid) return false;
    const Xf hand = skel::Compose(g_pin.hand_in_ctrl, PoseXf(h.grip));
    const ActiveGrip& g = G.grip[g_pin.side];
    out->rotation = Normalized(hand.rotation * g.hand_rot.Inverse());
    out->location = hand.translation - out->rotation.Rotate(g.hand_pos);
    Xf t;
    t.translation = out->location;
    t.rotation = out->rotation;
    ue::Call c(grab.object, "K2_SetActorTransform");
    c.Set("NewTransform", t).Set("bSweep", false).Set("bTeleport", true);
    c.Invoke();
    return true;
}

// The grab of the hand that holds `on`'s gun by its grip (either hand can,
// controls' either_hand_guns): slide and support grabs on that gun take its
// frame from it. The right hand's grab when neither does.
const hands::HandGrab& GunGrab(const hands::HandGrab grab[2], const hands::HandGrab& on) {
    for (int s : {kR, kL})
        if (grab[s].kind == hands::GrabKind::kGunGrip && grab[s].object == on.object) return grab[s];
    return grab[kR];
}

// A visual-only snap with the hand's own pose, posed by the owner: the left
// hand reaching for the holstered pistol (kPointHolster), the off hand on the
// pistol in the other hand (kPointOffhand*). (The right hand reaching for the
// holster uses its gun grip, fitted by holster_pose::Fit.)
bool OwnReach(const hands::HandGrab& grab) {
    if (!grab.holster_preview) return false;
    return (grab.kind == hands::GrabKind::kGunGrip && grab.point == hands::kPointHolster) ||
           (grab.kind == hands::GrabKind::kSupportGrip && hands::OffhandPoint(grab.point));
}

bool ObjectFrame(const hands::HandGrab& grab, const hands::HandGrab& right, hands::Pose* out) {
    if ((grab.kind == hands::GrabKind::kMagHeld || grab.kind == hands::GrabKind::kMagInsert) && PinnedObject(grab, out))
        return true;
    switch (grab.kind) {
        case hands::GrabKind::kGunGrip:
            *out = grab.target;
            return true;
        case hands::GrabKind::kSlideGrab:
        case hands::GrabKind::kSupportGrip:
            out->location = grab.target.location;
            out->rotation = right.kind == hands::GrabKind::kGunGrip && right.object == grab.object
                                ? right.target.rotation
                                : grab.target.rotation;
            return true;
        case hands::GrabKind::kMagHeld:
        case hands::GrabKind::kMagInsert: {
            if (!grab.object) return false;
            ue::Call c(grab.object, "GetTransform");
            if (!c.Invoke()) return false;
            Xf t = c.Result<Xf>();
            out->location = t.translation;
            out->rotation = t.rotation;
            return true;
        }
        default:
            return false;
    }
}

// A slide grab with no posed hold yet: the hand stays where the controller
// was on the slide when it grabbed (relative to the grab point), instead of
// being pulled onto the grab point inside the slide.
struct FreeSlideHand {
    bool on = false;
    FQuat rot;     // controller grip in the object frame
    FVector pos;
} g_free_slide[2];

// Slide grabs have several poses, picked once when the hand closes on the
// slide (like Half-Life: Alyx). Hand on the FRONT half of the slide = front
// hold. On the rear half the angle decides: palm facing down onto it =
// overhand, otherwise a rear pinch. The rear pinch has two records, posed
// from above and from behind the slide (PickRearPinch). Each is its own record.
enum SlideHold { kSlideOver, kSlideRear, kSlideFront, kSlideBehind, kSlideHoldCount };
// (New names on 2026-11-02 so the owner's earlier test saves, slide-over/front, do not hide the built-in holds.)
const char* kSlideHoldKey[] = {"rear-flat", "rear-pinch", "front-flat", "rear-pinch-behind"};
const char* kSlideHoldName[] = {"rear overhand", "rear pinch", "front", "rear pinch from behind"};
int g_slide_hold[2] = {kSlideOver, kSlideOver};
UObject g_slide_jump_logged[2] = {0, 0};  // slide grab whose drawn jump is logged

bool PartBox(UObject comp, FVector* mn, FVector* mx, const char** how);

// budget_ms: time this call may spend measuring the slide (< 0: finish now).
bool SlideMarksWorld(UObject gun, const FQuat& gun_rot, FVector* sight, FVector* rear, FVector* front,
                     double budget_ms = -1);

int PickSlideHold(const hands::HandState& h, const hands::Pose& gun, UObject gun_actor) {
    const FQuat inv = gun.rotation.Inverse();
    const FVector n = inv.Rotate(h.palm_normal);                    // palm direction, gun frame
    const FVector at = inv.Rotate(h.grip.location - gun.location);  // fist from the grab point, gun frame
    // From the measured slide: front half = front hold; within reach of
    // the rear sight = pinch (owner: "only the rear iron sights"); the rest
    // of the rear = overhand. Unmeasured: 5 cm ahead of the grab point
    // splits front/rear and there is no pinch.
    // Pinch zone: within this of the rear sight, stretched back along the
    // slide (owner 2026-11-03: "a little bit longer and extended past the back
    // of the slide by about .25 of an inch"). Same as the controls grab zone.
    static const double pinch_sight = config::GetFloat("hand_slide_pinch_radius_cm", 6.5f);
    static const double pinch_back = config::GetFloat("hand_slide_pinch_back_cm", 10.f);
    FVector sight_w, rear_w, front_w;
    const bool shape = SlideMarksWorld(gun_actor, gun.rotation, &sight_w, &rear_w, &front_w);
    double split = 5.0, to_sight = -1;
    FVector sight_at{};
    if (shape) {
        split = (inv.Rotate(rear_w - gun.location).X + inv.Rotate(front_w - gun.location).X) * 0.5;
        sight_at = inv.Rotate(sight_w - gun.location);
        // Distance to the segment from the sight back along the barrel.
        const double along = std::clamp(at.X - sight_at.X, -pinch_back, 0.0);
        to_sight = Length(at - (sight_at + FVector{along, 0, 0}));
    }
    int best;
    // Rear half outside the pinch reach: owner 2026-11-03 10:47 kept getting
    // the overhand pose when reaching for the pinch (fists 10-13 cm back and
    // a little low, palm turned in). Overhand only when the palm faces down
    // onto the top of the slide; any other rear grab is the pinch.
    const bool palm_down_on_top = n.Z < -0.85 && (!shape || at.Z > sight_at.Z - 1.0);
    if (shape && to_sight < pinch_sight) best = kSlideRear;
    else if (at.X > split) best = kSlideFront;
    else if (shape && !palm_down_on_top) best = kSlideRear;
    else best = kSlideOver;
    log::Info("body: slide grab: %s hold (fist (%.1f %.1f %.1f) from the grab point; %s; rear sight at (%.1f %.1f "
              "%.1f), %.1f cm from it, pinch within %.1f; front half from %.1f; palm in gun frame %.2f %.2f %.2f)",
              kSlideHoldName[best], at.X, at.Y, at.Z, shape ? "slide measured" : "slide NOT measured", sight_at.X,
              sight_at.Y, sight_at.Z, to_sight, pinch_sight, split, n.X, n.Y, n.Z);
    return best;
}

std::string BaseRecordKey(int side, const hands::HandGrab& grab) {
    // Shotgun shells in the hand: one pose per hold, whatever the ammo type
    // (controls' smart reload: quad, twin, single).
    if (grab.kind == hands::GrabKind::kMagHeld && hands::ShellsPoint(grab.point)) {
        const int n = grab.point - hands::kPointShells;
        return std::string("shells.") + (n >= 4 ? "quad" : n >= 2 ? "twin" : "single");
    }
    std::string cls = grab.object ? ue::GetName(ue::GetClass(grab.object)) : "unknown";
    switch (grab.kind) {
        // A hand reaching for the holstered pistol with its own pose
        // (kPointHolster): one per hand, see RecordKey.
        // The main hand on a long gun pulled back for a butt strike
        // (kPointButtStrike, gun_melee::ButtStrikeStance): its own pose.
        case hands::GrabKind::kGunGrip:
            if (grab.point == hands::kPointButtStrike) return cls + ".butt-strike";
            return grab.point == hands::kPointHolster ? cls + ".holster" : cls + ".grip";
        // A rifle's charging handle (grab.point = kPointChargingHandle): one
        // pose for each hand (owner, 2026-11-04: "one for left hand, one for
        // right hand, at the rear of the charging handle").
        case hands::GrabKind::kSlideGrab:
            if (grab.point >= 0) return cls + ".charging-" + (side == kL ? "left" : "right");
            return cls + ".slide-" + kSlideHoldKey[g_slide_hold[side]];
        // One pose per grip point on the handguard (vr/interact/grip_points.h).
        // The off hand snapped onto the pistol in the other hand (controls'
        // offhand_snap): one pose with the gun held out, one near the chest.
        case hands::GrabKind::kSupportGrip:
            if (grab.point == hands::kPointOffhand) return cls + ".offhand";
            if (grab.point == hands::kPointOffhandChest) return cls + ".offhand-chest";
            return grab.point >= 0 ? cls + ".support-" + interact::PointKey(grab.point) : cls + ".support";
        // Stage 2: the magazine lined up with the magwell, going in.
        case hands::GrabKind::kMagInsert: return cls + ".mag-insert";
        default: return cls + ".mag";
    }
}

// Either hand can hold either gun and the magazines (controls,
// either_hand_guns, owner 2026-11-06). A hand holding something the other
// hand usually holds (the gun's grip: the right; the slide, a magazine or the
// handguard: the left) has its own saved pose, "-left" / "-right" after the
// name, so posing it never changes the usual hand's. Until it is posed it
// uses the usual hand's pose, mirrored. (The charging handle already has one
// pose per hand.)
std::string RecordKey(int side, const hands::HandGrab& grab) {
    const bool charging = grab.kind == hands::GrabKind::kSlideGrab && grab.point >= 0;
    // The reach for the holstered pistol always has the hand's own name
    // (BP_HG_C.holster-left): GripRecord never mirrors it from the other hand.
    // So does the off hand on the pistol (BP_HG_C.offhand-left,
    // BP_HG_C.offhand-chest-left).
    const bool holster = grab.kind == hands::GrabKind::kGunGrip && grab.point == hands::kPointHolster;
    const bool offhand = grab.kind == hands::GrabKind::kSupportGrip && hands::OffhandPoint(grab.point);
    const int usual = grab.kind == hands::GrabKind::kGunGrip ? kR : kL;
    return gr::SideKey(BaseRecordKey(side, grab), side == kR,
                       !holster && !offhand && (charging || side == usual));
}

std::wstring RecordPath(const std::string& key) { return ModPath(L"grips\\" + Widen(key) + L".ini"); }

fingers::DepthFn DepthIn(const ActiveGrip& g) {
    return [&g](const FVector& p) { return gr::Depth(g.rec, g.hand_pos + g.hand_rot.Rotate(p)); };
}

// Solves the fingers for the record's targets. tip = -1: all of them;
// quick: refine from the current solution (live editing). Plain math on `g`
// and `r` only, so it can run on a worker thread on copies.
void SolveGripOn(ActiveGrip& g, const HandRig& r, int only_tip, bool quick, double finger_radius) {
    gr::HandInObject(g.rec, r.geo, &g.hand_rot, &g.hand_pos);
    fingers::DepthFn fn = DepthIn(g);
    for (int tip = 0; tip < gr::kTipCount; ++tip) {
        if (only_tip >= 0 && tip != only_tip) continue;
        const int f = TipFinger(tip);
        if (g.rec.posed[tip]) {
            // Set joint by joint in the editor: no solving.
            fingers::Angles a;
            for (int j = 0; j < 3; ++j) a.extra[j] = g.rec.joint[tip][j];
            g.tip_angles[tip] = a;
            g.err[tip] = 0;
            g.clip[tip] = g.rec.outline.empty()
                              ? 0.0
                              : fingers::FingerDepth(r.chain[f], a, fn, finger_radius);
            continue;
        }
        FVector target = g.hand_rot.Inverse().Rotate(g.rec.tip[tip] - g.hand_pos);
        g.tip_angles[tip] = fingers::Solve(r.chain[f], target, fingers::DefaultLimits(f == hand_pose::kThumb),
                                           &g.err[tip], g.rec.outline.empty() ? nullptr : &fn, finger_radius,
                                           &g.clip[tip], quick ? &g.tip_angles[tip] : nullptr);
    }
    g.palm_clip = 0;
    for (double along : {0.3, 0.5, 0.7, 0.9})
        for (double sidew : {-0.4, 0.0, 0.4}) {
            FVector p = r.geo.knuckle * along + r.palm_h * 1.5 + r.geo.across * (sidew * 5.0);
            g.palm_clip = std::max(g.palm_clip, fn(p));
        }
    // The web: from the thumb's base joint to the index knuckle.
    g.web_clip = 0;
    const FVector tb = r.chain[hand_pose::kThumb].ref[0].translation, ib = r.chain[hand_pose::kIndex].ref[0].translation;
    for (double a : {0.0, 0.25, 0.5, 0.75}) g.web_clip = std::max(g.web_clip, fn(tb + (ib - tb) * a));
    g.approach_ready = false;
    if (!quick && g.key == "BP_HG_C.grip" && g.rec.right_hand) {
        g.approach = holster_pose::Fit(g.rec, r.chain, g.hand_rot, g.hand_pos, finger_radius);
        g.approach_ready = true;
    }
}

void SolveGrip(int side, int only_tip, bool quick) {
    perf::Scope t(g_perf_grip);
    SolveGripOn(G.grip[side], G.rig[side], only_tip, quick, S.finger_radius);
}

void ReportGripOn(const ActiveGrip& g, const HandRig& r, const char* why);
void ReportGrip(int side, const char* why) { ReportGripOn(G.grip[side], G.rig[side], why); }

void ReportGripOn(const ActiveGrip& g, const HandRig& r, const char* why) {
    std::string tips, clips;
    double worst = g.palm_clip;
    const char* worst_name = "palm";
    for (int t = 0; t < gr::kTipCount; ++t) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), " %s %.1f", gr::TipName(t), g.err[t]);
        tips += buf;
        std::snprintf(buf, sizeof(buf), " %s %.1f", gr::TipName(t), g.clip[t]);
        clips += buf;
        if (g.clip[t] > worst) {
            worst = g.clip[t];
            worst_name = gr::TipName(t);
        }
    }
    log::Info("body: grip %s (%s, %s): fingertips off target (cm):%s", g.key.c_str(),
              g.from_file ? "saved file" : "built-in", why, tips.c_str());
    log::Info("body: grip %s clip check: deepest %.1f cm inside the %s outline (%s); fingers:%s palm %.1f%s",
              g.key.c_str(), worst, g.rec.outline.empty() ? "(no outline)" : "object's", worst_name, clips.c_str(),
              g.palm_clip, worst < 0.3 ? " - clean" : "");
    // How the hand lines up with the object (gun: X = barrel): with a
    // straight wrist, the forearm runs along this direction.
    if (r.ok) {
        FVector fa = Normalize(g.hand_rot.Rotate(r.forearm_dir));
        log::Info("body: grip %s hand: wrist at (%.1f %.1f %.1f) cm; a straight wrist puts the forearm %.0f deg %s and "
                  "%.0f deg %s of the barrel; web of the hand %.1f cm inside",
                  g.key.c_str(), g.hand_pos.X, g.hand_pos.Y, g.hand_pos.Z,
                  std::fabs(std::atan2(fa.Y, fa.X)) * kRadToDeg, fa.Y >= 0 ? "right" : "left",
                  std::fabs(std::atan2(fa.Z, std::hypot(fa.X, fa.Y))) * kRadToDeg, fa.Z >= 0 ? "above" : "below",
                  g.web_clip);
    }
}

// Solved grips by record key and hand, so picking the same kind of object
// up again costs nothing.
std::map<std::string, ActiveGrip> g_grip_cache;
std::string CacheKey(int side, const std::string& key) { return key + (side == kL ? "#L" : "#R"); }

// ---- grip solves off the game thread ----
// Solving a stored grip searches every fingertip that isn't posed joint by
// joint against the object's outline: about 90 ms for the pistol. It ran on
// the game thread the first time the gun was drawn, a freeze the owner
// noticed (2026-11-03: "the game hitches when i grab my gun for the first
// time"). The solve is plain math, so it runs on a worker thread on copies
// of the grip and the hand rig; until it is in (a few frames) the hand
// follows the controller. Saved gun grips are solved ahead, when the body is
// set up, so normally the draw finds them ready. Results land in
// g_grip_cache; all of this is touched on the game thread only, except
// SolveJob::grip and ::ms, which the worker owns until `done`.
struct SolveJob {
    int side = 0;
    std::string cache_key;
    ActiveGrip grip;
    HandRig rig;
    double radius = 0;
    double ms = 0;
    std::atomic<bool> done{false};
};
std::vector<std::shared_ptr<SolveJob>> g_solve_jobs;

// A solve worth taking off the game thread: some fingertip is solved
// against an outline (posed fingers only get a depth check).
bool LongSolve(const gr::Record& rec) {
    if (rec.outline.empty()) return false;
    for (int t = 0; t < gr::kTipCount; ++t)
        if (!rec.posed[t]) return true;
    return false;
}

DWORD WINAPI SolveWorker(LPVOID p) {
    std::unique_ptr<std::shared_ptr<SolveJob>> hold(static_cast<std::shared_ptr<SolveJob>*>(p));
    SolveJob& j = **hold;
    const double t0 = perf::NowMs();
    SolveGripOn(j.grip, j.rig, -1, false, j.radius);
    j.ms = perf::NowMs() - t0;
    j.done.store(true, std::memory_order_release);
    return 0;
}

// Starts solving `g` (rec and key set) for `side`; true if it is solving
// now (or already was).
bool StartSolve(int side, const ActiveGrip& g) {
    const std::string ck = CacheKey(side, g.key);
    for (const auto& j : g_solve_jobs)
        if (j->cache_key == ck) return true;
    auto job = std::make_shared<SolveJob>();
    job->side = side;
    job->cache_key = ck;
    job->grip = g;
    job->grip.object = 0;
    job->rig = G.rig[side];
    job->radius = S.finger_radius;
    auto* hold = new std::shared_ptr<SolveJob>(job);
    HANDLE t = CreateThread(nullptr, 0, SolveWorker, hold, CREATE_SUSPENDED, nullptr);
    if (!t) {
        delete hold;
        return false;
    }
    SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
    ResumeThread(t);
    CloseHandle(t);
    g_solve_jobs.push_back(job);
    return true;
}

// Game thread, once per tick: finished solves into the cache, and a hand
// waiting for one takes it.
void CollectSolves() {
    for (size_t i = 0; i < g_solve_jobs.size();) {
        SolveJob& j = *g_solve_jobs[i];
        if (!j.done.load(std::memory_order_acquire)) {
            ++i;
            continue;
        }
        // A grip saved or loaded meanwhile wins over this older solve.
        if (!g_grip_cache.count(j.cache_key)) {
            j.grip.have = true;
            j.grip.pending = false;
            g_grip_cache[j.cache_key] = j.grip;
            char why[96];
            std::snprintf(why, sizeof(why), "solved in %.1f ms off the game thread", j.ms);
            ReportGripOn(j.grip, j.rig, why);
        }
        g_solve_jobs.erase(g_solve_jobs.begin() + static_cast<std::ptrdiff_t>(i));
    }
    for (int side = 0; side < 2; ++side) {
        ActiveGrip& g = G.grip[side];
        if (!g.pending) continue;
        auto it = g_grip_cache.find(CacheKey(side, g.key));
        if (it == g_grip_cache.end()) {
            bool running = false;
            for (const auto& j : g_solve_jobs) running = running || j->cache_key == CacheKey(side, g.key);
            if (!running) g.pending = false;  // the worker could not start: stays on the controller
            continue;
        }
        const UObject object = g.object;
        const hands::GrabKind kind = g.kind;
        g = it->second;
        g.object = object;
        g.kind = kind;
        g.pending = false;
    }
}

// ---- built-in magazine hold ----
// The magazine's outline in its own actor frame: the union of its parts'
// boxes. Game parts report zero local bounds, so read the mesh asset's box
// (static mesh bounding box, or skeletal mesh bounds) and place it with the
// component's transform.
struct MeshBox {  // FBox: Min, Max, IsValid (+ padding) = 56 bytes
    FVector min, max;
    uint8_t valid;
    uint8_t pad[7];
};
struct MeshBounds {  // FBoxSphereBounds: Origin, BoxExtent, SphereRadius
    FVector origin, extent;
    double radius;
};

bool PartBox(UObject comp, FVector* mn, FVector* mx, const char** how) {
    auto ok = [](const FVector& a, const FVector& b) {
        double s = std::max({b.X - a.X, b.Y - a.Y, b.Z - a.Z});
        return std::isfinite(s) && s > 0.3 && s < 200.0;
    };
    ue::Call gm(comp, "GetStaticMesh");
    UObject mesh = gm.Invoke() ? gm.Result<UObject>() : 0;
    if (mesh) {
        ue::Call bb(mesh, "GetBoundingBox");
        if (bb.Invoke()) {
            MeshBox b = bb.Result<MeshBox>();
            if (ok(b.min, b.max)) return *mn = b.min, *mx = b.max, *how = "static mesh", true;
        }
    }
    ue::Call ga(comp, "GetSkinnedAsset");
    UObject skin = ga.Invoke() ? ga.Result<UObject>() : 0;
    if (skin) {
        ue::Call bb(skin, "GetBounds");
        if (bb.Invoke()) {
            MeshBounds b = bb.Result<MeshBounds>();
            if (ok(b.origin - b.extent, b.origin + b.extent))
                return *mn = b.origin - b.extent, *mx = b.origin + b.extent, *how = "skeletal mesh", true;
        }
    }
    ue::Call lb(comp, "GetLocalBounds");
    if (lb.Invoke()) {
        ue::Vec3d a = lb.Get<ue::Vec3d>("Min"), c = lb.Get<ue::Vec3d>("Max");
        FVector a2{a.x, a.y, a.z}, c2{c.x, c.y, c.z};
        if (ok(a2, c2)) return *mn = a2, *mx = c2, *how = "local bounds", true;
    }
    return false;
}

int ActorLocalBox(UObject actor, const Xf& actor_xf, FVector* lo, FVector* hi) {
    static UObject prim_class = ue::FindObject("Class /Script/Engine.PrimitiveComponent");
    if (!prim_class) return 0;
    ue::Call list(actor, "K2_GetComponentsByClass");
    list.Set("ComponentClass", prim_class);
    int32_t size = 0;
    const uint8_t* raw = list.Invoke() ? list.Raw("ReturnValue", &size) : nullptr;
    struct Arr {
        uintptr_t data;
        int32_t num, max;
    } arr{};
    if (raw && size >= (int32_t)sizeof(Arr)) std::memcpy(&arr, raw, sizeof(Arr));
    int parts = 0;
    for (int i = 0; i < arr.num && i < 64; ++i) {
        UObject c = mem::ReadOr<uintptr_t>(arr.data + i * sizeof(uintptr_t), 0);
        if (!c) continue;
        FVector mn, mx;
        const char* how = "";
        bool got = PartBox(c, &mn, &mx, &how);
        const Xf cw = ComponentToWorld(c);
        FVector a{}, b{};
        if (got) {
            for (int k = 0; k < 8; ++k) {
                FVector corner{k & 1 ? mx.X : mn.X, k & 2 ? mx.Y : mn.Y, k & 4 ? mx.Z : mn.Z};
                Xf pt;
                pt.translation = skel::Apply(cw, corner);
                FVector l = skel::Relative(pt, actor_xf).translation;
                if (k == 0) a = b = l;
                a = {std::min(a.X, l.X), std::min(a.Y, l.Y), std::min(a.Z, l.Z)};
                b = {std::max(b.X, l.X), std::max(b.Y, l.Y), std::max(b.Z, l.Z)};
            }
            if (parts == 0) *lo = a, *hi = b;
            *lo = {std::min(lo->X, a.X), std::min(lo->Y, a.Y), std::min(lo->Z, a.Z)};
            *hi = {std::max(hi->X, b.X), std::max(hi->Y, b.Y), std::max(hi->Z, b.Z)};
            ++parts;
        }
        log::Info("body: magazine part %s (%s): %s%s", ue::GetName(c).c_str(), ue::GetName(ue::GetClass(c)).c_str(),
                  got ? how : "no size",
                  got ? (" " + std::to_string(int(a.X * 10) / 10.0) + ".." + std::to_string(int(b.X * 10) / 10.0) +
                         " x, " + std::to_string(int(a.Y * 10) / 10.0) + ".." + std::to_string(int(b.Y * 10) / 10.0) +
                         " y, " + std::to_string(int(a.Z * 10) / 10.0) + ".." + std::to_string(int(b.Z * 10) / 10.0) +
                         " z cm in the magazine's frame")
                            .c_str()
                      : "");
    }
    return parts;
}

// Nearest actor axis (signed) to a direction.
FVector SnapAxis(const FVector& d, const FVector& not_this = FVector{}) {
    FVector best{1, 0, 0};
    double best_dot = -2;
    const FVector axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (const FVector& a : axes) {
        if (std::fabs(Dot(a, not_this)) > 0.5) continue;
        double dd = Dot(a, d);
        if (std::fabs(dd) > best_dot) best_dot = std::fabs(dd), best = dd < 0 ? a * -1.0 : a;
    }
    return best;
}

// The owner's magazine hold (reference photos 2026-11-01 and 2026-11-02):
// the magazine lies diagonally across the palm, base low in the heel of the
// hand, middle/ring/pinky wrapped round the lower body, index straight up the
// FRONT of the magazine to the top round, thumb up the side. Built in the
// magazine's frame from its measured outline and the way it is carried (its
// top points where the controller points, its front away from the back of
// the hand), then tuned in the grip editor.
bool DefaultMagRecord(int side, UObject mag, gr::Record* rec) {
    ue::Call t(mag, "GetTransform");
    if (!t.Invoke()) return false;
    const Xf actor = t.Result<Xf>();
    hands::HandState h = hands::Get(side == kL ? hands::kLeft : hands::kRight);
    if (!h.valid) return false;
    FVector lo{}, hi{};
    const int n = ActorLocalBox(mag, actor, &lo, &hi);
    // Mag axes in its own frame: up = controller pointing direction, front =
    // away from the back of the hand (controls carries it that way).
    const FQuat inv = actor.rotation.Inverse();
    FVector up_w = h.aim.rotation.Rotate(FVector{1, 0, 0});
    FVector fr_w = h.aim.rotation.Rotate(FVector{0, 0, -1});
    FVector U = SnapAxis(inv.Rotate(up_w));
    FVector F = SnapAxis(inv.Rotate(fr_w), U);
    FVector Sx = Cross(U, F);  // right
    auto along = [&](const FVector& a, double* mn, double* mx) {
        double p = Dot(lo, a), q = Dot(hi, a);
        *mn = std::min(p, q), *mx = std::max(p, q);
    };
    double u0, u1, f0, f1, s0, s1;
    along(U, &u0, &u1), along(F, &f0, &f1), along(Sx, &s0, &s1);
    double L = u1 - u0, d = f1 - f0, w = s1 - s0;
    FVector centre = (lo + hi) * 0.5;
    const bool measured = n >= 1 && L > 6 && L < 40 && d > 1 && d < 8 && w > 0.8 && w < 6;
    if (!measured) {
        // Glock 19 magazine, centred on the actor.
        L = 12.5, d = 3.2, w = 2.8;
        centre = U * (L * 0.5 - 1.0);
        lo = centre - (U * L + F * d + Sx * w) * 0.5;
        hi = centre + (U * L + F * d + Sx * w) * 0.5;
    }
    const double mirror = side == kL ? 1.0 : -1.0;  // the palm is on the mag's left side for the left hand
    // Canonical (side, front, up-from-base) -> magazine frame.
    auto P = [&](double sv, double fv, double uv) {
        return centre + Sx * (sv * mirror) + F * fv + U * (uv - L * 0.5);
    };
    auto D = [&](double sv, double fv, double uv) { return Normalize(Sx * (sv * mirror) + F * fv + U * uv); };
    gr::Record r;
    r.name = "magazine";
    r.right_hand = side == kR;
    // From the photos: the magazine stands up out of the hand like an
    // extension of the index finger. Its base sits in the palm at the roots
    // of the fingers, the palm against its left side; the index runs
    // straight up the front edge to the top round; middle, ring and pinky
    // curl round the lower body onto the right side; the thumb lies flat up
    // the right side.
    r.knuckle = P(-w * 0.5 - 1.2, -0.4, 1.0);
    r.fingers = D(0, 0, 1.0);    // wrist -> knuckles: up along the magazine
    r.across = D(0, 1.0, 0);     // pinky -> index: towards the front
    r.tip[gr::kIndexOff] = P(-w * 0.5 + 0.4, d * 0.5 + 0.6, L - 0.8);  // up the front edge to the top round
    r.tip[gr::kIndexRest] = r.tip[gr::kIndexOff];
    r.tip[gr::kIndexPull] = r.tip[gr::kIndexOff];
    r.tip[gr::kMiddle] = P(w * 0.5 + 0.7, 0.2, 4.0);  // curled round onto the right side
    r.tip[gr::kRing] = P(w * 0.5 + 0.7, -0.7, 3.0);
    r.tip[gr::kPinky] = P(w * 0.5 + 0.6, -1.3, 2.0);
    r.tip[gr::kThumb] = P(w * 0.5 + 0.7, 0.4, 0.55 * L);  // flat up the right side
    // Outline for the clip check, in the magazine's frame.
    FVector half = (hi - lo) * 0.5;
    half = {std::fabs(half.X), std::fabs(half.Y), std::fabs(half.Z)};
    r.outline = {{"magazine", (lo + hi) * 0.5, half, 0.0}};
    *rec = r;
    log::Info("body: magazine %s: %s %.1f x %.1f x %.1f cm (long x deep x wide), top along (%.0f %.0f %.0f), front "
              "along (%.0f %.0f %.0f) of the magazine; built-in hold from the owner's photos",
              ue::GetName(ue::GetClass(mag)).c_str(), measured ? "measured" : "not measurable, assumed", L, d, w,
              U.X, U.Y, U.Z, F.X, F.Y, F.Z);
    return true;
}

// The stored record for a grab: the built-in hold and/or the saved file, in
// the hand's handedness. false: there is none (the hand stays on the
// controller). `object` is only needed for magazines and old slide files.
// Poses saved before the joint ranges (2026-11-04) may bend joints in ways
// a hand can't: they are fitted to the ranges when loaded, keeping each
// finger's shape as near as the ranges allow. The file is only rewritten
// when the grip is saved in the editor; the first time a file needs fitting,
// a copy of it as it was is kept in grips\before-joint-limits\.
void LimitRecord(int side, const std::string& key, gr::Record* rec, const std::string* file_text) {
    if (!S.joint_limits || !G.rig[side].ok) return;
    static const char* joint_name[3] = {"base", "middle", "tip"};
    std::string what;
    double worst = 0;
    for (int t = 0; t < gr::kTipCount; ++t) {
        jl::FitReport rep = LimitPosedFinger(G.rig[side], rec, t);
        if (!rep.changed) continue;
        worst = std::max(worst, rep.moved_cm);
        std::string parts;
        for (int j = 0; j < 3; ++j) {
            if (rep.side_deg[j] < 1 && rep.twist_deg[j] < 1 && rep.flex_deg[j] < 1) continue;
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s%s: %.0f sideways, %.0f roll, %.0f curl", parts.empty() ? "" : "; ",
                          joint_name[j], rep.side_deg[j], rep.twist_deg[j], rep.flex_deg[j]);
            parts += buf;
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), " | %s moved %.1f cm", gr::TipName(t), rep.moved_cm);
        what += buf;
        if (!parts.empty()) what += " (deg taken out: " + parts + ")";
    }
    if (what.empty()) return;
    static std::set<std::string> logged;
    if (logged.insert(key + (side == kL ? "#L" : "#R")).second)
        log::Info("body: grip %s: posed fingers fitted to human joint ranges, furthest joint or fingertip moved %.1f cm%s",
                  key.c_str(), worst, what.c_str());
    if (!file_text) return;
    const std::wstring dir = ModPath(L"grips\\before-joint-limits");
    const std::wstring path = dir + L"\\" + Widen(key) + L".ini";
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    CreateDirectoryW(dir.c_str(), nullptr);
    if (WriteText(path, *file_text))
        log::Info("body: grip %s: the file as it was posed is kept in grips\\before-joint-limits\\%s.ini",
                  key.c_str(), key.c_str());
    else
        log::Error("body: could not copy grips\\%s.ini to grips\\before-joint-limits", key.c_str());
}

// ---- a magazine going into the magwell (stage 2) ----
// Which of the game's reload animations plays for a magazine: the pistol's
// for pistol magazines (BPC_Mag_HG_...); none for AR magazines
// (BPC_Mag_AR_...), which take the two poses only, unless
// mag_insert_anim_ar says otherwise; none for any other magazine. -1: none.
bool IsArMag(UObject mag) { return mag && ue::GetName(ue::GetClass(mag)).find("_AR_") != std::string::npos; }
bool IsPistolMag(UObject mag) { return mag && ue::GetName(ue::GetClass(mag)).find("_HG_") != std::string::npos; }
int InsertClip(UObject mag) {
    if (!S.mag_insert_anim || !mag) return -1;
    if (IsPistolMag(mag)) return reload_anim::kPistol;
    if (IsArMag(mag)) return S.mag_insert_ar_clip;
    return -1;
}
// Why InsertClip gave none (for the log).
const char* NoClipWhy(UObject mag) {
    if (!S.mag_insert_anim) return "the game's reload is off (mag_insert_anim=0)";
    if (IsArMag(mag)) return "AR magazines take your own poses only (mag_insert_anim_ar=off)";
    return "the game has no reload animation for this magazine";
}

// The game's own hold of a magazine while it reloads (BP_GunBase
// Gun_MagazineCreate): the magazine hangs from the hand socket
// HandItemPoint_L by its AttachPoint_ReloadHand, i.e. that point of the
// magazine sits on the socket. So the hand bone, in the magazine's frame, is
// the socket's place in the hand undone from that point. Fingers: the
// reload animation's pose where the insert begins. A left-hand record.
bool GameInsertHold(UObject mag, gr::Record* rec, std::string* why) {
    const HandRig& r = G.rig[kL];
    const int clip = InsertClip(mag);
    if (!r.ok) {
        *why = "the left hand is not rigged";
        return false;
    }
    if (clip < 0) {
        *why = NoClipWhy(mag);
        return false;
    }
    const reload_anim::ClipData& d = reload_anim::Get(static_cast<reload_anim::Clip>(clip));
    if (!d.ready || !d.have_socket) {
        *why = d.failed ? "the game's reload animation could not be read: " + d.why
                        : "the game's reload animation is not read yet";
        return false;
    }
    UObject point = 0;
    if (!ue::GetProp(mag, "AttachPoint_ReloadHand", point) || !point) {
        *why = "the magazine has no AttachPoint_ReloadHand";
        return false;
    }
    ue::Call rt(point, "GetRelativeTransform");
    if (!rt.Invoke()) {
        *why = "its AttachPoint_ReloadHand could not be read";
        return false;
    }
    Xf point_in_mag = rt.Result<Xf>();
    point_in_mag.scale = FVector{1, 1, 1};
    Xf socket_in_hand;
    socket_in_hand.rotation = d.socket_rot;
    socket_in_hand.translation = d.socket_pos;
    const Xf hand_in_socket = skel::Relative(Xf{}, socket_in_hand);
    const Xf hand = skel::Compose(hand_in_socket, point_in_mag);
    if (!std::isfinite(Length(hand.translation)) || Length(hand.translation) > 60.0) {
        *why = "the hold came out more than 60 cm from the magazine";
        return false;
    }
    *rec = gr::Record{};
    rec->right_hand = false;
    gr::StoreHand(rec, r.geo, Normalized(hand.rotation), hand.translation);
    for (int t = 0; t < gr::kTipCount; ++t) {
        const int f = TipFinger(t);
        fingers::Angles a;
        for (int j = 0; j < 3; ++j) {
            rec->joint[t][j] = Normalized(r.chain[f].ref[j].rotation.Inverse() * d.pose[0].joint[f][j]);
            a.extra[j] = rec->joint[t][j];
        }
        rec->posed[t] = true;
        Xf joints[3];
        FVector tip;
        fingers::Fk(r.chain[f], a, joints, &tip);
        rec->tip[t] = hand.translation + Normalized(hand.rotation).Rotate(tip);
    }
    return true;
}

bool GripRecord(int side, hands::GrabKind kind, UObject object, const std::string& key, gr::Record* out,
                bool* from_file) {
    std::string text;
    gr::Record rec;
    bool have = false;
    *from_file = false;
    // This hand's own pose if it has one, else the usual hand's (RecordKey).
    const std::string usual = gr::UsualKey(key);
    std::string file_key = key;
    // The reach for the holstered pistol (kPointHolster): the hand's own pose
    // only. The right hand uses its gun grip until it has one; the left hand
    // has none (the right hand's grip mirrored twisted the left arm, owner
    // 2026-11-07), so it stays on the controller until it is posed.
    const std::string holster_tail = ".holster";
    const bool holster = kind == hands::GrabKind::kGunGrip && usual.size() > holster_tail.size() &&
                         usual.compare(usual.size() - holster_tail.size(), holster_tail.size(), holster_tail) == 0;
    const std::string grip_key = holster ? usual.substr(0, usual.size() - holster_tail.size()) + ".grip" : key;
    if (kind == hands::GrabKind::kGunGrip && !(holster && side == kL)) {
        rec = gr::DefaultPistol();
        rec.name = key;
        have = true;
    }
    const bool is_mag = kind == hands::GrabKind::kMagHeld || kind == hands::GrabKind::kMagInsert;
    // No built-in magazine hold (the owner poses it in the editor): until a
    // grip is saved, the hand stays as it is on the controller, and the
    // editor starts from that.
    static const bool builtin_mag = config::GetBool("body_mag_builtin_hold", false);
    if (is_mag && builtin_mag && object && DefaultMagRecord(side, object, &rec)) {
        rec.name = key;
        have = true;
    }
    bool found = ReadText(RecordPath(key), &text);
    if (!found && holster) {
        if (side == kR) {
            found = ReadText(RecordPath(grip_key), &text);
            if (found) file_key = grip_key;
        } else {
            static std::set<std::string> told;
            if (told.insert(key).second)
                log::Info("body: %s not posed yet: the left hand stays on the controller near the holstered pistol "
                          "(to pose it: left hand on the pistol in its holster, hold left Y 1 s, pose it with the "
                          "right controller, tap Y to save)",
                          key.c_str());
        }
    }
    // The off hand on the pistol (BP_HG_C.offhand[-chest]-left): this
    // hand's other pose until both are posed, then the other hand's (the
    // gun is in the other hand too, so its pose mirrored fits).
    const bool offhand = kind == hands::GrabKind::kSupportGrip && gr::OffhandKey(usual);
    if (!found && offhand) {
        static std::set<std::string> told;
        bool mirrored = false;
        for (const gr::Fallback& f : gr::OffhandFallbacks(key)) {
            if (!ReadText(RecordPath(f.key), &text)) continue;
            found = true;
            file_key = f.key;
            mirrored = f.other_hand;
            break;
        }
        if (told.insert(key).second) {
            if (found)
                log::Info("body: %s not posed yet: using %s%s (to pose it: off hand on the pistol, hold left Y 1 s, "
                          "pose it with the other controller, tap Y to save)",
                          key.c_str(), file_key.c_str(), mirrored ? ", mirrored" : "");
            else
                log::Info("body: %s not posed yet: the off hand stays on its controller near the pistol (to pose "
                          "it: bring the off hand's controller against the gun hand's, hold left Y 1 s, pose it "
                          "with the other controller, tap Y to save)",
                          key.c_str());
        }
    }
    if (!found && usual != key && !holster && !offhand) {
        found = ReadText(RecordPath(usual), &text);
        if (found) {
            file_key = usual;
            static std::set<std::string> told;
            if (told.insert(key).second)
                log::Info("body: %s not posed yet: using %s, mirrored (tap Y in the grip editor to save this hand's "
                          "own pose as %s)",
                          key.c_str(), usual.c_str(), key.c_str());
        }
    }
    // The butt-strike pose (kPointButtStrike) not posed yet: this hand's gun
    // grip on that gun (its own, else the usual hand's mirrored), so the hand
    // looks as it did until the owner poses it.
    const std::string butt_tail = ".butt-strike";
    if (!found && kind == hands::GrabKind::kGunGrip && usual.size() > butt_tail.size() &&
        usual.compare(usual.size() - butt_tail.size(), butt_tail.size(), butt_tail) == 0) {
        const std::string grip_usual = usual.substr(0, usual.size() - butt_tail.size()) + ".grip";
        const std::string grip_own = gr::SideKey(grip_usual, side == kR, side == kR);
        for (const std::string& k : {grip_own, grip_usual}) {
            if (!ReadText(RecordPath(k), &text)) continue;
            found = true;
            file_key = k;
            break;
        }
        static std::set<std::string> told;
        if (told.insert(key).second)
            log::Info("body: %s not posed yet: using %s (to pose it: hold the gun in the butt-strike stance, main "
                      "hand by your head and the off hand up high, hold left Y 1 s, pose the main hand with the "
                      "other controller, tap Y to save)",
                      key.c_str(), found ? file_key.c_str() : "the built-in grip");
    }
    // A slide grip saved before there were three slide holds: use it for any
    // hold not posed yet.
    if (!found && kind == hands::GrabKind::kSlideGrab && object && usual.find(".charging-") == std::string::npos) {
        std::string cls = ue::GetName(ue::GetClass(object));
        found = ReadText(RecordPath(cls + ".slide"), &text);
        if (found) file_key = cls + ".slide";
    }
    // The left hand on a rifle's charging handle, not posed yet: a slide
    // hold posed on it before it had a pose per hand (owner, 2026-11-04
    // 04:56 UTC: BP_AR_C.slide-rear-pinch-behind), moved from the old grab
    // point to the new one so the hand stays where it was posed on the gun.
    FVector shift{};
    bool shifted = false;
    const std::string charging_left = ".charging-left";
    if (!found && kind == hands::GrabKind::kSlideGrab && object && side == kL && key.size() >= charging_left.size() &&
        key.compare(key.size() - charging_left.size(), charging_left.size(), charging_left) == 0 &&
        hands::ChargingHandleShift(object, &shift)) {
        std::string cls = ue::GetName(ue::GetClass(object));
        for (int h : {kSlideBehind, kSlideRear, kSlideOver, kSlideFront}) {
            const std::string old_key = cls + ".slide-" + kSlideHoldKey[h];
            if (!ReadText(RecordPath(old_key), &text)) continue;
            found = shifted = true;
            file_key = old_key;
            log::Info("body: %s not posed yet: using %s, posed on the old grab point (the grab point moved (%.1f "
                      "%.1f %.1f) cm in the gun frame; the hand stays where it was posed; tap Y in the grip editor "
                      "to save it as %s)",
                      key.c_str(), old_key.c_str(), shift.X, shift.Y, shift.Z, key.c_str());
            break;
        }
    }
    // A grip point not posed yet: the scope rail's end takes the pose behind
    // the light (the same hold on the handguard, a little further back);
    // any point takes the support grip saved before there were grip points.
    if (!found && kind == hands::GrabKind::kSupportGrip && object && usual.find(".support-") != std::string::npos) {
        std::string cls = ue::GetName(ue::GetClass(object));
        const std::string scope = std::string(".support-") + interact::PointKey(interact::kPointScopeRail);
        if (usual.size() >= scope.size() && usual.compare(usual.size() - scope.size(), scope.size(), scope) == 0) {
            found = ReadText(RecordPath(cls + ".support-" + interact::PointKey(interact::kPointLight)), &text);
            if (found) file_key = cls + ".support-" + interact::PointKey(interact::kPointLight);
        }
        if (!found) {
            found = ReadText(RecordPath(cls + ".support"), &text);
            if (found) file_key = cls + ".support";
        }
    }
    // A magazine lined up with the magwell, its stage 2 not posed yet: the
    // game's own reload hold, else the magazine's grab pose (stage 1).
    if (!found && kind == hands::GrabKind::kMagInsert && object) {
        static std::set<std::string> told;
        std::string why;
        if (S.mag_insert_game_hold && GameInsertHold(object, &rec, &why)) {
            have = true;
            if (told.insert(key + "#game").second)
                log::Info("body: %s not posed yet: the game's own reload hold (HandItemPoint_L on the magazine's "
                          "AttachPoint_ReloadHand); pose it in the grip editor while the magazine is lined up to "
                          "change it",
                          key.c_str());
        } else {
            const std::string tail = ".mag-insert";
            auto grab_key = [&](const std::string& k) {
                const size_t at = k.rfind(tail);
                return at == std::string::npos ? k : k.substr(0, at) + ".mag" + k.substr(at + tail.size());
            };
            for (const std::string& k : {grab_key(key), grab_key(usual)}) {
                if (ReadText(RecordPath(k), &text)) {
                    found = true;
                    file_key = k;
                    break;
                }
            }
            if (told.insert(key + "#grab").second)
                log::Info("body: %s not posed yet and %s: using the magazine's grab pose%s", key.c_str(),
                          S.mag_insert_game_hold ? why.c_str() : "the game's hold is off (mag_insert_game_hold=0)",
                          found ? (" (" + file_key + ")").c_str() : " (none saved: the hand stays on the controller)");
        }
    }
    if (found) {
        if (!have || is_mag) rec.right_hand = side == kR;
        if (gr::Parse(text, &rec)) {
            have = true;
            *from_file = true;
            if (shifted) {
                // Positions are relative to the grab point: the new point is
                // `shift` from the old one.
                rec.name = key;
                rec.knuckle = rec.knuckle - shift;
                for (FVector& t : rec.tip) t = t - shift;
                for (gr::OutlineBox& b : rec.outline) b.centre = b.centre - shift;
            }
            if (kind == hands::GrabKind::kGunGrip && gr::HasOldPistolHand(rec)) {
                // Saved before the hand was lined up with the barrel: keep the
                // fingertips, take the new hand and outline.
                gr::PistolHand(&rec);
                rec.outline = gr::DefaultPistol().outline;
                log::Info("body: grip %s had the old hand placement; moved the hand in line with the barrel "
                          "(your fingertip positions are kept; tap Y in the editor to save it)",
                          key.c_str());
            }
        } else {
            log::Error("body: grip file %s.ini could not be read, using the built-in grip", key.c_str());
        }
    }
    if (!have) return false;
    if (rec.right_hand != (side == kR)) rec = gr::Mirror(rec);
    // Saving from the editor writes this hand's own key, never the file read.
    rec.name = key;
    LimitRecord(side, file_key, &rec, *from_file ? &text : nullptr);
    *out = rec;
    return true;
}

void LoadGrip(int side, const hands::HandGrab& grab, bool from_disk = false) {
    ActiveGrip& g = G.grip[side];
    g = ActiveGrip{};
    g.object = grab.object;
    g.kind = grab.kind;
    g.point = grab.point;
    if (!S.grip_records || !G.rig[side].ok || !IsGrab(grab.kind) || !grab.object) return;
    g.key = RecordKey(side, grab);
    auto cached = g_grip_cache.find(CacheKey(side, g.key));
    if (!from_disk && cached != g_grip_cache.end()) {
        g = cached->second;
        g.object = grab.object;
        g.kind = grab.kind;
        g.point = grab.point;
        return;
    }
    gr::Record rec;
    bool from_file = false;
    if (!GripRecord(side, grab.kind, grab.object, g.key, &rec, &from_file)) return;
    g.rec = rec;
    g.from_file = from_file;
    // A long solve goes to a worker thread (see SolveJob); the editor's
    // "back to the saved grip" stays immediate.
    if (!from_disk && LongSolve(g.rec) && StartSolve(side, g)) {
        g.pending = true;
        return;
    }
    g.have = true;
    double t0 = perf::NowMs();
    SolveGrip(side, -1, false);
    char why[64];
    std::snprintf(why, sizeof(why), "solved in %.1f ms", perf::NowMs() - t0);
    ReportGrip(side, why);
    g_grip_cache[CacheKey(side, g.key)] = g;
}

// Saved gun grips solved ahead on the worker thread, once the hands are
// rigged (body setup), so the first draw finds them ready.
void PrewarmGrips() {
    if (!S.grip_records) return;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(ModPath(L"grips\\*.grip.ini").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    int started = 0;
    do {
        std::wstring file = fd.cFileName;
        const std::string base(file.begin(), file.end() - 4);  // drop ".ini"; grip files are named by class (ASCII)
        for (int side : {kR, kL}) {
            // The left hand's own gun grip (RecordKey), the right's mirrored until posed.
            const std::string key = side == kL ? base + "-left" : base;
            if (!G.rig[side].ok || g_grip_cache.count(CacheKey(side, key))) continue;
            ActiveGrip g;
            g.key = key;
            g.kind = hands::GrabKind::kGunGrip;
            if (!GripRecord(side, g.kind, 0, key, &g.rec, &g.from_file) || !LongSolve(g.rec)) continue;
            if (StartSolve(side, g)) ++started;
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    if (started) log::Info("body: solving %d saved gun grip(s) ahead, off the game thread", started);
}

// ---- grip editor ----
grip_editor::Editor g_editor;
int g_edit_side = -1;      // hand whose grip is edited
int g_edit_ctrl = -1;      // controller doing the editing
FVector g_tip_world[2][gr::kTipCount];
FVector g_palm_world[2];
// The "move the whole hand" handle: a ball off the back of the hand, clear
// of the finger joints (owner: "i need the ability to position the whole
// hand in relation to the gun"; the old palm ball sat among the joints and
// was never picked).
FVector g_handle_world[2];
Xf g_joint_world[2][hand_pose::kFingerCount][3];  // posed finger joints, world
Xf g_hand_world[2];
bool g_tips_valid[2] = {false, false};

// The visible EMPTY hand's pinch: the thumb tip and the index finger's
// middle knuckle (the owner's pinch: "side of the index's first knuckle"
// against the slide, thumb out), relative to the real controller's grip
// pose, from the last frame that hand was empty. Controls tests it against
// the slide's rear sight (2026-11-03: the controller's centre sits ~10 cm
// behind these when the hand comes in from behind the slide).
struct PinchNote {
    bool valid = false;
    FVector thumb, index;  // controller grip frame, cm
    double ms = 0;         // perf::NowMs when taken
};
PinchNote g_pinch[2];
hands::Pose g_raw_grip[2];  // controller grip before hand_walls moved the drawn hand
std::mutex g_pinch_mutex;
// What each visible hand touches things with (GetHandContacts), under g_pinch_mutex.
struct ContactNote {
    bool valid = false;
    double ms = 0;
    HandContacts c;
};
ContactNote g_contact[2];

void HideMarkers();
void StopEditor() {
    if (g_editor.Active()) log::Info("body: grip editor off");
    g_editor.Stop();
    g_pin.on = false;
    g_freeze.on = false;
    HideMarkers();
    g_edit_side = g_edit_ctrl = -1;
    hands::SetEditorHand(-1);
}

bool SaveGrip(int side) {
    ActiveGrip& g = G.grip[side];
    CreateDirectoryW(ModPath(L"grips").c_str(), nullptr);
    g.rec.name = g.key;
    if (!WriteText(RecordPath(g.key), gr::Serialize(g.rec))) {
        log::Error("body: could not write grips\\%s.ini", g.key.c_str());
        return false;
    }
    g.from_file = true;
    ReportGrip(side, "saved");
    g_grip_cache[CacheKey(side, g.key)] = g;
    return true;
}

// ---- editor grab points: small balls on the fingertips, palm and pointer ----
// Uses a sphere mesh the game already has loaded (engine basic shape), on
// plain StaticMeshComponents with no collision, shown only while editing.
// Balls: 15 finger joints (grip_editor item order), palm, object, pointer.
enum Marker : int { kMkPalm = grip_editor::kJointItems, kMkObject, kMkPointer, kMkCount };
struct EditorMarkers {
    bool tried = false;
    UObject mesh = 0;
    double mesh_size = 100;  // cm across at scale 1
    UObject owner = 0;
    UObject comp[kMkCount] = {};
    bool shown = false;
    bool object_shown = false;
} g_mk;

UObject FindMarkerMesh(double* size) {
    UObject sm_class = ue::FindObject("Class /Script/Engine.StaticMesh");
    if (!sm_class) return 0;
    // Best first: engine basic shapes, then any loaded sphere, then a cube.
    const char* exact[] = {"StaticMesh /Engine/BasicShapes/Sphere.Sphere", "StaticMesh /Engine/EngineMeshes/Sphere.Sphere",
                           "StaticMesh /Engine/BasicShapes/Cube.Cube"};
    UObject mesh = 0;
    for (const char* n : exact)
        if ((mesh = ue::FindObject(n))) break;
    if (!mesh) {
        UObject sphere = 0, cube = 0;
        ue::objects::ForEach([&](int, UObject o) {
            if (sphere || ue::GetClass(o) != sm_class) return;
            std::string n = Lower(ue::GetName(o));
            if (n == "sphere" || n == "sm_sphere" || n == "shape_sphere") sphere = o;
            else if (!cube && (n == "cube" || n == "sm_cube" || n == "shape_cube")) cube = o;
        });
        mesh = sphere ? sphere : cube;
    }
    if (!mesh) return 0;
    struct Box {  // FBox: Min, Max, IsValid (+ padding) = 56 bytes
        FVector min, max;
        uint8_t valid;
        uint8_t pad[7];
    };
    ue::Call bb(mesh, "GetBoundingBox");
    if (bb.Invoke()) {
        Box b = bb.Result<Box>();
        double s = std::fmax(b.max.X - b.min.X, std::fmax(b.max.Y - b.min.Y, b.max.Z - b.min.Z));
        if (s > 0.1 && s < 10000) *size = s;
    }
    log::Info("body: editor grab points use %s (%.0f cm)", ue::GetFullName(mesh).c_str(), *size);
    return mesh;
}

bool MakeMarkers(UObject player) {
    if (g_mk.owner == player && g_mk.comp[0]) return true;
    for (UObject& c : g_mk.comp) c = 0;  // a new player (level): make them again
    if (g_mk.tried && !g_mk.mesh) return false;
    if (!g_mk.tried) {
        g_mk.tried = true;
        g_mk.mesh = FindMarkerMesh(&g_mk.mesh_size);
        if (!g_mk.mesh) {
            log::Info("body: no sphere or cube mesh loaded, so the editor can't show grab points");
            return false;
        }
    }
    UObject cls = ue::FindObject("Class /Script/Engine.StaticMeshComponent");
    if (!cls || !player) return false;
    for (int i = 0; i < kMkCount; ++i) {
        ue::Call add(player, "AddComponentByClass");
        add.Set("Class", cls).Set("bManualAttachment", true).Set("RelativeTransform", Xf{}).Set("bDeferredFinish", false);
        UObject c = add.Invoke() ? add.Result<UObject>() : 0;
        if (!c) {
            log::Error("body: could not add the editor grab points");
            return false;
        }
        ue::Call col(c, "SetCollisionEnabled");
        col.Set("NewType", static_cast<uint8_t>(0));  // NoCollision
        col.Invoke();
        CallBool(c, "SetGenerateOverlapEvents", "bInGenerateOverlapEvents", false);
        CallBool(c, "SetCastShadow", "NewCastShadow", false);
        ue::Call sm(c, "SetStaticMesh");
        sm.Set("NewMesh", g_mk.mesh);
        sm.Invoke();
        SetVisible(c, false);
        g_mk.comp[i] = c;
    }
    g_mk.owner = player;
    g_mk.shown = false;
    return true;
}

void PlaceMarker(int i, const FVector& at, double diameter_cm) {
    Xf t;
    t.translation = at;
    double s = diameter_cm / g_mk.mesh_size;
    t.scale = FVector{s, s, s};
    ue::Call c(g_mk.comp[i], "K2_SetWorldTransform");
    c.Set("NewTransform", t).Set("bSweep", false).Set("bTeleport", true);
    c.Invoke();
}

void HideMarkers() {
    if (!g_mk.shown) return;
    if (g_mk.owner != G.player) {  // the level changed: those components are gone
        g_mk.shown = false;
        return;
    }
    for (UObject c : g_mk.comp)
        if (c) SetVisible(c, false);
    g_mk.shown = false;
}

// hover: the editor item a squeeze would take (or is holding).
void ShowMarkers(UObject player, int side, int hover, const FVector& pointer, const FVector* object) {
    if (!S.editor_markers || !MakeMarkers(player)) return;
    if (!g_mk.shown) {
        for (UObject c : g_mk.comp) SetVisible(c, true);
        g_mk.shown = true;
        g_mk.object_shown = true;
    }
    for (int m = 0; m < grip_editor::kJointItems; ++m) {
        bool hot = m == hover;
        PlaceMarker(m, g_joint_world[side][m / 3][m % 3].translation, hot ? S.joint_ball * 1.4 : S.joint_ball);
    }
    // The palm ball (move the hand) or, holding a magazine, the magazine ball.
    if (object) PlaceMarker(kMkObject, *object, hover == grip_editor::kObjectItem ? 4.5 : 2.5);
    else PlaceMarker(kMkPalm, g_handle_world[side], hover == grip_editor::kHandItem ? 5.0 : 3.5);
    if (g_mk.object_shown != (object != nullptr)) {
        g_mk.object_shown = object != nullptr;
        SetVisible(g_mk.comp[kMkObject], g_mk.object_shown);
        SetVisible(g_mk.comp[kMkPalm], !g_mk.object_shown);
    }
    PlaceMarker(kMkPointer, pointer, 1.5);
}

// The grip-record entry a finger's joints belong to (the index: the trigger
// state showing).
int FingerTip(int f, int index_state) {
    switch (f) {
        case hand_pose::kThumb: return gr::kThumb;
        case hand_pose::kIndex: return index_state;
        case hand_pose::kMiddle: return gr::kMiddle;
        case hand_pose::kRing: return gr::kRing;
        default: return gr::kPinky;
    }
}

const char* kFingerName[] = {"thumb", "index", "middle", "ring", "pinky"};
const char* kJointName[] = {"base", "middle", "tip"};

// Drag state for joints and the object (grip_editor handles the palm).
struct JointDrag {
    int finger = -1, joint = -1, tip = -1;
    bool at_limit = false;  // the joint is against the end of its human range
    FQuat joint_world0, ctrl_rot0;
    Xf obj_in_hand0, ctrl_in_hand0;
} g_drag;

// grab/hs: frozen while editing; live: the real controllers (the editing
// controller's grab point follows them).
void UpdateEditor(const hands::HandGrab grab[2], const hands::HandState hs[2], const hands::HandState live[2],
                  double dt) {
    const hands::EditButtons b = hands::GetEditButtons();
    // Which grip: the slide or charging handle first (right hand, then
    // left), then a magazine or the handguard (left hand, then right), then
    // a gun by its grip (right hand, then left). The other controller edits.
    // Either hand can hold either (controls' either_hand_guns); each hand's
    // pose is saved under its own name (RecordKey).
    // A hand snapped onto a pistol with its own pose (the left hand reaching
    // for the holstered pistol, kPointHolster; the off hand on the pistol in
    // the other hand, kPointOffhand*) comes before the gun grips, once it is
    // fully on it.
    int side = -1;
    const auto left_kind = hands::HeldGrabKind(grab[kL]), right_kind = hands::HeldGrabKind(grab[kR]);
    auto own_reach = [&](int s) {
        return OwnReach(grab[s]) && grab[s].object && grab[s].pose_blend > 0.95;
    };
    // The main hand in its butt-strike pose comes before the off hand on the
    // handguard (the off hand is the one up high, holding the gun).
    auto butt = [&](int s) { return grab[s].kind == hands::GrabKind::kGunGrip && grab[s].point == hands::kPointButtStrike; };
    if (g_editor.Active() && g_edit_side >= 0 && IsGrab(grab[g_edit_side].kind)) side = g_edit_side;
    else if (right_kind == hands::GrabKind::kSlideGrab) side = kR;
    else if (butt(kR)) side = kR;
    else if (butt(kL)) side = kL;
    else if (IsGrab(left_kind) && left_kind != hands::GrabKind::kGunGrip) side = kL;
    else if (IsGrab(right_kind) && right_kind != hands::GrabKind::kGunGrip) side = kR;
    else if (own_reach(kL)) side = kL;
    else if (own_reach(kR)) side = kR;
    else if (right_kind == hands::GrabKind::kGunGrip) side = kR;
    else if (left_kind == hands::GrabKind::kGunGrip) side = kL;
    if (g_editor.Active() && (side != g_edit_side || side < 0 || G.grip[side].object != grab[side].object)) {
        log::Info("body: grip editor stopped (the held object changed)");
        StopEditor();
    }
    if (side < 0 || !G.rig[side].ok || !g_tips_valid[side]) {
        if (g_editor.Active()) StopEditor();
        return;
    }
    const int ctrl = 1 - side;
    const bool is_mag = grab[side].kind == hands::GrabKind::kMagHeld || grab[side].kind == hands::GrabKind::kMagInsert;
    hands::Pose obj;
    if (!ObjectFrame(grab[side], GunGrab(grab, grab[side]), &obj) || !live[ctrl].valid) return;
    const FQuat inv = obj.rotation.Inverse();
    auto local = [&](const FVector& w) { return inv.Rotate(w - obj.location); };

    grip_editor::Input in;
    in.dt = dt;
    in.y_down = b.y_down;
    in.stick_click_pressed = b.left_stick_pressed;
    in.grab_down = b.grip_down[ctrl];
    // The grab point sits just ahead of the controller; when that hand holds a
    // gun it sits in front of the barrel, so the gun doesn't hide it or get in
    // the way.
    const double reach = hands::HeldGrabKind(grab[ctrl]) == hands::GrabKind::kGunGrip ? S.reach_gun : 3.0;
    const FVector pointer_w = live[ctrl].grip.location + live[ctrl].aim.rotation.Rotate(FVector{reach, 0, 0});
    in.pointer = local(pointer_w);
    in.pointer_rot = Normalized(inv * live[ctrl].grip.rotation);
    grip_editor::Scene sc;
    for (int m = 0; m < grip_editor::kJointItems; ++m) sc.joint[m] = local(g_joint_world[side][m / 3][m % 3].translation);
    sc.palm = local(g_handle_world[side]);
    ActiveGrip& g = G.grip[side];
    FVector object_w = obj.location;
    if (is_mag && !g.rec.outline.empty()) object_w = obj.location + obj.rotation.Rotate(g.rec.outline[0].centre);
    sc.has_palm = !is_mag;
    sc.has_object = is_mag;
    sc.object = local(object_w);
    sc.index_state = G.index_pull[side] > 0.5 ? gr::kIndexPull : G.index_touch[side] > 0.5 ? gr::kIndexRest
                                                                                             : gr::kIndexOff;
    FQuat hand_rot = g.hand_rot;
    FVector hand_pos = g.hand_pos;
    grip_editor::Output out = g_editor.Update(in, sc, &g.rec, &hand_rot, &hand_pos);
    const Xf hand_w = g_hand_world[side];  // last frame's posed hand
    if (out.entered) {
        g_freeze.on = true;
        for (int i = 0; i < 2; ++i) g_freeze.hs[i] = hs[i], g_freeze.grab[i] = grab[i];
        if (g_freeze.grab[side].holster_preview) {
            // Posing a snap (the reach for the holstered pistol, the off
            // hand on the pistol): show the pose itself (not the blend
            // toward it) while editing.
            g_freeze.grab[side].holster_preview = false;
            g_freeze.grab[side].pose_blend = 1.0;
            const int pt = grab[side].point;
            log::Info("body: grip editor: posing the %s hand's %s (%s)", side == kL ? "left" : "right",
                      pt == hands::kPointHolster ? "reach for the holstered pistol"
                      : pt == hands::kPointOffhandChest ? "hold on the pistol with the gun near the chest"
                      : "hold on the pistol with the gun held out",
                      RecordKey(side, grab[side]).c_str());
        }
        if (butt(side))
            log::Info("body: grip editor: posing the %s hand's butt-strike pose (%s)", side == kL ? "left" : "right",
                      RecordKey(side, grab[side]).c_str());
        log::Info("body: grip editor: hands and %s frozen in place; the %s controller's grab point is live",
                  is_mag ? "magazine" : "gun", ctrl == kR ? "right" : "left");
        g_edit_side = side;
        g_edit_ctrl = ctrl;
        hands::SetEditorHand(ctrl);
        if (!g.have) {
            // New record from the current hand: its pose and fingertips now.
            g.key = RecordKey(side, grab[side]);
            g.rec = gr::Record{};
            g.rec.name = g.key;
            g.rec.right_hand = side == kR;
            gr::StoreHand(&g.rec, G.rig[side].geo, Normalized(inv * hand_w.rotation), local(hand_w.translation));
            // Fingers exactly as they look now, joint by joint.
            for (int t = 0; t < gr::kTipCount; ++t) {
                g.rec.tip[t] = local(g_tip_world[side][t]);
                const int f = TipFinger(t);
                const fingers::Chain& c = G.rig[side].chain[f];
                for (int j = 0; j < 3; ++j)
                    g.rec.joint[t][j] = Normalized(c.ref[j].rotation.Inverse() * fingers::LocalRotation(c, j, G.cur[side][f]));
                g.rec.posed[t] = true;
                LimitPosedFinger(G.rig[side], &g.rec, t);
            }
            g.have = true;
            g.object = grab[side].object;
            g.kind = grab[side].kind;
            SolveGrip(side, -1, false);
        }
        if (is_mag && hs[side].valid) {
            g_pin.on = true;
            g_pin.side = side;
            g_pin.object = grab[side].object;
            g_pin.hand_in_ctrl = skel::Relative(hand_w, PoseXf(hs[side].grip));
        }
        log::Info("body: grip editor on for %s (edit with the %s controller: squeeze on a finger joint and turn the "
                  "controller to turn that joint; %s; tap Y to save, click the left stick to undo, hold Y 1 s to save "
                  "and finish)",
                  g.key.c_str(), ctrl == kL ? "left" : "right",
                  is_mag ? "squeeze the big ball on the magazine to move and turn the magazine in the hand"
                         : "squeeze the palm to move and turn the hand on the gun");
    }
    const HandRig& rig = G.rig[side];
    if (out.started && out.grabbed >= 0) {
        g_drag.finger = out.grabbed / 3;
        g_drag.joint = out.grabbed % 3;
        g_drag.tip = FingerTip(g_drag.finger, sc.index_state);
        g_drag.joint_world0 = g_joint_world[side][g_drag.finger][g_drag.joint].rotation;
        g_drag.ctrl_rot0 = live[ctrl].grip.rotation;
        g_drag.at_limit = false;
        // From now on this finger is posed joint by joint: start from what it shows.
        const int t = g_drag.tip;
        if (!g.rec.posed[t]) {
            const fingers::Chain& c = rig.chain[g_drag.finger];
            for (int j = 0; j < 3; ++j)
                g.rec.joint[t][j] = Normalized(c.ref[j].rotation.Inverse() * fingers::LocalRotation(c, j, g.tip_angles[t]));
            g.rec.posed[t] = true;
            LimitPosedFinger(rig, &g.rec, t);
        }
    }
    if (out.started && out.grabbed == grip_editor::kObjectItem) {
        Xf objx;
        objx.translation = obj.location;
        objx.rotation = obj.rotation;
        g_drag.obj_in_hand0 = skel::Relative(objx, hand_w);
        g_drag.ctrl_in_hand0 = skel::Relative(PoseXf(live[ctrl].grip), hand_w);
    }
    if (out.grabbed >= 0 && g_drag.tip >= 0) {
        // The joint turns with the controller (all three axes).
        const int f = g_drag.finger, n = g_drag.joint, t = g_drag.tip;
        const fingers::Chain& c = rig.chain[f];
        FQuat d = Normalized(live[ctrl].grip.rotation * g_drag.ctrl_rot0.Inverse());
        FQuat want_h = Normalized(hand_w.rotation.Inverse() * d * g_drag.joint_world0);
        fingers::Angles a;
        for (int j = 0; j < 3; ++j) a.extra[j] = g.rec.joint[t][j];
        Xf jx[3];
        fingers::Fk(c, a, jx, nullptr);
        FQuat parent = n == 0 ? FQuat{} : jx[n - 1].rotation;
        FQuat want_local = Normalized(parent.Inverse() * want_h);
        FQuat turn = Normalized(c.ref[n].rotation.Inverse() * want_local);
        if (S.joint_limits) {
            // Only the turns a real joint makes: the middle and tip joints
            // follow just the curl part of the controller's turn, the knuckle
            // also spreads a little, and every joint stops at the end of its
            // human range with a light tick.
            bool at_limit = false;
            turn = jl::Clamp(rig.axes[f][n], rig.limits[f][n], turn, &at_limit);
            if (at_limit && !g_drag.at_limit) out.haptic = std::max(out.haptic, 0.35f);
            g_drag.at_limit = at_limit;
        }
        g.rec.joint[t][n] = turn;
        SolveGrip(side, t, true);
    } else if (out.grabbed == grip_editor::kObjectItem) {
        // The magazine moves and turns with the controller, relative to the hand.
        Xf ctrl_h = skel::Relative(PoseXf(live[ctrl].grip), hand_w);
        FQuat d = Normalized(ctrl_h.rotation * g_drag.ctrl_in_hand0.rotation.Inverse());
        Xf o;
        o.rotation = Normalized(d * g_drag.obj_in_hand0.rotation);
        o.translation = ctrl_h.translation + d.Rotate(g_drag.obj_in_hand0.translation - g_drag.ctrl_in_hand0.translation);
        FQuat hr = o.rotation.Inverse();
        FVector hp = hr.Rotate(o.translation) * -1.0;
        gr::StoreHand(&g.rec, rig.geo, hr, hp);
        SolveGrip(side, -1, true);
    } else if (out.changed && out.grabbed == grip_editor::kHandItem) {
        gr::StoreHand(&g.rec, rig.geo, hand_rot, hand_pos);
        SolveGrip(side, -1, true);
    }
    if (out.grabbed < 0) g_drag.tip = -1;
    static int last_grab = grip_editor::kNoItem;
    if (out.grabbed != last_grab) {
        if (out.grabbed == grip_editor::kHandItem) log::Info("body: editor: moving the hand");
        else if (out.grabbed == grip_editor::kObjectItem) log::Info("body: editor: moving the magazine in the hand");
        else if (out.grabbed >= 0)
            log::Info("body: editor: turning the %s finger's %s joint", kFingerName[out.grabbed / 3],
                      kJointName[out.grabbed % 3]);
        else if (last_grab != grip_editor::kNoItem)
            log::Info("body: editor: hand knuckle at (%.1f %.1f %.1f) in the object, palm %.1f cm inside",
                      g.rec.knuckle.X, g.rec.knuckle.Y, g.rec.knuckle.Z, g.palm_clip);
        last_grab = out.grabbed;
    }
    if (out.revert) {
        LoadGrip(side, grab[side], true);
        if (!G.grip[side].have) StopEditor();
        log::Info("body: editor: back to the saved grip");
    }
    if (out.save) SaveGrip(side);
    if (out.exited) StopEditor();
    // Grab points: a ball on each joint, the palm (gun) or the magazine; the
    // one a squeeze would take grows, with a light tick when it changes.
    static int last_hover = grip_editor::kNoItem;
    if (g_editor.Active()) {
        ShowMarkers(G.player, side, out.hover, pointer_w, is_mag ? &object_w : nullptr);
        if (out.hover != last_hover && out.hover != grip_editor::kNoItem && out.grabbed == grip_editor::kNoItem &&
            out.haptic <= 0.f)
            out.haptic = 0.1f;
        last_hover = out.hover;
    } else {
        last_hover = grip_editor::kNoItem;
    }
    if (out.haptic > 0.f && ctrl >= 0)
        input::Haptic(ctrl == kL ? input::kLeft : input::kRight, out.haptic, 0.03f);
}

// ---- guided body calibration ----
std::atomic<bool> g_measure_requested{false}, g_calibration_active{false}, g_calibration_cancel{false};
std::atomic<bool> g_calibration_skip_arms{false};
std::atomic<unsigned> g_calibration_trigger_bits{0}; // L=1,R=2,valid=4
std::atomic<bool> g_calibration_trigger_interrupted{false};
std::atomic<bool> g_calibration_ramping{false};
calibration_hold::Hold g_calibration_trigger_hold;
double g_measure_core_time = 0, g_measure_haptic_ms = 0;
float g_measure_haptic_amplitude = -1;
std::mutex g_calibration_hud_mutex;
CalibrationHud g_calibration_hud;
double g_calibration_hud_until = 0;
body_calibration::Session g_calibration;
XrTime g_measure_sample_time = 0;
double g_measure_sample_seen_ms = 0;
bool g_measure_height_only = false;
double g_measure_scale = 1;
unsigned g_measure_attempt = 0;
body_calibration::Reason g_measure_reason = body_calibration::Reason::None;
double g_measure_reason_ms = 0;
double g_measure_trace_ms = 0;
UObject g_measure_player = 0;
FVector g_measure_fist[2], g_measure_wrist_offset[2], g_measure_position_setting[2];
FQuat g_measure_rotation_setting[2];
double g_measure_offsets[6] = {};
body_calibration::Phase g_measure_phase = body_calibration::Phase::Failed;
calibration_guidance::Action g_measure_guidance_action = calibration_guidance::Action::None;

void FreeHand(int side, const hands::HandState& h, const FVector& fist_at, FQuat* rot, FVector* pos);

void StopCalibrationHaptics() {
    g_calibration_ramping=false;
    input::StopHaptic(input::kLeft); input::StopHaptic(input::kRight);
    g_measure_haptic_ms = 0; g_measure_haptic_amplitude = -1;
}

void CalibrationFeedback(const calibration_hold::Feedback& feedback, double now) {
    if(feedback.stop) StopCalibrationHaptics();
    // Completion supersedes queued ramp pulses and cannot be dropped behind
    // them by the normal pulse budget. Both hands belong to this capture.
    if(feedback.completed) StopCalibrationHaptics();
    else if(feedback.ramp) g_calibration_ramping=true;
    if(feedback.completed || (feedback.ramp && (now-g_measure_haptic_ms>=45 || g_measure_haptic_amplitude<0))) {
        for(int h=0;h<2;++h) input::Haptic(static_cast<input::Hand>(h),feedback.amplitude,feedback.completed?.16f:.25f);
        g_measure_haptic_ms=now; g_measure_haptic_amplitude=feedback.amplitude;
    }
}

void CalibrationText(const char* title, const char* a, const char* b = "", const char* c = "",
                     int progress = 0, double seconds = 2.0) {
    std::lock_guard<std::mutex> lock(g_calibration_hud_mutex);
    g_calibration_hud = {};
    g_calibration_hud.visible = true;
    std::snprintf(g_calibration_hud.title, sizeof(g_calibration_hud.title), "%s", title);
    for (int i = 0; i < 3; ++i) {
        const char* text = i == 0 ? a : i == 1 ? b : c;
        std::snprintf(g_calibration_hud.line[i], sizeof(g_calibration_hud.line[i]), "%s", text);
    }
    g_calibration_hud.progress = std::clamp(progress, 0, 100);
    g_calibration_hud_until = perf::NowMs() + seconds * 1000;
}

void EndCalibration(const char* reason) {
    g_calibration_active = false;
    input::SetCalibrationCapture(false);
    g_measure_requested = false;
    g_calibration_cancel = false;
    g_calibration_skip_arms = false;
    g_calibration_trigger_bits = 0; g_calibration_trigger_interrupted = false;
    g_calibration_trigger_hold.Reset(); StopCalibrationHaptics();
    g_calibration.Reset();
    CalibrationText("Body setup stopped", reason, "Previous fit kept.", "Retry in VR Options.", 0, 7);
    log::Warn("body calibration: %s; previous fit retained", reason);
}

bool SaveCalibration(const body_calibration::Profile& profile) {
    const std::string serialized = body_calibration::Serialize(profile);
    if (serialized.empty()) return false;
    const std::wstring destination = ModPath(L"body-calibration.ini");
    const std::wstring temporary = ModPath(L"body-calibration.pending");
    if (!WriteText(temporary, serialized)) return false;
    // Preserve the last successful fit; replace the live file atomically.
    if (GetFileAttributesW(destination.c_str()) != INVALID_FILE_ATTRIBUTES &&
        !CopyFileW(destination.c_str(), ModPath(L"body-calibration.previous.ini").c_str(), FALSE)) return false;
    return MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

void StepMeasure(double /*dt*/) {
    const double now = perf::NowMs();
    // Body setup starts only from the VR tab (Calibrate body). Holding the
    // left stick click 2 s used to start it too, but that click also sprints
    // (or kicks with move_stick=right), so a held sprint mid-level started
    // setup and took every button (2026-11-08 button-overlap check).
    if (g_calibration_cancel.exchange(false)) {
        EndCalibration("Cancelled.");
        return;
    }
    if (g_calibration_active && (ui_panel::MenuOpen() || g_editor.Active() || g_measure_player != G.player)) {
        EndCalibration("Paused or player changed.");
        return;
    }
    if (g_measure_requested && !ui_panel::MenuOpen()) {
        g_measure_requested = false;
        if (g_editor.Active() || !G.rig[0].frame.valid || !G.rig[1].frame.valid ||
            !G.skel.arms_ok[0] || !G.skel.arms_ok[1]) {
            EndCalibration("Close the grip editor first.");
            return;
        }
        for (int h = 0; h < 2; ++h) {
            // The preparation stage can wait for an occluded controller.
            // Only actual held objects prevent entering setup.
            if (hands::HasHeldObject(hands::GetGrab(static_cast<hands::Hand>(h)),
                                    hands::GetInteraction(static_cast<hands::Hand>(h)),
                                    workbench::HoldsGrip(h), drinks::HoldsGrip(h))) {
                EndCalibration("Put down held items first.");
                return;
            }
        }
        g_calibration.Reset();
        g_calibration_skip_arms = false;
        g_calibration_trigger_bits = 0; g_calibration_trigger_interrupted = false;
        g_calibration_trigger_hold.Reset(); StopCalibrationHaptics();
        g_measure_core_time = 0;
        g_measure_height_only = false;
        g_calibration_active = true;
        input::SetCalibrationCapture(true);
        g_measure_phase = body_calibration::Phase::Failed;
        g_measure_guidance_action = calibration_guidance::Action::None;
        g_measure_player = G.player;
        g_measure_scale = stereo::TrackingScale();
        g_measure_attempt = 0;
        g_measure_reason = body_calibration::Reason::None;
        g_measure_reason_ms = 0;
        g_measure_trace_ms = 0;
        g_measure_sample_time = 0;
        g_measure_sample_seen_ms = now;
        g_measure_fist[0] = G.rig[0].frame.fist;
        g_measure_fist[1] = G.rig[1].frame.fist;
        for (int a = 0; a < 3; ++a) {
            g_measure_offsets[a] = S.hand_offset[a];
            g_measure_offsets[a + 3] = S.hand_rot[a];
        }
        if (!std::isfinite(g_measure_scale) || g_measure_scale <= 0) {
            EndCalibration("Tracking scale unavailable."); return;
        }
        // Freeze the positional mapping separately from the new visual wrist
        // basis. Recalibration must never feed its own corrected rotation back
        // into the shoulder/length observations.
        for (int h = 0; h < 2; ++h) {
            hands::HandState identity;
            identity.grip.rotation = FQuat{};
            identity.palm_normal = FVector{0, h == kL ? 1.0 : -1.0, 0};
            FQuat unused;
            FVector wrist;
            FreeHand(h, identity, FVector{}, &unused, &wrist);
            g_measure_wrist_offset[h] = wrist * (1.0 / g_measure_scale);
            g_measure_position_setting[h] = FVector{S.hand_offset[0], S.hand_offset[1], S.hand_offset[2]};
            const double mirror = h == kL ? -1.0 : 1.0;
            g_measure_rotation_setting[h] = ToQuat({S.hand_rot[0], S.hand_rot[1]*mirror, S.hand_rot[2]*mirror});
            const auto& d = g_measure_wrist_offset[h];
            log::Info("body calibration: %s frozen wrist offset %.4f,%.4f,%.4f physical cm; setting game cm %.3f,%.3f,%.3f; "
                      "manual rotation %.5f,%.5f,%.5f,%.5f", h == kL ? "left" : "right", d.X,d.Y,d.Z,
                      g_measure_position_setting[h].X,g_measure_position_setting[h].Y,g_measure_position_setting[h].Z,
                      g_measure_rotation_setting[h].X,g_measure_rotation_setting[h].Y,g_measure_rotation_setting[h].Z,g_measure_rotation_setting[h].W);
        }
        log::Info("body calibration: comfortable height -> forward -> out -> single elbow bend -> independent forward check; hold BOTH index triggers for the 2s measurement, release both between steps; X keeps arms, B/Y cancels");
    }
    if (!g_calibration_active) return;
    perf::Scope calibration_timer(g_perf_calibration);
    bool changed = std::fabs(stereo::TrackingScale() - g_measure_scale) > 1e-4;
    for (int h = 0; h < 2; ++h) changed = changed || Length(G.rig[h].frame.fist - g_measure_fist[h]) > 0.001;
    for (int a = 0; a < 3; ++a) changed = changed || S.hand_offset[a] != g_measure_offsets[a] ||
        S.hand_rot[a] != g_measure_offsets[a + 3];
    if (changed) { EndCalibration("Hand mapping changed. Retry."); return; }

    const input::CalibrationTracking raw = input::CalibrationSample();
    bool fresh = raw.head_valid && raw.time > 0;
    if (fresh && raw.time < g_measure_sample_time) { EndCalibration("Tracking clock changed. Retry."); return; }
    bool repeated = false;
    if (fresh && raw.time == g_measure_sample_time) {
        // Still service trigger releases immediately on a repeated pose. The
        // Session receives its previous timestamp so the pose isn't counted
        // twice. A fresh hold waits for the next actual tracking observation.
        repeated = now-g_measure_sample_seen_ms<=150.0;
        if(!repeated) fresh=false;
    } else if (fresh) {
        g_measure_sample_time = raw.time;
        g_measure_sample_seen_ms = now;
    }
    const auto before_capture=g_calibration.GetStatus();
    const unsigned trigger_bits=g_calibration_trigger_bits.load();
    calibration_hold::Buttons trigger_input;
    trigger_input.active=true;
    trigger_input.valid=(trigger_bits&4)!=0 && fresh &&
        (before_capture.phase==body_calibration::Phase::MeasureHeight || raw.valid);
    trigger_input.left=(trigger_bits&1)!=0; trigger_input.right=(trigger_bits&2)!=0;
    trigger_input.interrupted=g_calibration_trigger_interrupted.exchange(false) || g_calibration_skip_arms.load();
    trigger_input.can_start=!repeated;
    trigger_input.step=(std::uint64_t(before_capture.attempt)<<8)|unsigned(before_capture.phase);
    const auto admission=g_calibration_trigger_hold.Begin(trigger_input);
    if(admission.stop) StopCalibrationHaptics();
    body_calibration::Sample sample;
    sample.seconds = repeated && g_measure_core_time>0 ? g_measure_core_time : now / 1000.0;
    sample.origin_generation = stereo::OriginGeneration();
    sample.head_valid = fresh;
    sample.valid = fresh && raw.valid;
    sample.grip_orientation_valid = sample.valid;
    for (int h = 0; h < 2; ++h) {
        sample.wrist_offset_grip_cm[h] = g_measure_wrist_offset[h];
        sample.hand_position_setting_game_cm[h] = g_measure_position_setting[h];
        sample.hand_rotation_setting[h] = g_measure_rotation_setting[h];
    }
    for (int h=0;h<2;++h) {
        // Keep an existing center for the position fit's unobserved directions.
        // A previously estimated/legacy split is not fresh anatomical evidence:
        // use the avatar proportion for a new fit unless that side has explicit
        // measurement provenance. This does not alter the active saved profile.
        const auto& arm=M.profile.arm[h];
        const double sum=arm.upper_cm+arm.forearm_cm;
        if(M.calibrated && sum>0) {
            sample.prior_shoulder[h]=arm.shoulder; sample.prior_reach_cm[h]=sum;
        }
        if(M.calibrated && M.profile.version>=5 && arm.segments_measured && sum>0 &&
           body_calibration::PlausibleSplit(arm.upper_cm,arm.forearm_cm)) {
            sample.prior_forearm_fraction[h]=arm.forearm_cm/sum;
        } else if(G.arm_ref[h].valid) {
            const auto& frame=G.arm_ref[h];
            const double total=frame.upper_length+frame.forearm_length;
            if(total>0) sample.prior_forearm_fraction[h]=frame.forearm_length/total;
        }
    }
    sample.floor_known = xr::StandingSpace();
    sample.height_mode = CurrentHeightMode();
    sample.height_posture = body_calibration::DefaultHeightPosture(sample.height_mode);
    sample.skip_arms = g_calibration_skip_arms.exchange(false);
    sample.confirm_pose = admission.start;
    sample.capture_held = admission.held;
    if (sample.skip_arms) g_measure_height_only = true;
    if (sample.head_valid) {
        // All three poses were located at the same time with active TRACKED
        // bits. Raw tracking space excludes camera bob and fixed-height modes.
        const auto& head = raw.head;
        sample.head = XrToUePosition(head.position.x, head.position.y, head.position.z, 100.0);
        const auto& q = head.orientation;
        sample.head_rotation = XrToUeQuat(q.x, q.y, q.z, q.w);
    }
    if (sample.valid) {
        for (int h = 0; h < 2; ++h) {
            const auto& p = raw.grip[h];
            const FVector position = XrToUePosition(p.position.x, p.position.y, p.position.z, 100.0);
            sample.grip_rotation[h] = XrToUeQuat(p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w);
            sample.wrist[h] = position + sample.grip_rotation[h].Rotate(g_measure_wrist_offset[h]);
        }
    }
    const auto status = g_calibration.Update(sample);
    g_measure_core_time=sample.seconds;
    // Bounded, calibration-only observations make a rejected real-world path
    // reproducible. The normal log writer batches these away from the tick.
    if (now - g_measure_trace_ms >= 100.0 || sample.skip_arms || sample.confirm_pose || status.phase != g_measure_phase) {
        log::Info("body calibration sample: t=%.3f phase=%u attempt=%u origin=%llu tracked=%d/%d floor=%d skip=%d confirm=%d confirmed=%d held=%d accepted=%d mode=%u posture=%u "
                  "head=%.3f,%.3f,%.3f q=%.5f,%.5f,%.5f,%.5f left=%.3f,%.3f,%.3f right=%.3f,%.3f,%.3f "
                  "left-grip-q=%.5f,%.5f,%.5f,%.5f right-grip-q=%.5f,%.5f,%.5f,%.5f bends=%u span=%.1f residual=%.2f",
                  sample.seconds, static_cast<unsigned>(status.phase), status.attempt,
                  static_cast<unsigned long long>(sample.origin_generation), sample.head_valid, sample.valid,
                  sample.floor_known, sample.skip_arms, sample.confirm_pose, status.pose_confirmed, sample.capture_held,status.capture_accepted,
                  static_cast<unsigned>(sample.height_mode), static_cast<unsigned>(sample.height_posture),
                  sample.head.X, sample.head.Y, sample.head.Z,
                  sample.head_rotation.X, sample.head_rotation.Y, sample.head_rotation.Z, sample.head_rotation.W,
                  sample.wrist[0].X, sample.wrist[0].Y, sample.wrist[0].Z,
                  sample.wrist[1].X, sample.wrist[1].Y, sample.wrist[1].Z,
                  sample.grip_rotation[0].X,sample.grip_rotation[0].Y,sample.grip_rotation[0].Z,sample.grip_rotation[0].W,
                  sample.grip_rotation[1].X,sample.grip_rotation[1].Y,sample.grip_rotation[1].Z,sample.grip_rotation[1].W,
                  status.bend_samples,status.bend_span_degrees,status.bend_fit_error_cm);
        g_measure_trace_ms = now;
    }
    if (status.attempt != g_measure_attempt) {
        if (g_measure_attempt) {
            log::Info("body calibration: restarting in setup, attempt %u: %s (head %.1f cm / %.1f deg)",
                      status.attempt, body_calibration::ReasonText(status.reason),
                      status.head_motion_cm, status.head_rotation_deg);
        }
        g_measure_attempt = status.attempt;
    }
    if (status.reason != body_calibration::Reason::None && status.reason != g_measure_reason &&
        now - g_measure_reason_ms >= 1000.0) {
        log::Info("body calibration: pose guidance: %s (attempt %u, %.1f s, head %.1f cm / %.1f deg, %u arc samples)",
                  body_calibration::ReasonText(status.reason), status.attempt, status.elapsed_seconds,
                  status.head_motion_cm, status.head_rotation_deg, status.arc_samples);
        g_measure_reason_ms = now;
    }
    g_measure_reason = status.reason;
    using Phase = body_calibration::Phase;
    if (status.phase != g_measure_phase) {
        const auto previous_phase = g_measure_phase;
        g_measure_phase = status.phase;
        log::Info("body calibration: %s", body_calibration::Prompt(status.phase, status.height_posture));
        if (previous_phase == Phase::MeasureHeight)
            log::Info("body calibration: height step %s, posture %u, floor-to-eye %.1f cm", status.height_measured ? "accepted" : "kept previous height", static_cast<unsigned>(status.height_posture), status.measured_eye_cm);
    }
    CalibrationPose diagram = CalibrationPose::None;
    const auto guide=calibration_guidance::ForStatus(status);
    if(guide.valid && status.phase!=Phase::Complete && status.phase!=Phase::Failed) {
        using Picture=calibration_guidance::Illustration;
        switch(guide.illustration) {
            case Picture::Height:
                diagram=status.height_posture==body_calibration::HeightPosture::Seated ? CalibrationPose::SeatedHeight :
                    status.height_posture==body_calibration::HeightPosture::Standing ? CalibrationPose::Height : CalibrationPose::None;
                break;
            case Picture::Forward: diagram=CalibrationPose::Forward; break;
            case Picture::Out: diagram=CalibrationPose::Out; break;
            case Picture::Raising: diagram=CalibrationPose::BendUp; break;
            case Picture::Bent: diagram=CalibrationPose::Bent; break;
            case Picture::None: break;
        }
        CalibrationText(guide.title,guide.line[0],guide.line[1],guide.line[2],guide.progress);
        if(guide.action!=g_measure_guidance_action) {
            g_measure_guidance_action=guide.action;
            log::Info("body calibration: card phase=%u action=%u pose=%d confirmed=%d bent=%d degraded=%d reason=%u side=%d rms=%.2f drift=%.2f angle=%.2f verify=%.2f bend-reasons=%u/%u",
                static_cast<unsigned>(status.phase),static_cast<unsigned>(guide.action),status.pose_ready,status.pose_confirmed,
                status.bend_pose_bent,status.bend_degraded,static_cast<unsigned>(status.reason),status.rejected_side,
                status.pose_rms_cm,status.pose_drift_cm,status.orientation_rms_deg,status.verification_error_cm,
                static_cast<unsigned>(status.bend_reason[0]),static_cast<unsigned>(status.bend_reason[1]));
        }
    }
    bool terminal_success=false;
    switch (status.phase) {
        case Phase::MeasureHeight: case Phase::ArmsForward: case Phase::ArmsOut:
        case Phase::BendElbows: case Phase::VerifyForward: break;
        case Phase::Failed:
            EndCalibration(body_calibration::ReasonText(status.reason));
            if(guide.valid) CalibrationText(guide.title,guide.line[0],guide.line[1],guide.line[2],0,9);
            break;
        case Phase::Complete: {
            const auto* result = g_calibration.GetProfile();
            if (!result) { EndCalibration("No completed measurement was available."); break; }
            const bool arms_measured = status.arms_measured && result->arms_measured;
            const bool height_measured = status.height_measured && std::isfinite(status.measured_eye_cm) && status.measured_eye_cm > 0;
            const bool standing_measured = height_measured && status.height_posture == body_calibration::HeightPosture::Standing;
            body_calibration::Profile saved;
            const auto* previous = body_calibration::Validate(M.profile) ? &M.profile : nullptr;
            if ((arms_measured || height_measured) &&
                (!body_calibration::MergeProfile(previous, *result, &saved) || !SaveCalibration(saved))) {
                EndCalibration("Could not save the new fit.");
                break;
            }
            terminal_success=true; // saved successfully, or an explicitly unchanged result
            g_calibration_active = false;
            input::SetCalibrationCapture(false);
            g_calibration_skip_arms = false;
            g_calibration_trigger_bits = 0; g_calibration_trigger_interrupted = false;
            if (!arms_measured && !height_measured) {
                CalibrationText("Existing fit kept", sample.height_mode == body_calibration::HeightMode::Fixed ? "Character height stays fixed." : "No new height reference recorded.", "Arm poses were skipped.", "Nothing was changed.", 0, 9);
                log::Info("body calibration: arms skipped, no new posture reference; existing profile unchanged");
                break;
            }
            M.profile = saved;
            M.calibrated = saved.arms_measured;
            G.posture_state[0] = {}; G.posture_state[1] = {};
            G.twist_state[0] = {}; G.twist_state[1] = {};
            legs::Reset(G.legs);
            if (standing_measured) {
                M.standing_eye_cm = result->standing_eye_cm;
                char height[32]; std::snprintf(height, sizeof(height), "%.2f", M.standing_eye_cm);
                if (!config::Set("standing_height_cm", height)) log::Warn("body calibration: height is active but ini save failed");
                config::NotifyChanged();
                stereo::SetStandingEyeHeight(M.standing_eye_cm / 100.0);
            }
            for (int h = 0; h < 2; ++h) {
                if (arms_measured) {
                    const auto& arm = M.profile.arm[h];
                    log::Info("body calibration: %s shoulder offset %.2f %.2f %.2f cm, upper %.2f forearm %.2f cm, "
                          "arc %.1f deg RMS %.2f cm plane %.2f cm bent error %.2f cm (%u/%u inliers) shoulder-fit=%d pivot-dimensions=%u forearm-fit=%d segment-fit=%d verify=%.2f",
                          h == 0 ? "left" : "right", arm.shoulder.X, arm.shoulder.Y, arm.shoulder.Z,
                          arm.upper_cm, arm.forearm_cm, arm.fit.arc_degrees, arm.fit.radial_rms_cm,
                          arm.fit.plane_rms_cm, arm.fit.bent_error_cm, arm.fit.inliers, arm.fit.samples, arm.shoulder_measured,
                          arm.shoulder_observed_dimensions, arm.forearm_measured, arm.segments_measured, arm.verification_error_cm);
                    const auto& w = arm.wrist;
                    log::Info("body calibration: %s wrist measured=%d forearm-axis=(%.5f,%.5f,%.5f) palm-axis=(%.5f,%.5f,%.5f) "
                              "orientation RMS forearm/palm=%.2f/%.2f max=%.2f deg; bend inliers=%u/%u span=%.1f spread=%.2f cm",
                              h == kL ? "left" : "right",w.measured,w.forearm_axis_grip.X,w.forearm_axis_grip.Y,w.forearm_axis_grip.Z,
                              w.palm_axis_grip.X,w.palm_axis_grip.Y,w.palm_axis_grip.Z,w.forearm_rms_deg,w.palm_rms_deg,w.max_error_deg,
                              arm.fit.bend_inliers,arm.fit.bend_samples,arm.fit.bend_span_degrees,arm.fit.bend_length_spread_cm);
                }
            }
            char height[64];
            if (height_measured) std::snprintf(height, sizeof(height), "%s reference saved: %.0f cm.", standing_measured ? "Standing" : "Seated", status.measured_eye_cm);
            else std::snprintf(height, sizeof(height), "Existing height references kept.");
            log::Info("body calibration: saved %s; standing %.1f seated %.1f legacy %.1f cm (mode %u posture %u)",
                      arms_measured ? "new arm fit" : "height only, existing arms kept", M.profile.standing_eye_cm,
                      M.profile.seated_eye_cm, M.profile.legacy_eye_cm,
                      static_cast<unsigned>(sample.height_mode), static_cast<unsigned>(sample.height_posture));
            const bool any_forearm = arms_measured && (saved.arm[0].forearm_measured || saved.arm[1].forearm_measured);
            CalibrationText(arms_measured ? "Body fit saved" : "Height saved - arms kept", height,
                            arms_measured ? (status.reach_degraded ? "Some shoulder directions estimated." : "Shoulder fit saved.") : "Your current arm fit is unchanged.",
                            arms_measured ? (status.bend_degraded ? (any_forearm ? "Forearm fit saved; other lengths estimated." : "Arm proportions remain estimates.") : "Arm lengths and wrist fit saved.") : "Setup finished.", 100, 9);
            break;
        }
    }
    // A full-strength pulse acknowledges actual acceptance, and the final
    // pulse waits for a successful save. Rejected or cancelled holds stop.
    const bool accepted=status.capture_accepted && (status.phase!=Phase::Complete || terminal_success);
    CalibrationFeedback(g_calibration_trigger_hold.Observe(status.pose_confirmed,status.hold_fraction,accepted),now);
    if(!g_calibration_active) g_calibration_trigger_hold.Reset(); // do not stop a successful completion pulse
    if (g_calibration_active) {
        std::lock_guard<std::mutex> lock(g_calibration_hud_mutex);
        g_calibration_hud.pose = diagram;
        std::snprintf(g_calibration_hud.footer, sizeof(g_calibration_hud.footer), "%s",
                      g_calibration_trigger_hold.NeedsRelease() ? "Release BOTH triggers before the next hold" :
                      g_measure_height_only ? "Hold both triggers 2s  |  Arms skipped  |  B/Y: cancel" :
                          "Hold triggers 2s  |  X: keep arms  |  B/Y: cancel");
        // ForStatus owns every instruction row; no per-frame error or
        // stopwatch message is allowed to contradict the chosen action.

    }
}

// ---- solving ----
FVector YawDir(double yaw_deg) {
    double r = yaw_deg * kDegToRad;
    return {std::cos(r), std::sin(r), 0};
}

// Applies finger angles to the pose (joints are children of the hand bone).
void PoseFingers(int side, const fingers::Angles a[hand_pose::kFingerCount]) {
    const Skeleton& k = G.skel;
    const HandRig& r = G.rig[side];
    for (int f = 0; f < hand_pose::kFingerCount; ++f)
        for (int n = 0; n < 3; ++n) {
            Xf l = r.chain[f].ref[n];
            l.rotation = fingers::LocalRotation(r.chain[f], n, a[f]);
            G.pose.SetL(k.finger[side][f][n], l);
        }
}

// An empty hand on its controller (fist_at: where the fist goes).
void FreeHand(int side, const hands::HandState& h, const FVector& fist_at, FQuat* rot, FVector* pos) {
    const HandRig& rig = G.rig[side];
    if (M.calibrated && M.profile.arm[side].wrist.measured && rig.ok) {
        const auto& fit = M.profile.arm[side];
        const double scale = stereo::TrackingScale();
        if (std::isfinite(scale) && scale > 0) {
            const double mirror = side == kL ? -1.0 : 1.0;
            const FQuat manual = ToQuat({S.hand_rot[0], S.hand_rot[1]*mirror, S.hand_rot[2]*mirror});
            wrist_calibration::Mapping mapping;
            // fist_at retains the existing interaction anchor for grabs and
            // slides. Its rotation is the uncorrected tracked/slide grip.
            mapping.controller_world = fist_at;
            mapping.grip_world = h.grip.rotation;
            mapping.tracking_scale = scale;
            mapping.forearm_axis_hand = rig.forearm_dir;
            mapping.palm_axis_hand = rig.palm_h;
            mapping.position_delta_cm = (FVector{S.hand_offset[0], S.hand_offset[1], S.hand_offset[2]}
                                         - fit.hand_position_setting_game_cm) * (1.0/scale);
            mapping.rotation_delta_grip = Normalized(manual * fit.hand_rotation_setting.Inverse());
            if (wrist_calibration::Map(fit.wrist, mapping, pos, rot)) return;
        }
    }
    const FVector fist = fist_at + h.grip.rotation.Rotate(FVector{S.hand_offset[0], S.hand_offset[1], S.hand_offset[2]});
    const FVector fwd = h.grip.rotation.Rotate(FVector{1, 0, 0});
    FQuat hand_rot = rig.frame.valid ? HandRotation(rig.frame, fwd, h.palm_normal) : h.grip.rotation;
    if (S.hand_rot[0] || S.hand_rot[1] || S.hand_rot[2]) {
        double mirror = side == kL ? -1.0 : 1.0;
        hand_rot = Normalized(h.grip.rotation *
                              ToQuat(FRotator{S.hand_rot[0], S.hand_rot[1] * mirror, S.hand_rot[2] * mirror}) *
                              h.grip.rotation.Inverse() * hand_rot);
    }
    *rot = hand_rot;
    *pos = rig.frame.valid ? fist - hand_rot.Rotate(rig.frame.fist) : fist;
}

// Build the palm collision volume from the same calibrated mapping used to
// draw the empty hand. The controller origin is not the palm centre.
hand_motion::PalmShape CollisionPalm(int side, const hands::HandState& h) {
    hand_motion::PalmShape shape;
    const auto& rig = G.rig[side];
    if (!h.valid || !rig.ok) return shape;
    FQuat rotation; FVector wrist;
    FreeHand(side, h, h.grip.location, &rotation, &wrist);
    Xf hand = WX(G.skel.hand[side]);
    hand.rotation = rotation; hand.translation = wrist;
    const auto point = [&](const FVector& p) { return skel::Apply(hand, p); };
    const FVector origin = point({});
    if (!std::isfinite(Length(hand.scale)) || std::fabs(hand.scale.X) < 1e-6 ||
        std::fabs(hand.scale.Y) < 1e-6 || std::fabs(hand.scale.Z) < 1e-6) return shape;
    // Plane normals use the inverse transpose, including nonuniform scale.
    const FVector local_normal{rig.palm_h.X / hand.scale.X, rig.palm_h.Y / hand.scale.Y,
                               rig.palm_h.Z / hand.scale.Z};
    const FVector palm = Normalize(hand.rotation.Rotate(local_normal));
    const FVector forward = Normalize(Reject(point(rig.geo.fingers) - origin, palm));
    const double thickness = 1.5 / Length(local_normal);
    const FVector centre = point(rig.palm_centre) - palm * thickness;
    const double width = Length(point(rig.chain[hand_pose::kIndex].ref[0].translation) -
                                point(rig.chain[hand_pose::kPinky].ref[0].translation));
    const double length = Length(point(rig.chain[hand_pose::kMiddle].ref[0].translation) - origin);
    const double across_scale = Length(point(rig.across_h) - origin);
    const FQuat inverse = h.grip.rotation.Inverse();
    shape.centre = inverse.Rotate(centre - h.grip.location);
    shape.rotation = Normalized(inverse * AlignFrames({0,0,1}, {0,1,0}, palm, forward));
    shape.half = {width * 0.5 + 0.8 * across_scale, length * 0.45, thickness};
    shape.flatten = S.press_flat;
    shape.press_cm = S.press_push;
    shape.valid = std::isfinite(Length(shape.centre)) && std::isfinite(Length(shape.half)) &&
                  shape.half.X > 0.1 && shape.half.Y > 0.1 && shape.half.Z > 0.1;
    return shape;
}

// ---- built-in slide holds (owner's description, 2026-11-02) ----
// Front and rear overhand: hand flat, the thumb's pad against the slide.
// Rear pinch: a fist with the thumb straight out, the side of the index
// finger's first knuckle against the slide. The hand keeps the angle the
// controller had when it grabbed; it is only moved so the contact point
// touches the slide's surface, found with a line trace against the slide part.
// Unsaved: the grip editor fine-tunes it and Y saves it as usual.
// The slide's own parts. GunParts_Bolt(Slide) itself is a plain scene
// component (log: making it traceable failed); its meshes are its children.
// They get query-only collision that ignores every channel, so nothing in
// the game can hit them but K2_LineTraceComponent (which tests one component
// and never the world) can.
std::vector<UObject> SlideParts(UObject slide) {
    static UObject prim = ue::FindObject("Class /Script/Engine.PrimitiveComponent");
    std::vector<UObject> out;
    if (!prim || !slide) return out;
    std::vector<UObject> all{slide};
    ue::Call ch(slide, "GetChildrenComponents");
    ch.Set("bIncludeAllDescendants", true);
    if (ch.Invoke()) {
        int32_t size = 0;
        const uint8_t* raw = ch.Raw("Children", &size);
        TArrayRaw arr{};
        if (raw && size >= (int32_t)sizeof(TArrayRaw)) std::memcpy(&arr, raw, sizeof(arr));
        for (int k = 0; k < arr.num && k < 32; ++k) {
            UObject c = mem::ReadOr<uintptr_t>(arr.data + k * sizeof(uintptr_t), 0);
            if (c) all.push_back(c);
        }
    }
    for (UObject c : all) {
        if (!ue::reflection::IsA(c, prim)) continue;
        uint8_t coll = 0;
        ue::Call ce(c, "GetCollisionEnabled");
        if (ce.Invoke()) coll = ce.Result<uint8_t>();
        if (coll == 0) {
            ue::Call rr(c, "SetCollisionResponseToAllChannels");
            rr.Set("NewResponse", static_cast<uint8_t>(0));  // ECR_Ignore
            ue::Call e(c, "SetCollisionEnabled");
            e.Set("NewType", static_cast<uint8_t>(1));  // QueryOnly
            rr.Invoke();
            e.Invoke();
        }
        out.push_back(c);
    }
    return out;
}

bool TraceParts(const std::vector<UObject>& parts, const FVector& a, const FVector& b, FVector* hit) {
    double best = 1e30;
    for (UObject c : parts) {
        ue::Call t(c, "K2_LineTraceComponent");
        if (!t.Valid()) continue;
        t.Set("TraceStart", a).Set("TraceEnd", b).Set("bTraceComplex", true);
        if (!t.Invoke() || !t.Result<bool>()) continue;
        FVector h = t.Get<FVector>("HitLocation");
        double d = Length(h - a);
        if (d < best) best = d, *hit = h;
    }
    return best < 1e29;
}

// The slide measured once per gun with rays down onto its top along the
// centre line: rear and front end and the rear sight (the highest point of
// the rear 3.5 cm). Stored in the slide component's space, so it follows
// the slide as it moves.
//
// The rays are game traces, about 0.08 ms each, one per slide part (11 on
// the pistol) per spot: 121 spots every 0.5 cm took about 110 ms in one
// frame when the gun was first drawn (owner 2026-11-03: "the game hitches
// when i grab my gun for the first time"). Now a 2 cm pass finds the slide,
// then a 0.5 cm pass covers it from 1.5 cm past each end (every spot the old
// scan could hit), and the work is spread over frames, about 1 ms each,
// while the gun is in the hand. A slide grab before it is done finishes it
// at once. Spots are kept as (along the barrel, up) from the slide origin
// in the gun's axes, which doesn't change as the gun moves.
constexpr int kSlideSpots = 121;  // x = -30 .. +30 cm in 0.5 cm steps
double SlideSpotX(int i) { return -30.0 + 0.5 * i; }

struct SlideShape {
    UObject gun = 0, slide = 0;
    bool tried = false, ok = false;  // tried: measuring finished (ok or not)
    bool started = false;
    std::vector<UObject> parts;
    FVector sight, rear, front;  // slide component space
    // While measuring:
    std::vector<int> todo;          // spots still to trace
    bool fine = false;              // the 0.5 cm pass is queued
    std::vector<char> traced;       // per spot
    std::vector<double> top;        // per spot: height of the hit, NaN = no hit
    double ms = 0, last_ms = 0;
    int rays = 0, frames = 0;
} g_shape;

bool MeasureSlideStart(UObject gun) {
    g_shape = SlideShape{};
    g_shape.gun = gun;
    g_shape.started = true;
    if (!gun || !ue::GetProp(gun, "GunParts_Bolt(Slide)", g_shape.slide) || !g_shape.slide) {
        g_shape.tried = true;
        return false;
    }
    g_shape.parts = SlideParts(g_shape.slide);
    if (g_shape.parts.empty()) {
        log::Warn("body: slide measure: the slide has no mesh parts to trace");
        g_shape.tried = true;
        return false;
    }
    g_shape.traced.assign(kSlideSpots, 0);
    g_shape.top.assign(kSlideSpots, std::nan(""));
    for (int i = kSlideSpots - 1; i >= 0; i -= 4) g_shape.todo.push_back(i);  // 2 cm pass, taken from the back
    return true;
}

// budget_ms < 0: finish now. Returns true once measured.
bool MeasureSlide(UObject gun, const FQuat& gun_rot, double budget_ms = -1) {
    if (g_shape.gun == gun && g_shape.tried) return g_shape.ok;
    if (g_shape.gun != gun || !g_shape.started) {
        if (!MeasureSlideStart(gun)) return false;
    } else {
        // Picked up again after a while, or no longer the same slide (a gun
        // freed and another made at the same address): start over rather
        // than trace parts that may be gone.
        UObject slide = 0;
        if (perf::NowMs() - g_shape.last_ms > 2000.0 || !ue::GetProp(gun, "GunParts_Bolt(Slide)", slide) ||
            slide != g_shape.slide) {
            if (!MeasureSlideStart(gun)) return false;
        }
    }
    const double t0 = perf::NowMs();
    g_shape.last_ms = t0;
    ++g_shape.frames;
    const Xf sw = ComponentToWorld(g_shape.slide);
    const FVector X = gun_rot.Rotate(FVector{1, 0, 0}), Z = gun_rot.Rotate(FVector{0, 0, 1});
    int done_now = 0;
    for (;;) {
        if (g_shape.todo.empty()) {
            if (g_shape.fine) break;
            g_shape.fine = true;
            int lo = kSlideSpots, hi = -1;
            for (int i = 0; i < kSlideSpots; ++i)
                if (!std::isnan(g_shape.top[i])) lo = std::min(lo, i), hi = std::max(hi, i);
            if (hi < 0) break;
            lo = std::max(0, lo - 3), hi = std::min(kSlideSpots - 1, hi + 3);
            for (int i = hi; i >= lo; --i)
                if (!g_shape.traced[i]) g_shape.todo.push_back(i);
            continue;
        }
        if (budget_ms >= 0 && done_now > 0 && perf::NowMs() - t0 >= budget_ms) {
            g_shape.ms += perf::NowMs() - t0;
            return false;
        }
        const int i = g_shape.todo.back();
        g_shape.todo.pop_back();
        const double x = SlideSpotX(i);
        FVector a = sw.translation + X * x + Z * 15.0, b = sw.translation + X * x - Z * 15.0, h;
        g_shape.traced[i] = 1;
        g_shape.rays += static_cast<int>(g_shape.parts.size());
        ++done_now;
        if (TraceParts(g_shape.parts, a, b, &h)) g_shape.top[i] = Dot(h - sw.translation, Z);
    }
    g_shape.ms += perf::NowMs() - t0;
    g_shape.tried = true;
    double rear = 1e9, front = -1e9;
    std::vector<std::pair<double, double>> top;  // x along the barrel from the slide origin, z
    for (int i = 0; i < kSlideSpots; ++i) {
        if (std::isnan(g_shape.top[i])) continue;
        const double x = SlideSpotX(i);
        top.push_back({x, g_shape.top[i]});
        rear = std::min(rear, x);
        front = std::max(front, x);
    }
    if (top.size() < 6 || front - rear < 5.0 || front - rear > 50.0) {
        log::Warn("body: slide measure: %zu spots hit %zu part(s); no slide shape", top.size(),
                  g_shape.parts.size());
        return false;
    }
    double sx = rear, sz = -1e9;
    for (auto& [x, z] : top)
        if (x <= rear + 3.5 && z > sz) sx = x, sz = z;
    double rz = top.front().second, fz = top.back().second;
    auto to_local = [&](double x, double z) {
        Xf pt;
        pt.translation = sw.translation + X * x + Z * z;
        return skel::Relative(pt, sw).translation;
    };
    g_shape.sight = to_local(sx, sz);
    g_shape.rear = to_local(rear, rz);
    g_shape.front = to_local(front, fz);
    g_shape.ok = true;
    log::Info("body: slide measured (%zu parts): %.1f cm long, rear end %.1f / front end %.1f cm along the barrel "
              "from the slide origin, rear sight at %.1f, %.1f cm above the origin (%d rays over %d frame(s), "
              "%.1f ms in all)",
              g_shape.parts.size(), front - rear, rear, front, sx, sz, g_shape.rays, g_shape.frames, g_shape.ms);
    return true;
}

bool SlideMarksWorld(UObject gun, const FQuat& gun_rot, FVector* sight, FVector* rear, FVector* front,
                     double budget_ms) {
    if (!MeasureSlide(gun, gun_rot, budget_ms)) return false;
    const Xf sw = ComponentToWorld(g_shape.slide);
    if (sight) *sight = skel::Apply(sw, g_shape.sight);
    if (rear) *rear = skel::Apply(sw, g_shape.rear);
    if (front) *front = skel::Apply(sw, g_shape.front);
    return true;
}

bool BuildSlidePreset(int side, const hands::HandState& h, const hands::Pose& obj, const hands::HandGrab& grab) {
    static const bool enabled = config::GetBool("hand_slide_builtin_holds", true);
    const HandRig& rig = G.rig[side];
    if (!enabled || !rig.ok || !grab.object) return false;
    MeasureSlide(grab.object, obj.rotation);
    const UObject slide = g_shape.slide;
    if (!slide || g_shape.parts.empty()) return false;
    const int hold = g_slide_hold[side];
    const bool pinch = hold == kSlideRear || hold == kSlideBehind;
    // Hand where the controller grabbed, in the object frame.
    FQuat hand_rot;
    FVector hand_pos;
    FreeHand(side, h, h.grip.location, &hand_rot, &hand_pos);
    const FQuat inv = obj.rotation.Inverse();
    FQuat rot_o = Normalized(inv * hand_rot);
    FVector pos_o = inv.Rotate(hand_pos - obj.location);
    // Fingers: flat, or a fist; the thumb straight either way.
    gr::Record rec;
    rec.name = RecordKey(side, grab);
    rec.right_hand = side == kR;
    FVector contact_h{};  // hand space
    for (int t = 0; t < gr::kTipCount; ++t) {
        const int f = TipFinger(t);
        const fingers::Chain& c = rig.chain[f];
        const bool thumb = f == hand_pose::kThumb;
        fingers::Angles a = fingers::Curl(c, pinch && !thumb ? 1.0 : 0.0, fingers::DefaultLimits(thumb));
        for (int j = 0; j < 3; ++j) rec.joint[t][j] = Normalized(c.ref[j].rotation.Inverse() * fingers::LocalRotation(c, j, a));
        rec.posed[t] = true;
        Xf js[3];
        FVector tip;
        fingers::Fk(c, a, js, &tip);
        rec.tip[t] = tip;  // hand space for now
        if (!pinch && thumb) contact_h = (js[2].translation + tip) * 0.5;  // thumb pad
        if (pinch && t == gr::kIndexRest) contact_h = js[1].translation + rig.across_h * 0.9;  // side of the index knuckle
    }
    // Move the hand so the contact point sits on the slide: trace toward the
    // slide's centre line (gun X through the slide part's origin).
    const Xf sw = ComponentToWorld(slide);
    FVector cw = obj.location + obj.rotation.Rotate(pos_o + rot_o.Rotate(contact_h));
    const FVector axis = obj.rotation.Rotate(FVector{1, 0, 0});
    FVector on_line = sw.translation + axis * Dot(cw - sw.translation, axis);
    FVector d = on_line - cw;
    const double dist = Length(d);
    bool snapped = false;
    FVector hit{};
    if (dist > 0.1) {
        d = d * (1.0 / dist);
        snapped = TraceParts(g_shape.parts, cw - d * 8.0, on_line, &hit);
        if (snapped) {
            static const double pad = config::GetFloat("hand_slide_contact_skin_cm", 0.6f);
            pos_o = pos_o + inv.Rotate(hit - d * pad - cw);
        }
    }
    gr::StoreHand(&rec, rig.geo, rot_o, pos_o);
    for (int t = 0; t < gr::kTipCount; ++t) rec.tip[t] = pos_o + rot_o.Rotate(rec.tip[t]);
    ActiveGrip& g = G.grip[side];
    g.key = rec.name;
    g.rec = rec;
    g.have = true;
    g.from_file = false;
    SolveGrip(side, -1, false);
    log::Info("body: slide %s hold: built-in pose (%s), contact %s on the slide%s", kSlideHoldName[hold],
              pinch ? "fist, thumb out, index knuckle on the slide" : "flat hand, thumb pad on the slide",
              snapped ? "placed" : "NOT placed (the trace missed the slide; hand left where it grabbed)",
              snapped ? "" : "");
    return true;
}

// An empty hand pushed against a wall, table or door settles flat on it
// (owner 2026-11-03 14:14: the fingers "would close and make a fist, in real
// life people tend to press their hand against the wall"). Reworked after
// the owner's hand-physics reference (SentientArt/TheBasement, ragdoll hands
// sprung toward the controller): the hand keeps the controller's angle until
// it really touches the surface (its ball is stopped, or its fingertips
// reach it), then turns flat as the real hand goes on toward it, flat once
// it is press_push cm past where the ball stops. Only a palm or
// fingertips-first touch turns it: the back of the hand, or its side (a
// karate chop), keeps its angle. The palm is set on the surface, but never
// pulled toward it from further than the real hand has gone past the drawn
// one. Returns how flat, 0..1.
// h: the hand as drawn (after the wall check).
constexpr double kNoTouch = 1e9;
bool g_was_flat[2] = {false, false};      // for the log: flat last frame
double g_flat[2] = {0.0, 0.0};            // eased flatness
double g_touch_d[2] = {kNoTouch, kNoTouch};  // real fist height above the surface when it touched, this touch

double PressFlat(int side, const hands::HandState& h, FQuat* rot, FVector* pos, double dt) {
    double& eased = g_flat[side];
    double& touch_d = g_touch_d[side];
    const HandRig& rig = G.rig[side];
    FVector at, out;
    const hands::HandState real = hands::Get(side == kL ? hands::kLeft : hands::kRight);
    if (!S.press_flat || !rig.ok || !real.valid || !hand_walls::Contact(side, &at, &out)) {
        touch_d = kNoTouch;
        return eased = 0.0;
    }
    const double d = Dot(real.grip.location - at, out);  // the real fist centre's height above the surface
    // Remember where it touched; forget once the hand is pulled 2 cm back off it.
    if (hand_walls::Touching(side)) {
        if (touch_d == kNoTouch) touch_d = d;
    } else if (touch_d != kNoTouch && d > touch_d + 2.0) {
        touch_d = kNoTouch;
    }
    const FVector palm = Normalize(rot->Rotate(rig.palm_h));
    const FVector across = Normalize(rot->Rotate(rig.across_h));
    const FVector into = out * -1.0;
    // From where it touched (the fingertips may touch 15 cm before the fist
    // does) to press_push past where the hand's ball stops at the surface.
    const double flat_at = hand_walls::Radius() - S.press_push;
    double target =
        touch_d == kNoTouch ? 0.0 : std::clamp((touch_d - d) / std::max(0.5, touch_d - flat_at), 0.0, 1.0);
    // The back of the hand (palm 100-115 deg or more off the surface) and the
    // side of it (across the knuckles pointing into the surface) keep their
    // angle; faded, so the hand never pops between the two.
    const double facing = Dot(palm, into);
    const double c100 = std::cos(100.0 * kDegToRad), c115 = std::cos(115.0 * kDegToRad);
    target *= std::clamp((facing - c115) / (c100 - c115), 0.0, 1.0);
    target *= std::clamp((0.85 - std::fabs(Dot(across, into))) / 0.25, 0.0, 1.0);
    // Ease in (about 0.07 s), so the hand turns rather than snaps; let go at once.
    eased = target < eased ? target : eased + (target - eased) * (1.0 - std::exp(-dt / 0.07));
    const double w = eased;
    if (w <= 0.001) return 0.0;
    // Log the first 10 presses (flat after not being flat).
    static int logged = 0;
    const bool now_flat = w >= 0.99;
    if (now_flat && !g_was_flat[side] && logged < 10) {
        ++logged;
        log::Info("body: %s hand pressed flat on a surface (palm turned %.0f deg to face it; it touched with the real "
                  "fist %.1f cm off the surface, now %.1f cm)",
                  side == kL ? "left" : "right", std::acos(std::clamp(facing, -1.0, 1.0)) * kRadToDeg, touch_d, d);
    }
    g_was_flat[side] = now_flat;
    *rot = Normalized(Scale(Arc(palm, into), w) * *rot);
    // The palm's middle onto the surface (0.3 cm off it), never into it, and
    // pulled toward it no further than the wall check held the hand back.
    constexpr double kSkin = 0.3;
    const FVector c = *pos + rot->Rotate(rig.palm_centre);
    const double dc = Dot(c - at, out);
    const double held = std::max(0.0, Dot(h.grip.location - real.grip.location, out));
    const double shift = dc < kSkin ? kSkin - dc : -std::min((dc - kSkin) * w, held);
    *pos = *pos + out * shift;
    return w;
}

// Fingers against a surface, from frame to frame: each finger moves from
// where it was (a sprung finger can't jump through a wall).
struct FingerWall {
    bool on = false;  // the hand was against a surface last frame
    FVector last_hand{0, 0, 0};
    double bend[hand_pose::kFingerCount] = {};
    int dir[hand_pose::kFingerCount] = {};
    double drag_bend[hand_pose::kFingerCount] = {}, drag_spread[hand_pose::kFingerCount] = {};
};
FingerWall g_finger_wall[2];
finger_contact::State g_finger_contact[2][hand_pose::kFingerCount];
bool g_precise_fingers[2]{};
perf::Counter g_perf_finger_contact{"body individual finger contacts"};

void ResetFingerContacts(int side) {
    finger_world::Reset(side);
    for (auto& state : g_finger_contact[side]) finger_contact::Reset(state);
    g_finger_wall[side] = {};
    g_precise_fingers[side] = false;
    g_flat[side] = 0;
    g_touch_d[side] = kNoTouch;
    g_was_flat[side] = false;
}

void ResetHandPhysics(int side) {
    hand_walls::Reset(side);
    for (int s = 0; s < 2; ++s) {
        if (side >= 0 && side != s) continue;
        ResetFingerContacts(s);
        G.twist_state[s].support = {};
        G.cur_init[s] = false;
        g_tips_valid[s] = false;
        std::lock_guard<std::mutex> lock(g_pinch_mutex);
        g_pinch[s] = {};
        g_contact[s] = {};
    }
}

bool FreeContactAllowed(int side, const hands::HandGrab& grab) {
    const auto interaction = hands::GetInteraction(side == kL ? hands::kLeft : hands::kRight);
    return grab.kind == hands::GrabKind::kNone && interaction.actions_allowed &&
           interaction.owner == hands::GripOwner::kNone && !g_freeze.on && !CalibrationActive();
}

finger_contact::Bounds FingerBounds(const HandRig& rig, int finger) {
    finger_contact::Bounds bounds;
    for (int j = 0; j < 3; ++j) {
        bounds.axes[j] = rig.axes[finger][j];
        bounds.limits[j] = rig.limits[finger][j];
    }
    return bounds;
}

// Report the entire observation interval: a single frame every five seconds
// hid contacts and centimetres of unresolved clipping during playtests.
void NotePreciseFingers(int side, const finger_world::Stats& queries, int contacts,
                        double residual, double self_residual, double angular_step, double ik_gap) {
    struct Interval {
        double start_ms = 0, worst_world = 0, worst_self = 0, worst_angle = 0, worst_ik_gap = 0;
        int frames = 0, contact_frames = 0, unavailable = 0, resets = 0;
        int casts = 0, displayed = 0, temporal = 0, expired = 0, failures = 0, budget = 0;
        int primary_hits = 0, broad_misses = 0, recovered = 0, recovery_failed = 0, conflicts = 0;
    };
    static Interval totals[2];
    auto& n = totals[side];
    const double now = perf::NowMs();
    if (!n.start_ms) n.start_ms = now;
    ++n.frames;
    n.contact_frames += contacts > 0;
    n.unavailable += !queries.available;
    n.resets += queries.reset;
    n.casts += queries.casts; n.displayed += queries.displayed_casts; n.temporal += queries.temporal_casts;
    n.expired += queries.expired; n.failures += queries.failures; n.budget += queries.budget_limited;
    n.primary_hits += queries.primary_hits; n.broad_misses += queries.broad_false_clear;
    n.recovered += queries.recovered_overlaps; n.recovery_failed += queries.failed_recovery;
    n.conflicts += queries.conflicting_contacts;
    n.worst_world = std::max(n.worst_world, residual);
    n.worst_self = std::max(n.worst_self, self_residual);
    n.worst_angle = std::max(n.worst_angle, angular_step * kRadToDeg);
    n.worst_ik_gap = std::max(n.worst_ik_gap, ik_gap);
    if (now - n.start_ms < 5000) return;
    log::Info("hand contacts %s interval: frames=%d touching=%d queries=%d displayed=%d temporal=%d "
              "hits=%d coarse_misses=%d overlap_recovered=%d recovery_failed=%d conflicts=%d "
              "expired=%d failures=%d budget_frames=%d unavailable=%d resets=%d "
              "worst_world=%.3f cm worst_self=%.3f cm max_joint_step=%.2f deg max_ik_gap=%.3f cm",
              side == kL ? "left" : "right", n.frames, n.contact_frames, n.casts, n.displayed, n.temporal,
              n.primary_hits, n.broad_misses, n.recovered, n.recovery_failed, n.conflicts,
              n.expired, n.failures, n.budget, n.unavailable, n.resets,
              n.worst_world, n.worst_self, n.worst_angle, n.worst_ik_gap);
    n = {}; n.start_ms = now;
}

// Log line for fingertips dragging: the first 6 times in a run that a tip
// trailed 1 cm or more, at most every 3 s per hand.
void NoteFingerDrag(int side, double speed, double trail) {
    static int logged = 0;
    static double last_ms[2] = {-1e12, -1e12};
    if (trail < 1.0 || logged >= 6 || perf::NowMs() - last_ms[side] < 3000.0) return;
    ++logged;
    last_ms[side] = perf::NowMs();
    log::Info("fingers: %s fingertips drag along the surface (hand sliding %.0f cm/s, a tip trails %.1f cm)",
              side == kL ? "left" : "right", speed, trail);
}

// Log lines for fingers giving way on a wall: the first 10 times in a run
// that the wall changed a finger's bend noticeably, at most every 2 s per hand.
struct FingerRestNote {
    bool any = false;
    double want[hand_pose::kFingerCount] = {}, rest[hand_pose::kFingerCount] = {};
};
FingerRestNote g_rest_note[2];
int g_rest_logged = 0;
double g_rest_last_ms[2] = {-1e12, -1e12};

void NoteFingerRest(int side, int f, double want, double rest) {
    FingerRestNote& n = g_rest_note[side];
    n.want[f] = want;
    n.rest[f] = rest;
    if (std::fabs(rest - want) >= 0.15) n.any = true;
}

void FlushFingerRest(int side) {
    FingerRestNote& n = g_rest_note[side];
    if (n.any && g_rest_logged < 10 && perf::NowMs() - g_rest_last_ms[side] > 2000.0) {
        ++g_rest_logged;
        g_rest_last_ms[side] = perf::NowMs();
        static const char* names[hand_pose::kFingerCount] = {"thumb", "index", "middle", "ring", "pinky"};
        std::string list;
        for (int f = 0; f < hand_pose::kFingerCount; ++f) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%s%s %.2f->%.2f", list.empty() ? "" : ", ", names[f], n.want[f],
                          n.rest[f]);
            list += buf;
        }
        log::Info("fingers: %s hand's fingers give way against the surface (bend wanted->used, below 0 = bent back "
                  "past straight: %s)",
                  side == kL ? "left" : "right", list.c_str());
    }
    n = FingerRestNote{};
}

// Which rear pinch record to draw. Owner 2026-11-03 14:14: "i couldn't pinch
// the slide no matter what i did". His 08:02 run: every grab caught, but the
// one saved rear pinch was posed with the hand over the top of the slide
// (forearm 81 deg right, 64 deg up from the barrel), and he reaches in from
// behind the slide with the palm toward the muzzle. The drawn hand jumped
// 15-20 cm and turned 80-130 deg onto the slide, and he let go within 0.2 s:
// 3 of 48 such grabs racked, against 12 of 21 that turned under 60 deg.
// Half-Life: Alyx's pistol slide (vr_alyxgun_slide_anim_interact) has two
// authored hand poses, each with its own grab point: a pinch at the rear
// serrations with the wrist behind the slide and the fingers toward the
// muzzle (vr_alyxgun_slide_anim, bone "interact"), and an overhand grab at the
// front (vr_alyxgun_slide_anim_alt, bone "interact1"). So the rear pinch has
// two records here: posed from above (key rear-pinch, the owner's first save)
// and from behind (rear-pinch-behind). The saved one the hand turns least to
// reach is drawn. One that would turn it more than hand_slide_pose_max_turn_deg
// is not used; then the from-behind pinch, until it is posed, is the built-in
// fist with the thumb out at the hand's own angle (BuildSlidePreset), so the
// hand never flips onto the slide.
int PickRearPinch(int side, const hands::HandState& h, const hands::Pose& obj, const hands::HandGrab& grab) {
    static const double max_turn = config::GetFloat("hand_slide_pose_max_turn_deg", 60.f);
    const HandRig& rig = G.rig[side];
    if (!rig.ok) return kSlideRear;
    FQuat free_rot;
    FVector free_pos;
    FreeHand(side, h, h.grip.location, &free_rot, &free_pos);
    const int kinds[2] = {kSlideRear, kSlideBehind};
    bool saved[2] = {false, false};
    double turn[2] = {0.0, 0.0};
    for (int i = 0; i < 2; ++i) {
        g_slide_hold[side] = kinds[i];
        const std::string key = RecordKey(side, grab);
        gr::Record rec;
        auto cached = g_grip_cache.find(CacheKey(side, key));
        if (cached != g_grip_cache.end() && cached->second.from_file) {
            rec = cached->second.rec;
            saved[i] = true;
        } else {
            bool from_file = false;
            saved[i] = GripRecord(side, hands::GrabKind::kSlideGrab, grab.object, key, &rec, &from_file) && from_file;
        }
        if (!saved[i]) continue;
        FQuat rot_o;
        FVector pos_o;
        gr::HandInObject(rec, rig.geo, &rot_o, &pos_o);
        const FQuat drawn = Normalized(obj.rotation * rot_o);
        const double dq = std::fabs(free_rot.X * drawn.X + free_rot.Y * drawn.Y + free_rot.Z * drawn.Z +
                                    free_rot.W * drawn.W);
        turn[i] = 2.0 * std::acos(std::min(1.0, dq)) * kRadToDeg;
    }
    const int pick = kinds[slide_pose::ChooseRearPinch(saved, turn, max_turn)];
    g_slide_hold[side] = pick;
    char above[48], behind[48];
    if (saved[0]) std::snprintf(above, sizeof(above), "turns the hand %.0f deg", turn[0]);
    else std::snprintf(above, sizeof(above), "not posed");
    if (saved[1]) std::snprintf(behind, sizeof(behind), "turns the hand %.0f deg", turn[1]);
    else std::snprintf(behind, sizeof(behind), "not posed (built from where the hand is)");
    log::Info("body: slide rear pinch: the pose from above %s, the pose from behind %s; drawing the %s (a pose that "
              "turns the hand more than %.0f deg is not used)",
              above, behind, kSlideHoldName[pick], max_turn);
    return pick;
}

// The hand moving between a magazine's grab pose and its pose in the
// magwell (stage 2), and back: over a short moment instead of a jump.
struct MagStageBlend {
    bool on = false;
    UObject object = 0;
    FQuat rot;     // the hand in the magazine's frame, before the change
    FVector pos;
    double t0_ms = 0;
} g_mag_stage[2];
constexpr double kMagStageMs = 120.0;

bool IsMagKind(hands::GrabKind k) { return k == hands::GrabKind::kMagHeld || k == hands::GrabKind::kMagInsert; }

void SolveArm(int side, const hands::HandState& h, const hands::HandGrab& grab, const hands::HandGrab& right_grab,
              const FVector& body_fwd, const FVector& body_right, double dt) {
    const Skeleton& k = G.skel;
    if (!k.arms_ok[side]) { ResetHandPhysics(side); return; }
    const HandRig& rig = G.rig[side];
    const hands::Hand hs = side == kL ? hands::kLeft : hands::kRight;
    // Capture before any parent edits. Segment length scale must not leak
    // through the hierarchy into the next segment or shrink the hand/fingers.
    const FVector upper_skin_scale = WX(k.upperarm[side]).scale;
    const FVector forearm_skin_scale = WX(k.lowerarm[side]).scale;
    const FVector hand_skin_scale = WX(k.hand[side]).scale;
    hands::FingerInput fin = hands::GetFingers(hs);
    // The approach is a visual pose, independent from live trigger input.
    if (grab.holster_preview) { fin.trigger = 0; fin.trigger_touch = false; }

    // A new grab: load its stored grip.
    ActiveGrip& g = G.grip[side];
    if (grab.object != g.object || grab.kind != g.kind || grab.point != g.point) {
        G.twist_state[side].support = {};
        if (g_editor.Active() && g_edit_side == side) StopEditor();
        hands::Pose gun;
        // The pistol's slide holds are picked by where and how the hand
        // grabbed; a charging handle has one pose per hand instead.
        const bool charging = grab.kind == hands::GrabKind::kSlideGrab && grab.point >= 0;
        if (grab.kind == hands::GrabKind::kSlideGrab && !charging && ObjectFrame(grab, right_grab, &gun)) {
            g_slide_hold[side] = PickSlideHold(h, gun, grab.object);
            if (g_slide_hold[side] == kSlideRear) g_slide_hold[side] = PickRearPinch(side, h, gun, grab);
        }
        // Into or out of the magwell with the same magazine: from where the
        // hand was on it.
        const bool mag_stage = grab.object && grab.object == g.object && IsMagKind(grab.kind) && IsMagKind(g.kind) &&
                               g.have && !g.pending;
        if (mag_stage) g_mag_stage[side] = {true, grab.object, g.hand_rot, g.hand_pos, perf::NowMs()};
        else g_mag_stage[side].on = false;
        LoadGrip(side, grab);
        if (grab.kind == hands::GrabKind::kMagInsert) {
            const int clip = InsertClip(grab.object);
            const bool anim = side == kL && clip >= 0 && reload_anim::Get(static_cast<reload_anim::Clip>(clip)).ready;
            log::Info("body: %s hand's magazine lined up with the magwell: pose %s %s; fingers %s",
                      side == kL ? "left" : "right", g.key.c_str(),
                      g.have || g.pending ? (g.from_file ? "(saved)" : "(built in)") : "(none: hand on the controller)",
                      anim ? (clip == reload_anim::kRifle ? "play the game's AR reload as it goes in"
                                                          : "play the game's pistol reload as it goes in")
                           : side == kR ? "held (the game's reload animation is for the left hand)"
                           : clip < 0   ? (std::string("held: ") + NoClipWhy(grab.object)).c_str()
                                        : "held (the game's reload animation is not read)");
        }
        g_free_slide[side].on = false;
        // Not posed yet: the hand stays where the controller grabbed (the
        // pistol builds a pinch or overhand from the slide's shape first).
        if (grab.kind == hands::GrabKind::kSlideGrab && !g.have && !g.pending && ObjectFrame(grab, right_grab, &gun) &&
            (charging || !BuildSlidePreset(side, h, gun, grab))) {
            const FQuat inv = gun.rotation.Inverse();
            g_free_slide[side] = {true, Normalized(inv * h.grip.rotation), inv.Rotate(h.grip.location - gun.location)};
        }
        if (charging) {
            log::Info("body: %s hand on the charging handle: pose %s %s", side == kL ? "left" : "right",
                      g.key.c_str(),
                      g.have || g.pending ? (g.from_file ? "(saved)" : "(built in)")
                                          : "not posed yet (the hand stays where it grabbed; pose it in the grip "
                                            "editor)");
        }
    }
    // How far the drawn hand jumps from the controller onto the slide (2026-11-03:
    // grabs from behind the slide caught 10-14 cm behind the grab point).
    if (grab.kind == hands::GrabKind::kSlideGrab && grab.object != g_slide_jump_logged[side]) {
        hands::Pose o;
        if (g.have && rig.ok && rig.frame.valid && ObjectFrame(grab, right_grab, &o)) {
            g_slide_jump_logged[side] = grab.object;
            const FQuat drawn_rot = Normalized(o.rotation * g.hand_rot);
            const FVector drawn_fist = o.location + o.rotation.Rotate(g.hand_pos) + drawn_rot.Rotate(rig.frame.fist);
            FQuat free_rot;
            FVector free_pos;
            FreeHand(side, h, h.grip.location, &free_rot, &free_pos);
            const double dq = std::fabs(free_rot.X * drawn_rot.X + free_rot.Y * drawn_rot.Y +
                                        free_rot.Z * drawn_rot.Z + free_rot.W * drawn_rot.W);
            log::Info("body: %s%s hold drawn: the hand jumps %.1f cm and turns %.0f deg from the controller onto "
                      "the %s",
                      grab.point >= 0 ? "" : "slide ",
                      grab.point >= 0 ? "charging handle" : kSlideHoldName[g_slide_hold[side]],
                      Length(drawn_fist - h.grip.location), 2.0 * std::acos(std::min(1.0, dq)) * kRadToDeg,
                      grab.point >= 0 ? "handle" : "slide");
        }
    } else if (grab.kind != hands::GrabKind::kSlideGrab) {
        g_slide_jump_logged[side] = 0;
    }
    hands::Pose obj;
    const bool recorded = g.have && rig.ok && ObjectFrame(grab, right_grab, &obj);
    // The left hand's own reach for the holstered pistol (kPointHolster) and
    // the off hand on the pistol (kPointOffhand*) are posed by the owner:
    // shown as posed, not fitted like the right hand's gun grip reaching for
    // the holster (holster_pose::Fit is for that hold).
    const bool own_reach = OwnReach(grab);
    if (recorded && grab.holster_preview && !own_reach && !g.approach_ready) {
        g.approach = holster_pose::Fit(g.rec, rig.chain, g.hand_rot, g.hand_pos, S.finger_radius);
        g.approach_ready = true;
    }

    // Where the hand goes.
    FQuat hand_rot;
    FVector hand_pos;
    double flat = 0.0;  // how flat an empty hand is pressed on a wall
    if (recorded) {
        FQuat in_rot = g.hand_rot;
        FVector in_pos = g.hand_pos;
        MagStageBlend& mb = g_mag_stage[side];
        if (mb.on) {
            const double t = (perf::NowMs() - mb.t0_ms) / kMagStageMs;
            if (t >= 1.0 || t < 0.0 || mb.object != grab.object || !IsMagKind(grab.kind)) {
                mb.on = false;
            } else {
                const double e = t * t * (3.0 - 2.0 * t);
                in_pos = mb.pos + (g.hand_pos - mb.pos) * e;
                in_rot = Normalized(mb.rot * Scale(Normalized(mb.rot.Inverse() * g.hand_rot), e));
            }
        }
        hand_rot = Normalized(obj.rotation * in_rot);
        hand_pos = obj.location +
                   obj.rotation.Rotate(grab.holster_preview && !own_reach ? g.approach.hand_position : in_pos);
    } else {
        // Unposed slide hold: a controller carried along with the slide.
        hands::HandState hv = h;
        hands::Pose sobj;
        if (grab.kind == hands::GrabKind::kSlideGrab && g_free_slide[side].on && ObjectFrame(grab, right_grab, &sobj)) {
            const FQuat turn = Normalized(sobj.rotation * g_free_slide[side].rot * h.grip.rotation.Inverse());
            hv.grip.rotation = Normalized(sobj.rotation * g_free_slide[side].rot);
            hv.grip.location = sobj.location + sobj.rotation.Rotate(g_free_slide[side].pos);
            hv.palm_normal = turn.Rotate(h.palm_normal);
        }
        const bool free_slide = grab.kind == hands::GrabKind::kSlideGrab && g_free_slide[side].on;
        FreeHand(side, hv, IsGrab(grab.kind) && !free_slide ? grab.target.location : hv.grip.location, &hand_rot,
                 &hand_pos);
        if (IsGrab(grab.kind) && !free_slide && M.calibrated && M.profile.arm[side].wrist.measured && rig.frame.valid) {
            // This target denotes a contact/fist anchor, not a raw controller
            // origin. Seat the newly oriented hand on it; the frozen physical
            // wrist offset is reserved for free tracked hands and calibration.
            hand_pos = grab.target.location + hv.grip.rotation.Rotate(FVector{S.hand_offset[0], S.hand_offset[1], S.hand_offset[2]})
                       - hand_rot.Rotate(rig.frame.fist);
        }
        // The precise path gets its flattening from the checked oriented
        // palm proxy; no translation or rotation is added after that check.
        if (FreeContactAllowed(side, grab) &&
            !hand_walls::Motion(side).palm_shape) {
            flat = PressFlat(side, hv, &hand_rot, &hand_pos, dt);
        } else {
            flat = FreeContactAllowed(side, grab) ? hand_walls::Motion(side).flatness : 0.0;
            g_flat[side] = flat;
            g_touch_d[side] = kNoTouch;
        }
    }
    if (grab.holster_preview) {
        FQuat free_rot;
        FVector free_pos;
        FreeHand(side, h, h.grip.location, &free_rot, &free_pos);
        // Not posed (the left hand's own reach, BP_HG_C.holster-left; the
        // off hand on the pistol, BP_HG_C.offhand-left): the hand stays
        // where the controller is.
        const double blend = recorded ? std::clamp(grab.pose_blend, 0.0, 1.0) : 0.0;
        hand_pos = free_pos + (hand_pos - free_pos) * blend;
        hand_rot = Normalized(Scale(hand_rot * free_rot.Inverse(), blend) * free_rot);
    }
    if (flat < 0.99) g_was_flat[side] = false;
    const double hand_norm = hand_rot.X*hand_rot.X + hand_rot.Y*hand_rot.Y + hand_rot.Z*hand_rot.Z + hand_rot.W*hand_rot.W;
    if (!std::isfinite(Length(hand_pos)) || !std::isfinite(hand_norm) || hand_norm < 1e-9) {
        ResetHandPhysics(side); return;
    }

    const arm_pose::Reference& reference = G.arm_ref[side];
    if (!reference.valid) { ResetHandPhysics(side); return; }
    const double tracking_scale = stereo::TrackingScale();
    FVector calibrated_shoulder;
    double calibrated_upper = 0, calibrated_lower = 0;
    hands::Pose tracked_head;
    const FQuat body_rotation = YawOnly(std::atan2(body_fwd.Y, body_fwd.X) * kRadToDeg);
    const bool head_valid = hands::Head(&tracked_head);
    const bool calibrated = M.calibrated && head_valid && body_calibration::MapArmTarget(
        M.profile, side, tracked_head.location, body_rotation, tracking_scale,
        &calibrated_shoulder, &calibrated_upper, &calibrated_lower);
    if (calibrated) {
        Xf upper_arm = WX(k.upperarm[side]);
        upper_arm.translation = calibrated_shoulder;
        SetWX(k.upperarm[side], upper_arm);
    }
    FVector shoulder = WP(k.upperarm[side]);
    const double mesh_scale = (std::fabs(g_w.scale.X) + std::fabs(g_w.scale.Y) + std::fabs(g_w.scale.Z)) / 3.0;
    const bool measured = calibrated && S.arm_scale_override <= 0;
    const double upper = measured ? calibrated_upper
                                  : reference.upper_length * mesh_scale * ArmScale();
    const double lower = measured ? calibrated_lower
                                  : reference.forearm_length * mesh_scale * ArmScale();
    if (upper < 1.0 || lower < 1.0) { ResetHandPhysics(side); return; }
    const double reach = upper + lower;

    // A measured shoulder is authoritative. Legacy clavicle reach assistance
    // moves that anchor toward the hand and makes a straight calibrated arm
    // bend again, so it applies only to uncalibrated geometry.
    double dist = Length(hand_pos - shoulder);
    if (k.clavicle[side] >= 0 && !calibrated) {
        double want = std::clamp((dist / reach - 0.85) / 0.25, 0.0, 1.0);
        G.clav_amount[side] += (want - G.clav_amount[side]) * (1.0 - std::exp(-10.0 * dt));
        if (G.clav_amount[side] > 0.01) {
            FVector clav = WP(k.clavicle[side]);
            FQuat q = Arc(shoulder - clav, hand_pos - clav);
            RotW(k.clavicle[side], ClampAngle(Scale(q, 0.35 * G.clav_amount[side]), 25.0 * kDegToRad));
            shoulder = WP(k.upperarm[side]);
            dist = Length(hand_pos - shoulder);
        }
    } else G.clav_amount[side] = 0;

    // Free-hand elbow placement follows reach, body orientation and temporal
    // continuity. Controller roll is not an elbow observation. Explicit
    // recorded-gun wrist preferences remain scoped to those authored grips.
    const FVector outward = side == kL ? body_right * -1.0 : body_right;
    // The solver owns the body-relative rest preference for every runtime arm.
    arm_posture::Input posture_input;
    posture_input.reference = reference;
    posture_input.shoulder = shoulder;
    posture_input.wrist = hand_pos;
    posture_input.hand_rotation = hand_rot;
    posture_input.upper_cm = upper;
    posture_input.forearm_cm = lower;
    posture_input.max_stretch_cm = reach * (MaxStretch() - 1.0);
    posture_input.outward = outward;
    posture_input.forward = body_fwd;
    posture_input.torso_center_valid = head_valid;
    posture_input.torso_center = head_valid ? tracked_head.location : shoulder;
    posture_input.forearm_axis_hand = rig.forearm_dir;
    posture_input.palm_axis_hand = rig.palm_h;
    posture_input.hand_frame_valid = rig.ok;
    // Both authored gun grips constrain a real hand-on-object orientation.
    // Keep soft wrist relief for the support hand too, so the rest preference
    // cannot tuck its elbow at the expense of a sharply folded rifle wrist.
    posture_input.wrist_relief = S.elbow_relieves_wrist && recorded && rig.ok &&
        (grab.kind == hands::GrabKind::kGunGrip || grab.kind == hands::GrabKind::kSupportGrip);
    // A straight wrist (forearm in line behind the gun) is a pistol hold. On
    // a shouldered rifle it pulled the trigger elbow over the shoulder.
    posture_input.straight_wrist = S.gun_wrist_straight && recorded && rig.ok &&
        grab.kind == hands::GrabKind::kGunGrip && g.key.rfind("BP_HG", 0) == 0;
    posture_input.joint_limits = S.arm_joint_limits;
    // Owner 2026-11-07, rifle aimed: the trigger elbow sat out at shoulder
    // height. A hand on a gun keeps the elbow hanging.
    posture_input.gun_hold = recorded &&
        (grab.kind == hands::GrabKind::kGunGrip || grab.kind == hands::GrabKind::kSupportGrip);
    // A support hand far out under the handguard may turn its elbow in under
    // the gun (2026-11-07 run: support forearms twisted 10-30 deg too far).
    posture_input.support_hold = recorded && grab.kind == hands::GrabKind::kSupportGrip;
    posture_input.shoulder_twist_share = S.shoulder_twist_share;
    posture_input.radial_axis_hand = rig.across_h;
    posture_input.dt = dt;
    const auto posture = arm_posture::Solve(posture_input, &G.posture_state[side]);
    if (!posture.valid) { ResetHandPhysics(side); return; }
    const auto& sol = posture.solved;
    const auto& rotations = posture.rotations;
    Xf upper_arm = WX(k.upperarm[side]);
    upper_arm.rotation = rotations.upper;
    arm_pose::MatchSegmentScale(reference.upper_axis, reference.upper_scale, reference.upper_length,
                               Length(sol.mid - shoulder), upper_skin_scale, &upper_arm.scale);
    SetWX(k.upperarm[side], upper_arm);
    Xf forearm = WX(k.lowerarm[side]);
    forearm.translation = sol.mid;
    forearm.rotation = rotations.forearm;
    forearm.scale = forearm_skin_scale;
    arm_pose::MatchSegmentScale(reference.forearm_axis, reference.forearm_scale, reference.forearm_length,
                               Length(sol.end - sol.mid), forearm_skin_scale, &forearm.scale);
    SetWX(k.lowerarm[side], forearm);

    // Place skin joints at their reference fraction of each calibrated
    // segment. The twist bones can be sibling branches; inherited transforms
    // cannot be assumed to follow the moved wrist or the forearm's roll.
    Xf attachment;
    if (k.upperarm_twist[side] >= 0 && arm_pose::PlaceAttachment(
            G.upper_twist_ref[side], upper_arm, sol.mid, &attachment))
        SetWX(k.upperarm_twist[side], attachment);
    const FVector fa = Normalize(sol.end - sol.mid);
    // How the reference pose's forearm is turned (0 palm toward the biceps,
    // 90 deg thumb up). The skin twist's seam goes opposite a thumb-up
    // forearm, where the palm faces outward: a turn no forearm makes alone.
    const double side_sign = side == kR ? 1.0 : -1.0;
    const double rest_turn = arm_pose::ReferenceForearmRotation(reference, rig.palm_h, side_sign);
    const double twist_center = std::isfinite(rest_turn) ? side_sign * (kPi / 2 - rest_turn) : 0.0;
    if (std::isfinite(rest_turn) && !(std::fabs(rest_turn - G.rest_turn_logged[side]) < kDegToRad)) {
        G.rest_turn_logged[side] = rest_turn;
        log::Info("body: %s forearm in the skeleton's reference pose is turned %.0f deg from palm-toward-biceps "
                  "(90 = thumb up); skin twist %.2f at the elbow, %.2f at mid-forearm, all at the wrist",
                  side == kL ? "left" : "right", rest_turn * kRadToDeg, S.twist_at_elbow, S.twist_at_middle);
    }
    auto twist = arm_pose::FollowTwist(posture.raw_roll, dt, S.twist_at_elbow, S.twist_at_middle,
                                       &G.twist_state[side], twist_center, S.forearm_roll_share);
    // The authored support palm remains planted on the rifle. The wrist
    // display's share of its turn must not choose a new half-roll branch when
    // the IK frame crosses pi: it follows the bounded attachment history.
    // The authored hand, finger wrap and skin shares are untouched.
    bool support_palm = false;
    if(recorded && grab.kind == hands::GrabKind::kSupportGrip) {
        const auto support = arm_pose::FollowSupportTwist(reference, forearm.rotation, hand_rot,
            rig.palm_h, dt, S.forearm_roll_share, &G.twist_state[side].support, twist.watch_radians);
        if(support.valid) { twist.watch_radians=support.forearm_radians; support_palm=support.observable; }
    } else G.twist_state[side].support = {};
    Xf rolled_forearm = forearm;
    rolled_forearm.rotation = Normalized(AxisAngle(fa, twist.forearm_radians) * forearm.rotation);
    SetWX(k.lowerarm[side], rolled_forearm);
    Xf twist_frame = forearm;
    twist_frame.rotation = Normalized(AxisAngle(fa, twist.twist_radians) * forearm.rotation);
    if (k.forearm_twist[side] >= 0 && arm_pose::PlaceAttachment(
            G.forearm_twist_ref[side], twist_frame, sol.end, &attachment))
        SetWX(k.forearm_twist[side], attachment);
    Xf hand = WX(k.hand[side]);
    hand.rotation = hand_rot;
    hand.translation = sol.end;
    hand.scale = hand_skin_scale;
    SetWX(k.hand[side], hand);

    // Publish only after a successful solve, never a previous frame's pose
    // when one of the validation paths above returned early.
    auto& accessory = G.watch_arm[side];
    // The wrist display keeps its own share of the turn (as before the skin
    // twist moved toward the wrist).
    accessory.lower = forearm;
    accessory.lower.rotation = Normalized(AxisAngle(fa, twist.watch_radians) * forearm.rotation);
    // A rigid watch follows the wrist and ordinary body scale, not the skin's
    // new longitudinal ratio. Its thickness and wrist setback stay unchanged.
    accessory.lower.scale = forearm_skin_scale;
    accessory.hand = WX(k.hand[side]);
    accessory.twist = accessory.has_twist ? WX(k.forearm_twist[side]) : accessory.lower;
    G.watch_arm_ms[side] = perf::NowMs();

    // Separate reaching the chosen target from following the actual controller.
    // These are solved-bone measurements, not a GPU skinning measurement.
    hands::HandState raw_hand = h;
    raw_hand.grip = g_raw_grip[side];
    raw_hand.palm_normal = raw_hand.grip.rotation.Rotate(FVector{0, side == kL ? 1.0 : -1.0, 0});
    FQuat raw_rotation; FVector raw_wrist;
    FreeHand(side, raw_hand, raw_hand.grip.location, &raw_rotation, &raw_wrist);
    G.fit_controller_cm[side] += Length(sol.end - raw_wrist);
    const FQuat controller_delta = Normalized(hand_rot * raw_rotation.Inverse());
    G.fit_controller_deg[side] += 2 * std::acos(std::clamp(std::fabs(controller_delta.W), 0.0, 1.0)) * kRadToDeg;
    // Fit numbers for the log.
    G.fit_reach[side] += dist / reach;
    double gap = Length(sol.end - hand_pos);
    G.fit_gap[side] += gap;
    G.max_gap[side] = std::max(G.max_gap[side], gap);
    G.fit_flex[side] += std::fabs(posture.flex) * kRadToDeg;
    G.fit_dev[side] += std::fabs(posture.deviation) * kRadToDeg;
    G.fit_swivel[side] += posture.swivel * kRadToDeg;
    G.fit_twist[side] += std::fabs(twist.forearm_radians) * kRadToDeg;
    ++G.fit_n[side];

    // Averages hide sign and turn-count failures. Capture the actual bounded
    // bone angles and the simultaneous geometry for a repeatable headset pose.
    // Coordinates: body yaw frame, shoulder relative to head; elbow/wrist
    // relative to shoulder. This is solver evidence, not a skinning capture.
    if (S.fit_log) {
        const double now_ms = perf::NowMs();
        if (G.arm_sample_ms[side] == 0 || now_ms - G.arm_sample_ms[side] >= 5000) {
            G.arm_sample_ms[side] = now_ms;
            const FQuat inv = body_rotation.Inverse();
            const FVector sh = inv.Rotate(shoulder - (head_valid ? tracked_head.location : shoulder));
            const FVector el = inv.Rotate(sol.mid - shoulder), wr = inv.Rotate(sol.end - shoulder);
            const FQuat hq = Normalized(inv * hand_rot);
            // Where the controller puts the hand (the free hand's wrist and
            // turn) and how the head is tilted: a drawn hand on a gun sits
            // away from the real one, and a head leaning into the sights
            // carries the head-relative shoulder with it.
            const FVector real_wr = inv.Rotate(raw_wrist - shoulder);
            const FQuat real_q = Normalized(inv * raw_rotation);
            double head_pitch = 0, head_roll = 0;
            if (head_valid) {
                const FQuat head_q = Normalized(inv * tracked_head.rotation);
                head_pitch = std::asin(std::clamp(head_q.Rotate(FVector{1, 0, 0}).Z, -1.0, 1.0)) * kRadToDeg;
                head_roll = std::asin(std::clamp(-head_q.Rotate(FVector{0, 1, 0}).Z, -1.0, 1.0)) * kRadToDeg;
            }
            log::Info("body arm sample %s: calibrated=%d head=%d grab=%d lengths=%.2f/%.2f reach=%.3f "
                      "roll raw/target/lower/skin/watch=%+.1f/%+.1f/%+.1f/%+.1f/%+.1f seam=%d support-palm=%d "
                      "bend=%+.1f/%+.1f swivel=%+.1f forearm-turn=%+.0f elbow-up/limit=%.1f/%.1f "
                      "constrained=%d candidates=%u clearance=%.1f "
                      "shoulder=(%.2f,%.2f,%.2f) elbow=(%.2f,%.2f,%.2f) wrist=(%.2f,%.2f,%.2f) "
                      "hand-q=(%.4f,%.4f,%.4f,%.4f) real-wrist=(%.2f,%.2f,%.2f) real-hand-q=(%.4f,%.4f,%.4f,%.4f) "
                      "head-pitch/roll=%+.0f/%+.0f",
                      side == kL ? "left" : "right", calibrated, head_valid, static_cast<int>(grab.kind), upper, lower, dist/reach,
                      twist.raw_radians*kRadToDeg, twist.target_radians*kRadToDeg,
                      twist.forearm_radians*kRadToDeg, twist.twist_radians*kRadToDeg, twist.watch_radians*kRadToDeg,
                      twist.seam_guarded, support_palm,
                      posture.flex*kRadToDeg, posture.deviation*kRadToDeg, posture.swivel*kRadToDeg,
                      posture.forearm_rotation*kRadToDeg, posture.elbow_height_cm, posture.elbow_cap_cm,
                      posture.constrained, posture.candidates, posture.torso_clearance_cm,
                      sh.X,sh.Y,sh.Z,el.X,el.Y,el.Z,wr.X,wr.Y,wr.Z,hq.X,hq.Y,hq.Z,hq.W,
                      real_wr.X,real_wr.Y,real_wr.Z,real_q.X,real_q.Y,real_q.Z,real_q.W,head_pitch,head_roll);
        }
    }

    // Fingers.
    if (!S.hand_poses || !rig.ok) {
        ResetFingerContacts(side);
        return;
    }
    fingers::Angles want[hand_pose::kFingerCount];
    FVector wall_at, wall_out;
    bool contact_solved = false;
    const bool free_contact = !recorded && FreeContactAllowed(side, grab);
    const bool segment_contact = free_contact && hand_walls::Enabled() && S.fingers_rest && finger_world::Enabled();
    if (!segment_contact && g_precise_fingers[side]) ResetFingerContacts(side);
    // Index: off the trigger -> resting on it (touch) -> pulled (analog).
    const double follow = 1.0 - std::exp(-dt / 0.06);
    G.index_touch[side] += ((fin.trigger_touch || fin.trigger > 0.05f ? 1.0 : 0.0) - G.index_touch[side]) * follow;
    G.index_pull[side] = std::clamp<double>(fin.trigger, 0.0, 1.0);
    if (recorded) {
        g_finger_wall[side].on = false;
        want[hand_pose::kThumb] = g.tip_angles[gr::kThumb];
        want[hand_pose::kMiddle] = g.tip_angles[gr::kMiddle];
        want[hand_pose::kRing] = g.tip_angles[gr::kRing];
        want[hand_pose::kPinky] = g.tip_angles[gr::kPinky];
        fingers::Angles idx = fingers::Blend(g.tip_angles[gr::kIndexOff], g.tip_angles[gr::kIndexRest], G.index_touch[side]);
        want[hand_pose::kIndex] = fingers::Blend(idx, g.tip_angles[gr::kIndexPull], G.index_pull[side]);
        // The magazine going in: the game's own left-hand reload plays on
        // top of the pose, by how far in it is (back out: backwards).
        const int clip = grab.kind == hands::GrabKind::kMagInsert && side == kL ? InsertClip(grab.object) : -1;
        if (clip >= 0 && reload_anim::Get(static_cast<reload_anim::Clip>(clip)).ready) {
            reload_anim::Pose turn;
            reload_anim::Delta(reload_anim::Get(static_cast<reload_anim::Clip>(clip)), grab.insert, &turn);
            for (int f = 0; f < hand_pose::kFingerCount && f < reload_anim::kFingers; ++f)
                for (int j = 0; j < 3; ++j) want[f].extra[j] = Normalized(want[f].extra[j] * turn.joint[f][j]);
        }
    } else if (segment_contact) {
        perf::Scope contact_timer(g_perf_finger_contact);
        const hand_pose::Curl curl = hand_pose::Target(grab.kind, fin);
        for (int f = 0; f < hand_pose::kFingerCount; ++f) {
            // The intention remains independent from the final contact pose.
            want[f] = fingers::Curl(rig.chain[f],
                        std::clamp(curl.finger[f] * S.finger_scale, 0.0, 1.2) * (1.0 - flat),
                        fingers::DefaultLimits(f == hand_pose::kThumb));
        }
        if (S.thumb_opposition) {
            const auto bounds = FingerBounds(rig, hand_pose::kThumb);
            want[hand_pose::kThumb] = thumb_pose::Target(rig.chain[hand_pose::kThumb], rig.palm_h,
                rig.chain[hand_pose::kIndex].ref[0].translation,
                {fin.grip * (1.0 - flat), fin.trigger * (1.0 - flat), fin.thumb_touch && flat < 0.5}, bounds);
        }
        finger_contact::Contacts contacts[hand_pose::kFingerCount];
        finger_contact::Bounds bounds[hand_pose::kFingerCount];
        for (int f = 0; f < hand_pose::kFingerCount; ++f) bounds[f] = FingerBounds(rig, f);
        const auto query = finger_world::Update(G.player, side, rig.chain, want, hand, S.finger_radius, dt, contacts);
        int active = 0;
        double residual = 0;
        for (int f = 0; f < hand_pose::kFingerCount; ++f) {
            auto& state = g_finger_contact[side][f];
            if (!query.available && G.cur_init[side]) {
                // Missing world evidence is not permission to relax through
                // an obstacle. Retain the displayed pose until fresh queries
                // return; this carries no old contact plane forward.
                want[f] = G.cur[side][f];
                state.pose = want[f];
                state.angular_velocity = {};
                state.initialized = true;
                continue;
            }
            if (!state.initialized && G.cur_init[side] && !query.reset) {
                state.pose = G.cur[side][f];
                state.initialized = true;
            }
            const auto solved = finger_contact::Solve(rig.chain[f], want[f], state, hand,
                                    bounds[f], contacts[f], dt, query.reset);
            want[f] = solved.pose;
            active += solved.active_contacts;
            residual = std::max(residual, solved.residual_cm);
        }
        finger_self::Result separation;
        if (query.available) {
            separation = finger_self::Resolve(rig.chain, want, g_finger_contact[side], hand,
                                               bounds, contacts, S.finger_radius, dt);
            // Keep the world solve's frozen support too: a live finite
            // patch can lose a sample and underreport unresolved depth.
            residual = std::max(residual, separation.world_residual_cm);
        }
        double angular_step = 0;
        if (!query.reset && G.cur_init[side]) for (int f = 0; f < hand_pose::kFingerCount; ++f) {
            for (int j = 0; j < 3; ++j) {
                const FQuat before = fingers::LocalRotation(rig.chain[f], j, G.cur[side][f]);
                const FQuat after = fingers::LocalRotation(rig.chain[f], j, want[f]);
                const FQuat delta = Normalized(after * before.Inverse());
                angular_step = std::max(angular_step, 2 * std::acos(std::clamp(std::fabs(delta.W), 0.0, 1.0)));
            }
        }
        g_precise_fingers[side] = true;
        g_finger_wall[side].on = false;
        NotePreciseFingers(side, query, active, residual, separation.residual_cm, angular_step,
                           Length(hand.translation - hand_pos));
    } else {
        hand_pose::Curl c = hand_pose::Target(grab.kind, fin);
        // An empty hand against a wall or table: each finger moves toward the
        // controller's curl like a sprung finger and gives way where the
        // surface stops it (vr/body/finger_rest), and fingertips on the
        // surface trail behind as the hand slides along it.
        const bool on_wall = free_contact && S.fingers_rest &&
                             hand_walls::Contact(side, &wall_at, &wall_out);
        contact_solved = on_wall;
        FingerWall& fw = g_finger_wall[side];
        FVector slide{0, 0, 0};  // the hand's speed along the surface, cm/s
        if (on_wall && fw.on && dt > 1e-4) {
            const FVector v = (hand.translation - fw.last_hand) * (1.0 / dt);
            slide = v - wall_out * Dot(v, wall_out);
            const double sp = Length(slide);
            if (sp > 150.0) slide = slide * (150.0 / sp);
        }
        fw.last_hand = hand.translation;
        const double drag_follow = 1.0 - std::exp(-dt / 0.06);
        const double spread_max = 12.0 * kDegToRad;
        double trail = 0.0;  // furthest a fingertip trails this frame, cm (for the log)
        for (int f = 0; f < hand_pose::kFingerCount; ++f) {
            const fingers::Chain& ch = rig.chain[f];
            const fingers::Limits lim = fingers::DefaultLimits(f == hand_pose::kThumb);
            // Pressed flat on a wall: the fingers open with it.
            const double curl = std::clamp(c.finger[f] * S.finger_scale, 0.0, 1.2) * (1.0 - flat);
            if (!on_wall) {
                want[f] = fingers::Curl(ch, curl, lim);
                fw.bend[f] = curl;
                fw.dir[f] = 0;
                fw.drag_bend[f] = fw.drag_spread[f] = 0.0;
                continue;
            }
            const fingers::Push p = fingers::PushFromPlane(ch, lim, hand, fw.on ? fw.bend[f] : curl, curl, 1.2, wall_at,
                                                           wall_out, S.finger_radius, fw.on ? fw.dir[f] : 0);
            NoteFingerRest(side, f, curl, p.bend);
            fw.bend[f] = p.bend;
            fw.dir[f] = p.dir;
            // Drag: a fingertip on the surface (within 1 cm) trails where the
            // hand was 0.05 s ago, as far as small bends and spreads allow.
            double tb = 0.0, ts = 0.0;
            if (S.finger_drag > 0.0 && Length(slide) > 2.0) {
                const FVector tip = fingers::TipWorld(ch, fingers::Bend(ch, p.bend, lim), hand);
                if (Dot(tip - wall_at, wall_out) - S.finger_radius < 1.0)
                    fingers::DragTip(ch, lim, hand, p.bend, slide * (-0.05 * S.finger_drag), &tb, &ts);
            }
            fw.drag_bend[f] += (tb - fw.drag_bend[f]) * drag_follow;
            fw.drag_spread[f] += (std::clamp(ts, -spread_max, spread_max) - fw.drag_spread[f]) * drag_follow;
            fingers::Angles a = fingers::Bend(ch, p.bend + fw.drag_bend[f], lim);
            a.spread = fw.drag_spread[f];
            // Never dragged into the surface: drop what would put it in.
            const double allowed = p.depth + 0.05;
            if (fingers::PlaneDepth(ch, a, hand, wall_at, wall_out, S.finger_radius) > allowed) {
                a = fingers::Bend(ch, p.bend, lim);
                a.spread = fw.drag_spread[f];
                if (fingers::PlaneDepth(ch, a, hand, wall_at, wall_out, S.finger_radius) > allowed) a.spread = 0.0;
            }
            if (fw.drag_bend[f] != 0.0 || fw.drag_spread[f] != 0.0)
                trail = std::max(trail, Length(fingers::TipWorld(ch, a, hand) -
                                               fingers::TipWorld(ch, fingers::Bend(ch, p.bend, lim), hand)));
            want[f] = a;
        }
        fw.on = on_wall;
        if (on_wall) {
            FlushFingerRest(side);
            NoteFingerDrag(side, Length(slide), trail);
        }
    }
    if (free_contact && !segment_contact && !contact_solved && S.thumb_opposition) {
        want[hand_pose::kThumb] = thumb_pose::Target(rig.chain[hand_pose::kThumb], rig.palm_h,
            rig.chain[hand_pose::kIndex].ref[0].translation,
            {fin.grip * (1.0 - flat), fin.trigger * (1.0 - flat), fin.thumb_touch && flat < 0.5},
            FingerBounds(rig, hand_pose::kThumb));
    }
    if (grab.holster_preview) {
        if (recorded && !OwnReach(grab)) {
            for (int f = 0; f < hand_pose::kFingerCount; ++f) want[f] = g.approach.finger[f];
        }
        const hand_pose::Curl free_curl = hand_pose::Target(hands::GrabKind::kNone, hands::GetFingers(hs));
        for (int f = 0; f < hand_pose::kFingerCount; ++f) {
            const auto lim = fingers::DefaultLimits(f == hand_pose::kThumb);
            const auto free_angles = fingers::Curl(rig.chain[f], std::min(1.2, free_curl.finger[f] * S.finger_scale), lim);
            want[f] = fingers::Blend(free_angles, want[f], std::clamp(grab.pose_blend, 0.0, 1.0));
        }
    }
    // Contact solving already eased before enforcing constraints. Authored
    // grips keep their existing blend and immediate trigger-index behavior.
    const double ease = 1.0 - std::exp(-dt / 0.04);
    for (int f = 0; f < hand_pose::kFingerCount; ++f) {
        if (segment_contact || !G.cur_init[side] || f == hand_pose::kIndex)
            G.cur[side][f] = want[f];
        else if (contact_solved)
            G.cur[side][f] = fingers::BlendOnPlane(rig.chain[f], G.cur[side][f], want[f], ease,
                                                  hand, wall_at, wall_out, S.finger_radius);
        else
            G.cur[side][f] = fingers::Blend(G.cur[side][f], want[f], ease);
    }
    G.cur_init[side] = true;
    PoseFingers(side, G.cur[side]);
    if (segment_contact) finger_world::Commit(side, rig.chain, G.cur[side], hand, S.finger_radius);

    // Fingertips and palm in the world, for the editor.
    Xf hw = WX(k.hand[side]);
    for (int t = 0; t < gr::kTipCount; ++t) {
        int f = TipFinger(t);
        Xf j[3];
        FVector tip;
        fingers::Fk(rig.chain[f], G.cur[side][f], j, &tip);
        g_tip_world[side][t] = skel::Apply(hw, tip);
    }
    for (int f = 0; f < hand_pose::kFingerCount; ++f) {
        Xf j[3];
        fingers::Fk(rig.chain[f], G.cur[side][f], j, nullptr);
        for (int n = 0; n < 3; ++n) g_joint_world[side][f][n] = skel::Compose(j[n], hw);
    }
    g_palm_world[side] = skel::Apply(hw, rig.palm_centre);
    static const double handle_out = config::GetFloat("hand_editor_handle_cm", 7.f);
    g_handle_world[side] = skel::Apply(hw, rig.palm_centre - rig.palm_h * handle_out);
    g_hand_world[side] = hw;
    g_tips_valid[side] = true;
    if (grab.kind == hands::GrabKind::kNone) {
        const FQuat inv = g_raw_grip[side].rotation.Inverse();
        std::lock_guard<std::mutex> lock(g_pinch_mutex);
        PinchNote& p = g_pinch[side];
        p.thumb = inv.Rotate(g_tip_world[side][gr::kThumb] - g_raw_grip[side].location);
        p.index = inv.Rotate(g_joint_world[side][hand_pose::kIndex][1].translation - g_raw_grip[side].location);
        p.ms = perf::NowMs();
        p.valid = true;
    } else if (grab.holster_preview) {
        // A visual-only snap (onto the holstered pistol, or the off hand onto
        // the pistol in the other hand): the hand is still empty. Its last
        // pinch, carried by the controller, stays in use, so a slide grab
        // starting from the snap still finds the fingers (PinchPoint).
        std::lock_guard<std::mutex> lock(g_pinch_mutex);
        if (g_pinch[side].valid) g_pinch[side].ms = perf::NowMs();
    }
    {
        HandContacts hc;
        for (int f = 0; f < hand_pose::kFingerCount && f < 5; ++f)
            hc.tip[f] = fingers::TipWorld(rig.chain[f], G.cur[side][f], hw);
        hc.tip_radius = S.finger_radius;
        hc.palm = g_palm_world[side];
        hc.palm_out = Normalize(hw.rotation.Rotate(rig.palm_h));
        hc.empty = hands::HeldGrabKind(grab) == hands::GrabKind::kNone;
        std::lock_guard<std::mutex> lock(g_pinch_mutex);
        g_contact[side] = {true, perf::NowMs(), hc};
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

void FitLog(double dt, double yaw_lag) {
    G.fit_yaw_lag += std::fabs(yaw_lag);
    ++G.fit_yaw_n;
    G.fit_time += dt;
    if (G.fit_time < 10.0) return;
    if (S.fit_log) {
        std::string arms;
        for (int s = 0; s < 2; ++s) {
            if (!G.fit_n[s]) continue;
            double n = G.fit_n[s];
            char buf[384];
            std::snprintf(buf, sizeof(buf),
                          " | %s (%s): reach %.0f%% of arm, target gap %.1f cm (worst %.1f), wrist bend %.0f/%.0f deg "
                          "(flex/side), applied forearm roll mean-abs %.0f deg, elbow swing %+.0f deg, controller offset %.1f cm / %.0f deg",
                          s == kL ? "left" : "right",
                          S.arm_scale_override > 0 ? "override" : M.calibrated
                              ? (!body_calibration::PlausibleSplit(M.profile.arm[s].upper_cm, M.profile.arm[s].forearm_cm)
                                  ? "usual proportions; saved split not human"
                                  : M.profile.arm[s].segments_measured ? "fitted upper and forearm" : M.profile.arm[s].forearm_measured
                                  ? "fitted forearm; estimated upper" : "estimated proportions") : "avatar proportions",
                          100.0 * G.fit_reach[s] / n, G.fit_gap[s] / n, G.max_gap[s],
                          G.fit_flex[s] / n, G.fit_dev[s] / n, G.fit_twist[s] / n, G.fit_swivel[s] / n,
                          G.fit_controller_cm[s] / n, G.fit_controller_deg[s] / n);
            arms += buf;
        }
        log::Info("body fit: arm scale %.3f (%s), body %.0f deg behind head yaw, tracked pelvis offset %+.1f cm, "
                  "spine scale %.3f, shoulder height residual %+.1f cm, %d steps taken%s", ArmScale(),
                  S.arm_scale_override > 0 ? "ini override" : M.calibrated ? "profile lengths; provenance per side" : M.have ? "legacy measurement" : "default",
                  G.fit_yaw_n ? G.fit_yaw_lag / G.fit_yaw_n : 0.0, G.body_dz,
                  G.torso_spine_scale, G.torso_shoulder_residual, G.steps_taken, arms.c_str());
        if (G.fit_leg_frames) {
            const auto terrain = leg_world::ReadTerrainOffsets(G.anim_src ? G.anim_src : G.source);
            log::Info("body legs: %u frames, animation bypass %u, table-height recovery L/R %u/%u; "
                      "source ALS known=%d foot Z %.1f/%.1f pelvis Z %.1f alpha %.2f trace above %.1f",
                      G.fit_leg_frames, G.fit_leg_bypass, G.fit_leg_recovery[0], G.fit_leg_recovery[1],
                      terrain.known, terrain.foot_location[0].Z, terrain.foot_location[1].Z,
                      terrain.pelvis_location.Z, terrain.pelvis_alpha, terrain.trace_above);
            const auto& probes = G.leg_probe.diagnostics;
            log::Info("body stairs: scans accepted/rejected %llu/%llu, qualified feet %llu, traces last/peak %u/%u total %llu",
                      static_cast<unsigned long long>(probes.scans_accepted), static_cast<unsigned long long>(probes.scans_rejected),
                      static_cast<unsigned long long>(probes.stair_feet), probes.traces_last, probes.traces_peak,
                      static_cast<unsigned long long>(probes.traces_total));
        }
    }
    G.fit_leg_frames = G.fit_leg_bypass = G.fit_leg_recovery[0] = G.fit_leg_recovery[1] = 0;
    G.fit_time = G.fit_yaw_lag = 0;
    G.fit_yaw_n = 0;
    for (int s = 0; s < 2; ++s) {
        G.fit_reach[s] = G.fit_gap[s] = G.fit_flex[s] = G.fit_dev[s] = G.fit_swivel[s] = G.fit_twist[s] = 0;
        G.max_gap[s] = G.fit_controller_cm[s] = G.fit_controller_deg[s] = 0;
        G.fit_n[s] = 0;
    }
}

void PoseKick(UObject player, bool kicking, const FQuat& body_frame, const FQuat& pose_yaw) {
    const auto& k = G.skel;
    if (!S.legs || !k.legs_ok[0] || !k.legs_ok[1] || !G.leg_ref[0].valid || !G.leg_ref[1].valid) {
        controls::ClearKickHips(player);
        return;
    }
    kick::Plan plan;
    if (kicking && controls::GetKickPlan(player, &plan)) {
        // Snapshot both legs before either write. For a right kick, the native
        // left leg supplies the action and the native right supplies support.
        // Mirroring that support into the left prevents both legs kicking.
        kick::VisualLeg source[2];
        FQuat reference[2];
        FQuat toe_rotation[2], toe_reference[2];
        const bool toes = k.toe[kL] >= 0 && k.toe[kR] >= 0;
        for (int side=0; side<2; ++side) {
            source[side] = {WP(k.thigh[side]), WP(k.calf[side]), WP(k.foot[side]), WX(k.foot[side]).rotation};
            reference[side] = Normalized(pose_yaw * g_w.rotation * G.stance_rotation[side]);
            if (toes) {
                toe_rotation[side] = WX(k.toe[side]).rotation;
                toe_reference[side] = Normalized(pose_yaw * g_w.rotation * G.toe_reference[side]);
            }
        }
        kick::VisualLeg target[2];
        bool mapped[2] = {};
        mapped[plan.side] = kick::MapVisual(source[kL], source[plan.side].hip, plan.side == kR,
            body_frame, ToQuat(plan.aim), reference[kL], reference[plan.side], &target[plan.side], plan.blend);
        if (plan.side == kR)
            mapped[kL] = kick::MapVisual(source[kR], source[kL].hip, true, body_frame,
                Normalized(body_frame * ToQuat({kick::kNativePitch,0,0})), reference[kR], reference[kL], &target[kL]);
        // Incomplete mirroring must not leave a partly swapped kick.
        if (mapped[plan.side] && (plan.side == kL || mapped[kL])) {
            for (int side=0; side<2; ++side) if (mapped[side]) {
                auto length = [&](const FVector& v) {
                    return Length(FVector{v.X*g_w.scale.X,v.Y*g_w.scale.Y,v.Z*g_w.scale.Z});
                };
                const FVector pole = target[side].knee-target[side].hip;
                const auto solved = SolveTwoBone(target[side].hip, target[side].ankle,
                    length(G.leg_upper_cs[side]), length(G.leg_lower_cs[side]), pole, 0);
                const auto rotation = arm_pose::Orient(G.leg_ref[side], target[side].hip, solved, pole);
                if (!rotation.valid) continue;
                Xf thigh = WX(k.thigh[side]); thigh.rotation = rotation.upper; SetWX(k.thigh[side], thigh);
                Xf calf = WX(k.calf[side]); calf.translation = solved.mid; calf.rotation = rotation.forearm;
                SetWX(k.calf[side], calf);
                Xf foot = WX(k.foot[side]); foot.translation = solved.end; foot.rotation = target[side].foot;
                SetWX(k.foot[side], foot);
                if (toes) {
                    const bool support = plan.side == kR && side == kL;
                    const int from = support ? kR : kL;
                    auto toe_source = source[from]; toe_source.foot = toe_rotation[from];
                    kick::VisualLeg toe_target;
                    const FQuat aim = support ? Normalized(body_frame * ToQuat({kick::kNativePitch,0,0})) : ToQuat(plan.aim);
                    if (kick::MapVisual(toe_source, source[side].hip, plan.side == kR, body_frame, aim,
                        toe_reference[from], toe_reference[side], &toe_target, support ? 1.0 : plan.blend)) {
                        Xf toe = WX(k.toe[side]); toe.rotation = toe_target.foot; SetWX(k.toe[side], toe);
                    }
                }
            }
        }
    }
    // These are the rendered origins after room movement/body yaw. The native
    // hit trace consumes them only while their owning actor/frame remains fresh.
    const FVector hips[2] = {WP(k.thigh[kL]), WP(k.thigh[kR])};
    controls::PublishKickHips(player, hips);
}

void OnTick(UObject player, float dt_f) {
    for (double& ms : G.watch_arm_ms) ms = 0;
    static bool registered = [] {
        perf::Register(&g_perf_tick);
        perf::Register(&g_perf_engine);
        perf::Register(&g_perf_solve);
        perf::Register(&g_perf_grip);
        perf::Register(&g_perf_finger_contact);
        perf::Register(&g_perf_legs);
        perf::Register(&g_perf_arms);
        perf::Register(&g_perf_calibration);
        return true;
    }();
    (void)registered;
    perf::Scope tick_timer(g_perf_tick);
    // Every successful frame republishes its final hips. Any early return
    // must withdraw the preceding visual pose before a later kick trace.
    controls::ClearKickHips(player);
    const double dt = std::clamp(std::isfinite(dt_f) ? static_cast<double>(dt_f) : 0.0, 0.0, 0.1);
    PollToggle();
    if (g_calibration_cancel.exchange(false)) EndCalibration("Cancelled.");
    hands::Pose head;
    const bool vr = hands::Head(&head);
    if (!S.enabled || !vr) {
        ResetHandPhysics();
        if(!S.enabled && (g_calibration_active || g_measure_requested)) EndCalibration("Turn on the VR body first.");
        else if(g_calibration_active) {
            if(g_measure_player!=player) EndCalibration("Player changed.");
            else StepMeasure(dt); // invalid raw tracking pauses its window; accepted stages survive
        }
        for (int side = 0; side < 2; ++side) { G.arm_tracked[side] = false; G.posture_state[side] = {}; G.twist_state[side] = {}; }
        legs::Reset(G.legs);
        G.leg_probe = {};
        if (G.active && G.player == player) Deactivate();
        return;
    }
    if (!M.loaded) LoadMeasure();
    if (!Setup(player)) {
        ResetHandPhysics();
        if (g_calibration_active || g_measure_requested) EndCalibration("Avatar is not ready. Retry.");
        return;
    }
    Activate();
    if (!G.active) {
        ResetHandPhysics();
        if (g_calibration_active || g_measure_requested) EndCalibration("VR body is unavailable.");
        return;
    }
    const Skeleton& k = G.skel;

    {
        perf::Scope t(g_perf_engine);
        auto copy_pose = [] {
            ue::Call copy(G.pose_comp, "CopyPoseFromSkeletalComponent");
            copy.Set("InComponentToCopy", G.anim_src ? G.anim_src : G.source);
            return copy.Invoke();
        };
        if (!copy_pose()) {
            ResetHandPhysics();
            if (g_calibration_active || g_measure_requested) EndCalibration("Avatar pose is unavailable.");
            return;
        }
        G.engine.TryLocate();
        if (G.ref_pending) {
            CaptureReference();
            if (G.failed) {
                if (g_calibration_active || g_measure_requested) EndCalibration("Avatar reference is unavailable.");
                Deactivate();
                return;
            }
            copy_pose();
        }
        if (!G.engine.Read(&G.frame_local)) {
            ResetHandPhysics();
            if (g_calibration_active || g_measure_requested) EndCalibration("Avatar pose read failed.");
            return;
        }
        g_w = ComponentToWorld(G.pose_comp);
    }
    const double t_solve = perf::NowMs();
    G.pose.SetAllLocal(G.frame_local.data(), static_cast<int>(G.frame_local.size()));
    G.pose.ClearChanged();
    ++G.frames;

    // The game's left-hand reload animation, read once a few seconds into
    // the level (reload_anim.h); magazine poses in the magwell built before
    // it was read are built again.
    if (S.mag_insert_anim && G.frames > 200 && k.fingers_ok[kL]) {
        reload_anim::Bones bones;
        auto fname = [](const FName& n) {
            uint64_t v = 0;
            std::memcpy(&v, &n, sizeof(v));
            return v;
        };
        bones.hand = fname(G.engine.names[k.hand[kL]]);
        for (int f = 0; f < hand_pose::kFingerCount && f < reload_anim::kFingers; ++f)
            for (int j = 0; j < 3; ++j) bones.finger[f][j] = fname(G.engine.names[k.finger[kL][f][j]]);
        unsigned clips = 1u << reload_anim::kPistol;
        if (S.mag_insert_ar_clip >= 0) clips |= 1u << S.mag_insert_ar_clip;
        reload_anim::Tick(player, G.anim_src ? G.anim_src : G.source, bones, clips);
        static uint64_t seen = 0;
        if (reload_anim::Generation() != seen) {
            seen = reload_anim::Generation();
            for (auto it = g_grip_cache.begin(); it != g_grip_cache.end();)
                it = it->first.find(".mag-insert") != std::string::npos ? g_grip_cache.erase(it) : std::next(it);
            for (int side = 0; side < 2; ++side)
                if (G.grip[side].kind == hands::GrabKind::kMagInsert) G.grip[side].object = 0;  // reload next frame
        }
    }

    CollectSolves();
    const hands::HandState live[2] = {hands::Get(hands::kLeft), hands::Get(hands::kRight)};
    hands::HandState hs[2] = {live[0], live[1]};
    hands::HandGrab grab[2] = {hands::GetGrab(hands::kLeft), hands::GetGrab(hands::kRight)};
    StepMeasure(dt);  // raw tracking; before collision, grip snapping or body fitting
    // Setup can start after controls published this frame's visual approach.
    // Withdraw that local preview immediately; it never owns the real grip.
    if (CalibrationActive()) for (auto& pose : grab) if (pose.holster_preview) pose = {};
    if (g_freeze.on)
        for (int i = 0; i < 2; ++i) hs[i] = g_freeze.hs[i], grab[i] = g_freeze.grab[i];

    // The upper body belongs to the player, not the flat-screen animation:
    // no recoil kick, aim pitch or idle sway in the spine and shoulders.
    if (!S.animated_upper_body) {
        auto reset = [&](int b) {
            if (b >= 0) G.pose.SetL(b, G.ref[b]);
        };
        for (int b : k.spine) reset(b);
        reset(k.neck);
        reset(k.head);
        for (int side = 0; side < 2; ++side) {
            if (!hs[side].valid) continue;
            reset(k.clavicle[side]);
            reset(k.upperarm[side]);
            reset(k.lowerarm[side]);
            reset(k.hand[side]);
            reset(k.upperarm_twist[side]);
            reset(k.forearm_twist[side]);
        }
    }

    // Animated pose, before we change anything.
    ue::Call ar(player, "K2_GetActorRotation");
    ar.Invoke();
    const double actor_yaw = ar.Result<FRotator>().Yaw;
    Xf pelvis = k.pelvis >= 0 ? WX(k.pelvis) : Xf{};
    Xf foot_anim[2];
    FVector hip_anim[2], knee_anim[2];
    for (int s = 0; s < 2; ++s) if (k.legs_ok[s]) {
        hip_anim[s] = WP(k.thigh[s]);
        knee_anim[s] = WP(k.calf[s]);
        foot_anim[s] = WX(k.foot[s]);
    }
    ue::Call actor_location(player, "K2_GetActorLocation");
    const FVector actor_at = actor_location.Invoke() ? actor_location.Result<FVector>() : g_w.translation;
    const double tick_ms = perf::NowMs();
    const auto origin_generation = stereo::OriginGeneration();
    const bool reset_motion = G.origin_generation != origin_generation ||
        (G.last_tick_ms > 0 && tick_ms - G.last_tick_ms > 250) || !std::isfinite(dt_f) || dt_f <= 0 || dt_f > 0.15f ||
        (G.have_actor && Length(actor_at - G.last_actor) > 100);
    if (reset_motion) {
        ResetHandPhysics();
        legs::Reset(G.legs);
        G.leg_probe = {};
        for (int side = 0; side < 2; ++side) {
            G.posture_state[side] = {}; G.arm_tracked[side] = false;
            G.twist_state[side] = {}; G.clav_amount[side] = 0;
        }
    }
    G.last_tick_ms = tick_ms; G.origin_generation = origin_generation;
    G.last_actor = actor_at; G.have_actor = true;
    ue::Call vel(player, "GetVelocity");
    const FVector v = vel.Invoke() ? vel.Result<FVector>() : FVector{};
    const double speed = std::hypot(v.X, v.Y);
    const double t_leg_query = perf::NowMs();
    const auto movement = leg_world::ReadState(player);
    const double leg_query_ms = perf::NowMs() - t_leg_query;
    // ALS terrain IK can lift its pelvis as well as its feet onto furniture.
    // Remove only excessive upward idle displacement. Keep ORIGINAL animated
    // leg snapshots above so this rebase cannot conceal the high-foot error.
    if (S.legs && k.pelvis >= 0 && G.reference_pelvis_height > 1) {
        legs::Input source;
        source.state_known = movement.known;
        source.floor_known = movement.floor_known;
        source.grounded = movement.grounded;
        source.kicking = movement.kicking;
        source.floor_z = movement.floor_z;
        source.speed = speed;
        for (int side=0; side<2; ++side) if (k.legs_ok[side]) {
            source.foot[side].valid = true;
            source.foot[side].ankle = foot_anim[side].translation;
            source.foot[side].sole_offset = G.sole_offset[side] * std::fabs(g_w.scale.Z);
        }
        const double nominal = movement.floor_z + G.reference_pelvis_height * std::fabs(g_w.scale.Z);
        const double corrected = legs::CorrectIdlePelvisHeight(source, pelvis.translation.Z, nominal);
        if (corrected < pelvis.translation.Z) {
            pelvis.translation.Z = corrected;
            SetWX(k.pelvis, pelvis);
        }
    }
    const bool have_shoulders = k.arms_ok[kL] && k.arms_ok[kR];
    const FVector sc_anim = have_shoulders ? (WP(k.upperarm[kL]) + WP(k.upperarm[kR])) * 0.5
                                           : (k.neck >= 0 ? WP(k.neck) : pelvis.translation);

    // Body yaw: follows the head past a threshold, leans toward where the
    // hands are, and catches up while moving.
    const double head_yaw = YawOf(head.rotation);
    double target_yaw = head_yaw;
    if (hs[0].valid && hs[1].valid && S.turn_toward_hands > 0) {
        FVector mid = (hs[0].grip.location + hs[1].grip.location) * 0.5 - head.location;
        mid.Z = 0;
        if (Length(mid) > 15.0) {
            double hands_yaw = std::atan2(mid.Y, mid.X) * kRadToDeg;
            double d = NormalizeAxis(hands_yaw - head_yaw);
            if (std::fabs(d) < 100.0) target_yaw += d * S.turn_toward_hands;
        }
    }
    const double head_rel = NormalizeAxis(target_yaw - actor_yaw);
    double diff = NormalizeAxis(head_rel - G.body_yaw);
    if (std::fabs(diff) > S.turn_threshold) {
        G.body_yaw += diff > 0 ? diff - S.turn_threshold : diff + S.turn_threshold;
        diff = NormalizeAxis(head_rel - G.body_yaw);
    }
    if (v.X * v.X + v.Y * v.Y > 30.0 * 30.0) G.body_yaw += diff * (1.0 - std::exp(-5.0 * dt));
    G.body_yaw = NormalizeAxis(G.body_yaw);
    const FQuat yaw_q = YawOnly(G.body_yaw);
    const FVector body_fwd = YawDir(actor_yaw + G.body_yaw);
    const FVector body_right = YawDir(actor_yaw + G.body_yaw + 90.0);

    // Upper-body dimensions do not depend on seated eye height. A successful
    // arm fit supplies real shoulder offsets; the unmeasured fallback is stable.
    const double physical_scale = stereo::TrackingScale();
    const double unit = 22.0 * physical_scale;
    const double below = S.shoulder_below > 0 ? S.shoulder_below : unit;
    const double behind = S.shoulder_behind > 0 ? S.shoulder_behind : unit * 0.5;
    FVector shoulder_offset{-behind, 0, -below};
    if (M.calibrated) shoulder_offset = (M.profile.arm[0].shoulder + M.profile.arm[1].shoulder) * (physical_scale * 0.5);
    const FVector sc_target = head.location + body_fwd * shoulder_offset.X + body_right * shoulder_offset.Y +
                              FVector{0,0,shoulder_offset.Z};
    const FVector sc_anim_r = pelvis.translation + yaw_q.Rotate(sc_anim - pelvis.translation);
    FVector delta = sc_target - sc_anim_r;
    if (Length(delta) > 500.0) delta = FVector{};
    // Preserve the game's pelvis and leg stance in fixed/seated modes.
    // Only real-height tracking can add crouch; the camera difference already
    // includes game-crouch compensation. There is no previous-frame offset to
    // carry across a mode change or to double a newly applied game crouch.
    torso::Input torso_input;
    const auto height_mode = CurrentHeightMode();
    torso_input.mode = height_mode == body_calibration::HeightMode::Real ? torso::Mode::Real :
                       height_mode == body_calibration::HeightMode::Seated ? torso::Mode::Seated : torso::Mode::Fixed;
    torso_input.source_pelvis_z = pelvis.translation.Z;
    torso_input.source_top_spine_z = k.spine.empty() ? pelvis.translation.Z : WP(k.spine.back()).Z;
    torso_input.source_shoulders_z = sc_anim_r.Z;
    torso_input.target_shoulders_z = sc_anim_r.Z + delta.Z;
    torso_input.head_z = head.location.Z;
    torso_input.max_crouch_cm = S.max_crouch;
    FVector game_camera;
    torso_input.camera_known = stereo::GameCameraLocation(&game_camera);
    torso_input.game_camera_z = game_camera.Z;
    const auto torso_fit = torso::Fit(torso_input);
    G.body_dz = torso_fit.valid ? torso_fit.pelvis_offset : 0;
    G.torso_spine_scale = torso_fit.valid ? torso_fit.spine_scale : 1;
    G.torso_shoulder_residual = torso_fit.valid ? torso_fit.shoulder_residual : 0;
    const double crouch = G.body_dz;
    G.spine_source_z.resize(k.spine.size());
    for (std::size_t i=0; i<k.spine.size(); ++i) G.spine_source_z[i] = WP(k.spine[i]).Z;
    FVector offset{delta.X, delta.Y, 0};
    // The body always goes under the head: a little of the offset is lean,
    // the rest moves the whole body. (It used to stop at 60 cm, so walking
    // around the room left the body behind and the camera ended up inside
    // it or far from it.)
    FVector lean = offset * 0.3;
    if (Length(lean) > S.max_lean) lean = Normalize(lean) * S.max_lean;
    FVector pelvis_shift = offset - lean;
    {
        static double last_far_log = -1e9;
        double off = Length(offset);
        if (off > 60.0 && perf::NowMs() - last_far_log > 10000.0) {
            last_far_log = perf::NowMs();
            log::Info("body: your head is %.0f cm from the game character (walked in the room?); the body follows it",
                      off);
        }
    }
    if (k.pelvis >= 0) {
        Xf p = pelvis;
        p.rotation = Normalized(yaw_q * p.rotation);
        p.translation = pelvis.translation + FVector{0, 0, crouch} + pelvis_shift;
        SetWX(k.pelvis, p);
    }
    if (have_shoulders && !k.spine.empty() && k.pelvis >= 0) {
        // Remap absolute snapshots, once. Positive bounded scale compresses
        // the distributed spine rather than pushing its first joint below the
        // pelvis. Clavicles and shoulder roots follow the last spine together;
        // SolveArm still uses the independently measured physical arm lengths.
        if (torso_fit.valid) for (std::size_t i=0; i<k.spine.size(); ++i) {
            Xf bone = WX(k.spine[i]);
            bone.translation.Z = torso_fit.MapSpineZ(G.spine_source_z[i]);
            SetWX(k.spine[i], bone);
        }
        FVector want = sc_anim_r + pelvis_shift + FVector{0, 0, crouch} + lean;
        if (torso_fit.valid) want.Z = torso_fit.fitted_shoulders_z;
        FVector base = WP(k.spine[0]);
        FVector now = (WP(k.upperarm[kL]) + WP(k.upperarm[kR])) * 0.5;
        FQuat bend = ClampAngle(Arc(now - base, want - base), S.max_spine_bend * kDegToRad);
        FQuat step = Scale(bend, 1.0 / k.spine.size());
        for (int b : k.spine) RotW(b, step);
    }

    if (S.hide_head && k.head >= 0) {
        Xf l = G.pose.L(k.head);
        l.scale = FVector{0.001, 0.001, 0.001};
        G.pose.SetL(k.head, l);
    }

    // Contact planning precedes arm solving: supporting feet can make a
    // bounded pelvis correction without invalidating already-solved arms.
    WalkCheckTick(player, speed, dt);
    NotifyStudyReport(dt);
    const double t_legs = perf::NowMs();
    legs::Input leg_input;
    leg_input.dt = reset_motion ? 0.0 : dt_f;
    leg_input.origin = actor_at;
    leg_input.forward = body_fwd;
    leg_input.velocity = v;
    leg_input.speed = speed;
    const int known = movement.known && movement.floor_known ? 1 : 0;
    if (known != G.movement_known) {
        G.movement_known = known;
        log::Info("body legs: movement/kick/floor queries %s", known ? "ready" : "unavailable; retaining game animation");
    }
    leg_input.state_known = movement.known;
    leg_input.grounded = movement.grounded;
    leg_input.kicking = movement.kicking;
    leg_input.floor_known = movement.floor_known;
    leg_input.floor_z = movement.floor_z;
    auto move_animation = [&](const FVector& at) {
        return pelvis.translation + pelvis_shift + yaw_q.Rotate(at - pelvis.translation);
    };
    legs::Ground ground[2];
    if (S.legs && k.pelvis >= 0) {
        for (int side = 0; side < 2; ++side) {
            if (!k.legs_ok[side]) continue;
            auto& f = leg_input.foot[side];
            f.valid = true;
            f.hip = WP(k.thigh[side]);
            auto world_length = [&](const FVector& a) {
                return Length(FVector{a.X*g_w.scale.X, a.Y*g_w.scale.Y, a.Z*g_w.scale.Z});
            };
            f.upper_length = world_length(G.leg_upper_cs[side]);
            f.lower_length = world_length(G.leg_lower_cs[side]);
            f.animated_hip = move_animation(hip_anim[side]);
            f.knee = move_animation(knee_anim[side]);
            f.ankle = move_animation(foot_anim[side].translation);
            f.rotation = Normalized(yaw_q * foot_anim[side].rotation);
            f.sole_offset = G.sole_offset[side] * std::fabs(g_w.scale.Z);
            f.sole_up_local = G.sole_up_local[side];
            const FVector ref = G.stance_from_pelvis[side];
            const FVector scaled{ref.X*g_w.scale.X, ref.Y*g_w.scale.Y, ref.Z*g_w.scale.Z};
            f.stance_valid = G.leg_ref[side].valid && movement.floor_known;
            f.stance_ankle = WX(k.pelvis).translation + yaw_q.Rotate(g_w.rotation.Rotate(scaled));
            f.stance_ankle.Z = movement.floor_z + f.sole_offset;
            f.stance_rotation = Normalized(yaw_q * g_w.rotation * G.stance_rotation[side]);
            f.stance_pole = body_fwd;
        }
        leg_world::ProbeFeet(G.leg_probe, player, leg_input, ground);
        const auto plan = legs::Plan(G.legs, leg_input, ground);
        ++G.fit_leg_frames;
        if (!plan.foot[0].solve && !plan.foot[1].solve) ++G.fit_leg_bypass;
        for (int side=0; side<2; ++side)
            if (G.legs.foot[side].repairing) ++G.fit_leg_recovery[side];
        G.steps_taken = static_cast<int>(plan.steps_taken);
        if (Length(plan.pelvis_offset) > 0) {
            Xf p = WX(k.pelvis); p.translation = p.translation + plan.pelvis_offset; SetWX(k.pelvis, p);
        }
        for (int side = 0; side < 2; ++side) {
            const auto& f = leg_input.foot[side];
            const auto& target = plan.foot[side];
            if (!target.solve || !k.legs_ok[side]) continue;
            const double upper = f.upper_length, lower = f.lower_length;
            const auto solved = SolveTwoBone(WP(k.thigh[side]), target.ankle, upper, lower, target.pole, 0);
            const auto mapped = arm_pose::Orient(G.leg_ref[side], WP(k.thigh[side]), solved, target.pole);
            if (!mapped.valid) continue;
            Xf thigh = WX(k.thigh[side]); thigh.rotation = mapped.upper; SetWX(k.thigh[side], thigh);
            Xf calf = WX(k.calf[side]); calf.translation = solved.mid; calf.rotation = mapped.forearm; SetWX(k.calf[side], calf);
            Xf foot = WX(k.foot[side]); foot.rotation = target.rotation; foot.translation = solved.end;
            SetWX(k.foot[side], foot);
        }
    } else { legs::Reset(G.legs); G.leg_probe = {}; }
    PoseKick(player, movement.known && movement.kicking, YawOnly(actor_yaw + G.body_yaw), yaw_q);
    g_perf_legs.Add(perf::NowMs() - t_legs + leg_query_ms);

    if (!CalibrationActive()) UpdateEditor(grab, hs, live, dt);
    for (int side = 0; side < 2; ++side) g_raw_grip[side] = hs[side].grip;
    if (!g_freeze.on && !CalibrationActive()) {
        for (int side = 0; side < 2; ++side) hand_walls::SetPalmShape(side, CollisionPalm(side, hs[side]));
        hand_walls::Apply(player, head, hs, grab, dt);
    } else ResetHandPhysics();
    const double t_arms = perf::NowMs();
    for (int side = 0; side < 2; ++side) {
        if (!hs[side].valid) {
            ResetHandPhysics(side);
            G.arm_tracked[side] = false; G.posture_state[side] = {}; G.twist_state[side] = {}; continue;
        }
        if (hand_walls::Motion(side).reset) {
            ResetFingerContacts(side);
            G.cur_init[side] = false;
        }
        if (!G.arm_tracked[side]) {
            G.posture_state[side] = {}; G.twist_state[side] = {};
        }
        G.arm_tracked[side] = true;
        SolveArm(side, hs[side], grab[side], GunGrab(grab, grab[side]), body_fwd, body_right, dt);
    }
    g_perf_arms.Add(perf::NowMs() - t_arms);

    // Gear slots on the posed body.
    if (k.pelvis >= 0) {
        FQuat body_q = YawOnly(actor_yaw + G.body_yaw);
        FVector belt = WP(k.pelvis);
        FVector chest = k.spine.empty() ? belt : WP(k.spine.back());
        for (int i = 0; i < static_cast<int>(Slot::kCount); ++i) {
            g_slots[i].location = (kSlots[i].chest ? chest : belt) + body_q.Rotate(kSlots[i].offset);
            g_slots[i].rotation = body_q;
        }
        const FVector fwd = body_q.Rotate(FVector{1, 0, 0});
        g_anchors[static_cast<int>(Anchor::kBelt)] = {belt, body_q};
        g_anchors[static_cast<int>(Anchor::kChest)] = {
            chest, k.spine.empty() ? body_q : interact::FrameFromUp(chest - belt, fwd)};
        for (int s = 0; s < 2; ++s) {
            hands::Pose& a = g_anchors[static_cast<int>(s == kL ? Anchor::kThighL : Anchor::kThighR)];
            if (S.legs && k.legs_ok[s]) {
                const FVector hip = WP(k.thigh[s]), knee = WP(k.calf[s]);
                a = {(hip + knee) * 0.5, interact::FrameFromUp(hip - knee, fwd)};
            } else {
                a = {belt + body_q.Rotate(FVector{0, s == kL ? -10.0 : 10.0, -22.0}), body_q};
            }
        }
        g_slots_valid = true;
    }
    g_perf_solve.Add(perf::NowMs() - t_solve);

    int written;
    {
        perf::Scope t(g_perf_engine);
        written = G.engine.Write(G.pose);
    }
    FitLog(dt, NormalizeAxis(head_rel - G.body_yaw));
    if (G.frames == 1 || G.frames % 3000 == 0)
        log::Info("body: frame %d, %d bones written (%s path), yaw %.0f, lean %.0f", G.frames, written,
                  G.engine.Fast() ? "fast" : "slow", G.body_yaw, Length(lean));
}

}  // namespace

bool GetSlot(Slot slot, hands::Pose* out) {
    int i = static_cast<int>(slot);
    if (!g_slots_valid || i < 0 || i >= static_cast<int>(Slot::kCount)) return false;
    *out = g_slots[i];
    return true;
}

bool PinchPoint(int side, FVector* thumb_in_grip, FVector* index_in_grip) {
    if (side < 0 || side > 1) return false;
    std::lock_guard<std::mutex> lock(g_pinch_mutex);
    const PinchNote& p = g_pinch[side];
    if (!p.valid || perf::NowMs() - p.ms > 500.0) return false;
    *thumb_in_grip = p.thumb;
    *index_in_grip = p.index;
    return true;
}

bool GetHandContacts(int side, HandContacts* out) {
    if (side < 0 || side > 1 || !out) return false;
    std::lock_guard<std::mutex> lock(g_pinch_mutex);
    const ContactNote& n = g_contact[side];
    if (!n.valid || perf::NowMs() - n.ms > 100.0) return false;
    *out = n.c;
    return true;
}

bool GetForearm(int side, watch::Forearm* out) {
    if (!out || side < 0 || side > 1 || !G.active || G.ref_pending ||
        G.watch_arm_ms[side] <= 0 || perf::NowMs() - G.watch_arm_ms[side] > 100.0) return false;
    *out = G.watch_arm[side];
    return true;
}

bool SlideMarks(uintptr_t gun, FVector* rear_sight, FVector* rear_end, FVector* front_end) {
    // The hand that holds the gun by its grip (either hand can).
    hands::HandGrab r = hands::GetGrab(hands::kRight);
    const hands::HandGrab l = hands::GetGrab(hands::kLeft);
    if (!(hands::HeldGrabKind(r) == hands::GrabKind::kGunGrip && r.object == gun) &&
        hands::HeldGrabKind(l) == hands::GrabKind::kGunGrip && l.object == gun)
        r = l;
    if (!gun) return false;
    // Measured a little each frame (see SlideShape); until then, false.
    static const double budget = config::GetFloat("slide_measure_ms_per_frame", 1.0f);
    if (g_shape.gun != gun || !g_shape.tried) {
        // Measure only with the gun frame from the hand that holds it.
        if (hands::HeldGrabKind(r) != hands::GrabKind::kGunGrip || r.object != gun) return false;
        return SlideMarksWorld(gun, r.target.rotation, rear_sight, rear_end, front_end, budget);
    }
    return SlideMarksWorld(gun, r.target.rotation, rear_sight, rear_end, front_end, budget);
}

bool GetAnchor(Anchor anchor, hands::Pose* out) {
    int i = static_cast<int>(anchor);
    if (!g_slots_valid || i < 0 || i >= static_cast<int>(Anchor::kCount)) return false;
    *out = g_anchors[i];
    return true;
}

void RequestMeasure() { g_measure_requested = true; }
bool CalibrationActive() { return g_calibration_active.load(); }
void CancelCalibration() { g_calibration_cancel = true; SetCalibrationTriggers(false,false,false,true); }
void SkipCalibrationArms() { if (g_calibration_active) { g_calibration_skip_arms = true; SetCalibrationTriggers(false,false,false,true); } }
void SetCalibrationTriggers(bool left_down,bool right_down,bool valid,bool interrupted) {
    g_calibration_trigger_bits = g_calibration_active ? unsigned(left_down)|(unsigned(right_down)<<1)|(unsigned(valid)<<2) : 0;
    if(interrupted || !valid) g_calibration_trigger_interrupted=true;
    if((!valid || interrupted || !left_down || !right_down) && g_calibration_ramping.exchange(false)) {
        // Queue stops immediately, even if this game tick has no new tracked
        // pose. The helper controls rearming when body sampling resumes.
        input::StopHaptic(input::kLeft); input::StopHaptic(input::kRight);
    }
}
CalibrationHud GetCalibrationHud() {
    std::lock_guard<std::mutex> lock(g_calibration_hud_mutex);
    CalibrationHud hud = g_calibration_hud;
    hud.visible = hud.visible && perf::NowMs() < g_calibration_hud_until;
    return hud;
}

void Init() {
    LoadSettings();
    hand_walls::LoadSettings();
    finger_world::LoadSettings();
    config::AddChangeListener(&LoadSettings);
    config::AddChangeListener(&hand_walls::LoadSettings);
    config::AddChangeListener(&finger_world::LoadSettings);
    config::AddChangeListener([] { ResetHandPhysics(); });
    game::AddTickListener(&OnTick);
    NotifyStudyInit();
    log::Info("body: ready (%s; F9 toggles; grip editor: hold left Y 1 s while holding the gun)",
              S.enabled ? "on" : "off");
}

}  // namespace holevr::vr::body
