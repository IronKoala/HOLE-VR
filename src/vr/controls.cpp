#include "vr/controls.h"

#include <windows.h>

#include <cmath>
#include <mutex>

#include "core/config.h"
#include "core/log.h"
#include "game/game.h"
#include "game/gun.h"
#include "ue/call.h"
#include "vr/hands.h"
#include "vr/input.h"
#include "vr/interact/slide.h"
#include "vr/openxr.h"
#include "vr/stereo.h"

namespace holevr::vr::controls {
namespace {

using ue::UObject;

// FTransform (double precision) as returned by K2_GetComponentToWorld.
struct alignas(16) UETransform {
    FQuat rotation;
    FVector translation;
    double pad0 = 0;
    FVector scale{1, 1, 1};
    double pad1 = 0;
};
static_assert(sizeof(UETransform) == 0x60, "FTransform layout");

UETransform ComponentToWorld(UObject comp) {
    ue::Call c(comp, "K2_GetComponentToWorld");
    if (!c.Invoke()) return UETransform{};
    return c.Result<UETransform>();
}

FVector ToLocal(const UETransform& t, const FVector& world) {
    return t.rotation.Inverse().Rotate(world - t.translation);
}

FVector ActorLocation(UObject actor) {
    ue::Call c(actor, "K2_GetActorLocation");
    c.Invoke();
    return c.Result<FVector>();
}

FQuat ActorRotation(UObject actor) {
    ue::Call c(actor, "K2_GetActorRotation");
    c.Invoke();
    return ToQuat(c.Result<FRotator>());
}

void SetActorTransform(UObject actor, const FVector& loc, const FQuat& rot) {
    ue::Call c(actor, "K2_SetActorLocationAndRotation");
    c.Set("NewLocation", loc).Set("NewRotation", ToRotator(rot)).Set("bSweep", false).Set("bTeleport", true);
    c.Invoke();
}

void DetachIfAttached(UObject actor) {
    ue::Call parent(actor, "GetAttachParentActor");
    if (!parent.Invoke() || !parent.Result<UObject>()) return;
    ue::Call d(actor, "K2_DetachFromActor");
    const uint8_t keep_world = 1;  // EDetachmentRule::KeepWorld
    d.Set("LocationRule", keep_world).Set("RotationRule", keep_world).Set("ScaleRule", keep_world);
    d.Invoke();
}

// ---- settings ----
struct Settings {
    bool enabled = true;
    bool smooth_turn = false;
    double snap_degrees = 30.0;
    double smooth_speed = 180.0;   // degrees per second at full stick
    bool move_with_hand = false;   // false: head direction
    double deadzone = 0.15;
    double gun_pitch = 0.0;        // extra pitch on the held gun, degrees
    double gun_yaw = 0.0;
    double gun_roll = 0.0;
    bool log_grabs = true;
};
Settings g_set;

void LoadSettings() {
    g_set.enabled = config::GetBool("vr_controls", true);
    g_set.smooth_turn = config::GetInt("turn_mode", 0) == 1;
    g_set.snap_degrees = config::GetFloat("snap_turn_degrees", 30.f);
    g_set.smooth_speed = config::GetFloat("smooth_turn_speed", 180.f);
    g_set.move_with_hand = config::GetInt("move_direction", 0) == 1;
    g_set.deadzone = config::GetFloat("stick_deadzone", 0.15f);
    g_set.gun_pitch = config::GetFloat("gun_pitch_offset", 0.f);
    g_set.gun_yaw = config::GetFloat("gun_yaw_offset", 0.f);
    g_set.gun_roll = config::GetFloat("gun_roll_offset", 0.f);
}

// ---- shared hand state for other systems ----
std::mutex g_hands_mutex;
hands::HandState g_hands[2];
hands::HandGrab g_grab[2];
hands::FingerInput g_fingers[2];
hands::Pose g_head;
bool g_head_valid = false;
double g_clock = 0.0;

bool ToWorld(const XrPosef& p, hands::Pose* out) {
    return stereo::TrackingToWorld(p, &out->location, &out->rotation);
}

void UpdateHands(const input::Snapshot& s) {
    hands::HandState h[2];
    for (int i = 0; i < 2; ++i) {
        const input::HandInput& in = s.hand[i];
        if (!in.grip_valid || !ToWorld(in.grip, &h[i].grip)) continue;
        if (in.aim_valid) ToWorld(in.aim, &h[i].aim);
        else h[i].aim = h[i].grip;
        // Grip pose: +X (XR) is out of the palm for the left hand, into it
        // for the right; after conversion that is UE +Y.
        FVector palm_axis = h[i].grip.rotation.Rotate(FVector{0, 1, 0});
        h[i].palm_normal = i == 0 ? palm_axis : palm_axis * -1.0;
        // Wrist: about 7 cm back from the fist centre along the forearm.
        h[i].wrist.rotation = h[i].aim.rotation;
        h[i].wrist.location = h[i].grip.location + h[i].aim.rotation.Rotate(FVector{-7, 0, 0});
        h[i].valid = true;
        h[i].time = g_clock;
    }
    hands::Pose head;
    bool head_ok = false;
    xr::EyeView v[2];
    if (xr::LatestViews(v)) {
        XrPosef c = v[0].pose;
        c.position.x = (v[0].pose.position.x + v[1].pose.position.x) * 0.5f;
        c.position.y = (v[0].pose.position.y + v[1].pose.position.y) * 0.5f;
        c.position.z = (v[0].pose.position.z + v[1].pose.position.z) * 0.5f;
        head_ok = ToWorld(c, &head);
    }
    std::lock_guard<std::mutex> lock(g_hands_mutex);
    for (int i = 0; i < 2; ++i) {
        const input::HandInput& in = s.hand[i];
        g_fingers[i].trigger = in.trigger;
        g_fingers[i].trigger_touch = in.trigger_touch;
        g_fingers[i].grip = in.grip_value;
        g_fingers[i].thumb_touch = in.primary.down || in.secondary.down || in.stick_click.down;
    }
    g_hands[0] = h[0];
    g_hands[1] = h[1];
    g_head = head;
    g_head_valid = head_ok;
}

// ---- locomotion and turning ----
bool g_snap_armed = true;

void Locomotion(UObject player, const input::Snapshot& s, double dt) {
    const input::HandInput& left = s.hand[input::kLeft];
    const input::HandInput& right = s.hand[input::kRight];

    double mx = left.stick_x, my = left.stick_y;
    double mag = std::sqrt(mx * mx + my * my);
    if (mag > g_set.deadzone) {
        double scale = std::fmin(1.0, (mag - g_set.deadzone) / (1.0 - g_set.deadzone)) / mag;
        double yaw = 0;
        hands::HandState lh = hands::Get(hands::kLeft);
        hands::Pose head;
        if (g_set.move_with_hand && lh.valid) yaw = YawOf(lh.aim.rotation);
        else if (hands::Head(&head)) yaw = YawOf(head.rotation);
        double r = yaw * kDegToRad;
        FVector fwd{std::cos(r), std::sin(r), 0}, rgt{-std::sin(r), std::cos(r), 0};
        FVector dir = fwd * (my * scale) + rgt * (mx * scale);
        ue::Call c(player, "AddMovementInput");
        c.Set("WorldDirection", dir).Set("ScaleValue", 1.0f).Set("bForce", false);
        c.Invoke();
    }

    double turn = 0;
    if (g_set.smooth_turn) {
        if (std::fabs(right.stick_x) > g_set.deadzone) turn = right.stick_x * g_set.smooth_speed * dt;
    } else {
        if (g_snap_armed && std::fabs(right.stick_x) > 0.7) {
            turn = right.stick_x > 0 ? g_set.snap_degrees : -g_set.snap_degrees;
            g_snap_armed = false;
        } else if (std::fabs(right.stick_x) < 0.4) {
            g_snap_armed = true;
        }
    }
    if (turn != 0) {
        ue::Call gc(player, "GetController");
        UObject controller = gc.Invoke() ? gc.Result<UObject>() : 0;
        if (controller) {
            ue::Call get(controller, "GetControlRotation");
            get.Invoke();
            FRotator r = get.Result<FRotator>();
            r.Yaw = NormalizeAxis(r.Yaw + turn);
            ue::Call set(controller, "SetControlRotation");
            set.Set("NewRotation", r);
            set.Invoke();
        }
    }
}

// ---- weapon handling ----
struct WeaponState {
    UObject gun = 0;
    interact::Slide slide;
    bool slide_was_moved = false;
    double end_charging_at = -1;
    UObject held_mag = 0;      // magazine in the left hand, not yet inserted
    bool trigger_down = false;
    bool have_frame = false;
    FQuat gun_frame;           // gun frame (X barrel, Z up) expressed in actor space
    double check_chamber_at = -1;
    double pending_release_at = -1;
    bool retried_rack = false;
} W;

double Len(const FVector& v) { return std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z); }
FVector Norm(const FVector& v) { double l = Len(v); return l > 1e-6 ? v * (1.0 / l) : FVector{1, 0, 0}; }
double DotV(const FVector& a, const FVector& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
FVector Cross(const FVector& a, const FVector& b) {
    return {a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X};
}

// Rotation whose X/Y/Z axes are the given orthonormal vectors.
FQuat FromAxes(const FVector& x, const FVector& y, const FVector& z) {
    double m00 = x.X, m11 = y.Y, m22 = z.Z, tr = m00 + m11 + m22;
    FQuat q;
    if (tr > 0) {
        double s = std::sqrt(tr + 1.0) * 2;
        q.W = 0.25 * s;
        q.X = (y.Z - z.Y) / s;
        q.Y = (z.X - x.Z) / s;
        q.Z = (x.Y - y.X) / s;
    } else if (m00 > m11 && m00 > m22) {
        double s = std::sqrt(1.0 + m00 - m11 - m22) * 2;
        q.W = (y.Z - z.Y) / s;
        q.X = 0.25 * s;
        q.Y = (y.X + x.Y) / s;
        q.Z = (z.X + x.Z) / s;
    } else if (m11 > m22) {
        double s = std::sqrt(1.0 + m11 - m00 - m22) * 2;
        q.W = (z.X - x.Z) / s;
        q.X = (y.X + x.Y) / s;
        q.Y = 0.25 * s;
        q.Z = (z.Y + y.Z) / s;
    } else {
        double s = std::sqrt(1.0 + m22 - m00 - m11) * 2;
        q.W = (x.Y - y.X) / s;
        q.X = (z.X + x.Z) / s;
        q.Y = (z.Y + y.Z) / s;
        q.Z = 0.25 * s;
    }
    return q;
}

// Works out which way the barrel and the top of the gun point in the gun
// actor's own space, from its grip, muzzle and sight points, so any gun model
// ends up pointing where the hand points.
bool LearnGunFrame(UObject g, const FVector& actor_loc, const FQuat& actor_rot, const FVector& grip_world) {
    UObject muzzle = 0, sight = 0;
    ue::GetProp(g, "DefaultMuzzlePoint", muzzle);
    if (!muzzle) ue::GetProp(g, "MuzzleFirePoint", muzzle);
    ue::GetProp(g, "DefaultADSPoint", sight);
    if (!muzzle) return false;
    FQuat inv = actor_rot.Inverse();
    FVector grip = inv.Rotate(grip_world - actor_loc);
    FVector m = inv.Rotate(ComponentToWorld(muzzle).translation - actor_loc);
    FVector fwd = Norm(m - grip);
    // The slide runs exactly along the bore, so prefer it when there is one:
    // it moves along -X of its parent component (measured in the weapon lab).
    game::Gun gun(g);
    UObject slide_parent = 0;
    if (UObject sc = gun.SlideComponent()) ue::GetProp(sc, "AttachParent", slide_parent);
    if (slide_parent) {
        FVector bore = inv.Rotate(ComponentToWorld(slide_parent).rotation.Rotate(FVector{1, 0, 0}));
        if (DotV(bore, fwd) < 0) bore = bore * -1.0;  // keep it pointing toward the muzzle
        fwd = Norm(bore);
    }
    // Gun models are built along one of the actor's own axes; the parts
    // under it can be tilted for the flat-screen animations (seen in the
    // first headset test: the bore came out 12 degrees off +Y). Snap to the
    // nearest actor axis when it is close.
    auto snap = [](FVector v) {
        const FVector axes[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (const FVector& a : axes)
            if (DotV(v, a) > 0.9) return a;
        return v;
    };
    fwd = snap(fwd);
    FVector up_guess{0, 0, 1};
    if (sight) up_guess = inv.Rotate(ComponentToWorld(sight).translation - actor_loc) - grip;
    FVector up = up_guess - fwd * DotV(up_guess, fwd);
    if (Len(up) < 0.5) up = FVector{0, 0, 1} - fwd * fwd.Z;
    up = snap(Norm(up));
    if (std::fabs(DotV(up, fwd)) > 0.01) up = Norm(up - fwd * DotV(up, fwd));
    FVector right = Cross(up, fwd);
    W.gun_frame = FromAxes(fwd, right, up);
    W.have_frame = true;
    log::Info("controls: %s barrel along (%.2f %.2f %.2f), top along (%.2f %.2f %.2f) in actor space",
              ue::GetName(ue::GetClass(g)).c_str(), fwd.X, fwd.Y, fwd.Z, up.X, up.Y, up.Z);
    return true;
}


void HoldGun(game::Gun& gun, const hands::HandState& rh) {
    UObject g = gun.Object();
    UObject main = 0;
    ue::GetProp(g, "MainHandPosition", main);
    if (!main) return;
    DetachIfAttached(g);
    // Where the grip point sits relative to the gun actor, measured live.
    FVector actor_loc = ActorLocation(g);
    FQuat actor_rot = ActorRotation(g);
    UETransform main_world = ComponentToWorld(main);
    FVector rel = actor_rot.Inverse().Rotate(main_world.translation - actor_loc);

    if (!W.have_frame) LearnGunFrame(g, actor_loc, actor_rot, main_world.translation);
    FQuat hand = rh.aim.rotation * ToQuat(FRotator{g_set.gun_pitch, g_set.gun_yaw, g_set.gun_roll});
    FQuat rot = W.have_frame ? hand * W.gun_frame.Inverse() : hand;
    FVector loc = rh.grip.location - rot.Rotate(rel);
    SetActorTransform(g, loc, rot);
}

void Trigger(UObject player, const input::HandInput& rin) {
    if (rin.trigger_button.pressed || rin.trigger_button.released) {
        bool pressed = rin.trigger_button.down;
        ue::Call c(player, "PlayerInput_UseWeapon");
        c.Set("Press Released", pressed);
        c.Invoke();
        if (pressed) input::Haptic(input::kRight, 0.8f, 0.05f);
    }
}

void SlideHandling(game::Gun& gun, const hands::HandState& lh, const input::HandInput& lin, double dt) {
    UObject slide_comp = gun.SlideComponent();
    if (!slide_comp || !lh.valid) return;
    UObject parent = 0;
    ue::GetProp(slide_comp, "AttachParent", parent);
    if (!parent) return;
    UETransform pt = ComponentToWorld(parent);
    FVector hand = ToLocal(pt, lh.grip.location);

    interact::SlideConfig cfg;
    cfg.axis = FVector{-1, 0, 0};  // measured for the handgun in the weapon lab
    cfg.length = gun.SlideBackLength() > 0.5 ? gun.SlideBackLength() : 4.7;
    ue::Vec3d o = gun.SlideOrigin();
    cfg.grab_center = FVector{o.x, o.y, o.z} + cfg.axis * 4.0;
    cfg.grab_radius = 8.0;
    W.slide.Configure(cfg);

    if (lin.grip_button.pressed && W.held_mag == 0)
        log::Info("controls: left grab at slide-space (%.1f %.1f %.1f), %s the slide zone", hand.X, hand.Y,
                  hand.Z, W.slide.InZone(hand) ? "inside" : "outside");

    using interact::SlideEvent;
    SlideEvent e = W.slide.Update(hand, lin.grip_button.down && W.held_mag == 0, dt);
    switch (e) {
        case SlideEvent::kGrabbed: input::Haptic(input::kLeft, 0.3f, 0.03f); break;
        case SlideEvent::kReachedBack:
            gun.ChargeBoltPull();
            input::Haptic(input::kLeft, 0.7f, 0.05f);
            break;
        case SlideEvent::kRacked:
            log::Info("controls: rack, before: %s", gun.State().Describe().c_str());
            gun.ChargeBoltRelease();
            W.end_charging_at = g_clock + 0.4;
            W.check_chamber_at = g_clock + 0.6;
            input::Haptic(input::kLeft, 0.9f, 0.06f);
            input::Haptic(input::kRight, 0.5f, 0.05f);
            log::Info("controls: full rack");
            break;
        case SlideEvent::kPartialRelease:
        case SlideEvent::kSlipped:
            log::Info("controls: slide let go at %.0f%%", W.slide.Peak() * 100);
            break;
        default: break;
    }
    if (W.slide.Travel() > 0.001) {
        gun.SetSlideTravel(W.slide.Travel(), ue::Vec3d{cfg.axis.X, cfg.axis.Y, cfg.axis.Z});
        W.slide_was_moved = true;
    } else if (W.slide_was_moved) {
        gun.RestoreSlide();
        W.slide_was_moved = false;
    }
    if (W.end_charging_at > 0 && g_clock >= W.end_charging_at) {
        gun.EndBoltCharging();
        W.end_charging_at = -1;
    }
    // After an empty reload the slide sits on the slide catch and a rack
    // alone may not chamber a round: drop the slide off the catch as well.
    if (W.check_chamber_at > 0 && g_clock >= W.check_chamber_at) {
        W.check_chamber_at = -1;
        game::GunState st = gun.State();
        if (st.chamber == 0 && st.has_magazine && st.magazine > 0 && !W.retried_rack) {
            // Still empty: cycle the action once more (this is what loads a round).
            W.retried_rack = true;
            gun.ChargeBoltPull();
            W.pending_release_at = g_clock + 0.3;
            log::Info("controls: chamber still empty, cycling again");
        } else {
            W.retried_rack = false;
            // A rack always takes the slide off the slide catch. The game only
            // clears its "slide stopped" flag through its own animation, and a
            // gun with that flag set will not fire, so clear it here.
            if (st.slide_locked) {
                gun.ClearSlideLock();
                gun.RestoreSlide();
                log::Info("controls: cleared the slide lock");
            }
            log::Info("controls: rack, after: %s", gun.State().Describe().c_str());
        }
    }
    if (W.pending_release_at > 0 && g_clock >= W.pending_release_at) {
        W.pending_release_at = -1;
        gun.ChargeBoltRelease();
        W.end_charging_at = g_clock + 0.4;
        W.check_chamber_at = g_clock + 0.6;
    }
}

void MagazineHandling(game::Gun& gun, const hands::HandState& lh, const input::HandInput& lin,
                      const input::HandInput& rin) {
    // Right A / primary: magazine release.
    if (rin.secondary.pressed) {
        log::Info("controls: slide release (B), before: %s", gun.State().Describe().c_str());
        gun.ReleaseSlideCatch();
        W.end_charging_at = g_clock + 0.4;
        W.check_chamber_at = g_clock + 0.6;
        input::Haptic(input::kRight, 0.5f, 0.04f);
    }
    if (rin.primary.pressed && gun.State().has_magazine) {
        log::Info("controls: mag release, before: %s", gun.State().Describe().c_str());
        gun.DropMagazine();
        input::Haptic(input::kRight, 0.4f, 0.04f);
    }
    if (!lh.valid) return;

    // Left grab low on the body (below the chest) with an empty gun: take a
    // magazine from the pouch.
    hands::Pose head;
    bool low = hands::Head(&head) && lh.grip.location.Z < head.location.Z - 35.0;
    if (lin.grip_button.pressed && !W.held_mag && !W.slide.Held() && low && !gun.State().has_magazine) {
        gun.TakeNewMagazine();
        UObject mag = 0;
        ue::GetProp(gun.Object(), "MagazineObj_Temp", mag);
        if (!mag) ue::GetProp(gun.Object(), "MagazineObj", mag);
        W.held_mag = mag;
        input::Haptic(input::kLeft, 0.4f, 0.04f);
        log::Info("controls: took a magazine (%s)", mag ? ue::GetName(mag).c_str() : "none found");
    }
    if (!W.held_mag) return;

    // Carry it in the left hand.
    DetachIfAttached(W.held_mag);
    { ue::Call v(W.held_mag, "SetVisibility"); v.Set("IsVisible", true); v.Invoke(); }
    SetActorTransform(W.held_mag, lh.grip.location, lh.aim.rotation);

    // Insert when it reaches the magwell.
    UObject well = 0;
    ue::GetProp(gun.Object(), "MagazinePoint", well);
    if (well) {
        FVector d = ComponentToWorld(well).translation - lh.grip.location;
        if (std::sqrt(d.X * d.X + d.Y * d.Y + d.Z * d.Z) < 9.0) {
            gun.InsertMagazine();
            UObject mag = 0;
            if (ue::GetProp(gun.Object(), "MagazineObj", mag) && mag) {
                ue::Call v(mag, "SetVisibility");
                v.Set("IsVisible", true);
                v.Invoke();
            }
            W.held_mag = 0;
            input::Haptic(input::kLeft, 0.8f, 0.05f);
            input::Haptic(input::kRight, 0.6f, 0.05f);
            log::Info("controls: magazine inserted: %s", gun.State().Describe().c_str());
            return;
        }
    }
    // Let go of the grab: drop it.
    if (lin.grip_button.released) {
        ue::Call drop(W.held_mag, "DropMagazine");
        drop.Invoke();
        W.held_mag = 0;
        log::Info("controls: dropped the held magazine");
    }
}

void PublishGrabs(game::Gun& gun, const hands::HandState& rh, const hands::HandState& lh) {
    hands::HandGrab g[2];
    if (gun.Valid() && rh.valid) {
        g[hands::kRight].kind = hands::GrabKind::kGunGrip;
        g[hands::kRight].object = gun.Object();
        UObject main = 0;
        if (ue::GetProp(gun.Object(), "MainHandPosition", main) && main) {
            UETransform t = ComponentToWorld(main);
            g[hands::kRight].target = {t.translation, t.rotation};
        } else {
            g[hands::kRight].target = rh.grip;
        }
    }
    if (lh.valid) {
        if (W.slide.Held()) {
            g[hands::kLeft].kind = hands::GrabKind::kSlideGrab;
            g[hands::kLeft].object = gun.Object();
            g[hands::kLeft].target = lh.grip;
        } else if (W.held_mag) {
            g[hands::kLeft].kind = hands::GrabKind::kMagHeld;
            g[hands::kLeft].object = W.held_mag;
            g[hands::kLeft].target = lh.grip;
        }
    }
    std::lock_guard<std::mutex> lock(g_hands_mutex);
    g_grab[0] = g[0];
    g_grab[1] = g[1];
}

void OnTick(UObject player, float dt) {
    g_clock += dt;
    input::Snapshot s = input::Consume();
    if (!s.valid || !g_set.enabled) return;
    UpdateHands(s);
    Locomotion(player, s, dt);

    game::Gun gun = game::Gun::Current(player);
    if (gun.Object() != W.gun) {
        W = WeaponState();
        W.gun = gun.Object();
    }
    if (!gun.Valid()) {
        hands::HandState none;
        PublishGrabs(gun, none, none);
        return;
    }
    hands::HandState rh = hands::Get(hands::kRight), lh = hands::Get(hands::kLeft);
    if (rh.valid) HoldGun(gun, rh);
    Trigger(player, s.hand[input::kRight]);
    SlideHandling(gun, lh, s.hand[input::kLeft], dt);
    MagazineHandling(gun, lh, s.hand[input::kLeft], s.hand[input::kRight]);
    PublishGrabs(gun, rh, lh);
}

}  // namespace

void Init() {
    LoadSettings();
    game::AddTickListener(&OnTick);
    log::Info("controls: ready (%s turn, move by %s)", g_set.smooth_turn ? "smooth" : "snap",
              g_set.move_with_hand ? "hand" : "head");
}

}  // namespace holevr::vr::controls

namespace holevr::vr::hands {
HandState Get(Hand hand) {
    std::lock_guard<std::mutex> lock(controls::g_hands_mutex);
    return controls::g_hands[hand];
}
HandGrab GetGrab(Hand hand) {
    std::lock_guard<std::mutex> lock(controls::g_hands_mutex);
    return controls::g_grab[hand];
}
FingerInput GetFingers(Hand hand) {
    std::lock_guard<std::mutex> lock(controls::g_hands_mutex);
    return controls::g_fingers[hand];
}
bool Head(Pose* out) {
    std::lock_guard<std::mutex> lock(controls::g_hands_mutex);
    if (!controls::g_head_valid) return false;
    *out = controls::g_head;
    return true;
}
}  // namespace holevr::vr::hands
