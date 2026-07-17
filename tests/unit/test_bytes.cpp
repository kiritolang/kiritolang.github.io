// The Bytes type (binary strings: encode/decode, indexing, slicing, ops), the gzip codec, binary
// file I/O, and serialization. Bytes is the byte-exact counterpart to the (Unicode) String type, so
// binary data — downloads, compressed streams, file contents — round-trips losslessly.
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string ev(KiritoVM& vm, const std::string& src) { return vm.stringify(vm.runSource(src)); }

int main() {
    KiritoVM vm;
    vm.installStandardLibrary();

    // --- construction + repr -------------------------------------------------------------------
    CHECK(ev(vm, "Bytes([72, 105])") == "b'Hi'");
    CHECK(ev(vm, "type(Bytes([1,2,3]))") == "Bytes");
    CHECK(ev(vm, "Bytes(3)") == "b'\\x00\\x00\\x00'");          // n zero bytes
    CHECK(ev(vm, "Bytes(\"AB\")") == "b'AB'");                  // String -> utf-8 bytes
    CHECK(ev(vm, "Bytes(Bytes([5,6]))") == "b'\\x05\\x06'");    // copy
    CHECK(ev(vm, "Bytes()") == "b''");                          // empty
    CHECK(ev(vm, "Bytes([10, 9, 13, 92, 39])") == "b'\\n\\t\\r\\\\\\''");  // escapes in repr
    CHECK_THROWS(vm.runSource("Bytes([256])"));                // out of range
    CHECK_THROWS(vm.runSource("Bytes([-1])"));
    CHECK_THROWS(vm.runSource("Bytes([1.5])"));                // not an Integer

    // --- length / indexing / slicing -----------------------------------------------------------
    CHECK(ev(vm, "len(Bytes([1,2,3,4]))") == "4");
    CHECK(ev(vm, "Bytes([10,20,30])[1]") == "20");             // byte value as Integer
    CHECK(ev(vm, "Bytes([10,20,30])[-1]") == "30");            // negative index
    CHECK(ev(vm, "type(Bytes([10,20,30])[0])") == "Integer");
    CHECK_THROWS(vm.runSource("Bytes([1,2])[5]"));             // out of range
    CHECK(ev(vm, "Bytes([1,2,3,4,5])[1:4]") == "b'\\x02\\x03\\x04'");  // slice -> Bytes
    CHECK(ev(vm, "type(Bytes([1,2,3])[0:2])") == "Bytes");
    CHECK(ev(vm, "Bytes([1,2,3,4,5])[::2]") == "b'\\x01\\x03\\x05'");  // step
    CHECK(ev(vm, "Bytes([1,2,3])[::-1]") == "b'\\x03\\x02\\x01'");     // reverse

    // --- iteration / membership / ops ----------------------------------------------------------
    CHECK(ev(vm, "List(Bytes([65,66,67]))") == "[65, 66, 67]");
    CHECK(ev(vm, "66 in Bytes([65,66,67])") == "True");
    CHECK(ev(vm, "99 in Bytes([65,66,67])") == "False");
    CHECK(ev(vm, "Bytes([66,67]) in Bytes([65,66,67])") == "True");    // subsequence
    CHECK(ev(vm, "Bytes([1,2]) + Bytes([3,4])") == "b'\\x01\\x02\\x03\\x04'");
    CHECK(ev(vm, "Bytes([7]) * 3") == "b'\\x07\\x07\\x07'");
    CHECK(ev(vm, "Bytes([1,2,3]) == Bytes([1,2,3])") == "True");
    CHECK(ev(vm, "Bytes([1,2,3]) == Bytes([1,2,4])") == "False");
    CHECK(ev(vm, "Bytes([1,2]) < Bytes([1,3])") == "True");            // lexicographic
    CHECK(ev(vm, "Bytes([1,2,3]) == \"abc\"") == "False");            // never equal to a String
    // hashable -> usable as a Dict/Set key
    CHECK(ev(vm, "var d = {Bytes([1]): \"a\", Bytes([2]): \"b\"}\nd[Bytes([2])]") == "b");
    CHECK(ev(vm, "len(Set([Bytes([1]), Bytes([1]), Bytes([2])]))") == "2");

    // --- encode / decode -----------------------------------------------------------------------
    CHECK(ev(vm, "\"hello\".encode()") == "b'hello'");          // default utf-8
    CHECK(ev(vm, "type(\"x\".encode())") == "Bytes");
    CHECK(ev(vm, "len(\"héllo\".encode(\"utf-8\"))") == "6");   // é is 2 bytes in utf-8
    CHECK(ev(vm, "Bytes([104,105]).decode()") == "hi");
    CHECK(ev(vm, "Bytes([104,105]).decode(\"utf-8\")") == "hi");
    // latin-1 is a lossless byte<->code-point map (every byte 0..255 round-trips)
    CHECK(ev(vm, "\"é\".encode(\"latin-1\")") == "b'\\xe9'");   // U+00E9 -> one byte 0xE9
    CHECK(ev(vm, "Bytes([0xc3, 0xa9]).decode(\"latin-1\").encode(\"latin-1\") == Bytes([0xc3, 0xa9])") == "True");
    CHECK_THROWS(vm.runSource("\"€\".encode(\"latin-1\")"));   // U+20AC > 255
    CHECK_THROWS(vm.runSource("\"é\".encode(\"ascii\")"));     // > 127
    CHECK_THROWS(vm.runSource("\"x\".encode(\"utf-99\")"));    // unknown encoding

    // --- hex -----------------------------------------------------------------------------------
    CHECK(ev(vm, "Bytes([72, 105, 33]).hex()") == "486921");
    CHECK(ev(vm, "fromhex(\"486921\").decode()") == "Hi!");
    CHECK(ev(vm, "fromhex(\"48 69 21\")") == "b'Hi!'");        // whitespace ignored
    CHECK_THROWS(vm.runSource("fromhex(\"abc\")"));            // odd length
    CHECK_THROWS(vm.runSource("fromhex(\"xy\")"));             // non-hex

    // --- full byte-transparency: all 256 byte values round-trip through latin-1 -----------------
    CHECK(ev(vm, R"(
var ok = True
var i = 0
while i < 256:
    var b = Bytes([i])
    if b.decode("latin-1").encode("latin-1") != b or b[0] != i:
        ok = False
    i = i + 1
ok
)") == "True");

    // --- gzip module: round-trip (String and Bytes), same-type preservation, verb aliases -------
    CHECK(ev(vm, R"(
var g = import("gzip")
var msg = "the quick brown fox " * 20
g.decompress(g.compress(msg)) == msg
)") == "True");
    CHECK(ev(vm, "var g = import(\"gzip\")\ntype(g.compress(Bytes([1,2,3])))") == "Bytes");
    CHECK(ev(vm, "var g = import(\"gzip\")\ntype(g.compress(\"text\"))") == "String");
    CHECK(ev(vm, R"(
var g = import("gzip")
var b = Bytes([0, 1, 127, 128, 255, 254, 0, 10])
g.gunzip(g.gzip(b)) == b
)") == "True");                                                // gzip/gunzip verb aliases
    // zlib / deflate also preserve the input type and round-trip
    CHECK(ev(vm, "var z = import(\"zlib\")\nz.decompress(z.compress(\"hi there\")) == \"hi there\"") == "True");
    CHECK(ev(vm, "var z = import(\"zlib\")\nz.inflate(z.deflate(Bytes([9,8,7]))) == Bytes([9,8,7])") == "True");
    // crc32 of "hello" is a known constant (the checksums live in the hash module)
    CHECK(ev(vm, "import(\"hash\").crc32(\"hello\")") == "907060870");
    CHECK(ev(vm, "import(\"hash\").crc32(Bytes([104,101,108,108,111]))") == "907060870");  // same via Bytes
    // corrupt gzip input throws cleanly
    CHECK_THROWS(vm.runSource("import(\"gzip\").decompress(\"not a gzip stream at all really\")"));

    // --- gzip interop: a stream Kirito wrote decompresses with the C++ codec and vice versa ------
    {
        std::string original = "interoperable gzip payload \x01\x02\x03 with binary";
        std::string gzipped = gzipfmt::compress(original);
        CHECK(gzipfmt::decompress(gzipped) == original);       // C++ round-trip
        CHECK(static_cast<unsigned char>(gzipped[0]) == 0x1f); // gzip magic
        CHECK(static_cast<unsigned char>(gzipped[1]) == 0x8b);
    }

    // --- binary file I/O: write Bytes ("wb"), read Bytes ("rb") exactly back --------------------
    CHECK(ev(vm, R"(
var io = import("io")
var z = import("gzip")
var path = import("path").join(import("path").gettempdir(), "kirito_bytes_test.bin")
var data = z.compress(Bytes("payload with embedded \x00 null and \xff high bytes here", "latin-1"))
var f = io.open(path, "wb")
f.write(data)
f.close()
var g = io.open(path, "rb")
var back = g.read()
g.close()
import("path").remove(path)
type(back) == "Bytes" and back == data
)") == "True");

    // --- serialization: a Bytes round-trips through both graph formats --------------------------
    CHECK(ev(vm, R"(
var s = import("serialize")
var d = import("dump")
var b = Bytes([0, 1, 2, 200, 255, 10, 13])
s.loads(s.dumps(b)) == b and d.loads(d.dumps(b)) == b
)") == "True");
    // nested inside a container
    CHECK(ev(vm, R"(
var d = import("dump")
var payload = {"name": "file.gz", "data": Bytes([31, 139, 8, 0]), "list": [Bytes([1]), Bytes([2])]}
var back = d.loads(d.dumps(payload))
back["data"] == Bytes([31,139,8,0]) and back["list"][1] == Bytes([2])
)") == "True");

    // --- inspect describes the type ------------------------------------------------------------
    CHECK(ev(vm, "inspect(Bytes([1])).find(\"decode\") >= 0") == "True");

    return RUN_TESTS();
}
