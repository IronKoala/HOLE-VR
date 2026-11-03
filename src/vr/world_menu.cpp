#include "vr/world_menu.h"

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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
#include "vr/vr_options.h"

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
    int text_outline = 2;         // black outline around menu text, in layout pixels (0 = off)
    float text_scale = 1.5f;      // menu fonts this much bigger, pulled back where they don't fit (GrowText)
    float text_min_px = 32.f;     // ...and small print at least this big (layout pixels)...
    float text_max_px = 64.f;     // ...but no text grown past this (titles are big already)...
    float text_max_scale = 2.5f;  // ...nor more than this many times the game's size
    int snapshots = 2;            // pictures of each menu's panel per run, to check its layout (0 = off)
    bool fit_shape = true;        // a tall narrow menu gets a tall panel of the same area (FitContent)
    float fit_min_zoom = 1.3f;    // menus filling the whole screen still get at least this zoom
    bool follow_exposure = true;  // dim the menus as much as the game brightens dark areas
    float exposure_ref = -1.f;    // game exposure (stops) at which menus are drawn at full brightness
    float brightness = 1.f;       // all menus and the HUD, times this
    float brightness_min = 0.03f; // never dimmer than this
    bool separate = true;         // a column or row running into a picture is moved clear (SeparateParts)
    bool hud = true;              // the in-level HUD on the forearm, shown when you look at it
    float hud_size = 1.f;         // its width, times the menu's
    float hud_look_deg = 50.f;    // shown when the arm faces you within this angle...
    float hud_distance_m = 0.8f;  // ...and is this close to your eyes
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
// The menu button (vr/ui_panel, present thread) closes the pause menu when it
// is the menu on top: the game itself only opens it with Escape (owner
// 2026-11-03: "i want it to be a toggle instead of only an on button").
std::atomic<bool> g_top_is_pause{false};
std::atomic<bool> g_close_pause{false};
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

// A part of a menu's 1920x1080 layout, in its pixels.
struct Rect {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    double W() const { return x1 - x0; }
    double H() const { return y1 - y0; }
};

// One text of a menu on the arm, made bigger by GrowText.
struct TextItem {
    UObject obj = 0;
    int index = -1;
    float base = 0;                // the game's font size
    float scale = 1;               // how much bigger it is now
    bool measured = false;         // drawn at the game's size and measured
    bool grown = false;            // made bigger once (a text pulled back to the game's size stays there)
    double want_w = 0, want_h = 0; // the room its letters asked for at the last check (its words changed if this does)
    Rect before;                   // where it was then
    std::vector<UObject> chain;    // the widgets around it, nearest first, up to the menu
    size_t boxes = 0;              // how many of them it must fit in (up to the first list)
    std::vector<uint8_t> box_mode; // per widget around it: kBoth, kAcross (x only), kAlong (y only), kNone
    std::vector<double> base_over; // how far it already ran past each of those at the game's size
    float prev_scale = 1;          // its size before the last change (to tell when the engine has laid it out)
    double base_want_h = 0;        // the height its letters asked for at the game's size
};
constexpr uint8_t kBoth = 0, kAcross = 1, kAlong = 2, kNone = 3;

// A picture in a menu (an icon, a line, a card or button face): a growing
// text mustn't run over one it didn't cover, or out of one it sat inside.
struct Picture {
    UObject obj = 0;
    int index = -1;
    Rect before;
};

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
    bool fit_pending = false;  // waiting to be drawn on its panel once, to be measured
    double regrow_at = -1;     // GrowHitArea after a click
    // GrowText: the menu's texts, and where the growing is (see TextStep).
    std::vector<TextItem> texts;
    std::vector<Picture> pictures;
    int text_phase = 0;        // kTextOff / kTextBaseline / kTextCheck
    int text_stale = 0;        // frames waited for the engine to lay out the new sizes
    bool stale_off = false;    // the layout check never matched once: check as laid out from then on
    bool separated = false;    // parts running into pictures moved apart (SeparateParts)
    int fit_wait = 0;          // frames to wait after moving parts, before measuring the menu
    double recheck_at = -1;    // one more look a moment after the texts fit (TextStep)
    int rechecks = 0;
    int text_round = 0;
    int text_wait = 0;         // frames to let the engine lay the menu out again
    int text_pulled = 0;
    bool regrow_after_text = false;  // GrowHitArea once the texts of a new page fit
    double text_watch_at = 0;  // next look for texts that came into view or changed (TextWatch)
    double text_scan_at = 0;   // next look for texts the game added (NewPageTexts)
    double snapshot_at = -1;   // a picture of the panel then (SnapshotPanel)
    int switcher_page = -2;    // the death screen's page, to tell when it changes
    double page_watch_at = 0;  // next look at it (WatchDeathScreen)
    double page_snapshot_at = -1;
    double kill_capture_at = 0;
    int kill_captures = 0;
    bool quiet_check = false;  // a check because a text's words changed: logged only if something moved
};
constexpr int kTextOff = 0, kTextBaseline = 1, kTextCheck = 2;

