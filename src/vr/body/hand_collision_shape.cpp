#include "vr/body/hand_collision_shape.h"
#include "vr/body/finger_solve.h"
#include <algorithm>
#include <cmath>

namespace holevr::vr::hand_collision_shape {
namespace {
double MaximumScale(const FVector& scale) {
    return std::max({std::fabs(scale.X),std::fabs(scale.Y),std::fabs(scale.Z)});
}
FVector Scaled(const FVector& a,const FVector& b) { return {a.X*b.X,a.Y*b.Y,a.Z*b.Z}; }
}
Palm TransformPalm(const Palm& local, const skel::Xf& hand) {
    const auto& scale = hand.scale;
    const auto& q = hand.rotation;
    const double q2 = q.X*q.X + q.Y*q.Y + q.Z*q.Z + q.W*q.W;
    if (!local.fitted || !std::isfinite(q2) || q2 < .99 || q2 > 1.01 ||
        !std::isfinite(ik::Length(scale)) || !std::isfinite(ik::Length(hand.translation)) ||
        std::min({std::fabs(scale.X), std::fabs(scale.Y), std::fabs(scale.Z)}) < 1e-6 ||
        !std::isfinite(ik::Length(local.centre)) || !std::isfinite(ik::Length(local.half)) ||
        std::min({local.half.X, local.half.Y, local.half.Z}) <= 0) return {};
    for (const auto& axis : local.axis) {
        const double length = ik::Length(axis);
        if (!std::isfinite(length) || std::fabs(length - 1.0) > .001) return {};
    }
    if (ik::Dot(ik::Cross(local.axis[0], local.axis[1]), local.axis[2]) < .999) return {};

    const auto rotation = ik::Normalized(q);
    const auto vector = [&](const FVector& v) { return rotation.Rotate(Scaled(scale, v)); };
    const auto& normal = local.axis[2];
    Palm world;
    world.centre = hand.translation + vector(local.centre);
    // Preserve the palm-facing surface normal through anisotropic scale.
    world.axis[2] = ik::Normalize(rotation.Rotate({normal.X/scale.X, normal.Y/scale.Y, normal.Z/scale.Z}));
    world.axis[1] = ik::Normalize(ik::Reject(vector(local.axis[1]), world.axis[2]));
    world.axis[0] = ik::Normalize(ik::Cross(world.axis[1], world.axis[2]));
    const FVector edges[] = {vector(local.axis[0]*local.half.X), vector(local.axis[1]*local.half.Y),
                             vector(local.axis[2]*local.half.Z)};
    const auto extent = [&](const FVector& axis) {
        return std::fabs(ik::Dot(edges[0],axis)) + std::fabs(ik::Dot(edges[1],axis)) + std::fabs(ik::Dot(edges[2],axis));
    };
    world.half = {extent(world.axis[0]), extent(world.axis[1]), extent(world.axis[2])};
    world.fitted = std::isfinite(ik::Length(world.centre)) && std::isfinite(ik::Length(world.half));
    return world;
}
WorldSection Transform(const Segment& shape,const skel::Xf& joint,const skel::Xf& hand,double fallback) {
    WorldSection result;
    result.radius_cm=fallback;
    if(!shape.fitted) return result;
    auto vector=[&](const FVector& axis,double half) {
        return hand.rotation.Rotate(Scaled(hand.scale,joint.rotation.Rotate(Scaled(joint.scale,axis*half))));
    };
    result.fitted=true;
    result.radius_cm=shape.world_radius_cm*MaximumScale(joint.scale)*MaximumScale(hand.scale);
    result.side_half=vector(shape.side_axis,shape.side_half_cm);
    result.depth_half=vector(shape.depth_axis,shape.depth_half_cm);
    result.axial_half=vector(shape.axial_axis,shape.axial_half_cm);
    result.self_offset=vector(shape.self_offset,1.0);
    result.self_start_fraction=shape.self_start_fraction;
    return result;
}
double Support(const WorldSection& section,const FVector& normal) {
    if(!section.fitted) return section.radius_cm;
    const double side=ik::Dot(normal,section.side_half),depth=ik::Dot(normal,section.depth_half),
                 axial=ik::Dot(normal,section.axial_half);
    return std::sqrt(side*side+depth*depth+axial*axial);
}
bool FitWhiteMan(fingers::Chain chains[5],const FVector& palm_normal,Palm* palm) {
    if(palm) *palm={};
    for(int f=0;f<5;++f) for(auto& shape:chains[f].collision) shape={};
    // Asset fingerprint in native hand coordinates. Length and all five
    // knuckles must agree; a similarly named unknown avatar is not enough.
    static constexpr FVector knuckle[5]={{4.7620473,-2.3749475,-2.5378127},
        {12.068104,-1.7634194,-2.1093943},{12.244298,-1.293615,.571164},
        {11.497902,-1.7534872,2.8469179},{10.140665,-2.2631252,4.643146}};
    static constexpr double length[5][3]={{3.8697,4.0622,3.2497},{4.2875,3.3938,2.7150},
        {4.6404,3.6489,2.9191},{4.4302,3.4767,2.7814},{3.5710,2.9856,2.3885}};
    const int side=chains[0].ref[0].translation.X<0 ? 1 : 0;
    const double sign=side ? -1.0 : 1.0;
    skel::Xf reference[5][3]; FVector tip[5];
    for(int f=0;f<5;++f) {
        const double knuckle_error=ik::Length(chains[f].ref[0].translation-knuckle[f]*sign);
        if(!chains[f].valid || chains[f].thumb!=(f==0) ||
           !std::isfinite(knuckle_error) || knuckle_error>.03) return false;
        fingers::Fk(chains[f],{},reference[f],&tip[f]);
        for(int j=0;j<3;++j) {
            const auto end=j<2 ? reference[f][j+1].translation : tip[f];
            const double actual=ik::Length(end-reference[f][j].translation);
            if(!std::isfinite(actual) || std::fabs(actual-length[f][j])>.03) return false;
        }
    }
    const auto across=chains[1].ref[0].translation-chains[4].ref[0].translation;
    const auto middle=chains[2].ref[0].translation;
    const auto expected=ik::Normalize(side ? ik::Cross(middle,across) : ik::Cross(across,middle));
    const double agreement=ik::Dot(expected,ik::Normalize(palm_normal));
    if(!std::isfinite(agreement) || agreement<.999) return false;
    // Reference mesh body sections: >90% weight on this digit, dominant
    // matching phalanx, outside proximal web. Each measured side/depth box
    // was expanded by the maximum normalized elliptical radius of samples.
    // Two-hundredths centimeter covers rounded-table precision, not a tune
    // intended to force collision feasibility. Runtime calibration scales it.
    static constexpr double bone_half[2][5][3][2]={
        {{{2.551,2.103},{2.005,1.891},{1.341,1.387}},
         {{1.284,1.333},{1.226,1.184},{1.101,.992}},
         {{1.380,1.605},{1.307,1.434},{1.262,1.099}},
         {{1.280,1.623},{1.283,1.492},{1.057,1.130}},
         {{1.175,1.419},{1.107,1.193},{.914,.931}}},
        {{{2.601,2.219},{2.084,1.744},{1.253,1.455}},
         {{1.354,1.286},{1.274,1.295},{1.122,1.144}},
         {{1.325,1.436},{1.249,1.221},{1.334,1.202}},
         {{1.291,1.362},{1.261,1.193},{1.131,1.138}},
         {{1.119,1.324},{1.080,1.270},{.904,1.073}}}
    };
    // The bones are not exactly centred in the skin. These independently
    // enclosed ellipses use the centre of the measured cross-section box,
    // retaining its measured offset instead of inflating both sides equally.
    static constexpr double centered[2][5][3][4]={
        {{{-.080,-.010,2.464,2.093},{-.184,.159,1.869,1.779},{.260,.122,1.107,1.312}},
         {{-.069,.144,1.280,1.248},{-.086,.077,1.235,1.200},{-.029,.045,1.093,.964}},
         {{.054,.194,1.367,1.432},{.044,.218,1.280,1.216},{-.091,.089,1.171,1.010}},
         {{-.046,.215,1.207,1.352},{-.051,.231,1.179,1.178},{-.017,.142,1.046,.985}},
         {{-.027,.116,1.165,1.310},{-.036,.068,1.055,1.104},{.043,.069,.876,.865}}},
        {{{.254,-.225,2.464,2.093},{.305,-.078,1.880,1.766},{-.118,-.114,1.107,1.312}},
         {{.138,-.103,1.280,1.248},{.136,-.188,1.235,1.200},{.079,-.219,1.098,.968}},
         {{.006,-.047,1.367,1.432},{.012,-.044,1.280,1.216},{.146,-.163,1.158,1.010}},
         {{.092,-.027,1.207,1.352},{.094,-.034,1.179,1.178},{.047,-.123,1.050,.976}},
         {{.047,-.116,1.165,1.310},{.044,-.176,1.055,1.101},{-.012,-.188,.878,.857}}}
    };
    static constexpr double web_end[2][5]={ {1.044,.230,.184,.127,.310}, {1.046,.254,.210,.155,.352} };
    for(int f=0;f<5;++f) for(int j=0;j<3;++j) {
        auto& shape=chains[f].collision[j]; shape={}; shape.fitted=true;
        shape.side_half_cm=centered[side][f][j][2]+.02;
        shape.depth_half_cm=centered[side][f][j][3]+.02;
        // A rounded cap uses the smaller measured cross-section half-width;
        // it is not inferred from the artificial0.8bone fingertip extension.
        shape.axial_half_cm=std::min(shape.side_half_cm,shape.depth_half_cm);
        shape.world_radius_cm=std::max(bone_half[side][f][j][0],bone_half[side][f][j][1])+.02;
        const auto end=j<2 ? reference[f][j+1].translation : tip[f];
        const auto axial=ik::Normalize(end-reference[f][j].translation);
        const auto depth=ik::Normalize(ik::Reject(ik::Normalize(palm_normal),axial));
        const auto side_axis=ik::Normalize(ik::Cross(axial,depth));
        const auto inverse=reference[f][j].rotation.Inverse();
        shape.axial_axis=inverse.Rotate(axial); shape.depth_axis=inverse.Rotate(depth);
        shape.side_axis=inverse.Rotate(side_axis);
        shape.self_offset=shape.side_axis*centered[side][f][j][0]+shape.depth_axis*centered[side][f][j][1];
        if(j==0) shape.self_start_fraction=std::min(1.0,web_end[side][f]+shape.axial_half_cm/length[f][j]+.02);
        if(f==0 && j==1) shape.self_start_fraction=(web_end[side][0]-1)*length[0][0]/length[0][1]+
                                                   shape.axial_half_cm/length[0][1]+.02;
    }
    // The palm and CMC form one world envelope. The measured CMC radius
    // below covers the points outside the palm and other phalanx capsules,
    // with a .02 cm margin. Its self section retains the measured body fit.
    // Keep the full standalone CMC radius when a caller omits the palm.
    if(palm) {
        // Start with the 1113 hand-dominant vertices, then extend only to
        // web points outside all phalanx capsules. Weighted skinning was
        // sampled in reference, 30 controller goals and 40 self-resolved
        // poses per side. Values are native hand centimetres.
        static constexpr FVector centre[2]={{5.975526441,-1.903506535,.191068382},
                                            {-6.516373727,1.713252523,-.388764767}};
        static constexpr FVector half[2]={{5.252084046,6.804570730,3.492549217},
                                          {5.183716648,7.155440274,3.485355174}};
        static constexpr double cmc_radius[2]={2.261,2.112};
        chains[0].collision[0].world_radius_cm=cmc_radius[side];
        palm->fitted=true; palm->centre=centre[side];
        palm->axis[2]=ik::Normalize(palm_normal);
        palm->axis[1]=ik::Normalize(ik::Reject(chains[2].ref[0].translation,palm->axis[2]));
        palm->axis[0]=ik::Normalize(ik::Cross(palm->axis[1],palm->axis[2]));
        palm->half=half[side];
    }
    return true;
}
} // namespace holevr::vr::hand_collision_shape
