#pragma once

// Text and the clock over an animation (docs/leds.md section 10): a 5 x 7 font, proportional, one
// pixel between the characters, ASCII and the German letters (UTF-8); the clock in the same font,
// or in 3 x 5 digits when that does not fit the canvas. Pure C++, host tested.

#include <cstdint>

#include "px/effects/effects.hpp"

namespace px::effects {

constexpr uint16_t kTextHeight = 7;

// the width of `text` in pixels
uint16_t text_width(const char *text);

// Draws `text` with its top left corner at `x`, `y` (either may be outside: clipped).
void draw_text(Canvas &canvas, const char *text, int x, int y, Rgb color);

// Where a text of `width` pixels starts on a canvas `canvas_width` wide at `ms`: centered when it
// fits, else scrolling in from the right and out to the left, `speed` 1 about 5 and 255 about 50
// pixels a second (0 stands at the left edge).
int text_x(uint16_t canvas_width, uint16_t width, uint8_t speed, uint32_t ms);

// "hh:mm" in the middle of the canvas; the colon blinks with `colon`.
void draw_clock(Canvas &canvas, uint8_t hours, uint8_t minutes, bool colon, Rgb color);

} // namespace px::effects
