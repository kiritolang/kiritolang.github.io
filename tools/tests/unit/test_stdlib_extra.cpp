// Newer stdlib/builtin additions: datetime construction/arithmetic/strptime, net URL helpers, and
// the format() builtin's mini-format-spec.
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string run(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

int main() {
    // --- time: construction, fields, arithmetic, strptime, round-trip ---
    {
        KiritoVM vm;
        CHECK(run(vm, "var t = import(\"time\")\nt.make(2024, 2, 29, 12, 30, 45).iso()")
              == "2024-02-29T12:30:45");
        CHECK(run(vm, "var t = import(\"time\")\nt.make(2024, 2, 29).weekday") == "4");  // Thursday
        CHECK(run(vm, "var t = import(\"time\")\nvar d = t.make(2000, 1, 1)\nd.add(86400).day") == "2");
        CHECK(run(vm, "var t = import(\"time\")\nvar d = t.make(2000, 1, 2)\nd.sub(86400).day") == "1");
        CHECK(run(vm, "var t = import(\"time\")\nt.make(2000, 1, 2).diff(t.make(2000, 1, 1))") == "86400");
        CHECK(run(vm, "var t = import(\"time\")\nt.strptime(\"2023-06-15 08:05:00\", \"%Y-%m-%d %H:%M:%S\").iso()")
              == "2023-06-15T08:05:00");
        // strptime -> timestamp -> datetime round-trips
        CHECK(run(vm,
            "var t = import(\"time\")\nvar p = t.strptime(\"2023-06-15\", \"%Y-%m-%d\")\n"
            "t.datetime(p.timestamp).iso()") == "2023-06-15T00:00:00");
        // bad format throws
        try { vm.runSource("import(\"time\").strptime(\"nope\", \"%Y-%m-%d\")"); CHECK(false); }
        catch (const KiritoError&) {}
    }

    // --- net URL helpers ---
    {
        KiritoVM vm;
        CHECK(run(vm, "import(\"net\").quote(\"a b/c\")") == "a%20b%2Fc");
        CHECK(run(vm, "import(\"net\").unquote(\"a%20b%2Fc\")") == "a b/c");
        CHECK(run(vm, "import(\"net\").unquote(\"a+b\")") == "a b");  // + -> space
        CHECK(run(vm, "var n = import(\"net\")\nn.urlencode({\"q\": \"a b\"})") == "q=a%20b");
        CHECK(run(vm, "var n = import(\"net\")\nvar p = n.urlsplit(\"https://h.com:8080/x?a=1#f\")\np[\"host\"]") == "h.com");
        CHECK(run(vm, "var n = import(\"net\")\nvar p = n.urlsplit(\"https://h.com:8080/x?a=1#f\")\np[\"port\"]") == "8080");
        CHECK(run(vm, "var n = import(\"net\")\nvar p = n.urlsplit(\"https://h.com/x?a=1#f\")\np[\"query\"]") == "a=1");
        CHECK(run(vm, "var n = import(\"net\")\nvar p = n.urlsplit(\"http://h/p?x=1#frag\")\np[\"fragment\"]") == "frag");
        CHECK(run(vm, "var n = import(\"net\")\nvar q = n.parseqs(\"name=Ada+L&id=42\")\nq[\"name\"]") == "Ada L");
        CHECK(run(vm, "var n = import(\"net\")\nvar q = n.parseqs(\"name=Ada&id=42\")\nq[\"id\"]") == "42");
        // round-trip quote/unquote on tricky bytes
        CHECK(run(vm, "var n = import(\"net\")\nn.unquote(n.quote(\"100% & more?\"))") == "100% & more?");
    }

    // --- format() mini-format-spec ---
    {
        KiritoVM vm;
        CHECK(run(vm, "format(42, \"05d\")") == "00042");
        CHECK(run(vm, "format(255, \"x\")") == "ff");
        CHECK(run(vm, "format(255, \"X\")") == "FF");
        CHECK(run(vm, "format(7, \"b\")") == "111");
        CHECK(run(vm, "format(64, \"o\")") == "100");
        CHECK(run(vm, "format(3.14159, \".2f\")") == "3.14");
        CHECK(run(vm, "format(1234567, \",\")") == "1,234,567");
        CHECK(run(vm, "format(1234567.5, \",.1f\")") == "1,234,567.5");
        CHECK(run(vm, "format(\"hi\", \"<6\") + \"|\"") == "hi    |");
        CHECK(run(vm, "format(\"hi\", \">6\")") == "    hi");
        CHECK(run(vm, "format(\"hi\", \"^6\")") == "  hi  ");
        CHECK(run(vm, "format(\"x\", \"*^7\")") == "***x***");
        CHECK(run(vm, "format(0.25, \".1%\")") == "25.0%");
        CHECK(run(vm, "format(-42, \"+06d\")") == "-00042");
        CHECK(run(vm, "format(42, \"+d\")") == "+42");
        CHECK(run(vm, "format(42, \" d\")") == " 42");
        CHECK(run(vm, "format(\"hello\", \".3\")") == "hel");
        CHECK(run(vm, "format(42)") == "42");                 // no spec
        CHECK(run(vm, "format(255, \"#x\")") == "0xff");      // # alternate form adds the base prefix
        // a malformed spec throws
        try { vm.runSource("format(5, \"qq\")"); CHECK(false); } catch (const KiritoError&) {}
    }

    return RUN_TESTS();
}
