// Keyword arguments work uniformly: plain functions, class constructors (forwarded to _init_), and
// instance methods (including inherited and super-dispatched ones) all accept named args, defaults,
// and out-of-order keywords — exactly like a top-level function call.
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

int main() {
    // --- plain function (the already-working baseline) ---
    {
        KiritoVM vm;
        CHECK(evalStr(vm, "var f = Function(a, b = 2):\n    return a - b\nf(10, b = 3)") == "7");
        CHECK(evalStr(vm, "var f = Function(a, b):\n    return a - b\nf(b = 1, a = 9)") == "8");  // out of order
    }

    // --- class instantiation forwards keywords to _init_ ---
    {
        KiritoVM vm;
        const char* cls = R"(
class Point:
    var _init_ = Function(self, x, y = 0):
        self.x = x
        self.y = y
)";
        CHECK(evalStr(vm, std::string(cls) + "Point(1, y = 5).y") == "5");
        CHECK(evalStr(vm, std::string(cls) + "Point(x = 7).x") == "7");          // keyword for a positional
        CHECK(evalStr(vm, std::string(cls) + "Point(y = 9, x = 2).y") == "9");   // out of order
        CHECK(evalStr(vm, std::string(cls) + "Point(3).y") == "0");              // default still applies
    }

    // --- instance methods accept keywords ---
    {
        KiritoVM vm;
        const char* cls = R"(
class Calc:
    var _init_ = Function(self):
        self.base = 100
    var scale = Function(self, x, factor = 1, offset = 0):
        return self.base + x * factor + offset
var c = Calc()
)";
        CHECK(evalStr(vm, std::string(cls) + "c.scale(2, factor = 5)") == "110");
        CHECK(evalStr(vm, std::string(cls) + "c.scale(2, offset = 7, factor = 5)") == "117");  // out of order
        CHECK(evalStr(vm, std::string(cls) + "c.scale(x = 3)") == "103");
    }

    // --- keywords survive inheritance and _super_ dispatch ---
    {
        KiritoVM vm;
        const char* cls = R"(
class Animal:
    var _init_ = Function(self, name, sound = "..."):
        self.name = name
        self.sound = sound
    var speak = Function(self, times = 1):
        return self.sound * times
class Dog(Animal):
    var _init_ = Function(self, name):
        self._super_()._init_(name, sound = "woof")
    var speak = Function(self, times = 1, loud = False):
        var s = self._super_().speak(times = times)
        return s.upper() if loud else s
var d = Dog("Rex")
)";
        CHECK(evalStr(vm, std::string(cls) + "d.sound") == "woof");                 // kw passed up via super
        CHECK(evalStr(vm, std::string(cls) + "d.speak(times = 3)") == "woofwoofwoof");
        CHECK(evalStr(vm, std::string(cls) + "d.speak(times = 2, loud = True)") == "WOOFWOOF");
    }

    // --- a chained/returned method call still takes keywords ---
    {
        KiritoVM vm;
        const char* cls = R"(
class Builder:
    var _init_ = Function(self):
        self.parts = []
    var add = Function(self, item, sep = ","):
        self.parts.append(item)
        return self
    var render = Function(self, sep = ","):
        return sep.join(self.parts)
var b = Builder()
)";
        CHECK(evalStr(vm, std::string(cls) + "b.add(\"a\").add(\"b\").render(sep = \"-\")") == "a-b");
    }

    // --- errors: unknown keyword, duplicate, missing required ---
    {
        KiritoVM vm;
        vm.runSource(R"(
class P:
    var _init_ = Function(self, a, b = 1):
        self.a = a
)");
        CHECK_THROWS(vm.runSource("P(1, c = 9)"));    // unknown keyword
        CHECK_THROWS(vm.runSource("P(1, a = 2)"));    // 'a' given positionally and by keyword
        CHECK_THROWS(vm.runSource("P()"));            // missing required positional
        // positional-after-keyword is a parse error regardless of callee kind
        CHECK_THROWS(vm.runSource("P(a = 1, 2)"));
    }

    return RUN_TESTS();
}
