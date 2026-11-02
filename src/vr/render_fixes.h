#pragma once

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
}  // namespace holevr::vr::render_fixes
