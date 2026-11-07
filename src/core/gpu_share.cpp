#include "core/gpu_share.h"

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <tlhelp32.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/gpu_share_math.h"
#include "core/log.h"
#include "core/perf.h"

namespace holevr::gpu_share {
namespace {

using OpenQueryFn = PDH_STATUS(WINAPI*)(LPCWSTR, DWORD_PTR, PDH_HQUERY*);
using AddCounterFn = PDH_STATUS(WINAPI*)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER*);
using CollectFn = PDH_STATUS(WINAPI*)(PDH_HQUERY);
using ArrayFn = PDH_STATUS(WINAPI*)(PDH_HCOUNTER, DWORD, LPDWORD, LPDWORD, PPDH_FMT_COUNTERVALUE_ITEM_W);
using CloseFn = PDH_STATUS(WINAPI*)(PDH_HQUERY);

struct Pdh {
    OpenQueryFn open = nullptr;
    AddCounterFn add = nullptr;
    CollectFn collect = nullptr;
    ArrayFn array = nullptr;
    CloseFn close = nullptr;
} g_pdh;

constexpr DWORD kIntervalMs = 10000;
constexpr int kReopenEvery = 6;  // re-list the counters each minute, for programs started later

std::string Narrow(const wchar_t* w) {
    if (!w) return {};
    char buf[512];
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
    return n > 0 ? std::string(buf) : std::string();
}

// pid -> program name, from one snapshot of all processes (needs no access
// rights to the processes themselves, so system ones like dwm.exe are named).
std::map<uint32_t, std::string> ProcessNames() {
    std::map<uint32_t, std::string> names;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return names;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof(e);
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
        names[e.th32ProcessID] = Narrow(e.szExeFile);
    CloseHandle(snap);
    return names;
}

bool Open(PDH_HQUERY* q, PDH_HCOUNTER* c, PDH_STATUS* status) {
    *status = g_pdh.open(nullptr, 0, q);
    if (*status != ERROR_SUCCESS) return false;
    *status = g_pdh.add(*q, L"\\GPU Engine(*)\\Utilization Percentage", 0, c);
    if (*status != ERROR_SUCCESS) {
        g_pdh.close(*q);
        return false;
    }
    *status = g_pdh.collect(*q);  // the first collection is the baseline for the next
    return true;
}

DWORD WINAPI Loop(LPVOID) {
    HMODULE dll = LoadLibraryW(L"pdh.dll");
    if (dll) {
        g_pdh.open = reinterpret_cast<OpenQueryFn>(GetProcAddress(dll, "PdhOpenQueryW"));
        g_pdh.add = reinterpret_cast<AddCounterFn>(GetProcAddress(dll, "PdhAddEnglishCounterW"));
        g_pdh.collect = reinterpret_cast<CollectFn>(GetProcAddress(dll, "PdhCollectQueryData"));
        g_pdh.array = reinterpret_cast<ArrayFn>(GetProcAddress(dll, "PdhGetFormattedCounterArrayW"));
        g_pdh.close = reinterpret_cast<CloseFn>(GetProcAddress(dll, "PdhCloseQuery"));
    }
    if (!g_pdh.open || !g_pdh.add || !g_pdh.collect || !g_pdh.array || !g_pdh.close) {
        log::Warn("gpu share: Windows' performance counter library (pdh.dll) is not usable; off");
        return 0;
    }
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER counter = nullptr;
    PDH_STATUS status = 0;
    if (!Open(&query, &counter, &status)) {
        log::Warn("gpu share: Windows' GPU Engine counters are not available (status 0x%08lx); off",
                  static_cast<unsigned long>(status));
        return 0;
    }
    log::Info("gpu share: on; every 10 s, how busy each graphics card engine was and which programs used it "
              "(gpu_share=0 turns it off)");
    const uint32_t own = GetCurrentProcessId();
    std::vector<unsigned char> buf;
    int failures = 0;
    for (int round = 1;; ++round) {
        Sleep(kIntervalMs);
        const double t0 = perf::NowMs();
        status = g_pdh.collect(query);
        DWORD size = 0, count = 0;
        if (status == ERROR_SUCCESS)
            status = g_pdh.array(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, nullptr);
        if (status == static_cast<PDH_STATUS>(PDH_MORE_DATA)) {
            buf.assign(size, 0);
            status = g_pdh.array(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count,
                                 reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(buf.data()));
        }
        if (status != ERROR_SUCCESS) {
            if (++failures <= 3)
                log::Info("gpu share: reading the counters failed (status 0x%08lx)", static_cast<unsigned long>(status));
            if (failures >= 30) {
                log::Warn("gpu share: the counters keep failing; off");
                g_pdh.close(query);
                return 0;
            }
            continue;
        }
        failures = 0;
        std::map<std::string, Share> by_luid;
        const auto* items = reinterpret_cast<const PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
        for (DWORD i = 0; i < count; ++i) {
            if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
                items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)
                continue;
            Instance in;
            if (!ParseInstance(Narrow(items[i].szName), &in)) continue;
            by_luid[in.luid].Add(in, items[i].FmtValue.doubleValue);
        }
        // The card the game draws on: the one where the game uses the 3D
        // engine most (else the busiest one).
        const Share* card = nullptr;
        double best = -1.0;
        for (const auto& kv : by_luid) {
            const auto& g = kv.second.by_pid[static_cast<int>(Engine::k3D)];
            const auto it = g.find(own);
            const double score = (it != g.end() ? 1000.0 + it->second : 0.0) + kv.second.total[0];
            if (score > best) best = score, card = &kv.second;
        }
        if (card) {
            const std::map<uint32_t, std::string> names = ProcessNames();
            auto name_of = [&](uint32_t pid) -> std::string {
                if (pid == own) return "the game";
                const auto it = names.find(pid);
                return it != names.end() && !it->second.empty() ? it->second : "pid " + std::to_string(pid);
            };
            const int e3d = static_cast<int>(Engine::k3D), ecomp = static_cast<int>(Engine::kCompute);
            const int eenc = static_cast<int>(Engine::kVideoEncode), ecopy = static_cast<int>(Engine::kCopy);
            log::Info("gpu share (last 10 s): 3D engine busy %.0f%%: %s | compute %.0f%%: %s | video encode %.0f%%: "
                      "%s | copy %.0f%%: %s (read in %.0f ms)",
                      card->total[e3d], Top(card->by_pid[e3d], name_of).c_str(), card->total[ecomp],
                      Top(card->by_pid[ecomp], name_of, 3).c_str(), card->total[eenc],
                      Top(card->by_pid[eenc], name_of, 3).c_str(), card->total[ecopy],
                      Top(card->by_pid[ecopy], name_of, 3).c_str(), perf::NowMs() - t0);
        }
        if (round % kReopenEvery == 0) {
            g_pdh.close(query);
            if (!Open(&query, &counter, &status)) {
                log::Warn("gpu share: reopening the counters failed (status 0x%08lx); off",
                          static_cast<unsigned long>(status));
                return 0;
            }
        }
    }
}

}  // namespace

void Start() {
    if (!config::GetBool("gpu_share", true)) return;
    if (HANDLE t = CreateThread(nullptr, 0, &Loop, nullptr, 0, nullptr)) CloseHandle(t);
}

}  // namespace holevr::gpu_share
