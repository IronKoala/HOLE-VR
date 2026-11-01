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
    bool ReleaseSlideCatch();     // drop a locked-back slide, chambering a round

    // Calls a parameterless function on the gun by name (lab use).
    bool CallSimple(const char* function_name);

private:
    UObject self_ = 0;
};

}  // namespace holevr::game