struct Pointer {
    bool trigger_down[2] = {};
    int hand = input::kRight;
    bool key_down = false;
    float scroll_accum = 0.f;
    bool scroll_held = false;  // stick pushed since it was last centred
    double scroll_t = 0;       // last scroll update (s)
    int scroll_logs = 0;
    bool was_over_button = false;
    // Where the laser last was on a menu (layout pixels from its pivot), so
    // the stick scrolls there after the laser slips off.
    UObject spot_widget = 0;
    double spot_y = 0, spot_z = 0;
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
    // FitContent: where the engine drew each widget (SlateBlueprintLibrary on
    // a widget's cached geometry), and the widget kinds the measuring treats
    // specially.
    UObject slate_lib = 0;      // SlateBlueprintLibrary default object
    UObject krl = 0;            // KismetRenderingLibrary default object (panel pictures)
    UObject scroll_class = 0, switcher_class = 0, spacer_class = 0;
    // Outlined text (OutlineText): TextBlock.Font (FSlateFontInfo) and, inside
    // it, OutlineSettings.OutlineSize / OutlineColor.
    UObject text_class = 0;
    int32_t text_font = -1, font_size = 0, outline_size = -1, outline_color = -1;
    // Bigger text (GrowText): FSlateFontInfo.Size (a float in UE5, an int in
    // UE4), PanelSlot.Parent and the WidgetTree class, to climb from a text
    // to the boxes around it.
    int32_t font_pt = -1;
    bool font_pt_float = true;
    int32_t text_color = -1;    // TextBlock.ColorAndOpacity (FSlateColor: its colour first)
    int32_t slot_parent = -1;
    UObject tree_class = 0;
    // DrawOnTop: UMaterial.bDisableDepthTest, from the engine's property data.
    UObject material_class = 0;
    int32_t depth_off = -1;
    uint8_t depth_off_mask = 0;
    // The widget tree, read straight from the engine's fields (TreeRoot,
    // Children, Visibility): Widget.Visibility and .Slot,
    // UserWidget.WidgetTree, WidgetTree.RootWidget, PanelWidget.Slots (an
    // array of slots) and PanelSlot.Content (the child widget in a slot).
    UObject panel_slot_class = 0;
    int32_t widget_visibility = -1, widget_slot = -1, uw_tree = -1, tree_root = -1;
    int32_t panel_slots = -1, slot_content = -1;
    bool children_checked = false;  // Children() compared once with GetChildAt
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

// FitContent measures menus from the engine's own layout, and OutlineText
// edits TextBlock.Font: the classes, and the font fields from the engine's
// property data (each checked for the size this code writes).
void ResolveFitOffsets() {
    namespace rf = ue::reflection;
    L.slate_lib = Lookup("SlateBlueprintLibrary /Script/UMG.Default__SlateBlueprintLibrary");
    L.krl = Lookup("KismetRenderingLibrary /Script/Engine.Default__KismetRenderingLibrary");
    L.scroll_class = Lookup("Class /Script/UMG.ScrollBox");
    L.switcher_class = Lookup("Class /Script/UMG.WidgetSwitcher");
    L.spacer_class = Lookup("Class /Script/UMG.Spacer");
    L.text_class = Lookup("Class /Script/UMG.TextBlock");
    if (!L.text_class) return;
    rf::PropertyInfo font = rf::FindProperty(L.text_class, "Font");
    rf::PropertyInfo outline = font.valid() && font.inner ? rf::FindProperty(font.inner, "OutlineSettings") : rf::PropertyInfo{};
    rf::PropertyInfo size = outline.valid() && outline.inner ? rf::FindProperty(outline.inner, "OutlineSize") : rf::PropertyInfo{};
    rf::PropertyInfo color = outline.valid() && outline.inner ? rf::FindProperty(outline.inner, "OutlineColor") : rf::PropertyInfo{};
    if (!font.valid() || font.element_size <= 0 || font.element_size > 0x200 || !size.valid() ||
        size.element_size != 4 || !color.valid() || color.element_size != 16) {
        log::Warn("world menu: TextBlock font layout not found; menu text keeps no outline");
        return;
    }
    L.text_font = font.offset;
    L.font_size = font.element_size;
    L.outline_size = outline.offset + size.offset;
    L.outline_color = outline.offset + color.offset;
    rf::PropertyInfo tc = rf::FindProperty(L.text_class, "ColorAndOpacity");
    if (tc.valid() && tc.element_size >= 16) L.text_color = tc.offset;
    rf::PropertyInfo pt = rf::FindProperty(font.inner, "Size");
    if (pt.valid() && pt.element_size == 4 && pt.offset >= 0 && pt.offset + 4 <= L.font_size &&
        (pt.type == "FloatProperty" || pt.type == "IntProperty")) {
        L.font_pt = pt.offset;
        L.font_pt_float = pt.type == "FloatProperty";
    } else {
        log::Warn("world menu: SlateFontInfo.Size not found; menu text keeps the game's size");
    }
}

// Offsets for walking a menu's widget tree without calling into the engine
// for every widget. Calling GetChildAt and friends through the engine, and
// looking properties up by name for each new widget class, made the Options
// menu (775 widgets) take about 110 ms to walk (2026-11-02), a hitch the
// owner felt. Each offset is checked against the size the field must have;
// a missing one falls back to the engine calls.
void ResolveTreeOffsets() {
    namespace rf = ue::reflection;
    UObject widget = Lookup("Class /Script/UMG.Widget");
    UObject tree = Lookup("Class /Script/UMG.WidgetTree");
    L.panel_slot_class = Lookup("Class /Script/UMG.PanelSlot");
    auto offset = [](UObject cls, const char* name, int32_t size) -> int32_t {
        if (!cls) return -1;
        rf::PropertyInfo p = rf::FindProperty(cls, name);
        if (!p.valid() || p.element_size != size || p.offset <= 0) {
            log::Warn("world menu: %s.%s not found as a %d-byte field; using engine calls for it",
                      ue::GetName(cls).c_str(), name, size);
            return -1;
        }
        return p.offset;
    };
    L.widget_visibility = offset(widget, "Visibility", 1);
    L.widget_slot = offset(widget, "Slot", 8);
    L.uw_tree = offset(L.user_widget_class, "WidgetTree", 8);
    L.tree_root = offset(tree, "RootWidget", 8);
    L.panel_slots = offset(L.panel_class, "Slots", 16);  // TArray: data, count, capacity
    L.slot_content = offset(L.panel_slot_class, "Content", 8);
    L.slot_parent = offset(L.panel_slot_class, "Parent", 8);
    L.tree_class = tree;
    if (L.panel_slots < 0 || L.slot_content < 0) L.panel_slots = L.slot_content = -1;
    if (L.uw_tree < 0 || L.tree_root < 0) L.uw_tree = L.tree_root = -1;
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
    ResolveTreeOffsets();
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
    if (L.widget_visibility >= 0) return mem::ReadOr<uint8_t>(widget + L.widget_visibility, kVisible);
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
    // Drawn by the game: the controller predicted like the arm it sits on.
    input::Snapshot in = input::PeekGame();
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

// Where the panel's own middle goes, so that the part of the menu holding
// its buttons (FitContent's box, centred on cx,cy in the 1920x1080 layout)
// sits at PanelCentre. The panel keeps the engine's pivot (its middle):
// setting the pivot to the box moved where the laser thought the menu was
// but not the picture, which the engine had already built around the old
// pivot, so every click landed off by the box's distance from the screen
// centre (owner 2026-11-03: "i have to aim at the sub menu above the one i
// actually want", and the task giver's Get Reward buttons couldn't be hit).
// The widget's layout x runs along the panel's -Y and its y along -Z.
FVector PanelOrigin(const Panel& p) {
    double s = Scale() * p.zoom;
    FQuat rot = ToQuat(PanelRotation());
    return PanelCentre(p) + rot.Rotate(FVector{0, (p.cx - kDrawW * 0.5) * s, (p.cy - kDrawH * 0.5) * s});
}

void PosePanel(const Panel& p) {
    if (!Alive(p.comp, p.comp_index)) return;
    double s = Scale() * p.zoom;
    SetWorldPose(p.comp, PanelOrigin(p), PanelRotation());
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

// While a menu is being put on the arm (WalkScope in Host), each widget
// class's answer is remembered, so an 800-widget menu doesn't climb the same
// class chains thousands of times. Only for that one call, since a class can
// be unloaded between menus.
thread_local std::map<std::pair<UObject, UObject>, bool>* t_isa_cache = nullptr;

bool IsA(UObject obj, UObject base) {
    UObject cls = ue::GetClass(obj);
    if (!cls || !base) return false;
    if (!t_isa_cache) return ue::reflection::IsChildOf(cls, base);
    auto key = std::make_pair(cls, base);
    auto it = t_isa_cache->find(key);
    if (it != t_isa_cache->end()) return it->second;
    bool r = ue::reflection::IsChildOf(cls, base);
    t_isa_cache->emplace(key, r);
    return r;
}

// Everything a menu walk needs to be quick, for the length of one Host call:
// memory checks remember every block of memory they looked at (the widgets
// of one menu are spread over a hundred or so), and class answers are kept.
struct WalkScope {
    mem::ScopedRegionCache regions;
    std::map<std::pair<UObject, UObject>, bool> isa;
    std::map<std::pair<UObject, UObject>, bool>* outer = t_isa_cache;  // a scope inside another keeps it
    WalkScope() { t_isa_cache = &isa; }
    ~WalkScope() { t_isa_cache = outer; }
    WalkScope(const WalkScope&) = delete;
    WalkScope& operator=(const WalkScope&) = delete;
};

// A user widget's top widget (the root of its own tree), or 0.
UObject TreeRoot(UObject user_widget) {
    if (L.uw_tree >= 0) {
        UObject tree = mem::ReadOr<UObject>(user_widget + L.uw_tree, 0);
        return tree ? mem::ReadOr<UObject>(tree + L.tree_root, 0) : 0;
    }
    UObject tree = 0, root = 0;
    if (ue::GetProp(user_widget, "WidgetTree", tree) && tree && ue::GetProp(tree, "RootWidget", root)) return root;
    return 0;
}

void ChildrenByCall(UObject panel, int max, std::vector<UObject>* out) {
    ue::Call count(panel, "GetChildrenCount");
    int n = count.Invoke() ? count.Result<int32_t>() : 0;
    for (int i = 0; i < n && i < max; ++i) {
        ue::Call child(panel, "GetChildAt");
        child.Set("Index", static_cast<int32_t>(i));
        if (child.Invoke() && child.Result<UObject>()) out->push_back(child.Result<UObject>());
    }
}

// A panel's child widgets in order (what GetChildAt gives), read from its
// slots array. The first panel with children is also asked through the
// engine, and if the two disagree the engine calls are used from then on.
void Children(UObject panel, int max, std::vector<UObject>* out) {
    out->clear();
    if (L.panel_slots < 0) return ChildrenByCall(panel, max, out);
    struct PtrArray {
        uintptr_t data;
        int32_t num, max;
    } a{};
    if (!mem::Read(panel + L.panel_slots, a) || a.num < 0 || a.num > 4096 || a.max < a.num) return;
    int n = std::min(a.num, max);
    if (n == 0) return;
    std::vector<uintptr_t> slots(static_cast<size_t>(n));
    if (!mem::IsReadable(a.data, slots.size() * sizeof(uintptr_t))) return;
    std::memcpy(slots.data(), reinterpret_cast<const void*>(a.data), slots.size() * sizeof(uintptr_t));
    for (uintptr_t slot : slots) {
        UObject child = slot ? mem::ReadOr<UObject>(slot + L.slot_content, 0) : 0;
        if (child) out->push_back(child);
    }
    if (!L.children_checked) {
        L.children_checked = true;
        std::vector<UObject> engine;
        ChildrenByCall(panel, max, &engine);
        if (engine != *out) {
            log::Warn("world menu: reading %s's children directly gave %zu, the engine says %zu; using engine calls",
                      ue::GetName(panel).c_str(), out->size(), engine.size());
            L.panel_slots = -1;
            *out = engine;
        } else {
            log::Info("world menu: menu layout read directly (checked on %s: %zu children)", ue::GetName(panel).c_str(),
                      out->size());
        }
    }
}

// Walks a menu's widget tree (and the trees of the user widgets inside it,
// e.g. its buttons) and collects any BackgroundBlur. Returns how many widgets
// it looked at. Uses the engine's own PanelWidget.GetChildAt, so it only
// touches the menu's widgets.
int FindBlurs(UObject menu, std::vector<UObject>* blurs, std::vector<UObject>* texts) {
    if (!L.panel_class || !L.user_widget_class) return 0;
    int seen = 0;
    std::vector<std::pair<UObject, int>> todo;  // widget, depth
    std::vector<UObject> kids;
    auto push_tree = [&](UObject user_widget, int depth) {
        if (UObject root = TreeRoot(user_widget)) todo.push_back({root, depth});
    };
    push_tree(menu, 0);
    while (!todo.empty() && seen < 2000) {
        auto [w, depth] = todo.back();
        todo.pop_back();
        ++seen;
        if (L.blur_class && IsA(w, L.blur_class)) blurs->push_back(w);
        if (texts && L.text_class && IsA(w, L.text_class)) texts->push_back(w);
        if (depth >= 32) continue;
        if (IsA(w, L.user_widget_class)) {
            push_tree(w, depth + 1);
        } else if (IsA(w, L.panel_class)) {
            Children(w, 256, &kids);
            for (UObject k : kids) todo.push_back({k, depth + 1});
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
    std::vector<UObject> kids;
    auto push_tree = [&](UObject user_widget, int depth) {
        if (UObject root = TreeRoot(user_widget)) todo.push_back({root, depth});
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
            Children(w, 256, &kids);
            for (UObject k : kids) todo.push_back({k, depth});
        }
    }
    return hidden;
}

// ------------------------------------------------------------ fitting --
// The game lays its menus out for a 1920x1080 screen, mostly empty: the pause
// menu is a narrow column in the middle. Shrinking the whole screen onto the
// arm left the text tiny (owner 2026-11-02: "the menu is big enough but its
// still hard to read the text ... larger text but they dont overlap, and they
// still fit the individual boxes"). So once a menu has been drawn on its
// panel, the part of it that shows something is measured where the engine
// actually drew it (each widget's cached geometry), and the panel is zoomed
// until that part fills the space the whole screen used to take on the arm.
// Text and boxes grow together, so nothing overlaps or spills out.
//
// The first version (2026-11-02) worked the layout out from CanvasPanel
// slots and never measured a single menu: every menu's top widget is a
// ScaleBox, which it didn't follow (owner's 2026-11-03 run: "couldn't
// measure its layout" for Pause, Options, Task, area select and results).
// Reading the engine's own layout doesn't depend on what a menu is built of.

// Where the engine last drew a widget, in its window's pixels (for a menu on
// a panel: the panel's 1920x1080 layout). Three engine calls: the widget's
// cached geometry, then SlateBlueprintLibrary turns it into a position and a
// size, so nothing here depends on how FGeometry is laid out.
bool WidgetRect(UObject w, Rect* out) {
    if (!L.slate_lib || !w) return false;
    ue::Call g(w, "GetCachedGeometry");
    if (!g.Invoke()) return false;
    int32_t size = 0;
    const uint8_t* geo = g.Raw("ReturnValue", &size);
    if (!geo || size <= 0) return false;
    auto with_geometry = [&](ue::Call& c) {
        int32_t want = 0;
        uint8_t* dst = c.Raw("Geometry", &want);
        if (!dst || want != size) return false;
        std::memcpy(dst, geo, static_cast<size_t>(size));
        return true;
    };
    ue::Call at(L.slate_lib, "LocalToAbsolute");
    ue::Call extent(L.slate_lib, "GetAbsoluteSize");
    if (!with_geometry(at) || !with_geometry(extent)) return false;
    at.Set("LocalCoordinate", Vector2D{0, 0});
    if (!at.Invoke() || !extent.Invoke()) return false;
    Vector2D a = at.Result<Vector2D>(), e = extent.Result<Vector2D>();
    if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(e.x) || !std::isfinite(e.y) || e.x < 0 ||
        e.y < 0)
        return false;
    *out = {a.x, a.y, a.x + e.x, a.y + e.y};
    return true;
}

struct MeasureWalk {
    Rect screen{0, 0, static_cast<double>(kDrawW), static_cast<double>(kDrawH)};
    Rect box;
    bool any = false;
    int asked = 0;    // widgets whose geometry was read
    int counted = 0;  // parts that make up the box
    int wide = 0;     // full-width or full-height strips left out
};

bool InvisibleImage(UObject w) {
    if (!L.image_class || L.image_brush < 0 || !IsA(w, L.image_class)) return false;
    BrushLook b = ReadBrush(w, L.image_brush, L.image_color);
    return !b.drawn || b.alpha < 0.02f;
}

// Adds the parts of `w` that show something to the box. Buttons, tab pages
// and other parts built as their own widget count as one piece (one engine
// read each), a scrolling list as the part of it you can see, and of a
// switcher only the page it shows. Anything as big as the screen is a layer
// (a background, or an overlay holding the real content): looked inside if
// it is a widget of its own, else left out. So are strips running the whole
// width or height (lines and bars along an edge), which would stop any zoom.
void Measure(UObject w, int depth, MeasureWalk& m) {
    if (!w || depth > 24 || m.asked > 400) return;
    uint8_t vis = Visibility(w);
    if (vis == kCollapsed || vis == kHidden) return;
    const bool user = IsA(w, L.user_widget_class);
    const bool scroll = !user && L.scroll_class && IsA(w, L.scroll_class);
    const bool panel = !user && !scroll && IsA(w, L.panel_class);
    if (panel) {
        if (L.switcher_class && IsA(w, L.switcher_class)) {
            ue::Call a(w, "GetActiveWidget");
            if (a.Invoke() && a.Result<UObject>()) Measure(a.Result<UObject>(), depth + 1, m);
            return;
        }
        std::vector<UObject> kids;
        Children(w, 256, &kids);
        for (UObject k : kids) Measure(k, depth + 1, m);
        return;
    }
    if (!user && ((L.spacer_class && IsA(w, L.spacer_class)) || InvisibleImage(w))) return;
    Rect r;
    ++m.asked;
    if (!WidgetRect(w, &r)) return;
    if (r.W() <= 1 || r.H() <= 1) return;  // never drawn
    const Rect& s = m.screen;
    if (r.x1 <= s.x0 || r.y1 <= s.y0 || r.x0 >= s.x1 || r.y0 >= s.y1) return;  // off the screen
    if (r.W() >= s.W() * 0.9 && r.H() >= s.H() * 0.9) {
        if (user)
            if (UObject root = TreeRoot(w)) Measure(root, depth + 1, m);
        return;
    }
    if (r.W() >= s.W() * 0.95 || r.H() >= s.H() * 0.95) {
        ++m.wide;
        return;
    }
    r = {std::max(r.x0, s.x0), std::max(r.y0, s.y0), std::min(r.x1, s.x1), std::min(r.y1, s.y1)};
    if (!m.any) m.box = r;
    else m.box = {std::min(m.box.x0, r.x0), std::min(m.box.y0, r.y0), std::max(m.box.x1, r.x1),
                  std::max(m.box.y1, r.y1)};
    m.any = true;
    ++m.counted;
}

// The menu's whole layout has been drawn on its panel: its own top widget
// then fills the panel exactly. Before that (it was just moved there), the
// geometry is the flat screen's and means nothing here.
bool DrawnOnPanel(const Panel& p, Rect* whole) {
    if (!WidgetRect(p.widget, whole)) return false;
    return std::fabs(whole->x0) < 2 && std::fabs(whole->y0) < 2 && std::fabs(whole->W() - kDrawW) < 2 &&
           std::fabs(whole->H() - kDrawH) < 2;
}

bool MeasureMenu(const Panel& p, Rect* box, MeasureWalk* out) {
    WalkScope walk;
    MeasureWalk m;
    Measure(p.widget, 0, m);
    *out = m;
    if (!m.any) return false;
    // Clamp to the screen and keep a small border so nothing touches the edge.
    *box = {std::max(0.0, m.box.x0 - 24), std::max(0.0, m.box.y0 - 24), std::min(m.screen.x1, m.box.x1 + 24),
            std::min(m.screen.y1, m.box.y1 + 24)};
    return box->W() >= 80 && box->H() >= 40;
}

// Called each frame while a new menu waits to be fitted. Returns true once
// it is done (fitted, or shown whole if it can't be measured).
// The zoom and centre for a menu whose buttons fill `box` of the layout.
void ApplyFit(Panel& p, const Rect& box) {
    const double W = kDrawW, H = kDrawH;
    double zoom = std::min(W / box.W(), H / box.H());
    if (S.fit_shape) {
        // The same area on the arm as the whole screen had, shaped like the
        // menu: the pause menu (a narrow column) was held to the screen's
        // height and came out small; now it gets a taller, narrower panel.
        const double aspect = box.W() / box.H();
        const double fw = std::min(std::sqrt(W * H * aspect), W * 1.25);
        const double fh = std::min(std::sqrt(W * H / aspect), H * 1.5);
        zoom = std::max(zoom, std::min(fw / box.W(), fh / box.H()));
    }
    // Menus that fill the whole screen (death screen, Options, credits,
    // task cards) could hardly be zoomed at all and their print stayed tiny
    // (owner 2026-11-03): they get a panel at least this much bigger.
    // Not the death screen: the owner wants its size as it was (2026-11-03,
    // "they dont need to be bigger"; its trouble was brightness).
    if (p.name != "UIM_Results_C") zoom = std::max(zoom, static_cast<double>(S.fit_min_zoom));
    zoom = std::clamp(zoom, 1.0, static_cast<double>(S.fit_max_zoom));
    p.zoom = zoom;
    p.cx = (box.x0 + box.x1) * 0.5;
    p.cy = (box.y0 + box.y1) * 0.5;
    p.half_w = box.W() * 0.5;
    p.half_h = box.H() * 0.5;
    // The panel is placed so this point sits on the arm (PanelOrigin).
}

bool FitContent(Panel& p) {
    if (!S.fit || !L.slate_lib) return true;
    Rect whole;
    if (!DrawnOnPanel(p, &whole)) {
        if (Now() - p.hosted_at < 1.0) return false;  // not drawn on its panel yet: next frame
        log::Info("world menu: %s: not laid out on its panel after 1 s (%.0fx%.0f at %.0f,%.0f); shown whole",
                  p.name.c_str(), whole.W(), whole.H(), whole.x0, whole.y0);
        return true;
    }
    double t0 = perf::NowMs();
    Rect box;
    MeasureWalk m;
    if (!MeasureMenu(p, &box, &m)) {
        log::Info("world menu: %s: nothing to measure (%d widgets read, %d parts); shown whole", p.name.c_str(),
                  m.asked, m.counted);
        return true;
    }
    ApplyFit(p, box);
    log::Info("world menu: %s: its parts fill %.0f,%.0f to %.0f,%.0f of the screen; zoomed %.2fx on the arm "
              "(%d parts, %d edge strips left out, %d widgets read, %.1f ms)",
              p.name.c_str(), box.x0, box.y0, box.x1, box.y1, p.zoom, m.counted, m.wide, m.asked,
              perf::NowMs() - t0);
    return true;
}

// After a click the menu may show other parts (another Options tab): the
// laser's area grows to take them in. The zoom stays as it was, so the menu
// never jumps in size under your hand. Only in the first seconds after it
// opened, when the game is still filling it (the task giver's cards came a
// moment after the menu, and it was zoomed onto the first card alone), a
// menu that turned out much bigger is fitted again.
void GrowHitArea(Panel& p) {
    if (!S.fit || p.fit_pending) return;
    Rect box;
    MeasureWalk m;
    if (!MeasureMenu(p, &box, &m)) return;
    if (Now() - p.hosted_at < 3.0 && (box.W() > p.half_w * 2 * 1.15 || box.H() > p.half_h * 2 * 1.15)) {
        double was = p.zoom;
        ApplyFit(p, box);
        PosePanel(p);
        log::Info("world menu: %s: the game filled it after it opened (parts now %.0f,%.0f to %.0f,%.0f): fitted "
                  "again, zoom %.2fx -> %.2fx",
                  p.name.c_str(), box.x0, box.y0, box.x1, box.y1, was, p.zoom);
        return;
    }
    double hw = std::max({p.half_w, p.cx - box.x0, box.x1 - p.cx});
    double hh = std::max({p.half_h, p.cy - box.y0, box.y1 - p.cy});
    if (hw > p.half_w + 1 || hh > p.half_h + 1)
        log::Info("world menu: %s: laser area grown to %.0fx%.0f for parts shown after a click", p.name.c_str(),
                  hw * 2, hh * 2);
    p.half_w = hw;
    p.half_h = hh;
}

// While a menu waits to be measured (a frame or two) it is drawn see-through,
// so it never shows at the old size and then jumps.
// The colour part of the tint is the menus' brightness (UpdateBrightness).
float g_menu_gain = 1.f;  // game thread

void SetPanelOpacity(UObject comp, float a) {
    ue::Call c(comp, "SetTintColorAndOpacity");
    c.Set("NewTintColorAndOpacity", LinearColor{g_menu_gain, g_menu_gain, g_menu_gain, a});
    c.Invoke();
}

// ------------------------------------------------------- outlined text --
// Menus on the arm have no backdrop (the owner never wants one), so their
// text is drawn straight over the world. Over a bright scene, white text
// was unreadable (owner 2026-11-03, death screen: "i cant read the text").
// Every text in a menu on the arm gets a thin black outline, like subtitles,
// unless the game gave it an outline of its own.
int OutlineText(const std::vector<UObject>& texts) {
    if (S.text_outline <= 0 || L.text_font < 0) return 0;
    int n = 0;
    std::vector<uint8_t> font(static_cast<size_t>(L.font_size));
    for (UObject t : texts) {
        if (!mem::SafeCopy(font.data(), reinterpret_cast<const void*>(t + L.text_font), font.size())) continue;
        int32_t cur = 0;
        std::memcpy(&cur, font.data() + L.outline_size, sizeof(cur));
        if (cur > 0) continue;
        ue::Call c(t, "SetFont");
        int32_t want = 0;
        uint8_t* dst = c.Raw("InFontInfo", &want);
        if (!dst || want != L.font_size) {
            log::Warn("world menu: TextBlock.SetFont takes %d bytes, the font is %d; text keeps no outline", want,
                      L.font_size);
            L.text_font = -1;
            return n;
        }
        std::memcpy(dst, font.data(), font.size());
        int32_t size = S.text_outline;
        LinearColor black{0.f, 0.f, 0.f, 1.f};
        std::memcpy(dst + L.outline_size, &size, sizeof(size));
        std::memcpy(dst + L.outline_color, &black, sizeof(black));
        if (c.Invoke()) ++n;
    }
    return n;
}

// ----------------------------------------------------------- bigger text --
// Zooming a whole menu only goes so far on an arm: the game's menus fill most
// of a 1920x1080 screen with small print (owner 2026-11-03, after the zoom:
// "the text really is to small on the menu's we really gotta do something
// about it"; before: "larger text but they dont overlap, and they still fit
// the individual boxes"). So every text of a menu on the arm gets a bigger
// font (menu_text_scale), and the engine lays the menu out again. Then each
// text is checked where the engine put it: one that no longer fits the space
// it was given, runs out of a box around it, or now covers another text is
// pulled back a step, with the other texts in the same box, until all fit.
// Only after that is the menu zoomed onto the arm (FitContent).

float ReadFontSize(UObject t) {
    if (L.font_pt < 0 || L.text_font < 0) return 0.f;
    uintptr_t at = t + static_cast<uintptr_t>(L.text_font + L.font_pt);
    if (L.font_pt_float) return mem::ReadOr<float>(at, 0.f);
    return static_cast<float>(mem::ReadOr<int32_t>(at, 0));
}

bool WriteFontSize(UObject t, float size) {
    if (L.font_pt < 0 || L.text_font < 0) return false;
    std::vector<uint8_t> font(static_cast<size_t>(L.font_size));
    if (!mem::SafeCopy(font.data(), reinterpret_cast<const void*>(t + L.text_font), font.size())) return false;
    ue::Call c(t, "SetFont");
    int32_t want = 0;
    uint8_t* dst = c.Raw("InFontInfo", &want);
    if (!dst || want != L.font_size) return false;
    std::memcpy(dst, font.data(), font.size());
    if (L.font_pt_float) {
        std::memcpy(dst + L.font_pt, &size, sizeof(size));
    } else {
        int32_t whole = static_cast<int32_t>(std::lround(size));
        std::memcpy(dst + L.font_pt, &whole, sizeof(whole));
    }
    return c.Invoke();
}

// The size this mod gave each text, and the game's own size under it, so a
// menu shown again (the pause menu is the same widget every time) doesn't
// grow twice. If the game has set another size since, that is the new base.
std::unordered_map<UObject, std::pair<float, float>> g_font_sizes;  // text -> (ours, the game's)

float GameFontSize(UObject t) {
    float cur = ReadFontSize(t);
    auto it = g_font_sizes.find(t);
    if (it != g_font_sizes.end() && std::fabs(it->second.first - cur) < 0.01f) return it->second.second;
    return cur;
}

void SetTextScale(TextItem& t, float scale) {
    if (!Alive(t.obj, t.index) || t.base <= 0) return;
    float size = t.base * scale;
    if (WriteFontSize(t.obj, size)) {
        t.prev_scale = t.scale;
        t.scale = scale;
        if (g_font_sizes.size() > 20000) g_font_sizes.clear();
        g_font_sizes[t.obj] = {ReadFontSize(t.obj), t.base};
    }
}

// The widget a widget sits in: its slot's panel, or for the top widget of a
// button or other widget of its own, that widget (its tree's owner).
UObject ParentOf(UObject w) {
    if (L.widget_slot >= 0 && L.slot_parent >= 0) {
        UObject slot = mem::ReadOr<UObject>(w + L.widget_slot, 0);
        if (slot) return mem::ReadOr<UObject>(slot + L.slot_parent, 0);
    }
    UObject outer = ue::GetOuter(w);
    if (outer && L.tree_class && IsA(outer, L.tree_class)) return ue::GetOuter(outer);
    return 0;
}

// The widgets around a text, nearest first, up to the menu itself, and how
// many of them are boxes it must fit in: up to and including the first list
// (ScrollBox), since what is in a list may run past its ends (it scrolls).
// Every widget around it is checked; from a list up, only across the list
// (along it, its content scrolls). `boxes` stays the ones up to the first
// list: running out of one of those makes every text in it step back.
void TextChain(TextItem& t, UObject menu) {
    t.chain.clear();
    t.box_mode.clear();
    t.boxes = 0;
    bool in_list = false;
    uint8_t mode = kBoth;
    UObject w = t.obj;
    for (int i = 0; i < 32; ++i) {
        w = ParentOf(w);
        if (!w || w == menu) break;
        if (L.scroll_class && IsA(w, L.scroll_class)) {
            uint8_t orient = 1;  // EOrientation: 0 across the screen, 1 down it
            ue::GetProp(w, "Orientation", orient);
            uint8_t across = orient == 0 ? kAlong : kAcross;
            mode = mode == kBoth || mode == across ? across : kNone;
            if (!in_list) t.boxes = t.chain.size() + 1;
            in_list = true;
        }
        t.chain.push_back(w);
        t.box_mode.push_back(mode);
        if (!in_list) t.boxes = t.chain.size();
    }
}

// Whether a text is on the screen right now: it and everything around it
// visible, and on the page each switcher shows. A tab you left keeps the
// places the engine last drew it in, which would read as texts covering the
// new tab's texts.
bool ShownNow(const TextItem& t, std::unordered_map<UObject, UObject>& active) {
    auto hidden = [](UObject w) {
        uint8_t v = Visibility(w);
        return v == kCollapsed || v == kHidden;
    };
    if (hidden(t.obj)) return false;
    // Drawn fully see-through (the game's "Lock" label on its buttons is a
    // big invisible text that used to make the real label shrink).
    double alpha = 1.0;
    if (L.text_color >= 0) alpha = mem::ReadOr<LinearColor>(t.obj + L.text_color, LinearColor{1, 1, 1, 1}).a;
    if (L.render_opacity >= 0) alpha *= mem::ReadOr<float>(t.obj + L.render_opacity, 1.f);
    if (alpha < 0.05) return false;
    UObject below = t.obj;
    for (UObject w : t.chain) {
        if (hidden(w)) return false;
        if (L.render_opacity >= 0) {
            alpha *= mem::ReadOr<float>(w + L.render_opacity, 1.f);
            if (alpha < 0.05) return false;
        }
        if (L.switcher_class && IsA(w, L.switcher_class)) {
            auto it = active.find(w);
            if (it == active.end()) {
                ue::Call a(w, "GetActiveWidget");
                it = active.emplace(w, a.Invoke() ? a.Result<UObject>() : 0).first;
            }
            if (it->second && it->second != below) return false;
        }
        below = w;
    }
    return true;
}

// Where a widget was drawn (layout pixels), its size in its own units and
// the size it would like (for a text: what its letters need).
struct Placed {
    Rect r;
    double local_w = 0, local_h = 0, want_w = 0, want_h = 0;
    bool ok = false;
};

Placed PlaceOf(UObject w, bool want) {
    Placed out;
    if (!L.slate_lib || !w) return out;
    ue::Call g(w, "GetCachedGeometry");
    if (!g.Invoke()) return out;
    int32_t size = 0;
    const uint8_t* geo = g.Raw("ReturnValue", &size);
    if (!geo || size <= 0) return out;
    auto with_geometry = [&](ue::Call& c) {
        int32_t need = 0;
        uint8_t* dst = c.Raw("Geometry", &need);
        if (!dst || need != size) return false;
        std::memcpy(dst, geo, static_cast<size_t>(size));
        return true;
    };
    ue::Call at(L.slate_lib, "LocalToAbsolute");
    ue::Call extent(L.slate_lib, "GetAbsoluteSize");
    if (!with_geometry(at) || !with_geometry(extent)) return out;
    at.Set("LocalCoordinate", Vector2D{0, 0});
    if (!at.Invoke() || !extent.Invoke()) return out;
    Vector2D a = at.Result<Vector2D>(), e = extent.Result<Vector2D>();
    if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(e.x) || !std::isfinite(e.y) || e.x < 0 ||
        e.y < 0)
        return out;
    out.r = {a.x, a.y, a.x + e.x, a.y + e.y};
    out.ok = true;
    if (want) {
        ue::Call local(L.slate_lib, "GetLocalSize");
        ue::Call desired(w, "GetDesiredSize");
        if (with_geometry(local) && local.Invoke() && desired.Invoke()) {
            Vector2D l = local.Result<Vector2D>(), d = desired.Result<Vector2D>();
            if (std::isfinite(l.x) && std::isfinite(l.y) && std::isfinite(d.x) && std::isfinite(d.y)) {
                out.local_w = l.x;
                out.local_h = l.y;
                out.want_w = d.x;
                out.want_h = d.y;
            }
        }
    }
    return out;
}

bool Drawn(const Rect& r) { return r.W() > 1 && r.H() > 1; }

// How far `r` runs past box `b` (pixels; 0 or less is inside). A list only
// counts across: along it, its content scrolls.
double RunsPast(const Rect& r, const Rect& b, uint8_t mode = kBoth) {
    if (mode == kNone) return -1e9;
    double x = std::max(b.x0 - r.x0, r.x1 - b.x1), y = std::max(b.y0 - r.y0, r.y1 - b.y1);
    return mode == kAcross ? x : mode == kAlong ? y : std::max(x, y);
}

double Overlap(const Rect& a, const Rect& b) {
    double w = std::min(a.x1, b.x1) - std::max(a.x0, b.x0);
    double h = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
    return w > 0 && h > 0 ? w * h : 0.0;
}

// True if two boxes cross by more than `px` both ways (a real overlap, not
// two edges touching).
bool Crosses(const Rect& a, const Rect& b, double px = 3.0) {
    double w = std::min(a.x1, b.x1) - std::max(a.x0, b.x0);
    double h = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
    return w > px && h > px;
}

// How big a text the game drew at `base` is made: everything
// menu_text_scale times bigger, small print up to menu_text_min_px (the
// death screen's 12-point lines were what the owner still couldn't read,
// 2026-11-03), titles no bigger than menu_text_max_px (they are big
// already, and the room they leave goes to the small print), and nothing
// past menu_text_max_scale times the game's size.
float TargetScale(float base) {
    if (base <= 0) return 1.f;
    double size = std::max<double>(S.text_min_px, base * S.text_scale);
    size = std::min(size, std::max<double>(base, S.text_max_px));
    size = std::min(size, base * static_cast<double>(S.text_max_scale));
    return static_cast<float>(std::max(1.0, size / base));
}

// Adds the texts not yet known to the panel (all of them when it was just put
// on the arm; later, the ones the game added). A text this mod made bigger
// before (the pause menu is the same widget every time) goes back to the
// game's size first, so it is measured as the game laid it out; *reset says
// whether any did.
int AddTexts(Panel& p, const std::vector<UObject>& texts, bool* reset = nullptr) {
    if (S.text_scale <= 1.001f || L.font_pt < 0) return 0;
    std::unordered_set<UObject> known;
    for (const auto& k : p.texts) known.insert(k.obj);
    int added = 0;
    for (UObject t : texts) {
        if (known.count(t) || p.texts.size() >= 1500) continue;
        TextItem item;
        item.obj = t;
        item.index = IndexOf(t);
        item.base = GameFontSize(t);
        if (item.base < 1.f || item.base > 400.f) continue;
        item.scale = ReadFontSize(t) / item.base;
        if (std::fabs(item.scale - 1.f) > 0.01f) {
            SetTextScale(item, 1.f);
            if (reset) *reset = true;
        }
        known.insert(t);
        p.texts.push_back(item);
        ++added;
    }
    return added;
}

// The pictures a menu shows now, where the engine drew them. Whole-screen
// layers are left out (everything is inside those).
bool LooksLikeButton(UObject w) {
    std::string cls = Lower(ue::GetName(ue::GetClass(w)));
    return cls.find("button") != std::string::npos || cls.find("btn") != std::string::npos;
}

void CollectPictures(Panel& p) {
    p.pictures.clear();
    if (!L.image_class) return;
    std::vector<std::pair<UObject, int>> todo;
    std::vector<UObject> kids;
    if (UObject root = TreeRoot(p.widget)) todo.push_back({root, 0});
    int seen = 0;
    while (!todo.empty() && seen < 1500 && p.pictures.size() < 200) {
        auto [w, depth] = todo.back();
        todo.pop_back();
        ++seen;
        uint8_t v = Visibility(w);
        if (v == kCollapsed || v == kHidden) continue;
        if (L.render_opacity >= 0 && mem::ReadOr<float>(w + L.render_opacity, 1.f) < 0.05f) continue;
        if (IsA(w, L.image_class)) {
            if (InvisibleImage(w)) continue;
            Placed pl = PlaceOf(w, false);
            if (!pl.ok || !Drawn(pl.r) || (pl.r.W() >= kDrawW * 0.9 && pl.r.H() >= kDrawH * 0.9)) continue;
            p.pictures.push_back({w, IndexOf(w), pl.r});
            continue;
        }
        // Drawn boxes count too: a filled border, and each button (a widget of
        // its own named like one). A label inside one must stay inside it, and
        // other text must not run into it (the level select's "Return Home"
        // ran out of its button under the preview picture; the death screen's
        // total ran into Retry, 2026-11-03).
        const bool border = L.border_class && L.border_brush >= 0 && IsA(w, L.border_class);
        const bool button = !border && IsA(w, L.user_widget_class) && LooksLikeButton(w);
        if (border || button) {
            bool drawn = true;
            if (border) {
                BrushLook b = ReadBrush(w, L.border_brush, L.border_color);
                drawn = b.drawn && b.alpha >= 0.02f;
            }
            Placed pl = drawn ? PlaceOf(w, false) : Placed{};
            if (pl.ok && Drawn(pl.r) && !(pl.r.W() >= kDrawW * 0.9 && pl.r.H() >= kDrawH * 0.9))
                p.pictures.push_back({w, IndexOf(w), pl.r});
        }
        if (depth >= 24) continue;
        if (IsA(w, L.user_widget_class)) {
            if (UObject root = TreeRoot(w)) todo.push_back({root, depth + 1});
        } else if (L.switcher_class && IsA(w, L.switcher_class)) {
            ue::Call a(w, "GetActiveWidget");
            if (a.Invoke() && a.Result<UObject>()) todo.push_back({a.Result<UObject>(), depth + 1});
        } else if (IsA(w, L.panel_class)) {
            Children(w, 256, &kids);
            for (UObject k : kids) todo.push_back({k, depth + 1});
        }
    }
}

// Measures the texts the panel shows at the game's size, then makes those
// bigger. A text that isn't on the screen yet (another tab, a line the death
// screen hasn't shown yet) stays at the game's size until it is: TextWatch
// measures and grows it then. Growing texts nobody had measured left them
// unchecked, and the level menu's and death screen's later texts came out
// over each other (owner 2026-11-03: "some text overlap in the top left").
// Returns how many it made bigger.
int GrowTexts(Panel& p) {
    std::unordered_map<UObject, Rect> boxes;
    auto box_rect = [&](UObject w) {
        auto it = boxes.find(w);
        if (it != boxes.end()) return it->second;
        Placed pl = PlaceOf(w, false);
        Rect r = pl.ok ? pl.r : Rect{};
        boxes.emplace(w, r);
        return r;
    };
    std::unordered_map<UObject, UObject> active;
    for (auto& t : p.texts) {
        if (!Alive(t.obj, t.index)) continue;
        if (t.chain.empty()) TextChain(t, p.widget);
        if (t.measured || std::fabs(t.scale - 1.f) > 0.01f || !ShownNow(t, active)) continue;
        Placed pl = PlaceOf(t.obj, true);
        if (!pl.ok || !Drawn(pl.r)) continue;  // not on the screen yet: TextWatch looks again
        t.measured = true;
        t.before = pl.r;
        t.base_want_h = pl.want_h;
        t.prev_scale = t.scale;
        t.base_over.clear();
        for (size_t k = 0; k < t.chain.size(); ++k) {
            Rect br = box_rect(t.chain[k]);
            t.base_over.push_back(Drawn(br) ? RunsPast(pl.r, br, t.box_mode[k]) : 0.0);
        }
    }
    CollectPictures(p);
    int n = 0;
    for (auto& t : p.texts) {
        if (!t.measured || t.grown || t.scale > 1.01f || !Alive(t.obj, t.index)) continue;
        t.grown = true;
        // The death screen keeps the game's text sizes: the owner wants its
        // buttons no bigger (2026-11-03, "they dont need to be bigger"; its
        // trouble was brightness), and its grown total ran into Retry.
        if (p.name == "UIM_Results_C") continue;
        float s = TargetScale(t.base);
        if (s <= 1.01f) continue;
        SetTextScale(t, s);
        ++n;
    }
    return n;
}

double Area(const Rect& r) { return std::max(0.0, r.W()) * std::max(0.0, r.H()); }

// One check after the engine has laid the menu out with the bigger texts.
// Returns how many texts were pulled back this round (0 = all fit), or -1
// if the engine hasn't laid the new sizes out yet (look again next frame:
// checking the old layout passed texts that then ran out of their buttons).
int CheckTexts(Panel& p, bool as_is) {
    std::unordered_map<UObject, Rect> boxes;
    auto box_rect = [&](UObject w) {
        auto it = boxes.find(w);
        if (it != boxes.end()) return it->second;
        Placed pl = PlaceOf(w, false);
        Rect r = pl.ok ? pl.r : Rect{};
        boxes.emplace(w, r);
        return r;
    };
    struct Seen {
        Rect r;
        bool drawn = false;
    };
    std::vector<Seen> now(p.texts.size());
    std::vector<char> pull(p.texts.size(), 0);
    std::vector<UObject> tight;  // boxes something ran out of: everything in them steps back
    std::unordered_map<UObject, UObject> active;
    std::vector<Placed> placed(p.texts.size());
    int stale = 0;
    for (size_t i = 0; i < p.texts.size(); ++i) {
        TextItem& t = p.texts[i];
        if (!Alive(t.obj, t.index)) continue;
        if (t.chain.empty()) TextChain(t, p.widget);
        if (!ShownNow(t, active)) continue;
        const bool grown = t.scale > 1.01f;
        Placed pl = PlaceOf(t.obj, grown);
        if (!pl.ok || !Drawn(pl.r)) continue;
        placed[i] = pl;
        if (grown && t.base_want_h > 1 && pl.want_h > 1 && std::fabs(t.prev_scale - t.scale) > 0.01f) {
            const double was = t.base_want_h * t.prev_scale, want = t.base_want_h * t.scale;
            if (std::fabs(pl.want_h - was) < std::fabs(pl.want_h - want)) ++stale;
        }
    }
    if (stale > 0 && !as_is) return -1;
    std::vector<Rect> pic_now(p.pictures.size());
    for (size_t j = 0; j < p.pictures.size(); ++j) {
        const Picture& pic = p.pictures[j];
        if (!Alive(pic.obj, pic.index)) continue;
        Placed pp = PlaceOf(pic.obj, false);
        if (pp.ok) pic_now[j] = pp.r;
    }
    for (size_t i = 0; i < p.texts.size(); ++i) {
        TextItem& t = p.texts[i];
        const Placed& pl = placed[i];
        if (!pl.ok) continue;
        t.prev_scale = t.scale;  // the engine has laid it out at this size
        const bool grown = t.scale > 1.01f;
        now[i] = {pl.r, true};  // texts at the game's size still count for covering
        if (!grown) continue;
        t.want_w = pl.want_w;
        t.want_h = pl.want_h;
        // Its letters need more room than its slot gives: they spill out.
        if (pl.local_w > 1 && (pl.want_w > pl.local_w + 1.0 || pl.want_h > pl.local_h + 1.0)) pull[i] = 1;
        for (size_t k = 0; k < t.chain.size() && k < t.box_mode.size(); ++k) {
            Rect br = box_rect(t.chain[k]);
            if (!Drawn(br)) continue;
            double before = k < t.base_over.size() ? t.base_over[k] : 0.0;
            if (RunsPast(pl.r, br, t.box_mode[k]) > std::max(before, 0.0) + 2.0) {
                pull[i] = 1;
                if (k < t.boxes) tight.push_back(t.chain[k]);
                break;
            }
        }
        // Off the edge of the screen (the credits' line of names ran past it).
        if (t.measured) {
            const Rect screen{0, 0, static_cast<double>(kDrawW), static_cast<double>(kDrawH)};
            if (RunsPast(pl.r, screen) > std::max(RunsPast(t.before, screen), 0.0) + 2.0) pull[i] = 1;
        }
        for (size_t j = 0; j < p.pictures.size() && !pull[i] && t.measured; ++j) {
            const Rect& was_r = p.pictures[j].before;
            const Rect& is_r = pic_now[j];
            if (!Drawn(is_r)) continue;
            if (RunsPast(t.before, was_r) <= 0.5) {  // it sat inside this picture: it stays inside
                if (RunsPast(pl.r, is_r) > 2.0) pull[i] = 1;
                continue;
            }
            if (!Crosses(t.before, was_r)) {  // clear of it at the game's size: stays clear
                if (Crosses(pl.r, is_r)) pull[i] = 1;
                continue;
            }
            double share = Overlap(pl.r, is_r) / std::max(1.0, std::min(Area(pl.r), Area(is_r)));
            double was = Overlap(t.before, was_r) / std::max(1.0, std::min(Area(t.before), Area(was_r)));
            if (share > was + 0.08) pull[i] = 1;
        }
    }
    // Two texts that now cover each other more than they did at the game's
    // size. Measured as a share of the smaller one, so two texts the game
    // stacks on purpose (a glow under a title) don't count just for growing.
    for (size_t i = 0; i < p.texts.size(); ++i) {
        if (!now[i].drawn) continue;
        for (size_t j = i + 1; j < p.texts.size(); ++j) {
            if (!now[j].drawn) continue;
            double o = Overlap(now[i].r, now[j].r);
            if (o <= 0) continue;
            double share = o / std::max(1.0, std::min(Area(now[i].r), Area(now[j].r)));
            if (share < 0.08) continue;
            const TextItem &a = p.texts[i], &b = p.texts[j];
            double was = 0;
            if (a.measured && b.measured)
                was = Overlap(a.before, b.before) / std::max(1.0, std::min(Area(a.before), Area(b.before)));
            if (share <= was + 0.08) continue;
            pull[i] = pull[j] = 1;
        }
    }
    // Bigger texts pushed a picture or box into another one (the death
    // screen's total line slid under Retry): every grown text steps back.
    bool pushed = false;
    for (size_t a = 0; a < p.pictures.size() && !pushed; ++a) {
        if (!Drawn(pic_now[a])) continue;
        for (size_t b = a + 1; b < p.pictures.size() && !pushed; ++b) {
            if (!Drawn(pic_now[b])) continue;
            if (Crosses(p.pictures[a].before, p.pictures[b].before)) continue;
            if (Crosses(pic_now[a], pic_now[b])) pushed = true;
        }
    }
    if (pushed)
        for (size_t i = 0; i < p.texts.size(); ++i)
            if (now[i].drawn && p.texts[i].scale > 1.01f) pull[i] = 1;
    for (size_t i = 0; i < p.texts.size() && !tight.empty(); ++i) {
        if (pull[i] || !now[i].drawn) continue;
        const TextItem& t = p.texts[i];
        for (UObject b : tight)
            for (size_t k = 0; k < t.boxes && !pull[i]; ++k)
                if (t.chain[k] == b) pull[i] = 1;
    }
    int n = 0;
    for (size_t i = 0; i < p.texts.size(); ++i) {
        TextItem& t = p.texts[i];
        if (!pull[i] || t.scale <= 1.01f) continue;
        float next = t.scale * 0.85f;
        SetTextScale(t, next < 1.05f ? 1.f : next);
        t.want_w = t.want_h = 0;  // laid out again before it is compared
        ++n;
    }
    return n;
}

// Each text of a menu once per run (and once more after texts came into
// view), so an overlap the owner reports can be matched to its texts.
std::unordered_map<std::string, int> g_text_dumps;

void LogTexts(const Panel& p) {
    int& dumps = g_text_dumps[p.name];
    if (dumps >= 2) return;
    ++dumps;
    int lines = 0;
    std::unordered_map<UObject, UObject> active;
    for (const auto& t : p.texts) {
        if (lines >= 90) break;
        if (!Alive(t.obj, t.index) || !ShownNow(t, active)) continue;
        Placed pl = PlaceOf(t.obj, false);
        if (!pl.ok || !Drawn(pl.r)) continue;
        UObject owner = ue::GetOuter(ue::GetOuter(t.obj));  // text -> its widget tree -> the widget it belongs to
        ++lines;
        log::Info("world menu: %s: text %s (in %s): game size %.0f, now %.2fx, at %.0f,%.0f %.0fx%.0f%s", p.name.c_str(),
                  ue::GetName(t.obj).c_str(), owner ? ue::GetName(owner).c_str() : "?", t.base, t.scale, pl.r.x0,
                  pl.r.y0, pl.r.W(), pl.r.H(),
                  t.measured ? "" : " (not measured at the game's size)");
    }
}

// ------------------------------------------------------ parts moved apart --
// A few menus run into their own pictures in the game's layout: the level
// select's left column runs past its divider line toward the preview, and
// the credits' row of names sits on the logo's bottom edge. On the flat
// screen that passed; on the arm the owner reads it as clipped and
// overlapping (2026-11-03, both still there after the text checks). For a
// text and a picture or box it isn't in (a big one, or a long divider line:
// small icons beside their text are left alone), the widget holding the text
// just below the one they share is moved if it runs into the picture: by its
// render translation, the shortest way clear (left, right, up or down, at
// most 120 layout pixels), if that spot is on the screen and clear of
// everything else. Parts lined up on one edge move together.
// Rows of three or more texts and nothing else get their gaps widened to
// twice the text height (the credits' names ran together).

// Boxes that cross (more than 3 px both ways, or half of a thin line's width).
bool Touches(const Rect& a, const Rect& b) {
    double w = std::min(a.x1, b.x1) - std::max(a.x0, b.x0);
    double h = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
    double tw = std::min(3.0, 0.5 * std::min(a.W(), b.W())), th = std::min(3.0, 0.5 * std::min(a.H(), b.H()));
    return w > tw && h > th;
}

bool WidenRowGaps(Panel& p) {
    if (!L.text_class || L.widget_slot < 0) return false;
    std::unordered_map<UObject, std::vector<std::pair<Rect, UObject>>> rows;
    std::unordered_map<UObject, UObject> active;
    for (auto& t : p.texts) {
        if (!Alive(t.obj, t.index) || !ShownNow(t, active)) continue;
        UObject parent = ParentOf(t.obj);
        if (!parent || ue::GetName(ue::GetClass(parent)) != "HorizontalBox") continue;
        Placed pl = PlaceOf(t.obj, false);
        if (pl.ok && Drawn(pl.r)) rows[parent].push_back({pl.r, t.obj});
    }
    struct Margin {
        float left, top, right, bottom;
    };
    bool any = false;
    for (auto& [row, items] : rows) {
        std::vector<UObject> kids;
        Children(row, 64, &kids);
        if (items.size() < 3 || kids.size() != items.size()) continue;  // only texts in it
        std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.first.x0 < b.first.x0; });
        int widened = 0;
        double added = 0;
        for (size_t i = 1; i < items.size(); ++i) {
            const Rect &a = items[i - 1].first, &b = items[i].first;
            double gap = b.x0 - a.x1, want = 2.0 * std::max(a.H(), b.H());
            if (gap <= 4 || gap >= want) continue;  // touching on purpose (a role and its name), or wide enough
            UObject slot = mem::ReadOr<UObject>(items[i].second + L.widget_slot, 0);
            Margin m{};
            if (!slot || !ue::GetProp(slot, "Padding", m)) continue;
            m.left += static_cast<float>(want - gap);
            ue::Call c(slot, "SetPadding");
            c.Set("InPadding", m);
            if (!c.Invoke()) continue;
            ++widened;
            added += want - gap;
        }
        if (widened)
            log::Info("world menu: %s: %d gap(s) in a row of %zu texts widened to twice the text height (+%.0f px)",
                      p.name.c_str(), widened, items.size(), added);
        any = any || widened > 0;
    }
    return any;
}

