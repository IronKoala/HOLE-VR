#pragma once
// Math types laid out like UE 5.x's double-precision core types, plus the
// conversions between OpenXR and Unreal coordinate systems.
//
// OpenXR: right-handed, +Y up, -Z forward, metres.
// Unreal: left-handed, +Z up, +X forward, centimetres (WorldToMeters = 100).

#include <cmath>

namespace holevr::vr {

struct FVector {  // FVector3d
    double X = 0, Y = 0, Z = 0;
    FVector operator+(const FVector& o) const { return {X + o.X, Y + o.Y, Z + o.Z}; }
    FVector operator-(const FVector& o) const { return {X - o.X, Y - o.Y, Z - o.Z}; }
    FVector operator*(double s) const { return {X * s, Y * s, Z * s}; }
};

struct FRotator {  // FRotator3d, degrees
    double Pitch = 0, Yaw = 0, Roll = 0;
};

struct FQuat {  // FQuat4d. Hamilton product; A * B applies B first, like UE.
    double X = 0, Y = 0, Z = 0, W = 1;

    FQuat operator*(const FQuat& b) const {
        return {W * b.X + X * b.W + Y * b.Z - Z * b.Y,
                W * b.Y - X * b.Z + Y * b.W + Z * b.X,
                W * b.Z + X * b.Y - Y * b.X + Z * b.W,
                W * b.W - X * b.X - Y * b.Y - Z * b.Z};
    }
    FQuat Inverse() const { return {-X, -Y, -Z, W}; }
    FVector Rotate(const FVector& v) const {
        // v' = v + 2w(q x v) + 2(q x (q x v))
        FVector q{X, Y, Z};
        FVector t{2 * (q.Y * v.Z - q.Z * v.Y), 2 * (q.Z * v.X - q.X * v.Z), 2 * (q.X * v.Y - q.Y * v.X)};
        return {v.X + W * t.X + (q.Y * t.Z - q.Z * t.Y), v.Y + W * t.Y + (q.Z * t.X - q.X * t.Z),
                v.Z + W * t.Z + (q.X * t.Y - q.Y * t.X)};
    }
};

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;
constexpr double kRadToDeg = 180.0 / kPi;

inline double NormalizeAxis(double a) {
    a = std::fmod(a, 360.0);
    if (a > 180.0) a -= 360.0;
    if (a < -180.0) a += 360.0;
    return a;
}

// Same formulas as UE's FRotator::Quaternion and FQuat::Rotator.
inline FQuat ToQuat(const FRotator& r) {
    double sp = std::sin(r.Pitch * kDegToRad * 0.5), cp = std::cos(r.Pitch * kDegToRad * 0.5);
    double sy = std::sin(r.Yaw * kDegToRad * 0.5), cy = std::cos(r.Yaw * kDegToRad * 0.5);
    double sr = std::sin(r.Roll * kDegToRad * 0.5), cr = std::cos(r.Roll * kDegToRad * 0.5);
    return {cr * sp * sy - sr * cp * cy, -cr * sp * cy - sr * cp * sy,
            cr * cp * sy - sr * sp * cy, cr * cp * cy + sr * sp * sy};
}

inline FRotator ToRotator(const FQuat& q) {
    const double singularity = q.Z * q.X - q.W * q.Y;
    const double yaw_y = 2.0 * (q.W * q.Z + q.X * q.Y);
    const double yaw_x = 1.0 - 2.0 * (q.Y * q.Y + q.Z * q.Z);
    FRotator r;
    r.Yaw = std::atan2(yaw_y, yaw_x) * kRadToDeg;
    if (singularity < -0.4999995) {
        r.Pitch = -90.0;
        r.Roll = NormalizeAxis(-r.Yaw - 2.0 * std::atan2(q.X, q.W) * kRadToDeg);
    } else if (singularity > 0.4999995) {
        r.Pitch = 90.0;
        r.Roll = NormalizeAxis(r.Yaw - 2.0 * std::atan2(q.X, q.W) * kRadToDeg);
    } else {
        r.Pitch = std::asin(2.0 * singularity) * kRadToDeg;
        r.Roll = std::atan2(-2.0 * (q.W * q.X + q.Y * q.Z), 1.0 - 2.0 * (q.X * q.X + q.Y * q.Y)) * kRadToDeg;
    }
    return r;
}

inline FQuat YawOnly(double yaw_degrees) { return ToQuat(FRotator{0, yaw_degrees, 0}); }

// OpenXR pose -> Unreal. Same mapping as Epic's OpenXR plugin.
inline FVector XrToUePosition(float x, float y, float z, double world_to_meters) {
    return FVector{-z, x, y} * world_to_meters;
}
inline FQuat XrToUeQuat(float x, float y, float z, float w) { return FQuat{-z, x, y, -w}; }

// Yaw of a UE quaternion around +Z, in degrees.
inline double YawOf(const FQuat& q) { return ToRotator(q).Yaw; }

}  // namespace holevr::vr
