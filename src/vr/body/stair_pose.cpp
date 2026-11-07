#include "vr/body/stair_pose.h"
#include <algorithm>
#include <cmath>

namespace holevr::vr::body::stairs {
namespace {
bool Finite(FVector p) { return std::isfinite(p.X)&&std::isfinite(p.Y)&&std::isfinite(p.Z); }
double Dot(FVector a,FVector b) { return a.X*b.X+a.Y*b.Y+a.Z*b.Z; }
bool Surface(const Sample& s) {
    const double length=std::sqrt(Dot(s.normal,s.normal));
    return s.hit && s.support_id && std::isfinite(s.along) && std::isfinite(s.z) &&
        Finite(s.normal) && length>.5 && length<1.5 && s.normal.Z/length>=.94;
}
}
Profile Recognize(const std::array<Sample,kSamples>& input,const FVector& origin,
                  const FVector& direction,double floor_z) {
    Profile out;
    if(!Finite(origin)||!Finite(direction)||!std::isfinite(floor_z)) return out;
    const double length=std::hypot(direction.X,direction.Y);
    if(length<.5) return out;
    out.origin=origin; out.direction={direction.X/length,direction.Y/length,0};
    std::array<Tread,kSamples> groups{};
    unsigned n=0;
    for(int i=0;i<kSamples;++i) {
        const auto& sample=input[i];
        if(!Surface(sample) || (i && std::fabs(sample.along-input[i-1].along-kSpacing)>.1)) return {};
        if(n && std::fabs(sample.z-groups[n-1].z)<=1.5 && sample.support_id==groups[n-1].support_id) {
            auto& g=groups[n-1];
            g.z=(g.z*g.samples+sample.z)/(g.samples+1); ++g.samples;
            g.end=sample.along+kSpacing*.5;
        } else {
            groups[n++]={sample.along-kSpacing*.5,sample.along+kSpacing*.5,sample.z,sample.support_id,1};
        }
    }
    // Search for a complete run instead of letting a far table/landing edge
    // authorize a large jump. Every included step must have measured width.
    unsigned best_begin=0,best_count=0;
    int best_sign=0;
    for(unsigned first=0;first<n;++first) {
        if(groups[first].samples<2) continue;
        unsigned count=1; int sign=0; double rise=0; bool nearby=std::fabs(groups[first].z-floor_z)<=kMaxFloorDifference;
        for(unsigned j=first+1;j<n;++j) {
            if(groups[j].samples<2 || groups[j].end-groups[j].begin<18) break;
            const double dz=groups[j].z-groups[j-1].z;
            const int next=dz>0?1:-1;
            if(std::fabs(dz)<6 || std::fabs(dz)>kMaxRiser || (sign && next!=sign)) break;
            if(sign && std::fabs(std::fabs(dz)-rise)>std::max(3.0,.25*rise)) break;
            sign=next; rise=std::fabs(dz); ++count;
            nearby |= std::fabs(groups[j].z-floor_z)<=kMaxFloorDifference;
            if(count>=3 && nearby && count>best_count) { best_begin=first; best_count=count; best_sign=sign; }
        }
    }
    if(best_count<3) return {};
    out.valid=true; out.count=best_count; out.rise_sign=best_sign;
    for(unsigned i=0;i<best_count;++i) out.tread[i]=groups[best_begin+i];
    return out;
}
Landing Select(const Profile& profile,const FVector& desired,double floor_z,double advance) {
    Landing out;
    if(!profile.valid || profile.count<3 || profile.count>kSamples || !Finite(desired) ||
       !std::isfinite(floor_z) || !std::isfinite(advance)) return out;
    const FVector d=desired-profile.origin;
    const double along=Dot(d,profile.direction);
    const double across=std::fabs(d.X*profile.direction.Y-d.Y*profile.direction.X);
    if(across>35) return out;
    for(unsigned i=0;i<profile.count;++i) {
        const auto& t=profile.tread[i];
        if(along<t.begin || along>t.end || t.end-t.begin<18 ||
           std::fabs(t.z-floor_z)>kMaxFloorDifference) continue;
        const double at=std::clamp(along,t.begin+8,t.end-8);
        if(std::fabs(at-along)>8) return out;
        out.valid=true; out.sole=desired+profile.direction*(at-along); out.sole.Z=t.z;
        out.begin=t.begin; out.end=t.end; out.support_id=t.support_id; out.clearance_z=t.z;
        const double ahead=along+std::clamp(advance,-20.0,20.0);
        for(unsigned j=0;j<profile.count;++j) {
            const auto& next=profile.tread[j];
            if(ahead>=next.begin && ahead<=next.end && std::fabs(next.z-t.z)<=kMaxRiser)
                out.clearance_z=std::max(out.clearance_z,next.z);
        }
        return out;
    }
    return out;
}
} // namespace holevr::vr::body::stairs
