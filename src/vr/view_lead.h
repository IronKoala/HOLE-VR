#pragma once
// Keeps the hands and held things on the view the frame will be drawn from
// while the character moves: samples where the character's camera anchors
// are before and after the mod's tick listeners and hands them to
// stereo::CharacterAtTickStart / CharacterAtTickEnd (see view_lead_math.h).
namespace holevr::vr::view_lead {

// After game::Init. Game thread work only, from the player's tick.
void Init();

}  // namespace holevr::vr::view_lead
