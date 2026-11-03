#include "vr/interact/touch.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "core/perf.h"
#include "game/game.h"
#include "ue/call.h"
#include "ue/objects.h"
#include "ue/reflection.h"
#include "ue/script_dump.h"
#include "vr/body/body.h"
#include "vr/hands.h"
#include "vr/input.h"
#include "vr/interact/touch_math.h"
#include "vr/math.h"
#include "vr/ui_panel.h"
#include "vr/workbench.h"

namespace holevr::vr::touch {
namespace {

using ue::UObject;

// FTransform (double precision) as returned by K2_GetComponentToWorld.
struct alignas(16) Xf {
    FQuat rotation;
    FVector translation;
    double pad0 = 0;
    FVector scale{1, 1, 1};
    double pad1 = 0;
};
static_assert(sizeof(Xf) == 0x60, "FTransform layout");

struct Arr {
    uintptr_t data;
    int32_t num, max;
};

double Len(const FVector& v) { return std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z); }

Xf CompToWorld(UObject comp) {
    ue::Call c(comp, "K2_GetComponentToWorld");
    if (!c.Invoke()) return Xf{};
    return c.Result<Xf>();
}

FVector ActorLocation(UObject actor) {
    ue::Call c(actor, "K2_GetActorLocation");
    c.Invoke();
    return c.Result<FVector>();
}

// Still the object the engine keeps at its own slot (not destroyed or
// collected): items are destroyed when taken, things go with their level.
bool Live(UObject obj) {
    if (!obj) return false;
    const int32_t idx = mem::ReadOr<int32_t>(obj + ue::off::kObjIndex, -1);
    return idx >= 0 && ue::objects::Get(idx) == obj && ue::GetClass(obj) != 0;
}

// A float or double property, whichever the game made it.
bool ReadNumber(UObject obj, const char* name, double* out) {
    const ue::reflection::PropertyInfo p = ue::reflection::FindProperty(ue::GetClass(obj), name);
    if (!p.valid()) return false;
    if (p.element_size == 4) {
        float f = 0;
        if (!mem::Read(obj + p.offset, f)) return false;
        *out = f;
        return true;
    }
    if (p.element_size == 8) return mem::Read(obj + p.offset, *out);
    return false;
}

// A bool property: 1/0, or -1 when the thing has none.
int ReadFlag(UObject obj, const char* name) {
    const ue::reflection::PropertyInfo p = ue::reflection::FindProperty(ue::GetClass(obj), name);
    if (!p.valid() || p.type != "BoolProperty") return -1;
    uint8_t b = 0;
    if (!mem::Read(obj + p.offset, b)) return -1;
    return (b & (p.bool_mask ? p.bool_mask : 0xFF)) ? 1 : 0;
}

UObject Statics() {
    static UObject gps = 0;
    if (!gps) gps = ue::FindObject("GameplayStatics /Script/Engine.Default__GameplayStatics");
    return gps;
}
UObject Ksl() {
    static UObject ksl = 0;
    if (!ksl) ksl = ue::FindObject("KismetSystemLibrary /Script/Engine.Default__KismetSystemLibrary");
    return ksl;
}

// ------------------------------------------------------------------ settings
struct Settings {
    bool enabled = true, loot = true, containers = true, doors = true, study = true;
    double reach = 1.5;      // a touch when the hand is this close, cm
    double leave = 4.0;      // it has left once this much further away, cm
    double hold = 0.08;      // the hand rests this long first, s
    double door_hold = 0.2;  // longer for doors (walking through a doorway)
    double again = 1.0;      // the same thing again (touch or trigger) after, s
    std::vector<std::string> extra_containers, skip;
    HandleZoneSettings handle;
} S;

void LoadSettings() {
    S.enabled = config::GetBool("touch_interact", true);
    S.loot = config::GetBool("touch_interact_loot", true);
    S.containers = config::GetBool("touch_interact_containers", true);
    S.doors = config::GetBool("touch_interact_doors", true);
    S.study = config::GetBool("touch_interact_study", true);
    S.reach = std::clamp<double>(config::GetFloat("touch_interact_reach_cm", 1.5f), 0.0, 15.0);
    S.hold = std::clamp<double>(config::GetFloat("touch_interact_hold_s", 0.08f), 0.0, 2.0);
    S.door_hold = std::clamp<double>(config::GetFloat("touch_interact_door_hold_s", 0.2f), 0.0, 2.0);
    S.extra_containers = SplitList(config::GetString("touch_interact_extra_containers", ""));
    S.skip = SplitList(config::GetString("touch_interact_skip", ""));
    S.handle.height = std::clamp<double>(config::GetFloat("door_handle_height_cm", 100.0f), 40.0, 180.0);
    S.handle.from_edge = std::clamp<double>(config::GetFloat("door_handle_from_edge_cm", 22.0f), 5.0, 60.0);
}

// --------------------------------------------------------------- level state
struct Part {
    UObject comp = 0;   // the frame: this component's transform (0 = world)
    Box box;            // in the component's frame, scale applied (cm)
    const char* what = "";
};

struct Thing {
    UObject actor = 0;
    Kind kind = Kind::kOther;
    std::string name, cls;
    std::vector<std::string> chain;       // class names, own first
    std::vector<std::string> chain_paths;  // their /Game/ paths (bytecode dumps)
    FVector center_off;                   // bounds centre from the actor's location
    FVector center;
    double reach = 100;                   // bounds radius, cm
    bool built = false;
    std::vector<Part> parts;
    double volume = 0;
    TouchState st[2];
};

struct Outcome {
    UObject actor = 0;
    std::string name, cls, why;
    Kind kind = Kind::kOther;
    int open = -1, take = -1;
    double at = 0;
};

struct LevelState {
    UObject player = 0;
    double next_list = 0;
    std::vector<Thing> things;
    size_t rr = 0;
    Arr list_buf{};   // the engine's array from the last list, handed back so it is reused
    int counts[5] = {};
    bool counted = false;
    std::map<UObject, double> used;
    std::vector<Outcome> outcomes;
} L;

double g_clock = 0.0;
ue::ObjectRef g_iface("BlueprintGeneratedClass /Game/GamePlay/InteractSystem/BPI_InteractSystem.BPI_InteractSystem_C");
std::set<std::string> g_studied;

perf::Counter c_tick{"touch interact tick"}, c_list{"touch interact: list things"},
    c_build{"touch interact: measure a thing"};

// Bytecode listings of every interactable class met (dump/script), once a
// run, on a worker thread: what the game's boxes, fridges and doors do when
// interacted with, for the next round of this feature.
std::mutex g_dump_mu;
std::vector<std::string> g_dump_pending;
std::set<std::string> g_dump_queued;
std::atomic<bool> g_dump_running{false};
double g_next_dump = 0;

void QueueDump(const std::vector<std::string>& paths) {
    std::lock_guard<std::mutex> lock(g_dump_mu);
    for (const std::string& p : paths)
        if (p.rfind("/Game/", 0) == 0 && g_dump_queued.insert(p).second) g_dump_pending.push_back(p);
}

void MaybeRunDump() {
    if (g_clock < g_next_dump || g_dump_running.load()) return;
    g_next_dump = g_clock + 5.0;
    static std::vector<std::string> s_batch;
    {
        std::lock_guard<std::mutex> lock(g_dump_mu);
        if (g_dump_pending.empty()) return;
        s_batch.swap(g_dump_pending);
        g_dump_pending.clear();
    }
    g_dump_running = true;
    if (HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            const int n = ue::script::DumpClasses(config::Dir() + L"\\dump", s_batch);
            std::string names;
            for (const std::string& p : s_batch) {
                const size_t dot = p.rfind('.');
                names += (names.empty() ? "" : ", ") + (dot == std::string::npos ? p : p.substr(dot + 1));
            }
            log::Info("touch: study: the game's code for %s written to dump/script (%d of %zu classes)",
                      names.c_str(), n, s_batch.size());
            g_dump_running = false;
            return 0;
        }, nullptr, 0, nullptr))
        CloseHandle(t);
    else
        g_dump_running = false;
}

