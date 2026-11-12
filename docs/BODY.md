# VR body: calibrated arms, grounded legs and hand grips

Code: `src/vr/body/`. `body.cpp` is the engine glue; everything else is pure
logic with tests (`tests/test_body.cpp`, `tests/test_grip.cpp`,
`tests/test_joint_limits.cpp`):

| File | What it does |
|---|---|
| `skeleton.*` | The whole pose in our own memory: local + component transforms, parent links, subtree updates |
| `finger_solve.*` | Fingers solved once to fingertip targets, within joint limits, keeping out of the object's outline |
| `grip_record.*` | Stored grips (HOLE-specific representation), the built-in pistol grip, file format, mirroring, clip depth |
| `grip_editor.*` | In-headset grip editor state machine |
| `joint_limits.*` | Human ranges for posed finger joints, and fitting old poses to them |
| `ik_math.*` | Vectors, rotations, two-bone solver |
| `arm_pose.*` | Reference-derived limb frames, bounded skin roll and attachments; also maps legs |
| `arm_posture.*` | Body-relative elbow posture and extension continuity; optional authored-grip preference |
| `body_calibration.*` | Five-stage setup, representative pose windows, validation and versioned profiles |
| `functional_reach.*` | Orientation-aware effective shoulder/reach fit, with an explicit prior fallback |
| `calibration_guidance.*` | One complete instruction card for each setup state |
| `wrist_calibration.*` | Constant controller-local forearm/palm basis from explicit neutral holds |
| `torso_pose.*` | Mode-aware pelvis placement and bounded spine fitting from the copied game pose |
| `leg_pose.*` | Animation/idle/contact ownership, room steps and terrain-pose recovery |
| `leg_world.*` | Reflected kick, movement, capsule-floor and ground queries |
| `stair_pose.*` | Multi-tread recognition and footprint selection, independent of game objects |
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

Use **VR Options > Calibrate body** with both hands free, then close the menu.
Holding the left stick click for two seconds also starts it. Instructions stay
in front of the headset, including when the normal wrist HUD is disabled.

1. **Height:** relax and look ahead. Stand for **Your own**, sit for **Seated**,
   or use the usual play posture for **Character**. Character does not infer
   physical posture from the tracking floor.
2. **Forward:** comfortably extend arms in front, palms inward, wrists in line
   with forearms. Choose a comfortable height; do not lock the elbows.
3. **Out:** open the straight arms comfortably to the sides, palms down and
   wrists in line. The opening motion is recorded before confirmation.
4. **Bend:** bend elbows so the hands rise comfortably. Keep elbows near their
   current places and wrists in line. Let palms turn naturally during movement;
   the fitter does not impose an endpoint elbow angle or palm gate.
5. **Forward check:** return to the earlier comfortable straight-arm forward
   pose with palms inward. This separate sample checks repeatability.

**Hold both index triggers for two seconds** when the pose is comfortable.
Recording happens during the hold. Haptic amplitude rises from 0% toward 90%;
an accepted step ends with a 100% pulse. Release both triggers before the next
step. Releasing either early or losing tracking cancels the current window
and stops the ramp, while accepted stages remain. Opening the menu cancels
setup and keeps the previous saved fit. A no longer confirms a pose.
There is no automatic pose recognition against guessed shoulder/elbow locations,
no overhead reach and no required opening/bend angle. Natural movement remains
in the data; position drift and angular spread do not reset capture. Finite,
tracked samples, robust outlier handling and model consistency still matter.
Each card gives one action; the progress bar shows recording, not how closely
the user matches a prescribed body. Stage/session timeouts remain bounded.

**Left X keeps the saved arms** (or existing default settings). Your own/Seated
can still record the corresponding relaxed height reference even with untracked
hands. Character or unavailable floor tracking keeps existing height references.
B/Y cancels. Incomplete/failed attempts never overwrite the saved profile.
Restricted movement must not be forced or interpreted as shorter anatomy.

