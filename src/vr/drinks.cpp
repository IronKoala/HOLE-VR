#include "vr/drinks.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "game/game.h"
#include "ue/call.h"
#include "ue/names.h"
#include "ue/objects.h"
#include "vr/body/body.h"
#include "vr/hands.h"
#include "vr/input.h"
#include "vr/math.h"

namespace holevr::vr::drinks {
namespace {

using ue::FName;
using ue::UObject;

// The hub's drink machine. A /Game/ Blueprint of the home level: freed when
// you leave home, so only ever held in an ObjectRef (the 2026-11-03 CD crash).
constexpr const char* kMachineClass =
    "BlueprintGeneratedClass /Game/GamePlay/LevelComponents/LCBP_SkillUpgradeNew.LCBP_SkillUpgradeNew_C";
constexpr int kButtons = 6;

// ---------------------------------------------------------------- settings
struct Settings {
    bool enabled = true;          // drinks_by_hand
    bool finger_buttons = true;   // drinks_finger_buttons
    double press_gap_cm = 0.6;    // a fingertip this close in front of a yellow square presses it
    double sink_cm = 0.35;        // how far the pressed square goes in
    double grab_cm = 10.0;        // grip this close to the can's middle takes it
    double hold_along_cm = 1.5;   // can middle ahead of the fist centre, along the fist
    double hold_in_cm = 1.2;      // can middle this far further from the palm than the controller's handle
    double opening_deg = 0.0;     // turns the tab and opening around the can
    double tab_reach_cm = 2.0;    // fingertip this close to the tab opens it
    double mouth_forward_cm = 4.0;
    double mouth_down_cm = 10.0;
    double mouth_radius_cm = 11.0;
    double pour_tilt_deg = 60.0;  // can tipped at least this far from upright pours
    double pour_seconds = 1.2;    // pouring this long drinks it
    double hide_after_s = 6.0;    // the game's own can stays hidden this long after a drink
    double empty_can_s = 30.0;    // a dropped empty can lies there this long
} S;

void LoadSettings() {
    S.enabled = config::GetBool("drinks_by_hand", true);
    S.finger_buttons = config::GetBool("drinks_finger_buttons", true);
    S.press_gap_cm = config::GetFloat("drinks_press_gap_cm", 0.6f);
    S.sink_cm = config::GetFloat("drinks_button_sink_cm", 0.35f);
    S.grab_cm = config::GetFloat("drinks_grab_cm", 10.0f);
    S.hold_along_cm = config::GetFloat("drinks_can_along_fist_cm", 1.5f);
    S.hold_in_cm = config::GetFloat("drinks_can_from_palm_cm", 1.2f);
    S.opening_deg = config::GetFloat("drinks_can_opening_deg", 0.0f);
    S.tab_reach_cm = config::GetFloat("drinks_tab_reach_cm", 2.0f);
    S.mouth_forward_cm = config::GetFloat("drinks_mouth_forward_cm", 4.0f);
    S.mouth_down_cm = config::GetFloat("drinks_mouth_down_cm", 10.0f);
    S.mouth_radius_cm = config::GetFloat("drinks_mouth_radius_cm", 11.0f);
    S.pour_tilt_deg = config::GetFloat("drinks_pour_tilt_deg", 60.0f);
    S.pour_seconds = config::GetFloat("drinks_pour_seconds", 1.2f);
    S.hide_after_s = config::GetFloat("drinks_hide_game_can_s", 6.0f);
    S.empty_can_s = config::GetFloat("drinks_empty_can_s", 30.0f);
}

// ---------------------------------------------------------------- engine helpers
struct alignas(16) Xf {
    FQuat rotation;
    FVector translation;
    double pad0 = 0;
    FVector scale{1, 1, 1};
    double pad1 = 0;
};
static_assert(sizeof(Xf) == 0x60, "FTransform layout");

double Len(const FVector& v) { return std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z); }
FVector Norm(const FVector& v) {
    const double l = Len(v);
    return l > 1e-6 ? v * (1.0 / l) : FVector{0, 0, 1};
}
double Dot(const FVector& a, const FVector& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
double Axis(const FVector& v, int k) { return k == 0 ? v.X : k == 1 ? v.Y : v.Z; }
FVector Unit(int k) { return k == 0 ? FVector{1, 0, 0} : k == 1 ? FVector{0, 1, 0} : FVector{0, 0, 1}; }

// Rotation about a unit axis (radians), same formula as UE's FQuat(Axis, Angle).
FQuat AxisAngle(const FVector& axis, double rad) {
    const double s = std::sin(rad * 0.5);
    return FQuat{axis.X * s, axis.Y * s, axis.Z * s, std::cos(rad * 0.5)};
}

// Rotation whose X/Y/Z axes are the given orthonormal vectors.
FQuat FromAxes(const FVector& x, const FVector& y, const FVector& z) {
    const double m00 = x.X, m11 = y.Y, m22 = z.Z, tr = m00 + m11 + m22;
    FQuat q;
    if (tr > 0) {
        const double s = std::sqrt(tr + 1.0) * 2;
        q.W = 0.25 * s;
        q.X = (y.Z - z.Y) / s;
        q.Y = (z.X - x.Z) / s;
        q.Z = (x.Y - y.X) / s;
    } else if (m00 > m11 && m00 > m22) {
        const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2;
        q.W = (y.Z - z.Y) / s;
        q.X = 0.25 * s;
        q.Y = (y.X + x.Y) / s;
        q.Z = (z.X + x.Z) / s;
    } else if (m11 > m22) {
        const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2;
        q.W = (z.X - x.Z) / s;
        q.X = (y.X + x.Y) / s;
        q.Y = 0.25 * s;
        q.Z = (z.Y + y.Z) / s;
    } else {
        const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2;
        q.W = (x.Y - y.X) / s;
        q.X = (z.X + x.Z) / s;
        q.Y = (z.Y + y.Z) / s;
        q.Z = 0.25 * s;
    }
    return q;
}

Xf ComponentToWorld(UObject comp) {
    ue::Call c(comp, "K2_GetComponentToWorld");
    if (!comp || !c.Invoke()) return Xf{};
    return c.Result<Xf>();
}
void SetWorldXf(UObject comp, const Xf& t) {
    ue::Call c(comp, "K2_SetWorldTransform");
    c.Set("NewTransform", t).Set("bSweep", false).Set("bTeleport", true);
    c.Invoke();
}
void SetHiddenInGame(UObject comp, bool hidden) {
    ue::Call c(comp, "SetHiddenInGame");
    c.Set("NewHidden", hidden).Set("bPropagateToChildren", false);
    c.Invoke();
}
bool CompVisible(UObject comp) {
    ue::Call c(comp, "IsVisible");
    return comp && c.Invoke() && c.Result<bool>();
}
FVector ActorLocation(UObject a) {
    ue::Call c(a, "K2_GetActorLocation");
    c.Invoke();
    return c.Result<FVector>();
}
UObject ChildActor(UObject child_actor_component) {
    UObject a = 0;
    if (child_actor_component) ue::GetProp(child_actor_component, "ChildActor", a);
    return a && ue::IsLive(a) ? a : 0;
}
UObject CompProp(UObject actor, const char* name) {
    UObject c = 0;
    if (actor) ue::GetProp(actor, name, c);
    return c && ue::IsLive(c) ? c : 0;
}
std::string MeshName(UObject comp) {
    UObject sm = 0;
    if (!comp || !ue::GetProp(comp, "StaticMesh", sm) || !sm) return "no mesh";
    return ue::GetName(sm);
}
// The static mesh asset's own box (component space, before its scale).
// GetLocalBounds read 0 on the range timer (2026-11-03), this does not.
bool StaticMeshBox(UObject comp, FVector* mn, FVector* mx) {
    UObject sm = 0;
    if (!comp || !ue::GetProp(comp, "StaticMesh", sm) || !sm) return false;
    ue::Call c(sm, "GetBoundingBox");
    if (!c.Invoke()) return false;
    int32_t size = 0;
    const uint8_t* raw = c.Raw("ReturnValue", &size);
    if (!raw || size < 48) return false;
    double v[6];
    std::memcpy(v, raw, sizeof(v));
    *mn = FVector{v[0], v[1], v[2]};
    *mx = FVector{v[3], v[4], v[5]};
    return v[3] - v[0] > 0.1 || v[4] - v[1] > 0.1 || v[5] - v[2] > 0.1;
}
FVector AbsScale(const Xf& t) { return {std::fabs(t.scale.X), std::fabs(t.scale.Y), std::fabs(t.scale.Z)}; }
FVector Mul(const FVector& a, const FVector& b) { return {a.X * b.X, a.Y * b.Y, a.Z * b.Z}; }

const char* HandName(int h) { return h == 0 ? "left" : "right"; }
bool HandFree(int h) {
    const hands::GrabKind k = hands::GetGrab(static_cast<hands::Hand>(h)).kind;
    return k == hands::GrabKind::kNone || k == hands::GrabKind::kHolster;
}

// The game's interact on `target`, the way the interact key does it after
// its view trace found the thing: the thing's InteractInOverlay (sets up its
// prompt and readiness), the player's InteractTarget / IsInteractReady, then
// the player's own interact input.
bool GameInteract(UObject player, UObject target, std::string* note) {
    if (!player || !target || !ue::IsLive(target)) {
        if (note) *note = "nothing to interact with";
        return false;
    }
    UObject before = 0;
    ue::GetProp(player, "InteractTarget", before);
    ue::Call focus(target, "InteractInOverlay");
    const bool focused = focus.Invoke();
    ue::SetProp(player, "InteractTarget", target);
    ue::SetProp(player, "IsInteractReady", uint8_t{1});
    ue::Call c(player, "PlayerInput_Interact");
    const bool ok = c.Invoke();
    if (note) {
        char b[160];
        std::snprintf(b, sizeof(b), "through the game's interact%s%s; the game's view target was %s",
                      focused ? "" : ", no InteractInOverlay", ok ? "" : ", CALL FAILED",
                      before && ue::IsLive(before) ? ue::GetName(before).c_str() : "none");
        *note = b;
    }
    return ok;
}

// Names, looked up once (a full pass over the name pool) on the finder thread.
std::atomic<bool> g_names_ready{false};
FName g_color{}, g_physics_profile{}, g_none{};
bool g_have_color = false, g_have_physics = false;

// A plain engine shape (100 cm Cube) in one colour, no collision, movable.
UObject MakeShape(UObject actor, float r, float g, float b) {
    static UObject cls = ue::FindObject("Class /Script/Engine.StaticMeshComponent");
    static UObject cube = ue::FindObject("StaticMesh /Engine/BasicShapes/Cube.Cube");
    static UObject mat = ue::FindObject("Material /Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");
    if (!cls || !cube || !actor) return 0;
    ue::Call add(actor, "AddComponentByClass");
    add.Set("Class", cls).Set("bManualAttachment", true).Set("RelativeTransform", Xf{}).Set("bDeferredFinish", false);
    UObject c = add.Invoke() ? add.Result<UObject>() : 0;
    if (!c) return 0;
    ue::Call col(c, "SetCollisionEnabled");
    col.Set("NewType", uint8_t{0});
    col.Invoke();
    ue::Call ov(c, "SetGenerateOverlapEvents");
    ov.Set("bInGenerateOverlapEvents", false);
    ov.Invoke();
    ue::Call mob(c, "SetMobility");
    mob.Set("NewMobility", uint8_t{2});
    mob.Invoke();
    ue::Call sm(c, "SetStaticMesh");
    sm.Set("NewMesh", cube);
    sm.Invoke();
    if (mat && g_have_color) {
        ue::Call mid(c, "CreateDynamicMaterialInstance");
        mid.Set("ElementIndex", int32_t{0}).Set("SourceMaterial", mat);
        UObject m = mid.Invoke() ? mid.Result<UObject>() : 0;
        if (m) {
            struct { float r, g, b, a; } lc{r, g, b, 1.0f};
            ue::Call v(m, "SetVectorParameterValue");
            v.Set("ParameterName", g_color).Set("Value", lc);
            v.Invoke();
        }
    }
    return c;
}

// A copy of a game mesh (the can), no collision, movable.
UObject MakeMeshCopy(UObject actor, UObject mesh) {
    static UObject cls = ue::FindObject("Class /Script/Engine.StaticMeshComponent");
    if (!cls || !mesh || !actor) return 0;
    ue::Call add(actor, "AddComponentByClass");
    add.Set("Class", cls).Set("bManualAttachment", true).Set("RelativeTransform", Xf{}).Set("bDeferredFinish", false);
    UObject c = add.Invoke() ? add.Result<UObject>() : 0;
    if (!c) return 0;
    ue::Call col(c, "SetCollisionEnabled");
    col.Set("NewType", uint8_t{0});
    col.Invoke();
    ue::Call ov(c, "SetGenerateOverlapEvents");
    ov.Set("bInGenerateOverlapEvents", false);
    ov.Invoke();
    ue::Call mob(c, "SetMobility");
    mob.Set("NewMobility", uint8_t{2});
    mob.Invoke();
    ue::Call sm(c, "SetStaticMesh");
    sm.Set("NewMesh", mesh);
    sm.Invoke();
    return c;
}

void DestroyComp(UObject comp) {
    if (!comp || !ue::IsLive(comp)) return;
    ue::Call c(comp, "K2_DestroyComponent");
    c.Set("Object", comp);
    c.Invoke();
}

// ---------------------------------------------------------------- finder thread
// Finds the machine without scanning objects on the game thread (a scan of
// every object costs milliseconds; the hub has one machine).
ue::ObjectRef g_machine_class{kMachineClass, 10.0};   // finder thread only
std::atomic<UObject> g_found{0};

DWORD WINAPI FinderThread(LPVOID) {
    if (!g_names_ready.load()) {
        g_have_color = ue::names::Find("Color", &g_color);
        g_have_physics = ue::names::Find("PhysicsActor", &g_physics_profile);
        ue::names::Find("None", &g_none);
        g_names_ready = true;
    }
    for (;;) {
        Sleep(2000);
        if (!S.enabled) continue;
        const UObject cls = g_machine_class.Get();
        if (!cls) {
            g_found = 0;
            continue;
        }
        const UObject cur = g_found.load();
        if (cur && ue::IsLive(cur) && ue::GetClass(cur) == cls) continue;
        g_found = ue::FindFirstInstance(cls);
    }
}

// ---------------------------------------------------------------- the machine
struct Button {
    int index = 0;              // 1..6, the switch component's number
    UObject sw = 0;             // LCBP_SkillUpgradeSwitchNew_C
    UObject mesh = 0;           // the yellow square (Home_VendingMachine_ButtonNN), or the switch's own shape
    FVector mn, mx;             // its box, component space, scaled
    Xf rest{};                  // its world transform at rest
    int axis = 0;               // the thin axis: the way it is pushed
    double sign = 1;            // +1: its face is at mx on that axis
    bool movable = false;
    bool inside[2] = {false, false};
    bool sunk = false;
    std::string row;            // SkillRowName (which drink)
};

struct Machine {
    UObject player = 0;
    UObject actor = 0;
    double age = 0;
    bool set_up = false;
    Button btn[kButtons];
    int n_btn = 0;
    FVector center;             // middle of the machine's body
    UObject slot = 0;           // LCBP_SkillUpgradeSlotNew_C
    UObject can = 0;            // the game's can mesh
    double ready_at = 0;        // next press allowed
    double next_miss_log = 0;
    // Watching the game's can and the slot flags.
    bool watch_init = false;
    bool can_vis = false;
    uint8_t empty_slot = 0xFF, open_slot = 0xFF;
    FVector can_last;
    double can_still = 0;
    bool can_ready = false;     // visible and at rest: can be taken
    FVector can_mid;            // the game can's middle (world), when ready
    bool logged_rest = false;
    double next_far_check = 0;
    bool hands_close = false;   // a hand is within 3 m of the machine
    double hide_game_can_until = -1;
    bool hiding_game_can = false;
} M;

struct Held {
    bool on = false;
    int hand = -1;
    UObject comp = 0, tab = 0, hole = 0;
    FVector mn, mx;             // can mesh box, component space (unscaled)
    FVector scale{1, 1, 1};
    FQuat to_mesh{0, 0, 0, 1};  // mesh space -> can frame (Z = its axis)
    double height = 12, radius = 3.3;
    FVector mid_mesh;           // box middle, mesh space, scaled
    bool opened = false;
    double tab_t = 0;           // 0 closed .. 1 open
    bool tab_armed = false;     // the other hand's fingers were away from the tab first
    double pour = 0, next_gulp = 0, next_diag = 0;
    bool pouring = false;
    bool drunk = false;
    FVector last_mid, vel;
    // This frame's can frame.
    FVector mid, top, ax, ay, az;
} H;

struct Loose {
    UObject comp = 0, tab = 0, hole = 0;
    double age = 0;
    FVector start;
    bool checked = false;
};
std::vector<Loose> g_loose;

double g_clock = 0;

void ResetLevel(UObject player) {
    // A new player in the same hub (respawn): tidy up what we made and show
    // the game's can again. A new level: its objects are gone with it, so
    // they are only forgotten, never touched.
    if (M.actor && ue::IsLive(M.actor)) {
        for (UObject c : {H.comp, H.tab, H.hole}) DestroyComp(c);
        for (const Loose& l : g_loose)
            for (UObject c : {l.comp, l.tab, l.hole}) DestroyComp(c);
        if ((H.on || M.hiding_game_can) && M.can && ue::IsLive(M.can)) SetHiddenInGame(M.can, false);
    }
    M = Machine{};
    M.player = player;
    H = Held{};
    g_loose.clear();
}

// Which drink a switch sells, and its state, for the log.
std::string SwitchInfo(UObject sw) {
    if (!sw || !ue::IsLive(sw)) return "gone";
    FName row{};
    uint8_t unlocked = 0, available = 0, maxlv = 0;
    int32_t lv = 0, buy = 0, cost = 0;
    ue::GetProp(sw, "SkillRowName", row);
    ue::GetProp(sw, "IsUnlocked", unlocked);
    ue::GetProp(sw, "IsAvailable", available);
    ue::GetProp(sw, "IsMaxLv", maxlv);
    ue::GetProp(sw, "CurrentSkillLv", lv);
    ue::GetProp(sw, "BuySkillLv", buy);
    ue::GetProp(sw, "UpgradeCost", cost);
    char b[200];
    std::snprintf(b, sizeof(b), "%s: %s%s%s, level %d (buys %d), cost %d", ue::names::ToString(row).c_str(),
                  unlocked ? "unlocked" : "LOCKED", available ? ", available" : ", not available",
                  maxlv ? ", MAX" : "", lv, buy, cost);
    return b;
}

void SetUpMachine() {
    M.set_up = true;
    const UObject a = M.actor;
    const FVector origin = ActorLocation(a);
    // The machine's body, for which way its front faces.
    M.center = origin;
    if (UObject body = CompProp(a, "Home_VendingMachine_Home_VendingMachine")) {
        FVector mn, mx;
        const Xf t = ComponentToWorld(body);
        if (StaticMeshBox(body, &mn, &mx))
            M.center = t.translation + t.rotation.Rotate(Mul((mn + mx) * 0.5, AbsScale(t)));
    }
    M.slot = ChildActor(CompProp(a, "LCBP_SkillUpgradeSlot"));
    M.can = CompProp(a, "Can");
    log::Info("drinks: drink machine %s at (%.0f %.0f %.0f), body middle (%.0f %.0f %.0f); slot %s, can mesh %s (%s)",
              ue::GetName(a).c_str(), origin.X, origin.Y, origin.Z, M.center.X, M.center.Y, M.center.Z,
              M.slot ? ue::GetName(M.slot).c_str() : "NOT FOUND", M.can ? MeshName(M.can).c_str() : "NOT FOUND",
              M.can && CompVisible(M.can) ? "visible" : "hidden");

    // The yellow squares: the machine's ButtonNN meshes.
    struct Square { UObject comp; FVector mn, mx, mid; Xf t; bool used; };
    std::vector<Square> squares;
    for (int i = 1; i <= 9; ++i) {
        char name[48];
        std::snprintf(name, sizeof(name), "Home_VendingMachine_Button%02d", i);
        UObject c = CompProp(a, name);
        if (!c) continue;
        Square q{};
        q.comp = c;
        q.t = ComponentToWorld(c);
        if (!StaticMeshBox(c, &q.mn, &q.mx)) continue;
        const FVector s = AbsScale(q.t);
        q.mn = Mul(q.mn, s);
        q.mx = Mul(q.mx, s);
        q.mid = q.t.translation + q.t.rotation.Rotate((q.mn + q.mx) * 0.5);
        squares.push_back(q);
    }

    M.n_btn = 0;
    for (int i = 1; i <= kButtons; ++i) {
        char name[48];
        std::snprintf(name, sizeof(name), "LCBP_SkillUpgradeSwitch%d", i);
        const UObject sw = ChildActor(CompProp(a, name));
        if (!sw) {
            log::Info("drinks: button %d: no switch actor", i);
            continue;
        }
        Button b;
        b.index = i;
        b.sw = sw;
        FName row{};
        ue::GetProp(sw, "SkillRowName", row);
        b.row = ue::names::ToString(row);
        // Where the switch's interact box is.
        FVector at = ActorLocation(sw);
        UObject box = CompProp(sw, "Box");
        if (box) at = ComponentToWorld(box).translation;
        // Its yellow square: the nearest unused ButtonNN.
        int best = -1;
        double best_d = 25.0;
        for (size_t k = 0; k < squares.size(); ++k) {
            const double d = Len(squares[k].mid - at);
            if (!squares[k].used && d < best_d) {
                best_d = d;
                best = static_cast<int>(k);
            }
        }
        UObject cube = CompProp(sw, "Cube");
        if (best >= 0) {
            Square& q = squares[best];
            q.used = true;
            b.mesh = q.comp;
            b.mn = q.mn;
            b.mx = q.mx;
            b.rest = q.t;
        } else if (cube) {
            // No yellow square near it: the switch's own Cube.
            FVector mn, mx;
            const Xf t = ComponentToWorld(cube);
            if (StaticMeshBox(cube, &mn, &mx)) {
                b.mesh = cube;
                b.mn = Mul(mn, AbsScale(t));
                b.mx = Mul(mx, AbsScale(t));
                b.rest = t;
            }
        }
        if (!b.mesh) {
            log::Info("drinks: button %d (%s): no shape to press; the trigger interact still works", i,
                      SwitchInfo(sw).c_str());
            continue;
        }
        // Pushed along its thinnest side, from the side away from the machine's middle.
        const FVector ext = b.mx - b.mn;
        b.axis = ext.X <= ext.Y && ext.X <= ext.Z ? 0 : ext.Y <= ext.Z ? 1 : 2;
        const FVector mid = b.rest.translation + b.rest.rotation.Rotate((b.mn + b.mx) * 0.5);
        const FVector n = b.rest.rotation.Rotate(Unit(b.axis));
        b.sign = Dot(n, mid - M.center) >= 0 ? 1.0 : -1.0;
        const FVector face_n = n * b.sign;
        log::Info("drinks: button %d = %s (%s): %.1f x %.1f x %.1f cm at (%.0f %.0f %.0f), %.0f cm from the switch box, "
                  "faces (%.2f %.2f %.2f); switch Cube %s %s",
                  i, ue::GetName(b.mesh).c_str(), SwitchInfo(sw).c_str(), ext.X, ext.Y, ext.Z, mid.X, mid.Y, mid.Z,
                  best >= 0 ? best_d : -1.0, face_n.X, face_n.Y, face_n.Z, cube ? MeshName(cube).c_str() : "none",
                  cube && CompVisible(cube) ? "visible" : "hidden");
        if (S.sink_cm > 0) {
            ue::Call m(b.mesh, "SetMobility");
            m.Set("NewMobility", uint8_t{2});
            b.movable = m.Invoke();
        }
        M.btn[M.n_btn++] = b;
    }
    if (M.slot) {
        UObject box = CompProp(M.slot, "Box");
        UObject spline = CompProp(M.slot, "Spline");
        const Xf t = box ? ComponentToWorld(box) : Xf{};
        FVector e{};
        if (box) ue::GetProp(box, "BoxExtent", e);
        std::string path;
        if (spline) {
            ue::Call n(spline, "GetNumberOfSplinePoints");
            const int count = n.Invoke() ? n.Result<int32_t>() : 0;
            for (int i = 0; i < count && i < 6; ++i) {
                ue::Call p(spline, "GetLocationAtSplinePoint");
                p.Set("PointIndex", int32_t{i}).Set("CoordinateSpace", uint8_t{1});   // world
                if (!p.Invoke()) break;
                const FVector v = p.Result<FVector>();
                char b[64];
                std::snprintf(b, sizeof(b), " (%.0f %.0f %.0f)", v.X, v.Y, v.Z);
                path += b;
            }
        }
        log::Info("drinks: can slot box at (%.0f %.0f %.0f), %.0f x %.0f x %.0f cm; its spline:%s",
                  t.translation.X, t.translation.Y, t.translation.Z, e.X * 2 * t.scale.X, e.Y * 2 * t.scale.Y,
                  e.Z * 2 * t.scale.Z, path.empty() ? " none" : path.c_str());
    }
    if (UObject cl = CompProp(a, "CanLocation")) {
        const FVector p = ComponentToWorld(cl).translation;
        log::Info("drinks: CanLocation at (%.0f %.0f %.0f)", p.X, p.Y, p.Z);
    }
}

// ---------------------------------------------------------------- fingertips
struct Tip {
    FVector p;
    double r;
    int finger;   // 0 thumb .. 4 pinky; 5 = the controller's rough guess
};
const char* FingerName(int f) {
    static const char* n[] = {"thumb", "index finger", "middle finger", "ring finger", "little finger",
                              "fingers (controller guess)"};
    return n[std::clamp(f, 0, 5)];
}
// The fingertips the owner sees (vr/body, after hands stop at walls and
// fingers give way), else a rough point ahead of the controller.
int HandTips(int h, Tip out[5]) {
    body::HandContacts hc;
    if (body::GetHandContacts(h, &hc)) {
        for (int f = 0; f < 5; ++f) out[f] = Tip{hc.tip[f], std::fmax(hc.tip_radius, 0.6), f};
        return 5;
    }
    const hands::HandState hs = hands::Get(static_cast<hands::Hand>(h));
    if (!hs.valid) return 0;
    out[0] = Tip{hs.grip.location + hs.aim.rotation.Rotate(FVector{9, 0, 0}), 1.5, 5};
    return 1;
}

// ---------------------------------------------------------------- buttons
void SinkButton(Button& b, bool sink) {
    if (sink == b.sunk || !b.movable || S.sink_cm <= 0) return;
    b.sunk = sink;
    Xf t = b.rest;
    if (sink) t.translation = t.translation - b.rest.rotation.Rotate(Unit(b.axis)) * (b.sign * S.sink_cm);
    SetWorldXf(b.mesh, t);
}

void PressButtons(UObject player) {
    if (!S.finger_buttons) return;
    Tip tips[2][5];
    int n_tips[2] = {0, 0};
    for (int h = 0; h < 2; ++h) n_tips[h] = HandTips(h, tips[h]);
    for (int i = 0; i < M.n_btn; ++i) {
        Button& b = M.btn[i];
        if (!ue::IsLive(b.sw) || !ue::IsLive(b.mesh)) continue;
        const int k = b.axis, a1 = (k + 1) % 3, a2 = (k + 2) % 3;
        const double face = b.sign > 0 ? Axis(b.mx, k) : Axis(b.mn, k);
        const double depth_max = Axis(b.mx, k) - Axis(b.mn, k) + 3.0;
        const FQuat inv = b.rest.rotation.Inverse();
        bool any = false;
        for (int h = 0; h < 2; ++h) {
            bool in = false;
            int finger = -1;
            double gap_at = 0;
            double near_gap = 1e9;   // closest miss over the square, for the log
            int near_finger = -1;
            for (int t = 0; t < n_tips[h]; ++t) {
                const FVector l = inv.Rotate(tips[h][t].p - b.rest.translation);
                if (Axis(l, a1) < Axis(b.mn, a1) - 0.3 || Axis(l, a1) > Axis(b.mx, a1) + 0.3) continue;
                if (Axis(l, a2) < Axis(b.mn, a2) - 0.3 || Axis(l, a2) > Axis(b.mx, a2) + 0.3) continue;
                // Positive: in front of the face.
                const double gap = b.sign * (Axis(l, k) - face);
                if (gap <= tips[h][t].r + S.press_gap_cm && gap >= -depth_max) {
                    in = true;
                    finger = tips[h][t].finger;
                    gap_at = gap;
                    break;
                }
                if (gap > 0 && gap < near_gap) {
                    near_gap = gap;
                    near_finger = tips[h][t].finger;
                }
            }
            // A fingertip over a square that stops short (hands stop at the
            // machine's surface): say how far, so the reach can be set.
            if (!in && near_gap < 4.0 && g_clock >= M.next_miss_log) {
                M.next_miss_log = g_clock + 2.0;
                log::Info("drinks: %s hand's %s is %.1f cm in front of button %d's face (presses within %.1f)",
                          HandName(h), FingerName(near_finger), near_gap, b.index,
                          (n_tips[h] ? tips[h][0].r : 0.6) + S.press_gap_cm);
            }
            if (in && !b.inside[h]) {
                input::Haptic(static_cast<input::Hand>(h), 0.45f, 0.04f);
                if (g_clock >= M.ready_at) {
                    M.ready_at = g_clock + 1.0;
                    std::string note;
                    const std::string info = SwitchInfo(b.sw);
                    GameInteract(player, b.sw, &note);
                    log::Info("drinks: %s hand's %s pressed button %d (%s), %.1f cm off its face, %s", HandName(h),
                              FingerName(finger), b.index, info.c_str(), gap_at, note.c_str());
                } else {
                    log::Info("drinks: %s hand touched button %d %.2f s after the last press (not pressed again)",
                              HandName(h), b.index, g_clock - (M.ready_at - 1.0));
                }
            }
            b.inside[h] = in;
            any = any || in;
        }
        SinkButton(b, any);
    }
}

// ---------------------------------------------------------------- the can
void WatchGameCan() {
    if (!M.can || !ue::IsLive(M.can)) {
        M.can_ready = false;
        return;
    }
    uint8_t empty = 0xFF, open = 0xFF;
    ue::GetProp(M.actor, "IsEmptySlot", empty);
    ue::GetProp(M.actor, "IsOpenSlot", open);
    const Xf t = ComponentToWorld(M.can);
    const bool vis = !M.hiding_game_can && !H.on && CompVisible(M.can);
    if (!M.watch_init || empty != M.empty_slot || open != M.open_slot || vis != M.can_vis) {
        UObject sel = 0;
        ue::GetProp(M.actor, "SelectedCanMesh", sel);
        if (M.watch_init || vis)
            log::Info("drinks: machine: slot %s, %s; the game's can %s at (%.0f %.0f %.0f), mesh %s (selected %s)%s",
                      empty == 1 ? "empty" : empty == 0 ? "has a can" : "?", open == 1 ? "open" : open == 0 ? "closed" : "?",
                      vis ? "VISIBLE" : "hidden", t.translation.X, t.translation.Y, t.translation.Z,
                      MeshName(M.can).c_str(), sel ? ue::GetName(sel).c_str() : "none",
                      M.hiding_game_can || H.on ? " (we hide it)" : "");
        M.watch_init = true;
        M.empty_slot = empty;
        M.open_slot = open;
        M.can_vis = vis;
        M.logged_rest = false;
    }
    const double moved = Len(t.translation - M.can_last);
    M.can_last = t.translation;
    if (moved > 0.05) M.can_still = 0.0;   // still time is added each tick in OnTick
    M.can_ready = false;
    if (!vis) return;
    FVector mn, mx;
    if (!StaticMeshBox(M.can, &mn, &mx)) return;
    M.can_mid = t.translation + t.rotation.Rotate(Mul((mn + mx) * 0.5, AbsScale(t)));
    M.can_ready = M.can_still >= 0.25;
    if (M.can_ready && !M.logged_rest) {
        M.logged_rest = true;
        const FVector up = t.rotation.Rotate(FVector{0, 0, 1});
        log::Info("drinks: the game's can is at rest at (%.0f %.0f %.0f) (mesh up (%.2f %.2f %.2f)); reach in and grip to "
                  "take it",
                  M.can_mid.X, M.can_mid.Y, M.can_mid.Z, up.X, up.Y, up.Z);
    }
}

// This frame's can frame from the hand. The can lies through the fist like
// the controller's handle: +X of the grip pose runs through the closed fist
// from the little finger to the thumb (OpenXR grip -Z), so the top is at the
// thumb. A can is fatter than the handle, so its middle sits a little further
// from the palm.
void CanFrame(const hands::HandState& hs) {
    const FQuat r = hs.grip.rotation;
    H.az = Norm(r.Rotate(FVector{1, 0, 0}));
    FVector ax = r.Rotate(FVector{0, 0, 1});
    FVector ay = r.Rotate(FVector{0, -1, 0});
    if (S.opening_deg != 0) {
        const FQuat roll = AxisAngle(H.az, S.opening_deg * kDegToRad);
        ax = roll.Rotate(ax);
        ay = roll.Rotate(ay);
    }
    H.ax = Norm(ax);
    H.ay = Norm(ay);
    H.mid = hs.grip.location + H.az * S.hold_along_cm - hs.palm_normal * S.hold_in_cm;
    H.top = H.mid + H.az * (H.height * 0.5);
}

void PlaceHeld() {
    const FQuat can_rot = FromAxes(H.ax, H.ay, H.az);
    Xf t;
    t.rotation = can_rot * H.to_mesh;
    t.scale = H.scale;
    t.translation = H.mid - t.rotation.Rotate(H.mid_mesh);
    SetWorldXf(H.comp, t);
    // Tab: lies on the top, its nose toward the opening (+X); it opens by
    // lifting its ring end about a hinge at the nose.
    const double tab_len = 1.9, tab_w = 1.2, tab_th = 0.15;
    const FVector rest_mid = H.top + H.ax * (H.radius * 0.05) + H.az * (tab_th * 0.5 + 0.05);
    const FVector hinge = rest_mid + H.ax * (tab_len * 0.5);
    const FQuat lift = AxisAngle(H.ay, H.tab_t * 70.0 * kDegToRad);
    if (H.tab) {
        Xf tt;
        tt.rotation = lift * can_rot;
        tt.translation = hinge + lift.Rotate(rest_mid - hinge);
        tt.scale = FVector{tab_len / 100.0, tab_w / 100.0, tab_th / 100.0};
        SetWorldXf(H.tab, tt);
    }
    if (H.hole) {
        Xf ht;
        ht.rotation = can_rot;
        ht.translation = H.top + H.ax * (H.radius * 0.55) + H.az * 0.04;
        ht.scale = FVector{1.5 / 100.0, 1.1 / 100.0, 0.06 / 100.0};
        SetWorldXf(H.hole, ht);
    }
}

void TakeCan(int h, const hands::HandState& hs) {
    UObject mesh = 0;
    ue::GetProp(M.can, "StaticMesh", mesh);
    FVector mn, mx;
    if (!mesh || !StaticMeshBox(M.can, &mn, &mx)) {
        log::Warn("drinks: the game's can has no mesh to copy");
        return;
    }
    const Xf gt = ComponentToWorld(M.can);
    H = Held{};
    H.hand = h;
    H.scale = AbsScale(gt);
    H.mn = mn;
    H.mx = mx;
    // The mesh's long side is the can's axis (Z for a normal mesh).
    const FVector ext = Mul(mx - mn, H.scale);
    const int k = ext.Z >= ext.X && ext.Z >= ext.Y ? 2 : ext.X >= ext.Y ? 0 : 1;
    // to_mesh turns mesh space into can space (can Z = its axis): a mesh
    // long on X turns -90 deg about Y (X -> Z), one long on Y +90 about X (Y -> Z).
    H.to_mesh = k == 2 ? FQuat{0, 0, 0, 1} : k == 0 ? AxisAngle(FVector{0, 1, 0}, -kPi * 0.5)
                                                    : AxisAngle(FVector{1, 0, 0}, kPi * 0.5);
    H.height = Axis(ext, k);
    H.radius = 0.25 * (Axis(ext, (k + 1) % 3) + Axis(ext, (k + 2) % 3));
    H.mid_mesh = Mul((mn + mx) * 0.5, H.scale);
    H.comp = MakeMeshCopy(M.actor, mesh);
    if (!H.comp) {
        log::Warn("drinks: could not make the can for the hand");
        H = Held{};
        return;
    }
    H.tab = MakeShape(M.actor, 0.62f, 0.62f, 0.65f);
    H.hole = MakeShape(M.actor, 0.01f, 0.01f, 0.01f);
    if (H.hole) SetHiddenInGame(H.hole, true);
    H.on = true;
    SetHiddenInGame(M.can, true);
    CanFrame(hs);
    H.last_mid = H.mid;
    PlaceHeld();
    input::Haptic(static_cast<input::Hand>(h), 0.5f, 0.05f);
    log::Info("drinks: %s hand took the can (%s, %.1f cm tall, %.1f cm wide, long side %c) out of the slot, %.1f cm from "
              "its middle; the game's can stays in the slot, hidden. Open the tab with the other hand's finger.",
              HandName(h), ue::GetName(mesh).c_str(), H.height, H.radius * 2, "XYZ"[k],
              Len(hs.grip.location - M.can_mid));
}

void PutBack(const char* why) {
    DestroyComp(H.comp);
    DestroyComp(H.tab);
    DestroyComp(H.hole);
    if (M.can && ue::IsLive(M.can)) SetHiddenInGame(M.can, false);
    log::Info("drinks: the can went back into the slot (%s)%s", why, H.opened ? "; it was opened, not drunk" : "");
    H = Held{};
}

void DropEmpty() {
    Loose l;
    l.comp = H.comp;
    l.tab = H.tab;
    l.hole = H.hole;
    l.start = H.mid;
    // Tab and opening ride on the can now.
    for (UObject child : {H.tab, H.hole}) {
        if (!child) continue;
        ue::Call at(child, "K2_AttachToComponent");
        at.Set("Parent", H.comp).Set("SocketName", g_none).Set("LocationRule", uint8_t{1})
            .Set("RotationRule", uint8_t{1}).Set("ScaleRule", uint8_t{1}).Set("bWeldSimulatedBodies", false);
        at.Invoke();
    }
    bool physics = false;
    if (g_have_physics) {
        ue::Call p(H.comp, "SetCollisionProfileName");
        p.Set("InCollisionProfileName", g_physics_profile).Set("bUpdateOverlaps", false);
        p.Invoke();
        ue::Call ce(H.comp, "SetCollisionEnabled");
        ce.Set("NewType", uint8_t{3});   // query and physics
        ce.Invoke();
        ue::Call sim(H.comp, "SetSimulatePhysics");
        sim.Set("bSimulate", true);
        physics = sim.Invoke();
        ue::Call v(H.comp, "SetPhysicsLinearVelocity");
        v.Set("NewVel", H.vel).Set("bAddToCurrent", false).Set("BoneName", g_none);
        v.Invoke();
    }
    log::Info("drinks: let go of the empty can (moving %.0f cm/s)%s", Len(H.vel),
              physics ? "; it falls" : "; no physics profile, it goes away");
    if (!physics) {
        DestroyComp(H.comp);
        DestroyComp(H.tab);
        DestroyComp(H.hole);
    } else {
        g_loose.push_back(l);
    }
    H = Held{};
}

void Drink(UObject player) {
    std::string note;
    const bool ok = GameInteract(player, M.slot, &note);
    H.drunk = true;
    M.hide_game_can_until = g_clock + S.hide_after_s;
    input::Haptic(static_cast<input::Hand>(H.hand), 0.8f, 0.12f);
    log::Info("drinks: DRANK the can (poured %.1f s at the mouth): the game's slot interact %s, %s", H.pour,
              ok ? "ran" : "FAILED", note.c_str());
}

void HoldCan(UObject player, float dt) {
    const input::Snapshot snap = input::LastConsumed();
    const hands::HandState hs = hands::Get(static_cast<hands::Hand>(H.hand));
    if (!ue::IsLive(H.comp)) {
        log::Info("drinks: the can in the hand went away");
        DestroyComp(H.tab);
        DestroyComp(H.hole);
        if (!H.drunk && M.can && ue::IsLive(M.can)) SetHiddenInGame(M.can, false);
        H = Held{};
        return;
    }
    if (hs.valid && !snap.hand[H.hand].grip_button.down) {
        if (H.drunk) DropEmpty();
        else PutBack("let go before drinking");
        return;
    }
    if (!hs.valid) return;
    CanFrame(hs);
    if (dt > 1e-4) {
        const FVector v = (H.mid - H.last_mid) * (1.0 / dt);
        H.vel = H.vel * 0.6 + v * 0.4;
    }
    H.last_mid = H.mid;

    // The tab: a fingertip of the other hand on it.
    const int other = 1 - H.hand;
    if (!H.opened) {
        Tip tips[5];
        const int n = HandTips(other, tips);
        const FVector tab = H.top + H.ax * (H.radius * 0.05) + H.az * 0.1;
        double best = 1e9;
        int finger = -1;
        for (int t = 0; t < n; ++t) {
            const double d = Len(tips[t].p - tab) - tips[t].r;
            if (d < best) {
                best = d;
                finger = tips[t].finger;
            }
        }
        if (best > S.tab_reach_cm + 2.0) H.tab_armed = true;
        if (H.tab_armed && best <= S.tab_reach_cm) {
            H.opened = true;
            input::Haptic(static_cast<input::Hand>(other), 0.6f, 0.05f);
            input::Haptic(static_cast<input::Hand>(H.hand), 0.35f, 0.05f);
            if (H.hole) SetHiddenInGame(H.hole, false);
            log::Info("drinks: the %s hand's %s opened the tab (%.1f cm from it). Tip the can at your mouth to drink.",
                      HandName(other), FingerName(finger), std::fmax(best, 0.0));
        }
    }
    if (H.opened && H.tab_t < 1.0) H.tab_t = std::fmin(1.0, H.tab_t + dt / 0.12);
    PlaceHeld();

    // Pouring: the open can's top at the mouth, tipped.
    if (!H.opened || H.drunk) return;
    hands::Pose head;
    if (!hands::Head(&head)) return;
    const FVector mouth = head.location + head.rotation.Rotate(FVector{S.mouth_forward_cm, 0, -S.mouth_down_cm});
    const double d = Len(H.top - mouth);
    const double tilt = std::acos(std::clamp(H.az.Z, -1.0, 1.0)) * kRadToDeg;
    const bool pouring = d <= S.mouth_radius_cm && tilt >= S.pour_tilt_deg;
    if (pouring != H.pouring) {
        log::Info("drinks: %s pouring (can top %.1f cm from the mouth, tipped %.0f deg)", pouring ? "started" : "stopped",
                  d, tilt);
        H.pouring = pouring;
    }
    if (!pouring) {
        // Close to the mouth but not quite: say why, now and then.
        if (d <= S.mouth_radius_cm + 15.0 && g_clock >= H.next_diag) {
            H.next_diag = g_clock + 2.0;
            log::Info("drinks: can at the mouth? top %.1f cm from it (needs %.0f), tipped %.0f deg (needs %.0f)", d,
                      S.mouth_radius_cm, tilt, S.pour_tilt_deg);
        }
        return;
    }
    H.pour += dt;
    if (g_clock >= H.next_gulp) {
        H.next_gulp = g_clock + 0.35;
        input::Haptic(static_cast<input::Hand>(H.hand), 0.3f, 0.06f);
    }
    if (H.pour >= S.pour_seconds) Drink(player);
}

void TendLoose(float dt) {
    for (size_t i = 0; i < g_loose.size();) {
        Loose& l = g_loose[i];
        l.age += dt;
        bool gone = !ue::IsLive(l.comp);
        if (!gone && !l.checked && l.age >= 0.5) {
            l.checked = true;
            const FVector p = ComponentToWorld(l.comp).translation;
            const double moved = Len(p - l.start);
            log::Info("drinks: the dropped can moved %.0f cm in its first half second%s", moved,
                      moved < 1.0 ? " (no physics: removed)" : "");
            if (moved < 1.0) l.age = 1e9;
        }
        if (!gone && l.age >= S.empty_can_s) {
            DestroyComp(l.comp);
            DestroyComp(l.tab);
            DestroyComp(l.hole);
            gone = true;
        }
        if (gone) {
            g_loose.erase(g_loose.begin() + static_cast<long>(i));
            continue;
        }
        ++i;
    }
}

void OnTick(UObject player, float dt) {
    g_clock += dt;
    if (!S.enabled) return;
    if (player != M.player) ResetLevel(player);
    if (!player) return;
    M.age += dt;
    const UObject found = g_found.load();
    if (found != M.actor) {
        if (M.actor && H.on && ue::IsLive(M.actor)) PutBack("the machine changed");
        const UObject keep_player = M.player;
        M = Machine{};
        M.player = keep_player;
        H = Held{};
        M.actor = found && ue::IsLive(found) ? found : 0;
        if (M.actor) log::Info("drinks: found the drink machine (%s)", ue::GetName(M.actor).c_str());
    }
    if (!M.actor) return;
    if (!ue::IsLive(M.actor)) {
        M.actor = 0;
        H = Held{};
        g_loose.clear();
        return;
    }
    if (!M.set_up) {
        if (M.age < 1.0) return;
        SetUpMachine();
    }
    TendLoose(dt);
    if (H.on) {
        HoldCan(player, dt);
    }
    // Only near the machine: everything below reads the hands against it.
    if (g_clock >= M.next_far_check) {
        M.next_far_check = g_clock + 0.5;
        M.hands_close = false;
        for (int h = 0; h < 2; ++h) {
            const hands::HandState hs = hands::Get(static_cast<hands::Hand>(h));
            if (hs.valid && Len(hs.grip.location - M.center) < 300.0) M.hands_close = true;
        }
    }
    // The game's can stays hidden a while after a drink (the hand had it).
    if (M.hide_game_can_until > 0) {
        if (g_clock < M.hide_game_can_until) {
            if (!M.hiding_game_can && M.can) {
                SetHiddenInGame(M.can, true);
                M.hiding_game_can = true;
            }
        } else {
            M.hide_game_can_until = -1;
            if (M.can && ue::IsLive(M.can)) {
                SetHiddenInGame(M.can, false);
                const Xf t = ComponentToWorld(M.can);
                log::Info("drinks: after the drink the game's can is %s at (%.0f %.0f %.0f)",
                          CompVisible(M.can) ? "VISIBLE" : "hidden", t.translation.X, t.translation.Y,
                          t.translation.Z);
            }
            M.hiding_game_can = false;
        }
    }
    if (!M.hands_close) return;
    M.can_still += dt;
    WatchGameCan();
    PressButtons(player);
    if (H.on || !M.can_ready) return;
    const input::Snapshot snap = input::LastConsumed();
    for (int h = 0; h < 2; ++h) {
        const hands::HandState hs = hands::Get(static_cast<hands::Hand>(h));
        if (!hs.valid || !snap.hand[h].grip_button.pressed || !HandFree(h)) continue;
        const double d = Len(hs.grip.location - M.can_mid);
        if (d <= S.grab_cm) {
            TakeCan(h, hs);
            break;
        }
        if (d <= S.grab_cm + 15.0)
            log::Info("drinks: %s hand gripped %.1f cm from the can (takes it within %.0f)", HandName(h), d, S.grab_cm);
    }
}

}  // namespace

bool ClaimsGrip(int hand) {
    if (hand < 0 || hand > 1 || !S.enabled) return false;
    if (H.on) return H.hand == hand;
    if (!M.actor || !M.can_ready) return false;
    const hands::HandState hs = hands::Get(static_cast<hands::Hand>(hand));
    return hs.valid && HandFree(hand) && Len(hs.grip.location - M.can_mid) <= S.grab_cm;
}

void Init() {
    LoadSettings();
    config::AddChangeListener(&LoadSettings);
    game::AddTickListener(&OnTick);
    if (HANDLE t = CreateThread(nullptr, 0, &FinderThread, nullptr, 0, nullptr)) CloseHandle(t);
    log::Info("drinks: drink machine by hand %s (finger buttons %s)", S.enabled ? "on" : "off",
              S.finger_buttons ? "on" : "off");
}

}  // namespace holevr::vr::drinks
