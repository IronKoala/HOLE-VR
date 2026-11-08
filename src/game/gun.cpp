#include "game/gun.h"

#include <cstdio>

#include "core/log.h"
#include "game/game.h"
#include "ue/reflection.h"

namespace holevr::game {

bool GunState::operator==(const GunState& o) const {
    return chamber == o.chamber && magazine == o.magazine && capacity == o.capacity &&
           has_magazine == o.has_magazine && jammed == o.jammed && slide_locked == o.slide_locked &&
           reloading == o.reloading && bolt_charging == o.bolt_charging && firing == o.firing;
}

std::string GunState::Describe() const {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "chamber=%d mag=%s%d/%d jammed=%d slide_locked=%d reloading=%d charging=%d "
                  "firing=%d slide=(%.2f %.2f %.2f)",
                  chamber, has_magazine ? "" : "none:", magazine, capacity, jammed, slide_locked,
                  reloading, bolt_charging, firing, slide_location.x, slide_location.y,
                  slide_location.z);
    return buf;
}

Gun Gun::Current(UObject player) {
    if (!player) return Gun();
    UObject manager = 0;
    if (!ue::GetProp(player, "WeaponManager", manager) || !manager) return Gun();
    ue::Call c(manager, "GetCurrentWeapon");
    if (!c.Invoke() || !c.Get<bool>("Success")) return Gun();
    UObject weapon = c.Get<UObject>("CurrentWeapon");
    UObject gun_class = GunBaseClass();
    if (!weapon || !gun_class || !ue::reflection::IsA(weapon, gun_class)) return Gun();
    return Gun(weapon);
}

std::string Gun::ClassName() const { return self_ ? GetName(GetClass(self_)) : std::string(); }

GunState Gun::State() const {
    GunState s;
    if (!self_) return s;
    ue::Call ammo(self_, "GetCurrentAmmoInfo");
    if (ammo.Invoke()) {
        s.chamber = ammo.Get<int32_t>("AmmoInChamber");
        s.magazine = ammo.Get<int32_t>("AmmoInMagazine");
        s.jammed = ammo.Get<bool>("IsJamming");
        s.capacity = ammo.Get<int32_t>("MagazineCapacity");
    }
    UObject mag = 0;
    ue::GetProp(self_, "MagazineObj", mag);
    s.has_magazine = mag != 0;
    if (!s.has_magazine) s.magazine = -1;

    ue::Call motion(self_, "GetGunMotionInfo");
    if (motion.Invoke()) {
        s.slide_locked = motion.Get<bool>("IsBolt(Slide)Stopped");
        s.reloading = motion.Get<bool>("IsReloading");
        s.bolt_charging = motion.Get<bool>("IsBoltCharging");
        s.firing = motion.Get<bool>("IsFiring");
    }
    if (UObject slide = SlideComponent()) ue::GetProp(slide, "RelativeLocation", s.slide_location);
    return s;
}

UObject Gun::SlideComponent() const {
    UObject c = 0;
    if (self_) ue::GetProp(self_, "GunParts_Bolt(Slide)", c);
    return c;
}

ue::Vec3d Gun::SlideOrigin() const {
    ue::Vec3d v;
    if (self_) ue::GetProp(self_, "Bolt(Slide)_OriginLoc", v);
    return v;
}

double Gun::SlideBackLength() const {
    double d = 0;
    if (self_) ue::GetProp(self_, "Bolt(Slide)_BackLength", d);
    return d;
}

double Gun::SlideStopLength() const {
    double d = 0;
    if (self_) ue::GetProp(self_, "Bolt(Slide)_StopLength", d);
    return d;
}

void Gun::SetSlideTravel(double travel, const ue::Vec3d& axis) {
    UObject slide = SlideComponent();
    if (!slide) return;
    if (travel < 0) travel = 0;
    if (travel > 1) travel = 1;
    ue::Vec3d o = SlideOrigin();
    double len = SlideBackLength() * travel;
    ue::Vec3d p{o.x + axis.x * len, o.y + axis.y * len, o.z + axis.z * len};
    ue::Call c(slide, "K2_SetRelativeLocation");
    c.Set("NewLocation", p).Set("bSweep", false).Set("bTeleport", true);
    c.Invoke();
}

void Gun::RestoreSlide() { SetSlideTravel(0.0, ue::Vec3d{}); }

