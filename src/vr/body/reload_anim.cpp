#include "vr/body/reload_anim.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/memory.h"
#include "ue/call.h"
#include "ue/names.h"
#include "ue/objects.h"
#include "ue/reflection.h"
#include "vr/body/skeleton.h"

namespace holevr::vr::reload_anim {
namespace {

using ue::UObject;
using skel::Xf;
using namespace ik;

constexpr int32_t kRtsComponent = 2;         // ERelativeTransformSpace::RTS_Component
constexpr uint8_t kAnimationSingleNode = 1;  // EAnimationMode::AnimationSingleNode
constexpr int kWaitFrames = 3;               // frames from setting a time to reading its pose
constexpr double kFindRetryS = 30.0;         // between scans for an animation not loaded yet
constexpr double kGiveUpS = 600.0;           // stop looking after this long

struct Source {
    const char* label;
    const char* seq;
    const char* montage;
};
const Source kSource[kClipCount] = {
    {"pistol", "AnimSequence /Game/Animation/ALS_HG_Isosceles_Reload_17R.ALS_HG_Isosceles_Reload_17R",
     "AnimMontage /Game/Animation/Montage/ALS_HG_Isosceles_Reload_17R_Montage.ALS_HG_Isosceles_Reload_17R_Montage"},
    {"rifle", "AnimSequence /Game/Animation/ALS_AR_CClamp_Reload_30R.ALS_AR_CClamp_Reload_30R",
     "AnimMontage /Game/Animation/Montage/ALS_AR_CClamp_Reload_Montage.ALS_AR_CClamp_Reload_Montage"},
};

// Each lookup is one pass over every object, so at most one every 30 s.
ue::ObjectRef g_seq[kClipCount] = {ue::ObjectRef(kSource[0].seq, kFindRetryS),
                                   ue::ObjectRef(kSource[1].seq, kFindRetryS)};
ue::ObjectRef g_montage[kClipCount] = {ue::ObjectRef(kSource[0].montage, kFindRetryS),
                                       ue::ObjectRef(kSource[1].montage, kFindRetryS)};
ClipData g_clip[kClipCount];
uint64_t g_generation = 0;

enum class Step { kFind, kSample, kDone };
struct Work {
    Step step = Step::kFind;
    UObject seq = 0;
    double first_look = -1;  // s, steady clock
    int sample = -1;  // -1: the copy's pose before the animation is set (to prove it plays)
    int wait = 0;
    Pose before;
} g_work[kClipCount];

UObject g_player = 0;
UObject g_comp = 0;  // the hidden copy of the character mesh
int g_comp_clip = -1;
ue::FName g_socket{};
int g_socket_state = 0;  // 0 not looked up, 1 found, -1 not in the name pool

double NowS() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

ue::FName ToFName(uint64_t v) {
    ue::FName n;
    std::memcpy(static_cast<void*>(&n), &v, sizeof(n));
    return n;
}

std::string Squash(const std::string& s) {
    std::string out;
    for (char ch : s)
        if (std::isalnum(static_cast<unsigned char>(ch))) out += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

double TurnDeg(const FQuat& a, const FQuat& b) {
    const double d = std::fabs(a.X * b.X + a.Y * b.Y + a.Z * b.Z + a.W * b.W);
    return 2.0 * std::acos(std::min(1.0, d)) * 57.29577951308232;
}

int32_t Offset(UObject s, const char* name) {
    auto p = ue::reflection::FindProperty(s, name);
    return p.valid() ? p.offset : -1;
}

int32_t Stride(UObject s) {
    const int32_t n = ue::reflection::GetPropertiesSize(s);
    return n > 0 ? (n + 7) & ~7 : 0;
}

struct TArrayRaw {
    uintptr_t data;
    int32_t num, max;
};

bool ReadArray(UObject obj, int32_t off, int32_t stride, TArrayRaw* a) {
    if (off < 0 || stride <= 0 || !mem::Read(obj + off, *a)) return false;
    if (a->num < 0 || a->num > 4096 || a->num > a->max) return false;
    return a->num == 0 || (mem::LooksLikePointer(a->data) && mem::IsReadable(a->data, size_t(a->num) * stride));
}

// The notifies of an animation asset: their names and times (s into the
// asset). AnimSequenceBase::Notifies, laid out from reflection.
struct Notify {
    std::string name;
    double time;
};
bool ReadNotifies(UObject anim, std::vector<Notify>* out, std::string* why) {
    static UObject ev = 0;
    if (!ev) ev = ue::FindObject("ScriptStruct /Script/Engine.AnimNotifyEvent");
    if (!ev) {
        *why = "the engine's AnimNotifyEvent layout was not found";
        return false;
    }
    const int32_t stride = Stride(ev), o_name = Offset(ev, "NotifyName"), o_link = Offset(ev, "LinkValue"),
                  o_trigger = Offset(ev, "TriggerTimeOffset"), o_method = Offset(ev, "LinkMethod"),
                  o_seg_begin = Offset(ev, "SegmentBeginTime"), o_seg_len = Offset(ev, "SegmentLength");
    if (stride < 0x30 || stride > 0x400 || o_name < 0 || o_link < 0) {
        *why = "AnimNotifyEvent has no NotifyName/LinkValue";
        return false;
    }
    TArrayRaw arr{};
    if (!ReadArray(anim, Offset(ue::GetClass(anim), "Notifies"), stride, &arr)) {
        *why = "its Notifies list could not be read";
        return false;
    }
    for (int32_t i = 0; i < arr.num; ++i) {
        const uintptr_t e = arr.data + uintptr_t(i) * stride;
        Notify n;
        n.name = ue::names::ToString(mem::ReadOr<ue::FName>(e + o_name));
        double t = mem::ReadOr<float>(e + o_link, 0.f);
        // EAnimLinkMethod: 0 absolute, 1 relative to its montage segment, 2 a fraction of it.
        const uint8_t method = o_method >= 0 ? mem::ReadOr<uint8_t>(e + o_method, 0) : 0;
        const double seg_begin = o_seg_begin >= 0 ? mem::ReadOr<float>(e + o_seg_begin, 0.f) : 0.0;
        const double seg_len = o_seg_len >= 0 ? mem::ReadOr<float>(e + o_seg_len, 0.f) : 0.0;
        if (method == 1) t += seg_begin;
        if (method == 2) t = seg_begin + t * seg_len;
        if (o_trigger >= 0) t += mem::ReadOr<float>(e + o_trigger, 0.f);
        n.time = t;
        if (std::isfinite(t)) out->push_back(n);
    }
    return true;
}

// A montage time as a time into `seq`, through the montage's first slot's
// segments. false if no segment of `seq` covers it.
bool MontageToSequence(UObject montage, UObject seq, double t, double* out) {
    static UObject slot_s = 0, track_s = 0, seg_s = 0;
    if (!slot_s) slot_s = ue::FindObject("ScriptStruct /Script/Engine.SlotAnimationTrack");
    if (!track_s) track_s = ue::FindObject("ScriptStruct /Script/Engine.AnimTrack");
    if (!seg_s) seg_s = ue::FindObject("ScriptStruct /Script/Engine.AnimSegment");
    if (!slot_s || !track_s || !seg_s) return false;
    TArrayRaw slots{};
    if (!ReadArray(montage, Offset(ue::GetClass(montage), "SlotAnimTracks"), Stride(slot_s), &slots) || slots.num < 1)
        return false;
    const int32_t o_track = Offset(slot_s, "AnimTrack");
    if (o_track < 0) return false;
    TArrayRaw segs{};
    const int32_t seg_stride = Stride(seg_s);
    if (!ReadArray(slots.data + o_track, Offset(track_s, "AnimSegments"), seg_stride, &segs)) return false;
    const int32_t o_ref = Offset(seg_s, "AnimReference"), o_pos = Offset(seg_s, "StartPos"),
                  o_start = Offset(seg_s, "AnimStartTime"), o_end = Offset(seg_s, "AnimEndTime"),
                  o_rate = Offset(seg_s, "AnimPlayRate");
    if (o_ref < 0 || o_pos < 0 || o_start < 0 || o_end < 0) return false;
    for (int32_t i = 0; i < segs.num; ++i) {
        const uintptr_t s = segs.data + uintptr_t(i) * seg_stride;
        if (mem::ReadOr<uintptr_t>(s + o_ref, 0) != seq) continue;
        const double pos = mem::ReadOr<float>(s + o_pos, 0.f), a0 = mem::ReadOr<float>(s + o_start, 0.f),
                     a1 = mem::ReadOr<float>(s + o_end, 0.f);
        double rate = o_rate >= 0 ? mem::ReadOr<float>(s + o_rate, 1.f) : 1.0;
        if (!(std::fabs(rate) > 1e-3)) rate = 1.0;
        const double len = (a1 - a0) / std::fabs(rate);
        if (t < pos - 1e-3 || t > pos + len + 1e-3) continue;
        *out = rate > 0 ? a0 + (t - pos) * rate : a1 + (t - pos) * rate;
        return true;
    }
    return false;
}

double PlayLength(UObject anim) {
    ue::Call c(anim, "GetPlayLength");
    if (c.Valid() && c.Invoke()) {
        const float v = c.Result<float>(0.f);
        if (v > 0) return v;
    }
    const int32_t o = Offset(ue::GetClass(anim), "SequenceLength");
    return o >= 0 ? mem::ReadOr<float>(anim + o, 0.f) : 0.0;
}

bool IsInsert(const std::string& name, const char* which) {
    const std::string s = Squash(name);
    return s.find("magazineinsert") != std::string::npos && s.find(which) != std::string::npos;
}

// "Magazine Insert Begin" / "End" times in the sequence: its own notifies,
// else the montage's (mapped onto the sequence).
bool FindInsertTimes(int c, UObject seq, ClipData* d) {
    std::vector<Notify> list;
    std::string why;
    std::string names;
    double begin = -1, end = -1;
    if (ReadNotifies(seq, &list, &why)) {
        for (const Notify& n : list) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s%s %.2f", names.empty() ? "" : ", ", n.name.c_str(), n.time);
            names += buf;
            if (begin < 0 && IsInsert(n.name, "begin")) begin = n.time;
            if (end < 0 && IsInsert(n.name, "end")) end = n.time;
        }
    }
    const char* from = "the animation";
    if (begin < 0 || end < 0) {
        UObject montage = g_montage[c].Get();
        std::vector<Notify> ml;
        std::string mwhy;
        if (montage && ReadNotifies(montage, &ml, &mwhy)) {
            names += "; montage:";
            double mb = -1, me = -1;
            for (const Notify& n : ml) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), " %s %.2f", n.name.c_str(), n.time);
                names += buf;
                if (mb < 0 && IsInsert(n.name, "begin")) mb = n.time;
                if (me < 0 && IsInsert(n.name, "end")) me = n.time;
            }
            if (mb >= 0 && me >= 0) {
                double sb = mb, se = me;
                const bool mapped = MontageToSequence(montage, seq, mb, &sb) && MontageToSequence(montage, seq, me, &se);
                if (!mapped) log::Info("reload anim: %s montage segments not read; its times are used as they are",
                                       kSource[c].label);
                begin = sb;
                end = se;
                from = "its montage";
            }
        } else if (!montage) {
            names += "; montage not loaded";
        }
    }
    log::Info("reload anim: %s %s notifies: %s", kSource[c].label, d->name.c_str(), names.empty() ? "none" : names.c_str());
    if (begin < 0 || end < 0 || end <= begin + 0.02) {
        d->why = why.empty() ? "no \"Magazine Insert Begin\"/\"End\" marks" : why;
        return false;
    }
    d->t_begin = begin;
    d->t_end = end;
    log::Info("reload anim: %s insert from %.2f to %.2f s (marks from %s, animation %.2f s long)", kSource[c].label,
              begin, end, from, d->length);
    return true;
}

