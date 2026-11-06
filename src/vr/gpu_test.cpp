#include "vr/gpu_test.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "game/game.h"
#include "vr/cvars.h"
#include "vr/gpu_timer.h"
#include "vr/square_test.h"
#include "vr/ui_panel.h"

namespace holevr::vr::gpu_test {
namespace {
using ue::UObject;

// One render feature to measure: switched to `test` and back, three times,
// 1.5 s each way, while the GPU timer counts each frame. The first 0.4 s of
// every window is left out (the engine reallocates some buffers on a switch).
struct Suspect {
    const char* what;
    const char* cvar;  // nullptr: nothing switched (shows the noise)
    const char* test;
    bool is_float;
    std::string original;
    double base_ms = 0.0, test_ms = 0.0, base_idle = 0.0, test_idle = 0.0;
    long base_n = 0, test_n = 0, base_idle_n = 0, test_idle_n = 0;
    std::string tries;  // "-4.1 -4.5 -4.4"
    double cycle_base = 0.0;
    double saved = 0.0;
};

std::vector<Suspect> g_suspects;
enum class Phase { kWaiting, kRunning, kDone };
Phase g_phase = Phase::kWaiting;
double g_clock = 0.0, g_window_start = 0.0;
int g_index = 0, g_window = 0;  // windows per suspect: base, test, base, test, ...
bool g_settled = false;
constexpr double kWindowS = 1.5, kSettleS = 0.4;
constexpr int kCycles = 3;

void Add(const char* what, const char* cvar, const char* test, bool is_float = false) {
    Suspect s;
    s.what = what;
    s.cvar = cvar;
    s.test = test;
    s.is_float = is_float;
    g_suspects.push_back(s);
}

std::string ReadNow(const Suspect& s) {
    if (s.is_float) {
        char b[32];
        std::snprintf(b, sizeof(b), "%g", cvars::ReadFloatNow(s.cvar, -12345.f));
        return b;
    }
    return std::to_string(cvars::ReadIntNow(s.cvar, -12345));
}

void Set(UObject player, const Suspect& s, bool test) {
    if (!s.cvar || s.original.empty() || s.original == "-12345") return;
    cvars::RunNow(std::string(s.cvar) + " " + (test ? s.test : s.original), player);
}

void BeginSuspect(UObject player) {
    Suspect& s = g_suspects[g_index];
    if (s.cvar) s.original = ReadNow(s);
    s.base_ms = s.test_ms = s.base_idle = s.test_idle = 0.0;
    s.base_n = s.test_n = s.base_idle_n = s.test_idle_n = 0;
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
}

void FinishSuspect(UObject player) {
    Suspect& s = g_suspects[g_index];
    Set(player, s, false);
    const double base = s.base_n ? s.base_ms / s.base_n : 0.0, test = s.test_n ? s.test_ms / s.test_n : 0.0;
    const double bidle = s.base_idle_n ? s.base_idle / s.base_idle_n : 0.0;
    const double tidle = s.test_idle_n ? s.test_idle / s.test_idle_n : 0.0;
    s.saved = base - test;
    if (s.cvar && (s.original.empty() || s.original == "-12345")) {
        log::Info("GPU cost test: %s: %s could not be read, skipped", s.what, s.cvar);
        s.saved = 0.0;
    } else {
        log::Info("GPU cost test: %s (%s %s, was %s): GPU %.2f -> %.2f ms a frame (%+.2f ms; each try%s), GPU idle "
                  "%.2f -> %.2f ms; %ld + %ld frames",
                  s.what, s.cvar ? s.cvar : "nothing switched", s.cvar ? s.test : "", s.cvar ? s.original.c_str() : "-",
                  base, test, test - base, s.tries.c_str(), bidle, tidle, s.base_n, s.test_n);
    }
    ++g_index;
    if (g_index >= static_cast<int>(g_suspects.size())) {
        g_phase = Phase::kDone;
        Summary();
        log::Info("GPU cost test: done, every setting is back as it was");
        return;
    }
    BeginSuspect(player);
}

void OnTick(UObject player, float dt) {
    if (g_phase == Phase::kDone || !player) return;
    g_clock += dt;
    // A menu, or the square test switching its own suspects, changes what the
    // GPU draws.
    const bool menu = ui_panel::MenuOpen() || square_test::Busy();
    if (g_phase == Phase::kWaiting) {
        if (menu || g_clock < config::GetFloat("gpu_cost_test_after_s", 45.f) || !gpu_timer::On()) return;
        g_phase = Phase::kRunning;
        g_index = 0;
        log::Info("GPU cost test: starting; %zu settings, each switched off and on %d times for %.1f s "
                  "(about %.0f s in all; the picture changes while it runs; gpu_cost_test=0 turns it off)",
                  g_suspects.size(), kCycles, kWindowS, g_suspects.size() * kCycles * 2 * kWindowS);
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
        s.tries.clear();
        return;
    }
    const double in = g_clock - g_window_start;
    if (!g_settled && in >= kSettleS) {
        g_settled = true;
        gpu_timer::TakeWindow();  // drop the frames drawn while it settled
        return;
    }
    if (in < kWindowS) return;
    const gpu_timer::Window w = gpu_timer::TakeWindow();
    const bool was_test = g_window % 2 == 1;
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

void Init() {
    if (!config::GetBool("gpu_cost_test", true)) return;
    Add("nothing (measurement noise)", nullptr, nullptr);
    Add("Lumen global illumination", "r.Lumen.DiffuseIndirect.Allow", "0");
    Add("Lumen GI at quality 1 instead of the level's 2", "r.Lumen.ScreenProbeGather.TracingOctahedronResolution", "6");
    Add("Lumen GI probes at half density", "r.Lumen.ScreenProbeGather.DownsampleFactor", "32");
    Add("Lumen lighting of see-through things", "r.Lumen.TranslucencyVolume.Enable", "0");
    Add("Lumen short-range AO", "r.Lumen.ScreenProbeGather.ShortRangeAO", "0");
    Add("virtual shadow maps (regular shadows instead)", "r.Shadow.Virtual.Enable", "0");
    Add("shadow quality high instead of epic", "sg.ShadowQuality", "2");
    Add("volumetric fog", "r.VolumetricFog", "0");
    Add("screen-space AO", "r.AmbientOcclusionLevels", "0");
    Add("75% resolution", "r.ScreenPercentage", "75", true);
    game::AddTickListener(&OnTick);
    log::Info("GPU cost test: ready; runs once, %.0f s into the first level, if the GPU timer is on",
              config::GetFloat("gpu_cost_test_after_s", 45.f));
}

}  // namespace holevr::vr::gpu_test
