#include "windows/audio_engine.h"
#include "windows/devices.h"
#include "windows/audio_activation.h"
#include "windows/capture_stream.h"
#include "windows/diagnostics.h"

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <vector>

namespace partymic {
using Microsoft::WRL::ComPtr;
namespace {
unsigned long long microseconds() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
void update_max(std::atomic<unsigned long long>& value, unsigned long long next) {
    // Each maximum has a single writer (stream worker or process watcher).
    if (next > value.load(std::memory_order_relaxed))
        value.store(next, std::memory_order_relaxed);
}
class Event {
public:
    explicit Event(bool manual = false) : handle_(CreateEventW(nullptr, manual, FALSE, nullptr)) {
        if (!handle_) check(HRESULT_FROM_WIN32(GetLastError()), "Create audio event");
    }
    ~Event() { CloseHandle(handle_); }
    operator HANDLE() const { return handle_; }
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
private:
    HANDLE handle_;
};

struct ComScope {
    ComScope() { check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Initialize audio COM"); }
    ~ComScope() { CoUninitialize(); }
};

struct AudioTask {
    DWORD index = 0;
    HANDLE handle = AvSetMmThreadCharacteristicsW(L"Audio", &index);
    AudioTask() { if (handle) AvSetMmThreadPriority(handle, AVRT_PRIORITY_HIGH); }
    ~AudioTask() { if (handle) AvRevertMmThreadCharacteristics(handle); }
};

// Toolhelp snapshots and process-name allocations must never delay WASAPI.
// Monitor only the selected PID/name; no target-process handles are opened.
class ProcessWatch {
public:
    ProcessWatch(DWORD pid, std::wstring executable, HANDLE stop,
                 std::atomic<unsigned long long>& max_check)
        : worker_([this, pid, executable = std::move(executable), stop, &max_check] {
            try {
                HANDLE waits[] = {stop, cancel_};
                for (;;) {
                    const auto wait = WaitForMultipleObjects(2, waits, FALSE, 2000);
                    if (wait == WAIT_OBJECT_0 || wait == WAIT_OBJECT_0 + 1) return;
                    if (wait != WAIT_TIMEOUT) { state_ = State::failed; return; }
                    const auto begin = microseconds();
                    const bool matches = process_matches(pid, executable);
                    update_max(max_check, microseconds() - begin);
                    if (!matches) { state_ = State::exited; return; }
                }
            } catch (...) { state_ = State::failed; }
        }) {}
    ~ProcessWatch() {
        SetEvent(cancel_);
        if (worker_.joinable()) {
            diagnostics::write("source_watcher.join.begin");
            worker_.join();
            diagnostics::write("source_watcher.join.end");
        }
    }
    void check_source() const {
        switch (state_.load()) {
        case State::exited:
            throw std::runtime_error("Music application exited. Refresh and select the new process.");
        case State::failed:
            throw std::runtime_error("Music application monitoring failed. Refresh and select it again.");
        case State::valid: break;
        }
    }
private:
    enum class State { valid, exited, failed };
    Event cancel_{true};
    std::atomic<State> state_{State::valid};
    std::thread worker_;
};

WAVEFORMATEX format() {
    WAVEFORMATEX value{};
    value.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    value.nChannels = channels;
    value.nSamplesPerSec = sample_rate;
    value.wBitsPerSample = 32;
    value.nBlockAlign = channels * sizeof(float);
    value.nAvgBytesPerSec = sample_rate * value.nBlockAlign;
    return value;
}

// Explicit COM lifetime + the system free-threaded marshaler keep this callback
// agile without depending on MSVC-only WRL implementation helpers.
class Activation final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    Event completed{true};
    HRESULT result = E_PENDING;
    ComPtr<IAudioClient> client;
    AUDIOCLIENT_ACTIVATION_PARAMS parameters{};
    PROPVARIANT variant{};

