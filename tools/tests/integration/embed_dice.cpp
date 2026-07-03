// embed_dice.cpp — a deterministic tabletop combat resolver. C++ owns the two combatants (hp as
// int) and the round loop; Kirito owns the DICE POLICY: a seeded RNG plus an attack-resolution
// Function(attacker: Dict, defender: Dict, rng) -> Dict that rolls dice and returns
// {"hit": Bool, "damage": Integer}. Because the RNG is seeded (Random(42)) the whole fight is
// reproducible, so C++ asserts the outcome's INVARIANTS (loser at 0 hp, winner alive, hp never
// negative, round count consistent) and that a fresh identically-seeded run replays bit-for-bit.
//
// Flow per round: C++ (pick attacker/defender) → Kirito (roll to hit + damage, advancing the
// persistent rng) → C++ (apply clamped damage, swap sides).

#include <algorithm>
#include <array>
#include <string>
#include <utility>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

struct Combatant {
    std::string name;
    int64_t     hp;
    int64_t     attack;
    int64_t     defense;
    int64_t     power;   // max damage die
};

// Hand a combatant to Kirito as a Dict the resolver can read.
static Handle combatantToDict(KiritoVM& vm, const Combatant& c) {
    Dict d(vm);
    d.set("name",    Value(vm, c.name));
    d.set("hp",      Value(vm, c.hp));
    d.set("attack",  Value(vm, c.attack));
    d.set("defense", Value(vm, c.defense));
    d.set("power",   Value(vm, c.power));
    return d.handle();
}

class DiceArena {
public:
    DiceArena(KiritoVM& vm, Handle resolver, Handle rng)
        : vm_(vm), resolver_(resolver), rng_(rng) {}

    struct Outcome {
        std::string winner;
        int64_t     winnerHp = 0;
        int64_t     loserHp  = 0;
        int         rounds    = 0;
        int64_t     minHp     = 0;   // lowest hp observed anywhere in the fight
        bool        ended     = false;
    };

    // Run the fight to the death. `a` strikes first each of its turns.
    Outcome fight(Combatant a, Combatant b, int maxRounds) {
        RootScope rs(vm_);
        Handle rngRoot = rs.add(rng_);   // keep the persistent rng alive across every call
        (void)rngRoot;

        Combatant* atk = &a;
        Combatant* def = &b;
        Outcome out;
        out.minHp = std::min(a.hp, b.hp);

        while (a.hp > 0 && b.hp > 0 && out.rounds < maxRounds) {
            ++out.rounds;
            auto [hit, dmg] = resolve(rs, *atk, *def);
            if (hit && dmg > 0) {
                def->hp -= dmg;
                if (def->hp < 0) def->hp = 0;   // hp floors at zero, never negative
            }
            out.minHp = std::min(out.minHp, std::min(a.hp, b.hp));
            std::swap(atk, def);
        }

        if (a.hp <= 0 || b.hp <= 0) {
            out.ended = true;
            const Combatant& winner = (a.hp > 0) ? a : b;
            const Combatant& loser  = (a.hp > 0) ? b : a;
            out.winner   = winner.name;
            out.winnerHp = winner.hp;
            out.loserHp  = loser.hp;
        }
        return out;
    }

private:
    // Call the Kirito resolver; validate the returned Dict shape strictly.
    std::pair<bool, int64_t> resolve(RootScope& rs, const Combatant& atk, const Combatant& def) {
        Handle aH = rs.add(combatantToDict(vm_, atk));
        Handle dH = rs.add(combatantToDict(vm_, def));
        std::array<Handle, 3> args{aH, dH, rng_};
        Handle rH = rs.add(vm_.arena().deref(resolver_).call(vm_, args));
        Value r(vm_, rH);

        if (!r.isDict())
            throw KiritoError("resolver must return a Dict, got '" + r.typeName() + "'");
        Dict rd = r.asDict("resolver result");
        if (!rd.has("hit") || !rd.has("damage"))
            throw KiritoError("resolver Dict must carry both 'hit' and 'damage'");

        bool    hit = r.get("hit").asBool("hit");
        int64_t dmg = r.get("damage").asInt("damage");
        return {hit, dmg};
    }

