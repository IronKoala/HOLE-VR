#include "vr/square_test.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "core/perf.h"
#include "game/game.h"
#include "ue/call.h"
#include "ue/objects.h"
#include "ue/reflection.h"
#include "vr/cvars.h"
#include "vr/render_fixes.h"
#include "vr/square_watch.h"

namespace holevr::vr::square_test {
namespace {
using ue::UObject;

// EObjectFlags of templates and objects on their way out.
constexpr uint32_t kSkipFlags = 0x10 | 0x20 | 0x8000 | 0x10000;
constexpr uint8_t kWidgetSpaceScreen = 1;  // EWidgetSpace::Screen

bool g_on = false;
int g_trigger = 250;  // lone dots more in one eye than the other, whole picture
constexpr double kSettleMs = 400;
constexpr double kCheckEveryMs = 1000;
constexpr int kStrongSweeps = 2;  // in a row before a test starts
constexpr int kMaxTries = 3;

// ------------------------------------------------------------- suspects --

enum class Kind { kCvar, kWidget, kScreenComponent };

struct Suspect {
    Kind kind = Kind::kCvar;
    std::string label;
    // kCvar
    std::string cvar;
    int test_value = 0;
    int original = 0;
    // kWidget, kScreenComponent
    UObject obj = 0;
    int index = -1;
    float original_opacity = 1.f;
    bool original_visible = true;
    // Left switched off for the rest of the run when it clears the dots
    // (things you wouldn't notice gone).
    bool keep = false;
    // Groups: a suspect with `needs` is only tried once the coarse suspect
    // of that group cleared the dots.
    std::string group, needs;
    int tries = 0;
    int clears = 0;
    enum Verdict { kUntested, kClears, kNoEffect, kUnsure } verdict = kUntested;
    std::string results;  // "1650 -> 12 -> 1580" per try, for the summary
};

std::vector<Suspect> g_suspects;
std::deque<int> g_queue;  // indexes into g_suspects, next first
std::vector<std::string> g_found_groups;

void AddCvar(const char* name, int value, bool keep, const char* group = "", const char* needs = "") {
    Suspect s;
    s.kind = Kind::kCvar;
    s.cvar = name;
    s.test_value = value;
    s.label = std::string(name) + " " + std::to_string(value);
    s.keep = keep;
    s.group = group;
    s.needs = needs;
    g_suspects.push_back(s);
}

// Render features, coarse ones first. Each is safe to switch at runtime.
void AddRenderSuspects() {
    // Post-process materials (the game's own screen effects, e.g. its retro
    // look) work on the finished picture, per eye.
    AddCvar("r.PostProcessing.DisableMaterials", 1, false);
    // Lumen global illumination as a whole, then its parts.
    AddCvar("r.Lumen.DiffuseIndirect.Allow", 0, false, "lumen");
    AddCvar("r.Lumen.ScreenProbeGather.IntegrationTileClassification", 0, true, "", "lumen");
    AddCvar("r.Lumen.ScreenProbeGather.ScreenTraces", 0, true, "", "lumen");
    AddCvar("r.Lumen.ScreenProbeGather.ShortRangeAO", 0, true, "", "lumen");
    AddCvar("r.Lumen.ScreenProbeGather.Temporal", 0, false, "", "lumen");
    AddCvar("r.Shadow.Virtual.OnePassProjection", 0, true);
    AddCvar("r.VolumetricFog", 0, false);
    AddCvar("r.ReflectionMethod", 0, false);
    AddCvar("r.Lumen.TranslucencyVolume.Enable", 0, false);
    AddCvar("r.ContactShadows", 0, true);
    AddCvar("r.DistanceFieldAO", 0, false);
    AddCvar("r.AmbientOcclusionLevels", 0, false);
    AddCvar("r.BloomQuality", 0, false);
    AddCvar("r.PostProcessAAQuality", 0, false);
}

bool Alive(const Suspect& s) {
    if (s.kind == Kind::kCvar) return true;
    if (!s.obj || s.index < 0 || ue::objects::Get(s.index) != s.obj) return false;
    uint32_t flags = mem::ReadOr<uint32_t>(s.obj + ue::off::kObjFlags, kSkipFlags);
    return !(flags & kSkipFlags);
}

// Game thread.
bool SwitchOff(Suspect& s, UObject player) {
    switch (s.kind) {
        case Kind::kCvar:
            s.original = cvars::ReadIntNow(s.cvar, 0);
            if (s.original == s.test_value) return false;  // already off: nothing to test
            cvars::RunNow(s.cvar + " " + std::to_string(s.test_value), player);
            return true;
        case Kind::kWidget: {
            if (!Alive(s)) return false;
            float op = 1.f;
            ue::GetProp(s.obj, "RenderOpacity", op);
            s.original_opacity = op;
            ue::Call c(s.obj, "SetRenderOpacity");
            c.Set("InOpacity", 0.f);
            return c.Invoke();
        }
        case Kind::kScreenComponent: {
            if (!Alive(s)) return false;
            ue::Call v(s.obj, "IsVisible");
            s.original_visible = !v.Invoke() || v.Result<bool>();
            ue::Call c(s.obj, "SetVisibility");
            c.Set("bNewVisibility", false).Set("bPropagateToChildren", true);
            return c.Invoke();
        }
    }
    return false;
}

void PutBack(Suspect& s, UObject player) {
    switch (s.kind) {
        case Kind::kCvar:
            cvars::RunNow(s.cvar + " " + std::to_string(s.original), player);
            break;
        case Kind::kWidget:
            if (Alive(s)) {
                ue::Call c(s.obj, "SetRenderOpacity");
                c.Set("InOpacity", s.original_opacity);
                c.Invoke();
            }
            break;
        case Kind::kScreenComponent:
            if (Alive(s)) {
                ue::Call c(s.obj, "SetVisibility");
                c.Set("bNewVisibility", s.original_visible).Set("bPropagateToChildren", true);
                c.Invoke();
            }
            break;
    }
}

// ---------------------------------------------------------------- census --
// Every widget the game has on its window, and every widget component it
// draws flat (Space = Screen). A background scan finds the candidates; the
// game thread asks each one whether it is on the screen.

struct Found {
    UObject obj;
    int index;
    bool component;
};
std::mutex g_scan_mutex;
std::vector<Found> g_scan;
std::atomic<int> g_scan_state{0};  // 0 none, 1 running, 2 done

void StartScan() {
    g_scan_state = 1;
    std::thread([] {
        static UObject s_user_widget = 0, s_widget_comp = 0;
        if (!s_user_widget) s_user_widget = ue::FindObject("Class /Script/UMG.UserWidget");
        if (!s_widget_comp) s_widget_comp = ue::FindObject("Class /Script/UMG.WidgetComponent");
        std::vector<Found> found;
        if (s_user_widget || s_widget_comp) {
            ue::objects::ForEach([&](int index, UObject o) {
                uint32_t flags = mem::ReadOr<uint32_t>(o + ue::off::kObjFlags, kSkipFlags);
                if (flags & kSkipFlags) return;
                UObject cls = ue::GetClass(o);
                if (!cls) return;
                if (s_user_widget && ue::reflection::IsChildOf(cls, s_user_widget))
                    found.push_back({o, index, false});
                else if (s_widget_comp && ue::reflection::IsChildOf(cls, s_widget_comp))
                    found.push_back({o, index, true});
            });
        }
        {
            std::lock_guard<std::mutex> lock(g_scan_mutex);
            g_scan.swap(found);
        }
        g_scan_state = 2;
    }).detach();
}

const char* VisibilityName(uint8_t v) {
    static const char* const kNames[] = {"visible", "collapsed", "hidden", "not hit-testable (self & all)",
                                         "not hit-testable (self only)"};
    return v < 5 ? kNames[v] : "?";
}

// Game thread, once the scan is done: logs the flat UI and puts each piece
// that is drawn at the front of the queue.
void Census() {
    std::vector<Found> found;
    {
        std::lock_guard<std::mutex> lock(g_scan_mutex);
        found.swap(g_scan);
    }
    g_scan_state = 0;
    // Widgets from an earlier level are gone; drop them.
    std::vector<Suspect> kept;
    for (auto& s : g_suspects)
        if (s.kind == Kind::kCvar) kept.push_back(s);
    g_suspects.swap(kept);
    g_queue.clear();
    int on_screen = 0, drawn = 0, screen_comps = 0, widgets = 0, comps = 0;
    std::vector<int> flat;
    for (const Found& f : found) {
        if (ue::objects::Get(f.index) != f.obj) continue;
        const std::string cls = ue::GetName(ue::GetClass(f.obj));
        if (!f.component) {
            ++widgets;
            ue::Call in(f.obj, "IsInViewport");
            if (!in.Invoke() || !in.Result<bool>()) continue;
            ++on_screen;
            uint8_t vis = 0;
            float op = 1.f;
            ue::GetProp(f.obj, "Visibility", vis);
            ue::GetProp(f.obj, "RenderOpacity", op);
            const bool shown = (vis == 0 || vis == 3 || vis == 4) && op > 0.f;
            log::Info("square test: on the game window: %s (%s), %s, opacity %.3f%s", cls.c_str(),
                      ue::GetName(f.obj).c_str(), VisibilityName(vis), op, shown ? "" : " (not drawn)");
            if (!shown) continue;
            ++drawn;
            Suspect s;
            s.kind = Kind::kWidget;
            s.label = "flat widget " + cls;
            s.obj = f.obj;
            s.index = f.index;
            s.keep = true;  // flat UI only ever shows in one eye
            g_suspects.push_back(s);
            flat.push_back(static_cast<int>(g_suspects.size()) - 1);
        } else {
            ++comps;
            uint8_t space = 0;
            if (!ue::GetProp(f.obj, "Space", space) || space != kWidgetSpaceScreen) continue;
            ue::Call v(f.obj, "IsVisible");
            const bool visible = v.Invoke() && v.Result<bool>();
            UObject owner = ue::GetOuter(f.obj);
            log::Info("square test: widget component drawn flat on the window: %s (%s, in %s)%s", cls.c_str(),
                      ue::GetName(f.obj).c_str(), owner ? ue::GetName(owner).c_str() : "?",
                      visible ? "" : " (hidden)");
            if (!visible) continue;
            ++screen_comps;
            Suspect s;
            s.kind = Kind::kScreenComponent;
            s.label = "flat widget component " + ue::GetName(f.obj) + " (" + cls + ")";
            s.obj = f.obj;
            s.index = f.index;
            s.keep = true;
            g_suspects.push_back(s);
            flat.push_back(static_cast<int>(g_suspects.size()) - 1);
        }
    }
    log::Info("square test: flat UI: %d of %d widgets are on the game window (%d drawn); %d of %d widget components "
              "are drawn flat and visible",
              on_screen, widgets, drawn, screen_comps, comps);
    for (int i : flat) g_queue.push_back(i);
    for (int i = 0; i < static_cast<int>(g_suspects.size()); ++i)
        if (g_suspects[i].kind == Kind::kCvar && g_suspects[i].verdict != Suspect::kClears &&
            g_suspects[i].verdict != Suspect::kNoEffect)
            g_queue.push_back(i);
}

// ----------------------------------------------------------------- tests --

enum class Phase { kWatch, kScan, kBefore, kSettleOff, kOff, kSettleOn, kAfter, kDone };
Phase g_phase = Phase::kWatch;
double g_t = 0, g_since = 0, g_next_check = 0;
int g_strong = 0;
int g_current = -1;
bool g_switched = false;
int g_before = 0, g_off = 0, g_eye = 1;
UObject g_level_player = 0;  // player of the level the census is for
bool g_kept_any = false;

const char* EyeName(int e) { return e ? "right" : "left"; }

int BadEyeExcess(const square_watch::Sweep& s) { return s.dots[g_eye] - s.dots[1 - g_eye]; }

bool Ready(const Suspect& s) {
    if (s.needs.empty()) return true;
    return std::find(g_found_groups.begin(), g_found_groups.end(), s.needs) != g_found_groups.end();
}

// Next suspect worth trying, or -1.
int Next() {
    while (!g_queue.empty()) {
        int i = g_queue.front();
        g_queue.pop_front();
        Suspect& s = g_suspects[i];
        if (s.verdict == Suspect::kClears || s.verdict == Suspect::kNoEffect) continue;
        if (!Ready(s)) continue;
        if (!Alive(s)) continue;
        return i;
    }
    return -1;
}

void Summary() {
    std::string out;
    for (const Suspect& s : g_suspects) {
        if (s.verdict == Suspect::kUntested) continue;
        const char* v = s.verdict == Suspect::kClears ? "CLEARS THE DOTS"
                        : s.verdict == Suspect::kNoEffect ? "no effect"
                                                          : "unclear";
        out += " | " + s.label + ": " + v + " (" + s.results + ")";
    }
    log::Info("square test: summary (dots more in the %s eye before -> with it off -> after):%s", EyeName(g_eye),
              out.empty() ? " nothing tested" : out.c_str());
}

// Puts back whatever is switched off for the test in progress.
void Abort(UObject player, const char* why) {
    if (g_current >= 0 && g_switched) PutBack(g_suspects[g_current], player);
    if (g_current >= 0) {
        log::Info("square test: %s stopped (%s); put back", g_suspects[g_current].label.c_str(), why);
        g_queue.push_front(g_current);
    }
    g_current = -1;
    g_switched = false;
    g_phase = Phase::kWatch;
    g_strong = 0;
}

void Judge(UObject player, int after) {
    Suspect& s = g_suspects[g_current];
    ++s.tries;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s%d -> %d -> %d", s.results.empty() ? "" : ", ", g_before, g_off, after);
    s.results += buf;
    const int base = std::min(g_before, after);
    const char* what;
    if (base < g_trigger * 6 / 10) {
        what = "the dots changed on their own (you looked elsewhere?); trying again later";
        if (s.tries < kMaxTries) g_queue.push_back(g_current);
        else s.verdict = Suspect::kUnsure;
    } else if (g_off <= base / 4) {
        ++s.clears;
        if (s.clears >= 2) {
            s.verdict = Suspect::kClears;
            what = "FOUND: it clears the dots (twice)";
        } else {
            what = "it cleared the dots; checking once more";
            g_queue.push_front(g_current);
        }
    } else if (g_off >= base * 6 / 10) {
        s.verdict = Suspect::kNoEffect;
        what = "no effect";
    } else {
        what = "fewer dots but not gone; trying again later";
        if (s.tries < kMaxTries) g_queue.push_back(g_current);
        else s.verdict = Suspect::kUnsure;
    }
    log::Info("square test: %s: lone dots more in the %s eye %d before, %d with it off, %d after: %s",
              s.label.c_str(), EyeName(g_eye), g_before, g_off, after, what);
    if (s.verdict == Suspect::kClears) {
        if (!s.group.empty()) g_found_groups.push_back(s.group);
        if (s.keep && SwitchOff(s, player)) {
            g_kept_any = true;
            log::Info("square test: %s left off for the rest of this run", s.label.c_str());
        } else if (s.group.empty()) {
            log::Info("square test: %s put back (switching it off changes the picture too much to leave it)",
                      s.label.c_str());
        }
        // Its group's finer suspects go next.
        if (!s.group.empty())
            for (int i = static_cast<int>(g_suspects.size()) - 1; i >= 0; --i)
                if (g_suspects[i].needs == s.group && g_suspects[i].verdict == Suspect::kUntested)
                    g_queue.push_front(i);
    }
    g_current = -1;
    g_switched = false;
}

void Tick(UObject player, float) {
    if (!g_on || g_phase == Phase::kDone) return;
    const double now = perf::NowMs();
    // Menus up (no strips are read then) or a new level: stop the test in
    // progress and put things back.
    if (render_fixes::PauseBlurOn()) {
        if (g_current >= 0) Abort(player, "a menu came up");
        g_phase = Phase::kWatch;
        g_next_check = now + 2000;
        return;
    }
    if (player != g_level_player && g_phase != Phase::kWatch) Abort(player, "new level");

    square_watch::Sweep sw;
    switch (g_phase) {
        case Phase::kWatch: {
            if (now < g_next_check) return;
            g_next_check = now + kCheckEveryMs;
            if (!square_watch::LatestSweep(now - 1500, &sw)) {
                g_strong = 0;
                return;
            }
            g_strong = sw.Odd() >= g_trigger ? g_strong + 1 : 0;
            if (g_strong < kStrongSweeps) return;
            g_eye = sw.Eye();
            log::Info("square test: %d lone dots in the %s eye, %d in the %s eye (%.0f%% on one cell of an 8x4 "
                      "grid); looking for what draws them",
                      sw.dots[g_eye], EyeName(g_eye), sw.dots[1 - g_eye], EyeName(1 - g_eye),
                      100.f * sw.grid_share[g_eye]);
            if (player != g_level_player) {
                g_level_player = player;
                StartScan();
                g_phase = Phase::kScan;
            } else {
                g_since = now;
                g_phase = Phase::kBefore;
            }
            return;
        }
        case Phase::kScan:
            if (g_scan_state.load() != 2) return;
            Census();
            g_since = now;
            g_phase = Phase::kBefore;
            return;
        case Phase::kBefore:
            if (!square_watch::LatestSweep(g_since, &sw)) return;
            g_before = BadEyeExcess(sw);
            if (g_before < g_trigger * 6 / 10) {
                g_phase = Phase::kWatch;  // gone for now; wait for them again
                g_strong = 0;
                return;
            }
            g_current = Next();
            if (g_current < 0) {
                Summary();
                log::Info("square test: nothing left to try this run%s",
                          g_kept_any ? "" : "; nothing found that clears the dots");
                g_phase = Phase::kDone;
                return;
            }
            g_switched = SwitchOff(g_suspects[g_current], player);
            if (!g_switched) {
                Suspect& s = g_suspects[g_current];
                log::Info("square test: %s: skipped (%s)", s.label.c_str(),
                          s.kind == Kind::kCvar ? "already at that value" : "gone");
                if (s.kind == Kind::kCvar) {
                    s.verdict = Suspect::kNoEffect;
                    s.results = "already " + std::to_string(s.original);
                }
                g_current = -1;
                g_since = now;  // the baseline just taken is still good
                return;
            }
            g_t = now;
            g_phase = Phase::kSettleOff;
            return;
        case Phase::kSettleOff:
            if (now - g_t < kSettleMs) return;
            g_since = now;
            g_phase = Phase::kOff;
            return;
        case Phase::kOff:
            if (!square_watch::LatestSweep(g_since, &sw)) return;
            g_off = BadEyeExcess(sw);
            PutBack(g_suspects[g_current], player);
            g_switched = false;
            g_t = now;
            g_phase = Phase::kSettleOn;
            return;
        case Phase::kSettleOn:
            if (now - g_t < kSettleMs) return;
            g_since = now;
            g_phase = Phase::kAfter;
            return;
        case Phase::kAfter: {
            if (!square_watch::LatestSweep(g_since, &sw)) return;
            const int after = BadEyeExcess(sw);
            const bool kept_now = g_kept_any;
            Judge(player, after);
            if (g_kept_any && !kept_now) {
                // Left off: the dots should stay gone. One more summary, then stop.
                Summary();
                g_phase = Phase::kDone;
                return;
            }
            // Next suspect: a fresh "before" from here.
            g_since = now;
            g_phase = Phase::kBefore;
            if (after < g_trigger * 6 / 10) {
                g_phase = Phase::kWatch;
                g_strong = 0;
            }
            return;
        }
        case Phase::kDone:
            return;
    }
}

}  // namespace

void Init() {
    g_on = config::GetBool("square_test", true);
    g_trigger = std::max(20, config::GetInt("square_test_trigger", 250));
    if (!g_on) {
        log::Info("square test: off (square_test=0)");
        return;
    }
    AddRenderSuspects();
    for (int i = 0; i < static_cast<int>(g_suspects.size()); ++i) g_queue.push_back(i);
    game::AddTickListener(&Tick);
    log::Info("square test: ready; when one eye has %d+ more lone dots than the other it lists the game's flat UI "
              "and switches suspects off for about a second each (square_test=0 turns it off)",
              g_trigger);
}

}  // namespace holevr::vr::square_test
