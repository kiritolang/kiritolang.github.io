#ifndef KIRITO_STDLIB_GZIP_HPP
#define KIRITO_STDLIB_GZIP_HPP

#include <cstdint>
#include <span>
#include <string>

#include "builtins.hpp"
#include "bytes.hpp"
#include "deflate.hpp"
#include "native.hpp"

namespace kirito {

// The gzip container format (RFC 1952): a DEFLATE stream wrapped with a header (magic + flags) and a
// CRC-32 / ISIZE trailer. It is its own thing — `.gz` files, HTTP `Content-Encoding: gzip` — distinct
// from the bare zlib stream (RFC 1950) in the `zlib` module, so it lives in its own `gzip` module.
namespace gzipfmt {

// Wrap a DEFLATE body in the gzip container (RFC 1952). Sets OS = unknown and MTIME = 0 — a valid
// `.gz` stream interoperable with `gzip(1)`/`gunzip`, but not byte-identical to gzip(1)'s own output.
inline std::string compress(const std::string& data) {
    std::string out;
    auto byte = [&](unsigned v) { out += static_cast<char>(static_cast<unsigned char>(v)); };
    byte(0x1f); byte(0x8b); byte(0x08);          // magic, CM = deflate
    byte(0x00);                                  // FLG = 0
    for (int i = 0; i < 4; ++i) byte(0x00);      // MTIME = 0
    byte(0x00); byte(0xff);                      // XFL, OS = unknown
    out += deflate::compress(data);              // raw DEFLATE body
    uint32_t crc = deflate::crc32(data), isize = static_cast<uint32_t>(data.size());
    for (int i = 0; i < 4; ++i) byte((crc >> (8 * i)) & 0xff);
    for (int i = 0; i < 4; ++i) byte((isize >> (8 * i)) & 0xff);
    return out;
}

// Parse a gzip stream and return its decompressed contents. A `.gz` may hold several gzip members
// concatenated (RFC 1952 §2.2 — `cat a.gz b.gz`, what `gunzip` accepts), so loop: for each member
// validate the header, skip the optional FEXTRA/FNAME/FCOMMENT/FHCRC fields per FLG, INFLATE the body
// (the inflate reports how many bytes it consumed so we can locate the trailer + the next member),
// and verify the CRC-32 + ISIZE trailer. Output is the members' bodies concatenated.
inline std::string decompress(const std::string& data) {
    auto b = [&](std::size_t i) -> unsigned { return static_cast<unsigned char>(data[i]); };
    if (data.size() < 18) throw deflate::DeflateError("gzip: stream too short");
    std::string result;
    std::size_t off = 0;
    bool any = false;
    while (off < data.size()) {                  // consume members until the input is exactly exhausted
        // A member needs >=10 header + body + 8 trailer; leftover bytes that can't form one, or that
        // don't begin with the gzip magic, are corruption / trailing junk — reject,
        // not silently ignore.
        if (off + 18 > data.size())
            throw deflate::DeflateError(any ? "gzip: trailing data after the last member"
                                            : "gzip: stream too short");
        if (b(off) != 0x1f || b(off + 1) != 0x8b)
            throw deflate::DeflateError(any ? "gzip: trailing data is not a valid gzip member"
                                            : "gzip: bad magic (not a gzip stream)");
        if (b(off + 2) != 0x08) throw deflate::DeflateError("gzip: unsupported compression method");
        unsigned flg = b(off + 3);
        std::size_t pos = off + 10;              // fixed header: magic, CM, FLG, MTIME, XFL, OS
        if (flg & 0x04) {                        // FEXTRA: 2-byte length, then that many bytes
            if (pos + 2 > data.size()) throw deflate::DeflateError("gzip: truncated FEXTRA");
            std::size_t xlen = b(pos) | (b(pos + 1) << 8);
            pos += 2 + xlen;
        }
        if (flg & 0x08) { while (pos < data.size() && data[pos] != 0) ++pos; ++pos; }  // FNAME (NUL-terminated)
        if (flg & 0x10) { while (pos < data.size() && data[pos] != 0) ++pos; ++pos; }  // FCOMMENT
        if (flg & 0x02) pos += 2;                // FHCRC
        if (pos + 8 > data.size()) throw deflate::DeflateError("gzip: truncated header/stream");
        std::size_t consumed = 0;
        // Cap the AGGREGATE output across ALL members with a shrinking budget, not each member against
        // the full ceiling independently — otherwise a small `.gz` of many members is an unbounded
        // decompression bomb (A16-1: 400 MiB out of a 4.5 MiB input past the 256 MiB per-member cap).
        std::size_t budget = result.size() >= deflate::kMaxInflateOut
                                 ? 0 : deflate::kMaxInflateOut - result.size();
        // A string_view suffix — no copy — so many concatenated members inflate in O(total), not O(n^2).
        std::string out = deflate::inflateImpl(std::string_view(data).substr(pos), budget, &consumed);
        std::size_t t = pos + consumed;          // the 8-byte CRC-32 + ISIZE trailer follows the body
        if (t + 8 > data.size()) throw deflate::DeflateError("gzip: truncated stream");
        uint32_t want = b(t) | (b(t + 1) << 8) | (b(t + 2) << 16) | (static_cast<uint32_t>(b(t + 3)) << 24);
        if (deflate::crc32(out) != want) throw deflate::DeflateError("gzip: CRC-32 mismatch (corrupt stream)");
        // Also verify the ISIZE trailer (uncompressed length mod 2^32), as gzip(1) does — catches a
        // truncation/corruption that leaves the CRC slot intact but the length field wrong.
        uint32_t isize = b(t + 4) | (b(t + 5) << 8) | (b(t + 6) << 16) | (static_cast<uint32_t>(b(t + 7)) << 24);
        if ((static_cast<uint32_t>(out.size()) & 0xFFFFFFFFu) != isize)
            throw deflate::DeflateError("gzip: ISIZE mismatch (corrupt or truncated stream)");
        result += out;
        off = t + 8;
        any = true;
    }
    return result;
}

}  // namespace gzipfmt

// The `gzip` module: gzip-container compress/decompress. Each function accepts a String OR a Bytes
// and returns the same type as its input, so binary data (downloads, `.gz` files) stays byte-correct
// via Bytes while text round-trips as a String.
class GzipModule : public NativeModule {
public:
    std::string name() const override { return "gzip"; }
    void setup(ModuleBuilder& m) override {
        // String-or-Bytes raw view + matching-type result via the shared bytes.hpp helpers.
        auto codec = [&](const char* nm, std::string (*fn)(const std::string&)) {
            m.fn(nm, {{"data"}}, "", [fn, nm](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                Args args(vm, a, nm);
                try {
                    return makeStringOrBytes(vm, args[0].handle(), fn(argStringOrBytes(vm, args[0].handle(), nm)));
                } catch (const deflate::DeflateError& e) {
                    std::string msg = e.what();  // the gzip decode messages already carry the prefix
                    throw KiritoError(msg.rfind("gzip:", 0) == 0 ? msg : "gzip: " + msg);
                }
            });
        };
        codec("compress", gzipfmt::compress);
        codec("decompress", gzipfmt::decompress);
        m.alias("gzip", "compress");             // familiar verb aliases
        m.alias("gunzip", "decompress");
    }
};

}  // namespace kirito

#endif
