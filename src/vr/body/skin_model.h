#pragma once
#include "vr/body/skin_mesh.h"
namespace holevr::vr::skin_model {
void Reset();
bool Prepare(uintptr_t player,uintptr_t source,uintptr_t pose_component,
             const std::vector<std::string>& names,const std::vector<int>& parents,
             const std::vector<skel::Xf>& reference_local);
bool Ready();
bool Update(const skel::Pose& pose,const skel::Xf& world,bool shadow);
void Hide();
} // namespace holevr::vr::skin_model
