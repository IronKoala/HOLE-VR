#include "vr/stereo.h"

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "ue/process_event.h"
#include "core/perf.h"
#include "core/memory.h"
#include "hooks/hooks.h"
#include "ue/objects.h"
#include "vr/dxgi_hooks.h"
#include "vr/math.h"
#include "vr/openxr.h"

namespace holevr::vr::stereo {
namespace {

// ----------------------------------------------------------- command line --

std::wstring g_cmdline;
using GetCommandLineWFn = LPWSTR(WINAPI*)();
GetCommandLineWFn o_get_command_line_w = nullptr;

LPWSTR WINAPI HookGetCommandLineW() { return g_cmdline.data(); }

// ------------------------------------------------- FFakeStereoRenderingDevice --
//
// Layout in this build (UE 5.5), confirmed from its constructor:
//   0x00 IStereoRendering vtable          0x08 IStereoRenderTargetManager vtable
//   0x10 float FOVInDegrees               0x14 int32 Width   0x18 int32 Height
//   0x1C int32 NumViews (2)               0x20 bool use separate render target
// Both vtables are checked slot by slot at runtime before anything is swapped.
constexpr uintptr_t kFov = 0x10, kWidth = 0x14, kHeight = 0x18, kNumViews = 0x1C, kSeparateRT = 0x20;
constexpr size_t kPrimarySlots = 20;
constexpr size_t kSecondarySlots = 16;
// IStereoRendering slots (UE 5.5 header order).
constexpr size_t kIsStereoEnabled = 1;
constexpr size_t kCalculateStereoViewOffset = 11;
constexpr size_t kGetStereoProjectionMatrix = 12;
constexpr size_t kRenderTextureRenderThread = 14;
// IStereoRenderTargetManager slots.
constexpr size_t kShouldUseSeparateRenderTarget = 0;
constexpr size_t kCalculateRenderTargetSize = 2;
constexpr size_t kNeedReAllocateViewportRenderTarget = 3;

using CalcOffsetFn = void (*)(void*, int32_t, FRotator*, float, FVector*);
using ProjectionFn = double* (*)(void*, double*, int32_t);
using RenderTextureFn = void (*)(void*, void*, void*, void*, void*);
using CalcRTSizeFn = void (*)(void*, const void*, uint32_t*, uint32_t*);
using NeedReallocFn = bool (*)(void*, const void*);
using ShouldUseSeparateRTFn = bool (*)(void*);

uintptr_t g_device = 0;
void* g_primary[kPrimarySlots];
void* g_secondary[kSecondarySlots];
CalcOffsetFn o_calc_offset = nullptr;
ProjectionFn o_projection = nullptr;
RenderTextureFn o_render_texture = nullptr;
CalcRTSizeFn o_calc_rt_size = nullptr;
NeedReallocFn o_need_realloc = nullptr;
ShouldUseSeparateRTFn o_should_use_separate_rt = nullptr;
std::atomic<bool> g_separate_rt{false};
std::atomic<int> g_null_mirror_frames{0};

const float* g_near_plane = nullptr;  // GNearClippingPlane, read from the original projection code
std::atomic<bool> g_realloc{false};
uint32_t g_eye_w = 0, g_eye_h = 0;

// Views snapshotted for the frame being set up, so both eyes and the
// projection matrices come from the same tracking sample.
std::mutex g_frame_mutex;
xr::EyeView g_frame_views[2];
bool g_have_frame_views = false;

// Recentering, in UE axes but metres (scaled by WorldToMeters on use).
std::atomic<bool> g_recenter{true};
std::atomic<std::uint64_t> g_origin_generation{1};
FVector g_origin_m;
double g_origin_yaw = 0.0;
std::atomic<double> g_standing_eye_m{0.0};   // 0: not measured
std::atomic<bool> g_standing_apply{false};
std::atomic<XrTime> g_standing_clear_after{0};

// Logs the first time each hook runs, so a log shows which engine paths are live.
#define HOLEVR_FIRST_CALL(name)                                  \
    do {                                                         \
        static std::atomic<bool> seen{false};                    \
        if (!seen.exchange(true)) log::Info("stereo: first call to %s", name); \
    } while (0)

bool InText(uintptr_t p) {
    const mem::Section* t = mem::Game().Find(".text");
    return t && p >= t->start && p < t->start + t->size;
}

bool VtableLooksValid(uintptr_t vt, size_t slots) {
    for (size_t i = 0; i < slots; ++i)
        if (!InText(mem::ReadOr<uintptr_t>(vt + i * 8))) return false;
    return true;
}

bool LooksLikeFakeStereoDevice(uintptr_t p) {
    if (!mem::LooksLikePointer(p) || !mem::IsReadable(p, 0x28)) return false;
    uintptr_t vt = mem::ReadOr<uintptr_t>(p);
    const mem::Section* rdata = mem::Game().Find(".rdata");
    if (!rdata || vt < rdata->start || vt >= rdata->start + rdata->size) return false;
    // IsStereoEnabled() { return true; } is "mov al, 1; ret".
    uintptr_t is_enabled = mem::ReadOr<uintptr_t>(vt + kIsStereoEnabled * 8);
    unsigned char code[3];
    if (!mem::Read(is_enabled, code) || code[0] != 0xB0 || code[1] != 0x01 || code[2] != 0xC3) return false;
    float fov = mem::ReadOr<float>(p + kFov);
    return mem::ReadOr<int32_t>(p + kNumViews) == 2 && fov > 10.0f && fov < 179.0f &&
           mem::ReadOr<uint8_t>(p + kSeparateRT) <= 1 && VtableLooksValid(vt, kPrimarySlots) &&
           VtableLooksValid(mem::ReadOr<uintptr_t>(p + 8), kSecondarySlots);
}

ue::UObject FindGameEngine() {
    ue::UObject found = 0;
    ue::objects::ForEach([&](int, ue::UObject o) {
        if (found) return;
        if (ue::GetName(ue::GetClass(o)) != "GameEngine") return;
        if (ue::GetName(o).rfind("Default__", 0) == 0) return;
        found = o;
    });
    return found;
}

// GEngine->StereoRenderingDevice is a TSharedPtr; find its object pointer.
uintptr_t FindDevice(ue::UObject engine) {
    for (uintptr_t off = 0x400; off < 0x2000; off += 8) {
        uintptr_t p = mem::ReadOr<uintptr_t>(engine + off);
        if (LooksLikeFakeStereoDevice(p)) {
            log::Info("stereo: emulated stereo device at GEngine+0x%llx", static_cast<unsigned long long>(off));
            return p;
        }
    }
    return 0;
}

// The original projection function loads GNearClippingPlane right before
// storing it into M[3][2]: movss xmm0,[rip+X]; cvtps2pd xmm0,xmm0; movsd [rdi+70h],xmm0
const float* FindNearPlane(uintptr_t projection_fn) {
    static const unsigned char tail[] = {0x0F, 0x5A, 0xC0, 0xF2, 0x0F, 0x11, 0x47, 0x70};
    for (uintptr_t p = projection_fn; p < projection_fn + 0x100; ++p) {
        unsigned char b[16];
        if (!mem::Read(p, b)) return nullptr;
        if (b[0] == 0xF3 && b[1] == 0x0F && b[2] == 0x10 && b[3] == 0x05 && std::memcmp(b + 8, tail, sizeof(tail)) == 0) {
            int32_t disp;
            std::memcpy(&disp, b + 4, 4);
            return reinterpret_cast<const float*>(p + 8 + disp);
        }
    }
    return nullptr;
}

bool FrameViews(bool new_sample, xr::EyeView out[2]) {
    std::lock_guard<std::mutex> lock(g_frame_mutex);
    if (new_sample || !g_have_frame_views) {
        if (!xr::SessionRunning() || !xr::ViewsForNewPicture(g_frame_views)) return false;
        g_have_frame_views = true;
    }
    out[0] = g_frame_views[0];
    out[1] = g_frame_views[1];
    return true;
}

// The tracking-space -> game-world mapping used for the latest left eye, so
// hands and held objects land exactly where the eyes say they are.
struct WorldMapping {
    bool valid = false;
    FVector camera_location;  // camera the views are drawn from, before the headset offset (SettleView)
    FQuat base;               // game yaw (or full rotation with use_game_pitch)
    FQuat recenter;
    FVector origin_m;
    double world_to_meters = 100.0;
    double height_drop_m = 0.0;  // fixed_height: real head height above the recentre point, removed from everything
    FVector head_offset_cm;  // midpoint of the eyes from the game camera, world axes, horizontal
    double raise_cm = 0.0;   // SetViewRaiseCm: added to the eyes' (and hands') height
};
std::atomic<double> g_view_raise_cm{0.0};
std::mutex g_world_mutex;
WorldMapping g_world;
// Origin shifts requested by ShiftOriginWorld, applied at the next left-eye
// request so both eyes of a frame use the same origin. Guarded by g_world_mutex.
FVector g_pending_origin_m;
FVector g_pending_world_cm;

// The camera's expected move during the player's tick (view_lead_math.h).
// Guarded by g_world_mutex.
constexpr double kMaxLeadCm = 120.0;  // more than this in one frame is a teleport, not walking
view_lead::Anchors g_anchor_end;      // after the latest tick's listeners
view_lead::Anchors g_anchor_view;     // g_anchor_end when the latest left eye was set up
view_lead::Leads g_leads;             // from the latest tick start since those views
bool g_leads_set = false;
int g_lead_used = -1;
int g_lead_pick = -1;  // the anchor the lead would use, applied or not (g_lead_trust)
FVector g_lead_cm;  // added to the camera by TrackingToWorld and GameCameraLocation
// The lead is stopped while its prediction is far off the game's view (the
// view not following the character: the extraction hole's fall).
view_lead::LeadTrust g_lead_trust;
constexpr double kTrustAgainCm = 10.0;  // a measured frame this close starts the lead again
// The views are drawn from where the mod placed the hands for, eased toward
// the game's camera (view_lead::SettleView). g_world.camera_location is that
// drawn camera; g_game_cam_prev is the game's own camera at the same views.
constexpr double kSnapCm = 30.0;
FVector g_game_cam_prev;
bool g_have_game_cam = false;
// How far off the hands would have been, for the log. Leaf lock: the report
// line takes it and nothing else is taken while it is held.
std::mutex g_lead_stats_mutex;
view_lead::Stats g_lead_stats;
view_lead::GunShake g_gun_shake;   // also under g_lead_stats_mutex
view_lead::SettleStats g_settle_stats;  // also under g_lead_stats_mutex
view_lead::LeadTrust g_trust_log;       // totals only, also under g_lead_stats_mutex
bool g_moving_this_view = false;   // the camera moved between the latest two views (view thread)
FVector g_view_shift;              // this frame's views: drawn camera - game camera (view thread)

// view_follow in hole-vr.ini: how much of the game camera's unforeseen
// move the views take each frame (view_lead::SettleView). 1 = the game's
// camera exactly (hands and gun jump when it moves suddenly).
double ViewFollow() { return std::clamp(static_cast<double>(config::GetFloat("view_follow", 0.15f)), 0.05, 1.0); }

// The lead stopped or started again (view_lead::LeadTrust). View thread,
// no lock held. The first 40 changes of a run are logged one by one; the
// 10 s line counts them all.
void LogTrustChange(int change, const FVector& actual, const view_lead::Leads& l, int pick) {
    static int s_logged = 0;
    if (s_logged >= 40) return;
    ++s_logged;
    const double predicted = pick >= 0 && pick < view_lead::kKinds ? view_lead::Length(l.move[pick]) : 0.0;
    if (change < 0)
        log::Info("view lead: stopped; the game's view moved %.1f cm but the character's %s said %.1f cm (the view "
                  "isn't following the character); hands go with the game's view until they agree again",
                  view_lead::Length(actual), pick >= 0 ? view_lead::kKindNames[pick] : "anchor", predicted);
    else
        log::Info("view lead: on again; the game's view moved %.1f cm and the character's %s said %.1f cm",
                  view_lead::Length(actual), pick >= 0 ? view_lead::kKindNames[pick] : "anchor", predicted);
}

// The 10 s report line: how far the hands would have trailed the view, and
// how far off each anchor's prediction was.
std::string LeadReport(double) {
    view_lead::Stats s;
    view_lead::SettleStats settle;
    view_lead::LeadTrust trust;
    {
        std::lock_guard<std::mutex> lock(g_lead_stats_mutex);
        s = g_lead_stats;
        g_lead_stats = {};
        settle = g_settle_stats;
        g_settle_stats = {};
        trust = g_trust_log;
        g_trust_log = {};
    }
    if (s.frames == 0 && s.turn_frames == 0) return {};
    const int choice = static_cast<int>(config::GetFloat("view_lead", -1.f));
    std::string each;
    for (int k = 0; k < view_lead::kKinds; ++k) {
        if (!s.n[k]) continue;
        char b[64];
        std::snprintf(b, sizeof(b), "%s%s %.1f", each.empty() ? "" : ", ", view_lead::kKindNames[k],
                      s.sum[k] / s.n[k]);
        each += b;
    }
    const long long f = s.frames > 0 ? s.frames : 1;
    const long long tf = s.turn_frames > 0 ? s.turn_frames : 1;
    // Where the misses go (forward, sideways, up) and the worst frames.
    std::string worst;
    for (int i = 0; i < s.worst_count; ++i) {
        const view_lead::Stats::Worst& w = s.worst[i];
        char b[200];
        std::snprintf(b, sizeof(b), "%s%.1f cm (fwd %.1f side %.1f up %.1f; moved %.1f, predicted", i ? "; " : "",
                      w.cm, w.miss.X, w.miss.Y, w.miss.Z, view_lead::Length(w.actual));
        worst += b;
        for (int k = 0; k < view_lead::kKinds; ++k) {
            if (!w.leads.valid[k]) continue;
            std::snprintf(b, sizeof(b), " %s%s %.1f", view_lead::kKindNames[k], k == w.used ? "*" : "",
                          view_lead::Length(w.leads.move[k]));
            worst += b;
        }
        worst += ")";
    }
    // A hand at arm's length (about 45 cm out from the eyes) trails a turn
    // of a degrees by about 45 * a * pi / 180 cm.
    char line[640];
    std::snprintf(line, sizeof(line),
                  "frame timing: hands vs view while moving: %lld frames; uncorrected they would trail the view by "
                  "%.1f cm on average (worst %.1f), corrected %.1f cm (worst %.1f) with view_lead %d; by anchor: %s "
                  "cm; corrected miss forward %.1f, sideways %.1f, up %.1f cm. Turning: %lld frames, %.2f deg a frame "
                  "on average (worst %.1f), about %.1f cm at arm's length",
                  s.frames, s.none_sum / f, s.none_max, s.used_sum / f, s.used_max, choice,
                  each.empty() ? "none" : each.c_str(), s.axis_sum[0] / f, s.axis_sum[1] / f, s.axis_sum[2] / f,
                  s.turn_frames, s.turn_sum / tf, s.turn_max, 45.0 * (s.turn_sum / tf) * kDegToRad);
    std::string out = line;
    if (!worst.empty()) out += ". Worst frames: " + worst;
    if (settle.frames > 0) {
        const long long held = settle.frames - settle.snaps;
        const long long h = held > 0 ? held : 1;
        char b[400];
        std::snprintf(b, sizeof(b),
                      ". Views drawn with the hands (view_follow %.2f): what the mod placed is off them by %.2f cm on "
                      "average (worst %.1f), the views are %.2f cm from the game's camera on average (worst %.1f); "
                      "%lld of %lld frames drawn at the game's camera (no tick or a jump)",
                      ViewFollow(), settle.mismatch_sum / h, settle.mismatch_max, settle.off_sum / h, settle.off_max,
                      settle.snaps, settle.frames);
        out += b;
    }
    if (trust.stops > 0 || trust.off_frames > 0) {
        char b[260];
        std::snprintf(b, sizeof(b),
                      ". Lead stopped %d times (worst miss %.1f cm: the game's view not following the character), "
                      "off for %lld frames",
                      trust.stops, trust.worst_miss, trust.off_frames);
        out += b;
    }
    return out;
}

// The 10 s report line: how much the held gun shakes as the eyes see it.
std::string GunReport(double) {
    view_lead::GunShake g;
    {
        std::lock_guard<std::mutex> lock(g_lead_stats_mutex);
        g = g_gun_shake;
        g_gun_shake.ResetTotals();
    }
    if (g.n[0] + g.n[1] == 0 && g.checked_frames == 0) return {};
    auto avg = [](double sum, long long n) { return n > 0 ? sum / n : 0.0; };
    char b[700];
    std::snprintf(b, sizeof(b),
                  "frame timing: the gun as seen from the eyes shakes %.2f cm / %.2f deg a frame while moving "
                  "(worst %.1f cm / %.1f deg, %lld frames) and %.2f cm / %.2f deg standing still (%lld frames); "
                  "against the controller holding it (the mod's own part) it wobbles %.2f cm / %.2f deg a frame "
                  "while moving (worst %.1f cm / %.1f deg, %lld frames) and %.2f cm / %.2f deg still (%lld frames); "
                  "something else moved it after the mod placed it in %lld of %lld frames (worst %.1f cm)",
                  avg(g.cm_sum[1], g.n[1]), avg(g.deg_sum[1], g.n[1]), g.cm_max[1], g.deg_max[1], g.n[1],
                  avg(g.cm_sum[0], g.n[0]), avg(g.deg_sum[0], g.n[0]), g.n[0],
                  avg(g.place_cm_sum[1], g.place_n[1]), avg(g.place_deg_sum[1], g.place_n[1]), g.place_cm_max[1],
                  g.place_deg_max[1], g.place_n[1], avg(g.place_cm_sum[0], g.place_n[0]),
                  avg(g.place_deg_sum[0], g.place_n[0]), g.place_n[0], g.moved_frames, g.checked_frames,
                  g.moved_max);
    return b;
}

std::atomic<void (*)()> g_frame_callback{nullptr};
std::atomic<DWORD> g_view_thread{0};

// Game thread frames: time between one frame's eye views and the next's.
perf::Counter g_perf_gt_frame{"game thread frame"};
perf::Counter g_perf_after_eyes{"after-eyes callback (hands)"};

// After the right eye: the frame's head pose and world mapping are final.
void RunFrameCallback() {
    static double s_last = 0.0;
    static bool s_registered = false;
    double now = perf::NowMs();
    // Only the thread that set up the views (the game thread) runs it.
    DWORD me = GetCurrentThreadId();
    DWORD expected = 0;
    if (!g_view_thread.compare_exchange_strong(expected, me) && expected != me) return;
    if (!s_registered) {
        s_registered = true;
        perf::Register(&g_perf_gt_frame);
        perf::Register(&g_perf_after_eyes);
        perf::AddReportLine(&LeadReport);
        perf::AddReportLine(&GunReport);
        ue::process_event::SetGameThread(me);
    }
    if (s_last > 0.0 && now - s_last < 1000.0) g_perf_gt_frame.Add(now - s_last);
    s_last = now;
    auto fn = g_frame_callback.load();
    if (!fn) return;
    static bool running = false;
    if (running) return;
    running = true;
    {
        perf::Scope t(g_perf_after_eyes);
        fn();
    }
    running = false;
}

// world_scale: how big the world looks. 1 = true size; 1.2 = the world looks
// 20% bigger (you feel smaller). Done by scaling the tracking-to-world factor,
// which scales the eye spacing, head movement and hands together.
double WorldScale() {
    double s = config::GetFloat("world_scale", 1.f);
    if (!(s > 0)) s = 1.0;
    return std::clamp(s, 0.5, 2.0);
}

void HookCalcOffset(void* self, int32_t view_index, FRotator* rotation, float engine_world_to_meters,
                    FVector* location) {
    HOLEVR_FIRST_CALL("CalculateStereoViewOffset");
    struct AfterRightEye {
        int32_t i;
        ~AfterRightEye() {
            if (i == 1) RunFrameCallback();
        }
    } after{view_index};
    xr::EyeView views[2];
    // A left-eye request starts a new frame: take a fresh tracking sample.
    if (view_index < 0 || view_index > 1 || !FrameViews(view_index == 0, views)) {
        if (view_index == 0) g_view_shift = FVector{};
        o_calc_offset(self, view_index, rotation, engine_world_to_meters, location);
        return;
    }
    const double world_to_meters = engine_world_to_meters / WorldScale();
    if (view_index == 1) xr::SetRenderedViews(views);
    if (view_index == 0) {
        view_lead::Leads leads;
        int used = -1;
        FVector actual;
        bool measured = false;
        double turn = 0;
        view_lead::Settle settle;
        settle.view = *location;
        bool have_settle = false;
        int trust_changed = 0;  // 1 the lead started again, -1 it stopped
        int pick = -1;
        view_lead::LeadTrust trust;  // this frame's totals, for the log
        {
            std::lock_guard<std::mutex> lock(g_world_mutex);
            static double s_last_yaw = 0;
            turn = NormalizeAxis(rotation->Yaw - s_last_yaw);
            s_last_yaw = rotation->Yaw;
            // How the game's camera really moved since the latest views, less
            // the room-scale steps (already taken off the tracking origin):
            // what the tick's prediction should have said.
            if (g_leads_set && g_world.valid && g_have_game_cam) {
                measured = true;
                leads = g_leads;
                used = g_lead_used;
                pick = g_lead_pick;
                actual = *location - g_game_cam_prev - g_pending_world_cm;
            }
            // Draw the views from where this tick placed the hands for, eased
            // toward the game's camera.
            if (g_world.valid) {
                const FVector placed_for = g_world.camera_location + g_pending_world_cm + g_lead_cm;
                settle = view_lead::SettleView(*location, placed_for, ViewFollow(), g_leads_set && g_lead_used >= 0,
                                               kSnapCm);
                have_settle = true;
            }
            // Judge the lead by this frame, whether or not it was applied,
            // for the next tick (view_lead::LeadTrust).
            if (measured) {
                const bool was = g_lead_trust.trusted;
                g_lead_trust.Update(actual, leads, pick, kSnapCm, kTrustAgainCm);
                if (was != g_lead_trust.trusted) trust_changed = g_lead_trust.trusted ? 1 : -1;
                trust = g_lead_trust;
                g_lead_trust.ResetTotals();
            }
            g_game_cam_prev = *location;
            g_have_game_cam = true;
            g_origin_m = g_origin_m + g_pending_origin_m;
            g_pending_origin_m = FVector{};
            g_pending_world_cm = FVector{};
            // From here the camera is where the character is: no lead until
            // the next tick moves it again.
            g_anchor_view = g_anchor_end;
            g_leads = {};
            g_leads_set = false;
            g_lead_used = -1;
            g_lead_pick = -1;
            g_lead_cm = FVector{};
        }
        if (trust_changed) LogTrustChange(trust_changed, actual, leads, pick);
        g_moving_this_view = measured && view_lead::Length(actual) > 0.5;
        g_view_shift = settle.view - *location;
        if (measured || have_settle) {
            std::lock_guard<std::mutex> lock(g_lead_stats_mutex);
            if (measured) {
                g_lead_stats.Add(actual, leads, used, rotation->Yaw);
                g_lead_stats.AddTurn(turn);
                g_trust_log.off_frames += trust.off_frames;
                g_trust_log.stops += trust.stops;
                if (trust.worst_miss > g_trust_log.worst_miss) g_trust_log.worst_miss = trust.worst_miss;
            }
            if (have_settle) g_settle_stats.Add(settle);
        }
    }

    const bool recentered_now = g_recenter.exchange(false);
    if (recentered_now) {
        const XrVector3f& a = views[0].pose.position;
        const XrVector3f& b = views[1].pose.position;
        g_origin_m = XrToUePosition((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f, 1.0);
        const XrQuaternionf& q = views[0].pose.orientation;
        g_origin_yaw = YawOf(XrToUeQuat(q.x, q.y, q.z, q.w));
        log::Info("stereo: recentered (yaw %.1f)", g_origin_yaw);
    }
    const bool standing_requested = g_standing_apply.exchange(false);
    if (recentered_now || standing_requested) {
        const double y = g_standing_eye_m.load();
        if (y > 0.5 && y < 2.5 && xr::StandingSpace()) {
            const double before = g_origin_m.Z;
            g_origin_m.Z = XrToUePosition(0, static_cast<float>(y), 0, 1.0).Z;
            log::Info("stereo: standing eye height %.0f cm from your measurement (the head was %.0f cm %s it at the "
                      "recentre)", y * 100.0, std::fabs(before - g_origin_m.Z) * 100.0,
                      before >= g_origin_m.Z ? "above" : "below");
        } else if (standing_requested) {
            const double head_y = (static_cast<double>(views[0].pose.position.y) + views[1].pose.position.y) * 0.5;
            // View caches can retain the last valid pose during tracking loss.
            // A clear must wait for a new observation rather than reusing it.
            const bool fresh = y > 0 || (views[0].base_time > g_standing_clear_after.load() &&
                                        views[1].base_time > g_standing_clear_after.load());
            if (fresh && std::isfinite(head_y)) {
                const double before = g_origin_m.Z;
                g_origin_m.Z = head_y;  // UE tracking Z equals OpenXR Y, in metres
                if (std::fabs(before - head_y) > 1e-6) ++g_origin_generation;
                log::Info("stereo: standing reference inactive; vertical origin reset to current head %.1f cm (XY/yaw kept)",
                          head_y * 100.0);
            } else {
                g_standing_apply = true;  // retry at the next fresh valid picture
            }
        }
    }

    const XrPosef& pose = views[view_index].pose;
    FQuat eye_q = XrToUeQuat(pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w);
    FVector eye_p = XrToUePosition(pose.position.x, pose.position.y, pose.position.z, 1.0);
    FQuat recenter = YawOnly(-g_origin_yaw);
    FQuat local_q = recenter * eye_q;
    FVector local_p = recenter.Rotate(eye_p - g_origin_m) * world_to_meters;

    // Fixed height: the eyes stay at the game camera's height whether the
    // player sits, stands or crouches. The same vertical shift is applied to
    // the hands (TrackingToWorld) so they stay put relative to the head.
    double height_drop_m = 0.0;
    if (config::GetBool("fixed_height", true)) {
        const XrVector3f& a = views[0].pose.position;
        const XrVector3f& b = views[1].pose.position;
        height_drop_m = XrToUePosition(0, (a.y + b.y) * 0.5f, 0, 1.0).Z - g_origin_m.Z;
        local_p.Z -= height_drop_m * world_to_meters;
    }
    const double raise_cm = g_view_raise_cm.load();
    local_p.Z += raise_cm;

    // The game keeps steering yaw (mouse/stick turning); the headset owns
    // pitch and roll so the horizon never moves on its own.
    FQuat base = config::GetBool("use_game_pitch", false) ? ToQuat(*rotation) : YawOnly(rotation->Yaw);
    if (view_index == 0) {
        std::lock_guard<std::mutex> lock(g_world_mutex);
        const XrVector3f& a = views[0].pose.position;
        const XrVector3f& b = views[1].pose.position;
        FVector head_p = XrToUePosition((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f, 1.0);
        FVector head_offset = base.Rotate(recenter.Rotate(head_p - g_origin_m) * world_to_meters);
        head_offset.Z = 0;
        const FVector drawn_cam = *location + g_view_shift;
        g_world = {true, drawn_cam, base, FQuat{recenter.X, recenter.Y, recenter.Z, recenter.W}, g_origin_m,
                   world_to_meters, height_drop_m, head_offset, raise_cm};
        // The held gun as this picture's eyes see it (eye-yaw axes), for the
        // shake line in the log. g_anchor_view is this frame's tick end.
        FVector eye_local = recenter.Rotate(head_p - g_origin_m) * world_to_meters;
        eye_local.Z += raise_cm - height_drop_m * world_to_meters;
        const FVector eye = drawn_cam + base.Rotate(eye_local);
        const bool held = g_anchor_view.held != 0;
        const FQuat inv = base.Inverse();
        const FVector gun_at = held ? inv.Rotate(g_anchor_view.held_at - eye) : FVector{};
        const FQuat gun_rot = held ? inv * g_anchor_view.held_rot : FQuat{};
        // The gun in the controller's own axes, the controller as these eyes
        // see it (same tracking frame as the tick that placed the gun). The
        // origin, fixed height and raise cancel between eyes and hand.
        const bool grip = held && g_anchor_view.grip;
        FVector in_hand;
        FQuat in_hand_rot;
        if (grip) {
            const FQuat hand_rot = recenter * g_anchor_view.grip_rot;
            const FVector hand_at = recenter.Rotate(g_anchor_view.grip_m - head_p) * world_to_meters;
            const FQuat hand_inv = hand_rot.Inverse();
            in_hand = hand_inv.Rotate(gun_at - hand_at);
            in_hand_rot = hand_inv * gun_rot;
        }
        std::lock_guard<std::mutex> stats(g_lead_stats_mutex);  // leaf, taken inside g_world_mutex only here
        g_gun_shake.Add(held, gun_at, gun_rot, g_moving_this_view);
        g_gun_shake.AddPlacement(grip, in_hand, in_hand_rot, g_moving_this_view);
    }
    *rotation = ToRotator(base * local_q);
    *location = *location + g_view_shift + base.Rotate(local_p);
}

double* HookProjection(void* self, double* out, int32_t view_index) {
    HOLEVR_FIRST_CALL("GetStereoProjectionMatrix");
    xr::EyeView views[2];
    if (view_index < 0 || view_index > 1 || !FrameViews(false, views))
        return o_projection(self, out, view_index);
    const XrFovf& fov = views[view_index].fov;
    double tan_l = std::tan(fov.angleLeft), tan_r = std::tan(fov.angleRight);
    double tan_u = std::tan(fov.angleUp), tan_d = std::tan(fov.angleDown);
    double inv_rl = 1.0 / (tan_r - tan_l), inv_tb = 1.0 / (tan_u - tan_d);
    double z_near = g_near_plane ? *g_near_plane : 10.0;
    // Reversed-Z infinite projection, as Epic's OpenXR plugin builds it.
    double m[16] = {2.0 * inv_rl, 0, 0, 0,
                    0, 2.0 * inv_tb, 0, 0,
                    (tan_r + tan_l) * -inv_rl, (tan_u + tan_d) * -inv_tb, 0, 1,
                    0, 0, z_near, 0};
    std::memcpy(out, m, sizeof(m));
    return out;
}

// --- finding the D3D12 texture behind the engine's FRHITexture ------------

std::vector<uintptr_t> g_resource_vtables;
void* g_last_src = nullptr;
ID3D12Resource* g_last_resource = nullptr;

void LearnResourceVtables() {
    if (!g_resource_vtables.empty()) return;
    ID3D12CommandQueue* queue = dxgi_hooks::GameQueue();
    ID3D12Device* device = nullptr;
    if (!queue || FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))) return;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC tex{};
    tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex.Width = 64;
    tex.Height = 64;
    tex.DepthOrArraySize = 1;
    tex.MipLevels = 1;
    tex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex.SampleDesc.Count = 1;
    tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_RESOURCE_DESC buf{};
    buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf.Width = 256;
    buf.Height = 1;
    buf.DepthOrArraySize = 1;
    buf.MipLevels = 1;
    buf.SampleDesc.Count = 1;
    buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (const D3D12_RESOURCE_DESC* d : {&tex, &buf}) {
        ID3D12Resource* r = nullptr;
        if (SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, d, D3D12_RESOURCE_STATE_COMMON,
                                                      nullptr, IID_PPV_ARGS(&r)))) {
            g_resource_vtables.push_back(*reinterpret_cast<uintptr_t*>(r));
            r->Release();
        }
    }
    device->Release();
}