// Returns true if anything was changed (the menu is laid out again before
// it is measured for its zoom).
bool SeparateParts(Panel& p) {
    if (!S.separate || p.separated || p.name == "UIM_Results_C" || !L.slate_lib) return false;
    p.separated = true;
    bool changed = WidenRowGaps(p);
    auto up_of = [&](UObject w) {
        std::vector<UObject> up;
        for (UObject x = w; x && x != p.widget && up.size() < 40; x = ParentOf(x)) up.push_back(x);
        return up;
    };
    struct Thing {
        UObject w;
        std::vector<UObject> up;  // itself first, then each widget around it
        Rect r;
    };
    std::vector<Thing> texts, pics;
    std::unordered_map<UObject, UObject> active;
    for (auto& t : p.texts) {
        if (!Alive(t.obj, t.index) || !ShownNow(t, active)) continue;
        Placed pl = PlaceOf(t.obj, false);
        if (pl.ok && Drawn(pl.r)) texts.push_back({t.obj, up_of(t.obj), pl.r});
    }
    for (auto& pic : p.pictures) {
        if (!Alive(pic.obj, pic.index)) continue;
        Placed pl = PlaceOf(pic.obj, false);
        if (pl.ok && Drawn(pl.r)) pics.push_back({pic.obj, up_of(pic.obj), pl.r});
    }
    auto nested = [](const Rect& a, const Rect& b) { return RunsPast(a, b) <= 0.5 || RunsPast(b, a) <= 0.5; };
    auto inside = [](const Thing& th, UObject item) {
        return std::find(th.up.begin(), th.up.end(), item) != th.up.end();
    };
    // What to move: for a text and a picture, the widget holding the text
    // just below the one they share. It moves if it runs into the picture.
    struct Item {
        UObject w;
        Rect r;
        double factor;
        UObject hit;
        UObject leader = 0;  // moved along with this one (same column or row)
    };
    std::vector<Item> items;
    std::unordered_set<UObject> seen;
    auto big = [](const Rect& r) {
        return (r.W() >= 40 && r.H() >= 40) || (std::min(r.W(), r.H()) <= 6 && std::max(r.W(), r.H()) >= 100);
    };
    for (const Thing& t : texts) {
        for (const Thing& pic : pics) {
            if (inside(t, pic.w) || !big(pic.r)) continue;  // the text sits in this box, or it's an icon
            UObject item = t.up.back();
            for (size_t k = 1; k < t.up.size(); ++k)
                if (std::find(pic.up.begin(), pic.up.end(), t.up[k]) != pic.up.end()) {
                    item = t.up[k - 1];
                    break;
                }
            if (seen.count(item)) continue;
            Placed pl = PlaceOf(item, false);
            if (!pl.ok || !Drawn(pl.r) || (pl.r.W() >= kDrawW * 0.9 && pl.r.H() >= kDrawH * 0.9)) continue;
            if (nested(pl.r, pic.r) || !Touches(pl.r, pic.r)) continue;
            seen.insert(item);
            items.push_back({item, pl.r, pl.local_w > 1 ? pl.r.W() / pl.local_w : 1.0, pic.w});
        }
    }
    const Rect screen{0, 0, static_cast<double>(kDrawW), static_cast<double>(kDrawH)};
    struct Move {
        size_t item;
        double dx, dy;
    };
    std::vector<Move> moves;
    for (size_t i = 0; i < items.size(); ++i) {
        const Item& u = items[i];
        // Everything not inside the moving widget must stay clear of it.
        std::vector<Rect> others;
        for (const Thing& th : pics)
            if (!inside(th, u.w) && !nested(u.r, th.r)) others.push_back(th.r);
        for (const Thing& th : texts)
            if (!inside(th, u.w) && !nested(u.r, th.r)) others.push_back(th.r);
        auto blocked = [&](const Rect& r, Rect* hit) {
            for (const Rect& o : others)
                if (Touches(r, o)) {
                    *hit = o;
                    return true;
                }
            return false;
        };
        constexpr double kGap = 16, kMax = 120;
        double best_dx = 0, best_dy = 0, best = 1e9;
        for (int dir = 0; dir < 4; ++dir) {
            double dx = 0, dy = 0;
            for (int step = 0; step < 6; ++step) {
                Rect r{u.r.x0 + dx, u.r.y0 + dy, u.r.x1 + dx, u.r.y1 + dy};
                Rect o;
                if (!blocked(r, &o)) {
                    double d = std::fabs(dx) + std::fabs(dy);
                    if (step > 0 && d < best && d <= kMax && RunsPast(r, screen) <= 0) {
                        best = d;
                        best_dx = dx;
                        best_dy = dy;
                    }
                    break;
                }
                // Past what it ran into, never back the other way.
                const double was = dir < 2 ? dx : dy;
                if (dir == 0) dx = std::min(dx, o.x0 - u.r.x1 - kGap);
                if (dir == 1) dx = std::max(dx, o.x1 - u.r.x0 + kGap);
                if (dir == 2) dy = std::min(dy, o.y0 - u.r.y1 - kGap);
                if (dir == 3) dy = std::max(dy, o.y1 - u.r.y0 + kGap);
                if ((dir < 2 ? dx : dy) == was) break;
            }
        }
        if (best > 1e8) {
            log::Info("world menu: %s: %s runs into %s, but there's no clear spot within %.0f px to move it to",
                      p.name.c_str(), ue::GetName(u.w).c_str(), ue::GetName(u.hit).c_str(), kMax);
            continue;
        }
        moves.push_back({i, best_dx, best_dy});
    }
    // Parts lined up on the same edge move together (a column stays a column).
    for (auto& m : moves) {
        for (const auto& o : moves) {
            const Rect &a = items[m.item].r, &b = items[o.item].r;
            if (m.dx != 0 && o.dx != 0 && (m.dx < 0) == (o.dx < 0) && std::fabs(a.x0 - b.x0) <= 2 &&
                std::fabs(o.dx) > std::fabs(m.dx))
                m.dx = o.dx;
            if (m.dy != 0 && o.dy != 0 && (m.dy < 0) == (o.dy < 0) && std::fabs(a.y0 - b.y0) <= 2 &&
                std::fabs(o.dy) > std::fabs(m.dy))
                m.dy = o.dy;
        }
    }
    // The rest of a column (or row) goes with it: widgets beside a moved one,
    // in the same box and lined up on the same edge, if their new spot is
    // clear too (the level select's OFFICE and Return Home buttons under its
    // LEVEL title, which is what ran into the divider line).
    const size_t n_moves = moves.size();
    for (size_t k = 0; k < n_moves; ++k) {
        const Move m = moves[k];
        const Item u = items[m.item];
        UObject parent = ParentOf(u.w);
        if (!parent || !L.panel_class || !IsA(parent, L.panel_class)) continue;
        std::vector<UObject> kids;
        Children(parent, 64, &kids);
        for (UObject kid : kids) {
            if (kid == u.w || seen.count(kid)) continue;
            Placed pl = PlaceOf(kid, false);
            if (!pl.ok || !Drawn(pl.r)) continue;
            const bool col = m.dx != 0 && std::fabs(pl.r.x0 - u.r.x0) <= 2;
            const bool row = m.dy != 0 && std::fabs(pl.r.y0 - u.r.y0) <= 2;
            if (!col && !row) continue;
            const double dx = col ? m.dx : 0, dy = row ? m.dy : 0;
            const Rect to{pl.r.x0 + dx, pl.r.y0 + dy, pl.r.x1 + dx, pl.r.y1 + dy};
            if (RunsPast(to, screen) > 0) continue;
            auto stays = [&](const Thing& th) {  // not part of anything that moves
                if (inside(th, kid)) return false;
                for (const auto& mm : moves)
                    if (inside(th, items[mm.item].w)) return false;
                return !nested(pl.r, th.r);
            };
            bool clear = true;
            for (const Thing& th : pics)
                if (stays(th) && Touches(to, th.r)) clear = false;
            for (const Thing& th : texts)
                if (stays(th) && Touches(to, th.r)) clear = false;
            if (!clear) continue;
            seen.insert(kid);
            items.push_back({kid, pl.r, pl.local_w > 1 ? pl.r.W() / pl.local_w : 1.0, u.hit, u.w});
            moves.push_back({items.size() - 1, dx, dy});
        }
    }
    struct WidgetTransform {
        Vector2D translation, scale, shear;
        float angle;
    };
    for (const auto& m : moves) {
        const Item& u = items[m.item];
        WidgetTransform wt{};
        Vector2D now{0, 0};
        if (ue::GetProp(u.w, "RenderTransform", wt)) now = wt.translation;
        Vector2D to{now.x + m.dx / u.factor, now.y + m.dy / u.factor};
        ue::Call c(u.w, "SetRenderTranslation");
        c.Set("Translation", to);
        bool ok = c.Invoke();
        changed = changed || ok;
        log::Info("world menu: %s: %s (at %.0f,%.0f %.0fx%.0f) %s %s: moved %+.0f,%+.0f px%s", p.name.c_str(),
                  ue::GetName(u.w).c_str(), u.r.x0, u.r.y0, u.r.W(), u.r.H(), u.leader ? "lined up with" : "ran into",
                  ue::GetName(u.leader ? u.leader : u.hit).c_str(), m.dx, m.dy, ok ? "" : " (the engine refused)");
    }
    return changed;
}

