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
  - **Pistol grip**: the game's `MainHandPosition` is where its *wrist* goes
    (the gun's parts all sit ~8 cm forward/left/up of it), and the game's own
    hand does not wrap the grip, so neither is copied. The fist closes on the
    grip centre found from the gun's `GunParts_MagazineCatch` (top front of
    the grip): 2.5 cm back and 4.5 cm down the slanted grip, on the gun's
    centre line. Settings `hand_grip_*`.
  - **Scanned pistol grip** (`grip_probe.cpp`, default on,
    `hand_grip_from_gun_scan`): the parts report no size, so the gun is
    measured with rays when picked up: a 0.5 cm grid over its side in the gun
    frame, one ray from the right and one from the left per cell (~7300 rays,
    160 per frame, about half a second). That gives the gun's outline and
    both side surfaces. From it: grip bottom, the trigger guard (first thing
    right in front of the front strap), front/back strap, grip width and
    slant. The right hand is then placed from the avatar's measured knuckles:
    pinky->index up the grip axis, palm facing the grip's right side, middle
    knuckle one finger radius under the guard at the front strap, palm skin
    on the right side, pushed out if any palm point is inside. The thumb
    swings from the web around the back of the grip to the LEFT side and lies
    straight forward under the slide; its path is checked against the scan.
    Finger contact then tests the scan instead of traces. The log shows the
    measurements (`body: scanned ...`) and a clip check (`body: clip check`,
    deepest finger/palm point inside the gun) a few times per gun. If the scan
    fails the old placement below is used.
  - **Finger contact**: while holding a gun/slide/magazine, each finger joint
    curls only until its segment (offset by `finger_radius_cm`) touches the
    held object. The gun's parts have no collision in the game, so they get
    query-only collision that ignores every channel (nothing in the game can
    hit them) and are tested with `K2_LineTraceComponent` (one component,
    never the world). Magazines use boxes from their mesh bounds, and their
    collision is left alone so dropped ones still land.
  - **Palm seat**: a ray from outside the palm toward the grip centre finds
    the grip's surface and the palm skin (`hand_palm_skin_cm`) is put there,
    so the grip never ends up inside a big hand.
  - **Trigger discipline**: off the trigger the index finger's base joint
    turns it parallel to the barrel, lying along the frame.
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
