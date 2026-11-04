#include "vr/controls.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "core/perf.h"
#include "game/game.h"
#include "game/gun.h"
#include "game/weapon_custom.h"
#include "game/weapon_lab.h"
#include "ue/call.h"
#include "ue/process_event.h"
#include "ue/reflection.h"
#include "vr/body/body.h"
#include "vr/comfort.h"
#include "vr/comfort_math.h"
#include "vr/teleport.h"
#include "ue/names.h"
#include "vr/hands.h"
#include "vr/workbench.h"
#include "vr/drinks.h"
#include "vr/input.h"
#include "vr/interact/grip_points.h"
#include "vr/interact/holster.h"
#include "vr/interact/rifle_carry.h"
#include "vr/interact/slide.h"
#include "vr/interact/touch.h"
#include "vr/ui_panel.h"
#include "vr/openxr.h"
#include "vr/stereo.h"
#include "vr/view_lead_math.h"
#include "vr/world_menu.h"

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
    double light_reach = 12.0;     // left hand this close to the gun: left trigger = weapon light, cm
    double gun_pitch = 0.0;        // extra pitch on the held gun, degrees
    double gun_yaw = 0.0;
    double gun_roll = 0.0;
    double gun_forward = 0.0;      // extra shift of the gun along the barrel, cm
    double gun_right = 0.0;        // extra shift to the right, cm
    // Holsters (vr/interact/holster.h): pistol on the right hip, spare
    // magazine (the pouch) on the left hip.
    bool holster_pistol = true;     // false: the pistol is always in the hand
    bool holster_rifle = true;      // false: the rifle rests where the game hangs it
    bool holster_toggle = false;    // squeeze to draw, squeeze at the holster to put away
    double holster_cant = 10.0;     // grip tilted forward, degrees
    double belt_below_head = 60.0;  // belt height when the VR body is off, cm below the headset
    bool rack_trace = false;            // per-frame slide log while racking (debug)
    double slide_pinch_radius = 6.5;    // pinch zone around the rear sight...
    double slide_pinch_back = 14.0;     // ...stretched this far back along the slide (cm): the controller's grip
                                        // point sits 5-12 cm behind fingertips that pinch the rear of the slide
    double slide_grab_forward = 16.0;   // pistol slide grab zone reaches this far forward of the rear grab point
    // The visible fingers' pinch (thumb tip and index middle knuckle) also
    // grabs when it is at the rear sight: within slide_pinch_radius of the
    // line from the sight to this far past the slide's rear end (owner
    // 2026-11-03 03:25: "extend it about .25 inch past the back of the slide").
    bool finger_pinch = true;
    double finger_pinch_past_rear = 0.64;
    int slide_hold_log = 30;            // slide holds per run that log their frame-by-frame pull
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
    // Rifle in the left hand by its handguard while the pistol is out
    // (vr/interact/rifle_carry.h).
    bool rifle_carry = true;
    double carry_fist_below_bore = 4.0;   // bore this far above the left fist when picked up, cm
    double carry_pitch = 0.0;             // rifle turned in the left hand when picked up, degrees
    double carry_yaw = 0.0;
    double carry_roll = 0.0;
    double rifle_grip_reach = 14.0;       // right hand this close to the rifle's grip takes it, cm
    // Support hand on three grip points (vr/interact/grip_points.h), not
    // wherever it grabs.
    bool grip_points = true;
    double grip_point_inset = 4.5;        // hand centre this far in from the magwell, light and rail end, cm
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
    g_set.light_reach = config::GetFloat("light_hand_reach_cm", 12.f);
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
    g_set.holster_rifle = config::GetBool("holster_rifle", true);
    g_set.holster_toggle = config::GetBool("holster_pistol_grip_toggle", false);
    g_set.holster_cant = config::GetFloat("holster_pistol_cant_deg", 10.f);
    g_set.belt_below_head = config::GetFloat("holster_belt_below_head_cm", 60.f);
    g_set.slide_grab_forward = config::GetFloat("slide_grab_forward_cm", 16.f);
    g_set.rack_trace = config::GetBool("debug_rack_trace", false);
    g_set.slide_pinch_radius = config::GetFloat("slide_pinch_radius_cm", 6.5f);
    g_set.slide_pinch_back = config::GetFloat("slide_pinch_back_cm", 14.f);
    g_set.finger_pinch = config::GetBool("slide_finger_pinch", true);
    g_set.finger_pinch_past_rear = config::GetFloat("slide_finger_pinch_past_rear_cm", 0.64f);
    g_set.slide_hold_log = config::GetInt("slide_hold_log_count", 30);
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
    g_set.rifle_carry = config::GetBool("rifle_left_hand_carry", true);
    g_set.carry_fist_below_bore = config::GetFloat("rifle_carry_fist_below_bore_cm", 4.f);
    g_set.carry_pitch = config::GetFloat("rifle_carry_pitch_deg", 0.f);
    g_set.carry_yaw = config::GetFloat("rifle_carry_yaw_deg", 0.f);
    g_set.carry_roll = config::GetFloat("rifle_carry_roll_deg", 0.f);
    g_set.rifle_grip_reach = config::GetFloat("rifle_grip_reach_cm", 14.f);
    g_set.grip_points = config::GetBool("rifle_grip_points", true);
    g_set.grip_point_inset = std::clamp(config::GetFloat("rifle_grip_point_inset_cm", 4.5f), 0.f, 10.f);
    LoadHolsterSpots();
}

// ---- holster spots ----
// Offsets from the belt come from hole-vr.ini (holster_<name>_forward_cm ...),
// and spots placed in the headset (on any body part, at any angle) are saved
// to hole-vr/holsters.ini, which wins.
using interact::HolsterId;
constexpr int kPistolSpot = static_cast<int>(HolsterId::kPistol);
constexpr int kMagSpot = static_cast<int>(HolsterId::kMag);
constexpr int kRifleSpot = static_cast<int>(HolsterId::kRifle);
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
    static constexpr int kSpots = 3;   // pistol, magazine, rifle
    FVector spot[kSpots];   // where the hand grabs
    FQuat spot_anchor_rot[kSpots];
    int logged_mode = -1;
} g_hw;

const hands::Pose& SpotAnchor(int spot) { return g_hw.anchor[static_cast<int>(g_spots[spot].anchor)]; }

// ---- shared hand state for other systems ----
std::mutex g_hands_mutex;
hands::HandState g_hands[2];
hands::HandGrab g_grab[2];
hands::HandInteraction g_interaction[2];
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

// VR tab requests, served on the game thread once the menu has closed.
std::atomic<bool> g_req_grip_editor{false};
std::atomic<bool> g_req_holsters{false};
double g_editor_y_until = -1;   // g_clock until which left Y is held for the grip editor

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
    // The grip editor opens on left Y held 1 s (or from the VR tab, which
    // holds Y for it for 1.1 s); left stick click held 2 s measures the arms.
    const bool editor_open = hands::EditorHand() >= 0;
    const bool synth_y = g_clock < g_editor_y_until;
    (void)editor_open;
    g_edit_buttons.y_down = synth_y || s.hand[input::kLeft].secondary.down;
    g_edit_buttons.left_stick_pressed = s.hand[input::kLeft].stick_click.pressed;
    g_edit_buttons.left_stick_down = s.hand[input::kLeft].stick_click.down;
    g_hands[0] = h[0];
    g_hands[1] = h[1];
    g_head = head;
    g_head_valid = head_ok;
}

// ---- locomotion and turning ----
bool g_snap_armed = true;
// Sprinting: the game only sprints on (near) full stick, so while sprint is
// on, any push past half way counts as full.
bool g_sprint_full = false;

double Locomotion(UObject player, const input::Snapshot& s, double dt) {
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
        if (g_sprint_full && mag > 0.5) scale = 1.0 / mag;
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
            if (set.Invoke()) return turn;
        }
    }
    return 0;
}

