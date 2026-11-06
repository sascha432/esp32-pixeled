#pragma once

// Inside glow: the compiler (lexer.cpp the tokens, compiler.cpp the passes, declarations.cpp,
// statements.cpp and assignments.cpp, expressions.cpp, calls.cpp and registers.cpp the values).
// No syntax tree: the code is written while the source is read, in passes over it. The first pass
// collects the declarations; then the blocks are compiled, again when a pass found a local that
// became per pixel after it was used as one value for all pixels. A function is compiled where it
// is called (inlined), from its source.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "bytecode.hpp"

namespace px::effects::glow::detail {

// ------------------------------------------------------------------------------------- tokens

enum class Tok : uint8_t {
    End,
    Newline,
    Number,
    Name,
    String,
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    Comma,
    Semicolon,
    Question,
    Colon,
    DotDot,
    Assign,
    AddAssign,
    SubAssign,
    MulAssign,
    DivAssign,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Less,
    LessEq,
    Greater,
    GreaterEq,
    Equal,
    NotEqual,
    // && || ! (and, or, not)
    AndAnd,
    OrOr,
    Bang,
    // a character that is no token: Lexer::problem()
    Bad,
};

struct Token {
    Tok type = Tok::End;
    uint16_t line = 1;
    uint16_t column = 1;
    uint32_t at = 0;
    uint16_t length = 0;
    Fix number = 0;
};

// The tokens of a source. A newline is a token where it ends a statement: not inside ( ) or [ ].
class Lexer {
public:
    struct Mark {
        uint32_t at = 0;
        uint16_t line = 1;
        uint16_t column = 1;
        uint8_t depth = 0;
    };

    explicit Lexer(std::string_view source) : _source(source) {}

    Token next();
    Mark mark() const { return {_at, _line, _column, _depth}; }
    void reset(const Mark &m)
    {
        _at = m.at;
        _line = m.line;
        _column = m.column;
        _depth = m.depth;
    }
    std::string_view text(const Token &t) const { return _source.substr(t.at, t.length); }
    // what the last Tok::Bad was
    const char *problem() const { return _problem; }

private:
    char peek(size_t ahead = 0) const { return _at + ahead < _source.size() ? _source[_at + ahead] : '\0'; }
    void step();
    // skips spaces and comments; false at a newline that ends a statement
    bool skip_space();
    Token number(Token t);

