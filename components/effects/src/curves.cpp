// The second set of animations after WLED's 2D ones that draw on a trail (docs/leds.md section
// 6.3): DNA spiral, drift, drift rose, fireworks, fireworks 1D, ghost rider, Lissajous, sindots and
// scrolling text. Written from WLED's descriptions and how they look, not from its code. The trail,
// the steps of the animation's own clock and the brightness with the sound work as in
// particles.cpp (stage.hpp); WLED's sliders beyond the speed are fixed values here.

#include "px/effects/overlay.hpp"
#include "stage.hpp"

namespace px::effects::detail {

namespace {

// `sine` (-32767..32767) times `length`, both in 1/256 pixel
int32_t times(int32_t sine, int32_t length)
{
    return static_cast<int32_t>(static_cast<int64_t>(sine) * length >> 15);
}

} // namespace

// DNA spiral (after WLED's): two strands winding around each other down the canvas (a turn and a
// half over the height), a rung between them every third row; the helix turns, so it seems to
// scroll up. The strand in front is lighter
// than the one behind; in the palette's colors along the rows (the hue circle without one), the
// second strand a third of the way on, the colors moving. The trail fades. Reactive: as wide as it
// is loud (never below a quarter), faster and brighter; a beat moves the colors on.
void dna_spiral(Canvas &canvas, const Params &params, uint32_t ms)
{
    Frame f;
    if (!begin(canvas, params, ms, f)) {
        return;
    }
    World &world = f.world;
    Scene &s = *world.scene;
    const uint8_t hue = beat_hue(*world.m, f.sound, f.dt, 32u * params.strength / 128u);
    const int32_t cx = (world.w - 1) * 128;
    const int32_t swing = cx * f.sound.value / 255;
    const uint32_t per_row = 3u * 32768u / world.h;
    for (uint32_t step = 0; step < f.steps; step++) {
        const uint32_t t = ++s.steps;
        fade(world, 128);
        // a turn in 1.5 s
        const uint32_t turn = t * 700u;
        for (uint16_t y = 0; y < world.h; y++) {
            const uint32_t a = turn + y * per_row;
            const int32_t side = times(sin16(a), swing);
            // > 0: the first strand is in front
            const int32_t depth = cos16(a);
            const auto row_hue = static_cast<uint8_t>(y * 128u / world.h + (t >> 2) + hue);
            const Rgb one = paint(params, row_hue, 255, static_cast<uint8_t>(160 + (depth * 95 >> 15)));
            const Rgb two = paint(params, static_cast<uint8_t>(row_hue + 85), 255, static_cast<uint8_t>(160 - (depth * 95 >> 15)));
            const int32_t py = y * 256;
            if (y % 3 == 0) {
                line(world, cx + side, py, cx - side, py, scale(one, 80), scale(two, 80));
            }
            splat(world, cx + side, py, one);
            splat(world, cx - side, py, two);
        }
    }
    show(canvas, world, f.sound.value);
}

// Drift (after WLED's): a turning kaleidoscope: a point on every ring around the middle, a ring a
// pixel apart out to half the longer side, each turning at its own pace (the innermost about a turn
// a second, the outermost seven times slower) and mirrored across both axes; in the palette's colors
// from the middle out (the hue circle without one), moving on. The trail fades and blurs them into
// swirls. Reactive: faster and brighter with the sound, a beat moves the colors on.
void drift(Canvas &canvas, const Params &params, uint32_t ms)
{
    Frame f;
    if (!begin(canvas, params, ms, f)) {
        return;
    }
    World &world = f.world;
    Scene &s = *world.scene;
    const uint8_t hue = beat_hue(*world.m, f.sound, f.dt, 32u * params.strength / 128u);
    const int32_t cx = (world.w - 1) * 128;
    const int32_t cy = (world.h - 1) * 128;
    const uint32_t rings = longer_side(world) / 2u + 1u;
    for (uint32_t step = 0; step < f.steps; step++) {
        const uint32_t t = ++s.steps;
        fade(world, 240);
        blur(world, 48);
        for (uint32_t r = 1; r <= rings; r++) {
            const uint32_t angle = t * (160u + (rings - r) * 900u / rings);
            const auto radius = static_cast<int32_t>(r * 256u);
            const int32_t dx = times(cos16(angle), radius);
            const int32_t dy = times(sin16(angle), radius);
            const Rgb c = paint(params, static_cast<uint8_t>(r * 256u / rings + (t >> 2) + hue), 255, 255);
            splat(world, cx + dx, cy + dy, c);
            splat(world, cx - dx, cy + dy, c);
            splat(world, cx + dx, cy - dy, c);
            splat(world, cx - dx, cy - dy, c);
        }
    }
    show(canvas, world, f.sound.value);
}

// Drift rose (after WLED's): 36 points at their own angles around the middle, each swinging out
// and through the middle to the other side, the n-th n times as fast as the first (a minute at
// speed 128): arms wind up and unwind, petals come and go. The points are joined by dim lines and
// lie in the palette's colors around (the hue circle without one). The trail fades and blurs.
// Reactive: faster and brighter with the sound, a beat moves the colors on.
void drift_rose(Canvas &canvas, const Params &params, uint32_t ms)
{
    Frame f;
    if (!begin(canvas, params, ms, f)) {
        return;
    }
    World &world = f.world;
    Scene &s = *world.scene;
    const uint8_t hue = beat_hue(*world.m, f.sound, f.dt, 32u * params.strength / 128u);
    const int32_t cx = (world.w - 1) * 128;
    const int32_t cy = (world.h - 1) * 128;
    const int32_t reach = (shorter_side(world) - 1) * 128;
    constexpr uint32_t kPoints = 36;
    for (uint32_t step = 0; step < f.steps; step++) {
        const uint32_t t = ++s.steps;
        fade(world, 224);
        blur(world, 32);
        int32_t px = 0;
        int32_t py = 0;
        Rgb pc{};
        // once around and back to the first point: the ring is closed
        for (uint32_t i = 0; i <= kPoints; i++) {
            const uint32_t k = i % kPoints;
            const uint32_t angle = k * 65536u / kPoints;
            const int32_t r = times(sin16(t * (k + 1u) * 18u), reach);
            const int32_t x = cx + times(sin16(angle), r);
            const int32_t y = cy + times(cos16(angle), r);
            const Rgb c = paint(params, static_cast<uint8_t>(k * 256u / kPoints + (t >> 3) + hue), 255, 255);
            if (i > 0) {
                line(world, px, py, x, y, scale(pc, 72), scale(c, 72));
            }
            if (i < kPoints) {
                splat(world, x, y, c);
            }
            px = x;
            py = y;
            pc = c;
        }
    }
    show(canvas, world, f.sound.value);
}

// Fireworks (after WLED's): blobs of color lighting up at random places and fading again, each in a
// color of the palette at random (the hue circle without one): a light flashes, the blur spreads it
// out as it fades within about a second; one per 24 pixels a second. Reactive: as many as it is
// loud (an eighth in silence, twice at full), a burst on the beat, brighter.
void fireworks(Canvas &canvas, const Params &params, uint32_t ms)
{
    Frame f;
    if (!begin(canvas, params, ms, f)) {
        return;
    }
    World &world = f.world;
    Scene &s = *world.scene;
    Memory &m = *world.m;
    const uint32_t pixels = static_cast<uint32_t>(world.w) * world.h;
    // the blobs a step, x 65536 (62.5 steps a second)
    uint32_t births = pixels * 65536u / 1500u;
    births = f.sound.on ? births / 128u * (16u + f.sound.level) : births;
    const int32_t xmax = (world.w - 1) * 256;
    const int32_t ymax = (world.h - 1) * 256;
    for (uint32_t step = 0; step < f.steps; step++) {
        s.steps++;
        fade(world, 244);
        blur(world, 40);
        s.timer += births;
        uint32_t born = s.timer >> 16;
        s.timer &= 0xffffu;
        born += f.sound.beat && step == 0 ? 3u + pixels / 96u : 0u;
        for (uint32_t k = 0; k < born; k++) {
            // on a pixel, so a small one is sharp
            const int32_t x = random_upto(m, xmax) & ~255;
            const int32_t y = random_upto(m, ymax) & ~255;
            disc(world, x, y, 448 + random_below(m, 256), paint(params, random8(m), 255, 255), false);
        }
    }
    show(canvas, world, f.sound.value);
}

// Fireworks 1D (after WLED's exploding fireworks): a rocket shot up from the bottom at a random
// place, slowing as it climbs, bursts at the top of its way (55 to 90 % of the height) into sparks
// flying out every way and falling, white hot at first, then in the burst's color of the palette
// (the hue circle without one), dimming out within a second and a half; the next rocket 0.3 to 1.4 s
// after. Made for a matrix; a strip turned by a quarter fires along its length. Reactive: louder is
// more sparks, a beat fires a rocket at once, brighter.
void fireworks_1d(Canvas &canvas, const Params &params, uint32_t ms)
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
    // to the top of a tall canvas in about 0.6 s
    const int32_t g = world.h * 256 / 1000;
    const int32_t gravity = g > 1 ? g : 1;
    const int32_t burst = shorter_side(world) * 256 / 20 > 96 ? shorter_side(world) * 256 / 20 : 96;
    // the rocket is the first particle (life 0 while it waits, s.timer the steps until it goes), the
    // sparks the others
    Particle &rocket = world.particles[0];
    World sparks = world;
    sparks.particles = world.particles + 1;
    sparks.slots = world.slots - 1;
    for (uint32_t step = 0; step < f.steps; step++) {
        s.steps++;
        fade(world, 200);
        blur(world, 24);
        const bool beat = f.sound.beat && step == 0;
        if (rocket.life == 0 && (s.timer == 0 || beat)) {
            const int32_t height = ymax * (140 + random_below(m, 90)) / 256;
            const auto v = static_cast<int32_t>(isqrt(static_cast<uint32_t>(2 * gravity * height)));
            rocket = {random_upto(m, xmax), ymax, i16(random_signed(m, 12)), i16(-v), 1, random8(m), 0};
        } else if (rocket.life == 0) {
            s.timer--;
        }
        if (rocket.life != 0) {
            rocket.vy = i16(rocket.vy + gravity);
            rocket.x += rocket.vx;
            rocket.y += rocket.vy;
            splat(world, rocket.x, rocket.y, {255, 190, 110});
            if (rocket.vy >= 0 || rocket.y <= 0) {
                rocket.life = 0;
                s.timer = 20u + random_below(m, 70);
                const uint32_t many = 24u + (f.sound.on ? f.sound.level / 8u : 8u);
                for (uint32_t k = 0; k < many; k++) {
                    Particle *p = spawn(sparks);
                    if (p == nullptr) {
                        break;
                    }
                    const uint32_t angle = random16(m);
                    const int32_t v = burst * (96 + random_below(m, 160)) / 256;
                    const auto life = static_cast<uint8_t>(50 + random_below(m, 40));
                    *p = {rocket.x, rocket.y, i16(cos16(angle) * v >> 15), i16(sin16(angle) * v >> 15), life,
                          static_cast<uint8_t>(rocket.hue + random_below(m, 24)), life};
                }
            }
        }
        for (size_t i = 0; i < sparks.slots; i++) {
            Particle &p = sparks.particles[i];
            if (p.life == 0) {
                continue;
            }
            p.vx = i16(p.vx - p.vx / 20);
            p.vy = i16(p.vy - p.vy / 20 + (gravity + 1) / 2);
            p.x += p.vx;
            p.y += p.vy;
            p.life = off_canvas(world, p) ? 0 : static_cast<uint16_t>(p.life - 1);
            if (p.life == 0) {
                continue;
            }
            // tag: the life it began with
            const uint32_t age = p.tag - p.life;
            Rgb c = paint(params, p.hue, 255, 255);
            c = age < 8 ? mix(c, {255, 255, 255}, (8u - age) * 32u) : c;
            splat(world, p.x, p.y, scale(c, p.life * 255u / p.tag));
        }
    }
    show(canvas, world, f.sound.value);
}

