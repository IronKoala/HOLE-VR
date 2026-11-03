#include "vr/world_menu.h"

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "core/perf.h"
#include "game/game.h"
#include "ue/call.h"
#include "ue/names.h"
#include "ue/objects.h"
#include "ue/process_event.h"
#include "ue/reflection.h"
#include "vr/cvars.h"
#include "vr/dxgi_hooks.h"
#include "vr/input.h"
#include "vr/math.h"
#include "vr/openxr.h"
#include "vr/render_fixes.h"
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
    float arm_size = 1.25f;       // owner 2026-11-02: menus 25% bigger than 0.30 m
    float arm_along_m = 0.20f;    // from the controller grip toward the elbow
    float arm_back_m = 0.05f;     // out from the back of the forearm
    float arm_roll_deg = 0.f;     // extra turn of the menu on the arm
    bool hide_covers = true;      // the menus' full-screen click-catcher
    bool laser = true;
    bool stack = true;            // a sub-menu takes the place of the menu under it
    bool hide_backdrops = true;   // see-through grey boxes inside the menus (HideBackdrops)
    bool own_hit = true;          // the laser tells the menu where it hits (no game collision check)
    bool arrive_buzz = true;      // buzz the menu hand when the game puts a menu on the arm
    bool fit = true;              // zoom each menu so its buttons fill the space on the arm
    float fit_max_zoom = 3.f;
    bool on_top = true;           // menus drawn over everything (no depth test)
    int black_box = 1;            // kBoxGrow / kBoxHide / kBoxOff (KeepBoxesAway)
} S;
constexpr int kBoxOff = 0, kBoxGrow = 1, kBoxHide = 2;

// The game's menus are laid out for a 1920x1080 screen.
constexpr int32_t kDrawW = 1920, kDrawH = 1080;

// ---------------------------------------------------- engine constants --

constexpr uint32_t RF_ClassDefaultObject = 0x10;
constexpr uint32_t RF_BeginDestroyed = 0x8000;
constexpr uint32_t RF_FinishDestroyed = 0x10000;

constexpr uint8_t kSpaceWorld = 0;          // EWidgetSpace::World
constexpr uint8_t kBlendTransparent = 2;    // EWidgetBlendMode::Transparent
constexpr uint8_t kSourceWorld = 0;         // EWidgetInteractionSource::World
constexpr uint8_t kSourceCustom = 3;        // EWidgetInteractionSource::Custom
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
    bool late = false;  // caught when it took focus, not when it was built
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
    bool shown = true;         // false while a sub-menu covers it
    // FitContent: how much the menu is zoomed on the arm, and the part of the
    // 1920x1080 layout that holds its buttons (centre and half size), which
    // sits at the panel's spot on the arm and is where the laser can hit.
    double zoom = 1.0;
    double cx = kDrawW * 0.5, cy = kDrawH * 0.5;
    double half_w = kDrawW * 0.5, half_h = kDrawH * 0.5;
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

// Engine objects looked up by name. A lookup by name walks every object in
// the game, which took over a second with a level loaded and froze the game
// when it ran as the pause menu opened (2026-11-02 log: 2.5 s stall). So all
// of them are found once, on the start-up thread, in ResolveLookups().
struct Lookups {
    UObject ksl = 0;            // KismetSystemLibrary default object
    UObject statics = 0;        // GameplayStatics default object
    UObject actor_class = 0;
    UObject widget_comp_class = 0;
    UObject wic_class = 0;      // WidgetInteractionComponent
    UObject panel_class = 0;    // PanelWidget: has child widgets
    UObject user_widget_class = 0;
    UObject blur_class = 0;     // BackgroundBlur
    UObject image_class = 0;    // Image and Border: where a menu's see-through
    UObject border_class = 0;   // grey backdrop box lives (HideBackdrops)
    // Byte offsets inside them, from the engine's own property data.
    int32_t image_brush = -1, image_color = -1, border_brush = -1, border_color = -1;
    int32_t brush_tint = -1, brush_resource = -1, brush_draw_as = -1, slate_color = -1;
    int32_t render_opacity = -1;
    // FitContent: CanvasPanel children's layout (CanvasPanelSlot.LayoutData).
    UObject canvas_class = 0, canvas_slot_class = 0;
    int32_t slot_offsets = -1, slot_anchor_min = -1, slot_anchor_max = -1, slot_alignment = -1;
    int32_t slot_autosize = -1;
    uint8_t slot_autosize_mask = 0;
    // DrawOnTop: UMaterial.bDisableDepthTest, from the engine's property data.
    UObject material_class = 0;
    int32_t depth_off = -1;
    uint8_t depth_off_mask = 0;
    FName ui_profile;           // collision profile "UI"
    bool have_ui_profile = false;
    FName left_mouse;           // key name "LeftMouseButton", for the laser
} L;

UObject Lookup(const char* full_name) {
    UObject o = ue::FindObject(full_name);
    if (!o) log::Warn("world menu: %s not found", full_name);
    return o;
}

// Offsets for HideBackdrops: Image.Brush / Image.ColorAndOpacity,
// Border.Background / Border.BrushColor, and inside FSlateBrush its
// TintColor (an FSlateColor: SpecifiedColor), ResourceObject and DrawAs.
void ResolveBackdropOffsets() {
    namespace rf = ue::reflection;
    UObject widget = ue::FindObject("Class /Script/UMG.Widget");
    if (!L.image_class || !L.border_class || !widget) return;
    rf::PropertyInfo ib = rf::FindProperty(L.image_class, "Brush");
    rf::PropertyInfo ic = rf::FindProperty(L.image_class, "ColorAndOpacity");
    rf::PropertyInfo bb = rf::FindProperty(L.border_class, "Background");
    rf::PropertyInfo bc = rf::FindProperty(L.border_class, "BrushColor");
    rf::PropertyInfo ro = rf::FindProperty(widget, "RenderOpacity");
    if (!ib.valid() || !ib.inner || !ic.valid() || !bb.valid() || !bc.valid() || !ro.valid()) {
        log::Warn("world menu: Image/Border layout not found; menu backdrops stay");
        return;
    }
    rf::PropertyInfo tint = rf::FindProperty(ib.inner, "TintColor");
    rf::PropertyInfo res = rf::FindProperty(ib.inner, "ResourceObject");
    rf::PropertyInfo draw = rf::FindProperty(ib.inner, "DrawAs");
    rf::PropertyInfo spec = tint.valid() && tint.inner ? rf::FindProperty(tint.inner, "SpecifiedColor") : rf::PropertyInfo{};
    if (!tint.valid() || !res.valid() || !draw.valid() || !spec.valid()) {
        log::Warn("world menu: SlateBrush layout not found; menu backdrops stay");
        return;
    }
    L.image_brush = ib.offset;
    L.image_color = ic.offset;
    L.border_brush = bb.offset;
    L.border_color = bc.offset;
    L.brush_tint = tint.offset;
    L.brush_resource = res.offset;
    L.brush_draw_as = draw.offset;
    L.slate_color = spec.offset;
    L.render_opacity = ro.offset;
}

// EWidgetInteractionSource::Custom, read from the engine's enum: UEnum keeps
// its TArray<TPair<FName, int64>> Names right after UField (0x30) and the
// CppType string, at 0x40 in UE 5.5. Checked by name; 3 (its value in every
// UE5 release) if the layout doesn't match.
uint8_t g_source_custom = kSourceCustom;
void ResolveCustomSource() {
    UObject e = ue::FindObject("Enum /Script/UMG.EWidgetInteractionSource");
    struct Arr {
        uintptr_t data;
        int32_t num, max;
    };
    Arr a = e ? mem::ReadOr<Arr>(e + 0x40, Arr{}) : Arr{};
    if (a.data && a.num > 0 && a.num <= 16 && a.max >= a.num) {
        for (int i = 0; i < a.num; ++i) {
            FName n = mem::ReadOr<FName>(a.data + i * 16, FName{});
            int64_t v = mem::ReadOr<int64_t>(a.data + i * 16 + 8, -1);
            std::string s = ue::names::ToString(n);
            if (s == "EWidgetInteractionSource::Custom" && v >= 0 && v < 256) {
                g_source_custom = static_cast<uint8_t>(v);
                log::Info("world menu: pointer source Custom = %d (from the engine)", static_cast<int>(v));
                return;
            }
        }
    }
    log::Warn("world menu: EWidgetInteractionSource not readable; using Custom = %d", kSourceCustom);
}

