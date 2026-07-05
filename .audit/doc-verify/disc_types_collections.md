# Discrepancies — collection types (List / Set / Dict)

Area verified against `docs/pages/09-types.md`, `src/kirito/collections.hpp`, `src/kirito/runtime.hpp`.
Test: `tools/tests/scripts/verify_types_collections.ki` (145 asserts). No source was modified; current
behaviour is PINNED by the assertions noted below.

## 1. `in` on a Set with an unhashable element returns `False` silently (Dict `in` throws)

`x in <Set>` (`SetVal::contains(const ObjectArena&)`) short-circuits `if (!v.hashable()) return false;`,
so `[1] in {1, 2}` evaluates to `False` — no error. But `x in <Dict>` goes through `DictVal::find` →
`requireHashable(k)`, so `[1] in {"a": 1}` **throws** `unhashable type 'List'`. Asymmetric handling of
an unhashable membership probe between the two hash-bucketed containers.

- Not necessarily wrong (an unhashable value provably can't be a member, so `False` is defensible), but
  the inconsistency with Dict is surprising. Flagging, not fixing.
- PINNED: `assert ([1] in {1, 2}) == False` and `raises_msg(... {}[[1]] ..., "unhashable type 'List'")`.

## 2. `Set.remove` on an unhashable value throws `"unhashable type"` — WITHOUT the type name

`SetVal::getAttr` "remove" throws `KiritoError("unhashable type")` (no name), whereas `Set.add` /
`Dict[...]` throw via `requireHashable` → `"unhashable type '<TypeName>'"` (with the name). Minor
message inconsistency; only the value-name suffix differs. (Not asserted in the test to avoid pinning a
name-less message as canonical; noted here.)

## 3. `extend` / `update` non-iterable fallback messages are unreachable for scalar args

`xs.extend(5)` and `{}.update(5)` surface `type 'Integer' is not iterable` (thrown by
`Object::iterate`), never the hand-written fallbacks `"extend expects an iterable"` /
`"update expects a Dict or an iterable of [key, value] pairs"`. Those fallbacks only fire for a type
whose `iterate()` returns `std::nullopt` rather than throwing (none of the built-ins do). The surfaced
message is still clear and correct, so this is a dead-path note, not a defect.

- PINNED with needle `"not iterable"` for both cases.

## Everything else verified clean

All documented List/Set/Dict methods and operators behave exactly as `09-types.md` describes, with the
exact error messages from the implementation (`pop from empty List`, `index out of range`,
`remove: value not in List`, `key not found: <k>`, `pop: key not found`, `popitem: dictionary is empty`,
`pop from an empty Set`, `remove: value not in Set`, set operators requiring a Set RHS →
`does not support this binary operator`, etc.). Dict/Set iteration order is never asserted (unordered by
design); contents are checked via `len`, membership, and `sorted(List(...))`.
