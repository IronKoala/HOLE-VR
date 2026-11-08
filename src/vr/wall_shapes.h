#pragma once
// What the hands and the held gun collide with, and where the game's hit
// boxes differ from the shapes you see.
//
// Owner 2026-11-07: "The hit boxes in the hub need to be refined so the
// current physics hands and guns collide correctly and not sink into the
// wall." The mod cannot change the game's hit boxes, but its own checks can
// use each mesh's own triangles (Unreal's "complex" collision: the shape you
// see) instead of the simplified hit boxes the game gave it ("simple"
// collision: boxes and hulls, sometimes smaller than the wall or missing).
//
// Settings (hole-vr.ini):
//   wall_shape=nearest  hands, fingers and the held gun stop at whichever
//                       comes first, the shape you see or the game's hit box
//                       (default): they go into neither. Where a mesh has
//                       no triangles for collision, its hit box still stops
//                       them, as before.
//   wall_shape=visible  only the shapes you see
//   wall_shape=game     only the game's hit boxes (the mod before 2026-11-07)
//   wall_shape_report=1 the "wall shapes:" log line, every 10 s while hands
//                       or the gun touch things: per mesh, how far the
//                       game's hit box is from the surface you see.
#include <vector>

#include "ue/objects.h"
#include "vr/math.h"

namespace holevr::vr::wall_shapes {

using ue::UObject;

void LoadSettings();

enum class Mode : int { kGame = 0, kVisible = 1, kNearest = 2 };
// Read from the settings (wall_shape; the older hand_world_trace_complex=1
// with wall_shape=game means visible).
Mode ModeSetting();
const char* ModeName(Mode mode);
// A query in `mode` is made `Passes` times, pass `p` with bTraceComplex =
// Complex(mode, p); the nearest hit wins (an overlap at the start first).
inline int Passes(Mode mode) { return mode == Mode::kNearest ? 2 : 1; }
inline bool Complex(Mode mode, int pass) { return mode == Mode::kVisible || (mode == Mode::kNearest && pass == 0); }

// Who touched something, for the log.
enum Who : int { kHand = 0, kGun = 1 };

// Notes where the game's hit box and the visible shape are on the line from
// `from` to `to` (a line that crosses the surface just touched), for the
// "wall shapes" log line. Two line traces, at most four times a second for
// each `who`. Game thread.
void Compare(UObject player, const FVector& from, const FVector& to, const std::vector<UObject>& ignore, Who who);

}  // namespace holevr::vr::wall_shapes
