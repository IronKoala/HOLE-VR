// Windows integration test: all device playback is silent. Synthetic game
// sessions verify output following without changing any Windows defaults.
#include "../src/vr/interact/holster_audio.cpp"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
namespace holevr::log {
void Info(const char* fmt, ...) {
    va_list args; va_start(args,fmt); std::vprintf(fmt,args); va_end(args); std::puts("");
}
void Warn(const char* fmt, ...) {
    va_list args; va_start(args,fmt); std::vfprintf(stderr,fmt,args); va_end(args); std::fputs("\n",stderr);
}
}
using namespace holevr::vr::holster_audio;
void Check(bool okay,const char* why) {
    if(!okay){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}
}
std::vector<Output> Outputs() {
    ComPtr<IMMDeviceEnumerator> devices;
    Check(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&devices))),"enumerator");
    ComPtr<IMMDeviceCollection> collection;
    Check(SUCCEEDED(devices->EnumAudioEndpoints(eRender,DEVICE_STATE_ACTIVE,&collection)),"outputs");
    UINT n=0;collection->GetCount(&n);std::vector<Output> result;
    for(UINT i=0;i<n;++i) {
        ComPtr<IMMDevice> device;collection->Item(i,&device);LPWSTR id=nullptr;device->GetId(&id);
        if(id){result.push_back({id,FriendlyName(device.Get())});CoTaskMemFree(id);}
    }
    return result;
}
struct FakeGame {
    ComPtr<IAudioClient> audio;
    void Open(const Output& output) {
        Close();
        constexpr GUID session{0x146ce37f,0xa621,0x42e4,{0x8a,0xf9,0x8f,0x45,0xb4,0x91,0x02,0x73}};
        ComPtr<IMMDeviceEnumerator> devices;ComPtr<IMMDevice> device;
        Check(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&devices))),"game enumerator");
        Check(SUCCEEDED(devices->GetDevice(output.id.c_str(),&device)),"game endpoint");
        Check(SUCCEEDED(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(audio.GetAddressOf()))),"game client");
        WAVEFORMATEX f{};f.wFormatTag=WAVE_FORMAT_IEEE_FLOAT;f.nChannels=2;f.nSamplesPerSec=44100;
        f.wBitsPerSample=32;f.nBlockAlign=8;f.nAvgBytesPerSec=352800;
        Check(SUCCEEDED(audio->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,300000,0,&f,&session)),"game format");
        ComPtr<IAudioSessionControl> control;
        Check(SUCCEEDED(audio->GetService(IID_PPV_ARGS(&control))),"game session");
        Check(SUCCEEDED(control->SetGroupingParam(&session,nullptr)),"game session tag");
        ComPtr<IAudioRenderClient> render;
        Check(SUCCEEDED(audio->GetService(IID_PPV_ARGS(&render))),"game render");
        UINT32 frames=0;audio->GetBufferSize(&frames);BYTE* data=nullptr;
        Check(SUCCEEDED(render->GetBuffer(frames,&data)),"game buffer");
        Check(SUCCEEDED(render->ReleaseBuffer(frames,AUDCLNT_BUFFERFLAGS_SILENT)),"game silent buffer");
        Check(SUCCEEDED(audio->Start()),"game started");
    }
    void Close(){if(audio)audio->Stop();audio.Reset();}
    ~FakeGame(){Close();}
};
void Mixing() {
    Clip positive{{0,64,0,64}},negative{{0,192,0,192}};
    Mixer m;m.gain={1,.5f};float out[8]{};
    m.Play(2,positive);m.Play(3,negative);m.Render(out,1);
    Check(out[0]==0 && out[1]==0,"signed PCM overlap cancels");
    m.Silence();m.Play(0,positive);m.Play(1,negative);m.Render(out,1);
    Check(out[0]==-.5f && out[1]==-.25f,"hood reversal replaces only mechanism");
    m.Render(out,4);
    Check(out[0]==-.5f && out[2]==0 && out[6]==0,"one-shot zero-fills tail");
    for(auto& slot:m.slots)Check(!slot.clip,"finished voice released");
    m.Play(2,positive);m.Play(3,positive);m.Play(2,positive);m.Render(out,1);
    Check(out[0]==1 && out[1]==.75f,"stereo mixing clamps peaks");
    m.Silence();m.Render(out,1);Check(out[0]==0 && out[1]==0,"cancel silent");
}
int main(int argc,char** argv) {
    Check(argc==2,"supply WAV directory");
    Check(SUCCEEDED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)),"COM ready");
    {
        Mixing();std::array<Clip,4> clips;
        for(size_t i=0;i<4;++i) {
            std::ifstream in(std::filesystem::path(argv[1])/kFiles[i],std::ios::binary);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),{});std::string error;
            Check(ReadWave(bytes.data(),bytes.size(),clips[i],error),"actual clip loads");
        }
        const auto outputs=Outputs();Check(outputs.size()>=2,"two outputs for migration test");
        Engine engine;Check(!engine.Open({}),"no default fallback");
        Check(GameEndpoint().id.empty(),"no game means no guessed output");
        FakeGame game;game.Open(outputs[0]);
        Check(GameEndpoint().id==outputs[0].id,"find game output");
        Check(engine.Open(GameEndpoint()),"open game endpoint");engine.Position({0,0});
        engine.Play(0,clips[0]);engine.Play(2,clips[2]);engine.Play(3,clips[3]);
        int active=0;for(auto& slot:engine.mixer.slots)if(slot.clip)++active;
        Check(active==3,"mechanism draw and stow overlap");engine.Play(1,clips[1]);
        for(auto& slot:engine.mixer.slots)Check(slot.cue!=0,"close cancels open");
        const auto until=GetTickCount64()+1250;
        while(GetTickCount64()<until){engine.Pump();Sleep(5);}
        Check(!engine.failed,"four clips render without errors");
        for(auto& slot:engine.mixer.slots)Check(!slot.clip,"one-shots finish");
        game.Open(outputs[1]);
        Check(GameEndpoint(engine.endpoint).id==outputs[1].id,"follow migration despite own old stream");
        Check(engine.Open(GameEndpoint(engine.endpoint)),"rebuild on migrated output");
        engine.Position({0,0});engine.Play(3,clips[3]);engine.Pump();engine.Silence();
        Check(!engine.started && !engine.failed,"pause clears endpoint buffer");
        engine.Play(0,clips[0]);engine.Pump();Check(engine.started && !engine.failed,"resume restarts");
        game.Close();Check(GameEndpoint(engine.endpoint).id.empty(),"own stream cannot impersonate game");
        engine.Reset();
    }
    CoUninitialize();
    std::puts("Silent device test passed: mixer, four clips, overlap, reversal, completion, pause/resume, automatic output migration, own-session exclusion, no default fallback.");
}
