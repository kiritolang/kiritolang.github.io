// embed_calc.cpp — an extensible RPN calculator. The stack, tokenizer, and dispatch loop live in
// C++; every OPERATOR is a Kirito Function(stack : List) -> Any that pops what it wants + pushes
// its result. Users can register new operators from Kirito source at runtime — including ones
// that call BACK into C++ helpers (e.g. `sqrt`, `abs`) exposed via a native module.
//
// Flow per token: C++ (scan) → C++ (dispatch by name) → Kirito (operator body) → C++ (push result).
// Verifies a battery of expressions plus adversarial cases (unknown operator, stack underflow,
// operator that throws, wrong-typed operand).

#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// Split a String into whitespace-separated tokens.
static std::vector<std::string> tokenize(const std::string& src) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < src.size()) {
        while (i < src.size() && std::isspace(static_cast<unsigned char>(src[i]))) ++i;
        std::size_t j = i;
        while (j < src.size() && !std::isspace(static_cast<unsigned char>(src[j]))) ++j;
        if (j > i) out.emplace_back(src.substr(i, j - i));
        i = j;
    }
    return out;
}

// A token that looks like a number goes onto the stack as-is; otherwise it dispatches to the
// operator table. Returns the top of the stack at the end.
class Calc {
public:
    explicit Calc(KiritoVM& vm) : vm_(vm) {}

    // Register a Kirito operator: name -> Function(stack : List) -> Any.
    void registerOp(const std::string& name, Handle fn) { ops_[name] = fn; }

    // Run one RPN expression. Returns the final top-of-stack.
    Value run(const std::string& src) {
        // The "stack" is a Kirito List — operators can push and pop with the built-in list methods.
        List stackB(vm_);
        Handle stackH = stackB.build().handle();
        RootScope rs(vm_);
        Handle stackR = rs.add(stackH);
        for (const auto& tok : tokenize(src)) {
            if (isNumber(tok)) {
                // Push onto the stack. `Value::at()` reads; there is no in-place setter, so push
                // through the low-level ListVal::elems vector (the arena hands us a mutable ref).
                auto& lst = static_cast<ListVal&>(vm_.arena().deref(stackR));
                Handle vH = std::string(".").compare(&tok[tok.find_first_of(".eE")]) == 0
                                ? val(vm_, std::stod(tok)).handle()
                                : (isFloatToken(tok) ? val(vm_, std::stod(tok)).handle()
                                                     : val(vm_, static_cast<int64_t>(std::stoll(tok))).handle());
                lst.elems.push_back(vH);
                continue;
            }
            auto it = ops_.find(tok);
            if (it == ops_.end())
                throw KiritoError("calc: unknown operator '" + tok + "'");
            std::array<Handle, 1> args{stackR};
            (void)vm_.arena().deref(it->second).call(vm_, args);
            // The operator mutated the stack in place; we discard its return value.
        }
        Value stackV(vm_, stackR);
        if (stackV.len() == 0)
            throw KiritoError("calc: empty stack at end of expression");
        return stackV.at(-1);
    }

private:
    static bool isFloatToken(const std::string& t) {
        return t.find('.') != std::string::npos || t.find('e') != std::string::npos || t.find('E') != std::string::npos;
    }
    static bool isNumber(const std::string& t) {
        if (t.empty()) return false;
        std::size_t i = 0;
        if (t[i] == '-' || t[i] == '+') ++i;
        if (i == t.size()) return false;
        bool sawDigit = false, sawDot = false, sawE = false;
        for (; i < t.size(); ++i) {
            char c = t[i];
            if (c >= '0' && c <= '9') sawDigit = true;
            else if (c == '.' && !sawDot && !sawE) sawDot = true;
            else if ((c == 'e' || c == 'E') && !sawE) sawE = true;
            else if ((c == '+' || c == '-') && sawE && (t[i - 1] == 'e' || t[i - 1] == 'E')) { /* ok */ }
            else return false;
        }
        return sawDigit;
    }

    KiritoVM& vm_;
    std::unordered_map<std::string, Handle> ops_;
};

