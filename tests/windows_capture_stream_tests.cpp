// No hardware: the adapters below implement the operating-system COM seam.
// The tests call the same CaptureStream interface used by mixer and recorder.
#include "windows/capture_stream.h"
#include <audiopolicy.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace partymic;
namespace {
void require(bool value, const char* reason) {
    if (!value) throw std::runtime_error(reason);
}
class Session final : public IAudioSessionControl {
public:
    ULONG references = 1;
    IAudioSessionEvents* notification = nullptr;
    STDMETHOD(QueryInterface)(REFIID iid, void** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IAudioSessionControl)) return E_NOINTERFACE;
        *value = static_cast<IAudioSessionControl*>(this); AddRef(); return S_OK;
    }
    STDMETHOD_(ULONG, AddRef)() override { return ++references; }
    STDMETHOD_(ULONG, Release)() override { return --references; }
    STDMETHOD(GetState)(AudioSessionState*) override { return E_NOTIMPL; }
    STDMETHOD(GetDisplayName)(LPWSTR*) override { return E_NOTIMPL; }
    STDMETHOD(SetDisplayName)(LPCWSTR, LPCGUID) override { return E_NOTIMPL; }
    STDMETHOD(GetIconPath)(LPWSTR*) override { return E_NOTIMPL; }
    STDMETHOD(SetIconPath)(LPCWSTR, LPCGUID) override { return E_NOTIMPL; }
    STDMETHOD(GetGroupingParam)(GUID*) override { return E_NOTIMPL; }
    STDMETHOD(SetGroupingParam)(LPCGUID, LPCGUID) override { return E_NOTIMPL; }
    STDMETHOD(RegisterAudioSessionNotification)(IAudioSessionEvents* value) override {
        if (notification) return E_UNEXPECTED;
        notification = value; notification->AddRef(); return S_OK;
    }
    STDMETHOD(UnregisterAudioSessionNotification)(IAudioSessionEvents* value) override {
        if (notification != value) return E_INVALIDARG;
        notification->Release(); notification = nullptr; return S_OK;
    }
};
class Capture final : public IAudioCaptureClient {
public:
    ULONG references = 1;
    bool held = false, consumed = false;
    DWORD flags = 0;
    std::array<float, 4> samples{0.25f, -0.5f, 0.125f, 0.0f};
    STDMETHOD(QueryInterface)(REFIID iid, void** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IAudioCaptureClient)) return E_NOINTERFACE;
        *value = static_cast<IAudioCaptureClient*>(this); AddRef(); return S_OK;
    }
    STDMETHOD_(ULONG, AddRef)() override { return ++references; }
    STDMETHOD_(ULONG, Release)() override { return --references; }
    STDMETHOD(GetNextPacketSize)(UINT32* count) override { *count = consumed ? 0 : 2; return S_OK; }
    STDMETHOD(GetBuffer)(BYTE** bytes, UINT32* count, DWORD* result_flags, UINT64*, UINT64*) override {
        if (held) return AUDCLNT_E_OUT_OF_ORDER;
        held = true; *count = 2; *result_flags = flags;
        *bytes = flags & AUDCLNT_BUFFERFLAGS_SILENT ? nullptr : reinterpret_cast<BYTE*>(samples.data());
        return S_OK;
    }
    STDMETHOD(ReleaseBuffer)(UINT32 frames) override {
        if (!held || frames != 2) return AUDCLNT_E_OUT_OF_ORDER;
        held = false; consumed = true; return S_OK;
    }
};
class Client final : public IAudioClient {
public:
    ULONG references = 1;
    Session session;
    Capture capture;
    bool started = false;
    HRESULT query_result = S_OK;
    HRESULT initialize_result = S_OK, capture_result = S_OK, start_result = S_OK;
    WAVEFORMATEXTENSIBLE native{};
    WAVEFORMATEXTENSIBLE initialized{};
    DWORD stream_flags = 0;
    Client() { native.Format = {WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 384000, 8, 32, 0}; }
    STDMETHOD(QueryInterface)(REFIID, void**) override { return E_NOINTERFACE; }
    STDMETHOD_(ULONG, AddRef)() override { return ++references; }
    STDMETHOD_(ULONG, Release)() override { return --references; }
    STDMETHOD(Initialize)(AUDCLNT_SHAREMODE mode, DWORD flags, REFERENCE_TIME, REFERENCE_TIME,
                         const WAVEFORMATEX* wave, LPCGUID) override {
        if (mode != AUDCLNT_SHAREMODE_SHARED) return E_INVALIDARG;
        if (FAILED(initialize_result)) return initialize_result;
        // A real OS consumer reads sizeof(WAVEFORMATEX)+cbSize bytes.
        // Reject a descriptor that promises more than the adapter can hold.
        if (wave->cbSize > sizeof(initialized) - sizeof(WAVEFORMATEX)) return E_INVALIDARG;
        stream_flags = flags;
        std::memcpy(&initialized, wave, sizeof(WAVEFORMATEX) + wave->cbSize);
        return S_OK;
    }
    STDMETHOD(GetBufferSize)(UINT32* count) override { *count = 480; return S_OK; }
    STDMETHOD(GetStreamLatency)(REFERENCE_TIME*) override { return E_NOTIMPL; }
    STDMETHOD(GetCurrentPadding)(UINT32*) override { return E_NOTIMPL; }
    STDMETHOD(IsFormatSupported)(AUDCLNT_SHAREMODE, const WAVEFORMATEX*, WAVEFORMATEX**) override { return E_NOTIMPL; }
    STDMETHOD(GetMixFormat)(WAVEFORMATEX** result) override {
        if (FAILED(query_result)) { *result = nullptr; return query_result; }
        const auto bytes = std::max(sizeof(native), sizeof(WAVEFORMATEX) + size_t(native.Format.cbSize));
        *result = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(bytes));
        if (!*result) return E_OUTOFMEMORY;
        std::memset(*result, 0, bytes);
        std::memcpy(*result, &native, sizeof(native)); return S_OK;
    }
    STDMETHOD(GetDevicePeriod)(REFERENCE_TIME*, REFERENCE_TIME*) override { return E_NOTIMPL; }
    STDMETHOD(Start)() override { if (FAILED(start_result)) return start_result; started = true; return S_OK; }
    STDMETHOD(Stop)() override { started = false; return S_OK; }
    STDMETHOD(Reset)() override { return E_NOTIMPL; }
    STDMETHOD(SetEventHandle)(HANDLE event) override { return event ? S_OK : E_INVALIDARG; }
    STDMETHOD(GetService)(REFIID iid, void** result) override {
        if (iid == __uuidof(IAudioCaptureClient)) {
            if (FAILED(capture_result)) { *result = nullptr; return capture_result; }
            return capture.QueryInterface(iid, result);
        }
        return session.QueryInterface(iid, result);
    }
};
class Device final : public IMMDevice {
    Client& client_;
public:
    explicit Device(Client& client) : client_(client) {}
    STDMETHOD(QueryInterface)(REFIID, void**) override { return E_NOINTERFACE; }
    STDMETHOD_(ULONG, AddRef)() override { return 1; }
    STDMETHOD_(ULONG, Release)() override { return 1; }
    STDMETHOD(Activate)(REFIID iid, DWORD, PROPVARIANT*, void** result) override {
        *result = nullptr;
        if (iid != __uuidof(IAudioClient)) return E_NOINTERFACE;
        *result = static_cast<IAudioClient*>(&client_); client_.AddRef(); return S_OK;
    }
    STDMETHOD(OpenPropertyStore)(DWORD, IPropertyStore**) override { return E_NOTIMPL; }
    STDMETHOD(GetId)(LPWSTR*) override { return E_NOTIMPL; }
    STDMETHOD(GetState)(DWORD*) override { return E_NOTIMPL; }
};
void close_releases_capture_resources() {
    Client client;
    Device device(client);
    {
        CaptureStream stream(&device, CaptureTiming::event, "test");
        stream.start();
        stream.close();
        stream.close();
    }
    require(!client.started && client.references == 1 && client.capture.references == 1 &&
                client.session.references == 1 && !client.session.notification,
            "Closing the public stream releases capture and session ownership, including repeated close");
}
void packets_reach_the_consumer() {
    Client client;
    Device device(client);
    CaptureStream stream(&device, CaptureTiming::polling, "test");
    stream.start();
    std::array<float, 4> received{};
    UINT32 frames = 0;
    stream.drain([&](std::span<const float> packet, UINT32 count) {
        require(packet.size() == 4, "One stereo packet contains four samples");
        std::copy(packet.begin(), packet.end(), received.begin()); frames += count;
    });
    require(frames == 2 && received == std::array<float, 4>{0.25f, -0.5f, 0.125f, 0.0f},
            "Capture delivers the known OS packet without altering audio");
}
void consumer_failure_does_not_hold_an_os_packet() {
    Client client;
    Device device(client);
    CaptureStream stream(&device, CaptureTiming::polling, "test");
    stream.start();
    bool threw = false;
    try {
        stream.drain([](std::span<const float>, UINT32) { throw std::runtime_error("consumer failed"); });
    } catch (const std::runtime_error& error) {
        threw = std::string(error.what()) == "consumer failed";
    }
    require(threw, "Consumer error propagates to its caller");
    // The OS adapter rejects the next GetBuffer if the first packet is held.
    stream.drain([](std::span<const float>, UINT32) {});
    require(!client.capture.held, "Consumer failure releases the OS packet before returning");
}
void shared_format_behavior_is_preserved() {
    constexpr DWORD conversion = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    {
        Client client; Device device(client);
        CaptureStream stream(&device, CaptureTiming::event, "test");
        require(client.stream_flags == AUDCLNT_STREAMFLAGS_EVENTCALLBACK &&
                    client.initialized.Format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT,
                "Native stereo 48k float opens directly without a converter");
    }
    {
        Client client; Device device(client);
        client.native.Format = {WAVE_FORMAT_EXTENSIBLE, 2, 48000, 384000, 8, 32, 22};
        client.native.Samples.wValidBitsPerSample = 32; client.native.dwChannelMask = 3;
        client.native.SubFormat = {3, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
        CaptureStream stream(&device, CaptureTiming::polling, "test");
        require(!client.stream_flags && std::memcmp(&client.initialized, &client.native, sizeof(client.native)) == 0,
                "Native extensible channel mask and valid bits reach Windows unchanged");
    }
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        Client client; Device device(client);
        if (scenario == 0) client.native.Format = {WAVE_FORMAT_PCM, 2, 48000, 192000, 4, 16, 0};
        if (scenario == 1) client.native.Format = {WAVE_FORMAT_IEEE_FLOAT, 1, 48000, 192000, 4, 32, 0};
        if (scenario == 2) client.native.Format = {WAVE_FORMAT_IEEE_FLOAT, 2, 44100, 352800, 8, 32, 0};
        if (scenario == 3) client.query_result = E_FAIL;
        if (scenario == 4) {
            client.native.Format = {WAVE_FORMAT_EXTENSIBLE, 2, 48000, 192000, 4, 16, 22};
            client.native.Samples.wValidBitsPerSample = 16; client.native.dwChannelMask = 3;
            client.native.SubFormat = {1, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
        }
        if (scenario == 5) client.native.Format = {WAVE_FORMAT_IEEE_FLOAT, 2, 96000, 768000, 8, 32, 0};
        CaptureStream stream(&device, CaptureTiming::event, "test");
        require(client.stream_flags == (conversion | AUDCLNT_STREAMFLAGS_EVENTCALLBACK) &&
                    client.initialized.Format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT &&
                    client.initialized.Format.nSamplesPerSec == 48000 && client.initialized.Format.nChannels == 2,
                "Unsupported native formats retain the stereo 48k float conversion path");
    }
}
void silent_packets_do_not_require_a_data_pointer() {
    Client client; Device device(client);
    client.capture.flags = AUDCLNT_BUFFERFLAGS_SILENT | AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY;
    CaptureStream stream(&device, CaptureTiming::polling, "test");
    stream.start();
    UINT32 received = 0;
    stream.drain([&](std::span<const float> samples, UINT32 frames) {
        require(samples.empty(), "Windows silent flag is represented without dereferencing null");
        received += frames;
    });
    stream.close();
    const auto statistics = stream.statistics();
    require(received == 2 && statistics.frames == 2 && statistics.silent_frames == 2 &&
                statistics.signal_frames == 0 && statistics.discontinuities == 1,
            "Silent and discontinuous packet diagnostics remain readable after close");
}
void extended_format_does_not_expose_unowned_bytes() {
    Client client; Device device(client);
    client.native.Format = {WAVE_FORMAT_EXTENSIBLE, 2, 48000, 384000, 8, 32, 24};
    client.native.Samples.wValidBitsPerSample = 32;
    client.native.dwChannelMask = 3;
    client.native.SubFormat = {3, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
    CaptureStream stream(&device, CaptureTiming::polling, "extended-format");
    stream.start();
    unsigned frames = 0;
    stream.drain([&](std::span<const float>, UINT32 count) { frames += count; });
    require(frames == 2 && client.initialized.Format.cbSize <= 22,
        "Formats with extra native bytes use a fully owned conversion descriptor");
}
void failed_open_or_start_releases_ownership() {
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        Client client; Device device(client);
        if (scenario == 0) client.initialize_result = E_FAIL;
        if (scenario == 1) client.capture_result = E_FAIL;
        if (scenario == 2) client.start_result = E_FAIL;
        bool failed = false;
        try {
            CaptureStream stream(&device, CaptureTiming::event, "test");
            stream.start();
        } catch (const std::runtime_error&) { failed = true; }
        require(failed && !client.started && client.references == 1 && client.capture.references == 1 &&
                    client.session.references == 1 && !client.session.notification,
                "Failed open or start leaves no capture or notification ownership behind");
    }
}
}
int main() {
    try {
        close_releases_capture_resources();
        packets_reach_the_consumer();
        consumer_failure_does_not_hold_an_os_packet();
        shared_format_behavior_is_preserved();
        extended_format_does_not_expose_unowned_bytes();
        silent_packets_do_not_require_a_data_pointer();
        failed_open_or_start_releases_ownership();
        std::cout << "PASS capture stream lifecycle\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
