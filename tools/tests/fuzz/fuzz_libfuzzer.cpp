// libFuzzer entrypoint: coverage-guided version of the offline fuzz_eval.cpp corpus soaker. Each
// input is fed to the interpreter under the same contract as the offline fuzzer — a KiritoError is
// FINE, anything else is a bug that libFuzzer's coverage-guided exploration will hunt.
//
// Build with clang, `-DKIRITO_ENABLE_LIBFUZZER=ON`, then run:
//   ./build/ki_fuzz corpus/ -max_total_time=600 -jobs=4
//
// A crashing input drops in the current directory as `crash-<sha>` — save it under
// `tools/tests/errors/<name>.ki` + `.experr` as a regression test.
#include <cstddef>
#include <cstdint>
#include <string>

#include "kirito.hpp"

using namespace kirito;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Guard rails: cap input size so the fuzzer doesn't spend its budget compiling multi-MB
    // programs. 64 KiB is more than enough to exercise every parser / lexer / VM path.
    if (size > 65536) return 0;

    std::string src(reinterpret_cast<const char*>(data), size);

    KiritoVM vm;
    vm.setGcThreshold(64);      // exercise the collector aggressively
    vm.setMaxCallDepth(200);    // stress the recursion guard cheaply
    try {
        vm.runSource(src);
    } catch (const KiritoError&) {
        // A caught KiritoError is the expected path for invalid input.
    } catch (const std::exception&) {
        // Any other C++ exception escaping the interpreter boundary is a bug — return non-zero to
        // let libFuzzer treat it as a crash and minimise the reproducer.
        return 1;
    }
    return 0;
}
