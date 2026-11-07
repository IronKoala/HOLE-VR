#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include "vr/bench_vise_fit.h"
#include "vr/workbench_pose.h"
#include "vr/workbench_rules.h"
#include "vr/interact/rifle_carry.h"
#include "game/attachment_transaction.h"
#include "game/tube_refresh.h"
using namespace holevr;
using namespace vr;
namespace fit = bench_vise::fit;
namespace rules = workbench::rules;
using game::custom::SavedPart;
using ue::FName;
static int errors = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %d: %s\n",__LINE__,#x); ++errors; } } while (0)
double Distance(FVector a,FVector b) { const auto d=a-b; return std::sqrt(d.X*d.X+d.Y*d.Y+d.Z*d.Z); }
bool Same(const std::vector<SavedPart>& a,const std::vector<SavedPart>& b) {
    if (a.size()!=b.size()) return false;
    for (size_t i=0;i<a.size();++i)
        if (a[i].path!=b[i].path || !game::custom::SameName(a[i].row,b[i].row)) return false;
    return true;
}

int main() {
    int fits = 0, old_bad_gaps = 0;
    double largest_old_error = 0;
    // The actor can move between controls' remembered held pose and the
    // live component reads (base-game animation / body update). Reconstruct
    // the actual placed contact through production ActorFromFrame, not just
    // a second copy of the measurement formula.
    for (const auto& profile : fit::kProfiles) {
        for (double actor_yaw : {-160.,-25.,0.,71.,179.}) {
            const FVector actor_location{32,-160,110}, main_actor{-2,7,1.5}, mesh_actor{1,2,3};
            const FQuat native = YawOnly(90); // native HOLE gun barrel axis
            const FQuat actor_now = ToQuat({13,actor_yaw,-8});
            for (double scale_y : {.85,1.,1.1}) for (const FQuat mesh_tilt : {FQuat{},ToQuat({12,-8,5})}) {
                const FVector scale{1,scale_y,1};
                fit::Sample sample{actor_now,native,actor_now*native*mesh_tilt,
                    actor_location+actor_now.Rotate(mesh_actor),scale,
                    actor_location+actor_now.Rotate(main_actor)};
                FVector contact; double gap=0; FQuat mesh_in_frame;
                CHECK(fit::Measure(profile,sample,&contact,&gap,&mesh_in_frame));
                CHECK(std::fabs(gap-(profile.width*scale_y+.2)) < 1e-8);
                const FVector scaled{profile.contact.X,profile.contact.Y*scale_y,profile.contact.Z};
                const FVector contact_actor=mesh_actor+(native*mesh_tilt).Rotate(scaled);
                const FVector previous_frame_contact = YawOnly(90).Inverse().Rotate(
                    sample.mesh_location+sample.mesh_rotation.Rotate(scaled)-sample.main_location);
                double old_gap = 0;
                if (!fit::Gap(profile,YawOnly(90).Inverse()*sample.mesh_rotation,scale,&old_gap) ||
                    std::fabs(old_gap-gap)>.2) ++old_bad_gaps;
                for (double bench_yaw : {0.,53.,180.}) for (auto hand : {workbench::ReleaseHand::kLeft,workbench::ReleaseHand::kRight}) {
                    const FVector out=YawOnly(bench_yaw).Rotate({1,0,0});
                    FQuat parked; CHECK(workbench::BenchFrame(out,hand,&parked));
                    const FQuat physical_mesh=parked;
                    parked=parked*mesh_in_frame.Inverse();
                    const FVector destination=FVector{114,-230,90}+out*(3.78+gap*.5)+FVector{0,0,13.275};
                    for (const FVector offset : {FVector{-8,2,-6},FVector{3,-1,-4}}) {
                        const auto grab=bench_vise::motion::HandSpot(destination,parked,contact,offset);
                        FQuat rotation; FVector location;
                        interact::ActorFromFrame(parked,grab+parked.Rotate(offset),native,main_actor,&rotation,&location);
                        CHECK(Distance(location+rotation.Rotate(contact_actor),destination)<1e-8);
                        CHECK(Distance((rotation*native*mesh_tilt).Rotate({1,0,0}),physical_mesh.Rotate({1,0,0}))<1e-8);
                        CHECK(Distance((rotation*native*mesh_tilt).Rotate({0,0,1}),{0,0,1})<1e-8);
                        const auto old_grab=bench_vise::motion::HandSpot(destination,parked,previous_frame_contact,offset);
                        interact::ActorFromFrame(parked,old_grab+parked.Rotate(offset),native,main_actor,&rotation,&location);
                        largest_old_error=std::max(largest_old_error,Distance(location+rotation.Rotate(contact_actor),destination));
                        ++fits;
                    }
                }
            }
        }
    }
    CHECK(largest_old_error>2 && old_bad_gaps>0);
    for (int bad=0;bad<3;++bad) {
        fit::Sample sample;
        if (bad==0) sample.mesh_location.X=std::numeric_limits<double>::quiet_NaN();
        if (bad==1) sample.native_frame={0,0,0,0};
        if (bad==2) sample.mesh_scale.Y=0;
        FVector contact{42,43,44}; double gap=99;
        CHECK(!fit::Measure(fit::kProfiles[0],sample,&contact,&gap));
        CHECK(contact.X==42 && contact.Y==43 && contact.Z==44 && gap==99);
    }

    const FName ammo{1,0},mag{2,0},light{3,0},other{4,0},empty{5,0},sight{6,0};
    const std::vector<SavedPart> before{{ammo,{0}},{mag,{1}},{sight,{3}},{light,{5}},{other,{6}}};
    std::vector<SavedPart> moved;
    CHECK(rules::SlotEdit(before,6,light,5,empty,&moved));
    CHECK(rules::StillFitted(moved,5,empty) && rules::StillFitted(moved,6,light));
    CHECK(!rules::StillFitted(moved,5,light)); // no duplicate at the original mount
    CHECK(game::custom::tube_refresh::SameAmmo(before,moved));
    CHECK(rules::StillFitted(moved,3,sight));
    rules::LetGoFacts release{true,5,6,true,true};
    CHECK(rules::DecideLetGo(release)==rules::LetGo::kFit);
    release.slot_can_be_empty=false;
    CHECK(rules::DecideLetGo(release)==rules::LetGo::kBackOnGun);
    release.snapped=5;
    CHECK(rules::DecideLetGo(release)==rules::LetGo::kPutBack);
    for (const auto slots : {std::pair{0,-1},std::pair{6,1},std::pair{256,-1},std::pair{6,7}}) {
        auto output=before;
        CHECK(!rules::SlotEdit(before,slots.first,light,slots.second,empty,&output));
        CHECK(Same(output,before));
    }
    auto output=before;
    CHECK(!rules::SlotEdit(before,6,light,5,FName{},&output)); CHECK(Same(output,before));
    CHECK(rules::SlotEdit(before,7,light,-1,{},&output)); // loose library item
    CHECK(rules::StillFitted(output,7,light) && rules::StillFitted(output,5,light));
    auto saved=before, live=before;
    const auto rollback=game::custom::transaction::Run(
        [&]{ saved=moved; return true; },[&]{ live=moved; return false; },
        [&]{ live=before; return true; },[&]{ saved=before; return true; });
    CHECK(rollback==game::custom::transaction::Result::kRestored);
    CHECK(Same(saved,before) && Same(live,before));

    // Both hand candidates are prepared before the first Grab mutates state.
    // One physical installed part may have at most one temporary hand copy.
    struct Held { bool active=false,from_gun=false; int from_slot=-1; };
    std::array<Held,2> held{};
    std::vector<game::custom::Installed> installed{{101,"LIGHT_A",7,{5},0},{102,"LIGHT_B",7,{6},0}};
    CHECK(rules::CanTakeFittedPart(installed,5,"LIGHT_A",101,held));
    held[0]={true,true,5};
    CHECK(!rules::CanTakeFittedPart(installed,5,"LIGHT_A",101,held));
    CHECK(rules::CanTakeFittedPart(installed,6,"LIGHT_B",102,held));
    installed[1].actor=202;
    CHECK(!rules::CanTakeFittedPart(installed,6,"LIGHT_B",102,held));
    struct Loose { ue::UObject actor; };
    std::vector<Loose> table{{301},{302},{303}};
    CHECK(rules::LooseIndex(table,302)==1);
    table.erase(table.begin());
    CHECK(rules::LooseIndex(table,302)==0 && rules::LooseIndex(table,301)==-1);
    table.erase(table.begin());
    CHECK(rules::LooseIndex(table,302)==-1);

    // A left-carried rifle is a valid target for a part in the right hand.
    // A visual holster approach, magazine, or different gun is not.
    hands::HandGrab grab;
    grab.object=99; grab.kind=hands::GrabKind::kSupportGrip;
    CHECK(rules::HoldsTargetGun(99,grab));
    grab.kind=hands::GrabKind::kGunGrip; CHECK(rules::HoldsTargetGun(99,grab));
    grab.holster_preview=true; CHECK(!rules::HoldsTargetGun(99,grab));
    grab.holster_preview=false; CHECK(!rules::HoldsTargetGun(98,grab));
    for (auto kind:{hands::GrabKind::kNone,hands::GrabKind::kSlideGrab,hands::GrabKind::kMagHeld,hands::GrabKind::kHolster}) {
        grab.kind=kind; CHECK(!rules::HoldsTargetGun(99,grab));
    }
    std::printf("Vise bughunt: %d placement cases; old mixed-pose max contact error %.2f cm; %d old gap failures; %d failures\n",
                fits,largest_old_error,old_bad_gaps,errors);
    return errors?1:0;
}
