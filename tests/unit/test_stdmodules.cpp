// The Kirito-authored standard-library modules (itertools, functools, collections, statistics,
// string, textwrap, base64, csv, heapq, bisect, copy, enum) and the new constructor builtins.
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string run(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

int main() {
    // --- List/Set/Dict constructors ---
    {
        KiritoVM vm;
        CHECK(run(vm, "String(List(\"abc\"))") == "['a', 'b', 'c']");
        CHECK(run(vm, "len(Set([1, 2, 2, 3, 3, 3]))") == "3");
        CHECK(run(vm, "var d = Dict([[1, 2], [3, 4]])\nString(d[3])") == "4");
        CHECK(run(vm, "String(List())") == "[]");
    }

    // --- itertools ---
    {
        KiritoVM vm;
        CHECK(run(vm, "var it = import(\"itertools\")\nString(it.chain([[1, 2], [3], [4, 5]]))") == "[1, 2, 3, 4, 5]");
        CHECK(run(vm, "var it = import(\"itertools\")\nString(it.accumulate([1, 2, 3, 4]))") == "[1, 3, 6, 10]");
        CHECK(run(vm, "var it = import(\"itertools\")\nlen(it.permutations([1, 2, 3]))") == "6");
        CHECK(run(vm, "var it = import(\"itertools\")\nString(it.combinations([1, 2, 3], 2))") == "[[1, 2], [1, 3], [2, 3]]");
        CHECK(run(vm, "var it = import(\"itertools\")\nString(it.product([[1, 2], [3, 4]]))") == "[[1, 3], [1, 4], [2, 3], [2, 4]]");
        CHECK(run(vm, "var it = import(\"itertools\")\nString(it.repeat(7, 3))") == "[7, 7, 7]");
        CHECK(run(vm, "var it = import(\"itertools\")\nString(it.islice([0, 1, 2, 3, 4, 5], 1, 5, 2))") == "[1, 3]");
    }

    // --- functools ---
    {
        KiritoVM vm;
        CHECK(run(vm, "var f = import(\"functools\")\nf.reduce(Function(a, b): return a * b, [1, 2, 3, 4])") == "24");
        CHECK(run(vm, "var f = import(\"functools\")\nf.reduce(Function(a, b): return a + b, [1, 2, 3], 100)") == "106");
    }

    // --- collections ---
    {
        KiritoVM vm;
        CHECK(run(vm, "var c = import(\"collections\")\nvar d = c.deque([1, 2, 3])\nd.appendleft(0)\nd.popleft()") == "0");
        CHECK(run(vm, "var c = import(\"collections\")\nvar k = c.Counter([\"a\", \"b\", \"a\"])\nk.get(\"a\")") == "2");
        CHECK(run(vm, "var c = import(\"collections\")\nvar dd = c.defaultdict(Function(): return 0)\ndd[\"x\"] = dd[\"x\"] + 5\ndd[\"x\"]") == "5");
    }

    // --- statistics ---
    {
        KiritoVM vm;
        CHECK(run(vm, "var s = import(\"statistics\")\nString(s.mean([2, 4, 6]))") == "4.0");
        CHECK(run(vm, "var s = import(\"statistics\")\nString(s.median([1, 2, 3, 4]))") == "2.5");
        CHECK(run(vm, "var s = import(\"statistics\")\ns.mode([1, 1, 2, 3, 1])") == "1");
        CHECK(run(vm, "var s = import(\"statistics\")\nString(round(s.variance([1, 2, 3, 4, 5]), 1))") == "2.5");
    }

    // --- string ---
    {
        KiritoVM vm;
        CHECK(run(vm, "import(\"string\").digits") == "0123456789");
        CHECK(run(vm, "import(\"string\").capwords(\"hello WORLD\")") == "Hello World");
        CHECK(run(vm, "len(import(\"string\").ascii_letters)") == "52");
    }

    // --- textwrap ---
    {
        KiritoVM vm;
        CHECK(run(vm, "var t = import(\"textwrap\")\nString(t.wrap(\"a b c d e\", 3))") == "['a b', 'c d', 'e']");
        CHECK(run(vm, "var t = import(\"textwrap\")\nt.dedent(\"    x\\n    y\")") == "x\ny");
    }

    // --- base64 round-trip ---
    {
        KiritoVM vm;
        CHECK(run(vm, "import(\"base64\").encode([77, 97, 110])") == "TWFu");
        CHECK(run(vm, "var b = import(\"base64\")\nString(b.decode(b.encode([1, 2, 3, 4, 5])))") == "[1, 2, 3, 4, 5]");
        CHECK(run(vm, "import(\"base64\").encode([77])") == "TQ==");
    }

    // --- csv ---
    {
        KiritoVM vm;
        CHECK(run(vm, "var c = import(\"csv\")\nc.formatrow([\"a\", \"b,c\", \"d\"])") == "a,\"b,c\",d");
        CHECK(run(vm, "var c = import(\"csv\")\nString(c.parserow(\"x,\\\"y,z\\\",w\"))") == "['x', 'y,z', 'w']");
    }

    // --- heapq ---
    {
        KiritoVM vm;
        CHECK(run(vm, "var h = import(\"heapq\")\nString(h.nsmallest(3, [5, 1, 8, 3, 9, 2]))") == "[1, 2, 3]");
        CHECK(run(vm, "var h = import(\"heapq\")\nvar q = []\nh.heappush(q, 5)\nh.heappush(q, 1)\nh.heappush(q, 3)\nh.heappop(q)") == "1");
    }

    // --- bisect ---
    {
        KiritoVM vm;
        CHECK(run(vm, "import(\"bisect\").bisectleft([1, 3, 5, 7], 4)") == "2");
        CHECK(run(vm, "import(\"bisect\").bisectright([1, 3, 3, 5], 3)") == "3");
        CHECK(run(vm, "var b = import(\"bisect\")\nvar a = [1, 3, 5]\nb.insortleft(a, 4)\nString(a)") == "[1, 3, 4, 5]");
    }

    // --- copy ---
    {
        KiritoVM vm;
        CHECK(run(vm, "var c = import(\"copy\")\nvar o = [[1, 2], [3]]\nvar d = c.deepcopy(o)\nd[0][0] = 9\nString(o)") == "[[1, 2], [3]]");
        CHECK(run(vm, "var c = import(\"copy\")\nvar o = [1, 2]\nvar s = c.copy(o)\ns.append(3)\nlen(o)") == "2");
    }

    // --- enum ---
    {
        KiritoVM vm;
        CHECK(run(vm, "var e = import(\"enum\")\nvar C = e.Enum([\"A\", \"B\", \"C\"])\nString(C.get(\"B\"))") == "1");
        CHECK(run(vm, "var e = import(\"enum\")\nvar C = e.Enum([\"A\", \"B\"])\nC.nameof(0)") == "A");
        CHECK(run(vm, "var e = import(\"enum\")\nvar C = e.Enum([\"A\"])\nString(\"A\" in C)") == "True");
    }

    // --- _iter_ protocol on a user class ---
    {
        KiritoVM vm;
        CHECK(run(vm,
            "class Bag:\n"
            "    var _init_ = Function(self):\n"
            "        self._xs = [1, 2, 3]\n"
            "    var _iter_ = Function(self):\n"
            "        return iter(self._xs)\n"
            "var total = 0\nfor x in Bag():\n    total = total + x\ntotal") == "6");
    }

    // --- list.pop(index) ---
    {
        KiritoVM vm;
        CHECK(run(vm, "var a = [10, 20, 30]\na.pop(0)") == "10");
        CHECK(run(vm, "var a = [10, 20, 30]\na.pop(-1)") == "30");
        CHECK(run(vm, "var a = [10, 20, 30]\na.pop(1)\nString(a)") == "[10, 30]");
    }

    return RUN_TESTS();
}