    std::string_view _source;
    uint32_t _at = 0;
    uint16_t _line = 1;
    uint16_t _column = 1;
    uint8_t _depth = 0;
    const char *_problem = "";
};

// ------------------------------------------------------------------------------------ compiling

constexpr size_t kMaxName = 16;
constexpr size_t kMaxArgs = 6;
constexpr size_t kMaxFunctions = 16;
constexpr size_t kMaxGlobals = kMaxParams + kMaxVars + kMaxArrays + kMaxGrids + kMaxFunctions;
constexpr size_t kMaxLocals = 64;
// local declarations in one pass (calls of functions count each time)
constexpr size_t kMaxDecls = 512;
// blocks, parentheses, calls inside each other
constexpr size_t kMaxNesting = 16;
constexpr size_t kMaxLoops = 8;
constexpr size_t kMaxJumps = 8;
// the operators and values waiting in the expressions being read
constexpr size_t kMaxPending = 48;

enum class Kind : uint8_t {
    Const,
    Scalar,
    Vector,
};

// A value while compiling: a constant or a register
struct Val {
    Kind kind = Kind::Const;
    uint8_t reg = 0;
    // a register of its own: freed once used
    bool temp = false;
    // the same for every pixel during the whole frame (the per-pixel code computes it once a frame)
    bool invariant = true;
    Fix k = 0;
};

enum class Context : uint8_t {
    Init,
    Frame,
    Pixel,
};

enum class Sym : uint8_t {
    Param,
    Var,
    Array,
    Grid,
    Function,
};

struct Global {
    char name[kMaxName];
    Sym kind;
    uint8_t id;
};

struct Function {
    char args[kMaxArgs][kMaxName];
    uint8_t argc;
    // at its '{'
    Lexer::Mark body;
};

struct VarInit {
    // at the value after '='
    Lexer::Mark at;
    uint8_t var;
};

struct Local {
    char name[kMaxName];
    Val val;
    // the block it belongs to
    uint8_t depth;
    // a function's argument still the caller's value (copied once written)
    bool alias;
    // a loop's counter: not to be written
    bool counter;
    uint16_t decl;
};

struct Loop {
    uint16_t breaks[kMaxJumps];
    uint16_t continues[kMaxJumps];
    uint8_t break_count;
    uint8_t continue_count;
    // the masks open when it started (a per-pixel condition inside it cannot break out)
    uint8_t masks;
};

// a per-pixel condition: the lanes it is true for, opened at block depth `depth`
struct Mask {
    uint8_t reg;
    uint8_t depth;
};

struct Pending {
    Op op;
    uint8_t precedence;
    bool unary;
    // the operands the other way round (> as <)
    bool swap;
    uint16_t line;
    uint16_t column;
};

struct Inlined {
    uint8_t function;
    // its locals start here, and the loops it can break out of
    uint8_t locals;
    uint8_t loops;
};

enum class Fn : uint8_t {
    Abs,
    Floor,
    Ceil,
    Frac,
    Round,
    Sqrt,
    Sin,
    Cos,
    Wave,
    Tri,
    Saw,
    Square,
    Min,
    Max,
    Clamp,
    Mix,
    Step,
    Smooth,
    Atan2,
    Hypot,
    Hash,
    Noise,
    Rand,
    Time,
    Band,
    Hsv,
    Rgb,
    Pal,
    Col,
    Heat,
    Fade,
    Blur,
    Clear,
    Dot,
    Line,
    Disc,
    Blob,
};

struct BuiltinFunction {
    const char *name;
    Fn fn;
    uint8_t min;
    uint8_t max;
    // gives a value (else a statement)
    bool value;
};

// the built-in function of this name, nullptr for none
const BuiltinFunction *find_function(std::string_view name);

// a word of the language (if, for, ...); a built-in value or function (t, x, sin, hsv, ...)
bool is_keyword(std::string_view name);
bool builtin_name(std::string_view name);

// a name into a fixed array, cut to fit
inline void copy_name(char (&to)[kMaxName], std::string_view from)
{
    const size_t n = from.size() < kMaxName - 1 ? from.size() : kMaxName - 1;
    for (size_t i = 0; i < n; i++) {
        to[i] = from[i];
    }
    to[n] = '\0';
}

// The compiler, in the caller's workspace. Its first members are the program it builds: finish()
// moves the parts together, so the program is at the start of the workspace.
class Compiler {
public:
    Compiler(std::string_view source, Error &error);

    // the program's size, 0 on an error
    size_t run();

private:
    // ---- compiler.cpp: tokens, declarations, statements, the passes
    void advance();
    bool at(Tok t) const { return _tok.type == t; }
    bool at_word(const char *word) const;
    bool accept(Tok t);
    bool expect(Tok t, const char *what);
    void skip_newlines();
    void end_of_statement();
    bool else_follows();
    void fail(const char *format, ...);
    void fail_at(uint16_t line, uint16_t column, const char *format, ...);
    std::string_view text() const { return _lex.text(_tok); }
    bool enter();
    void leave() { _nesting--; }

    bool scan();
    void declare_param();
    void declare_vars();
    void declare_grids();
    void declare_function();
    bool record_block(Lexer::Mark &mark, bool &present);
    void skip_block();
    void skip_value();
    bool new_name(std::string_view name);
    int find_global(std::string_view name) const;
    bool signed_number(Fix &out);
    bool add_global(std::string_view name, Sym kind, uint8_t id);

    void reset_pass();
    void compile_init();
    void compile_block_at(const Lexer::Mark &mark, Context context);
    size_t finish();

    void block();
    void statements(bool function_body, Val *result);
    void statement();
    void if_statement();
    void for_statement();
    void repeat_statement();
    void while_statement();
    void jump_statement(bool is_break);
    bool open_loop();
    void loop_end(uint16_t top, uint16_t exit, uint8_t counter);
    void counted_loop(const Token *name, Val from, Val to);
    void assignment(const Token &name);
    void assign_local(const Token &name, Tok op, Val value);
    void assign_element(const Global &global, const Token &name);
    void open_scope() { _depth++; }
    // closes the innermost block: frees its locals' registers but `keep`'s
    void close_scope(const Val *keep = nullptr);
    Local *find_local(std::string_view name);
    Local *declare_local(std::string_view name, Val value);
    bool masked_for(const Local &local) const;
    void store_scalar(uint8_t reg, Val value);
    void store_vector(uint8_t reg, Val value, bool masked);
    void patch(uint16_t at, uint16_t target);

