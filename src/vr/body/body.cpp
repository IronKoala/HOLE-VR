#include "vr/body/body.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
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
#include "vr/body/finger_solve.h"
#include "vr/body/grip_editor.h"
#include "vr/body/grip_record.h"
#include "vr/body/hand_pose.h"
#include "vr/body/ik_math.h"
#include "vr/body/skeleton.h"
#include "vr/hands.h"
#include "vr/input.h"

namespace holevr::vr::body {
namespace {

using ue::FName;
using ue::UObject;
using namespace ik;
using skel::Xf;
namespace gr = grip_record;

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
    double shoulder_below = 0.0;   // 0 = from eye height
    double shoulder_behind = 0.0;
    double forearm_roll_share = 0.5;
    double twist_bone_share = 1.0;
    double elbow_follow_hand = 0.7;
    bool elbow_relieves_wrist = true;
    bool grip_records = true;
    double finger_radius = 0.5;
    bool editor_markers = true;
    bool gun_wrist_straight = true;
    bool fit_log = true;
    bool fixed_height = true;
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
    S.shadow = config::GetInt("body_shadow", 1);
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
    S.twist_bone_share = std::clamp<double>(config::GetFloat("body_forearm_twist_bone_share", 1.0f), 0.0, 1.0);
    S.elbow_follow_hand = std::clamp<double>(config::GetFloat("body_elbow_follow_hand", 0.7f), 0.0, 2.0);
    S.elbow_relieves_wrist = config::GetBool("body_elbow_relieves_wrist", true);
    S.grip_records = config::GetBool("hand_grip_records", true);
    S.finger_radius = config::GetFloat("hand_finger_radius_cm", 0.5f);
    S.editor_markers = config::GetBool("hand_editor_show_grab_points", true);
    S.gun_wrist_straight = config::GetBool("body_gun_wrist_straight", true);
    S.fit_log = config::GetBool("body_fit_log", true);
    S.fixed_height = config::GetBool("fixed_height", true);
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
    fclose(f);
    return ok;
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
};

// The grip a hand is using right now.
struct ActiveGrip {
    std::string key;            // file name without extension
    UObject object = 0;
    hands::GrabKind kind = hands::GrabKind::kNone;
    bool have = false;          // a record is in use
    bool from_file = false;
    gr::Record rec;
    FQuat hand_rot;             // hand bone in the object frame
    FVector hand_pos;
    fingers::Angles tip_angles[gr::kTipCount];
    double err[gr::kTipCount] = {};
    double clip[gr::kTipCount] = {};
    double palm_clip = 0;
    double web_clip = 0;  // web between thumb and index (meets the tang)
};

struct State {
    UObject player = 0;
    UObject source = 0;  // the game's BodyMesh
    UObject pose_comp = 0;
    EnginePose engine;
    skel::Pose pose;
    std::vector<Xf> ref;    // reference (rest) pose, local
    std::vector<Xf> frame_local;
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
    FVector elbow_hinge_local[2];
    bool hinge_known[2] = {false, false};
    FVector knee_hinge_local[2];
    bool knee_known[2] = {false, false};
    double clav_amount[2] = {0, 0};
    double stand_dz = -1e9;
    double body_dz = 0;  // whole body moved up/down so the shoulders sit under the eyes
    int frames = 0;
    // fit log
    double fit_time = 0;
    double fit_reach[2] = {0, 0}, fit_gap[2] = {0, 0}, fit_flex[2] = {0, 0}, fit_dev[2] = {0, 0};
    double fit_swivel[2] = {0, 0}, fit_twist[2] = {0, 0};
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
    double reach[2] = {0, 0};
} M;

void LoadMeasure() {
    M.loaded = true;
    std::string text;
    if (!ReadText(ModPath(L"body-measure.ini"), &text)) {
        log::Info("body: arms not measured yet (stand straight, arms out to the sides, hold the left stick click 2 s)");
        return;
    }
    double s = 0;
    const char* p = std::strstr(text.c_str(), "arm_scale=");
    if (p && std::sscanf(p + 10, "%lf", &s) == 1 && s > 0.7 && s < 1.5) {
        M.have = true;
        M.arm_scale = s;
        log::Info("body: arm scale %.3f from your measurement", s);
    }
}

double ArmScale() {
    if (S.arm_scale_override > 0.0) return S.arm_scale_override;
    return M.have ? M.arm_scale : 1.0;
}
double MaxStretch() { return M.have || S.arm_scale_override > 0.0 ? S.max_stretch : S.max_stretch_unmeasured; }

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

perf::Counter g_perf_tick{"body tick"};
perf::Counter g_perf_engine{"body engine read+write"};
perf::Counter g_perf_solve{"body solve"};
perf::Counter g_perf_grip{"body grip solve (once per grip)"};

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

void ApplyShadows() {
    CallBool(G.source, "SetCastHiddenShadow", "NewCastHiddenShadow", S.shadow == 1);
    CallBool(G.pose_comp, "SetCastShadow", "NewCastShadow", S.shadow == 2);
}

void StopEditor();

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
    StopEditor();
    if (!G.active) return;
    for (UObject f : G.followers) SetLeader(f, G.source);
    G.followers.clear();
    if (G.pose_comp) SetVisible(G.pose_comp, false);
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
    if (player == G.player && source == G.source && G.pose_comp) return true;
    if (G.failed && player == G.player && source == G.source) return false;
    StopEditor();
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
    G.ref_pending = true;
    return true;
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
    log::Info("body: %s hand: middle knuckle %.1f cm from the wrist, fingers %.1f %.1f %.1f %.1f %.1f cm "
              "(thumb..pinky), handedness %.2f%s",
              side == kL ? "left" : "right", Length(mcp_m), lengths[0], lengths[1], lengths[2], lengths[3], lengths[4],
              check, r.ok ? "" : " (FAILED: no stored grips for this hand)");
}

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
    const Skeleton& k = G.skel;
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

