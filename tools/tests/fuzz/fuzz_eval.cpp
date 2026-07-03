// Stability fuzzing: feed many random programs and random byte soup to the VM and assert that
// it always either succeeds or throws a KiritoError — never crashes, aborts, or throws something
// unexpected. Run under AddressSanitizer to also catch memory errors.
#include <random>
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::mt19937 rng(12345);
static int randi(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); }

// Generate a mostly-valid random expression, so the evaluator is exercised deeply.
static std::string genExpr(int depth) {
    static const char* names[] = {"x", "y", "z", "undefined1", "a"};
    static const char* binops[] = {" + ", " - ", " * ", " // ", " % ", " < ", " == ", " and ", " or "};
    if (depth <= 0 || randi(0, 3) == 0) {
        switch (randi(0, 5)) {
            case 0: return std::to_string(randi(-100, 100));
            case 1: return std::to_string(randi(-100, 100)) + "." + std::to_string(randi(0, 999));
            case 2: return std::string("\"") + (randi(0, 1) ? "txt" : "") + "\"";
            case 3: return randi(0, 1) ? "True" : "False";
            case 4: return names[randi(0, 4)];
            default: return "None";
        }
    }
    switch (randi(0, 5)) {
        case 0: return "(" + genExpr(depth - 1) + binops[randi(0, 8)] + genExpr(depth - 1) + ")";
        case 1: return "-" + genExpr(depth - 1);
        case 2: return "not " + genExpr(depth - 1);
        case 3: return "len(" + genExpr(depth - 1) + ")";
        case 4: return "[" + genExpr(depth - 1) + ", " + genExpr(depth - 1) + "]";
        default: return "(" + genExpr(depth - 1) + ")";
    }
}

// Run source; OK if it succeeds or throws a KiritoError. Anything else is a failure.
static void runSafely(const std::string& src) {
    KiritoVM vm;
    vm.setGcThreshold(64);    // exercise the collector aggressively
    vm.setMaxCallDepth(200);  // stress the recursion guard cheaply
    try {
        vm.runSource(src);
    } catch (const KiritoError&) {
        // expected for invalid programs
    } catch (const std::exception& e) {
        std::printf("FAIL unexpected std::exception: %s\nsrc:\n%s\n", e.what(), src.c_str());
        ++kitest::failures;
    } catch (...) {
        std::printf("FAIL unexpected non-standard exception\nsrc:\n%s\n", src.c_str());
        ++kitest::failures;
    }
}

