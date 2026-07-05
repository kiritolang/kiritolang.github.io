// Deep TLS tests for the net HTTP client. Compiled only when KIRITO_ENABLE_TLS is defined (the debug
// and release presets turn it on); otherwise this is an empty program. A real in-process OpenSSL
// server with an ephemeral, self-signed certificate stands up on 127.0.0.1 and the Kirito client
// (net.get) drives an end-to-end HTTPS handshake against it — no external network. Covers:
//   * verify=False accepts a self-signed cert and round-trips the body,
//   * verify=True REJECTS a self-signed cert (no trusted CA),
//   * verify=True SUCCEEDS once the cert is placed in the trust store (SSL_CERT_FILE),
//   * hostname verification: a cert whose SAN does not cover 127.0.0.1 is rejected under verify=True,
//   * chunked transfer-encoding and gzip content-encoding decode correctly over TLS,
//   * a large (~512 KiB) body round-trips over TLS,
//   * net.tlsenabled reports True in this build.
#include "kirito.hpp"

#ifndef KIRITO_ENABLE_TLS
int main() { return 0; }   // TLS not compiled in — nothing to exercise here.
#else

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <cstdio>
#include <functional>
#include <string>
#include <thread>

#include "../check.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) { return vm.stringify(vm.runSource(src)); }
static ResponseVal& evalResponse(KiritoVM& vm, const std::string& src) {
    return static_cast<ResponseVal&>(vm.arena().deref(vm.runSource(src)));
}

// A listening TCP socket on an ephemeral 127.0.0.1 port; returns fd, sets `port`.
static int makeListener(int& port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return -1;
    ::listen(fd, 8);
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    port = ntohs(addr.sin_port);
    return fd;
}

// An ephemeral self-signed cert + key (RSA-2048), self-issued so it is its own CA — usable both as
// the server identity and, when placed in the client's trust store, as the trust anchor.
struct KeyCert { EVP_PKEY* key = nullptr; X509* cert = nullptr; };
static KeyCert makeSelfSigned(const std::string& cn, const std::string& san) {
    KeyCert kc;
    kc.key = EVP_RSA_gen(2048);
    X509* x = X509_new();
    X509_set_version(x, 2);  // v3
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), -3600);
    X509_gmtime_adj(X509_getm_notAfter(x), 3600 * 24);
    X509_set_pubkey(x, kc.key);
    X509_NAME* name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(cn.c_str()), -1, -1, 0);
    X509_set_issuer_name(x, name);  // self-signed: issuer == subject
    X509V3_CTX vctx;
    X509V3_set_ctx_nodb(&vctx);
    X509V3_set_ctx(&vctx, x, x, nullptr, nullptr, 0);
    auto addExt = [&](int nid, const char* val) {
        X509_EXTENSION* ex = X509V3_EXT_conf_nid(nullptr, &vctx, nid, val);
        if (ex) { X509_add_ext(x, ex, -1); X509_EXTENSION_free(ex); }
    };
    addExt(NID_basic_constraints, "critical,CA:TRUE");
    if (!san.empty()) addExt(NID_subject_alt_name, san.c_str());
    X509_sign(x, kc.key, EVP_sha256());
    kc.cert = x;
    return kc;
}

// Write a cert to a PEM file (for the client's SSL_CERT_FILE trust store). Returns the path.
static std::string writeCertPem(X509* cert, const std::string& tag) {
    std::string path = "/tmp/kirito_tls_" + std::to_string(::getpid()) + "_" + tag + ".pem";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f) { PEM_write_X509(f, cert); std::fclose(f); }
    return path;
}

// A server SSL_CTX presenting `kc`.
static SSL_CTX* makeServerCtx(const KeyCert& kc) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    SSL_CTX_use_certificate(ctx, kc.cert);
    SSL_CTX_use_PrivateKey(ctx, kc.key);
    return ctx;
}

// Serve `n` sequential TLS connections: read the request headers, reply with handler(request).
static void serveTls(int srv, int n, SSL_CTX* ctx, std::function<std::string(const std::string&)> handler) {
    for (int i = 0; i < n; ++i) {
        int c = ::accept(srv, nullptr, nullptr);
        if (c < 0) break;
        SSL* ssl = SSL_new(ctx);
        SSL_set_fd(ssl, c);
        if (SSL_accept(ssl) == 1) {
            std::string req;
            char buf[4096];
            while (req.find("\r\n\r\n") == std::string::npos) {
                int r = SSL_read(ssl, buf, sizeof(buf));
                if (r <= 0) break;
                req.append(buf, static_cast<std::size_t>(r));
            }
            std::string resp = handler(req);
            std::size_t sent = 0;
            while (sent < resp.size()) {
                int w = SSL_write(ssl, resp.data() + sent, static_cast<int>(resp.size() - sent));
                if (w <= 0) break;
                sent += static_cast<std::size_t>(w);
            }
        }
        SSL_shutdown(ssl);
        SSL_free(ssl);
        ::close(c);
    }
    ::close(srv);
}

static std::string httpResp(const std::string& body, const std::string& extraHeaders = "") {
    return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + extraHeaders +
           "Connection: close\r\n\r\n" + body;
}
static std::string gzipWrap(const std::string& s) {
    return std::string("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03", 10) + deflate::compress(s) +
           std::string(8, '\0');
}
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

static std::string urlFor(int port, const std::string& path = "/") {
    return "https://127.0.0.1:" + std::to_string(port) + path;
}

