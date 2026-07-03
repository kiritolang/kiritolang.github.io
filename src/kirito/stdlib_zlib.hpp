#ifndef KIRITO_STDLIB_ZLIB_HPP
#define KIRITO_STDLIB_ZLIB_HPP

#include <cstdint>
#include <span>
#include <string>

#include "builtins.hpp"
#include "bytes.hpp"
#include "deflate.hpp"
#include "native.hpp"

namespace kirito {

// The `zlib` module: zlib-stream (RFC 1950) and raw DEFLATE (RFC 1951) compression. A self-contained
// DEFLATE/INFLATE (no external dependency), interoperable with standard zlib tools. The gzip
// *container* (RFC 1952) is its own thing — see the `gzip` module; the Adler-32 / CRC-32 checksums
// live in the `hash` module. Every function accepts a String OR a Bytes and returns a value of the
// same type as its input, so binary data (downloads, files) stays byte-correct via Bytes while text
// round-trips as a String.
class ZlibModule : public NativeModule {
public:
    std::string name() const override { return "zlib"; }
    void setup(ModuleBuilder& m) override {
        // String-or-Bytes raw view + matching-type result via the shared bytes.hpp helpers.
        // A codec function `data -> data` (same type), translating DeflateError to a clean KiritoError.
        auto codec = [&](const char* name, std::string (*fn)(const std::string&)) {
            m.fn(name, {{"data"}}, "", [fn, name](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                Args args(vm, a, name);
                try {
                    return makeStringOrBytes(vm, args[0].handle(), fn(argStringOrBytes(vm, args[0].handle(), name)));
                } catch (const deflate::DeflateError& e) {
                    throw KiritoError(std::string("zlib: ") + e.what());
                }
            });
        };
        codec("compress", deflate::zlibCompress);
        codec("decompress", deflate::zlibDecompress);
        codec("deflate", deflate::compress);     // raw DEFLATE (no zlib header/trailer)
        codec("inflate", deflate::inflate);
    }
};

}  // namespace kirito

#endif