// Runs each frame while texts are being grown. Returns true once they fit
// (or there is nothing to grow), so the menu can be zoomed.
bool TextStep(Panel& p) {
    if (p.text_phase == kTextOff) return true;
    if (p.text_wait > 0) {
        --p.text_wait;
        return false;
    }
    if (p.text_phase == kTextBaseline) {
        GrowTexts(p);
        p.text_phase = kTextCheck;
        p.text_round = 0;
        p.text_wait = 2;
        return false;
    }
    double t0 = perf::NowMs();
    if (p.text_round < 8) {
        int pulled = CheckTexts(p, p.stale_off);
        if (pulled < 0) {
            if (++p.text_stale < 40) return false;  // laid out with the new sizes in a frame or two
            // This used to accept the texts unchecked, which is how the
            // credits' names came out over the logo and past the screen edge.
            log::Info("world menu: %s: the engine didn't lay out the new text sizes in 40 frames; checking them as "
                      "they are laid out (and from now on)",
                      p.name.c_str());
            p.stale_off = true;
            pulled = CheckTexts(p, true);
        }
        p.text_stale = 0;
        p.text_pulled += pulled;
        ++p.text_round;
        if (pulled > 0) {
            // Laid out again with the smaller sizes before the next check (or,
            // after the last round, before the menu is measured for its zoom).
            p.text_wait = 2;
            return false;
        }
    }
    p.text_phase = kTextOff;
    // One more look a moment later, in case the engine laid something out
    // late; again after any look that had to pull a text back.
    if ((!p.quiet_check || p.text_pulled > 0) && p.rechecks < 6) {
        p.recheck_at = Now() + 0.6;
        ++p.rechecks;
    }
    if (p.quiet_check && p.text_pulled == 0) {  // nothing moved: nothing to say
        p.quiet_check = false;
        return true;
    }
    int grown = 0, measured = 0, waiting = 0, small = 0, small_grown = 0;
    double sum = 0;
    float base_min = 1e9f, base_max = 0;
    for (const auto& t : p.texts) {
        if (t.scale > 1.01f) {
            ++grown;
            sum += t.scale;
        }
        if (t.measured) ++measured;
        else if (!t.grown) ++waiting;
        if (t.measured && t.base < S.text_min_px) {
            ++small;
            if (t.base * t.scale >= S.text_min_px * 0.85f) ++small_grown;
        }
        base_min = std::min(base_min, t.base);
        base_max = std::max(base_max, t.base);
    }
    log::Info("world menu: %s: text made bigger%s: %d of %zu texts grown, %.2fx on average (game sizes %.0f-%.0f); "
              "%d of %d small texts now near %.0f px; %d measured on screen, %d not shown yet; %d step(s) back "
              "where a text didn't fit, %d rounds, last %.1f ms",
              p.name.c_str(), p.quiet_check ? " (looked again)" : "", grown, p.texts.size(),
              grown ? sum / grown : 1.0, p.texts.empty() ? 0.f : base_min,
              base_max, small_grown, small, S.text_min_px, measured, waiting, p.text_pulled, p.text_round,
              perf::NowMs() - t0);
    if (!p.quiet_check) LogTexts(p);
    p.quiet_check = false;
    return true;
}

