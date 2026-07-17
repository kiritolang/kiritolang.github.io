#ifndef KIRITO_LEXER_HPP
#define KIRITO_LEXER_HPP

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"

namespace kirito {

enum class TokenType {
    Integer, Float, String, FString, Identifier,
    KwVar, KwTrue, KwFalse, KwNone,
    KwIf, KwElif, KwElse, KwWhile, KwBreak, KwContinue,
    KwAnd, KwOr, KwNot, KwFunction, KwReturn, KwFor, KwIn,
    KwTry, KwCatch, KwFinally, KwThrow, KwAs, KwClass, KwWith, KwPass, KwTodo, KwAssert, KwDiscard,
    KwSwitch,  // `case`/`default` are contextual (soft) keywords — lexed as identifiers, recognized
               // only inside a switch body — so they stay usable as ordinary names everywhere else.
    Plus, Minus, Star, Slash, SlashSlash, Percent, StarStar, Arrow,
    Assign, EqEq, NotEq, Lt, Le, Gt, Ge,
    LParen, RParen, LBracket, RBracket, LBrace, RBrace,
    Colon, Comma, Dot,
    Newline, Indent, Dedent, EndOfFile,
};

struct Token {
    TokenType type;
    std::string text;  // literal text for numbers; empty otherwise
    SourceSpan span;
    bool raw = false;  // set on an FString token from a raw prefix (rf"..."): suppress \-escapes in
                       // its literal segments. Plain String tokens are fully decoded in the lexer, so
                       // this never applies to them.
};

// Turns source text into a flat token stream. Tracks 1-based line/col so every token (and any
// error) can point at an exact location. Indentation handling arrives in a later milestone.
class Lexer {
public:
    explicit Lexer(std::string_view source) : src_(normalizeNewlines(source)) {}
    // Seed the starting line/col so a sub-lexed fragment (an f-string's embedded `{expr}`) produces
    // spans ABSOLUTE to the enclosing source file rather than relative to line 1 — so errors inside an
    // f-string report the real file location (A02-2).
    Lexer(std::string_view source, uint32_t startLine, uint32_t startCol)
        : src_(normalizeNewlines(source)), line_(startLine), col_(startCol) {}

    // Universal newlines: collapse CRLF and lone CR to a single LF up front, so a file authored on
    // Windows (or copied through a CRLF filesystem, e.g. Windows -> WSL) lexes identically to Unix
    // LF. Indentation, blank-line detection and multiline strings are then all measured against '\n'
    // alone — without this, a '\r' left on a blank line defeats the blank-line check and corrupts the
    // indent/dedent stream. Universal-newline source handling.
    static std::string normalizeNewlines(std::string_view s) {
        // A leading UTF-8 BOM is a byte-order marker, not source: the same Windows editors this
        // function already forgives for CRLF write one by default, and without this the file dies on
        // its first byte ("unexpected character") though it is otherwise perfectly valid Kirito.
        if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
            static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF)
            s.remove_prefix(3);
        std::string out;
        out.reserve(s.size());
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\r') {
                out.push_back('\n');
                if (i + 1 < s.size() && s[i + 1] == '\n') ++i;  // CRLF -> one LF
            } else {
                out.push_back(s[i]);
            }
        }
        return out;
    }

    // The newline-normalized source the lexer worked over. Token line/col are absolute into THIS text,
    // so the parser can map a token back to a byte offset (see Parser's line-start index) to capture a
    // construct's verbatim source for serialization.
    const std::string& source() const { return src_; }

    std::vector<Token> tokenize() {
        std::vector<Token> out;
        indent_ = {{0, 0}};
        bool lineStart = true;
        while (pos_ < src_.size()) {
            if (lineStart && parenDepth_ == 0) {
                if (!handleIndentation(out)) continue;  // blank/comment line: nothing emitted
                lineStart = false;
                continue;
            }
            char c = src_[pos_];
            const std::size_t tokStart = pos_;
            const std::size_t emitted = out.size();
            if (c == ' ' || c == '\t' || c == '\r') {
                advance();
            } else if (c == '#') {
                while (pos_ < src_.size() && src_[pos_] != '\n') advance();
            } else if (c == '\n') {
                if (parenDepth_ == 0) {
                    out.push_back(make(TokenType::Newline, line_, col_));
                    advance();
                    lineStart = true;
                } else {
                    advance();  // newline is insignificant inside (), line continuation
                }
            } else if (std::isdigit(static_cast<unsigned char>(c))) {
                out.push_back(number());
            } else if (c == '"' || c == '\'') {
                out.push_back(stringLiteral(/*isRaw=*/false, /*isF=*/false));
            } else if ((c == 'r' || c == 'R' || c == 'f' || c == 'F') && tryStringLiteral(out)) {
                // a prefixed string literal (r"..", f"..", rf".." / fr"..) was lexed
            } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                out.push_back(identifier());
            } else {
                out.push_back(op());
            }
            // A token's exact source extent, which the parser needs to slice a Function/class
            // literal's verbatim source (Parser::captureSource). Whitespace and comments emit no
            // token, so they are never covered; the zero-width Indent/Dedent markers that
            // handleIndentation emits above keep length 0.
            if (out.size() > emitted)
                out.back().span.length = static_cast<uint32_t>(pos_ - tokStart);
        }
        // Close the final logical line, then unwind any open indentation, then EOF.
        if (!out.empty() && out.back().type != TokenType::Newline)
            out.push_back(make(TokenType::Newline, line_, col_));
        while (indent_.size() > 1) {
            indent_.pop_back();
            out.push_back(make(TokenType::Dedent, line_, col_));
        }
        out.push_back(make(TokenType::EndOfFile, line_, col_));
        return out;
    }

