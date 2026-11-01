# Step 3: native controls and weapon handling

How tracked hands, comfort options, physical weapon handling, hand poses and
body IK plug into HOLE. Step 2 (the headset thread) owns the OpenXR session and
rendering; this step builds on it through `vr::xr::Listener` (see `src/vr/openxr.h`
once step 2 lands) and never opens its own session.

## What the game gives us (from the class dump)

- **Player** `BP_Player_C : ALS_Base_CharacterBP_C` (Advanced Locomotion System v4).
  Input arrives as legacy action/axis events and lands in callable functions:
  `PlayerInput_UseWeapon(Press Released)`, `PlayerInput_ReloadWeapon`,
  `PlayerInput_Charge`, `PlayerInput_AimMotion`, `PlayerInput_Interact`,
  `PlayerInput_SwitchWeapon`, `PlayerInput_SwitchLight`, `PlayerInput_MeleeAttack`,
  `PlayerInput_Lean`. Movement goes through `AddMovementInput`, turning through the
  controller's rotation. `SetCameraShake(double)` already exists for comfort.
- **Weapon manager** `BPC_WeaponManager_C` (player property `WeaponManager`):
  `GetCurrentWeapon`, `UseCurrentWeapon`, `ChamberAction`, the reload notifies
  `Notify_Reload_*`, and hand IK helpers (`GetHandIK_LocationRotation`,
  `GetChargingHand_LocationRotation`).
- **Guns** `BP_GunBase_C` (`BP_HG_C` handgun, `BP_AR_C` rifle) model every moving
  part as its own component: `GunParts_Bolt(Slide)`, `GunParts_Bolt(Slide)Catch`,
  `GunParts_MagazineCatch`, `GunParts_ChargingHandle`, `GunParts_Trigger`,
  `MagazinePoint`, `ChargingHandPoint`, plus geometry (`Bolt(Slide)_OriginLoc`,
  `Bolt(Slide)_BackLength`, `Bolt(Slide)_StopLength`) and state (`AmmoInChamber`,
  `MagazineObj`, `IsBolt(Slide)Stopped`, `IsJamming`). Reload steps are separate
  functions: `Gun_MagazineDrop`, `Gun_MagazineDetach`, `ReloadMag_TakeNewMag`,
  `ReloadMag_InsertMag_Begin/End`, `Gun_ChargeBolt`, `Gun_ReleaseBolt`.
- **Magazines** `BP_MagazineBase_C` with `AttachPoint_ReloadHand`, `AttachPoint_Mag`
  and per-round state.
- **Body**: `BodyMesh` (skeletal mesh on the ALS skeleton). The engine has
  `PoseableMeshComponent` (`CopyPoseFromSkeletalComponent`, `SetBone*ByName`),
  `FullBodyIK`, `IKRig` and `ControlRig` compiled in.

## Layers

| Layer | Files | Job |
|---|---|---|
| Engine calls | `src/ue/process_event.*`, `src/ue/call.*` | Find `UObject::ProcessEvent`, call any game function by name with named parameters, observe or block calls by patching the ProcessEvent slot of the native `Character`, `Actor` and `ActorComponent` vtables. |
| Game glue | `src/game/game.*`, `src/game/gun.*` | Game-thread tick (after `BP_Player_C::ReceiveTick`), work queue, current gun and its parts. |
| Weapon lab | `src/game/weapon_lab.*` | Flat-screen hotkeys to exercise the game-side half of reloads and racking, and a trace of the game's own reload/charge events. |
| VR input | `src/vr/input/*` (after step 2) | OpenXR action set: grip/aim poses, trigger, grip, sticks, buttons, haptics. Bindings for Index, Touch and simple profiles. |
| Interaction | `src/vr/interact/*` | Pure logic, unit tested on the build machine: slide grab along the gun axis with partial-rack detection, magwell insertion, holsters/pouches, two-hand grip. |
| Comfort | `src/vr/comfort/*` | Smooth/snap turn, head/hand movement direction with fallback, vignette amount, shake toggles. Pure logic, unit tested. |
| Body | `src/vr/body/*` | Hide `BodyMesh` from the camera, drive a `PoseableMeshComponent` copy of its pose with two-bone arm IK and head/spine alignment; legs stay on ALS locomotion. Calibration for height and arm length. |

## Weapon handling design

- **Slide/bolt**: grabbing near `ChargingHandPoint` or the slide locks the hand to
  the slide axis (learned from the game's own slide animation, see the lab log).
  Travel is the hand's movement along that axis divided by `Bolt(Slide)_BackLength`.
  We write the slide part's position every frame while held.
  - Released after at least 90% travel: full rack, the game's `Gun_ChargeBolt`
    ejects and feeds a round.
  - Released earlier: **partial rack / chamber check**. The slide springs back and
    nothing is ejected. Between about 30% and 90% the chambered round is visible.
  - Slide catch: if the game reports the slide locked open, a short pull past the
    catch or the slide-release button calls `Gun_ReleaseBolt`.
- **Magazine**: mag-release button calls `Gun_MagazineDrop` (or the off hand pulls
  it with `Gun_MagazineDetach`). Reaching to the pouch slot with the off hand and
  gripping spawns a magazine in that hand (`ReloadMag_TakeNewMag`); bringing its
  top within a few cm of `MagazinePoint`, roughly aligned, seats it
  (`ReloadMag_InsertMag_Begin/End`).
- **Firing**: trigger value drives `PlayerInput_UseWeapon`; the game's own recoil
  feeds a hand kick instead of camera movement.
- **Hand poses**: per weapon and grip point, blended with finger curl from the
  controller (trigger touch/pull, grip, thumb rest).

## Weapon lab (flat screen, available now)

| Key | Does |
|---|---|
| F5 | Tracing on/off. Logs the game's weapon events and every ammo/slide change. |
| F6 (hold) | Pull the slide back by hand. Let go early = chamber check, at the end = full rack. |
| Ctrl+F6 | Slide catch release. |
| F7 | Magazine release (drops). Ctrl+F7 pulls it out instead. |
| F8 | New magazine and insert. Shift+F8 tries the game's other load function. |

Results go to `hole-vr/hole-vr.log` (lines starting `lab:`).

## Playtest findings (2026-11-01)

- Handgun `BP_HG_C`: slide moves along -X in its parent space, 4.70 cm full travel
  (`Bolt(Slide)_BackLength`), catch stop at 3.60 cm. Writing the slide part's
  relative location works and the game does not fight it while idle.
- Full rack that matches the game's own logic: `Gun_ChargeBolt(IsTriggerReleased=false)`
  at the back of the stroke (chamber empties), then `Gun_ChargeBolt(true)` on release
  (round fed, magazine count drops by one). Afterwards call
  `SetIsBoltCharging(false)`, or the game's off-hand IK stays on the slide.
  `WeaponManager.ChamberAction` does the same but plays the flat-screen hand animation.
- `Gun_MagazineDrop` drops the magazine. `ReloadMag_TakeNewMag` plus
  `ReloadMag_InsertMag_Begin/End` loads a full magazine but leaves it hidden; call the
  magazine's `SetVisibility(true)`.
