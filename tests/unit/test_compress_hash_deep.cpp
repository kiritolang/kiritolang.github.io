// test_compress_hash_deep.cpp — the raw-DEFLATE inflate() failure-message checks: these call
// kirito::deflate::inflate directly on hand-crafted malformed byte streams (no Kirito-script path to
// that internal C++ API), so they stay a C++ TU. Everything else in the original file — malformed
// gzip headers & members, hash() dispatch (incl. `_hash_`), checksum sign-safety, SHA-1 padding
// boundaries, large round-trips, and String-vs-Bytes type preservation — was a pure
// run-a-script-check-a-value slice and was converted to tests/scripts/unit_compress_hash_deep.ki
// (+ .expected).
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

int main() {
    // Two failure classes not otherwise exercised. Raw DEFLATE (no zlib header/Adler trailer) so the
    // crafted bytes reach the inflate loop directly. Both streams are fixed-Huffman, BFINAL=1
    // BTYPE=01 (LSB-first bit packing).
    auto inflateErr = [](const std::string& in) -> std::string {
        try { (void)deflate::inflate(in); return ""; }
        catch (const deflate::DeflateError& e) { return e.what(); }
    };
    // A length/distance pair emitted as the FIRST token: length 3 (sym 257), distance 1 (sym 0), but
    // output is still empty, so distance 1 > out.size() 0 -> "distance too far back".
    CHECK(inflateErr(std::string("\x03\x02", 2)) == "distance too far back");
    // Header consumes 3 bits of the single byte; the 7-bit literal/length code then needs bits past
    // end-of-input -> "unexpected end of deflate stream" (truncated mid-token).
    CHECK(inflateErr(std::string("\x03", 1)) == "unexpected end of deflate stream");
    // Truncated STORED block: BFINAL|BTYPE=00 header byte then fewer than 4 LEN/NLEN bytes.
    CHECK(inflateErr(std::string("\x01\x05", 2)) == "truncated stored block header");
    // Stored block with LEN=5 but a body shorter than 5 bytes -> "truncated stored block".
    CHECK(inflateErr(std::string("\x01\x05\x00\xfa\xff\x41", 6)) == "truncated stored block");

    if (kitest::failures == 0) std::printf("test_compress_hash_deep: all passed\n");
    return RUN_TESTS();
}
