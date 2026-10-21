// components/effects/src/effects.cpp: the helpers and that every animation fills its canvas.

#include <cstdio>
#include <vector>

#include "check.hpp"
#include "px/effects/effects.hpp"

using namespace px::effects;

namespace {

bool same(Rgb a, Rgb b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

} // namespace

TEST(effects_sin8_has_the_shape_of_a_sine)
{
    CHECK(sin8(0) == 128);
    CHECK(sin8(64) == 255);
    CHECK(sin8(128) == 128);
    CHECK(sin8(192) == 1);
    CHECK(sin8(32) > 210 && sin8(32) < 222); // 128 + 127 * sin(45°) = 217.8
}

TEST(effects_hsv_primaries)
{
    CHECK(same(hsv(0, 255, 255), {255, 0, 0}));
    CHECK(hsv(85, 255, 255).g > 250 && hsv(85, 255, 255).r < 10);
    CHECK(hsv(170, 255, 255).b > 250 && hsv(170, 255, 255).g < 10);
    CHECK(same(hsv(123, 0, 200), {200, 200, 200})); // no saturation: grey
    CHECK(same(hsv(77, 255, 0), {0, 0, 0}));
}

TEST(effects_list_and_find)
{
    REQUIRE(count() >= 5);
    CHECK(find("solid") == 0);
    CHECK(find("rainbow") < count());
    CHECK(find("nothing") == count());
    CHECK(info(count()) == nullptr);
    for (size_t i = 0; i < count(); i++) {
        CHECK(find(info(i)->slug) == i);
    }
}

TEST(effects_solid_and_edges)
{
    std::vector<Rgb> pixels(6 * 4);
    Canvas canvas{6, 4, pixels.data()};
    Params params;
    params.color = {1, 2, 3};
    render(find("solid"), canvas, params, 0);
    for (const Rgb &p : pixels) {
        CHECK(same(p, {1, 2, 3}));
    }
    render(find("edges"), canvas, params, 0);
    CHECK(same(canvas.at(0, 0), {255, 255, 255}));
    CHECK(same(canvas.at(3, 0), {255, 0, 0}));
    CHECK(same(canvas.at(5, 2), {0, 255, 0}));
    CHECK(same(canvas.at(2, 3), {0, 0, 255}));
    CHECK(same(canvas.at(0, 2), {255, 255, 0}));
    CHECK(same(canvas.at(2, 2), {0, 0, 0}));
}

TEST(effects_moving_animations_change_with_time_and_stand_at_speed_0)
{
    std::vector<Rgb> a(16 * 8), b(16 * 8);
    Canvas ca{16, 8, a.data()}, cb{16, 8, b.data()};
    Params params;
    for (const char *slug : {"rainbow", "plasma", "breathe"}) {
        params.speed = 128;
        render(find(slug), ca, params, 1000);
        render(find(slug), cb, params, 1400);
        bool changed = false;
        for (size_t i = 0; i < a.size(); i++) {
            changed |= !same(a[i], b[i]);
        }
        CHECK(changed);
        params.speed = 0;
        render(find(slug), ca, params, 1000);
        render(find(slug), cb, params, 90000);
        bool still = true;
        for (size_t i = 0; i < a.size(); i++) {
            still &= same(a[i], b[i]);
        }
        CHECK(still);
    }
}

TEST(effects_spectrum_draws_a_bar_per_band)
{
    std::vector<Rgb> pixels(32 * 8);
    Canvas canvas{32, 8, pixels.data()};
    Audio audio;
    audio.active = true;
    audio.bands[0] = 255; // the bass: full height
    audio.bands[31] = 128; // the treble: half
    Params params;
    params.audio = &audio;
    render(find("spectrum"), canvas, params, 0);
    const auto lit = [&](uint16_t x, uint16_t y) { return !same(canvas.at(x, y), {0, 0, 0}); };
    CHECK(lit(0, 0) && lit(0, 7));         // column 0 from top to bottom
    CHECK(!lit(31, 3) && lit(31, 4));      // column 31: the lower half
    CHECK(!lit(15, 7));                    // a silent band stays dark
    // without audio: only the line at the bottom
    params.audio = nullptr;
    render(find("spectrum"), canvas, params, 0);
    CHECK(lit(15, 7) && !lit(15, 6));
}

namespace {

// a canvas with the memory and the spare canvas the render task gives
struct Owned {
    std::vector<Rgb> pixels;
    std::vector<Rgb> spare;
    std::vector<uint8_t> memory;
    Canvas canvas;

    Owned(uint16_t w, uint16_t h)
        : pixels(static_cast<size_t>(w) * h), spare(pixels.size()), memory(memory_size(w, h)),
          canvas{w, h, pixels.data(), memory.data(), memory.size(), spare.data()}
    {
    }
};

uint32_t brightness_sum(const Canvas &canvas)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < static_cast<size_t>(canvas.width) * canvas.height; i++) {
        sum += canvas.pixels[i].r + canvas.pixels[i].g + canvas.pixels[i].b;
    }
    return sum;
}

} // namespace

