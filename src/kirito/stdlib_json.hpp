#ifndef KIRITO_STDLIB_JSON_HPP
#define KIRITO_STDLIB_JSON_HPP

#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "fum/unordered_set.hpp"
#include "builtins.hpp"
#include "collections.hpp"
#include "native.hpp"

namespace kirito {

// The native-binding idiom below re-uses `vm`/`self` as bound-method lambda parameters that
// intentionally shadow the enclosing getAttr/setup `vm`/`self` (same VM, by design). Silence
// -Wshadow for these mechanical bindings; it stays active in the evaluator/parser/lexer core.
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wshadow"
#endif

// JSON support. parse() turns JSON text into native Kirito values — objects become Dicts (so a
// parsed JSON object *is* a Dict), arrays become Lists, and primitives the obvious scalars.
// stringify() does the reverse.
namespace json {

class Parser {
public:
    Parser(KiritoVM& vm, const std::string& src, RootScope& roots) : vm_(vm), s_(src), roots_(roots) {}

    Handle parse() {
        skipWs();
        Handle v = value();
        skipWs();
        if (pos_ != s_.size()) fail("trailing characters after JSON value");
        return v;
    }

private:
    [[noreturn]] void fail(const std::string& msg) { throw KiritoError("JSON parse error: " + msg); }
    void skipWs() { while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_; }
    char peek() { return pos_ < s_.size() ? s_[pos_] : '\0'; }
    bool match(const char* lit) {
        std::size_t n = std::char_traits<char>::length(lit);
        if (s_.compare(pos_, n, lit) == 0) { pos_ += n; return true; }
        return false;
    }

    // Recursion-depth guard: deeply-nested JSON ([[[[...]]]]) must throw, not overflow the C++ stack.
    struct DepthGuard {
        int& d;
        explicit DepthGuard(int& depth) : d(depth) {
            if (++d > 1000) throw KiritoError("JSON parse error: nesting too deep");
        }
        ~DepthGuard() { --d; }
    };

    Handle value() {
        skipWs();
        char c = peek();
        if (c == '{') return object();
        if (c == '[') return array();
        if (c == '"') return roots_.add(vm_.makeString(string()));
        if (c == 't') { if (match("true")) return vm_.makeBool(true); fail("invalid literal"); }
        if (c == 'f') { if (match("false")) return vm_.makeBool(false); fail("invalid literal"); }
        if (c == 'n') { if (match("null")) return vm_.none(); fail("invalid literal"); }
        // Non-finite floats (and what dumps emits): NaN / Infinity / -Infinity.
        if (c == 'N') { if (match("NaN")) return roots_.add(vm_.makeFloat(std::nan(""))); fail("invalid literal"); }
        if (c == 'I') { if (match("Infinity")) return roots_.add(vm_.makeFloat(HUGE_VAL)); fail("invalid literal"); }
        if (c == '-' && s_.compare(pos_, 9, "-Infinity") == 0) { pos_ += 9; return roots_.add(vm_.makeFloat(-HUGE_VAL)); }
        if (c == '-' || (c >= '0' && c <= '9')) return number();
        fail("unexpected character");
    }

    Handle object() {
        DepthGuard g(depth_);
        ++pos_;  // {
        auto dict = std::make_unique<DictVal>();
        Handle h = roots_.add(vm_.alloc(std::move(dict)));
        auto& d = static_cast<DictVal&>(vm_.arena().deref(h));
        skipWs();
        if (peek() == '}') { ++pos_; return h; }
        while (true) {
            skipWs();
            if (peek() != '"') fail("expected string key");
            Handle key = roots_.add(vm_.makeString(string()));
            skipWs();
            if (peek() != ':') fail("expected ':'");
            ++pos_;
            Handle val = value();
            d.set(vm_.arena(), key, val);
            skipWs();
            if (peek() == ',') { ++pos_; continue; }
            if (peek() == '}') { ++pos_; break; }
            fail("expected ',' or '}'");
        }
        return h;
    }

