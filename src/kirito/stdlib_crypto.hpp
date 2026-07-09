#ifndef KIRITO_STDLIB_CRYPTO_HPP
#define KIRITO_STDLIB_CRYPTO_HPP

// The `crypto` module: authenticated encryption (AES-GCM), public-key signatures and encryption
// (RSA / EC), and X.509 certificate parsing — built on OpenSSL and therefore GATED on
// KIRITO_ENABLE_TLS, exactly like the `net` module's HTTPS/TLS. In a build WITHOUT OpenSSL the module
// still imports (so `import("crypto")` never fails and `crypto.enabled` is readable) but every
// operation throws a clear "requires KIRITO_ENABLE_TLS" error rather than silently doing nothing.
//
// Keys are PEM strings (portable, serialize-friendly, no native key object to leak). Byte inputs are
// String-or-Bytes; byte outputs are Bytes; ciphers/hashes are named by lowercase string. Every
// OpenSSL resource is owned by a unique_ptr with the matching *_free deleter (no leak on the throw
// paths), and the error queue is cleared on entry so a stale error can't be misreported.

#include <memory>
#include <span>
#include <string>

#include "builtins.hpp"
#include "bytes.hpp"
#include "collections.hpp"
#include "native.hpp"

#ifdef KIRITO_ENABLE_TLS
#include <climits>
#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509v3.h>
#endif

namespace kirito {

#ifdef KIRITO_ENABLE_TLS
// RAII owners + small helpers for the OpenSSL surface. Header-only, no global state (matches the VM
// encapsulation rule): every handle is freed by its unique_ptr deleter.
namespace cryptossl {

struct PKeyDel { void operator()(EVP_PKEY* p) const { EVP_PKEY_free(p); } };
struct PKeyCtxDel { void operator()(EVP_PKEY_CTX* p) const { EVP_PKEY_CTX_free(p); } };
struct CipherCtxDel { void operator()(EVP_CIPHER_CTX* p) const { EVP_CIPHER_CTX_free(p); } };
struct MdCtxDel { void operator()(EVP_MD_CTX* p) const { EVP_MD_CTX_free(p); } };
struct BioDel { void operator()(BIO* p) const { BIO_free(p); } };
struct X509Del { void operator()(X509* p) const { X509_free(p); } };
using PKey = std::unique_ptr<EVP_PKEY, PKeyDel>;
using PKeyCtx = std::unique_ptr<EVP_PKEY_CTX, PKeyCtxDel>;
using CipherCtx = std::unique_ptr<EVP_CIPHER_CTX, CipherCtxDel>;
using MdCtx = std::unique_ptr<EVP_MD_CTX, MdCtxDel>;
using Bio = std::unique_ptr<BIO, BioDel>;
using X509Ptr = std::unique_ptr<X509, X509Del>;

inline const unsigned char* uc(const std::string& s) {
    return reinterpret_cast<const unsigned char*>(s.data());
}
inline unsigned char* ucw(std::string& s) { return reinterpret_cast<unsigned char*>(s.data()); }

inline int intLen(std::size_t n, const char* who) {
    if (n > static_cast<std::size_t>(INT_MAX)) throw KiritoError(std::string(who) + ": input too large");
    return static_cast<int>(n);
}

// The most recent OpenSSL error, appended to a context string (or just the context if the queue is
// empty). Callers ERR_clear_error() on entry so this reports THEIR failure, not a leftover.
inline std::string sslError(const std::string& ctx) {
    unsigned long e = ERR_get_error();
    if (!e) return ctx;
    char buf[256];
    ERR_error_string_n(e, buf, sizeof(buf));
    return ctx + ": " + buf;
}

inline const EVP_MD* mdByName(const std::string& algo) {
    if (algo == "sha256") return EVP_sha256();
    if (algo == "sha384") return EVP_sha384();
    if (algo == "sha512") return EVP_sha512();
    if (algo == "sha1") return EVP_sha1();
    throw KiritoError("crypto: unknown hash '" + algo + "' (use sha1/sha256/sha384/sha512)");
}

inline const EVP_CIPHER* gcmCipher(std::size_t keyLen) {
    switch (keyLen) {
        case 16: return EVP_aes_128_gcm();
        case 24: return EVP_aes_192_gcm();
        case 32: return EVP_aes_256_gcm();
        default: throw KiritoError("aes: key must be 16, 24 or 32 bytes (AES-128/192/256)");
    }
}

inline std::string bioRead(BIO* bio) {
    char* data = nullptr;
    long len = BIO_get_mem_data(bio, &data);
    return len > 0 ? std::string(data, static_cast<std::size_t>(len)) : std::string();
}
inline std::string privateToPEM(EVP_PKEY* k) {
    Bio bio(BIO_new(BIO_s_mem()));
    if (!bio || PEM_write_bio_PrivateKey(bio.get(), k, nullptr, nullptr, 0, nullptr, nullptr) != 1)
        throw KiritoError(sslError("write private key PEM"));
    return bioRead(bio.get());
}
inline std::string publicToPEM(EVP_PKEY* k) {
    Bio bio(BIO_new(BIO_s_mem()));
    if (!bio || PEM_write_bio_PUBKEY(bio.get(), k) != 1)
        throw KiritoError(sslError("write public key PEM"));
    return bioRead(bio.get());
}
inline PKey loadPrivate(const std::string& pem) {
    Bio bio(BIO_new_mem_buf(pem.data(), intLen(pem.size(), "private key")));
    EVP_PKEY* k = bio ? PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr) : nullptr;
    if (!k) throw KiritoError(sslError("invalid private key PEM"));
    return PKey(k);
}
inline PKey loadPublic(const std::string& pem) {
    Bio bio(BIO_new_mem_buf(pem.data(), intLen(pem.size(), "public key")));
    EVP_PKEY* k = bio ? PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr) : nullptr;
    if (!k) throw KiritoError(sslError("invalid public key PEM"));
    return PKey(k);
}

