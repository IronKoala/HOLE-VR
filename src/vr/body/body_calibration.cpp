#include "vr/body/body_calibration.h"
#include "vr/body/functional_reach.h"
#include "vr/body/elbow_calibration.h"
#include <algorithm>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <sstream>

namespace holevr::vr::body_calibration {
namespace {
constexpr std::size_t kMaxArc=256; // legacy persisted sample bound
constexpr std::size_t kMaxHeld=160;
double Dot(FVector a,FVector b) { return a.X*b.X+a.Y*b.Y+a.Z*b.Z; }
double Length(FVector a) { return std::sqrt(Dot(a,a)); }
bool Finite(FVector v) { return std::isfinite(v.X)&&std::isfinite(v.Y)&&std::isfinite(v.Z); }
double Side(int side) { return side==0?-1.0:1.0; }
bool Unit(FQuat q,FQuat* out) {
    const double n=std::sqrt(q.X*q.X+q.Y*q.Y+q.Z*q.Z+q.W*q.W);
    if(!std::isfinite(n)||n<1e-9) return false;
    *out={q.X/n,q.Y/n,q.Z/n,q.W/n}; return true;
}
bool HeightValid(double cm,HeightPosture p) {
    return std::isfinite(cm) && (p==HeightPosture::Standing ? cm>=80&&cm<=230 :
        p==HeightPosture::Seated ? cm>=40&&cm<=200 : false);
}
bool HeightContextValid(HeightMode m,HeightPosture p) {
    return (m==HeightMode::Fixed||m==HeightMode::Real||m==HeightMode::Seated)&&
        (p==HeightPosture::Unspecified||p==HeightPosture::Standing||p==HeightPosture::Seated);
}
bool ShoulderOK(FVector p,int side) {
    return Finite(p)&&std::fabs(p.X)<=35&&p.Z>=-55&&p.Z<=-5&&p.Y*Side(side)>=4&&p.Y*Side(side)<=40;
}
double Quantile(std::vector<double> v,double fraction) {
    if(v.empty()) return 0;
    std::sort(v.begin(),v.end()); const double index=fraction*(v.size()-1);
    const auto lo=std::size_t(index); return v[lo]+(v[std::min(lo+1,v.size()-1)]-v[lo])*(index-lo);
}
bool SettingsOK(const Settings& s) {
    const double values[]={s.preparation_seconds,s.hold_seconds,s.hold_rms_cm,s.hold_drift_cm,
        s.orientation_rms_deg,s.max_head_rotation_deg,s.observation_gap_seconds,
        s.timeout_seconds,s.stage_timeout_seconds,s.bend_min_span_deg};
    for(double v:values) if(!std::isfinite(v)||v<0) return false;
    return s.preparation_seconds<=15&&s.hold_seconds>=.5&&s.hold_seconds<=2.5&&
        s.hold_rms_cm>=.5&&s.hold_rms_cm<=5&&s.hold_drift_cm>=.5&&s.hold_drift_cm<=8&&
        s.orientation_rms_deg>=2&&s.orientation_rms_deg<=20&&s.max_head_rotation_deg>=10&&
        s.max_head_rotation_deg<=60&&s.observation_gap_seconds>=.2&&s.observation_gap_seconds<=2&&
        s.stage_timeout_seconds>=10&&s.stage_timeout_seconds<=300&&
        s.timeout_seconds>=s.stage_timeout_seconds&&s.timeout_seconds<=1200&&
        s.bend_min_span_deg>=15&&s.bend_min_span_deg<=60;
}
}

HeightPosture DefaultHeightPosture(HeightMode mode) {
    return mode==HeightMode::Real?HeightPosture::Standing:
        mode==HeightMode::Seated?HeightPosture::Seated:HeightPosture::Unspecified;
}
Session::Session(const Settings& settings):settings_(settings) { held_.reserve(kMaxHeld); bent_.reserve(256); Reset(); }
void Session::Reset() {
    status_={}; profile_={}; started_=have_baseline_=false;
    started_at_=last_time_=preparation_until_=stage_started_at_=0;
    tracking_interrupted_=skip_arms_=height_done_=height_floor_known_=false;
    held_.clear(); bent_.clear(); straight_motion_.clear(); for(auto& h:straight_hold_) h.clear();
    for(int s=0;s<2;++s) { reach_[s]=0; prior_fraction_[s]=.47; prior_shoulder_[s]={-5,Side(s)*20,-25}; prior_reach_[s]=60; forward_[s]={}; straight_[s]={}; }
    if(!SettingsOK(settings_)) Fail(Reason::InvalidSample);
}
void Session::Enter(Phase phase,double seconds,double preparation) {
    status_.phase=phase; status_.reason=Reason::None; status_.hold_fraction=0;
    status_.pose_ready=false; status_.pose_confirmed=false; status_.bend_ready=false; status_.bend_pose_bent=false;
    status_.stage_elapsed_seconds=0; stage_started_at_=seconds;
    preparation_until_=seconds+preparation; status_.preparation_remaining_seconds=preparation;
    held_.clear();
}
void Session::Fail(Reason reason) {
    status_.phase=Phase::Failed; status_.reason=reason; status_.hold_fraction=0;
    status_.pose_ready=status_.baseline_locked=status_.arms_measured=false;
    status_.preparation_remaining_seconds=0; profile_={};
}
void Session::CompleteHeightOnly() {
    const double standing=profile_.standing_eye_cm,seated=profile_.seated_eye_cm;
    profile_={}; profile_.arms_measured=false;
    profile_.standing_eye_cm=standing; profile_.seated_eye_cm=seated;
    status_.phase=Phase::Complete; status_.arms_measured=false; status_.baseline_locked=false;
    status_.preparation_remaining_seconds=0; status_.hold_fraction=1;
    status_.reason=status_.height_measured?Reason::None:
        (height_posture_==HeightPosture::Unspecified?Reason::HeightUnspecified:Reason::HeightUnavailable);
}
bool Session::Hold(const HeldSample& sample,FVector mean[2]) {
    // A user-confirmed representative window, not a stillness test. Movement
    // remains in the fit; model residuals/observability decide what it supports.
    if(held_.empty()||sample.seconds-held_.back().seconds>=1.0/60.0) {
        if(held_.size()==kMaxHeld) held_.erase(held_.begin());
        held_.push_back(sample);
    }
    while(held_.size()>3&&held_[1].seconds<=sample.seconds-settings_.hold_seconds) held_.erase(held_.begin());
    status_.pose_rms_cm=status_.pose_drift_cm=status_.orientation_rms_deg=0;
    status_.hold_fraction=std::clamp((sample.seconds-held_.front().seconds)/settings_.hold_seconds,0.0,1.0);
    for(int side=0;side<2;++side) {
        std::vector<double> x,y,z;
        for(const auto& h:held_) { x.push_back(h.wrist[side].X); y.push_back(h.wrist[side].Y); z.push_back(h.wrist[side].Z); }
        const FVector median{Quantile(x,.5),Quantile(y,.5),Quantile(z,.5)};
        std::vector<double> distances,deviations;
        for(const auto& h:held_) distances.push_back(Length(h.wrist[side]-median));
        const double typical=Quantile(distances,.5);
        for(double d:distances) deviations.push_back(std::fabs(d-typical));
        const double glitch_gate=std::max(10.0,typical+6*Quantile(deviations,.5));
        std::vector<const HeldSample*> good;
        for(const auto& h:held_) if(Length(h.wrist[side]-median)<=glitch_gate) good.push_back(&h);
        if(good.size()<held_.size()*.85||good.empty()) { status_.reason=Reason::InvalidSample; return false; }
        mean[side]={}; for(const auto* h:good) mean[side]=mean[side]+h->wrist[side];
        mean[side]=mean[side]*(1.0/good.size());
        double square=0;
        for(const auto* h:good) { const double d=Length(h->wrist[side]-mean[side]); square+=d*d; }
        const double rms=std::sqrt(square/good.size());
        const std::size_t count=std::max(std::size_t(1),good.size()/3);
        FVector first,last;
        for(std::size_t i=0;i<count;++i) { first=first+good[i]->wrist[side]; last=last+good[good.size()-1-i]->wrist[side]; }
        const double drift=Length((last-first)*(1.0/count));
        status_.pose_rms_cm=std::max(status_.pose_rms_cm,rms);
        status_.pose_drift_cm=std::max(status_.pose_drift_cm,drift);
        if(status_.phase!=Phase::MeasureHeight) {
            const FQuat reference=good.front()->grip[side]; FQuat sum{0,0,0,0},average;
            for(const auto* h:good) {
                const auto q=h->grip[side];
                const double sign=q.X*reference.X+q.Y*reference.Y+q.Z*reference.Z+q.W*reference.W<0?-1.:1.;
                sum.X+=sign*q.X; sum.Y+=sign*q.Y; sum.Z+=sign*q.Z; sum.W+=sign*q.W;
            }
            if(!Unit(sum,&average)) { status_.reason=Reason::WristPoseMismatch; return false; }
            square=0; unsigned outliers=0;
            for(const auto* h:good) {
                const auto q=h->grip[side];
                const double dot=std::fabs(q.X*average.X+q.Y*average.Y+q.Z*average.Z+q.W*average.W);
                const double angle=2*std::acos(std::clamp(dot,0.0,1.0))*kRadToDeg;
                if(angle>30) ++outliers; else square+=angle*angle;
            }
            const double angular=std::sqrt(square/std::max(std::size_t(1),good.size()-outliers));
            status_.orientation_rms_deg=std::max(status_.orientation_rms_deg,angular);
        }
    }
    return held_.size()>=6&&sample.seconds-held_.front().seconds>=settings_.hold_seconds;
}
bool Session::FitArms() {
    // Learn the spatial directions supported by comfortable reach motion.
    // An unobserved direction retains its prior; it never freezes the other
    // observable coordinates or turns an assumed shoulder into a pose gate.
    Profile candidate=profile_;
    bool degraded=false;
    for(int side=0;side<2;++side) {
        std::vector<functional_reach::Group> groups(2);
        for(int stage=0;stage<2;++stage) for(const auto& h:straight_hold_[stage])
            groups[stage].push_back({h.wrist[side],h.grip[side]});
        if(straight_motion_.size()>=3) {
            groups.emplace_back();
            for(const auto& h:straight_motion_) groups.back().push_back({h.wrist[side],h.grip[side]});
        }
        auto fit=functional_reach::FitPositions(groups,prior_shoulder_[side],prior_reach_[side]);
        // A bent or interrupted transition must not poison every later hold.
        // Endpoint-only recovery is explicitly conditional on the prior.
        if(!fit.valid && groups.size()>2) {
            groups.resize(2);
            fit=functional_reach::FitPositions(groups,prior_shoulder_[side],prior_reach_[side]);
        }
        const auto shoulder=fit.shoulder;
        const double reach=fit.reach;
        if(!fit.valid||!ShoulderOK(shoulder,side)||reach<25||reach>110) {
            status_.rejected_side=side; status_.reason=Reason::BadArcFit; return false;
        }
        auto& arm=candidate.arm[side]; arm.shoulder=shoulder; reach_[side]=reach;
        arm.shoulder_measured=fit.observed; degraded|=!fit.observed;
        arm.shoulder_observed_dimensions=fit.observed_dimensions;
        arm.upper_cm=reach*(1-prior_fraction_[side]); arm.forearm_cm=reach*prior_fraction_[side];
        arm.segments_measured=false; arm.forearm_measured=false;
        arm.fit.radial_rms_cm=fit.rms;
        const auto forward_vector=forward_[side]-shoulder, out_vector=straight_[side]-shoulder;
        arm.fit.arc_degrees=std::acos(std::clamp(Dot(forward_vector,out_vector)/
            (Length(forward_vector)*Length(out_vector)),-1.0,1.0))*kRadToDeg;
        // No prescribed opening angle. The fit's motion excitation and
        // uncertainty limits determine whether this person's motion helps.
        if(!std::isfinite(arm.fit.arc_degrees)) {
            status_.rejected_side=side; status_.reason=Reason::BadArcFit; return false;
        }
        arm.fit.samples=fit.samples; arm.fit.inliers=fit.inliers;
        std::vector<wrist_calibration::Observation> observations;
        for(int stage=0;stage<2;++stage) for(const auto& h:straight_hold_[stage]) {
            const auto direction=h.wrist[side]-shoulder;
            const double length=Length(direction);
            if(length<1) { status_.reason=Reason::BadArcFit; return false; }
            const auto axis=direction*(1.0/length);
            const FVector cue=stage==0?FVector{0,-Side(side),0}:FVector{0,0,-1};
            // Down/inward is a roll cue, not a demand for a palm plane that
            // becomes anatomically impossible as comfortable arms slope down.
            // Project it here using the functional arm direction. Avoid the
            // poorly defined case where that direction approaches the cue.
            const auto palm=cue-axis*Dot(cue,axis);
            if(Length(palm)<.1) { status_.reason=Reason::WristPoseMismatch; return false; }
            observations.push_back({stage==0?wrist_calibration::Stage::Forward:wrist_calibration::Stage::Out,
                h.grip[side],direction,palm,true});
        }
        const auto wrist=wrist_calibration::Fit(observations,frozen_offset_[side]);
        if(wrist.reason!=wrist_calibration::Reason::None) {
            status_.rejected_side=side; status_.reason=Reason::WristPoseMismatch; return false;
        }
        arm.wrist=wrist.calibration; arm.hand_position_setting_game_cm=frozen_position_setting_[side];
        arm.hand_rotation_setting=frozen_rotation_setting_[side];
    }
    const double width=candidate.arm[1].shoulder.Y-candidate.arm[0].shoulder.Y;
    if(width<12||width>80) { status_.reason=Reason::BadArcFit; return false; }
    profile_=candidate; status_.reach_degraded=degraded; status_.rejected_side=-1; return true;
}

Reason FitSegments(const FVector& shoulder,double total,const wrist_calibration::Calibration& wrist,
                   const std::vector<BendObservation>& observations,double minimum_span,SegmentFit* out) {
    if(!out || !Finite(shoulder) || !std::isfinite(total) || total<25 || total>110 ||
       !std::isfinite(minimum_span) || minimum_span<15 || minimum_span>60 ||
       !wrist.measured || !wrist_calibration::Validate(wrist) || observations.size()>256)
        return Reason::InvalidSample;
    struct Point { FVector d,f; double a,b,length; };
    std::vector<Point> points;
    for(const auto& sample:observations) {
        FQuat q;
        if(!Finite(sample.wrist) || !Unit(sample.grip_rotation,&q)) return Reason::InvalidSample;
        const FVector d=sample.wrist-shoulder,f=q.Rotate(wrist.forearm_axis_grip);
        const double distance=Length(d),a=2*(total-Dot(d,f)),b=total*total-Dot(d,d);
        // Almost straight arms contain essentially no information about the
        // segment split; repeating that pose cannot create extra evidence.
        if(distance<.20*total || distance>.985*total || a<.12*total) continue;
        const double length=b/a;
        if(std::isfinite(length) && length>5 && length<90) points.push_back({d,f,a,b,length});
    }
    if(points.size()<10) return Reason::MoreBendMotion;
    auto quantile=[](std::vector<double> v,double f) {
        std::sort(v.begin(),v.end());
        const double index=f*(v.size()-1); const auto lo=std::size_t(index);
        return v[lo]+(v[std::min(lo+1,v.size()-1)]-v[lo])*(index-lo);
    };
    std::vector<double> lengths;
    for(const auto& p:points) lengths.push_back(p.length);
    const double median=quantile(lengths,.5);
    std::vector<double> deviation;
    for(double v:lengths) deviation.push_back(std::fabs(v-median));
    const double gate=std::clamp(3*quantile(deviation,.5),2.0,4.0);
    std::vector<Point> accepted;
    double aa=0,ab=0;
    for(const auto& p:points) if(std::fabs(p.length-median)<=gate) {
        accepted.push_back(p); aa+=p.a*p.a; ab+=p.a*p.b;
    }
    if(accepted.size()<10 || accepted.size()<points.size()*.80 || aa<1e-6) return Reason::UncertainBend;
    const double lower=ab/aa,upper=total-lower;
    if(upper<10 || upper>75 || lower<10 || lower>70 || lower/total<.20 || lower/total>.80)
        return Reason::UncertainBend;
    std::vector<double> angles,inlier_lengths;
    double residual2=0,xmean=0,ymean=0;
    for(const auto& p:accepted) {
        const FVector u=p.d-p.f*lower;
        const double ul=Length(u);
        if(ul<1) return Reason::UncertainBend;
        const double residual=ul-upper;
        residual2+=residual*residual;
        angles.push_back(std::acos(std::clamp(Dot(u,p.f)/ul,-1.0,1.0))*kRadToDeg);
        inlier_lengths.push_back(p.length);
        xmean+=Dot(p.d,p.f); ymean+=Dot(p.d,p.d);
    }
    const double span=quantile(angles,.95)-quantile(angles,.05);
    if(span<minimum_span) return Reason::MoreBendMotion;
    const double rms=std::sqrt(residual2/accepted.size());
    const double spread=quantile(inlier_lengths,.90)-quantile(inlier_lengths,.10);
    if(rms>1.5 || spread>std::max(3.0,total*.06)) return Reason::UncertainBend;
    // Independent two-parameter regression does NOT force upper+lower to
    // the straight-arm radius. Require it to agree, rather than rescaling a
    // single idealized bent endpoint until the sum happens to match.
    xmean/=accepted.size(); ymean/=accepted.size();
    double variance=0,covariance=0;
    for(const auto& p:accepted) {
        const double dx=Dot(p.d,p.f)-xmean;
        variance+=dx*dx; covariance+=dx*(Dot(p.d,p.d)-ymean);
    }
    if(std::sqrt(variance/accepted.size())<std::max(1.0,total*.02)) return Reason::MoreBendMotion;
    const double free_lower=covariance/(2*variance),constant=ymean-2*free_lower*xmean;
    const double square=constant+free_lower*free_lower;
    if(square<=0 || free_lower<10 || free_lower>70) return Reason::UncertainBend;
    const double free_upper=std::sqrt(square);
    if(std::fabs(free_lower-lower)>3.5 || std::fabs(free_upper+free_lower-total)>std::max(2.5,.05*total))
        return Reason::UncertainBend;
    *out={upper,lower,rms,span,spread,unsigned(points.size()),unsigned(accepted.size())};
    return Reason::None;
}



void Session::FitBent() {
    status_.bend_degraded=false; status_.rejected_side=-1;
    status_.bend_fit_error_cm=status_.bend_span_degrees=0;
    for(int side=0;side<2;++side) {
        auto& arm=profile_.arm[side];
        std::vector<elbow_calibration::Observation> observations;
        for(const auto& p:bent_) observations.push_back({p.wrist[side],p.grip[side].Rotate(arm.wrist.forearm_axis_grip)});
        const auto fit=elbow_calibration::Fit(observations);
        const double upper=Length(fit.elbow-arm.shoulder), total=upper+fit.forearm_cm;
        auto reason=fit.valid?Reason::None:Reason::UncertainBend;
        // Independent motion estimates E and F without the shoulder or reach.
        // Only now compare the result with the separate straight-arm fit.
        // Correlated elbow translation can mimic forearm length, so a small
        // residual alone is not enough to claim a usable segment split.
        // A split no human arm has means the elbow itself moved while bending.
        if(reason==Reason::None && (upper<10||upper>75||fit.forearm_cm<10||fit.forearm_cm>70||
            fit.forearm_cm/total<.20||fit.forearm_cm/total>.80||!PlausibleSplit(upper,fit.forearm_cm)||
            std::fabs(total-reach_[side])>std::max(3.0,.06*reach_[side]))) reason=Reason::UncertainBend;
        status_.bend_reason[side]=reason;
        if(reason==Reason::None) {
            arm.upper_cm=upper; arm.forearm_cm=fit.forearm_cm; arm.forearm_measured=true;
            arm.segments_measured=arm.shoulder_measured;
            status_.bend_degraded|=!arm.segments_measured;
            arm.fit.bent_error_cm=fit.rms_cm; arm.fit.bend_span_degrees=fit.span_degrees;
            arm.fit.bend_length_spread_cm=fit.uncertainty_cm; arm.fit.bend_samples=fit.samples; arm.fit.bend_inliers=fit.inliers;
            status_.bend_fit_error_cm=std::max(status_.bend_fit_error_cm,fit.rms_cm);
            status_.bend_span_degrees=std::max(status_.bend_span_degrees,fit.span_degrees);
        } else {
            arm.segments_measured=false; arm.forearm_measured=false; status_.bend_degraded=true; status_.rejected_side=side;
            // Unknown residual is represented by the rejection reason, not
            // reported as a successful zero-error fit. Do not fabricate evidence.
            arm.fit.bend_samples=unsigned(observations.size());
            arm.fit.bend_inliers=0;
        }
    }
}
bool Session::Verify(const FVector mean[2]) {
    status_.verification_error_cm=0;
    for(int side=0;side<2;++side) {
        auto& arm=profile_.arm[side];
        const double position_error=Length(mean[side]-forward_[side]);
        const double length_error=std::fabs(Length(mean[side]-arm.shoulder)-reach_[side]);
        const double error=std::max(position_error,length_error);
        status_.verification_error_cm=std::max(status_.verification_error_cm,error);
        if(error>std::max(6.0,.10*reach_[side])) { status_.rejected_side=side; return false; }
        double square=0,palm_square=0; unsigned inliers=0;
        for(const auto& h:held_) {
            auto direction=h.wrist[side]-arm.shoulder;
            const double length=Length(direction); if(length<1) continue;
            direction=direction*(1.0/length);
            FVector target_palm{0,-Side(side),0};
            target_palm=target_palm-direction*Dot(target_palm,direction);
            const double palm_length=Length(target_palm); if(palm_length<.1) continue;
            target_palm=target_palm*(1.0/palm_length);
            const auto palm=h.grip[side].Rotate(arm.wrist.palm_axis_grip);
            const double palm_angle=std::acos(std::clamp(Dot(target_palm,palm),-1.0,1.0))*kRadToDeg;
            const FVector measured=h.grip[side].Rotate(arm.wrist.forearm_axis_grip);
            const double angle=std::acos(std::clamp(Dot(direction,measured),-1.0,1.0))*kRadToDeg;
            if(angle>30||palm_angle>35) continue;
            ++inliers; palm_square+=palm_angle*palm_angle;
            square+=angle*angle;
        }
        if(inliers<6||inliers<held_.size()*.85||std::sqrt(square/inliers)>15||std::sqrt(palm_square/inliers)>20) {
            status_.rejected_side=side; return false;
        }
        arm.verification_error_cm=error;
    }
    return Validate(profile_);
}
Status Session::Update(const Sample& sample) {
    status_.capture_accepted=false;
    if(status_.phase==Phase::Complete||status_.phase==Phase::Failed) return status_;
    // Button release must revoke capture even if tracking repeats its last
    // timestamp. Accepted stages and the useful opening/bend motion remain.
    if(!sample.capture_held) {
        held_.clear(); status_.hold_fraction=0;
        status_.pose_confirmed=status_.pose_ready=false;
    }
    if(!std::isfinite(sample.seconds)||!HeightContextValid(sample.height_mode,sample.height_posture)) { Fail(Reason::InvalidSample); return status_; }
    skip_arms_|=sample.skip_arms;
    double gap=0;
    if(!started_) {
        started_=true; started_at_=last_time_=sample.seconds;
        height_mode_=sample.height_mode; height_posture_=sample.height_posture; status_.height_posture=height_posture_;
        origin_=sample.origin_generation;
        for(int s=0;s<2;++s) {
            if(std::isfinite(sample.prior_forearm_fraction[s])&&sample.prior_forearm_fraction[s]>=.35&&sample.prior_forearm_fraction[s]<=.65)
                prior_fraction_[s]=sample.prior_forearm_fraction[s];
            if(ShoulderOK(sample.prior_shoulder[s],s)) prior_shoulder_[s]=sample.prior_shoulder[s];
            if(std::isfinite(sample.prior_reach_cm[s])&&sample.prior_reach_cm[s]>=25&&sample.prior_reach_cm[s]<=110)
                prior_reach_[s]=sample.prior_reach_cm[s];
        }
        Enter(Phase::MeasureHeight,sample.seconds,settings_.preparation_seconds);
    } else {
        if(sample.seconds<last_time_) { Fail(Reason::TimeWentBackwards); return status_; }
        if(sample.origin_generation!=origin_) { Fail(Reason::OriginChanged); return status_; }
        if(sample.height_mode!=height_mode_||sample.height_posture!=height_posture_) { Fail(Reason::HeightModeChanged); return status_; }
        if(skip_arms_&&height_done_) { CompleteHeightOnly(); return status_; }
        if(sample.seconds==last_time_) return status_; // no stale observations counted
        gap=sample.seconds-last_time_; last_time_=sample.seconds;
        if(gap>settings_.observation_gap_seconds) { held_.clear(); status_.hold_fraction=0; tracking_interrupted_=true; }
    }
    const bool height=status_.phase==Phase::MeasureHeight;
    const bool tracked=height?(sample.head_valid||sample.valid):sample.valid&&sample.grip_orientation_valid;
    if(!tracked||tracking_interrupted_) {
        // Pause the active window, never throw away accepted stages. Origin,
        // mode and clock changes still fail transactionally above.
        stage_started_at_+=gap; preparation_until_+=gap; started_at_+=gap;
        held_.clear(); status_.hold_fraction=0; status_.pose_ready=false; status_.pose_confirmed=false;
        if(!tracked) { tracking_interrupted_=true; status_.reason=Reason::TrackingLost; return status_; }
        tracking_interrupted_=false;
    }
    status_.elapsed_seconds=sample.seconds-started_at_;
    status_.stage_elapsed_seconds=sample.seconds-stage_started_at_;
    status_.preparation_remaining_seconds=std::max(0.0,preparation_until_-sample.seconds);
    if(status_.elapsed_seconds>settings_.timeout_seconds) { Fail(Reason::TimedOut); return status_; }
    if(status_.stage_elapsed_seconds>settings_.stage_timeout_seconds) { Fail(Reason::StageTimedOut); return status_; }
    FQuat rotation;
    if(!Finite(sample.head)||!Unit(sample.head_rotation,&rotation)||
       (!height&&(!Finite(sample.wrist[0])||!Finite(sample.wrist[1])))) { Fail(Reason::InvalidSample); return status_; }
    if(status_.reason!=Reason::BadArcFit && status_.reason!=Reason::WristPoseMismatch &&
       status_.reason!=Reason::VerificationMismatch) status_.reason=Reason::None;
    if(sample.capture_held && (sample.confirm_pose || (height && skip_arms_))) {
        if(!status_.pose_confirmed) held_.clear();
        status_.pose_confirmed=true;
        preparation_until_=sample.seconds;
        status_.preparation_remaining_seconds=0;
        status_.reason=Reason::None;
    }
    status_.pose_ready=status_.pose_confirmed;
    const bool looking_forward=std::fabs(rotation.Rotate({1,0,0}).Z)<std::sin(40*kDegToRad);
    if(!looking_forward) { held_.clear(); status_.hold_fraction=0; status_.reason=Reason::LookForward; return status_; }
    FVector mean[2];
    if(height) {
        if(sample.floor_known!=height_floor_known_) { held_.clear(); height_floor_known_=sample.floor_known; }
        if(status_.pose_confirmed && Hold({sample.seconds,{sample.head,sample.head},{}},mean)) {
            height_done_=true; status_.height_measured=sample.floor_known&&HeightValid(mean[0].Z,height_posture_);
            status_.measured_eye_cm=status_.height_measured?mean[0].Z:0;
            profile_.standing_eye_cm=status_.height_measured&&height_posture_==HeightPosture::Standing?mean[0].Z:0;
            profile_.seated_eye_cm=status_.height_measured&&height_posture_==HeightPosture::Seated?mean[0].Z:0;
            status_.standing_eye_cm=profile_.standing_eye_cm;
            if(skip_arms_) CompleteHeightOnly(); else Enter(Phase::ArmsForward,sample.seconds,settings_.preparation_seconds);
            status_.capture_accepted=true;
        }
        return status_;
    }
    if(!have_baseline_) {
        head_=sample.head; head_rotation_=rotation; world_to_reference_=YawOnly(YawOf(rotation)).Inverse(); have_baseline_=true;
    }
    status_.head_motion_cm=Length(sample.head-head_);
    status_.head_rotation_deg=std::fabs(std::remainder(YawOf(rotation)-YawOf(head_rotation_),360.0));
    if(status_.head_rotation_deg>settings_.max_head_rotation_deg) {
        held_.clear(); status_.hold_fraction=0; status_.reason=Reason::LookForward; return status_;
    }
    HeldSample local{sample.seconds,{},{}};
    for(int side=0;side<2;++side) {
        // Translation-relative measurements tolerate common body sway. Head
        // motion is only a torso proxy: independent neck motion contributes
        // uncertainty, so robust windows and a separate reach check remain.
        local.wrist[side]=world_to_reference_.Rotate(sample.wrist[side]-sample.head);
        FQuat q,setting;
        if(!Unit(sample.grip_rotation[side],&q)||!Finite(sample.wrist_offset_grip_cm[side])||Length(sample.wrist_offset_grip_cm[side])>35||
           !Finite(sample.hand_position_setting_game_cm[side])||Length(sample.hand_position_setting_game_cm[side])>50||
           !Unit(sample.hand_rotation_setting[side],&setting)) { Fail(Reason::InvalidSample); return status_; }
        local.grip[side]=world_to_reference_*q;
        if(status_.baseline_locked&&(Length(sample.wrist_offset_grip_cm[side]-frozen_offset_[side])>.1||
           Length(sample.hand_position_setting_game_cm[side]-frozen_position_setting_[side])>.1||
           std::fabs(setting.X*frozen_rotation_setting_[side].X+setting.Y*frozen_rotation_setting_[side].Y+
               setting.Z*frozen_rotation_setting_[side].Z+setting.W*frozen_rotation_setting_[side].W)<std::cos(.01*kDegToRad))) {
            Fail(Reason::WristPoseMismatch); return status_;
        }
        if(!status_.baseline_locked) { frozen_offset_[side]=sample.wrist_offset_grip_cm[side]; frozen_position_setting_[side]=sample.hand_position_setting_game_cm[side]; frozen_rotation_setting_[side]=setting; }
    }
    // Capture opening motion even while the preparation card is visible.
    // Keep distinct poses instead of allowing a long endpoint dwell to erase
    // the short informative transition. Both side-specific fits share times.
    if(status_.phase==Phase::ArmsOut) {
        bool novel=straight_motion_.empty();
        if(!novel && sample.seconds-straight_motion_.back().seconds>=.05) {
            for(int side=0;side<2;++side) {
                novel|=Length(local.wrist[side]-straight_motion_.back().wrist[side])>1;
            }
        }
        if(novel) {
            if(straight_motion_.size()==96) { for(std::size_t i=0;i<48;++i) straight_motion_[i]=straight_motion_[2*i]; straight_motion_.resize(48); }
            straight_motion_.push_back(local);
        }
    }
    // The user declares a comfortable pose. No guessed shoulder/elbow or
    // prescribed controller coordinates decide whether capture may begin.
    switch(status_.phase) {
    case Phase::ArmsForward:
    case Phase::VerifyForward:
        if(status_.pose_confirmed&&Hold(local,mean)) {
            if(status_.phase==Phase::VerifyForward) {
                if(Verify(mean)) { status_.phase=Phase::Complete; status_.arms_measured=true; status_.hold_fraction=1; status_.capture_accepted=true; }
                else { status_.reason=Reason::VerificationMismatch; status_.hold_fraction=0; status_.pose_confirmed=status_.pose_ready=false; held_.clear(); }
            } else {
                for(int s=0;s<2;++s) forward_[s]=mean[s]; straight_hold_[0]=held_; status_.baseline_locked=true;
                Enter(Phase::ArmsOut,sample.seconds,settings_.preparation_seconds);
                status_.capture_accepted=true;
            }
        }
        break;
    case Phase::ArmsOut:
        if(status_.pose_confirmed&&Hold(local,mean)) {
            for(int s=0;s<2;++s) straight_[s]=mean[s]; straight_hold_[1]=held_;
            if(FitArms()) { bent_.clear(); bent_.push_back(local); Enter(Phase::BendElbows,sample.seconds,0); status_.capture_accepted=true; }
            else {
                // The combined model cannot tell which accepted pose caused
                // the disagreement. Replacing only Out traps a bad Forward
                // window forever. Keep height, explicitly request a fresh pair.
                const auto reason=status_.reason;
                for(auto& h:straight_hold_) h.clear();
                straight_motion_.clear(); bent_.clear(); status_.baseline_locked=false;
                ++status_.attempt;
                Enter(Phase::ArmsForward,sample.seconds,0);
                status_.reason=reason;
            }
        }
        break;
    case Phase::BendElbows: {
        bool novel=bent_.empty();
        if(!novel && sample.seconds-bent_.back().seconds>=.04) for(int side=0;side<2;++side) {
            const auto axis=profile_.arm[side].wrist.forearm_axis_grip;
            novel|=Dot(local.grip[side].Rotate(axis),bent_.back().grip[side].Rotate(axis))<std::cos(.5*kDegToRad)||
                Length(local.wrist[side]-bent_.back().wrist[side])>.5;
        }
        if(novel) {
            if(bent_.size()==256) { for(std::size_t i=0;i<128;++i) bent_[i]=bent_[2*i]; bent_.resize(128); }
            bent_.push_back(local); status_.bend_samples=unsigned(bent_.size());
        }
        status_.bend_pose_bent=status_.pose_confirmed;
        if(status_.pose_confirmed&&Hold(local,mean)) {
            FitBent(); status_.bend_ready=true;
            Enter(Phase::VerifyForward,sample.seconds,settings_.preparation_seconds);
            status_.capture_accepted=true;
        }
        break;
    }
    default: break;
    }
    return status_;
}

const char* Prompt(Phase phase,HeightPosture posture) {
    switch(phase) {
    case Phase::MeasureHeight: return posture==HeightPosture::Seated?"Sit comfortably and look ahead. Small movement is OK.":
        posture==HeightPosture::Standing?"Stand comfortably and look ahead. Small movement is OK.":"Keep your usual play posture. Character height stays fixed.";
    case Phase::ArmsForward: return "Reach forward comfortably. Palms face each other.";
    case Phase::ArmsOut: return "Open your arms to the sides. Palms face down.";
    case Phase::BendElbows: return "Bend your elbows so your hands rise. Let palms turn naturally.";
    case Phase::VerifyForward: return "Reach forward again. Palms face each other.";
    case Phase::Complete: return "Body fit saved. Joint positions remain estimates.";
    case Phase::Failed: return "Setup stopped. Previous calibration is kept.";
    }
    return "";
}
const char* ReasonText(Reason reason) {
    switch(reason) {
    case Reason::None: return "";
    case Reason::TrackingLost: return "Tracking paused. Completed steps are kept.";
    case Reason::InvalidSample: return "Tracking data was invalid. Previous fit kept.";
    case Reason::TimeWentBackwards: return "Tracking clock changed. Start setup again.";
    case Reason::OriginChanged: return "Tracking origin changed. Start setup again.";
    case Reason::HeadMoved: case Reason::LookForward: return "Look ahead comfortably.";
    case Reason::TimedOut: case Reason::StageTimedOut: return "Setup stopped after waiting. Previous fit kept.";
    case Reason::NotEnoughArc: case Reason::BadArcFit: return "Reach comfortably to the sides; avoid bending your elbows.";
    case Reason::UnstablePose: return "Stay near this pose. Small movement is OK.";
    case Reason::ArmsNotLevel: case Reason::HandsTooLow: return "Use a comfortable reach in the shown direction.";
    case Reason::ArmDirectionMismatch: return "Follow the current forward or sideways arm direction.";
    case Reason::BentPoseMismatch: case Reason::MoreBendMotion: return "Bend your elbows so your hands rise.";
    case Reason::UncertainBend: return "Elbow split is uncertain; keeping an estimated proportion.";
    case Reason::WristPoseMismatch: return "Follow the palm direction; avoid deliberately bending your wrists.";
    case Reason::InvalidProfile: return "The estimate was inconsistent. Previous fit kept.";
    case Reason::HeightUnavailable: return "Floor height unavailable. Existing height kept.";
    case Reason::HeightUnspecified: return "Fixed character height kept; physical posture was not assumed.";
    case Reason::HeightModeChanged: return "Height mode changed. Start setup again.";
    case Reason::VerificationMismatch: return "Return to your comfortable forward reach. Previous fit is safe.";
    }
    return "";
}

bool Validate(const Profile& profile) {
    if (profile.version!=Profile::kVersion) return false;
    if (profile.standing_eye_cm!=0 && !HeightValid(profile.standing_eye_cm,HeightPosture::Standing)) return false;
    if (profile.seated_eye_cm!=0 && !HeightValid(profile.seated_eye_cm,HeightPosture::Seated)) return false;
    if (profile.legacy_eye_cm!=0 && !HeightValid(profile.legacy_eye_cm,HeightPosture::Standing)) return false;
    if (!profile.arms_measured) return true;
    for (int side=0; side<2; ++side) {
        const auto& a=profile.arm[side]; const auto& f=a.fit;
        const double values[]={a.upper_cm,a.forearm_cm,f.radial_rms_cm,f.plane_rms_cm,f.arc_degrees,f.bent_error_cm,
            f.bend_span_degrees,f.bend_length_spread_cm,a.wrist.forearm_rms_deg,a.wrist.palm_rms_deg,a.wrist.max_error_deg};
        for (double v:values) if (!std::isfinite(v)) return false;
        FQuat setting;
        if(!Finite(a.hand_position_setting_game_cm) || Length(a.hand_position_setting_game_cm)>50 ||
           !Unit(a.hand_rotation_setting,&setting) ||
           std::fabs(a.hand_rotation_setting.X*a.hand_rotation_setting.X+a.hand_rotation_setting.Y*a.hand_rotation_setting.Y+
                     a.hand_rotation_setting.Z*a.hand_rotation_setting.Z+a.hand_rotation_setting.W*a.hand_rotation_setting.W-1)>1e-5 ||
           !Finite(a.wrist.forearm_axis_grip) || !Finite(a.wrist.palm_axis_grip) || !Finite(a.wrist.wrist_offset_cm) ||
           Length(a.wrist.wrist_offset_cm)>35 || f.bend_span_degrees<0 || f.bend_span_degrees>180 ||
           f.bend_length_spread_cm<0 || f.bend_length_spread_cm>20 || f.bend_samples>256 ||
           f.bend_inliers>f.bend_samples || a.wrist.forearm_rms_deg<0 || a.wrist.forearm_rms_deg>180 ||
           a.wrist.palm_rms_deg<0 || a.wrist.palm_rms_deg>180 || a.wrist.max_error_deg<0 || a.wrist.max_error_deg>180) return false;
        if(a.wrist.measured && !wrist_calibration::Validate(a.wrist)) return false;
        if((a.segments_measured || a.forearm_measured) && a.wrist.measured && (f.bend_inliers<10 || f.bend_span_degrees<=0)) return false;
        if(a.shoulder_observed_dimensions>3) return false;
        if(a.shoulder_measured!=(a.shoulder_observed_dimensions==3) ||
           (a.forearm_measured && !a.wrist.measured) ||
           (a.segments_measured && (!a.shoulder_measured || !a.forearm_measured))) return false;
        if(!std::isfinite(a.verification_error_cm) || a.verification_error_cm<0 || a.verification_error_cm>12) return false;
        if (!ShoulderOK(a.shoulder,side) || a.upper_cm<10 || a.upper_cm>75 || a.forearm_cm<10 ||
            a.forearm_cm>70 || a.upper_cm+a.forearm_cm<25 || a.upper_cm+a.forearm_cm>110 ||
            a.upper_cm/(a.upper_cm+a.forearm_cm)<.20 || a.upper_cm/(a.upper_cm+a.forearm_cm)>.80 ||
            f.radial_rms_cm<0 || f.radial_rms_cm>10 || f.plane_rms_cm<0 || f.plane_rms_cm>15 ||
            f.arc_degrees<0 || f.arc_degrees>180 || f.bent_error_cm<0 || f.bent_error_cm>10 ||
            f.inliers<12 || f.samples<f.inliers || f.samples>2*kMaxArc) return false;
    }
    const double width=profile.arm[1].shoulder.Y-profile.arm[0].shoulder.Y;
    return width>=12 && width<=80;
}
std::string Serialize(const Profile& profile) {
    if (!Validate(profile) || (!profile.arms_measured && profile.standing_eye_cm==0 &&
        profile.seated_eye_cm==0 && profile.legacy_eye_cm==0)) return {};
    std::ostringstream out; out.imbue(std::locale::classic()); out<<std::setprecision(17);
    out<<"version=6\nunits=cm\nmethod=tracked_body_profile\narms_measured="<<(profile.arms_measured?1:0)
       <<"\nstanding_eye_cm="<<profile.standing_eye_cm<<"\nseated_eye_cm="<<profile.seated_eye_cm
       <<"\nlegacy_eye_cm="<<profile.legacy_eye_cm<<'\n';
    for (int side=0; profile.arms_measured && side<2; ++side) {
        const auto& a=profile.arm[side]; const auto& f=a.fit;
        const std::string p=side==0?"left_":"right_";
        out<<p<<"shoulder="<<a.shoulder.X<<' '<<a.shoulder.Y<<' '<<a.shoulder.Z<<'\n';
        out<<p<<"shoulder_measured="<<(a.shoulder_measured?1:0)<<'\n';
        out<<p<<"shoulder_observed_dimensions="<<a.shoulder_observed_dimensions<<'\n';
        out<<p<<"segments_measured="<<(a.segments_measured?1:0)<<'\n'<<p<<"verification_error_cm="<<a.verification_error_cm<<'\n';
        out<<p<<"forearm_measured="<<(a.forearm_measured?1:0)<<'\n';
        out<<p<<"upper="<<a.upper_cm<<'\n'<<p<<"forearm="<<a.forearm_cm<<'\n';
        out<<p<<"radial_rms="<<f.radial_rms_cm<<'\n'<<p<<"plane_rms="<<f.plane_rms_cm<<'\n';
        out<<p<<"arc_degrees="<<f.arc_degrees<<'\n'<<p<<"bent_error="<<f.bent_error_cm<<'\n';
        out<<p<<"inliers="<<f.inliers<<'\n'<<p<<"samples="<<f.samples<<'\n';
        out<<p<<"bend_span="<<f.bend_span_degrees<<'\n'<<p<<"bend_length_spread="<<f.bend_length_spread_cm<<'\n';
        out<<p<<"bend_samples="<<f.bend_samples<<'\n'<<p<<"bend_inliers="<<f.bend_inliers<<'\n';
        out<<p<<"wrist_measured="<<(a.wrist.measured?1:0)<<'\n';
        auto vector=[&](const char* name,FVector v) { out<<p<<name<<'='<<v.X<<' '<<v.Y<<' '<<v.Z<<'\n'; };
        vector("forearm_axis_grip",a.wrist.forearm_axis_grip); vector("palm_axis_grip",a.wrist.palm_axis_grip);
        vector("wrist_offset_grip_cm",a.wrist.wrist_offset_cm); vector("hand_position_setting_game_cm",a.hand_position_setting_game_cm);
        const auto q=a.hand_rotation_setting;
        out<<p<<"hand_rotation_setting="<<q.X<<' '<<q.Y<<' '<<q.Z<<' '<<q.W<<'\n';
        out<<p<<"forearm_rms_deg="<<a.wrist.forearm_rms_deg<<'\n'<<p<<"palm_rms_deg="<<a.wrist.palm_rms_deg<<'\n';
        out<<p<<"wrist_max_error_deg="<<a.wrist.max_error_deg<<'\n';
    }
    return out.str();
}
bool Parse(const std::string& text, Profile* out) {
    if (!out || text.empty() || text.size()>8192) return false;
    std::map<std::string,std::string> fields;
    std::istringstream lines(text); std::string line;
    while (std::getline(lines,line)) {
        if (!line.empty() && line.back()=='\r') line.pop_back();
        if (line.empty()) continue;
        const auto eq=line.find('=');
        if (eq==std::string::npos || !fields.emplace(line.substr(0,eq),line.substr(eq+1)).second) return false;
    }
    auto literal=[&](const char* key,const char* value) {
        const auto i=fields.find(key);
        if (i==fields.end() || i->second!=value) return false;
        fields.erase(i); return true;
    };
    const auto version=fields.find("version");
    if (version==fields.end() || (version->second!="1" && version->second!="2" && version->second!="3" && version->second!="4" && version->second!="5" && version->second!="6")) return false;
    const unsigned source_version=unsigned(version->second[0]-'0');
    fields.erase(version);
    if (!literal("units","cm") ||
        !literal("method",source_version>=4?"tracked_body_profile":(source_version==1?"frontal_arc_bent_t":"two_arcs_bent_t"))) return false;
    auto read=[&](const std::string& key,auto&... value) {
        const auto i=fields.find(key); if (i==fields.end()) return false;
        std::istringstream in(i->second); in.imbue(std::locale::classic());
        if (!(in>>...>>value)) return false;
        in>>std::ws; if (!in.eof()) return false;
        fields.erase(i); return true;
    };
    Profile parsed;
    if (source_version!=1) {
        unsigned measured=0;
        if (!read("arms_measured",measured) || measured>1) return false;
        parsed.arms_measured=measured!=0;
    }
    if (!read("standing_eye_cm",parsed.standing_eye_cm)) return false;
    if (source_version>=3) {
        if (!read("seated_eye_cm",parsed.seated_eye_cm) || !read("legacy_eye_cm",parsed.legacy_eye_cm)) return false;
    } else {
        // Older records knew the tracking-space floor but not whether the
        // person was sitting or standing. Do not infer posture from height.
        parsed.legacy_eye_cm=parsed.standing_eye_cm; parsed.standing_eye_cm=0;
    }
    for (int side=0; parsed.arms_measured && side<2; ++side) {
        auto& a=parsed.arm[side]; auto& f=a.fit; const std::string p=side==0?"left_":"right_";
        // Legacy formats did not preserve segment confidence. Keep their
        // runtime geometry, but never promote it to a measured prior next time.
        if(source_version<6) a.segments_measured=false;
        if (!read(p+"shoulder",a.shoulder.X,a.shoulder.Y,a.shoulder.Z) || !read(p+"upper",a.upper_cm) ||
            !read(p+"forearm",a.forearm_cm) || !read(p+"radial_rms",f.radial_rms_cm) ||
            !read(p+"plane_rms",f.plane_rms_cm) || !read(p+"arc_degrees",f.arc_degrees) ||
            !read(p+"bent_error",f.bent_error_cm) || !read(p+"inliers",f.inliers) || !read(p+"samples",f.samples)) return false;
        if(source_version>=5) {
            unsigned measured=0;
            if(!read(p+"segments_measured",measured)||measured>1||!read(p+"verification_error_cm",a.verification_error_cm)) return false;
            // V5's segment solver was conditioned on its shoulder prior;
            // it did not establish the independent provenance used by v6.
            a.segments_measured=source_version>=6 && measured!=0;
            if(!read(p+"shoulder_measured",measured)||measured>1) return false;
            a.shoulder_measured=measured!=0;
            a.shoulder_observed_dimensions=a.shoulder_measured?3:0;
        }
        if(source_version>=6) {
            unsigned measured=0;
            if(!read(p+"shoulder_observed_dimensions",a.shoulder_observed_dimensions)||
               !read(p+"forearm_measured",measured)||measured>1) return false;
            a.forearm_measured=measured!=0;
        }
        if(source_version>=4) {
            unsigned measured=0;
            auto vector=[&](const char* key,FVector& v) { return read(p+key,v.X,v.Y,v.Z); };
            auto& q=a.hand_rotation_setting;
            if(!read(p+"bend_span",f.bend_span_degrees) || !read(p+"bend_length_spread",f.bend_length_spread_cm) ||
               !read(p+"bend_samples",f.bend_samples) || !read(p+"bend_inliers",f.bend_inliers) ||
               !read(p+"wrist_measured",measured) || measured>1 ||
               !vector("forearm_axis_grip",a.wrist.forearm_axis_grip) || !vector("palm_axis_grip",a.wrist.palm_axis_grip) ||
               !vector("wrist_offset_grip_cm",a.wrist.wrist_offset_cm) || !vector("hand_position_setting_game_cm",a.hand_position_setting_game_cm) ||
               !read(p+"hand_rotation_setting",q.X,q.Y,q.Z,q.W) ||
               !read(p+"forearm_rms_deg",a.wrist.forearm_rms_deg) || !read(p+"palm_rms_deg",a.wrist.palm_rms_deg) ||
               !read(p+"wrist_max_error_deg",a.wrist.max_error_deg)) return false;
            a.wrist.measured=measured!=0;
        }
    }
    if (!fields.empty() || !Validate(parsed) || (!parsed.arms_measured && parsed.standing_eye_cm==0 &&
        parsed.seated_eye_cm==0 && parsed.legacy_eye_cm==0)) return false;
    *out=parsed; return true;
}
bool MergeProfile(const Profile* previous,const Profile& result,Profile* out) {
    if(!out || !Validate(result) || (previous && !Validate(*previous))) return false;
    Profile merged=result;
    if(previous) {
        if(!result.arms_measured) {
            merged.arms_measured=previous->arms_measured;
            merged.arm[0]=previous->arm[0]; merged.arm[1]=previous->arm[1];
        }
        if(result.standing_eye_cm==0) merged.standing_eye_cm=previous->standing_eye_cm;
        if(result.seated_eye_cm==0) merged.seated_eye_cm=previous->seated_eye_cm;
        if(result.legacy_eye_cm==0) merged.legacy_eye_cm=previous->legacy_eye_cm;
    }
    if(!Validate(merged)) return false;
    *out=merged; return true;
}
bool PlausibleSplit(double upper_cm, double forearm_cm) {
    const double total = upper_cm + forearm_cm;
    if (!std::isfinite(total) || upper_cm <= 0 || forearm_cm <= 0) return false;
    const double share = forearm_cm / total;
    return share >= kMinForearmShare && share <= kMaxForearmShare;
}
bool UsableSegments(const Arm& arm, double* upper_cm, double* forearm_cm) {
    if (PlausibleSplit(arm.upper_cm, arm.forearm_cm)) {
        *upper_cm = arm.upper_cm; *forearm_cm = arm.forearm_cm;
        return true;
    }
    const double total = arm.upper_cm + arm.forearm_cm;
    *forearm_cm = total * kDefaultForearmShare; *upper_cm = total - *forearm_cm;
    return false;
}
bool MapArmTarget(const Profile& profile, int side, const FVector& head_world,
                  const FQuat& body_yaw, double tracking_scale,
                  FVector* shoulder_world, double* upper_world, double* forearm_world) {
    FQuat yaw;
    if (!shoulder_world || !upper_world || !forearm_world || side < 0 || side > 1 ||
        !Validate(profile) || !profile.arms_measured || !Finite(head_world) || !Unit(body_yaw, &yaw) ||
        !std::isfinite(tracking_scale) || tracking_scale <= 0 || tracking_scale > 1000) return false;
    const auto& arm = profile.arm[side];
    const FVector shoulder = head_world + yaw.Rotate(arm.shoulder * tracking_scale);
    if (!Finite(shoulder)) return false;
    double upper = 0, forearm = 0;
    UsableSegments(arm, &upper, &forearm);
    *shoulder_world = shoulder;
    *upper_world = upper * tracking_scale;
    *forearm_world = forearm * tracking_scale;
    return true;
}

bool MapWristTarget(const FVector& raw_controller_cm, const FVector& wrist_game_delta_cm,
                    double tracking_scale, FVector* wrist_real_cm) {
    if (!wrist_real_cm || !Finite(raw_controller_cm) || !Finite(wrist_game_delta_cm) ||
        !std::isfinite(tracking_scale) || tracking_scale <= 0 || tracking_scale > 1000) return false;
    const FVector wrist = raw_controller_cm + wrist_game_delta_cm * (1.0 / tracking_scale);
    if (!Finite(wrist)) return false;
    *wrist_real_cm = wrist;
    return true;
}
}  // namespace holevr::vr::body_calibration
