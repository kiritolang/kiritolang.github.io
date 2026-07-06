# A16 — Audit: sys + time + proc_compat (v1.14)

Scope: src/kirito/stdlib_sys.hpp, src/kirito/stdlib_time.hpp, src/kirito/proc_compat.hpp
Status: IN PROGRESS

Prior findings merged (v1.13 A19): DateTime setEpoch narrowing (A19-1), pipe fd leak (A19-2),
Windows timeout UB (A19-3), env race (A19-4), exit-no-fstream-flush (A19-5), NUL-in-argv (A19-6),
sys.exit untested (A19-7), extreme-epoch (A19-8), NaN/neg timeout (A19-9), tz format directives (A19-10).
Hunting NEW angles.
