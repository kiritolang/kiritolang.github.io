#ifndef KIRITO_STDLIB_NET_HPP
#define KIRITO_STDLIB_NET_HPP

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/time.h>
#endif

#ifdef KIRITO_ENABLE_TLS
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#endif

#include "fum/unordered_set.hpp"
#include "builtins.hpp"
#include "bytes.hpp"
#include "collections.hpp"
#include "deflate.hpp"
#include "native.hpp"
#include "net_compat.hpp"
#include "stdlib_json.hpp"

// On Windows OpenSSL has no default CA store, so we read the system trust ("ROOT") store via the
// CryptoAPI. wincrypt.h must come AFTER net_compat's winsock2.h/windows.h, and it #defines some names
// that clash with OpenSSL's struct typedefs — undo those so the rest of the file sees OpenSSL's types.
#if defined(KIRITO_ENABLE_TLS) && defined(_WIN32)
#include <wincrypt.h>
#undef X509_NAME
#undef X509_EXTENSIONS
#undef PKCS7_ISSUER_AND_SERIAL
#undef PKCS7_SIGNER_INFO
#undef OCSP_REQUEST
#undef OCSP_RESPONSE
#endif

namespace kirito {

// The native-binding idiom below re-uses `vm`/`self` as bound-method lambda parameters that
// intentionally shadow the enclosing getAttr/setup `vm`/`self` (same VM, by design). Silence
// -Wshadow for these mechanical bindings; it stays active in the evaluator/parser/lexer core.
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wshadow"
#endif

// A socket. Wraps an OS socket handle (POSIX fd or Winsock SOCKET), closed automatically when the
// value is collected. There is no global state; you create a Socket and operate on it. The family
// (AF_INET/AF_INET6) and type (SOCK_STREAM/SOCK_DGRAM) are remembered so bind/connect/sendto resolve
// addresses in the right family and datagram vs stream semantics are correct.
class SocketVal : public NativeClass<SocketVal> {
public:
    static constexpr const char* kTypeName = "Socket";
    std::vector<std::string> inspectMembers() const override {
        return {"family: String", "type: String",
                "connect(host, port)", "bind(host, port)", "listen(backlog)", "accept() -> Socket",
                "send(data) -> Integer", "recv(n) -> Bytes", "recvall() -> Bytes",
                "sendto(data, host, port) -> Integer", "recvfrom(n) -> [Bytes, [host, port]]",
                "getsockname() -> [host, port]", "getpeername() -> [host, port]",
                "setsockopt(option, value)", "getsockopt(option) -> Integer",
                "setreuseaddr(flag)", "setnodelay(flag)", "setbroadcast(flag)", "setkeepalive(flag)",
                "setblocking(flag)", "settimeout(seconds)", "shutdown(how)",
                "starttls(server_hostname, verify) -> None", "cipher() -> String", "is_tls: Bool",
                "fileno() -> Integer", "close()", "detach() -> Integer"};
    }
    netcompat::socket_t fd = netcompat::kInvalidSocket;
    int family = AF_INET;
    int socktype = SOCK_STREAM;
    bool closed = false;
    double timeout = 0.0;   // settimeout() value (seconds); 0 = blocking. Bounds connect() too.
    std::string peerHost;   // the host last connect()ed to — starttls uses it as the default SNI/verify name.
#ifdef KIRITO_ENABLE_TLS
    // When a TLS session is active (after starttls) send/recv route through it. The Socket owns both
    // and tears them down before closing the fd. nullptr => plaintext.
    SSL* ssl_ = nullptr;
    SSL_CTX* sslCtx_ = nullptr;
#endif
    bool isTls() const {
#ifdef KIRITO_ENABLE_TLS
        return ssl_ != nullptr;
#else
        return false;
#endif
    }

    SocketVal() : SocketVal(AF_INET, SOCK_STREAM) {}
    SocketVal(int fam, int type) : family(fam), socktype(type) {
        netcompat::startup();
        fd = ::socket(fam, type, 0);
        if (!netcompat::isValid(fd))
            throw KiritoError("socket() failed: " + netcompat::lastError());
    }
    // Adopt an existing fd (accept()/fromfd()/socketpair()); the caller supplies the best-known
    // family/type. All three are required — on POSIX socket_t IS int, so a defaulted 2-arg overload
    // would be ambiguous with SocketVal(fam, type).
    SocketVal(netcompat::socket_t existing, int fam, int type)
        : fd(existing), family(fam), socktype(type) {}
    ~SocketVal() override { closeFd(); }

    void closeFd() {
#ifdef KIRITO_ENABLE_TLS
        if (ssl_) { SSL_shutdown(ssl_); SSL_free(ssl_); ssl_ = nullptr; }
        if (sslCtx_) { SSL_CTX_free(sslCtx_); sslCtx_ = nullptr; }
#endif
        if (netcompat::isValid(fd) && !closed) { netcompat::closeSocket(fd); closed = true; }
    }

#ifdef KIRITO_ENABLE_TLS
    // TLS byte I/O (only reached when isTls()). SSL_write/SSL_read handle the record framing; a clean
    // peer close (SSL_ERROR_ZERO_RETURN) is EOF, everything else is a hard error.
    void tlsSendAll(const std::string& data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            int n = SSL_write(ssl_, data.data() + sent, static_cast<int>(data.size() - sent));
            if (n <= 0) throw KiritoError("SSL_write failed: " + netcompat::lastError());
            sent += static_cast<std::size_t>(n);
        }
    }
    long long tlsRecv(char* buf, std::size_t n) {
        int got = SSL_read(ssl_, buf, static_cast<int>(n));
        if (got > 0) return got;
        int err = SSL_get_error(ssl_, got);
        if (err == SSL_ERROR_ZERO_RETURN) return 0;   // clean TLS close_notify -> EOF
        return -1;
    }
    // Read to EOF over TLS, bounded by `maxBytes` (the caller passes net::kMaxRecvAll — not visible
    // from this in-class context, since the `net` namespace opens further down the header).
    std::string tlsRecvAll(std::size_t maxBytes) {
        std::string raw;
        char buf[4096];
        long long got;
        while ((got = tlsRecv(buf, sizeof(buf))) > 0) {
            raw.append(buf, static_cast<std::size_t>(got));
            if (raw.size() > maxBytes) throw KiritoError("recvall exceeds the size limit");
        }
        if (got < 0) throw KiritoError("SSL_read failed");
        return raw;
    }
#endif
    // The fd for a syscall, or a clean error if the socket was closed/detached — so an operation on a
    // dead socket throws "recv: socket is closed" instead of a raw EBADF or touching a stale handle.
    netcompat::socket_t fdOrThrow(const char* op) const {
        if (closed || !netcompat::isValid(fd))
            throw KiritoError(std::string(op) + ": socket is closed");
        return fd;
    }

    static int64_t asInt(KiritoVM& vm, Handle h) { return Value(vm, h).asInt("argument"); }
    static const std::string& asStr(KiritoVM& vm, Handle h) { return Value(vm, h).asStringRef("argument"); }

    Handle getAttr(KiritoVM& vm, Handle self, std::string_view name) override;
};

