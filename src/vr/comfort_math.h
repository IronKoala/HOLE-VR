#pragma once
// Pure logic behind the comfort and body features (no game, no Windows), so
// it can be unit tested natively (tests/test_comfort.cpp):
//   - vignette strength smoothing and the picture it is drawn from,
//   - physical crouch and jump from the real head height,
//   - aim smoothing for the gun hand,
//   - grip toggle (squeeze once to hold, once more to let go).
#include <cstdint>
#include <vector>

#include "vr/math.h"

namespace holevr::vr::comfort {

// ---- vignette picture ----
// One static texture: kVignetteLevels square tiles (4 x 4) of a black ring
// whose clear middle gets smaller from level 1 to the last, then a strip of
// kFadeLevels solid black blocks of rising opacity for fades.
constexpr int kTile = 256;
constexpr int kTilesPerRow = 4;
constexpr int kVignetteLevels = 16;  // level 0 = nothing drawn
constexpr int kFadeLevels = 16;      // block k = (k + 1) / 16 opaque
constexpr int kFadeBlock = 64;
constexpr int kTexW = kTile * kTilesPerRow;                                   // 1024
constexpr int kTexH = kTile * (kVignetteLevels / kTilesPerRow) + kFadeBlock;  // 1088
// The quad: kQuadHalfM either side of the view centre, kQuadDistM ahead.
constexpr double kQuadHalfM = 3.0;
constexpr double kQuadDistM = 1.0;

// Clear half-angle of the view (degrees from straight ahead) at strength
// 0..1; the ring goes from clear to black over the next kRampDeg.
double ClearHalfAngleDeg(double strength);
constexpr double kRampDeg = 14.0;
// Premultiplied BGRA pixels (black, alpha only) of the whole texture.
std::vector<uint32_t> TexturePixels();
struct Rect {
    int x, y, w, h;
};
// 0 = draw nothing; otherwise the tile for this strength (0..1).
int VignetteLevel(double strength);
Rect VignetteTile(int level);
// 0 = nothing; otherwise the block for this opacity (0..1), inset so the
// texture filter never reaches the next block.
int FadeLevel(double opacity);
Rect FadeBlock(int level);

// Follows a target quickly up and more slowly down (seconds to ~63%).
struct Smoother {
    double value = 0.0;
    double Step(double target, double dt, double rise_s, double fall_s);
};

// ---- physical crouch and jump ----
enum class BodyEvent { kNone, kCrouch, kStand, kJump };
// Feed the real head height (metres above the floor, or any fixed origin)
// every frame. Standing height is the highest head height seen (spikes
// above 2.3 m ignored), unless SetStanding gives a measured one.
class PhysicalBody {
public:
    void Configure(bool crouch, double crouch_drop_cm, bool jump);
    void SetStanding(double head_m);
    // Returns kCrouch when the head drops crouch_drop_cm below standing,
    // kStand when it comes back within (crouch_drop_cm - 10), kJump for a
    // fast rise above standing height. Edges only: one event per change.
    BodyEvent Update(double head_m, double dt);
    bool Crouched() const { return crouched_; }
    double Standing() const { return standing_; }
    double Drop() const { return have_ ? standing_ - last_ : 0.0; }
    void Reset();

private:
    bool crouch_on_ = true, jump_on_ = false;
    double drop_m_ = 0.35;
    bool have_ = false;
    bool measured_ = false;
    double standing_ = 0.0;
    double last_ = 0.0;
    double vel_ = 0.0;  // m/s, smoothed
    bool crouched_ = false;
    double jump_cooldown_ = 0.0;
    double warmup_ = 0.0;
};

// ---- aim smoothing ----
// Filters the gun hand's rotation: slow, small motion (tremor) is smoothed,
// fast motion passes straight through so swinging to a new target never
// lags. strength 0 = off .. 1 = strongest.
class AimSmoother {
public:
    FQuat Step(const FQuat& target, double dt, double strength);
    void Reset() { have_ = false; }

private:
    bool have_ = false;
    FQuat q_{};
};
double AngleDeg(const FQuat& a, const FQuat& b);

// ---- grip toggle ----
// What the item-holding code sees of the grip button. In hold mode it is the
// button itself. In toggle mode, once an item is held the grip reads as held
// until the next squeeze, which reads as the let-go.
struct GripView {
    bool down = false, pressed = false, released = false;
};
// holding: an item is in the hand after last frame's handling.
GripView ToggleGrip(bool toggle, bool holding, bool down, bool pressed, bool released);

// ---- the game camera's height ----
// Keeps the view at the character's standing eye height when the game's own
// camera drops without a crouch and stays down (seen 2026-11-03: after two
// quick melee punches the camera sat 25 cm low for the rest of the match
// while the character ran normally). Short dips (vaults, hits) still play.
// In real-height mode every drop below standing is given back, crouches
// included, since your own head already does the crouching.
class CameraHeight {
public:
    // rel_cm: the game camera's height above the character's feet this
    // frame. crouched: the character's crouch flag. crouch_press: the stick
    // crouch was pressed this frame (a crouch the flag may not show). still:
    // the character is standing still, not sprinting (sprinting and moving
    // lower the game camera 10-26 cm on purpose, so only a drop that stays
    // while standing still starts the hold). Returns how far to raise the
    // view, cm.
    double Update(double rel_cm, bool crouched, bool crouch_press, bool still, bool real_mode, double dt);
    bool HaveStanding() const { return have_; }
    double Standing() const { return stand_; }
    double Drop() const { return drop_; }
    bool Holding() const { return holding_; }
    int CrouchPresses() const { return presses_; }
    void Reset() { *this = CameraHeight{}; }

private:
    bool have_ = false;
    double stand_ = 0;
    double stable_ref_ = 0, stable_t_ = 0;
    int presses_ = 0;          // stick crouches since the camera was last at standing height (0.6 s)
    double standing_t_ = 0;
    double low_t_ = 0;         // how long an unexplained drop has lasted standing still
    bool holding_ = false;
    double drop_ = 0;
    double extra_ = 0;
};

}  // namespace holevr::vr::comfort
