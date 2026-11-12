#pragma once
// Local approved WhiteMan mesh, driven by the existing solved pose. No IK.
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "vr/body/skeleton.h"

namespace holevr::vr::skin_mesh {
struct UV { double u=0,v=0; };
struct Bone { std::string name; int parent=-1; skel::Xf local; };
struct Point { FVector position; std::array<uint16_t,4> bone{}; std::array<double,4> weight{}; };
struct Corner { uint32_t point=0; FVector normal; UV uv; bool flat=false; };
struct Mesh { uint32_t content_id=0; std::vector<Bone> bones; std::vector<Point> points; std::vector<Corner> corners; };
struct Matrix { double m[3][4]{}; };
struct Binding { std::vector<int> bones; std::vector<Matrix> inverse_rest; };
struct Frame {
    std::vector<Matrix> bones,points;
    std::vector<FVector> positions,vertices,normals;
};
bool Decode(std::span<const uint8_t> bytes,Mesh* result,std::string* error=nullptr);
bool Bind(const Mesh& mesh,const std::vector<std::string>& names,const std::vector<int>& parents,
          const std::vector<skel::Xf>& reference_local,Binding* result,std::string* error=nullptr);
bool Deform(const Mesh& mesh,const Binding& binding,const skel::Pose& pose,Frame* frame);
} // namespace holevr::vr::skin_mesh
