// Glow's interface (px/effects/glow.hpp): compile(), the check of a program, its memory and a frame
// of it. A frame: the time and the sound, the init code once, the frame code, the trail (or dark) on
// the canvas, then the per-pixel code in runs of up to 32 pixels (row by row when it writes a grid,
// else straight through the canvas).

#include <cstdio>
#include <cstring>
#include <iterator>
#include <new>
#include <type_traits>

#include "../shared.hpp"
#include "compiler.hpp"
#include "machine.hpp"

namespace px::effects::glow {

using namespace detail;

namespace {

size_t align8(size_t n)
{
    return (n + 7) & ~size_t{7};
}

// The middle of pixel n of `count` across (n + 0.5) / count in 16.16, stepped from one pixel to
// the next without a division: the whole step and the rest of it (a 64-bit multiplication or a
// division per pixel took longer than the pixel's own code on the ESP32)
struct Across {
    uint32_t value;
    uint32_t rest;
    uint32_t step;
    uint32_t step_rest;
    uint32_t count;

    Across(uint32_t n, uint32_t count_) : count(count_)
    {
        const uint32_t numerator = (2 * n + 1) * 32768u;
        value = numerator / count;
        rest = numerator % count;
        step = 65536u / count;
        step_rest = 65536u % count;
    }

