#include "windows/diagnostics.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace partymic::diagnostics {
namespace {
constexpr size_t queue_limit = 256, file_limit = 2 * 1024 * 1024, keep_files = 5;
struct Entry { SYSTEMTIME time; ULONGLONG tick; DWORD thread; std::string text; };
class Logger {
public:
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Entry> queue;
    std::thread worker;
    std::atomic<bool> enabled{false};
    std::atomic<unsigned long long> dropped{0}, accepted{0}, completed{0};
    std::atomic<bool> failed{false};
    bool quitting = false;
    std::filesystem::path directory;
    std::string prefix;

    ~Logger() { shutdown(); }
    void shutdown() noexcept {
        enabled = false;
        { std::lock_guard lock(mutex); quitting = true; }
        wake.notify_all();
        if (worker.joinable()) worker.join();
    }
    void retain() {
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            const auto name = entry.path().filename().string();
            if (entry.is_regular_file() && name.starts_with("partymic-") && name.ends_with(".log"))
                files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
        while (files.size() > keep_files) {
            std::error_code ignored;
            std::filesystem::remove(files.front(), ignored);
            files.erase(files.begin());
        }
    }
    void run() noexcept {
        try {
            std::filesystem::create_directories(directory);
            std::ofstream file;
            size_t bytes = file_limit;
            unsigned part = 0;
            for (;;) {
                std::deque<Entry> batch;
                {
                    std::unique_lock lock(mutex);
                    wake.wait_for(lock, std::chrono::milliseconds(500), [&] { return quitting || !queue.empty(); });
                    if (queue.empty() && quitting) break;
                    batch.swap(queue);
                }
                if (batch.empty()) continue;
                for (const auto& entry : batch) {
                    if (bytes >= file_limit) {
                        file.close();
                        char suffix[24]; std::snprintf(suffix, sizeof(suffix), "-%06u.log", part++);
                        file.open(directory / (prefix + suffix), std::ios::binary | std::ios::trunc);
                        if (!file) throw std::runtime_error("Cannot open log");
                        retain(); bytes = 0;
                    }
                    const auto& t = entry.time;
                    char head[160];
                    std::snprintf(head, sizeof(head), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ tick=%llu pid=%lu tid=%lu ",
                        t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,
                        entry.tick,GetCurrentProcessId(),entry.thread);
                    file << head << entry.text << '\n';
                    bytes += std::char_traits<char>::length(head) + entry.text.size() + 1;
                }
                file.flush();
                if (!file) throw std::runtime_error("Cannot write log");
                completed.fetch_add(batch.size());
            }
        } catch (...) { failed = true; enabled = false; }
    }
};
Logger& logger() { static Logger value; return value; }
}
std::filesystem::path folder() {
    PWSTR local = nullptr;
    const auto hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local);
    if (FAILED(hr)) throw std::runtime_error("Cannot find local application folder");
    std::filesystem::path result = std::filesystem::path(local) / L"PartyMic" / L"Logs";
    CoTaskMemFree(local); return result;
}
std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}
void start(std::string_view version, const std::filesystem::path& directory) noexcept {
    auto& log = logger();
    if (log.worker.joinable()) return;
    try {
        log.directory = directory.empty() ? folder() : directory;
        {std::lock_guard lock(log.mutex);log.queue.clear();}
        log.accepted = log.completed = log.dropped = 0;
        SYSTEMTIME t{};GetSystemTime(&t);
        char prefix[100];std::snprintf(prefix,sizeof(prefix),"partymic-%04u%02u%02u-%02u%02u%02u-%03u-%lu",
            t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,GetCurrentProcessId());
        log.prefix = prefix;
        log.quitting = false; log.failed = false; log.enabled = true;
        log.worker = std::thread([&log] { log.run(); });
        write("app.start", "version=" + std::string(version) + " logs_local_only=1 audio_saved=0");
    } catch (...) { log.enabled = false; }
}
void write(std::string_view event, std::string_view detail) noexcept {
    auto& log = logger();
    if (!log.enabled) return;
    try {
        std::unique_lock lock(log.mutex, std::try_to_lock);
        if (!lock || log.queue.size() >= queue_limit) { ++log.dropped; return; }
        std::string text(event.substr(0, 100));
        if (!detail.empty()) { text += ' '; text += detail.substr(0, 1900); }
        std::replace(text.begin(),text.end(),'\n',' '); std::replace(text.begin(),text.end(),'\r',' ');
        SYSTEMTIME time{}; GetSystemTime(&time);
        log.queue.push_back({time, GetTickCount64(), GetCurrentThreadId(), std::move(text)});
        ++log.accepted;
        lock.unlock(); log.wake.notify_one();
    } catch (...) { ++log.dropped; }
}
void write(std::string_view event, const std::wstring& detail) noexcept {
    try { write(event, utf8(detail)); } catch (...) {}
}
bool flush(unsigned timeout_ms) noexcept {
    auto& log = logger();
    const auto target = log.accepted.load(), end = GetTickCount64() + timeout_ms;
    log.wake.notify_one();
    while (log.completed.load() < target && !log.failed && GetTickCount64() < end) Sleep(5);
    return !log.failed && log.completed.load() >= target;
}
void stop() noexcept {
    auto& log = logger();
    if (!log.worker.joinable()) return;
    write("app.exit", "dropped_log_events=" + std::to_string(log.dropped.load()));
    log.shutdown();
}
}
