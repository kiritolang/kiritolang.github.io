# A04 Bytecode VM

Status: IN PROGRESS

Scope: src/kirito/bytecode_vm.hpp, src/kirito/control.hpp; cross-referencing runtime.hpp and
compiler.hpp (SetupBlock/PopBlock/finally/with codegen) since the two co-design the block stack.

Method: static read + adversarial `.ki` snippets run against build-debug/ki (nested
try/finally/with/loops with return/break/continue/throw at every level, deep recursion,
higher-order natives, GC pressure).

