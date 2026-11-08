#include "vr/wall_shapes.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "core/config.h"
#include "core/log.h"
#include "core/perf.h"
#include "ue/call.h"

namespace holevr::vr::wall_shapes {
namespace {

bool g_report = true;

struct TArrayRaw {
    const UObject* data;
    int32_t num, max;
};

struct LineHit {
    bool hit = false;
    double distance = 0;
    std::string mesh;
};

UObject g_ksl = 0, g_gps = 0;
double g_next_lookup_ms = 0;

std::string MeshName(UObject component, UObject actor) {
    UObject mesh = 0;
    if (component && ue::GetProp(component, "StaticMesh", mesh) && mesh) return ue::GetName(mesh);
    std::string out = component ? ue::GetName(ue::GetClass(component)) : std::string("something");
    if (actor) out += " of " + ue::GetName(actor);
    return out;
}

bool Line(UObject player, const FVector& from, const FVector& to, const std::vector<UObject>& ignore, bool complex,
          LineHit* out) {
    *out = {};
    ue::Call t(g_ksl, "LineTraceSingle");
    ue::Call b(g_gps, "BreakHitResult");
    if (!t.Valid() || !b.Valid()) return false;
    TArrayRaw arr{ignore.data(), static_cast<int32_t>(ignore.size()), static_cast<int32_t>(ignore.size())};
    t.Set("WorldContextObject", player).Set("Start", from).Set("End", to).Set("TraceChannel", uint8_t{0})
        .Set("bTraceComplex", complex).Set("ActorsToIgnore", arr).Set("bIgnoreSelf", true)
        .Set("ReturnValue", false);
    if (!t.Invoke()) return false;
    if (!t.Result<bool>()) return true;
    int32_t hit_size = 0, in_size = 0;
    const uint8_t* hit = t.Raw("OutHit", &hit_size);
    uint8_t* in = b.Raw("Hit", &in_size);
    if (!hit || !in || in_size != hit_size) return false;
    std::memcpy(in, hit, static_cast<size_t>(hit_size));
    if (!b.Invoke()) return false;
    if (!b.Get<bool>("bBlockingHit") || b.Get<bool>("bInitialOverlap")) return true;
    const FVector at = b.Get<FVector>("ImpactPoint");
    const FVector d = at - from;
    out->hit = true;
    out->distance = std::sqrt(d.X * d.X + d.Y * d.Y + d.Z * d.Z);
    out->mesh = MeshName(b.Get<UObject>("HitComponent"), b.Get<UObject>("HitActor"));
    return true;
}

// Per mesh, since the last report. Leaf lock.
struct Stats {
    int looks = 0;
    int same = 0;        // within half a centimetre
    int behind = 0;      // the hit box is behind the surface you see (things sink in)
    int in_front = 0;    // the hit box stands out in front of it (things stop short)
    int no_box = 0;      // the game's hit boxes miss this line altogether
    int only_box = 0;    // a hit box, but nothing visible was hit first (for wall_shape=game)
    double behind_sum = 0, behind_worst = 0, front_sum = 0, front_worst = 0;
    int by[2] = {0, 0};
};
std::mutex g_mutex;
std::map<std::string, Stats> g_stats;
double g_last_ms[2] = {-1e9, -1e9};

std::string Report(double) {
    std::map<std::string, Stats> stats;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        stats.swap(g_stats);
    }
    if (stats.empty()) return {};
    std::vector<std::pair<std::string, Stats>> list(stats.begin(), stats.end());
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second.looks > b.second.looks; });
    std::string out = "wall shapes (where the hands and gun touched things; the game's hit box vs the shape you see):";
    char buf[320];
    int shown = 0;
    for (const auto& [name, s] : list) {
        if (++shown > 8) {
            std::snprintf(buf, sizeof(buf), " | %zu more", list.size() - 8);
            out += buf;
            break;
        }
        std::string what;
        if (s.behind > 0) {
            std::snprintf(buf, sizeof(buf), "; hit box %.1f cm behind the surface %d times (worst %.1f)",
                          s.behind_sum / s.behind, s.behind, s.behind_worst);
            what += buf;
        }
        if (s.in_front > 0) {
            std::snprintf(buf, sizeof(buf), "; hit box %.1f cm in front of it %d times (worst %.1f)",
                          s.front_sum / s.in_front, s.in_front, s.front_worst);
            what += buf;
        }
        if (s.same > 0) {
            std::snprintf(buf, sizeof(buf), "; same place %d times", s.same);
            what += buf;
        }
        if (s.no_box > 0) {
            std::snprintf(buf, sizeof(buf), "; NO hit box on the line %d times", s.no_box);
            what += buf;
        }
        if (s.only_box > 0) {
            std::snprintf(buf, sizeof(buf), "; hit box with nothing visible before it %d times", s.only_box);
            what += buf;
        }
        std::snprintf(buf, sizeof(buf), " | %s: %d looks (hands %d, gun %d)", name.c_str(), s.looks, s.by[0], s.by[1]);
        out += buf + what;
    }
    return out;
}

}  // namespace

