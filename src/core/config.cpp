#include "core/config.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <map>
#include <mutex>
#include <string>

#include "core/ini_edit.h"
#include "core/log.h"

namespace holevr::config {
namespace {
// Read from the game thread every frame (and at start-up from others);
// changed from the game thread by Set.
std::mutex g_mutex;
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
    {"title_auto_start",
     "\n"
     "; 1 = the first time the title screen comes up, press START for you before\n"
     "; it shows, so the game starts with it closed. 0 = leave the title alone.\n"
     "title_auto_start=1\n"},
    {"vr_render_fixes",
     "\n"
     "; --- Effects that break in VR (1 = turn the effect off, 0 = leave it) ---\n"
     "; Master switch for everything in this section.\n"
     "vr_render_fixes=1\n"
     "vr_disable_motion_blur=1\n"
     "vr_disable_depth_of_field=1\n"
     "; Screen-edge darkening from the game's own effects. The comfort vignette\n"
     "; for turning and moving is a separate setting (comfort_vignette_*).\n"
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
     "; Game menus go on your forearm (see menu_world_place below); the other\n"
     "; hand is a laser pointer: trigger clicks, thumbstick scrolls, B/Y goes\n"
     "; back. The controller menu button opens/closes the pause menu.\n"
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
     "; world = each game menu is taken off the flat screen and put in the world\n"
     ";         (on your forearm unless menu_world_place=front)\n"
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
    {"menu_submenu_replaces",
     "\n"
     "; --- Sub-menus on the forearm (Options, Yes/No, Task, Credits) ---\n"
     "; 1 = a sub-menu takes the place of the menu it came from on your arm, and\n"
     ";     that menu comes back when you leave the sub-menu\n"
     "; 0 = both stay on the arm, the sub-menu just in front\n"
     "menu_submenu_replaces=1\n"},
    {"menu_forearm_size",
     "\n"
     "; How big the forearm menus are, times menu_forearm_width_m (1.25 = 25%\n"
     "; bigger than 0.30 m, what the owner asked for on 2026-11-02)\n"
     "menu_forearm_size=1.25\n"
     "; 1 = hide the see-through grey box some menus draw behind their buttons\n"
     "; (Options has one); it shows as a shadowy box on the arm\n"
     "menu_hide_backdrops=1\n"},
    {"menu_laser_own_hit",
     "\n"
     "; --- Laser on forearm menus ---\n"
     "; 1 = the laser tells the menu where it hits, so nothing in the level can\n"
     ";     block it (the area select menu in the pit after the microwave fall\n"
     ";     could not be clicked with 0)\n"
     "; 0 = the old way: the game's collision is checked along the beam\n"
     "menu_laser_own_hit=1\n"
     "; 1 = the menu hand buzzes when the game puts a menu on your arm\n"
     "menu_arrive_buzz=1\n"},
    {"menu_always_on_top",
     "\n"
     "; --- Seeing forearm menus wherever your arm is ---\n"
     "; 1 = forearm menus are drawn on top of everything in the game, so walls,\n"
     ";     your own body or a box around your head can't cover them\n"
     "menu_always_on_top=1\n"
     "; The black box the game puts around your head (the pit after the\n"
     "; microwave fall) covered the menu once your arm left it. While a menu is\n"
     "; on your arm: grow = make the box bigger than your arm's reach (still\n"
     "; black), hide = hide the box, off = leave it alone\n"
     "menu_black_box=grow\n"},
    {"menu_fit_content",
     "\n"
     "; 1 = zoom each forearm menu so the part holding its buttons fills the\n"
     "; menu's space on the arm (bigger text; text and boxes grow together)\n"
     "menu_fit_content=1\n"
     "; Most a menu is zoomed (1 = never)\n"
     "menu_fit_max_zoom=3\n"},
    {"menu_text_outline",
     "\n"
     "; Black outline around the text of menus on the arm, so it reads over a\n"
     "; bright scene (layout pixels; 0 = off)\n"
     "menu_text_outline=2\n"},
    {"hud_on_forearm",
     "\n"
     "; 1 = the game's in-level HUD (kill feed, objectives, money, prompts) sits\n"
     "; on your forearm where the menus go, and shows when you look at your arm.\n"
     "; 0 = it stays on the flat screen (stuck to your face in the headset)\n"
     "hud_on_forearm=1\n"
     "; Its size, times the menu's size on the arm\n"
     "hud_forearm_size=1.0\n"
     "; It shows when the arm faces you within this many degrees, this close to your eyes\n"
     "hud_forearm_look_deg=50\n"
     "hud_forearm_distance_m=0.8\n"},
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
     "; left hand this close to the gun (cm): left trigger toggles the weapon light instead of interacting\n"
     "light_hand_reach_cm=12\n"
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
     "; Pistol slide can be grabbed this far forward of its rear (front serrations, press checks)\n"
     "slide_grab_forward_cm=16\n"
     "; Pinch at the rear sight: radius, and how far it stretches back along the slide (cm)\n"
     "slide_pinch_radius_cm=6.5\n"
     "slide_pinch_back_cm=0.64\n"},
    {"holster_pistol",
     "\n"
     "; --- Holsters on your VR body ---\n"
     "; Pistol on the right hip: hold the right grip at the hip to draw it and keep\n"
     "; holding it. Let go at the hip to holster it; let go anywhere else and it goes\n"
     "; back by itself. 1 = on, 0 = the pistol is always in your hand (old way).\n"
     "holster_pistol=1\n"
     "; 1 = squeeze once to draw and once more at the hip to holster, instead of holding.\n"
     "holster_pistol_grip_toggle=0\n"
     "; Grip tilt toward the front, degrees.\n"
     "holster_pistol_cant_deg=10\n"
     "; Where the holsters sit, in cm from your belt: forward, right (+) or left (-),\n"
     "; up (+) or down (-), and how close the hand must be to take or put back.\n"
     "; Easier: hold the pistol and/or a magazine anywhere on your body (hip, chest,\n"
     "; armpit, thigh, back), at the angle you want it to hang, and hold the right\n"
     "; stick click for 2 s. It then rides on the nearest body part. That is saved\n"
     "; in hole-vr\\holsters.ini, which wins over these lines (delete it to use\n"
     "; these again).\n"
     "holster_pistol_forward_cm=0\n"
     "holster_pistol_right_cm=21\n"
     "holster_pistol_up_cm=0\n"
     "holster_pistol_radius_cm=12\n"
     "; Spare magazine pouch on the left hip (always holds a magazine; let go of a\n"
     "; magazine at the pouch to put it back).\n"
     "holster_mag_forward_cm=5\n"
     "holster_mag_right_cm=-18\n"
     "holster_mag_up_cm=0\n"
     "holster_mag_radius_cm=14\n"
     "; With the VR body off (F9), the belt is taken this far below the headset.\n"
     "holster_belt_below_head_cm=60\n"},
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
    {"comfort_vignette_move",
     "\n"
     "; --- Comfort and the VR tab (2026-11-03) ---\n"
     "; These can all be changed in the game: pause, Options, VR tab. Each click\n"
     "; on a row there picks the next choice and saves it here at once.\n"
     "; Comfort vignette: the edges of your view darken while the stick moves\n"
     "; or turns you (0 = off .. 1 = strongest). Real walking never darkens it.\n"
     "comfort_vignette_move=0\n"
     "comfort_vignette_turn=0\n"
     "; 1 = the view fades to black when your head goes into a wall or object.\n"
     "head_wall_fade=1\n"
     "; Height: fixed = always the character's eye height (real crouching does\n"
     "; not lower you unless physical_crouch crouches the character),\n"
     "; real = your own head height, seated = the character's height and\n"
     "; physical crouch and jump off.\n"
     "height_mode=fixed\n"
     "; 1 = crouching for real makes the character crouch (and standing up\n"
     "; stands it up). How far your head must drop, cm.\n"
     "physical_crouch=1\n"
     "physical_crouch_cm=35\n"
     "; 1 = a quick hop (head rising fast) makes the character jump.\n"
     "physical_jump=0\n"
     "; 1 = squeeze once to take the pistol or a magazine and once more to let\n"
     "; go, instead of holding the grip (slide and support hand stay hold).\n"
     "grip_toggle=0\n"
     "; Steadier aim: smooths small shakes of the gun hand, never big moves\n"
     "; (0 = off .. 1 = strongest).\n"
     "aim_smoothing=0\n"},
    {"world_scale",
     "\n"
     "; smooth = the left stick walks; teleport = push the left stick forward to\n"
     "; aim an arc, let go to jump there, pull it back to cancel.\n"
     "locomotion=smooth\n"
     "; Which stick moves you: left (default) or right (the other one turns,\n"
     "; jumps and crouches; the moving stick's click sprints).\n"
     "move_stick=left\n"
     "; How big the world looks: 1 = true size, 1.1 = 10% bigger (you feel\n"
     "; smaller), 0.9 = 10% smaller (0.5 - 2).\n"
     "world_scale=1\n"
     "; Your standing eye height, cm, from Calibrate body on the VR tab (stand\n"
     "; straight, arms out). 0 = learned while you play. Real crouch uses it.\n"
     "standing_height_cm=0\n"},
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
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::string* v = Find(key);
    if (!v || v->empty()) return fallback;
    char c = (*v)[0];
    return c == '1' || c == 't' || c == 'T' || c == 'y' || c == 'Y';
}

int GetInt(const char* key, int fallback) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::string* v = Find(key);
    return (v && !v->empty()) ? std::atoi(v->c_str()) : fallback;
}

