#include "vr/watch_math.h"
#include "vr/watch_mesh_data.h"
#include <cstdio>
#include <limits>
using namespace holevr::vr;
using namespace holevr::vr::watch;
using namespace holevr::vr::ik;
static int failures=0;
#define EXPECT(c) do {if(!(c)){std::printf("FAIL line %d: %s\n",__LINE__,#c);++failures;}} while(0)
static bool Near(double a,double b,double e=1e-7){return std::fabs(a-b)<e;}
static bool Near(FVector a,FVector b,double e=1e-7){return Length(a-b)<e;}
static bool Rotation(FQuat a,FQuat b){return Near(a.Rotate({1,0,0}),b.Rotate({1,0,0}))&&Near(a.Rotate({0,1,0}),b.Rotate({0,1,0}));}
static skel::Xf At(FVector p,FQuat q={}){skel::Xf x;x.translation=p;x.rotation=q;return x;}

void TimerLifecycle() {
    RunClock c;EXPECT(!c.HasTime());
    c.Loaded(1,false,100);EXPECT(!c.HasTime()&&!c.Running());
    c.Loaded(2,true,120);EXPECT(c.HasTime()&&c.Running()&&Near(c.Elapsed(120),0));
    c.Loaded(2,true,140);EXPECT(Near(c.Elapsed(150),30)); // repeated tick never resets
    EXPECT(Near(c.Elapsed(180),60)); // pause retains elapsed wall time
    c.Exit(181.25);EXPECT(!c.Running()&&Near(c.Elapsed(200),61.25));
    c.Exit(205);EXPECT(Near(c.Elapsed(210),61.25)); // results + EndPlay are idempotent
    c.Loaded(2,true,220);EXPECT(!c.Running()); // old level ticks during exit
    c.Loaded(3,false,230);EXPECT(Near(c.Elapsed(300),61.25)); // hub retains last
    c.Loaded(4,true,400);EXPECT(Near(c.Elapsed(401),1)); // even same map, new instance
    c.Loaded(5,true,410);EXPECT(Near(c.Elapsed(411),1)); // next run without visiting hub
    c.Exit(420);c.Loaded(6,false,430);EXPECT(Near(c.Elapsed(999),10));
    c.Loaded(0,true,1000);EXPECT(!c.Running());
    EXPECT(FormatElapsed(0)=="00:00");EXPECT(FormatElapsed(59.99)=="00:59");
    EXPECT(FormatElapsed(61.25)=="01:01");EXPECT(FormatElapsed(3599)=="59:59");
    EXPECT(FormatElapsed(3600)=="01:00:00");EXPECT(FormatElapsed(3661)=="01:01:01");
    EXPECT(FormatElapsed(999999)=="99:59:59");EXPECT(FormatElapsed(-1)=="00:00");
    EXPECT(FormatElapsed(std::numeric_limits<double>::quiet_NaN())=="00:00");
    const auto midnight=LocalAngles(0,0,0,0),afternoon=LocalAngles(15,30,30,500);
    EXPECT(Near(midnight.hour,0)&&Near(midnight.minute,0)&&Near(midnight.second,0));
    EXPECT(Near(afternoon.hour,105.25416666666667*kDegToRad));
    EXPECT(Near(afternoon.minute,183.05*kDegToRad));EXPECT(Near(afternoon.second,183*kDegToRad));
    // 12 points along -Y after Blender -> UE reflection; 3 points along +X.
    EXPECT(Near(AxisAngle({0,0,1},LocalAngles(3,0,0,0).hour).Rotate({0,-1,0}),{1,0,0}));
    EXPECT(Near(LocalAngles(23,59,59,999).hour,LocalAngles(11,59,59,999).hour));
}
TapInput Raised() {
    TapInput i;i.now=10;i.tracked=i.actions=true;i.head={0,0,165};i.centre={35,0,145};
    i.elbow={15,-15,130};i.normal=Normalize(i.head-i.centre);i.forward=i.normal*-1;
    i.tip=i.centre+i.normal*3;return i;
}
void Hover(Tap& t,TapInput& i,int frames=16) {
    for(int n=0;n<frames;++n){i.now+=1.0/72;EXPECT(!t.Update(i));}
}
bool Touch(Tap& t,TapInput& i){i.now+=1.0/72;i.tip=i.centre+i.normal*.4;return t.Update(i);}
void GestureGates() {
    auto good=Raised();EXPECT(LookingAtRaisedWatch(good));
    for(int block=0;block<9;++block) {
        Tap t;auto i=good;
        switch(block) {
            case 0:i.tracked=false;break;
            case 1:i.actions=false;break;
            case 2:i.forward={-1,0,0};break;
            case 3:i.normal=i.normal*-1;break;
            case 4:i.centre.Z=120;i.tip=i.centre+i.normal*3;break; // arm down
            case 5:i.elbow.Z=155;break; // wrist hangs below elbow
            case 6:i.centre=i.head+i.forward*10;break; // too close
            case 7:i.centre=i.head+i.forward*90;break; // too far
            case 8:i.head.X=std::numeric_limits<double>::quiet_NaN();break;
        }
        EXPECT(!LookingAtRaisedWatch(i));Hover(t,i);EXPECT(!Touch(t,i));
    }
    {Tap t;auto i=good;Hover(t,i);EXPECT(Touch(t,i));Hover(t,i,100);EXPECT(!Touch(t,i));} // held touch
    {Tap t;auto i=good;i.tip=i.centre+i.normal*.4;Hover(t,i,100);EXPECT(!Touch(t,i));} // look at existing contact
    {Tap t;auto i=good;i.tip=i.tip+FVector{0,3,0};Hover(t,i);i.tip=i.centre+i.normal*.4+FVector{0,3,0};i.now+=.014;EXPECT(!t.Update(i));} // misses face
    {Tap t;auto i=good;Hover(t,i);i.tip=i.centre-i.normal*2;i.now+=.014;EXPECT(t.Update(i));} // fast tap crossing between frames
    {Tap t;auto i=good;Hover(t,i);i.now+=1;EXPECT(!Touch(t,i));} // tracking gap cannot synthesize tap
    {Tap t;auto i=good;Hover(t,i);i.centre.X+=100;i.tip.X+=100;EXPECT(!Touch(t,i));} // teleport
    {Tap t;auto i=good;Hover(t,i);i.actions=false;EXPECT(!Touch(t,i));i.actions=true;Hover(t,i,30);EXPECT(!Touch(t,i));} // menu/ownership
    {Tap t;auto i=good;Hover(t,i);EXPECT(Touch(t,i));i.tip=i.centre+i.normal*3;Hover(t,i,12);EXPECT(!Touch(t,i));
        i.tip=i.centre+i.normal*3;Hover(t,i,72);EXPECT(Touch(t,i));} // lift + cooldown permits next tap
    {Tap t;auto i=good;Hover(t,i,3);EXPECT(!Touch(t,i));} // no dwell
}
void WristMount() {
    Forearm a;
    a.ref_lower=At({3,12,127},ToQuat({12,-68,90}));
    a.ref_hand=skel::Compose(At({26.975,0,0},ToQuat({10,12,30})),a.ref_lower);
    a.ref_twist=a.ref_lower;a.lower=a.ref_lower;a.hand=a.ref_hand;
    const auto local=mesh::kReferenceMount;
    auto want=skel::Compose(local,a.ref_lower),got=Mount(a,local,0);
    EXPECT(Near(got.translation,want.translation));EXPECT(Rotation(got.rotation,want.rotation));
    // Calibrated forearm can shorten/lengthen without sliding the band away
    // from the wrist, or multiplying wrist thickness by segment length.
    for(double length:{18.,22.124314128,26.3728101166,32.}) {
        a.lower=At({7,5,120},ToQuat({-30,100,30}));
        a.hand=skel::Compose(At({length,0,0}),a.lower);
        got=Mount(a,local,0);
        const auto back=a.lower.rotation.Inverse().Rotate(got.translation-a.hand.translation);
        EXPECT(Near(back,local.translation-FVector{26.975,0,0}));
        EXPECT(Near(got.scale,{1,1,1}));
    }
    a.lower.scale={.9,.9,.9};a.hand.translation={40,0,140};got=Mount(a,local,0);
    EXPECT(Near(a.lower.rotation.Inverse().Rotate(got.translation-a.hand.translation),
                (local.translation-FVector{26.975,0,0})*.9));
    EXPECT(Near(got.scale,{.9,.9,.9}));
    // Twist-bone rest axes must not add a roll at zero skin weight.
    a.has_twist=true;a.ref_twist.rotation=a.ref_lower.rotation*AxisAngle({1,0,0},.4);
    a.twist.rotation=a.lower.rotation*AxisAngle({1,0,0},1.2);
    EXPECT(Rotation(Mount(a,local,mesh::kTwistSkinWeight).rotation,a.lower.rotation*local.rotation));
    a.twist.rotation=a.lower.rotation*(a.ref_lower.rotation.Inverse()*a.ref_twist.rotation);
    EXPECT(Rotation(Mount(a,local,1).rotation,a.lower.rotation*local.rotation));
}
int main(){TimerLifecycle();GestureGates();WristMount();std::printf("watch: %s (%d failures)\n",failures?"FAILED":"passed",failures);return failures?1:0;}
