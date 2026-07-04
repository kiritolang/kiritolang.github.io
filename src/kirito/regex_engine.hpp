#ifndef KIRITO_REGEX_ENGINE_HPP
#define KIRITO_REGEX_ENGINE_HPP

#include <cstdint>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "fum/unordered_map.hpp"
#include "builtins.hpp"  // utf8Starts / utf8DecodeAt / utf8Encode / utf8ToLowerCp / utf8ToUpperCp

// A from-scratch regular-expression engine with a guaranteed LINEAR-TIME match: it compiles the
// pattern to a small bytecode program and simulates a Thompson NFA with Pike's algorithm, tracking
// capture-group positions per thread. At every input position each program counter is considered at
// most once (a visited-generation marker), so the running set of NFA threads is bounded by the
// program size — matching is O(input * program), with NO catastrophic backtracking, ever. This is
// the same engineering choice RE2 makes; consequently (and by design) the two constructs that force
// super-linear backtracking — backreferences and arbitrary lookaround — are deliberately rejected.
//
// The engine works on Unicode code points (an int32 sequence), so it composes with Kirito's
// code-point-indexed strings: every position/span it reports is a code-point index.
namespace kirito {
namespace reng {

struct RegexError : std::runtime_error {
    explicit RegexError(const std::string& m) : std::runtime_error(m) {}
};

enum Flags : int { IGNORECASE = 1, MULTILINE = 2, DOTALL = 4 };

// A character class: a union of inclusive code-point ranges, optionally negated.
struct CharClass {
    std::vector<std::pair<int32_t, int32_t>> ranges;
    bool negated = false;
};

// Compiled instruction. The op set is the classic Thompson/Pike set plus capture saves and
// zero-width assertions.
struct Inst {
    enum Op { Char, Any, Class, Match, Jmp, Split, Save, Assert } op;
    int32_t ch = 0;     // Char: the code point to match
    int klass = -1;     // Class: index into Program::classes
    int x = 0, y = 0;   // Jmp -> x; Split -> x (preferred) then y
    int slot = 0;       // Save: capture slot to record the current position into
    int assertKind = 0; // Assert: one of AssertKind
};

enum AssertKind { ABeginText, AEndText, ABeginLine, AEndLine, AWordBoundary, ANotWordBoundary };

struct Program {
    std::vector<Inst> insts;
    std::vector<CharClass> classes;
    int numGroups = 0;                                  // capturing groups, excluding the whole match
    std::vector<std::string> groupNames;                // size numGroups+1; "" when unnamed
    fum::unordered_map<std::string, int> nameToGroup;   // name -> group index (1-based)
    int flags = 0;
};

// ----------------------------------------------------------------------------- parsing -> AST
namespace detail {

inline bool isWordCp(int32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// Predefined shorthand ranges (ASCII semantics, like most engines' default).
inline std::vector<std::pair<int32_t, int32_t>> digitRanges() { return {{'0', '9'}}; }
inline std::vector<std::pair<int32_t, int32_t>> wordRanges() {
    return {{'0', '9'}, {'A', 'Z'}, {'a', 'z'}, {'_', '_'}};
}
inline std::vector<std::pair<int32_t, int32_t>> spaceRanges() {
    return {{'\t', '\r'}, {' ', ' '}};  // \t \n \v \f \r and space
}
// Complement of a set of ranges over the full code-point space [0, 0x10FFFF].
inline std::vector<std::pair<int32_t, int32_t>> complement(std::vector<std::pair<int32_t, int32_t>> rs) {
    std::sort(rs.begin(), rs.end());
    std::vector<std::pair<int32_t, int32_t>> out;
    int32_t next = 0;
    for (auto& r : rs) {
        if (r.first > next) out.push_back({next, r.first - 1});
        if (r.second + 1 > next) next = r.second + 1;
    }
    if (next <= 0x10FFFF) out.push_back({next, 0x10FFFF});
    return out;
}

struct Node {
    enum Kind { Empty, Lit, Any, Class, Concat, Alt, Star, Plus, Quest, Repeat, Group, Anchor } kind;
    int32_t ch = 0;
    int klass = -1;
    std::vector<Node> kids;
    bool greedy = true;
    int rmin = 0, rmax = 0;   // Repeat (rmax == -1 means unbounded)
    int group = -1;           // Group: capture index (>=1), or -1 for non-capturing
    int anchor = 0;           // Anchor: an AssertKind
};

// Recursive-descent parser. Tracks classes and group numbering; bounds nesting depth so a
// pathologically nested pattern throws instead of overflowing the native stack.
class Parser {
public:
    Parser(const std::vector<int32_t>& cps, int& flags, std::vector<CharClass>& classes,
           std::vector<std::string>& names, fum::unordered_map<std::string, int>& nameMap)
        : s_(cps), flags_(flags), classes_(classes), names_(names), nameMap_(nameMap) {}

    Node parse() {
        // A leading run of global inline-flag groups, e.g. (?i)(?m), applies to the whole pattern.
        Node root = parseAlternation();
        if (pos_ != s_.size()) throw RegexError("unbalanced ')' in pattern");
        return root;
    }
    int groupCount() const { return groupCounter_; }

private:
    int32_t peek() const { return pos_ < s_.size() ? s_[pos_] : -1; }
    int32_t next() { return s_[pos_++]; }
    bool eat(int32_t c) { if (peek() == c) { ++pos_; return true; } return false; }

    struct Depth {
        int& d;
        explicit Depth(int& dd) : d(dd) { if (++d > 2000) throw RegexError("pattern nested too deeply"); }
        ~Depth() { --d; }
    };

    Node parseAlternation() {
        Depth g(depth_);
        std::vector<Node> alts;
        alts.push_back(parseConcat());
        while (eat('|')) alts.push_back(parseConcat());
        if (alts.size() == 1) return std::move(alts[0]);
        Node n; n.kind = Node::Alt; n.kids = std::move(alts);
        return n;
    }

    Node parseConcat() {
        std::vector<Node> parts;
        while (pos_ < s_.size() && peek() != '|' && peek() != ')') {
            parts.push_back(parseQuantified());
        }
        if (parts.empty()) { Node e; e.kind = Node::Empty; return e; }
        if (parts.size() == 1) return std::move(parts[0]);
        Node n; n.kind = Node::Concat; n.kids = std::move(parts);
        return n;
    }

    Node parseQuantified() {
        Node atom = parseAtom();
        int32_t c = peek();
        // A quantifier needs a repeatable atom: a zero-width anchor or an empty inline-flag group
        // has "nothing to repeat". Note an empty *group* `(?:)*` is a Group node,
        // so it stays repeatable.
        bool zeroWidth = atom.kind == Node::Anchor || atom.kind == Node::Empty;
        if (zeroWidth && (c == '*' || c == '+' || c == '?'))
            throw RegexError("nothing to repeat");
        if (c == '*' || c == '+' || c == '?') {
            ++pos_;
            bool greedy = !eat('?');
            Node n;
            n.kind = (c == '*') ? Node::Star : (c == '+') ? Node::Plus : Node::Quest;
            n.greedy = greedy;
            n.kids.push_back(std::move(atom));
            return n;
        }
        if (c == '{') {
            std::size_t save = pos_;
            ++pos_;
            int lo = 0, hi = 0; bool ok = true, hasLo = false;
            while (peek() >= '0' && peek() <= '9') { lo = lo * 10 + (next() - '0'); hasLo = true; if (lo > 100000) lo = 100000; }
            if (!hasLo) ok = false;
            if (ok && eat(',')) {
                if (peek() == '}') hi = -1;  // {n,}
                else {
                    bool hasHi = false;
                    while (peek() >= '0' && peek() <= '9') { hi = hi * 10 + (next() - '0'); hasHi = true; if (hi > 100000) hi = 100000; }
                    if (!hasHi) ok = false;
                }
            } else if (ok) {
                hi = lo;  // {n}
            }
            if (ok && eat('}')) {
                if (zeroWidth) throw RegexError("nothing to repeat");
                if (hi != -1 && hi < lo) throw RegexError("bad repetition bounds {" + std::to_string(lo) + "," + std::to_string(hi) + "}");
                if (lo > 1000 || hi > 1000) throw RegexError("repetition count too large (max 1000)");
                bool greedy = !eat('?');
                Node n; n.kind = Node::Repeat; n.rmin = lo; n.rmax = hi; n.greedy = greedy;
                n.kids.push_back(std::move(atom));
                return n;
            }
            pos_ = save;  // not a valid {..}: treat '{' as a literal (lenient)
        }
        return atom;
    }

    Node parseAtom() {
        Depth g(depth_);
        int32_t c = peek();
        if (c == '(') return parseGroup();
        if (c == '[') return parseClass();
        if (c == ')') throw RegexError("unbalanced ')' in pattern");
        if (c == '*' || c == '+' || c == '?') throw RegexError("nothing to repeat");
        if (c == '.') { ++pos_; Node n; n.kind = Node::Any; return n; }
        // ^ and $ are flag-agnostic "line" anchors; whether they honor MULTILINE is decided at match
        // time (so an inline (?m) anywhere in the pattern applies). \A and \z/\Z are the absolute ones.
        if (c == '^') { ++pos_; Node n; n.kind = Node::Anchor; n.anchor = ABeginLine; return n; }
        if (c == '$') { ++pos_; Node n; n.kind = Node::Anchor; n.anchor = AEndLine; return n; }
        if (c == '\\') return parseEscape();
        ++pos_;
        Node n; n.kind = Node::Lit; n.ch = c; return n;
    }

    Node parseGroup() {
        ++pos_;  // consume '('
        int capture = -1;
        std::string name;
        if (eat('?')) {
            int32_t k = peek();
            if (k == ':') { ++pos_; }                                   // (?:...) non-capturing
            else if (k == '=' || k == '!') throw RegexError("lookahead (?= / ?!) is not supported (it would break the linear-time guarantee)");
            else if (k == 'P') {
                ++pos_;
                if (eat('=')) throw RegexError("named backreferences are not supported");
                if (!eat('<')) throw RegexError("malformed named group (expected '<')");
                name = parseGroupName('>');
                capture = ++groupCounter_;
            } else if (k == '<') {
                ++pos_;
                if (peek() == '=' || peek() == '!') throw RegexError("lookbehind (?<= / ?<!) is not supported (it would break the linear-time guarantee)");
                name = parseGroupName('>');
                capture = ++groupCounter_;
            } else {
                // inline flags, e.g. (?i) (?ms) — applied globally to the whole pattern.
                bool any = false;
                while (true) {
                    int32_t f = peek();
                    if (f == 'i') { flags_ |= IGNORECASE; ++pos_; any = true; }
                    else if (f == 'm') { flags_ |= MULTILINE; ++pos_; any = true; }
                    else if (f == 's') { flags_ |= DOTALL; ++pos_; any = true; }
                    else break;
                }
                if (!any || !eat(')')) throw RegexError("unsupported (?...) group");
                Node e; e.kind = Node::Empty; return e;
            }
        } else {
            capture = ++groupCounter_;
        }
        Node inner = parseAlternation();
        if (!eat(')')) throw RegexError("missing ) in pattern");
        Node n; n.kind = Node::Group; n.group = capture;
        n.kids.push_back(std::move(inner));
        if (capture >= 1) {
            if (static_cast<int>(names_.size()) <= capture) names_.resize(capture + 1);
            names_[capture] = name;
            if (!name.empty()) {
                if (nameMap_.count(name)) throw RegexError("duplicate group name '" + name + "'");
                nameMap_[name] = capture;
            }
        }
        return n;
    }

    std::string parseGroupName(int32_t close) {
        std::string out;
        while (pos_ < s_.size() && peek() != close) { utf8Encode(static_cast<unsigned>(next()), out); }
        if (!eat(close)) throw RegexError("malformed group name");
        if (out.empty()) throw RegexError("empty group name");
        // A group name must be an identifier: a letter or '_' then letters/digits/'_'.
        auto ident = [](unsigned char ch) {
            return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
        };
        if (!ident(static_cast<unsigned char>(out[0])))
            throw RegexError("bad group name '" + out + "'");
        for (unsigned char ch : out)
            if (!ident(ch) && !(ch >= '0' && ch <= '9'))
                throw RegexError("bad group name '" + out + "'");
        return out;
    }

    // Read an octal escape continuing from `firstVal` (the value of the first octal digit, already
    // consumed): up to two further octal digits. Octal escapes are plain literal code points.
    int32_t readOctalAfter(int32_t firstVal) {
        int32_t v = firstVal;
        for (int i = 0; i < 2; ++i) {
            int32_t d = peek();
            if (d < '0' || d > '7') break;
            v = v * 8 + (d - '0');
            ++pos_;
        }
        return v;
    }

    // Decode a single-character escape appearing INSIDE a character class. Unlike outside a class,
    // here `\b` is a backspace (not a word boundary) and `\0`-`\7` begin an octal escape.
    int32_t classSingleEscape(int32_t e) {
        if (e == 'b') return 0x08;                         // backspace
        if (e >= '0' && e <= '7') return readOctalAfter(e - '0');
        return decodeEscapeChar(e);
    }

    // \d \w \s and their negations, anchors, and escaped characters.
    Node parseEscape() {
        ++pos_;  // consume '\'
        if (pos_ >= s_.size()) throw RegexError("trailing backslash in pattern");
        int32_t c = next();
        switch (c) {
            case 'd': { return classNode(digitRanges(), false); } break;
            case 'D': { return classNode(digitRanges(), true); } break;
            case 'w': { return classNode(wordRanges(), false); } break;
            case 'W': { return classNode(wordRanges(), true); } break;
            case 's': { return classNode(spaceRanges(), false); } break;
            case 'S': { return classNode(spaceRanges(), true); } break;
            case 'b': { return anchorNode(AWordBoundary); } break;
            case 'B': { return anchorNode(ANotWordBoundary); } break;
            case 'A': { return anchorNode(ABeginText); } break;
            case 'Z': case 'z': { return anchorNode(AEndText); } break;
            default: { break; } break;
        }
        if (c >= '1' && c <= '9') throw RegexError("backreferences are not supported (they would break the linear-time guarantee)");
        Node n; n.kind = Node::Lit; n.ch = decodeEscapeChar(c); return n;
    }

    int32_t decodeEscapeChar(int32_t c) {
        switch (c) {
            case 'n': { return '\n'; } break; case 't': { return '\t'; } break; case 'r': { return '\r'; } break;
            case 'f': { return '\f'; } break; case 'v': { return '\v'; } break; case 'a': { return '\a'; } break;
            case '0': { return readOctalAfter(0); } break;   // \0, \012, ... octal escape
            case 'x': { return readHex(2); } break;
            case 'u': { return readHex(4); } break;
            case 'U': { return readHex(8); } break;
            default: { return c; } break;  // an escaped metacharacter or any other char is itself
        }
    }
    int32_t readHex(int n) {
        uint32_t v = 0;  // unsigned: \U takes 8 hex digits, which overflows a signed int32 (UB)
        for (int i = 0; i < n; ++i) {
            int32_t h = peek(); uint32_t d;
            if (h >= '0' && h <= '9') d = static_cast<uint32_t>(h - '0');
            else if (h >= 'a' && h <= 'f') d = static_cast<uint32_t>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') d = static_cast<uint32_t>(h - 'A' + 10);
            else throw RegexError("incomplete \\x/\\u escape");
            v = v * 16u + d; ++pos_;
        }
        if (v > 0x10FFFF) throw RegexError("escape value out of range (above U+10FFFF)");
        // A lone UTF-16 surrogate is not a valid scalar value; it can never match well-formed input
        // (a silently-dead pattern element), so reject it like Python's re does.
        if (v >= 0xD800 && v <= 0xDFFF) throw RegexError("escape is a UTF-16 surrogate (not a scalar code point)");
        return static_cast<int32_t>(v);
    }

    Node anchorNode(int kind) { Node n; n.kind = Node::Anchor; n.anchor = kind; return n; }
    Node classNode(std::vector<std::pair<int32_t, int32_t>> ranges, bool negated) {
        CharClass cc; cc.ranges = std::move(ranges); cc.negated = negated;
        int idx = static_cast<int>(classes_.size());
        classes_.push_back(std::move(cc));
        Node n; n.kind = Node::Class; n.klass = idx; return n;
    }

    Node parseClass() {
        ++pos_;  // consume '['
        CharClass cc;
        if (eat('^')) cc.negated = true;
        bool first = true;
        while (true) {
            if (pos_ >= s_.size()) throw RegexError("unterminated character class");
            int32_t c = peek();
            if (c == ']' && !first) { ++pos_; break; }
            first = false;
            // a shorthand inside the class contributes its ranges
            if (c == '\\') {
                ++pos_;
                if (pos_ >= s_.size()) throw RegexError("trailing backslash in character class");
                int32_t e = next();
                if (e == 'd') { addRanges(cc, digitRanges()); continue; }
                if (e == 'w') { addRanges(cc, wordRanges()); continue; }
                if (e == 's') { addRanges(cc, spaceRanges()); continue; }
                if (e == 'D') { addRanges(cc, complement(digitRanges())); continue; }
                if (e == 'W') { addRanges(cc, complement(wordRanges())); continue; }
                if (e == 'S') { addRanges(cc, complement(spaceRanges())); continue; }
                c = classSingleEscape(e);
            } else {
                ++pos_;
            }
            // a range a-b (but a trailing '-' before ']' is a literal '-')
            if (peek() == '-' && pos_ + 1 < s_.size() && s_[pos_ + 1] != ']') {
                ++pos_;  // consume '-'
                int32_t hi = peek();
                if (hi == '\\') { ++pos_; hi = classSingleEscape(next()); }
                else ++pos_;
                if (hi < c) throw RegexError("bad character range in class");
                cc.ranges.push_back({c, hi});
            } else {
                cc.ranges.push_back({c, c});
            }
        }
        int idx = static_cast<int>(classes_.size());
        classes_.push_back(std::move(cc));
        Node n; n.kind = Node::Class; n.klass = idx; return n;
    }
    static void addRanges(CharClass& cc, const std::vector<std::pair<int32_t, int32_t>>& rs) {
        cc.ranges.insert(cc.ranges.end(), rs.begin(), rs.end());
    }

    const std::vector<int32_t>& s_;
    std::size_t pos_ = 0;
    int depth_ = 0;
    int groupCounter_ = 0;
    int& flags_;
    std::vector<CharClass>& classes_;
    std::vector<std::string>& names_;
    fum::unordered_map<std::string, int>& nameMap_;
};

// --------------------------------------------------------------------------- AST -> bytecode
class Compiler {
public:
    explicit Compiler(Program& p) : p_(p) {}

    void compileTop(const Node& root) {
        emit({Inst::Save, 0, -1, 0, 0, 0, 0});            // slot 0 = whole-match start
        compile(root);
        emit({Inst::Save, 0, -1, 0, 0, 1, 0});            // slot 1 = whole-match end
        emit({Inst::Match, 0, -1, 0, 0, 0, 0});
    }

private:
    int emit(Inst i) {
        if (p_.insts.size() >= 200000) throw RegexError("compiled pattern too large");
        p_.insts.push_back(i);
        return static_cast<int>(p_.insts.size()) - 1;
    }
    int here() const { return static_cast<int>(p_.insts.size()); }

    void compile(const Node& n) {
        switch (n.kind) {
            case Node::Empty: { break; } break;
            case Node::Lit: { emit({Inst::Char, n.ch, -1, 0, 0, 0, 0}); } break;
            case Node::Any: { emit({Inst::Any, 0, -1, 0, 0, 0, 0}); } break;
            case Node::Class: { emit({Inst::Class, 0, n.klass, 0, 0, 0, 0}); } break;
            case Node::Anchor: { emit({Inst::Assert, 0, -1, 0, 0, 0, n.anchor}); } break;
            case Node::Concat: { for (const Node& k : n.kids) compile(k); } break;
            case Node::Alt: { compileAlt(n.kids); } break;
            case Node::Star: { compileStar(n.kids[0], n.greedy); } break;
            case Node::Plus: { compilePlus(n.kids[0], n.greedy); } break;
            case Node::Quest: { compileQuest(n.kids[0], n.greedy); } break;
            case Node::Repeat: { compileRepeat(n); } break;
            case Node::Group: { compileGroup(n); } break;
        }
    }

    void compileAlt(const std::vector<Node>& alts) {
        // split A, next ; A: <alt0> ; jmp END ; next: ... ; last alt has no split/jmp ; END:
        std::vector<int> jmpsToEnd;
        for (std::size_t i = 0; i + 1 < alts.size(); ++i) {
            int sp = emit({Inst::Split, 0, -1, 0, 0, 0, 0});
            p_.insts[sp].x = here();
            compile(alts[i]);
            jmpsToEnd.push_back(emit({Inst::Jmp, 0, -1, 0, 0, 0, 0}));
            p_.insts[sp].y = here();
        }
        compile(alts.back());
        for (int j : jmpsToEnd) p_.insts[j].x = here();
    }

    void compileStar(const Node& body, bool greedy) {
        int sp = emit({Inst::Split, 0, -1, 0, 0, 0, 0});
        int bodyStart = here();
        compile(body);
        emit({Inst::Jmp, 0, -1, sp, 0, 0, 0});
        int end = here();
        if (greedy) { p_.insts[sp].x = bodyStart; p_.insts[sp].y = end; }
        else        { p_.insts[sp].x = end; p_.insts[sp].y = bodyStart; }
    }
    void compilePlus(const Node& body, bool greedy) {
        int bodyStart = here();
        compile(body);
        int sp = emit({Inst::Split, 0, -1, 0, 0, 0, 0});
        int end = here();
        if (greedy) { p_.insts[sp].x = bodyStart; p_.insts[sp].y = end; }
        else        { p_.insts[sp].x = end; p_.insts[sp].y = bodyStart; }
    }
    void compileQuest(const Node& body, bool greedy) {
        int sp = emit({Inst::Split, 0, -1, 0, 0, 0, 0});
        int bodyStart = here();
        compile(body);
        int end = here();
        if (greedy) { p_.insts[sp].x = bodyStart; p_.insts[sp].y = end; }
        else        { p_.insts[sp].x = end; p_.insts[sp].y = bodyStart; }
    }
    void compileRepeat(const Node& n) {
        const Node& body = n.kids[0];
        for (int i = 0; i < n.rmin; ++i) compile(body);          // n mandatory copies
        if (n.rmax == -1) {
            if (n.rmin == 0) compileStar(body, n.greedy);
            else compileStar(body, n.greedy);                    // n copies above, then * for the rest
        } else {
            for (int i = n.rmin; i < n.rmax; ++i) compileQuest(body, n.greedy);  // up to (m-n) optionals
        }
    }
    void compileGroup(const Node& n) {
        if (n.group >= 1) {
            emit({Inst::Save, 0, -1, 0, 0, 2 * n.group, 0});
            compile(n.kids[0]);
            emit({Inst::Save, 0, -1, 0, 0, 2 * n.group + 1, 0});
        } else {
            compile(n.kids[0]);
        }
    }

    Program& p_;
};

}  // namespace detail

// Compile a UTF-8 pattern into a Program. Throws RegexError on a syntax error.
inline Program compile(const std::string& patternUtf8, int flags) {
    std::vector<int32_t> cps;
    for (std::size_t st : utf8Starts(patternUtf8)) cps.push_back(static_cast<int32_t>(utf8DecodeAt(patternUtf8, st)));
    Program prog;
    prog.flags = flags;
    detail::Parser parser(cps, prog.flags, prog.classes, prog.groupNames, prog.nameToGroup);
    detail::Node root = parser.parse();
    prog.numGroups = parser.groupCount();
    // Each NFA thread carries a per-group capture-position vector (size 2*(numGroups+1)) that is copied
    // as threads split/save. A pattern with a huge number of groups (e.g. `"()"*99000`) makes that copy
    // dominate — turning the O(input * program) guarantee into O(input * program * numGroups) and
    // hanging the Pike VM even on a 1-byte subject. Cap the group count so the per-thread copy stays
    // bounded; 1000 is far beyond any realistic pattern. (Rejected here at compile time, before the VM
    // ever runs, so the blow-up is unreachable.)
    if (prog.numGroups > 1000) throw RegexError("too many capture groups (max 1000)");
    if (static_cast<int>(prog.groupNames.size()) <= prog.numGroups) prog.groupNames.resize(prog.numGroups + 1);
    detail::Compiler comp(prog);
    comp.compileTop(root);
    return prog;
}

// ------------------------------------------------------------------------------- the Pike VM
namespace detail {

inline bool classMatches(const CharClass& cc, int32_t c, bool ignorecase) {
    auto raw = [&](int32_t x) {
        for (auto& r : cc.ranges) if (x >= r.first && x <= r.second) return true;
        return false;
    };
    bool in = raw(c);
    if (!in && ignorecase) {
        int32_t lo = static_cast<int32_t>(utf8ToLowerCp(static_cast<unsigned>(c)));
        int32_t up = static_cast<int32_t>(utf8ToUpperCp(static_cast<unsigned>(c)));
        in = raw(lo) || raw(up);
    }
    return cc.negated ? !in : in;
}

inline bool charEq(int32_t a, int32_t b, bool ignorecase) {
    if (a == b) return true;
    if (!ignorecase) return false;
    return utf8ToLowerCp(static_cast<unsigned>(a)) == utf8ToLowerCp(static_cast<unsigned>(b));
}

inline bool assertHolds(int kind, const std::vector<int32_t>& t, int sp, int flags) {
    int n = static_cast<int>(t.size());
    switch (kind) {
        case ABeginText: { return sp == 0; } break;                      // \A — absolute start
        case AEndText:   { return sp == n; } break;                      // \z / \Z — absolute end
        case ABeginLine: {                                      // ^
            if (flags & MULTILINE) return sp == 0 || t[sp - 1] == '\n';
            return sp == 0;
        } break;
        case AEndLine: {                                        // $ — end, or just before a final newline
            if (flags & MULTILINE) return sp == n || t[sp] == '\n';
            return sp == n || (sp == n - 1 && t[sp] == '\n');
        } break;
        case AWordBoundary: {
            bool a = sp > 0 && isWordCp(t[sp - 1]);
            bool b = sp < n && isWordCp(t[sp]);
            return a != b;
        } break;
        case ANotWordBoundary: {
            bool a = sp > 0 && isWordCp(t[sp - 1]);
            bool b = sp < n && isWordCp(t[sp]);
            return a == b;
        } break;
    }
    return false;
}

struct Thread { int pc; std::vector<int> caps; };

// Follow all epsilon transitions from `pc0` and append the reachable consuming/Match instructions to
// `list`, in priority order. An explicit stack (not recursion) keeps a huge program from overflowing
// the native stack; `visited`/`gen` ensures each pc is added at most once per step (the bound that
// makes the whole simulation linear).
inline void addThread(const Program& prog, const std::vector<int32_t>& text, int sp,
                      std::vector<Thread>& list, std::vector<int>& visited, int gen,
                      int pc0, std::vector<int> caps0) {
    std::vector<Thread> stack;
    stack.push_back({pc0, std::move(caps0)});
    while (!stack.empty()) {
        Thread fr = std::move(stack.back());
        stack.pop_back();
        if (visited[fr.pc] == gen) continue;
        visited[fr.pc] = gen;
        const Inst& in = prog.insts[fr.pc];
        switch (in.op) {
            case Inst::Jmp: {
                stack.push_back({in.x, std::move(fr.caps)});
            } break;
            case Inst::Split: {
                stack.push_back({in.y, fr.caps});                 // lower priority pushed first
                stack.push_back({in.x, std::move(fr.caps)});      // higher priority popped first
            } break;
            case Inst::Save: {
                std::vector<int> c = fr.caps;
                if (in.slot >= 0 && in.slot < static_cast<int>(c.size())) c[in.slot] = sp;
                stack.push_back({fr.pc + 1, std::move(c)});
            } break;
            case Inst::Assert: {
                if (assertHolds(in.assertKind, text, sp, prog.flags))
                    stack.push_back({fr.pc + 1, std::move(fr.caps)});
            } break;
            default: {  // Char / Any / Class / Match — these consume (or finish); record the thread
                list.push_back({fr.pc, std::move(fr.caps)});
            } break;
        }
    }
}

}  // namespace detail

// The result of a successful run: capture slots in code-point indices, slots[2g]/slots[2g+1] the
// start/end of group g (g==0 is the whole match); -1 means the group didn't participate.
struct MatchResult {
    bool matched = false;
    std::vector<int> slots;
};

// Run the program over `text` (code points). startPos is a code-point index.
//   anchored   — the match must begin exactly at startPos (re.match); else it may begin anywhere
//                at or after startPos (re.search), preferring the leftmost start.
//   requireEnd — the match must reach the end of text (re.fullmatch).
// Linear time: O(text.size() * program.size()).
inline MatchResult run(const Program& prog, const std::vector<int32_t>& text,
                       int startPos, bool anchored, bool requireEnd) {
    using namespace detail;
    int n = static_cast<int>(text.size());
    int m = static_cast<int>(prog.insts.size());
    std::vector<int> visited(m, -1);
    int gen = 0;
    int capN = 2 * (prog.numGroups + 1);
    std::vector<int> init(capN, -1);

    std::vector<Thread> clist, nlist;
    clist.reserve(m); nlist.reserve(m);
    int curGen = ++gen;
    addThread(prog, text, startPos, clist, visited, curGen, 0, init);

    MatchResult best;
    for (int sp = startPos;; ++sp) {
        if (clist.empty() && (best.matched || anchored)) break;
        int nextGen = ++gen;
        nlist.clear();
        bool cut = false;
        for (std::size_t ti = 0; ti < clist.size() && !cut; ++ti) {
            Thread& t = clist[ti];
            const Inst& in = prog.insts[t.pc];
            switch (in.op) {
                case Inst::Char: {
                    if (sp < n && charEq(text[sp], in.ch, prog.flags & IGNORECASE))
                        addThread(prog, text, sp + 1, nlist, visited, nextGen, t.pc + 1, t.caps);
                } break;
                case Inst::Any: {
                    if (sp < n && ((prog.flags & DOTALL) || text[sp] != '\n'))
                        addThread(prog, text, sp + 1, nlist, visited, nextGen, t.pc + 1, t.caps);
                } break;
                case Inst::Class: {
                    if (sp < n && classMatches(prog.classes[in.klass], text[sp], prog.flags & IGNORECASE))
                        addThread(prog, text, sp + 1, nlist, visited, nextGen, t.pc + 1, t.caps);
                } break;
                case Inst::Match: {
                    if (!requireEnd || sp == n) {
                        best.matched = true;
                        best.slots = t.caps;
                        cut = true;  // lower-priority threads can't beat this one; stop scanning clist
                    }
                } break;
                default: { break; } break;  // epsilon ops never appear in a thread list
            }
        }
        std::swap(clist, nlist);
        curGen = nextGen;
        if (sp >= n) break;
        // For a search, keep trying to start a new match (lowest priority) until one is found.
        if (!anchored && !best.matched)
            addThread(prog, text, sp + 1, clist, visited, curGen, 0, init);
    }
    return best;
}

// Decode a UTF-8 string into code points (for the module layer).
inline std::vector<int32_t> toCodepoints(const std::string& s) {
    std::vector<int32_t> out;
    for (std::size_t st : utf8Starts(s)) out.push_back(static_cast<int32_t>(utf8DecodeAt(s, st)));
    return out;
}

}  // namespace reng
}  // namespace kirito

#endif
