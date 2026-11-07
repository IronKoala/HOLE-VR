#include "vr/body/torso_pose.h"
#include "vr/body/skeleton.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>

using namespace holevr::vr::body::torso;
using namespace holevr::vr;
namespace {
int checks=0;
void Check(bool value,const char* text) {
    ++checks;
    if (!value) { std::fprintf(stderr,"FAIL: %s\n",text); std::exit(1); }
}
bool Near(double a,double b) { return std::fabs(a-b)<1e-8; }
Input Standing() {
    Input in;
    in.source_pelvis_z=95; in.source_top_spine_z=145; in.source_shoulders_z=165;
    in.target_shoulders_z=145; in.head_z=in.game_camera_z=165; in.camera_known=true;
    return in;
}
void FixedAndSeated() {
    for (const auto mode:{Mode::Fixed,Mode::Seated,static_cast<Mode>(99)}) {
        auto in=Standing(); in.mode=mode; in.head_z=116.48;
        const auto out=Fit(in);
        Check(out.valid && Near(out.pelvis_offset,0),"fixed, seated and unknown mode preserve standing pelvis");
        Check(Near(out.spine_scale,.6) && Near(out.fitted_shoulders_z,145),"shoulder mismatch fits upper torso without a squat");
        in.source_pelvis_z-=40; in.source_top_spine_z-=40; in.source_shoulders_z-=40;
        in.target_shoulders_z-=40; in.game_camera_z-=40;
        const auto crouch=Fit(in);
        Check(crouch.valid && Near(crouch.pelvis_offset,0),"game crouch remains owned by copied animation");
        Check(Near(crouch.MapSpineZ(in.source_pelvis_z),55),"game-crouched pelvis is neither raised nor lowered");
    }
}
void RealCrouch() {
    auto in=Standing(); in.mode=Mode::Real; in.head_z-=25; in.target_shoulders_z-=25;
    auto out=Fit(in);
    Check(out.valid && Near(out.pelvis_offset,-25),"real head drop adds crouch while game remains standing");
    Check(Near(out.spine_scale,.6),"physical crouch does not change neutral torso proportions");
    in=Standing(); in.mode=Mode::Real;
    in.source_pelvis_z-=40; in.source_top_spine_z-=40; in.source_shoulders_z-=40;
    in.target_shoulders_z-=40; in.game_camera_z-=40; in.head_z-=40;
    out=Fit(in);
    Check(Near(out.pelvis_offset,0),"camera compensation plus game crouch is not applied twice");
    in.head_z+=40;
    Check(Near(Fit(in).pelvis_offset,0),"standing physical head cannot undo stick-controlled game crouch");
    in.camera_known=false; in.head_z-=80;
    Check(Near(Fit(in).pelvis_offset,0),"unknown camera cannot authorize extra physical crouch");
}
void BoundsAndNoHistory() {
    auto in=Standing(); in.mode=Mode::Real; in.head_z=-1000;
    Check(Near(Fit(in).pelvis_offset,-70),"physical crouch is bounded");
    in.mode=Mode::Seated;
    Check(Near(Fit(in).pelvis_offset,0),"mode switch immediately clears previous real crouch");
    in=Standing(); const auto first=Fit(in);
    Check(Near(Fit(in).MapSpineZ(120),first.MapSpineZ(120)),"repeated copied poses cannot accumulate torso compression");
    in.source_pelvis_z+=10000; in.source_top_spine_z+=10000; in.source_shoulders_z+=10000;
    in.target_shoulders_z+=10000; in.head_z+=10000; in.game_camera_z+=10000;
    Check(Near(Fit(in).MapSpineZ(10120)-10000,first.MapSpineZ(120)),"world origin reset preserves relative geometry");
    for (const double target:{-1000.0,1000.0}) {
        in=Standing(); in.target_shoulders_z=target;
        const auto out=Fit(in);
        Check(out.valid && out.spine_scale>=.5 && out.spine_scale<=1.5,"extreme shoulder target has bounded torso scale");
        double previous=out.MapSpineZ(95);
        for(const double z:{105.0,120.0,135.0,145.0}) {
            const double mapped=out.MapSpineZ(z);
            Check(mapped>previous,"bounded affine spine fit preserves joint order"); previous=mapped;
        }
        Check(std::fabs(out.shoulder_residual)>1,"unreachable torso target remains an explicit residual");
    }
    const double nan=std::numeric_limits<double>::quiet_NaN();
    in=Standing(); in.source_top_spine_z=nan;
    Check(!Fit(in).valid,"nonfinite source geometry is rejected");
    in=Standing(); in.source_top_spine_z=in.source_pelvis_z;
    Check(!Fit(in).valid,"degenerate spine cannot be inverted to fit the headset");
    in=Standing(); in.mode=Mode::Real; in.head_z=nan;
    Check(Fit(in).valid && Near(Fit(in).pelvis_offset,0),"invalid physical height preserves game stance");
    in=Standing(); in.target_shoulders_z=nan;
    Check(!Fit(in).valid,"invalid shoulder target cannot deform the torso");
}
void SkeletonIntegration() {
    // Real hierarchy: pelvis branches into legs and spine; clavicles and
    // arms descend from the top spine. Exercise the body's absolute-write
    // ordering rather than merely checking the helper's scalar formula.
    const std::vector<int> parent={-1,0,1,2,3,4,5,6,3,8,0,10,11};
    for(const double game_drop:{0.0,40.0}) {
        skel::Xf world[13];
        world[0].translation={0,0,95-game_drop};
        world[1].translation={0,0,105-game_drop};
        world[2].translation={0,0,125-game_drop};
        world[3].translation={0,0,145-game_drop};
        world[4].translation={0,-5,155-game_drop};
        world[5].translation={0,-20,165-game_drop};
        world[6].translation={31,-20,165-game_drop};
        world[7].translation={59,-20,165-game_drop};
        world[8].translation={0,5,155-game_drop};
        world[9].translation={0,20,165-game_drop};
        world[10].translation={0,-10,95-game_drop};
        world[11].translation={15,-10,52-game_drop*.5};
        world[12].translation={0,-10,8};
        skel::Xf local[13];
        for(int i=0;i<13;++i) local[i]=parent[i]<0?world[i]:skel::Relative(world[i],world[parent[i]]);
        skel::Pose pose;
        Check(pose.Init(parent),"torso integration hierarchy is valid");
        pose.SetAllLocal(local,13);
        auto input=Standing(); input.mode=Mode::Seated;
        input.source_pelvis_z-=game_drop; input.source_top_spine_z-=game_drop;
        input.source_shoulders_z-=game_drop; input.target_shoulders_z-=game_drop;
        const auto fit=Fit(input);
        const double source_z[]={pose.C(1).translation.Z,pose.C(2).translation.Z,pose.C(3).translation.Z};
        auto pelvis=pose.C(0); pelvis.translation.Z+=fit.pelvis_offset; pose.SetC(0,pelvis);
        for(int i=0;i<3;++i) {
            auto spine=pose.C(i+1); spine.translation.Z=fit.MapSpineZ(source_z[i]); pose.SetC(i+1,spine);
        }
        Check(Near(pose.C(0).translation.Z,95-game_drop) &&
              Near(pose.C(10).translation.Z,world[10].translation.Z) &&
              Near(pose.C(11).translation.Z,world[11].translation.Z) &&
              Near(pose.C(12).translation.Z,8),"upper-torso fit preserves standing or crouched game pelvis, knee and foot");
        Check(Near(pose.C(5).translation.Z,input.target_shoulders_z) &&
              Near(pose.C(9).translation.Z,input.target_shoulders_z),"both shoulder roots follow fitted top spine");
        Check(Near(pose.C(5).translation.Z-pose.C(4).translation.Z,10) &&
              Near(pose.C(9).translation.Z-pose.C(8).translation.Z,10),"clavicles follow shoulder displacement without a new vertical gap");
        Check(Near(ik::Length(pose.C(6).translation-pose.C(5).translation),31) &&
              Near(ik::Length(pose.C(7).translation-pose.C(6).translation),28),"torso fit does not rescale either physical arm segment");
    }
}
}
int main() {
    FixedAndSeated(); RealCrouch(); BoundsAndNoHistory(); SkeletonIntegration();
    std::printf("torso_pose: %d checks passed\n",checks);
}