bool IsD3D12Resource(uintptr_t p) {
    if (!mem::LooksLikePointer(p)) return false;
    uintptr_t vt;
    if (!mem::Read(p, vt)) return false;
    for (uintptr_t known : g_resource_vtables)
        if (vt == known) return true;
    return false;
}

// The engine hands us an RDG or RHI texture wrapper. The ID3D12Resource sits a
// few pointers below it (FRDGTexture -> pooled RHI texture -> FD3D12Resource ->
// ID3D12Resource), at offsets that differ between engine builds. Search up to
// three levels for an object with the D3D12 runtime's resource vtable that is
// a side-by-side 2D texture.
ID3D12Resource* SearchResource(uintptr_t obj, int depth, std::vector<uintptr_t>& path) {
    const uintptr_t span = depth == 0 ? 0x200 : 0x100;
    for (uintptr_t off = 0; off < span; off += 8) {
        uintptr_t p = mem::ReadOr<uintptr_t>(obj + off);
        if (!mem::LooksLikePointer(p) || p == obj) continue;
        path.push_back(off);
        if (IsD3D12Resource(p)) {
            auto* res = reinterpret_cast<ID3D12Resource*>(p);
            D3D12_RESOURCE_DESC d = res->GetDesc();
            if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width >= d.Height) return res;
        } else if (depth < 2) {
            if (ID3D12Resource* r = SearchResource(p, depth + 1, path)) return r;
        }
        path.pop_back();
    }
    return nullptr;
}

