# Gun melee (pistol hammer-fist, rifle butt strikes)

Swing a held gun into an enemy (or one of the hub's mannequins or shooting
targets) and it hits. There is no button and no gesture to start it: the gun
has to really touch the target while it moves at hitting speed. Slow contact
is just a touch, so aiming, holstering and reloading near an enemy never hit.

## What can hit

Owner 2026-11-08: "Set Pistol to hammer-fist only. AR/SG/SR to butt strikes
only."

| Gun | Part that hits (ball radius) |
|---|---|
| Pistol | the bottom of the grip, a hammer-fist strike (3.5 cm) |
| Rifle, shotgun, sniper rifle | the butt: the measured stock pad when there is one (5 cm) |

A rifle carried in the off hand (one-handed, by its foregrip) can hit too.

## Butt-strike hand pose

With a rifle, shotgun or sniper rifle in the main hand, bring the main hand
near your head with the off hand up high: the main hand takes its own pose,
`<gun class>.butt-strike` (for example `BP_AR_C.butt-strike`; the left hand's
is `...butt-strike-left`). It is only a hand pose; it starts nothing.

- The stance: the main hand within 30 cm of the eyes
  (`butt_strike_pose_near_head_cm`), the off hand 10 cm or more above it
  (`butt_strike_pose_off_hand_above_cm`), and the barrel 50 degrees or more
  off where you look (`butt_strike_pose_barrel_off_view_deg`), so shouldering
  and aiming, even upwards, never count. Once in, it holds with some slack
  (10 cm, 6 cm, 15 degrees) and for 0.5 s after (`butt_strike_pose_hold_s`),
  so the strike itself keeps the pose. `butt_strike_pose=0` turns it off.
- To pose it: get into the stance, hold left Y 1 s (the grip editor poses the
  main hand first in this stance, not the off hand on the handguard), pose
  the main hand with the left controller, tap Y to save.
- Until it is posed the hand keeps its normal grip on that gun.
- Log lines: `gun melee: butt-strike stance on|off (main hand N cm from the
  eyes, off hand N cm above it, barrel N deg off your view ...)`, and `body:
  <class>.butt-strike not posed yet: using ...`.

## How a hit is decided

- **Speed is measured in the headset's own tracking space**, so walking, stick
  and snap turns, recentring and the seated height drop never count as a
  swing. Only your real arm moving the gun does. A hit needs 2.0 m/s at the
  striking part (`gun_melee_min_speed`); a full hit is 4.5 m/s
  (`gun_melee_full_speed`). In between, the hit scales from 40 % to 100 %.
  Steady aiming is under 1 m/s.
- **Contact** is a ball swept from where the striking part was last frame to
  where it is now, the way the game's own kick looks for what it hits: a
  multi sweep, which lists everything the ball passes through and then the
  one thing that stops it. The hub's mannequins and targets only overlap the
  game's hit channel (bullets and the kick pass through them), so a sweep
  that reports only what stops it never saw them: that was the "no damage,
  no stun, no sound" on the hub dummies (owner, 2026-11-08). In order:
  1. the game's own hit channel (what bullets and the kick use),
  2. the visibility channel by the game's hit boxes, then by the shapes you
     see (the gun's wall check meets the mannequins there),
  3. each nearby enemy's own body shapes, so a hit lands on a real body part
     (head, arm, chest...).

  The first thing that can be hit wins, unless something that cannot be hit
  stopped the ball before it (a wall). What the ball only passes through (a
  trigger volume, an enemy's capsule, what it carries) is neither.
- **The swing is the hand's own path.** The wall check holds the drawn gun out
  of whatever it reaches (a wall, a mannequin, an enemy); the speed and the
  contact use where your hand is taking the gun, so being held at a target
  does not slow the hit. A wall (anything that cannot be hit) between the gun
  and an enemy still stops it; what an enemy carries or wears does not.
- A shot's kick moving the gun is never a swing. Neither is the gun jumping:
  over 15 m/s (the gun snapping into a hand reads 58 m/s), or the first
  0.3 s after a gun comes into a hand. One swing through a body is one hit
  (`gun_melee_cooldown_s`, 0.45 s per target).

## What a hit does

It goes through the game's own hit event (the one bullets, the kick and enemy
melee weapons use), so targets react the game's way.

- **Damage** = share of the target's own max HP (an enemy's GetHP, a
  mannequin's HP and MaxHP numbers) × swing × the game's body-part
  and armour multiplier. Pistol 15 % (`gun_melee_pistol_damage`), long gun
  25 % (`gun_melee_long_gun_damage`) for a full swing. Armour soaks it the
  way it soaks the game's melee.
- **Stun** = share of the kick's stun × swing × the game's stun multiplier for
  the body part. Pistol 0.7 (`gun_melee_pistol_stun`), long gun 1.2
  (`gun_melee_long_gun_stun`).
- A body that is already a ragdoll gets shoved, like the kick does.
- The game's blunt melee sound and hit splash play at the contact, and the
  controller buzzes (both hands when two hold the gun).

Turn it off with `gun_melee=0` in `hole-vr.ini`. All settings apply live.

## Checking a run

Log lines start `gun melee:`:

- `... can hit with: ...` once per gun picked up (the strike point).
- `... hit enemy|target NAME (CLASS, part BONE, surface N) at X m/s (...,
  found by SWEEP) ...` for every hit, with the damage and stun worked out,
  then `NAME HP a -> b`.
- `hit sound NAME, hit splash ready` at the first hit (the hammer sound, or
  the kick's when the hammer one is not loaded).
- `... swing hit nothing: N frame(s) at hitting speed, top X m/s; the sweeps
  met ...` for each of the first 40 swings that missed: what each sweep met
  (`hit channel:`, `visibility:`, `shapes you see:`, each NAME as `a wall`,
  `passed through`, or `can be hit, but behind a wall`) and the nearest
  enemy.
- `so far N swings (M frames at hitting speed), K hits (found by ...); J
  jumps of the gun ... not counted as swings` at most once a minute.

Code: `src/vr/gun_melee.cpp` (engine side), `src/vr/gun_melee_math.h`
(tested in `tests/test_gun_melee.cpp`), called from `MeleeStrikes` in
`src/vr/controls.cpp`.