void ClassChain(UObject actor, Thing& t) {
    UObject cls = ue::GetClass(actor);
    t.cls = cls ? ue::GetName(cls) : "?";
    for (int i = 0; cls && i < 16; ++i) {
        const std::string n = ue::GetName(cls);
        if (n == "Actor" || n == "Object") break;
        t.chain.push_back(n);
        t.chain_paths.push_back(ue::GetPathName(cls));
        cls = ue::reflection::GetSuper(cls);
    }
}

Kind ClassifyThing(const Thing& t) {
    std::vector<std::string> chain = t.chain;
    Kind k = Classify(chain, S.extra_containers, S.skip);
    // The range timer is pressed by vr/controls' physical button.
    for (const std::string& c : chain)
        if (c.rfind("BP_ShootingTimer", 0) == 0) k = Kind::kSkip;
    return k;
}

bool KindOn(Kind k) {
    switch (k) {
        case Kind::kLoot: return S.loot;
        case Kind::kContainer: return S.containers;
        case Kind::kDoor: return S.doors;
        default: return false;
    }
}

// Every actor with the game's interact interface, merged into the list
// (known ones keep their shapes and touch state).
void RefreshList(UObject player) {
    perf::Scope timer(c_list);
    L.next_list = g_clock + 1.5;
    const UObject iface = g_iface.Get();
    if (!iface || !Statics()) return;
    ue::Call all(Statics(), "GetAllActorsWithInterface");
    all.Set("WorldContextObject", player).Set("Interface", iface);
    Arr reuse = L.list_buf;
    reuse.num = 0;
    if (reuse.data) all.Set("OutActors", reuse);
    if (!all.Invoke()) return;
    int32_t size = 0;
    const uint8_t* raw = all.Raw("OutActors", &size);
    Arr arr{};
    if (!raw || size < (int32_t)sizeof(Arr)) return;
    std::memcpy(&arr, raw, sizeof(Arr));
    L.list_buf = arr;
    std::unordered_map<UObject, size_t> old;
    for (size_t i = 0; i < L.things.size(); ++i) old[L.things[i].actor] = i;
    std::vector<Thing> next;
    next.reserve(static_cast<size_t>(std::max(arr.num, 0)));
    int counts[5] = {};
    for (int i = 0; i < arr.num && i < 2048; ++i) {
        const UObject a = mem::ReadOr<uintptr_t>(arr.data + i * sizeof(uintptr_t), 0);
        if (!a || a == player || !Live(a)) continue;
        auto it = old.find(a);
        if (it != old.end()) {
            next.push_back(std::move(L.things[it->second]));
        } else {
            Thing t;
            t.actor = a;
            t.name = ue::GetName(a);
            ClassChain(a, t);
            t.kind = ClassifyThing(t);
            ue::Call b(a, "GetActorBounds");
            b.Set("bOnlyCollidingComponents", false).Set("bIncludeFromChildActors", false);
            const FVector loc = ActorLocation(a);
            if (b.Invoke()) {
                const FVector o = b.Get<FVector>("Origin"), e = b.Get<FVector>("BoxExtent");
                t.center_off = o - loc;
                t.reach = std::fmin(Len(e), 600.0);
            }
            t.center = loc + t.center_off;
            next.push_back(std::move(t));
        }
        ++counts[static_cast<int>(next.back().kind)];
    }
    L.things.swap(next);
    bool changed = !L.counted;
    for (int k = 0; k < 5; ++k) changed = changed || (counts[k] != L.counts[k] && k != static_cast<int>(Kind::kLoot));
    if (changed) {
        log::Info("touch: %zu things to interact with here: %d loot, %d containers, %d doors, %d trigger only, %d left "
                  "to other code (listed in %.2f ms)",
                  L.things.size(), counts[2], counts[3], counts[4], counts[1], counts[0], perf::NowMs() - timer.start);
    }
    std::memcpy(L.counts, counts, sizeof(counts));
    L.counted = true;
}

