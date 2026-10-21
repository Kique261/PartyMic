// Manual hardware diagnostic. Reads Brio through legacy/communications APIs.
// Reports scalar levels only; never saves audio or changes endpoint settings.
#include "windows/devices.h"
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmsystem.h>
#include <dsound.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fcntl.h>
#include <io.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

using namespace partymic;
using Microsoft::WRL::ComPtr;
namespace {
// Microsoft's win32metadata endpointvolume.h ABI; MinGW only forward-declares
// IAudioMeterInformation. Keep a distinct name for native SDK compatibility.
struct PassiveMeter : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMeteringChannelCount(UINT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelsPeakValues(UINT32, float*) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD*) = 0;
};
constexpr GUID meter_iid{0xc02216f6,0x8c67,0x4b5b,{0x9d,0x00,0xd0,0x08,0xe7,0x3e,0x00,0x64}};
struct ComScope {
    ComScope() { check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM"); }
    ~ComScope() { CoUninitialize(); }
};
WAVEFORMATEX voice_format() { return {WAVE_FORMAT_PCM, 1, 16000, 32000, 2, 16, 0}; }
struct Levels {
    unsigned long long frames = 0, nonzero = 0;
    double squares = 0, peak = 0;
    void add(const void* bytes, size_t size, bool silent = false) {
        const auto* pcm = static_cast<const int16_t*>(bytes);
        for (size_t i = 0; i < size / 2; ++i) {
            const auto x = silent ? 0.0 : pcm[i] / 32768.0;
            ++frames; nonzero += x != 0; squares += x * x;
            peak = std::max(peak, std::abs(x));
        }
    }
    void report(const std::wstring& api, unsigned second) const {
        std::wcout << L"MIC api=" << api << L" second=" << second << L" tick=" << GetTickCount64()
            << L" frames=" << frames << L" nonzero=" << nonzero
            << L" rms=" << (frames ? std::sqrt(squares / frames) : 0)
            << L" peak=" << peak << std::endl;
    }
};
ComPtr<IMMDevice> brio() {
    std::wstring id;
    for (const auto& e : endpoints(eCapture))
        if (e.name.find(L"Brio 90") != std::wstring::npos) id = e.id;
    if (id.empty()) throw std::runtime_error("Brio 90 missing");
    ComPtr<IMMDeviceEnumerator> enumerator;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        IID_PPV_ARGS(&enumerator)), "Enumerator");
    ComPtr<IMMDevice> device;
    check(enumerator->GetDevice(id.c_str(), &device), "Brio device");
    return device;
}
ComPtr<IMMDevice> simulated_default_cable() {
    for (const auto& endpoint : endpoints(eCapture))
        if (endpoint.name.find(L"CABLE Output") != std::wstring::npos)
            return device_by_id(endpoint.id, eCapture);
    throw std::runtime_error("CABLE Output missing");
}
void state(bool include_format = true, IMMDevice* endpoint = nullptr) {
    ComPtr<IMMDevice> device = endpoint;
    if (!device) device = brio();
    if (include_format) {
    ComPtr<IAudioClient> format_client;
    check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(format_client.GetAddressOf())), "Format client");
    WAVEFORMATEX* format=nullptr;
    check(format_client->GetMixFormat(&format),"Device format");
    std::wcout<<L"FORMAT tag="<<format->wFormatTag<<L" channels="<<format->nChannels
        <<L" rate="<<format->nSamplesPerSec<<L" bits="<<format->wBitsPerSample
        <<L" align="<<format->nBlockAlign<<std::endl;
    CoTaskMemFree(format);format_client.Reset();
    }
    ComPtr<IAudioEndpointVolume> volume;
    check(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(volume.GetAddressOf())), "Endpoint volume");
    BOOL mute = FALSE; float level = 0;
    check(volume->GetMute(&mute), "Read mute");
    check(volume->GetMasterVolumeLevelScalar(&level), "Read level");
    std::wcout << L"ENDPOINT muted=" << mute << L" level=" << level << std::endl;
    ComPtr<IAudioSessionManager2> manager;
    check(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(manager.GetAddressOf())), "Capture session manager");
    ComPtr<IAudioSessionEnumerator> sessions;
    check(manager->GetSessionEnumerator(&sessions), "Capture sessions");
    int count = 0;check(sessions->GetCount(&count), "Session count");
    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> control;
        check(sessions->GetSession(i, &control), "Session");
        ComPtr<IAudioSessionControl2> session;
        ComPtr<ISimpleAudioVolume> simple;
        if (FAILED(control.As(&session)) || FAILED(control.As(&simple))) continue;
        DWORD pid = 0; AudioSessionState status{};
        check(session->GetProcessId(&pid), "Session pid");
        check(session->GetState(&status), "Session state");
        check(simple->GetMute(&mute), "Session mute");
        check(simple->GetMasterVolume(&level), "Session level");
        std::wcout << L"SESSION pid=" << pid << L" state=" << status << L" muted=" << mute << L" level=" << level;
        ComPtr<IChannelAudioVolume> channels;
        if (SUCCEEDED(control.As(&channels))) {
            UINT n = 0;
            if (SUCCEEDED(channels->GetChannelCount(&n)) && n <= 32)
                for (UINT c = 0; c < n; ++c) {
                    float gain = -1;
                    channels->GetChannelVolume(c, &gain);
                    std::wcout << L" channel" << c << L"=" << gain;
                }
        }
        std::wcout << std::endl;
        LPWSTR identifier = nullptr;
        const auto identity_hr = session->GetSessionIdentifier(&identifier);
        std::wcout << L"SESSION_ID pid=" << pid << L" hr=" << identity_hr
            << L" value=" << (identifier ? identifier : L"") << std::endl;
        CoTaskMemFree(identifier);
    }
}
void passive_inventory() {
    std::wcout << L"PASSIVE_ALL no_capture_client=1 tick=" << GetTickCount64() << std::endl;
    for (const auto role : {eConsole, eMultimedia, eCommunications})
        std::wcout << L"DEFAULT_CAPTURE role=" << role << L" id=" << default_endpoint_id(eCapture, role) << std::endl;
    for (const auto& endpoint : endpoints(eCapture)) {
        std::wcout << L"DEVICE name=" << endpoint.name << L" id=" << endpoint.id << std::endl;
        try {
            auto device = device_by_id(endpoint.id, eCapture);
            state(false, device.Get());
            ComPtr<PassiveMeter> meter;
            const auto activated = device->Activate(meter_iid, CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(meter.GetAddressOf()));
            float peak = -1;
            const auto hr = meter ? meter->GetPeakValue(&peak) : activated;
            std::wcout << L"METER hr=" << hr << L" peak=" << peak << std::endl;
        } catch (const std::exception& error) {
            const std::string message = error.what();
            std::wcout << L"DEVICE_ERROR " << std::wstring(message.begin(), message.end()) << std::endl;
        }
    }
}
void passive_meter(unsigned seconds) {
    // Reading endpoint/session controls does not initialize or start a stream.
    // Use this before an active probe, which could alter a failing device state.
    auto device = brio();
    ComPtr<PassiveMeter> meter;
    const auto activated = device->Activate(meter_iid, CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(meter.GetAddressOf()));
    state(false);
    std::wcout << L"PASSIVE no_capture_client=1 meter_hr=" << activated << std::endl;
    const auto deadline = GetTickCount64() + seconds * 1000ULL;
    while (GetTickCount64() < deadline) {
        float peak = -1;
        const auto hr = meter ? meter->GetPeakValue(&peak) : activated;
        std::wcout << L"METER tick=" << GetTickCount64() << L" hr=" << hr << L" peak=" << peak << std::endl;
        Sleep(200);
    }
    state(false);
}
class Probe {
public:
    virtual ~Probe() = default;
    virtual void drain(Levels&) = 0;
};
class Wasapi final : public Probe {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
public:
    explicit Wasapi(ComPtr<IMMDevice> device, bool communications) {
        check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(client.GetAddressOf())), "Probe WASAPI activate");
        ComPtr<IAudioClient2> properties;
        check(client.As(&properties), "Probe client2");
        AudioClientProperties options{}; options.cbSize = sizeof(options);
        options.eCategory = communications ? AudioCategory_Communications : AudioCategory_Other;
        check(properties->SetClientProperties(&options), "Probe category");
        auto format = voice_format();
        check(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 1000000, 0, &format, nullptr), "Probe WASAPI initialize");
        check(client->GetService(IID_PPV_ARGS(&capture)), "Probe capture service");
        check(client->Start(), "Probe start");
    }
    ~Wasapi() override { if(client)client->Stop(); }
    void drain(Levels& levels) override {
        for(unsigned i=0;i<64;++i) {
            UINT32 count=0;check(capture->GetNextPacketSize(&count),"Probe packet");if(!count)break;
            BYTE* data=nullptr;DWORD flags=0;
            check(capture->GetBuffer(&data,&count,&flags,nullptr,nullptr),"Probe read");
            levels.add(data,size_t(count)*2,(flags&AUDCLNT_BUFFERFLAGS_SILENT)!=0);
            check(capture->ReleaseBuffer(count),"Probe release");
        }
    }
};
void mmcheck(MMRESULT result, const char* operation) {
    if(result!=MMSYSERR_NOERROR)
        throw std::runtime_error(std::string(operation)+" MMRESULT="+std::to_string(result));
}
class WaveIn final : public Probe {
    HWAVEIN input = nullptr;
    std::array<std::array<char,640>,8> buffers{};
    std::array<WAVEHDR,8> headers{};
    void close() noexcept {
        if(!input)return;
        waveInStop(input);waveInReset(input);
        for(auto& header:headers)
            if(header.dwFlags&WHDR_PREPARED)waveInUnprepareHeader(input,&header,sizeof(header));
        waveInClose(input);input=nullptr;
    }
public:
    WaveIn() {
        UINT id=UINT(-1);
        for(UINT i=0;i<waveInGetNumDevs();++i) {
            WAVEINCAPSW caps{};
            if(waveInGetDevCapsW(i,&caps,sizeof(caps))==MMSYSERR_NOERROR &&
                std::wstring(caps.szPname).find(L"Brio 90")!=std::wstring::npos)id=i;
        }
        if(id==UINT(-1))throw std::runtime_error("WinMM Brio missing");
        auto format=voice_format();
        mmcheck(waveInOpen(&input,id,&format,0,0,CALLBACK_NULL),"waveInOpen");
        try {
            for(size_t i=0;i<headers.size();++i) {
                auto& header=headers[i];header.lpData=buffers[i].data();header.dwBufferLength=DWORD(buffers[i].size());
                mmcheck(waveInPrepareHeader(input,&header,sizeof(header)),"waveInPrepareHeader");
                mmcheck(waveInAddBuffer(input,&header,sizeof(header)),"waveInAddBuffer");
            }
            mmcheck(waveInStart(input),"waveInStart");
        }catch(...){close();throw;}
    }
    ~WaveIn() override {close();}
    void drain(Levels& levels) override {
        for(auto& header:headers)if(header.dwFlags&WHDR_DONE) {
            levels.add(header.lpData,header.dwBytesRecorded);
            mmcheck(waveInAddBuffer(input,&header,sizeof(header)),"waveIn requeue");
        }
    }
};
class DirectSound final : public Probe {
    ComPtr<IDirectSoundCapture> device;
    ComPtr<IDirectSoundCaptureBuffer> buffer;
    DWORD cursor=0;
    static BOOL CALLBACK enumerate(GUID* id,LPCWSTR description,LPCWSTR,void* context) {
        if(id&&std::wstring(description).find(L"Brio 90")!=std::wstring::npos) {
            *static_cast<GUID*>(context)=*id;return FALSE;
        }
        return TRUE;
    }
public:
    DirectSound() {
        GUID id=GUID_NULL;
        check(DirectSoundCaptureEnumerateW(enumerate,&id),"DirectSound capture enumeration");
        if(IsEqualGUID(id,GUID_NULL))throw std::runtime_error("DirectSound Brio missing");
        check(DirectSoundCaptureCreate(&id,&device,nullptr),"DirectSoundCaptureCreate");
        auto format=voice_format();DSCBUFFERDESC description{};
        description.dwSize=sizeof(description);description.dwBufferBytes=32000;description.lpwfxFormat=&format;
        check(device->CreateCaptureBuffer(&description,&buffer,nullptr),"DirectSound capture buffer");
        check(buffer->Start(DSCBSTART_LOOPING),"DirectSound capture start");
    }
    ~DirectSound() override {if(buffer)buffer->Stop();}
    void drain(Levels& levels) override {
        DWORD safe=0;check(buffer->GetCurrentPosition(nullptr,&safe),"DirectSound position");
        DWORD count=(safe+32000-cursor)%32000;count-=count%2;if(!count)return;
        void* first=nullptr;void* second=nullptr;DWORD nfirst=0,nsecond=0;
        check(buffer->Lock(cursor,count,&first,&nfirst,&second,&nsecond,0),"DirectSound read");
        levels.add(first,nfirst);if(second)levels.add(second,nsecond);
        check(buffer->Unlock(first,nfirst,second,nsecond),"DirectSound release");
        cursor=(cursor+count)%32000;
    }
};
}
int wmain(int argc,wchar_t** argv) {
    _setmode(_fileno(stdout),_O_U8TEXT);
    try {
        ComScope com;
        if (argc == 2 && std::wstring(argv[1]) == L"snapshot-all") {
            passive_inventory(); return 0;
        }
        if (argc == 3 && std::wstring(argv[1]) == L"snapshot") {
            const auto seconds = std::stoi(argv[2]);
            if (seconds < 1 || seconds > 10) throw std::runtime_error("Snapshot duration must be 1..10 seconds");
            passive_meter(static_cast<unsigned>(seconds)); return 0;
        }
        if(argc==1){state();return 0;}
        if(argc!=3)throw std::runtime_error("Use [wavein|directsound|other|communications|route] seconds (1..30)");
        const std::wstring method=argv[1];const auto seconds=std::stoi(argv[2]);
        if(seconds<1||seconds>30)throw std::runtime_error("Duration must be 1..30 seconds");
        if(method!=L"route")state();
        std::unique_ptr<Probe> probe;
        if(method==L"wavein")probe=std::make_unique<WaveIn>();
        else if(method==L"directsound")probe=std::make_unique<DirectSound>();
        else if(method==L"other"||method==L"communications")
            probe=std::make_unique<Wasapi>(brio(),method==L"communications");
        else if(method==L"route") {
            std::wcout<<L"ROUTE displayed=Brio90 effective=CABLE_Output model=incident_replay"
                <<L" actual_console_default="<<default_endpoint_id(eCapture,eConsole)<<std::endl;
            probe=std::make_unique<Wasapi>(simulated_default_cable(),false);
        }
        else throw std::runtime_error("Unknown capture API");
        std::wcout<<L"OPEN api="<<method<<L" tick="<<GetTickCount64()<<L" pid="<<GetCurrentProcessId()<<std::endl;
        const auto start=GetTickCount64();unsigned reported=0;Levels levels,total;
        while(GetTickCount64()-start<static_cast<unsigned>(seconds)*1000ULL) {
            probe->drain(levels);
            const auto elapsed=unsigned((GetTickCount64()-start)/1000);
            if(elapsed>reported) {
                levels.report(method,elapsed);total.frames+=levels.frames;total.nonzero+=levels.nonzero;
                levels={};reported=elapsed;
            }
            Sleep(3);
        }
        probe->drain(levels);levels.report(method,unsigned(seconds));total.frames+=levels.frames;total.nonzero+=levels.nonzero;
        probe.reset();if(method!=L"route")state();
        std::wcout<<L"TOTAL frames="<<total.frames<<L" nonzero="<<total.nonzero<<std::endl;
        if(total.frames<static_cast<unsigned>(seconds)*8000ULL)throw std::runtime_error("Too few captured frames");
        std::wcout<<L"PASS packet delivery; signal audibility is not certified"<<std::endl;
        return 0;
    }catch(const std::exception& error){std::wcout<<L"FAIL "<<error.what()<<std::endl;return 1;}
}
