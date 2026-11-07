#include "vr/body/functional_reach.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace holevr::vr::functional_reach {
namespace {
using Matrix = std::array<std::array<double,3>,3>;
double Dot(FVector a,FVector b) { return a.X*b.X+a.Y*b.Y+a.Z*b.Z; }
double Length(FVector a) { return std::sqrt(Dot(a,a)); }
bool Finite(FVector a) { return std::isfinite(a.X)&&std::isfinite(a.Y)&&std::isfinite(a.Z); }
FVector Vec(const std::array<double,3>& v) { return {v[0],v[1],v[2]}; }
std::array<double,3> Array(FVector v) { return {v.X,v.Y,v.Z}; }
Matrix Rotation(FQuat q) {
    const auto x=q.Rotate({1,0,0}),y=q.Rotate({0,1,0}),z=q.Rotate({0,0,1});
    return {{{x.X,y.X,z.X},{x.Y,y.Y,z.Y},{x.Z,y.Z,z.Z}}};
}
FVector Mul(const Matrix& m,FVector v) {
    const auto a=Array(v); std::array<double,3> b{};
    for(int i=0;i<3;++i) for(int j=0;j<3;++j) b[i]+=m[i][j]*a[j];
    return Vec(b);
}
FVector TransposeMul(const Matrix& m,FVector v) {
    const auto a=Array(v); std::array<double,3> b{};
    for(int i=0;i<3;++i) for(int j=0;j<3;++j) b[i]+=m[j][i]*a[j];
    return Vec(b);
}
struct Point { FVector p; Matrix r; double weight=1; };
using Points=std::vector<std::vector<Point>>;
struct Model { FVector shoulder,g; double condition=0,excitation=0; bool observed=false; };
// Symmetric Jacobi eigendecomposition. Columns of vectors are eigenvectors.
void Eigen(Matrix a,std::array<double,3>* values,Matrix* vectors) {
    *vectors={{{1,0,0},{0,1,0},{0,0,1}}};
    for(int iteration=0;iteration<32;++iteration) {
        int p=0,q=1;
        if(std::fabs(a[0][2])>std::fabs(a[p][q])) { p=0;q=2; }
        if(std::fabs(a[1][2])>std::fabs(a[p][q])) { p=1;q=2; }
        if(std::fabs(a[p][q])<1e-13) break;
        const double angle=.5*std::atan2(2*a[p][q],a[q][q]-a[p][p]);
        const double c=std::cos(angle),s=std::sin(angle);
        const double pp=a[p][p],qq=a[q][q],pq=a[p][q];
        a[p][p]=c*c*pp-2*s*c*pq+s*s*qq;
        a[q][q]=s*s*pp+2*s*c*pq+c*c*qq; a[p][q]=a[q][p]=0;
        for(int k=0;k<3;++k) if(k!=p&&k!=q) {
            const double kp=a[k][p],kq=a[k][q];
            a[k][p]=a[p][k]=c*kp-s*kq;
            a[k][q]=a[q][k]=s*kp+c*kq;
        }
        for(int k=0;k<3;++k) {
            const double kp=(*vectors)[k][p],kq=(*vectors)[k][q];
            (*vectors)[k][p]=c*kp-s*kq; (*vectors)[k][q]=s*kp+c*kq;
        }
    }
    *values={a[0][0],a[1][1],a[2][2]};
}
Model Solve(const Points& groups,FVector prior,bool fixed_prior=false) {
    Matrix mean_r{}; FVector mean_p; double total=0;
    for(const auto& group:groups) {
        double sum=0; for(const auto& p:group) sum+=p.weight;
        if(sum<=0) continue;
        for(const auto& p:group) {
            const double w=p.weight/sum;
            mean_p=mean_p+p.p*w; total+=w;
            for(int i=0;i<3;++i) for(int j=0;j<3;++j) mean_r[i][j]+=w*p.r[i][j];
        }
    }
    Model model; model.shoulder=prior;
    if(total<=0) return model;
    mean_p=mean_p*(1/total);
    for(auto& row:mean_r) for(auto& x:row) x/=total;
    Matrix normal{}; FVector rhs,fixed;
    for(const auto& group:groups) {
        double sum=0; for(const auto& p:group) sum+=p.weight;
        if(sum<=0) continue;
        for(const auto& p:group) {
            const double w=p.weight/(sum*total);
            Matrix a=p.r;
            for(int i=0;i<3;++i) for(int j=0;j<3;++j) a[i][j]-=mean_r[i][j];
            rhs=rhs+TransposeMul(a,p.p-mean_p)*w;
            fixed=fixed+TransposeMul(p.r,p.p-prior)*w;
            for(int i=0;i<3;++i) for(int j=0;j<3;++j) for(int k=0;k<3;++k)
                normal[i][j]+=w*a[k][i]*a[k][j];
        }
    }
    std::array<double,3> values; Matrix vectors; Eigen(normal,&values,&vectors);
    const double small=std::max(0.0,*std::min_element(values.begin(),values.end()));
    const double large=*std::max_element(values.begin(),values.end());
    model.condition=large>1e-12?small/large:0;
    model.excitation=small;
    // Absolute excitation matters too: an isotropic but tiny rotation cloud
    // has a good ratio while position noise still dominates its inverse.
    model.observed=!fixed_prior&&small>=.01&&model.condition>=.01;
    if(model.observed) {
        for(int i=0;i<3;++i) {
            const FVector axis{vectors[0][i],vectors[1][i],vectors[2][i]};
            model.g=model.g+axis*(Dot(axis,rhs)/values[i]);
        }
        model.shoulder=mean_p-Mul(mean_r,model.g);
    } else model.g=fixed;
    return model;
}
double Residual(const Point& p,const Model& m) { return Length(p.p-m.shoulder-Mul(p.r,m.g)); }
double WorstRms(const Points& groups,const Model& model) {
    double worst=0;
    for(const auto& group:groups) {
        double square=0,count=0;
        for(const auto& p:group) if(p.weight>0) {
            const double r=Residual(p,model);
            if(!std::isfinite(r)||r>4) return 1e9;
            square+=r*r; ++count;
        }
        worst=std::max(worst,std::sqrt(square/count));
    }
    return worst;
}
}

