#pragma once
#include <cstdint>

#include "ue/objects.h"

// UObject::ProcessEvent: the engine's single entry point for calling any
// UFunction (native or Blueprint) on an object. We use it two ways:
//   - calling game functions (see ue/call.h), and
//   - observing/intercepting calls by patching the virtual-table slot of
//     selected native classes, which also gives us a game-thread tick.
namespace holevr::ue::process_event {

// Finds UFunction::FunctionFlags and the ProcessEvent vtable index. Needs
// reflection::Init() first. Safe to call repeatedly until it returns true.
bool Init();
bool Ready();

int VtableIndex();
uintptr_t FunctionFlagsOffset();

// Calls obj->ProcessEvent(function, params). Must run on the game thread.
void Invoke(UObject obj, UObject function, void* params);

// Hook callbacks run on the game thread. A pre-callback returning false
// stops the original call (the game never sees it).
using PreFn = bool (*)(UObject self, UObject function, void* params);
using PostFn = void (*)(UObject self, UObject function, void* params);

// Patches the ProcessEvent slot of the vtable used by `sample` (any object or
// class default object of the native class). Every object sharing that vtable
// goes through our callbacks afterwards. Idempotent.
bool HookVtableOf(UObject sample);

// Register callbacks for one UFunction (by object address). Up to 64 entries.
bool On(UObject function, PreFn pre, PostFn post);
// Called for every hooked ProcessEvent before the per-function callbacks.
// Keep it cheap. Only one; later calls replace it.
void OnAny(PostFn post);

}  // namespace holevr::ue::process_event
