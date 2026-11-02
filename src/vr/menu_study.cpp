#include "vr/menu_study.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
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
#include "ue/objects.h"
#include "ue/process_event.h"
#include "ue/reflection.h"
#include "vr/openxr.h"
#include "vr/ui_panel.h"

namespace holevr::vr::menu_study {
namespace {

using ue::UObject;

constexpr uint32_t RF_ClassDefaultObject = 0x10;
constexpr uint32_t RF_BeginDestroyed = 0x8000;
constexpr uint32_t RF_FinishDestroyed = 0x10000;

bool g_on = false;
UObject g_wbgc = 0;  // WidgetBlueprintGeneratedClass

double Now() { return GetTickCount64() / 1000.0; }

bool StartsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// ------------------------------------------------------ event trace --

std::mutex g_mutex;
// Per class: 0 = not traced, 1 = traced.
std::unordered_map<UObject, uint8_t> g_class_kind;
std::unordered_map<UObject, std::string> g_fn_names;
std::atomic<int> g_trace_lines{0};
constexpr int kMaxTraceLines = 4000;
std::string g_last_line;
int g_repeat = 0;

// Counting what still fires while a menu is open.
std::atomic<bool> g_counting{false};
std::unordered_map<std::string, int> g_counts;
std::atomic<int> g_player_ticks{0};

// Menus the game has put up: a UIM_ widget's Construct (added to the screen
// or a panel) until its Destruct (removed). UIM_HUD is the in-game HUD.
std::unordered_set<UObject> g_open_menus;
std::atomic<int> g_open_count{0};
std::atomic<int> g_menu_seq{0};

bool TracedClassName(const std::string& n) {
    if (StartsWith(n, "HUDP_")) return false;  // in-game HUD parts: noisy, not menus
    return StartsWith(n, "UIM_") || StartsWith(n, "UIP_") || StartsWith(n, "UIS_") ||
           StartsWith(n, "BP_GameInstance") || StartsWith(n, "ALS_CustomPlayerController") ||
           StartsWith(n, "BP_UIControllPawn") || StartsWith(n, "BP_FPSGameMode") ||
           StartsWith(n, "SYS_LevelManager") || n.find("HUD") != std::string::npos;
}

bool NoisyFunction(const std::string& n) {
    return n.find("Tick") != std::string::npos || n.find("Paint") != std::string::npos ||
           n.find("__UpdateFunc") != std::string::npos || n.find("FinishedFunc") != std::string::npos ||
           StartsWith(n, "Get") || StartsWith(n, "InpAxisEvt") || n.find("Hovered") != std::string::npos ||
           n.find("Unhovered") != std::string::npos || n == "ReceiveDrawHUD" || n == "OnMouseMove";
}

const std::string& FnName(UObject fn) {
    auto it = g_fn_names.find(fn);
    if (it != g_fn_names.end()) return it->second;
    return g_fn_names.emplace(fn, ue::GetName(fn)).first->second;
}

void OnAnyEvent(UObject self, UObject function, void*) {
    if (!g_on || !self || !function) return;
    UObject cls = ue::GetClass(self);
    if (!cls) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_class_kind.find(cls);
    if (it == g_class_kind.end())
        it = g_class_kind.emplace(cls, TracedClassName(ue::GetName(cls)) ? 1 : 0).first;
    const std::string& fn = FnName(function);
    if (g_counting.load(std::memory_order_relaxed)) {
        std::string key = ue::GetName(cls) + "." + fn;
        if (g_counts.size() < 4000 || g_counts.count(key)) ++g_counts[key];
    }
    if (it->second && (fn == "Construct" || fn == "Destruct")) {
        std::string cn = ue::GetName(cls);
        if (StartsWith(cn, "UIM_") && cn != "UIM_HUD_C") {
            bool changed = fn == "Construct" ? g_open_menus.insert(self).second : g_open_menus.erase(self) > 0;
            if (changed) {
                g_open_count = static_cast<int>(g_open_menus.size());
                ++g_menu_seq;
                log::Info("menu study: MENU %s %s (%d menu(s) up)", fn == "Construct" ? "UP" : "DOWN",
                          ue::GetName(self).c_str(), g_open_count.load());
            }
        }
    }
    if (!it->second || NoisyFunction(fn)) return;
    if (g_trace_lines.load(std::memory_order_relaxed) >= kMaxTraceLines) return;
    std::string line = ue::GetName(self) + "." + fn;
    if (line == g_last_line) {
        ++g_repeat;
        return;
    }
    if (g_repeat) log::Info("menu study: event   (previous line repeated %d more times)", g_repeat);
    g_repeat = 0;
    g_last_line = line;
    ++g_trace_lines;
    log::Info("menu study: event   %s  (outer %s)", line.c_str(),
              ue::GetName(ue::GetOuter(self)).c_str());
}

void OnPlayerTick(UObject, float) { ++g_player_ticks; }

// ------------------------------------------------------- snapshots --

std::string EnumOrByte(UObject obj, const char* prop) {
    uint8_t v = 0;
    if (!ue::GetProp(obj, prop, v)) return "?";
    static const char* kVis[] = {"Visible", "Collapsed", "Hidden", "HitTestInvisible", "SelfHitTestInvisible"};
    if (std::strcmp(prop, "Visibility") == 0 && v < 5) return kVis[v];
    return std::to_string(v);
}

bool IsStudiedWidgetClass(UObject cls) {
    if (!g_wbgc) g_wbgc = ue::FindObject("Class /Script/UMG.WidgetBlueprintGeneratedClass");
    if (!g_wbgc || ue::GetClass(cls) != g_wbgc) return false;
    std::string n = ue::GetName(cls);
    return StartsWith(n, "UIM_") || StartsWith(n, "UIP_") || StartsWith(n, "UIS_") || StartsWith(n, "HUD");
}

void Snapshot(const char* why) {
    if (!ue::objects::Ready() || !ue::reflection::Ready()) return;
    struct Row {
        std::string path, cls, vis, parent;
        float opacity;
    };
    std::vector<Row> rows;
    std::unordered_map<UObject, bool> class_ok;
    ue::objects::ForEach([&](int, UObject o) {
        UObject cls = ue::GetClass(o);
        if (!cls) return;
        auto it = class_ok.find(cls);
        if (it == class_ok.end()) it = class_ok.emplace(cls, IsStudiedWidgetClass(cls)).first;
        if (!it->second || StartsWith(ue::GetName(cls), "HUDP_")) return;
        uint32_t flags = mem::ReadOr<uint32_t>(o + ue::off::kObjFlags, RF_ClassDefaultObject);
        if (flags & (RF_ClassDefaultObject | RF_BeginDestroyed | RF_FinishDestroyed)) return;
        Row r;
        r.path = ue::GetPathName(o);
        r.cls = ue::GetName(cls);
        r.vis = EnumOrByte(o, "Visibility");
        r.opacity = -1.f;
        ue::GetProp(o, "RenderOpacity", r.opacity);
        UObject slot = 0, parent = 0;
        if (ue::GetProp(o, "Slot", slot) && slot && ue::GetProp(slot, "Parent", parent) && parent)
            r.parent = ue::GetName(ue::GetClass(parent)) + " " + ue::GetName(parent);
        else
            r.parent = "(none: top level or root of its widget tree)";
        rows.push_back(std::move(r));
    });
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.path < b.path; });
    // The controller: which pawn it holds, and whether the cursor is on.
    UObject player = game::Player(), pc = 0, pawn = 0;
    if (player) ue::GetProp(player, "Controller", pc);
    if (pc) ue::GetProp(pc, "Pawn", pawn);
    log::Info("menu study: ---- snapshot (%s): %zu widgets; menu open=%d; player %s; controller %s; pawn %s; "
              "player ticks in last poll window %d",
              why, rows.size(), ui_panel::MenuOpen() ? 1 : 0, player ? ue::GetName(ue::GetClass(player)).c_str() : "none",
              pc ? ue::GetName(ue::GetClass(pc)).c_str() : "none", pawn ? ue::GetName(ue::GetClass(pawn)).c_str() : "none",
              g_player_ticks.load());
    int n = 0;
    for (const auto& r : rows) {
        if (++n > 400) {
            log::Info("menu study:   ... %zu more", rows.size() - 400);
            break;
        }
        log::Info("menu study:   %s | vis %s | opacity %.2f | parent %s", r.path.c_str(), r.vis.c_str(), r.opacity,
                  r.parent.c_str());
    }
    log::Info("menu study: ---- end of snapshot (%s)", why);
}

