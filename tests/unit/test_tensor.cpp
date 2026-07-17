#include <complex>
#include <iostream>
#include <sstream>
#include <string>

#include "../check.hpp"
#include "kirito.hpp"
#include "kirito/tensor.hpp"

using namespace kirito;
using tensor::Shape;
using tensor::Tensor;

static std::string evalStr(KiritoVM& vm, const std::string& src) { return vm.stringify(vm.runSource(src)); }

int main() {
    // ===== the engine (tensor.hpp), tested directly =====================================
    // shape / strides / numel
    CHECK(tensor::numel({2, 3, 4}) == 24);
    CHECK(tensor::rowMajorStrides({2, 3, 4}) == Shape({12, 4, 1}));

    // broadcasting rules
    CHECK(tensor::broadcastShapes({2, 3}, {3}) == Shape({2, 3}));
    CHECK(tensor::broadcastShapes({2, 1}, {1, 3}) == Shape({2, 3}));
    CHECK_THROWS(tensor::broadcastShapes({2, 3}, {4}));

    // construction + indexing
    Tensor<double> a({2, 3}, {1, 2, 3, 4, 5, 6});
    CHECK(a.ndim() == 2 && a.size() == 6);
    CHECK(a.at({1, 2}) == 6.0);
    CHECK_THROWS(a.at({2, 0}));            // out of range
    CHECK_THROWS((Tensor<double>({2, 2}, {1, 2, 3})));  // data/shape mismatch

    // reshape / flatten / transpose / permute
    CHECK(tensor::reshape(a, {3, 2}).shape == Shape({3, 2}));
    CHECK_THROWS(tensor::reshape(a, {4, 2}));
    CHECK(tensor::flatten(a).shape == Shape({6}));
    auto at = tensor::transpose(a);       // 3x2
    CHECK(at.shape == Shape({3, 2}));
    CHECK(at.at({0, 1}) == 4.0 && at.at({2, 0}) == 3.0);
    CHECK(tensor::permute(a, {1, 0}).at({2, 0}) == 3.0);
    CHECK_THROWS(tensor::permute(a, {0, 0}));  // not a permutation

    // elementwise (with broadcasting) + scalar
    CHECK(tensor::add(a, a).at({1, 2}) == 12.0);
    Tensor<double> row({1, 3}, {10, 20, 30});
    CHECK(tensor::add(a, row).at({1, 0}) == 14.0);   // broadcast the row
    CHECK(tensor::scalarOp(a, 2.0, '*').at({0, 1}) == 4.0);
    CHECK_THROWS(tensor::div(a, Tensor<double>({2, 3}, {0, 1, 1, 1, 1, 1})));  // divide by zero

    // matmul (2-D) and batched
    Tensor<double> p({2, 2}, {1, 2, 3, 4}), q({2, 2}, {5, 6, 7, 8});
    auto pq = tensor::matmul(p, q);
    CHECK(pq.at({0, 0}) == 19.0 && pq.at({1, 1}) == 50.0);
    CHECK_THROWS(tensor::matmul(p, Tensor<double>({3, 2})));   // inner dims differ
    Tensor<double> batch({2, 2, 2}, {1, 0, 0, 1, 2, 0, 0, 2});  // two diagonal matrices
    CHECK(tensor::matmul(batch, batch).shape == Shape({2, 2, 2}));

    // dot, reductions
    CHECK(tensor::dot(Tensor<double>({3}, {1, 2, 3}), Tensor<double>({3}, {4, 5, 6})) == 32.0);
    CHECK(tensor::sumAll(a) == 21.0);
    CHECK(tensor::minAll(a) == 1.0 && tensor::maxAll(a) == 6.0);
    auto s0 = tensor::reduceAxis(a, 0, [](double x, double y) { return x + y; });
    CHECK(s0.shape == Shape({3}) && s0.at({0}) == 5.0 && s0.at({2}) == 9.0);

    // linear algebra (square 2-D)
    Tensor<double> A({2, 2}, {1, 2, 3, 4});
    CHECK(tensor::determinant(A) == -2.0);
    CHECK(tensor::trace(A) == 5.0);
    auto inv = tensor::inverse(A);
    auto I = tensor::matmul(A, inv);                // ~ identity
    CHECK(std::fabs(I.at({0, 0}) - 1.0) < 1e-9 && std::fabs(I.at({0, 1})) < 1e-9);
    CHECK_THROWS(tensor::inverse(Tensor<double>({2, 2}, {1, 2, 2, 4})));  // singular
    CHECK_THROWS(tensor::determinant(Tensor<double>({2, 3})));            // non-square

    // new engine ops: slice / flip / concatenate / take / cumulative / solve / outer / kron
    CHECK(tensor::sliceAxis(A, 0, 0, 1, 1).shape == Shape({1, 2}));
    CHECK(tensor::flip(Tensor<double>({3}, {1, 2, 3}), 0).data == std::vector<double>({3, 2, 1}));
    {
        Tensor<double> r0({1, 2}, {1, 2}), r1({1, 2}, {3, 4});
        CHECK(tensor::concatenate<double>({&r0, &r1}, 0).shape == Shape({2, 2}));
    }
    CHECK(tensor::takeAxis0(A, {1, 0}).at({0, 0}) == 3.0);
    CHECK(tensor::cumulative(Tensor<double>({3}, {1, 1, 1}), 0, [](double x, double y) { return x + y; }).data == std::vector<double>({1, 2, 3}));
    {
        Tensor<double> S({2, 2}, {2, 0, 0, 4}), rhs({2, 1}, {2, 8});
        auto x = tensor::solve(S, rhs);                 // [1, 2]
        CHECK(std::fabs(x.at({0, 0}) - 1.0) < 1e-9 && std::fabs(x.at({1, 0}) - 2.0) < 1e-9);
    }
    CHECK(tensor::outer(Tensor<double>({2}, {1, 2}), Tensor<double>({2}, {3, 4})).at({1, 1}) == 8.0);
    CHECK(tensor::kron(tensor::Tensor<double>({2, 2}, {1, 0, 0, 1}), Tensor<double>({2, 2}, {1, 1, 1, 1})).shape == Shape({4, 4}));

    // complex element type: the same engine, instantiated for std::complex<double>
    using cd = std::complex<double>;
    Tensor<cd> z({2, 2}, {cd(1, 1), cd(2, 0), cd(0, 1), cd(1, -1)});
    CHECK(tensor::determinant(z) == cd(2, -2));     // (1+i)(1-i) - 2i = 2 - 2i
    CHECK_THROWS(tensor::minAll(z));                // complex is unordered

    // ===== the Kirito `tensor` module ===================================================
    KiritoVM vm;
    auto run = [&](const std::string& body) { return evalStr(vm, "var T = import(\"tensor\")\n" + body); };
    CHECK(run("T.Tensor([[1, 2], [3, 4]])") == "[[1.0, 2.0], [3.0, 4.0]]");
    CHECK(run("T.Tensor([[1, 2], [3, 4]]).dtype()") == "Float");
    CHECK(run("T.zeros([2, 3]).shape()") == "[2, 3]");
    CHECK(run("T.Tensor([1, 2, 3]) + T.Tensor([10, 20, 30])") == "[11.0, 22.0, 33.0]");
    CHECK(run("T.Tensor([[1, 2], [3, 4]]).matmul(T.Tensor([[5, 6], [7, 8]]))") == "[[19.0, 22.0], [43.0, 50.0]]");
    CHECK(run("T.Tensor([[1, 2], [3, 4]]).sum()") == "10.0");
    CHECK(run("T.Tensor([[1, 2], [3, 4]]).sum(0)") == "[4.0, 6.0]");
    CHECK(run("T.Tensor([1, 4, 9]).apply(Function(x): return x ** 0.5)") == "[1.0, 2.0, 3.0]");
    CHECK(run("T.Tensor([[1, 2], [3, 4]], dtype=\"Complex\").dtype()") == "Complex");
    CHECK(run("T.Tensor([1, 2, 3])[1]") == "2.0");
    CHECK_THROWS(vm.runSource("import(\"tensor\").Tensor([[1, 2], [3]])\n"));            // ragged
    CHECK_THROWS(vm.runSource("import(\"tensor\").Tensor([1, 2, 3], dtype=\"Complex\").min()\n"));  // complex min

    // ===== autograd =====================================================================
    CHECK(run("T.zeros([2]).requiresgrad()") == "False");                                // off by default
    CHECK(run("T.zeros([2], requiresgrad=True).requiresgrad()") == "True");              // opt-in kwarg
    // d/dx sum(x^2) = 2x
    CHECK(run("var x = T.Tensor([1, 2, 3], requiresgrad=True)\nx.square().sum().backward()\nx.grad") == "[2.0, 4.0, 6.0]");
    // product rule via backward: d/da sum(a*b) = b, d/db sum(a*b) = a
    CHECK(run("var a = T.Tensor([2, 3], requiresgrad=True)\nvar b = T.Tensor([5, 7], requiresgrad=True)\n(a*b).sum().backward()\na.grad") == "[5.0, 7.0]");
    // matmul backward through an identity is ones
    CHECK(run("var m = T.Tensor([[1,2],[3,4]], requiresgrad=True)\nm.matmul(T.eye(2)).sum().backward()\nm.grad") == "[[1.0, 1.0], [1.0, 1.0]]");
    // broadcasting backward: column sums
    CHECK(run("var a = T.Tensor([[1,2,3],[4,5,6]], requiresgrad=True)\nvar w = T.Tensor([1,1,1], requiresgrad=True)\n(a*w).sum().backward()\nw.grad") == "[5.0, 7.0, 9.0]");
    // relu gradient mask
    CHECK(run("var z = T.Tensor([-1, 2], requiresgrad=True)\nz.relu().sum().backward()\nz.grad") == "[0.0, 1.0]");
    // grad accumulates across backward() calls; zerograd clears
    CHECK(run("var p = T.Tensor([1, 2], requiresgrad=True)\np.sum().backward()\np.sum().backward()\np.grad") == "[2.0, 2.0]");
    CHECK(run("var p = T.Tensor([1, 2], requiresgrad=True)\np.sum().backward()\np.zerograd()\np.grad") == "None");
    // tensordot equals matmul, and is differentiable
    CHECK(run("T.tensordot(T.Tensor([[1,2],[3,4]]), T.Tensor([[5,6],[7,8]]), 1)") == "[[19.0, 22.0], [43.0, 50.0]]");
    CHECK(run("var t = T.Tensor([[1,2],[3,4]], requiresgrad=True)\nT.tensordot(t, T.eye(2), 1).sum().backward()\nt.grad") == "[[1.0, 1.0], [1.0, 1.0]]");
    // detach drops grad
    CHECK(run("T.Tensor([1.0], requiresgrad=True).detach().requiresgrad()") == "False");
    CHECK_THROWS(vm.runSource("import(\"tensor\").zeros([2]).backward()\n"));            // backward without grad
    CHECK_THROWS(vm.runSource("import(\"tensor\").zeros([2], dtype=\"Complex\").requiresgrad(True)\n"));  // complex can't require grad

    // ===== NumPy-style surface (module level) ===========================================
    CHECK(run("T.Tensor([[1,2,3],[4,5,6]])[0:1]") == "[[1.0, 2.0, 3.0]]");                // axis-0 slice
    CHECK(run("T.Tensor([[1,2],[3,4]])[[1,0]]") == "[[3.0, 4.0], [1.0, 2.0]]");           // fancy index
    CHECK(run("var a = T.Tensor([1,2,3,4])\na[a.gt(2)]") == "[3.0, 4.0]");                // boolean mask
    CHECK(run("T.Tensor([1,2,3]).gt(2)") == "[0.0, 0.0, 1.0]");                           // comparison mask
    CHECK(run("(T.Tensor([1,2,3]) > 2)") == "[0.0, 0.0, 1.0]");                           // > operator
    CHECK(run("T.where(T.Tensor([1,0,1]), T.Tensor([1,2,3]), T.zeros([3]))") == "[1.0, 0.0, 3.0]");
    CHECK(run("T.Tensor([[1,2],[3,4]]).max(0)") == "[3.0, 4.0]");                         // max along axis
    CHECK(run("T.Tensor([[1,2],[3,4]]).argmax(1)") == "[1.0, 1.0]");
    CHECK(run("T.Tensor([1,2,3]).cumsum()") == "[1.0, 3.0, 6.0]");
    CHECK(run("(T.Tensor([5,7]) % 3)") == "[2.0, 1.0]");                                  // modulo op
    CHECK(run("(T.Tensor([2,3]) ** 2)") == "[4.0, 9.0]");                                 // power op
    CHECK(run("T.Tensor([1.7,-1.2]).floor()") == "[1.0, -2.0]");
    CHECK(run("T.concatenate([T.Tensor([1,2]), T.Tensor([3,4])], 0)") == "[1.0, 2.0, 3.0, 4.0]");
    CHECK(run("T.stack([T.Tensor([1,2]), T.Tensor([3,4])], 0)") == "[[1.0, 2.0], [3.0, 4.0]]");
    CHECK(run("T.Tensor([1,2,3]).flip()") == "[3.0, 2.0, 1.0]");
    CHECK(run("T.linspace(0, 1, 5)") == "[0.0, 0.25, 0.5, 0.75, 1.0]");
    CHECK(run("T.diag(T.Tensor([1,2]))") == "[[1.0, 0.0], [0.0, 2.0]]");
    CHECK(run("T.det(T.Tensor([[4,3],[6,3]]))") == "-6.0");                               // linalg
    CHECK(run("T.inv(T.Tensor([[4,3],[6,3]])).matmul(T.Tensor([[4,3],[6,3]]))") == "[[1.0, 0.0], [0.0, 1.0]]");
    CHECK(run("T.outer(T.Tensor([1,2]), T.Tensor([3,4]))") == "[[3.0, 4.0], [6.0, 8.0]]");
    CHECK(run("T.cross(T.Tensor([1,0,0]), T.Tensor([0,1,0]))") == "[0.0, 0.0, 1.0]");
    CHECK(run("T.einsum(\"ij,jk->ik\", T.Tensor([[1,2],[3,4]]), T.eye(2))") == "[[1.0, 2.0], [3.0, 4.0]]");
    CHECK(run("T.Tensor([3,1,2]).sort()") == "[1.0, 2.0, 3.0]");
    CHECK(run("T.Tensor([3,1,2]).argsort()") == "[1.0, 2.0, 0.0]");
    CHECK(run("T.unique(T.Tensor([3,1,3,2,1]))") == "[1.0, 2.0, 3.0]");
    CHECK(run("T.searchsorted(T.Tensor([1,3,5]), 4)") == "2");                            // insertion index
    CHECK(run("T.Tensor([[1,2]], dtype=\"Complex\").real()") == "[[1.0, 2.0]]");
    // autograd through the new ops
    CHECK(run("var x = T.Tensor([1,2,3,4], requiresgrad=True)\nT.where(T.Tensor([1,0,1,0]), x, T.zeros([4])).sum().backward()\nx.grad") == "[1.0, 0.0, 1.0, 0.0]");
    CHECK(run("var x = T.Tensor([-1.0,0.5,2.0], requiresgrad=True)\nx.clip(0,1).sum().backward()\nx.grad") == "[0.0, 1.0, 0.0]");
    CHECK_THROWS(vm.runSource("import(\"tensor\").Tensor([1.0,2.0], dtype=\"Complex\").lt(3.0)\n"));  // complex ordering undefined

    // serialization: gradient-free tensors round-trip; grad-requiring ones refuse
    CHECK(run("var s = import(\"serialize\")\nvar t = T.Tensor([[1.5,2.5],[3.5,4.5]])\ns.loads(s.dumps(t)) == t") == "True");
    CHECK(run("var d = import(\"dump\")\nvar t = T.Tensor([1,2,3])\nd.loads(d.dumps(t)) == t") == "True");
    CHECK_THROWS(vm.runSource("var s = import(\"serialize\")\nvar T = import(\"tensor\")\ndiscard s.dumps(T.Tensor([1.0], requiresgrad=True))\n"));  // grad tensor refuses

    // a non-differentiable op on a grad tensor warns once (to stderr)
    {
        std::ostringstream cap;
        std::streambuf* old = std::cerr.rdbuf(cap.rdbuf());
        KiritoVM wvm;
        wvm.runSource("var T = import(\"tensor\")\nvar a = T.Tensor([1.0,2.0], requiresgrad=True)\ndiscard a.min()\ndiscard a.min()\n");
        std::cerr.rdbuf(old);
        std::string w = cap.str();
        CHECK(w.find("not differentiable") != std::string::npos);             // warned
        CHECK(w.find("min") != std::string::npos);
        std::size_t first = w.find("min"), second = w.find("min", first + 1);
        CHECK(second == std::string::npos);                                    // warned exactly once
    }

    // --- item(): a one-element tensor -> a scalar Float/Complex ---------------------------------
    CHECK(evalStr(vm, "var T = import(\"tensor\")\nT.Tensor([42.0]).item()") == "42.0");
    CHECK(evalStr(vm, "var T = import(\"tensor\")\ntype(T.Tensor([[7.0]]).item())") == "Float");
    CHECK(evalStr(vm, "var T = import(\"tensor\")\nT.zeros([1,1,1]).item()") == "0.0");
    // works on a grad-tracked scalar result (sum/mean return a scalar tensor under grad)
    CHECK(evalStr(vm, "var T = import(\"tensor\")\nvar g = T.Tensor([3.0,4.0], requiresgrad=True)\n(g*g).sum().item()") == "25.0");
    CHECK(evalStr(vm, "var T = import(\"tensor\")\ntype(T.Tensor([5.0], dtype=\"Complex\").item())") == "Complex");
    CHECK_THROWS(vm.runSource("var T = import(\"tensor\")\nT.Tensor([1.0,2.0]).item()"));   // multi-element
    CHECK_THROWS(vm.runSource("var T = import(\"tensor\")\nT.zeros([0]).item()"));          // empty

    // --- tolist(): tensor -> nested Kirito List ------------------------------------------------
    CHECK(evalStr(vm, "var T = import(\"tensor\")\nT.Tensor([1.0,2.0,3.0]).tolist()") == "[1.0, 2.0, 3.0]");
    CHECK(evalStr(vm, "var T = import(\"tensor\")\ntype(T.Tensor([1.0]).tolist())") == "List");
    CHECK(evalStr(vm, "var T = import(\"tensor\")\nT.Tensor([[1.0,2.0],[3.0,4.0]]).tolist()") == "[[1.0, 2.0], [3.0, 4.0]]");
    CHECK(evalStr(vm, "var T = import(\"tensor\")\nT.zeros([2,1,2]).tolist()") == "[[[0.0, 0.0]], [[0.0, 0.0]]]");
    // round-trips: Tensor(t.tolist()) == t
    CHECK(evalStr(vm, "var T = import(\"tensor\")\nvar t = T.arange(24.0).reshape([2,3,4])\nT.Tensor(t.tolist()) == t") == "True");
    // tolist of a complex tensor nests Complex values
    CHECK(evalStr(vm, "var T = import(\"tensor\")\ntype(T.Tensor([[1.0]], dtype=\"Complex\").tolist()[0][0])") == "Complex");

    // GC regression: tolist() builds a nested List whose float leaves sit in a parent ListVal that
    // is not yet in the arena. If those leaves aren't GC-rooted, a collection triggered mid-build by
    // a sibling allocation reclaims them, yielding a stale handle (hit by Float-tensor image I/O).
    // Force a collection before every allocation so the partial structure is maximally exposed.
    {
        KiritoVM gcvm;
        gcvm.setGcThreshold(1);
        CHECK(evalStr(gcvm, "var T = import(\"tensor\")\nvar t = T.arange(60.0).reshape([4,5,3])\nT.Tensor(t.tolist()) == t") == "True");
        // and the leaf values survive intact (sum 0..59)
        CHECK(evalStr(gcvm, "var T = import(\"tensor\")\nvar s = 0.0\nfor row in T.arange(60.0).reshape([4,5,3]).tolist():\n  for px in row:\n    for v in px:\n      s = s + v\ns") == "1770.0");
    }

    // item()/tolist() are described under inspect
    CHECK(evalStr(vm, "var T = import(\"tensor\")\ninspect(T.zeros([1])).find(\"item()\") >= 0") == "True");
    CHECK(evalStr(vm, "var T = import(\"tensor\")\ninspect(T.zeros([1])).find(\"tolist()\") >= 0") == "True");

    return RUN_TESTS();
}
