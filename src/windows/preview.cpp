#include "windows/preview.h"
#include "windows/devices.h"
#include "windows/diagnostics.h"
#include <audioclient.h>
#include <audiopolicy.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace partymic {
namespace {
using Microsoft::WRL::ComPtr;
struct ComScope {
    ComScope(){check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"Preview COM");}
    ~ComScope(){CoUninitialize();}
};
struct Client {
    ComPtr<IAudioClient> audio;
    bool started=false;
    Client(const std::wstring& id,EDataFlow flow){
        bool found=false;
        for(const auto& e:endpoints(flow))if(e.id==id){
            std::wstring name=e.name;std::transform(name.begin(),name.end(),name.begin(),[](wchar_t c){return std::towlower(c);});
            if(flow==eRender && (is_virtual_cable(e.name)||name.find(L"vb-audio")!=std::wstring::npos||name.find(L"steam streaming")!=std::wstring::npos))
                throw std::runtime_error("Preview output must be speakers/headphones, never a virtual cable");
            found=true;
        }
        if(!found)throw std::runtime_error("Preview device unavailable; refresh devices");
        ComPtr<IMMDeviceEnumerator> enumerator;
        check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)),"Preview enumerator");
        ComPtr<IMMDevice> device;check(enumerator->GetDevice(id.c_str(),&device),"Preview device");
        check(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(audio.GetAddressOf())),"Preview activate");
        WAVEFORMATEX format{WAVE_FORMAT_IEEE_FLOAT,2,48000,384000,8,32,0};
        check(audio->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,1000000,0,&format,nullptr),"Preview initialize");
    }
    void start(){check(audio->Start(),"Preview start");started=true;}
    void close(){if(!audio)return;diagnostics::write("preview.stream.release.begin");if(started)audio->Stop();audio->Reset();audio.Reset();started=false;diagnostics::write("preview.stream.release.end");}
    ~Client(){close();}
};
std::wstring error_text(const char* text){
    const int count=MultiByteToWideChar(CP_UTF8,0,text,-1,nullptr,0);
    if(!count)return L"未知音频错误";
    std::wstring out(count,L'\0');MultiByteToWideChar(CP_UTF8,0,text,-1,out.data(),count);out.pop_back();return out;
}
float finite_gain(float x,float max){return std::isfinite(x)?std::clamp(x,0.0f,max):0;}
float limited(float x){
    if(!std::isfinite(x))return 0;
    if(std::abs(x)>0.8f)return std::copysign(0.8f+0.18f*std::tanh((std::abs(x)-0.8f)/0.18f),x);
    return x;
}
std::filesystem::path save_wav(const std::vector<int16_t>& pcm,bool mixed){
    PWSTR desktop=nullptr;
    check(SHGetKnownFolderPath(FOLDERID_Desktop,KF_FLAG_CREATE,nullptr,&desktop),"Recording desktop folder");
    std::filesystem::path folder=std::filesystem::path(desktop)/L"PartyMic录音";CoTaskMemFree(desktop);
    std::filesystem::create_directories(folder);
    SYSTEMTIME time{};GetLocalTime(&time);wchar_t name[128];
    swprintf(name,128,L"%ls-%04u%02u%02u-%02u%02u%02u-%03u-%llu.wav",mixed?L"混音":L"人声",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,time.wMilliseconds,GetTickCount64());
    auto path=folder/name;
    // Write once after capture; no file I/O on the running mixer's audio thread.
    std::vector<char> bytes;bytes.reserve(44+pcm.size()*2);
    auto tag=[&](const char* s){bytes.insert(bytes.end(),s,s+4);};
    auto u16=[&](uint16_t n){bytes.push_back(char(n));bytes.push_back(char(n>>8));};
    auto u32=[&](uint32_t n){for(int i=0;i<4;++i)bytes.push_back(char(n>>(8*i)));};
    tag("RIFF");u32(uint32_t(36+pcm.size()*2));tag("WAVE");tag("fmt ");u32(16);u16(1);u16(2);u32(48000);u32(192000);u16(4);u16(16);tag("data");u32(uint32_t(pcm.size()*2));
    for(auto x:pcm)u16(uint16_t(x));
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)check(HRESULT_FROM_WIN32(GetLastError()),"Create WAV recording");
    DWORD written=0;const BOOL ok=WriteFile(file,bytes.data(),DWORD(bytes.size()),&written,nullptr);const DWORD error=GetLastError();CloseHandle(file);
    if(!ok||written!=bytes.size()){DeleteFileW(path.c_str());if(!ok)check(HRESULT_FROM_WIN32(error),"Save WAV recording");throw std::runtime_error("Incomplete WAV write");}
    return path;
}
std::vector<int16_t> load_wav(const std::filesystem::path& path){
    std::ifstream file(path,std::ios::binary);std::array<unsigned char,44> h{};
    file.read(reinterpret_cast<char*>(h.data()),h.size());
    auto u16=[&](size_t i){return unsigned(h[i])|(unsigned(h[i+1])<<8);};
    auto u32=[&](size_t i){return uint32_t(h[i])|(uint32_t(h[i+1])<<8)|(uint32_t(h[i+2])<<16)|(uint32_t(h[i+3])<<24);};
    if(!file||std::string(reinterpret_cast<char*>(h.data()),4)!="RIFF"||std::string(reinterpret_cast<char*>(h.data()+8),4)!="WAVE"||std::string(reinterpret_cast<char*>(h.data()+36),4)!="data"||u32(16)!=16||u16(20)!=1||u16(22)!=2||u32(24)!=48000||u16(34)!=16)
        throw std::runtime_error("Unsupported test WAV format");
    const auto size=u32(40);
    if(!size||size%4||size>30*48000*4)throw std::runtime_error("Invalid test WAV length");
    std::vector<int16_t> pcm(size/2);file.read(reinterpret_cast<char*>(pcm.data()),size);
    if(!file)throw std::runtime_error("Incomplete WAV recording");
    return pcm;
}
}
AudioPreview::~AudioPreview(){stop();}
void AudioPreview::message(std::wstring text){std::lock_guard lock(mutex_);status_=std::move(text);}
std::wstring AudioPreview::status()const{std::lock_guard lock(mutex_);return status_;}
std::filesystem::path AudioPreview::last_file()const{std::lock_guard lock(mutex_);return last_file_;}
void AudioPreview::stop(){
    const bool was_playing=playing();request_stop();if(worker_.joinable())worker_.join();mode_=0;level_=0;
    if(was_playing)message(L"试听已停止。");
}
void AudioPreview::request_stop() noexcept {
    if(busy()&&!stop_.exchange(true))diagnostics::write("preview.stop.request");
}
void AudioPreview::record(std::wstring id,bool mixed,unsigned seconds){
    if(busy())throw std::runtime_error("Stop current recording/playback first");
    if(seconds<1||seconds>30)throw std::runtime_error("Recording duration must be 1-30 seconds");
    stop();stop_=false;progress_tick_=GetTickCount64();mode_=1;message(L"正在准备录音…");
    diagnostics::write("preview.record.request", "mixed="+std::to_string(mixed)+" seconds="+std::to_string(seconds));
    try{worker_=std::thread(&AudioPreview::capture,this,std::move(id),mixed,seconds);}catch(...){mode_=0;throw;}
}
void AudioPreview::play(std::wstring id){
    if(busy())throw std::runtime_error("Stop current recording/playback first");
    auto file=last_file();if(file.empty())throw std::runtime_error("Record a sample before playback");
    stop();stop_=false;progress_tick_=GetTickCount64();mode_=2;message(L"正在准备试听…");
    diagnostics::write("preview.play.request");
    try{worker_=std::thread(&AudioPreview::playback,this,std::move(id),std::move(file));}catch(...){mode_=0;throw;}
}
void AudioPreview::capture(std::wstring id,bool mixed,unsigned seconds){
    try{
        diagnostics::write("preview.capture.open",L"device="+id);
        ComScope com;Client client(id,eCapture);ComPtr<IAudioCaptureClient> capture;
        check(client.audio->GetService(IID_PPV_ARGS(&capture)),"Preview capture service");
        std::vector<int16_t> pcm;const size_t target=size_t(seconds)*48000*2;pcm.reserve(target);
        client.start();const auto start=GetTickCount64();unsigned previous=0,discontinuities=0;float current_gain=0;
        message(mixed?L"正在录制混音 0 秒…":L"正在录制人声 0 秒…");
        while(!stop_&&pcm.size()<target){
            progress_tick_=GetTickCount64();
            if(GetTickCount64()-start>(seconds+5)*1000ULL)throw std::runtime_error("Recording timed out");
            UINT32 count=0;check(capture->GetNextPacketSize(&count),"Preview packet size");
            if(!count){Sleep(3);continue;}
            BYTE* bytes=nullptr;DWORD flags=0;check(capture->GetBuffer(&bytes,&count,&flags,nullptr,nullptr),"Preview read");
            if(flags&AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)++discontinuities;
            const auto* input=reinterpret_cast<float*>(bytes);float peak=0;
            const float gain=finite_gain(mic_gain_.load(),2);const bool muted=mic_muted_;
            for(size_t f=0;f<count&&pcm.size()<target;++f){
                current_gain+=(gain-current_gain)*0.004158f;
                for(size_t c=0;c<2;++c){
                    float x=(flags&AUDCLNT_BUFFERFLAGS_SILENT)?0:input[2*f+c];
                    if(!mixed)x=muted?0:limited(x*current_gain);
                    if(!std::isfinite(x))x=0;
                    peak=std::max(peak,std::abs(x));pcm.push_back(int16_t(std::lround(std::clamp(x,-1.0f,1.0f)*32767)));
                }
            }
            check(capture->ReleaseBuffer(count),"Preview capture release");level_=peak;
            const auto elapsed=unsigned(pcm.size()/(48000*2));
            if(elapsed!=previous){previous=elapsed;message((mixed?L"正在录制混音 ":L"正在录制人声 ")+std::to_wstring(elapsed)+L" / "+std::to_wstring(seconds)+L" 秒；可调整人声音量或提前停止。");}
        }
        capture.Reset();client.close();
        diagnostics::write("preview.capture.finished", "frames="+std::to_string(pcm.size()/2)+" discontinuities="+std::to_string(discontinuities));
        if(pcm.size()<4800*2){message(L"录音过短，未保存。请重新录制。");}
        else{
            diagnostics::write("preview.save.begin","frames="+std::to_string(pcm.size()/2));
            auto path=save_wav(pcm,mixed);{std::lock_guard lock(mutex_);last_file_=path;}
            diagnostics::write("preview.save.end");
            message(L"已保存 "+std::to_wstring(pcm.size()/(48000*2))+L" 秒录音："+path.filename().wstring()+L"；采集间断 "+std::to_wstring(discontinuities)+L"。点击试听或打开文件夹。");
        }
    }catch(const std::exception& e){diagnostics::write("preview.capture.error",e.what());message(L"录音失败："+error_text(e.what()));}
    diagnostics::write("preview.stopped", "record resources_released=1");
    mode_=0;level_=0;
}
void AudioPreview::playback(std::wstring id,std::filesystem::path file){
    try{
        diagnostics::write("preview.play.open",L"device="+id);
        ComScope com;auto pcm=load_wav(file);Client client(id,eRender);ComPtr<IAudioRenderClient> render;
        check(client.audio->GetService(IID_PPV_ARGS(&render)),"Preview render service");
        ComPtr<ISimpleAudioVolume> volume;check(client.audio->GetService(IID_PPV_ARGS(&volume)),"Preview session volume");
        check(volume->SetMasterVolume(1,nullptr),"Preview own session level");check(volume->SetMute(FALSE,nullptr),"Preview own session mute");
        UINT32 capacity=0;check(client.audio->GetBufferSize(&capacity),"Preview render capacity");
        const size_t frames=pcm.size()/2;size_t sent=0;float current_gain=0;
        const auto write=[&](UINT32 count){
            BYTE* bytes=nullptr;check(render->GetBuffer(count,&bytes),"Preview render buffer");auto* out=reinterpret_cast<float*>(bytes);float peak=0;
            const float gain=finite_gain(playback_gain_.load(),1);
            for(size_t f=0;f<count;++f){current_gain+=(gain-current_gain)*0.004158f;for(size_t c=0;c<2;++c){const float x=pcm[(sent+f)*2+c]/32768.0f*current_gain;out[f*2+c]=x;peak=std::max(peak,std::abs(x));}}
            check(render->ReleaseBuffer(count,0),"Preview render release");sent+=count;level_=peak;
        };
        write(UINT32(std::min(frames,size_t(capacity))));client.start();
        const auto deadline=GetTickCount64()+frames*1000/48000+5000;
        message(L"正在试听："+file.filename().wstring()+L"。麦克风混音已停止，可调节试听音量。");
        while(!stop_){
            progress_tick_=GetTickCount64();
            if(GetTickCount64()>deadline)throw std::runtime_error("Playback timed out");
            UINT32 padding=0;check(client.audio->GetCurrentPadding(&padding),"Preview render padding");
            if(padding>capacity)throw std::runtime_error("Invalid preview padding");
            if(sent==frames&&padding==0)break;
            const auto count=UINT32(std::min(frames-sent,size_t(capacity-padding)));if(count)write(count);Sleep(3);
        }
        message(stop_?L"试听已停止。":L"试听结束。可调整人声音量后重新录制对比；文件也可用系统播放器打开。");
    }catch(const std::exception& e){diagnostics::write("preview.play.error",e.what());message(L"试听失败："+error_text(e.what()));}
    diagnostics::write("preview.stopped", "play resources_released=1");
    mode_=0;level_=0;
}
}
