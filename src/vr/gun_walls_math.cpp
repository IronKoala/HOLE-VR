#include "vr/gun_walls_math.h"

#include <algorithm>
#include <cmath>

namespace holevr::vr::gun_walls {
namespace {
double Dot(const FVector& a, const FVector& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
double Len(const FVector& v) { return std::sqrt(Dot(v, v)); }
bool Finite(const FVector& v) { return std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z); }

struct Corner {
    FVector at;
    Part part;
};

// The 26 directions out of a cube, faces first, then edges, then corners.
std::vector<FVector> Directions() {
    std::vector<FVector> out;
    for (int nonzero = 1; nonzero <= 3; ++nonzero)
        for (int x = -1; x <= 1; ++x)
            for (int y = -1; y <= 1; ++y)
                for (int z = -1; z <= 1; ++z) {
                    if ((x != 0) + (y != 0) + (z != 0) != nonzero) continue;
                    const double s = 1.0 / std::sqrt(static_cast<double>(nonzero));
                    out.push_back(FVector{x * s, y * s, z * s});
                }
    return out;
}

bool Usable(const Box& b) {
    return Finite(b.lo) && Finite(b.hi) && b.hi.X >= b.lo.X && b.hi.Y >= b.lo.Y && b.hi.Z >= b.lo.Z;
}
}  // namespace

const char* PartName(Part part) {
    switch (part) {
        case Part::kGun: return "gun";
        case Part::kFiringHand: return "gun hand";
        case Part::kMagazine: return "magazine";
        case Part::kSupportHand: return "support hand";
        default: return "?";
    }
}

Box FiringHandBox() {
    // A fist round a pistol grip: about 8 cm front to back, 9 cm side to
    // side, from the web under the slide (2.5 cm up) to the little finger.
    return Box{FVector{-4.5, -4.5, -6.5}, FVector{3.5, 4.5, 2.5}};
}

Box SupportHandBox(const FVector& draw) {
    // Fingers and palm round a handguard: mostly under and to the sides.
    return Box{draw + FVector{-4.5, -4.5, -6.0}, draw + FVector{4.5, 4.5, 2.5}};
}

std::vector<Point> Outline(const std::vector<PartBox>& boxes, double merge_cm, double slice_cm,
                           std::size_t max_points) {
    std::vector<Point> out;
    std::vector<Corner> corners;
    std::vector<PartBox> usable;
    for (const PartBox& pb : boxes) {
        if (!Usable(pb.box)) continue;
        usable.push_back(pb);
        const Box& b = pb.box;
        for (int k = 0; k < 8; ++k)
            corners.push_back({FVector{k & 1 ? b.hi.X : b.lo.X, k & 2 ? b.hi.Y : b.lo.Y, k & 4 ? b.hi.Z : b.lo.Z},
                               pb.part});
    }
    if (corners.empty() || max_points == 0) return out;
    const double merge = std::max(0.0, merge_cm);
    auto keep = [&](const Point& p) {
        if (out.size() >= max_points) return;
        for (const Point& q : out)
            if (Len(q.at - p.at) < merge) return;
        out.push_back(p);
    };
    constexpr double kTieCm = 0.3;
    for (const FVector& d : Directions()) {
        double best = -1e300;
        for (const Corner& c : corners) best = std::max(best, Dot(c.at, d));
        FVector sum;
        int n = 0;
        Part part = Part::kGun;
        double part_best = -1e300;
        for (const Corner& c : corners) {
            const double s = Dot(c.at, d);
            if (s < best - kTieCm) continue;
            sum = sum + c.at;
            ++n;
            if (s > part_best) part_best = s, part = c.part;
        }
        if (n > 0) keep(Point{sum * (1.0 / n), part});
    }
    // Slices along a long gun.
    double min_x = 1e300, max_x = -1e300;
    for (const PartBox& pb : usable) min_x = std::min(min_x, pb.box.lo.X), max_x = std::max(max_x, pb.box.hi.X);
    const double length = max_x - min_x;
    if (slice_cm > 1.0 && length > 1.5 * slice_cm) {
        const int slices = static_cast<int>(std::ceil(length / slice_cm));
        for (int i = 0; i < slices; ++i) {
            const double x = min_x + (i + 0.5) * length / slices;
            // Widest to the right, to the left, highest and lowest of the
            // boxes this slice's middle passes through.
            const PartBox* side[4] = {nullptr, nullptr, nullptr, nullptr};
            for (const PartBox& pb : usable) {
                if (x < pb.box.lo.X || x > pb.box.hi.X) continue;
                if (!side[0] || pb.box.hi.Y > side[0]->box.hi.Y) side[0] = &pb;
                if (!side[1] || pb.box.lo.Y < side[1]->box.lo.Y) side[1] = &pb;
                if (!side[2] || pb.box.hi.Z > side[2]->box.hi.Z) side[2] = &pb;
                if (!side[3] || pb.box.lo.Z < side[3]->box.lo.Z) side[3] = &pb;
            }
            for (int s = 0; s < 4; ++s) {
                if (!side[s]) continue;
                const Box& b = side[s]->box;
                const FVector mid = (b.lo + b.hi) * 0.5;
                FVector p{x, mid.Y, mid.Z};
                if (s == 0) p.Y = b.hi.Y;
                if (s == 1) p.Y = b.lo.Y;
                if (s == 2) p.Z = b.hi.Z;
                if (s == 3) p.Z = b.lo.Z;
                keep(Point{p, side[s]->part});
            }
        }
    }
    return out;
}

FVector PushOut(const FVector& point, const FVector& impact, const FVector& normal, double clearance_cm,
                double max_cm) {
    const double n_len = Len(normal);
    if (!(n_len > 0.5) || !Finite(point) || !Finite(impact)) return FVector{};
    const FVector n = normal * (1.0 / n_len);
    const double depth = Dot(impact - point, n);  // > 0: the point is behind the surface
    if (!(depth > 0.0)) return FVector{};
    return n * std::fmin(depth + std::fmax(0.0, clearance_cm), std::fmax(0.0, max_cm));
}

void Bounds(const std::vector<FVector>& points, FVector* centre, double* radius) {
    *centre = FVector{};
    *radius = 0;
    if (points.empty()) return;
    FVector lo = points[0], hi = points[0];
    for (const FVector& p : points) {
        lo = FVector{std::min(lo.X, p.X), std::min(lo.Y, p.Y), std::min(lo.Z, p.Z)};
        hi = FVector{std::max(hi.X, p.X), std::max(hi.Y, p.Y), std::max(hi.Z, p.Z)};
    }
    *centre = (lo + hi) * 0.5;
    for (const FVector& p : points) *radius = std::max(*radius, Len(p - *centre));
}

Hit LookPast(const LineFn& once, const SkipFn& skip, const LeaveOutFn& leave_out, const FVector& from,
             const FVector& to, int tries) {
    if (!once) return Hit{};
    for (int i = 0; i < tries; ++i) {
        const Hit h = once(from, to);
        if (!h.hit || h.eyes_inside || !skip || !skip(h.what)) return h;
        if (leave_out) leave_out(h.what);
    }
    return Hit{};
}

std::vector<int> GroupsAlongBarrel(const std::vector<Point>& points, double group_cm, int max_groups) {
    std::vector<int> out(points.size(), 0);
    if (points.empty() || !(group_cm > 1.0) || max_groups <= 1) return out;
    double lo = points[0].at.X, hi = points[0].at.X;
    for (const Point& p : points) lo = std::min(lo, p.at.X), hi = std::max(hi, p.at.X);
    const int n = std::clamp(static_cast<int>(std::ceil((hi - lo) / group_cm)), 1, max_groups);
    if (n == 1) return out;
    const double step = (hi - lo) / n;
    for (std::size_t i = 0; i < points.size(); ++i)
        out[i] = std::clamp(static_cast<int>((points[i].at.X - lo) / step), 0, n - 1);
    return out;
}

Result Solve(const FVector& eyes, const std::vector<FVector>& points, const std::vector<Part>& parts,
             const LineFn& line, double clearance_cm, double max_cm, int passes, const std::vector<int>& groups,
             const CloseFn& close) {
    Result r;
    if (!line || points.empty()) return r;
    const bool grouped = groups.size() == points.size();
    int group_count = 1;
    if (grouped)
        for (int g : groups) group_count = std::max(group_count, g + 1);
    std::vector<char> looked(static_cast<std::size_t>(group_count), 1);
    for (int pass = 0; pass < passes; ++pass) {
        if (close) {
            // Each group's ball (its points moved by the push so far).
            for (int g = 0; g < group_count; ++g) {
                std::vector<FVector> mine;
                for (std::size_t i = 0; i < points.size(); ++i)
                    if (!grouped || groups[i] == g) mine.push_back(points[i] + r.push);
                FVector centre;
                double radius = 0;
                Bounds(mine, &centre, &radius);
                looked[g] = !mine.empty() && close(eyes, centre, radius + std::fmax(0.0, clearance_cm));
                if (pass == 0 && looked[g]) ++r.groups_close;
            }
        }
        double deepest = 0;
        int best = -1;
        FVector best_push;
        std::uintptr_t best_what = 0;
        for (std::size_t i = 0; i < points.size(); ++i) {
            if (!looked[grouped ? groups[i] : 0]) continue;
            const FVector p = points[i] + r.push;
            const Hit h = line(eyes, p);
            ++r.queries;
            if (h.eyes_inside) {
                // As before: with the eyes in something, nothing seen from
                // them can be trusted. Keep what was pushed already.
                r.eyes_inside = true;
                return r;
            }
            if (!h.hit) continue;
            const FVector more = PushOut(p, h.impact, h.normal, clearance_cm, max_cm);
            const double size = Len(more);
            if (size < 0.01) continue;
            if (pass == 0) ++r.pushed_points;
            if (size > deepest) {
                deepest = size;
                best = static_cast<int>(i);
                best_push = more;
                best_what = h.what;
            }
        }
        if (best < 0) break;
        r.push = r.push + best_push;
        if (pass == 0) {
            r.deepest = best;
            r.deepest_part = static_cast<std::size_t>(best) < parts.size() ? parts[best] : Part::kGun;
            r.what = best_what;
        }
        const double total = Len(r.push);
        if (total > max_cm && total > 0) {
            r.push = r.push * (max_cm / total);
            break;
        }
    }
    return r;
}

}  // namespace holevr::vr::gun_walls
