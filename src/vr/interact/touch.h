#pragma once
// Touch interact: rest an empty hand on loot to take it, on a loot box or
// fridge to open it, on a door's handle to open or close the door. Each touch
// runs the game's own interact (the same steps as looking at the thing and
// pressing the interact key), so the game decides what happens. The trigger
// interact (vr/controls HandInteract) asks this module what the hand is on
// or points at, so the hand wins over where the head looks.
//
// Shapes: loot and containers use their visible meshes, doors a zone around
// the handle worked out from the door leaf and its hinge. Log lines start
// with "touch:"; design and settings: docs/TOUCH_INTERACT.md.

#include <string>
#include <vector>

#include "ue/objects.h"

namespace holevr::vr::touch {

// Registers the game tick listener. Call after body::Init(), so this frame's
// drawn hands (body::GetHandContacts) are ready.
void Init();

// What the trigger interact should use for hand `hand` (0 left, 1 right):
// the interactable the hand touches (smallest first), else the one its aim
// ray hits with the game's own interact trace. `how` is "touching" or
// "pointing at". 0 when neither: the game's own (head) target is left.
// `ignore`: actors the aim ray passes through (the gun in the other hand).
struct Pick {
    ue::UObject actor = 0;
    const char* how = "";
    std::string cls;   // class name, for the log
    std::string kind;  // KindName
};
Pick PickForHand(ue::UObject player, int hand, const std::vector<ue::UObject>& ignore);

// The game's own interact on `target`, as if the player looked at it and
// pressed the interact key: focus it (its InteractInOverlay), make it the
// player's InteractTarget, then the player's PlayerInput_Interact. Remembers
// the time, so a touch and a trigger press never do the same thing twice
// within a second. `why` goes in the log ("left hand touching" ...).
bool InteractThroughGame(ue::UObject player, ue::UObject target, const char* why);

// True when `actor` was interacted with (touch or trigger) less than
// `within_s` seconds ago.
bool RecentlyUsed(ue::UObject actor, double within_s);

}  // namespace holevr::vr::touch
