#include "vr/interact/holster_model.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include "core/log.h"
#include "core/memory.h"
#include "ue/call.h"
#include "ue/names.h"
#include "ue/objects.h"
#include "ue/reflection.h"
#include "vr/interact/holster_mesh_data.h"

namespace holevr::vr::holster_model {
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
Ref g_player, g_body, g_hood;
double g_retry = 0;
bool g_visible = false;

void Destroy(Ref& ref) {
    if (ref.Live()) {
        ue::Call c(ref.obj, "K2_DestroyComponent");
        c.Set("Object", g_player.Live() ? g_player.obj : UObject{0});
        c.Invoke();
    }
    ref = {};
}
void Clear() {
    Destroy(g_body);
    Destroy(g_hood);
    g_visible = false;
}
void Visibility(const Ref& ref, bool on) {
    if (!ref.Live()) return;
    ue::Call c(ref.obj, "SetVisibility");
    c.Set("bNewVisibility", on).Set("bPropagateToChildren", false);
    c.Invoke();
}

// Borrowed, const arrays are only used by this synchronous native call.
// The component copies them into its own buffers. No engine-owned array is
// ever freed by the DLL, and no borrowed memory escapes the call.
struct ArrayView { const void* data = nullptr; int32_t count = 0, capacity = 0; };
static_assert(sizeof(ArrayView) == 16);
bool Array(ue::Call& call, const char* name, const void* data, int count) {
    const auto p = ue::reflection::FindProperty(call.Function(), name);
    constexpr uint64_t kConstParm = 0x2;
    if (p.type != "ArrayProperty" || p.element_size != sizeof(ArrayView) || !(p.flags & kConstParm)) {
        log::Warn("holster model: %s array signature is unsupported (%s, %d bytes, flags %llx)",
                  name, p.type.c_str(), p.element_size, static_cast<unsigned long long>(p.flags));
        return false;
    }
    int32_t size = 0;
    uint8_t* raw = call.Raw(name, &size);
    if (!raw || size != sizeof(ArrayView)) return false;
    const ArrayView v{data, count, count};
    std::memcpy(raw, &v, sizeof(v));
    return true;
}

bool Section(UObject comp, int index, const mesh::Section& src, UObject material, ue::FName color_name) {
    std::vector<FVector> vertices, normals;
    std::vector<int32_t> triangles;
    vertices.reserve(src.count);
    normals.reserve(src.count);
    triangles.reserve(src.count);
    for (int i = 0; i < src.count; ++i) {
        vertices.push_back(src.vertices[i].position);
        normals.push_back(src.vertices[i].normal);
        triangles.push_back(i);
    }
    ue::Call c(comp, "CreateMeshSection_LinearColor");
    if (!c.Valid() || !Array(c, "Vertices", vertices.data(), src.count) ||
        !Array(c, "Triangles", triangles.data(), src.count) || !Array(c, "Normals", normals.data(), src.count))
        return false;
    for (const char* name : {"UV0", "UV1", "UV2", "UV3", "VertexColors", "Tangents"})
        if (!Array(c, name, nullptr, 0)) return false;
    c.Set("SectionIndex", int32_t{index}).Set("bCreateCollision", false).Set("bSRGBConversion", false);
    if (!c.Invoke()) return false;
    ue::Call check(comp, "GetNumSections");
    if (!check.Invoke() || check.Result<int32_t>() <= index) return false;
    ue::Call mid(comp, "CreateDynamicMaterialInstance");
    mid.Set("ElementIndex", int32_t{index}).Set("SourceMaterial", material);
    const UObject m = mid.Invoke() ? mid.Result<UObject>() : 0;
    if (!m) return false;
    ue::Call color(m, "SetVectorParameterValue");
    color.Set("ParameterName", color_name).Set("Value", src.color);
    return color.Invoke();
}

bool Add(UObject player, UObject cls, Ref& out) {
    ue::Call c(player, "AddComponentByClass");
    c.Set("Class", cls).Set("bManualAttachment", true).Set("RelativeTransform", Transform{})
        .Set("bDeferredFinish", false);
    out.Set(c.Invoke() ? c.Result<UObject>() : 0);
    if (!out.Live()) return false;
    Visibility(out, false);
    ue::Call col(out.obj, "SetCollisionEnabled");
    col.Set("NewType", uint8_t{0});
    col.Invoke();
    ue::Call overlap(out.obj, "SetGenerateOverlapEvents");
    overlap.Set("bInGenerateOverlapEvents", false);
    overlap.Invoke();
    ue::Call mobility(out.obj, "SetMobility");
    mobility.Set("NewMobility", uint8_t{2});
    mobility.Invoke();
    // The component belongs to the player but is placed in world space by
    // the holster anchor each late tick. Never reparent the pistol or body.
    ue::Call owner_visible(out.obj, "SetOwnerNoSee");
    owner_visible.Set("bNewOwnerNoSee", false);
    owner_visible.Invoke();
    return true;
}

bool Ensure(UObject player, double now) {
    if (!ue::IsLive(player)) { Hide(); return false; }
    if (g_player.obj != player || !g_player.Live()) {
        Clear();
        g_player.Set(player);
        g_retry = 0;
    }
    if (g_body.Live() && g_hood.Live()) return true;
    if (now < g_retry) return false;
    g_retry = now + 5.0;
    Clear();
    static ue::ObjectRef proc("Class /Script/ProceduralMeshComponent.ProceduralMeshComponent", 5.0);
    static ue::ObjectRef mat("Material /Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial", 5.0);
    const UObject cls = proc.Get(), material = mat.Get();
    ue::FName color{};
    const bool have_color = ue::names::Find("Color", &color);
    if (!cls || !material || !have_color) {
        log::Warn("holster model: waiting for mesh class/material/colour (%d/%d/%d)", !!cls, !!material, have_color);
        return false;
    }
    bool ok = Add(player, cls, g_body) && Add(player, cls, g_hood);
    for (int i = 0; ok && i < 3; ++i) ok = Section(g_body.obj, i, mesh::kSections[i], material, color);
    if (ok) ok = Section(g_hood.obj, 0, mesh::kSections[3], material, color);
    if (!ok) {
        log::Warn("holster model: mesh setup failed validation; no partial model kept, retry in 5 seconds");
        Clear();
        return false;
    }
    log::Info("holster model: approved v4, 1284 triangles, green/olive/carbon; body and moving hood ready");
    return true;
}
void Place(const Ref& ref, const Transform& t) {
    ue::Call c(ref.obj, "K2_SetWorldTransform");
    c.Set("NewTransform", t).Set("bSweep", false).Set("bTeleport", true);
    c.Invoke();
}
bool Finite(const Transform& t) {
    return std::isfinite(t.translation.X) && std::isfinite(t.translation.Y) && std::isfinite(t.translation.Z) &&
           std::isfinite(t.rotation.X) && std::isfinite(t.rotation.Y) && std::isfinite(t.rotation.Z) &&
           std::isfinite(t.rotation.W) && std::isfinite(t.scale.X) && std::isfinite(t.scale.Y) &&
           std::isfinite(t.scale.Z) && t.scale.X > 0 && t.scale.Y > 0 && t.scale.Z > 0;
}
}  // namespace

void Show(uintptr_t player, const Transform& seat, double hood_open, double now) {
    if (!Finite(seat) || !std::isfinite(hood_open)) { Hide(); return; }
    if (!Ensure(player, now)) return;
    Place(g_body, seat);
    Transform hood = seat;
    const FVector p{mesh::kHoodPivot.X * seat.scale.X, mesh::kHoodPivot.Y * seat.scale.Y,
                    mesh::kHoodPivot.Z * seat.scale.Z};
    hood.translation = seat.translation + seat.rotation.Rotate(p);
    const double half = std::clamp(hood_open, 0.0, 1.0) * mesh::kHoodOpenDegrees * kDegToRad * 0.5;
    hood.rotation = seat.rotation * FQuat{0, std::sin(half), 0, std::cos(half)};
    Place(g_hood, hood);
    if (!g_visible) { Visibility(g_body, true); Visibility(g_hood, true); g_visible = true; }
}
void Hide() {
    if (g_visible) { Visibility(g_body, false); Visibility(g_hood, false); }
    g_visible = false;
}
}  // namespace holevr::vr::holster_model
