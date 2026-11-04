# Rifle in the left hand, pistol in the right

Owner request (2026-11-03): "hold the foregrip of the rifle in one hand and
pistol in the other". The rifle stays in the left hand by its handguard while
the pistol is the gun that fires, like a soldier going from a rifle to a
sidearm.

First headset run with the AR: 2026-11-03 21:01 (owner's local time; build
399a3d0). Carry, pistol draw and fly-back worked. The foregrip didn't feel
right, so it now has three grip points (below), and a relaxed squeeze no longer
drops the rifle. Those two changes are not headset-tested yet.

## How it plays

- **From the rifle in both hands.** Left hand on the handguard, right on the
  grip. Let go of the right grip: the left hand keeps the rifle where it is.
  Then reach to the pistol holster and squeeze: the game switches to the
  pistol and it comes out in the right hand. You can also squeeze at the
  pistol holster straight away without letting go first (rifles stay in the
  right hand whether or not the grip is held, so this is the way when you
  were not holding the right grip).
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
rail ends." A left squeeze on the handguard now takes the nearest of three
points along the barrel, both for the two-hand hold and for the rifle in the
left hand alone:

| Point | Where (gun frame, X along the barrel) | Pose file |
| --- | --- | --- |
| in front of the magwell | 4.5 cm ahead of the handguard's back end or the lower receiver's front, whichever is further forward | `grips/BP_AR_C.support-magwell.ini` |
| behind the light | 4.5 cm behind the rear end of a light or laser fitted on the handguard; halfway between the other two when there is none, or no room behind it | `grips/BP_AR_C.support-light.ini` |
| at the end of the rail | 4.5 cm behind the front end of the picatinny rails on the handguard (the handguard's own front end if no rail is found) | `grips/BP_AR_C.support-railend.ini` |

Every point sits on the handguard's left side (2.5 cm outside it), level with
its middle. That is where the owner's left controller was in the first run,
about 5 cm left of the bore. The parts are found by component name
(`*Handguard*`, `*Rail*` on the handguard, `*LowerRec*`, or `*Receiver*` on a
shotgun) and an installed part of category 12 (light or laser, from the gun's
`WeaponParts`). Their sizes come from `StaticMesh.GetBoundingBox`, in the gun
frame. They are measured again at most every 2 s on a grab, because parts can
change at the workbench. When no handguard is found, the hand holds wherever
it grabbed, as before.

The drawn hand goes to the point, and each point has its own pose in the grip
editor. Until a point is posed, the older `BP_AR_C.support.ini` pose is used
for it. The barrel is still steered by where the controller grabbed, so taking
a point never turns the gun. The grab zone ends 4 cm past the rail, so a grab
on the bare barrel does nothing.

Letting go: in the first run the two-hand hold ended after 0.3 to 3 s about 30
times ("support hand off the handguard"), and the right hand kept letting go
of the rifle while aiming. The grip button releases under 0.35. A hand that
holds the rifle now lets go only when its squeeze stays under 0.20 for 0.12 s:
the support hand, the left hand holding the rifle alone, and the right hand
on a rifle the left hand also holds. Tracking loss under 0.25 s doesn't end
the two-hand hold.

## Settings (hole-vr.ini, group `rifle_left_hand_carry`)

| Key | Default | Meaning |
| --- | --- | --- |
| `rifle_left_hand_carry` | 1 | 0 turns all of this off (the old one-gun handling) |
| `rifle_carry_fist_below_bore_cm` | 4 | bore height over the left fist when picked up from the back |
| `rifle_carry_pitch_deg`, `_yaw_deg`, `_roll_deg` | 0 | rifle turned in the left hand when picked up (+ pitch tips the muzzle up) |
| `rifle_grip_reach_cm` | 14 | how close the right hand must squeeze to the rifle's grip to take it |
| `rifle_grip_points` (own group) | 1 | 0: the hand holds wherever it grabs (no grip points) |
| `rifle_grip_point_inset_cm` | 4.5 | how far the middle of the hand sits from the magwell, the light and the rail end |

## Game side

`BPC_WeaponManager_C` (dump 2026-11-03) keeps `Obj_PrimaryWeapon` (rifle) and
`Obj_SecondaryWeapon` (pistol) alive at once, with `Enum_CurrentWeapon` for
the one that fires and `WeaponLocation_Back` / `WeaponLocation_RF` /
`WeaponLocation_BackWaist` for where the others hang. Switching uses the
player's own `InpActEvt_SwitchToPrimary_K2Node_InputActionEvent_7` /
`InpActEvt_SwitchToSecondary_K2Node_InputActionEvent_6` (falls back to
`PlayerInput_ScrollWeapon` if missing). The mod waits up to 2.5 s for the
game to switch; until then the rifle stays where it is.

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
  front of the magwell (x y z), behind the light (x y z), at the end of the rail
  (x y z); parts: handguard X a..b Y .. Z ..; rails end at N; magwell (lower
  receiver) front at N; <light class> X a..b`: what was measured, logged when it
  changes. `no handguard part found` = the old free grab.
- `controls: left grab at gun-frame (x y z), <point name> | off the handguard`
  (two-hand hold) and `rifle carry: left grab at rifle-frame (x y z), <point
  name> | off the handguard` (rifle not in the right hand).
- `controls: support hand off the handguard after N s (why)`: the squeeze
  (and the lowest it got while held), tracking lost, charging handle, magazine.
- `rifle carry: left hand picked up the rifle ...`, `right hand let go ...`,
  `drawing the pistol ...`, `left hand let go ...`, `right hand took the rifle's grip`
- `rifle carry: asked the game to switch to the pistol`, then `the game switched
  to the pistol N s after we asked` (or `did not switch within 2.5 s`).
- `rifle carry: rifle back on <component>` (or `attach FAILED`).

## Open

- Grip points and the softer let-go are untested in the headset. Check the
  `grip points:` line against where the three points should be.
- The rifle angle in the left hand when picked up from the back is a guess
  (barrel along the controller's pointing direction); the `rifle_carry_*_deg`
  keys tune it.
- With a posed `BP_AR_C.support` grip, the drawn hand follows the pose on the
  rifle, which can sit a little off the real controller while the left hand
  holds the rifle alone.
- The rifle now rests in its own holster behind the right shoulder
  (`holster_rifle=1`, docs/HOLSTERS.md) instead of the game's spot.
