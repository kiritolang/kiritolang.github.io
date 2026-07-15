#ifndef KIRITO_MODULE_HPP
#define KIRITO_MODULE_HPP

#include <string>
#include <string_view>

#include "fum/unordered_map.hpp"
#include "object.hpp"

namespace kirito {

// A module value: a namespace of named members (functions, values, types) reached by member
// access, e.g. io.print. Built by a NativeModule's setup() or by importing a .ki file.
class ModuleValue : public Object {
public:
    explicit ModuleValue(std::string name) : name_(std::move(name)) {}

    fum::unordered_map<std::string, Handle> members;

    ValueKind kind() const override { return ValueKind::Module; }
    std::string typeName() const override { return "Module"; }
    bool truthy() const override { return true; }
    std::string str(StringifyCtx&) const override { return "<module " + name_ + ">"; }
    bool equals(const ObjectArena&, const Object& other) const override { return this == &other; }
    void children(std::vector<Handle>& out) const override {
        for (const auto& [k, h] : members) out.push_back(h);
    }
    Handle getAttr(KiritoVM&, Handle, std::string_view name) override {
        auto it = members.find(std::string(name));
        if (it == members.end())
            throw KiritoError("module '" + name_ + "' has no member '" + std::string(name) + "'");
        return it->second;
    }
    // Rebinding a module member is allowed, so e.g. `io.stdout = io.open(...)`
    // redirects every subsequent io.print. The module is a per-VM singleton, so this is global.
    void setAttr(KiritoVM&, std::string_view name, Handle value) override {
        gcWriteBarrier(this, value);
        members[std::string(name)] = value;
    }
    // Barriered member install (the native ModuleBuilder path). A module is a per-VM singleton that
    // outlives every value bound into it, so once promoted it is a permanent old->young store site.
    void setMember(const std::string& name, Handle h) {
        gcWriteBarrier(this, h);
        members[name] = h;
    }
    const std::string& name() const { return name_; }

private:
    std::string name_;
};

}  // namespace kirito

#endif
