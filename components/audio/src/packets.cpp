// The UDP audio inputs' packets (packets.hpp, docs/leds.md section 6.1).

#include "packets.hpp"

#include <cstring>

namespace px::audio::detail {

namespace {

constexpr size_t kWarlsHeader = 8;
constexpr size_t kWledSyncSize = 44;

float read_float(const uint8_t *data)
{
    // little endian on the wire, as on the chips and the PC
    float value = 0.0f;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

uint8_t clamp8(float value)
{
    // NaN as well as negative values give 0
    return value >= 255.0f ? 255 : value > 0.0f ? static_cast<uint8_t>(value + 0.5f) : 0;
}

void set_active(effects::Audio &audio)
{
    audio.active = audio.loudness != 0;
    for (uint8_t band : audio.bands) {
        audio.active = audio.active || band != 0;
    }
}

} // namespace

void spread_bands(const uint8_t *values, size_t count, uint8_t (&bands)[effects::kBands])
{
    if (count < 2) {
        std::memset(bands, count == 1 ? values[0] : 0, sizeof(bands));
        return;
    }
    for (size_t b = 0; b < effects::kBands; b++) {
        // where band b falls between the values, in 1/256
        const size_t position = b * (count - 1) * 256 / (effects::kBands - 1);
        const size_t index = position >> 8;
        const uint32_t fraction = position & 255;
        const uint32_t a = values[index];
        const uint32_t next = index + 1 < count ? values[index + 1] : a;
        bands[b] = static_cast<uint8_t>((a * (256 - fraction) + next * fraction) >> 8);
    }
}

bool parse_warls(const uint8_t *data, size_t size, Received &out)
{
    if (size < kWarlsHeader || data[0] != 1 || data[2] != 0xde || data[3] != 0x4d || data[6] != 0x42) {
        return false;
    }
    const uint8_t count = data[7];
    if (data[1] == 0 || count < 2 || count > effects::kBands || size < kWarlsHeader + count) {
        return false;
    }
    out = {};
    spread_bands(data + kWarlsHeader, count, out.audio.bands);
    out.audio.loudness = data[4] > data[5] ? data[4] : data[5];
    set_active(out.audio);
    out.hold_ms = data[1] * 1000u;
    out.has_beat = false;
    return true;
}

bool parse_wled_sync(const uint8_t *data, size_t size, Received &out)
{
    if (size < kWledSyncSize || std::memcmp(data, "00002", 6) != 0) {
        return false;
    }
    out = {};
    spread_bands(data + 18, 16, out.audio.bands);
    out.audio.loudness = clamp8(read_float(data + 12));
    set_active(out.audio);
    out.audio.beat = out.audio.active && data[16] != 0;
    out.hold_ms = kWledSyncHoldMs;
    out.has_beat = true;
    return true;
}

bool BeatFinder::step(const effects::Audio &audio, uint8_t ratio_tenths, uint32_t ms)
{
    if (!audio.active) {
        _any = false;
        return false;
    }
    // x 16 for the average's precision
    const uint32_t bass = (audio.bands[0] + audio.bands[1] + audio.bands[2] + audio.bands[3]) * 16u;
    if (!_any) {
        // the first sound sets the average: its start is not a beat
        _any = true;
        _average = bass;
        _last_ms = ms;
        return false;
    }
    const bool beat = ms - _last_ms >= 140 && bass * 10u > _average * ratio_tenths && bass >= 64u * 16u;
    if (beat) {
        _last_ms = ms;
    }
    _average = static_cast<uint32_t>(static_cast<int32_t>(_average) +
                                     (static_cast<int32_t>(bass) - static_cast<int32_t>(_average)) / 16);
    return beat;
}

} // namespace px::audio::detail