int main() {
    // 1) Random valid-ish expression programs.
    for (int i = 0; i < 3000; ++i) {
        std::string prog = "var x = " + std::to_string(randi(-50, 50)) +
                           "\nvar y = " + genExpr(2) + "\n" + genExpr(3) + "\n";
        runSafely(prog);
    }

    // 2) Random truncations of a known-good program (parser robustness).
    const std::string good =
        "var f = Function(n):\n    if n < 2:\n        return n\n    return f(n-1)+f(n-2)\nf(10)\n";
    for (int i = 0; i < 1000; ++i) runSafely(good.substr(0, randi(0, static_cast<int>(good.size()))));

    // 3) Random byte soup (lexer robustness, including non-UTF-8 and control bytes).
    for (int i = 0; i < 3000; ++i) {
        std::string s;
        int n = randi(0, 40);
        for (int j = 0; j < n; ++j) s.push_back(static_cast<char>(randi(1, 255)));
        runSafely(s);
    }

    // 4) Feature/library soup: assemble random fragments exercising collections, strings, classes,
    //    control flow, and every stdlib module with random (sometimes invalid) arguments.
    const char* frags[] = {
        "var L = [%d, %d, %d]\n", "L.append(%d)\n", "L.sort()\n", "L.insert(%d, %d)\n",
        "var D = {%d: %d, %d: %d}\n", "D.get(%d, 0)\n", "var S = {%d, %d, %d}\n",
        "var t = \"abc%d\"\nt[%d]\nt[%d:%d]\nt.upper()\nt.split(\"b\")\n",
        "f\"value {%d} and {%d}\"\n",
        "range(%d)\nsum(range(%d))\nsorted([%d, %d, %d])\n",
        "map(Function(x): return x + %d, [%d, %d])\n",
        "import(\"math\").sqrt(%d)\nimport(\"math\").factorial(%d %% 10)\n",
        "var rng = import(\"random\").Random(%d)\nrng.randint(%d, %d)\n",
        "import(\"json\").parse(\"[%d, %d]\")\n",
        "var m = import(\"matrix\").Matrix([[%d, %d], [%d, %d]])\nm.determinant()\n",
        "var sr = import(\"serialize\")\nsr.loads(sr.dumps([%d, [%d], %d]))\n",
        "class C:\n    var _init_ = Function(self, v):\n        self.v = v\nC(%d).v\n",
        "try:\n    throw %d\ncatch as e:\n    e + %d\n",
        "var i = 0\nvar s = 0\nwhile i < %d %% 20:\n    s = s + i\n    i = i + 1\ns\n",
        "%d if %d > %d else %d\n",  // not valid Kirito (no ternary) -> exercises parse errors too
    };
    // Fill a fragment's %d with random integers and %% with a literal % (no printf, to keep the
    // format string out of -Wformat-nonliteral's way).
    auto fill = [&](const char* t) {
        std::string out;
        for (const char* p = t; *p; ++p) {
            if (p[0] == '%' && p[1] == 'd') { out += std::to_string(randi(-50, 50)); ++p; }
            else if (p[0] == '%' && p[1] == '%') { out += '%'; ++p; }
            else out += *p;
        }
        return out;
    };
    for (int i = 0; i < 4000; ++i) {
        std::string prog;
        int lines = randi(1, 4);
        for (int j = 0; j < lines; ++j)
            prog += fill(frags[randi(0, static_cast<int>(std::size(frags)) - 1)]);
        runSafely(prog);
    }

    // 5) Class / operator-overload soup: complete programs exercising operator dispatch, private
    //    members, _str_, _getitem_/_setitem_, _call_, inheritance — the class attack surface.
    //    Run with a small call-depth and aggressive GC to stress those paths too.
    const char* classProgs[] = {
        // operator overloads + _str_
        "class V:\n    var _init_ = Function(self, x):\n        self.x = x\n"
        "    var _add_ = Function(self, o):\n        return V(self.x + o.x)\n"
        "    var _str_ = Function(self):\n        return String(self.x)\n"
        "String(V(%d) + V(%d))\n",
        // private member access from inside (ok) and a method chain
        "class C:\n    var _init_ = Function(self):\n        self._n = %d\n"
        "    var get = Function(self):\n        return self._n\nC().get()\n",
        // private member access from OUTSIDE (must throw, not crash)
        "class C:\n    var _init_ = Function(self):\n        self._n = %d\nC()._n\n",
        // _getitem_/_setitem_ variadic
        "class G:\n    var _init_ = Function(self):\n        self.d = {}\n"
        "    var _setitem_ = Function(self, a, b, v):\n        self.d[String(a) + String(b)] = v\n"
        "    var _getitem_ = Function(self, a, b):\n        return self.d[String(a) + String(b)]\n"
        "var g = G()\ng[%d, %d] = %d\ng[%d, %d]\n",
        // _call_ + inheritance
        "class Base:\n    var _init_ = Function(self, n):\n        self.n = n\n"
        "    var _call_ = Function(self, x):\n        return x + self.n\n"
        "class Sub(Base):\n    var twice = Function(self, x):\n        return self(x) * 2\n"
        "Sub(%d).twice(%d)\n",
        // instance stored in a container then GC-churned
        "class P:\n    var _init_ = Function(self, v):\n        self.v = v\n"
        "var lst = []\nvar i = 0\nwhile i < %d %% 50:\n    lst.append(P(i))\n    i = i + 1\n"
        "len(lst)\n",
        // a class with a _str_ that references itself (cycle-ish)
        "class Node:\n    var _init_ = Function(self):\n        self.next = None\n"
        "    var _str_ = Function(self):\n        return \"node\"\n"
        "var a = Node()\na.next = a\nString(a)\n",
    };
    for (int i = 0; i < 3000; ++i) {
        std::string prog = fill(classProgs[randi(0, static_cast<int>(std::size(classProgs)) - 1)]);
        runSafely(prog);
    }

    if (kitest::failures == 0) std::printf("fuzz: 14000 inputs, no crashes\n");
    return RUN_TESTS();
}
