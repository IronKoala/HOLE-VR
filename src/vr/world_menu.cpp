#include "vr/world_menu.h"

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "game/game.h"
#include "ue/call.h"
#include "ue/names.h"
#include "ue/objects.h"
#include "ue/process_event.h"
#include "ue/reflection.h"
#include "vr/dxgi_hooks.h"
#include "vr/input.h"
#include "vr/math.h"
#include "vr/openxr.h"
#include "vr/stereo.h"
#include "vr/ui_panel.h"

namespace holevr::vr::world_menu {
namespace {
using ue::FName;
using ue::UObject;

template <typename T>
T Proc(const char* name) {
    return reinterpret_cast<T>(xr::GetProc(name));
}

// ------------------------------------------------------------- settings --

struct Settings {
    bool enabled = true;
    float width_m = 1.8f;
    float distance_m = 1.6f;
    float drop_m = 0.15f;
    float follow_deg = 60.f;
    float backdrop = 0.f;
    bool forearm = true;          // on the back of the off-hand forearm, else floating in front
    int arm = input::kLeft;
    float arm_width_m = 0.30f;
    float arm_along_m = 0.20f;    // from the controller grip toward the elbow
    float arm_back_m = 0.05f;     // out from the back of the forearm
    bool hide_covers = true;      // the menus' full-screen click-catcher
    bool laser = true;
} S;

// The game's menus are laid out for a 1920x1080 screen.
constexpr int32_t kDrawW = 1920, kDrawH = 1080;

// ---------------------------------------------------- engine constants --

constexpr uint32_t RF_ClassDefaultObject = 0x10;
constexpr uint32_t RF_BeginDestroyed = 0x8000;
constexpr uint32_t RF_FinishDestroyed = 0x10000;

constexpr uint8_t kSpaceWorld = 0;          // EWidgetSpace::World
constexpr uint8_t kBlendTransparent = 2;    // EWidgetBlendMode::Transparent
constexpr uint8_t kSourceWorld = 0;         // EWidgetInteractionSource::World
constexpr uint8_t kVisible = 0, kCollapsed = 1, kHidden = 2;  // ESlateVisibility

// FTransform (double precision).
struct alignas(16) Xf {
    FQuat rotation;
    FVector translation;
    double pad0 = 0;
    FVector scale{1, 1, 1};
    double pad1 = 0;
};
static_assert(sizeof(Xf) == 0x60, "FTransform layout");

struct LinearColor {
    float r, g, b, a;
};
struct Vector2D {
    double x, y;
};
// FKey: the name plus a lazily filled details pointer we leave empty.
struct Key {
    FName name;
    void* details_obj = nullptr;
    void* details_ref = nullptr;
};
static_assert(sizeof(Key) == 0x18, "FKey layout");

// --------------------------------------------- what the scan thread finds --

struct Candidate {
    UObject obj = 0;
    int index = -1;
    UObject cls = 0;
};
std::atomic<bool> g_active{false};

UObject g_wbgc_class = 0;  // WidgetBlueprintGeneratedClass
std::unordered_set<UObject> g_menu_classes;

// What the game does with its menus, seen on the game thread:
//  - a UIM_ menu's Construct event fires when the game puts it on the screen
//    (verified in the 2026-11-02 study runs: Pause and Option both do this);
//  - Widget.RemoveFromParent is how its Blueprints take a menu away again.
// Construct alone can't tell us a hosted menu closed: the panel keeps its
// Slate widget alive, so we also follow RemoveFromParent itself.
std::mutex g_events_mutex;
std::vector<Candidate> g_new_menus;
std::vector<UObject> g_removed;
thread_local bool t_our_call = false;  // our own RemoveFromParent
ue::process_event::NativeFn g_orig_remove = nullptr;
std::atomic<bool> g_menu_up{false};
std::unordered_set<UObject> g_not_menu_classes;

bool IsMenuClass(UObject cls) {
    if (g_menu_classes.count(cls)) return true;
    if (g_not_menu_classes.count(cls)) return false;
    bool menu = false;
    if (ue::GetClass(cls) == g_wbgc_class) {
        std::string n = ue::GetName(cls);
        // Top-level screens are UIM_*; the in-game HUD stays where it is.
        // The title screen and loading screen are left alone (hosting the
        // title froze the game once); UIM_HUD is the in-game HUD.
        menu = n.rfind("UIM_", 0) == 0 && n != "UIM_HUD_C" && n != "UIM_Title_C" && n != "UIM_Loading_C";
    }
    (menu ? g_menu_classes : g_not_menu_classes).insert(cls);
    return menu;
}

// ----------------------------------------------- game-thread state --

struct Panel {
    UObject widget = 0;
    int widget_index = -1;
    UObject comp = 0;
    int comp_index = -1;
    std::string name;
    double hosted_at = 0;
    bool saw_open = false;     // IsOpened() has said true at least once
    bool open_unreliable = false;
    uint8_t last_vis = 255;
    int layer = 0;             // 0 = the first menu, sub-menus come forward
};

struct Pointer {
    bool trigger_down[2] = {};
    int hand = input::kRight;
    bool key_down = false;
    float scroll_accum = 0.f;
    bool was_over_button = false;
};

struct State {
    UObject actor = 0;
    int actor_index = -1;
    UObject wic = 0;
    int wic_index = -1;
    std::vector<Panel> panels;
    // Where the panels sit (world): the first panel's centre and facing yaw.
    bool placed = false;
    FVector centre;
    double yaw = 0;
    FQuat rot;           // panel rotation; its +X is the face
    double up_sign = 1;  // forearm: which edge is the top
    double cm_per_m = 100;
    double closed_since = -1;  // cursor gone at this time
    double last_take = 0;
    float last_distance = -1;
    Pointer pt;
} G;

double Now() { return GetTickCount64() / 1000.0; }

// A picture of the headset view a second after a menu goes on the arm, so
// placement can be checked from the log folder without asking the player.
double g_capture_at = -1;
std::string g_capture_name;

// Laser shared with the OpenXR thread.
struct Laser {
    bool show = false;
    int hand = input::kRight;
    bool hit = false;
    bool pressed = false;
    float length_m = 0.f;
};
std::mutex g_laser_mutex;
Laser g_laser;

// ------------------------------------------------------------ helpers --

UObject Ksl() {
    static UObject k = 0;
    if (!k) k = ue::FindObject("KismetSystemLibrary /Script/Engine.Default__KismetSystemLibrary");
    return k;
}

// The object still lives in the same slot and the engine calls it valid.
bool Alive(UObject obj, int index) {
    if (!obj || index < 0 || ue::objects::Get(index) != obj) return false;
    uint32_t flags = mem::ReadOr<uint32_t>(obj + ue::off::kObjFlags, RF_BeginDestroyed);
    if (flags & (RF_BeginDestroyed | RF_FinishDestroyed | RF_ClassDefaultObject)) return false;
    UObject k = Ksl();
    if (!k) return true;
    ue::Call c(k, "IsValid");
    c.Set("Object", obj);
    return c.Invoke() && c.Result<bool>();
}

int IndexOf(UObject obj) { return mem::ReadOr<int32_t>(obj + ue::off::kObjIndex, -1); }

bool CallBool(UObject obj, const char* fn) {
    ue::Call c(obj, fn);
    return c.Invoke() && c.Result<bool>();
}

void CallVoid(UObject obj, const char* fn) {
    ue::Call c(obj, fn);
    c.Invoke();
}

uint8_t Visibility(UObject widget) {
    uint8_t v = kVisible;
    ue::GetProp(widget, "Visibility", v);
    return v;
}

bool FindName(const char* text, FName* out) {
    if (ue::names::Find(text, out)) return true;
    log::Warn("world menu: name \"%s\" not found", text);
    return false;
}

FName LeftMouse() {
    static FName n;
    static bool tried = false;
    if (!tried) {
        tried = true;
        FindName("LeftMouseButton", &n);
    }
    return n;
}

// ---------------------------------------------------------- the actor --

UObject SpawnHost(UObject world_context) {
    static UObject statics = 0, actor_class = 0;
    if (!statics) statics = ue::FindObject("GameplayStatics /Script/Engine.Default__GameplayStatics");
    if (!actor_class) actor_class = ue::FindObject("Class /Script/Engine.Actor");
    if (!statics || !actor_class) return 0;
    Xf t;
    ue::Call begin(statics, "BeginDeferredActorSpawnFromClass");
    begin.Set("WorldContextObject", world_context).Set("ActorClass", actor_class).Set("SpawnTransform", t)
        .Set("CollisionHandlingOverride", static_cast<uint8_t>(1));  // AlwaysSpawn
    if (!begin.Invoke() || !begin.Result<UObject>()) return 0;
    UObject actor = begin.Result<UObject>();
    ue::Call finish(statics, "FinishSpawningActor");
    finish.Set("Actor", actor).Set("SpawnTransform", t);
    finish.Invoke();
    return finish.Result<UObject>() ? finish.Result<UObject>() : actor;
}

UObject AddComponent(UObject actor, const char* class_name, bool deferred) {
    UObject cls = ue::FindObject(class_name);
    if (!cls) {
        log::Error("world menu: %s not found", class_name);
        return 0;
    }
    ue::Call add(actor, "AddComponentByClass");
    add.Set("Class", cls).Set("bManualAttachment", true).Set("RelativeTransform", Xf{})
        .Set("bDeferredFinish", deferred);
    if (!add.Invoke()) return 0;
    return add.Result<UObject>();
}

void FinishComponent(UObject actor, UObject comp) {
    ue::Call fin(actor, "FinishAddComponent");
    fin.Set("Component", comp).Set("bManualAttachment", true).Set("RelativeTransform", Xf{});
    fin.Invoke();
}

void TickWhenPaused(UObject comp) {
    ue::Call c(comp, "SetTickableWhenPaused");
    c.Set("bTickableWhenPaused", true);
    c.Invoke();
}

void SetWorldPose(UObject comp, const FVector& loc, const FRotator& rot) {
    ue::Call c(comp, "K2_SetWorldLocationAndRotation");
    c.Set("NewLocation", loc).Set("NewRotation", rot).Set("bSweep", false).Set("bTeleport", true);
    c.Invoke();
}

void DestroyComponent(UObject comp, int index) {
    if (!Alive(comp, index)) return;
    ue::Call set(comp, "SetWidget");
    set.Set("Widget", UObject{0});
    set.Invoke();
    ue::Call d(comp, "K2_DestroyComponent");
    d.Set("Object", comp);
    d.Invoke();
}

bool EnsureActor(UObject world_context) {
    if (G.actor && Alive(G.actor, G.actor_index)) {
        if (G.wic && Alive(G.wic, G.wic_index)) return true;
    } else {
        // A new level (or first use): everything we made went with the old one.
        G.actor = G.wic = 0;
        G.panels.clear();
        G.placed = false;
        G.actor = SpawnHost(world_context);
        if (!G.actor) {
            log::Error("world menu: could not create the panel holder");
            return false;
        }
        G.actor_index = IndexOf(G.actor);
        log::Info("world menu: panel holder %s", ue::GetName(G.actor).c_str());
    }
    UObject wic = AddComponent(G.actor, "Class /Script/UMG.WidgetInteractionComponent", true);
    if (!wic) {
        log::Error("world menu: could not create the pointer");
        return false;
    }
    ue::SetProp<uint8_t>(wic, "InteractionSource", kSourceWorld);
    ue::SetProp<float>(wic, "InteractionDistance", 0.f);
    FinishComponent(G.actor, wic);
    TickWhenPaused(wic);
    G.wic = wic;
    G.wic_index = IndexOf(wic);
    G.last_distance = 0.f;
    return true;
}

// ---------------------------------------------------------- placement --

bool HeadInWorld(FVector* loc, double* yaw) {
    xr::EyeView eyes[2];
    if (!xr::LatestViews(eyes)) return false;
    XrPosef head = eyes[0].pose;
    head.position.x = (eyes[0].pose.position.x + eyes[1].pose.position.x) * 0.5f;
    head.position.y = (eyes[0].pose.position.y + eyes[1].pose.position.y) * 0.5f;
    head.position.z = (eyes[0].pose.position.z + eyes[1].pose.position.z) * 0.5f;
    FQuat q;
    if (!stereo::TrackingToWorld(head, loc, &q)) return false;
    *yaw = YawOf(q);
    // World units per metre, from a point one metre further along the view.
    XrPosef ahead = head;
    ahead.position.y += 1.f;
    FVector a;
    if (stereo::TrackingToWorld(ahead, &a, nullptr)) {
        FVector d = a - *loc;
        double len = std::sqrt(d.X * d.X + d.Y * d.Y + d.Z * d.Z);
        if (len > 1.0 && len < 10000.0) G.cm_per_m = len;
    }
    return true;
}

double PanelWidthM() { return S.forearm ? S.arm_width_m : S.width_m; }
double Scale() { return PanelWidthM() * G.cm_per_m / kDrawW; }

// Sub-menus sit a little in front of the menu they came from.
FVector PanelCentre(const Panel& p) {
    FVector n = G.rot.Rotate(FVector{1, 0, 0});
    double toward = p.layer * (S.forearm ? 0.006 : 0.04) * G.cm_per_m;
    return G.centre + n * toward;
}

// Facing back at the head: the widget's front is its +X side.
FRotator PanelRotation() { return ToRotator(G.rot); }


// Rotation whose columns are the given orthonormal axes.
FQuat FromAxes(const FVector& x, const FVector& y, const FVector& z) {
    double tr = x.X + y.Y + z.Z;
    if (tr > 0) {
        double s = std::sqrt(tr + 1.0) * 2.0;
        return {(y.Z - z.Y) / s, (z.X - x.Z) / s, (x.Y - y.X) / s, 0.25 * s};
    }
    if (x.X > y.Y && x.X > z.Z) {
        double s = std::sqrt(1.0 + x.X - y.Y - z.Z) * 2.0;
        return {0.25 * s, (y.X + x.Y) / s, (z.X + x.Z) / s, (y.Z - z.Y) / s};
    }
    if (y.Y > z.Z) {
        double s = std::sqrt(1.0 + y.Y - x.X - z.Z) * 2.0;
        return {(y.X + x.Y) / s, 0.25 * s, (z.Y + y.Z) / s, (z.X - x.Z) / s};
    }
    double s = std::sqrt(1.0 + z.Z - x.X - y.Y) * 2.0;
    return {(z.X + x.Z) / s, (z.Y + y.Z) / s, 0.25 * s, (x.Y - y.X) / s};
}

FVector Cross(const FVector& a, const FVector& b) {
    return {a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X};
}
double DotV(const FVector& a, const FVector& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }

// The menu lies on the back of the forearm like a big watch face: its long
// side along the arm, its face out of the back of the arm, and its top on
// whichever side of the arm is higher in your view, so it reads upright.
// Uses the controller, not the body, so it works whatever the body does.
bool PlaceOnForearm() {
    input::Snapshot in = input::Peek();
    if (!in.valid || !in.hand[S.arm].grip_valid) return G.placed;
    FVector loc;
    FQuat q;
    if (!stereo::TrackingToWorld(in.hand[S.arm].grip, &loc, &q)) return G.placed;
    FVector head;
    double yaw;
    HeadInWorld(&head, &yaw);  // refreshes cm_per_m
    // Grip axes in UE: +X through the fist, +Z along the thumb, +Y out of the
    // back of the left hand.
    double side = S.arm == input::kLeft ? 1.0 : -1.0;
    FVector back = q.Rotate(FVector{0, side, 0});
    FVector thumb = q.Rotate(FVector{0, 0, 1});
    G.centre = loc + q.Rotate(FVector{-S.arm_along_m * G.cm_per_m, side * S.arm_back_m * G.cm_per_m, 0});
    xr::EyeView eyes[2];
    if (xr::LatestViews(eyes)) {
        FQuat hq;
        if (stereo::TrackingToWorld(eyes[0].pose, nullptr, &hq)) {
            FVector head_up = hq.Rotate(FVector{0, 0, 1});
            if (DotV(thumb, head_up) * G.up_sign < -0.25) G.up_sign = -G.up_sign;
        }
    }
    FVector up = thumb * G.up_sign;
    FVector y = Cross(up, back);  // widget's left edge
    G.rot = FromAxes(back, y, up);
    G.placed = true;
    return true;
}

void PosePanel(const Panel& p) {
    if (!Alive(p.comp, p.comp_index)) return;
    double s = Scale();
    SetWorldPose(p.comp, PanelCentre(p), PanelRotation());
    ue::Call sc(p.comp, "SetWorldScale3D");
    sc.Set("NewScale", FVector{s, s, s});
    sc.Invoke();
}

void PlacePanels(const FVector& head, double yaw) {
    double d = S.distance_m * G.cm_per_m;
    G.yaw = yaw;
    G.rot = YawOnly(yaw + 180.0);
    G.centre = {head.X + std::cos(yaw * kDegToRad) * d, head.Y + std::sin(yaw * kDegToRad) * d,
                head.Z - S.drop_m * G.cm_per_m};
    G.placed = true;
    for (auto& p : G.panels) PosePanel(p);
}

// ------------------------------------------------------------ hosting --

bool Host(const Candidate& c) {
    if (!EnsureActor(c.obj)) return false;
    UObject comp = AddComponent(G.actor, "Class /Script/UMG.WidgetComponent", true);
    if (!comp) {
        log::Error("world menu: could not create a panel for %s", ue::GetName(c.obj).c_str());
        return false;
    }
    // Before the component starts: these pick its material and texture size.
    ue::SetProp<uint8_t>(comp, "Space", kSpaceWorld);
    ue::SetProp<uint8_t>(comp, "BlendMode", kBlendTransparent);
    struct IntPoint {
        int32_t x, y;
    } size{kDrawW, kDrawH};
    ue::SetProp(comp, "DrawSize", size);
    ue::SetProp<uint8_t>(comp, "bIsTwoSided", 1);
    ue::SetProp<uint8_t>(comp, "TickWhenOffscreen", 1);
    ue::SetProp(comp, "BackgroundColor", LinearColor{0.f, 0.f, 0.f, S.backdrop});
    FinishComponent(G.actor, comp);
    TickWhenPaused(comp);
    FName ui;
    if (ue::names::Find("UI", &ui)) {
        ue::Call cp(comp, "SetCollisionProfileName");
        cp.Set("InCollisionProfileName", ui).Set("bUpdateOverlaps", false);
        cp.Invoke();
    }

    // Onto the panel first, then off the flat screen. In this order the
    // panel keeps the menu's Slate widget alive, so the game doesn't see the
    // menu destroyed and built again (its Destruct/Construct events).
    ue::Call set(comp, "SetWidget");
    set.Set("Widget", c.obj);
    set.Invoke();
    t_our_call = true;
    CallVoid(c.obj, "RemoveFromParent");
    t_our_call = false;

    Panel p;
    p.widget = c.obj;
    p.widget_index = c.index;
    p.comp = comp;
    p.comp_index = IndexOf(comp);
    p.name = ue::GetName(ue::GetClass(c.obj));
    p.hosted_at = Now();
    p.layer = G.panels.empty() ? 0 : G.panels.back().layer + 1;
    G.panels.push_back(p);
    log::Info("world menu: %s is now a panel on the forearm (layer %d), off the flat screen", p.name.c_str(), p.layer);
    g_capture_at = Now() + 1.0;
    g_capture_name = p.name;

    FVector head;
    double yaw;
    if (S.forearm) {
        if (PlaceOnForearm()) PosePanel(G.panels.back());
    } else if (G.placed) PosePanel(G.panels.back());
    else if (HeadInWorld(&head, &yaw)) PlacePanels(head, yaw);
    if (S.hide_covers) {
        // The full-screen "click here to get focus back" cover: a box over the
        // whole panel that a VR player never needs.
        UObject cover = 0;
        if (ue::GetProp(c.obj, "UIP_FocusLostCover", cover) && cover) {
            ue::Call v(cover, "SetVisibility");
            v.Set("InVisibility", kCollapsed);
            v.Invoke();
        }
    }
    // Full-screen backdrops inside the menu (a background blur or image the
    // size of the screen) would read as a box on the arm. List what the menu
    // is built from, and hide any background blur.
    std::string prefix = ue::GetPathName(c.obj) + ".";
    std::unordered_map<std::string, int> kinds;
    std::vector<UObject> blurs;
    ue::objects::ForEach([&](int, UObject o) {
        if (ue::IsDefaultObject(o)) return;
        std::string path = ue::GetPathName(o);
        if (path.compare(0, prefix.size(), prefix) != 0) return;
        std::string cn = ue::GetName(ue::GetClass(o));
        ++kinds[cn];
        if (cn == "BackgroundBlur") blurs.push_back(o);
    });
    std::string list;
    for (const auto& k : kinds) list += " " + k.first + " x" + std::to_string(k.second) + ";";
    log::Info("world menu: %s is built from:%s", p.name.c_str(), list.c_str());
    for (UObject b : blurs) {
        ue::Call v(b, "SetVisibility");
        v.Set("InVisibility", kCollapsed);
        v.Invoke();
        log::Info("world menu: hid %s (a background blur)", ue::GetName(b).c_str());
    }
    return true;
}

void ReleaseKey() {
    if (!G.pt.key_down) return;
    G.pt.key_down = false;
    if (!Alive(G.wic, G.wic_index)) return;
    ue::Call c(G.wic, "ReleasePointerKey");
    c.Set("Key", Key{LeftMouse()});
    c.Invoke();
}

void Unhost(size_t i, const char* why) {
    Panel p = G.panels[i];
    G.panels.erase(G.panels.begin() + static_cast<long>(i));
    log::Info("world menu: %s %s, panel removed", p.name.c_str(), why);
    DestroyComponent(p.comp, p.comp_index);
}


// Takes menus the game just put on the flat screen onto the forearm.
void TakeNewMenus() {
    std::vector<Candidate> list;
    {
        std::lock_guard<std::mutex> lock(g_events_mutex);
        list.swap(g_new_menus);
    }
    for (const auto& c : list) {
        bool hosted = false;
        for (auto& p : G.panels) hosted = hosted || p.widget == c.obj;
        if (hosted || !Alive(c.obj, c.index)) continue;
        if (!CallBool(c.obj, "IsInViewport")) {
            log::Info("world menu: %s came up but isn't on the screen, left alone",
                      ue::GetName(c.cls).c_str());
            continue;
        }
        Host(c);
    }
}

// Lets go of menus the game took away (RemoveFromParent) or destroyed.
void DropClosedMenus() {
    std::vector<UObject> removed;
    {
        std::lock_guard<std::mutex> lock(g_events_mutex);
        removed.swap(g_removed);
    }
    for (size_t i = G.panels.size(); i-- > 0;) {
        Panel& p = G.panels[i];
        if (std::find(removed.begin(), removed.end(), p.widget) != removed.end()) {
            Unhost(i, "closed by the game");
            continue;
        }
        if (!Alive(p.widget, p.widget_index)) {
            Unhost(i, "was destroyed");
            continue;
        }
        if (!Alive(p.comp, p.comp_index)) {
            G.panels.erase(G.panels.begin() + static_cast<long>(i));
            continue;
        }
        uint8_t vis = Visibility(p.widget);
        if (vis != p.last_vis) {
            if (p.last_vis != 255) log::Info("world menu: %s visibility %d", p.name.c_str(), vis);
            p.last_vis = vis;
        }
    }
    if (G.panels.empty()) G.placed = false;
}

// ------------------------------------------------------------ pointer --

struct Hit {
    bool ok = false;
    double t = 0;
    FVector point;
};

Hit RayPanel(const FVector& o, const FVector& d, const Panel& p) {
    Hit h;
    FQuat rot = ToQuat(PanelRotation());
    FVector n = rot.Rotate(FVector{1, 0, 0});
    double denom = d.X * n.X + d.Y * n.Y + d.Z * n.Z;
    if (denom > -1e-4) return h;
    FVector c = PanelCentre(p);
    FVector oc = c - o;
    double t = (oc.X * n.X + oc.Y * n.Y + oc.Z * n.Z) / denom;
    if (t <= 0) return h;
    FVector at = o + d * t;
    FVector local = rot.Inverse().Rotate(at - c);
    double s = Scale();
    if (std::fabs(local.Y) > kDrawW * 0.5 * s || std::fabs(local.Z) > kDrawH * 0.5 * s) return h;
    h.ok = true;
    h.t = t;
    h.point = at;
    return h;
}

void UpdatePointer() {
    input::Snapshot in = input::Peek();
    Laser laser;
    laser.show = S.laser && in.valid;
    if (!in.valid || !Alive(G.wic, G.wic_index)) {
        std::lock_guard<std::mutex> lock(g_laser_mutex);
        g_laser = Laser{};
        return;
    }
    // The hand whose trigger was pulled last points.
    for (int hnd = 0; hnd < 2; ++hnd) {
        bool t = in.hand[hnd].trigger > (G.pt.trigger_down[hnd] ? 0.35f : 0.6f);
        if (t && !G.pt.trigger_down[hnd] && hnd != G.pt.hand && !(S.forearm && hnd == S.arm)) {
            ReleaseKey();
            G.pt.hand = hnd;
        }
        G.pt.trigger_down[hnd] = t;
    }
    if (S.forearm) G.pt.hand = 1 - S.arm;  // the hand holding the menu can't point at it
    const input::HandInput& h = in.hand[G.pt.hand];
    laser.hand = G.pt.hand;
    FVector o;
    FQuat q;
    Hit best;
    if (h.aim_valid && stereo::TrackingToWorld(h.aim, &o, &q)) {
        FVector d = q.Rotate(FVector{1, 0, 0});
        for (const auto& p : G.panels) {
            uint8_t vis = p.last_vis == 255 ? kVisible : p.last_vis;
            if (vis == kCollapsed || vis == kHidden) continue;
            Hit hit = RayPanel(o, d, p);
            if (hit.ok && (!best.ok || hit.t < best.t)) best = hit;
        }
        if (best.ok) {
            // The interaction component traces a few centimetres through the
            // panel we hit, so nothing else in the world gets in the way.
            double back = 0.03 * G.cm_per_m;
            FVector from = best.point - d * back;
            FRotator r{std::asin(std::clamp(d.Z, -1.0, 1.0)) * kRadToDeg, std::atan2(d.Y, d.X) * kRadToDeg, 0};
            SetWorldPose(G.wic, from, r);
            laser.hit = true;
            laser.length_m = static_cast<float>(best.t / G.cm_per_m);
        }
    }
    float want = best.ok ? static_cast<float>(0.06 * G.cm_per_m) : 0.f;
    if (want != G.last_distance) {
        ue::SetProp<float>(G.wic, "InteractionDistance", want);
        G.last_distance = want;
    }

    bool down = G.pt.trigger_down[G.pt.hand] && best.ok;
    if (down && !G.pt.key_down) {
        ue::Call c(G.wic, "PressPointerKey");
        c.Set("Key", Key{LeftMouse()});
        c.Invoke();
        G.pt.key_down = true;
        input::Haptic(static_cast<input::Hand>(G.pt.hand), 0.3f, 0.02f, 200.f);
    } else if (!G.pt.trigger_down[G.pt.hand] && G.pt.key_down) {
        ReleaseKey();
    }
    laser.pressed = G.pt.key_down;

    // A light tick when the laser moves onto something clickable.
    uint8_t over = 0;
    ue::GetProp(G.wic, "bIsHoveredWidgetInteractable", over);
    bool over_button = best.ok && over;
    UObject hovered = 0;
    ue::GetProp(G.wic, "HoveredWidgetComponent", hovered);
    static UObject s_logged_hover = 1;
    static int s_hover_logs = 0;
    if (hovered != s_logged_hover && s_hover_logs < 40) {
        ++s_hover_logs;
        s_logged_hover = hovered;
        log::Info("world menu: pointer %s a panel (laser %s)", hovered ? "is over" : "is not over",
                  best.ok ? "hits one" : "misses");
    }
    if (over_button && !G.pt.was_over_button)
        input::Haptic(static_cast<input::Hand>(G.pt.hand), 0.12f, 0.01f, 160.f);
    G.pt.was_over_button = over_button;

    // Thumbstick up/down scrolls, about 4 notches a second at full tilt.
    if (best.ok) {
        G.pt.scroll_accum += h.stick_y * 4.f / 90.f;
        if (std::fabs(h.stick_y) < 0.2f) G.pt.scroll_accum = 0.f;
        if (std::fabs(G.pt.scroll_accum) >= 1.f) {
            float notch = G.pt.scroll_accum > 0 ? 1.f : -1.f;
            G.pt.scroll_accum -= notch;
            ue::Call c(G.wic, "ScrollWheel");
            c.Set("ScrollDelta", notch);
            c.Invoke();
        }
    }
    std::lock_guard<std::mutex> lock(g_laser_mutex);
    g_laser = laser;
}

// --------------------------------------------------------- frame pump --

void OnGameFrame() {
    if (!S.enabled || !ue::reflection::Ready() || !ue::process_event::Ready()) return;
    // Once per frame: the controller's tick and the player's tick can both call.
    static uint64_t s_last_ms = 0;
    uint64_t ms = GetTickCount64();
    if (ms - s_last_ms < 4) return;
    s_last_ms = ms;
    double now = Now();
    TakeNewMenus();
    DropClosedMenus();
    g_menu_up = !G.panels.empty();
    if (G.panels.empty()) {
        g_active = false;
        std::lock_guard<std::mutex> lock(g_laser_mutex);
        g_laser = Laser{};
        return;
    }
    // How often the panels are updated while a menu is up, once a second.
    static int s_frames = 0;
    static double s_rate_at = 0;
    ++s_frames;
    if (now - s_rate_at >= 1.0) {
        if (s_rate_at > 0 && s_frames > 0)
            log::Info("world menu: %d updates/s, %zu panel(s) on the forearm", s_frames, G.panels.size());
        s_frames = 0;
        s_rate_at = now;
    }
    if (g_capture_at > 0 && now >= g_capture_at) {
        g_capture_at = -1;
        xr::RequestCapture(("forearm-" + g_capture_name).c_str());
    }
    g_active = !G.panels.empty();
    if (G.panels.empty()) return;

    FVector head;
    double yaw;
    if (S.forearm) {
        if (PlaceOnForearm())
            for (auto& p : G.panels) PosePanel(p);
    } else if (HeadInWorld(&head, &yaw)) {
        if (!G.placed) PlacePanels(head, yaw);
        else {
            double diff = std::fabs(NormalizeAxis(yaw - G.yaw));
            if (diff > S.follow_deg) PlacePanels(head, yaw);
        }
    }
    UpdatePointer();
}

// -------------------------------------------------------- laser layer --
// A thin beam from the controller to the panel and a dot where it lands,
// drawn by the headset compositor so they are always sharp and on top.

constexpr int kTexW = 128, kTexH = 64;  // left half: dot, right half: beam

struct LaserGpu {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D12KHR> images;
    bool uploaded = false;
    bool failed = false;
    XrCompositionLayerQuad beam{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad dot{XR_TYPE_COMPOSITION_LAYER_QUAD};
} LG;

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER t{};
    t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    t.Transition.pResource = r;
    t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    t.Transition.StateBefore = a;
    t.Transition.StateAfter = b;
    return t;
}

// Premultiplied white with soft edges.
std::vector<uint32_t> LaserPixels() {
    std::vector<uint32_t> px(kTexW * kTexH);
    for (int y = 0; y < kTexH; ++y) {
        for (int x = 0; x < kTexW; ++x) {
            float a;
            if (x < 64) {
                float dx = (x + 0.5f - 32.f) / 32.f, dy = (y + 0.5f - 32.f) / 32.f;
                float r = std::sqrt(dx * dx + dy * dy);
                a = r < 0.55f ? 1.f : r < 1.f ? (1.f - r) / 0.45f : 0.f;
            } else {
                float dx = std::fabs((x - 64 + 0.5f - 32.f) / 32.f);
                a = dx < 0.3f ? 0.85f : dx < 1.f ? 0.85f * (1.f - dx) / 0.7f : 0.f;
            }
            uint32_t v = static_cast<uint32_t>(std::clamp(a, 0.f, 1.f) * 255.f + 0.5f);
            px[y * kTexW + x] = (v << 24) | (v << 16) | (v << 8) | v;
        }
    }
    return px;
}

bool UploadLaser(XrSession session) {
    if (LG.failed) return false;
    ID3D12Device* dev = xr::Device();
    ID3D12CommandQueue* queue = dxgi_hooks::GameQueue();
    if (!dev || !queue) return false;
    auto enum_formats = Proc<PFN_xrEnumerateSwapchainFormats>("xrEnumerateSwapchainFormats");
    auto create = Proc<PFN_xrCreateSwapchain>("xrCreateSwapchain");
    auto enum_images = Proc<PFN_xrEnumerateSwapchainImages>("xrEnumerateSwapchainImages");
    auto acquire = Proc<PFN_xrAcquireSwapchainImage>("xrAcquireSwapchainImage");
    auto wait = Proc<PFN_xrWaitSwapchainImage>("xrWaitSwapchainImage");
    auto release = Proc<PFN_xrReleaseSwapchainImage>("xrReleaseSwapchainImage");
    if (!enum_formats || !create || !enum_images || !acquire || !wait || !release) return false;
    LG.failed = true;  // until it works
    uint32_t n = 0;
    enum_formats(session, 0, &n, nullptr);
    std::vector<int64_t> formats(n);
    enum_formats(session, n, &n, formats.data());
    auto has = [&](DXGI_FORMAT f) {
        return std::find(formats.begin(), formats.end(), static_cast<int64_t>(f)) != formats.end();
    };
    DXGI_FORMAT fmt = has(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
                      : has(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                                             : DXGI_FORMAT_UNKNOWN;
    if (fmt == DXGI_FORMAT_UNKNOWN) return false;
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = fmt;
    ci.sampleCount = 1;
    ci.width = kTexW;
    ci.height = kTexH;
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    if (XR_FAILED(create(session, &ci, &LG.swapchain))) {
        log::Error("world menu: laser swapchain failed");
        return false;
    }
    enum_images(LG.swapchain, 0, &n, nullptr);
    LG.images.assign(n, XrSwapchainImageD3D12KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    enum_images(LG.swapchain, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(LG.images.data()));

    std::vector<uint32_t> px = LaserPixels();
    if (fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
        for (auto& p : px) p = (p & 0xFF00FF00u) | ((p & 0xFF) << 16) | ((p >> 16) & 0xFF);
    }
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cmd = nullptr;
    ID3D12Fence* fence = nullptr;
    ID3D12Resource* upload = nullptr;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = kTexW * kTexH * 4;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bool ok = SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
              SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cmd))) &&
              SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) &&
              SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                     nullptr, IID_PPV_ARGS(&upload)));
    void* map = nullptr;
    D3D12_RANGE none{0, 0};
    if (ok && SUCCEEDED(upload->Map(0, &none, &map))) {
        std::memcpy(map, px.data(), px.size() * 4);
        upload->Unmap(0, nullptr);
    } else {
        ok = false;
    }
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (ok && XR_SUCCEEDED(acquire(LG.swapchain, &ai, &index))) {
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = 100000000;
        ok = XR_SUCCEEDED(wait(LG.swapchain, &wi));
        if (ok) {
            ID3D12Resource* dst = LG.images[index].texture;
            D3D12_RESOURCE_BARRIER b = Transition(dst, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->ResourceBarrier(1, &b);
            D3D12_TEXTURE_COPY_LOCATION to{};
            to.pResource = dst;
            to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION from{};
            from.pResource = upload;
            from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            from.PlacedFootprint.Footprint.Format = dst->GetDesc().Format;
            from.PlacedFootprint.Footprint.Width = kTexW;
            from.PlacedFootprint.Footprint.Height = kTexH;
            from.PlacedFootprint.Footprint.Depth = 1;
            from.PlacedFootprint.Footprint.RowPitch = kTexW * 4;
            cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
            cmd->ResourceBarrier(1, &b);
            ok = SUCCEEDED(cmd->Close());
            if (ok) {
                ID3D12CommandList* lists[] = {cmd};
                queue->ExecuteCommandLists(1, lists);
                queue->Signal(fence, 1);
                HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                fence->SetEventOnCompletion(1, ev);
                WaitForSingleObject(ev, 500);
                CloseHandle(ev);
            }
        }
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        release(LG.swapchain, &ri);
    } else {
        ok = false;
    }
    if (upload) upload->Release();
    if (fence) fence->Release();
    if (cmd) cmd->Release();
    if (alloc) alloc->Release();
    if (!ok) {
        log::Error("world menu: could not upload the laser image");
        return false;
    }
    LG.failed = false;
    LG.uploaded = true;
    return true;
}

