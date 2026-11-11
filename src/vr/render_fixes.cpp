#include "vr/render_fixes.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "core/perf.h"
#include "ue/call.h"
#include "ue/objects.h"
#include "ue/reflection.h"
#include "vr/cvars.h"

namespace holevr::vr::render_fixes {
namespace {
using ue::UObject;
namespace refl = ue::reflection;

// EObjectFlags we must not touch: templates and objects being destroyed.
constexpr uint32_t RF_ClassDefaultObject = 0x10;
constexpr uint32_t RF_ArchetypeObject = 0x20;
constexpr uint32_t RF_BeginDestroyed = 0x8000;
constexpr uint32_t RF_FinishDestroyed = 0x10000;
constexpr uint32_t kSkipFlags =
    RF_ClassDefaultObject | RF_ArchetypeObject | RF_BeginDestroyed | RF_FinishDestroyed;

// One post-process value we force to a neutral setting, with the
// bOverride_ flag that makes the engine use it.
struct Fix {
    const char* setting;   // hole-vr.ini key
    const char* override;  // bOverride_ property in FPostProcessSettings
    const char* value;     // float property in FPostProcessSettings
    float neutral;
    // Resolved at Init.
    int32_t value_offset = -1;
    int32_t flag_offset = -1;
    uint8_t flag_mask = 0;
};

std::vector<Fix> g_fixes = {
    // Smears the image when you turn your head; a common cause of VR sickness.
    {"vr_disable_motion_blur", "bOverride_MotionBlurAmount", "MotionBlurAmount", 0.f},
    {"vr_disable_motion_blur", "bOverride_MotionBlurMax", "MotionBlurMax", 0.f},
    // The game picks the focus, not your eyes, so out-of-focus areas look broken.
    {"vr_disable_depth_of_field", "bOverride_DepthOfFieldFocalDistance",
     "DepthOfFieldFocalDistance", 0.f},
    // Screen-edge darkening reads as wearing goggles; comfort vignette is separate.
    {"vr_disable_vignette", "bOverride_VignetteIntensity", "VignetteIntensity", 0.f},
    // Screen-space flares do not line up between the two eyes.
    {"vr_disable_lens_flares", "bOverride_LensFlareIntensity", "LensFlareIntensity", 0.f},
    // Colour fringing at the edges stacks with the headset lens's own.
    {"vr_disable_chromatic_aberration", "bOverride_SceneFringeIntensity",
     "SceneFringeIntensity", 0.f},
    // Grain is drawn per eye and shimmers in stereo.
    {"vr_disable_film_grain", "bOverride_FilmGrainIntensity", "FilmGrainIntensity", 0.f},
};

// Console settings for the pause blur: nothing nearer than the focus gets
// blurred (hands, arms, body, menu). pause_blur_foreground_radius sets the
// foreground blur limit (fraction of the picture width; engine default
// 0.025). The white squares the owner saw on 2026-11-03 were not the blur:
// they come during a run, never with a menu up (see cvars.cpp, shadow pages).
std::vector<std::pair<std::string, std::string>> PauseBlurCvars() {
    float r = config::GetFloat("pause_blur_foreground_radius", 0.f);
    if (!(r >= 0.f)) r = 0.f;
    char value[32];
    std::snprintf(value, sizeof(value), "%.4f", r);
    return {{"r.DOF.Kernel.MaxForegroundRadius", value}};
}

// Pause blur: while a forearm menu is up, depth of field is switched on and
// focused about arm's length away, so the world behind the menu goes soft
// while the hands, arms and menu stay sharp. Foreground blur is turned off
// (PauseBlurCvars), so nothing nearer than the focus is blurred. It is written into the same cameras and post-process components
// as the fixes above, so the forced "no depth of field" never fights it.
struct PauseSetting {
    const char* override;
    const char* value;
    float target;
    int32_t value_offset = -1;
    int32_t flag_offset = -1;
    uint8_t flag_mask = 0;
};
// The aperture (f-stop) is never touched: in UE5 it also sets the exposure of
// a physical camera, and setting f/1.2 made the whole picture several stops
// brighter while paused (owner's 2026-11-02 run; capture mean 92 -> 208 in
// the same room). The blur amount comes from the sensor width instead, which
// only the depth of field uses. The focal distance comes last, so the blur
// only starts once the sensor width is in.
std::vector<PauseSetting> g_pause = {
    // A wide "sensor" makes the lens model blur more for the same headset view.
    {"bOverride_DepthOfFieldSensorWidth", "DepthOfFieldSensorWidth", 1000.f},
    {"bOverride_DepthOfFieldFocalDistance", "DepthOfFieldFocalDistance", 100.f},  // cm
};
constexpr size_t kPauseSensor = 0, kPauseFocus = 1;

// Read only, for the log: the game's own aperture and exposure mode.
struct Probe {
    const char* override;
    const char* value;
    int32_t value_offset = -1;
    int32_t flag_offset = -1;
    uint8_t flag_mask = 0;
};
Probe g_probe_fstop{"bOverride_DepthOfFieldFstop", "DepthOfFieldFstop"};
Probe g_probe_exposure{"bOverride_AutoExposureMethod", "AutoExposureMethod"};
// Exposure settings, read for the forearm menus' brightness (ReadExposure).
struct Field {
    const char* override;  // bOverride_ flag, or nullptr
    const char* value;
    int32_t value_offset = -1;
    int32_t flag_offset = -1;
    uint8_t flag_mask = 0;
    uint8_t value_mask = 0;  // BoolProperty values (bitfields)
};
Field g_exp_method{"bOverride_AutoExposureMethod", "AutoExposureMethod"};
Field g_exp_bias{"bOverride_AutoExposureBias", "AutoExposureBias"};
Field g_exp_physical{"bOverride_AutoExposureApplyPhysicalCameraExposure", "AutoExposureApplyPhysicalCameraExposure"};
Field g_exp_fstop{"bOverride_DepthOfFieldFstop", "DepthOfFieldFstop"};
Field g_exp_shutter{"bOverride_CameraShutterSpeed", "CameraShutterSpeed"};
Field g_exp_iso{"bOverride_CameraISO", "CameraISO"};
// Read only, for the white squares hunt (2026-11-03): the reflections and
// global illumination each post-process source asks for. A level's own
// settings override r.ReflectionMethod, so the log line "reflection method 2"
// alone doesn't say whether Lumen reflections run in a level.
struct LightField {
    const char* label;
    const char* override;
    const char* value;
    const char* const* names;  // enum value names, or nullptr for a float
    int name_count;
    int32_t value_offset = -1;
    int32_t flag_offset = -1;
    uint8_t flag_mask = 0;
};
const char* const kReflectionNames[] = {"none", "Lumen", "screen space"};
const char* const kGiNames[] = {"none", "Lumen", "screen space", "plugin"};
const char* const kRayLightingNames[] = {"default", "surface cache", "hit lighting"};
LightField g_light[] = {
    {"reflections", "bOverride_ReflectionMethod", "ReflectionMethod", kReflectionNames, 3},
    {"GI", "bOverride_DynamicGlobalIlluminationMethod", "DynamicGlobalIlluminationMethod", kGiNames, 4},
    {"Lumen reflection quality", "bOverride_LumenReflectionQuality", "LumenReflectionQuality", nullptr, 0},
    {"Lumen GI quality", "bOverride_LumenFinalGatherQuality", "LumenFinalGatherQuality", nullptr, 0},
    {"Lumen reflection lighting", "bOverride_LumenRayLightingMode", "LumenRayLightingMode", kRayLightingNames, 3},
    {"SSR intensity", "bOverride_ScreenSpaceReflectionIntensity", "ScreenSpaceReflectionIntensity", nullptr, 0},
    {"SSR quality", "bOverride_ScreenSpaceReflectionQuality", "ScreenSpaceReflectionQuality", nullptr, 0},
};
// VR cap on Lumen GI quality (vr_lumen_gi_quality, default 1; 0 = leave the
// game's). A level's post-process component asks for quality 2; in a headset
// that cost is paid once per eye (the 08:49 run's cost test: about 0.7 ms of
// GPU time a frame). Only sources that set the quality themselves are capped;
// the rest use the engine's default of 1.
float g_gi_quality_cap = 0.f;
constexpr int kGiQualityField = 3;  // g_light index of LumenFinalGatherQuality
// Background thread (Apply) only: what was last logged for each source.
std::unordered_map<UObject, std::string> g_light_logged;
bool g_pause_ready = false;
std::atomic<bool> g_pause_on{false};
int32_t g_dof_focal_offset = -1;  // the "no depth of field" fix's value
// Serialises writes into one object's settings between the background Apply
// and the game thread's SetPauseBlur, and the pause flag with them.
std::mutex g_write_mutex;

// Post-process settings we write, kept so the pause blur can be switched on
// the moment a menu opens without scanning every object again.
struct Target {
    UObject obj;
    int index;
    int32_t offset;
};
std::mutex g_targets_mutex;
std::vector<Target> g_targets;

struct Kind {
    int32_t settings_offset = -1;  // FPostProcessSettings inside the object, or -1
    bool is_capture = false;
};

UObject g_cls_pp_component = 0, g_cls_pp_volume = 0, g_cls_camera = 0, g_cls_capture = 0;
int32_t g_pp_component_settings = -1, g_pp_volume_settings = -1, g_camera_settings = -1;
refl::PropertyInfo g_capture_every_frame, g_capture_on_movement, g_component_visible, g_capture_fov;
bool g_disable_player_capture = true;
bool g_kill_shot_camera_when_shown = false;
bool g_ready = false;
// The scene captures we stop (Apply): the player's, and the kill shot room's
// with vr_kill_shot_camera_when_shown=1. The death screen runs them again
// (AllowPlayerCapture); g_allow_capture says whether it currently does.
std::mutex g_captures_mutex;
std::vector<Target> g_captures;
std::atomic<bool> g_allow_capture{false};
// Scene captures that draw the level into a picture every frame and that the
// mod leaves running (Apply), and the ones the GPU cost test paused
// (PauseSceneCaptures). Both under g_captures_mutex.
std::vector<Target> g_live_captures;
std::vector<Target> g_paused_captures;
std::vector<Target> g_seen_live;
std::string g_live_names;
// Gun scopes' lens cameras (BPC_Scope_*), as of the last Apply; under
// g_captures_mutex. UpdateScopes (game thread) switches them.
std::vector<Target> g_scopes;
bool g_scope_near_eye = true;  // vr_scope_view=near_eye
bool g_scope_zoom_eye = true;  // vr_scope_zoom=eye or flat (not game)
bool g_scope_zoom_flat = false; // vr_scope_zoom=flat
// The last list of scene captures logged (background thread only).
std::string g_census_logged;
std::unordered_map<UObject, Kind> g_kinds;

// Per-run counters so the log says what was touched without repeating itself.
struct Counts {
    int cameras = 0, components = 0, volumes = 0, captures = 0;
    bool operator!=(const Counts& o) const {
        return cameras != o.cameras || components != o.components || volumes != o.volumes ||
               captures != o.captures;
    }
};
Counts g_last;

bool SetBool(uintptr_t addr, uint8_t mask, bool on) {
    uint8_t b;
    if (!mem::Read(addr, b)) return false;
    uint8_t nb = on ? (b | mask) : (b & ~mask);
    return nb == b || mem::Write(addr, nb);
}

void WriteFloat(uintptr_t addr, float v) {
    float cur;
    if (mem::Read(addr, cur) && cur != v) mem::Write(addr, v);
}

// Caller holds g_write_mutex.
void CapGiQuality(uintptr_t settings) {
    const LightField& f = g_light[kGiQualityField];
    if (g_gi_quality_cap <= 0.f || f.value_offset < 0 || f.flag_offset < 0) return;
    if (!(mem::ReadOr<uint8_t>(settings + f.flag_offset) & f.flag_mask)) return;
    float cur;
    if (mem::Read(settings + f.value_offset, cur) && cur > g_gi_quality_cap)
        mem::Write(settings + f.value_offset, g_gi_quality_cap);
}

// Caller holds g_write_mutex.
void ForceNeutral(uintptr_t settings) {
    CapGiQuality(settings);
    const bool pause = g_pause_ready && g_pause_on.load();
    for (const Fix& f : g_fixes) {
        if (f.value_offset < 0) continue;
        float cur;
        if (!mem::Read(settings + f.value_offset, cur)) return;
        if (pause && f.value_offset == g_dof_focal_offset) continue;  // written below
        if (cur != f.neutral) mem::Write(settings + f.value_offset, f.neutral);
        SetBool(settings + f.flag_offset, f.flag_mask, true);
    }
    if (!pause) return;
    for (const PauseSetting& p : g_pause) {
        WriteFloat(settings + p.value_offset, p.target);
        SetBool(settings + p.flag_offset, p.flag_mask, true);
    }
}

// "reflections Lumen, Lumen reflection quality 0.25" for the settings this
// source turns on (its bOverride_ flags), or "" when it sets none of them.
std::string DescribeLighting(uintptr_t settings) {
    std::string out;
    char buf[96];
    for (const LightField& f : g_light) {
        if (f.value_offset < 0 || f.flag_offset < 0) continue;
        if (!(mem::ReadOr<uint8_t>(settings + f.flag_offset) & f.flag_mask)) continue;
        if (f.names) {
            const int v = mem::ReadOr<uint8_t>(settings + f.value_offset, 255);
            if (v < f.name_count) std::snprintf(buf, sizeof(buf), "%s %s", f.label, f.names[v]);
            else std::snprintf(buf, sizeof(buf), "%s %d", f.label, v);
        } else {
            std::snprintf(buf, sizeof(buf), "%s %.2f", f.label, mem::ReadOr<float>(settings + f.value_offset, -1.f));
        }
        if (!out.empty()) out += ", ";
        out += buf;
    }
    return out;
}

// Logs a source's lighting settings when they first show or change.
void NoteLighting(UObject obj, uintptr_t settings) {
    std::string what = DescribeLighting(settings);
    auto it = g_light_logged.find(obj);
    if (it != g_light_logged.end() && it->second == what) return;
    if (g_light_logged.size() > 512) g_light_logged.clear();
    const bool first = it == g_light_logged.end();
    g_light_logged[obj] = what;
    if (what.empty() && first) return;  // sets none of them: nothing to say
    log::Info("render fixes: %s (%s) asks for %s", ue::GetName(obj).c_str(), ue::GetName(ue::GetClass(obj)).c_str(),
              what.empty() ? "no reflection or GI settings of its own any more" : what.c_str());
}

Kind Classify(UObject cls) {
    Kind k;
    if (g_cls_camera && refl::IsChildOf(cls, g_cls_camera)) k.settings_offset = g_camera_settings;
    else if (g_cls_pp_component && refl::IsChildOf(cls, g_cls_pp_component))
        k.settings_offset = g_pp_component_settings;
    else if (g_cls_pp_volume && refl::IsChildOf(cls, g_cls_pp_volume))
        k.settings_offset = g_pp_volume_settings;
    else if (g_cls_capture && refl::IsChildOf(cls, g_cls_capture)) k.is_capture = true;
    return k;
}

// The player's own scene capture renders the level a second time every frame
// (the game exposes it as the "Capture2D" option). In a headset that cost is
// paid on top of two eye views, so it is stopped unless the player opts in.
bool IsPlayerCapture(UObject comp) {
    UObject outer = ue::GetOuter(comp);
    if (!outer) return false;
    std::string cls = ue::GetName(ue::GetClass(outer));
    return cls.rfind("BP_Player", 0) == 0;
}

// The kill shot room (LCBP_DeadReviewRoom) photographs your body for the
// death and results screens with its own scene capture, 1280x544, set to take
// a picture every frame (owner's 2026-11-07 run). Only those screens show the
// picture, and while one is up the capture runs again (AllowPlayerCapture)
// and the forearm menu asks it for pictures. vr_kill_shot_camera_when_shown=1
// stops it the rest of the time.
bool IsKillShotCapture(UObject comp) {
    UObject outer = ue::GetOuter(comp);
    if (!outer) return false;
    return ue::GetName(ue::GetClass(outer)).rfind("LCBP_DeadReviewRoom", 0) == 0;
}

// A gun scope's lens camera (BPC_Scope_2x_AR_C and the like): it draws the
// level a second time, with all its shadows and lighting, into the lens
// picture (RT_Scope 512x512) whenever it moves. In a headset a gun moves
// every frame, held or on your back, so that is every frame. The scope's
// Blueprint (BP_ScopeBase) never asks for pictures itself: these flags are
// all that drive it. Owner's run 2026-11-08 22:56 local, the first with a 2x
// scope on the rifle: the GPU frame in the home base was 14.3-15.5 ms against
// a 13.9 ms budget, 10-12 ms in the 09:20 run without it (the likely cause;
// the "scope view" report line and the GPU frame show whether it is).
bool IsScopeCapture(UObject comp) {
    UObject outer = ue::GetOuter(comp);
    if (!outer) return false;
    return ue::GetName(ue::GetClass(outer)).rfind("BPC_Scope", 0) == 0;
}

bool Bit(UObject obj, const refl::PropertyInfo& p) {
    return p.valid() && p.bool_mask && (mem::ReadOr<uint8_t>(obj + p.offset, 0) & p.bool_mask) != 0;
}

bool Alive(const Target& t) {
    if (t.index < 0 || ue::objects::Get(t.index) != t.obj) return false;
    return !(mem::ReadOr<uint32_t>(t.obj + ue::off::kObjFlags, kSkipFlags) & kSkipFlags);
}

// One scene capture for the log: "LCBP_DeadReviewRoom_C_1.SceneCaptureComponent2D
// shown, every frame, into DeadReviewCamera 1280x544, source 2".
// moving_as: -1 reads the capture's own flag; 0/1 says what the game set
// (a scope's, which UpdateScopes switches, so the census doesn't change).
std::string DescribeCapture(UObject obj, bool every, bool stopped, int moving_as = -1) {
    UObject outer = ue::GetOuter(obj), target = 0;
    ue::GetProp(obj, "TextureTarget", target);
    int32_t sx = 0, sy = 0;
    uint8_t source = 255;
    ue::GetProp(obj, "CaptureSource", source);
    if (target) {
        ue::GetProp(target, "SizeX", sx);
        ue::GetProp(target, "SizeY", sy);
    }
    const bool moving = moving_as >= 0 ? moving_as != 0 : Bit(obj, g_capture_on_movement);
    char b[320];
    std::snprintf(b, sizeof(b), "%s.%s (%s) %s, %s, into %s %dx%d, source %d%s",
                  outer ? ue::GetName(outer).c_str() : "?", ue::GetName(obj).c_str(),
                  ue::GetName(ue::GetClass(obj)).c_str(),
                  !g_component_visible.valid() ? "shown?" : Bit(obj, g_component_visible) ? "shown" : "hidden",
                  every ? "every frame" : moving ? "when it moves" : "only when asked",
                  target ? ue::GetName(target).c_str() : "nothing", sx, sy, source,
                  stopped ? ", stopped in VR" : "");
    return b;
}

}  // namespace

bool Init() {
    if (!refl::Ready()) return false;
    g_cls_pp_component = ue::FindObject("Class /Script/Engine.PostProcessComponent");
    g_cls_pp_volume = ue::FindObject("Class /Script/Engine.PostProcessVolume");
    g_cls_camera = ue::FindObject("Class /Script/Engine.CameraComponent");
    // Every kind of scene capture (2D, cube, planar reflection) is a
    // SceneCaptureComponent, which holds the capture flags.
    g_cls_capture = ue::FindObject("Class /Script/Engine.SceneCaptureComponent");
    UObject pp_struct = ue::FindObject("ScriptStruct /Script/Engine.PostProcessSettings");
    if (!pp_struct || !g_cls_camera) {
        log::Warn("render fixes: engine post-process types not found, fixes disabled");
        return false;
    }
    if (g_cls_pp_component)
        g_pp_component_settings = refl::FindPropertyOffset(g_cls_pp_component, "Settings");
    if (g_cls_pp_volume) g_pp_volume_settings = refl::FindPropertyOffset(g_cls_pp_volume, "Settings");
    g_camera_settings = refl::FindPropertyOffset(g_cls_camera, "PostProcessSettings");
    if (g_cls_capture) {
        g_capture_every_frame = refl::FindProperty(g_cls_capture, "bCaptureEveryFrame");
        g_capture_on_movement = refl::FindProperty(g_cls_capture, "bCaptureOnMovement");
        if (!g_capture_every_frame.bool_mask) g_capture_every_frame = {};
        if (!g_capture_on_movement.bool_mask) g_capture_on_movement = {};
        g_capture_fov = refl::FindProperty(g_cls_capture, "FOVAngle");
        if (g_capture_fov.type != "FloatProperty" || g_capture_fov.element_size != 4) g_capture_fov = {};
    }
    if (UObject scene_component = ue::FindObject("Class /Script/Engine.SceneComponent")) {
        g_component_visible = refl::FindProperty(scene_component, "bVisible");
        if (!g_component_visible.bool_mask) g_component_visible = {};
    }
    g_disable_player_capture = config::GetBool("vr_disable_player_scene_capture", true);
    g_kill_shot_camera_when_shown = config::GetBool("vr_kill_shot_camera_when_shown", false);
    {
        std::string v = config::GetString("vr_scope_view", "near_eye");
        for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        g_scope_near_eye = v != "always" && v != "game";
        std::string z = config::GetString("vr_scope_zoom", "eye");
        for (char& ch : z) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        g_scope_zoom_eye = z != "game";
        g_scope_zoom_flat = z == "flat";
    }
    {
        int found = 0;
        for (Field* f : {&g_exp_method, &g_exp_bias, &g_exp_physical, &g_exp_fstop, &g_exp_shutter, &g_exp_iso}) {
            refl::PropertyInfo flag = refl::FindProperty(pp_struct, f->override);
            refl::PropertyInfo val = refl::FindProperty(pp_struct, f->value);
            if (!val.valid()) continue;
            f->value_offset = val.offset;
            f->value_mask = val.bool_mask;
            if (flag.valid() && flag.bool_mask) {
                f->flag_offset = flag.offset;
                f->flag_mask = flag.bool_mask;
            }
            ++found;
        }
        log::Info("render fixes: %d of 6 exposure settings readable (method %s, bias %s, physical camera %s)", found,
                  g_exp_method.value_offset >= 0 ? "yes" : "no", g_exp_bias.value_offset >= 0 ? "yes" : "no",
                  g_exp_physical.value_offset >= 0 ? "yes" : "no");
    }

    {
        int found = 0;
        for (LightField& f : g_light) {
            refl::PropertyInfo flag = refl::FindProperty(pp_struct, f.override);
            refl::PropertyInfo val = refl::FindProperty(pp_struct, f.value);
            if (!flag.valid() || !flag.bool_mask || !val.valid()) continue;
            const bool is_byte = val.type == "ByteProperty" || val.type == "EnumProperty";
            if (f.names ? !is_byte || val.element_size != 1 : val.type != "FloatProperty") continue;
            f.flag_offset = flag.offset;
            f.flag_mask = flag.bool_mask;
            f.value_offset = val.offset;
            ++found;
        }
        log::Info("render fixes: %d of %zu reflection and GI settings readable", found, std::size(g_light));
        g_gi_quality_cap = config::GetFloat("vr_lumen_gi_quality", 1.f);
        if (g_gi_quality_cap > 0.f)
            log::Info("render fixes: Lumen GI quality capped at %.2f in VR (vr_lumen_gi_quality; 0 = the game's own, which is 2 in levels)%s",
                      g_gi_quality_cap,
                      g_light[kGiQualityField].value_offset >= 0 ? "" : ", but the setting wasn't found");
    }

    int enabled = 0;
    for (Fix& f : g_fixes) {
        if (!config::GetBool(f.setting, true)) continue;
        refl::PropertyInfo flag = refl::FindProperty(pp_struct, f.override);
        refl::PropertyInfo val = refl::FindProperty(pp_struct, f.value);
        if (!flag.valid() || flag.type != "BoolProperty" || !flag.bool_mask || !val.valid() ||
            val.type != "FloatProperty") {
            log::Warn("render fixes: %s/%s not found in this engine build, skipping", f.override,
                      f.value);
            continue;
        }
        f.flag_offset = flag.offset;
        f.flag_mask = flag.bool_mask;
        f.value_offset = val.offset;
        ++enabled;
    }
    for (const Fix& f : g_fixes)
        if (f.value_offset >= 0 && std::string(f.value) == "DepthOfFieldFocalDistance") g_dof_focal_offset = f.value_offset;
    if (!config::GetBool("pause_blur", true)) {
        log::Info("render fixes: pause blur off (pause_blur=0)");
    } else if (g_dof_focal_offset < 0) {
        log::Info("render fixes: pause blur off (needs vr_disable_depth_of_field=1)");
    } else {
        g_pause[kPauseSensor].target = config::GetFloat("pause_blur_sensor_mm", 1000.f);
        g_pause[kPauseFocus].target = config::GetFloat("pause_blur_focus_m", 1.0f) * 100.f;
        for (Probe* pr : {&g_probe_fstop, &g_probe_exposure}) {
            refl::PropertyInfo flag = refl::FindProperty(pp_struct, pr->override);
            refl::PropertyInfo val = refl::FindProperty(pp_struct, pr->value);
            if (!flag.valid() || !flag.bool_mask || !val.valid()) continue;
            pr->flag_offset = flag.offset;
            pr->flag_mask = flag.bool_mask;
            pr->value_offset = val.offset;
        }
        bool ok = true;
        for (PauseSetting& p : g_pause) {
            refl::PropertyInfo flag = refl::FindProperty(pp_struct, p.override);
            refl::PropertyInfo val = refl::FindProperty(pp_struct, p.value);
            if (!flag.valid() || flag.type != "BoolProperty" || !flag.bool_mask || !val.valid() ||
                val.type != "FloatProperty") {
                log::Warn("render fixes: %s/%s not found, pause blur off", p.override, p.value);
                ok = false;
                break;
            }
            p.flag_offset = flag.offset;
            p.flag_mask = flag.bool_mask;
            p.value_offset = val.offset;
        }
        if (ok) {
            for (const auto& [name, value] : PauseBlurCvars()) vr::cvars::Want(name, value);
            g_pause_ready = true;
            log::Info("render fixes: pause blur ready (focus %.2f m, sensor %.0f mm; the game's aperture and "
                      "exposure are left alone)",
                      g_pause[kPauseFocus].target / 100.f, g_pause[kPauseSensor].target);
        }
    }
    log::Info("render fixes: %d post-process overrides active; camera settings at 0x%x, "
              "component 0x%x, volume 0x%x; player scene capture %s; kill shot room camera %s "
              "(vr_kill_shot_camera_when_shown); capture flags %s",
              enabled, g_camera_settings, g_pp_component_settings, g_pp_volume_settings,
              g_disable_player_capture ? "off" : "left on",
              g_kill_shot_camera_when_shown ? "only while its picture shows" : "left as the game set it",
              g_capture_every_frame.valid() && g_component_visible.valid() ? "readable" : "NOT readable");
    g_ready = true;
    return true;
}

void Apply() {
    if (!g_ready) return;
    // A pass over every object each second: remember memory regions for the
    // pass, or most reads ask Windows again (2026-11-03 performance pass).
    mem::ScopedRegionCache region_cache;
    Counts c;
    std::vector<Target> targets, captures, live, paused_now, scopes;
    std::vector<std::string> census;
    std::string live_names;
    {
        std::lock_guard<std::mutex> lock(g_captures_mutex);
        paused_now = g_paused_captures;
    }
    ue::objects::ForEach([&](int, UObject obj) {
        uint32_t flags = mem::ReadOr<uint32_t>(obj + ue::off::kObjFlags, kSkipFlags);
        if (flags & kSkipFlags) return;
        UObject cls = ue::GetClass(obj);
        if (!cls) return;
        auto it = g_kinds.find(cls);
        if (it == g_kinds.end()) it = g_kinds.emplace(cls, Classify(cls)).first;
        const Kind& k = it->second;
        if (k.settings_offset >= 0) {
            // Forcing the neutral value everywhere (camera, unbound and local
            // volumes) means it wins however the engine blends them.
            {
                std::lock_guard<std::mutex> lock(g_write_mutex);
                ForceNeutral(obj + k.settings_offset);
            }
            NoteLighting(obj, obj + k.settings_offset);
            targets.push_back({obj, mem::ReadOr<int32_t>(obj + ue::off::kObjIndex, -1), k.settings_offset});
            if (k.settings_offset == g_camera_settings) ++c.cameras;
            else if (k.settings_offset == g_pp_component_settings) ++c.components;
            else ++c.volumes;
        } else if (k.is_capture) {
            const int index = mem::ReadOr<int32_t>(obj + ue::off::kObjIndex, -1);
            const bool stop = (g_disable_player_capture && IsPlayerCapture(obj)) ||
                              (g_kill_shot_camera_when_shown && IsKillShotCapture(obj));
            if (!stop) {
                // Left running: counted for the log and the GPU cost test.
                const bool paused = std::any_of(paused_now.begin(), paused_now.end(),
                                                [&](const Target& t) { return t.obj == obj; });
                const bool every = paused || Bit(obj, g_capture_every_frame);
                const bool shown = !g_component_visible.valid() || Bit(obj, g_component_visible);
                if (IsScopeCapture(obj)) {
                    scopes.push_back({obj, index, 0});
                    if (g_scope_near_eye) {
                        census.push_back(DescribeCapture(obj, every, false, 1) +
                                         ", its view drawn only at the eye (vr_scope_view)");
                        return;
                    }
                }
                census.push_back(DescribeCapture(obj, every, false));
                if (every && shown && !paused) {
                    live.push_back({obj, index, 0});
                    if (!live_names.empty()) live_names += ", ";
                    live_names += ue::GetName(ue::GetOuter(obj));
                }
                return;
            }
            captures.push_back({obj, index, 0});
            census.push_back(DescribeCapture(obj, g_allow_capture.load(), true));
            if (g_allow_capture.load()) return;  // the death screen shows its picture
            bool ok = true;
            if (g_capture_every_frame.bool_mask)
                ok &= SetBool(obj + g_capture_every_frame.offset, g_capture_every_frame.bool_mask, false);
            if (g_capture_on_movement.bool_mask)
                ok &= SetBool(obj + g_capture_on_movement.offset, g_capture_on_movement.bool_mask, false);
            if (ok) ++c.captures;
        }
    });
    {
        std::lock_guard<std::mutex> lock(g_targets_mutex);
        g_targets.swap(targets);
    }
    {
        std::lock_guard<std::mutex> lock(g_captures_mutex);
        g_captures.swap(captures);
        g_live_captures.swap(live);
        g_live_names = live_names;
        g_scopes.swap(scopes);
    }
    // Every scene capture in the world: each one that takes a picture every
    // frame draws the level again, with its own shadows, lighting and post
    // effects, on top of the two eyes (2026-11-07: the frame's fixed GPU
    // cost had about 4 ms nobody could place).
    {
        std::sort(census.begin(), census.end());
        int every = 0;
        std::string line;
        for (const std::string& e : census) {
            line += " | " + e;
            if (e.find(", every frame,") != std::string::npos && e.find(" shown,") != std::string::npos &&
                e.find("stopped in VR") == std::string::npos)
                ++every;
        }
        if (line != g_census_logged) {
            g_census_logged = line;
            log::Info("render fixes: scene captures in the world: %zu, %d drawing the level every frame%s",
                      census.size(), every, line.c_str());
        }
    }
    if (c != g_last) {
        log::Info("render fixes applied: %d cameras, %d post-process components, %d volumes, "
                  "%d player scene captures stopped",
                  c.cameras, c.components, c.volumes, c.captures);
        g_last = c;
    }
}

// "f/4.0*" = aperture 4.0, set by this object (* = override on); "exp 2*" = exposure method.
std::string Describe(uintptr_t settings) {
    std::string out;
    char buf[64];
    if (g_probe_fstop.value_offset >= 0) {
        float f = mem::ReadOr<float>(settings + g_probe_fstop.value_offset, -1.f);
        bool set = mem::ReadOr<uint8_t>(settings + g_probe_fstop.flag_offset) & g_probe_fstop.flag_mask;
        std::snprintf(buf, sizeof(buf), "f/%.1f%s", f, set ? "*" : "");
        out += buf;
    }
    if (g_probe_exposure.value_offset >= 0) {
        int m = mem::ReadOr<uint8_t>(settings + g_probe_exposure.value_offset, 255);
        bool set = mem::ReadOr<uint8_t>(settings + g_probe_exposure.flag_offset) & g_probe_exposure.flag_mask;
        std::snprintf(buf, sizeof(buf), " exp %d%s", m, set ? "*" : "");
        out += buf;
    }
    return out;
}

bool PauseBlurOn() { return g_pause_on.load(); }

int SetPauseBlur(bool on) {
    if (!g_pause_ready) return -1;
    {
        std::lock_guard<std::mutex> lock(g_write_mutex);
        g_pause_on = on;
    }
    std::vector<Target> targets;
    {
        std::lock_guard<std::mutex> lock(g_targets_mutex);
        targets = g_targets;
    }
    int n = 0;
    std::string seen;
    for (const Target& t : targets) {
        if (t.index < 0 || ue::objects::Get(t.index) != t.obj) continue;
        uint32_t flags = mem::ReadOr<uint32_t>(t.obj + ue::off::kObjFlags, kSkipFlags);
        if (flags & kSkipFlags) continue;
        {
            std::lock_guard<std::mutex> lock(g_write_mutex);
            ForceNeutral(t.obj + t.offset);
        }
        if (on && n < 8) seen += " | " + ue::GetName(ue::GetClass(t.obj)) + " " + Describe(t.obj + t.offset);
        ++n;
    }
    if (on) log::Info("render fixes: pause blur on %d settings (aperture/exposure untouched:%s)", n, seen.c_str());
    return n;
}

void ApplyPauseBlurCvars(uintptr_t context) {
    for (const auto& [name, value] : PauseBlurCvars()) vr::cvars::RunNow(name + " " + value, context);
}

// The death screen's picture of whoever killed you (UIM_Results_C's
// IMG_Killer) may come from the player's own scene capture, which Apply
// stops in VR because it draws the whole level a second time every frame.
// While the death screen is up nothing is played, so the capture runs again
// (and takes a picture at once); when it closes, Apply stops it again.
int AllowPlayerCapture(bool allow) {
    if (g_allow_capture.exchange(allow) == allow) return 0;
    std::vector<Target> captures;
    {
        std::lock_guard<std::mutex> lock(g_captures_mutex);
        captures = g_captures;
    }
    int n = 0;
    for (const Target& t : captures) {
        if (t.index < 0 || ue::objects::Get(t.index) != t.obj) continue;
        uint32_t flags = mem::ReadOr<uint32_t>(t.obj + ue::off::kObjFlags, kSkipFlags);
        if (flags & kSkipFlags) continue;
        if (g_capture_every_frame.bool_mask)
            SetBool(t.obj + g_capture_every_frame.offset, g_capture_every_frame.bool_mask, allow);
        UObject target = 0;
        ue::GetProp(t.obj, "TextureTarget", target);
        if (allow) {
            ue::Call c(t.obj, "CaptureScene");
            c.Invoke();
        }
        log::Info("render fixes: scene capture %s.%s %s (picture into %s)",
                  ue::GetName(ue::GetOuter(t.obj)).c_str(), ue::GetName(t.obj).c_str(),
                  allow ? "running again for the death screen" : "stopped again",
                  target ? ue::GetName(target).c_str() : "nothing");
        ++n;
    }
    return n;
}

int PauseSceneCaptures(bool pause, std::string* names) {
    if (!g_capture_every_frame.valid()) return 0;
    std::lock_guard<std::mutex> lock(g_captures_mutex);
    auto has = [](const std::vector<Target>& v, const Target& t) {
        return std::any_of(v.begin(), v.end(), [&](const Target& o) { return o.obj == t.obj; });
    };
    int n = 0;
    if (!pause) {
        for (const Target& t : g_paused_captures)
            if (Alive(t) && SetBool(t.obj + g_capture_every_frame.offset, g_capture_every_frame.bool_mask, true)) ++n;
        g_paused_captures.clear();
        return n;
    }
    // Apply may have run while they were paused and not listed them; the
    // ones paused before are remembered so every test window gets them all.
    g_seen_live.erase(std::remove_if(g_seen_live.begin(), g_seen_live.end(), [](const Target& t) { return !Alive(t); }),
                      g_seen_live.end());
    for (const Target& t : g_live_captures)
        if (!has(g_seen_live, t)) g_seen_live.push_back(t);
    std::string list;
    for (const Target& t : g_seen_live) {
        if (has(g_paused_captures, t) || !Bit(t.obj, g_capture_every_frame)) continue;
        if (!SetBool(t.obj + g_capture_every_frame.offset, g_capture_every_frame.bool_mask, false)) continue;
        g_paused_captures.push_back(t);
        if (!list.empty()) list += ", ";
        list += ue::GetName(ue::GetOuter(t.obj));
        ++n;
    }
    if (names) *names = list;
    return n;
}

int LiveSceneCaptures(std::string* names) {
    std::lock_guard<std::mutex> lock(g_captures_mutex);
    if (names) *names = g_live_names;
    return static_cast<int>(g_live_captures.size() + g_paused_captures.size());
}

namespace {
bool FlagOn(uintptr_t settings, const Field& f) {
    return f.flag_offset >= 0 && (mem::ReadOr<uint8_t>(settings + f.flag_offset) & f.flag_mask);
}
}  // namespace

// The engine's manual exposure (PostProcessEyeAdaptation.cpp) draws the world
// 2^AutoExposureBias times as bright, divided by the physical camera's
// exposure (EV100 from aperture, shutter and ISO) when that is applied.
Exposure ReadExposure(UObject obj) {
    Exposure e;
    if (!g_ready || !obj || g_exp_bias.value_offset < 0) return e;
    UObject cls = ue::GetClass(obj);
    if (!cls) return e;
    Kind k = Classify(cls);
    if (k.settings_offset < 0) return e;
    uintptr_t s = obj + k.settings_offset;
    float bias;
    if (!mem::Read(s + g_exp_bias.value_offset, bias) || !(bias > -20.f && bias < 20.f)) return e;
    e.bias = bias;
    e.bias_set = FlagOn(s, g_exp_bias);
    if (g_exp_method.value_offset >= 0) {
        e.method = mem::ReadOr<uint8_t>(s + g_exp_method.value_offset, 255);
        e.method_set = FlagOn(s, g_exp_method);
    }
    if (g_exp_physical.value_offset >= 0 && g_exp_physical.value_mask)
        e.physical = mem::ReadOr<uint8_t>(s + g_exp_physical.value_offset) & g_exp_physical.value_mask;
    auto read = [&](const Field& f, float fallback, float lo, float hi) {
        float v = f.value_offset >= 0 ? mem::ReadOr<float>(s + f.value_offset, fallback) : fallback;
        return v > lo && v < hi ? v : fallback;
    };
    float fstop = read(g_exp_fstop, 4.f, 0.5f, 64.f);
    float shutter = read(g_exp_shutter, 60.f, 0.5f, 10000.f);
    float iso = read(g_exp_iso, 100.f, 1.f, 100000.f);
    e.camera_ev100 = static_cast<float>(std::log2(static_cast<double>(fstop) * fstop * shutter * 100.0 / iso));
    e.stops = e.bias - (e.physical ? e.camera_ev100 : 0.f);
    e.ok = true;
    return e;
}

namespace {
// Game thread state for UpdateScopes.
struct ScopeState {
    UObject obj = 0;
    int index = -1;
    bool off = false;          // we switched its capture off
    double near_at = -1e9;     // last time it was at the eye
    // Zoom (vr_scope_zoom=eye), from MeasureLens.
    int lens_tries = 0;
    bool lens_ok = false;
    float game_fov = 0;        // FOVAngle as the game set it (90 x k / M)
    double mag = 0;            // LensMagnificationFactor (M), 0 when out of range
    double radius = 0;         // eyepiece lens radius, cm
    double ads_dist = 0;       // the game's aiming camera (ADSPoint) to the lens centre, cm
    FVector lens_c;            // lens centre in the lens camera's own space
    float set_fov = -1;        // the FOVAngle we last wrote
};
std::vector<ScopeState> g_scope_state;
std::atomic<int> g_scope_frames{0}, g_scope_drawn_frames{0}, g_scope_switches{0};
int g_scope_logged = 0;
int g_lens_logged = 0;
// What the zoom did since the last report (game thread writes, report reads).
std::mutex g_zoom_mutex;
struct ZoomStats {
    int frames = 0;
    double fov_lo = 1e9, fov_hi = 0, d_lo = 1e9, d_hi = 0;
    float game_fov = 0;
    double ads_dist = 0;
} g_zoom;

struct alignas(16) Xform {
    FQuat rotation;
    FVector translation;
    double pad0 = 0;
    FVector scale{1, 1, 1};
    double pad1 = 0;
};
static_assert(sizeof(Xform) == 0x60, "FTransform layout");

double Length(const FVector& v) { return std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z); }

std::string ScopeReport(double) {
    const int frames = g_scope_frames.exchange(0), drawn = g_scope_drawn_frames.exchange(0);
    const int switches = g_scope_switches.exchange(0);
    if (frames <= 0) return {};
    char b[400];
    int n = std::snprintf(b, sizeof(b), "scope view: drawn in %d of %d frames, switched %d times (%s)", drawn, frames,
                          switches,
                          g_scope_near_eye ? "only at the eye, vr_scope_view=near_eye" : "always, as the game does");
    ZoomStats z;
    {
        std::lock_guard<std::mutex> lock(g_zoom_mutex);
        z = g_zoom;
        g_zoom = ZoomStats{};
    }
    if (z.frames > 0 && n > 0 && n < (int)sizeof(b))
        std::snprintf(b + n, sizeof(b) - n,
                      "; lens view %.1f-%.1f deg for your eye %.1f-%.1f cm from the lens in %d frames (the game's: "
                      "%.1f deg, for its camera %.1f cm from it; vr_scope_zoom=%s)",
                      z.fov_lo, z.fov_hi, z.d_lo, z.d_hi, z.frames, z.game_fov, z.ads_dist,
                      g_scope_zoom_flat ? "flat" : "eye");
    return b;
}

Xform ComponentXform(UObject comp, bool* ok) {
    ue::Call c(comp, "K2_GetComponentToWorld");
    *ok = c.Invoke();
    return *ok ? c.Result<Xform>() : Xform{};
}

// A scope's eyepiece lens, measured once (BP_ScopeBase: EyepieceLens shows
// the lens camera's picture; LensMagnificationFactor M; LensMagnification-
// Factor_ByEyeRelief k; the sight's ADSPoint, where the game puts its
// flat-screen camera when aiming). The game sets the lens camera to
// 90 x k / M degrees once (SetMagnificationFactor, at BeginPlay): sized for
// its own camera at the ADSPoint, close behind the lens. Your eye sits
// further back (the rifle's sight point plus eye relief), the lens takes up
// less of your view, and the same picture in it looks zoomed out (owner
// 2026-11-11: "the 2x scope isn't zooming in, but is instead zooming out").
bool MeasureLens(UObject capture, const Xform& cap, ScopeState& st) {
    UObject scope = ue::GetOuter(capture);
    const std::string name = scope ? ue::GetName(scope) : std::string("?");
    auto fail = [&](const char* why) {
        if (g_lens_logged < 6) {
            ++g_lens_logged;
            log::Info("render fixes: %s's lens not measured (%s): its zoom stays as the game set it", name.c_str(),
                      why);
        }
        return false;
    };
    double m = 0, k = 0;
    if (!scope || !ue::GetProp(scope, "LensMagnificationFactor", m) ||
        !ue::GetProp(scope, "LensMagnificationFactor_ByEyeRelief", k))
        return fail("no magnification on the scope");
    const float fov = mem::ReadOr<float>(capture + g_capture_fov.offset, 0.f);
    if (!(fov > 0.5f && fov < 120.f)) return fail("its lens camera's view angle is out of range");
    UObject lens = 0, ads = 0, sm = 0;
    ue::GetProp(scope, "EyepieceLens", lens);
    ue::GetProp(scope, "ADSPoint", ads);
    if (!lens || !ue::IsLive(lens) || !ads || !ue::IsLive(ads)) return fail("no eyepiece lens or aiming point");
    if (!ue::GetProp(lens, "StaticMesh", sm) || !sm) return fail("the eyepiece lens has no mesh");
    ue::Call bb(sm, "GetBoundingBox");
    int32_t bsz = 0;
    const uint8_t* raw = bb.Invoke() ? bb.Raw("ReturnValue", &bsz) : nullptr;
    if (!raw || bsz < 48) return fail("the lens mesh has no box");
    double v[6];
    std::memcpy(v, raw, sizeof(v));
    for (double x : v)
        if (!std::isfinite(x) || std::fabs(x) > 1e4) return fail("the lens mesh box is not finite");
    bool lok = false, aok = false;
    const Xform lx = ComponentXform(lens, &lok), ax = ComponentXform(ads, &aok);
    if (!lok || !aok) return fail("the lens or aiming point has no place");
    // The lens is a disc: its thinnest side is along the sight line, the
    // other two are its width.
    const double sc[3] = {std::fabs(lx.scale.X), std::fabs(lx.scale.Y), std::fabs(lx.scale.Z)};
    double e[3];
    for (int i = 0; i < 3; ++i) e[i] = (v[3 + i] - v[i]) * sc[i];
    int thin = 0;
    for (int i = 1; i < 3; ++i)
        if (e[i] < e[thin]) thin = i;
    const double w1 = e[(thin + 1) % 3], w2 = e[(thin + 2) % 3];
    if (!(std::fmin(w1, w2) > 0.5 && std::fmax(w1, w2) < 15.0 && e[thin] < 0.6 * std::fmin(w1, w2)))
        return fail("the lens mesh is not a disc 0.5-15 cm across");
    const FVector mid{(v[0] + v[3]) * 0.5 * lx.scale.X, (v[1] + v[4]) * 0.5 * lx.scale.Y,
                      (v[2] + v[5]) * 0.5 * lx.scale.Z};
    const FVector centre = lx.translation + lx.rotation.Rotate(mid);
    const double d_ads = Length(ax.translation - centre);
    if (!(d_ads > 1.5 && d_ads < 40.0)) return fail("the game's aiming point is not 1.5-40 cm from the lens");
    st.radius = 0.25 * (w1 + w2);
    st.ads_dist = d_ads;
    st.game_fov = fov;
    st.mag = m >= 1.0 && m <= 20.0 ? m : 0.0;
    st.lens_c = cap.rotation.Inverse().Rotate(centre - cap.translation);
    if (g_lens_logged < 6) {
        ++g_lens_logged;
        const double lens_deg = 2.0 * std::atan(st.radius / d_ads) * 180.0 / 3.14159265358979;
        log::Info("render fixes: %s lens: magnification %.2f (by eye relief %.3f), the game's lens view %.1f deg "
                  "(90 x %.3f / %.2f = %.1f); eyepiece lens %.1f cm across, the game's aiming camera %.1f cm behind "
                  "it, where the lens is %.1f deg wide (%.2fx on a flat screen); in the headset the view follows "
                  "how far your eye is from the lens, %s",
                  name.c_str(), m, k, fov, k, m, 90.0 * k / (m > 0 ? m : 1), 2.0 * st.radius, d_ads, lens_deg,
                  lens_deg / fov,
                  g_scope_zoom_flat || st.mag <= 0 ? "magnifying as the flat screen does (vr_scope_zoom=flat)"
                                                   : "magnifying as marked (vr_scope_zoom=eye)");
    }
    return true;
}

// The lens camera's view angle for an eye d cm from the lens. eye: the lens
// takes up 2 x atan(r / d) of your view and shows M times less than that, so
// things in it look M times as big as beside it (a 2x scope, twice). flat:
// the lens shows the same share of its picture per degree of your view as
// it does for the game's flat-screen camera (the game's own tuning, k).
float EyeFov(const ScopeState& st, double d) {
    constexpr double kDeg = 180.0 / 3.14159265358979;
    const double eye_lens = std::atan(st.radius / std::fmax(d, 0.5));
    double fov;
    if (!g_scope_zoom_flat && st.mag > 0)
        fov = 2.0 * eye_lens * kDeg / st.mag;
    else
        fov = st.game_fov * eye_lens / std::atan(st.radius / st.ads_dist);
    fov = std::fmax(0.5, std::fmin(fov, 120.0));
    return static_cast<float>(fov);
}
}  // namespace

