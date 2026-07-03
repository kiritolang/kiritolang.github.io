// embed_dsl.cpp — a mini expression DSL. C++ owns the tokenizer, Pratt parser, and evaluator for
// a small language of Integers and identifiers with `+ - * /`, parentheses, and function calls.
// The FUNCTION table is populated from Kirito — every "function" the expression can call is a
// Kirito Function(args: List) -> Any. The evaluator therefore delegates every real computation
// to Kirito once it has assembled the argument list, which is the sort of embed pattern you'd
// use for a formula-language / spreadsheet cell.
//
// Flow: C++ (tokenise) -> C++ (Pratt-parse to AST) -> C++ (walk AST) -> Kirito (function body) ->
// C++ (aggregate) -> Kirito (next function) -> C++ (final value).

#include <cctype>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

struct Tok { enum K { Num, Ident, Plus, Minus, Star, Slash, LParen, RParen, Comma, End } k; std::string s; int64_t n = 0; };

class DslLexer {
public:
    explicit DslLexer(const std::string& s) : s_(s) {}
    Tok next() {
        while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
        if (i_ >= s_.size()) return {Tok::End, ""};
        char c = s_[i_];
        if (std::isdigit(static_cast<unsigned char>(c))) {
            std::size_t j = i_;
            while (j < s_.size() && std::isdigit(static_cast<unsigned char>(s_[j]))) ++j;
            Tok t{Tok::Num, s_.substr(i_, j - i_), std::stoll(s_.substr(i_, j - i_))};
            i_ = j;
            return t;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::size_t j = i_;
            while (j < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[j])) || s_[j] == '_')) ++j;
            Tok t{Tok::Ident, s_.substr(i_, j - i_)};
            i_ = j;
            return t;
        }
        ++i_;
        switch (c) {
            case '+': return {Tok::Plus,   "+"};
            case '-': return {Tok::Minus,  "-"};
            case '*': return {Tok::Star,   "*"};
            case '/': return {Tok::Slash,  "/"};
            case '(': return {Tok::LParen, "("};
            case ')': return {Tok::RParen, ")"};
            case ',': return {Tok::Comma,  ","};
            default:  throw std::runtime_error(std::string("dsl: unexpected char '") + c + "'");
        }
    }
private:
    std::string s_;
    std::size_t i_ = 0;
};

// Simple AST as a tagged union.
struct Ast {
    enum K { N, Bin, Call } k;
    int64_t n = 0;
    char op = 0;
    std::string fn;
    std::unique_ptr<Ast> lhs, rhs;
    std::vector<std::unique_ptr<Ast>> args;
};

class DslParser {
public:
    explicit DslParser(DslLexer& l) : l_(l) { cur_ = l_.next(); }
    std::unique_ptr<Ast> parseExpr() {
        auto e = parseSum();
        if (cur_.k != Tok::End) throw std::runtime_error("dsl: trailing input");
        return e;
    }
private:
    void eat() { cur_ = l_.next(); }
    std::unique_ptr<Ast> parseSum() {
        auto lhs = parseProd();
        while (cur_.k == Tok::Plus || cur_.k == Tok::Minus) {
            char op = cur_.k == Tok::Plus ? '+' : '-';
            eat();
            auto rhs = parseProd();
            auto n = std::make_unique<Ast>();
            n->k = Ast::Bin; n->op = op; n->lhs = std::move(lhs); n->rhs = std::move(rhs);
            lhs = std::move(n);
        }
        return lhs;
    }
    std::unique_ptr<Ast> parseProd() {
        auto lhs = parseAtom();
        while (cur_.k == Tok::Star || cur_.k == Tok::Slash) {
            char op = cur_.k == Tok::Star ? '*' : '/';
            eat();
            auto rhs = parseAtom();
            auto n = std::make_unique<Ast>();
            n->k = Ast::Bin; n->op = op; n->lhs = std::move(lhs); n->rhs = std::move(rhs);
            lhs = std::move(n);
        }
        return lhs;
    }
    std::unique_ptr<Ast> parseAtom() {
        if (cur_.k == Tok::Num) { auto n = std::make_unique<Ast>(); n->k = Ast::N; n->n = cur_.n; eat(); return n; }
        if (cur_.k == Tok::Ident) {
            std::string name = cur_.s;
            eat();
            if (cur_.k == Tok::LParen) {
                eat();
                auto n = std::make_unique<Ast>();
                n->k = Ast::Call; n->fn = name;
                if (cur_.k != Tok::RParen) {
                    while (true) {
                        n->args.push_back(parseSum());
                        if (cur_.k == Tok::Comma) { eat(); continue; }
                        break;
                    }
                }
                if (cur_.k != Tok::RParen) throw std::runtime_error("dsl: expected ')' in call");
                eat();
                return n;
            }
            // Bare identifier is a zero-arg function call.
            auto n = std::make_unique<Ast>();
            n->k = Ast::Call; n->fn = name;
            return n;
        }
        if (cur_.k == Tok::LParen) {
            eat();
            auto e = parseSum();
            if (cur_.k != Tok::RParen) throw std::runtime_error("dsl: expected ')'");
            eat();
            return e;
        }
        if (cur_.k == Tok::Minus) {
            eat();
            auto rhs = parseAtom();
            auto zero = std::make_unique<Ast>(); zero->k = Ast::N; zero->n = 0;
            auto n = std::make_unique<Ast>(); n->k = Ast::Bin; n->op = '-'; n->lhs = std::move(zero); n->rhs = std::move(rhs);
            return n;
        }
        throw std::runtime_error("dsl: expected expression");
    }
    DslLexer& l_;
    Tok cur_;
};

