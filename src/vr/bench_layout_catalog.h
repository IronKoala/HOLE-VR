#pragma once
#include <string>
#include "vr/math.h"

namespace holevr::vr::bench_layout {
// Verified from the same cooked Blueprint hierarchy as the private editor.
// Bottom light rail preferred; SG has side rails and uses its left rail.
// No game geometry is embedded: just row identities and actor orientation.
struct Part { const char* row; FQuat actor_in_mesh; };
inline constexpr Part kParts[] = {
    {"LIGHT_AR_LASER", {-1,0,0,0}},
    {"LIGHT_AR_LASER_GREEN", {-1,0,0,0}},
    {"LIGHT_AR_LIGHT", {-1,0,0,0}},
    {"LIGHT_HG_LASER", {0,0,0,1}},
    {"LIGHT_HG_LASER_GREEN", {0,0,0,1}},
    {"LIGHT_HG_LIGHT", {0,0,0,1}},
    {"LIGHT_SG_LASER", {0.70710678118654757,0,0,0.70710678118654757}},
    {"LIGHT_SG_LASER_GREEN", {0.70710678118654757,0,0,0.70710678118654757}},
    {"LIGHT_SG_LIGHT", {0.70710678118654757,0,0,0.70710678118654757}},
    {"LIGHT_SR_LASER", {-1,0,0,0}},
    {"LIGHT_SR_LASER_GREEN", {-1,0,0,0}},
    {"LIGHT_SR_LIGHT", {-1,0,0,0}},
    {"MUZZLE_AR_FH", {0,0,0,1}},
    {"MUZZLE_AR_MB", {0,0,0,1}},
    {"MUZZLE_AR_SUP", {0,0,0,1}},
    {"MUZZLE_HG_COM", {0,0,0,1}},
    {"MUZZLE_HG_SUP", {0,0,0,1}},
    {"MUZZLE_SG_BA", {0,0,0,1}},
    {"MUZZLE_SG_CK", {0,0,0,1}},
    {"MUZZLE_SG_SUP", {0,0,0,1}},
    {"MUZZLE_SR_MB", {0,0,0,1}},
    {"MUZZLE_SR_SUP", {0,0,0,1}},
    {"MUZZLE_SR_TP", {0,0,0,1}},
    {"SIGHT_AR_DOTSIGHT", {0,0,0,1}},
    {"SIGHT_AR_HOLOSIGHT", {0,0,0,1}},
    {"SIGHT_AR_IRONSIGHT", {0,0,0,1}},
    {"SIGHT_AR_SCOPE", {0,0,0,1}},
    {"SIGHT_AR_SCOPE_2X", {0,0,0,1}},
    {"SIGHT_HG_DOTSIGHT", {0,0,0,1}},
    {"SIGHT_HG_IRONSIGHT", {0,0,0,1}},
    {"SIGHT_SG_DOTSIGHT", {0,0,0,1}},
    {"SIGHT_SG_HOLOSIGHT", {0,0,0,1}},
    {"SIGHT_SG_IRONSIGHT", {0,0,0,1}},
    {"SIGHT_SG_SCOPE", {0,0,0,1}},
    {"SIGHT_SG_SCOPE_2X", {0,0,0,1}},
    {"SIGHT_SR_DOTSIGHT", {0,0,0,1}},
    {"SIGHT_SR_HOLOSIGHT", {0,0,0,1}},
    {"SIGHT_SR_IRONSIGHT", {0,0,0,1}},
    {"SIGHT_SR_SCOPE", {0,0,0,1}},
    {"SIGHT_SR_SCOPE_2X", {0,0,0,1}},
};
inline const Part* FindPart(const std::string& row) {
    for (const auto& part:kParts) if (row==part.row) return &part;
    return nullptr;
}
} // namespace holevr::vr::bench_layout
