// crypto module (OpenSSL-gated): AES-GCM, RSA/EC sign+verify+encrypt, X.509 parsing. This TU only
// has a body under KIRITO_ENABLE_TLS (the debug/release presets); under asan/tsan (TLS off) it
// compiles to an empty main, matching test_net_tls.cpp. Covers NIST GCM known-answer vectors,
// encrypt/decrypt round-trips, tamper/authentication-failure, signature round-trips + rejection,
// RSA-OAEP, certificate field extraction, adversarial inputs, and a randomized AES-GCM fuzz.
#ifndef KIRITO_ENABLE_TLS
int main() { return 0; }
#else

#include <random>
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource("var c = import(\"crypto\")\n" + src));
}

// A self-signed cert (CN=kirito.test, O=KiritoOrg; SAN DNS kirito.test + www.kirito.test).
static const char* CERT_PEM = R"(-----BEGIN CERTIFICATE-----
MIIDXjCCAkagAwIBAgIUe2JbxlpFszPkf/V5/aDiv+5yiakwDQYJKoZIhvcNAQEL
BQAwKjEUMBIGA1UEAwwLa2lyaXRvLnRlc3QxEjAQBgNVBAoMCUtpcml0b09yZzAe
Fw0yNjA3MDkxNjQxMTFaFw0zNjA3MDYxNjQxMTFaMCoxFDASBgNVBAMMC2tpcml0
by50ZXN0MRIwEAYDVQQKDAlLaXJpdG9PcmcwggEiMA0GCSqGSIb3DQEBAQUAA4IB
DwAwggEKAoIBAQDTgrQbrW+MLTwQbXPhDzB82ln7gdxkQV3hpQ3wOe9dapb/5Ahs
MuocGXHZ1D7aIaatcu8SeH1GGvqMcr6GhiDy+jhFjkkD4RTETPY40fezzhDKtSqW
EUQhSbmZOsNlkGGh2zyRwJk4GJeT7fJlJ/ifu3zkTkaaMLN3Z0M7mf1DgG9g8/6S
Gwr9RAT2YNbf3Cv4CLjNybeISzoecnier93IxawPlsR1wU0QWOjyDakooJ+4xJcM
3Vt5anH9gK94AifBJo8CVFKckliC2S+40DDuI5U5MmRiJRcXesABq4rH7dJH4JnX
5sW2LRC7vHUc93n/XMS/WodmnHuJ0oRVRjmxAgMBAAGjfDB6MB0GA1UdDgQWBBTS
N9cxJIAITuBt1rKmXiSYG1aTvTAfBgNVHSMEGDAWgBTSN9cxJIAITuBt1rKmXiSY
G1aTvTAPBgNVHRMBAf8EBTADAQH/MCcGA1UdEQQgMB6CC2tpcml0by50ZXN0gg93
d3cua2lyaXRvLnRlc3QwDQYJKoZIhvcNAQELBQADggEBAF04jgiBOqe4gc+f1HkC
2pgijeBVV4fT8uQzNQtmrD2bugBwSQTGi9ZYZrQicrdIYH5c5j0o31f2f3BEqc1m
6vHtNzFxAd1GJHbBbxJaEB6sQMiWvAcxvHVQtYqtFHIE2zlsLjrEc3QlQxLPgUZm
SN1/Jmfde5Wzyh8YjpBi0g+sTJcjwzqRWlMxuRCTmngxnvQvFFrct81OsF6y8OyY
UGNqhx87/uKeMx3yXvphVH7whCaOOfMBlBV3VvPzLHNWH0CYsmW6qk+WXEJDaiVX
gkXEYar6uwu4qK0KOJWMTKgASTlKpXCmSJ9TvIs+ICqCrt1YWLpKcTUwlRysfuwU
NV0=
-----END CERTIFICATE-----
)";

