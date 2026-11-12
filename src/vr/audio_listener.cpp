#include "vr/audio_listener.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

#include "core/config.h"
#include "core/log.h"
#include "core/perf.h"
#include "ue/call.h"

namespace holevr::vr::audio_listener {
namespace {
using ue::UObject;

// Owner 2026-11-11: "something going on with the sound in the office, ...
// quieter and more echo-y". The mod never changed the game's sound settings;
// what it did leave is the game's ears at the game's camera: placed at the
// character's head but turned only by the stick, so when you looked round
// with your head, sounds stayed placed for where the body faced. The 10 s
// report says how far the game's camera was from your head and how far it
// was turned from where you look, so a run shows whether that was large.
bool g_on = true;
bool g_loaded = false;
bool g_set = false;           // we put an override on g_pc
UObject g_pc = 0;
double g_sampled_at = -1;
std::mutex g_mutex;
struct Stats {
    int samples = 0;
    double cm_sum = 0, cm_max = 0, deg_sum = 0, deg_max = 0;
    int frames = 0;
} g_stats;

double Dot(const FVector& a, const FVector& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
double Length(const FVector& v) { return std::sqrt(Dot(v, v)); }

std::string Report(double) {
    Stats s;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        s = g_stats;
        g_stats = Stats{};
    }
    if (s.samples <= 0 && s.frames <= 0) return {};
    char b[300];
    std::snprintf(b, sizeof(b),
                  "audio: the game hears from %s (%d frames); its own camera was %.0f cm (most %.0f) from your head "
                  "and turned %.0f deg (most %.0f) from where you look",
                  g_on ? "your head, vr_audio_listener=head" : "its own camera, vr_audio_listener=game", s.frames,
                  s.samples ? s.cm_sum / s.samples : 0.0, s.cm_max, s.samples ? s.deg_sum / s.samples : 0.0,
                  s.deg_max);
    return b;
}

void Load() {
    g_loaded = true;
    std::string v = config::GetString("vr_audio_listener", "head");
    for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    g_on = v != "game";
    perf::AddReportLine(&Report);
}
}  // namespace

void Update(UObject player, bool have_head, const FVector& head_location, const FQuat& head_rotation, double now_s) {
    if (!g_loaded) Load();
    if (!player) return;
    ue::Call gc(player, "GetController");
    const UObject pc = gc.Invoke() ? gc.Result<UObject>() : 0;
    if (!pc) return;
    if (pc != g_pc) {
        g_pc = pc;
        g_set = false;
    }
    if (!have_head) return;
    // Once a second: where the game's own camera is, against your head.
    if (now_s - g_sampled_at > 1.0 || now_s < g_sampled_at) {
        g_sampled_at = now_s;
        UObject cam = 0;
        if (ue::GetProp(pc, "PlayerCameraManager", cam) && cam) {
            ue::Call l(cam, "GetCameraLocation");
            ue::Call r(cam, "GetCameraRotation");
            if (l.Invoke() && r.Invoke()) {
                const FVector at = l.Result<FVector>();
                const FVector fwd = ToQuat(r.Result<FRotator>()).Rotate(FVector{1, 0, 0});
                const FVector look = head_rotation.Rotate(FVector{1, 0, 0});
                const double cm = Length(at - head_location);
                const double c = Dot(fwd, look) / (Length(fwd) * Length(look) + 1e-9);
                const double deg = std::acos(std::clamp(c, -1.0, 1.0)) * 180.0 / 3.14159265358979;
                if (std::isfinite(cm) && std::isfinite(deg)) {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    ++g_stats.samples;
                    g_stats.cm_sum += cm;
                    g_stats.deg_sum += deg;
                    g_stats.cm_max = std::max(g_stats.cm_max, cm);
                    g_stats.deg_max = std::max(g_stats.deg_max, deg);
                }
            }
        }
    }
    if (!g_on) return;
    ue::Call set(pc, "SetAudioListenerOverride");
    // A parameter not found would be left at zero: the game would hear from
    // the world's origin. Checked before every call; off for good if wrong.
    int32_t loc_size = 0, rot_size = 0;
    if (!set.Raw("Location", &loc_size) || !set.Raw("Rotation", &rot_size) || loc_size != sizeof(FVector) ||
        rot_size != sizeof(FRotator)) {
        g_on = false;
        log::Warn("audio: SetAudioListenerOverride not as expected (Location %d, Rotation %d bytes); the game keeps "
                  "hearing from its camera",
                  loc_size, rot_size);
        return;
    }
    set.Set("AttachToComponent", UObject{0}).Set("Location", head_location).Set("Rotation", ToRotator(head_rotation));
    if (!set.Invoke()) return;
    if (!g_set) log::Info("audio: the game now hears from your head, turned as your head is (vr_audio_listener=head)");
    g_set = true;
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_stats.frames;
}

}  // namespace holevr::vr::audio_listener
