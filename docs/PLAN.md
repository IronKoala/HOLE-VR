# HOLE VR roadmap

Agreed with the project owner (Mtthew), who directs features and playtests.

## Target

- HOLE on Steam, `HOLE-Win64-Shipping.exe`, Unreal Engine 5.5 (CL 40574608), DirectX 12.
- Headset: Steam Frame through SteamVR (also Steam Link). OpenXR is the API.
- The engine ships with `FullBodyIK`, `IKRig`, `PBIK`, `ControlRig`,
  `EnhancedInput` and `UMG` modules compiled in, so body IK and input can use
  the engine's own systems instead of bundling our own.

## Steps

1. **Foundation** (in progress): loader DLL, engine discovery, class dump,
   logging, settings file. Next in this step: hook `UObject::ProcessEvent` and
   the game's tick so later steps can run code every frame.
2. **Into the headset**: OpenXR session on SteamVR, enable UE's stereo rendering
   path and feed it headset poses, submit D3D12 eye textures. Correct world scale.
   First playtest milestone.
3. **Native controls**: tracked hands drive aiming directly, smooth or snap
   turning, movement relative to head or controller. Then physical weapon
   handling and full-body IK (see below).
4. **Polish**: menus and HUD on world-space panels, fixes for effects that break
   in stereo, performance, installer.

## Weapon interaction (must-haves)

- Manual reloads: drop magazine, take a fresh one from a body pouch, insert.
- Slide racking and bolt pulls, moved along the weapon's axis, not the head's.
- **Partial rack** for a chamber check: pull the slide part way, see the round,
  let it return without ejecting.
- Two-handed support grip that follows moving parts (pump fore-end etc.).
- Hand poses per weapon and per grip point; open/closed hand from controller input.
- Holsters / body slots with calibration.

## Full-body IK

Use the engine's own `FullBodyIK` / `IKRig` (PBIK) solvers on the player mesh,
driven by head and hand targets, with leg animation from locomotion. Body
calibration (height, arm length) feeds the solver.

## Comfort and settings (inspired by TheFreeMike's F.E.A.R. VR)

- Movement direction: head or controller (fall back to head if the hand loses tracking).
- Turning: smooth (default 180 deg/s, adjustable) or snap.
- Optional vignette while moving/turning.
- Separate toggles for damage camera motion and explosion/effects shake.
- Handedness, movement stick choice, remappable layouts saved per controller and hand.
- Calibration pages: height, hand fit, holster/pouch positions, grip and sight
  alignment, each with its own reset.
- Wrist HUD toggle, subtitles size, tutorial/hint cards.
