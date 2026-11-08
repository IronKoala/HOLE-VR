#include "vr/body/torso_pose.h"

#include <algorithm>
#include <cmath>

namespace holevr::vr::body::torso {
double Output::MapSpineZ(double source_z) const {
    if (!valid || !std::isfinite(source_z)) return source_z;
    return source_pelvis_z + pelvis_offset + (source_z-source_pelvis_z)*spine_scale;
}
Output Fit(const Input& in) {
    Output out;
    if (!std::isfinite(in.source_pelvis_z) || !std::isfinite(in.source_top_spine_z) ||
        !std::isfinite(in.source_shoulders_z) || !std::isfinite(in.target_shoulders_z)) return out;
    out.source_pelvis_z = in.source_pelvis_z;
    out.fitted_shoulders_z = in.source_shoulders_z;

    // The camera's existing game-crouch compensation is already present in
    // this difference. Do not subtract the raw standing eye height again.
    // A positive difference must not undo a game-controlled crouch.
    if (in.mode == Mode::Real && in.camera_known && std::isfinite(in.head_z) &&
        std::isfinite(in.game_camera_z) && std::isfinite(in.max_crouch_cm)) {
        const double drop = in.head_z-in.game_camera_z;
        if (std::isfinite(drop)) out.pelvis_offset = std::clamp(drop, -std::clamp(in.max_crouch_cm,0.0,120.0), 0.0);
    }
    const double height = in.source_top_spine_z-in.source_pelvis_z;
    if (!std::isfinite(height) || height < 5 || height > 150) return {};
    const double requested = in.target_shoulders_z-in.source_shoulders_z-out.pelvis_offset;
    if (!std::isfinite(requested)) return {};
    // A positive affine scale preserves spine order and limits each vertical
    // segment to half through one-and-a-half its source extent. Clavicles
    // inherit the last spine's displacement, while arm lengths are untouched.
    out.spine_scale = std::clamp(1.0+requested/height,0.5,1.5);
    out.fitted_shoulders_z = in.source_shoulders_z+out.pelvis_offset+height*(out.spine_scale-1);
    out.shoulder_residual = in.target_shoulders_z-out.fitted_shoulders_z;
    if (!std::isfinite(out.fitted_shoulders_z) || !std::isfinite(out.shoulder_residual)) return {};
    out.valid = true;
    return out;
}
} // namespace holevr::vr::body::torso
