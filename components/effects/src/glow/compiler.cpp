// The compiler (compiler.hpp): the tokens it reads and the passes over the source, the program's
// layout. The declarations are in declarations.cpp, the statements in statements.cpp and
// assignments.cpp, the values in expressions.cpp, calls.cpp and registers.cpp.

#include "compiler.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace px::effects::glow::detail {

namespace {

// (step in a param's range is the built-in function's name)
constexpr const char *kKeywords[] = {"anim",  "sound", "param",    "var",    "grid", "fn", "init",
                                     "frame", "pixel", "if",       "else",   "for",  "in", "repeat",
                                     "while", "break", "continue", "return", "and",  "or", "not"};

// the passes over the blocks before the values per pixel settle
constexpr int kMaxPasses = 4;

} // namespace

bool is_keyword(std::string_view name)
{
    for (const char *k : kKeywords) {
        if (name == k) {
            return true;
        }
    }
    return false;
}

Compiler::Compiler(std::string_view source, Error &error) : _lex(source), _error(&error)
{
    std::memset(&_header, 0, sizeof(_header));
    *_error = {};
    advance();
}

// ------------------------------------------------------------------------------------- tokens

void Compiler::advance()
{
    _last = _tok;
    if (_failed) {
        _tok.type = Tok::End;
        return;
    }
    _tok_mark = _lex.mark();
    _tok = _lex.next();
    if (_tok.type == Tok::Bad) {
        fail_at(_tok.line, _tok.column, "%s", _lex.problem());
        _tok.type = Tok::End;
    }
}

bool Compiler::at_word(const char *word) const
{
    return _tok.type == Tok::Name && text() == word;
}

bool Compiler::accept(Tok t)
{
    if (!at(t)) {
        return false;
    }
    advance();
    return true;
}

bool Compiler::expect(Tok t, const char *what)
{
    if (accept(t)) {
        return true;
    }
    fail("expected %s", what);
    return false;
}

void Compiler::skip_newlines()
{
    while (at(Tok::Newline)) {
        advance();
    }
}

void Compiler::end_of_statement()
{
    if (at(Tok::Newline) || at(Tok::Semicolon)) {
        advance();
    } else if (!at(Tok::RBrace) && !at(Tok::End)) {
        fail("expected the end of the line");
    }
}

bool Compiler::else_follows()
{
    if (at_word("else")) {
        return true;
    }
    if (!at(Tok::Newline)) {
        return false;
    }
    // `}` and `else` on separate lines: a look past the newline
    const Lexer::Mark back = _tok_mark;
    const Token last = _last;
    advance();
    if (at_word("else")) {
        return true;
    }
    _lex.reset(back);
    advance();
    _last = last;
    return false;
}

void Compiler::fail(const char *format, ...)
{
    if (_failed) {
        return;
    }
    va_list args;
    va_start(args, format);
    std::vsnprintf(_error->text, sizeof(_error->text), format, args);
    va_end(args);
    _error->line = _tok.line;
    _error->column = _tok.column;
    _failed = true;
}

void Compiler::fail_at(uint16_t line, uint16_t column, const char *format, ...)
{
    if (_failed) {
        return;
    }
    va_list args;
    va_start(args, format);
    std::vsnprintf(_error->text, sizeof(_error->text), format, args);
    va_end(args);
    _error->line = line;
    _error->column = column;
    _failed = true;
}

bool Compiler::enter()
{
    if (_nesting >= kMaxNesting) {
        fail("nested too deeply (at most %u blocks, parentheses and calls)", static_cast<unsigned>(kMaxNesting));
        return false;
    }
    _nesting++;
    return true;
}

// ------------------------------------------------------------------------------------ the passes

size_t Compiler::run()
{
    if (!scan()) {
        return 0;
    }
    for (int pass = 0; pass < kMaxPasses; pass++) {
        reset_pass();
        compile_init();
        const uint16_t frame = _up;
        if (_has_frame) {
            compile_block_at(_frame_at, Context::Frame);
        }
        _streams[kFrame] = {frame, _up};
        const uint16_t body = _up;
        if (_has_pixel) {
            compile_block_at(_pixel_at, Context::Pixel);
        }
        _streams[kBody] = {body, _up};
        if (_failed) {
            return 0;
        }
        if (!_again) {
            return finish();
        }
    }
    fail_at(0, 0, "too complex: which values are per pixel does not settle");
    return 0;
}

