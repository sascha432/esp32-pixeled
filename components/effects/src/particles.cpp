// The animations after WLED's 2D ones that draw on a trail of their own (docs/leds.md section
// 6.3): colored bursts, blobs, waverly and the particle ones (WLED's "PS"). Written from WLED's
// descriptions and how they look, not from its code.
//
// The trail is an RGB canvas in the canvas' memory after the header; the scene (the state beyond
// the particles) and the particles follow it. Each step the trail fades and blurs a little, the
// world moves on and the lines, blobs or particles are drawn on it, keeping the lighter of the two
// (a slow or resting one keeps its color instead of adding up to white). Every frame the trail is
// copied onto the canvas at the sound's brightness. A step is 16 ms of the animation's own clock:
// the speed turns it (0 stands still, 128 as built, 255 almost twice as fast) and the sound makes
// it faster (faster()), so an animation looks the same at any frame rate. At speed 0 the first
// frame makes a second of steps, a picture to stand still. Integer math only, like effects.cpp.

#include <cstring>

#include "internal.hpp"

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
    // a turn in 1/65536 (vortex, nova, galaxy; the rider's heading)
    uint32_t angle;
    // the vortex' way round (+1 or -1) and the steps until it turns; the galaxy's births to come
    // (x 65536)
    int32_t direction;
    uint32_t timer;
    // the rider, 1/256 pixel
    int32_t x;
    int32_t y;
};

size_t align8(size_t n)
{
    return (n + 7) & ~size_t{7};
}

// about one particle per three pixels, 48 to 384
size_t particle_slots(uint16_t width, uint16_t height)
{
    const size_t third = static_cast<size_t>(width) * height / 3;
    return third < 48 ? 48 : (third > 384 ? 384 : third);
}

struct World {
    Memory *m;
    Rgb *trail;
    Scene *scene;
    Particle *particles;
    size_t slots;
    uint16_t w;
    uint16_t h;
};

// a frame of one of these animations: its world, what it hears and the steps its clock made
struct Frame {
    World world;
    Sound sound;
    uint32_t dt;
    uint32_t steps;
    uint8_t bands[kBands];
};

// The start of a frame; false without memory (the canvas is dark then).
bool begin(Canvas &canvas, const Params &params, uint32_t ms, Frame &f)
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
void show(Canvas &canvas, const World &world, uint8_t value)
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

// every channel of the trail keeps keep / 256 of its light
void fade(World &world, uint32_t keep)
{
    auto *p = reinterpret_cast<uint8_t *>(world.trail);
    const size_t n = static_cast<size_t>(world.w) * world.h * sizeof(Rgb);
    for (size_t i = 0; i < n; i++) {
        p[i] = static_cast<uint8_t>(p[i] * keep >> 8);
    }
}

uint8_t blurred(uint8_t before, uint8_t here, uint8_t after, uint32_t amount)
{
    const uint32_t mean = (before + 2u * here + after + 2u) / 4u;
    return static_cast<uint8_t>((here * (256u - amount) + mean * amount) >> 8);
}

// n pixels `stride` apart: each takes amount / 256 of the mean of itself and its neighbors (1 2 1);
// at the ends a pixel is its own neighbor
void blur_line(Rgb *p, size_t n, size_t stride, uint32_t amount)
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
void blur(World &world, uint32_t amount)
{
    for (uint16_t y = 0; y < world.h; y++) {
        blur_line(world.trail + static_cast<size_t>(y) * world.w, world.w, 1, amount);
    }
    for (uint16_t x = 0; x < world.w; x++) {
        blur_line(world.trail + x, world.h, world.w, amount);
    }
}

// the lighter of the trail and `color` times `weight` (0..256), each channel; off the canvas nothing
void plot(World &world, int32_t x, int32_t y, Rgb color, uint32_t weight)
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
void splat(World &world, int32_t x, int32_t y, Rgb color)
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
Rgb mix(Rgb a, Rgb b, uint32_t t)
{
    return {static_cast<uint8_t>((a.r * (256u - t) + b.r * t) >> 8), static_cast<uint8_t>((a.g * (256u - t) + b.g * t) >> 8),
            static_cast<uint8_t>((a.b * (256u - t) + b.b * t) >> 8)};
}

// a line from x0, y0 to x1, y1 (1/256 pixel), its color going from `from` to `to`
void line(World &world, int32_t x0, int32_t y0, int32_t x1, int32_t y1, Rgb from, Rgb to)
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
void disc(World &world, int32_t x, int32_t y, int32_t radius, Rgb color, bool firm)
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

// a free particle; nullptr when all live
Particle *spawn(World &world)
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

int16_t i16(int32_t v)
{
    return static_cast<int16_t>(v < -32767 ? -32767 : (v > 32767 ? 32767 : v));
}

// keeps a coordinate within 0..limit: mirrored at an edge, its velocity turned inward with keep /
// 256 of it
void bounce(int32_t &p, int16_t &v, int32_t limit, int32_t keep = 256)
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
int32_t random_signed(Memory &m, int32_t range)
{
    return static_cast<int32_t>((static_cast<int64_t>(random16(m)) * (2 * range + 1)) >> 16) - range;
}