    void next()
    {
        value += step;
        rest += step_rest;
        if (rest >= count) {
            rest -= count;
            value++;
        }
    }
};

// the places of a run's 1 .. kLanes pixels from column x, row y on, stepping through the rows: x, y,
// u, v and i as the program uses them
void fill(const Header &h, Fix *v, uint16_t x, uint16_t y, uint32_t base, uint32_t lanes, uint16_t width,
          uint16_t height)
{
    const uint8_t uses = h.pixel_values;
    if ((uses & ((1u << kX) | (1u << kU))) != 0) {
        Fix *px = v + kX * kLanes;
        Fix *pu = v + kU * kLanes;
        Across u(x, width);
        uint16_t column = x;
        for (uint32_t k = 0; k < lanes; k++) {
            px[k] = column << 16;
            pu[k] = static_cast<Fix>(u.value);
            if (++column == width) {
                column = 0;
                u = Across(0, width);
            } else {
                u.next();
            }
        }
    }
    if ((uses & ((1u << kY) | (1u << kV))) != 0) {
        Fix *py = v + kY * kLanes;
        Fix *pv = v + kV * kLanes;
        uint16_t column = x;
        uint16_t row = y;
        Fix rv = static_cast<Fix>(Across(row, height).value);
        for (uint32_t k = 0; k < lanes; k++) {
            py[k] = row << 16;
            pv[k] = rv;
            if (++column == width) {
                column = 0;
                row++;
                rv = static_cast<Fix>(Across(row, height).value);
            }
        }
    }
    if ((uses & (1u << kI)) != 0) {
        Fix *pi = v + kI * kLanes;
        for (uint32_t k = 0; k < lanes; k++) {
            pi[k] = static_cast<Fix>((base + k) << 16);
        }
    }
}

// the hue (turns) and saturation of the light's color
void hue_and_saturation(Rgb c, Fix &hue, Fix &saturation)
{
    const int32_t hi = c.r > c.g ? (c.r > c.b ? c.r : c.b) : (c.g > c.b ? c.g : c.b);
    const int32_t lo = c.r < c.g ? (c.r < c.b ? c.r : c.b) : (c.g < c.b ? c.g : c.b);
    hue = 0;
    saturation = hi == 0 ? 0 : (hi - lo) * kOne / hi;
    const int32_t d = hi - lo;
    if (d == 0) {
        return;
    }
    int32_t h;
    if (hi == c.r) {
        h = (c.g - c.b) * kOne / (6 * d);
    } else if (hi == c.g) {
        h = kOne / 3 + (c.b - c.r) * kOne / (6 * d);
    } else {
        h = 2 * kOne / 3 + (c.r - c.g) * kOne / (6 * d);
    }
    hue = h & 0xffff;
}

Fix bands_mean(const uint8_t *bands, size_t first, size_t last)
{
    uint32_t sum = 0;
    for (size_t b = first; b < last; b++) {
        sum += bands[b];
    }
    return fx::from_byte(static_cast<uint8_t>(sum / (last - first)));
}

Fix side(uint32_t n)
{
    return static_cast<Fix>((n > 32767 ? 32767 : n) << 16);
}

Result stop(const Header &h, Canvas &canvas, State &state, const Machine &m)
{
    state.stopped = 1;
    state.stopped_line = lines_of(h)[m.stopped_at];
    px::effects::detail::clear(canvas);
    return {Status::Stopped, state.stopped_line, 0};
}

Result frame(const Header &h, Canvas &c, const Params &p, const uint8_t (&sliders)[kMaxParams], uint32_t ms,
             void *scratch)
{
    const uint16_t w = c.width;
    const uint16_t hh = c.height;
    const size_t n = static_cast<size_t>(w) * hh;
    const Layout l = layout(h, w, hh);
    if (c.memory == nullptr || c.memory_size < l.total) {
        px::effects::detail::clear(c);
        return {Status::NoMemory, 0, static_cast<uint32_t>(l.total)};
    }
    auto &memory = *reinterpret_cast<px::effects::detail::Memory *>(c.memory);
    auto &state = *reinterpret_cast<State *>(c.memory + l.state);
    const uint32_t dt = px::effects::detail::elapsed(memory, ms);
    const px::effects::detail::Sound sound = px::effects::detail::listen(memory, p, dt);
    uint8_t bands[kBands];
    px::effects::detail::hear_bands(&memory, p, dt, bands);
    if (state.stopped != 0) {
        px::effects::detail::clear(c);
        return {Status::Stopped, state.stopped_line, 0};
    }
    const bool first = state.started != kMagic;
    if (first) {
        state = {};
        state.started = kMagic;
        state.random = memory.random | 1u;
        state.pen = {255, 255, 255};
    }
    // the time at the speed: 128 as the clock, 0 stands, 255 about four times as fast
    const auto step = static_cast<uint32_t>(static_cast<uint64_t>(dt) * 65536u *
                                            px::effects::detail::per_ms(p.speed) / (1000u * 129u));
    state.time = (state.time + step) & 0x7fffffffu;

    Fix *s = static_cast<Fix *>(scratch);
    Fix *v = s + kScalars;
    std::memcpy(s + h.const_base, consts_of(h), h.const_count * sizeof(Fix));
    s[kT] = static_cast<Fix>(state.time);
    s[kDt] = static_cast<Fix>(step);
    s[kW] = side(w);
    s[kH] = side(hh);
    s[kN] = side(static_cast<uint32_t>(n));
    hue_and_saturation(p.color, s[kHue], s[kSat]);
    s[kLevel] = fx::from_byte(sound.level);
    s[kBeat] = sound.beat ? kOne : 0;
    s[kBass] = bands_mean(bands, 0, 6);
    s[kMid] = bands_mean(bands, 6, 18);
    s[kHigh] = bands_mean(bands, 18, kBands);
    s[kSpeed] = fx::from_byte(p.speed);
    for (size_t k = 0; k < h.info.param_count; k++) {
        s[kParamBase + k] = param_value(h.info.params[k], sliders[k]);
    }
    auto *vars = reinterpret_cast<Fix *>(c.memory + l.vars);
    std::memcpy(s + kVarBase, vars, h.var_count * sizeof(Fix));

    Machine m{};
    m.s = s;
    m.v = v;
    m.header = &h;
    m.code = code_of(h);
    m.params = &p;
    m.state = &state;
    m.arrays = reinterpret_cast<Fix *>(c.memory + l.arrays);
    m.grids = c.memory + l.grids;
    m.rows = c.memory + l.rows;
    for (uint8_t g = 0, slot = 0; g < kMaxGrids; g++) {
        m.row_slot[g] = slot;
        slot = static_cast<uint8_t>(slot + ((h.grid_rows >> g) & 1u));
    }
    m.trail = (h.flags & kDraws) != 0 ? reinterpret_cast<Rgb *>(c.memory + l.trail) : nullptr;
    m.width = w;
    m.height = hh;
    m.bands = bands;
    // the color tables of this frame, after the registers: a lookup per pixel instead of a blend
    auto *palette = reinterpret_cast<Rgb *>(v + kVectors * kLanes);
    auto *heat = palette + kTableColors;
    for (size_t i = 0; i < kTableColors; i++) {
        if ((h.flags & kPalette) != 0) {
            palette[i] = palette_color(p.palette, static_cast<uint8_t>(i), p.color);
        }
        if ((h.flags & kHeat) != 0) {
            heat[i] = px::effects::detail::heat_color(static_cast<uint8_t>(i), p);
        }
    }
    m.palette = palette;
    m.heat = heat;

    m.budget = kFrameSteps;
    bool ok = !first || run(m, h.streams[kInit], 0);
    if (ok) {
        m.budget = kFrameSteps;
        ok = run(m, h.streams[kFrame], 0);
    }
    std::memcpy(vars, s + kVarBase, h.var_count * sizeof(Fix));
    if (!ok) {
        return stop(h, c, state, m);
    }
    if (m.trail != nullptr) {
        std::memcpy(c.pixels, m.trail, n * sizeof(Rgb));
    } else {
        px::effects::detail::clear(c);
    }
    if ((h.flags & kHasPixel) == 0) {
        return {Status::Ok, 0, 0};
    }
    m.budget = kFrameSteps;
    if (!run(m, h.streams[kPrologue], 0)) {
        return stop(h, c, state, m);
    }
    m.budget = static_cast<int32_t>(n > 0x3fffff ? 0x7fffffff : n * kPixelSteps);
    const Range body = h.streams[kBody];
    if ((h.flags & kRows) == 0) {
        for (size_t base = 0; base < n; base += kLanes) {
            const auto lanes = static_cast<uint32_t>(n - base < kLanes ? n - base : kLanes);
            fill(h, v, static_cast<uint16_t>(base % w), static_cast<uint16_t>(base / w), static_cast<uint32_t>(base),
                 lanes, w, hh);
            m.pixels = c.pixels + base;
            if (!run(m, body, lanes)) {
                return stop(h, c, state, m);
            }
        }
        return {Status::Ok, 0, 0};
    }
    // writes a grid: row by row, the written cells kept in a row buffer until the row is done
    for (uint16_t y = 0; y < hh; y++) {
        for (uint8_t g = 0; g < h.grid_count; g++) {
            if ((h.grid_rows >> g & 1u) != 0) {
                std::memcpy(m.rows + m.row_slot[g] * w, m.grids + g * n + static_cast<size_t>(y) * w, w);
            }
        }
        for (uint16_t x0 = 0; x0 < w; x0 = static_cast<uint16_t>(x0 + kLanes)) {
            const uint32_t lanes = w - x0 < kLanes ? w - x0 : static_cast<uint32_t>(kLanes);
            const uint32_t base = static_cast<uint32_t>(y) * w + x0;
            fill(h, v, x0, y, base, lanes, w, hh);
            m.pixels = c.pixels + base;
            m.x0 = x0;
            if (!run(m, body, lanes)) {
                return stop(h, c, state, m);
            }
        }
        for (uint8_t g = 0; g < h.grid_count; g++) {
            if ((h.grid_rows >> g & 1u) != 0) {
                std::memcpy(m.grids + g * n + static_cast<size_t>(y) * w, m.rows + m.row_slot[g] * w, w);
            }
        }
    }
    return {Status::Ok, 0, 0};
}

bool terminated(const char *text, size_t size)
{
    return std::memchr(text, '\0', size) != nullptr;
}

bool check_field(char kind, uint8_t value, const Header &h)
{
    switch (kind) {
    case 'V':
        return value < h.vector_count;
    case 'M':
        return value == kNoMask || value < h.vector_count;
    default:
        return true;
    }
}

bool check_instruction(const Header &h, const Ins &in, Stream stream, Range range)
{
    if (in.op >= Op::Count) {
        return false;
    }
    const Signature &sig = signature(in.op);
    if ((sig.where == 'P' && stream != kBody) || (sig.where == 'F' && stream == kBody)) {
        return false;
    }
    const uint8_t fields[5] = {in.d, in.a, in.b, in.c, in.e};
    for (size_t i = 0; i < 5; i++) {
        if (!check_field(sig.fields[i], fields[i], h)) {
            return false;
        }
    }
    switch (sig.imm) {
    case 'J':
        return in.imm >= range.from && in.imm <= range.to;
    case 'A':
        return in.imm < h.array_count;
    case 'G':
        return in.imm < h.grid_count;
    default:
        return true;
    }
}

// a 16.16 number as text: 1.5, -0.25
void format_fix(char *out, size_t size, Fix v)
{
    const bool negative = v < 0;
    const uint32_t a = negative ? 0u - static_cast<uint32_t>(v) : static_cast<uint32_t>(v);
    const uint32_t fraction = static_cast<uint32_t>((static_cast<uint64_t>(a & 0xffffu) * 10000u + 32768u) >> 16);
    const uint32_t whole = (a >> 16) + fraction / 10000u;
    std::snprintf(out, size, "%s%u.%04u", negative ? "-" : "", static_cast<unsigned>(whole),
                  static_cast<unsigned>(fraction % 10000u));
}

} // namespace

namespace detail {

const Signature &signature(Op op)
{
    static constexpr Signature kTable[] = {
#define GLOW_SIGNATURE_BINARY(name, f)                                                                                 \
    {#f, "SSS--", '-', '*'}, {#f, "VVV--", '-', 'P'}, {#f, "VVS--", '-', 'P'}, {#f, "VSV--", '-', 'P'},
#define GLOW_SIGNATURE_UNARY(name, f) {#f, "SS---", '-', '*'}, {#f, "VV---", '-', 'P'},
        GLOW_BINARY(GLOW_SIGNATURE_BINARY) GLOW_UNARY(GLOW_SIGNATURE_UNARY)
#undef GLOW_SIGNATURE_BINARY
#undef GLOW_SIGNATURE_UNARY
        {"bcast", "VS---", '-', 'P'},
        {"sel", "SSSS-", '-', '*'},
        {"sel", "VVVV-", '-', 'P'},
        {"noise", "SSSS-", '-', '*'},
        {"noise", "VVVV-", '-', 'P'},
        {"rand", "S----", '-', '*'},
        {"rand", "V----", '-', 'P'},
        {"band", "SS---", '-', '*'},
        {"band", "VV---", '-', 'P'},
        {"lda", "SS---", 'A', '*'},
        {"lda", "VV---", 'A', 'P'},
        {"sta", "-SS--", 'A', 'F'},
        {"ldg", "SSS--", 'G', '*'},
        {"ldg", "VVV--", 'G', 'P'},
        {"stg", "-SSS-", 'G', 'F'},
        {"stg", "-VM--", 'G', 'P'},
        {"jmp", "-----", 'J', '*'},
        {"jz", "-S---", 'J', '*'},
        {"jnz", "-S---", 'J', '*'},
        {"jnone", "-V---", 'J', 'P'},
        {"hsv", "-VVVM", '-', 'P'},
        {"rgb", "-VVVM", '-', 'P'},
        {"pal", "-VV-M", '-', 'P'},
        {"col", "-V--M", '-', 'P'},
        {"heat", "-V--M", '-', 'P'},
        {"pen.hsv", "-SSS-", '-', 'F'},
        {"pen.rgb", "-SSS-", '-', 'F'},
        {"pen.pal", "-SS--", '-', 'F'},
        {"pen.col", "-S---", '-', 'F'},
        {"pen.heat", "-S---", '-', 'F'},
        {"fade", "-S---", '-', 'F'},
        {"blur", "-S---", '-', 'F'},
        {"clear", "-----", '-', 'F'},
        {"dot", "-SS--", '-', 'F'},
        {"line", "-SSSS", '-', 'F'},
        {"disc", "-SSS-", '-', 'F'},
        {"blob", "-SSS-", '-', 'F'},
    };
    static_assert(std::size(kTable) == static_cast<size_t>(Op::Count), "a signature per instruction");
    return kTable[static_cast<size_t>(op)];
}

Layout layout(const Header &h, uint16_t width, uint16_t height)
{
    const size_t n = static_cast<size_t>(width) * height;
    // a quarter turn swaps the sides: the row buffers fit either
    const size_t longer = width > height ? width : height;
    Layout l{};
    size_t at = px::effects::detail::kHeader;
    l.state = at;
    at += align8(sizeof(State));
    l.vars = at;
    at += h.var_count * sizeof(Fix);
    l.arrays = at;
    at = align8(at + h.array_words * sizeof(Fix));
    l.trail = at;
    at += (h.flags & kDraws) != 0 ? align8(n * sizeof(Rgb)) : 0;
    l.grids = at;
    at += h.grid_count * n;
    l.rows = at;
    at += static_cast<size_t>(std::popcount(h.grid_rows)) * longer;
    l.total = at;
    return l;
}

} // namespace detail

// -------------------------------------------------------------------------------------- interface

size_t workspace_size()
{
    return sizeof(Compiler);
}

size_t compile(std::string_view source, void *workspace, Error &error)
{
    // the program is built in the compiler's first member, at the start of the workspace
    static_assert(std::is_standard_layout_v<Compiler>, "the program is at the start of the workspace");
    if (source.size() > kMaxSource) {
        error = {0, 0, {}};
        std::snprintf(error.text, sizeof(error.text), "the script is longer than %u bytes",
                      static_cast<unsigned>(kMaxSource));
        return 0;
    }
    auto *compiler = new (workspace) Compiler(source, error);
    const size_t size = compiler->run();
    compiler->~Compiler();
    return size;
}

const Program *program(const void *bytes, size_t size)
{
    if (bytes == nullptr || size < sizeof(Header)) {
        return nullptr;
    }
    const auto &h = *static_cast<const Header *>(bytes);
    const size_t consts_at = (sizeof(Header) + 3) & ~size_t{3};
    if (h.magic != kMagic || h.size != size || h.consts_at != consts_at || h.const_count > kMaxConsts ||
        h.code_count > kMaxCode || h.code_at != consts_at + h.const_count * sizeof(Fix) ||
        h.lines_at != h.code_at + h.code_count * sizeof(Ins) || size != h.lines_at + h.code_count * sizeof(uint16_t)) {
        return nullptr;
    }
    if (h.const_base + h.const_count != kScalars || h.scalar_count != kScalars || h.vector_count > kVectors ||
        h.var_count > kMaxVars || h.const_base < kVarBase + h.var_count || h.array_count > kMaxArrays ||
        h.array_words > kMaxArrayWords || h.grid_count > kMaxGrids || h.info.param_count > kMaxParams ||
        !terminated(h.info.name, sizeof(h.info.name))) {
        return nullptr;
    }
    for (size_t a = 0; a < h.array_count; a++) {
        if (h.arrays[a].size == 0 || h.arrays[a].offset + h.arrays[a].size > h.array_words) {
            return nullptr;
        }
    }
    for (size_t k = 0; k < h.info.param_count; k++) {
        const Param &p = h.info.params[k];
        if (!terminated(p.name, sizeof(p.name)) || !terminated(p.label, sizeof(p.label)) || p.min >= p.max) {
            return nullptr;
        }
    }
    const Ins *code = code_of(h);
    for (uint8_t s = 0; s < kStreams; s++) {
        const Range r = h.streams[s];
        if (r.from > r.to || r.to > h.code_count) {
            return nullptr;
        }
        for (uint16_t pc = r.from; pc < r.to; pc++) {
            if (!check_instruction(h, code[pc], static_cast<Stream>(s), r)) {
                return nullptr;
            }
        }
    }
    return static_cast<const Program *>(bytes);
}

size_t size(const Program &program)
{
    return header_of(program).size;
}

const Info &info(const Program &program)
{
    return header_of(program).info;
}

size_t memory_need(const Program &program, uint16_t width, uint16_t height)
{
    return layout(header_of(program), width, height).total;
}

size_t scratch_size()
{
    return (kScalars + kVectors * kLanes) * sizeof(Fix) + 2 * kTableColors * sizeof(Rgb);
}

Result render(const Program &program, Canvas &canvas, const Params &params, const uint8_t (&sliders)[kMaxParams],
              uint32_t ms, void *scratch)
{
    if (canvas.width == 0 || canvas.height == 0) {
        return {Status::Ok, 0, 0};
    }
    const Header &h = header_of(program);
    // a script that does not follow the audio does not get it
    Params p = params;
    const Reactive reactive = h.info.reactive;
    const bool hears = reactive == Reactive::Required ||
                       (reactive == Reactive::Optional && params.reactive && params.strength > 0);
    p.audio = hears ? params.audio : nullptr;
    Result result{Status::Ok, 0, 0};
    px::effects::detail::render_turned(canvas, p, [&](Canvas &c) { result = frame(h, c, p, sliders, ms, scratch); });
    return result;
}

int32_t param_value(const Param &param, uint8_t slider)
{
    const int64_t span = static_cast<int64_t>(param.max) - param.min;
    int64_t v = param.min + span * slider / 255;
    if (param.step > 0) {
        v = param.min + (v - param.min + param.step / 2) / param.step * param.step;
        v = v > param.max ? param.max : v;
    }
    return static_cast<int32_t>(v);
}

uint8_t param_slider(const Param &param, int32_t value)
{
    const int64_t span = static_cast<int64_t>(param.max) - param.min;
    if (span <= 0) {
        return 0;
    }
    const int64_t s = ((static_cast<int64_t>(value) - param.min) * 255 + span / 2) / span;
    return static_cast<uint8_t>(s < 0 ? 0 : (s > 255 ? 255 : s));
}

void disassemble(const Program &program, void (*line)(const char *text, void *context), void *context)
{
    const Header &h = header_of(program);
    char text[128];
    std::snprintf(text, sizeof(text), "%s: %u instructions, %u numbers, %u per-pixel registers, %u bytes",
                  h.info.name, static_cast<unsigned>(h.code_count), static_cast<unsigned>(h.const_count),
                  static_cast<unsigned>(h.vector_count), static_cast<unsigned>(h.size));
    line(text, context);
    const Fix *consts = consts_of(h);
    for (uint16_t k = 0; k < h.const_count; k++) {
        char number[24];
        format_fix(number, sizeof(number), consts[k]);
        std::snprintf(text, sizeof(text), "  s%u = %s", static_cast<unsigned>(h.const_base + k), number);
        line(text, context);
    }
    static constexpr const char *kStreamNames[kStreams] = {"init", "frame", "per pixel, once a frame", "per pixel"};
    static constexpr uint8_t kOrder[kStreams] = {kInit, kFrame, kPrologue, kBody};
    const Ins *code = code_of(h);
    const uint16_t *lines = lines_of(h);
    for (const uint8_t s : kOrder) {
        const Range r = h.streams[s];
        std::snprintf(text, sizeof(text), "%s: %u", kStreamNames[s], static_cast<unsigned>(r.to - r.from));
        line(text, context);
        for (uint16_t pc = r.from; pc < r.to; pc++) {
            const Ins &in = code[pc];
            const Signature &sig = signature(in.op);
            size_t at = static_cast<size_t>(
                std::snprintf(text, sizeof(text), "%5u  %-9s", static_cast<unsigned>(pc), sig.name));
            const uint8_t fields[5] = {in.d, in.a, in.b, in.c, in.e};
            for (size_t f = 0; f < 5 && at < sizeof(text); f++) {
                const char kind = sig.fields[f];
                if (kind == '-' || (kind == 'M' && fields[f] == kNoMask)) {
                    continue;
                }
                const char *prefix = kind == 'M' ? " if v" : (kind == 'S' ? " s" : " v");
                at += static_cast<size_t>(
                    std::snprintf(text + at, sizeof(text) - at, "%s%u", prefix, static_cast<unsigned>(fields[f])));
            }
            if (sig.imm != '-' && at < sizeof(text)) {
                const char *what = sig.imm == 'J' ? " ->" : (sig.imm == 'A' ? " array" : " grid");
                at += static_cast<size_t>(
                    std::snprintf(text + at, sizeof(text) - at, "%s %u", what, static_cast<unsigned>(in.imm)));
            }
            if (at < sizeof(text)) {
                std::snprintf(text + at, sizeof(text) - at, "   ; line %u", static_cast<unsigned>(lines[pc]));
            }
            line(text, context);
        }
    }
}

} // namespace px::effects::glow
