# v1.15 audit — DRY / single-source-of-truth scan

Subsystem: duplicated logic across src/kirito/*.hpp that could drift; constants defined in
multiple places; parallel implementations that should share a helper.

## LOG
- Starting scan.

## FINDINGS
### F1 [MED] Three verbatim String-or-Bytes byte-extraction helpers (drift risk on error msg/semantics)
- where:
  - src/kirito/bytes.hpp:342 `argStringOrBytes` (returns `const std::string&`)
  - src/kirito/stdlib_io.hpp:25 `ioRawBytes` (returns `const std::string&`)
  - src/kirito/stdlib_net.hpp:192 `payloadBytes` (returns `std::string` BY VALUE — copies)
- duplicated: identical body — String -> `.value()`, BytesVal -> `.data`, else throw
  `who + " expects a String or Bytes"`. bytes.hpp already advertises itself as the shared home
  ("shared by the hash/zlib/gzip modules so each needn't re-roll it") yet io & net re-rolled it.
- drift: net's `payloadBytes` returns by value (a needless copy on every send) vs the other two
  returning a reference — already a minor perf divergence. Any future fix (e.g. accept a List of
  ints, or a bytearray) must be applied in 3 places. Error text is identical today (good) but each
  copy can drift independently.
- fix idea: delete ioRawBytes/payloadBytes; call bytes.hpp::argStringOrBytes everywhere (net keeps
  a copy only if it needs to own the buffer, which it doesn't — sendAll takes const&).