// EVP_DigestSign one-shot — works for both RSA (PKCS#1 v1.5) and EC (ECDSA), so rsasign / ecsign
// share it (and rsaverify / ecverify share pkeyVerify).
inline std::string pkeySign(const std::string& privPem, const std::string& msg, const std::string& algo) {
    PKey key = loadPrivate(privPem);
    MdCtx ctx(EVP_MD_CTX_new());
    if (!ctx || EVP_DigestSignInit(ctx.get(), nullptr, mdByName(algo), nullptr, key.get()) != 1)
        throw KiritoError(sslError("sign init"));
    std::size_t siglen = 0;
    if (EVP_DigestSign(ctx.get(), nullptr, &siglen, uc(msg), msg.size()) != 1)
        throw KiritoError(sslError("sign"));
    std::string sig(siglen, '\0');
    if (EVP_DigestSign(ctx.get(), ucw(sig), &siglen, uc(msg), msg.size()) != 1)
        throw KiritoError(sslError("sign"));
    sig.resize(siglen);
    return sig;
}
inline bool pkeyVerify(const std::string& pubPem, const std::string& msg, const std::string& sig,
                       const std::string& algo) {
    PKey key = loadPublic(pubPem);
    MdCtx ctx(EVP_MD_CTX_new());
    if (!ctx || EVP_DigestVerifyInit(ctx.get(), nullptr, mdByName(algo), nullptr, key.get()) != 1)
        throw KiritoError(sslError("verify init"));
    int rc = EVP_DigestVerify(ctx.get(), uc(sig), sig.size(), uc(msg), msg.size());
    if (rc == 1) return true;
    if (rc == 0) return false;                 // a genuine signature mismatch — not an error
    throw KiritoError(sslError("verify"));      // negative: a hard error (wrong key type, etc.)
}

