#include "vr/gpu_timer.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "core/config.h"
#include "core/log.h"
#include "core/perf.h"
#include "hooks/hooks.h"

namespace holevr::vr::gpu_timer {
namespace {

// ID3D12CommandQueue: IUnknown 0-2, ID3D12Object 3-6, ID3D12DeviceChild 7,
// UpdateTileMappings 8, CopyTileMappings 9, ExecuteCommandLists 10.
constexpr size_t kExecuteCommandLists = 10;
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
ExecuteFn o_execute = nullptr;

std::atomic<ID3D12CommandQueue*> g_queue{nullptr};
std::atomic<bool> g_ready{false};
std::atomic<bool> g_want_start{false};
thread_local bool t_inside = false;  // our own submits, and everything inside the game's Present

constexpr int kSlots = 16;  // stamps in flight (2 a frame)
struct Slot {
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    UINT64 fence = 0;  // done once the fence reaches this
    bool pending = false;
    bool is_end = false;
    long long frame = 0;
    double present_ms = 0.0;  // end stamps: CPU time of the Present
};
std::mutex g_mutex;  // leaf lock: recording and reading stamps
Slot g_slots[kSlots];
int g_next = 0;
ID3D12QueryHeap* g_heap = nullptr;
ID3D12Resource* g_readback = nullptr;
ID3D12Fence* g_fence = nullptr;
UINT64 g_fence_value = 0;
UINT64 g_freq = 0;
UINT64 g_cal_gpu = 0, g_cal_cpu = 0;
DWORD g_cal_at = 0;
long long g_frame = 0;  // presents seen
long long g_skipped = 0;

// Results by frame (a small ring): GPU ticks, 0 = not known.
constexpr int kRing = 64;
struct FrameStamps {
    long long frame = -1;
    UINT64 start = 0, end = 0;
    double present_ms = 0.0;
};
FrameStamps g_ring[kRing];

// Totals since the last TakeWindow (the GPU cost test), under g_mutex.
Window g_window;

perf::Counter g_perf_frame{"GPU frame (its first work to done)"};
perf::Counter g_perf_idle{"GPU idle before the frame's work"};
perf::Counter g_perf_done{"GPU done after the game's Present (stamped at Present)"};

double TicksToMs(double ticks) { return ticks * 1000.0 / static_cast<double>(g_freq); }

double GpuToCpuMs(UINT64 ts) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return static_cast<double>(g_cal_cpu) * 1000.0 / static_cast<double>(f.QuadPart) +
           TicksToMs(static_cast<double>(ts) - static_cast<double>(g_cal_gpu));
}

FrameStamps& Ring(long long frame) {
    FrameStamps& r = g_ring[frame % kRing];
    if (r.frame != frame) r = FrameStamps{frame, 0, 0, 0.0};
    return r;
}

void Finish(long long frame) {
    FrameStamps& r = g_ring[frame % kRing];
    if (r.frame != frame || !r.start || !r.end || r.end < r.start) return;
    const double gpu_ms = TicksToMs(static_cast<double>(r.end - r.start));
    g_perf_frame.Add(gpu_ms);
    g_window.frame_ms += gpu_ms;
    ++g_window.frames;
    if (g_cal_at && r.present_ms > 0.0) {
        const double done = GpuToCpuMs(r.end) - r.present_ms;
        if (done > -50.0 && done < 1000.0) g_perf_done.Add(std::max(done, 0.0));
    }
    const FrameStamps& prev = g_ring[(frame - 1) % kRing];
    if (prev.frame == frame - 1 && prev.end && r.start >= prev.end) {
        const double idle = TicksToMs(static_cast<double>(r.start - prev.end));
        g_perf_idle.Add(idle);
        g_window.idle_ms += idle;
        ++g_window.idles;
    }
}

// Reads every stamp the GPU has finished. g_mutex held.
void Collect() {
    if (!g_fence) return;
    const UINT64 done = g_fence->GetCompletedValue();
    for (int i = 0; i < kSlots; ++i) {
        Slot& s = g_slots[i];
        if (!s.pending || done < s.fence) continue;
        s.pending = false;
        void* mapped = nullptr;
        D3D12_RANGE range{i * sizeof(UINT64), (i + 1) * sizeof(UINT64)};
        if (FAILED(g_readback->Map(0, &range, &mapped)) || !mapped) continue;
        UINT64 ts = 0;
        std::memcpy(&ts, static_cast<const uint8_t*>(mapped) + range.Begin, sizeof(ts));
        D3D12_RANGE none{0, 0};
        g_readback->Unmap(0, &none);
        if (!ts) continue;
        FrameStamps& r = Ring(s.frame);
        if (s.is_end) {
            r.end = ts;
            r.present_ms = s.present_ms;
            Finish(s.frame);
            Finish(s.frame + 1);  // its idle needs this end
        } else {
            r.start = ts;
            Finish(s.frame);
        }
    }
}

// Queues one timestamp on the game's queue. g_mutex held.
void Stamp(bool is_end, long long frame, double present_ms) {
    Slot& s = g_slots[g_next];
    if (s.fence && g_fence->GetCompletedValue() < s.fence) {
        ++g_skipped;  // the GPU is more than 8 frames behind: skip rather than wait
        return;
    }
    if (FAILED(s.alloc->Reset()) || FAILED(s.list->Reset(s.alloc, nullptr))) return;
    s.list->EndQuery(g_heap, D3D12_QUERY_TYPE_TIMESTAMP, g_next);
    s.list->ResolveQueryData(g_heap, D3D12_QUERY_TYPE_TIMESTAMP, g_next, 1, g_readback, g_next * sizeof(UINT64));
    if (FAILED(s.list->Close())) return;
    ID3D12CommandList* lists[] = {s.list};
    ID3D12CommandQueue* q = g_queue.load();
    const bool was_inside = t_inside;
    t_inside = true;
    o_execute(q, 1, lists);
    t_inside = was_inside;
    if (FAILED(q->Signal(g_fence, ++g_fence_value))) return;
    s.fence = g_fence_value;
    s.pending = true;
    s.is_end = is_end;
    s.frame = frame;
    s.present_ms = present_ms;
    g_next = (g_next + 1) % kSlots;
}

void STDMETHODCALLTYPE HookExecute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    if (!t_inside && q == g_queue.load() && g_ready.load(std::memory_order_acquire) &&
        g_want_start.exchange(false)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        Stamp(false, g_frame + 1, 0.0);
    }
    o_execute(q, n, lists);
}

