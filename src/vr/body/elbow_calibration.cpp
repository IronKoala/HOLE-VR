#include "vr/body/elbow_calibration.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace holevr::vr::elbow_calibration {
namespace {
double Dot(FVector a,FVector b) { return a.X*b.X+a.Y*b.Y+a.Z*b.Z; }
double Length(FVector a) { return std::sqrt(Dot(a,a)); }
bool Finite(FVector a) { return std::isfinite(a.X)&&std::isfinite(a.Y)&&std::isfinite(a.Z); }
double Median(std::vector<double> values) {
    std::sort(values.begin(),values.end());
    const auto n=values.size();
    return n%2?values[n/2]:(values[n/2-1]+values[n/2])*.5;
}
struct Point { FVector p,u; double weight=1; bool accepted=true; };
struct FitState { FVector elbow; double length=0,rms=0,variance=0,weight=0; };
bool Solve(const std::vector<Point>& points,FitState* out) {
    double weight=0; FVector pmean{},umean{};
    for(const auto& p:points) if(p.accepted) {
        weight+=p.weight; pmean=pmean+p.p*p.weight; umean=umean+p.u*p.weight;
    }
    if(weight<=0) return false;
    pmean=pmean*(1/weight); umean=umean*(1/weight);
    double variance=0,covariance=0;
    for(const auto& p:points) if(p.accepted) {
        const auto du=p.u-umean;
        variance+=p.weight*Dot(du,du); covariance+=p.weight*Dot(p.p-pmean,du);
    }
    if(variance<=1e-9) return false;
    const double length=covariance/variance;
    const auto elbow=pmean-umean*length;
    if(!std::isfinite(length)||length<10||length>70||!Finite(elbow)) return false;
    double residual2=0;
    for(const auto& p:points) if(p.accepted) {
        const auto d=p.p-elbow-p.u*length;
        residual2+=p.weight*Dot(d,d);
    }
    *out={elbow,length,std::sqrt(residual2/weight),variance/weight,weight};
    return true;
}
} // namespace

Result Fit(const std::vector<Observation>& observations) {
    Result result;
    result.samples=unsigned(observations.size());
    if(observations.size()<10||observations.size()>512) return result;
    std::vector<Point> points;
    points.reserve(observations.size());
    for(const auto& sample:observations) {
        const double norm=Length(sample.forearm_direction);
        if(!Finite(sample.wrist)||!Finite(sample.forearm_direction)||!std::isfinite(norm)||norm<1e-6) return result;
        points.push_back({sample.wrist,sample.forearm_direction*(1/norm)});
    }
    // Inverse local angular density gives the moving portion comparable weight
    // to a pause, regardless of how many frames the pause contributes.
    constexpr double cluster_cos=.9993908270190958; // 2 degrees, sampling resolution only
    double total_weight=0;
    for(auto& p:points) {
        unsigned neighbors=0;
        for(const auto& q:points) if(Dot(p.u,q.u)>=cluster_cos) ++neighbors;
        p.weight=1.0/std::max(1u,neighbors); total_weight+=p.weight;
    }
    // A median of separated-pair scalar fits has a majority of clean pairs
    // when isolated bad samples are below the allowed 15% budget.
    std::vector<double> lengths;
    for(std::size_t i=0;i<points.size();++i) for(std::size_t j=0;j<i;++j) {
        const auto du=points[i].u-points[j].u;
        const double separation=Dot(du,du);
        if(separation>.0027) lengths.push_back(Dot(points[i].p-points[j].p,du)/separation);
    }
    if(lengths.size()<10) return result;
    const double seed_length=Median(lengths);
    if(!std::isfinite(seed_length)||seed_length<10||seed_length>70) return result;
    std::vector<FVector> centers;
    for(const auto& p:points) centers.push_back(p.p-p.u*seed_length);
    // A medoid is rotation/translation covariant and cannot be dragged far by
    // one position spike. The accepted-set solve below supplies the final mean.
    FVector seed_center;
    double best=std::numeric_limits<double>::infinity();
    for(const auto& c:centers) {
        double distance=0;
        for(std::size_t i=0;i<centers.size();++i) distance+=Length(centers[i]-c)*points[i].weight;
        if(distance<best) { best=distance; seed_center=c; }
    }
    std::vector<double> residuals;
    for(const auto& c:centers) residuals.push_back(Length(c-seed_center));
    const double gate=std::clamp(3*Median(residuals),1.0,4.0);
    for(std::size_t i=0;i<points.size();++i) points[i].accepted=residuals[i]<=gate;
    FitState fit;
    // Recompute the same physical model after outlier selection, and allow
    // only removals. An excluded spike cannot expand the final acceptance gate.
    for(int pass=0;pass<3;++pass) {
        if(!Solve(points,&fit)) return result;
        bool removed=false;
        for(auto& p:points) if(p.accepted&&Length(p.p-fit.elbow-p.u*fit.length)>4) {
            p.accepted=false; removed=true;
        }
        if(!removed) break;
    }
    if(!Solve(points,&fit)) return result;
    unsigned count=0,largest_cluster=0,regions=0;
    std::vector<FVector> region_centers;
    for(const auto& p:points) if(p.accepted) {
        ++count;
        unsigned neighbors=0;
        for(const auto& q:points) if(q.accepted&&Dot(p.u,q.u)>=cluster_cos) ++neighbors;
        largest_cluster=std::max(largest_cluster,neighbors);
        bool distinct=true;
        for(const auto& q:region_centers) if(Dot(p.u,q)>=cluster_cos) { distinct=false; break; }
        if(distinct) { region_centers.push_back(p.u); ++regions; }
    }
    if(count<10||count<points.size()*.85||fit.weight<total_weight*.85||regions<4||
       count-largest_cluster<count*.25||fit.rms>1.5) return result;
    // A residual-free stationary jitter trace must not look infinitely precise.
    // The 0.25cm floor is a conservative numerical sensitivity budget, not a
    // claim about headset accuracy or a universal anatomical movement range.
    const double sensitivity=std::max(.25,fit.rms)/std::sqrt(fit.variance);
    if(!std::isfinite(sensitivity)||sensitivity>5) return result;
    double minimum_dot=1;
    for(const auto& p:points) if(p.accepted) for(const auto& q:points) if(q.accepted)
        minimum_dot=std::min(minimum_dot,Dot(p.u,q.u));
    result.valid=true; result.elbow=fit.elbow; result.forearm_cm=fit.length;
    result.rms_cm=fit.rms; result.span_degrees=std::acos(std::clamp(minimum_dot,-1.0,1.0))*kRadToDeg;
    result.uncertainty_cm=sensitivity; result.inliers=count;
    return result;
}
} // namespace holevr::vr::elbow_calibration