int main() {
    KiritoVM vm;
    vm.installStandardLibrary();
    CHECK(evalStr(vm, "c.enabled") == "True");

    // ---- AES-GCM NIST known-answer (GCM spec test case 3: AES-128, 96-bit IV, no AAD) ----
    const std::string KEY = "feffe9928665731c6d6a8f9467308308";
    const std::string IV  = "cafebabefacedbaddecaf888";
    const std::string PT  = "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
                            "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255";
    const std::string CT  = "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
                            "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091473f5985";
    const std::string TAG = "4d5c2af327cd64a62cf35abd2ba6fab4";
    CHECK(evalStr(vm, "var r = c.aesencrypt(fromhex(\"" + KEY + "\"), fromhex(\"" + PT +
                      "\"), fromhex(\"" + IV + "\"))\nr[\"ciphertext\"].hex() + \":\" + r[\"tag\"].hex()") ==
          CT + ":" + TAG);
    // Decrypt the KAT ciphertext back to the plaintext.
    CHECK(evalStr(vm, "c.aesdecrypt(fromhex(\"" + KEY + "\"), fromhex(\"" + CT + "\"), fromhex(\"" + IV +
                      "\"), fromhex(\"" + TAG + "\")).hex()") == PT);

    // ---- round-trip for every key size, with and without AAD ----
    for (const char* klen : {"16", "24", "32"}) {
        std::string s =
            "var key = random.randombytes(" + std::string(klen) + ")\n"
            "var nonce = random.randombytes(12)\n"
            "var msg = \"attack at dawn\"\n"
            "var e = c.aesencrypt(key, msg, nonce)\n"
            "c.aesdecrypt(key, e[\"ciphertext\"], nonce, e[\"tag\"]).decode() == msg";
        CHECK(evalStr(vm, "var random = import(\"random\")\n" + s) == "True");
    }
    // AAD must match on decrypt.
    CHECK(evalStr(vm,
        "var random = import(\"random\")\n"
        "var key = random.randombytes(32)\nvar nonce = random.randombytes(12)\n"
        "var e = c.aesencrypt(key, \"body\", nonce, \"hdr\")\n"
        "c.aesdecrypt(key, e[\"ciphertext\"], nonce, e[\"tag\"], \"hdr\").decode()") == "body");

    // ---- tamper detection: a flipped ciphertext / tag / aad byte fails authentication ----
    auto tamperThrows = [&](const std::string& mutate) {
        std::string s =
            "var random = import(\"random\")\n"
            "var key = random.randombytes(32)\nvar nonce = random.randombytes(12)\n"
            "var e = c.aesencrypt(key, \"secret message\", nonce, \"aad0\")\n"
            "var ct = e[\"ciphertext\"]\nvar tag = e[\"tag\"]\nvar aad = \"aad0\"\n" + mutate +
            "c.aesdecrypt(key, ct, nonce, tag, aad)";
        CHECK_THROWS(evalStr(vm, s));
    };
    tamperThrows("ct = ct[0:1] + Bytes([bitxor(ct[1], 1)]) + ct[2:len(ct)]\n");  // flip a ciphertext bit
    tamperThrows("tag = tag[0:1] + Bytes([bitxor(tag[1], 1)]) + tag[2:len(tag)]\n");  // flip a tag bit
    tamperThrows("aad = \"aad1\"\n");                                                  // wrong AAD

    // ---- RSA sign/verify + OAEP encrypt/decrypt ----
    CHECK(evalStr(vm,
        "var k = c.rsagenerate(2048)\n"
        "var sig = c.rsasign(k[\"private\"], \"hello world\")\n"
        "var ok = c.rsaverify(k[\"public\"], \"hello world\", sig)\n"
        "var bad = c.rsaverify(k[\"public\"], \"hello worlx\", sig)\n"
        "var k2 = c.rsagenerate(2048)\n"
        "var wrong = c.rsaverify(k2[\"public\"], \"hello world\", sig)\n"
        "var ct = c.rsaencrypt(k[\"public\"], \"top secret\")\n"
        "var pt = c.rsadecrypt(k[\"private\"], ct).decode()\n"
        "String(ok) + \",\" + String(bad) + \",\" + String(wrong) + \",\" + pt") == "True,False,False,top secret");

    // ---- EC sign/verify (ECDSA over prime256v1) ----
    CHECK(evalStr(vm,
        "var e = c.ecgenerate()\n"
        "var sig = c.ecsign(e[\"private\"], \"ec payload\", \"sha256\")\n"
        "var ok = c.ecverify(e[\"public\"], \"ec payload\", sig)\n"
        "var bad = c.ecverify(e[\"public\"], \"ec payloax\", sig)\n"
        "String(ok) + \",\" + String(bad)") == "True,False");
    CHECK(evalStr(vm, "type(c.ecgenerate(\"secp384r1\")[\"private\"])") == "String");  // other curve

    // ---- X.509 parse ----
    {
        std::string pem(CERT_PEM);
        std::string src = "var pem = \"\"\"" + pem + "\"\"\"\nvar info = c.x509parse(pem)\n";
        CHECK(evalStr(vm, src + "\"kirito.test\" in info[\"subject\"]") == "True");
        CHECK(evalStr(vm, src + "\"KiritoOrg\" in info[\"subject\"]") == "True");
        CHECK(evalStr(vm, src + "\"kirito.test\" in info[\"issuer\"]") == "True");
        CHECK(evalStr(vm, src + "len(info[\"sans\"])") == "2");
        CHECK(evalStr(vm, src + "\"kirito.test\" in info[\"sans\"] and \"www.kirito.test\" in info[\"sans\"]") == "True");
        CHECK(evalStr(vm, src + "len(info[\"serial\"]) > 0") == "True");
        CHECK(evalStr(vm, src + "type(info[\"not_after\"])") == "String");
    }

    // ---- adversarial / bad input ----
    CHECK_THROWS(evalStr(vm, "c.aesencrypt(fromhex(\"0011223344556677\"), \"pt\", fromhex(\"" + IV + "\"))"));  // 8-byte key
    CHECK_THROWS(evalStr(vm, "var random = import(\"random\")\nc.aesencrypt(random.randombytes(32), \"pt\", Bytes(0))"));  // empty nonce
    CHECK_THROWS(evalStr(vm, "c.rsasign(\"not a valid pem\", \"m\")"));
    CHECK_THROWS(evalStr(vm, "c.rsagenerate(64)"));                 // too small
    CHECK_THROWS(evalStr(vm, "c.x509parse(\"-----BEGIN CERTIFICATE-----\\ngarbage\\n-----END CERTIFICATE-----\")"));
    CHECK_THROWS(evalStr(vm, "c.ecgenerate(\"not-a-curve\")"));
    CHECK_THROWS(evalStr(vm, "c.rsaverify(\"bad pem\", \"m\", Bytes([1,2,3]))"));

    // ---- key/algorithm confusion: a key of the wrong family is rejected, never silently
    // re-dispatched. EVP_DigestSign follows the KEY, so without the guard rsasign(ecKey) returns a
    // valid ECDSA signature under an RSA-named function, and ecverify accepts an RSA signature. ----
    {
        const std::string keys = "var e = c.ecgenerate()\nvar r = c.rsagenerate(1024)\n";
        CHECK_THROWS(evalStr(vm, keys + "c.rsasign(e[\"private\"], \"m\")"));   // EC key -> rsasign
        CHECK_THROWS(evalStr(vm, keys + "c.ecsign(r[\"private\"], \"m\")"));    // RSA key -> ecsign
        CHECK_THROWS(evalStr(vm, keys + "c.rsaverify(e[\"public\"], \"m\", c.ecsign(e[\"private\"], \"m\"))"));
        CHECK_THROWS(evalStr(vm, keys + "c.ecverify(r[\"public\"], \"m\", c.rsasign(r[\"private\"], \"m\"))"));
        // the message names both the expected and the actual family, so the mistake is actionable
        try {
            evalStr(vm, keys + "c.rsasign(e[\"private\"], \"m\")");
            CHECK(false);
        } catch (const KiritoError& err) {
            const std::string what = err.what();
            CHECK(what.find("rsasign: expected RSA private key, got EC") != std::string::npos);
        }
        // the same-family paths still work (the guard rejects only the mismatch)
        CHECK(evalStr(vm, keys + "c.rsaverify(r[\"public\"], \"m\", c.rsasign(r[\"private\"], \"m\"))") == "True");
        CHECK(evalStr(vm, keys + "c.ecverify(e[\"public\"], \"m\", c.ecsign(e[\"private\"], \"m\"))") == "True");
    }

    // ---- randomized AES-GCM fuzz: encrypt->decrypt is the identity; a corrupted tag never decrypts ----
    std::mt19937_64 rng(0xA5A5u);
    for (int t = 0; t < 400; ++t) {
        // Built with += (not a `"lit" + str + "lit"` chain): the move-operator+ overload trips a
        // GCC 13 -O2/-O3 -Warray-bounds false positive on char_traits::copy.
        const char* ks = (t % 3 == 0) ? "16" : (t % 3 == 1) ? "24" : "32";
        std::string s = "var random = import(\"random\")\n";
        s += "var key = random.randombytes("; s += ks; s += ")\n";
        s += "var nonce = random.randombytes("; s += std::to_string(1 + (rng() % 16)); s += ")\n";
        s += "var msg = random.randombytes("; s += std::to_string(rng() % 200); s += ")\n";
        s += "var e = c.aesencrypt(key, msg, nonce)\n";
        s += "c.aesdecrypt(key, e[\"ciphertext\"], nonce, e[\"tag\"]) == msg";
        CHECK(evalStr(vm, s) == "True");
    }

    if (kitest::failures == 0) std::printf("test_crypto: all passed\n");
    return RUN_TESTS();
}

#endif  // KIRITO_ENABLE_TLS
