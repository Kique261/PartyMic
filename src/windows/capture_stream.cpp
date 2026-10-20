#include "windows/capture_stream.h"
#include "windows/audio_observer.h"
#include "windows/devices.h"
#include "windows/diagnostics.h"
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <wrl/client.h>
#include <cmath>
#include <string>
#include <stdexcept>

namespace partymic {
using Microsoft::WRL::ComPtr;
namespace {
struct SharedCaptureFormat {
    WAVEFORMATEXTENSIBLE storage{};
    DWORD flags = 0;
    const WAVEFORMATEX* wave() const { return &storage.Format; }
};
SharedCaptureFormat capture_format(IAudioClient* client,DWORD flags,const char* label) {
    SharedCaptureFormat result;
    result.storage.Format={WAVE_FORMAT_IEEE_FLOAT,2,48000,384000,8,32,0};
    result.flags=flags;
    WAVEFORMATEX* native=nullptr;
    const auto hr=client->GetMixFormat(&native);
    bool direct=false;
    if(SUCCEEDED(hr)&&native) {
        bool floating=native->wFormatTag==WAVE_FORMAT_IEEE_FLOAT;
        if(native->wFormatTag==WAVE_FORMAT_EXTENSIBLE&&native->cbSize>=22) {
            // WAVEFORMATEX subtype GUID defined by the Windows SDK.
            static constexpr GUID ieee_float={WAVE_FORMAT_IEEE_FLOAT,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
            floating=IsEqualGUID(reinterpret_cast<WAVEFORMATEXTENSIBLE*>(native)->SubFormat,ieee_float);
        }
        direct=floating&&native->nSamplesPerSec==48000&&native->nChannels==2&&
            native->wBitsPerSample==32&&native->nBlockAlign==8;
        if(direct) {
            if(native->wFormatTag==WAVE_FORMAT_EXTENSIBLE)result.storage=*reinterpret_cast<WAVEFORMATEXTENSIBLE*>(native);
            else result.storage.Format=*native;
            result.flags&=~(AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY);
        }
        diagnostics::write("capture.native_format","stream="+std::string(label)+" tag="+std::to_string(native->wFormatTag)+
            " channels="+std::to_string(native->nChannels)+" rate="+std::to_string(native->nSamplesPerSec)+
            " bits="+std::to_string(native->wBitsPerSample)+" direct="+std::to_string(direct));
    }else diagnostics::write("capture.native_format","stream="+std::string(label)+" query_hr="+std::to_string(hr));
    CoTaskMemFree(native);
    return result;
}
}
struct CaptureStream::State {
    std::string label;
    HANDLE event = nullptr;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    AudioObserver observer;
    bool started = false;
    explicit State(const char* name) : label(name) {}
    ~State() {
        diagnostics::write("stream.release.begin", label);
        if (started) {
            const auto hr = client->Stop();
            diagnostics::write("stream.stop.result", label + " stop_hr=" + std::to_string(hr));
        }
        observer.close();
        capture.Reset();
        client.Reset();
        if (event) CloseHandle(event);
        diagnostics::write("stream.release.end", label);
    }
};
CaptureStream::CaptureStream(IMMDevice* endpoint, CaptureTiming timing, const char* label) {
    if (!endpoint) throw std::invalid_argument("Capture endpoint is required");
    auto state = std::make_unique<State>(label);
    check(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(state->client.GetAddressOf())), "Activate capture endpoint");
    ComPtr<IAudioEndpointVolume> endpoint_volume;
    if (SUCCEEDED(endpoint->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(endpoint_volume.GetAddressOf())))) {
        BOOL muted = FALSE; float level = 0;
        const auto a = endpoint_volume->GetMute(&muted), b = endpoint_volume->GetMasterVolumeLevelScalar(&level);
        diagnostics::write("microphone.endpoint.state", "stream=" + state->label + " read_only=1 mute_hr=" +
            std::to_string(a) + " level_hr=" + std::to_string(b) + " muted=" + std::to_string(muted) + " level=" + std::to_string(level));
    }
    DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (timing == CaptureTiming::event) {
        flags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        state->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!state->event) check(HRESULT_FROM_WIN32(GetLastError()), "Create capture event");
    }
    const auto format = capture_format(state->client.Get(), flags, label);
    diagnostics::write("stream.initialize.begin", state->label + " shared=1 capture=1 flags=" + std::to_string(format.flags));
    check(state->client->Initialize(AUDCLNT_SHAREMODE_SHARED, format.flags,
        timing == CaptureTiming::event ? 0 : 1000000, 0, format.wave(), nullptr), "Initialize shared capture");
    if (state->event) check(state->client->SetEventHandle(state->event), "Set capture event");
    check(state->client->GetBufferSize(&buffer_frames_), "Read capture buffer size");
    check(state->client->GetService(IID_PPV_ARGS(&state->capture)), "Get capture service");
    state->observer.attach(state->client.Get(), endpoint, label);
    ComPtr<ISimpleAudioVolume> volume;
    if (SUCCEEDED(state->client->GetService(IID_PPV_ARGS(&volume)))) {
        BOOL muted = FALSE; float level = 0;
        const auto a = volume->GetMute(&muted), b = volume->GetMasterVolume(&level);
        diagnostics::write("microphone.session.state", "stream=" + state->label + " read_only=1 mute_hr=" +
            std::to_string(a) + " level_hr=" + std::to_string(b) + " muted=" + std::to_string(muted) + " level=" + std::to_string(level));
    }
    diagnostics::write("stream.initialize.end", state->label + " buffer_frames=" + std::to_string(buffer_frames_));
    state_ = std::move(state);
}
CaptureStream::~CaptureStream() { close(); }
void CaptureStream::close() noexcept { state_.reset(); }
HANDLE CaptureStream::ready() const noexcept { return state_ ? state_->event : nullptr; }
void CaptureStream::start() {
    if (!state_) throw std::logic_error("Capture stream is closed");
    if (state_->started) throw std::logic_error("Capture stream is already running");
    diagnostics::write("stream.start.begin", state_->label);
    check(state_->client->Start(), "Start shared capture");
    state_->started = true;
    diagnostics::write("stream.start.end", state_->label);
}
void CaptureStream::drain(const Consumer& consume) {
    if (!state_ || !state_->started) throw std::logic_error("Capture stream is not running");
    for (unsigned packet = 0; packet < 128; ++packet) {
        UINT32 frames = 0;
        check(state_->capture->GetNextPacketSize(&frames), "Read capture packet size");
        if (!frames) return;
        BYTE* bytes = nullptr;
        DWORD flags = 0;
        check(state_->capture->GetBuffer(&bytes, &frames, &flags, nullptr, nullptr), "Read capture packet");
        const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        const std::span<const float> samples = silent ? std::span<const float>{} :
            std::span<const float>{reinterpret_cast<const float*>(bytes), size_t(frames) * 2};
        statistics_.frames += frames;
        if (silent) statistics_.silent_frames += frames;
        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) ++statistics_.discontinuities;
        for (UINT32 i = 0; i < frames && !silent; ++i)
            if (std::abs(samples[i * 2]) > 0.00001f || std::abs(samples[i * 2 + 1]) > 0.00001f)
                ++statistics_.signal_frames;
        // A caller error must not leave a Windows packet acquired. Stop/close
        // may follow immediately, or the caller may try another drain.
        try { consume(samples, frames); }
        catch (...) {
            state_->capture->ReleaseBuffer(frames);
            throw;
        }
        check(state_->capture->ReleaseBuffer(frames), "Release capture packet");
    }
}
}
