// embed_expr_ast.cpp — an arithmetic expression evaluator whose PARSER is Kirito. Kirito owns the
// lexer + recursive-descent parser that turns "3 + 4 * 2" into an AST of nested Dicts/Lists; C++
// owns the tree-walking evaluator that folds that AST down to a double. So C++ walks a
// Kirito-produced data structure via isDict/has/get/asStringRef/asFloat.
//
// AST node shapes (produced by Kirito, consumed by C++):
//   leaf   {"num": <Float>}
//   binary {"op": "+"|"-"|"*"|"/", "left": <node>, "right": <node>}
//
// Flow per expression: C++ (hand source String) → Kirito (tokenize + parse → AST Dict) →
// C++ (recursively evaluate the AST → double).

#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// A pure C++ tree-walker over the Kirito-produced AST. It knows nothing about how the AST was
// built — only its node protocol (a Dict with either "num" or "op"). Precedence and grouping are
// already baked into the tree by the Kirito parser, so evaluation is a plain post-order fold.
static double evalNode(Value node) {
    if (!node.isDict())
        throw KiritoError("eval: AST node must be a Dict, got '" + node.typeName() + "'");
    if (node.has("num"))
        return node.get("num").asFloat("num leaf");
    if (node.has("op")) {
        // Copy the op String out of the temporary before it dies (Value::get returns by value).
        std::string op = node.get("op").asStringRef("op");
        double l = evalNode(node.get("left"));
        double r = evalNode(node.get("right"));
        if (op == "+") return l + r;
        if (op == "-") return l - r;
        if (op == "*") return l * r;
        if (op == "/") {
            if (r == 0.0) throw KiritoError("eval: division by zero");
            return l / r;
        }
        throw KiritoError("eval: unknown op '" + op + "'");
    }
    throw KiritoError("eval: malformed node (neither 'num' nor 'op')");
}

// The Kirito parser: a lexer + a classic recursive-descent grammar
//   expr   = term  (('+' | '-') term)*      (left-assoc, lowest precedence)
//   term   = factor (('*' | '/') factor)*   (left-assoc, higher precedence)
//   factor = number | '(' expr ')'
// Mutable parse position lives in a one-element List cell `pos` so the nested closures share it.
static const char* kParserSrc = R"KI(
Function(src : String) -> Dict:
    var toks = []
    var i = 0
    var n = len(src)
    while i < n:
        var c = src[i]
        if c == " " or c == "\t":
            i = i + 1
            continue
        if c == "+" or c == "-" or c == "*" or c == "/" or c == "(" or c == ")":
            toks.append({"kind": c})
            i = i + 1
            continue
        if c >= "0" and c <= "9":
            var num = ""
            while i < n and ((src[i] >= "0" and src[i] <= "9") or src[i] == "."):
                num = num + src[i]
                i = i + 1
            toks.append({"kind": "num", "value": Float(num)})
            continue
        throw "lex error: unexpected character '" + c + "'"

    var pos = [0]
    var peek = Function():
        if pos[0] < len(toks):
            return toks[pos[0]]
        return None
    var advance = Function():
        var t = toks[pos[0]]
        pos[0] = pos[0] + 1
        return t

    var parseExpr = None
    var parseFactor = Function():
        var t = peek()
        if t == None:
            throw "parse error: unexpected end of input"
        if t["kind"] == "(":
            discard advance()
            var inner = parseExpr()
            var close = peek()
            if close == None or close["kind"] != ")":
                throw "parse error: expected ')'"
            discard advance()
            return inner
        if t["kind"] == "num":
            discard advance()
            return {"num": t["value"]}
        throw "parse error: unexpected token '" + t["kind"] + "'"
    var parseTerm = Function():
        var left = parseFactor()
        while peek() != None and (peek()["kind"] == "*" or peek()["kind"] == "/"):
            var op = advance()["kind"]
            var right = parseFactor()
            left = {"op": op, "left": left, "right": right}
        return left
    parseExpr = Function():
        var left = parseTerm()
        while peek() != None and (peek()["kind"] == "+" or peek()["kind"] == "-"):
            var op = advance()["kind"]
            var right = parseTerm()
            left = {"op": op, "left": left, "right": right}
        return left

    if len(toks) == 0:
        throw "parse error: empty expression"
    var ast = parseExpr()
    if pos[0] != len(toks):
        throw "parse error: trailing tokens after expression"
    return ast
)KI";