int main() {
    KiritoVM vm;
    SSL_library_init();
    SSL_load_error_strings();

    // net.tlsenabled must report True in a TLS build.
    CHECK(evalStr(vm, "import(\"net\").tlsenabled\n") == "True");

    // The good identity: covers 127.0.0.1 (via IP SAN) and localhost.
    KeyCert good = makeSelfSigned("localhost", "DNS:localhost,IP:127.0.0.1");
    std::string goodPem = writeCertPem(good.cert, "good");

    // --- verify=False: accept the self-signed cert, round-trip the body. ---
    {
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        SSL_CTX* ctx = makeServerCtx(good);
        std::thread server(serveTls, srv, 1, ctx, [](const std::string&) { return httpResp("tls-ok"); });
        ResponseVal& r = evalResponse(vm, "import(\"net\").get(\"" + urlFor(port) + "\", {\"verify\": False})\n");
        CHECK(r.status == 200);
        CHECK(r.body == "tls-ok");
        server.join();
        SSL_CTX_free(ctx);
    }

    // --- verify=True with an UNtrusted self-signed cert: the handshake must fail (throw). ---
    {
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        SSL_CTX* ctx = makeServerCtx(good);
        ::unsetenv("SSL_CERT_FILE");   // ensure our cert is NOT trusted
        std::thread server(serveTls, srv, 1, ctx, [](const std::string&) { return httpResp("nope"); });
        CHECK_THROWS(vm.runSource("import(\"net\").get(\"" + urlFor(port) + "\", {\"verify\": True})\n"));
        server.join();
        SSL_CTX_free(ctx);
    }

    // --- verify=True with the cert placed in the trust store (SSL_CERT_FILE): must succeed, and the
    //     127.0.0.1 IP-SAN satisfies hostname verification. ---
    {
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        SSL_CTX* ctx = makeServerCtx(good);
        ::setenv("SSL_CERT_FILE", goodPem.c_str(), 1);
        std::thread server(serveTls, srv, 1, ctx, [](const std::string&) { return httpResp("trusted"); });
        ResponseVal& r = evalResponse(vm, "import(\"net\").get(\"" + urlFor(port) + "\", {\"verify\": True})\n");
        CHECK(r.status == 200);
        CHECK(r.body == "trusted");
        server.join();
        SSL_CTX_free(ctx);
        ::unsetenv("SSL_CERT_FILE");
    }

    // --- hostname verification: a TRUSTED cert whose SAN does NOT cover 127.0.0.1 is still rejected
    //     under verify=True (the name check fails even though the chain is trusted). ---
    {
        KeyCert wrong = makeSelfSigned("wronghost.example", "DNS:wronghost.example");
        std::string wrongPem = writeCertPem(wrong.cert, "wrong");
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        SSL_CTX* ctx = makeServerCtx(wrong);
        ::setenv("SSL_CERT_FILE", wrongPem.c_str(), 1);   // trusted chain, but wrong hostname
        std::thread server(serveTls, srv, 1, ctx, [](const std::string&) { return httpResp("mismatch"); });
        CHECK_THROWS(vm.runSource("import(\"net\").get(\"" + urlFor(port) + "\", {\"verify\": True})\n"));
        server.join();
        SSL_CTX_free(ctx);
        ::unsetenv("SSL_CERT_FILE");
        X509_free(wrong.cert);
        EVP_PKEY_free(wrong.key);
        ::remove(wrongPem.c_str());
    }

    // --- chunked transfer-encoding over TLS: the client de-chunks transparently. ---
    {
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        SSL_CTX* ctx = makeServerCtx(good);
        std::string body = "chunked-over-tls-body-0123456789";
        std::thread server(serveTls, srv, 1, ctx, [&](const std::string&) {
            return "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n" +
                   chunkEncode(body, 7);
        });
        ResponseVal& r = evalResponse(vm, "import(\"net\").get(\"" + urlFor(port) + "\", {\"verify\": False})\n");
        CHECK(r.status == 200);
        CHECK(r.body == body);
        server.join();
        SSL_CTX_free(ctx);
    }

    // --- gzip content-encoding over TLS: the client decompresses transparently. ---
    {
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        SSL_CTX* ctx = makeServerCtx(good);
        std::string body(4000, 'Z');
        body += "the quick brown fox";
        std::string gz = gzipWrap(body);
        std::thread server(serveTls, srv, 1, ctx, [&](const std::string&) {
            return "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: " +
                   std::to_string(gz.size()) + "\r\nConnection: close\r\n\r\n" + gz;
        });
        ResponseVal& r = evalResponse(vm, "import(\"net\").get(\"" + urlFor(port) + "\", {\"verify\": False})\n");
        CHECK(r.status == 200);
        CHECK(r.body == body);
        server.join();
        SSL_CTX_free(ctx);
    }

    // --- a large (~512 KiB) body round-trips over TLS (many SSL_read chunks). ---
    {
        int port = 0, srv = makeListener(port);
        CHECK(srv >= 0);
        SSL_CTX* ctx = makeServerCtx(good);
        std::string body;
        body.reserve(512 * 1024);
        for (int i = 0; i < 512 * 1024; ++i) body += static_cast<char>('A' + (i % 26));
        std::thread server(serveTls, srv, 1, ctx, [&](const std::string&) { return httpResp(body); });
        ResponseVal& r = evalResponse(vm, "import(\"net\").get(\"" + urlFor(port) + "\", {\"verify\": False})\n");
        CHECK(r.status == 200);
        CHECK(r.body.size() == body.size());
        CHECK(r.body == body);
        server.join();
        SSL_CTX_free(ctx);
    }

    X509_free(good.cert);
    EVP_PKEY_free(good.key);
    ::remove(goodPem.c_str());
    return RUN_TESTS();
}

#endif  // KIRITO_ENABLE_TLS
