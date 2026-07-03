# Lesson 12 — Context Managers and with

Some resources must be cleaned up: a file must be closed, a lock released, a timer stopped — even if
an error interrupts the work. You *could* do this with `try`/`finally` every time, but Kirito offers
a cleaner tool: the `with` statement and the **context-manager protocol**.

## The `with` statement

`with EXPR as NAME:` enters a context, binds its resource to `NAME`, runs the block, and guarantees
cleanup on the way out — normal exit *or* exception. The classic case is files:

```kirito
var io = import("io")
var path = import("path")
var demo = path.join(path.getcwd(), "lesson17_demo.txt")

with io.open(demo, "w") as file:
    file.write("line one\n")
    file.write("line two\n")
# the file is closed automatically here, even if write() had thrown

with io.open(demo, "r") as file:
    io.print(file.read())
# => line one / line two
discard path.remove(demo, missing_ok = True)            # tidy up the demo file
```

No explicit `close()` — leaving the `with` block closes the file for you. That's the whole appeal:
the cleanup is impossible to forget.

## The protocol: `_enter_` and `_exit_`

`with` works on any object that implements two special methods:

- `_enter_(self)` — runs on entry; its return value is what `as NAME` binds.
- `_exit_(self)` — runs on exit (always), to release the resource.

Knowing this, you can write your own context managers. Here's a timer that reports how long its block
took:

```kirito
var io = import("io")
var time = import("time")

class Timer:
    var _init_ = Function(self, label):
        self.label = label
    var _enter_ = Function(self):
        self._start = time.perfcounterns()      # record the start time on entry
        return self                              # bound to `as t`
    var _exit_ = Function(self):
        var elapsed_ns = time.perfcounterns() - self._start
        io.print(f"{self.label} took {elapsed_ns} ns")

with Timer("work") as t:
    var total = 0
    for i in range(1000):
        total = total + i
# => work took <some number> ns   (printed automatically on exit)
```

**Walkthrough:** `_enter_` stamps the start time and hands back `self`; the block does its work;
`_exit_` runs the moment the block ends (or throws) and reports the elapsed time. The caller can't
forget to stop the timer — the protocol does it. Any "do X before, undo it after" pattern fits this
shape: open/close, lock/unlock, set-up/tear-down.

## In-memory buffers as contexts

`BytesIO` is an in-memory stream usable anywhere a file is — and it's a context manager too. It's
ideal for tests and for capturing output without touching disk:

```kirito
var io = import("io")

var buffer = io.BytesIO()
buffer.write("captured text")
io.print(buffer.getvalue())        # => captured text
```

## Redirecting output with `tee`

The `tee` standard-library module builds on the stream protocol to *fan out* writes — sending output
to several places at once, or capturing it. `tee_stdout` is a context manager that mirrors everything
printed to `stdout` into extra streams while the block runs:

```kirito
var io = import("io")
var tee = import("tee")

var log = io.BytesIO()
with tee.tee_stdout(log):
    io.print("this goes to the screen AND the log")
io.print("captured copy:", log.getvalue())
# => this goes to the screen AND the log
# => captured copy: this goes to the screen AND the log
```

When the `with` block ends, `stdout` is restored automatically — again, cleanup you can't forget.

## Try it

Write a `Transaction` context manager whose `_enter_` prints `"BEGIN"` and returns `self`, and whose
`_exit_` prints `"COMMIT"`. Run a `with Transaction():` block that prints some work in between, and
watch BEGIN/work/COMMIT bracket it. (Bonus, once you've read more: have it print `"ROLLBACK"` instead
when the block thrown — combine `with` and exceptions.)

## What you learned

- `with EXPR as NAME:` runs setup, your block, then guaranteed cleanup.
- The protocol is `_enter_` (returns the bound resource) and `_exit_` (always runs).
- `io.open` files and `BytesIO` buffers are context managers; so is anything you write.
- The `tee` module redirects/captures stream output via the same protocol.
