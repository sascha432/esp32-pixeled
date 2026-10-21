// The frame of the output (frame.hpp, docs/leds.md section 3).

#include "frame.hpp"

#include <cmath>

namespace px::leds::detail {

Gamma::Gamma()
{
    for (int i = 0; i < 256; i++) {
        table[i] = static_cast<uint16_t>(std::pow(static_cast<float>(i) / 255.0f, 2.2f) * 65535.0f + 0.5f);
        // output steps of a full channel: 1 at brightness 1, 255 at 255; 2 % linear in the curve, so
        // the first slider steps, where gamma alone is flat, still differ
        const float x = static_cast<float>(i - 1) / 254.0f;
        const float steps = i == 0 ? 0.0f : 1.0f + 254.0f * (0.98f * std::pow(x, 2.2f) + 0.02f * x);
        brightness[i] = static_cast<uint16_t>(steps / 255.0f * 65535.0f + 0.5f);
    }
}

uint8_t dither_offset(uint32_t frame, uint16_t led)
{
    // the frame number with its bits reversed (0, 128, 64, 192, 32, ...): any run of frames meets
    // the thresholds evenly; an odd step per LED shifts the pattern from one LED to the next
    uint8_t f = static_cast<uint8_t>(frame);
    f = static_cast<uint8_t>((f & 0xf0) >> 4 | (f & 0x0f) << 4);
    f = static_cast<uint8_t>((f & 0xcc) >> 2 | (f & 0x33) << 2);
    f = static_cast<uint8_t>((f & 0xaa) >> 1 | (f & 0x55) << 1);
    return static_cast<uint8_t>(f + led * 167u);
}

void compose(const effects::Rgb *canvas, const layout::Table &table, uint8_t level, const Gamma &gamma,
             const LaneMix *lanes, uint32_t frame_number, uint8_t *frame)
{
    const size_t pixels = table.led.size();
    // the brightness on its curve, in 1/65536
    const uint32_t scale = gamma.brightness[level];
    // the factors of every lane with the brightness in them, in 1/65536
    uint32_t factors[layout::kMaxLanes][3];
    for (size_t i = 0; i < table.lanes; i++) {
        factors[i][0] = scale * lanes[i].red >> 8;
        factors[i][1] = scale * lanes[i].green >> 8;
        factors[i][2] = scale * lanes[i].blue >> 8;
    }
    // Every channel of a dithering lane is dithered, also below one output step: a weak channel
    // that is rounded away changes the hue (250,0,60 stayed red up to brightness 12 and turned
    // pink at once, seen on the test matrix). LEDs that show the dithering as flicker (other chips
    // on one panel) get it off for their lane, and there the values are rounded.
    const auto value = [&](uint8_t c, uint32_t factor, uint32_t offset) {
        const auto v = static_cast<uint32_t>(static_cast<uint64_t>(gamma.table[c]) * factor >> 16);
        const uint32_t out = (v + offset) >> 8;
        return static_cast<uint8_t>(out > 255 ? 255 : out);
    };
    for (size_t i = 0; i < pixels; i++) {
        const uint16_t led = table.led[i];
        if (led == layout::kNone) {
            continue;
        }
        const uint8_t lane = table.lane[i];
        // without dithering the value is rounded
        const uint32_t offset = lanes[lane].dither ? dither_offset(frame_number, led) : 128;
        uint8_t *out = frame + static_cast<size_t>(led) * 3;
        out[0] = value(canvas[i].r, factors[lane][0], offset);
        out[1] = value(canvas[i].g, factors[lane][1], offset);
        out[2] = value(canvas[i].b, factors[lane][2], offset);
    }
}

uint32_t estimate_mw(const uint8_t *frame, size_t leds, const PowerModel &model)
{
    uint64_t sum = 0;
    for (size_t i = 0; i < leds * 3; i++) {
        sum += frame[i];
    }
    return static_cast<uint32_t>(leds * model.idle_mw + sum * model.channel_mw / 255);
}

uint32_t limit_power(uint8_t *frame, size_t leds, const PowerModel &model, uint32_t limit_mw, bool &limited)
{
    const uint32_t total = estimate_mw(frame, leds, model);
    limited = false;
    if (limit_mw == 0 || total <= limit_mw) {
        return total;
    }
    limited = true;
    const auto idle = static_cast<uint32_t>(leds * model.idle_mw);
    const uint32_t colors = total - idle;
    const uint32_t room = limit_mw > idle ? limit_mw - idle : 0;
    // the scale in 1/65536, rounded down so the result stays below the limit
    const auto scale = static_cast<uint32_t>(static_cast<uint64_t>(room) * 65536 / colors);
    for (size_t i = 0; i < leds * 3; i++) {
        frame[i] = static_cast<uint8_t>((frame[i] * scale) >> 16);
    }
    return estimate_mw(frame, leds, model);
}

} // namespace px::leds::detail