float GetFloat(const char* key, float fallback) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::string* v = Find(key);
    return (v && !v->empty()) ? static_cast<float>(std::atof(v->c_str())) : fallback;
}

std::string GetString(const char* key, const char* fallback) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::string* v = Find(key);
    return (v && !v->empty()) ? *v : std::string(fallback);
}

namespace {
std::atomic<ChangeFn> g_listeners[16];

bool ReadFile(const std::wstring& path, std::string* out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    out->clear();
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
    fclose(f);
    return true;
}
}  // namespace

bool Set(const char* key, const std::string& value) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_values[key] = value;
    }
    if (g_dir.empty()) return false;
    const std::wstring path = g_dir + L"\\hole-vr.ini";
    std::string text;
    if (!ReadFile(path, &text)) {
        log::Warn("settings: could not read hole-vr.ini to save %s=%s", key, value.c_str());
        return false;
    }
    std::string changed = SetIniValue(text, key, value);
    if (changed == text) return true;
    // Written beside it first, then swapped in, so a crash mid-write never
    // leaves a half-written settings file.
    const std::wstring tmp = path + L".new";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) {
        log::Warn("settings: could not write hole-vr.ini to save %s=%s", key, value.c_str());
        return false;
    }
    bool ok = fwrite(changed.data(), 1, changed.size(), f) == changed.size();
    ok = fclose(f) == 0 && ok;
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) {
        log::Warn("settings: saving %s=%s to hole-vr.ini failed", key, value.c_str());
        return false;
    }
    log::Info("settings: saved %s=%s in hole-vr.ini", key, value.c_str());
    return true;
}

void AddChangeListener(ChangeFn fn) {
    for (auto& slot : g_listeners) {
        ChangeFn cur = slot.load();
        if (cur == fn) return;
        ChangeFn expected = nullptr;
        if (!cur && slot.compare_exchange_strong(expected, fn)) return;
    }
    log::Error("settings: too many change listeners");
}

void NotifyChanged() {
    for (auto& slot : g_listeners)
        if (ChangeFn fn = slot.load()) fn();
}

std::vector<std::pair<std::string, std::string>> WithPrefix(const char* prefix) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<std::pair<std::string, std::string>> out;
    const size_t n = std::strlen(prefix);
    for (const auto& [k, v] : g_values)
        if (k.size() > n && k.compare(0, n, prefix) == 0) out.emplace_back(k.substr(n), v);
    return out;
}

}  // namespace holevr::config
