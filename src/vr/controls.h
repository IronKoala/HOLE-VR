#pragma once
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

}  // namespace holevr::vr::controls
