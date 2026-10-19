#pragma once
#include "core/mixer.h"
#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace partymic {
struct AudioSelection {
    DWORD music_pid = 0;
    std::wstring music_executable;
    std::wstring mic_id;
    std::wstring output_id;
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();
    void start(AudioSelection selection);
    void stop();
    // UI uses cooperative requests and polls active(), never joins a live worker.
    void request_stop() noexcept;
    bool stopping() const { return stop_requested_.load() && active(); }
    unsigned long long progress_tick() const { return progress_tick_.load(); }
    unsigned long long mic_frames() const { return mic_frames_.load(); }
    unsigned long long music_frames() const { return music_frames_.load(); }
    unsigned long long mic_signal_frames() const { return mic_signal_frames_.load(); }
    unsigned long long rendered_frames() const { return rendered_frames_.load(); }
    const char* stage() const;
    void settings(const MixSettings& value);
    MixSettings settings() const;
    std::wstring status() const;
    bool active() const { return active_.load(); }
    float music_level() const { return music_level_.load(); }
    float mic_level() const { return mic_level_.load(); }
    float output_level() const { return output_level_.load(); }
    float duck_gain() const { return duck_gain_.load(); }
    unsigned long long dropped_frames() const { return dropped_frames_.load(); }
    unsigned long long music_starved_frames() const { return music_starved_frames_.load(); }
    unsigned long long mic_starved_frames() const { return mic_starved_frames_.load(); }
    // Scalar diagnostics only: no audio, target-process handles or logging on
    // the streaming thread. Values remain available after stop().
    unsigned long long max_wake_gap_us() const { return max_wake_gap_us_.load(); }
    unsigned long long max_process_check_us() const { return max_process_check_us_.load(); }
    unsigned long long max_render_us() const { return max_render_us_.load(); }
    unsigned long long max_capture_drain_us() const { return max_capture_drain_us_.load(); }
    unsigned long long startup_dropped_frames() const { return startup_dropped_frames_.load(); }
    unsigned long long empty_render_wakes() const { return empty_render_wakes_.load(); }
    unsigned long long music_discontinuities() const { return music_discontinuities_.load(); }
    unsigned long long mic_discontinuities() const { return mic_discontinuities_.load(); }
    unsigned long long music_silent_frames() const { return music_silent_frames_.load(); }
    unsigned render_buffer_frames() const { return render_buffer_frames_.load(); }
    bool mmcss_registered() const { return mmcss_registered_.load(); }
private:
    enum class Stage { idle, validate, output_open, mic_open, music_open, prefill,
        music_read, mic_read, padding, mixing, output_write, release_music, release_mic, release_output, released };
    void mark(Stage stage) noexcept;
    void run(AudioSelection selection);
    void set_status(std::wstring text);
    std::thread worker_;
    HANDLE stop_event_ = nullptr;
    std::atomic<bool> stop_requested_ = false;
    std::atomic<Stage> stage_{Stage::idle};
    std::atomic<unsigned long long> progress_tick_ = 0, mic_frames_ = 0, music_frames_ = 0;
    std::atomic<unsigned long long> mic_signal_frames_ = 0, rendered_frames_ = 0;
    std::atomic<bool> active_ = false;
    std::atomic<float> music_level_ = 0, mic_level_ = 0, output_level_ = 0, duck_gain_ = 1;
    std::atomic<unsigned long long> dropped_frames_ = 0;
    std::atomic<unsigned long long> music_starved_frames_ = 0, mic_starved_frames_ = 0;
    std::atomic<unsigned long long> max_wake_gap_us_ = 0, max_process_check_us_ = 0, max_render_us_ = 0;
    std::atomic<unsigned long long> max_capture_drain_us_ = 0, startup_dropped_frames_ = 0, empty_render_wakes_ = 0;
    std::atomic<unsigned long long> music_discontinuities_ = 0, mic_discontinuities_ = 0;
    std::atomic<unsigned long long> music_silent_frames_ = 0;
    std::atomic<unsigned> render_buffer_frames_ = 0;
    std::atomic<bool> mmcss_registered_ = false;
    mutable std::mutex mutex_;
    MixSettings settings_;
    std::wstring status_ = L"就绪：请选择音乐软件、实体麦克风和虚拟声卡。";
};
} // namespace partymic
