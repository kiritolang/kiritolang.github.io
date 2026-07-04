#ifndef KIRITO_STDLIB_KIMODULES_HPP
#define KIRITO_STDLIB_KIMODULES_HPP

// Standard-library modules authored in idiomatic Kirito and frozen into the binary as source. Each
// is registered with vm.registerSourceModule(name, src) and evaluated once per VM on first import.
// Keeping these in Kirito (rather than C++) dogfoods the language and keeps the modules terse; the
// performance-critical primitives they build on (list/dict/set, sort, string ops) are already native.

#include <string_view>

namespace kirito::kimod {

// --- itertools: lazy-style combinators returning Lists (Kirito has no generators yet) ----------
inline constexpr std::string_view itertools = R"KI(
var count = Function(start = 0, step = 1, stop = None):
    # Bounded count: requires stop (unbounded would never return a List). Returns [start, start+step, ...).
    if stop == None:
        throw "itertools.count needs a stop bound (no lazy generators)"
    if step == 0:
        throw "itertools.count step must not be zero"
    var out = []
    var x = start
    while x < stop if step > 0 else x > stop:
        out.append(x)
        x = x + step
    return out

var repeat = Function(value, times):
    var out = []
    for i in range(times):
        out.append(value)
    return out

var cycle = Function(iterable, times):
    var out = []
    for i in range(times):
        for x in iterable:
            out.append(x)
    return out

var chain = Function(lists):
    var out = []
    for lst in lists:
        for x in lst:
            out.append(x)
    return out

var islice = Function(iterable, start, stop, step = 1):
    var out = []
    var i = 0
    for x in iterable:
        if i >= stop:
            break
        if i >= start and (i - start) % step == 0:
            out.append(x)
        i = i + 1
    return out

var accumulate = Function(iterable, func = None):
    # `first` (not `total == None`) marks the leading element, so accumulate([None, 1, 2]) folds the
    # leading None instead of treating it as "not started yet".
    var out = []
    var first = True
    var total = None
    for x in iterable:
        if first:
            total = x
            first = False
        elif func == None:
            total = total + x
        else:
            total = func(total, x)
        out.append(total)
    return out

var product = Function(lists):
    # Cartesian product of a list of lists -> list of lists.
    var out = [[]]
    for pool in lists:
        var nxt = []
        for prefix in out:
            for item in pool:
                var combo = prefix.copy()
                combo.append(item)
                nxt.append(combo)
        out = nxt
    return out

var permutations = Function(items, r = None):
    var n = len(items)
    var k = r
    if k == None:
        k = n
    var result = []
    if k > n or k < 0:
        return result
    # Iterative DFS (recursion is discouraged in Kirito): each frame is a list of chosen indices.
    # Pushing candidate indices in descending order makes the smallest pop first, so the emission
    # order matches the classic recursive enumeration exactly.
    var stack = [[]]
    while len(stack) > 0:
        var chosen = stack.pop()
        if len(chosen) == k:
            var perm = []
            for idx in chosen:
                perm.append(items[idx])
            result.append(perm)
            continue
        var i = n - 1
        while i >= 0:
            if i not in chosen:
                stack.append(chosen + [i])
            i = i - 1
    return result

var combinations = Function(items, r):
    var n = len(items)
    var result = []
    if r > n or r < 0:
        return result
    # Iterative DFS (no recursion): each frame is [chosen indices, next-start]. Pushing the next
    # candidate indices in descending order keeps emission in ascending lexicographic order.
    var stack = [[[], 0]]
    while len(stack) > 0:
        var frame = stack.pop()
        var chosen = frame[0]
        var start = frame[1]
        if len(chosen) == r:
            var comb = []
            for idx in chosen:
                comb.append(items[idx])
            result.append(comb)
            continue
        var i = n - 1
        while i >= start:
            stack.append([chosen + [i], i + 1])
            i = i - 1
    return result

var takewhile = Function(pred, iterable):
    var out = []
    for x in iterable:
        if not pred(x):
            break
        out.append(x)
    return out

var dropwhile = Function(pred, iterable):
    var out = []
    var dropping = True
    for x in iterable:
        if dropping and pred(x):
            continue
        dropping = False
        out.append(x)
    return out

var filterfalse = Function(pred, iterable):
    var out = []
    for x in iterable:
        if not pred(x):
            out.append(x)
    return out

var compress = Function(data, selectors):
    var out = []
    var i = 0
    for x in data:
        if i < len(selectors) and selectors[i]:
            out.append(x)
        i = i + 1
    return out

var starmap = Function(func, argtuples):
    var out = []
    for args in argtuples:
        out.append(func(args))
    return out

var pairwise = Function(iterable):
    var out = []
    var prev = None
    var first = True
    for x in iterable:
        if not first:
            out.append([prev, x])
        prev = x
        first = False
    return out

var ziplongest = Function(lists, fillvalue = None):
    var longest = 0
    for lst in lists:
        if len(lst) > longest:
            longest = len(lst)
    var out = []
    for i in range(longest):
        var row = []
        for lst in lists:
            if i < len(lst):
                row.append(lst[i])
            else:
                row.append(fillvalue)
        out.append(row)
    return out

var groupby = Function(iterable, key = None):
    # Group CONSECUTIVE elements sharing a key -> list of [key, [members...]].
    var out = []
    var haveCur = False
    var curKey = None
    var curGroup = []
    for x in iterable:
        var k = x
        if key != None:
            k = key(x)
        if haveCur and k == curKey:
            curGroup.append(x)
        else:
            if haveCur:
                out.append([curKey, curGroup])
            curKey = k
            curGroup = [x]
            haveCur = True
    if haveCur:
        out.append([curKey, curGroup])
    return out
)KI";

// --- functools ---------------------------------------------------------------------------------
inline constexpr std::string_view functools = R"KI(
var reduce = Function(func, iterable, initial = None):
    # `have` tracks whether we hold an accumulator yet — NOT `acc == None`, so a fold that legitimately
    # produces None (reduce(fn, [1,2,3]) where fn returns None) is not mistaken for an empty sequence.
    var acc = initial
    var have = initial != None
    for x in iterable:
        if have:
            acc = func(acc, x)
        else:
            acc = x
            have = True
    if not have:
        throw "reduce of empty sequence with no initial value"
    return acc

var partial = Function(func, bound):
    # partial(f, [a, b]) -> g(rest...) calls f(a, b, rest...). `bound` is a list of leading args,
    # SNAPSHOT at creation (like Python's functools.partial) so a later mutation of the caller's list
    # never leaks into subsequent calls.
    var frozen = bound.copy()
    return Function(rest):
        var allargs = frozen.copy()
        for x in rest:
            allargs.append(x)
        return func(allargs)

var cache = Function(func):
    # Memoize a single-argument function (the argument must be hashable). Returns a wrapper.
    var store = {}
    return Function(x):
        if x not in store:
            store[x] = func(x)
        return store[x]
)KI";

// --- collections: deque, Counter, defaultdict, OrderedDict (as classes) ------------------------
inline constexpr std::string_view collections = R"KI(
class deque:
    var _init_ = Function(self, items = None):
        self._items = []
        if items != None:
            for x in items:
                self._items.append(x)
    var append = Function(self, x):
        self._items.append(x)
    var appendleft = Function(self, x):
        self._items.insert(0, x)
    var pop = Function(self):
        return self._items.pop()
    var popleft = Function(self):
        return self._items.pop(0)
    var _len_ = Function(self) -> Integer:
        return len(self._items)
    var _getitem_ = Function(self, i):
        return self._items[i]
    var _str_ = Function(self) -> String:
        return "deque(" + String(self._items) + ")"
    var _iter_ = Function(self):
        return self._items

class Counter:
    var _init_ = Function(self, items = None):
        self._counts = {}
        if items != None:
            for x in items:
                self.add(x)
    var add = Function(self, x):
        if x in self._counts:
            self._counts[x] = self._counts[x] + 1
        else:
            self._counts[x] = 1
    var get = Function(self, x):
        return self._counts.get(x, 0)
    var items = Function(self):
        return self._counts.items()
    var mostcommon = Function(self, n = None):
        var pairs = sorted(self._counts.items(), Function(p): return p[1], True)
        if n == None:
            return pairs
        return pairs[0:n]
    var _getitem_ = Function(self, x):
        return self._counts.get(x, 0)
    var _str_ = Function(self) -> String:
        return "Counter(" + String(self._counts) + ")"

class defaultdict:
    var _init_ = Function(self, factory):
        self._factory = factory
        self._data = {}
    var _getitem_ = Function(self, k):
        if k not in self._data:
            self._data[k] = self._factory()
        return self._data[k]
    var _setitem_ = Function(self, k, v):
        self._data[k] = v
    var _contains_ = Function(self, k):
        return k in self._data
    var keys = Function(self):
        return self._data.keys()
    var values = Function(self):
        return self._data.values()
    var items = Function(self):
        return self._data.items()
    var _str_ = Function(self) -> String:
        return "defaultdict(" + String(self._data) + ")"
)KI";

// --- statistics --------------------------------------------------------------------------------
inline constexpr std::string_view statistics = R"KI(
var _math = import("math")

var mean = Function(data) -> Float:
    if len(data) == 0:
        throw "mean requires at least one data point"
    return Float(sum(data)) / Float(len(data))

