#pragma once
// Turns the engine's built-in "emulated stereo" device into a real headset
// device: the game is started with -emulatestereo, which makes UE create
// FFakeStereoRenderingDevice and render both eyes side by side into one
// texture. We swap that device's virtual functions for our own so the eyes
// use the headset's pose, field of view and resolution, and grab the eye
// texture to send to OpenXR.

namespace holevr::vr::stereo {

// Called from DllMain, before the engine reads its command line.
void InstallCommandLineHook();

// Called from the init thread after engine discovery and xr::InitInstance.
bool Install();

// Re-zero the headset position/yaw against the game camera.
void Recenter();

}  // namespace holevr::vr::stereo
