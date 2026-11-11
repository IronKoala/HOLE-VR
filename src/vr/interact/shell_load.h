#pragma once
// Shotgun shells by hand (owner 2026-11-07, "smart reload"; spec in
// research/shotgun-benelli-m4-beretta-1301.md, 4.6 to 4.8):
//
// - The caddy (the magazine pouch, while the shotgun is the gun in hand)
//   gives as many shells as the tube needs: 4 when it is missing 4 or more,
//   2 when it is missing 2 or 3, 1 when it is missing 1 (the chamber does not
//   count). With the bolt locked open on an empty gun the tube is locked, so
//   it gives 1 shell for the ejection port.
// - Four are held like a competition "quad" (two pairs, the pair that goes in
//   first under the thumb), two like a "twin", one on its own.
// - They go into the tube two per push: the leading shell's nose comes to
//   the loading port's mouth, the hand pushes along the tube, the first shell
//   clicks in at first_cm and the second at second_cm in the same sweep.
//
// Pure logic, unit tested in tests/test_shell_load.cpp. vr/controls spawns
// the shells, reads the hands and makes the game's own shell calls.

#include "vr/math.h"

namespace holevr::vr::interact::shells {

constexpr double kLength = 7.0;  // a 70 mm 12 gauge shell, cm
constexpr double kSpacing = 2.2; // shell to shell in the hand (rim width), cm
constexpr int kMaxHeld = 4;

struct Settings {
    bool smart = true;              // shotgun_smart_reload: 4 / 2 / 1 by what the tube is missing
    int fixed = 4;                  // smart reload off: always this many
    bool empty_gun_single = true;   // shotgun_empty_gun_single: 1 shell for the port on an empty, open gun
    bool tube_needs_closed_bolt = true;  // shotgun_tube_needs_closed_bolt
    double capture_cm = 4.0;        // leading shell's nose this close to the port's mouth
    double capture_deg = 35.0;      // and pointing this close to along the tube
    double let_go_side_cm = 5.0;    // captured: the hand this far off the tube's line lets go
    double pull_out_cm = 3.0;       // captured: pulled back this far behind where it caught lets go
    double first_cm = 5.5;          // the first shell of a push clicks in here
    double second_cm = 12.5;        // the second, in the same sweep
    double block_cm = 1.0;          // a shell the tube refuses stops this far in
    double port_cm = 4.0;           // a shell this close to the ejection port drops in
};

// The gun as the caddy and the tube see it (the game's own counts).
struct Tube {
    int capacity = 0;     // shells the tube holds
    int in_tube = 0;      // shells in it now
    int chamber = 0;      // 0 or 1
    bool bolt_open = false;  // the bolt is held open by the bolt catch
};

// The tube refuses shells: the bolt is locked open (shotgun_tube_needs_closed_bolt).
bool TubeLocked(const Settings& s, const Tube& t);
// The tube has room for another shell.
bool TubeRoom(const Tube& t);
// A shell can go in through the ejection port: bolt open, chamber empty,
// nothing waiting on the lifter (the tube is empty).
bool PortOpen(const Tube& t);
// How many shells a grab at the caddy gives (0: none, the tube is full or
// locked: a short buzz).
int SmartCount(const Settings& s, const Tube& t);
// "quad", "twin", "single" (or "three" for a quad that lost one), for the log.
const char* HoldName(int n);

// Where the shell in slot `slot` of an n-shell hold sits in the hand: its
// base (rim) and the way its nose points, in the controller's grip frame (X
// forward through the fist, Y right, Z up). Brass towards the thumb and
// index, crimps out past the little finger; a quad is two pairs, slot 0 and
// 1 (the pair that goes in first) on top under the thumb. `nudge` moves the
// whole hold (shotgun_hold_*_cm). Slots past n-1 are clamped.
FVector HoldBase(int n, int slot, const FVector& nudge);
FVector HoldNoseDir();

// The push into the loading port, one sweep at a time.
struct Sweep {
    bool captured = false;
    double offset = 0;    // along-axis distance of the nose from the mouth when it caught
    double depth = 0;     // the hand's push since it caught, cm (capped while blocked)
    int pair = 0;         // shells this sweep loads (1 or 2)
    int loaded = 0;       // shells of this sweep already in
    bool block_told = false;
};
enum class Step { kNone, kCaptured, kCommit, kBlocked, kLetGo };
struct StepIn {
    FVector nose;       // slot 0's nose as the hand holds it, minus the port's mouth (world, cm)
    FVector nose_dir;   // the way it points (unit)
    FVector axis;       // into the tube (unit, world)
    int held = 0;       // shells still in the hand
    bool room = true;   // TubeRoom
    bool locked = false;  // TubeLocked
};
// One frame of the push. kCommit: one shell goes into the tube now (at most
// one a frame); when it was the sweep's last, `captured` is false again.
// kBlocked: the tube refused it (once a sweep). kLetGo: the hand left the
// line or pulled back out; the shells stay in the hand.
Step Advance(Sweep& w, const Settings& s, const StepIn& in);
// How far the leading shell's nose is drawn past the mouth, cm (captured).
double ShownDepth(const Sweep& w, const Settings& s);

// A shell (its middle) close enough to the ejection port to drop in.
bool AtEjectionPort(const FVector& shell_middle, const FVector& port, const Settings& s);

}  // namespace holevr::vr::interact::shells
