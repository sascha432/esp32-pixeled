#pragma once

// The stage of the animations that draw on a trail (particles.cpp, curves.cpp): the particles, the
// scene, a frame's start (the memory, the sound, the steps of the animation's own clock) and its
// end (the trail onto the canvas), and the small helpers they share. In an anonymous namespace like
// trail.hpp: each file that includes it has its own copy of what it uses.

#include <cstring>

#include "internal.hpp"
#include "trail.hpp"

namespace px::effects::detail {

namespace {

constexpr uint32_t kStepMs = 16;
// at most this many steps in a frame (100 ms at speed 128; a slower frame rate is slower)
constexpr uint32_t kMaxSteps = 16;
// at speed 0 the steps of the first frame
constexpr uint32_t kStillSteps = 60;

// a point that moves: its place in 1/256 pixel (0 is the middle of the first pixel), its velocity
// in 1/256 pixel per step
struct Particle {
    int32_t x;
    int32_t y;
    int16_t vx;
    int16_t vy;
    // steps left to live; 0: a free slot
    uint16_t life;
    uint8_t hue;
    // what an animation keeps per particle (a band, a brightness)
    uint8_t tag;
};

static_assert(sizeof(Particle) == 16, "a particle is 16 bytes");

// an animation's state beyond its particles
struct Scene {
    // the clock: milliseconds x 64 toward the next step, and the steps made
    uint32_t clock;
    uint32_t steps;
    // where the search for a free particle goes on
    uint16_t next;
    // a turn in 1/65536 (vortex, nova, galaxy; the riders' heading)
    uint32_t angle;
    // the vortex' way round (+1 or -1) and the steps until it turns; the births to come of the
    // galaxy, the waterfall and the fireworks (x 65536); the steps until the next rocket
    int32_t direction;
    uint32_t timer;
    // the riders, 1/256 pixel; the scrolling text's way (x)
    int32_t x;
    int32_t y;
};

[[maybe_unused]] size_t align8(size_t n)
{
    return (n + 7) & ~size_t{7};
}

// about one particle per three pixels, 48 to 384
[[maybe_unused]] size_t particle_slots(uint16_t width, uint16_t height)
{
    const size_t third = static_cast<size_t>(width) * height / 3;
    return third < 48 ? 48 : (third > 384 ? 384 : third);
}

// a frame of one of these animations: its world, what it hears and the steps its clock made
struct Frame {
    World world;
    Sound sound;
    uint32_t dt;
    uint32_t steps;
    uint8_t bands[kBands];
};

// The start of a frame; false without memory (the canvas is dark then).
[[maybe_unused]] bool begin(Canvas &canvas, const Params &params, uint32_t ms, Frame &f)
{
    Memory *m = memory_of(canvas);
    if (m == nullptr) {
        clear(canvas);
        return false;
    }
    const uint16_t w = canvas.width;
    const uint16_t h = canvas.height;
    uint8_t *at = canvas.memory + kHeader;
    f.world.m = m;
    f.world.trail = reinterpret_cast<Rgb *>(at);
    at += align8(static_cast<size_t>(w) * h * sizeof(Rgb));
    f.world.scene = reinterpret_cast<Scene *>(at);
    at += align8(sizeof(Scene));
    f.world.particles = reinterpret_cast<Particle *>(at);
    f.world.slots = particle_slots(w, h);
    f.world.w = w;
    f.world.h = h;
    f.dt = elapsed(*m, ms);
    f.sound = listen(*m, params, f.dt);
    hear_bands(m, params, f.dt, f.bands);
    Scene &s = *f.world.scene;
    // 64 a millisecond at speed 128 without sound
    const uint32_t rate = params.speed == 0 ? 0u : 8u + params.speed * 7u / 16u;
    s.clock += f.dt * rate * faster(f.sound, params.strength) / 64u;
    f.steps = s.clock / (kStepMs * 64u);
    s.clock %= kStepMs * 64u;
    f.steps = f.steps > kMaxSteps ? kMaxSteps : f.steps;
    if (params.speed == 0 && s.steps == 0) {
        f.steps = kStillSteps;
    }
    return true;
}

// the trail onto the canvas at `value`
[[maybe_unused]] void show(Canvas &canvas, const World &world, uint8_t value)
{
    const size_t n = static_cast<size_t>(world.w) * world.h;
    if (value == 255) {
        std::memcpy(canvas.pixels, world.trail, n * sizeof(Rgb));
        return;
    }
    for (size_t i = 0; i < n; i++) {
        canvas.pixels[i] = scale(world.trail[i], value);
    }
}


// a free particle; nullptr when all live
[[maybe_unused]] Particle *spawn(World &world)
{
    Scene &s = *world.scene;
    for (size_t k = 0; k < world.slots; k++) {
        const size_t i = (s.next + k) % world.slots;
        if (world.particles[i].life == 0) {
            s.next = static_cast<uint16_t>((i + 1) % world.slots);
            return &world.particles[i];
        }
    }
    return nullptr;
}

[[maybe_unused]] int16_t i16(int32_t v)
{
    return static_cast<int16_t>(v < -32767 ? -32767 : (v > 32767 ? 32767 : v));
}

// keeps a coordinate within 0..limit: mirrored at an edge, its velocity turned inward with keep /
// 256 of it
[[maybe_unused]] void bounce(int32_t &p, int16_t &v, int32_t limit, int32_t keep = 256)
{
    const int32_t speed = (v < 0 ? -v : v) * keep / 256;
    if (p < 0) {
        p = -p;
        v = i16(speed);
    } else if (p > limit) {
        p = 2 * limit - p;
        v = i16(-speed);
    }
    p = p < 0 ? 0 : (p > limit ? limit : p);
}

// -range..range
[[maybe_unused]] int32_t random_signed(Memory &m, int32_t range)
{
    return static_cast<int32_t>((static_cast<int64_t>(random16(m)) * (2 * range + 1)) >> 16) - range;
}

// 0..limit
[[maybe_unused]] int32_t random_upto(Memory &m, int32_t limit)
{
    return static_cast<int32_t>((static_cast<int64_t>(random16(m)) * (limit + 1)) >> 16);
}

[[maybe_unused]] uint32_t isqrt(uint32_t n)
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

// a sine (-32767..32767) as a place along a side of `pixels`, in 1/256 pixel
[[maybe_unused]] int32_t across(uint16_t pixels, int32_t sine)
{
    return static_cast<int32_t>((static_cast<int64_t>(sine) + 32767) * (pixels - 1) * 256 / 65534);
}

// more than a pixel off the canvas
[[maybe_unused]] bool off_canvas(const World &world, const Particle &p)
{
    return p.x < -256 || p.y < -256 || p.x > world.w * 256 || p.y > world.h * 256;
}

// the loudest of the bands first..last - 1 (at least `first`)
[[maybe_unused]] uint8_t loudest(const uint8_t (&bands)[kBands], size_t first, size_t last)
{
    uint8_t value = bands[first < kBands ? first : kBands - 1];
    for (size_t b = first + 1; b < last && b < kBands; b++) {
        value = bands[b] > value ? bands[b] : value;
    }
    return value;
}

[[maybe_unused]] size_t live_particles(const World &world)
{
    size_t live = 0;
    for (size_t i = 0; i < world.slots; i++) {
        live += world.particles[i].life != 0 ? 1 : 0;
    }
    return live;
}

[[maybe_unused]] uint16_t longer_side(const World &world)
{
    return world.w > world.h ? world.w : world.h;
}

[[maybe_unused]] uint16_t shorter_side(const World &world)
{
    return world.w < world.h ? world.w : world.h;
}

} // namespace

} // namespace px::effects::detail
