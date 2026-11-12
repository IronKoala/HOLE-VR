#include "vr/body/skin_mesh.h"
#include "vr/body/arm_pose.h"
#include "vr/watch_math.h"
#include "vr/watch_mesh_data.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
using namespace holevr::vr;
static void Check(bool b,const char* msg){if(!b){std::printf("FAIL %s\n",msg);std::exit(1);}}
static std::vector<uint8_t> Read(const char* path){std::ifstream f(path,std::ios::binary|std::ios::ate);Check(bool(f),"file");std::vector<uint8_t> b(size_t(f.tellg()));f.seekg(0);f.read(reinterpret_cast<char*>(b.data()),b.size());Check(bool(f),"read");return b;}
template<class T>static T Item(const std::vector<uint8_t>& b,size_t& at){Check(at+sizeof(T)<=b.size(),"bounds");T v;std::memcpy(&v,b.data()+at,sizeof(T));at+=sizeof(T);return v;}
static skel::Xf Xf(const std::vector<uint8_t>& b,size_t& at){auto v=Item<std::array<double,10>>(b,at);skel::Xf t;t.rotation={v[0],v[1],v[2],v[3]};t.translation={v[4],v[5],v[6]};t.scale={v[7],v[8],v[9]};return t;}
int main(int argc,char** argv){
    Check(argc==3,"fixture arguments");skin_mesh::Mesh mesh;std::string error;Check(skin_mesh::Decode(Read(argv[1]),&mesh,&error),error.c_str());
    std::vector<std::string> names;std::vector<int> parents;std::vector<skel::Xf> local;int low=-1,helper=-1,hand=-1;
    for(size_t i=0;i<mesh.bones.size();++i){const auto& b=mesh.bones[i];names.push_back(b.name);parents.push_back(b.parent);local.push_back(b.local);if(b.name=="lowerarm_l")low=int(i);if(b.name=="lowerarm_twist_01_l")helper=int(i);if(b.name=="hand_l")hand=int(i);}
    Check(low>=0&&helper>=0&&hand>=0,"wrist bones");skel::Pose reference,pose;Check(reference.Init(parents)&&pose.Init(parents),"rig");reference.SetAllLocal(local.data(),int(local.size()));
    skin_mesh::Binding binding;skin_mesh::Frame frame;Check(skin_mesh::Bind(mesh,names,parents,local,&binding),"bind");
    auto f=Read(argv[2]);size_t at=0;auto magic=Item<std::array<char,8>>(f,at);Check(!std::memcmp(magic.data(),"HWWFIT6\0",8),"fit fixture format");
    auto cases=Item<uint32_t>(f,at),bones=Item<uint32_t>(f,at),points=Item<uint32_t>(f,at);Check(bones==mesh.bones.size()&&points==mesh.points.size(),"sizes");double worst=0,mount_error=0;
    for(uint32_t c=0;c<cases;++c){
        auto label=Item<std::array<char,32>>(f,at);pose.SetAllLocal(local.data(),int(local.size()));
        for(uint32_t b=0;b<bones;++b)pose.SetC(int(b),Xf(f,at));const auto expected_mount=Xf(f,at);
        watch::Forearm arm;arm.ref_lower=reference.C(low);arm.ref_hand=reference.C(hand);arm.ref_twist=reference.C(helper);arm.has_twist=true;
        arm.lower=pose.C(low);arm.lower.scale={1,1,1};arm.hand=pose.C(hand);arm.twist=pose.C(helper);
        const auto mount=watch::Mount(arm,watch::mesh::kReferenceMount,watch::mesh::kTwistSkinWeight);
        mount_error=std::max(mount_error,ik::Length(mount.translation-expected_mount.translation));
        Check(ik::Length(mount.translation-expected_mount.translation)<.0001,"watch stays at fitted wrist location");
        for(FVector axis: {FVector{1,0,0},FVector{0,1,0},FVector{0,0,1}})Check(ik::Length(mount.rotation.Rotate(axis)-expected_mount.rotation.Rotate(axis))<1e-6,"watch follows fitted full wrist rotation");
        const auto attach=arm_pose::MeasureAttachment(arm.ref_lower,arm.ref_hand,arm.ref_twist);auto full=pose.C(low);
        full.rotation=pose.C(helper).rotation*(arm.ref_lower.rotation.Inverse()*arm.ref_twist.rotation).Inverse();skel::Xf placed;
        Check(arm_pose::PlaceAttachment(attach,full,arm.hand.translation,&placed),"native calibrated helper placement");
        Check(ik::Length(placed.translation-pose.C(helper).translation)<.0001,"authoring matches actual helper placement");
        Check(skin_mesh::Deform(mesh,binding,pose,&frame),"calibrated skin");double e=0;
        for(uint32_t p=0;p<points;++p){const auto v=Item<std::array<float,3>>(f,at);const auto got=mount.rotation.Inverse().Rotate(frame.positions[p]-mount.translation);e=std::max(e,ik::Length(got-FVector{v[0],v[1],v[2]}));}
        worst=std::max(worst,e);Check(e<.002,"native watch/skin clearance uses checked model pose");
        std::printf("%s %.8f cm\n",label.data(),e);
    }
    Check(at==f.size(),"all fit fixtures consumed");std::printf("PASS %u calibrated fit poses, skin discrepancy %.8f cm, mount discrepancy %.8f cm\n",cases,worst,mount_error);
}
