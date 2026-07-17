// The stdlib gap-fill additions: new collection/string methods, itertools/functools/heapq/base64/
// statistics members, math.prod/comb/perm, random.gauss, json loads/dumps/indent + surrogate pairs,
// and io path helpers.
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string run(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

int main() {
    // --- list/dict/set methods ---
    {
        KiritoVM vm;
        CHECK(run(vm, "[1, 2, 2, 3, 2].count(2)") == "3");
        CHECK(run(vm, "var a = [1, 2, 3]\na.clear()\nlen(a)") == "0");
        CHECK(run(vm, "var d = {\"a\": 1}\nd.update({\"b\": 2, \"a\": 9})\nString(d.get(\"a\")) + String(d.get(\"b\"))") == "92");
        CHECK(run(vm, "var d = {}\nd.setdefault(\"x\", 5)") == "5");
        CHECK(run(vm, "var d = {\"x\": 1}\nd.setdefault(\"x\", 5)") == "1");
        CHECK(run(vm, "var d = {\"a\": 1, \"b\": 2}\nd.clear()\nlen(d)") == "0");
        CHECK(run(vm, "var s = {1, 2, 3}\ns.discard(2)\ns.discard(99)\nString(2 in s) + String(len(s))") == "False2");
        CHECK(run(vm, "var s = {1, 2, 3}\ns.clear()\nlen(s)") == "0");
        CHECK(run(vm, "String({1, 2, 3}.issubset({1, 2, 3, 4}))") == "True");
        CHECK(run(vm, "String({1, 2, 3}.issuperset({1, 2}))") == "True");
        CHECK(run(vm, "String({1, 2}.isdisjoint({3, 4}))") == "True");
        CHECK(run(vm, "String({1, 2}.isdisjoint({2, 3}))") == "False");
        CHECK(run(vm, "len({1, 2, 3}.symmetricdifference({2, 3, 4}))") == "2");  // {1, 4}
        CHECK(run(vm, "var s = {7}\ns.pop()") == "7");
    }

    // --- string methods ---
    {
        KiritoVM vm;
        CHECK(run(vm, "String(\"123\".isdigit())") == "True");
        CHECK(run(vm, "String(\"12a\".isdigit())") == "False");
        CHECK(run(vm, "String(\"abc\".isalpha())") == "True");
        CHECK(run(vm, "String(\"ąćź\".isalpha())") == "True");   // non-ASCII letters
        CHECK(run(vm, "String(\"a1\".isalnum())") == "True");
        CHECK(run(vm, "String(\"   \".isspace())") == "True");
        CHECK(run(vm, "String(\"abc\".islower())") == "True");
        CHECK(run(vm, "String(\"ABC\".isupper())") == "True");
        CHECK(run(vm, "\"hello world\".index(\"world\")") == "6");
        CHECK(run(vm, "\"a.b.c\".rfind(\".\")") == "3");
        CHECK(run(vm, "\"foobar\".removeprefix(\"foo\")") == "bar");
        CHECK(run(vm, "\"foobar\".removesuffix(\"bar\")") == "foo");
        CHECK(run(vm, "\"hi\".ljust(5) + \"|\"") == "hi   |");
        CHECK(run(vm, "\"hi\".rjust(5)") == "   hi");
        CHECK(run(vm, "\"hi\".center(6, \"*\")") == "**hi**");
        CHECK(run(vm, "\"42\".zfill(5)") == "00042");
        CHECK(run(vm, "\"-42\".zfill(5)") == "-0042");
        CHECK(run(vm, "String(\"a=b=c\".partition(\"=\"))") == "['a', '=', 'b=c']");
        CHECK(run(vm, "String(\"a=b=c\".rpartition(\"=\"))") == "['a=b', '=', 'c']");
    }

    // --- itertools additions ---
    {
        KiritoVM vm;
        CHECK(run(vm, "var it = import(\"itertools\")\nString(it.takewhile(Function(x): return x < 3, [1, 2, 3, 1]))") == "[1, 2]");
        CHECK(run(vm, "var it = import(\"itertools\")\nString(it.dropwhile(Function(x): return x < 3, [1, 2, 3, 1]))") == "[3, 1]");
        CHECK(run(vm, "var it = import(\"itertools\")\nString(it.ziplongest([[1, 2], [3, 4, 5]], 0))") == "[[1, 3], [2, 4], [0, 5]]");
        CHECK(run(vm, "var it = import(\"itertools\")\nString(it.pairwise([1, 2, 3]))") == "[[1, 2], [2, 3]]");
        CHECK(run(vm, "var it = import(\"itertools\")\nString(it.groupby([1, 1, 2, 3, 3]))") == "[[1, [1, 1]], [2, [2]], [3, [3, 3]]]");
        CHECK(run(vm, "var it = import(\"itertools\")\nString(it.compress([1, 2, 3, 4], [1, 0, 1, 0]))") == "[1, 3]");
    }

    // --- functools.cache memoizes ---
    {
        KiritoVM vm;
        CHECK(run(vm,
            "var f = import(\"functools\")\nvar n = [0]\n"
            "var work = Function(x):\n    n[0] = n[0] + 1\n    return x * x\n"
            "var c = f.cache(work)\nvar r = c(5) + c(5) + c(5)\nString(r) + \" \" + String(n[0])") == "75 1");
    }

    // --- heapq additions ---
    {
        KiritoVM vm;
        CHECK(run(vm, "var h = import(\"heapq\")\nString(h.nlargest(3, [5, 1, 8, 3, 9, 2]))") == "[9, 8, 5]");
        CHECK(run(vm, "var h = import(\"heapq\")\nString(h.merge([[1, 4], [2, 5], [3, 6]]))") == "[1, 2, 3, 4, 5, 6]");
    }

    // --- base64 urlsafe round-trip ---
    {
        KiritoVM vm;
        CHECK(run(vm, "var b = import(\"base64\")\nString(b.urlsafedecode(b.urlsafeencode([251, 255, 191]))) == String([251, 255, 191])") == "True");
    }

    // --- statistics additions ---
    {
        KiritoVM vm;
        CHECK(run(vm, "var s = import(\"statistics\")\nString(s.multimode([1, 1, 2, 2, 3]))") == "[1, 2]");
        CHECK(run(vm, "var s = import(\"statistics\")\nlen(s.quantiles([1, 2, 3, 4, 5, 6, 7, 8]))") == "3");
    }

    // --- math additions ---
    {
        KiritoVM vm;
        CHECK(run(vm, "import(\"math\").prod([1, 2, 3, 4])") == "24");
        CHECK(run(vm, "String(import(\"math\").prod([1.5, 2.0]))") == "3.0");
        CHECK(run(vm, "import(\"math\").comb(5, 2)") == "10");
        CHECK(run(vm, "import(\"math\").perm(5, 2)") == "20");
        CHECK(run(vm, "import(\"math\").comb(10, 0)") == "1");
    }

    // --- random.gauss is deterministic per seed, varies across draws ---
    {
        KiritoVM vm;
        CHECK(run(vm, "var r = import(\"random\").Random(1)\nString(r.gauss(0.0, 1.0) != r.gauss(0.0, 1.0))") == "True");
    }

    // --- json aliases, indent, surrogate-pair decoding ---
    {
        KiritoVM vm;
        CHECK(run(vm, "var j = import(\"json\")\nString(j.loads(\"[1, 2, 3]\"))") == "[1, 2, 3]");
        CHECK(run(vm, "var j = import(\"json\")\nj.dumps([1, 2])") == "[1, 2]");
        CHECK(run(vm, "var j = import(\"json\")\nj.dumps({\"a\": 1}, 2)") == "{\n  \"a\": 1\n}");
        // a surrogate pair decodes to one astral code point
        CHECK(run(vm, "var j = import(\"json\")\nlen(j.parse(\"{\\\"e\\\": \\\"\\\\uD83D\\\\uDE00\\\"}\")[\"e\"])") == "1");
    }

    // --- path helpers (the `path` module — moved out of io) ---
    {
        KiritoVM vm;
        CHECK(run(vm, "import(\"path\").basename(\"/a/b/c.txt\")") == "c.txt");
        CHECK(run(vm, "import(\"path\").dirname(\"/a/b/c.txt\")") == "/a/b");
        CHECK(run(vm, "String(import(\"path\").splitext(\"file.tar.gz\"))") == "['file.tar', '.gz']");
        CHECK(run(vm, "import(\"path\").join(\"a\", \"b\", \"c\")") == "a/b/c");
    }

    return RUN_TESTS();
}
