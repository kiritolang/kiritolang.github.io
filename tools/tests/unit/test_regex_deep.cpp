// test_regex_deep.cpp — adversarial/edge coverage for the `regex` module: fills audited gaps
// (named-key start/end/span, \v \f \a escapes, two-digit template refs, _call_ replacements,
// pos/endpos on findall/finditer, non-serializability, malformed group names, lenient repetition,
// octal/negated shorthands in classes, keyword args on methods, and the str() repr forms).
#include <string>
#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// Run a Kirito program, return the last expression stringified.
static std::string run(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}
// True iff the program raises a Kirito error (a caught KiritoError or any std::exception).
static bool throws(KiritoVM& vm, const std::string& src) {
    try { vm.runSource(src); return false; }
    catch (...) { return true; }
}

int main() {
    KiritoVM vm;

    // ---------------------------------------------------------------- named-group key on start/end/span
    CHECK(run(vm, R"KI(import("regex")
var m = regex.search(r"(?P<yr>\d+)", "abc 2024 xyz")
m.start("yr"))KI") == "4");
    CHECK(run(vm, R"KI(import("regex")
var m = regex.search(r"(?P<yr>\d+)", "abc 2024 xyz")
m.end("yr"))KI") == "8");
    CHECK(run(vm, R"KI(import("regex")
var m = regex.search(r"(?P<yr>\d+)", "abc 2024 xyz")
m.span("yr"))KI") == "[4, 8]");
    CHECK(run(vm, R"KI(import("regex")
regex.search(r"(?P<yr>\d+)", "abc 2024 xyz").group("yr"))KI") == "2024");
    // a String key naming no group throws
    CHECK(throws(vm, R"KI(import("regex")
regex.search(r"(?P<yr>\d+)", "abc 2024 xyz").start("nope"))KI"));

    // ---------------------------------------------------------------- \v \f \a pattern escapes
    CHECK(run(vm, R"KI(import("regex")
ord(regex.fullmatch(r"\v", "\x0b").group()))KI") == "11");
    CHECK(run(vm, R"KI(import("regex")
ord(regex.fullmatch(r"\f", "\x0c").group()))KI") == "12");
    CHECK(run(vm, R"KI(import("regex")
ord(regex.fullmatch(r"\a", "\x07").group()))KI") == "7");

    // ---------------------------------------------------------------- two-digit template refs in sub
    CHECK(run(vm, R"KI(import("regex")
regex.sub(r"(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)(l)", r"\12", "abcdefghijkl"))KI") == "l");
    CHECK(run(vm, R"KI(import("regex")
regex.sub(r"(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)(l)", r"\g<12>", "abcdefghijkl"))KI") == "l");
    // single-digit \1 still resolves the first group, not the twelfth
    CHECK(run(vm, R"KI(import("regex")
regex.sub(r"(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)(l)", r"\1", "abcdefghijkl"))KI") == "a");
    // \n \t \r cooked inside a replacement template
    CHECK(run(vm, R"KI(import("regex")
ord(regex.sub("x", r"\t", "x")))KI") == "9");
    CHECK(run(vm, R"KI(import("regex")
ord(regex.sub("x", r"\n", "x")))KI") == "10");
    CHECK(run(vm, R"KI(import("regex")
ord(regex.sub("x", r"\r", "x")))KI") == "13");

    // ---------------------------------------------------------------- Instance _call_ as sub replacement
    CHECK(run(vm, R"KI(import("regex")
class Rep:
    var _call_ = Function(self, m):
        return "[" + m.group() + "]"
regex.sub("a", Rep(), "banana"))KI") == "b[a]n[a]n[a]");
    // the same instance via the compiled-Regex method, count=0 => replace all
    CHECK(run(vm, R"KI(import("regex")
class Rep:
    var _call_ = Function(self, m):
        return "X"
regex.compile("a").sub(Rep(), "aaa", 0))KI") == "XXX");

    // ---------------------------------------------------------------- findall / finditer pos & endpos
    CHECK(run(vm, R"KI(import("regex")
regex.compile(r"\d").findall("a1b2c3", 3))KI") == "['2', '3']");
    CHECK(run(vm, R"KI(import("regex")
regex.compile(r"\d").findall("a1b2c3", 0, 4))KI") == "['1', '2']");
    CHECK(run(vm, R"KI(import("regex")
len(regex.compile(r"\d").finditer("a1b2c3", 3)))KI") == "2");
    CHECK(run(vm, R"KI(import("regex")
len(regex.compile(r"\d").finditer("a1b2c3", 0, 4)))KI") == "2");

    // ---------------------------------------------------------------- Match / Regex are not serializable
    CHECK(throws(vm, R"KI(import("regex")
import("dump")
dump.dumps(regex.search("b", "abc")))KI"));
    CHECK(throws(vm, R"KI(import("regex")
import("serialize")
serialize.dumps(regex.search("b", "abc")))KI"));
    CHECK(throws(vm, R"KI(import("regex")
import("dump")
dump.dumps(regex.compile("ab")))KI"));

    // ---------------------------------------------------------------- bad group keys on a Match
    CHECK(throws(vm, R"KI(import("regex")
regex.search("b", "abc").group(1.5))KI"));
    CHECK(throws(vm, R"KI(import("regex")
regex.search("b", "abc").group([]))KI"));
    CHECK(throws(vm, R"KI(import("regex")
regex.search("b", "abc").group(-1))KI"));
    CHECK(throws(vm, R"KI(import("regex")
regex.search("b", "abc").start(-1))KI"));
    CHECK(throws(vm, R"KI(import("regex")
regex.search("b", "abc").end(-1))KI"));
    CHECK(throws(vm, R"KI(import("regex")
regex.search("b", "abc").span(-1))KI"));
    // a positive index past the group count also throws (no groups here)
    CHECK(throws(vm, R"KI(import("regex")
regex.search("b", "abc").group(1))KI"));

    // ---------------------------------------------------------------- malformed named groups (compile errors)
    CHECK(throws(vm, R"KI(import("regex")
regex.compile("(?P<x>a)(?P<x>b)"))KI"));   // duplicate name
    CHECK(throws(vm, R"KI(import("regex")
regex.compile("(?P<>a)"))KI"));            // empty name
    CHECK(throws(vm, R"KI(import("regex")
regex.compile("(?P<1x>a)"))KI"));          // bad (non-identifier) name
    CHECK(throws(vm, R"KI(import("regex")
regex.compile("(?P<a"))KI"));              // unterminated named group

    // ---------------------------------------------------------------- malformed \g<...> in a template
    CHECK(throws(vm, R"KI(import("regex")
regex.sub("(a)", r"\g<1", "a"))KI"));      // unterminated \g<
    CHECK(throws(vm, R"KI(import("regex")
regex.sub("(a)", r"\g1", "a"))KI"));       // no '<' after \g
    CHECK(throws(vm, R"KI(import("regex")
regex.sub("(a)", r"\g<nope>", "a"))KI"));  // no such named group

    // ---------------------------------------------------------------- clamped pos/endpos (must NOT throw)
    CHECK(run(vm, R"KI(import("regex")
regex.compile(r"\d").search("a1b", -5).group())KI") == "1");  // negative pos -> from 0
    CHECK(run(vm, R"KI(import("regex")
regex.compile(r"\d").search("a1b", 0, -1) == None)KI") == "True");  // negative endpos -> empty view
    CHECK(run(vm, R"KI(import("regex")
regex.compile(r"\d").search("a1b", 100) == None)KI") == "True");    // pos past end -> no match

    // ---------------------------------------------------------------- lenient invalid repetition ('{' literal)
    CHECK(run(vm, R"KI(import("regex")
regex.fullmatch("a{", "a{").group())KI") == "a{");
    CHECK(run(vm, R"KI(import("regex")
regex.fullmatch("a{x}", "a{x}").group())KI") == "a{x}");
    CHECK(run(vm, R"KI(import("regex")
regex.fullmatch("a{,3}", "a{,3}").group())KI") == "a{,3}");

    // ---------------------------------------------------------------- octal + negated shorthands in a class
    CHECK(run(vm, R"KI(import("regex")
regex.fullmatch(r"[\101]", "A").group())KI") == "A");   // \101 octal == 'A'
    CHECK(run(vm, R"KI(import("regex")
regex.fullmatch(r"[\D]", "a") != None)KI") == "True");
    CHECK(run(vm, R"KI(import("regex")
regex.fullmatch(r"[\D]", "5") == None)KI") == "True");
    CHECK(run(vm, R"KI(import("regex")
regex.fullmatch(r"[\W]", "!") != None)KI") == "True");
    CHECK(run(vm, R"KI(import("regex")
regex.fullmatch(r"[\S]", " ") == None)KI") == "True");

    // ---------------------------------------------------------------- keyword args on Regex methods
    CHECK(run(vm, R"KI(import("regex")
regex.compile("a").sub(repl="X", string="banana", count=0))KI") == "bXnXnX");
    CHECK(run(vm, R"KI(import("regex")
regex.compile(r"\d").match(string="12a", pos=0).group())KI") == "1");
    CHECK(run(vm, R"KI(import("regex")
regex.compile(r"\d+").match(string="123", pos=0, endpos=2).group())KI") == "12");
    CHECK(run(vm, R"KI(import("regex")
regex.compile(",").split(string="a,b,c", maxsplit=1))KI") == "['a', 'b,c']");

    // ---------------------------------------------------------------- module finditer flags=, sub count=0
    CHECK(run(vm, R"KI(import("regex")
len(regex.finditer("a", "AaA", flags=regex.IGNORECASE)))KI") == "3");
    CHECK(run(vm, R"KI(import("regex")
var f = Function(m): return "X"
regex.compile("a").sub(f, "aaa", 0))KI") == "XXX");

    // ---------------------------------------------------------------- textual repr of Regex / Match
    CHECK(run(vm, R"KI(import("regex")
regex.compile("ab"))KI") == "<Regex /ab/>");
    CHECK(run(vm, R"KI(import("regex")
regex.search("b", "abc"))KI") == "<Match span=(1, 2)>");

    return RUN_TESTS();
}
