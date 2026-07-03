#ifndef KIRITO_STDLIB_DUMP_HPP
#define KIRITO_STDLIB_DUMP_HPP

#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "bytes.hpp"
#include "native.hpp"
#include "stdlib_serde.hpp"

namespace kirito {

// The native-binding idiom below re-uses `vm`/`self` as bound-method lambda parameters that
// intentionally shadow the enclosing getAttr/setup `vm`/`self` (same VM, by design). Silence
// -Wshadow for these mechanical bindings; it stays active in the evaluator/parser/lexer core.
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wshadow"
#endif

// The `dump` module: compact BINARY serialization that preserves shared references and cycles (like
// a portable `pickle`). `dumps(value)` returns the blob as `Bytes`; `loads(bytes)` reconstructs the
// graph. The graph walk and reconstruction are shared with the text `serialize` module via
// serde::flatten / serde::rebuild (stdlib_serde.hpp); this file is only the binary codec.
//
// Wire format (little-endian): magic "KDMP" (4 bytes), version u8 = 1, u32 objectCount, then
// objectCount records each `u8 tag + payload` (tags match serde::Tag: 0 None, 1 Bool u8, 2 Integer
// i64, 3 Float f64-bits, 4 String u32 len + bytes, 5 List u32 count + ids, 6 Dict u32 count + (k,v)
// id pairs, 7 Set u32 count + ids, 8 Object [user class] u32 nameLen + name + u32 count + (k,v) id
// pairs, 9 Stateful [_getstate_-based] u32 nameLen + name + single state id), then u32 rootId.
namespace dumpfmt {

inline void putU8(std::string& b, uint8_t v) { b.push_back(static_cast<char>(v)); }
inline void putU32(std::string& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
inline void putI64(std::string& b, int64_t v) {
    uint64_t u = static_cast<uint64_t>(v);
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<char>((u >> (8 * i)) & 0xFF));
}
inline void putF64(std::string& b, double d) {
    uint64_t u;
    std::memcpy(&u, &d, sizeof(u));
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<char>((u >> (8 * i)) & 0xFF));
}

class Reader {
public:
    explicit Reader(const std::string& s) : s_(s) {}
    uint8_t u8() { need(1); return static_cast<uint8_t>(s_[pos_++]); }
    uint32_t u32() {
        need(4);
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<uint8_t>(s_[pos_++])) << (8 * i);
        return v;
    }
    int64_t i64() {
        need(8);
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(static_cast<uint8_t>(s_[pos_++])) << (8 * i);
        return static_cast<int64_t>(v);
    }
    double f64() {
        uint64_t u = static_cast<uint64_t>(i64());
        double d;
        std::memcpy(&d, &u, sizeof(d));
        return d;
    }
    std::string bytes(uint32_t n) {
        need(n);
        std::string out = s_.substr(pos_, n);
        pos_ += n;
        return out;
    }
    void expect(const char* magic, std::size_t n) {
        need(n);
        if (s_.compare(pos_, n, magic, n) != 0) throw KiritoError("bad dump header");
        pos_ += n;
    }

private:
    void need(std::size_t n) { if (pos_ + n > s_.size()) throw KiritoError("truncated dump data"); }
    const std::string& s_;
    std::size_t pos_ = 0;
};

inline std::string encode(const std::vector<serde::Node>& nodes, uint32_t rootId) {
    std::string b;
    b.append("KDMP");
    putU8(b, 1);
    putU32(b, static_cast<uint32_t>(nodes.size()));
    for (const serde::Node& n : nodes) {
        putU8(b, static_cast<uint8_t>(n.tag));
        switch (n.tag) {
            case serde::Tag::None: { break; } break;
            case serde::Tag::Bool: { putU8(b, n.b ? 1 : 0); } break;
            case serde::Tag::Integer: { putI64(b, n.i); } break;
            case serde::Tag::Float: { putF64(b, n.f); } break;
            case serde::Tag::String: { putU32(b, static_cast<uint32_t>(n.s.size())); b.append(n.s); } break;
            case serde::Tag::List:
            case serde::Tag::Set: {
                putU32(b, static_cast<uint32_t>(n.links.size()));
                for (uint32_t id : n.links) putU32(b, id);
            } break;
            case serde::Tag::Dict: {
                putU32(b, static_cast<uint32_t>(n.links.size() / 2));
                for (uint32_t id : n.links) putU32(b, id);
            } break;
            case serde::Tag::Object: {  // class name + key/val id pairs
                putU32(b, static_cast<uint32_t>(n.s.size())); b.append(n.s);
                putU32(b, static_cast<uint32_t>(n.links.size() / 2));
                for (uint32_t id : n.links) putU32(b, id);
            } break;
            case serde::Tag::Stateful: {  // class name + single state id
                putU32(b, static_cast<uint32_t>(n.s.size())); b.append(n.s);
                putU32(b, n.links.empty() ? 0u : n.links[0]);
            } break;
        }
    }
    putU32(b, rootId);
    return b;
}

