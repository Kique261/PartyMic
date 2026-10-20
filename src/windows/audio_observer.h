#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <memory>

namespace partymic {
// Read-only subscriptions. Close on the stream's owning COM thread, before
// releasing IAudioClient: the registrations themselves hold session references.
class AudioObserver {
public:
    AudioObserver();
    ~AudioObserver();
    void attach(IAudioClient* client, IMMDevice* endpoint, const char* label);
    void close() noexcept;
    AudioObserver(const AudioObserver&)=delete;
    AudioObserver& operator=(const AudioObserver&)=delete;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