The position-only reach model is `|p-S| = reach`, with wrist position `p` and
an effective head-relative shoulder pivot `S`. Endpoint windows and opening
motion have equal group weights. The centered position covariance identifies
which spatial directions are supported: a planar arc typically learns two;
two endpoints constrain only one. Supported components update the prior, while
null components retain it. `shoulder_observed_dimensions` records 0–3 and
`shoulder_measured` is true only for all three. A partial fit is conditional,
not a complete anatomical measurement. Spatial noise and leverage checks stop
one off-plane spike from creating a false third dimension. Every group retains
at least 85% inliers with radial RMS <=2 cm and accepted residuals <=4 cm;
poorly conditioned axes are demoted using bounded residual sensitivity.

Forward/out samples also learn one consistent controller-local forearm/palm
basis per hand. Palm cues define roll after projection onto the actual fitted
reach direction; they do not establish a required arm elevation. Nearly parallel
palm/reach directions cannot establish reliable roll. Inconsistent wrist data
returns to the forward step for a fresh pair, retaining the measured height.
A bad accepted forward sample cannot trap the user in repeated side-pose retries.

The separate elbow-motion model is `p = E + F*u`: effective elbow `E`, forearm
length `F`, and learned neutral-wrist forearm direction `u`. Neither the shoulder
nor total reach enters that solve. Robust motion samples, angular-density
weights and sensitivity checks allow useful limited motion while rejecting
stationary jitter. Novel samples preserve a short bend through a long pause.
After solving, upper length `|E-S|` and the sum are checked against the independent
reach fit. A fitted forearm is recorded separately as `forearm_measured`; both
segments are trusted for a future ratio prior only when the shoulder is also
fully supported. An uncertain bend proceeds with estimated proportions rather
than demanding repeated bend cycles. Uncertain old ratios are not recycled as
new measurements. The final forward check compares the learned reference and
per-sample wrist directions, allowing ordinary pose variation before commit.

These models estimate effective geometry; controllers do not track anatomical
shoulders, elbows or wrist centres. Approximate shoulder stability during opening,
approximate elbow stability during bending, neutral wrists and the frozen
controller-to-wrist offset remain assumptions. Correlated joint motion can
imitate a different segment length. A clean residual or successful repeat does
not prove anatomical accuracy. Confidence/provenance remains explicit.

A fitted split must also be one a human arm has: the forearm 40-53% of
shoulder-to-wrist reach (adult joint centres run about 41-47%; the avatar uses
47%). The owner's 2026-11-06 setup fitted the left forearm at 20.8 cm of 59.4
(35%) while the right arm kept 47% (29.4 of 62.6): an elbow that moved during the
bend. That short forearm put the left elbow too far from the shoulder, so the
left wrist bent past its range in roughly a quarter of empty-hand poses and the
two-hand pistol elbow crossed the chest. A new setup now rejects such a bend
fit (`PlausibleSplit`, that side keeps the prior split). A profile already saved
with one is drawn with 47% of the same reach (`UsableSegments`, used by
`MapArmTarget`); the file is not rewritten, and the log says so at load
("arm's saved split ... is outside a human arm's 40-53%"). Replaying the 11-07
and 11-08 logs with it: left wrist past its range by more than 10 deg went from
35 to 13 of 148 empty-hand pistol samples (worst 101 to 24 deg), 4 to 1 of 12
left gun-hand samples, 24 to 18 of 37 rifle-support samples (worst 90 to 48),
forearm turn past range 9 to 0, and the two-hand pistol support elbow no longer
crosses the head's midline in live play (5 of 13 before).

All tracked inputs share an OpenXR sample time and physical centimetres. Duplicate
timestamps do not count. Tracking loss clears the current sample window and confirmation,
preserves accepted stages, and requires both triggers released before a fresh hold; recentering, height-mode/posture changes or invalid
inputs invalidate the attempt. Height is recorded only in its relaxed step.
Standing and seated baselines remain independent; seated eye height never sets
standing body proportions or the standing crouch baseline.

