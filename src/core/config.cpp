#include "core/config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "core/log.h"

namespace holevr::config {
namespace {
std::map<std::string, std::string> g_values;

// Settings blocks, keyed by the setting they introduce. Blocks missing from
// an existing hole-vr.ini (from an older version) are appended on load so
// players can always see every option.
struct Block {
    const char* key;
    const char* text;
};
const Block kBlocks[] = {
    {"dump_on_start",
     "; HOLE VR settings. Lines starting with ; are comments.\n"
     "\n"
     "; Write a list of every class, function and property in the game to the\n"
     "; dump folder on startup. Needed while the mod is being developed.\n"
     "dump_on_start=1\n"},
    {"engine_wait_seconds",
     "\n"
     "; Seconds to wait for the engine to finish starting before giving up.\n"
     "engine_wait_seconds=90\n"},
    {"vr_enabled",
     "\n"
     "; 1 = play in VR (needs SteamVR running), 0 = normal flat game.\n"
     "vr_enabled=1\n"
     "\n"
     "; Render resolution relative to what the headset asks for (0.3 - 2.0).\n"
     "resolution_scale=1.0\n"
     "\n"
     "; Show the eye view in the game window on the monitor.\n"
     "desktop_mirror=1\n"
     "\n"
     "; 1 = let the game's mouse/stick pitch tilt your view (not recommended).\n"
     "use_game_pitch=0\n"
     "\n"
     "; 1 = seated tracking space instead of standing/room scale.\n"
     "seated_space=0\n"},
    {"vr_render_fixes",
     "\n"
     "; --- Effects that break in VR (1 = turn the effect off, 0 = leave it) ---\n"
     "; Master switch for everything in this section.\n"
     "vr_render_fixes=1\n"
     "vr_disable_motion_blur=1\n"
     "vr_disable_depth_of_field=1\n"
     "; Screen-edge darkening from the game's own effects. The comfort vignette\n"
     "; for turning and moving is a separate setting.\n"
     "vr_disable_vignette=1\n"
     "vr_disable_lens_flares=1\n"
     "vr_disable_chromatic_aberration=1\n"
     "vr_disable_film_grain=1\n"
     "; The player's extra scene capture renders the level a second time\n"
     "; (the game's Capture2D option). Stopping it saves a lot of GPU time.\n"
     "vr_disable_player_scene_capture=1\n"},
    {"vr_anti_aliasing",
     "\n"
     "; --- Anti-aliasing and console variables ---\n"
     "; Smoothing of jagged edges. The game's default (tsr) can leave after-images\n"
     "; and shimmer on things that move fast, like your hands and gun.\n"
     ";   fxaa = no after-images, slightly softer edges (VR default)\n"
     ";   taa  = smoother edges, some ghosting\n"
     ";   tsr  = the game's own, sharpest when still, most ghosting\n"
     ";   off  = no smoothing, sharp but jagged\n"
     ";   game = leave whatever the game set\n"
     "; Ctrl+F10 in game cycles through these to compare (not saved).\n"
     "vr_anti_aliasing=fxaa\n"
     ";\n"
     "; Any engine console variable can be set here with a cvar. prefix, e.g.\n"
     "; cvar.r.Tonemapper.Sharpen=0.5\n"},
    {"vr_controls",
     "\n"
     "; --- Motion controls ---\n"
     "; 1 = use the VR controllers, 0 = keyboard/mouse/gamepad only.\n"
     "vr_controls=1\n"
     "; Turning on the right stick: 0 = snap, 1 = smooth.\n"
     "turn_mode=1\n"
     "snap_turn_degrees=30\n"
     "; Degrees per second at full stick for smooth turning.\n"
     "smooth_turn_speed=90\n"
     "; Left stick moves toward: 0 = where you look, 1 = where your left hand points.\n"
     "move_direction=0\n"
     "stick_deadzone=0.15\n"
     "; Tilts the gun in your hand up (+) or down (-), degrees.\n"
     "gun_pitch_offset=0\n"
     "; Turns the gun left (-) or right (+), and rolls it left (-) or right (+), degrees.\n"
     "gun_yaw_offset=0\n"
     "gun_roll_offset=0\n"
     "; Moves the gun in your hand forward (+) / back (-) and right (+) / left (-), cm.\n"
     "gun_forward_offset_cm=0\n"
     "gun_right_offset_cm=0\n"
     "; 1 = the gun stops at walls instead of passing through them.\n"
     "gun_wall_collision=1\n"
     "; Strength of the gun's kick in your hand when it fires (0 = off, 1 = normal).\n"
     "gun_recoil_kick=1\n"
     "; Comfort: let the game's recoil move your view and aim (0 = off; the gun\n"
     "; still kicks in your hand), and the game's camera shake (0 = off).\n"
     "game_camera_recoil=0\n"
     "game_camera_shake=0\n"
     "; Spare magazine pouch, measured from your headset: forward, right (+) or\n"
     "; left (-), and down, in cm. Grab within the radius to take a magazine.\n"
     "mag_pouch_forward_cm=15\n"
     "mag_pouch_side_cm=-15\n"
     "mag_pouch_down_cm=55\n"
     "mag_pouch_grab_radius_cm=14\n"},
    {"fixed_height",
     "\n"
     "; 1 = your in-game height is always the character's, seated or standing\n"
     "; (real crouching does not lower you). 0 = follow your real head height.\n"
     "fixed_height=1\n"},
    {"body_ik",
     "\n"
     "; --- VR body (full-body IK and hand poses). F9 in game toggles it. ---\n"
     "; 1 = show your character's body following the headset and controllers.\n"
     "body_ik=1\n"
     "; Degrees you can turn your head before the body turns with it.\n"
     "body_turn_deadzone=45\n"
     "; How far the body may crouch and lean with you (cm), and bend at the spine.\n"
     "body_max_crouch_cm=70\n"
     "body_max_lean_cm=30\n"
     "body_max_spine_bend=45\n"
     "; Hands may follow the controller this far past full arm reach (cm).\n"
     "body_hand_reach_extra_cm=30\n"
     "; Arm length: 0 = learn it from how far you reach, or a fixed multiplier\n"
     "; such as 1.15. body_max_arm_scale caps the learned value.\n"
     "body_arm_scale=0\n"
     "body_max_arm_scale=1.25\n"
     "; Where your neck is relative to your eyes (cm). Moves the avatar's shoulders.\n"
     "body_neck_below_eyes_cm=14\n"
     "body_neck_behind_eyes_cm=8\n"
     "; Slant of a pistol grip, degrees (how the hand wraps the gun).\n"
     "hand_grip_angle=20\n"
     "; 1 = fingers curl until they touch what you hold (gun, slide, magazine).\n"
     "finger_contact=1\n"
     "; Finger thickness used for that touch, cm (bigger = fingers stop sooner).\n"
     "finger_radius_cm=0.8\n"
     "; 1 = rest your palm on the surface of what you hold (no gun inside the hand).\n"
     "hand_palm_seat=1\n"
     "; Palm thickness from the hand's centre line (cm), and where on the palm touches (0 wrist .. 1 knuckles).\n"
     "hand_palm_skin_cm=2\n"
     "hand_palm_seat_along=0.5\n"
     "; How low on the magazine your hand holds it (share of its height below the middle).\n"
     "hand_mag_grip_low=0.25\n"
     "; How far the index finger may turn to point up the front of a held magazine (degrees).\n"
     "hand_mag_index_up_max_deg=75\n"
     "; 1 = measure each pistol with rays when you pick it up and place the hand from its real shape\n"
     "; (middle finger under the trigger guard, palm on the right side, thumb along the left side).\n"
     "hand_grip_from_gun_scan=1\n"
     "; CPU time per frame spent measuring a new gun, ms (more = faster scan, lower fps while it runs).\n"
     "hand_gun_scan_ms_per_frame=2\n"
     "; Scanned grip: palm thickness from the knuckle joints (cm), middle knuckle behind the front strap (cm),\n"
     "; thumb height above the bottom of the trigger guard (cm), gap kept between skin and gun (cm).\n"
     "hand_scan_palm_skin_cm=1.5\n"
     "hand_scan_knuckle_behind_front_cm=0.5\n"
     "hand_scan_thumb_above_guard_cm=2\n"
     "hand_scan_clearance_cm=0.2\n"
     "; 1 = copy the game's own hand placement on the gun (it does not wrap the grip).\n"
     "hand_copy_game_grip=0\n"
     "; Where the fist closes on a pistol grip, measured from the gun's magazine catch (cm).\n"
     "hand_grip_behind_mag_catch_cm=2.5\n"
     "hand_grip_below_mag_catch_cm=4.5\n"
     "; High grip: middle finger this far below the trigger guard (cm). Used when the gun has a trigger guard part.\n"
     "hand_grip_below_trigger_guard_cm=1\n"
     "; How far the thumb may turn to point forward along the frame (degrees).\n"
     "hand_thumb_forward_max_deg=60\n"
     "; Sideways (+ = toward the gun's right side).\n"
     "hand_grip_side_cm=0\n"
     "; Where the grip sits inside your hand: out from the palm (cm), and how far\n"
     "; from the wrist toward the knuckles (0-1).\n"
     "hand_grip_palm_depth_cm=2.5\n"
     "hand_grip_fist_along=0.75\n"
     "; Only for guns without a magazine catch: fist offset from the game's grip point (cm).\n"
     "hand_gun_fallback_forward_cm=3.5\n"
     "hand_gun_fallback_right_cm=-3.8\n"
     "hand_gun_fallback_up_cm=6.2\n"
     "; How much of a wrist twist the forearm rolls with (0-1). Higher = less pinching.\n"
     "body_wrist_twist_share=0.75\n"
     "; 1 = let the game's animation (recoil, aim sway) move your spine and shoulders.\n"
     "body_animated_upper_body=0\n"
     "; Keep the feet planted with leg IK while crouching/turning.\n"
     "body_leg_ik=1\n"
     "; Hide the character's head so it never blocks the view.\n"
     "body_hide_head=1\n"
     "; Shadow: 0 none, 1 the game's animated body (has a head), 2 the VR body.\n"
     "body_shadow=1\n"
     "; Finger poses for gun grip, trigger, slide and magazine (1 = on).\n"
     "hand_poses=1\n"
     "finger_curl_scale=1.0\n"
     "; Fine-tune where the avatar's hand sits on the controller.\n"
     "hand_offset_forward_cm=0\n"
     "hand_offset_right_cm=0\n"
     "hand_offset_up_cm=0\n"
     "hand_pitch_offset=0\n"
     "hand_yaw_offset=0\n"
     "hand_roll_offset=0\n"
     "hand_palm_depth_cm=3\n"},
};

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

const std::string* Find(const char* key) {
    auto it = g_values.find(key);
    return it == g_values.end() ? nullptr : &it->second;
}
}  // namespace

void Load(const std::wstring& mod_dir) {
    std::wstring path = mod_dir + L"\\hole-vr.ini";
    FILE* f = _wfopen(path.c_str(), L"r");
    if (!f) {
        if (FILE* out = _wfopen(path.c_str(), L"w")) {
            for (const Block& b : kBlocks) fputs(b.text, out);
            fclose(out);
            log::Info("Created default hole-vr.ini");
        }
        f = _wfopen(path.c_str(), L"r");
        if (!f) return;
    }
    char buf[1024];
    while (fgets(buf, sizeof(buf), f)) {
        std::string line = Trim(buf);
        if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        g_values[Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
    }
    fclose(f);
    log::Info("Loaded %zu settings from hole-vr.ini", g_values.size());

    int added = 0;
    for (const Block& b : kBlocks) {
        if (Find(b.key)) continue;
        if (FILE* out = _wfopen(path.c_str(), L"a")) {
            fputs(b.text, out);
            fclose(out);
            ++added;
        }
    }
    if (added) log::Info("Added %d new setting groups to hole-vr.ini", added);
}

bool GetBool(const char* key, bool fallback) {
    const std::string* v = Find(key);
    if (!v || v->empty()) return fallback;
    char c = (*v)[0];
    return c == '1' || c == 't' || c == 'T' || c == 'y' || c == 'Y';
}

int GetInt(const char* key, int fallback) {
    const std::string* v = Find(key);
    return (v && !v->empty()) ? std::atoi(v->c_str()) : fallback;
}

float GetFloat(const char* key, float fallback) {
    const std::string* v = Find(key);
    return (v && !v->empty()) ? static_cast<float>(std::atof(v->c_str())) : fallback;
}

std::string GetString(const char* key, const char* fallback) {
    const std::string* v = Find(key);
    return (v && !v->empty()) ? *v : std::string(fallback);
}

std::vector<std::pair<std::string, std::string>> WithPrefix(const char* prefix) {
    std::vector<std::pair<std::string, std::string>> out;
    const size_t n = std::strlen(prefix);
    for (const auto& [k, v] : g_values)
        if (k.size() > n && k.compare(0, n, prefix) == 0) out.emplace_back(k.substr(n), v);
    return out;
}

}  // namespace holevr::config
