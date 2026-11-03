#include "vr/controls.h"

#include <windows.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "game/game.h"
#include "game/gun.h"
#include "game/weapon_lab.h"
#include "ue/call.h"
#include "ue/process_event.h"
#include "ue/reflection.h"
#include "vr/body/body.h"
#include "vr/comfort.h"
#include "vr/comfort_math.h"
#include "vr/teleport.h"
#include "vr/hands.h"
#include "vr/input.h"
#include "vr/interact/holster.h"
#include "vr/interact/slide.h"
#include "vr/ui_panel.h"
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
    // Holsters (vr/interact/holster.h): pistol on the right hip, spare
    // magazine (the pouch) on the left hip.
    bool holster_pistol = true;     // false: the pistol is always in the hand
    bool holster_toggle = false;    // squeeze to draw, squeeze at the holster to put away
    double holster_cant = 10.0;     // grip tilted forward, degrees
    double belt_below_head = 60.0;  // belt height when the VR body is off, cm below the headset
    bool rack_trace = false;            // per-frame slide log while racking (debug)
    double slide_pinch_radius = 6.5;    // pinch zone around the rear sight...
    double slide_pinch_back = 0.64;     // ...stretched this far back along the slide (cm)
    double slide_grab_forward = 16.0;   // pistol slide grab zone reaches this far forward of the rear grab point
    bool wall_collision = true;    // keep the gun out of walls
    double recoil_kick = 1.0;      // scale of the in-hand recoil kick, 0 = off
    bool game_recoil = false;      // let the game's camera/aim recoil run
    bool camera_shake = false;     // the game's camera shake option
    bool log_grabs = true;
    // Rifle virtual stock: near the shoulder the rifle pivots from it.
    bool virtual_stock = true;
    double stock_snap = 12.0;      // cm from the shoulder pocket to lock on
    double stock_release = 20.0;   // cm the hands must pull it away to let go
    double stock_forward = 0.0;    // move the shoulder pocket, cm (head frame)
    double stock_right = 0.0;
    double stock_up = 0.0;
    bool right_eye = true;         // aiming eye (the stock sits on that side)
    bool room_scale = true;        // the character follows you around the room
    // VR tab settings (2026-11-03).
    bool grip_toggle = false;      // magazines: squeeze to take, squeeze again to let go
    bool teleport = false;         // locomotion=teleport: the left stick aims a teleport arc
    double standing_height_cm = 0; // measured standing head height (0 = learn it as you play)
    bool swap_sticks = false;      // move_stick=right: the right stick moves, the left one turns
    double aim_smoothing = 0.0;    // 0 off .. 1 strongest
    bool real_height = false;      // fixed_height=0: the view follows your real head height
    bool seated = false;           // height_mode=seated
    bool physical_crouch = true;
    double physical_crouch_cm = 35.0;
    bool physical_jump = false;
};
Settings g_set;

void LoadHolsterSpots();

void LoadSettings() {
    g_set.enabled = config::GetBool("vr_controls", true);
    g_set.smooth_turn = config::GetInt("turn_mode", 1) == 1;
    g_set.snap_degrees = config::GetFloat("snap_turn_degrees", 30.f);
    g_set.smooth_speed = config::GetFloat("smooth_turn_speed", 90.f);
    g_set.move_with_hand = config::GetInt("move_direction", 0) == 1;
    g_set.deadzone = config::GetFloat("stick_deadzone", 0.15f);
    g_set.gun_pitch = config::GetFloat("gun_pitch_offset", 0.f);
    g_set.gun_yaw = config::GetFloat("gun_yaw_offset", 0.f);
    g_set.gun_roll = config::GetFloat("gun_roll_offset", 0.f);
    g_set.wall_collision = config::GetBool("gun_wall_collision", true);
    g_set.recoil_kick = config::GetFloat("gun_recoil_kick", 1.f);
    g_set.game_recoil = config::GetBool("game_camera_recoil", false);
    g_set.camera_shake = config::GetBool("game_camera_shake", false);
    g_set.gun_forward = config::GetFloat("gun_forward_offset_cm", 0.f);
    g_set.gun_right = config::GetFloat("gun_right_offset_cm", 0.f);
    g_set.holster_pistol = config::GetBool("holster_pistol", true);
    g_set.holster_toggle = config::GetBool("holster_pistol_grip_toggle", false);
    g_set.holster_cant = config::GetFloat("holster_pistol_cant_deg", 10.f);
    g_set.belt_below_head = config::GetFloat("holster_belt_below_head_cm", 60.f);
    g_set.slide_grab_forward = config::GetFloat("slide_grab_forward_cm", 16.f);
    g_set.rack_trace = config::GetBool("debug_rack_trace", false);
    g_set.slide_pinch_radius = config::GetFloat("slide_pinch_radius_cm", 6.5f);
    g_set.slide_pinch_back = config::GetFloat("slide_pinch_back_cm", 0.64f);
    g_set.virtual_stock = config::GetBool("rifle_virtual_stock", true);
    g_set.stock_snap = config::GetFloat("rifle_stock_snap_cm", 12.f);
    g_set.stock_release = config::GetFloat("rifle_stock_release_cm", 20.f);
    g_set.stock_forward = config::GetFloat("rifle_stock_forward_cm", 0.f);
    g_set.stock_right = config::GetFloat("rifle_stock_right_cm", 0.f);
    g_set.stock_up = config::GetFloat("rifle_stock_up_cm", 0.f);
    g_set.right_eye = config::GetInt("dominant_eye_right", 1) != 0;
    g_set.room_scale = config::GetBool("room_scale_follow", true);
    // One grip toggle for everything held (the pistol's own key still works
    // when grip_toggle isn't in the ini yet).
    g_set.grip_toggle = config::GetBool("grip_toggle", false);
    g_set.teleport = config::GetString("locomotion", "smooth") == "teleport";
    g_set.standing_height_cm = config::GetFloat("standing_height_cm", 0.f);
    g_set.swap_sticks = config::GetString("move_stick", "left") == "right";
    g_set.holster_toggle = config::GetBool("grip_toggle", config::GetBool("holster_pistol_grip_toggle", false));
    g_set.aim_smoothing = std::fmax(0.0, std::fmin(1.0, config::GetFloat("aim_smoothing", 0.f)));
    g_set.real_height = !config::GetBool("fixed_height", true);
    g_set.seated = !g_set.real_height && config::GetString("height_mode", "fixed") == "seated";
    g_set.physical_crouch = config::GetBool("physical_crouch", true) && !g_set.seated;
    g_set.physical_crouch_cm = config::GetFloat("physical_crouch_cm", 35.f);
    g_set.physical_jump = config::GetBool("physical_jump", false) && !g_set.seated;
    LoadHolsterSpots();
}

// ---- holster spots ----
// Offsets from the belt come from hole-vr.ini (holster_<name>_forward_cm ...),
// and spots placed in the headset (on any body part, at any angle) are saved
// to hole-vr/holsters.ini, which wins.
using interact::HolsterId;
constexpr int kPistolSpot = static_cast<int>(HolsterId::kPistol);
constexpr int kMagSpot = static_cast<int>(HolsterId::kMag);
interact::HolsterSpot g_spots[static_cast<int>(HolsterId::kCount)];
bool g_spot_moved[static_cast<int>(HolsterId::kCount)] = {};  // placed in the headset (holsters.ini)

std::wstring HolsterFile() { return config::Dir() + L"\\holsters.ini"; }

// Where a seated magazine sits in each gun (actor space), learned from a
// magazine in the gun and kept across levels and launches in mag-seats.ini.
// Without it the first reload of a gun aimed at the magwell's top point, so
// the magazine had to be pushed up through the gun before it seated.
struct MagSeat {
    FVector offset;
    FQuat rot;
};
std::map<std::string, MagSeat> g_mag_seats;
std::wstring MagSeatFile() { return config::Dir() + L"\\mag-seats.ini"; }

void LoadMagSeats() {
    FILE* f = _wfopen(MagSeatFile().c_str(), L"r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char name[128] = {};
        double v[7];
        if (line[0] == ';' || std::sscanf(line, "%127[^=]=%lf %lf %lf %lf %lf %lf %lf", name, &v[0], &v[1], &v[2],
                                          &v[3], &v[4], &v[5], &v[6]) != 8)
            continue;
        MagSeat m;
        m.offset = FVector{v[0], v[1], v[2]};
        m.rot.X = v[3]; m.rot.Y = v[4]; m.rot.Z = v[5]; m.rot.W = v[6];
        if (std::sqrt(m.offset.X * m.offset.X + m.offset.Y * m.offset.Y + m.offset.Z * m.offset.Z) > 15.0) continue;
        g_mag_seats[name] = m;
        log::Info("controls: magazine seat for %s from mag-seats.ini (%.1f %.1f %.1f)", name, v[0], v[1], v[2]);
    }
    fclose(f);
}

void SaveMagSeats() {
    FILE* f = _wfopen(MagSeatFile().c_str(), L"w");
    if (!f) return;
    std::fputs("; Where a seated magazine sits in each gun, learned in game. Delete to relearn.\n", f);
    for (const auto& [name, m] : g_mag_seats)
        std::fprintf(f, "%s=%.3f %.3f %.3f %.6f %.6f %.6f %.6f\n", name.c_str(), m.offset.X, m.offset.Y, m.offset.Z,
                     m.rot.X, m.rot.Y, m.rot.Z, m.rot.W);
    fclose(f);
}

void LoadHolsterSpots() {
    for (int i = 0; i < static_cast<int>(HolsterId::kCount); ++i) {
        const HolsterId id = static_cast<HolsterId>(i);
        interact::HolsterSpot sp = interact::DefaultSpot(id);
        const std::string k = std::string("holster_") + interact::HolsterName(id);
        sp.offset.X = config::GetFloat((k + "_forward_cm").c_str(), static_cast<float>(sp.offset.X));
        sp.offset.Y = config::GetFloat((k + "_right_cm").c_str(), static_cast<float>(sp.offset.Y));
        sp.offset.Z = config::GetFloat((k + "_up_cm").c_str(), static_cast<float>(sp.offset.Z));
        sp.radius = config::GetFloat((k + "_radius_cm").c_str(), static_cast<float>(sp.radius));
        g_spots[i] = sp;
    }
    if (FILE* f = _wfopen(HolsterFile().c_str(), L"r")) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            for (int i = 0; i < static_cast<int>(HolsterId::kCount); ++i) {
                if (!interact::ParseSpotLine(line, static_cast<HolsterId>(i), &g_spots[i])) continue;
                g_spot_moved[i] = true;
                const interact::HolsterSpot& sp = g_spots[i];
                log::Info("holster: %s spot from holsters.ini: on %s, (%.1f %.1f %.1f) cm, %s",
                          interact::HolsterName(static_cast<HolsterId>(i)), interact::AnchorPlace(sp.anchor),
                          sp.offset.X, sp.offset.Y, sp.offset.Z,
                          sp.turned ? "at the angle it was placed" : "hanging the built-in way");
            }
        }
        fclose(f);
    }
}

bool SaveHolsterSpots() {
    FILE* f = _wfopen(HolsterFile().c_str(), L"w");
    if (!f) return false;
    std::fputs("; Holsters placed in the headset: the body part each rides on (belt, chest,\n"
               "; left_thigh, right_thigh), where the hand grabs in cm (forward, right, up in\n"
               "; that part's frame), and how the item is turned there.\n"
               "; Delete a line (or this file) to go back to the holster_* values in hole-vr.ini.\n", f);
    for (int i = 0; i < static_cast<int>(HolsterId::kCount); ++i) {
        if (!g_spot_moved[i]) continue;
        std::fprintf(f, "%s\n", interact::FormatSpotLine(static_cast<HolsterId>(i), g_spots[i]).c_str());
    }
    fclose(f);
    return true;
}

// Where the holsters are this frame, in the world. Worked out by the late
// tick (after the VR body is posed), so the controls tick sees last frame's.
constexpr int kAnchors = static_cast<int>(interact::HolsterAnchor::kCount);
static_assert(kAnchors == static_cast<int>(body::Anchor::kCount), "body anchors match holster anchors");
struct HolsterWorld {
    bool valid = false;
    bool on_body = false;   // from the VR body; false: from the headset (body off)
    FVector belt;
    FQuat body_rot;         // yaw only
    hands::Pose anchor[kAnchors];  // belt, chest, left thigh, right thigh (interact::HolsterAnchor)
    FVector spot[2];        // pistol, magazine: where the hand grabs
    FQuat spot_anchor_rot[2];
    int logged_mode = -1;
} g_hw;

const hands::Pose& SpotAnchor(int spot) { return g_hw.anchor[static_cast<int>(g_spots[spot].anchor)]; }

