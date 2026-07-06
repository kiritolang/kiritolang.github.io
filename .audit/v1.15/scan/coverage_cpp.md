# v1.15 audit — C++ test coverage completeness

Subsystem: whether `tools/tests/unit/*.cpp` exercises the whole public C++ surface
(`value.hpp` Value/wrapper API, `native.hpp` extension points, `vm.hpp`/`kirito.hpp` embedding)
AND Kirito-visible behavior, from every angle (happy / edge / error / GC).

Repo root: /home/user/kiritolang.github.io. 126 unit test files.

Task = produce a GAP MAP. Do NOT write tests. Name symbol, file:line, missing angle.

## LOG
- Starting: enumerate value.hpp surface, then map tests.
