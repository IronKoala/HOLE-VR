#include "vr/interact/holster_audio.h"
#include <windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <wrl/client.h>
// MinGW-w64's endpointvolume.h only forward-declares IAudioMeterInformation.
// Same layout and IID as the Windows SDK, so the MinGW build matches the
// clang-cl build this file was written with.
#if defined(__MINGW32__) && !defined(__IAudioMeterInformation_INTERFACE_DEFINED__)
#define __IAudioMeterInformation_INTERFACE_DEFINED__
struct IAudioMeterInformation : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float* peak) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMeteringChannelCount(UINT* count) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelsPeakValues(UINT32 count, float* peaks) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD* mask) = 0;
};
__CRT_UUID_DECL(IAudioMeterInformation, 0xC02216F6, 0x8C67, 0x4B5B, 0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00,
                0x64)
#endif
// MinGW-w64 declares PKEY_Device_FriendlyName but no library defines it.
#if defined(__MINGW32__)
static const PROPERTYKEY kDeviceFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
#else
#define kDeviceFriendlyName PKEY_Device_FriendlyName
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>
#include "core/log.h"
#include "vr/interact/holster_audio_data.h"
#include "vr/interact/holster_feedback.h"

namespace holevr::vr::holster_audio {
namespace {
using Microsoft::WRL::ComPtr;
using interact::HolsterCue;
constexpr std::array<HolsterCue, 4> kCues{HolsterCue::kHoodOpen, HolsterCue::kHoodClose, HolsterCue::kDraw, HolsterCue::kHolster};
constexpr std::array<const wchar_t*, 4> kFiles{L"hood_open.wav", L"hood_close.wav", L"draw.wav", L"holster.wav"};
int Index(HolsterCue cue) {
    for (int i = 0; i < 4; ++i) if (kCues[i] == cue) return i;
    return -1;
}
struct Event { HolsterCue cue{}; ULONGLONG time = 0; uint64_t generation = 0; };
struct State {
    bool enabled = false;
    Stereo gain;
    ULONGLONG time = 0;
    uint64_t generation = 0;
};
struct Output { std::wstring id, name; };
struct Shared {
    Output output;
    std::mutex mutex;
    std::condition_variable wake;
    State state;
    std::array<Event, 16> queue{};
    size_t count = 0;
};
// Kept alive until the process exits, like the mod's existing background loop.
// No COM/audio cleanup runs under the DLL loader lock.
Shared* g_shared = nullptr;

std::wstring FriendlyName(IMMDevice* device) {
    ComPtr<IPropertyStore> properties;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &properties))) return {};
    PROPVARIANT name{};
    std::wstring result;
    if (SUCCEEDED(properties->GetValue(kDeviceFriendlyName, &name)) && name.vt == VT_LPWSTR && name.pwszVal)
        result = name.pwszVal;
    PropVariantClear(&name);
    return result;
}

// A private WASAPI session keeps our stream distinguishable from Unreal's.
// Otherwise the routing scan can mistake its own playback for game audio and
// stay on the old speakers after the game moves to a headset.
constexpr GUID kHolsterSession{0xdc74f59a,0xf8b3,0x4a20,{0x9b,0x32,0x62,0xb2,0xa5,0x77,0xe4,0x18}};

