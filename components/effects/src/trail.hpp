#pragma once

// The trail of the animations that draw (particles.cpp) and of the scripts that draw (glow,
// CONFIG_PX_SCRIPTS): an RGB canvas in the canvas' memory that fades, blurs and takes points,
// lines and discs, each keeping the lighter of the two colors. In an anonymous namespace on
// purpose: every file that includes it has its own copy, so particles.cpp compiles as it did
// before the scripts, and a firmware without them carries nothing of them.

#include <cstddef>
#include <cstdint>

#include "internal.hpp"

namespace px::effects::detail {

namespace {

// particles.cpp's (glow has none)
struct Scene;
struct Particle;

struct World {
    Memory *m;
    Rgb *trail;
    Scene *scene;
    Particle *particles;
    size_t slots;
    uint16_t w;
    uint16_t h;
};

// every channel of the trail keeps keep / 256 of its light
[[maybe_unused]] void fade(World &world, uint32_t keep)
{
    auto *p = reinterpret_cast<uint8_t *>(world.trail);
    const size_t n = static_cast<size_t>(world.w) * world.h * sizeof(Rgb);
    for (size_t i = 0; i < n; i++) {
        p[i] = static_cast<uint8_t>(p[i] * keep >> 8);
    }
}

[[maybe_unused]] uint8_t blurred(uint8_t before, uint8_t here, uint8_t after, uint32_t amount)
{
    const uint32_t mean = (before + 2u * here + after + 2u) / 4u;
    return static_cast<uint8_t>((here * (256u - amount) + mean * amount) >> 8);
}

// n pixels `stride` apart: each takes amount / 256 of the mean of itself and its neighbors (1 2 1);
// at the ends a pixel is its own neighbor
[[maybe_unused]] void blur_line(Rgb *p, size_t n, size_t stride, uint32_t amount)
{
    if (n < 2) {
        return;
    }
    Rgb before = p[0];
    for (size_t i = 0; i < n; i++) {
        Rgb &c = p[i * stride];
        const Rgb here = c;
        const Rgb after = i + 1 < n ? p[(i + 1) * stride] : here;
        c = {blurred(before.r, here.r, after.r, amount), blurred(before.g, here.g, after.g, amount),
             blurred(before.b, here.b, after.b, amount)};
        before = here;
    }
}

// the trail blurred along the rows, then the columns
[[maybe_unused]] void blur(World &world, uint32_t amount)
{
    for (uint16_t y = 0; y < world.h; y++) {
        blur_line(world.trail + static_cast<size_t>(y) * world.w, world.w, 1, amount);
    }
    for (uint16_t x = 0; x < world.w; x++) {
        blur_line(world.trail + x, world.h, world.w, amount);
    }
}

// the lighter of the trail and `color` times `weight` (0..256), each channel; off the canvas nothing
[[maybe_unused]] void plot(World &world, int32_t x, int32_t y, Rgb color, uint32_t weight)
{
    if (x < 0 || y < 0 || x >= world.w || y >= world.h || weight == 0) {
        return;
    }
    Rgb &p = world.trail[static_cast<size_t>(y) * world.w + static_cast<size_t>(x)];
    const auto r = static_cast<uint8_t>(color.r * weight >> 8);
    const auto g = static_cast<uint8_t>(color.g * weight >> 8);
    const auto b = static_cast<uint8_t>(color.b * weight >> 8);
    p = {p.r > r ? p.r : r, p.g > g ? p.g : g, p.b > b ? p.b : b};
}

// a point at x, y (1/256 pixel) on the four pixels around it, each by its share twice (a point
// between two pixels lights both, not each by half: the LEDs' gamma would make it dim)
[[maybe_unused]] void splat(World &world, int32_t x, int32_t y, Rgb color)
{
    const int32_t ix = x >> 8;
    const int32_t iy = y >> 8;
    const uint32_t fx = static_cast<uint32_t>(x) & 255u;
    const uint32_t fy = static_cast<uint32_t>(y) & 255u;
    const auto share = [](uint32_t a, uint32_t b) {
        const uint32_t weight = a * b >> 7;
        return weight > 256 ? 256u : weight;
    };
    plot(world, ix, iy, color, share(256u - fx, 256u - fy));
    plot(world, ix + 1, iy, color, share(fx, 256u - fy));
    plot(world, ix, iy + 1, color, share(256u - fx, fy));
    plot(world, ix + 1, iy + 1, color, share(fx, fy));
}

// from a (t 0) to b (t 256)
[[maybe_unused]] Rgb mix(Rgb a, Rgb b, uint32_t t)
{
    return {static_cast<uint8_t>((a.r * (256u - t) + b.r * t) >> 8), static_cast<uint8_t>((a.g * (256u - t) + b.g * t) >> 8),
            static_cast<uint8_t>((a.b * (256u - t) + b.b * t) >> 8)};
}

// a line from x0, y0 to x1, y1 (1/256 pixel), its color going from `from` to `to`
[[maybe_unused]] void line(World &world, int32_t x0, int32_t y0, int32_t x1, int32_t y1, Rgb from, Rgb to)
{
    const int32_t dx = x1 - x0;
    const int32_t dy = y1 - y0;
    const int32_t ax = dx < 0 ? -dx : dx;
    const int32_t ay = dy < 0 ? -dy : dy;
    // a point every half pixel
    const int32_t n = (ax > ay ? ax : ay) / 128 + 1;
    for (int32_t i = 0; i <= n; i++) {
        const int32_t t = i * 256 / n;
        splat(world, x0 + static_cast<int32_t>(static_cast<int64_t>(dx) * t / 256),
              y0 + static_cast<int32_t>(static_cast<int64_t>(dy) * t / 256), mix(from, to, static_cast<uint32_t>(t)));
    }
}

// a disc of `radius` (1/256 pixel, at most 200 pixels) around x, y: `firm` full nearly to its edge,
// else falling off from the middle (a soft blob); a small one is a point, the smaller the dimmer
[[maybe_unused]] void disc(World &world, int32_t x, int32_t y, int32_t radius, Rgb color, bool firm)
{
    if (radius < 384) {
        splat(world, x, y, scale(color, static_cast<uint32_t>(radius > 0 ? radius : 0) * 255u / 384u));
        return;
    }
    radius = radius > 200 * 256 ? 200 * 256 : radius;
    // in 1/16 pixel, so the squares fit 32 bits
    const int32_t r = radius >> 4;
    const auto r2 = static_cast<uint32_t>(r * r);
    const uint32_t inverse = (256u << 16) / r2;
    const int32_t x0 = (x - radius) >> 8 < 0 ? 0 : (x - radius) >> 8;
    const int32_t y0 = (y - radius) >> 8 < 0 ? 0 : (y - radius) >> 8;
    const int32_t x1 = (x + radius + 255) >> 8 >= world.w ? world.w - 1 : (x + radius + 255) >> 8;
    const int32_t y1 = (y + radius + 255) >> 8 >= world.h ? world.h - 1 : (y + radius + 255) >> 8;
    for (int32_t py = y0; py <= y1; py++) {
        const int32_t dy = (py * 256 - y) >> 4;
        for (int32_t px = x0; px <= x1; px++) {
            const int32_t dx = (px * 256 - x) >> 4;
            const auto d2 = static_cast<uint32_t>(dx * dx + dy * dy);
            if (d2 >= r2) {
                continue;
            }
            uint32_t f = (r2 - d2) * inverse >> 16;
            f = firm ? (f * 8 > 256 ? 256 : f * 8) : f;
            plot(world, px, py, color, f);
        }
    }
}

} // namespace

} // namespace px::effects::detail
