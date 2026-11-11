#pragma once
// Mesh-derived collision dimensions. Native dimensions and anatomical axes
// belong to the reference bone frame; runtime transforms include hand scale.
#include "vr/body/skeleton.h"

namespace holevr::vr::fingers { struct Chain; }
namespace holevr::vr::hand_collision_shape {
struct Segment {
    bool fitted = false;
    double world_radius_cm = 0;
    FVector side_axis{}, depth_axis{}, axial_axis{};
    FVector self_offset{}; // measured cross-section centre; world spheres remain bone-centred
    double side_half_cm = 0, depth_half_cm = 0, axial_half_cm = 0;
    double self_start_fraction = 0; // shared proximal web excluded only from self tests
};
struct WorldSection {
    bool fitted = false;
    double radius_cm = 0, self_start_fraction = 0;
    FVector side_half{}, depth_half{}, axial_half{};
    FVector self_offset{};
};
struct Palm {
    bool fitted = false;
    // hand-local X across, Y forward, Z palmward. No "{}" on axis[3]: FVector
    // zero-initialises itself, and GCC 13 rejects "return {}" for Palm with it.
    FVector centre{}, axis[3], half{};
};
// Converts the local palm to an orthonormal world box enclosing every scaled corner.
Palm TransformPalm(const Palm& local, const skel::Xf& hand);
WorldSection Transform(const Segment& shape, const skel::Xf& joint,
                       const skel::Xf& hand, double fallback_radius_cm);
double Support(const WorldSection& section, const FVector& unit_normal);
// Applies only after reference lengths and knuckle layout identify this mesh.
// Unknown rigs retain their configured fallback dimensions.
// Providing a palm output fits the combined palm/CMC envelope. Omitting it
// retains the larger standalone CMC capsule for callers without a palm box.
bool FitWhiteMan(fingers::Chain chains[5], const FVector& palm_normal, Palm* palm = nullptr);
} // namespace holevr::vr::hand_collision_shape