private:
    void advance() {
        if (src_[pos_] == '\n') { ++line_; col_ = 1; } else { ++col_; }
        ++pos_;
    }
    char peek(size_t ahead = 0) const {
        size_t i = pos_ + ahead;
        return i < src_.size() ? src_[i] : '\0';
    }
    // True at end of input. Distinct from `peek() == '\0'`, which is also true for a genuine embedded
    // NUL byte — so end-of-input tests must use this, not the '\0' sentinel (a NUL in a string literal
    // is a valid character, not a premature terminator).
    bool atEnd() const { return pos_ >= src_.size(); }
    Token make(TokenType t, uint32_t line, uint32_t col, std::string text = {}, bool raw = false) const {
        return Token{t, std::move(text), SourceSpan{line, col, 0}, raw};
    }

    // At a logical line start: measure indentation and emit Indent / Dedent(s). Blank lines and
    // comment-only lines yield no layout tokens (returns false after consuming the line).
    //
    // Tabs and spaces are both allowed, but indentation must be UNAMBIGUOUS: every indent is
    // measured two ways — tabs as 8 columns ("wide") and tabs as 1 column ("narrow") — and the two
    // measures must agree on the relation (deeper/same/shallower) to each enclosing level. This is
    // the indentation rule and rejects e.g. a tab where the surrounding block used 8 spaces.
    bool handleIndentation(std::vector<Token>& out) {
        int64_t wide = 0, narrow = 0;  // int64: a pathological run of leading tabs/spaces must not
        size_t scan = pos_;            // overflow (wide grows up to 8x the byte count)
        while (scan < src_.size()) {
            char c = src_[scan];
            if (c == ' ') { ++wide; ++narrow; ++scan; }
            else if (c == '\t') { wide += 8 - (wide % 8); ++narrow; ++scan; }
            else break;
        }
        // Blank or comment-only line: skip to (and over) the newline, emit nothing.
        if (scan >= src_.size() || src_[scan] == '\n' || src_[scan] == '#') {
            while (pos_ < src_.size() && src_[pos_] != '\n') advance();
            if (pos_ < src_.size()) advance();
            return false;
        }
        while (pos_ < scan) advance();  // step over the leading whitespace

        auto ambiguous = [&] {
            throw KiritoError("inconsistent use of tabs and spaces in indentation",
                              SourceSpan{line_, 1, 0});
        };
        Indent cur{wide, narrow};
        const Indent top = indent_.back();
        if (cur.wide == top.wide) {
            if (cur.narrow != top.narrow) ambiguous();  // same wide width, different tab usage
        } else if (cur.wide > top.wide) {
            if (cur.narrow <= top.narrow) ambiguous();
            indent_.push_back(cur);
            out.push_back(make(TokenType::Indent, line_, col_));
        } else {
            if (cur.narrow >= top.narrow) ambiguous();
            while (cur.wide < indent_.back().wide) {
                indent_.pop_back();
                out.push_back(make(TokenType::Dedent, line_, col_));
            }
            if (cur.wide != indent_.back().wide) throw KiritoError("inconsistent dedent", SourceSpan{line_, 1, 0});
            if (cur.narrow != indent_.back().narrow) ambiguous();
        }
        return true;
    }

    Token number() {
        uint32_t line = line_, col = col_;
        std::string text;
        bool isFloat = false;
        // Base-prefixed integer literals: 0x.. (hex), 0b.. (binary), 0o.. (octal). The prefix and
        // digits are kept in `text`; the parser decodes the base. At least one base digit is required.
        if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X' || peek(1) == 'b' ||
                              peek(1) == 'B' || peek(1) == 'o' || peek(1) == 'O')) {
            char base = peek(1);
            bool hex = (base == 'x' || base == 'X'), bin = (base == 'b' || base == 'B');
            text += peek(); advance();   // 0
            text += peek(); advance();   // x / b / o
            auto isBaseDigit = [&](char c) {
                if (hex) return std::isxdigit(static_cast<unsigned char>(c)) != 0;
                if (bin) return c == '0' || c == '1';
                return c >= '0' && c <= '7';
            };
            bool any = false;
            while (isBaseDigit(peek())) { text += peek(); advance(); any = true; }
            if (!any)
                throw KiritoError("invalid numeric literal '" + text + "'",
                                  SourceSpan{line, col, static_cast<uint32_t>(text.size())});
            return make(TokenType::Integer, line, col, std::move(text));
        }
        while (std::isdigit(static_cast<unsigned char>(peek()))) { text += peek(); advance(); }
        // A well-formed exponent at offset k: `e`/`E`, an optional sign, then at least one digit.
        // Anything less is not an exponent, so a bare identifier after a number (`1 else`) and a
        // method call on a literal (`1.compare(x)`) are left alone.
        auto exponentAt = [&](std::size_t k) {
            if (peek(k) != 'e' && peek(k) != 'E') return false;
            ++k;
            if (peek(k) == '+' || peek(k) == '-') ++k;
            return std::isdigit(static_cast<unsigned char>(peek(k))) != 0;
        };
        // The fraction: a digit after the '.' (1.5), or an exponent directly after it (1.e5 — how
        // every C-family language spells it). Without the latter the '.' would fall through to op(),
        // and `1.e5` would silently become member access (1).e5, failing only at RUNTIME with
        // "type 'Integer' has no attribute 'e5'".
        if (peek() == '.' && (std::isdigit(static_cast<unsigned char>(peek(1))) || exponentAt(1))) {
            isFloat = true;
            text += peek(); advance();
            while (std::isdigit(static_cast<unsigned char>(peek()))) { text += peek(); advance(); }
        }
        // Scientific notation makes it a Float (1e10, 1.5e3, 2e-3, 1E5).
        if (exponentAt(0)) {
            isFloat = true;
            text += peek(); advance();                                   // e/E
            if (peek() == '+' || peek() == '-') { text += peek(); advance(); }
            while (std::isdigit(static_cast<unsigned char>(peek()))) { text += peek(); advance(); }
        }
        return make(isFloat ? TokenType::Float : TokenType::Integer, line, col, std::move(text));
    }

    Token identifier() {
        uint32_t line = line_, col = col_;
        std::string text;
        while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_') {
            text += peek();
            advance();
        }
        TokenType type = TokenType::Identifier;
        if (text == "var") type = TokenType::KwVar;
        else if (text == "True") type = TokenType::KwTrue;
        else if (text == "False") type = TokenType::KwFalse;
        else if (text == "None") type = TokenType::KwNone;
        else if (text == "if") type = TokenType::KwIf;
        else if (text == "elif") type = TokenType::KwElif;
        else if (text == "else") type = TokenType::KwElse;
        else if (text == "while") type = TokenType::KwWhile;
        else if (text == "break") type = TokenType::KwBreak;
        else if (text == "continue") type = TokenType::KwContinue;
        else if (text == "and") type = TokenType::KwAnd;
        else if (text == "or") type = TokenType::KwOr;
        else if (text == "not") type = TokenType::KwNot;
        else if (text == "Function") type = TokenType::KwFunction;
        else if (text == "return") type = TokenType::KwReturn;
        else if (text == "for") type = TokenType::KwFor;
        else if (text == "in") type = TokenType::KwIn;
        else if (text == "try") type = TokenType::KwTry;
        else if (text == "catch") type = TokenType::KwCatch;
        else if (text == "finally") type = TokenType::KwFinally;
        else if (text == "throw") type = TokenType::KwThrow;
        else if (text == "as") type = TokenType::KwAs;
        else if (text == "class") type = TokenType::KwClass;
        else if (text == "with") type = TokenType::KwWith;
        else if (text == "pass") type = TokenType::KwPass;
        else if (text == "todo") type = TokenType::KwTodo;
        else if (text == "assert") type = TokenType::KwAssert;
        else if (text == "discard") type = TokenType::KwDiscard;
        else if (text == "switch") type = TokenType::KwSwitch;
        return make(type, line, col, std::move(text));
    }

    // Probe for a prefixed string literal at the current position: an optional `r` (raw) and/or `f`
    // (f-string) prefix — in either order, each at most once, case-insensitive — immediately followed
    // by a quote. If matched, lex it and return true; otherwise consume nothing and return false (so
    // the caller falls through to identifier lexing — `rf` alone is just a name).
    bool tryStringLiteral(std::vector<Token>& out) {
        std::size_t p = pos_;
        bool isRaw = false, isF = false;
        while (p < src_.size()) {
            char c = src_[p];
            if ((c == 'r' || c == 'R') && !isRaw) { isRaw = true; ++p; }
            else if ((c == 'f' || c == 'F') && !isF) { isF = true; ++p; }
            else break;
        }
        if (p >= src_.size() || (src_[p] != '"' && src_[p] != '\'')) return false;
        out.push_back(stringLiteral(isRaw, isF));
        return true;
    }

    // Unified string-literal lexer covering every spelling: plain/`r`/`f`/`rf`, single- or
    // double-quoted, and single-line or triple-quoted (multiline). Plain strings are decoded here
    // (escapes resolved, or kept verbatim when raw); f-strings keep their inner text RAW for the
    // parser to split into literal/`{expr}` parts, with `raw` recorded so the parser knows whether to
    // decode escapes in the literal pieces.
    Token stringLiteral(bool isRaw, bool isF) {
        uint32_t line = line_, col = col_;
        while (peek() == 'r' || peek() == 'R' || peek() == 'f' || peek() == 'F') advance();  // prefix
        char q = peek();
        advance();  // opening quote
        bool triple = (peek() == q && peek(1) == q);
        if (triple) { advance(); advance(); }
        const char* what = isF ? "f-string" : "string";

        auto atClose = [&]() -> bool {
            if (peek() != q) return false;
            return !triple || (peek(1) == q && peek(2) == q);
        };
        auto unterminated = [&]() -> KiritoError {
            return KiritoError(std::string("unterminated ") + (triple ? "triple-quoted " : "") + what,
                               SourceSpan{line, col, 1});
        };

        std::string text;
        while (!atClose()) {
            if (atEnd()) throw unterminated();     // real end-of-input (a NUL byte is a valid char, below)
            char c = peek();
            if (c == '\n' && !triple) throw unterminated();  // single-line forms can't span lines
            if (c == '\\') {
                // f-strings (raw or not) keep escapes verbatim; the parser decodes them later, so a
                // backslash here also shields the next char from terminating the literal.
                if (isF || isRaw) {
                    text += c; advance();
                    if (atEnd()) throw unterminated();
                    char e = peek();
                    if (e == '\n' && !triple) throw unterminated();
                    text += e; advance();
                    continue;
                }
                text += decodeEscape();  // cooked plain string: resolve the escape now
                continue;
            }
            text += c;
            advance();
        }
        advance();  // closing quote
        if (triple) { advance(); advance(); }
        return make(isF ? TokenType::FString : TokenType::String, line, col, std::move(text), isRaw);
    }

    // Decode one backslash escape of a cooked (non-raw) plain string, with the backslash at peek().
    // Returns the decoded text (usually one char; possibly more for future multi-char escapes).
    std::string decodeEscape() {
        // Delegate to the single shared cooked-escape decoder (common.hpp) so the plain-string and
        // f-string spellings can never diverge; attach this lexer's span on error.
        std::string out, err;
        std::size_t consumed = decodeCookedEscape(src_, pos_, out, err);
        if (consumed == 0) throw KiritoError(err, SourceSpan{line_, col_, 1});
        for (std::size_t k = 0; k < consumed; ++k) advance();
        return out;
    }

    Token op() {
        uint32_t line = line_, col = col_;
        char c = peek();
        switch (c) {
            case '+': { advance(); return make(TokenType::Plus, line, col); } break;
            case '-': {
                advance();
                if (peek() == '>') { advance(); return make(TokenType::Arrow, line, col); }
                return make(TokenType::Minus, line, col);
            } break;
            case '*': {
                advance();
                if (peek() == '*') { advance(); return make(TokenType::StarStar, line, col); }
                return make(TokenType::Star, line, col);
            } break;
            case '/': {
                advance();
                if (peek() == '/') { advance(); return make(TokenType::SlashSlash, line, col); }
                return make(TokenType::Slash, line, col);
            } break;
            case '%': { advance(); return make(TokenType::Percent, line, col); } break;
            case ':': { advance(); return make(TokenType::Colon, line, col); } break;
            case ',': { advance(); return make(TokenType::Comma, line, col); } break;
            case '.': { advance(); return make(TokenType::Dot, line, col); } break;
            case '(': { advance(); ++parenDepth_; return make(TokenType::LParen, line, col); } break;
            case ')': {
                advance();
                if (parenDepth_ > 0) --parenDepth_;
                return make(TokenType::RParen, line, col);
            } break;
            case '[': { advance(); ++parenDepth_; return make(TokenType::LBracket, line, col); } break;
            case ']': {
                advance();
                if (parenDepth_ > 0) --parenDepth_;
                return make(TokenType::RBracket, line, col);
            } break;
            case '{': { advance(); ++parenDepth_; return make(TokenType::LBrace, line, col); } break;
            case '}': {
                advance();
                if (parenDepth_ > 0) --parenDepth_;
                return make(TokenType::RBrace, line, col);
            } break;
            case '=': {
                advance();
                if (peek() == '=') { advance(); return make(TokenType::EqEq, line, col); }
                return make(TokenType::Assign, line, col);
            } break;
            case '!': {
                advance();
                if (peek() == '=') { advance(); return make(TokenType::NotEq, line, col); }
                throw KiritoError("unexpected '!' (did you mean '!=' or 'not'?)",
                                  SourceSpan{line, col, 1});
            } break;
            case '<': {
                advance();
                if (peek() == '=') { advance(); return make(TokenType::Le, line, col); }
                return make(TokenType::Lt, line, col);
            } break;
            case '>': {
                advance();
                if (peek() == '=') { advance(); return make(TokenType::Ge, line, col); }
                return make(TokenType::Gt, line, col);
            } break;
            default: {
                // Render the offending byte printably: a raw non-ASCII/control byte (a UTF-8
                // continuation byte, an embedded NUL) spliced straight into the message corrupts or
                // truncates the diagnostic (A01-3). Show it as \xHH instead.
                unsigned char uc = static_cast<unsigned char>(c);
                std::string shown;
                if (uc >= 0x20 && uc < 0x7f) {
                    shown = std::string(1, c);
                } else {
                    static const char kHex[] = "0123456789abcdef";
                    shown = "\\x";
                    shown += kHex[uc >> 4];
                    shown += kHex[uc & 0xf];
                }
                throw KiritoError("unexpected character '" + shown + "'",
                                  SourceSpan{line, col, 1});
            } break;
        }
    }

    std::string src_;  // owned, newline-normalized (see normalizeNewlines)
    size_t pos_ = 0;
    uint32_t line_ = 1;
    uint32_t col_ = 1;
    struct Indent { int64_t wide; int64_t narrow; };  // tab-as-8 and tab-as-1 column measures (int64: a
                                                       // huge leading-whitespace run must not overflow)
    std::vector<Indent> indent_{{0, 0}};
    int parenDepth_ = 0;
};

}  // namespace kirito

#endif
