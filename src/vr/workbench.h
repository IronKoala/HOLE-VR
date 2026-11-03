#pragma once
// The hub's weapon workbench in VR (LCBP_WeaponCustom_C): lay the gun on the
// table, pull attachments off it by hand, take the attachments you own off
// the table and put them on the gun. Drives the game's own attachment system
// (vr/../game/weapon_custom.h). Design and log lines: docs/WORKBENCH.md.

namespace holevr::vr::workbench {

// Registers the game tick listener. Call after game::Init().
void Init();

}  // namespace holevr::vr::workbench
