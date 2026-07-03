// Carrier translation unit for a shared precompiled header. Its only job is to make CMake compile
// the umbrella header once per build; every test target then reuses that PCH (REUSE_FROM) instead
// of re-parsing the whole header-only interpreter in each of the ~60 test TUs — the dominant build
// cost. Intentionally empty otherwise.
#include "kirito.hpp"
