#pragma once

// GPU cost test (2026-11-06): once per run, 45 s into the first level
// (gpu_cost_test_after_s), switches each suspect render feature off and on
// three times for 1.5 s and logs what the GPU timer measured each way, so the
// log says in milliseconds what Lumen, the shadows, the fog and the
// resolution cost on this PC in this scene. Everything is put back after
// each one, and a menu opening (or the square test) pauses it. gpu_cost_test=0 turns it off.
namespace holevr::vr::gpu_test {
// After cvars::Init().
void Init();
// Game thread: switching render features now (the square test waits).
bool Running();
}  // namespace holevr::vr::gpu_test
