#ifndef KIRITO_FUNCTION_HPP
#define KIRITO_FUNCTION_HPP

#include <functional>
#include <span>
#include <string>
#include <vector>

#include "ast.hpp"
#include "handle.hpp"
#include "object.hpp"

namespace kirito {

// A C++ callable exposed to Kirito: receives its args as handles plus the VM (to allocate
// results / dereference operands) and returns a handle. This is how built-in functions, bound
// methods, and embedder-registered functions all appear to the evaluator — identical to a
// Kirito function.
using NativeFn = std::function<Handle(KiritoVM&, std::span<const Handle>)>;

// (NamedArg — a keyword call argument — now lives in handle.hpp so the class/instance call paths
// can see it too; it is still spelled `kirito::NamedArg` everywhere.)

// A variadic native that ALSO receives keyword arguments (positional *args + named). Used for the
// few builtins that are both variadic and want named options (e.g. io.print(..., stream=f)).
using NativeFnKw = std::function<Handle(KiritoVM&, std::span<const Handle>, std::span<const NamedArg>)>;

// One declared parameter of a native function's signature, so native functions can describe
// themselves (for `inspect`) and accept keyword arguments / defaults exactly like Kirito functions.
// Construct as {"x"}, {"x", "Integer"} (annotated), or {"x", "Integer", defaultHandle} (with default).
struct NativeParam {
    std::string name;
    std::string annotation;     // "" if unannotated
    bool hasDefault = false;
    Handle defaultValue{};      // valid iff hasDefault
    NativeParam(std::string n, std::string ann = "") : name(std::move(n)), annotation(std::move(ann)) {}
    NativeParam(std::string n, std::string ann, Handle def)
        : name(std::move(n)), annotation(std::move(ann)), hasDefault(true), defaultValue(def) {}
};

class NativeFunction : public Object {
public:
    NativeFunction(std::string name, NativeFn fn, std::vector<Handle> captures = {})
        : name_(std::move(name)), fn_(std::move(fn)), captures_(std::move(captures)) {}
    // With a declared signature: enables keyword arguments, defaults, and a precise `inspect`.
    NativeFunction(std::string name, std::vector<NativeParam> sig, std::string returnType, NativeFn fn,
                   std::vector<Handle> captures = {})
        : name_(std::move(name)), fn_(std::move(fn)), captures_(std::move(captures)),
          sig_(std::move(sig)), returnType_(std::move(returnType)), hasSig_(true) {}
    // Variadic AND keyword-aware: keeps the raw positional protocol but also receives named args.
    NativeFunction(std::string name, NativeFnKw kwfn, std::vector<Handle> captures = {})
        : name_(std::move(name)), kwFn_(std::move(kwfn)), captures_(std::move(captures)),
          acceptsKwargs_(true) {}

    ValueKind kind() const override { return ValueKind::NativeFunction; }
    std::string typeName() const override { return "Function"; }
    bool truthy() const override { return true; }
    std::string str(StringifyCtx&) const override { return "<function " + name_ + ">"; }
    bool equals(const ObjectArena&, const Object& other) const override { return this == &other; }
    Handle call(KiritoVM& vm, std::span<const Handle> args) override {
        return acceptsKwargs_ ? kwFn_(vm, args, {}) : fn_(vm, args);
    }
    // Call with named arguments (only for acceptsKwargs() natives).
    Handle callKw(KiritoVM& vm, std::span<const Handle> args, std::span<const NamedArg> named) {
        return kwFn_(vm, args, named);
    }

    const std::string& name() const { return name_; }
    bool acceptsKwargs() const { return acceptsKwargs_; }
    bool hasSignature() const { return hasSig_; }
    const std::vector<NativeParam>& params() const { return sig_; }
    const std::string& returnType() const { return returnType_; }

