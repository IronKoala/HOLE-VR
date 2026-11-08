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

// The game thread's id, so the report can count events that reach our hook
// from other threads (animation workers): callbacks take locks there.
void SetGameThread(unsigned long id);
// Events seen off the game thread since the last call.
long long TakeOffThreadEvents();

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
// Keep it cheap. Up to 4 listeners; each is added once.
void OnAny(PostFn post);

// A native UFunction's exec thunk, which Blueprint code calls directly (not
// through ProcessEvent): Func(Context, FFrame& Stack, RESULT_DECL).
using NativeFn = void (*)(UObject context, void* stack, void* result);
// Replaces `function`'s exec thunk with `hook`; `*original` gets the old one,
// which the hook must call. Finds UFunction::Func by layout on first use.
bool HookNative(UObject function, NativeFn hook, NativeFn* original);

// Calls made from inside one of our callbacks skip all callbacks, so a
// callback never re-enters itself. That also hid game code a callback sets
// off: a laser click on "Options" runs the button's OnClicked and builds the
// Options menu right inside our per-frame callback, so the menu's Construct
// never reached us and Options stayed on the flat screen (owner's run
// 2026-11-02 12:32 UTC). While one of these is alive on the game thread, the
// game's own calls are seen as if the game had made them at the top level;
// calls our callbacks make are still skipped.
class GameCodeScope {
public:
    GameCodeScope();
    ~GameCodeScope();
    GameCodeScope(const GameCodeScope&) = delete;
    GameCodeScope& operator=(const GameCodeScope&) = delete;

private:
    int saved_;
};

}  // namespace holevr::ue::process_event
