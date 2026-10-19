// Opt-in Windows hardware integration test. Never records/saves microphone audio.
// Own child processes produce quiet tones; only scalar levels/spectra are logged.
#include "windows/audio_engine.h"
#include "windows/devices.h"
#include <audioclient.h>
#include <avrt.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <fcntl.h>
#include <io.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <exception>
#include <utility>

using namespace partymic;
using Microsoft::WRL::ComPtr;
namespace {
constexpr double pi = 3.14159265358979323846;
struct ComScope {
    ComScope() { check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM"); }
    ~ComScope() { CoUninitialize(); }
};
struct Handle {
    HANDLE value = nullptr;
    ~Handle() {
        if (value)
            CloseHandle(value);
    }
};
struct AudioTask {
    DWORD index = 0;
    HANDLE value = AvSetMmThreadCharacteristicsW(L"Audio", &index);
    AudioTask() {
        if (value) AvSetMmThreadPriority(value, AVRT_PRIORITY_HIGH);
    }
    ~AudioTask() { if (value) AvRevertMmThreadCharacteristics(value); }
};
WAVEFORMATEX wave_format() {
    WAVEFORMATEX f{};
    f.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    f.nChannels = 2;
    f.nSamplesPerSec = 48000;
    f.wBitsPerSample = 32;
    f.nBlockAlign = 8;
    f.nAvgBytesPerSec = 384000;
    return f;
}
ComPtr<IAudioClient> endpoint_client(const std::wstring &id) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)),
          "Device enumerator");
    ComPtr<IMMDevice> device;
    check(enumerator->GetDevice(id.c_str(), &device), "Device");
    ComPtr<IAudioClient> client;
    check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                           reinterpret_cast<void **>(client.GetAddressOf())),
          "Endpoint activation");
    return client;
}
void initialize(IAudioClient *client) {
    auto f = wave_format();
    check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                             AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                             1000000, 0, &f, nullptr),
          "Test audio initialize");
}
int tone(double frequency, const wchar_t *event_name) {
    AudioTask audio_task;
    Handle stop{OpenEventW(SYNCHRONIZE, FALSE, event_name)};
    if (!stop.value)
        throw std::runtime_error("Tone stop event unavailable");
    const auto id = default_endpoint_id(eRender, eMultimedia);
    for (const auto &e : endpoints(eRender))
        if (e.id == id && is_virtual_cable(e.name))
            throw std::runtime_error("Tone refuses default virtual cable output");
    auto client = endpoint_client(id);
    initialize(client.Get());
    ComPtr<IAudioRenderClient> render;
    check(client->GetService(IID_PPV_ARGS(&render)), "Test render service");
    UINT32 capacity = 0;
    check(client->GetBufferSize(&capacity), "Test render capacity");
    check(client->Start(), "Tone start");
    unsigned long long position = 0;
    const auto deadline = GetTickCount64() + 120000;
    while (GetTickCount64() < deadline && WaitForSingleObject(stop.value, 3) == WAIT_TIMEOUT) {
        UINT32 padding = 0;
        check(client->GetCurrentPadding(&padding), "Tone padding");
        const auto frames = capacity - padding;
        if (!frames)
            continue;
        BYTE *bytes = nullptr;
        check(render->GetBuffer(frames, &bytes), "Tone buffer");
        auto *data = reinterpret_cast<float *>(bytes);
        for (UINT32 i = 0; i < frames; ++i, ++position) {
            // 2.5% full-scale source, intentionally quiet on physical playback.
            const float sample = static_cast<float>(0.025 * std::sin(2 * pi * frequency * position / 48000));
            data[2 * i] = data[2 * i + 1] = sample;
        }
        check(render->ReleaseBuffer(frames, 0), "Tone release");
    }
    check(client->Stop(), "Tone stop");
    return 0;
}
struct ChildTone {
    Handle stop;
    PROCESS_INFORMATION process{};
    explicit ChildTone(unsigned frequency) {
        std::wstring name = L"Local\\PartyMicSmoke-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                            std::to_wstring(frequency);
        stop.value = CreateEventW(nullptr, TRUE, FALSE, name.c_str());
        if (!stop.value)
            throw std::runtime_error("Create tone stop event");
        wchar_t path[32768];
        if (!GetModuleFileNameW(nullptr, path, 32768))
            throw std::runtime_error("Test executable path");
        std::wstring command =
            L"\"" + std::wstring(path) + L"\" --tone " + std::to_wstring(frequency) + L" \"" + name + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        if (!CreateProcessW(path, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                            &startup, &process))
            throw std::runtime_error("Start own tone child");
        CloseHandle(process.hThread);
    }
    void finish() {
        if (!process.hProcess)
            return;
        SetEvent(stop.value);
        const auto wait = WaitForSingleObject(process.hProcess, 5000);
        if (wait != WAIT_OBJECT_0)
            throw std::runtime_error("Tone child failed to stop");
        DWORD code = 1;
        GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hProcess);
        process.hProcess = nullptr;
        if (code)
            throw std::runtime_error("Tone child returned error");
    }
    ~ChildTone() {
        if (process.hProcess) {
            SetEvent(stop.value);
            WaitForSingleObject(process.hProcess, 5000);
            CloseHandle(process.hProcess);
        }
    }
};
struct Metrics {
    unsigned long long frames = 0;
    double squares = 0, peak = 0, real[2]{}, imaginary[2]{}, tone_power[2]{};
    unsigned block_frames = 0, blocks = 0;
    unsigned long long zero_frames = 0, zero_run = 0, longest_zero_run = 0;
    unsigned discontinuities = 0;
    void add(const float *data, UINT32 count, DWORD flags) {
        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
            ++discontinuities;
        for (UINT32 i = 0; i < count; ++i, ++frames) {
            const double x = (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                                 ? 0
                                 : (static_cast<double>(data[2 * i]) + data[2 * i + 1]) / 2;
            if (!std::isfinite(x))
                throw std::runtime_error("Non-finite cable sample");
            squares += x * x;
            peak = std::max(peak, std::abs(x));
            if (std::abs(x) < 1e-8) {
                ++zero_frames;
                longest_zero_run = std::max(longest_zero_run, ++zero_run);
            } else
                zero_run = 0;
            for (int k = 0; k < 2; ++k) {
                const double phase = 2 * pi * (k == 0 ? 997 : 1499) * block_frames / 48000;
                real[k] += x * std::cos(phase);
                imaginary[k] += x * std::sin(phase);
            }
            if (++block_frames == 4800) {
                // Average spectral power in 100 ms blocks. A phase shift between
                // blocks must not cancel an otherwise present test tone.
                for (int k = 0; k < 2; ++k) {
                    tone_power[k] += 4 * (real[k] * real[k] + imaginary[k] * imaginary[k]) / (4800.0 * 4800);
                    real[k] = imaginary[k] = 0;
                }
                ++blocks;
                block_frames = 0;
            }
        }
    }
    double rms() const { return frames ? std::sqrt(squares / frames) : 0; }
    double amplitude(int k) const { return blocks ? std::sqrt(tone_power[k] / blocks) : 0; }
};
class CableCapture {
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    Handle stop_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::mutex mutex_;
    Metrics accumulated_;
    std::exception_ptr failure_;
    std::thread worker_;

