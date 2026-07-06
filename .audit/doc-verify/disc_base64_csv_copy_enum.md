# Discrepancies: base64 · csv · copy · enum (stdlib_kimodules.hpp)

Doc source: `docs/pages/10-stdlib.md`. Impl: `src/kirito/stdlib_kimodules.hpp`.
Tests: `tools/tests/scripts/verify_{base64,csv,copy,enum}.ki` (+ `.expected`).
Verified against `build-debug/ki`. **No `src/` edits made.**

## Summary

| Module | Doc-vs-impl | Notes |
|--------|-------------|-------|
| base64 | matches | Behaviour is stricter/richer than the doc describes (hardening, `=` handling) — pinned, not a defect |
| csv    | matches | `\r` handling and trailing-newline behaviour under-documented — pinned |
| copy   | matches | Shallow/deep and cycle-safety all as documented |
| enum   | **FINDING** | Duplicate member names silently corrupt the enum (no uniqueness check) |

---

## enum — FINDING: duplicate names silently corrupt the enum (no error)

`Enum(names)` (class `Enum._init_`) iterates `names` and assigns each an incrementing index,
populating `_byName`, `_byValue`, and `_order` with **no uniqueness check**. A duplicated name
therefore silently corrupts the enum instead of raising:

```
var dup = enum.Enum(["A", "B", "A"])
dup.get("A")    == 2            # last occurrence wins; the original index 0 is unreachable via get()
dup.names()     == ["A","B","A"]  # duplicate leaks into names()
dup.values()    == [2, 1, 2]      # index 0 is GONE, 2 is duplicated — no longer a clean 0..n-1 range
dup.nameof(0)   == "A"          # _byValue still holds 0->"A" (the shadowed name), inconsistent with get()
enum.Enum(["X","X"])            # builds WITHOUT any error
```

**Impact:** `values()` no longer returns the contiguous `0..n-1` index set the doc implies, `get()`
and `nameof()` disagree for the shadowed value, and the corruption is silent (no throw). Python's
`enum.Enum` raises on duplicate member names; a `throw` in `_init_` on a repeated name would be the
correct guard.

**Status:** current behaviour PINNED in `verify_enum.ki` (the "FINDING" block asserts each of the
corrupt values so a future fix will flip the test and be noticed). Not fixed here — `src/` is
off-limits for this pass; flag for a follow-up defect fix.

---

## base64 — matches (behaviour richer than doc; pinned)

Doc: `encode(List|Bytes|String)->String`, `decode(String)->List`, `urlsafeencode`/`urlsafedecode`.
All exact RFC-4648 vectors pass (`""`,`"f"`→`Zg==`, …, `"foobar"`→`Zm9vYmFy`; `"Hi"`→`SGk=`).
`decode` returns a **List of Integer byte values** (not Bytes) as documented. Behaviours the doc
does not spell out, now pinned:

- **`decode` stops at the first `=`** and ignores the remainder: `decode("SGk=extra") == [72,105]`.
- **Padding-less input decodes** when it has zero leftover bits: `decode("SGk") == [72,105]`.
- **Whitespace is rejected** (the audit's note confirmed): the decoder has no whitespace tolerance,
  so `decode("SG k=")` throws `invalid base64 character: ' '`. Standard-alphabet decode also rejects
  the url-safe chars `-`/`_`, and url-safe decode rejects `+`/`/`.
- **Length/corruption guards throw** (not silent): a lone trailing char (6 leftover bits) throws
  `invalid base64: a lone trailing character (invalid length)`; non-zero leftover bits throw
  `invalid base64: truncated or corrupted input` (`decode("S")`, `decode("SB")`, `decode("SG=")`).

No discrepancy — decode is *stricter* than a permissive base64 (a good thing). Consider documenting
the `=`-stop and no-whitespace behaviour.

---

## csv — matches (edge behaviour pinned)

`parse`/`parserow`/`format`/`formatrow`, RFC-4180-style quoting (quote a field containing `,` `"`
`\n` `\r`; double embedded quotes). Exact behaviour pinned:

- **`parse("")` → `[]`** (no rows); a **trailing newline creates no spurious empty row**
  (`parse("a,b\n") == [["a","b"]]`).
- **Lone `\r` is skipped** (CRLF collapsed to LF): `parse("a\rb") == [["ab"]]`; `\r\n` is a clean
  row terminator. `parserow` does NOT skip `\r` (it only splits on `,`), but a `\r` inside a field
  is preserved and triggers quoting on `formatrow`.
- **`parserow("")` → `[""]`** (one empty field), whereas **`parse("")` → `[]`** — a deliberate
  asymmetry (a lone empty line vs. empty document).
- **`formatrow` stringifies non-strings** via `String(f)`: `formatrow([1,2.5,True]) == "1,2.5,True"`,
  and a nested container stringifies then quotes (contains `,`): `formatrow([[1,2]]) == '"[1, 2]"'`.
- **`parse` never throws** on malformed quoting; an unterminated quote absorbs the rest of the text.
- Round-trip `parse(format(rows)) == rows` holds for quotable data (commas, embedded quotes, newlines).

No discrepancy. Suggest documenting the `parse`/`parserow` empty-input asymmetry and the `\r` rule.

---

## copy — matches

- `copy` of `List`/`Dict`/`Set` is **shallow** (new top container, nested containers shared by id);
  immutable scalars (`None`/`Bool`/`Integer`/`Float`/`String`/`Bytes`) returned by identity.
- `deepcopy` is **deep and cycle-safe**: nested containers independent; **shared references preserved**
  (`both = [shared, shared]` → `id(db[0]) == id(db[1])`); **self-referential structures do not hang**
  and the cycle is re-created pointing at the copy (`cyc.append(cyc)` → `id(dcyc[2]) == id(dcyc)`);
  cyclic dicts likewise.
- A **class instance** is copied via the `serialize` graph codec, so **both `copy` and `deepcopy`
  return a DEEP, independent instance** (`copy(Box([1,2]))` has an independent `.v`) — exactly as the
  doc states (Kirito has no per-instance attribute introspection).

No discrepancy.
