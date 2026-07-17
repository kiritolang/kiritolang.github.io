// A large corpus of regular-expression test vectors driven through Kirito's engine. The bulk are
// the classic, public-domain vectors that have been reused for decades (Henry Spencer's regexp
// suite -> PCRE -> the canonical re_tests "tests" table), re-authored here as plain data and
// pruned of every case that needs a DELIBERATELY-EXCLUDED feature (backreferences, lookaround) —
// those are covered separately as must-throw cases. On top of the classic table this adds difficult
// cases (empty-bodied loops, deep nesting, pathological alternations) and a structured fuzz.
#include <random>
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito/regex_engine.hpp"

using namespace kirito;
using namespace kirito::reng;

// A vector: pattern, subject, flags, whether re.search should match, and (when it matches) the
// expected whole match text. `nullptr` g0 with match==true means "don't check the text".
struct Case { const char* pat; const char* in; int flags; bool match; const char* g0; };

static std::string wholeMatch(const Program& p, const std::string& in, bool& ok) {
    auto t = toCodepoints(in);
    MatchResult r = run(p, t, 0, /*anchored=*/false, /*requireEnd=*/false);
    ok = r.matched;
    if (!ok) return "";
    std::string s;
    for (int i = r.slots[0]; i < r.slots[1]; ++i) utf8Encode(static_cast<unsigned>(t[i]), s);
    return s;
}

