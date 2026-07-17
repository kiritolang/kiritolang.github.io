cmake_minimum_required(VERSION 3.28)
project(Kirito CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Optional HTTPS support in the `net` module (links OpenSSL). Off by default so the core stays
# dependency-free; enable with -DKIRITO_ENABLE_TLS=ON.
option(KIRITO_ENABLE_TLS "Enable HTTPS in the net module via OpenSSL" OFF)

# Optional libFuzzer (coverage-guided) build of the stability fuzzer. Off by default. Enable with
# -DKIRITO_ENABLE_LIBFUZZER=ON — requires clang. Builds tests/fuzz/fuzz_libfuzzer.cpp as
# `ki_fuzz`, which reads inputs from libFuzzer's `LLVMFuzzerTestOneInput` and runs each through the
# interpreter under the same "OK if throws / not OK if crashes" contract as the offline fuzzer.
# Run:  ./ki_fuzz corpus/ -max_total_time=600
option(KIRITO_ENABLE_LIBFUZZER "Build ki_fuzz (libFuzzer entrypoint) for coverage-guided fuzzing" OFF)

# Static linking for all builds, so the binaries are self-contained and don't depend on matching
# shared libraries on the target machine. MSVC uses the static CRT (/MT); GCC/Clang link fully
# static. Note: a fully static glibc link emits a harmless link-time warning about getaddrinfo
# (glibc's NSS name resolution prefers shared modules) — plain DNS/hosts resolution still works.
# When TLS is enabled, OpenSSL is usually shared-only, so we fall back to a static C++ runtime.
# A fully static link is incompatible with the sanitizer runtimes (asan/ubsan), so when those flags
# are present we drop static linking entirely (a dynamic build is fine for the test/diagnostic
# variant the sanitizers run under).
string(FIND "${CMAKE_CXX_FLAGS}" "sanitize" _kirito_san)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
elseif(NOT _kirito_san EQUAL -1)
    # sanitizer build: link dynamically
elseif(KIRITO_ENABLE_TLS)
    add_link_options(-static-libgcc -static-libstdc++)
else()
    add_link_options(-static)
endif()

# Prefer a faster linker when one is installed (mold > lld > the system default). Linking the ~156
# static test/embed binaries is a large slice of a clean build, and mold/lld cut it substantially.
# Purely a link-time acceleration — the -static / self-contained-binary policy above is unchanged, and
# both linkers handle a fully-static C++ link. Opt out with -DKIRITO_FAST_LINKER=OFF.
option(KIRITO_FAST_LINKER "Use mold/lld if available for faster linking" ON)
if(KIRITO_FAST_LINKER AND NOT MSVC)
    find_program(KIRITO_MOLD mold)
    find_program(KIRITO_LLD NAMES ld.lld lld)
    if(KIRITO_MOLD)
        add_link_options(-fuse-ld=mold)
        message(STATUS "Kirito: linking with mold (${KIRITO_MOLD})")
    elseif(KIRITO_LLD)
        add_link_options(-fuse-ld=lld)
        message(STATUS "Kirito: linking with lld (${KIRITO_LLD})")
    endif()
endif()

# The interpreter is header-only: an INTERFACE target that just exposes src/. It also carries
# the platform's socket library and the threads library (the multiprocessing dispatcher uses
# std::thread/mutex) so any consumer (CLI or tests) links them transparently.
find_package(Threads REQUIRED)
add_library(kirito INTERFACE)
target_include_directories(kirito INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(kirito INTERFACE cxx_std_20)
target_link_libraries(kirito INTERFACE Threads::Threads)
# Clang leaves sized deallocation off by default (GCC/MSVC enable it). Object re-declares the sized
# aligned operator delete only under __cpp_sized_deallocation, so the header compiles either way; turn
# the feature ON for Clang so the sized-delete fast path is kept. Propagates to every consumer.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(kirito INTERFACE -fsized-deallocation)
endif()
if(WIN32)
    target_link_libraries(kirito INTERFACE ws2_32 bcrypt)  # Winsock; bcrypt = BCryptGenRandom (OS CSPRNG)
endif()

# HTTPS in the `net` module is opt-in (-DKIRITO_ENABLE_TLS). Because src/kirito.hpp itself
# #includes <openssl/ssl.h> under that flag, the TLS dependency (define + include path + link libs)
# belongs on the INTERFACE target — so EVERY consumer that compiles the umbrella header gets OpenSSL's
# headers: the shared PCH carrier and the tests, not only the final `ki` link. (Without this the PCH
# carrier couldn't find openssl/ssl.h against a non-system OpenSSL, e.g. the mingw cross-build.)
if(KIRITO_ENABLE_TLS)
    find_package(OpenSSL REQUIRED)
    target_compile_definitions(kirito INTERFACE KIRITO_ENABLE_TLS)
    target_link_libraries(kirito INTERFACE OpenSSL::SSL OpenSSL::Crypto)
    if(WIN32)
        # A static OpenSSL on Windows pulls in the system cert store + CNG RNG; link those libs so
        # a fully-static mingw/MSVC build resolves (harmless when OpenSSL is shared).
        target_link_libraries(kirito INTERFACE crypt32 bcrypt ws2_32)
    endif()
endif()

# ONE precompiled header for the whole build. src/kirito.hpp pulls in the entire header-only
# interpreter; parsing it dominates compile time and it is included by main.cpp AND every test TU
# (~156 in all). Compile it into a PCH ONCE here (a tiny carrier TU) and have `ki` plus every test
# REUSE_FROM it, so the core is parsed a single time per build instead of once per TU. Defined here
# (not under tests) so the `ki` target below can reuse it too.
# -Winvalid-pch (with the presets' -Werror) makes a silently-unusable PCH a hard error, so a flag
# mismatch that would quietly drop the PCH and reparse everything can never slip through.
add_library(kirito_pch STATIC ${CMAKE_CURRENT_SOURCE_DIR}/tests/pch_carrier.cpp)
target_link_libraries(kirito_pch PRIVATE kirito)
if(NOT MSVC)
    target_compile_options(kirito_pch PRIVATE -Wall -Wextra -Wpedantic -Winvalid-pch)
endif()
target_precompile_headers(kirito_pch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/kirito.hpp)

# The only compiled translation unit with a main().
add_executable(ki main.cpp)
target_link_libraries(ki PRIVATE kirito)
if(NOT MSVC)
    target_compile_options(ki PRIVATE -Wall -Wextra -Wpedantic -Winvalid-pch)
endif()
target_precompile_headers(ki REUSE_FROM kirito_pch)  # reuse the one shared PCH (parsed once)

# Windows defaults to a small stack (1 MB MSVC / 2 MB mingw) vs ~8 MB on Linux, and reaching the
# parser's nesting guard (kMaxParseDepth) descends the full precedence chain per level — a few MB of
# frames. So reserve a generous 64 MB stack on Windows (just address space, not committed) and the
# catchable "nested too deeply" guard fires first, exactly as on Linux, instead of a raw overflow.
if(WIN32)
    if(MSVC)
        target_link_options(ki PRIVATE /STACK:67108864)
    else()
        target_link_options(ki PRIVATE -Wl,--stack,67108864)
    endif()
endif()

# (TLS/OpenSSL is wired onto the `kirito` INTERFACE target above, so `ki` — which links `kirito` —
# picks up the define, headers, and OpenSSL link libs transitively.)

# libFuzzer entrypoint: a coverage-guided binary that reads inputs from libFuzzer and runs each
# through the interpreter under the same contract as the offline fuzz_eval.cpp. Requires clang
# with `-fsanitize=fuzzer` — the caller supplies it via the CMake configure flags, we only build
# the target when the option is on.
if(KIRITO_ENABLE_LIBFUZZER)
    add_executable(ki_fuzz tests/fuzz/fuzz_libfuzzer.cpp)
    target_link_libraries(ki_fuzz PRIVATE kirito)
    target_compile_options(ki_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(ki_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

enable_testing()
add_subdirectory(tests)
