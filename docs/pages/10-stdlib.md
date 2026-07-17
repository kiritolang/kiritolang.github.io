# Standard Library

Import a module with `import("name")`; each module loads once per VM. Every entry below lists a
signature (`name(args) → ReturnType`), what it takes, and what it does. Fixed-arity functions accept
**keyword arguments** and **defaults**; `inspect(module)` prints the same signatures live.

A leading `*args` denotes a variadic positional list; `[arg]` denotes an optional argument.

---

## arg

A small command-line argument parser. Build a `Parser`, declare the arguments on it, then
run `parse` against a list of strings **yourself** — typically the main file's `arglist` (recall
`arglist` is empty in imported modules, so argument handling belongs in the program you run).

- `Parser(description = "") → Parser` — create a parser.

### Parser object

The configuration lives on the instance; the declaration methods each return the parser, so they
chain:

- `p.positional(name, help = "") → Parser` — a required positional argument (consumed in order).
- `p.option(name, default = None, help = "") → Parser` — a `--name VALUE` option. The value is
  converted to the **type of `default`** — an Integer default parses the value as an Integer (a bad
  value throws a clear error), a Float default as a Float, otherwise it stays a String.
- `p.flag(name, help = "") → Parser` — a boolean `--name` flag (default `False`, `True` when present).
- `p.usage() → String` — the generated usage/help text.
- `p.parse(args) → Dict` — parse `args` into a Dict keyed by argument name. Returns **`None`** if
  `-h`/`--help` is present (after printing `usage()`), so the program can stop. Accepts `--name value`,
  `--name=value`, and short `-n value` / `-f` (matched by the name's first letter); extra positionals
  are collected under the `"rest"` key. Throws a clear, catchable error on an unknown option, a
  missing required positional, or a value that can't be converted.

```kirito
var io = import("io")
var arg = import("arg")

var p = arg.Parser("greet someone")
p.positional("name")
p.option("count", 1)        # --count N  (parsed as an Integer, because the default is one)
p.flag("loud")              # --loud

var opts = p.parse(["Ada", "--loud"])   # normally you pass `arglist` (the real CLI args)
if opts != None:              # None means --help was shown
    var greeting = f"Hello, {opts['name']}!"
    if opts["loud"]:
        greeting = greeting.upper()
    var i = 0
    while i < opts["count"]:
        io.print(greeting)
        i = i + 1
```

Run as `ki greet.ki Ada --count 2 --loud` → prints `HELLO, ADA!` twice.

---

## base64

Operates on **byte values**: a `List` of Integers (0–255), a [`Bytes`](types.html#bytes), or a
`String` (encoded as its UTF-8 bytes).

- `encode(data: List | Bytes | String) → String` — Base64-encode the data.
- `decode(s: String) → List` — decode Base64 text back to a list of byte values.
- `urlsafeencode(data: List | Bytes | String) → String` — encode using the URL-safe alphabet (`-_`).
- `urlsafedecode(s: String) → List` — decode using the URL-safe alphabet (`-_`).

---

## bisect

Binary search / ordered insertion into a sorted List.

- `bisectleft(a, x) → Integer` — the leftmost insertion index that keeps `a` sorted.
- `bisectright(a, x) → Integer` — the rightmost insertion index that keeps `a` sorted.
- `insortleft(a, x) → None` — insert `x` into the sorted List `a` at the leftmost valid position.
- `insortright(a, x) → None` — insert `x` into the sorted List `a` at the rightmost valid position.
- `bisect(a, x)` / `insort(a, x)` — aliases for the `*right` variants.

---

## collections

- `deque([iterable]) → deque` — a double-ended queue.
- `Counter([iterable]) → Counter` — a multiset that tallies its elements.
- `defaultdict(factory) → defaultdict` — a Dict that fills a missing key by calling `factory()`.

### deque object

- `dq.append(x) → None` — add `x` to the right end.
- `dq.appendleft(x) → None` — add `x` to the left end.
- `dq.pop()` — remove and return the rightmost element.
- `dq.popleft()` — remove and return the leftmost element.
- `dq[i]` — the element at index `i`.
- `len(dq) → Integer` — the number of elements.
- iterable — a `for` loop yields the elements left to right.

### Counter object

- `c.add(x) → None` — increment the count for `x`.
- `c.get(x) → Integer` — the count for `x` (`0` if unseen).
- `c[x] → Integer` — index syntax for the count of `x`.
- `c.items() → List` — `[value, count]` pairs.
- `c.mostcommon([n: Integer]) → List` — `[value, count]` pairs, highest count first. The sort is
  stable and the underlying Dict is insertion-ordered, so *tied* counts appear in first-insertion
  order. With `n`, only the top `n`; `n = 0` gives `[]`, and a **negative**
  `n` returns all but the `|n|` least-common pairs (an end-slice — don't pass a negative `n` expecting
  an empty list).

### defaultdict object

- `d[k]` — the value for `k`, inserting `factory()` if `k` is absent.
- `d[k] = v` — set the value for `k`.
- `k in d → Bool` — whether `k` is present.
- `d.keys() → List` — all keys.
- `d.values() → List` — all values.
- `d.items() → List` — all `[key, value]` pairs.

---

## complex

Complex numbers and complex matrices, implemented in C++ (`std::complex<double>`). Reals coerce to
the real axis, so any function or operator below also accepts plain `Integer`/`Float` arguments.

### Constructors and constants

- `Complex(re: Number, im: Number = 0) → Complex` — build `re + im·i`.
- `of(re: Number, im: Number) → Complex` — the two-argument constructor.
- `real(re: Number) → Complex` — a real number on the complex plane (`re + 0i`).
- `polar(r: Number, theta: Number) → Complex` — from polar form, `r·(cos θ + i·sin θ)`.
- `i`, `zero`, `one` — the imaginary unit, `0`, and `1` as `Complex` values.
- No `pi`/`e`/`tau` here: those are real-axis constants with a single source of truth in
  `math.pi`/`math.e`/`math.tau`. Lift one to the complex plane with `real(math.pi)` if you need it as
  a `Complex` value.

### Complex object

- `z.re → Float`, `z.im → Float` — the real and imaginary parts (also `z.real`/`z.imag`).
- `z1 + z2`, `z1 - z2`, `z1 * z2`, `z1 / z2`, `z1 ** z2`, `-z` — arithmetic. A `Complex` must be the
  left operand when mixing with a number (`z + 2`, not `2 + z`). Division by zero throws.
- `z1 == z2 → Bool` — **exact** equality (real and imaginary parts bit-equal; `NaN` never equal); a
  `Complex` with zero imaginary part also equals the matching real number (`Complex(2, 0) == 2`).
- `z.compare(other, rel_tol = 1e-9, abs_tol = 0.0) → Bool` — tolerant comparison (relative/absolute tolerance on the
  complex magnitude); use it for computed values, e.g. `(1+i)**2 ≈ 2i`.
- Complex numbers are **unordered**: `<`, `<=`, `>`, `>=` throw.
- `z.conjugate() → Complex` — the complex conjugate.
- `z.modulus() → Float` — the magnitude `|z|` (also `z.magnitude()` / `z.abs()`).
- `z.argument() → Float` — the phase angle in radians (also `z.arg()` / `z.phase()`).
- `z.norm2() → Float` — the squared magnitude `|z|²` (no square root).
- `z.is_zero() → Bool` — True when `z` is (numerically) zero: magnitude below `1e-10` (a tolerant
  check, deliberately unlike the exact `==`).

### Module functions

Scalar reductions (one per line):

- `modulus(z) → Float` — the magnitude `|z|` (alias `abs`).
- `phase(z) → Float` — the phase angle, in radians (alias `argument`).
- `norm2(z) → Float` — the squared magnitude `|z|²`.
- `conjugate(z) → Complex` — the complex conjugate.

The analytic math set — the complex extensions of the `math` functions — each take a `Complex` or a
number and return a `Complex`. They are defined across the whole complex plane (so `sqrt(-1)` → `i`,
`log(-1)` → `iπ`, `asin(2)`/`acosh(0)` are valid), but the true singularities throw a `math domain
error` on the same out-of-domain inputs: `log(0)`/`log10(0)`, `atanh(±1)`, and `pow`/`**` of zero to
a negative or non-real power (`zero ** -1`):

- `exp(z)`
- `log(z)` — natural logarithm (principal branch); throws on `0`.
- `log10(z)` — throws on `0`.
- `sqrt(z)` — principal square root.
- `cbrt(z)` — principal cube root.
- `pow(z, w)` — `z` raised to the power `w`.
- `sin(z)`
- `cos(z)`
- `tan(z)`
- `asin(z)`
- `acos(z)`
- `atan(z)`
- `sinh(z)`
- `cosh(z)`
- `tanh(z)`
- `asinh(z)`
- `acosh(z)`
- `atanh(z)`

### Complex matrices

- `Matrix(rows: List) → ComplexMatrix` — build from a nested list whose cells are `Complex` values
  or numbers (rows must be equal length).
- `zeros(rows, cols) → ComplexMatrix` — a zero matrix.
- `ones(rows, cols) → ComplexMatrix` — a matrix of `1+0i`.
- `identity(n) → ComplexMatrix` — the n×n identity.
- `vector(values: List) → ComplexMatrix` — a 1×n complex row vector.

Like real matrices, complex matrices are arbitrary-shape; `determinant`/`inverse`/`trace` need a
square matrix and throw otherwise.

#### ComplexMatrix object

- `m[i, j] → Complex`, `m[i] → List`, `m[i, j] = v` — element / row access and assignment.
- `m.get(i, j)`, `m.set(i, j, v)`, `m.row(i)`, `m.rows()`, `m.cols()`, `m.shape()`.
- `m1 == m2 → Bool` — **exact** equality; `m.compare(other, rel_tol = 1e-9, abs_tol = 0.0) → Bool`
  is the tolerant form for computed matrices.
- `m + n`, `m - n`, `m * n` — addition/subtraction and matrix or scalar (`Complex`/number) multiply.
- `m.transpose() → ComplexMatrix` — the transpose.
- `m.conjugate() → ComplexMatrix` — element-wise complex conjugate.
- `m.hermitian() → ComplexMatrix` — the conjugate transpose (also `m.conjugatetranspose()`).
- `m.determinant() → Complex` — determinant via **Gaussian elimination** with partial pivoting.
- `m.inverse() → ComplexMatrix` — inverse via **fast O(n³) Gauss-Jordan** elimination (throws if
  singular).
- `m.trace() → Complex` — sum of the diagonal.
- `m.apply(fn) → ComplexMatrix` — a new matrix with `fn` applied to each element.

**Vector operations** (a ComplexMatrix with one dimension equal to 1 is a vector):

- `u.dot(v) → Complex` — the scalar (Hermitian inner) product of two equal-length vectors:
  `Σ conj(uᵢ)·vᵢ`, so `u.dot(u) = Σ |uᵢ|²` is real and non-negative. (`*` is always matrix multiply.)
- `u.cross(v) → ComplexMatrix` — the cross product of two 3-element vectors.
- `m.norm() → Float` — the Euclidean 2-norm `sqrt(Σ |zᵢ|²)`.

---

## copy

- `copy(obj)` — a shallow copy (a `List`/`Dict`/`Set` with the same elements; immutable scalars are
  returned as-is).
- `deepcopy(obj)` — a deep copy (handles shared references and cycles).
- A **class instance** (or a native value object like `Matrix`/`Tensor`/`DateTime`) is copied via the
  `serialize` graph codec — both `copy` and `deepcopy` return an independent (deep) instance, since
  Kirito has no per-instance attribute introspection. A value that can't be serialized (a live
  socket/file) is returned unchanged (best effort).

---

## crypto

Advanced cryptography — AES-GCM authenticated encryption, RSA/EC signatures and RSA encryption, and
X.509 certificate parsing. Built on **OpenSSL**, so it is available only when Kirito is compiled with
`-DKIRITO_ENABLE_TLS` (the `debug`/`release` presets enable it), exactly like the [`net`](#net)
module's HTTPS. The self-contained basics — HMAC, SHA-512, PBKDF2, secure random — live in
[`hash`](#hash) and [`random`](#random) instead, so they work in every build.

- `enabled → Bool` — whether this build has OpenSSL. When `False`, every function below throws a clear
  "requires KIRITO_ENABLE_TLS" error rather than silently doing nothing — branch on it like
  `net.tlsenabled`.

**Argument types** — three kinds, consistent across the module:

- **Byte data** — `key`, `iv`, `aad`, `plaintext`, `message`, `signature`, and the `ciphertext`/`data`
  payloads accept a **`String` or [`Bytes`](types.html#bytes)**, read byte-exact (a `String` contributes
  its UTF-8 bytes, a `Bytes` its raw bytes), so binary payloads work directly. Every **byte result**
  (ciphertext, tag, signature, decrypted plaintext) is returned as **`Bytes`**.
- **Keys & certificates** — `private_pem`, `public_pem`, and the `x509parse` argument are **PEM
  `String`s** (the `-----BEGIN … -----` text) — portable, printable, serializable; there is no native
  key object to leak.
- **Options** — `bits` is an **`Integer`**; `curve` and `algo` are **`String`s** (`algo` is one of
  `"sha256"` / `"sha384"` / `"sha512"`, default `"sha256"`).

- `aesencrypt(key, plaintext, iv, aad = None) → Dict` — AES-GCM authenticated encryption.
  `key` is 16/24/32 bytes (AES-128/192/256); `iv` is a unique per-message value (12 bytes is
  standard — **never reuse an iv with the same key**); optional `aad` is authenticated but not
  encrypted. Returns `{ciphertext: Bytes, tag: Bytes}`.
- `aesdecrypt(key, ciphertext, iv, tag, aad = None) → Bytes` — decrypt and verify. Throws
  `authentication failed` if the key/iv/tag/aad don't match (tampered or wrong) — it never returns
  unauthenticated plaintext.
- `rsagenerate(bits = 2048) → Dict` — a new RSA keypair as `{private: String, public: String}` (PEM).
- `rsasign(private_pem, message, algo = "sha256") → Bytes` / `rsaverify(public_pem, message,
  signature, algo = "sha256") → Bool` — PKCS#1 v1.5 signatures. The key must actually *be* an RSA
  key: a key of another family throws rather than silently signing with that family's algorithm.
- `rsaencrypt(public_pem, data) → Bytes` / `rsadecrypt(private_pem, data) → Bytes` — RSA-OAEP
  (SHA-256). Encrypts a short payload directly (for bulk data, encrypt with AES-GCM and wrap the AES
  key with RSA).
- `ecgenerate(curve = "prime256v1") → Dict` — an EC keypair (PEM); other curves e.g. `secp384r1`,
  `secp521r1`.
- `ecsign(private_pem, message, algo = "sha256") → Bytes` / `ecverify(public_pem, message,
  signature, algo = "sha256") → Bool` — ECDSA signatures. As with `rsasign`, the key must be an EC
  key; an RSA key throws.
- `x509parse(pem) → Dict` — parse a certificate to
  `{subject, issuer, serial, not_before, not_after, sans}` (`sans` is a List of DNS names).

<!--norun (requires an HTTPS/TLS build for OpenSSL)-->
```kirito
var crypto = import("crypto")
if crypto.enabled:
    var key = import("random").randombytes(32)      # AES-256 key
    var iv = import("random").randombytes(12)
    var sealed = crypto.aesencrypt(key, "secret", iv)
    var plain = crypto.aesdecrypt(key, sealed["ciphertext"], iv, sealed["tag"])
    # sign & verify
    var k = crypto.rsagenerate(2048)
    var sig = crypto.rsasign(k["private"], "message")
    assert crypto.rsaverify(k["public"], "message", sig)
```

---

## csv

Low-level CSV parsing/formatting (RFC-4180-style quoting). For tabular data analysis, see
[`tabular`](#tabular), which builds on this.

- `parse(text)` — parse CSV text into a List of rows (each a List of fields).
- `parserow(line)` — parse one CSV line into a List of fields.
- `format(rows) → String` — serialize a List of rows to CSV text.
- `formatrow(fields) → String` — serialize one row.

---

## dump

Compact **binary** serialization (the binary counterpart of `serialize`), preserving references and
cycles. `dumps` returns the blob as [`Bytes`](types.html#bytes); `loads` reconstructs from it.

**Never `loads`/`load` a blob from an untrusted source** — reconstructing a `Function`/`class` value
re-parses and runs its source, so a blob is a program, not data. See
[serialize's security note](#security-never-load-a-blob-you-do-not-trust); use [`json`](#json) for
data that crosses a trust boundary.

- `dumps(value) → Bytes` — serialize to a compact binary blob.
- `loads(data)` — reconstruct the value graph; pass the **`Bytes`** returned by `dumps` directly (do
  **not** wrap it in `String(...)` — the blob is raw binary, and a String round-trip corrupts it).
- `save(value, path) → None` — serialize `value` straight to a file.
- `load(path)` — reconstruct a value from a file written by `save`.

Persist a blob yourself with binary file I/O, e.g. `io.open(path, "wb").write(dump.dumps(x))`. To
round-trip in memory: `dump.loads(dump.dumps(x))`.

---

## enum

- `Enum(names: List) → Enum` — build an enumeration mapping each name to its index.

### Enum object

- `e.get(name) → Integer` — the value (index) of a member; throws on an unknown name.
- `e[name] → Integer` — index syntax for the same lookup as `e.get(name)`.
- `e.nameof(value) → String` — the name for a value.
- `e.names() → List` — all member names.
- `e.values() → List` — all member values.
- `name in e` — membership test.

---

## functools

- `reduce(func, iterable[, initial])` — fold the two-argument `func` over the iterable left-to-right.
- `partial(func, bound: List) → Function` — pre-bind a list of leading arguments. The result takes a **list** of the remaining arguments and calls `func` with the combined argument list (`func` should accept a single list of arguments).
- `cache(func) → Function` — memoize a single-argument function on its argument.

---

## gzip

The gzip container format (RFC 1952) — `.gz` files and HTTP `Content-Encoding: gzip`. A DEFLATE
stream wrapped with a header and a CRC-32 trailer; distinct from the bare zlib stream above, so it is
its own module. Each function takes a **String or a [`Bytes`](types.html#bytes)** and returns the
same type as its input.

- `compress(data) → data` (alias `gzip`) — wrap the DEFLATE body in the gzip container (RFC 1952). A
  valid `.gz` stream interoperable with `gzip(1)`/`gunzip` (OS = unknown, MTIME = 0, so not byte-identical to gzip(1)).
- `decompress(data) → data` (alias `gunzip`) — validate the header, skip the optional filename/extra
  fields, INFLATE, and verify both trailers — the CRC-32 **and** the ISIZE (uncompressed length)
  (throws on a corrupt stream). Multiple gzip members concatenated into one stream (`cat a.gz b.gz`)
  are all decoded and joined.

Pair with `net.get(url).content` (raw `Bytes`) to fetch and unpack a `.gz` resource:

<!--norun (requires network access and an HTTPS/TLS build)-->
```kirito
var net = import("net")
var gzip = import("gzip")
var resp = net.get("https://example.com/data.csv.gz")
var csv = gzip.decompress(resp.content).decode("utf-8")   # raw bytes -> text
```

---

## hash

Digests and checksums of byte data, plus a generic hasher for `Dict`/`Set`-compatible values. The
byte-data functions each take a **String or a [`Bytes`](types.html#bytes)**, so binary data hashes
correctly; `hash(value)` accepts any hashable value.

- `md5(data) → String` — MD5 lowercase-hex digest.
- `sha1(data) → String` — SHA-1 lowercase-hex digest.
- `sha256(data) → String` — SHA-256 lowercase-hex digest.
- `sha384(data) → String` — SHA-384 lowercase-hex digest.
- `sha512(data) → String` — SHA-512 lowercase-hex digest.
- `hmac(key, msg, algo = "sha256") → String` — the keyed-hash message authentication code (HMAC,
  RFC 2104), lowercase hex. `key` and `msg` are each a String or Bytes; `algo` is one of
  `md5`/`sha1`/`sha256`/`sha384`/`sha512` (an unknown name throws). Use it to authenticate a message
  with a shared secret.
- `pbkdf2(password, salt, iterations, dklen = 32, algo = "sha256") → Bytes` — derive a key from a
  password (PBKDF2-HMAC, RFC 8018). `password`/`salt` are String or Bytes; `iterations` ≥ 1 (higher is
  slower to brute-force); `dklen` is the output length in bytes (1…1048576). Returns Bytes — call
  `.hex()` for a hex string. This is the right primitive for password storage / key stretching.
- `comparedigest(a, b) → Bool` — constant-time equality for MACs and digests: the comparison time
  does not depend on where the first differing byte is, so it can't leak a secret via timing. `a`/`b`
  are both String or both Bytes. Use it (not `==`) to verify an HMAC or a stored digest.
- `adler32(data) → Integer` — Adler-32 checksum (as zlib uses).
- `crc32(data) → Integer` — CRC-32 (IEEE) checksum (as gzip/PNG use).
- `crc64(data) → Integer` — CRC-64/XZ checksum, returned as a signed Integer (the top bit makes large
  values negative, since Kirito integers are 64-bit signed).
- `hash(value) → Integer` — the same hash `Dict`/`Set` bucket their keys by, exposed to Kirito. Works
  on every hashable value: `Integer` (identity), `Float`, `Bool`, `None`, `String` and `Bytes`
  (content-based), and a user-class instance whose class defines
  [`_hash_`](types.html#hashability-set-dict-keys). An unhashable input throws `unhashable type
  '<name>'` — the same message a `Dict[key]` assignment would produce. Compose this inside a
  class's own `_hash_` to fold nested attributes (`return hash.hash(self.email)`).

---

## heapq

A min-heap maintained inside an ordinary List.

- `heapify(items) → List` — return a **new** heap-ordered List from `items` (does not modify `items`).
- `heappush(heap, item) → None` — push onto `heap` in place, keeping the heap invariant.
- `heappop(heap)` — pop and return the smallest element.
- `heapreplace(heap, item)` — pop the smallest, then push `item` (one pass).
- `merge(lists) → List` — merge already-sorted inputs (a List of Lists) into one sorted List.
- `nlargest(n, items) → List` — the `n` largest elements.
- `nsmallest(n, items) → List` — the `n` smallest elements.

---

## int

Arbitrary-precision integers — a `BigInt` value type that grows past the native `Integer`'s 64-bit
range, plus the integer-meaningful math functions and primality testing. Pure C++ (no external
dependency).

`BigInt` follows the language's conventions: arithmetic dispatches on the **left** operand (so
`BigInt(2) + 3` works but `3 + BigInt(2)` throws — put the BigInt first, or wrap the other side with
`int.big(x)`), only `==`/`!=` are symmetric across types, and `/` is **true division** yielding a
`Float` (use `//` for a BigInt floor-quotient). A BigInt hashes equal to an equal native `Integer`, so
they interchange as `Dict`/`Set` keys, and it serializes through `serialize`/`dump`.

- `BigInt(value) → BigInt` / `big(value) → BigInt` — from an Integer, a String (decimal, or `0x`/`0o`/
  `0b`-prefixed), or another BigInt.
- `fromstring(s, base = 10) → BigInt` — parse in an explicit base 2…36.
- `gcd(a, b) → BigInt`, `lcm(a, b) → BigInt` — greatest common divisor / least common multiple.
- `factorial(n) → BigInt`, `comb(n, k) → BigInt`, `perm(n, k) → BigInt` — exact (unbounded) analogues
  of the `math` builtins, which stay int64 and throw on overflow.
- `isqrt(n) → BigInt` — integer square root (floor of √n).
- `abs(n) → BigInt`.
- `pow(base, exp) → BigInt` — exact integer power (`exp` ≥ 0).
- `modpow(base, exp, mod) → BigInt` — modular exponentiation `base**exp % mod` (efficient; `exp` ≥ 0).
- `modinv(a, m) → BigInt` — modular inverse of `a` mod `m` (throws if `a` and `m` aren't coprime).
- `isprime(n) → Bool` — **deterministic** primality by trial division (O(√n); a tight native loop for
  values that fit int64, exact but slow for very large `n`).
- `isprobableprime(n, rounds = 25) → Bool` — **probabilistic** primality (Miller-Rabin with random
  bases from the OS CSPRNG); fast even for very large `n`, with a false-positive probability below
  `4^-rounds`.
- `randomprime(bits, rounds = 25) → BigInt` — a random prime of exactly `bits` bits.
- `zero`, `one` — BigInt constants.

`isprobableprime` and `randomprime` draw from the OS cryptographic RNG (see
[secure random](#random)); if it is unavailable they **throw** rather than fall back to predictable
values — a fixed Miller-Rabin base or a guessable prime would silently defeat them. The deterministic
`isprime` uses no randomness and always works.

`BigInt` methods: `n.modpow(exponent, modulus)`, `n.isprime()`, `n.isprobableprime(rounds = 25)`,
`n.bitlength() → Integer`, `n.toint() → Integer` (throws if it doesn't fit a native Integer).

```kirito
var int = import("int")
assert String(int.factorial(30)) == "265252859812191058636308480000000"
assert int.pow(2, 128) == int.fromstring("340282366920938463463374607431768211456")
assert int.BigInt(10) / int.BigInt(4) == 2.5           # true division -> Float
assert int.BigInt(10) // int.BigInt(4) == int.BigInt(2)  # floor division -> BigInt
assert int.gcd(int.factorial(20), int.factorial(15)) == int.factorial(15)
assert int.isprime(97) and not int.isprime(561)      # 561 is a Carmichael number
assert int.BigInt(4).modpow(13, 497) == 445
```

---

## io

Console I/O, files, and in-memory buffers — actual I/O only. Path and filesystem operations live in
the dedicated [`path`](#path) module.

### Console (interchangeable streams)

`io.print`/`write`/`input`/`read` act on whatever is bound to `io.stdout` / `io.stderr` / `io.stdin`
— rebindable stream objects that accept any `File`, `BytesIO`, other stream, or object exposing
`write`/`readline`/`read`. The originals live at `io.__stdout__` / `io.__stderr__` / `io.__stdin__`.
Each call also takes an optional `stream=` keyword to redirect that single call without rebinding.

- `print(*args, stream = io.stdout) → None` — write the arguments space-separated, newline-terminated and flushed.
- `eprint(*args, stream = io.stderr) → None` — like `print`, but defaulting to `stderr`.
- `write(*args, stream = io.stdout) → None` — write the arguments with no separator and no trailing newline.
- `input([prompt], stream = io.stdin) → String` — write `prompt` (if given) to `stdout`, then read and return one line from `stream` (without the newline).
- `read([n], stream = io.stdin) → String` — read `n` characters from `stream`, or everything until EOF if `n` is omitted.
- `stdout` / `stderr` / `stdin` — the current standard streams (rebindable); `__stdout__` / `__stderr__` / `__stdin__` hold the originals.

### Files and buffers

- `open(path: String, mode: String = "r") → File` — open a file. Modes: `"r"` read, `"w"` truncate-write, `"a"` append, `"r+"` read/write. Append a `"b"` (`"rb"`/`"wb"`/`"ab"`/`"r+b"`) for **binary** mode: `read`/`readline`/iteration then yield [`Bytes`](types.html#bytes) and `write`/`writelines` accept Bytes (the right mode for non-text files — images, gzip, `dump` blobs). Throws if it can't be opened. Usable as a `with` context manager.
- `BytesIO([initial: String]) → BytesIO` — an in-memory read/write byte buffer, usable anywhere a file or stream is expected.

### File object

Returned by `io.open`. Iterating a file yields its remaining lines.

- `f.read([size]) → String` — read `size` characters, or the whole rest of the file if omitted. (In binary mode, returns [`Bytes`](types.html#bytes); `size` counts bytes.)
- `f.readline() → String` — read one line (without the trailing newline). (Bytes in binary mode.)
- `f.readlines() → List` — read all remaining lines into a List.
- `f.write(data: String | Bytes) → None` — write `data` at the current position. Throws on a closed file
  or one opened read-only — a write is never silently dropped.
- `f.writelines(lines) → None` — write each String/Bytes in an iterable (same throwing rules).
- `f.seek(offset: Integer, whence: Integer = 0) → Integer` — move the read/write cursor and return the
  new position. `whence` is `0` (from the start, the default), `1` (relative to the current position),
  or `2` (from the end).
- `f.tell() → Integer` — the current byte position.
- `f.flush() → None` — flush buffered output.
- `f.close() → None` — close the file (also done automatically on `with` exit / collection).
  Reading, writing, or seeking a **closed** file throws a catchable error; reading a write-only
  (`"w"`/`"a"`) file or writing a read-only (`"r"`) one likewise throws.

### BytesIO object

- `b.write(data: String | Bytes) → Integer` — write bytes at the cursor (overwriting/extending); returns the count written.
- `b.read([size]) → String` — read `size` bytes from the cursor, or the rest if omitted (an explicit
  `read(None)` also reads the rest, unlike `File.read` which requires an Integer).
- `b.readline() → String` — read up to and including the next newline (returned without it).
- `b.getvalue() → String` — the entire buffer contents.
- `b.tell() → Integer` — the current cursor position.
- `b.seek(offset[, whence]) → Integer` — move the cursor (whence 0=start, 1=cur, 2=end). A resulting
  position before the start clamps to 0 (whereas `File.seek` to a negative position throws).
- `b.size() → Integer` — total buffer length in bytes (`len(b)` also works).
- `b.truncate() → Integer` — drop everything after the cursor.
- `b.flush() → None` — a no-op (the buffer is always in sync); present for the common stream protocol.
- `b.close() → None` — close the buffer (also via a `with` block); part of the stream protocol.

---

## itertools

- `count(start = 0, step = 1, stop = None) → List` — integers from `start` by `step`; supply `stop` since the result is eager. (Parameter order is `start, step, stop` — `step` comes before `stop`, unlike `range(start, stop, step)`.)
- `repeat(value, times) → List` — `value` repeated `times` times.
- `cycle(iterable, times) → List` — the iterable repeated `times` times.
- `chain(lists) → List` — concatenate the iterables in a list-of-iterables (`chain([[1,2],[3,4]])`).
- `islice(iterable, start, stop[, step]) → List` — a slice of an iterable. `start`/`stop` must be non-negative and `step` a positive integer (a negative index or a non-positive `step` raises), matching Python's `islice`.
- `accumulate(iterable[, func]) → List` — running totals (or running `func` reductions).
- `product(lists) → List` — Cartesian product of a list-of-iterables (`product([[1,2],[3,4]])`).
- `permutations(items[, r]) → List` — r-length orderings.
- `combinations(items, r) → List` — r-length combinations.
- `takewhile(pred, iterable) → List` — the leading run of elements while `pred` holds.
- `dropwhile(pred, iterable) → List` — the rest, after that leading run.
- `filterfalse(pred, iterable) → List` — elements where `pred` is falsy.
- `compress(data, selectors) → List` — `data` elements where the matching selector is truthy.
- `starmap(func, argtuples) → List` — call `func` once per tuple, passing the whole tuple as a single
  List argument (Kirito has no `*args` spread): `func(t)` where `t` is `[a, b, …]`.
- `pairwise(iterable) → List` — consecutive overlapping pairs.
- `ziplongest(lists, fillvalue = None) → List` — zip a list-of-iterables, padding short ones with `fillvalue`.
- `groupby(iterable[, key]) → List` — group consecutive elements sharing a key.

---

## json

JSON parsing and serialization (flat data interchange — for reference/cycle-preserving snapshots see
`serialize` / `dump` below). `loads` and `dumps` are aliases of `parse` and `stringify`.

- `parse(text: String)` — parse JSON text into Kirito values (objects → Dict, arrays → List, decodes `\u` escapes and surrogate pairs). Throws a clear error on malformed input. Alias `loads`.
- `stringify(value, indent: Integer = 0) → String` — serialize a value to JSON; compact by default, pretty-printed when the indent width is greater than zero. Alias `dumps`.

As a JavaScript-style extension, non-finite Floats round-trip: `stringify` emits the bare tokens
`NaN` / `Infinity` / `-Infinity`, and `parse` accepts them back. This is convenient within Kirito but
is **not** strict RFC-8259, so such output may be rejected by other JSON readers.

Floats **round-trip exactly**: `loads(dumps(x))` recovers the identical double for any Float `x`
(`dumps` emits enough digits, e.g. `0.1 + 0.2` → `"0.30000000000000004"`).

**Parsing is lenient about number spelling and duplicate keys.** A leading-zero integer (`012` → `12`)
and a trailing-dot float (`1.` → `1.0`) are accepted; a duplicate object key keeps the **last** value
(`{"a":1,"a":2}` → `{'a': 2}`); a lone/unpaired `\u` surrogate decodes to the replacement character
`U+FFFD` rather than throwing; and an integer literal too large for int64 widens to a `Float` (which may
become `Infinity` if it also overflows a double).

**`stringify` only serializes JSON-representable values, and the indent is capped.** A value that has no
JSON form — a `Set`, a function, a class/instance without a JSON mapping, or a structure containing a
**cycle** — throws `cannot serialize '<Type>' to JSON`. The `indent` width has a hard maximum of 100;
a larger value throws `json.stringify: indent too large (maximum 100)`.

---

## math

Constants and the usual numeric functions. Both type **and domain** errors throw a clear `math domain
error` rather than silently returning `nan`/`inf` rubbish — `sqrt(-1)`, `log(0)`, `log(-1)`, `asin(2)`,
`acos(2)`, `acosh(0)`, `atanh(1)`, `log2(0)`, `log10(0)`, `log1p(-1)`, `gamma(0)`/`gamma(-1)`,
`lgamma(0)`, `pow(-2, 0.5)` (negative base, non-integer exponent), `pow(0, -1)` (zero to a negative
power), `fmod(x, 0)`, and a `log` base `≤ 0` or `== 1` all throw. A NaN argument passes through
unchanged (`sqrt(nan) → nan`), and a genuine *range* condition — overflow to infinity such
as `exp(1000) → inf` — is not a domain error and does not throw. Results are `Float` unless noted.

- Constants: `pi`, `e`, `tau`, `inf`, `nan` (all `Float`).
- `sqrt(x: Number) → Float` — square root.
- `cbrt(x: Number) → Float` — cube root.
- `sin(x: Number) → Float` — sine (radians).
- `cos(x: Number) → Float` — cosine (radians).
- `tan(x: Number) → Float` — tangent (radians).
- `asin(x: Number) → Float` — arcsine.
- `acos(x: Number) → Float` — arccosine.
- `atan(x: Number) → Float` — arctangent.
- `sinh(x: Number) → Float` — hyperbolic sine.
- `cosh(x: Number) → Float` — hyperbolic cosine.
- `tanh(x: Number) → Float` — hyperbolic tangent.
- `asinh(x: Number) → Float` — inverse hyperbolic sine.
- `acosh(x: Number) → Float` — inverse hyperbolic cosine.
- `atanh(x: Number) → Float` — inverse hyperbolic tangent.
- `atan2(y: Number, x: Number) → Float` — arctangent of `y/x` using the signs of both for the quadrant.
- `hypot(x: Number, y: Number) → Float` — `sqrt(x² + y²)` without overflow.
- `exp(x: Number) → Float` — `e ** x`.
- `expm1(x: Number) → Float` — `exp(x) - 1`, accurate for small `x`.
- `log1p(x: Number) → Float` — `log(1 + x)`, accurate for small `x`.
- `log2(x: Number) → Float` — base-2 logarithm.
- `log10(x: Number) → Float` — base-10 logarithm.
- `log(x: Number, base = None) → Float` — natural log, or log base `base` when given.
- `pow(x: Number, y: Number) → Float` — `x ** y` as a Float (the builtin `pow` does Integer/modular).
- `gamma(x: Number) → Float` — the gamma function.
- `lgamma(x: Number) → Float` — the natural log of the absolute value of gamma.
- `erf(x: Number) → Float` — the error function.
- `erfc(x: Number) → Float` — the complementary error function (`1 - erf(x)`, accurate for large `x`).
- `floor(x: Number) → Integer` — round down to an Integer.
- `ceil(x: Number) → Integer` — round up to an Integer.
- `trunc(x: Number) → Float` — truncate toward zero.
- `fabs(x: Number) → Float` — absolute value as a Float.
- `copysign(x: Number, y: Number) → Float` — `|x|` with the sign of `y`.
- `fmod(x: Number, y: Number) → Float` — floating-point remainder of `x/y` (the result has the sign of `x`).
- `degrees(x: Number) → Float` — convert radians to degrees.
- `radians(x: Number) → Float` — convert degrees to radians.
- `isnan(x: Number) → Bool` — whether `x` is NaN.
- `isinf(x: Number) → Bool` — whether `x` is infinite.
- `isfinite(x: Number) → Bool` — whether `x` is finite (neither NaN nor infinite).
- `gcd(a: Integer, b: Integer) → Integer` — greatest common divisor.
- `lcm(a: Integer, b: Integer) → Integer` — least common multiple.
- `factorial(n: Integer) → Integer` — `n!` (throws on negatives / Integer overflow).
- `comb(n: Integer, k: Integer) → Integer` — combinations “n choose k”.
- `perm(n: Integer, k = None) → Integer` — permutations of `n` taken `k` at a time; with `k` omitted
  (or `None`) it returns `n!`.
- `prod(iterable, start = 1) → Number` — product of the elements times `start` (Integer if all Integer,
  else Float). Like `factorial`/`comb`/`perm`/`lcm`, an all-Integer product throws on Integer overflow
  rather than silently wrapping; a Float anywhere in the mix makes the result a Float (no overflow).

---

## matrix

Dense real matrices — a 2-D `tensor` of doubles, with the familiar matrix API and the
`*`-means-matrix-multiply convention. For complex-valued numbers and matrices, see the `complex`
module below; for arbitrary-rank arrays, see `tensor`.

- `Matrix(rows: List) → Matrix` — build from a nested list of numbers (rows must be equal length).
- `Matrix(rows: Integer, cols: Integer) → Matrix` — a zero matrix of the given shape.
- `zeros(rows: Integer, cols: Integer) → Matrix` — a matrix filled with `0`.
- `ones(rows: Integer, cols: Integer) → Matrix` — a matrix filled with `1`.
- `identity(n: Integer) → Matrix` — the n×n identity.
- `vector(values: List) → Matrix` — a 1×n row vector from a flat list of numbers.

Matrices are arbitrary-shape (any rows × cols). Shape-specific operations (`determinant`, `inverse`,
`trace`) require a square matrix and throw otherwise; `*` requires conformable inner dimensions.

### Matrix object

- `m[i, j] → Float` — element access.
- `m[i] → List` — the whole row `i` as a List of Floats.
- `m[i, j] = v` — element assignment.
- `m1 == m2 → Bool` — **exact** equality (same shape, every element bit-equal; `NaN` never equal). For
  computed matrices use the tolerant `.compare` below.
- `m.compare(other, rel_tol = 1e-9, abs_tol = 0.0) → Bool` — tolerant comparison (relative/absolute tolerance per
  element). When the target has **exact zeros** (e.g. an identity's off-diagonals) pass an `abs_tol`,
  since `rel_tol` alone can't match a near-zero element: `(A * A.inverse()).compare(matrix.identity(2), abs_tol = 1e-9)`.
- `m.get(i, j) → Float` — explicit element access (the method form of `m[i, j]`).
- `m.set(i, j, v) → None` — explicit element assignment (the method form of `m[i, j] = v`).
- `m.rows() → Integer` — the number of rows.
- `m.cols() → Integer` — the number of columns.
- `m.shape() → List` — `[rows, cols]`.
- `m.row(i) → List` — the `i`-th row as a List of its elements.
- `m + n`, `m - n`, `m * n` — matrix addition/subtraction, and matrix or scalar multiplication. A
  scalar must be the **right** operand (`A * 2`, not `2 * A`).
- `m.transpose() → Matrix` — the transpose.
- `m.determinant() → Float` — determinant (square matrices). A matrix whose elimination produces a
  pivot below ~`1e-15` is treated as singular and the determinant is reported as `0.0` (a conservative
  guard against an ill-conditioned, near-garbage value).
- `m.inverse() → Matrix` — inverse. **Throws** `"singular"` if the matrix is singular (pivot below the
  threshold above) — unlike `determinant`, which returns `0.0`.
- `m.trace() → Float` — sum of the diagonal.
- `m.sum() → Float` — sum of every element.
- `m.apply(fn) → Matrix` — a new matrix with `fn` applied to each element.

**Vector operations** (a Matrix with one dimension equal to 1 is a vector):

- `u.dot(v) → Float` — the scalar (dot) product of two equal-length vectors (any orientation).
  (`*` is always matrix multiply, never a dot product.)
- `u.cross(v) → Matrix` — the cross product of two 3-element vectors (result keeps `u`'s orientation).
- `m.norm() → Float` — the Euclidean (Frobenius) 2-norm `sqrt(Σ xᵢ²)` — the length of a vector.

---

## net

A complete socket foundation (TCP **and** UDP, over IPv4 and IPv6), a full-fledged HTTP/1.1 client,
and URL helpers. `net.tlsenabled` (`Bool`) reports whether this build was compiled with HTTPS support
(`-DKIRITO_ENABLE_TLS`) — the `debug` and `release` build presets enable it. (Deliberately out of
scope, by design: asyncio/event loops and a built-in HTTP server — servers are built directly on the
sockets below; see the `webserver` example.)

### HTTP client

- `request(method: String, url: String, options: Dict = None) → Response` — perform any HTTP request.
- `get(url: String, options: Dict = None) → Response` — a `GET` request.
- `post(url: String, options: Dict = None) → Response` — a `POST` request.
- `put(url: String, options: Dict = None) → Response` — a `PUT` request.
- `delete(url: String, options: Dict = None) → Response` — a `DELETE` request.
- `patch(url: String, options: Dict = None) → Response` — a `PATCH` request.
- `head(url: String, options: Dict = None) → Response` — a `HEAD` request.
- `options(url: String, options: Dict = None) → Response` — an `OPTIONS` request.
- `Session() → Session` — a session that persists a cookie jar (`.cookies`) and default headers
  (`.headers`) across requests; has the same `request(method, url[, options])` and verb methods
  (`s.get(url[, options])`, …).
  **Cookies are scoped to the host that set them**: a cookie a server sends back is remembered with
  that server's hostname and is sent only to that hostname, so a Session that logs in to one site
  never hands its session cookie to another. (Scoping is by hostname, not port — matching the cookie
  RFC and the redirect rule below. The jar is keyed by cookie *name*, so if two hosts set the same
  name, the last one wins and the cookie follows that host.) A cookie **you** put in the jar yourself
  (`s.cookies["k"] = "v"`) has no origin and is sent to every host you ask — it is your explicit
  instruction. `.headers` are likewise sent to every host, so treat a default `Authorization` header
  as "for anyone this Session talks to" and pass a per-call `headers` option instead when it is meant
  for one origin only.

The `options` Dict may contain: `headers` (Dict), `params` (Dict → query string), `data` (String, a
form-Dict, or `Bytes` → sent as `application/octet-stream`), `json` (any value → JSON body +
`application/json`), `files` (Dict → `multipart/form-data`
upload; value is content or `[filename, content]`), `auth` (`[user, pass]` → HTTP Basic), `timeout`
(seconds), `allowredirects` (Bool, default `True`) / `maxredirects` (Integer, default 10), `verify`
(Bool, default `True` — TLS certificate verification; trust roots come from the OS — OpenSSL's default
paths or the `SSL_CERT_FILE` env var on Unix, the Windows system certificate store on Windows — and a
verify failure reports the specific reason; pass `verify = False` to skip), and `cookies` (Dict). Redirects are followed
(the `Authorization` header and cookies are **stripped when a redirect crosses to a different origin**,
matching `requests` — so credentials never leak to a third-party host; same-origin redirects keep them),
chunked transfer-encoding is decoded, and `gzip`/`deflate` responses are decompressed automatically.

### Response object

- `r.status` (`Integer`, alias `r.statuscode`), `r.reason` (`String`), `r.ok` (`Bool`, true for a 1xx–3xx status, i.e. `100 ≤ status < 400`).
- `r.url` — the final URL (after any redirects).
- `r.text` — the decoded response body (`String`); `r.body` is an alias.
- `r.content` — the response body as [`Bytes`](types.html#bytes), after any HTTP `Content-Encoding`/
  `Transfer-Encoding` is decoded (gzip/deflate/chunked).
  For a genuine binary download — an image, or a `.gz` file served *without* `Content-Encoding: gzip` —
  this is the raw bytes, so `gzip.decompress(net.get(url).content)` unpacks a fetched `.gz`. (If the
  server sets `Content-Encoding: gzip`, the body is already decompressed here.)
- `r.headers` — a Dict of response headers; `r.header(name[, default])` looks one up
  **case-insensitively** (returning `default`, or `None`, when absent).
- `r.cookies` — a Dict of cookies set by the server.
- `r.json()` — parse the body as JSON.
- `r.raiseforstatus()` — throw if the status is ≥ 400, else return the response.

### URL helpers (`urllib.parse` style)

- `quote(s: String) → String` — percent-encode a String (UTF-8); a space becomes `%20` and a
  literal `+` becomes `%2B`.
- `unquote(s: String) → String` — percent-decode a String (UTF-8). Like `urllib.parse.unquote_plus`,
  a `+` decodes to a space (so it round-trips query strings; `quote` never emits a bare `+`).
- `urlencode(params: Dict) → String` — build a `k=v&...` query string (keys and values encoded).
- `parseqs(query: String) → Dict` — parse `k=v&...` into a Dict (keys and values decoded). Duplicate
  keys are last-wins and a pair with an empty key is dropped.
- `urlsplit(url: String) → Dict` — split a URL into `scheme`/`host`/`port`/`path`/`query`/`fragment`
  (all `String`; `port` is the textual digits, empty when absent — use `Integer(d["port"])` if you need
  it numeric). A bracketed IPv6 literal is preserved in `host` with its brackets, and the optional
  port follows after `]:` — `urlsplit("http://[::1]:8080/p")` -> `host = "[::1]", port = "8080"`.

### Sockets

**Constructors.**

- `Socket() → Socket` — a new TCP/IPv4 socket (the historical default).
- `socket(family: String = "inet", type: String = "stream") → Socket` — the general constructor;
  `family` is `"inet"` (IPv4) or `"inet6"` (IPv6), `type` is `"stream"` (TCP) or `"dgram"` (UDP).
- `tcpsocket(family: String = "inet") → Socket` / `udpsocket(family: String = "inet") → Socket` —
  shorthands for the stream / datagram cases.
- `socketpair(type: String = "stream") → [Socket, Socket]` — a connected, share-nothing pair of
  sockets (native `AF_UNIX` on POSIX; a loopback stream pair on Windows, where a datagram pair is
  unsupported).
- `fromfd(fd: Integer, family: String = "inet", type: String = "stream") → Socket` — adopt an existing
  raw socket file descriptor (e.g. one handed over by `s.detach()`). Valid only within the same OS
  process — the basis for handing an accepted connection to a worker VM under `parallel` (see
  [`parallel`](#parallel)).

The read-only attributes `s.family` (`"inet"`/`"inet6"`) and `s.type` (`"stream"`/`"dgram"`) report
how the socket was created.

**Connection / stream I/O.**

- `s.connect(host: String, port: Integer) → None` — connect to a peer. On a UDP socket this sets the
  default peer, so `send`/`recv` then work without an address.
- `s.bind(host: String, port: Integer) → None` — bind (sets `SO_REUSEADDR`); an empty host binds all
  interfaces. Resolves the host in the socket's family (IPv4 or IPv6).
- `s.listen([backlog: Integer]) → None` / `s.accept() → Socket` — server side (TCP); the accepted
  socket inherits the listener's family/type.
- `s.send(data: String | Bytes) → Integer` — send all of `data` (text or binary); returns the byte count.
- `s.recv([n: Integer]) → Bytes` — receive up to `n` bytes (default 4096). A socket carries raw bytes,
  so this returns `Bytes`; for a text protocol call `.decode()` on the result
  (e.g. `s.recv(4096).decode("utf-8")`).
- `s.recvall() → Bytes` — receive until the peer closes (raw `Bytes`; `.decode()` for text).

**Datagram (UDP) I/O.**

- `s.sendto(data: String | Bytes, host: String, port: Integer) → Integer` — send one datagram to an
  address; returns the byte count.
- `s.recvfrom([n: Integer]) → [Bytes, [host, port]]` — receive one datagram (default buffer 65536)
  together with the sender's `[host, port]`.

**Address, options, lifecycle.**

- `s.getsockname() → [host, port]` / `s.getpeername() → [host, port]` — the local / remote address.
- `s.shutdown([how: String]) → None` — half-close one direction: `"read"`, `"write"`, or `"both"`
  (default `"both"`).
- `s.setsockopt(option: String, value: Integer) → None` / `s.getsockopt(option: String) → Integer` —
  string-keyed socket options (no raw `SO_*` integers). Settable: `reuseaddr`, `broadcast`,
  `keepalive`, `rcvbuf`, `sndbuf`, `nodelay` (and `reuseport` where the OS provides it); `getsockopt`
  additionally reads `error`, `type`, `acceptconn`.
- `s.setreuseaddr(flag)` / `s.setnodelay(flag)` / `s.setbroadcast(flag)` / `s.setkeepalive(flag)` —
  named conveniences for the common toggles.
- `s.settimeout(seconds) → None` — bound send/recv **and a subsequent `connect()`** with a timeout
  (a non-blocking connect + `select` under the hood), so a connect to a black-hole host can't hang
  past the timeout.
- `s.setblocking(flag: Bool) → None` — switch between blocking and non-blocking mode.
- `s.starttls(server_hostname = None, verify = True) → None` — **upgrade a connected stream socket to
  TLS** (needs a `net.tlsenabled` build). Works for both **STARTTLS** — do the plaintext protocol
  handshake first (SMTP `EHLO`/`STARTTLS`, IMAP, FTP), then call `starttls` to encrypt the *same*
  connection — and **implicit TLS** (SMTPS/IMAPS/HTTPS: `connect` then `starttls` immediately). After
  it returns, `send`/`recv`/`recvall` are transparently encrypted. `server_hostname` drives SNI and
  certificate hostname verification, defaulting to the host you `connect`ed to; `verify` (default
  `True`) checks the peer certificate against the trust store (`SSL_CERT_FILE` / the OS store), exactly
  like the HTTPS client — pass `verify = False` for a self-signed peer. Throws on a UDP socket, a closed
  socket, an already-TLS socket, a failed/mismatched handshake, or (in a non-TLS build) a clear
  "requires KIRITO_ENABLE_TLS" error — it never silently falls back to plaintext.
- `s.cipher() → String` — the negotiated cipher suite name once TLS is active, else `None`.
- `s.is_tls → Bool` — whether a TLS session is currently active on this socket.
- `s.fileno() → Integer` — the raw fd, **non-destructively** (unlike `detach`); returns `-1` on a
  closed or detached socket (its fd is gone or recycled, so it must not be adopted). `detach` is
  refused on a TLS socket (the TLS session owns the fd).
- `s.close() → None` — close the socket.
- `s.detach() → Integer` — surrender the raw fd to the caller and stop owning it (the socket's
  destructor will no longer close it), then clear it — a second `detach()` throws rather than handing
  out the same fd twice. Pair with `net.fromfd` to hand a connection to a worker VM.

Every entry point validates its input — an unknown `family`/`type`/`option`, a port outside 0–65535,
or a negative size throws a clear error, and any call on a closed (or detached) socket throws
`"… : socket is closed"` rather than a raw OS error.

### Name resolution

- `gethostname() → String` — the local hostname.
- `gethostbyname(host: String) → String` — resolve a name to its first IPv4 address.
- `getaddrinfo(host: String[, port[, family[, type]]]) → List` — resolve to a List of
  `{family, type, host, port}` dicts. `port` may be an Integer or a service-name String; `family`
  (`"inet"`/`"inet6"`) and `type` (`"stream"`/`"dgram"`) filter the results when given.

A Socket is also usable as a `with` context manager — it is closed automatically on block exit
(`with net.Socket() as s: ...`). A `Response` additionally supports Dict-style indexing as a
convenience: `r["status"]`, `r["body"]`, `r["headers"]`, etc. map to the same fields as the attributes.

---

## parallel

True parallelism by **multiprocessing**: many fully-isolated `KiritoVM`s, each on its own OS thread,
that share **nothing** and communicate only by passing serialized values through thread-safe
primitives. (Kirito values live in a per-VM, unsynchronized arena, so they can't be shared across
threads directly — instead a value is serialized out of one VM and rebuilt in another, exactly like
`dump`.) This module is provided by the `ki` interpreter, which runs every VM under a coordinator (a
`KiritoDispatcher`); a bare embedded `KiritoVM` has no `parallel` module.

- `cpucount() → Integer` — the number of hardware threads available (at least 1).
- `spawn(fn, *args, **kwargs) → Task` — run `fn(*args, **kwargs)` in a **fresh worker VM** on another
  thread. `fn` must be a Kirito function defined in a **loadable `.ki` file** (the worker re-reads that
  file and locates `fn` by source position); `args`/`kwargs` and the eventual return value must be
  **serializable** (the same value types `dump` supports, including class instances with
  `_getstate_`/`_setstate_`). A non-serializable **argument** is caught synchronously at `spawn` (it
  throws there); a non-serializable **return value** surfaces later, at `join`.

> **Share-nothing rule.** A worker sees its parameters, the defining file's **module-level** names, and
> its `import`s — but **not** local variables captured from an enclosing function (the closure does not
> cross). Keep side-effecting startup under `if argmain:` so a worker, which re-evaluates the file with
> `argmain` False, only defines functions instead of re-running the program.

### Task

- `t.join() → value` — block until the worker finishes and return its result (rebuilt in the caller's
  VM). If the worker threw, `join` re-throws it here. `join` is **idempotent**: a second call returns
  the same value (or re-throws the same error) from the cached result. Enforced type annotations on
  the spawned function are checked inside the worker, so an annotation violation surfaces here at `join`.
- `t.done() → Bool` — whether the worker has finished (non-blocking). True once the worker has
  finished **whether it returned or threw** — pair it with `join` to retrieve the value or error.

### Queue

The central transfer primitive: a thread-safe FIFO that carries serialized values between VMs. Passing
a Queue into `spawn` (or through another Queue) references the **same** underlying queue.

- `Queue(maxsize: Integer = 0) → Queue` — a new queue; `maxsize = 0` is unbounded, otherwise bounded
  (a full `put` blocks for back-pressure). A negative `maxsize` throws.
- `q.put(item, block: Bool = True, timeout = None) → None` — enqueue `item` (serialized). On a full
  bounded queue: blocks, or throws if `block = False` / the `timeout` elapses.
- `q.get(block: Bool = True, timeout = None) → value` — dequeue the next value. On an empty queue:
  blocks, or throws if `block = False` / the `timeout` elapses.
- `q.putnowait(item)` / `q.getnowait()` — non-blocking `put` / `get`.
- `q.qsize() → Integer`, `q.empty() → Bool`, `q.full() → Bool`.
- `q.close() → None` — mark the queue closed. Pending and subsequent `put`s throw; `get` drains the
  remaining items and then throws — the idiom a consumer loop uses to stop (here `q` is a `Queue` and
  `handle` a function from the surrounding program):

<!--norun (consumer-loop idiom fragment; q/handle come from the surrounding program)-->
```kirito
var running = True
while running:
    try:
        handle(q.get())
    catch as e:        # thrown when the queue is closed and drained
        running = False
```

### Lock, Event, Semaphore, Barrier

Coordination primitives, all cross-VM by identity (pass them into `spawn` / through a Queue) and all
woken by interpreter shutdown.

- `Lock() → Lock` — a non-reentrant mutex. `l.acquire(block = True, timeout = None) → Bool` (True if
  acquired, False on timeout), `l.release()`, `l.locked() → Bool`. Best used as a context manager,
  which always releases: `with l: ...`. Non-reentrant means a worker that already holds the lock and
  acquires it again **throws** (rather than self-deadlocking); releasing an unheld lock also throws.
- `Event() → Event` — a resettable flag. `e.set()`, `e.clear()`, `e.isset() → Bool`,
  `e.wait(timeout = None) → Bool` (True once set, False on timeout).
- `Semaphore(value: Integer = 1) → Semaphore` — a permit counter for bounded concurrency (an initial
  `value` below 0 throws). `s.acquire(block = True, timeout = None) → Bool`, `s.release()`; also a
  context manager (`with s:`).
- `Barrier(parties: Integer) → Barrier` — an N-party rendezvous (`parties` must be at least 1, else it
  throws). `b.wait(timeout = None) → Integer`
  (returns the arrival index 0..parties-1; the last arrival releases all), `b.parties() → Integer`,
  `b.nwaiting() → Integer`, `b.reset()`, `b.abort()`. Unlike the others, `b.wait` on **timeout throws**
  (it breaks the barrier) rather than returning a sentinel. `reset()` breaks only the currently-waiting
  parties and keeps the barrier reusable; `abort()` is **permanent** — every later `wait` throws.

These primitives cross VM boundaries **by identity** (passing one into `spawn`, or returning one from a
worker, shares the same underlying object). They define no *value* `==`/hash: equality is **identity**
(a handle compares `==` to itself, but two separately-obtained wrappers of the same cross-VM object do
not), and a primitive is **unhashable**, so it cannot be a Dict/Set key.

### Avoiding deadlock

The runtime is deadlock-safe by construction: every blocking call honors a `timeout`, and interpreter
shutdown aborts every blocked primitive (a blocked op then throws "operation aborted"). For
application-level safety:

- prefer `with lock:` / `with sem:` so a primitive is always released, even on an exception;
- pass a `timeout =` to any blocking call that might never be satisfied;
- if a worker acquires several `Lock`s, acquire them in a consistent order across all workers;
- `close()` a Queue when production ends so consumers stop instead of blocking forever.

---

## path

Kirito's `os.path` **and** `os` filesystem surface: the **single home for path and filesystem
operations**, so you never have to remember whether a helper lives in `io` or `sys`. It covers pure
path-string manipulation, read-only filesystem queries (including the system temp directory and the
running interpreter's own path), *and* filesystem mutation (create / delete / rename / chmod / list).
[`io`](#io) keeps only actual I/O (open/print/read/write, streams, `BytesIO`).

Path strings use `/` on **every platform** (results are identical cross-platform, unlike the native
`\` on Windows). The splitting helpers still accept a `\` as a separator, so native Windows paths
(from `path.getcwd`/`path.listdir`) split correctly.

```kirito
var path = import("path")
var io = import("io")
var full = path.join(path.getcwd(), "data", "report.csv")   # os.path.join semantics
if path.exists(full) and path.isfile(full):
    io.print(path.basename(full))                          # => report.csv
    io.print(path.splitext(full)[1])                       # => .csv
```

### Path strings

- `join(*parts) → String` — join path components with `/`, exactly like `os.path.join`. A later
  component that is **absolute** (starts with `/`) resets the result; a separator is inserted between
  components only when the running result doesn't already end in one — so an empty component still
  emits a separator (`join("a", "")` → `"a/"`, `join("a", "", "b")` → `"a/b"`). It needs at least one
  component — `join()` with no parts **throws**. A leading `\` is **not** treated as absolute (only
  `/` resets).
- `dirname(path: String) → String` — the directory part of `path` (the root is kept: `dirname("/a")`
  is `"/"`).
- `basename(path: String) → String` — the final component of `path` (empty for a trailing-slash path).
- `splitext(path: String) → List` — `[root, ext]`, splitting off the **last** extension. A leading run
  of dots is protected, so `.bashrc`/`..`/`...x` have no extension (matching `os.path.splitext`).
- `dirname`/`basename`/`splitext` split on either `/` or `\` and return literal substrings of the
  input — they do not rewrite separators, so only `basename` (the final component) is guaranteed free
  of a backslash; a `\` inside a retained prefix is preserved. A non-String argument **throws**.

### Filesystem queries

- `exists(path: String) → Bool` — whether `path` exists (tolerant: a missing/inaccessible path is
  simply `False`, never a throw).
- `isfile(path: String) → Bool` — whether `path` is a regular file (tolerant).
- `isdir(path: String) → Bool` — whether `path` is a directory (tolerant).
- `getsize(path: String) → Integer` — the file size in bytes. Unlike the tolerant predicates,
  `getsize` **throws** on a missing or non-regular path (there is no sensible size to return).
- `listdir(path: String) → List` — the entry names directly under `path` (tolerant: a missing dir
  lists as `[]`).
- `walk(dir: String) → List` — every file path under `dir`, recursively (flattened; tolerant).
- `getcwd() → String` — the current working directory.
- `gettempdir() → String` — the system temp directory (honors `TMPDIR`/`TMP`/`TEMP`, falls back to
  `/tmp`) — a stable scratch location to build temp file paths with `path.join`.
- `fasttemp() → String` — the **fastest available** scratch directory: a RAM-backed tmpfs where the
  OS publishes one (Linux `/dev/shm`), otherwise `gettempdir()`. Best-effort and portable — you get
  memory-speed temp I/O (handy for large intermediate files, e.g. staging data for a subprocess)
  without leaving the process, and it degrades transparently on macOS/Windows, which expose no public
  RAM disk. Use it exactly like `gettempdir()`: `path.join(path.fasttemp(), name)`.
- `executable` — the absolute path of the running `ki` interpreter binary (a `String`, `""` if it
  can't be determined). A filesystem location, so it lives here in `path`; used e.g. by `kpm` to
  locate the binary it self-replaces.

### Filesystem mutation

The mutating ops are **strict by default** — they throw rather than silently no-op — with opt-in
leniency. `mkdir`/`remove`/`rmtree` return a `Bool` (`True` = it did the work, `False` = the opt-in
lenient no-op); `rename` returns `None`; `chmod` returns a `Bool` success flag.

- `mkdir(path: String, exist_ok = False) → Bool` — create the directory (and any missing parents).
  Returns `True` when it creates it; **throws** if `path` already exists. Pass `exist_ok = True` to
  make an existing directory a no-op (returns `False`, nothing created).
- `remove(path: String, missing_ok = False) → Bool` — delete a file (or an *empty* directory).
  Returns `True` when it removes something; **throws** if `path` does not exist. Pass
  `missing_ok = True` to make a missing path a no-op (returns `False`). A non-empty directory throws —
  use `rmtree`.
- `rmtree(path: String, missing_ok = False) → Bool` — **recursively** delete a directory and
  everything under it (or a single file) — the recursive counterpart to `remove` (`rm -rf` /
  `shutil.rmtree`). Same strict/`missing_ok` contract as `remove`.
- `rename(src: String, dst: String) → None` — rename/move a path (throws on failure).
- `chmod(path: String, mode: Integer) → Bool` — set permission bits from a POSIX-style octal (e.g.
  `0o755`); lenient (a missing file returns `False`, no throw). On Windows only the owner read/write
  bits are meaningful.

```kirito
var path = import("path")
var io = import("io")
var d = path.join(path.getcwd(), "scratch")
path.mkdir(d, exist_ok = True)              # idempotent setup
with io.open(path.join(d, "note.txt"), "w") as f:
    f.write("hi")
io.print(path.getsize(path.join(d, "note.txt")))   # => 2
path.rmtree(d)                            # recursively remove the whole tree
```

---

## random

Object-based RNG — no global state; create a generator and call methods on it.

- `Random(seed: Integer = None, generator: String = "xoshiro") → Random` — a new generator. With
  no seed (or `None`) it is seeded from the OS; with a seed it is reproducible. `generator` selects
  the underlying engine — `"xoshiro"` (the default, xoshiro256++: 256-bit state, ~1.5–1.75× faster
  than Mersenne Twister on raw `next()` and every `<random>` distribution) or `"mersenne_twister"`
  (`std::mt19937_64`: 19937-bit state, longer period, kept for anyone who wants the historical
  engine or has an existing on-disk checkpoint pinned to it). The underlying engine names
  `"xoshiro256"` / `"mt19937_64"` also work. An unknown generator throws.

### Random object

- `r.generator → String` — the engine name (`"xoshiro"` or `"mersenne_twister"`), read-only.
- `r.seed(a: Integer) → None` — reseed the current engine (does not switch generator kind).
- `r.random() → Float` — uniform in `[0.0, 1.0)`.
- `r.uniform(a, b) → Float` — uniform in `[a, b)` (the upper bound `b` is excluded).
- `r.randint(a, b) → Integer` — uniform integer in `[a, b]` (inclusive).
- `r.randrange(stop)` / `r.randrange(start, stop[, step]) → Integer` — like `range`, a random member.
- `r.choice(seq)` — a random element of a non-empty sequence (a single scalar).
- `r.choices(population, k = 1) → List` — a List of `k` elements sampled **with replacement** (elements
  can repeat), so `k` may exceed `len(population)`. `choice(seq)` is the `k = 1` case of this same draw,
  unwrapped to the single element. `k = 0` gives `[]`; a negative or enormous `k`, or an empty
  population, throws. (Contrast `sample`, which is **without** replacement.)
- `r.shuffle(seq) → None` — shuffle a List in place.
- `r.sample(population, k) → List` — `k` distinct elements chosen at random (without replacement).
- `r.gauss(mu, sigma) → Float` — a sample from a normal distribution (alias `normalvariate`). Both
  arguments default (`mu = 0.0`, `sigma = 1.0`) and take keywords, so `r.gauss(sigma = 2.0)` works; a
  negative `sigma` throws.
- `r.expovariate(lambd) → Float` — exponential distribution.

### Secure random (OS CSPRNG)

Module-level functions that draw from the operating system's cryptographic RNG (getrandom /
BCryptGenRandom / `/dev/urandom`). Unlike a seeded `Random`, these are **unpredictable** — use them
for tokens, keys, salts and nonces, not a seeded generator's stream.

- `randombytes(n = 32) → Bytes` — `n` cryptographically secure random bytes.
- `randomhex(n = 32) → String` — `n` random bytes as `2n` lowercase hex chars.
- `randomurlsafe(n = 32) → String` — `n` random bytes as URL-safe base64 (alphabet `A–Za–z0–9-_`,
  no padding) — a compact, URL-safe token.
- `randombelow(n) → Integer` — a uniform, **bias-free** integer in `[0, n)` (rejection sampling), for a
  secure random index or dice roll (`n` must be positive).
- `hasentropy() → Bool` — whether the OS cryptographic RNG is usable right now. The functions
  above (and [`int`](#int)'s `isprobableprime`/`randomprime`) throw if it isn't — probe this first
  if you need to degrade gracefully rather than catch the error.

```kirito
var random = import("random")
var session = random.randomhex(16)         # a 32-char hex session id
assert len(session) == 32
var pick = random.randombelow(6)             # a fair die: 0..5
assert pick >= 0 and pick < 6
```

---

## regex

Regular expressions with a **guaranteed linear-time** match. The engine compiles the pattern to a
bytecode program and simulates a Thompson NFA (Pike's algorithm, tracking capture positions), so
matching is O(text × pattern) with **no catastrophic backtracking** — a pattern like `(a+)+b` against
a long input is instant, not exponential. The cost of that guarantee (the same trade-off RE2 makes)
is that two backtracking-only constructs are deliberately **not supported** and throw a clear error:
**backreferences** (`\1`) and **lookaround** (`(?=…)`, `(?!…)`, `(?<=…)`, `(?<!…)`).

All positions and spans are **code-point indices** (consistent with
Kirito's String indexing). Flags combine with `+`: `re.IGNORECASE` (alias `re.I`), `re.MULTILINE`
(`re.M`), `re.DOTALL` (`re.S`).

### Supported syntax

Literals and `.` (any char except newline; `\n` too under DOTALL); character classes `[...]`,
`[^...]`, ranges `a-z`; shorthands `\d \D \w \W \s \S` (ASCII); anchors `^ $`, `\b \B`, `\A \z \Z`;
groups `(...)`, non-capturing `(?:...)`, named `(?P<name>...)` or `(?<name>...)`; alternation `|`;
quantifiers `* + ?`, `{n}`, `{n,}`, `{n,m}`, each greedy or **lazy** with a trailing `?`; escapes
`\n \t \r \f \v \a \xHH \uHHHH \UHHHHHHHH` (2-, 4-, and 8-hex-digit code points), octal `\0NN` (a leading-zero octal escape; `\b` is a backspace
*inside* a class), and any escaped metacharacter; inline flags `(?i)` / `(?m)` / `(?s)`. A bare
`\1`–`\9` is **rejected** (it reads as a backreference, which is unsupported) — write an octal
character as `\0NN`.

### One divergence from Python: a repeated group that can match empty

When a capturing group can match the empty string *and* is itself repeated (`(a*)*`, `(a*){0,}`),
Kirito's value for that **group** differs from Python's — `search("(a*)*", "")` gives `[None]` where
Python gives `('',)`, and `fullmatch("(a*)*", "aaa")` gives `['aaa']` (the iteration that consumed the
text) where Python gives `('',)` (a final empty iteration at the end).

**The match itself is always correct** — `group(0)`, whether the pattern matches, and every span are
right; only the captured value of such a group differs. This falls out of simulating all alternatives
at once instead of backtracking: threads that reach the same position are merged, so "looped again and
matched empty" cannot be kept apart from "did not loop again". Go's `regexp` and RE2 diverge from
Perl-family engines here for the same reason, and it is part of the price of the linear-time
guarantee. In practice, write `(a*)` or `(a+)*` when the captured value matters — a repeated nullable
group is almost always an accident anyway.

The engine is validated against a large, classic regular-expression test corpus (run through
Kirito in `tools/tests/scripts/spec_regex_corpus.ki`): zero false positives/negatives, and every
unsupported-feature or invalid pattern is rejected with a clean error rather than crashing.

### Module functions

- `compile(pattern: String, flags: Integer = 0) → Regex` — compile once, reuse many times.
- `match(pattern: String, string: String, flags: Integer = 0)` — match anchored at the start, or `None`.
- `search(pattern: String, string: String, flags: Integer = 0)` — first match anywhere, or `None`.
- `fullmatch(pattern: String, string: String, flags: Integer = 0)` — match that covers the whole string, or `None`.
- `findall(pattern: String, string: String, flags: Integer = 0) → List` — all matches (see Regex.findall for the shape).
- `finditer(pattern: String, string: String, flags: Integer = 0) → List` — a List of `Match` objects.
- `sub(pattern: String, repl, string: String, count: Integer = 0, flags: Integer = 0) → String` — substitute matches.
- `split(pattern: String, string: String, maxsplit: Integer = 0, flags: Integer = 0) → List` — split on matches.
- `escape(s: String) → String` — backslash-escape regex metacharacters so `s` matches literally.

### Regex object

Returned by `compile`. The search methods take the subject `string` plus an optional start `pos`
(default 0) and end `endpos` (default the string length); `endpos` makes the subject look exactly
that many code points long, so `$`/`\b` anchor there. Attributes: `r.pattern`, `r.groups` (group
count), `r.groupindex` (name → group number).

- `r.match(string[, pos[, endpos]]) → Match` — anchored match at `pos`, or `None`.
- `r.search(string[, pos[, endpos]]) → Match` — first match at/after `pos`, or `None`.
- `r.fullmatch(string[, pos[, endpos]]) → Match` — whole-(sub)string match, or `None`.
- `r.findall(string[, pos[, endpos]]) → List` — with **0** groups: a List of the matched Strings; with **1** group: a List of that group's Strings; with **2+** groups: a List of per-match group Lists. A group that did not participate in a match renders as the empty String `""` here (unlike `Match.group(n)`/`Match.groups()`, which give `None` for the same absent group).
- `r.finditer(string[, pos[, endpos]]) → List` — a List of `Match` objects, one per non-overlapping match.
- `r.sub(repl, string[, count]) → String` — replace matches. `repl` is either a template String (`\1`, `\g<name>`, `\g<0>`, `\\`) or a **function** taking a `Match` and returning a String. `count = 0` replaces all.
- `r.split(string[, maxsplit]) → List` — split around matches; any captured groups are interleaved into the result.
- `r.pattern` — the source pattern String.
- `r.groups` — the number of capturing groups.
- `r.groupindex` — a Dict mapping each named group to its number.

### Match object

Returned by a successful `match`/`search`/`fullmatch` (and by `finditer`):

- `m.group([key]) → String` — the whole match (no arg or `0`), or group `key` (a number or a name); `None` if that group didn't participate. Several keys return a List.
- `m.groups([default]) → List` — all capturing groups (1..n); non-participating groups are `default` (default `None`).
- `m.groupdict([default]) → Dict` — named groups by name.
- `m.start([key]) → Integer` / `m.end([key]) → Integer` — code-point start/end of the whole match or a group (`-1` if absent).
- `m.span([key]) → List` — `[start, end]`.
- `m.string` — the subject the match was found in.

```kirito
var io = import("io")
var re = import("regex")
var m = re.search("(?P<user>\\w+)@(?P<host>[\\w.]+)", "contact ada@kirito.dev now")
io.print(m.group())            # => ada@kirito.dev
io.print(m.group("user"))      # => ada
io.print(m.groupdict())        # => {'user': 'ada', 'host': 'kirito.dev'}  (order may vary)

io.print(re.findall("\\d+", "12 and 345"))               # => ['12', '345']
io.print(re.sub("\\s+", "_", "a   b  c"))                 # => a_b_c
var rx = re.compile("cat|dog", re.IGNORECASE)
io.print(rx.findall("Cat dog CAT"))                        # => ['Cat', 'dog', 'CAT']
```

---

## semver

Semantic versioning — parse, compare, and range-match version strings, following [semver.org](https://semver.org)
precedence and the [node-semver](https://github.com/npm/node-semver) range grammar. This is the
versioning core `kpm` uses to resolve `owner/repo@<constraint>` against a repository's git tags (see
[Packages & kpm](packages.html)). A version is `MAJOR.MINOR.PATCH` with an optional `-prerelease`
and `+build` (a leading `v`/`=` is tolerated, e.g. `v1.2.3`).

- `clean(s: String) → String` — strip a leading `v`/`=` and surrounding whitespace.
- `parse(s: String) → Dict` — `{major, minor, patch, prerelease, build, raw}` (`prerelease`/`build`
  are Lists of dot-separated identifier Strings). Throws on an invalid version.
- `valid(s: String) → String` — the cleaned version string if valid, else `None`.
- `major(s) / minor(s) / patch(s) → Integer` — a single component.
- `prerelease(s) → List` — the prerelease identifiers, or `None` if there are none.
- `compare(a, b) → Integer` — `-1` / `0` / `1` by precedence (build metadata is ignored; a
  prerelease sorts **before** its release; numeric prerelease identifiers sort before alphanumeric).
- `eq / neq / lt / lte / gt / gte(a, b) → Bool` — comparison shortcuts.
- `diff(a, b) → String` — the kind of change: `"major"` / `"minor"` / `"patch"` / `"prerelease"`,
  or `None` if equal.
- `inc(s, release: String) → String` — bump by `"major"` / `"minor"` / `"patch"` (drops prerelease/build).
- `satisfies(version, rng) → Bool` — does `version` match the range `rng`? Supports caret (`^1.2.3`),
  tilde (`~1.2`), comparators (`>=1.0.0 <2.0.0`), x-ranges (`1.2.x`, `1.x`, `*`), hyphen ranges
  (`1.0.0 - 2.0.0`), AND (space) and OR (`||`). Prereleases are excluded unless a comparator in the
  matched set pins the same `major.minor.patch` (node-semver's default).
- `validrange(rng: String) → Bool` — is `rng` a parseable range? (`kpm` uses this to tell a
  semver constraint from a literal git ref like `main`.)
- `sort(versions: List) / rsort(versions) → List` — sort by precedence, ascending / descending
  (invalid versions are dropped).
- `maxsatisfying(versions, rng) / minsatisfying(versions, rng)` — the highest / lowest version
  in the list that satisfies the range, returned as its **original** string (so a `v`-prefixed tag
  comes back unchanged, usable directly as a git ref), or `None`.

```kirito
var s = import("semver")

s.satisfies("1.4.0", "^1.2.0")          # True  (>=1.2.0 <2.0.0)
s.satisfies("2.0.0", "^1.2.0")          # False
s.maxsatisfying(["v1.0.0", "v1.4.2", "v2.0.0"], "^1.0.0")   # "v1.4.2"
s.sort(["1.10.0", "1.2.0", "1.1.0"])    # ["1.1.0", "1.2.0", "1.10.0"]
s.gt("1.0.10", "1.0.9")                 # True  (numeric, not lexical)
```

---

## serialize

`serialize` and `dump` are **two formats of the same thing** — full object-graph serialization that
preserves shared references and cycles (a full object snapshot, unlike `json` which is flat data
interchange with no aliasing). They share one graph walk and reconstruction core and differ only in
output: **`serialize` is human-readable text**, **`dump` is compact binary**. Supported value types:
`None`/`Bool`/`Integer`/`Float`/`String`/[`Bytes`](types.html#bytes)/`List`/`Dict`/`Set`, **user
`class` instances**, and — self-contained — **`Function` and `class` values themselves**.

### Security: never load a blob you do not trust

**`loads`/`load` runs code.** A `Function`/`class` blob carries the construct's *source text*, and
reconstructing it re-parses and executes that source — a class body's eager class-variable
initializers run right there, at load time. A blob is therefore a **program**, not inert data: whoever
wrote it chooses what runs inside your VM the moment you load it, with your permissions.

<!--norun (illustrates the hazard rather than running it; `blob` is deliberately undefined)-->
```kirito
# If `blob` came from somewhere you don't control, this is the same as running their script:
var value = dump.loads(blob)     # a class body inside the blob executes HERE
```

This is inherent to the source-reparse design and is exactly the hazard Python's `pickle` carries;
treat the two the same way. Load blobs only from sources you would equally trust to hand you a `.ki`
file and let you run it — your own files, your own processes, an authenticated peer. For data that
crosses a trust boundary, use [`json`](#json) instead: it carries values only, never code. (Kirito's
deserializer is hardened against *memory-safety* abuse — malformed and byte-flipped blobs fail
cleanly — but no amount of that stops a well-formed blob whose class body simply asks to run.)

### Functions and classes serialize by default

A **`Function`** value round-trips through both formats with no special work: its source text is stored
and re-parsed on load, and the free variables it **captures travel with it — by value**. A referenced
**user function or class travels recursively** (so a function that calls a helper serializes the helper
too), a **standard/stdlib module reconnects by re-`import`** on load (a closure over `math`/`json`/… is
not copied — it re-binds to the loading VM's module), and a builtin (`len`, `range`, …) simply
re-resolves. Self-reference and mutual recursion are preserved, so a recursive `Function` still recurses
after a round-trip.

A **`class`** value serializes the same way — its source (and its base class, which **travels** with it)
is re-run on load, which also **re-registers the class by name**. This is the key consequence for
instances: an **instance now carries its class**, so `dump.loads(dump.dumps(myInstance))` works in a
fresh VM **with no import of the defining module** — the class is reconstructed from the blob. (An
instance whose class is already defined in the loading VM still reconnects to it by name, as before.)

Two limitations: a `Function` literal written **inside an f-string** has no captured source, so it
isn't serializable (define it as a normal binding); and a **native/built-in** function bound to a
variable (e.g. `var f = math.sqrt`) can't be serialized — wrap it in a Kirito `Function`, or re-`import`
the module on load. Live-resource natives (below) remain non-serializable.

A class instance is serialized **by its attributes** by default and reconstructed against its class —
which now **travels in the blob** (re-registered on load), so the loading VM needs no import; if a class
of that name is already defined there, the instance reconnects to it instead. Either way `_init_` is
*not* re-run. A
class can override this with the **`_getstate_`/`_setstate_` protocol**: `_getstate_(self)` returns
the serializable state to store, and `_setstate_(self, state)` restores it — useful to drop transient
fields (recomputing them on load) or to reduce a value to plain serializable data. A native (C++)
type participates the same way: define `_getstate_`/`_setstate_` and register a reconstructor with
`vm.registerDeserializer(typeName, factory)`. (`json` has no object notion, so it can't serialize
instances.)

The native **value** types opt in and round-trip through both `serialize` and `dump`: **`Matrix`**
and **`Vector`** (`matrix`), **`Complex`** and **`ComplexMatrix`** (`complex`), **`DateTime`**
(`time`), **`Random`** (`random`, restoring the generator's exact stream — a reproducible
checkpoint), and gradient-free **`Tensor`** (`tensor`; a Tensor that requires grad must be
`detach()`-ed first). They can be stored standalone or nested inside Lists/Dicts/Sets/instances, with
shared references preserved. Resource-like natives that wrap live state — `Socket`/`Session` (`net`),
open files/`BytesIO`/streams (`io`), a compiled `Regex`/`Match` (`regex`) — are **not** serializable
and throw a clear, catchable error instead.

Human-readable **text** serialization → a `String`.

- `dumps(value) → String` — serialize to a text blob.
- `loads(text: String)` — reconstruct the value graph from a `dumps` blob.
- `save(value, path: String) → None` — `dumps` to a file.
- `load(path: String)` — `loads` from a file.

---

## statistics

- `mean(data) → Float` — arithmetic mean.
- `median(data) → Float` — middle value.
- `mode(data)` — the single most common value.
- `multimode(data) → List` — all values tied for most common.
- `variance(data) → Float` — the sample variance.
- `stdev(data) → Float` — the sample standard deviation.
- `pvariance(data) → Float` — the population variance.
- `pstdev(data) → Float` — the population standard deviation.
- `quantiles(data[, n]) → List` — cut points dividing `data` into `n` equal groups (`n ≥ 1`, default
  `4`); throws on fewer than two data points or `n < 1`.

---

## string

- Constants: `ascii_letters`, `ascii_lowercase`, `ascii_uppercase`, `digits`, `hexdigits`, `octdigits`, `punctuation`, `whitespace` (all `String`).
- `capwords(s) → String` — capitalize each **space-separated** word (splits on single spaces; does not
  collapse whitespace runs or treat tabs/newlines as separators).

Fuzzy comparison, built on the native `String.levenshtein` edit distance:

- `similarity(a, b) → Float | List` — a `0.0`–`1.0` ratio, `1 - editdistance / longerlength` (two empty strings are `1.0`). `b` may be a single String (returns one `Float`) **or a List of candidate Strings** (returns one score per candidate, computed in a single native call).
- `closest(query, candidates) → String` — the candidate with the smallest edit distance (ties to the earliest), or `None` for an empty list. One native call computes every distance at once.
- `fuzzymatch(query, candidates, cutoff = 0.6) → List` — every `[candidate, score]` pair whose similarity is at least `cutoff`, sorted by score descending.

```kirito
var string = import("string")
string.closest("aple", ["apple", "grape", "mango"])   # "apple"   (typo correction)
string.similarity("kitten", "sitting")                  # ~0.571
string.similarity("abc", ["abc", "abd", "xyz"])         # [1.0, 0.667, 0.0]  (List form)
```

---

## sys

Process environment and platform.

- `platform` — `"linux"` / `"darwin"` / `"windows"` (a `String`).
- `arch` — the CPU architecture, normalized to the names used in release-asset filenames:
  `"x64"` / `"arm64"` / `"x86"` / `"unknown"` (a `String`).
- `version` — the Kirito interpreter version, a semantic-version `String` (e.g. `"1.9.5"`); the same
  value `ki --version` prints. `kpm` compares it against the latest GitHub release to self-upgrade.
  (The running binary's own path is [`path.executable`](#path), and the temp dir is
  [`path.gettempdir`](#path) — filesystem locations live in `path`.)
- `getenv(name: String, default = None)` — an environment variable, or `default` if unset.
- `setenv(name: String, value: String) → None` — set a variable.
- `unsetenv(name: String) → None` — remove a variable.
- `environ() → Dict` — all environment variables.
- `traceback() → String` — the call chain of the most recent error this VM unwound (empty until one
  is thrown); useful for logging inside a `catch`.

> **Encoding & empty values.** Names and values are byte-for-byte round-tripped on POSIX. On
> Windows they go through the narrow (ANSI code-page) environment API, so a non-ASCII value isn't
> guaranteed to survive a `setenv`→`getenv` round-trip (keep env values ASCII for portability), and
> setting a variable to the **empty string** is treated as *unset* — `getenv` then returns `None`.
- `exit(code: Integer = 0)` — terminate the process with the given exit code. The code is taken mod
  256 (POSIX 8-bit wrap: `exit(256)` → `0`, `exit(-1)` → `255`). A non-Integer, non-`None` argument is
  printed to stderr and the process exits `1` (so `sys.exit("error message")` reports a failure rather
  than masking it as a success).

### Running external programs

Run another program (e.g. `ffmpeg`, `git`) and collect its result. Both functions block until the
child finishes, capture its output, and return a `Dict` `{"code", "stdout", "stderr"}` — the exit
code as an `Integer`, stdout and stderr as Strings. stdout and stderr are drained concurrently, so a
child that produces a lot on either stream can't deadlock. **This is for external programs, not the
`parallel` worker-VM model** (which runs Kirito functions, not other executables). The Kirito API is
identical on Linux, macOS and Windows.

- `createprocess(args: List, cwd = None, input = "", timeout = None, binary = False) → Dict` — run a
  program **directly** by its argument vector. `args[0]` is the program (looked up on `PATH`); the
  remaining items are passed to it **verbatim** — there is no shell, so no quoting, globbing or injection.
  - `cwd` — working directory for the child (`None` = inherit the parent's).
  - `input` — a **String or Bytes** written to the child's stdin (then closed); `""` sends nothing. A
    String is written as its UTF-8 encoding; a **Bytes is written verbatim** — use Bytes to send
    arbitrary binary, since a String built from `Bytes.decode()` would re-encode every byte ≥ 0x80 into
    a multi-byte UTF-8 sequence and corrupt a binary consumer.
  - `timeout` — seconds (a number); if the child is still running when it elapses, it is killed and a
    catchable error is thrown. `None` waits indefinitely.
  - `binary` — when `True`, `stdout`/`stderr` come back as raw **Bytes** (byte-exact) instead of a
    String. Pair a Bytes `input` with `binary = True` for a fully byte-faithful binary pipeline.
  - A program that can't be started (not found, etc.) throws a clear catchable error.

  <!--norun (needs ffmpeg/pngquant + a network URL; illustrative)-->
  ```kirito
  var r = sys.createprocess(["ffmpeg", "-i", "in.mov", "out.mp4"], timeout = 60)
  if r["code"] != 0:
      io.eprint(r["stderr"])

  # pipe raw bytes through a tool without touching disk:
  var png = net.get(url).content                       # a Bytes
  var out = sys.createprocess(["pngquant", "-"], input = png, binary = True)["stdout"]  # a Bytes
  ```

- `shell(command: String, cwd = None, input = "", timeout = None, binary = False) → Dict` — run
  `command` through the **system shell** (`/bin/sh -c` on POSIX, `cmd.exe /c` on Windows), so pipes,
  redirection, globbing and shell scripts work. Same `cwd`/`input`/`timeout`/`binary` options (a Bytes
  `input` and `binary = True` give byte-exact binary I/O). Capture the output with:

  ```kirito
  var head = sys.shell("git rev-parse HEAD")["stdout"].strip()
  ```

  > Prefer `createprocess` (an argv list) when the arguments come from untrusted input — `shell`
  > passes the whole string to the shell, so it is subject to the usual shell-injection caveats.

  The exit code follows the platform convention; on POSIX a child terminated by a signal reports
  `128 + signal`. Output is captured as a byte-transparent `String` (decode/parse it as needed).

---

## tabular

A dataframe-style data-analysis library: a labelled 1-D **`Series`** and 2-D **`DataFrame`**, with CSV
I/O, label/position indexing, boolean masking, element-wise arithmetic (on `Series` —
a `DataFrame` is operated on per-column), aggregations, group-by, joins, and missing-data handling.
`Series`-to-`Series` arithmetic and comparison require **equal length** (a mismatch throws consistently
in either order — no silent truncation); a `Series`-to-scalar op broadcasts the scalar. Element-wise
arithmetic and comparison **propagate missing** (a `None`/NaN element yields `None`, pandas-style)
rather than throwing — so a boolean mask like `df[df["col"] > v]` works even when the column has a
blank cell (which `readcsv` produces from an empty field).
Public names follow Kirito's lowercase-no-underscore convention (`readcsv`, `sortvalues`,
`valuecounts`, `resetindex`, ...).

> **Column order from a dict.** Kirito dicts are not insertion-ordered, so
> `DataFrame({"a": ..., "b": ...})` does not guarantee column order. Pass `columns=[...]` (or use
> `readcsv`, whose header row is an ordered List) when order matters.

### Module functions

- `Series(values, index = None, name = None)` — a 1-D labelled column.
- `DataFrame(data = None, columns = None, index = None)` — `data` is a Dict of `column → values`, a
  List of row-Lists (pair with `columns`), or a List of row-Dicts (columns are the key union).
- `readcsv(source, header = True, infer = True)` — build a DataFrame from CSV text (or a filename).
  With `infer`, each cell becomes Integer/Float/Bool/None/String; a short row's missing trailing cells
  are `None`, but a row with **more** fields than the header throws (no silent data loss, like pandas).
  Fully **blank lines are skipped** (pandas `skip_blank_lines=True` parity) — so a single-column frame
  whose only value on a row is `None` serialises to a blank line via `tocsv` and that row is dropped on
  re-read; in a multi-column frame a `None` is a real empty field (`a,,c`) and survives the round-trip.
- `merge(left, right, on, how = "inner")` — join two DataFrames on a key column; `how` is
  `"inner"`/`"left"`/`"right"`/`"outer"`. A non-key column present in **both** frames is disambiguated
  pandas-style: the left copy becomes `<name>_x` and the right `<name>_y`.
- `concat(frames)` — stack DataFrames vertically (column union, missing filled with `None`).

### Series

Indexing: `s[label]` (by index label, falling back to position), `s.iat(pos)`. Element-wise `+ - *
/ // %` and comparisons `== != > >= < <=` against a scalar or another Series (the comparisons return
a **boolean Series** for masking); `s.eq(x)`/`s.ne(x)`/`s.isin(values)`. Because `==`/`!=` are
element-wise, a Series has no single truth value — using one directly in an `if`/`while` is an error;
reduce it first (`s.all()`/`s.any()`) or index with the mask.

- Aggregations (skip missing; Bool counts as 0/1): `sum`, `mean`, `min`, `max`, `median`, `variance`,
  `std`, `prod`, `count`.
- `unique()`, `nunique()`, `valuecounts()` (a Series of counts, descending). `unique`/`nunique`
  treat a missing value (`None`) as one distinct value; `valuecounts` skips missing values.
- `apply(fn)`/`map(fn)`, `astype("Integer"|"Float"|"Bool"|"String")`.
- `fillna(value)`, `dropna()`, `head(n=5)`, `tail(n=5)`, `sortvalues(ascending=True)`, `resetindex()`,
  `tolist()`, `copy()`.

### DataFrame

- Selection: `df["col"]` → Series; `df[["a", "b"]]` → DataFrame; `df[boolean_series]` → masked rows;
  `df.iloc[i]`/`df.iloc[[i, j]]` (by position), `df.loc[label]`/`df.loc[[l1, l2]]` (by label);
  `df.column(name)`, `df.at(label, col)`, `df.iat(pos, col)`.
- `df["new"] = series_or_list_or_scalar` adds/replaces a column; `assign(name, value)` returns a copy
  with the column added.
- Shape/views: `shape()` → `[rows, cols]`, `nrows()`, `columns`, `index`, `len(df)`,
  `head`/`tail`/`slice`, `rowat(pos)` (one row as a Series, indexed by column name) / `rowsat(positions)` (a sub-DataFrame),
  `rename(columns)`, `drop(columns)`, `setindex(col)`, `resetindex()`, `copy()`, `todict()`,
  `torows()`, `iterrows()`, `tocsv()`.
- Aggregations over **numeric** columns → a Series indexed by column: `sum`, `mean`, `min`, `max`,
  `std`. `count` is the exception — it tallies non-null values for **every** column (any dtype).
  `apply(fn)` maps `fn` over each numeric column's Series, returning a Series indexed by column.
  `describe()` → a DataFrame of count/mean/std/min/median/max.
- `sortvalues(by, ascending = True)`, `groupby(col)`, `merge(other, on, how)`, `dropna()`,
  `fillna(value)`.

### GroupBy

`df.groupby(col)` returns a grouping with numeric-column reductions `sum`/`mean`/`min`/`max`/`std`,
`size()` (a Series of group sizes), and `count` — which, unlike the numeric reductions, spans **every**
value column and reports each group's **row count** in each (effectively `size()` broadcast across the
columns, not a per-column non-null tally). Also `agg({col: reducer})` where `reducer` is one of
`"sum"`/`"mean"`/`"min"`/`"max"`/`"std"`/`"count"`/`"median"`, and `apply(fn)` (fn receives each
group's sub-DataFrame).

```kirito
var io = import("io")
var tb = import("tabular")

var df = tb.readcsv("name,dept,salary\nAda,eng,120\nAlan,eng,110\nGrace,ops,95\nEdsger,ops,130")
io.print(df[df["salary"] > 100]["name"].tolist())     # ['Ada', 'Alan', 'Edsger']
io.print(df.groupby("dept").mean()["salary"].tolist()) # [115.0, 112.5]
io.print(df.sortvalues("salary", ascending=False)["name"].tolist())
```

---

## tee

Fan-out streams: clone what you write to a stream into one or more extra streams (for example, mirror
stdout into a log file). A `Tee` implements the `write`/`writelines`/`flush` stream protocol, so it
can be assigned to `io.stdout`/`io.stderr`, and it is a context manager (it flushes on exit).

- `Tee(primary, copies = None) → Tee` — a stream that writes each chunk to every *copy* first, then to
  `primary`. `copies` is a single stream or a List; `primary` may be `None` for a pure fan-out sink.
- `t.write(data) → Integer` — write `data` to every stream (copies first, then primary).
- `t.writelines(lines) → None` — write each String in an iterable to every stream.
- `t.flush() → None` — flush every underlying stream.
- `t.close() → None` — close the Tee (flushes; does not close the copy streams you supplied).
- `t.streams() → List` — the underlying streams in write order (copies, then primary).
- `tee_stdout(copies)` — a context manager that makes `io.stdout` also write to `copies` inside the
  block, restoring the original on exit (the copy streams are never closed — you own them).
- `tee_stderr(copies)` — the same for `io.stderr`.

```kirito
var io = import("io")
var tee = import("tee")
with io.open("session.log", "w") as f:
    with tee.tee_stdout(f):
        io.print("appears on the console and in session.log")
# stdout is restored here
```

---

## tensor

Dense **N-dimensional** arrays — the generalization of a matrix to any rank. The element type
(**dtype**) is chosen at construction: `"Float"` (double, the default) or `"Complex"`. The numeric
engine is shared C++ (`src/kirito/tensor.hpp`) and is what the `matrix` and `complex` matrix types are
themselves built on; a 2-D tensor *is* a matrix. It is CPU-only but carries a **reverse-mode autograd**
(see below) and a GPU-forward-compatible single-buffer design.

Tensor **arithmetic is pure**: every operation returns a *new* tensor and never mutates its operands,
which is what makes the autograd graph well-defined. The only in-place operation is element assignment
(`t[i, j] = v`). A consequence is that a gradient-descent step **rebinds** the parameter
(`w = w - w.grad*lr`, re-marked `requiresgrad(True)`) — a functional update, like JAX/Optax — rather
than mutating it in place as PyTorch does; the [tensors lesson](bonus-05-tensors.html#why-the-update-rebinds-tensors-are-immutable)
walks through this.

### Constructors and factories

Each constructor/factory takes an optional **`requiresgrad`** keyword (default `False`) that marks the
result as a differentiable leaf (Float only — see [Autograd](#autograd)).

- `Tensor(data: List, dtype = "Float", requiresgrad = False) → Tensor` — build from a (rectangular)
  nested list; the nesting depth sets the rank. `tensor` is an alias of `Tensor`. Passing a bare
  `Number` instead of a list builds a **rank-0** (scalar) tensor — `shape() == []`, `ndim() == 0`,
  `size() == 1` — the same 0-D tensor a full contraction or whole-tensor reduction yields; use
  `item()` to extract the scalar back.
- `zeros(shape: List, dtype = "Float", requiresgrad = False) → Tensor` — a tensor of zeros.
- `ones(shape: List, dtype = "Float", requiresgrad = False) → Tensor` — a tensor of ones.
- `full(shape: List, value: Number, dtype = "Float", requiresgrad = False) → Tensor` — filled with `value`.
- `eye(n: Integer, dtype = "Float", requiresgrad = False) → Tensor` — the n×n identity matrix.
- `arange(stop)` / `arange(start, stop[, step]) → Tensor` — a 1-D ramp of Floats from `start` up to
  (but excluding) `stop`, stepping by `step`.

### Tensor object

- `t.shape() → List`, `t.ndim() → Integer`, `t.size() → Integer`, `t.dtype() → String`.
- `t[i, j, ...] → Number` — a **full** index (one per dimension) returns the scalar element.
- `t[i] → Tensor` — a **partial** index returns the sub-tensor of the remaining axes.
- `t[i, j, ...] = v` — assign an element (full index).
- `a + b`, `a - b`, `a * b`, `a / b`, `a % b`, `a // b`, `a ** b` — **element-wise** with
  **broadcasting** (axes align from the right; each must be equal or 1). `*` is element-wise
  (Hadamard), **not** matrix multiply; `%`/`//` throw on a zero divisor, like scalar arithmetic.
  Mixing a `Float` and a `Complex` tensor promotes the result to `Complex`. A scalar operand applies
  element-wise (`t * 2`, `t ** 2`).
- `-t` — element-wise negation.
- `a == b → Bool` — equal shape and **exact** element-wise equality (`NaN` never equal); distinct
  from the elementwise `.eq()` mask. Use `a.compare(other, rel_tol = 1e-9, abs_tol = 0.0) → Bool` for
  a tolerant whole-tensor check (a `solve`/`inv` result vs its literal) — pass an `abs_tol` when the
  target contains exact zeros (`rel_tol` alone can't match a near-zero element).
- `t.matmul(other) → Tensor` — matrix product (2-D), or **batched** over the leading dimensions for
  rank ≥ 2.
- `t.dot(other) → Number` — the dot product of two 1-D tensors.
- `t.transpose() → Tensor` — reverse all axes (the matrix transpose when 2-D).
- `t.permute(axes: List) → Tensor` — reorder axes by the given permutation.
- `t.reshape(shape: List) → Tensor` — same elements, new shape.
- `t.flatten() → Tensor` — a 1-D copy.
- `t.apply(fn) → Tensor` — a new tensor with `fn` mapped over every element (the element-wise map).
- `t.astype(dtype: String) → Tensor` — convert dtype (`Float → Complex`, or `Complex → Float` keeping
  the real part).
- `t.item() → Float | Complex` — the single scalar of a one-element tensor (throws otherwise).
- `t.tolist() → List` — the tensor as a nested Kirito `List` (Float/Complex leaves), shape-for-shape.
- `t.sum(axis = None)`, `t.mean(axis = None)`, `t.prod(axis = None)` — reduce the whole tensor to a
  scalar, or one `axis` to a lower-rank tensor. A **negative axis** counts from the end NumPy-style
  (`-1` is the last axis); an out-of-range axis throws. (Applies to every axis-taking reduction below.)
- `t.min(axis = None)`, `t.max(axis = None)` — extremes (whole-tensor or along an axis; throw for a
  `Complex` tensor, which is unordered).

### Indexing & slicing

- `t[i, j, ...]` — integer index (full → element, partial → sub-tensor); `t[i, j] = v` assigns.
  Negative indices count from the end of each axis (`t[-1]` is the last row), like NumPy/slicing.
- `t[start:stop:step]` — slice the **first** axis (negative bounds count from the end; returns a detached copy).
- `t[mask]` — boolean selection where `mask` is a same-shape 0/1 tensor (→ 1-D).
- `t[[i, j, k]]` — fancy index: gather those rows along axis 0. Indices may be negative (counted from
  the end, like scalar indexing).
- `t.slice(start, stop, step, axis = 0) → Tensor` and `t.take(indices, axis = 0) → Tensor` — the
  **autograd-aware** forms of slicing / row-gather (the gradient scatters back); `take` indices may be
  negative.

### Comparisons, logic, selection

- `t.eq/ne/lt/le/gt/ge(other) → Tensor` — element-wise comparisons returning a 0/1 mask (the
  `< <= > >=` operators do the same; `==`/`!=` stay whole-tensor `Bool`). Float only.
- `t.logicaland/logicalor/logicalxor(other)`, `t.logicalnot()` — element-wise logic on 0/1 masks.
- `where(cond, a, b) → Tensor` — element-wise select (`cond` non-zero → `a` else `b`); differentiable.
- `t.clip(lo, hi)`, `t.maximum(other)`, `t.minimum(other)` — clamp / element-wise max / min;
  differentiable.

### More reductions

- `t.argmin(axis = None)`, `t.argmax(axis = None)` — index of the extreme.
- `t.std(axis = None, ddof = 0)`, `t.var(axis = None, ddof = 0)` — standard deviation / variance.
- `t.all(axis = None)`, `t.any(axis = None)` — truth reductions.
- `t.ptp(axis = None)` — max − min; `t.median(axis = None)` — sorts `NaN` last (like `sort`/`argsort`/`unique`), so a `NaN` only affects the result when it lands at the median position.
- `t.cumsum(axis = None)` (differentiable) / `t.cumprod(axis = None)` — cumulative scans
  (`axis = None` flattens first).

**Reducing nothing.** When there is nothing to reduce — a zero-length axis, or an empty tensor — the
answer depends on whether the reduction has an identity. `sum` and `prod` do, so they return it
(`zeros([3, 0]).sum(axis = 1)` → `[0.0, 0.0, 0.0]`, `prod` → `[1.0, 1.0, 1.0]`, as in NumPy): the sum
of no numbers really is 0. `mean`, `min`, `max`, `ptp`, `median`, `std` and `var` have none — the mean
of nothing is `0/0` and the largest of nothing does not exist — so they **throw**, the same way
`math.sqrt(-1)` throws instead of handing back a quiet `NaN`. Check `len` first, or reduce with `sum`.

### Structural ops

- `t.squeeze(axis = None)`, `t.expanddims(axis)`, `t.swapaxes(axis1, axis2)`, `t.flip(axis = None)`,
  `t.broadcastto(shape)`, `t.repeat(count, axis = None)`, `t.tile(reps)` — all differentiable except
  `repeat`/`tile`.
- `concatenate(tensors, axis = 0)` (alias `concat`), `stack(tensors, axis = 0)`,
  `split(t, sections, axis = 0)` — join / split a List of tensors (differentiable). `sections` is an
  Integer (equal parts) or a List of sizes.

### Creation helpers

- `linspace(start, stop, num = 50)`, `zeroslike(t)`, `oneslike(t)`, `fulllike(t, value)`,
  `identity(n)` (alias of `eye`), `diag(t, k = 0)` (1-D → diagonal matrix, 2-D → its diagonal),
  `tril(t, k = 0)` / `triu(t, k = 0)` (lower / upper triangle).

### Linear algebra (module functions, 2-D)

- `det(t)`, `inv(t)`, `solve(a, b)`, `trace(t)`, `norm(t, ord = 2)` (**entry-wise** — the flattened
  vector norm, so on a 2-D tensor `ord = 2` is Frobenius, not the induced matrix 2-norm), `outer(a, b)`,
  `inner(a, b)`,
  `kron(a, b)`, `cross(a, b)` (3-vectors), and `einsum(spec, *tensors)` — a general Einstein-summation
  (transpose / diagonal / trace / contraction / outer, any subscript string). Work on both dtypes
  where it makes sense (forward only; the differentiable linear algebra is `matmul`/`tensordot`).
  `inner`/`tensordot`/`contract`/`einsum` return a **Tensor** (0-D for a full contraction — call
  `.item()` for a Float scalar); only `dot` and the scalar reductions (no `axis`) return a plain Float.
  A repeated *output* label in `einsum` (e.g. `"ii->ii"`) is rejected. `outer` flattens operands of any
  rank, but `kron` requires both operands to be **2-D** (throws otherwise). `det` can underflow to `0.0`
  for an extreme-but-well-conditioned matrix (e.g. `diag(1e-200)`, true det `1e-400`) even though `inv`
  and `solve` still succeed — the singularity test is scale-relative, so `det == 0.0` does not by itself
  mean singular at extreme scales.

### Sorting & search

- `t.sort(axis = None)`, `t.argsort(axis = None)` (default last axis), `unique(t)` (sorted unique 1-D),
  `nonzero(t)` (a List of per-axis index tensors), `searchsorted(a, v)` (insertion indices into a
  sorted 1-D `a`).

### Complex helpers

- `t.real()`, `t.imag()`, `t.angle()` → Float tensors; `t.conj()` / `t.conjugate()` → the conjugate (Complex; no-op on Float tensors). On a **Float** tensor `angle()` returns the phase of each element treated as a real number: `pi` for negative elements, `0` otherwise.

### Differentiable element-wise math

Every one of these returns a new tensor with the function applied element-wise, and each is
**differentiable** under autograd (Float only):

- exponential / log: `exp`, `log`, `log10`, `log2`, `softplus`, `erf`
- powers / roots: `sqrt`, `cbrt`, `square`, `reciprocal`, `pow(p)`
- sign / magnitude / rounding: `abs`, `sign`, `floor`, `ceil`, `round`, `trunc` (gradient zero)
- trigonometric: `sin`, `cos`, `tan`, `asin`, `acos`, `atan`
- hyperbolic: `sinh`, `cosh`, `tanh`, `asinh`, `acosh`, `atanh`
- neural-net: `relu`, `sigmoid`

Like the scalar `math` module — and like the tensor engine's own division-by-zero guard — these throw a
clear `tensor … : math domain error` on an out-of-domain **element** instead of silently emitting
`NaN`/`inf` into the result (and poisoning gradients): `log`/`log10`/`log2` of `≤ 0`, `sqrt` of a
negative, `asin`/`acos` outside `[-1, 1]`, `acosh` below `1`, `atanh` at/outside `±1`, `reciprocal` of
`0`, and `pow` of a negative base to a non-integer exponent (or zero to a negative power). A `NaN`
element passes through; genuine overflow to `inf` is not a domain error.

### tensordot / contract

General tensor contraction over chosen axes (built from `permute`/`reshape`/`matmul`, so it is
differentiable):

- `tensordot(a: Tensor, b: Tensor, axes = 2) → Tensor` — `axes` is an **Integer** `N` (contract the
  last `N` axes of `a` with the first `N` of `b` — `N = 0` is the outer product, `N = 1` is matrix
  multiply, `N = 2` is the Frobenius double-contraction to a scalar) or a **`[a-axes, b-axes]`** pair
  (each an Integer or a List of Integers) naming the axes to pair up.
- `contract(a: Tensor, b: Tensor, aaxes, baxes) → Tensor` — `tensordot` with the two axis lists given
  explicitly.

### Autograd

Reverse-mode automatic differentiation, opt-in and **Float-only**. Tensors **do not** track gradients
by default; mark a leaf with `requiresgrad = True` (constructor keyword) or `t.requiresgrad(True)`
post-creation. Differentiable operations then record a computational graph; `backward()` walks it and
accumulates each tensor's gradient. The graph records *operations*, not where the data lives, so the
design carries forward to a future GPU backend.

Autograd applies to the **Float** dtype only: a **Complex tensor has no gradients** — marking one with
`requiresgrad = True` (constructor keyword or method) throws, and the differentiable element-wise math
methods are Float-only as well. Complex tensors remain a full numeric container (arithmetic, `matmul`,
`tensordot`, reshaping, reductions); use the `complex` module for complex analytic functions.

- `t.requiresgrad() → Bool` — whether `t` tracks gradients; `t.requiresgrad(flag)` sets it (Float
  only; turning it off detaches `t` from the graph).
- `t.grad → Tensor` — the accumulated gradient (same shape as `t`), or `None` before `backward()`.
- `t.backward(seed = None) → None` — propagate gradients back from `t`. With no `seed`, `t` must be a
  scalar (a 0-D or single-element tensor) and the seed is `1`; otherwise pass a seed tensor of `t`'s
  shape. Gradients **accumulate** into `.grad` (call `zerograd()` between steps).
- `t.zerograd() → None` — clear `t.grad`.
- `t.detach() → Tensor` — an independent copy (its own buffer) that tracks no gradient — it stops
  gradient flow. (It is a deep copy, not a view: mutating one via element assignment does not affect
  the other.)
- `nograd()` — a context manager: inside `with tensor.nograd():` no operation tracks gradients (for
  inference or for in-place parameter updates).

**Differentiable ops:** `+ - * / **` (with broadcasting), `matmul`, `tensordot`/`contract`,
`sum`/`mean` (whole-tensor or per-axis), `transpose`/`permute`/`reshape`/`flatten`, `squeeze`/
`expanddims`/`swapaxes`/`flip`/`broadcastto`, `concatenate`/`stack`/`split`,
`where`/`clip`/`maximum`/`minimum`, `cumsum`, the grad-aware `slice`/`take`, unary `-`, and the
differentiable math set above. Non-differentiable ops — `apply` (an arbitrary function), `min`/`max`,
`argmin`/`argmax`, `prod`, `ptp`, `median`, `all`/`any`, `cumprod`, `dot`, `sort`/`argsort`,
`unique`/`nonzero`/`searchsorted`, `einsum`, `%`/`//`, the linear-algebra family
(`det`/`inv`/`solve`/`trace`/`norm`/`outer`/`kron`/`cross`), `astype`, `repeat`/`tile`, and plain
indexing — detach (stop the gradient). Each emits a **one-time detach warning** when used on a
grad-tracking tensor, so a gradient break is **never silent** (call `detach()` or use `nograd()` to
do it intentionally). On a grad-tracking tensor, a whole-tensor `sum`/`mean` returns a 0-D tensor
(so the graph continues) rather than a plain `Float`.

```kirito
var io = import("io")
var T = import("tensor")

# fit y = 2x + 1 by gradient descent
var xs = T.Tensor([[0], [1], [2], [3]])
var ys = T.Tensor([[1], [3], [5], [7]])
var w = T.zeros([1, 1], requiresgrad = True)
var b = T.zeros([1, 1], requiresgrad = True)
var step = 0
while step < 500:
    var loss = (xs.matmul(w) + b - ys).square().mean()
    w.zerograd()
    b.zerograd()
    loss.backward()
    with T.nograd():
        w = w - w.grad * 0.05
        b = b - b.grad * 0.05
    w.requiresgrad(True)
    b.requiresgrad(True)
    step = step + 1
io.print(w[0, 0], b[0, 0])      # ~ 2.0  ~ 1.0
```

---

## textwrap

- `wrap(text[, width]) → List` — wrap into a list of lines.
- `fill(text[, width]) → String` — wrap into a single newline-joined String.
- `indent(text, prefix) → String` — prefix each line.
- `dedent(text) → String` — remove the common leading whitespace.

---

## time

Clocks and calendar time.

- `time() → Float` — seconds since the Unix epoch (wall clock).
- `timens() → Integer` — nanoseconds since the epoch.
- `monotonic() → Float` — seconds from a steady clock (for measuring intervals).
- `perfcounterns() → Integer` — nanoseconds from the highest-resolution clock.
- `sleep(seconds: Number) → None` — pause execution. `seconds ≤ 0` is a clean no-op; a non-finite value (`NaN`/`Infinity`) or one larger than `1e9` throws (`sleep: seconds too large (maximum 1e9)`) rather than hanging.
- `now() → DateTime` — current UTC time.
- `datetime([timestamp: Number]) → DateTime` — a `DateTime` from epoch seconds (current time if omitted).
- `make(year, month, day, hour = 0, minute = 0, second = 0) → DateTime` — build from UTC components.
  Out-of-range components **normalize** (C `mktime`-style: month 13 → January of the next year,
  day 32 → the 1st of the next month), rather than throwing.
- `strptime(text: String, format: String) → DateTime` — parse a time string against a format. The
  directive set is deliberately small: `%Y %m %d %H %M %S` and a literal `%%` (any other `%`-directive
  throws "does not match format" — this is narrower than `dt.format`, which delegates to the full
  `strftime`). Unlike `make`, parsing is strict: a literal/format mismatch, an **out-of-range** field
  (`2024-99-99`, hour `25`), or **unconverted trailing input** (`"2024-01-01XYZ"`) all throw rather
  than silently producing a garbage date.

### DateTime object

The UTC fields and epoch seconds are Integer **attributes** (no parentheses):

- `dt.year` — the year.
- `dt.month` — the month (1–12).
- `dt.day` — the day of the month.
- `dt.hour` — the hour (0–23).
- `dt.minute` — the minute (0–59).
- `dt.second` — the second (0–59).
- `dt.weekday` — the day of the week, **0 = Sunday … 6 = Saturday** (C convention; Sunday-based, not Monday-based).
- `dt.yearday` — the day of the year.
- `dt.timestamp` — epoch seconds.

Its methods:

- `dt.iso() → String` — ISO-8601 text; `dt.isoformat()` is an alias.
- `dt.format(fmt: String) → String` — format with `%`-codes (`%Y`, `%m`, `%d`, `%H`, `%M`, `%S`, …).
- `dt.add(seconds) → DateTime` — a new DateTime shifted forward by `seconds`.
- `dt.sub(seconds) → DateTime` — a new DateTime shifted back by `seconds`.
- `dt.diff(other) → Integer` — difference (`self - other`) in seconds.

A `DateTime` has **value equality and hashing by instant** (epoch seconds): two DateTimes for the
same moment compare `==` and hash the same, so a DateTime can be a `Dict` key or `Set` member. It is
also **serializable** — it round-trips through both [`serialize`](#serialize) and [`dump`](#dump).

---

## xml

A small, dependency-free XML parser/serializer. It parses
elements, attributes, text, nested children, comments, the `<?xml?>` declaration, `<!DOCTYPE>`,
`<![CDATA[…]]>` sections, and the standard entities (`&lt; &gt; &amp; &quot; &apos;` and numeric
`&#65;` / `&#x41;`); it serializes a tree back to XML. The parser is **lenient** — malformed markup
is tolerated rather than throwing.

### Module functions

- `parse(text: String) → Element` — parse a document and return its root `Element` (or `None` if the
  text contains no element). `fromstring` is an alias. Given malformed input with several top-level
  siblings, the **last** one becomes the root.
- `tostring(element) → String` — serialize an element (and its subtree) back to XML.
- `Element(tag, attrib = None) → Element` — construct an element directly (for building a tree).

### Element

An element exposes `.tag` (String), `.attrib` (a Dict of attribute → value), `.text` (character data
before the first child), `.tail` (character data after this element's end tag),
and `.children` (a List of child `Element`s). It is iterable (yields its children), supports `len`
and indexing (`elem[0]`).

- `get(key, default = None)` — an attribute value, or `default` if absent.
- `find(tag)` — the first child with that tag, or `None`.
- `findall(tag)` — a List of all children with that tag.
- `findtext(tag, default = "")` — the text of the first matching child, or `default`.
- `itertext()` — a List of all text in document order (this element's text, then each descendant's
  text and tail, walking the whole subtree).
- `tostring()` — serialize this element (also its `_str_`).

```kirito
var io = import("io")
var xml = import("xml")

var root = xml.parse("<books><book id='1'><title>The Hobbit</title></book>" +
                     "<book id='2'><title>SICP</title></book></books>")
for book in root.findall("book"):
    io.print(book.get("id") + ": " + book.findtext("title"))
# 1: The Hobbit
# 2: SICP
io.print(xml.tostring(root.find("book")))   # <book id="1"><title>The Hobbit</title></book>
```

---

## zlib

DEFLATE compression (interoperable with standard zlib), self-contained. Every function takes a
**String or a [`Bytes`](types.html#bytes)** and returns the **same type as its input**, so binary
data (downloads, files) stays byte-correct as Bytes while text round-trips as a String. The gzip
*container* is its own [`gzip`](#gzip) module.

- `compress(data) → data` — zlib-format (RFC 1950) compress.
- `decompress(data) → data` — zlib-format decompress (throws on bad data).
- `deflate(data) → data` — raw DEFLATE compression (no zlib header).
- `inflate(data) → data` — raw DEFLATE decompression (no zlib header).
