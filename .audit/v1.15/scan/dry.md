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

### F2 [MED] The "256 MiB ceiling" resource policy is re-declared in 7+ places (two hardcoded literals)
- common.hpp:159 `kMaxRepeat = 256ull*1024*1024` — comment literally claims "Single source of truth"
  but only for String/List/Bytes repetition/padding/range. The SAME 256 MiB ceiling is separately
  re-declared elsewhere, all conceptually the same "far-beyond-any-scripting-use" cap:
  - src/kirito/bytes.hpp:321  `256ull * 1024 * 1024` HARDCODED literal ("Bytes too large")
  - src/kirito/stdlib_random.hpp:234 `256ull * 1024 * 1024` HARDCODED literal (comment says
    "matching runtime.hpp's kMaxRepeat" — but copies the number instead of referencing it)
  - src/kirito/stdlib_io.hpp:260  `kMaxBuf = 256ull*1024*1024` ("matches Bytes")
  - src/kirito/proc_compat.hpp:52 `kMaxCapture = 256ull*1024*1024`
  - src/kirito/stdlib_net.hpp:212 `kMaxRecvAll = 256ull*1024*1024`
  - src/kirito/deflate.hpp:224   `kMaxInflateOut = 256ull*1024*1024`
  - src/kirito/stdlib_sys.hpp:57 error text hardcodes "256 MiB" (couples the MESSAGE to the number —
    if kMaxCapture changes, the message lies; ALREADY a drift-in-waiting)
- drift risk: raising/lowering the policy (or making it configurable) requires touching all of these;
  the two hardcoded literals (bytes.hpp:321, random.hpp:234) will silently be missed. bytes.hpp:321
  could reference common.hpp::kMaxRepeat directly (bytes already includes common per F-context).
- fix idea: one `kByteCap`/`kMemCap` in common.hpp; have the module caps reference it (or explicitly
  document why each differs). Note stdlib_tensor.hpp:200 already does this right
  (`kMaxElems = tensor::kMaxElems`).

### F3 [LOW] matrix and complex each define an identical 16Mi element cap independently
- src/kirito/stdlib_matrix.hpp:104 `kMaxMatrixElems = 16ull*1024*1024`
- src/kirito/stdlib_complex.hpp:269 `kMaxElems = 16ull*1024*1024`
- Both are the "2-D matrix built on the tensor engine" cap (tensor engine itself is 64Mi at
  tensor.hpp:49). Same value, same intent, two copies + duplicated guard code
  (matrix.hpp:106/110/313 vs complex.hpp:271/275/498 are structurally identical guards).
- drift: if one is bumped the two matrix families diverge silently. Low sev (correctness unaffected).
