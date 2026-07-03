#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

int main() {
    KiritoVM vm;

    // reproducibility: same seed -> same sequence (no global RNG; each object is independent)
    CHECK(evalStr(vm, R"(
var random = import("random")
var a = random.Random(123)
var b = random.Random(123)
var same = True
var i = 0
while i < 20:
    if a.randint(1, 1000000) != b.randint(1, 1000000):
        same = False
    i = i + 1
same
)") == "True");

    // randint stays within the inclusive range
    CHECK(evalStr(vm, R"(
var rng = import("random").Random(7)
var ok = True
var i = 0
while i < 1000:
    var x = rng.randint(1, 6)
    if x < 1 or x > 6:
        ok = False
    i = i + 1
ok
)") == "True");

    // random() in [0, 1)
    CHECK(evalStr(vm, R"(
var rng = import("random").Random(1)
var ok = True
var i = 0
while i < 1000:
    var x = rng.random()
    if x < 0.0 or x >= 1.0:
        ok = False
    i = i + 1
ok
)") == "True");

    // choice returns a member; shuffle preserves length; sample gives the right count
    CHECK(evalStr(vm, R"(
var rng = import("random").Random(99)
var data = [10, 20, 30, 40, 50]
rng.choice(data) in data
)") == "True");
    CHECK(evalStr(vm, R"(
var rng = import("random").Random(99)
var data = [1, 2, 3, 4, 5, 6, 7, 8]
rng.shuffle(data)
len(data)
)") == "8");
    CHECK(evalStr(vm, R"(
var rng = import("random").Random(99)
len(rng.sample([1, 2, 3, 4, 5], 3))
)") == "3");

    // uniform stays in range
    CHECK(evalStr(vm, R"(
var rng = import("random").Random(5)
var x = rng.uniform(2.0, 3.0)
x >= 2.0 and x <= 3.0
)") == "True");

    // empty-sequence and bad-range errors
    CHECK_THROWS(vm.runSource("import(\"random\").Random(1).choice([])\n"));
    CHECK_THROWS(vm.runSource("import(\"random\").Random(1).randint(5, 1)\n"));

    // choices(population, k): a List of k elements sampled WITH replacement.
    CHECK(evalStr(vm, R"(
var rng = import("random").Random(7)
len(rng.choices([1, 2, 3], 5))
)") == "5");
    CHECK(evalStr(vm, "len(import(\"random\").Random(7).choices([1, 2, 3]))") == "1");     // k defaults to 1
    CHECK(evalStr(vm, "import(\"random\").Random(7).choices([1, 2, 3], 0)") == "[]");       // k = 0
    CHECK(evalStr(vm, "import(\"random\").Random(7).choices(population = [9], k = 3)") == "[9, 9, 9]");  // kwargs
    // WITH replacement: 20 draws from 2 values must repeat (sample, WITHOUT replacement, would throw).
    CHECK(evalStr(vm, R"(
var rng = import("random").Random(7)
len(Set(rng.choices([0, 1], 20))) < 20
)") == "True");
    // determinism: same seed -> identical result.
    CHECK(evalStr(vm, R"(
import("random").Random(3).choices([1, 2, 3, 4], 6) == import("random").Random(3).choices([1, 2, 3, 4], 6)
)") == "True");
    // choice is the k=1 case unwrapped: under one seed, choice(seq) == choices(seq, 1)[0], and stays scalar.
    CHECK(evalStr(vm, R"(
import("random").Random(88).choice([5, 6, 7]) == import("random").Random(88).choices([5, 6, 7], 1)[0]
)") == "True");
    CHECK(evalStr(vm, "import(\"random\").Random(1).choice([42])") == "42");   // scalar, not [42]
    // error surface
    CHECK_THROWS(vm.runSource("import(\"random\").Random(1).choices([])\n"));            // empty population
    CHECK_THROWS(vm.runSource("import(\"random\").Random(1).choices([1, 2], -1)\n"));    // negative k
    CHECK_THROWS(vm.runSource("import(\"random\").Random(1).choices(5, 2)\n"));          // non-iterable
    // inspect advertises it
    CHECK(evalStr(vm, "\"choices(population\" in inspect(import(\"random\").Random(1))") == "True");

    return RUN_TESTS();
}
