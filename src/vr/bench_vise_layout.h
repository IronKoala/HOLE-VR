#pragma once
#include <algorithm>
#include "vr/math.h"

namespace holevr::vr::bench_vise::layout {
// Centimetres, measured on DS_Workbench and the final vise mesh.
inline constexpr double kBaseInset = 10.0;
inline constexpr double kSurfaceLift = 0.1;
// Leave visible tabletop between loose parts and the 22.71 cm-wide vise.
inline constexpr double kPartsClearance = 20.0;
// The bench bounds include a raised back lip. If the tabletop trace misses,
// keep the base on the actual flat surface instead of floating on lip height.
inline constexpr double kTopBelowBounds = 2.308142;
inline FVector Seat(const FVector& edge_center, const FVector& out, double table_z) {
    FVector p = edge_center - out * kBaseInset;
    p.Z = table_z + kSurfaceLift;
    return p;
}

struct PartSpot { double along, inset; };
// Place parts on either side, keeping their full rotated bounds clear of
// the vise and the table edge. Only wrap inward when a side's row is full.
class PartsRow {
public:
    explicit PartsRow(double front=2) { front_[0]=front_[1]=std::max(2.0,front); }
    PartSpot Next(double half_width, double half_depth, double table_half_width) {
        const int side = next_++ % 2;
        half_width = std::max(half_width, 0.5);
        half_depth = std::max(half_depth, 0.5);
        if (cursor_[side] + 2*half_width > table_half_width - 3 && cursor_[side] > kPartsClearance) {
            front_[side] += depth_[side] + 3;
            cursor_[side] = kPartsClearance;
            depth_[side] = 0;
        }
        const double along = cursor_[side] + half_width;
        const double inset = std::max(7.0, front_[side] + half_depth);
        cursor_[side] += 2*half_width + 2;
        depth_[side] = std::max(depth_[side], inset + half_depth - front_[side]);
        return {side == 0 ? -along : along, inset};
    }
private:
    int next_ = 0;
    double cursor_[2] = {kPartsClearance, kPartsClearance};
    double front_[2] = {2, 2}, depth_[2] = {0, 0};
};
}  // namespace holevr::vr::bench_vise::layout
