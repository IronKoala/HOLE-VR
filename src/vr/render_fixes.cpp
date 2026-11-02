#include "vr/render_fixes.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "ue/objects.h"
#include "ue/reflection.h"

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

struct Kind {
    int32_t settings_offset = -1;  // FPostProcessSettings inside the object, or -1
    bool is_capture = false;
};

UObject g_cls_pp_component = 0, g_cls_pp_volume = 0, g_cls_camera = 0, g_cls_capture2d = 0;
int32_t g_pp_component_settings = -1, g_pp_volume_settings = -1, g_camera_settings = -1;
refl::PropertyInfo g_capture_every_frame, g_capture_on_movement;
bool g_disable_player_capture = true;
bool g_ready = false;
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

void ForceNeutral(uintptr_t settings) {
    for (const Fix& f : g_fixes) {
        if (f.value_offset < 0) continue;
        float cur;
        if (!mem::Read(settings + f.value_offset, cur)) return;
        if (cur != f.neutral) mem::Write(settings + f.value_offset, f.neutral);
        SetBool(settings + f.flag_offset, f.flag_mask, true);
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
    log::Info("render fixes: %d post-process overrides active; camera settings at 0x%x, "
              "component 0x%x, volume 0x%x; player scene capture %s",
              enabled, g_camera_settings, g_pp_component_settings, g_pp_volume_settings,
              g_disable_player_capture ? "off" : "left on");
    g_ready = true;
    return true;
}

void Apply() {
    if (!g_ready) return;
    Counts c;
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
            ForceNeutral(obj + k.settings_offset);
            if (k.settings_offset == g_camera_settings) ++c.cameras;
            else if (k.settings_offset == g_pp_component_settings) ++c.components;
            else ++c.volumes;
        } else if (k.is_capture && g_disable_player_capture && IsPlayerCapture(obj)) {
            bool ok = true;
            if (g_capture_every_frame.bool_mask)
                ok &= SetBool(obj + g_capture_every_frame.offset, g_capture_every_frame.bool_mask, false);
            if (g_capture_on_movement.bool_mask)
                ok &= SetBool(obj + g_capture_on_movement.offset, g_capture_on_movement.bool_mask, false);
            if (ok) ++c.captures;
        }
    });
    if (c != g_last) {
        log::Info("render fixes applied: %d cameras, %d post-process components, %d volumes, "
                  "%d player scene captures stopped",
                  c.cameras, c.components, c.volumes, c.captures);
        g_last = c;
    }
}

}  // namespace holevr::vr::render_fixes
