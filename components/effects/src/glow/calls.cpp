// The compiler's calls (compiler.hpp): the built-in functions (math, colors, drawing) and the
// script's own, compiled where they are called.

#include <cstring>

#include "compiler.hpp"

namespace px::effects::glow::detail {

// ----------------------------------------------------------------------------------------- calls

Val Compiler::call(const Token &name, bool want_value)
{
    const std::string_view n = _lex.text(name);
    const int ni = static_cast<int>(n.size());
    // at '('
    advance();
    Val args[kMaxArgs];
    size_t argc = 0;
    while (!_failed && !at(Tok::RParen)) {
        if (argc == kMaxArgs) {
            fail("too many values for '%.*s'", ni, n.data());
            break;
        }
        args[argc++] = expression();
        if (!at(Tok::RParen) && !expect(Tok::Comma, "',' or ')'")) {
            break;
        }
    }
    if (_failed || !expect(Tok::RParen, "')'")) {
        for (size_t i = 0; i < argc; i++) {
            release(args[i]);
        }
        return {};
    }
    const int g = find_global(n);
    if (g >= 0 && _globals[g].kind == Sym::Function) {
        Val result = call_function(_globals[g].id, args, argc, name);
        if (!want_value) {
            release(result);
            result = {};
        }
        return result;
    }
    const BuiltinFunction *f = find_function(n);
    const char *problem = nullptr;
    if (f == nullptr) {
        problem = g >= 0 ? "'%.*s' is no function" : "unknown function '%.*s'";
    } else if (argc < f->min || argc > f->max) {
        fail_at(name.line, name.column, f->min == f->max ? "'%.*s' takes %u values" : "'%.*s' takes %u to %u values",
                ni, n.data(), static_cast<unsigned>(f->min), static_cast<unsigned>(f->max));
    } else if (want_value && !f->value) {
        problem = "'%.*s' gives no value: it is a statement of its own";
    } else if (!want_value && f->value) {
        problem = "'%.*s' alone does nothing: use its value";
    }
    if (problem != nullptr) {
        fail_at(name.line, name.column, problem, ni, n.data());
    }
    if (_failed) {
        for (size_t i = 0; i < argc; i++) {
            release(args[i]);
        }
        return {};
    }
    return call_builtin(f->fn, args, argc, name);
}

Val Compiler::call_builtin(Fn fn, Val *a, size_t argc, const Token &name)
{
    switch (fn) {
    case Fn::Abs:
        return unary(Op::AbsS, a[0]);
    case Fn::Floor:
        return unary(Op::FloorS, a[0]);
    case Fn::Ceil:
        return unary(Op::CeilS, a[0]);
    case Fn::Frac:
    case Fn::Saw:
        return unary(Op::FracS, a[0]);
    case Fn::Round:
        return unary(Op::RoundS, a[0]);
    case Fn::Sqrt:
        return unary(Op::SqrtS, a[0]);
    case Fn::Sin:
        return unary(Op::SinS, a[0]);
    case Fn::Cos:
        return unary(Op::CosS, a[0]);
    case Fn::Wave:
        return unary(Op::WaveS, a[0]);
    case Fn::Tri:
        return unary(Op::TriS, a[0]);
    case Fn::Square:
        return binary(Op::SquareS, a[0], argc > 1 ? a[1] : constant(kOne / 2));
    case Fn::Min:
    case Fn::Max: {
        Val v = a[0];
        for (size_t i = 1; i < argc; i++) {
            v = binary(fn == Fn::Min ? Op::MinS : Op::MaxS, v, a[i]);
        }
        return v;
    }
    case Fn::Clamp:
        return binary(Op::MinS, binary(Op::MaxS, a[0], a[1]), a[2]);
    case Fn::Mix:
        // a + (b - a) t
        return binary(Op::AddS, a[0], binary(Op::MulS, binary(Op::SubS, a[1], borrow(a[0])), a[2]));
    case Fn::Step:
        return binary(Op::LeS, a[0], a[1]);
    case Fn::Smooth: {
        // t = clamp((x - e0) / (e1 - e0), 0, 1); t t (3 - 2 t)
        const Val span = binary(Op::SubS, a[1], borrow(a[0]));
        const Val t = binary(Op::MinS, binary(Op::MaxS, binary(Op::DivS, binary(Op::SubS, a[2], a[0]), span), constant(0)),
                             constant(kOne));
        const Val curve = binary(Op::SubS, constant(3 * kOne), binary(Op::MulS, borrow(t), constant(2 * kOne)));
        const Val r = binary(Op::MulS, binary(Op::MulS, borrow(t), borrow(t)), curve);
        release(t);
        return r;
    }
    case Fn::Atan2:
        return binary(Op::Atan2S, a[0], a[1]);
    case Fn::Hypot:
        return binary(Op::HypotS, a[0], a[1]);
    case Fn::Hash:
        return binary(Op::HashS, a[0], argc > 1 ? a[1] : constant(0));
    case Fn::Noise: {
        Val args[3] = {a[0], argc > 1 ? a[1] : constant(0), argc > 2 ? a[2] : constant(0)};
        if (args[0].kind == Kind::Const && args[1].kind == Kind::Const && args[2].kind == Kind::Const) {
            return constant(fx::noise(args[0].k, args[1].k, args[2].k));
        }
        return nary(Op::NoiseS, Op::NoiseV, args, 3, true);
    }
    case Fn::Rand: {
        Val r;
        if (_context == Context::Pixel) {
            r = {Kind::Vector, alloc_v(), true, false, 0};
            emit(Op::RandV, r.reg);
        } else {
            r = {Kind::Scalar, alloc_s(false), true, false, 0};
            emit(Op::RandS, r.reg);
        }
        return argc == 0 ? r : unary(Op::FloorS, binary(Op::MulS, r, a[0]));
    }
    case Fn::Time:
        return unary(Op::FracS, binary(Op::DivS, Val{Kind::Scalar, kT, false, true, 0}, a[0]));
    case Fn::Band:
        // a band does not change during the frame
        _uses_sound = true;
        return nary(Op::BandS, Op::BandV, a, 1, true);
    case Fn::Hsv:
    case Fn::Rgb:
    case Fn::Pal:
    case Fn::Col:
    case Fn::Heat:
        return color(fn, a, argc);
    default:
        return draw(fn, a, argc, name);
    }
}

Val Compiler::color(Fn fn, Val *a, size_t argc)
{
    // the defaults: hsv(h, 1, 1), pal(p, 1), col(1)
    Val v[3] = {argc > 0 ? a[0] : constant(kOne), argc > 1 ? a[1] : constant(kOne), argc > 2 ? a[2] : constant(kOne)};
    const size_t n = fn == Fn::Hsv || fn == Fn::Rgb ? 3 : (fn == Fn::Pal ? 2 : 1);
    _uses_color = _uses_color || fn == Fn::Col;
    _uses_palette = _uses_palette || fn == Fn::Pal || fn == Fn::Heat;
    _tables = static_cast<uint8_t>(_tables | (fn == Fn::Pal ? kPalette : 0) | (fn == Fn::Heat ? kHeat : 0));
    static constexpr Op kPixel[] = {Op::Chsv, Op::Crgb, Op::Cpal, Op::Ccol, Op::Cheat};
    static constexpr Op kPen[] = {Op::Phsv, Op::Prgb, Op::Ppal, Op::Pcol, Op::Pheat};
    const size_t which = static_cast<size_t>(fn) - static_cast<size_t>(Fn::Hsv);
    uint8_t r[3] = {0, 0, 0};
    if (_context == Context::Pixel) {
        for (size_t i = 0; i < n; i++) {
            r[i] = vreg(v[i]);
        }
        emit(kPixel[which], 0, r[0], r[1], r[2], mask());
    } else {
        for (size_t i = 0; i < n; i++) {
            r[i] = sreg(v[i]);
        }
        emit(kPen[which], 0, r[0], r[1], r[2]);
    }
    for (size_t i = 0; i < 3; i++) {
        release(v[i]);
    }
    return {};
}

Val Compiler::draw(Fn fn, Val *a, size_t argc, const Token &name)
{
    if (_context == Context::Pixel) {
        fail_at(name.line, name.column,
                "drawing is for init and frame: per pixel the color comes from hsv(), rgb(), pal(), col() or heat()");
        for (size_t i = 0; i < argc; i++) {
            release(a[i]);
        }
        return {};
    }
    _draws = true;
    uint8_t r[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < argc; i++) {
        r[i] = sreg(a[i]);
    }
    static constexpr Op kDraw[] = {Op::Fade, Op::Blur, Op::Clear, Op::Dot, Op::Line, Op::Disc, Op::Blob};
    emit(kDraw[static_cast<size_t>(fn) - static_cast<size_t>(Fn::Fade)], 0, r[0], r[1], r[2], r[3]);
    for (size_t i = 0; i < argc; i++) {
        release(a[i]);
    }
    return {};
}

Val Compiler::call_function(uint8_t index, Val *args, size_t argc, const Token &name)
{
    const Function &f = _functions[index];
    const std::string_view n = _lex.text(name);
    const int ni = static_cast<int>(n.size());
    bool ok = argc == f.argc;
    if (!ok) {
        fail_at(name.line, name.column, "'%.*s' takes %u values", ni, n.data(), static_cast<unsigned>(f.argc));
    }
    for (uint8_t i = 0; ok && i < _inlined_count; i++) {
        if (_inlined[i].function == index) {
            fail_at(name.line, name.column, "'%.*s' calls itself: not possible", ni, n.data());
            ok = false;
        }
    }
    if (!ok || _inlined_count == kMaxNesting || !enter()) {
        for (size_t i = 0; i < argc; i++) {
            release(args[i]);
        }
        return {};
    }
    // reads the function's source here, then on after the call
    const Lexer::Mark resume = _tok_mark;
    _inlined[_inlined_count++] = {index, _local_count, _loop_count};
    open_scope();
    for (size_t i = 0; i < argc && !_failed; i++) {
        if (_local_count == kMaxLocals || _decl == kMaxDecls) {
            fail("too many local values at once");
            release(args[i]);
            continue;
        }
        Local &l = _locals[_local_count++];
        std::memcpy(l.name, f.args[i], kMaxName);
        l.depth = _depth;
        l.counter = false;
        l.decl = _decl++;
        l.val = args[i];
        // a temporary value becomes the argument's own, a constant or a named one stays the caller's
        // until written
        l.alias = !args[i].temp;
        l.val.temp = false;
        if (_context == Context::Pixel && varying(l.decl) && args[i].kind != Kind::Vector) {
            const uint8_t r = alloc_v();
            store_vector(r, args[i], false);
            l.val = {Kind::Vector, r, false, false, 0};
            l.alias = false;
        }
    }
    _lex.reset(f.body);
    advance();
    Val result = constant(0);
    if (expect(Tok::LBrace, "'{'")) {
        statements(true, &result);
        expect(Tok::RBrace, "'}'");
    }
    // a result in one of the function's own registers outlives them as a copy
    if (!_failed && !result.temp && result.kind != Kind::Const) {
        for (uint8_t i = _inlined[_inlined_count - 1].locals; i < _local_count; i++) {
            const Local &l = _locals[i];
            if (!l.alias && l.val.kind == result.kind && l.val.reg == result.reg) {
                const bool vector = result.kind == Kind::Vector;
                const uint8_t d = vector ? alloc_v() : alloc_s(false);
                emit(vector ? Op::MovV : Op::MovS, d, result.reg);
                result = {result.kind, d, true, false, 0};
                break;
            }
        }
    }
    close_scope(&result);
    _inlined_count--;
    leave();
    _lex.reset(resume);
    advance();
    return result;
}

} // namespace px::effects::glow::detail
