// components/leds/src/frame.cpp: brightness, gamma, the table and the power limit.

#include <vector>

#include "check.hpp"
#include "frame.hpp"

using namespace px::leds::detail;
using px::effects::Rgb;

namespace {

const LaneMix kDither{};
const LaneMix kPlain{.dither = false};

// a table of one lane: every pixel on lane 0
px::layout::Table one_lane(std::initializer_list<uint16_t> leds, uint16_t width)
{
    px::layout::Table table;
    table.width = width;
    table.height = 1;
    table.led = leds;
    table.lane.assign(table.led.size(), 0);
    table.lanes = 1;
    table.leds = table.led.size();
    return table;
}

} // namespace

TEST(frame_gamma_in_16_bits)
{
    const Gamma gamma;
    CHECK(gamma.table[0] == 0);
    CHECK(gamma.table[255] == 65535);
    CHECK(gamma.table[2] > 0);                                   // steps below the output's 8 bits
    CHECK(gamma.table[128] / 256 > 50 && gamma.table[128] / 256 < 60); // half looks like a fifth: 55
}

TEST(frame_brightness_curve_starts_at_one_output_step)
{
    const Gamma gamma;
    CHECK(gamma.brightness[0] == 0);
    CHECK(gamma.brightness[255] == 65535);
    CHECK(gamma.brightness[1] == 257);                              // one step of 255
    CHECK(gamma.brightness[16] > 257 && gamma.brightness[16] < 2 * 257); // still below two steps
    for (int i = 1; i < 256; i++) {
        CHECK(gamma.brightness[i] > gamma.brightness[i - 1]);       // every slider step is brighter
    }
}

TEST(frame_dithering_averages_to_the_16_bit_value)
{
    const Gamma gamma;
    const px::layout::Table table = one_lane({0}, 1);
    // a dark red between two output steps
    const Rgb canvas[1] = {{60, 0, 0}};
    const double exact = (gamma.table[60] * static_cast<uint32_t>(gamma.brightness[100]) >> 16) / 256.0;
    CHECK(exact > 1.0);
    uint32_t sum = 0;
    uint8_t frame[3];
    for (uint32_t n = 0; n < 256; n++) {
        compose(canvas, table, 100, gamma, &kDither, n, frame);
        sum += frame[0];
        CHECK(frame[0] == static_cast<uint8_t>(exact) || frame[0] == static_cast<uint8_t>(exact) + 1);
    }
    CHECK(sum / 256.0 > exact - 0.05 && sum / 256.0 < exact + 0.05);
    // without it: one value, rounded
    compose(canvas, table, 100, gamma, &kPlain, 0, frame);
    CHECK(frame[0] == static_cast<uint8_t>(exact + 0.5));
}

TEST(frame_weak_channels_keep_the_hue_on_average)
{
    const Gamma gamma;
    const px::layout::Table table = one_lane({0}, 1);
    // a dark pink (250, 0, 60 at brightness 8): the blue is a fraction of one output step
    const Rgb canvas[1] = {{250, 0, 60}};
    const uint32_t scale = gamma.brightness[8];
    const double red = (gamma.table[250] * scale >> 16) / 256.0;
    const double blue = (gamma.table[60] * scale >> 16) / 256.0;
    CHECK(blue > 0.0 && blue < 1.0);
    uint32_t sum_red = 0, sum_blue = 0;
    uint8_t frame[3];
    for (uint32_t n = 0; n < 256; n++) {
        compose(canvas, table, 8, gamma, &kDither, n, frame);
        sum_red += frame[0];
        sum_blue += frame[2];
    }
    // dithered: the blue is there on average, so the color stays pink
    CHECK(sum_blue / 256.0 > blue - 0.05 && sum_blue / 256.0 < blue + 0.05);
    CHECK(sum_red / 256.0 > red - 0.05 && sum_red / 256.0 < red + 0.05);
    // a lane without dithering: rounded, the same in every frame
    compose(canvas, table, 8, gamma, &kPlain, 0, frame);
    const uint8_t plain_blue = frame[2];
    for (uint32_t n = 1; n < 16; n++) {
        compose(canvas, table, 8, gamma, &kPlain, n, frame);
        CHECK(frame[2] == plain_blue);
    }
}

TEST(frame_dither_offsets_spread_over_frames_and_leds)
{
    uint32_t sum = 0;
    for (uint32_t n = 0; n < 256; n++) {
        sum += dither_offset(n, 0);
    }
    CHECK(sum == 255 * 128); // every threshold once
    CHECK(dither_offset(0, 0) != dither_offset(0, 1));
}

TEST(frame_compose_maps_scales_and_skips_pixels_without_an_led)
{
    const Gamma gamma;
    px::layout::Table table = one_lane({2, px::layout::kNone, 0}, 3);
    table.leds = 3;
    const Rgb canvas[3] = {{255, 0, 0}, {9, 9, 9}, {0, 0, 255}};
    std::vector<uint8_t> frame(9, 7);
    compose(canvas, table, 255, gamma, &kPlain, 0, frame.data());
    CHECK(frame[6] == 255 && frame[7] == 0 && frame[8] == 0); // canvas 0 -> LED 2
    CHECK(frame[0] == 0 && frame[2] == 255);                 // canvas 2 -> LED 0
    CHECK(frame[3] == 7);                                    // LED 1 has no pixel: untouched
    compose(canvas, table, 0, gamma, &kPlain, 0, frame.data());
    CHECK(frame[6] == 0 && frame[2] == 0);
}

TEST(frame_power_limit_scales_down_to_the_limit)
{
    const PowerModel model{80, 5};
    std::vector<uint8_t> frame(100 * 3, 255);
    // 100 LEDs at full white: 500 mW idle + 100 * 3 * 80 mW
    CHECK(estimate_mw(frame.data(), 100, model) == 24500);
    bool limited = false;
    const uint32_t after = limit_power(frame.data(), 100, model, 10000, limited);
    CHECK(limited);
    CHECK(after <= 10000 && after > 9900);
    std::vector<uint8_t> dark(100 * 3, 0);
    CHECK(limit_power(dark.data(), 100, model, 10000, limited) == 500 && !limited);
    // no limit
    std::vector<uint8_t> full(100 * 3, 255);
    CHECK(limit_power(full.data(), 100, model, 0, limited) == 24500 && !limited);
}

TEST(frame_lane_factors_and_dithering_per_lane)
{
    const Gamma gamma;
    // two pixels on two lanes: lane 1 at half brightness without blue and without dithering
    px::layout::Table table;
    table.width = 2;
    table.height = 1;
    table.led = {0, 1};
    table.lane = {0, 1};
    table.lanes = 2;
    table.leds = 2;
    const LaneMix lanes[2] = {{}, {.red = 128, .green = 128, .blue = 0, .dither = false}};
    const Rgb canvas[2] = {{255, 255, 255}, {255, 255, 255}};
    uint8_t frame[6];
    compose(canvas, table, 255, gamma, lanes, 0, frame);
    CHECK(frame[0] == 255 && frame[1] == 255 && frame[2] == 255);
    CHECK(frame[3] == 128 && frame[4] == 128 && frame[5] == 0);
}
