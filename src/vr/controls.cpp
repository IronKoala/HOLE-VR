#include "vr/controls.h"

#include <windows.h>

#include <cmath>
#include <mutex>

#include "core/config.h"
#include "core/log.h"
#include "game/game.h"
#include "game/gun.h"
#include "game/weapon_lab.h"
#include "ue/call.h"
#include "ue/process_event.h"
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
    double gun_forward = 0.0;      // extra shift of the gun along the barrel, cm
    double gun_right = 0.0;        // extra shift to the right, cm
    // Magazine pouch, relative to where you face: cm forward, right (+) /
    // left (-), and below the headset.
    double pouch_forward = 15.0;
    double pouch_side = -15.0;
    double pouch_down = 55.0;
    double pouch_radius = 14.0;
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
    g_set.gun_forward = config::GetFloat("gun_forward_offset_cm", 0.f);
    g_set.gun_right = config::GetFloat("gun_right_offset_cm", 0.f);
    g_set.pouch_forward = config::GetFloat("mag_pouch_forward_cm", 15.f);
    g_set.pouch_side = config::GetFloat("mag_pouch_side_cm", -15.f);
    g_set.pouch_down = config::GetFloat("mag_pouch_down_cm", 55.f);
    g_set.pouch_radius = config::GetFloat("mag_pouch_grab_radius_cm", 14.f);
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
    UObject pouch_mag = 0;     // spare magazine shown at the pouch, ready to grab
    UObject mag_class = 0;     // class of this gun's magazines, for spawning spares
    double refill_pouch_at = 0; // when to put a new magazine in the pouch
    bool trigger_down = false;
    bool have_frame = false;
    FQuat gun_frame;           // gun frame (X barrel, Z up) expressed in actor space
    double check_chamber_at = -1;
    double pending_release_at = -1;
    bool retried_rack = false;
    bool dead_trigger = false;  // Glock: after a dry fire the trigger is dead until the slide moves
    bool trigger_sent = false;  // the game saw the current trigger press
    double lock_after_shot_at = -1; // last round fired with no magazine: lock the slide
    bool lock_on_release = false; // B pressed while holding the slide back: lock it open
    bool timeline_fast = false;   // game's charge timeline sped up for this gun
    // Gun pose this frame (from HoldGun).
    bool have_pose = false;
    FQuat actor_rot;              // gun actor rotation in the world
    FQuat frame_rot;              // gun frame (X barrel, Z up) in the world
    // Where the slide grab point is in the world, for snapping the hand to it.
    bool have_slide_point = false;
    FVector slide_point;
    // Magazine seating, Half-Life: Alyx style: bring the mag up under the
    // grip, it locks onto the magwell axis, push it in and it seats itself.
    bool mag_engaged = false;
    bool mag_seating = false;
    double seat_from = 0, seat_t0 = 0;
    FVector mag_shown;            // where the held mag is drawn
    bool seat_learned = false;
    FVector seat_offset;          // inserted mag origin minus MagazinePoint, actor space
    FQuat seat_rot;               // inserted mag rotation, actor space
    UObject learn_mag = 0;
    double learn_at = -1;
    double trace_until = -1;      // per-frame rack trace (diagnosing the chamber hitch)
    FVector gun_set_loc;          // where HoldGun put the gun this frame
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
    // The controller's pointing ray sits about 30 degrees above a pistol's
    // bore in a natural grip (second headset test: barrel at 10 o'clock
    // instead of 9 seen from the side), so pitch down by that first.
    // Third test: barrel about 5 degrees left of where the hand points.
    constexpr double kBasePitch = -30.0;
    constexpr double kBaseYaw = 5.0;
    FQuat hand = rh.aim.rotation * ToQuat(FRotator{kBasePitch + g_set.gun_pitch,
                                                   kBaseYaw + g_set.gun_yaw, g_set.gun_roll});
    FQuat rot = W.have_frame ? hand * W.gun_frame.Inverse() : hand;
    // `hand` is the gun's own frame in the world (X barrel, Y right, Z up).
    // Fourth test: gun sat about 5 cm too far forward and 1.3 cm too far left.
    // Fifth test: about 5 cm too high.
    constexpr double kBaseForward = -5.0, kBaseRight = 1.3, kBaseUp = -5.0;
    FVector shift = hand.Rotate(FVector{kBaseForward + g_set.gun_forward, kBaseRight + g_set.gun_right, kBaseUp});
    FVector loc = rh.grip.location + shift - rot.Rotate(rel);
    SetActorTransform(g, loc, rot);
    W.have_pose = true;
    W.gun_set_loc = loc;
    W.actor_rot = rot;
    W.frame_rot = hand;
}

