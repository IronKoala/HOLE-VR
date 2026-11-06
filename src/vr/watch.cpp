#include "vr/watch.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <string>
#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "core/perf.h"
#include "game/game.h"
#include "ue/call.h"
#include "ue/process_event.h"
#include "ue/reflection.h"
#include "vr/body/body.h"
#include "vr/hands.h"
#include "vr/input.h"
#include "vr/ui_panel.h"
#include "vr/watch_math.h"
#include "vr/watch_mesh_data.h"
#include "vr/watch_model.h"
#include "vr/world_menu.h"

namespace holevr::vr::watch {
namespace {
using ue::UObject;
struct Ref {
    UObject obj=0,cls=0;int index=-1;ue::FName name{};
    void Set(UObject p) {
        obj=p;cls=p?ue::GetClass(p):0;name=p?ue::GetFName(p):ue::FName{};
        index=p?mem::ReadOr<int32_t>(p+ue::off::kObjIndex,-1):-1;
    }
    bool Live() const {
        if(!obj || index<0 || ue::objects::Get(index)!=obj || ue::GetClass(obj)!=cls)return false;
        const auto n=ue::GetFName(obj);return !std::memcmp(&n,&name,sizeof(n));
    }
};
Ref g_player,g_world,g_manager;
RunClock g_run;
Tap g_tap;
uint64_t g_session=0;
double g_loaded_at=0,g_find_at=0;
bool g_bound=false,g_ended=false,g_enabled=true;
std::string g_map;

double Now(){return perf::NowMs()/1000.0;}
HandAngles LocalTime() {
    SYSTEMTIME t{};GetLocalTime(&t);
    return LocalAngles(t.wHour,t.wMinute,t.wSecond,t.wMilliseconds);
}
std::string Display(double now){return g_run.HasTime()?FormatElapsed(g_run.Elapsed(now)):"";}
UObject World(UObject object) {
    for(int i=0;object && i<8;++i,object=ue::GetOuter(object))
        if(ue::GetName(ue::GetClass(object))=="World")return object;
    return 0;
}
bool Flag(UObject object,const char* name,bool* out) {
    if(!ue::IsLive(object))return false;
    const auto p=ue::reflection::FindProperty(ue::GetClass(object),name);
    uint8_t value=0;
    if(p.type!="BoolProperty" || !p.bool_mask || !mem::Read(object+p.offset,value))return false;
    *out=(value&p.bool_mask)!=0;return true;
}
void Exit(const char* why) {
    if(g_ended)return;
    const bool was_running=g_run.Running();g_run.Exit(Now());g_ended=true;g_tap.Reset();
    if(was_running)log::Info("watch timer: stopped at %s (%s); retained until next level loads",Display(Now()).c_str(),why);
}
bool BeforeExit(UObject self,UObject function,void*) {
    if(g_manager.obj==self && g_manager.Live())Exit(ue::GetName(function).c_str());
    return true;
}
bool BeforePlayerEnd(UObject self,UObject,void*) {
    if(g_player.obj==self && g_player.Live()){Exit("player left level");model::Hide();}
    return true;
}
void HookExitEvents() {
    int hooks=0;
    for(const char* name:{"OpenResults","Open_ResultsUI","NextLevelNoRsltUI"}) {
        const UObject fn=ue::FindFunction(g_manager.cls,name);
        if(fn && ue::process_event::On(fn,&BeforeExit,nullptr))++hooks;
    }
    const UObject end=ue::FindFunction(g_player.cls,"ReceiveEndPlay");
    if(end && ue::process_event::On(end,&BeforePlayerEnd,nullptr))++hooks;
    ue::process_event::HookVtableOf(g_manager.obj);
    log::Info("watch timer: %d/4 exit event hooks; exit-hole and results-state checks also active",hooks);
}
void CheckExitState() {
    if(!g_bound || g_ended || !g_run.Running())return;
    if(!g_player.Live() || !g_world.Live()){Exit("level objects released");return;}
    if(!g_manager.Live())return;
    UObject hole=0;
    ue::GetProp(g_manager.obj,"ActiveHole",hole);
    if(!hole)ue::GetProp(g_player.obj,"InteractTarget",hole);
    if(hole && ue::IsLive(hole) && ue::GetName(ue::GetClass(hole))=="LCBP_Hole_C") {
        bool exit=false,falling=false;
        if(Flag(hole,"IsExitPoint",&exit) && exit && Flag(hole,"IsPlayerFalling",&falling) && falling)
            Exit("entered exit hole");
    }
}
void ObserveLevel(UObject player,double now) {
    if(g_player.obj!=player || !g_player.Live()) {
        Exit("next player loaded");
        model::Hide();g_tap.Reset();g_player.Set(player);g_world.Set(World(player));
        g_map=g_world.Live()?ue::GetName(g_world.obj):"unknown";
        g_manager={};g_bound=false;g_ended=false;g_loaded_at=now;g_find_at=0;
    }
    if(g_bound){CheckExitState();return;}
    if(now<g_find_at || !g_world.Live())return;g_find_at=now+1;
    static ue::ObjectRef cls("BlueprintGeneratedClass /Game/GamePlay/SYS_LevelManager.SYS_LevelManager_C",1);
    const auto manager_class=cls.Get(now);if(!manager_class)return;
    // One lookup per loaded level, restricted to the current player's world.
    ue::objects::ForEach([&](int,UObject object) {
        if(!g_manager.obj && ue::GetClass(object)==manager_class && !ue::IsDefaultObject(object) && World(object)==g_world.obj)
            g_manager.Set(object);
    });
    if(!g_manager.Live())return;
    bool selection=false;
    if(!Flag(g_manager.obj,"IsOpenAreaSelectMode",&selection)){g_manager={};return;}
    const bool home=g_map.find("Area000_Home")!=std::string::npos;
    const bool run=!home && !selection;
    g_bound=true;
    // Start from the first player tick in this level, not from texture loads,
    // body calibration, headset tracking, manager lookup or watch visibility.
    g_run.Loaded(++g_session,run,g_loaded_at);
    HookExitEvents();
    log::Info("watch timer: loaded %s, %s; last/run display %s",g_map.c_str(),run?"run started":"hub (last time retained)",
              g_run.HasTime()?Display(now).c_str():"blank before first run");
    CheckExitState();
}
void OnTick(UObject player,float) {
    const double now=Now();ObserveLevel(player,now);
    Forearm arm;
    if(!g_enabled || !body::GetForearm(hands::kLeft,&arm)) {model::Hide();g_tap.Reset();return;}
    const auto mount=Mount(arm,mesh::kReferenceMount,mesh::kTwistSkinWeight);
    model::Show(player,mount,LocalTime(),Display(now),now);
    body::HandContacts right;hands::Pose head;
    const auto ownership=hands::GetInteraction(hands::kRight);
    TapInput in;in.now=now;
    in.tracked=model::Visible() && hands::Get(hands::kLeft).valid && hands::Get(hands::kRight).valid &&
        hands::Head(&head) && body::GetHandContacts(hands::kRight,&right);
    in.actions=in.tracked && right.empty && ownership.actions_allowed && ownership.owner==hands::GripOwner::kNone &&
        hands::EditorHand()<0 && !body::CalibrationActive() && !ui_panel::MenuOpen() && !g_ended;
    in.head=head.location;in.forward=head.rotation.Rotate({1,0,0});
    in.centre=skel::Apply(mount,mesh::kFaceCentre);in.normal=mount.rotation.Rotate({0,0,1});
    in.elbow=arm.lower.translation;in.tip=right.tip[1];in.tip_radius=right.tip_radius;
    in.radius=mesh::kFaceRadius*std::max(mount.scale.X,mount.scale.Y);
    if(g_tap.Update(in) && ui_panel::RequestPauseFromWatch())input::Haptic(input::kRight,.25f,.035f);
}
} // namespace
void MenuTick() {
    CheckExitState();
    if(world_menu::MenuUp()) {g_tap.Reset();const double now=Now();model::Clock(LocalTime(),Display(now),now);}
}
void ResultsOpened() {if(g_bound && g_run.Running())Exit("results menu");}
void InitGame() {
    g_enabled=config::GetBool("watch_enabled",true);
    game::AddTickListener(&OnTick);
    log::Info("watch: %s; local analog time, per-level retained run timer; raised wrist + gaze + fresh right-index tap opens pause",
              g_enabled?"ready":"disabled");
}
} // namespace holevr::vr::watch
