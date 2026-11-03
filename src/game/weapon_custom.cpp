#include "game/weapon_custom.h"

#include <cstring>
#include <functional>
#include <map>
#include <mutex>

#include "core/log.h"
#include "core/memory.h"
#include "ue/call.h"
#include "ue/reflection.h"
#include "vr/math.h"

namespace holevr::game::custom {
namespace {

using vr::FQuat;
using vr::FRotator;
using vr::FVector;

// FTransform (double precision) as returned by K2_GetComponentToWorld.
struct alignas(16) Xform {
    FQuat rotation;
    FVector translation;
    double pad0 = 0;
    FVector scale{1, 1, 1};
    double pad1 = 0;
};
static_assert(sizeof(Xform) == 0x60, "FTransform layout");

Xform ComponentToWorld(UObject comp) {
    ue::Call c(comp, "K2_GetComponentToWorld");
    if (!c.Invoke()) return Xform{};
    return c.Result<Xform>();
}
FVector ActorLocation(UObject a) {
    ue::Call c(a, "K2_GetActorLocation");
    c.Invoke();
    return c.Result<FVector>();
}
FQuat ActorRotation(UObject a) {
    ue::Call c(a, "K2_GetActorRotation");
    c.Invoke();
    return vr::ToQuat(c.Result<FRotator>());
}

std::string NameOf(const FName& n) { return ue::names::ToString(n); }

// TArray { T* Data; int32 Num; int32 Max; }
struct ArrayView {
    uintptr_t data = 0;
    int32_t num = 0;
};
bool ReadArray(uintptr_t at, ArrayView* out, int32_t max_num = 100000) {
    uintptr_t d = 0;
    int32_t n = 0, m = 0;
    if (!mem::Read(at, d) || !mem::Read(at + 8, n) || !mem::Read(at + 12, m)) return false;
    if (n < 0 || n > m || n > max_num || (n > 0 && !d)) return false;
    out->data = d;
    out->num = n;
    return true;
}

// TSet / TMap storage (UE 5.x): TSparseArray Elements {TArray Data;
// TBitArray AllocationFlags {uint32 Inline[4]; uint32* Secondary; int32
// NumBits, MaxBits}; int32 FirstFreeIndex, NumFreeIndices} then the hash.
// Calls fn(element address) for each element in use. `stride` is
// sizeof(TSetElement<T>): the value plus HashNextId and HashIndex (8 bytes).
bool ForEachSparse(uintptr_t at, size_t stride, const std::function<void(uintptr_t)>& fn) {
    ArrayView a;
    if (!ReadArray(at, &a)) return false;
    const int32_t num_free = mem::ReadOr<int32_t>(at + 0x34, -1);
    if (num_free < 0 || num_free > a.num) return false;
    const uintptr_t secondary = mem::ReadOr<uintptr_t>(at + 0x20);
    const uintptr_t bits = secondary ? secondary : at + 0x10;
    for (int32_t i = 0; i < a.num; ++i) {
        if (num_free > 0) {
            uint32_t word = 0;
            if (!mem::Read(bits + (i / 32) * 4ull, word)) return false;
            if (!(word & (1u << (i % 32)))) continue;
        }
        fn(a.data + i * stride);
    }
    return true;
}

// Offset of a member of a Blueprint struct by its plain name (the engine
// adds _N_<GUID> to member names), -1 if missing.
int32_t Member(UObject ustruct, const char* name) {
    static std::mutex m;
    static std::map<std::pair<UObject, std::string>, int32_t> cache;
    std::lock_guard<std::mutex> lock(m);
    auto key = std::make_pair(ustruct, std::string(name));
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    int32_t found = -1;
    const size_t n = std::strlen(name);
    for (const auto& p : ue::reflection::GetProperties(ustruct)) {
        if (p.name == name || (p.name.size() > n && p.name.compare(0, n, name) == 0 && p.name[n] == '_')) {
            found = p.offset;
            break;
        }
    }
    if (ustruct) cache[key] = found;
    return found;
}

UObject Struct(const char* full_name) {
    static std::mutex m;
    static std::map<std::string, UObject> cache;
    {
        std::lock_guard<std::mutex> lock(m);
        auto it = cache.find(full_name);
        if (it != cache.end()) return it->second;
    }
    UObject s = ue::FindObject(full_name);
    std::lock_guard<std::mutex> lock(m);
    if (s) cache[full_name] = s;
    return s;
}

constexpr const char* kPartsData = "UserDefinedStruct /Game/DataBase/Str_GunPartsData.Str_GunPartsData";
constexpr const char* kPartsAndPath = "UserDefinedStruct /Game/Weapons/Custom/Data/Str_PartsAndPath.Str_PartsAndPath";
constexpr const char* kSaveTemp =
    "UserDefinedStruct /Game/Weapons/Custom/Data/Str_WeaponCustomSaveData_Temp.Str_WeaponCustomSaveData_Temp";

std::vector<uint8_t> ReadBytes(uintptr_t array_at) {
    std::vector<uint8_t> out;
    ArrayView a;
    if (!ReadArray(array_at, &a, 64) || a.num == 0) return out;
    out.resize(a.num);
    if (!mem::IsReadable(a.data, a.num) || !mem::SafeCopy(out.data(), reinterpret_cast<const void*>(a.data), a.num))
        out.clear();
    return out;
}

// Soft object pointer (TPersistentObjectPtr<FSoftObjectPath>): weak pointer
// (8), then the path's package name and asset name (FNames), then a sub path.
std::string SoftPath(uintptr_t at) {
    FName package, asset;
    if (!mem::Read(at + 0x08, package) || !mem::Read(at + 0x10, asset)) return {};
    std::string p = NameOf(package), a = NameOf(asset);
    if (p.empty() || p == "None") return {};
    return p + "." + a;
}

std::string Short(const std::string& path) {
    size_t dot = path.rfind('.');
    return dot == std::string::npos ? path : path.substr(dot + 1);
}

}  // namespace

std::string NameString(const FName& n) { return NameOf(n); }

std::string PathString(const std::vector<uint8_t>& path) {
    std::string s = "[";
    for (size_t i = 0; i < path.size(); ++i) s += (i ? "," : "") + std::to_string(path[i]);
    return s + "]";
}

UObject GameInstance(UObject world_context) {
    static UObject statics = 0;
    if (!statics) statics = ue::FindObject("GameplayStatics /Script/Engine.Default__GameplayStatics");
    if (!statics || !world_context) return 0;
    ue::Call c(statics, "GetGameInstance");
    c.Set("WorldContextObject", world_context);
    if (!c.Invoke()) return 0;
    return c.Result<UObject>();
}

UObject SaveData(UObject gi) {
    UObject s = 0;
    if (!gi || !ue::GetProp(gi, "SaveData", s)) return 0;
    return s;
}

bool Purchased(UObject gi, std::vector<FName>* out) {
    out->clear();
    UObject save = SaveData(gi);
    if (!save) return false;
    const int32_t o = ue::PropertyOffset(save, "WeaponPartsPurchasedItems");
    if (o < 0) return false;
    return ForEachSparse(save + o, 16, [&](uintptr_t e) {
        FName n;
        if (mem::Read(e, n)) out->push_back(n);
    });
}

bool Database(UObject gi, std::vector<PartRow>* out) {
    out->clear();
    UObject dt = 0;
    if (!gi || !ue::GetProp(gi, "GunPartsDB", dt) || !dt) return false;
    // UDataTable: RowStruct (a UPROPERTY) is followed by the native
    // TMap<FName, uint8*> RowMap.
    const int32_t rs = ue::reflection::FindPropertyOffset(ue::GetClass(dt), "RowStruct");
    if (rs < 0) return false;
    UObject row_struct = mem::ReadOr<uintptr_t>(dt + rs);
    if (!row_struct || ue::GetName(row_struct) != "Str_GunPartsData") {
        log::Warn("workbench: GunPartsDB rows are %s, not Str_GunPartsData", ue::GetName(row_struct).c_str());
        return false;
    }
    const int32_t o_cat = Member(row_struct, "GunPartsCategory");
    const int32_t o_attach = Member(row_struct, "AttachID");
    const int32_t o_sub = Member(row_struct, "AttachSubID");
    const int32_t o_bp = Member(row_struct, "BP");
    const int32_t o_cost = Member(row_struct, "UnlockDataCost");
    if (o_cat < 0 || o_attach < 0 || o_bp < 0) return false;
    return ForEachSparse(dt + rs + 8, 24, [&](uintptr_t e) {
        PartRow r;
        uintptr_t data = 0;
        if (!mem::Read(e, r.row) || !mem::Read(e + 8, data) || !data) return;
        r.name = NameOf(r.row);
        r.category = mem::ReadOr<uint8_t>(data + o_cat, 0xFF);
        r.attach_id = NameOf(mem::ReadOr<FName>(data + o_attach));
        if (o_sub >= 0) r.attach_sub_id = NameOf(mem::ReadOr<FName>(data + o_sub));
        r.class_path = SoftPath(data + o_bp);
        r.bp_soft = data + o_bp;
        if (o_cost >= 0) r.unlock_cost = mem::ReadOr<int32_t>(data + o_cost);
        out->push_back(std::move(r));
    });
}

bool Saved(UObject gi, const FName& weapon_row, std::vector<SavedPart>* out) {
    out->clear();
    UObject save = SaveData(gi);
    if (!save) return false;
    const int32_t o = ue::PropertyOffset(save, "WeaponCustomSaveData_Temp");
    UObject pp = Struct(kPartsAndPath), temp = Struct(kSaveTemp);
    if (o < 0 || !pp || !temp) return false;
    const int32_t o_list = Member(temp, "CustomParts");
    const int32_t o_row = Member(pp, "PartsRowName"), o_path = Member(pp, "WeaponLocalPath");
    const int32_t pp_size = ue::reflection::GetPropertiesSize(pp);
    if (o_list < 0 || o_row < 0 || o_path < 0 || pp_size <= 0) return false;
    bool found = false;
    // TMap<FName, Str_WeaponCustomSaveData_Temp>: key, 16-byte value, hash ids.
    bool ok = ForEachSparse(save + o, 8 + 16 + 8, [&](uintptr_t e) {
        FName key;
        if (found || !mem::Read(e, key) || key.comparison_index != weapon_row.comparison_index ||
            key.number != weapon_row.number)
            return;
        found = true;
        ArrayView a;
        if (!ReadArray(e + 8 + o_list, &a, 64)) return;
        for (int32_t i = 0; i < a.num; ++i) {
            const uintptr_t el = a.data + static_cast<uintptr_t>(i) * pp_size;
            SavedPart p;
            if (!mem::Read(el + o_row, p.row)) continue;
            p.path = ReadBytes(el + o_path);
            out->push_back(std::move(p));
        }
    });
    return ok && found;
}

bool WeaponRow(UObject gun, FName* out) {
    const int32_t o = gun ? ue::PropertyOffset(gun, "WeaponCustomMetaData") : -1;
    // Str_WeaponCustomMetaData starts with WeaponRowName.
    return o >= 0 && mem::Read(gun + o, *out) && out->comparison_index != 0;
}

std::vector<Slot> Slots(UObject gun) {
    std::vector<Slot> out;
    for (int sub = 0; sub < 2; ++sub) {
        const int32_t o = ue::PropertyOffset(gun, sub ? "WeaponPartsSubSlotPoints" : "WeaponPartsSlotPoints");
        ArrayView a;
        if (o < 0 || !ReadArray(gun + o, &a, 64)) continue;
        for (int32_t i = 0; i < a.num; ++i) {
            UObject c = mem::ReadOr<uintptr_t>(a.data + i * 8ull);
            if (!c) continue;
            Slot s;
            s.component = c;
            s.sub = sub != 0;
            s.name = ue::GetName(c);
            uint8_t idx = 0xFF;
            FName id;
            if (sub) {
                ue::GetProp(c, "ParentSlotIndex", idx);
                ue::GetProp(c, "SlotSubID", id);
            } else {
                ue::GetProp(c, "SlotIndex", idx);
                ue::GetProp(c, "SlotID", id);
                bool empty_ok = false;
                ue::GetProp(c, "EnablePartsEmpty", empty_ok);
                s.can_be_empty = empty_ok;
            }
            s.index = idx;
            s.slot_id = NameOf(id);
            out.push_back(std::move(s));
        }
    }
    return out;
}

std::vector<Installed> InstalledParts(UObject gun) {
    std::vector<Installed> out;
    const int32_t o = ue::PropertyOffset(gun, "WeaponParts");
    ArrayView a;
    if (o < 0 || !ReadArray(gun + o, &a, 64)) return out;
    for (int32_t i = 0; i < a.num; ++i) {
        UObject p = mem::ReadOr<uintptr_t>(a.data + i * 8ull);
        if (!p) continue;
        Installed in;
        in.actor = p;
        FName row;
        if (ue::GetProp(p, "WeaponPartsRowName", row)) in.row = NameOf(row);
        uint8_t cat = 0xFF;
        if (ue::GetProp(p, "WeaponPartsCategory", cat)) in.category = cat;
        const int32_t op = ue::PropertyOffset(p, "SlotIndexPath");
        if (op >= 0) in.path = ReadBytes(p + op);
        ue::GetProp(p, "AttachPoint", in.attach_point);
        out.push_back(std::move(in));
    }
    return out;
}

bool WriteSaved(UObject gi, const FName& weapon_row, const std::vector<SavedPart>& parts) {
    UObject pp = Struct(kPartsAndPath);
    const int32_t o_row = pp ? Member(pp, "PartsRowName") : -1;
    const int32_t o_path = pp ? Member(pp, "WeaponLocalPath") : -1;
    const int32_t size = pp ? ue::reflection::GetPropertiesSize(pp) : 0;
    if (!gi || o_row < 0 || o_path < 0 || size < 0x18) return false;
    // Our memory: the game copies the struct into its save map (it does not
    // keep or free what it was given). Kept alive until the call returns.
    std::vector<std::vector<uint8_t>> paths;
    paths.reserve(parts.size());
    std::vector<uint8_t> elems(parts.size() * static_cast<size_t>(size), 0);
    for (size_t i = 0; i < parts.size(); ++i) {
        paths.push_back(parts[i].path);
        uint8_t* el = elems.data() + i * size;
        std::memcpy(el + o_row, &parts[i].row, sizeof(FName));
        const uintptr_t data = paths.back().empty() ? 0 : reinterpret_cast<uintptr_t>(paths.back().data());
        const int32_t n = static_cast<int32_t>(paths.back().size());
        std::memcpy(el + o_path, &data, 8);
        std::memcpy(el + o_path + 8, &n, 4);
        std::memcpy(el + o_path + 12, &n, 4);
    }
    struct {
        uintptr_t data;
        int32_t num, max;
    } list{parts.empty() ? 0 : reinterpret_cast<uintptr_t>(elems.data()), static_cast<int32_t>(parts.size()),
           static_cast<int32_t>(parts.size())};
    ue::Call c(gi, "SetSaveData_WeaponCustomSaveData");
    int32_t sz = 0;
    uint8_t* raw = c.Raw("WeaponCustomSaveData", &sz);
    if (!c.Valid() || !raw || sz != 16) return false;
    c.Set("WeaponRowName", weapon_row);
    std::memcpy(raw, &list, 16);
    if (!c.Invoke()) return false;
    return c.Get<bool>("Success");
}

UObject PartClass(const PartRow& part) {
    static std::map<std::string, UObject> cache;
    if (part.class_path.empty()) return 0;
    auto it = cache.find(part.class_path);
    if (it != cache.end() && it->second) return it->second;
    UObject cls = ue::FindObject("BlueprintGeneratedClass " + part.class_path);
    if (!cls && part.bp_soft) {
        // Not loaded yet (a part never fitted this session): the game's own
        // LoadClassAsset_Blocking, given a copy of the row's soft pointer.
        static UObject ksl = 0;
        if (!ksl) ksl = ue::FindObject("KismetSystemLibrary /Script/Engine.Default__KismetSystemLibrary");
        if (ksl) {
            ue::Call c(ksl, "LoadClassAsset_Blocking");
            int32_t size = 0;
            uint8_t* raw = c.Raw("AssetClass", &size);
            if (raw && size == 0x28 && mem::SafeCopy(raw, reinterpret_cast<const void*>(part.bp_soft), 0x28) &&
                c.Invoke())
                cls = c.Result<UObject>();
            log::Info("workbench: loaded the class of %s from disk: %s", part.name.c_str(), cls ? "ok" : "FAILED");
        }
    }
    if (cls) cache[part.class_path] = cls;
    return cls;
}

bool ApplyLoadout(UObject player, UObject gun, const std::vector<SavedPart>& parts, std::string* why) {
    auto fail = [&](const char* w) {
        if (why) *why = w;
        return false;
    };
    UObject gi = GameInstance(player);
    FName row;
    if (!gi || !gun || !WeaponRow(gun, &row)) return fail("no game instance or weapon row");
    // Tube-fed guns empty and refill their magazine when they rebuild
    // (ClearMagazine + Init_MagazineCreate): not for them.
    uint8_t mag_type = 0;
    if (ue::GetProp(gun, "MagazineType", mag_type) && mag_type == 1) return fail("tube magazine gun");
    // A magazine the game is moving between hand and gun (MagazineObj_Temp)
    // would be put in the gun by the rebuild (Gun_MagazineLoad).
    UObject temp_mag = 0;
    if (ue::GetProp(gun, "MagazineObj_Temp", temp_mag) && temp_mag) return fail("a magazine is on its way into the gun");
    const int32_t o_meta = ue::PropertyOffset(gun, "WeaponCustomMetaData");
    UObject meta_struct = Struct("UserDefinedStruct /Game/Weapons/Custom/Data/Str_WeaponCustomMetaData.Str_WeaponCustomMetaData");
    const int32_t o_list = meta_struct ? Member(meta_struct, "WeaponCustomPartsAndPath") : -1;
    if (o_meta < 0 || o_list < 0) return fail("gun metadata layout not found");

    if (!WriteSaved(gi, row, parts)) return fail("the game did not take the new list");

    ue::Call m(gi, "GetSaveData_WeaponCustomMetaData");
    if (!m.Valid()) return fail("no GetSaveData_WeaponCustomMetaData");
    m.Set("WeaponRowName", row);
    if (!m.Invoke() || !m.Get<bool>("Success")) return fail("the game built no metadata from the saved list");
    int32_t size = 0;
    uint8_t* raw = m.Raw("WeaponCustomMetaData", &size);
    struct Arr {
        uintptr_t data;
        int32_t num, max;
    } fresh{}, old{};
    if (!raw || size < o_list + 16) return fail("metadata parameter missing");
    FName meta_row;
    std::memcpy(&meta_row, raw, sizeof(FName));
    std::memcpy(&fresh, raw + o_list, sizeof(Arr));
    if (!SameName(meta_row, row) || fresh.num < 0 || fresh.num > 64 || fresh.num > fresh.max ||
        (fresh.num > 0 && !fresh.data))
        return fail("metadata from the game looks wrong");
    // Hand the game-allocated list to the gun; its old list stays with us
    // (a few hundred bytes, never freed: freeing needs the engine allocator).
    if (!mem::Read(gun + o_meta + o_list, old) || !mem::Write(gun + o_meta + o_list, fresh))
        return fail("could not write the gun's metadata");
    std::memcpy(raw + o_list, &old, sizeof(Arr));

    UObject wm = 0;
    if (!ue::GetProp(player, "WeaponManager", wm) || !wm) return fail("no weapon manager");
    ue::Call r(wm, "RefreshWeaponSpec");
    if (!r.Invoke()) return fail("RefreshWeaponSpec did not run");

    // The hub's weapon racks show the guns with their parts (the menu
    // refreshes them too).
    static UObject rack_class = 0, statics = 0;
    if (!rack_class)
        rack_class = ue::FindObject("BlueprintGeneratedClass /Game/GamePlay/LevelComponents/LCBP_WeaponEquip.LCBP_WeaponEquip_C");
    if (!statics) statics = ue::FindObject("GameplayStatics /Script/Engine.Default__GameplayStatics");
    if (rack_class && statics) {
        ue::Call all(statics, "GetAllActorsOfClass");
        all.Set("WorldContextObject", player).Set("ActorClass", rack_class);
        int32_t asz = 0;
        const uint8_t* araw = all.Invoke() ? all.Raw("OutActors", &asz) : nullptr;
        Arr racks{};
        if (araw && asz >= 16) std::memcpy(&racks, araw, sizeof(Arr));
        for (int32_t i = 0; i < racks.num && i < 16; ++i) {
            UObject a = mem::ReadOr<uintptr_t>(racks.data + i * 8ull);
            if (!a) continue;
            ue::Call rf(a, "Refresh");
            rf.Set("IsNewWeapon", false);
            rf.Invoke();
        }
    }
    return true;
}

void Study(UObject player, UObject bench) {
    UObject gi = GameInstance(player);
    log::Info("workbench study: game instance %s, save data %s", ue::GetName(gi).c_str(),
              ue::GetName(SaveData(gi)).c_str());
    if (bench) {
        const FVector bl = ActorLocation(bench);
        const FRotator br = vr::ToRotator(ActorRotation(bench));
        log::Info("workbench study: %s at (%.0f %.0f %.0f) yaw %.0f", ue::GetName(bench).c_str(), bl.X, bl.Y, bl.Z,
                  br.Yaw);
        for (const char* comp : {"DS_Workbench", "DS_ToolBox", "Plane", "HG", "Ammo"}) {
            UObject c = 0;
            if (!ue::GetProp(bench, comp, c) || !c) continue;
            const Xform t = ComponentToWorld(c);
            std::string box;
            UObject sm = 0;
            if (ue::GetProp(c, "StaticMesh", sm) && sm) {
                ue::Call b(sm, "GetBoundingBox");
                int32_t size = 0;
                if (b.Invoke()) {
                    const uint8_t* raw = b.Raw("ReturnValue", &size);
                    if (raw && size >= 48) {
                        double v[6];
                        std::memcpy(v, raw, sizeof(v));
                        char buf[160];
                        snprintf(buf, sizeof(buf), ", mesh box (%.1f %.1f %.1f)..(%.1f %.1f %.1f) scale (%.2f %.2f %.2f)",
                                 v[0], v[1], v[2], v[3], v[4], v[5], t.scale.X, t.scale.Y, t.scale.Z);
                        box = buf;
                    }
                }
            }
            const FRotator r = vr::ToRotator(t.rotation);
            log::Info("workbench study:   %s at (%.1f %.1f %.1f) rot (%.0f %.0f %.0f)%s", comp, t.translation.X,
                      t.translation.Y, t.translation.Z, r.Pitch, r.Yaw, r.Roll, box.c_str());
        }
        UObject box = 0;
        if (ue::GetProp(bench, "Box", box) && box) {
            ue::Call e(box, "GetScaledBoxExtent");
            e.Invoke();
            const FVector ext = e.Result<FVector>();
            const Xform t = ComponentToWorld(box);
            log::Info("workbench study:   interact box at (%.1f %.1f %.1f), half size (%.1f %.1f %.1f)",
                      t.translation.X, t.translation.Y, t.translation.Z, ext.X, ext.Y, ext.Z);
        }
    }

    std::vector<PartRow> db;
    const bool have_db = Database(gi, &db);
    std::map<std::string, const PartRow*> by_name;
    for (const auto& r : db) by_name[r.name] = &r;
    log::Info("workbench study: parts database: %s, %zu rows", have_db ? "read" : "NOT READ", db.size());
    for (const auto& r : db)
        log::Info("workbench study:   part %s: category %d, fits slot \"%s\" (sub \"%s\"), class %s, cost %d",
                  r.name.c_str(), r.category, r.attach_id.c_str(), r.attach_sub_id.c_str(),
                  Short(r.class_path).c_str(), r.unlock_cost);

    std::vector<FName> owned;
    const bool have_owned = Purchased(gi, &owned);
    std::string owned_s;
    for (const auto& n : owned) owned_s += (owned_s.empty() ? "" : ", ") + NameOf(n);
    log::Info("workbench study: owned parts (%s, %zu): %s", have_owned ? "read" : "NOT READ", owned.size(),
              owned_s.c_str());

    UObject wm = 0;
    ue::GetProp(player, "WeaponManager", wm);
    for (const char* slot : {"Obj_PrimaryWeapon", "Obj_SecondaryWeapon"}) {
        UObject gun = 0;
        if (!wm || !ue::GetProp(wm, slot, gun) || !gun) {
            log::Info("workbench study: %s: none", slot);
            continue;
        }
        FName row;
        const bool have_row = WeaponRow(gun, &row);
        const FVector gl = ActorLocation(gun);
        const FQuat gq = ActorRotation(gun);
        log::Info("workbench study: %s: %s, weapon row %s", slot, ue::GetName(ue::GetClass(gun)).c_str(),
                  have_row ? NameOf(row).c_str() : "?");
        for (const Slot& s : Slots(gun)) {
            const Xform t = ComponentToWorld(s.component);
            const FVector local = gq.Inverse().Rotate(t.translation - gl);
            log::Info("workbench study:   %s %s: index %d, id \"%s\"%s, at (%.1f %.1f %.1f) on the gun",
                      s.sub ? "sub slot" : "slot", s.name.c_str(), s.index, s.slot_id.c_str(),
                      s.can_be_empty ? ", may be empty" : "", local.X, local.Y, local.Z);
        }
        for (const Installed& p : InstalledParts(gun)) {
            const FVector local = gq.Inverse().Rotate(ActorLocation(p.actor) - gl);
            std::string ap;
            if (p.attach_point) {
                const FVector a = gq.Inverse().Rotate(ComponentToWorld(p.attach_point).translation - gl);
                char buf[96];
                snprintf(buf, sizeof(buf), ", attach point (%.1f %.1f %.1f)", a.X, a.Y, a.Z);
                ap = buf;
            }
            log::Info("workbench study:   part %s (%s): category %d, slot path %s, at (%.1f %.1f %.1f)%s",
                      p.row.c_str(), ue::GetName(ue::GetClass(p.actor)).c_str(), p.category,
                      PathString(p.path).c_str(), local.X, local.Y, local.Z, ap.c_str());
        }
        if (!have_row) continue;
        std::vector<SavedPart> saved;
        if (Saved(gi, row, &saved)) {
            std::string s;
            for (const auto& p : saved) s += (s.empty() ? "" : ", ") + NameOf(p.row) + PathString(p.path);
            log::Info("workbench study:   saved attachments for %s: %s", NameOf(row).c_str(),
                      s.empty() ? "(none)" : s.c_str());
        } else {
            log::Info("workbench study:   no saved attachments for %s", NameOf(row).c_str());
        }
        // Which owned parts the game lists for this gun, per category.
        for (int cat = 0; cat < 16; ++cat) {
            ue::Call c(gi, "GetGunPartsDataByCategoryFromDB");
            if (!c.Valid()) break;
            c.Set("WeaponRowName", row).Set("PartsCategory", static_cast<uint8_t>(cat));
            c.Invoke();
            int32_t size = 0;
            const uint8_t* raw = c.Raw("GunPartsRowNames", &size);
            ArrayView a;
            if (!raw || size != 16 || !ReadArray(reinterpret_cast<uintptr_t>(raw), &a, 256) || a.num == 0) continue;
            std::string s;
            for (int32_t i = 0; i < a.num; ++i) {
                FName n = mem::ReadOr<FName>(a.data + i * 8ull);
                std::string name = NameOf(n);
                bool mine = false;
                for (const auto& o : owned)
                    if (o.comparison_index == n.comparison_index && o.number == n.number) mine = true;
                s += (s.empty() ? "" : ", ") + name + (mine ? " (owned)" : "");
            }
            log::Info("workbench study:   category %d parts for %s: %s", cat, NameOf(row).c_str(), s.c_str());
        }
    }
}

}  // namespace holevr::game::custom
