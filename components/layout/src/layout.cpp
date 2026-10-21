// The layout table (px/layout/layout.hpp, docs/leds.md section 4.1).

#include "px/layout/layout.hpp"

#include <cstdio>
#include <iterator>

namespace px::layout {

namespace {

template <class... Args>
std::unexpected<Error> fail(const char *format, Args... args)
{
    Error error{};
    std::snprintf(error.text, sizeof(error.text), format, args...);
    return std::unexpected(error);
}

bool at_left(Corner c)
{
    return c == Corner::TopLeft || c == Corner::BottomLeft;
}

bool at_top(Corner c)
{
    return c == Corner::TopLeft || c == Corner::TopRight;
}

} // namespace

size_t index_in_panel(const Panel &panel, uint16_t x, uint16_t y)
{
    // positions counted from the first LED's corner
    const size_t from_side = at_left(panel.first) ? x : panel.width - 1u - x;
    const size_t from_end = at_top(panel.first) ? y : panel.height - 1u - y;
    // the run the LED is in and its place in the run
    const size_t run = panel.columns ? from_side : from_end;
    const size_t length = panel.columns ? panel.height : panel.width;
    size_t place = panel.columns ? from_end : from_side;
    if (panel.zigzag && run % 2 == 1) {
        place = length - 1 - place;
    }
    return run * length + place;
}

std::expected<Table, Error> build(uint16_t width, uint16_t height, std::span<const Panel> panels)
{
    const size_t pixels = static_cast<size_t>(width) * height;
    if (pixels == 0 || pixels > kMaxPixels) {
        return fail("the canvas is %ux%u, it may have 1 to %u pixels", width, height, static_cast<unsigned>(kMaxPixels));
    }
    Table table;
    table.width = width;
    table.height = height;
    table.led.assign(pixels, kNone);
    table.lane.assign(pixels, 0);

    // the first LED of each panel within its lane: the panels before it on the lane
    size_t start[64] = {};
    if (panels.size() > std::size(start)) {
        return fail("at most %u panels", static_cast<unsigned>(std::size(start)));
    }
    for (size_t i = 0; i < panels.size(); i++) {
        const Panel &p = panels[i];
        const unsigned n = static_cast<unsigned>(i + 1);
        if (p.width == 0 || p.height == 0) {
            return fail("panel %u has no size", n);
        }
        if (p.lane >= kMaxLanes) {
            return fail("panel %u: lane %u, there are %u", n, p.lane + 1u, static_cast<unsigned>(kMaxLanes));
        }
        if (p.x + p.width > width || p.y + p.height > height) {
            return fail("panel %u reaches outside the %ux%u canvas", n, width, height);
        }
        start[i] = table.lane_pixels[p.lane];
        const size_t lane_end = start[i] + static_cast<size_t>(p.width) * p.height;
        if (lane_end > kNone) {
            return fail("lane %u has more than %u LEDs", p.lane + 1u, static_cast<unsigned>(kNone));
        }
        table.lane_pixels[p.lane] = static_cast<uint16_t>(lane_end);
        if (p.lane + 1u > table.lanes) {
            table.lanes = p.lane + 1u;
        }
    }
    // the frame holds the lanes one after another
    size_t lane_start[kMaxLanes] = {};
    for (size_t lane = 0; lane < table.lanes; lane++) {
        if (table.lane_pixels[lane] == 0) {
            return fail("lane %u has no panel, lane %u has one", static_cast<unsigned>(lane + 1),
                        static_cast<unsigned>(table.lanes));
        }
        lane_start[lane] = table.leds;
        table.leds += table.lane_pixels[lane];
    }
    if (table.leds > kNone) {
        return fail("%u LEDs, at most %u", static_cast<unsigned>(table.leds), static_cast<unsigned>(kNone));
    }
    for (size_t i = 0; i < panels.size(); i++) {
        const Panel &p = panels[i];
        for (uint16_t y = 0; y < p.height; y++) {
            for (uint16_t x = 0; x < p.width; x++) {
                uint16_t &cell = table.led[(p.y + y) * static_cast<size_t>(width) + p.x + x];
                if (cell != kNone) {
                    return fail("panel %u overlaps another one at %u,%u", static_cast<unsigned>(i + 1),
                                static_cast<unsigned>(p.x + x), static_cast<unsigned>(p.y + y));
                }
                cell = static_cast<uint16_t>(lane_start[p.lane] + start[i] + index_in_panel(p, x, y));
                table.lane[(p.y + y) * static_cast<size_t>(width) + p.x + x] = p.lane;
            }
        }
    }
    return table;
}

} // namespace px::layout