// CanvasPanelSlot.LayoutData (FAnchorData): Offsets (FMargin: Left, Top,
// Right, Bottom floats), Anchors (Minimum, Maximum: FVector2D, doubles in
// UE5) and Alignment (FVector2D), plus bAutoSize.
void ResolveFitOffsets() {
    namespace rf = ue::reflection;
    L.canvas_class = Lookup("Class /Script/UMG.CanvasPanel");
    L.canvas_slot_class = Lookup("Class /Script/UMG.CanvasPanelSlot");
    if (!L.canvas_class || !L.canvas_slot_class) return;
    rf::PropertyInfo layout = rf::FindProperty(L.canvas_slot_class, "LayoutData");
    rf::PropertyInfo autosize = rf::FindProperty(L.canvas_slot_class, "bAutoSize");
    if (!layout.valid() || !layout.inner || !autosize.valid()) {
        log::Warn("world menu: CanvasPanelSlot layout not found; menus are not zoomed to fit");
        return;
    }
    rf::PropertyInfo offsets = rf::FindProperty(layout.inner, "Offsets");
    rf::PropertyInfo anchors = rf::FindProperty(layout.inner, "Anchors");
    rf::PropertyInfo align = rf::FindProperty(layout.inner, "Alignment");
    rf::PropertyInfo amin = anchors.valid() && anchors.inner ? rf::FindProperty(anchors.inner, "Minimum") : rf::PropertyInfo{};
    rf::PropertyInfo amax = anchors.valid() && anchors.inner ? rf::FindProperty(anchors.inner, "Maximum") : rf::PropertyInfo{};
    // Only the layout this code reads: four floats, and doubles for the vectors.
    if (!offsets.valid() || offsets.element_size != 16 || !align.valid() || align.element_size != 16 ||
        !amin.valid() || amin.element_size != 16 || !amax.valid() || amax.element_size != 16) {
        log::Warn("world menu: AnchorData layout is not the one expected; menus are not zoomed to fit");
        return;
    }
    L.slot_offsets = layout.offset + offsets.offset;
    L.slot_anchor_min = layout.offset + anchors.offset + amin.offset;
    L.slot_anchor_max = layout.offset + anchors.offset + amax.offset;
    L.slot_alignment = layout.offset + align.offset;
    L.slot_autosize = autosize.offset;
    L.slot_autosize_mask = autosize.bool_mask ? autosize.bool_mask : 0xFF;
}

void ResolveLookups() {
    ResolveCustomSource();
    L.ksl = Lookup("KismetSystemLibrary /Script/Engine.Default__KismetSystemLibrary");
    L.statics = Lookup("GameplayStatics /Script/Engine.Default__GameplayStatics");
    L.actor_class = Lookup("Class /Script/Engine.Actor");
    L.widget_comp_class = Lookup("Class /Script/UMG.WidgetComponent");
    L.wic_class = Lookup("Class /Script/UMG.WidgetInteractionComponent");
    L.panel_class = Lookup("Class /Script/UMG.PanelWidget");
    L.user_widget_class = Lookup("Class /Script/UMG.UserWidget");
    L.blur_class = Lookup("Class /Script/UMG.BackgroundBlur");
    L.image_class = Lookup("Class /Script/UMG.Image");
    L.border_class = Lookup("Class /Script/UMG.Border");
    ResolveBackdropOffsets();
    ResolveFitOffsets();
    L.material_class = Lookup("Class /Script/Engine.Material");
    if (L.material_class) {
        auto dd = ue::reflection::FindProperty(L.material_class, "bDisableDepthTest");
        if (dd.valid() && dd.type == "BoolProperty" && dd.element_size == 1) {
            L.depth_off = dd.offset;
            L.depth_off_mask = dd.bool_mask ? dd.bool_mask : 0xFF;
        } else {
            log::Warn("world menu: Material.bDisableDepthTest not found; menus can't be drawn on top");
        }
    }
    L.have_ui_profile = ue::names::Find("UI", &L.ui_profile);
    if (!ue::names::Find("LeftMouseButton", &L.left_mouse))
        log::Warn("world menu: name \"LeftMouseButton\" not found");
}

UObject Ksl() { return L.ksl; }

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

FName LeftMouse() { return L.left_mouse; }

// ---------------------------------------------------------- the actor --