    Handle array() {
        DepthGuard g(depth_);
        ++pos_;  // [
        auto list = std::make_unique<ListVal>();
        Handle h = roots_.add(vm_.alloc(std::move(list)));
        auto& l = static_cast<ListVal&>(vm_.arena().deref(h));
        skipWs();
        if (peek() == ']') { ++pos_; return h; }
        while (true) {
            // Barriered append, like object()'s d.set() below. The raw elems.push_back this replaces
            // skipped the write barrier: `l` is rooted across the recursive value() call, so a
            // collection can PROMOTE it, and an old list silently gaining a young element is never
            // enrolled in the remembered set — the next minor then frees a nested array that is
            // still perfectly reachable. (v1.15 A19-2: json.loads("[1, [2]]") lost its inner list.)
            l.append(vm_.arena(), value());
            skipWs();
            if (peek() == ',') { ++pos_; continue; }
            if (peek() == ']') { ++pos_; break; }
            fail("expected ',' or ']'");
        }
        return h;
    }

    std::string string() {
        ++pos_;  // opening quote
        std::string out;
        while (pos_ < s_.size() && s_[pos_] != '"') {
            char c = s_[pos_++];
            if (c == '\\') {
                if (pos_ >= s_.size()) fail("bad escape");
                char e = s_[pos_++];
                switch (e) {
                    case '"': { out += '"'; } break;
                    case '\\': { out += '\\'; } break;
                    case '/': { out += '/'; } break;
                    case 'n': { out += '\n'; } break;
                    case 't': { out += '\t'; } break;
                    case 'r': { out += '\r'; } break;
                    case 'b': { out += '\b'; } break;
                    case 'f': { out += '\f'; } break;
                    case 'u': {
                        unsigned cp = readHex4();
                        // Combine a UTF-16 surrogate pair (😀) into one astral code point.
                        if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 1 < s_.size() &&
                            s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            unsigned lo = readHex4();
                            if (lo >= 0xDC00 && lo <= 0xDFFF)
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            else fail("invalid low surrogate in \\u escape");   // a malformed \u-pair is a
                                              // stricter error than a truly-lone surrogate (which U+FFFD-
                                              // substitutes below) — deliberate, pinned by json/serde tests.
                        }
                        // An UNPAIRED surrogate (a lone \uD800 high, or a lone low) is not a valid code
                        // point and would encode to invalid UTF-8; substitute U+FFFD (like browsers /
                        // WHATWG) so the decoded String is always well-formed UTF-8.
                        if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
                        utf8Encode(cp, out);  // shared encoder (builtins.hpp), 4-byte-capable
                    } break;
                    default: { fail("bad escape"); } break;
                }
            } else {
                out += c;
            }
        }
        if (pos_ >= s_.size()) fail("unterminated string");
        ++pos_;  // closing quote
        return out;
    }

    // Read exactly four hex digits at pos_ (advancing past them) into a code-point value.
    unsigned readHex4() {
        if (pos_ + 4 > s_.size()) fail("bad \\u escape");
        unsigned cp = 0;
        for (int k = 0; k < 4; ++k) {
            char d = s_[pos_ + k];
            int v = hexDigitValue(d);
            if (v < 0) fail("invalid \\u escape (expected hex digits)");
            cp = cp * 16 + static_cast<unsigned>(v);
        }
        pos_ += 4;
        return cp;
    }

    Handle number() {
        std::size_t start = pos_;
        bool isFloat = false;
        if (peek() == '-') ++pos_;
        // The number scan is intentionally LENIENT vs strict RFC 8259 (a leading zero, a trailing
        // dot, an empty exponent all parse) — a deliberate, regression-guarded design choice pinned
        // by deep_serialization.ki. Do NOT tighten this to the strict grammar without updating that
        // test and the design intent.
        while (pos_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[pos_])))) ++pos_;
        if (peek() == '.') { isFloat = true; ++pos_; while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_; }
        if (peek() == 'e' || peek() == 'E') { isFloat = true; ++pos_; if (peek() == '+' || peek() == '-') ++pos_; while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_; }
        std::string tok = s_.substr(start, pos_ - start);
        if (tok.empty() || tok == "-") fail("invalid number");
        try {
            if (isFloat) return roots_.add(vm_.makeFloat(parseDouble(tok)));
            return vm_.makeInt(static_cast<int64_t>(std::stoll(tok)));
        } catch (const std::out_of_range&) {
            // An integer too large for int64 -> widen to Float (mirroring dynamic languages). If
            // even the double overflows, represent it as infinity rather than throwing.
            try { return roots_.add(vm_.makeFloat(parseDouble(tok))); }
            catch (const std::out_of_range&) {
                return roots_.add(vm_.makeFloat(tok[0] == '-' ? -HUGE_VAL : HUGE_VAL));
            }
        } catch (const std::invalid_argument&) {
            fail("invalid number");
        }
    }

    KiritoVM& vm_;
    const std::string& s_;
    RootScope& roots_;
    std::size_t pos_ = 0;
    int depth_ = 0;
};

