#include <cstdio>
// Adversarial / edge-case probing of the EXISTING libraries (json, strings, collections, io,
// serialize, math, random). Every probe must either produce a defined result or throw a clean
// KiritoError — never crash, hang, or corrupt memory. Run under ASan for memory safety too.
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// Returns true if the source ran without crashing (success OR a clean KiritoError both count).
static bool survives(KiritoVM& vm, const std::string& src) {
    try {
        vm.runSource(src);
        return true;
    } catch (const KiritoError&) {
        return true;
    } catch (...) {
        return false;  // any non-KiritoError escape is a failure
    }
}

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

int main() {
    KiritoVM vm;

    // ---------------- JSON ----------------
    // malformed inputs must throw, not crash
    const char* badJson[] = {
        "{", "}", "[", "]", "[1,", "{\"a\":}", "{:1}", "[1 2]", "tru", "nul",
        "\"unterminated", "{\"a\":1,}", "[,]", "1.2.3", "{\"a\"\"b\"}", "[[[[[[[[[[",
        "\\\\\\\\", "\x01\x02\x03", "{\"a\": \"\\uZZZZ\"}", "01234",
    };
    for (const char* j : badJson) {
        std::string src = "import(\"json\").parse(\"";
        for (char c : std::string(j)) {  // escape for embedding in a Kirito string literal
            if (c == '"') src += "\\\"";
            else if (c == '\\') src += "\\\\";
            else if (c == '\n') src += "\\n";
            else if (static_cast<unsigned char>(c) < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\x%02x", static_cast<unsigned char>(c)); src += b; }
            else src.push_back(c);
        }
        src += "\")\n";
        CHECK(survives(vm, src));
    }
    // valid JSON still works after all the bad inputs
    CHECK(evalStr(vm, "import(\"json\").parse(\"[1, 2, 3]\")[1]") == "2");
    // deeply nested JSON (stack-bounded)
    CHECK(survives(vm, "import(\"json\").parse(\"" + std::string(200, '[') + std::string(200, ']') + "\")\n"));
    // huge numbers
    CHECK(survives(vm, "import(\"json\").parse(\"99999999999999999999999999999\")\n"));
    // stringify of non-serializable throws
    CHECK(survives(vm, "import(\"json\").stringify(Function(x): return x)\n"));

    // ---------------- Strings ----------------
    // slicing extremes and weird steps never crash
    CHECK(evalStr(vm, "\"hello\"[1000:2000]") == "");
    CHECK(evalStr(vm, "\"hello\"[-1000:1000]") == "hello");
    CHECK(evalStr(vm, "\"hello\"[::1]") == "hello");
    CHECK(evalStr(vm, "\"\"[0:0]") == "");
    CHECK(survives(vm, "\"abc\"[0:3:0]\n"));  // zero step -> clean error
    // out-of-range indexing throws
    CHECK(survives(vm, "\"\"[0]\n"));
    CHECK(survives(vm, "\"a\"[99]\n"));
    // huge repetition is bounded by memory but doesn't crash logically
    CHECK(evalStr(vm, "len(\"ab\" * 0)") == "0");
    CHECK(survives(vm, "\"x\" * -5\n"));  // negative repeat
    // format with mismatched braces / indices
    CHECK(survives(vm, "\"{}\".format()\n"));        // too few args
    CHECK(survives(vm, "\"{5}\".format(1)\n"));       // index out of range
    CHECK(survives(vm, "\"{\".format(1)\n"));         // unmatched brace
    CHECK(evalStr(vm, "\"{} {} {}\".format(1, 2, 3)") == "1 2 3");
    // unicode boundaries
    CHECK(evalStr(vm, "len(\"\xc3\xa9\xc3\xa9\xc3\xa9\")") == "3");  // 3 é's
    CHECK(evalStr(vm, "\"\xc3\xa9\xc3\xa9\"[1]") == "\xc3\xa9");
    // split/join edge cases
    CHECK(evalStr(vm, "len(\"\".split(\",\"))") == "1");      // empty string -> [""]
    CHECK(evalStr(vm, "\",\".join([])") == "");
    CHECK(survives(vm, "\"a\".split(\"\")\n"));               // empty separator -> clean error

    // ---------------- Collections ----------------
    // index / key errors throw
    CHECK(survives(vm, "[][0]\n"));
    CHECK(survives(vm, "[1, 2, 3][-100]\n"));
    CHECK(survives(vm, "{}[\"missing\"]\n"));
    CHECK(survives(vm, "var d = {}\nd[[1]] = 2\n"));          // unhashable key
    CHECK(survives(vm, "var s = {}\n{[1], [2]}\n"));          // unhashable set element
    // pop from empty
    CHECK(survives(vm, "[].pop()\n"));
    CHECK(survives(vm, "var d = {}\nd.pop(\"x\")\n"));
    // remove non-existent
    CHECK(survives(vm, "var a = [1, 2]\na.remove(99)\n"));
    // huge list operations stay correct
    CHECK(evalStr(vm, R"(
var a = []
for i in range(10000):
    a.append(i)
a.reverse()
a[0]
)") == "9999");
    // self-referential containers: str/len/in don't hang
    CHECK(survives(vm, "var a = []\na.append(a)\nString(a)\n"));
    CHECK(survives(vm, "var a = []\na.append(a)\nlen(a)\n"));
    CHECK(evalStr(vm, "var a = []\na.append(a)\na[0] == a") == "True");

    // ---------------- Serialize / dump ----------------
    CHECK(survives(vm, "import(\"serialize\").loads(\"garbage\")\n"));
    CHECK(survives(vm, "import(\"serialize\").loads(\"\")\n"));
    CHECK(survives(vm, "import(\"dump\").loads(\"garbage\")\n"));
    CHECK(survives(vm, "import(\"dump\").loads(\"KDMP\\x01\\xff\\xff\\xff\\xff\")\n"));  // huge count
    // a self-ref structure dumped and reloaded stays self-ref
    CHECK(evalStr(vm, R"(
var d = import("dump")
var a = []
a.append(a)
var r = d.loads(d.dumps(a))
r[0] == r
)") == "True");

    // ---------------- Math ----------------
    CHECK(survives(vm, "import(\"math\").sqrt(-1)\n"));        // domain error, cleanly caught (no crash)
    CHECK(survives(vm, "import(\"math\").log(0)\n"));
    CHECK(survives(vm, "import(\"math\").factorial(-1)\n"));   // clean error
    CHECK(survives(vm, "import(\"math\").factorial(100000)\n"));  // huge, but bounded
    // sqrt(-1) now THROWS a clear math domain error rather than silently returning NaN.
    {
        bool thrown = false;
        try { vm.runSource("import(\"math\").sqrt(-1.0)"); }
        catch (const KiritoError&) { thrown = true; }
        CHECK(thrown);
    }

    // ---------------- Random ----------------
    CHECK(survives(vm, "import(\"random\").Random(1).randint(10, 1)\n"));  // empty range
    CHECK(survives(vm, "import(\"random\").Random(1).choice([])\n"));      // empty sequence
    CHECK(survives(vm, "import(\"random\").Random(1).sample([1, 2], 5)\n")); // k too large

    // ---------------- after all the abuse, the VM is still fully functional ----------------
    CHECK(evalStr(vm, "1 + 2 * 3") == "7");
    CHECK(evalStr(vm, "import(\"json\").parse(\"{\\\"ok\\\": true}\")[\"ok\"]") == "True");
    CHECK(evalStr(vm, "sorted([3, 1, 2])") == "[1, 2, 3]");

    return RUN_TESTS();
}