// 0..limit
int32_t random_upto(Memory &m, int32_t limit)
{
    return static_cast<int32_t>((static_cast<int64_t>(random16(m)) * (limit + 1)) >> 16);
}

uint32_t isqrt(uint32_t n)
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
int32_t across(uint16_t pixels, int32_t sine)
{
    return static_cast<int32_t>((static_cast<int64_t>(sine) + 32767) * (pixels - 1) * 256 / 65534);
}

// more than a pixel off the canvas
bool off_canvas(const World &world, const Particle &p)
{
    return p.x < -256 || p.y < -256 || p.x > world.w * 256 || p.y > world.h * 256;
}

// the loudest of the bands first..last - 1 (at least `first`)
uint8_t loudest(const uint8_t (&bands)[kBands], size_t first, size_t last)
{
    uint8_t value = bands[first < kBands ? first : kBands - 1];
    for (size_t b = first + 1; b < last && b < kBands; b++) {
        value = bands[b] > value ? bands[b] : value;
    }
    return value;
}

size_t live_particles(const World &world)
{
    size_t live = 0;
    for (size_t i = 0; i < world.slots; i++) {
        live += world.particles[i].life != 0 ? 1 : 0;
    }
    return live;
}

uint16_t longer_side(const World &world)
{
    return world.w > world.h ? world.w : world.h;
}

uint16_t shorter_side(const World &world)
{
    return world.w < world.h ? world.w : world.h;
}

} // namespace

size_t trail_memory_size(uint16_t width, uint16_t height)
{
    return align8(static_cast<size_t>(width) * height * sizeof(Rgb)) + align8(sizeof(Scene)) +
           particle_slots(width, height) * sizeof(Particle);
}

// Colored bursts (after WLED's): eight rays from a point wandering over the canvas to ends that
// circle around, each in its own color of the palette (the hue circle without one), dim at the
// point and full at the end with a light dot there; the colors move on. The trail fades and
// blurs. Reactive: the rays reach as far as the sound is loud (never below a quarter), the whole
// is faster and brighter with it; a beat moves the colors on.
void colored_bursts(Canvas &canvas, const Params &params, uint32_t ms)
{
    Frame f;
    if (!begin(canvas, params, ms, f)) {
        return;
    }
    World &world = f.world;
    Scene &s = *world.scene;
    const uint8_t hue = beat_hue(*world.m, f.sound, f.dt, 40u * params.strength / 128u);
    constexpr uint32_t kRays = 8;
    for (uint32_t step = 0; step < f.steps; step++) {
        const uint32_t t = ++s.steps;
        fade(world, 192);
        blur(world, 40);
        // the point goes across in 5 and 4 s, the ends in 6.5 and 4.5 s
        const int32_t cx = across(world.w, sin16(t * 210u));
        const int32_t cy = across(world.h, sin16(t * 262u + 16384u));
        for (uint32_t i = 0; i < kRays; i++) {
            int32_t ex = across(world.w, sin16(t * 160u + i * 65536u / kRays));
            int32_t ey = across(world.h, sin16(t * 233u + i * 2u * 65536u / kRays + 16384u));
            ex = cx + (ex - cx) * f.sound.value / 255;
            ey = cy + (ey - cy) * f.sound.value / 255;
            const Rgb end = paint(params, static_cast<uint8_t>(i * 256u / kRays + (t >> 3) + hue), 255, 255);
            line(world, cx, cy, ex, ey, scale(end, 40), end);
            splat(world, ex, ey, mix(end, {255, 255, 255}, 128));
        }
    }
    show(canvas, world, f.sound.value);
}

