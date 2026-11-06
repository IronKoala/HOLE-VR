#pragma once
#include <cmath>
#include "game/weapon_custom.h"

namespace holevr::game::custom::tube_refresh {
// BP_WeaponBase.InitializeWeapon + BP_GunBase.Init_CalcurateWeaponSpec,
// verified against the local Blueprint dump. Refresh cosmetic attachments
// without ClearMagazine/Init_MagazineCreate (which replace the internally
// fed SG/SR ammunition). Used for all four supported cosmetic loadouts;
// ammo/magazine slots must be unchanged by the caller.
inline constexpr const char* kSteps[] = {
    "Init_CollectWeaponPartsSlotPoint", "Init_GenerateWeaponParts",
    "Init_GunSpec", "Init_MuzzlePointAndFX", "Init_ADSPoint",
    "Init_AmmoClass", "Init_MagazineClass", "Init_HeatMaterials", "Init_FireSound",
    "Init_SoundParam", "Init_GunAnimParam"
};

// All four guns' SP_Ammo / SP_Magazine occupy root slots 0 and 1. Cosmetic
// attachment edits must leave these rows and their nested paths alone.
inline bool SameAmmo(const std::vector<SavedPart>& before, const std::vector<SavedPart>& after) {
    auto contains=[](const SavedPart& a,const std::vector<SavedPart>& list) {
        for (const auto& b:list) if (a.path==b.path && SameName(a.row,b.row)) return true;
        return false;
    };
    for (const auto& a:before)
        if (!a.path.empty() && a.path[0]<=1 && !contains(a,after)) return false;
    for (const auto& a:after)
        if (!a.path.empty() && a.path[0]<=1 && !contains(a,before)) return false;
    return true;
}

template<class Engine>
bool Ready(Engine& e) {
    for (const char* step : kSteps) if (!e.Has(step)) return false;
    return e.RateFieldsValid();
}

template<class Engine>
bool Run(Engine& e) {
    if (!Ready(e)) return false; // No partial rebuild when an entry point is missing.
    for (int i=0;i<9;++i) if (!e.Call(kSteps[i])) return false;
    const double rpm=e.Rpm();
    if (!std::isfinite(rpm) || rpm<=0 || !e.SetSecondsPerRound(60.0/rpm)) return false;
    for (int i=9;i<11;++i) if (!e.Call(kSteps[i])) return false;
    return true;
}
}  // namespace holevr::game::custom::tube_refresh
