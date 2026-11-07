# Holsters on the VR body

Owner request (2026-11-02): a holster for the pistol on the right hip and one
for magazines on the left hip. A third and fourth slot (rifle, rifle
magazines) come once the rifle is unlocked.

## How it plays

- **Pistol, right hip.** The pistol starts in its holster: muzzle down, grip
  up and back, tilted 10 degrees forward (`holster_pistol_cant_deg`), right
  side facing out. Reach to the grip and squeeze the right grip to draw. Keep
  squeezing to hold it. Let go at the hip and it is holstered (firm click);
  let go anywhere else and it flies back to the holster by itself in 0.2 s.
  `holster_pistol_grip_toggle=1` changes this to squeeze to draw and squeeze
  at the hip to holster.
- While holstered, the trigger, mag release (A), slide release (B), slide
  grabs and magazine seating do nothing. The right hand is free
  (`GrabKind::kNone`), so the body shows an open hand.
- **Magazines, left hip.** This is the old magazine pouch, moved from the
  left front of the belt to the left hip. It still always holds a magazine
  (refilled 0.5 s after one is taken; seating still swaps in the game's own
  magazine, see `MagazineHandling`). New: let go of a magazine at the pouch
  and it goes back in instead of dropping. The magazine stands upright with
  the rounds facing forward, as it sits in a level gun facing the body's front.
- A light haptic tick when a free hand reaches a holster that has something in
  it.