// ------------------------------------------------------------------- shapes
struct CompInfo {
    UObject comp = 0;
    std::string name, cls, mesh;
    bool visible = true;
    int objtype = -1, coll = -1;
    bool has_box = false;
    Box box;
    Xf xf;
};

const char* CollName(int c) {
    switch (c) {
        case 0: return "no collision";
        case 1: return "query";
        case 2: return "physics";
        case 3: return "query+physics";
        case 4: return "probe";
        case 5: return "query+probe";
    }
    return "?";
}

bool Queryable(int coll) { return coll == 1 || coll == 3 || coll == 5; }

CompInfo ReadComp(UObject c) {
    CompInfo ci;
    ci.comp = c;
    ci.name = ue::GetName(c);
    const UObject cc = ue::GetClass(c);
    ci.cls = cc ? ue::GetName(cc) : "?";
    ci.xf = CompToWorld(c);
    ue::Call vis(c, "IsVisible");
    if (vis.Invoke()) ci.visible = vis.Result<bool>();
    if (ReadFlag(c, "bHiddenInGame") == 1) ci.visible = false;
    ue::Call ot(c, "GetCollisionObjectType");
    if (ot.Invoke()) ci.objtype = ot.Result<uint8_t>();
    ue::Call ce(c, "GetCollisionEnabled");
    if (ce.Invoke()) ci.coll = ce.Result<uint8_t>();
    const FVector s{std::fabs(ci.xf.scale.X), std::fabs(ci.xf.scale.Y), std::fabs(ci.xf.scale.Z)};
    double r = 0, hh = 0;
    ue::Vec3d e{};
    if (ci.cls == "BoxComponent" && ue::GetProp(c, "BoxExtent", e)) {
        ci.box = ScaledBox(FVector{-e.x, -e.y, -e.z}, FVector{e.x, e.y, e.z}, s);
        ci.has_box = e.x > 0 || e.y > 0 || e.z > 0;
    } else if (ci.cls == "SphereComponent" && ReadNumber(c, "SphereRadius", &r)) {
        const double k = r * std::fmin(s.X, std::fmin(s.Y, s.Z));
        ci.box = Box{FVector{-k, -k, -k}, FVector{k, k, k}};
        ci.has_box = k > 0;
    } else if (ci.cls == "CapsuleComponent" && ReadNumber(c, "CapsuleRadius", &r) &&
               ReadNumber(c, "CapsuleHalfHeight", &hh)) {
        const double kr = r * std::fmin(s.X, s.Y), kh = hh * s.Z;
        ci.box = Box{FVector{-kr, -kr, -kh}, FVector{kr, kr, kh}};
        ci.has_box = kr > 0;
    } else {
        UObject sm = 0;
        if (ue::GetProp(c, "StaticMesh", sm) && sm) {
            ci.mesh = ue::GetName(sm);
            ue::Call bb(sm, "GetBoundingBox");
            int32_t bsz = 0;
            const uint8_t* b = bb.Invoke() ? bb.Raw("ReturnValue", &bsz) : nullptr;
            if (b && bsz >= 48) {
                double v[6];
                std::memcpy(v, b, sizeof(v));
                if (v[3] - v[0] > 0.05 || v[4] - v[1] > 0.05 || v[5] - v[2] > 0.05) {
                    ci.box = ScaledBox(FVector{v[0], v[1], v[2]}, FVector{v[3], v[4], v[5]}, ci.xf.scale);
                    ci.has_box = true;
                }
            }
        }
    }
    return ci;
}