// Ghost rider (after WLED's): a light riding over the canvas on a way that winds like a kite in a
// tornado: its heading turns now one way, now the other, faster and slower, into loops; out at an
// edge, it comes back in at the other. It sheds ghosts that drift off to both sides of its way and
// fade within a second; its color goes around the palette (the hue circle without one), each ghost
// changing on as it ages. The trail fades and blurs. Unlike PS ghost rider, which bounces off the
// edges and sheds its particles behind. Reactive: faster and brighter with the sound, a beat throws
// it a quarter turn.
void ghost_rider(Canvas &canvas, const Params &params, uint32_t ms)
{
    Frame f;
    if (!begin(canvas, params, ms, f)) {
        return;
    }
    World &world = f.world;
    Scene &s = *world.scene;
    Memory &m = *world.m;
    const int32_t wide = world.w * 256;
    const int32_t high = world.h * 256;
    if (s.steps == 0) {
        s.x = wide / 2;
        s.y = high / 2;
        s.angle = random16(m);
    }
    // across the canvas in about a second
    const int32_t fast = longer_side(world) * 256 / 60;
    const int32_t speed = fast > 48 ? fast : 48;
    constexpr uint16_t kLife = 64;
    for (uint32_t step = 0; step < f.steps; step++) {
        const uint32_t t = ++s.steps;
        fade(world, 216);
        blur(world, 64);
        const int32_t turn = (sin16(t * 40u) * 640 >> 15) + (sin16(t * 13u + 9000u) * 320 >> 15);
        s.angle += static_cast<uint32_t>(turn);
        s.angle += f.sound.beat && step == 0 ? 16384u : 0u;
        s.x = ((s.x + (cos16(s.angle) * speed >> 15)) % wide + wide) % wide;
        s.y = ((s.y + (sin16(s.angle) * speed >> 15)) % high + high) % high;
        const auto hue = static_cast<uint8_t>(t >> 1);
        for (uint32_t side = 0; side < 2; side++) {
            Particle *p = spawn(world);
            if (p == nullptr) {
                break;
            }
            const uint32_t angle = s.angle + (side == 0 ? 16384u : 49152u) + static_cast<uint32_t>(random_signed(m, 3000));
            const int32_t v = speed * (48 + random_below(m, 48)) / 256;
            *p = {s.x, s.y, i16(cos16(angle) * v >> 15), i16(sin16(angle) * v >> 15), kLife, hue, 0};
        }
        for (size_t i = 0; i < world.slots; i++) {
            Particle &p = world.particles[i];
            if (p.life == 0) {
                continue;
            }
            p.vx = i16(p.vx - p.vx / 32);
            p.vy = i16(p.vy - p.vy / 32);
            p.x += p.vx;
            p.y += p.vy;
            p.life = off_canvas(world, p) ? 0 : static_cast<uint16_t>(p.life - 1);
            if (p.life != 0) {
                splat(world, p.x, p.y,
                      paint(params, static_cast<uint8_t>(p.hue + (kLife - p.life)), 255, static_cast<uint8_t>(p.life * 255u / kLife)));
            }
        }
        splat(world, s.x, s.y, mix(paint(params, hue, 255, 255), {255, 255, 255}, 160));
    }
    show(canvas, world, f.sound.value);
}

