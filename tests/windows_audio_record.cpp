// Explicitly requested, local listening test. Writes PCM WAV only with --record.
#include "windows/audio_engine.h"
#include "windows/devices.h"
#include <audioclient.h>
#include <endpointvolume.h>
#include <audiopolicy.h>
#include <fcntl.h>
#include <io.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace partymic;
using Microsoft::WRL::ComPtr;
namespace {
void need(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
struct Com { Com(){check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM");} ~Com(){CoUninitialize();} };
void physical_playback() {
    for (auto role : {eConsole,eMultimedia}) {
        const auto id=default_endpoint_id(eRender,role);
        bool physical=false;
        for(const auto& e:endpoints(eRender)) if(e.id==id) physical=!is_virtual_cable(e.name);
        need(physical,"Playback requires a physical default output");
    }
}
std::vector<char> wav(const std::vector<int16_t>& pcm) {
    std::vector<char> b;
    const auto tag=[&](const char* s){b.insert(b.end(),s,s+4);};
    const auto u16=[&](uint16_t v){b.push_back(char(v));b.push_back(char(v>>8));};
    const auto u32=[&](uint32_t v){for(int n=0;n<4;++n)b.push_back(char(v>>(8*n)));};
    tag("RIFF");u32(uint32_t(36+pcm.size()*2));tag("WAVE");tag("fmt ");u32(16);
    u16(1);u16(2);u32(48000);u32(192000);u16(4);u16(16);tag("data");u32(uint32_t(pcm.size()*2));
    for(const auto s:pcm)u16(uint16_t(s));
    return b;
}
struct Recorder {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    unsigned discontinuities=0;
    explicit Recorder(const std::wstring& id){
        ComPtr<IMMDeviceEnumerator> en;
        check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&en)),"Enumerator");
        ComPtr<IMMDevice> dev;check(en->GetDevice(id.c_str(),&dev),"Cable capture device");
        check(dev->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(client.GetAddressOf())),"Capture activation");
        WAVEFORMATEX f{WAVE_FORMAT_IEEE_FLOAT,2,48000,384000,8,32,0};
        check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,1000000,0,&f,nullptr),"Capture initialize");
        check(client->GetService(IID_PPV_ARGS(&capture)),"Capture service");
        check(client->Start(),"Capture start");
    }
    ~Recorder(){client->Stop();}
    void read(std::vector<int16_t>* pcm,size_t max_samples){
        for(unsigned n=0;n<128;++n){
            UINT32 count=0;check(capture->GetNextPacketSize(&count),"Packet size");if(!count)break;
            BYTE* data=nullptr;DWORD flags=0;
            check(capture->GetBuffer(&data,&count,&flags,nullptr,nullptr),"Capture buffer");
            if(flags&AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)++discontinuities;
            if(pcm){
                const auto* f=reinterpret_cast<const float*>(data);
                for(size_t i=0;i<size_t(count)*2 && pcm->size()<max_samples;++i){
                    float x=(flags&AUDCLNT_BUFFERFLAGS_SILENT)?0:f[i];
                    if(!std::isfinite(x))x=0;
                    pcm->push_back(int16_t(std::lround(std::clamp(x,-1.0f,1.0f)*32767)));
                }
            }
            check(capture->ReleaseBuffer(count),"Capture release");
        }
    }
};
void devices(){
    _setmode(_fileno(stdout),_O_U8TEXT);
    ComPtr<IMMDeviceEnumerator> en;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&en)),"Enumerator");
    std::wcout<<L"console_default="<<default_endpoint_id(eRender,eConsole)<<L"\nmultimedia_default="<<default_endpoint_id(eRender,eMultimedia)<<std::endl;
    for(const auto& e:endpoints(eRender)){
        std::wcout<<L"ENDPOINT "<<e.name<<L" | "<<e.id<<std::endl;
        ComPtr<IMMDevice> dev;check(en->GetDevice(e.id.c_str(),&dev),"Device");
        ComPtr<IAudioEndpointVolume> volume;
        if(SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(volume.GetAddressOf())))){
            float level=0;BOOL mute=FALSE;volume->GetMasterVolumeLevelScalar(&level);volume->GetMute(&mute);
            std::wcout<<L" master="<<level<<L" mute="<<mute<<std::endl;
        }
        ComPtr<IAudioSessionManager2> manager;
        if(FAILED(dev->Activate(__uuidof(IAudioSessionManager2),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(manager.GetAddressOf()))))continue;
        ComPtr<IAudioSessionEnumerator> sessions;if(FAILED(manager->GetSessionEnumerator(&sessions)))continue;
        int count=0;sessions->GetCount(&count);
        for(int i=0;i<count;++i){
            ComPtr<IAudioSessionControl> control;ComPtr<IAudioSessionControl2> detail;ComPtr<ISimpleAudioVolume> v;
            if(FAILED(sessions->GetSession(i,&control))||FAILED(control.As(&detail))||FAILED(control.As(&v)))continue;
            DWORD pid=0;AudioSessionState state{};float level=0;BOOL mute=FALSE;
            detail->GetProcessId(&pid);control->GetState(&state);v->GetMasterVolume(&level);v->GetMute(&mute);
            std::wcout<<L" session_pid="<<pid<<L" state="<<state<<L" volume="<<level<<L" mute="<<mute<<L" system_sounds="<<(detail->IsSystemSoundsSession()==S_OK)<<std::endl;
        }
    }
}
void render_samples(const std::vector<int16_t>& samples,float gain){
    physical_playback();
    std::wstring id;
    for(const auto& e:endpoints(eRender))if(e.name.find(L"Mi Monitor")!=std::wstring::npos)id=e.id;
    need(!id.empty(),"HDMI Mi Monitor output unavailable");
    ComPtr<IMMDeviceEnumerator> en;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&en)),"Enumerator");
    ComPtr<IMMDevice> dev;check(en->GetDevice(id.c_str(),&dev),"HDMI playback device");
    ComPtr<IAudioClient> client;
    check(dev->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(client.GetAddressOf())),"Render activation");
    WAVEFORMATEX f{WAVE_FORMAT_IEEE_FLOAT,2,48000,384000,8,32,0};
    check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,1000000,0,&f,nullptr),"Render initialize");
    ComPtr<IAudioRenderClient> render;check(client->GetService(IID_PPV_ARGS(&render)),"Render service");
    ComPtr<ISimpleAudioVolume> volume;check(client->GetService(IID_PPV_ARGS(&volume)),"Own playback session volume");
    check(volume->SetMasterVolume(1,nullptr),"Own session volume");check(volume->SetMute(FALSE,nullptr),"Own session unmute");
    UINT32 capacity=0;check(client->GetBufferSize(&capacity),"Render capacity");
    const auto send=[&](size_t offset,UINT32 frames){
        BYTE* bytes=nullptr;check(render->GetBuffer(frames,&bytes),"Render buffer");auto* out=reinterpret_cast<float*>(bytes);
        for(size_t i=0;i<size_t(frames)*2;++i)out[i]=std::clamp(samples[offset*2+i]/32768.0f*gain,-0.35f,0.35f);
        check(render->ReleaseBuffer(frames,0),"Render release");
    };
    const size_t frames=samples.size()/2;size_t sent=std::min(size_t(capacity),frames);send(0,UINT32(sent));
    check(client->Start(),"HDMI render start");
    try{
        const auto deadline=GetTickCount64()+frames*1000/48000+10000;
        for(;;){
            need(GetTickCount64()<deadline,"HDMI playback timed out");
            UINT32 padding=0;check(client->GetCurrentPadding(&padding),"Render padding");
            if(sent==frames&&padding==0)break;
            const auto count=UINT32(std::min(frames-sent,size_t(capacity-padding)));
            if(count){send(sent,count);sent+=count;}
            Sleep(3);
        }
        check(client->Stop(),"HDMI render stop");
    }catch(...){client->Stop();throw;}
    std::cout<<"HDMI PLAYBACK frames="<<frames<<" gain="<<gain<<" peak_limit=0.35"<<std::endl;
}
void cue(){
    std::vector<int16_t> samples(48000*2);
    for(size_t i=0;i<48000;++i){
        const double time=double(i)/48000;
        if(time<0.35||(time>=0.5&&time<0.85)){
            const double local=time<0.35?time:time-0.5;
            const double envelope=std::min({1.0,local/0.01,(0.35-local)/0.01});
            const auto value=int16_t(4915*envelope*std::sin(2*3.141592653589793*(time<0.35?660:880)*time));
            samples[2*i]=samples[2*i+1]=value;
        }
    }
    render_samples(samples,1);
}
void play_file(const std::filesystem::path& path){
    std::ifstream file(path,std::ios::binary);need(bool(file),"Recording open failed");
    char header[44]{};file.read(header,44);
    need(file.gcount()==44&&std::string(header,4)=="RIFF"&&std::string(header+8,4)=="WAVE"&&std::string(header+36,4)=="data"&&header[20]==1&&header[21]==0&&header[22]==2&&header[23]==0&&uint8_t(header[24])==0x80&&uint8_t(header[25])==0xbb&&header[26]==0&&header[27]==0&&header[34]==16,"Expected test PCM WAV");
    const uint32_t size=uint8_t(header[40])|(uint32_t(uint8_t(header[41]))<<8)|(uint32_t(uint8_t(header[42]))<<16)|(uint32_t(uint8_t(header[43]))<<24);
    need(size>0&&size<=48000*4*60&&size%4==0,"Unexpected WAV size");
    std::vector<int16_t> samples(size/2);file.read(reinterpret_cast<char*>(samples.data()),size);need(bool(file),"Truncated WAV");
    float peak=0;for(const auto x:samples)peak=std::max(peak,std::abs(x/32768.0f));
    const float gain=peak>0?std::min(4.0f,0.35f/peak):1;
    render_samples(samples,gain);
}
void record(const std::filesystem::path& output){
    need(!std::filesystem::exists(output),"Refusing to overwrite existing recording");
    physical_playback();
    AudioSelection selection;
    for(const auto& a:audio_applications())if(_wcsicmp(a.executable.c_str(),L"cloudmusic.exe")==0){selection.music_pid=a.pid;selection.music_executable=a.executable;break;}
    need(selection.music_pid!=0,"NetEase audio session unavailable");
    std::wstring cable;
    for(const auto& e:endpoints(eCapture)){
        if(e.name.find(L"Brio 90")!=std::wstring::npos)selection.mic_id=e.id;
        if(e.name.find(L"CABLE Output")!=std::wstring::npos)cable=e.id;
    }
    for(const auto& e:endpoints(eRender))if(is_virtual_cable(e.name)){selection.output_id=e.id;break;}
    need(!selection.mic_id.empty()&&!cable.empty()&&!selection.output_id.empty(),"Brio / stereo cable unavailable");
    std::vector<int16_t> pcm;pcm.reserve(20*48000*2);
    AudioEngine engine;MixSettings settings;settings.mic_muted=true;engine.settings(settings);
    Recorder reader(cable);engine.start(selection);
    const auto deadline=GetTickCount64()+10000;
    while(engine.active()&&engine.status().find(L"CABLE Output")==std::wstring::npos&&GetTickCount64()<deadline)Sleep(10);
    need(engine.active()&&engine.status().find(L"CABLE Output")!=std::wstring::npos,"Mixer startup failed");
    cue();
    const auto settle=GetTickCount64()+250;
    while(GetTickCount64()<settle){reader.read(nullptr,0);Sleep(3);}
    reader.read(nullptr,0);reader.discontinuities=0;
    std::cout<<"RECORDING START: 0-4s music; 4-16s music + voice with ducking; 16-20s voice"<<std::endl;
    const auto start=GetTickCount64();unsigned reported=0;
    while(pcm.size()<20*48000*2){
        need(engine.active(),"Mixer stopped during recording");
        need(GetTickCount64()-start<25000,"Recording timed out");
        const auto sec=pcm.size()/(48000*2);
        settings.mic_muted=sec<4;settings.music_muted=sec>=16;engine.settings(settings);
        reader.read(&pcm,20*48000*2);
        if(pcm.size()/(48000*2)>reported){reported=unsigned(pcm.size()/(48000*2));
            std::cout<<"second="<<reported<<" music="<<engine.music_level()<<" mic="<<engine.mic_level()<<" duck_gain="<<engine.duck_gain()<<std::endl;}
        Sleep(3);
    }
    engine.stop();
    auto bytes=wav(pcm);std::ofstream stream(output,std::ios::binary);stream.write(bytes.data(),std::streamsize(bytes.size()));stream.close();
    need(bool(stream),"WAV write failed");
    std::cout<<"RECORDING SAVED frames="<<pcm.size()/2<<" discontinuities="<<reader.discontinuities
        <<" dropped="<<engine.dropped_frames()<<" music_starved="<<engine.music_starved_frames()<<" mic_starved="<<engine.mic_starved_frames()<<std::endl;
}
}
int wmain(int argc,wchar_t** argv){
    try{Com com;
        if(argc==2&&std::wstring(argv[1])==L"--devices"){devices();return 0;}
        if(argc==2&&std::wstring(argv[1])==L"--cue"){cue();return 0;}
        if(argc==3&&std::wstring(argv[1])==L"--record"){record(argv[2]);return 0;}
        if(argc==3&&std::wstring(argv[1])==L"--play"){
            play_file(argv[2]);
            std::cout<<"PLAYBACK COMPLETED"<<std::endl;return 0;
        }
        throw std::runtime_error("Usage: windows_audio_record --record|--play file.wav");
    }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}
}