Result Fit(const std::vector<Group>& input,FVector priorShoulder,double priorReach) {
    Result result;
    if(!Finite(priorShoulder)||Length(priorShoulder)>150||!std::isfinite(priorReach)||priorReach<25||priorReach>110) return result;
    Points groups;
    for(const auto& source:input) {
        if(source.empty()) continue;
        if(source.size()<3||source.size()>1024) return result;
        auto& group=groups.emplace_back();
        for(const auto& sample:source) {
            FQuat q=sample.grip;
            const double n=std::sqrt(q.X*q.X+q.Y*q.Y+q.Z*q.Z+q.W*q.W);
            if(!Finite(sample.wrist)||Length(sample.wrist)>1000||!std::isfinite(n)||n<1e-9) return result;
            q={q.X/n,q.Y/n,q.Z/n,q.W/n}; group.push_back({sample.wrist,Rotation(q),1});
        }
    }
    if(groups.size()<2||groups.size()>8) return result;
    Model model;
    // Huber reweighting finds a representative model before the hard budget.
    // Normalize each group independently: long pauses cannot hide a bad stage.
    for(int iteration=0;iteration<16;++iteration) {
        model=Solve(groups,priorShoulder);
        for(auto& group:groups) for(auto& p:group) {
            const double r=Residual(p,model); p.weight=r>1.5?1.5/r:1;
        }
    }
    model=Solve(groups,priorShoulder);
    for(auto& group:groups) {
        unsigned accepted=0;
        for(auto& p:group) { p.weight=Residual(p,model)<=4?1:0; accepted+=p.weight>0; }
        if(accepted<3||accepted*20<group.size()*17) return result;
        result.samples+=unsigned(group.size()); result.inliers+=accepted;
    }
    model=Solve(groups,priorShoulder);
    double worst=WorstRms(groups,model);
    // Weak excitation can amplify a small residual into a large pivot error.
    // This is a conservative sensitivity check, not a statistical confidence
    // interval (tracking/model errors need not be independent samples).
    if(model.observed&&worst/std::sqrt(model.excitation)>5) {
        model=Solve(groups,priorShoulder,true);
        worst=WorstRms(groups,model);
    }
    const double reach=Length(model.g);
    if(!Finite(model.shoulder)||!std::isfinite(reach)||reach<25||reach>110) return Result{};
    if(worst>2) return Result{};
    result.valid=true; result.observed=model.observed; result.shoulder=model.shoulder;
    result.reach=reach; result.rms=worst; result.condition=model.condition;
    return result;
}

