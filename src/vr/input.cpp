#include "vr/input.h"

#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"
#include "vr/openxr.h"

namespace holevr::vr::input {
namespace {

template <typename F>
F Fn(const char* name) {
    return reinterpret_cast<F>(xr::GetProc(name));
}

XrInstance g_instance = XR_NULL_HANDLE;
XrActionSet g_set = XR_NULL_HANDLE;
XrPath g_hand_path[2] = {XR_NULL_PATH, XR_NULL_PATH};
XrSpace g_grip_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
XrSpace g_aim_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};

struct Actions {
    XrAction grip_pose = XR_NULL_HANDLE, aim_pose = XR_NULL_HANDLE;
    XrAction trigger = XR_NULL_HANDLE, trigger_touch = XR_NULL_HANDLE, squeeze = XR_NULL_HANDLE;
    XrAction stick = XR_NULL_HANDLE, stick_click = XR_NULL_HANDLE;
    XrAction primary = XR_NULL_HANDLE, secondary = XR_NULL_HANDLE, menu = XR_NULL_HANDLE;
    XrAction haptic = XR_NULL_HANDLE;
} A;

std::mutex g_mutex;
Snapshot g_latest;     // written on the render thread
Snapshot g_consumed;   // what the game thread last saw (for edges)
bool g_pending_edges[2][6][2] = {};  // [hand][button][pressed/released] accumulated

struct HapticRequest {
    Hand hand;
    float amplitude, seconds, frequency;
};
std::vector<HapticRequest> g_haptics;

XrPath Path(const char* s) {
    static auto string_to_path = Fn<PFN_xrStringToPath>("xrStringToPath");
    XrPath p = XR_NULL_PATH;
    if (string_to_path) string_to_path(g_instance, s, &p);
    return p;
}

bool Check(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return true;
    log::Error("input: %s failed (%d)", what, static_cast<int>(r));
    return false;
}

XrAction MakeAction(const char* name, const char* localized, XrActionType type) {
    static auto create = Fn<PFN_xrCreateAction>("xrCreateAction");
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(info.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    info.actionType = type;
    info.countSubactionPaths = 2;
    info.subactionPaths = g_hand_path;
    XrAction a = XR_NULL_HANDLE;
    Check(create(g_set, &info, &a), name);
    return a;
}

struct Binding {
    XrAction action;
    const char* path;
};

void Suggest(const char* profile, const std::vector<Binding>& bindings) {
    static auto suggest = Fn<PFN_xrSuggestInteractionProfileBindings>("xrSuggestInteractionProfileBindings");
    std::vector<XrActionSuggestedBinding> b;
    for (const auto& x : bindings) {
        XrPath p = Path(x.path);
        if (x.action && p) b.push_back({x.action, p});
    }
    XrInteractionProfileSuggestedBinding s{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    s.interactionProfile = Path(profile);
    s.countSuggestedBindings = static_cast<uint32_t>(b.size());
    s.suggestedBindings = b.data();
    XrResult r = suggest(g_instance, &s);
    if (XR_SUCCEEDED(r)) log::Info("input: bindings for %s (%zu)", profile, b.size());
    else log::Warn("input: runtime rejected bindings for %s (%d)", profile, static_cast<int>(r));
}

std::vector<Binding> BothHands(const std::vector<std::pair<XrAction, const char*>>& per_hand) {
    std::vector<Binding> out;
    for (const char* hand : {"/user/hand/left", "/user/hand/right"}) {
        for (const auto& [action, suffix] : per_hand) {
            static std::deque<std::string> storage;  // keeps c_str() stable
            storage.push_back(std::string(hand) + suffix);
            out.push_back({action, storage.back().c_str()});
        }
    }
    return out;
}

void SuggestAll() {
    // Valve Index: SteamVR maps most controllers (including newer Valve ones)
    // onto these bindings, so this is the main profile.
    auto index = BothHands({{A.grip_pose, "/input/grip/pose"},
                            {A.aim_pose, "/input/aim/pose"},
                            {A.trigger, "/input/trigger/value"},
                            {A.trigger_touch, "/input/trigger/touch"},
                            {A.squeeze, "/input/squeeze/value"},
                            {A.stick, "/input/thumbstick"},
                            {A.stick_click, "/input/thumbstick/click"},
                            {A.primary, "/input/a/click"},
                            {A.secondary, "/input/b/click"},
                            {A.haptic, "/output/haptic"}});
    Suggest("/interaction_profiles/valve/index_controller", index);

    auto touch = BothHands({{A.grip_pose, "/input/grip/pose"},
                            {A.aim_pose, "/input/aim/pose"},
                            {A.trigger, "/input/trigger/value"},
                            {A.trigger_touch, "/input/trigger/touch"},
                            {A.squeeze, "/input/squeeze/value"},
                            {A.stick, "/input/thumbstick"},
                            {A.stick_click, "/input/thumbstick/click"},
                            {A.haptic, "/output/haptic"}});
    touch.push_back({A.primary, "/user/hand/left/input/x/click"});
    touch.push_back({A.secondary, "/user/hand/left/input/y/click"});
    touch.push_back({A.menu, "/user/hand/left/input/menu/click"});
    touch.push_back({A.primary, "/user/hand/right/input/a/click"});
    touch.push_back({A.secondary, "/user/hand/right/input/b/click"});
    Suggest("/interaction_profiles/oculus/touch_controller", touch);

    auto simple = BothHands({{A.grip_pose, "/input/grip/pose"},
                             {A.aim_pose, "/input/aim/pose"},
                             {A.trigger, "/input/select/click"},
                             {A.menu, "/input/menu/click"},
                             {A.haptic, "/output/haptic"}});
    Suggest("/interaction_profiles/khr/simple_controller", simple);
}

struct InputListener : xr::Listener {
    void OnInstanceCreated(XrInstance instance) override {
        g_instance = instance;
        auto create_set = Fn<PFN_xrCreateActionSet>("xrCreateActionSet");
        if (!create_set) return;
        XrActionSetCreateInfo info{XR_TYPE_ACTION_SET_CREATE_INFO};
        std::strcpy(info.actionSetName, "holevr");
        std::strcpy(info.localizedActionSetName, "HOLE VR");
        if (!Check(create_set(instance, &info, &g_set), "xrCreateActionSet")) return;
        g_hand_path[kLeft] = Path("/user/hand/left");
        g_hand_path[kRight] = Path("/user/hand/right");

        A.grip_pose = MakeAction("grip_pose", "Hand (grip)", XR_ACTION_TYPE_POSE_INPUT);
        A.aim_pose = MakeAction("aim_pose", "Hand (pointing)", XR_ACTION_TYPE_POSE_INPUT);
        A.trigger = MakeAction("trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT);
        A.trigger_touch = MakeAction("trigger_touch", "Trigger touch", XR_ACTION_TYPE_BOOLEAN_INPUT);
        A.squeeze = MakeAction("grab", "Grab", XR_ACTION_TYPE_FLOAT_INPUT);
        A.stick = MakeAction("thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT);
        A.stick_click = MakeAction("thumbstick_click", "Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT);
        A.primary = MakeAction("primary", "Primary button", XR_ACTION_TYPE_BOOLEAN_INPUT);
        A.secondary = MakeAction("secondary", "Secondary button", XR_ACTION_TYPE_BOOLEAN_INPUT);
        A.menu = MakeAction("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT);
        A.haptic = MakeAction("haptic", "Vibration", XR_ACTION_TYPE_VIBRATION_OUTPUT);
        SuggestAll();
    }

    void OnSessionCreated(XrSession session, std::vector<XrActionSet>& sets) override {
        if (!g_set) return;
        auto create_space = Fn<PFN_xrCreateActionSpace>("xrCreateActionSpace");
        for (int h = 0; h < 2; ++h) {
            XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            info.poseInActionSpace.orientation.w = 1.f;
            info.subactionPath = g_hand_path[h];
            info.action = A.grip_pose;
            Check(create_space(session, &info, &g_grip_space[h]), "grip space");
            info.action = A.aim_pose;
            Check(create_space(session, &info, &g_aim_space[h]), "aim space");
        }
        sets.push_back(g_set);
        log::Info("input: controller actions ready");
    }

    void OnFrame(XrSession session, XrSpace space, XrTime time) override {
        if (!g_set) return;
        static auto sync = Fn<PFN_xrSyncActions>("xrSyncActions");
        static auto get_float = Fn<PFN_xrGetActionStateFloat>("xrGetActionStateFloat");
        static auto get_bool = Fn<PFN_xrGetActionStateBoolean>("xrGetActionStateBoolean");
        static auto get_vec2 = Fn<PFN_xrGetActionStateVector2f>("xrGetActionStateVector2f");
        static auto get_pose = Fn<PFN_xrGetActionStatePose>("xrGetActionStatePose");
        static auto locate = Fn<PFN_xrLocateSpace>("xrLocateSpace");
        static auto haptic = Fn<PFN_xrApplyHapticFeedback>("xrApplyHapticFeedback");

        XrActiveActionSet active{g_set, XR_NULL_PATH};
        XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
        si.countActiveActionSets = 1;
        si.activeActionSets = &active;
        XrResult sr = sync(session, &si);
        if (XR_FAILED(sr)) return;  // e.g. session not focused (dashboard open)

        Snapshot s;
        s.valid = true;
        s.time = time;
        for (int h = 0; h < 2; ++h) {
            HandInput& hi = s.hand[h];
            XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
            gi.subactionPath = g_hand_path[h];

            auto f = [&](XrAction a) {
                XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
                gi.action = a;
                return a && XR_SUCCEEDED(get_float(session, &gi, &st)) && st.isActive ? st.currentState : 0.f;
            };
            auto b = [&](XrAction a) {
                XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
                gi.action = a;
                return a && XR_SUCCEEDED(get_bool(session, &gi, &st)) && st.isActive && st.currentState;
            };
            hi.trigger = f(A.trigger);
            hi.trigger_touch = b(A.trigger_touch) || hi.trigger > 0.05f;
            hi.grip_value = f(A.squeeze);
            {
                XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
                gi.action = A.stick;
                if (XR_SUCCEEDED(get_vec2(session, &gi, &st)) && st.isActive) {
                    hi.stick_x = st.currentState.x;
                    hi.stick_y = st.currentState.y;
                }
            }
            hi.stick_click.down = b(A.stick_click);
            hi.primary.down = b(A.primary);
            hi.secondary.down = b(A.secondary);
            hi.menu.down = b(A.menu);

            XrActionStatePose ps{XR_TYPE_ACTION_STATE_POSE};
            gi.action = A.grip_pose;
            hi.active = XR_SUCCEEDED(get_pose(session, &gi, &ps)) && ps.isActive;

            XrSpaceVelocity vel{XR_TYPE_SPACE_VELOCITY};
            XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION, &vel};
            constexpr XrSpaceLocationFlags kTracked =
                XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            if (g_grip_space[h] && XR_SUCCEEDED(locate(g_grip_space[h], space, time, &loc)) &&
                (loc.locationFlags & kTracked) == kTracked) {
                hi.grip_valid = true;
                hi.grip = loc.pose;
                if (vel.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) hi.linear_velocity = vel.linearVelocity;
            }
            XrSpaceLocation aloc{XR_TYPE_SPACE_LOCATION};
            if (g_aim_space[h] && XR_SUCCEEDED(locate(g_aim_space[h], space, time, &aloc)) &&
                (aloc.locationFlags & kTracked) == kTracked) {
                hi.aim_valid = true;
                hi.aim = aloc.pose;
            }
        }

        std::vector<HapticRequest> haptics;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            // Accumulate edges so a press shorter than a game frame still counts.
            for (int h = 0; h < 2; ++h) {
                const HandInput& prev = g_latest.hand[h];
                const HandInput& cur = s.hand[h];
                const bool prev_down[6] = {prev.stick_click.down, prev.primary.down, prev.secondary.down,
                                           prev.menu.down, prev.trigger_button.down, prev.grip_button.down};
                // Thresholds with hysteresis for analog buttons.
                bool trig = prev.trigger_button.down ? cur.trigger > 0.55f : cur.trigger > 0.75f;
                bool grab = prev.grip_button.down ? cur.grip_value > 0.35f : cur.grip_value > 0.6f;
                s.hand[h].trigger_button.down = trig;
                s.hand[h].grip_button.down = grab;
                const bool now_down[6] = {cur.stick_click.down, cur.primary.down, cur.secondary.down,
                                          cur.menu.down, trig, grab};
                for (int i = 0; i < 6; ++i) {
                    if (now_down[i] && !prev_down[i]) g_pending_edges[h][i][0] = true;
                    if (!now_down[i] && prev_down[i]) g_pending_edges[h][i][1] = true;
                }
            }
            g_latest = s;
            haptics.swap(g_haptics);
        }
        for (const auto& r : haptics) {
            XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
            v.amplitude = r.amplitude;
            v.duration = static_cast<XrDuration>(r.seconds * 1e9);
            v.frequency = r.frequency > 0 ? r.frequency : XR_FREQUENCY_UNSPECIFIED;
            XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
            hi.action = A.haptic;
            hi.subactionPath = g_hand_path[r.hand];
            haptic(session, &hi, reinterpret_cast<const XrHapticBaseHeader*>(&v));
        }
    }

    void OnSessionEnding() override {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_latest = Snapshot();
    }
};

InputListener g_listener;

}  // namespace

void Init() { xr::RegisterListener(&g_listener); }

Snapshot Consume() {
    std::lock_guard<std::mutex> lock(g_mutex);
    Snapshot s = g_latest;
    for (int h = 0; h < 2; ++h) {
        Button* buttons[6] = {&s.hand[h].stick_click, &s.hand[h].primary, &s.hand[h].secondary,
                              &s.hand[h].menu, &s.hand[h].trigger_button, &s.hand[h].grip_button};
        for (int i = 0; i < 6; ++i) {
            buttons[i]->pressed = g_pending_edges[h][i][0];
            buttons[i]->released = g_pending_edges[h][i][1];
            g_pending_edges[h][i][0] = g_pending_edges[h][i][1] = false;
        }
    }
    g_consumed = s;
    return s;
}

void Haptic(Hand hand, float amplitude, float seconds, float frequency_hz) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_haptics.size() < 16) g_haptics.push_back({hand, amplitude, seconds, frequency_hz});
}

}  // namespace holevr::vr::input
