#pragma once
// The hub's drink machine by hand (owner 2026-11-03: "i want to push the
// button with my finger, (so for example the health drink i just unlocked i
// want to push the yellow square underneath it) to drink it i want to
// physical grab the drink > open the tab > pour the drink down my mouth").
//
// The machine is LCBP_SkillUpgradeNew_C (Area000_Home): six buttons
// (LCBP_SkillUpgradeSwitchNew_C child actors, drawn as the yellow squares
// Home_VendingMachine_Button01..06), a can slot (LCBP_SkillUpgradeSlotNew_C)
// and the can itself (its "Can" mesh). Every purchase and every drink goes
// through the game's own interact, exactly as the interact key does:
//   - a fingertip touching a yellow square presses that button;
//   - a grip at the can in the slot takes it (a copy in the hand; the game's
//     can stays in the slot, hidden, so the game's state is untouched);
//   - a fingertip of the other hand on the tab opens it;
//   - the open can tipped at the mouth pours; after a moment the game's own
//     slot interact runs, which is how the game drinks it.
// Let go before drinking and the can goes back in the slot; let go of the
// empty can and it drops. Settings: the drinks_* keys in hole-vr.ini.

namespace holevr::vr::drinks {

// Registers the game tick listener and the machine finder. Call after
// controls::Init(), so the hands this frame are already published.
void Init();

// --- Called by vr/controls ---
// Candidate query: this hand holds a can, or is at a can waiting in the slot.
// controls leaves that hand's grip alone (no holster draw, no slide grab).
bool ClaimsGrip(int hand);
// Actual hold only, for the shared ownership resolver.
bool HoldsGrip(int hand);

}  // namespace holevr::vr::drinks