namespace net {

// Resolve host:port for a given family/type into a sockaddr_storage (the first usable address).
// `passive` (for bind) sets AI_PASSIVE so an empty host means "all interfaces" (INADDR_ANY /
// in6addr_any). On failure returns false and fills `err` with the getaddrinfo reason.
inline bool resolveAddr(const std::string& host, int port, int family, int socktype,
                        sockaddr_storage& out, socklen_t& outLen, std::string& err, bool passive = false) {
    netcompat::startup();
    addrinfo hints{};
    hints.ai_family = family;
    hints.ai_socktype = socktype;
    if (passive) hints.ai_flags = AI_PASSIVE;
    addrinfo* res = nullptr;
    std::string portStr = std::to_string(port);
    const char* node = (passive && host.empty()) ? nullptr : host.c_str();
    int rc = ::getaddrinfo(node, portStr.c_str(), &hints, &res);
    if (rc != 0 || !res) { err = ::gai_strerror(rc); return false; }
    std::memcpy(&out, res->ai_addr, res->ai_addrlen);
    outLen = static_cast<socklen_t>(res->ai_addrlen);
    ::freeaddrinfo(res);
    return true;
}

// Format a resolved sockaddr back into a numeric (host, port) pair (IPv4 or IPv6), via getnameinfo.
inline void formatAddr(const sockaddr* sa, socklen_t len, std::string& host, int& port) {
    char hbuf[NI_MAXHOST], pbuf[NI_MAXSERV];
    if (::getnameinfo(sa, len, hbuf, sizeof(hbuf), pbuf, sizeof(pbuf),
                      NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
        host = hbuf;
        port = std::atoi(pbuf);
    } else {
        host.clear();
        port = 0;
    }
}

// String <-> constant mappings for the address family / socket type (Kirito exposes these as short
// lowercase strings rather than raw AF_*/SOCK_* integers).
inline int parseFamily(const std::string& s) {
    if (s == "inet" || s == "inet4" || s == "ipv4") return AF_INET;
    if (s == "inet6" || s == "ipv6") return AF_INET6;
    throw KiritoError("unknown address family '" + s + "' (expected 'inet' or 'inet6')");
}
inline int parseType(const std::string& s) {
    if (s == "stream" || s == "tcp") return SOCK_STREAM;
    if (s == "dgram" || s == "udp") return SOCK_DGRAM;
    throw KiritoError("unknown socket type '" + s + "' (expected 'stream' or 'dgram')");
}
inline const char* familyName(int f) { return f == AF_INET6 ? "inet6" : "inet"; }
inline const char* typeName(int t) { return t == SOCK_DGRAM ? "dgram" : "stream"; }

// Socket options addressed by a short lowercase string (so no raw SO_*/IPPROTO_* integers leak into
// Kirito). settable+gettable int options; a few are getsockopt-only (error/type/acceptconn).
struct SockOpt { int level; int opt; };
inline bool lookupSockOpt(const std::string& name, SockOpt& out) {
    if (name == "reuseaddr") { out = {SOL_SOCKET, SO_REUSEADDR}; return true; }
    if (name == "broadcast") { out = {SOL_SOCKET, SO_BROADCAST}; return true; }
    if (name == "keepalive") { out = {SOL_SOCKET, SO_KEEPALIVE}; return true; }
    if (name == "rcvbuf")    { out = {SOL_SOCKET, SO_RCVBUF};    return true; }
    if (name == "sndbuf")    { out = {SOL_SOCKET, SO_SNDBUF};    return true; }
    if (name == "nodelay")   { out = {IPPROTO_TCP, TCP_NODELAY}; return true; }
#ifdef SO_REUSEPORT
    if (name == "reuseport") { out = {SOL_SOCKET, SO_REUSEPORT}; return true; }
#endif
    return false;
}
inline bool lookupGetSockOpt(const std::string& name, SockOpt& out) {
    if (lookupSockOpt(name, out)) return true;
    if (name == "error")      { out = {SOL_SOCKET, SO_ERROR};      return true; }
    if (name == "type")       { out = {SOL_SOCKET, SO_TYPE};       return true; }
    if (name == "acceptconn") { out = {SOL_SOCKET, SO_ACCEPTCONN}; return true; }
    return false;
}
inline const char* sockOptNames() {
    return "reuseaddr, broadcast, keepalive, rcvbuf, sndbuf, nodelay"
#ifdef SO_REUSEPORT
           ", reuseport"
#endif
        ;
}
inline std::string getSockOptNames() {
    // Built with += rather than `std::string(...) + const char*`: the move-operator+ overload trips a
    // GCC 13 -O2/-O3 -Warray-bounds false positive (char_traits::copy bounds on the SSO buffer).
    std::string names = sockOptNames();
    names += ", error, type, acceptconn";
    return names;
}

// Extract the raw bytes to transmit from a String or Bytes argument (send/sendto accept either).
inline std::string payloadBytes(KiritoVM& vm, Handle h, const char* who) {
    Object& o = vm.arena().deref(h);
    if (o.kind() == ValueKind::String) return static_cast<StrVal&>(o).value();
    if (auto* b = dynamic_cast<BytesVal*>(&o)) return b->data;
    throw KiritoError(std::string(who) + " expects a String or Bytes");
}

inline void sendAll(netcompat::socket_t fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        long long n = netcompat::sendBytes(fd, data.data() + sent, data.size() - sent);
        if (n <= 0) throw KiritoError("send failed: " + netcompat::lastError());
        sent += static_cast<std::size_t>(n);
    }
}

// Ceiling so a hostile/garrulous peer that never closes the connection throws instead of streaming
// until the process OOMs (consistent with the 256 MiB ceiling the other resource guards use).
// Namespace-scoped so both recvAll (plain TCP) and the HTTPS SSL_read loop (net::kMaxRecvAll, only
// compiled under KIRITO_ENABLE_TLS) share the one bound.
inline constexpr std::size_t kMaxRecvAll = 256ull * 1024 * 1024;

inline std::string recvAll(netcompat::socket_t fd) {
    std::string out;
    char buf[4096];
    while (true) {
        long long n = netcompat::recvBytes(fd, buf, sizeof(buf));
        if (n < 0) throw KiritoError("recv failed: " + netcompat::lastError());
        if (n == 0) break;
        out.append(buf, static_cast<std::size_t>(n));
        if (out.size() > kMaxRecvAll) throw KiritoError("recvall: received data exceeds the size limit");
    }
    return out;
}

// Apply a send/recv timeout (seconds) so a stalled peer throws instead of hanging forever.
inline void setTimeout(netcompat::socket_t fd, double seconds) {
    if (seconds <= 0) return;
#ifdef _WIN32
    DWORD ms = static_cast<DWORD>(seconds * 1000.0);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
    struct timeval tv;
    tv.tv_sec = static_cast<long>(seconds);
    tv.tv_usec = static_cast<long>((seconds - static_cast<double>(tv.tv_sec)) * 1e6);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

// Parse an http://host[:port]/path URL.
struct Url {
    std::string host, path;
    int port = 80;
    bool tls = false;
};
inline Url parseUrl(const std::string& url) {
    Url u;
    // A URL must carry no raw control characters: a CR/LF in the host or path would inject into the
    // request line / Host header (header injection / request smuggling). This matters most for a
    // redirect `Location` (server-controlled, and re-parsed here on each hop). They must be
    // percent-encoded, so reject them outright.
    for (unsigned char ch : url)
        if (ch < 0x20 || ch == 0x7F) throw KiritoError("URL contains a control character");
    std::string rest;
    if (url.compare(0, 7, "http://") == 0) {
        rest = url.substr(7);
        u.port = 80;
    } else if (url.compare(0, 8, "https://") == 0) {
        rest = url.substr(8);
        u.port = 443;
        u.tls = true;
    } else {
        throw KiritoError("URL must start with http:// or https://");
    }
    std::size_t slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    u.path = slash == std::string::npos ? "/" : rest.substr(slash);
    std::string portStr;
    // An IPv6 literal is bracketed (http://[::1]:8080/path); the optional :port follows the ']'. A
    // bare host:port splits on the single colon. (Without the bracket case, the first ':' of an IPv6
    // address would be mistaken for the port separator.)
    if (!hostport.empty() && hostport.front() == '[') {
        std::size_t rb = hostport.find(']');
        if (rb == std::string::npos) throw KiritoError("malformed IPv6 URL (missing ']'): " + url);
        u.host = hostport.substr(1, rb - 1);
        if (rb + 1 < hostport.size()) {
            if (hostport[rb + 1] != ':') throw KiritoError("malformed IPv6 URL (junk after ']'): " + url);
            portStr = hostport.substr(rb + 2);
        }
    } else {
        std::size_t colon = hostport.find(':');
        u.host = hostport.substr(0, colon);
        if (colon != std::string::npos) portStr = hostport.substr(colon + 1);
    }
    if (!portStr.empty()) {
        // Validate explicitly so a non-numeric/out-of-range port gives a clear error rather than a raw
        // std::stoi exception ("stoi") or silent truncation.
        if (portStr.find_first_not_of("0123456789") != std::string::npos)
            throw KiritoError("invalid port in URL '" + url + "': '" + portStr + "'");
        long pn = 0;
        for (char c : portStr) { pn = pn * 10 + (c - '0'); if (pn > 65535) break; }
        if (pn < 1 || pn > 65535) throw KiritoError("port out of range in URL '" + url + "': " + portStr);
        u.port = static_cast<int>(pn);
    }
    return u;
}
// The value for the HTTP `Host:` header: the host re-bracketed if it is an IPv6 literal, plus `:port`
// whenever the port is not the scheme default (80 http / 443 https) — strict vhost routers need both.
inline std::string hostHeader(const Url& u) {
    std::string h = u.host.find(':') != std::string::npos ? "[" + u.host + "]" : u.host;
    if (u.port != (u.tls ? 443 : 80)) h += ":" + std::to_string(u.port);
    return h;
}

// --- URL helpers (urllib.parse style) --------------------------------------------------------
// Percent-encode all but the RFC 3986 "unreserved" set (the quote default).
inline std::string percentEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}
// Decode %XX escapes (and '+' as space, matching query-string decoding). Malformed escapes pass
// through literally.
inline std::string percentDecode(const std::string& s) {
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = val(s[i + 1]), lo = val(s[i + 2]);
            if (hi >= 0 && lo >= 0) { out += static_cast<char>(hi * 16 + lo); i += 2; continue; }
            out += s[i];
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

// A general URL split (any scheme), unlike parseUrl which is HTTP-specific.
struct UrlParts { std::string scheme, host, port, path, query, fragment; };
inline UrlParts splitUrl(const std::string& url) {
    UrlParts p;
    std::string rest = url;
    std::size_t sc = rest.find("://");
    if (sc != std::string::npos) { p.scheme = rest.substr(0, sc); rest = rest.substr(sc + 3); }
    std::size_t hash = rest.find('#');
    if (hash != std::string::npos) { p.fragment = rest.substr(hash + 1); rest = rest.substr(0, hash); }
    std::size_t q = rest.find('?');
    if (q != std::string::npos) { p.query = rest.substr(q + 1); rest = rest.substr(0, q); }
    std::size_t slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    p.path = slash == std::string::npos ? "" : rest.substr(slash);
    // Strip any "user:pass@" userinfo so it doesn't mangle host/port (urlsplit excludes
    // userinfo from hostname/port). The authority host is everything after the LAST '@'.
    std::size_t at = hostport.rfind('@');
    if (at != std::string::npos) hostport = hostport.substr(at + 1);
    // IPv6 literals are bracketed: `[::1]:8080`. Keep the brackets in `host`; the port (if any)
    // is the `:NNNN` after `]`. Without this the plain `find(':')` would split the address itself.
    if (!hostport.empty() && hostport[0] == '[') {
        std::size_t rb = hostport.find(']');
        if (rb != std::string::npos) {
            p.host = hostport.substr(0, rb + 1);
            if (rb + 1 < hostport.size() && hostport[rb + 1] == ':')
                p.port = hostport.substr(rb + 2);
            return p;
        }
    }
    std::size_t colon = hostport.find(':');
    if (colon == std::string::npos) { p.host = hostport; }
    else { p.host = hostport.substr(0, colon); p.port = hostport.substr(colon + 1); }
    return p;
}

// Resolve a possibly-relative Location against the request URL (absolute, root-relative, or relative).
inline std::string resolveUrl(const std::string& base, const std::string& loc) {
    if (loc.compare(0, 7, "http://") == 0 || loc.compare(0, 8, "https://") == 0) return loc;
    UrlParts b = splitUrl(base);
    std::string origin = b.scheme + "://" + b.host + (b.port.empty() ? "" : ":" + b.port);
    if (!loc.empty() && loc[0] == '/') return origin + loc;
    // relative to the base path's directory
    std::string dir = b.path;
    std::size_t slash = dir.find_last_of('/');
    dir = slash == std::string::npos ? "/" : dir.substr(0, slash + 1);
    return origin + dir + loc;
}

inline std::string asciiLower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// Which credentials a redirect from `from` to `to` must NOT carry (NET-1). Mirrors requests'
// should_strip_auth: the Authorization header is dropped when the hostname changes, or when the
// port/scheme changes — EXCEPT a same-host http->https upgrade on the standard ports, which keeps it.
// The cookie jar is dropped only when the HOSTNAME changes (cookies are host-scoped, not port- or
// scheme-scoped, per RFC 6265). Pure + side-effect-free so the policy is unit-testable without a
// live server. (Ports here are already the scheme default 80/443 when the URL omitted one.)
struct RedirectScope { bool dropAuth; bool dropCookies; };
inline RedirectScope redirectScope(const Url& from, const Url& to) {
    bool hostnameChanged = asciiLower(to.host) != asciiLower(from.host);
    bool stdUpgrade = !from.tls && from.port == 80 && to.tls && to.port == 443;  // http->https, std ports
    bool stripAuth = hostnameChanged ||
                     (!stdUpgrade && (from.port != to.port || from.tls != to.tls));
    return {stripAuth, hostnameChanged};
}

// Decode a chunked transfer-encoded body.
inline std::string dechunk(const std::string& body) {
    std::string out;
    std::size_t i = 0;
    while (i < body.size()) {
        std::size_t eol = body.find("\r\n", i);
        if (eol == std::string::npos) break;
        std::string sizeHex = body.substr(i, eol - i);
        std::size_t semi = sizeHex.find(';');
        if (semi != std::string::npos) sizeHex = sizeHex.substr(0, semi);
        long sz = std::strtol(sizeHex.c_str(), nullptr, 16);
        i = eol + 2;
        if (sz <= 0) break;
        if (i + static_cast<std::size_t>(sz) > body.size()) { out.append(body, i, std::string::npos); break; }
        out.append(body, i, static_cast<std::size_t>(sz));
        i += static_cast<std::size_t>(sz) + 2;  // data + trailing CRLF
    }
    return out;
}

// Strip the gzip wrapper and inflate the DEFLATE payload.
inline std::string gunzip(const std::string& s) {
    if (s.size() < 18 || static_cast<unsigned char>(s[0]) != 0x1f ||
        static_cast<unsigned char>(s[1]) != 0x8b)
        throw KiritoError("invalid gzip data");
    unsigned char flg = static_cast<unsigned char>(s[3]);
    std::size_t i = 10;
    if (flg & 4) {  // FEXTRA
        std::size_t xlen = static_cast<unsigned char>(s[i]) |
                           (static_cast<unsigned char>(s[i + 1]) << 8);
        i += 2 + xlen;
    }
    if (flg & 8) { while (i < s.size() && s[i] != 0) ++i; ++i; }   // FNAME
    if (flg & 16) { while (i < s.size() && s[i] != 0) ++i; ++i; }  // FCOMMENT
    if (flg & 2) i += 2;                                           // FHCRC
    if (i + 8 > s.size()) throw KiritoError("truncated gzip data");
    return deflate::inflate(s.substr(i, s.size() - i - 8));
}

inline std::string decodeBody(const std::string& body, const std::string& enc) {
    if (enc == "gzip" || enc == "x-gzip") return gunzip(body);
    if (enc == "deflate") {
        try { return deflate::inflate(body); }
        catch (...) { return deflate::inflate(body.size() >= 2 ? body.substr(2) : body); }
    }
    return body;
}

// A parsed HTTP response.
struct HttpResult {
    int status = 0;
    std::string reason;
    std::vector<std::pair<std::string, std::string>> headers;  // original case, in order
    std::string body;
    std::string finalUrl;

    std::string header(const std::string& name) const {  // case-insensitive lookup
        std::string lc = asciiLower(name);
        for (const auto& [k, v] : headers) if (asciiLower(k) == lc) return v;
        return "";
    }
};

// Parse a raw HTTP/1.1 response: status line, headers, and the entity body (de-chunked and
// decompressed as the response's own headers direct).
inline HttpResult parseRaw(const std::string& raw) {
    HttpResult r;
    std::size_t headerEnd = raw.find("\r\n\r\n");
    std::string head = headerEnd == std::string::npos ? raw : raw.substr(0, headerEnd);
    std::string body = headerEnd == std::string::npos ? "" : raw.substr(headerEnd + 4);
    std::size_t pos = 0;
    bool first = true;
    while (pos < head.size()) {
        std::size_t eol = head.find("\r\n", pos);
        std::string line = head.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        if (first) {
            std::size_t sp = line.find(' ');
            if (sp != std::string::npos) {
                r.status = std::atoi(line.c_str() + sp + 1);
                std::size_t sp2 = line.find(' ', sp + 1);
                if (sp2 != std::string::npos) r.reason = line.substr(sp2 + 1);
            }
            first = false;
        } else if (!line.empty()) {
            std::size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string key = line.substr(0, colon);
                std::size_t vs = line.find_first_not_of(' ', colon + 1);
                r.headers.emplace_back(key, vs == std::string::npos ? "" : line.substr(vs));
            }
        }
        if (eol == std::string::npos) break;
        pos = eol + 2;
    }
    if (asciiLower(r.header("transfer-encoding")).find("chunked") != std::string::npos)
        body = dechunk(body);
    std::string enc = asciiLower(r.header("content-encoding"));
    if (!enc.empty()) {
        try { body = decodeBody(body, enc); } catch (...) { /* leave body as-is on decode failure */ }
    }
    r.body = std::move(body);
    return r;
}

// Open a connected TCP socket to the URL's host:port, or throw. Family-agnostic (AF_UNSPEC): tries
// each address getaddrinfo returns, so an IPv6 host — http://[::1]:8080/ — connects as readily as IPv4.
inline netcompat::socket_t dialTcp(const Url& u, double timeout = 0.0) {
    netcompat::startup();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;          // IPv4 or IPv6, whichever the host has
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    std::string portStr = std::to_string(u.port);
    if (::getaddrinfo(u.host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res)
        throw KiritoError("could not resolve host '" + u.host + "'");
    netcompat::socket_t fd = netcompat::kInvalidSocket;
    std::string lastErr;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (!netcompat::isValid(fd)) { lastErr = netcompat::lastError(); continue; }
        // `timeout` bounds the connect too (not just send/recv): a black-hole host would otherwise
        // hang for the OS default regardless of the request timeout.
        if (netcompat::connectWithTimeout(fd, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen), timeout) == 0)
            break;  // connected
        lastErr = netcompat::lastError();
        netcompat::closeSocket(fd);
        fd = netcompat::kInvalidSocket;
    }
    ::freeaddrinfo(res);
    if (!netcompat::isValid(fd))
        throw KiritoError("could not connect to " + u.host + ": " + (lastErr.empty() ? "no usable address" : lastErr));
    return fd;
}

