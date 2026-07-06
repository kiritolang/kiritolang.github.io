# A09 — String + Bytes (v1.14 audit)

**Status: IN PROGRESS**

Subsystem: `src/kirito/builtins.hpp` (StrVal + utf8 helpers), `src/kirito/runtime.hpp` (String
methods + format mini-spec), `src/kirito/bytes.hpp` (Bytes), encode/decode.

Runner: `/home/user/kiritolang.github.io/build-debug/ki`

## Map
- StrVal class: `builtins.hpp:164`. UTF-8 lenient decode (`utf8Starts`, `utf8DecodeAt`) — invalid
  UTF-8 never throws at construction; strings are byte-transparent.
- String methods: `runtime.hpp` (`getAttr` from ~1196).
- Format mini-spec: `runtime.hpp`.
- Bytes: `bytes.hpp`.

## Findings
(recorded incrementally below)

