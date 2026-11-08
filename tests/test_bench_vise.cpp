// Geometry and actual-bench fit regression checks; no engine is required.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "vr/bench_vise_mesh.h"
#include "vr/bench_vise_layout.h"
#include "vr/bench_vise_motion.h"
#include <limits>

using namespace holevr::vr;
using namespace holevr::vr::bench_vise;
namespace {
int failures = 0;
void Check(bool ok, const char* message) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", message);
    if (!ok) ++failures;
}
FVector Cross(const FVector& a, const FVector& b) {
    return {a.Y*b.Z-a.Z*b.Y, a.Z*b.X-a.X*b.Z, a.X*b.Y-a.Y*b.X};
}
double Dot(const FVector& a, const FVector& b) { return a.X*b.X+a.Y*b.Y+a.Z*b.Z; }
bool Finite(const FVector& v) { return std::isfinite(v.X)&&std::isfinite(v.Y)&&std::isfinite(v.Z); }
}

int main() {
    int triangles = 0, contact_vertices = 0;
    bool valid = true, winding = true, flat = true, supported = true;
    FVector lo{1e9,1e9,1e9}, hi{-1e9,-1e9,-1e9};
    // Actual DS_Workbench bounds and tabletop, measured from its game mesh.
    const double front = 55.6332707, side_min = -137.9779339, side_max = 106.2901378;
    const double middle = (side_min+side_max)*.5, table_z = 90.0270283;
    const auto seat = layout::Seat({front,middle,92.3351705}, {1,0,0}, table_z);
    for (const auto& section : mesh::kSections) {
        valid &= section.count>0 && section.count%3==0;
        triangles += section.count/3;
        for (int i=0;i<section.count;++i) {
            const auto& v=section.vertices[i];
            valid &= Finite(v.position)&&Finite(v.normal)&&std::fabs(Dot(v.normal,v.normal)-1)<1e-5;
            lo.X=std::min(lo.X,v.position.X);lo.Y=std::min(lo.Y,v.position.Y);lo.Z=std::min(lo.Z,v.position.Z);
            hi.X=std::max(hi.X,v.position.X);hi.Y=std::max(hi.Y,v.position.Y);hi.Z=std::max(hi.Z,v.position.Z);
            // Every bottom contact lies on the table, inset from its edge.
            if (v.position.Z<.001) {
                ++contact_vertices;
                const FVector p=seat+v.position;
                supported &= p.X<front-2 && p.X>-55.4075181 && p.Y>side_min && p.Y<side_max;
                supported &= std::fabs(p.Z-table_z-layout::kSurfaceLift)<.001;
            }
        }
        for (int i=0;i<section.count;i+=3) {
            const auto& a=section.vertices[i];const auto& b=section.vertices[i+1];const auto& c=section.vertices[i+2];
            const auto cross=Cross(b.position-a.position,c.position-a.position);
            const double area2=std::sqrt(Dot(cross,cross));
            // UE clockwise fronts: the numerical cross product points
            // opposite the outward normal after reflecting Blender Y.
            winding &= area2>1e-7 && Dot(cross,a.normal)/area2<-.999;
            flat &= Dot(a.normal,b.normal)>.99999 && Dot(a.normal,c.normal)>.99999;
        }
    }
    Check(valid && triangles==512, "512 finite triangles; valid unit normals in all three material sections");
    Check(winding, "no collapsed faces or inverted normals after conversion to Unreal coordinates");
    Check(flat, "flat face normals preserve the low-poly silhouette");
    Check(std::fabs(hi.X-lo.X-30.4)<.001 && std::fabs(hi.Z-lo.Z-16.65)<.001 && lo.Z>=-.001,
          "open size is 30.4 cm long and 16.65 cm tall, with its origin on the tabletop");
    Check(contact_vertices>20 && supported, "entire base is supported with at least 2 cm front margin");
    Check(std::fabs(seat.Y-middle)<1e-8 && std::fabs(seat.X-front+10)<1e-8,
          "base is centred on the actual bench edge, 10 cm inward");
    Check(hi.X-layout::kBaseInset>9 && lo.Z==0, "handle overhangs the player-facing edge while the base stays supported");
    layout::PartsRow row;
    bool clear_parts=true, on_table=true, easy_reach=true;
    // Mixed long suppressors and small optics force wrapping, while small
    // first-row parts stay at the previous comfortable 7 cm inset.
    for (int i=0;i<24;++i) {
        const double hw=i%5==0 ? 18 : 4, hd=i%3==0 ? 5 : 3;
        const auto p=row.Next(hw,hd,(side_max-side_min)*.5);
        clear_parts &= std::fabs(p.along)-hw>=layout::kPartsClearance-1e-7;
        on_table &= std::fabs(p.along)+hw < (side_max-side_min)*.5 && p.inset-hd>=2 && p.inset+hd<111;
        if (i<2) easy_reach &= p.inset==7;
    }
    Check(clear_parts && on_table && easy_reach, "parts fit beside the vise; long parts clear it and short ones remain close to the front");
    Check(std::fabs(92.3351705-layout::kTopBelowBounds-table_z)<1e-4,
          "trace fallback seats the vise on the tabletop, not on the raised back lip");
    for (const double yaw : {0.,86.,180.,-90.}) {
        const double r=yaw*kDegToRad;
        const FVector out{std::cos(r),std::sin(r),0}, along{-out.Y,out.X,0};
        const FVector edge=FVector{139.7,-234.6,0}+out*front+along*middle;
        const auto p=layout::Seat(edge,out,table_z), delta=p-edge;
        Check(std::fabs(Dot(delta,out)+10)<1e-7 && std::fabs(Dot(delta,along))<1e-7,
              "bench rotation preserves the centre and inward mount direction");
    }
    Check(std::fabs(mesh::kSections[0].color.r-.058023f)<1e-7 &&
          std::fabs(mesh::kSections[1].color.g-.242630f)<1e-7 &&
          std::fabs(mesh::kSections[2].color.b-.02f)<1e-7,
          "native material values match the selected HOLE bible entries in linear space");

    double fixed_face=-1e9, moving_face=1e9;
    int part_triangles[2]={};
    for (const auto& s:mesh::kSections) {
        Check(s.part>=0 && s.part<2,"mesh section belongs to the fixed base or moving assembly");
        part_triangles[s.part]+=s.count/3;
        if (s.material!=1) continue;
        for (int i=0;i<s.count;++i) {
            const auto& p=s.vertices[i].position;
            if (p.Z<10.3) continue;
            if (s.part==0) fixed_face=std::max(fixed_face,p.X);
            else moving_face=std::min(moving_face,p.X);
        }
    }
    Check(part_triangles[0]==264 && part_triangles[1]==248,"base stays fixed; jaw, spindle and handle move together");
    Check(std::fabs(fixed_face-motion::kFixedJawFace)<1e-5 && std::fabs(moving_face-fixed_face-motion::kOpenGap)<1e-5,
          "exported jaw faces match the 8 cm open state");
    const double loaded=motion::LoadedGap(3.196);
    Check(std::fabs(moving_face+(loaded-motion::kOpenGap)-fixed_face-loaded)<1e-5 && loaded>3.196,
          "loaded state leaves 2 mm total grip clearance instead of closing through the pistol");
    Check(std::fabs(moving_face+(motion::kClosedGap-motion::kOpenGap)-fixed_face-.6)<1e-5,
          "fully closed model state matches the original 6 mm gap");
    for (int hz:{30,60,90,120}) {
        motion::Jaw jaw;
        for (int i=0;i<hz;++i) jaw.Update(false,loaded,1./hz);
        Check(jaw.Gap()==motion::kOpenGap,"empty vise stays open");
        bool monotonic=true;double previous=jaw.Gap();
        for (int i=0;i<hz;++i) {
            const double gap=jaw.Update(true,loaded,1./hz);
            monotonic &= gap<=previous+1e-8 && gap>=loaded-1e-8;previous=gap;
        }
        Check(monotonic && std::fabs(jaw.Gap()-loaded)<1e-7,"parking closes smoothly without overshoot at each frame rate");
        const double frozen=jaw.Gap();jaw.Update(true,loaded,0);
        Check(jaw.Gap()==frozen,"paused game leaves the jaw in place");
        for (int i=0;i<hz;++i) jaw.Update(false,loaded,1./hz);
        Check(jaw.Gap()==motion::kOpenGap,"pickup, return-to-holster or weapon replacement reopens the vise");
    }
    motion::Jaw reversal;
    reversal.Update(true,loaded,.1);reversal.Update(true,loaded,.1);
    const double halfway=reversal.Gap();reversal.Update(false,loaded,0);
    Check(reversal.Gap()==halfway,"pickup midway through closing reverses without a position jump");
    reversal.Update(false,loaded,-1);reversal.Update(false,loaded,std::numeric_limits<double>::quiet_NaN());
    Check(reversal.Gap()==halfway,"bad or backwards time cannot corrupt the jaw pose");
    reversal.Reset();Check(reversal.Gap()==motion::kOpenGap,"new level resets the jaw open");
    for (double yaw:{-90.,90.,176.}) {
        const auto frame=ToQuat({0,yaw,0});
        const FVector target{150,-190,103.375},contact_from_main{1.7,-1.3,9.2};
        for (const FVector offset:{FVector{-5,1.3,-5},FVector{-1,2,-5}}) {
            const auto hand=motion::HandSpot(target,frame,contact_from_main,offset);
            const auto resulting_contact=hand+frame.Rotate(offset+contact_from_main);
            const auto error=resulting_contact-target;
            Check(Dot(error,error)<1e-10,"actual gun-placement formula seats the grip for both barrel directions and calibrated offsets");
        }
    }
    return failures ? 1 : 0;
}
