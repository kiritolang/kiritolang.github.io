#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

int main() {
    KiritoVM vm;

    // None is interned and behaves correctly through the arena.
    Handle n = vm.none();
    CHECK(vm.arena().deref(n).kind() == ValueKind::None);
    CHECK(vm.arena().deref(n).truthy() == false);
    CHECK(vm.stringify(n) == "None");

    // A handle that names no live slot is rejected, not silently dereferenced.
    CHECK_THROWS(vm.arena().deref(Handle{999, 0}));

    // Two VMs are fully independent — each owns its own arena.
    KiritoVM vm2;
    CHECK(vm2.stringify(vm2.none()) == "None");
    CHECK(vm.stringify(vm.none()) == "None");

    return RUN_TESTS();
}
