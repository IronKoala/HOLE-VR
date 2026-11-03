# VR workbench: swap gun attachments by hand

Owner ask (2026-11-03): at the hub workbench, lay the gun on the table and
pull attachments off it by grabbing them. The attachments you own lie on the
table; grab one and put it on the gun by hand to install it. It must drive the
game's real attachment system (stats, aiming point, light, saved loadout), not
a look-alike.

## How the game does it (class dump 2026-11-03, hub level Area000_Home)

| Piece | Game class / data | Notes |
|---|---|---|
| Workbench | `LCBP_WeaponCustom_C` (in `LCBPGroup_WPSWC`) | Meshes `DS_Workbench`, `DS_ToolBox`, a decorative pistol (`HG` + `HG_9x19_S_*` meshes), mags and rounds; `Box` interact trigger; `Interact()` opens the menu through `BPC_WeaponCustomManager_C.OpenWeaponCustomUI`. |
| Menu | `UIM_WeaponCustom_C` | Weapon tabs HG / AR / SG / SR, up to 8 slot buttons, 5 part buttons. Shows the gun in a separate photo booth (`LCBP_WeaponCustomSampleBox_C`, scene capture) and possesses `BP_UIControllPawn_C` while open. **Not used in VR** (no boxed menus). |
| Equip | `UIM_WeaponCustom_C.SelectParts` | Builds the new list (`CreateNewWeaponCustomSaveData`), writes it with the game instance's `SetSaveData_WeaponCustomSaveData(WeaponRowName, Str_WeaponCustomSaveData_Temp)`, then touches every `LCBP_WeaponEquip_C` rack and calls the player's `BPI_ToWeaponManager` (`RefreshWeapon`). Exact order: see `dump/script/UIM_WeaponCustom_C.txt` once dumped. |
| Saved loadout | `SGO_SaveData_C.WeaponCustomSaveData_Temp` (map weapon row -> list of `Str_PartsAndPath {PartsRowName, WeaponLocalPath: bytes}`) | Read in place by `game::custom::Saved`. |
| Owned parts | `SGO_SaveData_C.WeaponPartsPurchasedItems` (set of part rows) | Read in place by `game::custom::Purchased`. Blueprint unlocks are a separate map (`WeaponPartsUnlockStatus`). |
| Part data | Game instance `GunPartsDB` (DataTable of `Str_GunPartsData`: category, `AttachID`, `AttachSubID`, spec, cost, icons, `BP` class) | Read in place by `game::custom::Database`. |
| Gun slots | `BP_WeaponPartsSlotPoint_C` (`SlotIndex`, `SlotID`, `EnablePartsEmpty`) and `BP_WeaponPartsSubSlotPoint_C` (`ParentSlotIndex`, `SlotSubID`) on the gun; listed in `WeaponPartsSlotPoints` / `WeaponPartsSubSlotPoints` | Pistol `BP_HG_C`: `SP_Sight`, `SP_MuzzleDevice`, `SP_LightAndLaser_Down`, `SP_Magazine`, `SP_Ammo`, sub slot `SubSP_SlideMuzzle`. Rifle `BP_AR_C`: sight, muzzle, light/laser top/right/left/down, magazine, ammo. |
| Installed parts | One actor per part (`BP_WeaponPartsBase_C`: `AttachPoint`, `WeaponPartsRowName`, `WeaponPartsCategory`, `SlotIndexPath`), in the gun's `WeaponParts` | Spawned by the gun's `Init_GenerateWeaponPartsByCustomData` from `WeaponCustomMetaData` (row + list of part class and slot path). |
| Gun stats after a change | `BP_GunBase_C`: `Init_GunSpec`, `Init_ADSPoint`, `Init_MuzzlePointAndFX`, `Init_FireSound`, `RefreshGunSpec` | Called by the game's own refresh. |

At dump time the owner's pistol carried `BPC_IronSight_HG_C` (sight slot),
`BPC_WeaponLight_Compact_HG_C` (light slot), a `BPC_Mag_HG_9x19mm_10Rx_C`
magazine and 9x19 rounds; the parts each owner owns are logged by the study
pass (below).

## Study pass (build 1)

Nothing changes in play. On each launch:

- `dump/script/<Class>.txt`: bytecode listings of the weapons, their parts,
  the workbench, the menu, the player and the save data (`ue/script_dump.*`,
  ini `dump_script_classes`). Log: `script dump: N classes, M functions
  listed ... (K listings stopped at an unknown byte)`; K should be 0.
- In the hub, 6 s after the level starts: `workbench study:` lines with the
  bench position and table size, every part in the game's database, the parts
  the player owns, each gun's slots and installed parts (positions on the
  gun), the saved loadout, and which parts the game offers per category.
  Off with `workbench_study=0`.
- `workbench: <hand> hand at the bench ...` the first time a hand comes within
  1.2 m of the bench in a level.

## Plan (build 2, after reading the study)

1. Lay the gun down: let go of the pistol over the table top and it stays
   there, lying on its side, instead of flying back to the holster. Grab it
   again to pick it up. Walking away leaves it until you come back (or it
   flies to the holster past a set distance).
2. Take a part off: either hand grabs a removable part on the gun (a slot the
   game allows empty, or any part when you put another in its place). It
   comes off in the hand; let go away from the gun and it lies on the table.
   The gun then really loses it (saved loadout + the game's refresh).
3. Put a part on: owned parts that fit the gun lie in a row on the table.
   Grab one, bring it to its slot: near the slot it snaps into place in the
   hand (haptic tick); let go and it is installed through the game. A part
   already in that slot comes off onto the table.
4. Magazine type and ammo type are parts too; they stay on the normal
   magazine handling for now.

No boxed menus; nothing new on the screen except the parts themselves.
