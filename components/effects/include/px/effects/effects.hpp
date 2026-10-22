#pragma once

// The animations (docs/leds.md section 6): each draws a whole canvas from its parameters and the
// time. The ones that need state between frames (fire, the reactive ones) keep it in the canvas'
// memory, which the render task gives and clears. Each animation can be turned in quarter turns
// (Params::rotation); render() does that for all of them. The ones with colors of their own (a
// rainbow, the heat of fire) can take them from a palette instead (Params::palette, docs/leds.md
// section 6.4). Pure C++, host tested.

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
    // the animation's memory between frames, memory_size(width, height) bytes, all 0 for a new
    // animation, rotation or canvas; nullptr: the animations that need it draw without it
    uint8_t *memory = nullptr;
    size_t memory_size = 0;
    // a second width x height canvas for the quarter turns; nullptr: rotation 1 and 3 do nothing
    Rgb *spare = nullptr;

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

// What the audio input heard last (docs/leds.md section 6.1): the same from every source (the
// microphone, later UDP), 0..255 each, already scaled to the music's own range.
constexpr size_t kBands = 32;

struct Audio {
    // a source runs and hears more than silence
    bool active = false;
    uint8_t loudness = 0;
    // log-spaced from about 50 Hz (band 0) to 10 kHz (band 31)
    uint8_t bands[kBands] = {};
    // a beat in the bass since the last frame
    bool beat = false;
};

struct Params {
    Rgb color = {255, 120, 40};
    // 0 stands still, 255 is the fastest
    uint8_t speed = 128;
    // the audio input; nullptr without one
    const Audio *audio = nullptr;
    // quarter turns clockwise, 0..3: the picture turned by 0, 90, 180 or 270 degrees
    uint8_t rotation = 0;
    // an animation with Reactive::Optional follows the audio (one with Required always does)
    bool reactive = false;
    // how strongly a reactive animation follows the audio: 128 as built, 255 twice as much, 0 not
    // at all (like `reactive` off)
    uint8_t strength = 128;
    // how fast what it hears (the loudness, the bands) rises and falls: the milliseconds to get
    // there, 0 at once
    uint16_t attack_ms = 20;
    uint16_t release_ms = 250;
    // an index of the palettes (palette_info), 0: the animation's own colors
    uint8_t palette = 0;
};

constexpr uint16_t kMaxAttackMs = 2000;
constexpr uint16_t kMaxReleaseMs = 5000;

struct Info {
    // the name in MQTT, the API and the backup: lower case, '-'
    const char *slug;
    // the name in the web UI and in Home Assistant's effect list
    const char *name;
    Reactive reactive;
    // it shows Params::color
    bool uses_color;
    // it takes its colors from Params::palette
    bool uses_palette;
};

size_t count();
// the animations before the test patterns (they come last)
size_t animation_count();
// nullptr for an index from count() on
const Info *info(size_t index);
// the index of `slug`, count() when there is none
size_t find(const char *slug);

// The palettes (docs/leds.md section 6.4): 16 colors around a circle, blended between them.
// Palette 0 is none (the animation's own colors); "color" is made from Params::color.
struct PaletteInfo {
    const char *slug;
    const char *name;
};

size_t palette_count();
// nullptr for an index from palette_count() on
const PaletteInfo *palette_info(size_t index);
// the index of `slug`, palette_count() when there is none
size_t find_palette(const char *slug);
// the color at `index` (0..255 once around) of palette `palette`; `color` for the "color" palette.
// Palette 0 (none) gives the rainbow.
Rgb palette_color(uint8_t palette, uint8_t index, Rgb color);

// The memory a canvas of `width` x `height` needs for every animation (Canvas::memory).
size_t memory_size(uint16_t width, uint16_t height);

// Draws animation `index` at `ms` (a clock that only moves forward) on the whole canvas, turned by
// Params::rotation.
void render(size_t index, Canvas &canvas, const Params &params, uint32_t ms);

// The crossfade (docs/leds.md section 3): `amount` 0 keeps `into`, 255 is `from`.
void blend(Rgb *into, const Rgb *from, size_t count, uint8_t amount);

// helpers, public for the tests
Rgb hsv(uint8_t hue, uint8_t saturation, uint8_t value);
uint8_t sin8(uint8_t angle);
// smooth value noise, 0..255; x and y in 1/256 of a lattice cell, z the time
uint8_t noise8(uint32_t x, uint32_t y, uint32_t z);

} // namespace px::effects
