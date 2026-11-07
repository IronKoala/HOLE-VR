#pragma once
#include <cstdint>
#include "vr/math.h"

namespace holevr::vr::holster_model {
// UE FTransform, verified against reflected function parameters before use.
struct alignas(16) Transform {
    FQuat rotation;
    FVector translation;
    double pad0 = 0;
    FVector scale{1, 1, 1};
    double pad1 = 0;
};
static_assert(sizeof(Transform) == 96);
// Same actor-from-grip construction as the existing holster. Keeping this
// pure makes calibrated position/rotation and mesh offsets testable.
inline Transform MeshAtSeat(const FVector& grip_world, const FQuat& frame, const FQuat& gun_frame,
                            const FVector& grip_rel, const FVector& actor_loc, const FQuat& actor_rot,
                            const Transform& mesh_world) {
    const FQuat inv = actor_rot.Inverse(), seat_rot = frame * gun_frame.Inverse();
    const FVector seat_loc = grip_world - seat_rot.Rotate(grip_rel);
    Transform out;
    out.translation = seat_loc + seat_rot.Rotate(inv.Rotate(mesh_world.translation - actor_loc));
    out.rotation = seat_rot * inv * mesh_world.rotation;
    out.scale = mesh_world.scale;
    return out;
}
// Native pistol MainFrame mesh's transform at the EXISTING calibrated seat.
// Game thread only. Owned by the player, independent of the moving gun.
void Show(uintptr_t player, const Transform& pistol_mesh_seat, double hood_open, double now);
void Hide();
}  // namespace holevr::vr::holster_model