double MaxSide(const Box& b) {
    return std::fmax(b.hi.X - b.lo.X, std::fmax(b.hi.Y - b.lo.Y, b.hi.Z - b.lo.Z));
}

// The pieces of `t` a hand touches it by:
//   loot, containers: its visible meshes (the 6 biggest);
//   doors: a zone around the handle on the door leaf;
//   trigger-only things: the parts the game's own interact ray hits.
// Each falls back to the other kinds of shape, then to the actor's bounds.
void BuildParts(Thing& t) {
    perf::Scope timer(c_build);
    t.built = true;
    t.parts.clear();
    static UObject prim = 0;
    if (!prim) prim = ue::FindObject("Class /Script/Engine.PrimitiveComponent");
    std::vector<CompInfo> infos;
    if (prim) {
        ue::Call list(t.actor, "K2_GetComponentsByClass");
        list.Set("ComponentClass", prim);
        int32_t size = 0;
        const uint8_t* raw = list.Invoke() ? list.Raw("ReturnValue", &size) : nullptr;
        Arr arr{};
        if (raw && size >= 16) std::memcpy(&arr, raw, sizeof(Arr));
        for (int32_t i = 0; i < arr.num && i < 64; ++i)
            if (UObject c = mem::ReadOr<uintptr_t>(arr.data + i * 8ull)) infos.push_back(ReadComp(c));
    }
    auto add_meshes = [&](double max_side) {
        std::vector<const CompInfo*> m;
        for (const CompInfo& ci : infos)
            if (ci.has_box && ci.visible && !ci.mesh.empty() && MaxSide(ci.box) <= max_side) m.push_back(&ci);
        std::sort(m.begin(), m.end(), [](const CompInfo* a, const CompInfo* b) {
            return BoxVolume(a->box) > BoxVolume(b->box);
        });
        for (size_t i = 0; i < m.size() && i < 6; ++i) t.parts.push_back({m[i]->comp, m[i]->box, "mesh"});
    };
    auto add_interact = [&]() {
        // Object type 16 is what the game's interact ray accepts (BP_Player
        // UpdateInteractLineTrace: GetCollisionObjectType == 16).
        for (const CompInfo& ci : infos)
            if (ci.has_box && ci.objtype == 16 && Queryable(ci.coll) && t.parts.size() < 6)
                t.parts.push_back({ci.comp, ci.box, "interact shape"});
    };
    std::string door_note;
    if (t.kind == Kind::kDoor) {
        UObject leaf = 0, axis = 0, ibox = 0;
        ue::GetProp(t.actor, "office_doorB", leaf);
        ue::GetProp(t.actor, "DoorAxis", axis);
        ue::GetProp(t.actor, "InteractBox", ibox);
        const CompInfo* li = nullptr;
        const CompInfo* bi = nullptr;
        for (const CompInfo& ci : infos) {
            if (ci.comp == leaf) li = &ci;
            if (ci.comp == ibox) bi = &ci;
        }
        if (li && li->has_box) {
            const FVector up = li->xf.rotation.Inverse().Rotate(FVector{0, 0, 1});
            FVector hinge;
            if (axis) hinge = ToFrame(li->xf.rotation, li->xf.translation, CompToWorld(axis).translation);
            Box zones[2];
            const int n = DoorHandleZones(li->box, hinge, axis != 0, up, S.handle, zones);
            for (int i = 0; i < n; ++i) t.parts.push_back({leaf, zones[i], "handle zone"});
            char buf[200];
            std::snprintf(buf, sizeof(buf), "leaf %.0f x %.0f x %.0f cm, hinge %s, %d handle zone(s)",
                          li->box.hi.X - li->box.lo.X, li->box.hi.Y - li->box.lo.Y, li->box.hi.Z - li->box.lo.Z,
                          axis ? "found" : "unknown", n);
            door_note = buf;
        }
        if (bi && bi->has_box) {
            const FVector c = bi->xf.translation;
            char buf[200];
            std::snprintf(buf, sizeof(buf), "; its InteractBox is %.0f x %.0f x %.0f cm, centre %.0f cm above the leaf's "
                          "bottom", bi->box.hi.X - bi->box.lo.X, bi->box.hi.Y - bi->box.lo.Y,
                          bi->box.hi.Z - bi->box.lo.Z,
                          li ? c.Z - (li->xf.translation.Z + std::fmin(li->box.lo.Z, li->box.hi.Z)) : 0.0);
            door_note += buf;
            // An InteractBox the size of a handle is the handle: use it.
            if (MaxSide(bi->box) <= 45.0) {
                t.parts.clear();
                t.parts.push_back({ibox, bi->box, "interact box (handle sized)"});
            } else if (t.parts.empty()) {
                t.parts.push_back({ibox, bi->box, "interact box"});
            }
        }
    } else if (t.kind == Kind::kOther || t.kind == Kind::kSkip) {
        add_interact();
        if (t.parts.empty()) add_meshes(200.0);
    } else {
        add_meshes(t.kind == Kind::kLoot ? 80.0 : 300.0);
        if (t.parts.empty()) add_interact();
    }
    if (t.parts.empty()) {
        // The actor's bounds, in the world.
        ue::Call b(t.actor, "GetActorBounds");
        b.Set("bOnlyCollidingComponents", false).Set("bIncludeFromChildActors", false);
        if (b.Invoke()) {
            const FVector o = b.Get<FVector>("Origin"), e = b.Get<FVector>("BoxExtent");
            if (Len(e) < 400) t.parts.push_back({0, Box{o - e, o + e}, "bounds"});
        }
    }
    t.volume = 0;
    for (const Part& p : t.parts) t.volume += BoxVolume(p.box);

    // Study: once a run per class, what it is made of and what touch uses,
    // and its bytecode (what its Interact does).
    if (!S.study || !g_studied.insert(t.cls).second) return;
    std::string parents;
    for (size_t i = 1; i < t.chain.size(); ++i) parents += (i > 1 ? " < " : "") + t.chain[i];
    log::Info("touch: study: %s (%s; parents: %s), %zu parts; touch uses %zu %s%s%s", t.cls.c_str(), KindName(t.kind),
              parents.empty() ? "none" : parents.c_str(), infos.size(), t.parts.size(),
              t.parts.empty() ? "nothing" : t.parts[0].what, door_note.empty() ? "" : "; ", door_note.c_str());
    const FVector al = ActorLocation(t.actor);
    for (const CompInfo& ci : infos) {
        const FVector c = ci.has_box ? (ci.box.lo + ci.box.hi) * 0.5 : FVector{};
        const FVector w = ci.xf.translation + ci.xf.rotation.Rotate(c);
        log::Info("touch: study:   %s (%s%s%s) %s, object type %d, %s; %s %.1f x %.1f x %.1f cm, centre (%.0f %.0f %.0f) "
                  "from the actor",
                  ci.name.c_str(), ci.cls.c_str(), ci.mesh.empty() ? "" : ", mesh ", ci.mesh.c_str(),
                  ci.visible ? "shown" : "hidden", ci.objtype, CollName(ci.coll), ci.has_box ? "size" : "no shape",
                  ci.box.hi.X - ci.box.lo.X, ci.box.hi.Y - ci.box.lo.Y, ci.box.hi.Z - ci.box.lo.Z, w.X - al.X,
                  w.Y - al.Y, w.Z - al.Z);
    }
    QueueDump(t.chain_paths);
}

