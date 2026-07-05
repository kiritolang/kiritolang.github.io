# Doc-vs-implementation discrepancies & silent bugs — consolidated

Roll-up of the label-X doc-verification pass. 39 `verify_<module>.ki` tests (**3201 assertions**) cover
every documented builtin, type, operator/dunder, and all 35 stdlib modules — functioning, edge, and
hardening (right error message; silent-error probes). Per-module raw notes live in `disc_*.md`.

**Coverage:** 360 / 361 documented `name(...)` symbols are exercised by the new tests; the 1 remainder
(`net.delete`, the HTTP DELETE verb) needs a live server and is covered by the existing server-driven
`r10_net` / `r8_net_regex` / `spec_net_tls` suites. Attributes and the full method surface were each
verified per module. Full debug CTest **768/768 green**.

## Real bugs found — FIXED in src (with the verify tests flipped to the corrected behaviour)

| # | Bug | Fix |
|---|-----|-----|
| 1 | **Bytes ordering**: `sorted()`/`min()`/`max()` threw `cannot order 'Bytes'` although the `< <= > >=` operators (and the docs) order Bytes lexicographically. | `runtime.hpp` `kiLessThan` now has a Bytes branch (unsigned-byte order). |
| 2 | **enum duplicate names** silently corrupted the map — `Enum([...,"A",..,"A"])` overwrote the first ordinal, so `get`/`nameof` disagreed and `values()` was no longer `0..n-1`. | `stdlib_kimodules.hpp` `Enum._init_` throws `duplicate enum member: X`. |
| 3 | **net `recv(0)` blocked** — a raw `::recv(fd, buf, 0)` blocks on Linux until data/EOF. | `stdlib_net.hpp` `recv(0)` returns empty Bytes immediately (Python semantics); still errors on a closed socket. |
| 4 | **tabular `Series.all()`/`any()`** were documented as the boolean-Series reduction but did not exist. | `stdlib_kimodules.hpp` — implemented (skipna; vacuous-True `all`, vacuous-False `any`). |

## Doc fixes (impl was correct; docs were wrong)

- **`DataFrame.rowat(pos)`** returns a **Series** (indexed by column name), not a Dict — doc corrected.
- Minor doc-notes recorded per module (e.g. decimal int-literal wraparound is undocumented but by-design;
  base64 `decode` is stricter than documented; csv `parse("")` vs `parserow("")` asymmetry) — left as
  pinned in-test, no behaviour change.

## Pinned by-design behaviours (NOT bugs — flagged so a future change trips a test)

Recorded across the `disc_*.md` files; the notable ones:
- **Bool is not numeric** — `sum([True, False])`, `mean([True])`, `0.0 + True` all raise (strong typing).
- **`multimode([])` → `[]`** while `mode([])` raises (intentional asymmetry).
- **Scalar-on-the-left tensor arithmetic** (`2 * t`) raises — no reflected dunder (documented invariant).
- **`_str_` returning a non-String** is silently re-stringified (unlike the return-checked `_bool_`/`_hash_`/`_len_`).
- **Set `in` with an unhashable element** returns False silently while Dict `in` throws (membership asymmetry).
- **`uniform(a, b)` with `a > b`** returns a value in the reversed range (matches Python).
- **ASCII-only `\w`/`\d`/`\s`** in regex; `escape` treats cp>127 as wordish (documented asymmetry).
- **IPv6** socket creation is unavailable in some sandboxes (`verify_net` gates those asserts behind a probe).

These join the cross-round false-positive list in `../README.md`.
