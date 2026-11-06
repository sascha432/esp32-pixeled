// The machine (machine.hpp): one loop over the instructions. A vector instruction is a tight loop
// over the run's pixels, so the cost of reading an instruction is shared by up to 32 of them; the
// scalar ones (the init and frame code, and what is the same for the pixels of a run) run once.
//
// Each loop over the lanes is a small function (lanes_*), the math forced inline into it, and the
// function forced into the switch: GCC inlines nothing more into a function this big by itself, and
// a call per pixel cost several times the operation (measured 2026-11-05 on the ESP32: 700 cycles a
// pixel for plasma's 17 instructions), a call per instruction the window spills of the Xtensa's
// deep call chain. The switch has jump tables (components/effects/CMakeLists.txt: ESP-IDF builds
// without them, a chain of compares for 130 cases).

#include <cstring>

#include "../shared.hpp"
#include "../trail.hpp"
#include "machine.hpp"

namespace px::effects::glow::detail {

namespace {

using px::effects::detail::World;

Fix *vec(const Machine &m, uint8_t reg)
{
    return m.v + static_cast<size_t>(reg) * kLanes;
}

Fix next_random(uint32_t &x)
{
    // xorshift32, the upper 16 bits: 0..1
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return static_cast<Fix>(x >> 16);
}

int32_t clamp_index(Fix f, int32_t size)
{
    const int32_t i = f >> 16;
    return i < 0 ? 0 : (i >= size ? size - 1 : i);
}

uint8_t *cell(const Machine &m, uint16_t grid, Fix x, Fix y)
{
    const size_t n = static_cast<size_t>(m.width) * m.height;
    return m.grids + grid * n + static_cast<size_t>(clamp_index(y, m.height)) * m.width +
           static_cast<size_t>(clamp_index(x, m.width));
}

Fix element(const Machine &m, uint16_t array, Fix index)
{
    const Array &a = m.header->arrays[array];
    return m.arrays[a.offset + clamp_index(index, a.size)];
}

// 0..1 as 0..256 (the trail's fade and blur)
uint32_t amount256(Fix a)
{
    const Fix c = a < 0 ? 0 : (a > kOne ? kOne : a);
    return static_cast<uint32_t>((c + 128) >> 8);
}

// a color of a frame's table at a place (a turn) and a brightness
Rgb from_table(const Rgb *table, uint8_t index, Fix value)
{
    const Rgb c = table[index];
    const uint8_t v = fx::byte(value);
    return v == 255 ? c : fx::scale(c, v);
}

World world_of(const Machine &m)
{
    return {nullptr, m.trail, nullptr, nullptr, 0, m.width, m.height};
}

#define GLOW_LANES GLOW_INLINE

template <Fix (*F)(Fix, Fix)>
GLOW_LANES void lanes_vv(Fix *d, const Fix *a, const Fix *b, uint32_t n)
{
    for (uint32_t k = 0; k < n; k++) {
        d[k] = F(a[k], b[k]);
    }
}

template <Fix (*F)(Fix, Fix)>
GLOW_LANES void lanes_vs(Fix *d, const Fix *a, Fix b, uint32_t n)
{
    for (uint32_t k = 0; k < n; k++) {
        d[k] = F(a[k], b);
    }
}

template <Fix (*F)(Fix, Fix)>
GLOW_LANES void lanes_sv(Fix *d, Fix a, const Fix *b, uint32_t n)
{
    for (uint32_t k = 0; k < n; k++) {
        d[k] = F(a, b[k]);
    }
}

template <Fix (*F)(Fix)>
GLOW_LANES void lanes_v(Fix *d, const Fix *a, uint32_t n)
{
    for (uint32_t k = 0; k < n; k++) {
        d[k] = F(a[k]);
    }
}

GLOW_LANES void lanes_fill(Fix *d, Fix a, uint32_t n)
{
    for (uint32_t k = 0; k < n; k++) {
        d[k] = a;
    }
}

GLOW_LANES void lanes_select(Fix *d, const Fix *c, const Fix *a, const Fix *b, uint32_t n)
{
    for (uint32_t k = 0; k < n; k++) {
        d[k] = c[k] != 0 ? a[k] : b[k];
    }
}

GLOW_LANES void lanes_noise(Fix *d, const Fix *a, const Fix *b, const Fix *c, uint32_t n)
{
    for (uint32_t k = 0; k < n; k++) {
        d[k] = fx::noise(a[k], b[k], c[k]);
    }
}

GLOW_LANES void lanes_random(Fix *d, uint32_t &state, uint32_t n)
{
    uint32_t x = state;
    for (uint32_t k = 0; k < n; k++) {
        d[k] = next_random(x);
    }
    state = x;
}

GLOW_LANES void lanes_band(Fix *d, const Fix *a, const uint8_t *bands, uint32_t n)
{
    for (uint32_t k = 0; k < n; k++) {
        d[k] = fx::from_byte(bands[clamp_index(a[k], static_cast<int32_t>(kBands))]);
    }
}

GLOW_LANES void lanes_element(Fix *d, const Fix *a, const Fix *array, int32_t size, uint32_t n)
{
    for (uint32_t k = 0; k < n; k++) {
        d[k] = array[clamp_index(a[k], size)];
    }
}

GLOW_LANES void lanes_cell(const Machine &m, uint16_t grid, Fix *d, const Fix *a, const Fix *b, uint32_t n)
{
    const uint8_t *g = m.grids + grid * (static_cast<size_t>(m.width) * m.height);
    for (uint32_t k = 0; k < n; k++) {
        d[k] = fx::from_byte(g[static_cast<size_t>(clamp_index(b[k], m.height)) * m.width +
                                static_cast<size_t>(clamp_index(a[k], m.width))]);
    }
}

GLOW_LANES void lanes_row(uint8_t *row, const Fix *a, const Fix *mask, uint32_t n)
{
    for (uint32_t k = 0; k < n; k++) {
        if (mask == nullptr || mask[k] != 0) {
            row[k] = fx::byte(a[k]);
        }
    }
}

// the pixels' colors of a color instruction (hsv, rgb, pal, col, heat)
GLOW_LANES void lanes_color(const Machine &m, const Ins &in, uint32_t n)
{
    const Fix *a = vec(m, in.a);
    const Fix *b = vec(m, in.b);
    const Fix *c = vec(m, in.c);
    const Fix *mask = in.e == kNoMask ? nullptr : vec(m, in.e);
    Rgb *pixels = m.pixels;
    for (uint32_t k = 0; k < n; k++) {
        if (mask != nullptr && mask[k] == 0) {
            continue;
        }
        switch (in.op) {
        case Op::Chsv:
            pixels[k] = fx::hsv(fx::hue8(a[k]), fx::byte(b[k]), fx::byte(c[k]));
            break;
        case Op::Crgb:
            pixels[k] = {fx::byte(a[k]), fx::byte(b[k]), fx::byte(c[k])};
            break;
        case Op::Cpal:
            pixels[k] = from_table(m.palette, fx::hue8(a[k]), b[k]);
            break;
        case Op::Ccol:
            pixels[k] = fx::scale(m.params->color, fx::byte(a[k]));
            break;
        default:
            pixels[k] = m.heat[fx::byte(a[k])];
            break;
        }
    }
}

// a jump to `target`; back (a loop) it costs the instructions jumped over from the budget
bool jump(Machine &m, uint32_t &pc, uint16_t target, uint32_t weight)
{
    if (target < pc) {
        m.budget -= static_cast<int32_t>((pc - target) * weight);
        if (m.budget < 0) {
            m.stopped_at = static_cast<uint16_t>(pc - 1);
            return false;
        }
    }
    pc = target;
    return true;
}

} // namespace

bool run(Machine &m, Range range, uint32_t lanes)
{
    Fix *const s = m.s;
    const Ins *const code = m.code;
    const uint32_t weight = lanes > 0 ? lanes : 1;
    uint32_t pc = range.from;
    while (pc < range.to) {
        const Ins &in = code[pc++];
        switch (in.op) {
#define GLOW_RUN_BINARY(name, f)                                                                                       \
    case Op::name##S:                                                                                                  \
        s[in.d] = fx::f(s[in.a], s[in.b]);                                                                             \
        break;                                                                                                         \
    case Op::name##VV:                                                                                                 \
        lanes_vv<fx::f>(vec(m, in.d), vec(m, in.a), vec(m, in.b), lanes);                                              \
        break;                                                                                                         \
    case Op::name##VS:                                                                                                 \
        lanes_vs<fx::f>(vec(m, in.d), vec(m, in.a), s[in.b], lanes);                                                   \
        break;                                                                                                         \
    case Op::name##SV:                                                                                                 \
        lanes_sv<fx::f>(vec(m, in.d), s[in.a], vec(m, in.b), lanes);                                                   \
        break;
#define GLOW_RUN_UNARY(name, f)                                                                                        \
    case Op::name##S:                                                                                                  \
        s[in.d] = fx::f(s[in.a]);                                                                                      \
        break;                                                                                                         \
    case Op::name##V:                                                                                                  \
        lanes_v<fx::f>(vec(m, in.d), vec(m, in.a), lanes);                                                             \
        break;
            GLOW_BINARY(GLOW_RUN_BINARY)
            GLOW_UNARY(GLOW_RUN_UNARY)
#undef GLOW_RUN_BINARY
#undef GLOW_RUN_UNARY

        case Op::Bcast:
            lanes_fill(vec(m, in.d), s[in.a], lanes);
            break;
        case Op::SelS:
            s[in.d] = s[in.a] != 0 ? s[in.b] : s[in.c];
            break;
        case Op::SelV:
            lanes_select(vec(m, in.d), vec(m, in.a), vec(m, in.b), vec(m, in.c), lanes);
            break;
        case Op::NoiseS:
            s[in.d] = fx::noise(s[in.a], s[in.b], s[in.c]);
            break;
        case Op::NoiseV:
            lanes_noise(vec(m, in.d), vec(m, in.a), vec(m, in.b), vec(m, in.c), lanes);
            break;
        case Op::RandS:
            s[in.d] = next_random(m.state->random);
            break;
        case Op::RandV:
            lanes_random(vec(m, in.d), m.state->random, lanes);
            break;
        case Op::BandS:
            s[in.d] = fx::from_byte(m.bands[clamp_index(s[in.a], static_cast<int32_t>(kBands))]);
            break;
        case Op::BandV:
            lanes_band(vec(m, in.d), vec(m, in.a), m.bands, lanes);
            break;
        case Op::LdaS:
            s[in.d] = element(m, in.imm, s[in.a]);
            break;
        case Op::LdaV: {
            const Array &a = m.header->arrays[in.imm];
            lanes_element(vec(m, in.d), vec(m, in.a), m.arrays + a.offset, a.size, lanes);
            break;
        }
        case Op::Sta: {
            const Array &a = m.header->arrays[in.imm];
            m.arrays[a.offset + clamp_index(s[in.a], a.size)] = s[in.b];
            break;
        }
        case Op::LdgS:
            s[in.d] = fx::from_byte(*cell(m, in.imm, s[in.a], s[in.b]));
            break;
        case Op::LdgV:
            lanes_cell(m, in.imm, vec(m, in.d), vec(m, in.a), vec(m, in.b), lanes);
            break;
        case Op::StgS:
            *cell(m, in.imm, s[in.a], s[in.b]) = fx::byte(s[in.c]);
            break;
        case Op::StgV:
            // into the row buffer, at the run's columns; the row is written after it
            lanes_row(m.rows + static_cast<size_t>(m.row_slot[in.imm]) * m.width + m.x0, vec(m, in.a),
                      in.b == kNoMask ? nullptr : vec(m, in.b), lanes);
            break;
        case Op::Jmp:
            if (!jump(m, pc, in.imm, weight)) {
                return false;
            }
            break;
        case Op::Jz:
            if (s[in.a] == 0 && !jump(m, pc, in.imm, weight)) {
                return false;
            }
            break;
        case Op::Jnz:
            if (s[in.a] != 0 && !jump(m, pc, in.imm, weight)) {
                return false;
            }
            break;
        case Op::Jnone: {
            // forward only (the compiler's blocks): no steps to count
            const Fix *mask = vec(m, in.a);
            bool any = false;
            for (uint32_t k = 0; k < lanes && !any; k++) {
                any = mask[k] != 0;
            }
            pc = any ? pc : in.imm;
            break;
        }
        case Op::Chsv:
        case Op::Crgb:
        case Op::Cpal:
        case Op::Ccol:
        case Op::Cheat:
            lanes_color(m, in, lanes);
            break;
        case Op::Phsv:
            m.state->pen = hsv(fx::hue8(s[in.a]), fx::byte(s[in.b]), fx::byte(s[in.c]));
            break;
        case Op::Prgb:
            m.state->pen = {fx::byte(s[in.a]), fx::byte(s[in.b]), fx::byte(s[in.c])};
            break;
        case Op::Ppal:
            m.state->pen = from_table(m.palette, fx::hue8(s[in.a]), s[in.b]);
            break;
        case Op::Pcol:
            m.state->pen = px::effects::detail::scale(m.params->color, fx::byte(s[in.a]));
            break;
        case Op::Pheat:
            m.state->pen = m.heat[fx::byte(s[in.a])];
            break;
        case Op::Fade:
        case Op::Blur:
        case Op::Clear:
        case Op::Dot:
        case Op::Line:
        case Op::Disc:
        case Op::Blob: {
            if (m.trail == nullptr) {
                break;
            }
            World w = world_of(m);
            const Rgb pen = m.state->pen;
            // places in pixels as 1/256 pixel
            const int32_t a = s[in.a] >> 8;
            const int32_t b = s[in.b] >> 8;
            const int32_t c = s[in.c] >> 8;
            switch (in.op) {
            case Op::Fade:
                px::effects::detail::fade(w, 256u - amount256(s[in.a]));
                break;
            case Op::Blur:
                px::effects::detail::blur(w, amount256(s[in.a]));
                break;
            case Op::Clear:
                std::memset(m.trail, 0, static_cast<size_t>(m.width) * m.height * sizeof(Rgb));
                break;
            case Op::Dot:
                px::effects::detail::splat(w, a, b, pen);
                break;
            case Op::Line:
                px::effects::detail::line(w, a, b, c, s[in.e] >> 8, pen, pen);
                break;
            default:
                px::effects::detail::disc(w, a, b, c, pen, in.op == Op::Disc);
                break;
            }
            break;
        }
        default:
            break;
        }
    }
    return true;
}

} // namespace px::effects::glow::detail
