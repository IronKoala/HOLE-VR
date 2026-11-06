#pragma once
#include <algorithm>
#include <vector>

namespace holevr::vr::input {
struct HapticRequest { int hand; float amplitude=0,seconds=0,frequency=0; bool stop=false; };
// Stop supersedes pending pulses for THIS hand, including a queued ramp.
// Other-hand requests retain order and content. A stop cannot be dropped just
// because the normal pulse budget is full; repeated stops coalesce per hand.
inline void QueueHapticStop(std::vector<HapticRequest>& queue,int hand) {
    queue.erase(std::remove_if(queue.begin(),queue.end(),[&](const auto& r){return r.hand==hand;}),queue.end());
    queue.push_back({hand,0,0,0,true});
}
} // namespace holevr::vr::input
