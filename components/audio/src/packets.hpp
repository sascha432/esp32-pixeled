#pragma once

// The UDP audio inputs (docs/leds.md section 6.1): the packets of another device or a PC turned
// into the audio frame. Pure C++, host tested.
//
// WARLS (the kfc_fw format, port 21324): 0 protocol 1, 1 timeout in s (0: ignore the packet),
// 2..3 magic 0x4dde (little endian), 4 left level, 5 right level, 6 magic 0x42, 7 band count
// (2 to 32), then a byte per band.
//
// WLED audio sync v2 (multicast 239.0.0.1, port 11988), 44 bytes, little endian: 0 header
// "00002\0", 8 sampleRaw (float), 12 sampleSmth (float, 0..255), 16 samplePeak (non-zero: a
// beat), 18 fftResult (16 bytes, 0..255), 36 FFT_Magnitude (float), 40 FFT_MajorPeak (float, Hz).

#include <cstddef>
#include <cstdint>

#include "px/effects/effects.hpp"

namespace px::audio::detail {

constexpr uint16_t kWarlsPort = 21324;
constexpr uint16_t kWledSyncPort = 11988;
// the multicast group of WLED audio sync, 239.0.0.1
constexpr uint32_t kWledSyncGroup = (239u << 24) | 1u;
// a WLED sync source counts as gone this long after its last packet
constexpr uint32_t kWledSyncHoldMs = 1000;

struct Received {
    // the bands spread to 32, the loudness, `active` when there is any sound
    effects::Audio audio;
    // the source counts as there this long after the packet
    uint32_t hold_ms;
    // the packet says whether there was a beat (WLED sync); else the beat is found from the bass
    bool has_beat;
};

// false for a packet of another kind, a broken one or one with timeout 0
bool parse_warls(const uint8_t *data, size_t size, Received &out);
bool parse_wled_sync(const uint8_t *data, size_t size, Received &out);

// `count` (2 to 32) values spread over the 32 bands, in between interpolated
void spread_bands(const uint8_t *values, size_t count, uint8_t (&bands)[effects::kBands]);

// The beat of a source that does not send one: the bass (bands 0 to 3) well above its average, at
// most one in 140 ms, like the microphone's analysis.
class BeatFinder {
public:
    // `ratio_tenths` as audio::Settings::beat_ratio_tenths; `ms` a clock that moves forward
    bool step(const effects::Audio &audio, uint8_t ratio_tenths, uint32_t ms);

private:
    uint32_t _average = 0;
    uint32_t _last_ms = 0;
    bool _any = false;
};

} // namespace px::audio::detail
