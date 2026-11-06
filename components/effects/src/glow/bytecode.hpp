#pragma once

// Inside glow (px/effects/glow.hpp, docs/glow.md): the instructions, the program's layout, the
// script's memory and the 16.16 math, shared by the compiler, the checker and the machine.

#include <bit>
#include <cstddef>
#include <cstdint>

#include "../internal.hpp"
#include "px/effects/glow.hpp"

namespace px::effects::glow::detail {

using Fix = int32_t;
constexpr Fix kOne = 65536;

// ---------------------------------------------------------------------------------- registers

// The machine has 256 scalar registers and 32 vector registers of 32 lanes: the per-pixel code runs
// on up to 32 pixels at once, an instruction a tight loop over them.
constexpr size_t kScalars = 256;
constexpr size_t kVectors = 32;
constexpr size_t kLanes = 32;

// the scalars every script sees, set before every frame
enum Builtin : uint8_t {
    kT,
    kDt,
    kW,
    kH,
    kN,
    kHue,
    kSat,
    kLevel,
    kBeat,
    kBass,
    kMid,
    kHigh,
    kSpeed,
    kBuiltins,
};

constexpr uint8_t kParamBase = 16;
constexpr uint8_t kVarBase = kParamBase + kMaxParams;
constexpr size_t kMaxVars = 64;

// the vectors of the pixels' places, filled before every run of the per-pixel code (as used)
enum PixelValue : uint8_t {
    kX,
    kY,
    kU,
    kV,
    kI,
    kPixelValues,
};

// a vector field of an instruction without a mask
constexpr uint8_t kNoMask = 255;

// --------------------------------------------------------------------------------- instructions

// binary operations: scalar, vector op vector, vector op scalar, scalar op vector
#define GLOW_BINARY(X)                                                                                                \
    X(Add, add) X(Sub, sub) X(Mul, mul) X(Div, div) X(Mod, mod) X(Min, min) X(Max, max) X(Lt, lt) X(Le, le) X(Eq, eq)  \
        X(Ne, ne) X(And, both) X(Or, either) X(Atan2, atan2) X(Hypot, hypot) X(Hash, hash) X(Square, square)           \
            X(MulHi, mulhi)

// unary operations: scalar, vector
#define GLOW_UNARY(X)                                                                                                 \
    X(Mov, mov) X(Neg, neg) X(Not, logical_not) X(Abs, abs) X(Floor, floor) X(Ceil, ceil) X(Frac, frac)               \
        X(Round, round) X(Sqrt, sqrt) X(Sin, sin) X(Cos, cos) X(Wave, wave) X(Tri, tri)

enum class Op : uint8_t {
#define GLOW_BINARY_OPS(name, f) name##S, name##VV, name##VS, name##SV,
#define GLOW_UNARY_OPS(name, f) name##S, name##V,
    GLOW_BINARY(GLOW_BINARY_OPS) GLOW_UNARY(GLOW_UNARY_OPS)
#undef GLOW_BINARY_OPS
#undef GLOW_UNARY_OPS
    // d = a (a scalar into every lane)
    Bcast,
    // d = a != 0 ? b : c
    SelS,
    SelV,
    // d = noise(a, b, c)
    NoiseS,
    NoiseV,
    RandS,
    RandV,
    // d = band a
    BandS,
    BandV,
    // d = array imm [a]; array imm [a] = b
    LdaS,
    LdaV,
    Sta,
    // d = grid imm [a, b]; grid imm [a, b] = c; per pixel: grid imm [x, y] = a where mask b
    LdgS,
    LdgV,
    StgS,
    StgV,
    // to imm; if a == 0; if a != 0
    Jmp,
    Jz,
    Jnz,
    // to imm when no pixel of the chunk is in mask a: a block of a per-pixel if with nothing to do
    Jnone,
    // the pixels' colors where mask e: hsv(a, b, c), rgb(a, b, c), pal(a, b), col(a), heat(a)
    Chsv,
    Crgb,
    Cpal,
    Ccol,
    Cheat,
    // the pen's color, the same
    Phsv,
    Prgb,
    Ppal,
    Pcol,
    Pheat,
    // the trail: fade(a), blur(a), clear(), dot(a, b), line(a, b, c, e), disc(a, b, c), blob(a, b, c)
    Fade,
    Blur,
    Clear,
    Dot,
    Line,
    Disc,
    Blob,
    Count,
};

struct Ins {
    Op op;
    uint8_t d;
    uint8_t a;
    uint8_t b;
    uint8_t c;
    uint8_t e;
    uint16_t imm;
};

static_assert(sizeof(Ins) == 8, "an instruction is 8 bytes");

// What an instruction's fields are, for the checker and the disassembler: the register fields d, a,
// b, c, e ('-' none, 'S' scalar, 'V' vector, 'M' vector or kNoMask), imm ('-', 'J' a jump target,
// 'A' an array, 'G' a grid), where ('*' any code, 'P' only the per-pixel code, 'F' only the
// scalar code: init, frame and what the per-pixel code computes once a frame)
struct Signature {
    const char *name;
    char fields[6];
    char imm;
    char where;
};

const Signature &signature(Op op);

// ---------------------------------------------------------------------------------- the program

constexpr uint32_t kMagic = 0x31574c47; // "GLW1"
constexpr size_t kMaxArrays = 16;
constexpr size_t kMaxArrayWords = 1024;
constexpr size_t kMaxGrids = 8;
// instructions of a program: the examples take 40 to 150; the workspace holds them (10 bytes
// each), and a compile during a backup import shares the heap with the import's tree
constexpr size_t kMaxCode = 512;
constexpr size_t kMaxConsts = 96;

enum Stream : uint8_t {
    kInit,
    kFrame,
    // the per-pixel code's values that are the same for every pixel: once a frame
    kPrologue,
    kBody,
    kStreams,
};

struct Range {
    uint16_t from;
    uint16_t to;
};

enum ProgramFlags : uint8_t {
    kHasPixel = 1,
    // draws on the trail (init, frame)
    kDraws = 2,
    // the per-pixel code writes a grid: it runs row by row
    kRows = 4,
    // takes colors from the palette (pal) or the heat (heat): their tables are made for the frame
    kPalette = 8,
    kHeat = 16,
};

// the tables of a frame: 256 colors each, after the registers in the scratch
constexpr size_t kTableColors = 256;

struct Array {
    uint16_t offset;
    uint16_t size;
};

// The program: this header, the constants (the scalars from const_base on), the instructions and
// the line of each
struct Header {
    uint32_t magic;
    uint32_t size;
    Info info;
    Range streams[kStreams];
    uint16_t code_count;
    uint16_t const_count;
    uint16_t const_base;
    uint16_t scalar_count;
    uint8_t vector_count;
    uint8_t var_count;
    uint8_t array_count;
    uint8_t grid_count;
    uint16_t array_words;
    // bit per PixelValue
    uint8_t pixel_values;
    uint8_t flags;
    // bit per grid written by the per-pixel code (a row buffer each)
    uint8_t grid_rows;
    Array arrays[kMaxArrays];
    uint32_t consts_at;
    uint32_t code_at;
    uint32_t lines_at;
};

inline const Header &header_of(const Program &program)
{
    return *reinterpret_cast<const Header *>(&program);
}

inline const Fix *consts_of(const Header &h)
{
    return reinterpret_cast<const Fix *>(reinterpret_cast<const uint8_t *>(&h) + h.consts_at);
}

inline const Ins *code_of(const Header &h)
{
    return reinterpret_cast<const Ins *>(reinterpret_cast<const uint8_t *>(&h) + h.code_at);
}

inline const uint16_t *lines_of(const Header &h)
{
    return reinterpret_cast<const uint16_t *>(reinterpret_cast<const uint8_t *>(&h) + h.lines_at);
}

// --------------------------------------------------------------------- the memory between frames

// After effects' header (px::effects::detail::Memory: the clock, the random numbers, the sound)
struct State {
    // kMagic once the init code ran
    uint32_t started;
    // t in 1/65536 seconds (the speed's)
    uint32_t time;
    uint32_t random;
    uint16_t stopped_line;
    uint8_t stopped;
    Rgb pen;
};

struct Layout {
    size_t state;
    size_t vars;
    size_t arrays;
    size_t trail;
    size_t grids;
    size_t rows;
    size_t total;
};

Layout layout(const Header &h, uint16_t width, uint16_t height);

// ------------------------------------------------------------------------------- 16.16 math

// Inlined wherever used: the machine's lanes (vm.cpp) must not call a function per pixel
#if defined(_MSC_VER)
#define GLOW_INLINE __forceinline
#else
#define GLOW_INLINE inline __attribute__((always_inline))
#endif

namespace fx {

GLOW_INLINE Fix wrap(int64_t v)
{
    return static_cast<Fix>(static_cast<uint32_t>(static_cast<uint64_t>(v)));
}

GLOW_INLINE Fix mov(Fix a)
{
    return a;
}

GLOW_INLINE Fix add(Fix a, Fix b)
{
    return static_cast<Fix>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}

GLOW_INLINE Fix sub(Fix a, Fix b)
{
    return static_cast<Fix>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}

GLOW_INLINE Fix mul(Fix a, Fix b)
{
    return wrap((static_cast<int64_t>(a) * b) >> 16);
}

// In 32 bits (the chips divide 32 bits in hardware, 64 bits in software): the whole part, then the
// fraction of the rest, exact for a divisor below 1.0, else from its upper 16 bits (within 2^-15).
// Toward 0, wrapping like the multiplication.
GLOW_INLINE Fix div(Fix a, Fix b)
{
    if (b == 0) {
        return 0;
    }
    const uint32_t ua = a < 0 ? 0u - static_cast<uint32_t>(a) : static_cast<uint32_t>(a);
    const uint32_t ub = b < 0 ? 0u - static_cast<uint32_t>(b) : static_cast<uint32_t>(b);
    const uint32_t whole = ua / ub;
    const uint32_t rest = ua - whole * ub;
    uint32_t fraction;
    if (ub <= 0xffffu) {
        fraction = (rest << 16) / ub;
    } else {
        const int shift = std::bit_width(ub) - 16;
        fraction = ((rest >> shift) << 16) / (ub >> shift);
        fraction = fraction > 0xffffu ? 0xffffu : fraction;
    }
    const uint32_t q = (whole << 16) + fraction;
    return (a < 0) != (b < 0) ? static_cast<Fix>(0u - q) : static_cast<Fix>(q);
}

// a times b as a fraction of 2^32, rounded: the compiler's division by a constant over 2 (a / c is
// a mulhi of 2^48 / c, below 2^31: a signed 32 x 32 bit multiplication the chips do in hardware)
GLOW_INLINE Fix mulhi(Fix a, Fix b)
{
    return wrap((static_cast<int64_t>(a) * b + 0x80000000LL) >> 32);
}

// floored: the sign of b (x % 1 is 0..1 for a negative x too)
GLOW_INLINE Fix mod(Fix a, Fix b)
{
    if (b == 0 || b == -1) {
        return 0;
    }
    Fix r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) {
        r = add(r, b);
    }
    return r;
}

GLOW_INLINE Fix min(Fix a, Fix b)
{
    return a < b ? a : b;
}

GLOW_INLINE Fix max(Fix a, Fix b)
{
    return a > b ? a : b;
}

GLOW_INLINE Fix lt(Fix a, Fix b)
{
    return a < b ? kOne : 0;
}

GLOW_INLINE Fix le(Fix a, Fix b)
{
    return a <= b ? kOne : 0;
}

GLOW_INLINE Fix eq(Fix a, Fix b)
{
    return a == b ? kOne : 0;
}

GLOW_INLINE Fix ne(Fix a, Fix b)
{
    return a != b ? kOne : 0;
}

GLOW_INLINE Fix both(Fix a, Fix b)
{
    return a != 0 && b != 0 ? kOne : 0;
}

GLOW_INLINE Fix either(Fix a, Fix b)
{
    return a != 0 || b != 0 ? kOne : 0;
}

GLOW_INLINE Fix neg(Fix a)
{
    return static_cast<Fix>(0u - static_cast<uint32_t>(a));
}

GLOW_INLINE Fix logical_not(Fix a)
{
    return a == 0 ? kOne : 0;
}

GLOW_INLINE Fix abs(Fix a)
{
    return a < 0 ? neg(a) : a;
}

GLOW_INLINE Fix floor(Fix a)
{
    return static_cast<Fix>(static_cast<uint32_t>(a) & 0xffff0000u);
}

GLOW_INLINE Fix ceil(Fix a)
{
    return floor(add(a, 0xffff));
}

GLOW_INLINE Fix frac(Fix a)
{
    return a & 0xffff;
}

GLOW_INLINE Fix round(Fix a)
{
    return floor(add(a, 0x8000));
}

GLOW_INLINE uint32_t isqrt32(uint32_t n)
{
    if (n == 0) {
        return 0;
    }
    uint32_t root = 0;
    // the highest power of 4 not above n
    uint32_t bit = 1u << ((std::bit_width(n) - 1) & ~1u);
    while (bit != 0) {
        if (n >= root + bit) {
            n -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

// the square root of a 64-bit number, the low bits dropped for one above 32 bits
GLOW_INLINE uint32_t isqrt64(uint64_t n)
{
    if (n <= 0xffffffffu) {
        return isqrt32(static_cast<uint32_t>(n));
    }
    // the smallest even shift that leaves 32 bits
    const int shift = (std::bit_width(n) - 31) & ~1;
    return isqrt32(static_cast<uint32_t>(n >> shift)) << (shift / 2);
}

GLOW_INLINE Fix sqrt(Fix a)
{
    if (a <= 0) {
        return 0;
    }
    // sqrt(a) in 16.16 is sqrt(a raw) x 256: the raw value shifted up as far as it fits, by an even
    // number, at most 16
    const int room = (32 - std::bit_width(static_cast<uint32_t>(a))) & ~1;
    const int k = room < 16 ? room : 16;
    return static_cast<Fix>(isqrt32(static_cast<uint32_t>(a) << k) << ((16 - k) / 2));
}

GLOW_INLINE Fix hypot(Fix a, Fix b)
{
    const uint64_t x = static_cast<uint32_t>(abs(a));
    const uint64_t y = static_cast<uint32_t>(abs(b));
    const uint32_t r = isqrt64(x * x + y * y);
    return r > 0x7fffffffu ? 0x7fffffff : static_cast<Fix>(r);
}

// the angle of (x, y) in turns, 0..1 (counterclockwise from the x axis as on paper; the canvas'
// y points down, so on the LEDs clockwise)
GLOW_INLINE Fix atan2(Fix y, Fix x)
{
    if (x == 0 && y == 0) {
        return 0;
    }
    const uint32_t ax = static_cast<uint32_t>(abs(x));
    const uint32_t ay = static_cast<uint32_t>(abs(y));
    const bool steep = ay > ax;
    const uint32_t lo = steep ? ax : ay;
    const uint32_t hi = steep ? ay : ax;
    const int64_t r = static_cast<int64_t>((static_cast<uint64_t>(lo) << 16) / hi);
    // atan(r) / 2 pi = r / 8 + r (1 - r) (0.2447 + 0.0663 r) / 2 pi, within 0.0003 turns
    const int64_t poly = 2552 + ((692 * r) >> 16);
    int64_t a = (r >> 3) + ((((r * (65536 - r)) >> 16) * poly) >> 16);
    a = steep ? 16384 - a : a;
    a = x < 0 ? 32768 - a : a;
    a = y < 0 ? 65536 - a : a;
    return static_cast<Fix>(a & 0xffff);
}

GLOW_INLINE Fix hash(Fix a, Fix b)
{
    uint32_t h = static_cast<uint32_t>(a >> 16) * 0x9e3779b1u ^ static_cast<uint32_t>(b >> 16) * 0x85ebca77u;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    h *= 0x297a2d39u;
    h ^= h >> 15;
    return static_cast<Fix>(h >> 16);
}

// 1 for the first `duty` of every turn, else 0
GLOW_INLINE Fix square(Fix a, Fix duty)
{
    return frac(a) < duty ? kOne : 0;
}

// sin(i * 2 pi / 256) * 32767 for a whole turn and one more: a lookup and a blend, no branches
// (the per-pixel loops)
constexpr int16_t kSine[257] = {
    0,      804,    1608,   2410,   3212,   4011,   4808,   5602,   6393,   7179,   7962,   8739,   9512,
    10278,  11039,  11793,  12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,  18204,  18868,
    19519,  20159,  20787,  21403,  22005,  22594,  23170,  23731,  24279,  24811,  25329,  25832,  26319,
    26790,  27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,  30273,  30571,  30852,  31113,
    31356,  31580,  31785,  31971,  32137,  32285,  32412,  32521,  32609,  32678,  32728,  32757,  32767,
    32757,  32728,  32678,  32609,  32521,  32412,  32285,  32137,  31971,  31785,  31580,  31356,  31113,
    30852,  30571,  30273,  29956,  29621,  29268,  28898,  28510,  28105,  27683,  27245,  26790,  26319,
    25832,  25329,  24811,  24279,  23731,  23170,  22594,  22005,  21403,  20787,  20159,  19519,  18868,
    18204,  17530,  16846,  16151,  15446,  14732,  14010,  13279,  12539,  11793,  11039,  10278,  9512,
    8739,   7962,   7179,   6393,   5602,   4808,   4011,   3212,   2410,   1608,   804,    0,      -804,
    -1608,  -2410,  -3212,  -4011,  -4808,  -5602,  -6393,  -7179,  -7962,  -8739,  -9512,  -10278, -11039,
    -11793, -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530, -18204, -18868, -19519, -20159,
    -20787, -21403, -22005, -22594, -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790, -27245,
    -27683, -28105, -28510, -28898, -29268, -29621, -29956, -30273, -30571, -30852, -31113, -31356, -31580,
    -31785, -31971, -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757, -32767, -32757, -32728,
    -32678, -32609, -32521, -32412, -32285, -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
    -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683, -27245, -26790, -26319, -25832, -25329,
    -24811, -24279, -23731, -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868, -18204, -17530,
    -16846, -16151, -15446, -14732, -14010, -13279, -12539, -11793, -11039, -10278, -9512,  -8739,  -7962,
    -7179,  -6393,  -5602,  -4808,  -4011,  -3212,  -2410,  -1608,  -804,   0,
};

// sin of `angle` (1/65536 of a turn) as -32767..32767
GLOW_INLINE int32_t sin16(uint32_t angle)
{
    const uint32_t i = (angle >> 8) & 255u;
    const auto f = static_cast<int32_t>(angle & 255u);
    return kSine[i] + ((kSine[i + 1] - kSine[i]) * f >> 8);
}

// x in turns
GLOW_INLINE Fix sin(Fix a)
{
    const int32_t s = sin16(static_cast<uint32_t>(a));
    return s * 2 + (s >> 14);
}

GLOW_INLINE Fix cos(Fix a)
{
    return sin(add(a, 16384));
}

// 0..1, starting at 0.5 and rising, a turn a period
GLOW_INLINE Fix wave(Fix a)
{
    const int32_t s = sin16(static_cast<uint32_t>(a)) + 32767;
    return s + (s >> 15);
}

// 0 at 0, 1 at 0.5, a turn a period
GLOW_INLINE Fix tri(Fix a)
{
    const Fix f = frac(a);
    return f < 32768 ? f * 2 : (65536 - f) * 2;
}

// 0..1 as a byte: 0..255
GLOW_INLINE uint8_t byte(Fix a)
{
    return a <= 0 ? 0 : (a >= 65535 ? 255 : static_cast<uint8_t>((a * 255 + 32768) >> 16));
}

GLOW_INLINE Fix from_byte(uint8_t b)
{
    return b * 257;
}

// a turn (the fraction) as a hue byte
GLOW_INLINE uint8_t hue8(Fix a)
{
    return static_cast<uint8_t>(static_cast<uint32_t>(a) >> 8);
}

// a coordinate of noise: 1/256 of a cell, positive for all of -32768..32767
GLOW_INLINE uint32_t noise_coord(Fix a)
{
    return static_cast<uint32_t>((a >> 8) + (1 << 23));
}

GLOW_INLINE Fix noise(Fix x, Fix y, Fix z)
{
    return from_byte(noise8(noise_coord(x), noise_coord(y), noise_coord(z)));
}

// effects::hsv, the same colors, inline: a call per pixel into other flash cost three times the
// color (the ESP32's cache; host tested against effects::hsv)
GLOW_INLINE Rgb hsv(uint8_t hue, uint8_t saturation, uint8_t value)
{
    const uint8_t sector = hue / 43;
    const auto rest = static_cast<uint8_t>((hue - sector * 43) * 6);
    const auto p = static_cast<uint8_t>(value * (255 - saturation) / 255);
    const auto q = static_cast<uint8_t>(value * (255 - saturation * rest / 255) / 255);
    const auto t = static_cast<uint8_t>(value * (255 - saturation * (255 - rest) / 255) / 255);
    switch (sector) {
    case 0:
        return {value, t, p};
    case 1:
        return {q, value, p};
    case 2:
        return {p, value, t};
    case 3:
        return {p, q, value};
    case 4:
        return {t, p, value};
    default:
        return {value, p, q};
    }
}

// effects::detail::scale, inline: a color at `value` / 255
GLOW_INLINE Rgb scale(Rgb c, uint32_t value)
{
    return {static_cast<uint8_t>(c.r * value / 255), static_cast<uint8_t>(c.g * value / 255),
            static_cast<uint8_t>(c.b * value / 255)};
}

} // namespace fx

} // namespace px::effects::glow::detail
