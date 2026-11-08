#include "vr/watch_marshal.h"
#include <array>
#include <cstdio>
#include <string>
using namespace holevr::vr::watch::marshal;
namespace {
int checks=0,failures=0;
#define EXPECT(c) do { ++checks; if(!(c)){++failures;std::printf("FAIL line %d: %s\n",__LINE__,#c);} } while(0)
constexpr std::uint64_t kObservedFilename=0x18000000000280;
Input Filename(std::uint64_t flags=kObservedFilename) { return {"ImportFileAsTexture2D","Filename","StrProperty",16,1,flags}; }
Input Mesh(std::string_view name="Vertices",std::uint64_t flags=0x08000182) {
    return {"CreateMeshSection_LinearColor",name,"ArrayProperty",16,1,flags};
}
void RuntimeContract() {
    // Exact flags from HOLE's failing import, not a guessed ConstParm fixture.
    EXPECT(ReadOnlyInput(Filename()));
    EXPECT(ReadOnlyInput(Filename(0x82))); // const plain input
    EXPECT(ReadOnlyInput(Filename(0x08000182))); // explicit const reference input
    for(std::uint64_t flags:{0ull,0x2ull,0x100ull,0x180ull,0x182ull,
                            0x480ull,0x482ull,0x580ull,0x08000080ull,0x08000180ull}) {
        EXPECT(!ReadOnlyInput(Filename(flags))); // non-params, outputs, returns and mutable refs
        EXPECT(!ReadOnlyInput(Mesh("Vertices",flags)));
    }
    // Return wins even if const/reference bits also happen to be set.
    for(std::uint64_t mask=0;mask<8;++mask) {
        auto flags=std::uint64_t{0x480}|((mask&1)?0x2:0)|((mask&2)?0x100:0)|((mask&4)?0x08000000:0);
        EXPECT(!ReadOnlyInput(Filename(flags)));
        EXPECT(!ReadOnlyInput(Mesh("Vertices",flags)));
    }
    for(const auto name:{"Vertices","Triangles","Normals","UV0","UV1","UV2","UV3","VertexColors","Tangents"}) {
        EXPECT(ReadOnlyInput(Mesh(name))); // UE const refs carry OutParm as well
        EXPECT(ReadOnlyInput(Mesh(name,0x82)));
        EXPECT(!ReadOnlyInput(Mesh(name,kObservedFilename))); // filename exception is not generic
    }
    auto update=Mesh();update.function="UpdateMeshSection_LinearColor";EXPECT(ReadOnlyInput(update));
    update.name="Triangles";EXPECT(!ReadOnlyInput(update));
    auto other=Filename();other.function="UnreviewedFunction";EXPECT(!ReadOnlyInput(other));
    other=Filename();other.name="ReturnValue";EXPECT(!ReadOnlyInput(other));
    other=Filename();other.type="ArrayProperty";EXPECT(!ReadOnlyInput(other));
    other=Mesh();other.name="OutputArray";EXPECT(!ReadOnlyInput(other));
    for(int size:{0,8,15,17,32}) {other=Filename();other.element_size=size;EXPECT(!ReadOnlyInput(other));}
    for(int dim:{0,2,8}) {other=Filename();other.array_dim=dim;EXPECT(!ReadOnlyInput(other));}
}
void BorrowedLifetimeAndFrame() {
    const std::u16string path=u"D:\\HOLE\\hole-vr\\watch\\dial.png";
    std::array<unsigned char,32> frame;frame.fill(0xa5);
    EXPECT(WriteInputView(Filename(),frame.data()+8,16,path.c_str(),static_cast<int>(path.size()+1)));
    View view;std::memcpy(&view,frame.data()+8,sizeof(view));
    EXPECT(view.data==path.c_str() && view.count==static_cast<int>(path.size()+1) && view.capacity==view.count);
    EXPECT(static_cast<const char16_t*>(view.data)[view.count-1]==u'\0');
    for(int i=0;i<8;++i) EXPECT(frame[i]==0xa5 && frame[i+24]==0xa5);
    // A synchronous consumer can read/copy the filename while its owner lives.
    // This is not a simulated Unreal runtime or an async lifetime claim.
    const std::u16string consumed(static_cast<const char16_t*>(view.data));EXPECT(consumed==path);
    const auto before=frame;
    EXPECT(!WriteInputView(Filename(0x180),frame.data()+8,16,path.c_str(),5));
    EXPECT(!WriteInputView(Filename(),frame.data()+8,8,path.c_str(),5));
    EXPECT(!WriteInputView(Filename(),frame.data()+8,16,nullptr,5));
    EXPECT(!WriteInputView(Filename(),frame.data()+8,16,path.c_str(),-1));
    EXPECT(!WriteInputView(Filename(),frame.data()+8,16,path.c_str(),0));
    EXPECT(!WriteInputView(Filename(),nullptr,16,path.c_str(),5));
    EXPECT(frame==before); // rejected contracts never mutate the parameter frame
    EXPECT(WriteInputView(Mesh("Tangents"),frame.data()+8,16,nullptr,0));
    std::memcpy(&view,frame.data()+8,sizeof(view));EXPECT(!view.data && view.count==0 && view.capacity==0);
}
}
int main(){RuntimeContract();BorrowedLifetimeAndFrame();std::printf("watch marshal: %d checks, %d failures\n",checks,failures);return failures?1:0;}
