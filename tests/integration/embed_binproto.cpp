// embed_binproto.cpp — a binary message codec over a byte frame. C++ owns the message framing loop
// (a wire buffer that concatenates frames and slices them back out); Kirito owns the frame FORMAT —
// a Function(msg: Dict) -> Bytes encoder and a Function(frame: Bytes) -> Dict decoder. C++ round-
// trips messages through Kirito and inspects the raw bytes (indexing, length) at the boundary.
//
// Wire format (defined entirely in Kirito): [type:1 byte][len:1 byte][body: len utf-8 bytes].
// Flow per message: C++ (message) → Kirito encode → C++ (concatenate onto the wire) → C++ (read the
// length byte to slice one frame) → Kirito decode → C++ (assert it matches the original).

#include <cstdint>
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// The C++-side message the wire carries. Kirito sees it as a Dict {"type": Integer, "body": String}.
struct Message {
    int64_t     type;
    std::string body;
};

static bool operator==(const Message& a, const Message& b) {
    return a.type == b.type && a.body == b.body;
}

// A Codec pairs a Kirito encoder and decoder and lets C++ frame/deframe around them.
class Codec {
public:
    Codec(KiritoVM& vm, Handle encoder, Handle decoder)
        : vm_(vm), encoder_(encoder), decoder_(decoder) {}

    // Encode a C++ Message into a wire frame by handing Kirito a Dict and taking back Bytes.
    Bytes encode(const Message& m) {
        Dict d(vm_);
        d.set("type", Value(vm_, m.type));
        d.set("body", Value(vm_, m.body));
        std::array<Handle, 1> args{d.handle()};
        Handle out = vm_.arena().deref(encoder_).call(vm_, args);
        Value r(vm_, out);
        // The Kirito -> Bytes annotation is enforced at runtime, but assert the C++-visible kind too.
        if (!r.isBytes())
            throw KiritoError("codec: encoder must return Bytes, got '" + r.typeName() + "'");
        return r.asBytes("frame");
    }

    // Decode one wire frame (Bytes) back into a C++ Message via the Kirito decoder.
    Message decode(const Bytes& frame) {
        std::array<Handle, 1> args{frame.handle()};
        Handle out = vm_.arena().deref(decoder_).call(vm_, args);
        Value r(vm_, out);
        Dict d = r.asDict("decoded message");
        return { d["type"].asInt("type"), std::string(d["body"].asStringRef("body")) };
    }

private:
    KiritoVM& vm_;
    Handle    encoder_;
    Handle    decoder_;
};

// A trivial wire: C++ owns the raw bytes; frames are appended, then sliced back out using the length
// byte that the Kirito format puts at index 1. This is the C++-side framing loop the contract wants.
class Wire {
public:
    void append(const Bytes& frame) {
        for (std::size_t i = 0; i < frame.size(); ++i)
            buf_.push_back(static_cast<unsigned char>(frame[i]));
    }
    std::size_t size() const { return buf_.size(); }
    unsigned char at(std::size_t i) const { return buf_.at(i); }

    // Slice the frame starting at `pos`, reading its 1-byte length prefix (byte index 1 of the
    // frame). Returns the wrapped Bytes and advances `pos` past it. Throws if the wire is truncated.
    Bytes readFrame(KiritoVM& vm, std::size_t& pos) const {
        if (pos + 2 > buf_.size())
            throw KiritoError("wire: truncated header");
        std::size_t bodyLen = buf_[pos + 1];
        std::size_t total    = 2 + bodyLen;
        if (pos + total > buf_.size())
            throw KiritoError("wire: truncated body");
        std::string raw(reinterpret_cast<const char*>(buf_.data() + pos), total);
        pos += total;
        return Bytes(vm, raw);
    }

private:
    std::vector<unsigned char> buf_;
};

