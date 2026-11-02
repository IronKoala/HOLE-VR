#include "vr/interact/holster.h"

#include <algorithm>
#include <cmath>

namespace holevr::vr::interact {
namespace {

double Len(const FVector& v) { return std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z); }

// Rotation whose X/Y/Z axes are the given orthonormal vectors.
FQuat FromAxes(const FVector& x, const FVector& y, const FVector& z) {
    const double tr = x.X + y.Y + z.Z;
    FQuat q;
    if (tr > 0) {
        double s = std::sqrt(tr + 1.0) * 2;
        q.W = 0.25 * s;
        q.X = (y.Z - z.Y) / s;
        q.Y = (z.X - x.Z) / s;
        q.Z = (x.Y - y.X) / s;
    } else if (x.X > y.Y && x.X > z.Z) {
        double s = std::sqrt(1.0 + x.X - y.Y - z.Z) * 2;
        q.W = (y.Z - z.Y) / s;
        q.X = 0.25 * s;
        q.Y = (y.X + x.Y) / s;
        q.Z = (z.X + x.Z) / s;
    } else if (y.Y > z.Z) {
        double s = std::sqrt(1.0 + y.Y - x.X - z.Z) * 2;
        q.W = (z.X - x.Z) / s;
        q.X = (y.X + x.Y) / s;
        q.Y = 0.25 * s;
        q.Z = (z.Y + y.Z) / s;
    } else {
        double s = std::sqrt(1.0 + z.Z - x.X - y.Y) * 2;
        q.W = (x.Y - y.X) / s;
        q.X = (z.X + x.Z) / s;
        q.Y = (z.Y + y.Z) / s;
        q.Z = 0.25 * s;
    }
    return q;
}

}  // namespace

HolsterSpot DefaultSpot(HolsterId id) {
    HolsterSpot s;
    switch (id) {
        case HolsterId::kPistol:
            // Grip of the pistol at the right hip, level with the pelvis.
            s.anchor = HolsterAnchor::kBelt;
            s.offset = FVector{0, 21, 0};
            s.radius = 12.0;
            s.hand = 1;
            break;
        case HolsterId::kMag:
            // Magazine pouch on the left hip, a little forward of the side.
            s.anchor = HolsterAnchor::kBelt;
            s.offset = FVector{5, -18, 0};
            s.radius = 14.0;
            s.hand = 0;
            break;
        case HolsterId::kRifle:
            // Not built yet: rifle across the back.
            s.enabled = false;
            s.anchor = HolsterAnchor::kChest;
            s.offset = FVector{-16, 0, 4};
            s.radius = 16.0;
            s.hand = 1;
            break;
        case HolsterId::kRifleMag:
            // Not built yet: rifle magazines at the front of the belt.
            s.enabled = false;
            s.anchor = HolsterAnchor::kBelt;
            s.offset = FVector{12, -10, 0};
            s.radius = 14.0;
            s.hand = 0;
            break;
        default: s.enabled = false; break;
    }
    return s;
}

const char* HolsterName(HolsterId id) {
    switch (id) {
        case HolsterId::kPistol: return "pistol";
        case HolsterId::kMag: return "mag";
        case HolsterId::kRifle: return "rifle";
        case HolsterId::kRifleMag: return "rifle_mag";
        default: return "unknown";
    }
}

FVector SpotWorld(const HolsterSpot& s, const FVector& anchor, const FQuat& body_rot) {
    return anchor + body_rot.Rotate(s.offset);
}

FVector SpotOffset(const FVector& world, const FVector& anchor, const FQuat& body_rot) {
    return body_rot.Inverse().Rotate(world - anchor);
}

FQuat PistolHolsterFrame(const FQuat& body_rot, double cant_deg) {
    const double c = cant_deg * kDegToRad;
    // Body frame: X forward, Y right, Z up.
    const FVector barrel{-std::sin(c), 0, -std::cos(c)};  // muzzle down, swung back by the cant
    const FVector top{std::cos(c), 0, -std::sin(c)};      // slide toward the front
    const FVector right{0, 1, 0};                         // the gun's right side faces out
    return body_rot * FromAxes(barrel, right, top);
}

FQuat MagPouchGunFrame(const FQuat& body_rot, double rake_deg) {
    const double r = rake_deg * kDegToRad;
    // Nose pitched up by the rake: the raked magwell then points straight up.
    const FVector barrel{std::cos(r), 0, std::sin(r)};
    const FVector top{-std::sin(r), 0, std::cos(r)};
    return body_rot * FromAxes(barrel, FVector{0, 1, 0}, top);
}

void HolsterDraw::Configure(double radius, DrawMode mode, double return_s, double settle_s) {
    radius_ = radius;
    mode_ = mode;
    return_s_ = return_s;
    settle_s_ = settle_s;
}

void HolsterDraw::Reset(HolsterState s) {
    state_ = s;
    blend_ = 1.0;
    blend_speed_ = 0.0;
    hover_ = false;
    last_dist_ = 0.0;
}

HolsterEvent HolsterDraw::Update(const DrawInput& in) {
    HolsterEvent ev = HolsterEvent::kNone;
    // Out of the hand: slide / fly into the holster.
    if (state_ != HolsterState::kInHand && blend_ < 1.0) {
        blend_ = blend_speed_ > 0.0 ? std::min(1.0, blend_ + blend_speed_ * std::max(0.0, in.dt)) : 1.0;
        if (blend_ >= 1.0 && state_ == HolsterState::kReturning) {
            state_ = HolsterState::kHolstered;
            ev = HolsterEvent::kReturned;
        }
    }
    const bool measurable = in.hand_valid && in.spot_valid;
    const double dist = measurable ? Len(in.hand - in.spot) : 1e9;
    const bool was_hover = hover_;
    if (state_ != HolsterState::kInHand && measurable)
        hover_ = was_hover ? dist <= radius_ + 3.0 : dist <= radius_;
    else
        hover_ = false;
    if (hover_ && !was_hover && ev == HolsterEvent::kNone) ev = HolsterEvent::kHoverEnter;
    if (in.freeze || !measurable) return ev;

    auto blend_in = [&](double seconds) {
        blend_ = 0.0;
        blend_speed_ = seconds > 0.0 ? 1.0 / seconds : 0.0;
        if (seconds <= 0.0) blend_ = 1.0;
    };
    switch (state_) {
        case HolsterState::kHolstered:
        case HolsterState::kReturning:
            if (in.grip_pressed && dist <= radius_) {
                state_ = HolsterState::kInHand;
                hover_ = false;
                last_dist_ = dist;
                return HolsterEvent::kDrawn;
            }
            break;
        case HolsterState::kInHand: {
            const bool let_go = mode_ == DrawMode::kHold ? !in.grip_down : false;
            const bool put_away = mode_ == DrawMode::kHold ? let_go && dist <= radius_
                                                           : in.grip_pressed && dist <= radius_;
            if (put_away) {
                state_ = HolsterState::kHolstered;
                last_dist_ = dist;
                blend_in(settle_s_);
                return HolsterEvent::kStowed;
            }
            if (let_go) {
                state_ = HolsterState::kReturning;
                last_dist_ = dist;
                blend_in(return_s_);
                if (blend_ >= 1.0) state_ = HolsterState::kHolstered;
                return HolsterEvent::kReturnStarted;
            }
            break;
        }
    }
    return ev;
}

bool HolsterZone::Update(double dist, double radius, bool active) {
    const bool was = inside_;
    inside_ = active && (was ? dist <= radius + 3.0 : dist <= radius);
    return inside_ && !was;
}

}  // namespace holevr::vr::interact
