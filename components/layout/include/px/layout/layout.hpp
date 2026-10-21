#pragma once

// The layout of the LEDs (docs/leds.md section 4.1): the canvas the animations draw on, and the
// panels that show it. A panel is described by three answers: its size, the corner of its first
// LED, and how its LEDs run (rows or columns, zigzag or all the same way). A strip is a panel of
// one row. Pure C++, host tested.
//
// build() turns the panels into a table: for every canvas pixel the index of its LED in the
// output frame (the pixels of lane 1, then lane 2, ...), or kNone. Panels on the same lane are
// chained in the order they are listed.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include "px/memory/large.hpp"

namespace px::layout {

enum class Corner : uint8_t {
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

struct Panel {
    // the output lane, from 0
    uint8_t lane;
    // the panel's top left pixel on the canvas
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    // where the first LED is, as seen on the canvas
    Corner first = Corner::TopLeft;
    // the LEDs run along columns (up or down) instead of rows
    bool columns = false;
    // every other row (column) runs back
    bool zigzag = true;
};

// at most this many lanes and canvas pixels
constexpr size_t kMaxLanes = 16;
constexpr size_t kMaxPixels = 8192;

constexpr uint16_t kNone = 0xffff;

struct Table {
    uint16_t width = 0;
    uint16_t height = 0;
    // canvas pixel (x + y * width) -> LED index in the output frame, kNone: no LED there
    memory::Vector<uint16_t> led;
    // canvas pixel -> its lane (0 where there is no LED), for the settings of each lane
    memory::Vector<uint8_t> lane;
    // the pixels of each lane
    uint16_t lane_pixels[kMaxLanes] = {};
    size_t lanes = 0;
    size_t leds = 0;
};

// The problem of a layout, for people: "panel 2 reaches outside the canvas"
struct Error {
    char text[96];
};

// The index of the LED at (x, y) of the panel, counted from the panel's first LED.
size_t index_in_panel(const Panel &panel, uint16_t x, uint16_t y);

// The table of a canvas of `width` x `height` shown by `panels`. Refused: an empty canvas or one
// larger than kMaxPixels, a panel of size 0, outside the canvas or on a lane from kMaxLanes on,
// panels that overlap, a lane without a panel while a later lane has one.
std::expected<Table, Error> build(uint16_t width, uint16_t height, std::span<const Panel> panels);

} // namespace px::layout