Xf SocketXf(UObject comp, ue::FName name) {
    ue::Call c(comp, "GetSocketTransform");
    c.Set("InSocketName", name).Set("TransformSpace", static_cast<uint8_t>(kRtsComponent));
    if (!c.Invoke()) return Xf{};
    return c.Result<Xf>();
}

bool HasBone(UObject comp, ue::FName name) {
    ue::Call c(comp, "GetBoneIndex");
    c.Set("BoneName", name);
    return c.Invoke() && c.Result<int32_t>(-1) >= 0;
}

void ReadPose(UObject comp, const Bones& b, Pose* out) {
    const Xf hand = SocketXf(comp, ToFName(b.hand));
    for (int f = 0; f < kFingers; ++f) {
        FQuat parent = hand.rotation;
        for (int j = 0; j < 3; ++j) {
            const Xf x = SocketXf(comp, ToFName(b.finger[f][j]));
            out->joint[f][j] = Normalized(parent.Inverse() * x.rotation);
            parent = x.rotation;
        }
    }
}

void DestroyCopy() {
    if (g_comp && ue::IsLive(g_comp)) {
        ue::Call d(g_comp, "K2_DestroyComponent");
        d.Set("Object", g_comp);
        d.Invoke();
    }
    g_comp = 0;
    g_comp_clip = -1;
}