struct V3 {
    float x, y, z;
};
V3 Add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 Mul(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 Norm(V3 a) {
    float l = std::sqrt(Dot(a, a));
    return l > 1e-6f ? Mul(a, 1.f / l) : V3{0, 0, 1};
}
V3 FromXr(XrVector3f v) { return {v.x, v.y, v.z}; }
V3 Rotate(XrQuaternionf q, V3 v) {
    V3 u{q.x, q.y, q.z};
    V3 t = Mul(Cross(u, v), 2.f);
    return Add(Add(v, Mul(t, q.w)), Cross(u, t));
}

// Rotation whose columns are the given orthonormal axes.
XrQuaternionf FromAxes(V3 x, V3 y, V3 z) {
    float tr = x.x + y.y + z.z;
    XrQuaternionf q;
    if (tr > 0) {
        float s = std::sqrt(tr + 1.f) * 2.f;
        q = {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, 0.25f * s};
    } else if (x.x > y.y && x.x > z.z) {
        float s = std::sqrt(1.f + x.x - y.y - z.z) * 2.f;
        q = {0.25f * s, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
    } else if (y.y > z.z) {
        float s = std::sqrt(1.f + y.y - x.x - z.z) * 2.f;
        q = {(y.x + x.y) / s, 0.25f * s, (z.y + y.z) / s, (z.x - x.z) / s};
    } else {
        float s = std::sqrt(1.f + z.z - x.x - y.y) * 2.f;
        q = {(z.x + x.z) / s, (z.y + y.z) / s, 0.25f * s, (x.y - y.x) / s};
    }
    return q;
}

struct LaserListener : xr::Listener {
    void AddLayers(XrSession session, XrSpace space, XrTime,
                   std::vector<const XrCompositionLayerBaseHeader*>& layers) override {
        Laser l;
        {
            std::lock_guard<std::mutex> lock(g_laser_mutex);
            l = g_laser;
        }
        if (!l.show || !g_active.load()) return;
        input::Snapshot in = input::Peek();
        if (!in.valid || !in.hand[l.hand].aim_valid) return;
        xr::EyeView eyes[2];
        if (!xr::LatestViews(eyes)) return;
        if (!LG.uploaded && !UploadLaser(session)) return;

        const XrPosef& aim = in.hand[l.hand].aim;
        V3 o = FromXr(aim.position);
        V3 d = Rotate(aim.orientation, V3{0, 0, -1});
        V3 head = Mul(Add(FromXr(eyes[0].pose.position), FromXr(eyes[1].pose.position)), 0.5f);
        float len = l.hit ? l.length_m : 0.35f;
        if (len < 0.02f) return;

        // Beam: quad +Y along the ray, its face turned toward the head.
        V3 mid = Add(o, Mul(d, len * 0.5f));
        V3 to_head = Sub(head, mid);
        V3 z = Norm(Sub(to_head, Mul(d, Dot(to_head, d))));
        V3 y = d;
        V3 x = Cross(y, z);
        LG.beam.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        LG.beam.space = space;
        LG.beam.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        LG.beam.subImage.swapchain = LG.swapchain;
        LG.beam.subImage.imageRect = {{64, 0}, {64, kTexH}};
        LG.beam.pose = {FromAxes(x, y, z), {mid.x, mid.y, mid.z}};
        LG.beam.size = {l.hit ? 0.004f : 0.003f, len};
        layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&LG.beam));

        if (!l.hit) return;
        // Dot: where the beam lands, facing the head.
        V3 end = Add(o, Mul(d, len - 0.005f));
        V3 dz = Norm(Sub(head, end));
        V3 up{0, 1, 0};
        V3 dx = Norm(Cross(up, dz));
        V3 dy = Cross(dz, dx);
        float size = (l.pressed ? 0.014f : 0.02f) * std::max(0.5f, len / 1.5f);
        LG.dot.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        LG.dot.space = space;
        LG.dot.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        LG.dot.subImage.swapchain = LG.swapchain;
        LG.dot.subImage.imageRect = {{0, 0}, {64, kTexH}};
        LG.dot.pose = {FromAxes(dx, dy, dz), {end.x, end.y, end.z}};
        LG.dot.size = {size, size};
        layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&LG.dot));
    }

    void OnSessionEnding() override {
        if (LG.swapchain)
            if (auto destroy = Proc<PFN_xrDestroySwapchain>("xrDestroySwapchain")) destroy(LG.swapchain);
        LG = LaserGpu{};
    }
} g_laser_listener;

}  // namespace

