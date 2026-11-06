// components/effects/src/glow: the language (docs/glow.md), its errors, the 16.16 math, exact
// pixels of small scripts, the per-pixel code on 1 and on 32 pixels at once, the step budget, the
// memory and the example scripts of web/glow.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "check.hpp"
#include "glow/bytecode.hpp"
#include "px/effects/effects.hpp"
#include "px/effects/glow.hpp"

using namespace px::effects;
namespace fx = px::effects::glow::detail::fx;
using px::effects::glow::detail::kOne;

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Compiled {
    std::vector<uint8_t> bytes;
    const glow::Program *program = nullptr;
    glow::Error error{};
};

Compiled build(const std::string &source)
{
    std::vector<uint8_t> workspace(glow::workspace_size());
    Compiled c;
    const size_t size = glow::compile(source, workspace.data(), c.error);
    if (size != 0) {
        c.bytes.assign(workspace.begin(), workspace.begin() + static_cast<std::ptrdiff_t>(size));
        c.program = glow::program(c.bytes.data(), c.bytes.size());
    }
    return c;
}

// a canvas with its memory and the machine's registers
struct Stage {
    uint16_t width;
    uint16_t height;
    std::vector<Rgb> pixels;
    std::vector<Rgb> spare;
    std::vector<uint8_t> memory;
    std::vector<uint8_t> scratch;
    Canvas canvas;
    Params params;
    uint8_t sliders[glow::kMaxParams] = {};

    Stage(uint16_t w, uint16_t h, size_t memory_size = 0)
        : width(w), height(h), pixels(static_cast<size_t>(w) * h), spare(static_cast<size_t>(w) * h),
          memory(memory_size != 0 ? memory_size : memory_size_of(w, h)), scratch(glow::scratch_size()),
          canvas{w, h, pixels.data(), memory.data(), memory.size(), spare.data()}
    {
    }

    static size_t memory_size_of(uint16_t w, uint16_t h) { return px::effects::memory_size(w, h); }

    glow::Result draw(const Compiled &c, uint32_t ms)
    {
        return glow::render(*c.program, canvas, params, sliders, ms, scratch.data());
    }

    const Rgb &at(uint16_t x, uint16_t y) const { return pixels[static_cast<size_t>(y) * width + x]; }
};

bool same(Rgb a, Rgb b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

std::string script(const char *body)
{
    return std::string("anim \"test\"\n") + body;
}

// the error of a source that does not compile: "line:column text"
std::string error_of(const std::string &source)
{
    const Compiled c = build(source);
    if (c.program != nullptr) {
        return "compiled";
    }
    char text[128];
    std::snprintf(text, sizeof(text), "%u:%u %s", static_cast<unsigned>(c.error.line),
                  static_cast<unsigned>(c.error.column), c.error.text);
    return text;
}

bool contains(const std::string &text, const char *part)
{
    return text.find(part) != std::string::npos;
}

double real(int32_t f)
{
    return f / 65536.0;
}

int32_t fix(double d)
{
    return static_cast<int32_t>(std::lround(d * 65536.0));
}

} // namespace

TEST(glow_compiles_and_draws)
{
    const Compiled c = build(script("pixel {\n  hsv(u + v + t, 1, 1)\n}\n"));
    REQUIRE(c.program != nullptr);
    CHECK_STR(glow::info(*c.program).name, "test");
    CHECK(glow::info(*c.program).reactive == Reactive::Never);
    Stage s(8, 4);
    CHECK(s.draw(c, 0).status == glow::Status::Ok);
    bool lit = false;
    for (const Rgb &p : s.pixels) {
        lit = lit || p.r != 0 || p.g != 0 || p.b != 0;
    }
    CHECK(lit);
}

