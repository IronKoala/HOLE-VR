#include "vr/body/finger_rest.h"

#include <algorithm>
#include <cmath>

#include "vr/body/ik_math.h"

namespace holevr::vr::fingers {

double PlaneDepth(const Chain& c, const Angles& a, const skel::Xf& hand, const FVector& point,
                  const FVector& normal, double radius) {
    skel::Xf j[3];
    FVector tip;
    Fk(c, a, j, &tip);
    double deepest = 0.0;
    auto check = [&](const FVector& local) {
        const FVector w = skel::Apply(hand, local);
        deepest = std::max(deepest, ik::Dot(point - w, normal) + radius);
    };
    // Joint 0 (the knuckle) does not move with the bend, so it is left out:
    // only what the bend can change decides the pick.
    check(j[1].translation);
    check(j[2].translation);
    check(tip);
    // Halfway along the last bone too, so a fingertip can't slip past an edge.
    check((j[2].translation + tip) * 0.5);
    return deepest;
}

Angles Bend(const Chain& c, double bend, const Limits& lim) {
    if (bend >= 0.0) return Curl(c, bend, lim);
    Angles a;
    const double share = c.thumb ? 0.5 : 1.0;
    for (int j = 0; j < 3; ++j) a.flex[j] = -bend * lim.flex_min[j] * kDegToRad * share;
    return a;
}

Push PushFromPlane(const Chain& c, const Limits& lim, const skel::Xf& hand, double from, double want,
                   double max_curl, const FVector& point, const FVector& normal, double radius, int prefer) {
    constexpr double kLo = -1.0, kStep = 0.05;
    from = std::clamp(from, kLo, max_curl);
    want = std::clamp(want, kLo, max_curl);
    auto at = [&](double b) { return PlaneDepth(c, Bend(c, b, lim), hand, point, normal, radius); };
    // The clear end of a step that goes from clear `ok` into the surface at `in`.
    auto touch = [&](double ok, double in) {
        for (int i = 0; i < 6; ++i) {
            const double mid = (ok + in) * 0.5;
            (at(mid) > 0.0 ? in : ok) = mid;
        }
        return ok;
    };
    Push p;
    p.bend = from;
    p.depth = at(from);
    if (p.depth > 0.0) {
        // The hand moved it into the surface. Which way the surface pushes:
        // the side where a small change takes the finger out faster. Pressed
        // on its pad or nails the difference is large (about 1 cm a step);
        // straight on it is small and the finger bends back rather than
        // buckling into a fist (the look the owner didn't want). Last
        // frame's side wins a near tie, so it doesn't flicker.
        p.pushed = true;
        const double back = at(std::max(kLo, from - kStep)), fwd = at(std::min(max_curl, from + kStep));
        const double margin = prefer > 0 ? 0.0 : prefer < 0 ? 0.4 : 0.3;
        int dir = fwd < back - margin ? +1 : -1;
        // Out along one side: to the first clear bend, else the shallowest.
        auto out_along = [&](int way) {
            double prev = from;
            for (int n = 1;; ++n) {
                double b = from + way * n * kStep;
                if (b < kLo - 1e-9 || b > max_curl + 1e-9) break;
                b = std::clamp(b, kLo, max_curl);
                const double d = at(b);
                if (d <= 0.0) {
                    p.bend = touch(b, prev);
                    p.depth = 0.0;
                    return;
                }
                if (d < p.depth) p.bend = b, p.depth = d;
                prev = b;
            }
        };
        out_along(dir);
        // Nothing gained that way (already at its limit, or the guess was
        // wrong): the other way.
        if (p.bend == from) {
            dir = -dir;
            out_along(dir);
        }
        // Still in, or pushed away from what the controller wants: it stays
        // where the surface left it this frame.
        if (p.depth > 0.0 || (want - p.bend) * dir <= 0.0) {
            p.dir = dir;
            return p;
        }
    }
    // Clear: it bends toward the wanted bend and stops where it touches.
    const double start = p.bend;
    const int way = want > start ? +1 : -1;
    double ok = start;
    p.bend = want;
    for (int n = 1; way * (want - ok) > 1e-12; ++n) {
        const double b = way > 0 ? std::min(want, start + n * kStep) : std::max(want, start - n * kStep);
        if (at(b) > 0.0) {
            p.bend = touch(ok, b);
            break;
        }
        ok = b;
    }
    p.depth = 0.0;
    p.dir = p.bend < want - 1e-3 ? -1 : p.bend > want + 1e-3 ? +1 : 0;
    return p;
}

FVector TipWorld(const Chain& c, const Angles& a, const skel::Xf& hand) {
    skel::Xf j[3];
    FVector tip;
    Fk(c, a, j, &tip);
    return skel::Apply(hand, tip);
}

void DragTip(const Chain& c, const Limits& lim, const skel::Xf& hand, double bend, const FVector& move,
             double* d_bend, double* d_spread) {
    *d_bend = 0.0;
    *d_spread = 0.0;
    constexpr double kH = 0.02;
    const Angles a0 = Bend(c, bend, lim);
    const FVector t0 = TipWorld(c, a0, hand);
    const double hb = bend + kH <= 1.2 ? kH : -kH;
    const FVector jb = (TipWorld(c, Bend(c, bend + hb, lim), hand) - t0) * (1.0 / hb);
    Angles as = a0;
    as.spread += kH;
    const FVector js = (TipWorld(c, as, hand) - t0) * (1.0 / kH);
    // Least squares over the two: [jb js] x = move.
    const double a11 = ik::Dot(jb, jb), a12 = ik::Dot(jb, js), a22 = ik::Dot(js, js);
    const double r1 = ik::Dot(jb, move), r2 = ik::Dot(js, move);
    const double det = a11 * a22 - a12 * a12;
    double x1 = 0.0, x2 = 0.0;
    if (std::fabs(det) > 1e-9 * std::max(1.0, a11 * a22)) {
        x1 = (r1 * a22 - r2 * a12) / det;
        x2 = (a11 * r2 - a12 * r1) / det;
    } else if (a11 > 1e-9) {
        x1 = r1 / a11;
    }
    *d_bend = std::clamp(x1, -0.3, 0.3);
    *d_spread = std::clamp(x2, lim.spread_min * kDegToRad, lim.spread_max * kDegToRad);
}

}  // namespace holevr::vr::fingers
