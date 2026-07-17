#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

int main() {
    KiritoVM vm;

    // --- MD5 standard test vectors ---
    CHECK(evalStr(vm, "import(\"hash\").md5(\"\")") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(evalStr(vm, "import(\"hash\").md5(\"a\")") == "0cc175b9c0f1b6a831c399e269772661");
    CHECK(evalStr(vm, "import(\"hash\").md5(\"abc\")") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(evalStr(vm, "import(\"hash\").md5(\"message digest\")") == "f96b697d7cb7938d525a2f31aaf161d0");
    CHECK(evalStr(vm, "import(\"hash\").md5(\"abcdefghijklmnopqrstuvwxyz\")") ==
          "c3fcd3d76192e4007dfb496cca67e13b");

    // --- SHA-256 standard test vectors ---
    CHECK(evalStr(vm, "import(\"hash\").sha256(\"\")") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(evalStr(vm, "import(\"hash\").sha256(\"abc\")") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(evalStr(vm, "import(\"hash\").sha256(\"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq\")") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // --- SHA-1 standard test vectors ---
    CHECK(evalStr(vm, "import(\"hash\").sha1(\"\")") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(evalStr(vm, "import(\"hash\").sha1(\"abc\")") == "a9993e364706816aba3e25717850c26c9cd0d89d");

    // --- multi-block inputs (longer than 64 bytes, exercising the padding/length path) ---
    // 1,000,000 'a' characters: the classic FIPS test vector.
    CHECK(evalStr(vm, R"(
var h = import("hash")
var s = "a" * 1000000
h.sha256(s)
)") == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    CHECK(evalStr(vm, R"(
var h = import("hash")
var s = "a" * 1000000
h.md5(s)
)") == "7707d6ae4e027c70eea2a935c2296f21");

    // exactly 64 bytes (one full block, forcing a second padding block)
    CHECK(evalStr(vm, R"(
var h = import("hash")
var s = "0123456789" * 6 + "0123"
h.sha256(s)
)") == "9674d9e078535b7cec43284387a6ee39956188e735a85452b0050b55341cda56");

    // --- binary-safe data (all 256 byte values) ---
    CHECK(evalStr(vm, R"(
var h = import("hash")
var s = ""
for i in range(256):
    s = s + "\x00"
len(h.sha256(s))
)") == "64");  // sha256 hex digest is always 64 chars

    // digest lengths are fixed regardless of input
    CHECK(evalStr(vm, "len(import(\"hash\").md5(\"anything\"))") == "32");
    CHECK(evalStr(vm, "len(import(\"hash\").sha1(\"anything\"))") == "40");
    CHECK(evalStr(vm, "len(import(\"hash\").sha256(\"anything\"))") == "64");

    // determinism: same input -> same digest
    CHECK(evalStr(vm, R"(
var h = import("hash")
h.sha256("repeatable") == h.sha256("repeatable")
)") == "True");
    // avalanche: one-bit change -> different digest
    CHECK(evalStr(vm, R"(
var h = import("hash")
h.sha256("hello") != h.sha256("hellp")
)") == "True");

    // --- checksums: Adler-32 / CRC-32 (the hash module's fast non-cryptographic digests) ---
    CHECK(evalStr(vm, "import(\"hash\").crc32(\"hello\")") == "907060870");   // known constant
    CHECK(evalStr(vm, "import(\"hash\").crc32(\"\")") == "0");
    CHECK(evalStr(vm, "import(\"hash\").adler32(\"\")") == "1");
    CHECK(evalStr(vm, "import(\"hash\").adler32(\"Wikipedia\")") == "300286872");  // the classic example
    CHECK(evalStr(vm, "import(\"hash\").crc64(\"\")") == "0");
    // CRC-64/XZ check value for "123456789" is 0x995dc9bbdf1939fa (a signed int64 -> negative)
    CHECK(evalStr(vm, "import(\"hash\").crc64(\"123456789\")") == "-7395533204333446662");

    // --- every function accepts a Bytes byte-identically to the same-byte String ---
    CHECK(evalStr(vm, "var h = import(\"hash\")\nh.crc32(Bytes([104,101,108,108,111])) == h.crc32(\"hello\")") == "True");
    CHECK(evalStr(vm, "var h = import(\"hash\")\nh.sha256(\"abc\".encode()) == h.sha256(\"abc\")") == "True");
    CHECK(evalStr(vm, "var h = import(\"hash\")\nh.md5(Bytes([97,98,99])) == h.md5(\"abc\")") == "True");
    // binary data with high bytes hashes (a String can't address these losslessly; Bytes can)
    CHECK(evalStr(vm, "len(import(\"hash\").sha256(Bytes([0, 255, 128, 1, 254])))") == "64");

    // hostile: wrong argument types throw, not crash
    CHECK_THROWS(vm.runSource("import(\"hash\").md5(42)"));
    CHECK_THROWS(vm.runSource("import(\"hash\").sha256([1, 2, 3])"));
    CHECK_THROWS(vm.runSource("import(\"hash\").sha1(None)"));
    CHECK_THROWS(vm.runSource("import(\"hash\").crc32(None)"));

    return RUN_TESTS();
}