TEST(glow_errors_have_a_place)
{
    CHECK_STR(error_of("pixel { }"), "1:1 the script starts with its name: anim \"My animation\"");
    CHECK_STR(error_of(script("pixel {\n  hsb(1)\n}")), "3:3 unknown function 'hsb'");
    CHECK_STR(error_of(script("frame {\n  x = 1\n}")), "3:3 'x' is a built-in name");
    CHECK_STR(error_of(script("frame {\n  a = b\n}")), "3:7 unknown name 'b'");
    CHECK_STR(error_of(script("pixel {\n  while 1 { }\n}")), "3:3 while is not possible per pixel: use for");
    CHECK(contains(error_of(script("pixel {\n  dot(1, 2)\n}")), "3:3 drawing is for init and frame"));
    CHECK(contains(error_of(script("var p[4]\npixel {\n  p[0] = 1\n}")), "4:3 an array cannot change per pixel"));
    CHECK(contains(error_of(script("var a\npixel {\n  a = 1\n}")), "4:3 a variable cannot change per pixel"));
    CHECK(contains(error_of(script("fn f(a) {\n  return f(a)\n}\npixel { hsv(f(1)) }")), "calls itself"));
    CHECK(contains(error_of(script("pixel {\n  hsv(1)\n")), "no closing '}'"));
    CHECK(contains(error_of(script("frame { a = 40000 }")), "the number is too large"));
    CHECK(contains(error_of(script("frame { a = 1 +\n}")), "expected a value"));
    CHECK(contains(error_of(script("frame { hsv(1) hsv(2) }")), "expected the end of the line"));
    CHECK(contains(error_of(script("param s = 2 [0 .. 1]")), "outside its range"));
    CHECK(contains(error_of(script("pixel { for k in 0 .. x { } }")), "the same for every pixel"));
    CHECK(contains(error_of(script("pixel { if x < 1 { break } }")), "break outside a loop"));
    CHECK(contains(error_of(script("pixel { for k in 0 .. 2 { if x < 1 { break } } }")),
                   "under a per-pixel condition"));
    CHECK(contains(error_of(script("fn f() {\n  return 1\n  hsv(1)\n}\nframe { f() }")),
                   "return is the last statement"));
    CHECK(contains(error_of(script("pixel { a = sin(1) }")), "compiled"));
    CHECK(contains(error_of(script("pixel { sin(1) }")), "alone does nothing"));
    CHECK(contains(error_of(script("pixel { a = hsv(1) }")), "gives no value"));
    CHECK(contains(error_of(script("grid g\npixel { g[x + 1, y] = 1 }")), "at [x, y] only"));
    CHECK(contains(error_of(script("frame { if 1 { a = 1 }\n b = a }")), "unknown name 'a'"));
    CHECK(contains(error_of(script("pixel { f = (((((((((((((((((1))))))))))))))))) }")), "nested too deeply"));
}

TEST(glow_math_against_double)
{
    for (double a = -3.7; a < 3.7; a += 0.173) {
        CHECK(std::fabs(real(fx::sin(fix(a))) - std::sin(a * 2 * kPi)) < 0.001);
        CHECK(std::fabs(real(fx::cos(fix(a))) - std::cos(a * 2 * kPi)) < 0.001);
        CHECK(std::fabs(real(fx::wave(fix(a))) - (0.5 + 0.5 * std::sin(a * 2 * kPi))) < 0.001);
        CHECK(real(fx::floor(fix(a))) == std::floor(real(fix(a))));
        CHECK(std::fabs(real(fx::frac(fix(a))) - (real(fix(a)) - std::floor(real(fix(a))))) < 1e-9);
        for (double b = -2.9; b < 2.9; b += 0.37) {
            // against the inputs as 16.16 has them
            const double qa = real(fix(a));
            const double qb = real(fix(b));
            CHECK(std::fabs(real(fx::mul(fix(a), fix(b))) - qa * qb) < 0.0001);
            CHECK(std::fabs(real(fx::div(fix(a), fix(b))) - qa / qb) < 0.0001);
            CHECK(std::fabs(real(fx::hypot(fix(a), fix(b))) - std::hypot(a, b)) < 0.0002);
            double turns = std::atan2(b, a) / (2 * kPi);
            turns -= std::floor(turns);
            double got = real(fx::atan2(fix(b), fix(a)));
            double diff = std::fabs(got - turns);
            CHECK(std::fmin(diff, 1 - diff) < 0.0005);
            const double m = real(fx::mod(fix(a), fix(b)));
            CHECK(std::fabs(m - (qa - qb * std::floor(qa / qb))) < 0.0001);
        }
    }
    for (double a = 0; a < 30000; a = a * 1.7 + 0.01) {
        CHECK(std::fabs(real(fx::sqrt(fix(a))) - std::sqrt(real(fix(a)))) < 0.001 + std::sqrt(a) * 0.0002);
    }
    CHECK(fx::div(kOne, 0) == 0);
    CHECK(fx::mod(kOne, 0) == 0);
    CHECK(fx::sqrt(-kOne) == 0);
    CHECK(fx::byte(fx::from_byte(77)) == 77);
    CHECK(fx::byte(-5) == 0 && fx::byte(2 * kOne) == 255);
}