bool CreateResources(ID3D12CommandQueue* queue) {
    ID3D12Device* device = nullptr;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || !device) return false;
    bool ok = SUCCEEDED(queue->GetTimestampFrequency(&g_freq)) && g_freq;
    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = kSlots;
    ok = ok && SUCCEEDED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_heap)));
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = kSlots * sizeof(UINT64);
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ok = ok && SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&g_readback)));
    ok = ok && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)));
    for (Slot& s : g_slots) {
        ok = ok && SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.alloc)));
        ok = ok && SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.alloc, nullptr,
                                                       IID_PPV_ARGS(&s.list)));
        if (ok) s.list->Close();
    }
    device->Release();
    return ok;
}

std::string Report(double) {
    long long skipped;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        skipped = g_skipped;
        g_skipped = 0;
    }
    if (!skipped) return {};
    char b[160];
    std::snprintf(b, sizeof(b), "GPU timer: %lld stamps skipped (the GPU was more than 8 frames behind)", skipped);
    return b;
}

}  // namespace

void Init(ID3D12CommandQueue* queue) {
    if (!queue || o_execute || !config::GetBool("gpu_timer", true)) return;
    if (!CreateResources(queue)) {
        log::Warn("GPU timer: could not create its timestamp resources; off");
        return;
    }
    g_queue = queue;
    void** vt = *reinterpret_cast<void***>(queue);
    o_execute = reinterpret_cast<ExecuteFn>(
        hooks::PatchVtableSlot(vt, kExecuteCommandLists, reinterpret_cast<void*>(&HookExecute)));
    if (!o_execute) {
        log::Warn("GPU timer: could not hook the queue; off");
        return;
    }
    perf::Register(&g_perf_frame);
    perf::Register(&g_perf_idle);
    perf::Register(&g_perf_done);
    perf::AddReportLine(&Report);
    g_ready.store(true, std::memory_order_release);
    log::Info("GPU timer: on (timestamps at each Present and before the next frame's first submit)");
}

void PresentBegin() {
    t_inside = true;
    if (!g_ready.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    DWORD now = GetTickCount();
    if (!g_cal_at || now - g_cal_at > 2000) {
        if (SUCCEEDED(g_queue.load()->GetClockCalibration(&g_cal_gpu, &g_cal_cpu))) g_cal_at = now;
    }
    Collect();
    ++g_frame;
    Stamp(true, g_frame, perf::NowMs());
}

Window TakeWindow() {
    std::lock_guard<std::mutex> lock(g_mutex);
    Window w = g_window;
    g_window = Window{};
    return w;
}

bool On() { return g_ready.load(std::memory_order_acquire); }

void PresentEnd() {
    t_inside = false;
    if (g_ready.load(std::memory_order_acquire)) g_want_start.store(true);
}

}  // namespace holevr::vr::gpu_timer
