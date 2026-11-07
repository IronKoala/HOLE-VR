#pragma once
#include <algorithm>
#include <cmath>
#include "vr/bench_vise_motion.h"

namespace holevr::vr::bench_vise::fit {
// Mesh-local centimetres, measured from HOLE's actual assets. Width is the
// mesh cross section inside the 9.9 x 5.85 cm jaw pad, not the entire gun.
// SR rests on its solid fore-end: clamping its sloping wrist would put the
// buttstock through the lower casting. HG/AR/SG use the bottom of the grip.
struct Profile {
    const char* category;
    const char* component;
    const char* mesh;
    FVector contact;
    double width;
    FVector lo, hi;
};
inline constexpr Profile kProfiles[] = {
    // Offset the grip toward the rear of the pads and lift it 2.5 cm.
    // This keeps the 24/33-round magazines outside the casting and leaves
    // the longest magazine above the tabletop in either barrel direction.
    {"HG", "HG_Mk3_9mm_00_MainFrame", "HG_9x19_S_00_MainFrame", {-0.5,0,-7.2}, 3.196,
     {-8.155,-1.598,-7.990}, {9.596,1.598,1.478}},
    {"AR", "AR_Truss556_07_Grip", "AR_556x45_F_07_Grip", {-1.5,0,-2.8}, 2.381016,
     {-5.257,-1.588,-6.002}, {3.500,1.588,6.696}},
    {"SG", "SG_Tombo12_09_Grip", "SG_Tombo12_09_Grip", {-5.4,0,-6.8}, 2.996003,
     {-8.466,-1.544,-9.963}, {.184,1.544,5.410}},
    {"SR", "SR_762x51_B_01_Stock", "SR_762x51_B_01_Stock", {33,0,1.5}, 3.613942,
     {-26.400,-1.921,-8.067}, {45.440,1.921,4.343}}
};
inline bool MatchesBounds(const Profile& p, const FVector& lo, const FVector& hi) {
    const double errors[]={lo.X-p.lo.X,lo.Y-p.lo.Y,lo.Z-p.lo.Z,hi.X-p.hi.X,hi.Y-p.hi.Y,hi.Z-p.hi.Z};
    for (double e:errors) if (!std::isfinite(e) || std::fabs(e)>.12) return false;
    return true;
}
inline bool Gap(const Profile& p, const FQuat& mesh_to_gun, const FVector& scale, double* gap) {
    if (!std::isfinite(scale.X)||!std::isfinite(scale.Y)||!std::isfinite(scale.Z) ||
        scale.X<=0 || scale.Y<=0 || scale.Z<=0) return false;
    double width=0;
    for (int k=0;k<8;++k) {
        const FVector d{(k&1?4.95:-4.95)*scale.X,(k&2?.5:-.5)*p.width*scale.Y,
                        (k&4?2.925:-2.925)*scale.Z};
        const FVector v=mesh_to_gun.Rotate(d);
        if (!std::isfinite(v.Y)) return false;
        width=std::max(width,2*std::fabs(v.Y));
    }
    if (width<.5 || width+motion::kGripClearance>motion::kOpenGap) return false;
    *gap=motion::LoadedGap(width);
    return true;
}

// All world transforms in a sample come from the live actor. A remembered
// controller/gun pose belongs to a different tick: mixing it with these
// transforms makes both the contact offset and jaw width depend on the last
// hand movement or the base game's intervening animation.
struct Sample {
    FQuat actor_rotation, native_frame, mesh_rotation;
    FVector mesh_location, mesh_scale{1,1,1}, main_location;
};
inline bool Measure(const Profile& p, const Sample& s, FVector* contact_from_main, double* gap,
                    FQuat* mesh_in_frame = nullptr) {
    for (const FQuat& q : {s.actor_rotation, s.native_frame, s.mesh_rotation}) {
        const double length = q.X*q.X+q.Y*q.Y+q.Z*q.Z+q.W*q.W;
        if (!std::isfinite(length) || std::fabs(length-1) > 1e-4) return false;
    }
    const FQuat world_to_frame = (s.actor_rotation*s.native_frame).Inverse();
    const FVector scaled{p.contact.X*s.mesh_scale.X,p.contact.Y*s.mesh_scale.Y,p.contact.Z*s.mesh_scale.Z};
    const FVector contact = world_to_frame.Rotate(s.mesh_location+s.mesh_rotation.Rotate(scaled)-s.main_location);
    double measured_gap = 0;
    // Profiles and the static fit references use mesh +X down the barrel,
    // +Z up. Seat that physical mesh upright, undoing the base game's tilt
    // under the actor's approximate gun frame. Its clamp width is then Y.
    if (!std::isfinite(contact.X) || !std::isfinite(contact.Y) || !std::isfinite(contact.Z) ||
        !Gap(p,FQuat{},s.mesh_scale,&measured_gap)) return false;
    *contact_from_main = contact;
    *gap = measured_gap;
    if (mesh_in_frame) *mesh_in_frame = world_to_frame*s.mesh_rotation;
    return true;
}
}  // namespace holevr::vr::bench_vise::fit