// Send request bytes and read the full response. Plain TCP by default; TLS (with optional peer
// verification) when built with KIRITO_ENABLE_TLS. `timeout` (seconds, 0 = none) bounds send/recv.
std::string httpExchange(const Url& u, const std::string& request, double timeout, bool verify);

#ifdef KIRITO_ENABLE_TLS
#ifdef _WIN32
// Populate an SSL_CTX's trust store from the Windows system "ROOT" certificate store (the same trust
// curl/browsers use). OpenSSL on Windows ships no default CA bundle, so without this every HTTPS
// verify would fail the handshake with "unable to get local issuer certificate".
inline void addWindowsRootCerts(SSL_CTX* ctx) {
    HCERTSTORE sys = CertOpenSystemStoreA(0, "ROOT");
    if (!sys) return;
    X509_STORE* store = SSL_CTX_get_cert_store(ctx);
    PCCERT_CONTEXT cert = nullptr;
    while ((cert = CertEnumCertificatesInStore(sys, cert)) != nullptr) {
        const unsigned char* der = cert->pbCertEncoded;
        X509* x = d2i_X509(nullptr, &der, static_cast<long>(cert->cbCertEncoded));
        if (x) { X509_STORE_add_cert(store, x); X509_free(x); }
    }
    CertCloseStore(sys, 0);
}
#endif

// Perform a CLIENT TLS handshake over an already-connected fd. `host` drives SNI and (when `verify`)
// hostname verification; pass an empty host to skip both (an IP-only peer with verify=False). On
// success returns the new SSL + its SSL_CTX via out-params (the caller owns and must free both); on
// failure it frees whatever it allocated and throws — it never closes `fd` (the caller owns that).
// Shared by the HTTP/HTTPS client and by Socket.starttls so the two can't drift.
inline void tlsClientHandshake(netcompat::socket_t fd, const std::string& host, bool verify,
                               SSL*& sslOut, SSL_CTX*& ctxOut) {
    static bool inited = [] { SSL_library_init(); SSL_load_error_strings(); return true; }();
    (void)inited;
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) throw KiritoError("SSL_CTX_new failed");
    if (verify) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        SSL_CTX_set_default_verify_paths(ctx);   // Unix CA dirs + the SSL_CERT_FILE / SSL_CERT_DIR env vars
#ifdef _WIN32
        addWindowsRootCerts(ctx);                // and the Windows system trust store
#endif
    }
    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, static_cast<int>(fd));
    if (!host.empty()) SSL_set_tlsext_host_name(ssl, host.c_str());  // SNI
    if (verify && !host.empty()) {
        SSL_set_hostflags(ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        SSL_set1_host(ssl, host.c_str());
    }
    if (SSL_connect(ssl) != 1) {
        long vr = SSL_get_verify_result(ssl);
        unsigned long e = ERR_peek_last_error();
        std::string why;
        if (vr != X509_V_OK)
            why = std::string(": ") + X509_verify_cert_error_string(vr) +
                  " (no trusted CA found; set the SSL_CERT_FILE env var to a CA bundle, or pass verify=False)";
        else if (e) { char b[256]; ERR_error_string_n(e, b, sizeof(b)); why = std::string(": ") + b; }
        SSL_free(ssl); SSL_CTX_free(ctx);
        throw KiritoError("TLS handshake" + (host.empty() ? std::string() : " with " + host) + " failed" + why);
    }
    if (verify && SSL_get_verify_result(ssl) != X509_V_OK) {
        SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx);
        throw KiritoError("TLS certificate verification failed" +
                          (host.empty() ? std::string() : " for " + host) + " (pass verify=False to skip)");
    }
    sslOut = ssl; ctxOut = ctx;
}