int main() {
    KiritoVM vm;

    // Kirito ENCODER: {"type": Integer, "body": String} -> Bytes. Uses body.encode() (String -> utf-8
    // Bytes) and prefixes the type byte + a 1-byte length. Bytes(list) builds a frame from Integers.
    Handle encoder = vm.runSource(R"KI(
Function(msg : Dict) -> Bytes:
    var body = msg["body"].encode("utf-8")
    if len(body) > 255:
        throw "body too long to frame"
    var header = Bytes([msg["type"], len(body)])
    return header + body
)KI");

    // Kirito DECODER: Bytes -> {"type": Integer, "body": String}. Reads b[0]/b[1] (each an Integer in
    // 0..255), slices the body Bytes, and .decode()s it back to a String. Throws on a short frame.
    Handle decoder = vm.runSource(R"KI(
Function(frame : Bytes) -> Dict:
    if len(frame) < 2:
        throw "frame too short"
    var kind = frame[0]
    var n = frame[1]
    if len(frame) < 2 + n:
        throw "frame truncated"
    var body = frame[2 : 2 + n]
    return {"type": kind, "body": body.decode("utf-8")}
)KI");

    Codec codec(vm, encoder, decoder);

    // ---- single-frame round trip + byte-level inspection at the boundary ----
    {
        Message m{7, "hello"};
        Bytes frame = codec.encode(m);
        // Wire layout: [type][len][body...]. Verify the raw bytes C++ sees.
        CHECK(frame.size() == 2 + 5);
        CHECK(frame[0] == 7);              // type byte (Integer 0..255)
        CHECK(frame[1] == 5);              // length prefix
        CHECK(frame[2] == 'h');            // first body byte
        // data() exposes the underlying std::string view of the Bytes.
        CHECK(frame.data().substr(2) == "hello");

        Message back = codec.decode(frame);
        CHECK(back == m);
        CHECK(back.type == 7);
        CHECK(back.body == "hello");
    }

    // ---- multi-message wire: C++ concatenates frames, then deframes by the length byte ----
    std::vector<Message> messages{
        {1, "login"},
        {2, "ping"},
        {3, ""},                 // empty body: length prefix 0
        {255, "bye"},            // full-range type byte
        {9, "\x00\x01\x02"},     // non-textual bytes survive utf-8 round trip (latin-safe control chars)
    };

    Wire wire;
    for (const Message& m : messages)
        wire.append(codec.encode(m));

    // The wire is exactly the sum of frame sizes.
    std::size_t expectTotal = 0;
    for (const Message& m : messages)
        expectTotal += 2 + m.body.size();
    CHECK(wire.size() == expectTotal);

    // Deframe the whole wire and decode each frame back through Kirito.
    std::vector<Message> decoded;
    std::size_t pos = 0;
    while (pos < wire.size()) {
        Bytes frame = wire.readFrame(vm, pos);
        decoded.push_back(codec.decode(frame));
    }
    CHECK(pos == wire.size());
    CHECK(decoded.size() == messages.size());
    for (std::size_t i = 0; i < messages.size(); ++i)
        CHECK(decoded[i] == messages[i]);

    // Spot-check the empty-body frame: its length byte is 0 and it is exactly 2 bytes on the wire.
    {
        Bytes f = codec.encode({3, ""});
        CHECK(f.size() == 2);
        CHECK(f[1] == 0);
        CHECK(codec.decode(f).body.empty());
    }

    // ---- Bytes iteration / values: every body byte is an Integer 0..255, verified in Kirito ----
    {
        // sumbytes returns the arithmetic sum of a Bytes' bytes — proves b[i] is 0..255 and iterable.
        Handle sumFn = vm.runSource(R"KI(
Function(b : Bytes) -> Integer:
    var total = 0
    for x in b:
        total = total + x
    return total
)KI");
        Bytes f = codec.encode({0, "AB"});   // frame bytes: 0, 2, 65, 66
        std::array<Handle, 1> args{f.handle()};
        Value s(vm, vm.arena().deref(sumFn).call(vm, args));
        CHECK(s.asInt("sum") == 0 + 2 + 65 + 66);
    }

    // ---- adversarial: decoding a truncated frame (fewer bytes than the length promises) throws ----
    {
        Message m{4, "abcdef"};
        Bytes full = codec.encode(m);
        // Chop the body in half: header still claims 6 body bytes but only 3 are present.
        std::string cut = full.data().substr(0, 2 + 3);
        Bytes broken(vm, cut);
        CHECK_THROWS(codec.decode(broken));
    }

    // ---- adversarial: a one-byte frame (no length prefix at all) throws in the decoder ----
    {
        Bytes stub(vm, std::string("\x05", 1));
        CHECK_THROWS(codec.decode(stub));
    }

    // ---- adversarial: the C++ wire framer rejects a truncated header on the raw buffer ----
    {
        Wire w;
        w.append(Bytes(vm, std::string("\x01", 1)));  // a lone byte, no length
        std::size_t p = 0;
        CHECK_THROWS(w.readFrame(vm, p));
    }

    // ---- adversarial: encoder enforces its own domain rule (body over 255 bytes cannot be framed) ----
    {
        Message big{1, std::string(256, 'x')};
        CHECK_THROWS(codec.encode(big));
    }

    return RUN_TESTS();
}