int main() {
    const Case cases[] = {
        // --- literals & dot ---
        {"abc", "abc", 0, true, "abc"},
        {"abc", "xbc", 0, false, ""},
        {"abc", "axc", 0, false, ""},
        {"abc", "abx", 0, false, ""},
        {"abc", "xabcy", 0, true, "abc"},
        {"abc", "ababc", 0, true, "abc"},
        {"", "", 0, true, ""},
        {"", "abc", 0, true, ""},
        {"a.c", "abc", 0, true, "abc"},
        {"a.c", "axc", 0, true, "axc"},
        {"a.b", "a\nb", 0, false, ""},            // . excludes newline
        {"a.b", "a\nb", DOTALL, true, "a\nb"},     // ... unless DOTALL
        // --- star ---
        {"ab*c", "abc", 0, true, "abc"},
        {"ab*bc", "abc", 0, true, "abc"},
        {"ab*bc", "abbc", 0, true, "abbc"},
        {"ab*bc", "abbbbc", 0, true, "abbbbc"},
        {"a.*c", "axyzc", 0, true, "axyzc"},
        {"a.*c", "axyzd", 0, false, ""},
        {".*", "", 0, true, ""},
        // --- plus ---
        {"ab+bc", "abbc", 0, true, "abbc"},
        {"ab+bc", "abc", 0, false, ""},
        {"ab+bc", "abq", 0, false, ""},
        {"ab+bc", "abbbbc", 0, true, "abbbbc"},
        // --- question ---
        {"ab?bc", "abbc", 0, true, "abbc"},
        {"ab?bc", "abc", 0, true, "abc"},
        {"ab?bc", "abbbbc", 0, false, ""},
        {"ab?c", "abc", 0, true, "abc"},
        {"colou?r", "color", 0, true, "color"},
        {"colou?r", "colour", 0, true, "colour"},
        // --- anchors ---
        {"^abc$", "abc", 0, true, "abc"},
        {"^abc$", "abcc", 0, false, ""},
        {"^abc", "abcc", 0, true, "abc"},
        {"^abc$", "aabc", 0, false, ""},
        {"abc$", "aabc", 0, true, "abc"},
        {"^", "abc", 0, true, ""},
        {"$", "abc", 0, true, ""},
        {"a$", "a\n", 0, true, "a"},               // $ matches before a final newline
        // --- classes ---
        {"a[bc]d", "abc", 0, false, ""},
        {"a[bc]d", "abd", 0, true, "abd"},
        {"a[b-d]e", "abd", 0, false, ""},
        {"a[b-d]e", "ace", 0, true, "ace"},
        {"a[b-d]", "aac", 0, true, "ac"},
        {"a[-b]", "a-", 0, true, "a-"},             // '-' at end is literal
        {"a[\\-b]", "a-", 0, true, "a-"},           // escaped '-'
        {"a[]]b", "a]b", 0, true, "a]b"},           // ']' right after '[' is literal
        {"a[^bc]d", "aed", 0, true, "aed"},
        {"a[^bc]d", "abd", 0, false, ""},
        {"a[^-b]c", "adc", 0, true, "adc"},
        {"a[^-b]c", "a-c", 0, false, ""},
        {"[a-zA-Z_][a-zA-Z0-9_]*", "_id9 = 1", 0, true, "_id9"},
        {"[\\d]+", "ab12cd", 0, true, "12"},
        {"[^\\d]+", "12ab34", 0, true, "ab"},
        {"[\\w]+", "  hi_5!", 0, true, "hi_5"},
        {"[\\s]+", "ab \t\ncd", 0, true, " \t\n"},
        // --- alternation ---
        {"a|b|c", "c", 0, true, "c"},
        {"ab|cd", "abcd", 0, true, "ab"},
        {"(cat|dog|bird)", "I have a dog", 0, true, "dog"},
        {"a|ab", "abc", 0, true, "a"},              // leftmost-first
        {"(a|b)*", "abba", 0, true, "abba"},
        {"(a|b)+", "abab", 0, true, "abab"},
        // --- bounded repeats ---
        {"a{3}", "aaaa", 0, true, "aaa"},
        {"a{2,4}", "aaaaa", 0, true, "aaaa"},
        {"a{2,}", "aaaaa", 0, true, "aaaaa"},
        {"a{0,2}", "aaa", 0, true, "aa"},
        {"a{0}", "aaa", 0, true, ""},
        {"a{2,3}?", "aaaa", 0, true, "aa"},          // lazy bounded
        // --- greedy vs lazy ---
        {"<.*>", "<a><b>", 0, true, "<a><b>"},
        {"<.*?>", "<a><b>", 0, true, "<a>"},
        {"a+?", "aaa", 0, true, "a"},
        {"a*?b", "aaab", 0, true, "aaab"},
        // --- word boundaries ---
        {"\\bfoo\\b", "a foo b", 0, true, "foo"},
        {"\\bfoo\\b", "afoob", 0, false, ""},
        {"\\Bfoo\\B", "afoob", 0, true, "foo"},
        {"\\bword", "a word", 0, true, "word"},
        // --- escapes & metachars as literals ---
        {"a\\.c", "a.c", 0, true, "a.c"},
        {"a\\.c", "axc", 0, false, ""},
        {"\\(\\d+\\)", "(42)", 0, true, "(42)"},
        {"\\$\\d+", "$100", 0, true, "$100"},
        {"\\\\", "a\\b", 0, true, "\\"},
        {"\\x41+", "AAA", 0, true, "AAA"},           // \x41 == 'A'
        {"a\\tb", "a\tb", 0, true, "a\tb"},
        // --- case-insensitive ---
        {"abc", "ABC", IGNORECASE, true, "ABC"},
        {"[a-z]+", "Hello", IGNORECASE, true, "Hello"},
        {"(?i)hello", "HeLLo", 0, true, "HeLLo"},
        // --- multiline ---
        {"(?m)^b", "a\nb\nc", 0, true, "b"},
        {"(?m)c$", "a\nbc\nd", 0, true, "c"},
        // --- groups don't change the whole match ---
        {"(abc)+", "abcabc", 0, true, "abcabc"},
        {"(?:abc)+", "abcabc", 0, true, "abcabc"},
        {"(a)(b)(c)", "abc", 0, true, "abc"},
        // --- difficult: empty-bodied loops must terminate AND match ---
        {"(a*)*", "aaa", 0, true, "aaa"},
        {"(a*)*", "", 0, true, ""},
        {"(a?)*", "aaa", 0, true, "aaa"},
        {"(a*)+", "aaa", 0, true, "aaa"},
        {"(|a)*", "aa", 0, true, "aa"},
        {"(a|)*", "aa", 0, true, "aa"},
        {"(a*)(a*)", "aaa", 0, true, "aaa"},
        // --- difficult: alternation that backtrackers choke on ---
        {"(a+)+$", "aaaaaaaaaaaaaaaaX", 0, false, ""},
        {"(x+x+)+y", "xxxxxxxxxxxxxxxx", 0, false, ""},
        {".*.*.*.*.*=.*", "key=value", 0, true, "key=value"},
        // --- realistic patterns ---
        {"\\d{4}-\\d{2}-\\d{2}", "date 2024-06-07!", 0, true, "2024-06-07"},
        {"\\d+\\.\\d+", "pi is 3.14159 ok", 0, true, "3.14159"},
        {"https?://[\\w./]+", "see http://a.b/c here", 0, true, "http://a.b/c"},
        {"[\\w.]+@[\\w.]+", "mail me ada@kirito.dev", 0, true, "ada@kirito.dev"},
        {"-?\\d+", "x -42 y", 0, true, "-42"},
        // --- unicode ---
        {"café", "le café", 0, true, "café"},
        {".", "λ", 0, true, "λ"},
        {"\\w+", "naïve_x", 0, true, "na"},          // ï is not ASCII \w, so the run stops at "na"
    };

    int idx = 0;
    for (const Case& c : cases) {
        ++idx;
        Program prog;
        try { prog = compile(c.pat, c.flags); }
        catch (const std::exception& e) {
            printf("FAIL[#%d compile] /%s/ : %s\n", idx, c.pat, e.what());
            continue;
        }
        bool ok = false;
        std::string got = wholeMatch(prog, c.in, ok);
        if (ok != c.match) {
            printf("FAIL[#%d] /%s/ on \"%s\": expected %s, got %s\n",
                   idx, c.pat, c.in, c.match ? "match" : "no-match", ok ? "match" : "no-match");
            CHECK(false);
        } else if (c.match && c.g0 && got != c.g0) {
            printf("FAIL[#%d] /%s/ on \"%s\": expected group0 [%s], got [%s]\n",
                   idx, c.pat, c.in, c.g0, got.c_str());
            CHECK(false);
        }
    }

    // The "\\w+" on "naïve_x" should match the ASCII run "na" (ï breaks it), then "ve_x" later.
    { Program p = compile("\\w+", 0); bool ok; CHECK(wholeMatch(p, "naïve_x", ok) == "na"); }

    // --- must-throw: the deliberately excluded constructs ---
    auto rejects = [](const char* pat) {
        try { compile(pat, 0); return false; } catch (const RegexError&) { return true; }
    };
    CHECK(rejects("(a)\\1"));        // backreference
    CHECK(rejects("(?P<n>a)(?P=n)")); // named backreference
    CHECK(rejects("(?=abc)"));        // lookahead
    CHECK(rejects("(?!abc)"));        // negative lookahead
    CHECK(rejects("(?<=abc)"));       // lookbehind
    CHECK(rejects("(?<!abc)"));       // negative lookbehind
    CHECK(rejects("a**"));            // double quantifier (nothing to repeat)
    CHECK(rejects("(?#comment)"));    // unsupported (?...) construct

    // --- structured fuzz: build random patterns over a tiny grammar; matching must always
    // terminate (linear time), and any reported match must re-fullmatch and have in-bounds groups.
    {
        std::mt19937 rng(0xA11CE);
        const char* atom[] = {"a", "b", "c", ".", "\\d", "\\w", "[abc]", "[^a]", "(a|bc)", "(?:ab)"};
        const char* quant[] = {"", "*", "+", "?", "*?", "+?", "{1,2}", "{0,3}", "{2}"};
        for (int it = 0; it < 5000; ++it) {
            std::string pat;
            int parts = 1 + static_cast<int>(rng() % 5);
            for (int i = 0; i < parts; ++i) {
                if (i && (rng() % 5 == 0)) pat += "|";
                pat += atom[rng() % 10];
                pat += quant[rng() % 9];
            }
            if (rng() % 3 == 0) pat = "^" + pat;
            if (rng() % 3 == 0) pat += "$";
            Program prog;
            try { prog = compile(pat, (rng() % 2) ? IGNORECASE : 0); }
            catch (const RegexError&) { continue; }
            std::string in;
            int len = rng() % 16;
            for (int i = 0; i < len; ++i) in += static_cast<char>('a' + (rng() % 4));
            auto text = toCodepoints(in);
            MatchResult r = run(prog, text, 0, false, false);   // must terminate
            if (r.matched) {
                int a = r.slots[0], b = r.slots[1];
                CHECK(a >= 0 && a <= b && b <= static_cast<int>(text.size()));
                for (int g = 1; g <= prog.numGroups; ++g) {
                    int ga = r.slots[2 * g], gb = r.slots[2 * g + 1];
                    if (ga >= 0) { CHECK(ga >= a && ga <= gb && gb <= b); }
                }
                std::vector<int32_t> sub(text.begin() + a, text.begin() + b);
                CHECK(run(prog, sub, 0, true, true).matched);
            }
        }
    }

    return RUN_TESTS();
}
