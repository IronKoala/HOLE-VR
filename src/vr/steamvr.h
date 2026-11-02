#pragma once
// Getting into the headset without any steps at the PC.
//
// If the game starts while SteamVR is closed, the first OpenXR call brings up
// a bare SteamVR server. With a wireless headset (Steam Frame over Steam Link)
// that server finds no connected headset, refuses the game and exits, and
// every retry does the same, so VR never starts (SteamVR logs:
// "Refusing connect ... VRInitError_Driver_WirelessHmdNotConnected").
// Started the normal way (Steam's SteamVR Launch button, which runs
// vrstartup.exe), SteamVR stays up and waits for the headset, which then
// connects in about a second. So we start it that way before the first
// OpenXR call: through Steam first, vrstartup.exe directly if that fails.

namespace holevr::vr::steamvr {

// From the VR start-up thread, before xr::InitInstance. Starts SteamVR if the
// active OpenXR runtime is SteamVR and it isn't running, then waits (up to
// about a minute) for it to come up. Returns at once if it is already up, the
// runtime is something else, or start_steamvr=0 in hole-vr.ini.
void EnsureRunning();

// True while SteamVR's own processes (vrmonitor or vrcompositor) are running.
bool Running();

}  // namespace holevr::vr::steamvr