// A tiny native module the Kirito operators can call for numeric helpers. Shows the round-trip
// C++ → Kirito (operator) → C++ (helper) → Kirito (compute) → C++ (push).
struct CalcModule : NativeModule {
    std::string name() const override { return "calc"; }
    void setup(ModuleBuilder& m) override {
        m.fn("abs", {{"x", "Number"}}, "Float",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                 Args args(vm, a, "abs");
                 double d = args.at(0).asFloat("x");
                 return val(vm, d < 0.0 ? -d : d);
             });
    }
};

int main() {
    KiritoVM vm;
    vm.install<CalcModule>();
    Calc c(vm);

    auto compile = [&](const char* src) { return vm.runSource(src); };
    // A helper for the arithmetic ops: pops b then a, pushes a OP b. Kirito has no `;` — split
    // multi-statement bodies over multiple lines.
    auto mkArith = [&](const char* op) {
        std::string src = "Function(s):\n    var b = s.pop()\n    var a = s.pop()\n    s.append(a " + std::string(op) + " b)\n";
        return compile(src.c_str());
    };
    c.registerOp("+", mkArith("+"));
    c.registerOp("-", mkArith("-"));
    c.registerOp("*", mkArith("*"));
    c.registerOp("/", mkArith("/"));
    c.registerOp("neg", compile("Function(s):\n    var x = s.pop()\n    s.append(0 - x)\n"));
    c.registerOp("dup", compile("Function(s):\n    s.append(s[-1])\n"));
    c.registerOp("swap", compile(R"KI(
Function(s):
    var b = s.pop()
    var a = s.pop()
    s.append(b)
    s.append(a)
)KI"));
    c.registerOp("abs", compile(R"KI(
var calc = import("calc")
Function(s):
    var x = s.pop()
    s.append(calc.abs(x))
)KI"));

    // ---- basic arithmetic ----
    CHECK(c.run("3 4 +").asInt("") == 7);
    CHECK(c.run("10 2 -").asInt("") == 8);
    CHECK(c.run("6 7 *").asInt("") == 42);
    // ---- true division: `/` yields Float ----
    {
        Value r = c.run("15 2 /");
        CHECK(r.isFloat());
        CHECK(std::abs(r.asFloat("") - 7.5) < 1e-9);
    }
    // ---- multi-token, longer expression ----
    CHECK(c.run("3 4 + 2 *").asInt("") == 14);        // (3+4)*2 = 14
    CHECK(c.run("100 5 4 * -").asInt("") == 80);      // 100 - (5*4) = 80
    // ---- Float literals + calc.abs (calls back into C++) ----
    {
        Value r = c.run("-3.5 abs");
        CHECK(r.isFloat());
        CHECK(std::abs(r.asFloat("") - 3.5) < 1e-9);
    }
    // ---- Kirito-side stack manipulation: dup, swap, neg ----
    CHECK(c.run("5 dup +").asInt("") == 10);
    CHECK(c.run("1 2 swap -").asInt("") == 1);
    CHECK(c.run("7 neg").asInt("") == -7);

    // ---- register a NEW operator at runtime + use it: `sq` = a * a
    c.registerOp("sq", compile("Function(s):\n    var x = s.pop()\n    s.append(x * x)\n"));
    CHECK(c.run("6 sq").asInt("") == 36);
    CHECK(c.run("3 sq 4 sq +").asInt("") == 25);   // 3² + 4² = 25

    // ---- adversarial: unknown operator + stack underflow + operator that throws ----
    CHECK_THROWS(c.run("3 4 bogus"));                 // unknown operator
    CHECK_THROWS(c.run("+"));                          // stack underflow
    CHECK_THROWS(c.run("1 2 /").asString(""));        // asString on a Float throws (not a semantic bug)
    CHECK_THROWS(c.run(""));                           // empty stack at end
    // An operator that throws inside its Kirito body surfaces cleanly.
    c.registerOp("boom", compile("Function(s): throw \"kaboom\"\n"));
    CHECK_THROWS(c.run("1 2 boom"));

    // ---- wrong-typed operand: adding a String to an Integer ----
    c.registerOp("push_hi", compile("Function(s): s.append(\"hi\")\n"));
    CHECK_THROWS(c.run("1 push_hi +"));

    return RUN_TESTS();
}
