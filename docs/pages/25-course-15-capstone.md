# Lesson 15 — Capstone: A Task Manager

Time to bring it all together. In this final lesson we build one small but complete program — a
**task manager** that holds a to-do list, keeps it ordered by urgency, and saves it to disk as JSON.
It exercises nearly everything from the course: classes with annotations, **operator overloading**
(so your own objects sort themselves), private state, exceptions, f-strings, `with`-blocks, files,
and the `json` module.

The code below is **one program**, shown a section at a time — paste the blocks into a single file,
top to bottom, and it runs. Nothing is repeated; each section adds the next piece.

## The `Task`

A `Task` is a small value type: a title, a priority, and a done flag. Two things make it pleasant to
work with. Its `_str_` prints a tidy one-liner, and its `_lt_` defines an **ordering** — once a class
says how two of its instances compare, `sorted()`, `min()`, `max()` and the `<` operator all work on
it for free.

<!--norun (one section of the single program assembled across this lesson)-->
```kirito
var io = import("io")
var json = import("json")
var path = import("path")

# Priorities, ranked most urgent (0) to least. The rank drives the ordering below.
var RANK = {"high": 0, "medium": 1, "low": 2}

class Task:
    var _init_ = Function(self, title : String, priority : String = "medium"):
        if not (priority in RANK):
            throw f"unknown priority: {priority}"
        self.title = title
        self.priority = priority
        self.done = False

    # A one-line view: a checkbox, the priority in a fixed-width column, then the title.
    var _str_ = Function(self) -> String:
        var box = "[x]" if self.done else "[ ]"
        return f"{box} {self.priority:<6} {self.title}"

    # The sort key: pending tasks before done ones, then by priority rank, then by title.
    # Returning a List lets one comparison rank on three fields at once — Lists compare
    # element by element, so `[0, 0, "a"] < [0, 1, "b"]`.
    var _key = Function(self):
        return [Integer(self.done), RANK[self.priority], self.title]

    # With _lt_ defined, sorted()/min()/max()/< all order Tasks — no key function needed anywhere.
    var _lt_ = Function(self, other : Task) -> Bool:
        return self._key() < other._key()

    # Reduce to a plain Dict so the json module can store it.
    var to_dict = Function(self) -> Dict:
        return {"title": self.title, "priority": self.priority, "done": self.done}
```

`_key` has a **single leading underscore**, so it's private — part of how `Task` orders itself, not
something callers should touch.

## Reporting errors

When a caller asks for a task that isn't there, we throw a **specific** exception type so the caller
can catch exactly that case:

<!--norun (one section of the single program assembled across this lesson)-->
```kirito
class TaskNotFound:
    var _init_ = Function(self, title):
        self.title = title
```

## The `TaskList`

The list keeps its tasks in a **private** `List` and exposes a clean interface. Each method does one
job. Note how `ordered` just calls `sorted(self._tasks)` — because `Task` defined `_lt_`, that's all
it takes — and how `save`/`load` use a `with` block so the file is always closed:

<!--norun (one section of the single program assembled across this lesson)-->
```kirito
class TaskList:
    var _init_ = Function(self):
        self._tasks = []                       # private: a List of Task

    var add = Function(self, task : Task):
        self._tasks.append(task)

    var complete = Function(self, title : String):
        for task in self._tasks:
            if task.title == title:
                task.done = True
                return
        throw TaskNotFound(title)

    # Only the unfinished tasks.
    var pending = Function(self):
        var out = []
        for task in self._tasks:
            if not task.done:
                out.append(task)
        return out

    # len(tasklist) works because of _len_.
    var _len_ = Function(self) -> Integer:
        return len(self._tasks)

    # Tasks in urgency order — sorted() leans on Task._lt_.
    var ordered = Function(self):
        return sorted(self._tasks)

    var save = Function(self, path : String):
        var records = []
        for task in self.ordered():
            records.append(task.to_dict())
        with io.open(path, "w") as f:
            f.write(json.dumps(records, indent = 2))

    var load = Function(self, path : String):
        with io.open(path, "r") as f:
            var records = json.loads(f.read())
        for record in records:
            var task = Task(record["title"], record["priority"])
            task.done = record["done"]
            self.add(task)
```

## Running it