// The texts of a page shown after a click (another Options tab), or ones the
// game added later (the death screen's kill list): the outline and the
// bigger font too. Returns true if texts are being grown (the laser area
// waits for them).
bool NewPageTexts(Panel& p) {
    if (!Alive(p.widget, p.widget_index)) return false;
    std::vector<UObject> blurs, texts;
    FindBlurs(p.widget, &blurs, &texts);
    for (UObject b : blurs) {
        if (Visibility(b) == kCollapsed) continue;
        ue::Call v(b, "SetVisibility");
        v.Set("InVisibility", kCollapsed);
        v.Invoke();
        log::Info("world menu: hid %s (a background blur on a new page)", ue::GetName(b).c_str());
    }
    std::unordered_set<UObject> known;
    for (const auto& k : p.texts) known.insert(k.obj);
    std::vector<UObject> fresh;
    for (UObject t : texts)
        if (!known.count(t)) fresh.push_back(t);
    if (fresh.empty()) return false;
    int outlined = OutlineText(fresh);
    bool reset = false;
    int added = AddTexts(p, fresh, &reset);
    log::Info("world menu: %s: %zu new texts on the page (%d outlined, %d to grow)", p.name.c_str(), fresh.size(),
              outlined, added);
    if (added <= 0) return false;
    p.text_phase = kTextBaseline;
    p.text_wait = reset ? 2 : 0;  // laid out at the game's sizes before they are measured
    p.text_pulled = 0;
    return true;
}

// Twice a second while a menu is on the arm and nothing is being grown:
// texts that came into view since (the death screen shows its lines one by
// one; a tab you opened) are measured and grown, and grown texts whose
// words changed (a count running up) are checked again. Returns 1 if texts
// are being grown, 2 if checked again, else 0.
int TextWatch(Panel& p) {
    if (p.texts.empty() || !L.slate_lib) return 0;
    std::unordered_map<UObject, UObject> active;
    int appeared = 0, changed = 0;
    for (auto& t : p.texts) {
        if (!Alive(t.obj, t.index)) continue;
        const bool waiting = !t.measured && !t.grown && std::fabs(t.scale - 1.f) <= 0.01f;
        const bool grown = t.scale > 1.01f;
        if (!waiting && !(grown && t.want_w > 0)) continue;
        if (t.chain.empty()) TextChain(t, p.widget);
        if (!ShownNow(t, active)) continue;
        if (waiting) {
            Placed pl = PlaceOf(t.obj, false);
            if (pl.ok && Drawn(pl.r)) ++appeared;
            continue;
        }
        ue::Call d(t.obj, "GetDesiredSize");
        if (!d.Invoke()) continue;
        Vector2D v = d.Result<Vector2D>();
        if (std::fabs(v.x - t.want_w) > 2.0 || std::fabs(v.y - t.want_h) > 2.0) ++changed;
    }
    p.text_pulled = 0;
    if (appeared > 0) {
        log::Info("world menu: %s: %d text(s) came into view: measured, then made bigger", p.name.c_str(), appeared);
        p.text_phase = kTextBaseline;
        p.text_wait = 0;
        p.quiet_check = false;
        return 1;
    }
    if (changed > 0) {
        p.text_phase = kTextCheck;
        p.text_round = 0;
        p.text_wait = 0;
        p.quiet_check = true;
        return 2;
    }
    return 0;
}

// ------------------------------------------------------- panel pictures --
// A picture of what a menu's panel shows (its whole 1920x1080 layout, not
// the headset view, which rarely has the arm in it), saved beside the log as
// panel-<menu>-<n>.exr, so a layout can be checked without asking the owner.
// The engine reads the picture back and packs it, a short hitch, so only a
// couple per menu and run (menu_snapshots).

