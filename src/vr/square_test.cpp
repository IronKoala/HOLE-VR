#include "vr/square_test.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <limits>
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
// Lone dots more in one eye than in the other eye's same strip, in one strip
// (Sweep::best_strip). The owner's 15:2x run: up to about 90 in ordinary
// strips and 147 at worst in the base, 150 to 600 where the dots were.
int g_trigger = 200;
constexpr double kSettleMs = 400;
constexpr double kCheckEveryMs = 500;
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
    // The dots seen at full size (15:2x run, capture-dots-2 and -3) repeat
    // on an 8 x 8 pixel tile with a fixed pattern inside it (mostly its left
    // four columns), the mark of a compute pass that works in 8 x 8 tiles
    // and leaves some of each tile unwritten or wrong. Lumen's screen probe
    // gather and the virtual shadow maps' one-pass projection both work that
    // way, so they go first. Already ruled out in that run: the game's flat
    // UI (none on the window) and post-process materials (no effect).
    //
    // Lumen global illumination as a whole, then its parts.
    AddCvar("r.Lumen.DiffuseIndirect.Allow", 0, false, "lumen");
    AddCvar("r.Lumen.ScreenProbeGather.IntegrationTileClassification", 0, true, "", "lumen");
    AddCvar("r.Lumen.ScreenProbeGather.ShortRangeAO", 0, true, "", "lumen");
    AddCvar("r.Lumen.ScreenProbeGather.ScreenTraces", 0, true, "", "lumen");
    AddCvar("r.Lumen.ScreenProbeGather.Temporal", 0, false, "", "lumen");
    AddCvar("r.Lumen.ScreenProbeGather.SpatialFilterProbes", 0, false, "", "lumen");
    AddCvar("r.Shadow.Virtual.OnePassProjection", 0, true);
    // Lumen reflections draw garbage into the right eye of a side-by-side
    // stereo picture on newer NVIDIA drivers (Epic's UE-220474, UE 5.5,
    // reproduced with -emulatestereo, which is how this mod renders). The
    // levels ask for screen space reflections (render fixes log), so this is
    // unlikely, but it is cheap to rule out.
    AddCvar("r.Lumen.Reflections.Allow", 0, true);
    AddCvar("r.ContactShadows", 0, true);
    AddCvar("r.VolumetricFog", 0, false);
    AddCvar("r.DistanceFieldAO", 0, false);
    AddCvar("r.AmbientOcclusionLevels", 0, false);
    AddCvar("r.Lumen.TranslucencyVolume.Enable", 0, false);
    AddCvar("r.ReflectionMethod", 0, false);
    AddCvar("r.BloomQuality", 0, false);
    AddCvar("r.PostProcessAAQuality", 0, false);
    AddCvar("r.PostProcessing.DisableMaterials", 1, false);
}

// How close the picture came to starting the test, once every 30 s when it
// came anywhere near, so a run where the dots showed but stayed under the
// trigger still says how many there were.
int g_closest = 0, g_closest_eye = 0;
float g_closest_share = 0.f;
double g_closest_since = -1;
void NoteClosest(const square_watch::Sweep& sw, double now) {
    if (g_closest_since < 0) g_closest_since = now;
    const int eye = sw.best_strip[1] > sw.best_strip[0] ? 1 : 0;
    if (sw.best_strip[eye] > g_closest) {
        g_closest = sw.best_strip[eye];
        g_closest_eye = eye;
        g_closest_share = sw.grid_share[eye];
    }
    if (now - g_closest_since < 30000) return;
    if (g_closest >= g_trigger / 3)
        log::Info("square test: most lone dots in one strip of one eye beyond the other eye's in the last 30 s: %d "
                  "(%s eye, %.0f%% of that eye's dots on one cell of an 8x4 grid; the test starts at %d)",
                  g_closest, g_closest_eye ? "right" : "left", 100.f * g_closest_share, g_trigger);
    g_closest = 0;
    g_closest_since = now;
}

bool Alive(const Suspect& s) {
    if (s.kind == Kind::kCvar) return true;
    if (!s.obj || s.index < 0 || ue::objects::Get(s.index) != s.obj) return false;
    uint32_t flags = mem::ReadOr<uint32_t>(s.obj + ue::off::kObjFlags, kSkipFlags);
    return !(flags & kSkipFlags);
}

