# Gun melee (pistol whip, rifle butt and muzzle strikes)

Swing a held gun into an enemy (or one of the hub's mannequins or shooting
targets) and it hits. There is no button and no gesture to start it: the gun
has to really touch the target while it moves at hitting speed. Slow contact
is just a touch, so aiming, holstering and reloading near an enemy never hit.

## What can hit

| Gun | Parts that hit (ball radius) |
|---|---|
| Pistol | muzzle end of the slide (3.5 cm), middle of the slide for a backhand whip (3.5 cm), bottom of the grip for a hammer-fist strike (3.5 cm) |
| Long gun (rifle, shotgun) | butt (the measured stock pad when there is one, 5 cm), muzzle (4 cm), middle of the handguard (4 cm) |

A rifle carried in the off hand (one-handed, by its foregrip) can hit too.

## How a hit is decided

- **Speed is measured in the headset's own tracking space**, so walking, stick
  and snap turns, recentring and the seated height drop never count as a
  swing. Only your real arm moving the gun does. A hit needs 2.0 m/s at the
  striking part (`gun_melee_min_speed`); a full hit is 4.5 m/s
  (`gun_melee_full_speed`). In between, the hit scales from 40 % to 100 %.
  Steady aiming is under 1 m/s.
- **Contact** is a swept ball from where the striking part was last frame to
  where it is now: first on the game's own bullet channel (finds mannequins,
  targets, and enemies whose bodies answer to it; a wall in the way stops it),
  then on each nearby enemy's own body shapes, so a hit lands on a real body
  part (head, arm, chest...).
- **The swing is the hand's own path.** The wall check holds the drawn gun out
  of whatever it reaches (a wall, a mannequin, an enemy); the speed and the
  contact use where your hand is taking the gun, so being held at a target
  does not slow the hit. A wall (anything that cannot be hit) between the gun
  and an enemy still stops it; what an enemy carries or wears does not.
- A shot's kick moving the gun is never a swing. One swing through a body is
  one hit (`gun_melee_cooldown_s`, 0.45 s per target).

## What a hit does

It goes through the game's own hit event (the one bullets, the kick and enemy
melee weapons use), so targets react the game's way.

- **Damage** = share of the target's own max HP × swing × the game's body-part
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

- `... can hit with: ...` once per gun picked up (the strike points).
- `... hit enemy|target NAME (CLASS, part BONE, surface N) at X m/s ...` for
  every hit, with the damage and stun worked out, then `NAME HP a -> b`.
- `... swung at X m/s, touched nothing it can hit (stopped by NAME) (wall push
  N cm)` for the first 12 fast swings that missed: the speed reached, what was
  in the way, and how far the wall check was holding the gun back.
- `N fast swing frames, M hits so far` once a minute while swinging.

Code: `src/vr/gun_melee.cpp` (engine side), `src/vr/gun_melee_math.h`
(tested in `tests/test_gun_melee.cpp`), called from `MeleeStrikes` in
`src/vr/controls.cpp`.