bool IsGrab(hands::GrabKind k) {
    return k == hands::GrabKind::kGunGrip || k == hands::GrabKind::kSlideGrab || k == hands::GrabKind::kMagHeld ||
           k == hands::GrabKind::kMagInsert || k == hands::GrabKind::kSupportGrip;
}

// The frame a grip is stored in. Gun: the gun frame at its grip point.
// Slide / charging handle / rifle handguard: the gun frame at the point the
// hand holds. Magazine: the magazine itself.
bool ObjectFrame(const hands::HandGrab& grab, const hands::HandGrab& right, hands::Pose* out) {
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

std::string RecordKey(const hands::HandGrab& grab) {
    std::string cls = grab.object ? ue::GetName(ue::GetClass(grab.object)) : "unknown";
    switch (grab.kind) {
        case hands::GrabKind::kGunGrip: return cls + ".grip";
        case hands::GrabKind::kSlideGrab: return cls + ".slide";
        case hands::GrabKind::kSupportGrip: return cls + ".support";
        default: return cls + ".mag";
    }
}

std::wstring RecordPath(const std::string& key) { return ModPath(L"grips\\" + Widen(key) + L".ini"); }

fingers::DepthFn DepthIn(const ActiveGrip& g) {
    return [&g](const FVector& p) { return gr::Depth(g.rec, g.hand_pos + g.hand_rot.Rotate(p)); };
}

// Solves the fingers for the record's targets. tip = -1: all of them;
// quick: refine from the current solution (live editing).
void SolveGrip(int side, int only_tip, bool quick) {
    perf::Scope t(g_perf_grip);
    ActiveGrip& g = G.grip[side];
    const HandRig& r = G.rig[side];
    gr::HandInObject(g.rec, r.geo, &g.hand_rot, &g.hand_pos);
    fingers::DepthFn fn = DepthIn(g);
    for (int tip = 0; tip < gr::kTipCount; ++tip) {
        if (only_tip >= 0 && tip != only_tip) continue;
        const int f = TipFinger(tip);
        FVector target = g.hand_rot.Inverse().Rotate(g.rec.tip[tip] - g.hand_pos);
        g.tip_angles[tip] = fingers::Solve(r.chain[f], target, fingers::DefaultLimits(f == hand_pose::kThumb),
                                           &g.err[tip], g.rec.outline.empty() ? nullptr : &fn, S.finger_radius,
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
}

void ReportGrip(int side, const char* why) {
    const ActiveGrip& g = G.grip[side];
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
    const HandRig& r = G.rig[side];
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

void LoadGrip(int side, const hands::HandGrab& grab, bool from_disk = false) {
    ActiveGrip& g = G.grip[side];
    g = ActiveGrip{};
    g.object = grab.object;
    g.kind = grab.kind;
    if (!S.grip_records || !G.rig[side].ok || !IsGrab(grab.kind) || !grab.object) return;
    g.key = RecordKey(grab);
    auto cached = g_grip_cache.find(CacheKey(side, g.key));
    if (!from_disk && cached != g_grip_cache.end()) {
        g = cached->second;
        g.object = grab.object;
        g.kind = grab.kind;
        return;
    }
    std::string text;
    gr::Record rec;
    bool have = false;
    if (grab.kind == hands::GrabKind::kGunGrip) {
        rec = gr::DefaultPistol();
        rec.name = g.key;
        have = true;
    }
    if (ReadText(RecordPath(g.key), &text)) {
        if (!have) rec.right_hand = side == kR;
        if (gr::Parse(text, &rec)) {
            have = true;
            g.from_file = true;
            if (grab.kind == hands::GrabKind::kGunGrip && gr::HasOldPistolHand(rec)) {
                // Saved before the hand was lined up with the barrel: keep the
                // fingertips, take the new hand and outline.
                gr::PistolHand(&rec);
                rec.outline = gr::DefaultPistol().outline;
                log::Info("body: grip %s had the old hand placement; moved the hand in line with the barrel "
                          "(your fingertip positions are kept; tap Y in the editor to save it)",
                          g.key.c_str());
            }
        } else {
            log::Error("body: grip file %s.ini could not be read, using the built-in grip", g.key.c_str());
        }
    }
    if (!have) return;
    if (rec.right_hand != (side == kR)) rec = gr::Mirror(rec);
    g.rec = rec;
    g.have = true;
    double t0 = perf::NowMs();
    SolveGrip(side, -1, false);
    char why[64];
    std::snprintf(why, sizeof(why), "solved in %.1f ms", perf::NowMs() - t0);
    ReportGrip(side, why);
    g_grip_cache[CacheKey(side, g.key)] = g;
}

// ---- grip editor ----
grip_editor::Editor g_editor;
int g_edit_side = -1;      // hand whose grip is edited
int g_edit_ctrl = -1;      // controller doing the editing
FVector g_tip_world[2][gr::kTipCount];
FVector g_palm_world[2];
Xf g_hand_world[2];
bool g_tips_valid[2] = {false, false};

void HideMarkers();
void StopEditor() {
    if (g_editor.Active()) log::Info("body: grip editor off");
    g_editor.Stop();
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
enum Marker : int { kMkThumb, kMkIndex, kMkMiddle, kMkRing, kMkPinky, kMkPalm, kMkPointer, kMkCount };
struct EditorMarkers {
    bool tried = false;
    UObject mesh = 0;
    double mesh_size = 100;  // cm across at scale 1
    UObject owner = 0;
    UObject comp[kMkCount] = {};
    bool shown = false;
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
void ShowMarkers(UObject player, int side, int index_tip, int hover, const FVector& pointer) {
    if (!S.editor_markers || !MakeMarkers(player)) return;
    if (!g_mk.shown) {
        for (UObject c : g_mk.comp) SetVisible(c, true);
        g_mk.shown = true;
    }
    auto item_of = [](int m, int index_tip) {
        switch (m) {
        case kMkThumb: return static_cast<int>(gr::kThumb);
        case kMkIndex: return index_tip;
        case kMkMiddle: return static_cast<int>(gr::kMiddle);
        case kMkRing: return static_cast<int>(gr::kRing);
        case kMkPinky: return static_cast<int>(gr::kPinky);
        default: return grip_editor::kHandItem;
        }
    };
    for (int m = 0; m < kMkPointer; ++m) {
        FVector at = m == kMkPalm ? g_palm_world[side] : g_tip_world[side][item_of(m, index_tip)];
        bool hot = item_of(m, index_tip) == hover;
        double d = m == kMkPalm ? (hot ? 4.0 : 2.0) : (hot ? 2.6 : 1.1);
        PlaceMarker(m, at, d);
    }
    PlaceMarker(kMkPointer, pointer, 0.8);
}

void UpdateEditor(const hands::HandGrab grab[2], const hands::HandState hs[2], double dt) {
    const hands::EditButtons b = hands::GetEditButtons();
    // Which grip: a magazine or the slide in the left hand first (the right
    // controller edits), else the gun in the right hand (the left edits).
    int side = -1;
    if (IsGrab(grab[kL].kind) && grab[kL].kind != hands::GrabKind::kGunGrip) side = kL;
    else if (grab[kR].kind == hands::GrabKind::kGunGrip) side = kR;
    if (g_editor.Active() && (side != g_edit_side || side < 0 || G.grip[side].object != grab[side].object)) {
        log::Info("body: grip editor stopped (the held object changed)");
        StopEditor();
    }
    if (side < 0 || !G.rig[side].ok || !g_tips_valid[side]) {
        if (g_editor.Active()) StopEditor();
        return;
    }
    const int ctrl = 1 - side;
    hands::Pose obj;
    if (!ObjectFrame(grab[side], grab[kR], &obj) || !hs[ctrl].valid) return;
    const FQuat inv = obj.rotation.Inverse();
    auto local = [&](const FVector& w) { return inv.Rotate(w - obj.location); };

    grip_editor::Input in;
    in.dt = dt;
    in.y_down = b.y_down;
    in.stick_click_pressed = b.left_stick_pressed;
    in.grab_down = b.grip_down[ctrl];
    in.pointer = local(hs[ctrl].grip.location + hs[ctrl].aim.rotation.Rotate(FVector{3, 0, 0}));
    in.pointer_rot = Normalized(inv * hs[ctrl].grip.rotation);
    grip_editor::Scene sc;
    for (int t = 0; t < gr::kTipCount; ++t) sc.tip[t] = local(g_tip_world[side][t]);
    sc.palm = local(g_palm_world[side]);
    sc.index_state = G.index_pull[side] > 0.5 ? gr::kIndexPull : G.index_touch[side] > 0.5 ? gr::kIndexRest
                                                                                             : gr::kIndexOff;
    ActiveGrip& g = G.grip[side];
    FQuat hand_rot = g.hand_rot;
    FVector hand_pos = g.hand_pos;
    grip_editor::Output out = g_editor.Update(in, sc, &g.rec, &hand_rot, &hand_pos);
    if (out.entered) {
        g_edit_side = side;
        g_edit_ctrl = ctrl;
        hands::SetEditorHand(ctrl);
        if (!g.have) {
            // New record from the current hand: its pose and fingertips now.
            g.key = RecordKey(grab[side]);
            g.rec = gr::Record{};
            g.rec.name = g.key;
            g.rec.right_hand = side == kR;
            const Xf& hw = g_hand_world[side];  // last frame's posed hand
            gr::StoreHand(&g.rec, G.rig[side].geo, Normalized(inv * hw.rotation), local(hw.translation));
            for (int t = 0; t < gr::kTipCount; ++t) g.rec.tip[t] = sc.tip[t];
            g.have = true;
            g.object = grab[side].object;
            g.kind = grab[side].kind;
            SolveGrip(side, -1, false);
        }
        log::Info("body: grip editor on for %s (edit with the %s controller: squeeze near a fingertip or the palm "
                  "and move; tap Y to save, click the left stick to undo, hold Y 1 s to save and finish)",
                  g.key.c_str(), ctrl == kL ? "left" : "right");
    }
    if (out.changed) {
        if (out.grabbed == grip_editor::kHandItem) {
            gr::StoreHand(&g.rec, G.rig[side].geo, hand_rot, hand_pos);
            SolveGrip(side, -1, true);
        } else if (out.grabbed >= 0) {
            SolveGrip(side, out.grabbed, true);
        }
    }
    static int last_grab = grip_editor::kNoItem;
    if (out.grabbed != last_grab) {
        if (out.grabbed == grip_editor::kHandItem) log::Info("body: editor: moving the hand");
        else if (out.grabbed >= 0) log::Info("body: editor: moving the %s fingertip", gr::TipName(out.grabbed));
        else if (last_grab != grip_editor::kNoItem) {
            const ActiveGrip& gg = G.grip[side];
            if (last_grab >= 0)
                log::Info("body: editor: %s tip at (%.1f %.1f %.1f), %.1f cm off, %.1f cm inside", gr::TipName(last_grab),
                          gg.rec.tip[last_grab].X, gg.rec.tip[last_grab].Y, gg.rec.tip[last_grab].Z, gg.err[last_grab],
                          gg.clip[last_grab]);
            else
                log::Info("body: editor: hand knuckle at (%.1f %.1f %.1f), palm %.1f cm inside", gg.rec.knuckle.X,
                          gg.rec.knuckle.Y, gg.rec.knuckle.Z, gg.palm_clip);
        }
        last_grab = out.grabbed;
    }
    if (out.revert) {
        LoadGrip(side, grab[side], true);
        if (!G.grip[side].have) StopEditor();
        log::Info("body: editor: back to the saved grip");
    }
    if (out.save) SaveGrip(side);
    if (out.exited) StopEditor();
    // Grab points: a ball on each fingertip and the palm; the one a squeeze
    // would take grows, with a light tick in the editing hand when it changes.
    static int last_hover = grip_editor::kNoItem;
    if (g_editor.Active()) {
        FVector pointer_w = hs[ctrl].grip.location + hs[ctrl].aim.rotation.Rotate(FVector{3, 0, 0});
        ShowMarkers(G.player, side, sc.index_state, out.hover, pointer_w);
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

// ---- one-time body measure ----
struct MeasureRun {
    double hold = 0;     // left stick held
    double countdown = -1;
    int last_beat = -1;
} g_meas;

void StepMeasure(const hands::HandState hs[2], const FVector& shoulder_centre, const FVector& body_right, double dt) {
    const hands::EditButtons b = hands::GetEditButtons();
    if (g_editor.Active()) {
        g_meas = MeasureRun{};
        return;
    }
    if (g_meas.countdown < 0) {
        g_meas.hold = b.left_stick_down ? g_meas.hold + dt : 0;
        if (g_meas.hold >= 2.0) {
            g_meas.countdown = 3.0;
            g_meas.last_beat = 3;
            g_meas.hold = 0;
            log::Info("body: measuring your arms in 3 s: stand straight, arms straight out to the sides");
            input::Haptic(input::kLeft, 0.6f, 0.1f);
            input::Haptic(input::kRight, 0.6f, 0.1f);
        }
        return;
    }
    g_meas.countdown -= dt;
    int beat = static_cast<int>(std::ceil(g_meas.countdown));
    if (beat != g_meas.last_beat && beat > 0) {
        g_meas.last_beat = beat;
        input::Haptic(input::kLeft, 0.3f, 0.05f);
        input::Haptic(input::kRight, 0.3f, 0.05f);
    }
    if (g_meas.countdown > 0) return;
    g_meas = MeasureRun{};
    const Skeleton& k = G.skel;
    if (!hs[0].valid || !hs[1].valid || !k.arms_ok[0] || !k.arms_ok[1]) {
        log::Error("body: arm measure failed (controllers not tracked)");
        return;
    }
    skel::Pose refp;
    refp.Init(G.engine.parent);
    refp.SetAllLocal(G.ref.data(), static_cast<int>(G.ref.size()));
    double half_width = 0.5 * Length(refp.C(k.upperarm[kL]).translation - refp.C(k.upperarm[kR]).translation);
    double scale_sum = 0, reach[2] = {0, 0};
    for (int s = 0; s < 2; ++s) {
        double upper = Length(refp.C(k.lowerarm[s]).translation - refp.C(k.upperarm[s]).translation);
        double lower = Length(refp.C(k.hand[s]).translation - refp.C(k.lowerarm[s]).translation);
        FVector shoulder = shoulder_centre + body_right * (s == kL ? -half_width : half_width);
        reach[s] = Length(hs[s].grip.location - shoulder);
        double wrist = reach[s] - Length(G.rig[s].frame.fist);
        scale_sum += wrist / (upper + lower);
    }
    double scale = std::clamp(scale_sum * 0.5, 0.8, 1.35);
    double spread = Length(hs[0].grip.location - hs[1].grip.location);
    char text[256];
    std::snprintf(text, sizeof(text),
                  "; Written by the arm measure (left stick click 2 s, arms out). Delete to measure again.\n"
                  "arm_scale=%.3f\nreach_left_cm=%.1f\nreach_right_cm=%.1f\nhand_span_cm=%.1f\n",
                  scale, reach[0], reach[1], spread);
    WriteText(ModPath(L"body-measure.ini"), text);
    M.have = true;
    M.arm_scale = scale;
    log::Info("body: measured your arms: shoulder to hand left %.0f cm, right %.0f cm, hand span %.0f cm -> arm scale "
              "%.3f (saved)",
              reach[0], reach[1], spread, scale);
    input::Haptic(input::kLeft, 0.8f, 0.2f);
    input::Haptic(input::kRight, 0.8f, 0.2f);
}

// ---- solving ----
FVector YawDir(double yaw_deg) {
    double r = yaw_deg * kDegToRad;
    return {std::cos(r), std::sin(r), 0};
}

// Rotates the root bone of a two-bone chain so its middle joint lands on
// sol.mid, bending in the plane the solver chose; places the middle joint
// exactly (arm length scale / stretch is bone movement, never bone scale);
// then turns the middle bone toward sol.end.
void SolveLimb(int root_b, int mid_b, int end_b, const TwoBoneResult& sol, FVector* hinge_local, bool* hinge_known) {
    Xf root = WX(root_b);
    FVector mid = WP(mid_b), end = WP(end_b);
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
    RotW(root_b, q);
    Xf m = WX(mid_b);
    m.translation = sol.mid;
    SetWX(mid_b, m);
    RotW(mid_b, Arc(WP(end_b) - sol.mid, sol.end - sol.mid));
}

// Wrist bend (radians) for a forearm direction and hand rotation: flex
// (toward the palm/back of the hand) and deviation (sideways), measured from
// the avatar's straight wrist.
void WristBend(const HandRig& r, const FQuat& hand_rot, const FVector& forearm, double* flex, double* dev) {
    FVector d = hand_rot.Inverse().Rotate(Normalize(forearm));
    FVector e1 = Normalize(r.forearm_dir);
    FVector e2 = Normalize(Reject(r.palm_h, e1));
    FVector e3 = Cross(e1, e2);
    *flex = std::atan2(Dot(d, e2), Dot(d, e1));
    *dev = std::atan2(Dot(d, e3), Dot(d, e1));
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

void SolveArm(int side, const hands::HandState& h, const hands::HandGrab& grab, const hands::HandGrab& right_grab,
              const FVector& body_fwd, const FVector& body_right, double dt) {
    const Skeleton& k = G.skel;
    if (!k.arms_ok[side]) return;
    const HandRig& rig = G.rig[side];
    const hands::Hand hs = side == kL ? hands::kLeft : hands::kRight;
    const hands::FingerInput fin = hands::GetFingers(hs);

    // A new grab: load its stored grip.
    ActiveGrip& g = G.grip[side];
    if (grab.object != g.object || grab.kind != g.kind) {
        if (g_editor.Active() && g_edit_side == side) StopEditor();
        LoadGrip(side, grab);
    }
    hands::Pose obj;
    const bool recorded = g.have && rig.ok && ObjectFrame(grab, right_grab, &obj);

    // Where the hand goes.
    FQuat hand_rot;
    FVector hand_pos;
    if (recorded) {
        hand_rot = Normalized(obj.rotation * g.hand_rot);
        hand_pos = obj.location + obj.rotation.Rotate(g.hand_pos);
    } else {
        FVector fist = IsGrab(grab.kind) ? grab.target.location : h.grip.location;
        fist = fist + h.grip.rotation.Rotate(FVector{S.hand_offset[0], S.hand_offset[1], S.hand_offset[2]});
        FVector fwd = h.grip.rotation.Rotate(FVector{1, 0, 0});
        hand_rot = rig.frame.valid ? HandRotation(rig.frame, fwd, h.palm_normal) : h.grip.rotation;
        if (S.hand_rot[0] || S.hand_rot[1] || S.hand_rot[2]) {
            double mirror = side == kL ? -1.0 : 1.0;
            hand_rot = Normalized(h.grip.rotation *
                                  ToQuat(FRotator{S.hand_rot[0], S.hand_rot[1] * mirror, S.hand_rot[2] * mirror}) *
                                  h.grip.rotation.Inverse() * hand_rot);
        }
        hand_pos = rig.frame.valid ? fist - hand_rot.Rotate(rig.frame.fist) : fist;
    }

    FVector shoulder = WP(k.upperarm[side]);
    const Xf fore_local = G.pose.L(k.lowerarm[side]), hand_local = G.pose.L(k.hand[side]);
    const double scale = ArmScale();
    const double upper = Length(fore_local.translation) * scale, lower = Length(hand_local.translation) * scale;
    if (upper < 1.0 || lower < 1.0) return;
    const double reach = upper + lower;

    // Collarbone: only when reaching far, faded in and smoothed.
    double dist = Length(hand_pos - shoulder);
    if (k.clavicle[side] >= 0) {
        double want = std::clamp((dist / reach - 0.85) / 0.25, 0.0, 1.0);
        G.clav_amount[side] += (want - G.clav_amount[side]) * (1.0 - std::exp(-10.0 * dt));
        if (G.clav_amount[side] > 0.01) {
            FVector clav = WP(k.clavicle[side]);
            FQuat q = Arc(shoulder - clav, hand_pos - clav);
            RotW(k.clavicle[side], ClampAngle(Scale(q, 0.35 * G.clav_amount[side]), 25.0 * kDegToRad));
            shoulder = WP(k.upperarm[side]);
            dist = Length(hand_pos - shoulder);
        }
    }

    // Elbow: hangs down and a little out and back; follows the hand's pinky
    // side (turn the palm down and it swings out, like a real arm); then,
    // if the wrist would still bend past its range, the elbow swings further.
    FVector outward = side == kL ? body_right * -1.0 : body_right;
    double above = std::max(0.0, hand_pos.Z - shoulder.Z) / reach;
    FVector pole = FVector{0, 0, -1} * (1.0 - 0.6 * above) + outward * (0.7 + 0.6 * above) - body_fwd * 0.3;
    if (rig.ok) pole = pole + hand_rot.Rotate(rig.across_h * -1.0) * S.elbow_follow_hand;
    const double stretch = reach * (MaxStretch() - 1.0);
    const FVector axis = Normalize(hand_pos - shoulder);
    double best_cost = 1e18, best_ang = 0, flex = 0, dev = 0;
    TwoBoneResult sol{};
    // Holding the gun, the wrist stays as straight as the arm allows (the
    // owner's photos: forearm right behind the gun), so the elbow searches
    // wider and minimises the whole bend, not just what is past the limits.
    const bool straight = S.gun_wrist_straight && recorded && rig.ok && grab.kind == hands::GrabKind::kGunGrip;
    const int steps = straight ? 9 : 6;  // +-45 deg (+-90 flung the elbow up, 120 deg forearm twist)
    const double step_deg = straight ? 5.0 : 10.0;
    for (int step = -steps; step <= steps; ++step) {
        if (step != 0 && (!S.elbow_relieves_wrist || !rig.ok) && !straight) continue;
        double ang = step * step_deg * kDegToRad;
        TwoBoneResult s = SolveTwoBone(shoulder, hand_pos, upper, lower, AxisAngle(axis, ang).Rotate(pole), stretch);
        double fl = 0, dv = 0;
        if (rig.ok) WristBend(rig, hand_rot, s.end - s.mid, &fl, &dv);
        double over_f = std::max(0.0, std::fabs(fl) - 55.0 * kDegToRad);
        double over_d = std::max(0.0, std::fabs(dv) - 25.0 * kDegToRad);
        double cost = straight ? fl * fl + 2.0 * dv * dv + 0.4 * ang * ang
                               : over_f * over_f + 2.0 * over_d * over_d + 0.15 * ang * ang;
        if (cost < best_cost) {
            best_cost = cost;
            best_ang = ang;
            sol = s;
            flex = fl;
            dev = dv;
        }
    }
    SolveLimb(k.upperarm[side], k.lowerarm[side], k.hand[side], sol, &G.elbow_hinge_local[side],
              &G.hinge_known[side]);

    // Forearm roll (Blade & Sorcery / Final IK recipe): split the hand's
    // twist about the forearm. The forearm bone takes its share, the twist
    // bone near the wrist the rest, then the hand gets its exact rotation, so
    // the wrist joint itself never twists.
    double twist = 0;
    {
        Xf fore = WX(k.lowerarm[side]);
        FVector fa = Normalize(sol.end - fore.translation);
        // The hand's rotation relative to the forearm, against the reference.
        FQuat rel_now = Normalized(fore.rotation.Inverse() * hand_rot);
        FQuat rel_ref = G.ref[k.hand[side]].rotation;
        FVector axis_local = fore.rotation.Inverse().Rotate(fa);
        twist = TwistAngle(Normalized(rel_now * rel_ref.Inverse()), axis_local);
        // The halfway reference keeps the forearm from following the hand alone.
        RotW(k.lowerarm[side], AxisAngle(fa, twist * S.forearm_roll_share));
        if (k.forearm_twist[side] >= 0)
            RotW(k.forearm_twist[side], AxisAngle(fa, twist * (S.twist_bone_share - S.forearm_roll_share)));
    }
    Xf hand = WX(k.hand[side]);
    hand.rotation = hand_rot;
    hand.translation = sol.end;
    SetWX(k.hand[side], hand);

    // Fit numbers for the log.
    G.fit_reach[side] += dist / reach;
    double gap = Length(sol.end - hand_pos);
    G.fit_gap[side] += gap;
    G.max_gap[side] = std::max(G.max_gap[side], gap);
    G.fit_flex[side] += std::fabs(flex) * kRadToDeg;
    G.fit_dev[side] += std::fabs(dev) * kRadToDeg;
    G.fit_swivel[side] += best_ang * kRadToDeg;
    G.fit_twist[side] += std::fabs(twist) * kRadToDeg;
    ++G.fit_n[side];

    // Fingers.
    if (!S.hand_poses || !rig.ok) return;
    fingers::Angles want[hand_pose::kFingerCount];
    // Index: off the trigger -> resting on it (touch) -> pulled (analog).
    const double follow = 1.0 - std::exp(-dt / 0.06);
    G.index_touch[side] += ((fin.trigger_touch || fin.trigger > 0.05f ? 1.0 : 0.0) - G.index_touch[side]) * follow;
    G.index_pull[side] = std::clamp<double>(fin.trigger, 0.0, 1.0);
    if (recorded) {
        want[hand_pose::kThumb] = g.tip_angles[gr::kThumb];
        want[hand_pose::kMiddle] = g.tip_angles[gr::kMiddle];
        want[hand_pose::kRing] = g.tip_angles[gr::kRing];
        want[hand_pose::kPinky] = g.tip_angles[gr::kPinky];
        fingers::Angles idx = fingers::Blend(g.tip_angles[gr::kIndexOff], g.tip_angles[gr::kIndexRest], G.index_touch[side]);
        want[hand_pose::kIndex] = fingers::Blend(idx, g.tip_angles[gr::kIndexPull], G.index_pull[side]);
    } else {
        hand_pose::Curl c = hand_pose::Target(grab.kind, fin);
        for (int f = 0; f < hand_pose::kFingerCount; ++f)
            want[f] = fingers::Curl(rig.chain[f], std::min(1.2, c.finger[f] * S.finger_scale),
                                    fingers::DefaultLimits(f == hand_pose::kThumb));
    }
    // Ease into a new pose (grab blend ~0.1 s); the index stays immediate.
    const double ease = 1.0 - std::exp(-dt / 0.04);
    for (int f = 0; f < hand_pose::kFingerCount; ++f)
        G.cur[side][f] = !G.cur_init[side] || f == hand_pose::kIndex ? want[f]
                                                                     : fingers::Blend(G.cur[side][f], want[f], ease);
    G.cur_init[side] = true;
    PoseFingers(side, G.cur[side]);

    // Fingertips and palm in the world, for the editor.
    Xf hw = WX(k.hand[side]);
    for (int t = 0; t < gr::kTipCount; ++t) {
        int f = TipFinger(t);
        Xf j[3];
        FVector tip;
        fingers::Fk(rig.chain[f], G.cur[side][f], j, &tip);
        g_tip_world[side][t] = skel::Apply(hw, tip);
    }
    g_palm_world[side] = skel::Apply(hw, rig.palm_centre);
    g_hand_world[side] = hw;
    g_tips_valid[side] = true;
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
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          " | %s: reach %.0f%% of arm, hand gap %.1f cm (worst %.1f), wrist bend %.0f/%.0f deg "
                          "(flex/side), forearm twist %.0f deg, elbow swing %+.0f deg",
                          s == kL ? "left" : "right", 100.0 * G.fit_reach[s] / n, G.fit_gap[s] / n, G.max_gap[s],
                          G.fit_flex[s] / n, G.fit_dev[s] / n, G.fit_twist[s] / n, G.fit_swivel[s] / n);
            arms += buf;
        }
        log::Info("body fit: arm scale %.3f (%s), body %.0f deg behind head yaw, body moved %+.0f cm to put the "
                  "shoulders under the eyes%s", ArmScale(),
                  S.arm_scale_override > 0 ? "ini" : M.have ? "measured" : "not measured",
                  G.fit_yaw_n ? G.fit_yaw_lag / G.fit_yaw_n : 0.0, G.body_dz, arms.c_str());
    }
    G.fit_time = G.fit_yaw_lag = 0;
    G.fit_yaw_n = 0;
    for (int s = 0; s < 2; ++s) {
        G.fit_reach[s] = G.fit_gap[s] = G.fit_flex[s] = G.fit_dev[s] = G.fit_swivel[s] = G.fit_twist[s] = 0;
        G.max_gap[s] = 0;
        G.fit_n[s] = 0;
    }
}

void OnTick(UObject player, float dt_f) {
    static bool registered = [] {
        perf::Register(&g_perf_tick);
        perf::Register(&g_perf_engine);
        perf::Register(&g_perf_solve);
        perf::Register(&g_perf_grip);
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
    if (!M.loaded) LoadMeasure();
    if (!Setup(player)) return;
    Activate();
    if (!G.active) return;
    const Skeleton& k = G.skel;

    {
        perf::Scope t(g_perf_engine);
        auto copy_pose = [] {
            ue::Call copy(G.pose_comp, "CopyPoseFromSkeletalComponent");
            copy.Set("InComponentToCopy", G.source);
            return copy.Invoke();
        };
        if (!copy_pose()) return;
        G.engine.TryLocate();
        if (G.ref_pending) {
            CaptureReference();
            if (G.failed) {
                Deactivate();
                return;
            }
            copy_pose();
        }
        if (!G.engine.Read(&G.frame_local)) return;
        g_w = ComponentToWorld(G.pose_comp);
    }
    const double t_solve = perf::NowMs();
    G.pose.SetAllLocal(G.frame_local.data(), static_cast<int>(G.frame_local.size()));
    G.pose.ClearChanged();
    ++G.frames;

    hands::HandState hs[2] = {hands::Get(hands::kLeft), hands::Get(hands::kRight)};
    hands::HandGrab grab[2] = {hands::GetGrab(hands::kLeft), hands::GetGrab(hands::kRight)};

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
            reset(k.upperarm_twist[side]);
            reset(k.forearm_twist[side]);
        }
    }

    // Animated pose, before we change anything.
    ue::Call ar(player, "K2_GetActorRotation");
    ar.Invoke();
    const double actor_yaw = ar.Result<FRotator>().Yaw;
    const Xf pelvis = k.pelvis >= 0 ? WX(k.pelvis) : Xf{};
    Xf foot_anim[2];
    for (int s = 0; s < 2; ++s)
        if (k.legs_ok[s]) foot_anim[s] = WX(k.foot[s]);
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
    ue::Call vel(player, "GetVelocity");
    vel.Invoke();
    FVector v = vel.Result<FVector>();
    if (v.X * v.X + v.Y * v.Y > 30.0 * 30.0) G.body_yaw += diff * (1.0 - std::exp(-5.0 * dt));
    G.body_yaw = NormalizeAxis(G.body_yaw);
    const FQuat yaw_q = YawOnly(G.body_yaw);
    const FVector body_fwd = YawDir(actor_yaw + G.body_yaw);
    const FVector body_right = YawDir(actor_yaw + G.body_yaw + 90.0);

    // Shoulders from the head (Blade & Sorcery's recipe): one "head unit"
    // (eye height / 7.5) below the eyes and half a unit behind them, along
    // the head's level facing. The body is moved and bent so the avatar's
    // shoulder centre sits there.
    double eye_h = head.location.Z - WP(0).Z;
    if (!(eye_h > 100.0 && eye_h < 230.0)) eye_h = 165.0;
    const double unit = eye_h / 7.5;
    const double below = S.shoulder_below > 0 ? S.shoulder_below : unit;
    const double behind = S.shoulder_behind > 0 ? S.shoulder_behind : unit * 0.5;
    const FVector head_fwd = YawDir(head_yaw);
    const FVector sc_target = head.location - head_fwd * behind - FVector{0, 0, below};
    const FVector sc_anim_r = pelvis.translation + yaw_q.Rotate(sc_anim - pelvis.translation);
    FVector delta = sc_target - sc_anim_r;
    if (Length(delta) > 500.0) delta = FVector{};  // teleport in progress
    if (delta.Z > G.stand_dz) G.stand_dz = delta.Z;
    // Height: the avatar is taller than the game's camera (its shoulders
    // came out level with the eyes, camera "in the collarbone"), so the whole
    // body moves down (knees bend) until the shoulders are one head unit
    // below the eyes. With fixed_height this is a constant; otherwise real
    // crouching adds to it.
    {
        double lo = S.fixed_height ? -45.0 : -(S.max_crouch + 45.0);
        double want_dz = std::clamp(delta.Z, lo, 10.0);
        G.body_dz += (want_dz - G.body_dz) * (1.0 - std::exp(-8.0 * dt));
    }
    const double crouch = G.body_dz;
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
        FVector want = sc_anim_r + pelvis_shift + FVector{0, 0, crouch} + lean;
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

    UpdateEditor(grab, hs, dt);
    StepMeasure(hs, sc_target, body_right, dt);
    for (int side = 0; side < 2; ++side)
        if (hs[side].valid) SolveArm(side, hs[side], grab[side], grab[kR], body_fwd, body_right, dt);

    // Legs keep the animated feet where they were (turned with the body).
    if (S.legs && k.pelvis >= 0)
        for (int s = 0; s < 2; ++s) {
            if (!k.legs_ok[s]) continue;
            FVector hip = WP(k.thigh[s]), knee = WP(k.calf[s]), ankle = WP(k.foot[s]);
            double upper = Length(knee - hip), lower = Length(ankle - knee);
            if (upper < 1.0 || lower < 1.0) continue;
            FVector outward = s == kL ? body_right * -1.0 : body_right;
            FVector foot_target =
                pelvis.translation + pelvis_shift + yaw_q.Rotate(foot_anim[s].translation - pelvis.translation);
            TwoBoneResult sol = SolveTwoBone(hip, foot_target, upper, lower, body_fwd + outward * 0.15, 0.0);
            SolveLimb(k.thigh[s], k.calf[s], k.foot[s], sol, &G.knee_hinge_local[s], &G.knee_known[s]);
            Xf foot = WX(k.foot[s]);
            foot.rotation = Normalized(yaw_q * foot_anim[s].rotation);
            SetWX(k.foot[s], foot);
        }

    // Gear slots on the posed body.
    if (k.pelvis >= 0) {
        FQuat body_q = YawOnly(actor_yaw + G.body_yaw);
        FVector belt = WP(k.pelvis);
        FVector chest = k.spine.empty() ? belt : WP(k.spine.back());
        for (int i = 0; i < static_cast<int>(Slot::kCount); ++i) {
            g_slots[i].location = (kSlots[i].chest ? chest : belt) + body_q.Rotate(kSlots[i].offset);
            g_slots[i].rotation = body_q;
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

void Init() {
    LoadSettings();
    game::AddTickListener(&OnTick);
    log::Info("body: ready (%s; F9 toggles; grip editor: hold left Y 1 s while holding the gun)",
              S.enabled ? "on" : "off");
}

}  // namespace holevr::vr::body
