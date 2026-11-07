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
  unlocked; bought at its `LCBP_YellowSign_C`). Once bought, the facility's
  `CBP_2_Unlocked` child is `LCBPGroup_WPSWC_C` (long tables, parts shop,
  and the bench as its `LCBP_WeaponCustom` child actor). Before that the only
  object of the bench's class is that group's child-actor template
  (`LCBP_WeaponCustom_GEN_VARIABLE_LCBP_WeaponCustom_C_CAT`): not in any
  level, at the world origin. Build 2 took that template for the bench, so
  it would never have seen the real one; the search now skips class
  defaults and templates and keeps only actors owned by a Level.
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
  the gun lying on the table or held in either hand, including a rifle carried
  by its fore-end in the left hand.
- **Grips**: while the workbench has a hand's grip (holding a part, or the
  hand at a part and closer to it than to the gun's grip), controls ignores
  that grip (`workbench::ClaimsGrip`): no draw, slide or magazine grab.
- **Every change** goes through `game::custom::ApplyLoadout`: the saved list
  (like the menu), the game's metadata for the gun, a target-only refresh, the
  hub weapon racks' `Refresh`. Then the gun's part actors are read back to
  check the slot really changed.

### The green zone (owner 2026-11-04, bought bench)

The bench's table (`DS_Workbench`, owner's run): 111 x 244 cm, top at 90 cm.
The game's display pistol (`HG`) lies 28 cm in from the long side the owner
stood at, and they laid the pistol 14-32 cm in from that edge, hand at full
reach. The owner: "its really hard to reach the table ... the area where you
let go of the gun? color it (#00FF8D) with see through opacity, and bring it
closer to the end of the table and stand it upright".

- A green box stands upright on the front edge of the table: 34 cm along the
  edge, 52 cm high, 14 cm deep, its middle 4 cm out past the edge, 2 cm above
  the top (`workbench_box_*`; the first build's `workbench_zone_*` keys are no
  longer read). Owner 07:57: "make it twice as tall, in the center of it i want
  100% visibility, the lines that make the box solid with a quickly gradual
  decrease in its opacity, if my body isnt near the box i dont want to see it
  at all". So its 12 edges are solid 0.6 cm lines, and each side has four rings
  of 1 cm strips stepping inward at 50, 25, 12 and 5 % opacity. The middle of
  every side is clear. It is drawn with engine cubes and the engine's one-sided
  see-through widget material tinted #00FF8D, one instanced component per
  opacity level (`rules::BoxFrame`, host-tested). It shows only while your head
  is within 1 m of it. The front is the
  side the game's `HG` display pistol lies toward; it is now centred along
  that edge to line up with the vise, once per hub visit. Also hidden while a menu is on the forearm (the menus switch off the
  depth test of the widget material it shares).
- Once the workbench is bought, letting go of a supported weapon inside the
  box (8 cm margin) holds it upright. As seen from the player-facing edge,
  right-hand release points the barrel left; left-hand release points it
  right. The controller's incoming twist cannot reverse that choice. Let go
  elsewhere and the existing holster/storage behavior applies. Spare parts
  lie beside the vise, normally 7 cm in from the edge. Their rotated bounds
  determine spacing, with a 40 cm clear centre for the vise. This leaves
  about 8.6 cm of visible tabletop beside the vise so small detached parts,
  such as the pistol light, are easier to see and grab. A full side wraps
  inward into another row.

### Chosen detached-part layout

`hole-vr/bench-layout.ini` is optional and read at mod startup. It is separate
from body calibration and `hole-vr.ini`. The private editor supplies one
placement per real attachment row; it does not unlock or create unowned parts.
The existing owned/unfitted checks and attachment transactions are unchanged.

The file starts with `version=1`. Each remaining line is
`ATTACHMENT_ROW=x_cm,depth_cm,yaw_degrees`. The origin is the middle of the
player-facing tabletop edge. Positive x is player-right; positive depth is
away from the player. Yaw 0 points the native attachment forward direction
right, 90 away, and 180 left. Positions stay fixed when another attachment
is fitted, a weapon is picked up and parked again, or the release hand changes.
The weapon itself retains the existing hand-dependent barrel direction.

Native part rolls come from the same verified cooked Blueprint transforms as
the editor: bottom rails for HG/AR/SR lights, left rail for SG lights. Only
attachment identities and rotations are compiled, not game geometry. Runtime
mesh bounds determine the actor origin and tabletop height, with the same
0.5 cm edge margin and 2.8 cm back-lip allowance used by the editor.

An invalid file is rejected as a whole. Missing entries and unavailable mesh
bounds retain automatic placement. The loader logs how many fixed entries it
accepted, and each table refresh reports how many were used. The layout is not
applied to ordinary tables outside the purchased workbench.

### Bench vise marker (owner 2026-11-04)

The supplied xammurapi88 vise has been adapted to 512 flat-shaded triangles,
30.4 x 22.71 x 16.65 cm when open, with an 8 cm jaw opening, a shorter
rear slide, and a compact faceted T-handle. Attribution and the supplied
Sketchfab Standard license metadata are in `docs/BENCH-VISE-CREDIT.txt`.

`bench_vise.cpp` draws two non-colliding procedural components owned by the
actual unlocked workbench. The base sits 10 cm in from the centre of the
player-facing edge, 1 mm above the traced tabletop. The handle points out
toward the player. The bounding box's raised rear lip is corrected in the
fallback tabletop height. The marker is visible from across the room; the
green box keeps its existing proximity/menu visibility rules.

Five private material instances across three finishes use the game's weapon-metal shader with
the exact linear BaseColor, Metallic and Roughness values from the HOLE
color bible: MI_GunMetalBlack #44454A, MI_GunMetalSteel #828784, and
MI_CarbonBlack #232426. Shared weapon materials are never edited.

Parking HG, AR or SG seats its lower grip between the jaws. SR seats its
solid fore-end; the sloping stock intersects the lower casting if clamped
at the wrist. `bench_vise_fit.h` profiles name and validate the actual game
meshes and their bounding boxes. The measured contact-band widths are
3.196 / 2.381 / 2.996 / 3.614 cm respectively. Live component transforms
and the existing controller-to-grip offset place each weapon in either
barrel direction. Measurement uses the live actor rotation and controls'
native gun frame together, not a held pose remembered from another tick.
The measured mesh-to-frame rotation is compensated when seating, so the
physical mesh is upright even if the base game tilted it under the actor.
The jaw width and spare-part rows follow that actual seated mesh.
HG's contact is (-0.5, 0, -7.2) cm on its main frame:
the grip sits 2.5 cm higher and toward the rear of the pads so both extended
magazines clear the casting. The 33-round magazine has about 1.1 cm of
table clearance including the 1 mm base lift. The moving jaw and T-handle close over 0.45 seconds to
the measured grip width plus 2 mm clearance. Picking up, switching away,
or returning the weapon opens it again; level changes reset it open.
The base never moves. Reversing midway does not jump. Pausing freezes motion.
The model also includes open and fully closed (6 mm gap) poses and a loop.

The vise does not claim hands. Existing grips and loadout application remain
in `workbench.cpp`; `controls.cpp` passes its existing grip offset and the
releasing hand to parking. The existing pistol carry remains right-handed;
both hands can carry/release long guns. A pistol released in overlapping
hip and bench zones is offered to the bench before stowing. If geometry cannot be
verified, the old parking pose is retained and the jaw stays open.
Component references are checked before use; level changes clear them,
and incomplete creation is removed and retried.

Long guns use the existing rifle-carry decisions. Releasing the right hand
while the left still supports the gun transfers the hold; only releasing
the last hand parks it. Either hand can pick it up, clearing bench ownership
before normal carrying. Late placement keeps the gun at the bench after
body animation. The back holster's grab zone and pose are disabled for the
parked gun; ending the bench session restores ordinary storage, including
when the VR back holster is disabled. The gun must be the selected weapon
for an attachment session, as before. Loose parts start at least 12 cm in
from the front edge for long guns so they clear a low-hanging stock.

All four guns' cosmetic edits use `game/tube_refresh.h`: the verified game's slot,
attachment, spec, muzzle, sight, heat, sound and animation initialization
steps, with the rate-per-shot calculation. They omit `ClearMagazine` and
`Init_MagazineCreate`, preserving round actors, shell order, internal SG/SR magazines and
chamber. For all four weapons, retained magazines/rounds receive the new muzzle component and
updated weapon stats; ammo counts and actor identities are checked afterward.
The magazine and cached projectiles name their pointer `MuzzlePoint`; rounds
name it `Muzzle Point` with a space. Both spellings are reflected and validated.
Unfired rounds' `MetaBullet_LineTrace` entries also receive the new muzzle and
combined weapon/ammo stats using the game's InitializeBullet calculations.
Spent shells' old projectile references are left alone. No ammo actor or
projectile is spawned, destroyed or reordered by this cosmetic refresh.
Missing reflection data, an active reload/bolt/firing action, an unverified
magazine layout, or changes to ammo/magazine slots are refused before saving.
Only the target gun is refreshed; other carried weapons are untouched.
An attachment transaction restores both the old metadata and the saved
list after a failed rebuild or verification. Incomplete restoration is
logged distinctly. Attachment snap proximity is refreshed on the actual
release frame. After any rebuild or rollback, hidden held parts are resolved
against the current installed actors, not old actor pointers waiting for GC.

Moving an installed attachment to another compatible mount now empties its
old mount in the same transaction. Required mounts cannot be left empty;
failure restores both slots. Each acquisition rechecks the current source:
two simultaneous hands cannot take the same installed part, and removing an
earlier loose part does not invalidate the other hand's cached table index.
If a temporary held copy disappears, cancellation restores its hidden original.

Validation: `test_bench_vise` checks native geometry, winding, normals,
actual-bench base support, rotation, material values, attachment clearance,
both jaw endpoints, timing at several frame rates, quick reversals, resets,
and alignment against the existing gun-placement formula.
`test_rifle_carry` covers the original carrying and hand-transfer behavior.
`test_vise_weapons` adds four model fits, custom grip offsets, both barrel
directions, part spacing, last-hand release, pickups, ammo-slot guards,
loaded/empty tube refresh and missing-function preflight cases. Private
Blender references check triangle intersections for all three long guns.
`test_workbench_release` checks the explicit release-hand direction across
bench rotations, incoming gun twists, custom grip offsets, pistol hip/bench
overlap, and replacement part identities. `test_attachment_transaction`
injects failures before and after rebuilding and checks both recovery steps.
`all-weapon-fit-check.json` checks all four mesh profiles, 53 attachment
Blueprints (including magazine variants), every applicable mount, both barrel
directions, and open/intermediate/loaded jaws. No vise intersections or
below-table vertices remain in these static references; game meshes are
not included in public model exports. This does not test game animation.
`test_ammo_refresh` reproduces the actual magazine/round/projectile property
names from the runtime dump, the removable-light release/commit helpers,
malformed-layout refusal and mixed-ammunition projectile stat calculations.
`test_workbench_bughunt` adds 1,440 physical mesh placements across all four
profiles, live/remembered actor-pose differences, mesh tilt, nonuniform width,
both release hands, bench rotations and custom grip offsets. It reconstructs
the placed mesh through the real carrying transform. It also covers atomic
slot moves and rollback, required/ammo-slot guards, simultaneous acquisitions,
stale actor identities, shifted table indices, and left-held target identity.
`test_workbench_rules` retains coverage of the existing attachment release
rules and green frame geometry. The renderer/placement still needs a check
in the running game; offline renders use the actual bench with studio lighting.

### Log lines to check

- `workbench: bench LCBP_WeaponCustom_..._CAT_N in this level at (...), ... cm from the player (N found); facility HF_WPSWC_C_1`
  (before it is bought: `workbench: facility HF_WPSWC_C_1 in this level, no bench in it ...`)
- `workbench: workbench locked|UNLOCKED (facility state N, bench at ...)`
- `workbench: green zone at (...) on the table's edge at the front, where the game's display pistol lies, ...` and `workbench: green zone made (...): 12 edge lines, 96 fading strips, 34 x 14 x 52 cm, see-through green`
- `workbench: right|left-hand release, weapon held upright at (...), barrel left|right from the player-facing edge; it carries ...`
- Bought this session: `workbench: the workbench is bought (facility state 2) but its bench was not found yet; looking again`, then the bench line above.
- `workbench study: LCBP_WeaponCustom_... at (...) yaw ...` and its `DS_Workbench` / `DS_ToolBox` / `Plane` / `HG` / `Ammo` boxes (where the real bench's top is)
- `workbench: pistol laid on <actor> at (...), right side up; it carries ...`
- `workbench: on the table beside the HG001: LIGHT_HG_LIGHT, ...`
- `workbench: left hand took LIGHT_HG_LIGHT off the HG001 (slot SP_LightAndLaser_Down)`
- `workbench: take LIGHT_HG_LIGHT off SP_LightAndLaser_Down: done, checked on the gun. The gun now carries ...; saved: ...`
- `workbench: LIGHT_HG_LIGHT lines up with SP_LightAndLaser_Down`
- `workbench: fit LIGHT_HG_LIGHT in SP_LightAndLaser_Down: done, checked on the gun. ...`
- `workbench: SIGHT_HG_IRONSIGHT back on the gun: the game always needs a part there`
- Failures: `workbench: ... did not happen: <reason>`.

Not yet: magazine and ammo types (they stay on the normal magazine handling),
hand poses for holding a part. HG, AR, SG and SR bench placement and automatic
jaws are implemented; the all-weapon update still needs a headset check.
