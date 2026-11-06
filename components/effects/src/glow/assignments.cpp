// The compiler's assignments (compiler.hpp): to locals, variables, arrays and grids; the locals
// and their blocks, per pixel under a mask.

#include <cstring>

#include "compiler.hpp"

namespace px::effects::glow::detail {

namespace {

Op assign_op(Tok t)
{
    switch (t) {
    case Tok::AddAssign:
        return Op::AddS;
    case Tok::SubAssign:
        return Op::SubS;
    case Tok::MulAssign:
        return Op::MulS;
    default:
        return Op::DivS;
    }
}

bool is_assign(Tok t)
{
    return t == Tok::Assign || t == Tok::AddAssign || t == Tok::SubAssign || t == Tok::MulAssign ||
           t == Tok::DivAssign;
}

} // namespace

// --------------------------------------------------------------------------------- assignments

void Compiler::assignment(const Token &name)
{
    const std::string_view n = _lex.text(name);
    if (at(Tok::LParen)) {
        release(call(name, false));
        return;
    }
    if (at(Tok::LBracket)) {
        const int g = find_global(n);
        if (g < 0 || (_globals[g].kind != Sym::Array && _globals[g].kind != Sym::Grid)) {
            fail_at(name.line, name.column, "'%.*s' is no array or grid", static_cast<int>(n.size()), n.data());
            return;
        }
        assign_element(_globals[g], name);
        return;
    }
    if (!is_assign(_tok.type)) {
        fail("expected '=' or '(' after '%.*s'", static_cast<int>(n.size()), n.data());
        return;
    }
    const Tok op = _tok.type;
    advance();
    skip_newlines();
    assign_local(name, op, expression());
}

void Compiler::assign_local(const Token &name, Tok op, Val value)
{
    if (_failed) {
        release(value);
        return;
    }
    const std::string_view n = _lex.text(name);
    const int ni = static_cast<int>(n.size());
    Local *local = find_local(n);
    if (local == nullptr) {
        const int g = find_global(n);
        if (g >= 0 && _globals[g].kind == Sym::Var) {
            if (_context == Context::Pixel) {
                fail_at(name.line, name.column, "a variable cannot change per pixel ('%.*s')", ni, n.data());
                release(value);
                return;
            }
            const auto reg = static_cast<uint8_t>(kVarBase + _globals[g].id);
            if (op != Tok::Assign) {
                value = binary(assign_op(op), Val{Kind::Scalar, reg, false, true, 0}, value);
            }
            store_scalar(reg, value);
            return;
        }
        if (g >= 0) {
            const Sym kind = _globals[g].kind;
            fail_at(name.line, name.column,
                    kind == Sym::Param      ? "a param cannot be changed ('%.*s')"
                    : kind == Sym::Function ? "'%.*s' is a function"
                                            : "'%.*s' needs its place: %.*s[...]",
                    ni, n.data(), ni, n.data());
        } else if (builtin_name(n)) {
            fail_at(name.line, name.column, "'%.*s' is a built-in name", ni, n.data());
        } else if (op != Tok::Assign) {
            fail_at(name.line, name.column, "unknown name '%.*s'", ni, n.data());
        } else {
            declare_local(n, value);
            return;
        }
        release(value);
        return;
    }
    if (local->counter) {
        fail_at(name.line, name.column, "the loop's counter cannot be changed");
        release(value);
        return;
    }
    if (op != Tok::Assign) {
        value = binary(assign_op(op), borrow(local->val), value);
    }
    const bool masked = masked_for(*local);
    if (local->val.kind != Kind::Vector && _context == Context::Pixel && (value.kind == Kind::Vector || masked)) {
        // one value for all pixels so far: per pixel from the next pass on
        make_varying(local->decl);
        release(value);
        return;
    }
    if (local->alias) {
        // a function's argument: the caller's value until written, then its own
        const Val old = local->val;
        local->alias = false;
        if (old.kind == Kind::Vector) {
            local->val = {Kind::Vector, alloc_v(), false, false, 0};
            if (masked) {
                emit(Op::MovV, local->val.reg, old.reg);
            }
        } else {
            local->val = {Kind::Scalar, alloc_s(false), false, false, 0};
        }
    }
    if (local->val.kind == Kind::Vector) {
        store_vector(local->val.reg, value, masked);
    } else {
        store_scalar(local->val.reg, value);
    }
}

void Compiler::assign_element(const Global &global, const Token &name)
{
    advance();
    if (global.kind == Sym::Array) {
        if (_context == Context::Pixel) {
            fail_at(name.line, name.column, "an array cannot change per pixel");
            return;
        }
        Val index = expression();
        if (!expect(Tok::RBracket, "']'") || !is_assign(_tok.type)) {
            if (!_failed) {
                fail("expected '='");
            }
            release(index);
            return;
        }
        const Tok op = _tok.type;
        advance();
        Val value = expression();
        if (op != Tok::Assign) {
            const uint8_t old = alloc_s(false);
            emit(Op::LdaS, old, sreg(index), 0, 0, 0, global.id);
            value = binary(assign_op(op), Val{Kind::Scalar, old, true, false, 0}, value);
        }
        emit(Op::Sta, 0, sreg(index), sreg(value), 0, 0, global.id);
        release(index);
        release(value);
        return;
    }
    if (_context == Context::Pixel) {
        // per pixel only its own cell, after the row (the pixels of a row run at once)
        if (!at_word("x") || (advance(), !accept(Tok::Comma)) || !at_word("y") || (advance(), !accept(Tok::RBracket))) {
            fail("per pixel a grid is written at [x, y] only");
            return;
        }
        if (!is_assign(_tok.type)) {
            fail("expected '='");
            return;
        }
        const Tok op = _tok.type;
        advance();
        Val value = expression();
        if (op != Tok::Assign) {
            const uint8_t old = alloc_v();
            emit(Op::LdgV, old, kX, kY, 0, 0, global.id);
            _pixel_values |= (1u << kX) | (1u << kY);
            value = binary(assign_op(op), Val{Kind::Vector, old, true, false, 0}, value);
        }
        _grid_rows = static_cast<uint8_t>(_grid_rows | (1u << global.id));
        emit(Op::StgV, 0, vreg(value), mask(), 0, 0, global.id);
        release(value);
        return;
    }
    Val gx = expression();
    if (!expect(Tok::Comma, "',' and the row: grid[x, y]")) {
        release(gx);
        return;
    }
    Val gy = expression();
    if (!expect(Tok::RBracket, "']'") || !is_assign(_tok.type)) {
        if (!_failed) {
            fail("expected '='");
        }
        release(gx);
        release(gy);
        return;
    }
    const Tok op = _tok.type;
    advance();
    Val value = expression();
    const uint8_t rx = sreg(gx);
    const uint8_t ry = sreg(gy);
    if (op != Tok::Assign) {
        const uint8_t old = alloc_s(false);
        emit(Op::LdgS, old, rx, ry, 0, 0, global.id);
        value = binary(assign_op(op), Val{Kind::Scalar, old, true, false, 0}, value);
    }
    emit(Op::StgS, 0, rx, ry, sreg(value), 0, global.id);
    release(gx);
    release(gy);
    release(value);
}

Local *Compiler::find_local(std::string_view name)
{
    const uint8_t first = _inlined_count != 0 ? _inlined[_inlined_count - 1].locals : 0;
    for (int i = _local_count - 1; i >= first; i--) {
        if (name == _locals[i].name) {
            return &_locals[i];
        }
    }
    return nullptr;
}

Local *Compiler::declare_local(std::string_view name, Val value)
{
    const int ni = static_cast<int>(name.size());
    if (name.size() >= kMaxName) {
        fail("the name is too long (at most %u characters)", static_cast<unsigned>(kMaxName - 1));
    } else if (is_keyword(name)) {
        fail("'%.*s' is a word of the language", ni, name.data());
    } else if (_local_count == kMaxLocals) {
        fail("too many local values at once");
    } else if (_decl == kMaxDecls) {
        fail("too complex: too many local values");
    }
    if (_failed) {
        release(value);
        return nullptr;
    }
    const uint16_t decl = _decl++;
    Local &l = _locals[_local_count++];
    copy_name(l.name, name);
    l.depth = _depth;
    l.alias = false;
    l.counter = false;
    l.decl = decl;
    if (_context == Context::Pixel && (value.kind == Kind::Vector || varying(decl))) {
        if (value.kind == Kind::Vector && value.temp) {
            l.val = {Kind::Vector, value.reg, false, false, 0};
        } else {
            l.val = {Kind::Vector, alloc_v(), false, false, 0};
            store_vector(l.val.reg, value, false);
        }
    } else if (value.kind == Kind::Scalar && value.temp) {
        l.val = {Kind::Scalar, value.reg, false, false, 0};
    } else {
        l.val = {Kind::Scalar, alloc_s(false), false, false, 0};
        store_scalar(l.val.reg, value);
    }
    return &l;
}

void Compiler::close_scope(const Val *keep)
{
    while (_local_count > 0 && _locals[_local_count - 1].depth == _depth) {
        const Local &l = _locals[--_local_count];
        if (l.alias || (keep != nullptr && keep->kind == l.val.kind && keep->reg == l.val.reg)) {
            continue;
        }
        if (l.val.kind == Kind::Vector) {
            free_v(l.val.reg);
        } else if (l.val.kind == Kind::Scalar) {
            free_s(l.val.reg);
        }
    }
    _depth--;
}

bool Compiler::masked_for(const Local &local) const
{
    return _mask_count != 0 && local.depth < _masks[_mask_count - 1].depth;
}

void Compiler::store_scalar(uint8_t reg, Val value)
{
    if (_failed) {
        return;
    }
    const uint8_t r = sreg(value);
    if (r != reg) {
        emit(Op::MovS, reg, r);
    }
    release(value);
}

void Compiler::store_vector(uint8_t reg, Val value, bool masked)
{
    if (_failed) {
        return;
    }
    if (masked) {
        const uint8_t v = vreg(value);
        emit(Op::SelV, reg, mask(), v, reg);
    } else if (value.kind == Kind::Vector) {
        if (value.reg != reg) {
            emit(Op::MovV, reg, value.reg);
        }
    } else {
        emit(Op::Bcast, reg, sreg(value));
    }
    release(value);
}

} // namespace px::effects::glow::detail