void UpdateScopes(bool have_eye, const FVector& eye, const FVector& look, double now_s) {
    static const bool registered = [] {
        perf::AddReportLine(&ScopeReport);
        return true;
    }();
    (void)registered;
    if (!g_ready || !g_capture_on_movement.bool_mask) return;
    std::vector<Target> scopes;
    {
        std::lock_guard<std::mutex> lock(g_captures_mutex);
        scopes = g_scopes;
    }
    // Forget scopes that are gone (a new level, a scope taken off at the bench).
    g_scope_state.erase(std::remove_if(g_scope_state.begin(), g_scope_state.end(),
                                       [](const ScopeState& st) { return !Alive({st.obj, st.index, 0}); }),
                        g_scope_state.end());
    if (scopes.empty()) return;
    const double look_len = Length(look);
    bool any_drawn = false;
    for (const Target& t : scopes) {
        if (!Alive(t)) continue;
        auto it = std::find_if(g_scope_state.begin(), g_scope_state.end(),
                               [&](const ScopeState& st) { return st.obj == t.obj; });
        if (it == g_scope_state.end()) {
            g_scope_state.push_back({t.obj, t.index, false, -1e9});
            it = g_scope_state.end() - 1;
        }
        ScopeState& st = *it;
        bool have_x = false;
        Xform x;
        if (have_eye) x = ComponentXform(t.obj, &have_x);
        bool want = true;
        if (g_scope_near_eye) {
            // At the eye: the lens camera within 40 cm of the aiming eye and
            // pointing within 35 degrees of where the head looks. Kept on for
            // 0.3 s after, so a wobble at the edge doesn't switch it.
            bool at_eye = false;
            if (have_x && look_len > 1e-6) {
                const FVector fwd = x.rotation.Rotate(FVector{1, 0, 0});
                const double cosine =
                    (fwd.X * look.X + fwd.Y * look.Y + fwd.Z * look.Z) / (Length(fwd) * look_len + 1e-9);
                at_eye = Length(x.translation - eye) < 40.0 && cosine > 0.82;
            }
            if (at_eye) st.near_at = now_s;
            want = now_s - st.near_at < 0.3 && now_s >= st.near_at;
        }
        // Zoom for where your eye is (vr_scope_zoom=eye): only a scope near
        // the eye; the others keep the last view angle.
        if (g_scope_zoom_eye && g_capture_fov.valid() && have_x && Length(x.translation - eye) < 60.0) {
            if (!st.lens_ok && st.lens_tries < 3) {
                ++st.lens_tries;
                st.lens_ok = MeasureLens(t.obj, x, st);
            }
            if (st.lens_ok) {
                const FVector lens_w = x.translation + x.rotation.Rotate(st.lens_c);
                const double d = Length(eye - lens_w);
                const float fov = EyeFov(st, d);
                // Against what the lens camera has now, in case the game sets
                // it again (it is set once at BeginPlay in the scripts read).
                const float now_fov = mem::ReadOr<float>(t.obj + g_capture_fov.offset, -1.f);
                static int game_resets = 0;
                if (st.set_fov > 0 && std::fabs(now_fov - st.set_fov) > 0.05f && game_resets < 3) {
                    ++game_resets;
                    log::Info("render fixes: the game set %s's lens view again (%.1f deg; ours was %.1f)",
                              ue::GetName(ue::GetOuter(t.obj)).c_str(), now_fov, st.set_fov);
                }
                if (std::fabs(fov - now_fov) > 0.05f && mem::Write<float>(t.obj + g_capture_fov.offset, fov)) {
                    if (st.set_fov < 0)
                        log::Info("render fixes: %s's lens view set for your eye: %.1f deg with the lens %.1f cm from "
                                  "it (the game's %.1f deg was for its camera %.1f cm away)",
                                  ue::GetName(ue::GetOuter(t.obj)).c_str(), fov, d, st.game_fov, st.ads_dist);
                    st.set_fov = fov;
                }
                std::lock_guard<std::mutex> lock(g_zoom_mutex);
                ++g_zoom.frames;
                g_zoom.fov_lo = std::fmin(g_zoom.fov_lo, fov);
                g_zoom.fov_hi = std::fmax(g_zoom.fov_hi, fov);
                g_zoom.d_lo = std::fmin(g_zoom.d_lo, d);
                g_zoom.d_hi = std::fmax(g_zoom.d_hi, d);
                g_zoom.game_fov = st.game_fov;
                g_zoom.ads_dist = st.ads_dist;
            }
        }
        bool switched = false;
        if (want) {
            if (st.off) {
                SetBool(t.obj + g_capture_on_movement.offset, g_capture_on_movement.bool_mask, true);
                st.off = false;
                switched = true;
            }
        } else if (!st.off && Bit(t.obj, g_capture_on_movement)) {
            // Only what the game left on, so it is put back as it was.
            SetBool(t.obj + g_capture_on_movement.offset, g_capture_on_movement.bool_mask, false);
            st.off = true;
            switched = true;
        }
        if (switched) {
            ++g_scope_switches;
            if (g_scope_logged < 6) {
                ++g_scope_logged;
                log::Info("render fixes: %s's view %s", ue::GetName(ue::GetOuter(t.obj)).c_str(),
                          st.off ? "stopped (the scope is away from the eye; its lens keeps the last picture)"
                                 : "drawn again (the scope is at the eye, or vr_scope_view=always)");
            }
        }
        if (!st.off && Bit(t.obj, g_capture_on_movement)) any_drawn = true;
    }
    ++g_scope_frames;
    if (any_drawn) ++g_scope_drawn_frames;
}

std::string ExposureReport() {
    std::vector<Target> targets;
    {
        std::lock_guard<std::mutex> lock(g_targets_mutex);
        targets = g_targets;
    }
    std::string out;
    int n = 0;
    for (const Target& t : targets) {
        if (n >= 8) break;
        if (t.index < 0 || ue::objects::Get(t.index) != t.obj) continue;
        Exposure e = ReadExposure(t.obj);
        if (!e.ok) continue;
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s%s exp %d%s bias %.2f%s%s", n ? " | " : "",
                      ue::GetName(ue::GetClass(t.obj)).c_str(), e.method, e.method_set ? "*" : "", e.bias,
                      e.bias_set ? "*" : "", e.physical ? " physical" : "");
        out += buf;
        ++n;
    }
    return out.empty() ? "none read" : out;
}

}  // namespace holevr::vr::render_fixes
