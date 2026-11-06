#pragma once

// What the built-in animations (effects.cpp) share with the scripts (glow, CONFIG_PX_SCRIPTS): the
// quarter turns of render() and fire's colors. In an anonymous namespace on purpose: every file that
// includes it has its own copy, so effects.cpp compiles as it did before the scripts, and a firmware
// without them carries nothing of them.

#include <cstdint>

#include "internal.hpp"

namespace px::effects::detail {

namespace {

// `draw(canvas)` on the whole canvas, turned by `p.rotation`: for a quarter turn drawn on the spare
// canvas (h x w) and turned onto the canvas, for half a turn reversed in place
template <class Draw>
void render_turned(Canvas &canvas, const Params &p, Draw draw)
{
    const uint8_t rotation = p.rotation & 3;
    const uint16_t w = canvas.width;
    const uint16_t h = canvas.height;
    if ((rotation == 1 || rotation == 3) && canvas.spare != nullptr) {
        // drawn on the spare canvas, h x w, then turned onto the canvas
        Canvas turned{h, w, canvas.spare, canvas.memory, canvas.memory_size, nullptr};
        draw(turned);
        for (uint16_t y = 0; y < h; y++) {
            for (uint16_t x = 0; x < w; x++) {
                // clockwise: the turned canvas' top left lands top right; the other way bottom left
                canvas.at(x, y) = rotation == 1 ? turned.at(y, static_cast<uint16_t>(w - 1 - x))
                                                : turned.at(static_cast<uint16_t>(h - 1 - y), x);
            }
        }
        return;
    }
    draw(canvas);
    if (rotation == 2) {
        // half a turn: the pixels in reverse order
        const size_t n = static_cast<size_t>(w) * h;
        for (size_t i = 0; i < n / 2; i++) {
            const Rgb swap = canvas.pixels[i];
            canvas.pixels[i] = canvas.pixels[n - 1 - i];
            canvas.pixels[n - 1 - i] = swap;
        }
    }
}

// black, red, yellow, white; with a palette its colors, dark at no heat (the palette's end is not
// blended into its start)
[[maybe_unused]] Rgb heat_color(uint8_t heat, const Params &params)
{
    if (params.palette != 0) {
        return scale(palette_color(params.palette, static_cast<uint8_t>(heat * 240u / 255u), params.color),
                     heat < 85 ? heat * 3u : 255u);
    }
    const auto t = static_cast<uint8_t>(heat * 191u / 255u);
    const auto ramp = static_cast<uint8_t>((t & 63u) << 2);
    if ((t & 0x80) != 0) {
        return {255, 255, ramp};
    }
    if ((t & 0x40) != 0) {
        return {255, ramp, 0};
    }
    return {ramp, 0, 0};
}

} // namespace

} // namespace px::effects::detail
