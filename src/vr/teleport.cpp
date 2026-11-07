#include "vr/teleport.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/memory.h"
#include "game/gun.h"
#include "ue/call.h"
#include "ue/objects.h"
#include "vr/comfort.h"
#include "vr/hands.h"
#include "vr/math.h"
#include "vr/stereo.h"
#include "vr/teleport_math.h"

namespace holevr::vr::teleport {
namespace {
using ue::UObject;

// The throw: 8.5 m/s, normal gravity, sampled every 60 ms for 1.8 s. From
// waist height at 45 degrees that reaches about 8 m on flat ground.
constexpr double kSpeed = 850.0;
constexpr double kGravity = 980.0;
constexpr double kStep = 0.06;
constexpr int kPoints = 30;
constexpr double kMaxRiseCm = 150.0;   // highest step up you can teleport onto
constexpr double kBeadCm = 3.5;
constexpr double kDiscCm = 50.0;
constexpr double kBlinkS = 0.22;       // the view is black this long
constexpr double kMoveAfterS = 0.08;   // the move happens once it is dark

double NowS() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct alignas(16) Xf {  // FTransform (double): rotation, translation, scale, with padding
    FQuat rotation;
    FVector translation;
    double pad0 = 0;
    FVector scale{1, 1, 1};
    double pad1 = 0;
};
static_assert(sizeof(Xf) == 0x60, "FTransform layout");

struct TArrayRaw {
    const UObject* data;
    int32_t num, max;
};

struct State {
    bool tried = false;
    UObject mesh = 0;
    double mesh_size = 100.0;
    UObject ksl = 0, gps = 0;
    UObject owner = 0;
    UObject beads[kPoints] = {};
    UObject disc = 0;
    int owner_index = -1, disc_index = -1, bead0_index = -1;
    int beads_shown = 0;
    bool disc_shown = false;
    StickGesture gesture;
    // The spot under the arc this frame.
    bool have_target = false;
    FVector target;
    const char* why = "";
    // A teleport waiting for the blink to go dark.
    double move_at = -1;
    FVector move_to;
    double last_log = 0;
    // The landing's second footstep (the first plays with the move).
    double step_at = -1;
    int steps = 0;
} T;

// ---- the landing footsteps ----
// Owner 2026-11-07: "the payer needs to make a stepping sound when
// teleporting to the new location so the enemies can hear it, like with
// walking". Walking's footsteps are the game's Footstep_AnimNotify (from the
// locomotion system the player is built on), fired by the walk animation on
// the character Mesh. Its Received_Notify picks the sound for the surface
// under the foot, plays it, and reports the noise event enemies hear
// (AISense_Hearing ReportNoiseEvent, with the player's footstep volume
// skill), as the class dump of 2026-11-07 shows. So the landing plays the
// walk-forward animation's own two footstep notifies, left then right, on the
// player's Mesh at the new place: the same sound and the same noise as two
// steps of walking. Kept as ObjectRefs: they belong to animation assets.
constexpr double kSecondStepS = 0.28;
ue::ObjectRef g_walk_step[2] = {
    ue::ObjectRef("Footstep_AnimNotify_C /Game/AdvancedLocomotionV4/CharacterAssets/MannequinSkeleton/"
                  "AnimationExamples/Base/Locomotion/ALS_N_Walk_F.ALS_N_Walk_F:Footstep_Notify_C_0", 5.0),
    ue::ObjectRef("Footstep_AnimNotify_C /Game/AdvancedLocomotionV4/CharacterAssets/MannequinSkeleton/"
                  "AnimationExamples/Base/Locomotion/ALS_N_Walk_F.ALS_N_Walk_F:Footstep_Notify_C_1", 5.0),
};

// One footstep, foot 0 or 1, as walking makes it. Returns false (and logs
// why, a few times) when the game's footstep or the player's Mesh is missing.
bool Footstep(UObject player, int foot) {
    static int complaints = 0;
    UObject notify = g_walk_step[foot & 1].Get();
    UObject mesh = 0;
    const bool have_mesh = ue::GetProp(player, "Mesh", mesh) && mesh;
    if (!notify || !have_mesh) {
        if (complaints++ < 3)
            log::Info("teleport: no landing footstep (%s)",
                      !notify ? "the game's walking footstep is not loaded" : "the player has no Mesh");
        return false;
    }
    ue::Call c(notify, "Received_Notify");
    c.Set("MeshComp", mesh).Set("Animation", ue::GetOuter(notify));
    const bool ok = c.Invoke();
    if (!ok && complaints++ < 3) log::Info("teleport: the game's footstep could not be called");
    return ok;
}

int IndexOf(UObject obj) { return obj ? mem::ReadOr<int32_t>(obj + ue::off::kObjIndex, -1) : -1; }
bool Alive(UObject obj, int index) { return obj && index >= 0 && ue::objects::Get(index) == obj; }
// The arc's components still exist (they go when the level or player does).
bool MarkersAlive() {
    return Alive(T.owner, T.owner_index) && Alive(T.disc, T.disc_index) && Alive(T.beads[0], T.bead0_index);
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

UObject FindMarkerMesh(double* size) {
    UObject sm_class = ue::FindObject("Class /Script/Engine.StaticMesh");
    if (!sm_class) return 0;
    const char* exact[] = {"StaticMesh /Engine/BasicShapes/Sphere.Sphere", "StaticMesh /Engine/EngineMeshes/Sphere.Sphere",
                           "StaticMesh /Engine/BasicShapes/Cube.Cube"};
    UObject mesh = 0;
    for (const char* n : exact)
        if ((mesh = ue::FindObject(n))) break;
    if (!mesh) {
        UObject sphere = 0, cube = 0;
        ue::objects::ForEach([&](int, UObject o) {
            if (sphere || ue::GetClass(o) != sm_class) return;
            std::string n = Lower(ue::GetName(o));
            if (n == "sphere" || n == "sm_sphere" || n == "shape_sphere") sphere = o;
            else if (!cube && (n == "cube" || n == "sm_cube" || n == "shape_cube")) cube = o;
        });
        mesh = sphere ? sphere : cube;
    }
    if (!mesh) return 0;
    struct Box {
        FVector min, max;
        uint8_t valid;
        uint8_t pad[7];
    };
    ue::Call bb(mesh, "GetBoundingBox");
    if (bb.Invoke()) {
        Box b = bb.Result<Box>();
        double s = std::fmax(b.max.X - b.min.X, std::fmax(b.max.Y - b.min.Y, b.max.Z - b.min.Z));
        if (s > 0.1 && s < 10000) *size = s;
    }
    return mesh;
}

void SetVisible(UObject comp, bool visible) {
    if (!comp) return;
    ue::Call c(comp, "SetVisibility");
    c.Set("bNewVisibility", visible).Set("bPropagateToChildren", false);
    c.Invoke();
}

void CallBool(UObject comp, const char* fn, const char* param, bool v) {
    ue::Call c(comp, fn);
    c.Set(param, v);
    c.Invoke();
}

UObject MakeMarker(UObject player, UObject cls) {
    ue::Call add(player, "AddComponentByClass");
    add.Set("Class", cls).Set("bManualAttachment", true).Set("RelativeTransform", Xf{}).Set("bDeferredFinish", false);
    UObject c = add.Invoke() ? add.Result<UObject>() : 0;
    if (!c) return 0;
    ue::Call col(c, "SetCollisionEnabled");
    col.Set("NewType", static_cast<uint8_t>(0));  // NoCollision
    col.Invoke();
    CallBool(c, "SetGenerateOverlapEvents", "bInGenerateOverlapEvents", false);
    CallBool(c, "SetCastShadow", "NewCastShadow", false);
    ue::Call sm(c, "SetStaticMesh");
    sm.Set("NewMesh", T.mesh);
    sm.Invoke();
    SetVisible(c, false);
    return c;
}

bool MakeMarkers(UObject player) {
    if (T.owner == player && MarkersAlive()) return true;
    T.owner = 0;
    T.disc = 0;
    for (UObject& b : T.beads) b = 0;
    T.beads_shown = 0;
    T.disc_shown = false;
    if (T.tried && !T.mesh) return false;
    if (!T.tried) {
        T.tried = true;
        T.ksl = ue::FindObject("KismetSystemLibrary /Script/Engine.Default__KismetSystemLibrary");
        T.gps = ue::FindObject("GameplayStatics /Script/Engine.Default__GameplayStatics");
        T.mesh = FindMarkerMesh(&T.mesh_size);
        if (!T.mesh || !T.ksl || !T.gps) {
            log::Warn("teleport: can't show the arc (sphere mesh %s, trace library %s); teleport is off",
                      T.mesh ? "found" : "missing", T.ksl && T.gps ? "found" : "missing");
            T.mesh = 0;
            return false;
        }
        log::Info("teleport: arc uses %s (%.0f cm)", ue::GetName(T.mesh).c_str(), T.mesh_size);
    }
    UObject cls = ue::FindObject("Class /Script/Engine.StaticMeshComponent");
    if (!cls || !player) return false;
    for (UObject& b : T.beads)
        if (!(b = MakeMarker(player, cls))) {
            log::Error("teleport: could not add the arc beads");
            return false;
        }
    if (!(T.disc = MakeMarker(player, cls))) return false;
    T.owner = player;
    T.owner_index = IndexOf(player);
    T.disc_index = IndexOf(T.disc);
    T.bead0_index = IndexOf(T.beads[0]);
    return true;
}

void Place(UObject comp, const FVector& at, const FVector& size_cm) {
    Xf t;
    t.translation = at;
    t.scale = FVector{size_cm.X / T.mesh_size, size_cm.Y / T.mesh_size, size_cm.Z / T.mesh_size};
    ue::Call c(comp, "K2_SetWorldTransform");
    c.Set("NewTransform", t).Set("bSweep", false).Set("bTeleport", true);
    c.Invoke();
}

void ShowBeads(int n) {
    n = std::clamp(n, 0, kPoints);
    for (int i = n; i < T.beads_shown; ++i) SetVisible(T.beads[i], false);
    for (int i = T.beads_shown; i < n; ++i) SetVisible(T.beads[i], true);
    T.beads_shown = n;
}

void ShowDisc(bool on) {
    if (on != T.disc_shown) SetVisible(T.disc, on);
    T.disc_shown = on;
}

// One straight piece of the arc against the level. The player and the gun
// are not in the way.
bool Trace(UObject player, const FVector& a, const FVector& b, FVector* at, FVector* normal) {
    UObject ignore[2] = {player, 0};
    int32_t n = 1;
    game::Gun gun = game::Gun::Current(player);
    if (gun.Valid()) ignore[n++] = gun.Object();
    TArrayRaw arr{ignore, n, n};
    ue::Call t(T.ksl, "LineTraceSingle");
    t.Set("WorldContextObject", player).Set("Start", a).Set("End", b).Set("TraceChannel", uint8_t{0})
        .Set("bTraceComplex", false).Set("ActorsToIgnore", arr).Set("bIgnoreSelf", true);
    if (!t.Invoke() || !t.Result<bool>()) return false;
    int32_t hit_size = 0;
    uint8_t* hit = t.Raw("OutHit", &hit_size);
    if (!hit) return false;
    ue::Call br(T.gps, "BreakHitResult");
    int32_t in_size = 0;
    uint8_t* in = br.Raw("Hit", &in_size);
    if (!in || in_size != hit_size) return false;
    std::memcpy(in, hit, static_cast<size_t>(hit_size));
    if (!br.Invoke() || !br.Get<bool>("bBlockingHit") || br.Get<bool>("bInitialOverlap")) return false;
    *at = br.Get<FVector>("ImpactPoint");
    *normal = br.Get<FVector>("ImpactNormal");
    return true;
}

bool Feet(UObject player, FVector* feet, double* half) {
    ue::Call loc(player, "K2_GetActorLocation");
    if (!loc.Invoke()) return false;
    *half = 88.0;
    UObject capsule = 0;
    if (ue::GetProp(player, "CapsuleComponent", capsule) && capsule) {
        ue::Call h(capsule, "GetScaledCapsuleHalfHeight");
        if (h.Invoke() && h.Result<float>() > 10.f) *half = h.Result<float>();
    }
    *feet = loc.Result<FVector>();
    feet->Z -= *half;
    return true;
}

void Aim(UObject player, int hand) {
    T.have_target = false;
    hands::HandState h = hands::Get(static_cast<hands::Hand>(hand));
    FVector feet;
    double half = 88;
    if (!h.valid || !Feet(player, &feet, &half)) {
        ShowBeads(0);
        ShowDisc(false);
        return;
    }
    FVector dir = h.aim.rotation.Rotate(FVector{1, 0, 0});
    std::vector<FVector> pts = ArcPoints(h.aim.location, dir, kSpeed, kGravity, kStep, kPoints);
    int shown = 0;
    FVector at, normal;
    bool hit = false;
    for (int i = 1; i < kPoints && !hit; ++i) {
        hit = Trace(player, pts[static_cast<size_t>(i - 1)], pts[static_cast<size_t>(i)], &at, &normal);
        // A bead at the start of each piece; the last one sits on the hit.
        Place(T.beads[shown++], hit ? at : pts[static_cast<size_t>(i)], FVector{kBeadCm, kBeadCm, kBeadCm});
    }
    ShowBeads(shown);
    if (!hit) {
        T.why = "nothing in reach";
        ShowDisc(false);
        return;
    }
    Landing l = CheckLanding(feet, at, normal.Z, kMaxRiseCm);
    T.why = l.why;
    if (!l.ok) {
        ShowDisc(false);
        return;
    }
    T.have_target = true;
    T.target = at;
    Place(T.disc, at + FVector{0, 0, 1.0}, FVector{kDiscCm, kDiscCm, 2.0});
    ShowDisc(true);
}

void Go(UObject player) {
    if (!T.have_target) {
        log::Info("teleport: let go with no place to land (%s)", T.why);
        input::Haptic(input::kLeft, 0.2f, 0.03f);
        return;
    }
    FVector feet;
    double half = 88;
    if (!Feet(player, &feet, &half)) return;
    FVector head{};
    stereo::HeadOffsetWorld(&head);
    head.Z = 0;
    T.move_to = CapsuleDestination(T.target, head, half);
    T.move_at = NowS() + kMoveAfterS;
    comfort::Blink(kBlinkS);
}

void Move(UObject player) {
    T.move_at = -1;
    ue::Call loc(player, "K2_GetActorLocation");
    ue::Call rot(player, "K2_GetActorRotation");
    if (!loc.Invoke() || !rot.Invoke()) return;
    FVector from = loc.Result<FVector>();
    ue::Call tp(player, "K2_TeleportTo");
    tp.Set("DestLocation", T.move_to).Set("DestRotation", rot.Result<FRotator>());
    bool ok = tp.Invoke() && tp.Result<bool>();
    FVector d = T.move_to - from;
    double dist = std::sqrt(d.X * d.X + d.Y * d.Y + d.Z * d.Z);
    if (ok) {
        const bool stepped = Footstep(player, 0);
        T.steps = stepped ? 1 : 0;
        T.step_at = stepped ? NowS() + kSecondStepS : -1;
        log::Info("teleport: moved %.0f cm (to %.0f, %.0f, %.0f)%s", dist, T.move_to.X, T.move_to.Y, T.move_to.Z,
                  stepped ? "; footsteps as walking makes them (sound and the noise enemies hear)" : "");
    } else {
        log::Info("teleport: no room for the character there (%.0f cm away); stayed put", dist);
        input::Haptic(input::kLeft, 0.3f, 0.05f);
    }
}

}  // namespace

void Hide() {
    T.gesture.Reset();
    T.have_target = false;
    T.move_at = -1;
    T.step_at = -1;
    if (MarkersAlive()) {
        ShowBeads(0);
        ShowDisc(false);
    }
}

void Update(UObject player, const input::HandInput& stick, int hand) {
    if (!player) return;
    if (T.move_at > 0 && NowS() >= T.move_at) Move(player);
    if (T.step_at > 0 && NowS() >= T.step_at) {
        T.step_at = -1;
        if (Footstep(player, T.steps)) ++T.steps;
    }
    if (!MakeMarkers(player)) return;
    switch (T.gesture.Update(stick.stick_x, stick.stick_y)) {
        case Gesture::kAiming:
            Aim(player, hand);
            break;
        case Gesture::kGo:
            Go(player);
            ShowBeads(0);
            ShowDisc(false);
            break;
        case Gesture::kCancel:
            T.have_target = false;
            ShowBeads(0);
            ShowDisc(false);
            break;
        case Gesture::kIdle:
            break;
    }
}

}  // namespace holevr::vr::teleport
