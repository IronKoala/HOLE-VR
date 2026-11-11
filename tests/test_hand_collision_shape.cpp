#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include "vr/body/hand_collision_shape.h"
#include "vr/body/thumb_pose.h"
#include "fixtures/white_man_hands.h"
#include "fixtures/white_man_shape_samples.h"

using namespace holevr::vr;
using namespace holevr::vr::ik;
namespace shape=hand_collision_shape;
static int failures=0;
#define EXPECT(x) do { if(!(x)) { std::printf("FAIL %d: %s\n",__LINE__,#x); ++failures; } } while(0)
static FVector LocalPoint(const skel::Xf& frame,const FVector& point) {
    const auto p=frame.rotation.Inverse().Rotate(point-frame.translation);
    return {p.X/frame.scale.X,p.Y/frame.scale.Y,p.Z/frame.scale.Z};
}
static double Distance(const FVector& p,const FVector& a,const FVector& b) {
    const auto d=b-a;
    const double t=Dot(d,d)>1e-10 ? std::clamp(Dot(p-a,d)/Dot(d,d),0.0,1.0) : 0;
    return Length(p-a-d*t);
}
int main() {
    for(int side=0;side<2;++side) {
        fingers::Chain chain[5]; finger_contact::Bounds bounds[5]; FVector palm;
        white_man_hands::Build(side,chain,bounds,&palm);
        shape::Palm box;
        EXPECT(shape::FitWhiteMan(chain,palm,&box) && box.fitted);
        EXPECT(Dot(Cross(box.axis[0],box.axis[1]),box.axis[2])>.999999);
        for(const auto& sample:white_man_shape_samples::palm) if(sample.side==side) {
            const auto p=sample.point-box.centre;
            EXPECT(std::fabs(Dot(p,box.axis[0]))<=box.half.X);
            EXPECT(std::fabs(Dot(p,box.axis[1]))<=box.half.Y);
            EXPECT(std::fabs(Dot(p,box.axis[2]))<=box.half.Z);
        }
        skel::Xf reference[5][3]; FVector reference_tip[5];
        for(int f=0;f<5;++f) fingers::Fk(chain[f],{},reference[f],&reference_tip[f]);
        double worst_excess=0;
        int sample_count=0;
        for(const FVector scale: {FVector{.75,.75,.75},FVector{1,1,1},FVector{1.25,1.25,1.25},FVector{1.18,.83,1.07}}) {
            skel::Xf hand; hand.rotation=AxisAngle({.2,.8,-.3},.6); hand.translation={15,-8,22}; hand.scale=scale;
            const auto world_box = shape::TransformPalm(box, hand);
            EXPECT(world_box.fitted && Dot(Cross(world_box.axis[0],world_box.axis[1]),world_box.axis[2]) > .999999);
            for(int x : {-1,1}) for(int y : {-1,1}) for(int z : {-1,1}) {
                const auto corner = box.centre + box.axis[0]*(x*box.half.X) + box.axis[1]*(y*box.half.Y) + box.axis[2]*(z*box.half.Z);
                const auto relative = skel::Apply(hand,corner)-world_box.centre;
                EXPECT(std::fabs(Dot(relative,world_box.axis[0])) <= world_box.half.X+1e-8);
                EXPECT(std::fabs(Dot(relative,world_box.axis[1])) <= world_box.half.Y+1e-8);
                EXPECT(std::fabs(Dot(relative,world_box.axis[2])) <= world_box.half.Z+1e-8);
            }
            auto invalid_hand=hand; invalid_hand.scale.Y=0;
            EXPECT(!shape::TransformPalm(box,invalid_hand).fitted);
            invalid_hand=hand; invalid_hand.translation.X=std::numeric_limits<double>::quiet_NaN();
            EXPECT(!shape::TransformPalm(box,invalid_hand).fitted);
            for(int mode=-1;mode<30;++mode) {
                const double grip=mode<0?0:(mode%15)/3*.25;
                const double trigger=mode<0?0:(mode%3)*.5;
                skel::Xf posed[5][3]; FVector tip[5];
                for(int f=0;f<5;++f) {
                    const auto target=mode<0 ? fingers::Angles{} : f==0 ?
                        thumb_pose::Target(chain[f],palm,chain[1].ref[0].translation,
                            {grip,trigger,mode>=15},bounds[f]) :
                        finger_contact::CurlTarget(chain[f],f==1?std::max(grip,trigger):grip,bounds[f]);
                    fingers::Fk(chain[f],target,posed[f],&tip[f]);
                }
                const auto check_sample=[&](const auto& sample) {
                    if(sample.side!=side) return;
                    FVector skinned{};
                    for(const auto& influence:sample.influence) {
                        const auto point=influence.finger<0 ? sample.point :
                            skel::Apply(posed[influence.finger][influence.joint],
                                LocalPoint(reference[influence.finger][influence.joint],sample.point));
                        skinned=skinned+point*influence.weight;
                    }
                    skinned=skel::Apply(hand,skinned);
                    const auto relative=skinned-world_box.centre;
                    const FVector outside={std::max(0.0,std::fabs(Dot(relative,world_box.axis[0]))-world_box.half.X),
                        std::max(0.0,std::fabs(Dot(relative,world_box.axis[1]))-world_box.half.Y),
                        std::max(0.0,std::fabs(Dot(relative,world_box.axis[2]))-world_box.half.Z)};
                    double excess=Length(outside);
                    for(int f=0;f<5;++f) for(int j=0;j<3;++j) {
                        const auto section=shape::Transform(chain[f].collision[j],posed[f][j],hand,.5);
                        const auto a=skel::Apply(hand,posed[f][j].translation);
                        const auto b=skel::Apply(hand,j<2?posed[f][j+1].translation:tip[f]);
                        excess=std::min(excess,Distance(skinned,a,b)-section.radius_cm);
                    }
                    if(excess>worst_excess) worst_excess=excess;
                    if(excess>.001) std::printf("skin excess side%d mode%d digit%d segment%d = %.5f cm\n",side,mode,sample.finger,sample.segment,excess);
                    EXPECT(excess<=.001); ++sample_count;
                };
                for(const auto& sample:white_man_shape_samples::finger) check_sample(sample);
                for(const auto& sample:white_man_shape_samples::web) check_sample(sample);
                for(const auto& sample:white_man_shape_samples::open_front) check_sample(sample);
            }
            const auto section=shape::Transform(chain[2].collision[1],reference[2][1],hand,.5);
            for(int n=0;n<30;++n) {
                const auto direction=Normalize({std::sin(n*.8),std::cos(n*.3),std::sin(n*.4+.7)});
                const double support=shape::Support(section,direction);
                for(int k=0;k<30;++k) {
                    const auto q=Normalize({std::sin(k*.7),std::cos(k*.4),std::sin(k*.9+.2)});
                    const auto surface=section.side_half*q.X+section.depth_half*q.Y+section.axial_half*q.Z;
                    EXPECT(Dot(surface,direction)<=support+1e-9);
                }
            }
        }
        std::printf("fitted mesh side%d weighted checks=%d max excess=%.6f cm\n",side,sample_count,worst_excess);
        EXPECT(chain[0].collision[0].self_start_fraction==1);
        const double union_radius=chain[0].collision[0].world_radius_cm;
        EXPECT(shape::FitWhiteMan(chain,palm));
        EXPECT(chain[0].collision[0].world_radius_cm>union_radius);
        chain[2].ref[0].translation.X+=.2;
        EXPECT(!shape::FitWhiteMan(chain,palm,&box) && !box.fitted);
        for(const auto& finger:chain) for(const auto& segment:finger.collision) EXPECT(!segment.fitted);
        EXPECT(shape::Transform(chain[0].collision[0],reference[0][0],{},.5).radius_cm==.5);
    }
    if(failures) { std::printf("hand collision shape: %d failures\n",failures); return 1; }
    std::puts("hand collision shape: all tests passed"); return 0;
}
