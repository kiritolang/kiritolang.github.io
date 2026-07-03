// The `regex` module + its linear-time engine. Covers the public API (compile/match/search/
// fullmatch/findall/finditer/sub/split/escape, Match accessors, flags), adversarial/error cases
// (the constructs we deliberately reject), a linear-time stress (no catastrophic backtracking), and
// a property-based fuzz against the engine directly.
#include <chrono>
#include <random>
#include <string>

#include "../check.hpp"
#include "kirito.hpp"
#include "kirito/regex_engine.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}
// run a snippet with the regex module already imported as `re` and `io`
static std::string re(KiritoVM& vm, const std::string& expr) {
    return evalStr(vm, "var io = import(\"io\")\nvar re = import(\"regex\")\n" + expr);
}

int main() {
    // -------------------------------------------------- search / match / fullmatch
    {
        KiritoVM vm;
        CHECK(re(vm, "re.search(\"\\\\d+\", \"abc123def\").group()") == "123");
        CHECK(re(vm, "re.match(\"\\\\d+\", \"abc123\")") == "None");          // not anchored at 0
        CHECK(re(vm, "re.match(\"abc\", \"abcdef\").group()") == "abc");
        CHECK(re(vm, "re.fullmatch(\"\\\\d+\", \"12345\").group()") == "12345");
        CHECK(re(vm, "re.fullmatch(\"\\\\d+\", \"123a\")") == "None");
        CHECK(re(vm, "re.fullmatch(\"a*?\", \"aaa\").group()") == "aaa");      // fullmatch forces lazy to consume all
        CHECK(re(vm, "re.search(\"x\", \"no ex here\").start()") == "4");
        CHECK(re(vm, "re.search(\"missing\", \"text\")") == "None");
    }

    // -------------------------------------------------- groups, named groups, accessors
    {
        KiritoVM vm;
        CHECK(re(vm, "re.search(\"(\\\\d+)-(\\\\d+)\", \"id 12-345 x\").group(1)") == "12");
        CHECK(re(vm, "re.search(\"(\\\\d+)-(\\\\d+)\", \"id 12-345 x\").group(2)") == "345");
        CHECK(re(vm, "re.search(\"(\\\\d+)-(\\\\d+)\", \"id 12-345 x\").groups()") == "['12', '345']");
        CHECK(re(vm, "re.search(\"(\\\\d+)-(\\\\d+)\", \"id 12-345 x\").span()") == "[3, 9]");
        CHECK(re(vm, "re.search(\"(\\\\d+)-(\\\\d+)\", \"id 12-345 x\").span(2)") == "[6, 9]");
        // named groups + groupdict
        CHECK(re(vm, "re.search(\"(?P<y>\\\\d{4})-(?P<m>\\\\d{2})\", \"2024-06\").group(\"y\")") == "2024");
        CHECK(re(vm, "re.search(\"(?<y>\\\\d{4})\", \"2024\").group(\"y\")") == "2024");   // (?<name>) form
        CHECK(re(vm, "re.search(\"(?P<a>x)(?P<b>y)\", \"xy\").groupdict()[\"a\"]") == "x");
        CHECK(re(vm, "re.search(\"(?P<a>x)(?P<b>y)\", \"xy\").groupdict()[\"b\"]") == "y");
        // optional group that didn't participate -> None / start == -1
        CHECK(re(vm, "re.search(\"a(b)?c\", \"ac\").group(1)") == "None");
        CHECK(re(vm, "re.search(\"a(b)?c\", \"ac\").start(1)") == "-1");
        CHECK(re(vm, "re.search(\"a(b)?c\", \"abc\").group(1)") == "b");
    }

    // -------------------------------------------------- quantifiers & alternation priority
    {
        KiritoVM vm;
        CHECK(re(vm, "re.search(\"a{2,3}\", \"aaaa\").group()") == "aaa");      // greedy
        CHECK(re(vm, "re.search(\"a{2,3}?\", \"aaaa\").group()") == "aa");      // lazy
        CHECK(re(vm, "re.search(\"a+?\", \"aaa\").group()") == "a");
        CHECK(re(vm, "re.search(\"<.*>\", \"<a><b>\").group()") == "<a><b>");   // greedy .*
        CHECK(re(vm, "re.search(\"<.*?>\", \"<a><b>\").group()") == "<a>");     // lazy .*?
        CHECK(re(vm, "re.search(\"a|ab\", \"abc\").group()") == "a");           // leftmost-first
        CHECK(re(vm, "re.search(\"colou?r\", \"colour\").group()") == "colour");
    }

    // -------------------------------------------------- character classes, anchors, boundaries
    {
        KiritoVM vm;
        CHECK(re(vm, "re.search(\"[^0-9]+\", \"123abc456\").group()") == "abc");
        CHECK(re(vm, "re.search(\"[a-fA-F0-9]+\", \"xyz 0FxA z\").group()") == "0F");  // 'x' isn't a hex digit
        CHECK(re(vm, "re.findall(\"[a-fA-F0-9]+\", \"xyz 0FxA z\")") == "['0F', 'A']");
        CHECK(re(vm, "re.findall(\"\\\\bword\\\\b\", \"word words word\")") == "['word', 'word']");
        CHECK(re(vm, "re.search(\"^abc$\", \"abc\").group()") == "abc");
        CHECK(re(vm, "re.search(\"^abc$\", \"xabc\")") == "None");
        CHECK(re(vm, "re.findall(\"(?m)^\\\\w+\", \"one\\ntwo\\nthree\")") == "['one', 'two', 'three']");
        CHECK(re(vm, "re.search(\".\", \"\\n\")") == "None");                   // . excludes newline
        CHECK(re(vm, "re.search(\"(?s).\", \"\\n\").group()") == "\n");          // DOTALL
    }

    // -------------------------------------------------- flags
    {
        KiritoVM vm;
        CHECK(re(vm, "re.search(\"hello\", \"HELLO\", re.IGNORECASE).group()") == "HELLO");
        CHECK(re(vm, "re.compile(\"cat\", re.I).findall(\"Cat CAT cat\")") == "['Cat', 'CAT', 'cat']");
        CHECK(re(vm, "re.search(\"(?i)abc\", \"ABC\").group()") == "ABC");       // inline flag
    }

    // -------------------------------------------------- findall variants
    {
        KiritoVM vm;
        CHECK(re(vm, "re.findall(\"\\\\w+\", \"a bb ccc\")") == "['a', 'bb', 'ccc']");          // 0 groups -> whole
        CHECK(re(vm, "re.findall(\"(\\\\w)\\\\w*\", \"foo bar\")") == "['f', 'b']");           // 1 group -> that group
        CHECK(re(vm, "re.findall(\"(\\\\w+)=(\\\\d+)\", \"a=1 b=22\")") == "[['a', '1'], ['b', '22']]");  // >1 -> tuples
        CHECK(re(vm, "len(re.finditer(\"\\\\d+\", \"1 22 333\"))") == "3");
        CHECK(re(vm, "re.findall(\"\\\\d*\", \"a1b\")") == "['', '1', '', '']");                   // empty matches included
    }

    // -------------------------------------------------- sub (string & callable repl) and split
    {
        KiritoVM vm;
        CHECK(re(vm, "re.sub(\"\\\\d+\", \"#\", \"a1b22c333\")") == "a#b#c#");
        CHECK(re(vm, "re.sub(\"(\\\\w+)@(\\\\w+)\", \"\\\\2.\\\\1\", \"user@host\")") == "host.user");
        CHECK(re(vm, "re.sub(\"(?P<n>\\\\d+)\", \"[\\\\g<n>]\", \"x9y\")") == "x[9]y");
        CHECK(re(vm, "re.sub(\"a\", \"X\", \"aaaa\", 2)") == "XXaa");             // count
        CHECK(re(vm, "re.sub(\"\", \"-\", \"ab\")") == "-a-b-");                  // empty-pattern sub
        // callable replacement
        CHECK(re(vm, "re.sub(\"\\\\d+\", Function(m): return \"<\" + m.group() + \">\", \"a1b22\")") == "a<1>b<22>");
        // split
        CHECK(re(vm, "re.split(\",\\\\s*\", \"a, b,c ,  d\")") == "['a', 'b', 'c ', 'd']");
        CHECK(re(vm, "re.split(\"(\\\\s+)\", \"a b\")") == "['a', ' ', 'b']");          // captured separator kept
        CHECK(re(vm, "re.split(\"x\", \"axbxc\", 1)") == "['a', 'bxc']");             // maxsplit
    }

    // -------------------------------------------------- escape + Regex attributes
    {
        KiritoVM vm;
        CHECK(re(vm, "re.escape(\"a.b*c\")") == "a\\.b\\*c");
        CHECK(re(vm, "re.search(re.escape(\"a.b\"), \"xa.byz\").group()") == "a.b");
        CHECK(re(vm, "re.compile(\"(a)(b)\").groups") == "2");
        CHECK(re(vm, "re.compile(\"x+\").pattern") == "x+");
    }

    // -------------------------------------------------- unicode (literal UTF-8 é in the subject/pattern)
    {
        KiritoVM vm;
        CHECK(re(vm, "re.search(\"café\", \"a café b\").group()") == "café");
        CHECK(re(vm, "re.search(\".\", \"é\").group()") == "é");            // one code point
        CHECK(re(vm, "re.search(\"é+\", \"ééé!\").span()") == "[0, 3]");    // spans are code-point indices
    }

    // -------------------------------------------------- adversarial: rejected constructs throw
    {
        KiritoVM vm;
        CHECK_THROWS(vm.runSource("import(\"regex\").compile(\"(a)\\\\1\")"));     // backreference
        CHECK_THROWS(vm.runSource("import(\"regex\").compile(\"(?=a)\")"));        // lookahead
        CHECK_THROWS(vm.runSource("import(\"regex\").compile(\"(?<=a)\")"));       // lookbehind
        CHECK_THROWS(vm.runSource("import(\"regex\").compile(\"(abc\")"));         // unbalanced (
        CHECK_THROWS(vm.runSource("import(\"regex\").compile(\"abc)\")"));         // unbalanced )
        CHECK_THROWS(vm.runSource("import(\"regex\").compile(\"[z-a]\")"));        // bad range
        CHECK_THROWS(vm.runSource("import(\"regex\").compile(\"*abc\")"));         // nothing to repeat
        CHECK_THROWS(vm.runSource("import(\"regex\").compile(\"a{2,1}\")"));       // bad bounds
        CHECK_THROWS(vm.runSource("import(\"regex\").compile(\"a{5000}\")"));      // repeat too large
        CHECK_THROWS(vm.runSource("import(\"regex\").compile(\"[abc\")"));         // unterminated class
    }

    // -------------------------------------------------- linear time: no catastrophic backtracking
    {
        KiritoVM vm;
        // (a+)+b on a long run of 'a' with no trailing b: exponential for a backtracker, instant here.
        auto start = std::chrono::steady_clock::now();
        CHECK(re(vm, "re.search(\"(a+)+b\", \"a\" * 6000 + \"c\")") == "None");
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        CHECK(ms < 5000);   // generously bounded; a backtracking engine never finishes
    }

    // -------------------------------------------------- property-based fuzz on the engine
    {
        using namespace reng;
        std::mt19937 rng(0x9E3779B9u);
        const char* atoms[] = {"a", "b", "c", ".", "\\d", "\\w", "[ab]", "[^a]", "(a|b)", "x"};
        const char* quants[] = {"", "*", "+", "?", "*?", "+?", "{1,3}", "{2}"};
        for (int iter = 0; iter < 3000; ++iter) {
            // build a small random pattern
            std::string pat;
            int parts = 1 + (rng() % 4);
            for (int i = 0; i < parts; ++i) {
                pat += atoms[rng() % 10];
                pat += quants[rng() % 8];
                if (i + 1 < parts && (rng() % 4 == 0)) pat += "|";
            }
            Program prog;
            try { prog = compile(pat, (rng() % 2) ? IGNORECASE : 0); }
            catch (const RegexError&) { continue; }   // some random patterns are invalid; skip
            // random input over a small alphabet
            std::string in;
            int len = static_cast<int>(rng() % 12);
            for (int i = 0; i < len; ++i) in += static_cast<char>('a' + (rng() % 4));
            auto text = toCodepoints(in);
            MatchResult r = run(prog, text, 0, false, false);   // must terminate (linear time)
            if (r.matched) {
                // invariants: whole-match span is well-formed and within bounds
                int a = r.slots[0], b = r.slots[1];
                CHECK(a >= 0 && a <= b && b <= static_cast<int>(text.size()));
                // every participating group lies within the whole match
                for (int g = 1; g <= prog.numGroups; ++g) {
                    int ga = r.slots[2 * g], gb = r.slots[2 * g + 1];
                    if (ga >= 0) CHECK(ga >= a && ga <= gb && gb <= b);
                }
                // the matched substring, fullmatched against the same pattern, still matches
                std::vector<int32_t> sub(text.begin() + a, text.begin() + b);
                CHECK(run(prog, sub, 0, true, true).matched);
            }
        }
    }

    return RUN_TESTS();
}