// Blobs (after WLED's): discs in the palette's colors (the hue circle without one) wandering over
// the canvas and bouncing off its edges, each growing to its size and shrinking away again in 4 to
// 8 s, then coming back elsewhere in another color; one per 64 pixels, 3 to 8. The trail fades and
// blurs. Reactive: each blob is as large as its band is loud (bass to treble over the blobs, never
// below a quarter), the whole faster and brighter with the sound; a beat pushes them.
void blobs(Canvas &canvas, const Params &params, uint32_t ms)
{
    Frame f;
    if (!begin(canvas, params, ms, f)) {
        return;
    }
    World &world = f.world;
    Scene &s = *world.scene;
    Memory &m = *world.m;
    const size_t many = static_cast<size_t>(world.w) * world.h / 64;
    const size_t count = many < 3 ? 3 : (many > 8 ? 8 : many);
    // the largest radius: a quarter of the shorter side, at least a pixel and a half
    const int32_t largest = shorter_side(world) * 64 > 384 ? shorter_side(world) * 64 : 384;
    const int32_t xmax = (world.w - 1) * 256;
    const int32_t ymax = (world.h - 1) * 256;
    for (uint32_t step = 0; step < f.steps; step++) {
        s.steps++;
        fade(world, 216);
        blur(world, 32);
        for (size_t i = 0; i < count; i++) {
            Particle &b = world.particles[i];
            if (b.life == 0) {
                // life is the size's phase: half a sine, up and down
                b = {random_upto(m, xmax), random_upto(m, ymax), i16(random_signed(m, 40)), i16(random_signed(m, 40)), 1,
                     random8(m), static_cast<uint8_t>(i * kBands / count)};
            }
            if (f.sound.beat && step == 0) {
                b.vx = i16(b.vx + random_signed(m, 80));
                b.vy = i16(b.vy + random_signed(m, 80));
            }
            // a push slows down to the cruising speed
            b.vx = i16(b.vx > 48 || b.vx < -48 ? b.vx - b.vx / 16 : b.vx);
            b.vy = i16(b.vy > 48 || b.vy < -48 ? b.vy - b.vy / 16 : b.vy);
            b.x += b.vx;
            b.y += b.vy;
            bounce(b.x, b.vx, xmax);
            bounce(b.y, b.vy, ymax);
            const uint32_t pace = 64u + (i * 37u & 63u);
            b.life = b.life + pace >= 32768u ? 0 : static_cast<uint16_t>(b.life + pace);
            const int32_t radius = static_cast<int32_t>(static_cast<int64_t>(largest) * sin16(b.life) >> 15) *
                                   with_sound(m, f.bands[b.tag]) / 255;
            disc(world, b.x, b.y, radius, paint(params, b.hue, 255, 255), true);
        }
    }
    show(canvas, world, f.sound.value);
}

// Waverly (after WLED's): waves of noise mirrored up and down from the middle row, each column as
// high as the noise there, which drifts slowly; in the palette's colors from the middle out (the
// hue circle without one). The trail fades and blurs. Reactive: the waves are as high as the sound
// is loud (never below a quarter) and move faster with it.
void waverly(Canvas &canvas, const Params &params, uint32_t ms)
{
    Frame f;
    if (!begin(canvas, params, ms, f)) {
        return;
    }
    World &world = f.world;
    Scene &s = *world.scene;
    const uint32_t half = (world.h + 1u) / 2u;
    // four noise cells across
    const uint32_t across_x = 1024u / world.w;
    for (uint32_t step = 0; step < f.steps; step++) {
        const uint32_t t = ++s.steps;
        fade(world, 208);
        blur(world, 24);
        // the noise drifts more than a cell a second
        const uint32_t z = t * 5u;
        for (uint16_t x = 0; x < world.w; x++) {
            uint32_t n = noise8(x * across_x, z, z >> 1);
            // the noise mostly lies between 48 and 208: stretched to the whole height
            n = n < 48 ? 0 : (n - 48) * 255u / 160u;
            n = n > 255 ? 255 : n;
            // the height in 1/256 row from the middle
            const uint32_t height = n * f.sound.value / 255u * half * 256u / 255u;
            const uint32_t full = height >> 8;
            for (uint32_t j = 0; j <= full && j < half; j++) {
                const uint32_t part = j < full ? 256u : (height & 255u);
                const Rgb c = paint(params, static_cast<uint8_t>(j * 200u / half + (t >> 4)), 255, 255);
                plot(world, x, static_cast<int32_t>((world.h - 1u) / 2u) - static_cast<int32_t>(j), c, part);
                plot(world, x, static_cast<int32_t>(world.h / 2u + j), c, part);
            }
        }
    }
    show(canvas, world, 255);
}

// PS vortex (after WLED's): particles shot from the middle in four spiral arms that turn, each arm
// in its own color of the palette (the hue circle without one), the colors moving on; the spin
// turns round every 3 to 10 s. The particles reach the edge in about 0.8 s; the trail smears.
// Reactive: faster and brighter with the sound, a beat shoots a burst from every arm and moves the
// colors on.
void ps_vortex(Canvas &canvas, const Params &params, uint32_t ms)
{
    Frame f;
    if (!begin(canvas, params, ms, f)) {
        return;
    }
    World &world = f.world;
    Scene &s = *world.scene;
    Memory &m = *world.m;
    const uint8_t hue = beat_hue(m, f.sound, f.dt, 32u * params.strength / 128u);
    if (s.direction == 0) {
        s.direction = 1;
        s.timer = 190u + 2u * random_below(m, 218);
    }
    const int32_t cx = (world.w - 1) * 128;
    const int32_t cy = (world.h - 1) * 128;
    const int32_t fast = longer_side(world) * 256 / 100;
    const int32_t speed = fast > 24 ? fast : 24;
    constexpr uint32_t kArms = 4;
    for (uint32_t step = 0; step < f.steps; step++) {
        const uint32_t t = ++s.steps;
        fade(world, 224);
        blur(world, 32);
        // about half a turn a second
        s.angle += static_cast<uint32_t>(s.direction * 600);
        if (--s.timer == 0) {
            s.direction = -s.direction;
            s.timer = 190u + 2u * random_below(m, 218);
        }
        const uint32_t shots = f.sound.beat && step == 0 ? 4 : 1;
        for (uint32_t arm = 0; arm < kArms; arm++) {
            for (uint32_t k = 0; k < shots; k++) {
                Particle *p = spawn(world);
                if (p == nullptr) {
                    break;
                }
                const uint32_t angle = s.angle + arm * 65536u / kArms + static_cast<uint32_t>(random_signed(m, 1000));
                const int32_t v = k == 0 ? speed : speed * (160 + random_below(m, 160)) / 256;
                *p = {cx, cy, i16(cos16(angle) * v >> 15), i16(sin16(angle) * v >> 15), 400,
                      static_cast<uint8_t>(arm * 256u / kArms + (t >> 2) + hue), 0};
            }
        }
        for (size_t i = 0; i < world.slots; i++) {
            Particle &p = world.particles[i];
            if (p.life == 0) {
                continue;
            }
            p.x += p.vx;
            p.y += p.vy;
            p.life = off_canvas(world, p) ? 0 : static_cast<uint16_t>(p.life - 1);
            if (p.life != 0) {
                splat(world, p.x, p.y, paint(params, p.hue, 255, 255));
            }
        }
    }
    show(canvas, world, f.sound.value);
}

