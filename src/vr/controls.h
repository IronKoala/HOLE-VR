#pragma once
#include <cstdint>
#include "vr/kick.h"
// Motion controls on the game thread: the gun follows the right hand and the
// trigger fires; the left hand racks the slide, the right A button drops the
// magazine and the left hand fetches and inserts a new one; left stick moves,
// right stick turns (snap or smooth). Settings in hole-vr.ini (vr_controls...).

namespace holevr::vr::controls {

// Registers the game tick listener. Call after game::Init().
void Init();
// Registers the late tick that puts holstered items on the VR body. Call
// after body::Init(), so it runs after the body is posed for the frame.
void InitLate();
// VR tab "Edit hand grips": opens (or closes) the grip editor once the menu
// closes. Inside the editor, left Y tap saves and left Y held 1 s closes it.
void RequestGripEditor();
// VR tab "Move holsters": 5 s after the menu closes, each holster moves to
// where the hand holding its item is.
void RequestHolsterMove();

// Game-thread body integration. A plan is frozen at the accepted kick press;
// native TL_Kick remains the authoritative indication that an action is playing.
bool GetKickPlan(uintptr_t player, kick::Plan* out);
// Publish final drawn world hips after body posing, including room translation.
// A trace uses these only in the same/next controls tick, for at most 250 ms.
void PublishKickHips(uintptr_t player, const FVector hips[2]);
void ClearKickHips(uintptr_t player);

}  // namespace holevr::vr::controls