void Trigger(UObject player, game::Gun& gun, const input::HandInput& rin) {
    // The game's parameter means "trigger released": false on press, true on release.
    auto send = [&](bool released) {
        ue::Call c(player, "PlayerInput_UseWeapon");
        c.Set("Press Released", released);
        c.Invoke();
    };
    if (rin.trigger_button.pressed) {
        if (W.dead_trigger) {
            log::Info("controls: dead trigger (dry fired, slide not moved since)");
            return;
        }
        game::GunState st = gun.State();
        const bool dry = st.chamber == 0 && !st.slide_locked;
        send(false);
        W.trigger_sent = true;
        if (st.chamber > 0 && !st.has_magazine && !st.slide_locked)
            W.lock_after_shot_at = g_clock + 0.25;   // after the game's recoil animation
        if (dry) {
            W.dead_trigger = true;
            input::Haptic(input::kRight, 0.25f, 0.02f);
        } else {
            input::Haptic(input::kRight, 0.8f, 0.05f);
        }
    } else if (rin.trigger_button.released && W.trigger_sent) {
        send(true);
        W.trigger_sent = false;
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
    // Locked open (empty gun): the slide rests on the catch, and pulling it a
    // little further back and letting go releases it, like a real pistol.
    const bool locked = gun.State().slide_locked;
    const double stop = gun.SlideStopLength();
    const double rest = locked && stop > 0 ? std::fmin(0.95, stop / cfg.length) : 0.0;
    if (locked) cfg.full_threshold = std::fmin(1.0, rest + 0.08);
    W.slide.Configure(cfg);
    W.slide.SetRest(rest);

    if (lin.grip_button.pressed && W.held_mag == 0)
        log::Info("controls: left grab at slide-space (%.1f %.1f %.1f), %s the slide zone", hand.X, hand.Y,
                  hand.Z, W.slide.InZone(hand) ? "inside" : "outside");

    using interact::SlideEvent;
    SlideEvent e = W.slide.Update(hand, lin.grip_button.down && W.held_mag == 0, dt);
    switch (e) {
        case SlideEvent::kGrabbed:
            input::Haptic(input::kLeft, 0.3f, 0.03f);
            W.trace_until = g_clock + 30.0;   // until released + 1.5 s
            break;
        case SlideEvent::kReachedBack:
            gun.ChargeBoltPull();
            input::Haptic(input::kLeft, 0.7f, 0.05f);
            break;
        case SlideEvent::kRacked: {
            game::GunState st = gun.State();
            log::Info("controls: rack, before: %s", st.Describe().c_str());
            // Glock: the slide stays open if the thumb pushed the slide stop up
            // (B while holding it back) or an empty magazine's follower lifts it.
            if (W.lock_on_release || (st.has_magazine && st.magazine == 0)) {
                W.lock_on_release = false;
                gun.EndBoltCharging();
                ue::SetProp<bool>(gun.Object(), "IsBolt(Slide)Stopped", true);
                input::Haptic(input::kLeft, 0.6f, 0.04f);
                log::Info("controls: slide locked open: %s", gun.State().Describe().c_str());
                break;
            }
            // Racked off the slide catch: the slide is free now. Clear the
            // game's lock flag before it runs forward, or the slide stops at
            // the catch until the flag is cleared later (the chamber hitch).
            if (st.slide_locked) {
                gun.ClearSlideLock();
                W.slide.SetRest(0.0);
            }
            gun.ChargeBoltRelease();
            W.end_charging_at = g_clock + 0.1;
            W.check_chamber_at = g_clock + 0.15;
            W.trace_until = g_clock + 1.5;
            input::Haptic(input::kLeft, 0.9f, 0.06f);
            input::Haptic(input::kRight, 0.5f, 0.05f);
            log::Info("controls: full rack");
            break;
        }
        case SlideEvent::kPartialRelease:
        case SlideEvent::kSlipped:
            W.lock_on_release = false;
            W.trace_until = g_clock + 1.5;
            log::Info("controls: slide let go at %.0f%%", W.slide.Peak() * 100);
            break;
        default: break;
    }
    // The hand snaps to the slide's grab point while it holds the slide.
    W.slide_point = pt.translation +
                    pt.rotation.Rotate(cfg.grab_center + cfg.axis * (W.slide.Travel() * cfg.length));
    W.have_slide_point = true;
    // Moving the slide a few mm resets a Glock's trigger after a dry fire.
    if (W.dead_trigger && W.slide.Travel() > rest + 0.12) {
        W.dead_trigger = false;
        log::Info("controls: trigger reset by the slide");
    }
    if (W.slide.Travel() > 0.001 && (W.slide.Held() || !locked || W.slide.Travel() > rest + 0.001)) {
        gun.SetSlideTravel(W.slide.Travel(), ue::Vec3d{cfg.axis.X, cfg.axis.Y, cfg.axis.Z});
        W.slide_was_moved = true;
    } else if (W.slide_was_moved) {
        // Back to rest: closed, or sitting on the slide catch if locked open.
        if (locked) gun.SetSlideTravel(rest, ue::Vec3d{cfg.axis.X, cfg.axis.Y, cfg.axis.Z});
        else gun.RestoreSlide();
        W.slide_was_moved = false;
    }
    // Owner's rule: firing the round in the chamber with no magazine locks the
    // slide back, like the last round of a magazine.
    if (W.lock_after_shot_at > 0 && g_clock >= W.lock_after_shot_at && !W.slide.Held()) {
        W.lock_after_shot_at = -1;
        game::GunState st = gun.State();
        if (st.chamber == 0 && !st.has_magazine && !st.slide_locked && !st.firing) {
            ue::SetProp<bool>(gun.Object(), "IsBolt(Slide)Stopped", true);
            double r = stop > 0 ? std::fmin(0.95, stop / cfg.length) : 0.77;
            W.slide.SetRest(r);
            gun.SetSlideTravel(r, ue::Vec3d{cfg.axis.X, cfg.axis.Y, cfg.axis.Z});
            log::Info("controls: last round fired with no magazine, slide locked back");
        } else if (st.firing) {
            W.lock_after_shot_at = g_clock + 0.05;
        }
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
            W.pending_release_at = g_clock + 0.08;
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
        W.end_charging_at = g_clock + 0.1;
        W.check_chamber_at = g_clock + 0.15;
    }
}

// Creates a spare magazine actor of the same class as the gun's own. The
// game's "take a new magazine" step can't be used while a magazine is in the
// gun: it removes the inserted one (fourth test: inserted mags vanished).
UObject SpawnMagazine(UObject player, UObject cls, const FVector& loc, const FQuat& rot) {
    static UObject statics = 0;
    if (!statics) statics = ue::FindObject("GameplayStatics /Script/Engine.Default__GameplayStatics");
    if (!statics || !cls) return 0;
    UETransform t;
    t.rotation = rot;
    t.translation = loc;
    ue::Call begin(statics, "BeginDeferredActorSpawnFromClass");
    const uint8_t always_spawn = 1;  // ESpawnActorCollisionHandlingMethod::AlwaysSpawn
    begin.Set("WorldContextObject", player).Set("ActorClass", cls).Set("SpawnTransform", t)
        .Set("CollisionHandlingOverride", always_spawn).Set("Owner", player);
    if (!begin.Invoke()) return 0;
    UObject actor = begin.Result<UObject>();
    if (!actor) return 0;
    ue::Call finish(statics, "FinishSpawningActor");
    finish.Set("Actor", actor).Set("SpawnTransform", t);
    finish.Invoke();
    UObject done = finish.Result<UObject>();
    return done ? done : actor;
}

void RefillPouch(game::Gun& gun, UObject player, const FVector& loc, const FQuat& rot) {
    if (!W.mag_class) {
        UObject m = 0;
        if (ue::GetProp(gun.Object(), "MagazineObj", m) && m) W.mag_class = ue::GetClass(m);
        else if (ue::GetProp(gun.Object(), "MagazineObj_Temp", m) && m) W.mag_class = ue::GetClass(m);
    }
    UObject mag = 0;
    if (W.mag_class) {
        mag = SpawnMagazine(player, W.mag_class, loc, rot);
        if (mag) log::Info("controls: spawned pouch magazine %s", ue::GetName(mag).c_str());
    } else if (!gun.State().has_magazine) {
        // Empty gun and no class known yet: the game's own step is safe here.
        gun.TakeNewMagazine();
        ue::GetProp(gun.Object(), "MagazineObj_Temp", mag);
        if (mag) W.mag_class = ue::GetClass(mag);
        if (mag) log::Info("controls: took pouch magazine from the game (%s)", ue::GetName(mag).c_str());
    }
    if (mag) {
        W.pouch_mag = mag;
    } else {
        W.refill_pouch_at = g_clock + 2.0;   // try again later
    }
}

void MagazineHandling(game::Gun& gun, UObject player, const hands::HandState& lh, const input::HandInput& lin,
                      const input::HandInput& rin) {
    // Right A / primary: magazine release.
    // Right B: the slide stop lever. Pressed down on a locked slide it
    // releases it; with the slide held back by the left hand it pushes the
    // lever up so the slide stays open; otherwise it does nothing.
    // B held at any moment while the slide is back far enough pushes the
    // lever up (pressed before or during the rack, released before the slide).
    if (rin.secondary.down && W.slide.Held() && !W.lock_on_release) {
        double len = gun.SlideBackLength() > 0.5 ? gun.SlideBackLength() : 4.7;
        double stop = gun.SlideStopLength() > 0 ? gun.SlideStopLength() / len : 0.75;
        if (W.slide.Travel() >= stop - 0.05) {
            W.lock_on_release = true;
            input::Haptic(input::kRight, 0.3f, 0.03f);
            log::Info("controls: slide stop pushed up, slide will stay open");
        }
    }
    if (rin.secondary.pressed && !W.slide.Held()) {
        game::GunState st = gun.State();
        // An empty magazine's follower holds the lever up: B can't drop it.
        if (st.slide_locked && st.has_magazine && st.magazine == 0) {
            log::Info("controls: slide release blocked by the empty magazine");
        } else if (st.slide_locked) {
            log::Info("controls: slide release (B), before: %s", st.Describe().c_str());
            gun.ReleaseSlideCatch();
            W.dead_trigger = false;
            W.end_charging_at = g_clock + 0.4;
            W.check_chamber_at = g_clock + 0.6;
            input::Haptic(input::kRight, 0.5f, 0.04f);
        }
    }
    if (rin.primary.pressed && gun.State().has_magazine) {
        log::Info("controls: mag release, before: %s", gun.State().Describe().c_str());
        gun.DropMagazine();
        input::Haptic(input::kRight, 0.4f, 0.04f);
    }
    if (!lh.valid) return;

    // A spare magazine always sits in a pouch at the left front of the belt,
    // following where you face, so you can see where to reach.
    hands::Pose head;
    bool have_head = hands::Head(&head);
    FVector pouch_loc;
    FQuat pouch_rot;
    if (have_head) {
        double yaw = YawOf(head.rotation);
        pouch_rot = ToQuat(FRotator{0, yaw, 0});
        pouch_loc = head.location + pouch_rot.Rotate(FVector{g_set.pouch_forward, g_set.pouch_side, 0}) -
                    FVector{0, 0, g_set.pouch_down};
    }
    if (!W.pouch_mag && !W.held_mag && g_clock >= W.refill_pouch_at && have_head)
        RefillPouch(gun, player, pouch_loc, pouch_rot);
    if (W.pouch_mag && have_head) {
        DetachIfAttached(W.pouch_mag);
        { ue::Call v(W.pouch_mag, "SetVisibility"); v.Set("IsVisible", true); v.Invoke(); }
        SetActorTransform(W.pouch_mag, pouch_loc, pouch_rot);
    }

    // Left grab at the pouch: take that magazine, whether or not the gun has one.
    if (lin.grip_button.pressed && W.pouch_mag && !W.held_mag && !W.slide.Held() && have_head) {
        FVector d = lh.grip.location - pouch_loc;
        if (std::sqrt(d.X * d.X + d.Y * d.Y + d.Z * d.Z) <= g_set.pouch_radius) {
            W.held_mag = W.pouch_mag;
            W.pouch_mag = 0;
            input::Haptic(input::kLeft, 0.4f, 0.04f);
            log::Info("controls: took the pouch magazine");
        }
    }
    if (!W.held_mag) return;

    // Carry it in the left hand.
    DetachIfAttached(W.held_mag);
    { ue::Call v(W.held_mag, "SetVisibility"); v.Set("IsVisible", true); v.Invoke(); }
    FVector show_loc = lh.grip.location;
    FQuat show_rot = lh.aim.rotation;

    // Magwell: the mag must come up from under the grip along the magwell
    // axis. Close enough to that axis it locks on and slides along it; pushed
    // most of the way in, it seats itself (a short animation of what the hand
    // was doing anyway). Touching it to the top of the gun does nothing.
    UObject well = 0;
    ue::GetProp(gun.Object(), "MagazinePoint", well);
    const bool can_seat = well && W.have_pose && !gun.State().has_magazine;
    if (can_seat) {
        FVector seat = ComponentToWorld(well).translation;
        if (W.seat_learned) seat = seat + W.actor_rot.Rotate(W.seat_offset);
        // Out of the magwell: down the grip, raked back about 18 degrees.
        constexpr double kRake = 18.0 * 3.14159265358979 / 180.0;
        FVector out = Norm(W.frame_rot.Rotate(FVector{-std::sin(kRake), 0, -std::cos(kRake)}));
        FVector p = lh.grip.location - seat;
        double along = DotV(p, out);
        double lateral = Len(p - out * along);
        FQuat seat_rot = W.seat_learned ? W.actor_rot * W.seat_rot : show_rot;

        if (W.mag_seating) {
            double t = std::fmin(1.0, (g_clock - W.seat_t0) / 0.1);
            along = W.seat_from * (1.0 - t);
            show_loc = seat + out * along;
            show_rot = seat_rot;
            SetActorTransform(W.held_mag, show_loc, show_rot);
            W.mag_shown = show_loc;
            if (t < 1.0) return;
            W.mag_seating = false;
            W.mag_engaged = false;
            ue::SetProp<UObject>(gun.Object(), "MagazineObj_Temp", W.held_mag);
            gun.InsertMagazine();
            UObject mag = 0;
            if (ue::GetProp(gun.Object(), "MagazineObj", mag) && mag) {
                ue::Call v(mag, "SetVisibility");
                v.Set("IsVisible", true);
                v.Invoke();
                if (!W.seat_learned) {
                    W.learn_mag = mag;
                    W.learn_at = g_clock + 0.05;
                }
            }
            W.held_mag = 0;
            W.refill_pouch_at = g_clock + 0.5;
            input::Haptic(input::kLeft, 0.9f, 0.05f);
            input::Haptic(input::kRight, 0.7f, 0.05f);
            log::Info("controls: magazine seated: %s", gun.State().Describe().c_str());
            return;
        }
        if (!W.mag_engaged) {
            if (along > 2.0 && along < 14.0 && lateral < 4.5) {
                W.mag_engaged = true;
                input::Haptic(input::kLeft, 0.25f, 0.02f);
                log::Info("controls: magazine lined up with the magwell (%.1f cm below, %.1f off axis)",
                          along, lateral);
            }
        } else if (lateral > 7.0 || along > 17.0) {
            W.mag_engaged = false;
        }
        if (W.mag_engaged) {
            double a = std::fmax(0.0, along);
            show_loc = seat + out * a;
            show_rot = seat_rot;
            if (a < 6.0 && lin.grip_button.down) {
                W.mag_seating = true;
                W.seat_from = a;
                W.seat_t0 = g_clock;
            }
        }
    } else {
        W.mag_engaged = false;
    }
    SetActorTransform(W.held_mag, show_loc, show_rot);
    W.mag_shown = show_loc;

    // Let go of the grab: drop it.
    if (lin.grip_button.released) {
        W.mag_engaged = false;
        ue::Call drop(W.held_mag, "DropMagazine");
        drop.Invoke();
        W.held_mag = 0;
        W.refill_pouch_at = g_clock + 0.5;
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
            // Rotation = the gun's own frame (X barrel, Y right, Z up); the
            // body builds the hand around the grip from it.
            g[hands::kRight].target = {t.translation, W.have_pose ? W.frame_rot : t.rotation};
        } else {
            g[hands::kRight].target = rh.grip;
        }
    }
    if (lh.valid) {
        if (W.slide.Held()) {
            g[hands::kLeft].kind = hands::GrabKind::kSlideGrab;
            g[hands::kLeft].object = gun.Object();
            g[hands::kLeft].target = lh.grip;
            if (W.have_slide_point) g[hands::kLeft].target.location = W.slide_point;
        } else if (W.held_mag) {
            g[hands::kLeft].kind = hands::GrabKind::kMagHeld;
            g[hands::kLeft].object = W.held_mag;
            g[hands::kLeft].target = lh.grip;
            if (W.mag_engaged || W.mag_seating) g[hands::kLeft].target.location = W.mag_shown;
        }
    }
    std::lock_guard<std::mutex> lock(g_hands_mutex);
    g_grab[0] = g[0];
    g_grab[1] = g[1];
}