// PS fuzzy noise (after WLED's): particles drifting in a field of noise that changes slowly, each
// pushed the way the noise points where it is, slowed by friction and bouncing off the edges;
// colored by the noise around it in the palette (the hue circle without one), a little jitter so
// they do not all gather where the noise turns. The trail smears. Reactive: faster and brighter
// with the sound, a beat kicks every particle.
void ps_fuzzy_noise(Canvas &canvas, const Params &params, uint32_t ms)
{
    Frame f;
    if (!begin(canvas, params, ms, f)) {
        return;
    }
    World &world = f.world;
    Scene &s = *world.scene;
    Memory &m = *world.m;
    const int32_t xmax = (world.w - 1) * 256;
    const int32_t ymax = (world.h - 1) * 256;
    // four noise cells along the longer side
    const uint32_t longer = longer_side(world);
    for (uint32_t step = 0; step < f.steps; step++) {
        const uint32_t t = ++s.steps;
        fade(world, 232);
        blur(world, 56);
        // the field changes by a cell a second
        const uint32_t z = t * 4u;
        for (size_t i = 0; i < world.slots; i++) {
            Particle &p = world.particles[i];
            if (p.life == 0) {
                p = {random_upto(m, xmax), random_upto(m, ymax), 0, 0, 1, 0, 0};
            }
            const uint8_t n = noise8(static_cast<uint32_t>(p.x) * 4u / longer, static_cast<uint32_t>(p.y) * 4u / longer, z);
            // the noise as a way: twice around over its range (it mostly lies in the middle)
            const uint32_t angle = n * 512u;
            // pushed by 7, slowed by a twentieth: at most about 35 pixels a second
            p.vx = i16(p.vx + (cos16(angle) * 7 >> 15) - p.vx / 20 + random_signed(m, 2));
            p.vy = i16(p.vy + (sin16(angle) * 7 >> 15) - p.vy / 20 + random_signed(m, 2));
            if (f.sound.beat && step == 0) {
                p.vx = i16(p.vx + random_signed(m, 96));
                p.vy = i16(p.vy + random_signed(m, 96));
            }
            p.x += p.vx;
            p.y += p.vy;
            bounce(p.x, p.vx, xmax);
            bounce(p.y, p.vy, ymax);
            p.hue = n;
            splat(world, p.x, p.y, paint(params, static_cast<uint8_t>(n * 2u + (t >> 3)), 255, 255));
        }
    }
    show(canvas, world, f.sound.value);
}

