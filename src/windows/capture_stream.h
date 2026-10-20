#pragma once
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functional>
#include <memory>
#include <span>

namespace partymic {
enum class CaptureTiming { event, polling };
struct CaptureStatistics {
    unsigned long long frames = 0, signal_frames = 0;
    unsigned long long discontinuities = 0, silent_frames = 0;
};

// Owns one shared 48 kHz stereo float capture on the caller's COM thread.
// Takes an OS endpoint; activation, format and notifications stay inside.
// A silent packet has an empty span and a nonzero frame count. The consumer
// must finish using samples before it returns and must not reenter/close the
// stream. Consumer exceptions propagate after returning the OS packet.
// close() is idempotent.
class CaptureStream {
public:
    using Consumer = std::function<void(std::span<const float>, UINT32 frames)>;
    CaptureStream(IMMDevice* endpoint, CaptureTiming timing, const char* label);
    ~CaptureStream();
    CaptureStream(const CaptureStream&) = delete;
    CaptureStream& operator=(const CaptureStream&) = delete;
    void start();
    void drain(const Consumer& consume);
    void close() noexcept;
    HANDLE ready() const noexcept;
    UINT32 buffer_frames() const noexcept { return buffer_frames_; }
    CaptureStatistics statistics() const noexcept { return statistics_; }
private:
    struct State;
    std::unique_ptr<State> state_;
    UINT32 buffer_frames_ = 0;
    CaptureStatistics statistics_;
};
}