bool Gun::CallSimple(const char* function_name) {
    if (!self_) return false;
    ue::Call c(self_, function_name);
    return c.Invoke();
}

bool Gun::DropMagazine() {
    ue::Call c(self_, "Gun_MagazineDrop");
    return c.Invoke() && c.Get<bool>("Success");
}

bool Gun::DetachMagazine() {
    ue::Call c(self_, "Gun_MagazineDetach");
    return c.Invoke() && c.Get<bool>("Success");
}

bool Gun::TakeNewMagazine() { return CallSimple("ReloadMag_TakeNewMag"); }

bool Gun::InsertMagazine() {
    return CallSimple("ReloadMag_InsertMag_Begin") && CallSimple("ReloadMag_InsertMag_End");
}

bool Gun::ChargeBolt() {
    ue::Call c(self_, "Gun_ChargeBolt");
    c.Set("IsTriggerReleased", true);
    return c.Invoke();
}

bool Gun::ChargeBoltPull() {
    ue::Call c(self_, "Gun_ChargeBolt");
    c.Set("IsTriggerReleased", false);
    return c.Invoke();
}

bool Gun::ChargeBoltRelease() {
    ue::Call c(self_, "Gun_ChargeBolt");
    c.Set("IsTriggerReleased", true);
    return c.Invoke();
}

bool Gun::EndBoltCharging() {
    ue::Call c(self_, "SetIsBoltCharging");
    c.Set("IsBoltCharging", false);
    return c.Invoke();
}

bool Gun::ClearSlideLock() { return ue::SetProp<bool>(self_, "IsBolt(Slide)Stopped", false); }

bool Gun::ReleaseSlideCatch() { return CallSimple("Gun_ReleaseBolt"); }

bool Gun::LoadOneShell(std::string* why) {
    auto fail = [&](const char* w) {
        if (why) *why = w;
        return false;
    };
    if (!self_) return fail("no gun");
    if (!ue::FindFunction(GetClass(self_), "ReloadShell_TakeNewShell") ||
        !ue::FindFunction(GetClass(self_), "ReloadShell_InsertShell_End") ||
        !ue::FindFunction(GetClass(self_), "GetShellNumInHand"))
        return fail("the gun has no shell reload functions");
    // OneReloadMaxShell must be what the dump says: a plain int.
    const auto p = ue::reflection::FindProperty(GetClass(self_), "OneReloadMaxShell");
    if (!p.valid() || p.type != "IntProperty" || p.element_size != 4 || p.array_dim != 1 || p.offset <= 0)
        return fail("OneReloadMaxShell is missing or not an int");
    int32_t old_max = 0;
    if (!mem::Read(self_ + p.offset, old_max)) return fail("OneReloadMaxShell unreadable");
    UObject tube = 0;
    if (!ue::GetProp(self_, "MagazineObj", tube) || !tube) return fail("no tube magazine");
    auto shells_in_hand = [&]() {
        ue::Call n(self_, "GetShellNumInHand");
        return n.Invoke() ? n.Get<int32_t>("ShellNum", -1) : -1;
    };
    if (shells_in_hand() != 0) CallSimple("DestroyShellsInHand");  // left over from an earlier call
    if (!ue::SetProp<int32_t>(self_, "OneReloadMaxShell", 1)) return fail("OneReloadMaxShell not writable");
    const bool took = CallSimple("ReloadShell_TakeNewShell");
    ue::SetProp<int32_t>(self_, "OneReloadMaxShell", old_max);
    const int made = shells_in_hand();
    if (!took) return fail("ReloadShell_TakeNewShell failed");
    if (made != 1) {
        CallSimple("DestroyShellsInHand");
        return fail(made == 0 ? "the game made no shell (tube full?)" : "the game did not make exactly one shell");
    }
    CallSimple("ReloadShell_InsertShell_Begin");
    if (!CallSimple("ReloadShell_InsertShell_End")) {
        CallSimple("DestroyShellsInHand");
        return fail("ReloadShell_InsertShell_End failed");
    }
    return true;
}

bool Gun::PlayShellBlocked() { return CallSimple("ReloadMag_InsertMag_Block"); }

UObject Gun::AmmoClass() const {
    UObject c = 0;
    if (self_) ue::GetProp(self_, "AmmoClass", c);
    return c;
}

}  // namespace holevr::game
