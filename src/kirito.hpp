#ifndef KIRITO_HPP
#define KIRITO_HPP

// Umbrella header — `#include "kirito.hpp"` to embed Kirito in your own C++ program.
// This pulls in everything needed to construct a KiritoVM and run Kirito source. It does
// NOT define main(); the standalone interpreter lives in main.cpp.

#include "kirito/common.hpp"
#include "kirito/control.hpp"
#include "kirito/exceptions.hpp"
#include "kirito/handle.hpp"
#include "kirito/object.hpp"
#include "kirito/arena.hpp"
#include "kirito/builtins.hpp"
#include "kirito/collections.hpp"
#include "kirito/class_value.hpp"
#include "kirito/module.hpp"
#include "kirito/ast.hpp"
#include "kirito/bytecode.hpp"
#include "kirito/lexer.hpp"
#include "kirito/parser.hpp"
#include "kirito/environment.hpp"
#include "kirito/function.hpp"
#include "kirito/vm.hpp"
#include "kirito/compiler.hpp"
#include "kirito/value.hpp"
#include "kirito/native.hpp"
#include "kirito/bytes.hpp"
#include "kirito/stdlib_io.hpp"
#include "kirito/stdlib_math.hpp"
#include "kirito/stdlib_random.hpp"
#include "kirito/tensor.hpp"
#include "kirito/stdlib_matrix.hpp"
#include "kirito/stdlib_complex.hpp"
#include "kirito/stdlib_tensor.hpp"
#include "kirito/stdlib_json.hpp"
#include "kirito/stdlib_net.hpp"
#include "kirito/stdlib_serialize.hpp"
#include "kirito/stdlib_sys.hpp"
#include "kirito/stdlib_time.hpp"
#include "kirito/stdlib_dump.hpp"
#include "kirito/stdlib_zlib.hpp"
#include "kirito/stdlib_gzip.hpp"
#include "kirito/stdlib_hash.hpp"
#include "kirito/stdlib_crypto.hpp"
#include "kirito/stdlib_int.hpp"
#include "kirito/stdlib_regex.hpp"
#include "kirito/analyzer.hpp"
#include "kirito/resolver.hpp"
#include "kirito/runtime.hpp"
// The bytecode execution engine (the sole engine). AFTER runtime.hpp — it dispatches through the
// shared operation helpers (applyCall / applyBinaryOp / evalMemberGet / ...) runtime.hpp defines.
#include "kirito/bytecode_vm.hpp"
// Multiprocessing: the dispatcher and the `parallel` module. AFTER runtime.hpp — they call inline
// KiritoVM members (evalIn / importModule / install) that runtime.hpp defines.
#include "kirito/dispatcher.hpp"
#include "kirito/stdlib_parallel.hpp"

#endif