Only a complete result atomically replaces `hole-vr/body-calibration.ini`; the
previous successful file is copied to `body-calibration.previous.ini`. Version6
adds observed shoulder dimensions and independent forearm provenance. Versions1–5
remain readable without rewriting their files. Pre-v6 segment lengths stay usable
in gameplay, but their older conditional confidence is not promoted to an independent measured prior.
V1/v2 height remains unclassified (`legacy_eye_cm`). Reference-only updates preserve
existing arms and the other height baseline. New wrist mapping activates only
after a successful new arm fit. Capture/runtime share the frozen physical wrist
offset; later manual settings apply as deltas without compounding corrections.
Saved object grips remain authoritative.

Calibrated shoulders use a body-yaw frame; looking down no longer adds a
backward pelvis displacement. The accepted shoulder estimate remains authoritative:
legacy clavicle reach assistance applies only to uncalibrated arms. Calibrated
arm segments do not stretch. The reference skeleton supplies limb axes
independently of the game's aiming animation, and the forearm-to-hand reference
includes intermediate twist bones.

Arm joint ranges (owner 2026-11-07: "the only time humans elbows go above our
head is when we reach straight up"; every arm joint needs a twist and bend
limit; "the human shoulder has a small amount of twist"). With
`body_arm_joint_limits=1` the solver adds, on top of the posture below:

| Joint | Range kept | What gives way past it |
|---|---|---|
| Shoulder (elbow height) | elbow at most 10% of the upper arm above the shoulder, plus 1.3 cm for each cm the hand is raised beyond a third of the upper arm, or reaches back over the shoulder | nothing: steep cost, also releases a plane carried through a straight arm |
| Shoulder (elbow height, hand on a gun) | elbow hangs: 45% of the upper arm below the shoulder with the hand at shoulder height, rising half a cm per cm of hand height (between 57% below and 10% above), plus the same raised-hand allowance; and the elbow stays behind the hand | as above |
| Shoulder (turn) | takes `body_shoulder_twist_share` (0.15) of the forearm's turn from thumb-up | |
| Forearm turn | -10 (palm toward the biceps) to 180 (palm away) deg; 90 = thumb up | the shoulder turns the arm (elbow swings up and out past palm-down) |
| Wrist, every hand (free hands and gun grips) | 70 back, 80 toward the palm, 20 toward the thumb, 35 toward the little finger, as an ellipse around the straight wrist | the elbow moves so the forearm comes from a reachable side |

Where no elbow can reach the height limit (a nearly straight arm, or a hand
high up), the limit starts 6% of the upper arm above the lowest elbow the
arm can reach, so the other ranges still choose among the low elbows.

The forearm's turn is read from the wrist's side-to-side axis and the bend
plane, so a bent wrist doesn't read as a turn. The 2026-11-07 run had the
shouldered rifle's trigger elbow up to 30 cm above the shoulder and the
support elbow 19 cm above: the pistol's straight-wrist preference applied to
the rifle grip too (now pistol grips only, key `BP_HG*`), and wrist relief
on a folded arm chose the top of the elbow circle. Ranges are limits, not a
pose: a hand in line with the resting forearm leaves the resting elbow alone.

The 09:21 run on the first ranges (872c72a) kept every elbow at or below
shoulder height unless the hand was raised, but with the rifle at the cheek
the trigger hand sat only 13-16 cm ahead of the measured shoulder, so the arm
was folded almost shut. About a quarter of those samples put the trigger
elbow out at shoulder height, ahead of the hand. The wrist measure then used
atan2 side angles, which flip once a bend passes 90 deg, so a forearm
arriving from the back of the hand read as a small bend and made that pose
look cheap. The wrist bend is now measured as the whole angle between the
forearm and the hand, and a gun hold keeps the elbow hanging. Replaying that
run's 78 rifle trigger samples through the solver: no elbow out at shoulder
height (13 before). With the hand that close the wrist still bends past its
range when the rifle is at the cheek; the gun hides most of it.