inline std::string httpExchange(const Url& u, const std::string& request, double timeout, bool verify) {
    netcompat::socket_t fd = dialTcp(u, timeout);
    setTimeout(fd, timeout);
    if (!u.tls) {
        std::string raw;
        try { net::sendAll(fd, request); raw = net::recvAll(fd); }
        catch (...) { netcompat::closeSocket(fd); throw; }
        netcompat::closeSocket(fd);
        return raw;
    }
    SSL* ssl = nullptr; SSL_CTX* ctx = nullptr;
    try { tlsClientHandshake(fd, u.host, verify, ssl, ctx); }
    catch (...) { netcompat::closeSocket(fd); throw; }
    std::string raw;
    try {
        std::size_t sent = 0;
        while (sent < request.size()) {
            int n = SSL_write(ssl, request.data() + sent, static_cast<int>(request.size() - sent));
            if (n <= 0) throw KiritoError("SSL_write failed");
            sent += static_cast<std::size_t>(n);
        }
        char buf[4096];
        int n;
        while ((n = SSL_read(ssl, buf, sizeof(buf))) > 0) {
            raw.append(buf, static_cast<std::size_t>(n));
            // Bound the response like the plain-TCP recvAll path, so a chatty HTTPS server can't OOM us.
            if (raw.size() > net::kMaxRecvAll) throw KiritoError("HTTPS response exceeds the size limit");
        }
    } catch (...) {
        SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); netcompat::closeSocket(fd);
        throw;
    }
    SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); netcompat::closeSocket(fd);
    return raw;
}
#else
inline std::string httpExchange(const Url& u, const std::string& request, double timeout, bool verify) {
    (void)verify;
    if (u.tls)
        throw KiritoError("https requires building with KIRITO_ENABLE_TLS (OpenSSL); use http:// otherwise");
    netcompat::socket_t fd = dialTcp(u, timeout);
    setTimeout(fd, timeout);
    std::string raw;
    try { net::sendAll(fd, request); raw = net::recvAll(fd); }
    catch (...) { netcompat::closeSocket(fd); throw; }
    netcompat::closeSocket(fd);
    return raw;
}
#endif

}  // namespace net

// A rich HTTP response value (requests-style). Exposes status/reason/ok/url/text/headers/cookies
// attributes, a json()/raiseforstatus()/header() method set, and Dict-style ["status"]/["body"]
// indexing for convenience.
class ResponseVal : public NativeClass<ResponseVal> {
public:
    static constexpr const char* kTypeName = "Response";
    std::vector<std::string> inspectMembers() const override {
        return {"status: Integer", "statuscode: Integer", "reason: String", "ok: Bool", "url: String", "text: String", "body: String", "content: Bytes", "headers: Dict", "cookies: Dict", "json() -> Any", "header(name, default) -> String", "raiseforstatus()"};
    }
    int status = 0;
    std::string reason, url, body;
    Handle headersH{}, cookiesH{};  // Dicts (headers in original case; cookies name->value)

    void children(std::vector<Handle>& out) const override { out.push_back(headersH); out.push_back(cookiesH); }
    std::string str(StringifyCtx&) const override { return "<Response [" + std::to_string(status) + "]>"; }
    Handle getAttr(KiritoVM& vm, Handle self, std::string_view name) override;
    // Dict-style indexing for convenience: r["status"]/r["body"]/... map to the same fields as the
    // attributes, so a Response can be consumed like a Dict.
    Handle getItem(KiritoVM& vm, std::span<const Handle> keys) override {
        if (keys.size() != 1) throw KiritoError("Response indexing takes a single string key");
        const Object& k = vm.arena().deref(keys[0]);
        if (k.kind() != ValueKind::String) throw KiritoError("Response index must be a String key");
        const std::string& name = static_cast<const StrVal&>(k).value();
        if (name == "status" || name == "statuscode") return vm.makeInt(status);
        if (name == "reason") return vm.makeString(reason);
        if (name == "ok") return vm.makeBool(status >= 100 && status < 400);
        if (name == "url") return vm.makeString(url);
        if (name == "text" || name == "body") return vm.makeString(body);
        if (name == "content") return vm.alloc(std::make_unique<BytesVal>(body));
        if (name == "headers") return headersH;
        if (name == "cookies") return cookiesH;
        throw KiritoError("Response has no field '" + name + "'");
    }
};

// A persistent HTTP session: keeps a cookie jar and default headers across requests (requests.Session
// style). `headers` and `cookies` are mutable Dicts; the verb methods merge them into each request
// and fold any Set-Cookie back into the jar.
class SessionVal : public NativeClass<SessionVal> {
public:
    static constexpr const char* kTypeName = "Session";
    std::vector<std::string> inspectMembers() const override {
        return {"headers: Dict", "cookies: Dict", "get(url, opts) -> Response", "post(url, opts) -> Response", "put(url, opts) -> Response", "delete(url, opts) -> Response", "patch(url, opts) -> Response", "head(url, opts) -> Response", "options(url, opts) -> Response", "request(method, url, opts) -> Response"};
    }
    Handle headersH{}, cookiesH{};
    void children(std::vector<Handle>& out) const override { out.push_back(headersH); out.push_back(cookiesH); }
    std::string str(StringifyCtx&) const override { return "<Session>"; }
    Handle getAttr(KiritoVM& vm, Handle self, std::string_view name) override;
};

namespace net {

// Build a Response value from a parsed result + accumulated cookie jar.
inline Handle makeResponse(KiritoVM& vm, const HttpResult& r, Handle cookiesH) {
    RootScope rs(vm);
    Dict hd(vm);
    for (const auto& [k, v] : r.headers) hd.set(k, Value(vm, v));
    auto resp = std::make_unique<ResponseVal>();
    resp->status = r.status;
    resp->reason = r.reason;
    resp->url = r.finalUrl;
    resp->body = r.body;
    resp->headersH = rs.add(hd.handle());
    resp->cookiesH = cookiesH;
    return vm.alloc(std::move(resp));
}

}  // namespace net

// Read an optional key from an options Dict; returns none() if absent / not a Dict.
inline Handle netOpt(KiritoVM& vm, Handle opts, const char* key) {
    const Object& o = vm.arena().deref(opts);
    if (o.kind() != ValueKind::Dict) return vm.none();
    Handle k = vm.makeString(key);
    const Handle* p = static_cast<const DictVal&>(o).find(vm.arena(), k);
    return p ? *p : vm.none();
}

// Encode a Dict of params as application/x-www-form-urlencoded (`k=v&...`, percent-encoded). The one
// implementation shared by the request body (`data=`), the query string (`params=`), and `urlencode`.
inline std::string formUrlencode(KiritoVM& vm, Handle dict) {
    std::string out;
    for (const auto& [k, v] : Value(vm, dict).pairs()) {
        if (!out.empty()) out += "&";
        out += net::percentEncode(k.str()) + "=" + net::percentEncode(v.str());
    }
    return out;
}