bool CallBool1(UObject obj, const char* fn, const char* param, bool v) {
    ue::Call c(obj, fn);
    c.Set(param, v);
    return c.Invoke();
}

// The hidden copy: the character mesh's own asset (the animations are made
// for its skeleton), never drawn, no collision, posed by the animation alone.
bool MakeCopy(UObject player, UObject char_mesh) {
    static UObject skm_class = 0;
    if (!skm_class) skm_class = ue::FindObject("Class /Script/Engine.SkeletalMeshComponent");
    ue::Call asset(char_mesh, "GetSkinnedAsset");
    UObject mesh = asset.Invoke() ? asset.Result<UObject>() : 0;
    if (!skm_class || !mesh) return false;
    ue::Call add(player, "AddComponentByClass");
    add.Set("Class", skm_class).Set("bManualAttachment", true).Set("RelativeTransform", Xf{})
        .Set("bDeferredFinish", false);
    if (!add.Invoke() || !add.Result<UObject>()) return false;
    g_comp = add.Result<UObject>();
    CallBool1(g_comp, "SetHiddenInGame", "NewHidden", true);
    {
        ue::Call v(g_comp, "SetVisibility");
        v.Set("bNewVisibility", false).Set("bPropagateToChildren", false);
        v.Invoke();
    }
    {
        ue::Call col(g_comp, "SetCollisionEnabled");
        col.Set("NewType", static_cast<uint8_t>(0));  // ECollisionEnabled::NoCollision
        col.Invoke();
    }
    CallBool1(g_comp, "SetCastShadow", "NewCastShadow", false);
    {
        ue::Call set_mesh(g_comp, "SetSkinnedAssetAndUpdate");
        set_mesh.Set("NewMesh", mesh).Set("bReinitPose", true);
        set_mesh.Invoke();
    }
    // Never drawn, so the engine would skip its pose; and at a low detail
    // level it may drop the finger bones.
    ue::SetProp<uint8_t>(g_comp, "VisibilityBasedAnimTickOption", 0);  // AlwaysTickPoseAndRefreshBones
    {
        ue::Call lod(g_comp, "SetForcedLOD");
        lod.Set("InNewForcedLOD", static_cast<int32_t>(1));
        lod.Invoke();
    }
    log::Info("reload anim: hidden copy of %s made to read the game's reload animation from",
              ue::GetName(mesh).c_str());
    return true;
}

