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
    {"hand_world_trace_complex",
     "\n"
     "; Diagnostic surface mode shared by empty-hand palm and finger checks.\n"
     "; 0 = the game's simple collision, 1 = complex mesh collision.\n"
     "; Compare only when visible surfaces lack contact; this does not create\n"
     "; collision on meshes that disable it, and detailed queries may cost more.\n"
     "hand_world_trace_complex=0\n"},
    {"hand_individual_finger_collision",
     "\n"
     "; Empty fingers check each of their three segments against nearby objects.\n"
     "; Joints bend independently; requires both hand_wall_collision and\n"
     "; hand_fingers_rest_on_walls.\n"
     "; 0 = use the older shared wall-plane response.\n"
     "hand_individual_finger_collision=1\n"
     "; Maximum collision checks per hand per frame (16-64).\n"
     "; Lower budgets reduce displayed-pose and fast-motion coverage.\n"
     "hand_finger_query_budget=32\n"
     "; Refresh even when your hand is still (5-50 milliseconds).\n"
     "hand_finger_contact_refresh_ms=50\n"},
    {"hand_thumb_opposition",
     "\n"
     "; Empty thumb turns its pad toward the index finger for a pinch or grip.\n"
     "; Saved weapon grips keep their authored thumb pose.\n"
     "hand_thumb_opposition=1\n"},
    {"hand_follow_hz",
     "\n"
     "; Empty hands follow with bounded spring resistance and slide on surfaces.\n"
     "; Higher follow rate feels firmer; damping controls settling.\n"
     "hand_follow_hz=10\n"
     "hand_follow_damping=1\n"
     "hand_follow_max_force_n=65\n"
     "hand_follow_max_speed_cm_s=700\n"
     "hand_surface_friction=5\n"
     "; Large persistent separation enters recovery while still respecting walls.\n"
     "hand_recovery_distance_cm=40\n"},
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
    {"menu_text_scale",
     "\n"
     "; Menu text on the arm this many times the game's size (1 = the game's).\n"
     "; A text that wouldn't fit its box or would cover another is made\n"
     "; smaller again, just enough to fit\n"
     "menu_text_scale=1.5\n"
     "; 1 = a tall narrow menu (the pause menu) gets a tall narrow panel of the\n"
     "; same area, so it can be zoomed further\n"
     "menu_fit_shape=1\n"},
    {"menu_text_min_px",
     "\n"
     "; Small menu text on the arm is made at least this big (layout pixels; the\n"
     "; game's smallest is 12), big titles no bigger than menu_text_max_px, and\n"
     "; nothing more than menu_text_max_scale times the game's size. Texts that\n"
     "; wouldn't fit are made smaller again\n"
     "menu_text_min_px=32\n"
     "menu_text_max_px=64\n"
     "menu_text_max_scale=2.5\n"
     "; At most this many pictures of each menu's panel per run, when\n"
     "; menu_snapshot_diagnostics=1\n"
     "menu_snapshots=2\n"},
    {"menu_snapshot_diagnostics",
     "\n"
     "; 1 = save pictures of menu panels beside the log (EXR), to check their\n"
     "; layout. Each one holds the game up for about 0.1 s (a visible hitch)\n"
     "menu_snapshot_diagnostics=0\n"},
    {"menu_fit_min_zoom",
     "\n"
     "; Menus that fill the whole screen (death screen, Options, credits, task\n"
     "; cards) are made at least this much bigger on the arm than the whole\n"
     "; screen would be (1 = no bigger)\n"
     "menu_fit_min_zoom=1.3\n"},
    {"menu_follow_exposure",
     "\n"
     "; The game brightens the picture in dark areas, and the arm menus are in the\n"
     "; world, so they got brightened too (death screen washed out). 1 = dim the\n"
     "; menus and HUD by the same amount so they look the same everywhere\n"
     "menu_follow_exposure=1\n"
     "; All arm menus and the HUD, times this (1 = as the game draws them)\n"
     "menu_brightness=1\n"},
    {"menu_full_brightness_at",
     "\n"
     "; Game exposure (stops) at which the arm menus keep their own colours;\n"
     "; each stop brighter halves them. Lit rooms are about +1.1, dark areas\n"
     "; about +2.6 (higher = brighter menus everywhere, lower = dimmer)\n"
     "menu_full_brightness_at=-1\n"
     "; Never dimmer than this\n"
     "menu_dimmest=0.03\n"},
    {"menu_move_parts_apart",
     "\n"
     "; 1 = a column or row of a menu that runs into a picture or divider line\n"
     "; is moved clear of it (level select, credits), and rows of names get\n"
     "; wider gaps\n"
     "menu_move_parts_apart=1\n"},
    {"hole_buzz",
     "\n"
     "; 1 = both hands tick softly each second while the hole in the floor counts\n"
     "; down (stay in it), and buzz once as you drop in\n"
     "hole_buzz=1\n"},
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
     "; 1 = also let the game's own keyboard/gamepad movement through (Steam Input can turn the VR\n"
     "; controllers into a gamepad the game reads, which walked the character on its own). 0 = off.\n"
     "game_move_input=0\n"
     "; 1 = also let the game's own mouse/gamepad look through. In VR it pitched the game's aim, bending\n"
     "; the body's back or sinking it into a crouch under the VR body. 0 = off.\n"
     "game_look_input=0\n"
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
     "; 1 = empty hands stop at walls, floors, doors and tables instead of passing\n"
     "; through them (the controller still grabs and opens things where it really is).\n"
     "hand_wall_collision=1\n"
     "; How far past the fist the fingertips feel for a wall (cm, 0 = off).\n"
     "hand_wall_probe_cm=14\n"
     "; 1 = fingers of a hand against a wall or table give way on it like physics\n"
     "; fingers (bend back where their pads press, curl where their backs press)\n"
     "; instead of going in.\n"
     "hand_fingers_rest_on_walls=1\n"
     "; Legacy plane mode: fingertips trail as the hand slides (0 = off,\n"
     "; 1 = normal, 2 = twice as much).\n"
     "hand_finger_drag=1\n"
     "; Legacy plane mode: an empty hand pushed against a wall settles flat on it\n"
     "; once it touches (the side or back of the hand keeps its angle). It is flat\n"
     "; once the real hand is this far (cm) past where the drawn hand stopped.\n"
     "hand_press_flat_on_walls=1\n"
     "hand_press_flat_push_cm=4\n"
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
     "; How far your aiming eye sits behind the rear sight when shouldered (cm).\n"
     "rifle_eye_relief_cm=9\n"
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
     "slide_pinch_back_cm=14\n"},
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
    {"holster_model",
     "\n"
     "; --- Fitted pistol holster ---\n"
     "; Original model follows the existing pistol spot in holsters.ini.\n"
     "holster_model=1\n"
     "; Blend the visible free right hand onto its saved pistol grip nearby.\n"
     "; Draw/stow inputs are unchanged; an empty hood stays open.\n"
     "holster_hand_snap=1\n"},
    {"holster_sounds",
     "\n"
     "; --- Pistol holster audio ---\n"
     "; Four mono PCM16 WAVs in sounds/holster follow the game's audio output.\n"
     "holster_sounds=1\n"
     "; Separate sound level, 0 (silent) to 1; uses the Windows game mixer.\n"
     "holster_sound_volume=0.8\n"},
    {"watch_enabled",
     "\n"
     "; --- Wrist watch ---\n"
     "; Local analog clock and retained level timer on the left wrist.\n"
     "; Raise and look at it, then tap with a free right index to open pause.\n"
     "watch_enabled=1\n"},
    {"holster_rifle",
     "\n"
     "; --- Rifle holster ---\n"
     "; 1 = the rifle rests behind your right shoulder when you don't hold it: reach\n"
     "; back over the shoulder and squeeze the right grip to take it. 0 = it rests\n"
     "; where the game hangs it. Move it anywhere with VR tab > Move holsters while\n"
     "; holding the rifle (saved in hole-vr\\holsters.ini, which wins over these).\n"
     "holster_rifle=1\n"
     "; Where the hand grabs it, in cm from the top of your spine: forward, right\n"
     "; (+) or left (-), up (+) or down (-), and how close the hand must be.\n"
     "holster_rifle_forward_cm=-14\n"
     "holster_rifle_right_cm=14\n"
     "holster_rifle_up_cm=6\n"
     "holster_rifle_radius_cm=18\n"},
    {"rifle_left_hand_carry",
     "\n"
     "; --- Rifle in your left hand while the pistol is out ---\n"
     "; 1 = the left hand can hold the rifle by its handguard on its own: let go of\n"
     "; the rifle's grip (or draw the pistol) with the left hand on the handguard and\n"
     "; the left hand keeps it; or grab the handguard of the rifle on your back. Let\n"
     "; go and it goes back on your back. Grab its grip with the right hand to fire it.\n"
     "rifle_left_hand_carry=1\n"
     "; How the rifle sits in the left hand when picked up from your back: the bore\n"
     "; this far above your fist (cm), and turned (degrees: + pitch tips the muzzle up).\n"
     "rifle_carry_fist_below_bore_cm=4\n"
     "rifle_carry_pitch_deg=0\n"
     "rifle_carry_yaw_deg=0\n"
     "rifle_carry_roll_deg=0\n"
     "; The right hand takes the rifle when it squeezes this close to its grip (cm).\n"
     "rifle_grip_reach_cm=14\n"},
    {"rifle_grip_points",
     "\n"
     "; --- Where the left hand holds a rifle's handguard ---\n"
     "; 1 = three grip points: just in front of the magazine, where the scope's top\n"
     "; rail ends, and just behind the light. The left hand takes the nearest one, and\n"
     "; each has its own hand pose in the grip editor. 0 = the hand holds wherever it grabs.\n"
     "rifle_grip_points=1\n"
     "; How far the middle of the hand sits behind the light (cm; half a hand's width).\n"
     "rifle_grip_point_inset_cm=4.5\n"},
    {"rifle_right_let_go",
     "\n"
     "; --- Letting go of the rifle with the right hand ---\n"
     "; 1 = open the right hand on the rifle (the left hand not on it) and the rifle\n"
     "; goes back to its holster by itself, like the pistol. With the left hand on the\n"
     "; handguard the left hand keeps it. 0 = the rifle stays in the right hand.\n"
     "rifle_right_let_go=1\n"},
    {"rifle_charging_handle_grab_cm",
     "\n"
     "; --- The rifle's charging handle ---\n"
     "; Either hand grabs the charging handle at its rear end when it squeezes this\n"
     "; close to it (cm). The right hand can do it while the left hand holds the rifle\n"
     "; by the handguard. Each hand has its own pose in the grip editor.\n"
     "rifle_charging_handle_grab_cm=9\n"},
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
     "; Ground-aware idle feet and room steps. Preserve moving strides and kicks.\n"
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
     "; --- VR body calibration and pose constraints ---\n"
     "; Degrees you can turn your head before the body turns with it.\n"
     "body_turn_threshold_deg=25\n"
     "; How much the body also turns toward where your hands are (0-1).\n"
     "body_turn_toward_hands=0.3\n"
     "; Calibrate body in VR Options with both hands free, or hold LEFT STICK 2 s.\n"
     "; Follow the overhead -> slow straight-arm T -> bent-elbow T instructions.\n"
     "; Saved in body-calibration.ini; failed attempts preserve the previous fit.\n"
     "; body_arm_scale_override: 0 = use the measurement, or a fixed multiplier.\n"
     "body_arm_scale_override=0\n"
     "; Legacy-fit arm stretch limits. The new calibrated segments do not stretch.\n"
     "body_max_arm_stretch=1.04\n"
     "body_max_arm_stretch_unmeasured=1.15\n"
     "; Fallback shoulder offsets before calibration. 0 = from standing height.\n"
     "body_shoulder_below_eyes_cm=0\n"
     "body_shoulder_behind_eyes_cm=0\n"
     "; Forearm roll: share of the hand's twist taken by the forearm bone and by\n"
     "; the forearm twist bone (less pinching at the wrist).\n"
     "body_forearm_roll_share=0.5\n"
     "body_forearm_twist_bone_share=1.0\n"
     "; Optional wrist relief for recorded gun grips only. Free-hand elbow\n"
     "; placement follows the body and reach, independently of hand rotation.\n"
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
     "; The fade begins this close to a wall or object (cm) and is black this\n"
     "; close to it, so you never see through.\n"
     "head_wall_fade_start_cm=15\n"
     "head_wall_fade_full_cm=2\n"
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
     "; Physical standing eye height, cm, measured in Your Own height mode.\n"
     "; Seated calibration stores its own reference. 0 = unknown; real crouch\n"
     "; learns a standing reference while you play. Character height is separate.\n"
     "standing_height_cm=0\n"},
    {"monitor_view",
     "\n"
     "; What the game window on the monitor shows, and so what OBS or the NVIDIA\n"
     "; app records: right = the right eye's view as a normal 16:9 picture,\n"
     "; left = the left eye's, both = both eyes side by side (the old way).\n"
     "; The headset picture is the same either way.\n"
     "monitor_view=right\n"
     "; Shape of the one-eye view, width / height (1.778 = 16:9).\n"
     "monitor_view_aspect=1.778\n"},
    {"workbench_hands",
     "\n"
     "; --- Workbench: swap gun attachments by hand (hub) ---\n"
     "; Let go of the pistol over a table and it lies there. Grab an attachment off\n"
     "; it (light, sight, muzzle) with either hand; the attachments you own lie\n"
     "; beside it; bring one to its place on the gun until it snaps, let go and it\n"
     "; is fitted for real (saved like the game's workbench menu). 1 = on.\n"
     "workbench_hands=1\n"
     "; 1 = until the hub's workbench is unlocked, any table in the hub works.\n"
     "; Once it is unlocked only the workbench does.\n"
     "workbench_tables_before_unlock=1\n"
     "; Grab reach around an attachment, and how close it must come to its place\n"
     "; on the gun to snap there (cm).\n"
     "workbench_grab_cm=3\n"
     "workbench_snap_cm=5\n"
     "; Walk this far from a pistol lying on a table and it goes back to the holster (m).\n"
     "workbench_walk_away_m=3\n"},
    {"workbench_box",
     "\n"
     "; Once the workbench is bought: a green box stands on the front edge of its\n"
     "; table where you walk up to it; let go of the pistol in it and it is held\n"
     "; there upright. 1 = on. Its edges are solid green lines that fade out\n"
     "; toward the middle of each side, which is clear. Size (cm); how far out\n"
     "; past the table edge its middle sits and how high above the table top; the\n"
     "; line thickness and how far in the colour fades; how near your head must\n"
     "; be for it to show.\n"
     "workbench_box=1\n"
     "workbench_box_width_cm=34\n"
     "workbench_box_height_cm=52\n"
     "workbench_box_depth_cm=14\n"
     "workbench_box_out_cm=4\n"
     "workbench_box_lift_cm=2\n"
     "workbench_box_line_cm=0.6\n"
     "workbench_box_fade_cm=4\n"
     "workbench_box_show_cm=100\n"},
    {"touch_interact",
     "\n"
     "; --- Touch interact: rest an empty hand on a thing ---\n"
     "; Loot: touch it to take it. Loot boxes and fridges: touch to open. Doors:\n"
     "; rest a hand on the handle to open or close. The game's own interact runs.\n"
     "; The trigger still interacts too: what the hand touches or points at wins\n"
     "; over where you look. 1 = on.\n"
     "touch_interact=1\n"
     "touch_interact_loot=1\n"
     "touch_interact_containers=1\n"
     "touch_interact_doors=1\n"
     "; How close the hand must come (cm) and how long it rests there first (s).\n"
     "touch_interact_reach_cm=1.5\n"
     "touch_interact_hold_s=0.08\n"
     "touch_interact_door_hold_s=0.2\n"
     "; Door handle: height above the bottom of the door, and how far in from the\n"
     "; door's edge the handle zone reaches (cm).\n"
     "door_handle_height_cm=100\n"
     "door_handle_from_edge_cm=22\n"
     "; Class names (or parts of them, comma separated) that also count as loot\n"
     "; containers, or that touch should leave alone.\n"
     "touch_interact_extra_containers=\n"
     "touch_interact_skip=\n"
     "; 1 = log what each kind of thing is made of and save its game code in\n"
     "; dump/script (once a run), to improve this feature.\n"
     "touch_interact_study=1\n"},
    {"touch_interact_point_cone_deg",
     "\n"
     "; --- Trigger interact aim ---\n"
     "; The trigger with an empty hand uses, in order: what the hand touches or is\n"
     "; within trigger_near_cm of; what it points at exactly; the thing nearest the\n"
     "; aim within point_cone_deg (or point_min_cm) of it, not through a wall; then\n"
     "; where you look.\n"
     "touch_interact_trigger_near_cm=8\n"
     "touch_interact_point_cone_deg=8\n"
     "touch_interact_point_min_cm=6\n"
     "; 1 = log where each item that appears during a level goes (first 80 a level).\n"
     "touch_interact_loot_log=1\n"},
    {"drinks_by_hand",
     "\n"
     "; --- Drink machine by hand (hub) ---\n"
     "; Touch a yellow square with a fingertip to buy that drink; grip the can in\n"
     "; the slot to take it; touch the can's own tab with the other hand's finger\n"
     "; to open it; tip it at your mouth to drink (the game's own buy and drink).\n"
     "; 1 = on.\n"
     "drinks_by_hand=1\n"
     "; 1 = fingertips press the machine's yellow squares.\n"
     "drinks_finger_buttons=1\n"
     "; A fingertip this close in front of a yellow square presses it, and how far\n"
     "; the square goes in while pressed (cm).\n"
     "drinks_press_gap_cm=0.6\n"
     "drinks_button_sink_cm=0.35\n"
     "; Grip this close to the can's middle to take it (cm).\n"
     "drinks_grab_cm=10\n"
     "; The can in the hand: its middle this far ahead of the fist along it, this\n"
     "; far further from the palm than the controller's handle (cm), and turned\n"
     "; this much more around its axis (degrees; by itself the opening faces you).\n"
     "drinks_can_along_fist_cm=1.5\n"
     "drinks_can_from_palm_cm=1.2\n"
     "drinks_can_opening_deg=0\n"
     "; A fingertip this close to the middle of the can's own tab opens it (cm).\n"
     "drinks_tab_reach_cm=2.0\n"
     "; Your mouth, from the middle of your eyes: forward, down, and how close the\n"
     "; can's top must come to it (cm). Tip the can at least this far from upright\n"
     "; (degrees), for this long (seconds), to drink it.\n"
     "drinks_mouth_forward_cm=4\n"
     "drinks_mouth_down_cm=10\n"
     "drinks_mouth_radius_cm=11\n"
     "drinks_pour_tilt_deg=60\n"
     "drinks_pour_seconds=1.2\n"
     "; After a drink the game's own can stays hidden this long (s); a dropped\n"
     "; empty can lies on the floor this long (s).\n"
     "drinks_hide_game_can_s=6\n"
     "drinks_empty_can_s=30\n"},
    {"paired_capture_enabled",
     "\n"
     "; White squares (diagnostic): when the square watch sees dots in one eye, save\n"
     "; the next frame's whole picture before the headset copy plus both copied eye\n"
     "; pictures, and check the copies match it pixel for pixel. Ctrl+F11 asks for\n"
     "; one by hand. At most paired_capture_limit per run (8 max). Each costs a\n"
     "; short hitch. Off by default: 7 captures already showed the copies exact.\n"
     "; See docs/WHITE_SQUARE_CAPTURE.md.\n"
     "paired_capture_enabled=0\n"
     "paired_capture_limit=3\n"
     "paired_capture_on_dots=1\n"},
    {"square_watch",
     "\n"
     "; White squares (diagnostic): compare the two eyes for white squares and dots\n"
     "; drawn into one eye only, and save pictures when they show (square_watch=1).\n"
     "; It reads one strip of the picture on one frame in square_watch_every (1 to\n"
     "; 8; fewer frames cost less). square_test=1 also switches game effects off\n"
     "; one at a time when the dots show, to find which draws them (needs the watch).\n"
     "; Both cost frame time, so they are off for play.\n"
     "square_watch=0\n"
     "square_watch_every=4\n"
     "square_test=0\n"},
    {"process_metrics",
     "\n"
     "; Every 30 s log the game process's memory, handles, CPU and disk use (game\n"
     "; and mod together: a rise alone is not proof the mod leaks).\n"
     "process_metrics=1\n"},
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
