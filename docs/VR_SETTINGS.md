# VR settings and comfort

Owner request (2026-11-03): a dedicated VR sub-menu in Options, plus the
comfort, height, grip and aiming features from the VR reference study. The
full list and build order is in the project plan (vr-features-2026-11-03).
Build 1 (PR #68): the VR tab, vignette, head-in-wall fade, height modes,
real crouch and jump, grip toggle, aim smoothing. Build 2: teleport, world
size, which stick moves, body calibration (height and arms) and the shot
study (does the game shoot from the muzzle?). Still to come: the gun in the
left hand (full left-handed mode), then physics hands and finger physics.

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
| Moving | Smooth, Teleport | `locomotion` |
| Turning | Smooth, Snap | `turn_mode` |
| Snap turn | 15, 30, 45, 60, 90 deg | `snap_turn_degrees` |
| Turn speed | 60, 90, 120, 180, 240 deg/s | `smooth_turn_speed` |
| Move stick | Left, Right | `move_stick` |
| Move toward | Where you look, Stick hand | `move_direction` |
| Vignette moving | Off, Light, Medium, Strong | `comfort_vignette_move` (0, 0.35, 0.6, 0.85) |
| Vignette turning | Off, Light, Medium, Strong | `comfort_vignette_turn` |
| Head in a wall | Fades, No fade | `head_wall_fade` |
| Height | Character, Your own, Seated | `fixed_height` + `height_mode` |
| Real crouch | On, Off | `physical_crouch` |
| Hop to jump | Off, On | `physical_jump` |
| World size | 80% to 120% | `world_scale` (0.8 to 1.2) |
| Room walking | On, Off | `room_scale_follow` |
| Grip | Hold, Toggle | `grip_toggle` |
| Aiming eye | Right, Left | `dominant_eye_right` |
| Aim smoothing | Off, Light, Medium, Strong | `aim_smoothing` (0, 0.33, 0.66, 1) |
| Rifle stock | On, Off | `rifle_virtual_stock` |
| Gun stops at walls | On, Off | `gun_wall_collision` |
| Gun kick | Off, Light, Normal, Strong | `gun_recoil_kick` |
| Monitor shows | Right eye, Left eye, Both eyes | `monitor_view` (right, left, both) |
| Calibrate body | (action) | after the menu closes: height at 2 to 4 s (`standing_height_cm`), arms at 5 s (`body-measure.ini`) |
| Edit hand grips | (action) | opens the grip editor once the menu closes; while it is open the row reads "open, click to close" and closes it |
| Move holsters | (action) | 5 s after the menu closes (a buzz each second), each holster moves to where the hand holding its item is |

Move holsters is here because the right stick click (which used to be held
for it) is now the kick. The grip editor (left Y hold) and the arm measure
(left stick click hold) still open on their buttons too.

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
- Head in a wall: a line from the body's middle at head height to the head.
  If it hits something, the view fades to black over the last 1 to 9 cm.
  The player and the held gun are ignored. No fade while a menu is on the
  forearm, or when the view is more than 1 m from the character's capsule
  (the game moving its camera, as on the extract results screen; menus
  thread, PR #79). Owner 2026-11-03: "if i successfully extract i cant see
  the menu".

## Height, real crouch and hop to jump (controls.cpp `PhysicalCrouchAndJump`)

- **Character** (default): your eyes are at the character's eye height
  whatever your real height.
- **Your own**: your real head height. When the game's camera drops for a
  crouch, the mod gives that drop back (`stereo::SetViewRaiseCm`) so your eyes
  stay where your real head is.
- **Seated**: like Character, and real crouch and hop to jump are off.
- **Real crouch**: dropping your head 35 cm (`physical_crouch_cm`) below your
  standing height makes the character crouch (the game's own Crouch). Standing
  back up past 25 cm stands it up. Your standing height is learned in the
  first 1.5 s and is the highest still head height under 2.3 m.
- **Hop to jump** (off by default): the head rising faster than 1.1 m/s while
  above standing height presses jump. Then 1 s before it can fire again.

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

Calibrate body on the VR tab. After the menu closes, stand straight with your
arms straight out to the sides. At 2 s both controllers buzz and your eye
height is measured for 2 s (the highest point), then saved as
`standing_height_cm` (a strong buzz). At 5 s the body measures your arms as
before (left stick click 2 s still works too). Real crouch uses the standing
height from the start of every run instead of learning it.

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

## Log lines to check after a test

- `vr options: ready (a VR tab is added to Options)` at start.
- `vr options: following clicks through ...` the first time Options opens.
- `vr options: VR tab button added to ...` and `vr options: VR page added to
  Options (23 rows ...)` each time Options opens.
- `vr options: VR tab opened` when the VR tab is clicked.
- `vr options: VR page added to Options (23 rows ..., 20 rows with < >
  arrows; made over N frames, X ms in all, Y ms at most in one frame)`.
- `vr options: <row> -> <choice> (key=value, right arrow | left arrow | row
  clicked)` and `settings: saved key=value in hole-vr.ini` on each click, then
  `controls: settings changed (...)`; `... is already at its last choice` at
  an end.
- `comfort: head inside a wall or object, view faded` / `head out of the wall
  again after N s`.
- `controls: physical crouch (...)` / `physical stand (...)`.
- `controls: crouch (right stick down)` / `stand up (...)`, then `stick crouch:
  the character is crouched (the game's own crouch input did it)` or `... left
  the character standing; calling Crouch directly`, and `... by itself N s
  after` if something else changes it.
- `teleport: arc uses ...`, `teleport: moved N cm`, `teleport: no room ...`.
- `controls: height measured: your eyes are N cm above the floor`.
- `vr options: Edit hand grips clicked ...` then `controls: opening/closing the
  grip editor (asked for from the VR tab)`.
- `vr options: Move holsters clicked ...` then the holster countdown lines.
- `shot study 1: barrel line hits ... camera line hits ... N cm apart`, `shot
  study: gun ... fire functions: ...`, `lab: [..s] gun.<event>`.

## Tests

- `tests/test_comfort.cpp`: vignette texture, levels, smoothing, real crouch
  and jump, aim smoothing, grip toggle.
- `tests/test_vr_options.cpp`: ini edit in place, menu rows, current choice,
  teleport and hop to jump off by default, action rows.
- `tests/test_teleport.cpp`: arc, stick gesture, landing check.
- `tests/test_spectator.cpp`: monitor view crop and placement, straight ahead
  from the field of view, ini value parsing.
