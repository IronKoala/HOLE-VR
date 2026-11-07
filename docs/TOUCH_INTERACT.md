# Touch interact: loot, boxes, fridges and doors by hand

Owner, 2026-11-03 15:23: "opening up cardboard boxes is unreliable ... to pick
up loot and open loot crates (fridge and box for example) i'd like to just
place my hand on the item, to open doors i'd like the ability to just place my
hand on the door handle". Project rule: no body-motion gestures. Resting a
hand on the thing is the whole action.

Code: `src/vr/interact/touch.cpp` (game glue), `src/vr/interact/touch_math.cpp`
(maths, unit tested by `tests/test_touch.cpp`). The trigger interact
(`HandInteract` in `src/vr/controls.cpp`) uses the same module.

## Why box opening was unreliable (run 14:54, build 48bd9e5)

The trigger interact let the game's own target win. That is the thing the
head looks at (BP_Player `UpdateInteractLineTrace`, a ray from the camera).
The hand only won when it was within about 8 cm of a thing's bounds, and
pointing with the hand never won. At 09:03:31-09:03:42 local time the owner
pressed the trigger 20 times at `BP_DataBox_500_C_2147478824`. Every press
went to `TItem0004_HDD_C_2147478881`, which the owner was looking at, until
the hand finally came within range of the box.

## What it does now

- **Touch** (no button): rest an empty hand on a thing. Loot (`BP_ItemBase_C`
  and its children) is taken. Containers (`BP_ItemBoxBase_C`, `LCBP_Fridge_C`)
  open. Doors (`BP_DoorBase_C`) open or close when the hand rests on the
  handle.
  - The hand must come from outside and rest for 0.08 s (doors 0.2 s), so a
    hand brushing past does nothing. It must leave before it can touch the
    same thing again. Only a hand seen away counts as leaving: lost tracking,
    a menu or pause, or the hand holding something while it is on the thing
    does not, and a rest cut short that way starts again (outside review
    finding 6; `TestUnknown` in `tests/test_touch.cpp`).
  - An item that pops out of a box into the hand is not taken until the hand
    has left it once.
  - When the hand is on several things, the smallest wins, so an item lying on
    a box is taken and the box is not opened.
  - The same thing is never used twice within 1 s, whether by touch or by
    trigger.
  - A hand holding the gun, a magazine, the slide, a workbench part or a
    drink can is never used for touch. Neither is the grip editor's hand.
