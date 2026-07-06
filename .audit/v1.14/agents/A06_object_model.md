# A06 — Object model audit (v1.14)

Scope: object.hpp, class_value.hpp, instance_value.hpp, function_value.hpp, module_value.hpp,
plus the runtime.hpp instance-slot bodies (dunder dispatch, privacy, _super_, attr get/set,
method binding, class-attr copy-on-write). Read-only on src/.

STATUS: in progress

Prior context: v1.13 A07 found A07-1..13 (iter self-recursion crash, findMethod recursion,
str depth, VM-less equals inconsistency, _str_ not type-checked, bound-method identity,
plus coverage gaps). Hunting NEW angles for v1.14.

---
