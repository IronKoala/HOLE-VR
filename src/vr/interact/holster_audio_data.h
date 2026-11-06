#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "vr/math.h"

namespace holevr::vr::holster_audio {
struct Clip { std::vector<uint8_t> pcm; };
// Deliberately bounded format: mono 44.1 kHz PCM16, at most ten seconds.
// Clips are decoded once on the audio worker, never from a game/render tick.
bool ReadWave(const uint8_t* bytes, size_t size, Clip& clip, std::string& error);
struct Stereo { float left = 0, right = 0; };
Stereo SpatialGain(const FVector& source, const FVector& head, const FQuat& rotation, float volume);
}  // namespace holevr::vr::holster_audio