// Lissajous (after WLED's): a Lissajous figure over the whole canvas, its x going three times and
// its y twice around while it is drawn once (96 points joined by lines); the phase between the two
// drifts (a turn in 9 s), so the figure seems to turn in space. In the palette's colors along it
// (the hue circle without one), moving on. The trail fades. Reactive: faster and brighter with the
// sound, a beat moves the colors on.
void lissajous(Canvas &canvas, const Params &params, uint32_t ms)
{
    Frame f;
    if (!begin(canvas, params, ms, f)) {
        return;
    }
    World &world = f.world;
    Scene &s = *world.scene;
    const uint8_t hue = beat_hue(*world.m, f.sound, f.dt, 32u * params.strength / 128u);
    constexpr uint32_t kPoints = 96;
    for (uint32_t step = 0; step < f.steps; step++) {
        const uint32_t t = ++s.steps;
        fade(world, 176);
        const uint32_t phase = t * 116u;
        int32_t px = 0;
        int32_t py = 0;
        Rgb pc{};
        for (uint32_t i = 0; i <= kPoints; i++) {
            const uint32_t u = i * 65536u / kPoints;
            const int32_t x = across(world.w, sin16(u * 3u + phase));
            const int32_t y = across(world.h, cos16(u * 2u));
            const Rgb c = paint(params, static_cast<uint8_t>(i * 256u / kPoints + (t >> 2) + hue), 255, 255);
            if (i > 0) {
                line(world, px, py, x, y, pc, c);
            }
            px = x;
            py = y;
            pc = c;
        }
    }
    show(canvas, world, f.sound.value);
}

