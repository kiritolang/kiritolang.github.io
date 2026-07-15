#ifndef KIRITO_BYTECODE_HPP
#define KIRITO_BYTECODE_HPP

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ast.hpp"
#include "common.hpp"
#include "fum/unordered_map.hpp"
#include "handle.hpp"

namespace kirito {

// --- The bytecode intermediate representation -------------------------------------------------
//
// Kirito's second back end (behind the stable AST boundary): a compiler turns a Block of statements
// into a flat Proto of stack-machine instructions, and a BytecodeVM executes it with an explicit
// operand stack instead of native recursion. The value model (Object protocol), operator dispatch,
// call protocol, and GC are all reused; bytecode owns only the control structure. It is the sole
// execution engine — the program runs by compiling each body to a Proto and executing it.
//
// Compilation is per body (a function body, the top-level program, a class body) and lazy: a nested
// function literal is emitted as MakeFunction(funcExpr*) and its own body is compiled on first call.
// The compiler handles every AST node; a genuine program error (a deep nest, an invalid assignment
// target, ...) is thrown as a KiritoError, exactly like a parser diagnostic.

enum class Op : uint8_t {
    LoadConst,        // a: push consts[a]
    LoadNone,         //    push the None singleton
    LoadName,         // a: push the value bound to names[a] (NameError if undefined)
    LoadGlobal,       // a: push global slot a — a builtin/global resolved at compile time (O(1), no walk)
    LoadVar,          // a: (envVars[a]=depth,index,name) push the (depth-up, index) env slot — O(1), no walk
    AssignVar,        // a: (envVars[a]) rebind the (depth-up, index) env slot = pop() — O(1), no walk
    StoreName,        // a: define names[a] = pop() in the current scope (var)
    AssignName,       // a: rebind the nearest existing names[a] = pop() (NameError if undefined)
    LoadLocal,        // a: push frame slot a (a non-captured function local); unwritten -> "not defined"
    StoreLocal,       // a: frame slot a = pop()  (a non-captured function local — var/for/with/catch)
    AssignLocal,      // a: frame slot a = pop(); if unwritten (rebind before its var ran) -> "not defined"
    Pop,              //    discard the top of stack
    Dup,              //    push a copy of the top of stack
    UnaryOp,          // a: (UnOp) replace top with op(top)
    BinaryOp,         // a: (BinOp) rhs=pop, lhs=pop, push op(lhs, rhs)
    Jump,             // a: ip = a
    PopJumpIfFalse,   // a: v=pop; if not truthy(v): ip = a
    PopJumpIfTrue,    // a: v=pop; if truthy(v): ip = a
    JumpIfFalseOrPop, // a: if not truthy(peek): ip = a   else pop   (`and`)
    JumpIfTrueOrPop,  // a: if truthy(peek): ip = a       else pop   (`or`)
    SetResult,        //    frame.result = pop()   (an expression statement's value)
    ClearResult,      //    frame.result = None    (every other statement)
    LoadResult,       //    push frame.result      (top-level program return value)
    Call,             // a: dispatch calls[a]; the callee then its args are on the stack
    MakeFunction,     // a: push a closure of funcs[a] capturing the current scope
    BuildClass,       // a: (classes[a]) base on stack (if any) -> build the class, bind its name
    GetAttr,          // a: replace top with top.names[a]  (member read; handles _super_/privacy)
    SetAttr,          // a: value=pop, obj=pop; obj.names[a] = value
    GetItem,          // a: (key count) keys then obj... -> push obj[keys]
    SetItem,          // a: (key count) value, keys, obj -> obj[keys] = value
    GetSlice,         //    step,stop,start,obj on stack -> push obj[start:stop:step]
    BuildList,        // a: (count) pop count items -> push a List
    BuildSet,         // a: (count) pop count items -> push a Set
    BuildDict,        // a: (count) pop count key/value pairs -> push a Dict
    BuildPack,        // a: (count) pop count items -> push a List (bare-comma packing)
    FormatValue,      // a: (names[a] = spec, "" for none) replace top with its formatted String
    BuildString,      // a: (count) concatenate count Strings on the stack -> push the joined String
    GetIter,          //    replace the top iterable with an internal iteration cursor
    ForIter,          // a: advance the cursor on top; if exhausted pop it and ip=a, else push next item
    Unpack,           // a: (unpacks[a]) pop an iterable -> push its n spread slots, first target (slot 0) on top
    SwitchMatch,      //    v=pop, subj=pop -> push Bool(subj and v are the same scalar by type+value)
    SwitchDispatch,   // a: (switches[a]) pop subject -> ip = the arm offset for key(subject), else default (O(1))
    SetupBlock,       // a: push an exception block (try/with): on a throw, unwind here with the exc value
    PopBlock,         //    pop the innermost exception block (left normally)
    Reraise,          //    pop an exception value -> re-throw it (unmatched catch / after a finally)
    SaveExcSpan,      // a: excSpans[a] = the in-flight exception's span   (park it across a finally)
    RestoreExcSpan,   // a: the in-flight exception's span = excSpans[a]   (unpark it, before Reraise)
    ExcMatch,         //    type=pop, exc=pop -> push Bool(exc is an instance of the class `type`)
    Throw,            //    pop -> throw it as a Kirito exception (assert/throw)
    Return,           //    pop -> return it from this frame
};

// A destructuring shape for the Unpack opcode: how many targets, and which (if any) is the starred
// one that absorbs the surplus into a List (-1 = none). Mirrors VarDecl/For/tuple-assign unpacking.
struct UnpackSpec {
    uint32_t count = 0;
    int32_t starIndex = -1;
};

// A switch's compile-time dispatch table: every (literal-scalar) case value's key mapped to the
// bytecode offset of its arm, plus the default offset. Built once by the compiler so SwitchDispatch
// runs in O(1) — hash the subject's key once and jump — instead of an O(n) per-case comparison chain.
// (A switch with any non-literal case value falls back to the SwitchMatch comparison chain instead.)
struct SwitchTable {
    fum::unordered_map<std::string, uint32_t> targets;  // scalar key (scalarSwitchKey form) -> arm offset
    uint32_t defaultTarget = 0;                          // arm to run when no case key matches
};

// A resolved lexical reference into an indexed env scope: how many EnvValue hops up from the running
// frame's scope to the owning scope, the binding's fixed slot there, and its name (for a clean
// "referenced before assignment" diagnostic and a debug-only slot-name assertion). Produced by the
// resolver, which guarantees the (depth, index) by construction — module scopes read the index back
// from the live scope, function scopes share one ordered-layout helper between annotation and the
// runtime slot pre-declaration — so no runtime name lookup is ever needed.
struct EnvVarRef {
    uint16_t depth = 0;
    uint32_t index = 0;
    std::string name;
};

// A call site's static shape: how many leading positional args, then the names of the trailing
// keyword args (in the order their values are pushed). Reconstructed into positional span + NamedArg
// list at run time. Kept out of the instruction so the hot loop stays compact.
struct CallSpec {
    uint32_t positional = 0;
    std::vector<std::string> names;  // keyword argument names, value-push order
};

struct Instr {
    Op op;
    uint32_t a = 0;
    SourceSpan span{};  // for diagnostics: a runtime error is tagged with this node's location
};

// A compiled body. consts hold materialised literal values (pinned as GC roots by the VM that built
// the Proto); names/funcs/calls are the operand side tables the instruction stream indexes into.
struct Proto {
    std::vector<Instr> code;
    std::vector<Handle> consts;                       // LoadConst targets (GC-pinned by the VM)
    std::vector<std::string> names;                   // identifiers (names/attrs/format specs)
    std::vector<const ast::FunctionExpr*> funcs;      // MakeFunction targets
    std::vector<CallSpec> calls;                      // Call targets
    std::vector<UnpackSpec> unpacks;                  // Unpack targets
    std::vector<const ast::ClassStmt*> classes;       // BuildClass targets (name/base/body)
    std::vector<SwitchTable> switches;                // SwitchDispatch targets (compile-time case tables)
    std::vector<EnvVarRef> envVars;                    // LoadVar/AssignVar targets (depth,index into an env scope)
    std::vector<std::string> envSlots;                 // captured non-param locals to pre-declare in the scope env
    uint32_t excSpanSlots = 0;                         // Save/RestoreExcSpan slots (one per try-with-finally)
    uint32_t localCount = 0;                           // frame slots to reserve for slot-addressed locals
    std::vector<std::string> localNames;              // slot -> name (for "referenced before assignment" errors)
    std::vector<int> paramSlots;                       // param i -> its frame slot, or -1 if captured (name-based)
};

class KiritoVM;

// Compile (if needed) and execute a body against `scope`, returning its result. `isFunction` selects
// the implicit tail (a function returns None on fall-through; the top-level program / a class body
// returns its last expression). Defined in bytecode_vm.hpp (after the runtime), so the call sites
// (KiFunction::callFull / KiritoVM::evalIn / the module loaders) only need this declaration.
Handle runBytecodeBody(KiritoVM& vm, Handle scope, const ast::Block& body, Handle ownerClass,
                       bool hasOwner, bool isFunction, std::string frameLabel = "<module>",
                       const ast::FunctionExpr* fnDef = nullptr,
                       std::span<const Handle> paramValues = {});
// Compile and evaluate a single expression against `scope` (a parameter's default value).
Handle runBytecodeExpr(KiritoVM& vm, Handle scope, const ast::Expr& e);

}  // namespace kirito

#endif