void LoadSettings() {
    static const bool registered = [] {
        perf::AddReportLine(&Report);
        return true;
    }();
    (void)registered;
    g_report = config::GetBool("wall_shape_report", true);
    log::Info("wall shapes: hands and the held gun stop at %s", ModeName(ModeSetting()));
}

Mode ModeSetting() {
    std::string v = config::GetString("wall_shape", "nearest");
    for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (v == "game") return config::GetBool("hand_world_trace_complex", false) ? Mode::kVisible : Mode::kGame;
    if (v == "visible") return Mode::kVisible;
    return Mode::kNearest;
}

const char* ModeName(Mode mode) {
    switch (mode) {
        case Mode::kGame: return "the game's hit boxes";
        case Mode::kVisible: return "the shapes you see (each mesh's own triangles)";
        default: return "whichever comes first, the shape you see or the game's hit box";
    }
}

void Compare(UObject player, const FVector& from, const FVector& to, const std::vector<UObject>& ignore, Who who) {
    if (!g_report || !player || (who != kHand && who != kGun)) return;
    const double now = perf::NowMs();
    if (now - g_last_ms[who] < 250.0) return;
    g_last_ms[who] = now;
    if ((!g_ksl || !g_gps) && now >= g_next_lookup_ms) {
        g_next_lookup_ms = now + 5000.0;
        if (!g_ksl) g_ksl = ue::FindObject("KismetSystemLibrary /Script/Engine.Default__KismetSystemLibrary");
        if (!g_gps) g_gps = ue::FindObject("GameplayStatics /Script/Engine.Default__GameplayStatics");
    }
    if (!g_ksl || !g_gps) return;
    LineHit visible, box;
    if (!Line(player, from, to, ignore, true, &visible) || !Line(player, from, to, ignore, false, &box)) return;
    if (!visible.hit && !box.hit) return;
    // Named by what you see there (and the hit box's owner when that is
    // something else, such as an invisible blocking volume); by the hit box
    // when nothing visible was hit.
    std::string name = visible.hit ? visible.mesh : box.mesh;
    if (visible.hit && box.hit && box.mesh != visible.mesh) name += " (hit box: " + box.mesh + ")";
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_stats.size() > 64 && !g_stats.count(name)) return;
    Stats& s = g_stats[name];
    ++s.looks;
    ++s.by[who];
    if (!visible.hit) {
        ++s.only_box;
    } else if (!box.hit) {
        ++s.no_box;
    } else {
        const double gap = box.distance - visible.distance;  // > 0: the hit box is further than the surface
        if (gap > 0.5) {
            ++s.behind;
            s.behind_sum += gap;
            s.behind_worst = std::max(s.behind_worst, gap);
        } else if (gap < -0.5) {
            ++s.in_front;
            s.front_sum += -gap;
            s.front_worst = std::max(s.front_worst, -gap);
        } else {
            ++s.same;
        }
    }
}

}  // namespace holevr::vr::wall_shapes
