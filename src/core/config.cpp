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
    {"menu_panel",
     "\n"
     "; --- Menus in the headset ---\n"
     "; While a game menu is open, the game screen floats in front of you and\n"
     "; either controller is a laser pointer: trigger clicks, thumbstick scrolls,\n"
     "; B/Y goes back. The controller menu button opens/closes the pause menu.\n"
     "menu_panel=1\n"
     "menu_panel_width_m=1.6\n"
     "menu_panel_distance_m=1.6\n"
     "; How far below eye height the middle of the screen sits.\n"
     "menu_panel_drop_m=0.1\n"
     "; Turn further away than this and the screen comes back in front of you.\n"
     "menu_panel_follow_deg=60\n"
     "menu_button_pauses=1\n"},
    {"menu_mode",
     "\n"
     "; --- Menus as panels in the world ---\n"
     "; world  = each game menu is taken off the flat screen and shown on its own\n"
     ";          panel floating in front of you (like other VR games)\n"
     "; window = the old way: the whole game screen on a flat panel\n"
     "menu_mode=world\n"
     "menu_world_width_m=1.8\n"
     "menu_world_distance_m=1.6\n"
     "menu_world_drop_m=0.15\n"
     "; Darkness behind the menu, 0 (none, just the game's own art) to 1 (solid)\n"
     "menu_world_backdrop=0.55\n"
     "; Laser from the controller to the menu\n"
     "menu_world_laser=1\n"},
    {"menu_world_place",
     "\n"
     "; --- Menu on your forearm, status on your hand ---\n"
     "; forearm = game menus lie on the back of the wrist_hud_hand forearm, like a\n"
     ";           big watch face; point with the other hand's laser\n"
     "; front   = game menus float in front of you\n"
     "menu_world_place=forearm\n"
     "menu_forearm_width_m=0.30\n"
     "; From the controller grip toward the elbow, and out from the arm\n"
     "menu_forearm_along_m=0.20\n"
     "menu_forearm_back_m=0.05\n"
     "; Darkness behind the menu, 0 = none (just the menu itself)\n"
     "menu_world_backdrop_v2=0\n"
     "; Hide the menus' full-screen focus cover (an invisible box over everything)\n"
     "menu_hide_focus_cover=1\n"
     "; Ammo and health over the back of the hand (from the grip, out and along)\n"
     "hand_hud_offset_back_m=0.045\n"
     "hand_hud_offset_along_m=0.0\n"},
    {"wrist_hud",
     "\n"
     "; --- Wrist display ---\n"
     "; Ammo (magazine+chamber), reload hints and health on your wrist, shown\n"
     "; when you turn the wrist toward your face like a watch.\n"
     "wrist_hud=1\n"
     "; left or right wrist\n"
     "wrist_hud_hand=left\n"
     "; 1 = always visible instead of only when you look at it\n"
     "wrist_hud_always=0\n"
     "wrist_hud_show_distance_m=0.5\n"
     "wrist_hud_show_angle_deg=40\n"
     "wrist_hud_width_m=0.10\n"
     "wrist_hud_offset_back_m=0.035\n"
     "wrist_hud_offset_elbow_m=0.10\n"
     "; 0 = no ammo count anywhere, for a harder, more realistic game\n"
     "wrist_hud_ammo=1\n"
     "; Hide the flat crosshair, ammo, reload hint, barrel heat and health\n"
     "; parts of the game's HUD (the wrist shows them instead).\n"
     "hud_hide_flat_parts=1\n"
     "; Controllers pulse like a heartbeat when health is low.\n"
     "low_health_heartbeat=1\n"
     "low_health_below=0.3\n"},
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
     "; Rifle virtual stock: bring the butt near your shoulder and the rifle pivots\n"
     "; from it (1 = on). Snap/release distances in cm, and shoulder pocket\n"
     "; adjustments in cm (forward, right, up) if the stock sits wrong.\n"
     "rifle_virtual_stock=1\n"
     "rifle_stock_snap_cm=12\n"
     "rifle_stock_release_cm=20\n"
     "rifle_stock_forward_cm=0\n"
     "rifle_stock_right_cm=0\n"
     "rifle_stock_up_cm=0\n"
     "; Room-scale: the character follows you when you walk around the room\n"
     "; (1 = on; walls stop it).\n"
     "room_scale_follow=1\n"
     "; Aiming eye: 1 = right, 0 = left.\n"
     "dominant_eye_right=1\n"
     "; Spare magazine pouch, measured from your headset: forward, right (+) or\n"
     "; left (-), and down, in cm. Grab within the radius to take a magazine.\n"
     "mag_pouch_forward_cm=15\n"
     "mag_pouch_side_cm=-15\n"
     "mag_pouch_down_cm=55\n"
     "mag_pouch_grab_radius_cm=14\n"
     "; Pistol slide can be grabbed this far forward of its rear (front serrations, press checks)\n"
     "slide_grab_forward_cm=16\n"
     "; ...and pinched from this far behind its rear, on the bore line\n"
     "slide_grab_back_cm=15\n"},
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
     "; How far the body may crouch and lean with you (cm), and bend at the spine.\n"
     "body_max_crouch_cm=70\n"
     "body_max_lean_cm=30\n"
     "body_max_spine_bend=45\n"
     "; 1 = let the game's animation (recoil, aim sway) move your spine and shoulders.\n"
     "body_animated_upper_body=0\n"
     "; Keep the feet planted with leg IK while crouching/turning.\n"
     "body_leg_ik=1\n"
     "; Hide the character's head so it never blocks the view.\n"
     "body_hide_head=1\n"
     "; Shadow: 0 none, 1 the game's own animated body (out of step with you), 2 the VR body.\n"
     "body_shadow_source=2\n"
     "; Finger poses for gun grip, trigger, slide and magazine (1 = on).\n"
     "hand_poses=1\n"
     "finger_curl_scale=1.0\n"
     "; Fine-tune where the avatar's hand sits on the controller (empty hands).\n"
     "hand_offset_forward_cm=0\n"
     "hand_offset_right_cm=0\n"
     "hand_offset_up_cm=0\n"
     "hand_pitch_offset=0\n"
     "hand_yaw_offset=0\n"
     "hand_roll_offset=0\n"
     "hand_palm_depth_cm=3\n"},
    {"body_turn_threshold_deg",
     "\n"
     "; --- VR body, second version (2026-11-01) ---\n"
     "; Degrees you can turn your head before the body turns with it.\n"
     "body_turn_threshold_deg=25\n"
     "; How much the body also turns toward where your hands are (0-1).\n"
     "body_turn_toward_hands=0.3\n"
     "; Arm length. Measure it once: stand straight, hold both arms straight out\n"
     "; to the sides and hold the LEFT STICK click for 2 seconds (keep the pose\n"
     "; for 3 more seconds). Saved in hole-vr/body-measure.ini.\n"
     "; body_arm_scale_override: 0 = use the measurement, or a fixed multiplier.\n"
     "body_arm_scale_override=0\n"
     "; Most the arms may stretch past their length to keep the hands on the\n"
     "; controllers: after measuring, and before.\n"
     "body_max_arm_stretch=1.04\n"
     "body_max_arm_stretch_unmeasured=1.15\n"
     "; Shoulders below and behind your eyes, cm. 0 = from your eye height.\n"
     "body_shoulder_below_eyes_cm=0\n"
     "body_shoulder_behind_eyes_cm=0\n"
     "; Forearm roll: share of the hand's twist taken by the forearm bone and by\n"
     "; the forearm twist bone (less pinching at the wrist).\n"
     "body_forearm_roll_share=0.5\n"
     "body_forearm_twist_bone_share=1.0\n"
     "; How much the elbow follows the hand's turn (0-1), and whether it may\n"
     "; swing further to keep the wrist from bending too far.\n"
     "body_elbow_follow_hand=0.7\n"
     "body_elbow_relieves_wrist=1\n"
     "; Holding the pistol: the elbow goes where the wrist stays straightest\n"
     "; (forearm in line behind the gun).\n"
     "body_gun_wrist_straight=1\n"
     "; 1 = stored grips (hole-vr/grips, tuned with the grip editor: hold left Y\n"
     "; for 1 s while holding the gun). 0 = simple finger curls only.\n"
     "hand_grip_records=1\n"
     "; Finger thickness the grip solver keeps out of the gun, cm.\n"
     "hand_finger_radius_cm=0.5\n"
     "; Grip editor: show a dot on each fingertip and the palm (the one a\n"
     "; squeeze would take grows) and a dot on the editing controller.\n"
     "hand_editor_show_grab_points=1\n"
     "; Log a body fit line (reach, wrist bend, body turn) every 10 s.\n"
     "body_fit_log=1\n"},
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

namespace {
std::wstring g_dir;
}
const std::wstring& Dir() { return g_dir; }

void Load(const std::wstring& mod_dir) {
    g_dir = mod_dir;
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
