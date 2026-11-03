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
    // Joint 0 (the knuckle) does not move with the curl, so it is left out:
    // only what the curl can change decides the pick.
    check(j[1].translation);
    check(j[2].translation);
    check(tip);
    // Halfway along the last bone too, so a fingertip can't slip past an edge.
    check((j[2].translation + tip) * 0.5);
    return deepest;
}

double CurlClearOfPlane(const Chain& c, const Limits& lim, const skel::Xf& hand, double want, double max_curl,
                        const FVector& point, const FVector& normal, double radius, double* depth) {
    want = std::clamp(want, 0.0, max_curl);
    auto at = [&](double curl) { return PlaneDepth(c, Curl(c, curl, lim), hand, point, normal, radius); };
    double best = want, best_depth = at(want);
    if (best_depth > 0.0) {
        // Step out from the wanted curl both ways, nearest first.
        constexpr double kStep = 0.05;
        for (int n = 1; n * kStep <= max_curl + 1e-9; ++n) {
            bool any = false;
            for (double curl : {want - n * kStep, want + n * kStep}) {
                if (curl < -1e-9 || curl > max_curl + 1e-9) continue;
                any = true;
                curl = std::clamp(curl, 0.0, max_curl);
                const double d = at(curl);
                if (d < best_depth) best = curl, best_depth = d;
                if (d <= 0.0) break;
            }
            if (best_depth <= 0.0 || !any) break;
        }
    }
    if (depth) *depth = best_depth;
    return best;
}

}  // namespace holevr::vr::fingers