  public:
    explicit CableCapture(const std::wstring &id) : client_(endpoint_client(id)) {
        initialize(client_.Get());
        check(client_->GetService(IID_PPV_ARGS(&capture_)), "Cable capture service");
        if (!stop_.value) throw std::runtime_error("Cable observer stop event");
        UINT32 capacity = 0;
        check(client_->GetBufferSize(&capacity), "Cable observer capacity");
        std::vector<float> scratch(capacity * channels);
        check(client_->Start(), "Cable capture start");
        // Keep draining while the test's main thread prints or changes controls.
        // Otherwise slow console/PowerShell logging can overrun the observer and
        // masquerade as a gap in the mixer. Never discard discontinuity flags.
        worker_ = std::thread([this, scratch = std::move(scratch)]() mutable {
            try {
                ComScope com;
                AudioTask audio_task;
                while (WaitForSingleObject(stop_.value, 3) == WAIT_TIMEOUT) {
                    for (unsigned n = 0; n < 128; ++n) {
                        UINT32 count = 0;
                        check(capture_->GetNextPacketSize(&count), "Cable packet size");
                        if (!count) break;
                        BYTE* bytes = nullptr;
                        DWORD flags = 0;
                        check(capture_->GetBuffer(&bytes, &count, &flags, nullptr, nullptr), "Cable read");
                        if (count * channels > scratch.size()) {
                            capture_->ReleaseBuffer(count);
                            throw std::runtime_error("Cable observer packet exceeds capacity");
                        }
                        const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                        if (!silent) std::copy_n(reinterpret_cast<float*>(bytes), count * channels, scratch.data());
                        check(capture_->ReleaseBuffer(count), "Cable release");
                        // Analysis happens after ReleaseBuffer; the capture API
                        // is not held while calculating spectra.
                        std::lock_guard lock(mutex_);
                        accumulated_.add(silent ? nullptr : scratch.data(), count, flags);
                    }
                }
            } catch (...) {
                std::lock_guard lock(mutex_);
                failure_ = std::current_exception();
            }
        });
    }
    ~CableCapture() {
        SetEvent(stop_.value);
        if (worker_.joinable()) worker_.join();
        client_->Stop();
    }
    Metrics measure(DWORD duration) {
        Sleep(duration);
        std::lock_guard lock(mutex_);
        if (failure_) std::rethrow_exception(failure_);
        return std::exchange(accumulated_, Metrics{});
    }
    Metrics settled_measure() {
        measure(400);
        return measure(1600);
    }
};
void require(bool value, const char *text) {
    if (!value)
        throw std::runtime_error(text);
    std::wcout << L"PASS " << text << std::endl;
}
void report(const wchar_t *label, const Metrics &m) {
    std::wcout << label << L" frames=" << m.frames << L" rms=" << m.rms() << L" peak=" << m.peak
               << L" selected_997=" << m.amplitude(0) << L" other_1499=" << m.amplitude(1)
               << L" discontinuities=" << m.discontinuities << L" zero_frames=" << m.zero_frames
               << L" longest_zero_run=" << m.longest_zero_run << std::endl;
}
void report_engine(const AudioEngine &engine) {
    std::wcout << L"DIAGNOSTICS render_buffer_frames=" << engine.render_buffer_frames()
               << L" mmcss_registered=" << engine.mmcss_registered()
               << L" max_wake_gap_us=" << engine.max_wake_gap_us()
               << L" max_process_check_us=" << engine.max_process_check_us()
               << L" max_render_us=" << engine.max_render_us()
               << L" max_capture_drain_us=" << engine.max_capture_drain_us()
               << L" startup_dropped_frames=" << engine.startup_dropped_frames()
               << L" empty_render_wakes=" << engine.empty_render_wakes()
               << L" music_discontinuities=" << engine.music_discontinuities()
               << L" mic_discontinuities=" << engine.mic_discontinuities()
               << L" music_silent_frames=" << engine.music_silent_frames() << std::endl;
    std::wcout << L"INPUT music_starved_frames=" << engine.music_starved_frames()
               << L" mic_starved_frames=" << engine.mic_starved_frames()
               << L" dropped_frames=" << engine.dropped_frames() << std::endl;
}
void analyzer_check() {
    std::vector<float> samples(76800 * 2);
    for (unsigned i = 0; i < 76800; ++i) {
        // Equal energy before/after a half-turn phase shift; a single coherent
        // sum across the whole window would cancel the tone almost completely.
        const float x =
            static_cast<float>(0.025 * std::sin(2 * pi * 997 * i / 48000 + (i >= 38400 ? pi : 0)));
        samples[2 * i] = samples[2 * i + 1] = x;
    }
    Metrics m;
    m.add(samples.data(), 76800, 0);
    require(std::abs(m.amplitude(0) - 0.025) < 0.0001 && m.amplitude(1) < m.amplitude(0) * 0.05 &&
                std::abs(m.rms() - 0.025 / std::sqrt(2.0)) < 0.00001,
            "Spectral measurement preserves energy across a phase inversion");
}
void await_running(AudioEngine &engine) {
    const auto end = GetTickCount64() + 10000;
    while (GetTickCount64() < end) {
        if (!engine.active()) {
            std::wcerr << engine.status() << std::endl;
            throw std::runtime_error("Engine stopped during start");
        }
        if (engine.status().find(L"CABLE Output") != std::wstring::npos)
            return;
        Sleep(10);
    }
    throw std::runtime_error("Engine startup deadline");
}
AudioSelection selection_for(DWORD pid, const std::wstring &executable, std::wstring &capture) {
    AudioSelection selection{pid, executable, {}, {}};
    for (const auto &e : endpoints(eCapture)) {
        if (e.name.find(L"Brio 90") != std::wstring::npos)
            selection.mic_id = e.id;
        if (e.name.find(L"CABLE Output") != std::wstring::npos)
            capture = e.id;
    }
    for (const auto &e : endpoints(eRender))
        if (is_virtual_cable(e.name)) {
            selection.output_id = e.id;
            break;
        }
    require(!selection.mic_id.empty(), "Brio 90 microphone found");
    require(!selection.output_id.empty() && !capture.empty(), "VB-CABLE stereo endpoints found");
    return selection;
}
int player_test(const std::wstring &executable, unsigned seconds, DWORD explicit_pid = 0,
                bool diagnostic = false) {
    DWORD pid = explicit_pid;
    if (!pid)
        for (const auto &app : audio_applications())
            if (_wcsicmp(app.executable.c_str(), executable.c_str()) == 0) {
                pid = app.pid;
                break;
            }
    require(pid != 0, "Requested player has an audio session");
    std::wcout << L"player=" << executable << L" pid=" << pid << L" soak_seconds=" << seconds << std::endl;
    std::wstring capture;
    const auto selection = selection_for(pid, executable, capture);
    AudioEngine engine;
    MixSettings settings;
    settings.mic_muted = true;
    settings.ducking = false;
    engine.settings(settings);
    CableCapture reader(capture);
    engine.start(selection);
    await_running(engine);
    const auto music = reader.settled_measure();
    report(L"player_music_only", music);
    report_engine(engine);
    require(music.frames > 48000 && music.rms() > 0.00001 && music.peak <= 1.001,
            "Actual player audio reaches virtual microphone");
    settings.music_muted = true;
    engine.settings(settings);
    const auto muted = reader.settled_measure();
    report(L"player_both_muted", muted);
    require(muted.rms() < 0.00001, "Actual player can be muted independently");
    settings.mic_muted = false;
    engine.settings(settings);
    const auto mic = reader.settled_measure();
    report(L"player_microphone_only", mic);
    std::wcout << L"MIC_ACTIVITY_OBSERVED=" << (mic.rms() > 0.00001) << std::endl;
    settings.music_muted = false;
    engine.settings(settings);
    reader.measure(400);
    DWORD handles_before = 0, handles_after = 0;
    require(GetProcessHandleCount(GetCurrentProcess(), &handles_before), "Read initial own handle count");
    const auto begin = GetTickCount64();
    while (GetTickCount64() - begin < seconds * 1000ULL) {
        const auto m = reader.measure(1000);
        require(engine.active() && m.frames > 24000 && m.peak <= 1.001,
                "Combined player/microphone stream remains active and bounded");
        std::wcout << L"soak_elapsed_ms=" << GetTickCount64() - begin << L" rms=" << m.rms() << L" peak="
                   << m.peak << L" dropped_frames=" << engine.dropped_frames() << L" music_starved_frames="
                   << engine.music_starved_frames() << L" mic_starved_frames=" << engine.mic_starved_frames()
                   << L" longest_zero_run=" << m.longest_zero_run << L" capture_discontinuities="
                   << m.discontinuities << std::endl;
        report_engine(engine);
        if (explicit_pid && !diagnostic)
            require(m.longest_zero_run < 48 && m.discontinuities == 0 &&
                        engine.music_starved_frames() == 0 && engine.dropped_frames() == 0,
                    "Continuous test tone has no >=1 ms gaps, capture loss, input starvation or overflow");
    }
    require(GetProcessHandleCount(GetCurrentProcess(), &handles_after), "Read final own handle count");
    std::wcout << L"soak_handles_before=" << handles_before << L" after=" << handles_after
               << L" dropped_frames=" << engine.dropped_frames() << std::endl;
    require(handles_after <= handles_before + 4, "Player soak has no sustained handle growth");
    engine.stop();
    const auto stopped = reader.settled_measure();
    report(L"player_stopped", stopped);
    require(stopped.rms() < 0.00001, "Stop removes actual player and microphone output");
    std::wcout << (diagnostic ? L"ALL CONTINUITY DIAGNOSTICS COMPLETED"
                            : (explicit_pid ? L"ALL SYNTHETIC SOAK CHECKS PASSED" : L"ALL ACTUAL PLAYER CHECKS PASSED"))
               << std::endl;
    return 0;
}
int run() {
    analyzer_check();
    ChildTone selected(997), other(1499);
    std::wstring capture;
    const auto selection = selection_for(selected.process.dwProcessId, L"windows_audio_smoke.exe", capture);
    Sleep(700);
    AudioEngine engine;
    MixSettings settings;
    settings.ducking = false;
    settings.mic_muted = true;
    settings.music_gain = 1;
    engine.settings(settings);
    CableCapture reader(capture);
    engine.start(selection);
    await_running(engine);
    const auto music = reader.settled_measure();
    report(L"music_only", music);
    report_engine(engine);
    require(music.frames > 48000, "Cable capture receives frames");
    require(music.amplitude(0) > 0.003, "Selected application tone reaches cable");
    require(music.amplitude(1) < music.amplitude(0) * 0.05, "Other application tone excluded (>26 dB)");
    require(music.peak <= 1.001, "Output samples bounded");
    require(music.longest_zero_run < 48, "Selected tone has no silence gap of 1 ms or longer");
    settings.music_gain = 0.25f;
    engine.settings(settings);
    const auto quarter = reader.settled_measure();
    report(L"music_quarter", quarter);
    report_engine(engine);
    require(quarter.rms() > music.rms() * 0.22 && quarter.rms() < music.rms() * 0.28,
            "Music gain affects measured RMS (25% +/-3%)");
    require(quarter.longest_zero_run < 48, "Quarter-volume tone has no silence gap of 1 ms or longer");
    settings.music_muted = true;
    engine.settings(settings);
    const auto muted = reader.settled_measure();
    report(L"both_muted", muted);
    require(muted.rms() < 0.00001, "Both inputs muted produces silence");
    settings.mic_muted = false;
    engine.settings(settings);
    const auto microphone = reader.settled_measure();
    report(L"microphone_only", microphone);
    require(engine.active(), "Brio microphone stream remains active");
    // Silence in the room is valid: only assert acoustic activity after it is observed.
    std::wcout << L"MIC_ACTIVITY_OBSERVED=" << (microphone.rms() > 0.00001) << std::endl;
    settings.music_muted = false;
    engine.settings(settings);
    const auto combined = reader.settled_measure();
    report(L"combined", combined);
    report_engine(engine);
    require(combined.amplitude(0) > quarter.amplitude(0) * 0.5 && combined.peak <= 1.001,
            "Music and microphone can run together");
    require(combined.longest_zero_run < 48, "Combined tone has no silence gap of 1 ms or longer");
    std::wcout << L"dropped_frames=" << engine.dropped_frames() << std::endl;
    std::wcout << L"music_starved_frames=" << engine.music_starved_frames() << L" mic_starved_frames="
               << engine.mic_starved_frames() << std::endl;
    engine.stop();
    const auto stopped = reader.settled_measure();
    report(L"stopped", stopped);
    require(stopped.rms() < 0.00001, "Stop removes cable output");
    DWORD handles_before = 0, handles_after = 0;
    check(GetProcessHandleCount(GetCurrentProcess(), &handles_before) ? S_OK
                                                                      : HRESULT_FROM_WIN32(GetLastError()),
          "Count own handles before restarts");
    for (int i = 0; i < 20; ++i) {
        engine.start(selection);
        engine.stop();
    }
    require(!engine.active(), "20 immediate start/stop cycles finish");
    settings.mic_muted = true;
    engine.settings(settings);
    for (int i = 0; i < 50; ++i) {
        engine.start(selection);
        await_running(engine);
        reader.measure(100);
        engine.stop();
    }
    require(!engine.active(), "50 full stream restart cycles finish");
    check(GetProcessHandleCount(GetCurrentProcess(), &handles_after) ? S_OK
                                                                     : HRESULT_FROM_WIN32(GetLastError()),
          "Count own handles after restarts");
    std::wcout << L"own_handles_before=" << handles_before << L" after=" << handles_after << std::endl;
    require(handles_after <= handles_before + 12, "No sustained handle growth across restarts");
    auto invalid_output = selection;
    invalid_output.output_id = default_endpoint_id(eRender, eMultimedia);
    engine.start(invalid_output);
    auto reject_deadline = GetTickCount64() + 5000;
    while (engine.active() && GetTickCount64() < reject_deadline)
        Sleep(10);
    std::wcout << L"physical_output_status=" << engine.status() << std::endl;
    require(!engine.active() && engine.status().find(L"Output must") != std::wstring::npos,
            "Physical playback endpoint rejected as mix output");
    engine.stop();
    engine.start(selection);
    await_running(engine);
    selected.finish();
    const auto deadline = GetTickCount64() + 5000;
    while (engine.active() && GetTickCount64() < deadline)
        reader.measure(50);
    std::wcout << L"source_exit_status=" << engine.status() << std::endl;
    require(!engine.active() && engine.status().find(L"exited") != std::wstring::npos,
            "Source exit stops capture with diagnostic");
    engine.stop();
    other.finish();
    std::wcout << L"ALL WINDOWS AUDIO SMOKE CHECKS PASSED" << std::endl;
    return 0;
}
int fidelity_test() {
    double reference = 0;
    for (unsigned hz : {1000U, 8000U, 12000U, 16000U, 20000U}) {
        ChildTone source(hz);
        std::wstring capture;
        const auto selection = selection_for(source.process.dwProcessId, L"windows_audio_smoke.exe", capture);
        Sleep(700);
        AudioEngine engine;
        MixSettings settings;
        settings.music_gain = 1;
        settings.mic_muted = true;
        settings.ducking = false;
        engine.settings(settings);
        CableCapture reader(capture);
        engine.start(selection);
        await_running(engine);
        const auto measured = reader.settled_measure();
        require(measured.frames > 48000 && measured.rms() > 0.00001,
                "Fidelity probe receives a nonzero signal");
        report_engine(engine);
        if (!reference)
            reference = measured.rms();
        const double relative_db = 20 * std::log10(measured.rms() / reference);
        const double absolute_db = 20 * std::log10(measured.rms() / (0.025 / std::sqrt(2.0)));
        std::wcout << L"FREQUENCY hz=" << hz << L" rms=" << measured.rms() << L" relative_to_1k_db="
                   << relative_db << L" absolute_db=" << absolute_db << L" dropped_frames="
                   << engine.dropped_frames() << L" music_starved_frames=" << engine.music_starved_frames()
                   << std::endl;
        require(std::abs(relative_db) < 0.5 && std::abs(absolute_db) < 0.5,
                "Process capture through actual cable preserves 1-20 kHz within 0.5 dB");
        require(measured.peak < 0.04 && engine.dropped_frames() == 0 && engine.music_starved_frames() == 0,
                "Fidelity probe is bounded without input loss");
        engine.stop();
        source.finish();
    }
    std::wcout << L"ALL WINDOWS AUDIO FIDELITY CHECKS PASSED" << std::endl;
    return 0;
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U8TEXT);
    try {
        ComScope com;
        if (argc == 4 && std::wstring(argv[1]) == L"--tone")
            return tone(std::stod(argv[2]), argv[3]);
        for (EDataFlow flow : {eRender, eCapture}) {
            std::wcout << (flow == eRender ? L"RENDER" : L"CAPTURE") << std::endl;
            for (const auto &e : endpoints(flow))
                std::wcout << e.name << L" | " << e.id << std::endl;
            for (ERole role : {eConsole, eMultimedia, eCommunications})
                std::wcout << L"default role=" << role << L" " << default_endpoint_id(flow, role)
                           << std::endl;
        }
        if (argc == 4 && std::wstring(argv[1]) == L"--player") {
            const int seconds = std::stoi(argv[3]);
            if (seconds < 1 || seconds > 3600)
                throw std::runtime_error("Player test duration must be 1..3600 seconds");
            return player_test(argv[2], static_cast<unsigned>(seconds));
        }
        if (argc == 3 && (std::wstring(argv[1]) == L"--soak" || std::wstring(argv[1]) == L"--diagnose")) {
            const bool diagnostic = std::wstring(argv[1]) == L"--diagnose";
            const int seconds = std::stoi(argv[2]);
            if (seconds < 1 || seconds > 90)
                throw std::runtime_error("Synthetic soak duration must be 1..90 seconds");
            ChildTone source(997);
            Sleep(700);
            const auto result = player_test(L"windows_audio_smoke.exe", static_cast<unsigned>(seconds),
                                            source.process.dwProcessId, diagnostic);
            source.finish();
            return result;
        }
        if (argc == 2 && std::wstring(argv[1]) == L"--run")
            return run();
        if (argc == 2 && std::wstring(argv[1]) == L"--fidelity")
            return fidelity_test();
        std::wcout << L"Use --run, --fidelity, --soak 90, --diagnose 30, or --player cloudmusic.exe 60. No recordings."
                   << std::endl;
        return 0;
    } catch (const std::exception &e) {
        std::wcout << L"FAIL " << e.what() << std::endl;
        return 1;
    }
}
