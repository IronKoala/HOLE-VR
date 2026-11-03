# VR settings and comfort

Owner request (2026-11-03): a dedicated VR sub-menu in Options, plus the
comfort, height, grip and aiming features from the VR reference study. The
full list and build order is in the project plan (vr-features-2026-11-03).
This page covers build 1. Teleport, world scale, left-handed mode, body
calibration and muzzle trace come in build 2. Physics hands and finger
physics come in build 3.

## The VR tab (src/vr/vr_options.cpp, src/vr/vr_menu_model.cpp)

When the Options menu (`UIM_Option_C`) goes onto the forearm, a **VR** tab
button is added after the game's own tab buttons (GAME ... GAMEPAD). It is a
copy of the game's own tab button class, so it looks and clicks like the rest.
Its page is a scroll box in the game's `WidgetSwitcher_OPTION`, holding one
button per setting. The buttons are copies of the game's Back button class.

Each row reads "Setting: choice". Clicking a row moves it to the next choice,
writes the ini key(s) into `hole-vr.ini` in place (comments and layout kept,
`config::Set`), and tells every part of the mod to re-read its settings
(`config::NotifyChanged`). Nothing needs a restart.

| Row | Choices | ini key(s) |
|---|---|---|
| Turning | Smooth, Snap | `turn_mode` |
| Snap turn | 15, 30, 45, 60, 90 deg | `snap_turn_degrees` |
| Turn speed | 60, 90, 120, 180, 240 deg/s | `smooth_turn_speed` |
| Move toward | Where you look, Left hand | `move_direction` |
| Vignette moving | Off, Light, Medium, Strong | `comfort_vignette_move` (0, 0.35, 0.6, 0.85) |
| Vignette turning | Off, Light, Medium, Strong | `comfort_vignette_turn` |
| Head in a wall | Fades, No fade | `head_wall_fade` |
| Height | Character, Your own, Seated | `fixed_height` + `height_mode` |
| Real crouch | On, Off | `physical_crouch` |
| Hop to jump | Off, On | `physical_jump` |
| Room walking | On, Off | `room_scale_follow` |
| Grip | Hold, Toggle | `grip_toggle` |
| Aiming eye | Right, Left | `dominant_eye_right` |
| Aim smoothing | Off, Light, Medium, Strong | `aim_smoothing` (0, 0.33, 0.66, 1) |
| Rifle stock | On, Off | `rifle_virtual_stock` |
| Gun stops at walls | On, Off | `gun_wall_collision` |
| Gun kick | Off, Light, Normal, Strong | `gun_recoil_kick` |
| Measure arms | (action) | starts the arm measure 5 s after the menu closes |

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
  The player and the held gun are ignored.

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

## Grip toggle and aim smoothing

- **Grip: Toggle** (`grip_toggle=1`): squeeze once to grab a magazine, the
  gun's front (support hand) or the pistol from its holster, and squeeze again
  to let go. The old `holster_pistol_grip_toggle` still works when
  `grip_toggle` is not set.
- **Aim smoothing**: takes out small shakes of the gun hand (under a few
  degrees). Fast swings and big moves (over 20 deg in a frame) are not slowed.

## Log lines to check after a test

- `vr options: ready (a VR tab is added to Options)` at start.
- `vr options: following clicks through ...` the first time Options opens.
- `vr options: VR tab button added to ...` and `vr options: VR page added to
  Options (18 rows ...)` each time Options opens.
- `vr options: VR tab opened` when the VR tab is clicked.
- `vr options: <row> -> <choice> (key=value)` and `settings: saved key=value in
  hole-vr.ini` on each click, then `controls: settings changed (...)`.
- `comfort: head inside a wall or object, view faded` / `head out of the wall
  again after N s`.
- `controls: physical crouch (...)` / `physical stand (...)`.

## Tests

- `tests/test_comfort.cpp`: vignette texture, levels, smoothing, real crouch
  and jump, aim smoothing, grip toggle.
- `tests/test_vr_options.cpp`: ini edit in place, menu rows, current choice.
