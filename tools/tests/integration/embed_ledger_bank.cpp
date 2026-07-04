// embed_ledger_bank.cpp — a double-entry accounting engine. C++ owns the accounts (name→int64
// cents balance) and the journal of transactions; each POSTING RULE is a Kirito
// Function(txn: Dict) -> List that returns the transaction's legs — a list of Dicts
// {"account": String, "delta": Integer cents}. C++ enforces the double-entry invariant: every
// posting's legs MUST sum to zero, else the post is rejected.
//
// Flow per transaction: C++ (build txn Dict) → Kirito (posting rule → legs) → C++ (check
// sum-to-zero, apply deltas to the ledger, append to the journal).

#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

struct Leg {
    std::string account;
    int64_t     delta;   // signed cents; a posting's legs sum to zero
};

struct JournalEntry {
    std::string      memo;
    std::vector<Leg> legs;
};

class Ledger {
public:
    explicit Ledger(KiritoVM& vm) : vm_(vm) {}

    // Register a named posting rule: a Kirito Function(txn: Dict) -> List of leg Dicts.
    void addRule(const std::string& name, Handle fn) { rules_[name] = fn; }

    int64_t balance(const std::string& account) const {
        auto it = accounts_.find(account);
        return it == accounts_.end() ? 0 : it->second;
    }
    std::size_t journalSize() const { return journal_.size(); }

    // Run the named rule over the transaction Dict, validate the returned legs, and apply them.
    void post(const std::string& rule, const std::string& memo, Handle txn) {
        auto it = rules_.find(rule);
        if (it == rules_.end())
            throw KiritoError("ledger: no such rule '" + rule + "'");

        RootScope rs(vm_);
        Handle txnH = rs.add(txn);
        std::array<Handle, 1> args{txnH};
        Handle legsH = rs.add(vm_.arena().deref(it->second).call(vm_, args));
        Value result(vm_, legsH);

        // A rule MUST return a List of legs — anything else is a programming error, fail loudly.
        if (!result.isList())
            throw KiritoError("ledger: rule '" + rule + "' must return a List, got '" +
                              result.typeName() + "'");

        std::vector<Leg> legs;
        int64_t sum = 0;
        for (Value item : result.items()) {
            if (!item.isDict())
                throw KiritoError("ledger: each leg must be a Dict, got '" + item.typeName() + "'");
            std::string account = item.get("account").asStringRef("leg account");
            int64_t delta = item.get("delta").asInt("leg delta");
            sum += delta;
            legs.push_back({account, delta});
        }
        // Double-entry invariant: the legs of a single posting MUST net to zero.
        if (sum != 0)
            throw KiritoError("ledger: unbalanced posting for '" + memo + "' (legs sum to " +
                              std::to_string(sum) + ", not 0)");

        for (const Leg& leg : legs)
            accounts_[leg.account] += leg.delta;
        journal_.push_back({memo, std::move(legs)});
    }

private:
    KiritoVM&                                     vm_;
    std::unordered_map<std::string, Handle>       rules_;
    std::unordered_map<std::string, int64_t>      accounts_;
    std::vector<JournalEntry>                     journal_;
};

// Build a transaction Dict the rules consume: from/to account names + an amount in cents.
static Handle makeTxn(KiritoVM& vm, const std::string& from, const std::string& to, int64_t cents) {
    Dict d(vm);
    d.set("from",   Value(vm, from));
    d.set("to",     Value(vm, to));
    d.set("amount", Value(vm, cents));
    return d.handle();
}