// PS waterfall (after WLED's): water poured over the canvas from the top, a quarter of the way in:
// drops shot to the right with a little scatter, falling, bouncing off the walls and splashing on
// the floor, gone 0.4 s after they reach it; in blues (the palette's colors with one). As many
// drops as keep a third of the particles busy, so the flow does not stall. The trail smears.
// Reactive: up to twice the water and brighter with the sound, a beat pours a burst.
void ps_waterfall(Canvas &canvas, const Params &params, uint32_t ms)
{
    Frame f;
    if (!begin(canvas, params, ms, f)) {
        return;
    }
    World &world = f.world;
    Scene &s = *world.scene;
    Memory &m = *world.m;
    const int32_t xmax = (world.w - 1) * 256;
    const int32_t ymax = (world.h - 1) * 256;
    // down the canvas in about a second
    const int32_t gravity = world.h / 8 > 1 ? world.h / 8 : 1;
    const int32_t sideways = world.w * 2 > 16 ? world.w * 2 : 16;
    constexpr uint16_t kLife = 200;
    constexpr uint16_t kSplash = 24;
    // a drop lives about 90 steps: the drops a step (x 65536) that keep a third of the particles
    const uint32_t flow = static_cast<uint32_t>(world.slots) * 65536u / 270u;
    for (uint32_t step = 0; step < f.steps; step++) {
        s.steps++;
        fade(world, 200);
        blur(world, 24);
        s.timer += flow + (f.sound.on ? flow / 255u * f.sound.level : 0u);
        uint32_t drops = s.timer >> 16;
        s.timer &= 0xffffu;
        drops += f.sound.beat && step == 0 ? 12u : 0u;
        for (uint32_t d = 0; d < drops; d++) {
            Particle *p = spawn(world);
            if (p == nullptr) {
                break;
            }
            const int32_t x = xmax / 4 + random_signed(m, 128);
            const auto hue = static_cast<uint8_t>(params.palette == 0 ? 130 + random_below(m, 40) : random8(m));
            *p = {x < 0 ? 0 : x, 0, i16(sideways * (128 + random8(m)) / 256), i16(random_below(m, 32)), kLife, hue, 0};
        }
        for (size_t i = 0; i < world.slots; i++) {
            Particle &p = world.particles[i];
            if (p.life == 0) {
                continue;
            }
            p.vy = i16(p.vy + gravity);
            p.x += p.vx;
            p.y += p.vy;
            bounce(p.x, p.vx, xmax, 192);
            if (p.y >= ymax) {
                // a splash: a third of the fall back up, scattered sideways; on the floor it slows
                // and soon goes out
                p.y = ymax;
                p.vy = i16(-p.vy / 3);
                p.vx = i16(p.vx - p.vx / 32 + random_signed(m, 24));
                p.life = p.life > kSplash ? kSplash : p.life;
            }
            p.y = p.y < 0 ? 0 : p.y;
            p.life--;
            const uint32_t light = p.life < kSplash ? p.life * 255u / kSplash : 255u;
            splat(world, p.x, p.y, paint(params, p.hue, 200, static_cast<uint8_t>(light)));
        }
    }
    show(canvas, world, f.sound.value);
}

// PS GEQ 2D (after WLED's): particles shot up from the floor, the bands spread over the columns
// (bass left, treble right), each the higher and the more often the louder its band; they fall
// back, bounce a little on the floor and go out. In the colors of the spectrum (the palette's with
// one). The trail smears. Without sound a dim line at the bottom, as Spectrum.
void ps_geq_2d(Canvas &canvas, const Params &params, uint32_t ms)
{
    Frame f;
    if (!begin(canvas, params, ms, f)) {
        return;
    }
    World &world = f.world;
    Scene &s = *world.scene;
    Memory &m = *world.m;
    const uint16_t w = world.w;
    const uint16_t h = world.h;
    const int32_t xmax = (w - 1) * 256;
    const int32_t ymax = (h - 1) * 256;
    // a full band reaches the top in half a second
    const int32_t g = (h - 1) * 512 / 961;
    const int32_t gravity = g > 1 ? g : 1;
    // all columns full shoot about as many as there are particles
    const uint32_t often = static_cast<uint32_t>(world.slots) * 65536u / (w * 90u);
    const uint32_t chance = often > 65535 ? 65535 : often;
    for (uint32_t step = 0; step < f.steps; step++) {
        s.steps++;
        fade(world, 200);
        blur(world, 16);
        // from a column at random, so with all particles used no side gets them all
        const auto first = static_cast<uint16_t>(random16(m) % w);
        for (uint16_t k = 0; k < w; k++) {
            const auto x = static_cast<uint16_t>((first + k) % w);
            const uint8_t level = loudest(f.bands, static_cast<size_t>(x) * kBands / w, (static_cast<size_t>(x) + 1) * kBands / w);
            if (level < 8 || random16(m) >= level * chance / 255u) {
                continue;
            }
            Particle *p = spawn(world);
            if (p == nullptr) {
                break;
            }
            const auto height = static_cast<uint32_t>(level * ymax / 255 * (224 + random_below(m, 32)) / 256);
            const auto v = static_cast<int32_t>(isqrt(2u * static_cast<uint32_t>(gravity) * height));
            const int32_t px = x * 256 + random_signed(m, 64);
            *p = {px < 0 ? 0 : (px > xmax ? xmax : px), ymax, i16(random_signed(m, 16)), i16(-v), 150,
                  static_cast<uint8_t>(x * 200u / (w > 1 ? w - 1u : 1u)), 0};
        }
        for (size_t i = 0; i < world.slots; i++) {
            Particle &p = world.particles[i];
            if (p.life == 0) {
                continue;
            }
            p.vy = i16(p.vy + gravity);
            p.x += p.vx;
            p.y += p.vy;
            bounce(p.x, p.vx, xmax, 192);
            p.life--;
            if (p.y >= ymax) {
                // a little bounce; resting, it goes out
                p.y = ymax;
                const int32_t up = p.vy / 4;
                p.vy = i16(-up);
                p.life = up < gravity * 2 ? 0 : p.life;
            }
            if (p.y < 0) {
                p.y = 0;
                p.vy = 0;
            }
            if (p.life != 0) {
                splat(world, p.x, p.y, paint(params, p.hue, 255, 255));
            }
        }
    }
    show(canvas, world, 255);
    if (live_particles(world) == 0) {
        for (uint16_t x = 0; x < w; x++) {
            canvas.at(x, static_cast<uint16_t>(h - 1)) = paint(params, static_cast<uint8_t>(x * 200u / (w > 1 ? w - 1u : 1u)), 255, 40);
        }
    }
}

