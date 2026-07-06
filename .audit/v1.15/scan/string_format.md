# v1.15 audit — String type, f-strings, format mini-spec

Subsystem: String method table (`src/kirito/runtime.hpp` StrVal::getAttr ~L1126-1561),
format mini-spec (`applyFormatSpec` ~L2660-2802), `format` builtin (~L3297), f-string
(`Op::FormatValue`/`BuildString` in bytecode_vm.hpp ~L273), reprString (builtins.hpp L262),
levenshtein (runtime.hpp L1106), string indexing/slicing (runtime.hpp L1024-1095).

Probe binary: ./build-debug/ki

## LOG
- Read StrVal::getAttr method table, getItem/slice/iterate, binary, applyFormatSpec, format builtin,
  reprString, levenshtein. Starting probes.
