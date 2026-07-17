// test_compress_hash_deep.cpp — adversarial/edge coverage for zlib/gzip/hash: malformed gzip
// headers & members, hash() dispatch (incl. `_hash_`), checksum sign-safety, SHA-1 padding
// boundaries, large round-trips, and String-vs-Bytes type preservation.
#include <cctype>
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// Run a Kirito program, return the last expression stringified.
static std::string run(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}
// True iff the program throws (a KiritoError or any std::exception crossing the boundary).
static bool throws(KiritoVM& vm, const std::string& src) {
    try { vm.runSource(src); return false; }
    catch (...) { return true; }
}
// The error message a failing program raises ("" if it did not raise).
static std::string errmsg(KiritoVM& vm, const std::string& src) {
    try { vm.runSource(src); return ""; }
    catch (const std::exception& e) { return e.what(); }
    catch (...) { return "?"; }
}
static bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}
static bool isHexDigest(const std::string& s, std::size_t n) {
    if (s.size() != n) return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

int main() {
    KiritoVM vm;

    // ---------------------------------------------------------------------------------------------
    // gzip: FLG header fields (FNAME/FEXTRA/FCOMMENT/FHCRC) must be parsed & skipped correctly.
    // We build a valid FLG=0 stream via gzip.compress(Bytes), then splice header flags + fields in
    // by hand (Bytes slicing/concat) and assert the payload is still recovered exactly.
    // Layout: [0..1]=magic, [2]=CM, [3]=FLG, [4..9]=MTIME/XFL/OS, [10..]=DEFLATE body + trailer.
    // ---------------------------------------------------------------------------------------------
    const char* kPreamble =
        "var g = import(\"gzip\")\n"
        "var payload = Bytes(\"hello gzip world, hello gzip world, hello!\")\n"
        "var z = g.compress(payload)\n";

    // FNAME (0x08): NUL-terminated file name after the fixed header.
    CHECK(run(vm, std::string(kPreamble) + R"KI(
var name = Bytes("archive.txt") + Bytes([0])
var spliced = z[0:3] + Bytes([0x08]) + z[4:10] + name + z[10:len(z)]
g.decompress(spliced) == payload
)KI") == "True");

    // FEXTRA (0x04): 2-byte little-endian length, then that many extra bytes.
    CHECK(run(vm, std::string(kPreamble) + R"KI(
var extra = Bytes([4, 0]) + Bytes([1, 2, 3, 4])
var spliced = z[0:3] + Bytes([0x04]) + z[4:10] + extra + z[10:len(z)]
g.decompress(spliced) == payload
)KI") == "True");

    // FCOMMENT (0x10): NUL-terminated comment.
    CHECK(run(vm, std::string(kPreamble) + R"KI(
var comment = Bytes("a comment") + Bytes([0])
var spliced = z[0:3] + Bytes([0x10]) + z[4:10] + comment + z[10:len(z)]
g.decompress(spliced) == payload
)KI") == "True");

    // FHCRC (0x02): a 2-byte header CRC16 that decompress skips (does not verify) — arbitrary bytes.
    CHECK(run(vm, std::string(kPreamble) + R"KI(
var spliced = z[0:3] + Bytes([0x02]) + z[4:10] + Bytes([0xAB, 0xCD]) + z[10:len(z)]
g.decompress(spliced) == payload
)KI") == "True");

    // ALL flags at once (0x04|0x08|0x10|0x02 = 0x1E), fields in RFC-1952 order
    // FEXTRA, FNAME, FCOMMENT, FHCRC.
    CHECK(run(vm, std::string(kPreamble) + R"KI(
var extra   = Bytes([3, 0]) + Bytes([9, 9, 9])
var name    = Bytes("f.bin") + Bytes([0])
var comment = Bytes("c") + Bytes([0])
var fhcrc   = Bytes([0x12, 0x34])
var spliced = z[0:3] + Bytes([0x1E]) + z[4:10] + extra + name + comment + fhcrc + z[10:len(z)]
g.decompress(spliced) == payload
)KI") == "True");

    // Empty FNAME (just the terminating NUL) is still valid.
    CHECK(run(vm, std::string(kPreamble) + R"KI(
var spliced = z[0:3] + Bytes([0x08]) + z[4:10] + Bytes([0]) + z[10:len(z)]
g.decompress(spliced) == payload
)KI") == "True");

    // --- gzip malformed / adversarial paths ---

    // Unsupported compression method: CM != 8 must throw.
    CHECK(throws(vm, std::string(kPreamble) + "g.decompress(z[0:2] + Bytes([9]) + z[3:len(z)])"));
    CHECK(contains(errmsg(vm, std::string(kPreamble) + "g.decompress(z[0:2] + Bytes([0]) + z[3:len(z)])"),
                   "unsupported compression method"));

    // Bad magic (not a gzip stream at all) — but long enough to pass the length guard.
    CHECK(contains(errmsg(vm, std::string(kPreamble) + "g.decompress(Bytes([0,0]) + z[2:len(z)])"),
                   "bad magic"));

    // Truncated FEXTRA: FLG claims FEXTRA with a length that overruns the stream -> throw.
    CHECK(throws(vm, std::string(kPreamble) + R"KI(
g.decompress(z[0:3] + Bytes([0x04]) + z[4:10] + Bytes([0xFF, 0xFF]) + z[10:len(z)])
)KI"));

    // Truncated FNAME: FLG sets FNAME but no NUL terminator before end-of-stream -> throw.
    CHECK(throws(vm, std::string(kPreamble) + R"KI(
g.decompress(z[0:3] + Bytes([0x08]) + z[4:10] + Bytes([65, 66, 67]))
)KI"));

    // Trailing garbage after a complete member: reject as "trailing data ...".
    {
        std::string trailing = errmsg(vm, std::string(kPreamble) + "g.decompress(z + Bytes([1, 2, 3]))");
        CHECK(trailing != "");
        CHECK(contains(trailing, "trailing"));
        // A corrupt FIRST member (flip a body byte) fails DIFFERENTLY — NOT a "trailing" error.
        std::string corrupt = errmsg(vm, std::string(kPreamble) +
            "g.decompress(z[0:12] + Bytes([(z[12] + 1) % 256]) + z[13:len(z)])");
        CHECK(corrupt != "");
        CHECK(!contains(corrupt, "trailing"));
    }

    // A too-short stream (below the 18-byte header+trailer floor) throws.
    CHECK(throws(vm, "import(\"gzip\").decompress(Bytes([0x1f, 0x8b, 0x08, 0x00]))"));

    // Multi-member concatenation (cat a.gz b.gz) decompresses to the concatenation of bodies.
    CHECK(run(vm, R"KI(
var g = import("gzip")
g.decompress(g.compress(Bytes("AAA")) + g.compress(Bytes("BBB"))) == Bytes("AAABBB")
)KI") == "True");

    // Truncated trailer (drop the last CRC/ISIZE bytes) throws.
    CHECK(throws(vm, std::string(kPreamble) + "g.decompress(z[0:len(z)-4])"));

    // ---------------------------------------------------------------------------------------------
    // hash(value): the exposed Dict/Set hash. Works on Bytes/Float/Bool and dispatches `_hash_`.
    // ---------------------------------------------------------------------------------------------
    CHECK(run(vm, "type(import(\"hash\").hash(Bytes([1,2,3])))") == "Integer");
    CHECK(run(vm, "type(import(\"hash\").hash(3.14))") == "Integer");
    CHECK(run(vm, "type(import(\"hash\").hash(True))") == "Integer");
    CHECK(run(vm, "type(import(\"hash\").hash(\"str\"))") == "Integer");
    CHECK(run(vm, "type(import(\"hash\").hash(42))") == "Integer");
    CHECK(run(vm, "type(import(\"hash\").hash(None))") == "Integer");

    // stability across two calls (same value hashes the same both times).
    CHECK(run(vm, R"KI(
var h = import("hash")
h.hash(Bytes([9,8,7])) == h.hash(Bytes([9,8,7]))
)KI") == "True");
    CHECK(run(vm, R"KI(
var h = import("hash")
h.hash(3.5) == h.hash(3.5) and h.hash(True) == h.hash(True)
)KI") == "True");

    // `_hash_` IS dispatched: a class whose _hash_ returns a fixed Integer -> hash(instance) == it.
    CHECK(run(vm, R"KI(
class Fixed:
    var _hash_ = Function(self):
        return 777
import("hash").hash(Fixed())
)KI") == "777");
    // Distinct instances of the same class share the fixed hash (proves dispatch, not identity).
    CHECK(run(vm, R"KI(
class Fixed:
    var _hash_ = Function(self):
        return 12321
var h = import("hash")
h.hash(Fixed()) == h.hash(Fixed())
)KI") == "True");

    // Unhashable containers throw the standard "unhashable type" error.
    CHECK(throws(vm, "import(\"hash\").hash([1,2,3])"));
    CHECK(throws(vm, "import(\"hash\").hash({\"k\": 1})"));
    CHECK(throws(vm, "import(\"hash\").hash(Set([1,2,3]))"));
    CHECK(contains(errmsg(vm, "import(\"hash\").hash([1,2,3])"), "unhashable"));

    // ---------------------------------------------------------------------------------------------
    // crc64: always an Integer; distinct inputs distinct; at least one input has the top bit CLEAR
    // (a non-negative Integer, no sign-extension surprise).
    // ---------------------------------------------------------------------------------------------
    {
        bool sawNonNegative = false;
        for (char c = 'a'; c <= 'z'; ++c) {
            std::string in(1, c);
            CHECK(run(vm, "type(import(\"hash\").crc64(\"" + in + "\"))") == "Integer");
            if (run(vm, "import(\"hash\").crc64(\"" + in + "\") >= 0") == "True")
                sawNonNegative = true;
        }
        CHECK(sawNonNegative);
        // a spread of distinct inputs yield distinct crc64 values.
        CHECK(run(vm, R"KI(
var h = import("hash")
var xs = ["", "a", "b", "ab", "ba", "hello", "world", "hello!"]
var seen = Set()
for x in xs:
    seen.add(h.crc64(x))
len(seen) == len(xs)
)KI") == "True");
        // crc64 is deterministic and String/Bytes-equivalent for the same bytes.
        CHECK(run(vm, "var h = import(\"hash\")\nh.crc64(\"abc\") == h.crc64(Bytes(\"abc\"))") == "True");
    }

    // ---------------------------------------------------------------------------------------------
    // adler32 / crc32: always a NON-NEGATIVE Integer (no sign-extension), incl. high-bit input.
    // ---------------------------------------------------------------------------------------------
    {
        const char* inputs[] = {
            "\"\"", "\"a\"", "\"abc\"",
            "Bytes([255,255,255,255])", "Bytes([255,254,253,252,251])",
            "Bytes([0,0,0,0])", "Bytes([128,200,255,1])",
        };
        for (const char* in : inputs) {
            std::string a = std::string("import(\"hash\").adler32(") + in + ")";
            std::string c = std::string("import(\"hash\").crc32(") + in + ")";
            CHECK(run(vm, "type(" + a + ")") == "Integer");
            CHECK(run(vm, "type(" + c + ")") == "Integer");
            CHECK(run(vm, a + " >= 0") == "True");
            CHECK(run(vm, c + " >= 0") == "True");
        }
        // String/Bytes equivalence for the same bytes.
        CHECK(run(vm, "var h = import(\"hash\")\nh.crc32(\"abc\") == h.crc32(Bytes(\"abc\"))") == "True");
        CHECK(run(vm, "var h = import(\"hash\")\nh.adler32(\"abc\") == h.adler32(Bytes(\"abc\"))") == "True");
    }

    // ---------------------------------------------------------------------------------------------
    // SHA-1: exact known vectors, multi-block, and the 55/56/63/64-byte padding boundaries.
    // ---------------------------------------------------------------------------------------------
    CHECK(run(vm, "import(\"hash\").sha1(\"\")") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(run(vm, "import(\"hash\").sha1(\"abc\")") == "a9993e364706816aba3e25717850c26c9cd0d89d");

    // Exact-length hex + determinism at each interesting length (padding boundaries around 56/64).
    {
        int lengths[] = {1, 54, 55, 56, 57, 63, 64, 65, 119, 128, 200};
        std::string prev;
        for (int n : lengths) {
            std::string src = "import(\"hash\").sha1(\"a\" * " + std::to_string(n) + ")";
            std::string d = run(vm, src);
            CHECK(isHexDigest(d, 40));
            CHECK(run(vm, src) == d);       // deterministic
            if (!prev.empty()) CHECK(d != prev);  // different lengths -> different digests
            prev = d;
        }
    }
    // Avalanche across the 55->56 boundary (the extra byte forces a second padding block).
    CHECK(run(vm, R"KI(
var h = import("hash")
h.sha1("a" * 55) != h.sha1("a" * 56)
)KI") == "True");
    // A single-bit-ish change flips the whole digest.
    CHECK(run(vm, R"KI(
var h = import("hash")
h.sha1("The quick brown fox") != h.sha1("The quick brown fox.")
)KI") == "True");

    // exact empty-input digests for md5/sha256 too.
    CHECK(run(vm, "import(\"hash\").md5(\"\")") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(run(vm, "import(\"hash\").sha256(\"\")") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    // ---------------------------------------------------------------------------------------------
    // Large round-trips (~1.5 MB) recover the original exactly — zlib, raw deflate, gzip.
    // ---------------------------------------------------------------------------------------------
    CHECK(run(vm, R"KI(
var z = import("zlib")
var s = "abc123" * 260000
z.decompress(z.compress(s)) == s
)KI") == "True");
    CHECK(run(vm, R"KI(
var z = import("zlib")
var s = "The quick brown fox jumps over the lazy dog. " * 40000
z.inflate(z.deflate(s)) == s
)KI") == "True");
    CHECK(run(vm, R"KI(
var g = import("gzip")
var s = "xyzXYZ0123456789" * 100000
g.decompress(g.compress(s)) == s
)KI") == "True");
    // Large Bytes round-trip through gzip stays byte-exact.
    CHECK(run(vm, R"KI(
var g = import("gzip")
var b = Bytes("payload-block-") * 90000
g.decompress(g.compress(b)) == b
)KI") == "True");

    // ---------------------------------------------------------------------------------------------
    // Type preservation: digests/checksums return String/Integer; codecs return the input's type.
    // ---------------------------------------------------------------------------------------------
    // digests -> String for both String and Bytes input.
    for (const char* fn : {"md5", "sha1", "sha256"}) {
        std::string f = std::string("import(\"hash\").") + fn;
        CHECK(run(vm, "type(" + f + "(\"data\"))") == "String");
        CHECK(run(vm, "type(" + f + "(Bytes([1,2,3])))") == "String");
        // same bytes -> same digest regardless of the input's Kirito type.
        CHECK(run(vm, "var h = import(\"hash\")\nh." + std::string(fn) +
                      "(\"abc\") == h." + std::string(fn) + "(Bytes(\"abc\"))") == "True");
    }
    // checksums -> Integer for both.
    for (const char* fn : {"adler32", "crc32", "crc64"}) {
        std::string f = std::string("import(\"hash\").") + fn;
        CHECK(run(vm, "type(" + f + "(\"data\"))") == "Integer");
        CHECK(run(vm, "type(" + f + "(Bytes([1,2,3])))") == "Integer");
    }
    // zlib/gzip codecs: String in -> String out; Bytes in -> Bytes out; round-trip preserves type.
    for (const char* mod : {"zlib", "gzip"}) {
        std::string imp = std::string("import(\"") + mod + "\")";
        CHECK(run(vm, "type(" + imp + ".compress(\"hi\"))") == "String");
        CHECK(run(vm, "type(" + imp + ".compress(Bytes([1,2,3])))") == "Bytes");
        CHECK(run(vm, "type(" + imp + ".decompress(" + imp + ".compress(\"hi\")))") == "String");
        CHECK(run(vm, "type(" + imp + ".decompress(" + imp + ".compress(Bytes([1,2,3]))))") == "Bytes");
        // round-trip value preservation for both kinds.
        CHECK(run(vm, imp + ".decompress(" + imp + ".compress(\"round trip text\")) == \"round trip text\"") == "True");
        CHECK(run(vm, imp + ".decompress(" + imp + ".compress(Bytes([0,255,127,1,254]))) == Bytes([0,255,127,1,254])") == "True");
    }
    // raw deflate/inflate type preservation.
    CHECK(run(vm, "type(import(\"zlib\").deflate(\"x\"))") == "String");
    CHECK(run(vm, "type(import(\"zlib\").deflate(Bytes([1,2])))") == "Bytes");
    CHECK(run(vm, "type(import(\"zlib\").inflate(import(\"zlib\").deflate(Bytes([1,2]))))") == "Bytes");

    // empty-input codec round-trips.
    CHECK(run(vm, "import(\"zlib\").decompress(import(\"zlib\").compress(\"\")) == \"\"") == "True");
    CHECK(run(vm, "import(\"gzip\").decompress(import(\"gzip\").compress(Bytes())) == Bytes()") == "True");

    return RUN_TESTS();
}
