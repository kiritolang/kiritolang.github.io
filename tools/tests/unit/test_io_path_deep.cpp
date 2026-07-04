// test_io_path_deep.cpp — adversarial/edge coverage for the io + path modules: std streams as
// context managers, tolerant listing on files, binary writelines, seek/mode/rename/mkdir edges.
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// Run a Kirito program, return the last expression stringified.
static std::string run(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}
// True iff the program raises a Kirito error (a caught KiritoError or any std::exception).
static bool throws(KiritoVM& vm, const std::string& src) {
    try { vm.runSource(src); return false; }
    catch (...) { return true; }
}

int main() {
    KiritoVM vm;

    // --- io.stdout / io.stderr as `with` context managers (ZERO prior coverage) ---
    // `with io.stdout as s:` must enter (yield the stream) and exit (no-op close); `s` is usable.
    CHECK(run(vm, R"KI(
var io = import("io")
var entered = False
with io.stdout as s:
    s.flush()
    entered = True
entered
)KI") == "True");
    CHECK(run(vm, R"KI(
var io = import("io")
var entered = False
with io.stderr as s:
    s.flush()
    entered = True
entered
)KI") == "True");

    // --- path.listdir / path.walk on a path that IS a regular file (tolerant -> []) ---
    CHECK(run(vm, R"KI(
var io = import("io")
var path = import("path")
var p = path.join(path.gettempdir(), "kdeep_regfile.txt")
var f = io.open(p, "w")
f.write("hi")
f.close()
var n = len(path.listdir(p))
path.remove(p)
n
)KI") == "0");
    CHECK(run(vm, R"KI(
var io = import("io")
var path = import("path")
var p = path.join(path.gettempdir(), "kdeep_regfile2.txt")
var f = io.open(p, "w")
f.write("hi")
f.close()
var n = len(path.walk(p))
path.remove(p)
n
)KI") == "0");
    // and still tolerant on an outright-missing path:
    CHECK(run(vm, "var path = import(\"path\")\nlen(path.listdir(path.join(path.gettempdir(), \"kdeep_nope_zzz\")))") == "0");
    CHECK(run(vm, "var path = import(\"path\")\nlen(path.walk(path.join(path.gettempdir(), \"kdeep_nope_zzz\")))") == "0");

    // --- File.writelines with Bytes items in binary mode ("wb") ---
    // Each item is written raw (no separator); read back byte-for-byte.
    CHECK(run(vm, R"KI(
var io = import("io")
var path = import("path")
var p = path.join(path.gettempdir(), "kdeep_wb.bin")
var f = io.open(p, "wb")
f.writelines([Bytes([104, 105]), Bytes([10, 65])])
f.close()
var g = io.open(p, "rb")
var data = List(g.read())
g.close()
path.remove(p)
data
)KI") == "[104, 105, 10, 65]");

    // --- path.walk(dir=...) keyword (the param is named `dir`, not `path`) ---
    CHECK(run(vm, R"KI(
var io = import("io")
var path = import("path")
var d = path.join(path.gettempdir(), "kdeep_walkdir")
path.rmtree(d, missing_ok=True)
path.mkdir(d, exist_ok=True)
var fp = path.join(d, "a.txt")
var f = io.open(fp, "w")
f.write("x")
f.close()
var n = len(path.walk(dir=d))
path.rmtree(d)
n
)KI") == "1");

    // --- path.chmod with high bits (sticky/setuid) must not throw; file stays readable ---
    // Low bits stick; exact permission bits are platform-dependent, so assert leniently (isfile).
    CHECK(run(vm, R"KI(
var io = import("io")
var path = import("path")
var p = path.join(path.gettempdir(), "kdeep_chmod.txt")
var f = io.open(p, "w")
f.write("x")
f.close()
discard path.chmod(p, 0o1755)
discard path.chmod(p, 0o4755)
var readable = path.isfile(p)
path.remove(p)
readable
)KI") == "True");

    // --- io.read(-1) / io.read(negative) => read-all branch, via a rebound BytesIO stdin ---
    {
        KiritoVM v2;   // isolate stdin rebinding from the shared VM
        CHECK(run(v2, R"KI(
var io = import("io")
var b = io.BytesIO("hello world")
io.stdin = b
var r = io.read(-1)
io.stdin = io.__stdin__
r
)KI") == "hello world");
        CHECK(run(v2, R"KI(
var io = import("io")
var b = io.BytesIO("abcdef")
io.stdin = b
var r = io.read(3)
io.stdin = io.__stdin__
r
)KI") == "abc");
        // read() with no argument is also read-all:
        CHECK(run(v2, R"KI(
var io = import("io")
var b = io.BytesIO("payload")
io.stdin = b
var r = io.read()
io.stdin = io.__stdin__
r
)KI") == "payload");
    }

    // --- seek with a non-Integer offset must throw (File and BytesIO) ---
    CHECK(throws(vm, "var io = import(\"io\")\nvar b = io.BytesIO(\"abc\")\nb.seek(\"x\")"));
    CHECK(run(vm, R"KI(
var io = import("io")
var path = import("path")
var p = path.join(path.gettempdir(), "kdeep_seekf.txt")
var f = io.open(p, "w")
var caught = False
try:
    f.seek("x")
catch as e:
    caught = True
f.close()
path.remove(p)
caught
)KI") == "True");
    // BytesIO clamps a negative absolute target to 0 (no throw); File rejects it (throws).
    CHECK(run(vm, "var io = import(\"io\")\nvar b = io.BytesIO(\"abc\")\ndiscard b.seek(-100)\nb.tell()") == "0");
    CHECK(run(vm, R"KI(
var io = import("io")
var path = import("path")
var p = path.join(path.gettempdir(), "kdeep_seekneg.txt")
var f = io.open(p, "w")
var caught = False
try:
    f.seek(-5)
catch as e:
    caught = True
f.close()
path.remove(p)
caught
)KI") == "True");

    // --- File.seek beyond EOF then write ("wb"): extends the file (POSIX zero-fills the hole) ---
    // Robust assertion: total length and the written tail; the gap bytes are zero-filled on POSIX.
    CHECK(run(vm, R"KI(
var io = import("io")
var path = import("path")
var p = path.join(path.gettempdir(), "kdeep_hole.bin")
var f = io.open(p, "wb")
discard f.seek(5)
f.write(Bytes([65, 66]))
f.close()
var g = io.open(p, "rb")
var data = List(g.read())
g.close()
path.remove(p)
[len(data), data[5], data[6]]
)KI") == "[7, 65, 66]");

    // --- open modes "w+" and "a+" must throw "unsupported file mode" ---
    CHECK(throws(vm, "var io = import(\"io\")\nvar path = import(\"path\")\nio.open(path.join(path.gettempdir(), \"kdeep_wp.txt\"), \"w+\")"));
    CHECK(throws(vm, "var io = import(\"io\")\nvar path = import(\"path\")\nio.open(path.join(path.gettempdir(), \"kdeep_ap.txt\"), \"a+\")"));
    // a stray unsupported mode letter also throws:
    CHECK(throws(vm, "var io = import(\"io\")\nvar path = import(\"path\")\nio.open(path.join(path.gettempdir(), \"kdeep_zz.txt\"), \"z\")"));

    // --- open "w"/"a" in a non-existent directory must throw (could not open file) ---
    CHECK(throws(vm, R"KI(
var io = import("io")
var path = import("path")
io.open(path.join(path.gettempdir(), "kdeep_nodir_xyz", "f.txt"), "w")
)KI"));
    CHECK(throws(vm, R"KI(
var io = import("io")
var path = import("path")
io.open(path.join(path.gettempdir(), "kdeep_nodir_xyz", "f.txt"), "a")
)KI"));
    // "r" on a missing file likewise throws:
    CHECK(throws(vm, R"KI(
var io = import("io")
var path = import("path")
io.open(path.join(path.gettempdir(), "kdeep_missing_read.txt"), "r")
)KI"));

    // --- path.rename onto an EXISTING destination silently overwrites ---
    CHECK(run(vm, R"KI(
var io = import("io")
var path = import("path")
var s = path.join(path.gettempdir(), "kdeep_src.txt")
var d = path.join(path.gettempdir(), "kdeep_dst.txt")
var f = io.open(s, "w")
f.write("SRC")
f.close()
var g = io.open(d, "w")
g.write("DSTOLD")
g.close()
path.rename(s, d)
var h = io.open(d, "r")
var content = h.read()
h.close()
path.remove(d)
[content, path.exists(s)]
)KI") == "['SRC', False]");

    // --- path.mkdir on an existing FILE: exist_ok=True returns False; exist_ok=False throws ---
    CHECK(run(vm, R"KI(
var io = import("io")
var path = import("path")
var p = path.join(path.gettempdir(), "kdeep_mkfile.txt")
var f = io.open(p, "w")
f.write("x")
f.close()
var r = path.mkdir(p, exist_ok=True)
path.remove(p)
r
)KI") == "False");
    CHECK(run(vm, R"KI(
var io = import("io")
var path = import("path")
var p = path.join(path.gettempdir(), "kdeep_mkfile2.txt")
var f = io.open(p, "w")
f.write("x")
f.close()
var caught = False
try:
    discard path.mkdir(p, exist_ok=False)
catch as e:
    caught = True
path.remove(p)
caught
)KI") == "True");

    // --- open with >2 positional args must throw ---
    CHECK(throws(vm, R"KI(
var io = import("io")
var path = import("path")
io.open(path.join(path.gettempdir(), "kdeep_x.txt"), "w", "extra")
)KI"));

    // --- File read(n) after readline/iteration drove to EOF: returns "" (empty), no crash ---
    CHECK(run(vm, R"KI(
var io = import("io")
var path = import("path")
var p = path.join(path.gettempdir(), "kdeep_eof.txt")
var f = io.open(p, "w")
f.write("line1\nline2\n")
f.close()
var g = io.open(p, "r")
discard g.readlines()
var after = g.read(10)
g.close()
path.remove(p)
after
)KI") == "");
    // binary mode: read(n) past EOF is an empty Bytes (length 0):
    CHECK(run(vm, R"KI(
var io = import("io")
var path = import("path")
var p = path.join(path.gettempdir(), "kdeep_eofb.bin")
var f = io.open(p, "wb")
f.write(Bytes([1, 2, 3]))
f.close()
var g = io.open(p, "rb")
discard g.read()
var n = len(g.read(5))
g.close()
path.remove(p)
n
)KI") == "0");
    // and iteration to EOF followed by read(n) also yields "" without crashing:
    CHECK(run(vm, R"KI(
var io = import("io")
var path = import("path")
var p = path.join(path.gettempdir(), "kdeep_eof_iter.txt")
var f = io.open(p, "w")
f.write("a\nb\n")
f.close()
var g = io.open(p, "r")
for line in g:
    discard line
var after = g.read(4)
g.close()
path.remove(p)
after
)KI") == "");

    return RUN_TESTS();
}
