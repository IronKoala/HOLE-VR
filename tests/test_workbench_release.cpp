#include <cstdio>
#include <cmath>
#include <limits>
#include "vr/workbench_pose.h"
#include "vr/workbench_carry.h"
#include "vr/workbench_rules.h"
#include "vr/bench_vise_fit.h"
using namespace holevr;
using namespace vr;
static int errors=0;
#define CHECK(x) do{if(!(x)){std::printf("FAIL %d: %s\n",__LINE__,#x);++errors;}}while(0)
double Dist(FVector a,FVector b){auto d=a-b;return std::sqrt(d.X*d.X+d.Y*d.Y+d.Z*d.Z);}
int main(){
    using workbench::ReleaseHand;
    for(const auto& gun:bench_vise::fit::kProfiles){
        for(double yaw:{-175.,-90.,0.,86.,179.}){
            const FVector out=YawOnly(yaw).Rotate({1,0,0});
            const FVector viewer_left=YawOnly(yaw+180).Rotate({0,-1,0});
            for(auto hand:{ReleaseHand::kLeft,ReleaseHand::kRight}){
                FQuat parked;CHECK(workbench::BenchFrame(out,hand,&parked));
                const FVector expected=viewer_left*(hand==ReleaseHand::kRight?1:-1);
                CHECK(Dist(parked.Rotate({1,0,0}),expected)<1e-9);
                CHECK(Dist(parked.Rotate({0,0,1}),{0,0,1})<1e-9);
                // Opposite incoming muzzle directions, twisted controller,
                // and an actor whose barrel is +Y (the actual AR convention).
                for(double turn:{0.,180.}){
                    const FQuat incoming=ToQuat({32,turn,24});
                    const FQuat in_actor=YawOnly(90),old_actor=incoming*in_actor.Inverse();
                    const FVector main_rel{1,2,3},main_world{19,31,127},offset{-8,2,-6};
                    const FVector contact_rel=in_actor.Rotate(gun.contact);
                    const FVector contact_world=main_world+old_actor.Rotate(contact_rel-main_rel);
                    const FVector contact_from_main=incoming.Inverse().Rotate(contact_world-main_world);
                    const FVector destination=FVector{110,-230,90}+out*6+FVector{0,0,13.275};
                    const FVector grab=bench_vise::motion::HandSpot(destination,parked,contact_from_main,offset);
                    FQuat actor_rot;FVector actor_loc;
                    interact::ActorFromFrame(parked,grab+parked.Rotate(offset),in_actor,main_rel,&actor_rot,&actor_loc);
                    CHECK(Dist(actor_loc+actor_rot.Rotate(contact_rel),destination)<1e-8);
                    CHECK(Dist((actor_rot*in_actor).Rotate({1,0,0}),expected)<1e-8);
                }
            }
        }
    }
    FQuat bad;
    CHECK(!workbench::BenchFrame({},ReleaseHand::kRight,&bad));
    CHECK(!workbench::BenchFrame({std::numeric_limits<double>::quiet_NaN(),0,0},ReleaseHand::kLeft,&bad));
    // Pistol release by the hip and away from it must both offer it to the
    // bench. Frozen/tracking-lost input never produces a parking event.
    for(double distance:{1.,100.}){
        interact::HolsterDraw pistol;pistol.Reset(interact::HolsterState::kInHand);
        interact::DrawInput in;in.hand={distance,0,0};in.hand_valid=in.spot_valid=true;in.grip_down=false;
        CHECK(workbench::MayPark(pistol.Update(in)));
        pistol.Reset(interact::HolsterState::kInHand);in.freeze=true;
        CHECK(!workbench::MayPark(pistol.Update(in)));CHECK(pistol.InHand());
        in.freeze=false;in.hand_valid=false;
        CHECK(!workbench::MayPark(pistol.Update(in)));CHECK(pistol.InHand());
    }
    // An old hidden actor can still exist, but only the newly installed
    // actor should be hidden/unhidden when the other hand edits a slot.
    std::vector<game::custom::Installed> parts{{200,"LIGHT_A",7,{5},0},{300,"LIGHT_A",7,{6},0}};
    CHECK(workbench::rules::FittedActor(parts,5,"LIGHT_A")==200);
    CHECK(workbench::rules::FittedActor(parts,6,"LIGHT_A")==300);
    CHECK(workbench::rules::FittedActor(parts,5,"LIGHT_OLD")==0);
    parts[0].actor=500;
    CHECK(workbench::rules::FittedActor(parts,5,"LIGHT_A")==500);
    std::printf("workbench release/actor refresh: %d failures\n",errors);return errors?1:0;
}
