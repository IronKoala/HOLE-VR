#include <cstdio>
#include <limits>
#include "vr/bench_layout.h"
using namespace holevr::vr;
namespace layout=bench_layout;
#include "bench_layout_editor_fixtures.h"
static int errors=0;
#define CHECK(x) do {if(!(x)){std::printf("FAIL %d: %s\n",__LINE__,#x);++errors;}}while(0)
double Distance(FVector a,FVector b){const auto d=a-b;return std::sqrt(d.X*d.X+d.Y*d.Y+d.Z*d.Z);}
int main(){
    layout::Layout saved;std::string error;
    CHECK(layout::Parse("\xEF\xBB\xBF# chosen layout\r\nversion = 1\r\nLIGHT_HG_LIGHT = -34.5, 23, -90 # near left\n",&saved,&error));
    CHECK(saved.size()==1 && saved.at("LIGHT_HG_LIGHT").x==-34.5 && saved.at("LIGHT_HG_LIGHT").yaw==270);
    for(const std::string text:{"LIGHT_HG_LIGHT=1,2,3", "version=2\nLIGHT_HG_LIGHT=1,2,3",
        "version=1\nLIGHT_HG_LIGHT=1,2,nan", "version=1\nLIGHT_HG_LIGHT=1,inf,3",
        "version=1\nLIGHT_HG_LIGHT=1,2,3 garbage", "version=1\nLIGHT_HG_LIGHT=1 2 3",
        "version=1\nLIGHT_HG_LIGHT=1,2", "version=1\nLIGHT_HG_LIGHT=1,2,3\nLIGHT_HG_LIGHT=4,5,6",
        "version=1\nUNKNOWN_ROW=1,2,3", "version=1\nMAG_HG_10R=1,2,3",
        "version=1\nLIGHT_HG_LIGHT=1,-2,3", "version=1\nversion=1"}) {
        CHECK(!layout::Parse(text,&saved,&error));
        CHECK(saved.size()==1 && saved.at("LIGHT_HG_LIGHT").x==-34.5);
    }
    CHECK(!layout::Parse(std::string(32769,' '),&saved,&error));
    CHECK(layout::Parse("version=1\n",&saved,&error) && saved.empty());
    int samples=0;
    for(const auto& fixture:kEditorFixtures){
        const auto* part=layout::FindPart(fixture.row);CHECK(part);
        if(!part)continue;
        for(double bench_yaw:{0.,73.,180.,-90.})for(double yaw:{0.,35.,90.,180.,270.}){
            const double a=bench_yaw*kDegToRad,b=yaw*kDegToRad;
            const FVector edge{135,-81,90},out{std::cos(a),std::sin(a),0},right{out.Y,-out.X,0};
            const layout::Placement wanted{-45,45,yaw};layout::Pose pose;
            CHECK(layout::Place(*part,wanted,edge,out,122.134,111.04,fixture.lo,fixture.hi,&pose));
            CHECK(!pose.clamped);
            FVector origin=pose.centre-pose.rotation.Rotate((fixture.lo+fixture.hi)*.5);
            double bottom=1e10;
            for(int k=0;k<8;++k){const FVector corner{k&1?fixture.hi.X:fixture.lo.X,k&2?fixture.hi.Y:fixture.lo.Y,k&4?fixture.hi.Z:fixture.lo.Z};bottom=std::min(bottom,pose.rotation.Rotate(corner).Z);}
            origin.Z=edge.Z+.3-bottom;
            for(const auto& sample:fixture.samples){
                const auto& e=sample.editor;
                // Three.js Y-up preview rotation, independently projected into
                // the bench's right/outward/up directions. Samples are literal
                // vertices verified against the shipped editor asset payload.
                const double x=std::cos(b)*e.X+std::sin(b)*e.Z;
                const double z=-std::sin(b)*e.X+std::cos(b)*e.Z;
                const FVector expected=edge+right*(wanted.x+x)+out*(-wanted.depth+z)+FVector{0,0,.3+e.Y};
                const FVector actual=origin+pose.rotation.Rotate(sample.actor);
                const double error_cm=Distance(expected,actual);
                if(error_cm>.025){std::printf("mesh mismatch %s bench %.1f yaw %.1f: %.5f cm\n",fixture.row,bench_yaw,yaw,error_cm);++errors;}
                ++samples;
            }
        }
    }
    const auto& light=*layout::FindPart("LIGHT_HG_LIGHT");layout::Pose bounded;
    CHECK(layout::Place(light,{500,500,90},{0,0,90},{1,0,0},122,111,{-3,-2,-1},{3,2,1},&bounded)&&bounded.clamped);
    CHECK(!layout::Place(light,{1,2,3},{0,0,0},{0,0,0},122,111,{-1,-1,-1},{1,1,1},&bounded));
    CHECK(!layout::Place(light,{1,2,3},{0,0,0},{1,0,0},1,1,{-3,-3,-3},{3,3,3},&bounded));
    CHECK(!layout::Place(light,{1,2,std::numeric_limits<double>::quiet_NaN()},{0,0,0},{1,0,0},122,111,{-1,-1,-1},{1,1,1},&bounded));
    std::printf("bench layout: %d real editor vertex/pose comparisons; %d failures\n",samples,errors);
    return errors?1:0;
}
