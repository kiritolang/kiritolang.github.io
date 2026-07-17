#ifndef KIRITO_TEST_PARALLEL_UTIL_HPP
#define KIRITO_TEST_PARALLEL_UTIL_HPP

// Shared helpers for the parallel/dispatcher tests: a deadlock watchdog plus a temp-file scenario
// runner (parallel.spawn re-reads a function's defining file in the worker, so spawnable functions
// must live in a real .ki file — runSource with a temp path gives them one).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <utility>

#include "check.hpp"
#include "kirito.hpp"

namespace kitest {

// Run `fn` under a wall-clock watchdog. If it doesn't finish within `seconds` it has almost certainly
// deadlocked: print a clear FAIL and HARD-exit (a hung worker thread would otherwise block normal
// process teardown forever, turning a regression into an indefinite CI hang). On success, re-throw
// anything `fn` threw so the caller can observe it.
template <class Fn>
inline void noDeadlock(const char* what, double seconds, Fn&& fn) {
    auto fut = std::async(std::launch::async, std::forward<Fn>(fn));
    if (fut.wait_for(std::chrono::duration<double>(seconds)) != std::future_status::ready) {
        std::printf("FAIL deadlock: %s exceeded %.1fs\n", what, seconds);
        std::fflush(stdout);
        std::_Exit(1);
    }
    fut.get();
}

// Write Kirito source to a unique temp .ki file; return its path. The caller runs the source with
// this path as the chunk name, so functions defined in it carry it as sourceFile and parallel.spawn
// can re-read the file in a worker VM.
inline std::string writeTempKi(const std::string& src) {
    static std::atomic<uint64_t> counter{0};
    auto path = std::filesystem::temp_directory_path() /
                ("ki_parallel_test_" + std::to_string(counter.fetch_add(1)) + ".ki");
    std::ofstream(path.string(), std::ios::binary) << src;
    return path.string();
}

// Run a self-contained Kirito program on a fresh dispatcher under the watchdog, returning the last
// expression's value stringified. The program's functions are spawnable (it lives in a real file).
// The dispatcher is shut down (all workers joined) and the temp file removed before returning.
inline std::string runScenario(const char* what, const std::string& src, double seconds = 20.0) {
    std::string out;
    noDeadlock(what, seconds, [&] {
        kirito::KiritoDispatcher disp;
        std::string path = writeTempKi(src);
        struct Cleanup {
            kirito::KiritoDispatcher& d;
            std::string p;
            ~Cleanup() { d.shutdown(); std::error_code ec; std::filesystem::remove(p, ec); }
        } cleanup{disp, path};
        kirito::Handle r = disp.mainVM().runSource(src, path);
        out = disp.mainVM().stringify(r);
    });
    return out;
}

// Like runScenario but expects the program to throw; returns the error message (empty if it did NOT
// throw — a test failure the caller asserts on).
inline std::string runScenarioError(const char* what, const std::string& src, double seconds = 20.0) {
    std::string msg;
    noDeadlock(what, seconds, [&] {
        kirito::KiritoDispatcher disp;
        std::string path = writeTempKi(src);
        struct Cleanup {
            kirito::KiritoDispatcher& d;
            std::string p;
            ~Cleanup() { d.shutdown(); std::error_code ec; std::filesystem::remove(p, ec); }
        } cleanup{disp, path};
        try {
            disp.mainVM().runSource(src, path);
        } catch (const kirito::KiritoError& e) {
            msg = e.what();
        }
    });
    return msg;
}

// Run a self-checking Kirito program (it uses `assert`, guarded by `if argmain:` so workers — which
// re-evaluate the file with argmain False — only define the spawnable functions). Records a failure
// if it throws unexpectedly.
inline void expectOk(const char* what, const std::string& src) {
    std::string err;
    bool threw = false;
    try {
        runScenario(what, src);
    } catch (const kirito::KiritoError& e) {
        threw = true;
        err = e.what();
    }
    if (threw) {
        std::printf("FAIL %s: unexpected error: %s\n", what, err.c_str());
        ++failures;
    }
}

// Run a program expected to throw an error whose message contains `needle`.
inline void expectError(const char* what, const std::string& src, const std::string& needle) {
    std::string msg = runScenarioError(what, src);
    if (msg.empty() || msg.find(needle) == std::string::npos) {
        std::printf("FAIL %s: expected error containing '%s', got '%s'\n", what, needle.c_str(),
                    msg.c_str());
        ++failures;
    }
}

}  // namespace kitest

#endif