TEST(glow_exact_pixels)
{
    // u and v are the pixels' middles, 0..1
    {
        const Compiled c = build(script("pixel { rgb(u, v, 0) }"));
        REQUIRE(c.program != nullptr);
        Stage s(4, 2);
        REQUIRE(s.draw(c, 0).status == glow::Status::Ok);
        for (uint16_t y = 0; y < 2; y++) {
            for (uint16_t x = 0; x < 4; x++) {
                CHECK(s.at(x, y).r == fx::byte(static_cast<int32_t>((2 * x + 1) * 65536 / 8)));
                CHECK(s.at(x, y).g == fx::byte(static_cast<int32_t>((2 * y + 1) * 65536 / 4)));
            }
        }
    }
    // a condition per pixel, else, a local that becomes per pixel in a loop, a function, ? :
    {
        const Compiled c = build(script("fn twice(a) {\n  return a * 2\n}\n"
                                        "pixel {\n"
                                        "  d = 0\n"
                                        "  for k in 0 .. 3 { d += x }\n"
                                        "  if x < 2 {\n    rgb(1, d / 10, 0)\n  }\n  else {\n    rgb(0, d / 10, twice(y) / 4)\n  }\n"
                                        "}\n"));
        REQUIRE(c.program != nullptr);
        Stage s(4, 2);
        REQUIRE(s.draw(c, 0).status == glow::Status::Ok);
        for (uint16_t y = 0; y < 2; y++) {
            for (uint16_t x = 0; x < 4; x++) {
                const Rgb p = s.at(x, y);
                CHECK(p.r == (x < 2 ? 255 : 0));
                // a division by a constant is a multiplication: within a step of the division
                CHECK(std::abs(p.g - fx::byte(fx::div(3 * x * kOne, 10 * kOne))) <= 1);
                CHECK(p.b == (x < 2 ? 0 : fx::byte(y * 2 * kOne / 4)));
            }
        }
    }
    // a local set under a condition per pixel keeps the others' value
    {
        const Compiled c = build(script("pixel {\n  a = 0.25\n  if y > 0 { a = 1 }\n  rgb(a, x > 1 ? 1 : 0, 0)\n}"));
        REQUIRE(c.program != nullptr);
        Stage s(4, 2);
        REQUIRE(s.draw(c, 0).status == glow::Status::Ok);
        CHECK(s.at(0, 0).r == 64 && s.at(3, 0).r == 64 && s.at(0, 1).r == 255);
        CHECK(s.at(1, 0).g == 0 && s.at(2, 1).g == 255);
    }
    // only the pixels a color is given to change: the rest stays dark
    {
        const Compiled c = build(script("pixel { if i == 5 { rgb(1, 1, 1) } }"));
        REQUIRE(c.program != nullptr);
        Stage s(4, 2);
        REQUIRE(s.draw(c, 0).status == glow::Status::Ok);
        for (uint16_t k = 0; k < 8; k++) {
            CHECK(same(s.pixels[k], k == 5 ? Rgb{255, 255, 255} : Rgb{0, 0, 0}));
        }
    }
}

