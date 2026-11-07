#include "vr/interact/holster_audio_data.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>
using namespace holevr::vr;
using namespace holevr::vr::holster_audio;
namespace {
int checks = 0;
void Check(bool good, const char* what) {
    ++checks;
    if (!good) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
void U32(std::vector<uint8_t>& b, size_t at, uint32_t value) {
    for (int i = 0; i < 4; ++i) b[at+i] = uint8_t(value >> (8*i));
}
void U16(std::vector<uint8_t>& b, size_t at, uint16_t value) {
    b[at] = uint8_t(value); b[at+1] = uint8_t(value >> 8);
}
std::vector<uint8_t> Wave() {
    std::vector<uint8_t> b(52);
    std::memcpy(b.data(), "RIFF", 4); U32(b, 4, 44);
    std::memcpy(b.data()+8, "WAVEfmt ", 8); U32(b, 16, 16);
    U16(b, 20, 1); U16(b, 22, 1); U32(b, 24, 44100); U32(b, 28, 88200);
    U16(b, 32, 2); U16(b, 34, 16); std::memcpy(b.data()+36, "data", 4); U32(b, 40, 8);
    U16(b, 44, 123); U16(b, 46, 32767); U16(b, 48, 32768); U16(b, 50, 0);
    return b;
}
bool Near(double a, double b) { return std::abs(a-b) < 1e-6; }
}
int main() {
    Clip clip; std::string error;
    const auto good = Wave();
    Check(ReadWave(good.data(), good.size(), clip, error), "valid PCM WAV");
    Check(clip.pcm.size() == 8 && clip.pcm[0] == 123 && clip.pcm[2] == 255, "PCM retained exactly");
    for (size_t n = 0; n < good.size(); ++n) {
        Check(!ReadWave(good.data(), n, clip, error), "truncation rejected");
        Check(clip.pcm.empty() && !error.empty(), "failure clears previous clip");
    }
    for (size_t at : {size_t(0),size_t(8),size_t(20),size_t(22),size_t(24),size_t(28),size_t(32),size_t(34)}) {
        auto bad = good; bad[at] ^= 1;
        Check(!ReadWave(bad.data(), bad.size(), clip, error), "wrong container/format rejected");
    }
    for (uint32_t length : {0u,1u,9u,0xffffffffu}) {
        auto bad = good; U32(bad,40,length);
        Check(!ReadWave(bad.data(), bad.size(), clip, error), "invalid data length rejected");
    }
    auto extra = good;
    extra.insert(extra.begin()+12, {'J','U','N','K',1,0,0,0,42,0}); U32(extra,4,uint32_t(extra.size()-8));
    Check(ReadWave(extra.data(),extra.size(),clip,error), "odd metadata chunk and padding accepted");
    auto order = good;
    order.erase(order.begin()+12,order.begin()+36);
    order.insert(order.end(),good.begin()+12,good.begin()+36);
    Check(ReadWave(order.data(),order.size(),clip,error), "data before format accepted");
    auto duplicate = good;
    duplicate.insert(duplicate.end(),good.begin()+36,good.end()); U32(duplicate,4,uint32_t(duplicate.size()-8));
    Check(!ReadWave(duplicate.data(),duplicate.size(),clip,error), "duplicate data rejected");
    Check(!ReadWave(nullptr,100,clip,error), "null file rejected");

    const auto center = SpatialGain({0,0,-60}, {}, {}, 1);
    Check(Near(center.left,center.right), "below head is centered");
    Check(Near(center.left*center.left+center.right*center.right,1), "equal-power center");
    const auto right = SpatialGain({0,60,0}, {}, {}, 1);
    const auto left = SpatialGain({0,-60,0}, {}, {}, 1);
    Check(right.right > right.left && left.left > left.right, "UE Y is ear direction");
    Check(Near(right.right,left.left) && Near(right.left,left.right), "left-right symmetry");
    const auto turned = SpatialGain({0,60,0}, {}, YawOnly(90), 1);
    Check(Near(turned.left,turned.right), "head rotation changes panning");
    const auto far = SpatialGain({0,0,-160}, {}, {}, 1);
    Check(Near(far.left,center.left*.5), "distance attenuation");
    const auto quiet = SpatialGain({0,60,0}, {}, {}, .5f);
    Check(Near(quiet.right,right.right*.5), "volume applies without changing pan");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (auto gain : {SpatialGain({nan,0,0},{},{},1), SpatialGain({},{},{0,0,0,nan},1),
                      SpatialGain({},{},{},float(nan)), SpatialGain({},{},{0,0,0,0},1),
                      SpatialGain({},{},{},0), SpatialGain({1000,0,0},{},{},1)})
        Check(gain.left==0 && gain.right==0,"invalid/muted/stale position silent");
    for (int yaw=-180; yaw<=180; yaw+=3) for (int side=-100; side<=100; side+=5) {
        const auto gain=SpatialGain({30,double(side),-65},{},YawOnly(yaw),.8f);
        Check(std::isfinite(gain.left) && std::isfinite(gain.right) && gain.left>=0 && gain.right>=0,
              "continuous body source is finite");
        Check(gain.left*gain.left+gain.right*gain.right<=.640001f,"panning cannot amplify beyond chosen volume");
    }
    std::printf("%d holster audio checks passed\n", checks);
}
