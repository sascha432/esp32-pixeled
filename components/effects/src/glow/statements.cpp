// The compiler's statements (compiler.hpp): blocks, if (a jump, or per pixel both branches with
// masks), the loops, break and continue.

#include "compiler.hpp"

namespace px::effects::glow::detail {

// --------------------------------------------------------------------------------- statements

void Compiler::block()
{
    if (!enter()) {
        return;
    }
    if (expect(Tok::LBrace, "'{'")) {
        open_scope();
        statements(false, nullptr);
        close_scope();
        expect(Tok::RBrace, "'}'");
    }
    leave();
}

void Compiler::statements(bool function_body, Val *result)
{
    for (;;) {
        while (at(Tok::Newline) || at(Tok::Semicolon)) {
            advance();
        }
        if (_failed || at(Tok::RBrace) || at(Tok::End)) {
            return;
        }
        if (function_body && at_word("return")) {
            advance();
            *result = at(Tok::Newline) || at(Tok::Semicolon) || at(Tok::RBrace) ? constant(0) : expression();
            while (at(Tok::Newline) || at(Tok::Semicolon)) {
                advance();
            }
            if (!at(Tok::RBrace)) {
                fail("return is the last statement of a function");
            }
            return;
        }
        statement();
    }
}

void Compiler::statement()
{
    if (!at(Tok::Name)) {
        fail("expected a statement");
        return;
    }
    const std::string_view word = text();
    if (word == "if") {
        if_statement();
    } else if (word == "for") {
        for_statement();
    } else if (word == "repeat") {
        repeat_statement();
    } else if (word == "while") {
        while_statement();
    } else if (word == "break" || word == "continue") {
        jump_statement(word == "break");
    } else if (word == "return") {
        fail(_inlined_count != 0 ? "return is the last statement of a function" : "return only in a function");
        return;
    } else if (word == "else") {
        fail("else without if");
        return;
    } else if (is_keyword(word)) {
        fail("'%.*s' cannot start a statement", static_cast<int>(word.size()), word.data());
        return;
    } else {
        const Token name = _tok;
        advance();
        assignment(name);
    }
    end_of_statement();
}

void Compiler::if_statement()
{
    advance();
    Val cond = expression();
    if (_failed) {
        release(cond);
        return;
    }
    if (cond.kind != Kind::Vector) {
        // the same for every pixel: a jump
        const uint8_t r = sreg(cond);
        release(cond);
        const uint16_t skip = emit(Op::Jz, 0, r);
        block();
        if (!else_follows()) {
            patch(skip, here());
            return;
        }
        advance();
        const uint16_t over = emit(Op::Jmp);
        patch(skip, here());
        if (at_word("if")) {
            if_statement();
        } else {
            block();
        }
        patch(over, here());
        return;
    }
    // per pixel: both branches run, each where its mask is true
    if (_mask_count == kMaxNesting) {
        fail("nested too deeply");
        release(cond);
        return;
    }
    const uint8_t outer = mask();
    const Val inside = outer == kNoMask ? borrow(cond) : binary(Op::AndS, Val{Kind::Vector, outer, false, false, 0}, borrow(cond));
    _masks[_mask_count++] = {inside.reg, static_cast<uint8_t>(_depth + 1)};
    // a chunk with no pixel inside skips the block: all of it is masked, its locals end with it
    const uint16_t skip = emit(Op::Jnone, 0, inside.reg);
    block();
    patch(skip, here());
    _mask_count--;
    release(inside);
    if (else_follows()) {
        advance();
        Val other = unary(Op::NotS, borrow(cond));
        if (outer != kNoMask) {
            other = binary(Op::AndS, Val{Kind::Vector, outer, false, false, 0}, other);
        }
        _masks[_mask_count++] = {other.reg, static_cast<uint8_t>(_depth + 1)};
        const uint16_t skip_other = emit(Op::Jnone, 0, other.reg);
        if (at_word("if")) {
            if_statement();
        } else {
            block();
        }
        patch(skip_other, here());
        _mask_count--;
        release(other);
    }
    release(cond);
}

void Compiler::loop_end(uint16_t top, uint16_t exit, uint8_t counter)
{
    // continue: the counter's step (if any), then back to the test
    Loop &loop = _loops[_loop_count - 1];
    for (uint8_t i = 0; i < loop.continue_count; i++) {
        patch(loop.continues[i], here());
    }
    if (counter != 0) {
        emit(Op::AddS, counter, counter, konst(kOne));
    }
    emit(Op::Jmp, 0, 0, 0, 0, 0, top);
    patch(exit, here());
    for (uint8_t i = 0; i < loop.break_count; i++) {
        patch(loop.breaks[i], here());
    }
    _loop_count--;
}

bool Compiler::open_loop()
{
    if (_loop_count == kMaxLoops) {
        fail("loops nested too deeply");
        return false;
    }
    Loop &loop = _loops[_loop_count++];
    loop = {};
    loop.masks = _mask_count;
    return true;
}

void Compiler::counted_loop(const Token *name, Val from, Val to)
{
    if (_failed) {
        release(from);
        release(to);
        return;
    }
    if (from.kind == Kind::Vector || to.kind == Kind::Vector) {
        fail("the loop's bounds must be the same for every pixel");
        release(from);
        release(to);
        return;
    }
    open_scope();
    // the end, evaluated once
    Val end = to;
    if (to.kind == Kind::Scalar && !to.temp) {
        end = {Kind::Scalar, alloc_s(false), true, false, 0};
        emit(Op::MovS, end.reg, to.reg);
    }
    const uint8_t last = sreg(end);
    uint8_t counter;
    if (name != nullptr) {
        Local *local = declare_local(_lex.text(*name), from);
        if (local == nullptr) {
            release(end);
            close_scope();
            return;
        }
        local->counter = true;
        counter = local->val.reg;
    } else {
        counter = alloc_s(false);
        store_scalar(counter, from);
    }
    const uint16_t top = here();
    const uint8_t test = alloc_s(false);
    emit(Op::LtS, test, counter, last);
    const uint16_t exit = emit(Op::Jz, 0, test);
    free_s(test);
    if (open_loop()) {
        block();
        loop_end(top, exit, counter);
    }
    if (name == nullptr) {
        free_s(counter);
    }
    release(end);
    close_scope();
}

void Compiler::for_statement()
{
    advance();
    if (!at(Tok::Name) || is_keyword(text()) || builtin_name(text())) {
        fail("expected the loop's counter: for k in 0 .. 10");
        return;
    }
    const Token name = _tok;
    advance();
    if (!at_word("in")) {
        fail("expected 'in': for k in 0 .. 10");
        return;
    }
    advance();
    Val from = expression();
    if (!expect(Tok::DotDot, "'..': for k in 0 .. 10")) {
        release(from);
        return;
    }
    Val to = expression();
    counted_loop(&name, from, to);
}

void Compiler::repeat_statement()
{
    advance();
    Val times = expression();
    counted_loop(nullptr, constant(0), times);
}

void Compiler::while_statement()
{
    if (_context == Context::Pixel) {
        fail("while is not possible per pixel: use for");
        return;
    }
    advance();
    const uint16_t top = here();
    Val cond = expression();
    if (_failed) {
        release(cond);
        return;
    }
    const uint8_t r = sreg(cond);
    release(cond);
    const uint16_t exit = emit(Op::Jz, 0, r);
    if (open_loop()) {
        block();
        loop_end(top, exit, 0);
    }
}

void Compiler::jump_statement(bool is_break)
{
    const char *word = is_break ? "break" : "continue";
    const uint8_t outside = _inlined_count != 0 ? _inlined[_inlined_count - 1].loops : 0;
    if (_loop_count <= outside) {
        fail("%s outside a loop", word);
        return;
    }
    Loop &loop = _loops[_loop_count - 1];
    if (_mask_count > loop.masks) {
        fail("%s under a per-pixel condition is not possible", word);
        return;
    }
    uint8_t &count = is_break ? loop.break_count : loop.continue_count;
    if (count == kMaxJumps) {
        fail("too many %s in one loop", word);
        return;
    }
    (is_break ? loop.breaks : loop.continues)[count++] = emit(Op::Jmp);
    advance();
}

void Compiler::patch(uint16_t at, uint16_t target)
{
    if (!_failed) {
        _code[at].imm = target;
    }
}

} // namespace px::effects::glow::detail
