# VR body: full-body IK and hand poses

Code: `src/vr/body/` (`body.cpp` engine glue, `ik_math.*` and `hand_pose.*` pure
logic with tests in `tests/test_body.cpp`). Inputs come from `src/vr/hands.h`
(owned by the controls work): hand/head poses, `GetGrab()` (what each hand
holds and where the hand should sit) and `GetFingers()` (trigger/grip/touch).

## How it works

- The game's `BodyMesh` (ALS animation, game camera on its head) keeps running
  but is hidden, forced to LOD0 and to refresh bones while hidden.
- A `PoseableMeshComponent` with the same mesh and materials is added to the
  player and attached to `BodyMesh`. Every game frame it copies the animated
  pose, then we overwrite:
  - **Pelvis**: yaw follows the headset with a dead zone (`body_turn_deadzone`),
    catching up while moving; drops when you crouch; shifts when you lean past
    `body_max_lean_cm`.
  - **Spine**: bends (spread over all spine bones) so the neck sits under the
    real head.
  - **Arms**: analytic two-bone IK from the animated shoulder to the hand
    target, elbow bending in the arm's own hinge plane, pole down/out/back.
    While a hand holds something (`GrabKind`) the target is the held point.
  - **Hands**: the avatar hand's frame is measured from its finger bases
    (index/middle/pinky), so any skeleton maps onto the controller grip axis
    and palm direction without per-mesh numbers.
  - **Fingers**: reset to the reference pose, then curled about the palm
    hinge axis by `hand_pose::Target()` (gun grip with trigger discipline,
    trigger pull, slide pinch, magazine hold, free hand from the sensors).
  - **Legs**: two-bone IK keeps the animated feet planted.
  - **Head**: scaled to zero so it never blocks the view.
- Clothing/hair meshes that used `BodyMesh` as their leader pose are switched to
  the copy. F9 (or `body_ik=0`) puts the game's own body back.

Bones are found by name (UE4/UE5 mannequin and Mixamo spellings); the full bone
list is logged on first use (`body:` lines in `hole-vr.log`).

## To check in the first playtest

- Log: bone list, `skeleton ... arms L1 R1 ... fingers L1 R1`, `VR body on`.
- Hands sit on the controllers and the right hand wraps the pistol grip;
  index finger rests along the frame until you touch the trigger.
- Elbows bend naturally (down and out); no twisted wrists.
- Fingers curl the right way (if they bend backwards, the palm sign is off).
- Crouch in real life: the body crouches, feet stay on the floor.