Output GameEndpoint(const std::wstring& current = {}, DWORD process = GetCurrentProcessId()) {
    ComPtr<IMMDeviceEnumerator> devices;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&devices)))) return {};
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(devices->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) return {};
    UINT count = 0;
    if (FAILED(collection->GetCount(&count))) return {};
    Output best;
    int best_score = -1;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        ComPtr<IAudioSessionManager2> manager;
        ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED(collection->Item(i, &device)) ||
            FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(manager.GetAddressOf()))) ||
            FAILED(manager->GetSessionEnumerator(&sessions))) continue;
        LPWSTR raw_id = nullptr;
        if (FAILED(device->GetId(&raw_id)) || !raw_id) continue;
        const std::wstring id(raw_id);
        CoTaskMemFree(raw_id);
        int n = 0;
        if (FAILED(sessions->GetCount(&n))) continue;
        for (int j = 0; j < n; ++j) {
            ComPtr<IAudioSessionControl> control;
            ComPtr<IAudioSessionControl2> control2;
            DWORD pid = 0;
            GUID group{};
            AudioSessionState state = AudioSessionStateExpired;
            if (FAILED(sessions->GetSession(j, &control)) || FAILED(control.As(&control2)) ||
                control2->GetProcessId(&pid) != S_OK || pid != process ||
                FAILED(control->GetState(&state)) || state != AudioSessionStateActive ||
                FAILED(control->GetGroupingParam(&group)) || IsEqualGUID(group,kHolsterSession)) continue;
            ComPtr<IAudioMeterInformation> meter;
            float peak = 0;
            if (SUCCEEDED(control.As(&meter))) meter->GetPeakValue(&peak);
            // Prefer actual game sound if multiple streams are active; keep
            // a stable route through silent frames instead of device order.
            const int score = (peak > .0001f ? 2 : 0) + (id == current ? 1 : 0);
            if (score > best_score) { best = {id,FriendlyName(device.Get())}; best_score = score; }
        }
    }
    return best; // No Windows-default or named-device fallback.
}

void RouteWorker(Shared* shared) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) { log::Warn("holster audio: cannot inspect the game's output"); return; }
    struct ComScope { ~ComScope() { CoUninitialize(); } } com_scope;
    try {
        std::wstring current;
        for (;;) {
            auto output = GameEndpoint(current);
            current = output.id;
            { std::lock_guard<std::mutex> lock(shared->mutex); shared->output = std::move(output); }
            shared->wake.notify_one();
            // Enumeration stays off both the game thread and playback thread.
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    } catch (...) { log::Warn("holster audio: output tracking stopped"); }
}

struct Mixer {
    struct Slot { const Clip* clip = nullptr; size_t sample = 0; int cue = -1; };
    std::array<Slot,8> slots{};
    Stereo gain;
    void Silence() { for (auto& slot : slots) slot = {}; }
    void Play(int cue, const Clip& clip) {
        if (clip.pcm.empty()) return;
        if (cue < 2) for (auto& slot : slots) if (slot.cue >= 0 && slot.cue < 2) slot = {};
        for (auto& slot : slots) if (!slot.clip) { slot = {&clip,0,cue}; return; }
    }
    void Render(float* stereo, UINT32 frames) {
        std::fill_n(stereo,size_t(frames)*2,0.f);
        for (auto& slot : slots) {
            if (!slot.clip) continue;
            const auto& pcm = slot.clip->pcm;
            const size_t total = pcm.size()/2;
            const auto n = std::min<size_t>(frames,total-slot.sample);
            for (size_t i=0;i<n;++i) {
                const size_t offset = (slot.sample+i)*2;
                const int word = int(pcm[offset]) | (int(pcm[offset+1])<<8);
                const float sample = (word >= 32768 ? word-65536 : word)/32768.f;
                stereo[2*i] += sample*gain.left;
                stereo[2*i+1] += sample*gain.right;
            }
            slot.sample += n;
            if (slot.sample == total) slot = {};
        }
        for (size_t i=0;i<size_t(frames)*2;++i) stereo[i] = std::clamp(stereo[i],-1.f,1.f);
    }
};

