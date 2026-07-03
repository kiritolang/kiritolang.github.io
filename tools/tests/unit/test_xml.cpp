// The built-in `xml` module (ElementTree-style parser/serializer): a C++ integration test that the
// frozen Kirito module loads and its core surface works. Exhaustive behavioral + fuzz coverage lives
// in tests/scripts/spec_xml.ki; this guards the embedded module and the import path.
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string run(const std::string& body) {
    KiritoVM vm;
    return vm.stringify(vm.runSource("var xml = import(\"xml\")\n" + body));
}

int main() {
    // --- parsing: tags, attributes, nested text ---
    CHECK(run("xml.parse(\"<r><a>1</a></r>\").tag") == "r");
    CHECK(run("xml.parse(\"<r x='1' y=\\\"2\\\"/>\").get(\"x\")") == "1");
    CHECK(run("xml.parse(\"<r x='1' y=\\\"2\\\"/>\").get(\"y\")") == "2");
    CHECK(run("xml.parse(\"<r><a>hi</a></r>\").find(\"a\").text") == "hi");
    CHECK(run("len(xml.parse(\"<r><a/><a/><b/></r>\").findall(\"a\"))") == "2");
    CHECK(run("xml.parse(\"<r><a/></r>\").find(\"nope\")") == "None");
    CHECK(run("xml.parse(\"<r/>\").get(\"missing\", \"d\")") == "d");

    // --- entities decode (named + numeric dec/hex); unknown kept verbatim ---
    CHECK(run("xml.parse(\"<e>&lt;&gt;&amp;&quot;&apos;</e>\").text") == "<>&\"'");
    CHECK(run("xml.parse(\"<e>&#65;&#x42;</e>\").text") == "AB");
    CHECK(run("xml.parse(\"<e>a&zzz;b</e>\").text") == "a&zzz;b");

    // --- comments / declaration / DOCTYPE / CDATA ---
    CHECK(run("xml.parse(\"<?xml version='1.0'?><!-- c --><r>x</r>\").text") == "x");
    CHECK(run("xml.parse(\"<r><![CDATA[ <raw> & ]]></r>\").text") == " <raw> & ");

    // --- mixed content keeps order via tail; itertext gathers it ---
    CHECK(run("\"\".join(xml.parse(\"<r>x<a>y</a>z</r>\").itertext())") == "xyz");

    // --- serialization round-trips (re-escaping) ---
    CHECK(run("xml.tostring(xml.parse(\"<r k=\\\"a&amp;b\\\">x &lt; y</r>\"))") == "<r k=\"a&amp;b\">x &lt; y</r>");
    CHECK(run("xml.tostring(xml.parse(\"<a/>\"))") == "<a />");

    // --- adversarial: a lenient parser never crashes; empty input -> None ---
    CHECK(run("xml.parse(\"\")") == "None");
    {
        KiritoVM vm;
        // these must not throw (the parser is intentionally tolerant of malformed markup)
        CHECK(vm.stringify(vm.runSource("var xml = import(\"xml\")\nxml.parse(\"<a><b></a>\").tag")) == "a");
        CHECK(vm.stringify(vm.runSource("var xml = import(\"xml\")\nxml.parse(\"<a\").tag")) == "a");  // lenient
        CHECK(vm.stringify(vm.runSource("var xml = import(\"xml\")\nxml.parse(\"plain text\")")) == "None");  // no element
    }

    return RUN_TESTS();
}
