// components/audio/src/analysis.cpp: the FFT, the bands, the gain, silence and the beat.

#include <cmath>
#include <memory>
#include <vector>

#include "analysis.hpp"
#include "check.hpp"

using namespace px::audio::detail;
using px::effects::Audio;
using px::effects::kBands;

namespace {

constexpr float kTwoPi = 6.28318531f;

// one block of a sine of `hz` at `amplitude` (0..1 of full scale), continuing at sample `offset`
std::vector<float> sine(float hz, float amplitude, size_t offset = 0)
{
    std::vector<float> samples(kFftSize);
    for (size_t i = 0; i < kFftSize; i++) {
        samples[i] = amplitude * std::sin(kTwoPi * hz * static_cast<float>(i + offset) / kSampleRate);
    }
    return samples;
}

size_t loudest_band(const Audio &audio)
{
    size_t best = 0;
    for (size_t b = 1; b < kBands; b++) {
        best = audio.bands[b] > audio.bands[best] ? b : best;
    }
    return best;
}

} // namespace

TEST(audio_fft_finds_the_bin_of_a_sine)
{
    float re[64];
    float im[64] = {};
    for (size_t i = 0; i < 64; i++) {
        re[i] = std::cos(kTwoPi * 5.0f * static_cast<float>(i) / 64.0f);
    }
    fft(re, im, 64);
    CHECK(std::fabs(re[5] - 32.0f) < 0.01f);
    CHECK(std::fabs(re[59] - 32.0f) < 0.01f);
    CHECK(std::fabs(re[4]) < 0.01f && std::fabs(re[6]) < 0.01f && std::fabs(re[0]) < 0.01f);
}

TEST(audio_silence_is_inactive)
{
    auto analyzer = std::make_unique<Analyzer>();
    Audio audio{};
    const std::vector<float> quiet(kFftSize, 0.0f);
    for (int i = 0; i < 10; i++) {
        analyzer->process(quiet.data(), audio);
    }
    CHECK(!audio.active);
    CHECK(!audio.beat);
    CHECK(audio.loudness == 0);
    for (size_t b = 0; b < kBands; b++) {
        CHECK(audio.bands[b] == 0);
    }
    // a very quiet sine, below the floor, as well
    const auto hiss = sine(1000.0f, 0.0002f);
    analyzer->process(hiss.data(), audio);
    CHECK(!audio.active);
}

TEST(audio_a_sine_fills_its_band_and_is_low_elsewhere)
{
    for (const float hz : {80.0f, 440.0f, 1000.0f, 5000.0f}) {
        auto analyzer = std::make_unique<Analyzer>();
        Audio audio{};
        const auto samples = sine(hz, 0.1f);
        analyzer->process(samples.data(), audio);
        REQUIRE(audio.active);
        const size_t band = loudest_band(audio);
        CHECK(audio.bands[band] == 255);
        // the band's frequency range: log-spaced from 50 Hz to 10 kHz
        const float low = 50.0f * std::pow(200.0f, static_cast<float>(band) / kBands);
        const float high = 50.0f * std::pow(200.0f, static_cast<float>(band + 1) / kBands);
        CHECK(hz > low * 0.8f && hz < high * 1.25f);
        // far away bands stay low (the window leaks a few bins)
        CHECK(audio.bands[band > 10 ? band - 10 : band + 10] < 64);
        CHECK(audio.loudness > 128);
    }
}

TEST(audio_the_gain_shows_quiet_and_loud_alike)
{
    auto loud = std::make_unique<Analyzer>();
    auto quiet = std::make_unique<Analyzer>();
    Audio a{};
    Audio b{};
    for (int i = 0; i < 3; i++) {
        const auto l = sine(1000.0f, 0.5f, i * kFftSize);
        const auto q = sine(1000.0f, 0.01f, i * kFftSize);
        loud->process(l.data(), a);
        quiet->process(q.data(), b);
    }
    CHECK(a.active && b.active);
    CHECK(loudest_band(a) == loudest_band(b));
    CHECK(a.bands[loudest_band(a)] == 255 && b.bands[loudest_band(b)] == 255);
    CHECK(loud->reference_db() > quiet->reference_db() + 30.0f);
}

TEST(audio_bars_fall_slowly)
{
    auto analyzer = std::make_unique<Analyzer>();
    Audio audio{};
    const auto tone = sine(1000.0f, 0.1f);
    analyzer->process(tone.data(), audio);
    const size_t band = loudest_band(audio);
    REQUIRE(audio.bands[band] == 255);
    // a much quieter tone next: the bar falls by 12 % per block, not at once
    const auto soft = sine(1000.0f, 0.0001f);
    analyzer->process(soft.data(), audio);
    CHECK(audio.bands[band] > 215 && audio.bands[band] < 230);
    analyzer->process(soft.data(), audio);
    CHECK(audio.bands[band] > 185 && audio.bands[band] < 200);
}

TEST(audio_a_bass_kick_is_a_beat)
{
    auto analyzer = std::make_unique<Analyzer>();
    Audio audio{};
    // a steady quiet bass, then a kick 4x as loud
    int beats = 0;
    for (int i = 0; i < 40; i++) {
        const auto steady = sine(60.0f, 0.02f, i * kFftSize);
        analyzer->process(steady.data(), audio);
        beats += audio.beat ? 1 : 0;
    }
    CHECK(beats == 0);
    const auto kick = sine(60.0f, 0.08f);
    analyzer->process(kick.data(), audio);
    CHECK(audio.beat);
    // the holdoff: the next kick right after is none
    analyzer->process(kick.data(), audio);
    CHECK(!audio.beat);
}
