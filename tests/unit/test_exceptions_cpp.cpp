// Thorough tests of exceptions at the C++/embedding boundary: native functions that throw, how
// those surface to Kirito try/catch, how Kirito `throw` surfaces back to the C++ embedder, and
// that unwinding through both layers is GC-safe and leaves the VM usable.
#include <span>
#include <stdexcept>
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

int main() {
    // --- 1. A native function that throws KiritoError is catchable in Kirito as a String message.
    {
        KiritoVM vm;
        vm.registerGlobal("boom", vm.arena().alloc(std::make_unique<NativeFunction>(
            "boom", [](KiritoVM&, std::span<const Handle>) -> Handle {
                throw KiritoError("native failure");
            })));
        CHECK(evalStr(vm, "try:\n    boom()\ncatch as e:\n    e\n") == "native failure");
    }

    // --- 1b. A native function that throws a PLAIN std::exception (not KiritoError) is also
    //         catchable in Kirito as a String message, and the VM stays usable.
    {
        KiritoVM vm;
        vm.registerGlobal("stdboom", vm.arena().alloc(std::make_unique<NativeFunction>(
            "stdboom", [](KiritoVM&, std::span<const Handle>) -> Handle {
                throw std::out_of_range("index 5 out of bounds");
            })));
        CHECK(evalStr(vm, "try:\n    stdboom()\ncatch as e:\n    e\n") == "index 5 out of bounds");
        CHECK(evalStr(vm, "1 + 1") == "2");  // VM still works after unwinding a std::exception
        // a different std exception type, surfaced via with/finally too
        vm.registerGlobal("rtboom", vm.arena().alloc(std::make_unique<NativeFunction>(
            "rtboom", [](KiritoVM&, std::span<const Handle>) -> Handle {
                throw std::runtime_error("plain runtime error");
            })));
        CHECK(evalStr(vm,
            "var log = []\ntry:\n    rtboom()\ncatch as e:\n    log.append(e)\nfinally:\n    log.append(\"fin\")\n"
            "\", \".join(log)") == "plain runtime error, fin");
    }

    // --- 2. An uncaught native throw surfaces to the embedder as a KiritoError.
    {
        KiritoVM vm;
        vm.registerGlobal("boom", vm.arena().alloc(std::make_unique<NativeFunction>(
            "boom", [](KiritoVM&, std::span<const Handle>) -> Handle {
                throw KiritoError("unhandled native");
            })));
        bool caught = false;
        try {
            vm.runSource("boom()\n");
        } catch (const KiritoError& e) {
            caught = true;
            CHECK(std::string(e.what()).find("unhandled native") != std::string::npos);
        }
        CHECK(caught);
        // The VM is still usable after the C++ exception unwound through it.
        CHECK(evalStr(vm, "1 + 2") == "3");
    }

    // --- 3. A native function can re-enter Kirito (call a Kirito fn) and propagate its exception.
    {
        KiritoVM vm;
        // applyTwice(f, x): calls the Kirito callable f on x (and would propagate any throw).
        vm.registerGlobal("applyfn", vm.arena().alloc(std::make_unique<NativeFunction>(
            "applyfn", [](KiritoVM& kv, std::span<const Handle> a) -> Handle {
                std::array<Handle, 1> args{a[1]};
                return kv.arena().deref(a[0]).call(kv, args);
            })));
        // A Kirito callback that throws; the throw must propagate out through the native frame
        // and be catchable by the surrounding Kirito try.
        CHECK(evalStr(vm, R"(
var bad = Function(x):
    throw "from callback"
var r = "none"
try:
    applyfn(bad, 5)
catch as e:
    r = e
r
)") == "from callback");
    }

    // --- 4. An uncaught Kirito `throw` of a class instance surfaces to the embedder as a
    //        KiritoError whose message carries the thrown value's text (runSource never lets a raw
    //        throw escape unwrapped). We give Err a _str_ so the value is observable in the message.
    {
        KiritoVM vm;
        bool caught = false;
        std::string msg;
        try {
            vm.runSource(R"KI(
class Err:
    var _init_ = Function(self, code):
        self.code = code
    var _str_ = Function(self) -> String:
        return "Err(" + String(self.code) + ")"
throw Err(42)
)KI");
        } catch (const KiritoError& e) {
            caught = true;
            msg = e.what();
        }
        CHECK(caught);
        CHECK(msg.find("uncaught exception") != std::string::npos);
        CHECK(msg.find("Err(42)") != std::string::npos);  // the thrown instance's _str_ is in the message
    }

    // --- 5. GC safety: an exception thrown deep in a heavily-allocating computation unwinds
    //        cleanly under aggressive GC, and the VM keeps working.
    {
        KiritoVM vm;
        vm.setGcThreshold(32);
        CHECK(evalStr(vm, R"(
var deep = Function(n):
    var junk = [n, n + 1, n + 2]
    if n > 300:
        throw "deep enough"
    return deep(n + 1)
var caught = "no"
try:
    deep(0)
catch as e:
    caught = e
caught
)") == "deep enough");
        CHECK(evalStr(vm, "[1, 2, 3]") == "[1, 2, 3]");
    }

    // --- 6. finally runs while unwinding a native-thrown exception.
    {
        KiritoVM vm;
        vm.registerGlobal("boom", vm.arena().alloc(std::make_unique<NativeFunction>(
            "boom", [](KiritoVM&, std::span<const Handle>) -> Handle {
                throw KiritoError("x");
            })));
        CHECK(evalStr(vm, R"(
var log = []
try:
    try:
        boom()
    finally:
        log.append("inner-finally")
catch as e:
    log.append("outer-catch")
log
)") == "['inner-finally', 'outer-catch']");
    }

    return RUN_TESTS();
}