TEST(glow_hsv_is_effects_hsv)
{
    // the machine's copy (inline for the per-pixel loops) gives the same colors
    for (int h = 0; h < 256; h++) {
        for (int s = 0; s < 256; s += 5) {
            for (int v = 0; v < 256; v += 5) {
                const auto hue = static_cast<uint8_t>(h);
                const auto sat = static_cast<uint8_t>(s);
                const auto val = static_cast<uint8_t>(v);
                if (!same(fx::hsv(hue, sat, val), hsv(hue, sat, val))) {
                    CHECK(same(fx::hsv(hue, sat, val), hsv(hue, sat, val)));
                    return;
                }
            }
        }
    }
}

TEST(glow_division_by_a_constant)
{
    // compiled as a multiplication by the reciprocal: within 2^-15 of the division
    const double divisors[] = {1.5, 3, 7, 10, 100, 3000, 0.3, 0.07, 0.5, -3, -0.25};
    for (const double c : divisors) {
        char text[96];
        std::snprintf(text, sizeof(text), "var q\nframe { q = (t + 5.5) / %.4f }", c);
        const Compiled program = build(script(text));
        REQUIRE(program.program != nullptr);
        Stage s(2, 1);
        REQUIRE(s.draw(program, 0).status == glow::Status::Ok);
        const auto &h = glow::detail::header_of(*program.program);
        int32_t q = 0;
        std::memcpy(&q, s.memory.data() + glow::detail::layout(h, 2, 1).vars, sizeof(q));
        const double expected = 5.5 / (std::lround(c * 65536.0) / 65536.0);
        CHECK(std::fabs(real(q) - expected) < 0.00004 + std::fabs(expected) * 0.00002);
    }
}

TEST(glow_variables_and_arrays_keep_their_values)
{
    const Compiled c = build(script("var n, p[4]\n"
                                    "init { p[1] = 0.5 }\n"
                                    "frame {\n  n += 1\n  p[0] = n / 10\n}\n"
                                    "pixel { rgb(p[0], p[1], p[x]) }"));
    REQUIRE(c.program != nullptr);
    Stage s(4, 1);
    for (uint32_t f = 0; f < 3; f++) {
        REQUIRE(s.draw(c, f * 10).status == glow::Status::Ok);
    }
    CHECK(std::abs(s.at(0, 0).r - fx::byte(fx::div(3 * kOne, 10 * kOne))) <= 1);
    CHECK(s.at(0, 0).g == fx::byte(kOne / 2));
    CHECK(s.at(1, 0).b == fx::byte(kOne / 2));
    CHECK(s.at(3, 0).b == 0);
}

TEST(glow_one_pixel_at_a_time_is_the_same)
{
    // the same script on a 1 x 40 canvas run pixel by pixel (a grid written: runs of a row) and on a
    // 40 x 1 canvas run 32 pixels at once: the same colors (i is the same on both)
    const char *body = "fn bump(a, b) {\n  c = a * b\n  if c > 0.3 { c = c * 0.5 }\n  return c + 0.1\n}\n"
                       "pixel {\n"
                       "  f = i / 40\n"
                       "  s = 0\n"
                       "  for k in 0 .. 4 {\n    s += wave(f * (k + 1) + t)\n    if f > k / 4 { s -= 0.1 }\n  }\n"
                       "  g = f < 0.5 ? sqrt(f) : hypot(f, 0.3)\n"
                       "  if f > 0.2 {\n    if f < 0.7 { g = bump(f, s) } else { g = noise(f * 3, s, 0.5) }\n  }\n"
                       "  hsv(s / 4, 1 - g / 2, max(g, 0.2))\n"
                       "%s}\n";
    char a[1024];
    char b[1024];
    std::snprintf(a, sizeof(a), body, "  w[x, y] = 0\n");
    std::snprintf(b, sizeof(b), body, "");
    const Compiled one = build(script((std::string("grid w\n") + a).c_str()));
    const Compiled many = build(script(b));
    REQUIRE(one.program != nullptr);
    REQUIRE(many.program != nullptr);
    Stage tall(1, 40);
    Stage wide(40, 1);
    for (uint32_t ms = 0; ms < 2000; ms += 500) {
        REQUIRE(tall.draw(one, ms).status == glow::Status::Ok);
        REQUIRE(wide.draw(many, ms).status == glow::Status::Ok);
        for (size_t k = 0; k < 40; k++) {
            CHECK(same(tall.pixels[k], wide.pixels[k]));
        }
    }
}

