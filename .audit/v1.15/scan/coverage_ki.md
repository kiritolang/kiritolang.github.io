# v1.15 audit — .ki test coverage completeness

Subsystem: whether `tools/tests/scripts/*.ki` tests every builtin/type-method/stdlib symbol +
argument-combination + angle (adversarial/fuzz/edge/typical/malformed/random). Output: GAP LIST.

Method: (1) inventory Kirito-visible surface from docs (08-builtins, 09-types, 10-stdlib).
(2) survey `.ki` tests, map coverage. (3) gap list = under-tested symbols/args/angles.

Test families present (counts): spec 66, verify 39, probe 38, audit 24, amarg 24, labx 23,
r6 21, r8 16, r7 16, r4 15, r9 13, r10 8, deep 8, r5 6, r11 3, parallel 3, random 3, sys 5.
359 `.ki` scripts total. labx_* (23) is the recently-added layer — broad per-subsystem sweeps.

## LOG
- Starting: reading docs to build symbol inventory.

## OVERALL ASSESSMENT (interim)
The `.ki` suite is **very** comprehensive. Every module is imported by tests; every builtin is
referenced by dozens of files. Spot-checks of "obscure" documented edge cases all landed as
COVERED, including: string windowed find/rfind/index/count (start/end, negative, OOB), replace count,
split maxsplit, strip(chars); json duplicate-key last-wins, indent-too-large(>100), Infinity/NaN
round-trip; Counter.mostcommon(negative/0/n); BytesIO vs File seek whence semantics + negative
clamp-vs-throw; round(nan)/round(inf) throw vs round(nan,2) passthrough; abs(INT64_MIN) wrap;
math domain errors (log2(0), log1p, gamma(-1), lgamma(0), fmod); format numeric-flag-on-String
rejects; itertools compress/islice/cycle/starmap; functools partial/cache; bisect aliases.
The labx_* layer (23 files) plus deep_/audit_/r*_ give dense per-subsystem sweeps.

=> Symbol-level and common-edge coverage is essentially SATURATED. Remaining gaps are NARROW:
specific kwarg spellings, a few error/malformed paths, and huge-surface modules (tensor/net/regex)
where individual methods may lack adversarial angles. Focus the gap list there.

## GAP LIST
(appended as confirmed)
