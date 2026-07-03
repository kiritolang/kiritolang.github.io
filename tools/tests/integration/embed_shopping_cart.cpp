// embed_shopping_cart.cpp — an e-commerce pricing engine with stackable discount rules. C++ owns
// the cart (a List of line-item Dicts) and the checkout loop; Kirito owns each promotion rule — a
// Function(cart: List, subtotal: Integer) -> Integer returning a discount in cents. C++ sums the
// subtotal, applies promotions in order (accumulating discounts, clamping the running total at zero),
// and returns the final total.
//
// Flow per checkout: C++ (build cart Dicts + subtotal) → for each promotion → Kirito (discount
// decision) → C++ (accumulate + clamp) → final total in cents.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

struct LineItem {
    std::string sku;
    int64_t     qty;
    int64_t     unit_price_cents;
};

// A cart is a List of {"sku","qty","unit_price_cents"} Dicts — the shape every promotion receives.
static Handle cartToList(KiritoVM& vm, const std::vector<LineItem>& cart) {
    List xs(vm);
    for (const LineItem& li : cart) {
        Dict d(vm);
        d.set("sku",              Value(vm, li.sku));
        d.set("qty",              Value(vm, li.qty));
        d.set("unit_price_cents", Value(vm, li.unit_price_cents));
        xs.push(Value(vm, d.handle()));
    }
    return xs.handle();
}

class PricingEngine {
public:
    explicit PricingEngine(KiritoVM& vm) : vm_(vm) {}
    void addPromotion(Handle fn) { promotions_.push_back(fn); }

    static int64_t subtotalOf(const std::vector<LineItem>& cart) {
        int64_t sub = 0;
        for (const LineItem& li : cart) sub += li.qty * li.unit_price_cents;
        return sub;
    }

    // Compute the final total: subtotal minus every promotion's discount, accumulated in order,
    // never letting the running total drop below zero.
    int64_t checkout(const std::vector<LineItem>& cart) {
        RootScope rs(vm_);
        Handle cartH = rs.add(cartToList(vm_, cart));
        int64_t subtotal = subtotalOf(cart);
        Value subtotalV(vm_, subtotal);

        int64_t total = subtotal;
        for (Handle pH : promotions_) {
            std::array<Handle, 2> args{cartH, subtotalV.handle()};
            Handle discH = rs.add(vm_.arena().deref(pH).call(vm_, args));
            Value disc(vm_, discH);
            // A promotion MUST return an Integer number of cents. Anything else is a bug in the
            // rule — surface it loudly rather than silently coercing.
            if (!disc.isInt())
                throw KiritoError("pricing: promotion must return an Integer discount, got '" +
                                  disc.typeName() + "'");
            total -= disc.asInt("discount");
            if (total < 0) total = 0;   // C++ clamps: a rule can never make the customer owe negative.
        }
        return total;
    }

private:
    KiritoVM&           vm_;
    std::vector<Handle> promotions_;
};

int main() {
    KiritoVM vm;
    auto compile = [&](const char* src) { return vm.runSource(src); };

    // A representative cart: 2 widgets @ $12.50, 4 gizmos @ $3.00, 1 gadget @ $40.00.
    //   subtotal = 2*1250 + 4*300 + 1*4000 = 2500 + 1200 + 4000 = 7700 cents ($77.00)
    const std::vector<LineItem> cart = {
        {"widget", 2, 1250},
        {"gizmo",  4,  300},
        {"gadget", 1, 4000},
    };
    CHECK(PricingEngine::subtotalOf(cart) == 7700);

    // Sanity-check the C++→Kirito cart marshalling: round-trip the Dict list and read the fields
    // back with items()/get() — the exact shape every promotion will iterate.
    {
        Value cartV(vm, cartToList(vm, cart));
        std::vector<Value> lines = cartV.items();
        CHECK(lines.size() == 3);
        CHECK(lines.at(0).get("sku").asStringRef("sku") == "widget");
        CHECK(lines.at(0).get("qty").asInt("qty") == 2);
        CHECK(lines.at(2).get("unit_price_cents").asInt("price") == 4000);
    }

    // Promotion A: 10% off orders over $50 (5000 cents). Uses floordiv for integer percentage.
    const char* tenPercentOver50 = R"KI(
Function(cart, subtotal) -> Integer:
    if subtotal > 5000:
        return subtotal // 10
    return 0
)KI";

    // Promotion B: buy-3-get-1-free on "gizmo" — for every 4 gizmos, one gizmo's unit price is free.
    // Iterates the cart List inside Kirito to find the gizmo line and its unit price.
    const char* buy3get1Gizmo = R"KI(
