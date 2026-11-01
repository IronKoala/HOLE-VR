#include "ue/call.h"

#include <map>
#include <mutex>
#include <utility>

#include "core/log.h"
#include "ue/process_event.h"
#include "ue/reflection.h"

namespace holevr::ue {
namespace {

constexpr uint32_t RF_ClassDefaultObject = 0x10;

std::mutex g_mutex;
std::map<std::pair<UObject, std::string>, UObject> g_fn_cache;
std::map<std::pair<UObject, std::string>, int32_t> g_prop_cache;

struct ParamLayout {
    std::map<std::string, std::pair<int32_t, int32_t>> params;  // name -> (offset, size)
    int32_t frame_size = 0;
};
std::map<UObject, ParamLayout> g_layouts;

const ParamLayout& LayoutOf(UObject fn) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_layouts.find(fn);
    if (it != g_layouts.end()) return it->second;
    ParamLayout l;
    l.frame_size = reflection::GetPropertiesSize(fn);
    for (const auto& p : reflection::GetProperties(fn)) {
        if (p.flags & reflection::CPF_Parm)
            l.params[p.name] = {p.offset, p.element_size * (p.array_dim > 0 ? p.array_dim : 1)};
    }
    return g_layouts.emplace(fn, std::move(l)).first->second;
}

}  // namespace

bool IsDefaultObject(UObject obj) {
    return (mem::ReadOr<uint32_t>(obj + off::kObjFlags) & RF_ClassDefaultObject) != 0;
}

UObject FindFunction(UObject cls, const std::string& name) {
    if (!cls) return 0;
    auto key = std::make_pair(cls, name);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_fn_cache.find(key);
        if (it != g_fn_cache.end()) return it->second;
    }
    UObject found = 0;
    for (UObject s = cls; s && !found; s = reflection::GetSuper(s)) {
        for (UObject f : reflection::GetFunctions(s)) {
            if (GetName(f) == name) {
                found = f;
                break;
            }
        }
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (found) g_fn_cache[key] = found;  // do not cache misses: classes load late
    return found;
}

UObject FindFirstInstance(UObject cls) {
    if (!cls) return 0;
    UObject found = 0;
    objects::ForEach([&](int, UObject o) {
        if (found || IsDefaultObject(o)) return;
        if (reflection::IsChildOf(GetClass(o), cls)) found = o;
    });
    return found;
}

int32_t PropertyOffset(UObject obj, const std::string& name) {
    UObject cls = GetClass(obj);
    if (!cls) return -1;
    auto key = std::make_pair(cls, name);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_prop_cache.find(key);
        if (it != g_prop_cache.end()) return it->second;
    }
    int32_t o = reflection::FindPropertyOffset(cls, name);
    if (o < 0) {
        // Blueprint struct members carry a _N_<GUID> suffix; match on the prefix.
        for (UObject s = cls; s && o < 0; s = reflection::GetSuper(s)) {
            for (const auto& p : reflection::GetProperties(s)) {
                if (p.name.size() > name.size() + 34 && p.name.compare(0, name.size(), name) == 0 &&
                    p.name[name.size()] == '_') {
                    o = p.offset;
                    break;
                }
            }
        }
    }
    if (o >= 0) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_prop_cache[key] = o;
    }
    return o;
}

Call::Call(UObject obj, const char* function_name)
    : Call(obj, FindFunction(GetClass(obj), function_name)) {
    name_ = function_name;
}

Call::Call(UObject obj, UObject function) : obj_(obj), fn_(function) {
    if (fn_) {
        const ParamLayout& l = LayoutOf(fn_);
        buf_.assign(static_cast<size_t>(l.frame_size > 0 ? l.frame_size : 0) + 16, 0);
        if (name_.empty()) name_ = GetName(fn_);
    }
}

int32_t Call::Offset(const char* param, size_t size) const {
    if (!fn_) return -1;
    const ParamLayout& l = LayoutOf(fn_);
    auto it = l.params.find(param);
    if (it == l.params.end()) {
        log::Warn("call: %s has no parameter '%s'", name_.c_str(), param);
        return -1;
    }
    auto [o, sz] = it->second;
    if (static_cast<size_t>(sz) != size || o < 0 || o + sz > static_cast<int32_t>(buf_.size())) {
        log::Warn("call: %s parameter '%s' is %d bytes, caller used %zu", name_.c_str(), param, sz, size);
        return -1;
    }
    return o;
}

uint8_t* Call::Raw(const char* param, int32_t* size) {
    if (size) *size = 0;
    if (!fn_) return nullptr;
    const ParamLayout& l = LayoutOf(fn_);
    auto it = l.params.find(param);
    if (it == l.params.end()) return nullptr;
    auto [o, sz] = it->second;
    if (o < 0 || o + sz > static_cast<int32_t>(buf_.size())) return nullptr;
    if (size) *size = sz;
    return buf_.data() + o;
}

bool Call::Invoke() {
    if (!Valid()) {
        log::Warn("call: function '%s' not found on %s", name_.c_str(), GetName(obj_).c_str());
        return false;
    }
    process_event::Invoke(obj_, fn_, buf_.data());
    return true;
}

}  // namespace holevr::ue