// Sindots (after WLED's): 13 dots going round a circle that fills the canvas (once in 2.5 s), a
// twentieth of a turn apart, while the view swings, so the circle turns into ellipses and a line
// and back; each dot in its color of the palette (the hue circle without one), moving on. The trail
// fades and blurs. Reactive: faster and brighter with the sound, a beat moves the colors on.
void sindots(Canvas &canvas, const Params &params, uint32_t ms)
{
    Frame f;
    if (!begin(canvas, params, ms, f)) {
        return;
    }
    World &world = f.world;
    Scene &s = *world.scene;
    const uint8_t hue = beat_hue(*world.m, f.sound, f.dt, 32u * params.strength / 128u);
    constexpr uint32_t kDots = 13;
    for (uint32_t step = 0; step < f.steps; step++) {
        const uint32_t t = ++s.steps;
        fade(world, 224);
        blur(world, 40);
        const uint32_t round = t * 420u;
        // up to a quarter turn between x and y: from a circle to a line
        const auto view = static_cast<uint32_t>(sin16(t * 61u) / 2);
        for (uint32_t k = 0; k < kDots; k++) {
            const uint32_t a = round + k * 65536u / 20u;
            splat(world, across(world.w, sin16(a)), across(world.h, cos16(a + view)),
                  paint(params, static_cast<uint8_t>(k * 256u / kDots + (t >> 3) + hue), 255, 255));
        }
    }
    show(canvas, world, f.sound.value);
}

