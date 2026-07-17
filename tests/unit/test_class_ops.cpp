#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

int main() {
    KiritoVM vm;

    // _init_ runs on construction
    CHECK(evalStr(vm, R"(
class P:
    var _init_ = Function(self, x):
        self.x = x
P(7).x
)") == "7");

    // _str_ controls stringification (used by io.print / String())
    CHECK(evalStr(vm, R"(
class Money:
    var _init_ = Function(self, n):
        self.n = n
    var _str_ = Function(self):
        return "$" + String(self.n)
String(Money(42))
)") == "$42");

    // _add_ operator overloading
    CHECK(evalStr(vm, R"KI(
class V:
    var _init_ = Function(self, a, b):
        self.a = a
        self.b = b
    var _add_ = Function(self, o):
        return V(self.a + o.a, self.b + o.b)
    var _str_ = Function(self):
        return "(" + String(self.a) + ", " + String(self.b) + ")"
String(V(1, 2) + V(10, 20))
)KI") == "(11, 22)");

    // _sub_ / _mul_
    CHECK(evalStr(vm, R"(
class N:
    var _init_ = Function(self, v):
        self.v = v
    var _sub_ = Function(self, o):
        return N(self.v - o.v)
    var _mul_ = Function(self, o):
        return N(self.v * o.v)
    var _str_ = Function(self):
        return String(self.v)
String(N(10) - N(3)) + "/" + String(N(4) * N(5))
)") == "7/20");

    // _eq_ overrides equality
    CHECK(evalStr(vm, R"(
class Pt:
    var _init_ = Function(self, x):
        self.x = x
    var _eq_ = Function(self, o):
        return self.x == o.x
Pt(5) == Pt(5)
)") == "True");
    CHECK(evalStr(vm, R"(
class Pt:
    var _init_ = Function(self, x):
        self.x = x
    var _eq_ = Function(self, o):
        return self.x == o.x
Pt(5) == Pt(6)
)") == "False");

    // _lt_ ordering
    CHECK(evalStr(vm, R"(
class Ver:
    var _init_ = Function(self, n):
        self.n = n
    var _lt_ = Function(self, o):
        return self.n < o.n
Ver(1) < Ver(2)
)") == "True");

    // _getitem_ (multi-key) / _setitem_ / _len_
    CHECK(evalStr(vm, R"(
class Grid:
    var _init_ = Function(self):
        self.data = {}
    var _setitem_ = Function(self, x, y, v):
        self.data[String(x) + "," + String(y)] = v
    var _getitem_ = Function(self, x, y):
        return self.data[String(x) + "," + String(y)]
    var _len_ = Function(self):
        return len(self.data)
var g = Grid()
g[1, 2] = 99
g[3, 4] = 77
String(g[1, 2]) + "/" + String(g[3, 4]) + "/" + String(len(g))
)") == "99/77/2");

    // _call_ makes an instance callable
    CHECK(evalStr(vm, R"(
class Adder:
    var _init_ = Function(self, n):
        self.n = n
    var _call_ = Function(self, x):
        return x + self.n
var add3 = Adder(3)
add3(10)
)") == "13");

    // _neg_ unary
    CHECK(evalStr(vm, R"(
class M:
    var _init_ = Function(self, v):
        self.v = v
    var _neg_ = Function(self):
        return M(0 - self.v)
    var _str_ = Function(self):
        return String(self.v)
String(-M(5))
)") == "-5");

    // _contains_ for the `in` operator
    CHECK(evalStr(vm, R"(
class Bag:
    var _init_ = Function(self):
        self.items = [1, 2, 3]
    var _contains_ = Function(self, x):
        return x in self.items
2 in Bag()
)") == "True");

    // --- privacy: _name (leading underscore, no trailing) is private ---
    // accessible inside the class:
    CHECK(evalStr(vm, R"(
class Counter:
    var _init_ = Function(self):
        self._count = 0
    var bump = Function(self):
        self._count = self._count + 1
        return self._count
var c = Counter()
c.bump()
c.bump()
)") == "2");
    // NOT readable from outside:
    CHECK_THROWS(vm.runSource(R"(
class Counter:
    var _init_ = Function(self):
        self._count = 0
var c = Counter()
c._count
)"));
    // NOT writable from outside:
    CHECK_THROWS(vm.runSource(R"(
class Counter:
    var _init_ = Function(self):
        self._count = 0
var c = Counter()
c._count = 5
)"));
    // a method may access another same-class instance's private member:
    CHECK(evalStr(vm, R"(
class Box:
    var _init_ = Function(self, v):
        self._v = v
    var combine = Function(self, other):
        return self._v + other._v
Box(3).combine(Box(4))
)") == "7");
    // dunder methods (_init_, _str_, ...) are NOT private — callable/visible:
    CHECK(evalStr(vm, R"(
class Q:
    var _init_ = Function(self):
        self.ok = 1
Q().ok
)") == "1");

    return RUN_TESTS();
}
