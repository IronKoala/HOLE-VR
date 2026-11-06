#include "vr/bench_vise.h"
#include <cmath>
#include <cstring>
#include <vector>
#include "core/log.h"
#include "core/memory.h"
#include "ue/call.h"
#include "ue/names.h"
#include "ue/objects.h"
#include "ue/reflection.h"
#include "vr/bench_vise_mesh.h"
#include "vr/bench_vise_motion.h"

namespace holevr::vr::bench_vise {
namespace {
using ue::UObject;
struct Ref {
    UObject obj = 0, cls = 0;
    int index = -1;
    ue::FName name{};
    void Set(UObject p) {
        obj = p;
        cls = p ? ue::GetClass(p) : 0;
        index = p ? mem::ReadOr<int32_t>(p + ue::off::kObjIndex, -1) : -1;
        name = p ? ue::GetFName(p) : ue::FName{};
    }
    bool Live() const {
        if (!obj || index < 0 || ue::objects::Get(index) != obj || ue::GetClass(obj) != cls) return false;
        const auto current = ue::GetFName(obj);
        return std::memcmp(&current, &name, sizeof(name)) == 0;
    }
};
struct alignas(16) Transform {
    FQuat rotation;
    FVector translation;
    double pad0 = 0;
    FVector scale{1, 1, 1};
    double pad1 = 0;
};
static_assert(sizeof(Transform) == 0x60);
Ref g_owner, g_components[2];
double g_retry = 0;
bool g_visible = false, g_placed[2] = {};
Transform g_pose[2];
motion::Jaw g_jaw;
double g_last_time = -1;
int g_occupied = -1;

bool Visibility(bool on) {
    bool ok = true;
    for (const auto& part : g_components) {
        if (!part.Live()) { ok=false; continue; }
        ue::Call c(part.obj, "SetVisibility");
        c.Set("bNewVisibility", on).Set("bPropagateToChildren", false);
        ok = c.Invoke() && ok;
    }
    return ok;
}
void ClearComponent() {
    for (auto& part : g_components) {
      if (part.Live()) {
        ue::Call c(part.obj, "K2_DestroyComponent");
        c.Set("Object", g_owner.Live() ? g_owner.obj : UObject{0});
        c.Invoke();
      }
      part = {};
    }
    g_visible = g_placed[0] = g_placed[1] = false;
    g_jaw.Reset(); g_last_time=-1; g_occupied=-1;
}

// This call copies const arrays synchronously. Only borrow caller storage
// for reflected const parameters; never transfer CRT allocations to UE.
struct ArrayView { const void* data = nullptr; int32_t count = 0, capacity = 0; };
static_assert(sizeof(ArrayView) == 16);
bool Array(ue::Call& c, const char* name, const void* data, int count) {
    const auto p = ue::reflection::FindProperty(c.Function(), name);
    if (p.type != "ArrayProperty" || p.element_size != sizeof(ArrayView) || !(p.flags & uint64_t{2})) {
        log::Warn("bench vise: unsupported const array %s (%s, %d bytes)", name, p.type.c_str(), p.element_size);
        return false;
    }
    int32_t size = 0;
    auto* raw = c.Raw(name, &size);
    if (!raw || size != sizeof(ArrayView)) return false;
    const ArrayView v{data, count, count};
    std::memcpy(raw, &v, sizeof(v));
    return true;
}

bool Section(int index, UObject material, ue::FName base_color, ue::FName metallic, ue::FName roughness) {
    const auto& src = mesh::kSections[index];
    const UObject component = g_components[src.part].obj;
    std::vector<FVector> positions, normals;
    std::vector<int32_t> triangles;
    positions.reserve(src.count); normals.reserve(src.count); triangles.reserve(src.count);
    for (int i = 0; i < src.count; ++i) {
        positions.push_back(src.vertices[i].position);
        normals.push_back(src.vertices[i].normal);
        triangles.push_back(i);
    }
    ue::Call c(component, "CreateMeshSection_LinearColor");
    if (!c.Valid() || !Array(c, "Vertices", positions.data(), src.count) ||
        !Array(c, "Triangles", triangles.data(), src.count) || !Array(c, "Normals", normals.data(), src.count))
        return false;
    for (const char* name : {"UV0", "UV1", "UV2", "UV3", "VertexColors", "Tangents"})
        if (!Array(c, name, nullptr, 0)) return false;
    c.Set("SectionIndex", int32_t{src.material}).Set("bCreateCollision", false).Set("bSRGBConversion", false);
    if (!c.Invoke()) return false;
    ue::Call count(component, "GetNumSections");
    if (!count.Invoke() || count.Result<int32_t>() <= src.material) return false;
    // Use the game's weapon-metal shader. All three material values are
    // copied in linear space from the extracted HOLE bible. A private MID
    // also avoids changing any weapon's shared material.
    ue::Call mid(component, "CreateDynamicMaterialInstance");
    mid.Set("ElementIndex", int32_t{src.material}).Set("SourceMaterial", material);
    const UObject m = mid.Invoke() ? mid.Result<UObject>() : 0;
    if (!m) return false;
    ue::Call color(m, "SetVectorParameterValue");
    color.Set("ParameterName", base_color).Set("Value", src.color);
    ue::Call metal(m, "SetScalarParameterValue");
    metal.Set("ParameterName", metallic).Set("Value", src.metallic);
    ue::Call rough(m, "SetScalarParameterValue");
    rough.Set("ParameterName", roughness).Set("Value", src.roughness);
    if (!color.Invoke() || !metal.Invoke() || !rough.Invoke()) return false;
    ue::Call get_color(m, "K2_GetVectorParameterValue");
    get_color.Set("ParameterName", base_color);
    ue::Call get_metal(m, "K2_GetScalarParameterValue");
    get_metal.Set("ParameterName", metallic);
    ue::Call get_rough(m, "K2_GetScalarParameterValue");
    get_rough.Set("ParameterName", roughness);
    if (!get_color.Invoke() || !get_metal.Invoke() || !get_rough.Invoke()) return false;
    const auto read = get_color.Result<mesh::Color>();
    const bool matches = std::fabs(read.r-src.color.r)<1e-5f && std::fabs(read.g-src.color.g)<1e-5f &&
        std::fabs(read.b-src.color.b)<1e-5f && std::fabs(read.a-src.color.a)<1e-5f &&
        std::fabs(get_metal.Result<float>()-src.metallic)<1e-5f &&
        std::fabs(get_rough.Result<float>()-src.roughness)<1e-5f;
    if (!matches) log::Warn("bench vise: section %d material did not retain the bible values", index);
    return matches;
}

bool Ensure(UObject bench, double now) {
    if (!ue::IsLive(bench)) { Hide(); return false; }
    if (g_owner.obj != bench || !g_owner.Live()) {
        ClearComponent();
        g_owner.Set(bench);
        g_retry = 0;
    }
    if (g_components[0].Live() && g_components[1].Live()) return true;
    if (now < g_retry) return false;
    g_retry = now + 5;
    ClearComponent();
    static ue::ObjectRef proc("Class /Script/ProceduralMeshComponent.ProceduralMeshComponent", 5);
    static ue::ObjectRef metal("MaterialInstanceConstant /Game/Weapons/Materials/MI_GunMetalBlack.MI_GunMetalBlack", 5);
    const UObject cls = proc.Get(), material = metal.Get();
    ue::FName base_color{}, metallic{}, roughness{};
    if (!cls || !material || !ue::names::Find("BaseColor", &base_color) ||
        !ue::names::Find("Metallic", &metallic) || !ue::names::Find("Roughness", &roughness)) {
        log::Warn("bench vise: waiting for mesh class and HOLE weapon-metal material");
        return false;
    }
    bool ok = true;
    for (auto& part : g_components) {
      ue::Call add(bench, "AddComponentByClass");
    add.Set("Class", cls).Set("bManualAttachment", true).Set("RelativeTransform", Transform{})
       .Set("bDeferredFinish", false);
      part.Set(add.Invoke() ? add.Result<UObject>() : 0);
      if (!part.Live()) { ok=false; break; }
      ue::Call visibility(part.obj, "SetVisibility");
      visibility.Set("bNewVisibility", false).Set("bPropagateToChildren", false);
      ok = visibility.Invoke() && ok;
    ue::Call col(part.obj, "SetCollisionEnabled");
    col.Set("NewType", uint8_t{0});
    ok = col.Invoke() && ok;
    ue::Call overlap(part.obj, "SetGenerateOverlapEvents");
    overlap.Set("bInGenerateOverlapEvents", false);
    ok = overlap.Invoke() && ok;
    ue::Call mobility(part.obj, "SetMobility");
    mobility.Set("NewMobility", uint8_t{2});
    ok = mobility.Invoke() && ok;
    }
    for (int i = 0; ok && i < mesh::kSectionCount; ++i) ok = Section(i, material, base_color, metallic, roughness);
    if (!ok) {
        log::Warn("bench vise: setup failed validation, removed partial model; retry in 5 seconds");
        ClearComponent();
        return false;
    }
    log::Info("bench vise: ready, %d triangles, fixed base and moving jaw; automatic open/close, HOLE metal palette", mesh::kTriangleCount);
    return true;
}

bool SamePose(const Transform& a, const Transform& b) {
    return std::fabs(a.translation.X-b.translation.X)<.001 && std::fabs(a.translation.Y-b.translation.Y)<.001 &&
        std::fabs(a.translation.Z-b.translation.Z)<.001 && std::fabs(a.rotation.X-b.rotation.X)<1e-6 &&
        std::fabs(a.rotation.Y-b.rotation.Y)<1e-6 && std::fabs(a.rotation.Z-b.rotation.Z)<1e-6 &&
        std::fabs(a.rotation.W-b.rotation.W)<1e-6;
}
}  // namespace

void Show(uintptr_t bench, const FVector& position, const FQuat& rotation,
          bool occupied, double loaded_gap_cm, double now) {
    if (!std::isfinite(position.X) || !std::isfinite(position.Y) || !std::isfinite(position.Z) ||
        !std::isfinite(rotation.X) || !std::isfinite(rotation.Y) || !std::isfinite(rotation.Z) ||
        !std::isfinite(rotation.W) || !std::isfinite(now)) { Hide(); return; }
    if (!Ensure(bench, now)) return;
    const double dt = g_last_time >= 0 ? std::max(0.0, now-g_last_time) : 0;
    g_last_time=now;
    const double gap=g_jaw.Update(occupied,loaded_gap_cm,dt);
    if (g_occupied != int(occupied)) {
        log::Info("bench vise: %s, target jaw gap %.2f cm", occupied ? "closing on parked weapon" : "opening (empty)",
                  occupied ? loaded_gap_cm : motion::kOpenGap);
        g_occupied=int(occupied);
    }
    for (int part=0;part<2;++part) {
      Transform t;
    t.translation = position;
    t.rotation = rotation;
    if (part==1) t.translation=t.translation+rotation.Rotate({gap-motion::kOpenGap,0,0});
    if (!g_placed[part] || !SamePose(t, g_pose[part])) {
        ue::Call c(g_components[part].obj, "K2_SetWorldTransform");
        c.Set("NewTransform", t).Set("bSweep", false).Set("bTeleport", true);
        if (!c.Invoke()) { Hide(); return; }
        if (!g_placed[part] && part==0)
            log::Info("bench vise: centred on the player-facing edge at (%.1f %.1f %.1f)", position.X, position.Y, position.Z);
        g_pose[part] = t; g_placed[part] = true;
    }
    }
    if (!g_visible) g_visible = Visibility(true);
}
void Hide() {
    if (g_visible) Visibility(false);
    g_visible = false;
}
void Reset() {
    ClearComponent();
    g_owner = {};
    g_retry = 0;
}
}  // namespace holevr::vr::bench_vise