TEST(glow_endless_loops_stop_the_script_not_the_firmware)
{
    const Compiled frame_loop = build(script("frame {\n  a = 0\n  while 1 { a += 1 }\n}"));
    REQUIRE(frame_loop.program != nullptr);
    Stage s(4, 2);
    s.pixels[0] = {9, 9, 9};
    glow::Result r = s.draw(frame_loop, 0);
    CHECK(r.status == glow::Status::Stopped);
    CHECK(r.line == 4);
    CHECK(same(s.pixels[0], {0, 0, 0}));
    // stays stopped, dark
    CHECK(s.draw(frame_loop, 10).status == glow::Status::Stopped);
    // on fresh memory it runs again (and stops again)
    std::fill(s.memory.begin(), s.memory.end(), uint8_t{0});
    CHECK(s.draw(frame_loop, 20).status == glow::Status::Stopped);

    const Compiled pixel_loop = build(script("pixel {\n  s = 0\n  for k in 0 .. 30000 { s += 1 }\n  rgb(s, 0, 0)\n}"));
    REQUIRE(pixel_loop.program != nullptr);
    Stage p(4, 2);
    r = p.draw(pixel_loop, 0);
    CHECK(r.status == glow::Status::Stopped);
    CHECK(r.line == 4);
    // a loop within the budget runs
    const Compiled fine = build(script("pixel {\n  s = 0\n  for k in 0 .. 50 { s += 0.01 }\n  rgb(s, 0, 0)\n}"));
    REQUIRE(fine.program != nullptr);
    Stage q(4, 2);
    CHECK(q.draw(fine, 0).status == glow::Status::Ok);
    CHECK(q.at(0, 0).r == fx::byte(50 * fix(0.01)));
}

TEST(glow_memory_need)
{
    const Compiled c = build(script("grid a, b\nvar p[100]\npixel {\n  a[x, y] = 1\n  rgb(b[x, y], 0, 0)\n}"));
    REQUIRE(c.program != nullptr);
    const size_t need = glow::memory_need(*c.program, 24, 32);
    CHECK(need > 2 * 24 * 32 + 400);
    Stage small(24, 32, need - 1);
    const glow::Result r = small.draw(c, 0);
    CHECK(r.status == glow::Status::NoMemory);
    CHECK(r.need == need);
    Stage enough(24, 32, need);
    CHECK(enough.draw(c, 0).status == glow::Status::Ok);
}

TEST(glow_grid_written_per_pixel_takes_a_row_a_frame)
{
    // the rows below as they were, the written rows above: heat rises a row a frame
    const Compiled c =
        build(script("grid g\nframe { g[0, 3] = 1 }\npixel {\n  h = g[x, y + 1]\n  g[x, y] = h\n  rgb(h, 0, 0)\n}"));
    REQUIRE(c.program != nullptr);
    Stage s(1, 4);
    REQUIRE(s.draw(c, 0).status == glow::Status::Ok);
    CHECK(s.at(0, 0).r == 0 && s.at(0, 1).r == 0 && s.at(0, 2).r == 255 && s.at(0, 3).r == 255);
    REQUIRE(s.draw(c, 10).status == glow::Status::Ok);
    CHECK(s.at(0, 0).r == 0 && s.at(0, 1).r == 255);
    REQUIRE(s.draw(c, 20).status == glow::Status::Ok);
    CHECK(s.at(0, 0).r == 255);
}

