#pragma once
#include <windows.h>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace partymic {
// Explicit local recordings only. One capture or playback worker at a time.
class AudioPreview {
public:
    ~AudioPreview();
    void record(std::wstring capture_id, bool mixed, unsigned seconds = 15);
    void play(std::wstring output_id);
    void stop();
    void request_stop() noexcept;
    bool stopping() const { return stop_.load() && busy(); }
    unsigned long long progress_tick() const { return progress_tick_.load(); }
    void microphone(float gain, bool muted) { mic_gain_ = gain; mic_muted_ = muted; }
    void playback_gain(float gain) { playback_gain_ = gain; }
    bool busy() const { return mode_.load() != 0; }
    bool recording() const { return mode_.load() == 1; }
    bool playing() const { return mode_.load() == 2; }
    float level() const { return level_.load(); }
    std::wstring status() const;
    std::filesystem::path last_file() const;
private:
    void capture(std::wstring id, bool mixed, unsigned seconds);
    void playback(std::wstring id, std::filesystem::path file);
    void message(std::wstring text);
    std::thread worker_;
    std::atomic<bool> stop_ = false, mic_muted_ = false;
    std::atomic<int> mode_ = 0;
    std::atomic<unsigned long long> progress_tick_ = 0;
    std::atomic<float> mic_gain_ = 1, playback_gain_ = 0.7f, level_ = 0;
    mutable std::mutex mutex_;
    std::wstring status_ = L"录音保存在桌面的 PartyMic录音 文件夹。试听前会停止混音。";
    std::filesystem::path last_file_;
};
}