// The core HTTP driver: build the request from `opts`, follow redirects, accumulate cookies, and
// return a Response. `opts` is a Dict (or none) with keys: headers, params, data, json, auth,
// timeout, allowredirects, maxredirects, verify, cookies.
inline Handle netRequest(KiritoVM& vm, const std::string& method0, const std::string& url0, Handle opts) {
    RootScope rs(vm);
    auto isStr = [&](Handle h) { return vm.arena().deref(h).kind() == ValueKind::String; };
    auto asStr = [&](Handle h) { return static_cast<const StrVal&>(vm.arena().deref(h)).value(); };

    // body + content type from json= / data=
    std::string body, contentType;
    Handle jsonH = netOpt(vm, opts, "json");
    Handle dataH = netOpt(vm, opts, "data");
    if (vm.arena().deref(jsonH).kind() != ValueKind::None) {
        fum::unordered_set<const Object*> active;
        json::write(vm, jsonH, body, active);
        contentType = "application/json";
    } else if (vm.arena().deref(dataH).kind() != ValueKind::None) {
        const Object& d = vm.arena().deref(dataH);
        if (d.kind() == ValueKind::Dict) {
            body = formUrlencode(vm, dataH);
            contentType = "application/x-www-form-urlencoded";
        } else if (const auto* db = dynamic_cast<const BytesVal*>(&d)) {
            body = db->data;  // raw bytes, not the b'...' repr (Bytes is a NativeClass, kind Instance)
            contentType = "application/octet-stream";
        } else {
            body = Value(vm, dataH).str();
            contentType = "text/plain";
        }
    }

    // multipart/form-data file upload (files = {field: content} or {field: [filename, content]})
    Handle filesH = netOpt(vm, opts, "files");
    if (vm.arena().deref(filesH).kind() == ValueKind::Dict) {
        const std::string boundary = "----KiritoFormBoundary7MA4YWxkTrZu0gW";
        // Bytes content must go in raw (its .data), NOT its b'...' repr — otherwise a binary upload
        // (image/gzip/dump blob) is silently mangled. Mirrors the data= branch above; only the
        // filename is stringified.
        auto rawContent = [&vm](Handle h) -> std::string {
            const Object& o = vm.arena().deref(h);
            if (const auto* b = dynamic_cast<const BytesVal*>(&o)) return b->data;
            return Value(vm, h).str();
        };
        std::string mp;
        for (const auto& [k, v] : Value(vm, filesH).pairs()) {
            std::string field = k.str(), filename = k.str(), content;
            const Object& vo = vm.arena().deref(v.handle());
            if (vo.kind() == ValueKind::List) {
                const ListVal& l = static_cast<const ListVal&>(vo);
                if (l.elems.size() >= 2) { filename = vm.stringify(l.elems[0]); content = rawContent(l.elems[1]); }
            } else {
                content = rawContent(v.handle());
            }
            mp += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + field +
                  "\"; filename=\"" + filename + "\"\r\nContent-Type: application/octet-stream\r\n\r\n" +
                  content + "\r\n";
        }
        mp += "--" + boundary + "--\r\n";
        body = mp;
        contentType = "multipart/form-data; boundary=" + boundary;
    }

    // query params appended to the URL
    std::string url = url0;
    Handle paramsH = netOpt(vm, opts, "params");
    if (vm.arena().deref(paramsH).kind() == ValueKind::Dict) {
        std::string qs = formUrlencode(vm, paramsH);
        if (!qs.empty()) url += (url.find('?') == std::string::npos ? "?" : "&") + qs;
    }

    // assembled (case-insensitively keyed) request headers, with defaults
    std::vector<std::pair<std::string, std::string>> hdrs;
    auto setHdr = [&](const std::string& k, const std::string& v) {
        // Reject CR/LF in a header key or value: user-supplied headers/cookies flow here and are written
        // verbatim into the request, so a CRLF would inject extra headers or smuggle a second request
        // (A18-1). Rejection (not encoding) is correct for header fields, matching http.client/requests.
        if (k.find_first_of("\r\n") != std::string::npos || v.find_first_of("\r\n") != std::string::npos)
            throw KiritoError("header contains a control character (CR/LF): '" + k + "'");
        std::string lc = net::asciiLower(k);
        for (auto& h : hdrs) if (net::asciiLower(h.first) == lc) { h.second = v; return; }
        hdrs.emplace_back(k, v);
    };
    setHdr("User-Agent", "kirito/1.0");
    setHdr("Accept", "*/*");
    if (!contentType.empty()) setHdr("Content-Type", contentType);
    Handle authH = netOpt(vm, opts, "auth");
    if (vm.arena().deref(authH).kind() == ValueKind::List) {
        const ListVal& l = static_cast<const ListVal&>(vm.arena().deref(authH));
        if (l.elems.size() == 2 && isStr(l.elems[0]) && isStr(l.elems[1]))
            setHdr("Authorization", "Basic " + base64Encode(asStr(l.elems[0]) + ":" + asStr(l.elems[1])));
    }
    Handle headersH = netOpt(vm, opts, "headers");
    if (vm.arena().deref(headersH).kind() == ValueKind::Dict)
        for (const auto& [k, v] : Value(vm, headersH).pairs()) setHdr(k.str(), v.str());

    // cookie jar (seeded from cookies= option), persisted across redirects
    Dict jar(vm);
    Handle cookiesOpt = netOpt(vm, opts, "cookies");
    if (vm.arena().deref(cookiesOpt).kind() == ValueKind::Dict)
        for (const auto& [k, v] : Value(vm, cookiesOpt).pairs()) {
            std::string ck = k.str(), cv = v.str();  // reject CRLF: cookies fold into a Cookie header (A18-1)
            if (ck.find_first_of("\r\n") != std::string::npos || cv.find_first_of("\r\n") != std::string::npos)
                throw KiritoError("cookie contains a control character (CR/LF): '" + ck + "'");
            jar.set(ck, Value(vm, cv));
        }
    Handle jarH = rs.add(jar.handle());
    auto& jarDict = static_cast<DictVal&>(vm.arena().deref(jarH));

    double timeout = 0.0;
    Handle toH = netOpt(vm, opts, "timeout");
    if (vm.arena().deref(toH).kind() == ValueKind::Float) timeout = static_cast<const FloatVal&>(vm.arena().deref(toH)).value();
    else if (vm.arena().deref(toH).kind() == ValueKind::Integer) timeout = static_cast<double>(static_cast<const IntVal&>(vm.arena().deref(toH)).value());

    bool verify = true;
    Handle verifyH = netOpt(vm, opts, "verify");
    if (vm.arena().deref(verifyH).kind() == ValueKind::Bool) verify = static_cast<const BoolVal&>(vm.arena().deref(verifyH)).value();

    bool allowRedirects = true;
    Handle arH = netOpt(vm, opts, "allowredirects");
    if (vm.arena().deref(arH).kind() == ValueKind::Bool) allowRedirects = static_cast<const BoolVal&>(vm.arena().deref(arH)).value();
    int maxRedirects = 10;
    Handle mrH = netOpt(vm, opts, "maxredirects");
    if (vm.arena().deref(mrH).kind() == ValueKind::Integer) maxRedirects = static_cast<int>(static_cast<const IntVal&>(vm.arena().deref(mrH)).value());

    std::string method = method0, curUrl = url, curBody = body;
    for (int redirect = 0;; ++redirect) {
        net::Url u = net::parseUrl(curUrl);
        std::string target = u.path;
        std::string req = method + " " + target + " HTTP/1.1\r\nHost: " + net::hostHeader(u) + "\r\n";
        for (const auto& [k, v] : hdrs)
            if (net::asciiLower(k) != "host" && net::asciiLower(k) != "content-length") req += k + ": " + v + "\r\n";
        // Cookie header from the jar
        std::string cookieHdr;
        for (Handle k : jarDict.keys()) {
            const std::string& cn = static_cast<const StrVal&>(vm.arena().deref(k)).value();
            const Handle* cv = jarDict.find(vm.arena(), k);
            if (!cookieHdr.empty()) cookieHdr += "; ";
            cookieHdr += cn + "=" + (cv ? vm.stringify(*cv) : "");
        }
        if (!cookieHdr.empty()) req += "Cookie: " + cookieHdr + "\r\n";
        req += "Connection: close\r\n";
        if (!curBody.empty()) req += "Content-Length: " + std::to_string(curBody.size()) + "\r\n";
        req += "\r\n" + curBody;

        std::string raw = net::httpExchange(u, req, timeout, verify);
        net::HttpResult r = net::parseRaw(raw);
        r.finalUrl = curUrl;

        // fold any Set-Cookie headers into the jar
        for (const auto& [k, v] : r.headers) {
            if (net::asciiLower(k) != "set-cookie") continue;
            std::string nv = v.substr(0, v.find(';'));
            std::size_t eq = nv.find('=');
            if (eq != std::string::npos)
                jarDict.set(vm.arena(), rs.add(vm.makeString(nv.substr(0, eq))),
                            rs.add(vm.makeString(nv.substr(eq + 1))));
        }

        bool isRedirect = r.status == 301 || r.status == 302 || r.status == 303 ||
                          r.status == 307 || r.status == 308;
        std::string loc = r.header("location");
        if (allowRedirects && isRedirect && !loc.empty() && redirect < maxRedirects) {
            std::string newUrl = net::resolveUrl(curUrl, loc);
            // A redirect must not leak credentials to a DIFFERENT origin or over a downgraded scheme
            // (NET-1: allowredirects is on by default). The policy lives in one testable place.
            net::RedirectScope scope = net::redirectScope(u, net::parseUrl(newUrl));
            if (scope.dropAuth)
                hdrs.erase(std::remove_if(hdrs.begin(), hdrs.end(),
                                          [](const std::pair<std::string, std::string>& h) {
                                              return net::asciiLower(h.first) == "authorization";
                                          }),
                           hdrs.end());
            if (scope.dropCookies) jar.clear();
            curUrl = newUrl;
            if (r.status == 303 || ((r.status == 301 || r.status == 302) && method == "POST")) {
                method = "GET";
                curBody.clear();
                for (auto& h : hdrs)
                    if (net::asciiLower(h.first) == "content-type") h.second.clear();
            }
            continue;
        }
        return net::makeResponse(vm, r, jarH);
    }
}

// A Session request: merge the session's default headers + cookie jar into `opts`, perform the
// request, then fold the response's cookies back into the jar.
inline Handle sessionDo(KiritoVM& vm, Handle self, const std::string& method, const std::string& url, Handle opts) {
    auto& s = static_cast<SessionVal&>(vm.arena().deref(self));
    RootScope rs(vm);
    auto mergeInto = [&](Dict& dst, Handle src) {
        if (vm.arena().deref(src).kind() == ValueKind::Dict)
            for (const auto& [k, v] : Value(vm, src).pairs()) dst.set(k.str(), v.handle());
    };
    Dict eff(vm);
    mergeInto(eff, opts);                              // start from caller options
    Dict hdr(vm);
    mergeInto(hdr, s.headersH);                        // session defaults...
    mergeInto(hdr, netOpt(vm, opts, "headers"));       // ...overlaid by per-call headers
    eff.set("headers", hdr);
    Dict ck(vm);
    mergeInto(ck, s.cookiesH);
    mergeInto(ck, netOpt(vm, opts, "cookies"));
    eff.set("cookies", ck);
    Handle effH = rs.add(eff.handle());

    Handle resp = netRequest(vm, method, url, effH);
    // persist response cookies into the session jar
    auto& r = static_cast<ResponseVal&>(vm.arena().deref(resp));
    auto& jar = static_cast<DictVal&>(vm.arena().deref(s.cookiesH));
    const DictVal& rc = static_cast<const DictVal&>(vm.arena().deref(r.cookiesH));
    for (Handle k : rc.keys()) jar.set(vm.arena(), k, *rc.find(vm.arena(), k));
    return resp;
}

inline Handle SessionVal::getAttr(KiritoVM& vm, Handle self, std::string_view name) {
    if (name == "headers") return headersH;
    if (name == "cookies") return cookiesH;
    auto verb = [&](const char* nm, const char* method) {
        return makeMethod(vm, nm, {"url", "opts"},
            [self, method](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                Args args(vm, a, "session");
                Handle opts = args.size() > 1 ? a[1] : vm.none();
                return sessionDo(vm, self, method, args[0].asStringRef("url"), opts);
            }, std::vector<Handle>{self});
    };
    if (name == "get") return verb("get", "GET");
    if (name == "post") return verb("post", "POST");
    if (name == "put") return verb("put", "PUT");
    if (name == "delete") return verb("delete", "DELETE");
    if (name == "patch") return verb("patch", "PATCH");
    if (name == "head") return verb("head", "HEAD");
    if (name == "options") return verb("options", "OPTIONS");
    if (name == "request")
        return makeMethod(vm, "request", {"method", "url", "opts"},
            [self](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                Args args(vm, a, "session.request");
                Handle opts = args.size() > 2 ? a[2] : vm.none();
                return sessionDo(vm, self, args[0].asStringRef("method"), args[1].asStringRef("url"), opts);
            }, std::vector<Handle>{self});
    return Object::getAttr(vm, self, name);
}