    KiritoVM& vm_;
    Handle    resolver_;
    Handle    rng_;
};

// A freshly-seeded rng: the trailing `rng` expression is what runSource returns.
static Handle freshRng(KiritoVM& vm) {
    return vm.runSource("import(\"random\")\nvar rng = Random(42)\nrng\n");
}

int main() {
    KiritoVM vm;
    auto compile = [&](const char* src) { return vm.runSource(src); };

    // The real dice policy: d20-to-hit against defense, then a power-sided damage die.
    Handle resolver = compile(R"KI(
Function(attacker: Dict, defender: Dict, rng) -> Dict:
    var roll = rng.randint(1, 20)
    var hit = (roll + attacker["attack"]) >= defender["defense"]
    var dmg = 0
    if hit:
        dmg = rng.randint(1, attacker["power"])
    return {"hit": hit, "damage": dmg}
)KI");

    const Combatant knight{"Knight", 30, 6, 13, 6};
    const Combatant goblin{"Goblin", 24, 4, 11, 5};

    RootScope keep(vm);
    Handle rngH = keep.add(freshRng(vm));

    DiceArena arena(vm, resolver, rngH);
    auto first = arena.fight(knight, goblin, 1000);

    // ---- outcome invariants (true regardless of the exact rolls) ----
    CHECK(first.ended);                 // the fight resolves within the round cap
    CHECK(first.rounds > 0);
    CHECK(first.loserHp == 0);          // the loser is dropped to exactly zero
    CHECK(first.winnerHp > 0);          // the winner is still standing
    CHECK(first.winnerHp <= 30);        // nobody gains hp
    CHECK(first.minHp >= 0);            // hp is never negative at any point
    CHECK(!first.winner.empty());
    CHECK(first.winner == "Knight" || first.winner == "Goblin");

    // ---- determinism: a fresh Random(42) replays the same fight byte-for-byte ----
    {
        RootScope keep2(vm);
        Handle rng2 = keep2.add(freshRng(vm));
        DiceArena arena2(vm, resolver, rng2);
        auto replay = arena2.fight(knight, goblin, 1000);
        CHECK(replay.winner   == first.winner);
        CHECK(replay.winnerHp == first.winnerHp);
        CHECK(replay.loserHp  == first.loserHp);
        CHECK(replay.rounds   == first.rounds);
    }

    // ---- the persistent rng ADVANCES: continuing the same rng into a rematch diverges from a
    //      fresh-seeded fight (proof the handle carries state across calls) ----
    {
        auto rematch = arena.fight(knight, goblin, 1000);   // same arena → same, now-advanced rng
        CHECK(rematch.ended);
        // At least one of the recorded facts differs from the first fresh-seeded fight.
        bool diverged = rematch.winner != first.winner
                     || rematch.winnerHp != first.winnerHp
                     || rematch.rounds != first.rounds;
        CHECK(diverged);
    }

    // ---- adversarial: resolver returns a Dict missing "damage" → strict validation throws ----
    {
        Handle bad = compile(R"KI(
Function(attacker: Dict, defender: Dict, rng) -> Dict:
    return {"hit": True}
)KI");
        RootScope k(vm);
        Handle rng3 = k.add(freshRng(vm));
        DiceArena badArena(vm, bad, rng3);
        CHECK_THROWS(badArena.fight(knight, goblin, 1000));
    }

    // ---- adversarial: resolver returns a non-Dict entirely → throws ----
    {
        Handle wrong = compile(R"KI(
Function(attacker: Dict, defender: Dict, rng):
    return rng.randint(1, 6)
)KI");
        RootScope k(vm);
        Handle rng4 = k.add(freshRng(vm));
        DiceArena wrongArena(vm, wrong, rng4);
        CHECK_THROWS(wrongArena.fight(knight, goblin, 1000));
    }

    return RUN_TESTS();
}