// A stick turn takes effect on the next views (the game camera follows the
// control rotation after the tick), but the hands were mapped before it with
// the latest views' yaw: the gun trailed every turn by that frame's angle
// (perf thread 2026-11-03: 2.2 deg a frame, ~1.7 cm at arm's length). Turn
// this tick's hands and head about the game camera by the same angle.
void TurnHandsWithView(double turn) {
    if (turn == 0) return;
    FVector pivot;
    if (!stereo::GameCameraLocation(&pivot)) return;
    const FQuat q = YawOnly(turn);
    auto turn_pose = [&](hands::Pose& p) {
        p.location = pivot + q.Rotate(p.location - pivot);
        p.rotation = q * p.rotation;
    };
    std::lock_guard<std::mutex> lock(g_hands_mutex);
    for (int i = 0; i < 2; ++i) {
        if (!g_hands[i].valid) continue;
        turn_pose(g_hands[i].grip);
        turn_pose(g_hands[i].aim);
        turn_pose(g_hands[i].wrist);
        g_hands[i].palm_normal = q.Rotate(g_hands[i].palm_normal);
    }
    if (g_head_valid) turn_pose(g_head);
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
    int spawn_fail_logs = 0;
    int wait_logs = 0;
    double refill_pouch_at = 0; // when to put a new magazine in the pouch
    // Magwell diagnostics (owner 2026-11-04: "i cannot reload the AR at all").
    double magwell_log_at = 0;
    bool magwell_point_logged = false;
    UObject dropped_mag = 0;
    FVector dropped_at;
    double dropped_check_at = -1;
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
    // Grip points (vr/interact/grip_points.h): the one the support hand
    // holds (-1: none, the hand is drawn where it grabbed) and where the hand
    // is drawn, gun frame. support_local (where the controller grabbed) still
    // steers the barrel, so taking a point never turns the gun.
    int support_id = -1;
    FVector support_draw;
    interact::LongSqueeze support_squeeze;
    double support_since = 0;
    double support_lost_for = 0;
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
    // Rifle only: the right hand let go of it (the left hand holds it by the
    // handguard, or it is back on the back: vr/interact/rifle_carry.h).
    bool right_let_go = false;
    interact::HolsterDraw holster;
    bool have_release = false;    // gun pose when it left the hand, for the fly-back
    FVector release_loc;
    FQuat release_rot;
    interact::HolsterZone mag_zone;  // left hand at the magazine pouch (haptic tick)
    // Slide hold diagnostics (2026-11-03: grabs from behind caught but let
    // go at 0-20% and the log couldn't say why).
    struct SlideDiag {
        bool have_prev = false;
        FVector prev_hand;          // left controller in slide space, last frame
        double prev_clock = 0;
        int holds = 0;              // holds with this gun
        bool holding = false;
        double grab_clock = 0;
        FVector grab_hand_w, grab_gun_w;
        double hand_moved = 0, gun_moved = 0;  // furthest from where they were at the grab, world cm
        float grip_min = 1.f, grip_last = 0.f, trigger_max = 0.f;
        double needed_cm = 0;       // hand pull a full rack needs
        bool by_fingers = false;    // caught by the visible fingers' pinch, not the controller centre
        std::string pulls, grips;   // per frame, for the first holds
        bool told_blocked = false;  // logged this squeeze's refused slide grab
    } sd;
    interact::SqueezeGate slide_squeeze;  // only a fresh squeeze grabs the slide
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
// ends up pointing where the hand points. Any gun, held or not (the rifle in
// the left hand and the spare pistol use it too); LearnGunFrame keeps it for
// the gun in the right hand.
struct GunFrameResult {
    FQuat frame;                  // gun frame (X barrel, Y right, Z up) in actor space
    double muzzle_dist = 18.0;    // grip to muzzle along the bore, cm
    double bore_height = 6.0;     // bore above the grip point, cm
    bool have_slide_axis = false;
    FVector slide_axis;           // slide/bolt pull direction in its parent's space
    FVector fwd, up;              // for the log
    UObject sight = 0;
    FVector grip;                 // grip point, actor space
};
bool ComputeGunFrame(UObject g, const FVector& actor_loc, const FQuat& actor_rot, const FVector& grip_world,
                     GunFrameResult* out) {
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
    out->frame = FromAxes(fwd, right, up);
    out->muzzle_dist = std::fmax(10.0, DotV(m - grip, fwd));
    out->bore_height = DotV(m - grip, up);
    out->fwd = fwd;
    out->up = up;
    out->sight = sight;
    out->grip = grip;
    // The slide/bolt is pulled straight back along the bore. Express that in
    // the slide's parent space (the pistol's came out -X, as measured in the
    // weapon lab); a rifle's bolt parent may be turned differently.
    out->have_slide_axis = slide_parent != 0;
    if (slide_parent) {
        FVector back_w = actor_rot.Rotate(fwd * -1.0);
        FVector back_p = Norm(ComponentToWorld(slide_parent).rotation.Inverse().Rotate(back_w));
        out->slide_axis = snap(back_p);
    }
    return true;
}

bool LearnGunFrame(UObject g, const FVector& actor_loc, const FQuat& actor_rot, const FVector& grip_world) {
    GunFrameResult r;
    if (!ComputeGunFrame(g, actor_loc, actor_rot, grip_world, &r)) return false;
    W.gun_frame = r.frame;
    W.have_frame = true;
    W.muzzle_dist = r.muzzle_dist;
    W.bore_height = r.bore_height;
    MeasureStock(g, actor_loc, actor_rot, r.grip, r.sight);
    if (r.have_slide_axis) {
        W.slide_axis = r.slide_axis;
        log::Info("controls: slide/bolt pulls along (%.2f %.2f %.2f) in its parent's space", W.slide_axis.X,
                  W.slide_axis.Y, W.slide_axis.Z);
    }
    log::Info("controls: %s barrel along (%.2f %.2f %.2f), top along (%.2f %.2f %.2f) in actor space; muzzle %.1f cm "
              "ahead of and %.1f cm above the grip point",
              ue::GetName(ue::GetClass(g)).c_str(), r.fwd.X, r.fwd.Y, r.fwd.Z, r.up.X, r.up.Y, r.up.Z, W.muzzle_dist,
              W.bore_height);
    return true;
}

struct TArrayRaw {
    const UObject* data;
    int32_t num, max;
};

// The gun's wall push, for the 10 s log line (WallPushReport). Leaf lock:
// nothing else is taken while it is held.
struct WallPushLog {
    long long frames = 0;       // frames the gun was held and checked
    long long pushed = 0;       // frames it was pushed out of something
    double push_max = 0;        // largest push, cm
    long long jumps = 0;        // frames the push changed by over 3 cm
    double jump_max = 0;        // largest change from one frame to the next, cm
    long long eyes_inside = 0;  // frames the eyes were inside something (no push)
    std::string worst_what;     // what the largest push was against
};
std::mutex g_push_log_mutex;
WallPushLog g_push_log;

std::string WallPushReport(double) {
    WallPushLog w;
    {
        std::lock_guard<std::mutex> lock(g_push_log_mutex);
        w = g_push_log;
        g_push_log = {};
    }
    if (w.pushed == 0 && w.eyes_inside == 0) return {};
    char b[400];
    std::snprintf(b, sizeof(b),
                  "gun wall push: the held gun was pushed out of something in %lld of %lld frames (most %.1f cm, "
                  "against %s); the push changed by over 3 cm in a frame %lld times (worst %.1f cm); eyes inside "
                  "something (no push) %lld frames",
                  w.pushed, w.frames, w.push_max, w.worst_what.empty() ? "nothing" : w.worst_what.c_str(), w.jumps,
                  w.jump_max, w.eyes_inside);
    return b;
}

// One line from the eyes to `to` against the world. True with the first
// surface's point and normal (and what it belongs to); false when the way
// is clear. *eyes_inside: the line started inside something.
bool EyeLine(UObject ksl, UObject gps, UObject player, const FVector& from, const FVector& to,
             const std::vector<UObject>& ignore, FVector* at, FVector* normal, UObject* what, bool* eyes_inside) {
    TArrayRaw arr{ignore.data(), static_cast<int32_t>(ignore.size()), static_cast<int32_t>(ignore.size())};
    ue::Call t(ksl, "LineTraceSingle");
    t.Set("WorldContextObject", player).Set("Start", from).Set("End", to)
        .Set("TraceChannel", uint8_t{0}).Set("bTraceComplex", false).Set("ActorsToIgnore", arr)
        .Set("bIgnoreSelf", true);
    if (!t.Invoke() || !t.Result<bool>()) return false;
    int32_t hit_size = 0;
    uint8_t* hit = t.Raw("OutHit", &hit_size);
    if (!hit) return false;
    ue::Call b(gps, "BreakHitResult");
    int32_t in_size = 0;
    uint8_t* in = b.Raw("Hit", &in_size);
    if (!in || in_size != hit_size) return false;
    std::memcpy(in, hit, static_cast<size_t>(hit_size));
    if (!b.Invoke() || !b.Get<bool>("bBlockingHit")) return false;
    if (b.Get<bool>("bInitialOverlap")) {
        *eyes_inside = true;
        return false;
    }
    *at = b.Get<FVector>("ImpactPoint");
    *normal = b.Get<FVector>("ImpactNormal");
    *what = b.Get<UObject>("HitActor");
    return true;
}

// Keeps the held gun out of walls, like Half-Life: Alyx: the line from the
// eyes to the muzzle finds what is in the way, and the whole gun moves out of
// it along that surface (view_lead::WallPushOut; the hand on the gun follows
// it), so against a wall it slides along it instead of coming back toward the
// face. Twice, for a corner between two surfaces. Returns the offset to add
// to the gun's location.
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
    const FVector muzzle = grip + frame.Rotate(FVector{W.muzzle_dist, 0, 0});
    // Not the gun or what the hands hold, and not the forearm menu or wrist
    // HUD (they sit in front of the eyes whenever the left wrist is up).
    std::vector<UObject> ignore{gun};
    if (W.held_mag) ignore.push_back(W.held_mag);
    if (W.pouch_mag) ignore.push_back(W.pouch_mag);
    const hands::HandGrab left = hands::GetGrab(hands::kLeft);
    if (left.object && left.object != gun) ignore.push_back(left.object);
    if (UObject holder = static_cast<UObject>(world_menu::PanelHolder())) ignore.push_back(holder);
    constexpr double kClearanceCm = 1.0, kMaxPushCm = 40.0;
    FVector push;
    bool eyes_inside = false;
    UObject what = 0;
    for (int pass = 0; pass < 2; ++pass) {
        FVector at, normal;
        UObject hit_what = 0;
        if (!EyeLine(ksl, gps, player, head.location, muzzle + push, ignore, &at, &normal, &hit_what, &eyes_inside))
            break;
        const FVector more = view_lead::WallPushOut(muzzle + push, at, normal, kClearanceCm, kMaxPushCm);
        if (Len(more) < 0.01) break;
        push = push + more;
        if (!what) what = hit_what;
    }
    if (Len(push) > kMaxPushCm) push = push * (kMaxPushCm / Len(push));
    // For the log.
    static FVector s_last;
    static double s_last_clock = -1;
    const bool follows = s_last_clock >= 0 && g_clock - s_last_clock < 0.1;
    const double step = follows ? Len(push - s_last) : 0.0;
    s_last = push;
    s_last_clock = g_clock;
    const double size = Len(push);
    bool most = false;
    {
        std::lock_guard<std::mutex> lock(g_push_log_mutex);
        ++g_push_log.frames;
        if (eyes_inside) ++g_push_log.eyes_inside;
        if (size > 0) ++g_push_log.pushed;
        if (step > 3.0) ++g_push_log.jumps;
        if (step > g_push_log.jump_max) g_push_log.jump_max = step;
        most = size > g_push_log.push_max;
        if (most) g_push_log.push_max = size;
    }
    if (most) {
        const std::string name = what ? ue::GetName(what) : std::string("something");
        std::lock_guard<std::mutex> lock(g_push_log_mutex);
        if (g_push_log.push_max == size) g_push_log.worst_what = name;
    }
    return push;
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
    if (two) W.support_point = W.grip_world + hand.Rotate(W.support_id >= 0 ? W.support_draw : W.support_local);
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
    // Input edges pile up between game ticks (vr/input), so one tick can
    // carry a press and a release together, in either order. Outside review
    // 2026-11-04: with release as an else-if after press, a quick tap sent
    // only the press and left the game holding the trigger. Now: a press
    // while the game still holds the trigger (release then press) first lets
    // go; after the press, the game is let go whenever the trigger is up.
    if (rin.trigger_button.pressed) {
        if (W.trigger_sent) {
            send(true);
            W.trigger_sent = false;
        }
        if (W.dead_trigger) {
            log::Info("controls: dead trigger (dry fired, slide not moved since)");
        } else {
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
        }
    }
    if (W.trigger_sent && !rin.trigger_button.down) {
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

    // The visible hand's pinch (thumb tip, index middle knuckle), from the
    // body's last frame, carried by this frame's controller.
    FVector thumb_g, index_g, thumb{}, index{}, pinch{};
    const bool have_fingers = W.is_pistol && cfg.use_span && body::PinchPoint(0, &thumb_g, &index_g);
    double pinch_dist = -1;   // pinch point to the rear-sight line, cm
    if (have_fingers) {
        thumb = ToLocal(pt, lh.grip.location + lh.grip.rotation.Rotate(thumb_g));
        index = ToLocal(pt, lh.grip.location + lh.grip.rotation.Rotate(index_g));
        pinch = (thumb + index) * 0.5;
        // Line from the rear sight back to just past the slide's rear end.
        const double back = std::fmax(0.0, interact::Dot(cfg.span_rear - cfg.sight_center, cfg.axis)) +
                            g_set.finger_pinch_past_rear;
        const double t = std::clamp(interact::Dot(pinch - cfg.sight_center, cfg.axis), 0.0, back);
        pinch_dist = Len(pinch - (cfg.sight_center + cfg.axis * t));
    }
    const bool fingers_in = have_fingers && g_set.finger_pinch && pinch_dist <= g_set.slide_pinch_radius;
    const bool centre_in = W.slide.InZone(hand);
    W.slide.AcceptNextPress(fingers_in);

    // Left hand's speed along the slide (+ = back toward you), cm/s.
    double pull_speed = 0;
    if (W.sd.have_prev && g_clock > W.sd.prev_clock)
        pull_speed = interact::Dot(hand - W.sd.prev_hand, cfg.axis) / (g_clock - W.sd.prev_clock);
    W.sd.prev_hand = hand;
    W.sd.prev_clock = g_clock;
    W.sd.have_prev = true;

    if (lin.grip_button.pressed && W.held_mag == 0 && in_hand) {
        log::Info("controls: left grab at slide-space (%.1f %.1f %.1f), %s the slide zone", hand.X, hand.Y,
                  hand.Z, centre_in ? "inside" : "outside");
        const FVector palm = Norm(pt.rotation.Inverse().Rotate(lh.palm_normal));
        if (Len(hand - cfg.grab_center) > 40.0) {
            // Nowhere near the slide (the pouch, a door): no finger details.
        } else if (have_fingers)
            log::Info("controls: left grab: fingers at slide-space thumb tip (%.1f %.1f %.1f), index knuckle "
                      "(%.1f %.1f %.1f), pinch %.1f cm from the rear sight line (grabs within %.1f%s); controller "
                      "centre %s; palm (%.2f %.2f %.2f); hand moving %.0f cm/s back along the slide; squeeze %.2f",
                      thumb.X, thumb.Y, thumb.Z, index.X, index.Y, index.Z, pinch_dist, g_set.slide_pinch_radius,
                      g_set.finger_pinch ? "" : ", off", centre_in ? "inside" : "outside", palm.X, palm.Y, palm.Z,
                      pull_speed, lin.grip_value);
        else
            log::Info("controls: left grab: fingers not known (%s); palm (%.2f %.2f %.2f); hand moving %.0f cm/s "
                      "back along the slide; squeeze %.2f",
                      !W.is_pistol ? "not a pistol" : !cfg.use_span ? "slide not measured yet" : "VR body hand not posed",
                      palm.X, palm.Y, palm.Z, pull_speed, lin.grip_value);
    }

    using interact::SlideEvent;
    // Only a squeeze that starts with the hand free grabs the slide: one
    // still held from seating a magazine (or from before the pistol was in
    // the other hand) does not, the hand opens and squeezes again.
    const bool grab_down =
        W.slide_squeeze.Update(lin.grip_button.pressed, lin.grip_button.down, W.held_mag == 0 && in_hand);
    if (!W.slide_squeeze.Blocked()) {
        W.sd.told_blocked = false;
    } else if (!W.sd.told_blocked && !W.slide.Held() && (centre_in || fingers_in)) {
        W.sd.told_blocked = true;
        log::Info("controls: slide not grabbed: the hand is %s, but this squeeze began holding a magazine or "
                  "before the pistol was in hand (squeeze %.2f); open the hand and squeeze again to grab it",
                  centre_in ? "in the slide zone" : "at the rear sight by the fingers", lin.grip_value);
    }
    SlideEvent e = W.slide.Update(hand, grab_down, dt);
    // Hold diagnostics: every frame while held, a summary when it ends.
    if (e == SlideEvent::kGrabbed) {
        W.sd.holding = true;
        ++W.sd.holds;
        W.sd.grab_clock = g_clock;
        W.sd.grab_hand_w = lh.grip.location;
        W.sd.grab_gun_w = pt.translation;
        W.sd.hand_moved = W.sd.gun_moved = 0;
        W.sd.grip_min = lin.grip_value;
        W.sd.trigger_max = lin.trigger;
        W.sd.needed_cm = std::fmax(0.0, cfg.full_threshold - W.slide.Travel()) * cfg.length;
        W.sd.by_fingers = fingers_in && !centre_in;
        W.sd.pulls.clear();
        W.sd.grips.clear();
        log::Info("controls: slide hold #%d: grabbed by the %s (controller centre %s, fingers %s); a rack needs "
                  "%.1f cm of pull",
                  W.sd.holds, W.sd.by_fingers ? "fingers' pinch at the rear sight" : "controller centre",
                  centre_in ? "inside" : "outside",
                  !have_fingers ? "not known" : fingers_in ? "at the rear sight" : "not at the rear sight",
                  W.sd.needed_cm);
    } else if (W.sd.holding && W.slide.Held()) {
        W.sd.hand_moved = std::fmax(W.sd.hand_moved, Len(lh.grip.location - W.sd.grab_hand_w));
        W.sd.gun_moved = std::fmax(W.sd.gun_moved, Len(pt.translation - W.sd.grab_gun_w));
        W.sd.grip_min = std::fmin(W.sd.grip_min, lin.grip_value);
        W.sd.trigger_max = std::fmax(W.sd.trigger_max, lin.trigger);
        if (W.sd.holds <= g_set.slide_hold_log && W.sd.pulls.size() < 420) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), " %.1f", W.slide.Stats().last_pull);
            W.sd.pulls += buf;
            std::snprintf(buf, sizeof(buf), " %.2f", lin.grip_value);
            W.sd.grips += buf;
        }
    }
    W.sd.grip_last = lin.grip_value;
    if (W.sd.holding && !W.slide.Held()) {
        W.sd.holding = false;
        const interact::SlideHoldStats& st = W.slide.Stats();
        const char* why = st.slipped                    ? "the hand slipped off (far from the slide and from where it grabbed)"
                          : !in_hand                   ? "the pistol left the hand"
                          : W.held_mag != 0            ? "a magazine was taken"
                          : !lin.grip_button.down      ? "the grip was let go"
                                                       : "unknown";
        log::Info("controls: slide hold #%d ended after %.2f s (%d frames): %s (squeeze now %.2f, lowest %.2f while "
                  "held; holds while above 0.35); hand went back up to %.1f cm and forward up to %.1f cm along the "
                  "slide (a rack needed %.1f), ended at %.1f; sideways up to %.1f cm from where it grabbed (grabbed "
                  "%.1f cm off the slide's line); slide reached %.0f%%; trigger up to %.2f; in the world the hand "
                  "moved up to %.1f cm and the gun %.1f cm; %s",
                  W.sd.holds, g_clock - W.sd.grab_clock, st.frames, why, lin.grip_value, W.sd.grip_min, st.max_pull,
                  -st.min_pull, W.sd.needed_cm, st.last_pull, st.max_drift, st.start_off_axis, W.slide.Peak() * 100,
                  W.sd.trigger_max, W.sd.hand_moved, W.sd.gun_moved,
                  e == SlideEvent::kRacked ? "RACKED" : "not racked");
        if (!W.sd.pulls.empty())
            log::Info("controls: slide hold #%d per frame: pull back (cm)%s | squeeze%s", W.sd.holds,
                      W.sd.pulls.c_str(), W.sd.grips.c_str());
    }
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
        case SlideEvent::kSlipped: {
            // Outside review 2026-11-04: B accepted the slide stop at the
            // catch (about 75 % of travel) but a let-go short of a full
            // rack (90 %) still let the slide close. Past the catch with the
            // lever up, the slide now locks there: the round was pulled out
            // on the way back, as on a real pistol.
            double len = gun.SlideBackLength() > 0.5 ? gun.SlideBackLength() : 4.7;
            double stop = gun.SlideStopLength() > 0 ? gun.SlideStopLength() / len : 0.75;
            if (W.lock_on_release && W.is_pistol && W.slide.Travel() >= stop - 0.05 && !gun.State().slide_locked) {
                W.lock_on_release = false;
                log::Info("controls: slide let go at %.0f%% with the slide stop up, before: %s",
                          W.slide.Travel() * 100, gun.State().Describe().c_str());
                gun.ChargeBoltPull();
                gun.EndBoltCharging();
                ue::SetProp<bool>(gun.Object(), "IsBolt(Slide)Stopped", true);
                W.trace_until = g_clock + 1.5;
                input::Haptic(input::kLeft, 0.6f, 0.04f);
                log::Info("controls: slide locked open at the catch: %s", gun.State().Describe().c_str());
                break;
            }
        }
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