Now we drive the program: add tasks, complete one, print them in order, persist to disk, reload into a
fresh list, and handle a missing task gracefully — every failure is caught and the program keeps
running:

<!--norun (the finished program: assemble the sections above into one file to run it)-->
```kirito
var inbox = TaskList()
inbox.add(Task("Write the report", "high"))
inbox.add(Task("Water the plants", "low"))
inbox.add(Task("Reply to Ada", "medium"))
inbox.add(Task("Ship the release", "high"))

inbox.complete("Water the plants")

io.print(f"{len(inbox)} tasks, {len(inbox.pending())} still pending:")
for task in inbox.ordered():
    io.print(f"  {task}")                       # uses Task._str_

# Persist to disk and reload into a brand-new list.
var store = path.join(path.getcwd(), "tasks.json")
inbox.save(store)

var reloaded = TaskList()
reloaded.load(store)
io.print(f"reloaded {len(reloaded)} tasks from disk")

# Completing a task that doesn't exist is handled, not fatal.
try:
    reloaded.complete("Take over the world")
catch TaskNotFound as e:
    io.print(f"no such task: {e.title}")

discard path.remove(store, missing_ok = True)  # clean up the demo file
```

Running it prints:

```text
4 tasks, 3 still pending:
  [ ] high   Ship the release
  [ ] high   Write the report
  [ ] medium Reply to Ada
  [x] low    Water the plants
reloaded 4 tasks from disk
no such task: Take over the world
```

The two `high` tasks come first (alphabetical between them), then `medium`, and the completed task
sinks to the bottom — all from that one `_lt_` definition.

## What each lesson contributed

- **Classes (Lessons 8–9):** `Task` bundles a person-sized value with the behavior that belongs to
  it; `TaskList` bundles the storage with the operations on it.
- **Operators & protocols (Lesson 9):** `_str_` makes a `Task` print itself, `_lt_` makes it
  **sortable** (so `sorted(self._tasks)` needs no key), and `_len_` makes `len(tasklist)` work.
- **Private state (Lesson 8):** `_tasks` and `_key` are private — callers go through the public
  methods, so the list stays consistent and you could swap the storage later.
- **Annotations (Lesson 7):** `: Task`, `: String`, `-> Bool`/`-> Integer` document *and* enforce the
  contract at each boundary.
- **Exceptions (Lesson 11):** `TaskNotFound` turns "bad input" into a specific, catchable event; the
  driver recovers from it and carries on.
- **Context managers & files (Lessons 12–13):** `save`/`load` persist the whole list inside a `with`
  block that closes the file no matter what.
- **The standard library (Lesson 14):** `json` serializes the records; `path.join`/`path.getcwd`/
  `path.remove` handle the path.
- **Collections, strings & functions (Lessons 5, 3, 6):** a `List` of tasks ordered by a
  List-valued key; f-strings with a `:<6` alignment spec; `sorted` doing the work.

## Where to go next

You now know enough to write real programs. To go further:

- **Extend this project:** add due dates with the `time` module, a `priority` filter, or load on
  startup and save on exit so the list survives across runs. Wrap it in an `io.input` loop for an
  interactive CLI — see the [command-line lesson](bonus-02-cli.html) for argument parsing.
- **Persist objects directly:** instead of hand-writing `to_dict`/`load`, the `serialize` and `dump`
  modules save a whole object graph (shared references and all) in one call — try replacing `save`
  with `serialize.save(self._tasks, path)`.
- **Read the reference pages:** the [Standard Library](stdlib.html), [Built-in Functions](builtins.html),
  and [Types](types.html) pages document every function and method with signatures.
- **Study real programs:** the `examples/` directory has complete projects — an RPN calculator, a
  word-frequency analyzer, a linear-system solver — and `examples/big_projects/` has larger ones (a
  networked SQL database, an HTTP server, a deep-learning library) written entirely in Kirito.
- **Embed or extend Kirito in C++:** the [C++ API](cpp-api.html) page covers running Kirito inside
  a C++ program and adding your own native functions, modules, and types.

Congratulations — you've finished the **core course**. What follows are **bonus lessons**: deeper
dives into specialized libraries — regular expressions, command-line programs, tabular data analysis,
linear algebra, tensors with automatic differentiation, and concurrency. Take them in any order, or go build
something.
