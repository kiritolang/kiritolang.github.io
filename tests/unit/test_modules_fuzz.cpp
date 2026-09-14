// Stress / fuzz / adversarial tests for the native stdlib modules after the Value-API rewrite.
// Goals: round-trip invariants (compress/serialize/hash/json), correct error behavior on bad input
// (wrong types, wrong arity, bad keywords), and survival of randomized/hostile inputs without
// crashing (every failure must be a catchable KiritoError, never a segfault/UB — run under ASan).
#include <memory>
#include <random>
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

int main() {
    // NOTE: the deterministic round-trip invariants (zlib/serialize/json/dump/hash), wrong-type
    // argument checks, and wrong-arity/unknown-keyword checks are a static golden — converted to
    // tests/scripts/unit_modules_fuzz.ki. Only the randomized fuzz loop below (an mt19937_64-driven
    // RNG with an in-C++ crash-count oracle) has no static golden and stays here.

    // ===== fuzz: random programs over the modules must never crash; only throw catchable errors =====
    std::mt19937_64 rng(20240601);
    const char* ops[] = {
        "import(\"hash\").sha256(%S)",
        "import(\"zlib\").decompress(%S)",
        "import(\"zlib\").compress(%S)",
        "import(\"json\").parse(%S)",
        "import(\"math\").sqrt(%N)",
        "import(\"math\").log(%N)",
        "import(\"math\").factorial(%N)",
        "import(\"matrix\").identity(%N)",
        "import(\"matrix\").zeros(%N, %N)",
        "import(\"net\").parseqs(%S)",
        "import(\"net\").unquote(%S)",
        "import(\"serialize\").loads(%S)",
        "import(\"time\").make(%N, %N, %N)",
    };
    // The fuzz deliberately triggers large/abusive allocations (huge matrices, string repeats,
    // decompression). Reusing a single VM for all 4000 iterations would let unreclaimed
    // intermediates pile up into gigabytes; recycle the VM every batch so peak memory stays small
    // and each batch is independent. (Each input is still meant to fail cleanly, never crash.)
    auto fuzzVm = std::make_unique<KiritoVM>();
    int crashes = 0, ran = 0;
    for (int i = 0; i < 4000; ++i) {
        if (i % 100 == 0) fuzzVm = std::make_unique<KiritoVM>();
        std::string tmpl = ops[rng() % (sizeof(ops) / sizeof(ops[0]))];
        std::string src;
        for (std::size_t k = 0; k < tmpl.size(); ++k) {
            if (tmpl[k] == '%' && k + 1 < tmpl.size()) {
                char kind = tmpl[++k];
                if (kind == 'S') {
                    std::string s = "\"";
                    int n = static_cast<int>(rng() % 12);
                    for (int c = 0; c < n; ++c) {
                        char ch = static_cast<char>(33 + rng() % 90);  // printable ASCII
                        if (ch == '"' || ch == '\\') s += '\\';
                        s += ch;
                    }
                    src += s + "\"";
                } else {  // %N : a random integer, sometimes large/negative/edge
                    int64_t v = static_cast<int64_t>(rng() % 5000);
                    if (rng() % 4 == 0) v = -v;
                    if (rng() % 8 == 0) v = static_cast<int64_t>(rng());  // huge
                    src += std::to_string(v);
                }
            } else {
                src += tmpl[k];
            }
        }
        ++ran;
        try {
            fuzzVm->runSource(src);
        } catch (const KiritoError&) {
            // expected for hostile / ill-typed input
        } catch (...) {
            ++crashes;  // any non-KiritoError escaping is a bug
        }
    }
    CHECK(ran == 4000);
    CHECK(crashes == 0);

    return RUN_TESTS();
}
