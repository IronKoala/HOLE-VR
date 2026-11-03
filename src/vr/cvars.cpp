#include "vr/cvars.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

#include "core/config.h"
#include "core/log.h"
#include "game/game.h"
#include "ue/call.h"
#include "ue/objects.h"

namespace holevr::vr::cvars {
namespace {
using ue::UObject;

// FString as the engine lays it out: TArray<TCHAR> with the terminator
// counted. The engine only reads these (const FString& parameters), so a
// buffer we own is fine.
struct FStringView {
    const wchar_t* data;
    int32_t num, max;
};

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

std::mutex g_mutex;
std::map<std::string, std::string> g_wanted;
bool g_ready = false;
// One queued apply at a time: the game queue only drains while the player ticks.
std::atomic<bool> g_pending{false};

struct AaMode {
    const char* name;
    int method;  // r.AntiAliasingMethod: 0 none, 1 FXAA, 2 TAA, 4 TSR
};
constexpr AaMode kAaModes[] = {{"fxaa", 1}, {"taa", 2}, {"tsr", 4}, {"off", 0}};
int g_aa_index = -1;  // -1: leave the game's setting

// Found once in Init(), on the start-up thread. A lookup by name walks every
// object in the game (about half a second with a level loaded), so it must
// never happen on the game thread: the first one used to land on the frame
// the pause menu opened (2026-11-02 log: 545 ms).
std::atomic<UObject> g_ksl{0};

UObject Library() { return g_ksl.load(); }

// Game thread only.
bool ReadInt(UObject ksl, const std::string& name, int* out) {
    std::wstring w = Widen(name);
    FStringView s{w.c_str(), static_cast<int32_t>(w.size() + 1), static_cast<int32_t>(w.size() + 1)};
    ue::Call c(ksl, "GetConsoleVariableIntValue");
    c.Set("VariableName", s);
    if (!c.Invoke()) return false;
    *out = c.Result<int32_t>();
    return true;
}

bool Execute(UObject ksl, UObject context, const std::string& command) {
    std::wstring w = Widen(command);
    FStringView s{w.c_str(), static_cast<int32_t>(w.size() + 1), static_cast<int32_t>(w.size() + 1)};
    ue::Call c(ksl, "ExecuteConsoleCommand");
    c.Set("WorldContextObject", context).Set("Command", s).Set("SpecificPlayer", UObject{0});
    return c.Invoke();
}

bool IsInteger(const std::string& v) {
    if (v.empty()) return false;
    char* end = nullptr;
    std::strtol(v.c_str(), &end, 10);
    return end && *end == '\0';
}

// Game thread, once with a level loaded: what the game renders with, and
// room in the virtual shadow map page pool for two eyes.
//
// White squares and bars in one eye (owner 2026-11-03): they come during a
// run, never with a menu up, randomly in either eye; one capture had square
// blocks of speckled light on a shadowed floor in the left eye only. With
// virtual shadow maps each eye is its own view with its own directional
// light clipmap, so two eyes ask for about twice the shadow pages of the flat
// game. When the pool runs out, the pages that don't fit are drawn without
// their shadow: square patches of full sunlight (this game's sun patches are
// near white) in whichever eye lost out. vr_shadow_pages: -1 = twice the
// game's pool (default), 0 = leave it, N = that many pages (64 KB each).
void StereoRenderSetup(UObject ksl) {
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    int vsm = -1, pages = -1, cache = -1, quality = -1, gi = -1, refl = -1;
    ReadInt(ksl, "r.Shadow.Virtual.Enable", &vsm);
    ReadInt(ksl, "r.Shadow.Virtual.MaxPhysicalPages", &pages);
    ReadInt(ksl, "r.Shadow.Virtual.Cache", &cache);
    ReadInt(ksl, "sg.ShadowQuality", &quality);
    ReadInt(ksl, "r.DynamicGlobalIlluminationMethod", &gi);
    ReadInt(ksl, "r.ReflectionMethod", &refl);
    log::Info("cvars: the game renders with virtual shadow maps %s (%d pages, cache %d), shadow quality %d, "
              "global illumination method %d, reflection method %d",
              vsm == 1 ? "on" : vsm == 0 ? "off" : "unknown", pages, cache, quality, gi, refl);
    if (vsm != 1 || pages <= 0) return;
    int target = config::GetInt("vr_shadow_pages", -1);
    if (target < 0) target = pages * 2;
    target = std::min(target, 16384);
    if (target > pages) {
        log::Info("cvars: virtual shadow map pool %d -> %d pages for two eyes (+%d MB of video memory; "
                  "vr_shadow_pages)",
                  pages, target, (target - pages) / 16);
        Want("r.Shadow.Virtual.MaxPhysicalPages", std::to_string(target));
    }
}

// Game thread: set every wanted variable that differs from its current value.
void ApplyOnGameThread() {
    UObject ksl = Library();
    UObject player = game::Player();
    if (!ksl || !player) return;  // needs a world; menus before the first level wait
    StereoRenderSetup(ksl);
    std::map<std::string, std::string> wanted;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        wanted = g_wanted;
    }
    static std::map<std::string, std::string> s_logged;
    for (const auto& [name, value] : wanted) {
        // Integers can be compared cheaply; other values are set once per
        // change of wanted value (the engine ignores a repeat).
        if (IsInteger(value)) {
            int cur = 0;
            if (ReadInt(ksl, name, &cur) && cur == std::atoi(value.c_str())) continue;
            if (s_logged[name] != value || cur != std::atoi(value.c_str()))
                log::Info("cvars: %s %d -> %s", name.c_str(), cur, value.c_str());
        } else if (s_logged[name] == value) {
            continue;
        } else {
            log::Info("cvars: %s -> %s", name.c_str(), value.c_str());
        }
        Execute(ksl, player, name + " " + value);
        s_logged[name] = value;
    }
}

}  // namespace

