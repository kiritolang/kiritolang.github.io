#include <random>
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

int main() {
    KiritoVM vm;

    // basic in-place sort
    CHECK(evalStr(vm, "var a = [3, 1, 4, 1, 5, 9, 2, 6]\na.sort()\na") == "[1, 1, 2, 3, 4, 5, 6, 9]");
    CHECK(evalStr(vm, "var a = []\na.sort()\na") == "[]");
    CHECK(evalStr(vm, "var a = [42]\na.sort()\na") == "[42]");
    CHECK(evalStr(vm, "var a = [\"banana\", \"apple\", \"cherry\"]\na.sort()\na") ==
          "['apple', 'banana', 'cherry']");
    CHECK(evalStr(vm, "var a = [3.5, 1.2, 2.8]\na.sort()\na") == "[1.2, 2.8, 3.5]");

    // reverse
    CHECK(evalStr(vm, "var a = [1, 2, 3]\na.sort(None, True)\na") == "[3, 2, 1]");

    // key function
    CHECK(evalStr(vm, R"(
var a = ["bb", "a", "ccc", "dd", "e"]
a.sort(Function(w): return len(w))
a
)") == "['a', 'e', 'bb', 'dd', 'ccc']");

    // key + reverse
    CHECK(evalStr(vm, R"(
var a = ["a", "ccc", "bb"]
a.sort(Function(w): return len(w), True)
a
)") == "['ccc', 'bb', 'a']");

    // STABILITY: equal keys preserve original relative order
    CHECK(evalStr(vm, R"(
var pairs = [[1, "a"], [2, "b"], [1, "c"], [2, "d"], [1, "e"]]
pairs.sort(Function(p): return p[0])
pairs
)") == "[[1, 'a'], [1, 'c'], [1, 'e'], [2, 'b'], [2, 'd']]");

    // stability under reverse: equal keys still keep original order
    CHECK(evalStr(vm, R"(
var pairs = [[1, "a"], [1, "b"], [1, "c"]]
pairs.sort(Function(p): return p[0], True)
pairs
)") == "[[1, 'a'], [1, 'b'], [1, 'c']]");

    // sorted() builtin: new list, original unchanged
    CHECK(evalStr(vm, R"(
var a = [3, 1, 2]
var b = sorted(a)
String(a) + " / " + String(b)
)") == "[3, 1, 2] / [1, 2, 3]");
    CHECK(evalStr(vm, "sorted([3, 1, 2], None, True)") == "[3, 2, 1]");
    CHECK(evalStr(vm, "sorted([\"xx\", \"y\", \"zzz\"], Function(s): return len(s))") == "['y', 'xx', 'zzz']");

    // sorting a large random list yields a correctly ordered result (verified by scanning)
    CHECK(evalStr(vm, R"(
var rng = import("random").Random(123)
var a = []
for i in range(2000):
    a.append(rng.randint(0, 1000))
a.sort()
var ok = True
for i in range(1, len(a)):
    if a[i] < a[i - 1]:
        ok = False
ok and len(a) == 2000
)") == "True");

    // a large stable-sort correctness check: sort (value, index) pairs by value; for equal values
    // the original index order must be preserved.
    CHECK(evalStr(vm, R"(
var rng = import("random").Random(7)
var items = []
for i in range(1000):
    items.append([rng.randint(0, 10), i])   # many ties -> stability matters
items.sort(Function(p): return p[0])
var ok = True
for i in range(1, len(items)):
    if items[i][0] == items[i - 1][0] and items[i][1] < items[i - 1][1]:
        ok = False   # equal key but index went backwards -> not stable
ok
)") == "True");

    // mixed int/float sorts numerically
    CHECK(evalStr(vm, "var a = [3, 1.5, 2, 0.5]\na.sort()\na") == "[0.5, 1.5, 2, 3]");

    // hostile: genuinely un-orderable elements (mixing types) throw cleanly
    CHECK_THROWS(vm.runSource("var a = [1, \"two\", 3]\na.sort()\n"));

    // Lists ARE orderable (lexicographic): sorting and list-keyed sort both work.
    CHECK(evalStr(vm, "var a = [[2], [1], [1, 0]]\na.sort()\na") == "[[1], [1, 0], [2]]");
    CHECK(evalStr(vm, "var a = [3, 1, 2]\na.sort(Function(x): return [x % 2, x])\na") == "[2, 1, 3]");
    // but ordering a List against a non-List still throws
    CHECK_THROWS(vm.runSource("var a = [[1], 2]\na.sort()\n"));

    return RUN_TESTS();
}
