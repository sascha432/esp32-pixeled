// The analysis of the audio input (analysis.hpp, docs/leds.md section 6.1). Float math, single
// precision: the FPUs of the ESP32 and S3 have no double.

#include "analysis.hpp"

#include <cmath>

namespace px::audio::detail {

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kLowHz = 50.0f;
constexpr float kHighHz = 10000.0f;
// a full scale sine through the Hann window gives a bin of N / 4
constexpr float kFullScale = static_cast<float>(kFftSize) / 4.0f;
// the gain falls this many dB per block (43 blocks a second): about 3 dB/s
constexpr float kReleaseDb = 0.07f;
// the bars fall this share of their height per block
constexpr float kFall = 0.12f;
// the bass bands of the beat
constexpr size_t kBassBands = 4;
// at most one beat in this many blocks (140 ms)
constexpr uint32_t kBeatBlocks = 6;

float db(float magnitude)
{
    return 20.0f * std::log10(magnitude / kFullScale + 1e-9f);
}

} // namespace

void fft(float *re, float *im, size_t n)
{
    // bit reversal
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            const float r = re[i];
            re[i] = re[j];
            re[j] = r;
            const float m = im[i];
            im[i] = im[j];
            im[j] = m;
        }
    }
    for (size_t length = 2; length <= n; length <<= 1) {
        const float angle = -2.0f * kPi / static_cast<float>(length);
        const float step_re = std::cos(angle);
        const float step_im = std::sin(angle);
        for (size_t start = 0; start < n; start += length) {
            float w_re = 1.0f;
            float w_im = 0.0f;
            for (size_t k = 0; k < length / 2; k++) {
                const size_t a = start + k;
                const size_t b = a + length / 2;
                const float t_re = re[b] * w_re - im[b] * w_im;
                const float t_im = re[b] * w_im + im[b] * w_re;
                re[b] = re[a] - t_re;
                im[b] = im[a] - t_im;
                re[a] += t_re;
                im[a] += t_im;
                const float next = w_re * step_re - w_im * step_im;
                w_im = w_re * step_im + w_im * step_re;
                w_re = next;
            }
        }
    }
}

Analyzer::Analyzer()
{
    for (size_t i = 0; i < kFftSize; i++) {
        _window[i] = 0.5f - 0.5f * std::cos(2.0f * kPi * static_cast<float>(i) / static_cast<float>(kFftSize - 1));
    }
    // the bins of each band: log-spaced edges, at least one bin, never the DC bin
    const float bin_hz = static_cast<float>(kSampleRate) / static_cast<float>(kFftSize);
    for (size_t b = 0; b < effects::kBands; b++) {
        const float low = kLowHz * std::pow(kHighHz / kLowHz, static_cast<float>(b) / effects::kBands);
        const float high = kLowHz * std::pow(kHighHz / kLowHz, static_cast<float>(b + 1) / effects::kBands);
        const auto first = static_cast<uint16_t>(low / bin_hz + 0.5f);
        const auto last = static_cast<uint16_t>(high / bin_hz + 0.5f);
        _low[b] = first < 1 ? 1 : first;
        _high[b] = last < _low[b] ? _low[b] : last;
    }
}

void Analyzer::configure(const Settings &settings)
{
    _gain_db = static_cast<float>(settings.gain_db);
    _agc = settings.agc;
    _silence_db = static_cast<float>(settings.silence_db);
    _range_db = static_cast<float>(settings.range_db < 1 ? 1 : settings.range_db);
    _beat_ratio = static_cast<float>(settings.beat_ratio_tenths) / 10.0f;
}

void Analyzer::process(const float *samples, effects::Audio &out)
{
    // the loudness: the block's RMS, without the DC the microphone may have
    float mean = 0.0f;
    for (size_t i = 0; i < kFftSize; i++) {
        mean += samples[i];
    }
    mean /= static_cast<float>(kFftSize);
    float power = 0.0f;
    for (size_t i = 0; i < kFftSize; i++) {
        const float s = samples[i] - mean;
        power += s * s;
        _re[i] = s * _window[i];
        _im[i] = 0.0f;
    }
    const float rms_db = 10.0f * std::log10(power / static_cast<float>(kFftSize) + 1e-12f) + 3.0f + _gain_db;
    fft(_re, _im, kFftSize);

    // each band: its loudest bin in dB; the bass energy for the beat
    float band_db[effects::kBands];
    float loudest = -120.0f;
    float bass = 0.0f;
    for (size_t b = 0; b < effects::kBands; b++) {
        float magnitude = 0.0f;
        for (uint16_t k = _low[b]; k <= _high[b]; k++) {
            const float m = std::sqrt(_re[k] * _re[k] + _im[k] * _im[k]);
            magnitude = m > magnitude ? m : magnitude;
        }
        band_db[b] = db(magnitude) + _gain_db;
        loudest = band_db[b] > loudest ? band_db[b] : loudest;
        if (b < kBassBands) {
            bass += magnitude * magnitude;
        }
    }
    _loudest_db = loudest;

    // the automatic gain: up at once, down slowly, never below the silence; without it the range
    // below full scale
    if (_agc) {
        _reference_db = loudest > _reference_db ? loudest : _reference_db - kReleaseDb;
        _reference_db = _reference_db < _silence_db ? _silence_db : _reference_db;
    } else {
        _reference_db = 0.0f;
    }
    out.active = loudest > _silence_db;
    const float bottom = _reference_db - _range_db;
    for (size_t b = 0; b < effects::kBands; b++) {
        // silence: the bars fall to 0
        float value = out.active ? (band_db[b] - bottom) / _range_db : 0.0f;
        value = value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
        // rise at once, fall slowly
        const float fallen = _shown[b] - kFall;
        _shown[b] = value > fallen ? value : fallen;
        out.bands[b] = static_cast<uint8_t>(_shown[b] * 255.0f + 0.5f);
    }
    float loudness = out.active ? (rms_db - bottom) / _range_db : 0.0f;
    loudness = loudness < 0.0f ? 0.0f : loudness > 1.0f ? 1.0f : loudness;
    out.loudness = static_cast<uint8_t>(loudness * 255.0f + 0.5f);

    // the beat: the bass well above its own average, not too often; the average starts at the
    // first sound, not at 0, or the start of every sound would be a beat
    if (!out.active) {
        _bass_average = 0.0f;
    } else if (_bass_average == 0.0f) {
        _bass_average = bass;
    }
    _since_beat++;
    out.beat = out.active && _since_beat >= kBeatBlocks && bass > _beat_ratio * _bass_average && _bass_average > 0.0f;
    if (out.beat) {
        _since_beat = 0;
    }
    _bass_average += (bass - _bass_average) * 0.05f;
}

} // namespace px::audio::detail