ID3D12Resource* FindNativeTexture(void* engine_texture) {
    std::vector<uintptr_t> path;
    ID3D12Resource* res = SearchResource(reinterpret_cast<uintptr_t>(engine_texture), 0, path);
    if (!res) return nullptr;
    D3D12_RESOURCE_DESC d = res->GetDesc();
    std::string chain;
    char hex[24];
    for (uintptr_t o : path) {
        snprintf(hex, sizeof(hex), " +0x%llx", static_cast<unsigned long long>(o));
        chain += hex;
    }
    log::Info("stereo: eye texture %llux%u format %d (pointer path%s)", static_cast<unsigned long long>(d.Width),
              d.Height, static_cast<int>(d.Format), chain.c_str());
    return res;
}

void HookRenderTexture(void* self, void* cmdlist, void* back_buffer, void* src_texture, void* window_size) {
    HOLEVR_FIRST_CALL("RenderTexture_RenderThread");
    if (!src_texture || !back_buffer) {
        // While the viewport switches to its separate eye texture there are
        // frames with no texture yet. The engine's own mirror pass reads
        // through these unchecked (the 0xe2 crash), so skip it this frame.
        if (g_null_mirror_frames.fetch_add(1) == 0)
            log::Info("stereo: no eye texture yet (eye %p, window %p); skipping the mirror until it exists",
                      src_texture, back_buffer);
        xr::SetEyeTexture(nullptr);
        return;
    }
    if (src_texture && src_texture != g_last_src) {
        LearnResourceVtables();
        g_last_resource = FindNativeTexture(src_texture);
        g_last_src = src_texture;
        if (!g_last_resource) log::Warn("stereo: could not find the D3D12 texture for the eye image");
    }
    xr::SetEyeTexture(g_last_resource);
    if (config::GetBool("desktop_mirror", true))
        o_render_texture(self, cmdlist, back_buffer, src_texture, window_size);
}