class Env {
public:
    explicit Env(KiritoVM& vm) : vm_(vm) {}
    void define(const std::string& name, Handle fn) { fns_[name] = fn; }
    // Evaluate to a Handle (whichever Kirito value the last function returned, or an Integer for
    // constants). Delegates arithmetic operators to Kirito too — so `+` on user-defined types
    // would just work (if we ever added them).
    Handle eval(const Ast* a) {
        switch (a->k) {
            case Ast::N: return val(vm_, a->n).handle();
            case Ast::Bin: {
                Handle l = eval(a->lhs.get());
                Handle r = eval(a->rhs.get());
                std::string name = std::string("__op") + a->op;
                auto it = fns_.find(name);
                if (it == fns_.end()) throw std::runtime_error(std::string("dsl: no operator ") + a->op);
                RootScope rs(vm_);
                Handle f = rs.add(it->second);
                List L(vm_); L.add(l); L.add(r);
                std::array<Handle, 1> args{rs.add(L.build().handle())};
                return vm_.arena().deref(f).call(vm_, args);
            }
            case Ast::Call: {
                auto it = fns_.find(a->fn);
                if (it == fns_.end()) throw std::runtime_error("dsl: unknown '" + a->fn + "'");
                RootScope rs(vm_);
                Handle f = rs.add(it->second);
                List L(vm_);
                for (const auto& c : a->args) L.add(rs.add(eval(c.get())));
                std::array<Handle, 1> args{rs.add(L.build().handle())};
                return vm_.arena().deref(f).call(vm_, args);
            }
        }
        throw std::runtime_error("dsl: unreachable");
    }
private:
    KiritoVM& vm_;
    std::unordered_map<std::string, Handle> fns_;
};

int main() {
    KiritoVM vm;
    Env env(vm);
    auto compile = [&](const char* s) { return vm.runSource(s); };

    // Operators — all four fold a 2-element list.
    env.define("__op+", compile("Function(a): return a[0] + a[1]\n"));
    env.define("__op-", compile("Function(a): return a[0] - a[1]\n"));
    env.define("__op*", compile("Function(a): return a[0] * a[1]\n"));
    env.define("__op/", compile("Function(a): return a[0] // a[1]\n"));    // floor div for the test

    // Some named functions.
    env.define("pi",  compile("Function(_): return 314\n"));                            // "pi × 100"
    env.define("min", compile(R"KI(
Function(a):
    var m = a[0]
    for x in a:
        if x < m:
            m = x
    return m
)KI"));
    env.define("max", compile(R"KI(
Function(a):
    var m = a[0]
    for x in a:
        if x > m:
            m = x
    return m
)KI"));
    env.define("clamp", compile(R"KI(
Function(a):
    var x = a[0]
    var lo = a[1]
    var hi = a[2]
    if x < lo:
        return lo
    if x > hi:
        return hi
    return x
)KI"));

    auto eval = [&](const char* src) {
        DslLexer l(src);
        DslParser p(l);
        auto ast = p.parseExpr();
        return Value(vm, env.eval(ast.get()));
    };

    CHECK(eval("1 + 2 * 3").asInt("") == 7);
    CHECK(eval("(1 + 2) * 3").asInt("") == 9);
    CHECK(eval("100 - 30 - 20").asInt("") == 50);
    CHECK(eval("15 / 4").asInt("") == 3);           // // floor
    CHECK(eval("-7 + 10").asInt("") == 3);
    CHECK(eval("pi").asInt("") == 314);              // bare identifier
    CHECK(eval("pi() + 1").asInt("") == 315);        // pi() with explicit ()
    CHECK(eval("min(3, 1, 4, 1, 5, 9)").asInt("") == 1);
    CHECK(eval("max(3, 1, 4, 1, 5, 9)").asInt("") == 9);
    CHECK(eval("clamp(200, 0, 100)").asInt("") == 100);
    CHECK(eval("clamp(-50, 0, 100)").asInt("") == 0);
    CHECK(eval("clamp(42, 0, 100)").asInt("") == 42);
    CHECK(eval("max(1, min(10, 5))").asInt("") == 5);   // nested call

    // ---- adversarial ----
    // unknown identifier
    CHECK_THROWS(eval("nope()"));
    // trailing input
    CHECK_THROWS(eval("1 + 2 3"));
    // unmatched paren
    CHECK_THROWS(eval("(1 + 2"));
    // Kirito UDF that throws propagates
    env.define("kaboom", compile("Function(a): throw \"kaboom!\"\n"));
    CHECK_THROWS(eval("1 + kaboom()"));
    // Integer division by zero throws from the Kirito // operator
    CHECK_THROWS(eval("10 / 0"));

    return RUN_TESTS();
}
