#ifndef KIRITO_BYTES_HPP
#define KIRITO_BYTES_HPP

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "builtins.hpp"   // utf8Encode / utf8DecodeAt / utf8Starts, StrVal
#include "native.hpp"     // NativeClass, makeMethod, value.hpp

namespace kirito {

// The native-binding idiom below re-uses `vm`/`self` as bound-method lambda parameters that
// intentionally shadow the enclosing getAttr `vm`/`self` (same VM, by design). Silence -Wshadow for
// these mechanical bindings; it stays active in the evaluator/parser/lexer core.
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wshadow"
#endif

// An immutable sequence of raw bytes (0–255). Distinct from String, which is
// Unicode (code-point) text: a String holds UTF-8 and indexes by code point, so it cannot losslessly
// hold or address arbitrary binary. Bytes indexes by byte (b[i] -> Integer 0–255), so it is the right
// type for binary I/O — network downloads, compressed data, file contents. Convert with
// `s.encode([enc])` (String -> Bytes) and `b.decode([enc])` (Bytes -> String); `latin-1` maps each
// byte to one code point and back losslessly.
class BytesVal : public NativeClass<BytesVal> {
public:
    static constexpr const char* kTypeName = "Bytes";
    std::string data;

    BytesVal() = default;
    explicit BytesVal(std::string d) : data(std::move(d)) {}

    bool truthy() const override { return !data.empty(); }

    // repr: b'...' with printable ASCII verbatim and \xHH / \n \t \r \\ \' for the rest.
    std::string str(StringifyCtx&) const override {
        std::string out = "b'";
        for (unsigned char c : data) {
            switch (c) {
                case '\n': { out += "\\n"; } break;
                case '\t': { out += "\\t"; } break;
                case '\r': { out += "\\r"; } break;
                case '\\': { out += "\\\\"; } break;
                case '\'': { out += "\\'"; } break;
                default: {
                    if (c >= 0x20 && c < 0x7f) out += static_cast<char>(c);
                    else {
                        static const char* hex = "0123456789abcdef";
                        out += "\\x";
                        out += hex[c >> 4];
                        out += hex[c & 0xf];
                    }
                } break;
            }
        }
        out += "'";
        return out;
    }

    bool equals(const ObjectArena&, const Object& other) const override {
        const auto* b = dynamic_cast<const BytesVal*>(&other);
        return b && b->data == data;
    }
    bool hashable() const override { return true; }
    std::size_t hash() const override { return std::hash<std::string>{}(data); }

    std::optional<int64_t> length(KiritoVM&) override { return static_cast<int64_t>(data.size()); }

    // b[i] -> the byte at i as an Integer (negative indices count from the end).
    Handle getItem(KiritoVM& vm, std::span<const Handle> keys) override {
        const Object& k = vm.arena().deref(singleKey(*this, keys));
        if (k.kind() != ValueKind::Integer)
            throw KiritoError("Bytes index must be Integer, not '" + k.typeName() + "'");
        int64_t i = static_cast<const IntVal&>(k).value();
        int64_t n = static_cast<int64_t>(data.size());
        if (i < 0) i += n;
        if (i < 0 || i >= n) throw KiritoError("Bytes index out of range");
        return vm.makeInt(static_cast<unsigned char>(data[static_cast<std::size_t>(i)]));
    }

    // b[a:b:c] -> a Bytes slice.
    Handle slice(KiritoVM& vm, Handle sH, Handle eH, Handle stH) override {
        auto idx = sliceIndices(vm, static_cast<int64_t>(data.size()), sH, eH, stH);
        std::string out;
        out.reserve(idx.size());
        for (int64_t i : idx) out += data[static_cast<std::size_t>(i)];
        return vm.alloc(std::make_unique<BytesVal>(std::move(out)));
    }

    std::optional<std::vector<Handle>> iterate(KiritoVM& vm) override {
        std::vector<Handle> out;
        out.reserve(data.size());
        for (unsigned char c : data) out.push_back(vm.makeInt(c));
        return out;
    }

