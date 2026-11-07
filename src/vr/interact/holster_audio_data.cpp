#include "vr/interact/holster_audio_data.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace holevr::vr::holster_audio {
namespace {
uint16_t U16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
uint32_t U32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
bool Tag(const uint8_t* p, const char* tag) { return std::memcmp(p, tag, 4) == 0; }
bool Finite(const FVector& v) { return std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z); }
}
bool ReadWave(const uint8_t* bytes, size_t size, Clip& clip, std::string& error) {
    clip.pcm.clear();
    error.clear();
    auto fail = [&](const char* why) { error = why; return false; };
    if (!bytes || size < 12 || size > 2 * 1024 * 1024) return fail("invalid file size");
    if (!Tag(bytes, "RIFF") || !Tag(bytes + 8, "WAVE")) return fail("not a RIFF WAV");
    const size_t end = size_t(U32(bytes + 4)) + 8;
    if (end != size) return fail("RIFF length mismatch");
    bool format = false, data = false;
    const uint8_t* pcm = nullptr;
    size_t count = 0;
    for (size_t at = 12; at < end;) {
        if (end - at < 8) return fail("truncated chunk header");
        const size_t length = U32(bytes + at + 4);
        const size_t start = at + 8;
        if (length > end - start) return fail("truncated chunk");
        if (Tag(bytes + at, "fmt ")) {
            if (format || length < 16) return fail("invalid format chunk");
            const auto* p = bytes + start;
            if (U16(p) != 1 || U16(p + 2) != 1 || U32(p + 4) != 44100 ||
                U32(p + 8) != 88200 || U16(p + 12) != 2 || U16(p + 14) != 16)
                return fail("expected mono 44100 Hz PCM16");
            format = true;
        } else if (Tag(bytes + at, "data")) {
            if (data || length == 0 || length % 2 || length > 882000) return fail("invalid audio data");
            data = true;
            pcm = bytes + start;
            count = length;
        }
        const size_t padded = length + (length & 1);
        if (padded > end - start) return fail("missing chunk padding");
        at = start + padded;
    }
    if (!format || !data) return fail("missing format or data chunk");
    clip.pcm.assign(pcm, pcm + count);
    return true;
}

Stereo SpatialGain(const FVector& source, const FVector& head, const FQuat& rotation, float volume) {
    if (!Finite(source) || !Finite(head) || !std::isfinite(volume) || volume <= 0) return {};
    const double norm = rotation.X*rotation.X + rotation.Y*rotation.Y + rotation.Z*rotation.Z + rotation.W*rotation.W;
    if (!std::isfinite(norm) || norm < .25 || norm > 4) return {};
    const double inv = 1 / std::sqrt(norm);
    const FQuat q{rotation.X*inv, rotation.Y*inv, rotation.Z*inv, rotation.W*inv};
    const auto local = q.Inverse().Rotate(source - head);
    const double distance = std::sqrt(local.X*local.X + local.Y*local.Y + local.Z*local.Z);
    if (!std::isfinite(distance) || distance > 500) return {};
    // At a body-worn distance, turning the head changes the balance without
    // the hard one-ear cutoff a point source directly below the ear produces.
    const double pan = .8 * std::clamp(local.Y / std::max(25.0, distance), -1.0, 1.0);
    const double gain = std::clamp(double(volume), 0.0, 1.0) * std::min(1.0, 80 / std::max(1.0, distance));
    return {float(gain * std::sqrt((1-pan)*.5)), float(gain * std::sqrt((1+pan)*.5))};
}
}  // namespace holevr::vr::holster_audio
