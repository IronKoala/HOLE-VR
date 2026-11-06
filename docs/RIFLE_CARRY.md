# Rifle in the left hand, pistol in the right

Since 2026-11-06 either hand can hold either gun (docs/HOLSTERS.md, "Either
hand"). Below, "right hand" means the gun hand and "left hand" the other hand;
`interact::Decide` is unchanged and is fed the two roles. New with it: the
other hand can hold the rifle by its own grip (`by_grip`) while the pistol is
out, and the rifle comes out in that hand when the pistol is put away.

Owner request (2026-11-03): "hold the foregrip of the rifle in one hand and
pistol in the other". The rifle stays in the left hand by its handguard while
the pistol is the gun that fires, like a soldier going from a rifle to a
sidearm.

First headset run with the AR: 2026-11-03 21:01 (owner's local time; build
399a3d0). Carry, pistol draw and fly-back worked. The foregrip didn't feel
right, so it now has three grip points (below), and a relaxed squeeze no longer
drops the rifle. Second run, 2026-11-03 22:02 (build e7b0fa9): the three points
were in the wrong places, and the rifle couldn't be let go with the right hand
alone. Both are changed (below) and not headset-tested yet.

## How it plays

- **From the rifle in both hands.** Left hand on the handguard, right on the
  grip. Let go of the right grip: the left hand keeps the rifle where it is.
  Then reach to the pistol holster and squeeze: the game switches to the
  pistol and it comes out in the right hand. You can also squeeze at the
  pistol holster straight away without letting go first.
- **Letting go of the rifle in the right hand alone** (no left hand on it):
  it flies back to its holster (or the game's spot on the back) and stays
  the game's current weapon, like the pistol going back by itself. Squeeze on
  it there to take it again, or squeeze at the pistol holster to draw the
  pistol. `rifle_right_let_go=0` keeps the old way (it stays in the hand).
- **Pistol out, rifle on the back.** Reach for the rifle's handguard where
  the game hangs it (on the back) and squeeze the left grip: the rifle comes
  into the left hand, lying along the hand (barrel the way the hand points,
  the bore 4 cm over the fist), at the point of the handguard you grabbed.
- **The left hand holds the rifle alone.** The pistol fires as usual. The
  left hand can't take magazines or the slide while it holds the rifle. Let go
  of the left grip (or squeeze again with grip toggle) and the rifle flies
  back to its spot on the back in 0.2 s.
- **Back to the rifle.** Holster the pistol, then squeeze the right grip on
  the rifle's grip (within 14 cm, `rifle_grip_reach_cm`): the game switches to
  the rifle and the left hand stays on the handguard as the support hand. The
  same works for the rifle on the back with the left hand empty.
- **The game's own weapon switch (left Y)** keeps working. Switching away from
  a rifle held in both hands leaves it in the left hand; switching to a rifle
  the left hand holds puts it in both hands.
- While the rifle is out, the pistol sits in its holster (not on the game's
  right-thigh spot), so it is drawn from where it always is.
- The left hand's grab is a support grip on the rifle, so a hand pose made in
  the grip editor for the handguard applies here too (one per grip point,
  below). Without one, the hand stays where the controller is.

## Grip points (vr/interact/grip_points.*)

Owner, 2026-11-04, after the first AR run: "I only want the grip points, one in
front of the magwell, one behind the flashlight and one where the picatinny
rail ends." After the second run (04:17 UTC), the first layout was wrong: "i
want one just behind the flash light one where the picatinny rail ends (the
picatinny that the scope will go on not the forward one) and one just in front
of the mag". A left squeeze on the gun takes the nearest of three points
(along the barrel and in height), both for the two-hand hold and for the rifle
in the left hand alone. Back to front:

| Point | Where (gun frame: X along the barrel, Z up) | Pose file |
| --- | --- | --- |
| in front of the magazine | 3 cm ahead of the front of the magazine in the gun, at the handguard's left face, 8 cm under the handguard (not under the magazine's bottom). Magazine out: where it last was; never seen: 2 cm behind the lower receiver's front | `grips/BP_AR_C.support-mag.ini` |
| where the scope rail ends | on the handguard at the front end of the top rail the scope goes on (`*PicatinnyRail_TopRear*`, or the rearmost of two or more `*Top*` rails); halfway along the handguard if there is none | `grips/BP_AR_C.support-scoperail.ini` (until posed: the light's pose) |
| behind the light | on the handguard, 4.5 cm behind the rear end of a light or laser fitted on it; the handguard's front end when there is none | `grips/BP_AR_C.support-light.ini` |

Where the owner reached in the second run: X 13 to 17 and Z -3 to 5 for the
magazine point (the handguard's bottom is at 8.7), X 32 to 40 and Z 8 to 14 for
the light. The two handguard points sit on its left side (2.5 cm outside it),
level with its middle. A point closer than 4.5 cm to another is dropped (a
short handguard has room for fewer).

The parts are found by component name: `*Handguard*`, `*Rail*`, `*LowerRec*`
(or `*Receiver*` on a shotgun), the game's `MagazineObj` while it is seated,
and an installed part of category 12 (light or laser, from the gun's
`WeaponParts`). Their sizes come from `StaticMesh.GetBoundingBox`, in the gun
frame. They are measured again at most every 2 s on a grab, because parts can
change at the workbench. When no handguard is found, the hand holds wherever
it grabbed, as before.

The drawn hand goes to the point, and each point has its own pose in the grip
editor. The second run's `support-magwell` and `support-railend` poses were
made for the old places, so they are no longer used (the files stay). Until a
point is posed, the older `BP_AR_C.support.ini` pose is used for it. The barrel
is still steered by where the controller grabbed, so taking a point never
turns the gun. The grab zone ends 4 cm past the rail, so a grab on the bare
barrel does nothing.

Letting go: in the first run the two-hand hold ended after 0.3 to 3 s about 30
times ("support hand off the handguard"), and the right hand kept letting go
of the rifle while aiming. The grip button releases under 0.35. A hand that
holds the rifle now lets go only when its squeeze stays under 0.20 for 0.12 s:
the support hand, the left hand holding the rifle alone, and the right hand
on the rifle. In the second run every let-go read 0.00, so an open hand lets
go at once. Tracking loss under 0.25 s doesn't end the two-hand hold. After
the grip editor or a menu, only a squeeze held at that moment counts as
holding, so the editor's own squeezes don't let go of the rifle.

## Charging handle (both hands)

Owner, 2026-11-04 05:26 UTC: "the charging handle? i want to grab points for
it, one for left hand, one for right hand, at the rear of the charging
handle." In the run before (22:54 to 22:56 local) the left hand's grabs
reached 4 to 9 cm behind the old grab point (the handle part's origin), and
the drawn hand kept changing between the pistol's slide holds (rear pinch,
overhand), jumping 8 to 12 cm and turning 66 to 69 degrees.

- **Where.** Both hands grab the T at the back of the upper receiver: the
  rear end of the charging handle's mesh (`*ChargingHandle*` static mesh, box
  in the bolt's parent space), 1 cm in, at its middle. A squeeze within 9 cm
  of it takes it (`rifle_charging_handle_grab_cm`). Without a mesh: the old
  point. The handle moves back with the hand and pulls the bolt as before.
- **Which hand.** The left hand while the right holds the rifle (as before).
  The right hand while the left holds the rifle by itself: let go of the
  grip with the right hand (the left keeps the rifle), then squeeze on the
  charging handle. A right squeeze nearer the rifle's grip than the handle
  takes the grip instead.
- **Poses.** One per hand, no picking by angle: `grips/BP_AR_C.charging-left.ini`
  and `grips/BP_AR_C.charging-right.ini`. Until the left one is posed, the
  slide hold the owner posed on the AR at 22:56 (`BP_AR_C.slide-rear-pinch-behind`)
  is used, moved onto the new grab point so the hand stays where it was posed
  on the gun. Until the right one is posed, the hand stays where it grabbed.
  To pose the right hand: right hand on the handle, hold left Y 1 s; the left
  controller edits. Coming back from the editor with the left hand open, the
  left hand keeps the rifle until it squeezes and lets go again.

## Settings (hole-vr.ini, group `rifle_left_hand_carry`)

| Key | Default | Meaning |
| --- | --- | --- |
| `rifle_left_hand_carry` | 1 | 0 turns all of this off (the old one-gun handling) |
| `rifle_carry_fist_below_bore_cm` | 4 | bore height over the left fist when picked up from the back |
| `rifle_carry_pitch_deg`, `_yaw_deg`, `_roll_deg` | 0 | rifle turned in the left hand when picked up (+ pitch tips the muzzle up) |
| `rifle_grip_reach_cm` | 14 | how close the right hand must squeeze to the rifle's grip to take it |
| `rifle_grip_points` (own group) | 1 | 0: the hand holds wherever it grabs (no grip points) |
| `rifle_grip_point_inset_cm` | 4.5 | how far the middle of the hand sits behind the light |
| `rifle_right_let_go` (own group) | 1 | 0: the rifle stays in the right hand when it lets go alone |
| `rifle_charging_handle_grab_cm` (own group) | 9 | how close either hand must squeeze to the charging handle's rear end |

## Game side

`BPC_WeaponManager_C` (dump 2026-11-03) keeps `Obj_PrimaryWeapon` (rifle) and
`Obj_SecondaryWeapon` (pistol) alive at once, with `Enum_CurrentWeapon` for
the one that fires and `WeaponLocation_Back` / `WeaponLocation_RF` /
`WeaponLocation_BackWaist` for where the others hang. Switching uses the
player's own `InpActEvt_SwitchToPrimary_K2Node_InputActionEvent_7` /
`InpActEvt_SwitchToSecondary_K2Node_InputActionEvent_6` (falls back to
`PlayerInput_ScrollWeapon` if missing). The mod waits up to 2.5 s for the
game to switch; until then the rifle stays where it is.

Switching to the pistol, the weapon manager also calls the rifle's
`SetOffsetTransform(false)` (BP_WeaponBase: `PartsOffset` / `BaseOffset` move
to their put-away place), as it does for the spare pistol. Owner, 2026-11-06
run: with the rifle in the left hand, drawing the pistol made the rifle "move
out of place on its own", and it came back when both hands took it. While the
mod places the rifle (left hand, flying back, resting in its holster) its
parts are kept in the held place (`IsOffset` true, the place its frame was
measured in); handed back to the game's own spot they go back. Log: `rifle
carry: the game moved the rifle's parts to their put-away place ...; put them
back in the held place (the grip point moved N cm) because ...`.

The rifle's spot is learned from where the game hangs it (its root
component's `AttachParent`, `AttachSocketName` and offset); before that it
is `WeaponLocation_Back`. Taking it detaches it; at the end of the fly-back
it is attached there again (`K2_AttachToComponent`, keep world) so the game
owns it again. A current rifle that neither hand holds is kept at the spot
by the mod (the game still thinks it is in the hand; the trigger does
nothing, the right trigger interacts).

## Where things are

| Part | File |
| --- | --- |
| Pure logic: who does what (`Decide`), handguard zone, rifle pose from the hand | `src/vr/interact/rifle_carry.*`, tests `tests/test_rifle_carry.cpp` |
| Game side: the weapon manager's guns, switching, placing the rifle and the spare pistol, the left-hand grab for the body | `src/vr/controls.cpp` (`RefreshCarryGuns`, `RifleCarry`, `PlaceRifle`, `PlaceSparePistol`, `RifleCarryBeforeGunChange`, `RifleCarryOnNewGun`, `PublishGrabs`) |
| Gun frame for any gun (shared with the right-hand gun) | `ComputeGunFrame` in `src/vr/controls.cpp` |

Tests: `g++ -std=c++20 -Isrc tests/test_rifle_carry.cpp src/vr/interact/rifle_carry.cpp -o /tmp/test_rifle_carry && /tmp/test_rifle_carry`

## Log lines to check (hole-vr.log)

- `rifle carry: the game has the rifle BP_AR_C and the pistol BP_HG_C`: the
  weapon manager has both. "no rifle" = not unlocked or not equipped.
- `rifle carry: BP_AR_C measured: barrel along (...), muzzle N cm ahead ...`
- `rifle carry: the game hangs the rifle on <component> (...); its grip is N cm
  below, N cm behind and N cm right/left of the headset`: where to reach.
- `grip points: BP_AR_C (in the right hand | in the left hand alone): points in
  front of the magazine (x y z), where the scope rail ends (x y z), behind the
  light (x y z); parts: handguard X a..b Y .. Z ..; rails: <name> X a..b Z c..d,
  ...; scope rail <name> ends at N; handguard rails end at N; lower receiver X
  a..b Z c..d; magazine <class> X a..b Y .. Z ..; <light class> X a..b Y .. Z ..`:
  what was measured, logged when it changes. "(no scope rail: halfway)", "(no
  light: the handguard's front)", "(no magazine measured)" say which fallback was
  used. `no handguard part found` = the old free grab.
- `controls: left grab at gun-frame (x y z), <point name> | off the handguard`
  (two-hand hold) and `rifle carry: left grab at rifle-frame (x y z), <point
  name> | off the handguard` (rifle not in the right hand).
- `controls: support hand off the handguard after N s (why)`: the squeeze
  (and the lowest it got while held), tracking lost, charging handle, magazine.
- `rifle carry: right hand let go of the rifle (...); it goes back to its holster`:
  the right hand alone opened.
- `controls: charging handle <mesh> X a..b Y .. Z ..; both hands grab it at its
  rear end (x y z), within N cm (...; the old grab point was (x y z))` and
  `charging handle grab point is (x y z) from the old one, gun frame`.
- `controls: right|left grab at slide-space (x y z), inside|outside the charging
  handle zone`, `slide hold #N (right|left hand)`, `charging handle not grabbed:
  the right hand squeezed nearer the rifle's grip`.
- `body: right|left hand on the charging handle: pose BP_AR_C.charging-right|left
  (saved) | not posed yet`, and `body: BP_AR_C.charging-left not posed yet: using
  BP_AR_C.slide-rear-pinch-behind ...` for the carried-over pose.
- `rifle carry: left hand picked up the rifle ...`, `right hand let go ...`,
  `drawing the pistol ...`, `left hand let go ...`, `right hand took the rifle's grip`
- `rifle carry: asked the game to switch to the pistol`, then `the game switched
  to the pistol N s after we asked` (or `did not switch within 2.5 s`).
- `rifle carry: rifle back on <component>` (or `attach FAILED`).

## Open

- The charging handle points (both hands) are untested in the headset.
- The second layout of grip points and the right-hand let-go are untested in
  the headset. The scope rail and magazine boxes have never been logged yet:
  check the `grip points:` line's `rails:` and `magazine` parts.
- The rifle angle in the left hand when picked up from the back is a guess
  (barrel along the controller's pointing direction); the `rifle_carry_*_deg`
  keys tune it.
- With a posed `BP_AR_C.support` grip, the drawn hand follows the pose on the
  rifle, which can sit a little off the real controller while the left hand
  holds the rifle alone.
- The rifle now rests in its own holster behind the right shoulder
  (`holster_rifle=1`, docs/HOLSTERS.md) instead of the game's spot.
