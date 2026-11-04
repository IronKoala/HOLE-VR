# VR body: full-body IK and hand grips (version 2)

Code: `src/vr/body/`. `body.cpp` is the engine glue; everything else is pure
logic with tests (`tests/test_body.cpp`, `tests/test_grip.cpp`,
`tests/test_joint_limits.cpp`):

| File | What it does |
|---|---|
| `skeleton.*` | The whole pose in our own memory: local + component transforms, parent links, subtree updates |
| `finger_solve.*` | Fingers solved once to fingertip targets, within joint limits, keeping out of the object's outline |
| `grip_record.*` | Stored grips (Into the Radius 1 format), the built-in pistol grip, file format, mirroring, clip depth |
| `grip_editor.*` | In-headset grip editor state machine |
| `joint_limits.*` | Human ranges for posed finger joints, and fitting old poses to them |
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
- Magazine: no built-in hold (the owner poses it in the editor). Until a
  grip is saved the hand stays as it is on the controller; the editor starts
  from exactly that hand. (`body_mag_builtin_hold=1` brings back the old
  guessed hold.)
- Slide, charging handle, rifle handguard: no built-in grip yet; the hand
  sits on the controller with a simple curl until a grip is made in the
  editor.
- Slide: three holds, like Half-Life: Alyx, each posed and saved on its
  own (`<Class>.slide-rear-flat|rear-pinch|front-flat.ini`). The hold is picked once, when
  the hand closes on the slide: fist over the FRONT half of the slide
  = **front**; fist within 6.5 cm of the rear sight, or of the line from it 0.64 cm back along the slide (`hand_slide_pinch_radius_cm`, `hand_slide_pinch_back_cm`; same as the controls grab zone)
  = **rear pinch**; else **rear overhand**. The slide is measured once per
  gun with line traces down onto the slide's own mesh parts (children of
  `GunParts_Bolt(Slide)`, made query-only traceable): rear end, front end
  and rear sight (highest point of the rear 3.5 cm). `DefaultADSPoint` is
  NOT the rear sight: it is the aiming eye point, 22 cm behind the slide.
  `body::SlideMarks()` gives these to controls for its grab zones.
  Until a hold is saved it uses a built-in pose (`hand_slide_builtin_holds`):
  front and rear overhand = flat hand with the thumb's pad on the slide;
  pinch = fist with the thumb straight out and the side of the index
  finger's middle knuckle on the slide. The hand keeps the controller's angle
  at the grab and is moved so that contact point touches the slide (a line
  trace against the slide part, which is made query-only traceable). A hold not posed yet keeps the
  hand where the controller grabbed, carried along with the slide. To pose one, grab
  the slide that way, then open the editor (hold left Y 1 s). A hold not
  posed yet uses an old `<Class>.slide.ini` if there is one. The log line
  `slide grab: <hold> hold` shows which was picked and the slide's span.
- Object frames: gun = X barrel, Y right, Z up at `MainHandPosition`;
  slide/handguard = gun frame at the point held; magazine = the magazine
  actor.

The log shows, for each grip: how far each fingertip ended from its target
and how deep anything sits inside the object's outline
(`body: grip ... clip check: deepest 0.2 cm ...`).

## Grip editor (in the headset)

