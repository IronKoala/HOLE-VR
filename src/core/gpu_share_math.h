#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

// Pure helpers for gpu_share (no Windows calls), so they can be unit tested.
namespace holevr::gpu_share {

enum class Engine { k3D, kCompute, kVideoEncode, kCopy, kOther };

// One "GPU Engine" counter instance, e.g.
// "pid_1234_luid_0x00000000_0x0000C3B0_phys_0_eng_0_engtype_3D".
struct Instance {
    uint32_t pid = 0;
    std::string luid;  // "0x00000000_0x0000C3B0"
    Engine engine = Engine::kOther;
    std::string engtype;
};

inline Engine EngineOf(const std::string& engtype) {
    if (engtype == "3D") return Engine::k3D;
    if (engtype.rfind("Compute", 0) == 0) return Engine::kCompute;
    if (engtype == "VideoEncode") return Engine::kVideoEncode;
    if (engtype == "Copy") return Engine::kCopy;
    return Engine::kOther;
}

inline bool ParseInstance(const std::string& name, Instance* out) {
    // pid_<n>_luid_<hi>_<lo>_phys_<n>_eng_<n>_engtype_<type>
    if (name.rfind("pid_", 0) != 0) return false;
    const size_t luid = name.find("_luid_");
    const size_t phys = name.find("_phys_");
    const size_t type = name.find("_engtype_");
    if (luid == std::string::npos || phys == std::string::npos || type == std::string::npos) return false;
    if (!(luid < phys && phys < type)) return false;
    const std::string pid = name.substr(4, luid - 4);
    if (pid.empty() || pid.find_first_not_of("0123456789") != std::string::npos) return false;
    out->pid = static_cast<uint32_t>(std::strtoul(pid.c_str(), nullptr, 10));
    out->luid = name.substr(luid + 6, phys - (luid + 6));
    out->engtype = name.substr(type + 9);
    out->engine = EngineOf(out->engtype);
    return !out->luid.empty() && !out->engtype.empty();
}

// Busy percentages of one adapter's engines over one interval, by process.
struct Share {
    std::map<uint32_t, double> by_pid[5];  // index: Engine
    double total[5] = {0, 0, 0, 0, 0};
    std::map<std::string, double> other_types;  // kOther engines by type name
    void Add(const Instance& in, double pct) {
        if (!(pct > 0.0)) return;
        const int e = static_cast<int>(in.engine);
        by_pid[e][in.pid] += pct;
        total[e] += pct;
        if (in.engine == Engine::kOther) other_types[in.engtype] += pct;
    }
};

// "name 78%, name 11%, others 2%": the biggest entries, the rest summed.
inline std::string FormatTop(const std::map<std::string, double>& by_name, size_t keep = 5) {
    std::vector<std::pair<std::string, double>> v(by_name.begin(), by_name.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string out;
    double rest = 0.0;
    char b[160];
    for (size_t i = 0; i < v.size(); ++i) {
        if (i < keep && v[i].second >= 0.5) {
            std::snprintf(b, sizeof(b), "%s%s %.0f%%", out.empty() ? "" : ", ", v[i].first.c_str(), v[i].second);
            out += b;
        } else {
            rest += v[i].second;
        }
    }
    if (rest >= 0.5) {
        std::snprintf(b, sizeof(b), "%sothers %.0f%%", out.empty() ? "" : ", ", rest);
        out += b;
    }
    return out.empty() ? "nothing" : out;
}

// The biggest users of one engine, names looked up through `name_of`.
template <typename NameOf>
std::string Top(const std::map<uint32_t, double>& by_pid, NameOf name_of, size_t keep = 5) {
    std::map<std::string, double> by_name;
    for (const auto& kv : by_pid) by_name[name_of(kv.first)] += kv.second;
    return FormatTop(by_name, keep);
}

// What one program used, by engine type as Windows names them ("3D",
// "Compute_0", "Graphics_1", ...), on every card.
struct ProgramUse {
    std::map<std::string, double> by_type;
    int instances = 0;  // counter instances seen for the program, any value
    void Add(const Instance& in, double pct, bool valid) {
        ++instances;
        if (valid && pct > 0.0) by_type[in.engtype] += pct;
    }
};

}  // namespace holevr::gpu_share
