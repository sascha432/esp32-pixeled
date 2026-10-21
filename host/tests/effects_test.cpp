// components/effects/src/effects.cpp: the helpers and that every animation fills its canvas.

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
