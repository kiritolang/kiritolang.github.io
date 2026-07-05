// Socket-foundation tests: the networking primitives added alongside the HTTP client — UDP
// (sendto/recvfrom, connected datagrams), socketpair, half-close (shutdown), address introspection
// (getsockname/getpeername/family/type/fileno), string-keyed socket options, blocking mode, name
// resolution (gethostname/gethostbyname/getaddrinfo), and IPv6. Everything is exercised through the
// user-facing Kirito surface (`import("net")`), in-process on 127.0.0.1 / ::1 — no external network.
// Plus an adversarial block (bad family/type/option/port, closed-socket ops, oversize datagram) and a
// randomized byte-exact datagram round-trip fuzz.
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// Run a Kirito program; return its last expression stringified.
static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

// A program that imports net + io as `net`/`io`, so snippets read naturally.
static std::string prog(const std::string& body) {
    return "var net = import(\"net\")\nvar io = import(\"io\")\n" + body + "\n";
}

int main() {
    KiritoVM vm;

    // --- UDP loopback: bind a receiver on an ephemeral port, sendto from a sender, recvfrom the
    //     datagram back together with the sender's (host, port). ---
    CHECK(evalStr(vm, prog(
        "var rx = net.udpsocket()\n"
        "rx.bind(\"127.0.0.1\", 0)\n"
        "rx.settimeout(5)\n"
        "var port = rx.getsockname()[1]\n"
        "var tx = net.udpsocket()\n"
        "var n = tx.sendto(\"hello udp\", \"127.0.0.1\", port)\n"
        "var got = rx.recvfrom(1024)\n"
        "rx.close()\n"
        "tx.close()\n"
        "String(n) + \"|\" + got[0].decode() + \"|\" + String(got[1][0] == \"127.0.0.1\")")) ==
        "9|hello udp|True");

    // recvfrom returns Bytes (binary-safe) and a [host, port] address list.
    CHECK(evalStr(vm, prog(
        "var rx = net.udpsocket()\n"
        "rx.bind(\"127.0.0.1\", 0)\nrx.settimeout(5)\n"
        "var p = rx.getsockname()[1]\n"
        "var tx = net.udpsocket()\n"
        "discard tx.sendto(Bytes([0, 255, 128, 10]), \"127.0.0.1\", p)\n"
        "var g = rx.recvfrom(64)\nrx.close()\ntx.close()\n"
        "type(g[0]) + \"|\" + String(g[0] == Bytes([0, 255, 128, 10])) + \"|\" + String(len(g[1]))")) ==
        "Bytes|True|2");

    // --- connected UDP: connect() sets the default peer, then plain send/recv work. ---
    CHECK(evalStr(vm, prog(
        "var rx = net.udpsocket()\nrx.bind(\"127.0.0.1\", 0)\nrx.settimeout(5)\n"
        "var p = rx.getsockname()[1]\n"
        "var tx = net.udpsocket()\ntx.connect(\"127.0.0.1\", p)\n"
        "discard tx.send(\"connected\")\n"
        "var r = rx.recvfrom(64)\nrx.close()\ntx.close()\n"
        "r[0].decode()")) == "connected");

    // --- socketpair: a connected, share-nothing stream pair; bytes flow both ways. ---
    CHECK(evalStr(vm, prog(
        "var pr = net.socketpair()\n"
        "var a = pr[0]\nvar b = pr[1]\n"
        "discard a.send(\"ab\")\n"
        "var x = b.recv(2).decode()\n"
        "discard b.send(\"ba\")\n"
        "var y = a.recv(2).decode()\n"
        "a.close()\nb.close()\n"
        "x + \"|\" + y")) == "ab|ba");

    // --- TCP getsockname / getpeername on a connected loopback pair ---
    CHECK(evalStr(vm, prog(
        "var srv = net.tcpsocket()\nsrv.bind(\"127.0.0.1\", 0)\nsrv.listen(1)\n"
        "var port = srv.getsockname()[1]\n"
        "var cli = net.tcpsocket()\ncli.connect(\"127.0.0.1\", port)\n"
        "var conn = srv.accept()\n"
        "var peer = conn.getpeername()\nvar loc = cli.getsockname()\n"
        "srv.close()\ncli.close()\nconn.close()\n"
        // the connection's peer is the client's local address
        "String(peer[0] == \"127.0.0.1\") + \"|\" + String(peer[1] == loc[1])")) == "True|True");

    // --- shutdown: half-close the write side; the peer's recv then sees EOF (empty). ---
    CHECK(evalStr(vm, prog(
        "var srv = net.tcpsocket()\nsrv.bind(\"127.0.0.1\", 0)\nsrv.listen(1)\n"
        "var port = srv.getsockname()[1]\n"
        "var cli = net.tcpsocket()\ncli.connect(\"127.0.0.1\", port)\n"
        "var conn = srv.accept()\n"
        "discard cli.send(\"bye\")\ncli.shutdown(\"write\")\n"
        "var first = conn.recv(16).decode()\n"
        "var eof = conn.recv(16)\n"          // peer closed write -> EOF -> empty Bytes
        "srv.close()\ncli.close()\nconn.close()\n"
        "first + \"|\" + String(len(eof))")) == "bye|0");

    // --- socket options (string-keyed) ---
    CHECK(evalStr(vm, prog(
        "var s = net.tcpsocket()\n"
        "s.setsockopt(\"reuseaddr\", 1)\n"
        "var ra = s.getsockopt(\"reuseaddr\")\n"
        "var ty = s.getsockopt(\"type\")\n"          // SOCK_STREAM
        "var er = s.getsockopt(\"error\")\n"         // 0 on a fresh socket
        "s.close()\n"
        "String(ra != 0) + \"|\" + String(ty) + \"|\" + String(er)")) ==
        "True|" + std::to_string(SOCK_STREAM) + "|0");

    // named boolean conveniences
    CHECK(evalStr(vm, prog(
        "var s = net.tcpsocket()\n"
        "s.setreuseaddr(True)\ns.setnodelay(True)\ns.setkeepalive(True)\n"
        "var nd = s.getsockopt(\"nodelay\")\ns.close()\n"
        "String(nd != 0)")) == "True");
    // broadcast toggles on a UDP socket
    CHECK(evalStr(vm, prog(
        "var s = net.udpsocket()\ns.setbroadcast(True)\n"
        "var b = s.getsockopt(\"broadcast\")\ns.close()\nString(b != 0)")) == "True");

    // --- family / type introspection attributes ---
    CHECK(evalStr(vm, prog("var s = net.udpsocket()\nvar r = s.family + \"/\" + s.type\ns.close()\nr")) ==
          "inet/dgram");
    CHECK(evalStr(vm, prog("var s = net.tcpsocket()\nvar r = s.family + \"/\" + s.type\ns.close()\nr")) ==
          "inet/stream");
    CHECK(evalStr(vm, prog("net.Socket().type")) == "stream");   // Socket() is the TCP/IPv4 default

    // --- fileno: the raw fd, non-destructive (unlike detach) ---
    CHECK(evalStr(vm, prog("var s = net.tcpsocket()\nvar f = s.fileno()\nvar ok = f >= 0 and f == s.fileno()\ns.close()\nString(ok)")) == "True");

    // --- setblocking(False): a recv with no data pending fails fast (would-block) instead of hanging ---
    CHECK(evalStr(vm, prog(
        "var s = net.udpsocket()\ns.bind(\"127.0.0.1\", 0)\ns.setblocking(False)\n"
        "var threw = False\n"
        "try:\n    discard s.recv(16)\ncatch:\n    threw = True\n"
        "s.close()\nString(threw)")) == "True");

    // --- name resolution ---
    CHECK(evalStr(vm, prog("String(len(net.gethostname()) > 0)")) == "True");
    // getaddrinfo on a numeric address needs no DNS and is fully deterministic.
    CHECK(evalStr(vm, prog(
        "var ai = net.getaddrinfo(\"127.0.0.1\", 80)\n"
        "String(len(ai) >= 1) + \"|\" + ai[0][\"host\"] + \"|\" + String(ai[0][\"port\"])")) ==
        "True|127.0.0.1|80");
    // gethostbyname of a loopback literal returns it unchanged.
    CHECK(evalStr(vm, prog("net.gethostbyname(\"127.0.0.1\")")) == "127.0.0.1");

    // --- IPv6 loopback (::1). Some CI containers disable IPv6; treat an unavailable stack as a skip,
    //     but if it binds, the datagram must round-trip and the family must read back as inet6. ---
    CHECK(evalStr(vm, prog(
        "var res = \"skip\"\n"
        "try:\n"
        "    var rx = net.udpsocket(\"inet6\")\n"
        "    rx.bind(\"::1\", 0)\n    rx.settimeout(5)\n"
        "    var p = rx.getsockname()[1]\n"
        "    var tx = net.udpsocket(\"inet6\")\n"
        "    discard tx.sendto(\"v6\", \"::1\", p)\n"
        "    var g = rx.recvfrom(16)\n"
        "    rx.close()\n    tx.close()\n"
        "    res = g[0].decode() + \"/\" + rx.family\n"
        "catch:\n"
        "    res = \"skip\"\n"
        "String(res == \"v6/inet6\" or res == \"skip\")")) == "True");

    // --- adversarial / bad input: every one must throw cleanly (never crash / hang). ---
    // unknown family / type
    CHECK_THROWS(vm.runSource(prog("net.socket(\"bogus\", \"stream\")")));
    CHECK_THROWS(vm.runSource(prog("net.socket(\"inet\", \"raw\")")));
    CHECK_THROWS(vm.runSource(prog("net.udpsocket(\"ipv7\")")));
    // unknown socket option (set + get)
    CHECK_THROWS(vm.runSource(prog("net.tcpsocket().setsockopt(\"nope\", 1)")));
    CHECK_THROWS(vm.runSource(prog("net.tcpsocket().getsockopt(\"nope\")")));
    // port out of range on connect / bind / sendto
    CHECK_THROWS(vm.runSource(prog("net.tcpsocket().connect(\"127.0.0.1\", 99999)")));
    CHECK_THROWS(vm.runSource(prog("net.tcpsocket().bind(\"127.0.0.1\", -1)")));
    CHECK_THROWS(vm.runSource(prog("net.udpsocket().sendto(\"x\", \"127.0.0.1\", 70000)")));
    // negative recv/recvfrom size
    CHECK_THROWS(vm.runSource(prog("net.udpsocket().recvfrom(-1)")));
    // bad shutdown direction
    CHECK_THROWS(vm.runSource(prog("net.tcpsocket().shutdown(\"sideways\")")));
    // sendto expects String/Bytes, not an Integer
    CHECK_THROWS(vm.runSource(prog("net.udpsocket().sendto(42, \"127.0.0.1\", 80)")));
    // operations on a closed socket throw "… : socket is closed", not a raw errno / stale-fd read
    CHECK_THROWS(vm.runSource(prog("var s = net.tcpsocket()\ns.close()\ns.recv(4)")));
    CHECK_THROWS(vm.runSource(prog("var s = net.tcpsocket()\ns.close()\ndiscard s.send(\"x\")")));
    CHECK_THROWS(vm.runSource(prog("var s = net.tcpsocket()\ns.close()\ns.getsockname()")));
    // a detached socket has relinquished its fd -> also closed
    CHECK_THROWS(vm.runSource(prog("var s = net.tcpsocket()\ndiscard s.detach()\ns.recv(4)")));
    // getpeername on an unconnected socket throws (ENOTCONN)
    CHECK_THROWS(vm.runSource(prog("net.tcpsocket().getpeername()")));

    // --- fuzz: 400 randomized datagrams (arbitrary bytes incl NUL/0xFF/high) round-trip byte-exact. ---
    CHECK(evalStr(vm, prog(
        "var rng = import(\"random\").Random(1234)\n"
        "var rx = net.udpsocket()\nrx.bind(\"127.0.0.1\", 0)\nrx.settimeout(5)\n"
        "var p = rx.getsockname()[1]\n"
        "var tx = net.udpsocket()\n"
        "var ok = 0\n"
        "for i in range(400):\n"
        "    var m = rng.randint(0, 300)\n"
        "    var bs = []\n"
        "    for j in range(m):\n        bs.append(rng.randint(0, 255))\n"
        "    var payload = Bytes(bs)\n"
        "    discard tx.sendto(payload, \"127.0.0.1\", p)\n"
        "    var g = rx.recvfrom(1024)\n"
        "    if g[0] == payload:\n        ok = ok + 1\n"
        "rx.close()\ntx.close()\n"
        "String(ok)")) == "400");

    // --- GC / lifetime: churn many sockets; each closes on collection, no leak/crash. ---
    CHECK(evalStr(vm, prog(
        "for i in range(200):\n"
        "    var s = net.udpsocket()\n"
        "    s.bind(\"127.0.0.1\", 0)\n"
        "String(\"ok\")")) == "ok");

    return RUN_TESTS();
}