    // `x in b`: an Integer byte value, or a Bytes subsequence.
    bool contains(KiritoVM& vm, Handle value) override {
        const Object& o = vm.arena().deref(value);
        if (o.kind() == ValueKind::Integer) {
            int64_t v = static_cast<const IntVal&>(o).value();
            if (v < 0 || v > 255) return false;
            return data.find(static_cast<char>(static_cast<unsigned char>(v))) != std::string::npos;
        }
        if (const auto* b = dynamic_cast<const BytesVal*>(&o))
            return data.find(b->data) != std::string::npos;
        throw KiritoError("'in <Bytes>' requires an Integer byte or a Bytes, not '" + o.typeName() + "'");
    }

    Handle binary(KiritoVM& vm, BinOp op, Handle self, Handle rhs) override {
        const Object& b = vm.arena().deref(rhs);
        if (op == BinOp::Add) {
            const auto* o = dynamic_cast<const BytesVal*>(&b);
            if (!o) throw KiritoError("can only concatenate Bytes to Bytes, not '" + b.typeName() + "'");
            return vm.alloc(std::make_unique<BytesVal>(data + o->data));
        }
        if (op == BinOp::Mul) {
            if (b.kind() != ValueKind::Integer) throw KiritoError("can only repeat Bytes by an Integer");
            int64_t nrep = static_cast<const IntVal&>(b).value();
            if (nrep <= 0 || data.empty()) return vm.alloc(std::make_unique<BytesVal>());
            if (static_cast<uint64_t>(nrep) > kMaxRepeat / data.size())  // shared cap (common.hpp)
                throw KiritoError("repeated Bytes too large");
            std::string out;
            out.reserve(data.size() * static_cast<std::size_t>(nrep));
            for (int64_t i = 0; i < nrep; ++i) out += data;
            return vm.alloc(std::make_unique<BytesVal>(std::move(out)));
        }
        if (const auto* o = dynamic_cast<const BytesVal*>(&b)) {  // lexicographic ordering
            switch (op) {
                case BinOp::Lt: { return vm.makeBool(data < o->data); } break;
                case BinOp::Le: { return vm.makeBool(data <= o->data); } break;
                case BinOp::Gt: { return vm.makeBool(data > o->data); } break;
                case BinOp::Ge: { return vm.makeBool(data >= o->data); } break;
                default: { break; } break;
            }
        }
        return Object::binary(vm, op, self, rhs);
    }

    std::vector<std::string> inspectMembers() const override {
        return {"decode(encoding) -> String", "hex() -> String", "apply(fn) -> Bytes"};
    }

    Handle getAttr(KiritoVM& vm, Handle self, std::string_view name) override;

};

namespace bytesutil {

// Normalise an encoding name: lowercase, '-'/'_' removed (so "UTF-8", "utf_8", "utf8" all match).
inline std::string normEnc(const std::string& e) {
    std::string out;
    for (char c : e) {
        if (c == '-' || c == '_') continue;
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// Encode a String's code points to bytes under `enc`. utf-8 keeps the String's bytes as-is; latin-1
// maps each code point (must be 0–255) to one byte; ascii requires code points < 128.
inline std::string encode(const std::string& s, const std::string& enc) {
    std::string e = normEnc(enc);
    if (e == "utf8") return s;  // a Kirito String already stores UTF-8 bytes
    if (e == "latin1" || e == "iso88591" || e == "ascii") {
        unsigned cap = (e == "ascii") ? 0x80u : 0x100u;
        std::string out;
        for (std::size_t st : utf8Starts(s)) {
            unsigned cp = utf8DecodeAt(s, st);
            if (cp >= cap) {
                char hex[16];
                std::snprintf(hex, sizeof(hex), "%04X", cp);  // U+ is conventionally hexadecimal
                throw KiritoError("'" + enc + "' codec can't encode code point U+" + hex);
            }
            out += static_cast<char>(static_cast<unsigned char>(cp));
        }
        return out;
    }
    throw KiritoError("unknown encoding: '" + enc + "'");
}

// Well-formed UTF-8 check: correct lead/continuation bytes, no overlong encodings, no surrogates,
// in range. Used so decode("utf-8") can't silently fabricate a String whose bytes aren't valid
// UTF-8 (which would then misbehave under code-point indexing/len) — this throws instead.
inline bool validUtf8(const std::string& s) {
    std::size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t len; unsigned cp;
        if (c < 0x80) { ++i; continue; }
        else if ((c >> 5) == 0x6) { len = 2; cp = c & 0x1Fu; }
        else if ((c >> 4) == 0xE) { len = 3; cp = c & 0x0Fu; }
        else if ((c >> 3) == 0x1E) { len = 4; cp = c & 0x07u; }
        else return false;                                  // stray continuation byte or 0xF8+ lead
        if (i + len > n) return false;                      // truncated multibyte sequence
        for (std::size_t k = 1; k < len; ++k) {
            unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc >> 6) != 0x2) return false;             // not a 10xxxxxx continuation byte
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000))
            return false;                                   // overlong
        if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) return false;  // out of range / surrogate
        i += len;
    }
    return true;
}

