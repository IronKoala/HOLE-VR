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

// Where in a frame a stamp sits. kEnd: the game's Present was called (its
// frame is queued). kCopied: our headset copy and image release are queued.
// kHanded: xrEndFrame returned (the runtime queued its hand-off work).
// kPresented: the game's real Present returned. kStart: before the next
// frame's first command lists.
enum Kind { kEnd, kCopied, kHanded, kPresented, kStart, kKinds };

constexpr int kSlots = 64;  // stamps in flight (5 a frame)
struct Slot {
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    UINT64 fence = 0;  // done once the fence reaches this
    bool pending = false;
    Kind kind = kEnd;
    long long frame = 0;
    double cpu_ms = 0.0;  // perf::NowMs() when it was queued
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
std::atomic<long long> g_frame{0};  // presents seen
long long g_skipped = 0;

// Results by frame (a small ring): GPU ticks, 0 = not known. A frame's start
// stamp is the one before its first work; its other stamps come after it.
constexpr int kRing = 64;
struct FrameStamps {
    long long frame = -1;
    UINT64 ts[kKinds] = {};
    double cpu_ms[kKinds] = {};
    bool counted = false;     // its GPU frame and "done" are counted
    bool idle_done = false;   // the idle before it is counted
    bool split_done = false;  // the gap before it is counted
};
FrameStamps g_ring[kRing];

// Totals since the last TakeWindow (the GPU cost test), under g_mutex.
Window g_window;

perf::Counter g_perf_frame{"GPU frame (its first work to done)"};
perf::Counter g_perf_idle{"GPU idle before the frame's work"};
perf::Counter g_perf_done{"GPU done after the game's Present (stamped at Present)"};
// The time between one frame's end and the next one's start, split by what
// was queued in it (GPU clock). "lag" = how long after it was queued the GPU
// reached the next frame's first work: near 0 means the GPU sat waiting for
// the CPU to queue it; larger means the GPU was still busy.
perf::Counter g_perf_gap_copy{"GPU between frames: game frame end to our copy queued"};
perf::Counter g_perf_gap_hand{"GPU between frames: our copy to xrEndFrame done"};
perf::Counter g_perf_gap_present{"GPU between frames: xrEndFrame to the game's Present done"};
perf::Counter g_perf_gap_next{"GPU between frames: Present to the next frame's first work"};
perf::Counter g_perf_start_lag{"GPU reached the next frame's first work, after it was queued"};

double TicksToMs(double ticks) { return ticks * 1000.0 / static_cast<double>(g_freq); }

double GpuToCpuMs(UINT64 ts) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return static_cast<double>(g_cal_cpu) * 1000.0 / static_cast<double>(f.QuadPart) +
           TicksToMs(static_cast<double>(ts) - static_cast<double>(g_cal_gpu));
}

FrameStamps& Ring(long long frame) {
    FrameStamps& r = g_ring[frame % kRing];
    if (r.frame != frame) {
        r = FrameStamps{};
        r.frame = frame;
    }
    return r;
}

// The gap after `prev`'s end, split by its stamps, once all of them and the
// next frame's start are in. A stamp out of order (xrEndFrame on its own
// thread can return after the next frame started) leaves that split out.
void Split(const FrameStamps& prev, FrameStamps& next) {
    if (next.split_done || !next.ts[kStart]) return;
    const UINT64 e = prev.ts[kEnd], c = prev.ts[kCopied], h = prev.ts[kHanded], p = prev.ts[kPresented];
    const UINT64 s = next.ts[kStart];
    if (!e || !c || !h || !p) return;
    next.split_done = true;
    if (e <= c && c <= h && h <= p && p <= s) {
        g_perf_gap_copy.Add(TicksToMs(static_cast<double>(c - e)));
        g_perf_gap_hand.Add(TicksToMs(static_cast<double>(h - c)));
        g_perf_gap_present.Add(TicksToMs(static_cast<double>(p - h)));
        g_perf_gap_next.Add(TicksToMs(static_cast<double>(s - p)));
    }
    if (g_cal_at && next.cpu_ms[kStart] > 0.0) {
        const double lag = GpuToCpuMs(s) - next.cpu_ms[kStart];
        if (lag > -50.0 && lag < 1000.0) g_perf_start_lag.Add(std::max(lag, 0.0));
    }
}

