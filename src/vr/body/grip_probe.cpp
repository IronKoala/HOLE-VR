#include "vr/body/grip_probe.h"

#include <algorithm>
#include <limits>

namespace holevr::vr::grip {

using namespace ik;

namespace {
const double kNaN = std::numeric_limits<double>::quiet_NaN();

double Median(std::vector<double> v) {
    if (v.empty()) return kNaN;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

struct Seg {
    int a, b;  // first and last occupied column
};

std::vector<Seg> Segments(const Grid& g, int iz) {
    std::vector<Seg> out;
    int start = -1;
    for (int ix = 0; ix <= g.nx; ++ix) {
        bool occ = ix < g.nx && g.Occ(ix, iz);
        if (occ && start < 0) start = ix;
        if (!occ && start >= 0) {
            out.push_back({start, ix - 1});
            start = -1;
        }
    }
    return out;
}
}  // namespace

void Grid::Init(double x0_, double x1, double z0_, double z1, double step_) {
    x0 = x0_;
    z0 = z0_;
    step = step_;
    nx = std::max(1, static_cast<int>(std::floor((x1 - x0) / step)) + 1);
    nz = std::max(1, static_cast<int>(std::floor((z1 - z0) / step)) + 1);
    yr.assign(static_cast<size_t>(nx) * nz, kNaN);
    yl.assign(static_cast<size_t>(nx) * nz, kNaN);
}

bool Grid::Occ(int ix, int iz) const {
    if (ix < 0 || iz < 0 || ix >= nx || iz >= nz) return false;
    size_t i = static_cast<size_t>(iz) * nx + ix;
    return !std::isnan(yr[i]) || !std::isnan(yl[i]);
}

bool Grid::Inside(const FVector& p, double* depth) const {
    int ix = static_cast<int>(std::lround((p.X - x0) / step));
    int iz = static_cast<int>(std::lround((p.Z - z0) / step));
    if (depth) *depth = 0;
    if (ix < 0 || iz < 0 || ix >= nx || iz >= nz) return false;
    size_t i = static_cast<size_t>(iz) * nx + ix;
    double r = yr[i], l = yl[i];
    if (std::isnan(r) && std::isnan(l)) return false;
    if (std::isnan(r)) r = l;  // one side only: treat as a thin wall
    if (std::isnan(l)) l = r;
    if (p.Y > r || p.Y < l) return false;
    if (depth) *depth = std::min(r - p.Y, p.Y - l);
    return true;
}

Shape Analyze(const Grid& g, double thumb_above) {
    Shape s;
    const double st = g.step;
    // Silhouette bounds.
    int lo_z = -1, hi_z = -1, lo_x = g.nx, hi_x = -1;
    for (int iz = 0; iz < g.nz; ++iz)
        for (int ix = 0; ix < g.nx; ++ix)
            if (g.Occ(ix, iz)) {
                ++s.cells;
                if (lo_z < 0) lo_z = iz;
                hi_z = iz;
                lo_x = std::min(lo_x, ix);
                hi_x = std::max(hi_x, ix);
            }
    if (s.cells < 40) {
        s.why = "the scan hit almost nothing";
        return s;
    }
    s.length = (hi_x - lo_x + 1) * st;
    s.height = (hi_z - lo_z + 1) * st;
    if (lo_z == 0 || hi_z == g.nz - 1 || lo_x == 0 || hi_x == g.nx - 1) {
        // Not fatal (a long barrel or a light can poke out), but the bottom
        // of the grip must be inside the scan.
        if (lo_z == 0) {
            s.why = "the grip runs out of the bottom of the scan";
            return s;
        }
    }

    // Bottom of the grip: lowest row with a real segment (at least 1 cm).
    int bottom = -1;
    Seg grip{0, 0};
    for (int iz = lo_z; iz <= hi_z && bottom < 0; ++iz) {
        for (const Seg& sg : Segments(g, iz))
            if ((sg.b - sg.a + 1) * st >= 1.0 && (bottom < 0 || sg.b - sg.a > grip.b - grip.a)) {
                bottom = iz;
                grip = sg;
            }
    }
    if (bottom < 0) {
        s.why = "no grip bottom";
        return s;
    }
    s.z_bottom = g.Z(bottom) - st * 0.5;

    // Walk up the grip. The trigger guard is the first thing in front of
    // the grip's front strap (joined to it or not).
    struct Row {
        int iz;
        Seg seg;
    };
    std::vector<Row> rows;
    int guard = -1;
    Seg prev = grip;
    for (int iz = bottom; iz <= hi_z; ++iz) {
        std::vector<Seg> segs = Segments(g, iz);
        const Seg* cur = nullptr;
        for (const Seg& sg : segs)
            if (sg.b >= prev.a - 2 && sg.a <= prev.b + 2) {
                cur = &sg;
                break;
            }
        if (!cur) {
            s.why = "the grip outline breaks off";
            return s;
        }
        double height = g.Z(iz) - s.z_bottom;
        if (height > 3.0) {
            bool ahead = false;
            for (int ix = prev.b + static_cast<int>(std::ceil(1.5 / st)); ix < g.nx; ++ix) {
                // Only right in front of the grip: a weapon light or rail
                // further forward is not the guard.
                if (g.X(ix) > g.X(prev.b) + 5.0) break;
                if (g.Occ(ix, iz)) {
                    ahead = true;
                    break;
                }
            }
            if (ahead) {
                guard = iz;
                break;
            }
        }
        rows.push_back({iz, *cur});
        prev = *cur;
    }
    if (guard < 0) {
        s.why = "no trigger guard found in front of the grip";
        return s;
    }
    s.z_guard = g.Z(guard) - st * 0.5;
    if (s.z_guard - s.z_bottom < 4.0 || s.z_guard - s.z_bottom > 18.0 || rows.size() < 6) {
        s.why = "grip height out of range";
        return s;
    }

    // Grip axis: straight-line fit of the middle of each row (skip the very
    // bottom where a base plate may flare).
    double sz = 0, sx = 0, szz = 0, szx = 0;
    int n = 0;
    std::vector<double> rights, lefts;
    for (size_t i = 1; i < rows.size(); ++i) {
        const Row& r = rows[i];
        double mid = 0.5 * (g.X(r.seg.a) + g.X(r.seg.b));
        double z = g.Z(r.iz);
        sz += z;
        sx += mid;
        szz += z * z;
        szx += z * mid;
        ++n;
        int c = (r.seg.a + r.seg.b) / 2;
        size_t k = static_cast<size_t>(r.iz) * g.nx + c;
        if (!std::isnan(g.yr[k])) rights.push_back(g.yr[k]);
        if (!std::isnan(g.yl[k])) lefts.push_back(g.yl[k]);
    }
    double den = n * szz - sz * sz;
    double slope = std::fabs(den) > 1e-9 ? (n * szx - sz * sx) / den : 0.0;  // dx/dz
    s.slant_deg = std::atan(slope) / kDegToRad;
    s.up = Normalize(FVector{slope, 0, 1});
    s.y_right = Median(rights);
    s.y_left = Median(lefts);
    if (std::isnan(s.y_right) || std::isnan(s.y_left) || s.y_right - s.y_left < 1.0 ||
        s.y_right - s.y_left > 8.0) {
        s.why = "grip width out of range";
        return s;
    }

    // Front and back strap just under the guard.
    const Row& ref = rows[rows.size() >= 3 ? rows.size() - 2 : rows.size() - 1];
    s.front = g.X(ref.seg.b) + st * 0.5;
    s.back = g.X(ref.seg.a) - st * 0.5;
    if (s.front - s.back < 2.0 || s.front - s.back > 12.0) {
        s.why = "grip depth out of range";
        return s;
    }

    // Thumb: back of the frame and its left side, a little above the guard.
    s.thumb_z = s.z_guard + thumb_above;
    int tz = static_cast<int>(std::lround((s.thumb_z - g.z0) / st));
    tz = std::clamp(tz, 0, g.nz - 1);
    s.thumb_x = s.back;
    s.thumb_y = s.y_left;
    for (const Seg& sg : Segments(g, tz)) {
        if (sg.b < (s.back - g.x0) / st - 4 || g.X(sg.a) > s.front) continue;
        s.thumb_x = g.X(sg.a) - st * 0.5;
        std::vector<double> l;
        for (int ix = sg.a; ix <= sg.b && g.X(ix) < s.thumb_x + 6.0; ++ix) {
            double v = g.yl[static_cast<size_t>(tz) * g.nx + ix];
            if (!std::isnan(v)) l.push_back(v);
        }
        if (!l.empty()) s.thumb_y = Median(l);
        break;
    }
    s.ok = true;
    return s;
}

std::vector<FVector> PalmPoints(const HandGeo& h, double palm_skin) {
    std::vector<FVector> out;
    for (int f = 1; f < 5; ++f)
        for (double t : {0.35, 0.6, 0.85, 1.0}) out.push_back(h.mcp[f] * t + h.palm * palm_skin);
    return out;
}

namespace {
Placement PlaceAt(const Grid& g, const Shape& s, const HandGeo& h, const Params& p, double yaw) {
    Placement out;
    // Pinky -> index runs up the grip, the palm faces into the gun's right
    // side, then the hand turns around the grip axis by `yaw`.
    out.rot = AxisAngle(s.up, yaw * kDegToRad) * AlignFrames(h.across, h.palm, s.up, FVector{0, -1, 0});
    out.yaw_deg = yaw;
    // Middle finger knuckle: one finger radius under the trigger guard, at the
    // front strap, palm skin on the right side of the grip.
    double z = s.z_guard - p.finger_radius;
    double front = s.front + (z - (s.z_guard - 1.0)) * s.up.X / std::max(0.2, s.up.Z);
    FVector knuckle{front - p.knuckle_back, s.y_right + p.palm_skin + p.clearance, z};
    out.pos = knuckle - out.rot.Rotate(h.mcp[2]);
    // No palm point may be inside the gun: slide the hand out to the right.
    std::vector<FVector> pts = PalmPoints(h, p.palm_skin);
    for (int it = 0; it < 6; ++it) {
        double worst = 0;
        for (const FVector& lp : pts) {
            double d;
            if (g.Inside(out.pos + out.rot.Rotate(lp), &d)) worst = std::max(worst, d);
        }
        if (worst <= 1e-3) break;
        out.pos.Y += worst + p.clearance;
        out.pushed += worst + p.clearance;
    }
    return out;
}
}  // namespace

Placement PlaceRightHand(const Grid& g, const Shape& s, const HandGeo& h, const Params& p) {
    // Turn the hand around the grip until the wrist is behind the gun's
    // centre line: the gun in line with the forearm (reference photos).
    const double centre = 0.5 * (s.y_right + s.y_left);
    Placement out = PlaceAt(g, s, h, p, 0.0);
    const double knuckle_y = s.y_right + p.palm_skin + p.clearance;
    auto cost = [&](const Placement& c) {
        FVector mid = c.pos + c.rot.Rotate(h.mcp[2]);
        return std::fabs(c.pos.Y - centre) + 1.0 * std::max(0.0, mid.Y - knuckle_y) + 0.01 * std::fabs(c.yaw_deg);
    };
    double best = cost(out);
    for (double yaw = -p.inline_max_yaw; yaw <= p.inline_max_yaw + 1e-9; yaw += 1.0) {
        Placement c = PlaceAt(g, s, h, p, yaw);
        double err = cost(c);
        if (err < best) {
            best = err;
            out = c;
        }
    }
    // Thumb knuckle at the back left corner of the frame, just above the
    // guard; the thumb then runs forward along the left side.
    out.thumb = FVector{s.thumb_x + 0.5, s.thumb_y - p.finger_radius - p.clearance, s.thumb_z};
    out.thumb_dir = FVector{1, 0, 0};
    return out;
}

double RightSide(const Grid& g, double x0, double x1, double z0, double z1) {
    double best = std::numeric_limits<double>::quiet_NaN();
    for (int iz = 0; iz < g.nz; ++iz) {
        if (g.Z(iz) < z0 || g.Z(iz) > z1) continue;
        for (int ix = 0; ix < g.nx; ++ix) {
            if (g.X(ix) < x0 || g.X(ix) > x1) continue;
            double v = g.yr[static_cast<size_t>(iz) * g.nx + ix];
            if (!std::isnan(v) && (std::isnan(best) || v > best)) best = v;
        }
    }
    return best;
}

double Penetration(const Grid& g, const std::vector<FVector>& pts, double radius, int* at) {
    double worst = 0;
    if (at) *at = -1;
    auto test = [&](const FVector& q, int i) {
        double d;
        if (g.Inside(q, &d) && d > worst) {
            worst = d;
            if (at) *at = i;
        }
    };
    for (size_t i = 0; i + 1 < pts.size() || (pts.size() == 1 && i == 0); ++i) {
        FVector a = pts[i], b = i + 1 < pts.size() ? pts[i + 1] : pts[i];
        const int steps = std::max(4, static_cast<int>(Length(b - a) / 0.4));
        for (int k = 0; k <= steps; ++k) {
            FVector q = a + (b - a) * (static_cast<double>(k) / steps);
            // A finger is a tube: its skin reaches `radius` past the bone line.
            test(q, static_cast<int>(i));
            if (radius > 0) {
                for (const FVector& o : {FVector{radius, 0, 0}, FVector{-radius, 0, 0}, FVector{0, radius, 0},
                                         FVector{0, -radius, 0}, FVector{0, 0, radius}, FVector{0, 0, -radius}})
                    test(q + o, static_cast<int>(i));
            }
        }
        if (pts.size() == 1) break;
    }
    return worst;
}

}  // namespace holevr::vr::grip
