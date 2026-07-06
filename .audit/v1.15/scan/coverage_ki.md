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

## GAP LIST
(appended as confirmed)