// RSA-OAEP (SHA-256) encrypt/decrypt through the same EVP_PKEY_CTX path.
inline std::string rsaOaep(EVP_PKEY* key, const std::string& in, bool encrypt) {
    PKeyCtx ctx(EVP_PKEY_CTX_new(key, nullptr));
    if (!ctx || (encrypt ? EVP_PKEY_encrypt_init(ctx.get()) : EVP_PKEY_decrypt_init(ctx.get())) != 1)
        throw KiritoError(sslError("rsa init"));
    if (EVP_PKEY_CTX_set_rsa_padding(ctx.get(), RSA_PKCS1_OAEP_PADDING) != 1 ||
        EVP_PKEY_CTX_set_rsa_oaep_md(ctx.get(), EVP_sha256()) != 1 ||
        EVP_PKEY_CTX_set_rsa_mgf1_md(ctx.get(), EVP_sha256()) != 1)
        throw KiritoError(sslError("rsa oaep setup"));
    auto op = encrypt ? EVP_PKEY_encrypt : EVP_PKEY_decrypt;
    const char* who = encrypt ? "rsaencrypt" : "rsadecrypt";
    std::size_t outlen = 0;
    if (op(ctx.get(), nullptr, &outlen, uc(in), in.size()) != 1) throw KiritoError(sslError(who));
    std::string out(outlen, '\0');
    if (op(ctx.get(), ucw(out), &outlen, uc(in), in.size()) != 1) throw KiritoError(sslError(who));
    out.resize(outlen);
    return out;
}

inline std::string asn1TimeStr(const ASN1_TIME* t) {
    if (!t) return "";
    Bio bio(BIO_new(BIO_s_mem()));
    if (!bio || ASN1_TIME_print(bio.get(), t) != 1) return "";
    return bioRead(bio.get());
}

}  // namespace cryptossl
#endif  // KIRITO_ENABLE_TLS

// The native-binding idiom: bound-method lambdas take a `vm` parameter that intentionally shadows the
// enclosing setup()'s `vm` (same VM, by design). Silence -Wshadow for these, as the other stdlib glue
// (net/random/...) does; it stays active in the compiler/VM/parser core.
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wshadow"
#endif

class CryptoModule : public NativeModule {
public:
    std::string name() const override { return "crypto"; }

    void setup(ModuleBuilder& m) override {
        KiritoVM& vm = m.vm();
#ifdef KIRITO_ENABLE_TLS
        m.value("enabled", vm.makeBool(true));
#else
        m.value("enabled", vm.makeBool(false));
#endif

        // --- AES-GCM authenticated encryption ---
        m.fn("aesencrypt", {{"key"}, {"plaintext"}, {"iv"}, {"aad", "", vm.none()}}, "Dict",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
#ifdef KIRITO_ENABLE_TLS
            using namespace cryptossl;
            ERR_clear_error();
            Args args(vm, a, "aesencrypt");
            const std::string& key = argStringOrBytes(vm, args[0].handle(), "aesencrypt key");
            const std::string& pt = argStringOrBytes(vm, args[1].handle(), "aesencrypt plaintext");
            const std::string& iv = argStringOrBytes(vm, args[2].handle(), "aesencrypt iv");
            const EVP_CIPHER* cipher = gcmCipher(key.size());
            if (iv.empty()) throw KiritoError("aesencrypt: iv must be non-empty");
            bool hasAad = !args[3].isNone();
            std::string aad = hasAad ? argStringOrBytes(vm, args[3].handle(), "aesencrypt aad") : std::string();

            CipherCtx ctx(EVP_CIPHER_CTX_new());
            int outl = 0, total = 0;
            if (!ctx || EVP_EncryptInit_ex(ctx.get(), cipher, nullptr, nullptr, nullptr) != 1 ||
                EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, intLen(iv.size(), "iv"), nullptr) != 1 ||
                EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, uc(key), uc(iv)) != 1)
                throw KiritoError(sslError("aesencrypt init"));
            if (hasAad && !aad.empty() &&
                EVP_EncryptUpdate(ctx.get(), nullptr, &outl, uc(aad), intLen(aad.size(), "aad")) != 1)
                throw KiritoError(sslError("aesencrypt aad"));
            std::string ct(pt.size(), '\0');
            if (!pt.empty()) {
                if (EVP_EncryptUpdate(ctx.get(), ucw(ct), &outl, uc(pt), intLen(pt.size(), "plaintext")) != 1)
                    throw KiritoError(sslError("aesencrypt"));
                total = outl;
            }
            if (EVP_EncryptFinal_ex(ctx.get(), ucw(ct) + total, &outl) != 1)
                throw KiritoError(sslError("aesencrypt final"));
            total += outl;
            ct.resize(static_cast<std::size_t>(total));
            std::string tag(16, '\0');
            if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, 16, ucw(tag)) != 1)
                throw KiritoError(sslError("aesencrypt tag"));
            Dict d(vm);
            d.set("ciphertext", Bytes(vm, std::move(ct)));
            d.set("tag", Bytes(vm, std::move(tag)));
            return d;
