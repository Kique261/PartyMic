#include "core/mixer.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace partymic;
namespace {
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
void near(float actual, float expected, const char *message) {
    require(std::abs(actual - expected) < 0.0001f, message);
}
void queue_tests() {
    AudioQueue queue(3);
    const float first[] = {1, -1, 2, -2};
    queue.push(first, 2);
    std::vector<float> output(2);
    queue.pop(output);
    near(output[0], 1, "FIFO order");
    const float second[] = {3, -3, 4, -4};
    queue.push(second, 2);
    std::vector<float> full(8);
    queue.pop(full);
    require(full == std::vector<float>({2, -2, 3, -3, 4, -4, 0, 0}), "Ring wrap and underflow silence");
    queue.push(first, 2);
    queue.push(second, 2);
    require(queue.dropped_frames() == 1, "Overflow must count dropped old frames");
    std::vector<float> three(6);
    queue.pop(three);
    require(three == std::vector<float>({2, -2, 3, -3, 4, -4}),
            "Overflow must retain newest complete frames");
    queue.clear();
    const float oversized[] = {0, 0, 1, -1, 2, -2, 3, -3, 4, -4};
    queue.push(oversized, 5);
    queue.pop(three);
    require(three == std::vector<float>({2, -2, 3, -3, 4, -4}), "Oversized packet retains tail");
    require(queue.dropped_frames() == 2, "Oversized packet drop accounting");
    queue.push(nullptr, 3, true);
    queue.pop(three);
    require(std::all_of(three.begin(), three.end(), [](float x) { return x == 0; }),
            "Silent capture packets");
    const float invalid[] = {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()};
    queue.push(invalid, 1);
    queue.pop(output);
    require(output[0] == 0 && output[1] == 0, "Invalid capture samples are silenced");
    bool threw = false;
    try {
        queue.pop(std::span<float>(output.data(), 1));
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    require(threw, "Partial stereo frames rejected");
}
void clock_tests() {
    for (int drift : {-1, 1}) {
        AudioQueue queue(2880);
        std::vector<float> input(1440 * channels), output(480 * channels);
        std::size_t position = 0;
        auto fill = [&input, &position](std::size_t frames) {
            for (std::size_t i = 0; i < frames; ++i, ++position) {
                input[2 * i] = 0.15f + 0.04f * std::sin(2 * std::numbers::pi * 300 * position / sample_rate);
                input[2 * i + 1] = -input[2 * i];
            }
        };
        fill(1440);
        queue.push(input.data(), 1440);
        for (int block = 0; block < 4000; ++block) {
            const auto count = static_cast<std::size_t>(480 + (block % 2 ? drift : 0));
            fill(count);
            queue.push(input.data(), count);
            require(queue.pop_clocked(output) == 0, "Clock drift must not starve buffered audio");
            for (std::size_t i = 0; i < 480; ++i) {
                require(std::isfinite(output[2 * i]), "Clock correction outputs finite samples");
                // A causal bandlimited filter has a short startup delay. A
                // continuous low-frequency signal must retain its DC/range
                // once that transient has passed, for either clock direction.
                if (block || i >= 64)
                    require(output[2 * i] >= 0.1099f && output[2 * i] <= 0.1901f,
                            "Clock correction preserves a smooth signal after startup");
                near(output[2 * i], -output[2 * i + 1], "Clock correction keeps stereo aligned");
            }
        }
        require(queue.dropped_frames() == 0, "Clock drift must not overflow the queue");
        require(queue.size_frames() > 200 && queue.size_frames() < 2000,
                "Clock drift keeps a bounded input lead");
        queue.clear();
        require(queue.pop_clocked(output) == 480, "Empty clocked queue counts silence frames");
        require(std::all_of(output.begin(), output.end(), [](float x) { return x == 0; }),
                "Clocked starvation outputs silence");
        fill(1440);
        queue.push(input.data(), 1440);
        require(queue.pop_clocked(output) == 0, "Clocked queue reprimes after starvation");
    }
}
void resampler_fidelity_tests() {
    for (int drift : {-1, 1}) {
        for (double hz : {1000., 8000., 12000., 16000., 20000.}) {
            AudioQueue queue(2880);
            std::vector<float> input(1440 * channels), output(480 * channels);
            std::size_t position = 0, samples = 0;
            auto feed = [&](std::size_t frames) {
                for (std::size_t i = 0; i < frames; ++i, ++position) {
                    const double phase = 2 * std::numbers::pi * position / sample_rate;
                    input[2 * i] = float(0.1 * std::sin(hz * phase));
                    input[2 * i + 1] = float(0.05 * std::cos(1300 * phase));
                }
                queue.push(input.data(), frames);
            };
            feed(1440);
            double left = 0, right = 0;
            for (int block = 0; block < 800; ++block) {
                feed(std::size_t(480 + (block % 2 ? drift : 0)));
                require(queue.pop_clocked(output) == 0, "Frequency regression has no starvation");
                for (float value : output)
                    require(std::isfinite(value), "Resampler cannot produce nonfinite samples");
                if (block > 400) {
                    for (std::size_t i = 0; i < 480; ++i) {
                        left += double(output[2 * i]) * output[2 * i];
                        right += double(output[2 * i + 1]) * output[2 * i + 1];
                        ++samples;
                    }
                }
            }
            const double left_db = 10 * std::log10(left / samples / (0.1 * 0.1 / 2));
            const double right_db = 10 * std::log10(right / samples / (0.05 * 0.05 / 2));
            std::cout << "QUEUE_FREQUENCY drift=" << drift << " hz=" << hz << " left_db=" << left_db
                      << " right_db=" << right_db << '\n';
            require(std::abs(left_db) < 0.1, "Clock correction preserves 1-20 kHz within 0.1 dB");
            require(std::abs(right_db) < 0.1, "Independent right-channel tone retains its gain");
            require(queue.dropped_frames() == 0, "Frequency regression has no overflow");
        }
    }
}
void resampler_discontinuity_tests() {
    AudioQueue queue(2880);
    std::vector<float> input(1440 * channels, 0.1f), output(480 * channels);
    queue.push(input.data(), 1440);
    require(queue.pop_clocked(output) == 0, "Prime discontinuity regression");
    near(output.back(), 0.1f, "Filter retains DC after startup");
    // Overflow must discard old filter history as well as old queue frames.
    std::vector<float> silence(2880 * channels);
    queue.push(silence.data(), 2880);
    require(queue.dropped_frames() > 0, "Discontinuity regression exercises overflow");
    queue.pop_clocked(output);
    require(std::all_of(output.begin(), output.end(), [](float x) { return x == 0; }),
            "Overflow does not leak old filter history into new silence");
    queue.clear();
    queue.push(input.data(), 1440);
    queue.pop_clocked(output);
    for (int block = 0; block < 5; ++block)
        queue.pop_clocked(output);
    require(std::all_of(output.begin(), output.end(), [](float x) { return x == 0; }),
            "Exhausted input settles to exact silence");
    queue.push(nullptr, 1440, true);
    require(queue.pop_clocked(output) == 0, "Silent input can reprime after starvation");
    require(std::all_of(output.begin(), output.end(), [](float x) { return x == 0; }),
            "Starvation does not leak old filter history into restarted silence");
    queue.clear();
    queue.push(input.data(), 1440);
    queue.pop_clocked(output);
    queue.push(nullptr, 4000, true);
    queue.pop_clocked(output);
    require(queue.dropped_frames() >= 1120, "Oversized clocked packet is accounted");
    require(std::all_of(output.begin(), output.end(), [](float x) { return x == 0; }),
            "Oversized packet resets history at the retained tail");
}
void mix_tests() {
    constexpr auto count = sample_rate * channels;
    std::vector<float> music(count, 0.2f), mic(count, 0.1f), output(count);
    MixSettings settings;
    settings.music_gain = 0.5f;
    settings.ducking = false;
    Mixer mixer;
    auto levels = mixer.mix(music, mic, output, settings);
    near(output.back(), 0.2f, "Independent source gains must sum");
    require(levels.output > 0.19f, "Output meter");
    settings.mic_muted = true;
    mixer.mix(music, mic, output, settings);
    near(output[0], 0.1f, "Microphone mute must take effect on next block");
    settings.music_muted = true;
    mixer.mix(music, mic, output, settings);
    require(std::all_of(output.begin(), output.end(), [](float x) { return x == 0; }), "Both sources muted");
    settings = {};
    settings.music_gain = settings.mic_gain = 2;
    settings.ducking = false;
    std::fill(music.begin(), music.end(), 1.0f);
    std::fill(mic.begin(), mic.end(), 1.0f);
    mixer.mix(music, mic, output, settings);
    require(std::all_of(output.begin(), output.end(),
                        [](float x) { return std::isfinite(x) && std::abs(x) <= 0.98001f; }),
            "Limiter ceiling");
    music[0] = std::numeric_limits<float>::quiet_NaN();
    mic[1] = std::numeric_limits<float>::infinity();
    settings.music_gain = std::numeric_limits<float>::quiet_NaN();
    mixer.mix(music, mic, output, settings);
    require(std::all_of(output.begin(), output.end(), [](float x) { return std::isfinite(x); }),
            "Nonfinite samples and gains cannot contaminate output");
    bool threw = false;
    try {
        mixer.mix(music, mic, std::span<float>(output.data(), 2), settings);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    require(threw, "Mismatched buffers rejected");
}
void ducking_tests() {
    std::vector<float> music(sample_rate * channels, 0.1f), voice(music.size(), 0.1f), output(music.size());
    Mixer mixer;
    MixSettings settings;
    require(!settings.ducking, "Music level is preserved by default");
    auto untouched = mixer.mix(music, voice, output, settings);
    near(untouched.duck_gain, 1, "Speech must not duck music by default");
    settings.ducking = true;
    auto speaking = mixer.mix(music, voice, output, settings);
    require(speaking.duck_gain > 0.64f && speaking.duck_gain < 0.66f,
            "Default optional ducking retains 65 percent");
    settings.duck_floor = 0.25f;
    auto stronger = mixer.mix(music, voice, output, settings);
    require(stronger.duck_gain < 0.26f, "Ducking depth is adjustable");
    settings.duck_floor = std::numeric_limits<float>::quiet_NaN();
    auto invalid = mixer.mix(music, voice, output, settings);
    require(std::isfinite(invalid.duck_gain), "Invalid ducking depth cannot contaminate output");
    std::fill(voice.begin(), voice.end(), 0);
    mixer.mix(music, voice, output, settings);
    auto quiet = mixer.mix(music, voice, output, settings);
    require(quiet.duck_gain > 0.97f, "Music must recover after speech");
    std::fill(voice.begin(), voice.end(), 0.3f);
    settings.mic_muted = true;
    auto muted = mixer.mix(music, voice, output, settings);
    require(muted.duck_gain > 0.99f && muted.mic == 0, "Muted microphone cannot trigger ducking");
    settings.mic_muted = false;
    settings.ducking = false;
    auto disabled = mixer.mix(music, voice, output, settings);
    require(disabled.duck_gain > 0.99f, "Ducking can be disabled");
}
void music_fidelity_tests() {
    const size_t frames = sample_rate;
    std::vector<float> music(frames * 2), voice(frames * 2), mixed(frames * 2), baseline(frames * 2),
        zeros(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        music[2 * i] = music[2 * i + 1] = 0.15f * std::sin(2 * 3.141592653589793 * 12000 * i / sample_rate);
        voice[2 * i] = voice[2 * i + 1] = 0.12f * std::sin(2 * 3.141592653589793 * 300 * i / sample_rate);
    }
    MixSettings s;
    s.music_gain = 1;
    s.mic_gain = 1;
    Mixer a, b;
    a.mix(music, voice, mixed, s);
    b.mix(zeros, voice, baseline, s);
    double error = 0, power = 0;
    for (size_t i = sample_rate; i < mixed.size(); ++i) {
        const double d = mixed[i] - baseline[i] - music[i];
        error += d * d;
        power += double(music[i]) * music[i];
    }
    require(error / power < 1e-8,
            "Speech does not attenuate or filter high-frequency music with ducking disabled");
}
void packet_independence() {
    std::mt19937 random(42);
    std::uniform_real_distribution<float> samples(-0.3f, 0.3f);
    const std::size_t count = 12000;
    std::vector<float> music(count), mic(count), whole(count), split(count);
    for (std::size_t i = 0; i < count; ++i) {
        music[i] = samples(random);
        mic[i] = samples(random);
    }
    Mixer first, second;
    MixSettings settings;
    settings.ducking = true;
    first.mix(music, mic, whole, settings);
    std::size_t offset = 0;
    while (offset < count) {
        const std::size_t size =
            std::min(count - offset, static_cast<std::size_t>(random() % 200 + 1) * channels);
        second.mix(std::span<const float>(music).subspan(offset, size),
                   std::span<const float>(mic).subspan(offset, size),
                   std::span<float>(split).subspan(offset, size), settings);
        offset += size;
    }
    for (std::size_t i = 0; i < count; ++i)
        near(split[i], whole[i], "Packet size cannot affect audio envelope");
}
} // namespace

int main() {
    try {
        queue_tests();
        clock_tests();
        resampler_fidelity_tests();
        resampler_discontinuity_tests();
        mix_tests();
        ducking_tests();
        music_fidelity_tests();
        packet_independence();
        std::cout << "PASS: queue ordering/overflow, clock drift, silence, mixing, mute, limiting, invalid "
                     "input, ducking, packet independence\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