inline void escapeString(const std::string& s, std::string& out) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"': { out += "\\\""; } break;
            case '\\': { out += "\\\\"; } break;
            case '\n': { out += "\\n"; } break;
            case '\t': { out += "\\t"; } break;
            case '\r': { out += "\\r"; } break;
            case '\b': { out += "\\b"; } break;
            case '\f': { out += "\\f"; } break;
            default: {
                // JSON requires control characters U+0000..U+001F to be \u-escaped; emitting them
                // raw produces invalid JSON (a NUL/0x01/... in the output). Other bytes (incl. UTF-8
                // continuation bytes) pass through unchanged.
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[(static_cast<unsigned char>(c) >> 4) & 0xF];
                    out += hex[static_cast<unsigned char>(c) & 0xF];
                } else {
                    out += c;
                }
            } break;
        }
    }
    out += '"';
}

// JSON-canonical text for a dict key (object keys must be strings). Bool/None keys use JSON's
// `true`/`false`/`null` (not Kirito's `"True"`/`"None"`, which would not round-trip); other non-string
// scalars use their str() text. (Non-string keys already sit outside strict JSON, but at least emit
// the canonical tokens so a Bool/None key is valid JSON.)
inline void writeJsonKey(KiritoVM& vm, Handle k, std::string& out) {
    const Object& ko = vm.arena().deref(k);
    if (ko.kind() == ValueKind::String) { escapeString(static_cast<const StrVal&>(ko).value(), out); return; }
    if (ko.kind() == ValueKind::Bool) { escapeString(static_cast<const BoolVal&>(ko).value() ? "true" : "false", out); return; }
    if (ko.kind() == ValueKind::None) { escapeString("null", out); return; }
    escapeString(vm.stringify(k), out);
}

// JSON float text. Finite values use the SHORTEST round-tripping form (so dumps->loads recovers the
// exact double — important now that Float == is exact; the lossy display %.15g would not survive the
// cycle). Non-finite values use the spelling (NaN / Infinity / -Infinity) which our
// parser accepts back (plain lowercase nan/inf would be rejected by JSON readers, including ours).
inline std::string jsonFloat(double d) {
    if (std::isnan(d)) return "NaN";
    if (std::isinf(d)) return d < 0 ? "-Infinity" : "Infinity";
    return floatToRoundtrip(d);
}

inline void write(KiritoVM& vm, Handle h, std::string& out, fum::unordered_set<const Object*>& active) {
    const Object& o = vm.arena().deref(h);
    switch (o.kind()) {
        case ValueKind::None: { out += "null"; return; } break;
        case ValueKind::Bool: { out += static_cast<const BoolVal&>(o).value() ? "true" : "false"; return; } break;
        case ValueKind::Integer: { out += std::to_string(static_cast<const IntVal&>(o).value()); return; } break;
        case ValueKind::Float: { out += jsonFloat(static_cast<const FloatVal&>(o).value()); return; } break;
        case ValueKind::String: { escapeString(static_cast<const StrVal&>(o).value(), out); return; } break;
        default: { break; } break;
    }
    if (active.count(&o)) throw KiritoError("cannot serialize a cyclic structure to JSON");
    if (active.size() > 1000) throw KiritoError("structure too deeply nested to serialize to JSON");
    active.insert(&o);
    if (o.kind() == ValueKind::List || o.kind() == ValueKind::Array) {
        out += '[';
        const auto& l = static_cast<const ListVal&>(o);
        for (std::size_t i = 0; i < l.elems.size(); ++i) { if (i) out += ", "; write(vm, l.elems[i], out, active); }
        out += ']';
    } else if (o.kind() == ValueKind::Dict) {
        out += '{';
        const auto& d = static_cast<const DictVal&>(o);
        bool first = true;
        for (Handle k : d.keys()) {
            if (!first) out += ", ";
            first = false;
            writeJsonKey(vm, k, out);
            out += ": ";
            write(vm, *d.find(vm.arena(), k), out, active);
        }
        out += '}';
    } else {
        active.erase(&o);
        throw KiritoError("cannot serialize '" + o.typeName() + "' to JSON");
    }
    active.erase(&o);
}

