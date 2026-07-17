// Unicode correctness: Kirito strings are code-point sequences, not byte sequences. Exercises the
// Polish pangram the user asked about, plus indexing/slicing/reversal/methods on multi-byte text.
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string run(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

int main() {
    KiritoVM vm;

    const std::string pangram =
        "chrząszcz brzmi w trzcinie w szczebrzeszynie w szczękach chrząszcza trzeszczy miąższ.";

    // round-trips unchanged
    CHECK(run(vm, "\"" + pangram + "\"") == pangram);
    // counted by code point (85 chars including the trailing '.'), not by byte
    CHECK(run(vm, "len(\"" + pangram + "\")") == "85");

    // code-point indexing
    CHECK(run(vm, "\"żółć\"[0]") == "ż");
    CHECK(run(vm, "\"żółć\"[1]") == "ó");
    CHECK(run(vm, "\"żółć\"[3]") == "ć");
    CHECK(run(vm, "len(\"żółć\")") == "4");

    // slicing on multi-byte text
    CHECK(run(vm, "\"miąższ\"[0:3]") == "mią");
    CHECK(run(vm, "\"miąższ\"[::-1]") == "zsżąim");
    CHECK(run(vm, "\"chrząszcz\"[3:5]") == "zą");  // c h r z ą s z c z -> indices 3,4

    // iteration yields code points
    CHECK(run(vm, "var n = 0\nfor c in \"żółć\":\n    n = n + 1\nn") == "4");

    // string methods are code-point aware
    CHECK(run(vm, "\"miąższ\".upper()") == "MIĄŻSZ");
    CHECK(run(vm, "\"CHRZĄSZCZ\".lower()") == "chrząszcz");
    CHECK(run(vm, "String(\"chrząszcz brzmi\".split(\" \"))") == "['chrząszcz', 'brzmi']");
    CHECK(run(vm, "\"-\".join([\"żółć\", \"miąższ\"])") == "żółć-miąższ");
    CHECK(run(vm, "\"szczebrzeszynie\".count(\"sz\")") == "2");
    CHECK(run(vm, "String(\"chrząszcz\".startswith(\"chrzą\"))") == "True");
    CHECK(run(vm, "\"  miąższ  \".strip()") == "miąższ");

    // ord/chr round-trip Polish code points
    CHECK(run(vm, "ord(\"ą\")") == "261");
    CHECK(run(vm, "chr(261)") == "ą");
    CHECK(run(vm, "ord(\"ż\")") == "380");
    CHECK(run(vm, "chr(ord(\"ł\"))") == "ł");

    // f-strings and concatenation preserve UTF-8
    CHECK(run(vm, "var w = \"chrząszcz\"\nf\"{w} brzmi\"") == "chrząszcz brzmi");
    CHECK(run(vm, "\"żółć\" + \"miąższ\"") == "żółćmiąższ");
    CHECK(run(vm, "\"ą\" * 3") == "ąąą");

    // astral-plane (4-byte) code point counts as one
    CHECK(run(vm, "len(\"😀\")") == "1");
    CHECK(run(vm, "\"a😀b\"[1]") == "😀");

    return RUN_TESTS();
}
