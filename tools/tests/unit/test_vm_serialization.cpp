// VM serialization stress test: the "save state in one VM, load it into a FRESH VM, keep running"
// scenario (the same cross-VM transfer the `parallel` module relies on). For every serializable
// shape we dump it in VM A, carry only the raw blob across to a brand-new VM B, reconstruct it there,
// and then both (a) assert the reconstructed graph is bit-for-bit intact and (b) RUN new Kirito code
// in B against it — proving the loaded VM is fully functional, with no data lost. Covered through BOTH
// codecs: `dump` (binary) and `serialize` (text). Adversarial shapes too: cycles, shared-reference
// (DAG) identity, deep nesting, NUL/Unicode strings, every native value type, and the non-
// serializable resources that must throw rather than silently corrupt.
#include <memory>
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// The cross-VM transfer medium: a dump blob is Bytes, a serialize blob is a String. Pull its raw
// octets out of the producing VM — nothing else crosses (no shared objects), exactly like a Queue.
static std::string blobBytes(KiritoVM& from, Handle h) {
    Object& o = from.arena().deref(h);
    if (auto* b = dynamic_cast<BytesVal*>(&o)) return b->data;
    if (o.kind() == ValueKind::String) return static_cast<const StrVal&>(o).value();
    throw std::runtime_error("blob is neither Bytes nor String");
}

// Build `graph` in a fresh VM A, dump it with `codec` ("dump" or "serialize"), spin up a brand-new
// VM B, run `prelude` there (e.g. class defs the deserializer needs), inject the blob as the global
// `blob`, load it into the global `x`, then return ev(B, check) — the assertion run IN the loaded VM.
static std::string crossVM(const std::string& codec, const std::string& buildGraph,
                           const std::string& prelude, const std::string& check) {
    std::string rawBlob;
    {
        KiritoVM a;
        a.installStandardLibrary();
        // A needs the same prelude (class defs) to BUILD the graph; build + dump in one run.
        std::string mk = (prelude.empty() ? std::string() : prelude + "\n")
            + "var c = import(\"" + codec + "\")\n" + buildGraph + "\nc.dumps(graph)\n";
        Handle blob = a.runSource(mk);
        rawBlob = blobBytes(a, blob);
    }  // VM A is destroyed — only rawBlob survives, like a serialized message
    KiritoVM b;
    b.installStandardLibrary();
    // The blob is a global (persists across runs); everything that must share a module scope — the
    // prelude (class defs the deserializer needs), the load into `x`, and the assertion — runs in ONE
    // runSource, whose last expression is the result. (Each runSource gets a fresh module scope.)
    Handle injected = (codec == "dump")
        ? b.alloc(std::make_unique<BytesVal>(rawBlob))
        : b.makeString(rawBlob);
    b.registerGlobal("blob", injected);
    std::string script = (prelude.empty() ? std::string() : prelude + "\n")
        + "var c = import(\"" + codec + "\")\nvar x = c.loads(blob)\n" + check + "\n";
    return b.stringify(b.runSource(script));
}

// Run the same scenario through BOTH codecs and require both to agree with `expect`.
#define BOTH(build, prelude, check, expect)                              \
    do {                                                                 \
        CHECK(crossVM("dump", build, prelude, check) == (expect));       \
        CHECK(crossVM("serialize", build, prelude, check) == (expect));  \
    } while (0)