// ------------------------------------------------------------------- hands
struct Ball {
    FVector p;
    double r;
};

// The hand as balls: the drawn hand's fingertips and palm (where the owner
// sees it, stopped at walls) and the real controller's fist (which goes on
// through a wall the drawn hand rests on, so pressing on a thing counts).
int HandBalls(int h, const hands::HandState& hs, Ball* out) {
    int n = 0;
    body::HandContacts hc;
    if (body::GetHandContacts(h, &hc)) {
        const double tr = std::fmax(hc.tip_radius, 0.8);
        for (int f = 0; f < 5; ++f) out[n++] = {hc.tip[f], tr};
        out[n++] = {hc.palm - hc.palm_out * 1.5, 2.5};
    } else {
        const FQuat ar = hs.aim.rotation;
        out[n++] = {hs.wrist.location, 4.0};
        out[n++] = {hs.grip.location + ar.Rotate(FVector{5, 0, 0}), 4.0};
        out[n++] = {hs.grip.location + ar.Rotate(FVector{9, 0, 0}), 3.0};
    }
    out[n++] = {hs.grip.location, 3.5};
    return n;
}

// Nothing in the hand and nothing else using it.
bool HandFree(int h, const hands::HandState& hs) {
    if (!hs.valid || hands::EditorHand() == h) return false;
    if (hands::GetGrab(h == 0 ? hands::kLeft : hands::kRight).kind != hands::GrabKind::kNone) return false;
    if (workbench::ClaimsGrip(h)) return false;
    body::HandContacts hc;
    if (body::GetHandContacts(h, &hc) && !hc.empty) return false;
    return true;
}

