# VR body: full-body IK and hand grips (version 2)

Code: `src/vr/body/`. `body.cpp` is the engine glue; everything else is pure
logic with tests (`tests/test_body.cpp`, `tests/test_grip.cpp`):

| File | What it does |
|---|---|
| `skeleton.*` | The whole pose in our own memory: local + component transforms, parent links, subtree updates |
| `finger_solve.*` | Fingers solved once to fingertip targets, within joint limits, keeping out of the object's outline |
| `grip_record.*` | Stored grips (Into the Radius 1 format), the built-in pistol grip, file format, mirroring, clip depth |
| `grip_editor.*` | In-headset grip editor state machine |
| `ik_math.*` | Vectors, rotations, two-bone solver |
| `hand_pose.*` | Simple finger curls for empty hands and objects without a stored grip |
| `grip_probe.*` | The old ray scan of a gun, no longer used at runtime; kept as a measuring tool |

Version 1 tried to work the grip out from the gun's shape every frame
(scans, finger traces, palm seating). It never matched the reference photos
and cost 13-17 ms a frame. Version 2 follows what shipped VR shooters do
(research: "How six VR games put hands on guns and swords", and the
Half-Life: Alyx write-up in `research/`).

## Speed: one read and one write per frame

Every `Get/SetBoneTransformByName` on the poseable mesh rebuilds the whole
skeleton (~40 us), and version 1 made ~300 of them a frame. Now:

1. `CopyPoseFromSkeletalComponent` (the game's animated pose),
2. one memory copy of the bone array (`BoneSpaceTransforms`, found once at
   startup by scanning the component and proving the engine reads it:
   log `body: fast pose path on (bone array at +0x...)`),
3. all IK in `skel::Pose` (about 10 us of maths),
4. one memory copy back of the bones that changed, plus one engine call so
   the renderer picks it up.

If the array can't be found the slow per-bone calls are used and the log says
so. The `perf:` line has `body tick`, `body engine read+write`, `body solve`
and `body grip solve (once per grip)`.

## Hands on objects: stored grips

A grip record (`hole-vr/grips/<class>.<kind>.ini`, e.g. `BP_HG_C.grip.ini`)
holds, in the object's frame: the middle knuckle position, the
wrist->knuckle and pinky->index directions, and a target point per fingertip
(thumb, index off the trigger / resting on it / pulled, middle, ring, pinky),
plus an outline of boxes for the clip check. When the hand takes the object,
the fingers are solved to the targets once (a few ms, cached per record) and
held. Only the index finger is live: it blends between its three stored
states from trigger touch and the analog trigger.

- Pistol hand (2026-11-02, owner's photos): the hand runs in line with the
  barrel (about 7 deg off), so with a straight wrist the forearm sits right
  behind the gun; the web of the hand sits low behind the back strap, under
  the tang (a `tang` box in the outline). Saved grips with the first hand
  placement get the new one on load, keeping their fingertips. Holding the
  pistol, the elbow is placed where the wrist bends least
  (`body_gun_wrist_straight`). The log line `grip ... hand:` gives the
  forearm-to-barrel angle and how deep the web sits in the gun.
- Pistol grip: built in (`grip_record::DefaultPistol`), from the owner's
  reference photos and the measured pistol: high grip, middle finger just
  under the trigger guard, thumb forward along the LEFT side of the frame,
  index straight along the right side above the guard. The thumb's side is a
  stored choice, so it can't flip.
- Magazine: built in (`DefaultMagRecord` in body.cpp), from the owner's
  photos: the magazine lies diagonally across the palm, base low in the heel
  of the hand, middle/ring/pinky wrapped round the lower body, index straight
  up the front to the top round, thumb up the side. It is placed from the
  magazine's measured outline (log line `body: magazine ...`) and tuned in
  the editor like any grip (saved as `<MagazineClass>.mag.ini`).
- Slide, charging handle, rifle handguard: no built-in grip yet; the hand
  sits on the controller with a simple curl until a grip is made in the
  editor.
- Object frames: gun = X barrel, Y right, Z up at `MainHandPosition`;
  slide/handguard = gun frame at the point held; magazine = the magazine
  actor.

The log shows, for each grip: how far each fingertip ended from its target
and how deep anything sits inside the object's outline
(`body: grip ... clip check: deepest 0.2 cm ...`).

## Grip editor (in the headset)

Hold **left Y for 1 s** while holding the pistol: the editor is on and the
left controller becomes the editing hand (its buttons stop reaching the game,
and the gun won't fire). Squeeze grip near a fingertip and move it: that
finger re-solves live. Squeeze near the palm: the whole hand moves and turns
with the controller. While editing, a small ball sits on each fingertip
and on the palm, and one on the editing controller; the ball a squeeze
would take grows and the controller ticks when it changes
(`hand_editor_show_grab_points`). Pull the real trigger to edit the "resting" and
"pulled" index poses. **Tap Y** to save, **click the left stick** to undo to
the saved grip, **hold Y 1 s** to save and finish. With a magazine, the slide
or a handguard in the left hand, the same buttons edit that grip and the
right controller does the moving.

## Body

- **Arm length is measured once**: stand straight, arms straight out to the
  sides, hold the left stick click for 2 s, keep the pose through three
  buzzes. Saved in `hole-vr/body-measure.ini`. No more live arm growth.
  Before measuring, arms may stretch up to 15 %; after, 4 %
  (`body_max_arm_stretch*`). Stretch moves bones, never scales them.
- **Shoulders from the head** (Blade & Sorcery): one head unit (eye height /
  7.5) below the eyes and half a unit behind, along the head's level facing.
  The pelvis moves and the spine bends to put the avatar's shoulders there.
- **Body turn**: 25° threshold (Into the Radius 1 / Final IK), leaning 30 %
  toward where the hands are, catching up while walking.
- **Collarbones** move only when reaching (from 85 % of arm length, up to
  25°, smoothed).
- **Elbows** hang down/out/back and follow the hand's pinky side (palm down
  swings the elbow out); if the wrist would still bend past ~55° (flex) or
  ~25° (sideways), the elbow swings further, up to 60°.
- **Forearm roll**: the hand's twist about the forearm is split: forearm bone
  50 %, forearm twist bone 100 %, then the hand gets its exact rotation, so
  the wrist never twists.
- Legs keep the animated feet planted; head hidden; fixed player height
  unchanged.
- `body fit:` log line every 10 s: arm scale and whether it was measured,
  reach as % of arm, gap between hand and target, wrist bend, forearm twist,
  elbow swing, body yaw lag.

## Settings

New keys (an old key keeps its old value in existing ini files, so changed
defaults get new names): `body_turn_threshold_deg`, `body_turn_toward_hands`,
`body_arm_scale_override`, `body_max_arm_stretch`,
`body_max_arm_stretch_unmeasured`, `body_shoulder_below_eyes_cm`,
`body_shoulder_behind_eyes_cm`, `body_forearm_roll_share`,
`body_forearm_twist_bone_share`, `body_elbow_follow_hand`,
`body_elbow_relieves_wrist`, `hand_grip_records`, `hand_finger_radius_cm`,
`body_fit_log`. Version 1 keys (scan, palm seat, contact, grip offsets) are
ignored.