void Init() {
    std::string mode = config::GetString("menu_mode", "world");
    S.enabled = config::GetBool("menu_panel", true) && mode != "window";
    S.width_m = config::GetFloat("menu_world_width_m", 1.8f);
    S.distance_m = config::GetFloat("menu_world_distance_m", 1.6f);
    S.drop_m = config::GetFloat("menu_world_drop_m", 0.15f);
    S.follow_deg = config::GetFloat("menu_panel_follow_deg", 60.f);
    S.backdrop = std::clamp(config::GetFloat("menu_world_backdrop_v2", 0.f), 0.f, 1.f);
    S.forearm = config::GetString("menu_world_place", "forearm") != "front";
    S.arm = config::GetString("wrist_hud_hand", "left") == "right" ? input::kRight : input::kLeft;
    S.arm_width_m = config::GetFloat("menu_forearm_width_m", 0.30f);
    S.arm_along_m = config::GetFloat("menu_forearm_along_m", 0.20f);
    S.arm_back_m = config::GetFloat("menu_forearm_back_m", 0.05f);
    S.hide_covers = config::GetBool("menu_hide_focus_cover", true);
    S.laser = config::GetBool("menu_world_laser", true);
    if (!S.enabled) {
        log::Info("world menu: off (menu_mode=%s)", mode.c_str());
        return;
    }
    xr::RegisterListener(&g_laser_listener);
    if (S.forearm)
        log::Info("world menu: on the back of the %s forearm, %.2f m wide", S.arm == input::kLeft ? "left" : "right",
                  S.arm_width_m);
    else
        log::Info("world menu: floating in front, %.2f m wide at %.2f m", S.width_m, S.distance_m);
}