// PS GEQ nova (after WLED's): particles shot from the middle in 16 directions, one per pair of
// bands, the faster and the more often the louder the pair; the directions turn slowly, the colors
// move along (the palette's with one). The trail smears. Without sound a dim dot in the middle.
void ps_geq_nova(Canvas &canvas, const Params &params, uint32_t ms)
{
    Frame f;
    if (!begin(canvas, params, ms, f)) {
        return;
    }
    World &world = f.world;
    Scene &s = *world.scene;
    Memory &m = *world.m;
    const int32_t cx = (world.w - 1) * 128;
    const int32_t cy = (world.h - 1) * 128;
    const int32_t reach = longer_side(world) * 128;
    constexpr uint32_t kRays = 16;
    const uint32_t often = static_cast<uint32_t>(world.slots) * 65536u / (kRays * 90u);
    const uint32_t chance = often > 65535 ? 65535 : often;
    uint32_t t = s.steps;
    for (uint32_t step = 0; step < f.steps; step++) {
        t = ++s.steps;
        fade(world, 216);
        blur(world, 32);
        // a turn in 7 s
        s.angle += 150;
        const uint32_t first = random_below(m, kRays);
        for (uint32_t k = 0; k < kRays; k++) {
            const uint32_t b = (first + k) % kRays;
            const uint8_t level = loudest(f.bands, b * 2, b * 2 + 2);
            if (level < 8 || random16(m) >= level * chance / 255u) {
                continue;
            }
            Particle *p = spawn(world);
            if (p == nullptr) {
                break;
            }
            // to the edge in 1.6 s quiet, 0.5 s loud
            const int32_t v = reach / (100 - level * 70 / 255);
            const uint32_t angle = s.angle + b * 65536u / kRays + static_cast<uint32_t>(random_signed(m, 400));
            *p = {cx, cy, i16(cos16(angle) * v >> 15), i16(sin16(angle) * v >> 15), 200,
                  static_cast<uint8_t>(b * 256u / kRays + (t >> 3)), 0};
        }
        for (size_t i = 0; i < world.slots; i++) {
            Particle &p = world.particles[i];
            if (p.life == 0) {
                continue;
            }
            p.x += p.vx;
            p.y += p.vy;
            p.life = off_canvas(world, p) ? 0 : static_cast<uint16_t>(p.life - 1);
            if (p.life != 0) {
                splat(world, p.x, p.y, paint(params, p.hue, 255, 255));
            }
        }
    }
    show(canvas, world, 255);
    if (live_particles(world) == 0) {
        canvas.at(static_cast<uint16_t>(cx >> 8), static_cast<uint16_t>(cy >> 8)) = paint(params, static_cast<uint8_t>(t >> 3), 255, 40);
    }
}

// PS ghost rider (after WLED's): a rider running over the canvas on a curl, always turning a little
// and bouncing off the edges, shedding particles backwards; they slow down, scatter, fade and change
// their color as they age (the palette's colors with one). The trail blurs. Reactive: faster and
// brighter with the sound, a beat makes it turn sharply and shed a burst.
void ps_ghost_rider(Canvas &canvas, const Params &params, uint32_t ms)
{
    Frame f;
    if (!begin(canvas, params, ms, f)) {
        return;
    }
    World &world = f.world;
    Scene &s = *world.scene;
    Memory &m = *world.m;
    const int32_t xmax = (world.w - 1) * 256;
    const int32_t ymax = (world.h - 1) * 256;
    if (s.steps == 0) {
        s.x = xmax / 2;
        s.y = ymax / 2;
        s.angle = random16(m);
    }
    // across the canvas in 1.3 s
    const int32_t fast = longer_side(world) * 256 / 80;
    const int32_t speed = fast > 48 ? fast : 48;
    constexpr uint16_t kLife = 80;
    for (uint32_t step = 0; step < f.steps; step++) {
        const uint32_t t = ++s.steps;
        fade(world, 232);
        blur(world, 48);
        // the curl: a third of a turn a second, more and less in a slow sway
        s.angle += static_cast<uint32_t>(330 + (sin16(t * 90u) * 250 >> 15));
        const bool beat = f.sound.beat && step == 0;
        if (beat) {
            s.angle += (random8(m) & 1u) != 0 ? 8192u : 0x10000u - 8192u;
        }
        s.x += cos16(s.angle) * speed >> 15;
        s.y += sin16(s.angle) * speed >> 15;
        if (s.x < 0 || s.x > xmax) {
            s.angle = 32768u - s.angle;
            s.x = s.x < 0 ? 0 : xmax;
        }
        if (s.y < 0 || s.y > ymax) {
            s.angle = 0u - s.angle;
            s.y = s.y < 0 ? 0 : ymax;
        }
        const auto hue = static_cast<uint8_t>(t >> 1);
        const uint32_t sheds = beat ? 10 : 2;
        for (uint32_t k = 0; k < sheds; k++) {
            Particle *p = spawn(world);
            if (p == nullptr) {
                break;
            }
            const uint32_t angle = s.angle + 32768u + static_cast<uint32_t>(random_signed(m, 6000));
            const int32_t v = speed * (64 + random_below(m, 64)) / 256;
            *p = {s.x, s.y, i16(cos16(angle) * v >> 15), i16(sin16(angle) * v >> 15), kLife, hue, 0};
        }
        for (size_t i = 0; i < world.slots; i++) {
            Particle &p = world.particles[i];
            if (p.life == 0) {
                continue;
            }
            p.vx = i16(p.vx - p.vx / 24);
            p.vy = i16(p.vy - p.vy / 24);
            p.x += p.vx;
            p.y += p.vy;
            bounce(p.x, p.vx, xmax, 128);
            bounce(p.y, p.vy, ymax, 128);
            p.life--;
            const uint32_t age = kLife - p.life;
            splat(world, p.x, p.y,
                  paint(params, static_cast<uint8_t>(p.hue + age * 2u), 255, static_cast<uint8_t>(p.life * 255u / kLife)));
        }
        splat(world, s.x, s.y, mix(paint(params, hue, 255, 255), {255, 255, 255}, 128));
    }
    show(canvas, world, f.sound.value);
}