Why the shouldered trigger wrist cannot be fixed by the elbow alone (same
replay): the recorded rifle grip (`BP_AR_C.grip`) lays the hand along the
barrel, as if the elbow were behind it at hand height. With the hand 13-16
cm ahead of the shoulder the elbow circle lies across the forearm's line, so
every elbow that is not inside the chest leaves the wrist 40-58 deg past its
range (a hanging elbow bends it toward the little finger; out at shoulder
height bends it back). Lifting the shoulder 6 cm only halves it. Tilting the
hand on the grip about 35 deg wrist-down (knuckles fixed), the way rifle
shooters hold the grip with a low elbow, keeps the wrist in range in 17 of
19 aimed samples with the upper arm hanging straight down, and 11 of 78
rifle trigger samples past 10 deg instead of 43: a grip-editor pose, so the
owner's to make. The drawn rifle hand also sits 10-11 cm from the free hand's
wrist (`body fit: ... controller offset`), the pistol hand 5-6 cm.

Support hand under the handguard (`posture_input.support_hold`): when every
elbow on the circle is at least 35-60% of the upper arm in front of the
shoulder, the midline guard and the medial preference fade, so the elbow can
turn in under the gun. Held at the guard's edge it had twisted the forearm
10-30 deg past its turn range (25 of 37 support samples past 10 deg; 11
after, all with the rifle held low near the body). The forearm and wrist
then share what neither range can hold: in that run the hand on the
handguard sat about 20 deg off any pose both ranges allow. Grip hands keep
the guard; a pistol's elbow must not cross the chest to straighten the wrist.

