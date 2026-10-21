#pragma once

// The animations (docs/leds.md section 6): each draws a whole canvas from its parameters and the
// time, without state of its own between frames (the ones that need state come with phase 4).
// Pure C++, host tested.

#include <cstddef>
#include <cstdint>

namespace px::effects {

struct Rgb {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

struct Canvas {
    uint16_t width;
    uint16_t height;
    // width x height pixels, row by row
    Rgb *pixels;

    Rgb &at(uint16_t x, uint16_t y)
    {
        return pixels[static_cast<size_t>(y) * width + x];
    }
};

// whether an animation reacts to audio (docs/leds.md section 6.2)
enum class Reactive : uint8_t {
    Never,
    Optional,
    Required,
};

struct Params {
    Rgb color = {255, 120, 40};
    // 0 stands still, 255 is the fastest
    uint8_t speed = 128;
};

struct Info {
    // the name in MQTT, the API and the backup: lower case, '-'
    const char *slug;
    // the name in the web UI and in Home Assistant's effect list
    const char *name;
    Reactive reactive;
    // it shows Params::color
    bool uses_color;
};

size_t count();
// nullptr for an index from count() on
const Info *info(size_t index);
// the index of `slug`, count() when there is none
size_t find(const char *slug);

// Draws animation `index` at `ms` (a clock that only moves forward) on the whole canvas.
void render(size_t index, Canvas &canvas, const Params &params, uint32_t ms);

// helpers, public for the tests
Rgb hsv(uint8_t hue, uint8_t saturation, uint8_t value);
uint8_t sin8(uint8_t angle);

} // namespace px::effects