inline Handle SocketVal::getAttr(KiritoVM& vm, Handle self, std::string_view name) {
    auto bind = [&](const char* nm, std::vector<std::string> params, NativeFn fn) {
        return makeMethod(vm, nm, std::move(params), std::move(fn), std::vector<Handle>{self});
    };
    // bind + a minimum-arity guard: makeMethod's positional path forwards the call args verbatim, so a
    // method that dereferences a[0]/a[1] would read past an empty span on an under-arity call (UB — and
    // socket.send() would transmit whatever that stale slot pointed at). Methods with required leading
    // args use this so a missing arg is a clean error before any deref.
    auto bindReq = [&](std::string nm, std::size_t minArgs, std::vector<std::string> params, NativeFn fn) {
        NativeFn guarded = [nm, minArgs, fn](KiritoVM& v, std::span<const Handle> a) -> Handle {
            if (a.size() < minArgs)
                throw KiritoError(nm + "() expected at least " + std::to_string(minArgs) + " argument(s)");
            return fn(v, a);
        };
        return makeMethod(vm, nm, std::move(params), std::move(guarded), std::vector<Handle>{self});
    };
    auto sock = [](KiritoVM& vm, Handle self) -> SocketVal& {
        return static_cast<SocketVal&>(vm.arena().deref(self));
    };
    // Read-only introspection attributes, like Python's socket.family / socket.type.
    if (name == "family") return vm.makeString(net::familyName(sock(vm, self).family));
    if (name == "type")   return vm.makeString(net::typeName(sock(vm, self).socktype));
    auto checkPort = [](int64_t port) {
        if (port < 0 || port > 65535)
            throw KiritoError("port out of range: " + std::to_string(port) + " (must be 0-65535)");
    };
    if (name == "connect")
        return bindReq("connect", 2, {"host", "port"}, [self, sock, checkPort](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            auto& s = sock(vm, self);
            int64_t port = asInt(vm, a[1]);
            checkPort(port);
            std::string host = asStr(vm, a[0]);
            sockaddr_storage ss{}; socklen_t sl = 0; std::string err;
            if (!net::resolveAddr(host, static_cast<int>(port), s.family, s.socktype, ss, sl, err))
                throw KiritoError("connect: could not resolve host '" + host + "': " + err);
            if (netcompat::connectWithTimeout(s.fdOrThrow("connect"), reinterpret_cast<sockaddr*>(&ss), sl, s.timeout) != 0)
                throw KiritoError("connect failed: " + netcompat::lastError());
            s.peerHost = host;   // remembered as starttls's default SNI / verify name
            return vm.none();
        });
    if (name == "bind")
        return bindReq("bind", 2, {"host", "port"}, [self, sock, checkPort](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            auto& s = sock(vm, self);
            netcompat::setSockOptInt(s.fdOrThrow("bind"), SOL_SOCKET, SO_REUSEADDR, 1);
            int64_t port = asInt(vm, a[1]);
            checkPort(port);
            std::string host = asStr(vm, a[0]);
            // Resolve so bind("localhost", ...) binds the loopback, not every interface; an empty host
            // means "all interfaces" (AI_PASSIVE → INADDR_ANY / in6addr_any for the socket's family).
            // Family-aware: an AF_INET6 socket binds an IPv6 address, an AF_INET socket an IPv4 one.
            sockaddr_storage ss{}; socklen_t sl = 0; std::string err;
            if (!net::resolveAddr(host, static_cast<int>(port), s.family, s.socktype, ss, sl, err, /*passive=*/true))
                throw KiritoError("bind: cannot resolve host '" + host + "': " + err);
            if (::bind(s.fd, reinterpret_cast<sockaddr*>(&ss), sl) != 0)
                throw KiritoError("bind failed: " + netcompat::lastError());
            return vm.none();
        });
    if (name == "listen")
        return bind("listen", {"backlog"}, [self, sock](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            int backlog = a.empty() ? 16 : static_cast<int>(asInt(vm, a[0]));
            if (::listen(sock(vm, self).fdOrThrow("listen"), backlog) != 0)
                throw KiritoError("listen failed: " + netcompat::lastError());
            return vm.none();
        });
    if (name == "accept")
        return bind("accept", {}, [self, sock](KiritoVM& vm, std::span<const Handle>) -> Handle {
            auto& s = sock(vm, self);
            netcompat::socket_t c = ::accept(s.fdOrThrow("accept"), nullptr, nullptr);
            if (!netcompat::isValid(c)) throw KiritoError("accept failed: " + netcompat::lastError());
            return vm.alloc(std::make_unique<SocketVal>(c, s.family, s.socktype));  // inherit family/type
        });
    if (name == "send")
        return bindReq("send", 1, {"data"}, [self, sock](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            std::string data = net::payloadBytes(vm, a[0], "send");   // String or Bytes
            auto& s = sock(vm, self);
            (void)s.fdOrThrow("send");
#ifdef KIRITO_ENABLE_TLS
            if (s.ssl_) { s.tlsSendAll(data); return vm.makeInt(static_cast<int64_t>(data.size())); }
#endif
            net::sendAll(s.fd, data);
            return vm.makeInt(static_cast<int64_t>(data.size()));
        });
    if (name == "recv")
        return bind("recv", {"n"}, [self, sock](KiritoVM& vm, std::span<const Handle> a) -> Handle {  // param `n` matches the docs (recv([n]))
            int64_t reqN = a.empty() ? 4096 : asInt(vm, a[0]);
            if (reqN < 0) throw KiritoError("recv size must be non-negative");
            // recv(0) returns empty Bytes immediately (Python semantics). A raw ::recv(fd, buf, 0)
            // BLOCKS on Linux until data or EOF, which is a surprising hang; short-circuit it. Still
            // validate the socket isn't closed, for a consistent error.
            auto& s = sock(vm, self);
            if (reqN == 0) { (void)s.fdOrThrow("recv"); return vm.alloc(std::make_unique<BytesVal>(std::string())); }
            std::size_t n = static_cast<std::size_t>(std::min<int64_t>(reqN, 64ll * 1024 * 1024));  // cap the buffer; recv returns <= size anyway
            std::vector<char> buf(n);
            (void)s.fdOrThrow("recv");
            long long got;
#ifdef KIRITO_ENABLE_TLS
            if (s.ssl_) got = s.tlsRecv(buf.data(), n);
            else
#endif
            got = netcompat::recvBytes(s.fd, buf.data(), n);
            if (got < 0) throw KiritoError("recv failed: " + netcompat::lastError());
            // Bytes, not String: a socket carries raw bytes (binary video, gzip, ...). Decode for text.
            return vm.alloc(std::make_unique<BytesVal>(std::string(buf.data(), static_cast<std::size_t>(got))));
        });
    if (name == "recvall")
        return bind("recvall", {}, [self, sock](KiritoVM& vm, std::span<const Handle>) -> Handle {
            auto& s = sock(vm, self);
            (void)s.fdOrThrow("recvall");
#ifdef KIRITO_ENABLE_TLS
            if (s.ssl_) return vm.alloc(std::make_unique<BytesVal>(s.tlsRecvAll(net::kMaxRecvAll)));
#endif
            return vm.alloc(std::make_unique<BytesVal>(net::recvAll(s.fd)));
        });
    if (name == "settimeout")
        return bindReq("settimeout", 1, {"seconds"}, [self, sock](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            const Object& t = vm.arena().deref(a[0]);
            double secs = t.kind() == ValueKind::Float
                              ? static_cast<const FloatVal&>(t).value()
                              : static_cast<double>(asInt(vm, a[0]));
            auto& s = sock(vm, self);
            net::setTimeout(s.fdOrThrow("settimeout"), secs);
            s.timeout = secs;   // also bounds a subsequent connect() (Python semantics)
            return vm.none();
        });
    // --- datagram (UDP) I/O ---
    if (name == "sendto")
        return bindReq("sendto", 3, {"data", "host", "port"}, [self, sock, checkPort](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            auto& s = sock(vm, self);
            std::string data = net::payloadBytes(vm, a[0], "sendto");   // String or Bytes
            if (data.size() > netcompat::kIoChunk) throw KiritoError("sendto: datagram too large");
            int64_t port = asInt(vm, a[2]);
            checkPort(port);
            std::string host = asStr(vm, a[1]);
            sockaddr_storage ss{}; socklen_t sl = 0; std::string err;
            if (!net::resolveAddr(host, static_cast<int>(port), s.family, s.socktype, ss, sl, err))
                throw KiritoError("sendto: could not resolve host '" + host + "': " + err);
            long long n = netcompat::sendTo(s.fdOrThrow("sendto"), data.data(), data.size(),
                                            reinterpret_cast<sockaddr*>(&ss), sl);
            if (n < 0) throw KiritoError("sendto failed: " + netcompat::lastError());
            return vm.makeInt(n);
        });
    if (name == "recvfrom")   // recvfrom([n]) -> [Bytes, [host, port]]
        return bind("recvfrom", {"n"}, [self, sock](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            int64_t reqN = a.empty() ? 65536 : asInt(vm, a[0]);
            if (reqN < 0) throw KiritoError("recvfrom size must be non-negative");
            std::size_t n = static_cast<std::size_t>(std::min<int64_t>(reqN, 64ll * 1024 * 1024));
            std::vector<char> buf(n);
            sockaddr_storage ss{}; socklen_t sl = sizeof(ss);
            long long got = netcompat::recvFrom(sock(vm, self).fdOrThrow("recvfrom"), buf.data(), n,
                                                reinterpret_cast<sockaddr*>(&ss), &sl);
            if (got < 0) throw KiritoError("recvfrom failed: " + netcompat::lastError());
            std::string host; int port;
            net::formatAddr(reinterpret_cast<sockaddr*>(&ss), sl, host, port);
            RootScope rs(vm);
            Handle data = rs.add(vm.alloc(std::make_unique<BytesVal>(std::string(buf.data(), static_cast<std::size_t>(got)))));
            List addr(vm);
            addr.push(vm.makeString(host));
            addr.push(vm.makeInt(static_cast<int64_t>(port)));
            List out(vm);
            out.push(data);
            out.push(addr);
            return out.handle();
        });
    // --- half-close / address introspection ---
    if (name == "shutdown")
        return bind("shutdown", {"how"}, [self, sock](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            std::string how = a.empty() ? "both" : asStr(vm, a[0]);
            int h;
            if (how == "read" || how == "r") h = 0;
            else if (how == "write" || how == "w") h = 1;
            else if (how == "both" || how == "rw" || how == "readwrite") h = 2;
            else throw KiritoError("shutdown: how must be 'read', 'write', or 'both' (got '" + how + "')");
            if (netcompat::shutdownSocket(sock(vm, self).fdOrThrow("shutdown"), h) != 0)
                throw KiritoError("shutdown failed: " + netcompat::lastError());
            return vm.none();
        });
    if (name == "getsockname" || name == "getpeername") {
        bool peer = (name == "getpeername");
        return bind(peer ? "getpeername" : "getsockname", {}, [self, sock, peer](KiritoVM& vm, std::span<const Handle>) -> Handle {
            auto& s = sock(vm, self);
            const char* op = peer ? "getpeername" : "getsockname";
            sockaddr_storage ss{}; socklen_t sl = sizeof(ss);
            int rc = peer ? ::getpeername(s.fdOrThrow(op), reinterpret_cast<sockaddr*>(&ss), &sl)
                          : ::getsockname(s.fdOrThrow(op), reinterpret_cast<sockaddr*>(&ss), &sl);
            if (rc != 0) throw KiritoError(std::string(op) + " failed: " + netcompat::lastError());
            std::string host; int port;
            net::formatAddr(reinterpret_cast<sockaddr*>(&ss), sl, host, port);
            List out(vm);
            out.push(vm.makeString(host));
            out.push(vm.makeInt(static_cast<int64_t>(port)));
            return out.handle();
        });
    }
    // --- socket options (string-keyed; no raw SO_*/IPPROTO_* integers leak into Kirito) ---
    if (name == "setsockopt")
        return bindReq("setsockopt", 2, {"option", "value"}, [self, sock](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            std::string opt = asStr(vm, a[0]);
            int64_t val = asInt(vm, a[1]);
            net::SockOpt so;
            if (!net::lookupSockOpt(opt, so))
                throw KiritoError("setsockopt: unknown option '" + opt + "' (valid: " + net::sockOptNames() + ")");
            if (!netcompat::setSockOptInt(sock(vm, self).fdOrThrow("setsockopt"), so.level, so.opt, static_cast<int>(val)))
                throw KiritoError("setsockopt(" + opt + ") failed: " + netcompat::lastError());
            return vm.none();
        });
    if (name == "getsockopt")
        return bindReq("getsockopt", 1, {"option"}, [self, sock](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            std::string opt = asStr(vm, a[0]);
            net::SockOpt so;
            if (!net::lookupGetSockOpt(opt, so))
                throw KiritoError("getsockopt: unknown option '" + opt + "' (valid: " + net::getSockOptNames() + ")");
            int val = 0;
            if (!netcompat::getSockOptInt(sock(vm, self).fdOrThrow("getsockopt"), so.level, so.opt, val))
                throw KiritoError("getsockopt(" + opt + ") failed: " + netcompat::lastError());
            return vm.makeInt(val);
        });
    {
        // Named boolean conveniences over setsockopt (the common toggles), so callers needn't remember
        // the option strings for the everyday cases.
        auto boolSetter = [&](const char* methodName, int level, int opt) {
            return bindReq(methodName, 1, {"flag"}, [self, sock, level, opt, methodName](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                int on = Value(vm, a[0]).truthy() ? 1 : 0;
                if (!netcompat::setSockOptInt(sock(vm, self).fdOrThrow(methodName), level, opt, on))
                    throw KiritoError(std::string(methodName) + " failed: " + netcompat::lastError());
                return vm.none();
            });
        };
        if (name == "setreuseaddr") return boolSetter("setreuseaddr", SOL_SOCKET, SO_REUSEADDR);
        if (name == "setbroadcast") return boolSetter("setbroadcast", SOL_SOCKET, SO_BROADCAST);
        if (name == "setkeepalive") return boolSetter("setkeepalive", SOL_SOCKET, SO_KEEPALIVE);
        if (name == "setnodelay")   return boolSetter("setnodelay",   IPPROTO_TCP, TCP_NODELAY);
    }
    if (name == "setblocking")
        return bindReq("setblocking", 1, {"flag"}, [self, sock](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            bool blocking = Value(vm, a[0]).truthy();
            if (!netcompat::setBlocking(sock(vm, self).fdOrThrow("setblocking"), blocking))
                throw KiritoError("setblocking failed: " + netcompat::lastError());
            return vm.none();
        });
    if (name == "fileno")   // the raw fd, non-destructively (cf. detach, which relinquishes ownership)
        return bind("fileno", {}, [self, sock](KiritoVM& vm, std::span<const Handle>) -> Handle {
            auto& s = sock(vm, self);
            // -1 on a closed/detached socket (Python semantics): the underlying fd is gone or recycled,
            // so handing its old number to net.fromfd would adopt an unrelated fd.
            if (s.closed || !netcompat::isValid(s.fd)) return vm.makeInt(-1);
            return vm.makeInt(static_cast<int64_t>(s.fd));
        });
    if (name == "close" || name == "_exit_")
        return bind("close", {}, [self, sock](KiritoVM& vm, std::span<const Handle>) -> Handle {
            sock(vm, self).closeFd();
            return vm.none();
        });
    // detach() -> Integer: surrender the raw fd to the caller and stop owning it (the destructor won't
    // close it). Lets an acceptor hand a connection to a worker VM via net.fromfd in one OS process.
    if (name == "detach")
        return bind("detach", {}, [self, sock](KiritoVM& vm, std::span<const Handle>) -> Handle {
            auto& s = sock(vm, self);
            if (s.isTls()) throw KiritoError("detach: cannot detach a TLS socket (the TLS session owns the fd)");
            int64_t fd = static_cast<int64_t>(s.fdOrThrow("detach"));  // detach-twice throws, not a stale fd
            s.closed = true;             // relinquish ownership: ~SocketVal must not close this fd
            s.fd = netcompat::kInvalidSocket;  // and the old number can't leak out via a second detach/fileno
            return vm.makeInt(fd);
        });
    // --- TLS: upgrade a connected stream socket to TLS (STARTTLS, or implicit TLS right after connect).
    if (name == "is_tls") return vm.makeBool(sock(vm, self).isTls());
    if (name == "starttls")
        return bind("starttls", {"server_hostname", "verify"}, [self, sock](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            auto& s = sock(vm, self);
            (void)s.fdOrThrow("starttls");
            if (s.socktype != SOCK_STREAM) throw KiritoError("starttls: only a stream (TCP) socket can use TLS");
            if (s.isTls()) throw KiritoError("starttls: this socket is already using TLS");
            // server_hostname (arg 0) drives SNI + certificate verification; defaults to the connect() host.
            std::string host = s.peerHost;
            if (a.size() > 0 && vm.arena().deref(a[0]).kind() != ValueKind::None)
                host = Value(vm, a[0]).asStringRef("starttls server_hostname");
            bool verify = a.size() > 1 ? vm.arena().deref(a[1]).truthy() : true;
#ifdef KIRITO_ENABLE_TLS
            if (verify && host.empty())
                throw KiritoError("starttls: a server_hostname is required for certificate verification (or pass verify=False)");
            SSL* ssl = nullptr; SSL_CTX* ctx = nullptr;
            net::tlsClientHandshake(s.fd, host, verify, ssl, ctx);  // throws (fd left open) on any failure
            s.ssl_ = ssl; s.sslCtx_ = ctx;
            return vm.none();
#else
            (void)host; (void)verify;
            throw KiritoError("starttls: socket TLS requires building with KIRITO_ENABLE_TLS (OpenSSL); "
                              "net.tlsenabled is False in this build");
#endif
        });
    if (name == "cipher")
        return bind("cipher", {}, [self, sock](KiritoVM& vm, std::span<const Handle>) -> Handle {
#ifdef KIRITO_ENABLE_TLS
            auto& s = sock(vm, self);
            if (s.ssl_) { const char* c = SSL_get_cipher(s.ssl_); if (c) return vm.makeString(c); }
#else
            (void)sock; (void)self;
#endif
            return vm.none();   // not a TLS socket (or a non-TLS build) -> None
        });
    if (name == "_enter_")
        return bind("_enter_", {}, [self](KiritoVM&, std::span<const Handle>) { return self; });
    return Object::getAttr(vm, self, name);
}

