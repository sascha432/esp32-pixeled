// The compiler's operations and registers (compiler.hpp): where an instruction goes. Per pixel a
// value that is the same for all pixels the whole frame is computed once a frame (the prologue),
// one the same for the pixels of a run once a run, the rest per pixel.

#include "compiler.hpp"

namespace px::effects::glow::detail {

namespace {

Op shifted(Op base, int by)
{
    return static_cast<Op>(static_cast<uint8_t>(base) + by);
}

Fix fold(Op base, Fix a, Fix b)
{
    switch (base) {
#define GLOW_FOLD_BINARY(name, f)                                                                                      \
    case Op::name##S:                                                                                                  \
        return fx::f(a, b);
#define GLOW_FOLD_UNARY(name, f)                                                                                       \
    case Op::name##S:                                                                                                  \
        return fx::f(a);
        GLOW_BINARY(GLOW_FOLD_BINARY)
        GLOW_UNARY(GLOW_FOLD_UNARY)
#undef GLOW_FOLD_BINARY
#undef GLOW_FOLD_UNARY
    default:
        return 0;
    }
}

} // namespace

// ------------------------------------------------------------------------------------ operations

Val Compiler::unary(Op base, Val a)
{
    if (_failed) {
        release(a);
        return {};
    }
    if (a.kind == Kind::Const) {
        return constant(fold(base, a.k, 0));
    }
    if (a.kind == Kind::Vector) {
        const uint8_t r = a.reg;
        release(a);
        const uint8_t d = alloc_v();
        emit(shifted(base, 1), d, r);
        return {Kind::Vector, d, true, false, 0};
    }
    const bool once = _context == Context::Pixel && a.invariant;
    const uint8_t r = sreg(a);
    release(a);
    const uint8_t d = alloc_s(once);
    emit(base, d, r, 0, 0, 0, 0, once);
    return {Kind::Scalar, d, !once, once, 0};
}

Val Compiler::binary(Op base, Val a, Val b)
{
    if (_failed) {
        release(a);
        release(b);
        return {};
    }
    if (a.kind == Kind::Const && b.kind == Kind::Const) {
        return constant(fold(base, a.k, b.k));
    }
    if (base == Op::DivS && b.kind == Kind::Const && b.k != 0) {
        // by a constant: a multiplication (a division takes many times as long), rounded where the
        // division would cut off
        const uint32_t c = b.k < 0 ? 0u - static_cast<uint32_t>(b.k) : static_cast<uint32_t>(b.k);
        Val q = a;
        if (c > 2u * kOne) {
            // 2^48 / c below 2^31
            const auto reciprocal = static_cast<uint32_t>(((uint64_t{1} << 48) + c / 2) / c);
            q = binary(Op::MulHiS, a, constant(static_cast<Fix>(reciprocal)));
        } else if (c != static_cast<uint32_t>(kOne)) {
            // up to 2: times the reciprocal in 16.16 (but for the very smallest divisors)
            if (c < 4) {
                return binary_plain(base, a, b);
            }
            q = binary(Op::MulS, a, constant(static_cast<Fix>(((uint64_t{1} << 32) + c / 2) / c)));
        }
        return b.k < 0 ? unary(Op::NegS, q) : q;
    }
    return binary_plain(base, a, b);
}

Val Compiler::binary_plain(Op base, Val a, Val b)
{
    if (a.kind != Kind::Vector && b.kind != Kind::Vector) {
        const bool once = _context == Context::Pixel && a.invariant && b.invariant;
        const uint8_t ra = sreg(a);
        const uint8_t rb = sreg(b);
        release(a);
        release(b);
        const uint8_t d = alloc_s(once);
        emit(base, d, ra, rb, 0, 0, 0, once);
        return {Kind::Scalar, d, !once, once, 0};
    }
    Op op;
    uint8_t ra;
    uint8_t rb;
    if (a.kind == Kind::Vector && b.kind == Kind::Vector) {
        op = shifted(base, 1);
        ra = a.reg;
        rb = b.reg;
    } else if (a.kind == Kind::Vector) {
        op = shifted(base, 2);
        ra = a.reg;
        rb = sreg(b);
    } else {
        op = shifted(base, 3);
        ra = sreg(a);
        rb = b.reg;
    }
    release(a);
    release(b);
    const uint8_t d = alloc_v();
    emit(op, d, ra, rb);
    return {Kind::Vector, d, true, false, 0};
}

Val Compiler::nary(Op scalar, Op vector, Val *args, size_t argc, bool pure)
{
    uint8_t r[3] = {0, 0, 0};
    bool any_vector = false;
    bool invariant = true;
    for (size_t i = 0; i < argc; i++) {
        any_vector = any_vector || args[i].kind == Kind::Vector;
        invariant = invariant && args[i].invariant;
    }
    if (any_vector) {
        for (size_t i = 0; i < argc; i++) {
            r[i] = vreg(args[i]);
        }
        for (size_t i = 0; i < argc; i++) {
            release(args[i]);
        }
        const uint8_t d = alloc_v();
        emit(vector, d, r[0], r[1], r[2]);
        return {Kind::Vector, d, true, false, 0};
    }
    const bool once = _context == Context::Pixel && pure && invariant;
    for (size_t i = 0; i < argc; i++) {
        r[i] = sreg(args[i]);
    }
    for (size_t i = 0; i < argc; i++) {
        release(args[i]);
    }
    const uint8_t d = alloc_s(once);
    emit(scalar, d, r[0], r[1], r[2], 0, 0, once);
    return {Kind::Scalar, d, !once, once, 0};
}

Val Compiler::select(Val c, Val a, Val b)
{
    if (_failed) {
        release(c);
        release(a);
        release(b);
        return {};
    }
    if (c.kind == Kind::Const) {
        release(c.k != 0 ? b : a);
        return c.k != 0 ? a : b;
    }
    Val args[3] = {c, a, b};
    return nary(Op::SelS, Op::SelV, args, 3, true);
}

// ------------------------------------------------------------------------------------- registers

uint8_t Compiler::sreg(const Val &v)
{
    if (v.kind == Kind::Const) {
        return konst(v.k);
    }
    if (v.kind == Kind::Vector) {
        fail("a value per pixel where one for all pixels is needed");
        return 0;
    }
    return v.reg;
}

uint8_t Compiler::vreg(Val &v)
{
    if (v.kind == Kind::Vector) {
        return v.reg;
    }
    const uint8_t s = sreg(v);
    release(v);
    const uint8_t d = alloc_v();
    emit(Op::Bcast, d, s);
    v = {Kind::Vector, d, true, false, 0};
    return d;
}

void Compiler::release(const Val &v)
{
    if (!v.temp) {
        return;
    }
    if (v.kind == Kind::Scalar) {
        free_s(v.reg);
    } else if (v.kind == Kind::Vector) {
        free_v(v.reg);
    }
}

uint8_t Compiler::konst(Fix k)
{
    for (uint8_t i = 0; i < _const_count; i++) {
        if (_consts[i] == k) {
            return static_cast<uint8_t>(kScalars - 1 - i);
        }
    }
    const size_t reg = kScalars - 1 - _const_count;
    if (_const_count == kMaxConsts || (_s_used[reg / 32] & (1u << (reg % 32))) != 0) {
        fail("too many numbers (at most %u different ones)", static_cast<unsigned>(kMaxConsts));
        return 0;
    }
    _s_used[reg / 32] |= 1u << (reg % 32);
    _s_permanent[reg / 32] |= 1u << (reg % 32);
    _consts[_const_count++] = k;
    return static_cast<uint8_t>(reg);
}

uint8_t Compiler::alloc_s(bool permanent)
{
    const size_t first = kVarBase + _header.var_count;
    const size_t end = kScalars - _const_count;
    for (size_t r = first; r < end; r++) {
        if ((_s_used[r / 32] & (1u << (r % 32))) == 0) {
            _s_used[r / 32] |= 1u << (r % 32);
            if (permanent) {
                _s_permanent[r / 32] |= 1u << (r % 32);
            }
            return static_cast<uint8_t>(r);
        }
    }
    fail("too complex: too many values at once");
    return 0;
}

void Compiler::free_s(uint8_t reg)
{
    if (reg >= kVarBase + _header.var_count && (_s_permanent[reg / 32] & (1u << (reg % 32))) == 0) {
        _s_used[reg / 32] &= ~(1u << (reg % 32));
    }
}

uint8_t Compiler::alloc_v()
{
    for (uint8_t r = kPixelValues; r < kVectors; r++) {
        if ((_v_used & (1u << r)) == 0) {
            _v_used |= 1u << r;
            _v_high = r + 1 > _v_high ? static_cast<uint8_t>(r + 1) : _v_high;
            return r;
        }
    }
    fail("too complex: too many values per pixel at once (at most %u)", static_cast<unsigned>(kVectors));
    return kPixelValues;
}

void Compiler::free_v(uint8_t reg)
{
    if (reg >= kPixelValues && reg < kVectors) {
        _v_used &= ~(1u << reg);
    }
}

void Compiler::make_varying(uint16_t decl)
{
    _varying[decl / 8] = static_cast<uint8_t>(_varying[decl / 8] | (1u << (decl % 8)));
    _again = true;
}

uint16_t Compiler::emit(Op op, uint8_t d, uint8_t a, uint8_t b, uint8_t c, uint8_t e, uint16_t imm, bool once)
{
    if (_failed) {
        return 0;
    }
    if (_up >= _down) {
        fail("the script is too long (at most %u instructions)", static_cast<unsigned>(kMaxCode));
        return 0;
    }
    const uint16_t at = once ? --_down : _up++;
    _code[at] = {op, d, a, b, c, e, imm};
    _lines[at] = _last.line != 0 ? _last.line : _tok.line;
    return at;
}

} // namespace px::effects::glow::detail
