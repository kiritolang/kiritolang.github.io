#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

static const char* kResource = R"(
class Resource:
    var _init_ = Function(self, log):
        self.log = log
    var _enter_ = Function(self):
        self.log.append("enter")
        return self
    var _exit_ = Function(self):
        self.log.append("exit")
)";

int main() {
    KiritoVM vm;

    // enter/body/exit run in order; `as` binds enter()'s return value
    CHECK(evalStr(vm, std::string(kResource) + R"(
var log = []
with Resource(log) as r:
    log.append("body")
log
)") == "['enter', 'body', 'exit']");

    // exit runs even when the body throws, and the exception still propagates to an outer handler
    CHECK(evalStr(vm, std::string(kResource) + R"(
var log = []
try:
    with Resource(log) as r:
        log.append("body")
        throw "boom"
catch as e:
    log.append("caught " + e)
log
)") == "['enter', 'body', 'exit', 'caught boom']");

    // the bound value is the manager (enter returned self)
    CHECK(evalStr(vm, std::string(kResource) + R"(
var log = []
with Resource(log) as r:
    r.log.append("via-r")
log
)") == "['enter', 'via-r', 'exit']");

    return RUN_TESTS();
}
