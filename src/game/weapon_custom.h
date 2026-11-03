#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "ue/names.h"
#include "ue/objects.h"

// HOLE's weapon customisation ("workbench") system, read from the game's own
// data so the VR workbench can drive the real thing:
//
//   - Every attachment ("weapon part") is a row of the game's GunPartsDB
//     table (Str_GunPartsData): category, AttachID (which slot it fits),
//     AttachSubID, the part's Blueprint class.
//   - A gun Blueprint (BP_HG_C, BP_AR_C...) has slot points
//     (BP_WeaponPartsSlotPoint_C: SlotIndex, SlotID, EnablePartsEmpty) and
//     spawns one part actor (BP_WeaponPartsBase_C) per installed part.
//   - What is installed is kept in the save data per weapon row
//     (SGO_SaveData_C.WeaponCustomSaveData_Temp: part row + slot path);
//     what the player owns is SGO_SaveData_C.WeaponPartsPurchasedItems.
//   - The game's workbench menu (UIM_WeaponCustom_C.SelectParts) writes the
//     save data through the game instance and then refreshes the weapons.
//
// Read-only helpers read containers in place (no engine allocations).
// Game thread only.
namespace holevr::game::custom {
using ue::FName;
using ue::UObject;

struct PartRow {
    FName row;
    std::string name;          // row name, e.g. "WeaponLight_Compact_HG"
    int category = -1;         // EN_GunPartsCategory value
    std::string attach_id;     // the SlotID of the slot it fits
    std::string attach_sub_id;
    std::string class_path;    // its Blueprint class, /Game/....X_C
    int unlock_cost = 0;
};

struct Slot {
    UObject component = 0;     // BP_WeaponPartsSlotPoint_C / SubSlotPoint
    std::string name;          // component name, e.g. "SP_Sight"
    bool sub = false;          // a sub slot (on a part or alternate mount)
    int index = -1;            // SlotIndex (ParentSlotIndex for sub slots)
    std::string slot_id;       // SlotID (SlotSubID for sub slots)
    bool can_be_empty = false; // EnablePartsEmpty
};

struct Installed {
    UObject actor = 0;         // the part actor on the gun
    std::string row;           // WeaponPartsRowName
    int category = -1;
    std::vector<uint8_t> path; // SlotIndexPath
    UObject attach_point = 0;  // the part's AttachPoint component
};

struct SavedPart {
    FName row;
    std::vector<uint8_t> path;  // WeaponLocalPath (slot indices)
};

UObject GameInstance(UObject world_context);
UObject SaveData(UObject game_instance);

// Parts the player owns (bought). False if the save data can't be read.
bool Purchased(UObject game_instance, std::vector<FName>* out);
// Every gun part row in the game's database.
bool Database(UObject game_instance, std::vector<PartRow>* out);
// The saved attachment list for one weapon row ("HG_...").
bool Saved(UObject game_instance, const FName& weapon_row, std::vector<SavedPart>* out);

// The gun's weapon row (WeaponCustomMetaData.WeaponRowName).
bool WeaponRow(UObject gun, FName* out);
std::vector<Slot> Slots(UObject gun);
std::vector<Installed> InstalledParts(UObject gun);

// Writes the saved attachment list for a weapon row through the game's own
// SetSaveData_WeaponCustomSaveData (what the workbench menu calls).
bool WriteSaved(UObject game_instance, const FName& weapon_row, const std::vector<SavedPart>& parts);

std::string PathString(const std::vector<uint8_t>& path);

// Logs the workbench, the guns' slots and parts, the save data and what the
// player owns. Once per level (the caller decides).
void Study(UObject player, UObject workbench);

}  // namespace holevr::game::custom