// Format a byte as a 2-digit hex literal (the decode error labels it "0x...").
inline std::string hexByte(unsigned char c) {
    static const char* d = "0123456789abcdef";
    return std::string("0x") + d[(c >> 4) & 0xF] + d[c & 0xF];
}

// Decode bytes to a String (UTF-8 text) under `enc`. utf-8 keeps the bytes (they are already the
// String's storage) once validated; latin-1/ascii map each byte to a code point, then UTF-8-encode it.
inline std::string decode(const std::string& data, const std::string& enc) {
    std::string e = normEnc(enc);
    if (e == "utf8") {
        if (!validUtf8(data)) throw KiritoError("'utf-8' codec can't decode: invalid UTF-8 byte sequence");
        return data;
    }
    if (e == "latin1" || e == "iso88591" || e == "ascii") {
        unsigned cap = (e == "ascii") ? 0x80u : 0x100u;
        std::string out;
        for (unsigned char c : data) {
            if (c >= cap) throw KiritoError("'" + enc + "' codec can't decode byte " + hexByte(c));
            utf8Encode(c, out);
        }
        return out;
    }
    throw KiritoError("unknown encoding: '" + enc + "'");
}

inline std::string toHex(const std::string& data) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (unsigned char c : data) { out += hex[c >> 4]; out += hex[c & 0xf]; }
    return out;
}

inline std::string fromHex(const std::string& s) {
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    std::size_t i = 0;
    while (i < s.size()) {
        if (std::isspace(static_cast<unsigned char>(s[i]))) { ++i; continue; }
        if (i + 1 >= s.size()) throw KiritoError("fromhex: odd-length hex string");
        int hi = nib(s[i]), lo = nib(s[i + 1]);
        if (hi < 0 || lo < 0) throw KiritoError("fromhex: non-hex digit");
        out += static_cast<char>(static_cast<unsigned char>((hi << 4) | lo));
        i += 2;
    }
    return out;
}

}  // namespace bytesutil

inline Handle BytesVal::getAttr(KiritoVM& vm, Handle self, std::string_view name) {
    auto self_b = [](KiritoVM& vm, Handle h) -> BytesVal& { return static_cast<BytesVal&>(vm.arena().deref(h)); };
    if (name == "decode")
        return makeMethod(vm, "decode", {"encoding"}, [self, self_b](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            std::string enc = "utf-8";
            if (!a.empty() && vm.arena().deref(a[0]).kind() != ValueKind::None) enc = Value(vm, a[0]).asStringRef("decode encoding");
            return vm.makeString(bytesutil::decode(self_b(vm, self).data, enc));
        }, std::vector<Handle>{self});
    if (name == "hex")
        return makeMethod(vm, "hex", {}, [self, self_b](KiritoVM& vm, std::span<const Handle>) -> Handle {
            return vm.makeString(bytesutil::toHex(self_b(vm, self).data));
        }, std::vector<Handle>{self});
    // apply(fn) — a new Bytes with `fn` applied to each byte (fn takes/returns an Integer 0..255).
    if (name == "apply")
        return makeMethod(vm, "apply", {"fn"}, [self, self_b](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            if (a.empty()) throw KiritoError("apply expects a function");
            Handle fn = a[0];
            std::string src = self_b(vm, self).data;
            std::string out;
            out.reserve(src.size());
            for (unsigned char c : src) {
                std::array<Handle, 1> args{vm.makeInt(c)};
                int64_t r = Value(vm, vm.arena().deref(fn).call(vm, args)).asInt("Bytes apply result");
                if (r < 0 || r > 255) throw KiritoError("Bytes apply: result must be a byte (0..255)");
                out += static_cast<char>(static_cast<unsigned char>(r));
            }
            return vm.alloc(std::make_unique<BytesVal>(std::move(out)));
        }, std::vector<Handle>{self});
    // serialization: round-trip the raw bytes as a latin-1 String (lossless byte<->code-point).
    if (name == "_getstate_")
        return makeMethod(vm, "_getstate_", {}, [self, self_b](KiritoVM& vm, std::span<const Handle>) -> Handle {
            return vm.makeString(bytesutil::decode(self_b(vm, self).data, "latin-1"));
        }, std::vector<Handle>{self});
    if (name == "_setstate_")
        return makeMethod(vm, "_setstate_", {"state"}, [self, self_b](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            if (a.empty()) throw KiritoError("_setstate_ expects the serialized state");
            self_b(vm, self).data = bytesutil::encode(Value(vm, a[0]).asStringRef("Bytes state"), "latin-1");
            return vm.none();
        }, std::vector<Handle>{self});
    return Object::getAttr(vm, self, name);
}

