#include "vr/square_watch.h"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/perf.h"
#include "game/game.h"
#include "vr/render_fixes.h"
#include "vr/square_watch_math.h"

namespace holevr::vr::square_watch {
namespace {

constexpr int kSlots = 3;          // strips in flight on the GPU
constexpr UINT kStripRows = 96;
constexpr int kMaxEvents = 5;      // saved pictures per run
constexpr double kEventGapMs = 10000;
constexpr int kEventScore = 40;    // odd 8 px blocks more in one eye than the other
constexpr int kOddScore = 6;       // counted as an odd strip in the report
constexpr double kWorstGapMs = 5000;

bool g_on = false;

perf::Counter g_perf_copy{"white squares watch: read back a strip (render thread)"};
perf::Counter g_perf_check{"white squares watch: compare the eyes (background)"};

// ------------------------------------------------------ render thread --

struct Slot {
    ID3D12Resource* buffer = nullptr;
    UINT64 size = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT y0 = 0, full_h = 0;
    enum State { kFree, kRecorded, kPending } state = kFree;
    UINT64 fence_value = 0;
};
Slot g_slots[kSlots];
int g_recorded = -1;
ID3D12Fence* g_fence = nullptr;
unsigned g_strip = 0;

bool Supported(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return true;
        default:
            return false;
    }
}
bool IsBgra(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_TYPELESS;
}
bool IsTenBit(DXGI_FORMAT f) { return f == DXGI_FORMAT_R10G10B10A2_UNORM || f == DXGI_FORMAT_R10G10B10A2_TYPELESS; }

// ------------------------------------------------------------ worker --

struct Job {
    std::vector<uint8_t> data;
    UINT row_pitch = 0, width = 0, rows = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT y0 = 0, full_h = 0;
};
std::mutex g_jobs_mutex;
std::condition_variable g_jobs_cv;
std::deque<Job> g_jobs;
std::vector<std::vector<uint8_t>> g_spare;  // strip buffers handed back by the worker, for reuse

// For the 10 s report. Leaf lock.
std::mutex g_stats_mutex;
long long g_checked = 0, g_odd = 0, g_busy = 0;
int g_worst = 0, g_worst_eye = 0, g_worst_y = 0, g_worst_shift = 0;
std::string g_saved;

// Worker thread only.
int g_events = 0;
double g_last_event = -1e12, g_last_worst = -1e12;
int g_run_worst = 0;
int g_burst = 0;  // strips still to add to the event's whole picture
int g_burst_n = 0;
UINT g_burst_w = 0, g_burst_h = 0;  // half size
std::vector<uint8_t> g_burst_rgb;   // half size, top row first

void Rgb(uint32_t px, bool bgra, bool ten, uint8_t out[3]) {
    if (ten) {
        for (int c = 0; c < 3; ++c) out[c] = static_cast<uint8_t>(((px >> (10 * c)) & 0x3ff) >> 2);
    } else {
        const uint8_t c0 = px & 0xff, c1 = (px >> 8) & 0xff, c2 = (px >> 16) & 0xff;
        out[0] = bgra ? c2 : c0;
        out[1] = c1;
        out[2] = bgra ? c0 : c2;
    }
}

// rgb: top row first, 3 bytes a pixel.
bool WriteBmp(const std::string& name, const std::vector<uint8_t>& rgb, UINT w, UINT h) {
    const UINT row = (w * 3 + 3) & ~3u;
    std::vector<uint8_t> out(54 + static_cast<size_t>(row) * h, 0);
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(&out[at], &v, 4); };
    out[0] = 'B';
    out[1] = 'M';
    put32(2, static_cast<uint32_t>(out.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, h);
    out[26] = 1;
    out[28] = 24;
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* src = &rgb[static_cast<size_t>(y) * w * 3];
        uint8_t* dst = &out[54 + static_cast<size_t>(h - 1 - y) * row];
        for (UINT x = 0; x < w; ++x) {
            dst[x * 3 + 0] = src[x * 3 + 2];
            dst[x * 3 + 1] = src[x * 3 + 1];
            dst[x * 3 + 2] = src[x * 3 + 0];
        }
    }
    const std::wstring path = config::Dir() + L"\\" + std::wstring(name.begin(), name.end());
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) {
        log::Warn("square watch: could not write %s", name.c_str());
        return false;
    }
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return true;
}

void Saved(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_stats_mutex);
    if (!g_saved.empty()) g_saved += ", ";
    g_saved += name;
}

void SaveStrip(const Job& j, const std::string& name) {
    const bool bgra = IsBgra(j.format), ten = IsTenBit(j.format);
    std::vector<uint8_t> rgb(static_cast<size_t>(j.width) * j.rows * 3);
    for (UINT y = 0; y < j.rows; ++y) {
        const uint8_t* src = j.data.data() + static_cast<size_t>(y) * j.row_pitch;
        for (UINT x = 0; x < j.width; ++x) {
            uint32_t px;
            std::memcpy(&px, src + x * 4, 4);
            Rgb(px, bgra, ten, &rgb[(static_cast<size_t>(y) * j.width + x) * 3]);
        }
    }
    if (WriteBmp(name, rgb, j.width, j.rows)) Saved(name);
}

