#pragma once
#include <string>
#include "vr/math.h"

namespace holevr::vr::holster_audio {
// Called once from controls Init (outside DllMain). File/COM/audio work runs
// on a dedicated process-lifetime worker; the mod itself cannot hot-unload.
void Init(const std::wstring& mod_dir);
// Game thread, before cue dispatch. The watchdog silences loading/paused ticks.
void Update(bool enabled, const FVector& source, const FVector& head, const FQuat& rotation, float volume);
// Game thread, on lost player/tracking/menu. Drops queued and playing sounds.
void Silence();
}  // namespace holevr::vr::holster_audio