// Distance from the hand's balls to the thing's parts (0 touching), using
// transforms read once per tick per component.
double HandDist(const Thing& t, const Ball* balls, int n, std::unordered_map<UObject, Xf>& xfs) {
    double best = 1e9;
    for (const Part& p : t.parts) {
        Xf xf;
        if (p.comp) {
            auto it = xfs.find(p.comp);
            if (it == xfs.end()) it = xfs.emplace(p.comp, CompToWorld(p.comp)).first;
            xf = it->second;
        }
        for (int i = 0; i < n; ++i) {
            const FVector l = p.comp ? ToFrame(xf.rotation, xf.translation, balls[i].p) : balls[i].p;
            best = std::fmin(best, std::fmax(0.0, DistToBox(l, p.box) - balls[i].r));
        }
    }
    return best;
}

Thing* FindThing(UObject actor) {
    for (Thing& t : L.things)
        if (t.actor == actor) return &t;
    return nullptr;
}

bool GamePaused(UObject player) {
    if (!Statics()) return false;
    ue::Call c(Statics(), "IsGamePaused");
    c.Set("WorldContextObject", player);
    return c.Invoke() && c.Result<bool>();
}

void CheckOutcomes() {
    for (size_t i = 0; i < L.outcomes.size();) {
        Outcome& o = L.outcomes[i];
        if (g_clock < o.at) {
            ++i;
            continue;
        }
        std::string what;
        if (!Live(o.actor)) {
            what = "it is gone (taken)";
        } else {
            const int open = ReadFlag(o.actor, "IsOpen"), take = ReadFlag(o.actor, "IsTake");
            if (open >= 0 && o.open >= 0 && open != o.open) what = open ? "it opened (IsOpen 0 -> 1)" : "it closed (IsOpen 1 -> 0)";
            else if (take >= 0 && o.take >= 0 && take != o.take) what = take ? "taken (IsTake 0 -> 1)" : "IsTake 1 -> 0";
            else if (open >= 0) what = open ? "no change the mod can see (it was already open)" : "no change the mod can see (still closed)";
            else what = "no change the mod can see";
        }
        log::Info("touch: result of %s %s (%s): %s", o.why.c_str(), o.name.c_str(), o.cls.c_str(), what.c_str());
        L.outcomes.erase(L.outcomes.begin() + static_cast<long>(i));
    }
}

