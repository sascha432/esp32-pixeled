#pragma once

// Inside the animations (effects.cpp, particles.cpp): the memory between frames, what a reactive
// animation hears, random numbers and colors. Not part of the component's interface.

#include <cstddef>
#include <cstdint>

#include "px/effects/effects.hpp"

namespace px::effects::detail {

// speed 0..255 as a phase step per millisecond in 1/65536 of a full turn
uint32_t per_ms(uint8_t speed);
uint32_t phase(uint32_t ms, uint8_t speed, uint32_t scale = 1);

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
// pixel) or the trail of particles.cpp follow it.
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

// nullptr without memory (or too little)
Memory *memory_of(Canvas &canvas);
uint8_t *cells_of(Canvas &canvas);
// the milliseconds since the last frame, at most 100 (0 for the first)
uint32_t elapsed(Memory &m, uint32_t ms);

uint8_t random8(Memory &m);
uint16_t random16(Memory &m);
// 0 .. limit - 1
uint8_t random_below(Memory &m, uint32_t limit);

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
uint8_t scaled(uint32_t value, uint8_t strength);
// the brightness of a reactive animation from the sound's level: never darker than a quarter
uint8_t value_of(uint8_t level);
// what `level` (0..255, heard) makes of a brightness or a size: value_of(level) while there is
// sound, full in silence, gliding between the two with Memory::presence
uint8_t with_sound(const Memory &m, uint8_t level);

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

Sound listen(Memory &m, const Params &params, uint32_t dt);
// The speed with the sound in 1/64 of the speed without it: at most 1.5 times as fast at strength
// 128, twice at 255
uint32_t faster(const Sound &sound, uint8_t strength);
// The hue the beats moved the colors on: `step` per beat, glided to in 250 ms, no jump
uint8_t beat_hue(Memory &m, const Sound &sound, uint32_t dt, uint32_t step);
// The bands an animation hears: smoothed with the attack and release in the memory, times the
// strength. In silence they fall with the release. Without memory as they came.
void hear_bands(Memory *m, const Params &params, uint32_t dt, uint8_t (&out)[kBands]);
// The phase of an animation that runs faster with the sound (`faster`, as rainbow and plasma)
uint32_t reactive_phase(Canvas &canvas, const Params &params, uint32_t ms, Sound &sound, uint32_t &dt);

Rgb scale(Rgb c, uint32_t value);
// The color of a hue at `value`: the hue circle without a palette, else the palette's color at that
// place (docs/leds.md section 6.4)
Rgb paint(const Params &params, uint8_t hue, uint8_t saturation, uint8_t value);
void clear(Canvas &canvas);

// sin and cos of `angle` (1/65536 of a turn) as -32767..32767
int32_t sin16(uint32_t angle);
int32_t cos16(uint32_t angle);

// particles.cpp: the memory its animations need after the header, and the animations
size_t trail_memory_size(uint16_t width, uint16_t height);
void colored_bursts(Canvas &canvas, const Params &params, uint32_t ms);
void blobs(Canvas &canvas, const Params &params, uint32_t ms);
void waverly(Canvas &canvas, const Params &params, uint32_t ms);
void ps_vortex(Canvas &canvas, const Params &params, uint32_t ms);
void ps_fuzzy_noise(Canvas &canvas, const Params &params, uint32_t ms);
void ps_waterfall(Canvas &canvas, const Params &params, uint32_t ms);
void ps_geq_2d(Canvas &canvas, const Params &params, uint32_t ms);
void ps_geq_nova(Canvas &canvas, const Params &params, uint32_t ms);
void ps_ghost_rider(Canvas &canvas, const Params &params, uint32_t ms);
void ps_blobs(Canvas &canvas, const Params &params, uint32_t ms);
void ps_galaxy(Canvas &canvas, const Params &params, uint32_t ms);

} // namespace px::effects::detail
