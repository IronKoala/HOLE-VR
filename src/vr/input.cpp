#include "vr/input.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/perf.h"
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
// The same controllers predicted ahead to when the game's picture will be
// shown (xr::PredictAhead), for the game thread: Consume and PeekGame put
// these poses in. Guarded by g_mutex.
struct GamePoses {
    bool grip_valid = false, aim_valid = false;
    XrPosef grip{{0, 0, 0, 1}, {0, 0, 0}}, aim{{0, 0, 0, 1}, {0, 0, 0}};
    XrVector3f linear_velocity{0, 0, 0};
};
GamePoses g_game[2];
bool g_have_game = false;
// The same controllers located for each moment the head was located for
// this headset frame (xr::PictureFrames), so the game can take them at
// exactly the moment its picture's eyes are for (xr::NewPictureShownTime).
// Hands taken at the smoothed lag instead were a frame off the eyes
// whenever the game's pipeline depth changed (often in a busy match), so a
// moving hand and its gun jumped against the view. Guarded by g_mutex.
constexpr int kMaxPictures = 8;
struct PicturePoses {
    XrTime time = 0;
    GamePoses hand[2];
};
PicturePoses g_pic[kMaxPictures];
int g_pic_count = 0;
XrDuration g_pic_period = 0;
// For the log: game frames whose hands matched their picture's moment.
std::atomic<long long> g_hand_matched{0}, g_hand_fallback{0};

std::string HandReport(double) {
    const long long m = g_hand_matched.exchange(0), f = g_hand_fallback.exchange(0);
    if (m + f == 0) return {};
    char line[200];
    std::snprintf(line, sizeof(line),
                  "frame timing: hands taken at their picture's own moment in %.0f%% of game frames "
                  "(else at the smoothed lag)",
                  100.0 * m / (m + f));
    return line;
}
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
        GamePoses game[2];
        const XrDuration ahead = xr::PredictAhead();
        static bool s_report = [] {
            perf::AddReportLine(&HandReport);
            return true;
        }();
        (void)s_report;
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
            // For the game: the same, at the time its picture will be shown.
            GamePoses& gp = game[h];
            gp = GamePoses{};
            gp.grip_valid = hi.grip_valid, gp.grip = hi.grip, gp.linear_velocity = hi.linear_velocity;
            gp.aim_valid = hi.aim_valid, gp.aim = hi.aim;
            if (ahead > 0) {
                XrSpaceVelocity gvel{XR_TYPE_SPACE_VELOCITY};
                XrSpaceLocation gloc{XR_TYPE_SPACE_LOCATION, &gvel};
                if (g_grip_space[h] && XR_SUCCEEDED(locate(g_grip_space[h], space, time + ahead, &gloc)) &&
                    (gloc.locationFlags & kTracked) == kTracked) {
                    gp.grip_valid = true;
                    gp.grip = gloc.pose;
                    if (gvel.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) gp.linear_velocity = gvel.linearVelocity;
                }
                XrSpaceLocation galoc{XR_TYPE_SPACE_LOCATION};
                if (g_aim_space[h] && XR_SUCCEEDED(locate(g_aim_space[h], space, time + ahead, &galoc)) &&
                    (galoc.locationFlags & kTracked) == kTracked) {
                    gp.aim_valid = true;
                    gp.aim = galoc.pose;
                }
            }
        }

        // Each moment the head was located for (see g_pic).
        PicturePoses pic[kMaxPictures];
        int pic_count = 0;
        int lo = 0, hi = -1;
        XrDuration period = 0;
        xr::PictureFrames(&lo, &hi, &period);
        constexpr XrSpaceLocationFlags kTrackedBoth =
            XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        for (int k = lo; k <= hi && pic_count < kMaxPictures; ++k) {
            PicturePoses& pp = pic[pic_count];
            pp.time = time + static_cast<XrTime>(k) * period;
            for (int h = 0; h < 2; ++h) {
                GamePoses& gp = pp.hand[h];
                XrSpaceVelocity gvel{XR_TYPE_SPACE_VELOCITY};
                XrSpaceLocation gloc{XR_TYPE_SPACE_LOCATION, &gvel};
                if (g_grip_space[h] && XR_SUCCEEDED(locate(g_grip_space[h], space, pp.time, &gloc)) &&
                    (gloc.locationFlags & kTrackedBoth) == kTrackedBoth) {
                    gp.grip_valid = true;
                    gp.grip = gloc.pose;
                    if (gvel.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) gp.linear_velocity = gvel.linearVelocity;
                }
                XrSpaceLocation galoc{XR_TYPE_SPACE_LOCATION};
                if (g_aim_space[h] && XR_SUCCEEDED(locate(g_aim_space[h], space, pp.time, &galoc)) &&
                    (galoc.locationFlags & kTrackedBoth) == kTrackedBoth) {
                    gp.aim_valid = true;
                    gp.aim = galoc.pose;
                }
            }
            ++pic_count;
        }

        std::vector<HapticRequest> haptics;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (int i = 0; i < pic_count; ++i) g_pic[i] = pic[i];
            g_pic_count = pic_count;
            g_pic_period = period;
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
            for (int h = 0; h < 2; ++h) g_game[h] = game[h];
            g_have_game = true;
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
        g_have_game = false;
        g_pic_count = 0;
    }
};

InputListener g_listener;

}  // namespace

void Init() { xr::RegisterListener(&g_listener); }

// g_latest with the game's controller poses: those located for the moment
// the picture being prepared will be shown (`shown`, from
// xr::NewPictureShownTime, taken before g_mutex so the two locks never
// nest), else the smoothed lag. Under g_mutex.
Snapshot ForGame(XrTime shown, bool count) {
    Snapshot s = g_latest;
    if (!g_have_game) return s;
    const GamePoses* use = g_game;
    if (shown > 0 && g_pic_period > 0) {
        for (int i = 0; i < g_pic_count; ++i) {
            const XrTime d = g_pic[i].time > shown ? g_pic[i].time - shown : shown - g_pic[i].time;
            if (d * 2 < g_pic_period) {
                use = g_pic[i].hand;
                break;
            }
        }
    }
    if (count) (use == g_game ? g_hand_fallback : g_hand_matched).fetch_add(1);
    for (int h = 0; h < 2; ++h) {
        HandInput& hi = s.hand[h];
        // A hand lost at that moment keeps the smoothed-lag pose.
        const GamePoses& gp = use[h].grip_valid ? use[h] : g_game[h];
        hi.grip_valid = gp.grip_valid;
        hi.grip = gp.grip;
        hi.linear_velocity = gp.linear_velocity;
        hi.aim_valid = gp.aim_valid;
        hi.aim = gp.aim;
    }
    return s;
}

Snapshot Consume() {
    const XrTime shown = xr::NewPictureShownTime();
    std::lock_guard<std::mutex> lock(g_mutex);
    Snapshot s = ForGame(shown, true);
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

Snapshot Peek() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_latest;
}

Snapshot LastConsumed() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_consumed;
}

Snapshot PeekGame() {
    const XrTime shown = xr::NewPictureShownTime();
    std::lock_guard<std::mutex> lock(g_mutex);
    return ForGame(shown, false);
}

void Haptic(Hand hand, float amplitude, float seconds, float frequency_hz) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_haptics.size() < 16) g_haptics.push_back({hand, amplitude, seconds, frequency_hz});
}

}  // namespace holevr::vr::input
