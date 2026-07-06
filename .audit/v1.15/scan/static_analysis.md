# v1.15 — Static Analysis + Performance Variance

Worker: static_analysis. Two jobs: (A) deep static analysis of C++, (B) perf-variance measurement.
Repo root: /home/user/kiritolang.github.io

## LOG
- Read BRIEFING. Tools present: clang-tidy 18, clang++ 18, g++ 13, cmake. cppcheck NOT installed.
  build-release exists with a `ki` binary.
- Starting (A) static analysis: max-warning compile of a small TU including kirito.hpp.

