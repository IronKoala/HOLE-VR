#include "game/game.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include "core/log.h"
#include "core/memory.h"
#include "core/perf.h"
#include "ue/call.h"
#include "ue/process_event.h"

namespace holevr::game {
namespace {

constexpr const char* kPlayerTick =
    "Function /Game/Characters/Player/Blueprints/BP_Player.BP_Player_C:ReceiveTick";

std::atomic<UObject> g_player{0};
std::atomic<UObject> g_tick_fn{0};
std::atomic<DWORD> g_last_tick_ms{0};

std::mutex g_queue_mutex;
std::vector<std::function<void()>> g_queue;

constexpr int kMaxListeners = 16;
TickFn g_listeners[kMaxListeners];
std::atomic<int> g_listener_count{0};
const char* kListenerNames[kMaxListeners] = {"tick#0", "tick#1", "tick#2", "tick#3", "tick#4", "tick#5",
                                             "tick#6", "tick#7", "tick#8", "tick#9", "tick#10", "tick#11",
                                             "tick#12", "tick#13", "tick#14", "tick#15"};
perf::Counter g_listener_perf[kMaxListeners];
perf::Counter g_queue_perf{"posted work"};
perf::Counter g_tick_perf{"mod tick total"};

std::atomic<UObject> g_gun_class{0}, g_weapon_class{0}, g_mag_class{0};

// One Maintain at a time: the early watcher and the main loop both call it.
std::mutex g_maintain_mutex;

// Seconds since the game process started, for the log.
double SinceLaunch() {
    FILETIME created, exited, kernel, user, now;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER c{{created.dwLowDateTime, created.dwHighDateTime}};
    ULARGE_INTEGER n{{now.dwLowDateTime, now.dwHighDateTime}};
    return (n.QuadPart - c.QuadPart) / 1e7;
}

std::atomic<bool> g_first_tick_logged{false};

void OnPlayerTick(UObject self, UObject, void* params) {
    // ReceiveTick(float DeltaSeconds): the only parameter, at offset 0.
    float dt = 0.f;
    std::memcpy(&dt, params, sizeof(dt));
    g_player.store(self);
    g_last_tick_ms.store(GetTickCount());
    if (!g_first_tick_logged.exchange(true))
        log::Info("game: the player's first tick, %.1f s after launch", SinceLaunch());

    perf::Scope total(g_tick_perf);
    double t0 = perf::NowMs();
    std::vector<std::function<void()>> work;
    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        work.swap(g_queue);
    }
    for (auto& w : work) w();
    g_queue_perf.Add(perf::NowMs() - t0);

    int n = g_listener_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        perf::Scope s(g_listener_perf[i]);
        g_listeners[i](self, dt);
    }
    perf::MaybeReport();
}

bool HookClassDefault(const char* full_name) {
    UObject cdo = FindObject(full_name);
    if (!cdo) {
        log::Warn("game: %s not found", full_name);
        return false;
    }
    return ue::process_event::HookVtableOf(cdo);
}

constexpr const char* kGunClass =
    "BlueprintGeneratedClass /Game/Weapons/Blueprints/BaseBlueprints/BP_GunBase.BP_GunBase_C";
constexpr const char* kWeaponClass =
    "BlueprintGeneratedClass /Game/Weapons/Blueprints/BaseBlueprints/BP_WeaponBase.BP_WeaponBase_C";
constexpr const char* kMagClass =
    "BlueprintGeneratedClass "
    "/Game/Weapons/Custom/Parts/GunParts/00_BaseBP/BP_MagazineBase.BP_MagazineBase_C";

// Background thread only: a full object scan per class that is not loaded yet.
void RefreshClass(const char* full_name, std::atomic<UObject>& cache) {
    UObject c = cache.load();
    if (c && GetFullName(c) == full_name) return;
    cache.store(FindObject(full_name));
}

// Finds the player's Blueprint tick and follows it. Call with
// g_maintain_mutex held.
void FollowPlayerTick() {
    UObject fn = g_tick_fn.load();
    DWORD since = GetTickCount() - g_last_tick_ms.load();
    if (fn && since < 2000) return;
    if (g_player.load() && since >= 2000) {
        g_player.store(0);  // left the level, paused or loading
    }
    UObject found = FindObject(kPlayerTick);
    if (found && found != fn) {
        ue::process_event::On(found, nullptr, &OnPlayerTick);
        g_tick_fn.store(found);
        log::Info("game: following the player's tick (%p), %.1f s after launch",
                  reinterpret_cast<void*>(found), SinceLaunch());
    }
}

}  // namespace

bool Init() {
    if (!ue::process_event::Ready()) return false;
    // BP_Player_C and the NPCs derive from Character; guns and magazines from
    // Actor; the weapon manager from ActorComponent. Each native class has its
    // own vtable, so hook each one we need to observe.
    bool ok = HookClassDefault("Character /Script/Engine.Default__Character");
    HookClassDefault("Actor /Script/Engine.Default__Actor");
    HookClassDefault("ActorComponent /Script/Engine.Default__ActorComponent");
    if (ok) {
        // Everything that runs on the player's tick (the VR body, controls,
        // holsters) waits for us to find it. The main loop only starts once
        // all the other start-up work is done, which was ~9 s after the
        // title closed, so look for it right away, ten times a second (twice
        // a second after the first two minutes), until the player ticks.
        if (HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
                for (int i = 0; i < 2400 && !g_first_tick_logged.load(); ++i) {
                    {
                        std::unique_lock<std::mutex> lock(g_maintain_mutex, std::try_to_lock);
                        if (lock.owns_lock()) FollowPlayerTick();
                    }
                    Sleep(i < 1200 ? 100 : 500);
                }
                return 0;
            }, nullptr, 0, nullptr))
            CloseHandle(t);
    }
    return ok;
}

void Maintain() {
    std::unique_lock<std::mutex> lock(g_maintain_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    // The player's tick first: everything on it waits for this.
    FollowPlayerTick();
    RefreshClass(kGunClass, g_gun_class);
    RefreshClass(kWeaponClass, g_weapon_class);
    RefreshClass(kMagClass, g_mag_class);
}

UObject Player() { return g_player.load(); }

void AddTickListener(TickFn fn) {
    int n = g_listener_count.load();
    if (n >= kMaxListeners) return;
    g_listeners[n] = fn;
    if (n == 0) {
        perf::Register(&g_tick_perf);
        perf::Register(&g_queue_perf);
    }
    g_listener_perf[n].name = kListenerNames[n];
    perf::Register(&g_listener_perf[n]);
    log::Info("game: tick listener #%d registered (the next \"ready\" line names it)", n);
    g_listener_count.store(n + 1, std::memory_order_release);
}

void Post(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    g_queue.push_back(std::move(fn));
}

UObject GunBaseClass() { return g_gun_class.load(); }
UObject WeaponBaseClass() { return g_weapon_class.load(); }
UObject MagazineBaseClass() { return g_mag_class.load(); }

}  // namespace holevr::game
