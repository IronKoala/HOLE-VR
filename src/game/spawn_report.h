#pragma once

// Diagnostic, read-only: what the game's own level manager (SYS_LevelManager)
// is doing with enemy spawns and its "object" events (OBJ001 = the flying
// retro TVs, OBJ002 = lights-off, OBJ003), written to the log. Nothing here
// changes the game: it only reads the manager's fields, the TV manager it
// spawns and the save's event progress.
//
// Log lines start with "spawns:". At each level: the level's spawn settings,
// which object event the level has (ObjectManagerClass) and whether it is
// open (IsOpenObject). Every 10 s in a run: enemies up now against the
// game's limit, waves, the game's run clock, and the object event's state
// (manager spawned or not, TVs alive and switched on). Changes to the object
// event are logged when they happen.
namespace holevr::game::spawn_report {

// Registers the game-thread tick listener (spawn_report=0 turns it off).
void InitGame();

}  // namespace holevr::game::spawn_report
