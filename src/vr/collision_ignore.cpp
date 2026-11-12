#include "vr/collision_ignore.h"
#include "core/memory.h"
#include "ue/call.h"
#include "vr/world_menu.h"
#include <algorithm>
#include <cmath>
#include <cstring>
namespace holevr::vr::collision_ignore {
namespace {
using ue::UObject;
struct Kept { UObject obj = 0; int32_t index = -1; };
bool Alive(const Kept& k) {
    return k.obj && k.index >= 0 && mem::ReadOr<int32_t>(k.obj + ue::off::kObjIndex, -1) == k.index &&
           ue::objects::Get(k.index) == k.obj;
}
Kept Keep(UObject object) { return {object, mem::ReadOr<int32_t>(object + ue::off::kObjIndex, -1)}; }

Kept g_player;
std::vector<Kept> g_attached;
std::vector<UObject> g_ignore, g_own_gear;
double g_attached_age=1e9;
void RefreshAttached(UObject player, double dt) {
    g_attached_age += dt;
    if (g_attached_age < 0.5) return;
    g_attached_age = 0;
    g_attached.clear();
    ue::Call c(player, "GetAttachedActors");
    if (!c.Valid()) return;
    int32_t flag = 0;
    if (c.Raw("bResetArray", &flag) && flag == 1) c.Set("bResetArray", true);
    if (c.Raw("bRecursivelyIncludeAttachedActors", &flag) && flag == 1) c.Set("bRecursivelyIncludeAttachedActors", true);
    if (!c.Invoke()) return;
    int32_t size = 0;
    const uint8_t* raw = c.Raw("OutActors", &size);
    struct Arr { uintptr_t data; int32_t num, max; } arr{};
    if (!raw || size < static_cast<int32_t>(sizeof(Arr))) return;
    std::memcpy(&arr, raw, sizeof(Arr));
    if (arr.num < 0 || arr.num > arr.max || arr.num > 4096) return;
    for (int i = 0; i < arr.num && i < 64; ++i) {
        UObject a = mem::ReadOr<uintptr_t>(arr.data + i * sizeof(uintptr_t), 0);
        if (a) g_attached.push_back(Keep(a));
    }
}

}
void Reset() {
    g_ignore.clear(); g_attached.clear(); g_attached_age=1e9; g_player={};
}
void Refresh(ue::UObject player,const hands::HandGrab grab[2],double dt) {
    if(!player || !ue::IsLive(player)) { Reset(); return; }
    if(g_player.obj!=player || !Alive(g_player)) { Reset(); g_player=Keep(player); }
    RefreshAttached(player,std::isfinite(dt)?std::clamp(dt,0.0,0.25):0.0);
    g_ignore.clear(); g_ignore.push_back(player);
    for(const Kept& kept:g_attached) if(Alive(kept)) g_ignore.push_back(kept.obj);
    for(int i=0;i<2;++i) if(grab[i].object && ue::IsLive(grab[i].object)) g_ignore.push_back(grab[i].object);
    if(ue::UObject holder=world_menu::PanelHolder()) g_ignore.push_back(holder);
    for(ue::UObject a:g_own_gear) if(ue::IsLive(a) && std::find(g_ignore.begin(),g_ignore.end(),a)==g_ignore.end()) g_ignore.push_back(a);
}
const std::vector<UObject>& IgnoredActors() {
    g_ignore.erase(std::remove_if(g_ignore.begin(), g_ignore.end(), [](UObject a) { return !ue::IsLive(a); }),
                   g_ignore.end());
    return g_ignore;
}
void SetOwnGear(const std::vector<UObject>& actors) { g_own_gear = actors; }

}
