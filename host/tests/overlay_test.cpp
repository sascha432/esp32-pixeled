// components/effects/src/overlay.cpp: the font, the scrolling and the clock.

#include <vector>

#include "check.hpp"
#include "px/effects/overlay.hpp"

using namespace px::effects;

namespace {

struct Picture {
    std::vector<Rgb> pixels;
    Canvas canvas;

    Picture(uint16_t w, uint16_t h) : pixels(static_cast<size_t>(w) * h), canvas{w, h, pixels.data()} {}

    bool lit(uint16_t x, uint16_t y)
    {
        const Rgb p = canvas.at(x, y);
        return p.r != 0 || p.g != 0 || p.b != 0;
    }

    // the columns with a lit pixel
    uint16_t lit_columns()
    {
        uint16_t count = 0;
        for (uint16_t x = 0; x < canvas.width; x++) {
            bool any = false;
            for (uint16_t y = 0; y < canvas.height; y++) {
                any |= lit(x, y);
            }
            count += any ? 1 : 0;
        }
        return count;
    }
};

} // namespace

TEST(overlay_text_width_is_proportional)
{
    CHECK(text_width("") == 0);
    CHECK(text_width("H") == 5);
    CHECK(text_width("i") == 3);
    CHECK(text_width("Hi") == 9);
    CHECK(text_width("a b") == 5 + 1 + 3 + 1 + 5);
    // UTF-8: an umlaut is one character, an unknown one a question mark
    CHECK(text_width("\xc3\xa4") == 5);
    CHECK(text_width("\xe2\x82\xac") == text_width("?"));
}

TEST(overlay_draws_the_letters)
{
    Picture p(12, 8);
    draw_text(p.canvas, "HI", 0, 0, {255, 255, 255});
    // H: both sides full height, the bar in the middle row
    for (uint16_t y = 0; y < 7; y++) {
        CHECK(p.lit(0, y) && p.lit(4, y));
    }
    CHECK(p.lit(2, 3) && !p.lit(2, 2));
    CHECK(!p.lit(5, 3));          // the gap
    CHECK(p.lit(7, 0) && p.lit(7, 6)); // I's stem
    CHECK(!p.lit(0, 7));          // nothing below the font
    // clipped at every edge, no crash
    draw_text(p.canvas, "Clipped", -3, -2, {1, 1, 1});
    draw_text(p.canvas, "Clipped", 10, 5, {1, 1, 1});
}

TEST(overlay_text_scrolls_when_it_is_wider)
{
    CHECK(text_x(24, 10, 128, 12345) == 7); // fits: centered, still
    CHECK(text_x(24, 50, 0, 12345) == 0);   // speed 0: stands at the left
    CHECK(text_x(24, 50, 255, 0) == 24);    // starts just right of the canvas
    const int a = text_x(24, 50, 255, 100);
    const int b = text_x(24, 50, 255, 200);
    CHECK(a < 24 && b < a);                 // moves left
    // after the whole way (74 pixels at 50 a second) it comes in again
    CHECK(text_x(24, 50, 255, 1480) == 24);
}

TEST(overlay_clock_fits_the_canvas)
{
    Picture wide(32, 8);
    draw_clock(wide.canvas, 12, 34, true, {255, 0, 0});
    CHECK(wide.lit_columns() >= 20);
    CHECK(wide.lit(15, 1) || wide.lit(16, 1)); // the colon
    Picture off(32, 8);
    draw_clock(off.canvas, 12, 34, false, {255, 0, 0});
    CHECK(off.lit_columns() < wide.lit_columns());
    // 24 wide: the small digits, 5 rows
    Picture small(24, 6);
    draw_clock(small.canvas, 9, 5, true, {255, 255, 255});
    CHECK(small.lit_columns() >= 12 && small.lit_columns() <= 17);
    CHECK(!small.lit(0, 0) && !small.lit(23, 0));
}