int main() {
    // --- scalars & strings survive A -> blob -> fresh B --------------------------------------------
    BOTH("var graph = 42", "", "x", "42");
    BOTH("var graph = -9223372036854775808", "", "x", "-9223372036854775808");
    // round-trips to the EXACT double (value, not 15-sig-fig display) — proves no precision lost
    BOTH("var graph = 3.141592653589793", "", "String(x == 3.141592653589793)", "True");
    BOTH("var graph = 0.1 + 0.2", "", "String(x == 0.1 + 0.2)", "True");
    BOTH("var graph = True", "", "x", "True");
    BOTH("var graph = None", "", "x", "None");
    BOTH("var graph = \"héllo\\x00wörld\"", "", "String(len(x))", "11");   // NUL + Unicode, byte-exact
    BOTH("var graph = \"\"", "", "String(len(x))", "0");

    // --- nested containers: structure and every leaf preserved ------------------------------------
    BOTH("var graph = [1, [2, [3, [4, 5]]], {\"k\": [6, 7]}]", "", "x[1][1][1][1]", "5");
    BOTH("var graph = [1, [2, [3, [4, 5]]], {\"k\": [6, 7]}]", "", "x[2][\"k\"][0]", "6");
    BOTH("var graph = {\"a\": {1, 2, 3}, \"b\": {\"c\": 4}}", "", "String(x[\"a\"].contains(2)) + String(x[\"b\"][\"c\"])", "True4");

    // --- shared references (a DAG): the same object reached twice stays ONE object after load ------
    BOTH("var shared = [1, 2]\nvar graph = [shared, shared]", "",
         "var ok = id(x[0]) == id(x[1])\nx[0].append(99)\nString(ok) + \" \" + String(x[1][2])", "True 99");

    // --- cycles: a self-referential structure round-trips and stays cyclic ------------------------
    BOTH("var graph = [1]\ngraph.append(graph)", "", "String(id(x[1]) == id(x)) + \" \" + String(x[1][1][1][0])", "True 1");
    BOTH("var graph = {\"self\": None, \"v\": 7}\ngraph[\"self\"] = graph", "",
         "String(id(x[\"self\"]) == id(x)) + \" \" + String(x[\"self\"][\"self\"][\"v\"])", "True 7");

    // --- user classes: reconstructed in B by name; methods RUN on the loaded instance -------------
    const std::string P =
        "class Point:\n"
        "    var _init_ = Function(self, x, y):\n"
        "        self.x = x\n"
        "        self.y = y\n"
        "    var norm2 = Function(self):\n"
        "        return self.x * self.x + self.y * self.y\n";
    // attributes preserved AND a method works on the loaded instance (functional, not just structural)
    BOTH("var graph = Point(3, 4)", P, "String(x.x) + \",\" + String(x.y) + \"=\" + String(x.norm2())", "3,4=25");
    // a list of instances, with one shared instance (identity preserved across the load)
    BOTH("var p = Point(1, 1)\nvar graph = [p, Point(2, 2), p]", P,
         "String(id(x[0]) == id(x[2])) + \" \" + String(x[1].norm2())", "True 8");

    // --- _getstate_ / _setstate_ custom protocol --------------------------------------------------
    const std::string Cnt =
        "class Counter:\n"
        "    var _init_ = Function(self, n):\n"
        "        self._n = n\n"                       // private attr
        "    var get = Function(self):\n"
        "        return self._n\n"
        "    var _getstate_ = Function(self):\n"
        "        return [self._n]\n"
        "    var _setstate_ = Function(self, st):\n"
        "        self._n = st[0]\n";
    BOTH("var graph = Counter(123)", Cnt, "String(x.get())", "123");

    // --- native value types: B must import the owning module FIRST so its deserializer is registered
    // before c.loads runs (the prelude runs before the load in B — the real-world constraint).
    BOTH("var graph = M.Matrix([[1, 2], [3, 4]])", "var M = import(\"matrix\")",
         "String(x[1, 1]) + \" \" + String(x.determinant())", "4.0 -2.0");
    BOTH("var graph = C.of(2, -3)", "var C = import(\"complex\")",
         "String(x.re) + \",\" + String(x.im)", "2.0,-3.0");
    BOTH("var graph = C.Matrix([[C.of(1, 1), C.of(2, 0)]])", "var C = import(\"complex\")",
         "String(x[0, 0].im)", "1.0");
    BOTH("var graph = T.Tensor([[1.0, 2.0], [3.0, 4.0]])", "var T = import(\"tensor\")",
         "String(x[0, 1]) + \",\" + String(x[1, 1]) + \" \" + String(x.shape())", "2.0,4.0 [2, 2]");
    BOTH("var graph = tm.make(2024, 6, 24, 12, 0, 0)", "var tm = import(\"time\")",
         "String(x.year) + \"-\" + String(x.month) + \"-\" + String(x.day)", "2024-6-24");
    // Random: the reconstructed generator reproduces the EXACT stream (a checkpoint)
    CHECK(crossVM("dump",
        "var r = rnd.Random(42)\ndiscard r.randint(0, 1000000)\nvar graph = r", "var rnd = import(\"random\")",
        "var saved = x.randint(0, 1000000)\nvar fresh = rnd.Random(42)\ndiscard fresh.randint(0, 1000000)\nString(saved == fresh.randint(0, 1000000))") == "True");
    // Bytes round-trip byte-exactly (builtin type, no import needed)
    BOTH("var graph = Bytes([0, 127, 255, 1, 2])", "", "String(x[2]) + \",\" + String(len(x))", "255,5");

    // --- a big mixed structure (stress): everything together, deep + wide --------------------------
    BOTH(
        "var graph = []\n"
        "var i = 0\n"
        "while i < 500:\n"
        "    graph.append({\"i\": i, \"sq\": i * i, \"tags\": [i, i + 1, i + 2], \"set\": {i % 7}})\n"
        "    i = i + 1\n",
        "", "String(len(x)) + \" \" + String(x[499][\"sq\"]) + \" \" + String(x[250][\"tags\"][2])",
        "500 249001 252");

    // --- non-serializable resources must THROW (never silently lose/corrupt data) ------------------
    {
        KiritoVM a; a.installStandardLibrary();
        CHECK_THROWS(a.runSource("var d = import(\"dump\")\nvar io = import(\"io\")\ndiscard d.dumps(io.BytesIO())"));
        CHECK_THROWS(a.runSource("var d = import(\"dump\")\nvar n = import(\"net\")\ndiscard d.dumps(n.Socket())"));
        CHECK_THROWS(a.runSource("var d = import(\"dump\")\nvar re = import(\"regex\")\ndiscard d.dumps(re.compile(\"a+\"))"));
        // and the text codec agrees
        CHECK_THROWS(a.runSource("var s = import(\"serialize\")\nvar io = import(\"io\")\ndiscard s.dumps(io.open(\"/tmp/x\", \"w\"))"));
    }

    // --- loading a corrupt/truncated blob fails cleanly (no crash, no partial state) ---------------
    {
        KiritoVM b; b.installStandardLibrary();
        CHECK_THROWS(b.runSource("var d = import(\"dump\")\ndiscard d.loads(Bytes([0, 1, 2, 3]))"));
        CHECK_THROWS(b.runSource("var s = import(\"serialize\")\ndiscard s.loads(\"not a valid graph !!!\")"));
    }

    return RUN_TESTS();
}
