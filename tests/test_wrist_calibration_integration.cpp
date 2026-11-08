// Independent fit -> persisted profile -> runtime hand/wrist mapping regression.
#include "vr/body/body_calibration.h"
#include "vr/body/ik_math.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

using namespace holevr::vr;
using namespace holevr::vr::ik;
namespace bc=holevr::vr::body_calibration;
namespace wc=holevr::vr::wrist_calibration;
namespace {
int checks=0;
void Check(bool ok,const char* why) { ++checks; if(!ok) { std::fprintf(stderr,"FAIL: %s\n",why); std::exit(1); } }
bool Near(double a,double b,double eps=1e-6) { return std::fabs(a-b)<eps; }
bool Near(FVector a,FVector b,double eps=1e-6) { return Length(a-b)<eps; }
bool Same(FQuat a,FQuat b) {
    return Near(a.Rotate({1,0,0}),b.Rotate({1,0,0})) && Near(a.Rotate({0,1,0}),b.Rotate({0,1,0}));
}
struct Fixture {
    int side;
    double sign;
    FVector axis, palm, offset, shoulder;
    wc::Calibration wrist;
    explicit Fixture(int which):side(which),sign(which?1.:-1.) {
        axis=Normalize({1,sign*.20,-.10});
        palm=Normalize(Reject({0,sign,.15},axis));
        offset={6,sign*3,2}; shoulder={-5,sign*20,-25};
        std::vector<wc::Observation> observations;
        const FQuat yaw=YawOnly(73);
        for(int stage=0;stage<2;++stage) for(int sample=0;sample<(stage?5:37);++sample) {
            wc::Observation o;
            o.stage=stage?wc::Stage::Out:wc::Stage::Forward;
            o.forearm_direction=yaw.Rotate(stage?FVector{0,sign,0}:FVector{1,0,0});
            o.palm_direction=yaw.Rotate(stage?FVector{0,0,-1}:FVector{0,-sign,0});
            o.palm_known=true;
            o.raw_grip_rotation=AlignFrames(axis,palm,o.forearm_direction,o.palm_direction);
            if(sample%2) { auto& q=o.raw_grip_rotation; q={-q.X,-q.Y,-q.Z,-q.W}; }
            observations.push_back(o);
        }
        // Overhead rolling has no prescribed palm direction and must not
        // overwrite the neutral basis or fabricate a new wrist offset.
        for(int degrees=-35;degrees<=35;degrees+=7) {
            wc::Observation o; o.stage=wc::Stage::Overhead;
            o.forearm_direction={0,0,1};
            o.raw_grip_rotation=YawOnly(degrees)*AlignFrames(axis,palm,{0,0,1},{1,0,0});
            observations.push_back(o);
        }
        const auto fit=wc::Fit(observations,offset);
        Check(fit.reason==wc::Reason::None && fit.calibration.measured,"neutral holds produce wrist basis");
        wrist=fit.calibration;
        Check(Near(wrist.forearm_axis_grip,axis) && Near(wrist.palm_axis_grip,palm),"mirrored grip axes recovered despite quaternion signs and unequal density");
        Check(Near(wrist.wrist_offset_cm,offset),"overhead roll preserves frozen physical offset");
    }
    std::vector<bc::BendObservation> Bends(double first=40,double last=90,int count=61) const {
        std::vector<bc::BendObservation> result;
        for(int i=0;i<count;++i) {
            const double a=(first+(last-first)*i/(count-1))*kDegToRad;
            const FVector upper={0,sign*std::cos(15*kDegToRad),-std::sin(15*kDegToRad)};
            const FVector forearm={0,sign*std::cos(a),std::sin(a)};
            const FVector inward=Normalize(Reject({0,-sign,0},forearm),{1,0,0});
            result.push_back({shoulder+upper*31+forearm*28,AlignFrames(axis,palm,forearm,inward)});
        }
        return result;
    }
};

bc::Profile MeasuredProfile() {
    bc::Profile profile;
    profile.standing_eye_cm=173; profile.seated_eye_cm=116;
    for(int side=0;side<2;++side) {
        Fixture f(side);
        bc::SegmentFit fit;
        Check(bc::FitSegments(f.shoulder,59,f.wrist,f.Bends(),25,&fit)==bc::Reason::None,"varied bends solve despite dropped upper arm and tilted forearm");
        Check(Near(fit.upper_cm,31,.01) && Near(fit.forearm_cm,28,.01),"correct elbow split replaces old misleading endpoint fit");
        auto& arm=profile.arm[side];
        arm.shoulder=f.shoulder; arm.upper_cm=fit.upper_cm; arm.forearm_cm=fit.forearm_cm;
        arm.fit.radial_rms_cm=.1; arm.fit.arc_degrees=90; arm.fit.inliers=80; arm.fit.samples=80;
        arm.fit.bent_error_cm=fit.rms_cm; arm.fit.bend_span_degrees=fit.span_degrees;
        arm.fit.bend_length_spread_cm=fit.length_spread_cm;
        arm.fit.bend_samples=fit.samples; arm.fit.bend_inliers=fit.inliers;
        arm.wrist=f.wrist;
        arm.hand_position_setting_game_cm={1,f.sign*2,3};
        arm.hand_rotation_setting=ToQuat({12,f.sign*17,-9});
    }
    Check(bc::Validate(profile),"completed measured profile validates");
    return profile;
}

void CounterexampleAndAmbiguity() {
    // The old endpoint sum check accepted these wrong lengths with 0.13 cm
    // total error. A 15-degree upper drop plus 25-degree forearm tilt can fool it.
    const double h=31*std::cos(15*kDegToRad)+28*std::sin(25*kDegToRad);
    const double z=-31*std::sin(15*kDegToRad)+28*std::cos(25*kDegToRad);
    Check(std::fabs(h+z-59)<.14 && std::fabs(h*59/(h+z)-31)>10,"fixture reproduces old accepted 10cm elbow error");
    for(int side=0;side<2;++side) {
        Fixture f(side); bc::SegmentFit unchanged; unchanged.upper_cm=999;
        const auto single=f.Bends(65,65,61);
        Check(bc::FitSegments(f.shoulder,59,f.wrist,single,25,&unchanged)!=bc::Reason::None && unchanged.upper_cm==999,
              "repeated ambiguous endpoint cannot become a new arm fit");
        Check(bc::FitSegments(f.shoulder,59,f.wrist,f.Bends(65,70),25,&unchanged)!=bc::Reason::None && unchanged.upper_cm==999,
              "limited bend span preserves output instead of shortening the arm");
        auto bad=f.wrist;
        bad.forearm_axis_grip=AxisAngle(bad.palm_axis_grip,35*kDegToRad).Rotate(bad.forearm_axis_grip);
        Check(bc::FitSegments(f.shoulder,59,bad,f.Bends(),25,&unchanged)!=bc::Reason::None && unchanged.upper_cm==999,
              "inconsistent longitudinal wrist direction cannot silently set elbow lengths");
    }
}

void PersistenceToRuntime() {
    const auto profile=MeasuredProfile(); const std::string saved=bc::Serialize(profile);
    bc::Profile loaded; Check(bc::Parse(saved,&loaded),"measured wrist profile reloads");
    Check(bc::Serialize(loaded)==saved,"new wrist and segment data roundtrip without drift");
    for(int side=0;side<2;++side) {
        const auto& arm=loaded.arm[side]; Fixture f(side);
        Check(arm.wrist.measured && Near(arm.wrist.wrist_offset_cm,f.offset),"loaded frozen wrist offset matches capture");
        Check(Near(arm.hand_position_setting_game_cm,profile.arm[side].hand_position_setting_game_cm) &&
              Same(arm.hand_rotation_setting,profile.arm[side].hand_rotation_setting),"captured manual settings survive persistence");
        for(double scale:{.8,1.,1.25}) for(double yaw:{-147.,0.,71.}) {
            const FQuat turn=YawOnly(yaw), grip=Normalized(turn*ToQuat({31,f.sign*57,-26}));
            const FVector physical_controller{34,f.sign*18,42}, origin{230,-85,19};
            wc::Mapping input;
            input.controller_world=origin+turn.Rotate(physical_controller*scale);
            input.grip_world=grip; input.tracking_scale=scale;
            // Hand bone axes deliberately differ from sensor axes and by side.
            const FQuat hand_reference=ToQuat({17,f.sign*43,29});
            input.forearm_axis_hand=hand_reference.Inverse().Rotate({1,0,0});
            input.palm_axis_hand=hand_reference.Inverse().Rotate({0,f.sign,0});
            const FVector same_setting=arm.hand_position_setting_game_cm;
            input.position_delta_cm=(same_setting-arm.hand_position_setting_game_cm)*(1.0/scale);
            FVector wrist; FQuat hand;
            Check(wc::Map(arm.wrist,input,&wrist,&hand),"loaded profile maps raw runtime pose");
            Check(Near(wrist,input.controller_world+grip.Rotate(f.offset*scale)),"frozen physical offset scales exactly once");
            Check(Near(input.position_delta_cm,FVector{}),"unchanged nonzero manual setting cannot invent an offset when scale changes");
            Check(Near(hand.Rotate(input.forearm_axis_hand),grip.Rotate(f.axis)) &&
                  Near(hand.Rotate(input.palm_axis_hand),grip.Rotate(f.palm)),"runtime hand basis matches capture under world yaw and mirrored bone axes");
            const FVector before=wrist;
            input.rotation_delta_grip=ToQuat({-11,23,19});
            Check(wc::Map(arm.wrist,input,&wrist,&hand) && Near(wrist,before),"manual orientation adjustment does not orbit calibrated wrist");
            Check(Near(hand.Rotate(input.forearm_axis_hand),(grip*input.rotation_delta_grip).Rotate(f.axis)),"manual orientation delta applies exactly once");
            const FVector change_game_cm={2,-1,.5};
            const FVector changed_setting=same_setting+change_game_cm;
            input.position_delta_cm=(changed_setting-arm.hand_position_setting_game_cm)*(1.0/scale);
            Check(wc::Map(arm.wrist,input,&wrist,&hand) && Near(wrist,before+grip.Rotate(change_game_cm)),
                  "intentional game-cm manual adjustment applies once at every physical scale");
            // For an unrecorded grab, controller_world instead denotes the
            // desired fist/contact point. Keep the calibrated orientation,
            // but seat the actual avatar fist on that anchor as SolveArm does.
            const FVector fist_local=hand_reference.Inverse().Rotate({5,f.sign*2,1});
            const FVector contact=input.controller_world+grip.Rotate(changed_setting);
            const FVector anchored_wrist=contact-hand.Rotate(fist_local);
            Check(Near(anchored_wrist+hand.Rotate(fist_local),contact),
                  "changed calibrated basis preserves explicit grab contact at every scale");
            const FQuat other_hand=Normalized(YawOnly(90)*hand);
            const FVector other_wrist=contact-other_hand.Rotate(fist_local);
            Check(Near(other_wrist+other_hand.Rotate(fist_local),contact),
                  "a ninety-degree grip orientation correction cannot displace the contact anchor");
            FVector shoulder; double upper=0,lower=0;
            Check(bc::MapArmTarget(loaded,side,origin,turn,scale,&shoulder,&upper,&lower) &&
                  Near(upper,31*scale,.01) && Near(lower,28*scale,.01),"same persisted physical scale drives arm lengths");
            // Recapture obtains the current physical offset from identity raw
            // grip mapping. Its orientation correction must not feed back into
            // that offset or accumulate on repeated calibration.
            wc::Mapping recapture=input; recapture.controller_world={}; recapture.grip_world={};
            recapture.position_delta_cm={};
            Check(wc::Map(arm.wrist,recapture,&wrist,&hand) && Near(wrist*(1.0/scale),f.offset),
                  "identity recapture retains frozen physical offset despite orientation delta");
        }
    }
}

void KeepPreviousAndLegacy() {
    const std::string legacy=
        "version=3\nunits=cm\nmethod=two_arcs_bent_t\narms_measured=1\nstanding_eye_cm=0\nseated_eye_cm=116\nlegacy_eye_cm=0\n"
        "left_shoulder=-5 -20 -25\nleft_upper=31\nleft_forearm=28\nleft_radial_rms=.4\nleft_plane_rms=0\nleft_arc_degrees=90\nleft_bent_error=.5\nleft_inliers=90\nleft_samples=95\n"
        "right_shoulder=-4 21 -24\nright_upper=35\nright_forearm=30\nright_radial_rms=.3\nright_plane_rms=0\nright_arc_degrees=90\nright_bent_error=.7\nright_inliers=92\nright_samples=96\n";
    bc::Profile old; Check(bc::Parse(legacy,&old),"legacy v3 arm fit loads");
    Check(!old.arm[0].wrist.measured && !old.arm[1].wrist.measured && old.arm[0].upper_cm==31 && old.arm[1].forearm_cm==30,
          "legacy profile gets no invented wrist basis or changed arm values");
    FVector wrist{7,8,9}; FQuat hand=ToQuat({12,34,56}); const FQuat before_hand=hand;
    wc::Mapping input;
    Check(!wc::Map(old.arm[0].wrist,input,&wrist,&hand) && Near(wrist,{7,8,9}) && Same(hand,before_hand),
          "legacy profiles explicitly retain caller's prior mapping path");
    const auto prior=MeasuredProfile(); const std::string saved=bc::Serialize(prior);
    bc::Profile merged=prior, invalid=prior; invalid.arm[0].forearm_cm=999;
    Check(!bc::MergeProfile(&prior,invalid,&merged) && bc::Serialize(merged)==saved,"failed measurement cannot overwrite previous arm or wrist calibration");
    bc::Settings settings; settings.preparation_seconds=0;
    bc::Session session(settings);
    bc::Sample sample; sample.head={0,0,116}; sample.head_valid=true; sample.floor_known=true;
    sample.height_mode=bc::HeightMode::Seated; sample.height_posture=bc::HeightPosture::Seated;
    sample.skip_arms=true; // height-only must work with both controllers untracked
    for(int i=0;i<100 && !session.GetProfile();++i) { sample.seconds=i/30.; session.Update(sample); sample.skip_arms=false; }
    const auto* height_only=session.GetProfile();
    Check(height_only && !height_only->arms_measured,"accessibility skip completes without tracked controllers");
    Check(bc::MergeProfile(&prior,*height_only,&merged),"height-only result merges with previous arms");
    Check(bc::Serialize(merged)==saved,"height-only skip preserves both previous arm and wrist fits exactly");
}
} // namespace
int main() {
    CounterexampleAndAmbiguity(); PersistenceToRuntime(); KeepPreviousAndLegacy();
    std::printf("wrist calibration integration: %d checks passed\n",checks);
}