void Compiler::reset_pass()
{
    _up = 0;
    _down = kMaxCode;
    _const_count = 0;
    std::memset(_s_used, 0, sizeof(_s_used));
    std::memset(_s_permanent, 0, sizeof(_s_permanent));
    // the built-in values, the params and the variables are taken
    for (size_t r = 0; r < kVarBase + _header.var_count; r++) {
        _s_used[r / 32] |= 1u << (r % 32);
    }
    _v_used = (1u << kPixelValues) - 1;
    _v_high = kPixelValues;
    _local_count = 0;
    _depth = 0;
    _mask_count = 0;
    _loop_count = 0;
    _inlined_count = 0;
    _value_count = 0;
    _pending_count = 0;
    _nesting = 0;
    _decl = 0;
    _again = false;
    _uses_sound = false;
    _uses_color = false;
    _uses_palette = false;
    _draws = false;
    _pixel_values = 0;
    _grid_rows = 0;
    _tables = 0;
}

void Compiler::compile_init()
{
    _context = Context::Init;
    const uint16_t from = _up;
    for (uint8_t i = 0; i < _var_init_count && !_failed; i++) {
        _lex.reset(_var_inits[i].at);
        advance();
        store_scalar(static_cast<uint8_t>(kVarBase + _var_inits[i].var), expression());
    }
    if (_has_init) {
        compile_block_at(_init_at, Context::Init);
    }
    _streams[kInit] = {from, _up};
}

void Compiler::compile_block_at(const Lexer::Mark &mark, Context context)
{
    _context = context;
    _lex.reset(mark);
    advance();
    block();
}

size_t Compiler::finish()
{
    Header &h = _header;
    // the once-a-frame part of the per-pixel code was written downward from the end: turned around
    // and moved after the rest
    const uint16_t once = static_cast<uint16_t>(kMaxCode - _down);
    std::reverse(_code + _down, _code + kMaxCode);
    std::reverse(_lines + _down, _lines + kMaxCode);
    std::memmove(_code + _up, _code + _down, once * sizeof(Ins));
    std::memmove(_lines + _up, _lines + _down, once * sizeof(uint16_t));
    _streams[kPrologue] = {_up, static_cast<uint16_t>(_up + once)};
    const uint16_t count = static_cast<uint16_t>(_up + once);

    h.magic = kMagic;
    for (size_t s = 0; s < kStreams; s++) {
        h.streams[s] = _streams[s];
    }
    h.code_count = count;
    h.const_count = _const_count;
    h.const_base = static_cast<uint16_t>(kScalars - _const_count);
    h.scalar_count = kScalars;
    h.vector_count = _v_high;
    h.pixel_values = _pixel_values;
    h.flags = static_cast<uint8_t>((_has_pixel ? kHasPixel : 0) | (_draws ? kDraws : 0) | (_grid_rows != 0 ? kRows : 0) |
                                   _tables);
    h.grid_rows = _grid_rows;
    Info &info = h.info;
    info.reactive = _sound == 3 ? Reactive::Required
                    : _sound == 2 || (_sound == 0 && _uses_sound) ? Reactive::Optional
                                                                  : Reactive::Never;
    info.uses_color = _uses_color;
    info.uses_palette = _uses_palette;

    // the parts together after the header: the constants (the scalars from const_base up, so the
    // last one made first), the instructions, their lines
    auto *base = reinterpret_cast<uint8_t *>(&_header);
    size_t at = (sizeof(Header) + 3) & ~size_t{3};
    std::reverse(_consts, _consts + _const_count);
    h.consts_at = static_cast<uint32_t>(at);
    std::memmove(base + at, _consts, _const_count * sizeof(Fix));
    at += _const_count * sizeof(Fix);
    h.code_at = static_cast<uint32_t>(at);
    std::memmove(base + at, _code, count * sizeof(Ins));
    at += count * sizeof(Ins);
    h.lines_at = static_cast<uint32_t>(at);
    std::memmove(base + at, _lines, count * sizeof(uint16_t));
    at += count * sizeof(uint16_t);
    h.size = static_cast<uint32_t>(at);
    return at;
}

} // namespace px::effects::glow::detail
