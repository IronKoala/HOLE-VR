#pragma once
// Shared weapon-query exclusions. No hand collision or motion solver.
#include "vr/hands.h"
#include "ue/objects.h"
#include <vector>
namespace holevr::vr::collision_ignore {
void Reset();
void Refresh(ue::UObject player, const hands::HandGrab grab[2], double dt);
const std::vector<ue::UObject>& IgnoredActors();
void SetOwnGear(const std::vector<ue::UObject>& actors);
}
