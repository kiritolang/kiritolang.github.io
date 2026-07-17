// Serialization of native objects via the _getstate_/_setstate_ protocol + a registered deserializer
// factory: a tiny mutable native type round-trips through both the text (`serialize`) and binary
// (`dump`) codecs, including when shared/aliased inside a container. This validates the "native side"
// of object serialization; user-class instance coverage lives in tests/scripts/spec_serialization.ki.
#include <memory>
#include <span>
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// A trivially-mutable native value that opts into serialization by exposing _getstate_/_setstate_.
struct Counter : NativeClass<Counter> {
    static constexpr const char* kTypeName = "Counter";
    int64_t value = 0;
    explicit Counter(int64_t v = 0) : value(v) {}

    Handle getAttr(KiritoVM& vm, Handle self, std::string_view name) override {
        if (name == "value")
            return vm.makeInt(value);
        if (name == "_getstate_")
            return makeMethod(vm, "_getstate_", {}, [self](KiritoVM& kv, std::span<const Handle>) -> Handle {
                return kv.makeInt(static_cast<Counter&>(kv.arena().deref(self)).value);
            }, std::vector<Handle>{self});
        if (name == "_setstate_")
            return makeMethod(vm, "_setstate_", {"state"}, [self](KiritoVM& kv, std::span<const Handle> a) -> Handle {
                static_cast<Counter&>(kv.arena().deref(self)).value =
                    static_cast<const IntVal&>(kv.arena().deref(a[0])).value();
                return kv.none();
            }, std::vector<Handle>{self});
        return Object::getAttr(vm, self, name);
    }
};

static int64_t counterValue(KiritoVM& vm, Handle h) {
    return static_cast<Counter&>(vm.arena().deref(h)).value;
}

int main() {
    KiritoVM vm;
    // A native type opts in by registering a factory that builds an empty instance (the protocol's
    // _setstate_ then fills it in).
    vm.registerDeserializer("Counter", [](KiritoVM& kv, Handle) {
        return kv.alloc(std::make_unique<Counter>(0));
    });

    // --- a bare native object round-trips through text and binary ---
    Handle c = vm.alloc(std::make_unique<Counter>(42));
    {
        Handle back = serial::loads(vm, serial::dumps(vm, c));
        CHECK(counterValue(vm, back) == 42);
    }
    {
        Handle back = dumpfmt::read(vm, dumpfmt::write(vm, c));
        CHECK(counterValue(vm, back) == 42);
    }

    // --- shared/aliased inside a List: the alias survives (both elements are the same object) ---
    {
        RootScope rs(vm);
        Handle c2 = rs.add(vm.alloc(std::make_unique<Counter>(7)));
        auto list = std::make_unique<ListVal>();
        list->elems = {c2, c2};
        Handle lh = rs.add(vm.alloc(std::move(list)));

        Handle lback = serial::loads(vm, serial::dumps(vm, lh));
        auto& lb = static_cast<ListVal&>(vm.arena().deref(lback));
        CHECK(lb.elems.size() == 2);
        CHECK(lb.elems[0] == lb.elems[1]);             // shared reference preserved
        CHECK(counterValue(vm, lb.elems[0]) == 7);

        Handle dback = dumpfmt::read(vm, dumpfmt::write(vm, lh));
        auto& db = static_cast<ListVal&>(vm.arena().deref(dback));
        CHECK(db.elems[0] == db.elems[1]);
        CHECK(counterValue(vm, db.elems[0]) == 7);
    }

    // --- adversarial: the SAME native object is NOT serializable in a VM that didn't register it ---
    {
        KiritoVM bare;
        Handle x = bare.alloc(std::make_unique<Counter>(1));
        std::string text = serial::dumps(bare, x);                 // serializing is fine (uses _getstate_)
        CHECK_THROWS(serial::loads(bare, text));                   // but rebuild needs a factory/class
    }

    return RUN_TESTS();
}