var median = Function(data) -> Float:
    var s = sorted(data)
    var n = len(s)
    if n == 0:
        throw "median requires at least one data point"
    if n % 2 == 1:
        return Float(s[n // 2])
    return (Float(s[n // 2 - 1]) + Float(s[n // 2])) / 2.0

var mode = Function(data):
    if len(data) == 0:
        throw "no mode for empty data"
    var counts = {}
    var best = None
    var bestCount = 0
    for x in data:
        var c = counts.get(x, 0) + 1
        counts[x] = c
        if c > bestCount:
            bestCount = c
            best = x
    return best

var variance = Function(data) -> Float:
    var n = len(data)
    if n < 2:
        throw "variance requires at least two data points"
    var m = mean(data)
    var total = 0.0
    for x in data:
        total = total + (Float(x) - m) * (Float(x) - m)
    return total / Float(n - 1)

var pvariance = Function(data) -> Float:
    var n = len(data)
    if n < 1:
        throw "pvariance requires at least one data point"
    var m = mean(data)
    var total = 0.0
    for x in data:
        total = total + (Float(x) - m) * (Float(x) - m)
    return total / Float(n)

var stdev = Function(data) -> Float:
    return _math.sqrt(variance(data))

var pstdev = Function(data) -> Float:
    return _math.sqrt(pvariance(data))

var multimode = Function(data):
    # All values tied for the highest count, in first-seen order.
    var counts = {}
    var order = []
    var best = 0
    for x in data:
        if x not in counts:
            order.append(x)
        var c = counts.get(x, 0) + 1
        counts[x] = c
        if c > best:
            best = c
    var out = []
    for x in order:
        if counts[x] == best:
            out.append(x)
    return out

var quantiles = Function(data, n = 4):
    # Cut points dividing sorted data into n equal groups (exclusive method, the default).
    var s = sorted(data)
    var ld = len(s)
    if ld < 2:
        throw "quantiles requires at least two data points"
    if n < 1:
        throw "quantiles: n must be at least 1"
    var out = []
    var i = 1
    while i < n:
        var pos = Float(i) * Float(ld + 1) / Float(n)
        var lo = Integer(pos)
        if lo < 1:
            out.append(Float(s[0]))
        elif lo >= ld:
            out.append(Float(s[ld - 1]))
        else:
            var frac = pos - Float(lo)
            out.append(Float(s[lo - 1]) + frac * (Float(s[lo]) - Float(s[lo - 1])))
        i = i + 1
    return out
)KI";

// --- string constants and helpers --------------------------------------------------------------
inline constexpr std::string_view string_mod = R"KI(
var ascii_lowercase = "abcdefghijklmnopqrstuvwxyz"
var ascii_uppercase = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
var ascii_letters = ascii_lowercase + ascii_uppercase
var digits = "0123456789"
var hexdigits = "0123456789abcdefABCDEF"
var octdigits = "01234567"
var punctuation = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"
var whitespace = " \t\n\r"

var capwords = Function(s) -> String:
    var parts = []
    for word in s.split(" "):
        if len(word) > 0:
            parts.append(word[0:1].upper() + word[1:].lower())
        else:
            parts.append(word)
    return " ".join(parts)

# --- fuzzy comparison (built on the native String.levenshtein edit distance) -------------------
# A 0.0..1.0 similarity ratio: 1 - editdistance / longerlength. Two empty strings are identical (1.0).
# similarity(a, b): `a` is a String; `b` is either a String (-> one Float score in [0, 1]) or a List
# of Strings (-> a List of per-candidate scores). The List form runs a single native levenshtein call.
var _ratio = Function(alen, dist, blen) -> Float:
    var longer = alen if alen >= blen else blen
    return 1.0 if longer == 0 else 1.0 - dist / longer
var similarity = Function(a, b):
    if type(b) == "List":
        var dists = a.levenshtein(b)
        var scores = []
        var i = 0
        while i < len(b):
            scores.append(_ratio(len(a), dists[i], len(b[i])))
            i = i + 1
        return scores
    return _ratio(len(a), a.levenshtein(b), len(b))

# The candidate most similar to `query` (smallest edit distance), or None for an empty list. Ties go
# to the earliest candidate. One native call computes every distance at once.
var closest = Function(query, candidates):
    if len(candidates) == 0:
        return None
    var dists = query.levenshtein(candidates)
    var best = 0
    var i = 1
    while i < len(dists):
        if dists[i] < dists[best]:
            best = i
        i = i + 1
    return candidates[best]

# Every candidate whose similarity to `query` is at least `cutoff`, as [candidate, score] pairs sorted
# by score descending (a get-close-matches helper, score-annotated).
var fuzzymatch = Function(query, candidates, cutoff = 0.6):
    var dists = query.levenshtein(candidates)
    var scored = []
    var i = 0
    while i < len(candidates):
        var longer = len(query) if len(query) >= len(candidates[i]) else len(candidates[i])
        var score = 1.0 if longer == 0 else 1.0 - dists[i] / longer
        if score >= cutoff:
            scored.append([candidates[i], score])
        i = i + 1
    scored.sort(Function(p): return p[1], True)   # by score, descending
    return scored
)KI";

// --- textwrap ----------------------------------------------------------------------------------
inline constexpr std::string_view textwrap = R"KI(
var wrap = Function(text, width = 70):
    var words = text.split(" ")
    var lines = []
    var current = ""
    for word in words:
        if len(current) == 0:
            current = word
        elif len(current) + 1 + len(word) <= width:
            current = current + " " + word
        else:
            lines.append(current)
            current = word
    if len(current) > 0:
        lines.append(current)
    return lines

var fill = Function(text, width = 70):
    return "\n".join(wrap(text, width))

var indent = Function(text, prefix) -> String:
    var lines = text.split("\n")
    var out = []
    for line in lines:
        if len(line) > 0:
            out.append(prefix + line)
        else:
            out.append(line)
    return "\n".join(out)

var dedent = Function(text) -> String:
    var lines = text.split("\n")
    var minIndent = None
    for line in lines:
        var stripped = line.lstrip()
        if len(stripped) > 0:
            var ind = len(line) - len(stripped)
            if minIndent == None or ind < minIndent:
                minIndent = ind
    if minIndent == None or minIndent == 0:
        return text
    var out = []
    for line in lines:
        if len(line) >= minIndent:
            out.append(line[minIndent:])
        else:
            out.append(line)
    return "\n".join(out)
)KI";

// --- base64 (standard alphabet) ----------------------------------------------------------------
inline constexpr std::string_view base64 = R"KI(
var _alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

var _index = {}
var _i = 0
while _i < len(_alphabet):
    _index[_alphabet[_i]] = _i
    _i = _i + 1

var encode = Function(data) -> String:
    # data: a List of byte values (0..255), a Bytes, or a String (encoded as UTF-8 bytes).
    # Returns the base64 String.
    if type(data) == "String":
        data = data.encode()
    var out = ""
    var i = 0
    var n = len(data)
    while i < n:
        var b0 = data[i]
        var b1 = 0
        var b2 = 0
        var have = 1
        if i + 1 < n:
            b1 = data[i + 1]
            have = 2
        if i + 2 < n:
            b2 = data[i + 2]
            have = 3
        var triple = b0 * 65536 + b1 * 256 + b2
        out = out + _alphabet[(triple // 262144) % 64]
        out = out + _alphabet[(triple // 4096) % 64]
        if have >= 2:
            out = out + _alphabet[(triple // 64) % 64]
        else:
            out = out + "="
        if have >= 3:
            out = out + _alphabet[triple % 64]
        else:
            out = out + "="
        i = i + 3
    return out

var decode = Function(s):
    # Returns a List of byte values.
    var out = []
    var buffer = 0
    var bits = 0
    for ch in s:
        if ch == "=":
            break
        if ch not in _index:
            throw "invalid base64 character: '" + ch + "'"
        buffer = buffer * 64 + _index[ch]
        bits = bits + 6
        if bits >= 8:
            bits = bits - 8
            out.append((buffer // (2 ** bits)) % 256)
    # A canonical base64 stream leaves no usable bits over: 6 leftover bits means a lone trailing
    # character (an invalid length), and any non-zero leftover bits mean a truncated/corrupted input.
    # Reject both rather than silently dropping data. Padding-less but
    # otherwise-valid input has zero leftover bits and still decodes.
    if bits == 6:
        throw "invalid base64: a lone trailing character (invalid length)"
    if bits > 0 and buffer % (2 ** bits) != 0:
        throw "invalid base64: truncated or corrupted input"
    return out

# URL-safe variant: '+' -> '-', '/' -> '_'. Encodes/decodes by translating to/from the standard form.
var urlsafeencode = Function(data) -> String:
    return encode(data).replace("+", "-").replace("/", "_")

var urlsafedecode = Function(s):
    return decode(s.replace("-", "+").replace("_", "/"))
)KI";

// --- csv (simple, RFC-style quoting) -----------------------------------------------------------
inline constexpr std::string_view csv = R"KI(
var _needsQuote = Function(field) -> Bool:
    return "," in field or "\"" in field or "\n" in field or "\r" in field

var formatrow = Function(fields) -> String:
    var parts = []
    for f in fields:
        var s = String(f)
        if _needsQuote(s):
            s = "\"" + s.replace("\"", "\"\"") + "\""
        parts.append(s)
    return ",".join(parts)

var parserow = Function(line):
    var fields = []
    var current = ""
    var inQuotes = False
    var i = 0
    var n = len(line)
    while i < n:
        var c = line[i]
        if inQuotes:
            if c == "\"":
                if i + 1 < n and line[i + 1] == "\"":
                    current = current + "\""
                    i = i + 1
                else:
                    inQuotes = False
            else:
                current = current + c
        elif c == "\"":
            inQuotes = True
        elif c == ",":
            fields.append(current)
            current = ""
        else:
            current = current + c
        i = i + 1
    fields.append(current)
    return fields

var format = Function(rows) -> String:
    var lines = []
    for row in rows:
        lines.append(formatrow(row))
    return "\n".join(lines)

var parse = Function(text):
    # RFC-4180-aware parse: track quote state across the whole text so a quoted field may contain
    # newlines without being split into separate rows. A `\n` is a row terminator only outside quotes.
    var rows = []
    var fields = []
    var current = ""
    var inQuotes = False
    var i = 0
    var n = len(text)
    while i < n:
        var c = text[i]
        if inQuotes:
            if c == "\"":
                if i + 1 < n and text[i + 1] == "\"":
                    current = current + "\""
                    i = i + 1
                else:
                    inQuotes = False
            else:
                current = current + c
        elif c == "\"":
            inQuotes = True
        elif c == ",":
            fields.append(current)
            current = ""
        elif c == "\n":
            fields.append(current)
            rows.append(fields)
            fields = []
            current = ""
        elif c == "\r":
            pass        # \r is skipped (CRLF treated as LF)
        else:
            current = current + c
        i = i + 1
    if current != "" or len(fields) > 0:
        fields.append(current)
        rows.append(fields)
    return rows
)KI";

// --- heapq (binary min-heap over a List) -------------------------------------------------------
inline constexpr std::string_view heapq = R"KI(
var _siftup = Function(heap, pos):
    var i = pos
    while i > 0:
        var parent = (i - 1) // 2
        if heap[i] < heap[parent]:
            var tmp = heap[i]
            heap[i] = heap[parent]
            heap[parent] = tmp
            i = parent
        else:
            break

var _siftdown = Function(heap):
    var n = len(heap)
    var i = 0
    while True:
        var left = 2 * i + 1
        var right = 2 * i + 2
        var smallest = i
        if left < n and heap[left] < heap[smallest]:
            smallest = left
        if right < n and heap[right] < heap[smallest]:
            smallest = right
        if smallest == i:
            break
        var tmp = heap[i]
        heap[i] = heap[smallest]
        heap[smallest] = tmp
        i = smallest

var heappush = Function(heap, item):
    heap.append(item)
    _siftup(heap, len(heap) - 1)

var heappop = Function(heap):
    var n = len(heap)
    if n == 0:
        throw "heappop from empty heap"
    var top = heap[0]
    var last = heap.pop()
    if len(heap) > 0:
        heap[0] = last
        _siftdown(heap)
    return top

var heapify = Function(items):
    var heap = []
    for x in items:
        heappush(heap, x)
    return heap

var nsmallest = Function(n, items):
    var heap = heapify(items)
    var out = []
    while len(out) < n and len(heap) > 0:
        out.append(heappop(heap))
    return out

var nlargest = Function(n, items):
    if n <= 0:           # match nsmallest: a non-positive n yields [], not a tail slice
        return []
    var s = sorted(items, None, True)
    return s[0:n]

var heapreplace = Function(heap, item):
    # Pop the smallest then push item (more efficient than separate pop+push).
    if len(heap) == 0:
        throw "heapreplace on empty heap"
    var top = heap[0]
    heap[0] = item
    _siftdown(heap)
    return top

var merge = Function(lists):
    # Merge already-sorted lists into one sorted list.
    var heap = []
    for lst in lists:
        for x in lst:
            heappush(heap, x)
    var out = []
    while len(heap) > 0:
        out.append(heappop(heap))
    return out
)KI";

// --- bisect (binary search on a sorted List) ---------------------------------------------------
inline constexpr std::string_view bisect = R"KI(
var bisectleft = Function(a, x) -> Integer:
    var lo = 0
    var hi = len(a)
    while lo < hi:
        var mid = (lo + hi) // 2
        if a[mid] < x:
            lo = mid + 1
        else:
            hi = mid
    return lo

var bisectright = Function(a, x) -> Integer:
    var lo = 0
    var hi = len(a)
    while lo < hi:
        var mid = (lo + hi) // 2
        if x < a[mid]:
            hi = mid
        else:
            lo = mid + 1
    return lo

var insortleft = Function(a, x):
    a.insert(bisectleft(a, x), x)

var insortright = Function(a, x):
    a.insert(bisectright(a, x), x)

# Convenience aliases: bare `bisect`/`insort` mean the right-hand variants.
var bisect = bisectright
var insort = insortright
)KI";

// --- copy (shallow / deep) ---------------------------------------------------------------------
inline constexpr std::string_view copy_mod = R"KI(
# Immutable scalars can be shared safely; copying them is a no-op.
var _IMMUTABLE = Set(["None", "Bool", "Integer", "Float", "String", "Bytes"])

# Pure Kirito has no generic way to enumerate/set an instance's attributes, so a class instance (or a
# native value object like Matrix/Tensor/DateTime) is copied via the serialize graph codec, which
# copies by attributes / the _getstate_/_setstate_ protocol and preserves shared refs + cycles. This
# is necessarily a DEEP, independent copy. A value that can't be serialized (e.g. a live socket/file)
# falls back to itself — best effort, matching the old behaviour for those.
var _copyViaSerde = Function(obj):
    var serialize = import("serialize")
    try:
        return serialize.loads(serialize.dumps(obj))
    catch as e:
        return obj

var copy = Function(obj):
    var t = type(obj)
    if t == "List":
        return obj.copy()
    if t == "Dict":
        return obj.copy()
    if t == "Set":
        return obj.copy()
    if t in _IMMUTABLE:
        return obj
    return _copyViaSerde(obj)        # user instance / native value object: independent (deep) copy

var deepcopy = Function(obj):
    var t = type(obj)
    if t in _IMMUTABLE:
        return obj
    if t != "List" and t != "Dict" and t != "Set":
        return _copyViaSerde(obj)    # instance / native value object — serde handles refs + cycles
    # Iterative + cycle-safe (recursion would overflow / loop on deep or self-referential data):
    # 1) discover every reachable container and give it an empty shell, keyed by id(original);
    # 2) fill the shells, mapping each child to its shell (or itself for scalars). Shared references
    #    and cycles are preserved because each original maps to exactly one shell.
    var memo = {}
    var order = []
    var stack = [obj]
    while len(stack) > 0:
        var cur = stack.pop()
        var cid = id(cur)
        if cid in memo:
            continue
        var ct = type(cur)
        if ct == "List":
            memo[cid] = []
        elif ct == "Dict":
            memo[cid] = {}
        elif ct == "Set":
            memo[cid] = Set()
        elif ct in _IMMUTABLE:
            continue                      # scalar: mapped() returns it unchanged
        else:
            memo[cid] = _copyViaSerde(cur)  # user instance / native value: independent deep copy
            continue                      # serde copied it whole — no shell to fill, don't descend
        order.append(cur)
        if ct == "Dict":
            for pair in cur.items():
                stack.append(pair[0])
                stack.append(pair[1])
        else:
            for x in cur:
                stack.append(x)
    var mapped = Function(x):
        var xid = id(x)
        return memo[xid] if xid in memo else x
    for cur in order:
        var ct = type(cur)
        var shell = memo[id(cur)]
        if ct == "List":
            for x in cur:
                shell.append(mapped(x))
        elif ct == "Set":
            for x in cur:
                shell.add(mapped(x))
        else:
            for pair in cur.items():
                shell[mapped(pair[0])] = mapped(pair[1])
    return memo[id(obj)]
)KI";

// --- enum (a tiny enum factory) ----------------------------------------------------------------
inline constexpr std::string_view enum_mod = R"KI(
class Enum:
    # Enum(["RED", "GREEN", "BLUE"]) -> members accessible as e.get("RED") -> 0, with names()/values().
    var _init_ = Function(self, names):
        self._byName = {}
        self._byValue = {}
        self._order = []                 # definition order (a Dict's keys() is unordered)
        var i = 0
        for name in names:
            self._byName[name] = i
            self._byValue[i] = name
            self._order.append(name)
            i = i + 1
    var get = Function(self, name):
        if name not in self._byName:
            throw "no such enum member: " + name
        return self._byName[name]
    var nameof = Function(self, value):
        return self._byValue[value]
    var names = Function(self):
        return self._order.copy()        # in definition order, not hash order
    var values = Function(self):
        var out = []
        for name in self._order:
            out.append(self._byName[name])
        return out
    var _getitem_ = Function(self, name):
        return self.get(name)
    var _contains_ = Function(self, name):
        return name in self._byName
)KI";

// --- tee: fan-out streams — clone output (e.g. stdout) into extra streams; context-manager hooks --
inline constexpr std::string_view tee = R"KI(
# tee — fan-out streams. A Tee writes every chunk to one or more "copy" streams (e.g. a log file)
# *before* passing it to the primary stream (e.g. the original stdout) — so output is saved before
# being handed on. It implements the stream `write`/`writelines`/`flush` protocol, so it can be
# assigned to io.stdout / io.stderr, and it works as a context manager (flushes on exit).
#
# The convenience context managers `tee_stdout` / `tee_stderr` hook the std streams for you:
#
#   var io = import("io")
#   var tee = import("tee")
#   with io.open("session.log", "w") as f:
#       with tee.tee_stdout(f):
#           io.print("appears on the console AND in session.log")
#   # stdout is restored here

var _io = import("io")

# Flush a stream if it supports it (files / BytesIO / std streams do); ignore if it doesn't.
var _maybeFlush = Function(s):
    if s == None:
        return None
    try:
        s.flush()
    catch as e:
        pass
    return None

class Tee:
    # Tee(primary, copies=None): writes go to each copy first, then to `primary`. `copies` may be a
    # single stream or a List of streams. `primary` may be None to make a pure fan-out sink.
    var _init_ = Function(self, primary, copies = None):
        self.primary = primary
        var cs = []
        if copies != None:
            if isinstance(copies, "List"):
                for s in copies:
                    cs.append(s)
            else:
                cs.append(copies)
        self.copies = cs

    # All underlying streams, in write order (copies first, then the primary).
    var streams = Function(self):
        var all = []
        for s in self.copies:
            all.append(s)
        if self.primary != None:
            all.append(self.primary)
        return all

    var write = Function(self, data):
        for s in self.copies:
            s.write(data)
        if self.primary != None:
            self.primary.write(data)
        return len(data)

    var writelines = Function(self, lines):
        for line in lines:
            self.write(line)
        return None

    var flush = Function(self):
        for s in self.copies:
            _maybeFlush(s)
        _maybeFlush(self.primary)
        return None

    var close = Function(self):
        self.flush()
        return None

    var _enter_ = Function(self):
        return self

    var _exit_ = Function(self):
        self.flush()
        return None

# Context manager: within the block, io.stdout also writes to `copies` (a stream or List). The
# original stdout is saved on enter and restored on exit (the copy streams are never closed —
# the caller owns them).
class tee_stdout:
    var _init_ = Function(self, copies):
        self._copies = copies
        self._saved = None

    var _enter_ = Function(self):
        self._saved = _io.stdout
        _io.stdout = Tee(self._saved, self._copies)
        return _io.stdout

    var _exit_ = Function(self):
        _io.stdout.flush()
        _io.stdout = self._saved
        return None

# Same, for io.stderr.
class tee_stderr:
    var _init_ = Function(self, copies):
        self._copies = copies
        self._saved = None

    var _enter_ = Function(self):
        self._saved = _io.stderr
        _io.stderr = Tee(self._saved, self._copies)
        return _io.stderr

    var _exit_ = Function(self):
        _io.stderr.flush()
        _io.stderr = self._saved
        return None
)KI";

// `arg` — a small argparse-style command-line parser. Build a Parser, declare positionals/options/
// flags, then parse a list of arguments (typically `arglist`) into a Dict. Option values are
// converted to the type of their default (Integer/Float, else String). `parse` returns None when
// -h/--help is given (after printing usage), so a program can stop; it throws a clear, catchable
// error on an unknown option, a missing required positional, or a bad numeric value.
inline constexpr std::string_view arg = R"KI(
var _io = import("io")

class Parser:
    var _init_ = Function(self, description = ""):
        self._desc = description
        self._positionals = []    # each: [name, help]
        self._options = []        # each: [name, default, help]
        self._flags = []          # each: [name, help]

    # --- declaration (chainable: each returns self) ---
    var positional = Function(self, name, help = ""):
        self._positionals.append([name, help])
        return self
    var option = Function(self, name, default = None, help = ""):
        self._options.append([name, default, help])
        return self
    var flag = Function(self, name, help = ""):
        self._flags.append([name, help])
        return self

    var _isflag = Function(self, name) -> Bool:
        for f in self._flags:
            if f[0] == name:
                return True
        return False
    var _isoption = Function(self, name) -> Bool:
        for o in self._options:
            if o[0] == name:
                return True
        return False
    var _byshort = Function(self, ch):
        # a short option -x matches the first letter of a flag or option name
        for f in self._flags:
            if f[0][0] == ch:
                return ["flag", f[0]]
        for o in self._options:
            if o[0][0] == ch:
                return ["option", o[0]]
        return None
    var _convert = Function(self, name, value):
        for o in self._options:
            if o[0] == name:
                if isinstance(o[1], "Integer"):
                    try:
                        return Integer(value)
                    catch as e:
                        throw "option --" + name + " expects an integer, got '" + value + "'"
                if isinstance(o[1], "Float"):
                    try:
                        return Float(value)
                    catch as e:
                        throw "option --" + name + " expects a number, got '" + value + "'"
                return value
        return value

    var usage = Function(self) -> String:
        var head = ["usage:"]
        for p in self._positionals:
            head.append("<" + p[0] + ">")
        for o in self._options:
            head.append("[--" + o[0] + " VALUE]")
        for f in self._flags:
            head.append("[--" + f[0] + "]")
        var lines = [" ".join(head)]
        if self._desc != "":
            lines.append("")
            lines.append(self._desc)
        lines.append("")
        lines.append("arguments:")
        for p in self._positionals:
            lines.append("  <" + p[0] + ">  " + p[1])
        for o in self._options:
            lines.append("  --" + o[0] + " VALUE  (default " + String(o[1]) + ")  " + o[2])
        for f in self._flags:
            lines.append("  --" + f[0] + "  " + f[1])
        lines.append("  -h, --help  show this help and stop")
        return "\n".join(lines)

    # Parse `args` (a list of Strings, e.g. arglist) -> a Dict of results, or None if -h/--help given.
    var parse = Function(self, args):
        var result = {}
        for o in self._options:
            result[o[0]] = o[1]
        for f in self._flags:
            result[f[0]] = False
        var positionals = []
        var i = 0
        while i < len(args):
            var token = args[i]
            if token == "-h" or token == "--help":
                _io.print(self.usage())
                return None
            if token.startswith("--"):
                var body = token[2:]
                var name = body
                var inline = None
                if "=" in body:
                    var eq = body.find("=")
                    name = body[0:eq]
                    inline = body[eq + 1:]
                if self._isflag(name):
                    result[name] = True
                elif self._isoption(name):
                    var value = inline
                    if value == None:
                        i = i + 1
                        if i >= len(args):
                            throw "option --" + name + " requires a value"
                        value = args[i]
                    result[name] = self._convert(name, value)
                else:
                    throw "unknown option: --" + name
            elif token.startswith("-") and len(token) == 2 and token[1:].isalpha():
                # A `-x` where x is a LETTER is a short option (a `-5`/`-2` stays a positional so a
                # negative number can be a positional argument). An unknown letter is a hard error, not
                # silently swallowed as a positional.
                var matched = self._byshort(token[1:])
                if matched == None:
                    throw "unknown option: -" + token[1:]
                if matched[0] == "flag":
                    result[matched[1]] = True
                else:
                    i = i + 1
                    if i >= len(args):
                        throw "option " + token + " requires a value"
                    result[matched[1]] = self._convert(matched[1], args[i])
            else:
                positionals.append(token)
            i = i + 1
        if len(positionals) < len(self._positionals):
            throw "missing required argument: <" + self._positionals[len(positionals)][0] + ">"
        var pi = 0
        for p in self._positionals:
            result[p[0]] = positionals[pi]
            pi = pi + 1
        var rest = []
        while pi < len(positionals):
            rest.append(positionals[pi])
            pi = pi + 1
        result["rest"] = rest
        return result
)KI";

// --- tabular: labelled Series/DataFrame with CSV I/O, indexing, aggregation, group-by, joins ---
inline constexpr std::string_view tabular = R"KI(
# A dataframe-style (pandas-like) data-analysis library in pure Kirito: labelled 1-D `Series` and
# 2-D `DataFrame`,
# with construction from columns/rows/CSV, label- and position-based indexing (loc/iloc), boolean
# masking, element-wise arithmetic and comparisons, aggregations, group-by, sorting, joins,
# concatenation, missing-data handling, and CSV round-tripping. Public names follow Kirito's
# lowercase-no-underscore convention (sortvalues, readcsv, valuecounts, resetindex, ...).

var _csv = import("csv")

# Aliases for builtins that share a name with a Series/DataFrame method (min/max). Inside a
# method a bare `min` resolves to the sibling method (class-body scope), so capture the
# builtins here at module scope where no such method exists.
var _min = min
var _max = max

# ----------------------------------------------------------------------------- helpers (private)
var _isnan = Function(x) -> Bool:
    # a missing value is None; also treat a Float NaN (x != x) as missing
    if x == None:
        return True
    if isinstance(x, "Float") and x != x:
        return True
    return False

var _numeric = Function(values):
    # the non-missing numeric values; Bool counts as 0/1 (like pandas)
    var out = []
    for v in values:
        if not _isnan(v):
            if isinstance(v, "Bool"):
                out.append(1 if v else 0)
            elif isinstance(v, "Integer") or isinstance(v, "Float"):
                out.append(v)
    return out

# True iff a column has at least one non-missing value and every non-missing value is numeric
# (Integer/Float/Bool) — used to pick which columns participate in frame/group reductions.
var _isnumericcol = Function(values) -> Bool:
    var seen = False
    for v in values:
        if not _isnan(v):
            if isinstance(v, "Integer") or isinstance(v, "Float") or isinstance(v, "Bool"):
                seen = True
            else:
                return False
    return seen

var _range = Function(n):
    var out = []
    var i = 0
    while i < n:
        out.append(i)
        i = i + 1
    return out

# A cell looks numeric only in a PLAIN decimal form. Integer()/Float() also accept 0x/0o/0b base
# prefixes and surrounding whitespace, which would silently convert a CSV cell that should stay text
# (a hex-looking id like "0x1F", or a whitespace-padded field) — so gate inference on this first.
var _looksnumeric = Function(text):
    if text != text.strip():
        return False
    var b = text
    if b.startswith("-") or b.startswith("+"):
        b = b[1:]
    if len(b) == 0:
        return False
    if b.startswith("0x") or b.startswith("0X") or b.startswith("0o") or b.startswith("0O") or b.startswith("0b") or b.startswith("0B"):
        return False
    return True

var _infer = Function(text):
    # turn a CSV cell string into Integer / Float / Bool / None / String
    if text == "":
        return None
    if text == "True" or text == "true":
        return True
    if text == "False" or text == "false":
        return False
    if not _looksnumeric(text):
        return text
    try:
        return Integer(text)
    catch as e:
        discard e
    try:
        return Float(text)
    catch as e:
        discard e
    return text

# ===================================================================================== Series
class Series:
    var _init_ = Function(self, values, index = None, name = None):
        self.values = List(values)
        if index == None:
            self.index = _range(len(self.values))
        else:
            self.index = List(index)
        if len(self.index) != len(self.values):
            throw "Series: index length does not match values length"
        self.name = name

    var _len_ = Function(self) -> Integer:
        return len(self.values)

    var _iter_ = Function(self):
        return self.values

    # by label first (if the label exists in the index), else by position
    var _getitem_ = Function(self, key):
        var pos = self.index.index(key) if key in self.index else key
        return self.values[pos]

    var _setitem_ = Function(self, key, value):
        var pos = self.index.index(key) if key in self.index else key
        self.values[pos] = value

    var iat = Function(self, pos):
        return self.values[pos]

    var tolist = Function(self):
        return List(self.values)

    var copy = Function(self):
        return Series(List(self.values), List(self.index), self.name)

    # --- element-wise arithmetic (Series-Series aligned by position, or Series-scalar) ---
    var _binop = Function(self, other, op):
        var out = []
        if isinstance(other, "Series"):
            if len(self.values) != len(other.values):
                throw "Series: length mismatch (" + String(len(self.values)) + " vs " + String(len(other.values)) + ")"
            var i = 0
            while i < len(self.values):
                out.append(op(self.values[i], other.values[i]))
                i = i + 1
        else:
            for v in self.values:
                out.append(op(v, other))
        return Series(out, List(self.index), self.name)

    var _add_ = Function(self, other):
        return self._binop(other, Function(a, b): return a + b)
    var _sub_ = Function(self, other):
        return self._binop(other, Function(a, b): return a - b)
    var _mul_ = Function(self, other):
        return self._binop(other, Function(a, b): return a * b)
    var _div_ = Function(self, other):
        return self._binop(other, Function(a, b): return a / b)
    var _floordiv_ = Function(self, other):
        return self._binop(other, Function(a, b): return a // b)
    var _mod_ = Function(self, other):
        return self._binop(other, Function(a, b): return a % b)

    # --- comparisons produce a boolean Series (for masking) ---
    var _gt_ = Function(self, other):
        return self._binop(other, Function(a, b): return a > b)
    var _ge_ = Function(self, other):
        return self._binop(other, Function(a, b): return a >= b)
    var _lt_ = Function(self, other):
        return self._binop(other, Function(a, b): return a < b)
    var _le_ = Function(self, other):
        return self._binop(other, Function(a, b): return a <= b)

    var eq = Function(self, other):
        return self._binop(other, Function(a, b): return a == b)
    var ne = Function(self, other):
        return self._binop(other, Function(a, b): return a != b)
    # `==`/`!=` are element-wise like `<`/`>` (and pandas), so `df[df["c"] == v]` masks rows. (Note
    # the pandas caveat this inherits: a Series can no longer be used directly in a boolean `if`.)
    var _eq_ = Function(self, other):
        return self._binop(other, Function(a, b): return a == b)
    var _ne_ = Function(self, other):
        return self._binop(other, Function(a, b): return a != b)

    var isin = Function(self, values):
        var out = []
        for v in self.values:
            out.append(v in values)
        return Series(out, List(self.index), self.name)

    # --- aggregations (skip missing) ---
    var sum = Function(self):
        var total = 0
        for v in _numeric(self.values):
            total = total + v
        return total
    var count = Function(self) -> Integer:
        # non-null tally of ANY type (like pandas), not just numeric values
        var c = 0
        for v in self.values:
            if not _isnan(v):
                c = c + 1
        return c
    var mean = Function(self):
        var nums = _numeric(self.values)
        if len(nums) == 0:
            return None
        return self.sum() / len(nums)
    var min = Function(self):
        var nums = _numeric(self.values)
        return _min(nums) if len(nums) > 0 else None
    var max = Function(self):
        var nums = _numeric(self.values)
        return _max(nums) if len(nums) > 0 else None
    var median = Function(self):
        var nums = sorted(_numeric(self.values))
        var n = len(nums)
        if n == 0:
            return None
        if n % 2 == 1:
            return nums[n // 2]
        return (nums[n // 2 - 1] + nums[n // 2]) / 2
    var variance = Function(self):
        var nums = _numeric(self.values)
        var n = len(nums)
        if n < 2:
            return None
        var m = self.mean()
        var acc = 0.0
        for v in nums:
            acc = acc + (v - m) * (v - m)
        return acc / (n - 1)
    var std = Function(self):
        var v = self.variance()
        return v ** 0.5 if v != None else None
    var prod = Function(self):
        var p = 1
        for v in _numeric(self.values):
            p = p * v
        return p

    var unique = Function(self):
        var seen = []
        for v in self.values:
            if v not in seen:
                seen.append(v)
        return seen

    var nunique = Function(self) -> Integer:
        return len(self.unique())

    var valuecounts = Function(self):
        # a Series of counts indexed by value, descending by count
        var counts = {}
        for v in self.values:
            if _isnan(v):
                continue
            counts[v] = counts.get(v, 0) + 1
        var items = []
        for k in counts.keys():
            items.append([k, counts[k]])
        items.sort(Function(p): return p[1], True)
        var idx = []
        var cnt = []
        for p in items:
            idx.append(p[0])
            cnt.append(p[1])
        return Series(cnt, idx, "count")

    var apply = Function(self, fn):
        var out = []
        for v in self.values:
            out.append(fn(v))
        return Series(out, List(self.index), self.name)
    var map = Function(self, fn):
        return self.apply(fn)

    var astype = Function(self, typename):
        var conv = String
        if typename == "Integer":
            conv = Integer
        elif typename == "Float":
            conv = Float
        elif typename == "Bool":
            conv = Bool
        return self.apply(conv)

    var fillna = Function(self, value):
        var out = []
        for v in self.values:
            out.append(value if _isnan(v) else v)
        return Series(out, List(self.index), self.name)

    var dropna = Function(self):
        var out = []
        var idx = []
        var i = 0
        while i < len(self.values):
            if not _isnan(self.values[i]):
                out.append(self.values[i])
                idx.append(self.index[i])
            i = i + 1
        return Series(out, idx, self.name)

    var head = Function(self, n = 5):
        return Series(self.values[0:n], self.index[0:n], self.name)
    var tail = Function(self, n = 5):
        var start = len(self.values) - n
        if start < 0:
            start = 0
        return Series(self.values[start:], self.index[start:], self.name)

    var sortvalues = Function(self, ascending = True):
        var pairs = []
        var i = 0
        while i < len(self.values):
            pairs.append([self.values[i], self.index[i]])
            i = i + 1
        pairs.sort(Function(p): return p[0], not ascending)
        var vals = []
        var idx = []
        for p in pairs:
            vals.append(p[0])
            idx.append(p[1])
        return Series(vals, idx, self.name)

    var resetindex = Function(self):
        return Series(List(self.values), _range(len(self.values)), self.name)

    var _str_ = Function(self) -> String:
        var lines = []
        var i = 0
        while i < len(self.values):
            lines.append(String(self.index[i]) + "    " + String(self.values[i]))
            i = i + 1
        if self.name != None:
            lines.append("Name: " + String(self.name))
        return "\n".join(lines)

# =========================================================================== loc / iloc helpers
# Position-based row access: frame.iloc[i] -> row as a Series (indexed by column); frame.iloc[[i,j]]
# -> a DataFrame of those rows.
class _Iloc:
    var _init_ = Function(self, frame):
        self.frame = frame
    var _getitem_ = Function(self, key):
        if isinstance(key, "List"):
            return self.frame.rowsat(key)
        return self.frame.rowat(key)

# Label-based row access: frame.loc[label] -> row as a Series; frame.loc[[l1, l2]] -> DataFrame.
class _Loc:
    var _init_ = Function(self, frame):
        self.frame = frame
    var _getitem_ = Function(self, key):
        if isinstance(key, "List"):
            var positions = []
            for label in key:
                positions.append(self.frame.index.index(label))
            return self.frame.rowsat(positions)
        return self.frame.rowat(self.frame.index.index(key))

# ===================================================================================== DataFrame
class DataFrame:
    var _init_ = Function(self, data = None, columns = None, index = None):
        self.columns = []
        self.data = {}
        if data == None:
            self.index = [] if index == None else List(index)
        elif isinstance(data, "Dict"):
            self._fromcolumns(data, columns)
            self.index = _range(self.nrows()) if index == None else List(index)
        elif isinstance(data, "List"):
            self._fromrows(data, columns)
            self.index = _range(self.nrows()) if index == None else List(index)
        else:
            throw "DataFrame: data must be a Dict of columns or a List of rows"
        self.iloc = _Iloc(self)
        self.loc = _Loc(self)

    var _fromcolumns = Function(self, data, columns):
        var cols = columns if columns != None else data.keys()
        var length = None
        for c in cols:
            var col = List(data[c])
            if length == None:
                length = len(col)
            elif len(col) != length:
                throw "DataFrame: all columns must have the same length"
            self.columns.append(c)
            self.data[c] = col

    var _fromrows = Function(self, rows, columns):
        if len(rows) == 0:
            if columns != None:
                for c in columns:
                    self.columns.append(c)
                    self.data[c] = []
            return
        if isinstance(rows[0], "Dict"):
            # union of keys, first-seen order (or the given column order)
            var cols = columns
            if cols == None:
                cols = []
                for r in rows:
                    for k in r.keys():
                        if k not in cols:
                            cols.append(k)
            for c in cols:
                self.columns.append(c)
                var col = []
                for r in rows:
                    col.append(r.get(c, None))
                self.data[c] = col
        else:
            var ncols = len(rows[0])
            var cols = columns if columns != None else _defaultcols(ncols)
            var ci = 0
            while ci < len(cols):
                var col = []
                for r in rows:
                    col.append(r[ci])
                self.columns.append(cols[ci])
                self.data[cols[ci]] = col
                ci = ci + 1

    var nrows = Function(self) -> Integer:
        if len(self.columns) == 0:
            return 0
        return len(self.data[self.columns[0]])

    var _len_ = Function(self) -> Integer:
        return self.nrows()

    var shape = Function(self):
        return [self.nrows(), len(self.columns)]

    var rowat = Function(self, pos):
        var vals = []
        for c in self.columns:
            vals.append(self.data[c][pos])
        return Series(vals, List(self.columns), self.index[pos])

    var rowsat = Function(self, positions):
        var newdata = {}
        for c in self.columns:
            var col = []
            for p in positions:
                col.append(self.data[c][p])
            newdata[c] = col
        var newindex = []
        for p in positions:
            newindex.append(self.index[p])
        return DataFrame(newdata, List(self.columns), newindex)

    # --- selection: column (String), column-subset (List of String), or row mask (boolean) ---
    var _getitem_ = Function(self, key):
        if isinstance(key, "Series"):
            return self._mask(key.values)
        if isinstance(key, "List"):
            if len(key) > 0 and isinstance(key[0], "Bool"):
                return self._mask(key)
            return self._subset(key)
        return Series(List(self.data[key]), List(self.index), key)

    var _setitem_ = Function(self, key, value):
        var col = []
        if isinstance(value, "Series"):
            col = List(value.values)
        elif isinstance(value, "List"):
            col = List(value)
        else:
            var i = 0
            while i < self.nrows():
                col.append(value)
                i = i + 1
        if len(col) != self.nrows() and self.nrows() > 0:
            throw "DataFrame: new column length must match row count"
        if key not in self.columns:
            self.columns.append(key)
        self.data[key] = col
        if len(self.index) != self.nrows():     # adding the first column to an empty frame: sync the index
            self.index = _range(self.nrows())

    var _mask = Function(self, flags):
        if len(flags) != self.nrows():           # a wrong-length boolean mask must not silently drop rows
            throw "boolean mask length does not match row count"
        var positions = []
        var i = 0
        while i < len(flags):
            if flags[i]:
                positions.append(i)
            i = i + 1
        return self.rowsat(positions)

    var _subset = Function(self, cols):
        var newdata = {}
        for c in cols:
            newdata[c] = List(self.data[c])
        return DataFrame(newdata, List(cols), List(self.index))

    var column = Function(self, name):
        return Series(List(self.data[name]), List(self.index), name)

    var at = Function(self, label, col):
        return self.data[col][self.index.index(label)]
    var iat = Function(self, pos, col):
        return self.data[self.columns[col]][pos]

    var head = Function(self, n = 5):
        return self.rowsat(_range(n if n < self.nrows() else self.nrows()))
    var tail = Function(self, n = 5):
        var total = self.nrows()
        var start = total - n
        if start < 0:
            start = 0
        var positions = []
        var i = start
        while i < total:
            positions.append(i)
            i = i + 1
        return self.rowsat(positions)
    var slice = Function(self, start, stop):
        var positions = []
        var i = start
        while i < stop:
            positions.append(i)
            i = i + 1
        return self.rowsat(positions)

    var copy = Function(self):
        var newdata = {}
        for c in self.columns:
            newdata[c] = List(self.data[c])
        return DataFrame(newdata, List(self.columns), List(self.index))

    var rename = Function(self, columns):
        var newcols = []
        var newdata = {}
        for c in self.columns:
            var nc = columns.get(c, c)
            newcols.append(nc)
            newdata[nc] = List(self.data[c])
        return DataFrame(newdata, newcols, List(self.index))

    var drop = Function(self, columns):
        var keep = []
        for c in self.columns:
            if c not in columns:
                keep.append(c)
        return self._subset(keep)

    var assign = Function(self, name, value):
        var out = self.copy()
        out[name] = value
        return out

    var resetindex = Function(self):
        return DataFrame(self._datacopy(), List(self.columns), _range(self.nrows()))

    var setindex = Function(self, col):
        var newindex = List(self.data[col])
        var out = self.drop([col])
        return DataFrame(out._datacopy(), List(out.columns), newindex)

    var _datacopy = Function(self):
        var newdata = {}
        for c in self.columns:
            newdata[c] = List(self.data[c])
        return newdata

    # --- per-column numeric aggregations -> a Series indexed by column name ---
    var _aggcols = Function(self, fn):
        var idx = []
        var vals = []
        for c in self.columns:
            if _isnumericcol(self.data[c]):
                idx.append(c)
                vals.append(fn(Series(self.data[c])))
        return Series(vals, idx, None)
    var sum = Function(self):
        return self._aggcols(Function(s): return s.sum())
    var mean = Function(self):
        return self._aggcols(Function(s): return s.mean())
    var min = Function(self):
        return self._aggcols(Function(s): return s.min())
    var max = Function(self):
        return self._aggcols(Function(s): return s.max())
    var std = Function(self):
        return self._aggcols(Function(s): return s.std())
    var count = Function(self):
        var idx = []
        var vals = []
        for c in self.columns:
            idx.append(c)
            vals.append(Series(self.data[c]).count())
        return Series(vals, idx, None)

    var describe = Function(self):
        # count/mean/std/min/median/max for every numeric column -> a DataFrame
        var stats = ["count", "mean", "std", "min", "median", "max"]
        var newdata = {}
        var numcols = []
        for c in self.columns:
            if _isnumericcol(self.data[c]):
                var s = Series(self.data[c])
                numcols.append(c)
                newdata[c] = [s.count(), s.mean(), s.std(), s.min(), s.median(), s.max()]
        return DataFrame(newdata, numcols, stats)

    var sortvalues = Function(self, by, ascending = True):
        var positions = _range(self.nrows())
        var col = self.data[by]
        positions.sort(Function(p): return col[p], not ascending)
        return self.rowsat(positions)

    var apply = Function(self, fn):
        # apply fn to each column Series -> a Series of results (axis=columns reduction)
        return self._aggcols(fn)

    var iterrows = Function(self):
        var out = []
        var i = 0
        while i < self.nrows():
            out.append([self.index[i], self.rowat(i)])
            i = i + 1
        return out

    var todict = Function(self):
        var out = {}
        for c in self.columns:
            out[c] = List(self.data[c])
        return out

    var torows = Function(self):
        var rows = []
        var i = 0
        while i < self.nrows():
            var row = {}
            for c in self.columns:
                row[c] = self.data[c][i]
            rows.append(row)
            i = i + 1
        return rows

    # --- missing data ---
    var dropna = Function(self):
        var positions = []
        var i = 0
        while i < self.nrows():
            var keep = True
            for c in self.columns:
                if _isnan(self.data[c][i]):
                    keep = False
            if keep:
                positions.append(i)
            i = i + 1
        return self.rowsat(positions)
    var fillna = Function(self, value):
        var newdata = {}
        for c in self.columns:
            var col = []
            for v in self.data[c]:
                col.append(value if _isnan(v) else v)
            newdata[c] = col
        return DataFrame(newdata, List(self.columns), List(self.index))

    var groupby = Function(self, by):
        return GroupBy(self, by)

    var merge = Function(self, other, on, how = "inner"):
        return _merge(self, other, on, how)

    var tocsv = Function(self):
        var rows = [List(self.columns)]
        var i = 0
        while i < self.nrows():
            var row = []
            for c in self.columns:
                var v = self.data[c][i]
                row.append("" if v == None else String(v))
            rows.append(row)
            i = i + 1
        return _csv.format(rows)

    var _str_ = Function(self) -> String:
        var widths = {}
        for c in self.columns:
            widths[c] = len(String(c))
        var i = 0
        while i < self.nrows():
            for c in self.columns:
                var w = len(String(self.data[c][i]))
                if w > widths[c]:
                    widths[c] = w
            i = i + 1
        var idxw = len("index")
        for label in self.index:
            if len(String(label)) > idxw:
                idxw = len(String(label))
        var header = "".ljust(idxw)
        for c in self.columns:
            header = header + "  " + String(c).rjust(widths[c])
        var lines = [header]
        i = 0
        while i < self.nrows():
            var line = String(self.index[i]).ljust(idxw)
            for c in self.columns:
                var v = self.data[c][i]
                line = line + "  " + ("" if v == None else String(v)).rjust(widths[c])
            lines.append(line)
            i = i + 1
        return "\n".join(lines)

# ===================================================================================== GroupBy
class GroupBy:
    var _init_ = Function(self, frame, by):
        self.frame = frame
        self.by = by
        # ordered group keys + the row positions in each group
        self.keys = []
        self.groups = {}
        var col = frame.data[by]
        var i = 0
        while i < frame.nrows():
            var k = col[i]
            if k not in self.groups:
                self.keys.append(k)
                self.groups[k] = []
            self.groups[k].append(i)
            i = i + 1

    var _valuecols = Function(self):
        var out = []
        for c in self.frame.columns:
            if c != self.by:
                out.append(c)
        return out

    # apply a Series-reduction to every value column of every group -> a DataFrame indexed by key
    var _reduce = Function(self, fn):
        var valcols = []
        for c in self._valuecols():
            if _isnumericcol(self.frame.data[c]):
                valcols.append(c)
        var newdata = {}
        for c in valcols:
            newdata[c] = []
            for k in self.keys:
                var vals = []
                for p in self.groups[k]:
                    vals.append(self.frame.data[c][p])
                newdata[c].append(fn(Series(vals)))
        return DataFrame(newdata, valcols, List(self.keys))

    var sum = Function(self):
        return self._reduce(Function(s): return s.sum())
    var mean = Function(self):
        return self._reduce(Function(s): return s.mean())
    var min = Function(self):
        return self._reduce(Function(s): return s.min())
    var max = Function(self):
        return self._reduce(Function(s): return s.max())
    var std = Function(self):
        return self._reduce(Function(s): return s.std())
    var count = Function(self):
        var valcols = self._valuecols()
        var newdata = {}
        for c in valcols:
            newdata[c] = []
            for k in self.keys:
                newdata[c].append(len(self.groups[k]))
        return DataFrame(newdata, valcols, List(self.keys))

    var size = Function(self):
        var vals = []
        for k in self.keys:
            vals.append(len(self.groups[k]))
        return Series(vals, List(self.keys), "size")

    # agg(dict): {column: reduction-name} -> a DataFrame; reduction is one of sum/mean/min/max/std/count
    var agg = Function(self, spec):
        var reducers = {"sum": Function(s): return s.sum(), "mean": Function(s): return s.mean(),
                        "min": Function(s): return s.min(), "max": Function(s): return s.max(),
                        "std": Function(s): return s.std(), "count": Function(s): return s.count(),
                        "median": Function(s): return s.median()}
        var cols = []
        var newdata = {}
        for c in spec.keys():
            cols.append(c)
            newdata[c] = []
        for k in self.keys:
            var positions = self.groups[k]
            for c in spec.keys():
                var vals = []
                for p in positions:
                    vals.append(self.frame.data[c][p])
                newdata[c].append(reducers[spec[c]](Series(vals)))
        return DataFrame(newdata, cols, List(self.keys))

    var apply = Function(self, fn):
        # fn receives the sub-DataFrame for each group -> a Series indexed by key
        var vals = []
        for k in self.keys:
            vals.append(fn(self.frame.rowsat(self.groups[k])))
        return Series(vals, List(self.keys), None)

# ===================================================================================== module funcs
var _defaultcols = Function(n):
    var out = []
    var i = 0
    while i < n:
        out.append("col" + String(i))
        i = i + 1
    return out

var _merge = Function(left, right, on, how):
    # validate `how` here so BOTH entry points (DataFrame.merge and module-level merge) reject a bad join
    if how != "inner" and how != "left" and how != "right" and how != "outer":
        throw "merge: how must be one of inner/left/right/outer, got '" + how + "'"
    # build an index of right rows by join key
    var rightidx = {}
    var rpos = 0
    while rpos < right.nrows():
        var k = right.data[on][rpos]
        if k not in rightidx:
            rightidx[k] = []
        rightidx[k].append(rpos)
        rpos = rpos + 1
    var leftcols = left.columns
    var rightvalcols = []
    for c in right.columns:
        if c != on:
            rightvalcols.append(c)
    # A non-key column present in BOTH frames would collide on one output name; suffix _x/_y (pandas-style).
    var lset = {}
    for c in leftcols:
        lset[c] = True
    var rset = {}
    for c in rightvalcols:
        rset[c] = True
    var leftout = []
    for c in leftcols:
        leftout.append(c + "_x" if c != on and c in rset else c)
    var rightout = []
    for c in rightvalcols:
        rightout.append(c + "_y" if c in lset else c)
    var outcols = []
    for c in leftout:
        outcols.append(c)
    for c in rightout:
        outcols.append(c)
    var newdata = {}
    for c in outcols:
        newdata[c] = []

    var emitrow = Function(lp, rp):
        var li = 0
        while li < len(leftcols):
            newdata[leftout[li]].append(left.data[leftcols[li]][lp])
            li = li + 1
        var ri = 0
        while ri < len(rightvalcols):
            newdata[rightout[ri]].append(None if rp == None else right.data[rightvalcols[ri]][rp])
            ri = ri + 1

    var matchedright = {}
    var lp = 0
    while lp < left.nrows():
        var k = left.data[on][lp]
        if k in rightidx:
            for rp in rightidx[k]:
                emitrow(lp, rp)
                matchedright[rp] = True
        elif how == "left" or how == "outer":
            emitrow(lp, None)
        lp = lp + 1

    if how == "right" or how == "outer":
        # right rows with no left match (emit left columns as None except the join key)
        var rp2 = 0
        while rp2 < right.nrows():
            if rp2 not in matchedright:
                var li3 = 0
                while li3 < len(leftcols):
                    if leftcols[li3] == on:
                        newdata[leftout[li3]].append(right.data[on][rp2])
                    else:
                        newdata[leftout[li3]].append(None)
                    li3 = li3 + 1
                var ri3 = 0
                while ri3 < len(rightvalcols):
                    newdata[rightout[ri3]].append(right.data[rightvalcols[ri3]][rp2])
                    ri3 = ri3 + 1
            rp2 = rp2 + 1

    return DataFrame(newdata, outcols, None)

var merge = Function(left, right, on, how = "inner"):
    return _merge(left, right, on, how)   # `how` validation lives in _merge (shared with DataFrame.merge)

var concat = Function(frames):
    # stack DataFrames vertically (union of columns, missing filled with None)
    var cols = []
    for f in frames:
        for c in f.columns:
            if c not in cols:
                cols.append(c)
    var newdata = {}
    for c in cols:
        newdata[c] = []
    for f in frames:
        var i = 0
        while i < f.nrows():
            for c in cols:
                newdata[c].append(f.data[c][i] if c in f.columns else None)
            i = i + 1
    return DataFrame(newdata, cols, None)

var readcsv = Function(source, header = True, infer = True):
    # source is CSV text (or a filename if it has no newline and the file exists)
    var text = source
    var io = import("io")
    var path = import("path")
    if "\n" not in source and path.exists(source):
        with io.open(source, "r") as fh:
            text = fh.read()
    var rows = _csv.parse(text)
    if len(rows) == 0:
        return DataFrame({})
    var cols = []
    var start = 0
    if header:
        cols = rows[0]
        start = 1
    else:
        cols = _defaultcols(len(rows[0]))
    var newdata = {}
    for c in cols:
        newdata[c] = []
    var r = start
    while r < len(rows):
        var row = rows[r]
        # A blank line parses to a single empty field; pandas skips such lines rather than emitting a
        # spurious all-None row, so drop it here too.
        if len(row) == 1 and row[0] == "":
            r = r + 1
            continue
        # A row with MORE fields than the header would silently lose its surplus cells; reject it
        # (like pandas' ParserError) rather than truncate. Short rows are still padded with "".
        if len(row) > len(cols):
            throw "readcsv: row " + String(r + 1) + " has " + String(len(row)) + " fields, expected " + String(len(cols))
        var ci = 0
        while ci < len(cols):
            var cell = row[ci] if ci < len(row) else ""
            newdata[cols[ci]].append(_infer(cell) if infer else cell)
            ci = ci + 1
        r = r + 1
    return DataFrame(newdata, cols, None)
)KI";

// --- xml: a dependency-free ElementTree-style XML parser/serializer in pure Kirito ---
inline constexpr std::string_view xml = R"KI(
# A small, dependency-free XML parser/serializer in pure Kirito, in the ElementTree style.
# Parses elements, attributes, text, nested children, comments, the XML declaration,
# CDATA sections, and the standard entities; serializes a tree back to XML. Tag/attribute access is
# via an `Element` tree: `.tag`, `.attrib`, `.text`, `.children`, with `find`/`findall`/`get`.

# --- entity (un)escaping ------------------------------------------------------------------------
# Strict numeric-entity parsers: return -1 (not garbage, not an exception) on an empty or malformed
# body, or one past the max code point — so a bad numeric entity can be kept verbatim (XML leniency).
var _parsehex = Function(s):
    if len(s) == 0:
        return -1
    var v = 0
    for c in s:
        var d = -1
        if c >= "0" and c <= "9":
            d = ord(c) - ord("0")
        elif c >= "a" and c <= "f":
            d = ord(c) - ord("a") + 10
        elif c >= "A" and c <= "F":
            d = ord(c) - ord("A") + 10
        if d < 0:
            return -1
        v = v * 16 + d
        if v > 1114111:
            return -1
    return v

var _parsedec = Function(s):
    if len(s) == 0:
        return -1
    var v = 0
    for c in s:
        if c < "0" or c > "9":
            return -1
        v = v * 10 + (ord(c) - ord("0"))
        if v > 1114111:
            return -1
    return v

var _decode = Function(s):
    # &lt; &gt; &amp; &quot; &apos; and numeric &#dd; / &#xHH;
    if "&" not in s:
        return s
    var out = ""
    var i = 0
    var n = len(s)
    while i < n:
        if s[i] == "&":
            var semi = s.find(";", i)
            if semi < 0:
                out = out + "&"
                i = i + 1
                continue
            var ent = s[i + 1:semi]
            if ent == "lt":
                out = out + "<"
            elif ent == "gt":
                out = out + ">"
            elif ent == "amp":
                out = out + "&"
            elif ent == "quot":
                out = out + "\""
            elif ent == "apos":
                out = out + "'"
            elif len(ent) > 0 and ent[0] == "#":
                var code = -1
                if len(ent) > 1 and (ent[1] == "x" or ent[1] == "X"):
                    code = _parsehex(ent[2:])
                else:
                    code = _parsedec(ent[1:])
                if code >= 0:
                    out = out + chr(code)
                else:
                    out = out + s[i:semi + 1]   # malformed numeric entity: keep verbatim (lenient)
            else:
                out = out + s[i:semi + 1]   # unknown entity: keep verbatim
            i = semi + 1
        else:
            out = out + s[i]
            i = i + 1
    return out

var _escape = Function(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")

var _escape_attr = Function(s):
    return _escape(s).replace("\"", "&quot;")

var _isspace = Function(c) -> Bool:
    return c == " " or c == "\t" or c == "\n" or c == "\r"

# --- the Element tree ---------------------------------------------------------------------------
class Element:
    var _init_ = Function(self, tag, attrib = None):
        self.tag = tag
        self.attrib = attrib if attrib != None else {}
        self.text = ""                 # character data before the first child
        self.tail = ""                 # character data after this element's end tag (ElementTree-style)
        self.children = []             # child Elements

    var _iter_ = Function(self):
        return self.children
    var _len_ = Function(self) -> Integer:
        return len(self.children)
    var _getitem_ = Function(self, i):
        return self.children[i]

    var get = Function(self, key, default = None):
        return self.attrib[key] if key in self.attrib else default

    var find = Function(self, tag):
        for c in self.children:
            if c.tag == tag:
                return c
        return None

    var findall = Function(self, tag):
        var out = []
        for c in self.children:
            if c.tag == tag:
                out.append(c)
        return out

    var findtext = Function(self, tag, default = ""):
        var e = self.find(tag)
        return e.text if e != None else default

    var itertext = Function(self):
        # all text in document order: this element's text, then each child's text + its tail.
        # Iterative (no recursion): a work stack of ["text", s] / ["node", elem] items, expanded
        # in reverse so the document order is preserved on pop.
        var parts = []
        var stack = [["node", self]]
        while len(stack) > 0:
            var item = stack.pop()
            if item[0] == "text":
                if item[1] != "":
                    parts.append(item[1])
            else:
                var e = item[1]
                var work = [["text", e.text]]
                for c in e.children:
                    work.append(["node", c])
                    work.append(["text", c.tail])
                var j = len(work) - 1
                while j >= 0:
                    stack.append(work[j])
                    j = j - 1
        return parts

    var tostring = Function(self) -> String:
        # Iterative serialization (no recursion): a work stack of ["str", s] fragments and
        # ["open", elem] expansions, pushed in reverse so they concatenate in document order.
        var out = ""
        var stack = [["open", self]]
        while len(stack) > 0:
            var item = stack.pop()
            if item[0] == "str":
                out = out + item[1]
            else:
                var e = item[1]
                var head = "<" + e.tag
                for k in e.attrib.keys():
                    head = head + " " + k + "=\"" + _escape_attr(String(e.attrib[k])) + "\""
                if len(e.children) == 0 and e.text == "":
                    out = out + head + " />"
                else:
                    var work = [["str", head + ">"], ["str", _escape(e.text)]]
                    for c in e.children:
                        work.append(["open", c])
                        work.append(["str", _escape(c.tail)])
                    work.append(["str", "</" + e.tag + ">"])
                    var j = len(work) - 1
                    while j >= 0:
                        stack.append(work[j])
                        j = j - 1
        return out

    var _str_ = Function(self) -> String:
        return self.tostring()

# --- the parser ---------------------------------------------------------------------------------
# Parse one start/empty tag body (after '<', up to and including '>'): returns [name, attrib, selfclose, newpos].
var _parse_tag = Function(s, i):
    var n = len(s)
    # tag name
    var start = i
    while i < n and not _isspace(s[i]) and s[i] != ">" and s[i] != "/":
        i = i + 1
    var name = s[start:i]
    var attrib = {}
    # attributes
    while i < n:
        while i < n and _isspace(s[i]):
            i = i + 1
        if i >= n:
            break
        if s[i] == "/" or s[i] == ">":
            break
        var astart = i
        while i < n and s[i] != "=" and not _isspace(s[i]) and s[i] != ">" and s[i] != "/":
            i = i + 1
        var aname = s[astart:i]
        while i < n and _isspace(s[i]):
            i = i + 1
        var aval = ""
        if i < n and s[i] == "=":
            i = i + 1
            while i < n and _isspace(s[i]):
                i = i + 1
            if i < n and (s[i] == "\"" or s[i] == "'"):
                var q = s[i]
                i = i + 1
                var vstart = i
                while i < n and s[i] != q:
                    i = i + 1
                aval = _decode(s[vstart:i])
                i = i + 1            # skip closing quote
        attrib[aname] = aval
    var selfclose = False
    if i < n and s[i] == "/":
        selfclose = True
        i = i + 1
    if i < n and s[i] == ">":
        i = i + 1
    return [name, attrib, selfclose, i]

# Character data belongs to the open element's `text` before its first child, or to the most recent
# child's `tail` afterwards — so mixed content keeps document order on round-trip.
var _addtext = Function(elem, data):
    if len(elem.children) > 0:
        var last = elem.children[len(elem.children) - 1]
        last.tail = last.tail + data
    else:
        elem.text = elem.text + data

var parse = Function(text : String):
    var n = len(text)
    var i = 0
    var root = None
    var stack = []
    while i < n:
        if text[i] != "<":
            # character data until the next '<'
            var lt = text.find("<", i)
            if lt < 0:
                lt = n
            if len(stack) > 0:
                _addtext(stack[len(stack) - 1], _decode(text[i:lt]))
            i = lt
            continue
        # we are at '<'
        if text[i:i + 4] == "<!--":
            var close = text.find("-->", i)
            i = (close + 3) if close >= 0 else n
            continue
        if text[i:i + 9] == "<![CDATA[":
            var close = text.find("]]>", i)
            var endc = close if close >= 0 else n
            if len(stack) > 0:
                _addtext(stack[len(stack) - 1], text[i + 9:endc])   # CDATA: raw, no decoding
            i = (endc + 3) if close >= 0 else n
            continue
        if text[i:i + 2] == "<?":
            var close = text.find("?>", i)
            i = (close + 2) if close >= 0 else n
            continue
        if text[i:i + 2] == "<!":
            var close = text.find(">", i)
            i = (close + 1) if close >= 0 else n
            continue
        if text[i:i + 2] == "</":
            var close = text.find(">", i)
            var endc = close if close >= 0 else n
            if len(stack) > 0:
                stack.pop()
            i = (endc + 1) if close >= 0 else n
            continue
        # a start or empty-element tag
        var parsed = _parse_tag(text, i + 1)
        var elem = Element(parsed[0], parsed[1])
        if len(stack) > 0:
            stack[len(stack) - 1].children.append(elem)
        else:
            root = elem
        if not parsed[2]:
            stack.append(elem)
        i = parsed[3]
    return root

# fromstring is an alias for parse (ElementTree naming); tostring serializes an element.
var fromstring = parse
var tostring = Function(element) -> String:
    return element.tostring()
)KI";

// --- semver (semantic versioning: parse / compare / ranges) ------------------------------------
// A from-scratch implementation of https://semver.org precedence plus a node-semver-style range
// grammar (^, ~, comparators, x-ranges, hyphen ranges, AND via spaces, OR via `||`). Used by `kpm`
// to resolve `owner/repo@<constraint>` against a repository's tags, and generally useful for any
// program that reasons about versions.
inline constexpr std::string_view semver = R"KI(
# ---- low-level identifier helpers ----
var _isnum = Function(s) -> Bool:
    # a non-empty run of ASCII digits
    if len(s) == 0:
        return False
    return s.isdigit()

# Strip a leading 'v'/'='/'V' and surrounding whitespace, e.g. "v1.2.3" -> "1.2.3".
var clean = Function(s) -> String:
    var t = s.strip()
    while len(t) > 0 and (t[0] == "v" or t[0] == "V" or t[0] == "=" or t[0] == " "):
        t = t[1:]
    return t.strip()

# Parse a strict "MAJOR.MINOR.PATCH[-pre][+build]" into a Dict, or throw. `pre`/`build` are Lists of
# dot-separated identifier Strings. A leading 'v' is tolerated.
var parse = Function(s):
    var raw = clean(s)
    var build = []
    var plus = raw.find("+")
    var core = raw
    if plus >= 0:
        build = raw[plus + 1:].split(".")
        core = raw[0:plus]
    var pre = []
    var dash = core.find("-")
    if dash >= 0:
        pre = core[dash + 1:].split(".")
        core = core[0:dash]
    var nums = core.split(".")
    if len(nums) != 3:
        throw "invalid version '" + s + "': need MAJOR.MINOR.PATCH"
    if not (_isnum(nums[0]) and _isnum(nums[1]) and _isnum(nums[2])):
        throw "invalid version '" + s + "': non-numeric core"
    # prerelease identifiers must be non-empty [0-9A-Za-z-]; numeric ones carry no meaning beyond value
    for p in pre:
        if len(p) == 0:
            throw "invalid version '" + s + "': empty prerelease identifier"
    return {"major": Integer(nums[0]), "minor": Integer(nums[1]), "patch": Integer(nums[2]),
            "prerelease": pre, "build": build, "raw": raw}

# Is `s` a valid semver? Returns the cleaned string, or None.
var valid = Function(s):
    try:
        var v = parse(s)
        return v["raw"]
    catch as e:
        return None

var major = Function(s):
    return parse(s)["major"]
var minor = Function(s):
    return parse(s)["minor"]
var patch = Function(s):
    return parse(s)["patch"]
var prerelease = Function(s):
    var p = parse(s)["prerelease"]
    return p if len(p) > 0 else None

# ---- precedence comparison ----
var _cmpint = Function(a, b):
    return -1 if a < b else (1 if a > b else 0)

var _cmpident = Function(x, y):
    var xn = _isnum(x)
    var yn = _isnum(y)
    if xn and yn:
        return _cmpint(Integer(x), Integer(y))
    if xn:
        return -1                 # numeric identifiers have lower precedence than alphanumeric
    if yn:
        return 1
    return -1 if x < y else (1 if x > y else 0)

var _cmppre = Function(a, b):
    # [] (a release) has HIGHER precedence than any prerelease
    if len(a) == 0 and len(b) == 0:
        return 0
    if len(a) == 0:
        return 1
    if len(b) == 0:
        return -1
    var i = 0
    while i < len(a) and i < len(b):
        var c = _cmpident(a[i], b[i])
        if c != 0:
            return c
        i = i + 1
    return _cmpint(len(a), len(b))   # all shared identifiers equal -> the longer one wins

# Compare two version strings: -1 if a < b, 0 if equal precedence, 1 if a > b. Build metadata is
# ignored (per the spec).
var compare = Function(a, b):
    var pa = parse(a)
    var pb = parse(b)
    var c = _cmpint(pa["major"], pb["major"])
    if c != 0:
        return c
    c = _cmpint(pa["minor"], pb["minor"])
    if c != 0:
        return c
    c = _cmpint(pa["patch"], pb["patch"])
    if c != 0:
        return c
    return _cmppre(pa["prerelease"], pb["prerelease"])

var eq = Function(a, b):
    return compare(a, b) == 0
var neq = Function(a, b):
    return compare(a, b) != 0
var lt = Function(a, b):
    return compare(a, b) < 0
var lte = Function(a, b):
    return compare(a, b) <= 0
var gt = Function(a, b):
    return compare(a, b) > 0
var gte = Function(a, b):
    return compare(a, b) >= 0

# The kind of change from a to b: "major"/"minor"/"patch"/"prerelease", or None if equal.
var diff = Function(a, b):
    var pa = parse(a)
    var pb = parse(b)
    if pa["major"] != pb["major"]:
        return "major"
    if pa["minor"] != pb["minor"]:
        return "minor"
    if pa["patch"] != pb["patch"]:
        return "patch"
    if _cmppre(pa["prerelease"], pb["prerelease"]) != 0:
        return "prerelease"
    return None

# Increment a version by a release type: "major"/"minor"/"patch". Drops prerelease/build.
var inc = Function(s, release) -> String:
    var p = parse(s)
    var mj = p["major"]
    var mn = p["minor"]
    var pt = p["patch"]
    if release == "major":
        mj = mj + 1
        mn = 0
        pt = 0
    elif release == "minor":
        mn = mn + 1
        pt = 0
    elif release == "patch":
        pt = pt + 1
    else:
        throw "inc: release must be major/minor/patch, got '" + String(release) + "'"
    return String(mj) + "." + String(mn) + "." + String(pt)

# ---- ranges ----
# A comparator is {"op": ">="/"<="/">"/"<"/"=", "v": <parsed-version-dict>}. A "set" is a List of
# comparators ANDed together; a range is a List of sets ORed together.
var _mkver = Function(mj, mn, pt, pre):
    return {"major": mj, "minor": mn, "patch": pt, "prerelease": pre, "build": []}

# Compare a parsed version dict to another (precedence only).
var _cmpv = Function(a, b):
    var c = _cmpint(a["major"], b["major"])
    if c != 0:
        return c
    c = _cmpint(a["minor"], b["minor"])
    if c != 0:
        return c
    c = _cmpint(a["patch"], b["patch"])
    if c != 0:
        return c
    return _cmppre(a["prerelease"], b["prerelease"])

# Expand one token ("^1.2", "~1", ">=1.0.0", "1.2.x", "*", "1.2.3") into a List of comparators.
var _expand = Function(tok):
    if tok == "" or tok == "*" or tok == "x" or tok == "X":
        return [{"op": ">=", "v": _mkver(0, 0, 0, [])}]
    var op = ""
    var rest = tok
    if rest[0:2] == ">=" or rest[0:2] == "<=":
        op = rest[0:2]
        rest = rest[2:]
    elif rest[0] == ">" or rest[0] == "<" or rest[0] == "=":
        op = rest[0]
        rest = rest[1:]
    elif rest[0] == "^" or rest[0] == "~":
        op = rest[0]
        rest = rest[1:]
    rest = clean(rest)
    # split the (possibly partial / x) version into up to 3 parts
    var parts = rest.split("-")[0].split(".")
    # reject a 4th+ component (1.2.3.4) — `valid` is strict about this, so a range must be too, else
    # validrange("1.2.3.4") would disagree with valid("1.2.3.4")
    if len(parts) > 3:
        throw "invalid version (too many components): " + rest
    var prepart = []
    var dash = rest.find("-")
    if dash >= 0:
        prepart = rest[dash + 1:].split(".")
    var hasx = Function(p):
        return p == "x" or p == "X" or p == "*" or p == ""
    var mj = 0
    var mn = 0
    var pt = 0
    var xmj = len(parts) < 1 or hasx(parts[0])
    var xmn = len(parts) < 2 or hasx(parts[1])
    var xpt = len(parts) < 3 or hasx(parts[2])
    if not xmj:
        mj = Integer(parts[0])
    if not xmn:
        mn = Integer(parts[1])
    if not xpt:
        pt = Integer(parts[2])
    if op == "^":
        var lo = _mkver(mj, mn, pt, prepart)
        var hi = None
        if mj > 0 or xmn:
            hi = _mkver(mj + 1, 0, 0, [])
        elif mn > 0 or xpt:
            hi = _mkver(0, mn + 1, 0, [])
        else:
            hi = _mkver(0, 0, pt + 1, [])
        return [{"op": ">=", "v": lo}, {"op": "<", "v": hi}]
    if op == "~":
        var lo = _mkver(mj, mn, pt, prepart)
        var hi = None
        if xmn:
            hi = _mkver(mj + 1, 0, 0, [])      # ~1 -> >=1.0.0 <2.0.0
        else:
            hi = _mkver(mj, mn + 1, 0, [])      # ~1.2 / ~1.2.3 -> >=1.2.x <1.3.0
        return [{"op": ">=", "v": lo}, {"op": "<", "v": hi}]
    if op == "" or op == "=":
        # x-ranges: a missing/x part widens to a range; a full version is exact.
        if xmj:
            return [{"op": ">=", "v": _mkver(0, 0, 0, [])}]
        if xmn:
            return [{"op": ">=", "v": _mkver(mj, 0, 0, [])}, {"op": "<", "v": _mkver(mj + 1, 0, 0, [])}]
        if xpt:
            return [{"op": ">=", "v": _mkver(mj, mn, 0, [])}, {"op": "<", "v": _mkver(mj, mn + 1, 0, [])}]
        return [{"op": "=", "v": _mkver(mj, mn, pt, prepart)}]
    # explicit comparator (>, >=, <, <=): fill missing parts with 0
    return [{"op": op, "v": _mkver(mj, mn, pt, prepart)}]

# Parse a range string into a List of comparator sets (OR of ANDs).
var _parserange = Function(rng):
    var sets = []
    for orpart in rng.split("||"):
        var toks = []
        for t in orpart.strip().split(" "):
            if len(t) > 0:
                toks.append(t)
        var comps = []
        var i = 0
        while i < len(toks):
            # hyphen range:  A - B  -> >=A <=B
            if i + 2 < len(toks) and toks[i + 1] == "-":
                var lo = _expand(toks[i])
                var hi = _expand(toks[i + 2])
                comps.append({"op": ">=", "v": lo[0]["v"]})
                # upper bound: a partial hi (e.g. "2.3") becomes "<next", else "<=hi"
                if len(hi) == 2:
                    comps.append(hi[1])
                else:
                    comps.append({"op": "<=", "v": hi[0]["v"]})
                i = i + 3
            else:
                for c in _expand(toks[i]):
                    comps.append(c)
                i = i + 1
        if len(comps) == 0:
            comps.append({"op": ">=", "v": _mkver(0, 0, 0, [])})
        sets.append(comps)
    return sets

var _testcomp = Function(pv, comp):
    var c = _cmpv(pv, comp["v"])
    var op = comp["op"]
    if op == ">=":
        return c >= 0
    if op == "<=":
        return c <= 0
    if op == ">":
        return c > 0
    if op == "<":
        return c < 0
    return c == 0                # "="

# Does `version` satisfy `range`? Prerelease versions match only when a comparator in the same
# satisfied set pins the same major.minor.patch with a prerelease (node-semver's default).
var satisfies = Function(version, rng) -> Bool:
    var sets = None
    try:
        sets = _parserange(rng)         # an unparseable range satisfies nothing (node-semver)
    catch as e:
        return False
    var pv = parse(version)
    for comps in sets:
        var ok = True
        for comp in comps:
            if not _testcomp(pv, comp):
                ok = False
        if ok and len(pv["prerelease"]) > 0:
            var allowed = False
            for comp in comps:
                if len(comp["v"]["prerelease"]) > 0 and comp["v"]["major"] == pv["major"] and comp["v"]["minor"] == pv["minor"] and comp["v"]["patch"] == pv["patch"]:
                    allowed = True
            if not allowed:
                ok = False
        if ok:
            return True
    return False

# Is `rng` a parseable range?
var validrange = Function(rng) -> Bool:
    try:
        discard _parserange(rng)
        return True
    catch as e:
        return False

# Sort version strings by precedence (insertion sort with compare(); invalid versions are dropped).
var _sortby = Function(versions, rev):
    var arr = []
    for v in versions:
        if valid(v) != None:
            arr.append(v)
    var i = 1
    while i < len(arr):
        var key = arr[i]
        var j = i - 1
        while j >= 0 and (compare(arr[j], key) < 0 if rev else compare(arr[j], key) > 0):
            arr[j + 1] = arr[j]
            j = j - 1
        arr[j + 1] = key
        i = i + 1
    return arr
# ascending by precedence
var sort = Function(versions):
    return _sortby(versions, False)
# descending by precedence
var rsort = Function(versions):
    return _sortby(versions, True)

# The highest / lowest version in `versions` that satisfies `range`, or None.
var maxsatisfying = Function(versions, rng):
    var best = None
    for v in versions:
        if valid(v) != None and satisfies(v, rng):
            if best == None or compare(v, best) > 0:
                best = v
    return best
var minsatisfying = Function(versions, rng):
    var best = None
    for v in versions:
        if valid(v) != None and satisfies(v, rng):
            if best == None or compare(v, best) < 0:
                best = v
    return best
)KI";

}  // namespace kirito::kimod

#endif