// The pump runs on the game thread from the player controller's tick, which
// keeps running while the game is paused (the 2026-11-02 study counted it at
// the frame rate during the pause menu), and from the player's tick until the
// controller is found. Nothing runs from inside the stereo view setup.
void OnPlayerTick(UObject, float) { OnGameFrame(); }
void AfterControllerTick(UObject, UObject, void*) { OnGameFrame(); }

// The game's Blueprints took a widget away (or we did, which is skipped).
void ExecRemoveFromParent(UObject context, void* stack, void* result) {
    if (!t_our_call && context) {
        std::lock_guard<std::mutex> lock(g_events_mutex);
        g_removed.push_back(context);
    }
    g_orig_remove(context, stack, result);
}

// A menu's Construct: the game just put it up.
void OnAnyEvent(UObject self, UObject function, void*) {
    if (!S.enabled || !self || !function) return;
    // Each widget class has its own Construct function object: remember
    // which function objects are a Construct.
    static std::mutex s_mutex;
    static std::unordered_map<UObject, bool> s_is_construct;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto it = s_is_construct.find(function);
        if (it == s_is_construct.end())
            it = s_is_construct.emplace(function, ue::GetName(function) == "Construct").first;
        if (!it->second) return;
    }
    UObject cls = ue::GetClass(self);
    if (!cls || !IsMenuClass(cls)) return;
    // Only menus that stand on their own; one built into another menu's
    // panels (UIM_YesNo inside Options) travels with that menu.
    UObject slot = 0;
    if (ue::GetProp(self, "Slot", slot) && slot) return;
    int index = IndexOf(self);
    log::Info("world menu: the game put up %s", ue::GetName(cls).c_str());
    std::lock_guard<std::mutex> lock(g_events_mutex);
    g_new_menus.push_back({self, index, cls});
}

