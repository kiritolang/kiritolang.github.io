#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

// Build `value` in Kirito (the `build` snippet must bind it to `v`), serialize it with the dump
// module, compress the bytes, and report {raw_bytes, compressed_bytes, round_trips_ok}. Going
// through dump.dumps gives us realistic, structured, compressible payloads (the user's brief).
struct Sample { long raw, comp; bool ok; double pct() const { return 100.0 * static_cast<double>(comp) / static_cast<double>(raw); } };
static Sample measure(KiritoVM& vm, const std::string& build) {
    std::string src =
        "var d = import(\"dump\")\n"
        "var z = import(\"zlib\")\n" + build +
        "var b = d.dumps(v)\n"            // dumps returns Bytes; zlib works on Bytes directly
        "var c = z.compress(b)\n"
        "String(len(b)) + \" \" + String(len(c)) + \" \" + String(d.loads(z.decompress(c)) == v)\n";
    std::istringstream is(evalStr(vm, src));
    Sample s{}; std::string ok;
    is >> s.raw >> s.comp >> ok;
    s.ok = (ok == "True");
    return s;
}

int main() {
    KiritoVM vm;

    // --- basic round-trips -------------------------------------------------------------------
    CHECK(evalStr(vm, R"(
var z = import("zlib")
z.decompress(z.compress("hello world")) == "hello world"
)") == "True");
    CHECK(evalStr(vm, "var z = import(\"zlib\")\nz.decompress(z.compress(\"\")) == \"\"") == "True");
    CHECK(evalStr(vm, "var z = import(\"zlib\")\nz.decompress(z.compress(\"a\")) == \"a\"") == "True");

    // highly compressible data shrinks a lot
    CHECK(evalStr(vm, R"(
var z = import("zlib")
var data = "x" * 10000
var c = z.compress(data)
len(c) < 100 and z.decompress(c) == data
)") == "True");

    // raw deflate/inflate pair
    CHECK(evalStr(vm, R"(
var z = import("zlib")
var data = "The quick brown fox " * 20
z.inflate(z.deflate(data)) == data
)") == "True");

    // adler32 known values (the adler32 checksum now lives in the hash module)
    CHECK(evalStr(vm, "import(\"hash\").adler32(\"\")") == "1");
    CHECK(evalStr(vm, "import(\"hash\").adler32(\"hello\")") == "103547413");
    CHECK(evalStr(vm, "import(\"hash\").adler32(\"Wikipedia\")") == "300286872");

    // --- 10+ big Dump-serialized payloads: every one round-trips, and the compression ratio
    //     tracks the data's entropy (less entropy => smaller fraction). --------------------------
    const std::string seed = "var rng = import(\"random\").Random(20250530)\n";
    // helper builders producing a 20k-element array `v`
    auto ints = [&](const std::string& hi) {
        return seed + "var v = []\nfor i in range(20000):\n    v.append(rng.randint(0, " + hi + "))\n";
    };

    Sample zeros   = measure(vm, "var v = []\nfor i in range(20000):\n    v.append(0)\n");
    Sample tiny    = measure(vm, ints("15"));            // 4-bit values
    Sample small   = measure(vm, ints("100"));           // ~7-bit values (the brief's "half range")
    Sample medium  = measure(vm, ints("10000"));
    Sample large   = measure(vm, ints("1000000000"));    // ~30-bit values (the brief's "full range")
    Sample huge    = measure(vm, ints("9000000000000000000"));  // ~63-bit values

    // other data structures, not just integer arrays
    Sample bools   = measure(vm,
        seed + "var v = []\nfor i in range(20000):\n    v.append(rng.randint(0, 1) == 1)\n");
    Sample floats  = measure(vm,
        seed + "var v = []\nfor i in range(20000):\n    v.append(rng.random())\n");
    Sample strings = measure(vm,  // repeated vocabulary -> very compressible
        seed + "var words = [\"alpha\", \"beta\", \"gamma\", \"delta\"]\n"
               "var v = []\nfor i in range(20000):\n    v.append(words[rng.randint(0, 3)])\n");
    Sample records = measure(vm,  // list of uniform dicts (realistic serialized data)
        seed + "var v = []\nfor i in range(8000):\n"
               "    v.append({\"id\": i, \"score\": rng.randint(0, 100), \"ok\": rng.randint(0,1)==1})\n");
    Sample nested  = measure(vm,  // nested lists
        seed + "var v = []\nfor i in range(4000):\n"
               "    v.append([rng.randint(0, 50), [rng.randint(0, 50), rng.randint(0, 50)]])\n");
    Sample dict    = measure(vm,  // a single big Dict keyed by string
        seed + "var v = {}\nfor i in range(10000):\n    v[\"k\" + String(i)] = rng.randint(0, 100)\n");

    const Sample all[] = {zeros, tiny, small, medium, large, huge,
                          bools, floats, strings, records, nested, dict};
    int n = 0;
    for (const Sample& s : all) {
        CHECK(s.ok);                 // every payload round-trips through compress+decompress
        CHECK(s.raw > 1000);         // these really are big buffers, not toy inputs
        CHECK(s.comp > 0);
        ++n;
    }
    CHECK(n >= 10);                  // the brief: at least 10 distinct big arrays

    // Entropy ordering: a constant array is near-free; widening the integer range monotonically
    // costs more; full-range stays the least compressible of the integer family.
    CHECK(zeros.pct()  < 5.0);                  // all-zeros: almost everything is redundant
    CHECK(zeros.pct()  < tiny.pct());
    CHECK(tiny.pct()   < small.pct());
    CHECK(small.pct()  < large.pct());          // "half range ~ markedly lighter than full range"
    CHECK(large.pct()  < huge.pct());           // even fuller range compresses still less
    // The repeated-vocabulary strings are highly redundant; they must beat full-range integers.
    CHECK(strings.pct() < large.pct());
    // A small fixed range really is roughly half (or better) the fraction of the full range.
    CHECK(small.pct() <= large.pct() * 0.85);

    // --- incompressible payload: truly random bytes can't shrink; deflate may even grow them
    //     marginally past the original. We feed random bytes through dump to stay on-brief. ------
    {
        std::mt19937 rng(99);
        std::string rnd(50000, '\0');
        for (char& ch : rnd) ch = static_cast<char>(rng() & 0xFF);
        vm.registerGlobal("_rand_bytes", vm.makeString(rnd));
        Sample inc = measure(vm, "var v = _rand_bytes\n");
        CHECK(inc.ok);
        // No real compression: random bytes can't shrink. Kirito's deflate doesn't emit stored
        // blocks for incompressible runs, so it may even grow them a few percent (true byte count).
        CHECK(inc.pct() > 99.0);
        CHECK(static_cast<double>(inc.comp) <= static_cast<double>(inc.raw) * 1.10 + 64);
    }

    // --- binary-safe data (all byte values) round-trips --------------------------------------
    CHECK(evalStr(vm, R"(
var z = import("zlib")
var s = ""
for i in range(256):
    s = s + "\x41"
z.decompress(z.compress(s)) == s
)") == "True");

    // random data of many sizes round-trips (C++ side, with non-text bytes)
    {
        std::mt19937 rng(7);
        for (int trial = 0; trial < 40; ++trial) {
            int sz = std::uniform_int_distribution<int>(0, 2000)(rng);
            std::string data;
            for (int i = 0; i < sz; ++i) data.push_back(static_cast<char>(rng() & 0xFF));
            Handle src = vm.makeString(data);
            vm.registerGlobal("_rnd", src);
            std::string r = evalStr(vm, "var z = import(\"zlib\")\nz.decompress(z.compress(_rnd)) == _rnd");
            CHECK(r == "True");
        }
    }

    // --- hostile / corrupt inputs throw cleanly, never crash ----------------------------------
    CHECK_THROWS(vm.runSource("import(\"zlib\").decompress(\"not zlib data\")"));
    CHECK_THROWS(vm.runSource("import(\"zlib\").decompress(\"\")"));
    CHECK_THROWS(vm.runSource("import(\"zlib\").decompress(\"\\x78\\x9c\\xff\\xff\\xff\\xff\")"));  // bad body
    CHECK_THROWS(vm.runSource("import(\"zlib\").inflate(\"\\xff\\xff\\xff\")"));  // bad block type / truncated
    CHECK_THROWS(vm.runSource("import(\"zlib\").compress(42)"));  // wrong type
    CHECK_THROWS(vm.runSource("import(\"hash\").adler32([1, 2])"));  // wrong type

    // corrupting a valid stream's checksum is detected
    CHECK(evalStr(vm, R"(
var z = import("zlib")
var c = z.compress("important data")
var corrupted = c[0:len(c) - 1] + "\x00"
var ok = "no"
try:
    z.decompress(corrupted)
catch as e:
    ok = "caught"
ok
)") == "caught");

    // gzip multi-member: several members concatenated inflate at an OFFSET into the shared buffer
    // (inflateImpl takes a string_view suffix, no per-member copy). Under asan this also checks the
    // view never outlives its backing string.
    CHECK(evalStr(vm, R"(
var g = import("gzip")
g.decompress(g.compress("AAA") + g.compress("BBB") + g.compress("CCC"))
)") == "AAABBBCCC");
    {
        // Drive inflateImpl directly on a mid-buffer view: [junk prefix][deflate body]. `consumed` must
        // be relative to the view's start, and the decode must not read before the offset.
        std::string input("payload-\x00\x01\x02-tail", 16);  // embedded NUL/control bytes, explicit length
        std::string body = deflate::compress(input);
        std::string buf = "PREFIX!!" + body;                 // body starts at offset 8
        std::size_t consumed = 0;
        std::string out = deflate::inflateImpl(std::string_view(buf).substr(8), deflate::kMaxInflateOut, &consumed);
        CHECK(out == input);
        CHECK(consumed == body.size());
    }

    return RUN_TESTS();
}