    explicit Activation(DWORD pid) {
        check(CoCreateFreeThreadedMarshaler(static_cast<IActivateAudioInterfaceCompletionHandler*>(this),
            &marshaler_), "Create audio activation marshaler");
        parameters.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        parameters.ProcessLoopbackParams.TargetProcessId = pid;
        parameters.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        variant.vt = VT_BLOB;
        variant.blob.cbSize = sizeof(parameters);
        variant.blob.pBlobData = reinterpret_cast<BYTE*>(&parameters);
    }
    STDMETHOD(QueryInterface)(REFIID iid, void** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IActivateAudioInterfaceCompletionHandler))
            *value = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        else if (iid == __uuidof(IAgileObject))
            *value = static_cast<IAgileObject*>(this);
        else if (iid == __uuidof(IMarshal) && marshaler_)
            return marshaler_->QueryInterface(iid, value);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    STDMETHOD_(ULONG, AddRef)() override { return static_cast<ULONG>(InterlockedIncrement(&references_)); }
    STDMETHOD_(ULONG, Release)() override {
        const auto remaining = InterlockedDecrement(&references_);
        if (!remaining) delete this;
        return static_cast<ULONG>(remaining);
    }
    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* operation) override {
        ComPtr<IUnknown> unknown;
        HRESULT activated = E_FAIL;
        result = operation->GetActivateResult(&activated, &unknown);
        if (SUCCEEDED(result)) result = activated;
        if (SUCCEEDED(result)) result = unknown ? unknown.As(&client) : E_NOINTERFACE;
        SetEvent(completed);
        return S_OK;
    }
private:
    ~Activation() = default;
    LONG references_ = 1;
    ComPtr<IUnknown> marshaler_;
};

ComPtr<IAudioClient> activate_process(DWORD pid, HANDLE stop) {
    ComPtr<Activation> callback;
    callback.Attach(new Activation(pid));
    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    check(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
        __uuidof(IAudioClient), &callback->variant, callback.Get(), &operation), "Activate application loopback");
    HANDLE waits[] = {stop, callback->completed};
    const auto wait = WaitForMultipleObjects(2, waits, FALSE, 15000);
    if (wait == WAIT_OBJECT_0) return {};
    if (wait == WAIT_TIMEOUT) throw std::runtime_error("Application loopback activation timed out");
    if (wait == WAIT_FAILED) check(HRESULT_FROM_WIN32(GetLastError()), "Wait for application loopback");
    check(callback->result, "Application loopback activation (Windows 11 required)");
    return callback->client;
}

ComPtr<IAudioClient> activate_endpoint(const std::wstring& id, EDataFlow flow) {
    auto device = device_by_id(id, flow);
    if (flow == eRender) {
        ComPtr<IPropertyStore> properties;
        check(device->OpenPropertyStore(STGM_READ, &properties), "Read virtual cable properties");
        PROPVARIANT name{};
        check(properties->GetValue(PKEY_Device_FriendlyName, &name), "Read virtual cable name");
        bool cable = name.vt == VT_LPWSTR && name.pwszVal && is_virtual_cable(name.pwszVal);
        PropVariantClear(&name);
        if (!cable) throw std::runtime_error("Output must be a VB-CABLE Input playback device");
        // Keeping the default output on headphones also prevents accidental rerouting
        // of games, system notifications and friends' voices into the cable.
        ComPtr<IMMDeviceEnumerator> enumerator;
        check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator)), "Check default output");
        for (ERole role : {eConsole, eMultimedia, eCommunications}) {
            ComPtr<IMMDevice> default_device;
            if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, role, &default_device))) {
                LPWSTR default_id = nullptr;
                if (SUCCEEDED(default_device->GetId(&default_id))) {
                    bool matches = id == default_id;
                    CoTaskMemFree(default_id);
                    if (matches) throw std::runtime_error("Set Windows default/communications playback to headphones, not VB-CABLE");
                }
            }
        }
    }
    ComPtr<IAudioClient> client;
    check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(client.GetAddressOf())), "Activate selected endpoint");
    return client;
}

struct Stream {
    const char* name;
    explicit Stream(const char* value) : name(value) {}
    Event ready;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    ComPtr<IAudioRenderClient> render;
    UINT32 buffer_frames = 0;
    bool started = false;
    unsigned long long discontinuities = 0, silent_frames = 0;
    unsigned long long captured_frames = 0, signal_frames = 0;
    ~Stream() {
        if (started && render) {
            try { finish_render(); } catch (...) {}
        }
        close(name);
    }

    void stop() noexcept {
        if (!started) return;
        // This client is released, never reused. Stop and COM release suffice;
        // avoid an additional Reset during handoff to an existing capture user.
        const auto stopped = client->Stop();
        diagnostics::write("stream.stop.result", std::string(name) + " stop_hr=" + std::to_string(stopped));
        started = false;
    }
    void close(const char* label) noexcept {
        if (!client) return;
        diagnostics::write("stream.release.begin", label);
        stop();
        // Release all service references on their owning audio thread before
        // the parent client and before waiting for the process watcher/output.
        capture.Reset(); render.Reset(); client.Reset();
        diagnostics::write("stream.release.end", label);
    }

