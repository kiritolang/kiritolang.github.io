# A08 — classes / dunders / inheritance / super / privacy / scoping

Audit of Kirito v1.16.1. Findings appended incrementally.

### F08-1 [Med] isinstance / typed-catch / type-annotation match user classes by NAME, not identity — same-named classes from different definitions conflate
- runtime.hpp:2083 `typeMatches` — walks the instance's class chain comparing `c.name == typeName` (a string). A resolved class arg is reduced to its `name` first (`resolveTypeName`, 2142). So an instance of one class `Foo` satisfies `isinstance(x, Foo)` / `catch Foo` / `-> Foo:` for an UNRELATED class also named `Foo`.
- Trigger (CONFIRMED, build-debug): a module `foomod` defines `class Foo`, the script defines its own `class Foo`; `isinstance(foomod.make(), Foo)` → `True` though the objects are instances of two different, unrelated classes. Same collision hits typed `catch Foo as e` and enforced parameter/return annotations.
- Why it matters: a soundness hole in the type gate — a subtype/instance check can pass for a value that is not of the intended type. Realistic whenever two modules (or a module + script) share a class name.
- Design tension: name-based matching is what lets serde/dump reconnect a deserialized instance to a rebuilt same-named class. But identity-with-name-fallback (prefer handle identity, fall back to name only for cross-VM/rebuilt cases) would close the in-VM hole. Contrast: privacy's `classIsSubclassOf` (2288) correctly uses handle IDENTITY (`cur == superH`).
- Fix idea: for an in-VM `isinstance(x, ClassValue)` where the arg is a live class handle, walk by handle identity; keep name-matching only for the String-name form and cross-VM reconnection. At minimum document the collision.
- Test to add: two same-named unrelated classes; assert `isinstance` / typed catch distinguish them (or document that they don't).
- Verified-real: YES (repro above).