// Secondary-interface methods receive this = device + 8.
void HookCalcRTSize(void* self, const void* viewport, uint32_t* x, uint32_t* y) {
    HOLEVR_FIRST_CALL("CalculateRenderTargetSize");
    if (g_eye_w && g_eye_h) {
        *x = g_eye_w * 2;
        *y = g_eye_h;
        g_realloc = false;
        return;
    }
    o_calc_rt_size(self, viewport, x, y);
}

// A separate eye texture at headset resolution, instead of drawing both eyes
// into the game window at monitor resolution.
bool HookShouldUseSeparateRT(void* self) {
    HOLEVR_FIRST_CALL("ShouldUseSeparateRenderTarget");
    if (g_separate_rt.load()) return true;
    return o_should_use_separate_rt(self);
}

bool HookNeedRealloc(void* self, const void* viewport) {
    HOLEVR_FIRST_CALL("NeedReAllocateViewportRenderTarget");
    return g_realloc.load() || o_need_realloc(self, viewport);
}

}  // namespace

void InstallCommandLineHook() {
    if (!config::GetBool("vr_enabled", true)) return;
    std::wstring original = GetCommandLineW();
    if (original.find(L"-emulatestereo") != std::wstring::npos) return;
    g_cmdline = original + L" -emulatestereo";
    o_get_command_line_w = reinterpret_cast<GetCommandLineWFn>(
        hooks::PatchExeImport("KERNEL32.dll", "GetCommandLineW", reinterpret_cast<void*>(&HookGetCommandLineW)));
    log::Info(o_get_command_line_w ? "Added -emulatestereo to the game's command line"
                                   : "Could not hook GetCommandLineW; add -emulatestereo to the launch options");
}