    void finish_render() {
        // Send silence through the downstream virtual cable before releasing
        // the stream. Releasing the client discards its remaining buffer.
        const auto deadline = GetTickCount64() + 250;
        while (started && GetTickCount64() < deadline) {
            UINT32 padding = 0;
            check(client->GetCurrentPadding(&padding), "Read shutdown padding");
            if (padding > buffer_frames) throw std::runtime_error("Invalid shutdown padding");
            const auto frames = buffer_frames - padding;
            if (frames) {
                BYTE* data = nullptr;
                check(render->GetBuffer(frames, &data), "Acquire shutdown silence");
                check(render->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT), "Write shutdown silence");
            }
            WaitForSingleObject(ready, 10);
        }
        stop();
    }

    void initialize(bool is_capture, bool process_loopback) {
        auto wave = format();
        DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (is_capture) flags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        if (process_loopback) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
        diagnostics::write("stream.initialize.begin", std::string("name=") + name +
            " shared=1 capture=" + std::to_string(is_capture) +
            " process_loopback=" + std::to_string(process_loopback) +
            " flags=" + std::to_string(flags) + " sample_rate=48000 channels=2");
        // Capture uses shared event mode with both timing arguments zero.
        // Render uses a 60 ms shared timer buffer, refilled on capture wakes or
        // the 10 ms fallback. The former 22 ms device minimum could underrun
        // after a single scheduling delay under load. Query the actual size.
        const auto initialized = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
            is_capture ? 0 : 600000, 0, &wave, nullptr);
        check(initialized, (std::string("Initialize ") + name + " 48 kHz stereo audio stream").c_str());
        if (is_capture) check(client->SetEventHandle(ready), "Set audio wake event");
        check(client->GetBufferSize(&buffer_frames), "Read audio buffer size");
        if (is_capture) check(client->GetService(IID_PPV_ARGS(&capture)), "Get capture client");
        else check(client->GetService(IID_PPV_ARGS(&render)), "Get render client");
        diagnostics::write("stream.initialize.end", std::string("name=") + name +
            " buffer_frames=" + std::to_string(buffer_frames));
    }
    void start() {
        diagnostics::write("stream.start.begin", name);
        check(client->Start(), (std::string("Start ") + name + " audio stream").c_str());
        started = true;
        diagnostics::write("stream.start.end", name);
    }
    void drain(AudioQueue& queue) {
        // Bounded per iteration so device errors/backlogs cannot starve Stop.
        for (unsigned packet = 0; packet < 128; ++packet) {
            UINT32 next = 0;
            check(capture->GetNextPacketSize(&next), "Read capture packet size");
            if (!next) break;
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            check(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr), "Read capture audio");
            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) ++discontinuities;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) silent_frames += frames;
            captured_frames += frames;
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data) {
                const auto* samples = reinterpret_cast<const float*>(data);
                for (UINT32 i = 0; i < frames; ++i)
                    if (std::abs(samples[i * channels]) > 0.00001f || std::abs(samples[i * channels + 1]) > 0.00001f)
                        ++signal_frames;
            }
            queue.push(reinterpret_cast<const float*>(data), frames,
                (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
            check(capture->ReleaseBuffer(frames), "Release capture packet");
        }
    }
};

std::wstring error_message(const char* text) {
    int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (!count) return L"未知音频错误";
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), count);
    result.pop_back();
    return result;
}
}

AudioEngine::AudioEngine() : stop_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
    if (!stop_event_) check(HRESULT_FROM_WIN32(GetLastError()), "Create stop event");
}
AudioEngine::~AudioEngine() { stop(); CloseHandle(stop_event_); }
void AudioEngine::mark(Stage stage) noexcept { stage_ = stage; progress_tick_ = GetTickCount64(); }
const char* AudioEngine::stage() const {
    static constexpr const char* names[] = {"idle", "validate_source", "open_output", "open_microphone", "open_process_loopback", "prefill",
        "read_music", "read_microphone", "read_padding", "mix", "write_output", "release_music", "release_microphone", "release_output", "released"};
    return names[static_cast<unsigned>(stage_.load())];
}
void AudioEngine::set_status(std::wstring text) {
    std::lock_guard lock(mutex_);
    status_ = std::move(text);
}
std::wstring AudioEngine::status() const { std::lock_guard lock(mutex_); return status_; }
void AudioEngine::settings(const MixSettings& value) { std::lock_guard lock(mutex_); settings_ = value; }
MixSettings AudioEngine::settings() const { std::lock_guard lock(mutex_); return settings_; }

