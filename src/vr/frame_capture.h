#pragma once
// Portable validation/comparison for a diagnostic capture. Readback row padding
// is not image data. The GPU submission fence must complete before these views
// can be constructed; UINT64_MAX is D3D12's device-removed sentinel.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace holevr::vr::frame_capture {
constexpr uint64_t kMaxSourcePixels = 16ull * 1024 * 1024;
struct Image {
    const uint8_t* data = nullptr;
    size_t bytes = 0;
    uint32_t width = 0, height = 0, row_pitch = 0;
};
inline bool FenceReady(uint64_t completed, uint64_t target) {
    return target != 0 && completed != std::numeric_limits<uint64_t>::max() && completed >= target;
}
inline bool ValidImage(const Image& im) {
    if (!im.data || !im.width || !im.height || uint64_t(im.width) * im.height > kMaxSourcePixels) return false;
    const uint64_t row_bytes = uint64_t(im.width) * 4;
    return im.row_pitch >= row_bytes &&
           uint64_t(im.height - 1) * im.row_pitch + row_bytes <= im.bytes;
}
struct Difference {
    bool valid = false;
    uint64_t pixels = 0, different = 0;
    uint32_t first_x = 0, first_y = 0, source_pixel = 0, copied_pixel = 0;
};
inline Difference CompareEye(const Image& source, const Image& copied, unsigned eye) {
    Difference out;
    if (eye > 1 || !ValidImage(source) || !ValidImage(copied) || (source.width & 1) ||
        source.width / 2 != copied.width || source.height != copied.height) return out;
    out.valid = true;
    out.pixels = uint64_t(copied.width) * copied.height;
    for (uint32_t y = 0; y < copied.height; ++y) {
        const uint8_t* a = source.data + size_t(y) * source.row_pitch + size_t(eye) * copied.width * 4;
        const uint8_t* b = copied.data + size_t(y) * copied.row_pitch;
        for (uint32_t x = 0; x < copied.width; ++x) {
            uint32_t av, bv;
            std::memcpy(&av, a + size_t(x) * 4, 4);
            std::memcpy(&bv, b + size_t(x) * 4, 4);
            if (av != bv && out.different++ == 0) {
                out.first_x = x; out.first_y = y;
                out.source_pixel = av; out.copied_pixel = bv;
            }
        }
    }
    return out;
}
} // namespace holevr::vr::frame_capture
