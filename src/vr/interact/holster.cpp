#include "vr/interact/holster.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace holevr::vr::interact {
namespace {

double Len(const FVector& v) { return std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z); }
double Dot(const FVector& a, const FVector& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
FVector Cross(const FVector& a, const FVector& b) {
    return FVector{a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X};
}
FQuat Normalized(const FQuat& q) {
    const double n = std::sqrt(q.X * q.X + q.Y * q.Y + q.Z * q.Z + q.W * q.W);
    if (n < 1e-9) return FQuat{};
    return FQuat{q.X / n, q.Y / n, q.Z / n, q.W / n};
}

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
            // Behind the right shoulder (owner, 2026-11-04): where the right
            // hand reaches back over the shoulder for the rifle's grip. Rides
            // on the chest so it follows the torso. A wide zone, since you
            // reach for it without seeing it.
            s.anchor = HolsterAnchor::kChest;
            s.offset = FVector{-14, 14, 6};
            s.radius = 18.0;
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

const char* AnchorName(HolsterAnchor a) {
    switch (a) {
        case HolsterAnchor::kBelt: return "belt";
        case HolsterAnchor::kChest: return "chest";
        case HolsterAnchor::kThighL: return "left_thigh";
        case HolsterAnchor::kThighR: return "right_thigh";
        default: return "belt";
    }
}

const char* AnchorPlace(HolsterAnchor a) {
    switch (a) {
        case HolsterAnchor::kChest: return "your chest";
        case HolsterAnchor::kThighL: return "your left thigh";
        case HolsterAnchor::kThighR: return "your right thigh";
        default: return "your belt";
    }
}

FQuat FrameFromUp(const FVector& up, const FVector& forward) {
    const double ul = Len(up);
    if (ul < 1e-6) return FQuat{};
    const FVector z = up * (1.0 / ul);
    FVector x = forward - z * Dot(forward, z);
    if (Len(x) < 1e-3) {
        // Forward along up: any perpendicular will do.
        x = std::fabs(z.Z) < 0.9 ? FVector{0, 0, 1} : FVector{1, 0, 0};
        x = x - z * Dot(x, z);
    }
    x = x * (1.0 / Len(x));
    const FVector y = Cross(z, x);
    return Normalized(FromAxes(x, y, z));
}

HolsterAnchor NearestAnchor(const FVector& hand, const FVector* anchor_locs, const bool* anchor_valid) {
    HolsterAnchor best = HolsterAnchor::kBelt;
    double best_d = 1e18;
    for (int i = 0; i < static_cast<int>(HolsterAnchor::kCount); ++i) {
        if (!anchor_valid[i]) continue;
        const double d = Len(hand - anchor_locs[i]);
        if (d < best_d - 1e-9) {
            best_d = d;
            best = static_cast<HolsterAnchor>(i);
        }
    }
    return best;
}

HolsterSpot PlaceSpot(const HolsterSpot& base, HolsterAnchor anchor, const FVector& anchor_loc,
                      const FQuat& anchor_rot, const FVector& hand, const FQuat* item_rot) {
    HolsterSpot s = base;
    s.anchor = anchor;
    s.offset = SpotOffset(hand, anchor_loc, anchor_rot);
    s.turned = item_rot != nullptr;
    s.rot = item_rot ? Normalized(anchor_rot.Inverse() * *item_rot) : FQuat{};
    return s;
}

std::string FormatSpotLine(HolsterId id, const HolsterSpot& s) {
    char buf[256];
    int n = std::snprintf(buf, sizeof(buf), "%s=%s %.1f %.1f %.1f", HolsterName(id), AnchorName(s.anchor),
                          s.offset.X, s.offset.Y, s.offset.Z);
    if (s.turned && n > 0 && n < static_cast<int>(sizeof(buf)))
        std::snprintf(buf + n, sizeof(buf) - n, " rot %.5f %.5f %.5f %.5f", s.rot.X, s.rot.Y, s.rot.Z, s.rot.W);
    return buf;
}

bool ParseSpotLine(const char* line, HolsterId id, HolsterSpot* s) {
    if (!line || line[0] == ';') return false;
    const char* name = HolsterName(id);
    const size_t nl = std::strlen(name);
    if (std::strncmp(line, name, nl) != 0 || line[nl] != '=') return false;
    const char* p = line + nl + 1;
    HolsterSpot out = *s;
    double x, y, z;
    // Old form: three numbers from the belt, built-in hanging.
    if (std::sscanf(p, "%lf %lf %lf", &x, &y, &z) == 3) {
        out.anchor = HolsterAnchor::kBelt;
        out.offset = FVector{x, y, z};
        out.turned = false;
        out.rot = FQuat{};
        *s = out;
        return true;
    }
    char anchor[32] = {};
    int used = 0;
    if (std::sscanf(p, "%31[a-z_] %lf %lf %lf%n", anchor, &x, &y, &z, &used) != 4) return false;
    bool known = false;
    for (int i = 0; i < static_cast<int>(HolsterAnchor::kCount); ++i) {
        if (std::strcmp(anchor, AnchorName(static_cast<HolsterAnchor>(i))) == 0) {
            out.anchor = static_cast<HolsterAnchor>(i);
            known = true;
        }
    }
    if (!known) return false;
    out.offset = FVector{x, y, z};
    out.turned = false;
    out.rot = FQuat{};
    FQuat q;
    if (std::sscanf(p + used, " rot %lf %lf %lf %lf", &q.X, &q.Y, &q.Z, &q.W) == 4) {
        out.turned = true;
        out.rot = Normalized(q);
    }
    *s = out;
    return true;
}

FVector SpotWorld(const HolsterSpot& s, const FVector& anchor, const FQuat& anchor_rot) {
    return anchor + anchor_rot.Rotate(s.offset);
}

FVector SpotOffset(const FVector& world, const FVector& anchor, const FQuat& anchor_rot) {
    return anchor_rot.Inverse().Rotate(world - anchor);
}

FQuat PistolHolsterFrame(const FQuat& body_rot, double cant_deg) {
    const double c = cant_deg * kDegToRad;
    // Body frame: X forward, Y right, Z up.
    const FVector barrel{-std::sin(c), 0, -std::cos(c)};  // muzzle down, swung back by the cant
    const FVector top{std::cos(c), 0, -std::sin(c)};      // slide toward the front
    const FVector right{0, 1, 0};                         // the gun's right side faces out
    return body_rot * FromAxes(barrel, right, top);
}

FQuat RifleBackFrame(const FQuat& anchor_rot, double slant_deg) {
    const double a = slant_deg * kDegToRad;
    // Anchor frame: X forward, Y right, Z up. The grip at the right shoulder,
    // the muzzle down the back and toward the left; the receiver's top lies
    // against the back, so the grip and magazine stick out behind you.
    const FVector barrel{0, -std::sin(a), -std::cos(a)};
    const FVector top{1, 0, 0};
    const FVector right = Cross(top, barrel);
    return anchor_rot * FromAxes(barrel, right, top);
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
