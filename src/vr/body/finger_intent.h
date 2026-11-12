#pragma once
// Free finger intention. Neutral input keeps Hands1's small added curl on
// the curved reference pose; anatomical opening remains available near objects.
#include "vr/body/controller_pose.h"
#include "vr/hands.h"

namespace holevr::vr::finger_intent {

// For index..pinky only. Rest is 12% extra curl on the source reference,
// then blends monotonically into the existing controller curl. Grip without
// trigger activation or touch straightens the index for pointing. This is an intention,
// never a collision correction or an authored object-grip replacement.
// relaxed_weight is the environmental rest blend: 1 away from objects,
// 0 near an observed object, with smooth intermediate values supplied by
// the caller. It removes only passive rest curl; deliberate grip/trigger
// closure is retained. Zero curl_scale preserves the disabled/straight target.
fingers::Angles FreeTarget(const fingers::Chain& chain, int finger,
                          const hands::FingerInput& input,
                          const controller_pose::Bounds& bounds,
                          double curl_scale = 1.0, double relaxed_weight = 1.0);

// Rest is 15% extra curl on the reference thumb. Active controls blend into
// the existing outside-thumb guide; a button adds only the Hands8 hinge fold.
// The same environmental weight opens the thumb as well as the other digits.
fingers::Angles FreeThumbTarget(const fingers::Chain& chain, const FVector& palm,
                               const FVector& index_base, const hands::FingerInput& input,
                               const controller_pose::Bounds& bounds, double relaxed_weight = 1.0);

} // namespace holevr::vr::finger_intent
