#ifndef KIRITO_STDLIB_SERIALIZE_HPP
#define KIRITO_STDLIB_SERIALIZE_HPP

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "native.hpp"
#include "stdlib_serde.hpp"

namespace kirito {

// The `serialize` module: human-readable TEXT serialization of an object graph, preserving shared
// references and cycles. The graph walk and reconstruction are shared with the binary `dump` module
// via serde::flatten / serde::rebuild (stdlib_serde.hpp); this file is only the text codec — how a
// serde::Node table is written to / read from a whitespace-tokenized string.
//
// Format: "KSER1 <count> " then one record per object, then the root id. Each record is a one-letter
// tag and payload: N | B <0|1> | I <int> | F <float> | S <len> <bytes> | L <count> <id…> |
// D <pairs> <key-id val-id …> | T <count> <id…>.
namespace serial {

inline char tagLetter(serde::Tag t) {
    switch (t) {
        case serde::Tag::None: { return 'N'; } break;
        case serde::Tag::Bool: { return 'B'; } break;
        case serde::Tag::Integer: { return 'I'; } break;
        case serde::Tag::Float: { return 'F'; } break;
        case serde::Tag::String: { return 'S'; } break;
        case serde::Tag::List: { return 'L'; } break;
        case serde::Tag::Dict: { return 'D'; } break;
        case serde::Tag::Set: { return 'T'; } break;
        case serde::Tag::Object: { return 'O'; } break;
        case serde::Tag::Stateful: { return 'P'; } break;
    }
    return '?';
}

inline std::string encode(const std::vector<serde::Node>& nodes, uint32_t rootId) {
    std::ostringstream out;
    out << "KSER1 " << nodes.size() << " ";
    for (const serde::Node& n : nodes) {
        out << tagLetter(n.tag) << " ";
        switch (n.tag) {
            case serde::Tag::None: { break; } break;
            case serde::Tag::Bool: { out << (n.b ? 1 : 0) << " "; } break;
            case serde::Tag::Integer: { out << n.i << " "; } break;
            case serde::Tag::Float: {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.17g", n.f);
                out << buf << " ";
            } break;
            case serde::Tag::String: { out << n.s.size() << " " << n.s << " "; } break;
            case serde::Tag::List:
            case serde::Tag::Set: {
                out << n.links.size() << " ";
                for (uint32_t id : n.links) out << id << " ";
            } break;
            case serde::Tag::Dict: {
                out << (n.links.size() / 2) << " ";
                for (uint32_t id : n.links) out << id << " ";
            } break;
            case serde::Tag::Object: {  // class name, then key/val id pairs (like a Dict)
                out << n.s.size() << " " << n.s << " " << (n.links.size() / 2) << " ";
                for (uint32_t id : n.links) out << id << " ";
            } break;
            case serde::Tag::Stateful: {  // class name, then the single state id
                out << n.s.size() << " " << n.s << " " << (n.links.empty() ? 0 : n.links[0]) << " ";
            } break;
        }
    }
    out << rootId;
    return out.str();
}

// Tokenizing reader over the text format.
class TextReader {
public:
    explicit TextReader(const std::string& s) : s_(s) {}

