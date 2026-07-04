// Regression tests for the post-documentation audit round: the numeric `.compare` method, exact
// Float `==`, reflected `*` repetition, isinstance/typed-catch over built-in types, `inspect(String)`,
// container repr (quoted strings), round-trippable json floats, the float display inf-edge, and the
// Kirito-authored stdlib fixes (functools.reduce/itertools.accumulate None handling, statistics.mode
// on empty, enum order, base64/xml leniency). One behaviour per CHECK.
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string ev(KiritoVM& vm, const std::string& src) { return vm.stringify(vm.runSource(src)); }

int main() {
    KiritoVM vm;
    vm.installStandardLibrary();

    // --- numeric .compare(other, rel_tol=1e-9, abs_tol=0.0) -> Bool ------------------------------
    CHECK(ev(vm, "(0.1 + 0.2).compare(0.3)") == "True");      // tolerant: close enough
    CHECK(ev(vm, "0.1 + 0.2 == 0.3") == "False");             // exact: not bit-equal
    CHECK(ev(vm, "(5).compare(5)") == "True");                // Integer receiver
    CHECK(ev(vm, "(5).compare(6)") == "False");
    CHECK(ev(vm, "(5).compare(5.0)") == "True");              // cross-type
    CHECK(ev(vm, "(1.0).compare(1.0000001, abs_tol = 0.001)") == "True");   // abs_tol kwarg
    CHECK(ev(vm, "(1.0).compare(1.5, abs_tol = 0.001)") == "False");
    CHECK(ev(vm, "(1000000.0).compare(1000000.1, rel_tol = 1e-3)") == "True");  // rel_tol kwarg
    CHECK(ev(vm, "(1.0).compare(other = 1.0)") == "True");    // first arg by keyword
    CHECK(ev(vm, "inspect(1.0).find(\"compare(other, rel_tol = 1e-09, abs_tol = 0.0) -> Bool\") >= 0") == "True");
    CHECK(ev(vm, "inspect(5).find(\"compare(\") >= 0") == "True");
    CHECK_THROWS(vm.runSource("(1.0).compare(\"x\")"));       // non-numeric other

    // --- exact Float == agrees with ordering and hashing ----------------------------------------
    CHECK(ev(vm, "1.0 == 1.0 and 2.5 == 2.5") == "True");
    CHECK(ev(vm, "0.0 == -0.0") == "True");
    CHECK(ev(vm, "var n = 0.0 / 1.0 * 0.0\nn == n") == "True");   // 0.0 is its own equal
    CHECK(ev(vm, "not (0.1 + 0.2 == 0.3) and (0.1 + 0.2 > 0.3)") == "True");  // trichotomy holds
    // distinct-but-close floats are distinct Set keys now (no epsilon collision)
    CHECK(ev(vm, "len({0.1 + 0.2, 0.3})") == "2");

    // --- Float display: clean %.15g, but the DBL_MAX inf-edge round-trips -------------------------
    CHECK(ev(vm, "String(0.1 + 0.2)") == "0.3");              // display stays clean
    CHECK(ev(vm, "String(2.0)") == "2.0");
    CHECK(ev(vm, "var b = 1.7976931348623157e308\nFloat(String(b)) == b") == "True");  // no overflow to inf
    CHECK(ev(vm, "var b = 1.7976931348623157e308\nString(b) == \"1.7976931348623157e+308\"") == "True");

    // --- reflected `*` repetition: Integer on the left of a sequence -----------------------------
    CHECK(ev(vm, "3 * \"ab\"") == "ababab");
    CHECK(ev(vm, "\"ab\" * 3") == "ababab");                  // both orders
    CHECK(ev(vm, "(3 * Bytes([65, 66])).decode()") == "ABABAB");
    CHECK(ev(vm, "3 * [1, 2]") == "[1, 2, 1, 2, 1, 2]");
    CHECK(ev(vm, "0 * \"ab\"") == "");

    // --- isinstance over built-in type constructors (not just String names) ----------------------
    CHECK(ev(vm, "isinstance(1, Integer)") == "True");
    CHECK(ev(vm, "isinstance(1.5, Float)") == "True");
    CHECK(ev(vm, "isinstance(1, Float)") == "False");
    CHECK(ev(vm, "isinstance(\"x\", String)") == "True");
    CHECK(ev(vm, "isinstance(True, Bool)") == "True");
    CHECK(ev(vm, "isinstance([1], List)") == "True");
    CHECK(ev(vm, "isinstance({1}, Set)") == "True");
    CHECK(ev(vm, "isinstance({\"a\": 1}, Dict)") == "True");
    CHECK(ev(vm, "isinstance(Bytes([1]), Bytes)") == "True");
    CHECK(ev(vm, "isinstance(1, \"Integer\")") == "True");    // String name still works
    CHECK_THROWS(vm.runSource("isinstance(1, 42)"));          // 42 is not a type

    // --- typed catch over built-in types ---------------------------------------------------------
    CHECK(ev(vm, R"(
var got = ""
try:
    throw "boom"
catch String as e:
    got = "str:" + e
got
)") == "str:boom");
    CHECK(ev(vm, R"(
var got = 0
try:
    throw 42
catch Integer as e:
    got = e
got
)") == "42");
    // a non-matching typed catch does not swallow the throw
    CHECK_THROWS(vm.runSource("try:\n    throw 42\ncatch String as e:\n    pass"));

    // --- inspect(String) lists the String methods ------------------------------------------------
    CHECK(ev(vm, "inspect(\"x\").find(\"upper() -> String\") >= 0") == "True");
    CHECK(ev(vm, "inspect(\"x\").find(\"split(sep, maxsplit) -> List\") >= 0") == "True");
    CHECK(ev(vm, "inspect(\"x\").find(\"String value:\") >= 0") == "True");

    // --- container repr: strings quoted when nested ----------------------------------------------
    CHECK(ev(vm, "String([\"a\", \"b\"])") == "['a', 'b']");
    CHECK(ev(vm, "String([\"\"])") == "['']");                // empty string distinguishable from []
    CHECK(ev(vm, "String([])") == "[]");
    CHECK(ev(vm, "String({\"k\": \"v\"})") == "{'k': 'v'}");
    CHECK(ev(vm, "String([1, \"two\", 3.0, True, None])") == "[1, 'two', 3.0, True, None]");
    CHECK(ev(vm, "String([[\"nested\"]])") == "[['nested']]");
    CHECK(ev(vm, "String([\"it's\"])") == "[\"it's\"]");      // quote selection
    CHECK(ev(vm, "String([\"tab\\there\"])") == "['tab\\there']");  // escapes
    CHECK(ev(vm, "String(\"hello\")") == "hello");            // bare string still unquoted

    // --- json: floats round-trip exactly now that == is exact ------------------------------------
    CHECK(ev(vm, "var j = import(\"json\")\nj.loads(j.dumps(0.1 + 0.2)) == 0.1 + 0.2") == "True");
    CHECK(ev(vm, "var j = import(\"json\")\nj.dumps(0.1)") == "0.1");   // still clean for ordinary values
    CHECK(ev(vm, "var j = import(\"json\")\nj.loads(j.dumps([1.5, 2.25, 0.1 + 0.2]))[2] == 0.1 + 0.2") == "True");

    // --- tensor: exact whole == but NaN never equal; sqrt/// /% of bad input throw ---------------
    CHECK(ev(vm, "var T = import(\"tensor\")\nT.Tensor([1, 2]) == T.Tensor([1, 2])") == "True");
    // sqrt of a negative now THROWS a clear domain error (was a silent NaN); the NaN-never-equal
    // whole-== check uses a literal NaN element instead.
    CHECK_THROWS(vm.runSource("var T = import(\"tensor\")\ndiscard T.Tensor([-1.0]).sqrt()"));
    CHECK(ev(vm, "var T = import(\"tensor\")\nvar n = import(\"math\").nan\nT.Tensor([n]) == T.Tensor([n])") == "False");
    CHECK_THROWS(vm.runSource("var T = import(\"tensor\")\ndiscard T.Tensor([1.0, 2.0]) // 0"));
    CHECK_THROWS(vm.runSource("var T = import(\"tensor\")\ndiscard T.Tensor([1.0, 2.0]) % 0"));

    // --- bytes: ascii encode error reports the code point in hex (U+00E9, not decimal) -----------
    CHECK(ev(vm, R"(
var msg = ""
try:
    discard "café".encode("ascii")
catch as e:
    msg = e
msg.find("U+00E9") >= 0
)") == "True");

    // --- math.perm: 1-arg form perm(n) == n! -----------------------------------------------------
    CHECK(ev(vm, "import(\"math\").perm(5)") == "120");
    CHECK(ev(vm, "import(\"math\").perm(5, 2)") == "20");
    CHECK(ev(vm, "import(\"math\").perm(n = 5)") == "120");

    // --- random.seed accepts the keyword `a` (matches inspect) -----------------------------------
    CHECK(ev(vm, R"(
var rng = import("random").Random()
rng.seed(a = 42)
var x = rng.randint(0, 1000000)
rng.seed(a = 42)
x == rng.randint(0, 1000000)
)") == "True");

    // --- functools.reduce / itertools.accumulate: None is a value, not "empty" -------------------
    CHECK(ev(vm, "import(\"functools\").reduce(Function(a, b): return None, [1, 2, 3])") == "None");
    CHECK(ev(vm, "import(\"functools\").reduce(Function(a, b): return a + b, [1, 2, 3, 4])") == "10");
    CHECK_THROWS(vm.runSource("discard import(\"functools\").reduce(Function(a, b): return a + b, [])"));
    CHECK(ev(vm, "import(\"itertools\").accumulate([1, 2, 3])") == "[1, 3, 6]");

    // --- statistics.mode on empty throws ---------------------------------------------------------
    CHECK_THROWS(vm.runSource("discard import(\"statistics\").mode([])"));
    CHECK(ev(vm, "import(\"statistics\").mode([1, 2, 2, 3])") == "2");

    // --- enum preserves definition order ---------------------------------------------------------
    CHECK(ev(vm, "import(\"enum\").Enum([\"RED\", \"GREEN\", \"BLUE\"]).names()") == "['RED', 'GREEN', 'BLUE']");
    CHECK(ev(vm, "import(\"enum\").Enum([\"RED\", \"GREEN\", \"BLUE\"]).values()") == "[0, 1, 2]");

    // --- base64.decode reports a base64 error (not a raw dict "key not found") --------------------
    CHECK(ev(vm, R"(
var msg = ""
try:
    discard import("base64").decode("###")
catch as e:
    msg = e
msg.find("base64") >= 0
)") == "True");

    // --- xml: malformed numeric entities are tolerated verbatim (lenient); valid ones decode ------
    CHECK(ev(vm, "import(\"xml\").fromstring(\"<a>&#xZZ;</a>\").text") == "&#xZZ;");
    CHECK(ev(vm, "import(\"xml\").fromstring(\"<a>&#;</a>\").text") == "&#;");
    CHECK(ev(vm, "import(\"xml\").fromstring(\"<a>&#999999999;</a>\").text") == "&#999999999;");
    CHECK(ev(vm, "import(\"xml\").fromstring(\"<a>&#65;&#x42;</a>\").text") == "AB");

    // --- ALL native numeric types: `==` is EXACT, tolerance only via `.compare` -------------------
    // Complex (scalar)
    CHECK(ev(vm, "var C = import(\"complex\")\nC.of(1.0, 0) == C.of(1.0000000001, 0)") == "False");
    CHECK(ev(vm, "var C = import(\"complex\")\nC.of(1.0, 0).compare(C.of(1.0000000001, 0))") == "True");
    CHECK(ev(vm, "var C = import(\"complex\")\nC.of(1, 2) == C.of(1, 2)") == "True");
    CHECK(ev(vm, "var C = import(\"complex\")\nC.of(0, 1).compare(C.of(0, 1.0000000001), abs_tol = 1e-6)") == "True");
    CHECK(ev(vm, "inspect(import(\"complex\").of(1.0, 1.0)).find(\"compare(\") >= 0") == "True");
    // real Matrix
    CHECK(ev(vm, "var M = import(\"matrix\")\nM.Matrix([[1.0]]) == M.Matrix([[1.0000000001]])") == "False");
    CHECK(ev(vm, "var M = import(\"matrix\")\nM.Matrix([[1.0]]).compare(M.Matrix([[1.0000000001]]))") == "True");
    CHECK(ev(vm, "var M = import(\"matrix\")\nM.identity(2) == M.identity(2)") == "True");
    CHECK(ev(vm, "inspect(import(\"matrix\").identity(2)).find(\"compare(\") >= 0") == "True");
    // Tensor (whole ==)
    CHECK(ev(vm, "var T = import(\"tensor\")\nT.Tensor([1.0]) == T.Tensor([1.0000000001])") == "False");
    CHECK(ev(vm, "var T = import(\"tensor\")\nT.Tensor([1.0]).compare(T.Tensor([1.0000000001]))") == "True");
    CHECK(ev(vm, "var T = import(\"tensor\")\nT.Tensor([1, 2, 3]) == T.Tensor([1, 2, 3])") == "True");
    CHECK(ev(vm, "inspect(import(\"tensor\").eye(2)).find(\"compare(\") >= 0") == "True");
    // ComplexMatrix
    CHECK(ev(vm, "var C = import(\"complex\")\nC.Matrix([[C.of(1,0)]]) == C.Matrix([[C.of(1.0000000001,0)]])") == "False");
    CHECK(ev(vm, "var C = import(\"complex\")\nC.Matrix([[C.of(1,0)]]).compare(C.Matrix([[C.of(1.0000000001,0)]]))") == "True");

    return RUN_TESTS();
}