Result FitPriorReach(const std::vector<Group>& endpoints,FVector priorShoulder) {
    Result result;
    if(endpoints.size()!=2||!Finite(priorShoulder)||Length(priorShoulder)>150) return result;
    std::vector<double> accepted[2];
    double mean[2]={};
    for(int stage=0;stage<2;++stage) {
        const auto& group=endpoints[stage];
        if(group.size()<3||group.size()>1024) return Result{};
        std::vector<double> radii; radii.reserve(group.size());
        for(const auto& point:group) {
            if(!Finite(point.wrist)) return Result{};
            const double radius=Length(point.wrist-priorShoulder);
            if(!std::isfinite(radius)||radius>1000) return Result{};
            radii.push_back(radius);
        }
        std::sort(radii.begin(),radii.end());
        const double median=(radii[(radii.size()-1)/2]+radii[radii.size()/2])*.5;
        for(double radius:radii) if(std::fabs(radius-median)<=4) {
            accepted[stage].push_back(radius);mean[stage]+=radius;
        }
        if(accepted[stage].size()<3||accepted[stage].size()*20<group.size()*17) return Result{};
        mean[stage]/=accepted[stage].size();
        result.samples+=unsigned(group.size()); result.inliers+=unsigned(accepted[stage].size());
    }
    const double reach=(mean[0]+mean[1])*.5;
    if(reach<25||reach>110||std::fabs(mean[0]-mean[1])>std::min(3.0,.05*reach)) return Result{};
    double worst=0;
    for(const auto& group:accepted) {
        double square=0;
        for(double radius:group) {
            const double error=radius-reach;
            if(std::fabs(error)>4) return Result{};
            square+=error*error;
        }
        worst=std::max(worst,std::sqrt(square/group.size()));
    }
    if(worst>2) return Result{};
    result.valid=true; result.shoulder=priorShoulder;result.reach=reach;result.rms=worst;
    return result;
}
namespace {
struct PositionPoint { FVector p; double weight=1; };
using PositionGroups=std::vector<std::vector<PositionPoint>>;
struct PositionModel {
    FVector centre;
    Matrix axes{};
    std::array<double,3> eigen{};
    std::array<bool,3> use{};
    unsigned dimensions=0;
    double radius=0,condition=0;
};
double Quantile(std::vector<double> v,double fraction) {
    if(v.empty()) return 0;
    std::sort(v.begin(),v.end());
    const double at=fraction*(v.size()-1); const auto i=std::size_t(at);
    return v[i]+(v[std::min(i+1,v.size()-1)]-v[i])*(at-i);
}
FVector Column(const Matrix& m,int i) { return {m[0][i],m[1][i],m[2][i]}; }
bool TrimEndpointSpikes(PositionGroups* groups) {
    // A rare displaced endpoint observation can lie exactly on another sphere.
    // Radial error alone cannot identify that tracking spike. Endpoint holds
    // supply a separate spatial consensus; transition samples remain free to
    // move throughout their arc. The same 15% stage budget applies.
    for(std::size_t g=0;g<2;++g) {
        auto& group=(*groups)[g]; std::vector<double> coordinate[3],distance;
        for(const auto& p:group) {
            coordinate[0].push_back(p.p.X); coordinate[1].push_back(p.p.Y); coordinate[2].push_back(p.p.Z);
        }
        const FVector median{Quantile(coordinate[0],.5),Quantile(coordinate[1],.5),Quantile(coordinate[2],.5)};
        for(const auto& p:group) distance.push_back(Length(p.p-median));
        const double gate=std::max(4.0,4*Quantile(distance,.5)); unsigned accepted=0;
        for(std::size_t i=0;i<group.size();++i) { group[i].weight=distance[i]<=gate?1:0; accepted+=group[i].weight>0; }
        if(accepted<3||accepted*20<group.size()*17) return false;
    }
    return true;
}
double PositionNoise(const PositionGroups& groups) {
    double noise=0;
    // Endpoint holds provide a noise/motion scale. A median estimate keeps
    // the permitted isolated spikes from inventing a third spatial axis.
    for(std::size_t g=0;g<std::min(std::size_t(2),groups.size());++g) {
        std::vector<double> coordinate[3],squared;
        for(const auto& p:groups[g]) if(p.weight>0) {
            coordinate[0].push_back(p.p.X); coordinate[1].push_back(p.p.Y); coordinate[2].push_back(p.p.Z);
        }
        const FVector median{Quantile(coordinate[0],.5),Quantile(coordinate[1],.5),Quantile(coordinate[2],.5)};
        for(const auto& p:groups[g]) if(p.weight>0) squared.push_back(Dot(p.p-median,p.p-median));
        noise=std::max(noise,Quantile(squared,.5)/3);
    }
    return noise;
}
double MeanRadius(const PositionGroups& groups,FVector centre) {
    double mean=0,total=0;
    for(const auto& group:groups) {
        double sum=0,radius=0;
        for(const auto& p:group) { sum+=p.weight; radius+=p.weight*Length(p.p-centre); }
        if(sum>0) { mean+=radius/sum; ++total; }
    }
    return total>0?mean/total:0;
}
PositionGroups PositionSupport(const PositionGroups& groups) {
    // A single off-plane spike can lie exactly on a different sphere. Radial
    // consensus alone would then manufacture a measured third direction.
    // Require the spatial subspace to survive removal of the most influential
    // <=15% of accepted observations in EACH stage, then recompute its axes.
    FVector mean; double total=0;
    for(const auto& group:groups) {
        double sum=0; for(const auto& p:group) sum+=p.weight;
        if(sum<=0) continue;
        for(const auto& p:group) mean=mean+p.p*(p.weight/sum);
        ++total;
    }
    if(total<=0) return groups;
    mean=mean*(1/total); Matrix covariance{};
    for(const auto& group:groups) {
        double sum=0; for(const auto& p:group) sum+=p.weight;
        if(sum<=0) continue;
        for(const auto& p:group) {
            const auto a=Array(p.p-mean); const double w=p.weight/(sum*total);
            for(int i=0;i<3;++i) for(int j=0;j<3;++j) covariance[i][j]+=w*a[i]*a[j];
        }
    }
    std::array<double,3> eigen; Matrix axes; Eigen(covariance,&eigen,&axes);
    auto supported=groups;
    for(auto& group:supported) {
        std::vector<std::pair<double,std::size_t>> leverage;
        for(std::size_t i=0;i<group.size();++i) if(group[i].weight>0) {
            double value=0;
            for(int j=0;j<3;++j) if(eigen[j]>1e-9) {
                const double projection=Dot(Column(axes,j),group[i].p-mean);
                value+=projection*projection/eigen[j];
            }
            leverage.push_back({value,i});
        }
        std::sort(leverage.begin(),leverage.end(),[](const auto& a,const auto& b){ return a.first>b.first; });
        for(std::size_t i=0;i<leverage.size()*15/100;++i) group[leverage[i].second].weight=0;
    }
    return supported;
}
PositionModel PositionSolve(const PositionGroups& input,FVector prior,double noise,double extra_floor=0) {
    const auto groups=PositionSupport(input);
    PositionModel model; model.centre=prior;
    FVector mean; double total=0;
    for(const auto& group:groups) {
        double sum=0; for(const auto& p:group) sum+=p.weight;
        if(sum<=0) continue;
        for(const auto& p:group) mean=mean+p.p*(p.weight/sum);
        ++total;
    }
    if(total<=0) return model;
    mean=mean*(1/total);
    Matrix covariance{}; FVector rhs;
    for(const auto& group:groups) {
        double sum=0; for(const auto& p:group) sum+=p.weight;
        if(sum<=0) continue;
        for(const auto& p:group) {
            const double weight=p.weight/(sum*total);
            const auto x=p.p-mean; const auto a=Array(x);
            rhs=rhs+x*(.5*Dot(x,x)*weight);
            for(int i=0;i<3;++i) for(int j=0;j<3;++j) covariance[i][j]+=weight*a[i]*a[j];
        }
    }
    Eigen(covariance,&model.eigen,&model.axes);
    const double largest=std::max(0.0,*std::max_element(model.eigen.begin(),model.eigen.end()));
    const double floor=std::max({1.0,.002*largest,9*noise,extra_floor});
    FVector delta;
    for(int i=0;i<3;++i) {
        const auto axis=Column(model.axes,i);
        model.use[i]=model.eigen[i]>=floor;
        if(model.use[i]) { delta=delta+axis*(Dot(axis,rhs)/model.eigen[i]); ++model.dimensions; }
        else delta=delta+axis*Dot(axis,prior-mean);
    }
    model.centre=mean+delta;
    model.radius=MeanRadius(groups,model.centre);
    model.condition=largest>1e-12?std::max(0.0,*std::min_element(model.eigen.begin(),model.eigen.end()))/largest:0;
    return model;
}
bool Plausible(const PositionModel& model) {
    return Finite(model.centre)&&Length(model.centre)<=150&&std::isfinite(model.radius)&&model.radius>=25&&model.radius<=110;
}
// Radial Gauss--Newton refinement has unit spatial derivatives, unlike the
// squared-distance algebraic seed. Only observed directions may move.
void RefinePosition(const PositionGroups& groups,PositionModel* model,int iterations) {
    model->radius=MeanRadius(groups,model->centre);
    if(model->dimensions==0) return;
    for(int iteration=0;iteration<iterations;++iteration) {
        FVector mean_normal; double mean_distance=0,total=0;
        std::vector<std::vector<double>> weights(groups.size());
        for(std::size_t g=0;g<groups.size();++g) {
            double sum=0; weights[g].resize(groups[g].size());
            for(std::size_t j=0;j<groups[g].size();++j) {
                const auto& p=groups[g][j]; const double distance=Length(p.p-model->centre);
                const double error=std::fabs(distance-model->radius);
                weights[g][j]=p.weight*(error>1.5?1.5/error:1); sum+=weights[g][j];
            }
            if(sum<=0) continue;
            for(std::size_t j=0;j<groups[g].size();++j) {
                weights[g][j]/=sum;
                const auto d=groups[g][j].p-model->centre; const double length=Length(d);
                if(length<1e-9) continue;
                mean_normal=mean_normal+d*(weights[g][j]/length);
                mean_distance+=weights[g][j]*length;
            }
            ++total;
        }
        if(total<=0) return;
        mean_normal=mean_normal*(1/total); mean_distance/=total;
        Matrix normal{}; FVector rhs;
        for(std::size_t g=0;g<groups.size();++g) for(std::size_t j=0;j<groups[g].size();++j) {
            const auto d=groups[g][j].p-model->centre; const double length=Length(d);
            if(length<1e-9) continue;
            const auto n=d*(1/length)-mean_normal;
            FVector projected;
            for(int k=0;k<3;++k) if(model->use[k]) projected=projected+Column(model->axes,k)*Dot(Column(model->axes,k),n);
            const double w=weights[g][j]/total; const auto a=Array(projected);
            rhs=rhs+projected*((length-mean_distance)*w);
            for(int i=0;i<3;++i) for(int k=0;k<3;++k) normal[i][k]+=w*a[i]*a[k];
        }
        std::array<double,3> eigen; Matrix axes; Eigen(normal,&eigen,&axes);
        FVector step;
        for(int i=0;i<3;++i) if(eigen[i]>1e-9) {
            const auto axis=Column(axes,i); step=step+axis*(Dot(axis,rhs)/eigen[i]);
        }
        FVector projected_step;
        for(int i=0;i<3;++i) if(model->use[i]) projected_step=projected_step+Column(model->axes,i)*Dot(Column(model->axes,i),step);
        step=projected_step;
        const double distance=Length(step); if(!std::isfinite(distance)) return;
        if(distance>5) step=step*(5/distance);
        model->centre=model->centre+step; model->radius=MeanRadius(groups,model->centre);
        if(distance<1e-7) break;
    }
}
struct PositionScore { bool supported=false; double loss=1e9,worst=1e9; unsigned inliers=0,samples=0; };
PositionScore ScorePosition(const PositionGroups& groups,const PositionModel& model) {
    PositionScore score;
    if(!Plausible(model)) return score;
    score.supported=true; score.loss=score.worst=0;
    for(const auto& group:groups) {
        unsigned count=0; double square=0,loss=0;
        for(const auto& p:group) {
            if(p.weight<=0) { loss+=16; continue; }
            const double residual=std::fabs(Length(p.p-model.centre)-model.radius);
            loss+=std::min(16.0,residual*residual);
            if(residual<=4) { ++count; square+=residual*residual; }
        }
        score.samples+=unsigned(group.size()); score.inliers+=count;
        if(count<3||count*20<group.size()*17) score.supported=false;
        score.loss+=loss/group.size();
        score.worst=std::max(score.worst,count?std::sqrt(square/count):1e9);
    }
    score.loss/=groups.size();
    return score;
}
bool TrimPosition(const PositionGroups& source,const PositionModel& model,PositionGroups* trimmed) {
    *trimmed=source;
    for(auto& group:*trimmed) {
        unsigned count=0;
        for(auto& p:group) { p.weight=p.weight>0&&std::fabs(Length(p.p-model.centre)-model.radius)<=4?1:0; count+=p.weight>0; }
        if(count<3||count*20<group.size()*17) return false;
    }
    return true;
}
} // namespace