- **Trigger** with an empty hand, in this order:
  1. what the hand touches or nearly touches (within
     `touch_interact_trigger_near_cm`, 8 cm), smallest first;
  2. what the hand points at exactly, found with the game's own interact
     trace (channel 2, hit part object type 16) from the hand instead of the
     eyes;
  3. what the hand points near: the thing whose shapes come closest to the
     aim, within `touch_interact_point_cone_deg` (8 degrees) or
     `touch_interact_point_min_cm` (6 cm) of it, as far as the game reaches
     when you look (`InteractiveDistance`, 200 cm). A wall, shelf or door in
     between (the Visibility trace) rules a thing out;
  4. where you look (the game's own target).

  The ray for 2 and 3 is the controller's aim pose turned
  `touch_interact_point_pitch_deg` (-30) down and `touch_interact_point_yaw_deg`
  (5, mirrored for the left hand) out: the aim pose points about 30 degrees
  above where a hand naturally points, the same offset the held pistol uses
  (`kBasePitch`/`kBaseYaw` in `vr/controls.cpp`). Owner 2026-11-06: "the angle
  isnt in line with my hand, i think its pointing more towards the sky"; the
  log agreed (the raw aim was level at a box on the floor 1.4 m away).

  A press buzzes only when it used something, and only a press that used
  something holds up the next one (0.5 s).

  Why the cone (run 2026-11-03 20:44, build 399a3d0): ten presses at two
  cardboard boxes from about 1 m away found nothing. The game's interact part
  on a `BP_DataBox_500_C` is a 64 cm sphere (`InteractArea`) centred on the
  box's base, half of it under the floor, so only an exact hit on that dome
  counted, and the game's own look target was empty at those moments too.
- Every interact goes through the game's own steps: the thing's
  `InteractInOverlay`, the player's `InteractTarget` and `IsInteractReady`,
  then `PlayerInput_Interact`. That is the same as looking at the thing and
  pressing the interact key, so the game decides what happens.
- Left alone: the hub drink machine (`LCBP_SkillUpgrade*`, owned by
  `vr/drinks`) and the range timer (`BP_ShootingTimer*`, a physical button
  in `vr/controls`). Pointing at them with the trigger still works.

## Shapes a hand touches

- Loot and containers: their visible static meshes (the 6 biggest; loot up
  to 80 cm, containers up to 300 cm). Meshes hidden in game are skipped
  (`LCBP_Fridge_C` carries several looks and shows one). The fallback is the
  parts the game's interact ray hits, then the actor's bounds.
- Doors: a zone around the handle on the door leaf (`office_doorB`), in the
  leaf's own frame so it swings with the door. Height is
  `door_handle_height_cm` (100) above the leaf's bottom, ±20 cm. Width is
  `door_handle_from_edge_cm` (22) in from the edge away from the hinge
  (`DoorAxis`), plus 4 cm past it. Depth is 12 cm out from both faces. If the
  door's `InteractBox` is handle-sized (≤ 45 cm), that box is used instead.
- The hand: the drawn fingertips and palm (`body::GetHandContacts`, stopped at
  walls), plus the real controller's fist. The fist goes on through a surface
  the drawn hand rests on, so pressing on a thing always counts.

## Log lines

- `touch: N things to interact with here: a loot, b containers, c doors, ...`:
  the level's list, every 1.5 s, logged when the counts change.
- `touch: left hand touching <actor> (<class>, <kind>, closed) through the
  game's interact`: a touch fired. Trigger presses say `left hand trigger,
  touching|pointing at|looking at`.
- `touch: result of ... : it opened (IsOpen 0 -> 1)` / `it is gone (taken)` /
  `it opened and the game removed it` (a box destroys itself 1 s after its
  loot is out) / `no change the mod can see`: read 1 s later. Use this to
  check each touch really did something. Before build 2 (399a3d0 and older)
  the "before" state was read after the game's Interact had run, so every
  line said "already open": ignore those.
- `touch: left hand trigger: pointing 3.2 deg off <actor> ...` / `... is in
  the way` / `the left hand touches and points at nothing ... aim from (...)
  along (...)`: how each trigger press picked (or didn't).
- `touch: loot: <item> appeared ...`, `... after 2.5 s is ... cm from you,
  ... above your feet; moved ...; n of m meshes shown; solid above it, floor
  below it` and `... is gone: taken by your hand | not by the mod's touch or
  trigger (walked into, timed out, or fell out of the level)`: where each
  item that appears during a level goes (first 80 lines a level). Owner
  2026-11-04: "no loot spawned". The game did spawn items from every box and
  fridge in that run, but almost none was picked up. Turn off with
  `touch_interact_loot_log=0`.
- `touch: study: <class> (...)`: once a run per class, its components (object
  type, collision, size, place) and the shapes touch uses. The class's
  bytecode goes to `dump/script/<class>.txt` (`touch: study: the game's code
  for ...`). Turn this off with `touch_interact_study=0`.
- perf lines: `touch interact tick`, `touch interact: list things`,
  `touch interact: measure a thing`.

## Settings (hole-vr.ini)

`touch_interact`, `touch_interact_loot`, `touch_interact_containers`,
`touch_interact_doors` (1 = on); `touch_interact_reach_cm` (1.5);
`touch_interact_hold_s` (0.08); `touch_interact_door_hold_s` (0.2);
`door_handle_height_cm` (100); `door_handle_from_edge_cm` (22);
`touch_interact_extra_containers` and `touch_interact_skip` (class-name
pieces, comma separated); `touch_interact_study` (1);
`touch_interact_trigger_near_cm` (8); `touch_interact_point_cone_deg` (8);
`touch_interact_point_min_cm` (6); `touch_interact_loot_log` (1);
`touch_interact_point_pitch_deg` (-30); `touch_interact_point_yaw_deg` (5).

## What the game does (bytecode, dump/script)

- `BP_ItemBoxBase_C.Interact` calls `OpenBox`: if not `IsOpen`, it sets it,
  plays the open animation (the cardboard box's flaps), then spawns each item
  in `ItemsInBox` at the box's base one by one (0.066 s apart) with
  `IsItemJump` (they jump up 4-7 m/s and out up to 2 m/s) and
  `IsToutchWithBody`, and destroys the box 1 s after the last one.
- `BP_ItemBase_C`: `Interact` takes the item (`TakeItem`); with
  `IsToutchWithBody`, the player's body walking into its `InteractArea`
  (64 cm sphere) takes it too. Health packs (`OItem_HP25_C`, once items) are
  only used when HP is below max and above 0, so at full health touching one
  does nothing.

## Not known yet (first build; the first two are answered above)

- Whether "cardboard boxes" in levels are `BP_ItemBoxBase_C`, `LCBP_Fridge_C`
  or something else. `BP_DataBox_500_C` and actors named `StaticMeshActor_N`
  were interacted with in the 14:54 run. Their classes are unknown: level
  designers often replace a placed mesh with a Blueprint and keep its name.
  The study lines answer this.
- What a box's `Interact` does with `SelectIndex` (its `ItemNameList` /
  `TargetItemID`): the bytecode dump answers this.
- Whether the door's `InteractBox` sits at the handle: the door study line
  compares it with the computed zone.
