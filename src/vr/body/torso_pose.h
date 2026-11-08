#pragma once
// Visual torso placement. Game animation owns stance; tracking owns arm
// dimensions and, only in real-height mode, additional physical crouch.
// Stateless: every result starts from this frame's copied animation.
namespace holevr::vr::body::torso {

enum class Mode { Fixed, Seated, Real };
struct Input {
    Mode mode = Mode::Fixed;
    double source_pelvis_z = 0;
    double source_top_spine_z = 0;
    double source_shoulders_z = 0;
    double target_shoulders_z = 0;
    double head_z = 0, game_camera_z = 0;
    bool camera_known = false;
    double max_crouch_cm = 70;
};
struct Output {
    bool valid = false;
    double pelvis_offset = 0;
    double spine_scale = 1;
    double source_pelvis_z = 0;
    double fitted_shoulders_z = 0;
    double shoulder_residual = 0;
    // Supply ORIGINAL copied spine positions, not already adjusted ones.
    double MapSpineZ(double source_z) const;
};
Output Fit(const Input& input);

} // namespace holevr::vr::body::torso
