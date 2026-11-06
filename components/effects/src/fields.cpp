// The second set of animations after WLED's 2D ones that work out every pixel (docs/leds.md
// section 6.3): Fire 2012, firenoise, hiphotic, octopus, Julia, polar lights, soap and sun
// radiation. Written from WLED's descriptions and how they look, not from its code; WLED's sliders
// beyond the speed are fixed values here.
//
// Their time is a phase kept in the memory and moved on by the time since the last frame (faster
// with the sound), so a change of the speed does not make the picture jump, and speed 0 stands
// still. Integer math only, like effects.cpp.

#include "internal.hpp"
#include "shared.hpp"

namespace px::effects::detail {

namespace {

// a frame of these animations: the phase (at speed 128 about 129 per millisecond, as plasma's),
// what they hear and the hue the beats moved the colors on
struct Clock {
    uint32_t phase;
    Sound sound;
    uint8_t hue;
};

Clock clock_of(Canvas &canvas, const Params &params, uint32_t ms)
{
    Memory *m = memory_of(canvas);
    if (m == nullptr) {
        return {phase(ms, params.speed), {false, 0, false, 0, 255}, 0};
    }
    const uint32_t dt = elapsed(*m, ms);
    const Sound sound = listen(*m, params, dt);
    m->phase += dt * per_ms(params.speed) * faster(sound, params.strength) / 64u;
    return {m->phase, sound, beat_hue(*m, sound, dt, 32u * params.strength / 128u)};
}

uint32_t isqrt32(uint32_t n)
{
    uint32_t root = 0;
    uint32_t bit = 1u << 30;
    while (bit > n) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (n >= root + bit) {
            n -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

// the angle of dx, dy in 1/65536 of a turn, 0 along x, a quarter along y: the octant, then an
// arc tangent of the smaller over the larger side (within about a tenth of a degree)
uint32_t angle_of(int32_t dx, int32_t dy)
{
    const uint32_t ax = static_cast<uint32_t>(dx < 0 ? -dx : dx);
    const uint32_t ay = static_cast<uint32_t>(dy < 0 ? -dy : dy);
    if (ax == 0 && ay == 0) {
        return 0;
    }
    const uint32_t lo = ax < ay ? ax : ay;
    const uint32_t hi = ax < ay ? ay : ax;
    const uint32_t z = lo * 256u / hi;
    // atan(z) in turns, z 0..1: z / 8 + z (1 - z) (0.0389 + 0.0105 z)
    uint32_t a = 32u * z + z * (256u - z) * (2552u + 691u * z / 256u) / 65536u;
    a = ay > ax ? 16384u - a : a;
    a = dx < 0 ? 32768u - a : a;
    a = dy < 0 ? 65536u - a : a;
    return a & 0xffffu;
}

uint8_t clamp8(int32_t v)
{
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

} // namespace

// Fire 2012 (after WLED's, the classic "Fire2012"): a fire in every column on its own: each step
// every cell cools a little, the heat drifts up (a cell takes from the two below it) and now and
// then a spark lights in the lowest cells (seven, fewer in a short column); black, red, yellow,
// white (the palette's colors with one). The columns flicker each on its own, unlike Fire, whose
// heat spreads sideways. The speed sets 20 to 100 steps a second; at speed 0 a second of them, then
// it stands. The cooling (80, Fire2012's 55 burns to the top of a matrix) and the sparks (120)
// are fixed. Reactive: each column sparks with its band (bass left, treble right), a beat sparks
// every column; dark in silence, like Fire.
void fire_2012(Canvas &canvas, const Params &params, uint32_t ms)
{
    const uint16_t w = canvas.width;
    const uint16_t h = canvas.height;
    Memory *m = memory_of(canvas);
    if (m == nullptr) {
        clear(canvas);
        return;
    }
    // a column after the other, the bottom cell first
    uint8_t *heat = cells_of(canvas);
    const bool first = !m->started;
    const uint32_t dt = elapsed(*m, ms);
    const Sound sound = listen(*m, params, dt);
    uint8_t bands[kBands];
    hear_bands(m, params, dt, bands);
    uint32_t steps = first && params.speed == 0 ? 60u : 0u;
    if (params.speed != 0) {
        m->carry += dt * (20u + params.speed * 80u / 255u);
        steps = m->carry / 1000;
        m->carry %= 1000;
        steps = steps > 4 ? 4 : steps;
    }
    const uint32_t cool_max = 800u / h + 2u;
    const uint32_t zone = h / 4u + 1u < 7u ? h / 4u + 1u : 7u;
    for (uint32_t step = 0; step < steps; step++) {
        for (uint16_t x = 0; x < w; x++) {
            uint8_t *column = heat + static_cast<size_t>(x) * h;
            for (uint16_t k = 0; k < h; k++) {
                const uint32_t cool = random8(*m) * (cool_max + 1u) >> 8;
                column[k] = column[k] > cool ? static_cast<uint8_t>(column[k] - cool) : 0;
            }
            for (uint16_t k = h - 1; k >= 2 && k < h; k--) {
                column[k] = static_cast<uint8_t>((column[k - 1] + column[k - 2] * 2u) / 3u);
            }
            uint32_t chance = 120;
            if (sound.on) {
                chance = bands[static_cast<size_t>(x) * kBands / w] * 3u / 4u;
                chance = sound.beat && step == 0 ? 256u : chance;
            }
            if (random8(*m) < chance) {
                uint8_t &c = column[random_below(*m, zone)];
                const uint32_t added = c + 160u + random_below(*m, 96);
                c = static_cast<uint8_t>(added > 255 ? 255 : added);
            }
        }
    }
    for (uint16_t x = 0; x < w; x++) {
        const uint8_t *column = heat + static_cast<size_t>(x) * h;
        for (uint16_t k = 0; k < h; k++) {
            canvas.at(x, static_cast<uint16_t>(h - 1 - k)) = heat_color(column[k], params);
        }
    }
}

// Firenoise (after WLED's): fire made of noise: smooth noise in tall cells (4 by 16 pixels) rising
// up the canvas, about two cells a second at speed 128, hot at the bottom and cooling toward the
// top; black, red, yellow, white (the palette's colors with one). Reactive: faster and brighter with
// the sound.
void firenoise(Canvas &canvas, const Params &params, uint32_t ms)
{
    const uint16_t w = canvas.width;
    const uint16_t h = canvas.height;
    if (memory_of(canvas) == nullptr) {
        clear(canvas);
        return;
    }
    const Clock c = clock_of(canvas, params, ms);
    uint8_t *row = cells_of(canvas);
    const uint32_t rise = c.phase >> 8;
    const uint32_t z = c.phase >> 10;
    for (uint16_t y = 0; y < h; y++) {
        // 255 at the bottom, 0 at the top
        const uint32_t low = h > 1 ? y * 255u / (h - 1u) : 255u;
        noise_row(row, w, 0, 64, y * 16u + rise, z);
        for (uint16_t x = 0; x < w; x++) {
            const auto heat = clamp8(static_cast<int32_t>(row[x] * (low + 32u) / 192u) - 24);
            canvas.at(x, y) = scale(heat_color(heat, params), c.sound.value);
        }
    }
}

// Hiphotic (after WLED's): a moving plasma: the sine of the mean of a sine along x and one along y,
// a wave and a half across each side, in the palette's colors (the hue circle without one).
// Reactive: faster and brighter with the sound, a beat moves the colors on.
void hiphotic(Canvas &canvas, const Params &params, uint32_t ms)
{
    const Clock c = clock_of(canvas, params, ms);
    const uint16_t w = canvas.width;
    const uint16_t h = canvas.height;
    const auto a = static_cast<uint8_t>(c.phase >> 10);
    const auto b = static_cast<uint8_t>(c.phase * 3u >> 12);
    const auto d = static_cast<uint8_t>(c.phase >> 11);
    const uint32_t sx = (384u + w / 2u) / w;
    const uint32_t sy = (384u + h / 2u) / h;
    for (uint16_t y = 0; y < h; y++) {
        const uint8_t across_y = sin8(static_cast<uint8_t>(y * sy + b));
        for (uint16_t x = 0; x < w; x++) {
            // a cosine along x: a quarter turn on
            const uint8_t v =
                sin8(static_cast<uint8_t>((sin8(static_cast<uint8_t>(x * sx + a + 64u)) + across_y) / 2u * 3u / 2u + d));
            canvas.at(x, y) = paint(params, static_cast<uint8_t>(v + c.hue), 255, c.sound.value);
        }
    }
}

// Octopus (after WLED's): a creature stuck in a whirlpool: four arms winding out from the middle,
// waving as a wave runs out along them, turning slowly; the colors run in toward the middle (the
// palette's with one, else the hue circle). The angle and the distance of every pixel are worked
// out once and kept in the memory. Reactive: faster and brighter with the sound, a beat moves the
// colors on.
void octopus(Canvas &canvas, const Params &params, uint32_t ms)
{
    const uint16_t w = canvas.width;
    const uint16_t h = canvas.height;
    Memory *m = memory_of(canvas);
    // the angle (1/256 turn) and the distance (the longer half side 128) of each pixel
    uint8_t *polar = m != nullptr ? cells_of(canvas) : nullptr;
    const bool first = m != nullptr && !m->started;
    const Clock c = clock_of(canvas, params, ms);
    const uint32_t longer = w > h ? w : h;
    const auto t = static_cast<uint8_t>(c.phase >> 10);
    constexpr uint32_t kArms = 4;
    for (uint16_t y = 0; y < h; y++) {
        // in half pixels from the middle
        const int32_t dy = 2 * y - (h - 1);
        for (uint16_t x = 0; x < w; x++) {
            const size_t i = static_cast<size_t>(y) * w + x;
            uint8_t angle;
            uint8_t radius;
            if (polar == nullptr || first) {
                const int32_t dx = 2 * x - (w - 1);
                angle = static_cast<uint8_t>(angle_of(dx, dy) >> 8);
                radius = static_cast<uint8_t>(isqrt32(static_cast<uint32_t>(dx * dx + dy * dy)) * 128u / longer);
                if (polar != nullptr) {
                    polar[2 * i] = angle;
                    polar[2 * i + 1] = radius;
                }
            } else {
                angle = polar[2 * i];
                radius = polar[2 * i + 1];
            }
            const uint8_t wave = sin8(static_cast<uint8_t>(radius * 3u - t * 2u)) >> 2;
            const uint32_t arm = sin8(static_cast<uint8_t>(angle * kArms + radius / 2u + wave - t));
            const uint32_t light = arm * arm >> 8;
            canvas.at(x, y) =
                paint(params, static_cast<uint8_t>(radius * 2u + t + c.hue), 255, static_cast<uint8_t>(light * c.sound.value / 255u));
        }
    }
}

// Julia (after WLED's): the Julia set of a point going along the edge of the Mandelbrot set's
// heart (a little inside it: once around in about a minute at speed 128), so the fractal grows
// arms, curls, splits into rabbits and joins again; 3 across the shorter side. Each pixel is
// colored by the steps it takes to leave (up to 24, fewer on a large canvas) in the palette's colors
// (the hue circle without one), moving on; dark inside the set and far out. Reactive: faster and
// brighter with the sound, a beat moves the colors on.
void julia(Canvas &canvas, const Params &params, uint32_t ms)
{
    const Clock c = clock_of(canvas, params, ms);
    const uint16_t w = canvas.width;
    const uint16_t h = canvas.height;
    const uint32_t pixels = static_cast<uint32_t>(w) * h;
    const uint32_t budget = 160000u / pixels;
    const uint32_t most = budget < 10 ? 10 : (budget > 24 ? 24 : budget);
    // 1/4096
    constexpr int32_t kOne = 4096;
    // the heart's edge: e^ia / 2 - e^2ia / 4, times 0.98; from a third of the way round (the
    // rabbits), not from its cusp (a plain blob)
    const uint32_t angle = (c.phase >> 7) + 21845u;
    const int32_t cr = ((cos16(angle) * 2007 >> 15) - (cos16(angle * 2u) * 1003 >> 15));
    const int32_t ci = ((sin16(angle) * 2007 >> 15) - (sin16(angle * 2u) * 1003 >> 15));
    const uint32_t shorter = w < h ? w : h;
    const auto per_pixel = static_cast<int32_t>(3u * kOne / shorter);
    const auto spin = static_cast<uint8_t>(c.phase >> 12);
    const auto start = [](int32_t v) { return v < -4 * kOne ? -4 * kOne : (v > 4 * kOne ? 4 * kOne : v); };
    for (uint16_t y = 0; y < h; y++) {
        const int32_t y0 = start((2 * y - (h - 1)) * per_pixel / 2);
        for (uint16_t x = 0; x < w; x++) {
            int32_t zx = start((2 * x - (w - 1)) * per_pixel / 2);
            int32_t zy = y0;
            uint32_t k = 0;
            for (; k < most; k++) {
                const int32_t xx = zx * zx >> 12;
                const int32_t yy = zy * zy >> 12;
                if (xx + yy > 4 * kOne) {
                    break;
                }
                zy = (zx * zy >> 11) + ci;
                zx = xx - yy + cr;
            }
            if (k == most) {
                canvas.at(x, y) = {0, 0, 0};
                continue;
            }
            const uint32_t light = k * 64u > 255u ? 255u : k * 64u;
            canvas.at(x, y) = paint(params, static_cast<uint8_t>(k * 20u + spin + c.hue), 255,
                                    static_cast<uint8_t>(light * c.sound.value / 255u));
        }
    }
}

// Polar lights (after WLED's): the northern lights: curtains hanging from the top in folds that
// run down (fine across, long down), waving slowly sideways and flickering; their lower edge waves
// between 40 and 85 % of the height down, brightest there and fading upward, dark below. Green at
// the edge, turning blue and violet above (the palette's colors from the edge up with one).
// Reactive: faster and brighter with the sound.
void polar_lights(Canvas &canvas, const Params &params, uint32_t ms)
{
    const uint16_t w = canvas.width;
    const uint16_t h = canvas.height;
    if (memory_of(canvas) == nullptr) {
        clear(canvas);
        return;
    }
    const Clock c = clock_of(canvas, params, ms);
    // the noise along the edge, then of a row
    uint8_t *edges = cells_of(canvas);
    uint8_t *row = edges + w;
    const uint32_t z = c.phase >> 11;
    const uint32_t drift = c.phase >> 10;
    noise_row(edges, w, drift / 2u, 20, 0x3000u, z);
    for (uint16_t y = 0; y < h; y++) {
        noise_row(row, w, drift, 64, y * 12u, z);
        for (uint16_t x = 0; x < w; x++) {
            const uint32_t edge = h * (100u + edges[x] * 120u / 255u) / 256u;
            if (y > edge + 1u) {
                canvas.at(x, y) = {0, 0, 0};
                continue;
            }
            const uint32_t n = row[x];
            const uint32_t stretched = n < 64 ? 0 : (n - 64u) * 2u;
            const uint32_t fold = stretched > 255 ? 255u : stretched;
            const uint32_t above = y <= edge ? edge - y : 0u;
            const uint32_t profile = y <= edge ? 255u - above * 160u / (edge + 1u) : 95u;
            const uint32_t light = fold * profile / 255u * c.sound.value / 255u;
            const uint32_t up = above * 255u / (edge + 1u);
            canvas.at(x, y) = params.palette == 0
                                  ? hsv(static_cast<uint8_t>(96u + up * 96u / 255u), 230, static_cast<uint8_t>(light))
                                  : scale(palette_color(params.palette, static_cast<uint8_t>(up), params.color), light);
        }
    }
}

// Soap (after WLED's): the colors of a soap film: smooth noise smeared in swirls, each row pushed
// sideways and each column up or down by more noise (up to two and a half cells of 6 pixels), all
// drifting slowly; in the palette's colors twice around (the hue circle without one). Reactive:
// faster and brighter with the sound, a beat moves the colors on.
void soap(Canvas &canvas, const Params &params, uint32_t ms)
{
    const uint16_t w = canvas.width;
    const uint16_t h = canvas.height;
    if (memory_of(canvas) == nullptr) {
        clear(canvas);
        return;
    }
    const Clock c = clock_of(canvas, params, ms);
    const uint32_t z = c.phase >> 10;
    const auto turn = static_cast<uint8_t>((c.phase >> 13) + c.hue);
    // how far each column is pushed
    uint8_t *columns = cells_of(canvas);
    noise_row(columns, w, 0, 20, z, 0x1234u);
    for (uint16_t y = 0; y < h; y++) {
        const int32_t dx = (static_cast<int32_t>(noise8(y * 20u, z, 0x5a5au)) - 128) * 5;
        for (uint16_t x = 0; x < w; x++) {
            const int32_t dy = (static_cast<int32_t>(columns[x]) - 128) * 5;
            const uint8_t n = noise8(static_cast<uint32_t>(static_cast<int32_t>(x * 40u + 0x8000u) + dx),
                                     static_cast<uint32_t>(static_cast<int32_t>(y * 40u + 0x8000u) + dy), z);
            canvas.at(x, y) = paint(params, static_cast<uint8_t>(n * 2u + turn), 255, c.sound.value);
        }
    }
}

// Sun radiation (after WLED's): the face of the sun: two layers of noise boiling slowly, lit from
// the top left like a relief (lighter where the surface faces the light, darker behind), in the
// colors of heat, orange to light yellow (the palette's colors with one). The relief is kept in the
// memory for the light. Reactive: faster and brighter with the sound.
void sun_radiation(Canvas &canvas, const Params &params, uint32_t ms)
{
    const uint16_t w = canvas.width;
    const uint16_t h = canvas.height;
    if (memory_of(canvas) == nullptr) {
        clear(canvas);
        return;
    }
    const Clock c = clock_of(canvas, params, ms);
    uint8_t *bump = cells_of(canvas);
    // the finer layer of a row
    uint8_t *fine = bump + static_cast<size_t>(w) * h;
    const uint32_t z = c.phase >> 10;
    for (uint16_t y = 0; y < h; y++) {
        uint8_t *coarse = bump + static_cast<size_t>(y) * w;
        noise_row(coarse, w, 0, 48, y * 48u, z);
        noise_row(fine, w, 0x4000u, 96, y * 96u, z * 2u);
        for (uint16_t x = 0; x < w; x++) {
            coarse[x] = static_cast<uint8_t>((coarse[x] * 3u + fine[x]) / 4u);
        }
    }
    const auto at = [&](int32_t x, int32_t y) -> int32_t {
        x = x < 0 ? 0 : (x >= w ? w - 1 : x);
        y = y < 0 ? 0 : (y >= h ? h - 1 : y);
        return bump[static_cast<size_t>(y) * w + static_cast<size_t>(x)];
    };
    for (uint16_t y = 0; y < h; y++) {
        for (uint16_t x = 0; x < w; x++) {
            const int32_t here = at(x, y);
            const int32_t slope = at(x - 1, y) - at(x + 1, y) + at(x, y - 1) - at(x, y + 1);
            const uint8_t heat = clamp8(128 + (here - 128) / 2 + slope * 3 / 2);
            canvas.at(x, y) = scale(heat_color(heat, params), c.sound.value);
        }
    }
}

} // namespace px::effects::detail
