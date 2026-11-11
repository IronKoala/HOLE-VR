# Combined build: hands4-8914320

This checkout retains calibration, hand physics, weapon vise, watch and holster
updates together. The hand patch starts from the installed November 11 source
`8914320dd273f209061c179bf634967973dd4976`. This includes the newer arm limits,
reload poses, visible-wall queries, own-gear exclusions, scope and audio updates.

| Component | Included source |
| --- | --- |
| Elbow posture | Current joint limits, rifle elbow preference, support relief and shoulder twist |
| Calibration | IK13: dual-trigger capture/haptics over IK12 comfortable-pose fitting and v6 provenance |
| Hand physics | Hands4: firm object contact, cooperative self-contact, fitted geometry, fresh queries and final wrist verification |
| Vise | V4, retained-ammunition fix, audit1 patch `2f74611ea01ca0e5997b2d4514f01c876a78e1a1a9839f11bc8e077b5679f947` |
| Watch | Model v3/integration v1, runtime filename reflection fix |
| Holster | Model v4/runtime v5 |

Hold both index triggers for two seconds to record a comfortable pose. Haptics
rise from 0% to 90%, with 100% only after acceptance. Release both before the next pose.
Early release cancels the current window. Left X keeps arms; B/Y cancels.
Capture no longer depends on guessed shoulder/elbow positions or a stillness test.
Recorded motion updates only supported shoulder directions. Independent elbow
motion can measure the forearm while upper length remains conditional. Saved
confidence prevents uncertain or legacy proportions becoming trusted new priors.
A bad forward/side pair retries both poses while retaining height. Tracking loss
requires both triggers released before a fresh hold. See BODY.md for model assumptions and limits.

The IK11 skin correction remains: solved segment lengths scale the longitudinal
bone direction while limb thickness, hand/finger size and rigid watch size remain
independent. Saved calibration geometry is unchanged by installation or loading.

Accessory ownership guards remain integrated. Hands4 extends the complete Hands3 baseline.
The installer embeds two watch PNGs and four holster WAVs. It creates missing
defaults while preserving existing calibration, settings, grips, artwork and sounds.
A fresh hash-verified full mod backup precedes deployment.

Build with both `HOLEVR_BUILD_TESTS=ON` and `HOLEVR_BUILD_HAND_TESTS=ON`.
The package records every passing suite, installer fixture checks, the source base plus the exact hand patch,
source manifest and content hashes. Installer fixtures cover absent defaults, custom files
and an asset-folder failure before replacing the DLL.

The dual-trigger regression drives the actual calibration Session at multiple
sample rates, including early release, duplicate observations, tracking loss,
rearming, acceptance and final save failure. The AR regression replays the
recorded support-hand seam crossing on both mirrored rig sides and checks
continuous sweeps, projection loss, quaternion sign changes and attachment resets.
These are offline checks. Headset appearance and haptic feel still need a playtest.

The AR support forearm now follows the authored palm continuously across the
180-degree boundary. Fractional roll retains bounded history within the same
attachment and resets on release, new grips or tracking resets. The saved hand
pose and finger placement stay anchored to the rifle. Support frames also remain
in rifle coordinates if the right-hand tracking override disappears.

Loose vise parts have 6 cm more space on each side (spacing patch
`1af7765e27d42c2727087e395cae0687d36c8df8ba9c5ccf40383add288bdc8a`).

IK14 changes the elbow preference, not calibration geometry. Bent arms prefer
less lateral flare, and a forward elbow no longer escapes the inward-placement
penalty. Cross-body and overhead targets remain reachable; the controller/grip
targets and accepted segment lengths stay authoritative. The style preference
is soft across the existing connected body domain, preventing a branch change
from imposing an abrupt new elbow window. New mirrored regressions cover rest,
inward reaches, lower hands, lateral/overhead motion and tracking variation.
These preferences infer posture; the headset/controllers do not measure elbows.

The optional fixed bench-layout reader from patch
`f2b4382615ed6e6742eac1bda95fd30c799ad6b5ee5a89a40ba796a66b9bbbfd`
is included. No chosen-layout file is bundled or installed by this build; without
one, existing automatic placement remains active. User-selected placement data
must arrive separately from the private layout editor. See WORKBENCH.md.