void Fail(int c, const std::string& why) {
    ClipData& d = g_clip[c];
    d.failed = true;
    d.why = why;
    g_work[c].step = Step::kDone;
    if (g_comp_clip == c) DestroyCopy();
    log::Info("reload anim: %s reload not used (%s); the VR hand keeps its plain hold while a magazine goes in",
              kSource[c].label, why.c_str());
}

void Finish(int c) {
    ClipData& d = g_clip[c];
    std::string turns;
    for (int f = 0; f < kFingers; ++f) {
        double m = 0;
        for (int i = 1; i < kSamples; ++i)
            for (int j = 0; j < 3; ++j) m = std::max(m, TurnDeg(d.pose[0].joint[f][j], d.pose[i].joint[f][j]));
        d.max_turn_deg[f] = m;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s%.0f", f ? "/" : "", m);
        turns += buf;
    }
    double from_ref = 0;
    for (int f = 0; f < kFingers; ++f)
        for (int j = 0; j < 3; ++j) from_ref = std::max(from_ref, TurnDeg(g_work[c].before.joint[f][j], d.pose[0].joint[f][j]));
    d.ready = true;
    g_work[c].step = Step::kDone;
    ++g_generation;
    log::Info("reload anim: %s reload read (%d poses from %.2f to %.2f s): left fingers turn %s deg in it (thumb..pinky), "
              "%.0f deg from the mesh's rest pose at the start; hand socket %s",
              kSource[c].label, kSamples, d.t_begin, d.t_end, turns.c_str(), from_ref,
              d.have_socket ? "read" : "NOT found (the hand keeps its own hold on the magazine)");
    if (d.have_socket)
        log::Info("reload anim: %s HandItemPoint_L at (%.1f %.1f %.1f) cm from the left hand bone, rotation (%.3f %.3f "
                  "%.3f %.3f)",
                  kSource[c].label, d.socket_pos.X, d.socket_pos.Y, d.socket_pos.Z, d.socket_rot.X, d.socket_rot.Y,
                  d.socket_rot.Z, d.socket_rot.W);
}

