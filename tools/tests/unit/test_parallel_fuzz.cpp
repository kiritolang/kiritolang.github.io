// Fuzz: random value graphs round-tripped through a Queue from one VM to another (the cross-VM
// transfer path that spawn/Queue rely on). Structural fidelity is checked by comparing the two VMs'
// stringifications. Graphs use only order-deterministic kinds (scalars / List / Dict) so the
// comparison is exact across arenas.

#include <cstddef>
#include <random>
#include <string>

#include "../parallel_util.hpp"

using namespace kitest;

// Structural equality across two arenas. Dicts are compared order-insensitively (Kirito Dicts are
// hash-bucket-ordered, so iteration order legitimately differs across VMs — matching Kirito's own
// order-insensitive `==`).
static bool deepEqual(kirito::KiritoVM& va, kirito::Handle a, kirito::KiritoVM& vb, kirito::Handle b) {
    using namespace kirito;
    Object& oa = va.arena().deref(a);
    Object& ob = vb.arena().deref(b);
    if (oa.kind() != ob.kind()) return false;
    switch (oa.kind()) {
        case ValueKind::None: return true;
        case ValueKind::Bool: return static_cast<BoolVal&>(oa).value() == static_cast<BoolVal&>(ob).value();
        case ValueKind::Integer: return static_cast<IntVal&>(oa).value() == static_cast<IntVal&>(ob).value();
        case ValueKind::Float: return static_cast<FloatVal&>(oa).value() == static_cast<FloatVal&>(ob).value();
        case ValueKind::String: return static_cast<StrVal&>(oa).value() == static_cast<StrVal&>(ob).value();
        case ValueKind::List: {
            auto& la = static_cast<ListVal&>(oa);
            auto& lb = static_cast<ListVal&>(ob);
            if (la.elems.size() != lb.elems.size()) return false;
            for (std::size_t i = 0; i < la.elems.size(); ++i)
                if (!deepEqual(va, la.elems[i], vb, lb.elems[i])) return false;
            return true;
        }
        case ValueKind::Dict: {
            auto& da = static_cast<DictVal&>(oa);
            auto& db = static_cast<DictVal&>(ob);
            if (da.count != db.count) return false;
            for (const auto& [ka, vva] : da.pairs()) {
                bool found = false;
                for (const auto& [kb, vvb] : db.pairs())
                    if (deepEqual(va, ka, vb, kb)) {
                        if (!deepEqual(va, vva, vb, vvb)) return false;
                        found = true;
                        break;
                    }
                if (!found) return false;
            }
            return true;
        }
        default: return false;
    }
}

static kirito::Handle randomValue(kirito::KiritoVM& vm, std::mt19937& rng, int depth) {
    int pick = depth <= 0 ? static_cast<int>(rng() % 5) : static_cast<int>(rng() % 7);
    switch (pick) {
        case 0:
            return vm.makeInt(static_cast<int64_t>(rng() % 4000) - 2000);
        case 1:
            return vm.makeFloat(static_cast<double>(static_cast<int64_t>(rng() % 4000) - 2000) / 7.0);
        case 2:
            return vm.makeBool(rng() % 2 == 0);
        case 3:
            return vm.none();
        case 4: {
            int len = static_cast<int>(rng() % 6);
            std::string s;
            for (int i = 0; i < len; ++i) s += static_cast<char>('a' + static_cast<int>(rng() % 26));
            return vm.makeString(s);
        }
        case 5: {
            kirito::List b(vm);
            int len = static_cast<int>(rng() % 4);
            for (int i = 0; i < len; ++i) b.add(randomValue(vm, rng, depth - 1));
            return b.build();
        }
        default: {
            kirito::Dict b(vm);
            int len = static_cast<int>(rng() % 4);
            for (int i = 0; i < len; ++i) b.set("k" + std::to_string(i), randomValue(vm, rng, depth - 1));
            return b.build();
        }
    }
}

int main() {
    noDeadlock("fuzz queue round-trip", 30.0, [] {
        std::mt19937 rng(0xBEEF);
        kirito::KiritoDispatcher disp;
        kirito::KiritoVM& vmA = disp.mainVM();
        kirito::KiritoVM vmB;  // bare VM; the dump codec is in installStandardLibrary
        auto q = disp.createQueue(0);
        for (int it = 0; it < 300; ++it) {
            kirito::RootScope rsA(vmA);
            kirito::Handle g = rsA.add(randomValue(vmA, rng, 4));
            q->put(kirito::dumpfmt::write(vmA, g), true, std::nullopt);
            std::string blob;
            kirito::WaitResult r = q->get(true, std::nullopt, blob);
            CHECK(r == kirito::WaitResult::Ok);
            kirito::RootScope rsB(vmB);
            kirito::Handle g2 = rsB.add(kirito::dumpfmt::read(vmB, blob));
            CHECK(deepEqual(vmA, g, vmB, g2));
        }
    });
    return RUN_TESTS();
}
