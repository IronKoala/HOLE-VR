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

## What the study found (owner's run, 2026-11-03)

- The owner owns only the starting parts (database cost -1): for the pistol
  the iron sight, the compact light, the 10-round magazine and FMJ ammo. The
  rest is sold at the parts shop that unlocks together with the bench.
- Pistol `HG001` slots: ammo `[0]` (`MAG_9X19`), magazine `[1]` (`MAGP_HG`),
  muzzle `[2]` (may be empty; the compensator mounts on the sub slot
  `SubSP_SlideMuzzle`), sight `[3]` (never empty), light `[7]` (may be empty).
- The bench is part of the hub facility `HF_WPSWC_C` (base
  `LCBP_HomeFacilityBase_C`, `FacilityState` 0 empty / 1 yellow sign / 2
  unlocked; bought at its `LCBP_YellowSign_C`). While locked, the bench actor
  sits at the world origin.
- From the bytecode (`dump/script`):
  - The menu (`UIM_WeaponCustom_C.SelectParts`) replaces or adds one entry
    `{part, [slot index]}` in the saved list; taking a part off writes the
    part row `Empty` for that slot. `SetSaveData_WeaponCustomSaveData` also
    saves the game to disk.
  - The menu then calls the player's `RefreshWeapon`, which runs
    `SetupPlayer`: every weapon is respawned and the health state is set up
    again. Not used in VR.
  - `BPC_WeaponManager_C.RefreshWeaponSpec` has each gun run
    `RefreshGunSpec` = `InitializeWeapon(false)`: collect slots, destroy and
    respawn the part actors from the gun's own `WeaponCustomMetaData`, and
    recompute stats, sight point, muzzle, sounds. Same gun actor; the
    magazine is only re-attached (`Gun_MagazineLoad`), except on tube-fed
    guns (`MagazineType` 1), which empty their tube.

## Build 2: the hands-on bench

- **Where**: in the hub. Until the workbench is unlocked, any table in the hub
  (`workbench_tables_before_unlock=1`); once unlocked, within 2.5 m of the
  bench only.
- **Lay the pistol down**: let go of it (outside its holster zone) with a
  flat surface up to 35 cm under the hand: it lies there on its side, barrel
  where it pointed, the side that faced up still up. Grab it like from the
  holster to pick it up. Walk 3 m away (`workbench_walk_away_m`) and it goes
  back to the holster and the table is cleared. Controls hooks:
  `workbench::ParkedGun` / `TryPark` / `GunTaken` in the pistol holster code.
- **Attachments on the gun**: either free hand, within 3 cm of a part
  (`workbench_grab_cm`), squeezes it off. The gun's own part is hidden while
  it is in the hand. Let go away from the gun: a slot the game allows empty
  is emptied for real (the part lies on the table); the sight slot is never
  empty, so the sight goes back on.
- **The table**: owned parts that fit the gun and are not on it lie in a row
  14 cm toward you, 9 cm apart, turned the way they sit on the gun.
- **Fitting**: bring a part's mounting point within 5 cm of its slot
  (`workbench_snap_cm`): it snaps onto the gun (haptic tick). Let go there
  and it is fitted for real. A part it replaces lands on the table. Works on
  the gun lying on the table or held in the right hand.
- **Grips**: while the workbench has a hand's grip (holding a part, or the
  hand at a part and closer to it than to the gun's grip), controls ignores
  that grip (`workbench::ClaimsGrip`): no draw, slide or magazine grab.
- **Every change** goes through `game::custom::ApplyLoadout`: the saved list
  (like the menu), the game's metadata for the gun, `RefreshWeaponSpec`, the
  hub weapon racks' `Refresh`. Then the gun's part actors are read back to
  check the slot really changed.

### Log lines to check

- `workbench: workbench locked|UNLOCKED (facility state N, bench at ...)`
- `workbench: pistol laid on <actor> at (...), right side up; it carries ...`
- `workbench: on the table beside the HG001: LIGHT_HG_LIGHT, ...`
- `workbench: left hand took LIGHT_HG_LIGHT off the HG001 (slot SP_LightAndLaser_Down)`
- `workbench: take LIGHT_HG_LIGHT off SP_LightAndLaser_Down: done, checked on the gun. The gun now carries ...; saved: ...`
- `workbench: LIGHT_HG_LIGHT lines up with SP_LightAndLaser_Down`
- `workbench: fit LIGHT_HG_LIGHT in SP_LightAndLaser_Down: done, checked on the gun. ...`
- `workbench: SIGHT_HG_IRONSIGHT back on the gun: the game always needs a part there`
- Failures: `workbench: ... did not happen: <reason>`.

Not yet: magazine and ammo types (they stay on the normal magazine handling),
rifles and shotguns (no holster yet; tube-fed guns refuse the rebuild), hand
poses for holding a part.
