#include "vr/vr_options.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "core/perf.h"
#include "ue/call.h"
#include "ue/objects.h"
#include "ue/process_event.h"
#include "ue/reflection.h"
#include "vr/body/body.h"
#include "vr/controls.h"
#include "vr/hands.h"
#include "vr/input.h"
#include "vr/vr_menu_model.h"

namespace holevr::vr::vr_options {
namespace {
using ue::UObject;
namespace rf = ue::reflection;

// FString as the engine lays it out (TArray<TCHAR>, terminator counted).
// The engine only reads it (const FString& parameters).
struct FStringView {
    const wchar_t* data;
    int32_t num, max;
};

struct Lookups {
    bool done = false;
    UObject wbl = 0;   // WidgetBlueprintLibrary (Create)
    UObject gps = 0;   // GameplayStatics (SpawnObject)
    UObject ktl = 0;   // KismetTextLibrary (Conv_StringToText)
    UObject scroll_class = 0, text_class = 0, rich_text_class = 0, panel_class = 0, user_widget_class = 0;
    UObject hbox_class = 0, sizebox_class = 0;  // for the arrows beside each setting (optional)
} L;

// A widget we made, with its object index so a freed one is never touched.
struct Made {
    UObject obj = 0;
    int index = -1;
};
int IndexOf(UObject obj) { return mem::ReadOr<int32_t>(obj + ue::off::kObjIndex, -1); }
Made Track(UObject obj) { return Made{obj, obj ? IndexOf(obj) : -1}; }
bool Alive(const Made& m) { return m.obj && m.index >= 0 && ue::objects::Get(m.index) == m.obj; }

// Owner 2026-11-03: "left and right arrows to switch between options (right
// arrow to make turning fast, left arrow to make turning slower)". A setting
// row is  [<] [Setting: choice] [>]: the arrows step to the previous / next
// choice and stop at the ends; clicking the middle still steps forward and
// wraps. The setup rows (Calibrate body ...) are one button.
constexpr float kArrowWidth = 64.f;
const wchar_t* const kLeftArrow = L"<";
const wchar_t* const kRightArrow = L">";

struct RowWidget {
    Made w;              // the middle button: "Setting: choice"
    Made left, right;    // the arrow buttons (setting rows only)
    int row = 0;         // index into vr_menu::Rows()
    int choice = 0;
    std::vector<UObject> texts, left_texts, right_texts;  // their TextBlocks
};

struct Tab {
    Made option;
    Made switcher;
    Made page;        // our ScrollBox in the switcher
    Made vr_button;
    std::vector<UObject> vr_button_texts;
    std::vector<RowWidget> rows;
    int relabel_frames = 0;   // labels set again for the first frames (the game's own PreConstruct)
    bool study_logged = false;
    // The rows are made a few per frame after Options opens (each is up to
    // five widgets with their Blueprint construction; all 23 in one frame
    // took 71 ms, a hitch the owner felt, 2026-11-03 02:49 run). The page is
    // hidden until the VR tab is clicked, so nobody sees it fill.
    UObject row_class = 0, owner = 0, outer = 0;
    size_t next_row = 0;
    bool building = false;
    int build_frames = 0, with_arrows = 0;
    double build_ms = 0, build_worst_ms = 0;
} T;

UObject g_click_fn = 0;  // the game buttons' "clicked" event

// ---- text ----
// FText values made once per string and kept forever, so they can be handed
// to SetText any number of times (each TextBlock takes its own reference).
std::map<std::wstring, std::vector<uint8_t>> g_texts;

std::wstring Widen(const std::string& s) {
    std::wstring w;
    for (char c : s) w += static_cast<wchar_t>(static_cast<unsigned char>(c));
    return w;
}

bool MakeText(const std::wstring& w, std::vector<uint8_t>* out) {
    if (!L.ktl) return false;
    ue::Call c(L.ktl, "Conv_StringToText");
    FStringView fs{w.c_str(), static_cast<int32_t>(w.size() + 1), static_cast<int32_t>(w.size() + 1)};
    c.Set("InString", fs);
    if (!c.Invoke()) return false;
    int32_t size = 0;
    const uint8_t* r = c.Raw("ReturnValue", &size);
    if (!r || size <= 0 || size > 64) return false;
    out->assign(r, r + size);
    return true;
}

const std::vector<uint8_t>* CachedText(const std::wstring& w) {
    auto it = g_texts.find(w);
    if (it != g_texts.end()) return &it->second;
    std::vector<uint8_t> t;
    if (!MakeText(w, &t)) return nullptr;
    return &g_texts.emplace(w, std::move(t)).first->second;
}

bool SetText(UObject text_block, const std::wstring& w) {
    const std::vector<uint8_t>* t = CachedText(w);
    if (!t) return false;
    ue::Call c(text_block, "SetText");
    int32_t size = 0;
    uint8_t* p = c.Raw("InText", &size);
    if (!p || size != static_cast<int32_t>(t->size())) return false;
    std::memcpy(p, t->data(), t->size());
    return c.Invoke();
}

// The Blueprint's own FText variables (its label), set before it is built so
// its PreConstruct shows our text from the start. Each gets a text of its
// own: the widget owns and frees it. Only Blueprint classes' variables, never
// engine ones like a tooltip.
int SetBlueprintTexts(UObject widget, const std::wstring& w, std::vector<std::string>* names) {
    int n = 0;
    for (UObject cls = ue::GetClass(widget); cls; cls = rf::GetSuper(cls)) {
        std::string cn = ue::GetName(cls);
        if (cn.size() < 2 || cn.compare(cn.size() - 2, 2, "_C") != 0) break;
        for (const auto& p : rf::GetProperties(cls)) {
            if (p.type != "TextProperty" || p.array_dim != 1) continue;
            std::vector<uint8_t> t;
            if (!MakeText(w, &t) || static_cast<int32_t>(t.size()) != p.element_size) continue;
            if (!mem::IsReadable(widget + p.offset, t.size())) continue;
            std::memcpy(reinterpret_cast<void*>(widget + p.offset), t.data(), t.size());
            if (names) names->push_back(p.name);
            ++n;
        }
    }
    return n;
}

// ---- widget tree ----
UObject TreeRoot(UObject user_widget) {
    UObject tree = 0, root = 0;
    if (ue::GetProp(user_widget, "WidgetTree", tree) && tree && ue::GetProp(tree, "RootWidget", root)) return root;
    return 0;
}

void Children(UObject panel, std::vector<UObject>* out) {
    out->clear();
    ue::Call count(panel, "GetChildrenCount");
    int n = count.Invoke() ? count.Result<int32_t>() : 0;
    for (int i = 0; i < n && i < 64; ++i) {
        ue::Call child(panel, "GetChildAt");
        child.Set("Index", static_cast<int32_t>(i));
        if (child.Invoke() && child.Result<UObject>()) out->push_back(child.Result<UObject>());
    }
}

// Every TextBlock in a user widget's tree (and the user widgets inside it).
std::vector<UObject> FindTexts(UObject user_widget) {
    std::vector<UObject> out, kids;
    std::vector<std::pair<UObject, int>> todo;
    if (UObject r = TreeRoot(user_widget)) todo.push_back({r, 0});
    int seen = 0;
    while (!todo.empty() && seen < 200) {
        auto [w, depth] = todo.back();
        todo.pop_back();
        ++seen;
        if ((L.text_class && rf::IsA(w, L.text_class)) || (L.rich_text_class && rf::IsA(w, L.rich_text_class))) {
            out.push_back(w);
            continue;
        }
        if (depth > 12) continue;
        if (L.user_widget_class && rf::IsA(w, L.user_widget_class)) {
            if (UObject r = TreeRoot(w)) todo.push_back({r, depth + 1});
        } else if (L.panel_class && rf::IsA(w, L.panel_class)) {
            Children(w, &kids);
            for (UObject k : kids) todo.push_back({k, depth + 1});
        }
    }
    return out;
}

// Copies a slot's layout (padding, size, alignment...) onto another slot of
// the same kind through its own setters (SetPadding(InPadding) ...), so the
// new button sits in the row exactly like the game's own.
int CopySlotLayout(UObject from, UObject to) {
    if (!from || !to || ue::GetClass(from) != ue::GetClass(to)) return 0;
    int copied = 0;
    for (UObject cls = ue::GetClass(from); cls; cls = rf::GetSuper(cls)) {
        if (ue::GetName(cls) == "PanelSlot") break;
        for (const auto& p : rf::GetProperties(cls)) {
            if (p.type == "ObjectProperty" || p.type == "BoolProperty" || p.array_dim != 1) continue;
            UObject fn = ue::FindFunction(ue::GetClass(to), "Set" + p.name);
            if (!fn) continue;
            const rf::PropertyInfo* param = nullptr;
            auto params = rf::GetProperties(fn);
            for (const auto& q : params)
                if ((q.flags & rf::CPF_Parm) && !(q.flags & rf::CPF_ReturnParm)) {
                    param = &q;
                    break;
                }
            if (!param || param->element_size != p.element_size) continue;
            std::vector<uint8_t> v(static_cast<size_t>(p.element_size));
            if (!mem::IsReadable(from + p.offset, v.size())) continue;
            std::memcpy(v.data(), reinterpret_cast<const void*>(from + p.offset), v.size());
            ue::Call c(to, fn);
            int32_t size = 0;
            uint8_t* dst = c.Raw(param->name.c_str(), &size);
            if (!dst || size != p.element_size) continue;
            std::memcpy(dst, v.data(), v.size());
            if (c.Invoke()) ++copied;
        }
    }
    return copied;
}

UObject SlotOf(UObject widget) {
    UObject slot = 0;
    ue::GetProp(widget, "Slot", slot);
    return slot;
}

UObject CreateUserWidget(UObject context, UObject cls, UObject owner) {
    if (!L.wbl || !cls) return 0;
    ue::Call c(L.wbl, "Create");
    c.Set("WorldContextObject", context).Set("WidgetType", cls).Set("OwningPlayer", owner);
    if (!c.Invoke()) return 0;
    return c.Result<UObject>();
}

UObject AddChild(UObject panel, UObject child) {
    ue::Call c(panel, "AddChild");
    c.Set("Content", child);
    if (!c.Invoke()) return 0;
    return c.Result<UObject>();
}

// One-time look at the classes the tab is made of, for later polish (the
// selected look of tab buttons, the game's option rows).
void LogClass(UObject cls, const char* what) {
    if (!cls) return;
    for (UObject c = cls; c; c = rf::GetSuper(c)) {
        std::string cn = ue::GetName(c);
        if (cn.size() < 2 || cn.compare(cn.size() - 2, 2, "_C") != 0) break;
        std::string props, fns;
        for (const auto& p : rf::GetProperties(c)) props += p.name + ":" + p.type + " ";
        for (UObject f : rf::GetFunctions(c)) fns += ue::GetName(f) + " ";
        log::Info("vr options: study %s %s: variables %s", what, cn.c_str(), props.c_str());
        log::Info("vr options: study %s %s: functions %s", what, cn.c_str(), fns.c_str());
    }
}

UObject FindClickFunction(UObject cls) {
    UObject fn = ue::FindFunction(
        cls, "BndEvt__UIP_Button_Primary_Button_K2Node_ComponentBoundEvent_0_OnButtonClickedEvent__DelegateSignature");
    if (fn) return fn;
    for (UObject c = cls; c && !fn; c = rf::GetSuper(c))
        for (UObject f : rf::GetFunctions(c))
            if (ue::GetName(f).find("OnButtonClickedEvent") != std::string::npos) {
                fn = f;
                break;
            }
    return fn;
}

// ---- settings ----
std::string Current(const char* key) { return config::GetString(key, ""); }

void LabelRow(RowWidget& r) {
    const auto& row = vr_menu::Rows()[static_cast<size_t>(r.row)];
    // The grip editor row says whether the editor is open (the menu can be
    // opened while it is, and the same row closes it).
    if (row.kind == vr_menu::RowKind::kGripEditor) r.choice = hands::EditorHand() >= 0 ? 1 : 0;
    std::wstring w = Widen(vr_menu::RowText(row, r.choice));
    for (UObject t : r.texts) SetText(t, w);
}

void RelabelAll() {
    for (auto& r : T.rows) {
        if (Alive(r.w)) LabelRow(r);
        if (Alive(r.left))
            for (UObject t : r.left_texts) SetText(t, kLeftArrow);
        if (Alive(r.right))
            for (UObject t : r.right_texts) SetText(t, kRightArrow);
    }
    if (Alive(T.vr_button))
        for (UObject t : T.vr_button_texts) SetText(t, L"VR");
}

// step: +1 right arrow, -1 left arrow (both stop at the ends), 0 the middle
// button (next choice, wrapping round).
void ClickRow(RowWidget& r, int step) {
    const auto& row = vr_menu::Rows()[static_cast<size_t>(r.row)];
    if (row.kind == vr_menu::RowKind::kMeasureArms) {
        body::RequestMeasure();
        log::Info("vr options: guided body calibration requested; close the menu with both hands free, then follow "
                  "height, forward, out, bend and forward-check instructions; hold both triggers 2s, release between steps; X keeps arms; B/Y cancels");
        input::Haptic(input::kLeft, 0.4f, 0.05f);
        return;
    }
    if (row.kind == vr_menu::RowKind::kGripEditor) {
        const bool open = hands::EditorHand() >= 0;
        controls::RequestGripEditor();
        log::Info("vr options: Edit hand grips clicked; the grip editor %s once the menu closes",
                  open ? "closes" : "opens");
        input::Haptic(input::kLeft, 0.4f, 0.05f);
        return;
    }
    if (row.kind == vr_menu::RowKind::kMoveHolsters) {
        controls::RequestHolsterMove();
        log::Info("vr options: Move holsters clicked; once the menu closes, hold each item where you want its "
                  "holster (5 s countdown, a buzz each second)");
        input::Haptic(input::kLeft, 0.4f, 0.05f);
        return;
    }
    const int n = static_cast<int>(row.choices.size());
    const int next = step == 0 ? (r.choice + 1) % n : std::clamp(r.choice + step, 0, n - 1);
    if (next == r.choice) {
        input::Haptic(input::kRight, 0.1f, 0.01f);
        log::Info("vr options: %s is already at its %s choice (%s)", row.label, step > 0 ? "last" : "first",
                  row.choices[static_cast<size_t>(r.choice)].text);
        return;
    }
    r.choice = next;
    const auto& c = row.choices[static_cast<size_t>(r.choice)];
    std::string what;
    bool saved = true;
    for (const auto& kv : c.sets) {
        saved = config::Set(kv.key, kv.value) && saved;
        what += std::string(what.empty() ? "" : ", ") + kv.key + "=" + kv.value;
    }
    config::NotifyChanged();
    LabelRow(r);
    log::Info("vr options: %s -> %s (%s, %s)%s", row.label, c.text, what.c_str(),
              step > 0 ? "right arrow" : step < 0 ? "left arrow" : "row clicked",
              saved ? "" : "; NOT saved in hole-vr.ini, used for this run only");
}

// Runs right after any game button's clicked event; only ours are acted on.
void AfterClick(UObject self, UObject, void*) {
    if (Alive(T.vr_button) && self == T.vr_button.obj) {
        if (Alive(T.switcher) && Alive(T.page)) {
            ue::Call c(T.switcher.obj, "SetActiveWidget");
            c.Set("Widget", T.page.obj);
            c.Invoke();
            T.relabel_frames = 3;
            log::Info("vr options: VR tab opened");
        }
        return;
    }
    for (auto& r : T.rows) {
        const int step = Alive(r.w) && r.w.obj == self         ? 0
                         : Alive(r.left) && r.left.obj == self   ? -1
                         : Alive(r.right) && r.right.obj == self ? 1
                                                                 : 2;
        if (step != 2) {
            ClickRow(r, step);
            return;
        }
    }
}

UObject SpawnWidget(UObject cls, UObject outer) {
    if (!cls) return 0;
    ue::Call so(L.gps, "SpawnObject");
    so.Set("ObjectClass", cls).Set("Outer", outer);
    return so.Invoke() ? so.Result<UObject>() : 0;
}

template <typename T>
bool SetStruct(UObject obj, const char* fn, const char* param, const T& v) {
    if (!obj) return false;
    ue::Call c(obj, fn);
    int32_t size = 0;
    uint8_t* p = c.Raw(param, &size);
    if (!p || size != static_cast<int32_t>(sizeof(T))) return false;
    std::memcpy(p, &v, sizeof(T));
    return c.Invoke();
}

struct Margin {
    float l, t, r, b;
};

// [<] [Setting: choice] [>] in a horizontal box; false (nothing added) if
// any part could not be made, and the row is the middle button alone.
bool AddArrows(RowWidget& r, UObject label, UObject outer, UObject owner_widget, UObject row_class, UObject owner,
               UObject* holder) {
    if (!L.hbox_class || !L.sizebox_class) return false;
    UObject hb = SpawnWidget(L.hbox_class, outer);
    UObject lbox = SpawnWidget(L.sizebox_class, outer), rbox = SpawnWidget(L.sizebox_class, outer);
    UObject lb = CreateUserWidget(owner_widget, row_class, owner);
    UObject rb = CreateUserWidget(owner_widget, row_class, owner);
    if (!hb || !lbox || !rbox || !lb || !rb) return false;
    ue::process_event::HookVtableOf(lb);
    ue::process_event::HookVtableOf(rb);
    SetBlueprintTexts(lb, kLeftArrow, nullptr);
    SetBlueprintTexts(rb, kRightArrow, nullptr);
    for (UObject b : {lbox, rbox}) {
        ue::Call w(b, "SetWidthOverride");
        w.Set("InWidthOverride", kArrowWidth);
        w.Invoke();
    }
    if (!AddChild(lbox, lb) || !AddChild(rbox, rb)) return false;
    AddChild(hb, lbox);
    UObject mid = AddChild(hb, label);
    AddChild(hb, rbox);
    // The middle takes the rest of the width (ESlateSizeRule::Fill = 1).
    struct ChildSize {
        float value;
        uint8_t rule, pad[3];
    } fill{1.f, 1, {0, 0, 0}};
    SetStruct(mid, "SetSize", "InSize", fill);
    SetStruct(mid, "SetPadding", "InPadding", Margin{4.f, 0.f, 4.f, 0.f});
    r.left = Track(lb);
    r.right = Track(rb);
    *holder = hb;
    return true;
}

// Children in a panel widget, or -1.
int ChildCount(UObject panel) {
    if (!panel) return -1;
    ue::Call c(panel, "GetChildrenCount");
    return c.Valid() && c.Invoke() ? c.Result<int32_t>() : -1;
}

UObject ParentOf(UObject widget) {
    if (!widget) return 0;
    ue::Call c(widget, "GetParent");
    return c.Invoke() ? c.Result<UObject>() : 0;
}

// True when our VR tab button is still in the tab row and our page still in
// the switcher of this same Options widget.
bool StillBuilt(UObject option) {
    if (!Alive(T.option) || T.option.obj != option || !Alive(T.vr_button) || !Alive(T.page) || !Alive(T.switcher))
        return false;
    return ParentOf(T.vr_button.obj) && ParentOf(T.page.obj) == T.switcher.obj;
}

void Build(UObject option) {
    mem::ScopedRegionCache rc;
    if (!L.done) {
        log::Warn("vr options: engine lookups missing; no VR tab");
        return;
    }
    // The game keeps its Options widget and shows the same one again each
    // time (the pause menu does the same: one UIM_Pause_C for the whole
    // session). Adding the tab on every opening put a second, third ... VR
    // tab and page into it: the "VR menu duplicating itself" (owner
    // 2026-11-06). The tab we already added is kept and only relabelled.
    if (StillBuilt(option)) {
        T.relabel_frames = 30;
        log::Info("vr options: Options opened again; its VR tab is already there (not added again; tab row has %d "
                  "buttons)", ChildCount(ParentOf(T.vr_button.obj)));
        return;
    }
    T = Tab{};
    T.option = Track(option);
    UObject switcher = 0, tab_button = 0, back = 0;
    ue::GetProp(option, "WidgetSwitcher_OPTION", switcher);
    ue::GetProp(option, "UIP_Button_Switcher_GAMEPAD", tab_button);
    if (!tab_button) ue::GetProp(option, "UIP_Button_Switcher_UI", tab_button);
    ue::GetProp(option, "UIP_Button_Secondary_Back", back);
    if (!switcher || !tab_button) {
        log::Warn("vr options: Options has no WidgetSwitcher_OPTION / tab buttons (%s / %s); no VR tab",
                  switcher ? "found" : "missing", tab_button ? "found" : "missing");
        return;
    }
    ue::Call gp(tab_button, "GetParent");
    UObject tab_row = gp.Invoke() ? gp.Result<UObject>() : 0;
    ue::Call op(option, "GetOwningPlayer");
    UObject owner = op.Invoke() ? op.Result<UObject>() : 0;
    UObject tab_class = ue::GetClass(tab_button);
    UObject row_class = back ? ue::GetClass(back) : tab_class;
    if (!tab_row) {
        log::Warn("vr options: the tab buttons have no parent panel; no VR tab");
        return;
    }
    if (!g_click_fn) {
        UObject row_fn = FindClickFunction(row_class);
        UObject tab_fn = FindClickFunction(tab_class);
        if (!row_fn || !tab_fn) {
            log::Warn("vr options: the game's button click event was not found (rows %s, tab %s); no VR tab",
                      row_fn ? "ok" : "missing", tab_fn ? "ok" : "missing");
            return;
        }
        ue::process_event::On(row_fn, nullptr, &AfterClick);
        if (tab_fn != row_fn) ue::process_event::On(tab_fn, nullptr, &AfterClick);
        g_click_fn = row_fn;
        log::Info("vr options: following clicks through %s.%s%s", ue::GetName(ue::GetOuter(row_fn)).c_str(),
                  ue::GetName(row_fn).c_str(), tab_fn != row_fn ? " (and the tab buttons' own)" : "");
    }
    static bool s_study = false;
    if (!s_study) {
        s_study = true;
        LogClass(tab_class, "tab button");
        LogClass(row_class, "row button");
    }

    // The VR tab button, after the game's own.
    UObject vr = CreateUserWidget(option, tab_class, owner);
    if (!vr) {
        log::Warn("vr options: could not make the VR tab button");
        return;
    }
    ue::process_event::HookVtableOf(vr);
    std::vector<std::string> names;
    SetBlueprintTexts(vr, L"VR", &names);
    const int tabs_before = ChildCount(tab_row);
    UObject vr_slot = AddChild(tab_row, vr);
    int copied = CopySlotLayout(SlotOf(tab_button), vr_slot);
    T.vr_button = Track(vr);
    T.vr_button_texts = FindTexts(vr);
    for (UObject t : T.vr_button_texts) SetText(t, L"VR");
    std::string text_names;
    for (auto& n : names) text_names += n + " ";
    log::Info("vr options: VR tab button added to %s (%s; %d layout values copied; %zu text(s); label variables: %s; "
              "the row had %d buttons before)",
              ue::GetName(tab_row).c_str(), ue::GetName(tab_class).c_str(), copied, T.vr_button_texts.size(),
              text_names.empty() ? "none" : text_names.c_str(), tabs_before);

    // The page: a scroll box of rows, one per setting.
    UObject tree = 0;
    ue::GetProp(option, "WidgetTree", tree);
    ue::Call so(L.gps, "SpawnObject");
    so.Set("ObjectClass", L.scroll_class).Set("Outer", tree ? tree : option);
    UObject page = so.Invoke() ? so.Result<UObject>() : 0;
    if (!page) {
        log::Warn("vr options: could not make the VR page (ScrollBox)");
        return;
    }
    UObject page_slot = AddChild(switcher, page);
    T.switcher = Track(switcher);
    T.page = Track(page);
    T.row_class = row_class;
    T.owner = owner;
    T.outer = tree ? tree : option;
    T.building = page_slot != 0;
    T.rows.reserve(vr_menu::Rows().size());
    if (!page_slot) log::Warn("vr options: the VR page could not be added to the Options switcher");
}

// One row: the middle button, and for a setting its < > arrows.
void BuildRow(size_t i) {
    const auto& rows = vr_menu::Rows();
    UObject option = T.option.obj;
    UObject w = CreateUserWidget(option, T.row_class, T.owner);
    if (!w) return;
    ue::process_event::HookVtableOf(w);
    RowWidget r;
    r.w = Track(w);
    r.row = static_cast<int>(i);
    r.choice = vr_menu::CurrentChoice(rows[i], &Current);
    SetBlueprintTexts(w, Widen(vr_menu::RowText(rows[i], r.choice)), nullptr);
    UObject holder = w;
    if (rows[i].kind == vr_menu::RowKind::kChoice && AddArrows(r, w, T.outer, option, T.row_class, T.owner, &holder))
        ++T.with_arrows;
    if (UObject slot = AddChild(T.page.obj, holder)) SetStruct(slot, "SetPadding", "InPadding", Margin{0.f, 3.f, 0.f, 3.f});
    r.texts = FindTexts(r.w.obj);
    if (Alive(r.left)) r.left_texts = FindTexts(r.left.obj);
    if (Alive(r.right)) r.right_texts = FindTexts(r.right.obj);
    T.rows.push_back(r);
    LabelRow(T.rows.back());
}

// From Tick while Options is open: rows until 3 ms of this frame are spent
// (at least one), then the next frame.
void BuildSomeRows() {
    if (!T.building || !Alive(T.page) || !Alive(T.option)) return;
    mem::ScopedRegionCache rc;
    const double t0 = perf::NowMs();
    const auto& rows = vr_menu::Rows();
    do {
        BuildRow(T.next_row++);
    } while (T.next_row < rows.size() && perf::NowMs() - t0 < 3.0);
    const double ms = perf::NowMs() - t0;
    ++T.build_frames;
    T.build_ms += ms;
    T.build_worst_ms = std::max(T.build_worst_ms, ms);
    if (T.next_row < rows.size()) return;
    T.building = false;
    RelabelAll();
    T.relabel_frames = 30;
    size_t texts = 0;
    for (auto& r : T.rows) texts += r.texts.size();
    log::Info("vr options: VR page added to Options (%zu rows from %s, %zu texts, %d rows with < > arrows%s; made over "
              "%d frames, %.1f ms in all, %.1f ms at most in one frame)",
              T.rows.size(), ue::GetName(T.row_class).c_str(), texts, T.with_arrows,
              L.hbox_class && L.sizebox_class ? "" : "; HorizontalBox/SizeBox class missing", T.build_frames, T.build_ms,
              T.build_worst_ms);
}

}  // namespace

void InitGame() {
    L.wbl = ue::FindObject("WidgetBlueprintLibrary /Script/UMG.Default__WidgetBlueprintLibrary");
    L.gps = ue::FindObject("GameplayStatics /Script/Engine.Default__GameplayStatics");
    L.ktl = ue::FindObject("KismetTextLibrary /Script/Engine.Default__KismetTextLibrary");
    L.scroll_class = ue::FindObject("Class /Script/UMG.ScrollBox");
    L.text_class = ue::FindObject("Class /Script/UMG.TextBlock");
    L.rich_text_class = ue::FindObject("Class /Script/UMG.RichTextBlock");
    L.panel_class = ue::FindObject("Class /Script/UMG.PanelWidget");
    L.user_widget_class = ue::FindObject("Class /Script/UMG.UserWidget");
    L.hbox_class = ue::FindObject("Class /Script/UMG.HorizontalBox");
    L.sizebox_class = ue::FindObject("Class /Script/UMG.SizeBox");
    L.done = L.wbl && L.gps && L.ktl && L.scroll_class && L.text_class && L.panel_class && L.user_widget_class;
    if (L.done)
        log::Info("vr options: ready (a VR tab is added to Options)");
    else
        log::Warn("vr options: engine lookups failed (widgets %d, statics %d, text %d, scroll %d, textblock %d); no VR "
                  "tab",
                  L.wbl ? 1 : 0, L.gps ? 1 : 0, L.ktl ? 1 : 0, L.scroll_class ? 1 : 0, L.text_class ? 1 : 0);
}

void OnMenuHosted(UObject menu, const std::string& class_name) {
    if (class_name != "UIM_Option_C") return;
    Build(menu);
}

void Tick() {
    if (!Alive(T.option)) {
        if (T.option.obj) T = Tab{};
        return;
    }
    if (T.building) BuildSomeRows();
    if (T.relabel_frames > 0) {
        --T.relabel_frames;
        RelabelAll();
    }
}

}  // namespace holevr::vr::vr_options
