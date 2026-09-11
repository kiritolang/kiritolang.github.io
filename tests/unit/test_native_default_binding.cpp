// Regression: a signatured NativeFunction invoked through a DIRECT Object::call — the public
// Value::call, or a stdlib `deref(fn).call(vm, args)` callback site — must BIND its arguments (fill
// defaults, or throw a clean "missing required argument"), never hand the impl an under-length span.
// Reading a defaulted slot past an under-length `args` is UB — a heap-buffer-overflow, confirmed under
// ASan, whose non-crashing symptom is "dangling handle (stale generation)". The bytecode VM bound via
// applyCall; NativeFunction::call now binds too, so the one virtual entry point is always safe. Every
// block below was verified to overflow/throw on the pre-fix build. See function.hpp.
#include <memory>
#include <span>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

int main() {
    // a signatured native with a DEFAULT, under-supplied through the public Value::call: the default
    // must be filled (was an OOB read of a[1]).
    {
        KiritoVM vm;
        Handle def = vm.makeInt(1000);
        std::vector<NativeParam> sig;
        sig.emplace_back("x", "Integer");
        sig.emplace_back("y", "Integer", def);
        Handle fnH = vm.alloc(std::make_unique<NativeFunction>(
            "addy", std::move(sig), "Integer",
            [](KiritoVM& v, std::span<const Handle> a) -> Handle {
                return v.makeInt(Value(v, a[0]).asInt("x") + Value(v, a[1]).asInt("y"));
            }));
        Value fn(vm, fnH);
        CHECK(fn.call({5}).asInt("r") == 1005);      // default y=1000 filled
        CHECK(fn.call({5, 2}).asInt("r") == 7);      // full arity still works
    }
    // a signatured native missing a REQUIRED arg must throw cleanly, never read OOB.
    {
        KiritoVM vm;
        std::vector<NativeParam> sig;
        sig.emplace_back("x", "Integer");
        sig.emplace_back("y", "Integer");
        Handle fnH = vm.alloc(std::make_unique<NativeFunction>(
            "addy", std::move(sig), "Integer",
            [](KiritoVM& v, std::span<const Handle> a) -> Handle {
                return v.makeInt(Value(v, a[0]).asInt("x") + Value(v, a[1]).asInt("y"));
            }));
        Value fn(vm, fnH);
        CHECK_THROWS(fn.call({5}));
    }
    // the bundled stdlib was a real victim: int.isprimeaks(n) has a defaulted `maxdegree`. Fetch it
    // and call it under-supplied through exactly the documented public Value API.
    {
        KiritoVM vm;
        Value mod(vm, vm.runSource("import(\"int\")\n"));
        Value fn = mod.getAttr("isprimeaks");
        CHECK(fn.call({97}).truthy());               // maxdegree defaulted; 97 is prime
        Value fn2 = mod.getAttr("isprime");          // trial-division isprime (single required arg)
        CHECK(fn2.call({97}).truthy());
        CHECK(!fn2.call({91}).truthy());
    }
    // arity matrix: one required + TWO defaults, exercised across every under/over-supply via Value::call.
    {
        KiritoVM vm;
        Handle d10 = vm.makeInt(10), d100 = vm.makeInt(100);
        std::vector<NativeParam> sig;
        sig.emplace_back("a", "Integer");
        sig.emplace_back("b", "Integer", d10);
        sig.emplace_back("c", "Integer", d100);
        Handle fnH = vm.alloc(std::make_unique<NativeFunction>(
            "abc", std::move(sig), "Integer",
            [](KiritoVM& v, std::span<const Handle> args) -> Handle {
                return v.makeInt(Value(v, args[0]).asInt("a") + Value(v, args[1]).asInt("b") +
                                 Value(v, args[2]).asInt("c"));
            }));
        Value fn(vm, fnH);
        CHECK(fn.call({1}).asInt("r") == 111);        // a=1, b=10, c=100 (both defaults filled)
        CHECK(fn.call({1, 2}).asInt("r") == 103);     // a=1, b=2, c=100 (one default filled)
        CHECK(fn.call({1, 2, 3}).asInt("r") == 6);    // all explicit
        CHECK_THROWS(fn.call({}));                    // missing required 'a' -> clean throw, not OOB
        CHECK_THROWS(fn.call({1, 2, 3, 4}));          // too many positionals -> clean throw
    }
    return RUN_TESTS();
}
