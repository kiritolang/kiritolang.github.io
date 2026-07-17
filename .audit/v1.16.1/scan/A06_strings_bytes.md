# A06 — strings + bytes (v1.16.1)

Audited directly via a broad adversarial pass (now the permanent golden test
`tests/scripts/spec_v1161_strings.ki`). Covered: unicode code-point index/slice/reverse, slice OOB
clamps + step, every method's optional args (split/find/rfind/count/replace/strip/startswith/endswith/
removeprefix/removesuffix/zfill/ljust/rjust/center/partition/levenshtein), unicode-aware case (Latin-1),
the format mini-spec (#alt / zero-pad / width / .precision / , / sign), f-strings (specs + single-quoted
dict key), Bytes (encode/decode utf-8+latin-1 round-trip, hex, Bytes-from-list, ascii-encode-throws), and
str-vs-repr in containers.

## Result: NO bugs found. (One initial FAIL was a bad test assertion — `format(255,"08b")` is "11111111",
255 being exactly 8 bits; format is correct.)

## Coverage notes
- rpartition / rindex / index (throwing variants) / splitlines / expandtabs / casefold — exercised
  elsewhere; not in this adversarial spec. isdigit/isalpha/isalnum/isspace/islower/isupper covered by
  older specs.
- Bytes slicing yields Bytes; iteration yields Integers; `+`/`*`/ordering/hashing — partially covered.
- The `string` module fuzzy matching (similarity/closest/fuzzymatch) built on levenshtein — separate area.