// Per-frame trace while the slide is handled and 1.5 s after: frame time,
// hand travel, where the game has the slide, charging/chamber state, and how
// far the gun actor sits from where HoldGun put it. Diagnoses the chamber hitch.
void RackTrace(game::Gun& gun, float dt) {
    if (W.trace_until < 0) return;
    if (g_clock > W.trace_until) {
        W.trace_until = -1;
        game::weapon_lab::SetTrace(false);
        log::Info("trace: end");
        return;
    }
    game::weapon_lab::SetTrace(true);
    game::GunState st = gun.State();
    FVector at = ActorLocation(gun.Object());
    log::Info("trace: dt=%.1fms held=%d travel=%.2f game_slide=%.2f charging=%d chamber=%d mag=%d locked=%d "
              "gun_off=%.2fcm",
              dt * 1000.0, W.slide.Held(), W.slide.Travel(), st.slide_location.x, st.bolt_charging,
              st.chamber, st.magazine, st.slide_locked, Len(at - W.gun_set_loc));
}

// The game's charge-bolt step plays a short timeline that snaps the slide
// back and forward. In VR the hand already moved the slide, so after each
// timeline frame put the slide back where the hand (or its spring) has it.
void AfterChargeTimeline(UObject self, UObject, void*) {
    if (!g_set.enabled || !self || self != W.gun) return;
    game::Gun gun(self);
    gun.SetSlideTravel(W.slide.Travel(), ue::Vec3d{-1, 0, 0});
}

