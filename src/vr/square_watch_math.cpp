#include "vr/square_watch_math.h"

#include <algorithm>

namespace holevr::vr::square_watch {

float Luma(uint32_t px, bool bgra, bool ten_bit) {
    float r, g, b;
    if (ten_bit) {
        r = (px & 0x3ff) / 1023.f;
        g = ((px >> 10) & 0x3ff) / 1023.f;
        b = ((px >> 20) & 0x3ff) / 1023.f;
    } else {
        const float c0 = (px & 0xff) / 255.f, c1 = ((px >> 8) & 0xff) / 255.f, c2 = ((px >> 16) & 0xff) / 255.f;
        r = bgra ? c2 : c0;
        g = c1;
        b = bgra ? c0 : c2;
    }
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

namespace {

// Summed-area table: mean of any block in four lookups.
struct Sums {
    int w = 0, h = 0;
    const double* s = nullptr;  // (w + 1) x (h + 1)
    double At(int x, int y) const { return s[static_cast<size_t>(y) * (w + 1) + x]; }
    // Mean of the b x b block at (x, y) in pixels of the whole band.
    float Mean(int x, int y, int b) const {
        const double sum = At(x + b, y + b) - At(x, y + b) - At(x + b, y) + At(x, y);
        return static_cast<float>(sum / (static_cast<double>(b) * b));
    }
};

Sums Build(const Band& band, std::vector<double>& store) {
    Sums t;
    t.w = band.width;
    t.h = band.height;
    store.assign(static_cast<size_t>(t.w + 1) * (t.h + 1), 0.0);
    for (int y = 0; y < t.h; ++y) {
        double row = 0;
        for (int x = 0; x < t.w; ++x) {
            row += band.luma[static_cast<size_t>(y) * t.w + x];
            store[static_cast<size_t>(y + 1) * (t.w + 1) + x + 1] = store[static_cast<size_t>(y) * (t.w + 1) + x + 1] + row;
        }
    }
    t.s = store.data();
    return t;
}

int SceneShiftFrom(const Sums& t, int eye_w, const Settings& s) {
    // Compare 16 px blocks of the left eye with the right eye's at each shift.
    const int b = 16, h = t.h, step = std::max(1, s.step);
    if (eye_w < 4 * b || h < b) return 0;
    const int all = (eye_w / b) * (h / b);
    double best = 1e30;
    int best_shift = 0;
    for (int d = -s.max_scene_shift; d <= s.max_scene_shift; d += step) {
        double sad = 0;
        int n = 0;
        for (int y = 0; y + b <= h; y += b) {
            for (int x = 0; x + b <= eye_w; x += b) {
                const int ox = x - d;
                if (ox < 0 || ox + b > eye_w) continue;
                const float a = t.Mean(x, y, b), c = t.Mean(eye_w + ox, y, b);
                sad += a > c ? a - c : c - a;
                ++n;
            }
        }
        // Too little overlap to judge.
        if (n * 2 < all) continue;
        sad /= n;
        // Prefer small shifts when two are about as good.
        sad += 1e-6 * (d < 0 ? -d : d);
        if (sad < best) {
            best = sad;
            best_shift = d;
        }
    }
    return best_shift;
}

}  // namespace

int SceneShift(const Band& band, const Settings& s) {
    const int eye_w = band.width / 2;
    if (eye_w <= 0 || band.height <= 0 || band.luma.size() < static_cast<size_t>(band.width) * band.height) return 0;
    std::vector<double> store;
    return SceneShiftFrom(Build(band, store), eye_w, s);
}

Odd FindOdd(const Band& band, const Settings& s, Scratch* scratch) {
    Odd odd;
    const int eye_w = band.width / 2, h = band.height, b = s.block;
    if (b <= 0 || s.step <= 0 || eye_w < b || h < b ||
        band.luma.size() < static_cast<size_t>(band.width) * band.height)
        return odd;
    Scratch local;
    const Sums t = Build(band, (scratch ? scratch : &local)->sums);
    odd.scene_shift = SceneShiftFrom(t, eye_w, s);
    for (int eye = 0; eye < 2; ++eye) {
        const int base = eye * eye_w, other = (1 - eye) * eye_w;
        // Where a point in this eye shows in the other: left eye x - shift,
        // right eye x + shift.
        const int dir = eye == 0 ? -1 : 1;
        for (int y = 0; y + b <= h; y += b) {
            for (int x = 0; x + b <= eye_w; x += b) {
                const float m = t.Mean(base + x, y, b);
                if (m < s.bright) continue;
                ++odd.checked[eye];
                bool any = false, matched = false;
                for (int dy = -s.max_up_down; dy <= s.max_up_down && !matched; dy += s.step) {
                    const int oy = y + dy;
                    if (oy < 0 || oy + b > h) continue;
                    for (int d = s.min_shift; d <= s.max_shift; d += s.step) {
                        const int ox = x + dir * (odd.scene_shift + d);
                        if (ox < 0 || ox + b > eye_w) continue;
                        any = true;
                        if (t.Mean(other + ox, oy, b) >= m - s.margin) {
                            matched = true;
                            break;
                        }
                    }
                }
                if (!any || matched) continue;
                if (odd.count[eye]++ == 0) {
                    odd.first_x[eye] = x;
                    odd.first_y[eye] = y;
                }
            }
        }
    }
    return odd;
}

Dots CountDots(const Band& band, int y0, const DotSettings& s) {
    Dots d;
    const int eye_w = band.width / 2, h = band.height, W = band.width;
    if (eye_w < 5 || h < 5 || band.luma.size() < static_cast<size_t>(band.width) * band.height) return d;
    const float* L = band.luma.data();
    for (int eye = 0; eye < 2; ++eye) {
        const int base = eye * eye_w;
        for (int y = 2; y < h - 2; ++y) {
            const float* row = L + static_cast<size_t>(y) * W + base;
            for (int x = 2; x < eye_w - 2; ++x) {
                const float v = row[x];
                if (v < s.min_luma) continue;
                // At least as bright as its four neighbours (one pixel of a
                // pair still counts, a smooth patch's pixels mostly don't).
                if (v < row[x - 1] || v < row[x + 1] || v < row[x - W] || v < row[x + W]) continue;
                // At most two of its eight neighbours anywhere near as bright:
                // a dot, a pair or a one-pixel line, not the corner of a patch.
                const float half = 0.5f * v;
                int near = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        if ((dx || dy) && row[x + dx + dy * W] >= half) ++near;
                if (near > 2) continue;
                // The 16 pixels two steps out.
                const float* up2 = row - 2 * W;
                const float* dn2 = row + 2 * W;
                float ring = 0.f;
                for (int dx = -2; dx <= 2; ++dx) ring += up2[x + dx] + dn2[x + dx];
                for (int dy = -1; dy <= 1; ++dy) ring += row[x - 2 + dy * W] + row[x + 2 + dy * W];
                ring *= 1.f / 16.f;
                if (v - ring < s.min_jump || v < s.min_ratio * ring) continue;
                ++d.count[eye];
                ++d.grid[eye][(x & 7) + 8 * ((y0 + y) & 3)];
            }
        }
    }
    return d;
}

float GridShare(const int grid[32]) {
    long long total = 0;
    int most = 0;
    for (int i = 0; i < 32; ++i) {
        total += grid[i];
        most = std::max(most, grid[i]);
    }
    return total > 0 ? static_cast<float>(most) / static_cast<float>(total) : 0.f;
}

}  // namespace holevr::vr::square_watch
