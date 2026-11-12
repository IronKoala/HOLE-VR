#include "vr/body/skin_mesh.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>

namespace holevr::vr::skin_mesh {
namespace {
bool Fail(std::string* error,const char* why) { if(error)*error=why;return false; }
// Unreal bone names (FName) ignore letter case: the live WhiteMan rig calls the
// head bone "Head" while the approved export has "head". Exact matching made
// every in-game bind fail ("rig bone missing", runs of 2026-11-12).
std::string Folded(std::string s) { for(char& c:s) c=char(std::tolower(static_cast<unsigned char>(c))); return s; }
bool Finite(const FVector& p) { return std::isfinite(p.X)&&std::isfinite(p.Y)&&std::isfinite(p.Z); }
bool Valid(const skel::Xf& p) {
    const auto& q=p.rotation;const double n=q.X*q.X+q.Y*q.Y+q.Z*q.Z+q.W*q.W;
    return Finite(p.translation)&&Finite(p.scale)&&std::isfinite(n)&&std::fabs(n-1)<.002 &&
        ik::Length(p.translation)<1e6 && std::max({std::fabs(p.scale.X),std::fabs(p.scale.Y),std::fabs(p.scale.Z)})<100;
}
struct Reader {
    std::span<const uint8_t> bytes; size_t at=0;
    template<class T> bool Read(T* p) {
        if(at>bytes.size() || sizeof(T)>bytes.size()-at)return false;
        std::memcpy(p,bytes.data()+at,sizeof(T));at+=sizeof(T);return true;
    }
};
Matrix Transform(const skel::Xf& p) {
    const FVector x=p.rotation.Rotate({p.scale.X,0,0}),y=p.rotation.Rotate({0,p.scale.Y,0}),z=p.rotation.Rotate({0,0,p.scale.Z});
    return {{{x.X,y.X,z.X,p.translation.X},{x.Y,y.Y,z.Y,p.translation.Y},{x.Z,y.Z,z.Z,p.translation.Z}}};
}
Matrix Multiply(const Matrix& a,const Matrix& b) {
    Matrix r;
    for(int i=0;i<3;++i)for(int j=0;j<4;++j) {
        for(int k=0;k<3;++k)r.m[i][j]+=a.m[i][k]*b.m[k][j];
        if(j==3)r.m[i][j]+=a.m[i][3];
    }
    return r;
}
FVector Apply(const Matrix& m,const FVector& p) {
    return {m.m[0][0]*p.X+m.m[0][1]*p.Y+m.m[0][2]*p.Z+m.m[0][3],
            m.m[1][0]*p.X+m.m[1][1]*p.Y+m.m[1][2]*p.Z+m.m[1][3],
            m.m[2][0]*p.X+m.m[2][1]*p.Y+m.m[2][2]*p.Z+m.m[2][3]};
}
Matrix InverseRest(const skel::Xf& p) {
    const auto q=p.rotation.Inverse();
    const auto x=q.Rotate({1,0,0}),y=q.Rotate({0,1,0}),z=q.Rotate({0,0,1});
    Matrix m{{{x.X/p.scale.X,y.X/p.scale.X,z.X/p.scale.X,0},
              {x.Y/p.scale.Y,y.Y/p.scale.Y,z.Y/p.scale.Y,0},
              {x.Z/p.scale.Z,y.Z/p.scale.Z,z.Z/p.scale.Z,0}}};
    const auto t=Apply(m,p.translation)*-1;m.m[0][3]=t.X;m.m[1][3]=t.Y;m.m[2][3]=t.Z;return m;
}
FVector Normal(const Matrix& a,const FVector& n) {
    // Cofactors are the inverse transpose up to determinant. Preserve its
    // sign for mirrored transforms; zero-size hidden bones use the fallback.
    const FVector x{a.m[0][0],a.m[1][0],a.m[2][0]},y{a.m[0][1],a.m[1][1],a.m[2][1]},z{a.m[0][2],a.m[1][2],a.m[2][2]};
    const auto cx=ik::Cross(y,z),cy=ik::Cross(z,x),cz=ik::Cross(x,y);
    const double det=ik::Dot(x,cx);const auto v=cx*n.X+cy*n.Y+cz*n.Z;
    return std::fabs(det)>1e-16&&ik::Length(v)>1e-15 ? ik::Normalize(v)*(det<0?-1:1) : n;
}
}
bool Decode(std::span<const uint8_t> bytes,Mesh* result,std::string* error) {
    if(!result)return Fail(error,"missing destination");
    Reader r{bytes};char magic[8];uint32_t version,nb,np,nc,size,checksum;
    if(!r.Read(&magic)||std::memcmp(magic,"HWRIST3\0",8)||!r.Read(&version)||version!=3||
       !r.Read(&nb)||!r.Read(&np)||!r.Read(&nc)||!r.Read(&size)||!r.Read(&checksum))return Fail(error,"invalid skin header");
    if(nb<1||nb>256||np<3||np>50000||nc<3||nc>120000||nc%3||size!=bytes.size()-r.at||
       uint64_t(size)!=uint64_t(nb)*148+uint64_t(np)*36+uint64_t(nc)*28)return Fail(error,"invalid skin sizes");
    uint32_t hash=2166136261u;for(size_t i=r.at;i<bytes.size();++i)hash=(hash^bytes[i])*16777619u;
    if(hash!=checksum)return Fail(error,"skin checksum differs");
    Mesh m;m.content_id=checksum;m.bones.resize(nb);m.points.resize(np);m.corners.resize(nc);
    std::map<std::string,int> names;
    for(uint32_t i=0;i<nb;++i) {
        char name[64];int32_t parent;double t[10];
        if(!r.Read(&name)||!r.Read(&parent)||!r.Read(&t)||!std::memchr(name,0,64))return Fail(error,"invalid bone record");
        auto& b=m.bones[i];b.name=name;b.parent=parent;b.local.rotation={t[0],t[1],t[2],t[3]};b.local.translation={t[4],t[5],t[6]};b.local.scale={t[7],t[8],t[9]};
        if(b.name.empty()||!names.emplace(b.name,i).second||parent>=int(i)||parent< -1||(i>0&&parent<0)||
           !Valid(b.local)||std::min({t[7],t[8],t[9]})<1e-6)return Fail(error,"invalid reference rig");
    }
    for(auto& p:m.points) {
        float xyz[3],w[4];uint16_t ids[4];
        if(!r.Read(&xyz)||!r.Read(&ids)||!r.Read(&w))return Fail(error,"truncated points");
        p.position={xyz[0],xyz[1],xyz[2]};double sum=0;
        if(!Finite(p.position)||ik::Length(p.position)>10000)return Fail(error,"invalid point position");
        for(int j=0;j<4;++j) {if(ids[j]>=nb||!std::isfinite(w[j])||w[j]<0||w[j]>1.00001)return Fail(error,"invalid skin influence");p.bone[j]=ids[j];p.weight[j]=w[j];sum+=w[j];}
        if(std::fabs(sum-1)>.0001)return Fail(error,"skin weights do not sum to one");
        for(auto& w:p.weight)w/=sum;
    }
    for(auto& c:m.corners) {
        float n[3],uv[2];uint32_t flat;
        if(!r.Read(&c.point)||!r.Read(&n)||!r.Read(&uv)||!r.Read(&flat)||c.point>=np||flat>1)return Fail(error,"invalid render vertex");
        c.normal={n[0],n[1],n[2]};c.uv={uv[0],uv[1]};c.flat=flat;
        if(!Finite(c.normal)||std::fabs(ik::Length(c.normal)-1)>.01||!std::isfinite(uv[0])||!std::isfinite(uv[1]))return Fail(error,"invalid normal or UV");
    }
    for(size_t i=0;i<m.corners.size();i+=3)if(m.corners[i].flat!=m.corners[i+1].flat||m.corners[i].flat!=m.corners[i+2].flat)return Fail(error,"inconsistent face normals");
    if(r.at!=bytes.size())return Fail(error,"trailing skin bytes");
    *result=std::move(m);return true;
}
bool Bind(const Mesh& mesh,const std::vector<std::string>& names,const std::vector<int>& parents,
          const std::vector<skel::Xf>& local,Binding* result,std::string* error) {
    if(!result||names.size()!=parents.size()||names.size()!=local.size()||mesh.bones.size()!=names.size())return Fail(error,"rig count differs");
    skel::Pose live,source;std::vector<int> sp;std::vector<skel::Xf> sl;
    for(const auto& b:mesh.bones){sp.push_back(b.parent);sl.push_back(b.local);}
    if(!live.Init(parents)||!source.Init(sp))return Fail(error,"invalid rig hierarchy");
    live.SetAllLocal(local.data(),int(local.size()));source.SetAllLocal(sl.data(),int(sl.size()));
    std::map<std::string,int> lookup;for(size_t i=0;i<names.size();++i)if(!lookup.emplace(Folded(names[i]),i).second)return Fail(error,"duplicate rig names");
    Binding binding;
    for(size_t i=0;i<mesh.bones.size();++i) {
        const auto& b=mesh.bones[i];const auto found=lookup.find(Folded(b.name));if(found==lookup.end())return Fail(error,"rig bone missing");
        int j=found->second;const std::string expected=b.parent<0?"":mesh.bones[b.parent].name;
        if(Folded(parents[j]<0?"":names[parents[j]])!=Folded(expected))return Fail(error,"rig parent differs");
        const auto& a=source.C(int(i));const auto& c=live.C(j);const auto dq=a.rotation.Inverse()*c.rotation;
        if(!Valid(c)||ik::Length(a.translation-c.translation)>.02||std::fabs(dq.W)<.99999||ik::Length(a.scale-c.scale)>.001)return Fail(error,"reference rig differs");
        binding.bones.push_back(j);binding.inverse_rest.push_back(InverseRest(a));
    }
    *result=std::move(binding);return true;
}
bool Deform(const Mesh& mesh,const Binding& binding,const skel::Pose& pose,Frame* f) {
    if(!f||binding.bones.size()!=mesh.bones.size()||binding.inverse_rest.size()!=mesh.bones.size())return false;
    f->bones.resize(mesh.bones.size());f->points.resize(mesh.points.size());f->positions.resize(mesh.points.size());
    f->vertices.resize(mesh.corners.size());f->normals.resize(mesh.corners.size());
    for(size_t i=0;i<f->bones.size();++i) {
        const int j=binding.bones[i];if(j<0||j>=pose.Count()||!Valid(pose.C(j)))return false;
        f->bones[i]=Multiply(Transform(pose.C(j)),binding.inverse_rest[i]);
    }
    for(size_t i=0;i<mesh.points.size();++i) {
        const auto& p=mesh.points[i];Matrix m;
        for(int j=0;j<4;++j)if(p.weight[j]>0)for(int a=0;a<3;++a)for(int b=0;b<4;++b)m.m[a][b]+=p.weight[j]*f->bones[p.bone[j]].m[a][b];
        f->points[i]=m;f->positions[i]=Apply(m,p.position);if(!Finite(f->positions[i]))return false;
    }
    for(size_t i=0;i<mesh.corners.size();++i) {
        const auto& c=mesh.corners[i];f->vertices[i]=f->positions[c.point];
        f->normals[i]=c.flat?c.normal:Normal(f->points[c.point],c.normal);
    }
    for(size_t i=0;i<mesh.corners.size();i+=3)if(mesh.corners[i].flat) {
        const auto v=ik::Cross(f->vertices[i+1]-f->vertices[i],f->vertices[i+2]-f->vertices[i]);
        // The reflected Blender export uses UE clockwise front faces, as
        // do the already installed watch, vise and holster meshes.
        const auto n=ik::Length(v)>1e-12?ik::Normalize(v)*-1:mesh.corners[i].normal;
        f->normals[i]=f->normals[i+1]=f->normals[i+2]=n;
    }
    return true;
}
} // namespace holevr::vr::skin_mesh