void AudioEngine::start(AudioSelection selection) {
    stop();
    ResetEvent(stop_event_); stop_requested_ = false;
    running_ = false;
    mic_frames_ = music_frames_ = mic_signal_frames_ = rendered_frames_ = 0;
    mark(Stage::validate);
    diagnostics::write("engine.start", L"source=" + selection.music_executable + L" pid=" + std::to_wstring(selection.music_pid) +
        L" microphone=" + selection.mic_id + L" output=" + selection.output_id);
    const auto controls = settings();
    diagnostics::write("engine.initial_settings", "music_gain=" + std::to_string(controls.music_gain) +
        " mic_gain=" + std::to_string(controls.mic_gain) + " music_muted=" + std::to_string(controls.music_muted) +
        " mic_muted=" + std::to_string(controls.mic_muted) + " ducking=" + std::to_string(controls.ducking));
    active_ = true;
    dropped_frames_ = 0;
    music_starved_frames_ = mic_starved_frames_ = 0;
    max_wake_gap_us_ = max_process_check_us_ = max_render_us_ = 0;
    max_capture_drain_us_ = startup_dropped_frames_ = empty_render_wakes_ = 0;
    music_discontinuities_ = mic_discontinuities_ = music_silent_frames_ = 0;
    render_buffer_frames_ = 0;
    mmcss_registered_ = false;
    set_status(L"正在初始化音频设备…");
    try { worker_ = std::thread(&AudioEngine::run, this, std::move(selection)); }
    catch (...) { active_ = false; throw; }
}
void AudioEngine::request_stop() noexcept {
    if (active_ && !stop_requested_.exchange(true)) diagnostics::write("engine.stop.request", stage());
    SetEvent(stop_event_);
}
void AudioEngine::stop() {
    request_stop();
    if (worker_.joinable()) worker_.join();
    active_ = false;
    running_ = false;
    music_level_ = mic_level_ = output_level_ = 0;
}

