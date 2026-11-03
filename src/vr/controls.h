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
// VR tab "Calibrate body": measures standing eye height once the menu closes
// (stand straight for 4 s). Saved as standing_height_cm.
void RequestHeightMeasure();
// VR tab "Edit hand grips": opens (or closes) the grip editor once the menu
// closes. Inside the editor, left Y tap saves and left Y held 1 s closes it.
void RequestGripEditor();
// VR tab "Move holsters": 5 s after the menu closes, each holster moves to
// where the hand holding its item is.
void RequestHolsterMove();

}  // namespace holevr::vr::controls
