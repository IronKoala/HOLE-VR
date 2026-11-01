#pragma once
#include <string>

// Engine console variables (r.* etc.) set through the engine's own
// KismetSystemLibrary.ExecuteConsoleCommand, on the game thread. Values from
// hole-vr.ini (vr_anti_aliasing, cvar.* lines) are re-applied whenever the
// game changes them back, e.g. after its graphics menu applies settings.
namespace holevr::vr::cvars {
// After game::Init(). Reads the settings.
void Init();
// From the background loop about once a second.
void Maintain();
// Ctrl+F10: next anti-aliasing mode (not saved to the ini).
void CycleAntiAliasing();
// Wanted value for a console variable; applied on the next Maintain().
void Want(const std::string& name, const std::string& value);
}  // namespace holevr::vr::cvars
