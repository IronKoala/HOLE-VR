#pragma once
#include <functional>

#include "ue/objects.h"

// HOLE-specific glue: finds the player, gives the rest of the mod a callback
// on the game thread every frame, and a queue to run work there.
namespace holevr::game {

using ue::UObject;
using ue::GetClass;
using ue::GetName;
using ue::GetFullName;
using ue::FindObject;

// Hooks ProcessEvent on the native classes HOLE's player, weapons and
// components derive from. Needs ue::process_event::Init() first.
bool Init();

// Call from a background thread every second or so. Re-finds the player's
// tick function after level loads (Blueprint classes can be reloaded).
void Maintain();

// The live BP_Player_C, or 0 when there is none (menus, loading).
UObject Player();

// Runs after the player's Blueprint tick, on the game thread, every frame.
using TickFn = void (*)(UObject player, float delta_seconds);
void AddTickListener(TickFn fn);
// Runs `begin` right before the first tick listener and `end` right after
// the last, every player tick, on the game thread. One pair; either may be
// null.
void SetTickBracket(TickFn begin, TickFn end);

// Queue work for the next game-thread tick. Safe from any thread.
void Post(std::function<void()> fn);

// Classes we use everywhere (0 until the game has loaded them).
UObject GunBaseClass();
UObject WeaponBaseClass();
UObject MagazineBaseClass();

}  // namespace holevr::game
