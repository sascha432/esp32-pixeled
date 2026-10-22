// The animations (px/effects/effects.hpp, docs/leds.md section 6.3). Integer math only: the C3
// and C6 have no FPU, and 8192 pixels per frame leave no room for float on the others either.

#include "px/effects/effects.hpp"

#include <cstring>
#include <iterator>

namespace px::effects {

namespace {

// speed 0..255 as a phase step per millisecond in 1/65536 of a full turn: 255 is about two turns
// per second, 1 about one turn in two minutes
uint32_t per_ms(uint8_t speed)
{
    return static_cast<uint32_t>(speed) * speed * 2 / 255 + (speed > 0 ? 1 : 0);
}

uint32_t phase(uint32_t ms, uint8_t speed, uint32_t scale = 1)
{
    return ms * per_ms(speed) * scale;
}

// ------------------------------------------------------------------ memory between frames

// ripple: a ring growing from where it started
struct Ring {
    bool on;
    uint8_t hue;
    uint16_t x;
    uint16_t y;
    // 1/16 pixel
    uint32_t radius;
};

constexpr size_t kRings = 8;

// The start of Canvas::memory, the same for every animation; the cells (fire's heat, one byte per
// pixel) follow it.
struct Memory {
    bool started;
    uint32_t last_ms;
    // the phase of the reactive animations: their speed follows the sound
    uint32_t phase;
    // fire, twinkle, matrix, ripple: milliseconds toward the next step
    uint32_t carry;
    uint32_t random;
    // the sound, smoothed, the flash of the last beat and how much sound there is (fading in and
    // out, so silence and sound do not snap; all 0..255 x 256)
    uint32_t level;
    uint32_t pulse;
    uint32_t presence;
    // the hue the beats moved the colors on (x 256): where it glides to and where it is
    uint32_t hue_target;
    uint32_t hue;
    // the bands, smoothed with the attack and release (0..255 x 256)
    uint16_t bands[kBands];
    // color fade: the hue the beats added; meteor: the head's way in 1/256 pixel
    uint32_t offset;
    Ring rings[kRings];
};

constexpr size_t kHeader = (sizeof(Memory) + 7) & ~size_t{7};

Memory *memory_of(Canvas &canvas)
{
    if (canvas.memory == nullptr || canvas.memory_size < memory_size(canvas.width, canvas.height)) {
        return nullptr;
    }
    return reinterpret_cast<Memory *>(canvas.memory);
}

uint8_t *cells_of(Canvas &canvas)
{
    return canvas.memory + kHeader;
}

// the milliseconds since the last frame, at most 100 (0 for the first)
uint32_t elapsed(Memory &m, uint32_t ms)
{
    if (!m.started) {
        m.started = true;
        m.last_ms = ms;
        m.random = 0x9e3779b9u ^ ms;
        return 0;
    }
    const uint32_t dt = ms - m.last_ms;
    m.last_ms = ms;
    return dt > 100 ? 100 : dt;
}

uint8_t random8(Memory &m)
{
    // xorshift32
    uint32_t x = m.random != 0 ? m.random : 0x9e3779b9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    m.random = x;
    return static_cast<uint8_t>(x >> 24);
}

uint16_t random16(Memory &m)
{
    return static_cast<uint16_t>(random8(m) << 8 | random8(m));
}

// 0 .. limit - 1
uint8_t random_below(Memory &m, uint32_t limit)
{
    return limit == 0 ? 0 : static_cast<uint8_t>(random8(m) * limit >> 8);
}

// `value` (x 256) toward `target` (x 256): up within `attack` ms, down within `release` (0: at once)
template <class T>
void follow(T &value, uint32_t target, uint32_t dt, uint16_t attack, uint16_t release)
{
    const uint32_t v = value;
    if (target > v) {
        value = static_cast<T>(dt >= attack ? target : v + (target - v) * dt / attack);
    } else if (target < v) {
        value = static_cast<T>(dt >= release ? target : v - (v - target) * dt / release);
    }
}

// a value of the audio (0..255) times the strength (128: as it is), at most 255
uint8_t scaled(uint32_t value, uint8_t strength)
{
    const uint32_t v = value * strength / 128u;
    return static_cast<uint8_t>(v > 255 ? 255 : v);
}

// the brightness of a reactive animation from the sound's level: never darker than a quarter
uint8_t value_of(uint8_t level)
{
    return static_cast<uint8_t>(64 + level * 191u / 255u);
}

// What a reactive animation hears (docs/leds.md section 6.2): `on` while there is sound; `level`
// the loudness smoothed (up within the attack, 20 ms by default, down within the release, a
// quarter second) times the strength; `beat` this frame (the beat's flash, `Memory::pulse`, is
// breathe's); `drive` the smoothed loudness alone, for the speed; `value` the brightness: from the
// level with sound, full in silence, gliding between the two (in 100 ms, out in 1 s). Without a
// source an animation runs as it does without audio.
struct Sound {
    bool on;
    uint8_t level;
    bool beat;
    uint8_t drive;
    uint8_t value;
};

Sound listen(Memory &m, const Params &params, uint32_t dt)
{
    const Audio *audio = params.audio;
    const bool on = audio != nullptr && audio->active;
    // the beat's flash fades in 300 ms
    const uint32_t fade = dt * 255u * 256u / 300u;
    m.pulse = m.pulse > fade ? m.pulse - fade : 0;
    if (on && audio->beat) {
        m.pulse = 255u * 256u;
    }
    // in silence the level falls with the release, so a short pause does not start it from 0
    follow(m.level, on ? audio->loudness * 256u : 0u, dt, params.attack_ms, params.release_ms);
    follow(m.presence, on ? 255u * 256u : 0u, dt, 100, 1000);
    const uint8_t heard = scaled(m.level >> 8, params.strength);
    const uint32_t presence = m.presence >> 8;
    const auto value = static_cast<uint8_t>(255u - presence * (255u - value_of(heard)) / 255u);
    return {on, heard, on && audio->beat, static_cast<uint8_t>(m.level >> 8), value};
}

// The speed with the sound in 1/64 of the speed without it: the smoothed loudness (without the
// beat's flash, so attack and release decide how it changes, and falling with the release in
// silence) times the strength; at most 1.5 times as fast at strength 128, twice at 255
uint32_t faster(const Sound &sound, uint8_t strength)
{
    return 64u + sound.drive * strength / 1020u;
}

// The hue the beats moved the colors on: `step` per beat, glided to in 250 ms, no jump
uint8_t beat_hue(Memory &m, const Sound &sound, uint32_t dt, uint32_t step)
{
    m.hue_target += sound.beat ? step << 8 : 0u;
    const uint32_t gap = m.hue_target - m.hue;
    m.hue += dt >= 250 ? gap : gap * dt / 250u;
    return static_cast<uint8_t>(m.hue >> 8);
}

// The bands an animation hears: smoothed with the attack and release in the memory, times the
// strength. In silence they fall with the release. Without memory as they came.
void hear_bands(Memory *m, const Params &params, uint32_t dt, uint8_t (&out)[kBands])
{
    const Audio *audio = params.audio;
    for (size_t b = 0; b < kBands; b++) {
        const uint8_t value = audio != nullptr && audio->active ? audio->bands[b] : 0;
        if (m == nullptr) {
            out[b] = scaled(value, params.strength);
            continue;
        }
        follow(m->bands[b], value * 256u, dt, params.attack_ms, params.release_ms);
        out[b] = scaled(m->bands[b] >> 8, params.strength);
    }
}

// The phase of an animation that runs faster with the sound (`faster`, as rainbow and plasma): from the clock without memory or audio, else kept in the memory. `sound` is what it
// hears, off without audio; `dt` the milliseconds since the last frame (0 without memory).
uint32_t reactive_phase(Canvas &canvas, const Params &params, uint32_t ms, Sound &sound, uint32_t &dt)
{
    sound = {false, 0, false, 0, 255};
    dt = 0;
    Memory *m = params.audio != nullptr ? memory_of(canvas) : nullptr;
    if (m == nullptr) {
        return phase(ms, params.speed);
    }
    const bool first = !m->started;
    dt = elapsed(*m, ms);
    m->phase = first ? phase(ms, params.speed) : m->phase;
    sound = listen(*m, params, dt);
    m->phase += dt * per_ms(params.speed) * faster(sound, params.strength) / 64u;
    return m->phase;
}

Rgb scale(Rgb c, uint32_t value)
{
    return {static_cast<uint8_t>(c.r * value / 255), static_cast<uint8_t>(c.g * value / 255),
            static_cast<uint8_t>(c.b * value / 255)};
}

// The color of a hue at `value`: the hue circle without a palette, else the palette's color at that
// place (docs/leds.md section 6.4)
Rgb paint(const Params &params, uint8_t hue, uint8_t saturation, uint8_t value)
{
    return params.palette == 0 ? hsv(hue, saturation, value)
                               : scale(palette_color(params.palette, hue, params.color), value);
}

void clear(Canvas &canvas)
{
    std::memset(canvas.pixels, 0, static_cast<size_t>(canvas.width) * canvas.height * sizeof(Rgb));
}

// ------------------------------------------------------------------------------ animations

void solid(Canvas &canvas, const Params &params, uint32_t)
{
    const size_t n = static_cast<size_t>(canvas.width) * canvas.height;
    for (size_t i = 0; i < n; i++) {
        canvas.pixels[i] = params.color;
    }
}

// A rainbow along the diagonal of the canvas (along a strip on a canvas of one row). Reactive: it
// runs faster and brighter with the sound, a beat moves the colors on.
void rainbow(Canvas &canvas, const Params &params, uint32_t ms)
{
    const uint32_t span = static_cast<uint32_t>(canvas.width) + canvas.height;
    auto shift = static_cast<uint8_t>(phase(ms, params.speed) >> 8);
    uint8_t value = 255;
    if (Memory *m = params.audio != nullptr ? memory_of(canvas) : nullptr) {
        const bool first = !m->started;
        const uint32_t dt = elapsed(*m, ms);
        // the colors go on from where they were without the sound
        m->phase = first ? phase(ms, params.speed) : m->phase;
        const Sound sound = listen(*m, params, dt);
        m->phase += dt * per_ms(params.speed) * faster(sound, params.strength) / 64u;
        value = sound.value;
        shift = static_cast<uint8_t>((m->phase >> 8) + beat_hue(*m, sound, dt, 24u * params.strength / 128u));
    }
    for (uint16_t y = 0; y < canvas.height; y++) {
        for (uint16_t x = 0; x < canvas.width; x++) {
            const auto hue = static_cast<uint8_t>((x + y) * 256u / span + shift);
            canvas.at(x, y) = paint(params, hue, 255, value);
        }
    }
}

// The classic plasma: three moving sine waves summed, the sum as a hue. Reactive: faster and
// brighter with the sound, a beat shifts the colors.
void plasma(Canvas &canvas, const Params &params, uint32_t ms)
{
    uint32_t base = phase(ms, params.speed);
    uint8_t value = 255;
    uint8_t hue_shift = 0;
    if (Memory *m = params.audio != nullptr ? memory_of(canvas) : nullptr) {
        const bool first = !m->started;
        const uint32_t dt = elapsed(*m, ms);
        m->phase = first ? phase(ms, params.speed) : m->phase;
        const Sound sound = listen(*m, params, dt);
        m->phase += dt * per_ms(params.speed) * faster(sound, params.strength) / 64u;
        value = sound.value;
        hue_shift = beat_hue(*m, sound, dt, 32u * params.strength / 128u);
        base = m->phase;
    }
    const auto t = static_cast<uint8_t>(base >> 9);
    const auto t2 = static_cast<uint8_t>(base * 3 >> 11);
    for (uint16_t y = 0; y < canvas.height; y++) {
        for (uint16_t x = 0; x < canvas.width; x++) {
            const uint32_t v = sin8(static_cast<uint8_t>(x * 16 + t)) + sin8(static_cast<uint8_t>(y * 12 + t2)) +
                               sin8(static_cast<uint8_t>((x + y) * 8 + t + t2));
            canvas.at(x, y) = paint(params, static_cast<uint8_t>(v / 3 + t + hue_shift), 240, value);
        }
    }
}

// The color breathing in and out. Reactive: the brightness follows the sound, a beat flashes.
void breathe(Canvas &canvas, const Params &params, uint32_t ms)
{
    uint8_t level = sin8(static_cast<uint8_t>(phase(ms, params.speed) >> 9));
    if (Memory *m = params.audio != nullptr ? memory_of(canvas) : nullptr) {
        const Sound sound = listen(*m, params, elapsed(*m, ms));
        // the breathing glides into the sound's brightness and back; a beat flashes
        const uint8_t flash = scaled(m->pulse >> 8, params.strength);
        const uint8_t heard = sound.level > flash ? sound.level : flash;
        const uint32_t presence = m->presence >> 8;
        level = static_cast<uint8_t>((level * (255u - presence) + value_of(heard) * presence) / 255u);
    }
    const Rgb c{static_cast<uint8_t>(params.color.r * level / 255), static_cast<uint8_t>(params.color.g * level / 255),
                static_cast<uint8_t>(params.color.b * level / 255)};
    const size_t n = static_cast<size_t>(canvas.width) * canvas.height;
    for (size_t i = 0; i < n; i++) {
        canvas.pixels[i] = c;
    }
}

// black, red, yellow, white; with a palette its colors, dark at no heat (the palette's end is not
// blended into its start)
Rgb heat_color(uint8_t heat, const Params &params)
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

// Fire (docs/leds.md section 6.3): a heat map with a byte per pixel, the bottom row first. Each
// step cools every cell a little, lets the heat rise (a cell takes from the two below it and their
// neighbors) and lights sparks in the bottom rows; the heat is shown black, red, yellow, white.
// The speed sets the steps per second (15 to 90). Reactive: each column sparks with its band (bass
// left, treble right), the flames grow taller when it is loud, a beat sparks every column. A strip
// turned by a quarter (rotation) burns along its length.
void fire(Canvas &canvas, const Params &params, uint32_t ms)
{
    const uint16_t w = canvas.width;
    const uint16_t h = canvas.height;
    Memory *m = memory_of(canvas);
    if (m == nullptr) {
        std::memset(canvas.pixels, 0, static_cast<size_t>(w) * h * sizeof(Rgb));
        return;
    }
    uint8_t *heat = cells_of(canvas);
    const auto cell = [&](uint16_t x, uint16_t row) -> uint8_t & { return heat[static_cast<size_t>(row) * w + x]; };
    const uint32_t dt = elapsed(*m, ms);
    const Sound sound = listen(*m, params, dt);
    uint8_t bands[kBands];
    hear_bands(m, params, dt, bands);
    // the steps since the last frame, from the speed
    m->carry += dt * (15u + params.speed * 75u / 255u);
    uint32_t steps = m->carry / 1000;
    m->carry %= 1000;
    steps = steps > 4 ? 4 : steps;
    // cooling: higher is shorter flames
    const uint32_t cooling = sound.on ? 40u + (255u - sound.level) * 60u / 255u : 55u;
    const uint32_t cool_max = cooling * 10u / h + 2u;
    const uint16_t spark_rows = h < 3 ? h : 3;
    for (uint32_t step = 0; step < steps; step++) {
        for (size_t i = 0; i < static_cast<size_t>(w) * h; i++) {
            const uint8_t cool = random_below(*m, cool_max);
            heat[i] = heat[i] > cool ? static_cast<uint8_t>(heat[i] - cool) : 0;
        }
        for (uint16_t row = h - 1; row >= 2 && row < h; row--) {
            for (uint16_t x = 0; x < w; x++) {
                const uint16_t left = x > 0 ? x - 1 : x;
                const uint16_t right = x + 1 < w ? x + 1 : x;
                cell(x, row) = static_cast<uint8_t>(
                    (cell(x, row - 1) * 2u + cell(x, row - 2) * 2u + cell(left, row - 1) + cell(right, row - 1)) / 6u);
            }
        }
        for (uint16_t x = 0; x < w; x++) {
            uint32_t chance = 120;
            uint32_t strength = 160;
            if (sound.on) {
                const uint8_t band = bands[static_cast<size_t>(x) * kBands / w];
                chance = band * 3u / 4u;
                strength = 96u + band * 159u / 255u;
                if (sound.beat && step == 0) {
                    chance = 256;
                    strength = 255;
                }
            }
            if (random8(*m) < chance) {
                uint8_t &c = cell(x, random_below(*m, spark_rows));
                const uint32_t added = c + strength * (192u + random_below(*m, 64)) / 255u;
                c = static_cast<uint8_t>(added > 255 ? 255 : added);
            }
        }
    }
    for (uint16_t row = 0; row < h; row++) {
        for (uint16_t x = 0; x < w; x++) {
            canvas.at(x, static_cast<uint16_t>(h - 1 - row)) = heat_color(cell(x, row), params);
        }
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

// The spectrum (docs/leds.md section 6.3): one bar per band from the bottom, the bands spread over
// the width (a narrower canvas shows the loudest band of each group), a rainbow from the bass on
// the left to the treble on the right. The bars follow the attack, the release and the strength.
// Without audio, or once the bars fell in silence, a dim line at the bottom says the animation
// runs.
void spectrum(Canvas &canvas, const Params &params, uint32_t ms)
{
    clear(canvas);
    const uint16_t w = canvas.width;
    const uint16_t h = canvas.height;
    Memory *m = memory_of(canvas);
    uint8_t bands[kBands];
    hear_bands(m, params, m != nullptr ? elapsed(*m, ms) : 0, bands);
    // the bars fall after the sound stopped: the line only once all are down
    bool bars = false;
    for (size_t b = 0; params.audio != nullptr && b < kBands; b++) {
        bars = bars || bands[b] != 0;
    }
    for (uint16_t x = 0; x < w; x++) {
        const auto hue = static_cast<uint8_t>(x * 200u / (w > 1 ? w - 1 : 1));
        if (!bars) {
            canvas.at(x, h - 1) = paint(params, hue, 255, 40);
            continue;
        }
        // the bands of this column: one, or the loudest of a group
        const size_t first = static_cast<size_t>(x) * kBands / w;
        const size_t last = (static_cast<size_t>(x) + 1) * kBands / w;
        uint8_t value = bands[first];
        for (size_t b = first + 1; b < last && b < kBands; b++) {
            value = bands[b] > value ? bands[b] : value;
        }
        // the height in rows, rounded; the lowest row lights from a little sound on
        const uint32_t rows = (static_cast<uint32_t>(value) * h + 127) / 255;
        const Rgb color = paint(params, hue, 255, 255);
        for (uint32_t r = 0; r < rows && r < h; r++) {
            canvas.at(x, static_cast<uint16_t>(h - 1 - r)) = color;
        }
    }
}

// A gradient from the color to the color a third of the hue circle on (its channels turned), there
// and back along the diagonal, moving with the speed. Reactive: faster and brighter with the sound.
void gradient(Canvas &canvas, const Params &params, uint32_t ms)
{
    Sound sound;
    uint32_t dt = 0;
    const auto shift = static_cast<uint8_t>(reactive_phase(canvas, params, ms, sound, dt) >> 8);
    const Rgb a = params.color;
    const Rgb b{a.b, a.r, a.g};
    const uint32_t value = sound.value;
    const uint32_t span = static_cast<uint32_t>(canvas.width) + canvas.height;
    for (uint16_t y = 0; y < canvas.height; y++) {
        for (uint16_t x = 0; x < canvas.width; x++) {
            const auto t = static_cast<uint8_t>((x + y) * 256u / span + shift);
            // there and back: no seam where the hue would jump
            const uint32_t f = t < 128 ? t * 2u : (255u - t) * 2u;
            const Rgb mixed{static_cast<uint8_t>((a.r * (255u - f) + b.r * f) / 255u),
                            static_cast<uint8_t>((a.g * (255u - f) + b.g * f) / 255u),
                            static_cast<uint8_t>((a.b * (255u - f) + b.b * f) / 255u)};
            canvas.at(x, y) = scale(mixed, value);
        }
    }
}

// All LEDs in one color going slowly around the hue circle (at speed 128 once in about 8 s).
// Reactive: brighter with the sound, every beat jumps on to the next color.
void color_fade(Canvas &canvas, const Params &params, uint32_t ms)
{
    Sound sound;
    uint32_t dt = 0;
    uint32_t hue = reactive_phase(canvas, params, ms, sound, dt) >> 12;
    if (Memory *m = params.audio != nullptr ? memory_of(canvas) : nullptr) {
        hue += beat_hue(*m, sound, dt, 64u);
    }
    const Rgb c = paint(params, static_cast<uint8_t>(hue), 255, sound.value);
    const size_t n = static_cast<size_t>(canvas.width) * canvas.height;
    for (size_t i = 0; i < n; i++) {
        canvas.pixels[i] = c;
    }
}

// the milliseconds of one twinkle (rise and fall) or one ripple: 3 s at speed 0, 0.4 s at 255
uint32_t duration_ms(uint8_t speed)
{
    return 3000u - speed * 2600u / 255u;
}

// Stars in the color that light up and fade out at random places, about one LED in seven lit. A
// byte per LED: 0 dark, 1..127 rising, 128..255 falling. Reactive: more stars with the sound,
// a burst on every beat.
void twinkle(Canvas &canvas, const Params &params, uint32_t ms)
{
    Memory *m = memory_of(canvas);
    if (m == nullptr) {
        clear(canvas);
        return;
    }
    uint8_t *cells = cells_of(canvas);
    const uint32_t dt = elapsed(*m, ms);
    const Sound sound = listen(*m, params, dt);
    const uint32_t duration = duration_ms(params.speed);
    m->carry += dt * 256u;
    const uint32_t step = m->carry / duration;
    m->carry %= duration;
    // a new star per LED and frame: 15 % lit over a twinkle's time, more with the sound
    uint32_t chance = 9830u * dt / duration;
    if (sound.on) {
        chance = chance * (64u + sound.level * 3u) / 64u;
        chance = sound.beat ? chance * 8u + 2000u : chance;
    }
    const size_t n = static_cast<size_t>(canvas.width) * canvas.height;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = cells[i];
        if (c != 0) {
            c += step;
            c = c > 255 ? 0 : c;
        } else if (random16(*m) < chance) {
            c = 1;
        }
        cells[i] = static_cast<uint8_t>(c);
        const uint32_t value = c < 128 ? c * 2u : (255u - c) * 2u;
        canvas.pixels[i] = scale(params.color, value > 255 ? 255 : value);
    }
}

// A meteor in the color along every row (left to right, each row at its own place), its tail
// fading out unevenly. The speed sets 4 to 64 pixels a second. Reactive: faster and brighter
// with the sound.
void meteor(Canvas &canvas, const Params &params, uint32_t ms)
{
    Memory *m = memory_of(canvas);
    if (m == nullptr) {
        clear(canvas);
        return;
    }
    const uint16_t w = canvas.width;
    const uint16_t h = canvas.height;
    uint8_t *cells = cells_of(canvas);
    const uint32_t dt = elapsed(*m, ms);
    const Sound sound = listen(*m, params, dt);
    const uint32_t pixels_per_s = 4u + params.speed * 60u / 255u;
    // the head runs off the end and on for a while, so the tail can fade
    const uint32_t length = (w + w / 2u + 4u) * 256u;
    m->offset += dt * pixels_per_s * 256u / 1000u * faster(sound, params.strength) / 64u;
    m->offset %= length;
    const uint32_t value = sound.value;
    for (uint16_t y = 0; y < h; y++) {
        uint8_t *row = cells + static_cast<size_t>(y) * w;
        // the tail: each pixel fades by about a third of its light every 100 ms, some skip a frame
        for (uint16_t x = 0; x < w; x++) {
            if (random8(*m) < 192) {
                const uint32_t fade = row[x] * dt / 300u + 1u;
                row[x] = row[x] > fade ? static_cast<uint8_t>(row[x] - fade) : 0;
            }
        }
        const uint32_t head = (m->offset + static_cast<uint32_t>(y) * 0x9e37u) % length >> 8;
        if (head < w) {
            row[head] = 255;
        }
        for (uint16_t x = 0; x < w; x++) {
            canvas.at(x, y) = scale(params.color, row[x] * value / 255u);
        }
    }
}

// Rings in the colors of the rainbow growing from random places and fading as they grow, a new
// one every 0.4 to 3 s (the speed). Reactive: a ring on every beat instead of the clock. On a
// strip two dots run apart.
void ripple(Canvas &canvas, const Params &params, uint32_t ms)
{
    clear(canvas);
    Memory *m = memory_of(canvas);
    if (m == nullptr) {
        return;
    }
    const uint16_t w = canvas.width;
    const uint16_t h = canvas.height;
    const bool first = !m->started;
    const uint32_t dt = elapsed(*m, ms);
    const Sound sound = listen(*m, params, dt);
    const uint32_t interval = duration_ms(params.speed);
    m->carry += dt;
    bool start = first || (sound.on ? sound.beat : m->carry >= interval);
    m->carry = start || m->carry >= interval ? 0 : m->carry;
    // 4 to 34 pixels a second; it dies at the far end of the canvas
    const uint32_t growth = (4u + params.speed * 30u / 255u) * 16u;
    const uint32_t largest = (static_cast<uint32_t>(w > h ? w : h) + 2u) * 16u;
    for (Ring &r : m->rings) {
        if (r.on) {
            r.radius += growth * dt / 1000u;
            r.on = r.radius < largest;
        } else if (start) {
            r = {true, random8(*m), static_cast<uint16_t>(random_below(*m, w > 255 ? 255 : w)),
                 static_cast<uint16_t>(random_below(*m, h > 255 ? 255 : h)), 0};
            start = false;
        }
    }
    const uint32_t loud = sound.value;
    for (const Ring &r : m->rings) {
        if (!r.on) {
            continue;
        }
        // |d - r| from the squares (|d² - r²| / 2r), all in 1/16 pixel; 1.5 pixels thick
        const uint64_t radius = r.radius < 8 ? 8 : r.radius;
        const uint64_t r2 = static_cast<uint64_t>(r.radius) * r.radius;
        const uint32_t fade = 255u - r.radius * 255u / largest;
        const Rgb color = paint(params, r.hue, 255, static_cast<uint8_t>(fade * loud / 255u));
        for (uint16_t y = 0; y < h; y++) {
            const int64_t dy = (static_cast<int64_t>(y) - r.y) * 16;
            for (uint16_t x = 0; x < w; x++) {
                const int64_t dx = (static_cast<int64_t>(x) - r.x) * 16;
                const auto d2 = static_cast<uint64_t>(dx * dx + dy * dy);
                const uint64_t off = (d2 > r2 ? d2 - r2 : r2 - d2) / (2u * radius);
                if (off >= 24) {
                    continue;
                }
                const auto v = static_cast<uint32_t>((24u - off) * 255u / 24u);
                Rgb &p = canvas.at(x, y);
                const Rgb add = scale(color, v);
                p = {static_cast<uint8_t>(p.r + add.r > 255 ? 255 : p.r + add.r),
                     static_cast<uint8_t>(p.g + add.g > 255 ? 255 : p.g + add.g),
                     static_cast<uint8_t>(p.b + add.b > 255 ? 255 : p.b + add.b)};
            }
        }
    }
}

// Matrix rain: drops in the color fall down the columns, a light head and a fading tail. A byte
// per LED: 255 a head, below it the tail's light. The speed sets 8 to 48 steps a second.
// Reactive: each column drops with its band (bass left), every column on a beat.
void matrix(Canvas &canvas, const Params &params, uint32_t ms)
{
    Memory *m = memory_of(canvas);
    if (m == nullptr) {
        clear(canvas);
        return;
    }
    const uint16_t w = canvas.width;
    const uint16_t h = canvas.height;
    uint8_t *cells = cells_of(canvas);
    const auto cell = [&](uint16_t x, uint16_t y) -> uint8_t & { return cells[static_cast<size_t>(y) * w + x]; };
    const uint32_t dt = elapsed(*m, ms);
    const Sound sound = listen(*m, params, dt);
    uint8_t bands[kBands];
    hear_bands(m, params, dt, bands);
    m->carry += dt * (8u + params.speed * 40u / 255u);
    uint32_t steps = m->carry / 1000;
    m->carry %= 1000;
    steps = steps > 4 ? 4 : steps;
    // the tail is about half the height
    const uint32_t fade = 230u / (h / 2u + 1u) + 1u;
    for (uint32_t step = 0; step < steps; step++) {
        for (size_t i = 0; i < static_cast<size_t>(w) * h; i++) {
            if (cells[i] != 255) {
                cells[i] = cells[i] > fade ? static_cast<uint8_t>(cells[i] - fade) : 0;
            }
        }
        // bottom up, so a head moves once
        for (uint16_t y = h; y-- > 0;) {
            for (uint16_t x = 0; x < w; x++) {
                if (cell(x, y) == 255) {
                    cell(x, y) = 230;
                    if (y + 1 < h) {
                        cell(x, static_cast<uint16_t>(y + 1)) = 255;
                    }
                }
            }
        }
        for (uint16_t x = 0; x < w; x++) {
            uint32_t chance = 16;
            if (sound.on) {
                chance = bands[static_cast<size_t>(x) * kBands / w] / 4u;
                chance = sound.beat && step == 0 ? 256 : chance;
            }
            // a new drop only where the last one has moved on
            if (random8(*m) < chance && cell(x, 0) < 128 && (h < 2 || cell(x, 1) != 255)) {
                cell(x, 0) = 255;
            }
        }
    }
    const Rgb head{static_cast<uint8_t>((params.color.r + 255u) / 2u), static_cast<uint8_t>((params.color.g + 255u) / 2u),
                   static_cast<uint8_t>((params.color.b + 255u) / 2u)};
    for (size_t i = 0; i < static_cast<size_t>(w) * h; i++) {
        canvas.pixels[i] = cells[i] == 255 ? head : scale(params.color, cells[i]);
    }
}

// Noise: smooth value noise moving slowly through the colors, like lava. Reactive: faster and
// brighter with the sound.
void noise(Canvas &canvas, const Params &params, uint32_t ms)
{
    Sound sound;
    uint32_t dt = 0;
    const uint32_t t = reactive_phase(canvas, params, ms, sound, dt);
    const uint8_t value = sound.value;
    // a lattice cell of 8 pixels (1/256 each); at speed 128 the noise drifts up about 4 pixels and
    // changes half a cell in depth a second, the colors go around in 16 s
    const uint32_t z = t >> 10;
    const auto hue = static_cast<uint8_t>(t >> 13);
    for (uint16_t y = 0; y < canvas.height; y++) {
        for (uint16_t x = 0; x < canvas.width; x++) {
            const uint8_t n = noise8(x * 32u, y * 32u + z, z);
            canvas.at(x, y) = paint(params, static_cast<uint8_t>(n + hue), 255, value);
        }
    }
}

using RenderFunction = void (*)(Canvas &, const Params &, uint32_t);

struct Entry {
    Info info;
    RenderFunction render;
};

constexpr Entry kEffects[] = {
    {{"solid", "Solid", Reactive::Never, true, false}, solid},
    {{"rainbow", "Rainbow", Reactive::Optional, false, true}, rainbow},
    {{"plasma", "Plasma", Reactive::Optional, false, true}, plasma},
    {{"breathe", "Breathe", Reactive::Optional, true, false}, breathe},
    {{"spectrum", "Spectrum", Reactive::Required, false, true}, spectrum},
    {{"fire", "Fire", Reactive::Optional, false, true}, fire},
    {{"gradient", "Gradient", Reactive::Optional, true, false}, gradient},
    {{"color-fade", "Color fade", Reactive::Optional, false, true}, color_fade},
    {{"twinkle", "Twinkle", Reactive::Optional, true, false}, twinkle},
    {{"meteor", "Meteor", Reactive::Optional, true, false}, meteor},
    {{"ripple", "Ripple", Reactive::Optional, false, true}, ripple},
    {{"matrix", "Matrix rain", Reactive::Optional, true, false}, matrix},
    {{"noise", "Noise", Reactive::Optional, false, true}, noise},
    // test patterns last; the index of an animation is stored (the state, the styles): a new one
    // goes before them
    {{"edges", "Test: edges", Reactive::Never, false, false}, edges},
};

// ------------------------------------------------------------------------------- palettes

// 16 colors each (0xrrggbb), once around; the colors between them are blended. Party, ocean,
// forest, lava, heat and cloud have the colors of FastLED's palettes of the same names.
struct Palette {
    PaletteInfo info;
    uint32_t colors[16];
};

// the index of a palette is stored (the styles, the presets): a new one goes at the end
constexpr Palette kPalettes[] = {
    // none: the animation's own colors (the table is the rainbow, for palette_color)
    {{"none", "Its own colors"},
     {0xff0000, 0xff6000, 0xffc000, 0xe1ff00, 0x81ff00, 0x21ff00, 0x00ff3c, 0x00ff9c, 0x00fffc, 0x00a5ff, 0x0045ff,
      0x1800ff, 0x7800ff, 0xd800ff, 0xff00c9, 0xff0069}},
    {{"rainbow", "Rainbow"},
     {0xff0000, 0xff6000, 0xffc000, 0xe1ff00, 0x81ff00, 0x21ff00, 0x00ff3c, 0x00ff9c, 0x00fffc, 0x00a5ff, 0x0045ff,
      0x1800ff, 0x7800ff, 0xd800ff, 0xff00c9, 0xff0069}},
    {{"party", "Party"},
     {0x5500ab, 0x84007c, 0xb5004b, 0xe5001b, 0xe81700, 0xb84700, 0xab7700, 0xabab00, 0xab5500, 0xdd2200, 0xf2000e,
      0xc2003e, 0x8f0071, 0x5f00a1, 0x2f00d0, 0x0007f9}},
    {{"ocean", "Ocean"},
     {0x191970, 0x00008b, 0x191970, 0x000080, 0x00008b, 0x0000cd, 0x2e8b57, 0x008080, 0x5f9ea0, 0x0000ff, 0x008b8b,
      0x6495ed, 0x7fffd4, 0x2e8b57, 0x00ffff, 0x87cefa}},
    {{"forest", "Forest"},
     {0x006400, 0x006400, 0x556b2f, 0x006400, 0x008000, 0x228b22, 0x6b8e23, 0x008000, 0x2e8b57, 0x66cdaa, 0x32cd32,
      0x9acd32, 0x90ee90, 0x7cfc00, 0x66cdaa, 0x228b22}},
    {{"lava", "Lava"},
     {0x000000, 0x800000, 0x000000, 0x800000, 0x8b0000, 0x8b0000, 0x800000, 0x8b0000, 0x8b0000, 0x8b0000, 0xff0000,
      0xffa500, 0xffffff, 0xffa500, 0xff0000, 0x8b0000}},
    {{"heat", "Heat"},
     {0x000000, 0x330000, 0x660000, 0x990000, 0xcc0000, 0xff0000, 0xff3300, 0xff6600, 0xff9900, 0xffcc00, 0xffff00,
      0xffff33, 0xffff66, 0xffff99, 0xffffcc, 0xffffff}},
    {{"cloud", "Cloud"},
     {0x0000ff, 0x00008b, 0x00008b, 0x00008b, 0x00008b, 0x00008b, 0x00008b, 0x00008b, 0x0000ff, 0x00008b, 0x87ceeb,
      0x87ceeb, 0xadd8e6, 0xffffff, 0xadd8e6, 0x87ceeb}},
    {{"sunset", "Sunset"},
     {0x780000, 0xb31600, 0xe03c00, 0xff6800, 0xffa020, 0xffc860, 0xff9060, 0xe05080, 0xa7169a, 0x6a0c8c, 0x2a0a6a,
      0x100a40, 0x2a0a6a, 0x6a0c8c, 0xb31600, 0x900400}},
    {{"pastel", "Pastel"},
     {0xff9191, 0xffbb91, 0xffe491, 0xf3ff91, 0xc9ff91, 0xa0ff91, 0x91ffab, 0x91ffd5, 0x91fffe, 0x91d9ff, 0x91afff,
      0x9c91ff, 0xc591ff, 0xef91ff, 0xff91e8, 0xff91bf}},
    // made from Params::color, see palette_color
    {{"color", "Shades of the color"}, {}},
};

constexpr size_t kColorPalette = std::size(kPalettes) - 1;

Rgb rgb_of(uint32_t c)
{
    return {static_cast<uint8_t>(c >> 16), static_cast<uint8_t>(c >> 8), static_cast<uint8_t>(c)};
}

} // namespace

size_t count()
{
    return std::size(kEffects);
}

size_t animation_count()
{
    return find("edges");
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

size_t palette_count()
{
    return std::size(kPalettes);
}

const PaletteInfo *palette_info(size_t index)
{
    return index < palette_count() ? &kPalettes[index].info : nullptr;
}

size_t find_palette(const char *slug)
{
    for (size_t i = 0; i < palette_count(); i++) {
        if (std::strcmp(kPalettes[i].info.slug, slug) == 0) {
            return i;
        }
    }
    return palette_count();
}

Rgb palette_color(uint8_t palette, uint8_t index, Rgb color)
{
    if (palette == kColorPalette) {
        // the color, darker toward the middle and back: a quarter of it at the darkest
        const uint32_t dip = index < 128 ? index * 2u : (255u - index) * 2u;
        return scale(color, 255u - dip * 191u / 255u);
    }
    const uint32_t *colors = kPalettes[palette < palette_count() ? palette : 0].colors;
    const Rgb a = rgb_of(colors[index >> 4]);
    const Rgb b = rgb_of(colors[((index >> 4) + 1) & 15]);
    const uint32_t f = (index & 15u) * 16u;
    return {static_cast<uint8_t>((a.r * (256u - f) + b.r * f) >> 8),
            static_cast<uint8_t>((a.g * (256u - f) + b.g * f) >> 8),
            static_cast<uint8_t>((a.b * (256u - f) + b.b * f) >> 8)};
}

size_t memory_size(uint16_t width, uint16_t height)
{
    return kHeader + static_cast<size_t>(width) * height;
}

void render(size_t index, Canvas &canvas, const Params &params, uint32_t ms)
{
    if (canvas.width == 0 || canvas.height == 0) {
        return;
    }
    const Entry &entry = kEffects[index < count() ? index : 0];
    // an animation that does not follow the audio does not get it
    Params p = params;
    const bool reactive = entry.info.reactive == Reactive::Required ||
                          (entry.info.reactive == Reactive::Optional && params.reactive && params.strength > 0);
    p.audio = reactive ? params.audio : nullptr;
    const uint8_t rotation = params.rotation & 3;
    const uint16_t w = canvas.width;
    const uint16_t h = canvas.height;
    if ((rotation == 1 || rotation == 3) && canvas.spare != nullptr) {
        // drawn on the spare canvas, h x w, then turned onto the canvas
        Canvas turned{h, w, canvas.spare, canvas.memory, canvas.memory_size, nullptr};
        entry.render(turned, p, ms);
        for (uint16_t y = 0; y < h; y++) {
            for (uint16_t x = 0; x < w; x++) {
                // clockwise: the turned canvas' top left lands top right; the other way bottom left
                canvas.at(x, y) = rotation == 1 ? turned.at(y, static_cast<uint16_t>(w - 1 - x))
                                                : turned.at(static_cast<uint16_t>(h - 1 - y), x);
            }
        }
        return;
    }
    entry.render(canvas, p, ms);
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

void blend(Rgb *into, const Rgb *from, size_t count, uint8_t amount)
{
    const uint32_t keep = 255u - amount;
    for (size_t i = 0; i < count; i++) {
        into[i] = {static_cast<uint8_t>((into[i].r * keep + from[i].r * amount) / 255u),
                   static_cast<uint8_t>((into[i].g * keep + from[i].g * amount) / 255u),
                   static_cast<uint8_t>((into[i].b * keep + from[i].b * amount) / 255u)};
    }
}

namespace {

// a random byte for every point of the lattice
uint8_t lattice(uint32_t x, uint32_t y, uint32_t z)
{
    uint32_t h = x * 0x8da6b343u ^ y * 0xd8163841u ^ z * 0xcb1ab31fu;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return static_cast<uint8_t>(h >> 24);
}

// 0..255 eased in and out (smoothstep), so the cells do not show
uint32_t ease(uint32_t t)
{
    return t * t * (768u - 2u * t) >> 16;
}

uint32_t mix(uint32_t a, uint32_t b, uint32_t t)
{
    return (a * (256u - t) + b * t) >> 8;
}

} // namespace

uint8_t noise8(uint32_t x, uint32_t y, uint32_t z)
{
    const uint32_t xi = x >> 8, yi = y >> 8, zi = z >> 8;
    const uint32_t fx = ease(x & 255), fy = ease(y & 255), fz = ease(z & 255);
    uint32_t slice[2];
    for (uint32_t k = 0; k < 2; k++) {
        const uint32_t top = mix(lattice(xi, yi, zi + k), lattice(xi + 1, yi, zi + k), fx);
        const uint32_t bottom = mix(lattice(xi, yi + 1, zi + k), lattice(xi + 1, yi + 1, zi + k), fx);
        slice[k] = mix(top, bottom, fy);
    }
    return static_cast<uint8_t>(mix(slice[0], slice[1], fz));
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
