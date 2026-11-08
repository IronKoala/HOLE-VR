#include "vr/hole_watch.h"

#include <windows.h>

#include <cmath>
#include <string>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "game/game.h"
#include "ue/call.h"
#include "ue/objects.h"
#include "ue/reflection.h"
#include "vr/input.h"
#include "vr/math.h"
#include "vr/world_menu.h"

namespace holevr::vr::hole_watch {
namespace {
using ue::UObject;
using vr::FVector;

double Clock() {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) / static_cast<double>(freq.QuadPart);
}

bool Live(UObject obj, int index) {
    if (!obj || index < 0) return false;
    return mem::ReadOr<int32_t>(obj + ue::off::kObjIndex, -1) == index && ue::objects::Get(index) == obj &&
           ue::GetClass(obj) != 0;
}

// The hole's own fields, found by name in its class (checked for size).
struct Fields {
    UObject cls = 0;
    int32_t state = -1, falling = -1, limit = -1, waited = -1, progress = -1, radius = -1, cancels = -1,
            exit_point = -1;
    uint8_t falling_mask = 0xFF, exit_mask = 0xFF;
    bool ok = false;
} F;

struct Watch {
    UObject hole = 0;
    int index = -1;
    std::string name;
    int state = -1, falling = -1, cancels = -1, inside = -1, tenth = -1;
    double waited = -1;
    double counting_until = 0;  // the wait went up recently
    double next_tick = 0;
    double checked = 0;
    int logs = 0;
} W;

bool g_buzz = true;

bool ResolveFields(UObject hole) {
    UObject cls = ue::GetClass(hole);
    if (cls == F.cls) return F.ok;
    F = Fields{};
    F.cls = cls;
    namespace rf = ue::reflection;
    auto field = [&](const char* name, int32_t size, uint8_t* mask) -> int32_t {
        rf::PropertyInfo p = rf::FindProperty(cls, name);
        if (!p.valid() || p.element_size != size || p.offset <= 0) return -1;
        if (mask) *mask = p.bool_mask ? p.bool_mask : 0xFF;
        return p.offset;
    };
    F.state = field("HoleState", 1, nullptr);
    F.falling = field("IsPlayerFalling", 1, &F.falling_mask);
    F.limit = field("WaitTimeLimit", 8, nullptr);
    F.waited = field("WaitTimeTemp", 8, nullptr);
    F.progress = field("Progress", 8, nullptr);
    F.radius = field("AreaRadius", 8, nullptr);
    F.cancels = field("StopCancelCount", 4, nullptr);
    F.exit_point = field("IsExitPoint", 1, &F.exit_mask);
    F.ok = F.state >= 0 && F.waited >= 0 && F.limit >= 0;
    log::Info("hole: %s fields %s (state %d, wait %d of %d, progress %d, area %d, falling %d, cancels %d)",
              ue::GetName(cls).c_str(), F.ok ? "found" : "NOT found, not watched", F.state, F.waited, F.limit,
              F.progress, F.radius, F.falling, F.cancels);
    return F.ok;
}

double ReadD(UObject o, int32_t off, double fallback) {
    return off >= 0 ? mem::ReadOr<double>(o + off, fallback) : fallback;
}
int ReadFlag(UObject o, int32_t off, uint8_t mask) {
    return off >= 0 ? ((mem::ReadOr<uint8_t>(o + off, 0) & mask) ? 1 : 0) : -1;
}

bool Location(UObject actor, FVector* out) {
    ue::Call c(actor, "K2_GetActorLocation");
    if (!c.Invoke()) return false;
    *out = c.Result<FVector>();
    return true;
}

void OnTick(UObject player, float) {
    if (!player) return;
    const double now = Clock();
    if (now - W.checked < 0.1) return;
    W.checked = now;
    // The hole you are at: the game's own interact target.
    UObject target = 0;
    ue::GetProp(player, "InteractTarget", target);
    if (target && target != W.hole) {
        int index = mem::ReadOr<int32_t>(target + ue::off::kObjIndex, -1);
        if (Live(target, index) && ue::GetName(ue::GetClass(target)) == "LCBP_Hole_C") {
            W = Watch{};
            W.hole = target;
            W.index = index;
            W.name = ue::GetName(target);
            W.checked = now;
            if (ResolveFields(target))
                log::Info("hole: you are at %s (%s; it waits %.1f s, its area is %.0f cm across)", W.name.c_str(),
                          ReadFlag(target, F.exit_point, F.exit_mask) == 1 ? "the way out of this level"
                                                                          : "the way to the level menu",
                          ReadD(target, F.limit, -1), ReadD(target, F.radius, -1) * 2);
        }
    }
    if (!W.hole) return;
    if (!Live(W.hole, W.index)) {
        W = Watch{};
        return;
    }
    if (!ResolveFields(W.hole)) return;
    const int state = mem::ReadOr<uint8_t>(W.hole + F.state, 0);
    const int falling = ReadFlag(W.hole, F.falling, F.falling_mask);
    const int cancels = F.cancels >= 0 ? mem::ReadOr<int32_t>(W.hole + F.cancels, -1) : -1;
    const double waited = ReadD(W.hole, F.waited, 0), limit = ReadD(W.hole, F.limit, 0);
    const double progress = ReadD(W.hole, F.progress, -1), radius = ReadD(W.hole, F.radius, -1);
    FVector me, it;
    double dist = -1;
    if (Location(player, &me) && Location(W.hole, &it)) dist = std::hypot(me.X - it.X, me.Y - it.Y);
    const int inside = radius > 0 && dist >= 0 ? (dist < radius ? 1 : 0) : -1;
    const int tenth = progress >= 0 && progress <= 1.0001 ? static_cast<int>(progress * 10.0 + 1e-6) : -1;
    if (W.waited >= 0 && waited > W.waited + 1e-4) W.counting_until = now + 0.35;
    const bool changed = state != W.state || falling != W.falling || cancels != W.cancels || inside != W.inside ||
                         tenth != W.tenth || (W.waited >= 0 && (waited < W.waited - 1e-4));
    if (changed && W.logs < 300) {
        ++W.logs;
        log::Info("hole: %s state %d (was %d); waited %.1f of %.1f s; progress %.0f%%; you are %.0f cm from its "
                  "middle (%s); falling %s; cancelled %d time(s)",
                  W.name.c_str(), state, W.state, waited, limit, progress * 100.0, dist,
                  inside < 0 ? "area unknown" : inside ? "inside its area" : "outside its area",
                  falling == 1 ? "yes" : falling == 0 ? "no" : "?", cancels);
    }
    // A soft tick on both hands each second while it counts, a firm one as
    // you drop in. Not while a menu is up.
    const bool counting = now < W.counting_until && falling != 1;
    if (g_buzz && counting && now >= W.next_tick && !world_menu::MenuUp()) {
        input::Haptic(input::kLeft, 0.18f, 0.04f);
        input::Haptic(input::kRight, 0.18f, 0.04f);
        W.next_tick = now + 1.0;
    }
    if (g_buzz && falling == 1 && W.falling == 0) {
        input::Haptic(input::kLeft, 0.6f, 0.15f);
        input::Haptic(input::kRight, 0.6f, 0.15f);
    }
    W.state = state;
    W.falling = falling;
    W.cancels = cancels;
    W.inside = inside;
    W.tenth = tenth;
    W.waited = waited;
}

}  // namespace

void InitGame() {
    g_buzz = config::GetBool("hole_buzz", true);
    game::AddTickListener(&OnTick);
    log::Info("hole: watching the hole you stand at (state and countdown in the log; hand ticks while it counts %s)",
              g_buzz ? "on" : "off");
}

}  // namespace holevr::vr::hole_watch