struct FStringView {
    const wchar_t* data;
    int32_t num, max;
};

std::unordered_map<std::string, int> g_snapshots;

void SnapshotPanel(const Panel& p, const std::string& tag) {
    if (S.snapshots <= 0 || !L.krl || !Alive(p.comp, p.comp_index)) return;
    const std::string key = p.name + tag;
    int& n = g_snapshots[key];
    if (n >= S.snapshots) return;
    ue::Call rt(p.comp, "GetRenderTarget");
    UObject target = rt.Invoke() ? rt.Result<UObject>() : 0;
    if (!target) {
        log::Info("world menu: %s: no picture of its panel (it has no render target)", p.name.c_str());
        return;
    }
    ++n;
    // The engine writes this float render target as OpenEXR, whatever the name.
    const std::string file = "panel-" + key + "-" + std::to_string(n) + ".exr";
    std::wstring dir = config::Dir(), wfile(file.begin(), file.end());
    FStringView d{dir.c_str(), static_cast<int32_t>(dir.size() + 1), static_cast<int32_t>(dir.size() + 1)};
    FStringView f{wfile.c_str(), static_cast<int32_t>(wfile.size() + 1), static_cast<int32_t>(wfile.size() + 1)};
    ue::Call c(L.krl, "ExportRenderTarget");
    c.Set("WorldContextObject", p.comp).Set("TextureRenderTarget", target).Set("FilePath", d).Set("Filename", f);
    double t0 = perf::NowMs();
    bool ok = c.Invoke();
    double ms = perf::NowMs() - t0;
    // What landed on disk (the engine may pick its own file type).
    std::wstring pattern = dir + L"\\panel-" + std::wstring(key.begin(), key.end()) + L"-" + std::to_wstring(n) + L"*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    std::string found = "nothing written";
    if (h != INVALID_HANDLE_VALUE) {
        std::wstring w = fd.cFileName;
        found = std::string(w.begin(), w.end()) + " (" + std::to_string(fd.nFileSizeLow / 1024) + " KB)";
        FindClose(h);
    }
    log::Info("world menu: %s: picture of its panel%s: %s, %s, %.0f ms", p.name.c_str(), tag.c_str(),
              ok ? "taken" : "the engine call failed", found.c_str(), ms);
}

// ------------------------------------------------------- the kill shot --
// The death screen (UIM_Results_C) has pages (a WidgetSwitcher); one shows
// the kill shot (UIP_DeadReview): a picture the game takes of your body in a
// small room (LCBP_DeadReviewRoom) with its own scene capture. The owner
// couldn't see it on the arm (2026-11-03). Each time the page changes, the
// log says what the kill shot view is made of and whether its room takes a
// picture, and a picture of the panel is saved.

void DescribeKillShot(UObject menu, int page) {
    UObject review = 0;
    ue::GetProp(menu, "UIP_DeadReview", review);
    if (!review) {
        log::Info("world menu: death screen page %d: no kill shot view (UIP_DeadReview) in it", page);
        return;
    }
    // It and what it sits in: hidden parts, and the page each switcher shows.
    std::string chain;
    UObject below = review;
    for (UObject w = ParentOf(review); w && w != menu && chain.size() < 600; below = w, w = ParentOf(w)) {
        uint8_t v = Visibility(w);
        chain += " < " + ue::GetName(w);
        if (v == kCollapsed) chain += " (collapsed)";
        else if (v == kHidden) chain += " (hidden)";
        if (L.switcher_class && IsA(w, L.switcher_class)) {
            ue::Call a(w, "GetActiveWidget");
            UObject shown = a.Invoke() ? a.Result<UObject>() : 0;
            chain += shown == below ? " (showing it)" : " (showing " + (shown ? ue::GetName(shown) : "nothing") + ")";
        }
    }
    Rect r;
    bool placed = WidgetRect(review, &r);
    log::Info("world menu: death screen page %d: kill shot view %s: visibility %d, %s%s", page,
              ue::GetName(review).c_str(), Visibility(review),
              placed ? ("drawn at " + std::to_string(int(r.x0)) + "," + std::to_string(int(r.y0)) + " " +
                        std::to_string(int(r.W())) + "x" + std::to_string(int(r.H())))
                           .c_str()
                     : "never drawn",
              chain.c_str());
    // What it is made of.
    std::vector<std::pair<UObject, int>> todo;
    std::vector<UObject> kids;
    if (UObject root = TreeRoot(review)) todo.push_back({root, 0});
    int seen = 0;
    while (!todo.empty() && seen < 40) {
        auto [w, depth] = todo.back();
        todo.pop_back();
        ++seen;
        std::string what = ue::GetName(ue::GetClass(w)) + " " + ue::GetName(w);
        uint8_t v = Visibility(w);
        Rect wr;
        bool wp = WidgetRect(w, &wr);
        std::string extra;
        if (L.image_class && IsA(w, L.image_class) && L.image_brush >= 0) {
            BrushLook b = ReadBrush(w, L.image_brush, L.image_color);
            UObject res = mem::ReadOr<UObject>(w + L.image_brush + L.brush_resource, 0);
            extra = ", picture from " + (res ? ue::GetName(ue::GetClass(res)) + " " + ue::GetName(res) : "nothing") +
                    ", drawn " + std::to_string(int(b.alpha * 100)) + "% opaque" + (b.drawn ? "" : " (draw-as none)");
        }
        log::Info("world menu:   kill shot part %s: visibility %d, %s%s", what.c_str(), v,
                  wp ? (std::to_string(int(wr.x0)) + "," + std::to_string(int(wr.y0)) + " " +
                        std::to_string(int(wr.W())) + "x" + std::to_string(int(wr.H())))
                           .c_str()
                     : "never drawn",
                  extra.c_str());
        if (depth >= 8) continue;
        if (IsA(w, L.user_widget_class)) {
            if (UObject root = TreeRoot(w)) todo.push_back({root, depth + 1});
        } else if (IsA(w, L.panel_class)) {
            Children(w, 32, &kids);
            for (UObject k : kids) todo.push_back({k, depth + 1});
        }
    }
    // The rooms that take its pictures.
    for (const char* room_name : {"ShotRoom", "Room"}) {
        UObject room = 0;
        ue::GetProp(review, room_name, room);
        UObject cap = 0;
        if (room) ue::GetProp(room, "SceneCaptureComponent2D", cap);
        UObject target = 0;
        int32_t sx = 0, sy = 0;
        bool every = false;
        uint8_t source = 255;
        if (cap) {
            ue::GetProp(cap, "CaptureSource", source);
            ue::GetProp(cap, "TextureTarget", target);
            auto pi = ue::reflection::FindProperty(ue::GetClass(cap), "bCaptureEveryFrame");
            if (pi.valid()) every = (mem::ReadOr<uint8_t>(cap + pi.offset, 0) & (pi.bool_mask ? pi.bool_mask : 0xFF)) != 0;
        }
        if (target) {
            ue::GetProp(target, "SizeX", sx);
            ue::GetProp(target, "SizeY", sy);
        }
        log::Info("world menu:   kill shot %s: %s; its camera %s, picture into %s %dx%d (source %d), %s", room_name,
                  room ? ue::GetName(room).c_str() : "none", cap ? ue::GetName(cap).c_str() : "none",
                  target ? ue::GetName(target).c_str() : "nothing", sx, sy, source,
                  every ? "takes a picture every frame" : "takes pictures only when asked");
    }
}

// The kill shot room's camera, if it only takes a picture when asked: asked
// four times a second while the death screen is up, so the picture is
// there however the game set the room up. Returns the camera, or 0.
UObject KillShotCamera(UObject menu, bool* every) {
    UObject review = 0, room = 0, cap = 0, target = 0;
    ue::GetProp(menu, "UIP_DeadReview", review);
    if (review) ue::GetProp(review, "ShotRoom", room);
    if (room) ue::GetProp(room, "SceneCaptureComponent2D", cap);
    if (cap) ue::GetProp(cap, "TextureTarget", target);
    if (!target) return 0;
    auto pi = ue::reflection::FindProperty(ue::GetClass(cap), "bCaptureEveryFrame");
    *every = pi.valid() && (mem::ReadOr<uint8_t>(cap + pi.offset, 0) & (pi.bool_mask ? pi.bool_mask : 0xFF)) != 0;
    return cap;
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
    if (!mem::Write<uint8_t>(at, static_cast<uint8_t>(v | L.depth_off_mask))) return;
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
    mem::Write<uint8_t>(at, static_cast<uint8_t>(v & ~L.depth_off_mask));
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
    vr_options::OnMenuHosted(c.obj, p.name);  // Options gets its VR tab before it is fitted to the arm
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
    WalkScope walk;
    double t_walk = perf::NowMs();
    std::vector<UObject> blurs, texts;
    int widgets = FindBlurs(c.obj, &blurs, &texts);
    double t_blurs = perf::NowMs();
    for (UObject b : blurs) {
        ue::Call v(b, "SetVisibility");
        v.Set("InVisibility", kCollapsed);
        v.Invoke();
        log::Info("world menu: hid %s (a background blur)", ue::GetName(b).c_str());
    }
    double t_hid = perf::NowMs();
    int boxes = HideBackdrops(c.obj, p.name);
    if (boxes > 0) log::Info("world menu: %s: hid %d see-through backdrop box(es)", p.name.c_str(), boxes);
    double t_boxes = perf::NowMs();
    int outlined = OutlineText(texts);
    // Texts grown and the menu measured once it has been drawn on its panel
    // (a few frames from now, see OnGameFrame); see-through until then.
    if (L.slate_lib && AddTexts(G.panels.back(), texts) > 0) G.panels.back().text_phase = kTextBaseline;
    if (S.fit && L.slate_lib) G.panels.back().fit_pending = true;
    // See-through until grown and zoomed; either way at the menus' brightness.
    SetPanelOpacity(comp, G.panels.back().fit_pending ? 0.f : 1.f);
    // Lists the game fills a moment after the menu is built get the same.
    G.panels.back().regrow_at = Now() + 0.8;
    // Then texts that come into view later, and texts the game adds later.
    G.panels.back().text_watch_at = Now() + 1.0;
    G.panels.back().text_scan_at = Now() + 3.0;
    double t_end = perf::NowMs();
    log::Info("world menu: %s on the arm in %.1f ms (%d widgets; blurs %.1f ms, backdrops %.1f ms, %d of %zu texts "
              "outlined %.1f ms, %u memory lookups; holder %s)",
              p.name.c_str(), t_end - t0, widgets, t_blurs - t_walk, t_boxes - t_hid, outlined, texts.size(),
              t_end - t_boxes, mem::ScopedRegionCache::Lookups(), holder_ready ? "made at level start" : "made just now");
    // A menu the game put up by itself (the area select after the microwave
    // fall comes up in a dark pit) is easy to miss on the arm: buzz that hand
    // so you look. Only for the first menu; sub-menus come from your clicks.
    if (S.arrive_buzz && S.forearm && p.layer == 0) {
        input::Haptic(static_cast<input::Hand>(S.arm), 0.7f, 0.25f, 160.f);
        log::Info("world menu: buzzed the %s hand: a menu is on the arm", S.arm == input::kLeft ? "left" : "right");
    }
    return true;
}

// The death screen (UIM_Results_C) showed no picture of who killed you on
// the arm (owner 2026-11-03: "i cant see the kill shot photo"). The player's
// scene capture, which VR stops for speed, runs again while it is up, and
// what the picture is drawn from is logged so the next run shows if that was
// the reason.
void OnDeathScreen(UObject menu, bool up) {
    int n = render_fixes::AllowPlayerCapture(up);
    if (!up || !menu) return;
    UObject img = 0;
    ue::GetProp(menu, "IMG_Killer", img);
    if (!img) {
        log::Info("world menu: death screen: no IMG_Killer picture found; %d player capture(s) switched", n);
        return;
    }
    UObject res = L.image_brush >= 0 ? mem::ReadOr<UObject>(img + L.image_brush + L.brush_resource, 0) : 0;
    float opacity = L.render_opacity >= 0 ? mem::ReadOr<float>(img + L.render_opacity, -1.f) : -1.f;
    log::Info("world menu: death screen: IMG_Killer (visibility %d, opacity %.2f) is drawn from %s %s; "
              "%d player scene capture(s) running again",
              Visibility(img), opacity, res ? ue::GetName(ue::GetClass(res)).c_str() : "nothing",
              res ? ue::GetName(res).c_str() : "", n);
}

// Whether a widget inside a menu shows: nothing around it hidden, and every
// switcher on the way showing the page it is on.
bool ShownInMenu(UObject w, UObject menu) {
    uint8_t v = Visibility(w);
    if (v == kCollapsed || v == kHidden) return false;
    UObject below = w;
    for (UObject up = ParentOf(w); up && up != menu; below = up, up = ParentOf(up)) {
        v = Visibility(up);
        if (v == kCollapsed || v == kHidden) return false;
        if (L.switcher_class && IsA(up, L.switcher_class)) {
            ue::Call a(up, "GetActiveWidget");
            if (!a.Invoke() || a.Result<UObject>() != below) return false;
        }
    }
    return true;
}