struct Engine {
    ComPtr<IAudioClient> audio;
    ComPtr<IAudioRenderClient> render;
    Mixer mixer;
    std::wstring endpoint;
    UINT32 capacity = 0;
    bool failed = false, started = false;
    void Reset() {
        if (audio && started) audio->Stop();
        render.Reset(); audio.Reset(); mixer.Silence();
        endpoint.clear(); capacity=0; failed=started=false;
    }
    ~Engine() { Reset(); }
    bool Open(const Output& output) {
        Reset();
        if (output.id.empty()) return false;
        ComPtr<IMMDeviceEnumerator> devices;
        ComPtr<IMMDevice> device;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&devices))) ||
            FAILED(devices->GetDevice(output.id.c_str(),&device)) ||
            FAILED(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,
                                    reinterpret_cast<void**>(audio.GetAddressOf())))) { Reset(); return false; }
        WAVEFORMATEX format{};
        format.wFormatTag=WAVE_FORMAT_IEEE_FLOAT; format.nChannels=2; format.nSamplesPerSec=44100;
        format.wBitsPerSample=32; format.nBlockAlign=8; format.nAvgBytesPerSec=352800;
        const DWORD flags=AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        ComPtr<IAudioSessionControl> session;
        if (FAILED(audio->Initialize(AUDCLNT_SHAREMODE_SHARED,flags,300000,0,&format,&kHolsterSession)) ||
            FAILED(audio->GetBufferSize(&capacity)) || !capacity ||
            FAILED(audio->GetService(IID_PPV_ARGS(&render))) ||
            FAILED(audio->GetService(IID_PPV_ARGS(&session))) ||
            FAILED(session->SetGroupingParam(&kHolsterSession,nullptr))) { Reset(); return false; }
        endpoint=output.id;
        log::Info("holster audio: following HOLE output: %ls; positional stereo, eight voices",output.name.c_str());
        return true;
    }
    void Silence() {
        mixer.Silence();
        if (audio && started) {
            if (FAILED(audio->Stop()) || FAILED(audio->Reset())) failed=true;
            started=false;
        }
    }
    void Position(Stereo gain) { mixer.gain=gain; }
    void Play(int cue,const Clip& clip) { mixer.Play(cue,clip); }
    void Pump() {
        if (!audio || failed) return;
        UINT32 padding=0;
        if (FAILED(audio->GetCurrentPadding(&padding)) || padding>capacity) { failed=true; return; }
        const UINT32 frames=capacity-padding;
        if (frames) {
            BYTE* data=nullptr;
            if (FAILED(render->GetBuffer(frames,&data)) || !data) { failed=true; return; }
            mixer.Render(reinterpret_cast<float*>(data),frames);
            if (FAILED(render->ReleaseBuffer(frames,0))) { failed=true; return; }
        }
        if (!started) {
            if (FAILED(audio->Start())) { failed=true; return; }
            started=true;
        }
    }
};

