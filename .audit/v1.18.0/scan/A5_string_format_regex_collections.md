# A5 — String / format / regex + core collections (List/Dict/Set/Bytes)

Probed on build-debug/ki + build-asan/ki; cross-checked vs CPython where docs claim parity.

## Findings
- MED — Set membership `in`/`contains`/`discard` swallowed the unhashable error (returned False /
  no-op) that Dict `in` and Set `add`/`remove` raise (SetHash::contains/remove did
  `!hashable() → false`, bypassing the `requireHashable` SSOT). → FIXED (#6).
- LOW/MED — a NaN Dict/Set key was insertable but unfindable/unremovable, and re-assignment
  fabricated a duplicate key (broke key-uniqueness). Deliberate exact-== float-key design, but the
  invariant break was real. → FIXED (#7, maintainer chose reject-at-insert).
- LOW — docs called `\xHH` "a byte"; it is code point U+00HH (UTF-8-encoded). → FIXED (#8).

## VERIFIED CORRECT
- Format-spec mini-language vs CPython: `#o`/`#x`, `.0g`, `.1%`, `,` grouping, `08.2f` sign-aware
  zero-pad, `.2e`, align; string specs reject `+`/`#`/`=`/`,`; multibyte fill throws (no UTF-8
  corruption). `.format()` positional/`{N}`/`{x}` behavior matches docs.
- String methods (split/replace/count/find/index/rfind/strip/join/startswith/endswith/partition/
  ljust/center/zfill/removeprefix/levenshtein) match the doc table; empty-pattern + overlapping count,
  code-point slicing/reverse/negative slices on multibyte all correct.
- Sort: stable, key/reverse, NaN total-order (sorts last), clean under ASan (no strict-weak-order UB).
- Collections: negative/OOB index throws, slice `[::0]` throws, extended-slice-assign size check,
  insert clamp, pop-empty throws, `*`-repeat overflow guard, dict get/pop/setdefault/update, set
  algebra, insertion-order preserved across delete/re-insert.
- Bytes: index/slice/OOB, encode/decode error paths raise structured messages with the offending
  code point.
- Regex (from-scratch linear-time): `(a+)+$` / `(a*)*b` on 40×"a" fast (no catastrophic backtracking);
  backreferences rejected; greedy/lazy, alternation, named groups/groupdict, `sub` templates, `split`
  retaining separators, empty-pattern sub/split/findall, `.` matching a multibyte code point,
  MULTILINE `^`/`$`, `\b`, `{n,m}`, inline flags. ASCII-only `\w` is documented.