void DumpCounts(const char* why) {
    std::vector<std::pair<std::string, int>> list;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        list.assign(g_counts.begin(), g_counts.end());
        g_counts.clear();
    }
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string s;
    for (size_t i = 0; i < list.size() && i < 16; ++i)
        s += " " + list[i].first + " x" + std::to_string(list[i].second) + ";";
    log::Info("menu study: events in the last second (%s), %zu kinds:%s", why, list.size(), s.c_str());
}

// ---------------------------------------------------------- vtables --

std::unordered_set<UObject> g_hooked_classes;

void HookFirstInstanceNamed(const char* class_name) {
    UObject found = 0;
    ue::objects::ForEach([&](int, UObject o) {
        if (found) return;
        UObject cls = ue::GetClass(o);
        if (!cls || g_hooked_classes.count(cls)) return;
        if (ue::GetName(cls) != class_name) return;
        uint32_t flags = mem::ReadOr<uint32_t>(o + ue::off::kObjFlags, RF_ClassDefaultObject);
        if (flags & (RF_ClassDefaultObject | RF_BeginDestroyed | RF_FinishDestroyed)) return;
        found = o;
    });
    if (!found) return;
    g_hooked_classes.insert(ue::GetClass(found));
    bool ok = ue::process_event::HookVtableOf(found);
    log::Info("menu study: %s events %s", class_name, ok ? "followed" : "could not be followed");
}

}  // namespace

