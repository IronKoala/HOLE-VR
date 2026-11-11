#include "game/spawn_report.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "game/game.h"
#include "game/weapon_custom.h"
#include "ue/call.h"
#include "ue/names.h"
#include "ue/objects.h"
#include "ue/reflection.h"
#include "ue/script_dump.h"

namespace holevr::game::spawn_report {
namespace {
using ue::FName;
using ue::UObject;
namespace rf = ue::reflection;

double Clock() {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) / static_cast<double>(freq.QuadPart);
}

// A kept object: still the same one (object-array slot, class and name)?
struct Ref {
    UObject obj = 0, cls = 0;
    int index = -1;
    FName name{};
    void Set(UObject p) {
        obj = p;
        cls = p ? ue::GetClass(p) : 0;
        name = p ? ue::GetFName(p) : FName{};
        index = p ? mem::ReadOr<int32_t>(p + ue::off::kObjIndex, -1) : -1;
    }
    bool Live() const {
        if (!obj || index < 0 || ue::objects::Get(index) != obj || ue::GetClass(obj) != cls) return false;
        const FName n = ue::GetFName(obj);
        return n.comparison_index == name.comparison_index && n.number == name.number;
    }
};

std::string NameOf(const FName& n) { return ue::names::ToString(n); }
std::string ObjName(UObject o) { return o && ue::IsLive(o) ? ue::GetName(o) : std::string("none"); }

// Field reads by name, searched up the class chain. Missing fields read as
// the fallback, so a game update that renames one shows up as "?" or -1.
template <typename T>
T Get(UObject o, const char* name, T fallback) {
    T v{};
    return o && ue::GetProp(o, name, v) ? v : fallback;
}
int Flag(UObject o, const char* name) {
    if (!o) return -1;
    const rf::PropertyInfo p = rf::FindProperty(ue::GetClass(o), name);
    uint8_t v = 0;
    if (!p.valid() || p.type != "BoolProperty" || !mem::Read(o + p.offset, v)) return -1;
    return (v & (p.bool_mask ? p.bool_mask : 0xFF)) ? 1 : 0;
}
const char* YesNo(int v) { return v < 0 ? "?" : v ? "yes" : "no"; }

// TArray { T* Data; int32 Num; int32 Max; } at a named field.
bool Array(UObject o, const char* name, uintptr_t* data, int32_t* num) {
    const int32_t off = o ? ue::PropertyOffset(o, name) : -1;
    int32_t n = 0, m = 0;
    uintptr_t d = 0;
    if (off < 0 || !mem::Read(o + off, d) || !mem::Read(o + off + 8, n) || !mem::Read(o + off + 12, m)) return false;
    if (n < 0 || n > m || n > 100000 || (n > 0 && !d)) return false;
    *data = d;
    *num = n;
    return true;
}
int32_t ArrayNum(UObject o, const char* name) {
    uintptr_t d = 0;
    int32_t n = -1;
    return Array(o, name, &d, &n) ? n : -1;
}

// TSet/TMap storage (UE 5.x): TSparseArray {TArray Data; TBitArray
// AllocationFlags {uint32 Inline[4]; uint32* Secondary; int32 NumBits,
// MaxBits}; int32 FirstFreeIndex, NumFreeIndices}. `stride` is the element
// plus its two hash ints. Same layout weapon_custom.cpp reads (verified there
// on the save's maps).
template <typename Fn>
bool ForEachSparse(uintptr_t at, size_t stride, Fn fn) {
    uintptr_t d = 0;
    int32_t n = 0, m = 0;
    if (!mem::Read(at, d) || !mem::Read(at + 8, n) || !mem::Read(at + 12, m)) return false;
    if (n < 0 || n > m || n > 100000 || (n > 0 && !d)) return false;
    const int32_t num_free = mem::ReadOr<int32_t>(at + 0x34, -1);
    if (num_free < 0 || num_free > n) return false;
    const uintptr_t secondary = mem::ReadOr<uintptr_t>(at + 0x20);
    const uintptr_t bits = secondary ? secondary : at + 0x10;
    for (int32_t i = 0; i < n; ++i) {
        if (num_free > 0) {
            uint32_t word = 0;
            if (!mem::Read(bits + (i / 32) * 4ull, word)) return false;
            if (!(word & (1u << (i % 32)))) continue;
        }
        fn(d + i * stride);
    }
    return true;
}

// Soft object pointer: weak pointer (8), then package and asset FNames.
std::string SoftPath(uintptr_t at) {
    FName package, asset;
    if (!mem::Read(at + 0x08, package) || !mem::Read(at + 0x10, asset)) return {};
    const std::string p = NameOf(package);
    if (p.empty() || p == "None") return {};
    return p + "." + NameOf(asset);
}

// A Blueprint struct member's offset by its plain name (the engine appends
// _N_<GUID>), -1 if missing.
int32_t Member(UObject ustruct, const char* name) {
    const size_t n = std::strlen(name);
    for (const auto& p : rf::GetProperties(ustruct))
        if (p.name == name || (p.name.size() > n && p.name.compare(0, n, name) == 0 && p.name[n] == '_'))
            return p.offset;
    return -1;
}

UObject World(UObject object) {
    for (int i = 0; object && i < 8; ++i, object = ue::GetOuter(object))
        if (ue::GetName(ue::GetClass(object)) == "World") return object;
    return 0;
}

struct State {
    Ref player, world, manager, object;
    std::string map;
    bool bound = false, run = false;
    double loaded_at = 0, find_at = 0, report_at = 0;
    // Last logged object-event state, to log changes as they happen.
    int open = -2, light_off = -2, tvs = -2, tvs_on = -2;
    bool looked = false;  // the object event's first look in this level is logged
    UObject object_class = 0;
    int reports = 0;
} S;

bool g_enabled = true;

UObject ManagerClass() {
    static ue::ObjectRef cls("BlueprintGeneratedClass /Game/GamePlay/SYS_LevelManager.SYS_LevelManager_C", 1);
    return cls.Get();
}

// The level's SYS_LevelManager, through the game's own GetActorOfClass.
UObject FindManager(UObject player) {
    static UObject statics = 0;
    if (!statics) statics = ue::FindObject("GameplayStatics /Script/Engine.Default__GameplayStatics");
    const UObject cls = ManagerClass();
    if (!statics || !cls) return 0;
    ue::Call c(statics, "GetActorOfClass");
    c.Set("WorldContextObject", player).Set("ActorClass", cls);
    if (!c.Invoke()) return 0;
    const UObject m = c.Result<UObject>();
    return m && ue::IsLive(m) && World(m) == S.world.obj ? m : 0;
}

// The game's own run clock (SYS_LevelManager.GetCurrentTimeSecond), -1 if
// it can't be read.
double GameSeconds(UObject manager) {
    ue::Call c(manager, "GetCurrentTimeSecond");
    if (!c.Valid() || !c.Invoke()) return -1;
    return c.Get<double>("Secconds", -1);
}

std::string Mmss(double s) {
    if (s < 0) return "?";
    const int t = static_cast<int>(s);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", t / 60, t % 60);
    return buf;
}

// What the object event's manager (BP_OBJ001Manager_C for the TVs) has:
// spawn points it found, TV actors alive, how many are switched on.
std::string ObjectEventText(UObject inst, int* tvs, int* tvs_on) {
    *tvs = -1;
    *tvs_on = -1;
    if (!inst) return "not started";
    if (!ue::IsLive(inst)) return "its manager is gone";
    std::string s = "started: " + ue::GetName(ue::GetClass(inst));
    uintptr_t d = 0;
    int32_t n = 0;
    if (Array(inst, "OBJ_Entity", &d, &n)) {
        int alive = 0, on = 0;
        for (int32_t i = 0; i < n && i < 256; ++i) {
            const UObject e = mem::ReadOr<uintptr_t>(d + i * 8ull);
            if (!e || !ue::IsLive(e)) continue;
            ++alive;
            if (Flag(e, "IsON") == 1) ++on;
        }
        *tvs = alive;
        *tvs_on = on;
        char buf[160];
        std::snprintf(buf, sizeof(buf), ", %d of %d TVs alive (%d switched on), %d spawn points, nearest-effect %.0f-%.0f cm, "
                      "picture overlay %.2f",
                      alive, n, on, ArrayNum(inst, "OBJ_Spawners"), Get<double>(inst, "EffectiveDistance_Min", -1),
                      Get<double>(inst, "EffectiveDistance_Max", -1), Get<double>(inst, "CurrentOpacity", -1));
        s += buf;
    }
    return s;
}

// Once per level: the event progress flags in the save, and the game's list
// of object events (DT_ObjectData rows and their classes).
void LogSaveAndObjects(UObject player) {
    const UObject gi = custom::GameInstance(player);
    const UObject save = custom::SaveData(gi);
    std::string events;
    int n = 0;
    const int32_t o = save ? ue::PropertyOffset(save, "EventProgressData") : -1;
    // TMap<FName, int32>: key (8), value (4), hash ids (8) = 20 bytes.
    const bool ok = o >= 0 && ForEachSparse(save + o, 20, [&](uintptr_t e) {
        FName key;
        int32_t value = 0;
        if (!mem::Read(e, key) || !mem::Read(e + 8, value)) return;
        if (n++ < 40) events += (events.empty() ? "" : ", ") + NameOf(key) + "=" + std::to_string(value);
    });
    log::Info("spawns: save's event progress (%s): %s", ok ? std::to_string(n).c_str() : "not readable",
              events.empty() ? "none" : events.c_str());

    UObject dt = 0;
    if (!gi || !ue::GetProp(gi, "ObjectDB", dt) || !dt || !ue::IsLive(dt)) {
        log::Info("spawns: the game's object list (ObjectDB) is not readable");
        return;
    }
    const int32_t rs = rf::FindPropertyOffset(ue::GetClass(dt), "RowStruct");
    const UObject row_struct = rs >= 0 ? mem::ReadOr<uintptr_t>(dt + rs) : 0;
    const int32_t o_class = row_struct ? Member(row_struct, "Class") : -1;
    std::string rows;
    // UDataTable: RowStruct is followed by TMap<FName, uint8*> RowMap
    // (key 8, value 8, hash ids 8).
    if (rs >= 0)
        ForEachSparse(dt + rs + 8, 24, [&](uintptr_t e) {
            FName row;
            uintptr_t data = 0;
            if (!mem::Read(e, row) || !mem::Read(e + 8, data) || !data) return;
            std::string cls = o_class >= 0 ? SoftPath(data + o_class) : std::string();
            const size_t dot = cls.rfind('.');
            if (dot != std::string::npos) cls = cls.substr(dot + 1);
            rows += (rows.empty() ? "" : ", ") + NameOf(row) + (cls.empty() ? "" : " = " + cls);
        });
    log::Info("spawns: the game's object events (%s, rows %s): %s", ObjName(dt).c_str(),
              row_struct ? ue::GetName(row_struct).c_str() : "?", rows.empty() ? "none read" : rows.c_str());
}

void LogLevel(UObject m) {
    FName row = Get<FName>(m, "LevelDataRowName", FName{});
    log::Info("spawns: %s (%s): level data row %s; enemies at once: start %d, end %d, now %d, at most %d per spawn, "
              "combat type %d; a spawn every %.1f s (+ up to %.1f s); never within %.0f cm of you; %d enemy spawners; "
              "%d spawn lists by time; debug: no database %s, simulate %s",
              S.map.c_str(), S.run ? "run" : "hub or menu", NameOf(row).c_str(),
              Get<uint8_t>(m, "SpawnNPC_Amount_StartLimit", 255), Get<uint8_t>(m, "SpawnNPC_Amount_EndLimit", 255),
              Get<uint8_t>(m, "SpawnNPC_Amount_TempLimit", 255), Get<uint8_t>(m, "SpawnNPC_OneSpawnMax", 255),
              Get<uint8_t>(m, "SpawnNPC_CombatType", 255), Get<double>(m, "SpawnNPC_Interval_Base", -1),
              Get<double>(m, "SpawnNPC_Interval_RandomRange", -1),
              Get<double>(m, "SpawnNPC_UnSpawnableDistanceToPlayer", -1), ArrayNum(m, "EnemySpawners"),
              ArrayNum(m, "SpawnNPC_AllTimeSpawnData"), YesNo(Flag(m, "Debug_DoNotUseDB")),
              YesNo(Flag(m, "Debug_SimulateMode")));
    // Str_SpawnListData: StartTime (int32) first; 0x58 bytes each.
    uintptr_t d = 0;
    int32_t n = 0;
    if (Array(m, "SpawnNPC_AllTimeSpawnData", &d, &n) && n > 0) {
        std::string times;
        for (int32_t i = 0; i < n && i < 32; ++i) {
            const int32_t t = mem::ReadOr<int32_t>(d + i * 0x58ull, -1);
            const int32_t kinds = mem::ReadOr<int32_t>(d + i * 0x58ull + 8 + 8, -1) -
                                  mem::ReadOr<int32_t>(d + i * 0x58ull + 8 + 0x34, 0);
            times += (times.empty() ? "" : ", ") + Mmss(t) + " (" + std::to_string(kinds) + " kinds)";
        }
        log::Info("spawns: %s: the enemy mix changes at %s", S.map.c_str(), times.c_str());
    }
}

void Report(UObject m, const char* why) {
    const UObject inst = Get<UObject>(m, "ObjectInstance", 0);
    int tvs = -1, on = -1;
    const std::string obj = ObjectEventText(inst, &tvs, &on);
    log::Info("spawns: %s %s (%s): enemies up now %d of the game's limit %d, wave %d, next spawn check %.1f of %.1f s; "
              "object event %s, open %s, lights-off event %s: %s",
              S.map.c_str(), Mmss(GameSeconds(m)).c_str(), why, Get<uint8_t>(m, "SpawnNPC_CurrentTotal", 255),
              Get<uint8_t>(m, "SpawnNPC_Amount_TempLimit", 255), Get<int32_t>(m, "WaveCount", -1),
              Get<double>(m, "SpawnTime", -1), Get<double>(m, "SpawnInterval", -1),
              ObjName(Get<UObject>(m, "ObjectManagerClass", 0)).c_str(), YesNo(Flag(m, "IsOpenObject")),
              YesNo(Flag(m, "IsEvent_OBJ002LightOFF")), obj.c_str());
    S.tvs = tvs;
    S.tvs_on = on;
}

void OnTick(UObject player, float) {
    if (!g_enabled || !player) return;
    const double now = Clock();
    if (S.player.obj != player || !S.player.Live()) {
        S = State{};
        S.player.Set(player);
        S.world.Set(World(player));
        S.map = S.world.Live() ? ue::GetName(S.world.obj) : "unknown";
        S.loaded_at = now;
        // The manager fills its spawn settings from the level database
        // during its first frames; read them a few seconds in.
        S.find_at = now + 3.0;
    }
    if (now < S.find_at) return;
    S.find_at = now + 1.0;
    if (!S.bound) {
        if (!S.world.Live()) return;
        const UObject m = FindManager(player);
        if (!m) return;
        S.manager.Set(m);
        S.bound = true;
        S.run = S.map.find("Area000_Home") == std::string::npos && Flag(m, "IsOpenAreaSelectMode") == 0;
        LogLevel(m);
        LogSaveAndObjects(player);
        // The level's own script (its level Blueprint and streamed parts)
        // can start object events too; it is loaded only in that level, so
        // the hub's script dump never has it. Once per map per launch (the
        // listing takes tens of ms, just after the level has loaded).
        static std::vector<std::string> dumped;
        if (S.run && std::find(dumped.begin(), dumped.end(), S.map) == dumped.end()) {
            dumped.push_back(S.map);
            const int written = ue::script::DumpClasses(config::Dir() + L"\\dump", {"/Game/Maps/" + S.map});
            log::Info("spawns: %s: its level script written to dump/script (%d classes)", S.map.c_str(), written);
        }
        S.report_at = now + 10.0;
    }
    if (!S.manager.Live()) return;
    const UObject m = S.manager.obj;
    // Object-event changes as they happen (checked once a second).
    const UObject cls = Get<UObject>(m, "ObjectManagerClass", 0);
    const UObject inst = Get<UObject>(m, "ObjectInstance", 0);
    const int open = Flag(m, "IsOpenObject"), light_off = Flag(m, "IsEvent_OBJ002LightOFF");
    const bool inst_changed = (inst && ue::IsLive(inst)) != S.object.Live() ||
                              (inst && S.object.obj != inst);
    if (!S.looked || cls != S.object_class || open != S.open || light_off != S.light_off || inst_changed) {
        const bool first = !S.looked;
        S.looked = true;
        S.object_class = cls;
        S.open = open;
        S.light_off = light_off;
        S.object.Set(inst && ue::IsLive(inst) ? inst : 0);
        Report(m, first ? "level start" : "object event changed");
        S.report_at = now + 10.0;
        return;
    }
    if (S.run && now >= S.report_at && S.reports < 400) {
        ++S.reports;
        Report(m, "every 10 s");
        S.report_at = now + 10.0;
    }
}

}  // namespace

void InitGame() {
    g_enabled = config::GetBool("spawn_report", true);
    if (!g_enabled) return;
    game::AddTickListener(&OnTick);
    log::Info("spawns: reporting the game's enemy spawns and object events (TVs) per level (read only; "
              "spawn_report=0 turns it off)");
}

}  // namespace holevr::game::spawn_report