// Game thread.
// Why the last SwitchOff returned false.
enum class Skip { kNone, kAlready, kUnreadable, kRefused, kGone } g_skip = Skip::kNone;

bool SwitchOff(Suspect& s, UObject player) {
    g_skip = Skip::kNone;
    switch (s.kind) {
        case Kind::kCvar: {
            constexpr int kUnread = std::numeric_limits<int>::min();
            const int was = cvars::ReadIntNow(s.cvar, kUnread);
            if (was == kUnread) {
                g_skip = Skip::kUnreadable;
                return false;
            }
            s.original = was;
            if (was == s.test_value) {  // already off: nothing to test
                g_skip = Skip::kAlready;
                return false;
            }
            cvars::RunNow(s.cvar + " " + std::to_string(s.test_value), player);
            // A console command alone is no evidence: the engine refuses a
            // value set with lower priority than the one it has. Read it back.
            const int now = cvars::ReadIntNow(s.cvar, kUnread);
            if (now != s.test_value) {
                cvars::RunNow(s.cvar + " " + std::to_string(was), player);
                log::Info("square test: %s: asked for %d but it reads %d; not tested", s.cvar.c_str(), s.test_value,
                          now);
                g_skip = Skip::kRefused;
                return false;
            }
            log::Info("square test: %s now reads %d (was %d)", s.cvar.c_str(), now, was);
            return true;
        }
        case Kind::kWidget: {
            if (!Alive(s)) {
                g_skip = Skip::kGone;
                return false;
            }
            float op = 1.f;
            ue::GetProp(s.obj, "RenderOpacity", op);
            s.original_opacity = op;
            ue::Call c(s.obj, "SetRenderOpacity");
            c.Set("InOpacity", 0.f);
            return c.Invoke();
        }
        case Kind::kScreenComponent: {
            if (!Alive(s)) {
                g_skip = Skip::kGone;
                return false;
            }
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

// Dots more in the bad eye, added up over the strips where it has many more.
int BadEyeExcess(const square_watch::Sweep& s) { return s.excess[g_eye]; }

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
            {
                const int eye = sw.best_strip[1] > sw.best_strip[0] ? 1 : 0;
                // Both checks have to point at the same eye.
                g_strong = sw.best_strip[eye] >= g_trigger && (g_strong == 0 || eye == g_eye) ? g_strong + 1 : 0;
                if (g_strong > 0) g_eye = eye;
            }
            NoteClosest(sw, now);
            if (g_strong < kStrongSweeps) return;
            log::Info("square test: %d lone dots more in the %s eye than the other in its worst strip (%d over all "
                      "such strips; whole picture %d vs %d); looking for what draws them",
                      sw.best_strip[g_eye], EyeName(g_eye), sw.excess[g_eye], sw.dots[g_eye], sw.dots[1 - g_eye]);
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
                const char* why = g_skip == Skip::kAlready      ? "already at that value"
                                  : g_skip == Skip::kUnreadable ? "the setting can't be read"
                                  : g_skip == Skip::kRefused    ? "the engine did not take the new value"
                                  : g_skip == Skip::kGone       ? "gone"
                                                                : "could not switch it";
                log::Info("square test: %s: skipped (%s)", s.label.c_str(), why);
                if (g_skip == Skip::kAlready) {
                    s.verdict = Suspect::kNoEffect;
                    s.results = "already " + std::to_string(s.original);
                } else if (s.kind == Kind::kCvar) {
                    s.verdict = Suspect::kUnsure;
                    s.results = why;
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
    g_on = config::GetBool("square_test", false);
    g_trigger = std::max(square_watch::kStripExcess, config::GetInt("square_test_trigger", 200));
    if (!g_on) {
        log::Info("square test: off (square_test=1 in hole-vr.ini turns it on)");
        return;
    }
    if (!square_watch::On()) {  // it only acts on what the watch sees
        g_on = false;
        log::Info("square test: off (needs square_watch=1)");
        return;
    }
    AddRenderSuspects();
    for (int i = 0; i < static_cast<int>(g_suspects.size()); ++i) g_queue.push_back(i);
    game::AddTickListener(&Tick);
    log::Info("square test: ready; when one strip of one eye has %d+ more lone dots than the other eye's it lists "
              "the game's flat UI and switches suspects off for about a second each (square_test=0 turns it off)",
              g_trigger);
}

}  // namespace holevr::vr::square_test
