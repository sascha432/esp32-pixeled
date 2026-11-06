#pragma once

// Glow: animations as scripts, compiled and run on the device (docs/glow.md). A script is source
// text; compile() turns it into a Program (one block of bytes, may be copied), render() draws a
// frame of it like effects::render() does for the built-in animations. Numbers are 16.16 fixed
// point; the per-pixel code runs on up to 32 pixels per instruction. A script cannot stop the
// firmware: indices are clamped, every frame has a step budget. Pure C++, host tested; in the
// firmware only with CONFIG_PX_SCRIPTS.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "px/effects/effects.hpp"

namespace px::effects::glow {

constexpr size_t kMaxParams = 8;
// the longest source compile() takes
constexpr size_t kMaxSource = 8192;
constexpr size_t kNameSize = 32;

// A slider of a script (`param name = initial [min .. max step s] "Label"`), values 16.16
struct Param {
    char name[16];
    char label[24];
    int32_t min;
    int32_t max;
    // 0: any value between
    int32_t step;
    int32_t initial;
};

// What a script is, for the lists (the web UI, Home Assistant) and the sliders
struct Info {
    char name[kNameSize];
    Reactive reactive;
    bool uses_color;
    bool uses_palette;
    uint8_t param_count;
    Param params[kMaxParams];
};

// A compile error: where (1-based; 0 when the whole script is meant) and what
struct Error {
    uint16_t line;
    uint16_t column;
    char text[80];
};

// A compiled script (opaque); its bytes start with a header
struct Program;

// The memory compile() works in (about 15 KB)
size_t workspace_size();

// Compiles `source` in `workspace` (workspace_size() bytes, aligned as malloc's). Returns the
// program's size, 0 with `error` set; the program is that many bytes at the start of the workspace,
// to be copied out before the workspace is used again.
size_t compile(std::string_view source, void *workspace, Error &error);

// `bytes` as a program after checking it (header, instructions, registers, jumps); nullptr when it
// is not one
const Program *program(const void *bytes, size_t size);
size_t size(const Program &program);
const Info &info(const Program &program);

// The canvas memory (Canvas::memory) the program needs on a canvas of `width` x `height`
size_t memory_need(const Program &program, uint16_t width, uint16_t height);

// The registers render() works with: one block for every program (the frames are drawn one after
// the other), internal RAM is faster
size_t scratch_size();

enum class Status : uint8_t {
    Ok,
    // the canvas memory is smaller than memory_need(): dark
    NoMemory,
    // the script used up a frame's steps (an endless loop?) at `line`: dark until it is drawn on
    // fresh memory
    Stopped,
};

struct Result {
    Status status;
    uint16_t line;
    // NoMemory: the bytes it needs
    uint32_t need;
};

// Draws a frame of `program` on the whole canvas at `ms` (a clock that only moves forward), turned
// by Params::rotation, with the audio only when it reacts. `sliders` are its params (0..255).
// Its state between frames is in Canvas::memory, which starts all 0.
Result render(const Program &program, Canvas &canvas, const Params &params, const uint8_t (&sliders)[kMaxParams],
              uint32_t ms, void *scratch);

// A slider position (0..255) as the param's value (16.16) and back
int32_t param_value(const Param &param, uint8_t slider);
uint8_t param_slider(const Param &param, int32_t value);

// The program as text, a line per instruction (console `script dis`)
void disassemble(const Program &program, void (*line)(const char *text, void *context), void *context);

} // namespace px::effects::glow
