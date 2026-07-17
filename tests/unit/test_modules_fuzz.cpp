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

static std::string run(KiritoVM& vm, const std::string& src) { return vm.stringify(vm.runSource(src)); }
static bool throws(KiritoVM& vm, const std::string& src) {
    try { vm.runSource(src); return false; } catch (const KiritoError&) { return true; }
}

int main() {
    KiritoVM vm;

    // ===== round-trip invariants =====
    // zlib: decompress(compress(x)) == x
    CHECK(run(vm, "var z = import(\"zlib\")\nvar s = \"the quick brown fox jumps over 123!\"\n"
                  "z.decompress(z.compress(s)) == s") == "True");
    CHECK(run(vm, "var z = import(\"zlib\")\nz.inflate(z.deflate(\"\")) == \"\"") == "True");
    CHECK(run(vm, "var z = import(\"zlib\")\nvar s = \"a\" * 1000\nz.decompress(z.compress(s)) == s") == "True");
    // serialize / json / dump round-trips
    CHECK(run(vm, "var s = import(\"serialize\")\ns.loads(s.dumps({\"a\": [1, 2, 3], \"b\": None}))") == "{'a': [1, 2, 3], 'b': None}");
    CHECK(run(vm, "var j = import(\"json\")\nj.loads(j.dumps([1, 2.5, \"x\", True, None]))") == "[1, 2.5, 'x', True, None]");
    CHECK(run(vm, "var d = import(\"dump\")\nd.loads(d.dumps({\"k\": [1, [2, [3]]]}))") == "{'k': [1, [2, [3]]]}");
    // hash: known digests
    CHECK(run(vm, "import(\"hash\").sha256(\"abc\")") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(run(vm, "import(\"hash\").md5(\"\")") == "d41d8cd98f00b204e9800998ecf8427e");

    // ===== wrong-type arguments throw cleanly (no crash) =====
    CHECK(throws(vm, "import(\"hash\").md5(123)"));
    CHECK(throws(vm, "import(\"zlib\").compress([1, 2, 3])"));
    CHECK(throws(vm, "import(\"math\").sqrt(\"x\")"));
    CHECK(throws(vm, "import(\"json\").parse(42)"));
    CHECK(throws(vm, "import(\"matrix\").zeros(\"a\", 2)"));
    CHECK(throws(vm, "import(\"time\").make(\"y\", 1, 1)"));
    CHECK(throws(vm, "import(\"net\").urlencode(\"not a dict\")"));
    CHECK(throws(vm, "import(\"random\").Random(\"seed\")"));
    CHECK(throws(vm, "import(\"zlib\").decompress(\"not valid deflate data!!!\")"));

    // ===== wrong arity / unknown keyword (the signature layer) =====
    CHECK(throws(vm, "import(\"hash\").sha1()"));                       // missing arg
    CHECK(throws(vm, "import(\"hash\").sha1(\"a\", \"b\")"));            // too many
    CHECK(throws(vm, "import(\"math\").sqrt(x=1, bogus=2)"));            // unknown keyword
    CHECK(throws(vm, "import(\"matrix\").identity()"));                 // missing required

    // keyword args still work on the rewritten modules
    CHECK(run(vm, "import(\"math\").log(8, base=2)") == "3.0");
    CHECK(run(vm, "import(\"json\").dumps([1, 2], indent=2) != \"\"") == "True");
    CHECK(run(vm, "import(\"net\").urlsplit(\"http://h:7/p?q=1\")[\"port\"]") == "7");

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
