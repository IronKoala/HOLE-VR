#include "vr/render_fixes.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
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

// Console settings for the pause blur.
// - Nothing nearer than the focus gets blurred: hands, arms, body, menu.
// - No bokeh sprites ("scatter"): in the owner's 2026-11-03 run the left eye
//   of a blurred menu frame (capture-forearm-UIM_Results_C.bmp, the game's
//   own picture before it reaches the headset) was covered in white dots and
//   bars in tile-sized blocks, the depth of field's sprite passes going wrong
//   with both eyes in one frame. The gather blur alone gives the soft
//   background. pause_blur_scatter=1 puts the sprites back.
std::vector<std::pair<std::string, std::string>> PauseBlurCvars() {
    std::vector<std::pair<std::string, std::string>> v = {{"r.DOF.Kernel.MaxForegroundRadius", "0.0"}};
    if (!config::GetBool("pause_blur_scatter", false)) {
        v.push_back({"r.DOF.Scatter.ForegroundCompositing", "0"});
        v.push_back({"r.DOF.Scatter.BackgroundCompositing", "0"});
    }
    return v;
}

// Pause blur: while a forearm menu is up, depth of field is switched on and
// focused about arm's length away, so the world behind the menu goes soft
// while the hands, arms and menu stay sharp. Foreground blur is turned off
// (r.DOF.Kernel.MaxForegroundRadius 0), so nothing nearer than the focus is
// blurred. It is written into the same cameras and post-process components
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

UObject g_cls_pp_component = 0, g_cls_pp_volume = 0, g_cls_camera = 0, g_cls_capture2d = 0;
int32_t g_pp_component_settings = -1, g_pp_volume_settings = -1, g_camera_settings = -1;
refl::PropertyInfo g_capture_every_frame, g_capture_on_movement;
bool g_disable_player_capture = true;
bool g_ready = false;
// The player's scene captures we stop (Apply), so the death screen can run
// them again (AllowPlayerCapture), and whether it currently does.
std::mutex g_captures_mutex;
std::vector<Target> g_captures;
std::atomic<bool> g_allow_capture{false};
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
void ForceNeutral(uintptr_t settings) {
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

Kind Classify(UObject cls) {
    Kind k;
    if (g_cls_camera && refl::IsChildOf(cls, g_cls_camera)) k.settings_offset = g_camera_settings;
    else if (g_cls_pp_component && refl::IsChildOf(cls, g_cls_pp_component))
        k.settings_offset = g_pp_component_settings;
    else if (g_cls_pp_volume && refl::IsChildOf(cls, g_cls_pp_volume))
        k.settings_offset = g_pp_volume_settings;
    else if (g_cls_capture2d && refl::IsChildOf(cls, g_cls_capture2d)) k.is_capture = true;
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

}  // namespace

bool Init() {
    if (!refl::Ready()) return false;
    g_cls_pp_component = ue::FindObject("Class /Script/Engine.PostProcessComponent");
    g_cls_pp_volume = ue::FindObject("Class /Script/Engine.PostProcessVolume");
    g_cls_camera = ue::FindObject("Class /Script/Engine.CameraComponent");
    g_cls_capture2d = ue::FindObject("Class /Script/Engine.SceneCaptureComponent2D");
    UObject pp_struct = ue::FindObject("ScriptStruct /Script/Engine.PostProcessSettings");
    if (!pp_struct || !g_cls_camera) {
        log::Warn("render fixes: engine post-process types not found, fixes disabled");
        return false;
    }
    if (g_cls_pp_component)
        g_pp_component_settings = refl::FindPropertyOffset(g_cls_pp_component, "Settings");
    if (g_cls_pp_volume) g_pp_volume_settings = refl::FindPropertyOffset(g_cls_pp_volume, "Settings");
    g_camera_settings = refl::FindPropertyOffset(g_cls_camera, "PostProcessSettings");
    if (g_cls_capture2d) {
        g_capture_every_frame = refl::FindProperty(g_cls_capture2d, "bCaptureEveryFrame");
        g_capture_on_movement = refl::FindProperty(g_cls_capture2d, "bCaptureOnMovement");
    }
    g_disable_player_capture = config::GetBool("vr_disable_player_scene_capture", true);

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
              "component 0x%x, volume 0x%x; player scene capture %s",
              enabled, g_camera_settings, g_pp_component_settings, g_pp_volume_settings,
              g_disable_player_capture ? "off" : "left on");
    g_ready = true;
    return true;
}

void Apply() {
    if (!g_ready) return;
    // A pass over every object each second: remember memory regions for the
    // pass, or most reads ask Windows again (2026-11-03 performance pass).
    mem::ScopedRegionCache region_cache;
    Counts c;
    std::vector<Target> targets, captures;
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
            targets.push_back({obj, mem::ReadOr<int32_t>(obj + ue::off::kObjIndex, -1), k.settings_offset});
            if (k.settings_offset == g_camera_settings) ++c.cameras;
            else if (k.settings_offset == g_pp_component_settings) ++c.components;
            else ++c.volumes;
        } else if (k.is_capture && g_disable_player_capture && IsPlayerCapture(obj)) {
            captures.push_back({obj, mem::ReadOr<int32_t>(obj + ue::off::kObjIndex, -1), 0});
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
        log::Info("render fixes: player scene capture %s %s (picture into %s)", ue::GetName(t.obj).c_str(),
                  allow ? "running again for the death screen" : "stopped again",
                  target ? ue::GetName(target).c_str() : "nothing");
        ++n;
    }
    return n;
}

}  // namespace holevr::vr::render_fixes