// Scrolling text (after WLED's): Params::text (the overlay's text, or the time and the date)
// running from right to left through the middle of the canvas in the font of the overlay, again
// after a gap of a quarter of the width; in the color (the palette's colors across the canvas with
// one, moving on). About 24 pixels a second at speed 128; the trail smears it a little. Without a
// text "PixeLED". Reactive: faster and brighter with the sound.
void scrolling_text(Canvas &canvas, const Params &params, uint32_t ms)
{
    Frame f;
    if (!begin(canvas, params, ms, f)) {
        return;
    }
    World &world = f.world;
    Scene &s = *world.scene;
    const char *text = params.text != nullptr && params.text[0] != 0 ? params.text : "PixeLED";
    const int32_t width = text_width(text);
    // x: the way the text went, in 1/256 pixel
    const int32_t loop = (world.w + width + world.w / 4 + 1) * 256;
    const int top = (static_cast<int>(world.h) - kTextHeight) / 2;
    const uint16_t first = static_cast<uint16_t>(top < 0 ? 0 : top);
    const uint16_t last = static_cast<uint16_t>(top + kTextHeight > world.h ? world.h : top + kTextHeight);
    // the text in white on the canvas (drawn over by show) gives each pixel its share
    Canvas letters{world.w, world.h, canvas.pixels};
    for (uint32_t step = 0; step < f.steps; step++) {
        const uint32_t t = ++s.steps;
        fade(world, 128);
        s.x = (s.x + 100) % loop;
        std::memset(&letters.at(0, first), 0, static_cast<size_t>(last - first) * world.w * sizeof(Rgb));
        draw_text(letters, text, world.w - (s.x >> 8), top, {255, 255, 255});
        for (uint16_t y = first; y < last; y++) {
            for (uint16_t x = 0; x < world.w; x++) {
                const uint8_t v = letters.at(x, y).r;
                if (v == 0) {
                    continue;
                }
                const Rgb c = params.palette == 0
                                  ? params.color
                                  : palette_color(params.palette, static_cast<uint8_t>(x * 256u / world.w + (t >> 2)), params.color);
                plot(world, x, y, c, v + 1u);
            }
        }
    }
    show(canvas, world, f.sound.value);
}

} // namespace px::effects::detail
