# Recipes

Short, task-shaped snippets that pull **several modules together** — the kind of thing the linear
[course](course-01-hello.html) teaches piece by piece but never assembles in one place. All assume
`var io = import("io")`.

## JSON round-trip

```kirito
var json = import("json")
var data = json.loads("{\"name\": \"Kirito\", \"version\": 1}")
io.print(data["name"])                       # Kirito
io.print(json.dumps(data, indent = 2))       # pretty-print back out
```

(`json.loads`/`dumps` are the canonical names; `parse`/`stringify` are aliases.)

## Hash binary data

```kirito
var hash = import("hash")
var blob = "café · 世界".encode()             # String -> Bytes (UTF-8)
io.print(hash.sha256(blob))                  # digest over the exact bytes
io.print(hash.crc32(blob))                   # non-cryptographic checksum (Integer)
```

Hashers take a `String` **or** `Bytes`, so `hash.sha256("café")` hashes the same bytes as above.

## Compress, then decompress — byte-exact

```kirito
var gzip = import("gzip")
var original = "log line\n" * 1000
var packed = gzip.compress(original.encode())          # Bytes in, Bytes out
io.print(f"{len(original)} -> {len(packed)} bytes")
var restored = gzip.decompress(packed).decode()
assert restored == original
```

## Extract and rewrite with regex

```kirito
var regex = import("regex")
var text = "Ada <ada@x.io>, Bo <bo@y.io>"
var emails = regex.findall(r"<([^>]+)>", text)         # ['ada@x.io', 'bo@y.io']
io.print(String(emails))
var masked = regex.sub(r"@\w+", "@***", text)          # redact domains
io.print(masked)
```

## Parse CSV, then emit JSON

```kirito
var csv = import("csv")
var json = import("json")
var rows = csv.parse("name,age\nAda,36\nBo,25\n")      # -> List of List
var header = rows[0]
var out = []
for r in rows[1:]:
    var obj = {}
    for i, col in enumerate(header):
        obj[col] = r[i]
    out.append(obj)
io.print(json.dumps(out))                              # [{"name": "Ada", "age": "36"}, ...]
```

## Serialize an object graph (shared references + cycles)

```kirito
var dump = import("dump")
class Node:
    var _init_ = Function(self, v):
        self.v = v
        self.next = None

var a = Node(1)
var b = Node(2)
a.next = b
b.next = a                                             # a cycle
var blob = dump.dumps([a, b])                          # binary Bytes; preserves the shared/cyclic refs
var back = dump.loads(blob)
assert back[0].next.next.v == back[0].v                # the cycle round-trips
```

> `dump.loads`/`serialize.loads` rebuild registered classes and run `_setstate_` — like `pickle`, only
> deserialize data you trust. See [Exceptions → trust boundary](exceptions.html#the-shared-limits-behind-the-guards).

## Run an external command

<!--norun (spawns a subprocess; environment-dependent)-->
```kirito
var sys = import("sys")
var r = sys.shell("echo hello && echo oops 1>&2")
io.print(r["code"], r["stdout"].strip(), r["stderr"].strip())   # 0 hello oops
```

## Download a URL (and unpack a .gz)

<!--norun (network access)-->
```kirito
var net = import("net")
var gzip = import("gzip")
var resp = net.get("https://example.com/data.json.gz")
resp.raiseforstatus()
var text = gzip.decompress(resp.content).decode()      # resp.content is raw Bytes
io.print(text[0:80])
```

## Fan work out across cores

<!--norun (parallel.spawn needs a file-defined function; illustrative)-->
```kirito
var parallel = import("parallel")
var square = Function(n): return n * n                  # must live in a .ki file to be spawned
if argmain:
    var tasks = []
    for n in range(8):
        tasks.append(parallel.spawn(square, n))
    var results = []
    for t in tasks:
        results.append(t.join())       # collect each worker's result
    io.print(String(results))
```

## Inspect anything at runtime

```kirito
var math = import("math")
io.print(inspect(math))          # the module's functions, with signatures
io.print(inspect("hi"))          # String's methods
```
