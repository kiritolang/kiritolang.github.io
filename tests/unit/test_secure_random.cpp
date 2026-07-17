// OS-CSPRNG secure random: random.randombytes / randomhex / randomurlsafe / randombelow. Covers
// structural correctness (lengths, alphabets), statistical sanity (distinctness, randombelow range +
// coverage + rough uniformity), and adversarial/bad-input rejection. The kernel entropy source is
// non-deterministic, so the checks are properties (never fixed vectors).
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}
static int64_t evalInt(KiritoVM& vm, const std::string& src) {
    return static_cast<const IntVal&>(vm.arena().deref(vm.runSource(src))).value();
}

int main() {
    KiritoVM vm;
    vm.installStandardLibrary();

    // ---- fillRandom low-level: fills every byte, succeeds ----
    {
        unsigned char buf[64];
        std::memset(buf, 0, sizeof(buf));
        CHECK(randcompat::fillRandom(buf, sizeof(buf)));
        CHECK(randcompat::fillRandom(nullptr, 0));  // n==0 is a no-op success
        // Two draws of a big buffer must differ (a stuck/zero source would fail this).
        unsigned char a[256], b[256];
        CHECK(randcompat::fillRandom(a, sizeof(a)) && randcompat::fillRandom(b, sizeof(b)));
        CHECK(std::memcmp(a, b, sizeof(a)) != 0);
    }

    // ---- hasentropy ----
    CHECK(evalStr(vm, "import(\"random\").hasentropy()") == "True");  // available on the test host

    // ---- randombytes ----
    CHECK(evalStr(vm, "type(import(\"random\").randombytes())") == "Bytes");
    CHECK(evalInt(vm, "len(import(\"random\").randombytes())") == 32);          // default 32
    CHECK(evalInt(vm, "len(import(\"random\").randombytes(16))") == 16);
    CHECK(evalInt(vm, "len(import(\"random\").randombytes(0))") == 0);
    CHECK(evalInt(vm, "len(import(\"random\").randombytes(1000))") == 1000);

    // ---- randomhex: 2n lowercase hex chars ----
    CHECK(evalInt(vm, "len(import(\"random\").randomhex())") == 64);            // 32 bytes -> 64 hex
    CHECK(evalInt(vm, "len(import(\"random\").randomhex(20))") == 40);
    {
        std::string h = evalStr(vm, "import(\"random\").randomhex(64)");
        CHECK(h.size() == 128);
        for (char c : h) CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }

    // ---- randomurlsafe: base64url alphabet, no padding ----
    {
        std::string u = evalStr(vm, "import(\"random\").randomurlsafe(48)");
        CHECK(!u.empty());
        for (char c : u)
            CHECK((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_');
        CHECK(u.find('=') == std::string::npos);   // never padded
        CHECK(u.find('+') == std::string::npos && u.find('/') == std::string::npos);  // url-safe alphabet
    }

    // ---- distinctness: thousands of tokens, no collision (128-bit -> birthday-negligible) ----
    {
        std::set<std::string> seen;
        for (int i = 0; i < 5000; ++i) seen.insert(evalStr(vm, "import(\"random\").randomhex(16)"));
        CHECK(seen.size() == 5000);
    }

    // ---- randombelow: range, edge, coverage, rough uniformity ----
    CHECK(evalInt(vm, "import(\"random\").randombelow(1)") == 0);   // only value in [0,1)
    {
        const int K = 16, N = 32000;
        std::vector<int> counts(K, 0);
        bool inRange = true;
        for (int i = 0; i < N; ++i) {
            int64_t v = evalInt(vm, "import(\"random\").randombelow(16)");
            if (v < 0 || v >= K) inRange = false;
            else ++counts[static_cast<std::size_t>(v)];
        }
        CHECK(inRange);
        bool coveredAll = true;
        double chi2 = 0.0, expected = static_cast<double>(N) / K;
        for (int c : counts) {
            if (c == 0) coveredAll = false;
            double d = c - expected;
            chi2 += d * d / expected;
        }
        CHECK(coveredAll);
        // 15 dof: the 99.9% critical value is ~37.7; a real CSPRNG sits well under a loose 50 bound.
        CHECK(chi2 < 50.0);
    }
    // A large modulus still stays in range (exercises the rejection loop over the full 64-bit draw).
    for (int i = 0; i < 1000; ++i) {
        int64_t v = evalInt(vm, "import(\"random\").randombelow(1000000007)");
        CHECK(v >= 0 && v < 1000000007);
    }

    // ---- adversarial / bad input ----
    CHECK_THROWS(evalStr(vm, "import(\"random\").randombytes(-1)"));
    CHECK_THROWS(evalStr(vm, "import(\"random\").randomhex(-5)"));
    CHECK_THROWS(evalStr(vm, "import(\"random\").randomurlsafe(-1)"));
    CHECK_THROWS(evalStr(vm, "import(\"random\").randombytes(999999999999)"));  // over the size cap
    CHECK_THROWS(evalStr(vm, "import(\"random\").randombelow(0)"));
    CHECK_THROWS(evalStr(vm, "import(\"random\").randombelow(-10)"));

    if (kitest::failures == 0) std::printf("test_secure_random: all passed\n");
    return RUN_TESTS();
}