void Worker(Shared* shared, std::wstring dir) {
    try {
        std::array<Clip, 4> clips;
        int loaded = 0;
        for (size_t i = 0; i < clips.size(); ++i) {
            const auto file = std::filesystem::path(dir) / L"sounds" / L"holster" / kFiles[i];
            std::ifstream in(file, std::ios::binary | std::ios::ate);
            const auto size = in ? in.tellg() : std::streampos(-1);
            std::string error;
            if (size > 0 && size <= 2*1024*1024) {
                std::vector<uint8_t> bytes(static_cast<size_t>(size));
                in.seekg(0);
                if (in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())) &&
                    ReadWave(bytes.data(), bytes.size(), clips[i], error)) {
                    ++loaded;
                    log::Info("holster audio: loaded %s (%.3f s)", interact::HolsterCueName(kCues[i]), clips[i].pcm.size()/88200.0);
                    continue;
                }
            }
            log::Warn("holster audio: %s unavailable (%s); this cue stays silent", interact::HolsterCueName(kCues[i]), error.empty() ? "missing/unreadable file" : error.c_str());
        }
        if (!loaded) return;
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(com)) { log::Warn("holster audio: COM initialization failed"); return; }
        struct ComScope { ~ComScope() { CoUninitialize(); } } com_scope;
        std::thread(RouteWorker,shared).detach();
        Engine engine;
        uint64_t generation = 0;
        ULONGLONG retry_at = 0;
        bool playing_allowed = false, warned = false;
        for (;;) {
            State state;
            Output output;
            std::array<Event, 16> events{};
            size_t count;
            {
                std::unique_lock<std::mutex> lock(shared->mutex);
                shared->wake.wait_for(lock, std::chrono::milliseconds(5), [&] { return shared->count != 0; });
                state = shared->state;
                output = shared->output;
                count = shared->count;
                std::copy_n(shared->queue.begin(), count, events.begin());
                shared->count = 0;
            }
            const auto now = GetTickCount64();
            const bool enabled = state.enabled && now - state.time < 300;
            if (engine.failed) {
                log::Warn("holster audio: output lost; rebuilding playback when available");
                engine.Reset();
                retry_at = now + 1000;
            }
            if (generation != state.generation || (!enabled && playing_allowed)) engine.Silence();
            generation = state.generation;
            playing_allowed = enabled;
            if (engine.audio && engine.endpoint != output.id) {
                engine.Reset(); retry_at=0;
            }
            if (output.id.empty()) {
                if (enabled && !warned) {
                    log::Info("holster audio: waiting for HOLE's active audio output");
                    warned=true;
                }
                continue;
            }
            if (!engine.audio && now >= retry_at) {
                if (!engine.Open(output)) {
                    if (!warned) log::Warn("holster audio: game output unavailable; staying silent and retrying");
                    warned = true;
                    retry_at = now + 1000;
                } else warned = false;
            }
            // Pre-open once the game's stream appears, so the first reach
            // never waits for device creation. No playback starts until enabled.
            if (!enabled || !engine.audio) continue;
            // Device creation can take longer than a tick. Recheck after it
            // so a menu/player change cannot replay an old queued action.
            State latest;
            { std::lock_guard<std::mutex> lock(shared->mutex); latest = shared->state; }
            if (!latest.enabled || latest.generation != generation || GetTickCount64() - latest.time >= 300) {
                engine.Silence();
                continue;
            }
            engine.Position(latest.gain);
            for (size_t i = 0; i < count; ++i) {
                const auto& event = events[i];
                const int index = Index(event.cue);
                if (index >= 0 && event.generation == generation && GetTickCount64() - event.time < 200)
                    engine.Play(index, clips[index]);
            }
            engine.Pump();
        }
    } catch (const std::exception& e) {
        log::Warn("holster audio: disabled after playback initialization error: %s", e.what());
    } catch (...) {
        log::Warn("holster audio: disabled after unexpected playback error");
    }
}

void Cue(HolsterCue cue, uintptr_t, const FVector&) {
    if (!g_shared || Index(cue) < 0) return;
    {
        std::lock_guard<std::mutex> lock(g_shared->mutex);
        if (!g_shared->state.enabled || g_shared->count == g_shared->queue.size()) return;
        g_shared->queue[g_shared->count++] = {cue, GetTickCount64(), g_shared->state.generation};
    }
    g_shared->wake.notify_one();
}
}  // namespace

void Init(const std::wstring& mod_dir) {
    if (g_shared) return;
    g_shared = new Shared;
    try { std::thread(Worker, g_shared, mod_dir).detach(); }
    catch (...) { log::Warn("holster audio: could not start playback worker"); return; }
    interact::SetHolsterCueSink(&Cue);
}
void Update(bool enabled, const FVector& source, const FVector& head, const FQuat& rotation, float volume) {
    if (!g_shared) return;
    const auto gain = SpatialGain(source, head, rotation, volume);
    if (!enabled || (gain.left <= 0 && gain.right <= 0)) { Silence(); return; }
    std::lock_guard<std::mutex> lock(g_shared->mutex);
    g_shared->state.enabled = true;
    g_shared->state.gain = gain;
    g_shared->state.time = GetTickCount64();
}
void Silence() {
    if (!g_shared) return;
    std::lock_guard<std::mutex> lock(g_shared->mutex);
    if (g_shared->state.enabled) ++g_shared->state.generation;
    g_shared->state.enabled = false;
    g_shared->count = 0;
}
}  // namespace holevr::vr::holster_audio
