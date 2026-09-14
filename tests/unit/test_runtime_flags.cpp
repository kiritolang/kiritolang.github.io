// Foundation layer (waterfall): the runtime knobs that HIGHER-level tests lean on must first be proven
// to actually DO something. A GC-soak test run under `--gc-threshold 1` is meaningless unless the
// threshold demonstrably forces collections; an inline-vs-`--no-inline` differential is vacuous unless
// the flag demonstrably toggles the transform. These are VM/embedding-API properties (collection
// counters, traceback shape, getter/setter round-trips) that a plain `.ki` result cannot express, so
// they legitimately live in C++. Everything downstream (the .ki soak + differential tests) rests on
// what is proven here.
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

int main() {
    // ---- setGcThreshold has teeth: threshold=1 collects on (nearly) every allocation, a huge threshold
    //      essentially never — so the `--gc-threshold 1` soak actually soaks. Same allocating workload
    //      (3000 fresh nested lists + fresh strings, non-interned) both ways; compare collection counts.
    const char* alloc =
        "var xs = []\nvar i = 0\nwhile i < 3000:\n xs.append([i, i + 1, \"s\" + String(i)])\n"
        " i = i + 1\nlen(xs)";
    {
        KiritoVM hot;  hot.installStandardLibrary();  hot.setGcThreshold(1);          hot.runSource(alloc);
        KiritoVM cold; cold.installStandardLibrary(); cold.setGcThreshold(100000000); cold.runSource(alloc);
        std::size_t hotTotal  = hot.gcMinorCount()  + hot.gcMajorCount();
        std::size_t coldTotal = cold.gcMinorCount() + cold.gcMajorCount();
        CHECK(hotTotal > 1000);            // per-alloc GC over ~3000 allocations -> thousands of collections
        CHECK(coldTotal < hotTotal / 10);  // a huge threshold collects dramatically less (the knob works)
        CHECK(hot.gcMajorCount() > 0);     // threshold=1 forces MAJOR collections too (pins both cadences)
    }

    // ---- setInliningEnabled has teeth: with inlining ON an inlined call leaves NO separate traceback
    //      frame; `--no-inline` (OFF) restores it. So the on==off value differential is NON-vacuous — the
    //      two modes really do execute differently — while the RESULT (the thrown error) is identical.
    //      Frame count is observed the only way a program can: via sys.traceback() after a Kirito catch.
    auto frameCount = [](bool inlining) -> long {
        KiritoVM vm; vm.installStandardLibrary(); vm.setInliningEnabled(inlining);
        const char* prog =
            "var sys = import(\"sys\")\n"
            "var f = Function():\n return (Function(x): return x // 0)(1)\n"   // an inlinable throwing lambda
            "var n = 0\n"
            "try:\n discard f()\ncatch as e:\n"
            " for line in sys.traceback().split(\"\\n\"):\n  if \", in \" in line:\n   n = n + 1\n"
            "n";
        return std::stol(vm.stringify(vm.runSource(prog)));
    };
    long framesOn = frameCount(true), framesOff = frameCount(false);
    CHECK(framesOn >= 1);                 // at least the enclosing function's own frame
    CHECK(framesOff == framesOn + 1);     // --no-inline restores exactly the inlined lambda's frame
    // the error itself is identical regardless of the flag (inlining changes frames/speed, not results)
    auto errText = [](bool inlining) -> std::string {
        KiritoVM vm; vm.installStandardLibrary(); vm.setInliningEnabled(inlining);
        try { vm.runSource("var f = Function():\n return (Function(x): return x // 0)(1)\nf()"); return "no-throw"; }
        catch (const KiritoError& e) { return e.what(); }
        catch (const std::exception& e) { return std::string("std:") + e.what(); }
    };
    CHECK(errText(true) == errText(false));
    CHECK(errText(true).find("division by zero") != std::string::npos);

    // ---- getters reflect setters (the embedding contract) ----
    {
        KiritoVM vm; vm.installStandardLibrary();
        CHECK(vm.inliningEnabled() == true);            // default on
        vm.setInliningEnabled(false); CHECK(vm.inliningEnabled() == false);
        vm.setInliningEnabled(true);  CHECK(vm.inliningEnabled() == true);
    }

    return RUN_TESTS();
}
