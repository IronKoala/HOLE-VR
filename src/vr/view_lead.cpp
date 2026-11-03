#include "vr/view_lead.h"

#include "core/log.h"
#include "core/perf.h"
#include "game/game.h"
#include "ue/call.h"
#include "vr/body/skeleton.h"
#include "vr/hands.h"
#include "vr/input.h"
#include "vr/math.h"
#include "vr/stereo.h"
#include "vr/view_lead_math.h"

namespace holevr::vr::view_lead {
namespace {

using ue::UObject;

perf::Counter g_perf{"view lead (where the character is)"};

FVector ComponentLocation(UObject comp, bool* ok) {
    ue::Call c(comp, "K2_GetComponentLocation");
    *ok = c.Invoke();
    return c.Result<FVector>();
}

Anchors Sample(UObject player) {
    perf::Scope t(g_perf);
    Anchors a;
    if (!player) return a;
    a.who = player;
    {
        ue::Call c(player, "K2_GetActorLocation");
        a.capsule = c.Invoke();
        a.capsule_at = c.Result<FVector>();
    }
    // HOLE's player camera: the active one when the game says which, else
    // the main one.
    UObject cam = 0;
    if (!(ue::GetProp(player, "ActiveMainCamera", cam) && cam)) ue::GetProp(player, "Camera_Main", cam);
    if (cam) a.camera_at = ComponentLocation(cam, &a.camera);
    UObject mesh = 0;
    if (ue::GetProp(player, "Mesh", mesh) && mesh) {
        ue::Call c(mesh, "K2_GetComponentToWorld");
        a.mesh = c.Invoke();
        const skel::Xf x = c.Result<skel::Xf>();
        a.mesh_at = x.translation;
        a.mesh_rot = x.rotation;
    }
    // The gun in the right hand, for the shake line in the log.
    const hands::HandGrab grab = hands::GetGrab(hands::kRight);
    if (grab.kind == hands::GrabKind::kGunGrip && grab.object) {
        ue::Call c(grab.object, "GetTransform");
        if (c.Invoke()) {
            const skel::Xf x = c.Result<skel::Xf>();
            a.held = grab.object;
            a.held_at = x.translation;
            a.held_rot = x.rotation;
            // The controller pose this tick's hands came from.
            const input::Snapshot s = input::LastConsumed();
            const input::HandInput& h = s.hand[input::kRight];
            if (s.valid && h.grip_valid) {
                a.grip = true;
                a.grip_m = XrToUePosition(h.grip.position.x, h.grip.position.y, h.grip.position.z, 1.0);
                a.grip_rot = XrToUeQuat(h.grip.orientation.x, h.grip.orientation.y, h.grip.orientation.z,
                                        h.grip.orientation.w);
            }
        }
    }
    static bool s_logged = false;
    if (!s_logged) {
        s_logged = true;
        log::Info("view lead: following the character by its camera (%s), body mesh (%s) and capsule (%s)",
                  a.camera ? "found" : "missing", a.mesh ? "found" : "missing", a.capsule ? "found" : "missing");
    }
    return a;
}

void OnTickBegin(UObject player, float) { stereo::CharacterAtTickStart(Sample(player)); }
void OnTickEnd(UObject player, float) { stereo::CharacterAtTickEnd(Sample(player)); }

}  // namespace

void Init() {
    perf::Register(&g_perf);
    game::SetTickBracket(&OnTickBegin, &OnTickEnd);
}

}  // namespace holevr::vr::view_lead
