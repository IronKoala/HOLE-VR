#include "vr/body/skin_model.h"
#include "vr/body/skin_asset_id.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "core/perf.h"
#include "ue/call.h"
#include "ue/reflection.h"
#include "vr/watch_marshal.h"

namespace holevr::vr::skin_model {
namespace {
using ue::UObject;
struct Ref {
    UObject obj=0,cls=0;int index=-1;ue::FName name{};
    void Set(UObject p) {obj=p;cls=p?ue::GetClass(p):0;name=p?ue::GetFName(p):ue::FName{};index=p?mem::ReadOr<int32_t>(p+ue::off::kObjIndex,-1):-1;}
    bool Live() const {if(!obj||index<0||ue::objects::Get(index)!=obj||ue::GetClass(obj)!=cls)return false;const auto n=ue::GetFName(obj);return !std::memcmp(&n,&name,sizeof(n));}
};
Ref player_ref,component,original;
skin_mesh::Mesh mesh;skin_mesh::Binding binding;skin_mesh::Frame frame;
std::vector<skin_mesh::UV> uv;std::vector<int32_t> triangles;
std::unique_ptr<ue::Call> update;
bool ready=false,visible=false;int last_shadow=-1;
perf::Counter skin_time{"body skin math"},engine_time{"body skin upload"};
void Bool(const Ref& r,const char* function,const char* name,bool value) {if(!r.Live())return;ue::Call c(r.obj,function);c.Set(name,value);c.Invoke();}
void Visibility(const Ref& r,bool value) {
    if(!r.Live())return;
    ue::Call c(r.obj,"SetVisibility");
    c.Set("bNewVisibility",value).Set("bPropagateToChildren",false);c.Invoke();
}
bool Array(ue::Call& c,const char* name,const void* data,int count) {
    const auto p=ue::reflection::FindProperty(c.Function(),name);
    // Input string_views must outlive validation (GetName returns a string).
    const auto function=ue::GetName(c.Function());
    const watch::marshal::Input stable{function,name,p.type,p.element_size,p.array_dim,p.flags};
    int32_t size=0;auto ptr=c.Raw(name,&size);
    if(!watch::marshal::WriteInputView(stable,ptr,size,data,count)) {
        log::Warn("body skin: unsupported %s signature (%s, %d, flags %llx)",name,p.type.c_str(),p.element_size,static_cast<unsigned long long>(p.flags));return false;
    }
    return true;
}
bool Inputs(ue::Call& c,bool create) {
    if(!c.Valid()||!Array(c,"Vertices",frame.vertices.data(),int(frame.vertices.size()))||
       !Array(c,"Normals",frame.normals.data(),int(frame.normals.size()))||
       !Array(c,"UV0",uv.data(),int(uv.size())))return false;
    for(const char* name:{"UV1","UV2","UV3","VertexColors","Tangents"})if(!Array(c,name,nullptr,0))return false;
    if(create&&!Array(c,"Triangles",triangles.data(),int(triangles.size())))return false;
    c.Set("SectionIndex",int32_t{0}).Set("bSRGBConversion",false);
    if(create)c.Set("bCreateCollision",false);
    return true;
}
bool Place(const skel::Xf& world) {
    ue::Call c(component.obj,"K2_SetWorldTransform");c.Set("NewTransform",world).Set("bSweep",false).Set("bTeleport",true);return c.Invoke();
}
void Fallback(bool shadow,const char* why) {
    log::Warn("body skin: %s; restoring the original posed mesh",why);
    Hide();Visibility(original,true);Bool(original,"SetCastShadow","NewCastShadow",shadow);ready=false;update.reset();
}
}
void Reset() {
    Hide();ready=false;update.reset();
    if(component.Live()) {ue::Call c(component.obj,"K2_DestroyComponent");c.Set("Object",player_ref.Live()?player_ref.obj:UObject{0});c.Invoke();}
    component={};original={};player_ref={};mesh={};binding={};frame={};uv.clear();triangles.clear();last_shadow=-1;
}
void Hide() {Visibility(component,false);visible=false;}
bool Ready() {return ready&&component.Live()&&original.Live()&&player_ref.Live();}
bool Prepare(uintptr_t player,uintptr_t source,uintptr_t pose_component,
             const std::vector<std::string>& names,const std::vector<int>& parents,const std::vector<skel::Xf>& reference) {
    Reset();
    if(!config::GetBool("body_refined_wrist_skin",true))return false;
    static const bool registered=[] {perf::Register(&skin_time);perf::Register(&engine_time);return true;}();(void)registered;
    player_ref.Set(player);original.Set(pose_component);
    if(!player_ref.Live()||!original.Live()||!ue::IsLive(source))return false;
    ue::Call asset(source,"GetSkinnedAsset");
    if(!asset.Invoke()||!asset.Result<UObject>()||ue::GetName(asset.Result<UObject>())!="WhiteMan") {
        log::Warn("body skin: model differs from the approved WhiteMan reference; retaining game skin");return false;
    }
    const auto path=std::filesystem::path(config::Dir())/L"body"/L"refined-whiteman.skin";
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file){log::Warn("body skin: approved mesh file missing; retaining game skin");return false;}
    const auto size=file.tellg();if(size<32||size>8*1024*1024){log::Warn("body skin: invalid file size");return false;}
    std::vector<uint8_t> bytes(static_cast<size_t>(size));file.seekg(0);file.read(reinterpret_cast<char*>(bytes.data()),size);
    std::string error;
    if(!file){log::Warn("body skin: could not read approved mesh; retaining game skin");return false;}
    if(!skin_mesh::Decode(bytes,&mesh,&error)) {
        log::Warn("body skin: invalid approved mesh: %s",error.c_str());return false;
    }
    if(mesh.content_id!=skin_mesh::kApprovedContentId) {
        log::Warn("body skin: asset revision does not match this build; retaining game skin");return false;
    }
    if(!skin_mesh::Bind(mesh,names,parents,reference,&binding,&error)) {
        log::Warn("body skin: cannot bind approved mesh: %s",error.c_str());return false;
    }
    skel::Pose rest;if(!rest.Init(parents))return false;rest.SetAllLocal(reference.data(),int(reference.size()));
    if(!skin_mesh::Deform(mesh,binding,rest,&frame))return false;
    uv.reserve(mesh.corners.size());triangles.reserve(mesh.corners.size());
    for(size_t i=0;i<mesh.corners.size();++i){uv.push_back(mesh.corners[i].uv);triangles.push_back(int(i));}
    static ue::ObjectRef proc("Class /Script/ProceduralMeshComponent.ProceduralMeshComponent",5);
    const auto cls=proc.Get();if(!cls){log::Warn("body skin: native procedural mesh class unavailable");return false;}
    ue::Call add(player,"AddComponentByClass");add.Set("Class",cls).Set("bManualAttachment",true).Set("RelativeTransform",skel::Xf{}).Set("bDeferredFinish",false);
    component.Set(add.Invoke()?add.Result<UObject>():0);if(!component.Live())return false;
    Hide();
    ue::Call collision(component.obj,"SetCollisionEnabled");collision.Set("NewType",uint8_t{0});collision.Invoke();
    Bool(component,"SetGenerateOverlapEvents","bInGenerateOverlapEvents",false);
    Bool(component,"SetOwnerNoSee","bNewOwnerNoSee",false);
    Bool(component,"SetCastHiddenShadow","NewCastHiddenShadow",false);
    ue::Call mobile(component.obj,"SetMobility");mobile.Set("NewMobility",uint8_t{2});mobile.Invoke();
    ue::Call create(component.obj,"CreateMeshSection_LinearColor");
    if(!Inputs(create,true)||!create.Invoke()){Reset();return false;}
    ue::Call sections(component.obj,"GetNumSections");
    if(!sections.Invoke()||sections.Result<int32_t>()!=1){Reset();return false;}
    ue::Call material(source,"GetMaterial");material.Set("ElementIndex",int32_t{0});
    if(!material.Invoke()||!material.Result<UObject>()){Reset();return false;}
    ue::Call set_material(component.obj,"SetMaterial");set_material.Set("ElementIndex",int32_t{0}).Set("Material",material.Result<UObject>());
    if(!set_material.Invoke()){Reset();return false;}
    update=std::make_unique<ue::Call>(component.obj,"UpdateMeshSection_LinearColor");
    if(!Inputs(*update,false)){Reset();return false;}
    ready=true;
    log::Info("body skin: revision 3 ready: %zu bones verified, %zu skin points, %zu triangles, original material; roll 0.5/1.0, wrist bend -3..+3 cm",
              mesh.bones.size(),mesh.points.size(),mesh.corners.size()/3);
    return true;
}
bool Update(const skel::Pose& pose,const skel::Xf& world,bool shadow) {
    if(!Ready()){if(ready)Fallback(shadow,"component released");return false;}
    {perf::Scope t(skin_time);if(!skin_mesh::Deform(mesh,binding,pose,&frame)){Fallback(shadow,"invalid solved pose");return false;}}
    // Frame arrays keep their sizes and allocations after Prepare. These
    // two known native inputs consume their borrowed views synchronously.
    {perf::Scope t(engine_time);if(!update||!update->Invoke()||!Place(world)){Fallback(shadow,"mesh update failed");return false;}}
    if(last_shadow!=int(shadow)) {Bool(component,"SetCastShadow","NewCastShadow",shadow);Bool(original,"SetCastShadow","NewCastShadow",false);last_shadow=int(shadow);}
    if(!visible){Visibility(component,true);Visibility(original,false);Bool(original,"SetCastShadow","NewCastShadow",false);visible=true;log::Info("body skin: refined mesh visible; original posed rig retained for IK and gear");}
    return true;
}
} // namespace holevr::vr::skin_model