    std::pair<std::vector<serde::Node>, uint32_t> decode() {
        if (token() != "KSER1") throw KiritoError("bad serialization header");
        long n = std::stol(token());
        if (n < 0 || static_cast<std::size_t>(n) > s_.size())
            throw KiritoError("corrupt serialized data: bad object count");
        // Grow incrementally rather than sizing to the untrusted `n` (a Node is ~80 B, min record a few
        // B) so a modest hostile blob can't force a multi-GB zeroed pre-allocation.
        std::vector<serde::Node> nodes;
        nodes.reserve(std::min<std::size_t>(static_cast<std::size_t>(n), std::size_t{1} << 16));
        for (long idx = 0; idx < n; ++idx) {
            nodes.emplace_back();
            serde::Node& nd = nodes.back();
            std::string t = token();
            // Tags are single chars — dispatch on t[0] (mirrors dump's numeric-tag switch).
            if (t.size() != 1) throw KiritoError("bad serialization tag '" + t + "'");
            switch (t[0]) {
                case 'N': { nd.tag = serde::Tag::None; } break;
                case 'B': { nd.tag = serde::Tag::Bool; nd.b = std::stoi(token()) != 0; } break;
                case 'I': { nd.tag = serde::Tag::Integer; nd.i = std::stoll(token()); } break;
                case 'F': { nd.tag = serde::Tag::Float; nd.f = parseDouble(token()); } break;   // parseDouble: subnormals don't trap
                case 'S': { nd.tag = serde::Tag::String; int len = countToken(); nd.s = rawBytes(len); } break;
                case 'L': { nd.tag = serde::Tag::List; readIds(nd.links); } break;
                case 'T': { nd.tag = serde::Tag::Set; readIds(nd.links); } break;
                case 'D': {
                    nd.tag = serde::Tag::Dict;
                    int pairs = countToken();
                    for (long k = 0; k < static_cast<long>(pairs) * 2; ++k) nd.links.push_back(static_cast<uint32_t>(std::stol(token())));
                } break;
                case 'O': {
                    nd.tag = serde::Tag::Object;
                    int len = countToken();
                    nd.s = rawBytes(len);
                    int pairs = countToken();
                    for (long k = 0; k < static_cast<long>(pairs) * 2; ++k) nd.links.push_back(static_cast<uint32_t>(std::stol(token())));
                } break;
                case 'P': {
                    nd.tag = serde::Tag::Stateful;
                    int len = countToken();
                    nd.s = rawBytes(len);
                    nd.links.push_back(static_cast<uint32_t>(std::stol(token())));
                } break;
                default: { throw KiritoError("bad serialization tag '" + t + "'"); } break;
            }
        }
        uint32_t rootId = static_cast<uint32_t>(std::stoul(token()));
        return {std::move(nodes), rootId};
    }

private:
    // A non-negative element/pair count from untrusted text, bounded by the blob length (you cannot have
    // more elements than bytes), so `count*2`/loops can't overflow int and a crafted huge/negative count
    // throws cleanly instead of looping wild or signed-overflowing (UB).
    int countToken() {
        long v = std::stol(token());
        if (v < 0 || v > static_cast<long>(s_.size())) throw KiritoError("corrupt serialized data: bad count");
        return static_cast<int>(v);
    }
    void readIds(std::vector<uint32_t>& out) {
        int c = countToken();
        for (int k = 0; k < c; ++k) out.push_back(static_cast<uint32_t>(std::stol(token())));
    }
    std::string token() {
        while (pos_ < s_.size() && s_[pos_] == ' ') ++pos_;
        std::size_t start = pos_;
        while (pos_ < s_.size() && s_[pos_] != ' ') ++pos_;
        if (start == pos_) throw KiritoError("unexpected end of serialized data");
        return s_.substr(start, pos_ - start);
    }
    std::string rawBytes(int len) {
        if (len < 0) throw KiritoError("corrupt serialized data: bad string length");
        if (pos_ < s_.size() && s_[pos_] == ' ') ++pos_;  // single separator
        if (pos_ + static_cast<std::size_t>(len) > s_.size()) throw KiritoError("truncated string");
        std::string out = s_.substr(pos_, static_cast<std::size_t>(len));
        pos_ += static_cast<std::size_t>(len);
        return out;
    }

    const std::string& s_;
    std::size_t pos_ = 0;
};

inline std::string dumps(KiritoVM& vm, Handle root) {
    auto [nodes, rootId] = serde::flatten(vm, root, "serialize");
    return encode(nodes, rootId);
}

inline Handle loads(KiritoVM& vm, const std::string& text) {
    try {
        auto [nodes, rootId] = TextReader(text).decode();
        return serde::rebuild(vm, nodes, rootId);
    } catch (const KiritoError&) {
        throw;  // already a clean, intentional diagnostic
    } catch (const std::exception& e) {
        // stol/stod on a malformed token, etc. -> a clean Kirito error, never an escape. std::stoi/stol/
        // stoll/stod set what() to just the function name ("stoi"); translate that to a readable message.
        std::string w = e.what();
        if (w == "stoi" || w == "stol" || w == "stoll" || w == "stod" || w == "stoull")
            w = "malformed number";
        throw KiritoError("corrupt serialized data: " + w);
    }
}

}  // namespace serial

class SerializeModule : public NativeModule {
public:
    std::string name() const override { return "serialize"; }
    void setup(ModuleBuilder& m) override {
        m.fn("dumps", {{"value"}}, "String", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            return Value(vm, serial::dumps(vm, Args(vm, a, "dumps")[0]));
        });
        m.fn("loads", {{"text", "String"}}, "", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            return serial::loads(vm, Args(vm, a, "loads")[0].asStringRef("loads"));
        });
        m.fn("save", {{"value"}, {"path", "String"}}, "", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "save");
            std::ofstream f(args[1].asStringRef("save path"), std::ios::binary);
            if (!f) throw KiritoError("could not open file for saving");
            f << serial::dumps(vm, args[0]);
            return Value::None(vm);
        });
        m.fn("load", {{"path", "String"}}, "", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            std::ifstream f(Args(vm, a, "load")[0].asStringRef("load path"), std::ios::binary);
            if (!f) throw KiritoError("could not open file for loading");
            std::stringstream ss;
            ss << f.rdbuf();
            return serial::loads(vm, ss.str());
        });
    }
};

}  // namespace kirito

#endif