- **Moving the holsters anywhere (owner, 2026-11-02: "if i want to put them
  on my chest let me, my arm pit let me").** Hold the pistol (right hand)
  and/or a magazine (left hand) wherever you want its holster, at the angle you
  want it to hang, and hold the right stick click for 2 s (strong buzz in that
  hand). The holster then sits exactly there and the item hangs exactly as it
  was held. Each holster rides on the nearest body part: belt (pelvis, turns
  with the body only), chest (top of the spine, tilts with the torso, for chest
  rigs, armpits, shoulders, back) or a thigh (middle of the thigh, swings with
  the leg, for drop-leg holsters). Saved in `hole-vr/holsters.ini` (that file
  wins over the ini values; delete it to go back). Only the items in your hands
  move, so holster the pistol first to move just the pouch. A short tap on the
  right stick click is still objective markers (it fires on release). Needs the
  VR body on; rifles keep the click for the fire selector.
- Grip editor: while it borrows a controller (`hands::EditorHand() >= 0`) or a
  menu is open, the holster never changes, so the gun stays in the hand.

## Where things are

| Part | File |
| --- | --- |
| Pure logic: spots, holster frames, draw / put away / fly back, hover zone | `src/vr/interact/holster.*`, tests `tests/test_holster.cpp` |
| Belt, chest and thigh anchors on the posed VR body | `body::GetAnchor` in `src/vr/body/body.*` |
| Game side: placing the gun and pouch magazine, grips, calibration | `src/vr/controls.cpp` (`UpdatePistolHolster`, `PlaceGunInHolster`, `PlacePouchMag`, `HolsterCalibration`, `OnLateTick`) |

A spot is where the hand grabs, as an offset in its anchor's frame, plus
(once placed in the headset) the item's rotation in that frame. Anchors
(`interact::HolsterAnchor`, same order as `body::Anchor`):

| Anchor | Bone | Frame |
| --- | --- | --- |
| belt | pelvis | body yaw only (X forward, Y right, Z up): leaning does not tip it |
| chest | top spine bone | Z from the pelvis up to it, X toward the body's front: tilts with the torso |
| left_thigh, right_thigh | middle of hip and knee | Z up along the thigh, X toward the front: swings with the leg |

Placing picks the nearest anchor to the hand (`interact::NearestAnchor`, belt
wins ties). The controls tick runs before the body, so holstered items are
placed by a second, late tick registered after `body::Init()`
(`controls::InitLate`); otherwise they would trail the body by a frame while
walking. With the VR body off (F9) the belt is taken
`holster_belt_below_head_cm` (60) below and 8 cm behind the headset, the chest
40 cm above it and the thighs 22 cm below it, all turned with the head's yaw.

`holsters.ini` lines: `pistol=chest 4.9 12.0 -3.0 rot qx qy qz qw` (anchor,
offset in cm, item rotation in the anchor frame as a quaternion). The older
`pistol=4.9 32.3 18.2` (belt, built-in hanging) still loads.

The pistol in the holster is placed exactly as `HoldGun` would place it for a
hand at the spot (same `GunGripOffset`), with the gun frame it had when the
holster was placed (or `PistolHolsterFrame` before that), so the spot is where
the controller grip is when holding the holstered gun. The pouch magazine sits
at the spot with the rotation the held magazine had when the pouch was placed
(or upright, `MagPouchRotation`, before that).

Settings (hole-vr.ini): `holster_pistol`, `holster_pistol_grip_toggle`,
`holster_pistol_cant_deg`, `holster_<pistol|mag>_<forward|right|up|radius>_cm`,
`holster_belt_below_head_cm`. The old `mag_pouch_*` keys are no longer used.

## Log lines to check after a playtest

- `holster: body parts from the VR body: belt N cm below the headset, chest N, thighs N and N`
  (belt roughly 65-75 cm standing), then `holster: pistol spot on your ...`
  and `holster: magazine spot on your ...`.
- `holster: pistol drawn (hand X cm from the holster spot)`,
  `holster: pistol holstered ...`, `holster: pistol let go N cm from the holster ...`.
- `controls: took the pouch magazine (hand X cm from the pouch)`,
  `controls: magazine put back in the pouch`.
- `holster: pouch magazine turned ...` and
  `controls: learned magazine seat (...), turned N deg from the magwell point`:
  if N is large, the pouch magazine looks turned until the first insert.
- `holster: moved the pistol holster to your chest, (...) cm, N cm from its centre, at the angle you held it`
  after placing one, then `holster: saved to holsters.ini`.

## Rifle holster (owner, 2026-11-04: "place it behind my right shoulder")

The game hangs the rifle on `WeaponLocation_Back`, which with the VR body put
its grip about 42 cm behind the headset; the owner's log shows about 20 right
squeezes 27-58 cm from it before one took it. `HolsterId::kRifle` is now a
real holster:

- Default: on the chest anchor, (-14, 14, 6) cm (behind the right shoulder),
  zone 18 cm. Hanging (`interact::RifleBackFrame`, 30 degrees): grip up at the
  shoulder, muzzle down the back toward the left, receiver top against the
  back so the grip and magazine stick out behind you.
- Taking it: squeeze the right grip anywhere in the zone (rifle carry's
  `right_on_rifle_grip`, as well as the old 14 cm from the grip). The left hand
  can still take it by the handguard. A light buzz when the free right hand
  reaches the zone.
- Putting it away: everything that used to send the rifle back to the game's
  spot (left hand lets go, drawing the pistol) now flies it to the holster.
  With the holster on the rifle stays detached from the game's spot; the late
  tick places it after the body pose (`PlaceRifleInHolster`), whether or not it
  is the current gun. `RifleSpotPose` returns `RifleHolsterPose`; `PlaceRifle`
  no longer calls `ReattachRifle` while the holster is on.
- Moving it: VR tab > Move holsters, then during the 5 s countdown hold the
  rifle in the right hand where you want it, at the angle you want.
- `holster_rifle=0` goes back to the game's own spot. Settings
  `holster_rifle_forward_cm`, `_right_cm`, `_up_cm`, `_radius_cm`.
- Log: `holster: rifle resting in its holster on your chest, grab spot N cm
  from the headset`, `rifle carry: right grab ... inside the rifle holster zone`,
  `holster: moved the rifle holster to ...`.

Still to come: `kRifleMag` (rifle magazine slot). The pouch holds the current
gun's magazines and is moved only with the pistol out.

## Either hand (owner 2026-11-06)

"i want to be able to grab either gun (and mag) with either hand whenever."
`either_hand_guns=1` (default; 0 = right hand holds the gun, left takes the
magazines, as before).

- **Two roles instead of two hands.** The gun hand holds the current gun by
  its grip: its trigger fires, its A / X drops the magazine, its B / Y
  releases the slide (the left Y tap no longer switches weapons while the left
  hand holds the gun). The other hand racks, reloads, holds the handguard,
  works the light (its trigger next to the gun) or holds the rifle by itself.
  `GunHand()` / `OtherHand()` in `controls.cpp`; the weapon code reads the
  same whichever hand has the gun.
- **Who is the gun hand** (`ChooseGunHand`): only a fresh squeeze of the other
  hand, free of anything else, that takes a gun changes it: the pistol from its
  holster (or the workbench) while no hand holds a gun, the rifle by its grip
  or anywhere in its holster zone off the handguard while the gun hand holds no
  gun, or the pistol from its holster while the gun hand holds the rifle alone.
  The left hand holds a gun mirrored (grip offset and the 5 degree yaw
  flipped).
- **Last gun drawn fires.** The game has one current weapon. With the rifle
  in one hand alone, squeezing the other hand at the pistol holster leaves the
  rifle in the first hand by its grip and draws the pistol in the second
  (`rifle carry: the left hand draws the pistol; the right hand keeps the rifle
  by its grip`). Holding the pistol, the free hand can take the rifle by its
  grip too (`rifle carry: the right hand took the rifle by its grip while ...`).
  Put the pistol away (holster it or let go) and the game switches to the
  rifle, which comes out in the hand that holds it.
- **Magazines.** Any free hand takes one from the pouch (the nearer if both
  squeeze); only the hand that is not holding the gun seats it. Haptic tick
  per hand at the pouch.
- **Holster lid and hand snap** follow the gun hand while it holds the
  pistol, else the nearer free hand. The rifle holster buzzes for either
  free hand.
- **Moving holsters** works with either hand: the gun in the gun hand, a
  magazine in whichever hand holds it.
- **Hand poses.** A hand holding what the other hand usually holds (the gun's
  grip: right; slide, magazine, handguard: left) uses the usual hand's saved
  pose mirrored, and the grip editor saves its own pose under the same name
  plus `-left` / `-right` (e.g. `grips/BP_HG_C.grip-left.ini`), so the
  right-hand grips are never overwritten. `grip_record::SideKey` / `UsualKey`,
  tested in `tests/test_grip.cpp`.
- **The left hand reaching for the holstered pistol** (owner 2026-11-07: "the
  current hand pose for when the left hand is near is just a re-use of the
  right hands pose and it causes the left arm to contort"). The pistol sits
  in its holster turned for the right hand, so the right hand's grip mirrored
  onto it twisted the left arm. The left hand's hand snap now has its own pose,
  `grips/BP_HG_C.holster-left.ini` (`hands::kPointHolster` on the reach,
  set by controls for the left hand only). It is never borrowed from the right
  hand: until it is posed, the left hand stays on the controller near the
  holster (the lid still opens). Shown exactly as posed (no
  `holster_pose::Fit`, which is for the right hand's gun grip). To pose it: put
  the left hand on the pistol in its holster the way you would grab it, hold
  left Y for 1 s (the hand freezes, the 3D holster stays in view), pose it with
  the right controller, tap Y to save. Drawing it still uses the held grip
  (`BP_HG_C.grip-left`). Log: `body: BP_HG_C.holster-left not posed yet`,
  `body: grip editor: posing the left hand's reach for the holstered pistol`.
- Log: `controls: the left hand is the gun hand now (it draws the pistol)`,
  `holster: pistol drawn by the left hand`, `controls: left hand took the pouch
  magazine`, `controls: mag release (left hand)`, `body: BP_HG_C.grip-left not
  posed yet: using BP_HG_C.grip, mirrored`.
