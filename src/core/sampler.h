#pragma once
// A sampling profiler built into the mod. A background thread stops the
// game thread, the render thread and the thread that presents frames a few
// hundred times a second, copies the stack, lets the thread go again, and
// then works out from the copy which functions were running. Every 10 s the
// log gets, per thread, how busy it was, which mod functions used the time
// (with what they call, and what they cost directly), which system calls and
// which modules the rest went to.
//
// No guessing which part of the mod is slow: the function names come from the
// DLL's own symbol table when the installed DLL has one (a build with debug
// data), otherwise as dxgi+0x... offsets that the matching build names.
//
// profile=0 in hole-vr.ini turns it off; profile_hz sets the rate per thread
// (default 500). While a thread is stopped the sampler takes no locks and
// allocates nothing, so it can't deadlock with it.
namespace holevr::sampler {

// Once, after the log and settings are up.
void Start();

// Game thread, at the start of every player tick: names the game thread and
// counts its frames for the per-frame figures.
void NoteGameFrame();

// The thread that calls the game's Present, once per frame.
void NotePresentThread();

}  // namespace holevr::sampler
