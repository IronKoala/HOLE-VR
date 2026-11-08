#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>
#include "vr/bench_vise_fit.h"
#include "vr/bench_vise_layout.h"
#include "vr/workbench_carry.h"
#include "game/tube_refresh.h"
using namespace holevr;
using namespace vr;
static int errors=0;
#define CHECK(x) do {if(!(x)){std::printf("FAIL line %d: %s\n",__LINE__,#x);++errors;}}while(0)

// Models the destructive engine refresh and a tube with distinct shell
// identities, including a mixed-ammunition order and an empty chamber.
struct Engine {
    std::vector<int> shells{12,27,9,31};int chamber=45;bool jammed=true;
    bool fields=true;std::string missing;std::vector<std::string> calls;
    bool parts=false,muzzle=false,sight=false;double rpm=300,spr=1;
    bool Has(const char* n){return missing!=n;}
    bool RateFieldsValid(){return fields;}
    bool Call(const char* n){
        calls.push_back(n);
        std::string s=n;
        if(s=="ClearMagazine" || s=="Init_MagazineCreate" || s=="RefreshGunSpec") {shells.clear();chamber=0;jammed=false;}
        if(s=="Init_GenerateWeaponParts")parts=true;
        if(s=="Init_MuzzlePointAndFX")muzzle=true;
        if(s=="Init_ADSPoint")sight=true;
        return true;
    }
    double Rpm(){return rpm;}
    bool SetSecondsPerRound(double d){spr=d;return true;}
};

int main(){
    namespace fit=bench_vise::fit;namespace motion=bench_vise::motion;
    const double expected[]={3.396,2.581016,3.196003,3.813942};
    for(int i=0;i<4;++i){
        const auto& p=fit::kProfiles[i];double gap=0;
        CHECK(fit::Gap(p,FQuat{},FVector{1,1,1},&gap));CHECK(std::fabs(gap-expected[i])<1e-5);
        CHECK(fit::MatchesBounds(p,p.lo,p.hi));CHECK(!fit::MatchesBounds(p,p.lo,p.hi+FVector{1,0,0}));
        CHECK(!fit::Gap(p,FQuat{},FVector{1,5,1},&gap));
        CHECK(!fit::Gap(p,FQuat{},FVector{1,0,1},&gap));
        CHECK(!fit::Gap(p,FQuat{},FVector{1,std::numeric_limits<double>::quiet_NaN(),1},&gap));
        // Custom hand offsets, both barrel directions, arbitrary bench yaw.
        for(double yaw:{0.,90.,180.,267.}){
            const FQuat frame=ToQuat(FRotator{0,yaw,0});const FVector contact{53,76,103};
            const FVector from_main=p.contact-FVector{-1,.7,-.3},offset{-8,2,-6};
            const FVector grab=motion::HandSpot(contact,frame,from_main,offset);
            const FVector reconstructed=grab+frame.Rotate(offset+from_main);
            CHECK(std::fabs(reconstructed.X-contact.X)<1e-8 && std::fabs(reconstructed.Y-contact.Y)<1e-8 &&
                  std::fabs(reconstructed.Z-contact.Z)<1e-8);
        }
    }
    // Stock/fore-end clearance: the complete spare-part bounds, not their
    // origins, remain at least 12 cm in from the front edge.
    bench_vise::layout::PartsRow parts(12);
    for(int i=0;i<12;++i){double hw=i%3==0?17:4,hd=i%2?3:5;auto s=parts.Next(hw,hd,122);
        CHECK(s.inset-hd>=12 && std::fabs(s.along)-hw>=14 && s.inset+hd<111);}

    using namespace interact;
    CarryInput c;c.have_rifle=true;c.rifle_current=true;c.right_has_rifle=true;c.right_grip_released=true;
    c.left_support=true;c.left_grip_down=true;
    CHECK(Decide(c)==CarryAction::kRightLetGo);CHECK(!workbench::MayPark(Decide(c),true));
    c.left_support=false;CHECK(workbench::MayPark(Decide(c),true));
    c.right_has_rifle=false;c.carrying=true;c.left_grip_down=false;
    CHECK(Decide(c)==CarryAction::kLetGo);CHECK(workbench::MayPark(Decide(c),true));
    CHECK(!workbench::MayPark(Decide(c),false));
    c.switching=true;CHECK(!workbench::MayPark(Decide(c),true));
    c={};c.have_rifle=true;c.rifle_current=true;c.right_grip_pressed=true;c.right_on_rifle_grip=true;
    CHECK(Decide(c)==CarryAction::kRightTakesRifle);CHECK(!workbench::MayPark(Decide(c),true));
    c.right_grip_pressed=false;c.left_grip_pressed=true;c.left_on_handguard=true;
    CHECK(Decide(c)==CarryAction::kPickUp);CHECK(!workbench::MayPark(Decide(c),true));

    namespace refresh=game::custom::tube_refresh;
    for(bool empty:{false,true}){
        Engine e;if(empty){e.shells.clear();e.chamber=0;e.jammed=false;}
        const auto shells=e.shells;const int chamber=e.chamber;const bool jam=e.jammed;
        CHECK(refresh::Run(e));CHECK(e.parts&&e.muzzle&&e.sight&&std::fabs(e.spr-.2)<1e-10);
        CHECK(e.shells==shells&&e.chamber==chamber&&e.jammed==jam);
    }
    for(const char* missing:refresh::kSteps){Engine e;e.missing=missing;CHECK(!refresh::Run(e));CHECK(e.calls.empty());}
    Engine missing;missing.fields=false;CHECK(!refresh::Run(missing));CHECK(missing.calls.empty());
    using game::custom::SavedPart;
    std::vector<SavedPart> a{{{10,0},{0}},{{20,0},{1}},{{30,0},{2}},{{40,0},{3}}},b=a;
    b[2].row={31,0};CHECK(refresh::SameAmmo(a,b));
    b[0].row={11,0};CHECK(!refresh::SameAmmo(a,b));
    b=a;b.erase(b.begin()+1);CHECK(!refresh::SameAmmo(a,b));
    b=a;b.push_back({{99,0},{1,2}});CHECK(!refresh::SameAmmo(a,b));
    std::printf("vise weapon fit/carry/tube refresh: %d failures\n",errors);return errors?1:0;
}
