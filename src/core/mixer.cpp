#include "core/mixer.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <numbers>
#include <stdexcept>

namespace partymic {
namespace {
float clean(float value) { return std::isfinite(value) ? std::clamp(value, -4.0f, 4.0f) : 0.0f; }
float gain(float value) { return std::isfinite(value) ? std::clamp(value, 0.0f, 2.0f) : 0.0f; }
constexpr size_t sinc_taps = 64, sinc_phases = 1024;
using SincTable = std::array<std::array<float, sinc_taps>, sinc_phases + 1>;
const SincTable &sinc_table() {
    static const SincTable table = [] {
        SincTable result{};
        constexpr double cutoff = 0.96;
        for (size_t phase = 0; phase <= sinc_phases; ++phase) {
            const double fraction = double(phase) / sinc_phases;
            double sum = 0;
            for (size_t tap = 0; tap < sinc_taps; ++tap) {
                const double x = double(tap) - 32 - fraction;
                const double window = std::abs(x) >= 32 ? 0
                                                        : 0.42 + 0.5 * std::cos(std::numbers::pi * x / 32) +
                                                              0.08 * std::cos(2 * std::numbers::pi * x / 32);
                const double value = std::abs(x) < 1e-10
                                         ? cutoff
                                         : std::sin(std::numbers::pi * cutoff * x) / (std::numbers::pi * x);
                result[phase][tap] = float(value * window);
                sum += result[phase][tap];
            }
            for (auto &weight : result[phase])
                weight = float(weight / sum);
        }
        return result;
    }();
    return table;
}

} // namespace

AudioQueue::AudioQueue(std::size_t capacity_frames)
    : data_(std::max<std::size_t>(1, capacity_frames) * channels, 0) {
    // Build the shared immutable table before any audio stream is started.
    (void)sinc_table();
}
void AudioQueue::remember_frame(std::size_t index) {
    history_[history_pos_ * channels] = data_[index];
    history_[history_pos_ * channels + 1] = data_[index + 1];
    history_pos_ = (history_pos_ + 1) % 64;
}
void AudioQueue::reset_clock() {
    fraction_ = clock_error_ = 0;
    clock_ratio_ = 1;
    primed_ = false;
    history_.fill(0);
    history_pos_ = 0;
}

void AudioQueue::push(const float *input, std::size_t frames, bool silent) {
    // Keep the newest frames when a producer outruns playback. No unbounded delay.
    const auto capacity = data_.size() / channels;
    if (frames > capacity) {
        const auto skipped = frames - capacity;
        dropped_ += skipped * channels;
        if (!silent && input)
            input += skipped * channels;
        frames = capacity;
        reset_clock();
    }
    const auto count = frames * channels;
    if (size_ + count > data_.size()) {
        const auto discard = size_ + count - data_.size();
        head_ = (head_ + discard) % data_.size();
        size_ -= discard;
        dropped_ += discard;
        // The retained head is no longer adjacent to the filter history.
        reset_clock();
    }
    for (std::size_t i = 0; i < count; ++i)
        data_[(head_ + size_ + i) % data_.size()] = silent || !input ? 0 : clean(input[i]);
    size_ += count;
}

void AudioQueue::pop(std::span<float> output) {
    if (output.size() % channels != 0)
        throw std::invalid_argument("Whole stereo frames required");
    const auto count = std::min(size_, output.size());
    for (std::size_t i = 0; i < count; ++i)
        output[i] = data_[(head_ + i) % data_.size()];
    std::fill(output.begin() + static_cast<std::ptrdiff_t>(count), output.end(), 0.0f);
    head_ = (head_ + count) % data_.size();
    size_ -= count;
    if (count)
        reset_clock();
}

std::size_t AudioQueue::pop_clocked(std::span<float> output, std::size_t target_frames) {
    if (output.size() % channels)
        throw std::invalid_argument("Whole stereo frames required");
    const auto frames = output.size() / channels;
    if (!frames)
        return 0;
    target_frames = std::max<std::size_t>(1, std::min(target_frames, data_.size() / channels / 2));
    if (!primed_) {
        if (size_frames() < target_frames) {
            std::fill(output.begin(), output.end(), 0);
            return frames;
        }
        primed_ = true;
        fraction_ = clock_error_ = 0;
        clock_ratio_ = 1;
    }
    // A slow, bounded fractional read rate keeps a lead without dropping or
    // duplicating whole packets when capture and render clocks differ.
    const double alpha = 1 - std::exp(-static_cast<double>(frames) / (sample_rate * 0.5));
    const double error = static_cast<double>(size_frames()) - target_frames - frames * 0.5;
    clock_error_ += (error - clock_error_) * alpha;
    const double requested = 1 + std::clamp(clock_error_ / (sample_rate * 5.0), -0.005, 0.005);
    clock_ratio_ += (requested - clock_ratio_) * alpha;
    std::size_t missing = 0;
    for (std::size_t i = 0; i < frames; ++i) {
        const auto available = size_frames();
        if (!available) {
            output[i * channels] = output[i * channels + 1] = 0;
            ++missing;
            if (primed_)
                reset_clock();
            continue;
        }
        const double phase_position = fraction_ * sinc_phases;
        const size_t phase = std::min(sinc_phases - 1, size_t(phase_position));
        const float phase_fraction = float(phase_position - phase);
        const auto &first = sinc_table()[phase];
        const auto &next = sinc_table()[phase + 1];
        float left = 0, right = 0;
        for (size_t tap = 0; tap < sinc_taps; ++tap) {
            const float weight = first[tap] + phase_fraction * (next[tap] - first[tap]);
            // History contains the last 64 consumed frames. The newest input
            // frame is at tap 63; output is delayed rather than reading ahead.
            const size_t index = ((history_pos_ + 1 + tap) % 64) * channels;
            const float l = tap == 63 ? data_[head_] : history_[index];
            const float r = tap == 63 ? data_[head_ + 1] : history_[index + 1];
            left += weight * l;
            right += weight * r;
        }
        output[i * channels] = left;
        output[i * channels + 1] = right;
        fraction_ += clock_ratio_;
        const auto consumed = std::min(available, static_cast<std::size_t>(fraction_));
        fraction_ -= consumed;
        for (size_t n = 0; n < consumed; ++n)
            remember_frame((head_ + n * channels) % data_.size());
        head_ = (head_ + consumed * channels) % data_.size();
        size_ -= consumed * channels;
        if (!size_)
            fraction_ = 0;
    }
    return missing;
}

void AudioQueue::clear() {
    head_ = size_ = dropped_ = 0;
    reset_clock();
}

void Mixer::reset() {
    envelope_ = 0;
    duck_gain_ = 1;
    music_gain_ = mic_gain_ = 0;
    hold_frames_ = 0;
}

Levels Mixer::mix(std::span<const float> music, std::span<const float> mic, std::span<float> output,
                  const MixSettings &settings) {
    if (music.size() != mic.size() || music.size() != output.size() || output.size() % channels)
        throw std::invalid_argument("Matching stereo buffers required");
    Levels levels;
    const float music_target = settings.music_muted ? 0 : gain(settings.music_gain);
    const float mic_target = settings.mic_muted ? 0 : gain(settings.mic_gain);
    // Sample-based smoothing: behavior is independent of the WASAPI packet size.
    const float attack = 1.0f - std::exp(-1.0f / (sample_rate * 0.010f));
    const float release = 1.0f - std::exp(-1.0f / (sample_rate * 0.300f));
    const float duck_attack = 1.0f - std::exp(-1.0f / (sample_rate * 0.080f));
    const float duck_release = 1.0f - std::exp(-1.0f / (sample_rate * 0.500f));
    const float duck_floor =
        std::isfinite(settings.duck_floor) ? std::clamp(settings.duck_floor, 0.25f, 1.0f) : 0.65f;
    const float smooth = 1.0f - std::exp(-1.0f / (sample_rate * 0.005f));
    for (std::size_t i = 0; i < output.size(); i += channels) {
        music_gain_ += (music_target - music_gain_) * smooth;
        mic_gain_ += (mic_target - mic_gain_) * smooth;
        float m[2] = {clean(music[i]) * music_gain_, clean(music[i + 1]) * music_gain_};
        // Explicit mute is immediate; voice detection must not react to a muted mic.
        float v[2] = {settings.mic_muted ? 0 : clean(mic[i]) * mic_gain_,
                      settings.mic_muted ? 0 : clean(mic[i + 1]) * mic_gain_};
        if (settings.music_muted)
            m[0] = m[1] = 0;
        const float voice = std::max(std::abs(v[0]), std::abs(v[1]));
        envelope_ += (voice - envelope_) * (voice > envelope_ ? attack : release);
        if (!settings.mic_muted && envelope_ > 0.025f && voice > 0.012f)
            hold_frames_ = sample_rate / 4;
        else if (hold_frames_)
            --hold_frames_;
        const float target = settings.ducking && !settings.mic_muted && hold_frames_ ? duck_floor : 1.0f;
        duck_gain_ += (target - duck_gain_) * (target < duck_gain_ ? duck_attack : duck_release);
        for (unsigned c = 0; c < channels; ++c) {
            // Soft knee above 0.8, continuously differentiable, ceiling < 0.98.
            float value = m[c] * duck_gain_ + v[c];
            if (std::abs(value) > 0.8f)
                value = std::copysign(0.8f + 0.18f * std::tanh((std::abs(value) - 0.8f) / 0.18f), value);
            output[i + c] = value;
            levels.music = std::max(levels.music, std::abs(m[c]));
            levels.mic = std::max(levels.mic, std::abs(v[c]));
            levels.output = std::max(levels.output, std::abs(value));
        }
    }
    levels.duck_gain = duck_gain_;
    return levels;
}
} // namespace partymic