// PS blobs (after WLED's): soft blobs drifting at random, each pulsing in size, pushing the others
// away where they meet and bouncing off the edges; each lives 5 to 15 s, fades out and comes back
// elsewhere in another color (the palette's with one); one per 96 pixels, 3 to 12. Reactive: each
// blob swells with its band (bass to treble over the blobs, never below a quarter), the whole
// faster and brighter with the sound; a beat pushes them.
void ps_blobs(Canvas &canvas, const Params &params, uint32_t ms)
{
    Frame f;
    if (!begin(canvas, params, ms, f)) {
        return;
    }
    World &world = f.world;
    Scene &s = *world.scene;
    Memory &m = *world.m;
    constexpr size_t kMost = 12;
    const size_t many = static_cast<size_t>(world.w) * world.h / 96;
    const size_t count = many < 3 ? 3 : (many > kMost ? kMost : many);
    const int32_t xmax = (world.w - 1) * 256;
    const int32_t ymax = (world.h - 1) * 256;
    // a fifth of the shorter side, at least two pixels
    const int32_t base = shorter_side(world) * 256 / 5 > 512 ? shorter_side(world) * 256 / 5 : 512;
    for (uint32_t step = 0; step < f.steps; step++) {
        const uint32_t t = ++s.steps;
        fade(world, 160);
        int32_t radius[kMost];
        for (size_t i = 0; i < count; i++) {
            Particle &b = world.particles[i];
            // each its own lifetime, 5 to 15 s
            const auto lifetime = static_cast<uint16_t>(310u + i * 389u % 620u);
            if (b.life == 0) {
                b = {random_upto(m, xmax), random_upto(m, ymax), i16(random_signed(m, 32)), i16(random_signed(m, 32)),
                     lifetime, random8(m), static_cast<uint8_t>(i * kBands / count)};
            }
            // wobbling about, pushed by the beat, slowed to the cruising speed
            b.vx = i16(b.vx + random_signed(m, 3) + (f.sound.beat && step == 0 ? random_signed(m, 96) : 0));
            b.vy = i16(b.vy + random_signed(m, 3) + (f.sound.beat && step == 0 ? random_signed(m, 96) : 0));
            b.vx = i16(b.vx > 40 || b.vx < -40 ? b.vx - b.vx / 16 : b.vx);
            b.vy = i16(b.vy > 40 || b.vy < -40 ? b.vy - b.vy / 16 : b.vy);
            b.x += b.vx;
            b.y += b.vy;
            bounce(b.x, b.vx, xmax);
            bounce(b.y, b.vy, ymax);
            b.life--;
            const int32_t pulse = 180 + (sin16(t * 200u + static_cast<uint32_t>(i) * 9000u) * 75 >> 15);
            radius[i] = base * pulse / 255 * with_sound(m, f.bands[b.tag]) / 255;
        }
        // blobs that meet push each other apart
        for (size_t i = 0; i < count; i++) {
            for (size_t j = i + 1; j < count; j++) {
                Particle &a = world.particles[i];
                Particle &b = world.particles[j];
                const int64_t dx = (b.x - a.x) >> 4;
                const int64_t dy = (b.y - a.y) >> 4;
                const int64_t reach = (radius[i] + radius[j]) >> 4;
                if (dx * dx + dy * dy >= reach * reach) {
                    continue;
                }
                const int32_t px = dx > 0 ? 4 : (dx < 0 ? -4 : 0);
                const int32_t py = dy > 0 ? 4 : (dy < 0 ? -4 : 0);
                a.vx = i16(a.vx - px);
                a.vy = i16(a.vy - py);
                b.vx = i16(b.vx + px);
                b.vy = i16(b.vy + py);
            }
        }
        for (size_t i = 0; i < count; i++) {
            const Particle &b = world.particles[i];
            const auto lifetime = static_cast<uint32_t>(310u + i * 389u % 620u);
            // in and out in half a second
            const uint32_t age = lifetime - b.life;
            const uint32_t in = age * 8u > 255u ? 255u : age * 8u;
            const uint32_t out = b.life * 8u > 255u ? 255u : b.life * 8u;
            disc(world, b.x, b.y, radius[i], paint(params, b.hue, 255, static_cast<uint8_t>(in < out ? in : out)), false);
        }
    }
    show(canvas, world, f.sound.value);
}