The forearm skin's twist runs from `body_forearm_twist_at_elbow` (0) of the
hand's turn at the elbow end to `body_forearm_twist_at_middle` (0.5) at the
mid-forearm twist joint and all of it at the wrist; the seam of that split
sits opposite a thumb-up forearm (palm facing outward), measured from the
skeleton's reference pose (logged: `body: <side> forearm in the skeleton's
reference pose is turned ...`). The wrist display keeps
`body_forearm_roll_share` of the turn, as before.

Without joint ranges (`body_arm_joint_limits=0`), free-hand elbow posture
follows a body-relative preference and continuity, not the controller's palm
orientation: turning a wrist does not pull the elbow around the arm. Explicit
authored grips may opt into a wrist/roll preference.
For neutral forward reaches the solver uses an outward interval and guards the
actual elbow against the torso midline. Cross-body and overhead reaches relax
this constraint. The default bent-arm preference keeps elbows nearer the sides,
with soft costs for excessive flare and unnecessary medial placement. Moving an
elbow forward no longer removes its medial cost. Deeply bent low-hand poses
retain room for lateral bend so tucking does not merely drive the elbow behind
the back. Authored rifle support grips receive soft wrist-bend relief, while
free-hand wrist turning still does not choose the elbow. An inward hand target can still
require a medial elbow; the solver preserves reach instead of pinning the elbow
to the shoulder line. Rest style is a soft cost throughout the connected body
interval, not a second hard angular window that can eject a valid prior plane.
Near extension it transports the last useful elbow plane;
retraction returns to the posture preference at a bounded rate. Exact
controller/grip hand targets and accepted lengths remain authoritative. An
unreachable target uses the existing two-bone reach clamp. This is inferred
elbow posture, not elbow tracking or a torso collision simulation.

Forearm roll uses a bounded principal target, applies each configured bone
share first, then smooths each resulting bone rotation independently. No turn
count accumulates and no fractional share multiplies an unwrapped angle. A
175-degree software seam limit and short voting window reduce sign chatter;
bone roll is limited to 360 degrees/second. Fractional roll necessarily has a
branch transition across a full turn; persistent crossings settle gradually,
rather than retaining a history-dependent half-turn offset. These are software
stability limits, not measured anatomical joint limits. Twist skin joints keep
their reference fraction along each calibrated segment, with absolute placement
and total roll for sibling branches as well as intermediate bones. The final
hand transform remains authoritative. Existing calibration files are unchanged.

In Character and Seated modes, the pelvis retains the game's current stance.
Fitting shoulders below the eyes adjusts the spine within bounds rather than
lowering the whole pelvis and producing a permanent visual squat. Game crouches
remain intact. Your own mode can additionally lower the pelvis for tracked
physical crouching relative to the game camera. Each frame starts from the
copied animation, so these adjustments do not accumulate.

Walking and sprinting retain source gait timing and knee planes. Airborne
movement and unavailable movement state retain the game animation; kicks bypass
the contact planner and have a separate mirrored/aimed pose path. Grounded idle feet need stable, nearby floor support
before planting; room steps retain their landing target until completion.
Moving supports use component-relative anchors. Tracking/origin discontinuities,
teleports and long frames clear stale plants.

Floor probes start near the collision capsule's floor, independently of the
animated feet. An idle foot already raised onto a table by ALS terrain IK cannot
authorize the tabletop as ground. Its stance and knee plane recover toward the
reference stance. Excessive upward source-pelvis displacement is corrected only
with matching idle terrain-corruption evidence; normal crouching and kicks keep
their animation. No ALS variables or gameplay collision are changed.
Stair recognition requires at least three flat treads with consistent risers,
then fresh centre/heel/toe support for each foot. Qualified adjacent treads can
place feet up to 30 cm above/below the capsule floor; isolated ledges, ramps and
tabletops do not authorize the wider range. Native gait determines swing versus
support, with toe clearance and independent planted feet. Supporting feet may
lower the pelvis within bounds to regain reach. The scan is cached and spread
over frames with a hard nine-trace limit per frame. Unqualified geometry falls
back to ordinary floor handling/animation. Actual stair collisions, stopping,
turning, ascent/descent and all height modes still need headset validation;
see [PLAN.md](PLAN.md#stairs-and-foot-placement-headset-validation-pending).

Right-stick kick selects the leg from headset aim at the press: right of the
body uses the right leg; left/centre uses the left. Side and aim remain fixed
until the native kick ends. The right action mirrors the game's left kick and
mirrors its supporting right leg into the left, using each leg's reference
axes and lengths. The added aim correction follows the native blend, so the
windup does not immediately rotate a standing leg to the impact direction.
The native trace's extra 30-degree pitch is compensated; a narrowly scoped
socket override supplies the selected rendered hip while preserving native
damage, stun, impulse and audio. Missing hooks, bones or fresh body data fall
back to the native kick. This adds no tracked feet or independent kick animation.

Every ten seconds, `body fit:` distinguishes target gap from controller-relative
position/rotation error and labels applied forearm roll as a mean absolute
angle. With `body_fit_log`, `body arm sample` also records one signed snapshot
per hand every five seconds: raw/target/lower-arm/skin roll, wrist bend, candidate
count, constraint flag, lengths and simultaneous joint geometry. Shoulder is
head-relative; elbow/wrist are shoulder-relative; all use the body-yaw frame
and cm. The hand quaternion is also body-relative. `real-wrist`/`real-hand-q` are where the
controller alone puts the hand (the free hand's wrist, shoulder-relative), so a drawn hand
on a gun can be compared with the real one, and `head-pitch/roll` (degrees, up and right
ear down positive, body-relative) shows a head leaning into the sights, which carries the
head-relative shoulder with it. `body legs:` reports bypass and terrain-recovery counts
plus read-only ALS offsets. These describe solved bones, not GPU skinning.
Performance counters include `body calibration`, `body legs and ground`, and
`body arms and fingers`.

## Individual hand contacts

Empty hands now have separate requested, contact-resolved and displayed poses.
`hand_walls` advances a bounded damped spring toward the tracked controller and
sweeps the rig-mapped palm box through the game's collision geometry
(with a fist sphere fallback when the rig is unavailable). Blocking surfaces
remove inward motion while allowing a checked tangential slide. Virtual force,
torque, linear/angular speed and integration steps are bounded. A stationary
proxy still queries, so an approaching object cannot inherit an old clear result.
Recovery from a tracking jump or overlap checks the head-to-target path before
placing the proxy. Failed queries hold a previously valid proxy; an unverified
initial placement skips that body's arm solve. Actual controller input is unchanged.
This is a collision-constrained kinematic spring, not a Chaos rigid body, and
does not apply forces to props.

`finger_world` queries the anatomically and temporally bounded candidate pose,
including all fifteen phalanx interiors. Contact acquisition and overlap recovery
share a strict 32 logical-sweep budget per hand per frame. The current
`wall_shape=nearest` mode runs both game collision and visible-shape traces
for each sweep. Logs count those engine calls separately. Redundant broad, displayed and
motion casts are coalesced when they describe the same checked feature. A validated
rigid static-mesh component transform can transport a finite contact patch; it
never renews the original geometry observation's 50 ms lifetime. Unsupported,
deforming, dead or malformed components require real casts. Unknown observations
are distinct from clear space and identify the affected fingers.

`finger_contact` arrests movement along the requested joint trajectory. Stationary
world pressure cannot accumulate arbitrary curl, spread or twist. Each digit can
stop independently, while movement that clears its contact remains possible.
Smoothing and the shared angular budget precede contact resolution; no later
unconstrained blend changes the accepted pose. Palm flattening no longer overrides
the user's squeeze intention. Self-contact allows bounded cooperative opening
and small local yielding while certifying separation throughout each accepted
path. Each pair of finger segments uses its own travel distance when checking
a movement. This lets a fingertip uncurl while two knuckles remain close.
World pressure that the joints cannot resolve is handed
to the rigid wrist response.

The shipped player hand uses a fitted palm box and individual segment radii
for world queries. Self-contact uses the measured widths and depths of the
fingers, including offsets where bones sit off-center in the skin. The shared
web at the base of adjacent fingers belongs to one continuous surface and
is excluded from their self-contact checks. The reference joint layout and
bone lengths identify the fitted mesh. Other rigs use `hand_finger_radius_cm`
for empty-hand collision.

`hand_rigid_contact` projects fresh, frozen capsule supports onto a bounded wrist
translation, constrained by arm reach. This includes immutable finger roots: in
the actual WhiteMan rig, the thumb CMC capsule extends 1.104 cm beyond the former
palm-only stopping plane. No amount of finger curling can clear that root.
`hand_walls::ConstrainFinal` checks the resulting mapped grip and the final IK wrist;
the arm is solved again from one copied temporal state, rather than moving only
the hand bone. Accepted supports prevent repeated spring re-entry for only the
remaining lifetime of their original observations.

Changed final finger geometry is checked against actual world collisions using
the remainder of the same frame budget. If that verification cannot finish, the
corrected wrist and finger pose are staged for the next frame's query. A
controller withdrawal or a change of grip cancels stale work. Unknown
geometry cannot authorize a new drawing. A previous full wrist/finger pose may
hold without renewing its evidence; an observed obstruction rejects even that
fallback. If no checked drawing or unchanged prior hold is available, the hand
is temporarily suppressed rather than exposing the freshly copied game-animation
pose. Frozen supports remain constraints when a translated sample leaves a
finite patch, so dropping a patch cannot falsely erase unresolved depth.

These are discrete capsule/box checks against the game's collision shapes, not
triangle-accurate skin collision or exact continuous detection for rotating
capsules. Endpoint verification prevents a root correction from placing a finger
inside a previously unobserved second obstacle; it does not certify every point
on an arbitrarily fast rotational path. Simple collision meshes may differ from
visible walls. The system is kinematic and does not apply forces to dynamic props.

`thumb_pose` generates separate open, wrap and pinch-like thumb intentions,
combining base flexion, side motion and pad-facing axial rotation. Signs follow
the reference rig geometry for mirrored or rolled bones. These are engineering
curves, not claims of measured human motion. The open base direction is fitted
to the rig's palm tangent plane; the thumb's neutral tip no longer projects
7.308 cm through the palm-facing direction. Non-thumb open intentions remove
reference-pose curl and the legacy idle-curl bias. Saved grips retain their authored
thumb and trigger poses.

Held objects, reserved interactions, grip editing and calibration bypass empty-hand
contacts. Tracking loss, player/body changes, origin changes, teleport, long frame
gaps and settings changes reset relevant contact history. IK13 dual-trigger calibration and AR support-hand wrist continuity
remain in this source baseline. Performance counters separately
report proxy collision, finger geometry queries and finger contact solving;
`hand contacts` logs actual reflected engine calls, cache reuse, incomplete
observations, held/rejected frames, final residuals and root corrections.

Settings added for this path: `hand_individual_finger_collision=1`,
`hand_finger_query_budget=32`, `hand_finger_contact_refresh_ms=50`,
`hand_thumb_opposition=1`, `hand_follow_hz=10`, `hand_follow_damping=1`,
`hand_follow_max_force_n=65`, `hand_follow_max_speed_cm_s=700`,
`hand_surface_friction=5`, and `hand_recovery_distance_cm=40`.
`hand_wall_collision` remains the overall collision switch and
`hand_fingers_rest_on_walls` the finger-contact switch.
Disabling individual collision restores the older shared-plane response;
`hand_finger_drag` and automatic `hand_press_flat_on_walls` apply only to that
older mode. Individual contacts preserve the spring proxy's palm orientation
instead of applying an unchecked palm translation/rotation after collision.

## Regression checks

Configure with `-DHOLEVR_BUILD_TESTS=ON`, build, then run `ctest --test-dir <build>
--output-on-failure`. Tests cover synthetic calibration geometry and rejection,
tracked-input boundaries with a simulated OpenXR runtime, saved-profile/scaled
skeleton integration, limb-frame invariants, foot contacts/table recovery,
kicks, airborne movement, height-mode/profile migration, torso stance, and
the existing grip/menu ownership behavior. Arm regressions include wound-history
versus fresh half-share rotations, noisy roll seams, mirrored neutral elbows,
cross-body/overhead reach, extension/retraction and calibrated shoulder/skin-joint
placement through an animated clavicle and intervening twist parent.
Headset testing remains necessary for comfort, actual skinning, and game-world
contact feel. Hand regressions additionally cover fresh stationary queries,
independent digits, joint response, mirrored thumb opposition, bounded spring
following, wall/corner sliding, moving obstacles and tracking recovery, including
the query-to-solver-to-display sequence. This implementation does not add tracked feet or a force-driven
whole-body ragdoll.

## Settings

New keys (an old key keeps its old value in existing ini files, so changed
defaults get new names): `body_turn_threshold_deg`, `body_turn_toward_hands`,
`body_arm_scale_override`, `body_max_arm_stretch`,
`body_max_arm_stretch_unmeasured`, `body_shoulder_below_eyes_cm`,
`body_shoulder_behind_eyes_cm`, `body_forearm_roll_share` (now only the
wrist display's share of the hand's turn), `body_arm_joint_limits`,
`body_shoulder_twist_share`, `body_forearm_twist_at_elbow`,
`body_forearm_twist_at_middle` (these two replace
`body_forearm_twist_bone_share`, which is no longer read),
`body_elbow_relieves_wrist`, `hand_grip_records`, `hand_joint_limits`, `hand_finger_radius_cm`,
`body_fit_log`. Version 1 keys (scan, palm seat, contact, grip offsets) are
ignored.
