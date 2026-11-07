#pragma once
// Geometry-only stair recognition. A single raised hit is never stair evidence.
#include <array>
#include <cstdint>
#include "vr/math.h"

namespace holevr::vr::body::stairs {
constexpr int kSamples=13;
constexpr double kSpacing=10.0;
constexpr double kMaxRiser=26.0;
constexpr double kMaxFloorDifference=30.0;
struct Sample {
    bool hit=false;
    double along=0,z=0;
    FVector normal{0,0,1};
    uintptr_t support_id=0;
};
struct Tread {
    double begin=0,end=0,z=0;
    uintptr_t support_id=0;
    unsigned samples=0;
};
struct Profile {
    bool valid=false;
    FVector origin,direction{1,0,0};
    std::array<Tread,kSamples> tread{};
    unsigned count=0;
    int rise_sign=0;
};
// Requires at least three flat, sufficiently wide plateaus separated by two
// consistent risers. Gaps, ramps, isolated ledges and furniture are rejected.
Profile Recognize(const std::array<Sample,kSamples>& samples,const FVector& origin,
                  const FVector& direction,double floor_z);
struct Landing {
    bool valid=false;
    FVector sole;
    double begin=0,end=0,clearance_z=0;
    uintptr_t support_id=0;
};
// Keep heel/toe inside one observed tread and only use current/adjacent levels.
// Runtime must still confirm this proposed footprint with fresh ground traces.
Landing Select(const Profile& profile,const FVector& desired,double floor_z,double advance=0);
} // namespace holevr::vr::body::stairs