// PS galaxy (after WLED's): stars born near the middle in two arms that wind out as the galaxy turns
// (the inner stars turn faster), warm white in the middle and blue outside (the palette's colors
// from the middle out with one), twinkling a little, a glow in the middle. Reactive: faster and
// brighter with the sound, a beat sends a burst of stars out.
void ps_galaxy(Canvas &canvas, const Params &params, uint32_t ms)
{
    Frame f;
    if (!begin(canvas, params, ms, f)) {
        return;
    }
    World &world = f.world;
    Scene &s = *world.scene;
    Memory &m = *world.m;
    const int32_t cx = (world.w - 1) * 128;
    const int32_t cy = (world.h - 1) * 128;
    // the stars in polar: x the radius (1/256 pixel), y the angle (1/65536 turn), vx the way out
    const int32_t reach = longer_side(world) * 154;
    const int32_t core = reach / 6 > 512 ? reach / 6 : 512;
    // the stars leave in about 5 s (330 steps): as many born as keep three quarters of them alive
    const uint32_t births = static_cast<uint32_t>(world.slots) * 3u * 65536u / (4u * 330u);
    for (uint32_t step = 0; step < f.steps; step++) {
        const uint32_t t = ++s.steps;
        fade(world, 176);
        // the arms: a turn in 4 s
        s.angle += 260;
        s.timer += births;
        const bool beat = f.sound.beat && step == 0;
        uint32_t born = s.timer >> 16;
        s.timer &= 0xffffu;
        born += beat ? 16 : 0;
        for (uint32_t k = 0; k < born; k++) {
            Particle *p = spawn(world);
            if (p == nullptr) {
                break;
            }
            const uint32_t angle = s.angle + (random8(m) & 1u) * 32768u + static_cast<uint32_t>(random_signed(m, 3000));
            // out in 3.3 to 6.6 s, a burst three times as fast
            const int32_t out = reach / 250 * (64 + random_below(m, 64)) / 128 * (beat && k >= born - 16 ? 3 : 1);
            *p = {random_upto(m, core / 2), static_cast<int32_t>(angle & 0xffffu), i16(out > 1 ? out : 1), 0, 1000, 0,
                  static_cast<uint8_t>(128 + random_below(m, 128))};
        }
        for (size_t i = 0; i < world.slots; i++) {
            Particle &p = world.particles[i];
            if (p.life == 0) {
                continue;
            }
            p.x += p.vx;
            // the inner stars turn as fast as the arms, the outer ones slower: the arms wind
            p.y = static_cast<int32_t>((static_cast<uint32_t>(p.y) + 260u * static_cast<uint32_t>(core) /
                                                                         static_cast<uint32_t>(core + p.x)) &
                                       0xffffu);
            p.life = p.x > reach ? 0 : static_cast<uint16_t>(p.life - 1);
            if (p.life == 0) {
                continue;
            }
            const auto angle = static_cast<uint32_t>(p.y);
            const int32_t x = cx + static_cast<int32_t>(static_cast<int64_t>(p.x) * cos16(angle) >> 15);
            const int32_t y = cy + static_cast<int32_t>(static_cast<int64_t>(p.x) * sin16(angle) >> 15);
            const int32_t twinkle = p.tag + (sin16(t * 1500u + static_cast<uint32_t>(i) * 7000u) * 40 >> 15);
            const auto light = static_cast<uint8_t>(twinkle < 0 ? 0 : (twinkle > 255 ? 255 : twinkle));
            const auto far = static_cast<uint32_t>(p.x) * 255u / static_cast<uint32_t>(reach);
            const Rgb c = scale(params.palette == 0 ? mix({255, 200, 120}, {50, 100, 255}, far + 1u)
                                                    : palette_color(params.palette, static_cast<uint8_t>(far * 240u / 255u), params.color),
                                light);
            splat(world, x, y, c);
        }
        const Rgb glow = scale(params.palette == 0 ? Rgb{255, 200, 120} : palette_color(params.palette, 0, params.color), 80);
        disc(world, cx, cy, core, glow, false);
    }
    show(canvas, world, f.sound.value);
}

} // namespace px::effects::detail
