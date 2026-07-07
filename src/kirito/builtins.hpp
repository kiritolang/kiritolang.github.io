#ifndef KIRITO_BUILTINS_HPP
#define KIRITO_BUILTINS_HPP
#include <span>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "arena.hpp"
#include "object.hpp"

namespace kirito {

// Byte offset of each UTF-8 code point in s (so strings index/slice by character, not byte).
inline std::vector<std::size_t> utf8Starts(const std::string& s) {
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i < s.size();) {
        starts.push_back(i);
        unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3
                      : (c >> 3) == 0x1E ? 4 : 1;
        i += n;
    }
    return starts;
}
inline std::size_t utf8Length(const std::string& s) { return utf8Starts(s).size(); }

// Decode the single code point starting at byte `i` (caller guarantees a valid start). Returns the
// code point; advances nothing (the byte width is recoverable from the lead byte).
inline unsigned utf8DecodeAt(const std::string& s, std::size_t i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) return c;
    if ((c >> 5) == 0x6 && i + 1 < s.size())
        return ((c & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
    if ((c >> 4) == 0xE && i + 2 < s.size())
        return ((c & 0x0F) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
               (static_cast<unsigned char>(s[i + 2]) & 0x3F);
    if ((c >> 3) == 0x1E && i + 3 < s.size())
        return ((c & 0x07) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12) |
               ((static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6) |
               (static_cast<unsigned char>(s[i + 3]) & 0x3F);
    return c;
}

// Unicode case mapping for the ranges that matter for European text: ASCII (U+0041..), Latin-1
// Supplement (U+00C0..U+00FF), and Latin Extended-A (U+0100..U+017F, which covers all Polish
// letters ąćęłńóśźż and friends). Outside these ranges the code point is returned unchanged. Full
// Unicode case folding (Greek, Cyrillic, special cases) is a future enrichment.
inline unsigned utf8ToUpperCp(unsigned cp) {
    if (cp >= 'a' && cp <= 'z') return cp - 32;
    if (cp >= 0x00E0 && cp <= 0x00FE && cp != 0x00F7) return cp - 32;  // à..þ (skip ÷)
    if (cp == 0x00FF) return 0x0178;                                   // ÿ -> Ÿ
    if (cp >= 0x0100 && cp <= 0x017F) {
        if (cp == 0x0131) return 'I';                                  // ı -> I
        if (cp == 0x017F) return 'S';                                  // ſ -> S
        if (cp >= 0x0139 && cp <= 0x0148) return (cp % 2 == 0) ? cp - 1 : cp;  // Ĺ..ň: odd=upper
        // U+0100..U+0137 and U+014A..U+0177 alternate upper(even)/lower(odd).
        if ((cp <= 0x0137) || (cp >= 0x014A && cp <= 0x0177)) return (cp % 2 == 1) ? cp - 1 : cp;
        if (cp >= 0x0179 && cp <= 0x017E) return (cp % 2 == 0) ? cp - 1 : cp;  // Ź..ž: even=upper
    }
    return cp;
}
inline unsigned utf8ToLowerCp(unsigned cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + 32;
    if (cp >= 0x00C0 && cp <= 0x00DE && cp != 0x00D7) return cp + 32;  // À..Þ (skip ×)
    if (cp == 0x0178) return 0x00FF;                                   // Ÿ -> ÿ
    if (cp >= 0x0100 && cp <= 0x017F) {
        if (cp >= 0x0139 && cp <= 0x0148) return (cp % 2 == 1) ? cp + 1 : cp;  // Ĺ..ň
        if ((cp <= 0x0137) || (cp >= 0x014A && cp <= 0x0177)) return (cp % 2 == 0) ? cp + 1 : cp;
        if (cp >= 0x0179 && cp <= 0x017E) return (cp % 2 == 1) ? cp + 1 : cp;  // Ź..ž
    }
    return cp;
}

// Append code point `cp` to `out` as UTF-8.
inline void utf8Encode(unsigned cp, std::string& out) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Render a double: shortest sensible form, always with a decimal point
// (so 2.0 prints "2.0", not "2"), while non-finite values pass through.
inline std::string floatToString(double d) {
    if (std::isnan(d)) return "nan";  // a NaN's sign bit is meaningless, but glibc %.15g leaks a set
                                      // one as "-nan" (e.g. inf - inf) — canonicalise every NaN to "nan".
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.15g", d);
    // Round-trip-overflow guard: %.15g rounds DBL_MAX *up* to 1.79769313486232e+308, which re-parses
    // to +inf (and even fails String->Float). For a finite value whose 15-digit form overflows, fall
    // back to the exact 17-digit form (always round-trips, never overflows). Normal values keep %.15g.
    if (std::isfinite(d) && !std::isfinite(std::strtod(buf, nullptr)))
        std::snprintf(buf, sizeof(buf), "%.17g", d);
    std::string s(buf);
    if (s.find_first_of(".eEni") == std::string::npos) s += ".0";
    return s;
}

// Render a double in its SHORTEST form that still round-trips exactly (parse(repr(d)) == d).
// Used where a float must survive a serialize/parse cycle (json) — unlike the
// display floatToString, which favours a clean %.15g and so isn't always round-trippable. Clean for
// ordinary values (0.1 -> "0.1") yet exact for awkward ones (0.1+0.2 -> "0.30000000000000004").
inline std::string floatToRoundtrip(double d) {
    if (!std::isfinite(d)) return floatToString(d);
    char buf[32];
    for (int prec = 15; prec <= 17; ++prec) {
        std::snprintf(buf, sizeof(buf), "%.*g", prec, d);
        if (std::strtod(buf, nullptr) == d) break;  // shortest precision that round-trips
    }
    std::string s(buf);
    if (s.find_first_of(".eEni") == std::string::npos) s += ".0";
    return s;
}

// The unit value. Interned once per VM, so every `None` shares one arena slot.
class NoneVal : public Object {
public:
    ValueKind kind() const override { return ValueKind::None; }
    std::string typeName() const override { return "None"; }
    bool truthy() const override { return false; }
    std::string str(StringifyCtx&) const override { return "None"; }
    bool equals(const ObjectArena&, const Object& other) const override {
        return other.kind() == ValueKind::None;
    }
    bool hashable() const override { return true; }
    std::size_t hash() const override { return 0; }
};

// Boolean. Interned per VM (True/False each share one slot).
class BoolVal : public Object {
public:
    explicit BoolVal(bool v) : value_(v) {}
    bool value() const { return value_; }

    ValueKind kind() const override { return ValueKind::Bool; }
    std::string typeName() const override { return "Bool"; }
    bool truthy() const override { return value_; }
    std::string str(StringifyCtx&) const override { return value_ ? "True" : "False"; }
    bool equals(const ObjectArena&, const Object& other) const override {
        return other.kind() == ValueKind::Bool &&
               static_cast<const BoolVal&>(other).value_ == value_;
    }
    bool hashable() const override { return true; }
    std::size_t hash() const override { return value_ ? 1 : 0; }

private:
    bool value_;
};

// Immutable text. Hash is computed once and cached.
class StrVal : public Object {
public:
    explicit StrVal(std::string v) : value_(std::move(v)), hash_(std::hash<std::string>{}(value_)) {
        ascii_ = true;
        for (unsigned char c : value_) if (c >= 0x80) { ascii_ = false; break; }
    }
    const std::string& value() const { return value_; }

    // A pure-ASCII string maps code-point index == byte index, so indexing/slicing/length are O(1)
    // with no scan. Computed once at construction (the value is immutable).
    bool isAscii() const { return ascii_; }
    // The byte offset of each code point. For non-ASCII strings this is built once and cached, so
    // repeated indexing (e.g. a lexer scanning char by char) is not O(n) per access. ASCII strings
    // never need it (use the byte index directly).
    const std::vector<std::size_t>& codePointStarts() const {
        if (!starts_.has_value()) starts_ = utf8Starts(value_);
        return *starts_;
    }

    ValueKind kind() const override { return ValueKind::String; }
    std::string typeName() const override { return "String"; }
    bool truthy() const override { return !value_.empty(); }
    std::string str(StringifyCtx&) const override { return value_; }
    bool equals(const ObjectArena&, const Object& other) const override {
        return other.kind() == ValueKind::String &&
               static_cast<const StrVal&>(other).value_ == value_;
    }
    bool hashable() const override { return true; }
    std::size_t hash() const override { return hash_; }
    std::optional<int64_t> length(KiritoVM&) override {
        return static_cast<int64_t>(ascii_ ? value_.size() : codePointStarts().size());
    }

    Handle binary(KiritoVM&, BinOp, Handle self, Handle rhs) override;
    Handle getItem(KiritoVM&, std::span<const Handle> keys) override;
    Handle slice(KiritoVM&, Handle start, Handle stop, Handle step) override;
    std::optional<std::vector<Handle>> iterate(KiritoVM&) override;
    bool contains(KiritoVM&, Handle value) override;
    Handle getAttr(KiritoVM&, Handle self, std::string_view name) override;
    std::vector<std::string> inspectMembers() const override;  // String's methods (runtime.hpp)

private:
    std::string value_;
    std::size_t hash_;
    bool ascii_;
    mutable std::optional<std::vector<std::size_t>> starts_;  // lazily built code-point offsets (non-ASCII)
};

// 64-bit signed integer.
class IntVal : public Object {
public:
    explicit IntVal(int64_t v) : value_(v) {}
    int64_t value() const { return value_; }

    ValueKind kind() const override { return ValueKind::Integer; }
    std::string typeName() const override { return "Integer"; }
    bool truthy() const override { return value_ != 0; }
    std::string str(StringifyCtx&) const override { return std::to_string(value_); }
    bool equals(const ObjectArena&, const Object& other) const override;
    bool hashable() const override { return true; }
    std::size_t hash() const override { return std::hash<int64_t>{}(value_); }

    Handle binary(KiritoVM&, BinOp, Handle self, Handle rhs) override;
    Handle unary(KiritoVM&, UnOp, Handle self) override;
    Handle getAttr(KiritoVM&, Handle self, std::string_view name) override;  // .compare() (runtime.hpp)
    std::vector<std::string> inspectMembers() const override;

private:
    int64_t value_;
};

// Double-precision floating point. Equality is EXACT IEEE-754 (see floatEqual in runtime.hpp); use
// the .compare(other, rel_tol=, abs_tol=) method for approximate comparison.
class FloatVal : public Object {
public:
    explicit FloatVal(double v) : value_(v) {}
    double value() const { return value_; }

    ValueKind kind() const override { return ValueKind::Float; }
    std::string typeName() const override { return "Float"; }
    bool truthy() const override { return value_ != 0.0; }
    std::string str(StringifyCtx&) const override { return floatToString(value_); }
    bool equals(const ObjectArena&, const Object& other) const override;
    bool hashable() const override { return true; }
    std::size_t hash() const override;

    Handle binary(KiritoVM&, BinOp, Handle self, Handle rhs) override;
    Handle unary(KiritoVM&, UnOp, Handle self) override;
    Handle getAttr(KiritoVM&, Handle self, std::string_view name) override;  // .compare() (runtime.hpp)
    std::vector<std::string> inspectMembers() const override;

private:
    double value_;
};

// repr of a String: quoted and escaped, so it reads back unambiguously when shown inside
// a container ([""] vs [] must differ; ["a", "b"] not [a, b]). Prefers single quotes, switching to
// double only when the text has a single quote but no double.
inline std::string reprString(const std::string& s) {
    char quote = '\'';
    if (s.find('\'') != std::string::npos && s.find('"') == std::string::npos) quote = '"';
    std::string out;
    out += quote;
    for (char c : s) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (c == quote || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else if (c == '\r') out += "\\r";
        else if (uc < 0x20 || uc == 0x7f) { char b[8]; std::snprintf(b, sizeof(b), "\\x%02x", uc); out += b; }  // 0x7f = DEL: a lone control byte that a terminal would eat, breaking the read-back-unambiguously contract
        else out += c;
    }
    out += quote;
    return out;
}

// Stringify a value as it should appear as an *element* of a container: a String is repr'd (quoted),
// everything else uses its normal str() — and nested containers recurse through this same helper for
// their own elements, so quoting is consistent at every depth. This is the str()-vs-repr distinction:
// a bare `print(s)` shows the raw text, but `print([s])` shows the quoted form.
inline std::string stringifyChild(StringifyCtx& ctx, Handle h) {
    const Object& o = ctx.arena.deref(h);
    if (o.kind() == ValueKind::String) return reprString(static_cast<const StrVal&>(o).value());
    return o.str(ctx);
}

}  // namespace kirito

#endif
