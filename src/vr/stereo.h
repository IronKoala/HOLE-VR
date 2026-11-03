#pragma once
// Turns the engine's built-in "emulated stereo" device into a real headset
// device: the game is started with -emulatestereo, which makes UE create
// FFakeStereoRenderingDevice and render both eyes side by side into one
// texture. We swap that device's virtual functions for our own so the eyes
// use the headset's pose, field of view and resolution, and grab the eye
// texture to send to OpenXR.

#include "vr/math.h"
#include "vr/openxr.h"
#include "vr/view_lead_math.h"

namespace holevr::vr::stereo {

// Called from DllMain, before the engine reads its command line.
void InstallCommandLineHook();

// Called from the init thread after engine discovery and xr::InitInstance.
bool Install();

// Re-zero the headset position/yaw against the game camera.
void Recenter();

// Maps a pose in OpenXR tracking space (hands, controllers) into the game
// world with exactly the transform the eyes used on the latest frame
// (recentering, world scale, game camera position and yaw), plus the
// camera's expected move during the player's tick (CharacterAtTickStart).
// Thread-safe; false until the first headset frame has been rendered.
bool TrackingToWorld(const XrPosef& tracking_pose, FVector* world_location, FQuat* world_rotation);

// Runs `fn` on the game thread once per rendered frame, right after both eye
// views are set up. Unlike the player's tick it keeps running while the game
// is paused and on the title screen, so menus can use it. One callback.
void SetGameFrameCallback(void (*fn)());

// Room-scale support. HeadOffsetWorld gives the horizontal offset (world
// axes, cm, Z = 0) of the player's head from the game camera on the latest
// frame, minus any shift already requested. After moving the pawn by some
// world delta, ShiftOriginWorld(delta) moves the tracking origin to match, so
// the view stays where it was. Horizontal only; both are thread-safe and
// return false until the first headset frame.
bool HeadOffsetWorld(FVector* offset_cm);
bool ShiftOriginWorld(const FVector& world_delta_cm);

// The game camera's location for the frame being prepared, before the
// headset's offset is added: the latest frame's, plus the character's move
// since then during the player's tick (below). False until the first
// headset frame.
bool GameCameraLocation(FVector* world_cm);

// The character moves before the player's tick, but the game camera only
// follows it after the tick, so poses mapped with the latest frame's camera
// would land one frame of walking behind the next views (steady at a steady
// frame rate, a jitter when frame times vary). The player's tick reports
// the character's camera anchors before the mod's listeners run (start) and
// after them (end); from the start until the next views are set up,
// TrackingToWorld and GameCameraLocation add the camera's expected move.
// view_lead in hole-vr.ini picks the anchor: -1 auto, 0 off, 1 camera,
// 2 body, 3 capsule. Game thread.
void CharacterAtTickStart(const view_lead::Anchors& now);
void CharacterAtTickEnd(const view_lead::Anchors& now);
// Raises the eyes (and the hands with them) this many cm above where they
// would be, from the next frame on. Real-height mode uses it to give back
// the game camera's own crouch drop. Thread-safe.
void SetViewRaiseCm(double cm);

}  // namespace holevr::vr::stereo