TEST(effects_rotation_turns_the_picture)
{
    // edges on 6 x 4: the white top left pixel; red top row, yellow left column
    Owned o(6, 4);
    Params params;
    params.rotation = 1; // 90 degrees clockwise: drawn on 4 x 6, its top left lands top right
    render(find("edges"), o.canvas, params, 0);
    CHECK(same(o.canvas.at(5, 0), {255, 255, 255}));
    CHECK(same(o.canvas.at(5, 2), {255, 0, 0}));    // its top row is now the right column
    CHECK(same(o.canvas.at(2, 0), {255, 255, 0}));  // its left column is now the top row
    params.rotation = 2;
    render(find("edges"), o.canvas, params, 0);
    CHECK(same(o.canvas.at(5, 3), {255, 255, 255}));
    CHECK(same(o.canvas.at(2, 3), {255, 0, 0}));
    params.rotation = 3;
    render(find("edges"), o.canvas, params, 0);
    CHECK(same(o.canvas.at(0, 3), {255, 255, 255}));
    CHECK(same(o.canvas.at(0, 1), {255, 0, 0}));    // its top row is now the left column
    // without a spare canvas a quarter turn is not done
    Canvas plain{6, 4, o.pixels.data()};
    params.rotation = 1;
    render(find("edges"), plain, params, 0);
    CHECK(same(plain.at(0, 0), {255, 255, 255}));
}

TEST(effects_fire_burns_from_the_bottom)
{
    Owned o(16, 16);
    Params params;
    params.speed = 128;
    for (uint32_t ms = 0; ms <= 3000; ms += 10) {
        render(find("fire"), o.canvas, params, ms);
    }
    uint32_t bottom = 0, top = 0;
    for (uint16_t x = 0; x < 16; x++) {
        for (uint16_t y = 0; y < 3; y++) {
            top += o.canvas.at(x, y).r;
            bottom += o.canvas.at(x, static_cast<uint16_t>(15 - y)).r;
        }
    }
    CHECK(bottom > 3000);
    CHECK(bottom > top * 3);
    // without memory it stays dark (and does not crash)
    Canvas plain{16, 16, o.pixels.data()};
    render(find("fire"), plain, params, 100);
    CHECK(brightness_sum(plain) == 0);
}

TEST(effects_reactive_follows_the_sound)
{
    Audio loud;
    loud.active = true;
    loud.loudness = 255;
    Audio quiet;
    quiet.active = true;
    quiet.loudness = 0;
    for (const char *slug : {"rainbow", "plasma", "breathe"}) {
        Owned a(8, 8), b(8, 8);
        Params params;
        params.speed = 0;
        params.color = {200, 200, 200};
        params.reactive = true;
        for (uint32_t ms = 0; ms <= 500; ms += 10) {
            params.audio = &loud;
            render(find(slug), a.canvas, params, ms);
            params.audio = &quiet;
            render(find(slug), b.canvas, params, ms);
        }
        CHECK(brightness_sum(a.canvas) > brightness_sum(b.canvas) * 3);
        // the switch off: the sound changes nothing
        Owned c(8, 8), d(8, 8);
        params.reactive = false;
        params.audio = &loud;
        render(find(slug), c.canvas, params, 500);
        params.audio = &quiet;
        render(find(slug), d.canvas, params, 500);
        CHECK(brightness_sum(c.canvas) == brightness_sum(d.canvas));
    }
}

TEST(effects_reactive_fire_sparks_with_its_bands)
{
    // the bass loud, the treble silent: the left half burns, the right stays dark
    Audio audio;
    audio.active = true;
    audio.loudness = 200;
    for (size_t b = 0; b < kBands / 2; b++) {
        audio.bands[b] = 255;
    }
    Owned o(16, 16);
    Params params;
    params.speed = 128;
    params.reactive = true;
    params.audio = &audio;
    for (uint32_t ms = 0; ms <= 2000; ms += 10) {
        render(find("fire"), o.canvas, params, ms);
    }
    uint32_t left = 0, right = 0;
    for (uint16_t y = 12; y < 16; y++) {
        for (uint16_t x = 0; x < 6; x++) {
            left += o.canvas.at(x, y).r;
            right += o.canvas.at(static_cast<uint16_t>(15 - x), y).r;
        }
    }
    CHECK(left > 2000);
    CHECK(left > right * 4);
}