    // ---- expressions.cpp: values
    Val expression();
    Val binary_expression();
    Val primary();
    bool reduce();
    Val name_value(const Token &name);
    Val element(const Token &name, const Global &global);
    Val call(const Token &name, bool want_value);
    Val call_builtin(Fn fn, Val *args, size_t argc, const Token &name);
    Val call_function(uint8_t index, Val *args, size_t argc, const Token &name);
    Val color(Fn fn, Val *args, size_t argc);
    Val draw(Fn fn, Val *args, size_t argc, const Token &name);

    Val constant(Fix k) const { return {Kind::Const, 0, false, true, k}; }
    Val unary(Op base, Val a);
    Val binary(Op base, Val a, Val b);
    // binary() without turning a division by a constant into a multiplication
    Val binary_plain(Op base, Val a, Val b);
    Val nary(Op scalar, Op vector, Val *args, size_t argc, bool pure);
    Val select(Val c, Val a, Val b);
    Val borrow(const Val &v) const
    {
        Val b = v;
        b.temp = false;
        return b;
    }
    uint8_t sreg(const Val &v);
    uint8_t vreg(Val &v);
    void release(const Val &v);
    uint8_t konst(Fix k);
    uint8_t alloc_s(bool permanent);
    void free_s(uint8_t reg);
    uint8_t alloc_v();
    void free_v(uint8_t reg);
    uint16_t emit(Op op, uint8_t d = 0, uint8_t a = 0, uint8_t b = 0, uint8_t c = 0, uint8_t e = 0, uint16_t imm = 0,
                  bool once = false);
    uint16_t here() const { return _up; }
    bool varying(uint16_t decl) const { return (_varying[decl / 8] & (1u << (decl % 8))) != 0; }
    void make_varying(uint16_t decl);
    uint8_t mask() const { return _mask_count != 0 ? _masks[_mask_count - 1].reg : kNoMask; }

    // ---- the program as it is built (first: finish() moves it together at the workspace's start)
    Header _header;
    Fix _consts[kMaxConsts];
    Ins _code[kMaxCode];
    uint16_t _lines[kMaxCode];

    // ---- the source
    Lexer _lex;
    Token _tok;
    // where _tok starts (to read it again)
    Lexer::Mark _tok_mark;
    Token _last;
    Error *_error;
    bool _failed = false;
    uint8_t _nesting = 0;

    // ---- declarations
    Global _globals[kMaxGlobals];
    uint8_t _global_count = 0;
    Function _functions[kMaxFunctions];
    uint8_t _function_count = 0;
    VarInit _var_inits[kMaxVars];
    uint8_t _var_init_count = 0;
    Lexer::Mark _init_at;
    Lexer::Mark _frame_at;
    Lexer::Mark _pixel_at;
    bool _has_init = false;
    bool _has_frame = false;
    bool _has_pixel = false;
    // `sound`: 0 not given (inferred), 1 never, 2 optional, 3 required
    uint8_t _sound = 0;
    bool _named = false;

    // ---- a pass
    Context _context = Context::Init;
    uint16_t _up = 0;
    uint16_t _down = kMaxCode;
    Range _streams[kStreams] = {};
    uint8_t _const_count = 0;
    uint32_t _s_used[kScalars / 32] = {};
    uint32_t _s_permanent[kScalars / 32] = {};
    uint32_t _v_used = 0;
    uint8_t _v_high = 0;
    Local _locals[kMaxLocals];
    uint8_t _local_count = 0;
    uint8_t _depth = 0;
    Mask _masks[kMaxNesting];
    uint8_t _mask_count = 0;
    Loop _loops[kMaxLoops];
    uint8_t _loop_count = 0;
    Inlined _inlined[kMaxNesting];
    uint8_t _inlined_count = 0;
    Val _values[kMaxPending];
    uint8_t _value_count = 0;
    Pending _pending[kMaxPending];
    uint8_t _pending_count = 0;
    uint16_t _decl = 0;
    uint8_t _varying[kMaxDecls / 8] = {};
    bool _again = false;
    bool _uses_sound = false;
    bool _uses_color = false;
    bool _uses_palette = false;
    bool _draws = false;
    uint8_t _pixel_values = 0;
    uint8_t _grid_rows = 0;
    // kPalette, kHeat: the color tables the program reads
    uint8_t _tables = 0;
};

} // namespace px::effects::glow::detail
