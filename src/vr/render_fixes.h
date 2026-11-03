#pragma once
#include <cstdint>
#include <string>

#include "ue/objects.h"

namespace holevr::vr::render_fixes {
// Turns off post-process effects that look wrong or cause discomfort in a
// headset (motion blur, depth of field, vignette, lens flares, chromatic
// aberration, film grain) and stops extra scene captures that would render
// the whole level a second time. Works by writing the engine's own
// post-process settings through reflection, so no engine code is patched.
//
// Init() reads settings and resolves engine types; call it after reflection
// is ready. Apply() is safe to call repeatedly (newly spawned cameras,
// volumes and captures are picked up on the next call).
bool Init();
void Apply();
// Game thread, when a forearm menu opens (true) or the last one closes
// (false): world past arm's length goes out of focus, near things stay sharp.
// Returns how many cameras/post-process components were updated, or -1 if
// the pause blur is off.
int SetPauseBlur(bool on);
// Console settings the pause blur needs, applied at once (context: any object
// in the game world, for the console command).
void ApplyPauseBlurCvars(uintptr_t context);
// Game thread: lets the player's scene capture (stopped in VR) run while the
// death screen is up, for its picture of who killed you. Returns how many
// captures were switched.
int AllowPlayerCapture(bool allow);

// The exposure one object's post-process settings ask for (a camera,
// post-process component or volume). The game raises it in dark areas
// (BP_Player.Exposure_BaseBrightness, set by each area's
// LCBP_LocalPostProcess), and the forearm menus are drawn in the world, so
// they are brightened with it.
struct Exposure {
    bool ok = false;
    int method = -1;            // EAutoExposureMethod: 0 histogram, 1 basic, 2 manual
    bool method_set = false;    // bOverride_ on in this object
    float bias = 0.f;           // AutoExposureBias, in stops
    bool bias_set = false;
    bool physical = false;      // AutoExposureApplyPhysicalCameraExposure
    float camera_ev100 = 0.f;   // from aperture, shutter and ISO (used when physical)
    // How many stops brighter than a plain 1:1 exposure the world is drawn.
    float stops = 0.f;
};
// Game thread or background; reads only.
Exposure ReadExposure(ue::UObject obj);
// For the log: each post-process object's exposure settings (up to 8).
std::string ExposureReport();
}  // namespace holevr::vr::render_fixes