TEST(glow_trail_drawing)
{
    const Compiled c =
        build(script("frame {\n  clear()\n  rgb(1, 0, 0)\n  dot(1, 0)\n  hsv(0.66, 1, 1)\n  line(0, 1, 3, 1)\n}"));
    REQUIRE(c.program != nullptr);
    Stage s(4, 2);
    REQUIRE(s.draw(c, 0).status == glow::Status::Ok);
    CHECK(same(s.at(1, 0), {255, 0, 0}));
    CHECK(same(s.at(0, 0), {0, 0, 0}));
    for (uint16_t x = 0; x < 4; x++) {
        CHECK(s.at(x, 1).b > 200 && s.at(x, 1).r < 30);
    }
    // the trail stays and fades: without clear() the dot fades a half a frame
    const Compiled fading = build(script("init {\n  rgb(1, 1, 1)\n  dot(0, 0)\n}\nframe { fade(0.5) }"));
    REQUIRE(fading.program != nullptr);
    Stage f(2, 1);
    REQUIRE(f.draw(fading, 0).status == glow::Status::Ok);
    CHECK(f.at(0, 0).r == 127);
    REQUIRE(f.draw(fading, 10).status == glow::Status::Ok);
    CHECK(f.at(0, 0).r == 63);
}

TEST(glow_params_and_sliders)
{
    const Compiled c = build(
        script("param level2 = 0.25 [0 .. 1] \"Level\"\nparam n = 3 [1 .. 8 step 1]\npixel { rgb(level2, n / 8, 0) }"));
    REQUIRE(c.program != nullptr);
    const glow::Info &info = glow::info(*c.program);
    REQUIRE(info.param_count == 2);
    CHECK_STR(info.params[0].label, "Level");
    CHECK_STR(info.params[1].label, "n");
    CHECK(glow::param_slider(info.params[0], info.params[0].initial) == 64);
    CHECK(glow::param_value(info.params[0], 255) == kOne);
    CHECK(glow::param_value(info.params[0], 0) == 0);
    // a step: whole values only
    for (int slider = 0; slider < 256; slider++) {
        CHECK(fx::frac(glow::param_value(info.params[1], static_cast<uint8_t>(slider))) == 0);
    }
    CHECK(glow::param_value(info.params[1], 255) == 8 * kOne);
    Stage s(2, 1);
    s.sliders[0] = 255;
    s.sliders[1] = glow::param_slider(info.params[1], 4 * kOne);
    REQUIRE(s.draw(c, 0).status == glow::Status::Ok);
    CHECK(s.at(0, 0).r == 255);
    CHECK(s.at(0, 0).g == 128);
}

TEST(glow_turned_by_quarters)
{
    const Compiled c = build(script("pixel { if x == 0 and y == 0 { rgb(1, 1, 1) } }"));
    REQUIRE(c.program != nullptr);
    Stage s(4, 2);
    s.params.rotation = 1;
    REQUIRE(s.draw(c, 0).status == glow::Status::Ok);
    // clockwise: the top left lands top right
    CHECK(same(s.at(3, 0), {255, 255, 255}));
    CHECK(same(s.at(0, 0), {0, 0, 0}));
    s.params.rotation = 2;
    REQUIRE(s.draw(c, 10).status == glow::Status::Ok);
    CHECK(same(s.at(3, 1), {255, 255, 255}));
}

TEST(glow_sound_and_colors_are_found)
{
    const Compiled none = build(script("pixel { hsv(u, 1, 1) }"));
    const Compiled hears = build(script("pixel { rgb(level, level, level) }"));
    const Compiled needs = build(script("sound required\npixel { pal(u, band(x)) }"));
    const Compiled colored = build(script("pixel { col(u) }"));
    REQUIRE(none.program != nullptr && hears.program != nullptr && needs.program != nullptr &&
            colored.program != nullptr);
    CHECK(glow::info(*none.program).reactive == Reactive::Never);
    CHECK(glow::info(*hears.program).reactive == Reactive::Optional);
    CHECK(glow::info(*needs.program).reactive == Reactive::Required);
    CHECK(glow::info(*needs.program).uses_palette);
    CHECK(!glow::info(*none.program).uses_palette && !glow::info(*none.program).uses_color);
    CHECK(glow::info(*colored.program).uses_color);
    // the level follows the audio only while it reacts
    Audio audio;
    audio.active = true;
    audio.loudness = 255;
    Stage s(2, 1);
    s.params.audio = &audio;
    s.params.attack_ms = 0;
    s.params.reactive = false;
    REQUIRE(s.draw(hears, 0).status == glow::Status::Ok);
    CHECK(same(s.at(0, 0), {0, 0, 0}));
    s.params.reactive = true;
    REQUIRE(s.draw(hears, 10).status == glow::Status::Ok);
    REQUIRE(s.draw(hears, 20).status == glow::Status::Ok);
    CHECK(s.at(0, 0).r > 200);
}