// Build a Bytes from a Kirito value: a List of Integers (0–255), an Integer n (n zero bytes), a String
// (encoded with `enc`, default utf-8), or another Bytes (copied).
inline Handle makeBytes(KiritoVM& vm, Handle x, const std::string& enc = "utf-8") {
    Object& o = vm.arena().deref(x);
    if (const auto* b = dynamic_cast<const BytesVal*>(&o))
        return vm.alloc(std::make_unique<BytesVal>(b->data));
    if (o.kind() == ValueKind::String)
        return vm.alloc(std::make_unique<BytesVal>(bytesutil::encode(static_cast<const StrVal&>(o).value(), enc)));
    if (o.kind() == ValueKind::Integer) {
        int64_t n = static_cast<const IntVal&>(o).value();
        if (n < 0) throw KiritoError("negative count");
        if (static_cast<uint64_t>(n) > 256ull * 1024 * 1024) throw KiritoError("Bytes too large");
        return vm.alloc(std::make_unique<BytesVal>(std::string(static_cast<std::size_t>(n), '\0')));
    }
    auto it = o.iterate(vm);
    if (!it) throw KiritoError("Bytes() expects a List of Integers, an Integer, a String, or Bytes");
    std::string out;
    out.reserve(it->size());
    for (Handle h : *it) {
        const Object& e = vm.arena().deref(h);
        if (e.kind() != ValueKind::Integer) throw KiritoError("Bytes() list elements must be Integers");
        int64_t v = static_cast<const IntVal&>(e).value();
        if (v < 0 || v > 255) throw KiritoError("Bytes() element out of range (0..255)");
        out += static_cast<char>(static_cast<unsigned char>(v));
    }
    return vm.alloc(std::make_unique<BytesVal>(std::move(out)));
}

// The String-or-Bytes "byte-transparent" duality, shared by the hash/zlib/gzip modules so each
// needn't re-roll it: a raw byte view of an argument, and a result wrapped to match the input type.
// argStringOrBytes returns a reference valid while the argument object lives (the caller keeps it
// rooted). makeStringOrBytes maps Bytes->Bytes / String->String so codecs stay byte-exact on binary.
inline const std::string& argStringOrBytes(KiritoVM& vm, Handle h, const char* who) {
    Object& o = vm.arena().deref(h);
    if (o.kind() == ValueKind::String) return static_cast<StrVal&>(o).value();
    if (auto* b = dynamic_cast<BytesVal*>(&o)) return b->data;
    throw KiritoError(std::string(who) + " expects a String or Bytes");
}
inline Handle makeStringOrBytes(KiritoVM& vm, Handle templateInput, std::string out) {
    if (dynamic_cast<BytesVal*>(&vm.arena().deref(templateInput)))
        return vm.alloc(std::make_unique<BytesVal>(std::move(out)));
    return vm.makeString(std::move(out));
}

#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

// Out-of-line definitions for the Bytes facade (needs BytesVal to be complete). Fresh-alloc paths
// go through Value::adopt() so the new BytesVal is GC-pinned.
inline Bytes::Bytes(KiritoVM& vm, std::string_view raw) {
    adopt(vm, vm.alloc(std::make_unique<BytesVal>(std::string(raw))));
}
inline Bytes::Bytes(KiritoVM& vm, std::string raw) {
    adopt(vm, vm.alloc(std::make_unique<BytesVal>(std::move(raw))));
}
inline const std::string& Bytes::data() const {
    return static_cast<const BytesVal&>(ref()).data;
}

}  // namespace kirito

#endif
