// The _super_() operator: self._super_() returns a parent view of self whose method lookup starts at
// the base of the currently-executing method's class. Supports calling overridden parent methods and
// the parent constructor, walks one level per call (multi-level inheritance), throws for a baseless
// class, and may be overridden by a class (discouraged).
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string run(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}
static std::string err(KiritoVM& vm, const std::string& src) {
    try { vm.runSource(src); return ""; } catch (const KiritoError& e) { return e.what(); }
}

static const char* HIER =
    "class Animal:\n"
    "    var _init_ = Function(self, name):\n"
    "        self.name = name\n"
    "    var describe = Function(self) -> String:\n"
    "        return \"Animal:\" + self.name\n"
    "    var speak = Function(self) -> String:\n"
    "        return \"...\"\n"
    "class Dog(Animal):\n"
    "    var _init_ = Function(self, name, breed):\n"
    "        self._super_()._init_(name)\n"
    "        self.breed = breed\n"
    "    var describe = Function(self) -> String:\n"
    "        return self._super_().describe() + \"(\" + self.breed + \")\"\n"
    "    var speak = Function(self) -> String:\n"
    "        return \"woof\"\n"
    "class Puppy(Dog):\n"
    "    var describe = Function(self) -> String:\n"
    "        return self._super_().describe() + \"[puppy]\"\n";

int main() {
    // parent constructor via super
    {
        KiritoVM vm;
        CHECK(run(vm, std::string(HIER) + "var d = Dog(\"Rex\", \"Lab\")\nd.name") == "Rex");
        CHECK(run(vm, std::string(HIER) + "var d = Dog(\"Rex\", \"Lab\")\nd.breed") == "Lab");
    }
    // method extension via super (Dog.describe calls Animal.describe)
    {
        KiritoVM vm;
        CHECK(run(vm, std::string(HIER) + "Dog(\"Rex\", \"Lab\").describe()") == "Animal:Rex(Lab)");
    }
    // overriding without super still works (Dog.speak shadows Animal.speak)
    {
        KiritoVM vm;
        CHECK(run(vm, std::string(HIER) + "Dog(\"Rex\", \"Lab\").speak()") == "woof");
    }
    // multi-level: Puppy -> Dog -> Animal, each super climbs exactly one level
    {
        KiritoVM vm;
        CHECK(run(vm, std::string(HIER) + "Puppy(\"Bit\", \"Corgi\").describe()")
              == "Animal:Bit(Corgi)[puppy]");
        // Puppy inherits Dog's speak (which itself overrides Animal's)
        CHECK(run(vm, std::string(HIER) + "Puppy(\"Bit\", \"Corgi\").speak()") == "woof");
    }
    // a baseless class: _super_() throws
    {
        KiritoVM vm;
        std::string e = err(vm,
            "class Lonely:\n    var go = Function(self):\n        return self._super_()\nLonely().go()\n");
        CHECK(e.find("does not inherit") != std::string::npos);
        CHECK(e.find("Lonely") != std::string::npos);
    }
    // accessing a member the base chain lacks throws a clear error
    {
        KiritoVM vm;
        std::string e = err(vm, std::string(HIER) +
            "class Cat(Animal):\n    var test = Function(self):\n        return self._super_().nope()\n"
            "Cat(\"x\")\nCat(\"x\").test()\n");
        CHECK(e.find("'super' object has no attribute 'nope'") != std::string::npos);
    }
    // a class may override _super_ (discouraged) — its definition wins
    {
        KiritoVM vm;
        CHECK(run(vm,
            "class Base:\n    var greet = Function(self):\n        return \"base\"\n"
            "class Weird(Base):\n"
            "    var _super_ = Function(self):\n        return \"overridden\"\n"
            "    var test = Function(self):\n        return self._super_()\n"
            "Weird().test()") == "overridden");
    }

    return RUN_TESTS();
}
