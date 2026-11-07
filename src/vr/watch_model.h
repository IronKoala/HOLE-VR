#pragma once
#include <cstdint>
#include <string>
#include "vr/watch_math.h"

namespace holevr::vr::watch::model {
// Game thread only. Five native components: case/band, three hands and the
// inset's glyphs. Original watch geometry and artwork, no game mesh copied.
void Show(uintptr_t player, const skel::Xf& mount, const HandAngles& hands,
          const std::string& elapsed, double now);
void Clock(const HandAngles& hands, const std::string& elapsed, double now);
void Hide();
bool Visible();
} // namespace holevr::vr::watch::model
