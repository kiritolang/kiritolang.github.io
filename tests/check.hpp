#ifndef KIRITO_TEST_CHECK_HPP
#define KIRITO_TEST_CHECK_HPP

// Tiny assertion helpers for the test executables — no framework. Each test is its own
// program; CHECK records failures, RUN_TESTS() returns the process exit code (0 == green).
#include <cstdio>

namespace kitest {
inline int failures = 0;
}

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++kitest::failures;                                                  \
        }                                                                        \
    } while (0)

#define CHECK_THROWS(expr)                                                       \
    do {                                                                         \
        bool threw = false;                                                      \
        try {                                                                    \
            (void)(expr);                                                        \
        } catch (...) {                                                          \
            threw = true;                                                        \
        }                                                                        \
        if (!threw) {                                                            \
            std::printf("FAIL %s:%d  expected exception: %s\n",                  \
                        __FILE__, __LINE__, #expr);                              \
            ++kitest::failures;                                                  \
        }                                                                        \
    } while (0)

#define RUN_TESTS() (kitest::failures == 0 ? 0 : 1)

#endif
