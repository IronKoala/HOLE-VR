# Drink machine by hand (hub)

Owner, 2026-11-03: "for drinks i dont want to use the "interact" key, i want to
push the button with my finger, (so for example the health drink i just
unlocked i want to push the yellow square underneath it) to drink it i want to
physical grab the drink > open the tab > pour the drink down my mouth."

Code: `src/vr/drinks.cpp`. Settings: the `drinks_*` keys in hole-vr.ini.

## The game's machine

From the class and object dumps (no bytecode yet; this build adds
`LCBP_SkillUpgrade` to `dump_script_classes`, so the next run writes
`dump/script/LCBP_SkillUpgrade*.txt`):

- `LCBP_SkillUpgradeNew_C` in `Area000_Home` (actor `LCBP_SkillUpgradeNew_C_2`):
  the machine. Meshes `Home_VendingMachine_Button01..06` (the yellow squares),
  `SampleCan01..06` (the cans on show), `Can` (the can that comes out),
  `CanLocation`, timelines `TL_BuyAnimation`, `TL_SlotMove`, flags
  `IsEmptySlot`, `IsOpenSlot`, `SelectedCanMesh`; events `BuyCan(SkillRowName,
  BuySkillLv)`, `DrinkCan()`, `UnlockShopItem`.
- `LCBP_SkillUpgradeSwitchNew_C` x6 (child actors `LCBP_SkillUpgradeSwitch1..6`):
  the buttons. `SkillRowName`, `IsUnlocked`, `IsAvailable`, `IsMaxLv`,
  `CurrentSkillLv`, `BuySkillLv`, `UpgradeCost`, `SafeFlag`; `Interact`.
- `LCBP_SkillUpgradeSlotNew_C` (child actor `LCBP_SkillUpgradeSlot`): the can
  slot, with a `Box`, a `Spline` and `Interact`.
- Sounds: `S_VendingMachine_Switch_Push(_Success/_Fail)`, `_Make`, `_Make_End`,
  `_Door_Open/_Close`, `_Drink`, `_SkillGet`.

The owner's 09:18 run (logs/run-1524) bought and drank with the trigger
interact: a switch, then the slot 3.7 s later. So the game buys on a switch's
interact and drinks on the slot's interact.

## What the mod does

Everything the game does goes through its own interact, the way the interact
key does it: the thing's `InteractInOverlay`, the player's `InteractTarget` and
`IsInteractReady`, then `PlayerInput_Interact`.

1. **Buy**: a fingertip of the drawn hand (`body::GetHandContacts`) touching a
   yellow square presses that button (within `drinks_press_gap_cm` of its face,
   inside its outline, coming from outside, 1 s apart). The square sinks
   `drinks_button_sink_cm` while touched. Each switch is matched to the nearest
   `ButtonNN` mesh at level start.
2. **Take**: when the game's `Can` is visible and at rest, a grip within
   `drinks_grab_cm` of its middle takes it. The hand gets a copy of the same can
   mesh; the game's can stays where it is, hidden (`SetHiddenInGame`), so the
   game still thinks a can is waiting. The can lies through the fist like the
   controller's handle (grip pose +X, little finger to thumb), top at the thumb.
3. **Open**: a fingertip of the other hand within `drinks_tab_reach_cm` of the
   tab opens it (the tab is a small engine shape on the can's top; it flips up
   and a dark opening shows).
4. **Drink**: the open can's top within `drinks_mouth_radius_cm` of the mouth
   (from the eyes: `drinks_mouth_forward_cm` ahead, `drinks_mouth_down_cm` down),
   tipped at least `drinks_pour_tilt_deg` from upright, for
   `drinks_pour_seconds` in total: the game's slot interact runs, which is how
   the game drinks it (effect, sounds, save). Haptic gulps while pouring.
5. **Let go**: before drinking, the can goes back in the slot (the game's can
   shows again). After drinking, the empty can drops with physics and goes
   away after `drinks_empty_can_s`.

Controls leave a hand alone while it holds a can or is at one
(`drinks::ClaimsGrip`, beside `workbench::ClaimsGrip`), and the trigger
interact does nothing with that hand (a fist round the can may pull the
trigger, and an interact would drink it the flat way).

## Log lines to check

- `drinks: found the drink machine` and `drinks: drink machine ... slot ..., can mesh ...`
- `drinks: button N = Home_VendingMachine_ButtonNN (<drink row>: unlocked/LOCKED ...)`: one per button, with size and facing.
- `drinks: <hand> hand's index finger pressed button N (...), through the game's interact`
- `drinks: <hand> hand's ... is X cm in front of button N's face (presses within Y)`: a finger that stopped short.
- `drinks: machine: slot ..., the game's can VISIBLE at ...` and `the game's can is at rest at ...`
- `drinks: <hand> hand took the can (...)`, `opened the tab`, `started pouring`, `DRANK the can ... slot interact ran`
- `drinks: can at the mouth? top X cm from it (needs N), tipped D deg (needs N)`: for tuning the mouth.
- `drinks: after the drink the game's can is VISIBLE/hidden`: what the game did with its own can.

## Not done yet

- Hand pose around the can: the hand closes as for an empty grip.
- The game's bytecode for the machine (next run's dump) will say what
  `Interact` on the switch and slot really do; the flow above may change with it.