Hold **left Y for 1 s** while holding the pistol (or a magazine in the
left hand): the editor is on and the OTHER controller becomes the editing
hand (its buttons stop reaching the game, and the gun won't fire).
While the editor runs, both hands and the held gun/magazine/slide stay
frozen where they were when it started; only the editing controller's grab
point follows the real controller (controls freezes the game's gun, slide
and grabs on its side while `hands::EditorHand() >= 0`).

- **Finger joints**: a small ball sits on every finger joint (15: base,
  middle and tip joint of each finger). Squeeze grip on one and turn the
  controller: that joint turns with it the way a real joint can (see
  "Joint ranges" below), the rest of the finger follows. Once a finger has
  been turned it keeps exactly those joint angles (saved as
  `pose_<finger>=` lines, four numbers per joint); it is no longer solved
  to a fingertip target.
- **Magazine**: a big ball sits on the magazine. Squeeze it and move/turn
  the controller: the magazine moves and turns in the hand (the hand stays
  where it is on its controller while editing).
- **Whole hand** (gun grip and slide holds): the ball 7 cm off the back of
  the hand (`hand_editor_handle_cm`) moves and turns the whole hand on the
  gun.
- The editing hand's grab point is a small ball just ahead of the
  controller; when that hand holds a gun it sits in front of the barrel
  (`hand_editor_gun_hand_reach_cm`, default 22). Joint balls are 2.6 cm
  across (`hand_editor_joint_ball_cm`) so they show through the fingers.
- The ball a squeeze would take grows and the controller ticks when it
  changes (`hand_editor_show_grab_points`). Pull the real trigger to edit
  the "resting" and "pulled" index poses.
- **Tap Y** to save, **click the left stick** to undo to the saved grip,
  **hold Y 1 s** to save and finish. Grips are saved per object class in
  `hole-vr/grips/<Class>.<grip|mag|slide-rear-flat|slide-rear-pinch|slide-front-flat|support>.ini`.

## Joint ranges

The owner asked for anatomically accurate posing (2026-11-04: "humans cant
bend our last 2 finger joints side to side"). `joint_limits.*` splits each
joint's turn about the joint's own axes (the same axes the fingertip solver
curls about, measured from the skeleton, so it does not depend on how the
bones are rolled) into curl, sideways bend and roll, and keeps each in a
human range:

| Joint | Curl (deg, 0 = straight) | Sideways | Roll |
|---|---|---|---|
| Finger base (MCP) | -30 to 90 (middle, ring 95; pinky 100) | index 22, middle 18, ring 18, pinky 28; 35% of that when fully curled | 10 |
| Finger middle (PIP) | -5 to 105 | none | none |
| Finger tip (DIP) | -15 to 90 | none | none |
| Thumb base (CMC) | -30 to 50, from the reference pose | 35 | 20 (turning toward the fingers) |
| Thumb middle (MCP) | -15 to 65 | 10, half when curled | 8 |
| Thumb tip (IP) | -25 to 80 | none | none |

Curl ranges are anatomical; the reference pose is already a little curled,
so each range is shifted by the bend measured there (logged once per hand:
`body: <hand> hand joint limits on: reference pose bends ...`). The tip
joint's bend can't be measured (the chain's fingertip carries on from the
middle bone) and counts as straight.

- **Editor**: a dragged joint takes only the part of the controller's turn
  the joint can make (a middle or tip joint follows just the curl), and
  stops at the end of its range with a light tick on the editing
  controller.
- **Saved poses** made before the ranges are fitted when loaded: every
  joint is brought into range and the knuckle (and for the thumb, its base)
  makes up what a middle or tip joint can't do, so the finger's joints and
  fingertip stay as near where they were posed as the ranges allow. The log
  line `body: grip <key>: posed fingers fitted to human joint ranges, ...`
  says how far each finger moved and what was taken out. The file is only
  rewritten when the grip is saved in the editor; the first time a file is
  fitted, a copy of it as posed is kept in
  `hole-vr/grips/before-joint-limits/`. A pose already in range is never
  touched.
- Grips that aren't posed joint by joint (the built-in pistol grip) are
  solved as before: the solver already only curls the middle and tip
  joints.
- `hand_joint_limits=0` turns it all off (free three-axis joints, saved
  poses used exactly as stored).

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
`body_elbow_relieves_wrist`, `hand_grip_records`, `hand_joint_limits`, `hand_finger_radius_cm`,
`body_fit_log`. Version 1 keys (scan, palm seat, contact, grip offsets) are
ignored.
