#ifndef KIRITO_ENVIRONMENT_HPP
#define KIRITO_ENVIRONMENT_HPP

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "arena.hpp"
#include "object.hpp"

namespace kirito {

// A lexical scope: name -> Handle, plus a handle to the enclosing scope. Environments are heap
// objects (addressed by handle) so a closure can capture and outlive the frame that made it, and
// so the whole scope chain stays GC-traceable and serializable. The chain is global -> module ->
// (later) function-local.
//
// Bindings live in a flat vector rather than a hash map: function-call scopes are tiny (a handful
// of names), so a linear scan with no per-binding heap allocation is markedly faster than an
// unordered_map (which mallocs a control block + nodes on every call). This is the single hottest
// data structure in the interpreter — see the call-heavy benchmark profile.
class EnvValue : public Object {
public:
    EnvValue() : hasParent_(false) {}
    explicit EnvValue(Handle parent) : parent_(parent), hasParent_(true) {}

    ValueKind kind() const override { return ValueKind::Environment; }
    std::string typeName() const override { return "Environment"; }
    bool truthy() const override { return true; }
    std::string str(StringifyCtx&) const override { return "<environment>"; }
    bool equals(const ObjectArena&, const Object& other) const override { return this == &other; }
    void children(std::vector<Handle>& out) const override {
        out.reserve(out.size() + vars_.size() + (hasParent_ ? 1 : 0));
        for (const auto& [name, h] : vars_) out.push_back(h);
        if (hasParent_) out.push_back(parent_);
    }

    bool hasParent() const { return hasParent_; }
    Handle parent() const { return parent_; }

    // Define (or overwrite) a binding in this scope.
    void define(const std::string& name, Handle h) {
        gcWriteBarrier(this, h);   // an old scope (global/module/class-body) gaining a young binding
        for (auto& [k, v] : vars_)
            if (k == name) { v = h; return; }
        vars_.push_back(name, h);
    }
    bool assignLocal(const std::string& name, Handle h) {
        for (auto& [k, v] : vars_)
            if (k == name) { gcWriteBarrier(this, h); v = h; return true; }
        return false;
    }
    const Handle* findLocal(const std::string& name) const {
        for (const auto& [k, v] : vars_)
            if (k == name) return &v;
        return nullptr;
    }
    // Iterable view of the bindings (used to snapshot class methods / module members).
    const auto& locals() const { return vars_; }
    // Positional access to the i-th binding — the O(1) path behind LoadGlobal/LoadVar(index). Valid
    // for a scope whose bindings only ever grow by append (global scope, and a module/function scope
    // once its slots are pre-declared), so index i is stable once assigned. `nameAt` backs the
    // debug-only slot-name assertion and the compiler's env-index read-back; `setAt` is the O(1)
    // StoreVar/AssignVar write.
    Handle at(std::size_t i) const { return vars_[i].second; }
    const std::string& nameAt(std::size_t i) const { return vars_[i].first; }
    void setAt(std::size_t i, Handle h) { gcWriteBarrier(this, h); vars_[i].second = h; }
    std::size_t size() const { return vars_.size(); }

    void reserve(std::size_t n) { vars_.reserve(n); }

private:
    using Binding = std::pair<std::string, Handle>;
    // A small-buffer vector: function-call scopes hold only a few bindings (params + a couple of
    // locals), so we keep up to kInline of them inline and avoid the per-call heap allocation a
    // std::vector would do on its first push. Spills to the heap only for larger scopes (module,
    // class body, a big function). Eliminating this malloc roughly halves call-path allocations.
    static constexpr std::size_t kInline = 4;
    class SmallVec {
    public:
        SmallVec() = default;
        SmallVec(const SmallVec& o) { for (std::size_t i = 0; i < o.size_; ++i) push(o[i].first, o[i].second); }
        SmallVec& operator=(const SmallVec& o) {
            if (this != &o) { clear(); for (std::size_t i = 0; i < o.size_; ++i) push(o[i].first, o[i].second); }
            return *this;
        }
        ~SmallVec() { clear(); }

        std::size_t size() const { return size_; }
        Binding* data() { return heap_ ? heap_ : inlineSlot(); }
        const Binding* data() const { return heap_ ? heap_ : inlineSlot(); }
        Binding& operator[](std::size_t i) { return data()[i]; }
        const Binding& operator[](std::size_t i) const { return data()[i]; }
        Binding* begin() { return data(); }
        Binding* end() { return data() + size_; }
        const Binding* begin() const { return data(); }
        const Binding* end() const { return data() + size_; }

        void reserve(std::size_t n) { if (n > cap_) grow(n); }
        void push(const std::string& k, Handle v) { push_back(k, v); }
        void push_back(const std::string& k, Handle v) {
            if (size_ == cap_) grow(cap_ * 2);
            new (data() + size_) Binding(k, v);
            ++size_;
        }
        void clear() {
            for (std::size_t i = 0; i < size_; ++i) data()[i].~Binding();
            size_ = 0;
            if (heap_) { ::operator delete(heap_); heap_ = nullptr; cap_ = kInline; }
        }

    private:
        Binding* inlineSlot() { return reinterpret_cast<Binding*>(&storage_); }
        const Binding* inlineSlot() const { return reinterpret_cast<const Binding*>(&storage_); }
        void grow(std::size_t want) {
            std::size_t newCap = want < kInline ? kInline : want;
            if (newCap <= cap_) return;
            Binding* fresh = static_cast<Binding*>(::operator new(newCap * sizeof(Binding)));
            Binding* old = data();
            for (std::size_t i = 0; i < size_; ++i) { new (fresh + i) Binding(std::move(old[i])); old[i].~Binding(); }
            if (heap_) ::operator delete(heap_);
            heap_ = fresh;
            cap_ = newCap;
        }
        alignas(Binding) unsigned char storage_[kInline * sizeof(Binding)];
        Binding* heap_ = nullptr;
        std::size_t size_ = 0;
        std::size_t cap_ = kInline;
    };

    SmallVec vars_;
    Handle parent_{};
    bool hasParent_;
};

// Resolve a name innermost-first along the scope chain.
inline std::optional<Handle> envLookup(const ObjectArena& arena, Handle env, const std::string& name) {
    Handle cur = env;
    while (true) {
        const auto& e = static_cast<const EnvValue&>(arena.deref(cur));
        if (const Handle* h = e.findLocal(name)) return *h;
        if (!e.hasParent()) return std::nullopt;
        cur = e.parent();
    }
}

// Rebind the nearest existing binding; false if the name is undefined anywhere in the chain.
inline bool envAssign(ObjectArena& arena, Handle env, const std::string& name, Handle value) {
    Handle cur = env;
    while (true) {
        auto& e = static_cast<EnvValue&>(arena.deref(cur));
        if (e.assignLocal(name, value)) return true;
        if (!e.hasParent()) return false;
        cur = e.parent();
    }
}

}  // namespace kirito

#endif