void AddToBurst(const Job& j) {
    const bool bgra = IsBgra(j.format), ten = IsTenBit(j.format);
    for (UINT y = 0; y < j.rows; ++y) {
        const UINT fy = j.y0 + y;
        if (fy % 2 || fy / 2 >= g_burst_h) continue;
        const uint8_t* src = j.data.data() + static_cast<size_t>(y) * j.row_pitch;
        uint8_t* dst = &g_burst_rgb[static_cast<size_t>(fy / 2) * g_burst_w * 3];
        for (UINT x = 0; x < g_burst_w && x * 2 < j.width; ++x) {
            uint32_t px;
            std::memcpy(&px, src + x * 2 * 4, 4);
            Rgb(px, bgra, ten, dst + x * 3);
        }
    }
}

void Check(const Job& j) {
    perf::Scope t(g_perf_check);
    const bool bgra = IsBgra(j.format), ten = IsTenBit(j.format);
    static Band band;  // worker thread only; kept to avoid big allocations every frame
    static Scratch scratch;
    band.width = static_cast<int>(j.width);
    band.height = static_cast<int>(j.rows);
    band.luma.resize(static_cast<size_t>(j.width) * j.rows);
    for (UINT y = 0; y < j.rows; ++y) {
        const uint8_t* src = j.data.data() + static_cast<size_t>(y) * j.row_pitch;
        float* dst = &band.luma[static_cast<size_t>(y) * j.width];
        for (UINT x = 0; x < j.width; ++x) {
            uint32_t px;
            std::memcpy(&px, src + x * 4, 4);
            dst[x] = Luma(px, bgra, ten);
        }
    }
    const Odd o = FindOdd(band, Settings{}, &scratch);
    const int score = o.Score(), eye = o.Eye();
    {
        std::lock_guard<std::mutex> lock(g_stats_mutex);
        ++g_checked;
        if (score >= kOddScore) ++g_odd;
        if (score > g_worst) {
            g_worst = score;
            g_worst_eye = eye;
            g_worst_y = static_cast<int>(j.y0);
            g_worst_shift = o.scene_shift;
        }
    }
    const double now = perf::NowMs();

    if (g_burst > 0) {
        AddToBurst(j);
        if (--g_burst == 0) {
            const std::string name = "capture-squares-" + std::to_string(g_burst_n) + ".bmp";
            if (WriteBmp(name, g_burst_rgb, g_burst_w, g_burst_h)) Saved(name);
            g_burst_rgb.clear();
            g_burst_rgb.shrink_to_fit();
        }
    }

    if (score >= kEventScore && g_burst == 0 && g_events < kMaxEvents && now - g_last_event >= kEventGapMs) {
        g_last_event = now;
        g_burst_n = ++g_events;
        log::Info("square watch: %d bright blocks in the %s eye that the other eye doesn't have, %u rows down "
                  "(first at x %d y %d of that eye; eyes %d px apart); saving the picture as capture-squares-%d",
                  score, eye ? "right" : "left", j.y0, o.first_x[eye], o.first_y[eye] + static_cast<int>(j.y0),
                  o.scene_shift, g_burst_n);
        SaveStrip(j, "capture-squares-" + std::to_string(g_burst_n) + "-strip.bmp");
        // The rest of the picture comes from the next strips, a frame each.
        g_burst_w = j.width / 2;
        g_burst_h = j.full_h / 2;
        g_burst_rgb.assign(static_cast<size_t>(g_burst_w) * g_burst_h * 3, 0);
        for (size_t i = 0; i + 2 < g_burst_rgb.size(); i += 3) g_burst_rgb[i] = g_burst_rgb[i + 2] = 255;  // magenta: not seen
        AddToBurst(j);
        g_burst = static_cast<int>((j.full_h + j.rows - 1) / j.rows) + 1;
        return;
    }
    if (score >= kOddScore && score > g_run_worst && now - g_last_worst >= kWorstGapMs) {
        g_run_worst = score;
        g_last_worst = now;
        SaveStrip(j, "capture-squares-worst-strip.bmp");
    }
}

void Worker() {
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lock(g_jobs_mutex);
            g_jobs_cv.wait(lock, [] { return !g_jobs.empty(); });
            j = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        Check(j);
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        if (g_spare.size() < 4) g_spare.push_back(std::move(j.data));
    }
}

