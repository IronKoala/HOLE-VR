#pragma once
#include <algorithm>
#include <cmath>
#include <locale>
#include <map>
#include <sstream>
#include <string>
#include "vr/math.h"
#include "vr/bench_layout_catalog.h"

namespace holevr::vr::bench_layout {
// Editor coordinates: cm from the middle of the player-facing tabletop edge.
// X is player-right; depth is inward. Yaw 0/90/180 points right/inward/left.
struct Placement { double x = 0, depth = 0, yaw = 0; };
using Layout = std::map<std::string, Placement>;
inline std::string Trim(const std::string& s) {
    const auto first=s.find_first_not_of(" \t\r\n");
    if (first==std::string::npos) return {};
    return s.substr(first,s.find_last_not_of(" \t\r\n")-first+1);
}
inline bool Valid(const Placement& p) {
    return std::isfinite(p.x)&&std::isfinite(p.depth)&&std::isfinite(p.yaw)&&
        std::fabs(p.x)<=500&&p.depth>=0&&p.depth<=500&&std::fabs(p.yaw)<=3600;
}
// Parse atomically: an invalid/unfinished file never partially replaces a layout.
inline bool Parse(std::string text, Layout* out, std::string* error) {
    const auto fail=[&](const char* why){if(error)*error=why;return false;};
    if (!out || text.size()>32768) return fail("file too large or missing output");
    if (text.compare(0,3,"\xEF\xBB\xBF")==0) text.erase(0,3);
    std::istringstream lines(text); std::string line; Layout pending; bool version=false;
    while (std::getline(lines,line)) {
        line=Trim(line.substr(0,line.find_first_of("#;")));
        if (line.empty()) continue;
        const auto equals=line.find('=');
        if (equals==std::string::npos) return fail("expected key=value");
        const std::string row=Trim(line.substr(0,equals)),value=Trim(line.substr(equals+1));
        if (row=="version") {
            if (version || value!="1") return fail("missing or unsupported layout version");
            version=true;continue;
        }
        if (!FindPart(row)) return fail("unknown attachment row");
        std::istringstream values(value); values.imbue(std::locale::classic());
        Placement p; char comma1=0,comma2=0;
        if (!(values>>p.x>>comma1>>p.depth>>comma2>>p.yaw) || comma1!=',' || comma2!=',' || !Valid(p))
            return fail("expected finite x,depth,yaw values");
        values>>std::ws;
        if (!values.eof()) return fail("unexpected text after yaw");
        p.yaw=std::fmod(std::fmod(p.yaw,360.0)+360.0,360.0);
        if (!pending.emplace(row,p).second) return fail("duplicate attachment row");
    }
    if (!version) return fail("missing layout version");
    *out=std::move(pending);if(error)error->clear();return true;
}

struct Pose { FVector centre; FQuat rotation; bool clamped=false; };
inline bool Finite(const FVector& v) { return std::isfinite(v.X)&&std::isfinite(v.Y)&&std::isfinite(v.Z); }
// No release-hand or inventory index participates in this transform. Thus
// changing hands, fitting a different part, or swapping weapons cannot shuffle
// the selected resting places. Native rolls reproduce the preview's real meshes.
inline bool Place(const Part& part, const Placement& p, const FVector& edge,
                  const FVector& outward, double half_width, double table_depth,
                  const FVector& lo, const FVector& hi, Pose* result) {
    if (!result || !Valid(p) || !Finite(edge) || !Finite(outward) || !Finite(lo) || !Finite(hi) ||
        !std::isfinite(half_width) || !std::isfinite(table_depth) ||
        half_width<=0 || table_depth<=0 || lo.X>hi.X || lo.Y>hi.Y || lo.Z>hi.Z) return false;
    const double length=std::hypot(outward.X,outward.Y);
    if (length<1e-6) return false;
    const FVector out{outward.X/length,outward.Y/length,0},right{out.Y,-out.X,0};
    const FQuat rotation=YawOnly(std::atan2(right.Y,right.X)*kRadToDeg-p.yaw)*part.actor_in_mesh;
    const FVector midpoint=(lo+hi)*.5;
    double hw=0,hd=0;
    for (int k=0;k<8;++k) {
        const FVector corner{k&1?hi.X:lo.X,k&2?hi.Y:lo.Y,k&4?hi.Z:lo.Z};
        const FVector d=rotation.Rotate(corner-midpoint);
        hw=std::max(hw,std::fabs(d.X*right.X+d.Y*right.Y));
        hd=std::max(hd,std::fabs(d.X*out.X+d.Y*out.Y));
    }
    // Match the editor's half-centimetre edge margin and back-lip allowance.
    if (half_width-hw<.5 || table_depth-2*hd<3.3) return false;
    const double x=std::clamp(p.x,-half_width+hw+.5,half_width-hw-.5);
    const double depth=std::clamp(p.depth,hd+.5,table_depth-hd-2.8);
    result->centre=edge+right*x-out*depth;
    result->rotation=rotation;
    result->clamped=std::fabs(x-p.x)>.11 || std::fabs(depth-p.depth)>.11;
    return true;
}
} // namespace holevr::vr::bench_layout
