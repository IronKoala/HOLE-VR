# Rifle in the left hand, pistol in the right

Owner request (2026-11-03): "hold the foregrip of the rifle in one hand and
pistol in the other". The rifle stays in the left hand by its handguard while
the pistol is the gun that fires, like a soldier going from a rifle to a
sidearm.

Not headset-tested yet: the owner had not unlocked the rifle when this was
built. Everything rifle-side below is untested until then; pistol-only play
does not change.

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
- The left hand's grab is a support grip on the rifle (`BP_AR_C.support`), so
  a hand pose made in the grip editor for the handguard applies here too.
  Without one, the hand stays where the controller is.

## Settings (hole-vr.ini, group `rifle_left_hand_carry`)

| Key | Default | Meaning |
| --- | --- | --- |
| `rifle_left_hand_carry` | 1 | 0 turns all of this off (the old one-gun handling) |
| `rifle_carry_fist_below_bore_cm` | 4 | bore height over the left fist when picked up from the back |
| `rifle_carry_pitch_deg`, `_yaw_deg`, `_roll_deg` | 0 | rifle turned in the left hand when picked up (+ pitch tips the muzzle up) |
| `rifle_grip_reach_cm` | 14 | how close the right hand must squeeze to the rifle's grip to take it |

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
- `rifle carry: left grab at rifle-frame (x y z), inside/outside the handguard zone`
- `rifle carry: left hand picked up the rifle ...`, `right hand let go ...`,
  `drawing the pistol ...`, `left hand let go ...`, `right hand took the rifle's grip`
- `rifle carry: asked the game to switch to the pistol`, then `the game switched
  to the pistol N s after we asked` (or `did not switch within 2.5 s`).
- `rifle carry: rifle back on <component>` (or `attach FAILED`).

## Open

- Untested in the headset (rifle not unlocked).
- The rifle angle in the left hand when picked up from the back is a guess
  (barrel along the controller's pointing direction); the `rifle_carry_*_deg`
  keys tune it.
- With a posed `BP_AR_C.support` grip, the drawn hand follows the pose on the
  rifle, which can sit a little off the real controller while the left hand
  holds the rifle alone.
- No rifle holster of our own yet: the rifle goes back to the game's spot.