// Render thread: a strip the GPU has finished copying goes to the worker.
void Collect(Slot& s) {
    s.state = Slot::kFree;
    const UINT rows = s.fp.Footprint.Height;
    const size_t bytes = static_cast<size_t>(s.fp.Footprint.RowPitch) * rows;
    void* mapped = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
    if (FAILED(s.buffer->Map(0, &range, &mapped)) || !mapped) return;
    Job j;
    {
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        if (!g_spare.empty()) {
            j.data = std::move(g_spare.back());
            g_spare.pop_back();
        }
    }
    j.data.resize(bytes);
    std::memcpy(j.data.data(), mapped, bytes);
    D3D12_RANGE none{0, 0};
    s.buffer->Unmap(0, &none);
    j.row_pitch = s.fp.Footprint.RowPitch;
    j.width = s.fp.Footprint.Width;
    j.rows = rows;
    j.format = s.format;
    j.y0 = s.y0;
    j.full_h = s.full_h;
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        if (g_jobs.size() < 2) {
            g_jobs.push_back(std::move(j));
            queued = true;
        }
    }
    if (queued) {
        g_jobs_cv.notify_one();
    } else {
        std::lock_guard<std::mutex> lock(g_stats_mutex);
        ++g_busy;
    }
}

std::string Report(double) {
    std::lock_guard<std::mutex> lock(g_stats_mutex);
    if (g_checked == 0 && g_saved.empty()) return {};
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "white squares watch: compared the eyes in %lld strips of the picture; %lld had bright blocks in "
                  "one eye only (worst %d blocks, %s eye, %d rows down, eyes %d px apart)%s",
                  g_checked, g_odd, g_worst, g_worst_eye ? "right" : "left", g_worst_y, g_worst_shift,
                  g_busy ? "; some strips skipped (checker busy)" : "");
    std::string out = buf;
    if (!g_saved.empty()) out += "; saved " + g_saved;
    g_checked = g_odd = g_busy = 0;
    g_worst = g_worst_eye = g_worst_y = g_worst_shift = 0;
    g_saved.clear();
    return out;
}

}  // namespace

void Init() {
    g_on = config::GetBool("square_watch", true);
    if (!g_on) {
        log::Info("square watch: off (square_watch=0)");
        return;
    }
    perf::Register(&g_perf_copy);
    perf::Register(&g_perf_check);
    perf::AddReportLine(&Report);
    std::thread(Worker).detach();
    log::Info("square watch: on; compares the two eyes in one strip of each headset picture and saves "
              "capture-squares-*.bmp when one eye has white blocks the other doesn't (square_watch=0 turns it off)");
}

void Record(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* source) {
    if (!g_on || !device || !cmd || !source) return;
    perf::Scope t(g_perf_copy);
    if (g_fence) {
        const UINT64 done = g_fence->GetCompletedValue();
        for (Slot& s : g_slots)
            if (s.state == Slot::kPending && s.fence_value <= done) Collect(s);
    }
    for (Slot& s : g_slots)
        if (s.state == Slot::kRecorded) s.state = Slot::kFree;  // recorded but never submitted
    g_recorded = -1;
    // Only in the game world with no forearm menu up (title screen and menus
    // have flat panels in one eye on purpose).
    if (!game::Player() || render_fixes::PauseBlurOn()) return;

    int idx = -1;
    for (int i = 0; i < kSlots; ++i)
        if (g_slots[i].state == Slot::kFree) {
            idx = i;
            break;
        }
    if (idx < 0) return;
    const D3D12_RESOURCE_DESC sd = source->GetDesc();
    if (sd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || sd.SampleDesc.Count != 1 || !Supported(sd.Format) ||
        sd.Width < 64 || sd.Height < 16)
        return;
    const UINT width = static_cast<UINT>(sd.Width), height = sd.Height;
    const UINT rows = std::min(kStripRows, height);
    const UINT strips = (height + rows - 1) / rows;
    const UINT y0 = std::min((g_strip++ % strips) * rows, height - rows);

    D3D12_RESOURCE_DESC bd = sd;
    bd.Height = rows;
    bd.MipLevels = 1;
    bd.DepthOrArraySize = 1;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    device->GetCopyableFootprints(&bd, 0, 1, 0, &fp, nullptr, nullptr, &total);
    Slot& s = g_slots[idx];
    if (!s.buffer || s.size < total) {
        if (s.buffer) s.buffer->Release();
        s.buffer = nullptr;
        s.size = 0;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = total;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&s.buffer)))) {
            s.buffer = nullptr;
            log::Warn("square watch: could not create a read-back buffer; off");
            g_on = false;
            return;
        }
        s.size = total;
    }
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = s.buffer;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = source;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_BOX box{0, y0, 0, width, y0 + rows, 1};
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    s.fp = fp;
    s.format = sd.Format;
    s.y0 = y0;
    s.full_h = height;
    s.state = Slot::kRecorded;
    g_recorded = idx;
}

void Submitted(ID3D12Fence* fence, UINT64 value) {
    if (g_recorded < 0 || !fence) return;
    g_fence = fence;
    g_slots[g_recorded].state = Slot::kPending;
    g_slots[g_recorded].fence_value = value;
    g_recorded = -1;
}

}  // namespace holevr::vr::square_watch
