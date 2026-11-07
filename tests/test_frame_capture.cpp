// clang++ -std=c++17 -Isrc tests/test_frame_capture.cpp -o test_frame_capture
#include "vr/frame_capture.h"
#include <cstdio>
#include <vector>
using namespace holevr::vr::frame_capture;
int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* why) { if (!ok) { ++failures; std::printf("FAIL: %s\n", why); } };
    // Last row omits padding, as GetCopyableFootprints permits. Source and eyes
    // have different strides, and each eye contains different pixel values.
    std::vector<uint8_t> s(32 + 16, 0xcc), l(16 + 8, 0xaa), r(24 + 8, 0xbb);
    for (unsigned y = 0; y < 2; ++y) for (unsigned x = 0; x < 4; ++x) {
        uint32_t value = 0x10203000 + y * 4 + x;
        std::memcpy(s.data() + y * 32 + x * 4, &value, 4);
        auto& eye = x < 2 ? l : r;
        std::memcpy(eye.data() + y * (x < 2 ? 16 : 24) + (x % 2) * 4, &value, 4);
    }
    Image source{s.data(), s.size(), 4, 2, 32}, left{l.data(),l.size(),2,2,16}, right{r.data(),r.size(),2,2,24};
    check(CompareEye(source,left,0).valid && !CompareEye(source,left,0).different, "left split / row padding");
    check(CompareEye(source,right,1).valid && !CompareEye(source,right,1).different, "right split / row padding");
    r[24+4] ^= 1;
    const auto diff = CompareEye(source,right,1);
    check(diff.valid && diff.different==1 && diff.first_x==1 && diff.first_y==1, "one corrupt pixel located");
    check(CompareEye(source,right,0).different==4, "eyes swapped detected");
    auto bad=source; bad.bytes=47;
    check(!ValidImage(bad), "incomplete final row rejected");
    bad=source; bad.row_pitch=15;
    check(!ValidImage(bad), "undersized stride rejected");
    bad=source; bad.width=3;
    check(!CompareEye(bad,left,0).valid, "odd-width source rejected");
    bad=left; bad.height=1;
    check(!CompareEye(source,bad,0).valid, "mismatched height rejected");
    check(!CompareEye(source,left,2).valid, "invalid eye rejected");
    bad=source; bad.data=nullptr;
    check(!ValidImage(bad), "null buffer rejected");
    check(!FenceReady(9,10) && FenceReady(10,10) && FenceReady(11,10), "completed fence required");
    check(!FenceReady(~uint64_t{0},10) && !FenceReady(0,0), "removed-device and unset fence rejected");
    std::printf("frame_capture: %s\n", failures ? "FAILED" : "all tests passed");
    return failures ? 1 : 0;
}
