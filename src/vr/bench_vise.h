#pragma once
#include <cstdint>
#include "vr/math.h"

namespace holevr::vr::bench_vise {
// Game-thread only. The bench owns the component; it has no collision and
// never owns a hand, moves a weapon, or changes the saved attachment list.
void Show(uintptr_t bench, const FVector& position, const FQuat& rotation,
          bool occupied, double loaded_gap_cm, double now);
void Hide();
void Reset();
}  // namespace holevr::vr::bench_vise