    // Resolve positional + named call arguments against the declared signature into the flat
    // positional vector the implementation expects: positional fill, then keywords by name, then
    // defaults, with clear errors for too-many/unknown/duplicate/missing. Only callable when
    // hasSignature() — signatureless natives keep their raw positional protocol.
    std::vector<Handle> bindArgs(std::span<const Handle> positional,
                                 std::span<const NamedArg> named) const {
        std::vector<Handle> out(sig_.size());
        std::vector<bool> set(sig_.size(), false);
        if (positional.size() > sig_.size())
            throw KiritoError(name_ + "() takes at most " + std::to_string(sig_.size()) +
                              " positional argument(s) but " + std::to_string(positional.size()) + " given");
        for (std::size_t i = 0; i < positional.size(); ++i) { out[i] = positional[i]; set[i] = true; }
        for (const auto& na : named) {
            std::size_t idx = sig_.size();
            for (std::size_t i = 0; i < sig_.size(); ++i) if (sig_[i].name == na.name) { idx = i; break; }
            if (idx == sig_.size())
                throw KiritoError(name_ + "() got an unexpected keyword argument '" + na.name + "'");
            if (set[idx])
                throw KiritoError(name_ + "() got multiple values for argument '" + na.name + "'");
            out[idx] = na.value; set[idx] = true;
        }
        for (std::size_t i = 0; i < sig_.size(); ++i)
            if (!set[i]) {
                if (!sig_[i].hasDefault)
                    throw KiritoError(name_ + "() missing required argument '" + sig_[i].name + "'");
                out[i] = sig_[i].defaultValue;
            }
        return out;
    }

    // Bound methods capture their receiver here so the GC keeps it alive while the method exists;
    // default values are roots too.
    void children(std::vector<Handle>& out) const override {
        out.insert(out.end(), captures_.begin(), captures_.end());
        for (const auto& p : sig_) if (p.hasDefault) out.push_back(p.defaultValue);
    }

private:
    std::string name_;
    NativeFn fn_;
    NativeFnKw kwFn_;
    std::vector<Handle> captures_;
    std::vector<NativeParam> sig_;
    std::string returnType_;
    bool hasSig_ = false;
    bool acceptsKwargs_ = false;
};

// A Kirito-defined function value. It points at its AST definition (owned by the VM, which keeps
// every parsed chunk alive) and captures a handle to its defining scope — that capture is what
// makes closures work and is a GC root via children().
class KiFunction : public Object {
public:
    KiFunction(const ast::FunctionExpr* def, Handle closure) : def_(def), closure_(closure) {}

    const ast::FunctionExpr& def() const { return *def_; }
    Handle closure() const { return closure_; }

    // When this function is a class method, the class it belongs to (so its body may access
    // private members of instances of that class). hasOwner is false for plain functions.
    Handle ownerClass{};
    bool hasOwner = false;

    // The chunk (file / frozen-module name) this function was defined in, so an error escaping a
    // call is attributed to the right file — not to whichever script happened to invoke it.
    std::string sourceFile;
    // The module this function was defined in (its clean import name, or "" for main/stdlib). Carried
    // so a class defined inside this function is qualified against its DEFINING module, even when the
    // function is called from another module. See KiritoVM::currentModuleName.
    std::string moduleName;

    ValueKind kind() const override { return ValueKind::Function; }
    std::string typeName() const override { return "Function"; }
    bool truthy() const override { return true; }
    std::string str(StringifyCtx&) const override { return "<function>"; }
    bool equals(const ObjectArena&, const Object& other) const override { return this == &other; }
    void children(std::vector<Handle>& out) const override {
        out.push_back(closure_);
        if (hasOwner) out.push_back(ownerClass);
    }

    Handle call(KiritoVM&, std::span<const Handle> args) override;

    using NamedArg = kirito::NamedArg;  // call sites refer to KiFunction::NamedArg
    // Full call path supporting positional + named args, defaults, and annotation enforcement.
    // `hiddenLeading` is the count of implicit leading args the caller spliced in (1 for a bound
    // method's / constructor's `self`), subtracted from arity-error counts so the numbers match what
    // the user actually typed (A09-3).
    Handle callFull(KiritoVM&, std::span<const Handle> positional, std::span<const NamedArg> named,
                    std::size_t hiddenLeading = 0);

private:
    const ast::FunctionExpr* def_;
    Handle closure_;
};

}  // namespace kirito

#endif
