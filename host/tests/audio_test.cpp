// components/audio/src/analysis.cpp: the FFT, the bands, the gain, silence and the beat;
// packets.cpp: the UDP inputs.

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "analysis.hpp"
#include "check.hpp"
#include "packets.hpp"

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

TEST(audio_settings_silence_level_and_gain)
{
    // the quiet hiss (about -74 dB) is silence at -62 dB, sound at -80 dB
    px::audio::Settings settings;
    settings.silence_db = -80;
    auto analyzer = std::make_unique<Analyzer>();
    analyzer->configure(settings);
    Audio audio{};
    const auto hiss = sine(1000.0f, 0.0002f);
    analyzer->process(hiss.data(), audio);
    CHECK(audio.active);
    // +20 dB gain moves the loudest band by 20 dB
    auto plain = std::make_unique<Analyzer>();
    auto gained = std::make_unique<Analyzer>();
    px::audio::Settings louder;
    louder.gain_db = 20;
    gained->configure(louder);
    const auto tone = sine(1000.0f, 0.01f);
    plain->process(tone.data(), audio);
    gained->process(tone.data(), audio);
    CHECK(std::fabs(gained->loudest_db() - plain->loudest_db() - 20.0f) < 0.01f);
}

TEST(audio_settings_without_automatic_gain_the_top_is_full_scale)
{
    // a sine at a tenth of full scale is about -20 dB: halfway up a range of 40 dB
    px::audio::Settings settings;
    settings.agc = false;
    settings.range_db = 40;
    auto analyzer = std::make_unique<Analyzer>();
    analyzer->configure(settings);
    Audio audio{};
    const auto tone = sine(1000.0f, 0.1f);
    analyzer->process(tone.data(), audio);
    CHECK(analyzer->reference_db() == 0.0f);
    const uint8_t top = audio.bands[loudest_band(audio)];
    CHECK(top > 115 && top < 141);
    // the same with the automatic gain fills the bar
    auto agc = std::make_unique<Analyzer>();
    agc->process(tone.data(), audio);
    CHECK(audio.bands[loudest_band(audio)] == 255);
}

TEST(audio_settings_beat_threshold)
{
    // a kick 1.5 times as loud (2.25 times the energy): a beat at 1.6, none at 3.0
    for (const uint8_t tenths : {uint8_t{16}, uint8_t{30}}) {
        px::audio::Settings settings;
        settings.beat_ratio_tenths = tenths;
        auto analyzer = std::make_unique<Analyzer>();
        analyzer->configure(settings);
        Audio audio{};
        for (int i = 0; i < 40; i++) {
            const auto steady = sine(60.0f, 0.02f, i * kFftSize);
            analyzer->process(steady.data(), audio);
        }
        const auto kick = sine(60.0f, 0.03f, 40 * kFftSize);
        analyzer->process(kick.data(), audio);
        CHECK(audio.beat == (tenths == 16));
    }
}

// ------------------------------------------------------------------ the UDP inputs' packets

namespace {

std::vector<uint8_t> warls(const std::vector<uint8_t> &bands, uint8_t left, uint8_t right, uint8_t timeout = 5)
{
    std::vector<uint8_t> packet(2 + 4 + 2 + ((bands.size() + 3) & ~size_t{3}) + 2);
    packet[0] = 1;
    packet[1] = timeout;
    packet[2] = 222;
    packet[3] = 77;
    packet[4] = left;
    packet[5] = right;
    packet[6] = 66;
    packet[7] = static_cast<uint8_t>(bands.size());
    std::memcpy(packet.data() + 8, bands.data(), bands.size());
    return packet;
}

std::vector<uint8_t> wled_sync(float smooth, uint8_t peak, const uint8_t (&fft)[16])
{
    std::vector<uint8_t> packet(44);
    std::memcpy(packet.data(), "00002", 6);
    const float raw = smooth;
    std::memcpy(packet.data() + 8, &raw, 4);
    std::memcpy(packet.data() + 12, &smooth, 4);
    packet[16] = peak;
    std::memcpy(packet.data() + 18, fft, 16);
    return packet;
}

} // namespace

TEST(audio_warls_packets)
{
    std::vector<uint8_t> bands(32);
    for (size_t i = 0; i < 32; i++) {
        bands[i] = static_cast<uint8_t>(i * 8);
    }
    Received r{};
    auto packet = warls(bands, 100, 180);
    CHECK(packet.size() == 42); // the size of the kfc_fw sender for 32 bands
    REQUIRE(parse_warls(packet.data(), packet.size(), r));
    CHECK(r.audio.active);
    CHECK(r.audio.loudness == 180);
    CHECK(r.audio.bands[0] == 0 && r.audio.bands[31] == 248);
    CHECK(r.hold_ms == 5000);
    CHECK(!r.has_beat);
    // fewer bands are spread over the 32
    auto few = warls({0, 255}, 0, 0);
    REQUIRE(parse_warls(few.data(), few.size(), r));
    CHECK(r.audio.bands[0] == 0 && r.audio.bands[31] == 255);
    CHECK(r.audio.bands[16] > 120 && r.audio.bands[16] < 140);
    // silence is not active
    auto silent = warls(std::vector<uint8_t>(32), 0, 0);
    REQUIRE(parse_warls(silent.data(), silent.size(), r));
    CHECK(!r.audio.active);
    // broken: timeout 0, magic, band count, short
    auto zero = warls(bands, 1, 1, 0);
    CHECK(!parse_warls(zero.data(), zero.size(), r));
    packet[6] = 65;
    CHECK(!parse_warls(packet.data(), packet.size(), r));
    auto one = warls({9}, 1, 1);
    CHECK(!parse_warls(one.data(), one.size(), r));
    CHECK(!parse_warls(few.data(), 9, r));
}

TEST(audio_wled_sync_packets)
{
    uint8_t fft[16] = {};
    fft[0] = 200;
    fft[15] = 50;
    Received r{};
    auto packet = wled_sync(120.4f, 1, fft);
    REQUIRE(parse_wled_sync(packet.data(), packet.size(), r));
    CHECK(r.audio.active && r.audio.beat && r.has_beat);
    CHECK(r.audio.loudness == 120);
    CHECK(r.audio.bands[0] == 200 && r.audio.bands[31] == 50);
    CHECK(r.hold_ms == kWledSyncHoldMs);
    auto quiet = wled_sync(0.0f, 1, {});
    REQUIRE(parse_wled_sync(quiet.data(), quiet.size(), r));
    CHECK(!r.audio.active && !r.audio.beat);
    // v1 and short packets are not taken
    packet[4] = '1';
    CHECK(!parse_wled_sync(packet.data(), packet.size(), r));
    packet[4] = '2';
    CHECK(!parse_wled_sync(packet.data(), 40, r));
}

TEST(audio_beat_finder_on_the_bass)
{
    BeatFinder finder;
    Audio audio;
    audio.active = true;
    for (size_t b = 0; b < 4; b++) {
        audio.bands[b] = 80;
    }
    size_t beats = 0;
    uint32_t ms = 0;
    for (; ms < 2000; ms += 20) {
        beats += finder.step(audio, 16, ms) ? 1 : 0;
    }
    CHECK(beats == 0); // steady bass: no beat
    for (size_t b = 0; b < 4; b++) {
        audio.bands[b] = 250;
    }
    CHECK(finder.step(audio, 16, ms));
    CHECK(!finder.step(audio, 16, ms + 20)); // not again within 140 ms
}