void Want(const std::string& name, const std::string& value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_wanted[name] = value;
}

float ReadFloatNow(const std::string& name, float fallback) {
    UObject ksl = Library();
    if (!ksl) return fallback;
    std::wstring w = Widen(name);
    FStringView str{w.c_str(), static_cast<int32_t>(w.size() + 1), static_cast<int32_t>(w.size() + 1)};
    ue::Call c(ksl, "GetConsoleVariableFloatValue");
    c.Set("VariableName", str);
    return c.Invoke() ? c.Result<float>() : fallback;
}

int ReadIntNow(const std::string& name, int fallback) {
    UObject ksl = Library();
    int v = fallback;
    if (!ksl || !ReadInt(ksl, name, &v)) return fallback;
    return v;
}

void RunNow(const std::string& command, UObject context) {
    UObject ksl = Library();
    if (!ksl) return;
    Execute(ksl, context, command);
}

void Init() {
    g_ksl = ue::FindObject("KismetSystemLibrary /Script/Engine.Default__KismetSystemLibrary");
    if (!g_ksl.load()) log::Warn("cvars: KismetSystemLibrary not found, console variables can't be set");
    std::string aa = config::GetString("vr_anti_aliasing", "fxaa");
    for (int i = 0; i < static_cast<int>(sizeof(kAaModes) / sizeof(kAaModes[0])); ++i)
        if (aa == kAaModes[i].name) g_aa_index = i;
    if (g_aa_index >= 0) Want("r.AntiAliasingMethod", std::to_string(kAaModes[g_aa_index].method));
    else if (aa != "game") log::Warn("cvars: unknown vr_anti_aliasing=%s, leaving the game's setting", aa.c_str());
    for (const auto& [name, value] : config::WithPrefix("cvar.")) Want(name, value);
    log::Info("cvars: anti-aliasing %s, %zu console variables from hole-vr.ini", aa.c_str(),
              config::WithPrefix("cvar.").size());
    g_ready = true;
}

void Maintain() {
    if (!g_ready || g_pending.exchange(true)) return;
    game::Post([] {
        g_pending = false;
        ApplyOnGameThread();
    });
}

void CycleAntiAliasing() {
    constexpr int n = static_cast<int>(sizeof(kAaModes) / sizeof(kAaModes[0]));
    g_aa_index = (g_aa_index + 1) % n;
    Want("r.AntiAliasingMethod", std::to_string(kAaModes[g_aa_index].method));
    log::Info("Ctrl+F10: anti-aliasing now %s", kAaModes[g_aa_index].name);
    game::Post([] { ApplyOnGameThread(); });
}

}  // namespace holevr::vr::cvars
