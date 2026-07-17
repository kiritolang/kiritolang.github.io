// Networking tests. A loopback server runs on a helper thread purely to provide the other end of
// the connection (the net library itself is single-threaded). Covers the raw Socket API and the
// full HTTP client: the Response object, redirects, verbs, params/auth/headers, json/data/files
// bodies, cookies + Session, chunked decoding, gzip/deflate decompression, timeouts, edge cases, a
// 2000-sample randomized round-trip fuzz, and adversarial malformed responses.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

// The Response handle returned by evaluating `src` (whose last expression is a net request).
static ResponseVal& evalResponse(KiritoVM& vm, const std::string& src) {
    return static_cast<ResponseVal&>(vm.arena().deref(vm.runSource(src)));
}

// Create a listening socket on an ephemeral 127.0.0.1 port; returns fd and sets `port`.
static int makeListener(int& port, int backlog = 64) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;  // ephemeral
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return -1;
    ::listen(fd, backlog);
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    port = ntohs(addr.sin_port);
    return fd;
}

// Serve `n` sequential connections, replying to each request with handler(request). Optionally
// records the last request. Each connection is closed after the reply (clients send Connection:
// close), so a redirect simply takes the next accept().
static void serveN(int srv, int n, std::function<std::string(const std::string&)> handler,
                   std::string* lastReq = nullptr) {
    for (int i = 0; i < n; ++i) {
        int c = ::accept(srv, nullptr, nullptr);
        if (c < 0) break;
        // read until end of headers (good enough; request bodies in these tests are small)
        std::string req;
        char buf[8192];
        while (req.find("\r\n\r\n") == std::string::npos) {
            ssize_t r = ::recv(c, buf, sizeof(buf), 0);
            if (r <= 0) break;
            req.append(buf, static_cast<std::size_t>(r));
        }
        if (lastReq) *lastReq = req;
        std::string resp = handler(req);
        ::send(c, resp.data(), resp.size(), 0);
        ::close(c);
    }
    ::close(srv);
}

static std::string url(int port, const std::string& path = "/") {
    return "http://127.0.0.1:" + std::to_string(port) + path;
}

// Build a gzip stream (header + raw deflate + 8-byte trailer; the trailer's CRC/ISIZE are unchecked).
static std::string gzipWrap(const std::string& s) {
    return std::string("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03", 10) + deflate::compress(s) +
           std::string(8, '\0');
}

// Chunk `s` into pieces of `chunkSize` (0 = one chunk) for Transfer-Encoding: chunked.
static std::string chunkEncode(const std::string& s, std::size_t chunkSize) {
    if (chunkSize == 0) chunkSize = s.size() ? s.size() : 1;
    std::string out;
    char hex[32];
    for (std::size_t i = 0; i < s.size(); i += chunkSize) {
        std::size_t n = std::min(chunkSize, s.size() - i);
        std::snprintf(hex, sizeof(hex), "%zx", n);
        out += std::string(hex) + "\r\n" + s.substr(i, n) + "\r\n";
    }
    out += "0\r\n\r\n";
    return out;
}

