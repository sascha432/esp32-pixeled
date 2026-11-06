// The tokens of a script (docs/glow.md "Syntax"): names, numbers (16.16), strings, operators;
// comments // and /* */; a newline ends a statement outside ( ) and [ ].

#include "compiler.hpp"

namespace px::effects::glow::detail {

namespace {

bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

bool is_name_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

} // namespace

void Lexer::step()
{
    if (_at < _source.size()) {
        if (_source[_at] == '\n') {
            _line++;
            _column = 1;
        } else {
            _column++;
        }
        _at++;
    }
}

bool Lexer::skip_space()
{
    for (;;) {
        const char c = peek();
        if (c == ' ' || c == '\t' || c == '\r') {
            step();
        } else if (c == '\n') {
            if (_depth == 0) {
                return false;
            }
            step();
        } else if (c == '/' && peek(1) == '/') {
            while (peek() != '\n' && peek() != '\0') {
                step();
            }
        } else if (c == '/' && peek(1) == '*') {
            step();
            step();
            while (peek() != '\0' && !(peek() == '*' && peek(1) == '/')) {
                step();
            }
            step();
            step();
        } else {
            return true;
        }
    }
}

Token Lexer::number(Token t)
{
    // the integer part, then up to 9 digits of the fraction, rounded to 1/65536
    uint32_t whole = 0;
    bool big = false;
    while (is_digit(peek())) {
        whole = whole * 10 + static_cast<uint32_t>(peek() - '0');
        big = big || whole > 32767;
        whole = whole > 100000 ? 100000 : whole;
        step();
    }
    uint64_t fraction = 0;
    uint64_t scale = 1;
    if (peek() == '.' && is_digit(peek(1))) {
        step();
        while (is_digit(peek())) {
            if (scale < 1000000000) {
                fraction = fraction * 10 + static_cast<uint32_t>(peek() - '0');
                scale *= 10;
            }
            step();
        }
    }
    t.length = static_cast<uint16_t>(_at - t.at);
    if (big) {
        t.type = Tok::Bad;
        _problem = "the number is too large (at most 32767)";
        return t;
    }
    const int64_t value = static_cast<int64_t>(whole) * 65536 + static_cast<int64_t>((fraction * 65536 + scale / 2) / scale);
    t.type = Tok::Number;
    t.number = value > 0x7fffffff ? 0x7fffffff : static_cast<Fix>(value);
    return t;
}

Token Lexer::next()
{
    Token t;
    if (!skip_space()) {
        // a newline: one token for any number of them (and the comments between)
        t.type = Tok::Newline;
        t.line = _line;
        t.column = _column;
        t.at = _at;
        while (peek() == '\n' || !skip_space()) {
            step();
        }
        return t;
    }
    t.line = _line;
    t.column = _column;
    t.at = _at;
    const char c = peek();
    if (c == '\0') {
        t.type = Tok::End;
        return t;
    }
    if (is_digit(c) || (c == '.' && is_digit(peek(1)))) {
        return number(t);
    }
    if (is_name_start(c)) {
        while (is_name_start(peek()) || is_digit(peek())) {
            step();
        }
        t.type = Tok::Name;
        t.length = static_cast<uint16_t>(_at - t.at);
        return t;
    }
    if (c == '"') {
        step();
        t.at = _at;
        while (peek() != '"' && peek() != '\n' && peek() != '\0') {
            step();
        }
        if (peek() != '"') {
            t.type = Tok::Bad;
            _problem = "the text has no closing \"";
            return t;
        }
        t.type = Tok::String;
        t.length = static_cast<uint16_t>(_at - t.at);
        step();
        return t;
    }
    const char d = peek(1);
    auto two = [&](Tok type) {
        step();
        step();
        t.type = type;
        t.length = 2;
        return t;
    };
    auto one = [&](Tok type) {
        step();
        t.type = type;
        t.length = 1;
        return t;
    };
    switch (c) {
    case '(':
        _depth++;
        return one(Tok::LParen);
    case ')':
        _depth = _depth > 0 ? static_cast<uint8_t>(_depth - 1) : 0;
        return one(Tok::RParen);
    case '[':
        _depth++;
        return one(Tok::LBracket);
    case ']':
        _depth = _depth > 0 ? static_cast<uint8_t>(_depth - 1) : 0;
        return one(Tok::RBracket);
    case '{':
        return one(Tok::LBrace);
    case '}':
        return one(Tok::RBrace);
    case ',':
        return one(Tok::Comma);
    case ';':
        return one(Tok::Semicolon);
    case '?':
        return one(Tok::Question);
    case ':':
        return one(Tok::Colon);
    case '.':
        if (d == '.') {
            return two(Tok::DotDot);
        }
        break;
    case '=':
        return d == '=' ? two(Tok::Equal) : one(Tok::Assign);
    case '+':
        return d == '=' ? two(Tok::AddAssign) : one(Tok::Plus);
    case '-':
        return d == '=' ? two(Tok::SubAssign) : one(Tok::Minus);
    case '*':
        return d == '=' ? two(Tok::MulAssign) : one(Tok::Star);
    case '/':
        return d == '=' ? two(Tok::DivAssign) : one(Tok::Slash);
    case '%':
        return one(Tok::Percent);
    case '<':
        return d == '=' ? two(Tok::LessEq) : one(Tok::Less);
    case '>':
        return d == '=' ? two(Tok::GreaterEq) : one(Tok::Greater);
    case '!':
        return d == '=' ? two(Tok::NotEqual) : one(Tok::Bang);
    case '&':
        if (d == '&') {
            return two(Tok::AndAnd);
        }
        break;
    case '|':
        if (d == '|') {
            return two(Tok::OrOr);
        }
        break;
    default:
        break;
    }
    step();
    t.type = Tok::Bad;
    t.length = 1;
    _problem = "a character that means nothing here";
    return t;
}

} // namespace px::effects::glow::detail