int main() {
    KiritoVM vm;
    auto compile = [&](const char* src) { return vm.runSource(src); };

    Ledger ledger(vm);

    // Rule "transfer": a plain two-leg move — debit `from`, credit `to`. Legs net to zero.
    ledger.addRule("transfer", compile(R"KI(
Function(txn) -> List:
    return [
        {"account": txn["from"], "delta": -txn["amount"]},
        {"account": txn["to"],   "delta":  txn["amount"]},
    ]
)KI"));

    // Rule "transfer_with_fee": like transfer, but the sender also pays a 2% fee (rounded down via
    // floor division) that lands in the "fees" account. Three legs, still summing to zero:
    //   sender:  -(amount + fee)   receiver: +amount   fees: +fee
    ledger.addRule("transfer_with_fee", compile(R"KI(
Function(txn) -> List:
    var fee = (txn["amount"] * 2) // 100
    return [
        {"account": txn["from"], "delta": -(txn["amount"] + fee)},
        {"account": txn["to"],   "delta":  txn["amount"]},
        {"account": "fees",      "delta":  fee},
    ]
)KI"));

    // ---- post several transactions ----
    // Alice sends Bob $100.00 (10000 cents), plainly.
    ledger.post("transfer", "alice->bob 100.00", makeTxn(vm, "alice", "bob", 10000));
    // Bob sends Carol $50.00 with a 2% fee ($1.00 → 100 cents to "fees").
    ledger.post("transfer_with_fee", "bob->carol 50.00 +fee", makeTxn(vm, "bob", "carol", 5000));
    // Alice sends Carol $25.00 with a fee (2% of 2500 = 50 cents).
    ledger.post("transfer_with_fee", "alice->carol 25.00 +fee", makeTxn(vm, "alice", "carol", 2500));

    // ---- assert final balances (all in cents) ----
    // alice: -10000 (to bob) - 2550 (25.00 + 0.50 fee, to carol) = -12550
    CHECK(ledger.balance("alice") == -12550);
    // bob: +10000 (from alice) - 5100 (50.00 + 1.00 fee) = +4900
    CHECK(ledger.balance("bob") == 4900);
    // carol: +5000 (from bob) + 2500 (from alice) = +7500
    CHECK(ledger.balance("carol") == 7500);
    // fees: 100 + 50 = 150
    CHECK(ledger.balance("fees") == 150);
    // an account no transaction ever touched reads zero
    CHECK(ledger.balance("nobody") == 0);
    // three postings recorded in the journal
    CHECK(ledger.journalSize() == 3);

    // The whole ledger must net to zero — double-entry conservation across every account.
    {
        int64_t total = ledger.balance("alice") + ledger.balance("bob") +
                        ledger.balance("carol") + ledger.balance("fees");
        CHECK(total == 0);
    }

    // ---- adversarial: a rule whose legs DON'T sum to zero must make posting throw ----
    {
        Ledger bad(vm);
        bad.addRule("lopsided", compile(R"KI(
Function(txn) -> List:
    return [
        {"account": txn["from"], "delta": -txn["amount"]},
        {"account": txn["to"],   "delta":  txn["amount"] + 1},
    ]
)KI"));
        CHECK_THROWS(bad.post("lopsided", "unbalanced", makeTxn(vm, "x", "y", 100)));
        // A rejected posting leaves the ledger untouched.
        CHECK(bad.balance("x") == 0);
        CHECK(bad.balance("y") == 0);
        CHECK(bad.journalSize() == 0);
    }

    // ---- adversarial: a rule returning a non-List must throw ----
    {
        Ledger bad(vm);
        bad.addRule("scalar", compile("Function(txn): return txn[\"amount\"]\n"));
        CHECK_THROWS(bad.post("scalar", "not-a-list", makeTxn(vm, "x", "y", 100)));
    }

    // ---- adversarial: a leg that isn't a Dict must throw ----
    {
        Ledger bad(vm);
        bad.addRule("badleg", compile("Function(txn): return [\"oops\"]\n"));
        CHECK_THROWS(bad.post("badleg", "bad-leg", makeTxn(vm, "x", "y", 100)));
    }

    // ---- posting through an unregistered rule name throws ----
    {
        Ledger bare(vm);
        CHECK_THROWS(bare.post("missing", "no-rule", makeTxn(vm, "x", "y", 100)));
    }

    return RUN_TESTS();
}
