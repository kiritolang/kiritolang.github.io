// test_tensor_deep.cpp — adversarial/edge coverage for `tensor`, filling audited gaps concentrated
// in the Complex-dtype code branches plus a few never-invoked symbols (the `tensor` ctor alias,
// `.conjugate()`, `astype` identity, negative-axis `flip`, whole-tensor `==` vs a non-tensor).
#include <string>
#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string run(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}
static bool throws(KiritoVM& vm, const std::string& src) {
    try { vm.runSource(src); return false; }
    catch (...) { return true; }
}

int main() {
    KiritoVM vm;

    // ---- the lowercase `tensor` constructor alias (never called; only `Tensor` was) ----
    CHECK(run(vm, "import(\"tensor\")\ntensor.tensor([1, 2, 3]).sum().item()") == "6.0");

    // ---- astype("Float") identity branch (Float -> Float, no cross to Complex) ----
    CHECK(run(vm, "import(\"tensor\")\ntensor.ones([2]).astype(\"Float\").sum().item()") == "2.0");

    // ---- negative-axis flip ----
    CHECK(run(vm, "import(\"tensor\")\ntensor.tensor([1, 2, 3]).flip(-1).tolist()") == "[3.0, 2.0, 1.0]");

    // ---- whole-tensor `==` against a non-tensor is a single False (not an elementwise mask) ----
    CHECK(run(vm, "import(\"tensor\")\ntensor.tensor([1, 2]) == 5") == "False");

    // ---- Complex-dtype branches: conjugate of a real-valued complex tensor is itself ----
    CHECK(run(vm, R"KI(import("tensor")
var z = tensor.ones([2, 2]).astype("Complex")
z.conjugate() == z)KI") == "True");

    // ---- Complex per-axis reduction runs (reduceAxis complex branch) ----
    CHECK(!throws(vm, R"KI(import("tensor")
var z = tensor.tensor([[1, 2], [3, 4]]).astype("Complex")
discard z.sum(0)
discard z.mean(1)
discard z.cumsum(0))KI"));

    // ---- Complex structural/apply/index branches run ----
    CHECK(!throws(vm, R"KI(import("tensor")
var z = tensor.tensor([[1, 2], [3, 4]]).astype("Complex")
discard z.transpose()
discard z.flatten()
discard z.apply(Function(x): return x)
discard z[0]
discard z[0, 1])KI"));

    // ---- Complex element assignment (cpx::asComplex setItem branch) ----
    CHECK(!throws(vm, R"KI(import("tensor")
import("complex")
var z = tensor.zeros([2, 2]).astype("Complex")
z[0, 0] = complex.of(1, 2))KI"));

    // ---- inner on 2-D operands (contracts last axis), and searchsorted with duplicate bounds ----
    CHECK(!throws(vm, R"KI(import("tensor")
discard tensor.tensor([[1, 2], [3, 4]]).inner(tensor.tensor([[1, 0], [0, 1]])))KI"));
    CHECK(!throws(vm, R"KI(import("tensor")
discard tensor.tensor([1, 2, 2, 2, 3]).searchsorted(2))KI"));

    // ---- a domain-error math element still throws under the tensor engine (parity with scalars) ----
    CHECK(throws(vm, "import(\"tensor\")\ntensor.tensor([-1.0, 4.0]).sqrt()"));

    return RUN_TESTS();
}
