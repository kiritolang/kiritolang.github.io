// Extensive tests for the `semver` frozen module: parsing/validation, semantic-versioning
// precedence (including the spec's worked example chain), and the node-semver range grammar
// (^, ~, comparators, x-ranges, hyphen ranges, AND/OR), plus maxsatisfying / sort / inc / diff and
// prerelease gating. This is the versioning core that `kpm` relies on to resolve `repo@<constraint>`.
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// Evaluate a one-expression program against a fresh `import("semver") as s`. Each call is isolated.
static std::string ev(const std::string& expr) {
    KiritoVM vm;
    return vm.stringify(vm.runSource("var s = import(\"semver\")\n" + expr + "\n"));
}
// satisfies(version, range) as a "True"/"False" string.
static std::string sat(const std::string& v, const std::string& r) {
    return ev("s.satisfies(\"" + v + "\", \"" + r + "\")");
}
static std::string cmp(const std::string& a, const std::string& b) {
    return ev("s.compare(\"" + a + "\", \"" + b + "\")");
}

int main() {
    // ---------- parse / valid ----------
    CHECK(ev("s.valid(\"1.2.3\")") == "1.2.3");
    CHECK(ev("s.valid(\"v2.0.0\")") == "2.0.0");          // leading v stripped
    CHECK(ev("s.valid(\"  =1.0.0 \")") == "1.0.0");       // leading '=' + whitespace
    CHECK(ev("s.valid(\"1.2.3-alpha.1+build.7\")") == "1.2.3-alpha.1+build.7");
    CHECK(ev("s.valid(\"1.2\")") == "None");              // need 3 components
    CHECK(ev("s.valid(\"1.2.3.4\")") == "None");
    CHECK(ev("s.valid(\"1.2.x\")") == "None");
    CHECK(ev("s.valid(\"\")") == "None");
    CHECK(ev("s.valid(\"abc\")") == "None");
    CHECK(ev("s.major(\"3.4.5\")") == "3");
    CHECK(ev("s.minor(\"3.4.5\")") == "4");
    CHECK(ev("s.patch(\"3.4.5\")") == "5");
    CHECK(ev("s.prerelease(\"1.0.0-rc.2\")") == "['rc', '2']");
    CHECK(ev("s.prerelease(\"1.0.0\")") == "None");
    CHECK_THROWS(KiritoVM().runSource("import(\"semver\").parse(\"nope\")\n"));

    // ---------- precedence: numeric, not lexical ----------
    CHECK(cmp("1.0.0", "1.0.0") == "0");
    CHECK(cmp("1.0.0", "1.0.1") == "-1");
    CHECK(cmp("1.10.0", "1.9.0") == "1");      // 10 > 9 numerically
    CHECK(cmp("2.0.0", "1.999.999") == "1");
    CHECK(ev("s.gt(\"1.0.10\", \"1.0.9\")") == "True");
    CHECK(ev("s.lt(\"1.0.9\", \"1.0.10\")") == "True");
    CHECK(ev("s.eq(\"1.0.0+build1\", \"1.0.0+build2\")") == "True");   // build metadata ignored
    CHECK(ev("s.neq(\"1.0.0\", \"1.0.1\")") == "True");
    CHECK(ev("s.gte(\"1.0.0\", \"1.0.0\")") == "True");
    CHECK(ev("s.lte(\"1.0.0\", \"1.0.0\")") == "True");

    // ---------- prerelease precedence (semver.org spec example) ----------
    // 1.0.0-alpha < 1.0.0-alpha.1 < 1.0.0-alpha.beta < 1.0.0-beta < 1.0.0-beta.2 <
    //   1.0.0-beta.11 < 1.0.0-rc.1 < 1.0.0
    CHECK(cmp("1.0.0-alpha", "1.0.0-alpha.1") == "-1");
    CHECK(cmp("1.0.0-alpha.1", "1.0.0-alpha.beta") == "-1");
    CHECK(cmp("1.0.0-alpha.beta", "1.0.0-beta") == "-1");
    CHECK(cmp("1.0.0-beta", "1.0.0-beta.2") == "-1");
    CHECK(cmp("1.0.0-beta.2", "1.0.0-beta.11") == "-1");   // 11 > 2 numerically
    CHECK(cmp("1.0.0-beta.11", "1.0.0-rc.1") == "-1");
    CHECK(cmp("1.0.0-rc.1", "1.0.0") == "-1");             // a prerelease is < its release
    CHECK(cmp("1.0.0-1", "1.0.0-alpha") == "-1");          // numeric id < alphanumeric id

    // ---------- caret ^ ----------
    CHECK(sat("1.2.3", "^1.2.3") == "True");
    CHECK(sat("1.9.0", "^1.2.3") == "True");
    CHECK(sat("1.2.2", "^1.2.3") == "False");              // below the floor
    CHECK(sat("2.0.0", "^1.2.3") == "False");              // hits the next-major ceiling
    CHECK(sat("0.2.9", "^0.2.3") == "True");               // 0.x: ceiling is 0.3.0
    CHECK(sat("0.3.0", "^0.2.3") == "False");
    CHECK(sat("0.0.3", "^0.0.3") == "True");               // 0.0.x: ceiling is 0.0.4
    CHECK(sat("0.0.4", "^0.0.3") == "False");
    CHECK(sat("1.5.0", "^1") == "True");                   // ^1 == >=1.0.0 <2.0.0
    CHECK(sat("2.0.0", "^1") == "False");
    CHECK(sat("1.3.0", "^1.2") == "True");                 // ^1.2 == >=1.2.0 <2.0.0

    // ---------- tilde ~ ----------
    CHECK(sat("1.2.9", "~1.2.3") == "True");
    CHECK(sat("1.3.0", "~1.2.3") == "False");
    CHECK(sat("1.2.0", "~1.2") == "True");                 // ~1.2 == >=1.2.0 <1.3.0
    CHECK(sat("1.3.0", "~1.2") == "False");
    CHECK(sat("1.9.9", "~1") == "True");                   // ~1 == >=1.0.0 <2.0.0
    CHECK(sat("2.0.0", "~1") == "False");

    // ---------- x-ranges, *, exact ----------
    CHECK(sat("1.2.7", "1.2.x") == "True");
    CHECK(sat("1.3.0", "1.2.x") == "False");
    CHECK(sat("1.9.9", "1.x") == "True");
    CHECK(sat("2.0.0", "1.x") == "False");
    CHECK(sat("9.9.9", "*") == "True");
    CHECK(sat("0.0.0", "") == "True");
    CHECK(sat("1.2.3", "1.2.3") == "True");                // bare = exact
    CHECK(sat("1.2.4", "1.2.3") == "False");

    // ---------- explicit comparators + AND ----------
    CHECK(sat("1.5.0", ">=1.0.0 <2.0.0") == "True");
    CHECK(sat("2.0.0", ">=1.0.0 <2.0.0") == "False");
    CHECK(sat("1.0.0", ">=1.0.0 <2.0.0") == "True");
    CHECK(sat("1.0.0", ">1.0.0") == "False");
    CHECK(sat("1.0.1", ">1.0.0") == "True");
    CHECK(sat("1.0.0", "<=1.0.0") == "True");

    // ---------- OR (||) and hyphen ranges ----------
    CHECK(sat("1.5.0", "^1.0.0 || ^3.0.0") == "True");
    CHECK(sat("3.5.0", "^1.0.0 || ^3.0.0") == "True");
    CHECK(sat("2.5.0", "^1.0.0 || ^3.0.0") == "False");
    CHECK(sat("1.5.0", "1.0.0 - 2.0.0") == "True");
    CHECK(sat("2.0.0", "1.0.0 - 2.0.0") == "True");        // inclusive upper
    CHECK(sat("2.0.1", "1.0.0 - 2.0.0") == "False");
    CHECK(sat("1.2.5", "1.2.3 - 1.3") == "True");          // partial upper -> <1.4.0

    // ---------- prerelease gating (node-semver default: excluded unless pinned) ----------
    CHECK(sat("2.0.0-beta", "^1.0.0") == "False");
    CHECK(sat("1.2.3-beta", "^1.0.0") == "False");         // prerelease not pinned by range
    CHECK(sat("1.2.3-beta.2", ">=1.2.3-beta.1") == "True");// same M.m.p with prerelease -> allowed
    CHECK(sat("1.2.4-beta", ">=1.2.3-beta.1") == "False"); // different patch, prerelease excluded

    // ---------- maxsatisfying / minsatisfying ----------
    CHECK(ev("s.maxsatisfying([\"1.0.0\", \"1.2.0\", \"1.9.9\", \"2.0.0\"], \"^1.0.0\")") == "1.9.9");
    CHECK(ev("s.minsatisfying([\"1.0.0\", \"1.2.0\", \"1.9.9\"], \"^1.0.0\")") == "1.0.0");
    CHECK(ev("s.maxsatisfying([\"1.0.0\", \"2.0.0\"], \"^3.0.0\")") == "None");
    // returns the ORIGINAL tag string (so kpm can use it directly as a git ref), invalids skipped
    CHECK(ev("s.maxsatisfying([\"v1.0.0\", \"v1.5.0\", \"not-a-version\"], \"*\")") == "v1.5.0");

    // ---------- sort / rsort (numeric ordering; invalids dropped) ----------
    CHECK(ev("s.sort([\"1.10.0\", \"1.2.0\", \"1.1.0\"])") == "['1.1.0', '1.2.0', '1.10.0']");
    CHECK(ev("s.rsort([\"1.1.0\", \"1.10.0\", \"1.2.0\"])") == "['1.10.0', '1.2.0', '1.1.0']");
    CHECK(ev("s.sort([\"2.0.0\", \"1.0.0-rc.1\", \"1.0.0\"])") == "['1.0.0-rc.1', '1.0.0', '2.0.0']");
    CHECK(ev("s.sort([\"1.0.0\", \"garbage\", \"0.9.0\"])") == "['0.9.0', '1.0.0']");

    // ---------- diff / inc ----------
    CHECK(ev("s.diff(\"1.2.3\", \"2.0.0\")") == "major");
    CHECK(ev("s.diff(\"1.2.3\", \"1.3.0\")") == "minor");
    CHECK(ev("s.diff(\"1.2.3\", \"1.2.4\")") == "patch");
    CHECK(ev("s.diff(\"1.2.3-a\", \"1.2.3-b\")") == "prerelease");
    CHECK(ev("s.diff(\"1.2.3\", \"1.2.3\")") == "None");
    CHECK(ev("s.inc(\"1.2.3\", \"major\")") == "2.0.0");
    CHECK(ev("s.inc(\"1.2.3\", \"minor\")") == "1.3.0");
    CHECK(ev("s.inc(\"1.2.3\", \"patch\")") == "1.2.4");
    CHECK(ev("s.inc(\"1.2.3-rc.1\", \"patch\")") == "1.2.4");   // prerelease dropped
    CHECK_THROWS(KiritoVM().runSource("import(\"semver\").inc(\"1.0.0\", \"bogus\")\n"));

    // ---------- validrange ----------
    CHECK(ev("s.validrange(\"^1.2.3\")") == "True");
    CHECK(ev("s.validrange(\">=1.0.0 <2.0.0\")") == "True");
    CHECK(ev("s.validrange(\"*\")") == "True");

    // ---------- adversarial: malformed input is rejected cleanly, never crashes ----------
    CHECK(ev("s.valid(\"1\")") == "None");                 // too few components
    CHECK(ev("s.valid(\"1.2.3-\")") == "None");            // empty prerelease identifier
    CHECK(ev("s.valid(\"1..3\")") == "None");              // empty numeric component
    CHECK(ev("s.valid(\"...\")") == "None");
    CHECK(ev("s.valid(\"v\")") == "None");
    CHECK(ev("s.valid(\"1.2.x\")") == "None");
    CHECK(ev("s.validrange(\"not a range\")") == "False"); // garbage comparator -> not a range
    CHECK(ev("s.validrange(\">=abc\")") == "False");        // non-numeric comparator operand
    CHECK(ev("s.validrange(\">=\")") == "True");            // lenient: a bare op widens to >=0.0.0
    CHECK(ev("s.maxsatisfying([\"x\", \"y\", \"\"], \"*\")") == "None");  // all invalid -> None
    CHECK(ev("s.sort([\"x\", \"y\"])") == "[]");           // invalids dropped -> empty
    CHECK_THROWS(KiritoVM().runSource("import(\"semver\").compare(\"1.0.0\", \"garbage\")\n"));
    // An UNPARSEABLE range satisfies nothing (node-semver: satisfies->false, maxsatisfying->null) —
    // never leaks the internal Integer()-conversion error to the caller (kpm passes user constraints here).
    CHECK(ev("s.satisfies(\"1.0.0\", \">=abc\")") == "False");
    CHECK(ev("s.maxsatisfying([\"1.0.0\", \"2.0.0\"], \"latest\")") == "None");

    // ---------- fuzz: random version-ish strings are total (no crash) and self-consistent ----------
    {
        KiritoVM vm;
        std::string out = vm.stringify(vm.runSource(R"KI(
var s = import("semver")
var rnd = import("random").Random(12345)
var alpha = "0123456789.-+abx"
var mk = Function():
    var t = ""
    var i = 0
    var n = rnd.randint(1, 12)
    while i < n:
        t = t + alpha[rnd.randint(0, len(alpha) - 1)]
        i = i + 1
    return t
var k = 0
while k < 3000:
    var v = mk()
    var ok = s.valid(v)              # total: a String or None, never throws
    if ok != None:
        assert s.compare(v, v) == 0  # reflexive
        assert s.eq(v, ok)           # the cleaned form is equal
        # a valid release matches "*"; a prerelease is gated out (so it must HAVE a prerelease)
        assert s.satisfies(ok, "*") or len(s.parse(ok)["prerelease"]) > 0
    var vr = s.validrange(v)         # total: Bool, never throws
    assert vr == True or vr == False
    k = k + 1
"fuzz-ok"
)KI"));
        CHECK(out == "fuzz-ok");
    }

    // ---------- fuzz: ordering invariants on a random pool (sortedness, idempotence, max/min) -----
    {
        KiritoVM vm;
        std::string out = vm.stringify(vm.runSource(R"KI(
var s = import("semver")
var rnd = import("random").Random(2024)
var pool = []
var i = 0
while i < 80:
    pool.append(String(rnd.randint(0, 3)) + "." + String(rnd.randint(0, 6)) + "." + String(rnd.randint(0, 9)))
    i = i + 1
var asc = s.sort(pool)
var j = 1
while j < len(asc):
    assert s.compare(asc[j - 1], asc[j]) <= 0     # non-decreasing by precedence
    j = j + 1
assert s.sort(asc) == asc                          # idempotent
var desc = s.rsort(pool)
var m = 1
while m < len(desc):
    assert s.compare(desc[m - 1], desc[m]) >= 0    # non-increasing by precedence
    m = m + 1
var best = s.maxsatisfying(pool, "*")
var least = s.minsatisfying(pool, "*")
var n = 0
while n < len(pool):
    assert s.compare(pool[n], best) <= 0           # nothing exceeds the max
    assert s.compare(pool[n], least) >= 0          # nothing is below the min
    n = n + 1
"order-ok"
)KI"));
        CHECK(out == "order-ok");
    }

    return RUN_TESTS();
}
