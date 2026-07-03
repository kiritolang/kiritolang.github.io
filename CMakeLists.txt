cmake_minimum_required(VERSION 3.28)
project(Kirito CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Optional HTTPS support in the `net` module (links OpenSSL). Off by default so the core stays
# dependency-free; enable with -DKIRITO_ENABLE_TLS=ON.
option(KIRITO_ENABLE_TLS "Enable HTTPS in the net module via OpenSSL" OFF)

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

# The interpreter is header-only: an INTERFACE target that just exposes src/. It also carries
# the platform's socket library and the threads library (the multiprocessing dispatcher uses
# std::thread/mutex) so any consumer (CLI or tests) links them transparently.
find_package(Threads REQUIRED)
add_library(kirito INTERFACE)
target_include_directories(kirito INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(kirito INTERFACE cxx_std_20)
target_link_libraries(kirito INTERFACE Threads::Threads)
if(WIN32)
    target_link_libraries(kirito INTERFACE ws2_32)  # Winsock
endif()

# The only compiled translation unit with a main().
add_executable(ki main.cpp)
target_link_libraries(ki PRIVATE kirito)
if(NOT MSVC)
    target_compile_options(ki PRIVATE -Wall -Wextra -Wpedantic)
endif()

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

if(KIRITO_ENABLE_TLS)
    find_package(OpenSSL REQUIRED)
    target_compile_definitions(ki PRIVATE KIRITO_ENABLE_TLS)
    target_link_libraries(ki PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    if(WIN32)
        # A static OpenSSL on Windows pulls in the system cert store + CNG RNG; link those libs so
        # a fully-static mingw/MSVC build resolves (harmless when OpenSSL is shared).
        target_link_libraries(ki PRIVATE crypt32 bcrypt ws2_32)
    endif()
endif()

enable_testing()
add_subdirectory(tools/tests)