bool Install() {
    if (g_device) return true;
    ue::UObject engine = FindGameEngine();
    if (!engine) return false;
    uintptr_t dev = FindDevice(engine);
    if (!dev) {
        log::Error("stereo: the engine's emulated stereo device is missing (was -emulatestereo applied?)");
        return false;
    }
    uintptr_t vt1 = mem::ReadOr<uintptr_t>(dev);
    uintptr_t vt2 = mem::ReadOr<uintptr_t>(dev + 8);
    std::memcpy(g_primary, reinterpret_cast<void*>(vt1), sizeof(g_primary));
    std::memcpy(g_secondary, reinterpret_cast<void*>(vt2), sizeof(g_secondary));

    o_calc_offset = reinterpret_cast<CalcOffsetFn>(g_primary[kCalculateStereoViewOffset]);
    o_projection = reinterpret_cast<ProjectionFn>(g_primary[kGetStereoProjectionMatrix]);
    o_render_texture = reinterpret_cast<RenderTextureFn>(g_primary[kRenderTextureRenderThread]);
    o_calc_rt_size = reinterpret_cast<CalcRTSizeFn>(g_secondary[kCalculateRenderTargetSize]);
    o_need_realloc = reinterpret_cast<NeedReallocFn>(g_secondary[kNeedReAllocateViewportRenderTarget]);
    o_should_use_separate_rt = reinterpret_cast<ShouldUseSeparateRTFn>(g_secondary[kShouldUseSeparateRenderTarget]);
    g_near_plane = FindNearPlane(reinterpret_cast<uintptr_t>(o_projection));
    if (!g_near_plane) log::Warn("stereo: near clip plane not found, using 10 cm");

    g_primary[kCalculateStereoViewOffset] = reinterpret_cast<void*>(&HookCalcOffset);
    g_primary[kGetStereoProjectionMatrix] = reinterpret_cast<void*>(&HookProjection);
    g_primary[kRenderTextureRenderThread] = reinterpret_cast<void*>(&HookRenderTexture);
    g_secondary[kCalculateRenderTargetSize] = reinterpret_cast<void*>(&HookCalcRTSize);
    g_secondary[kNeedReAllocateViewportRenderTarget] = reinterpret_cast<void*>(&HookNeedRealloc);
    g_secondary[kShouldUseSeparateRenderTarget] = reinterpret_cast<void*>(&HookShouldUseSeparateRT);

    xr::RecommendedEyeSize(&g_eye_w, &g_eye_h);
    if (g_eye_w && g_eye_h) {
        *reinterpret_cast<int32_t*>(dev + kWidth) = static_cast<int32_t>(g_eye_w * 2);
        *reinterpret_cast<int32_t*>(dev + kHeight) = static_cast<int32_t>(g_eye_h);
        g_realloc = true;
    }
    // Swap this one object's vtables; the engine's shared tables stay untouched.
    *reinterpret_cast<void* volatile*>(dev) = g_primary;
    *reinterpret_cast<void* volatile*>(dev + 8) = g_secondary;
    g_device = dev;
    log::Info("stereo: hooked (render target %ux%u, near plane %.1f)", g_eye_w * 2, g_eye_h,
              g_near_plane ? *g_near_plane : 10.0f);
    if (g_eye_w && config::GetBool("headset_resolution", false)) {
        // Switch the viewport to a separate eye texture. It asks for its
        // render target again when NeedReAllocate says so (g_realloc); if it
        // still has none after a few seconds, a window resize forces it.
        g_separate_rt = true;
        log::Info("stereo: asking the engine for a %ux%u eye texture (headset_resolution=1 in the ini; experimental)",
                  g_eye_w * 2, g_eye_h);
        if (HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
                Sleep(4000);
                if (!g_last_resource) {
                    log::Info("stereo: still no eye texture after 4 s (%d frames without one)",
                              g_null_mirror_frames.load());
                    dxgi_hooks::NudgeWindow();
                    Sleep(4000);
                }
                log::Info("stereo: eye texture %s (%d frames without one so far)",
                          g_last_resource ? "in use" : "STILL MISSING", g_null_mirror_frames.load());
                return 0;
            }, nullptr, 0, nullptr))
            CloseHandle(t);
    }
    return true;
}

