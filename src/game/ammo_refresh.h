#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>
#include "ue/reflection.h"

namespace holevr::game::custom::ammo_refresh {
// Verified cooked Blueprint defaults: SR is internally fed like SG; it is
// not a detachable-magazine exception. Both must skip ClearMagazine.
inline bool MagazineLayoutValid(std::string_view weapon, uint8_t type) {
    if (weapon == "BP_HG_C" || weapon == "BP_AR_C") return type == 0;
    if (weapon == "BP_SG_C" || weapon == "BP_SR_C") return type == 1;
    return false;
}
// Exact names from HOLE's reflected classes: BP_MagazineBase_C and
// BP_SingleBullet_LineTrace_C use MuzzlePoint; BP_AmmoBase_C uses Muzzle Point.
// Do not normalize arbitrary property names or accept an unverified layout.
template<class Find>
ue::reflection::PropertyInfo MuzzleProperty(Find find) {
    for (const char* name : {"MuzzlePoint", "Muzzle Point"}) {
        auto p = find(name);
        if (!p.valid()) continue;
        if (p.type == "ObjectProperty" && p.element_size == 8 && p.array_dim == 1 && p.offset >= 0)
            return p;
        return {};
    }
    return {};
}

struct Stat { const char* gun; const char* ammo; const char* projectile; };
// Matches BP_AmmoBase_C.InitializeBullet. Cached projectiles are not attached
// actors; they live in MetaBullet_LineTrace and need updating separately.
inline constexpr Stat kStats[] = {
    {"GunDamage", "AmmoDamage", "Damage"},
    {"GunStunPower", "AmmoStunPower", "StunPower"},
    {"GunDoT_Damage", "AmmoDoT_Damage", "DoT_Damage"},
    {"GunDoT_Stun", "AmmoDoT_Stun", "DoT_Stun"},
    {"GunDoT_Time", "AmmoDoT_Time", "DoT_Time"},
    {"GunDoT_Interval", "AmmoDoT_Interval", "DoT_Interval"},
    {"GunPenetrationPower", "AmmoPenetrationPower", "PenetrationPower"},
    {"GunMOA", "AmmoMOA", "MOA"}
};
inline constexpr size_t kPenetration = 6;
using Values = std::array<double, 8>;
inline bool ProjectileValues(const Values& gun, const Values& ammo, Values* out) {
    Values result{};
    for (size_t i = 0; i < result.size(); ++i) {
        result[i] = gun[i] + ammo[i];
        if (!std::isfinite(result[i])) return false;
    }
    // Blueprint FTrunc -> IntProperty. Check range before the C++ cast.
    const double penetration = std::trunc(result[kPenetration]);
    if (penetration < std::numeric_limits<int32_t>::min() ||
        penetration > std::numeric_limits<int32_t>::max()) return false;
    result[kPenetration] = penetration;
    *out = result;
    return true;
}
}  // namespace holevr::game::custom::ammo_refresh