void Finish(long long frame) {
    FrameStamps& r = g_ring[frame % kRing];
    if (r.frame != frame) return;
    FrameStamps& prev = g_ring[(frame - 1) % kRing];
    const bool have_prev = prev.frame == frame - 1;
    if (have_prev) Split(prev, r);
    const UINT64 start = r.ts[kStart], end = r.ts[kEnd];
    if (!r.counted && start && end && end >= start) {
        r.counted = true;
        const double gpu_ms = TicksToMs(static_cast<double>(end - start));
        g_perf_frame.Add(gpu_ms);
        g_window.frame_ms += gpu_ms;
        ++g_window.frames;
        if (g_cal_at && r.cpu_ms[kEnd] > 0.0) {
            const double done = GpuToCpuMs(end) - r.cpu_ms[kEnd];
            if (done > -50.0 && done < 1000.0) g_perf_done.Add(std::max(done, 0.0));
        }
    }
    if (!start) return;
    if (have_prev && !r.idle_done && prev.ts[kEnd] && start >= prev.ts[kEnd]) {
        r.idle_done = true;
        const double idle = TicksToMs(static_cast<double>(start - prev.ts[kEnd]));
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
        r.ts[s.kind] = ts;
        r.cpu_ms[s.kind] = s.cpu_ms;
        Finish(s.frame);
        Finish(s.frame + 1);  // its idle and gap need this frame's stamps
    }
}

// Queues one timestamp on the game's queue. g_mutex held.
void Stamp(Kind kind, long long frame) {
    Slot& s = g_slots[g_next];
    if (s.fence && g_fence->GetCompletedValue() < s.fence) {
        ++g_skipped;  // the GPU is more than 12 frames behind: skip rather than wait
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
    s.kind = kind;
    s.frame = frame;
    s.cpu_ms = perf::NowMs();
    g_next = (g_next + 1) % kSlots;
}

void STDMETHODCALLTYPE HookExecute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    if (!t_inside && q == g_queue.load() && g_ready.load(std::memory_order_acquire) &&
        g_want_start.exchange(false)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        Stamp(kStart, g_frame.load() + 1);
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
    std::snprintf(b, sizeof(b), "GPU timer: %lld stamps skipped (the GPU was more than 12 frames behind)", skipped);
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
    for (perf::Counter* c : {&g_perf_gap_copy, &g_perf_gap_hand, &g_perf_gap_present, &g_perf_gap_next, &g_perf_start_lag})
        perf::Register(c);
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
    Stamp(kEnd, ++g_frame);
}

long long Frame() { return g_frame.load(); }

void MarkOwnThread() { t_inside = true; }

namespace {
void StampLocked(Kind kind, long long frame) {
    if (!g_ready.load(std::memory_order_acquire) || frame <= 0) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    Stamp(kind, frame);
}
}  // namespace

void AfterCopy() { StampLocked(kCopied, g_frame.load()); }
void AfterHandOff(long long frame) { StampLocked(kHanded, frame); }

Window TakeWindow() {
    std::lock_guard<std::mutex> lock(g_mutex);
    Window w = g_window;
    g_window = Window{};
    return w;
}

bool On() { return g_ready.load(std::memory_order_acquire); }

void PresentEnd() {
    StampLocked(kPresented, g_frame.load());
    t_inside = false;
    if (g_ready.load(std::memory_order_acquire)) g_want_start.store(true);
}

}  // namespace holevr::vr::gpu_timer