void InitGame() {
    g_on = config::GetBool("menu_study", true);
    if (!g_on) return;
    const char* cdos[] = {"UserWidget /Script/UMG.Default__UserWidget",
                          "GameInstance /Script/Engine.Default__GameInstance"};
    for (const char* name : cdos) {
        UObject cdo = ue::FindObject(name);
        bool ok = cdo && ue::process_event::HookVtableOf(cdo);
        log::Info("menu study: %s %s", name, ok ? "followed" : "not found");
    }
    ue::process_event::OnAny(&OnAnyEvent);
    game::AddTickListener(&OnPlayerTick);
    log::Info("menu study: on (logging only; menu_study=0 turns it off)");
}

void Poll() {
    if (!g_on) return;
    static bool s_open = false;
    static double s_open_at = -1, s_closed_at = -1, s_player_since = -1, s_last_count = 0, s_last_hook = 0;
    static int s_snaps = 0, s_count_dumps = 0, s_step = 0, s_close_step = 0;
    static bool s_play_done = false;
    const double now = Now();

    if (now - s_last_hook > 2.0) {
        s_last_hook = now;
        HookFirstInstanceNamed("BP_GameInstance_C");
        HookFirstInstanceNamed("UIM_Pause_C");
        HookFirstInstanceNamed("UIM_Title_C");
        HookFirstInstanceNamed("ALS_CustomPlayerController_C");
        HookFirstInstanceNamed("ALS_PlayerCameraManager_C");
    }

    // A menu is up while the game has a UIM_ menu constructed (the mouse
    // cursor doesn't show in VR, so it can't be used for this).
    bool open = g_open_count.load() > 0;
    static int s_seq = 0;
    if (open && s_open && g_menu_seq.load() != s_seq) {
        // Another menu came up on top (e.g. Options from Pause): study it too.
        s_open_at = now;
        s_step = 0;
    }
    s_seq = g_menu_seq.load();
    if (open != s_open) {
        s_open = open;
        log::Info("menu study: menu %s (cursor %s)", open ? "OPENED" : "CLOSED", ui_panel::MenuOpen() ? "on" : "off");
        if (open) {
            s_open_at = now;
            s_step = 0;
            s_count_dumps = 0;
            g_counting = true;
            s_last_count = now;
            std::lock_guard<std::mutex> lock(g_mutex);
            g_counts.clear();
        } else {
            s_closed_at = now;
            s_close_step = 0;
            if (g_counting) DumpCounts("menu closing");
            g_counting = false;
        }
    }
    if (open && s_snaps < 24) {
        double t = now - s_open_at;
        if (s_step == 0 && t > 0.05) { ++s_step; ++s_snaps; Snapshot("menu just opened"); }
        if (s_step == 1 && t > 0.7) { ++s_step; ++s_snaps; Snapshot("menu open 0.7 s"); }
        if (s_step == 2 && t > 1.2) {
            ++s_step;
            static int s_caps = 0;
            if (++s_caps <= 6) xr::RequestCapture(("menu" + std::to_string(s_caps)).c_str());
        }
        if (s_step == 3 && t > 3.0) { ++s_step; ++s_snaps; Snapshot("menu open 3 s"); }
    }
    if (open && g_counting && now - s_last_count >= 1.0) {
        s_last_count = now;
        if (++s_count_dumps <= 40) DumpCounts("menu open");
        g_player_ticks = 0;
    }
    if (!open && s_closed_at > 0 && s_close_step == 0 && now - s_closed_at > 0.7 && s_snaps < 16) {
        s_close_step = 1;
        ++s_snaps;
        Snapshot("menu closed 0.7 s");
    }
    // Once, during play: what is on screen before any menu.
    if (game::Player() && !open) {
        if (s_player_since < 0) s_player_since = now;
        if (!s_play_done && now - s_player_since > 6.0) {
            s_play_done = true;
            g_player_ticks = 0;
            Snapshot("during play, no menu");
            xr::RequestCapture("play");
        }
    } else if (!game::Player()) {
        s_player_since = -1;
    }
}

}  // namespace holevr::vr::menu_study
