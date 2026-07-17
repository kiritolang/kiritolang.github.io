// C++-level regression tests for the deep audit findings. Pin each fix from C++ (independent of the
// .ki probe suite): Integer(String) honours 0x/0o/0b prefixes; List inspect strings advertise the
// actual kwarg names; csv.parse keeps RFC-4180 quoted newlines; net.urlsplit handles bracketed IPv6.
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string ev(KiritoVM& vm, const std::string& src) { return vm.stringify(vm.runSource(src)); }
static bool throws(KiritoVM& vm, const std::string& src) {
    try { vm.runSource(src); return false; } catch (...) { return true; }
}

int main() {
    // ---- Integer(String) accepts 0x/0o/0b prefixes (matching the documented promise) ----
    {
        KiritoVM vm;
        CHECK(ev(vm, "Integer(\"0xFF\")") == "255");
        CHECK(ev(vm, "Integer(\"0o17\")") == "15");
        CHECK(ev(vm, "Integer(\"0b1010\")") == "10");
        CHECK(ev(vm, "Integer(\"-0xFF\")") == "-255");
        CHECK(ev(vm, "Integer(\"+0xFF\")") == "255");
        CHECK(ev(vm, "Integer(\"0X1A\")") == "26");      // uppercase 0X
        CHECK(ev(vm, "Integer(\" 0xFF \")") == "255");   // surrounding whitespace tolerated
        // garbage still rejected
        CHECK(throws(vm, "Integer(\"0xZZ\")"));          // bad hex digit
        CHECK(throws(vm, "Integer(\"42abc\")"));         // trailing garbage
        CHECK(throws(vm, "Integer(\"0x\")"));            // prefix alone, no value
        CHECK(throws(vm, "Integer(\"0b3\")"));           // 3 isn't a binary digit
        // decimal still works as before
        CHECK(ev(vm, "Integer(\"42\")") == "42");
        CHECK(ev(vm, "Integer(\"-42\")") == "-42");
    }

    // ---- List inspect strings match the actual kwarg names (count/index/remove use 'value') ----
    {
        KiritoVM vm;
        // inspect mentions the bound name 'value', not the old 'item'
        CHECK(ev(vm, "\"count(value)\" in inspect([1, 2])") == "True");
        CHECK(ev(vm, "\"index(value, start, end)\" in inspect([1, 2])") == "True");
        CHECK(ev(vm, "\"remove(value)\" in inspect([1, 2])") == "True");
        // and the bindings now actually accept that name
        CHECK(ev(vm, "[1, 2, 2, 3].count(value = 2)") == "2");
        CHECK(ev(vm, "[1, 2, 3].index(value = 2)") == "1");
        CHECK(ev(vm, "var xs = [1, 2, 3]\nxs.remove(value = 2)\nxs") == "[1, 3]");
        // and `item` is now rejected (was never the right name despite the old inspect string)
        CHECK(throws(vm, "[1].count(item = 1)"));
    }

    // ---- csv.parse keeps RFC-4180 quoted newlines as part of the field, not as a row separator ----
    {
        KiritoVM vm;
        CHECK(ev(vm, "import(\"csv\").parse(\"\\\"a\\nb\\\",c\")") == "[[a\nb, c]]");
        // trailing newline doesn't add an empty row
        CHECK(ev(vm, "len(import(\"csv\").parse(\"x,y\\n\"))") == "1");
        // blank middle line IS preserved (an empty row)
        CHECK(ev(vm, "len(import(\"csv\").parse(\"a,b\\n\\nc,d\"))") == "3");
        // doubled-quote escape inside a quoted field still works
        CHECK(ev(vm, "import(\"csv\").parse(\"\\\"he said \\\"\\\"hi\\\"\\\"\\\",ok\")")
              == "[[he said \"hi\", ok]]");
        // round-trip a row with quoteworthy content
        CHECK(ev(vm, "var c = import(\"csv\")\nvar data = [[\"x,y\", \"z\\nw\"], [\"a\", \"b\"]]\n"
                     "c.parse(c.format(data)) == data") == "True");
    }

    // ---- net.urlsplit handles bracketed IPv6 literals; port is the suffix after ']' ----
    {
        KiritoVM vm;
        CHECK(ev(vm, "import(\"net\").urlsplit(\"http://[::1]:8080/path\")[\"host\"]") == "[::1]");
        CHECK(ev(vm, "import(\"net\").urlsplit(\"http://[::1]:8080/path\")[\"port\"]") == "8080");
        CHECK(ev(vm, "import(\"net\").urlsplit(\"http://[::1]:8080/path\")[\"path\"]") == "/path");
        // IPv6 without an explicit port
        CHECK(ev(vm, "import(\"net\").urlsplit(\"http://[2001:db8::1]/\")[\"host\"]")
              == "[2001:db8::1]");
        CHECK(ev(vm, "import(\"net\").urlsplit(\"http://[2001:db8::1]/\")[\"port\"]") == "");
        // ordinary hostnames still work
        CHECK(ev(vm, "import(\"net\").urlsplit(\"http://example.com:80/p\")[\"host\"]")
              == "example.com");
        CHECK(ev(vm, "import(\"net\").urlsplit(\"http://example.com:80/p\")[\"port\"]") == "80");
    }

    // ---- Dict.remove on a missing key throws (doc says: "throws if absent; like pop but returns
    //      nothing"). Before the fix the bool return of DictVal::remove was discarded and the
    //      method was a silent no-op.
    {
        KiritoVM vm;
        // present key: removes and returns None
        CHECK(ev(vm, "var d = {\"a\": 1, \"b\": 2}\nd.remove(\"a\")\nd") == "{b: 2}");
        // missing key: throws
        CHECK(throws(vm, "var d = {\"a\": 1}\nd.remove(\"missing\")"));
        // and the dict is unchanged after the throw
        CHECK(ev(vm, "var d = {\"a\": 1}\ntry:\n    d.remove(\"missing\")\ncatch as e:\n    pass\nd") == "{a: 1}");
        // kwarg form `key=` still works
        CHECK(ev(vm, "var d = {\"x\": 5}\nd.remove(key = \"x\")\nd") == "{}");
    }

    // ---- parseDouble: a subnormal/underflowing float no longer crashes the lexer or the
    //      serializers (std::stod threw std::out_of_range on underflow -> SIGABRT). It now parses
    //      to the (subnormal) value everywhere a double is read from text.
    {
        KiritoVM vm;
        // a subnormal literal lexes instead of aborting; it's tiny and positive
        CHECK(ev(vm, "var x = 5e-324\nx > 0.0 and x < 1e-300") == "True");
        CHECK(ev(vm, "1e-308 > 0.0") == "True");
        // Float(String) accepts a subnormal too
        CHECK(ev(vm, "Float(\"5e-324\") > 0.0") == "True");
        // genuine overflow still throws (unchanged: parseDouble treats ±inf as out-of-range, and
        // the converter surfaces it as a clear conversion error — only underflow was the crash)
        CHECK(throws(vm, "Float(\"1e400\")"));
        // serialize (text) + dump (binary) round-trip a subnormal that std::stod would have rejected
        CHECK(ev(vm, "var s = import(\"serialize\")\nvar t = 1e-308 * 0.001\ns.loads(s.dumps(t)) == t") == "True");
        CHECK(ev(vm, "var d = import(\"dump\")\nvar t = 1e-308 * 0.001\nd.loads(d.dumps(t)) == t") == "True");
    }

    // ---- json emits and re-parses the bareword non-finite spelling (NaN / Infinity /
    //      -Infinity), so a structure with a non-finite Float round-trips (was lowercase nan/inf,
    //      which json.parse rejected).
    {
        KiritoVM vm;
        CHECK(ev(vm, "import(\"json\").dumps(import(\"math\").inf)") == "Infinity");
        CHECK(ev(vm, "import(\"json\").dumps(-import(\"math\").inf)") == "-Infinity");
        CHECK(ev(vm, "import(\"json\").dumps(import(\"math\").nan)") == "NaN");
        CHECK(ev(vm, "import(\"json\").parse(\"Infinity\") == import(\"math\").inf") == "True");
        CHECK(ev(vm, "import(\"json\").parse(\"-Infinity\") == -import(\"math\").inf") == "True");
        CHECK(ev(vm, "import(\"math\").isnan(import(\"json\").parse(\"NaN\"))") == "True");
        CHECK(ev(vm, "var j = import(\"json\")\nvar m = import(\"math\")\n"
                     "j.loads(j.dumps([m.inf, -m.inf])) == [m.inf, -m.inf]") == "True");
    }

    // ---- round-4 fixes: float-literal overflow, tensor negative axis, UTF-8 decode validation,
    //      copy/deepcopy of a class instance, heapq.nlargest negative n ----
    {
        KiritoVM vm;
        // a too-large float literal overflows to +inf instead of crashing the parser (was SIGABRT)
        CHECK(ev(vm, "import(\"math\").isinf(1e999999)") == "True");
        CHECK(ev(vm, "1e999999 > 0.0 and -1e999999 < 0.0") == "True");
        // a subnormal literal still parses (underflow), doesn't crash
        CHECK(ev(vm, "5e-324 > 0.0") == "True");
    }
    {
        KiritoVM vm;
        // tensor negative axis is NumPy-style (-1 = last axis), not the whole-tensor collapse
        const std::string mk = "var T = import(\"tensor\")\nvar a = T.Tensor([[1.0, 2, 3], [4, 5, 6]])\n";
        CHECK(ev(vm, mk + "a.sum(-1).tolist()") == "[6.0, 15.0]");      // last axis
        CHECK(ev(vm, mk + "a.sum(-2).tolist()") == "[5.0, 7.0, 9.0]");  // axis 0
        CHECK(ev(vm, mk + "a.argmax(-1).tolist()") == "[2.0, 2.0]");
        CHECK(ev(vm, mk + "a.sum()") == "21.0");                        // no axis -> whole-tensor scalar
        CHECK(throws(vm, mk + "a.sum(-3)"));                            // out of range
        CHECK(throws(vm, mk + "a.sum(2)"));
    }
    {
        KiritoVM vm;
        // decode("utf-8") validates: invalid byte sequences throw; valid (multibyte) still decodes
        CHECK(ev(vm, "Bytes([104, 195, 169]).decode(\"utf-8\")") == "h\xc3\xa9");
        CHECK(ev(vm, "\"caf\xc3\xa9\".encode(\"utf-8\").decode(\"utf-8\")") == "caf\xc3\xa9");
        CHECK(throws(vm, "Bytes([255, 254, 128]).decode(\"utf-8\")"));  // stray bytes
        CHECK(throws(vm, "Bytes([0xC3]).decode(\"utf-8\")"));            // truncated 2-byte seq
        CHECK(throws(vm, "Bytes([0xED, 0xA0, 0x80]).decode(\"utf-8\")")); // surrogate U+D800
        // latin-1/ascii decode error reports the byte in hex
        CHECK(throws(vm, "Bytes([200]).decode(\"ascii\")"));
    }
    {
        KiritoVM vm;
        // copy/deepcopy of a class instance is an INDEPENDENT object (was the same object)
        const std::string box = "var c = import(\"copy\")\nclass Box:\n    var _init_ = Function(self, v):\n        self.v = v\nvar a = Box(1)\n";
        CHECK(ev(vm, box + "var b = c.copy(a)\nb.v = 99\na.v") == "1");
        CHECK(ev(vm, box + "var b = c.deepcopy(a)\nb.v = 77\na.v") == "1");
        // immutable scalars and containers unchanged
        CHECK(ev(vm, "import(\"copy\").copy(5)") == "5");
        CHECK(ev(vm, "var c = import(\"copy\")\nvar x = [1, 2]\nvar y = c.copy(x)\ny.append(3)\nx") == "[1, 2]");
    }
    {
        KiritoVM vm;
        // heapq.nlargest with a non-positive n returns [] (matches nsmallest), not a tail slice
        CHECK(ev(vm, "import(\"heapq\").nlargest(-2, [5, 4, 3, 2, 1])") == "[]");
        CHECK(ev(vm, "import(\"heapq\").nlargest(0, [3, 1, 2])") == "[]");
        CHECK(ev(vm, "import(\"heapq\").nlargest(2, [5, 4, 3, 2, 1])") == "[5, 4]");
    }

    // ---- round-5 fixes: BytesIO read after a seek-beyond-end (was a SIGABRT), File.tell after EOF ----
    {
        KiritoVM vm;
        // seek past the end then read/readline returns "" (was: avail underflow -> substr out_of_range -> abort)
        CHECK(ev(vm, "var b = import(\"io\").BytesIO(\"abc\")\ndiscard b.seek(10)\nb.read()") == "");
        CHECK(ev(vm, "var b = import(\"io\").BytesIO(\"abc\")\ndiscard b.seek(10)\nb.readline()") == "");
        // and the stream still works after seeking back in range
        CHECK(ev(vm, "var b = import(\"io\").BytesIO(\"abc\")\ndiscard b.seek(10)\ndiscard b.read()\ndiscard b.seek(1)\nb.read()") == "bc");
        // seek-past-end + write still zero-fills the gap
        CHECK(ev(vm, "var b = import(\"io\").BytesIO(\"ab\")\ndiscard b.seek(5)\ndiscard b.write(\"Z\")\nlen(b)") == "6");
    }
    {
        KiritoVM vm;
        // File.tell() after an over-long read reports the byte length, not -1 (EOF bit cleared)
        const std::string mk =
            "var io = import(\"io\")\n"
            "var p = import(\"path\").join(import(\"path\").gettempdir(), \"kira_tell_probe.txt\")\n"
            "var w = io.open(p, \"w\")\ndiscard w.write(\"hello\")\nw.close()\n"
            "var f = io.open(p, \"r\")\n";
        CHECK(ev(vm, mk + "discard f.read(1000)\nvar t = f.tell()\nf.close()\ndiscard import(\"path\").remove(p)\nt") == "5");
    }
}