void Recenter() { ++g_origin_generation; g_recenter = true; }
std::uint64_t OriginGeneration() { return g_origin_generation.load(); }
double TrackingScale() {
    std::lock_guard<std::mutex> lock(g_world_mutex);
    return g_world.valid && g_world.world_to_meters > 0 ? g_world.world_to_meters / 100.0 : 1.0;
}
void SetStandingEyeHeight(double floor_to_eye_m) {
    if (!std::isfinite(floor_to_eye_m) ||
        (floor_to_eye_m != 0 && !(floor_to_eye_m > 0.5 && floor_to_eye_m < 2.5))) return;
    if (floor_to_eye_m == 0) {
        xr::EyeView views[2];
        g_standing_clear_after = xr::LatestViews(views) ? std::max(views[0].base_time, views[1].base_time) : 0;
    }
    g_standing_eye_m = floor_to_eye_m;
    g_standing_apply = true;
}

void SetGameFrameCallback(void (*fn)()) { g_frame_callback.store(fn); }

bool TrackingToWorld(const XrPosef& pose, FVector* world_location, FQuat* world_rotation) {
    WorldMapping m;
    FVector lead;
    {
        std::lock_guard<std::mutex> lock(g_world_mutex);
        m = g_world;
        lead = g_lead_cm;
    }
    if (!m.valid) return false;
    FQuat q = XrToUeQuat(pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w);
    FVector p = XrToUePosition(pose.position.x, pose.position.y, pose.position.z, 1.0);
    FVector local_p = m.recenter.Rotate(p - m.origin_m) * m.world_to_meters;
    // Fixed height takes the head's own height off everything. Use the head
    // of the picture these poses are for, not the latest frame's: the hands
    // are for that picture's moment, and with the latest frame's head every
    // real crouch or head bob moved the gun up and down against the view by
    // a frame of head movement (more when frame times vary).
    double drop_m = m.height_drop_m;
    if (m.height_drop_m != 0.0) {
        xr::EyeView v[2];
        if (xr::ViewsForNewPicture(v))
            drop_m = XrToUePosition(0, (v[0].pose.position.y + v[1].pose.position.y) * 0.5f, 0, 1.0).Z - m.origin_m.Z;
    }
    local_p.Z -= drop_m * m.world_to_meters;
    local_p.Z += m.raise_cm;
    if (world_location) *world_location = m.camera_location + lead + m.base.Rotate(local_p);
    if (world_rotation) *world_rotation = m.base * (m.recenter * q);
    return true;
}