TEST(glow_program_check)
{
    const Compiled c = build(script("var p[4]\nframe { p[1] = t }\npixel { hsv(u + p[1], 1, x > 1 ? 1 : 0.5) }"));
    REQUIRE(c.program != nullptr);
    CHECK(glow::program(c.bytes.data(), c.bytes.size() - 1) == nullptr);
    std::vector<uint8_t> bad = c.bytes;
    bad[0] ^= 1;
    CHECK(glow::program(bad.data(), bad.size()) == nullptr);
    // every single byte changed: a valid program or none, never a crash when drawn
    Stage s(4, 2);
    size_t rejected = 0;
    for (size_t i = 0; i < c.bytes.size(); i++) {
        bad = c.bytes;
        bad[i] = static_cast<uint8_t>(bad[i] + 0x5b);
        const glow::Program *p = glow::program(bad.data(), bad.size());
        if (p == nullptr) {
            rejected++;
            continue;
        }
        s.memory.assign(s.memory.size(), 0);
        (void)glow::render(*p, s.canvas, s.params, s.sliders, 0, s.scratch.data());
    }
    CHECK(rejected > 0);
}

TEST(glow_disassembles)
{
    const Compiled c = build(script("frame { a = 1 }\npixel { hsv(u, 1, 1) }"));
    REQUIRE(c.program != nullptr);
    std::string text;
    glow::disassemble(
        *c.program,
        [](const char *line, void *context) {
            *static_cast<std::string *>(context) += line;
            *static_cast<std::string *>(context) += "\n";
        },
        &text);
    CHECK(contains(text, "test: "));
    CHECK(contains(text, "hsv"));
    CHECK(contains(text, "; line 3"));
}

TEST(glow_examples_run)
{
    // web/glow: every example compiles and draws on the test matrix' 24 x 32
    const std::filesystem::path dir = std::filesystem::path(PX_SOURCE_ROOT) / "web" / "glow";
    size_t examples = 0;
    for (const auto &entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() != ".glow") {
            continue;
        }
        examples++;
        const std::string name = entry.path().filename().string();
        std::ifstream file(entry.path(), std::ios::binary);
        std::stringstream text;
        text << file.rdbuf();
        const Compiled c = build(text.str());
        if (c.program == nullptr) {
            ::px::test::fail(__FILE__, __LINE__,
                             name + ": " + std::to_string(c.error.line) + ":" + std::to_string(c.error.column) + " " +
                                 c.error.text);
            continue;
        }
        const glow::Info &info = glow::info(*c.program);
        Audio audio;
        audio.active = true;
        Stage s(24, 32);
        s.params.audio = &audio;
        s.params.reactive = true;
        for (size_t k = 0; k < info.param_count; k++) {
            s.sliders[k] = glow::param_slider(info.params[k], info.params[k].initial);
        }
        bool lit = false;
        for (uint32_t frame = 0; frame < 100; frame++) {
            audio.loudness = static_cast<uint8_t>(frame * 37);
            audio.beat = frame % 10 == 0;
            const glow::Result r = s.draw(c, frame * 16);
            if (r.status != glow::Status::Ok) {
                ::px::test::fail(__FILE__, __LINE__,
                                 name + ": not ok at frame " + std::to_string(frame) + ", line " +
                                     std::to_string(r.line));
                break;
            }
            for (const Rgb &p : s.pixels) {
                lit = lit || p.r != 0 || p.g != 0 || p.b != 0;
            }
        }
        if (!lit) {
            ::px::test::fail(__FILE__, __LINE__, name + ": all dark");
        }
    }
    CHECK(examples >= 5);
}
