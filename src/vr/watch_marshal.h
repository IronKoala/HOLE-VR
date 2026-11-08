#pragma once
// Borrowed views for this watch's three known synchronous native functions.
// Not a general FString/TArray ownership implementation for ue::Call.
#include <cstdint>
#include <cstring>
#include <string_view>

namespace holevr::vr::watch::marshal {
struct View { const void* data=nullptr; std::int32_t count=0,capacity=0; };
static_assert(sizeof(View)==16);

struct Input {
    std::string_view function,name,type;
    std::int32_t element_size=0,array_dim=1;
    std::uint64_t flags=0;
};

inline bool ReadOnlyInput(const Input& in) {
    constexpr std::uint64_t kConst=0x2, kParm=0x80, kOut=0x100,
                            kReturn=0x400, kReference=0x08000000;
    if(in.element_size!=sizeof(View) || in.array_dim!=1 || !(in.flags&kParm) || (in.flags&kReturn)) return false;
    const bool reference=(in.flags&kReference)!=0, constant=(in.flags&kConst)!=0;
    // UE const-reference inputs can also carry OutParm. That combination is
    // read-only; a mutable reference or a standalone output is not.
    if(reference && !constant) return false;
    if((in.flags&kOut) && !(reference&&constant)) return false;
    if(in.function=="ImportFileAsTexture2D" && in.name=="Filename" && in.type=="StrProperty") {
        // UE5.5 C++ takes const FString&, but HOLE reflects this string as a
        // plain input: 0x18000000000280 (Parm|ZeroConstructor, no ConstParm).
        // This exception is limited to the documented filename input.
        return true;
    }
    const bool create=in.function=="CreateMeshSection_LinearColor";
    if(!create && in.function!="UpdateMeshSection_LinearColor") return false;
    if(in.type!="ArrayProperty" || !constant) return false;
    return in.name=="Vertices" || in.name=="Normals" || in.name=="UV0" ||
           in.name=="UV1" || in.name=="UV2" || in.name=="UV3" ||
           in.name=="VertexColors" || in.name=="Tangents" || (create&&in.name=="Triangles");
}

inline bool WriteInputView(const Input& in,void* parameter,std::int32_t parameter_size,
                           const void* data,std::int32_t count) {
    if(!ReadOnlyInput(in) || !parameter || parameter_size!=sizeof(View) || count<0 || (!data&&count>0)) return false;
    if(in.type=="StrProperty" && count==0) return false; // filename includes its terminator
    const View view{data,count,count};
    std::memcpy(parameter,&view,sizeof(view));
    return true;
}
} // namespace holevr::vr::watch::marshal
