#pragma once
// The order of OpenXR swapchain image calls for one eye, kept apart from the
// runtime so it can be tested without a headset (tests/test_swapchain_hold.cpp).
//
// Rules from the OpenXR spec this follows:
// - An image must be waited on (xrWaitSwapchainImage returned XR_SUCCESS)
//   before it is written, and before it is released.
// - XR_TIMEOUT_EXPIRED is a success code, but the image is not ready; the next
//   wait is for the same image, so the image stays held until then.
// - The headset shows a swapchain's most recently RELEASED image. An image
//   is therefore released only after this frame's picture has been copied into
//   it, and stays held over frames where the copy could not run.
#include <cstdint>

namespace holevr::vr {

struct HeldImage {
    bool acquired = false;  // between acquire and release
    bool waited = false;    // its wait completed: safe to write
    uint32_t index = 0;
};

enum class WaitResult { kReady, kTimeout, kError };
enum class ImageStep { kReady, kTimeout, kWaitError, kAcquireError };

// acquire(uint32_t* index) -> bool; wait() -> WaitResult.
template <class Acquire, class Wait>
ImageStep ReadyImage(HeldImage& h, Acquire&& acquire, Wait&& wait) {
    if (!h.acquired) {
        uint32_t index = 0;
        if (!acquire(&index)) return ImageStep::kAcquireError;
        h.acquired = true;
        h.waited = false;
        h.index = index;
    }
    if (!h.waited) {
        switch (wait()) {
            case WaitResult::kReady: h.waited = true; break;
            case WaitResult::kTimeout: return ImageStep::kTimeout;  // same image next time
            case WaitResult::kError: return ImageStep::kWaitError;
        }
    }
    return ImageStep::kReady;
}

// Releases the image if it was waited on; call only once it holds the new
// picture. release() -> void.
template <class Release>
bool ReleaseWritten(HeldImage& h, Release&& release) {
    if (!h.acquired || !h.waited) return false;
    release();
    h.acquired = false;
    h.waited = false;
    return true;
}

}  // namespace holevr::vr
