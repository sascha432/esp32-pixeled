// The animations (px/effects/effects.hpp, docs/leds.md section 6.3). Integer math only: the C3
// and C6 have no FPU, and 8192 pixels per frame leave no room for float on the others either.

#include "px/effects/effects.hpp"

#include <cstring>
#include <iterator>

namespace px::effects {

namespace {

// speed 0..255 as a phase step per millisecond in 1/65536 of a full turn: 255 is about two turns
// per second, 1 about one turn in two minutes
uint32_t phase(uint32_t ms, uint8_t speed, uint32_t scale = 1)
{
    const uint32_t per_ms = static_cast<uint32_t>(speed) * speed * 2 / 255 + (speed > 0 ? 1 : 0);
    return ms * per_ms * scale;
}

void solid(Canvas &canvas, const Params &params, uint32_t)
{
    const size_t n = static_cast<size_t>(canvas.width) * canvas.height;
    for (size_t i = 0; i < n; i++) {
        canvas.pixels[i] = params.color;
    }
}

// a rainbow along the diagonal of the canvas (along a strip on a canvas of one row)
void rainbow(Canvas &canvas, const Params &params, uint32_t ms)
{
    const uint32_t span = static_cast<uint32_t>(canvas.width) + canvas.height;
    const auto shift = static_cast<uint8_t>(phase(ms, params.speed) >> 8);
    for (uint16_t y = 0; y < canvas.height; y++) {
        for (uint16_t x = 0; x < canvas.width; x++) {
            const auto hue = static_cast<uint8_t>((x + y) * 256u / span + shift);
            canvas.at(x, y) = hsv(hue, 255, 255);
        }
    }
}

// the classic plasma: three moving sine waves summed, the sum as a hue
void plasma(Canvas &canvas, const Params &params, uint32_t ms)
{
    const auto t = static_cast<uint8_t>(phase(ms, params.speed) >> 9);
    const auto t2 = static_cast<uint8_t>(phase(ms, params.speed, 3) >> 11);
    for (uint16_t y = 0; y < canvas.height; y++) {
        for (uint16_t x = 0; x < canvas.width; x++) {
            const uint32_t v = sin8(static_cast<uint8_t>(x * 16 + t)) + sin8(static_cast<uint8_t>(y * 12 + t2)) +
                               sin8(static_cast<uint8_t>((x + y) * 8 + t + t2));
            canvas.at(x, y) = hsv(static_cast<uint8_t>(v / 3 + t), 240, 255);
        }
    }
}

// the color breathing in and out
void breathe(Canvas &canvas, const Params &params, uint32_t ms)
{
    const uint8_t level = sin8(static_cast<uint8_t>(phase(ms, params.speed) >> 9));
    const Rgb c{static_cast<uint8_t>(params.color.r * level / 255), static_cast<uint8_t>(params.color.g * level / 255),
                static_cast<uint8_t>(params.color.b * level / 255)};
    const size_t n = static_cast<size_t>(canvas.width) * canvas.height;
    for (size_t i = 0; i < n; i++) {
        canvas.pixels[i] = c;
    }
}

// the wiring check of docs/leds.md section 4.1: top red, right green, bottom blue, left yellow,
// the top left pixel white, the rest dark
void edges(Canvas &canvas, const Params &, uint32_t)
{
    std::memset(canvas.pixels, 0, static_cast<size_t>(canvas.width) * canvas.height * sizeof(Rgb));
    const uint16_t w = canvas.width;
    const uint16_t h = canvas.height;
    for (uint16_t y = 0; y < h; y++) {
        canvas.at(0, y) = {255, 255, 0};
        canvas.at(w - 1, y) = {0, 255, 0};
    }
    for (uint16_t x = 0; x < w; x++) {
        canvas.at(x, 0) = {255, 0, 0};
        canvas.at(x, h - 1) = {0, 0, 255};
    }
    canvas.at(0, 0) = {255, 255, 255};
}

using RenderFunction = void (*)(Canvas &, const Params &, uint32_t);

struct Entry {
    Info info;
    RenderFunction render;
};

constexpr Entry kEffects[] = {
    {{"solid", "Solid", Reactive::Never, true}, solid},
    {{"rainbow", "Rainbow", Reactive::Optional, false}, rainbow},
    {{"plasma", "Plasma", Reactive::Optional, false}, plasma},
    {{"breathe", "Breathe", Reactive::Optional, true}, breathe},
    {{"edges", "Test: edges", Reactive::Never, false}, edges},
};

} // namespace

size_t count()
{
    return std::size(kEffects);
}

const Info *info(size_t index)
{
    return index < count() ? &kEffects[index].info : nullptr;
}

size_t find(const char *slug)
{
    for (size_t i = 0; i < count(); i++) {
        if (std::strcmp(kEffects[i].info.slug, slug) == 0) {
            return i;
        }
    }
    return count();
}

void render(size_t index, Canvas &canvas, const Params &params, uint32_t ms)
{
    if (canvas.width == 0 || canvas.height == 0) {
        return;
    }
    kEffects[index < count() ? index : 0].render(canvas, params, ms);
}

Rgb hsv(uint8_t hue, uint8_t saturation, uint8_t value)
{
    // six sectors of 43 hue steps
    const uint8_t sector = hue / 43;
    const auto rest = static_cast<uint8_t>((hue - sector * 43) * 6);
    const auto p = static_cast<uint8_t>(value * (255 - saturation) / 255);
    const auto q = static_cast<uint8_t>(value * (255 - saturation * rest / 255) / 255);
    const auto t = static_cast<uint8_t>(value * (255 - saturation * (255 - rest) / 255) / 255);
    switch (sector) {
    case 0:
        return {value, t, p};
    case 1:
        return {q, value, p};
    case 2:
        return {p, value, t};
    case 3:
        return {p, q, value};
    case 4:
        return {t, p, value};
    default:
        return {value, p, q};
    }
}

uint8_t sin8(uint8_t angle)
{
    // a quarter wave, 65 values from 0 to 127: sin(i * pi / 128) * 127
    static constexpr uint8_t kQuarter[65] = {
        0,   3,   6,   9,   12,  16,  19,  22,  25,  28,  31,  34,  37,  40,  43,  46,  49,
        51,  54,  57,  60,  63,  65,  68,  71,  73,  76,  78,  81,  83,  85,  88,  90,  92,
        94,  96,  98,  100, 102, 104, 106, 107, 109, 111, 112, 113, 115, 116, 117, 118, 120,
        121, 122, 122, 123, 124, 125, 125, 126, 126, 126, 127, 127, 127, 127,
    };
    const uint8_t quarter = angle & 63;
    const uint8_t index = (angle & 64) != 0 ? 64 - quarter : quarter;
    const uint8_t value = kQuarter[index];
    return (angle & 128) != 0 ? static_cast<uint8_t>(128 - value) : static_cast<uint8_t>(128 + value);
}

} // namespace px::effects