void AudioEngine::run(AudioSelection selection) {
    try {
        ComScope com;
        AudioTask audio_task;
        mmcss_registered_ = audio_task.handle != nullptr;
        if (!selection.music_pid || !process_matches(selection.music_pid, selection.music_executable))
            throw std::runtime_error("Music application changed or exited. Refresh and select it again.");
        // Declare the watcher first so every stream releases before its join,
        // including exception unwinding after a device/source failure.
        ProcessWatch source_watch(selection.music_pid, selection.music_executable,
                                  stop_event_, max_process_check_us_);
        Stream output("output"), music("music");
        std::unique_ptr<CaptureStream> microphone;
        mark(Stage::output_open);
        output.client = activate_endpoint(selection.output_id, eRender);
        output.initialize(false, false);
        render_buffer_frames_ = output.buffer_frames;
        diagnostics::write("stream.output.ready", "buffer_frames=" + std::to_string(output.buffer_frames));
        mark(Stage::mic_open);
        microphone = std::make_unique<CaptureStream>(device_by_id(selection.mic_id, eCapture).Get(),
            CaptureTiming::event, "microphone");
        diagnostics::write("stream.microphone.ready", "shared=1 frames=" + std::to_string(microphone->buffer_frames()));
        mark(Stage::music_open);
        music.client = activate_process(selection.music_pid, stop_event_);
        if (music.client && WaitForSingleObject(stop_event_, 0) != WAIT_OBJECT_0) {
            music.initialize(true, true);
            // Both capture queues and all scratch buffers are allocated before Start.
            AudioQueue music_queue(sample_rate * 120 / 1000), mic_queue(sample_rate * 120 / 1000);
            const CaptureStream::Consumer collect_microphone = [&](std::span<const float> samples, UINT32 frames) {
                mic_queue.push(samples.data(), frames, samples.empty());
            };
            Mixer mixer;
            std::vector<float> music_samples(output.buffer_frames * channels);
            std::vector<float> mic_samples(output.buffer_frames * channels);
            music.start();
            microphone->start();
            mark(Stage::prefill);
            // Process loopback can deliver its first packet later than the mic.
            // Wait for actual queued frames, rather than assuming 30 ms elapsed
            // means both captures are ready. A paused source must still start.
            const auto prefill_deadline = GetTickCount64() + 100;
            const auto prefill_frames = sample_rate * 30 / 1000;
            HANDLE capture_waits[] = {stop_event_, music.ready, microphone->ready()};
            while (GetTickCount64() < prefill_deadline &&
                WaitForSingleObject(stop_event_, 0) != WAIT_OBJECT_0) {
                music.drain(music_queue);
                microphone->drain(collect_microphone);
                if (music_queue.size_frames() >= prefill_frames &&
                    mic_queue.size_frames() >= prefill_frames) break;
                const auto wait = WaitForMultipleObjects(3, capture_waits, FALSE, 5);
                if (wait == WAIT_FAILED) check(HRESULT_FROM_WIN32(GetLastError()), "Wait for capture prefill");
            }
            startup_dropped_frames_ = music_queue.dropped_frames() + mic_queue.dropped_frames();
            BYTE* initial = nullptr;
            check(output.render->GetBuffer(output.buffer_frames, &initial), "Prime output buffer");
            check(output.render->ReleaseBuffer(output.buffer_frames, AUDCLNT_BUFFERFLAGS_SILENT), "Prime output silence");
            output.start();
            running_ = true;
            diagnostics::write("engine.running", "shared=1 sample_rate=48000 channels=2");
            set_status(L"正在混音 → CABLE Output。请在语音客户端选择该录音设备。配戴耳机避免回声。");
            HANDLE waits[] = {stop_event_, music.ready, microphone->ready(), output.ready};
            auto last_wake = microseconds();
            for (;;) {
                auto wait = WaitForMultipleObjects(4, waits, FALSE, 10);
                if (wait == WAIT_OBJECT_0) break;
                if (wait == WAIT_FAILED) check(HRESULT_FROM_WIN32(GetLastError()), "Wait for audio");
                const auto woke = microseconds();
                update_max(max_wake_gap_us_, woke - last_wake);
                last_wake = woke;
                // Never reattach to a new PID automatically; restart is explicit.
                source_watch.check_source();
                const auto drain_begin = microseconds();
                mark(Stage::music_read);
                music.drain(music_queue);
                mark(Stage::mic_read);
                microphone->drain(collect_microphone);
                const auto microphone_stats = microphone->statistics();
                music_frames_ = music.captured_frames; mic_frames_ = microphone_stats.frames;
                mic_signal_frames_ = microphone_stats.signal_frames;
                update_max(max_capture_drain_us_, microseconds() - drain_begin);
                music_discontinuities_ = music.discontinuities;
                mic_discontinuities_ = microphone_stats.discontinuities;
                music_silent_frames_ = music.silent_frames;
                UINT32 padding = 0;
                mark(Stage::padding);
                check(output.client->GetCurrentPadding(&padding), "Read output padding");
                if (padding > output.buffer_frames) throw std::runtime_error("Invalid output padding");
                if (!padding) ++empty_render_wakes_;
                const auto frames = output.buffer_frames - padding;
                if (!frames) continue;
                const auto render_begin = microseconds();
                mark(Stage::mixing);
                auto m = std::span<float>(music_samples.data(), frames * channels);
                auto v = std::span<float>(mic_samples.data(), frames * channels);
                const auto controls = settings();
                const auto missing_music = music_queue.pop_clocked(m);
                const auto missing_mic = mic_queue.pop_clocked(v);
                if (!controls.music_muted && controls.music_gain > 0)
                    music_starved_frames_ += missing_music;
                if (!controls.mic_muted && controls.mic_gain > 0)
                    mic_starved_frames_ += missing_mic;
                BYTE* data = nullptr;
                mark(Stage::output_write);
                check(output.render->GetBuffer(frames, &data), "Acquire virtual cable output");
                const auto levels = mixer.mix(m, v,
                    {reinterpret_cast<float*>(data), frames * channels}, controls);
                check(output.render->ReleaseBuffer(frames, 0), "Write virtual cable output");
                rendered_frames_ += frames;
                update_max(max_render_us_, microseconds() - render_begin);
                music_level_ = levels.music;
                mic_level_ = levels.mic;
                output_level_ = levels.output;
                duck_gain_ = levels.duck_gain;
                dropped_frames_ = music_queue.dropped_frames() + mic_queue.dropped_frames();
            }
            running_ = false;
            mark(Stage::release_mic); microphone->close();
            mark(Stage::release_music); music.close("music");
            mark(Stage::release_output);
            output.finish_render();
            output.close("output");
        }
        set_status(L"已停止。虚拟麦克风不再接收本程序的音频。");
    } catch (const std::exception& error) {
        diagnostics::write("engine.error", std::string("stage=") + stage() + " " + error.what());
        set_status(L"音频已停止：" + error_message(error.what()));
    } catch (...) {
        diagnostics::write("engine.error", std::string("stage=") + stage() + " unknown_exception");
        set_status(L"音频已停止：未知错误。请重新选择设备后启动。");
    }
    music_level_ = mic_level_ = output_level_ = 0;
    running_ = false;
    mark(Stage::released);
    diagnostics::write("engine.stopped", "all_stream_scopes_released=1 mic_frames=" + std::to_string(mic_frames_.load()));
    active_ = false;
}
} // namespace partymic