#else
            (void)vm; (void)a;
            throw KiritoError("crypto.aesencrypt requires building with KIRITO_ENABLE_TLS (OpenSSL)");
#endif
        });

        m.fn("aesdecrypt", {{"key"}, {"ciphertext"}, {"iv"}, {"tag"}, {"aad", "", vm.none()}}, "Bytes",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
#ifdef KIRITO_ENABLE_TLS
            using namespace cryptossl;
            ERR_clear_error();
            Args args(vm, a, "aesdecrypt");
            const std::string& key = argStringOrBytes(vm, args[0].handle(), "aesdecrypt key");
            const std::string& ct = argStringOrBytes(vm, args[1].handle(), "aesdecrypt ciphertext");
            const std::string& iv = argStringOrBytes(vm, args[2].handle(), "aesdecrypt iv");
            std::string tag = argStringOrBytes(vm, args[3].handle(), "aesdecrypt tag");
            const EVP_CIPHER* cipher = gcmCipher(key.size());
            if (iv.empty()) throw KiritoError("aesdecrypt: iv must be non-empty");
            // Require the full 16-byte GCM tag (what aesencrypt always emits): accepting a truncated
            // tag would weaken forgery resistance to ~1/256 per byte dropped (A12-2).
            if (tag.size() != 16) throw KiritoError("aesdecrypt: tag must be 16 bytes");
            bool hasAad = !args[4].isNone();
            std::string aad = hasAad ? argStringOrBytes(vm, args[4].handle(), "aesdecrypt aad") : std::string();

            CipherCtx ctx(EVP_CIPHER_CTX_new());
            int outl = 0, total = 0;
            if (!ctx || EVP_DecryptInit_ex(ctx.get(), cipher, nullptr, nullptr, nullptr) != 1 ||
                EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, intLen(iv.size(), "iv"), nullptr) != 1 ||
                EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, uc(key), uc(iv)) != 1)
                throw KiritoError(sslError("aesdecrypt init"));
            if (hasAad && !aad.empty() &&
                EVP_DecryptUpdate(ctx.get(), nullptr, &outl, uc(aad), intLen(aad.size(), "aad")) != 1)
                throw KiritoError(sslError("aesdecrypt aad"));
            std::string pt(ct.size(), '\0');
            if (!ct.empty()) {
                if (EVP_DecryptUpdate(ctx.get(), ucw(pt), &outl, uc(ct), intLen(ct.size(), "ciphertext")) != 1)
                    throw KiritoError(sslError("aesdecrypt"));
                total = outl;
            }
            // SET_TAG must precede Final; Final returns <=0 iff the tag doesn't authenticate.
            if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, intLen(tag.size(), "tag"), ucw(tag)) != 1)
                throw KiritoError(sslError("aesdecrypt set tag"));
            if (EVP_DecryptFinal_ex(ctx.get(), ucw(pt) + total, &outl) <= 0)
                throw KiritoError("aesdecrypt: authentication failed (wrong key/iv/tag or tampered data)");
            total += outl;
            pt.resize(static_cast<std::size_t>(total));
            return Bytes(vm, std::move(pt));
#else
            (void)vm; (void)a;
            throw KiritoError("crypto.aesdecrypt requires building with KIRITO_ENABLE_TLS (OpenSSL)");