Result FitPositions(const std::vector<Group>& input,FVector priorShoulder,double priorReach) {
    Result result;
    if(input.size()<2||input.size()>8||!Finite(priorShoulder)||Length(priorShoulder)>150||
       !std::isfinite(priorReach)||priorReach<25||priorReach>110) return result;
    PositionGroups groups;
    for(const auto& source:input) {
        if(source.size()<3||source.size()>1024) return result;
        auto& group=groups.emplace_back();
        for(const auto& sample:source) {
            if(!Finite(sample.wrist)||Length(sample.wrist)>1000) return Result{};
            group.push_back({sample.wrist,1});
        }
    }
    if(!TrimEndpointSpikes(&groups)) return result;
    const double noise=PositionNoise(groups);
    PositionModel best; PositionScore best_score;
    auto consider=[&](PositionModel candidate) {
        const auto score=ScorePosition(groups,candidate);
        if(score.supported&&score.loss<best_score.loss) { best=candidate; best_score=score; }
    };
    consider(PositionSolve(groups,priorShoulder,noise));
    PositionModel prior; prior.centre=priorShoulder; prior.radius=MeanRadius(groups,priorShoulder); consider(prior);
    // Bounded deterministic seeds resist the high leverage of position spikes.
    // Each seed includes every stage; a long pause cannot erase a bad stage.
    unsigned random=0x9e3779b9u;
    auto pick=[&](std::size_t count) { random^=random<<13; random^=random>>17; random^=random<<5; return std::size_t(random)%count; };
    for(int trial=0;trial<96;++trial) {
        PositionGroups seed(groups.size());
        for(std::size_t g=0;g<groups.size();++g) for(int k=0;k<3;++k) {
            std::size_t chosen=pick(groups[g].size());
            while(groups[g][chosen].weight<=0) chosen=(chosen+1)%groups[g].size();
            seed[g].push_back(groups[g][chosen]);
        }
        consider(PositionSolve(seed,priorShoulder,noise));
    }
    if(!best_score.supported) return result;
    PositionGroups kept;
    double extra_floor=0;
    bool stable=false;
    for(int iteration=0;iteration<8;++iteration) {
        if(!TrimPosition(groups,best,&kept)) return Result{};
        best=PositionSolve(kept,priorShoulder,PositionNoise(kept),extra_floor);
        RefinePosition(kept,&best,12);
        best_score=ScorePosition(groups,best);
        if(!best_score.supported||!Plausible(best)) return Result{};
        // A deterministic residual-amplification indicator, not a confidence
        // interval. Weak directions are retained from the prior, never hidden
        // inside an all-or-nothing shoulder replacement.
        const double sensitivity_floor=std::pow(best_score.worst*best.radius/8.0,2);
        extra_floor=std::max(extra_floor,sensitivity_floor);
        bool demote=false;
        for(int i=0;i<3;++i) demote|=best.use[i]&&best.eigen[i]<extra_floor;
        PositionGroups next;
        if(!TrimPosition(groups,best,&next)) return Result{};
        bool same=true;
        for(std::size_t g=0;g<kept.size();++g) for(std::size_t i=0;i<kept[g].size();++i)
            same&=kept[g][i].weight==next[g][i].weight;
        // A rejected spike must not permanently raise the noise threshold
        // after it has left the accepted set. Re-evaluate that set's own
        // supported directions before applying its sensitivity limit.
        if(!same) { extra_floor=0; continue; }
        if(!demote) { stable=true; break; }
    }
    if(!stable||best_score.worst>2||!Plausible(best)) return Result{};
    result.valid=true; result.shoulder=best.centre; result.reach=best.radius;
    result.observed_dimensions=best.dimensions; result.observed=best.dimensions==3;
    result.rms=best_score.worst; result.condition=best.condition;
    result.samples=best_score.samples; result.inliers=best_score.inliers;
    for(int i=0;i<3;++i) if(best.use[i]) result.pivot_sensitivity_cm=std::max(result.pivot_sensitivity_cm,
        result.rms*result.reach/std::sqrt(best.eigen[i]));
    if(result.pivot_sensitivity_cm>8.0+1e-7) return Result{};
    return result;
}

} // namespace holevr::vr::functional_reach
