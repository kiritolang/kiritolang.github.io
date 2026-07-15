# A10 Serialization

**Status:** IN PROGRESS. Scanner A10, Kirito v1.15 audit — the NEW function/class value serialization.

Files: `src/kirito/stdlib_serde.hpp` (core flatten/rebuild), `stdlib_serialize.hpp` (KSER1 text),
`stdlib_dump.hpp` (KDMP binary). Cross-ref `locals.hpp` (freeVariables/eagerFreeVariables), parser
source-capture, `environment.hpp` (define barrier — confirmed barriered).

Baseline reading complete. `env.define` is barriered (environment.hpp:44). Pass-2 List/Set/Dict wires
use barriered `append/add/set`; Object attrs explicitly `gcWriteBarrier`. Depth guard 8000/1500 in
flatten's `visit`. Bytes/Matrix/DateTime/Random/Tensor are native `Instance`s serialized via
`_getstate_`/`_setstate_` (Stateful tag) + registered deserializer.

---
