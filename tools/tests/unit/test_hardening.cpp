// Hardening: randomized and edge-case probing of the newer surface (builtins, modules, unpacking,
// inspect, discard, constructors). Every case must produce a defined value or a clean KiritoError —
// never a crash, hang, or UB. Designed to be run under ASan/UBSan as well as normally.
#include <random>
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string ev(KiritoVM& vm, const std::string& src) { return vm.stringify(vm.runSource(src)); }
static bool throws(KiritoVM& vm, const std::string& src) {
    try { vm.runSource(src); return false; } catch (const KiritoError&) { return true; }
}

int main() {
    // --- builtins on bad arguments throw cleanly (never crash) ---
    {
        KiritoVM vm;
        CHECK(throws(vm, "chr(-1)"));
        CHECK(throws(vm, "chr(1114112)"));      // one past U+10FFFF
        CHECK(throws(vm, "ord(\"\")"));
        CHECK(throws(vm, "ord(\"ab\")"));
        CHECK(throws(vm, "bin(\"x\")"));
        CHECK(throws(vm, "pow(2, 3, 0)"));
        CHECK(throws(vm, "isinstance(5, 42)"));
        CHECK(throws(vm, "divmod(1, 0)"));
        CHECK(throws(vm, "reversed(5)"));
        CHECK(throws(vm, "all(5)"));
        CHECK(throws(vm, "List(5)"));
        CHECK(throws(vm, "Dict([1, 2, 3])"));
        // boundary values that must NOT throw
        CHECK(ev(vm, "chr(0)") == std::string(1, '\0'));
        CHECK(ev(vm, "chr(1114111)") != "");    // U+10FFFF is valid
        CHECK(ev(vm, "bin(0)") == "0b0");
        CHECK(ev(vm, "String(divmod(0, 1))") == "[0, 0]");
    }

    // --- ord/chr round-trip across the whole BMP + a sample of astral code points ---
    {
        KiritoVM vm;
        std::mt19937 rng(2024);
        for (int i = 0; i < 500; ++i) {
            unsigned cp = static_cast<unsigned>(rng() % 0x110000);
            if (cp >= 0xD800 && cp <= 0xDFFF) continue;  // surrogates aren't scalar values
            vm.registerGlobal("_cp", vm.makeInt(static_cast<int64_t>(cp)));
            CHECK(ev(vm, "ord(chr(_cp))") == std::to_string(cp));
        }
    }

    // --- unpacking with random shapes: counts always checked, never out-of-bounds ---
    {
        KiritoVM vm;
        std::mt19937 rng(7);
        for (int i = 0; i < 200; ++i) {
            int listLen = static_cast<int>(rng() % 6);
            int targets = 2 + static_cast<int>(rng() % 4);  // >=2 targets => genuine unpacking
            std::string lst = "[";
            for (int j = 0; j < listLen; ++j) lst += (j ? "," : "") + std::to_string(j);
            lst += "]";
            std::string names;
            for (int j = 0; j < targets; ++j) names += (j ? ", " : "") + std::string("v") + std::to_string(j);
            std::string src = "var " + names + " = " + lst + "\n";
            bool shouldFail = (listLen != targets);  // exact count required (no star)
            CHECK(throws(vm, src) == shouldFail);
        }
    }

    // --- starred unpacking absorbs the right slice for random sizes ---
    {
        KiritoVM vm;
        std::mt19937 rng(11);
        for (int i = 0; i < 100; ++i) {
            int n = 2 + static_cast<int>(rng() % 8);
            std::string lst = "[";
            for (int j = 0; j < n; ++j) lst += (j ? "," : "") + std::to_string(j);
            lst += "]";
            // var first, *rest = lst  -> rest has n-1 elements
            std::string out = ev(vm, "var first, *rest = " + lst + "\nlen(rest)");
            CHECK(out == std::to_string(n - 1));
        }
    }

    // --- deep recursion in module functions is bounded, not a stack smash ---
    {
        KiritoVM vm;
        vm.setMaxCallDepth(150);
        CHECK(throws(vm, "var ft = import(\"functools\")\n"
                         "var rec = Function(n):\n    return rec(n + 1)\nrec(0)\n"));
        CHECK(ev(vm, "1 + 1") == "2");  // VM still usable
    }

    // --- collections under churn don't corrupt or crash ---
    {
        KiritoVM vm;
        CHECK(ev(vm,
            "var c = import(\"collections\")\nvar d = c.deque()\nvar i = 0\n"
            "while i < 1000:\n    d.append(i)\n    i = i + 1\n"
            "while len(d) > 500:\n    discard d.popleft()\n"
            "len(d)") == "500");
    }

    // --- base64 round-trips random byte arrays exactly ---
    {
        KiritoVM vm;
        std::mt19937 rng(99);
        for (int trial = 0; trial < 50; ++trial) {
            int n = static_cast<int>(rng() % 40);
            std::string lst = "[";
            for (int j = 0; j < n; ++j) lst += (j ? "," : "") + std::to_string(rng() % 256);
            lst += "]";
            std::string src = "var b = import(\"base64\")\nString(b.decode(b.encode(" + lst + "))) == String(" + lst + ")";
            CHECK(ev(vm, src) == "True");
        }
    }

    // --- heapq always pops in sorted order (random pushes) ---
    {
        KiritoVM vm;
        CHECK(ev(vm,
            "var h = import(\"heapq\")\nvar r = import(\"random\").Random(5)\nvar q = []\n"
            "var i = 0\nwhile i < 500:\n    h.heappush(q, r.randint(0, 100000))\n    i = i + 1\n"
            "var prev = -1\nvar ok = True\n"
            "while len(q) > 0:\n    var x = h.heappop(q)\n    if x < prev:\n        ok = False\n    prev = x\n"
            "ok") == "True");
    }

    // --- inspect never crashes on any value kind ---
    {
        KiritoVM vm;
        for (const char* expr : {"5", "3.14", "\"s\"", "True", "None", "[1]", "{1: 2}",
                                 "import(\"math\")", "Function(x): return x"}) {
            CHECK(ev(vm, std::string("len(inspect(") + expr + ")) >= 0") == "True");
        }
    }

    // --- discard evaluates exactly once and yields None as a statement ---
    {
        KiritoVM vm;
        CHECK(ev(vm,
            "var n = 0\nvar bump = Function():\n    n = n + 1\n    return n\n"
            "discard bump()\ndiscard bump()\nn") == "2");
    }

    return RUN_TESTS();
}