void OnTick(UObject player, float dt) {
    g_clock += dt;
    if (player != L.player) {
        L = LevelState{};
        L.player = player;
    }
    if (!player) return;
    perf::Scope timer(c_tick);
    MaybeRunDump();
    CheckOutcomes();
    if (g_clock >= L.next_list) RefreshList(player);
    if (L.things.empty()) return;
    hands::Pose head;
    const bool have_head = hands::Head(&head);
    // Far things: their positions, a few a tick (items move: they pop out of
    // boxes and fall).
    for (int k = 0; k < 6 && !L.things.empty(); ++k) {
        L.rr = (L.rr + 1) % L.things.size();
        Thing& t = L.things[L.rr];
        if (Live(t.actor)) t.center = ActorLocation(t.actor) + t.center_off;
    }
    const bool can_fire = S.enabled && !ui_panel::MenuOpen() && !GamePaused(player);
    hands::HandState hs[2] = {hands::Get(hands::kLeft), hands::Get(hands::kRight)};
    bool hand_free[2];
    Ball balls[2][8];
    int nb[2] = {0, 0};
    for (int h = 0; h < 2; ++h) {
        hand_free[h] = can_fire && HandFree(h, hs[h]);
        if (hand_free[h]) nb[h] = HandBalls(h, hs[h], balls[h]);
    }
    std::unordered_map<UObject, Xf> xfs;
    struct Fire {
        Thing* t;
        int h;
        double dist;
    };
    std::vector<Fire> fires;
    for (Thing& t : L.things) {
        const bool close_by = have_head && Len(t.center - head.location) < t.reach + 150.0;
        bool live = true;
        if (close_by) {
            live = Live(t.actor);
            // Loot moves (pops out of boxes, falls); boxes and doors stay put
            // and the round above is enough for them.
            if (live && t.kind == Kind::kLoot) t.center = ActorLocation(t.actor) + t.center_off;
        }
        for (int h = 0; h < 2; ++h) {
            double d = 1e9;
            if (close_by && live && hand_free[h] && KindOn(t.kind) && Len(t.center - hs[h].grip.location) < t.reach + 40.0) {
                if (!t.built) BuildParts(t);
                d = HandDist(t, balls[h], nb[h], xfs);
            }
            const double hold = t.kind == Kind::kDoor ? S.door_hold : S.hold;
            if (StepTouch(t.st[h], d, g_clock, S.reach, S.reach + S.leave, hold)) fires.push_back({&t, h, d});
        }
    }
    // One touch a hand a tick: the smallest thing wins (an item lying on a
    // box is taken, not the box opened); the others the hand is on wait until
    // it leaves them.
    for (int h = 0; h < 2; ++h) {
        Fire* best = nullptr;
        for (Fire& f : fires)
            if (f.h == h && (!best || f.t->volume < best->t->volume)) best = &f;
        if (!best) continue;
        for (Fire& f : fires)
            if (f.h == h && &f != best) Disarm(f.t->st[h]);
        Thing& t = *best->t;
        if (RecentlyUsed(t.actor, S.again)) continue;
        for (Thing& o : L.things)
            if (&o != &t && o.st[h].inside) Disarm(o.st[h]);
        char why[64];
        std::snprintf(why, sizeof(why), "%s hand touching", h == 0 ? "left" : "right");
        InteractThroughGame(player, t.actor, why);
        input::Haptic(h == 0 ? input::kLeft : input::kRight, 0.45f, 0.05f);
    }
}

}  // namespace

bool RecentlyUsed(UObject actor, double within_s) {
    auto it = L.used.find(actor);
    return it != L.used.end() && g_clock - it->second < within_s;
}

bool InteractThroughGame(UObject player, UObject target, const char* why) {
    if (!player || !Live(target)) return false;
    UObject game_target = 0;
    ue::GetProp(player, "InteractTarget", game_target);
    bool focused = true;
    if (game_target != target) {
        // As if looked at: the thing sets up its prompt (InteractInOverlay),
        // then it is the player's target. The game's own view trace takes
        // the target back on its next tick.
        ue::Call focus(target, "InteractInOverlay");
        focused = focus.Invoke();
        ue::SetProp(player, "InteractTarget", target);
    }
    ue::SetProp(player, "IsInteractReady", uint8_t{1});
    ue::Call c(player, "PlayerInput_Interact");
    const bool ok = c.Invoke();
    L.used[target] = g_clock;
    Outcome o;
    o.actor = target;
    o.why = why;
    if (const Thing* t = FindThing(target)) {
        o.name = t->name;
        o.cls = t->cls;
        o.kind = t->kind;
    } else {
        o.name = ue::GetName(target);
        const UObject cls = ue::GetClass(target);
        o.cls = cls ? ue::GetName(cls) : "?";
    }
    o.open = ReadFlag(target, "IsOpen");
    o.take = ReadFlag(target, "IsTake");
    o.at = g_clock + 1.0;
    log::Info("touch: %s %s (%s, %s%s%s) through the game's interact%s%s", why, o.name.c_str(), o.cls.c_str(),
              KindName(o.kind), o.open < 0 ? "" : o.open ? ", open" : ", closed",
              game_target == target ? ", also where you look" : "", focused ? "" : ", no InteractInOverlay",
              ok ? "" : ", call failed");
    L.outcomes.push_back(std::move(o));
    return ok;
}