// Pretty-printing variant: `indent` spaces per nesting level.
inline void writeIndented(KiritoVM& vm, Handle h, std::string& out,
                          fum::unordered_set<const Object*>& active, int indent, int depth) {
    const Object& o = vm.arena().deref(h);
    switch (o.kind()) {
        case ValueKind::None: { out += "null"; return; } break;
        case ValueKind::Bool: { out += static_cast<const BoolVal&>(o).value() ? "true" : "false"; return; } break;
        case ValueKind::Integer: { out += std::to_string(static_cast<const IntVal&>(o).value()); return; } break;
        case ValueKind::Float: { out += jsonFloat(static_cast<const FloatVal&>(o).value()); return; } break;
        case ValueKind::String: { escapeString(static_cast<const StrVal&>(o).value(), out); return; } break;
        default: { break; } break;
    }
    if (active.count(&o)) throw KiritoError("cannot serialize a cyclic structure to JSON");
    if (active.size() > 1000) throw KiritoError("structure too deeply nested to serialize to JSON");
    active.insert(&o);
    std::string pad((depth + 1) * indent, ' '), padEnd(depth * indent, ' ');
    if (o.kind() == ValueKind::List || o.kind() == ValueKind::Array) {
        const auto& l = static_cast<const ListVal&>(o);
        if (l.elems.empty()) { out += "[]"; active.erase(&o); return; }
        out += "[\n";
        for (std::size_t i = 0; i < l.elems.size(); ++i) {
            if (i) out += ",\n";
            out += pad;
            writeIndented(vm, l.elems[i], out, active, indent, depth + 1);
        }
        out += "\n" + padEnd + "]";
    } else if (o.kind() == ValueKind::Dict) {
        const auto& d = static_cast<const DictVal&>(o);
        auto ks = d.keys();
        if (ks.empty()) { out += "{}"; active.erase(&o); return; }
        out += "{\n";
        bool first = true;
        for (Handle k : ks) {
            if (!first) out += ",\n";
            first = false;
            out += pad;
            writeJsonKey(vm, k, out);
            out += ": ";
            writeIndented(vm, *d.find(vm.arena(), k), out, active, indent, depth + 1);
        }
        out += "\n" + padEnd + "}";
    } else {
        active.erase(&o);
        throw KiritoError("cannot serialize '" + o.typeName() + "' to JSON");
    }
    active.erase(&o);
}

}  // namespace json

class JsonModule : public NativeModule {
public:
    std::string name() const override { return "json"; }
    void setup(ModuleBuilder& m) override {
        KiritoVM& vm = m.vm();
        m.fn("parse", {{"text", "String"}}, "", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            RootScope roots(vm);
            json::Parser p(vm, Args(vm, a, "json.parse")[0].asStringRef("json.parse"), roots);
            return p.parse();
        });
        m.fn("stringify", {{"value"}, {"indent", "Integer", vm.makeInt(0)}}, "String", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            // stringify(value[, indent]): compact by default; pretty-printed with `indent` spaces.
            Args args(vm, a, "json.stringify");
            std::string out;
            fum::unordered_set<const Object*> active;
            // Validate indent BEFORE narrowing to int: a huge value would overflow the
            // `(depth+1)*indent` pad-width arithmetic (signed UB) and, short of that, OOM with a raw
            // std::string allocator error. Cap it to a sane maximum with a clear message.
            int64_t indent64 = args.size() > 1 ? args[1].asInt("json.stringify indent") : 0;
            if (indent64 > 100) throw KiritoError("json.stringify: indent too large (maximum 100)");
            int indent = indent64 > 0 ? static_cast<int>(indent64) : 0;
            if (indent > 0) json::writeIndented(vm, args[0], out, active, indent, 0);
            else json::write(vm, args[0], out, active);
            return Value(vm, std::move(out));
        });
        // Convenience aliases.
        m.alias("loads", "parse");
        m.alias("dumps", "stringify");
    }
};

}  // namespace kirito

#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
#endif
