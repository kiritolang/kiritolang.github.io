# Discrepancies — scalar types None/Bool/Integer/Float (docs/pages/09-types.md vs src/kirito/{runtime,builtins,value}.hpp)

Overall the docs match the implementation closely — exact IEEE-754 `==`, NaN/inf/±0.0 behaviour,
floor-division/modulo sign rules, right-associative `**`, the `**` domain errors, base literals,
`.compare(other, rel_tol, abs_tol)` tolerance, Bool-is-not-Integer strong typing, and every hardening
error message were all confirmed byte-for-byte. One minor gap:

### types-scalars-1: a decimal integer literal exceeding INT64_MAX silently wraps
- kind: doc-missing (silent-ish behaviour, arguably intended)
- location: docs/pages/09-types.md:47-51 (Integer section) ; src/kirito lexer/number parsing
- expected (per docs): the docs say "Arithmetic wraps on overflow with well-defined two's-complement
  semantics." Only *arithmetic* overflow is documented; nothing is said about a *literal* whose
  decimal value exceeds `INT64_MAX`.
- actual: `9223372036854775808` (INT64_MAX + 1) lexes to `-9223372036854775808` (INT64_MIN) with no
  error and no diagnostic — the literal silently wraps. (Hex/oct/bin literals wrapping is expected and
  documented as "full-width two's-complement" in CLAUDE.md — `0xFFFFFFFFFFFFFFFF == -1` — but the
  decimal-literal case is not called out anywhere, so a user typing a too-big decimal constant gets a
  negative number with no warning.)
- resolution: test pins current behaviour (`assert 9223372036854775808 == imin`). A one-line doc note
  ("a decimal literal past 2**63-1 wraps like arithmetic; use hex for explicit bit patterns") would
  close the gap. No `src/` change (fixed-width int64 is by design; bigint literals are a listed future
  enrichment). Flagged, not fixed.

No genuine bugs, wrong messages, or unsound silent successes found for these four types.
