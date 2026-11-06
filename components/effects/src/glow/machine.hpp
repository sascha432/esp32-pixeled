#pragma once

// Inside glow: the machine that runs a program's instructions (vm.cpp), set up for a frame by
// runtime.cpp.

#include <cstddef>
#include <cstdint>

#include "bytecode.hpp"

namespace px::effects::glow::detail {

// a frame's step budget: the init and frame code, and the per-pixel code for each pixel (counted at
// the jumps back of the loops, by the instructions jumped over)
constexpr int32_t kFrameSteps = 100000;
constexpr int32_t kPixelSteps = 512;

struct Machine {
    Fix *s;
    // the vector registers, kLanes each
    Fix *v;
    const Header *header;
    const Ins *code;
    const Params *params;
    State *state;
    Fix *arrays;
    // grid g at grids + g x width x height
    uint8_t *grids;
    // the row buffer of grid g (written per pixel) at rows + row_slot[g] x width
    uint8_t *rows;
    uint8_t row_slot[kMaxGrids];
    // the trail (a program that draws)
    Rgb *trail;
    uint16_t width;
    uint16_t height;
    // the first pixel of the run of the per-pixel code, and its column (row by row)
    Rgb *pixels;
    uint16_t x0;
    // the bands heard, 0..255
    const uint8_t *bands;
    // the palette's and the heat's 256 colors of this frame (when the program takes them)
    const Rgb *palette;
    const Rgb *heat;
    int32_t budget;
    // where the budget ran out
    uint16_t stopped_at;
};

// Runs the instructions of `range` with `lanes` pixels (0 for the scalar code). False when the
// budget ran out (at Machine::stopped_at).
bool run(Machine &m, Range range, uint32_t lanes);

} // namespace px::effects::glow::detail
