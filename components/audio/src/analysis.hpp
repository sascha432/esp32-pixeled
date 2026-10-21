#pragma once

// The analysis of the audio input (docs/leds.md section 6.1): blocks of samples in, loudness, 32
// bands and the beat out. A Hann window and a 512-point FFT; the bands log-spaced from 50 Hz to
// 10 kHz, each the loudest bin of its range in dB, plus the gain of the settings. An automatic
// gain follows the loudest band (fast up, about 3 dB/s down) and shows the range below it as
// 0..255, so quiet and loud music both fill the bars; without it the range below full scale is
// shown. Below the silence level it is silence. Pure C++, host tested.

#include <cstddef>
#include <cstdint>

#include "px/audio/audio.hpp"
#include "px/effects/effects.hpp"

namespace px::audio::detail {

constexpr size_t kFftSize = 512;
constexpr uint32_t kSampleRate = 22050;

// In-place radix-2 FFT of `n` points (a power of 2): `re` and `im` in, the spectrum out.
void fft(float *re, float *im, size_t n);

class Analyzer {
public:
    Analyzer();

    // The settings (checked by audio::set_settings); the defaults until called.
    void configure(const Settings &settings);

    // One block of kFftSize samples, -1..1. Fills `out` (bands, loudness, beat, active).
    void process(const float *samples, effects::Audio &out);

    // the loudest band of the last block (the gain added) and the gain's reference, dB of full
    // scale
    float loudest_db() const
    {
        return _loudest_db;
    }
    float reference_db() const
    {
        return _reference_db;
    }

private:
    float _window[kFftSize];
    float _re[kFftSize];
    float _im[kFftSize];
    uint16_t _low[effects::kBands];
    uint16_t _high[effects::kBands];
    // the settings
    float _gain_db = 0.0f;
    bool _agc = true;
    float _silence_db = -62.0f;
    float _range_db = 40.0f;
    float _beat_ratio = 1.6f;
    // the bars fall slowly: what was shown last
    float _shown[effects::kBands] = {};
    float _reference_db = -62.0f;
    float _loudest_db = -120.0f;
    // the bass energy's average, for the beat
    float _bass_average = 0.0f;
    uint32_t _since_beat = 0;
};

} // namespace px::audio::detail
