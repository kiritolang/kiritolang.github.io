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

    // ===== the Kirito `tensor` module: KiritoVM/runSource-driven value/error checks live in
    // tests/scripts/unit_tensor.ki now. What stays here needs a C++-only capability the .ki golden
    // runner can't express: capturing stderr (the warn-once checks below) or a per-VM
    // setGcThreshold() (the GC regression further down). ==================================

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

    // REGRESSION: a grad-tracking Float tensor combined with a Complex tensor/scalar yields a non-grad
    // Complex result — the gradient breaks, so it must WARN (never silently detach), per the autograd
    // contract "a gradient break is never silent".
    {
        std::ostringstream cap;
        std::streambuf* old = std::cerr.rdbuf(cap.rdbuf());
        KiritoVM wvm;
        wvm.runSource("var T = import(\"tensor\")\nvar C = import(\"complex\")\n"
                      "var a = T.Tensor([1.0,2.0], requiresgrad=True)\n"
                      "var b = T.Tensor([C.of(1,1), C.of(2,2)], dtype=\"Complex\")\n"
                      "discard a + b\n");
        std::cerr.rdbuf(old);
        CHECK(cap.str().find("not differentiable") != std::string::npos);      // did not silently detach
    }

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

    return RUN_TESTS();
}
