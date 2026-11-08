#include "vr/gpu_test.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "game/game.h"
#include "vr/cvars.h"
#include "vr/gpu_timer.h"
#include "vr/openxr.h"
#include "vr/render_fixes.h"
#include "vr/square_test.h"
#include "vr/ui_panel.h"
#include "vr/world_menu.h"

namespace holevr::vr::gpu_test {
namespace {
using ue::UObject;

// One console variable a suspect switches. `test` is the value under test;
// one starting with '+' is added to the value the variable had ("+1").
struct Switch {
    const char* cvar;
    const char* test;
    bool is_float;
    std::string original;  // read when the suspect starts
};

// One render feature to measure: switched to `test` and back, three times,
// 1.5 s each way, while the GPU timer counts each frame. The first 0.4 s of
// every window is left out (the engine reallocates some buffers on a switch).
struct Suspect {
    const char* what;
    std::vector<Switch> sw;  // empty: nothing switched (shows the noise), or `toggle`
    void (*toggle)(bool test) = nullptr;  // switched by the mod, not a console variable
    void (*decide)(const struct Suspect& s) = nullptr;  // after it is measured
    bool (*available)() = nullptr;  // false: nothing to measure now, skipped
    double base_ms = 0.0, test_ms = 0.0, base_idle = 0.0, test_idle = 0.0;
    long base_n = 0, test_n = 0, base_idle_n = 0, test_idle_n = 0;
    long long base_sent = 0, test_sent = 0, base_missed = 0, test_missed = 0;  // headset frames
    std::string tries;  // "-4.1 -4.5 -4.4"
    double cycle_base = 0.0;
    double saved = 0.0;
};

std::vector<Suspect> g_suspects;
// 1: every suspect, some of which change the picture (shadows off, half
// resolution). 2: only the ones that leave the picture in the headset as it
// is, for runs where the owner plays normally.
int g_mode = 1;
enum class Phase { kWaiting, kRunning, kDone };
Phase g_phase = Phase::kWaiting;
double g_clock = 0.0, g_window_start = 0.0;
int g_index = 0, g_window = 0;  // windows per suspect: base, test, base, test, ...
bool g_settled = false;
long long g_win_sent = 0, g_win_missed = 0;  // headset totals when the window's counting began
// Seconds played in the current level, which must not be the home base
// (Area000_Home draws far less than a run's level, so its numbers say little).
UObject g_level_player = 0;
bool g_level_counts = false;
double g_level_clock = 0.0;
constexpr double kWindowS = 1.5, kSettleS = 0.4;
constexpr int kCycles = 3;

void Add(const char* what, std::vector<Switch> sw) {
    Suspect s;
    s.what = what;
    s.sw = std::move(sw);
    g_suspects.push_back(s);
}

void Add(const char* what, const char* cvar, const char* test, bool is_float = false) {
    if (cvar) Add(what, std::vector<Switch>{{cvar, test, is_float, {}}});
    else Add(what, std::vector<Switch>{});
}

bool Readable(const Switch& w) { return !w.original.empty() && w.original != "-12345"; }

std::string ReadNow(const Switch& w) {
    if (w.is_float) {
        const float v = cvars::ReadFloatNow(w.cvar, -12345.f);
        if (v == -12345.f) return "-12345";
        char b[32];
        std::snprintf(b, sizeof(b), "%g", v);
        return b;
    }
    return std::to_string(cvars::ReadIntNow(w.cvar, -12345));
}

std::string TestValue(const Switch& w) {
    if (w.test[0] != '+') return w.test;
    char b[32];
    std::snprintf(b, sizeof(b), "%g", std::atof(w.original.c_str()) + std::atof(w.test + 1));
    return b;
}

void Set(UObject player, const Suspect& s, bool test) {
    if (s.toggle) {
        s.toggle(test);
        return;
    }
    for (const Switch& w : s.sw)
        if (Readable(w)) cvars::RunNow(std::string(w.cvar) + " " + (test ? TestValue(w) : w.original), player);
}

// "r.A 0, r.B 0" and "3, 5" for the log.
std::string Describe(const Suspect& s, bool originals) {
    if (s.toggle) return originals ? "-" : "the mod";
    if (s.sw.empty()) return originals ? "-" : "nothing switched";
    std::string out;
    for (const Switch& w : s.sw) {
        if (!out.empty()) out += ", ";
        out += originals ? (Readable(w) ? w.original : std::string("?")) : std::string(w.cvar) + " " + TestValue(w);
    }
    return out;
}

void Summary();

// Skips suspects with nothing to measure now. False when none is left (the
// test is then over).
bool SkipUnavailable() {
    while (g_index < static_cast<int>(g_suspects.size())) {
        Suspect& s = g_suspects[g_index];
        if (!s.available || s.available()) return true;
        log::Info("GPU cost test: %s: nothing to switch in this level, skipped", s.what);
        s.saved = 0.0;
        ++g_index;
    }
    g_phase = Phase::kDone;
    Summary();
    log::Info("GPU cost test: done, every setting is back as it was");
    return false;
}

void BeginSuspect(UObject player) {
    if (!SkipUnavailable()) return;
    Suspect& s = g_suspects[g_index];
    for (Switch& w : s.sw) w.original = ReadNow(w);
    s.base_ms = s.test_ms = s.base_idle = s.test_idle = 0.0;
    s.base_n = s.test_n = s.base_idle_n = s.test_idle_n = 0;
    s.base_sent = s.test_sent = s.base_missed = s.test_missed = 0;
    s.tries.clear();
    g_window = 0;
    g_window_start = g_clock;
    g_settled = false;
    Set(player, s, false);
    gpu_timer::TakeWindow();
}

void Summary() {
    std::vector<const Suspect*> v;
    for (const Suspect& s : g_suspects) v.push_back(&s);
    std::sort(v.begin(), v.end(), [](const Suspect* a, const Suspect* b) { return a->saved > b->saved; });
    std::string line = "GPU cost test summary (GPU ms a frame saved by switching each off, biggest first):";
    char b[200];
    for (const Suspect* s : v) {
        std::snprintf(b, sizeof(b), " | %s %+.2f", s->what, s->saved);
        line += b;
    }
    log::Info("%s", line.c_str());
    // GPU frame = fixed part + per-pixel part x (resolution share)^2, from
    // each resolution point against the full-resolution windows.
    for (const Suspect& s : g_suspects) {
        if (s.sw.size() != 1 || std::string(s.sw[0].cvar) != "r.ScreenPercentage" || s.base_n == 0 ||
            s.test_n == 0)
            continue;
        const double f = std::atof(s.sw[0].test) / 100.0;
        const double base = s.base_ms / s.base_n;
        const double pixel = s.saved / (1.0 - f * f);
        log::Info("GPU cost test: from the %s point, about %.1f ms of the %.1f ms GPU frame follows the number of "
                  "pixels and %.1f ms does not",
                  s.what, pixel, base, base - pixel);
    }
}

void FinishSuspect(UObject player) {
    Suspect& s = g_suspects[g_index];
    Set(player, s, false);
    const double base = s.base_n ? s.base_ms / s.base_n : 0.0, test = s.test_n ? s.test_ms / s.test_n : 0.0;
    const double bidle = s.base_idle_n ? s.base_idle / s.base_idle_n : 0.0;
    const double tidle = s.test_idle_n ? s.test_idle / s.test_idle_n : 0.0;
    s.saved = base - test;
    const double bmiss = s.base_sent ? 100.0 * s.base_missed / (s.base_sent + s.base_missed) : 0.0;
    const double tmiss = s.test_sent ? 100.0 * s.test_missed / (s.test_sent + s.test_missed) : 0.0;
    const bool none = !s.sw.empty() && std::none_of(s.sw.begin(), s.sw.end(), Readable);
    if (none) {
        log::Info("GPU cost test: %s: %s could not be read, skipped", s.what, s.sw[0].cvar);
        s.saved = 0.0;
    } else {
        log::Info("GPU cost test: %s (%s, was %s): GPU %.2f -> %.2f ms a frame (%+.2f ms; each try%s), GPU idle "
                  "%.2f -> %.2f ms; headset slots missed %.1f%% -> %.1f%%; %ld + %ld frames",
                  s.what, Describe(s, false).c_str(), Describe(s, true).c_str(), base, test, test - base,
                  s.tries.c_str(), bidle, tidle, bmiss, tmiss, s.base_n, s.test_n);
    }
    if (s.decide) s.decide(s);
    ++g_index;
    BeginSuspect(player);
}

std::string MapOf(UObject object) {
    for (int i = 0; object && i < 8; ++i, object = ue::GetOuter(object))
        if (ue::GetName(ue::GetClass(object)) == "World") return ue::GetName(object);
    return {};
}

void OnTick(UObject player, float dt) {
    if (g_phase == Phase::kDone || !player) return;
    g_clock += dt;
    if (player != g_level_player) {
        g_level_player = player;
        const std::string map = MapOf(player);
        g_level_counts = !map.empty() && (map.find("Area000_Home") == std::string::npos ||
                                          config::GetBool("gpu_cost_test_at_home", false));
        g_level_clock = 0.0;
        if (g_phase == Phase::kRunning) {
            // A new level: put this setting back, and start it again once
            // 45 s into a run level.
            Set(player, g_suspects[g_index], false);
            g_phase = Phase::kWaiting;
            log::Info("GPU cost test: a new level (%s); waiting to start again", map.c_str());
        }
    }
    if (g_level_counts) g_level_clock += dt;
    // A menu, or the square test switching its own suspects, changes what the
    // GPU draws.
    const bool menu = ui_panel::MenuOpen() || square_test::Busy();
    if (g_phase == Phase::kWaiting) {
        if (menu || g_level_clock < config::GetFloat("gpu_cost_test_after_s", 45.f) || !gpu_timer::On()) return;
        g_phase = Phase::kRunning;
        // A restart after a level change goes on from the setting it was on
        // (each setting is put back before the next one starts).
        log::Info("GPU cost test: starting; %zu settings, each switched off and on %d times for %.1f s "
                  "(about %.0f s in all; %s; gpu_cost_test=0 turns it off)",
                  g_suspects.size(), kCycles, kWindowS, g_suspects.size() * kCycles * 2 * kWindowS,
                  g_mode == 2 ? "only settings that leave the picture in the headset as it is"
                              : "the picture changes while it runs");
        BeginSuspect(player);
        return;
    }
    Suspect& s = g_suspects[g_index];
    if (menu) {
        // Put the setting back and start this one again once it's over.
        Set(player, s, false);
        g_window = 0;
        g_window_start = g_clock;
        g_settled = false;
        s.base_ms = s.test_ms = s.base_idle = s.test_idle = 0.0;
        s.base_n = s.test_n = s.base_idle_n = s.test_idle_n = 0;
        s.base_sent = s.test_sent = s.base_missed = s.test_missed = 0;
        s.tries.clear();
        return;
    }
    const double in = g_clock - g_window_start;
    if (!g_settled && in >= kSettleS) {
        g_settled = true;
        gpu_timer::TakeWindow();  // drop the frames drawn while it settled
        xr::HeadsetFrameTotals(&g_win_sent, &g_win_missed);
        return;
    }
    if (in < kWindowS) return;
    const gpu_timer::Window w = gpu_timer::TakeWindow();
    long long sent = 0, missed = 0;
    xr::HeadsetFrameTotals(&sent, &missed);
    sent -= g_win_sent;
    missed -= g_win_missed;
    const bool was_test = g_window % 2 == 1;
    (was_test ? s.test_sent : s.base_sent) += sent;
    (was_test ? s.test_missed : s.base_missed) += missed;
    if (was_test) {
        s.test_ms += w.frame_ms;
        s.test_n += w.frames;
        s.test_idle += w.idle_ms;
        s.test_idle_n += w.idles;
        if (w.frames) {
            char b[24];
            std::snprintf(b, sizeof(b), " %+.1f", w.frame_ms / w.frames - s.cycle_base);
            s.tries += b;
        }
    } else {
        s.base_ms += w.frame_ms;
        s.base_n += w.frames;
        s.base_idle += w.idle_ms;
        s.base_idle_n += w.idles;
        s.cycle_base = w.frames ? w.frame_ms / w.frames : 0.0;
    }
    ++g_window;
    if (g_window >= kCycles * 2) {
        FinishSuspect(player);
        return;
    }
    Set(player, s, g_window % 2 == 1);
    g_window_start = g_clock;
    g_settled = false;
}

}  // namespace

bool Running() { return g_phase == Phase::kRunning; }

// xrEndFrame on its own thread: measured off (base) and on (test). With
// xr_end_async=2 it stays on for the rest of the run only if the headset
// missed fewer display slots with it, or as few while the GPU's whole frame
// (its work plus its idle) got shorter.
void ToggleAsyncEnd(bool on) { xr::SetAsyncEnd(on, "GPU cost test"); }

// Base: the HUD as it is (out of sight unless you look). Test: shown on the
// arm and redrawn every frame, as when you look at it.
void ToggleHud(bool on) { world_menu::ForceHudShown(on); }

// Base: the scene captures as the game runs them. Test: each one that takes
// a picture every frame (an extra render of the level) paused. Nothing in
// the headset changes unless a screen showing that picture is in view.
void ToggleSceneCaptures(bool test) {
    std::string names;
    const int n = render_fixes::PauseSceneCaptures(test, &names);
    static bool s_logged = false;
    if (test && !s_logged) {
        s_logged = true;
        log::Info("GPU cost test: paused %d scene capture(s) drawing every frame: %s", n, names.c_str());
    }
}
bool SceneCapturesLive() { return render_fixes::LiveSceneCaptures() > 0; }

void DecideAsyncEnd(const Suspect& s) {
    const double base = s.base_n ? (s.base_ms + s.base_idle) / s.base_n : 0.0;
    const double test = s.test_n ? (s.test_ms + s.test_idle) / s.test_n : 0.0;
    const double bmiss = s.base_sent ? static_cast<double>(s.base_missed) / (s.base_sent + s.base_missed) : 0.0;
    const double tmiss = s.test_sent ? static_cast<double>(s.test_missed) / (s.test_sent + s.test_missed) : 0.0;
    const bool measured = s.base_n > 30 && s.test_n > 30;
    const bool better = measured && (tmiss < bmiss * 0.8 - 0.002 || (tmiss <= bmiss + 0.002 && test < base - 0.2));
    log::Info("GPU cost test: xrEndFrame on its own thread: GPU work + idle %.2f -> %.2f ms a frame, headset slots "
              "missed %.1f%% -> %.1f%%; keeping it %s for this run",
              base, test, 100.0 * bmiss, 100.0 * tmiss, better ? "ON" : "off");
    xr::SetAsyncEnd(better, better ? "GPU cost test: it measured better" : "GPU cost test: it did not measure better");
}

void Init() {
    g_mode = config::GetInt("gpu_cost_test", 0);
    if (g_mode == 0 && config::GetBool("gpu_cost_test", false)) g_mode = 1;  // "true", "yes"
    if (g_mode != 1 && g_mode != 2) return;
    Add("nothing (measurement noise)", nullptr, nullptr);
    if (config::GetInt("xr_end_async", 1) == 2) {
        Add("xrEndFrame on its own thread (headset hand-off)", nullptr, nullptr);
        g_suspects.back().toggle = &ToggleAsyncEnd;
        g_suspects.back().decide = &DecideAsyncEnd;
    }
    // Work the picture doesn't need (2026-11-07, the ~4 ms of the fixed part
    // nothing else explained). Scene captures: the kill shot room's camera
    // was seen taking a picture every frame; each such picture is a whole
    // extra render of the level. Occlusion: without instanced stereo (the
    // game's shaders aren't built for it) each eye runs its own occlusion
    // queries; the engine's VR option lets the eyes take turns.
    Add("scene captures that draw every frame paused", nullptr, nullptr);
    g_suspects.back().toggle = &ToggleSceneCaptures;
    g_suspects.back().available = &SceneCapturesLive;
    Add("occlusion checks one eye a frame (vr.RoundRobinOcclusion)", "vr.RoundRobinOcclusion", "1");
    if (g_mode == 2) {
        game::AddTickListener(&OnTick);
        log::Info("GPU cost test: ready (gpu_cost_test=2: only what leaves the picture as it is); runs once, %.0f s "
                  "into the first run level (not the home base), if the GPU timer is on",
                  config::GetFloat("gpu_cost_test_after_s", 45.f));
        return;
    }
    // Measured on 2026-11-06 (08:49 run): Lumen GI off -0.63 ms, regular
    // shadows instead of virtual +7.9 ms, fog and screen-space AO were off
    // already. 2026-11-07 (Office): shadows High instead of Epic -0.67 ms and
    // Lumen GI probes at half density -0.88 ms are VR defaults now (cvars);
    // GI tracing at 3/4 -0.57 ms is about what the GI quality 1 cap does;
    // Lumen on see-through things -0.12 and short-range AO -0.09 are small.
    // Those are no longer switched.
    //
    // The same run split the 12.9 ms GPU frame into about 4.9 ms that
    // follows the pixel count and 7.9 ms that does not, of which all shadows
    // were 2.7 ms. These points split the rest of the fixed part.
    Add("50% resolution", "r.ScreenPercentage", "50", true);
    Add("all shadows off", "r.ShadowQuality", "0");
    Add("sun shadows one step coarser", "r.Shadow.Virtual.ResolutionLodBiasDirectional", "+1", true);
    Add("lamp shadows one step coarser", "r.Shadow.Virtual.ResolutionLodBiasLocal", "+1", true);
    Add("Lumen GI off entirely", "r.Lumen.DiffuseIndirect.Allow", "0");
    Add("objects drawn half as far", "r.ViewDistanceScale", "0.5", true);
    Add("post effects off (bloom, motion blur, depth of field, lens flares, auto exposure)",
        {{"r.BloomQuality", "0", false, {}},
         {"r.MotionBlurQuality", "0", false, {}},
         {"r.DepthOfFieldQuality", "0", false, {}},
         {"r.LensFlareQuality", "0", false, {}},
         {"r.EyeAdaptationQuality", "0", false, {}}});
    // Owner 2026-11-07: what do the menus cost? Out of sight the wrist HUD is
    // redrawn 4 times a second; this is it shown and redrawn every frame.
    Add("wrist HUD shown and redrawn every frame", nullptr, nullptr);
    g_suspects.back().toggle = &ToggleHud;
    game::AddTickListener(&OnTick);
    log::Info("GPU cost test: ready; runs once, %.0f s into the first run level (not the home base), if the GPU "
              "timer is on",
              config::GetFloat("gpu_cost_test_after_s", 45.f));
}

}  // namespace holevr::vr::gpu_test
