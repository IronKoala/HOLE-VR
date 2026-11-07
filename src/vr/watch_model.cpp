#include "vr/watch_model.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "ue/call.h"
#include "ue/names.h"
#include "ue/reflection.h"
#include "vr/watch_mesh_data.h"
#include "vr/watch_marshal.h"

namespace holevr::vr::watch::model {
namespace {
using ue::UObject;
struct Ref {
    UObject obj=0,cls=0; int index=-1; ue::FName name{};
    void Set(UObject p) {
        obj=p;cls=p?ue::GetClass(p):0;name=p?ue::GetFName(p):ue::FName{};
        index=p?mem::ReadOr<int32_t>(p+ue::off::kObjIndex,-1):-1;
    }
    bool Live() const {
        if(!obj || index<0 || ue::objects::Get(index)!=obj || ue::GetClass(obj)!=cls)return false;
        const auto n=ue::GetFName(obj);return !std::memcmp(&n,&name,sizeof(n));
    }
};
Ref g_player,g_parts[5];
skel::Xf g_mount;
double g_retry=0,g_clock_at=0;
bool g_visible=false;
std::string g_text;

void Visibility(const Ref& r,bool on) {
    if(!r.Live())return;
    ue::Call c(r.obj,"SetVisibility");
    c.Set("bNewVisibility",on).Set("bPropagateToChildren",false);c.Invoke();
}
void Clear() {
    for(auto& r:g_parts) {
        if(r.Live()) {
            ue::Call c(r.obj,"K2_DestroyComponent");
            c.Set("Object",g_player.Live()?g_player.obj:UObject{0});c.Invoke();
        }
        r={};
    }
    g_visible=false;g_text.clear();g_clock_at=0;
}

// These known native functions consume input views synchronously. The path
// string/geometry vectors below stay alive across Call::Invoke's direct
// ProcessEvent call; its byte buffer does not destroy engine containers.
// Never extend this to outputs, mutable refs, retained inputs or async calls.
bool Borrow(ue::Call& c,const char* name,const void* data,int count) {
    const auto p=ue::reflection::FindProperty(c.Function(),name);
    const auto function=ue::GetName(c.Function());
    const marshal::Input contract{function,name,p.type,p.element_size,p.array_dim,p.flags};
    int32_t size=0;auto raw=c.Raw(name,&size);
    if(!marshal::WriteInputView(contract,raw,size,data,count)) {
        log::Warn("watch model: unsupported %s signature (%s, %d bytes, flags %llx)",
                  name,p.type.c_str(),p.element_size,static_cast<unsigned long long>(p.flags));return false;
    }
    return true;
}
bool Array(ue::Call& c,const char* name,const void* p,int n) {return Borrow(c,name,p,n);}
UObject Texture(UObject player,const wchar_t* name) {
    static ue::ObjectRef lib("KismetRenderingLibrary /Script/Engine.Default__KismetRenderingLibrary",5);
    const auto path=config::Dir()+L"\\watch\\"+name;
    ue::Call c(lib.Get(),"ImportFileAsTexture2D");
    if(!c.Valid() || !Borrow(c,"Filename",path.c_str(),static_cast<int>(path.size()+1)))return 0;
    c.Set("WorldContextObject",player);
    return c.Invoke()?c.Result<UObject>():0;
}
bool Name(const char* text,ue::FName& out) {return ue::names::Find(text,&out);}
bool SetTexture(UObject material,UObject texture) {
    ue::FName slate{},tint{},opacity{};
    if(!Name("SlateUI",slate)||!Name("TintColorAndOpacity",tint)||!Name("OpacityFromTexture",opacity))return false;
    ue::Call c(material,"SetTextureParameterValue");c.Set("ParameterName",slate).Set("Value",texture);
    if(!c.Invoke())return false;
    ue::Call v(material,"SetVectorParameterValue");v.Set("ParameterName",tint).Set("Value",mesh::Color{1,1,1,1});
    ue::Call a(material,"SetScalarParameterValue");a.Set("ParameterName",opacity).Set("Value",float{1});
    ue::Call check(material,"K2_GetTextureParameterValue");check.Set("ParameterName",slate);
    return v.Invoke() && a.Invoke() && check.Invoke() && check.Result<UObject>()==texture;
}
bool Geometry(UObject comp,int section,const mesh::Vertex* src,int count,bool update) {
    std::vector<FVector> vertices,normals;std::vector<int32_t> indices;
    std::vector<mesh::UV> uv;std::vector<mesh::Color> colors;
    vertices.reserve(count);normals.reserve(count);uv.reserve(count);indices.reserve(count);colors.reserve(count);
    for(int i=0;i<count;++i) {
        vertices.push_back(src[i].position);normals.push_back(src[i].normal);uv.push_back(src[i].uv);
        indices.push_back(i);colors.push_back({1,1,1,1});
    }
    ue::Call c(comp,update?"UpdateMeshSection_LinearColor":"CreateMeshSection_LinearColor");
    if(!c.Valid() || !Array(c,"Vertices",vertices.data(),count) || !Array(c,"Normals",normals.data(),count) ||
        !Array(c,"UV0",uv.data(),count) || !Array(c,"VertexColors",colors.data(),count))return false;
    if(!update && !Array(c,"Triangles",indices.data(),count))return false;
    for(const char* n:{"UV1","UV2","UV3","Tangents"})if(!Array(c,n,nullptr,0))return false;
    c.Set("SectionIndex",int32_t{section}).Set("bSRGBConversion",false);
    if(!update)c.Set("bCreateCollision",false);
    if(!c.Invoke())return false;
    ue::Call check(comp,"GetNumSections");return check.Invoke() && check.Result<int32_t>()>section;
}
bool Material(UObject comp,int section,UObject base,mesh::Color color,UObject texture) {
    ue::Call c(comp,"CreateDynamicMaterialInstance");c.Set("ElementIndex",int32_t{section}).Set("SourceMaterial",base);
    const UObject material=c.Invoke()?c.Result<UObject>():0;if(!material)return false;
    if(texture)return SetTexture(material,texture);
    ue::FName n{};if(!Name("Color",n))return false;
    ue::Call v(material,"SetVectorParameterValue");v.Set("ParameterName",n).Set("Value",color);return v.Invoke();
}
bool Add(UObject player,UObject cls,Ref& out) {
    ue::Call c(player,"AddComponentByClass");
    c.Set("Class",cls).Set("bManualAttachment",true).Set("RelativeTransform",skel::Xf{}).Set("bDeferredFinish",false);
    out.Set(c.Invoke()?c.Result<UObject>():0);if(!out.Live())return false;
    Visibility(out,false);
    ue::Call col(out.obj,"SetCollisionEnabled");col.Set("NewType",uint8_t{0});col.Invoke();
    ue::Call overlap(out.obj,"SetGenerateOverlapEvents");overlap.Set("bInGenerateOverlapEvents",false);overlap.Invoke();
    ue::Call mobile(out.obj,"SetMobility");mobile.Set("NewMobility",uint8_t{2});mobile.Invoke();
    ue::Call owner(out.obj,"SetOwnerNoSee");owner.Set("bNewOwnerNoSee",false);owner.Invoke();
    return true;
}
bool Digits(const std::string& text,bool update) {
    mesh::Vertex vertices[48];
    const int length=static_cast<int>(std::min<size_t>(8,text.size()));
    for(int i=0;i<8;++i) {
        const char ch=i<length?text[i]:' ';
        const int cell=(ch>='0'&&ch<='9')?ch-'0':ch==':'?10:11;
        const double x=(i-(length-1)*.5)*mesh::kAdvance;
        for(int v=0;v<6;++v) {
            auto& dest=vertices[i*6+v];dest=mesh::kGlyph[v];
            dest.position=dest.position+mesh::kDigitOrigin+FVector{x,0,0};
            dest.uv.u+=(cell-11)/16.0;
        }
    }
    return Geometry(g_parts[4].obj,0,vertices,48,update);
}
bool Ensure(UObject player,double now) {
    if(!ue::IsLive(player)){Hide();return false;}
    if(g_player.obj!=player || !g_player.Live()){Clear();g_player.Set(player);g_retry=0;}
    bool ready=true;for(const auto& r:g_parts)ready=ready&&r.Live();if(ready)return true;
    if(now<g_retry)return false;g_retry=now+5;Clear();
    static ue::ObjectRef proc("Class /Script/ProceduralMeshComponent.ProceduralMeshComponent",5);
    static ue::ObjectRef solid("Material /Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial",5);
    static ue::ObjectRef masked("MaterialInstanceConstant /Engine/EngineMaterials/Widget3DPassThrough_Masked_OneSided.Widget3DPassThrough_Masked_OneSided",5);
    const UObject cls=proc.Get(),base=solid.Get(),art=masked.Get();
    if(!cls || !base || !art){log::Warn("watch model: waiting for native mesh and materials");return false;}
    const UObject dial=Texture(player,L"dial.png"),atlas=Texture(player,L"timer-atlas.png");
    if(!dial || !atlas){log::Warn("watch model: artwork not loaded from hole-vr/watch; retry in 5 seconds");return false;}
    bool ok=true;for(auto& r:g_parts)if(ok)ok=Add(player,cls,r);
    for(int part=0;ok && part<4;++part) {
        const auto& p=mesh::kParts[part];
        for(int s=0;ok && s<p.count;++s) {
            const auto& section=p.sections[s];
            ok=Geometry(g_parts[part].obj,s,section.vertices,section.count,false) &&
               Material(g_parts[part].obj,s,section.texture?art:base,section.color,section.texture?dial:0);
        }
    }
    if(ok)ok=Digits("",false) && Material(g_parts[4].obj,0,art,{1,1,1,1},atlas);
    if(!ok){log::Warn("watch model: incomplete mesh/material setup; hiding and retrying in 5 seconds");Clear();return false;}
    log::Info("watch model: original 1920-triangle watch ready, wrist fit + dial + run inset; five native components");
    return true;
}
void Place(const Ref& r,const skel::Xf& t) {
    if(!r.Live())return;
    ue::Call c(r.obj,"K2_SetWorldTransform");c.Set("NewTransform",t).Set("bSweep",false).Set("bTeleport",true);c.Invoke();
}
bool Finite(const skel::Xf& t) {
    return std::isfinite(t.translation.X)&&std::isfinite(t.translation.Y)&&std::isfinite(t.translation.Z)&&
        std::isfinite(t.rotation.X)&&std::isfinite(t.rotation.Y)&&std::isfinite(t.rotation.Z)&&std::isfinite(t.rotation.W)&&
        std::isfinite(t.scale.X)&&std::isfinite(t.scale.Y)&&std::isfinite(t.scale.Z)&&
        t.scale.X>0&&t.scale.Y>0&&t.scale.Z>0;
}
bool Update(const HandAngles& angles,const std::string& text) {
    const double rotations[]={angles.hour,angles.minute,angles.second};
    for(int part=1;part<4;++part) {
        skel::Xf t=g_mount;
        t.translation=skel::Apply(g_mount,mesh::kParts[part].pivot);
        t.rotation=g_mount.rotation*ik::AxisAngle({0,0,1},rotations[part-1]);
        Place(g_parts[part],t);
    }
    if(text!=g_text) {
        if(Digits(text,true))g_text=text;
        else return false;
    }
    return true;
}
} // namespace
void Show(uintptr_t player,const skel::Xf& mount,const HandAngles& angles,const std::string& text,double now) {
    if(!Finite(mount)){Hide();return;}
    if(!Ensure(player,now))return;
    g_mount=mount;
    Place(g_parts[0],g_mount);Place(g_parts[4],g_mount);
    if(!Update(angles,text)){Hide();return;}
    g_clock_at=now;
    if(!g_visible){for(auto& r:g_parts)Visibility(r,true);g_visible=true;}
}
void Clock(const HandAngles& angles,const std::string& text,double now) {
    // Controller tick continues while pause menus suspend the player. Move
    // only hands/glyphs, keeping the watch against the paused body's wrist.
    if(!g_visible || now-g_clock_at<1.0/30.0)return;
    if(!g_player.Live()){Hide();return;}
    if(!Update(angles,text))Hide();
    g_clock_at=now;
}
void Hide() {if(g_visible)for(auto& r:g_parts)Visibility(r,false);g_visible=false;}
bool Visible() {return g_visible;}
} // namespace holevr::vr::watch::model