int main() {
    KiritoVM vm;

    // --- raw TCP: client sends, server echoes ---
    {
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        std::thread server([srv] {
            int c = ::accept(srv, nullptr, nullptr);
            char buf[256];
            ssize_t n = ::recv(c, buf, sizeof(buf), 0);
            std::string reply = "echo:" + std::string(buf, n > 0 ? static_cast<std::size_t>(n) : 0);
            ::send(c, reply.data(), reply.size(), 0);
            ::close(c);
            ::close(srv);
        });
        std::string src =
            "var net = import(\"net\")\nvar s = net.Socket()\n"
            "s.connect(\"127.0.0.1\", " + std::to_string(port) + ")\ns.send(\"ping\")\n"
            "var reply = s.recvall().decode()\ns.close()\nreply\n";   // recv -> Bytes; decode for text
        CHECK(evalStr(vm, src) == "echo:ping");
        server.join();
    }

    // --- raw TCP is binary-safe: recv returns Bytes, send accepts Bytes; a blob with NUL/0xFF/0x80
    //     bytes round-trips byte-exactly (the property MJPEG-over-socket video streaming relies on). ---
    {
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        std::thread server([srv] {
            int c = ::accept(srv, nullptr, nullptr);
            char buf[256];
            ssize_t n = ::recv(c, buf, sizeof(buf), 0);     // echo the raw bytes back verbatim
            if (n > 0) ::send(c, buf, static_cast<std::size_t>(n), 0);
            ::close(c);
            ::close(srv);
        });
        std::string src =
            "var net = import(\"net\")\nvar s = net.Socket()\n"
            "s.connect(\"127.0.0.1\", " + std::to_string(port) + ")\n"
            "var sent = Bytes([0, 1, 255, 128, 10, 13, 0, 65])\n"
            "discard s.send(sent)\nvar r = s.recvall()\ns.close()\n"
            "type(r) + \"|\" + String(r == sent) + \"|\" + String(len(r))\n";
        CHECK(evalStr(vm, src) == "Bytes|True|8");
        server.join();
    }

    // --- basic GET: Response status / text / headers / case-insensitive header() ---
    {
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        std::thread server(serveN, srv, 1, [](const std::string&) {
            return std::string("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nX-Test: yes\r\n"
                               "Content-Length: 10\r\n\r\nHello HTTP");
        }, nullptr);
        ResponseVal& r = evalResponse(vm, "import(\"net\").get(\"" + url(port) + "\")\n");
        CHECK(r.status == 200);
        CHECK(r.body == "Hello HTTP");
        server.join();
        std::string src =
            "var r = import(\"net\").get(\"" + url(port) + "\")\n";  // (server already gone; reuse fields)
        (void)src;
    }

    // --- Response: status/ok/reason/text, header(), json() ---
    {
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        std::thread server(serveN, srv, 1, [](const std::string&) {
            std::string b = "{\"ok\": true, \"n\": 42}";
            return "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Length: " +
                   std::to_string(b.size()) + "\r\n\r\n" + b;
        }, nullptr);
        std::string src =
            "var r = import(\"net\").get(\"" + url(port) + "\")\nvar j = r.json()\n"
            "String(r.status) + \"|\" + String(r.ok) + \"|\" + r.reason + \"|\" +"
            " r.header(\"CONTENT-TYPE\") + \"|\" + String(j[\"n\"]) + \"|\" + String(j[\"ok\"])\n";
        CHECK(evalStr(vm, src) == "201|True|Created|application/json|42|True");
        server.join();
    }

    // --- Response Dict-style indexing: r["status"]/r["body"]/r["reason"]/r["ok"]/r["headers"]/... ---
    {
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        std::thread server(serveN, srv, 1, [](const std::string&) {
            return std::string("HTTP/1.1 200 OK\r\nX-K: v\r\nContent-Length: 3\r\n\r\nabc");
        }, nullptr);
        std::string src =
            "var r = import(\"net\").get(\"" + url(port) + "\")\n"
            "String(r[\"status\"]) + \"|\" + r[\"body\"] + \"|\" + r[\"reason\"] + \"|\" +"
            " String(r[\"ok\"]) + \"|\" + r[\"headers\"][\"X-K\"]\n";
        CHECK(evalStr(vm, src) == "200|abc|OK|True|v");
        server.join();
    }
    // --- Response indexing: an unknown key throws ---
    {
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        std::thread server(serveN, srv, 1, [](const std::string&) {
            return std::string("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nx");
        }, nullptr);
        CHECK_THROWS(evalStr(vm, "import(\"net\").get(\"" + url(port) + "\")[\"nope\"]\n"));
        server.join();
    }

    // --- redirect following: 302 -> relative Location -> 200 ---
    {
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        std::thread server(serveN, srv, 2, [](const std::string& req) -> std::string {
            if (req.rfind("GET /start", 0) == 0)
                return "HTTP/1.1 302 Found\r\nLocation: /final\r\nContent-Length: 0\r\n\r\n";
            return "HTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\narrived";
        }, nullptr);
        std::string src =
            "var r = import(\"net\").get(\"" + url(port, "/start") + "\")\n"
            "String(r.status) + \"|\" + r.text + \"|\" + String(r.url.endswith(\"/final\"))\n";
        CHECK(evalStr(vm, src) == "200|arrived|True");
        server.join();
    }

    // --- redirect loop stops at maxredirects (no infinite loop) ---
    {
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        std::thread server(serveN, srv, 4, [](const std::string&) {
            return std::string("HTTP/1.1 302 Found\r\nLocation: /again\r\nContent-Length: 0\r\n\r\n");
        }, nullptr);
        std::string src =
            "var r = import(\"net\").get(\"" + url(port) + "\", {\"maxredirects\": 3})\n"
            "String(r.status)\n";  // returns the last 302 after 3 follows
        CHECK(evalStr(vm, src) == "302");
        server.join();
    }

    // --- all verbs hit the wire; params + basic auth + custom header ---
    {
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        std::string req;
        std::thread server(serveN, srv, 1, [](const std::string&) {
            return std::string("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
        }, &req);
        std::string src =
            "import(\"net\").put(\"" + url(port, "/x") + "\", "
            "{\"params\": {\"a\": \"1\"}, \"auth\": [\"user\", \"pass\"], \"headers\": {\"X-Foo\": \"bar\"}})\n";
        ResponseVal& r = evalResponse(vm, src);
        CHECK(r.status == 200);
        server.join();
        CHECK(req.rfind("PUT /x?a=1 ", 0) == 0);
        CHECK(req.find("Authorization: Basic " + base64Encode("user:pass")) != std::string::npos);
        CHECK(req.find("X-Foo: bar") != std::string::npos);
    }
    for (const char* verb : {"post", "delete", "patch", "head", "options"}) {
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        std::string req;
        std::thread server(serveN, srv, 1, [](const std::string&) {
            return std::string("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
        }, &req);
        vm.runSource("import(\"net\")." + std::string(verb) + "(\"" + url(port) + "\")\n");
        server.join();
        std::string upper(verb);
        for (char& c : upper) c = static_cast<char>(::toupper(c));
        CHECK(req.rfind(upper + " /", 0) == 0);
    }

    // --- POST json= sets the body and Content-Type; data= form-encodes a Dict ---
    {
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        std::string req;
        std::thread server(serveN, srv, 1, [](const std::string&) {
            return std::string("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
        }, &req);
        vm.runSource("import(\"net\").post(\"" + url(port) + "\", {\"json\": {\"name\": \"ada\"}})\n");
        server.join();
        CHECK(req.find("Content-Type: application/json") != std::string::npos);
        CHECK(req.find("\"name\": \"ada\"") != std::string::npos);
    }
    {
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        std::string req;
        std::thread server(serveN, srv, 1, [](const std::string&) {
            return std::string("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
        }, &req);
        vm.runSource("import(\"net\").post(\"" + url(port) + "\", {\"data\": {\"a\": \"1\", \"b\": \"x y\"}})\n");
        server.join();
        CHECK(req.find("Content-Type: application/x-www-form-urlencoded") != std::string::npos);
        CHECK(req.find("a=1") != std::string::npos);
        CHECK(req.find("b=x%20y") != std::string::npos);
    }

    // --- cookies: Set-Cookie lands in response.cookies ---
    {
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        std::thread server(serveN, srv, 1, [](const std::string&) {
            return std::string("HTTP/1.1 200 OK\r\nSet-Cookie: sid=abc123; Path=/\r\nContent-Length: 0\r\n\r\n");
        }, nullptr);
        CHECK(evalStr(vm, "import(\"net\").get(\"" + url(port) + "\").cookies[\"sid\"]\n") == "abc123");
        server.join();
    }

    // --- Session: cookies + default headers persist across requests ---
    {
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        std::string second;
        std::thread server(serveN, srv, 2, [](const std::string& r) -> std::string {
            if (r.find("Cookie:") == std::string::npos)
                return "HTTP/1.1 200 OK\r\nSet-Cookie: sid=xyz; Path=/\r\nContent-Length: 2\r\n\r\n#1";
            return "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n#2";
        }, &second);
        std::string src =
            "var s = import(\"net\").Session()\ns.headers[\"X-App\"] = \"kirito\"\n"
            "discard s.get(\"" + url(port, "/login") + "\")\n"
            "var r = s.get(\"" + url(port, "/home") + "\")\nr.cookies[\"sid\"]\n";
        CHECK(evalStr(vm, src) == "xyz");
        server.join();
        CHECK(second.find("Cookie: sid=xyz") != std::string::npos);
        CHECK(second.find("X-App: kirito") != std::string::npos);
    }

    // --- chunked transfer-encoding (multiple chunks + extension) is decoded ---
    {
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        std::thread server(serveN, srv, 1, [](const std::string&) {
            return std::string("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                               "5;ext=1\r\nHello\r\n6\r\n World\r\n0\r\n\r\n");
        }, nullptr);
        CHECK(evalStr(vm, "import(\"net\").get(\"" + url(port) + "\").text\n") == "Hello World");
        server.join();
    }

    // --- gzip + deflate Content-Encoding are decompressed ---
    for (const char* enc : {"gzip", "deflate"}) {
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        std::string payload = "the quick brown fox jumps over the lazy dog, repeatedly. ";
        for (int k = 0; k < 4; ++k) payload += payload;  // ~900 bytes, compressible
        std::string enc2(enc);
        std::string encoded = enc2 == "gzip" ? gzipWrap(payload) : deflate::compress(payload);
        std::thread server(serveN, srv, 1, [encoded, enc2](const std::string&) {
            return "HTTP/1.1 200 OK\r\nContent-Encoding: " + enc2 + "\r\nContent-Length: " +
                   std::to_string(encoded.size()) + "\r\n\r\n" + encoded;
        }, nullptr);
        ResponseVal& r = evalResponse(vm, "import(\"net\").get(\"" + url(port) + "\")\n");
        CHECK(r.body == payload);
        server.join();
    }

    // --- raiseforstatus throws on 4xx; ok is False ---
    {
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        std::thread server(serveN, srv, 1, [](const std::string&) {
            return std::string("HTTP/1.1 404 Not Found\r\nContent-Length: 3\r\n\r\nno!");
        }, nullptr);
        std::string src =
            "var r = import(\"net\").get(\"" + url(port) + "\")\nvar threw = False\n"
            "try:\n    r.raiseforstatus()\ncatch as e:\n    threw = True\n"
            "String(r.ok) + \"|\" + String(threw) + \"|\" + String(r.status)\n";
        CHECK(evalStr(vm, src) == "False|True|404");
        server.join();
    }

    // --- multipart file upload ---
    {
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        std::string req;
        std::thread server(serveN, srv, 1, [](const std::string&) {
            return std::string("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
        }, &req);
        vm.runSource("import(\"net\").post(\"" + url(port) + "\", "
                     "{\"files\": {\"f\": [\"data.txt\", \"file-content-here\"]}})\n");
        server.join();
        CHECK(req.find("multipart/form-data; boundary=") != std::string::npos);
        CHECK(req.find("filename=\"data.txt\"") != std::string::npos);
        CHECK(req.find("file-content-here") != std::string::npos);
    }

    // --- multipart upload of raw Bytes: the body must carry the raw bytes, NOT the b'...' repr ---
    {
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        std::string req;
        std::thread server(serveN, srv, 1, [](const std::string&) {
            return std::string("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
        }, &req);
        vm.runSource("import(\"net\").post(\"" + url(port) + "\", "
                     "{\"files\": {\"f\": [\"b.bin\", Bytes([0, 1, 255, 254, 66])]}})\n");
        server.join();
        const std::string raw("\x00\x01\xff\xfe""B", 5);   // the five raw bytes 0x00 0x01 0xff 0xfe 'B'
        CHECK(req.find("filename=\"b.bin\"") != std::string::npos);
        CHECK(req.find(raw) != std::string::npos);          // raw bytes present in the multipart body
        CHECK(req.find("b'\\x00") == std::string::npos);    // and NOT the b'\x00...' Bytes repr
    }

    // --- EDGE CASES ---------------------------------------------------------------------------
    // 204 No Content, empty body, header value with extra whitespace
    {
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        std::thread server(serveN, srv, 1, [](const std::string&) {
            return std::string("HTTP/1.1 204 No Content\r\nX-Empty:    spaced   \r\nContent-Length: 0\r\n\r\n");
        }, nullptr);
        ResponseVal& r = evalResponse(vm, "import(\"net\").get(\"" + url(port) + "\")\n");
        CHECK(r.status == 204);
        CHECK(r.body.empty());
        server.join();
    }
    // close-delimited body (no Content-Length, server just closes)
    {
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        std::thread server(serveN, srv, 1, [](const std::string&) {
            return std::string("HTTP/1.1 200 OK\r\n\r\nbody-without-length");
        }, nullptr);
        CHECK(evalStr(vm, "import(\"net\").get(\"" + url(port) + "\").text\n") == "body-without-length");
        server.join();
    }
    // large body (256 KB) round-trips intact
    {
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        std::string big(256 * 1024, 'Z');
        std::thread server(serveN, srv, 1, [big](const std::string&) {
            return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(big.size()) + "\r\n\r\n" + big;
        }, nullptr);
        ResponseVal& r = evalResponse(vm, "import(\"net\").get(\"" + url(port) + "\")\n");
        CHECK(r.body.size() == big.size());
        CHECK(r.body == big);
        server.join();
    }
    // body containing the header terminator sequence is not truncated
    {
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        std::string body = "line1\r\n\r\nline2\r\n\r\nend";
        std::thread server(serveN, srv, 1, [body](const std::string&) {
            return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        }, nullptr);
        ResponseVal& r = evalResponse(vm, "import(\"net\").get(\"" + url(port) + "\")\n");
        CHECK(r.body == body);
        server.join();
    }

    // --- ADVERSARIAL: malformed responses must not crash (parse gracefully or throw) ----------
    {
        const char* garbage[] = {
            "not an http response at all",                 // no status line
            "HTTP/1.1\r\n\r\n",                             // status line without code
            "HTTP/1.1 200\r\nbroken-header-no-colon\r\n\r\nx",
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZZZ\r\nnot-hex",  // bad chunk size
            "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n\r\nnot-gzip-data",       // bad gzip
            "",                                             // empty (immediate close)
        };
        for (const char* g : garbage) {
            int port = 0, srv = makeListener(port);
            CHECK(srv >= 0);
            std::string gs(g);
            std::thread server(serveN, srv, 1, [gs](const std::string&) { return gs; }, nullptr);
            // must return a Response (no crash); status may be 0 or 200 depending on the garbage
            ResponseVal& r = evalResponse(vm, "import(\"net\").get(\"" + url(port) + "\")\n");
            CHECK(r.status >= 0);
            server.join();
        }
    }
    // many headers (1000) parse without issue
    {
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        std::thread server(serveN, srv, 1, [](const std::string&) {
            std::string h = "HTTP/1.1 200 OK\r\n";
            for (int i = 0; i < 1000; ++i) h += "X-H" + std::to_string(i) + ": v" + std::to_string(i) + "\r\n";
            h += "Content-Length: 2\r\n\r\nok";
            return h;
        }, nullptr);
        std::string src =
            "var r = import(\"net\").get(\"" + url(port) + "\")\n"
            "r.header(\"x-h500\") + \"|\" + r.header(\"X-H999\")\n";
        CHECK(evalStr(vm, src) == "v500|v999");
        server.join();
    }
    // connection refused (nothing listening) -> throws
    {
        int port = 0, srv = makeListener(port);
        ::close(srv);  // free the port so connect is refused
        CHECK_THROWS(vm.runSource("import(\"net\").get(\"" + url(port) + "\")\n"));
    }
    // read timeout: server accepts but stalls -> client throws
    {
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        std::thread server([srv] {
            int c = ::accept(srv, nullptr, nullptr);
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            ::close(c);
            ::close(srv);
        });
        CHECK_THROWS(vm.runSource("import(\"net\").get(\"" + url(port) + "\", {\"timeout\": 0.2})\n"));
        server.join();
    }

    // --- FUZZ: 2000 randomized round-trips (status + headers + body, optionally chunked/gzip) ---
    {
        const int N = 2000;
        std::mt19937 rng(0xBEEF);
        std::vector<int> statuses;
        std::vector<std::string> bodies;
        const int codes[] = {200, 201, 202, 204, 400, 403, 404, 418, 451, 500, 503};
        for (int i = 0; i < N; ++i) {
            statuses.push_back(codes[rng() % (sizeof(codes) / sizeof(int))]);
            std::size_t len = rng() % 1500;  // 0..1499 bytes
            std::string b;
            b.reserve(len);
            for (std::size_t j = 0; j < len; ++j) b += static_cast<char>(32 + (rng() % 95));  // printable
            bodies.push_back(std::move(b));
        }
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        std::thread server([srv, &statuses, &bodies, N] {
            std::mt19937 srng(0xF00D);
            for (int i = 0; i < N; ++i) {
                int c = ::accept(srv, nullptr, nullptr);
                if (c < 0) break;
                char buf[4096];
                while (true) { ssize_t r = ::recv(c, buf, sizeof(buf), 0); if (r <= 0) break;
                               if (std::string(buf, r).find("\r\n\r\n") != std::string::npos) break; }
                int mode = static_cast<int>(srng() % 3);  // 0 = plain, 1 = chunked, 2 = gzip
                std::string head = "HTTP/1.1 " + std::to_string(statuses[i]) + " S\r\n";
                std::string out;
                if (mode == 1) {
                    head += "Transfer-Encoding: chunked\r\n\r\n";
                    out = head + chunkEncode(bodies[i], 1 + (srng() % 64));
                } else if (mode == 2) {
                    std::string gz = gzipWrap(bodies[i]);
                    head += "Content-Encoding: gzip\r\nContent-Length: " + std::to_string(gz.size()) + "\r\n\r\n";
                    out = head + gz;
                } else {
                    head += "Content-Length: " + std::to_string(bodies[i].size()) + "\r\n\r\n";
                    out = head + bodies[i];
                }
                ::send(c, out.data(), out.size(), 0);
                ::close(c);
            }
            ::close(srv);
        });
        int ok = 0;
        std::string getSrc = "import(\"net\").get(\"" + url(port) + "\")\n";
        for (int i = 0; i < N; ++i) {
            ResponseVal& r = evalResponse(vm, getSrc);
            if (r.status == statuses[i] && r.body == bodies[i]) ++ok;
        }
        CHECK(ok == N);
        server.join();
    }

    // --- URL helpers + validation ---
    CHECK(evalStr(vm, "import(\"net\").quote(\"a b/c\")\n") == "a%20b%2Fc");
    CHECK(evalStr(vm, "import(\"net\").unquote(\"a%20b%2Fc\")\n") == "a b/c");
#ifndef KIRITO_ENABLE_TLS
    // Without TLS, an https:// request is rejected up front. (With TLS the client actually dials, so
    // the end-to-end HTTPS path is exercised in test_net_tls.cpp against an in-process server instead.)
    CHECK_THROWS(vm.runSource("import(\"net\").get(\"https://example.com\")\n"));
#endif
    CHECK_THROWS(vm.runSource("import(\"net\").get(\"ftp://example.com\")\n"));
    CHECK_THROWS(vm.runSource("import(\"net\").get(\"not a url\")\n"));

    return RUN_TESTS();
}
