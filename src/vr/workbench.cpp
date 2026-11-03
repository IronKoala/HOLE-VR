#include "vr/workbench.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <string>

#include "core/config.h"
#include "core/log.h"
#include "game/game.h"
#include "game/weapon_custom.h"
#include "ue/call.h"
#include "vr/hands.h"
#include "vr/math.h"

namespace holevr::vr::workbench {
namespace {

using ue::UObject;

constexpr const char* kBenchClass =
    "BlueprintGeneratedClass /Game/GamePlay/LevelComponents/LCBP_WeaponCustom.LCBP_WeaponCustom_C";

struct Level {
    UObject player = 0;
    double age = 0;          // seconds since this player (level) appeared
    bool searched = false;
    UObject bench = 0;
    bool studied = false;
    double next_near_check = 0;
    bool logged_near = false;
} L;

bool g_study = true;
UObject g_bench_class = 0;
// The bench search runs off the game thread (a full object scan).
std::atomic<int> g_search_state{0};   // 0 idle, 1 running, 2 done
std::atomic<UObject> g_search_result{0};
std::atomic<UObject> g_search_for{0};  // the player the search was for

FVector ActorLocation(UObject a) {
    ue::Call c(a, "K2_GetActorLocation");
    c.Invoke();
    return c.Result<FVector>();
}

double Dist(const FVector& a, const FVector& b) {
    const FVector d = a - b;
    return std::sqrt(d.X * d.X + d.Y * d.Y + d.Z * d.Z);
}

void OnTick(UObject player, float dt) {
    if (player != L.player) {
        L = Level();
        L.player = player;
    }
    if (!player) return;
    L.age += dt;
    // Look for the workbench once per level, a few seconds in (one object
    // scan; levels without a bench just don't have one).
    if (!L.searched && L.age > 4.0 && g_search_state.load() == 0) {
        L.searched = true;
        g_search_for = player;
        g_search_state = 1;
        if (HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
                if (!g_bench_class) g_bench_class = ue::FindObject(kBenchClass);
                g_search_result = g_bench_class ? ue::FindFirstInstance(g_bench_class) : 0;
                g_search_state = 2;
                return 0;
            }, nullptr, 0, nullptr))
            CloseHandle(t);
        else
            g_search_state = 0;
    }
    if (g_search_state.load() == 2) {
        g_search_state = 0;
        if (g_search_for.load() == player) L.bench = g_search_result.load();
        if (L.bench) {
            const FVector b = ActorLocation(L.bench);
            log::Info("workbench: %s in this level at (%.0f %.0f %.0f), %.0f cm from the player",
                      ue::GetName(L.bench).c_str(), b.X, b.Y, b.Z, Dist(b, ActorLocation(player)));
        }
    }
    if (!L.bench) return;
    if (g_study && !L.studied && L.age > 6.0) {
        L.studied = true;
        game::custom::Study(player, L.bench);
    }
    if (L.age >= L.next_near_check && !L.logged_near) {
        L.next_near_check = L.age + 1.0;
        const FVector b = ActorLocation(L.bench);
        for (int h = 0; h < 2; ++h) {
            hands::HandState hs = hands::Get(static_cast<hands::Hand>(h));
            if (!hs.valid) continue;
            const double d = Dist(hs.grip.location, b);
            if (d < 120.0) {
                L.logged_near = true;
                log::Info("workbench: %s hand at the bench: (%.0f %.0f %.0f), %.0f cm from the bench origin "
                          "(%.0f cm above it)",
                          h == hands::kRight ? "right" : "left", hs.grip.location.X, hs.grip.location.Y,
                          hs.grip.location.Z, d, hs.grip.location.Z - b.Z);
                break;
            }
        }
    }
}

}  // namespace

void Init() {
    g_study = config::GetBool("workbench_study", true);
    game::AddTickListener(&OnTick);
}

}  // namespace holevr::vr::workbench