#endif
        });

        // --- RSA ---
        m.fn("rsagenerate", {{"bits", "Integer", vm.makeInt(2048)}}, "Dict",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
#ifdef KIRITO_ENABLE_TLS
            using namespace cryptossl;
            ERR_clear_error();
            int64_t bits = Args(vm, a, "rsagenerate")[0].asInt("rsagenerate bits");
            if (bits < 512 || bits > 16384) throw KiritoError("rsagenerate: bits must be in [512, 16384]");
            PKey key(EVP_RSA_gen(static_cast<unsigned int>(bits)));
            if (!key) throw KiritoError(sslError("rsagenerate"));
            Dict d(vm);
            d.set("private", Value(vm, privateToPEM(key.get())));
            d.set("public", Value(vm, publicToPEM(key.get())));
            return d;
#else
            (void)vm; (void)a;
            throw KiritoError("crypto.rsagenerate requires building with KIRITO_ENABLE_TLS (OpenSSL)");
#endif
        });
        m.fn("rsasign", {{"private_pem", "String"}, {"message"}, {"algo", "String", vm.makeString("sha256")}}, "Bytes",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
#ifdef KIRITO_ENABLE_TLS
            using namespace cryptossl;
            ERR_clear_error();
            Args args(vm, a, "rsasign");
            std::string pem = args[0].asStringRef("rsasign private_pem");
            const std::string& msg = argStringOrBytes(vm, args[1].handle(), "rsasign message");
            return Bytes(vm, pkeySign(pem, msg, args[2].asStringRef("rsasign algo")));
#else
            (void)vm; (void)a;
            throw KiritoError("crypto.rsasign requires building with KIRITO_ENABLE_TLS (OpenSSL)");
#endif
        });
        m.fn("rsaverify", {{"public_pem", "String"}, {"message"}, {"signature"}, {"algo", "String", vm.makeString("sha256")}}, "Bool",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
#ifdef KIRITO_ENABLE_TLS
            using namespace cryptossl;
            ERR_clear_error();
            Args args(vm, a, "rsaverify");
            std::string pem = args[0].asStringRef("rsaverify public_pem");
            const std::string& msg = argStringOrBytes(vm, args[1].handle(), "rsaverify message");
            const std::string& sig = argStringOrBytes(vm, args[2].handle(), "rsaverify signature");
            return vm.makeBool(pkeyVerify(pem, msg, sig, args[3].asStringRef("rsaverify algo")));
#else
            (void)vm; (void)a;
            throw KiritoError("crypto.rsaverify requires building with KIRITO_ENABLE_TLS (OpenSSL)");
#endif
        });
        m.fn("rsaencrypt", {{"public_pem", "String"}, {"data"}}, "Bytes",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
#ifdef KIRITO_ENABLE_TLS
            using namespace cryptossl;
            ERR_clear_error();
            Args args(vm, a, "rsaencrypt");
            PKey key = loadPublic(args[0].asStringRef("rsaencrypt public_pem"));
            const std::string& data = argStringOrBytes(vm, args[1].handle(), "rsaencrypt data");
            return Bytes(vm, rsaOaep(key.get(), data, true));
#else
            (void)vm; (void)a;
            throw KiritoError("crypto.rsaencrypt requires building with KIRITO_ENABLE_TLS (OpenSSL)");
#endif
        });
        m.fn("rsadecrypt", {{"private_pem", "String"}, {"data"}}, "Bytes",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
#ifdef KIRITO_ENABLE_TLS
            using namespace cryptossl;
            ERR_clear_error();
            Args args(vm, a, "rsadecrypt");
            PKey key = loadPrivate(args[0].asStringRef("rsadecrypt private_pem"));
            const std::string& data = argStringOrBytes(vm, args[1].handle(), "rsadecrypt data");
            return Bytes(vm, rsaOaep(key.get(), data, false));
#else
            (void)vm; (void)a;
            throw KiritoError("crypto.rsadecrypt requires building with KIRITO_ENABLE_TLS (OpenSSL)");
#endif
        });

        // --- Elliptic curve (ECDSA sign/verify; key encryption is RSA-only, by design) ---
        m.fn("ecgenerate", {{"curve", "String", vm.makeString("prime256v1")}}, "Dict",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
#ifdef KIRITO_ENABLE_TLS
            using namespace cryptossl;
            ERR_clear_error();
            std::string curve = Args(vm, a, "ecgenerate")[0].asStringRef("ecgenerate curve");
            PKey key(EVP_EC_gen(curve.c_str()));
            if (!key) throw KiritoError(sslError("ecgenerate (unknown curve '" + curve + "'?)"));
            Dict d(vm);
            d.set("private", Value(vm, privateToPEM(key.get())));
            d.set("public", Value(vm, publicToPEM(key.get())));
            return d;
#else
            (void)vm; (void)a;
            throw KiritoError("crypto.ecgenerate requires building with KIRITO_ENABLE_TLS (OpenSSL)");
#endif
        });
        m.fn("ecsign", {{"private_pem", "String"}, {"message"}, {"algo", "String", vm.makeString("sha256")}}, "Bytes",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
#ifdef KIRITO_ENABLE_TLS
            using namespace cryptossl;
            ERR_clear_error();
            Args args(vm, a, "ecsign");
            std::string pem = args[0].asStringRef("ecsign private_pem");
            const std::string& msg = argStringOrBytes(vm, args[1].handle(), "ecsign message");
            return Bytes(vm, pkeySign(pem, msg, args[2].asStringRef("ecsign algo")));
#else
            (void)vm; (void)a;
            throw KiritoError("crypto.ecsign requires building with KIRITO_ENABLE_TLS (OpenSSL)");
#endif
        });
        m.fn("ecverify", {{"public_pem", "String"}, {"message"}, {"signature"}, {"algo", "String", vm.makeString("sha256")}}, "Bool",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
#ifdef KIRITO_ENABLE_TLS
            using namespace cryptossl;
            ERR_clear_error();
            Args args(vm, a, "ecverify");
            std::string pem = args[0].asStringRef("ecverify public_pem");
            const std::string& msg = argStringOrBytes(vm, args[1].handle(), "ecverify message");
            const std::string& sig = argStringOrBytes(vm, args[2].handle(), "ecverify signature");
            return vm.makeBool(pkeyVerify(pem, msg, sig, args[3].asStringRef("ecverify algo")));
#else
            (void)vm; (void)a;
            throw KiritoError("crypto.ecverify requires building with KIRITO_ENABLE_TLS (OpenSSL)");
#endif
        });

        // --- X.509 certificate parsing ---
        m.fn("x509parse", {{"pem", "String"}}, "Dict",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
#ifdef KIRITO_ENABLE_TLS
            using namespace cryptossl;
            ERR_clear_error();
            std::string pem = Args(vm, a, "x509parse")[0].asStringRef("x509parse pem");
            Bio bio(BIO_new_mem_buf(pem.data(), intLen(pem.size(), "pem")));
            X509Ptr cert(bio ? PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr) : nullptr);
            if (!cert) throw KiritoError(sslError("x509parse: invalid certificate PEM"));

            auto nameStr = [](X509_NAME* n) -> std::string {
                if (!n) return "";
                char* line = X509_NAME_oneline(n, nullptr, 0);
                std::string s = line ? line : "";
                if (line) OPENSSL_free(line);
                return s;
            };
            std::string serial;
            if (ASN1_INTEGER* sn = X509_get_serialNumber(cert.get())) {
                BIGNUM* bn = ASN1_INTEGER_to_BN(sn, nullptr);
                if (bn) { char* h = BN_bn2hex(bn); if (h) { serial = h; OPENSSL_free(h); } BN_free(bn); }
            }
            List sans(vm);
            if (auto* names = static_cast<GENERAL_NAMES*>(
                    X509_get_ext_d2i(cert.get(), NID_subject_alt_name, nullptr, nullptr))) {
                int cnt = sk_GENERAL_NAME_num(names);
                for (int i = 0; i < cnt; ++i) {
                    const GENERAL_NAME* gn = sk_GENERAL_NAME_value(names, i);
                    if (gn && gn->type == GEN_DNS) {
                        const unsigned char* d = ASN1_STRING_get0_data(gn->d.dNSName);
                        int len = ASN1_STRING_length(gn->d.dNSName);
                        if (d && len > 0)
                            sans.push(Value(vm, std::string(reinterpret_cast<const char*>(d), static_cast<std::size_t>(len))));
                    }
                }
                GENERAL_NAMES_free(names);
            }
            Dict d(vm);
            d.set("subject", Value(vm, nameStr(X509_get_subject_name(cert.get()))));
            d.set("issuer", Value(vm, nameStr(X509_get_issuer_name(cert.get()))));
            d.set("serial", Value(vm, serial));
            d.set("not_before", Value(vm, asn1TimeStr(X509_get0_notBefore(cert.get()))));
            d.set("not_after", Value(vm, asn1TimeStr(X509_get0_notAfter(cert.get()))));
            d.set("sans", sans);
            return d;
#else
            (void)vm; (void)a;
            throw KiritoError("crypto.x509parse requires building with KIRITO_ENABLE_TLS (OpenSSL)");
#endif
        });
    }
};

#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

}  // namespace kirito

#endif