// The game's recoil (BP_Player Recoil: camera, arm and control-rotation
// kick timelines) moves the VR camera and aim. In VR the kick belongs to the
// hand only, so the call is dropped while motion controls are on.
bool BeforePlayerRecoil(UObject, UObject, void*) { return !g_set.enabled; }

void HookPlayerRecoil(UObject player) {
    static bool done = false;
    if (done || !player) return;
    UObject fn = ue::FindFunction(ue::GetClass(player), "Recoil");
    if (fn) ue::process_event::On(fn, &BeforePlayerRecoil, nullptr);
    done = true;
    log::Info("controls: game camera recoil %s", fn ? "disabled" : "function not found");
}

void HookChargeTimeline() {
    static bool done = false;
    UObject cls = game::GunBaseClass();
    if (done || !cls) return;
    UObject update = ue::FindFunction(cls, "TL_ChargeBolt__UpdateFunc");
    UObject finished = ue::FindFunction(cls, "TL_ChargeBolt__FinishedFunc");
    if (update) ue::process_event::On(update, nullptr, &AfterChargeTimeline);
    if (finished) ue::process_event::On(finished, nullptr, &AfterChargeTimeline);
    done = true;
    log::Info("controls: charge timeline %s", update ? "overridden by the hand" : "not found");
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
    HookChargeTimeline();
    HookPlayerRecoil(player);
    if (!W.timeline_fast) {
        // The game's charge timeline only has to update its ammo state; the
        // hand already moved the slide. Run it 10x faster so the round is
        // chambered the moment the slide closes (no hitch).
        UObject tl = 0;
        if (ue::GetProp(gun.Object(), "TL_ChargeBolt", tl) && tl) {
            ue::Call r(tl, "SetPlayRate");
            r.Set("NewRate", 10.0f);
            r.Invoke();
        }
        W.timeline_fast = true;
    }
    if (W.learn_at > 0 && g_clock >= W.learn_at && W.have_pose) {
        W.learn_at = -1;
        UObject well = 0;
        ue::GetProp(gun.Object(), "MagazinePoint", well);
        UObject mag = 0;
        ue::GetProp(gun.Object(), "MagazineObj", mag);
        if (well && mag == W.learn_mag) {
            FVector off = ActorLocation(mag) - ComponentToWorld(well).translation;
            if (Len(off) < 15.0) {
                FQuat inv = ActorRotation(gun.Object()).Inverse();
                W.seat_offset = inv.Rotate(off);
                W.seat_rot = inv * ActorRotation(mag);
                W.seat_learned = true;
                log::Info("controls: learned magazine seat (%.1f %.1f %.1f)", W.seat_offset.X,
                          W.seat_offset.Y, W.seat_offset.Z);
            }
        }
    }
    {
        // Left X: weapon light.
        const input::HandInput& l = s.hand[input::kLeft];
        if (l.primary.pressed || l.primary.released) {
            ue::Call c(player, "PlayerInput_SwitchLight");
            c.Set("IsPressed", l.primary.down);
            c.Invoke();
        }
    }
    hands::HandState rh = hands::Get(hands::kRight), lh = hands::Get(hands::kLeft);
    if (rh.valid) HoldGun(gun, rh);
    Trigger(player, gun, s.hand[input::kRight]);
    SlideHandling(gun, lh, s.hand[input::kLeft], dt);
    MagazineHandling(gun, player, lh, s.hand[input::kLeft], s.hand[input::kRight]);
    PublishGrabs(gun, rh, lh);
    RackTrace(gun, dt);
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
