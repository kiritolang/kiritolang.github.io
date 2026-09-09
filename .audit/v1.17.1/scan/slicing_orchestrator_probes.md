# v1.17.1 deep audit — orchestrator direct probes (slicing feature)

Probed personally against `build-bin/ki-asan` (ASAN/UBSan) while the subsystem auditors ran. These
cover edges the three fan-out auditors (tensor / language-machinery / collections+docs) don't own.

## Verified CORRECT — Slice/Ellipsis are sealed off from serde/arith/hash (no silent-failure path)
Reproducer `/tmp/sl_serde.ki`, all clean rejections (no crash, no silent garbage):
- `serialize.dumps(slice)` → "cannot serialize type 'Slice'"; `dump.dumps` → "cannot dump type
  'Slice'"; `json.dumps` → "cannot serialize 'Slice' to JSON"; same for Ellipsis. So a Slice can
  never be silently mis-persisted.
- Arithmetic: `1 + slice` → "unsupported operand type 'Slice'"; `slice * 2` → clean binary-op error.
- Hashing: `slice` as a Dict key or Set member → "unhashable type 'Slice'" (clean). Cannot corrupt a
  hash container.
- `len(slice)` → "type 'Slice' has no length" (clean).
- `copy.copy`/`copy.deepcopy` of a Slice → `slice(1, 8, 2)` (meaningful); `deepcopy(...)` → `...`
  (Ellipsis singleton). Equality is structural (`slice(1,2,3)==slice(1,2,3)` True, vs `..4` False);
  `... == ...` True.

## Verified CORRECT — the parser LBracket rewrite caused NO subscript regression
Reproducer `/tmp/subscript.ki`, all correct:
- `xs[2]`, `xs[-1]`, `xs[1+1]`, `d["a"]`, `m["x"][1]`, `[5,6,7][0]`, `[1,2,3,4][1:3][0]`, `fn()[2]`,
  `"hello"[1]`, `"hello"[-3:]`, user-class variadic `grid[1,0]`/`grid[0,1]`, `aa[1]=20`.
- `Dict` multi-key `mm[1,2]=99` → "type 'Dict' takes exactly one index" (correct per docs — multi-key
  is a user-type protocol, not built-in Dict).
- `xs[1,]` (trailing comma) → clean parse error "expected an index or slice inside '[ ]'". LOW/none:
  not a crash; Kirito has no tuple-index model so a 1-tuple subscript isn't a thing. Acceptable.

## Nothing actionable found in these areas.