TEST(effects_strength_scales_the_reaction)
{
    Audio half;
    half.active = true;
    half.loudness = 100;
    uint32_t sums[3] = {};
    const uint8_t strengths[3] = {0, 128, 255};
    for (size_t i = 0; i < 3; i++) {
        Owned o(8, 8);
        Params params;
        params.color = {200, 200, 200};
        params.reactive = true;
        params.audio = &half;
        params.strength = strengths[i];
        for (uint32_t ms = 0; ms <= 500; ms += 10) {
            render(find("breathe"), o.canvas, params, ms);
        }
        sums[i] = brightness_sum(o.canvas);
    }
    CHECK(sums[2] > sums[1] * 3 / 2);
    // strength 0 is the switch off: breathe breathes (speed 128 at 500 ms), not dark from the sound
    Owned off(8, 8);
    Params params;
    params.color = {200, 200, 200};
    params.audio = &half;
    render(find("breathe"), off.canvas, params, 500);
    CHECK(sums[0] == brightness_sum(off.canvas));
}

namespace {

constexpr const char *kNewAnimations[] = {"gradient", "color-fade", "twinkle", "meteor", "ripple", "matrix", "noise"};

size_t lit_count(const Canvas &canvas)
{
    size_t lit = 0;
    for (size_t i = 0; i < static_cast<size_t>(canvas.width) * canvas.height; i++) {
        lit += canvas.pixels[i].r + canvas.pixels[i].g + canvas.pixels[i].b > 30 ? 1 : 0;
    }
    return lit;
}

} // namespace

TEST(effects_new_animations_draw_and_move)
{
    for (const char *slug : kNewAnimations) {
        REQUIRE(find(slug) < count());
        CHECK(info(find(slug))->reactive == Reactive::Optional);
        Owned o(16, 16);
        Params params;
        params.color = {0, 200, 60};
        std::vector<Rgb> before;
        uint32_t light = 0;
        for (uint32_t ms = 0; ms <= 3000; ms += 20) {
            render(find(slug), o.canvas, params, ms);
            if (ms == 2000) {
                before = o.pixels;
            }
            light += ms >= 2000 ? brightness_sum(o.canvas) : 0;
        }
        if (light == 0) {
            std::printf("%s stays dark\n", slug);
        }
        CHECK(light > 0);
        bool moved = false;
        for (size_t i = 0; i < before.size(); i++) {
            moved |= !same(before[i], o.pixels[i]);
        }
        CHECK(moved);
        // a strip and a single LED, and without memory: no crash
        Owned strip(40, 1), one(1, 1);
        Canvas plain{16, 16, o.pixels.data()};
        for (uint32_t ms = 0; ms <= 500; ms += 20) {
            render(find(slug), strip.canvas, params, ms);
            render(find(slug), one.canvas, params, ms);
            render(find(slug), plain, params, ms);
        }
    }
}

TEST(effects_new_animations_follow_the_sound)
{
    Audio loud;
    loud.active = true;
    loud.loudness = 255;
    for (uint8_t &b : loud.bands) {
        b = 255;
    }
    Audio quiet;
    quiet.active = true;
    for (const char *slug : {"gradient", "color-fade", "twinkle", "meteor", "matrix", "noise"}) {
        Owned a(16, 16), b(16, 16);
        Params params;
        params.color = {200, 200, 200};
        params.reactive = true;
        uint32_t sum_a = 0, sum_b = 0;
        for (uint32_t ms = 0; ms <= 3000; ms += 20) {
            params.audio = &loud;
            render(find(slug), a.canvas, params, ms);
            params.audio = &quiet;
            render(find(slug), b.canvas, params, ms);
            if (ms >= 1000) {
                sum_a += brightness_sum(a.canvas);
                sum_b += brightness_sum(b.canvas);
            }
        }
        CHECK(sum_a > sum_b * 3 / 2);
    }
}

TEST(effects_ripple_starts_a_ring_on_every_beat)
{
    Audio beat;
    beat.active = true;
    beat.loudness = 200;
    Audio steady = beat;
    size_t with = 0, without = 0;
    Owned a(24, 24), b(24, 24);
    Params params;
    params.reactive = true;
    params.speed = 128;
    for (uint32_t ms = 0; ms <= 2000; ms += 20) {
        beat.beat = ms % 200 == 0;
        params.audio = &beat;
        render(find("ripple"), a.canvas, params, ms);
        params.audio = &steady;
        render(find("ripple"), b.canvas, params, ms);
        with += lit_count(a.canvas);
        without += lit_count(b.canvas);
    }
    CHECK(with > without * 3);
}

