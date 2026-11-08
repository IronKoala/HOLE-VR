#include <cmath>
#include <cstdio>
#include <limits>
#include "vr/interact/holster_visual.h"
#include "vr/interact/holster_feedback.h"
#include "vr/interact/holster_model.h"
#include "vr/interact/holster_mesh_data.h"
#include "vr/hand_ownership.h"
using namespace holevr::vr;
using namespace holevr::vr::interact;
int failures = 0, checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
bool Near(double a, double b) { return std::fabs(a-b) < 1e-7; }
bool Near(FVector a, FVector b) { return Near(a.X,b.X) && Near(a.Y,b.Y) && Near(a.Z,b.Z); }
double Dot(FVector a, FVector b) { return a.X*b.X+a.Y*b.Y+a.Z*b.Z; }
FVector Cross(FVector a, FVector b) { return {a.Y*b.Z-a.Z*b.Y,a.Z*b.X-a.X*b.Z,a.X*b.Y-a.Y*b.X}; }

void StateRules() {
    HolsterVisual v;
    HolsterVisualInput i;
    i.enabled=i.seated=i.tracked=i.actions=i.hand_free=true;
    i.distance=30; i.dt=1.0/72;
    for(int f=0;f<72;++f) v.Update(i);
    CHECK(v.HoodOpen()==0 && v.HandBlend()==0);
    i.distance=11.9;
    for(int f=0;f<30;++f) v.Update(i);
    CHECK(v.Near() && v.HoodOpen()==1 && v.HandBlend()==1);
    i.distance=14.99; v.Update(i); CHECK(v.Near());
    i.distance=15.01;
    for(int f=0;f<30;++f) v.Update(i);
    CHECK(!v.Near() && v.HandBlend()==0 && v.HoodOpen()==0);
    // The gun leaves on the same frame it is drawn, even if the hood was up.
    i.seated=false; v.Update(i); CHECK(v.HoodOpen()==1 && v.HandBlend()==0);
    for(int f=0;f<1000;++f) { i.distance=f%2?0:100;v.Update(i);CHECK(v.HoodOpen()==1 && v.HandBlend()==0); }
    // A menu/editor, occupied hand or tracking loss must never trap the hand.
    i.seated=true;i.distance=0;
    for(int f=0;f<20;++f) v.Update(i);
    CHECK(v.HandBlend()==1);
    i.actions=false;v.Update(i);CHECK(v.HandBlend()==0);
    i.actions=true;i.hand_free=false;v.Update(i);CHECK(v.HandBlend()==0);
    i.hand_free=true;i.tracked=false;v.Update(i);CHECK(v.HandBlend()==0 && !v.Near());
    i.tracked=true;i.distance=std::numeric_limits<double>::quiet_NaN();v.Update(i);CHECK(!v.Near());
    i.distance=0;i.dt=-1;v.Update(i);CHECK(v.HandBlend()==0);
    i.dt=1e9;v.Update(i);CHECK(std::isfinite(v.HoodOpen()) && v.HandBlend()<=1);
    i.enabled=false;v.Update(i);CHECK(v.HandBlend()==0 && v.HoodOpen()==1);
}
void ExistingDraw() {
    for(auto mode:{DrawMode::kHold,DrawMode::kToggle}) {
        HolsterDraw draw;draw.Configure(12,mode);
        DrawInput i;i.dt=1.0/72;
        CHECK(PistolSeated(true,false,draw.State(),draw.Blend()));
        i.grip_down=i.grip_pressed=true;draw.Update(i);
        CHECK(draw.InHand());
        CHECK(!PistolSeated(true,false,draw.State(),draw.Blend()));
        i.grip_pressed=mode==DrawMode::kToggle;i.grip_down=false;draw.Update(i);
        CHECK(!draw.InHand());
        CHECK(!PistolSeated(true,false,draw.State(),draw.Blend())); // short settle not occupied yet
        i.grip_pressed=false;
        for(int f=0;f<30;++f)draw.Update(i);
        CHECK(PistolSeated(true,false,draw.State(),draw.Blend()));
        CHECK(!PistolSeated(true,true,draw.State(),draw.Blend())); // workbench
        CHECK(!PistolSeated(false,false,draw.State(),draw.Blend()));
    }
    HolsterDraw draw;draw.Configure(12,DrawMode::kHold);
    DrawInput i;i.dt=1.0/72;i.grip_down=i.grip_pressed=true;draw.Update(i);
    i.grip_down=i.grip_pressed=false;i.hand={100,0,0};draw.Update(i);
    CHECK(draw.State()==HolsterState::kReturning && !PistolSeated(true,false,draw.State(),draw.Blend()));
    for(int f=0;f<30;++f)draw.Update(i);
    CHECK(PistolSeated(true,false,draw.State(),draw.Blend()));
    hands::GripFacts f;f.attachment_held=true;
    CHECK(!hands::ResolveGrip(f).AllowsControls());
    f={};f.drink_held=true;CHECK(!hands::ResolveGrip(f).AllowsControls());
    f={};f.actions_allowed=false;CHECK(!hands::ResolveGrip(f).AllowsControls());
}
void OccupiedHood() {
    HolsterVisual v;
    HolsterVisualInput i;
    i.enabled=i.seated=i.tracked=i.actions=i.hand_free=true;
    i.dt=1.0/72; i.distance=100;
    for(int n=0;n<30;++n)v.Update(i);
    CHECK(v.HoodOpen()==0);
    i.hood_allowed=false;i.hand_free=false;
    for(int n=0;n<240;++n) {
        i.distance=n%2?0:100;v.Update(i);
        CHECK(v.HoodOpen()==0 && v.HandBlend()==0 && !v.Near());
    }
    i.hood_allowed=i.hand_free=true;i.distance=0;
    for(int n=0;n<30;++n)v.Update(i);
    CHECK(v.HoodOpen()==1);
    i.hood_allowed=false;i.hand_free=false;i.distance=100;
    for(int n=0;n<240;++n) {v.Update(i);CHECK(v.HoodOpen()==1 && v.HandBlend()==0);}
    i.hood_allowed=i.hand_free=true;v.Update(i);
    const double mid=v.HoodOpen(); CHECK(mid>0 && mid<1);
    i.hood_allowed=false;i.hand_free=false;
    for(int n=0;n<72;++n) {v.Update(i);CHECK(v.HoodOpen()==mid);}
    i.seated=false;v.Update(i);CHECK(v.HoodOpen()==1); // empty still wins
}
void CalibratedFit() {
    HolsterSpot sp;
    CHECK(ParseSpotLine("pistol=belt 5.6 34.9 15.9 rot -0.07713 -0.73441 -0.03793 -0.67324",HolsterId::kPistol,&sp));
    CHECK(sp.anchor==HolsterAnchor::kBelt && Near(sp.offset,{5.6,34.9,15.9}));
    // Native actor's gun points +Y; its mesh can have a local animated offset.
    const FQuat gun_frame=YawOnly(90), mesh_rel_rot=ToQuat({12,90,0});
    const FVector grip_rel{4.76,-1.521,-4.464}, mesh_rel{2,3,4};
    for(int turn=0;turn<360;turn+=15) {
        const FQuat body=YawOnly(turn), frame=body*sp.rot;
        const FVector spot=SpotWorld(sp,{100,200,70},body), grip=spot+frame.Rotate({-5,1.3,-5});
        const FQuat seat_rot=frame*gun_frame.Inverse();
        const FVector seat_loc=grip-seat_rot.Rotate(grip_rel);
        for(int held=0;held<360;held+=30) {
            const FVector a{300.0+held,-100,150};const FQuat r=ToQuat({23.0,static_cast<double>(held),-14});
            holster_model::Transform mesh;
            mesh.translation=a+r.Rotate(mesh_rel);mesh.rotation=r*mesh_rel_rot;
            const auto out=holster_model::MeshAtSeat(grip,frame,gun_frame,grip_rel,a,r,mesh);
            CHECK(Near(out.translation,seat_loc+seat_rot.Rotate(mesh_rel)));
            CHECK(Near(out.rotation.Rotate({1,0,0}),(seat_rot*mesh_rel_rot).Rotate({1,0,0})));
            CHECK(Near(seat_loc+seat_rot.Rotate(grip_rel),grip));
        }
    }
}
void Mesh() {
    namespace m=holster_model::mesh;
    int triangles=0;
    for(const auto& section:m::kSections) {
        CHECK(section.count%3==0 && section.count>0);
        for(int i=0;i<section.count;i+=3) {
            const auto& a=section.vertices[i];const auto& b=section.vertices[i+1];const auto& c=section.vertices[i+2];
            CHECK(Dot(Cross(b.position-a.position,c.position-a.position),a.normal)<-1e-10); // UE clockwise outside
            CHECK(std::fabs(Dot(a.normal,a.normal)-1)<1e-6);
            ++triangles;
        }
    }
    CHECK(triangles==1284);
    CHECK(std::fabs(m::kSections[0].color.r-.038204)<1e-7 && std::fabs(m::kSections[1].color.g-.164706)<1e-7);
    CHECK(Near(m::kHoodPivot,{-4.750999450683594,-5.468295682931057e-08,3.5}));
    // Sight-side top of closed hood swings forward/down, away from the draw.
    const double h=100*kDegToRad/2;const FQuat open{0,std::sin(h),0,std::cos(h)};
    const FVector moved=open.Rotate({-4.5,0,0});
    CHECK(moved.X>0 && moved.Z>4);
}
unsigned received = 0;
int calls = 0;
void CueSink(HolsterCue cue, uintptr_t player, const FVector& where) {
    CHECK(player == 321 && Near(where, {20, 30, 40}));
    received |= unsigned(cue); ++calls;
}
void SoundHooks() {
    using O = PistolOccupancy;
    const unsigned open = unsigned(HolsterCue::kHoodOpen), close = unsigned(HolsterCue::kHoodClose);
    const unsigned draw = unsigned(HolsterCue::kDraw), stow = unsigned(HolsterCue::kHolster);
    HolsterFeedback f;
    HolsterFeedbackInput i{true, 123, O::kSeated, false, 0};
    CHECK(f.Update(i) == 0); // loading a seated pistol is quiet
    i.hood_wants_open = true; i.hood_open = 0.1;
    CHECK(f.Update(i) == open);
    for (int n=0;n<50;++n) { i.hood_open=std::min(1.0,i.hood_open+.05); CHECK(f.Update(i)==0); }
    i.occupancy=O::kInHand; CHECK(f.Update(i)==draw);
    for (int n=0;n<50;++n) CHECK(f.Update(i)==0); // empty remains silent and open
    i.occupancy=O::kReturning; CHECK(f.Update(i)==0);
    i.occupancy=O::kSettling; CHECK(f.Update(i)==0);
    i.occupancy=O::kSeated; CHECK(f.Update(i)==stow);
    i.hood_wants_open=false; i.hood_open=.9; CHECK(f.Update(i)==close);
    for (int n=0;n<50;++n) { i.hood_open=std::max(0.0,i.hood_open-.05); CHECK(f.Update(i)==0); }
    // A direct draw may force open in the same frame: two distinct cues, once.
    i.occupancy=O::kInHand; i.hood_wants_open=true;i.hood_open=1;
    CHECK(f.Update(i)==(draw|open));
    i.occupancy=O::kParked; CHECK(f.Update(i)==0);
    i.occupancy=O::kInHand; CHECK(f.Update(i)==0); // table pickup is not a holster draw
    i.occupancy=O::kSeated; i.hood_wants_open=false; i.hood_open=.9;
    CHECK(f.Update(i)==(stow|close)); // carried pistol becoming spare seats once
    i.hood_wants_open=true; i.hood_open=.95; CHECK(f.Update(i)==open); // reversal
    i.pistol=124; CHECK(f.Update(i)==0); // another weapon, no invented action
    i.enabled=false; CHECK(f.Update(i)==0);
    i.enabled=true;i.occupancy=O::kInHand; CHECK(f.Update(i)==0);
    f.Reset();i.occupancy=O::kSeated;i.hood_wants_open=false;CHECK(f.Update(i)==0);
    i.hood_open=.5;CHECK(f.Update(i)==0); // initial closing animation stays quiet
    i.hood_open=std::numeric_limits<double>::quiet_NaN();CHECK(f.Update(i)==0);
    SetHolsterCueSink(CueSink);
    DispatchHolsterCues(open|draw,321,{20,30,40}); CHECK(calls==2 && received==(open|draw));
    DispatchHolsterCues(close,0,{20,30,40}); CHECK(calls==2);
    SetHolsterCueSink(nullptr);DispatchHolsterCues(stow,321,{20,30,40});CHECK(calls==2);
}
int main(){StateRules();ExistingDraw();OccupiedHood();CalibratedFit();Mesh();SoundHooks();std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
