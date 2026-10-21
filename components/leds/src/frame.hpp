#pragma once

// From the canvas to the frame of the output (docs/leds.md section 3): gamma, brightness, the
// layout's table, temporal dithering, the power estimate and its limit. Pure C++, host tested.

#include <cstddef>
#include <cstdint>

#include "px/effects/effects.hpp"
#include "px/layout/layout.hpp"

namespace px::leds::detail {

// gamma 2.2 of a color value as 16 bits: what the eye sees as half is about a fifth of the light
struct Gamma {
    uint16_t table[256];
    // The brightness (0..255, the slider and the fade) as the factor of the light in 1/65536, on a
    // perceptual curve: 1 is one output step of a full channel, the lowest the LEDs show, 255 is
    // all of them, and the steps between grow like the eye's (gamma 2.2 from the first output step
    // up, 2 % linear). Brightness 2 to about 16 lie between one and two output steps: only
    // dithering shows them. A linear brightness made every slider step one output step, and the dark end jumped
    // by doubling (seen on the test matrix).
    uint16_t brightness[256];
    Gamma();
};

// The fraction of the 16-bit values that dithering spreads over the frames: a value of 2.4 is 2
// in three frames out of five and 3 in the other two. `frame` counts the frames, every LED gets an
// offset of its own so the LEDs do not step together. 0 for every frame: no dithering.
uint8_t dither_offset(uint32_t frame, uint16_t led);

// What one lane does to its colors (docs/leds.md section 3, per-lane correction): a factor per
// channel in 1/256 (256 keeps the value; the lane's brightness times its color balance) and
// whether its LEDs take dithering.
struct LaneMix {
    uint16_t red = 256;
    uint16_t green = 256;
    uint16_t blue = 256;
    bool dither = true;
};

// Writes every canvas pixel that has an LED into `frame` (3 bytes per LED, R G B): the color
// through `gamma`, times the brightness `level` (0..255, Gamma::brightness) and the factors of its
// lane in `lanes` (one per
// lane of the table), the 8 bits below the output's 8 spread over the frames where the lane
// dithers. LEDs without a canvas pixel stay as they are.
void compose(const effects::Rgb *canvas, const layout::Table &table, uint8_t level, const Gamma &gamma,
             const LaneMix *lanes, uint32_t frame_number, uint8_t *frame);

struct PowerModel {
    uint32_t channel_mw;
    uint32_t idle_mw;
};

// the estimate of a frame of `leds` LEDs
uint32_t estimate_mw(const uint8_t *frame, size_t leds, const PowerModel &model);

// Scales the frame down until its estimate is at most `limit_mw` (0: no limit). Returns the
// estimate afterwards; `limited` says whether it scaled.
uint32_t limit_power(uint8_t *frame, size_t leds, const PowerModel &model, uint32_t limit_mw, bool &limited);

} // namespace px::leds::detail
