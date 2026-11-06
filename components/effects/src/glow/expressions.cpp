// The compiler's values (compiler.hpp): expressions read with an operator stack (no recursion per
// operator), names and the built-in values, the elements of arrays and grids. Calls are in
// calls.cpp, the operations and registers in registers.cpp.

#include <cstring>

#include "compiler.hpp"

namespace px::effects::glow::detail {

namespace {

enum Use : uint8_t {
    kPlain,
    kSound,
    kColor,
};

struct BuiltinValue {
    const char *name;
    uint8_t reg;
    bool pixel;
    Use use;
};

constexpr BuiltinValue kValues[] = {
    {"t", kT, false, kPlain},       {"dt", kDt, false, kPlain},     {"W", kW, false, kPlain},
    {"H", kH, false, kPlain},       {"N", kN, false, kPlain},       {"hue", kHue, false, kColor},
    {"sat", kSat, false, kColor},   {"level", kLevel, false, kSound}, {"beat", kBeat, false, kSound},
    {"bass", kBass, false, kSound}, {"mid", kMid, false, kSound},   {"high", kHigh, false, kSound},
    {"speed", kSpeed, false, kPlain}, {"x", kX, true, kPlain},      {"y", kY, true, kPlain},
    {"u", kU, true, kPlain},        {"v", kV, true, kPlain},        {"i", kI, true, kPlain},
};

const BuiltinValue *find_value(std::string_view name)
{
    for (const BuiltinValue &v : kValues) {
        if (name == v.name) {
            return &v;
        }
    }
    return nullptr;
}

} // namespace

constexpr BuiltinFunction kFunctions[] = {
    {"abs", Fn::Abs, 1, 1, true},       {"floor", Fn::Floor, 1, 1, true},   {"ceil", Fn::Ceil, 1, 1, true},
    {"frac", Fn::Frac, 1, 1, true},     {"round", Fn::Round, 1, 1, true},   {"sqrt", Fn::Sqrt, 1, 1, true},
    {"sin", Fn::Sin, 1, 1, true},       {"cos", Fn::Cos, 1, 1, true},       {"wave", Fn::Wave, 1, 1, true},
    {"tri", Fn::Tri, 1, 1, true},       {"saw", Fn::Saw, 1, 1, true},       {"square", Fn::Square, 1, 2, true},
    {"min", Fn::Min, 2, 4, true},       {"max", Fn::Max, 2, 4, true},       {"clamp", Fn::Clamp, 3, 3, true},
    {"mix", Fn::Mix, 3, 3, true},       {"step", Fn::Step, 2, 2, true},     {"smooth", Fn::Smooth, 3, 3, true},
    {"atan2", Fn::Atan2, 2, 2, true},   {"hypot", Fn::Hypot, 2, 2, true},   {"hash", Fn::Hash, 1, 2, true},
    {"noise", Fn::Noise, 1, 3, true},   {"rand", Fn::Rand, 0, 1, true},     {"time", Fn::Time, 1, 1, true},
    {"band", Fn::Band, 1, 1, true},     {"hsv", Fn::Hsv, 1, 3, false},      {"rgb", Fn::Rgb, 3, 3, false},
    {"pal", Fn::Pal, 1, 2, false},      {"col", Fn::Col, 0, 1, false},      {"heat", Fn::Heat, 1, 1, false},
    {"fade", Fn::Fade, 1, 1, false},    {"blur", Fn::Blur, 1, 1, false},    {"clear", Fn::Clear, 0, 0, false},
    {"dot", Fn::Dot, 2, 2, false},      {"line", Fn::Line, 4, 4, false},    {"disc", Fn::Disc, 3, 3, false},
    {"blob", Fn::Blob, 3, 3, false},
};


const BuiltinFunction *find_function(std::string_view name)
{
    for (const BuiltinFunction &f : kFunctions) {
        if (name == f.name) {
            return &f;
        }
    }
    return nullptr;
}


bool builtin_name(std::string_view name)
{
    return find_value(name) != nullptr || find_function(name) != nullptr;
}

// ---------------------------------------------------------------------------------- expressions

Val Compiler::expression()
{
    Val v = binary_expression();
    if (!_failed && accept(Tok::Question)) {
        if (!enter()) {
            release(v);
            return {};
        }
        skip_newlines();
        Val a = expression();
        skip_newlines();
        if (!expect(Tok::Colon, "':' of '? :'")) {
            release(v);
            release(a);
            leave();
            return {};
        }
        skip_newlines();
        Val b = expression();
        v = select(v, a, b);
        leave();
    }
    return v;
}

namespace {

// the binary operator at `t` (`word` for and, or), its precedence; `swap` for > and >=
bool binary_operator(Tok t, std::string_view word, Op &op, uint8_t &precedence, bool &swap)
{
    swap = false;
    switch (t) {
    case Tok::Plus:
        op = Op::AddS, precedence = 5;
        return true;
    case Tok::Minus:
        op = Op::SubS, precedence = 5;
        return true;
    case Tok::Star:
        op = Op::MulS, precedence = 6;
        return true;
    case Tok::Slash:
        op = Op::DivS, precedence = 6;
        return true;
    case Tok::Percent:
        op = Op::ModS, precedence = 6;
        return true;
    case Tok::Less:
        op = Op::LtS, precedence = 4;
        return true;
    case Tok::LessEq:
        op = Op::LeS, precedence = 4;
        return true;
    case Tok::Greater:
        op = Op::LtS, precedence = 4, swap = true;
        return true;
    case Tok::GreaterEq:
        op = Op::LeS, precedence = 4, swap = true;
        return true;
    case Tok::Equal:
        op = Op::EqS, precedence = 4;
        return true;
    case Tok::NotEqual:
        op = Op::NeS, precedence = 4;
        return true;
    case Tok::AndAnd:
        op = Op::AndS, precedence = 2;
        return true;
    case Tok::OrOr:
        op = Op::OrS, precedence = 1;
        return true;
    case Tok::Name:
        if (word == "and") {
            op = Op::AndS, precedence = 2;
            return true;
        }
        if (word == "or") {
            op = Op::OrS, precedence = 1;
            return true;
        }
        return false;
    default:
        return false;
    }
}

} // namespace

Val Compiler::binary_expression()
{
    const uint8_t values = _value_count;
    const uint8_t pending = _pending_count;
    for (;;) {
        // the prefix operators: - not !
        for (;;) {
            const bool minus = at(Tok::Minus);
            if (!minus && !at(Tok::Bang) && !at_word("not") && !at(Tok::Plus)) {
                break;
            }
            if (!at(Tok::Plus)) {
                if (_pending_count == kMaxPending) {
                    fail("the expression is too long");
                    break;
                }
                _pending[_pending_count++] = {minus ? Op::NegS : Op::NotS, static_cast<uint8_t>(minus ? 7 : 3), true,
                                              false, _tok.line, _tok.column};
            }
            advance();
        }
        if (_failed) {
            break;
        }
        const Val v = primary();
        if (_failed) {
            release(v);
            break;
        }
        if (_value_count == kMaxPending) {
            fail("the expression is too long");
            release(v);
            break;
        }
        _values[_value_count++] = v;
        Op op;
        uint8_t precedence;
        bool swap;
        if (!binary_operator(_tok.type, at(Tok::Name) ? text() : std::string_view{}, op, precedence, swap)) {
            break;
        }
        const Token token = _tok;
        advance();
        skip_newlines();
        while (_pending_count > pending && _pending[_pending_count - 1].precedence >= precedence && reduce()) {
        }
        if (_pending_count == kMaxPending) {
            fail("the expression is too long");
            break;
        }
        _pending[_pending_count++] = {op, precedence, false, swap, token.line, token.column};
    }
    while (!_failed && _pending_count > pending) {
        reduce();
    }
    if (_failed || _value_count != values + 1) {
        while (_value_count > values) {
            release(_values[--_value_count]);
        }
        _pending_count = pending;
        if (!_failed) {
            fail("expected a value");
        }
        return {};
    }
    return _values[--_value_count];
}

bool Compiler::reduce()
{
    const Pending p = _pending[--_pending_count];
    if (_value_count < (p.unary ? 1 : 2)) {
        fail_at(p.line, p.column, "expected a value");
        return false;
    }
    if (p.unary) {
        const Val a = _values[--_value_count];
        _values[_value_count++] = unary(p.op, a);
    } else {
        const Val b = _values[--_value_count];
        const Val a = _values[--_value_count];
        _values[_value_count++] = p.swap ? binary(p.op, b, a) : binary(p.op, a, b);
    }
    return !_failed;
}

Val Compiler::primary()
{
    if (!enter()) {
        return {};
    }
    Val v;
    if (at(Tok::Number)) {
        v = constant(_tok.number);
        advance();
    } else if (accept(Tok::LParen)) {
        v = expression();
        if (!expect(Tok::RParen, "')'")) {
            release(v);
            v = {};
        }
    } else if (at(Tok::Name) && !is_keyword(text())) {
        const Token name = _tok;
        advance();
        if (at(Tok::LParen)) {
            v = call(name, true);
        } else if (at(Tok::LBracket)) {
            const std::string_view n = _lex.text(name);
            const int g = find_global(n);
            if (g < 0 || (_globals[g].kind != Sym::Array && _globals[g].kind != Sym::Grid)) {
                fail_at(name.line, name.column, "'%.*s' is no array or grid", static_cast<int>(n.size()), n.data());
            } else {
                v = element(name, _globals[g]);
            }
        } else {
            v = name_value(name);
        }
    } else {
        fail("expected a value");
    }
    leave();
    return v;
}

Val Compiler::name_value(const Token &name)
{
    const std::string_view n = _lex.text(name);
    const int ni = static_cast<int>(n.size());
    if (const Local *local = find_local(n)) {
        Val v = local->val;
        v.temp = false;
        v.invariant = local->alias && v.invariant;
        return v;
    }
    const int g = find_global(n);
    if (g >= 0) {
        const Global &global = _globals[g];
        switch (global.kind) {
        case Sym::Param:
            return {Kind::Scalar, static_cast<uint8_t>(kParamBase + global.id), false, true, 0};
        case Sym::Var:
            return {Kind::Scalar, static_cast<uint8_t>(kVarBase + global.id), false, true, 0};
        case Sym::Function:
            fail_at(name.line, name.column, "'%.*s' is a function: %.*s(...)", ni, n.data(), ni, n.data());
            return {};
        default:
            fail_at(name.line, name.column, "'%.*s' needs its place: %.*s[...]", ni, n.data(), ni, n.data());
            return {};
        }
    }
    if (const BuiltinValue *b = find_value(n)) {
        if (b->pixel) {
            if (_context != Context::Pixel) {
                fail_at(name.line, name.column, "'%.*s' is known per pixel only", ni, n.data());
                return {};
            }
            _pixel_values = static_cast<uint8_t>(_pixel_values | (1u << b->reg));
            return {Kind::Vector, b->reg, false, false, 0};
        }
        _uses_sound = _uses_sound || b->use == kSound;
        _uses_color = _uses_color || b->use == kColor;
        return {Kind::Scalar, b->reg, false, true, 0};
    }
    fail_at(name.line, name.column,
            find_function(n) != nullptr ? "'%.*s' is a function: %.*s(...)" : "unknown name '%.*s'", ni, n.data(),
            ni, n.data());
    return {};
}

Val Compiler::element(const Token &name, const Global &global)
{
    advance();
    if (global.kind == Sym::Array) {
        Val index = expression();
        if (!expect(Tok::RBracket, "']'")) {
            release(index);
            return {};
        }
        if (index.kind == Kind::Vector) {
            const uint8_t r = index.reg;
            release(index);
            const uint8_t d = alloc_v();
            emit(Op::LdaV, d, r, 0, 0, 0, global.id);
            return {Kind::Vector, d, true, false, 0};
        }
        // the arrays do not change per pixel: once a frame when the index does not either
        const bool once = _context == Context::Pixel && index.invariant;
        const uint8_t r = sreg(index);
        release(index);
        const uint8_t d = alloc_s(once);
        emit(Op::LdaS, d, r, 0, 0, 0, global.id, once);
        return {Kind::Scalar, d, !once, once, 0};
    }
    Val gx = expression();
    if (!expect(Tok::Comma, "',' and the row: grid[x, y]")) {
        release(gx);
        return {};
    }
    Val gy = expression();
    if (!expect(Tok::RBracket, "']'")) {
        release(gx);
        release(gy);
        return {};
    }
    (void)name;
    // a grid changes per pixel (written row by row): read where it is used
    if (gx.kind == Kind::Vector || gy.kind == Kind::Vector) {
        const uint8_t rx = vreg(gx);
        const uint8_t ry = vreg(gy);
        release(gx);
        release(gy);
        const uint8_t d = alloc_v();
        emit(Op::LdgV, d, rx, ry, 0, 0, global.id);
        return {Kind::Vector, d, true, false, 0};
    }
    const uint8_t rx = sreg(gx);
    const uint8_t ry = sreg(gy);
    release(gx);
    release(gy);
    const uint8_t d = alloc_s(false);
    emit(Op::LdgS, d, rx, ry, 0, 0, global.id);
    return {Kind::Scalar, d, true, false, 0};
}

} // namespace px::effects::glow::detail
