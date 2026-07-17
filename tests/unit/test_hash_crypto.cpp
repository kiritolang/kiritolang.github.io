// hash-module crypto extensions: SHA-384/512, HMAC (RFC 2104), PBKDF2 (RFC 8018) and the
// constant-time comparedigest. Covers authoritative RFC known-answer vectors, structural
// properties, adversarial/bad-input handling, and a randomized fuzz loop. Both the C++ core
// (kirito::hashing) and the Kirito-visible module surface are exercised.
#include <random>
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}
static std::string hx(const std::string& raw) { return hashing::toHex(raw); }
static std::string rep(unsigned char b, std::size_t n) { return std::string(n, static_cast<char>(b)); }

int main() {
    // ---- raw digests: NIST SHA-384 / SHA-512 known vectors ----
    CHECK(hashing::sha512("") ==
          "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
          "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
    CHECK(hashing::sha512("abc") ==
          "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
          "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    CHECK(hashing::sha384("") ==
          "38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da"
          "274edebfe76f65fbd51ad2f14898b95b");
    CHECK(hashing::sha384("abc") ==
          "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
          "8086072ba1e7cc2358baeca134c825a7");
    // A message spanning multiple 128-byte blocks (>112 bytes forces an extra padding block).
    CHECK(hashing::sha512(std::string(200, 'a')).size() == 128);
    CHECK(hashing::sha512(std::string(112, 'x')) != hashing::sha512(std::string(113, 'x')));

    const hashing::HashAlgo* md5 = hashing::findAlgo("md5");
    const hashing::HashAlgo* sha1 = hashing::findAlgo("sha1");
    const hashing::HashAlgo* sha256 = hashing::findAlgo("sha256");
    const hashing::HashAlgo* sha384 = hashing::findAlgo("sha384");
    const hashing::HashAlgo* sha512 = hashing::findAlgo("sha512");
    CHECK(md5 && sha1 && sha256 && sha384 && sha512);
    CHECK(hashing::findAlgo("sha3") == nullptr);
    CHECK(hashing::findAlgo("") == nullptr);

    // ---- HMAC: RFC 2202 (MD5/SHA-1) + RFC 4231 (SHA-256/384/512) ----
    CHECK(hx(hashing::hmacRaw(*md5, rep(0x0b, 16), "Hi There")) == "9294727a3638bb1c13f48ef8158bfc9d");
    CHECK(hx(hashing::hmacRaw(*md5, "Jefe", "what do ya want for nothing?")) == "750c783e6ab0b503eaa86e310a5db738");
    CHECK(hx(hashing::hmacRaw(*sha1, rep(0x0b, 20), "Hi There")) == "b617318655057264e28bc0b6fb378c8ef146be00");
    CHECK(hx(hashing::hmacRaw(*sha256, rep(0x0b, 20), "Hi There")) ==
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    CHECK(hx(hashing::hmacRaw(*sha256, "Jefe", "what do ya want for nothing?")) ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    CHECK(hx(hashing::hmacRaw(*sha384, rep(0x0b, 20), "Hi There")) ==
          "afd03944d84895626b0825f4ab46907f15f9dadbe4101ec682aa034c7cebc59c"
          "faea9ea9076ede7f4af152e8b2fa9cb6");
    CHECK(hx(hashing::hmacRaw(*sha512, rep(0x0b, 20), "Hi There")) ==
          "87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cde"
          "daa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854");
    CHECK(hx(hashing::hmacRaw(*sha512, "Jefe", "what do ya want for nothing?")) ==
          "164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea250554"
          "9758bf75c05a994a6d034f65f8f0e6fdcaeab1a34d4a6b4b636e070a38bce737");
    // A key longer than the block is hashed first (RFC 4231 case 3-style: 131-byte key path).
    CHECK(hx(hashing::hmacRaw(*sha256, rep(0xaa, 131), "Test Using Larger Than Block-Size Key - Hash Key First")) ==
          "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");

    // ---- PBKDF2: RFC 6070 (HMAC-SHA1) + RFC 7914 (HMAC-SHA256) ----
    CHECK(hx(hashing::pbkdf2Raw(*sha1, "password", "salt", 1, 20)) == "0c60c80f961f0e71f3a9b524af6012062fe037a6");
    CHECK(hx(hashing::pbkdf2Raw(*sha1, "password", "salt", 2, 20)) == "ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957");
    CHECK(hx(hashing::pbkdf2Raw(*sha1, "password", "salt", 4096, 20)) == "4b007901b765489abead49d926f721d065a429c1");
    CHECK(hx(hashing::pbkdf2Raw(*sha256, "passwd", "salt", 1, 64)) ==
          "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
          "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783");
    // Block independence: the derived key for a smaller dklen is a prefix of a larger one.
    {
        std::string big = hashing::pbkdf2Raw(*sha256, "pw", "salty", 100, 100);
        std::string small = hashing::pbkdf2Raw(*sha256, "pw", "salty", 100, 30);
        CHECK(big.substr(0, 30) == small);
        // dklen=1 must yield exactly one byte, matching the first byte of the block.
        CHECK(hashing::pbkdf2Raw(*sha256, "pw", "salty", 100, 1) == big.substr(0, 1));
    }

    // ---- Kirito module surface mirrors the C++ core ----
    KiritoVM vm;
    vm.installStandardLibrary();
    CHECK(evalStr(vm, "import(\"hash\").sha512(\"abc\")") ==
          "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
          "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    CHECK(evalStr(vm, "import(\"hash\").sha384(\"abc\")") ==
          "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
          "8086072ba1e7cc2358baeca134c825a7");
    CHECK(evalStr(vm, "import(\"hash\").hmac(\"Jefe\", \"what do ya want for nothing?\")") ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    CHECK(evalStr(vm, "import(\"hash\").hmac(\"Jefe\", \"what do ya want for nothing?\", \"sha512\")") ==
          "164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea250554"
          "9758bf75c05a994a6d034f65f8f0e6fdcaeab1a34d4a6b4b636e070a38bce737");
    // pbkdf2 returns Bytes; .hex() gives the hex form.
    CHECK(evalStr(vm, "import(\"hash\").pbkdf2(\"password\", \"salt\", 4096, 20, \"sha1\").hex()") ==
          "4b007901b765489abead49d926f721d065a429c1");
    // keyword args work uniformly.
    CHECK(evalStr(vm, "import(\"hash\").pbkdf2(password=\"password\", salt=\"salt\", iterations=1, dklen=20, algo=\"sha1\").hex()") ==
          "0c60c80f961f0e71f3a9b524af6012062fe037a6");
    // String and Bytes inputs agree (Kirito Strings are byte-transparent).
    CHECK(evalStr(vm, "import(\"hash\").sha512(\"abc\") == import(\"hash\").sha512(Bytes([97,98,99]))") == "True");

    // ---- comparedigest ----
    CHECK(evalStr(vm, "import(\"hash\").comparedigest(\"abc\", \"abc\")") == "True");
    CHECK(evalStr(vm, "import(\"hash\").comparedigest(\"abc\", \"abd\")") == "False");
    CHECK(evalStr(vm, "import(\"hash\").comparedigest(\"abc\", \"abcd\")") == "False");
    CHECK(evalStr(vm, "import(\"hash\").comparedigest(\"\", \"\")") == "True");
    CHECK(evalStr(vm, "import(\"hash\").comparedigest(Bytes([1,2,3]), Bytes([1,2,3]))") == "True");
    CHECK(evalStr(vm, "import(\"hash\").comparedigest(Bytes([1,2,3]), Bytes([1,2,4]))") == "False");

    // ---- adversarial / bad input ----
    CHECK_THROWS(evalStr(vm, "import(\"hash\").hmac(\"k\", \"m\", \"sha3\")"));      // unknown algo
    CHECK_THROWS(evalStr(vm, "import(\"hash\").pbkdf2(\"p\", \"s\", 0)"));            // iterations < 1
    CHECK_THROWS(evalStr(vm, "import(\"hash\").pbkdf2(\"p\", \"s\", -5)"));           // negative iterations
    CHECK_THROWS(evalStr(vm, "import(\"hash\").pbkdf2(\"p\", \"s\", 1, 0)"));         // dklen < 1
    CHECK_THROWS(evalStr(vm, "import(\"hash\").pbkdf2(\"p\", \"s\", 1, 2000000)"));   // dklen too large
    CHECK_THROWS(evalStr(vm, "import(\"hash\").pbkdf2(\"p\", \"s\", 1, 20, \"sha3\")")); // unknown algo
    CHECK_THROWS(evalStr(vm, "import(\"hash\").hmac(123, \"m\")"));                   // non String/Bytes key
    CHECK_THROWS(evalStr(vm, "import(\"hash\").comparedigest(1, 2)"));               // non String/Bytes

    // ---- randomized fuzz: HMAC determinism, avalanche, core/module agreement; RFC-2104 differential ----
    std::mt19937_64 rng(0xC0FFEEu);
    const hashing::HashAlgo* algos[] = {md5, sha1, sha256, sha384, sha512};
    for (int t = 0; t < 3000; ++t) {
        const hashing::HashAlgo& al = *algos[rng() % 5];
        std::size_t klen = rng() % 200, mlen = rng() % 300;
        std::string key, msg;
        for (std::size_t i = 0; i < klen; ++i) key += static_cast<char>(rng() & 0xFF);
        for (std::size_t i = 0; i < mlen; ++i) msg += static_cast<char>(rng() & 0xFF);
        std::string mac = hashing::hmacRaw(al, key, msg);
        CHECK(mac.size() == al.digestLen);
        CHECK(hashing::hmacRaw(al, key, msg) == mac);  // deterministic

        // Independent RFC-2104 restatement (padding written out explicitly here, not via the helper).
        std::string k = key.size() > al.blockLen ? al.raw(key) : key;
        k.resize(al.blockLen, '\0');
        std::string ip(al.blockLen, 0), op(al.blockLen, 0);
        for (std::size_t i = 0; i < al.blockLen; ++i) {
            ip[i] = static_cast<char>(static_cast<unsigned char>(k[i]) ^ 0x36);
            op[i] = static_cast<char>(static_cast<unsigned char>(k[i]) ^ 0x5c);
        }
        CHECK(al.raw(op + al.raw(ip + msg)) == mac);

        // Avalanche: flipping one message bit changes the MAC (non-empty message only).
        if (!msg.empty()) {
            std::string m2 = msg;
            m2[rng() % m2.size()] ^= static_cast<char>(1u << (rng() % 8));
            CHECK(hashing::hmacRaw(al, key, m2) != mac);
        }
    }

    // PBKDF2 fuzz: prefix stability across dklen and iteration monotonic change.
    for (int t = 0; t < 200; ++t) {
        std::string pw, salt;
        for (int i = 0; i < 8; ++i) pw += static_cast<char>(rng() & 0xFF);
        for (int i = 0; i < 8; ++i) salt += static_cast<char>(rng() & 0xFF);
        uint32_t iters = static_cast<uint32_t>(1 + (rng() % 50));
        std::string full = hashing::pbkdf2Raw(*sha256, pw, salt, iters, 50);
        std::string part = hashing::pbkdf2Raw(*sha256, pw, salt, iters, 17);
        CHECK(full.substr(0, 17) == part);
        CHECK(hashing::pbkdf2Raw(*sha256, pw, salt, iters + 1, 50) != full);  // one more round differs
    }

    if (kitest::failures == 0) std::printf("test_hash_crypto: all passed\n");
    return RUN_TESTS();
}
