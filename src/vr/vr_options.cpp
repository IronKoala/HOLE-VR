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
#include "ue/call.h"
#include "ue/objects.h"
#include "ue/process_event.h"
#include "ue/reflection.h"
#include "vr/body/body.h"
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
} L;

// A widget we made, with its object index so a freed one is never touched.
struct Made {
    UObject obj = 0;
    int index = -1;
};
int IndexOf(UObject obj) { return mem::ReadOr<int32_t>(obj + ue::off::kObjIndex, -1); }
Made Track(UObject obj) { return Made{obj, obj ? IndexOf(obj) : -1}; }
bool Alive(const Made& m) { return m.obj && m.index >= 0 && ue::objects::Get(m.index) == m.obj; }

struct RowWidget {
    Made w;
    int row = 0;      // index into vr_menu::Rows()
    int choice = 0;
    std::vector<UObject> texts;  // its TextBlocks
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
    std::wstring w = Widen(vr_menu::RowText(row, r.choice));
    for (UObject t : r.texts) SetText(t, w);
}

void RelabelAll() {
    for (auto& r : T.rows)
        if (Alive(r.w)) LabelRow(r);
    if (Alive(T.vr_button))
        for (UObject t : T.vr_button_texts) SetText(t, L"VR");
}

void ClickRow(RowWidget& r) {
    const auto& row = vr_menu::Rows()[static_cast<size_t>(r.row)];
    if (row.kind == vr_menu::RowKind::kMeasureArms) {
        body::RequestMeasure();
        log::Info("vr options: Measure arms clicked; the measure starts 5 s after the game runs again");
        input::Haptic(input::kLeft, 0.4f, 0.05f);
        return;
    }
    r.choice = (r.choice + 1) % static_cast<int>(row.choices.size());
    const auto& c = row.choices[static_cast<size_t>(r.choice)];
    std::string what;
    bool saved = true;
    for (const auto& kv : c.sets) {
        saved = config::Set(kv.key, kv.value) && saved;
        what += std::string(what.empty() ? "" : ", ") + kv.key + "=" + kv.value;
    }
    config::NotifyChanged();
    LabelRow(r);
    log::Info("vr options: %s -> %s (%s)%s", row.label, c.text, what.c_str(),
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
    for (auto& r : T.rows)
        if (Alive(r.w) && r.w.obj == self) {
            ClickRow(r);
            return;
        }
}

void Build(UObject option) {
    if (!L.done) {
        log::Warn("vr options: engine lookups missing; no VR tab");
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
    UObject vr_slot = AddChild(tab_row, vr);
    int copied = CopySlotLayout(SlotOf(tab_button), vr_slot);
    T.vr_button = Track(vr);
    T.vr_button_texts = FindTexts(vr);
    for (UObject t : T.vr_button_texts) SetText(t, L"VR");
    std::string text_names;
    for (auto& n : names) text_names += n + " ";
    log::Info("vr options: VR tab button added to %s (%s; %d layout values copied; %zu text(s); label variables: %s)",
              ue::GetName(tab_row).c_str(), ue::GetName(tab_class).c_str(), copied, T.vr_button_texts.size(),
              text_names.empty() ? "none" : text_names.c_str());

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
    const auto& rows = vr_menu::Rows();
    for (size_t i = 0; i < rows.size(); ++i) {
        UObject w = CreateUserWidget(option, row_class, owner);
        if (!w) continue;
        ue::process_event::HookVtableOf(w);
        RowWidget r;
        r.w = Track(w);
        r.row = static_cast<int>(i);
        r.choice = vr_menu::CurrentChoice(rows[i], &Current);
        SetBlueprintTexts(w, Widen(vr_menu::RowText(rows[i], r.choice)), nullptr);
        UObject slot = AddChild(page, w);
        if (slot) {
            ue::Call pad(slot, "SetPadding");
            struct Margin {
                float l, t, r, b;
            } m{0.f, 3.f, 0.f, 3.f};
            int32_t size = 0;
            if (uint8_t* p = pad.Raw("InPadding", &size); p && size == static_cast<int32_t>(sizeof(m))) {
                std::memcpy(p, &m, sizeof(m));
                pad.Invoke();
            }
        }
        T.rows.push_back(r);
    }
    UObject page_slot = AddChild(switcher, page);
    T.switcher = Track(switcher);
    T.page = Track(page);
    // Built now (the switcher is on screen): their texts exist.
    for (auto& r : T.rows) r.texts = FindTexts(r.w.obj);
    RelabelAll();
    T.relabel_frames = 30;
    size_t texts = 0;
    for (auto& r : T.rows) texts += r.texts.size();
    log::Info("vr options: VR page added to Options (%zu rows from %s, %zu texts, slot %s)", T.rows.size(),
              ue::GetName(row_class).c_str(), texts, page_slot ? "ok" : "MISSING");
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
    if (T.relabel_frames > 0) {
        --T.relabel_frames;
        RelabelAll();
    }
}

}  // namespace holevr::vr::vr_options
