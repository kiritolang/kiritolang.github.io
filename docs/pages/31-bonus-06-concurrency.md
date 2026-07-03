# Bonus: Concurrency with `parallel`

Kirito runs code in parallel by **multiprocessing**: instead of many threads sharing one heap, it runs
many fully-isolated VMs — one per OS thread — that share **nothing** and talk only by passing
serialized values through thread-safe primitives. This avoids data races by construction (no VM's
values are ever touched by two threads), at the cost of one rule: anything you hand to another VM must
be **serializable**, and a worker can't reach the local variables of the function that spawned it.

This lesson builds up from a single `spawn` to the two patterns the example servers use.

> `parallel` is provided by the `ki` interpreter (which runs every VM under a coordinator). A bare
> embedded `KiritoVM` has no `parallel` module — see the embedding guide for `KiritoDispatcher`.

## Spawning work

`parallel.spawn(fn, *args, **kwargs)` runs `fn` on a fresh worker VM and returns a `Task`. Call
`t.join()` to block for its result (rebuilt in your VM). `fn` must be a function defined in a `.ki`
file — the worker re-reads that file to find it.

```kirito
var parallel = import("parallel")
var io = import("io")

var slow_square = Function(x):
    return x * x

if argmain:
    var t = parallel.spawn(slow_square, 9)
    io.print(t.join())          # 81
```

Spawn many at once for a parallel map-reduce — start every task, then join them all:

```kirito
var parallel = import("parallel")
var io = import("io")

var work = Function(lo, hi):    # sum of a sub-range
    var s = 0
    var i = lo
    while i < hi:
        s = s + i
        i = i + 1
    return s

if argmain:
    var tasks = []
    var i = 0
    while i < 4:
        tasks.append(parallel.spawn(work, i * 250, i * 250 + 250))
        i = i + 1
    var total = 0
    for t in tasks:
        total = total + t.join()
    io.print(total)             # 499500
```

> **The share-nothing rule.** A worker sees its parameters, the defining file's module-level names, and
> its `import`s — but *not* locals captured from an enclosing function (the closure does not cross).
> Keep functions you spawn at module level, and pass everything they need as arguments. Put your
> program's startup under `if argmain:` so a worker (which re-evaluates the file with `argmain` False)
> only *defines* functions instead of re-running the whole program.

## Queues: passing values between VMs

A `Queue` is a thread-safe FIFO that carries values across VMs. It's the one object you can share
between workers — pass it into `spawn` (or send it through another Queue) and every VM references the
**same** queue. A producer puts; consumers get; `close()` signals "no more":

```kirito
var parallel = import("parallel")
var io = import("io")

var producer = Function(out, n):
    var i = 0
    while i < n:
        out.put(i * i)
        i = i + 1
    out.close()                 # tell the consumer we're done

if argmain:
    var q = parallel.Queue()
    discard parallel.spawn(producer, q, 5)
    var got = []
    var running = True
    while running:
        try:
            got.append(q.get())     # blocks for the next item
        catch as e:                 # thrown once the queue is closed and drained
            running = False
    got.sort()
    io.print(got)               # [0, 1, 4, 9, 16]
```

A bounded `Queue(maxsize)` gives back-pressure: a full `put` blocks until a consumer makes room.

## Pattern 1 — the actor (one owner of shared state)

When state must be shared and mutated (a database, a counter, a cache), keep it on **one** owner VM and
funnel every access through a Queue. Workers send a request plus a private reply Queue; the owner does
the work and replies. Because only the owner touches the state, there are no races — and it's exactly
how the `sqldb` example serves a shared database from a pool of connection workers.

```kirito
var parallel = import("parallel")
var io = import("io")

var counter_owner = Function(reqq):     # the only VM that owns `total`
    var total = 0
    var running = True
    while running:
        var msg = None
        try:
            msg = reqq.get()
        catch as e:
            running = False
        if msg != None:                 # msg = [amount, replyq]
            total = total + msg[0]
            msg[1].put(total)

if argmain:
    var reqq = parallel.Queue()
    discard parallel.spawn(counter_owner, reqq)
    var reply = parallel.Queue()
    reqq.put([10, reply])
    io.print(reply.get())               # 10
    reqq.put([5, reply])
    io.print(reply.get())               # 15
    reqq.close()
```

## Pattern 2 — the worker pool (stateless)

When the work is independent (handling HTTP requests against read-only data), skip the owner: spawn a
pool of identical workers that pull jobs from a shared Queue. The `webserver` example uses this — an
acceptor hands each connection's socket to the pool. Sockets cross by *file descriptor*:
`socket.detach()` surrenders the fd, and `net.fromfd(fd)` adopts it in the worker (same OS process).

```kirito
var parallel = import("parallel")
var net = import("net")

var worker = Function(jobs):
    var running = True
    while running:
        try:
            var fd = jobs.get()
            var conn = net.fromfd(fd)   # adopt the handed-off connection
            # ... serve conn ...
            conn.close()
        catch as e:
            running = False             # jobs queue closed -> stop

if argmain:
    var jobs = parallel.Queue()
    var n = parallel.cpucount()
    var i = 0
    while i < n:
        discard parallel.spawn(worker, jobs)
        i = i + 1
    # accept loop: jobs.put(srv.accept().detach())
```

## Coordination primitives

Beyond Queues, `parallel` offers the classic primitives — all shared across VMs, all woken on
shutdown:

- **`Lock`** — mutual exclusion around an external resource. Prefer `with lock:` (always releases).
- **`Event`** — a flag many workers `wait()` on until one `set()`s it (e.g. a start/stop signal).
- **`Semaphore(n)`** — allow at most `n` workers into a section at once.
- **`Barrier(n)`** — make `n` workers rendezvous before any proceeds.

```kirito
var parallel = import("parallel")
var io = import("io")

var worker = Function(bar, out, id):
    discard bar.wait()          # all workers line up here first
    out.put(id)

if argmain:
    var n = 3
    var bar = parallel.Barrier(n)
    var q = parallel.Queue()
    var i = 0
    while i < n:
        discard parallel.spawn(worker, bar, q, i)
        i = i + 1
    var got = []
    while len(got) < n:
        got.append(q.get())
    got.sort()
    io.print(got)               # [0, 1, 2]
```

## Avoiding deadlock

The runtime is deadlock-safe by construction — interpreter shutdown aborts every blocked primitive — so
a program that exits never hangs. For your own logic:

- give a `timeout =` to any blocking call that might never be satisfied (`q.get(True, 5)`,
  `ev.wait(2)`, `lock.acquire(True, 1)`);
- use `with lock:` / `with sem:` so a primitive is released even on an exception;
- if a worker takes several locks, take them in a consistent order everywhere;
- `close()` a Queue when production ends, so consumers stop instead of waiting forever.
