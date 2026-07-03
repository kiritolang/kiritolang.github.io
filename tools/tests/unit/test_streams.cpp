// Interchangeable streams: io.print / write / input / read act on the current io.stdout / io.stdin,
// which can be rebound to a file, a BytesIO, the originals (io.__stdout__...), stderr, or any object
// that merely exposes write/readline/read (duck typing).
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string run(KiritoVM& vm, const std::string& src) { return vm.stringify(vm.runSource(src)); }

int main() {
    // print/write go to a redirected stdout (BytesIO); restoring via __stdout__ stops capture.
    {
        KiritoVM vm;
        CHECK(run(vm, R"(
var io = import("io")
var buf = io.BytesIO()
io.stdout = buf
io.print("hello", 42)
io.write("raw", "-bits")
io.stdout = io.__stdout__
buf.getvalue()
)") == "hello 42\nraw-bits");
    }

    // stderr is independently rebindable; eprint targets it.
    {
        KiritoVM vm;
        CHECK(run(vm, R"(
var io = import("io")
var e = io.BytesIO()
io.stderr = e
io.eprint("oops")
io.stderr = io.__stderr__
e.getvalue()
)") == "oops\n");
    }

    // input()/read() consume from a redirected stdin.
    {
        KiritoVM vm;
        CHECK(run(vm, R"(
var io = import("io")
io.stdin = io.BytesIO("alpha\nbeta\nrest of it")
var a = io.input()
var b = io.input()
var c = io.read()
a + "|" + b + "|" + c
)") == "alpha|beta|rest of it");
    }

    // read(n) reads exactly n characters.
    {
        KiritoVM vm;
        CHECK(run(vm, R"(
var io = import("io")
io.stdin = io.BytesIO("0123456789")
var first = io.read(4)
var second = io.read(3)
first + "|" + second
)") == "0123|456");
    }

    // a real file works as stdout, interchangeably with the std streams.
    {
        KiritoVM vm;
        CHECK(run(vm, R"(
var io = import("io")
var path = import("path").gettempdir() + "/kirito_stream_test.txt"
var f = io.open(path, "w")
io.stdout = f
io.print("into the file")
f.close()
io.stdout = io.__stdout__
var g = io.open(path, "r")
var content = g.read()
g.close()
import("path").remove(path)
content
)") == "into the file\n");
    }

    // duck typing: any object with write() can be stdout.
    {
        KiritoVM vm;
        CHECK(run(vm, R"(
var io = import("io")
class Capture:
    var _init_ = Function(self):
        self.parts = []
    var write = Function(self, s):
        self.parts.append(s)
var c = Capture()
io.stdout = c
io.print("a")
io.print("b")
io.stdout = io.__stdout__
"".join(c.parts)
)") == "a\nb\n");
    }

    // input() with a prompt writes the prompt to stdout, then reads stdin.
    {
        KiritoVM vm;
        CHECK(run(vm, R"(
var io = import("io")
var out = io.BytesIO()
io.stdout = out
io.stdin = io.BytesIO("Alice")
var name = io.input("Name? ")
io.stdout = io.__stdout__
out.getvalue() + "->" + name
)") == "Name? ->Alice");
    }

    return RUN_TESTS();
}