// ---- shared hand state for other systems ----
std::mutex g_hands_mutex;
hands::HandState g_hands[2];
hands::HandGrab g_grab[2];
hands::FingerInput g_fingers[2];
hands::Pose g_head;
bool g_head_valid = false;
hands::EditButtons g_edit_buttons;
std::atomic<int> g_editor_hand{-1};
double g_clock = 0.0;
double g_dt = 0.0;

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
        g_edit_buttons.grip_down[i] = in.grip_value > 0.6f;
    }
    g_edit_buttons.y_down = s.hand[input::kLeft].secondary.down;
    g_edit_buttons.left_stick_pressed = s.hand[input::kLeft].stick_click.pressed;
    g_edit_buttons.left_stick_down = s.hand[input::kLeft].stick_click.down;
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
    // Teleport mode: the stick aims an arc instead of walking.
    static bool s_teleport_on = false;
    const bool teleport_now = g_set.teleport && !ui_panel::MenuOpen();
    if (teleport_now) teleport::Update(player, left, g_set.swap_sticks ? input::kRight : input::kLeft);
    else if (s_teleport_on) teleport::Hide();
    s_teleport_on = teleport_now;
    if (!g_set.teleport && mag > g_set.deadzone) {
        double scale = std::fmin(1.0, (mag - g_set.deadzone) / (1.0 - g_set.deadzone)) / mag;
        double yaw = 0;
        hands::HandState lh = hands::Get(g_set.swap_sticks ? hands::kRight : hands::kLeft);
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
    // Comfort vignette: how fast the character really moves (walls stop the
    // stick, falls count) and how fast the stick turns you. Snap turns are
    // instant, so they have nothing to darken.
    {
        ue::Call v(player, "GetVelocity");
        FVector vel = v.Invoke() ? v.Result<FVector>() : FVector{};
        double horiz = std::sqrt(vel.X * vel.X + vel.Y * vel.Y);
        double move01 = std::fmax(horiz / 300.0, std::fabs(vel.Z) / 600.0);
        double turn01 = g_set.smooth_turn && dt > 0 ? std::fabs(turn) / dt / 90.0 : 0.0;
        comfort::SetMotion(move01, turn01);
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

// Room-scale walking: when you step around the room, move the character
// (its collision capsule) with you instead of leaving it where the stick put
// it. The move is swept, so walls stop the character; whatever it actually
// moved is taken off the headset's offset, so the view never jumps.
void RoomScaleFollow(UObject player) {
    if (!g_set.room_scale || !player) return;
    FVector off;
    if (!stereo::HeadOffsetWorld(&off)) return;
    off.Z = 0;
    double len = std::sqrt(off.X * off.X + off.Y * off.Y);
    if (len < 2.0) return;                       // ignore small head sway
    if (len > 50.0) off = off * (50.0 / len);    // at most 50 cm per frame
    FVector before = ActorLocation(player);
    ue::Call c(player, "K2_SetActorLocation");
    c.Set("NewLocation", before + off).Set("bSweep", true).Set("bTeleport", false);
    if (!c.Invoke()) return;
    FVector moved = ActorLocation(player) - before;
    moved.Z = 0;
    if (moved.X * moved.X + moved.Y * moved.Y > 1e-4) stereo::ShiftOriginWorld(moved);
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
    double mag_wait_until = -1.0;  // when to stop waiting for the game to give the gun a magazine
    double refill_pouch_at = 0; // when to put a new magazine in the pouch
    bool trigger_down = false;
    bool have_frame = false;
    FQuat gun_frame;           // gun frame (X barrel, Z up) expressed in actor space
    double check_chamber_at = -1;
    double pending_release_at = -1;
    bool retried_rack = false;
    bool dead_trigger = false;  // Glock: after a dry fire the trigger is dead until the slide moves
    bool trigger_sent = false;  // the game saw the current trigger press
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
    FQuat mag_shown_rot;          // and how it is turned
    bool seat_learned = false;
    FVector seat_offset;          // inserted mag origin minus MagazinePoint, actor space
    FQuat seat_rot;               // inserted mag rotation, actor space
    UObject learn_mag = 0;
    double learn_at = -1;
    double trace_until = -1;      // per-frame rack trace (diagnosing the chamber hitch)
    int last_rounds = -1;         // chamber + magazine last frame, to detect shots
    // Per-gun kind. Pistols (BP_HG_*) have a slide; rifles (BP_AR_*: AR-15
    // pattern) have a bolt carrier worked by a T charging handle at the back
    // of the upper receiver, a bolt catch on the left, a vertical magwell
    // and a handguard for the support hand.
    bool is_pistol = true;
    double muzzle_dist = 18.0;    // grip to muzzle along the bore, cm
    FVector slide_axis{-1, 0, 0}; // bolt/slide pull direction in its parent's space
    bool two_hand = false;        // support hand on the handguard
    FVector support_local;        // support hand point in the gun frame, from the grip
    FVector grip_world;           // gun-frame origin (the firing hand's grip point)
    bool handle_moved = false;    // charging handle moved off its rest
    double bore_height = 6.0;     // bore above the grip point, cm (gun frame Z)
    bool have_handle_grab = false;
    FVector handle_grab;          // charging handle at rest, in the bolt parent's space
    bool have_handle_origin = false;
    FVector handle_origin;        // charging handle relative location at rest
    bool have_support_point = false;
    FVector support_point;        // where the support hand holds the handguard, world
    bool last_has_mag = false;
    double kick_t0 = -1;          // time of the last shot, for the in-hand kick
    double kick_side = 0;         // this shot's random sideways part, -1..1
    // Virtual stock (rifles): gun-frame points, from the grip.
    FVector stock_local;          // middle of the butt pad
    FVector ads_local;            // the game's aiming eye point (DefaultADSPoint)
    bool edit_frozen = false;     // grip editor running: gun held still
    FVector edit_gun_loc;
    FQuat edit_gun_rot;
    bool have_stock = false;
    bool shouldered = false;
    double shoulder_blend = 0;    // 0 hands only .. 1 pivoting from the shoulder
    FVector gun_set_loc;          // where HoldGun put the gun this frame
    // Holster: the pistol lives on the right hip and the right grip holds it.
    // Rifles have no holster yet and stay in the hand.
    bool uses_holster = false;
    bool in_hand = true;          // false: in the holster or flying back to it
    interact::HolsterDraw holster;
    bool have_release = false;    // gun pose when it left the hand, for the fly-back
    FVector release_loc;
    FQuat release_rot;
    interact::HolsterZone mag_zone;  // left hand at the magazine pouch (haptic tick)
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

FQuat Nlerp(const FQuat& a, FQuat b, double t) {
    if (a.X * b.X + a.Y * b.Y + a.Z * b.Z + a.W * b.W < 0) { b.X = -b.X; b.Y = -b.Y; b.Z = -b.Z; b.W = -b.W; }
    FQuat q;
    q.X = a.X + (b.X - a.X) * t; q.Y = a.Y + (b.Y - a.Y) * t;
    q.Z = a.Z + (b.Z - a.Z) * t; q.W = a.W + (b.W - a.W) * t;
    double n = std::sqrt(q.X * q.X + q.Y * q.Y + q.Z * q.Z + q.W * q.W);
    q.X /= n; q.Y /= n; q.Z /= n; q.W /= n;
    return q;
}

void MeasureStock(UObject g, const FVector& actor_loc, const FQuat& actor_rot, const FVector& grip, UObject sight);

// Shortest rotation taking direction a onto direction b (both unit length).
FQuat FromTo(const FVector& a, const FVector& b) {
    FVector c = Cross(a, b);
    double d = DotV(a, b);
    FQuat q;
    if (d < -0.9999) {
        // Opposite: half turn about any axis at right angles to a.
        FVector axis = Norm(Cross(a, std::fabs(a.X) < 0.9 ? FVector{1, 0, 0} : FVector{0, 1, 0}));
        q.X = axis.X; q.Y = axis.Y; q.Z = axis.Z; q.W = 0;
        return q;
    }
    q.X = c.X; q.Y = c.Y; q.Z = c.Z; q.W = 1.0 + d;
    double n = std::sqrt(q.X * q.X + q.Y * q.Y + q.Z * q.Z + q.W * q.W);
    q.X /= n; q.Y /= n; q.Z /= n; q.W /= n;
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
    // The grip sits well below the bore, so grip->muzzle tilts up (37 deg on
    // the pistol: its slide was then rejected below and the whole gun, and
    // the hand around it, came out pitched). The sight sits just above the
    // bore at the back, so sight->muzzle runs nearly parallel to it.
    if (sight) {
        FVector s = inv.Rotate(ComponentToWorld(sight).translation - actor_loc);
        if (DotV(m - s, fwd) > 5.0) fwd = Norm(m - s);
    }
    // The slide runs exactly along the bore, so prefer it when there is one:
    // it moves along -X of its parent component (measured in the weapon lab).
    game::Gun gun(g);
    UObject slide_parent = 0;
    if (UObject sc = gun.SlideComponent()) ue::GetProp(sc, "AttachParent", slide_parent);
    if (slide_parent) {
        FVector bore = inv.Rotate(ComponentToWorld(slide_parent).rotation.Rotate(FVector{1, 0, 0}));
        if (DotV(bore, fwd) < 0) bore = bore * -1.0;  // keep it pointing toward the muzzle
        // Only trust it when it agrees with the muzzle direction (a rifle's
        // bolt parent may be oriented differently).
        if (DotV(Norm(bore), fwd) > 0.9) fwd = Norm(bore);
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
    W.muzzle_dist = std::fmax(10.0, DotV(m - grip, fwd));
    W.bore_height = DotV(m - grip, up);
    MeasureStock(g, actor_loc, actor_rot, grip, sight);
    // The slide/bolt is pulled straight back along the bore. Express that in
    // the slide's parent space (the pistol's came out -X, as measured in the
    // weapon lab); a rifle's bolt parent may be turned differently.
    if (slide_parent) {
        FVector back_w = actor_rot.Rotate(fwd * -1.0);
        FVector back_p = Norm(ComponentToWorld(slide_parent).rotation.Inverse().Rotate(back_w));
        W.slide_axis = snap(back_p);
        log::Info("controls: slide/bolt pulls along (%.2f %.2f %.2f) in its parent's space", W.slide_axis.X,
                  W.slide_axis.Y, W.slide_axis.Z);
    }
    log::Info("controls: %s barrel along (%.2f %.2f %.2f), top along (%.2f %.2f %.2f) in actor space; muzzle %.1f cm "
              "ahead of and %.1f cm above the grip point",
              ue::GetName(ue::GetClass(g)).c_str(), fwd.X, fwd.Y, fwd.Z, up.X, up.Y, up.Z, W.muzzle_dist, W.bore_height);
    return true;
}


// Keeps the gun out of walls, like Half-Life: Alyx: a trace from the head to
// the muzzle; when it hits something the whole gun is pulled back along that
// line so the muzzle rests at the surface (the hand on the gun follows it).
// Returns the offset to add to the gun's location.
struct TArrayRaw {
    const UObject* data;
    int32_t num, max;
};
FVector WallPushback(UObject gun, const FQuat& frame, const FVector& grip) {
    if (!g_set.wall_collision) return FVector{};
    // Looked up once: FindObject is a full object scan, too slow per frame.
    static UObject ksl = 0, gps = 0;
    static int lookups = 0;
    if ((!ksl || !gps) && lookups < 3) {
        ++lookups;
        if (!ksl) ksl = ue::FindObject("KismetSystemLibrary /Script/Engine.Default__KismetSystemLibrary");
        if (!gps) gps = ue::FindObject("GameplayStatics /Script/Engine.Default__GameplayStatics");
        if (!ksl || !gps) log::Warn("controls: wall check unavailable (trace library not found)");
    }
    hands::Pose head;
    UObject player = game::Player();
    if (!ksl || !gps || !player || !hands::Head(&head)) return FVector{};
    FVector muzzle = grip + frame.Rotate(FVector{W.muzzle_dist, 0, 0});
    UObject ignore[3] = {gun, W.held_mag, W.pouch_mag};
    int32_t n = 1 + (W.held_mag ? 1 : 0);
    if (W.pouch_mag) ignore[n++] = W.pouch_mag;
    TArrayRaw arr{ignore, n, n};
    ue::Call t(ksl, "LineTraceSingle");
    t.Set("WorldContextObject", player).Set("Start", head.location).Set("End", muzzle)
        .Set("TraceChannel", uint8_t{0}).Set("bTraceComplex", false).Set("ActorsToIgnore", arr)
        .Set("bIgnoreSelf", true);
    if (!t.Invoke() || !t.Result<bool>()) return FVector{};
    int32_t hit_size = 0;
    uint8_t* hit = t.Raw("OutHit", &hit_size);
    if (!hit) return FVector{};
    ue::Call b(gps, "BreakHitResult");
    int32_t in_size = 0;
    uint8_t* in = b.Raw("Hit", &in_size);
    if (!in || in_size != hit_size) return FVector{};
    std::memcpy(in, hit, static_cast<size_t>(hit_size));
    if (!b.Invoke() || !b.Get<bool>("bBlockingHit")) return FVector{};
    FVector at = b.Get<FVector>("Location");
    FVector dir = Norm(muzzle - head.location);
    // Muzzle rests 1 cm short of the surface.
    double back = DotV(muzzle - at, dir) + 1.0;
    if (back <= 0) return FVector{};
    return dir * -back;
}

// Rifle stock: finds the back end of the gun (the butt pad) from the
// bounds of all its parts, in the gun frame (X barrel, Z up, from the grip),
// and the game's aiming eye point. Falls back to AR-15 sizes.
void MeasureStock(UObject g, const FVector& actor_loc, const FQuat& actor_rot, const FVector& grip, UObject sight) {
    if (W.is_pistol) return;
    const FQuat to_frame = W.gun_frame.Inverse();   // actor space -> gun frame
    const FQuat inv = actor_rot.Inverse();
    double min_x = 0;
    int counted = 0;
    static UObject prim_class = 0;
    if (!prim_class) prim_class = ue::FindObject("Class /Script/Engine.PrimitiveComponent");
    ue::Call list(g, "K2_GetComponentsByClass");
    list.Set("ComponentClass", prim_class);
    int32_t size = 0;
    const uint8_t* raw = prim_class && list.Invoke() ? list.Raw("ReturnValue", &size) : nullptr;
    struct Arr { uintptr_t data; int32_t num, max; } arr{};
    if (raw && size >= (int32_t)sizeof(Arr)) std::memcpy(&arr, raw, sizeof(Arr));
    for (int i = 0; i < arr.num && i < 128; ++i) {
        UObject c = mem::ReadOr<uintptr_t>(arr.data + i * sizeof(uintptr_t), 0);
        if (!c) continue;
        ue::Call b(c, "GetLocalBounds");
        if (!b.Invoke()) continue;
        ue::Vec3d mn = b.Get<ue::Vec3d>("Min"), mx = b.Get<ue::Vec3d>("Max");
        if (mx.x - mn.x < 0.5 && mx.y - mn.y < 0.5 && mx.z - mn.z < 0.5) continue;  // empty
        UETransform t = ComponentToWorld(c);
        for (int k = 0; k < 8; ++k) {
            FVector corner{k & 1 ? mx.x : mn.x, k & 2 ? mx.y : mn.y, k & 4 ? mx.z : mn.z};
            corner = FVector{corner.X * t.scale.X, corner.Y * t.scale.Y, corner.Z * t.scale.Z};
            FVector w = t.translation + t.rotation.Rotate(corner);
            FVector f = to_frame.Rotate(inv.Rotate(w - actor_loc) - grip);
            if (f.X < min_x) min_x = f.X;
        }
        ++counted;
    }
    // AR-15 / M4: butt about 28 cm behind the pistol grip, pad centre a
    // little below the bore line.
    double back = -min_x;
    const bool measured = counted > 0 && back > 12.0 && back < 50.0;
    if (!measured) back = 28.0;
    W.stock_local = FVector{-back, 0, W.bore_height - 3.0};
    W.ads_local = FVector{0, 0, W.bore_height + 6.5};   // red dot height over the bore
    if (sight) {
        FVector a = to_frame.Rotate(inv.Rotate(ComponentToWorld(sight).translation - actor_loc) - grip);
        if (a.Z > W.bore_height && a.Z < W.bore_height + 15.0 && a.X > -back) W.ads_local = a;
    }
    W.have_stock = true;
    log::Info("controls: rifle stock %s at %.1f cm behind the grip (%d parts), aim point (%.1f %.1f %.1f)",
              measured ? "measured" : "assumed", back, counted, W.ads_local.X, W.ads_local.Y, W.ads_local.Z);
}

// Where the butt goes when the aiming eye looks down the sights: the
// shoulder pocket, worked out from the headset and the gun's own geometry.
bool ShoulderPocket(FVector* out) {
    hands::Pose head;
    if (!hands::Head(&head)) return false;
    FVector eye = head.location + head.rotation.Rotate(FVector{0, g_set.right_eye ? 3.2 : -3.2, 0});
    // Eye at the sight line, butt behind and below it along the bore. Keep
    // the eye-to-butt distance in a human range (12-30 cm).
    FVector d = W.stock_local - W.ads_local;
    d.X = std::fmax(-30.0, std::fmin(-12.0, d.X));
    d = d + FVector{g_set.stock_forward, g_set.stock_right, g_set.stock_up};
    *out = eye + head.rotation.Rotate(d);
    return true;
}

// Where the gun's grip point (MainHandPosition) sits from the controller's
// grip, in the gun frame. Fourth headset test: the gun sat about 5 cm too far
// forward and 1.3 cm too far left; fifth test: about 5 cm too high.
comfort::AimSmoother g_aim_smoother;

FVector GunGripOffset() {
    constexpr double kBaseForward = -5.0, kBaseRight = 1.3, kBaseUp = -5.0;
    return FVector{kBaseForward + g_set.gun_forward, kBaseRight + g_set.gun_right, kBaseUp};
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
    // `hand` is the gun's own frame in the world (X barrel, Y right, Z up).
    FQuat hand = rh.aim.rotation * ToQuat(FRotator{kBasePitch + g_set.gun_pitch, kBaseYaw + g_set.gun_yaw,
                                                   g_set.gun_roll});
    const FVector offset = GunGripOffset();
    FVector grip_w = rh.grip.location + hand.Rotate(offset);
    // Two hands: the support hand on the handguard steers the barrel. Turn
    // the one-hand pose so the point the support hand grabbed lies toward
    // where that hand is now; roll still comes from the firing hand.
    hands::HandState lh2 = hands::Get(hands::kLeft);
    const bool two = W.two_hand && lh2.valid && Len(W.support_local) > 5.0;
    if (two) {
        FVector v1 = Norm(hand.Rotate(W.support_local));
        FVector v2 = Norm(lh2.grip.location - grip_w);
        hand = FromTo(v1, v2) * hand;
        grip_w = rh.grip.location + hand.Rotate(offset);
    }
    // Virtual stock (rifles, like Into the Radius and Zero Caliber): when
    // the butt comes within a hand's width of the shoulder pocket it locks
    // there and the rifle pivots from the shoulder, aiming from it through
    // the support hand (or the firing hand when one-handed). The hands'
    // small wobbles no longer move the back of the gun. Pull it well away
    // to let go.
    FVector pocket;
    if (!W.is_pistol && W.have_stock && g_set.virtual_stock && ShoulderPocket(&pocket)) {
        double d = Len(grip_w + hand.Rotate(W.stock_local) - pocket);
        if (!W.shouldered && d < g_set.stock_snap) {
            W.shouldered = true;
            input::Haptic(input::kRight, 0.2f, 0.03f);
            log::Info("controls: rifle shouldered (%.1f cm from the pocket)", d);
        } else if (W.shouldered && d > g_set.stock_release) {
            W.shouldered = false;
            log::Info("controls: rifle off the shoulder");
        }
        W.shoulder_blend += (W.shouldered ? 1.0 : -1.0) * g_dt / 0.12;
        W.shoulder_blend = std::fmax(0.0, std::fmin(1.0, W.shoulder_blend));
        if (W.shoulder_blend > 0) {
            FVector front_local = two ? W.support_local : hand.Inverse().Rotate(rh.grip.location - grip_w);
            FVector front_w = two ? lh2.grip.location : rh.grip.location;
            FVector gun_dir = Norm(hand.Rotate(front_local - W.stock_local));
            FVector want = front_w - pocket;
            if (Len(want) > 5.0) {
                FQuat hs = FromTo(gun_dir, Norm(want)) * hand;
                FVector gs = pocket - hs.Rotate(W.stock_local);
                hand = Nlerp(hand, hs, W.shoulder_blend);
                grip_w = grip_w + (gs - grip_w) * W.shoulder_blend;
            }
        }
    } else {
        W.shouldered = false;
        W.shoulder_blend = 0;
    }
    // Aim smoothing (VR tab): slow shakes of the hand are smoothed out, fast
    // moves pass straight through. Turns about the grip.
    hand = g_aim_smoother.Step(hand, g_dt, g_set.aim_smoothing);
    // Recoil kick, at the grip so the arms follow it, never the camera:
    // the muzzle flips up (with a little random side) and the gun pushes
    // back, then returns (about 20 ms up, 100 ms back). A pistol in one
    // hand flips hard; a rifle less with two hands and least on the shoulder,
    // where it pivots from the butt and mostly pushes straight back.
    double kick = 0;
    if (W.kick_t0 >= 0 && g_set.recoil_kick > 0) {
        double t = g_clock - W.kick_t0;
        if (t > 0.4) W.kick_t0 = -1;
        else kick = g_set.recoil_kick * (1.0 - std::exp(-t / 0.012)) * std::exp(-t / 0.07) * 1.6;
    }
    if (kick > 0) {
        double up, back, side;
        if (W.is_pistol) { up = 7.0; back = 2.0; side = 1.0; }
        else if (W.shoulder_blend > 0.5) { up = 1.2; back = 1.5; side = 0.4; }
        else if (two) { up = 2.5; back = 2.5; side = 0.6; }
        else { up = 6.0; back = 3.0; side = 1.5; }
        FQuat kicked = hand * ToQuat(FRotator{kick * up, kick * side * W.kick_side, 0});
        FVector pivot = W.shoulder_blend > 0.5 ? grip_w + hand.Rotate(W.stock_local) : grip_w;
        FQuat delta = kicked * hand.Inverse();
        grip_w = pivot + delta.Rotate(grip_w - pivot) - kicked.Rotate(FVector{kick * back, 0, 0});
        hand = kicked;
    }
    FQuat rot = W.have_frame ? hand * W.gun_frame.Inverse() : hand;
    FVector loc = grip_w - rot.Rotate(rel);
    FVector push = WallPushback(g, hand, grip_w);
    loc = loc + push;
    W.grip_world = grip_w + push;
    W.have_support_point = two;
    if (two) W.support_point = W.grip_world + hand.Rotate(W.support_local);
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

// AR-15 charging handle: it moves back with the hand and pulls the bolt
// carrier with it, but the bolt does not pull it: when the bolt locks back
// on an empty magazine, or the gun fires, the handle stays forward. Let go,
// it rides forward with the bolt, or springs forward alone onto a locked bolt.
void MoveChargingHandle(game::Gun& gun, bool bolt_locked) {
    if (W.is_pistol) return;
    UObject handle = 0;
    ue::GetProp(gun.Object(), "GunParts_ChargingHandle", handle);
    if (!handle || !W.have_pose) return;
    if (!W.have_handle_origin) {
        ue::Vec3d o;
        ue::GetProp(gun.Object(), "ChargingHandle_OriginLoc", o);
        if (o.x == 0 && o.y == 0 && o.z == 0) ue::GetProp(handle, "RelativeLocation", o);
        W.handle_origin = FVector{o.x, o.y, o.z};
        W.have_handle_origin = true;
    }
    double travel = W.slide.Held() ? W.slide.HandleTravel() : (bolt_locked ? 0.0 : W.slide.Travel());
    if (travel < 0.001 && !W.handle_moved) return;
    UObject parent = 0;
    ue::GetProp(handle, "AttachParent", parent);
    if (!parent) return;
    FVector back_w = W.frame_rot.Rotate(FVector{-1, 0, 0});
    FVector axis = Norm(ComponentToWorld(parent).rotation.Inverse().Rotate(back_w));
    double len = gun.SlideBackLength() > 0.5 ? gun.SlideBackLength() : 8.0;
    FVector p = W.handle_origin + axis * (len * travel);
    ue::Call c(handle, "K2_SetRelativeLocation");
    c.Set("NewLocation", ue::Vec3d{p.X, p.Y, p.Z}).Set("bSweep", false).Set("bTeleport", true);
    c.Invoke();
    W.handle_moved = travel >= 0.001;
}

// in_hand false (pistol holstered): the slide can't be grabbed; one already
// held is let go and springs back as usual.
void SlideHandling(game::Gun& gun, const hands::HandState& lh, const input::HandInput& lin, double dt,
                   bool in_hand) {
    UObject slide_comp = gun.SlideComponent();
    if (!slide_comp || !lh.valid) return;
    UObject parent = 0;
    ue::GetProp(slide_comp, "AttachParent", parent);
    if (!parent) return;
    UETransform pt = ComponentToWorld(parent);
    FVector hand = ToLocal(pt, lh.grip.location);

    interact::SlideConfig cfg;
    cfg.axis = W.slide_axis;  // straight back along the bore (pistol: -X, measured in the weapon lab)
    cfg.length = gun.SlideBackLength() > 0.5 ? gun.SlideBackLength() : (W.is_pistol ? 4.7 : 8.0);
    ue::Vec3d o = gun.SlideOrigin();
    cfg.grab_center = FVector{o.x, o.y, o.z} + cfg.axis * 4.0;
    cfg.grab_radius = 10.0;  // until the slide is measured: the rear of the slide (owner grabs at -11..-13)
    cfg.grab_forward = g_set.slide_grab_forward;   // whole slide, front serrations included
    if (W.is_pistol) {
        // Zones from the slide the body measured (traces on its mesh): the
        // whole slide top from the front end to 2 cm past the rear end, and
        // the pinch at the rear sight. Until measured, the rear sphere and
        // front capsule above.
        FVector sight_w, rear_w, front_w;
        if (body::SlideMarks(gun.Object(), &sight_w, &rear_w, &front_w)) {
            cfg.use_span = true;
            cfg.span_rear = ToLocal(pt, rear_w);
            cfg.span_front = ToLocal(pt, front_w);
            cfg.sight_center = ToLocal(pt, sight_w);
            cfg.sight_radius = g_set.slide_pinch_radius;
            cfg.sight_back = g_set.slide_pinch_back;   // owner: about 1/4 inch further back than the sight
            static UObject logged_gun = 0;
            if (logged_gun != gun.Object()) {
                logged_gun = gun.Object();
                log::Info("controls: slide zones from the measured slide: rear end (%.1f %.1f %.1f), front end "
                          "(%.1f %.1f %.1f), rear sight (%.1f %.1f %.1f) in slide space",
                          cfg.span_rear.X, cfg.span_rear.Y, cfg.span_rear.Z, cfg.span_front.X, cfg.span_front.Y,
                          cfg.span_front.Z, cfg.sight_center.X, cfg.sight_center.Y, cfg.sight_center.Z);
            }
        }
    }
    if (!W.is_pistol) {
        cfg.grab_forward = 0.0;
        // AR-15: the left hand works the T-shaped charging handle at the
        // back of the upper receiver. It always sits forward (it does not
        // stay back with a locked bolt), so it is grabbed at its rest point.
        UObject handle = 0;
        ue::GetProp(gun.Object(), "GunParts_ChargingHandle", handle);
        if (!W.have_handle_grab) {
            UObject point = handle;
            if (!point) ue::GetProp(gun.Object(), "ChargingHandPoint", point);
            if (point) {
                W.handle_grab = ToLocal(pt, ComponentToWorld(point).translation);
                W.have_handle_grab = true;
                log::Info("controls: charging handle grab point (%.1f %.1f %.1f) in the bolt's parent space",
                          W.handle_grab.X, W.handle_grab.Y, W.handle_grab.Z);
            }
        }
        if (W.have_handle_grab) cfg.grab_center = W.handle_grab;
        cfg.grab_radius = 9.0;
        cfg.grab_at_closed = true;
    }
    // Locked open (empty gun): the slide rests on the catch, and pulling it a
    // little further back and letting go releases it, like a real pistol.
    const bool locked = gun.State().slide_locked;
    const double stop = gun.SlideStopLength();
    const double rest = locked && stop > 0 ? std::fmin(0.95, stop / cfg.length) : 0.0;
    if (locked) cfg.full_threshold = std::fmin(1.0, rest + 0.08);
    W.slide.Configure(cfg);
    W.slide.SetRest(rest);

    if (lin.grip_button.pressed && W.held_mag == 0 && in_hand)
        log::Info("controls: left grab at slide-space (%.1f %.1f %.1f), %s the slide zone", hand.X, hand.Y,
                  hand.Z, W.slide.InZone(hand) ? "inside" : "outside");

    using interact::SlideEvent;
    SlideEvent e = W.slide.Update(hand, lin.grip_button.down && W.held_mag == 0 && in_hand, dt);
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
    const double grab_travel = W.is_pistol ? W.slide.Travel() : W.slide.HandleTravel();
    W.slide_point = pt.translation + pt.rotation.Rotate(cfg.grab_center + cfg.axis * (grab_travel * cfg.length));
    W.have_slide_point = true;
    MoveChargingHandle(gun, locked);
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

bool LiveObject(UObject obj);

// Magazine class per gun class, kept across levels so a new match doesn't
// have to wait for (or take) the game's own magazine.
std::map<std::string, UObject> g_mag_class_by_gun;

void RefillPouch(game::Gun& gun, UObject player, const FVector& loc, const FQuat& rot) {
    if (!W.mag_class) {
        UObject m = 0;
        if (ue::GetProp(gun.Object(), "MagazineObj", m) && m) W.mag_class = ue::GetClass(m);
        else if (ue::GetProp(gun.Object(), "MagazineObj_Temp", m) && m) W.mag_class = ue::GetClass(m);
        if (W.mag_class) {
            g_mag_class_by_gun[gun.ClassName()] = W.mag_class;
        } else {
            auto known = g_mag_class_by_gun.find(gun.ClassName());
            if (known != g_mag_class_by_gun.end() && LiveObject(known->second)) W.mag_class = known->second;
        }
    }
    if (W.mag_wait_until < 0.0) W.mag_wait_until = g_clock + 5.0;
    UObject mag = 0;
    if (W.mag_class) {
        mag = SpawnMagazine(player, W.mag_class, loc, rot);
        if (mag) log::Info("controls: spawned pouch magazine %s", ue::GetName(mag).c_str());
    } else if (!gun.State().has_magazine && g_clock >= W.mag_wait_until) {
        // The game normally gives the gun its magazine just after the level
        // starts; only if it never does, borrow the game's own step. Taking
        // it earlier races the game's own load-in, and the game later moves
        // or destroys that magazine (pouch looks empty for seconds).
        gun.TakeNewMagazine();
        ue::GetProp(gun.Object(), "MagazineObj_Temp", mag);
        if (mag) W.mag_class = ue::GetClass(mag);
        if (mag) log::Info("controls: took pouch magazine from the game (%s)", ue::GetName(mag).c_str());
    }
    if (mag) {
        W.pouch_mag = mag;
    } else {
        W.refill_pouch_at = g_clock + 0.25;   // try again shortly
    }
}

// Magwell rake: a Glock's runs up the grip, raked about 18 degrees; an
// AR-15's magwell is square to the bore.
double MagwellRake() { return W.is_pistol ? 18.0 : 0.0; }

// How a seated magazine sits in the gun frame (X barrel, Z up), from the
// first insert (learned) or the magwell point. Doesn't depend on where the
// gun is, so it works with the gun in the holster too.
bool MagInGunFrame(UObject g, FQuat* out) {
    if (!W.have_frame) return false;
    if (W.seat_learned) {
        *out = W.gun_frame.Inverse() * W.seat_rot;
        return true;
    }
    UObject well = 0;
    ue::GetProp(g, "MagazinePoint", well);
    if (!well) return false;
    *out = W.gun_frame.Inverse() * (ActorRotation(g).Inverse() * ComponentToWorld(well).rotation);
    return true;
}

// The spare magazine in its pouch: upright, rounds facing forward, as it
// would sit in a gun held level and facing the way the body faces.
FQuat MagPouchRotation(UObject g) {
    FQuat m;
    const bool known = g && MagInGunFrame(g, &m);
    const int source = !known ? 0 : W.seat_learned ? 2 : 1;
    static int logged = -1;
    if (source != logged) {
        logged = source;
        log::Info("holster: pouch magazine turned %s",
                  source == 2   ? "as it sat in the gun after the first insert"
                  : source == 1 ? "from the magwell point (until the first insert)"
                                : "with the body (gun frame not known yet)");
    }
    if (known) return interact::MagPouchGunFrame(g_hw.body_rot, MagwellRake()) * m;
    return g_hw.body_rot;
}

// How the pouch magazine sits: as it was held when the pouch was placed in
// the headset, otherwise upright (MagPouchRotation).
FQuat PouchRotation(UObject g) {
    if (g_spots[kMagSpot].turned) return g_hw.spot_anchor_rot[1] * g_spots[kMagSpot].rot;
    return MagPouchRotation(g);
}

void MagazineHandling(game::Gun& gun, UObject player, const hands::HandState& lh, const input::HandInput& lin,
                      const input::HandInput& rin) {
    // Right A / primary: magazine release.
    // Right B: the slide stop lever. Pressed down on a locked slide it
    // releases it; with the slide held back by the left hand it pushes the
    // lever up so the slide stays open; otherwise it does nothing.
    // B held at any moment while the slide is back far enough pushes the
    // lever up (pressed before or during the rack, released before the slide).
    // None of these work on a holstered pistol.
    if (rin.secondary.down && W.slide.Held() && !W.lock_on_release && W.in_hand) {
        double len = gun.SlideBackLength() > 0.5 ? gun.SlideBackLength() : 4.7;
        double stop = gun.SlideStopLength() > 0 ? gun.SlideStopLength() / len : 0.75;
        if (W.slide.Travel() >= stop - 0.05) {
            W.lock_on_release = true;
            input::Haptic(input::kRight, 0.3f, 0.03f);
            log::Info("controls: slide stop pushed up, slide will stay open");
        }
    }
    if (rin.secondary.pressed && !W.slide.Held() && W.in_hand) {
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
    if (rin.primary.pressed && W.in_hand && gun.State().has_magazine) {
        log::Info("controls: mag release, before: %s", gun.State().Describe().c_str());
        gun.DropMagazine();
        input::Haptic(input::kRight, 0.4f, 0.04f);
    }
    if (!lh.valid) return;

    // A spare magazine always sits in its holster (the pouch), on the left
    // hip of the VR body unless placed somewhere else. The late tick places
    // it there each frame (PlacePouchMag); here the left hand takes it out
    // or puts it back.
    const bool have_pouch = g_hw.valid;
    const FVector pouch_loc = g_hw.spot[kMagSpot];
    const double pouch_radius = g_spots[kMagSpot].radius;
    if (!W.pouch_mag && !W.held_mag && g_clock >= W.refill_pouch_at && have_pouch)
        RefillPouch(gun, player, pouch_loc, PouchRotation(gun.Object()));
    const double pouch_dist = Len(lh.grip.location - pouch_loc);
    if (W.mag_zone.Update(pouch_dist, pouch_radius, have_pouch && W.pouch_mag && !W.held_mag && !W.slide.Held()))
        input::Haptic(input::kLeft, 0.15f, 0.015f);  // hand at the pouch

    // Left grab at the pouch: take that magazine, whether or not the gun has one.
    if (lin.grip_button.pressed && W.pouch_mag && !W.held_mag && !W.slide.Held() && have_pouch &&
        pouch_dist <= pouch_radius) {
        W.held_mag = W.pouch_mag;
        W.pouch_mag = 0;
        input::Haptic(input::kLeft, 0.4f, 0.04f);
        log::Info("controls: took the pouch magazine (hand %.1f cm from the pouch)", pouch_dist);
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
    // Into the magwell, in the gun frame (MagwellRake).
    const double kRake = MagwellRake() * 3.14159265358979 / 180.0;
    const FVector in_g{std::sin(kRake), 0, std::cos(kRake)};
    double aim_off_deg = 0;
    FQuat seat_rot = show_rot;
    FQuat mag_in_gun;
    bool have_mag_in_gun = well && MagInGunFrame(gun.Object(), &mag_in_gun);
    if (!have_mag_in_gun && well && W.have_pose && W.in_hand) {
        // Gun frame not learned: from the live gun pose instead.
        const FQuat sr = W.seat_learned ? W.actor_rot * W.seat_rot : ComponentToWorld(well).rotation;
        mag_in_gun = W.frame_rot.Inverse() * sr;
        have_mag_in_gun = true;
    }
    if (have_mag_in_gun) {
        // A fixed hold: the mag's top points where the controller points
        // (index finger up the front of the mag), the gun's front side faces
        // away from the back of the hand. So lining the mag up with the
        // magwell is "point your left controller up into the grip".
        FVector b_g = Norm(FVector{1, 0, 0} - in_g * DotV(FVector{1, 0, 0}, in_g));
        FQuat gun_basis = FromAxes(in_g, b_g, Cross(in_g, b_g));
        FVector hx = lh.aim.rotation.Rotate(FVector{1, 0, 0});
        FVector hz = lh.aim.rotation.Rotate(FVector{0, 0, 1});
        FVector f2 = Norm(hz * -1.0 - hx * DotV(hz * -1.0, hx));
        FQuat hand_basis = FromAxes(hx, f2, Cross(hx, f2));
        FQuat virtual_gun = hand_basis * gun_basis.Inverse();
        show_rot = virtual_gun * mag_in_gun;
        if (W.have_pose && W.in_hand) {
            // Where the mag sits when seated (learned from the first insert;
            // until then the magwell point's own rotation).
            seat_rot = W.seat_learned ? W.actor_rot * W.seat_rot : ComponentToWorld(well).rotation;
            FVector in_w = W.frame_rot.Rotate(in_g);
            aim_off_deg = std::acos(std::fmax(-1.0, std::fmin(1.0, DotV(hx, in_w)))) * 180.0 / 3.14159265358979;
        }
    }
    // Only into a gun in the hand, never one in the holster.
    const bool can_seat = well && W.have_pose && W.in_hand && !gun.State().has_magazine;
    if (can_seat) {
        FVector seat = ComponentToWorld(well).translation;
        if (W.seat_learned) seat = seat + W.actor_rot.Rotate(W.seat_offset);
        FVector out = W.frame_rot.Rotate(in_g) * -1.0;
        FVector p = lh.grip.location - seat;
        double along = DotV(p, out);
        double lateral = Len(p - out * along);

        if (W.mag_seating) {
            double t = std::fmin(1.0, (g_clock - W.seat_t0) / 0.1);
            along = W.seat_from * (1.0 - t);
            show_loc = seat + out * along;
            show_rot = seat_rot;
            SetActorTransform(W.held_mag, show_loc, show_rot);
            W.mag_shown = show_loc;
            W.mag_shown_rot = show_rot;
            if (t < 1.0) return;
            W.mag_seating = false;
            W.mag_engaged = false;
            // Bullet holes stopped after the first reload with a pouch magazine
            // spawned by the mod: those are not set up the way the game's own
            // magazines are. With the magwell empty, ask the game for a proper
            // magazine (its own reload step), seat that, and remove ours.
            UObject game_mag = 0;
            if (!gun.State().has_magazine) {
                ue::SetProp<UObject>(gun.Object(), "MagazineObj_Temp", UObject(0));
                gun.TakeNewMagazine();
                ue::GetProp(gun.Object(), "MagazineObj_Temp", game_mag);
                if (game_mag == W.held_mag) game_mag = 0;
            }
            if (game_mag) {
                SetActorTransform(game_mag, show_loc, show_rot);
                // Keep our magazine and put it back in the pouch for the next
                // reload. Destroying it left its rounds floating in the air.
                W.pouch_mag = W.held_mag;
                log::Info("controls: seating the game's own magazine %s (pouch magazine goes back to the pouch)",
                          ue::GetName(game_mag).c_str());
            } else {
                ue::SetProp<UObject>(gun.Object(), "MagazineObj_Temp", W.held_mag);
                log::Info("controls: seating the pouch magazine itself (game gave none)");
            }
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
            if (along > -4.0 && along < 14.0 && lateral < 5.5 && aim_off_deg < 40.0) {
                W.mag_engaged = true;
                input::Haptic(input::kLeft, 0.25f, 0.02f);
                log::Info("controls: magazine lined up with the magwell (%.1f cm below, %.1f off axis, %.0f deg)",
                          along, lateral, aim_off_deg);
            }
        } else if (lateral > 7.0 || along > 17.0 || aim_off_deg > 50.0) {
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
    W.mag_shown_rot = show_rot;

    // Let go of the grab: at the pouch it goes back in, anywhere else it drops.
    if (lin.grip_button.released) {
        W.mag_engaged = false;
        const double d = Len(lh.grip.location - pouch_loc);
        if (have_pouch && !W.pouch_mag && d <= pouch_radius) {
            W.pouch_mag = W.held_mag;
            W.held_mag = 0;
            input::Haptic(input::kLeft, 0.4f, 0.04f);
            log::Info("controls: magazine put back in the pouch (%.1f cm from it)", d);
            return;
        }
        ue::Call drop(W.held_mag, "DropMagazine");
        drop.Invoke();
        W.held_mag = 0;
        W.refill_pouch_at = g_clock + 0.5;
        log::Info("controls: dropped the held magazine");
    }
}

// Rifle support hand: grab the handguard (or the magwell, which many people
// hold) with the left hand and the gun is held in both hands; the barrel
// then points from the right hand toward the left. Let go to return to one hand.
void SupportGrip(const hands::HandState& lh, const input::HandInput& lin) {
    if (W.is_pistol) return;
    if (W.two_hand && (!lh.valid || !lin.grip_button.down || W.slide.Held() || W.held_mag)) {
        W.two_hand = false;
        W.have_support_point = false;
        log::Info("controls: support hand off the handguard");
        return;
    }
    if (W.two_hand || !lin.grip_button.pressed || !lh.valid || !W.have_pose || W.slide.Held() || W.held_mag)
        return;
    FVector p = W.frame_rot.Inverse().Rotate(lh.grip.location - W.grip_world);
    const bool along = p.X > 6.0 && p.X < W.muzzle_dist - 2.0;
    const bool beside = std::fabs(p.Y) < 10.0;
    const bool height = p.Z > W.bore_height - 14.0 && p.Z < W.bore_height + 7.0;
    log::Info("controls: left grab at gun-frame (%.1f %.1f %.1f), %s the handguard zone", p.X, p.Y, p.Z,
              along && beside && height ? "inside" : "outside");
    if (!(along && beside && height)) return;
    W.two_hand = true;
    W.support_local = p;
    input::Haptic(input::kLeft, 0.35f, 0.03f);
}

// Right stick click: fire selector (safe / semi / auto, as the gun allows).
void FireSelector(game::Gun& gun, const input::HandInput& rin) {
    // Rifles only: a Glock has no selector, and a stray click on the turn
    // stick must never put the pistol on safe.
    if (W.is_pistol || !rin.stick_click.pressed) return;
    ue::Call c(gun.Object(), "Gun_SwitchFireMode");
    c.Invoke();
    uint8_t mode = 0;
    ue::GetProp(gun.Object(), "SelectedFireMode", mode);
    input::Haptic(input::kRight, 0.3f, 0.02f);
    log::Info("controls: fire selector -> mode %d", mode);
}

// ---- other game actions on the controllers ----
// The game's own input events (keyboard/gamepad bindings) live on the player
// as InpActEvt_<Action>_K2Node_InputActionEvent_<n> stubs; where an action
// has a pressed and a released event, the lower number is "pressed" (the
// Blueprint compiler creates Pressed before Released). Calling the stub is
// what the engine does when the key goes down or up.
struct ActionEvent { const char* name; UObject fn = 0; bool looked = false; };
bool FireEvent(UObject player, ActionEvent& e) {
    if (!player) return false;
    if (!e.looked) {
        e.looked = true;
        e.fn = ue::FindFunction(ue::GetClass(player), e.name);
        log::Info("controls: game input %s %s", e.name, e.fn ? "found" : "NOT found");
    }
    if (!e.fn) return false;
    ue::Call c(player, e.fn);
    return c.Invoke();
}

struct GameActionState {
    ActionEvent jump_down{"InpActEvt_Jump_K2Node_InputActionEvent_19"};
    ActionEvent jump_up{"InpActEvt_Jump_K2Node_InputActionEvent_20"};
    ActionEvent crouch_down{"InpActEvt_Crouch_K2Node_InputActionEvent_17"};
    ActionEvent crouch_up{"InpActEvt_Crouch_K2Node_InputActionEvent_18"};
    ActionEvent sprint_down{"InpActEvt_Sprint_K2Node_InputActionEvent_13"};
    ActionEvent sprint_up{"InpActEvt_Sprint_K2Node_InputActionEvent_14"};
    ActionEvent walk_down{"InpActEvt_Walk_K2Node_InputActionEvent_15"};
    ActionEvent walk_up{"InpActEvt_Walk_K2Node_InputActionEvent_16"};
    ActionEvent nav{"InpActEvt_CheckNavigationIcon_K2Node_InputActionEvent_0"};
    bool jump_armed = true, crouch_armed = true;
    double jump_release_at = -1, crouch_release_at = -1, walk_release_at = -1;
    bool sprinting = false;
    double click_up_at = -1;    // left stick click released, waiting for a second click
    double click_down_at = -1;
    bool second_click = false;
    double y_down_at = -1;
    double rclick_down_at = -1;  // right stick click: tap = objective markers, hold 2 s = move holsters
    double melee_ready_at = 0;
    double melee_release_at = -1;
} GA;

bool GunIsRifle(UObject player);

// ---- physical crouch and jump (VR tab) ----
// Crouching for real crouches the character (hitbox, speed, cover) and
// standing up stands it up; a quick hop jumps (off by default). The
// character's Crouch/UnCrouch are called directly, so it works whether the
// game's own crouch is set to hold or toggle. Only changes of your real
// stance act: the stick crouch still works in between.
comfort::PhysicalBody g_phys;
struct CrouchRead {
    bool looked = false;
    int32_t offset = -1;
    uint8_t mask = 0;
    const char* how = "none";
} g_crouch_read;

// Is the character crouched? ACharacter.bIsCrouched (the engine's own flag,
// set by Crouch/UnCrouch, which the game's ALS stance follows).
bool GameCrouched(UObject player, bool* known) {
    if (!g_crouch_read.looked) {
        g_crouch_read.looked = true;
        auto p = ue::reflection::FindProperty(ue::GetClass(player), "bIsCrouched");
        if (p.valid() && p.type == "BoolProperty") {
            g_crouch_read.offset = p.offset;
            g_crouch_read.mask = p.bool_mask ? p.bool_mask : 0xFF;
            g_crouch_read.how = "Character.bIsCrouched";
        }
        log::Info("controls: physical crouch reads the character's crouch from %s", g_crouch_read.how);
    }
    *known = g_crouch_read.offset >= 0;
    if (!*known) return false;
    return (mem::ReadOr<uint8_t>(player + g_crouch_read.offset, 0) & g_crouch_read.mask) != 0;
}

// Real-height mode: the view follows your real head, so the game camera's
// own crouch drop is given back (else crouching for real would lower the
// view twice: once for your head, once for the character).
struct CamHeight {
    bool have_stand = false;
    double stand_rel = 0;   // camera above the capsule's bottom while standing, cm
    double extra = 0;       // what is given back to the view now, cm
} g_cam;

void RealHeightCrouchView(UObject player, bool crouched, bool known) {
    double want = 0;
    FVector cam;
    if (g_set.real_height && known && stereo::GameCameraLocation(&cam)) {
        ue::Call loc(player, "K2_GetActorLocation");
        UObject capsule = 0;
        double half = 0;
        if (ue::GetProp(player, "CapsuleComponent", capsule) && capsule) {
            ue::Call h(capsule, "GetScaledCapsuleHalfHeight");
            if (h.Invoke()) half = h.Result<float>();
        }
        if (loc.Invoke() && half > 10.0) {
            double rel = cam.Z - (loc.Result<FVector>().Z - half);
            if (!crouched) {
                g_cam.stand_rel = g_cam.have_stand ? g_cam.stand_rel + (rel - g_cam.stand_rel) * 0.1 : rel;
                g_cam.have_stand = true;
            } else if (g_cam.have_stand) {
                want = std::fmax(0.0, std::fmin(80.0, g_cam.stand_rel - rel));
            }
        }
    }
    g_cam.extra += (want - g_cam.extra) * (1.0 - std::exp(-12.0 * g_dt));
    if (std::fabs(g_cam.extra) < 0.05 && want == 0) g_cam.extra = 0;
    stereo::SetViewRaiseCm(g_cam.extra);
}

// Body calibration (VR tab: Calibrate body): after the menu closes, 2 s to
// stand up straight, then the highest head height over 2 s is your standing
// height. Saved as standing_height_cm, so real crouch knows it from the start.
struct HeightMeasure {
    std::atomic<bool> requested{false};
    double start = -1;
    double best = 0;
    double applied_cm = -1;
} g_height;

void HeightCalibration(double head_m) {
    if (g_height.requested.exchange(false)) {
        g_height.start = g_clock;
        g_height.best = 0;
        log::Info("controls: height measure: stand up straight and look ahead");
    }
    if (g_height.start < 0) return;
    const double t = g_clock - g_height.start;
    if (t < 2.0) return;
    if (g_height.best == 0) {
        input::Haptic(input::kLeft, 0.3f, 0.08f);
        input::Haptic(input::kRight, 0.3f, 0.08f);
    }
    g_height.best = std::fmax(g_height.best, head_m);
    if (t < 4.0) return;
    g_height.start = -1;
    const double cm = std::round(g_height.best * 100.0);
    if (cm < 100 || cm > 230) {
        log::Warn("controls: height measure gave %.0f cm, which can't be right; not kept", cm);
        return;
    }
    g_phys.SetStanding(cm / 100.0);
    g_height.applied_cm = cm;
    g_set.standing_height_cm = cm;
    char v[32];
    std::snprintf(v, sizeof(v), "%.0f", cm);
    config::Set("standing_height_cm", v);
    input::Haptic(input::kLeft, 0.6f, 0.15f);
    input::Haptic(input::kRight, 0.6f, 0.15f);
    log::Info("controls: height measured: your eyes are %.0f cm above the floor standing (real crouch uses this)",
              cm);
}

void PhysicalCrouchAndJump(UObject player) {
    bool known = false;
    const bool crouched = GameCrouched(player, &known);
    RealHeightCrouchView(player, crouched, known);
    g_phys.Configure(g_set.physical_crouch, g_set.physical_crouch_cm, g_set.physical_jump);
    if (g_set.standing_height_cm >= 100 && g_set.standing_height_cm != g_height.applied_cm) {
        g_phys.SetStanding(g_set.standing_height_cm / 100.0);
        g_height.applied_cm = g_set.standing_height_cm;
        log::Info("controls: standing height %.0f cm (measured earlier)", g_set.standing_height_cm);
    }
    xr::EyeView v[2];
    if (!xr::LatestViews(v)) return;
    const double head_m = (v[0].pose.position.y + v[1].pose.position.y) * 0.5;
    HeightCalibration(head_m);
    if (!g_set.physical_crouch && !g_set.physical_jump) return;
    const comfort::BodyEvent e = g_phys.Update(head_m, g_dt);
    using E = comfort::BodyEvent;
    if (e == E::kCrouch || e == E::kStand) {
        const bool down = e == E::kCrouch;
        log::Info("controls: physical %s (head %.0f cm below standing %.0f cm; character %s)",
                  down ? "crouch" : "stand", g_phys.Drop() * 100.0, g_phys.Standing() * 100.0,
                  !known ? "stance unknown" : crouched ? "crouched" : "standing");
        if (known && crouched == down) return;  // already there (stick crouch)
        ue::Call c(player, down ? "Crouch" : "UnCrouch");
        c.Set("bClientSimulation", false);
        if (!c.Invoke()) log::Warn("controls: the character has no %s", down ? "Crouch" : "UnCrouch");
        input::Haptic(input::kLeft, 0.15f, 0.02f);
    } else if (e == E::kJump && GA.jump_armed && GA.jump_release_at < 0) {
        FireEvent(player, GA.jump_down);
        GA.jump_release_at = g_clock + 0.15;
        log::Info("controls: physical jump (head rising fast)");
    }
}

// Every other game action, on buttons the gun, menus, wrist and grip editor
// don't use (lean is left out: lean for real).
//   Left trigger          interact (doors, pickups)
//   Left stick click      tap: sprint on/off (ends when you stop); double tap: walk/run
//   Right stick up        jump
//   Right stick down      crouch / stand
//   Left Y tap            switch weapon (holding Y for 1 s is the grip editor)
//   Right stick click     fire selector on a rifle, otherwise objective markers (tap)
//   Right stick click held 2 s   move the holsters to where the hands are
//   Punch with the empty left hand   melee attack
void GameActions(UObject player, const input::Snapshot& s) {
    if (!player) return;
    const input::HandInput& l = s.hand[input::kLeft];
    const input::HandInput& r = s.hand[input::kRight];
    if (ui_panel::MenuOpen()) {
        if (GA.sprinting) { FireEvent(player, GA.sprint_up); GA.sprinting = false; }
        return;
    }
    PhysicalCrouchAndJump(player);
    // Interact.
    if (l.trigger_button.pressed && !W.held_mag && !W.slide.Held()) {
        ue::Call c(player, "PlayerInput_Interact");
        c.Invoke();
        input::Haptic(input::kLeft, 0.2f, 0.02f);
        log::Info("controls: interact");
    }
    // Jump / crouch: flick the right stick up or down (turning uses left/right).
    const bool up = r.stick_y > 0.75f && std::fabs(r.stick_x) < 0.6f;
    const bool down = r.stick_y < -0.75f && std::fabs(r.stick_x) < 0.6f;
    if (up && GA.jump_armed) {
        GA.jump_armed = false;
        FireEvent(player, GA.jump_down);
        GA.jump_release_at = g_clock + 0.15;
        log::Info("controls: jump");
    }
    if (down && GA.crouch_armed) {
        GA.crouch_armed = false;
        FireEvent(player, GA.crouch_down);
        GA.crouch_release_at = g_clock + 0.1;
        log::Info("controls: crouch / stand");
    }
    if (r.stick_y < 0.4f) GA.jump_armed = true;
    if (r.stick_y > -0.4f) GA.crouch_armed = true;
    if (GA.jump_release_at > 0 && g_clock >= GA.jump_release_at) { FireEvent(player, GA.jump_up); GA.jump_release_at = -1; }
    if (GA.crouch_release_at > 0 && g_clock >= GA.crouch_release_at) { FireEvent(player, GA.crouch_up); GA.crouch_release_at = -1; }

    // Left stick click: a short tap toggles sprint, a double tap toggles
    // walk/run. Long holds are left alone (holding it 2 s measures your arms).
    if (l.stick_click.pressed) {
        GA.click_down_at = g_clock;
        GA.second_click = GA.click_up_at > 0 && g_clock - GA.click_up_at < 0.35;
    }
    if (l.stick_click.released && GA.click_down_at > 0) {
        const bool tap = g_clock - GA.click_down_at < 0.4;
        GA.click_down_at = -1;
        if (tap && GA.second_click) {
            GA.click_up_at = -1;
            GA.second_click = false;
            FireEvent(player, GA.walk_down);
            GA.walk_release_at = g_clock + 0.1;
            log::Info("controls: walk/run toggle");
        } else if (tap) {
            GA.click_up_at = g_clock;  // decide after the double-tap window
        }
    }
    if (GA.click_up_at > 0 && g_clock - GA.click_up_at >= 0.35) {
        GA.click_up_at = -1;
        GA.sprinting = !GA.sprinting;
        FireEvent(player, GA.sprinting ? GA.sprint_down : GA.sprint_up);
        log::Info("controls: sprint %s", GA.sprinting ? "on" : "off");
    }
    if (GA.walk_release_at > 0 && g_clock >= GA.walk_release_at) { FireEvent(player, GA.walk_up); GA.walk_release_at = -1; }
    // Sprint ends when you let go of the stick.
    if (GA.sprinting && std::sqrt(l.stick_x * l.stick_x + l.stick_y * l.stick_y) < g_set.deadzone) {
        GA.sprinting = false;
        FireEvent(player, GA.sprint_up);
        log::Info("controls: sprint off (stopped)");
    }

    // Left Y tap: next weapon. Held for the grip editor instead.
    if (l.secondary.pressed) GA.y_down_at = g_clock;
    if (l.secondary.released && GA.y_down_at > 0) {
        if (g_clock - GA.y_down_at < 0.4) {
            ue::Call c(player, "PlayerInput_ScrollWeapon");
            c.Set("IsUp", true);
            c.Invoke();
            log::Info("controls: switch weapon");
        }
        GA.y_down_at = -1;
    }

    // Right stick click without a rifle: objective markers, on a short tap
    // (holding it 2 s moves the holsters, see HolsterCalibration).
    if (r.stick_click.pressed) GA.rclick_down_at = g_clock;
    if (r.stick_click.released && GA.rclick_down_at > 0) {
        if (g_clock - GA.rclick_down_at < 0.4 && !GunIsRifle(player)) {
            FireEvent(player, GA.nav);
            log::Info("controls: objective markers");
        }
        GA.rclick_down_at = -1;
    }

    // Melee: punch with the empty left hand (fast, arm reaching out).
    hands::HandState lh = hands::Get(hands::kLeft);
    hands::Pose head;
    if (lh.valid && hands::Head(&head) && !W.held_mag && !W.slide.Held() && !W.two_hand &&
        g_clock >= GA.melee_ready_at) {
        const XrVector3f& v = l.linear_velocity;
        double speed = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        FVector reach = lh.grip.location - head.location;
        if (speed > 2.8 && Len(reach) > 40.0) {
            ue::Call c(player, "PlayerInput_MeleeAttack");
            c.Set("Press Released", false);
            c.Invoke();
            GA.melee_release_at = g_clock + 0.1;
            GA.melee_ready_at = g_clock + 0.7;
            input::Haptic(input::kLeft, 0.8f, 0.06f);
            log::Info("controls: melee (left hand at %.1f m/s)", speed);
        }
    }
    if (GA.melee_release_at > 0 && g_clock >= GA.melee_release_at) {
        ue::Call c(player, "PlayerInput_MeleeAttack");
        c.Set("Press Released", true);
        c.Invoke();
        GA.melee_release_at = -1;
    }
}

bool GunIsRifle(UObject player) {
    game::Gun gun = game::Gun::Current(player);
    return gun.Valid() && gun.Object() == W.gun && !W.is_pistol;
}

void PublishGrabs(game::Gun& gun, const hands::HandState& rh, const hands::HandState& lh) {
    hands::HandGrab g[2];
    if (gun.Valid() && rh.valid && W.in_hand) {
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
        } else if (W.two_hand && W.have_support_point) {
            g[hands::kLeft].kind = hands::GrabKind::kSupportGrip;
            g[hands::kLeft].object = gun.Object();
            g[hands::kLeft].target = lh.grip;
            g[hands::kLeft].target.location = W.support_point;
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
    if (!g_set.rack_trace) {   // per-frame slide diagnostics: off unless debug_rack_trace=1
        W.trace_until = -1;
        return;
    }
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
    gun.SetSlideTravel(W.slide.Travel(), ue::Vec3d{W.slide_axis.X, W.slide_axis.Y, W.slide_axis.Z});
    W.handle_moved = true;  // the game's charge animation may move the handle too
    MoveChargingHandle(gun, gun.State().slide_locked);
}

// The game's recoil (BP_Player Recoil: camera, arm and control-rotation
// kick timelines) moves the VR camera and aim. In VR the kick belongs to the
// hand only, so the call is dropped while motion controls are on.
// Blueprint-to-Blueprint calls skip ProcessEvent, so the Recoil call itself
// may never reach the hook; its timelines always do (timeline updates are
// delegates, which go through ProcessEvent). Block both.
bool BeforePlayerRecoil(UObject, UObject fn, void*) {
    if (!g_set.enabled || g_set.game_recoil) return true;
    static int logged = 0;
    if (logged < 3) {
        ++logged;
        log::Info("controls: blocked game recoil step %s", ue::GetName(fn).c_str());
    }
    return false;
}

void HookPlayerRecoil(UObject player) {
    static bool done = false;
    if (done || !player) return;
    UObject cls = ue::GetClass(player);
    int n = 0;
    for (const char* name : {"Recoil", "TL_CameraRecoil_Up__UpdateFunc", "TL_CameraRecoil_Down__UpdateFunc",
                             "TL_ControllerRecoil_Up__UpdateFunc", "TL_ControllerRecoil_Down__UpdateFunc",
                             "TL_ControllerRecoil_FullAutoRecoilConvergence__UpdateFunc",
                             "TL_ControllerRecoil_StopConvergence__UpdateFunc"}) {
        UObject fn = ue::FindFunction(cls, name);
        if (fn && ue::process_event::On(fn, &BeforePlayerRecoil, nullptr)) ++n;
    }
    done = true;
    log::Info("controls: game camera/aim recoil blocked (%d steps hooked)", n);
}

// The game's own "camera shake" option (BP_Player.CameraShake): off in VR
// unless the ini asks for it. Re-applied now and then in case the game's
// settings menu writes it back.
void ApplyCameraShakeSetting(UObject player) {
    static double next = 0;
    if (!player || g_clock < next) return;
    next = g_clock + 2.0;
    bool cur = true;
    if (ue::GetProp(player, "CameraShake", cur) && cur != g_set.camera_shake) {
        ue::SetProp<bool>(player, "CameraShake", g_set.camera_shake);
        log::Info("controls: game camera shake %s", g_set.camera_shake ? "on" : "off");
    }
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

// Bullet-hole bug (2026-11-02): every shot lands on the first hole. Log
// where the muzzle points against where the gun frame points, the player's
// control rotation and the game camera, so the log shows which one the
// game aims its bullets with. First 12 shots per launch.
// ---- shot study (muzzle trace) ----
// Does the game aim its shots from the gun's muzzle or from the camera? The
// first shots of a run log where a line from the muzzle along the barrel
// lands and where a line from the camera along the view lands, the game's
// own gun/player events for a moment around the shot (weapon lab trace),
// and once, the gun's and player's fire-related functions and variables.
bool TraceFirstHit(UObject player, UObject gun, const FVector& a, const FVector& b, FVector* at, std::string* what) {
    static UObject ksl = 0, gps = 0;
    static int lookups = 0;
    if ((!ksl || !gps) && lookups < 3) {
        ++lookups;
        if (!ksl) ksl = ue::FindObject("KismetSystemLibrary /Script/Engine.Default__KismetSystemLibrary");
        if (!gps) gps = ue::FindObject("GameplayStatics /Script/Engine.Default__GameplayStatics");
    }
    if (!ksl || !gps) return false;
    UObject ignore[2] = {player, gun};
    TArrayRaw arr{ignore, gun ? 2 : 1, 2};
    ue::Call t(ksl, "LineTraceSingle");
    t.Set("WorldContextObject", player).Set("Start", a).Set("End", b).Set("TraceChannel", uint8_t{0})
        .Set("bTraceComplex", false).Set("ActorsToIgnore", arr).Set("bIgnoreSelf", true);
    if (!t.Invoke() || !t.Result<bool>()) return false;
    int32_t hit_size = 0;
    uint8_t* hit = t.Raw("OutHit", &hit_size);
    if (!hit) return false;
    ue::Call br(gps, "BreakHitResult");
    int32_t in_size = 0;
    uint8_t* in = br.Raw("Hit", &in_size);
    if (!in || in_size != hit_size) return false;
    std::memcpy(in, hit, static_cast<size_t>(hit_size));
    if (!br.Invoke() || !br.Get<bool>("bBlockingHit")) return false;
    *at = br.Get<FVector>("ImpactPoint");
    UObject actor = br.Get<UObject>("HitActor");
    *what = actor ? ue::GetName(actor) : "nothing";
    return true;
}

bool FireWord(const std::string& name) {
    static const char* words[] = {"fire", "shoot", "shot", "trace", "bullet", "muzzle", "aim", "hit",
                                  "projectile", "spread", "damage", "camera"};
    std::string n;
    for (char c : name) n += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* w : words)
        if (n.find(w) != std::string::npos) return true;
    return false;
}

void StudyFireClass(UObject obj, const char* who) {
    for (UObject c = ue::GetClass(obj); c; c = ue::reflection::GetSuper(c)) {
        std::string cn = ue::GetName(c);
        if (cn.size() < 2 || cn.compare(cn.size() - 2, 2, "_C") != 0) break;
        std::string fns, vars;
        for (UObject f : ue::reflection::GetFunctions(c))
            if (FireWord(ue::GetName(f))) fns += ue::GetName(f) + " ";
        for (const auto& p : ue::reflection::GetProperties(c))
            if (FireWord(p.name)) vars += p.name + ":" + p.type + " ";
        log::Info("shot study: %s %s fire functions: %s", who, cn.c_str(), fns.empty() ? "none" : fns.c_str());
        log::Info("shot study: %s %s fire variables: %s", who, cn.c_str(), vars.empty() ? "none" : vars.c_str());
    }
}

double g_lab_trace_off_at = -1;

void ShotStudy(UObject player, game::Gun& gun, int n, const FVector& muzzle, const FVector& barrel,
               const FRotator& ctrl) {
    if (n == 1) {
        StudyFireClass(gun.Object(), "gun");
        StudyFireClass(player, "player");
    }
    if (n <= 3 && g_lab_trace_off_at < 0 && !config::GetBool("lab_trace_on_start", false)) {
        game::weapon_lab::SetTrace(true);
        g_lab_trace_off_at = g_clock + 0.4;
    }
    if (n > 6) return;
    FVector m_at, c_at, cam;
    std::string m_what, c_what;
    const bool m_hit = TraceFirstHit(player, gun.Object(), muzzle, muzzle + barrel * 5000.0, &m_at, &m_what);
    FVector view = ToQuat(ctrl).Rotate(FVector{1, 0, 0});
    const bool have_cam = stereo::GameCameraLocation(&cam);
    const bool c_hit =
        have_cam && TraceFirstHit(player, gun.Object(), cam, cam + view * 5000.0, &c_at, &c_what);
    FVector apart = m_at - c_at;
    log::Info("shot study %d: barrel line hits %s at (%.0f %.0f %.0f); camera line hits %s at (%.0f %.0f %.0f); %.0f cm "
              "apart. If the bullet hole is at the barrel point, shots come from the muzzle.",
              n, m_hit ? m_what.c_str() : "nothing", m_at.X, m_at.Y, m_at.Z, c_hit ? c_what.c_str() : "nothing",
              c_at.X, c_at.Y, c_at.Z, m_hit && c_hit ? Len(apart) : -1.0);
}

void ShotStudyTick() {
    if (g_lab_trace_off_at > 0 && g_clock >= g_lab_trace_off_at) {
        g_lab_trace_off_at = -1;
        game::weapon_lab::SetTrace(false);
    }
}

void LogShot(UObject player, game::Gun& gun) {
    static int n = 0;
    if (++n > 12) return;
    UObject muzzle = 0;
    ue::GetProp(gun.Object(), "MuzzleFirePoint", muzzle);
    if (!muzzle) ue::GetProp(gun.Object(), "DefaultMuzzlePoint", muzzle);
    FVector mloc, mfwd;
    if (muzzle) {
        UETransform t = ComponentToWorld(muzzle);
        mloc = t.translation;
        mfwd = t.rotation.Rotate(FVector{1, 0, 0});
    }
    FVector gfwd = W.frame_rot.Rotate(FVector{1, 0, 0});
    FRotator ctrl{};
    ue::Call gc(player, "GetController");
    if (UObject c = gc.Invoke() ? gc.Result<UObject>() : 0) {
        ue::Call r(c, "GetControlRotation");
        if (r.Invoke()) ctrl = r.Result<FRotator>();
    }
    log::Info("shot %d: muzzle (%.0f %.0f %.0f) muzzle X (%.2f %.2f %.2f) barrel (%.2f %.2f %.2f) "
              "control rot (p %.1f y %.1f)", n, mloc.X, mloc.Y, mloc.Z, mfwd.X, mfwd.Y, mfwd.Z, gfwd.X, gfwd.Y,
              gfwd.Z, ctrl.Pitch, ctrl.Yaw);
    UObject mag = 0, round = 0;
    ue::GetProp(gun.Object(), "MagazineObj", mag);
    ue::GetProp(gun.Object(), "AmmoObj_InChamber", round);
    log::Info("shot %d: magazine %s, chambered round %s", n, mag ? ue::GetName(mag).c_str() : "none",
              round ? ue::GetName(round).c_str() : "none");
    ShotStudy(player, gun, n, muzzle ? mloc : W.grip_world + gfwd * W.muzzle_dist, gfwd, ctrl);
}

// ---- holsters ----
// Where the holsters are: each rides on a part of the VR body (belt, chest,
// a thigh; this frame's pose: called from the late tick, after the body), or
// hangs from the headset when the body is off (F9).
void ComputeHolsterWorld() {
    g_hw.valid = false;
    hands::Pose a[kAnchors];
    bool got = true;
    for (int i = 0; i < kAnchors && got; ++i) got = body::GetAnchor(static_cast<body::Anchor>(i), &a[i]);
    if (got) {
        g_hw.on_body = true;
    } else {
        hands::Pose head;
        if (!hands::Head(&head)) return;
        g_hw.on_body = false;
        const FQuat yaw = ToQuat(FRotator{0, YawOf(head.rotation), 0});
        // The belt sits under the spine, a little behind the eyes; the rest
        // hang from it.
        const FVector belt =
            head.location + yaw.Rotate(FVector{-8, 0, 0}) - FVector{0, 0, g_set.belt_below_head};
        a[0] = {belt, yaw};
        a[1] = {belt + FVector{0, 0, 40}, yaw};
        a[2] = {belt + yaw.Rotate(FVector{0, -10, -22}), yaw};
        a[3] = {belt + yaw.Rotate(FVector{0, 10, -22}), yaw};
    }
    for (int i = 0; i < kAnchors; ++i) g_hw.anchor[i] = a[i];
    g_hw.belt = a[0].location;
    g_hw.body_rot = a[0].rotation;
    for (int i = 0; i < 2; ++i) {
        const hands::Pose& an = SpotAnchor(i);
        g_hw.spot[i] = interact::SpotWorld(g_spots[i], an.location, an.rotation);
        g_hw.spot_anchor_rot[i] = an.rotation;
    }
    g_hw.valid = true;
    const int mode = g_hw.on_body ? 1 : 0;
    if (mode != g_hw.logged_mode) {
        g_hw.logged_mode = mode;
        hands::Pose head;
        const bool hv = hands::Head(&head);
        auto below = [&](int i) { return hv ? head.location.Z - a[i].location.Z : 0.0; };
        log::Info("holster: body parts from %s: belt %.0f cm below the headset, chest %.0f, thighs %.0f and %.0f",
                  g_hw.on_body ? "the VR body" : "the headset (VR body off)", below(0), below(1), below(2),
                  below(3));
        for (int i = 0; i < 2; ++i) {
            const interact::HolsterSpot& sp = g_spots[i];
            log::Info("holster: %s spot on %s, (%.1f %.1f %.1f) cm, %s", i == 0 ? "pistol" : "magazine",
                      interact::AnchorPlace(sp.anchor), sp.offset.X, sp.offset.Y, sp.offset.Z,
                      sp.turned ? "at the angle it was placed" : "hanging the built-in way");
        }
    }
}

// The pistol in its holster: as it was held when the holster was placed in
// the headset, otherwise the built-in hip hanging (muzzle down, grip up and
// back, right side out: interact::PistolHolsterFrame). Placed exactly as
// HoldGun would place it for a hand at the holster spot, so drawing it moves
// the gun as little as possible. Flies or slides in from where it was let go.
void PlaceGunInHolster(game::Gun& gun) {
    UObject g = gun.Object();
    UObject main = 0;
    ue::GetProp(g, "MainHandPosition", main);
    if (!main) return;
    DetachIfAttached(g);
    const FVector actor_loc = ActorLocation(g);
    const FQuat actor_rot = ActorRotation(g);
    const UETransform main_world = ComponentToWorld(main);
    const FVector rel = actor_rot.Inverse().Rotate(main_world.translation - actor_loc);
    if (!W.have_frame) LearnGunFrame(g, actor_loc, actor_rot, main_world.translation);
    const interact::HolsterSpot& sp = g_spots[kPistolSpot];
    const FQuat frame = sp.turned ? g_hw.spot_anchor_rot[0] * sp.rot
                                  : interact::PistolHolsterFrame(g_hw.spot_anchor_rot[0], g_set.holster_cant);
    const FVector grip_w = g_hw.spot[0] + frame.Rotate(GunGripOffset());
    FQuat rot = W.have_frame ? frame * W.gun_frame.Inverse() : frame;
    FVector loc = grip_w - rot.Rotate(rel);
    const double b = W.holster.Blend();
    if (b < 1.0 && W.have_release) {
        const double e = b * b * (3.0 - 2.0 * b);
        loc = W.release_loc + (loc - W.release_loc) * e;
        rot = Nlerp(W.release_rot, rot, e);
    }
    SetActorTransform(g, loc, rot);
}

// True while obj is still the object the engine holds at its own index. A
// magazine the game destroys (dropped mags despawn, level streaming) frees
// its slot, so this catches it before we call functions on freed memory.
bool LiveObject(UObject obj) {
    if (!obj) return false;
    const int32_t idx = mem::ReadOr<int32_t>(obj + ue::off::kObjIndex, -1);
    return idx >= 0 && ue::objects::Get(idx) == obj && ue::GetClass(obj) != 0;
}

// Forget magazines the game destroyed; RefillPouch spawns a new pouch mag.
void DropDeadMagazines() {
    if (W.pouch_mag && !LiveObject(W.pouch_mag)) {
        log::Info("controls: the pouch magazine was destroyed by the game; a new one will be spawned");
        W.pouch_mag = 0;
        W.refill_pouch_at = g_clock + 0.5;
    }
    if (W.held_mag && !LiveObject(W.held_mag)) {
        log::Info("controls: the held magazine was destroyed by the game; the left hand lets go");
        W.held_mag = 0;
        W.refill_pouch_at = g_clock + 0.5;
    }
    if (W.learn_mag && !LiveObject(W.learn_mag)) W.learn_mag = 0;
}

// The spare magazine sitting in its pouch.
void PlacePouchMag(UObject gun) {
    if (W.pouch_mag && !LiveObject(W.pouch_mag)) return;   // OnTick clears it
    if (!W.pouch_mag) return;
    DetachIfAttached(W.pouch_mag);
    { ue::Call v(W.pouch_mag, "SetVisibility"); v.Set("IsVisible", true); v.Invoke(); }
    SetActorTransform(W.pouch_mag, g_hw.spot[1], PouchRotation(gun));
}

void ReleaseTrigger(UObject player) {
    if (!W.trigger_sent) return;
    ue::Call c(player, "PlayerInput_UseWeapon");
    c.Set("Press Released", true);
    c.Invoke();
    W.trigger_sent = false;
}

// Right grip: draw the pistol from the right hip and hold it. Let go at the
// hip and it is holstered; anywhere else it goes back to the holster by
// itself. While the grip editor borrows a controller, or a menu is open,
// nothing changes (the editor needs the gun to stay in the hand).
void UpdatePistolHolster(UObject player, const hands::HandState& rh, const input::HandInput& rin) {
    if (!W.uses_holster) {
        W.in_hand = true;
        return;
    }
    interact::DrawInput in;
    in.hand = rh.grip.location;
    in.hand_valid = rh.valid;
    in.spot = g_hw.spot[0];
    in.spot_valid = g_hw.valid;
    in.grip_down = rin.grip_button.down;
    in.grip_pressed = rin.grip_button.pressed;
    in.freeze = hands::EditorHand() >= 0 || ui_panel::MenuOpen();
    in.dt = g_dt;
    const bool was_in_hand = W.in_hand;
    using E = interact::HolsterEvent;
    const E e = W.holster.Update(in);
    W.in_hand = W.holster.InHand();
    switch (e) {
        case E::kHoverEnter:
            input::Haptic(input::kRight, 0.15f, 0.015f);
            break;
        case E::kDrawn:
            input::Haptic(input::kRight, 0.5f, 0.04f);
            log::Info("holster: pistol drawn (hand %.1f cm from the holster spot)", W.holster.LastDistance());
            break;
        case E::kStowed:
            input::Haptic(input::kRight, 0.6f, 0.05f);
            log::Info("holster: pistol holstered (let go %.1f cm from the holster spot)", W.holster.LastDistance());
            break;
        case E::kReturnStarted:
            log::Info("holster: pistol let go %.0f cm from the holster, it goes back by itself",
                      W.holster.LastDistance());
            break;
        default: break;
    }
    if (was_in_hand && !W.in_hand) {
        W.release_loc = W.gun_set_loc;
        W.release_rot = W.actor_rot;
        W.have_release = W.have_pose;
        ReleaseTrigger(player);
    }
}

// Right stick click held 2 s: each holster moves to where the hand holding
// its item is now (pistol in the right hand, magazine in the left), anywhere
// on the body, and keeps the item at the angle it is held. It rides on the
// nearest body part (belt, chest, a thigh) and is saved to
// hole-vr/holsters.ini. Needs the VR body, since the spots ride on it.
struct HolsterCalState {
    double down_at = -1;
    bool done = false;
} g_cal;

// Moves holster `spot` to the hand at `hand`, holding its item turned `item_rot`.
void PlaceHolsterAt(int spot, const FVector& hand, const FQuat* item_rot) {
    FVector locs[kAnchors];
    bool valid[kAnchors];
    for (int i = 0; i < kAnchors; ++i) {
        locs[i] = g_hw.anchor[i].location;
        valid[i] = true;
    }
    const interact::HolsterAnchor a = interact::NearestAnchor(hand, locs, valid);
    const hands::Pose& an = g_hw.anchor[static_cast<int>(a)];
    g_spots[spot] = interact::PlaceSpot(g_spots[spot], a, an.location, an.rotation, hand, item_rot);
    g_spot_moved[spot] = true;
    const interact::HolsterSpot& sp = g_spots[spot];
    log::Info("holster: moved the %s to %s, (%.1f %.1f %.1f) cm (forward, right, up), %.0f cm from its centre, %s",
              spot == kPistolSpot ? "pistol holster" : "magazine pouch", interact::AnchorPlace(a), sp.offset.X,
              sp.offset.Y, sp.offset.Z, Len(hand - an.location),
              sp.turned ? "at the angle you held it" : "hanging the built-in way (angle not known)");
}

void HolsterCalibration(const input::HandInput& rin, const hands::HandState& rh, const hands::HandState& lh) {
    if (rin.stick_click.pressed) {
        g_cal.down_at = g_clock;
        g_cal.done = false;
    }
    if (!rin.stick_click.down) g_cal.down_at = -1;
    if (g_cal.down_at < 0 || g_cal.done || g_clock - g_cal.down_at < 2.0) return;
    g_cal.done = true;
    if (!W.is_pistol) return;   // the rifle's selector is on this click; rifle holsters come later
    if (!g_hw.valid || !g_hw.on_body) {
        log::Info("holster: can't move the holsters while the VR body is off (F9)");
        input::Haptic(input::kRight, 0.2f, 0.1f);
        return;
    }
    int moved = 0;
    if (W.uses_holster && W.in_hand && rh.valid) {
        PlaceHolsterAt(kPistolSpot, rh.grip.location, W.have_pose ? &W.frame_rot : nullptr);
        input::Haptic(input::kRight, 0.8f, 0.1f);
        ++moved;
    }
    if (W.held_mag && lh.valid && !W.mag_engaged && !W.mag_seating) {
        PlaceHolsterAt(kMagSpot, lh.grip.location, &W.mag_shown_rot);
        input::Haptic(input::kLeft, 0.8f, 0.1f);
        ++moved;
    }
    if (!moved) {
        log::Info("holster: to move a holster, hold the pistol and/or a magazine where you want it, then hold the "
                  "right stick click 2 s");
        return;
    }
    const bool saved = SaveHolsterSpots();
    log::Info("holster: %s", saved ? "saved to holsters.ini" : "NOT saved (holsters.ini not writable)");
    g_hw.logged_mode = -1;
}

// Set by OnTick when it handled a gun this frame, so the late tick only
// places holstered items for the gun this frame's controls saw.
bool g_late_ok = false;

void OnLateTick(UObject player, float) {
    if (!g_set.enabled) return;
    ComputeHolsterWorld();
    if (!g_late_ok) return;
    g_late_ok = false;
    if (!g_hw.valid || !W.gun || !player) return;
    game::Gun gun(W.gun);
    if (W.uses_holster && !W.in_hand && !W.edit_frozen) PlaceGunInHolster(gun);
    PlacePouchMag(W.gun);
}

void OnTick(UObject player, float dt) {
    g_clock += dt;
    g_dt = dt;
    input::Snapshot s = input::Consume();
    if (!s.valid || !g_set.enabled) return;
    UpdateHands(s);
    // The grip editor (vr/body) borrows one controller: none of its buttons
    // reach the game, and the gun does not fire while a grip is edited.
    if (int eh = g_editor_hand.load(); eh == 0 || eh == 1) {
        input::HandInput& e = s.hand[eh];
        e.grip_button = e.trigger_button = e.primary = e.secondary = e.stick_click = input::Button{};
        e.grip_value = e.trigger = 0.f;
        s.hand[input::kRight].trigger_button = input::Button{};
    }
    if (g_set.swap_sticks) {
        // Stick roles swapped (left-handed players): the right stick moves
        // (and its click sprints), the left one turns, jumps and crouches.
        input::HandInput& a = s.hand[input::kLeft];
        input::HandInput& b = s.hand[input::kRight];
        std::swap(a.stick_x, b.stick_x);
        std::swap(a.stick_y, b.stick_y);
        std::swap(a.stick_click, b.stick_click);
    }
    Locomotion(player, s, dt);
    RoomScaleFollow(player);
    GameActions(player, s);
    ApplyCameraShakeSetting(player);

    game::Gun gun = game::Gun::Current(player);
    if (gun.Object() != W.gun) {
        W = WeaponState();
        W.gun = gun.Object();
        if (gun.Valid()) {
            W.is_pistol = gun.ClassName().find("HG") != std::string::npos;
            W.uses_holster = W.is_pistol && g_set.holster_pistol && g_spots[kPistolSpot].enabled;
            W.holster.Configure(g_spots[kPistolSpot].radius,
                                g_set.holster_toggle ? interact::DrawMode::kToggle : interact::DrawMode::kHold);
            W.holster.Reset(interact::HolsterState::kHolstered);
            W.in_hand = !W.uses_holster;
            log::Info("controls: holding %s (%s)", gun.ClassName().c_str(), W.is_pistol ? "pistol" : "rifle");
            auto seat = g_mag_seats.find(gun.ClassName());
            if (seat != g_mag_seats.end()) {
                W.seat_offset = seat->second.offset;
                W.seat_rot = seat->second.rot;
                W.seat_learned = true;
            }
            if (W.uses_holster)
                log::Info("holster: the pistol starts in its holster (on %s)",
                          interact::AnchorPlace(g_spots[kPistolSpot].anchor));
        }
    }
    if (!gun.Valid()) {
        hands::HandState none;
        PublishGrabs(gun, none, none);
        return;
    }
    DropDeadMagazines();
    HookChargeTimeline();
    HookPlayerRecoil(player);
    ShotStudyTick();
    {
        // A round leaving the gun while the trigger is held = a shot (works
        // for full auto too). Racking also removes a round, but not with the
        // trigger held and the slide untouched.
        game::GunState st = gun.State();
        int rounds = st.chamber + (st.magazine > 0 ? st.magazine : 0);
        if (W.last_rounds >= 0 && rounds < W.last_rounds && st.has_magazine == W.last_has_mag &&
            s.hand[input::kRight].trigger_button.down && !W.slide.Held())
        {
            W.kick_t0 = g_clock;
            W.kick_side = (std::rand() % 2001) / 1000.0 - 1.0;
            LogShot(player, gun);
        }
        W.last_rounds = rounds;
        W.last_has_mag = st.has_magazine;
    }
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
    if (!W.seat_learned && W.learn_at < 0 && W.have_pose && W.in_hand && !W.mag_seating) {
        // The gun came with a magazine in it: learn where it sits now, so the
        // first reload already lines up with the real seat.
        UObject mag = 0;
        if (gun.State().has_magazine && ue::GetProp(gun.Object(), "MagazineObj", mag) && mag) {
            W.learn_mag = mag;
            W.learn_at = g_clock;
        }
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
                g_mag_seats[gun.ClassName()] = MagSeat{W.seat_offset, W.seat_rot};
                SaveMagSeats();
                // How far the seated magazine is turned from the magwell
                // point (what the pouch and hand used before this insert).
                const FQuat wq = inv * ComponentToWorld(well).rotation;
                const double dq = std::fabs(wq.X * W.seat_rot.X + wq.Y * W.seat_rot.Y + wq.Z * W.seat_rot.Z +
                                            wq.W * W.seat_rot.W);
                log::Info("controls: learned magazine seat (%.1f %.1f %.1f), turned %.0f deg from the magwell point",
                          W.seat_offset.X, W.seat_offset.Y, W.seat_offset.Z,
                          2.0 * std::acos(std::fmin(1.0, dq)) * 180.0 / 3.14159265358979);
            }
        }
    }
    {
        // Left X: weapon light.
        const input::HandInput& l = s.hand[input::kLeft];
        if ((l.primary.pressed || l.primary.released) && !ui_panel::MenuOpen()) {
            ue::Call c(player, "PlayerInput_SwitchLight");
            c.Set("IsPressed", l.primary.down);
            c.Invoke();
        }
    }
    hands::HandState rh = hands::Get(hands::kRight), lh = hands::Get(hands::kLeft);
    // Grip editor: everything holds still while the owner poses the hands.
    // The gun stays where it was when editing started, and the slide, the
    // held magazine and the grabs keep their state (no drop, no release).
    const bool editing = hands::EditorHand() >= 0;
    if (editing && !W.edit_frozen) {
        W.edit_frozen = true;
        W.edit_gun_loc = ActorLocation(gun.Object());
        W.edit_gun_rot = ActorRotation(gun.Object());
        log::Info("controls: grip editor on, gun, slide and magazine frozen");
    } else if (!editing && W.edit_frozen) {
        W.edit_frozen = false;
        log::Info("controls: grip editor off, gun handling back on");
    }
    if (!editing) UpdatePistolHolster(player, rh, s.hand[input::kRight]);
    if (editing) SetActorTransform(gun.Object(), W.edit_gun_loc, W.edit_gun_rot);
    else if (rh.valid && W.in_hand) HoldGun(gun, rh);
    g_late_ok = true;
    if (editing) {
        if (W.trigger_sent) {
            ue::Call c(player, "PlayerInput_UseWeapon");
            c.Set("Press Released", true);
            c.Invoke();
            W.trigger_sent = false;
        }
    } else if (ui_panel::MenuOpen()) {
        // A game menu is up: the trigger is a laser-pointer click, so no
        // firing, grabbing or gun buttons. Let go of anything held.
        if (W.trigger_sent) {
            ue::Call c(player, "PlayerInput_UseWeapon");
            c.Set("Press Released", true);
            c.Invoke();
            W.trigger_sent = false;
        }
        W.two_hand = false;
        W.have_support_point = false;
    } else {
        if (W.in_hand) Trigger(player, gun, s.hand[input::kRight]);
        SlideHandling(gun, lh, s.hand[input::kLeft], dt, W.in_hand);
        // Grip toggle (VR tab): a held magazine stays in the hand until the
        // next squeeze. The slide and the support hand always hold.
        const input::HandInput& lraw = s.hand[input::kLeft];
        input::HandInput lmag = lraw;
        const bool toggle_drop = g_set.grip_toggle && W.held_mag && lraw.grip_button.pressed;
        {
            comfort::GripView gv = comfort::ToggleGrip(g_set.grip_toggle, W.held_mag != 0, lraw.grip_button.down,
                                                       lraw.grip_button.pressed, lraw.grip_button.released);
            lmag.grip_button.down = gv.down;
            lmag.grip_button.pressed = gv.pressed;
            lmag.grip_button.released = gv.released;
        }
        MagazineHandling(gun, player, lh, lmag, s.hand[input::kRight]);
        HolsterCalibration(s.hand[input::kRight], rh, lh);
        input::HandInput lsup = lraw;
        if (toggle_drop) lsup.grip_button.pressed = false;  // that squeeze let go of the magazine
        SupportGrip(lh, lsup);
        FireSelector(gun, s.hand[input::kRight]);
    }
    PublishGrabs(gun, rh, lh);
    RackTrace(gun, dt);
}

// The VR tab changed a setting: read them again and apply what needs more
// than a new value (the pistol holster's hold/toggle mode).
void ReloadSettings() {
    LoadSettings();
    if (W.gun)
        W.holster.Configure(g_spots[kPistolSpot].radius,
                            g_set.holster_toggle ? interact::DrawMode::kToggle : interact::DrawMode::kHold);
    g_aim_smoother.Reset();
    log::Info("controls: settings changed (%s, %s turn %.0f deg / %.0f deg/s, move by %s, grip %s, aim smoothing %.2f, "
              "height %s, physical crouch %s, physical jump %s)",
              g_set.teleport ? "teleport" : "smooth moving", g_set.smooth_turn ? "smooth" : "snap", g_set.snap_degrees,
              g_set.smooth_speed, g_set.move_with_hand ? "hand" : "head", g_set.grip_toggle ? "toggle" : "hold",
              g_set.aim_smoothing,
              g_set.real_height ? "real" : g_set.seated ? "seated" : "fixed", g_set.physical_crouch ? "on" : "off",
              g_set.physical_jump ? "on" : "off");
}

}  // namespace

void RequestHeightMeasure() {
    g_height.requested.store(true);
    log::Info("controls: height measure asked for from the VR tab; it starts when the menu closes");
}

void Init() {
    LoadSettings();
    LoadMagSeats();
    config::AddChangeListener(&ReloadSettings);
    game::AddTickListener(&OnTick);
    log::Info("controls: ready (%s turn, move by %s)", g_set.smooth_turn ? "smooth" : "snap",
              g_set.move_with_hand ? "hand" : "head");
}

void InitLate() {
    game::AddTickListener(&OnLateTick);
    log::Info("controls: holsters ready (pistol %s, %s; magazine pouch; to move one anywhere on the body, hold its "
              "item there at the angle you want and hold the right stick click 2 s)",
              g_set.holster_pistol ? "in a holster" : "always in the hand",
              g_set.holster_toggle ? "squeeze to draw, squeeze at the holster to put it away"
                                   : "hold the right grip to hold it");
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
EditButtons GetEditButtons() {
    std::lock_guard<std::mutex> lock(controls::g_hands_mutex);
    return controls::g_edit_buttons;
}
void SetEditorHand(int hand) { controls::g_editor_hand.store(hand); }
int EditorHand() { return controls::g_editor_hand.load(); }
bool Head(Pose* out) {
    std::lock_guard<std::mutex> lock(controls::g_hands_mutex);
    if (!controls::g_head_valid) return false;
    *out = controls::g_head;
    return true;
}
}  // namespace holevr::vr::hands
