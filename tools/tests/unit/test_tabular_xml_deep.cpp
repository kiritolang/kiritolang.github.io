// test_tabular_xml_deep.cpp — adversarial/edge coverage for `tabular` and `xml`, filling audited
// gaps: Series scalar -/// /% + setitem write path + _str_ rendering, DataFrame _str_, and xml's
// empty-numeric-entity / unterminated comment,CDATA,PI,decl lenient branches + non-String attribute
// coercion in tostring. (parallel's narrow gaps require the dispatcher-configured VM and belong in
// the test_parallel* harness, not a plain KiritoVM.)
#include <string>
#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string run(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}
static bool throws(KiritoVM& vm, const std::string& src) {
    try { vm.runSource(src); return false; }
    catch (...) { return true; }
}

int main() {
    KiritoVM vm;

    // ================= tabular =================

    // ---- Series scalar arithmetic: -, //, % (Series-scalar forms) ----
    CHECK(run(vm, "import(\"tabular\")\n(tabular.Series([10, 20, 30]) - 5).tolist()") == "[5, 15, 25]");
    CHECK(run(vm, "import(\"tabular\")\n(tabular.Series([10, 21]) // 2).tolist()") == "[5, 10]");
    CHECK(run(vm, "import(\"tabular\")\n(tabular.Series([10, 21, 32]) % 3).tolist()") == "[1, 0, 2]");

    // ---- Series _setitem_ write path (positional default index + string label) ----
    CHECK(run(vm, R"KI(import("tabular")
var s = tabular.Series([1, 2, 3])
s[0] = 99
s.tolist())KI") == "[99, 2, 3]");
    CHECK(run(vm, R"KI(import("tabular")
var s = tabular.Series([1, 2, 3], index=["a", "b", "c"])
s["b"] = 99
s.tolist())KI") == "[1, 99, 3]");

    // ---- Series / DataFrame _str_ rendering exercises the value + name/header layout ----
    CHECK(run(vm, "import(\"tabular\")\n\"20\" in String(tabular.Series([10, 20, 30]))") == "True");
    CHECK(run(vm, "import(\"tabular\")\n\"mycol\" in String(tabular.Series([10, 20], name=\"mycol\"))") == "True");
    CHECK(run(vm, R"KI(import("tabular")
"a" in String(tabular.readcsv("a,b\n1,2\n3,4")))KI") == "True");

    // ================= xml =================

    // ---- empty numeric entities are kept verbatim (parse returns -1 -> literal text) ----
    CHECK(run(vm, "import(\"xml\")\nxml.parse(\"<x>&#;</x>\").text") == "&#;");
    CHECK(run(vm, "import(\"xml\")\nxml.parse(\"<x>&#x;</x>\").text") == "&#x;");
    // a well-formed numeric entity still decodes (sanity around the same branch)
    CHECK(run(vm, "import(\"xml\")\nxml.parse(\"<x>&#65;</x>\").text") == "A");

    // ---- unterminated comment / CDATA / PI / decl are consumed leniently to end (no crash) ----
    CHECK(!throws(vm, "import(\"xml\")\ndiscard xml.parse(\"<a>ok</a><!-- trailing\")"));
    CHECK(!throws(vm, "import(\"xml\")\ndiscard xml.parse(\"<a>ok</a><![CDATA[ trailing\")"));
    CHECK(!throws(vm, "import(\"xml\")\ndiscard xml.parse(\"<a>ok</a><?pi trailing\")"));
    CHECK(!throws(vm, "import(\"xml\")\ndiscard xml.parse(\"<a>ok</a><!DOCTYPE x\")"));

    // ---- tostring coerces a non-String attribute value ----
    CHECK(run(vm, "import(\"xml\")\n\"42\" in xml.tostring(xml.Element(\"r\", {\"id\": 42}))") == "True");

    return RUN_TESTS();
}
