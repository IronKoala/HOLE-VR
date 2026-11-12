#pragma once
#include "ue/objects.h"
#include "vr/math.h"

namespace holevr::vr::audio_listener {
// Where the game hears from. The game hears from its own camera, and the
// headset's views are drawn from that camera with your head's offset added
// (vr/stereo), so the game's ears never turned with your head, only with the
// stick. With vr_audio_listener=head (the default) the game hears from your
// head, turned as your head is, every frame; game leaves it to the game.
//
// Game thread, once per player tick: player is the character, head its
// headset pose in the world (have_head false when there is none yet).
void Update(ue::UObject player, bool have_head, const FVector& head_location, const FQuat& head_rotation,
            double now_s);
}  // namespace holevr::vr::audio_listener