// While the death screen is up: each time its page changes, what the kill
// shot view is made of (DescribeKillShot) and a picture of the panel a second
// later; and the kill shot room's camera, if it only takes pictures when
// asked, asked four times a second while the kill shot shows.
void WatchDeathScreen(Panel& p, double now) {
    if (now >= p.page_watch_at) {
        p.page_watch_at = now + 0.5;
        WalkScope walk;
        UObject sw = 0;
        ue::GetProp(p.widget, "WidgetSwitcher", sw);
        int page = -1;
        if (sw) {
            ue::Call a(sw, "GetActiveWidgetIndex");
            if (a.Invoke()) page = a.Result<int32_t>();
        }
        if (page != p.switcher_page) {
            if (p.switcher_page == -2 && sw) {
                ue::Call n(sw, "GetNumWidgets");
                int count = n.Invoke() ? n.Result<int32_t>() : 0;
                std::string pages;
                for (int i = 0; i < count && i < 8; ++i) {
                    ue::Call g(sw, "GetWidgetAtIndex");
                    g.Set("Index", static_cast<int32_t>(i));
                    UObject w = g.Invoke() ? g.Result<UObject>() : 0;
                    if (!pages.empty()) pages += ", ";
                    pages += std::to_string(i) + " " + (w ? ue::GetName(w) : "?");
                }
                log::Info("world menu: death screen has %d page(s): %s", count, pages.c_str());
            }
            log::Info("world menu: death screen now on page %d", page);
            p.switcher_page = page;
            DescribeKillShot(p.widget, page);
            p.page_snapshot_at = now + 1.0;
        }
    }
    if (p.page_snapshot_at > 0 && now >= p.page_snapshot_at && p.text_phase == kTextOff) {
        p.page_snapshot_at = -1;
        WalkScope walk;
        SnapshotPanel(p, "-page" + std::to_string(p.switcher_page));
    }
    if (now >= p.kill_capture_at) {
        p.kill_capture_at = now + 0.25;
        WalkScope walk;
        UObject review = 0;
        ue::GetProp(p.widget, "UIP_DeadReview", review);
        bool every = false;
        UObject cam = review && ShownInMenu(review, p.widget) ? KillShotCamera(p.widget, &every) : 0;
        if (cam && !every) {
            ue::Call c(cam, "CaptureScene");
            bool ok = c.Invoke();
            if (p.kill_captures++ == 0)
                log::Info("world menu: kill shot shows and its camera only takes pictures when asked: asking it "
                          "4 times a second (%s)",
                          ok ? "it took one" : "the call failed");
        }
    }
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
    if (!G.panels.empty()) G.panels.back().regrow_at = Now() + 0.3;
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

// Widgets the game took away this frame (for the HUD too, see UpdateHud).
std::vector<UObject> g_removed_now;

// Lets go of menus the game took away (RemoveFromParent) or destroyed.
void DropClosedMenus() {
    std::vector<UObject>& removed = g_removed_now;
    removed.clear();
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
    g_top_is_pause = top >= 0 && G.panels[top].name == "UIM_Pause_C";
}

// The menu button was pressed with the pause menu on top: does what its
// Return button does (back to the game).
void ClosePauseMenu() {
    int top = -1;
    for (int i = static_cast<int>(G.panels.size()) - 1; i >= 0 && top < 0; --i)
        if (WidgetShows(G.panels[i])) top = i;
    if (top < 0 || G.panels[top].name != "UIM_Pause_C") return;
    UObject pause = G.panels[top].widget;
    ue::Call ret(pause, "BndEvt__UIM_Pause_UIP_Button_Secondary_Return_K2Node_ComponentBoundEvent_0_OnClicked__DelegateSignature");
    const char* how = "its Return button";
    bool ok = false;
    {
        ue::process_event::GameCodeScope game_code;  // the game may open or close menus from here
        if (ret.Valid()) {
            ok = ret.Invoke();
        } else {
            ue::Call rg(pause, "ReturnGame");
            how = "ReturnGame";
            ok = rg.Invoke();
        }
    }
    log::Info("world menu: menu button closes the pause menu (%s: %s)", how, ok ? "done" : "the call failed");
}


// ---------------------------------------------------- the HUD on the arm --
// The game's in-level HUD (UIM_HUD_C: kill feed, objectives, money, pick-up
// and interaction prompts) was left on the flat screen, which the headset
// shows stuck to your face (owner 2026-11-03: "the in-level ui (such as the
// kill feed) is still the flatscreen UI we need it to move to the forearm
// sitting where the main menu sits"). It goes on a panel of its own at the
// menu's spot on the forearm, the same way the menus do (onto the panel,
// then off the flat screen, so the game never sees it destroyed), and fades
// in when you turn the arm toward you like a watch. While a menu is up the
// menu has that spot and the HUD is hidden. The laser never points at it.
// Ammo and health stay on the back of the hand (vr/wrist_hud).
struct HudPanel {
    UObject widget = 0;
    int widget_index = -1;
    UObject comp = 0;
    int comp_index = -1;
    float alpha = 0.f;        // drawn opacity, fading toward 0 or 1
    double look_until = -1;   // stays up a moment after you look away
    float redraw = -1.f;      // the panel's redraw interval as last set
    bool captured = false;    // a picture of it taken for the log folder
    std::map<std::string, uint8_t> hidden;  // parts hidden on the arm, and their own visibility
} HP;

// Parts that only make sense on the flat screen: the crosshair dot (aim with
// the sights) and the waypoint marker, which the game puts where the target
// would be on the flat screen.
const char* const kFlatOnlyHudParts[] = {"HUDP_Dot", "HUDP_NavIcon"};

void HideFlatOnlyParts() {
    for (const char* name : kFlatOnlyHudParts) {
        UObject part = 0;
        if (!ue::GetProp(HP.widget, name, part) || !part) continue;
        uint8_t vis = Visibility(part);
        if (vis == kCollapsed) continue;
        if (!HP.hidden.count(name)) HP.hidden[name] = vis;
        ue::Call v(part, "SetVisibility");
        v.Set("InVisibility", kCollapsed);
        v.Invoke();
    }
}

// What the HUD is made of, once, so the log names the part that is the kill
// feed (and anything else the owner asks to move).
void LogHudParts(UObject hud) {
    static bool s_logged = false;
    if (s_logged) return;
    s_logged = true;
    UObject root = TreeRoot(hud);
    std::vector<UObject> top, kids;
    if (root) Children(root, 64, &top);
    if (top.size() == 1 && !IsA(top[0], L.user_widget_class) && IsA(top[0], L.panel_class)) {
        UObject only = top[0];
        Children(only, 64, &top);
    }
    std::string parts;
    for (UObject w : top) {
        if (!parts.empty()) parts += ", ";
        parts += ue::GetName(w) + " (" + ue::GetName(ue::GetClass(w)) + (Visibility(w) == kCollapsed ? ", collapsed" : Visibility(w) == kHidden ? ", hidden" : "") + ")";
        if (parts.size() > 1500) break;
    }
    log::Info("world menu: HUD %s is made of: %s", root ? ue::GetName(root).c_str() : "(no tree)", parts.c_str());
}

bool HostHud(UObject hud) {
    UObject comp = AddComponent(G.actor, L.widget_comp_class, true);
    if (!comp) return false;
    ue::SetProp<uint8_t>(comp, "Space", kSpaceWorld);
    ue::SetProp<uint8_t>(comp, "BlendMode", kBlendTransparent);
    struct IntPoint {
        int32_t x, y;
    } size{kDrawW, kDrawH};
    ue::SetProp(comp, "DrawSize", size);
    ue::SetProp<uint8_t>(comp, "bIsTwoSided", 1);
    ue::SetProp<uint8_t>(comp, "TickWhenOffscreen", 1);
    ue::SetProp(comp, "BackgroundColor", LinearColor{0.f, 0.f, 0.f, 0.f});
    FinishComponent(G.actor, comp);
    ue::Call nc(comp, "SetCollisionEnabled");
    nc.Set("NewType", static_cast<uint8_t>(0));  // ECollisionEnabled::NoCollision
    nc.Invoke();
    SetPanelOpacity(comp, 0.f);
    ue::Call set(comp, "SetWidget");
    set.Set("Widget", hud);
    set.Invoke();
    t_our_call = true;
    CallVoid(hud, "RemoveFromParent");
    t_our_call = false;
    HP.widget = hud;
    HP.widget_index = IndexOf(hud);
    HP.comp = comp;
    HP.comp_index = IndexOf(comp);
    HP.alpha = 0.f;
    HP.look_until = -1;
    HP.hidden.clear();
    WalkScope walk;
    std::vector<UObject> blurs, texts;
    int widgets = FindBlurs(hud, &blurs, &texts);
    for (UObject b : blurs) {
        ue::Call v(b, "SetVisibility");
        v.Set("InVisibility", kCollapsed);
        v.Invoke();
    }
    int outlined = OutlineText(texts);
    HideFlatOnlyParts();
    log::Info("world menu: the in-level HUD (%s) is now on the forearm, off the flat screen; it shows when you "
              "look at your arm (%d widgets, %d of %zu texts outlined, %zu blurs hidden)",
              ue::GetName(ue::GetClass(hud)).c_str(), widgets, outlined, texts.size(), blurs.size());
    LogHudParts(hud);
    return true;
}

void UnhostHud(const char* why) {
    if (!HP.widget) return;
    if (Alive(HP.widget, HP.widget_index)) {
        for (const auto& [name, vis] : HP.hidden) {
            UObject part = 0;
            if (!ue::GetProp(HP.widget, name, part) || !part) continue;
            ue::Call v(part, "SetVisibility");
            v.Set("InVisibility", vis);
            v.Invoke();
        }
    }
    DestroyComponent(HP.comp, HP.comp_index);
    log::Info("world menu: the HUD %s; its forearm panel is gone", why);
    HP = HudPanel{};
}

// Game thread, from the player's tick (twice a second): finds the HUD on the
// player once the game has put it on the flat screen, and puts it on the arm.
void FindHud(UObject player) {
    static double s_next = 0;
    double now = Now();
    if (!S.enabled || !S.hud || !S.forearm || now < s_next) return;
    s_next = now + 0.5;
    WalkScope walk;
    if (!G.actor || !Alive(G.actor, G.actor_index)) return;
    if (HP.widget) {
        if (Alive(HP.widget, HP.widget_index)) HideFlatOnlyParts();  // the game may show them again
        return;
    }
    UObject hud = 0;
    if (!player || !ue::GetProp(player, "HUD", hud) || !hud) return;
    if (!Alive(hud, IndexOf(hud)) || !CallBool(hud, "IsInViewport")) return;
    HostHud(hud);
}

// Is the arm turned toward you, like looking at a watch?
bool LookingAtArm() {
    xr::EyeView eyes[2];
    if (!xr::LatestViews(eyes)) return false;
    XrPosef mid = eyes[0].pose;
    mid.position.x = (eyes[0].pose.position.x + eyes[1].pose.position.x) * 0.5f;
    mid.position.y = (eyes[0].pose.position.y + eyes[1].pose.position.y) * 0.5f;
    mid.position.z = (eyes[0].pose.position.z + eyes[1].pose.position.z) * 0.5f;
    FVector head;
    FQuat hq;
    if (!stereo::TrackingToWorld(mid, &head, &hq)) return false;
    FVector to = head - G.centre;  // panel to eyes
    double d = std::sqrt(DotV(to, to));
    if (d < 1.0 || d > S.hud_distance_m * G.cm_per_m) return false;
    FVector dir = to * (1.0 / d);
    double facing = DotV(G.rot.Rotate(FVector{1, 0, 0}), dir);
    double gaze = -DotV(hq.Rotate(FVector{1, 0, 0}), dir);
    return facing > std::cos(S.hud_look_deg * kDegToRad) && gaze > std::cos(40.0 * kDegToRad);
}

// Every frame: keeps the HUD on the arm, fades it in and out, and lets go of
// it when the game takes it away or puts it back on the flat screen.
void UpdateHud(double now) {
    if (!HP.widget) return;
    WalkScope walk;
    if (std::find(g_removed_now.begin(), g_removed_now.end(), HP.widget) != g_removed_now.end())
        return UnhostHud("was taken away by the game");
    if (!Alive(HP.comp, HP.comp_index)) {
        log::Info("world menu: the HUD's forearm panel went with the level");
        HP = HudPanel{};
        return;
    }
    if (!Alive(HP.widget, HP.widget_index)) return UnhostHud("was destroyed");
    // The game put it on the flat screen again by itself: let it go there; it
    // comes back onto the arm on the next look (FindHud).
    if (CallBool(HP.widget, "IsInViewport")) return UnhostHud("was put back on the flat screen by the game");
    bool want = false;
    if (G.panels.empty() && PlaceOnForearm()) {
        if (LookingAtArm()) HP.look_until = now + 0.4;
        want = now < HP.look_until;
        double s = Scale() * S.hud_size;
        SetWorldPose(HP.comp, G.centre, PanelRotation());
        ue::Call sc(HP.comp, "SetWorldScale3D");
        sc.Set("NewScale", FVector{s, s, s});
        sc.Invoke();
    }
    static double s_last = now;
    double dt = std::clamp(now - s_last, 0.0, 0.1);
    s_last = now;
    float target = want ? 1.f : 0.f;
    // Out of sight it is drawn four times a second, which keeps its own
    // timers and animations going for a fraction of the GPU time.
    float redraw = want || HP.alpha > 0.f ? 0.f : 0.25f;
    if (redraw != HP.redraw) {
        ue::Call r(HP.comp, "SetRedrawTime");
        r.Set("InRedrawTime", redraw);
        r.Invoke();
        HP.redraw = redraw;
    }
    float a = G.panels.empty() ? HP.alpha + std::clamp(target - HP.alpha, static_cast<float>(-dt * 8), static_cast<float>(dt * 8)) : 0.f;
    if (std::fabs(a - HP.alpha) > 0.001f || (a == 0.f) != (HP.alpha == 0.f)) {
        SetPanelOpacity(HP.comp, a);
        HP.alpha = a;
    }
    if (a >= 1.f && !HP.captured) {
        HP.captured = true;
        log::Info("world menu: HUD shown on the arm (you looked at it)");
        xr::RequestCapture("forearm-hud");
    }
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
    Hit best, scroll_at;
    // Up/down on the pointing hand's stick, or on the menu hand's while that
    // one is still: either thumb scrolls (owner 2026-11-03: "i want to be
    // able to use my joy stick to scroll though menu's").
    float stick = h.stick_y;
    if (std::fabs(stick) < 0.2f && S.forearm) stick = in.hand[S.arm].stick_y;
    if (h.aim_valid && stereo::TrackingToWorld(h.aim, &o, &q)) {
        FVector d = q.Rotate(FVector{1, 0, 0});
        for (const auto& p : G.panels) {
            if (!p.shown || !WidgetShows(p)) continue;
            Hit hit = RayPanel(o, d, p);
            hit.panel = &p;
            if (hit.ok && (!best.ok || hit.t < best.t)) best = hit;
        }
        FQuat rot = ToQuat(PanelRotation());
        if (best.ok) {
            const Panel& p = *best.panel;
            double s = Scale() * p.zoom;
            FVector local = rot.Inverse().Rotate(best.point - PanelCentre(p));
            G.pt.spot_widget = p.widget;
            G.pt.spot_y = local.Y / s;
            G.pt.spot_z = local.Z / s;
            scroll_at = best;
        } else if (std::fabs(stick) >= 0.2f) {
            // The laser is off the menu: the pointer goes back to where it
            // last was on the top menu (its middle if it never was), so the
            // stick still scrolls that list. Nothing is clicked from there.
            const Panel* top = nullptr;
            for (const auto& p : G.panels)
                if (p.shown && WidgetShows(p)) top = &p;
            if (top) {
                if (G.pt.spot_widget != top->widget) {
                    G.pt.spot_widget = top->widget;
                    G.pt.spot_y = G.pt.spot_z = 0;
                }
                double s = Scale() * top->zoom;
                FVector n = rot.Rotate(FVector{1, 0, 0});
                scroll_at.ok = true;
                scroll_at.panel = top;
                scroll_at.point = PanelCentre(*top) + rot.Rotate(FVector{0, G.pt.spot_y * s, G.pt.spot_z * s});
                scroll_at.t = 0.03 * G.cm_per_m;
                o = scroll_at.point + n * scroll_at.t;
                d = n * -1.0;
            }
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
        SendHit(scroll_at.ok ? &scroll_at : nullptr, o, d);
        LogLaser(best, d);
    } else {
        SendHit(nullptr, o, FVector{1, 0, 0});
    }
    float want = scroll_at.ok ? static_cast<float>(0.06 * G.cm_per_m) : 0.f;
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

    // Stick up/down scrolls: one notch the moment it is pushed past half
    // way, then 2 to 15 notches a second the further it is pushed (the old
    // rate, 4 a second at full tilt on the pointing hand only, was too slow
    // to get down a long Options list).
    double t_now = perf::NowMs() / 1000.0;
    double dt = std::clamp(t_now - G.pt.scroll_t, 0.0, 0.1);
    G.pt.scroll_t = t_now;
    float tilt = std::fabs(stick);
    if (!scroll_at.ok || tilt < 0.2f) {
        G.pt.scroll_accum = 0.f;
        if (tilt < 0.2f) G.pt.scroll_held = false;
    } else {
        if (!G.pt.scroll_held && tilt >= 0.5f) {
            G.pt.scroll_held = true;
            G.pt.scroll_accum = 1.f;
        }
        G.pt.scroll_accum += static_cast<float>(dt) * (2.f + 13.f * tilt * tilt);
        for (int i = 0; i < 3 && G.pt.scroll_accum >= 1.f; ++i) {
            G.pt.scroll_accum -= 1.f;
            ue::process_event::GameCodeScope game_code;  // see ReleaseKey
            ue::Call c(G.wic, "ScrollWheel");
            c.Set("ScrollDelta", stick > 0 ? 1.f : -1.f);
            c.Invoke();
            if (G.pt.scroll_logs < 12) {
                ++G.pt.scroll_logs;
                log::Info("world menu: stick scrolled %s %s (%s)", scroll_at.panel->name.c_str(),
                          stick > 0 ? "up" : "down",
                          best.ok ? "where the laser points" : "where the laser last was on it");
            }
        }
    }
    std::lock_guard<std::mutex> lock(g_laser_mutex);
    g_laser = laser;
}

// --------------------------------------------------------- frame pump --

// ------------------------------------------------------ menu brightness --
// The game brightens the picture in dark areas: each area's
// LCBP_LocalPostProcess sets BP_Player.Exposure_BaseBrightness, which raises
// the exposure of the player's post-process. A flat menu is drawn after that
// and never sees it, but ours are in the world, so in a dark area the death
// screen's kill shot came out pure white and its orange buttons pale yellow,
// (owner 2026-11-03: the buttons are "too bright"; headset picture of the
// 10:08 run). The game's exposure is about +1.1 stops in lit rooms (manual
// bias 11 with the physical camera's EV100 9.9) and +2.6 in the dark area of
// the 11:00 run. Dimming only past the lit rooms' level (the first try) left
// the death screen's kill shot near white and its orange buttons yellow, and
// the level select's brown Enter button came out yellow in a lit room too:
// the headset picture matches the panel's own colours at about 0.4 of a 1:1
// exposure (raw value 0.55 showed as 0.94 at 1.76). So the menus are drawn
// at 2^-(exposure - menu_full_brightness_at) with the reference at -1 stops,
// wherever you are.
UObject g_player = 0;  // game thread
int g_player_index = -1;

void ApplyBrightness() {
    for (const auto& p : G.panels)
        if (Alive(p.comp, p.comp_index)) SetPanelOpacity(p.comp, p.fit_pending ? 0.f : 1.f);
    if (HP.comp && Alive(HP.comp, HP.comp_index)) SetPanelOpacity(HP.comp, HP.alpha);
}

void UpdateBrightness(double now) {
    static double s_next = 0;
    static size_t s_panels = 0;
    static float s_logged = -1.f;   // the brightness last written to the log
    const bool new_menu = G.panels.size() > s_panels;
    s_panels = G.panels.size();
    if (now < s_next && !new_menu) return;
    s_next = now + 0.25;
    render_fixes::Exposure e;
    UObject pp = 0;
    if (S.follow_exposure && g_player && Alive(g_player, g_player_index) && ue::GetProp(g_player, "PostProcess", pp) &&
        pp)
        e = render_fixes::ReadExposure(pp);
    const double ref = S.exposure_ref;
    float gain = g_menu_gain;  // the player can't be read (e.g. between levels): keep what we had
    if (!S.follow_exposure) {
        gain = S.brightness;
    } else if (e.ok && e.bias_set) {  // set by the game (not the engine's default before it does)
        gain = S.brightness * static_cast<float>(std::pow(2.0, -(e.stops - ref)));
    }
    gain = std::clamp(gain, S.brightness_min, 4.f);
    const bool changed = std::fabs(gain - g_menu_gain) > 0.02f * std::max(gain, g_menu_gain);
    if (changed) {
        g_menu_gain = gain;
        ApplyBrightness();
    }
    const bool log_it = new_menu || (changed && std::fabs(gain - s_logged) > 0.1f * std::max(gain, s_logged));
    if (!log_it || G.panels.empty()) return;
    s_logged = gain;
    double base = -1;
    if (g_player && Alive(g_player, g_player_index)) ue::GetProp(g_player, "Exposure_BaseBrightness", base);
    if (e.ok)
        log::Info("world menu: menus drawn at %.2f brightness (game exposure %+.2f stops: bias %.2f%s, method %d%s%s, "
                  "area brightness %.2f; reference %+.2f) [%s]",
                  gain, e.stops, e.bias, e.bias_set ? "*" : "", e.method, e.method_set ? "*" : "",
                  e.physical ? ", physical camera" : "", base, ref, render_fixes::ExposureReport().c_str());
    else
        log::Info("world menu: menus drawn at %.2f brightness (player's exposure not readable now%s) [%s]", gain,
                  S.follow_exposure ? "" : "; menu_follow_exposure=0", render_fixes::ExposureReport().c_str());
}

void OnGameFrame() {
    if (!S.enabled || !ue::reflection::Ready() || !ue::process_event::Ready()) return;
    // Once per frame: the controller's tick and the player's tick can both call.
    // Timed with the high-resolution clock: GetTickCount64 only moves every
    // 15.6 ms, which held the menu to 64 updates a second on the 72 Hz
    // headset, so 8 frames a second showed it where the arm had been (owner,
    // 2026-11-03: the area select was "really choppy and jittery").
    static double s_last_ms = 0;
    double ms = perf::NowMs();
    if (ms - s_last_ms < 4.0) return;
    s_last_ms = ms;
    double now = Now();
    TakeNewMenus();
    DropClosedMenus();
    UpdateStack();
    if (g_close_pause.exchange(false)) {
        ClosePauseMenu();
        DropClosedMenus();
        UpdateStack();
    }
    g_menu_up = !G.panels.empty();
    UpdateBrightness(now);
    static UObject s_death = 0;
    {
        UObject death = 0;
        for (const auto& p : G.panels)
            if (p.name == "UIM_Results_C") death = p.widget;
        if (death != s_death) {
            OnDeathScreen(death, death != 0);
            s_death = death;
        }
    }
    // Blur the world past arm's length while a menu is on the arm. Not on the
    // death screen: its kill shot is a picture the game takes of your body a
    // few metres away, and the blur turned it into a smear (2026-11-03 panel
    // picture; owner: "kill shot screen: no").
    static bool s_blur = false;
    const bool want_blur = !G.panels.empty() && !s_death;
    if (s_blur != want_blur) {
        s_blur = want_blur;
        // Nothing nearer than the focus may blur (hands, arms, body, menu).
        // Set right here, before the blur starts: the background cvars pass
        // only runs while the player ticks and had not run yet on the
        // owner's 2026-11-02 12:13 run, so the hands came out blurred.
        if (s_blur) render_fixes::ApplyPauseBlurCvars(G.actor);
        int n = render_fixes::SetPauseBlur(s_blur);
        if (n >= 0 && s_blur)
            log::Info("world menu: pause blur on (%d settings; r.DepthOfFieldQuality %d, "
                      "r.DOF.Kernel.MaxForegroundRadius %.3f)",
                      n, cvars::ReadIntNow("r.DepthOfFieldQuality", -1),
                      cvars::ReadFloatNow("r.DOF.Kernel.MaxForegroundRadius", -1.f));
        else if (n >= 0)
            log::Info("world menu: pause blur off (%d settings)", n);
    }
    UpdateHud(now);
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
    // The headset picture is taken once you look at the menu on your arm (at
    // a fixed second the arm was rarely in view), or never after 20 s.
    if (g_capture_at > 0 && now >= g_capture_at) {
        if (now >= g_capture_at + 20.0) {
            g_capture_at = -1;
        } else if (!S.forearm || LookingAtArm()) {
            g_capture_at = -1;
            xr::RequestCapture(("forearm-" + g_capture_name).c_str());
        }
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
    // New menus are measured once they have been drawn on their panel, then
    // zoomed and shown (FitContent); after a click the laser area may grow.
    for (auto& p : G.panels) {
        const bool settled = !p.fit_pending && p.regrow_at < 0 && p.text_phase == kTextOff;
        if (settled && p.recheck_at > 0 && now >= p.recheck_at) {
            p.recheck_at = -1;
            p.text_phase = kTextCheck;
            p.text_round = 0;
            p.text_wait = 0;
            p.text_pulled = 0;
            p.quiet_check = true;
        } else if (settled && p.shown && now >= p.text_scan_at) {
            p.text_scan_at = now + 3.0;
            WalkScope walk;
            if (NewPageTexts(p)) p.regrow_after_text = true;
        } else if (settled && p.shown && now >= p.text_watch_at) {
            p.text_watch_at = now + 0.5;
            WalkScope walk;
            if (TextWatch(p) == 1) p.regrow_after_text = true;
        }
        if (p.name == "UIM_Results_C" && !p.fit_pending) WatchDeathScreen(p, now);
        if (p.snapshot_at > 0 && now >= p.snapshot_at && !p.fit_pending && p.text_phase == kTextOff) {
            p.snapshot_at = -1;
            WalkScope walk;
            if (p.name != "UIM_Results_C") SnapshotPanel(p, "");  // the death screen: one per page instead
        }
        if (!p.fit_pending && p.regrow_at < 0 && p.text_phase == kTextOff) continue;
        WalkScope walk;
        if (p.fit_pending || p.text_phase != kTextOff) {
            Rect whole;
            const bool on_panel = DrawnOnPanel(p, &whole);
            if (!on_panel && now - p.hosted_at >= 1.0) p.text_phase = kTextOff;  // FitContent says why
            if (on_panel || now - p.hosted_at >= 1.0) {
                const bool was_growing = p.text_phase != kTextOff;
                if (TextStep(p)) {
                    if (SeparateParts(p)) p.fit_wait = 3;  // laid out at the new spots first
                    if (p.fit_wait > 0) {
                        --p.fit_wait;
                    } else if (p.fit_pending && FitContent(p)) {
                        p.fit_pending = false;
                        PosePanel(p);
                        SetPanelOpacity(p.comp, 1.f);
                        p.snapshot_at = now + 1.0;
                    }
                    if (was_growing && p.regrow_after_text) {
                        p.regrow_after_text = false;
                        GrowHitArea(p);
                        p.snapshot_at = now + 1.0;
                    }
                }
            }
        }
        if (p.regrow_at > 0 && now >= p.regrow_at && !p.fit_pending && p.text_phase == kTextOff) {
            p.regrow_at = -1;
            if (NewPageTexts(p)) p.regrow_after_text = true;  // grown first, then the laser area
            else GrowHitArea(p);
        }
    }
    KeepBoxesAway();
    vr_options::Tick();
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
    S.text_outline = std::clamp(config::GetInt("menu_text_outline", 2), 0, 8);
    S.text_scale = std::clamp(config::GetFloat("menu_text_scale", 1.5f), 1.f, 3.f);
    S.fit_shape = config::GetBool("menu_fit_shape", true);
    S.fit_min_zoom = std::clamp(config::GetFloat("menu_fit_min_zoom", 1.3f), 1.f, 3.f);
    S.follow_exposure = config::GetBool("menu_follow_exposure", true);
    // New names (2026-11-03): the first version's menu_exposure_ref=0 and
    // menu_brightness_min=0.1 are already in players' hole-vr.ini and meant
    // something else, so they are left there unread.
    S.exposure_ref = std::clamp(config::GetFloat("menu_full_brightness_at", -1.f), -15.f, 15.f);
    S.brightness = std::clamp(config::GetFloat("menu_brightness", 1.f), 0.05f, 4.f);
    S.brightness_min = std::clamp(config::GetFloat("menu_dimmest", 0.03f), 0.005f, 1.f);
    S.separate = config::GetBool("menu_move_parts_apart", true);
    S.text_min_px = std::clamp(config::GetFloat("menu_text_min_px", 32.f), 0.f, 120.f);
    S.text_max_px = std::clamp(config::GetFloat("menu_text_max_px", 64.f), 10.f, 200.f);
    S.text_max_scale = std::clamp(config::GetFloat("menu_text_max_scale", 2.5f), 1.f, 4.f);
    S.snapshots = std::clamp(config::GetInt("menu_snapshots", 2), 0, 10);
    S.hud = config::GetBool("hud_on_forearm", true);
    S.hud_size = std::clamp(config::GetFloat("hud_forearm_size", 1.f), 0.3f, 3.f);
    S.hud_look_deg = std::clamp(config::GetFloat("hud_forearm_look_deg", 50.f), 10.f, 90.f);
    S.hud_distance_m = std::clamp(config::GetFloat("hud_forearm_distance_m", 0.8f), 0.2f, 2.f);
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
        render_fixes::ApplyPauseBlurCvars(player);  // for the pause blur
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
    if (player && player != g_player) {
        g_player = player;
        g_player_index = IndexOf(player);
    }
    if (S.enabled && ue::process_event::Ready()) FollowFromPlayer(player);
    if (S.enabled && player && G.panels.empty()) PrepareHolder(player);
    if (S.enabled && player && G.panels.empty()) FindHud(player);
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
// Name-pool entries of "Construct" and "OnAddedToFocusPath", found once.
// This listener sees every game event (thousands a second in a level), so it
// compares the function's name entry instead of taking a lock and a map
// lookup per event (2026-11-03 performance pass).
std::atomic<bool> g_fn_names_ok{false};
uint32_t g_construct_entry = 0, g_focus_entry = 0;

void OnAnyEvent(UObject self, UObject function, void*) {
    if (!S.enabled || !self || !function) return;
    enum : uint8_t { kOther = 0, kConstruct = 1, kFocus = 2 };
    uint8_t kind;
    if (g_fn_names_ok.load(std::memory_order_relaxed)) {
        // The engine is calling this function right now; the read still goes
        // through SafeCopy (about 20 ns) so freed memory can never crash it.
        ue::FName fn;
        if (!mem::SafeCopy(&fn, reinterpret_cast<const void*>(function + ue::off::kObjName), sizeof(fn))) return;
        if (fn.number != 0) return;
        kind = fn.comparison_index == g_construct_entry ? kConstruct
               : fn.comparison_index == g_focus_entry   ? kFocus
                                                        : kOther;
        if (kind == kOther) return;
    } else {
        // Fallback: remember which function objects are a Construct (1) or an
        // OnAddedToFocusPath (2).
        static std::mutex s_mutex;
        static std::unordered_map<UObject, uint8_t> s_kind;
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
    {
        ue::FName c, f;
        if (ue::names::Find("Construct", &c) && ue::names::Find("OnAddedToFocusPath", &f)) {
            g_construct_entry = c.comparison_index;
            g_focus_entry = f.comparison_index;
            g_fn_names_ok = true;
        } else {
            log::Warn("world menu: function names not found in the name pool; using the slower event check");
        }
    }
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
bool PauseMenuOnTop() { return g_menu_up.load() && g_top_is_pause.load(); }
void RequestClosePause() { g_close_pause = true; }
bool Active() { return g_active.load(); }
bool Enabled() { return S.enabled; }

}  // namespace holevr::vr::world_menu
