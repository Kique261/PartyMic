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
    ~AudioEngine();
    void start(AudioSelection selection);
    void stop();
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
private:
    void run(AudioSelection selection);
    void set_status(std::wstring text);
    std::thread worker_;
    HANDLE stop_event_ = nullptr;
    std::atomic<bool> active_ = false;
    std::atomic<float> music_level_ = 0, mic_level_ = 0, output_level_ = 0, duck_gain_ = 1;
    std::atomic<unsigned long long> dropped_frames_ = 0;
    std::atomic<unsigned long long> music_starved_frames_ = 0, mic_starved_frames_ = 0;
    mutable std::mutex mutex_;
    MixSettings settings_;
    std::wstring status_ = L"就绪：请选择音乐软件、实体麦克风和虚拟声卡。";
};
} // namespace partymic
