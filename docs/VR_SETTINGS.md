# VR settings and comfort

Owner request (2026-11-03): a dedicated VR sub-menu in Options, plus the
comfort, height, grip and aiming features from the VR reference study. The
full list and build order is in the project plan (vr-features-2026-11-03).
Build 1 (PR #68): the VR tab, vignette, head-in-wall fade, height modes,
real crouch and jump, grip toggle, aim smoothing. Build 2: teleport, world
size, which stick moves, body calibration (height and arms) and the shot
study (does the game shoot from the muzzle?). Then: empty hands stop at
walls, and their fingers give way on the surface like physics hands. Still to come: the gun in the
left hand (full left-handed mode).

## The VR tab (src/vr/vr_options.cpp, src/vr/vr_menu_model.cpp)

When the Options menu (`UIM_Option_C`) goes onto the forearm, a **VR** tab
button is added after the game's own tab buttons (GAME ... GAMEPAD). It is a
copy of the game's own tab button class, so it looks and clicks like the rest.
Its page is a scroll box in the game's `WidgetSwitcher_OPTION`, holding one
button per setting. The buttons are copies of the game's Back button class.

Each setting row is `[<] [Setting: choice] [>]` (owner, 2026-11-03: "left
and right arrows to switch between options"). The right arrow steps to the
next choice and the left arrow to the previous one; both stop at the ends
(the choices are listed low to high, so right is faster, stronger or
bigger). Clicking the middle still steps forward and wraps round. The arrows
are copies of the same game button inside 64-unit-wide size boxes, in a
horizontal box. The setup rows (Calibrate body, Edit hand grips, Move
holsters) are one button. The right stick scrolls the page (the forearm
menu's own stick scrolling, `world_menu.cpp`, which sends the mouse wheel to
whatever the laser is on). A step writes the ini key(s) into `hole-vr.ini` in place (comments and layout kept,
`config::Set`), and tells every part of the mod to re-read its settings
(`config::NotifyChanged`). Nothing needs a restart.

| Row | Choices | ini key(s) |
|---|---|---|
| Movement | Teleport, no turning; Teleport, snap turn 30 deg; Walk, no turning; Walk, snap turn 15, 30, 45, 60, 90 deg; Walk, smooth turn 60, 90, 120, 180, 240 deg/s (default 90) | `locomotion` + `turn_mode` (0 snap, 1 smooth, 2 no stick turning) + `snap_turn_degrees` / `smooth_turn_speed` |
| Move stick | Left, Right | `move_stick` |
| Move toward (not with Teleport) | Where you look, Stick hand | `move_direction` |
| Vignette moving | Off, Light, Medium, Strong | `comfort_vignette_move` (0, 0.35, 0.6, 0.85) |
| Vignette turning (Smooth turn only) | Off, Light, Medium, Strong | `comfort_vignette_turn` |
| Wall fade | Off, 5, 10, 15, 20, 25 cm away | `head_wall_fade` + `head_wall_fade_start_cm` (on, 15) |
| Height | Character, Your own, Seated | `fixed_height` + `height_mode` |
| Real crouch (not Seated) | On, Off | `physical_crouch` |
| Hop to jump (not Seated) | Off, On | `physical_jump` |
| World size | 80% to 120% | `world_scale` (0.8 to 1.2) |
| Roomscale | On, Off | `room_scale_follow` |
| Grip | Hold, Toggle | `grip_toggle` |
| Aim smoothing | Off, Light, Medium, Strong | `aim_smoothing` (0, 0.33, 0.66, 1) |
| Rifle stock | On, Off | `rifle_virtual_stock` |
| Gun stops at walls | On, Off | `gun_wall_collision` |
| Hands stop at walls | On, Off | `hand_wall_collision` |
| Gun kick | Off, Light, Normal, Strong | `gun_recoil_kick` |
| Monitor shows | Right eye, Left eye, Both eyes | `monitor_view` (right, left, both) |
| Calibrate body | (action) | height → arms forward → arms out → bent elbows → forward check; hold both triggers for 2s to record; left X keeps the current arms |
| Edit hand grips | (action) | opens the grip editor once the menu closes; while it is open the row reads "open, click to close" and closes it |
| Move holsters | (action) | 5 s after the menu closes (a buzz each second), each holster moves to where the hand holding its item is |

**Descriptions** (owner 2026-11-06: "when one is selected/cursor is hover
over it, i want a brief description of what the option does just off to the
right"). The page is a horizontal box: the scroll box of rows (filling the
width) and, right of it, a 520-unit column (`SizeBox`) holding one wrapping
`TextBlock`. Pointing the laser at a row or its arrows, clicking it, or
selecting it with a gamepad shows that row's description (`Row::about` in
`vr_menu_model.cpp`), moved down level with the row (a render translation,
so nothing else is laid out again) and kept inside the column; it follows
the list when it scrolls. Moving off the rows keeps the last description.
Hover comes from the game button class's own hover/unhover and focus events
(`AfterHover`), which the laser's widget interaction sets off like a mouse.
The text gets the rows' font, and the forearm menu outlines and enlarges it
with the rest of the page. Log: `vr options: description column added right
of the rows`, `descriptions follow N row button event(s)`, and `describing
<row>` for the first six each time Options opens.

**Hidden rows** (owner 2026-11-06: "are any of them redundant/moot?"). A
row that does nothing with another row's current choice is collapsed
(`Row::only_with`): Vignette turning unless Movement is a Smooth turn (a
snap is instant, so there is nothing to darken); Move toward with Teleport
(the arc follows the stick hand); Real crouch and Hop to jump when Seated
(they are off then). Changing the deciding row shows or hides them at
once. Log: `vr options: Movement: hidden (they do nothing now): Vignette
turning`.

**Removed and merged** (2026-11-06): "Aiming eye" is off the tab: since
6DoF aiming it only chose which eye the rifle's shoulder lock lines the
sights up with. `dominant_eye_right` still works in the ini (right unless
set) for a later left-handed mode. "Head in a wall" and "Wall fade starts"
are one row, Wall fade: Off or the distance the fade starts at. Owner's
answer the same day: "Smooth turn, snap turn, teleport all merge into row":
Moving, Turning, Snap turn and Turn speed are one row, Movement, stepping
Teleport, Snap turn 15-90 deg, Smooth turn 60-240 deg/s. Then (owner,
same day): "Rename room walking to roomscale, and to add an option to turn
off joystick turning altogether": `turn_mode=2` turns the stick's sideways
push off (you turn with your body), offered as "Teleport, no turning" and
"Walk, no turning". A choice's keys that the ini has count for it when they
match and against it when they don't, so "no turning" (two keys) beats a
turn choice whose step or speed happens to match the ini.

Move holsters is here because the right stick click (which used to be held
for it) is now the kick. The grip editor (left Y hold) and the arm measure
(left stick click hold) still open on their buttons too.

Right-stick kick automatically chooses the right leg when you aim right of
the body; left/centre uses the left leg. Aim and side lock when the kick starts.
The existing rifle fire-selector use of the button is unchanged. The VR body
and native kick trace share the chosen side/direction, with the game's extra
upward trace offset removed. If the bilateral adapter or body is unavailable,
the native kick remains the fallback. No arm recalibration is required.

The tab button and an empty page are added when Options opens; the rows
are then made a few per frame (up to 3 ms of each frame, at least one row)
while the page is still hidden. Making all 23 rows at once took 71 ms, a
hitch the owner felt when Options opened (pause-menu thread's timing,
2026-11-03 02:49 run).

Why cycle buttons and not the game's sliders, checkboxes and dropdowns: the
game's option rows are bound to its own settings object, and dropdowns open a
popup that would land outside the forearm panel. A button that cycles is one
widget class the mod already knows how to click with the laser.

## Comfort vignette and head-in-wall fade (src/vr/comfort.cpp)

Both are OpenXR quad layers locked to the head (VIEW space), drawn by the
compositor on top of the game. The game renders nothing extra, so they cost no
game frame time. One small texture holds 16 vignette strengths and 16 fade
levels. Each frame picks one part of it.

- Vignette: strength = the moving setting times how fast you move (full at
  3 m/s) or the turning setting times how fast you turn (full at 90 deg/s,
  smooth turning only). It closes in over 0.1 s and opens over 0.3 s. At
  Strong the clear circle is about 33 deg wide each side of centre.
- Head in a wall: a ball is swept from the body's middle at head height to
  the eyes. Its radius is where the fade begins (`head_wall_fade_start_cm`,
  15 cm, the "Wall fade" row). Where it stops, the eyes are that
  close to a surface; the closer they are to it, measured along the
  surface's normal (comfort_math `WallFade`), the darker, until black
  `head_wall_fade_full_cm` (2 cm) before the surface. Until 2026-11-04 the
  distance was measured along the sweep, so leaning sideways along a wall
  counted as getting closer (an outside review's example: eyes 11 cm from a
  wall, 30 cm to the side, went fully black instead of about 31%). The first version used
  a line and only began fading 1 cm inside the wall (black 9 cm in), and
  the near clip plane is 1 cm, so the wall vanished and you saw through
  before it went dark (owner 2026-11-03 10:07: "the wall fade needs to kick
  in a lot sooner"). The player, the held gun, anything either hand
  holds and the forearm menu/HUD panels are ignored. No fade while a menu is on the
  forearm, or when the view is more than 1 m from the character's capsule
  (the game moving its camera, as on the extract results screen; menus
  thread, PR #79). Owner 2026-11-03: "if i successfully extract i cant see
  the menu".

## Height, real crouch and hop to jump (controls.cpp `PhysicalCrouchAndJump`)

- **Character** (default): your eyes are at the character's eye height
  whether you physically sit or stand. Calibration keeps this height and does
  not guess your physical posture or overwrite saved height references.
- **Your own**: your real head height. When the game's camera drops for a
  crouch, the mod gives that drop back (`stereo::SetViewRaiseCm`) so your eyes
  stay where your real head is. Calibrate standing upright to record the
  standing floor-to-eye reference.
- **Seated**: like Character, and real crouch and hop to jump are off.
  Calibration records a separate seated reference while you sit comfortably;
  it leaves the standing reference and character height unchanged.
- **Real crouch**: dropping your head 35 cm (`physical_crouch_cm`) below your
  standing height makes the character crouch (the game's own Crouch). Standing
  back up past 25 cm stands it up. A valid saved standing measurement sets the
  baseline; otherwise it learns the highest still head height under 2.3 m,
  starting after 1.5 s.
- **Hop to jump** (off by default): the head rising faster than 1.1 m/s while
  above standing height presses jump. Then 1 s before it can fire again.

In Character and Seated modes, the drawn pelvis follows the game's stance.
Shoulder fitting adjusts the spine within bounds instead of forcing a squat;
the game's actual crouches still apply. Switching height mode does not require
repeating a successful arm calibration.

### Stick crouch check and the camera hold (controls.cpp)

- **Stick crouch** (right stick down) sends the game's own crouch input. 0.4 s
  later the character must be in the stance the press asked for
  (`bIsCrouched`); if not, `Crouch`/`UnCrouch` is called directly. Crouching
  ends a sprint. For 3 s after, a stance change nobody asked for is logged.
  (Owner, 2026-11-03: "i cant stay crouched"; the run showed 11 presses in
  10 s and no stance readout.)
- Verified 2026-11-03 (three runs, 06:47 to 07:37 UTC): the game's own crouch
  input never crouched the character from standing (its crouch is
  hold-to-crouch, and the release 0.1 s later stands it up), so each crouch
  went through `Crouch` directly and stayed until the next press; standing
  up through the game's input works.
- **Camera hold** (Character and Seated height): if the game camera sits more
  than 8 cm below its standing height with no crouch for 2 s while the
  character stands still, the view is held at standing height until the
  camera comes back. Sprinting lowers the game camera 11 to 26 cm on purpose,
  so moving or sprinting never starts the hold (the first version did, on
  every sprint). The standing height is the highest camera height held for
  1 s, but after the first one it may only rise 4 cm (run 2 learned 165 cm
  after 151 cm and then held the view 13 cm too high).

## Grip toggle and aim smoothing

- **Grip: Toggle** (`grip_toggle=1`): squeeze once to take a magazine or to
  draw the pistol from its holster, and squeeze again to let go. The slide and
  the support hand on the gun's front stay hold-to-grab. The old `holster_pistol_grip_toggle` still works when
  `grip_toggle` is not set.
- **Aim smoothing**: takes out small shakes of the gun hand (under a few
  degrees). Fast swings and big moves (over 20 deg in a frame) are not slowed.

## Teleport (src/vr/teleport.cpp, src/vr/teleport_math.cpp)

Moving: Teleport (`locomotion=teleport`). Push the move stick forward: an arc
of small balls comes out of that hand (a throw at 8.5 m/s, up to about 8 m on
flat ground) and a flat disc shows where you land. The disc only shows on a
floor you can stand on: not steeper than about 45 degrees, not more than
1.5 m above your feet, not right at your feet. Let go of the stick to go
there: the view blinks black for 0.2 s and the character is moved with the
engine's own teleport (`K2_TeleportTo`, which refuses a spot the capsule
does not fit). Pull the stick back to cancel. Turning stays on the other
stick. The balls and disc are the engine's sphere mesh, made once per level
on the player, with no collision and no shadow.

## World size

`world_scale` scales how far the tracking moves you in the game: the eye
spacing, head movement and hands together. 1.2 makes the world look 20%
bigger (you feel smaller). Done in the stereo view offset (stereo.cpp
`WorldScale`), so the hands, holsters and wall checks all follow.

## Which stick moves

`move_stick=right` swaps the two sticks: the right stick moves (its click
sprints) and the left stick turns, jumps and crouches. The gun stays in the
right hand. Putting the gun in the left hand is a later change.

## Body calibration

Put down held items and select **Calibrate body** on the VR tab, then close the
menu. Follow the five steps at a comfortable pace:

1. **Height:** relax and look ahead. Stand for **Your own**, sit for **Seated**,
   or use the usual posture for **Character**. Character keeps existing height
   references because the tracking floor alone does not identify physical posture.
2. **Forward:** extend arms comfortably forward, palms inward and wrists in line.
   Choose a comfortable height; do not lock the elbows.
3. **Out:** open the straight arms comfortably to the sides, palms down and
   wrists in line. This opening motion helps estimate the shoulders.
4. **Bend:** let the hands rise by bending the elbows. Keep elbows near their
   current places and wrists in line; palms can turn naturally.
5. **Forward check:** return to the earlier comfortable forward pose, palms inward.

When comfortable, **hold both index triggers for two seconds**. Recording happens
during that hold and tolerates natural movement. Vibration builds from 0% toward
90%, then reaches 100% when the step is accepted. Release both triggers between
steps. Releasing either early resets only the current recording and stops the
vibration. A no longer confirms. The bar shows recording progress. There is no overhead reach, exact required arm
height, prescribed bend angle or repeated raise/lower cycle. The program learns
from the confirmed poses instead of deciding readiness from guessed joint positions.

**Left X skips arm setup and keeps the current arms.** Your own/Seated can still
record the corresponding height reference; Character or unavailable floor tracking
keeps existing references. No uncomfortable movement should be forced. B/Y cancels.
Tracking loss pauses the current sample and retains completed steps; release both triggers, then hold them again when ready. Recentring or
changing height mode during setup stops the attempt without saving partial data.

The fit learns the shoulder directions supported by the recorded motion. Other
directions remain estimates. Elbow motion independently estimates forearm length;
upper-arm length can still depend on an uncertain shoulder. These confidence levels
are saved separately, and an uncertain fit is not labeled a measured proportion.
The final forward sample checks repeatability rather than proving exact anatomy.

One learned wrist mapping applies across poses. Free-hand elbow posture remains
independent of wrist turning. Profile v6 preserves separate standing/seated heights,
shoulder confidence and forearm confidence. Updating one height keeps the other;
skipping arms keeps the previous arm fit. Older profiles remain usable and are not
rewritten by installation. Cancelled/failed setup keeps the saved file; a successful
replacement backs it up first. V1/v2 heights remain unclassified, never assumed standing.
See [BODY.md](BODY.md) for validation rules and remaining tracking limitations.

## Shot study (does the game shoot from the muzzle?)

At the first shots of every run the log gets, per shot, where a line from the
muzzle along the barrel lands and where a line from the game camera along the
view lands, with the distance between them. At the first shot it also lists
the gun's and player's functions and variables with fire, shot, trace,
bullet, muzzle, aim, hit, projectile, spread, damage or camera in the name, and
for 0.4 s around the first three shots the weapon lab trace logs the game's
own gun and player events. With the owner's report of where the bullet hole
appeared, this says whether shots need moving to the muzzle.

## Monitor view (src/vr/spectator.cpp, src/vr/spectator_math.cpp)

What the game window on the monitor shows, so OBS or the NVIDIA app can record
a normal-looking game picture. Steam's own Game Recording can't be used: it
refuses to record while SteamVR runs over Steam Link (the Steam Frame), with
"VRLink driver in use; can't start recording" in Steam's
`logs/streaming_log.txt`.

- The engine draws both eyes side by side into the window, and the headset
  picture is copied from there first (openxr.cpp `CopyEyes`). Only then, on
  the same GPU queue and right before the window is shown, the window is
  redrawn: the chosen eye's half is cropped to `monitor_view_aspect` (16:9),
  centred on straight ahead for that eye (its field of view is off-centre),
  scaled to fill the window's height or width and centred on black.
- On the 3440x1440 ultrawide: the right eye's 1720x968 middle is shown as
  2560x1440 with black bars left and right. On a 16:9 monitor it fills the
  screen.
- `monitor_view=both` keeps the old side-by-side picture. The headset picture
  is the same either way.
- Needs `d3dcompiler_47.dll` (part of Windows 10 and 11) for its one small
  shader; without it, or if any step fails, the log says so once and the
  monitor keeps showing both eyes.
- Log: `monitor view: right eye, 16:9`, then `monitor view: showing right eye
  WxH from (x,y) as WxH at (x,y) in the WxH window`. Cost shows in the perf
  report as "monitor view (one eye on the monitor)".

## Hands stop at walls (src/vr/body/hand_walls.cpp)

Owner's list, item 12: "physics hands that stop at walls". Each frame, for
each empty hand, a ball 4 cm in radius (`hand_wall_radius_cm`) is swept from
the eyes to the controller with the game's own trace (KismetSystemLibrary
`SphereTraceSingle`, visibility channel, the player and everything attached
to the player ignored). If it touches something, the VR body draws that hand
where the ball stopped: against the wall, floor, door, table or person in
the way, sliding along it as the real hand moves. The arm follows the drawn
hand.

- Only the drawn hand moves. Grabbing, holsters, the mag pouch, doors and
  the menu laser still use where the controller really is, so nothing gets
  harder to reach.
- A hand holding something follows that object: the gun keeps itself out of
  walls (`gun_wall_collision`, a trace from the eyes to the muzzle), and the
  support hand and slide hand follow the gun.
- If the eyes themselves are inside something (leaning through a wall), the
  hands are not stopped, since there is no sensible surface to stop them at.
- The forearm menu and wrist HUD panels are ignored too (they block the
  game's visibility traces). Before, looking at your wrist put the panels
  between the eyes and the left hand, and the hand was stopped up to 22 cm
  short of the controller for as long as you looked (run 2026-11-03 10:47,
  found by the slide pinch thread).
- A clear sweep is reused while the eyes and the hand stay within 3 mm of
  where they were, so a still hand costs nothing. The time shows in the
  perf report as "body hands stop at walls".
- Runs after the arm measure (Calibrate body wants the real hands) and not
  while the grip editor holds the hands still.
- The fingers reach well past the fist the ball is centred on, so when the
  ball's way is clear a thin ball (2 cm) is also swept from the fist along
  the controller's pointing direction, `hand_wall_probe_cm` (14) past it. A
  surface it meets is remembered for the fingers and the palm (below) before
  the fist gets there; the hand itself isn't moved for it.

Physics-style fingers (owner's list, item 13, "physical fingers";
`src/vr/body/finger_rest.cpp`). Reworked 2026-11-03 after the owner's
hand-physics reference, SentientArt/TheBasement (an Unreal 5.3 VR demo):
there each hand is a ragdoll, a box for the palm and a capsule for every
finger bone, and Unreal's Physics Control pulls every piece toward the
controller's pose like a spring with a capped force, so a wall simply wins
and fingers bend back where they touch. Our hands are drawn, not simulated,
so this copies the result:

- The wall check keeps the surface the hand last met (its impact point and
  normal) while the hand stays within 20 cm of that point and on its open
  side.
- Each finger of that empty hand has one bend number: 0 straight, up to 1.2
  curled as the controller asks, and below 0 bent back past straight (to the
  joints' backward limits at -1: the knuckle 25 deg, the outer joints 5 deg;
  the thumb half that).
- Each frame a finger moves from where it was toward the controller's bend
  and stops where it touches the surface (a sprung finger can't jump
  through a wall). If the hand moved it into the surface, the surface pushes
  it out the way it presses on it: pressed on its pad it bends back, pressed
  on its nails it curls, straight on a straight finger bends back and a
  curled one buckles further. When one way can't get it out, the other is
  tried. Last frame's side wins a near tie, so it doesn't flicker.
- Fingertip drag: a fingertip on the surface (within 1 cm) trails where the
  hand was 0.05 s before as the hand slides along it, as far as small bends
  (0.3) and spreads (12 deg) allow, eased over 0.06 s and never into the
  surface. `hand_finger_drag` (1; 0 = off, 2 = twice as much).
- Plain math on the plane, no extra traces. `hand_fingers_rest_on_walls=1`
  (on); off with the hand wall check too. Fingers on the gun and magazines
  keep their stored grips.

Other code that wants the hand the player sees (pressing the range timer's
button, for example) reads `body::GetHandContacts(side, &c)`: the five
drawn fingertips with the finger radius, the palm's middle and its outward
direction, and whether the hand is empty, as of the body's last frame (false
when older than 0.1 s). These are after the hand stopped at a wall and its
fingers gave way, so a press matches what the eyes see.

Hands settle flat on walls (owner 2026-11-03 14:14: the fingers "would
close and make a fist, in real life people tend to press their hand
against the wall"). The first version (1694d3d) turned the palm toward a
remembered surface from 8 cm before touching it; since the reference study
it behaves like a physics hand: the hand keeps the controller's angle until
it really touches (its ball is stopped, or the fingertip probe reaches the
surface), then turns to face the surface as the real hand goes on toward
it, flat once the real fist is `hand_press_flat_push_cm` (4) past where the
ball stops. The palm is set on the surface (middle 0.3 cm off, never in) but
never pulled toward it from further than the wall check held the hand back.
Only a palm or fingertips-first touch turns it: the back of the hand (palm
100-115 deg or more off the surface, faded) and its side (a karate chop:
the line across the knuckles pointing into the surface) keep their angle.
The fingers follow the controller's curl less the flatness.
`hand_press_flat_on_walls=1` (on); it needs the hand wall check.

## Log lines to check after a test

- `vr options: ready (a VR tab is added to Options)` at start.
- `vr options: following clicks through ...` the first time Options opens.
- `vr options: VR tab button added to ...` and `vr options: VR page added to
  Options (26 rows ...)` each time Options opens.
- `vr options: VR tab opened` when the VR tab is clicked.
- `vr options: VR page added to Options (26 rows ..., 23 rows with < >
  arrows; made over N frames, X ms in all, Y ms at most in one frame)`.
- `vr options: <row> -> <choice> (key=value, right arrow | left arrow | row
  clicked)` and `settings: saved key=value in hole-vr.ini` on each click, then
  `controls: settings changed (...)`; `... is already at its last choice` at
  an end.
- `comfort: head close to <thing> (N cm from it), view fading` /
  `head away from the wall again after N s (it got to N cm from|past the
  surface)`. Logged when the fade passes half way.
- `controls: physical crouch (...)` / `physical stand (...)`.
- `controls: crouch (right stick down)` / `stand up (...)`, then `stick crouch:
  the character is crouched (the game's own crouch input did it)` or `... left
  the character standing; calling Crouch directly`, and `... by itself N s
  after` if something else changes it.
- `teleport: arc uses ...`, `teleport: moved N cm`, `teleport: no room ...`.
- `body calibration:` stage, rejection and per-arm fit-quality records.
- `vr options: Edit hand grips clicked ...` then `controls: opening/closing the
  grip editor (asked for from the VR tab)`.
- `vr options: Move holsters clicked ...` then the holster countdown lines.
- `hand walls: on (a 4.0 cm ball from the eyes to each empty hand ...; the
  fingertips feel 14 cm past the fist)` once, `hand walls: left/right
  fingertips found <thing> N cm past the fist` the first 8 times, then `hand walls: left/right hand stopped at <thing> for N s; the real hand
  went up to N cm past the surface` for the first 12 touches, then a count
  each minute.
- `fingers: left/right hand's fingers give way against the surface (bend
  wanted->used, below 0 = bent back past straight: thumb 0.60->0.20, index
  0.12->-0.35, ...)`, the first 10 times a wall changes a finger's bend
  noticeably (at most every 2 s per hand).
- `fingers: left/right fingertips drag along the surface (hand sliding N
  cm/s, a tip trails N cm)`, the first 6 times a tip trails 1 cm or more.
- `body: left/right hand pressed flat on a surface (palm turned N deg to face
  it; it touched with the real fist N cm off the surface, now N cm)`, the
  first 10 times a hand goes flat.
- `shot study 1: barrel line hits ... camera line hits ... N cm apart`, `shot
  study: gun ... fire functions: ...`, `lab: [..s] gun.<event>`.

## Tests

- `tests/test_comfort.cpp`: vignette texture, levels, smoothing, real crouch
  and jump, aim smoothing, grip toggle.
- `tests/test_finger_rest.cpp`: fingers against a plane (the bend path, a
  curling finger stops where it touches, pressed on pads bends back past
  straight, pressed on nails curls, tip first: straight bends back and
  curled buckles, the other way when one can't clear, last frame's side
  wins a tie, same result anywhere in the world, fingertip drag).
- `tests/test_vr_options.cpp`: ini edit in place, menu rows, current choice,
  teleport and hop to jump off by default, action rows.
- `tests/test_teleport.cpp`: arc, stick gesture, landing check.
- `tests/test_spectator.cpp`: monitor view crop and placement, straight ahead
  from the field of view, ini value parsing.