inline Handle ResponseVal::getAttr(KiritoVM& vm, Handle self, std::string_view name) {
    if (name == "status" || name == "statuscode") return vm.makeInt(status);
    if (name == "reason") return vm.makeString(reason);
    if (name == "ok") return vm.makeBool(status >= 100 && status < 400);
    if (name == "url") return vm.makeString(url);
    if (name == "text" || name == "body") return vm.makeString(body);   // decoded text (a String)
    if (name == "content") return vm.alloc(std::make_unique<BytesVal>(body));  // raw bytes (Bytes)
    if (name == "headers") return headersH;
    if (name == "cookies") return cookiesH;
    auto bind = [&](const char* nm, std::vector<std::string> params, NativeFn fn) {
        return makeMethod(vm, nm, std::move(params), std::move(fn), std::vector<Handle>{self});
    };
    if (name == "json")
        return bind("json", {}, [self](KiritoVM& vm, std::span<const Handle>) -> Handle {
            auto& r = static_cast<ResponseVal&>(vm.arena().deref(self));
            RootScope rs(vm);
            return json::Parser(vm, r.body, rs).parse();
        });
    if (name == "raiseforstatus")
        return bind("raiseforstatus", {}, [self](KiritoVM& vm, std::span<const Handle>) -> Handle {
            auto& r = static_cast<ResponseVal&>(vm.arena().deref(self));
            if (r.status >= 400)
                throw KiritoError("HTTP " + std::to_string(r.status) +
                                  (r.reason.empty() ? "" : " " + r.reason) + " for " + r.url);
            return self;
        });
    if (name == "header")
        return bind("header", {"name", "default"}, [self](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            auto& r = static_cast<ResponseVal&>(vm.arena().deref(self));
            if (a.empty()) throw KiritoError("header() expected at least 1 argument (the header name)");
            std::string want = net::asciiLower(Value(vm, a[0]).asStringRef("header"));
            const DictVal& hd = static_cast<const DictVal&>(vm.arena().deref(r.headersH));
            for (Handle k : hd.keys())
                if (net::asciiLower(static_cast<const StrVal&>(vm.arena().deref(k)).value()) == want)
                    return *hd.find(vm.arena(), k);
            return a.size() > 1 ? a[1] : vm.none();
        });
    return Object::getAttr(vm, self, name);
}

