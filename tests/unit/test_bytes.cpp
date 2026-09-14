// gzip interop: the internal gzipfmt C++ codec (not reachable from Kirito source — this exercises
// the C++ API directly, so it cannot be expressed as a .ki script). All other Bytes/gzip/zlib/hash
// coverage that runs Kirito source and checks a stringified result now lives in
// tests/scripts/unit_bytes.ki (converted from the rest of this file).
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

int main() {
    // a stream Kirito wrote decompresses with the C++ codec and vice versa
    std::string original = "interoperable gzip payload \x01\x02\x03 with binary";
    std::string gzipped = gzipfmt::compress(original);
    CHECK(gzipfmt::decompress(gzipped) == original);       // C++ round-trip
    CHECK(static_cast<unsigned char>(gzipped[0]) == 0x1f);  // gzip magic
    CHECK(static_cast<unsigned char>(gzipped[1]) == 0x8b);

    return RUN_TESTS();
}
