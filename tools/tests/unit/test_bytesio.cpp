#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

int main() {
    KiritoVM vm;

    // write + getvalue (the "encode into a memory buffer" use-case)
    CHECK(evalStr(vm, R"(
var io = import("io")
var b = io.BytesIO()
b.write("Hello, ")
b.write("World!")
b.getvalue()
)") == "Hello, World!");

    // write returns the number of bytes written
    CHECK(evalStr(vm, "var io = import(\"io\")\nvar b = io.BytesIO()\nb.write(\"abcde\")") == "5");

    // tell tracks the cursor
    CHECK(evalStr(vm, R"(
var io = import("io")
var b = io.BytesIO()
b.write("12345")
b.tell()
)") == "5");

    // seek + read
    CHECK(evalStr(vm, R"(
var io = import("io")
var b = io.BytesIO()
b.write("Hello, World!")
b.seek(0)
b.read(5)
)") == "Hello");
    CHECK(evalStr(vm, R"(
var io = import("io")
var b = io.BytesIO()
b.write("Hello, World!")
b.seek(7)
b.read()
)") == "World!");

    // read() with no arg reads all remaining
    CHECK(evalStr(vm, R"(
var io = import("io")
var b = io.BytesIO("full contents")
b.read()
)") == "full contents");

    // reading past the end yields empty
    CHECK(evalStr(vm, R"(
var io = import("io")
var b = io.BytesIO("xy")
b.read()
b.read()
)") == "");

    // construct from initial bytes; getvalue is non-consuming
    CHECK(evalStr(vm, R"(
var io = import("io")
var b = io.BytesIO("preset")
var v1 = b.getvalue()
var v2 = b.getvalue()
v1 + "|" + v2
)") == "preset|preset");

    // overwrite semantics: seeking back and writing overwrites in place
    CHECK(evalStr(vm, R"(
var io = import("io")
var b = io.BytesIO()
b.write("AAAAA")
b.seek(1)
b.write("BB")
b.getvalue()
)") == "ABBAA");

    // size reflects buffer length, not cursor
    CHECK(evalStr(vm, R"(
var io = import("io")
var b = io.BytesIO()
b.write("hello")
b.seek(0)
b.size()
)") == "5");

    // seek whence: 1 = relative, 2 = from end
    CHECK(evalStr(vm, R"(
var io = import("io")
var b = io.BytesIO("0123456789")
b.seek(3)
b.seek(2, 1)
b.read(1)
)") == "5");
    CHECK(evalStr(vm, R"(
var io = import("io")
var b = io.BytesIO("0123456789")
b.seek(-2, 2)
b.read()
)") == "89");

    // truncate at the cursor
    CHECK(evalStr(vm, R"(
var io = import("io")
var b = io.BytesIO("keep this and drop that")
b.seek(9)
b.truncate()
b.getvalue()
)") == "keep this");

    // binary-safe: write bytes with NUL and high values, read back intact
    CHECK(evalStr(vm, R"(
var io = import("io")
var b = io.BytesIO()
b.write("a\x00b\xffc")
len(b.getvalue())
)") == "5");

    // works as a context manager
    CHECK(evalStr(vm, R"(
var io = import("io")
var result = ""
with io.BytesIO() as b:
    b.write("inside")
    result = b.getvalue()
result
)") == "inside");

    // round-trips a zlib-compressed payload through the buffer (real binary workflow)
    CHECK(evalStr(vm, R"(
var io = import("io")
var zlib = import("zlib")
var buf = io.BytesIO()
buf.write(zlib.compress("the payload to compress and store"))
buf.seek(0)
zlib.decompress(buf.read())
)") == "the payload to compress and store");

    // hostile: writing a non-String throws
    CHECK_THROWS(vm.runSource("var io = import(\"io\")\nvar b = io.BytesIO()\nb.write(42)"));
    CHECK_THROWS(vm.runSource("var io = import(\"io\")\nio.BytesIO(123)"));

    return RUN_TESTS();
}