class NetModule : public NativeModule {
public:
    std::string name() const override { return "net"; }
    void setup(ModuleBuilder& m) override {
        KiritoVM& vm = m.vm();
        // Whether this build has HTTPS/TLS (compiled with KIRITO_ENABLE_TLS). Lets Kirito code branch —
        // e.g. skip an https:// call, or a test — instead of hitting the "requires TLS" error.
#ifdef KIRITO_ENABLE_TLS
        m.value("tlsenabled", vm.makeBool(true));
#else
        m.value("tlsenabled", vm.makeBool(false));
#endif

        // --- socket constructors ---
        // Socket() is the historical TCP/IPv4 default; socket(family, type) is the general form and
        // tcpsocket/udpsocket are the common shortcuts. family: "inet"/"inet6"; type: "stream"/"dgram".
        m.fn("Socket", {}, "Socket", [](KiritoVM& vm, std::span<const Handle>) -> Handle {
            return vm.alloc(std::make_unique<SocketVal>());
        });
        m.fn("socket", {{"family", "String", vm.makeString("inet")}, {"type", "String", vm.makeString("stream")}}, "Socket",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                 Args args(vm, a, "socket");
                 int fam = net::parseFamily(args[0].asStringRef("family"));
                 int typ = net::parseType(args[1].asStringRef("type"));
                 return vm.alloc(std::make_unique<SocketVal>(fam, typ));
             });
        m.fn("tcpsocket", {{"family", "String", vm.makeString("inet")}}, "Socket",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                 int fam = net::parseFamily(Args(vm, a, "tcpsocket")[0].asStringRef("family"));
                 return vm.alloc(std::make_unique<SocketVal>(fam, SOCK_STREAM));
             });
        m.fn("udpsocket", {{"family", "String", vm.makeString("inet")}}, "Socket",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                 int fam = net::parseFamily(Args(vm, a, "udpsocket")[0].asStringRef("family"));
                 return vm.alloc(std::make_unique<SocketVal>(fam, SOCK_DGRAM));
             });
        // socketpair(type) -> [Socket, Socket]: a connected, share-nothing pair (Python socketpair).
        // POSIX uses AF_UNIX natively; Windows emulates a stream pair over 127.0.0.1 (dgram unsupported).
        m.fn("socketpair", {{"type", "String", vm.makeString("stream")}}, "List",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                 std::string ts = Args(vm, a, "socketpair")[0].asStringRef("type");
                 int typ = net::parseType(ts);
                 netcompat::socket_t fds[2];
                 if (!netcompat::socketPair(typ, fds))
                     throw KiritoError("socketpair(" + ts + ") failed: " + netcompat::lastError());
                 RootScope rs(vm);
                 Handle s0 = rs.add(vm.alloc(std::make_unique<SocketVal>(fds[0], AF_INET, typ)));
                 Handle s1 = rs.add(vm.alloc(std::make_unique<SocketVal>(fds[1], AF_INET, typ)));
                 List out(vm);
                 out.push(s0);
                 out.push(s1);
                 return out.handle();
             });
        // fromfd(fd[, family, type]) -> Socket: adopt an existing raw fd (e.g. one handed over by
        // socket.detach() to a worker VM). Valid only within the same OS process.
        m.fn("fromfd", {{"fd", "Integer"}, {"family", "String", vm.makeString("inet")}, {"type", "String", vm.makeString("stream")}}, "Socket",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                 Args args(vm, a, "fromfd");
                 int64_t fd = args[0].asInt("fromfd");
                 int fam = net::parseFamily(args[1].asStringRef("family"));
                 int typ = net::parseType(args[2].asStringRef("type"));
                 return vm.alloc(std::make_unique<SocketVal>(static_cast<netcompat::socket_t>(fd), fam, typ));
             });

        // --- name resolution ---
        m.fn("gethostname", {}, "String", [](KiritoVM& vm, std::span<const Handle>) -> Handle {
            return vm.makeString(netcompat::hostName());
        });
        // gethostbyname(host) -> the first IPv4 address as a string (Python's IPv4-only semantics).
        m.fn("gethostbyname", {{"host", "String"}}, "String", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            std::string host = Args(vm, a, "gethostbyname")[0].asStringRef("host");
            sockaddr_storage ss{}; socklen_t sl = 0; std::string err;
            if (!net::resolveAddr(host, 0, AF_INET, SOCK_STREAM, ss, sl, err))
                throw KiritoError("gethostbyname: cannot resolve '" + host + "': " + err);
            std::string ip; int port;
            net::formatAddr(reinterpret_cast<sockaddr*>(&ss), sl, ip, port);
            return vm.makeString(ip);
        });
        // getaddrinfo(host[, port[, family[, type]]]) -> List of {family, type, host, port} dicts.
        m.fn("getaddrinfo", {{"host", "String"}, {"port", "", vm.none()}, {"family", "String", vm.makeString("")}, {"type", "String", vm.makeString("")}}, "List",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                 Args args(vm, a, "getaddrinfo");
                 std::string host = args[0].asStringRef("host");
                 std::string portStr;
                 Value p(vm, a[1]);
                 if (p.isNone()) portStr = "";
                 else if (p.isString()) portStr = p.asStringRef("port");
                 else portStr = std::to_string(p.asInt("port"));
                 int fam = AF_UNSPEC, typ = 0;
                 std::string fs = args[2].asStringRef("family"); if (!fs.empty()) fam = net::parseFamily(fs);
                 std::string ty = args[3].asStringRef("type");   if (!ty.empty()) typ = net::parseType(ty);
                 netcompat::startup();
                 addrinfo hints{}; hints.ai_family = fam; hints.ai_socktype = typ;
                 addrinfo* res = nullptr;
                 int rc = ::getaddrinfo(host.empty() ? nullptr : host.c_str(),
                                        portStr.empty() ? nullptr : portStr.c_str(), &hints, &res);
                 if (rc != 0) throw KiritoError("getaddrinfo('" + host + "') failed: " + std::string(::gai_strerror(rc)));
                 RootScope rs(vm);
                 List out(vm);
                 for (addrinfo* ai = res; ai; ai = ai->ai_next) {
                     std::string ip; int port;
                     net::formatAddr(ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen), ip, port);
                     Dict d(vm, {{"family", net::familyName(ai->ai_family)},
                                 {"type", net::typeName(ai->ai_socktype)},
                                 {"host", ip}, {"port", static_cast<int64_t>(port)}});
                     out.push(d);
                 }
                 ::freeaddrinfo(res);
                 return out.handle();
             });
        m.fn("Session", {}, "Session", [](KiritoVM& vm, std::span<const Handle>) -> Handle {
            auto s = std::make_unique<SessionVal>();
            RootScope rs(vm);
            s->headersH = rs.add(Dict(vm).handle());
            s->cookiesH = rs.add(Dict(vm).handle());
            return vm.alloc(std::move(s));
        });

        // --- HTTP client (requests-style) ---
        // request(method, url[, options]) and the per-verb shortcuts return a Response.
        m.fn("request", {{"method", "String"}, {"url", "String"}, {"options", "", vm.none()}}, "Response",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                 Args args(vm, a, "request");
                 Handle opts = args.size() > 2 ? a[2] : vm.none();
                 return netRequest(vm, args[0].asStringRef("method"), args[1].asStringRef("url"), opts);
             });
        auto verb = [&m](const char* fnName, const char* method) {
            m.fn(fnName, {{"url", "String"}, {"options", "", m.vm().none()}}, "Response",
                 [method](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                     Args args(vm, a, "http");
                     Handle opts = args.size() > 1 ? a[1] : vm.none();
                     return netRequest(vm, method, args[0].asStringRef("url"), opts);
                 });
        };
        verb("get", "GET");
        verb("post", "POST");
        verb("put", "PUT");
        verb("delete", "DELETE");
        verb("patch", "PATCH");
        verb("head", "HEAD");
        verb("options", "OPTIONS");

        // --- URL helpers (urllib.parse style) ---
        m.fn("quote", {{"s", "String"}}, "String", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            return Value(vm, net::percentEncode(Args(vm, a, "quote")[0].asStringRef("s")));
        });
        m.fn("unquote", {{"s", "String"}}, "String", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            return Value(vm, net::percentDecode(Args(vm, a, "unquote")[0].asStringRef("s")));
        });
        m.fn("urlencode", {{"params", "Dict"}}, "String", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            return Value(vm, formUrlencode(vm, a[0]));
        });
        m.fn("parseqs", {{"query", "String"}}, "Dict", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            std::string q = Args(vm, a, "parseqs")[0].asStringRef("parseqs");
            Dict d(vm);
            std::size_t i = 0;
            while (i < q.size()) {
                std::size_t amp = q.find('&', i);
                std::string pair = q.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
                std::size_t eq = pair.find('=');
                std::string k = net::percentDecode(eq == std::string::npos ? pair : pair.substr(0, eq));
                std::string v = eq == std::string::npos ? "" : net::percentDecode(pair.substr(eq + 1));
                if (!k.empty()) d.set(k, Value(vm, v));
                if (amp == std::string::npos) break;
                i = amp + 1;
            }
            return d;
        });
        m.fn("urlsplit", {{"url", "String"}}, "Dict", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            net::UrlParts p = net::splitUrl(Args(vm, a, "urlsplit")[0].asStringRef("urlsplit"));
            return Dict(vm, {{"scheme", p.scheme}, {"host", p.host}, {"port", p.port},
                             {"path", p.path}, {"query", p.query}, {"fragment", p.fragment}});
        });
    }
};

}  // namespace kirito

#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
#endif
