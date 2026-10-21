// components/layout/src/layout.cpp: the panel answers into the canvas table.

#include "check.hpp"
#include "px/layout/layout.hpp"

using namespace px::layout;

namespace {

// the LED index at canvas x, y
uint16_t at(const Table &t, int x, int y)
{
    return t.led[static_cast<size_t>(y) * t.width + static_cast<size_t>(x)];
}

} // namespace

TEST(layout_rows_from_the_top_left_without_zigzag_count_like_reading)
{
    const Panel p{.lane = 0, .x = 0, .y = 0, .width = 4, .height = 3, .first = Corner::TopLeft, .zigzag = false};
    CHECK(index_in_panel(p, 0, 0) == 0);
    CHECK(index_in_panel(p, 3, 0) == 3);
    CHECK(index_in_panel(p, 0, 1) == 4);
    CHECK(index_in_panel(p, 3, 2) == 11);
}

TEST(layout_zigzag_rows_run_back_on_every_other_row)
{
    const Panel p{.lane = 0, .x = 0, .y = 0, .width = 4, .height = 3, .first = Corner::TopLeft};
    CHECK(index_in_panel(p, 3, 0) == 3);
    CHECK(index_in_panel(p, 3, 1) == 4); // under the end of the first row
    CHECK(index_in_panel(p, 0, 1) == 7);
    CHECK(index_in_panel(p, 0, 2) == 8);
}

TEST(layout_every_corner_and_columns)
{
    const Panel bottom_right{.lane = 0, .x = 0, .y = 0, .width = 4, .height = 3, .first = Corner::BottomRight};
    CHECK(index_in_panel(bottom_right, 3, 2) == 0);
    CHECK(index_in_panel(bottom_right, 0, 2) == 3);
    CHECK(index_in_panel(bottom_right, 0, 1) == 4);

    // columns from the bottom left, zigzag: up the first column, down the second
    const Panel columns{.lane = 0, .x = 0, .y = 0, .width = 4, .height = 3, .first = Corner::BottomLeft, .columns = true};
    CHECK(index_in_panel(columns, 0, 2) == 0);
    CHECK(index_in_panel(columns, 0, 0) == 2);
    CHECK(index_in_panel(columns, 1, 0) == 3);
    CHECK(index_in_panel(columns, 1, 2) == 5);
    CHECK(index_in_panel(columns, 3, 0) == 9); // the fourth column runs down again
    CHECK(index_in_panel(columns, 3, 2) == 11);
}

// The test matrix (docs/TODO.md): two 16x16 panels on lanes 1 and 2 above each other, an 8x32
// panel on lane 3 to their right; every panel starts top right with zigzag rows (found with the
// camera, 2026-10-19).
TEST(layout_test_matrix)
{
    const Panel panels[] = {
        {.lane = 0, .x = 0, .y = 0, .width = 16, .height = 16, .first = Corner::TopRight},
        {.lane = 1, .x = 0, .y = 16, .width = 16, .height = 16, .first = Corner::TopRight},
        {.lane = 2, .x = 16, .y = 0, .width = 8, .height = 32, .first = Corner::TopRight},
    };
    const auto table = build(24, 32, panels);
    REQUIRE(table.has_value());
    CHECK(table->leds == 768);
    CHECK(table->lanes == 3);
    CHECK(table->lane_pixels[0] == 256 && table->lane_pixels[1] == 256 && table->lane_pixels[2] == 256);
    CHECK(at(*table, 15, 0) == 0);
    CHECK(at(*table, 0, 0) == 15);
    CHECK(at(*table, 0, 1) == 16);
    CHECK(at(*table, 15, 1) == 31);
    CHECK(at(*table, 15, 16) == 256);   // lane 2 starts top right of the lower panel
    CHECK(at(*table, 23, 0) == 512);    // lane 3 top right
    CHECK(at(*table, 16, 0) == 519);    // its first row has 8 LEDs
    CHECK(at(*table, 16, 1) == 520);
    CHECK(at(*table, 23, 31) == 767);   // the 32nd row runs left to right: ends bottom right
    CHECK(at(*table, 15, 15) == 255);   // as the camera saw lane 1 end
    CHECK(table->lane[0] == 0 && table->lane[16 * 24] == 1 && table->lane[23] == 2);
    for (const uint16_t led : table->led) {
        CHECK(led != kNone);
    }
}

TEST(layout_panels_on_one_lane_are_chained)
{
    const Panel panels[] = {
        {.lane = 0, .x = 0, .y = 0, .width = 2, .height = 2, .first = Corner::TopLeft, .zigzag = false},
        {.lane = 0, .x = 2, .y = 0, .width = 2, .height = 2, .first = Corner::TopLeft, .zigzag = false},
    };
    const auto table = build(5, 2, panels);
    REQUIRE(table.has_value());
    CHECK(table->leds == 8);
    CHECK(at(*table, 2, 0) == 4);
    CHECK(at(*table, 4, 0) == kNone); // a canvas pixel without an LED
}

TEST(layout_refuses_what_cannot_be_shown)
{
    const Panel outside[] = {{.lane = 0, .x = 4, .y = 0, .width = 2, .height = 2}};
    CHECK_STR(build(5, 2, outside).error().text, "panel 1 reaches outside the 5x2 canvas");
    const Panel overlap[] = {{.lane = 0, .x = 0, .y = 0, .width = 2, .height = 2},
                             {.lane = 1, .x = 1, .y = 1, .width = 2, .height = 1}};
    CHECK_STR(build(4, 4, overlap).error().text, "panel 2 overlaps another one at 1,1");
    const Panel gap[] = {{.lane = 1, .x = 0, .y = 0, .width = 2, .height = 2}};
    CHECK_STR(build(4, 4, gap).error().text, "lane 1 has no panel, lane 2 has one");
    const Panel empty[] = {{.lane = 0, .x = 0, .y = 0, .width = 0, .height = 2}};
    CHECK_STR(build(4, 4, empty).error().text, "panel 1 has no size");
    CHECK_STR(build(100, 100, {}).error().text, "the canvas is 100x100, it may have 1 to 8192 pixels");
}
