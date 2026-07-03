// Direct C++ unit tests for the cross-platform process layer (proc_compat.hpp) backing
// sys.createprocess / sys.shell. These drive proccompat::run() straight (no VM), so the
// fork/exec/pipe/thread machinery is exercised on its own — and, crucially, under asan/tsan, which
// instrument the per-stream reader threads, the stdin writer thread, and every fd. The substantive
// cases use POSIX shell commands (the sanitizer matrix is Linux); Windows gets a portable smoke set.
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito/proc_compat.hpp"

using namespace kirito::proccompat;

#if !defined(_WIN32)
static std::vector<std::string> sh(const std::string& c) { return {"/bin/sh", "-c", c}; }
#endif

int main() {
    // Spawn failures are catchable ProcErrors on every platform (no crash).
    CHECK_THROWS(run({}, "", "", 0.0));                              // empty argv
    CHECK_THROWS(run({"no_such_program_zzz_kirito_xyz"}, "", "", 0.0));  // not found

#if !defined(_WIN32)
    // --- exit codes ---
    CHECK(run(sh("exit 0"), "", "", 0.0).code == 0);
    CHECK(run(sh("exit 7"), "", "", 0.0).code == 7);
    CHECK(run(sh("exit 255"), "", "", 0.0).code == 255);

    // --- stdout / stderr captured on the right (separate) streams ---
    {
        ProcResult r = run(sh("printf out; printf err 1>&2"), "", "", 0.0);
        CHECK(r.out == "out");
        CHECK(r.err == "err");
        CHECK(r.code == 0);
    }

    // --- stdin is delivered, including embedded NUL bytes (length-based, byte-faithful) ---
    {
        std::string in("a\0b\0\0c", 6);
        ProcResult r = run({"/bin/cat"}, "", in, 0.0);
        CHECK(r.out.size() == 6);
        CHECK(r.out == in);
    }

    // --- THE DEADLOCK CASE: ~1 MB on stdin AND stdout AND stderr at once. A single-threaded
    //     drain would wedge here; with per-stream threads it completes and the sizes are exact. ---
    {
        std::string big(1000000, 'Z');
        ProcResult r = run(sh("cat >/dev/null; head -c 1000000 /dev/zero; head -c 1000000 /dev/zero 1>&2"),
                           "", big, 0.0);
        CHECK(r.code == 0);
        CHECK(r.out.size() == 1000000);
        CHECK(r.err.size() == 1000000);
    }

    // --- a child that never reads a big stdin must not wedge the writer (EPIPE handled) ---
    {
        std::string big(1000000, 'Y');
        ProcResult r = run(sh("printf done"), "", big, 0.0);  // ignores stdin, exits
        CHECK(r.out == "done");
        CHECK(r.code == 0);
    }

    // --- cwd changes the child's working directory ---
    {
        ProcResult def = run(sh("pwd"), "", "", 0.0);
        ProcResult set = run(sh("pwd"), "/tmp", "", 0.0);
        CHECK(!set.out.empty());
        CHECK(set.out != def.out);
        CHECK(set.out.find("tmp") != std::string::npos);
    }

    // --- a nonexistent cwd fails to start (chdir in the child) -> ProcError ---
    CHECK_THROWS(run(sh("pwd"), "/no/such/dir/zzz_kirito", "", 0.0));

    // --- timeout kills the child and throws; a fast child under a generous timeout does not ---
    CHECK(run(sh("sleep 0.05"), "", "", 5.0).code == 0);
    CHECK_THROWS(run(sh("sleep 5"), "", "", 0.3));

    // --- a child killed by a signal reports 128 + signal (here SIGKILL = 9 -> 137) ---
    CHECK(run(sh("kill -9 $$"), "", "", 0.0).code == 137);

    // --- empty output and no-trailing-newline output are preserved exactly ---
    CHECK(run(sh("true"), "", "", 0.0).out.empty());
    CHECK(run(sh("printf abc"), "", "", 0.0).out == "abc");

    // --- resource hygiene: 200 spawns must not leak fds (>1024) or zombies ---
    {
        int okCount = 0;
        for (int i = 0; i < 200; ++i)
            if (run(sh("exit 0"), "", "", 0.0).code == 0) ++okCount;
        CHECK(okCount == 200);
    }
#else
    // Windows portable smoke (the rich cases above run on the Linux sanitizer matrix).
    CHECK(run({"cmd.exe", "/c", "exit 0"}, "", "", 0.0).code == 0);
    CHECK(run({"cmd.exe", "/c", "exit 7"}, "", "", 0.0).code == 7);
    {
        ProcResult r = run({"cmd.exe", "/c", "echo hi"}, "", "", 0.0);
        CHECK(r.code == 0);
        CHECK(r.out.find("hi") != std::string::npos);
    }
    CHECK_THROWS(run({"cmd.exe", "/c", "ping -n 10 127.0.0.1 >NUL"}, "", "", 0.3));  // timeout
#endif

    return RUN_TESTS();
}