TEST(effects_matrix_drops_with_the_bands)
{
    Audio audio;
    audio.active = true;
    audio.loudness = 200;
    for (size_t b = 0; b < kBands / 2; b++) {
        audio.bands[b] = 255;
    }
    Owned o(16, 16);
    Params params;
    params.reactive = true;
    params.audio = &audio;
    params.color = {0, 255, 0};
    uint32_t left = 0, right = 0;
    for (uint32_t ms = 0; ms <= 3000; ms += 20) {
        render(find("matrix"), o.canvas, params, ms);
        for (uint16_t y = 0; y < 16; y++) {
            for (uint16_t x = 0; x < 6; x++) {
                left += o.canvas.at(x, y).g;
                right += o.canvas.at(static_cast<uint16_t>(15 - x), y).g;
            }
        }
    }
    CHECK(left > 10000);
    CHECK(left > right * 4);
}

TEST(effects_twinkle_lights_about_one_in_seven)
{
    Owned o(32, 32);
    Params params;
    params.color = {255, 255, 255};
    size_t lit = 0, frames = 0;
    for (uint32_t ms = 0; ms <= 10000; ms += 20) {
        render(find("twinkle"), o.canvas, params, ms);
        if (ms >= 4000) {
            lit += lit_count(o.canvas);
            frames++;
        }
    }
    const size_t average = lit / frames;
    CHECK(average > 1024 / 20 && average < 1024 / 4);
}

TEST(effects_attack_and_release)
{
    Audio loud;
    loud.active = true;
    loud.loudness = 255;
    Audio quiet;
    quiet.active = true;
    // the release: after the sound stopped, a long release stays bright longer
    uint32_t sums[2] = {};
    const uint16_t releases[2] = {50, 3000};
    for (size_t i = 0; i < 2; i++) {
        Owned o(4, 4);
        Params params;
        params.color = {255, 255, 255};
        params.reactive = true;
        params.release_ms = releases[i];
        for (uint32_t ms = 0; ms <= 1000; ms += 10) {
            params.audio = ms < 600 ? &loud : &quiet;
            render(find("breathe"), o.canvas, params, ms);
        }
        sums[i] = brightness_sum(o.canvas);
    }
    CHECK(sums[1] > sums[0] * 3);
    // the attack: 100 ms into the sound a slow attack is still dark
    uint32_t rise[2] = {};
    const uint16_t attacks[2] = {0, 2000};
    for (size_t i = 0; i < 2; i++) {
        Owned o(4, 4);
        Params params;
        params.color = {255, 255, 255};
        params.reactive = true;
        params.attack_ms = attacks[i];
        params.audio = &loud;
        for (uint32_t ms = 0; ms <= 100; ms += 10) {
            render(find("breathe"), o.canvas, params, ms);
        }
        rise[i] = brightness_sum(o.canvas);
    }
    CHECK(rise[0] > rise[1] * 3);
}

TEST(effects_spectrum_bars_fall_with_the_release)
{
    Audio audio;
    audio.active = true;
    audio.bands[0] = 255;
    Audio silent;
    silent.active = true;
    const auto height = [](Owned &o) {
        uint16_t rows = 0;
        for (uint16_t y = 0; y < 8; y++) {
            rows += same(o.canvas.at(0, y), {0, 0, 0}) ? 0 : 1;
        }
        return rows;
    };
    Owned fast(32, 8), slow(32, 8);
    Params params;
    params.attack_ms = 0;
    for (uint32_t ms = 0; ms <= 300; ms += 10) {
        params.audio = ms < 100 ? &audio : &silent;
        params.release_ms = 0;
        render(find("spectrum"), fast.canvas, params, ms);
        params.release_ms = 2000;
        render(find("spectrum"), slow.canvas, params, ms);
    }
    CHECK(height(slow) >= 6);
    CHECK(height(fast) <= 1);
}

TEST(effects_blend_mixes_two_canvases)
{
    Rgb into[2] = {{0, 0, 0}, {200, 100, 50}};
    const Rgb from[2] = {{255, 255, 255}, {0, 0, 0}};
    blend(into, from, 2, 0);
    CHECK(same(into[0], {0, 0, 0}) && same(into[1], {200, 100, 50}));
    blend(into, from, 2, 128);
    CHECK(into[0].r == 128 && into[1].r == 99);
    blend(into, from, 2, 255);
    CHECK(same(into[0], {255, 255, 255}) && same(into[1], {0, 0, 0}));
}

TEST(effects_noise_is_smooth_and_spread)
{
    uint8_t low = 255, high = 0;
    int largest_step = 0;
    for (uint32_t y = 0; y < 64; y++) {
        for (uint32_t x = 0; x < 64 * 32; x += 32) {
            const uint8_t a = noise8(x, y * 32, 1000);
            const uint8_t b = noise8(x + 32, y * 32, 1000);
            low = a < low ? a : low;
            high = a > high ? a : high;
            const int step = a > b ? a - b : b - a;
            largest_step = step > largest_step ? step : largest_step;
        }
    }
    CHECK(high - low > 150);
    CHECK(largest_step < 80); // neighbors an eighth of a cell apart
}