UObject SpawnHost(UObject world_context) {
    UObject statics = L.statics, actor_class = L.actor_class;
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

UObject AddComponent(UObject actor, UObject cls, bool deferred) {
    if (!cls) return 0;
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
    UObject wic = AddComponent(G.actor, L.wic_class, true);
    if (!wic) {
        log::Error("world menu: could not create the pointer");
        return false;
    }
    // Custom: the laser hands the pointer its hit itself (SendHit), so the
    // game's collision along the beam never comes into it. With World the
    // pointer traced the game's Visibility channel and stopped at the first
    // thing in the way: in the pit under the microwave (area select menu,
    // owner's 2026-11-02 13:52 run) it never once reached the menu.
    ue::SetProp<uint8_t>(wic, "InteractionSource", S.own_hit ? g_source_custom : kSourceWorld);
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

double PanelWidthM() { return S.forearm ? S.arm_width_m * S.arm_size : S.width_m; }
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
    // Grip axes in UE: +X through the fist, +Z along the thumb. +Y points out
    // of the PALM of the left hand: with +1 here the owner saw the menu only
    // when turning the palm up (2026-11-02 run), so the back of the hand is -Y.
    double side = S.arm == input::kLeft ? -1.0 : 1.0;
    FVector back = q.Rotate(FVector{0, side, 0});
    FVector thumb = q.Rotate(FVector{0, 0, 1});
    xr::EyeView eyes[2];
    if (xr::LatestViews(eyes)) {
        FQuat hq;
        if (stereo::TrackingToWorld(eyes[0].pose, nullptr, &hq)) {
            FVector head_up = hq.Rotate(FVector{0, 0, 1});
            if (DotV(thumb, head_up) * G.up_sign < -0.25) G.up_sign = -G.up_sign;
        }
    }
    FVector up0 = thumb * G.up_sign;
    FVector y0 = Cross(up0, back);  // widget's left edge
    // Where the old layout put the menu (off the side of the hand), and where
    // it belongs: along the forearm toward the elbow, i.e. backwards along the
    // controller's pointing direction, flattened onto the back of the arm.
    // The whole layout (position and the menu's turn) is swung around the
    // wrist from the first to the second, which is what the owner asked for
    // (2026-11-02: "rotate the whole wrist menu ... so the pause menu is on
    // the forearm").
    FVector old_dir = q.Rotate(FVector{-1, 0, 0});
    FVector elbow_dir = old_dir;
    const input::HandInput& hi = in.hand[S.arm];
    FQuat aq;
    if (hi.aim_valid && stereo::TrackingToWorld(hi.aim, nullptr, &aq)) {
        FVector fwd = aq.Rotate(FVector{1, 0, 0});
        FVector e = fwd * -1.0;
        e = e - back * DotV(e, back);
        double len = std::sqrt(DotV(e, e));
        if (len > 0.2) elbow_dir = e * (1.0 / len);
    }
    old_dir = old_dir - back * DotV(old_dir, back);
    double ol = std::sqrt(DotV(old_dir, old_dir));
    if (ol > 1e-3) old_dir = old_dir * (1.0 / ol);
    double c = DotV(old_dir, elbow_dir);
    double sn = DotV(Cross(old_dir, back), elbow_dir);
    // Same turn applied to the menu's axes (in the plane of the back of the arm).
    FVector up1 = up0 * c + Cross(up0, back) * sn;
    FVector y1 = y0 * c + Cross(y0, back) * sn;
    // An extra turn of the menu on its own, if wanted (menu_forearm_roll_deg).
    double r = S.arm_roll_deg * kDegToRad;
    FVector up = up1 * std::cos(r) + y1 * std::sin(r);
    FVector y = y1 * std::cos(r) - up1 * std::sin(r);
    G.centre = loc + back * (S.arm_back_m * G.cm_per_m) + elbow_dir * (S.arm_along_m * G.cm_per_m);
    G.rot = FromAxes(back, y, up);
    G.placed = true;
    return true;
}

void PosePanel(const Panel& p) {
    if (!Alive(p.comp, p.comp_index)) return;
    double s = Scale() * p.zoom;
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

// ---------------------------------------------------- widget tree walk --

bool IsA(UObject obj, UObject base) {
    UObject cls = ue::GetClass(obj);
    return cls && base && ue::reflection::IsChildOf(cls, base);
}

// Walks a menu's widget tree (and the trees of the user widgets inside it,
// e.g. its buttons) and collects any BackgroundBlur. Returns how many widgets
// it looked at. Uses the engine's own PanelWidget.GetChildAt, so it only
// touches the menu's widgets.
int FindBlurs(UObject menu, std::vector<UObject>* blurs) {
    if (!L.panel_class || !L.user_widget_class) return 0;
    int seen = 0;
    std::vector<std::pair<UObject, int>> todo;  // widget, depth
    auto push_tree = [&](UObject user_widget, int depth) {
        UObject tree = 0, root = 0;
        if (ue::GetProp(user_widget, "WidgetTree", tree) && tree && ue::GetProp(tree, "RootWidget", root) && root)
            todo.push_back({root, depth});
    };
    push_tree(menu, 0);
    while (!todo.empty() && seen < 2000) {
        auto [w, depth] = todo.back();
        todo.pop_back();
        ++seen;
        if (L.blur_class && IsA(w, L.blur_class)) blurs->push_back(w);
        if (depth >= 32) continue;
        if (IsA(w, L.user_widget_class)) {
            push_tree(w, depth + 1);
        } else if (IsA(w, L.panel_class)) {
            ue::Call count(w, "GetChildrenCount");
            int n = count.Invoke() ? count.Result<int32_t>() : 0;
            for (int i = 0; i < n && i < 256; ++i) {
                ue::Call child(w, "GetChildAt");
                child.Set("Index", static_cast<int32_t>(i));
                if (child.Invoke() && child.Result<UObject>()) todo.push_back({child.Result<UObject>(), depth + 1});
            }
        }
    }
    return seen;
}

// ------------------------------------------------------------ backdrops --
// Some menus draw a see-through grey box behind their buttons (Options does:
// owner 2026-11-02 "i can see a grey shadowy box around them"). On a flat
// screen that dims the game behind the menu; on the arm it reads as a box,
// which the owner never wants. Such a box is an Image (or a Border's
// background) that is see-through and has no picture, just a colour. Only
// the menu's own layout and its frame parts are looked at: never the insides
// of buttons, sliders, tick boxes or the option tabs and task cards, whose
// own dark bars keep their text readable.

std::string Lower(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

bool HasAny(const std::string& lower, std::initializer_list<const char*> words) {
    for (const char* w : words)
        if (lower.find(w) != std::string::npos) return true;
    return false;
}

struct BrushLook {
    float alpha = 1.f;       // how see-through it is drawn (1 = solid)
    bool picture = false;    // has a texture or material
    bool drawn = true;       // DrawAs isn't NoDrawType
};

BrushLook ReadBrush(UObject w, int32_t brush, int32_t color) {
    BrushLook b;
    uintptr_t base = static_cast<uintptr_t>(w);
    LinearColor tint = mem::ReadOr<LinearColor>(base + brush + L.brush_tint + L.slate_color, LinearColor{1, 1, 1, 1});
    LinearColor col = mem::ReadOr<LinearColor>(base + color, LinearColor{1, 1, 1, 1});
    float opacity = mem::ReadOr<float>(base + L.render_opacity, 1.f);
    b.alpha = tint.a * col.a * opacity;
    b.picture = mem::ReadOr<UObject>(base + brush + L.brush_resource, 0) != 0;
    b.drawn = mem::ReadOr<uint8_t>(base + brush + L.brush_draw_as, 1) != 0;
    return b;
}

// Returns how many boxes it hid.
int HideBackdrops(UObject menu, const std::string& menu_name) {
    if (!S.hide_backdrops || L.image_brush < 0 || !L.panel_class || !L.user_widget_class) return 0;
    int hidden = 0, logged = 0, seen = 0;
    std::vector<std::pair<UObject, int>> todo;  // widget, user-widget depth
    auto push_tree = [&](UObject user_widget, int depth) {
        UObject tree = 0, root = 0;
        if (ue::GetProp(user_widget, "WidgetTree", tree) && tree && ue::GetProp(tree, "RootWidget", root) && root)
            todo.push_back({root, depth});
    };
    push_tree(menu, 0);
    while (!todo.empty() && seen < 1000) {
        auto [w, depth] = todo.back();
        todo.pop_back();
        ++seen;
        uint8_t vis = Visibility(w);
        if (vis == kCollapsed || vis == kHidden) continue;
        std::string name = ue::GetName(w);
        bool image = IsA(w, L.image_class), border = !image && IsA(w, L.border_class);
        if (image || border) {
            BrushLook b = image ? ReadBrush(w, L.image_brush, L.image_color) : ReadBrush(w, L.border_brush, L.border_color);
            std::string lo = Lower(name);
            bool named = HasAny(lo, {"shadow", "backdrop", "background", "bg", "dim", "fade"});
            bool see_through = b.alpha > 0.02f && b.alpha < 0.9f;
            bool backdrop = b.drawn && ((see_through && !b.picture) || (named && b.alpha > 0.02f));
            if (backdrop && image) {
                ue::Call v(w, "SetVisibility");
                v.Set("InVisibility", kHidden);
                v.Invoke();
                ++hidden;
            } else if (backdrop && border) {
                // A border holds the menu's content: only its own fill goes.
                ue::Call c(w, "SetBrushColor");
                c.Set("InBrushColor", LinearColor{0, 0, 0, 0});
                c.Invoke();
                ++hidden;
            }
            if (logged < 40) {
                ++logged;
                log::Info("world menu: %s: %s %s, %s, drawn %.2f opaque -> %s", menu_name.c_str(),
                          image ? "image" : "border", name.c_str(), b.picture ? "picture" : "plain colour",
                          b.alpha, backdrop ? "backdrop hidden" : "kept");
            }
        }
        if (IsA(w, L.user_widget_class)) {
            if (depth >= 2) continue;
            // The menu's frame parts only; skip controls, tab pages and cards.
            std::string cls = Lower(ue::GetName(ue::GetClass(w)));
            if (cls.rfind("uis_", 0) == 0 ||
                HasAny(cls, {"btn", "button", "che", "chb", "cmb", "sld", "key", "yesno", "card", "tab"}))
                continue;
            push_tree(w, depth + 1);
        } else if (IsA(w, L.panel_class)) {  // Border is a panel too (one child)
            ue::Call count(w, "GetChildrenCount");
            int n = count.Invoke() ? count.Result<int32_t>() : 0;
            for (int i = 0; i < n && i < 256; ++i) {
                ue::Call child(w, "GetChildAt");
                child.Set("Index", static_cast<int32_t>(i));
                if (child.Invoke() && child.Result<UObject>()) todo.push_back({child.Result<UObject>(), depth});
            }
        }
    }
    return hidden;
}

// ------------------------------------------------------------ fitting --
// The game lays its menus out for a 1920x1080 screen, mostly empty: the pause
// menu is a narrow column in the middle. Shrinking the whole screen onto the
// arm left the text tiny (owner 2026-11-02: "the menu is big enough but its
// still hard to read the text ... larger text but they dont overlap, and they
// still fit the individual boxes"). So each menu is measured from its own
// layout (where the game's CanvasPanel puts each part) and zoomed until the
// part holding its buttons fills the space the whole screen used to take on
// the arm. Text and boxes grow together, so nothing overlaps or spills out.

struct Rect {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    double W() const { return x1 - x0; }
    double H() const { return y1 - y0; }
};

Vector2D DesiredSize(UObject w) {
    ue::Call c(w, "GetDesiredSize");
    return c.Invoke() ? c.Result<Vector2D>() : Vector2D{0, 0};
}

// Where a CanvasPanel child sits inside the canvas's own rectangle.
bool SlotRect(UObject child, UObject slot, const Rect& parent, Rect* out) {
    uintptr_t b = static_cast<uintptr_t>(slot);
    struct Margin {
        float l, t, r, b;
    };
    Margin m = mem::ReadOr<Margin>(b + L.slot_offsets, Margin{0, 0, 0, 0});
    Vector2D amin = mem::ReadOr<Vector2D>(b + L.slot_anchor_min, Vector2D{0, 0});
    Vector2D amax = mem::ReadOr<Vector2D>(b + L.slot_anchor_max, Vector2D{0, 0});
    Vector2D al = mem::ReadOr<Vector2D>(b + L.slot_alignment, Vector2D{0, 0});
    bool autosize = (mem::ReadOr<uint8_t>(b + L.slot_autosize, 0) & L.slot_autosize_mask) != 0;
    auto sane = [](double v) { return std::isfinite(v) && v > -10.0 && v < 10.0; };
    if (!sane(amin.x) || !sane(amin.y) || !sane(amax.x) || !sane(amax.y) || !sane(al.x) || !sane(al.y)) return false;
    Vector2D want = autosize ? DesiredSize(child) : Vector2D{m.r, m.b};
    auto axis = [&](double lo, double size, double a0, double a1, float o0, float o1, double align, double desired,
                    double* r0, double* r1) {
        if (std::fabs(a1 - a0) < 1e-6) {  // anchored at a point: the offsets are position and size
            double start = lo + a0 * size + o0 - align * desired;
            *r0 = start;
            *r1 = start + desired;
        } else {  // stretched between two anchors: the offsets are margins
            *r0 = lo + a0 * size + o0;
            *r1 = lo + a1 * size - o1;
        }
    };
    axis(parent.x0, parent.W(), amin.x, amax.x, m.l, m.r, al.x, want.x, &out->x0, &out->x1);
    axis(parent.y0, parent.H(), amin.y, amax.y, m.t, m.b, al.y, want.y, &out->y0, &out->y1);
    return std::isfinite(out->x0) && std::isfinite(out->x1) && std::isfinite(out->y0) && std::isfinite(out->y1);
}

bool Covers(const Rect& r, const Rect& screen) {
    return r.W() >= screen.W() * 0.9 && r.H() >= screen.H() * 0.9;
}

// Adds the parts of `w` (laid out in `r`) that show something to `box`.
void Measure(UObject w, const Rect& r, const Rect& screen, int depth, Rect* box, bool* any, int* seen) {
    if (!w || ++*seen > 600 || depth > 12) return;
    uint8_t vis = Visibility(w);
    if (vis == kCollapsed || vis == kHidden) return;
    if (IsA(w, L.user_widget_class)) {
        // A user widget that fills its spot with its own canvas: look inside.
        UObject tree = 0, root = 0;
        if (ue::GetProp(w, "WidgetTree", tree) && tree && ue::GetProp(tree, "RootWidget", root) && root &&
            IsA(root, L.canvas_class)) {
            Measure(root, r, screen, depth + 1, box, any, seen);
            return;
        }
    } else if (IsA(w, L.canvas_class)) {
        ue::Call count(w, "GetChildrenCount");
        int n = count.Invoke() ? count.Result<int32_t>() : 0;
        for (int i = 0; i < n && i < 128; ++i) {
            ue::Call at(w, "GetChildAt");
            at.Set("Index", static_cast<int32_t>(i));
            UObject child = at.Invoke() ? at.Result<UObject>() : 0;
            UObject slot = 0;
            if (!child || !ue::GetProp(child, "Slot", slot) || !slot || !IsA(slot, L.canvas_slot_class)) continue;
            Rect cr;
            if (SlotRect(child, slot, r, &cr)) Measure(child, cr, screen, depth + 1, box, any, seen);
        }
        return;
    }
    Rect use = r;
    if (Covers(r, screen)) {
        // Something stretched over the whole screen (an overlay holding the
        // real content in its middle): count its content's size, centred.
        Vector2D d = DesiredSize(w);
        if (d.x <= 1 || d.y <= 1 || d.x >= screen.W() * 0.9) return;
        double mx = (r.x0 + r.x1) * 0.5, my = (r.y0 + r.y1) * 0.5;
        use = {mx - d.x * 0.5, my - d.y * 0.5, mx + d.x * 0.5, my + d.y * 0.5};
    }
    if (use.W() <= 1 || use.H() <= 1) return;
    if (!*any) *box = use;
    else *box = {std::min(box->x0, use.x0), std::min(box->y0, use.y0), std::max(box->x1, use.x1),
                 std::max(box->y1, use.y1)};
    *any = true;
}

void FitContent(Panel& p) {
    if (!S.fit || L.slot_offsets < 0 || !L.canvas_class || !L.user_widget_class) return;
    CallVoid(p.widget, "ForceLayoutPrepass");  // so the desired sizes are known
    Rect screen{0, 0, static_cast<double>(kDrawW), static_cast<double>(kDrawH)};
    Rect box;
    bool any = false;
    int seen = 0;
    Measure(p.widget, screen, screen, 0, &box, &any, &seen);
    if (!any) {
        log::Info("world menu: %s: couldn't measure its layout, shown whole (%d widgets looked at)", p.name.c_str(), seen);
        return;
    }
    // Clamp to the screen and keep a small border so nothing touches the edge.
    box = {std::max(0.0, box.x0 - 24), std::max(0.0, box.y0 - 24), std::min(screen.x1, box.x1 + 24),
           std::min(screen.y1, box.y1 + 24)};
    if (box.W() < 80 || box.H() < 40) {
        log::Info("world menu: %s: measured %.0fx%.0f, too small to trust; shown whole", p.name.c_str(), box.W(), box.H());
        return;
    }
    double zoom = std::min(screen.W() / box.W(), screen.H() / box.H());
    zoom = std::clamp(zoom, 1.0, static_cast<double>(S.fit_max_zoom));
    p.zoom = zoom;
    p.cx = (box.x0 + box.x1) * 0.5;
    p.cy = (box.y0 + box.y1) * 0.5;
    p.half_w = box.W() * 0.5;
    p.half_h = box.H() * 0.5;
    // The panel turns and sits around this point instead of the screen centre.
    ue::Call pivot(p.comp, "SetPivot");
    pivot.Set("InPivot", Vector2D{p.cx / kDrawW, p.cy / kDrawH});
    pivot.Invoke();
    log::Info("world menu: %s: buttons fill %.0f,%.0f to %.0f,%.0f of the screen; zoomed %.2fx on the arm",
              p.name.c_str(), box.x0, box.y0, box.x1, box.y1, zoom);
}

// ------------------------------------------------------------ hosting --

// ------------------------------------------------ menus drawn on top --
// A forearm menu is a panel in the game world, so anything nearer the eye
// covers it: a wall your arm goes into, your own body, or the black box the
// game puts around your head in the pit after the microwave fall (owner,
// 2026-11-03: "i need to be able to see the menu regardless of where my
// forearm is"). The panel's material (the engine's widget material) gets
// the engine's own "Disable Depth Test" switch while a menu is on the arm,
// which draws it over everything, and it is switched back when the last menu
// goes. Set before the panel starts, so the panel is made with it.
struct OnTop {
    UObject mat = 0;
    int index = -1;
    bool was = false;  // the switch before we touched it
    bool set = false;
    int lines = 0;
} OT;

UObject BaseMaterial(UObject mi) {
    if (!mi || !L.material_class) return 0;
    if (IsA(mi, L.material_class)) return mi;
    ue::Call g(mi, "GetBaseMaterial");
    UObject base = g.Invoke() ? g.Result<UObject>() : 0;
    return base && IsA(base, L.material_class) ? base : 0;
}

void DrawOnTop(UObject comp) {
    if (!S.on_top || L.depth_off < 0) return;
    UObject mi = 0;
    // Transparent and two-sided (Host) picks TranslucentMaterial.
    ue::GetProp(comp, "TranslucentMaterial", mi);
    UObject base = BaseMaterial(mi);
    if (!base) {
        if (OT.lines++ < 3) log::Warn("world menu: the panel's material wasn't found; menus can't be drawn on top");
        return;
    }
    uintptr_t at = base + static_cast<uintptr_t>(L.depth_off);
    if (!mem::IsReadable(at, 1)) return;
    uint8_t v = mem::ReadOr<uint8_t>(at, 0);
    if (!OT.set || OT.mat != base) {
        OT.mat = base;
        OT.index = IndexOf(base);
        OT.was = (v & L.depth_off_mask) != 0;
    }
    *reinterpret_cast<volatile uint8_t*>(at) = static_cast<uint8_t>(v | L.depth_off_mask);
    // Drawn last among see-through things too (smoke, glass), or those would
    // be drawn over it now that depth no longer decides.
    ue::SetProp<int32_t>(comp, "TranslucencySortPriority", 1000);
    bool took = (mem::ReadOr<uint8_t>(at, 0) & L.depth_off_mask) != 0;
    if (!OT.set && OT.lines++ < 6)
        log::Info("world menu: forearm menus drawn on top of the game (%s: depth test %s%s)",
                  ue::GetName(base).c_str(), took ? "off" : "STILL ON", OT.was ? ", it was already off" : "");
    OT.set = true;
}

void RestoreDepthTest() {
    if (!OT.set) return;
    OT.set = false;
    if (OT.was || !Alive(OT.mat, OT.index)) return;
    uintptr_t at = OT.mat + static_cast<uintptr_t>(L.depth_off);
    if (!mem::IsReadable(at, 1)) return;
    uint8_t v = mem::ReadOr<uint8_t>(at, 0);
    *reinterpret_cast<volatile uint8_t*>(at) = static_cast<uint8_t>(v & ~L.depth_off_mask);
}

// ------------------------------------------------- the game's black box --
// In the pit after the microwave fall the game puts a black box around your
// head (BlackCube_C; owner's 2026-11-02 21:05 run: the laser started inside
// BlackCube_C_1.Cube). Your arm leaves it easily, and its wall then covers
// the menu. While a menu is on the arm, a box like that which is around your
// head is made bigger than your reach (it stays black), or hidden. Found by
// the scan thread from its class name; only a box with your head inside it
// is ever touched, and it is put back when the last menu goes.
bool IsBlackBoxName(std::string n) {
    for (auto& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return n.find("black") != std::string::npos && n.find("cube") != std::string::npos;
}

std::mutex g_box_mutex;
std::vector<std::pair<UObject, int>> g_box_found;  // filled by the scan thread
std::atomic<bool> g_box_wanted{false};             // a menu is on the arm, look
std::unordered_map<UObject, bool> g_box_classes;   // scan thread only

struct BlackBox {
    UObject actor = 0;
    int index = -1;
    std::string name;
    FVector scale0{1, 1, 1};
    double grown = 1.0;  // total growth we applied
    bool hidden = false;
    bool said_away = false;
    bool said_fine = false;
};
std::vector<BlackBox> g_boxes;  // game thread
double g_box_check_at = 0;

// Scan thread: every actor whose class name has "black" and "cube" in it.
void FindBlackBoxes() {
    static uint64_t s_last = 0;
    if (!g_box_wanted.load() || !L.actor_class) return;
    uint64_t now = GetTickCount64();
    if (now - s_last < 2000) return;
    s_last = now;
    double t0 = perf::NowMs();
    std::vector<std::pair<UObject, int>> found;
    ue::objects::ForEach([&](int index, UObject o) {
        UObject cls = ue::GetClass(o);
        if (!cls) return;
        auto it = g_box_classes.find(cls);
        bool box;
        if (it != g_box_classes.end()) {
            box = it->second;
        } else {
            box = IsBlackBoxName(ue::GetName(cls)) && ue::reflection::IsChildOf(cls, L.actor_class);
            g_box_classes.emplace(cls, box);
        }
        if (!box) return;
        uint32_t flags = mem::ReadOr<uint32_t>(o + ue::off::kObjFlags, RF_ClassDefaultObject);
        if (flags & (RF_ClassDefaultObject | RF_BeginDestroyed | RF_FinishDestroyed)) return;
        found.emplace_back(o, index);
    });
    double ms = perf::NowMs() - t0;
    std::lock_guard<std::mutex> lock(g_box_mutex);
    static int s_lines = 0;
    if (found.size() != g_box_found.size() && s_lines++ < 20)
        log::Info("world menu: %zu black box actor(s) in the level (looked in %.1f ms)", found.size(), ms);
    g_box_found = std::move(found);
}

struct Bounds {
    FVector o, e;
};
bool ActorBounds(UObject actor, Bounds* b) {
    ue::Call c(actor, "GetActorBounds");
    c.Set("bOnlyCollidingComponents", false).Set("bIncludeFromChildActors", false);
    if (!c.Invoke()) return false;
    b->o = c.Get<FVector>("Origin");
    b->e = c.Get<FVector>("BoxExtent");
    return b->e.X > 0 && b->e.Y > 0 && b->e.Z > 0;
}

double Axis(const FVector& v, int i) { return i == 0 ? v.X : i == 1 ? v.Y : v.Z; }

// Head inside the box, with `reach` to spare on every side. `room` < 1 asks
// for more, for a box that is turned (its axis-aligned bounds are bigger
// than the box itself).
bool HeadFits(const Bounds& b, const FVector& head, double reach, double room) {
    for (int i = 0; i < 3; ++i)
        if (std::fabs(Axis(head, i) - Axis(b.o, i)) + reach > Axis(b.e, i) * room) return false;
    return true;
}

void HideBox(BlackBox& bx, const char* why) {
    if (bx.hidden) return;
    ue::Call h(bx.actor, "SetActorHiddenInGame");
    h.Set("bNewHidden", true);
    h.Invoke();
    bx.hidden = true;
    log::Info("world menu: hid the black box %s while a menu is on the arm (%s)", bx.name.c_str(), why);
}

void KeepBoxesAway() {
    if (S.black_box == kBoxOff || !S.forearm) return;
    double now = Now();
    if (now - g_box_check_at < 0.25) return;
    g_box_check_at = now;
    {
        std::lock_guard<std::mutex> lock(g_box_mutex);
        for (auto& f : g_box_found) {
            bool known = false;
            for (auto& bx : g_boxes) known = known || bx.actor == f.first;
            if (known || !Alive(f.first, f.second)) continue;
            BlackBox bx;
            bx.actor = f.first;
            bx.index = f.second;
            bx.name = ue::GetName(f.first);
            g_boxes.push_back(bx);
        }
    }
    FVector head;
    double yaw;
    if (g_boxes.empty() || !HeadInWorld(&head, &yaw)) return;
    const double reach = 1.5 * G.cm_per_m;  // arm's length and the menu, with room
    for (auto& bx : g_boxes) {
        if (bx.hidden || !Alive(bx.actor, bx.index)) continue;
        Bounds b;
        if (!ActorBounds(bx.actor, &b)) continue;
        if (!HeadFits(b, head, 0, 1.0)) {
            if (!bx.said_away) {
                bx.said_away = true;
                log::Info("world menu: black box %s isn't around your head; left alone", bx.name.c_str());
            }
            continue;
        }
        bx.said_away = false;
        if (S.black_box == kBoxHide) {
            HideBox(bx, "menu_black_box=hide");
            continue;
        }
        if (HeadFits(b, head, reach, 0.7)) {
            if (!bx.said_fine) {
                bx.said_fine = true;
                log::Info("world menu: black box %s is around your head and already bigger than your reach "
                          "(%.0f x %.0f x %.0f cm)", bx.name.c_str(), b.e.X * 2, b.e.Y * 2, b.e.Z * 2);
            }
            continue;
        }
        // Grow it about its own pivot until the head has `reach` to spare.
        ue::Call gl(bx.actor, "K2_GetActorLocation");
        ue::Call gs(bx.actor, "GetActorScale3D");
        if (!gl.Invoke() || !gs.Invoke()) continue;
        FVector pivot = gl.Result<FVector>();
        FVector scale = gs.Result<FVector>();
        if (bx.grown == 1.0) bx.scale0 = scale;
        double k = 0;
        for (double f : {1.5, 2.0, 3.0, 4.0, 6.0, 8.0, 12.0, 16.0, 24.0, 32.0, 48.0, 64.0}) {
            if (bx.grown * f > 64.0) break;
            Bounds g{pivot + (b.o - pivot) * f, b.e * f};
            if (HeadFits(g, head, reach, 0.7)) {
                k = f;
                break;
            }
        }
        if (k == 0) {
            HideBox(bx, "growing it enough would take more than 64 times its size");
            continue;
        }
        ue::Call ss(bx.actor, "SetActorScale3D");
        ss.Set("NewScale3D", scale * k);
        ss.Invoke();
        bx.grown *= k;
        Bounds after;
        if (!ActorBounds(bx.actor, &after) || !HeadFits(after, head, reach, 1.0)) {
            log::Warn("world menu: growing the black box %s didn't take (was %.0f x %.0f x %.0f cm)",
                      bx.name.c_str(), b.e.X * 2, b.e.Y * 2, b.e.Z * 2);
            HideBox(bx, "growing it didn't work");
            continue;
        }
        log::Info("world menu: black box %s was around your head (%.0f x %.0f x %.0f cm); grew it %.1fx to "
                  "%.0f x %.0f x %.0f cm so your arm stays inside it",
                  bx.name.c_str(), b.e.X * 2, b.e.Y * 2, b.e.Z * 2, bx.grown, after.e.X * 2, after.e.Y * 2,
                  after.e.Z * 2);
    }
}

// The last menu went: the boxes go back to how the game had them.
void PutBoxesBack() {
    g_box_wanted = false;
    for (auto& bx : g_boxes) {
        if (!Alive(bx.actor, bx.index)) continue;
        if (bx.grown != 1.0) {
            ue::Call ss(bx.actor, "SetActorScale3D");
            ss.Set("NewScale3D", bx.scale0);
            ss.Invoke();
        }
        if (bx.hidden) {
            ue::Call h(bx.actor, "SetActorHiddenInGame");
            h.Set("bNewHidden", false);
            h.Invoke();
        }
        if (bx.grown != 1.0 || bx.hidden) log::Info("world menu: put the black box %s back", bx.name.c_str());
    }
    g_boxes.clear();
    std::lock_guard<std::mutex> lock(g_box_mutex);
    g_box_found.clear();
}

bool Host(const Candidate& c) {
    double t0 = perf::NowMs();
    bool holder_ready = G.actor && G.wic;  // made at level start (PrepareHolder)
    if (!EnsureActor(c.obj)) return false;
    UObject comp = AddComponent(G.actor, L.widget_comp_class, true);
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
    DrawOnTop(comp);  // before it starts, so it is made with the switch on
    FinishComponent(G.actor, comp);
    if (S.forearm && S.black_box != kBoxOff) g_box_wanted = true;
    TickWhenPaused(comp);
    if (L.have_ui_profile) {
        ue::Call cp(comp, "SetCollisionProfileName");
        cp.Set("InCollisionProfileName", L.ui_profile).Set("bUpdateOverlaps", false);
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
    // Full-screen backdrops inside the menu (a background blur the size of
    // the screen) would read as a box on the arm, so any background blur is
    // hidden. Found by walking the menu's own widget tree (a few dozen
    // widgets), never by scanning every object in the game.
    std::vector<UObject> blurs;
    int widgets = FindBlurs(c.obj, &blurs);
    for (UObject b : blurs) {
        ue::Call v(b, "SetVisibility");
        v.Set("InVisibility", kCollapsed);
        v.Invoke();
        log::Info("world menu: hid %s (a background blur)", ue::GetName(b).c_str());
    }
    int boxes = HideBackdrops(c.obj, p.name);
    if (boxes > 0) log::Info("world menu: hid %d see-through backdrop box(es) in %s", boxes, p.name.c_str());
    FitContent(G.panels.back());
    if (G.placed) PosePanel(G.panels.back());  // zoomed, before this frame is drawn
    log::Info("world menu: %s on the arm in %.1f ms (%d widgets looked at; holder %s)", p.name.c_str(),
              perf::NowMs() - t0, widgets, holder_ready ? "made at level start" : "made just now");
    // A menu the game put up by itself (the area select after the microwave
    // fall comes up in a dark pit) is easy to miss on the arm: buzz that hand
    // so you look. Only for the first menu; sub-menus come from your clicks.
    if (S.arrive_buzz && S.forearm && p.layer == 0) {
        input::Haptic(static_cast<input::Hand>(S.arm), 0.7f, 0.25f, 160.f);
        log::Info("world menu: buzzed the %s hand: a menu is on the arm", S.arm == input::kLeft ? "left" : "right");
    }
    return true;
}

void ReleaseKey() {
    if (!G.pt.key_down) return;
    G.pt.key_down = false;
    if (!Alive(G.wic, G.wic_index)) return;
    // The click lands here (on release), and the game's OnClicked may put
    // up the next menu right away: let our menu catcher see it.
    ue::process_event::GameCodeScope game_code;
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
            if (!c.late)
                log::Info("world menu: %s came up but isn't on the screen, left alone",
                          ue::GetName(c.cls).c_str());
            continue;
        }
        if (c.late)
            log::Warn("world menu: %s was on the flat screen without being caught when it was built; "
                      "caught now that it took focus", ue::GetName(c.cls).c_str());
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

// ---------------------------------------------------------- sub-menus --
// The pause menu stays on the screen while Options (or a Yes/No dialog, Task,
// Credits...) is up: the 2026-11-02 study saw Pause only lose focus, never
// close. On the arm both would sit in the same spot, the pause buttons
// showing through the sub-menu. So only the top menu is shown, like a watch
// face changing page; the one underneath comes back when the top one closes.

bool WidgetShows(const Panel& p) {
    uint8_t vis = p.last_vis == 255 ? kVisible : p.last_vis;
    return vis != kCollapsed && vis != kHidden;
}

void SetShown(Panel& p, bool shown) {
    if (p.shown == shown) return;
    p.shown = shown;
    if (!Alive(p.comp, p.comp_index)) return;
    ue::Call c(p.comp, "SetVisibility");
    c.Set("bNewVisibility", shown).Set("bPropagateToChildren", true);
    c.Invoke();
    log::Info("world menu: %s %s", p.name.c_str(),
              shown ? "shown again (the menu on top of it closed)" : "hidden under the sub-menu on top of it");
}

// Cheap: a few property reads and at most one call per change, no searches.
void UpdateStack() {
    // The top menu is the newest one the game hasn't hidden itself.
    int top = -1;
    for (int i = static_cast<int>(G.panels.size()) - 1; i >= 0 && top < 0; --i)
        if (WidgetShows(G.panels[i])) top = i;
    for (int i = 0; i < static_cast<int>(G.panels.size()); ++i)
        SetShown(G.panels[i], !S.stack || top < 0 || i == top);
    // A sub-menu on top gets the keyboard focus, so B/Y (Escape) reaches it
    // (Options closes on Escape). Moving it from the flat screen onto the arm
    // can leave the focus behind on the flat screen. When the sub-menu
    // closes the game gives the focus back to the menu underneath itself.
    static UObject s_focused = 0;
    UObject want = top > 0 ? G.panels[top].widget : 0;
    if (want && want != s_focused) {
        CallVoid(want, "SetFocus");
        log::Info("world menu: keyboard focus to %s (%s)", G.panels[top].name.c_str(),
                  CallBool(want, "HasAnyUserFocus") || CallBool(want, "HasFocusedDescendants")
                      ? "has it" : "asked for; B/Y may not reach it");
    }
    s_focused = want;
}

// ------------------------------------------------------------ pointer --

struct Hit {
    bool ok = false;
    double t = 0;
    FVector point;
    const Panel* panel = nullptr;
};

// --------------------------------------------- the pointer's own hit --
// With the pointer's source set to Custom, each frame the laser's hit on a
// panel is handed to it as a hit result made by the engine itself
// (GameplayStatics.MakeHitResult, so the component reference is a proper
// one) and passed to WidgetInteractionComponent.SetCustomHitResult. When
// the laser is off every panel, an empty hit (not blocking) is handed over
// once, which lets go of the hovered button.
bool g_sent_hit = false;  // the pointer holds a hit on a panel
bool g_own_hit_broken = false;

void SendHit(const Hit* hit, const FVector& origin, const FVector& dir) {
    if (!S.own_hit || g_own_hit_broken || !Alive(G.wic, G.wic_index)) return;
    if (!hit && !g_sent_hit) return;
    ue::Call set(G.wic, "SetCustomHitResult");
    int32_t want = 0;
    uint8_t* dst = set.Raw("HitResult", &want);
    auto give_up = [](const char* why) {
        g_own_hit_broken = true;
        log::Error("world menu: %s; the laser goes back to the game's collision check", why);
        if (Alive(G.wic, G.wic_index)) ue::SetProp<uint8_t>(G.wic, "InteractionSource", kSourceWorld);
    };
    if (!set.Valid() || !dst || want <= 0) return give_up("the pointer has no SetCustomHitResult");
    if (hit && hit->panel && L.statics) {
        ue::Call make(L.statics, "MakeHitResult");
        FVector n = G.rot.Rotate(FVector{1, 0, 0});  // the panel's face
        FVector end = origin + dir * (hit->t + 0.05 * G.cm_per_m);
        make.Set("bBlockingHit", true).Set("bInitialOverlap", false)
            .Set("Time", static_cast<float>(hit->t / (hit->t + 0.05 * G.cm_per_m)))
            .Set("Distance", static_cast<float>(hit->t))
            .Set("Location", hit->point).Set("ImpactPoint", hit->point)
            .Set("Normal", n).Set("ImpactNormal", n)
            .Set("HitActor", G.actor).Set("HitComponent", hit->panel->comp)
            .Set("TraceStart", origin).Set("TraceEnd", end);
        int32_t size = 0;
        const uint8_t* made = make.Invoke() ? make.Raw("ReturnValue", &size) : nullptr;
        if (!made || size != want) {
            log::Error("world menu: MakeHitResult gave %d bytes, SetCustomHitResult wants %d", size, want);
            return give_up("could not make a hit for the pointer");
        }
        std::memcpy(dst, made, static_cast<size_t>(size));
    }
    // else: the zeroed buffer is "no hit" (bBlockingHit false, no component)
    {
        ue::process_event::GameCodeScope game_code;  // hover may make the game react
        set.Invoke();
    }
    g_sent_hit = hit != nullptr;
}

// ------------------------------------------------- laser diagnostics --
// What the laser is doing, logged when it changes (never per frame): which
// menu it is on, and once a second while it is on one, what the game's own
// collision finds along the beam where the pointer used to look (3 cm either
// side of the menu). That second line names whatever stopped the old
// pointer, e.g. a wall or a box the player stands inside.
struct LaserLog {
    std::string on;          // menu the laser is on, "" for none
    int lines = 0;
    double probe_at = 0;
    std::string probe_said;
    int probe_lines = 0;
} LL;

void ProbeBeam(const Hit& hit, const FVector& dir) {
    double now = Now();
    if (now - LL.probe_at < 1.0 || LL.probe_lines >= 30 || !L.ksl || !L.statics || !hit.panel) return;
    LL.probe_at = now;
    double back = 0.03 * G.cm_per_m;
    FVector from = hit.point - dir * back, to = hit.point + dir * back;
    ue::Call t(L.ksl, "LineTraceSingle");
    t.Set("WorldContextObject", G.actor).Set("Start", from).Set("End", to)
        .Set("TraceChannel", uint8_t{0}).Set("bTraceComplex", false).Set("bIgnoreSelf", false);
    std::string said;
    if (!t.Invoke()) return;
    if (!t.Result<bool>()) {
        said = "nothing blocks the beam at the menu (the menu itself isn't hit either)";
    } else {
        int32_t hit_size = 0, in_size = 0;
        uint8_t* raw = t.Raw("OutHit", &hit_size);
        ue::Call b(L.statics, "BreakHitResult");
        uint8_t* in = b.Raw("Hit", &in_size);
        if (!raw || !in || in_size != hit_size) return;
        std::memcpy(in, raw, static_cast<size_t>(hit_size));
        if (!b.Invoke()) return;
        UObject comp = b.Get<UObject>("HitComponent");
        UObject actor = b.Get<UObject>("HitActor");
        bool inside = b.Get<bool>("bInitialOverlap");
        float dist = b.Get<float>("Distance");
        char buf[400];
        if (comp && comp == hit.panel->comp) {
            std::snprintf(buf, sizeof(buf), "the game's collision reaches the menu (%.1f cm along)", dist);
        } else {
            std::string a = actor ? ue::GetName(actor) : "?";
            std::string c = comp ? ue::GetName(comp) : "?";
            std::string k = comp ? ue::GetName(ue::GetClass(comp)) : "?";
            if (inside)
                std::snprintf(buf, sizeof(buf), "the game's collision stops at %s.%s (%s): the beam starts inside it",
                              a.c_str(), c.c_str(), k.c_str());
            else
                std::snprintf(buf, sizeof(buf), "the game's collision stops at %s.%s (%s), %.1f cm along, before the menu",
                              a.c_str(), c.c_str(), k.c_str(), dist);
        }
        said = buf;
    }
    if (said == LL.probe_said) return;
    LL.probe_said = said;
    ++LL.probe_lines;
    log::Info("world menu: laser on %s: %s", hit.panel->name.c_str(), said.c_str());
}

void LogLaser(const Hit& best, const FVector& dir) {
    std::string on = best.ok && best.panel ? best.panel->name : std::string();
    if (on != LL.on && LL.lines < 60) {
        ++LL.lines;
        if (on.empty()) log::Info("world menu: laser left the menus");
        else log::Info("world menu: laser now on %s (pointer gets its hit %s)", on.c_str(),
                       S.own_hit && !g_own_hit_broken ? "from the laser" : "from the game's collision");
    }
    LL.on = on;
    if (best.ok) ProbeBeam(best, dir);
}

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
    // Only the part of the layout that holds the menu (FitContent), a little
    // wider so the edges of buttons still count.
    double s = Scale() * p.zoom;
    if (std::fabs(local.Y) > (p.half_w + 30) * s || std::fabs(local.Z) > (p.half_h + 30) * s) return h;
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
            if (!p.shown || !WidgetShows(p)) continue;
            Hit hit = RayPanel(o, d, p);
            hit.panel = &p;
            if (hit.ok && (!best.ok || hit.t < best.t)) best = hit;
        }
        if (best.ok) {
            // For the old collision-check pointer (menu_laser_own_hit=0): it
            // traces a few centimetres through the panel we hit.
            double back = 0.03 * G.cm_per_m;
            FVector from = best.point - d * back;
            FRotator r{std::asin(std::clamp(d.Z, -1.0, 1.0)) * kRadToDeg, std::atan2(d.Y, d.X) * kRadToDeg, 0};
            SetWorldPose(G.wic, from, r);
            laser.hit = true;
            laser.length_m = static_cast<float>(best.t / G.cm_per_m);
        }
        SendHit(best.ok ? &best : nullptr, o, d);
        LogLaser(best, d);
    } else {
        SendHit(nullptr, o, FVector{1, 0, 0});
    }
    float want = best.ok ? static_cast<float>(0.06 * G.cm_per_m) : 0.f;
    if (want != G.last_distance) {
        ue::SetProp<float>(G.wic, "InteractionDistance", want);
        G.last_distance = want;
    }

    bool down = G.pt.trigger_down[G.pt.hand] && best.ok;
    if (down && !G.pt.key_down) {
        ue::process_event::GameCodeScope game_code;  // see ReleaseKey
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
            ue::process_event::GameCodeScope game_code;  // see ReleaseKey
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
    UpdateStack();
    g_menu_up = !G.panels.empty();
    // Blur the world past arm's length while a menu is on the arm.
    static bool s_blur = false;
    if (s_blur != !G.panels.empty()) {
        s_blur = !G.panels.empty();
        // Nothing nearer than the focus may blur (hands, arms, body, menu).
        // Set right here, before the blur starts: the background cvars pass
        // only runs while the player ticks and had not run yet on the
        // owner's 2026-11-02 12:13 run, so the hands came out blurred.
        if (s_blur) cvars::RunNow("r.DOF.Kernel.MaxForegroundRadius 0", G.actor);
        int n = render_fixes::SetPauseBlur(s_blur);
        if (n >= 0 && s_blur)
            log::Info("world menu: pause blur on (%d settings; r.DepthOfFieldQuality %d, "
                      "r.DOF.Kernel.MaxForegroundRadius %.3f)",
                      n, cvars::ReadIntNow("r.DepthOfFieldQuality", -1),
                      cvars::ReadFloatNow("r.DOF.Kernel.MaxForegroundRadius", -1.f));
        else if (n >= 0)
            log::Info("world menu: pause blur off (%d settings)", n);
    }
    if (G.panels.empty()) {
        g_active = false;
        SendHit(nullptr, FVector{}, FVector{1, 0, 0});  // let go of the last menu's button
        RestoreDepthTest();
        if (!g_boxes.empty() || g_box_wanted.load()) PutBoxesBack();
        if (!LL.on.empty()) {
            LL.on.clear();
            LL.probe_said.clear();
        }
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
    KeepBoxesAway();
    UpdatePointer();
    // A laser click can put up the next menu right now (Options, Task...).
    // Take it onto the arm before this frame is drawn: left for the next
    // frame, it was drawn once on the flat screen, which the headset shows in
    // front of your face (owner 2026-11-02 13:57: "the menu starts on my face
    // and then move to my wrist").
    TakeNewMenus();
    UpdateStack();
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
    S.arm_roll_deg = config::GetFloat("menu_forearm_turn_deg", 0.f);
    S.hide_covers = config::GetBool("menu_hide_focus_cover", true);
    S.laser = config::GetBool("menu_world_laser", true);
    S.stack = config::GetBool("menu_submenu_replaces", true);
    S.arm_size = std::clamp(config::GetFloat("menu_forearm_size", 1.25f), 0.5f, 3.f);
    S.hide_backdrops = config::GetBool("menu_hide_backdrops", true);
    S.own_hit = config::GetBool("menu_laser_own_hit", true);
    S.arrive_buzz = config::GetBool("menu_arrive_buzz", true);
    S.on_top = config::GetBool("menu_always_on_top", true);
    {
        std::string bb = config::GetString("menu_black_box", "grow");
        S.black_box = bb == "off" || bb == "0" ? kBoxOff : bb == "hide" ? kBoxHide : kBoxGrow;
    }
    S.fit = config::GetBool("menu_fit_content", true);
    S.fit_max_zoom = std::clamp(config::GetFloat("menu_fit_max_zoom", 3.f), 1.f, 6.f);
    if (!S.enabled) {
        log::Info("world menu: off (menu_mode=%s)", mode.c_str());
        return;
    }
    xr::RegisterListener(&g_laser_listener);
    if (S.forearm)
        log::Info("world menu: on the back of the %s forearm, %.2f m wide (%.2f m x size %.2f)",
                  S.arm == input::kLeft ? "left" : "right", S.arm_width_m * S.arm_size, S.arm_width_m, S.arm_size);
    else
        log::Info("world menu: floating in front, %.2f m wide at %.2f m", S.width_m, S.distance_m);
}

// The pump runs on the game thread from the player controller's tick, which
// keeps running while the game is paused (the 2026-11-02 study counted it at
// the frame rate during the pause menu), and from the player's tick until the
// controller is found. Nothing runs from inside the stereo view setup.
// The panel holder (an actor with the laser's interaction component) is
// made during play, once per level, so opening a menu only has to add the
// menu's own panel. Before this the holder was made the moment the pause
// menu opened, as part of a 2.5 s stall.
void PrepareHolder(UObject player) {
    static double s_checked = 0;
    double now = Now();
    if (now - s_checked < 1.0) return;
    s_checked = now;
    if (G.actor && Alive(G.actor, G.actor_index) && G.wic && Alive(G.wic, G.wic_index)) return;
    double t0 = perf::NowMs();
    if (EnsureActor(player)) {
        cvars::RunNow("r.DOF.Kernel.MaxForegroundRadius 0", player);  // for the pause blur
        log::Info("world menu: forearm menu holder ready for this level (%.1f ms)", perf::NowMs() - t0);
    }
}

void AfterControllerTick(UObject, UObject, void*) { OnGameFrame(); }

// Follows one object's per-frame Blueprint event (its class's own override
// only, so we never take over a function object other code shares).
std::mutex g_follow_mutex;  // the player tick and the background Scan both call
UObject g_pc_cls = 0, g_cam_cls = 0;

void FollowFrameEventOf(UObject obj, const char* fn_name, UObject* hooked_cls) {
    UObject cls = ue::GetClass(obj);
    if (!cls) return;
    std::lock_guard<std::mutex> lock(g_follow_mutex);
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

// The player's own controller and camera manager, read straight off the
// player every frame (two property reads, no searches). The background Scan
// used to be the only way to find them, and it can lag far behind: on the
// owner's 2026-11-02 11:17 run the camera manager was found 64 s after the
// level started, so a pause 30 s in left the forearm menu frozen in place
// with a dead laser for 21 s.
void FollowFromPlayer(UObject player) {
    static UObject s_pc = 0, s_cam = 0;  // game thread only
    UObject pc = 0;
    if (!player || !ue::GetProp(player, "Controller", pc) || !pc) return;
    if (pc != s_pc) {
        s_pc = pc;
        FollowFrameEventOf(pc, "ReceiveTick", &g_pc_cls);
    }
    UObject cam = 0;
    if (ue::GetProp(pc, "PlayerCameraManager", cam) && cam && cam != s_cam) {
        s_cam = cam;
        FollowFrameEventOf(cam, "BlueprintUpdateCamera", &g_cam_cls);
    }
}

void OnPlayerTick(UObject player, float) {
    if (S.enabled && ue::process_event::Ready()) FollowFromPlayer(player);
    if (S.enabled && player && G.panels.empty()) PrepareHolder(player);
    OnGameFrame();
}

// The game's Blueprints took a widget away (or we did, which is skipped).
void ExecRemoveFromParent(UObject context, void* stack, void* result) {
    if (!t_our_call && context) {
        std::lock_guard<std::mutex> lock(g_events_mutex);
        g_removed.push_back(context);
    }
    g_orig_remove(context, stack, result);
}

// A menu's Construct: the game just put it up. A menu taking keyboard focus
// (OnAddedToFocusPath, which Slate fires on its own every time) is the
// backup, so a menu built where we couldn't see it still leaves the flat
// screen instead of sitting in front of the face; already hosted ones are
// skipped by TakeNewMenus.
void OnAnyEvent(UObject self, UObject function, void*) {
    if (!S.enabled || !self || !function) return;
    // Each widget class has its own function objects: remember which ones
    // are a Construct (1) or an OnAddedToFocusPath (2).
    enum : uint8_t { kOther = 0, kConstruct = 1, kFocus = 2 };
    static std::mutex s_mutex;
    static std::unordered_map<UObject, uint8_t> s_kind;
    uint8_t kind;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto it = s_kind.find(function);
        if (it == s_kind.end()) {
            std::string n = ue::GetName(function);
            it = s_kind.emplace(function, n == "Construct" ? kConstruct : n == "OnAddedToFocusPath" ? kFocus : kOther)
                     .first;
        }
        kind = it->second;
        if (kind == kOther) return;
    }
    UObject cls = ue::GetClass(self);
    if (!cls || !IsMenuClass(cls)) return;
    // Only menus that stand on their own; one built into another menu's
    // panels (UIM_YesNo inside Options) travels with that menu.
    UObject slot = 0;
    if (ue::GetProp(self, "Slot", slot) && slot) return;
    int index = IndexOf(self);
    bool late = kind == kFocus;
    if (!late) log::Info("world menu: the game put up %s", ue::GetName(cls).c_str());
    std::lock_guard<std::mutex> lock(g_events_mutex);
    g_new_menus.push_back({self, index, cls, late});
}

void InitGame() {
    if (!S.enabled) return;
    ResolveLookups();  // before any game-thread code can need them
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

// Background thread, the fallback when there is no player: finds the player
// controller and its camera manager by searching. Both kept firing at the
// frame rate during the pause menu in the 2026-11-02 study (ReceiveTick and
// BlueprintUpdateCamera). Skipped once the player tick has found them.
void FollowFrameEvent(const char* base_class, const char* fn_name, UObject* hooked_cls) {
    {
        std::lock_guard<std::mutex> lock(g_follow_mutex);
        if (*hooked_cls) return;
    }
    UObject base = ue::FindObject(base_class);
    if (!base) return;
    UObject obj = ue::FindFirstInstance(base);
    if (obj) FollowFrameEventOf(obj, fn_name, hooked_cls);
}

void Scan() {
    if (!S.enabled || !ue::objects::Ready() || !ue::process_event::Ready()) return;
    FindBlackBoxes();  // only while a menu is on the arm, every 2 s at most
    static int s_calls = 0;
    if ((s_calls++ % 4) != 0) return;
    FollowFrameEvent("Class /Script/Engine.PlayerController", "ReceiveTick", &g_pc_cls);
    FollowFrameEvent("Class /Script/Engine.PlayerCameraManager", "BlueprintUpdateCamera", &g_cam_cls);
}

bool MenuUp() { return g_menu_up.load(); }
bool Active() { return g_active.load(); }
bool Enabled() { return S.enabled; }

}  // namespace holevr::vr::world_menu
