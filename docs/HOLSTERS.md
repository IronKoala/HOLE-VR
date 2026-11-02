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

## Later: rifle slots

`HolsterId::kRifle` (chest anchor, across the back) and `kRifleMag` (belt
front) are defined but disabled. Rifles (`BP_AR_*`) have no holster yet and
stay in the hand as before; the pouch holds the current gun's magazines.