inline std::pair<std::vector<serde::Node>, uint32_t> decode(const std::string& data) {
    Reader r(data);
    r.expect("KDMP", 4);
    if (r.u8() != 1) throw KiritoError("unsupported dump version");
    uint32_t n = r.u32();
    // Each record is at least one tag byte, so a count exceeding the input is corrupt — reject before
    // allocating to avoid a huge/bad allocation.
    if (n > data.size()) throw KiritoError("corrupt dump: object count exceeds data size");
    std::vector<serde::Node> nodes(n);
    for (uint32_t i = 0; i < n; ++i) {
        serde::Node& nd = nodes[i];
        uint8_t t = r.u8();
        switch (t) {
            case 0: { nd.tag = serde::Tag::None; } break;
            case 1: { nd.tag = serde::Tag::Bool; nd.b = r.u8() != 0; } break;
            case 2: { nd.tag = serde::Tag::Integer; nd.i = r.i64(); } break;
            case 3: { nd.tag = serde::Tag::Float; nd.f = r.f64(); } break;
            case 4: { nd.tag = serde::Tag::String; nd.s = r.bytes(r.u32()); } break;
            case 5: { nd.tag = serde::Tag::List; uint32_t c = r.u32(); for (uint32_t k = 0; k < c; ++k) nd.links.push_back(r.u32()); } break;
            case 6: { nd.tag = serde::Tag::Dict; uint32_t c = r.u32(); for (uint32_t k = 0; k < c * 2; ++k) nd.links.push_back(r.u32()); } break;
            case 7: { nd.tag = serde::Tag::Set; uint32_t c = r.u32(); for (uint32_t k = 0; k < c; ++k) nd.links.push_back(r.u32()); } break;
            case 8: { nd.tag = serde::Tag::Object; nd.s = r.bytes(r.u32()); uint32_t c = r.u32(); for (uint32_t k = 0; k < c * 2; ++k) nd.links.push_back(r.u32()); } break;
            case 9: { nd.tag = serde::Tag::Stateful; nd.s = r.bytes(r.u32()); nd.links.push_back(r.u32()); } break;
            default: { throw KiritoError("bad dump tag"); } break;
        }
    }
    uint32_t rootId = r.u32();
    return {std::move(nodes), rootId};
}

inline std::string write(KiritoVM& vm, Handle root) {
    auto [nodes, rootId] = serde::flatten(vm, root, "dump");
    return encode(nodes, rootId);
}

inline Handle read(KiritoVM& vm, const std::string& data) {
    auto [nodes, rootId] = decode(data);
    return serde::rebuild(vm, nodes, rootId);
}

}  // namespace dumpfmt

class DumpModule : public NativeModule {
public:
    std::string name() const override { return "dump"; }
    void setup(ModuleBuilder& m) override {
        // dumps(value) -> Bytes: the compact binary blob (use io.open(path, "wb").write(...) or the
        // dump.save helper to persist it).
        m.fn("dumps", {{"value"}}, "Bytes", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            return vm.alloc(std::make_unique<BytesVal>(dumpfmt::write(vm, Args(vm, a, "dumps")[0])));
        });
        // loads(data) -> value: data is the Bytes from dumps (a String of the same bytes is accepted too).
        m.fn("loads", {{"data"}}, "", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Value v = Args(vm, a, "loads")[0];
            const Object& o = vm.arena().deref(v.handle());
            if (const auto* b = dynamic_cast<const BytesVal*>(&o)) return dumpfmt::read(vm, b->data);
            if (v.isString()) return dumpfmt::read(vm, v.asString());
            throw KiritoError("loads expects a Bytes (or String) of dump data");
        });
        m.fn("save", {{"value"}, {"path", "String"}}, "", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            // save(value, path): serialize `value` straight to a file (dumps + write in one step).
            Args args(vm, a, "save");
            std::string bytes = dumpfmt::write(vm, args[0]);
            std::ofstream f(args[1].asString("save path"), std::ios::binary);
            if (!f) throw KiritoError("could not open file for saving");
            f << bytes;
            return vm.none();
        });
        m.fn("load", {{"path", "String"}}, "", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            std::ifstream f(Args(vm, a, "load")[0].asString("load path"), std::ios::binary);
            if (!f) throw KiritoError("could not open file for loading");
            std::stringstream ss;
            ss << f.rdbuf();
            return dumpfmt::read(vm, ss.str());
        });
    }
};

}  // namespace kirito

#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
#endif