void InitGame() {
    if (!S.enabled) return;
    game::AddTickListener(&OnPlayerTick);
    g_wbgc_class = ue::FindObject("Class /Script/UMG.WidgetBlueprintGeneratedClass");
    UObject uw = ue::FindObject("UserWidget /Script/UMG.Default__UserWidget");
    bool widgets = uw && ue::process_event::HookVtableOf(uw);
    ue::process_event::OnAny(&OnAnyEvent);
    UObject remove = ue::FindObject("Function /Script/UMG.Widget:RemoveFromParent");
    bool removal = remove && ue::process_event::HookNative(remove, &ExecRemoveFromParent, &g_orig_remove);
    if (widgets && removal)
        log::Info("world menu: following the game's menus (Construct and RemoveFromParent)");
    else
        log::Error("world menu: could not follow the game's menus (widgets %d, removal %d); menus stay as the game draws them",
                   widgets ? 1 : 0, removal ? 1 : 0);
}

// Background thread: finds the player controller and its camera manager and
// follows their per-frame Blueprint events. Both kept firing at the frame rate
// during the pause menu in the 2026-11-02 study (ReceiveTick and
// BlueprintUpdateCamera). Only a class's own override is followed, so we
// never take over a function object other code shares.
void FollowFrameEvent(const char* base_class, const char* fn_name, UObject* hooked_cls) {
    UObject base = ue::FindObject(base_class);
    if (!base) return;
    UObject obj = ue::FindFirstInstance(base);
    if (!obj) return;
    UObject cls = ue::GetClass(obj);
    if (cls == *hooked_cls) return;
    *hooked_cls = cls;
    UObject fn = ue::FindFunction(cls, fn_name);
    if (!fn || ue::GetOuter(fn) != cls) {
        log::Info("world menu: %s has no %s of its own", ue::GetName(cls).c_str(), fn_name);
        return;
    }
    if (ue::process_event::HookVtableOf(obj) && ue::process_event::On(fn, nullptr, &AfterControllerTick))
        log::Info("world menu: driven by %s.%s (runs while paused)", ue::GetName(cls).c_str(), fn_name);
    else
        log::Warn("world menu: could not follow %s.%s", ue::GetName(cls).c_str(), fn_name);
}

void Scan() {
    if (!S.enabled || !ue::objects::Ready() || !ue::process_event::Ready()) return;
    static int s_calls = 0;
    if ((s_calls++ % 4) != 0) return;
    static UObject s_pc = 0, s_cam = 0;
    FollowFrameEvent("Class /Script/Engine.PlayerController", "ReceiveTick", &s_pc);
    FollowFrameEvent("Class /Script/Engine.PlayerCameraManager", "BlueprintUpdateCamera", &s_cam);
}

bool MenuUp() { return g_menu_up.load(); }
bool Active() { return g_active.load(); }
bool Enabled() { return S.enabled; }

}  // namespace holevr::vr::world_menu
