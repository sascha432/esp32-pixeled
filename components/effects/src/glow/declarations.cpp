// The compiler's first pass (compiler.hpp): the declarations of a script (anim, sound, param, var,
// grid, fn) and where its blocks start.

#include <cstring>

#include "compiler.hpp"

namespace px::effects::glow::detail {

// ------------------------------------------------------------------------------- declarations

bool Compiler::scan()
{
    for (;;) {
        while (at(Tok::Newline) || at(Tok::Semicolon)) {
            advance();
        }
        if (_failed || at(Tok::End)) {
            break;
        }
        const Token word = _tok;
        const std::string_view w = at(Tok::Name) ? text() : std::string_view{};
        if (w == "anim") {
            advance();
            if (_named) {
                fail_at(word.line, word.column, "the script has a name already");
            } else if (!at(Tok::String)) {
                fail("expected the name in quotes: anim \"My animation\"");
            } else if (_tok.length == 0 || _tok.length >= kNameSize) {
                fail("the name has 1 to %u characters", static_cast<unsigned>(kNameSize - 1));
            } else {
                std::memcpy(_header.info.name, text().data(), _tok.length);
                _named = true;
                advance();
            }
        } else if (w == "sound") {
            advance();
            _sound = at_word("never") ? 1 : at_word("optional") ? 2 : at_word("required") ? 3 : 0;
            if (_sound == 0) {
                fail("expected required, optional or never");
            }
            advance();
        } else if (w == "param") {
            advance();
            declare_param();
        } else if (w == "var") {
            advance();
            declare_vars();
        } else if (w == "grid") {
            advance();
            declare_grids();
        } else if (w == "fn") {
            advance();
            declare_function();
        } else if (w == "init" || w == "frame" || w == "pixel") {
            advance();
            Lexer::Mark &mark = w == "init" ? _init_at : (w == "frame" ? _frame_at : _pixel_at);
            bool &present = w == "init" ? _has_init : (w == "frame" ? _has_frame : _has_pixel);
            if (present) {
                fail_at(word.line, word.column, "the script has a %.*s block already", static_cast<int>(w.size()),
                        w.data());
            } else if (at(Tok::LBrace)) {
                mark = _tok_mark;
                present = true;
                skip_block();
            } else {
                fail("expected '{'");
            }
        } else {
            fail("expected anim, param, var, grid, fn, init, frame or pixel");
        }
        if (_failed) {
            break;
        }
        end_of_statement();
    }
    if (!_failed && !_named) {
        fail_at(1, 1, "the script starts with its name: anim \"My animation\"");
    }
    return !_failed;
}

void Compiler::skip_block()
{
    // at '{'
    int open = 0;
    do {
        open += at(Tok::LBrace) ? 1 : (at(Tok::RBrace) ? -1 : 0);
        if (at(Tok::End)) {
            fail("a block has no closing '}'");
            return;
        }
        advance();
    } while (open > 0 && !_failed);
}

void Compiler::skip_value()
{
    // an expression up to the next ',' or the end of the line, ( ) and [ ] inside it
    int open = 0;
    while (!_failed && !at(Tok::End)) {
        if (open == 0 && (at(Tok::Comma) || at(Tok::Newline) || at(Tok::Semicolon))) {
            return;
        }
        open += at(Tok::LParen) || at(Tok::LBracket) ? 1 : (at(Tok::RParen) || at(Tok::RBracket) ? -1 : 0);
        advance();
    }
}

int Compiler::find_global(std::string_view name) const
{
    for (uint8_t i = 0; i < _global_count; i++) {
        if (name == _globals[i].name) {
            return i;
        }
    }
    return -1;
}

bool Compiler::new_name(std::string_view name)
{
    if (!at(Tok::Name)) {
        fail("expected a name");
        return false;
    }
    if (name.size() >= kMaxName) {
        fail("the name is too long (at most %u characters)", static_cast<unsigned>(kMaxName - 1));
        return false;
    }
    if (is_keyword(name)) {
        fail("'%.*s' is a word of the language", static_cast<int>(name.size()), name.data());
        return false;
    }
    if (builtin_name(name)) {
        fail("'%.*s' is a built-in name", static_cast<int>(name.size()), name.data());
        return false;
    }
    if (find_global(name) >= 0) {
        fail("'%.*s' is declared twice", static_cast<int>(name.size()), name.data());
        return false;
    }
    return true;
}

bool Compiler::add_global(std::string_view name, Sym kind, uint8_t id)
{
    if (_global_count == kMaxGlobals) {
        fail("too many declarations");
        return false;
    }
    Global &g = _globals[_global_count++];
    copy_name(g.name, name);
    g.kind = kind;
    g.id = id;
    return true;
}

bool Compiler::signed_number(Fix &out)
{
    const bool negative = accept(Tok::Minus);
    if (!at(Tok::Number)) {
        fail("expected a number");
        return false;
    }
    out = negative ? fx::neg(_tok.number) : _tok.number;
    advance();
    return true;
}

void Compiler::declare_param()
{
    Info &info = _header.info;
    if (info.param_count == kMaxParams) {
        fail("at most %u params", static_cast<unsigned>(kMaxParams));
        return;
    }
    const std::string_view name = text();
    if (!new_name(name)) {
        return;
    }
    Param &p = info.params[info.param_count];
    std::memset(&p, 0, sizeof(p));
    copy_name(p.name, name);
    const Token at_name = _tok;
    advance();
    if (!expect(Tok::Assign, "'=' and its value: param size = 0.5 [0 .. 1]") || !signed_number(p.initial) ||
        !expect(Tok::LBracket, "'[' and the range: [0 .. 1]") || !signed_number(p.min) ||
        !expect(Tok::DotDot, "'..'") || !signed_number(p.max)) {
        return;
    }
    if (at_word("step")) {
        advance();
        if (!signed_number(p.step)) {
            return;
        }
    }
    if (!expect(Tok::RBracket, "']'")) {
        return;
    }
    if (at(Tok::String)) {
        const size_t n = _tok.length < sizeof(p.label) - 1 ? _tok.length : sizeof(p.label) - 1;
        std::memcpy(p.label, text().data(), n);
        advance();
    } else {
        std::memcpy(p.label, p.name, sizeof(p.name));
    }
    if (p.min >= p.max) {
        fail_at(at_name.line, at_name.column, "the range goes from the low value to the high one");
    } else if (p.initial < p.min || p.initial > p.max) {
        fail_at(at_name.line, at_name.column, "the value is outside its range");
    } else if (p.step < 0 || p.step > fx::sub(p.max, p.min)) {
        fail_at(at_name.line, at_name.column, "the step is 0 or up to the range");
    } else {
        add_global(name, Sym::Param, info.param_count++);
    }
}

void Compiler::declare_vars()
{
    do {
        skip_newlines();
        const std::string_view name = text();
        if (!new_name(name)) {
            return;
        }
        advance();
        if (accept(Tok::LBracket)) {
            // an array: its size a whole number
            if (!at(Tok::Number) || fx::frac(_tok.number) != 0 || _tok.number < kOne) {
                fail("expected the array's size, a whole number: var p[16]");
                return;
            }
            const uint32_t size = static_cast<uint32_t>(_tok.number >> 16);
            if (_header.array_count == kMaxArrays || _header.array_words + size > kMaxArrayWords) {
                fail("at most %u arrays of %u values together", static_cast<unsigned>(kMaxArrays),
                     static_cast<unsigned>(kMaxArrayWords));
                return;
            }
            advance();
            if (!expect(Tok::RBracket, "']'")) {
                return;
            }
            _header.arrays[_header.array_count] = {_header.array_words, static_cast<uint16_t>(size)};
            _header.array_words = static_cast<uint16_t>(_header.array_words + size);
            add_global(name, Sym::Array, _header.array_count++);
            continue;
        }
        if (_header.var_count == kMaxVars) {
            fail("at most %u variables", static_cast<unsigned>(kMaxVars));
            return;
        }
        const uint8_t id = _header.var_count++;
        if (accept(Tok::Assign)) {
            // compiled at the start of the init code
            _var_inits[_var_init_count++] = {_tok_mark, id};
            skip_value();
        }
        add_global(name, Sym::Var, id);
    } while (!_failed && accept(Tok::Comma));
}

void Compiler::declare_grids()
{
    do {
        skip_newlines();
        const std::string_view name = text();
        if (!new_name(name)) {
            return;
        }
        if (_header.grid_count == kMaxGrids) {
            fail("at most %u grids", static_cast<unsigned>(kMaxGrids));
            return;
        }
        advance();
        add_global(name, Sym::Grid, _header.grid_count++);
    } while (!_failed && accept(Tok::Comma));
}

void Compiler::declare_function()
{
    const std::string_view name = text();
    if (!new_name(name)) {
        return;
    }
    if (_function_count == kMaxFunctions) {
        fail("at most %u functions", static_cast<unsigned>(kMaxFunctions));
        return;
    }
    Function &f = _functions[_function_count];
    f.argc = 0;
    advance();
    if (!expect(Tok::LParen, "'(' and the function's values: fn name(a, b) { ... }")) {
        return;
    }
    while (!at(Tok::RParen)) {
        const std::string_view arg = text();
        if (!at(Tok::Name) || is_keyword(arg) || builtin_name(arg) || arg.size() >= kMaxName) {
            fail("expected a name for the function's value");
            return;
        }
        if (f.argc == kMaxArgs) {
            fail("at most %u values for a function", static_cast<unsigned>(kMaxArgs));
            return;
        }
        for (uint8_t i = 0; i < f.argc; i++) {
            if (arg == f.args[i]) {
                fail("'%.*s' is there twice", static_cast<int>(arg.size()), arg.data());
                return;
            }
        }
        copy_name(f.args[f.argc++], arg);
        advance();
        if (!accept(Tok::Comma) && !at(Tok::RParen)) {
            fail("expected ',' or ')'");
            return;
        }
    }
    advance();
    if (!at(Tok::LBrace)) {
        fail("expected '{'");
        return;
    }
    f.body = _tok_mark;
    skip_block();
    add_global(name, Sym::Function, _function_count++);
}

} // namespace px::effects::glow::detail
