#include "vr/body/skin_mesh.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <numeric>
using namespace holevr::vr;
using namespace holevr::vr::skin_mesh;
static void Check(bool value,const char* msg) {if(!value){std::fprintf(stderr,"FAIL: %s\n",msg);std::exit(1);}}
static std::vector<uint8_t> Read(const char* file) {std::ifstream f(file,std::ios::binary|std::ios::ate);Check(bool(f),"file exists");std::vector<uint8_t> b(size_t(f.tellg()));f.seekg(0);f.read(reinterpret_cast<char*>(b.data()),b.size());Check(bool(f),"file read");return b;}
template<class T>static T Item(const std::vector<uint8_t>& b,size_t& at) {Check(at+sizeof(T)<=b.size(),"fixture bounds");T v;std::memcpy(&v,b.data()+at,sizeof(v));at+=sizeof(v);return v;}
static void Hash(std::vector<uint8_t>& b) {uint32_t h=2166136261u;for(size_t i=32;i<b.size();++i)h=(h^b[i])*16777619u;std::memcpy(b.data()+28,&h,4);}
int main(int argc,char** argv) {
    Check(argc==3,"mesh and independent Blender fixtures required");const auto bytes=Read(argv[1]);Mesh m;std::string error;
    Check(Decode(bytes,&m,&error),error.c_str());Check(m.bones.size()==68&&m.corners.size()==34014,"approved export counts");
    std::vector<std::string> names;std::vector<int> parents;std::vector<skel::Xf> local;
    for(const auto& b:m.bones){names.push_back(b.name);parents.push_back(b.parent);local.push_back(b.local);}
    Binding binding;Check(Bind(m,names,parents,local,&binding,&error),error.c_str());
    skel::Pose pose;Check(pose.Init(parents),"pose init");pose.SetAllLocal(local.data(),int(local.size()));Frame frame;
    Check(Deform(m,binding,pose,&frame),"rest skin");double rest_error=0;
    for(size_t i=0;i<m.points.size();++i)rest_error=std::max(rest_error,ik::Length(frame.positions[i]-m.points[i].position));
    Check(rest_error<1e-6,"neutral geometry preserved");
    for(const auto& n:frame.normals)Check(std::fabs(ik::Length(n)-1)<1e-4,"unit normals");
    for(size_t i=0;i<m.corners.size();i+=3)if(m.corners[i].flat) {
        const auto outward=ik::Cross(frame.vertices[i+2]-frame.vertices[i],frame.vertices[i+1]-frame.vertices[i]);
        if(ik::Length(outward)>1e-8)Check(ik::Dot(ik::Normalize(outward),frame.normals[i])>.999,"Unreal clockwise winding and outward normals");
    }
    auto bad=bytes;bad.pop_back();Check(!Decode(bad,&m),"reject truncation without changing mesh");Check(m.bones.size()==68,"failed decode is atomic");
    bad=bytes;bad.back()^=1;Check(!Decode(bad,&m),"reject corruption");
    bad=bytes;const size_t corners=32+m.bones.size()*148+m.points.size()*36;uint32_t illegal=uint32_t(m.points.size());std::memcpy(bad.data()+corners,&illegal,4);Hash(bad);Check(!Decode(bad,&m),"reject out-of-range point index");
    bad=bytes;uint16_t bone=1000;std::memcpy(bad.data()+32+m.bones.size()*148+12,&bone,2);Hash(bad);Check(!Decode(bad,&m),"reject out-of-range bone");
    auto wrong=local;wrong[1].translation.X+=1;Check(!Bind(m,names,parents,wrong,&binding),"reject different reference skeleton");
    auto wrong_names=names;wrong_names[4]="other_bone";Check(!Bind(m,wrong_names,parents,local,&binding),"reject missing named bone");
    Check(Bind(m,names,parents,local,&binding),"restore binding");
    const auto fixture=Read(argv[2]);size_t at=0;using Magic=std::array<char,8>;const auto magic=Item<Magic>(fixture,at);Check(!std::memcmp(magic.data(),"HWPOSE3\0",8),"fixture magic");
    const auto cases=Item<uint32_t>(fixture,at),bones=Item<uint32_t>(fixture,at),points=Item<uint32_t>(fixture,at);Check(bones==m.bones.size()&&points==m.points.size(),"fixture counts");
    double maximum=0;
    for(uint32_t c=0;c<cases;++c) {
        auto label=Item<std::array<char,32>>(fixture,at);pose.SetAllLocal(local.data(),int(local.size()));
        for(uint32_t b=0;b<bones;++b){auto t=Item<std::array<double,10>>(fixture,at);skel::Xf x;x.rotation={t[0],t[1],t[2],t[3]};x.translation={t[4],t[5],t[6]};x.scale={t[7],t[8],t[9]};pose.SetC(int(b),x);}
        Check(Deform(m,binding,pose,&frame),"deform Blender pose");double e=0;
        for(uint32_t p=0;p<points;++p){auto v=Item<std::array<float,3>>(fixture,at);e=std::max(e,ik::Length(frame.positions[p]-FVector{v[0],v[1],v[2]}));}
        maximum=std::max(maximum,e);Check(e<.002,"native skin agrees with independently evaluated Blender mesh");
        std::printf("%s maximum %.8f cm\n",label.data(),e);
    }
    Check(at==fixture.size(),"fixture fully consumed");
    // Calibrated segment stretching and small hidden-head scales must remain
    // finite. Finger transforms are still the existing pose, never re-solved.
    pose.SetAllLocal(local.data(),int(local.size()));auto hidden=pose.L(6);hidden.scale={.001,.001,.001};pose.SetL(6,hidden);Check(Deform(m,binding,pose,&frame),"hidden bone scale");
    for(auto p:frame.vertices)Check(std::isfinite(p.X)&&std::isfinite(p.Y)&&std::isfinite(p.Z),"finite hidden pose");
    const auto start=std::chrono::steady_clock::now();for(int i=0;i<300;++i)Check(Deform(m,binding,pose,&frame),"benchmark deform");
    const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/300;
    auto invalid=pose.C(4);invalid.translation.X=std::numeric_limits<double>::quiet_NaN();pose.SetC(4,invalid);Check(!Deform(m,binding,pose,&frame),"reject invalid pose");
    std::printf("PASS: %u independent Blender poses, worst %.8f cm, native math %.3f ms/frame (engine upload excluded)\n",cases,maximum,ms);
}