// The pouch magazine's class is read from the gun's own magazine in each
// level. (A class remembered from an earlier level failed to spawn in the
// next one, 2026-11-03: the pouch stayed empty all match.)
void RefillPouch(game::Gun& gun, UObject player, const FVector& loc, const FQuat& rot) {
    if (!W.mag_class) {
        UObject m = 0;
        if (ue::GetProp(gun.Object(), "MagazineObj", m) && m) W.mag_class = ue::GetClass(m);
        else if (ue::GetProp(gun.Object(), "MagazineObj_Temp", m) && m) W.mag_class = ue::GetClass(m);
    }
    if (W.mag_wait_until < 0.0) W.mag_wait_until = g_clock + 5.0;
    UObject mag = 0;
    if (W.mag_class) {
        mag = SpawnMagazine(player, W.mag_class, loc, rot);
        if (mag) {
            log::Info("controls: spawned pouch magazine %s", ue::GetName(mag).c_str());
        } else {
            // Don't keep retrying a class that won't spawn: read it again from
            // the gun's own magazine next time.
            if (W.spawn_fail_logs++ < 5)
                log::Warn("controls: could not spawn a pouch magazine of %s; reading the class from the gun again",
                          ue::GetName(W.mag_class).c_str());
            W.mag_class = 0;
        }
    } else if (!gun.State().has_magazine && g_clock >= W.mag_wait_until) {
        // The game normally gives the gun its magazine just after the level
        // starts; only if it never does, borrow the game's own step. Taking
        // it earlier races the game's own load-in, and the game later moves
        // or destroys that magazine (pouch looks empty for seconds).
        gun.TakeNewMagazine();
        ue::GetProp(gun.Object(), "MagazineObj_Temp", mag);
        if (mag) W.mag_class = ue::GetClass(mag);
        if (mag) log::Info("controls: took pouch magazine from the game (%s)", ue::GetName(mag).c_str());
    } else if (W.wait_logs++ == 0) {
        log::Info("controls: pouch magazine waits for the gun's own magazine (to learn its class)");
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
    if (W.dropped_check_at > 0 && g_clock >= W.dropped_check_at) {
        W.dropped_check_at = -1;
        if (LiveObject(W.dropped_mag)) {
            const FVector now = ActorLocation(W.dropped_mag);
            log::Info("controls: dropped magazine %s moved %.0f cm down, %.0f cm in all, 0.8 s after the drop%s",
                      ue::GetName(W.dropped_mag).c_str(), W.dropped_at.Z - now.Z, Len(now - W.dropped_at),
                      Len(now - W.dropped_at) < 3.0 ? " (it is not falling: floating)" : "");
        }
        W.dropped_mag = 0;
    }
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
    if (!lh.valid) {
        // Outside review 2026-11-04: a grip let go while the left controller
        // had lost tracking was missed (its release edge came and went on a
        // tick that returned here), and the magazine stayed in the hand.
        // Now it drops where it was last shown.
        if (W.held_mag && !W.mag_seating && (lin.grip_button.released || !lin.grip_button.down)) {
            W.mag_engaged = false;
            ue::Call drop(W.held_mag, "DropMagazine");
            drop.Invoke();
            W.held_mag = 0;
            W.refill_pouch_at = g_clock + 0.5;
            log::Info("controls: dropped the held magazine (let go while the left controller had no tracking)");
        }
        return;
    }

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
        if (!W.magwell_point_logged) {
            W.magwell_point_logged = true;
            const FVector rel = W.frame_rot.Inverse().Rotate(seat - W.grip_world);
            const FVector wup = ComponentToWorld(well).rotation.Rotate(FVector{0, 0, 1});
            const FVector wup_g = W.frame_rot.Inverse().Rotate(wup);
            log::Info("controls: %s magwell point %s at (%.1f %.1f %.1f) cm from the grip (forward, right, up); its up "
                      "axis (%.2f %.2f %.2f) in the gun frame; seat %s",
                      W.is_pistol ? "pistol" : "rifle", ue::GetName(well).c_str(), rel.X, rel.Y, rel.Z, wup_g.X,
                      wup_g.Y, wup_g.Z, W.seat_learned ? "learned" : "not learned yet");
        }
        if (!W.mag_engaged && g_clock >= W.magwell_log_at && Len(p) < 35.0) {
            W.magwell_log_at = g_clock + 0.5;
            log::Info("controls: held magazine near the magwell: %.1f cm below it, %.1f cm off its axis, pointing %.0f deg "
                      "off it (lines up within -4..%.0f below, %.1f off, %.0f deg)", along, lateral, aim_off_deg,
                      W.is_pistol ? 14.0 : 16.0, W.is_pistol ? 5.5 : 7.5, W.is_pistol ? 40.0 : 60.0);
        }
        // A rifle's magwell is wider and its magazine is held more freely
        // (owner 2026-11-04: "i cannot reload the AR at all"): more slack.
        const double max_lat = W.is_pistol ? 5.5 : 7.5, max_aim = W.is_pistol ? 40.0 : 60.0;
        const double max_below = W.is_pistol ? 14.0 : 16.0;
        if (!W.mag_engaged) {
            if (along > -4.0 && along < max_below && lateral < max_lat && aim_off_deg < max_aim) {
                W.mag_engaged = true;
                input::Haptic(input::kLeft, 0.25f, 0.02f);
                log::Info("controls: magazine lined up with the magwell (%.1f cm below, %.1f off axis, %.0f deg)",
                          along, lateral, aim_off_deg);
            }
        } else if (lateral > max_lat + 1.5 || along > max_below + 3.0 || aim_off_deg > max_aim + 10.0) {
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
    // Also when the grip is simply up (a let-go missed on some earlier tick).
    if (lin.grip_button.released || !lin.grip_button.down) {
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
        W.dropped_mag = W.held_mag;
        W.dropped_at = W.mag_shown;
        W.dropped_check_at = g_clock + 0.8;
        W.held_mag = 0;
        W.refill_pouch_at = g_clock + 0.5;
        log::Info("controls: dropped the held magazine %s", ue::GetName(W.dropped_mag).c_str());
    }
}

// ---- grip points on a long gun's handguard (vr/interact/grip_points.h) ----
// Measured from the gun's own parts: its handguard mesh and the picatinny
// rails on it, the lower receiver (front of the magwell), and a light or
// laser fitted to it. In the gun frame given (frame_w, grip_w: where the gun
// is now), so they line up with whichever code holds the gun.

// A static mesh component's box in the gun frame. false: no mesh, or an
// empty box.
bool MeshBoxInFrame(UObject c, const FQuat& frame_inv, const FVector& grip_w, FVector* lo, FVector* hi) {
    UObject sm = 0;
    if (!ue::GetProp(c, "StaticMesh", sm) || !sm) return false;
    ue::Call bb(sm, "GetBoundingBox");
    int32_t bsz = 0;
    const uint8_t* b = bb.Invoke() ? bb.Raw("ReturnValue", &bsz) : nullptr;
    if (!b || bsz < 48) return false;
    double v[6];
    std::memcpy(v, b, sizeof(v));
    for (double x : v)
        if (!std::isfinite(x) || std::fabs(x) > 1e5) return false;
    if (v[3] - v[0] < 0.05 && v[4] - v[1] < 0.05 && v[5] - v[2] < 0.05) return false;
    const UETransform w = ComponentToWorld(c);
    for (int k = 0; k < 8; ++k) {
        const FVector corner{k & 1 ? v[3] : v[0], k & 2 ? v[4] : v[1], k & 4 ? v[5] : v[2]};
        const FVector scaled{corner.X * w.scale.X, corner.Y * w.scale.Y, corner.Z * w.scale.Z};
        const FVector f = frame_inv.Rotate(w.translation + w.rotation.Rotate(scaled) - grip_w);
        if (k == 0) *lo = *hi = f;
        *lo = FVector{std::min(lo->X, f.X), std::min(lo->Y, f.Y), std::min(lo->Z, f.Z)};
        *hi = FVector{std::max(hi->X, f.X), std::max(hi->Y, f.Y), std::max(hi->Z, f.Z)};
    }
    return true;
}

// The visible static mesh components of an actor.
std::vector<UObject> MeshComponents(UObject actor) {
    std::vector<UObject> out;
    static UObject smc_class = 0;
    if (!smc_class) smc_class = ue::FindObject("Class /Script/Engine.StaticMeshComponent");
    if (!smc_class || !actor) return out;
    ue::Call list(actor, "K2_GetComponentsByClass");
    list.Set("ComponentClass", smc_class);
    int32_t size = 0;
    const uint8_t* raw = list.Invoke() ? list.Raw("ReturnValue", &size) : nullptr;
    struct Arr { uintptr_t data; int32_t num, max; } arr{};
    if (raw && size >= (int32_t)sizeof(Arr)) std::memcpy(&arr, raw, sizeof(Arr));
    for (int32_t i = 0; i < arr.num && i < 64; ++i) {
        UObject c = mem::ReadOr<uintptr_t>(arr.data + i * 8ull, 0);
        if (!c) continue;
        ue::Call vis(c, "IsVisible");
        if (vis.Invoke() && !vis.Result<bool>()) continue;
        out.push_back(c);
    }
    return out;
}

std::string Lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// Measures the parts; `found` lists what was used, for the log.
interact::GunParts MeasureGunParts(UObject g, const FQuat& frame_w, const FVector& grip_w, std::string* found) {
    interact::GunParts parts;
    const FQuat inv = frame_w.Inverse();
    struct Rail { FVector lo, hi; };
    std::vector<Rail> rails;
    bool have_lower = false, have_receiver = false;
    double receiver_front = 0;
    std::string names;
    char buf[200];
    for (UObject c : MeshComponents(g)) {
        const std::string name = ue::GetName(c);
        const std::string n = Lower(name);
        FVector lo, hi;
        if (!MeshBoxInFrame(c, inv, grip_w, &lo, &hi)) continue;
        if (n.find("handguard") != std::string::npos) {
            if (!parts.have_handguard) {
                parts.hg_lo = lo, parts.hg_hi = hi;
            } else {
                parts.hg_lo = FVector{std::min(parts.hg_lo.X, lo.X), std::min(parts.hg_lo.Y, lo.Y),
                                      std::min(parts.hg_lo.Z, lo.Z)};
                parts.hg_hi = FVector{std::max(parts.hg_hi.X, hi.X), std::max(parts.hg_hi.Y, hi.Y),
                                      std::max(parts.hg_hi.Z, hi.Z)};
            }
            parts.have_handguard = true;
            snprintf(buf, sizeof(buf), "%s handguard X %.1f..%.1f Y %.1f..%.1f Z %.1f..%.1f", names.empty() ? "" : ";",
                     lo.X, hi.X, lo.Y, hi.Y, lo.Z, hi.Z);
            names += buf;
        } else if (n.find("rail") != std::string::npos) {
            rails.push_back({lo, hi});
        } else if (n.find("lowerrec") != std::string::npos) {
            parts.magwell_front = have_lower ? std::max(parts.magwell_front, hi.X) : hi.X;
            have_lower = true;
        } else if ((n.find("receiver") != std::string::npos || n.find("reciver") != std::string::npos) &&
                   n.find("upper") == std::string::npos) {
            receiver_front = have_receiver ? std::max(receiver_front, hi.X) : hi.X;
            have_receiver = true;
        }
    }
    if (parts.have_handguard) {
        // The rails on the handguard (not the receiver's top rail).
        for (const Rail& r : rails) {
            const double mid = 0.5 * (r.lo.X + r.hi.X);
            if (mid < parts.hg_lo.X - 1.0 || mid > parts.hg_hi.X + 1.0) continue;
            parts.rail_front = parts.have_rails ? std::max(parts.rail_front, r.hi.X) : r.hi.X;
            parts.have_rails = true;
        }
        if (parts.have_rails) {
            snprintf(buf, sizeof(buf), "; rails end at %.1f", parts.rail_front);
            names += buf;
        }
    }
    // A shotgun has no lower receiver; its receiver's front is where the
    // handguard starts.
    if (!have_lower && have_receiver) parts.magwell_front = receiver_front;
    parts.have_magwell = have_lower || have_receiver;
    if (parts.have_magwell) {
        snprintf(buf, sizeof(buf), "; %s front at %.1f", have_lower ? "magwell (lower receiver)" : "receiver",
                 parts.magwell_front);
        names += buf;
    }
    // A light or laser fitted to the gun: the rearmost one on the handguard.
    for (const auto& part : game::custom::InstalledParts(g)) {
        const std::string cls = part.actor ? ue::GetName(ue::GetClass(part.actor)) : "";
        const std::string lc = Lower(cls);
        const bool light = part.category == 12 || part.row.rfind("LIGHT_", 0) == 0 ||
                           lc.find("light") != std::string::npos || lc.find("laser") != std::string::npos;
        if (!light) continue;
        bool have = false;
        FVector plo, phi;
        for (UObject c : MeshComponents(part.actor)) {
            FVector lo, hi;
            if (!MeshBoxInFrame(c, inv, grip_w, &lo, &hi)) continue;
            // Skip big helper meshes (a light's beam cone).
            if (hi.X - lo.X > 30 || hi.Y - lo.Y > 30 || hi.Z - lo.Z > 30) continue;
            if (!have) plo = lo, phi = hi;
            plo = FVector{std::min(plo.X, lo.X), std::min(plo.Y, lo.Y), std::min(plo.Z, lo.Z)};
            phi = FVector{std::max(phi.X, hi.X), std::max(phi.Y, hi.Y), std::max(phi.Z, hi.Z)};
            have = true;
        }
        if (!have) continue;
        snprintf(buf, sizeof(buf), "; %s X %.1f..%.1f", cls.c_str(), plo.X, phi.X);
        names += buf;
        if (parts.have_handguard && (plo.X < parts.hg_lo.X - 2.0 || plo.X > parts.hg_hi.X)) continue;
        if (!parts.have_light || plo.X < parts.light_rear) parts.light_rear = plo.X;
        parts.have_light = true;
    }
    *found = names;
    return parts;
}

struct GripPointCache {
    UObject gun = 0;
    double at = -1;
    interact::GripPoints pts;
    std::string logged;
};
// One per gun frame: 0 the gun in the right hand (its frame as HoldGun
// measured it), 1 the rifle the left hand holds alone (rifle carry's frame).
// The two can put the grip point a few cm apart, so each keeps its own.
constexpr int kPointsInHand = 0, kPointsCarry = 1;
GripPointCache g_gpc[2];

// The gun's grip points, measured again at most every 2 s (parts can be
// changed at the workbench). Logged when they change.
const interact::GripPoints& GripPointsFor(int source, UObject g, const FQuat& frame_w, const FVector& grip_w,
                                          double muzzle_dist) {
    static interact::GripPoints none;
    if (!g || !g_set.grip_points) return none;
    GripPointCache* c = &g_gpc[source];
    if (c->gun != g) {
        *c = GripPointCache{};
        c->gun = g;
    }
    if (c->at >= 0 && g_clock - c->at < 2.0) return c->pts;
    c->at = g_clock;
    std::string found;
    const interact::GunParts parts = MeasureGunParts(g, frame_w, grip_w, &found);
    c->pts = interact::MakeGripPoints(parts, muzzle_dist, g_set.grip_point_inset);
    std::string line;
    char buf[160];
    if (c->pts.ok) {
        for (int i = 0; i < interact::kPointCount; ++i) {
            if (!c->pts.have[i]) continue;
            const FVector& p = c->pts.p[i];
            snprintf(buf, sizeof(buf), "%s %s%s (%.1f %.1f %.1f)", line.empty() ? "" : ",", interact::PointName(i),
                     i == interact::kPointLight && !c->pts.light_found ? " (no light: halfway)" : "", p.X, p.Y,
                     p.Z);
            line += buf;
        }
    }
    const std::string msg = c->pts.ok ? "points" + line : "no handguard part found; the support hand holds where it grabs";
    if (msg + found != c->logged) {
        c->logged = msg + found;
        log::Info("grip points: %s (%s): %s; parts:%s", ue::GetName(ue::GetClass(g)).c_str(),
                  source == kPointsInHand ? "in the right hand" : "in the left hand alone", msg.c_str(),
                  found.empty() ? " none recognised" : found.c_str());
    }
    return c->pts;
}

void SupportOff(const char* why) {
    W.two_hand = false;
    W.have_support_point = false;
    log::Info("controls: support hand off the handguard after %.1f s (%s)", g_clock - W.support_since, why);
}

// Rifle support hand: grab the handguard (or the magwell, which many people
// hold) with the left hand and the gun is held in both hands; the barrel
// then points from the right hand toward the left. The hand takes the
// nearest grip point. Let go to return to one hand: the squeeze has to stay
// under 0.2 for 0.12 s (the grip button lets go under 0.35, and in the
// owner's first AR run the hold kept ending after about a second).
void SupportGrip(const hands::HandState& lh, const input::HandInput& lin, double dt) {
    if (W.is_pistol) return;
    // The right hand let go of the rifle: the left hand holds it alone
    // (RifleCarry), there is no two-hand hold.
    if (!W.in_hand) {
        W.two_hand = false;
        W.have_support_point = false;
        return;
    }
    if (W.two_hand) {
        const hands::GripOwner owner = hands::GetInteraction(hands::kLeft).owner;
        char why[128] = "";
        if (W.slide.Held()) {
            snprintf(why, sizeof(why), "the hand took the charging handle");
        } else if (W.held_mag) {
            snprintf(why, sizeof(why), "the hand took a magazine");
        } else if (owner == hands::GripOwner::kWorkbench || owner == hands::GripOwner::kDrink) {
            snprintf(why, sizeof(why), "the hand is used by the %s", owner == hands::GripOwner::kDrink ? "drink" : "workbench");
        } else if (!lh.valid) {
            W.support_lost_for += dt;
            if (W.support_lost_for > 0.25) snprintf(why, sizeof(why), "the left controller lost tracking for 0.25 s");
        } else {
            W.support_lost_for = 0;
            if (!W.support_squeeze.Update(lin.grip_value, static_cast<float>(dt)))
                snprintf(why, sizeof(why), "the squeeze fell to %.2f; lowest %.2f while held", lin.grip_value,
                         W.support_squeeze.lowest);
        }
        if (why[0]) SupportOff(why);
        return;
    }
    if (!lin.grip_button.pressed || !lh.valid || !W.have_pose || W.slide.Held() || W.held_mag) return;
    FVector p = W.frame_rot.Inverse().Rotate(lh.grip.location - W.grip_world);
    int id = -1;
    const interact::GripPoints& gp = GripPointsFor(kPointsInHand, W.gun, W.frame_rot, W.grip_world, W.muzzle_dist);
    if (gp.ok) {
        id = interact::NearestPoint(gp, p);
        log::Info("controls: left grab at gun-frame (%.1f %.1f %.1f), %s", p.X, p.Y, p.Z,
                  id >= 0 ? interact::PointName(id) : "off the handguard");
        if (id < 0) return;
    } else {
        const bool along = p.X > 6.0 && p.X < W.muzzle_dist - 2.0;
        const bool beside = std::fabs(p.Y) < 10.0;
        const bool height = p.Z > W.bore_height - 14.0 && p.Z < W.bore_height + 7.0;
        log::Info("controls: left grab at gun-frame (%.1f %.1f %.1f), %s the handguard zone", p.X, p.Y, p.Z,
                  along && beside && height ? "inside" : "outside");
        if (!(along && beside && height)) return;
    }
    W.two_hand = true;
    W.support_local = p;
    W.support_id = id;
    W.support_draw = id >= 0 ? gp.p[id] : p;
    W.support_squeeze.Start();
    W.support_since = g_clock;
    W.support_lost_for = 0;
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
    double jump_release_at = -1, crouch_release_at = -1;
    // Stick crouch check: 0.4 s after a press the character must be in the
    // stance the press asked for (else Crouch/UnCrouch is called directly),
    // and for 3 s after, a stance change nobody asked for is logged.
    int crouch_want = -1;              // 1 crouched, 0 standing, -1 unknown
    double crouch_check_at = -1, crouch_watch_until = -1;
    bool sprinting = false;
    double sprint_on_at = -1;      // when sprint was switched on
    double sprint_idle_since = -1; // stick in the deadzone since
    bool sprint_moved = false;     // stick left the deadzone since sprint on
    bool sprint_checked = false;   // gait checked after sprint on
    double y_down_at = -1;
    double melee_ready_at = 0;
    double melee_release_at = -1;
    bool light_down = false;     // left trigger is pressing the weapon light
    double jump_block_until = 0; // no jump right after a crouch flick (stick spring-back)
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

// The view's height against the game camera's. Real-height mode: the view
// follows your real head, so the game camera's own drops (its crouch) are
// given back, else crouching for real would lower the view twice. Character
// and seated height: the game camera decides, except when it drops with no
// crouch and stays down (2026-11-03: after two quick punches it sat 25 cm
// low for the rest of the match); then the view is held at standing height.
comfort::CameraHeight g_camh;
UObject g_camh_player = 0;
bool g_crouch_pressed = false;  // the stick crouch fired since the last check
double g_camh_log_at = -1;

// Weapon lab trace (the game's own gun/player events) for a moment.
double g_lab_trace_off_at = -1;
void LabTraceFor(double seconds) {
    if (config::GetBool("lab_trace_on_start", false)) return;
    game::weapon_lab::SetTrace(true);
    g_lab_trace_off_at = std::fmax(g_lab_trace_off_at, g_clock + seconds);
}
void LabTraceTick() {
    if (g_lab_trace_off_at > 0 && g_clock >= g_lab_trace_off_at) {
        g_lab_trace_off_at = -1;
        game::weapon_lab::SetTrace(false);
    }
}

// The game camera against the character's capsule: height above the feet
// and distance from the capsule's middle line, cm.
bool CameraOnCharacter(UObject player, double* rel, double* side) {
    FVector cam;
    if (!stereo::GameCameraLocation(&cam)) return false;
    ue::Call loc(player, "K2_GetActorLocation");
    UObject capsule = 0;
    double half = 0;
    if (ue::GetProp(player, "CapsuleComponent", capsule) && capsule) {
        ue::Call h(capsule, "GetScaledCapsuleHalfHeight");
        if (h.Invoke()) half = h.Result<float>();
    }
    if (!loc.Invoke() || half <= 10.0) return false;
    const FVector at = loc.Result<FVector>();
    *rel = cam.Z - (at.Z - half);
    *side = std::sqrt((cam.X - at.X) * (cam.X - at.X) + (cam.Y - at.Y) * (cam.Y - at.Y));
    return true;
}

// The stick crouch sends the game's own crouch input (press, then release
// 0.1 s later). Owner 2026-11-03: "i cant stay crouched". Whether the game
// crouches on the press or only while it is held is the game's own setting,
// so 0.4 s after each press the character must be in the stance asked for;
// if it is not, Crouch/UnCrouch is called directly (as real crouch does).
// For 3 s after, a stance change nobody asked for is logged.
void StickCrouchCheck(UObject player) {
    static bool direct_called = false;
    if (GA.crouch_check_at > 0 && g_clock >= GA.crouch_check_at) {
        GA.crouch_check_at = -1;
        bool known = false;
        const bool now = GameCrouched(player, &known);
        if (!known || GA.crouch_want < 0) return;
        const bool want = GA.crouch_want == 1;
        if (now == want) {
            log::Info("controls: stick crouch: the character is %s%s", now ? "crouched" : "standing",
                      direct_called ? " (after calling it directly)" : " (the game's own crouch input did it)");
            direct_called = false;
            GA.crouch_watch_until = g_clock + 3.0;
        } else if (!direct_called) {
            ue::Call c(player, want ? "Crouch" : "UnCrouch");
            c.Set("bClientSimulation", false);
            const bool ok = c.Invoke();
            log::Info("controls: stick crouch: the game's crouch input left the character %s; %s", now ? "crouched" : "standing",
                      ok ? (want ? "calling Crouch directly" : "calling UnCrouch directly") : "the character has no Crouch/UnCrouch");
            direct_called = ok;
            if (ok) GA.crouch_check_at = g_clock + 0.4;  // the movement applies it on its next update
        } else {
            log::Info("controls: stick crouch: the character is still %s after calling %s directly (no room, or in the air)",
                      now ? "crouched" : "standing", want ? "Crouch" : "UnCrouch");
            direct_called = false;
        }
        return;
    }
    if (GA.crouch_watch_until > 0) {
        bool known = false;
        const bool now = GameCrouched(player, &known);
        if (g_clock >= GA.crouch_watch_until || !known || GA.crouch_want < 0) {
            GA.crouch_watch_until = -1;
        } else if (now != (GA.crouch_want == 1)) {
            log::Info("controls: stick crouch: the character %s by itself %.1f s after (sprint %s)",
                      now ? "crouched" : "stood up", 3.0 - (GA.crouch_watch_until - g_clock),
                      GA.sprinting ? "on" : "off");
            GA.crouch_watch_until = -1;
        }
    }
}

void CameraHeightView(UObject player, bool crouched, bool known) {
    if (player != g_camh_player) {
        g_camh.Reset();
        g_camh_player = player;
    }
    const bool pressed = g_crouch_pressed;
    g_crouch_pressed = false;
    double rel = 0, side = 0;
    double raise = 0;
    if (CameraOnCharacter(player, &rel, &side)) {
        const bool had = g_camh.HaveStanding(), was = g_camh.Holding();
        ue::Call v(player, "GetVelocity");
        const FVector vel = v.Invoke() ? v.Result<FVector>() : FVector{};
        const bool still = !GA.sprinting && std::sqrt(vel.X * vel.X + vel.Y * vel.Y) < 30.0;
        raise = g_camh.Update(rel, known && crouched, pressed, still, g_set.real_height, g_dt);
        if (!had && g_camh.HaveStanding())
            log::Info("controls: the game camera stands %.0f cm above the character's feet", g_camh.Standing());
        if (g_camh.Holding() != was) {
            if (g_camh.Holding())
                log::Info("controls: the game camera has been %.0f cm below standing height for 2 s standing still with no crouch "
                          "(character %s, %d stick crouch presses, camera %.0f cm from the character's middle); "
                          "your view is held at standing height",
                          g_camh.Drop(), !known ? "stance unknown" : crouched ? "crouched" : "standing",
                          g_camh.CrouchPresses(), side);
            else
                log::Info("controls: the game camera is back at standing height (or you crouched); view follows it "
                          "again");
        }
        // The camera far off the character's middle line is the other half
        // of the same stuck state: logged so the next run shows it.
        if (side > 30.0 && g_clock >= g_camh_log_at) {
            g_camh_log_at = g_clock + 10.0;
            log::Info("controls: the game camera is %.0f cm from the character's middle and %.0f cm above its feet "
                      "(standing %.0f)",
                      side, rel, g_camh.Standing());
        }
    }
    stereo::SetViewRaiseCm(raise);
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
    CameraHeightView(player, crouched, known);
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

// Every other game action on the controllers. No motion gestures (owner's
// rule: no punching or swinging to act). Lean is left out: lean for real.
//   Left stick            move; click: sprint on/off (ends when you stop)
//   Left trigger          weapon light with the left hand right next to the gun, else interact
//   Left Y                tap: switch weapon; hold 1 s: grip editor
//   Right stick           left/right turn, up jump, down crouch / stand; click: kick (fire selector on a rifle)
//   Right trigger         fire with the gun in hand, interact with the hand empty
//   Right A / B           magazine release / slide release with the gun in hand;
//                         switch weapon / objective markers with the hand empty
//   VR tab buttons        grip editor, move holsters, measure arms, measure height
// Is the left hand right next to the gun in the right hand? Distance from
// the left grip to the gun's line, from just behind the grip to the muzzle.
bool LeftHandNearGun() {
    if (!W.gun || !W.in_hand || !W.have_pose) return false;
    if (W.two_hand) return true;
    hands::HandState lh = hands::Get(hands::kLeft);
    if (!lh.valid) return false;
    FVector p = W.frame_rot.Inverse().Rotate(lh.grip.location - W.grip_world);
    const double x = std::clamp(p.X, -8.0, W.muzzle_dist + 2.0);
    const double dz = p.Z - std::clamp(p.Z, -12.0, W.bore_height);   // grip below the bore counts as the gun
    const double d = std::sqrt((p.X - x) * (p.X - x) + p.Y * p.Y + dz * dz);
    return d <= g_set.light_reach;
}

// Interact with the hand `hand` (a trigger press with that hand empty).
// Owner 2026-11-03 15:23: "opening up cardboard boxes is unreliable". In the
// 14:54 run 20 presses at a box went to the HDD the owner was looking at:
// what the head looked at (the game's own target) won unless the hand was
// within ~8 cm of the box, and pointing with the hand never won. Now what
// the hand touches, then what it points at (vr/interact/touch: the things'
// own shapes, then the game's own interact trace from the hand) wins; where
// you look is the fallback. Every interact goes through the game's own
// steps (focus the thing, then its interact input), and the log says what
// happened to the thing a second later ("touch: result of ...").
void HandInteract(UObject player, int hand) {
    // A hand holding a can (vr/drinks): a fist round the can can pull the
    // trigger too, and an interact then would drink it the flat way.
    if (!hands::GetInteraction(static_cast<hands::Hand>(hand)).AllowsControls()) return;
    // One interact per 0.5 s across both hands: some things (the hole's
    // countdown) toggle on each Interact, so a two-hand press cancelled itself.
    // Only after something was used: a press that found nothing doesn't
    // hold up the next try.
    static double s_last = -10.0;
    if (g_clock - s_last < 0.5) {
        log::Info("controls: interact (%s hand) ignored, %.2f s after the last one",
                  hand == input::kLeft ? "left" : "right", g_clock - s_last);
        return;
    }
    UObject game_target = 0;
    ue::GetProp(player, "InteractTarget", game_target);
    if (game_target && !LiveObject(game_target)) game_target = 0;
    std::vector<UObject> ignore;
    for (UObject a : {W.gun, W.held_mag, W.pouch_mag})
        if (a && LiveObject(a)) ignore.push_back(a);
    const touch::Pick pick = touch::PickForHand(player, hand, ignore);
    const char* side = hand == input::kLeft ? "left" : "right";
    const UObject target = pick.actor ? pick.actor : game_target;
    if (!target) {
        // No buzz: a press that buzzes did something.
        log::Info("controls: interact (%s hand): nothing touched, pointed at or looked at", side);
        return;
    }
    if (touch::RecentlyUsed(target, 0.8)) {
        log::Info("controls: interact (%s hand) on %s skipped: used a moment ago", side, ue::GetName(target).c_str());
        return;
    }
    s_last = g_clock;
    input::Haptic(static_cast<input::Hand>(hand), 0.35f, 0.04f);
    char why[96];
    std::snprintf(why, sizeof(why), "%s hand trigger, %s", side, pick.actor ? pick.how : "looking at");
    touch::InteractThroughGame(player, target, why);
}

// ---- physical buttons ----
// Owner 2026-11-03: "the ready button for the shooting range? i want to be
// able to physically press it to start the timer"; then "not just the finger
// tip, any part of the hand or the gun im holding". Either hand, or the gun in
// it, touching the timer's mesh presses it: the timer's own Interact, as the
// game's interact key does. A press needs the hand to come from outside;
// presses 1.5 s apart.
struct PressPart {
    UObject comp = 0;
    FVector mn, mx;   // box in the component's own (unscaled) space
};
struct PressButton {
    UObject actor = 0;
    PressPart parts[3];
    int n_parts = 0;
    bool inside[2] = {false, false};
    // The button body (part 0 when it is the timer Mesh) sinks as a hand or
    // the gun pushes down on its top: squashed from the top, bottom fixed.
    bool body_rest = false;     // rest transform captured
    UETransform rest{};         // world transform of the body at rest
    double depth = 0.0;         // how far it is pushed in now, cm
    bool armed = true;          // pushed back up since the last press
    // Owner 2026-11-03 14:35: "i only want the red button its self to react,
    // not its square base". The game's timer is one mesh, so it is hidden and
    // drawn again from engine shapes: a dark base and a red cap that sinks.
    UObject base_comp = 0, cap_comp = 0;
    FVector center_l;           // footprint centre, rest-local, scaled cm
    double bottom_l = 0, base_h = 0, cap_h = 0, cap_d = 0;
};

// The timer mesh's GetLocalBounds came back 0 x 0 x 0 in the owner's 10:56
// run, so the shape comes from the static mesh asset's own bounding box
// (StaticMesh.GetBoundingBox), for the timer body and its display; the
// interact Box (BoxExtent) only if neither has one.
bool StaticMeshBox(UObject comp, FVector* mn, FVector* mx) {
    UObject sm = 0;
    if (!comp || !ue::GetProp(comp, "StaticMesh", sm) || !sm) return false;
    ue::Call c(sm, "GetBoundingBox");
    if (!c.Invoke()) return false;
    int32_t size = 0;
    const uint8_t* raw = c.Raw("ReturnValue", &size);
    if (!raw || size < 48) return false;
    double v[6];
    std::memcpy(v, raw, sizeof(v));
    *mn = FVector{v[0], v[1], v[2]};
    *mx = FVector{v[3], v[4], v[5]};
    return v[3] - v[0] > 0.5 || v[4] - v[1] > 0.5 || v[5] - v[2] > 0.5;
}

void BuildPressParts(PressButton& b) {
    b.n_parts = 0;
    for (const char* name : {"Mesh", "timerDisplay"}) {
        UObject comp = 0;
        ue::GetProp(b.actor, name, comp);
        PressPart part;
        if (comp && StaticMeshBox(comp, &part.mn, &part.mx)) {
            part.comp = comp;
            b.parts[b.n_parts++] = part;
        }
    }
    if (b.n_parts == 0) {
        UObject box = 0;
        ue::Vec3d e{};
        if (ue::GetProp(b.actor, "Box", box) && box && ue::GetProp(box, "BoxExtent", e) && e.x > 0) {
            PressPart part;
            part.comp = box;
            part.mn = FVector{-e.x, -e.y, -e.z};
            part.mx = FVector{e.x, e.y, e.z};
            b.parts[b.n_parts++] = part;
        }
    }
    static int logs = 0;
    if (logs++ < 4) {
        for (int i = 0; i < b.n_parts; ++i) {
            const UETransform t = ComponentToWorld(b.parts[i].comp);
            const PressPart& q = b.parts[i];
            log::Info("controls: range timer part %s: %.1f x %.1f x %.1f cm at (%.0f %.0f %.0f)",
                      ue::GetName(q.comp).c_str(), (q.mx.X - q.mn.X) * t.scale.X, (q.mx.Y - q.mn.Y) * t.scale.Y,
                      (q.mx.Z - q.mn.Z) * t.scale.Z, t.translation.X, t.translation.Y, t.translation.Z);
        }
        if (b.n_parts == 0) log::Warn("controls: range timer has no shape to press");
    }
}
struct {
    // The timer is a home-level prop: its class is freed when you leave home
    // and comes back at a new address. Kept as an ObjectRef, which notices
    // (the 2026-11-03 14:02 run crashed passing the freed class on).
    ue::ObjectRef cls{
        "BlueprintGeneratedClass /Game/Props/00_Home/ShootingTimer/BP_ShootingTimer.BP_ShootingTimer_C"};
    UObject last_cls = 0;
    double next_refresh = 0;
    double ready_at = 0;
    std::vector<PressButton> buttons;
} g_press;

void RefreshPressButtons(UObject player) {
    if (g_clock < g_press.next_refresh) return;
    g_press.next_refresh = g_clock + 2.0;
    const UObject cls = g_press.cls.Get();
    if (cls != g_press.last_cls) {
        g_press.last_cls = cls;
        if (cls) log::Info("controls: shooting range timer class found; its button can be pressed by hand");
    }
    if (!cls) {
        g_press.buttons.clear();
        return;
    }
    static UObject gps = 0;
    if (!gps) gps = ue::FindObject("GameplayStatics /Script/Engine.Default__GameplayStatics");
    if (!gps) return;
    ue::Call all(gps, "GetAllActorsOfClass");
    all.Set("WorldContextObject", player).Set("ActorClass", cls);
    std::vector<PressButton> found;
    if (all.Invoke()) {
        int32_t size = 0;
        const uint8_t* raw = all.Raw("OutActors", &size);
        struct Arr { uintptr_t data; int32_t num, max; } arr{};
        if (raw && size >= (int32_t)sizeof(Arr)) std::memcpy(&arr, raw, sizeof(Arr));
        for (int i = 0; i < arr.num && i < 16; ++i) {
            UObject a = mem::ReadOr<uintptr_t>(arr.data + i * sizeof(uintptr_t), 0);
            if (!a) continue;
            PressButton b;
            b.actor = a;
            bool known = false;
            for (const PressButton& old : g_press.buttons)
                if (old.actor == a) { b = old; known = true; }
            if (!known) BuildPressParts(b);
            if (b.n_parts == 0) continue;
            found.push_back(b);
        }
    }
    g_press.buttons.swap(found);
}

// Owner 2026-11-03 14:14: "as i push the button down i want the red part to
// move down as i push it". The timer's red top is the Mesh part's top: how
// far a hand or gun ball reaches below the top (over its footprint) sinks
// it, up to kPressTravel; past kPressFire it presses. Side touches still
// press at once (owner: any part of the hand or gun).
constexpr double kPressTravel = 3.0;
constexpr double kPressFire = 1.8;
constexpr double kPressRearm = 0.6;

// One engine-shape piece of the rebuilt timer: a 100 cm Cube or Cylinder
// (pivot at its centre) placed in world space, no collision, plain colour.
UObject MakeTimerShape(UObject actor, const char* mesh_path, float r, float g, float bl) {
    static UObject cls = ue::FindObject("Class /Script/Engine.StaticMeshComponent");
    static UObject mat = ue::FindObject("Material /Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");
    static ue::FName color{};
    static bool have_color = ue::names::Find("Color", &color);
    UObject mesh = ue::FindObject(mesh_path);
    if (!cls || !mesh || !actor) return 0;
    ue::Call add(actor, "AddComponentByClass");
    add.Set("Class", cls).Set("bManualAttachment", true).Set("RelativeTransform", UETransform{})
        .Set("bDeferredFinish", false);
    UObject c = add.Invoke() ? add.Result<UObject>() : 0;
    if (!c) return 0;
    ue::Call col(c, "SetCollisionEnabled");
    col.Set("NewType", uint8_t{0});   // NoCollision
    col.Invoke();
    ue::Call ov(c, "SetGenerateOverlapEvents");
    ov.Set("bInGenerateOverlapEvents", false);
    ov.Invoke();
    ue::Call mob(c, "SetMobility");
    mob.Set("NewMobility", uint8_t{2});
    mob.Invoke();
    ue::Call sm(c, "SetStaticMesh");
    sm.Set("NewMesh", mesh);
    sm.Invoke();
    if (mat && have_color) {
        ue::Call mid(c, "CreateDynamicMaterialInstance");
        mid.Set("ElementIndex", int32_t{0}).Set("SourceMaterial", mat);
        UObject m = mid.Invoke() ? mid.Result<UObject>() : 0;
        if (m) {
            struct { float r, g, b, a; } lc{r, g, bl, 1.0f};
            ue::Call v(m, "SetVectorParameterValue");
            v.Set("ParameterName", color).Set("Value", lc);
            v.Invoke();
        }
    } else {
        log::Warn("controls: range timer shapes: no BasicShapeMaterial colour (material %s)", mat ? "found" : "missing");
    }
    return c;
}

void PlaceTimerShape(const PressButton& b, UObject comp, const FVector& local_center, const FVector& size_cm) {
    UETransform t;
    t.rotation = b.rest.rotation;
    t.translation = b.rest.translation + b.rest.rotation.Rotate(local_center);
    t.scale = FVector{size_cm.X / 100.0, size_cm.Y / 100.0, size_cm.Z / 100.0};
    ue::Call c(comp, "K2_SetWorldTransform");
    c.Set("NewTransform", t).Set("bSweep", false).Set("bTeleport", true);
    c.Invoke();
}

void PlaceTimerCap(const PressButton& b, double depth) {
    if (!b.cap_comp) return;
    // A dome: a sphere centred on the base's top, its lower half inside the
    // base (and the table under it).
    const FVector at{b.center_l.X, b.center_l.Y, b.bottom_l + b.base_h - depth};
    PlaceTimerShape(b, b.cap_comp, at, FVector{b.cap_d, b.cap_d, b.cap_h * 2.0});
}

// Rest pose known: hide the game's one-piece timer and draw base + cap.
// Matched to the owner's 2026-11-04 picture of the game's own timer
// (capture-timer-original.bmp): a light blue square block, half the height,
// with a red dome about 80 % of its width on top. On any failure the old
// squash stays.
void RebuildTimerShapes(PressButton& b) {
    const PressPart& p = b.parts[0];
    const FVector sc{std::fabs(b.rest.scale.X), std::fabs(b.rest.scale.Y), std::fabs(b.rest.scale.Z)};
    const double w = (p.mx.X - p.mn.X) * sc.X, d = (p.mx.Y - p.mn.Y) * sc.Y, h = (p.mx.Z - p.mn.Z) * sc.Z;
    if (w < 2 || d < 2 || h < 4) return;
    b.center_l = FVector{(p.mn.X + p.mx.X) * 0.5 * sc.X, (p.mn.Y + p.mx.Y) * 0.5 * sc.Y, 0};
    b.bottom_l = p.mn.Z * sc.Z;
    b.base_h = h * 0.5;
    b.cap_h = h - b.base_h;
    b.cap_d = std::fmin(w, d) * 0.8;
    b.base_comp = MakeTimerShape(b.actor, "StaticMesh /Engine/BasicShapes/Cube.Cube", 0.06f, 0.42f, 0.75f);
    b.cap_comp = b.base_comp ? MakeTimerShape(b.actor, "StaticMesh /Engine/BasicShapes/Sphere.Sphere", 0.45f, 0.012f, 0.02f) : 0;
    if (!b.base_comp || !b.cap_comp) {
        log::Warn("controls: range timer: could not build the base and red cap; the whole timer squashes instead");
        b.base_comp = b.cap_comp = 0;
        return;
    }
    PlaceTimerShape(b, b.base_comp, FVector{b.center_l.X, b.center_l.Y, b.bottom_l + b.base_h * 0.5},
                    FVector{w, d, b.base_h});
    PlaceTimerCap(b, 0.0);
    ue::Call vis(p.comp, "SetVisibility");
    vis.Set("bNewVisibility", false).Set("bPropagateToChildren", false);
    vis.Invoke();
    log::Info("controls: range timer rebuilt: blue base %.1f x %.1f x %.1f cm, red dome %.1f cm wide, %.1f cm tall; only the dome sinks",
              w, d, b.base_h, b.cap_d, b.cap_h);
}

void SetBodySquash(PressButton& b, double depth) {
    if (!b.body_rest || b.n_parts == 0) return;
    if (b.cap_comp) {
        PlaceTimerCap(b, depth);
        return;
    }
    UObject comp = b.parts[0].comp;
    const UETransform& t = b.rest;
    const double sz = std::fabs(t.scale.Z);
    const double h = (b.parts[0].mx.Z - b.parts[0].mn.Z) * sz;
    if (h < 2.0) return;
    const double k = std::fmax(0.5, (h - depth) / h);
    // Keep the bottom where it was: bottom = loc + up * mn.Z * scale.
    const FVector up = t.rotation.Rotate(FVector{0, 0, 1});
    const FVector loc = t.translation + up * (b.parts[0].mn.Z * sz * (1.0 - k));
    ue::Call sl(comp, "K2_SetWorldLocation");
    sl.Set("NewLocation", ue::Vec3d{loc.X, loc.Y, loc.Z}).Set("bSweep", false).Set("bTeleport", true);
    sl.Invoke();
    ue::Call ss(comp, "SetWorldScale3D");
    ss.Set("NewScale", ue::Vec3d{t.scale.X, t.scale.Y, t.scale.Z * k});
    ss.Invoke();
}

// Owner 2026-11-03 15:23: "make a second pass at the modeling make it look
// 1:1 like the original". The mod can't open the game's model, so it takes
// pictures: once a run, when the player stands within 1 m looking at the
// timer with no hand near it, the game's own timer is shown for a moment
// and saved (capture-timer-original.bmp), then the rebuilt one
// (capture-timer-rebuilt.bmp), to model the next version from.
void SetTimerShown(PressButton& b, bool original) {
    auto vis = [](UObject c, bool on) {
        if (!c) return;
        ue::Call v(c, "SetVisibility");
        v.Set("bNewVisibility", on).Set("bPropagateToChildren", false);
        v.Invoke();
    };
    vis(b.parts[0].comp, original);
    vis(b.base_comp, !original);
    vis(b.cap_comp, !original);
}

void TimerLookCapture(PressButton& b) {
    static int stage = 0;   // 0 waiting, 1 original shown, 2 original saved, 3 rebuilt saved, 4 done
    static double at = 0;
    if (stage >= 4 || !b.cap_comp) return;
    if (stage == 0) {
        hands::Pose head;
        if (!hands::Head(&head) || g_clock < 20.0) return;
        const FVector top = b.rest.translation + b.rest.rotation.Rotate(
                                FVector{b.center_l.X, b.center_l.Y, b.bottom_l + b.base_h});
        const FVector to = top - head.location;
        const double dist = std::sqrt(to.X * to.X + to.Y * to.Y + to.Z * to.Z);
        if (dist > 100.0 || dist < 25.0) return;
        const FVector fwd = head.rotation.Rotate(FVector{1, 0, 0});
        if ((fwd.X * to.X + fwd.Y * to.Y + fwd.Z * to.Z) / dist < std::cos(20.0 * kDegToRad)) return;
        for (int h = 0; h < 2; ++h) {
            const hands::HandState hs = hands::Get(h == 0 ? hands::kLeft : hands::kRight);
            const FVector d = hs.grip.location - top;
            if (hs.valid && std::sqrt(d.X * d.X + d.Y * d.Y + d.Z * d.Z) < 30.0) return;
        }
        SetTimerShown(b, true);
        stage = 1;
        at = g_clock + 0.15;
        log::Info("controls: range timer: taking a picture of the game's own timer from %.0f cm (for modelling)", dist);
        return;
    }
    if (g_clock < at) return;
    if (stage == 1) {
        xr::RequestCapture("timer-original");
        stage = 2;
        at = g_clock + 0.25;
    } else if (stage == 2) {
        SetTimerShown(b, false);
        stage = 3;
        at = g_clock + 0.15;
    } else if (stage == 3) {
        xr::RequestCapture("timer-rebuilt");
        stage = 4;
        log::Info("controls: range timer: pictures saved (capture-timer-original.bmp, capture-timer-rebuilt.bmp)");
    }
}

void PhysicalButtons(UObject player) {
    RefreshPressButtons(player);
    for (PressButton& b : g_press.buttons) {
        if (!LiveObject(b.actor)) continue;
        UETransform tr[3];
        for (int i = 0; i < b.n_parts; ++i) tr[i] = ComponentToWorld(b.parts[i].comp);
        // The body's rest pose: captured once, while it is not pushed in.
        UObject body = 0;
        ue::GetProp(b.actor, "Mesh", body);
        const bool can_sink = b.n_parts > 0 && body && b.parts[0].comp == body;
        if (can_sink && !b.body_rest) {
            const FVector up = tr[0].rotation.Rotate(FVector{0, 0, 1});
            if (up.Z > 0.95) {
                ue::Call m(body, "SetMobility");
                m.Set("NewMobility", uint8_t{2});   // Movable
                m.Invoke();
                b.rest = tr[0];
                b.body_rest = true;
                RebuildTimerShapes(b);
                UObject sm = 0;
                ue::GetProp(body, "StaticMesh", sm);
                log::Info("controls: range timer button sinks when pushed (mesh %s, %.1f cm tall)",
                          sm ? ue::GetName(sm).c_str() : "?",
                          (b.parts[0].mx.Z - b.parts[0].mn.Z) * std::fabs(tr[0].scale.Z));
            }
        }
        if (b.body_rest) tr[0] = b.rest;   // test against the button at rest
        double push = 0.0;                 // deepest push from above, both hands
        int push_hand = -1;
        for (int h = 0; h < 2; ++h) {
            const hands::HandState hs = hands::Get(h == 0 ? hands::kLeft : hands::kRight);
            if (!hs.valid) { b.inside[h] = false; continue; }
            // Any part of the hand (owner: "not just the finger tip, any part
            // of the hand or the gun im holding"): wrist, palm, knuckles and
            // fingertip as 4.5 cm balls; the gun in this hand along its bore
            // and slide as 2.5 cm balls.
            struct Ball { FVector p; double r; };
            Ball pts[16];
            int n = 0;
            const FQuat ar = hs.aim.rotation;
            // Owner 2026-11-03 15:23: "the hit boxes aren't very precise".
            // The hand the owner sees (vr/body, after hand walls and finger
            // give) when it is drawn: five fingertips and the palm, which
            // sits as a 3 cm ball just inside the palm skin. Else the
            // controller's rough balls.
            body::HandContacts hc;
            if (body::GetHandContacts(h, &hc)) {
                const double tr = std::fmax(hc.tip_radius, 0.8);
                for (int f = 0; f < 5; ++f) pts[n++] = {hc.tip[f], tr};
                pts[n++] = {hc.palm - hc.palm_out * 2.5, 3.0};
            } else {
                pts[n++] = {hs.wrist.location, 4.5};
                pts[n++] = {hs.grip.location, 4.5};
                pts[n++] = {hs.grip.location + ar.Rotate(FVector{5, 0, 0}), 4.5};
                pts[n++] = {hs.grip.location + ar.Rotate(FVector{9, 0, 0}), 3.0};
            }
            const bool gun_here = h == 1 && W.gun && W.in_hand && W.have_pose;
            if (gun_here) {
                for (double x = -4.0; x <= W.muzzle_dist + 0.1 && n < 16; x += 4.0)
                    pts[n++] = {W.grip_world + W.frame_rot.Rotate(FVector{x, 0, W.bore_height}), 2.5};
            }
            // Side touch: a ball overlaps a part but is not over the body's top.
            bool side = false;
            double hand_push = 0.0;
            for (int k = 0; k < n; ++k) {
                const Ball& bl = pts[k];
                for (int i = 0; i < b.n_parts; ++i) {
                    const UETransform& t = tr[i];
                    const FVector& mn = b.parts[i].mn;
                    const FVector& mx = b.parts[i].mx;
                    const FVector l = t.rotation.Inverse().Rotate(bl.p - t.translation);
                    const double sx = std::fabs(t.scale.X), sy = std::fabs(t.scale.Y), sz = std::fabs(t.scale.Z);
                    bool over_xy = l.X > mn.X * sx && l.X < mx.X * sx && l.Y > mn.Y * sy && l.Y < mx.Y * sy;
                    if (i == 0 && b.cap_comp) {
                        // Only the red cap is pushed down from above.
                        const double rx = l.X - b.center_l.X, ry = l.Y - b.center_l.Y;
                        over_xy = rx * rx + ry * ry < std::pow(b.cap_d * 0.5 + bl.r * 0.5, 2);
                    }
                    const double top = mx.Z * sz;
                    if (i == 0 && b.body_rest && over_xy && l.Z > top - kPressTravel) {
                        // Over the top: how far the ball's bottom is below it.
                        hand_push = std::fmax(hand_push, top - (l.Z - bl.r));
                        continue;
                    }
                    // Rebuilt timer: above the base only the cap's own sides count.
                    const double side_top = i == 0 && b.cap_comp && !over_xy ? b.bottom_l + b.base_h : top;
                    if (l.X > mn.X * sx - bl.r && l.X < mx.X * sx + bl.r && l.Y > mn.Y * sy - bl.r &&
                        l.Y < mx.Y * sy + bl.r && l.Z > mn.Z * sz - bl.r && l.Z < side_top + bl.r)
                        side = true;
                }
            }
            if (hand_push > push) {
                push = hand_push;
                push_hand = h;
            }
            if (side && !b.inside[h] && g_clock >= g_press.ready_at) {
                g_press.ready_at = g_clock + 1.5;
                bool on = false;
                ue::GetProp(b.actor, "IsTimerON", on);
                ue::Call c(b.actor, "Interact");
                c.Set("Actor", player).Set("SelectIndex", int32_t{0});
                const bool ok = c.Invoke();
                input::Haptic(h == 0 ? input::kLeft : input::kRight, 0.6f, 0.05f);
                log::Info("controls: %s hand touched the range timer (timer was %s)%s",
                          h == 0 ? "left" : "right", on ? "running" : "stopped", ok ? "" : ", call failed");
            }
            b.inside[h] = side;
        }
        if (!b.body_rest) continue;
        TimerLookCapture(b);
        // The red top follows the push, and springs back up when let go.
        const double target = std::clamp(push, 0.0, kPressTravel);
        const double back = 25.0 * g_dt;   // cm per tick going up
        const double d = target >= b.depth ? target : std::fmax(target, b.depth - back);
        if (std::fabs(d - b.depth) > 0.01) {
            b.depth = d;
            SetBodySquash(b, d);
        }
        if (b.depth < kPressRearm) b.armed = true;
        if (b.armed && b.depth >= kPressFire && push_hand >= 0 && g_clock >= g_press.ready_at) {
            b.armed = false;
            g_press.ready_at = g_clock + 1.5;
            bool on = false;
            ue::GetProp(b.actor, "IsTimerON", on);
            ue::Call c(b.actor, "Interact");
            c.Set("Actor", player).Set("SelectIndex", int32_t{0});
            const bool ok = c.Invoke();
            input::Haptic(push_hand == 0 ? input::kLeft : input::kRight, 0.7f, 0.06f);
            log::Info("controls: %s hand pushed the range timer button down %.1f cm (timer was %s)%s",
                      push_hand == 0 ? "left" : "right", b.depth, on ? "running" : "stopped",
                      ok ? "" : ", call failed");
        }
    }
}

// Reads a (possibly bitfield) bool property. -1 if it can't be read.
int ReadBoolProp(UObject obj, const char* name) {
    auto p = ue::reflection::FindProperty(ue::GetClass(obj), name);
    if (!p.valid() || p.type != "BoolProperty") return -1;
    uint8_t b = 0;
    if (!mem::Read(obj + p.offset, b)) return -1;
    return (b & (p.bool_mask ? p.bool_mask : 0xFF)) ? 1 : 0;
}

// The game's sprint input: with its "toggle sprint" option on, each press
// flips sprinting and a release does nothing, so the mod's on/off would get
// out of step. Then each change is a press and release.
void SendSprint(UObject player, bool on) {
    const bool toggle = ReadBoolProp(player, "ToggleSprint") == 1;
    if (toggle) {
        // Already in the asked-for gait (the game ends a sprint by itself
        // when you stop): a press would flip it the wrong way.
        uint8_t gait = 255;
        if (ue::GetProp(player, "Gait", gait) && gait <= 3 && (gait == 2) == on) return;
        FireEvent(player, GA.sprint_down);
        FireEvent(player, GA.sprint_up);
    } else {
        FireEvent(player, on ? GA.sprint_down : GA.sprint_up);
    }
}

void StopSprint(UObject player, const char* why) {
    if (!GA.sprinting) return;
    GA.sprinting = false;
    g_sprint_full = false;
    SendSprint(player, false);
    log::Info("controls: sprint off (%s, after %.1f s)", why, g_clock - GA.sprint_on_at);
}

void SprintControl(UObject player, const input::HandInput& l) {
    const double mag = std::sqrt(l.stick_x * l.stick_x + l.stick_y * l.stick_y);
    if (l.stick_click.pressed) {
        if (GA.sprinting) {
            StopSprint(player, "left stick click");
        } else {
            GA.sprinting = true;
            g_sprint_full = true;
            GA.sprint_on_at = g_clock;
            GA.sprint_idle_since = -1;
            GA.sprint_moved = mag >= g_set.deadzone;
            GA.sprint_checked = false;
            SendSprint(player, true);
            log::Info("controls: sprint on (left stick click; stick at %.2f; game toggle-sprint option %s)", mag,
                      ReadBoolProp(player, "ToggleSprint") == 1 ? "on" : "off");
        }
    }
    if (!GA.sprinting) return;
    if (mag >= g_set.deadzone) {
        GA.sprint_moved = true;
        GA.sprint_idle_since = -1;
    } else {
        if (GA.sprint_idle_since < 0) GA.sprint_idle_since = g_clock;
        const bool waiting_for_push = !GA.sprint_moved && g_clock - GA.sprint_on_at < 1.0;
        if (!waiting_for_push && g_clock - GA.sprint_idle_since >= 0.35) StopSprint(player, "stopped");
    }
    // Diagnostic: 0.6 s into a sprint, is the character really sprinting?
    if (GA.sprinting && !GA.sprint_checked && GA.sprint_moved && g_clock - GA.sprint_on_at > 0.6) {
        GA.sprint_checked = true;
        uint8_t gait = 255;
        ue::GetProp(player, "Gait", gait);
        ue::Call v(player, "GetVelocity");
        const FVector vel = v.Invoke() ? v.Result<FVector>() : FVector{};
        // The game may only sprint when moving roughly where the view
        // (control rotation) faces: log that angle too.
        FRotator ctrl{};
        ue::Call gc(player, "GetController");
        if (UObject c = gc.Invoke() ? gc.Result<UObject>() : 0) {
            ue::Call r(c, "GetControlRotation");
            if (r.Invoke()) ctrl = r.Result<FRotator>();
        }
        double off = std::atan2(vel.Y, vel.X) / kDegToRad - ctrl.Yaw;
        while (off > 180) off -= 360;
        while (off < -180) off += 360;
        log::Info("controls: sprint check: game gait %d (2 = sprinting), speed %.0f cm/s, stick %.2f, moving %.0f deg "
                  "off the view's facing", gait, std::sqrt(vel.X * vel.X + vel.Y * vel.Y), mag, off);
    }
}

void GameActions(UObject player, const input::Snapshot& s) {
    if (!player) return;
    LabTraceTick();
    const input::HandInput& l = s.hand[input::kLeft];
    const input::HandInput& r = s.hand[input::kRight];
    const bool menu_open = ui_panel::MenuOpen();
    if (!menu_open && g_req_grip_editor.exchange(false)) {
        g_editor_y_until = g_clock + 1.1;   // the editor toggles on a 1 s hold of Y
        log::Info("controls: opening/closing the grip editor (asked for from the VR tab)");
    }
    if (menu_open || hands::EditorHand() >= 0) {
        if (GA.sprinting) StopSprint(player, menu_open ? "menu" : "grip editor");
        if (GA.light_down) {
            ue::Call c(player, "PlayerInput_SwitchLight");
            c.Set("IsPressed", false);
            c.Invoke();
            GA.light_down = false;
        }
        return;
    }
    PhysicalCrouchAndJump(player);
    PhysicalButtons(player);
    // Left trigger: the weapon light when the left hand is right next to the
    // gun in your hand, otherwise interact. The light press lasts as long as
    // the trigger is held (the game toggles on the press).
    if (l.trigger_button.pressed && !W.held_mag && !W.slide.Held()) {
        if (LeftHandNearGun()) {
            ue::Call c(player, "PlayerInput_SwitchLight");
            c.Set("IsPressed", true);
            c.Invoke();
            GA.light_down = true;
            input::Haptic(input::kLeft, 0.25f, 0.02f);
            log::Info("controls: weapon light (left trigger, hand at the gun)");
        } else {
            HandInteract(player, input::kLeft);
        }
    }
    if (GA.light_down && !l.trigger_button.down) {
        ue::Call c(player, "PlayerInput_SwitchLight");
        c.Set("IsPressed", false);
        c.Invoke();
        GA.light_down = false;
    }
    // Right trigger interacts too while the right hand is empty (pistol
    // holstered, or no gun); with the gun in hand it only fires.
    if (r.trigger_button.pressed && !W.in_hand) HandInteract(player, input::kRight);
    // No gestures (owner's rule): every action is one press of one button,
    // and the combat ones never need the left thumb off the move stick.
    const bool gun_in_hand = W.gun && W.in_hand;
    // Right stick up: jump. Right stick down: crouch / stand.
    const bool up = r.stick_y > 0.75f && std::fabs(r.stick_x) < 0.6f;
    const bool down = r.stick_y < -0.75f && std::fabs(r.stick_x) < 0.6f;
    // A crouch flick can spring the stick back past the top: no jump for
    // 0.5 s after a crouch press.
    if (up && GA.jump_armed && GA.jump_release_at < 0 && g_clock >= GA.jump_block_until) {
        GA.jump_armed = false;
        FireEvent(player, GA.jump_down);
        GA.jump_release_at = g_clock + 0.15;
        log::Info("controls: jump (right stick up)");
    }
    if (down && GA.crouch_armed && GA.crouch_release_at < 0) {
        GA.crouch_armed = false;
        GA.jump_block_until = g_clock + 0.5;
        bool known = false;
        const bool was = GameCrouched(player, &known);
        // Crouching ends a sprint, as it does in most games.
        if (known && !was && GA.sprinting) {
            StopSprint(player, "crouching");
        }
        FireEvent(player, GA.crouch_down);
        g_crouch_pressed = true;
        GA.crouch_release_at = g_clock + 0.1;
        GA.crouch_want = known ? (was ? 0 : 1) : -1;
        GA.crouch_check_at = g_clock + 0.4;
        GA.crouch_watch_until = -1;
        log::Info("controls: %s (right stick down)", !known ? "crouch / stand" : was ? "stand up" : "crouch");
    }
    if (r.stick_y < 0.4f) GA.jump_armed = true;
    if (r.stick_y > -0.4f) GA.crouch_armed = true;
    // Left Y tap: next weapon (holding Y 1 s is the grip editor).
    if (l.secondary.pressed) GA.y_down_at = g_clock;
    if (l.secondary.released && GA.y_down_at > 0) {
        if (g_clock - GA.y_down_at < 0.4) {
            ue::Call c(player, "PlayerInput_ScrollWeapon");
            c.Set("IsUp", true);
            c.Invoke();
            log::Info("controls: switch weapon (left Y)");
        }
        GA.y_down_at = -1;
    }
    if (GA.jump_release_at > 0 && g_clock >= GA.jump_release_at) { FireEvent(player, GA.jump_up); GA.jump_release_at = -1; }
    if (GA.crouch_release_at > 0 && g_clock >= GA.crouch_release_at) { FireEvent(player, GA.crouch_up); GA.crouch_release_at = -1; }
    StickCrouchCheck(player);

    // Left stick click: sprint on/off; it ends by itself when you stop.
    // Owner 2026-11-03: "sprinting is very unreliable". The 15:24 log had
    // sprints ending 0.3 to 1.2 s after the click: pressing the stick in
    // lets it slip back towards the centre, and the old code ended the
    // sprint the moment it touched the deadzone. Now a sprint waits up to
    // 1 s for the stick to be pushed after the click, ends only after the
    // stick has rested for 0.35 s, and any push past half way is full speed
    // (the game won't sprint on a part push).
    SprintControl(player, l);

    // Right A / B with the right hand empty (pistol holstered): switch
    // weapon / objective markers. With the gun in hand they are the
    // magazine release and the slide release (MagazineHandling).
    if (!gun_in_hand && r.primary.pressed) {
        ue::Call c(player, "PlayerInput_ScrollWeapon");
        c.Set("IsUp", true);
        c.Invoke();
        log::Info("controls: switch weapon (right A, hand empty)");
    }
    if (!gun_in_hand && r.secondary.pressed) {
        FireEvent(player, GA.nav);
        log::Info("controls: objective markers (right B, hand empty)");
    }

    // Right stick click: kick (the game's melee). On a rifle it is the fire
    // selector instead (FireSelector).
    if (r.stick_click.pressed && !GunIsRifle(player) && g_clock >= GA.melee_ready_at) {
        ue::Call c(player, "PlayerInput_MeleeAttack");
        c.Set("Press Released", false);
        c.Invoke();
        GA.melee_release_at = g_clock + 0.1;
        GA.melee_ready_at = g_clock + 0.7;
        input::Haptic(input::kRight, 0.6f, 0.05f);
        log::Info("controls: kick (right stick click)");
        static int s_melee_traces = 0;
        if (s_melee_traces++ < 3) {
            double rel = 0, side = 0;
            if (CameraOnCharacter(player, &rel, &side))
                log::Info("controls: melee: game camera %.0f cm above the feet, %.0f cm from the middle", rel, side);
            LabTraceFor(2.0);
        }
    }
    if (GA.melee_release_at > 0 && g_clock >= GA.melee_release_at) {
        ue::Call c(player, "PlayerInput_MeleeAttack");
        c.Set("Press Released", true);
        c.Invoke();
        GA.melee_release_at = -1;
    }
}

// Rifle in the left hand (RifleCarry, further down).
// A gun's shape in its actor's space, measured once per gun.
struct GunGeom {
    UObject of = 0;
    bool ok = false;
    FQuat frame;          // gun frame (X barrel, Y right, Z up) in actor space
    FVector grip_rel;     // grip point (MainHandPosition) in actor space
    double muzzle_dist = 40.0;
    double bore_height = 5.0;
};

struct RifleCarryState {
    UObject rifle = 0;        // the weapon manager's primary weapon, when it is a rifle
    UObject pistol = 0;       // its secondary weapon, when it is a pistol
    GunGeom rg, pg;
    bool carried = false;     // the left hand holds the rifle by itself
    interact::CarryHold hold;
    double pick_t = 1.0;      // 0 when picked up .. 1 in the hand (blend from where it was)
    FVector pick_loc;
    FQuat pick_rot;
    FQuat frame_world;        // carried rifle's gun frame this frame
    FVector grip_world;       // and its grip point
    FVector support_world;    // where the left hand is drawn on it
    int point = -1;           // the grip point it holds (vr/interact/grip_points.h), -1 none
    FVector draw_local;       // where the hand is drawn, gun frame (the point, or where it grabbed)
    interact::LongSqueeze squeeze;   // the left grip, while it holds the rifle alone (hold mode)
    bool right_held = false;         // the right grip squeezed and not let go since (LongSqueeze)
    interact::LongSqueeze right_squeeze;
    // Where the rifle is kept when nobody holds it: the game's own spot
    // (learned from where the game hangs it; WeaponLocation_Back until then).
    UObject store_parent = 0;
    uint64_t store_socket = 0;   // FName
    bool have_rel = false;
    FVector rel_loc;             // rifle actor in the spot's frame
    FQuat rel_rot;
    double next_learn = 0;
    bool logged_store = false;
    bool returning = false;   // flying back to its spot
    double ret_t = 1.0;
    FVector ret_loc;
    FQuat ret_rot;
    bool parked = false;      // current rifle nobody holds: kept at its spot by us
    FVector last_loc;         // where we last put the rifle
    FQuat last_rot;
    bool have_last = false;
    // A switch asked of the game that has not happened yet.
    int switch_to = 0;        // 1 pistol, 2 rifle
    double switch_at = -1;
    bool draw_on_switch = false;
    bool support_on_switch = false;
    ActionEvent to_primary{"InpActEvt_SwitchToPrimary_K2Node_InputActionEvent_7"};
    ActionEvent to_secondary{"InpActEvt_SwitchToSecondary_K2Node_InputActionEvent_6"};
    bool logged_guns = false;
} g_rc;

bool GunIsRifle(UObject player) {
    game::Gun gun = game::Gun::Current(player);
    return gun.Valid() && gun.Object() == W.gun && !W.is_pistol;
}

bool ControlsHoldsGrip(int hand) {
    // Logical ownership must not disappear just because an IK pose is invalid.
    if (hand == hands::kRight) return W.gun && W.in_hand;
    return W.slide.Held() || W.held_mag || (g_rc.carried && g_rc.rifle) || W.two_hand;
}

void ResolveHandOwnership(bool actions_allowed) {
    hands::HandInteraction next[2];
    for (int h = 0; h < 2; ++h) {
        hands::GripFacts facts;
        facts.controls_held = ControlsHoldsGrip(h);
        facts.attachment_held = workbench::HoldsGrip(h);
        facts.drink_held = drinks::HoldsGrip(h);
        facts.actions_allowed = actions_allowed;
        if (actions_allowed) {
            facts.attachment_candidate = workbench::ClaimsGrip(h);
            facts.drink_candidate = drinks::ClaimsGrip(h);
        }
        next[h] = hands::ResolveGrip(facts);
    }
    std::lock_guard<std::mutex> lock(g_hands_mutex);
    for (int h = 0; h < 2; ++h) g_interaction[h] = next[h];
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
        } else if (g_rc.carried && g_rc.rifle) {
            // The rifle in the left hand by itself (RifleCarry): the hand on
            // its handguard, the rifle's own frame.
            g[hands::kLeft].kind = hands::GrabKind::kSupportGrip;
            g[hands::kLeft].object = g_rc.rifle;
            g[hands::kLeft].target = {g_rc.support_world, g_rc.frame_world};
            g[hands::kLeft].point = g_rc.point;
        } else if (W.two_hand && W.have_support_point) {
            g[hands::kLeft].kind = hands::GrabKind::kSupportGrip;
            g[hands::kLeft].object = gun.Object();
            g[hands::kLeft].target = lh.grip;
            g[hands::kLeft].target.location = W.support_point;
            g[hands::kLeft].point = W.support_id;
        }
    }
    std::lock_guard<std::mutex> lock(g_hands_mutex);
    g_grab[0] = g[0];
    g_grab[1] = g[1];
    for (int h = 0; h < 2; ++h)
        g_interaction[h] = hands::AfterControls(g_interaction[h], ControlsHoldsGrip(h));
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

// Kick aim (owner 2026-11-03: kicks "horribly unreliable" at landing where
// aimed). The game's KickTrace sweeps a sphere from the foot toward the
// actor location plus a reach turned by the CONTROL rotation, which in VR is
// only the stick-turn yaw: the kick went where the character faced, not
// where the player looks. Just for the trace, the control rotation takes the
// headset's yaw and pitch, and is put back right after, inside the same game
// call, so the camera and view never see it.
struct KickAim {
    UObject controller = 0;
    FRotator saved{};
    bool active = false;
    int logs = 0;
} g_kick_aim;

bool BeforeKickTrace(UObject self, UObject, void*) {
    g_kick_aim.active = false;
    if (!g_set.enabled || !config::GetBool("kick_aim_with_head", true)) return true;
    hands::Pose head;
    if (!hands::Head(&head)) return true;
    ue::Call gc(self, "GetController");
    UObject controller = gc.Invoke() ? gc.Result<UObject>() : 0;
    if (!controller) return true;
    ue::Call get(controller, "GetControlRotation");
    if (!get.Invoke()) return true;
    const FRotator saved = get.Result<FRotator>();
    FRotator look = ToRotator(head.rotation);
    look.Pitch = std::clamp(look.Pitch, -50.0, 30.0);
    look.Roll = 0;
    ue::Call set(controller, "SetControlRotation");
    set.Set("NewRotation", look);
    if (!set.Invoke()) return true;
    g_kick_aim.controller = controller;
    g_kick_aim.saved = saved;
    g_kick_aim.active = true;
    if (g_kick_aim.logs++ < 6)
        log::Info("controls: kick aimed where you look: yaw %.0f pitch %.0f (the character faces yaw %.0f, "
                  "%.0f deg off)",
                  look.Yaw, look.Pitch, saved.Yaw, NormalizeAxis(look.Yaw - saved.Yaw));
    return true;
}

void AfterKickTrace(UObject, UObject, void*) {
    if (!g_kick_aim.active) return;
    g_kick_aim.active = false;
    ue::Call set(g_kick_aim.controller, "SetControlRotation");
    set.Set("NewRotation", g_kick_aim.saved);
    set.Invoke();
}

void HookKickAim(UObject player) {
    static bool done = false;
    if (done || !player) return;
    done = true;
    UObject fn = ue::FindFunction(ue::GetClass(player), "TL_Kick__KickTrace__EventFunc");
    const bool ok = fn && ue::process_event::On(fn, &BeforeKickTrace, &AfterKickTrace);
    log::Info("controls: kick aim %s", ok ? "follows the headset" : "hook NOT found (kicks go where the body faces)");
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

void ShotStudy(UObject player, game::Gun& gun, int n, const FVector& muzzle, const FVector& barrel,
               const FRotator& ctrl) {
    if (n == 1) {
        StudyFireClass(gun.Object(), "gun");
        StudyFireClass(player, "player");
    }
    if (n <= 3) LabTraceFor(0.4);
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

void ShotStudyTick() { LabTraceTick(); }

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
    for (int i = 0; i < HolsterWorld::kSpots; ++i) {
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
        for (int i = 0; i < HolsterWorld::kSpots; ++i) {
            const interact::HolsterSpot& sp = g_spots[i];
            log::Info("holster: %s spot on %s, (%.1f %.1f %.1f) cm, %s",
                      i == kPistolSpot ? "pistol" : i == kMagSpot ? "magazine" : "rifle",
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
    {
        // Lying on a table at the workbench (vr/workbench): stays there.
        FVector spot;
        FQuat frame;
        if (workbench::ParkedGun(g, &spot, &frame)) {
            const FVector grip_w = spot + frame.Rotate(GunGripOffset());
            const FQuat rot = W.have_frame ? frame * W.gun_frame.Inverse() : frame;
            SetActorTransform(g, grip_w - rot.Rotate(rel), rot);
            return;
        }
    }
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
        W.in_hand = !W.right_let_go;
        return;
    }
    interact::DrawInput in;
    in.hand = rh.grip.location;
    in.hand_valid = rh.valid;
    in.spot = g_hw.spot[0];
    in.spot_valid = g_hw.valid;
    // Lying on a table (vr/workbench): it is picked up from there.
    FVector park_spot;
    FQuat park_frame;
    const bool parked = workbench::ParkedGun(W.gun, &park_spot, &park_frame);
    if (parked) {
        in.spot = park_spot;
        in.spot_valid = true;
    }
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
            if (parked) workbench::GunTaken();
            else log::Info("holster: pistol drawn (hand %.1f cm from the holster spot)", W.holster.LastDistance());
            break;
        case E::kStowed:
            input::Haptic(input::kRight, 0.6f, 0.05f);
            if (parked) log::Info("workbench: pistol put back down where it lay");
            else log::Info("holster: pistol holstered (let go %.1f cm from the holster spot)", W.holster.LastDistance());
            break;
        case E::kReturnStarted:
            // Over a table at the workbench it is laid down there instead.
            if (W.have_pose && workbench::TryPark(W.gun, W.frame_rot, rh.grip.location)) {
                W.holster.Reset(interact::HolsterState::kHolstered);
                break;
            }
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

// VR tab "Move holsters" (5 s countdown): each holster moves to where the hand holding
// its item is now (pistol in the right hand, magazine in the left), anywhere
// on the body, and keeps the item at the angle it is held. It rides on the
// nearest body part (belt, chest, a thigh) and is saved to
// hole-vr/holsters.ini. Needs the VR body, since the spots ride on it.
struct HolsterCalState {
    double down_at = -1;   // when the countdown started
    bool done = false;
    int beat = 0;
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
              spot == kPistolSpot ? "pistol holster" : spot == kMagSpot ? "magazine pouch" : "rifle holster",
              interact::AnchorPlace(a), sp.offset.X,
              sp.offset.Y, sp.offset.Z, Len(hand - an.location),
              sp.turned ? "at the angle you held it" : "hanging the built-in way (angle not known)");
}

void HolsterCalibration(const input::HandInput&, const hands::HandState& rh, const hands::HandState& lh) {
    // VR tab "Move holsters": 5 s after the menu closes (a buzz each second),
    // each holster moves to where the hand holding its item is.
    if (g_req_holsters.exchange(false)) {
        g_cal.down_at = g_clock;
        g_cal.done = false;
        g_cal.beat = 5;
        log::Info("holster: moving the holsters in 5 s: hold the pistol and the magazine (or the rifle) where you "
                  "want them");
    }
    if (g_cal.down_at < 0 || g_cal.done) return;
    const double left = 5.0 - (g_clock - g_cal.down_at);
    if (left > 0) {
        if (static_cast<int>(std::ceil(left)) < g_cal.beat) {
            g_cal.beat = static_cast<int>(std::ceil(left));
            input::Haptic(input::kLeft, 0.25f, 0.04f);
            input::Haptic(input::kRight, 0.25f, 0.04f);
        }
        return;
    }
    g_cal.done = true;
    g_cal.down_at = -1;
    if (!g_hw.valid || !g_hw.on_body) {
        log::Info("holster: can't move the holsters while the VR body is off (F9)");
        input::Haptic(input::kRight, 0.2f, 0.1f);
        return;
    }
    int moved = 0;
    if (!W.is_pistol) {
        // Rifle out: its holster goes where the right hand holds it. (The
        // pouch holds pistol magazines' spot; a rifle magazine slot comes later.)
        if (g_set.holster_rifle && g_spots[kRifleSpot].enabled && W.in_hand && W.have_pose && rh.valid) {
            PlaceHolsterAt(kRifleSpot, rh.grip.location, &W.frame_rot);
            input::Haptic(input::kRight, 0.8f, 0.1f);
            ++moved;
        }
    } else if (W.uses_holster && W.in_hand && rh.valid) {
        PlaceHolsterAt(kPistolSpot, rh.grip.location, W.have_pose ? &W.frame_rot : nullptr);
        input::Haptic(input::kRight, 0.8f, 0.1f);
        ++moved;
    }
    if (W.is_pistol && W.held_mag && lh.valid && !W.mag_engaged && !W.mag_seating) {
        PlaceHolsterAt(kMagSpot, lh.grip.location, &W.mag_shown_rot);
        input::Haptic(input::kLeft, 0.8f, 0.1f);
        ++moved;
    }
    if (!moved) {
        log::Info("holster: nothing moved: during the 5 s countdown (VR tab: Move holsters), hold the pistol "
                  "and/or a magazine (or the rifle in the right hand) where you want it");
        return;
    }
    const bool saved = SaveHolsterSpots();
    log::Info("holster: %s", saved ? "saved to holsters.ini" : "NOT saved (holsters.ini not writable)");
    g_hw.logged_mode = -1;
}

// ---- rifle holster (behind the right shoulder unless moved) ----
// The rifle-carry code (below) asks these where the rifle rests when nobody
// holds it; with the holster on, the rifle stays detached from the game's own
// back spot and the late tick keeps it here, after the body is posed.
bool RifleHolsterOn() {
    return g_set.holster_rifle && g_spots[kRifleSpot].enabled && g_hw.valid && g_rc.rifle;
}

// The rifle's gun frame and grip point (MainHandPosition) at the holster: as
// it was held when the holster was placed in the headset, otherwise
// interact::RifleBackFrame. Placed as HoldGun would for a hand at the spot.
void RifleHolsterFrame(FQuat* frame, FVector* grip_w) {
    const interact::HolsterSpot& sp = g_spots[kRifleSpot];
    *frame = sp.turned ? g_hw.spot_anchor_rot[kRifleSpot] * sp.rot
                       : interact::RifleBackFrame(g_hw.spot_anchor_rot[kRifleSpot], 30.0);
    *grip_w = g_hw.spot[kRifleSpot] + frame->Rotate(GunGripOffset());
}

// The rifle actor's world pose in the holster this frame.
bool RifleHolsterPose(FVector* actor_loc, FQuat* actor_rot) {
    if (!RifleHolsterOn() || !g_rc.rg.ok || g_rc.rg.of != g_rc.rifle) return false;
    FQuat f;
    FVector g;
    RifleHolsterFrame(&f, &g);
    interact::ActorFromFrame(f, g, g_rc.rg.frame, g_rc.rg.grip_rel, actor_rot, actor_loc);
    return true;
}

// Where the right hand squeezes to take the rifle from the holster.
bool RifleHolsterGrab(FVector* spot, double* radius) {
    if (!RifleHolsterOn()) return false;
    *spot = g_hw.spot[kRifleSpot];
    *radius = g_spots[kRifleSpot].radius;
    return true;
}

// The rifle is resting in the holster: neither hand holds it and it is not
// flying back.
bool RifleAtRest() {
    return g_rc.rifle && !g_rc.carried && !g_rc.returning && !(W.gun == g_rc.rifle && W.in_hand);
}

interact::HolsterZone g_rifle_zone;
int g_rifle_logged = -1;

// Late tick: keeps the resting rifle in its holster, and a light buzz when
// the free right hand reaches it.
void PlaceRifleInHolster() {
    const bool on = g_set.rifle_carry && RifleHolsterOn() && RifleAtRest() && !(W.gun == g_rc.rifle && W.edit_frozen);
    FVector loc;
    FQuat rot;
    const bool placed = on && RifleHolsterPose(&loc, &rot);
    if (placed) {
        DetachIfAttached(g_rc.rifle);
        SetActorTransform(g_rc.rifle, loc, rot);
    }
    if (static_cast<int>(placed) != g_rifle_logged) {
        g_rifle_logged = placed;
        if (placed)
            log::Info("holster: rifle resting in its holster on %s, grab spot %.0f cm from the headset",
                      interact::AnchorPlace(g_spots[kRifleSpot].anchor), [&] {
                          hands::Pose h;
                          return hands::Head(&h) ? Len(h.location - g_hw.spot[kRifleSpot]) : 0.0;
                      }());
    }
    const hands::HandState rh = hands::Get(hands::kRight);
    const bool right_free = !(W.gun && W.in_hand);
    const double d = rh.valid ? Len(rh.grip.location - g_hw.spot[kRifleSpot]) : 1e9;
    if (g_rifle_zone.Update(d, g_spots[kRifleSpot].radius, placed && right_free && rh.valid))
        input::Haptic(input::kRight, 0.15f, 0.015f);
}

// Set by OnTick when it handled a gun this frame, so the late tick only
// places holstered items for the gun this frame's controls saw.
bool g_late_ok = false;

// ---- rifle in the left hand (vr/interact/rifle_carry.h, docs/RIFLE_CARRY.md) ----
// The game keeps both guns on its weapon manager: Obj_PrimaryWeapon (the
// rifle, which it hangs on the back while the pistol is out) and
// Obj_SecondaryWeapon (the pistol). The left hand can hold the rifle by its
// handguard whichever gun is current, so the pistol fires with the rifle
// still in the other hand. The game's own SwitchToPrimary / SwitchToSecondary
// inputs change which gun fires.


bool IsPistolClass(UObject g) { return ue::GetName(ue::GetClass(g)).find("HG") != std::string::npos; }

bool EnsureGeom(UObject g, GunGeom* geo) {
    if (geo->of == g && geo->ok) return true;
    // A gun that could not be measured is tried again once a second.
    static double retry_at = 0;
    if (geo->of == g && g_clock < retry_at) return false;
    retry_at = g_clock + 1.0;
    *geo = GunGeom{};
    geo->of = g;
    UObject main = 0;
    GunFrameResult r;
    bool ok = g && ue::GetProp(g, "MainHandPosition", main) && main;
    const FVector loc = ok ? ActorLocation(g) : FVector{};
    const FQuat rot = ok ? ActorRotation(g) : FQuat{};
    const FVector main_w = ok ? ComponentToWorld(main).translation : FVector{};
    ok = ok && ComputeGunFrame(g, loc, rot, main_w, &r);
    if (!ok) {
        static UObject warned = 0;
        if (warned != g) {
            warned = g;
            log::Warn("rifle carry: could not measure %s (no grip or muzzle point); trying again every second",
                      g ? ue::GetName(ue::GetClass(g)).c_str() : "?");
        }
        return false;
    }
    geo->frame = r.frame;
    geo->grip_rel = rot.Inverse().Rotate(main_w - loc);
    geo->muzzle_dist = r.muzzle_dist;
    geo->bore_height = r.bore_height;
    geo->ok = true;
    log::Info("rifle carry: %s measured: barrel along (%.2f %.2f %.2f), muzzle %.1f cm ahead of and %.1f cm above the "
              "grip point",
              ue::GetName(ue::GetClass(g)).c_str(), r.fwd.X, r.fwd.Y, r.fwd.Z, geo->muzzle_dist, geo->bore_height);
    return true;
}

// The rifle's gun frame and grip point in the world, where it is now.
void RifleFrameNow(FQuat* frame, FVector* grip) {
    const FVector loc = ActorLocation(g_rc.rifle);
    const FQuat rot = ActorRotation(g_rc.rifle);
    *frame = rot * g_rc.rg.frame;
    *grip = loc + rot.Rotate(g_rc.rg.grip_rel);
}

void ResetRifleCarry(const char* why) {
    if (g_rc.carried || g_rc.returning || g_rc.parked || g_rc.switch_to)
        log::Info("rifle carry: reset (%s)", why);
    UObject rifle = g_rc.rifle, pistol = g_rc.pistol;
    GunGeom rg = g_rc.rg, pg = g_rc.pg;
    g_rc = RifleCarryState();
    g_rc.rifle = rifle;
    g_rc.pistol = pistol;
    g_rc.rg = rg;
    g_rc.pg = pg;
}

// Which guns the weapon manager has now. Forgets everything when they change
// (a new level, death).
void RefreshCarryGuns(UObject player) {
    UObject manager = 0, prim = 0, sec = 0;
    if (player) ue::GetProp(player, "WeaponManager", manager);
    if (manager) {
        ue::GetProp(manager, "Obj_PrimaryWeapon", prim);
        ue::GetProp(manager, "Obj_SecondaryWeapon", sec);
    }
    // Sorted out again only when the weapon manager's guns change (or one of
    // them dies), not every frame.
    static UObject last_prim = 0, last_sec = 0, rifle = 0, pistol = 0;
    const bool alive = (!rifle || LiveObject(rifle)) && (!pistol || LiveObject(pistol));
    if (prim != last_prim || sec != last_sec || !alive) {
        last_prim = prim;
        last_sec = sec;
        UObject gun_class = game::GunBaseClass();
        auto gun_or_0 = [&](UObject o) -> UObject {
            return o && gun_class && LiveObject(o) && ue::reflection::IsA(o, gun_class) ? o : 0;
        };
        prim = gun_or_0(prim);
        sec = gun_or_0(sec);
        rifle = prim && !IsPistolClass(prim) ? prim : sec && !IsPistolClass(sec) ? sec : 0;
        pistol = sec && IsPistolClass(sec) ? sec : prim && IsPistolClass(prim) ? prim : 0;
        if (!alive) last_prim = last_sec = 0;   // look again next frame
    }
    if (rifle != g_rc.rifle || pistol != g_rc.pistol || !g_rc.logged_guns) {
        const bool changed = rifle != g_rc.rifle;
        g_rc.rifle = rifle;
        g_rc.pistol = pistol;
        if (changed) {
            ResetRifleCarry("the guns changed");
            g_rc.rg = GunGeom{};
        }
        g_rc.pg = pistol == g_rc.pg.of ? g_rc.pg : GunGeom{};
        g_rc.logged_guns = true;
        log::Info("rifle carry: the game has %s%s and %s%s", rifle ? "the rifle " : "no rifle",
                  rifle ? ue::GetName(ue::GetClass(rifle)).c_str() : "", pistol ? "the pistol " : "no pistol",
                  pistol ? ue::GetName(ue::GetClass(pistol)).c_str() : "");
    }
    if (g_rc.rifle && !g_rc.store_parent && manager) {
        UObject back = 0;
        ue::GetProp(manager, "WeaponLocation_Back", back);
        g_rc.store_parent = back;
    }
}

// Learns where the game hangs the rifle when it is not out: its attach
// parent, socket and offset there. Every half second while the game has it
// (not current, not touched by us).
void LearnRifleSpot(bool rifle_current) {
    if (!g_rc.rifle || rifle_current || g_rc.carried || g_rc.returning || g_rc.parked || g_clock < g_rc.next_learn)
        return;
    g_rc.next_learn = g_clock + 0.5;
    UObject root = 0, parent = 0;
    uint64_t socket = 0;
    if (!ue::GetProp(g_rc.rifle, "RootComponent", root) || !root) return;
    ue::GetProp(root, "AttachParent", parent);
    ue::GetProp(root, "AttachSocketName", socket);
    if (!parent) return;
    const UETransform pw = ComponentToWorld(parent);
    const FQuat inv = pw.rotation.Inverse();
    const FVector rel_loc = inv.Rotate(ActorLocation(g_rc.rifle) - pw.translation);
    const FQuat rel_rot = inv * ActorRotation(g_rc.rifle);
    const bool moved = !g_rc.have_rel || parent != g_rc.store_parent || Len(rel_loc - g_rc.rel_loc) > 1.0;
    g_rc.store_parent = parent;
    g_rc.store_socket = socket;
    g_rc.rel_loc = rel_loc;
    g_rc.rel_rot = rel_rot;
    g_rc.have_rel = true;
    if (moved || !g_rc.logged_store) {
        g_rc.logged_store = true;
        hands::Pose head;
        double below = 0, behind = 0, side = 0;
        if (hands::Head(&head)) {
            const FQuat yaw = ToQuat(FRotator{0, YawOf(head.rotation), 0});
            FQuat f;
            FVector gp;
            RifleFrameNow(&f, &gp);
            const FVector d = yaw.Inverse().Rotate(gp - head.location);
            below = -d.Z;
            behind = -d.X;
            side = d.Y;
        }
        log::Info("rifle carry: the game hangs the rifle on %s (offset %.1f %.1f %.1f cm); its grip is %.0f cm below, "
                  "%.0f cm behind and %.0f cm %s of the headset",
                  ue::GetName(parent).c_str(), rel_loc.X, rel_loc.Y, rel_loc.Z, below, behind, std::fabs(side),
                  side >= 0 ? "right" : "left");
    }
}

// Where the rifle goes when nobody holds it.
bool RifleSpotPose(FVector* loc, FQuat* rot) {
    if (RifleHolsterOn()) return RifleHolsterPose(loc, rot);   // our holster (behind the right shoulder)
    if (!g_rc.store_parent || !LiveObject(g_rc.store_parent)) return false;
    const UETransform pw = ComponentToWorld(g_rc.store_parent);
    *rot = g_rc.have_rel ? pw.rotation * g_rc.rel_rot : pw.rotation;
    *loc = g_rc.have_rel ? pw.translation + pw.rotation.Rotate(g_rc.rel_loc) : pw.translation;
    return true;
}

// Gives the rifle back to the game at its spot (when it is not the current gun).
void ReattachRifle() {
    if (!g_rc.store_parent) return;
    ue::Call c(g_rc.rifle, "K2_AttachToComponent");
    const uint8_t rule = g_rc.have_rel ? 1 : 2;   // EAttachmentRule: KeepWorld (placed there) / SnapToTarget
    c.Set("Parent", g_rc.store_parent).Set("SocketName", g_rc.store_socket).Set("LocationRule", rule)
        .Set("RotationRule", rule).Set("ScaleRule", uint8_t{1}).Set("bWeldSimulatedBodies", false);
    const bool ok = c.Invoke() && c.Result<bool>();
    log::Info("rifle carry: rifle back on %s%s", ue::GetName(g_rc.store_parent).c_str(), ok ? "" : " (attach FAILED)");
}

void AskSwitch(UObject player, int to) {
    ActionEvent& e = to == 1 ? g_rc.to_secondary : g_rc.to_primary;
    bool ok = FireEvent(player, e);
    if (!ok) {
        // No such input on this game version: step through the weapons instead.
        ue::Call c(player, "PlayerInput_ScrollWeapon");
        c.Set("IsUp", true);
        ok = c.Invoke();
    }
    g_rc.switch_to = to;
    g_rc.switch_at = g_clock;
    log::Info("rifle carry: asked the game to switch to the %s%s", to == 1 ? "pistol" : "rifle", ok ? "" : " (FAILED)");
}

// The left hand's hold passes between the two-hand hold (W) and the rifle
// in the left hand alone (g_rc) with its grip point: the hand stays drawn
// where it was. g_rc.hold must already be set.
void CarryFromSupport() {
    g_rc.point = W.two_hand ? W.support_id : -1;
    g_rc.draw_local = g_rc.point >= 0 ? W.support_draw : g_rc.hold.support_local;
    g_rc.squeeze.Start();
}

void SupportFromCarry() {
    W.two_hand = true;
    W.support_local = g_rc.hold.support_local;
    W.support_id = g_rc.point;
    W.support_draw = g_rc.draw_local;
    W.support_squeeze.Start();
    W.support_since = g_clock;
    W.support_lost_for = 0;
}

// Called right before the right-hand gun changes (W is about to be reset):
// a rifle in both hands that the game puts away (left Y, or the switch we
// asked for) stays in the left hand.
void RifleCarryBeforeGunChange(UObject new_gun) {
    if (!g_set.rifle_carry || !g_rc.rifle || W.gun != g_rc.rifle || new_gun == g_rc.rifle) return;
    if (W.two_hand && W.in_hand && W.have_pose && !g_rc.carried && g_rc.rg.ok) {
        hands::HandState lh = hands::Get(hands::kLeft);
        if (lh.valid) {
            g_rc.carried = true;
            g_rc.hold = interact::HoldAsIs(lh.grip, W.frame_rot, W.grip_world);
            CarryFromSupport();
            g_rc.pick_t = 1.0;
            g_rc.returning = g_rc.parked = false;
            log::Info("rifle carry: the game put the rifle away with both hands on it; the left hand keeps it");
        }
    }
    if (W.trigger_sent) ReleaseTrigger(game::Player());
}

// Called right after the right-hand gun changed (W was reset).
void RifleCarryOnNewGun(game::Gun& gun) {
    if (!g_set.rifle_carry || !gun.Valid()) return;
    const double took = g_rc.switch_at >= 0 ? g_clock - g_rc.switch_at : -1;
    if (gun.Object() == g_rc.pistol && g_rc.pistol) {
        if (g_rc.switch_to == 1 && g_rc.draw_on_switch && W.uses_holster) {
            W.holster.Reset(interact::HolsterState::kInHand);
            W.in_hand = true;
            input::Haptic(input::kRight, 0.5f, 0.04f);
        }
        if (g_rc.switch_to == 1)
            log::Info("rifle carry: the game switched to the pistol %.2f s after we asked%s", took,
                      g_rc.draw_on_switch ? "; it is in the right hand" : "");
    } else if (gun.Object() == g_rc.rifle && g_rc.rifle) {
        if (g_rc.carried) {
            // The left hand stays on the handguard as the support hand.
            SupportFromCarry();
            g_rc.carried = false;
            log::Info("rifle carry: rifle in the right hand, the left hand stays on the handguard at (%.1f %.1f %.1f)",
                      W.support_local.X, W.support_local.Y, W.support_local.Z);
        }
        g_rc.returning = g_rc.parked = false;
        if (g_rc.switch_to == 2) log::Info("rifle carry: the game switched to the rifle %.2f s after we asked", took);
    }
    g_rc.switch_to = 0;
    g_rc.switch_at = -1;
    g_rc.draw_on_switch = g_rc.support_on_switch = false;
}

// Puts the rifle where it belongs this frame: in the left hand, flying back
// to its spot, or held at its spot (the current rifle nobody holds).
void PlaceRifle(const hands::HandState& lh, double dt, bool rifle_current) {
    if (!g_rc.rifle || !g_rc.rg.ok) return;
    FVector loc;
    FQuat rot;
    if (g_rc.carried) {
        if (!lh.valid) return;   // controller lost: it stays where it was
        FQuat f;
        FVector g;
        interact::PlaceFromHand(lh.grip, g_rc.hold, &f, &g);
        interact::ActorFromFrame(f, g, g_rc.rg.frame, g_rc.rg.grip_rel, &rot, &loc);
        if (g_rc.pick_t < 1.0) {
            g_rc.pick_t = std::fmin(1.0, g_rc.pick_t + dt / 0.12);
            const double e = interact::Ease(g_rc.pick_t);
            loc = g_rc.pick_loc + (loc - g_rc.pick_loc) * e;
            rot = interact::BlendRot(g_rc.pick_rot, rot, e);
        }
        g_rc.frame_world = rot * g_rc.rg.frame;
        g_rc.grip_world = loc + rot.Rotate(g_rc.rg.grip_rel);
        g_rc.support_world = g_rc.grip_world + g_rc.frame_world.Rotate(g_rc.draw_local);
    } else if (g_rc.returning || g_rc.parked) {
        FVector sl;
        FQuat sr;
        if (!RifleSpotPose(&sl, &sr)) {
            g_rc.returning = g_rc.parked = false;
            return;
        }
        if (g_rc.returning) {
            g_rc.ret_t = std::fmin(1.0, g_rc.ret_t + dt / 0.2);
            const double e = interact::Ease(g_rc.ret_t);
            loc = g_rc.ret_loc + (sl - g_rc.ret_loc) * e;
            rot = interact::BlendRot(g_rc.ret_rot, sr, e);
            if (g_rc.ret_t >= 1.0) {
                g_rc.returning = false;
                g_rc.parked = rifle_current;
                if (!rifle_current) {
                    SetActorTransform(g_rc.rifle, sl, sr);
                    if (!RifleHolsterOn()) ReattachRifle();   // the holster keeps it (late tick)
                    g_rc.have_last = false;
                    return;
                }
            }
        } else {
            if (!rifle_current) {
                // The game made something else current: it owns the rifle
                // again (or, with the holster on, the holster keeps it).
                g_rc.parked = false;
                SetActorTransform(g_rc.rifle, sl, sr);
                if (!RifleHolsterOn()) ReattachRifle();
                g_rc.have_last = false;
                return;
            }
            if (RifleHolsterOn()) return;   // resting in the holster: the late tick places it
            loc = sl;
            rot = sr;
        }
    } else {
        return;
    }
    DetachIfAttached(g_rc.rifle);
    SetActorTransform(g_rc.rifle, loc, rot);
    g_rc.last_loc = loc;
    g_rc.last_rot = rot;
    g_rc.have_last = true;
}

// The pistol in its holster while the rifle is out (the game would hang it
// on the right thigh), so it is drawn where it always is.
void PlaceSparePistol() {
    UObject p = g_rc.pistol;
    if (!g_set.rifle_carry || !p || p == W.gun || !g_rc.rifle || W.gun != g_rc.rifle) return;
    if (!g_set.holster_pistol || !g_spots[kPistolSpot].enabled || !g_hw.valid) return;
    if (!EnsureGeom(p, &g_rc.pg)) return;
    const interact::HolsterSpot& sp = g_spots[kPistolSpot];
    const FQuat frame = sp.turned ? g_hw.spot_anchor_rot[0] * sp.rot
                                  : interact::PistolHolsterFrame(g_hw.spot_anchor_rot[0], g_set.holster_cant);
    const FVector grip_w = g_hw.spot[0] + frame.Rotate(GunGripOffset());
    FQuat rot;
    FVector loc;
    interact::ActorFromFrame(frame, grip_w, g_rc.pg.frame, g_rc.pg.grip_rel, &rot, &loc);
    DetachIfAttached(p);
    SetActorTransform(p, loc, rot);
}

// Hand moves for the rifle (interact::Decide) and placing it. Runs after the
// left hand's slide / magazine / support grabs had their chance this frame.
void RifleCarry(UObject player, const hands::HandState& rh, const hands::HandState& lh,
                const input::HandInput& lin, const input::HandInput& rin, double dt, bool actions) {
    if (!g_set.rifle_carry || !g_rc.rifle) return;
    const bool rifle_current = W.gun == g_rc.rifle;
    if (!EnsureGeom(g_rc.rifle, &g_rc.rg)) return;
    LearnRifleSpot(rifle_current);
    if (g_rc.switch_to && g_clock - g_rc.switch_at > 2.5) {
        log::Info("rifle carry: the game did not switch to the %s within 2.5 s; nothing changed",
                  g_rc.switch_to == 1 ? "pistol" : "rifle");
        g_rc.switch_to = 0;
        g_rc.switch_at = -1;
        g_rc.draw_on_switch = g_rc.support_on_switch = false;
    }

    if (actions) {
        interact::CarryInput in;
        in.have_rifle = true;
        in.rifle_current = rifle_current;
        in.right_has_rifle = rifle_current && W.in_hand;
        in.right_has_pistol = W.gun && W.gun == g_rc.pistol && W.in_hand;
        in.left_support = rifle_current && W.two_hand;
        in.left_busy = W.slide.Held() || W.held_mag != 0;
        in.carrying = g_rc.carried;
        in.switching = g_rc.switch_to != 0;
        // A hand that holds the rifle a long time lets go only when its
        // squeeze stays low (interact::LongSqueeze), not when it relaxes a
        // little under the grip button's 0.35 (first AR run, 2026-11-03:
        // the rifle kept leaving the right hand while aiming).
        in.left_grip_down = g_rc.carried ? g_rc.squeeze.Update(lin.grip_value, dt) : lin.grip_button.down;
        in.left_grip_pressed = lin.grip_button.pressed;
        in.right_grip_pressed = rin.grip_button.pressed;
        if (rin.grip_button.pressed) {
            g_rc.right_held = true;
            g_rc.right_squeeze.Start();
        } else if (g_rc.right_held && !g_rc.right_squeeze.Update(rin.grip_value, dt)) {
            g_rc.right_held = false;
            in.right_grip_released = true;
        }
        in.toggle = g_set.grip_toggle;
        in.right_toggle = g_set.holster_toggle;
        FQuat frame;
        FVector grip;
        bool have_now = false;
        auto now = [&] {
            if (!have_now) RifleFrameNow(&frame, &grip);
            have_now = true;
        };
        int pick_point = -1;
        FVector pick_at;
        if (in.left_grip_pressed && lh.valid && !in.carrying && !in.right_has_rifle) {
            now();
            const FVector p = frame.Inverse().Rotate(lh.grip.location - grip);
            const bool close_by = Len(lh.grip.location - grip) < 60.0;
            const interact::GripPoints& gp =
                close_by ? GripPointsFor(kPointsCarry, g_rc.rifle, frame, grip, g_rc.rg.muzzle_dist) : interact::GripPoints{};
            if (gp.ok) {
                pick_point = interact::NearestPoint(gp, p);
                if (pick_point >= 0) pick_at = gp.p[pick_point];
                in.left_on_handguard = pick_point >= 0;
            } else {
                in.left_on_handguard = interact::OnHandguard(p, g_rc.rg.muzzle_dist, g_rc.rg.bore_height);
            }
            if (close_by)
                log::Info("rifle carry: left grab at rifle-frame (%.1f %.1f %.1f), %s%s", p.X, p.Y, p.Z,
                          !in.left_on_handguard ? "off the handguard"
                          : pick_point >= 0     ? interact::PointName(pick_point)
                                                : "inside the handguard zone",
                          in.left_busy ? " (hand already busy)" : "");
        }
        if (in.right_grip_pressed && rh.valid) {
            if (g_hw.valid && g_set.holster_pistol && g_spots[kPistolSpot].enabled && g_rc.pistol)
                in.right_at_pistol_holster =
                    Len(rh.grip.location - g_hw.spot[kPistolSpot]) <= g_spots[kPistolSpot].radius;
            if (!in.right_has_rifle && !in.right_has_pistol) {
                now();
                const double d = Len(rh.grip.location - grip);
                in.right_on_rifle_grip = d <= g_set.rifle_grip_reach;
                // Resting in the holster: anywhere in its (wider) zone.
                FVector hs;
                double hr = 0;
                const bool in_holster = RifleAtRest() && RifleHolsterGrab(&hs, &hr);
                const double hd = in_holster ? Len(rh.grip.location - hs) : 1e9;
                if (in_holster && hd <= hr) in.right_on_rifle_grip = true;
                if (d < 60.0 || hd < 60.0)
                    log::Info("rifle carry: right grab %.1f cm from the rifle's grip (reach %.0f)%s%s", d,
                              g_set.rifle_grip_reach,
                              in_holster ? (hd <= hr ? ", inside the rifle holster zone" : ", outside the rifle holster zone")
                                         : "",
                              in.right_at_pistol_holster ? ", at the pistol holster" : "");
            }
        }
        const interact::CarryAction a = interact::Decide(in);
        using A = interact::CarryAction;
        switch (a) {
            case A::kPickUp: {
                now();
                const FQuat turn = ToQuat(FRotator{g_set.carry_pitch, g_set.carry_yaw, g_set.carry_roll});
                if (pick_point >= 0) {
                    g_rc.hold = interact::HoldAtPoint(pick_at, turn);
                } else {
                    g_rc.hold = interact::HoldFromGrab(lh.grip, frame, grip, g_rc.rg.muzzle_dist,
                                                       g_rc.rg.bore_height, g_set.carry_fist_below_bore, turn);
                }
                g_rc.point = pick_point;
                g_rc.draw_local = g_rc.hold.support_local;
                g_rc.squeeze.Start();
                g_rc.pick_loc = ActorLocation(g_rc.rifle);
                g_rc.pick_rot = ActorRotation(g_rc.rifle);
                g_rc.pick_t = 0.0;
                g_rc.carried = true;
                g_rc.returning = g_rc.parked = false;
                input::Haptic(input::kLeft, 0.35f, 0.03f);
                log::Info("rifle carry: left hand picked up the rifle by the handguard at (%.1f %.1f %.1f) from the "
                          "grip, %s",
                          g_rc.hold.support_local.X, g_rc.hold.support_local.Y, g_rc.hold.support_local.Z,
                          interact::PointName(g_rc.point));
                break;
            }
            case A::kRightLetGo:
                if (!lh.valid || !W.have_pose) break;
                g_rc.hold = interact::HoldAsIs(lh.grip, W.frame_rot, W.grip_world);
                CarryFromSupport();
                g_rc.pick_t = 1.0;
                g_rc.carried = true;
                W.right_let_go = true;
                W.in_hand = false;
                W.two_hand = false;
                W.have_support_point = false;
                ReleaseTrigger(player);
                if (g_set.holster_toggle)
                    log::Info("rifle carry: right hand let go (squeezed again); the left hand keeps the rifle, %s",
                              interact::PointName(g_rc.point));
                else
                    log::Info("rifle carry: right hand let go (its squeeze stayed under 0.20; lowest %.2f while "
                              "held); the left hand keeps the rifle, %s",
                              g_rc.right_squeeze.lowest, interact::PointName(g_rc.point));
                break;
            case A::kDrawPistol:
                if (in.right_has_rifle) {
                    if (W.two_hand && lh.valid && W.have_pose) {
                        g_rc.hold = interact::HoldAsIs(lh.grip, W.frame_rot, W.grip_world);
                        CarryFromSupport();
                        g_rc.pick_t = 1.0;
                        g_rc.carried = true;
                        log::Info("rifle carry: drawing the pistol; the left hand keeps the rifle");
                    } else if (W.have_pose) {
                        g_rc.returning = true;
                        g_rc.ret_t = 0.0;
                        g_rc.ret_loc = W.gun_set_loc;
                        g_rc.ret_rot = W.actor_rot;
                        log::Info("rifle carry: drawing the pistol; the rifle goes on the back");
                    }
                    W.right_let_go = true;
                    W.in_hand = false;
                    W.two_hand = false;
                    W.have_support_point = false;
                    ReleaseTrigger(player);
                }
                g_rc.draw_on_switch = true;
                AskSwitch(player, 1);
                break;
            case A::kRightTakesRifle:
                if (rifle_current) {
                    W.right_let_go = false;
                    W.in_hand = true;
                    if (g_rc.carried) {
                        SupportFromCarry();
                        g_rc.carried = false;
                    }
                    g_rc.returning = g_rc.parked = false;
                    input::Haptic(input::kRight, 0.4f, 0.03f);
                    log::Info("rifle carry: right hand took the rifle's grip%s",
                              W.two_hand ? "; the left hand stays on the handguard" : "");
                } else {
                    g_rc.support_on_switch = g_rc.carried;
                    AskSwitch(player, 2);
                }
                break;
            case A::kLetGo:
                g_rc.carried = false;
                g_rc.returning = true;
                g_rc.ret_t = 0.0;
                g_rc.ret_loc = g_rc.have_last ? g_rc.last_loc : ActorLocation(g_rc.rifle);
                g_rc.ret_rot = g_rc.have_last ? g_rc.last_rot : ActorRotation(g_rc.rifle);
                input::Haptic(input::kLeft, 0.2f, 0.02f);
                log::Info("rifle carry: left hand let go; the rifle goes back on the back");
                break;
            default:
                break;
        }
    }
    // The current rifle back in the right hand: HoldGun places it.
    if (rifle_current && W.in_hand) g_rc.returning = g_rc.parked = false;
    PlaceRifle(lh, dt, rifle_current);
}

void OnLateTick(UObject player, float) {
    if (!g_set.enabled) return;
    ComputeHolsterWorld();
    if (!g_late_ok) return;
    g_late_ok = false;
    if (!g_hw.valid || !W.gun || !player) return;
    game::Gun gun(W.gun);
    if (W.uses_holster && !W.in_hand && !W.edit_frozen) PlaceGunInHolster(gun);
    PlacePouchMag(W.gun);
    if (!W.edit_frozen) PlaceSparePistol();
    PlaceRifleInHolster();
}

void OnTick(UObject player, float dt) {
    g_clock += dt;
    g_dt = dt;
    input::Snapshot s = input::Consume();
    if (!s.valid || !g_set.enabled) {
        ResolveHandOwnership(false);
        ReleaseTrigger(player);
        return;
    }
    UpdateHands(s);
    // Observe releases even when a menu/editor skips SlideHandling this tick.
    // The slide stays frozen until gameplay resumes, but a UI-owned re-squeeze
    // cannot inherit admission from the old gameplay hold.
    if (!s.hand[input::kLeft].grip_button.down)
        W.slide_squeeze.Update(false, false, false);
    ResolveHandOwnership(!ui_panel::MenuOpen() && hands::EditorHand() < 0);
    // Raw input remains available to the chosen feature through LastConsumed.
    // A can/attachment also owns the trigger: it must not toggle a weapon light
    // or interact with the world while that hand is manipulating the item.
    for (int h = 0; h < 2; ++h) {
        const auto access = hands::GetInteraction(static_cast<hands::Hand>(h));
        const auto owner = access.owner;
        if (owner == hands::GripOwner::kWorkbench || owner == hands::GripOwner::kDrink) {
            s.hand[h].grip_button = input::Button{};
        }
        if (!access.AllowsControls()) {
            s.hand[h].trigger_button = input::Button{};
        }
    }
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
    TurnHandsWithView(Locomotion(player, s, dt));
    RoomScaleFollow(player);
    GameActions(player, s);
    ApplyCameraShakeSetting(player);

    RefreshCarryGuns(player);
    game::Gun gun = game::Gun::Current(player);
    if (gun.Object() != W.gun) {
        RifleCarryBeforeGunChange(gun.Object());
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
            RifleCarryOnNewGun(gun);
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
    HookKickAim(player);
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
        RifleCarry(player, rh, lh, s.hand[input::kLeft], s.hand[input::kRight], dt, false);
    } else {
        if (W.in_hand) Trigger(player, gun, s.hand[input::kRight]);
        // While the left hand holds the rifle by itself its grip belongs to
        // the rifle: no slide, magazine or support grabs.
        input::HandInput lfree = s.hand[input::kLeft];
        if (g_rc.carried) lfree.grip_button = input::Button{};
        SlideHandling(gun, lh, lfree, dt, W.in_hand);
        // Grip toggle (VR tab): a held magazine stays in the hand until the
        // next squeeze. The slide and the support hand always hold.
        const input::HandInput& lraw = lfree;
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
        SupportGrip(lh, lsup, dt);
        FireSelector(gun, s.hand[input::kRight]);
        RifleCarry(player, rh, lh, s.hand[input::kLeft], s.hand[input::kRight], dt, true);
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

void RequestGripEditor() {
    g_req_grip_editor.store(true);
    log::Info("controls: grip editor asked for from the VR tab; it opens when the menu closes");
}

void RequestHolsterMove() {
    g_req_holsters.store(true);
    log::Info("controls: holster move asked for from the VR tab; 5 s countdown when the menu closes");
}

void RequestHeightMeasure() {
    g_height.requested.store(true);
    log::Info("controls: height measure asked for from the VR tab; it starts when the menu closes");
}

void Init() {
    LoadSettings();
    LoadMagSeats();
    config::AddChangeListener(&ReloadSettings);
    game::AddTickListener(&OnTick);
    perf::AddReportLine(&WallPushReport);
    log::Info("controls: ready (%s turn, move by %s)", g_set.smooth_turn ? "smooth" : "snap",
              g_set.move_with_hand ? "hand" : "head");
}

void InitLate() {
    game::AddTickListener(&OnLateTick);
    log::Info("controls: holsters ready (pistol %s, %s; magazine pouch; to move one anywhere on the body, hold its "
              "item there at the angle you want after VR tab: Move holsters)",
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
HandInteraction GetInteraction(Hand hand) {
    HandInteraction result;
    {
        std::lock_guard<std::mutex> lock(controls::g_hands_mutex);
        result = controls::g_interaction[hand];
    }
    // The body/editor or UI may open after controls ran in this same frame.
    result.actions_allowed = result.actions_allowed && EditorHand() < 0 && !ui_panel::MenuOpen();
    return result;
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