// A tiny front end: compile the Kirito parser once, then parse+evaluate any expression String.
class Calculator {
public:
    explicit Calculator(KiritoVM& vm) : vm_(vm), parser_(vm, vm.runSource(kParserSrc)) {}

    // Return the AST Dict for `expr` (Kirito owns this step entirely).
    Value parse(const std::string& expr) { return parser_.call({Value(vm_, expr)}); }

    // Parse then tree-walk to a double (C++ owns the walk).
    double eval(const std::string& expr) { return evalNode(parse(expr)); }

private:
    KiritoVM& vm_;
    Value     parser_;
};

int main() {
    KiritoVM vm;
    Calculator calc(vm);

    // ---- the AST really is nested Dicts/Lists that C++ can inspect ----
    {
        Value ast = calc.parse("3 + 4 * 2");
        // Top node is the '+' (lowest precedence binds last / sits at the root).
        CHECK(ast.isDict());
        CHECK(ast.has("op"));
        CHECK(ast.get("op").asStringRef("op") == "+");
        // Left of the root '+' is the leaf 3.
        Value left = ast.get("left");
        CHECK(left.has("num"));
        CHECK(left.get("num").asFloat("num") == 3.0);
        // Right of the root '+' is the '*' subtree (4 * 2) — precedence, encoded in the tree shape.
        Value right = ast.get("right");
        CHECK(right.has("op"));
        CHECK(right.get("op").asStringRef("op") == "*");
    }

    // ---- evaluation respects precedence ----
    CHECK(calc.eval("3 + 4 * 2") == 11.0);          // 3 + (4*2)
    CHECK(calc.eval("2 * 3 + 4") == 10.0);          // (2*3) + 4
    CHECK(calc.eval("10 - 2 - 3") == 5.0);          // left-associative: (10-2)-3
    CHECK(calc.eval("100 / 10 / 2") == 5.0);        // left-associative: (100/10)/2

    // ---- parentheses override precedence ----
    CHECK(calc.eval("(3 + 4) * 2") == 14.0);
    CHECK(calc.eval("2 * (3 + 4)") == 14.0);
    CHECK(calc.eval("(1 + 2) * (3 + 4)") == 21.0);
    CHECK(calc.eval("((5))") == 5.0);

    // ---- whitespace, decimals, and a single atom ----
    CHECK(calc.eval("7") == 7.0);
    CHECK(calc.eval("  6   *   7  ") == 42.0);
    CHECK(calc.eval("1.5 + 2.5") == 4.0);
    CHECK(calc.eval("3 * 4 + 5 * 6") == 42.0);

    // ---- adversarial: C++ evaluator must throw on an AST node with an unknown "op" ----
    {
        Dict lhs(vm); lhs.set("num", Value(vm, 2.0));
        Dict rhs(vm); rhs.set("num", Value(vm, 3.0));
        Dict bad(vm);
        bad.set("op",    Value(vm, "^"));                 // no such operator in the walker
        bad.set("left",  Value(vm, lhs.handle()));
        bad.set("right", Value(vm, rhs.handle()));
        CHECK_THROWS(evalNode(Value(vm, bad.handle())));
    }

    // ---- adversarial: a malformed node (neither "num" nor "op") throws ----
    {
        Dict empty(vm);
        CHECK_THROWS(evalNode(Value(vm, empty.handle())));
    }

    // ---- adversarial: a non-Dict node (a bare Integer) throws ----
    CHECK_THROWS(evalNode(Value(vm, 42)));

    // ---- adversarial: division by zero surfaces from the walker ----
    CHECK_THROWS(calc.eval("1 / 0"));

    // ---- adversarial: the Kirito parser itself rejects garbage input ----
    CHECK_THROWS(calc.parse("3 + "));                    // dangling operator
    CHECK_THROWS(calc.parse("(1 + 2"));                  // unbalanced parenthesis
    CHECK_THROWS(calc.parse("3 % 4"));                   // unknown character to the lexer
    CHECK_THROWS(calc.parse(""));                        // empty expression
    CHECK_THROWS(calc.parse("1 2"));                     // trailing token, no operator

    return RUN_TESTS();
}
