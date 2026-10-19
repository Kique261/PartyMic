#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace partymic {
constexpr unsigned sample_rate = 48000;
constexpr unsigned channels = 2;

// Owned by the audio thread. Whole stereo frames are retained/dropped together.
class AudioQueue {
  public:
    explicit AudioQueue(std::size_t capacity_frames = sample_rate / 10);
    void push(const float *interleaved, std::size_t frames, bool silent = false);
    void pop(std::span<float> interleaved);
    // Smoothly follows independent device clocks; returns unfilled output frames.
    std::size_t pop_clocked(std::span<float> interleaved, std::size_t target_frames = sample_rate / 50);
    void clear();
    [[nodiscard]] std::size_t size_frames() const { return size_ / channels; }
    [[nodiscard]] std::size_t dropped_frames() const { return dropped_ / channels; }

  private:
    std::vector<float> data_;
    std::size_t head_ = 0, size_ = 0, dropped_ = 0;
    double fraction_ = 0, clock_error_ = 0, clock_ratio_ = 1;
    bool primed_ = false;
    // Causal 64-tap fractional delay; adds 31 frames (0.65 ms at 48 kHz).
    std::array<float, 64 * channels> history_{};
    std::size_t history_pos_ = 0;
    void remember_frame(std::size_t index);
    void reset_clock();
};

struct MixSettings {
    float music_gain = 0.25f;
    float mic_gain = 1.0f;
    bool music_muted = false;
    bool mic_muted = false;
    bool ducking = false;
    float duck_floor = 0.65f; // Retained music level while speech is detected.
};

struct Levels {
    float music = 0, mic = 0, output = 0;
    float duck_gain = 1;
};

class Mixer {
  public:
    // Equal-length interleaved stereo buffers, no allocation in the audio path.
    Levels mix(std::span<const float> music, std::span<const float> mic, std::span<float> output,
               const MixSettings &settings);
    void reset();

  private:
    float envelope_ = 0;
    float duck_gain_ = 1;
    float music_gain_ = 0, mic_gain_ = 0;
    unsigned hold_frames_ = 0;
};
} // namespace partymic