Function(cart, subtotal) -> Integer:
    var discount = 0
    for item in cart:
        if item["sku"] == "gizmo":
            var free = item["qty"] // 4
            discount = discount + free * item["unit_price_cents"]
    return discount
)KI";

    // Promotion C: flat $5.00 coupon.
    const char* flatCoupon = R"KI(
Function(cart, subtotal) -> Integer:
    return 500
)KI";

    // ---- scenario 1: 10%-off alone ----
    {
        PricingEngine eng(vm);
        eng.addPromotion(compile(tenPercentOver50));
        // 7700 - 7700//10 (=770) = 6930
        CHECK(eng.checkout(cart) == 6930);
    }

    // ---- scenario 2: all three promotions stack, in registration order ----
    {
        PricingEngine eng(vm);
        eng.addPromotion(compile(tenPercentOver50));  // -770
        eng.addPromotion(compile(buy3get1Gizmo));     // 4 gizmos → 1 free @ 300 = -300
        eng.addPromotion(compile(flatCoupon));        // -500
        // 7700 - 770 - 300 - 500 = 6130
        CHECK(eng.checkout(cart) == 6130);
    }

    // ---- scenario 3: buy-3-get-1 alone; a cart with 8 gizmos frees exactly 2 ----
    {
        PricingEngine eng(vm);
        eng.addPromotion(compile(buy3get1Gizmo));
        std::vector<LineItem> bulk = {{"gizmo", 8, 300}};
        // subtotal 2400; free = 8//4 = 2 → -600; total 1800
        CHECK(eng.checkout(bulk) == 1800);
    }

    // ---- scenario 4: threshold boundary — a $40 cart does NOT qualify for the 10% rule ----
    {
        PricingEngine eng(vm);
        eng.addPromotion(compile(tenPercentOver50));
        std::vector<LineItem> small = {{"gadget", 1, 4000}};
        CHECK(eng.checkout(small) == 4000);   // subtotal 4000, not > 5000, no discount
    }

    // ---- scenario 5: empty cart — subtotal 0, every promo that reads subtotal is a no-op ----
    {
        PricingEngine eng(vm);
        eng.addPromotion(compile(tenPercentOver50));
        eng.addPromotion(compile(buy3get1Gizmo));
        std::vector<LineItem> none;
        CHECK(eng.checkout(none) == 0);
    }

    // ---- adversarial: a promotion returning a NEGATIVE discount larger than the subtotal would
    //      push the total negative — C++ must clamp it to zero. ----
    {
        PricingEngine eng(vm);
        // Returns a discount of -(subtotal + 1000): a "discount" that ADDS more than the whole cart.
        eng.addPromotion(compile(R"KI(
Function(cart, subtotal) -> Integer:
    return -(subtotal + 1000)
)KI"));
        int64_t total = eng.checkout(cart);
        CHECK(total == 0);        // clamped, never negative
        CHECK(total >= 0);
    }

    // ---- adversarial: clamp holds even when a later promo tries to claw the total back up after
    //      an earlier one already clamped it to zero ----
    {
        PricingEngine eng(vm);
        eng.addPromotion(compile(R"KI(
Function(cart, subtotal) -> Integer:
    return subtotal + 9999
)KI"));                                                  // over-discounts → total clamps to 0
        eng.addPromotion(compile(flatCoupon));           // another -500 off zero
        CHECK(eng.checkout(cart) == 0);
    }

    // ---- adversarial: a promotion returning a non-Integer (Float) must throw ----
    {
        PricingEngine eng(vm);
        eng.addPromotion(compile(R"KI(
Function(cart, subtotal) -> Integer:
    return subtotal / 10
)KI"));                                                  // true division → Float, not Integer
        CHECK_THROWS(eng.checkout(cart));
    }

    // ---- adversarial: a promotion returning a String must throw ----
    {
        PricingEngine eng(vm);
        eng.addPromotion(compile("Function(cart, subtotal): return \"free!\"\n"));
        CHECK_THROWS(eng.checkout(cart));
    }

    // ---- adversarial: a promotion that indexes a missing cart key throws out of Kirito ----
    {
        PricingEngine eng(vm);
        eng.addPromotion(compile(R"KI(
Function(cart, subtotal) -> Integer:
    return cart[0]["nonexistent"]
)KI"));
        CHECK_THROWS(eng.checkout(cart));
    }

    // ---- determinism: the same cart + promotion set yields the same total every time ----
    {
        PricingEngine eng(vm);
        eng.addPromotion(compile(tenPercentOver50));
        eng.addPromotion(compile(buy3get1Gizmo));
        int64_t first = eng.checkout(cart);
        for (int i = 0; i < 5; ++i)
            CHECK(eng.checkout(cart) == first);
    }

    return RUN_TESTS();
}