Pick PickForHand(UObject player, int hand, const std::vector<UObject>& ignore) {
    Pick out;
    if (!player) return out;
    if (player != L.player) {
        L = LevelState{};
        L.player = player;
    }
    if (L.things.empty() || g_clock >= L.next_list) RefreshList(player);
    const hands::HandState hs = hands::Get(hand == 0 ? hands::kLeft : hands::kRight);
    if (!hs.valid) return out;
    Ball balls[8];
    const int n = HandBalls(hand, hs, balls);
    std::unordered_map<UObject, Xf> xfs;
    // Touching: anything the game can interact with but the drink machine
    // (pressed and drunk by hand there), a bit more forgiving than a touch on
    // its own, since the trigger says it's meant.
    const Thing* best = nullptr;
    const Thing* nearest = nullptr;
    double nearest_d = 1e9;
    for (Thing& t : L.things) {
        if (t.kind == Kind::kSkip || !Live(t.actor)) continue;
        const double c = Len(t.center - hs.grip.location);
        if (c < nearest_d) {
            nearest_d = c;
            nearest = &t;
        }
        if (c > t.reach + 40.0) continue;
        if (!t.built) BuildParts(t);
        const double d = HandDist(t, balls, n, xfs);
        if (d <= S.reach + 3.0 && (!best || t.volume < best->volume)) best = &t;
    }
    if (best) {
        out.actor = best->actor;
        out.how = "touching";
        out.cls = best->cls;
        out.kind = KindName(best->kind);
        return out;
    }
    // Pointing: the game's own interact trace (channel 2, the hit part's
    // object type 16) from the hand instead of the eyes, as far as the game
    // reaches when you look.
    double reach = 200.0;
    if (!ReadNumber(player, "InteractiveDistance", &reach) || reach < 50.0 || reach > 2000.0) reach = 200.0;
    const FVector o = hs.aim.location, d = hs.aim.rotation.Rotate(FVector{1, 0, 0});
    if (Ksl() && Statics()) {
        Arr arr{ignore.empty() ? 0 : reinterpret_cast<uintptr_t>(ignore.data()), static_cast<int32_t>(ignore.size()),
                static_cast<int32_t>(ignore.size())};
        ue::Call t(Ksl(), "LineTraceSingle");
        t.Set("WorldContextObject", player).Set("Start", o).Set("End", o + d * reach).Set("TraceChannel", uint8_t{2})
            .Set("bTraceComplex", false).Set("ActorsToIgnore", arr).Set("bIgnoreSelf", true);
        int32_t hit_size = 0;
        uint8_t* hit = nullptr;
        if (t.Invoke() && t.Result<bool>()) hit = t.Raw("OutHit", &hit_size);
        if (hit) {
            ue::Call br(Statics(), "BreakHitResult");
            int32_t in_size = 0;
            uint8_t* in = br.Raw("Hit", &in_size);
            if (in && in_size == hit_size) {
                std::memcpy(in, hit, static_cast<size_t>(hit_size));
                if (br.Invoke()) {
                    const UObject actor = br.Get<UObject>("HitActor"), comp = br.Get<UObject>("HitComponent");
                    int type = -1;
                    if (comp) {
                        ue::Call ot(comp, "GetCollisionObjectType");
                        if (ot.Invoke()) type = ot.Result<uint8_t>();
                    }
                    if (const Thing* th = actor && type == 16 ? FindThing(actor) : nullptr) {
                        out.actor = th->actor;
                        out.how = "pointing at";
                        out.cls = th->cls;
                        out.kind = KindName(th->kind);
                        return out;
                    }
                }
            }
        }
    }
    static int s_diag = 0;
    if (s_diag++ < 30)
        log::Info("touch: the %s hand touches and points at nothing to interact with (%zu things here; nearest %s, "
                  "%s, %.0f cm from the hand)",
                  hand == 0 ? "left" : "right", L.things.size(), nearest ? nearest->name.c_str() : "none",
                  nearest ? nearest->cls.c_str() : "-", nearest_d);
    return out;
}

void Init() {
    LoadSettings();
    config::AddChangeListener(&LoadSettings);
    for (perf::Counter* c : {&c_tick, &c_list, &c_build}) perf::Register(c);
    game::AddTickListener(&OnTick);
    log::Info("touch: touch interact %s (loot %d, containers %d, doors %d; touch within %.1f cm, rest %.2f s, doors "
              "%.2f s)",
              S.enabled ? "on" : "off", S.loot, S.containers, S.doors, S.reach, S.hold, S.door_hold);
}

}  // namespace holevr::vr::touch
