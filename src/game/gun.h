#pragma once
#include <string>

#include "ue/call.h"
#include "ue/objects.h"

// Wrapper over HOLE's BP_GunBase_C (handgun BP_HG_C, rifle BP_AR_C). The game
// already simulates the slide/bolt, slide catch, magazine and chamber as
// separate parts, so VR handling drives those parts instead of playing the
// flat-screen reload animations. Game thread only.
namespace holevr::game {
using ue::UObject;
using ue::GetName;
using ue::GetClass;

struct GunState {
    int chamber = 0;         // rounds in the chamber (0 or 1)
    int magazine = -1;       // rounds in the inserted magazine, -1 if none
    int capacity = 0;
    bool has_magazine = false;
    bool jammed = false;
    bool slide_locked = false;   // slide held open by the slide catch
    bool reloading = false;      // the game's own reload animation is running
    bool bolt_charging = false;  // the game's own charge animation is running
    bool firing = false;
    ue::Vec3d slide_location;    // slide part, relative to its parent

    bool operator==(const GunState& o) const;
    std::string Describe() const;
};

class Gun {
public:
    // The weapon the player is holding, if it is a gun.
    static Gun Current(UObject player);
    explicit Gun(UObject self = 0) : self_(self) {}

    bool Valid() const { return self_ != 0; }
    UObject Object() const { return self_; }
    std::string ClassName() const;

    GunState State() const;

    // Slide/bolt geometry from the gun's own setup values.
    UObject SlideComponent() const;
    ue::Vec3d SlideOrigin() const;
    double SlideBackLength() const;   // full travel, in cm
    double SlideStopLength() const;   // travel where the slide catch holds it
    // Moves the slide part directly: 0 = closed, 1 = fully back. Purely
    // visual; the chamber only changes through Cycle()/Release() below.
    void SetSlideTravel(double travel, const ue::Vec3d& axis);
    void RestoreSlide();

    // Actions, each mapped to the game's own function for that step.
    bool DropMagazine();          // mag release: magazine falls out
    bool DetachMagazine();        // pull the magazine out by hand
    bool TakeNewMagazine();       // new magazine appears in the reload hand
    bool InsertMagazine();        // seat the magazine in the magwell
    bool ChargeBolt();            // full rack: eject the chambered round, feed a new one
    // The two halves of a hand-driven full rack: pull (slide reaches the back,
    // the chambered round ejects) and release (slide returns, feeds a round).
    bool ChargeBoltPull();
    bool ChargeBoltRelease();
    // Clear the game's "charging" flag so the off-hand IK lets go of the slide.
    bool EndBoltCharging();
    // Clear the game's "slide held open by the catch" flag.
    bool ClearSlideLock();
    bool ReleaseSlideCatch();     // drop a locked-back slide, chambering a round

    // Calls a parameterless function on the gun by name (lab use).
    bool CallSimple(const char* function_name);

    // ---- shotgun (tube-fed) shells, through the game's own shell reload ----
    // BP_GunBase_C (script dump 2026-11-03): ReloadShell_TakeNewShell runs
    // Gun_ShellCreate, which makes min(tube missing, OneReloadMaxShell)
    // shells in the hand; ReloadShell_InsertShell_Begin plays the shell-in
    // sound; ReloadShell_InsertShell_End runs Gun_ShellLoad, which loads EVERY
    // shell in the hand into the tube (BPI_Magazine.LoadSingleAmmo), then
    // turns the lifter. So OneReloadMaxShell is set to 1 for the call and put
    // back: exactly one shell goes in. Nothing is loaded (false, `why` says
    // why) if the gun lacks these functions or the property, has no tube, or
    // the game made other than one shell.
    bool LoadOneShell(std::string* why);
    // The game's "tube full" sound (ReloadMag_InsertMag_Block).
    bool PlayShellBlocked();
    // The class of the shells this gun fires (AmmoClass), 0 if none.
    UObject AmmoClass() const;

private:
    UObject self_ = 0;
};

}  // namespace holevr::game