bool HeadOffsetWorld(FVector* offset_cm) {
    std::lock_guard<std::mutex> lock(g_world_mutex);
    if (!g_world.valid) return false;
    // Include shifts already requested but not yet applied to a frame, so a
    // caller that moves the pawn every tick never counts a step twice.
    *offset_cm = g_world.head_offset_cm - g_pending_world_cm;
    offset_cm->Z = 0;
    return true;
}

bool ShiftOriginWorld(const FVector& world_delta_cm) {
    std::lock_guard<std::mutex> lock(g_world_mutex);
    if (!g_world.valid || g_world.world_to_meters <= 0) return false;
    FVector d = world_delta_cm;
    d.Z = 0;
    // world offset = base * recenter * (head - origin) * w2m, so moving the
    // origin by recenter^-1 * base^-1 * d / w2m moves the view back by d.
    FVector local = g_world.recenter.Inverse().Rotate(g_world.base.Inverse().Rotate(d));
    g_pending_origin_m = g_pending_origin_m + local * (1.0 / g_world.world_to_meters);
    g_pending_world_cm = g_pending_world_cm + d;
    return true;
}

bool GameCameraLocation(FVector* world_cm) {
    std::lock_guard<std::mutex> lock(g_world_mutex);
    if (!g_world.valid) return false;
    *world_cm = g_world.camera_location + g_lead_cm;
    return true;
}

void CharacterAtTickStart(const view_lead::Anchors& now) {
    const int choice = static_cast<int>(config::GetFloat("view_lead", -1.f));
    std::lock_guard<std::mutex> lock(g_world_mutex);
    if (now.held && now.held == g_anchor_end.held) {
        std::lock_guard<std::mutex> stats(g_lead_stats_mutex);
        g_gun_shake.AddMovedAfter(view_lead::Length(now.held_at - g_anchor_end.held_at));
    }
    if (!g_world.valid) return;
    g_leads = view_lead::Predict(g_anchor_view, now, g_world.camera_location, kMaxLeadCm);
    g_leads_set = true;
    g_lead_pick = view_lead::Pick(g_leads, choice);
    g_lead_used = g_lead_trust.trusted ? g_lead_pick : -1;
    g_lead_cm = g_lead_used >= 0 ? g_leads.move[g_lead_used] : FVector{};
}

void CharacterAtTickEnd(const view_lead::Anchors& now) {
    std::lock_guard<std::mutex> lock(g_world_mutex);
    g_anchor_end = now;
}

void SetViewRaiseCm(double cm) { g_view_raise_cm.store(cm); }

}  // namespace holevr::vr::stereo