double SampleTime(const ClipData& d, int i) {
    return d.t_begin + (d.t_end - d.t_begin) * i / double(kSamples - 1);
}

void SetTime(double t) {
    ue::Call p(g_comp, "SetPosition");
    p.Set("InPos", static_cast<float>(t)).Set("bFireNotifies", false);
    p.Invoke();
}

}  // namespace

const ClipData& Get(Clip c) { return g_clip[c]; }
uint64_t Generation() { return g_generation; }

void Tick(uintptr_t player, uintptr_t char_mesh, const Bones& bones, unsigned clips) {
    if (!player || !char_mesh || !bones.hand) return;
    if (player != g_player) {
        // A new level: the copy went with the old player. Sampling restarts.
        g_player = player;
        g_comp = 0;
        g_comp_clip = -1;
        for (int c = 0; c < kClipCount; ++c)
            if (g_work[c].step == Step::kSample) g_work[c].step = Step::kFind, g_work[c].sample = -1;
    }
    for (int c = 0; c < kClipCount; ++c) {
        if (!(clips & (1u << c))) continue;
        Work& w = g_work[c];
        ClipData& d = g_clip[c];
        if (w.step == Step::kDone) continue;
        if (w.step == Step::kFind) {
            if (g_comp_clip >= 0 && g_comp_clip != c) return;  // one clip at a time
            UObject seq = g_seq[c].Get();
            if (!seq) {
                const double now = NowS();
                if (w.first_look < 0) {
                    w.first_look = now;
                    log::Info("reload anim: %s reload animation not loaded yet; looking for it every %.0f s",
                              kSource[c].label, kFindRetryS);
                }
                if (now - w.first_look > kGiveUpS) Fail(c, "the animation never loaded");
                continue;
            }
            w.seq = seq;
            d = ClipData{};
            d.name = ue::GetName(seq);
            d.length = PlayLength(seq);
            if (!FindInsertTimes(c, seq, &d)) {
                Fail(c, d.why);
                continue;
            }
            for (int f = 0; f < kFingers; ++f)
                for (int j = 0; j < 3; ++j)
                    if (!bones.finger[f][j]) {
                        Fail(c, "the VR body's left hand has no finger bones");
                        return;
                    }
            if (!g_comp || !ue::IsLive(g_comp)) {
                g_comp = 0;
                if (!MakeCopy(player, char_mesh)) {
                    Fail(c, "the hidden copy of the character mesh could not be made");
                    continue;
                }
            }
            bool bones_ok = HasBone(g_comp, ToFName(bones.hand));
            for (int f = 0; f < kFingers; ++f)
                for (int j = 0; j < 3; ++j) bones_ok = bones_ok && HasBone(g_comp, ToFName(bones.finger[f][j]));
            if (!bones_ok) {
                Fail(c, "the character mesh does not have the VR hand's bone names");
                continue;
            }
            g_comp_clip = c;
            w.step = Step::kSample;
            w.sample = -1;
            w.wait = kWaitFrames;
            return;
        }
        // Step::kSample
        if (!g_comp || !ue::IsLive(g_comp)) {
            g_comp = 0;
            g_comp_clip = -1;
            w.step = Step::kFind;
            return;
        }
        if (--w.wait > 0) return;
        if (w.sample < 0) {
            // The copy's own pose, then the animation (paused).
            ReadPose(g_comp, bones, &w.before);
            ue::Call mode(g_comp, "SetAnimationMode");
            mode.Set("InAnimationMode", kAnimationSingleNode).Set("bForceInitAnimScriptInstance", true);
            mode.Invoke();
            ue::Call set(g_comp, "SetAnimation");
            set.Set("NewAnimToPlay", w.seq);
            set.Invoke();
            ue::Call stop(g_comp, "Stop");
            stop.Invoke();
            w.sample = 0;
            SetTime(SampleTime(d, 0));
            w.wait = kWaitFrames;
            return;
        }
        // Pose of sample w.sample is in.
        {
            ue::Call gp(g_comp, "GetPosition");
            const double at = gp.Invoke() ? gp.Result<float>(-1.f) : -1.0;
            const double want = SampleTime(d, w.sample);
            if (std::fabs(at - want) > 0.02) {
                Fail(c, "the hidden copy did not take the animation (at " + std::to_string(at) + " s, not " +
                            std::to_string(want) + " s)");
                continue;
            }
        }
        ReadPose(g_comp, bones, &d.pose[w.sample]);
        if (w.sample == 0) {
            if (g_socket_state == 0) g_socket_state = ue::names::Find("HandItemPoint_L", &g_socket) ? 1 : -1;
            ue::Call has(g_comp, "DoesSocketExist");
            has.Set("InSocketName", g_socket);
            if (g_socket_state == 1 && has.Invoke() && has.Result<bool>()) {
                const Xf hand = SocketXf(g_comp, ToFName(bones.hand));
                const Xf sock = SocketXf(g_comp, g_socket);
                const Xf rel = skel::Relative(sock, hand);
                d.socket_rot = Normalized(rel.rotation);
                d.socket_pos = rel.translation;
                d.have_socket = true;
                ue::Call bone(g_comp, "GetSocketBoneName");
                bone.Set("InSocketName", g_socket);
                if (bone.Invoke())
                    log::Info("reload anim: HandItemPoint_L is on bone %s",
                              ue::names::ToString(bone.Result<ue::FName>()).c_str());
            }
        }
        if (++w.sample < kSamples) {
            SetTime(SampleTime(d, w.sample));
            w.wait = kWaitFrames;
            return;
        }
        // All read. Proof that it played: the fingers moved from the copy's
        // rest pose or within the stretch.
        double moved = 0;
        for (int i = 0; i < kSamples; ++i)
            for (int f = 0; f < kFingers; ++f)
                for (int j = 0; j < 3; ++j) moved = std::max(moved, TurnDeg(w.before.joint[f][j], d.pose[i].joint[f][j]));
        if (moved < 0.5) {
            Fail(c, "the hidden copy's fingers never moved (the animation did not play on it)");
            continue;
        }
        Finish(c);
        DestroyCopy();
        return;
    }
}

}  // namespace holevr::vr::reload_anim
