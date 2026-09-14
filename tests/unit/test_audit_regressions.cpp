// Regression tests for the audit pass that could NOT be converted to .ki (see
// tests/scripts/unit_audit_regressions.ki for the rest of this file's original content, and its header
// comment for why these two blocks stay in C++):
//   - the nested-block-pyramid parse-depth guard needs O(N^2) indentation whitespace to reach the same
//     cap the other four deep-nesting shapes hit with O(N) source size, so it is generated in-memory
//     here rather than committed as a multi-MB .ki fixture (tests/errors/unit_audit_regressions_deep_
//     {index,add,pow,member}.ki cover the O(N) shapes).
//   - the net HTTP content-decoding integrity-trailer check calls net::decodeBody / gzipfmt::compress /
//     deflate::zlibCompress directly; none of those are reachable from Kirito script (decodeBody is only
//     invoked internally off a real HTTP response's Content-Encoding header).
// Run under -fsanitize=address,undefined to confirm the UB is gone.
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// Run `src` in a fresh stdlib-equipped VM; return the error message ("" if it did not throw).
static std::string err(const std::string& src) {
    KiritoVM vm;
    vm.installStandardLibrary();
    try { vm.runSource(src); return ""; }
    catch (const KiritoError& e) { return e.what(); }
    catch (const std::exception& e) { return std::string("std:") + e.what(); }
}
static bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

int main() {
    // === deep nesting: a nested-block indentation pyramid must THROW at parse time, never overflow
    // the native stack (parseIndentedSuite recursion). 5000 > the 2000 non-sanitizer parse-depth cap
    // (and the 250 sanitizer cap). Built in-memory so no multi-MB fixture is committed. -------------
    {
        const int N = 5000;
        std::string blk;
        for (int i = 0; i < N; ++i) blk += std::string(static_cast<std::size_t>(i), ' ') + "if True:\n";
        blk += std::string(static_cast<std::size_t>(N), ' ') + "discard 1\n";
        CHECK(has(err(blk), "nested too deeply"));
    }

    // === net HTTP content-decoding must VALIDATE the integrity trailer (audit v1.18.0). A response
    // body whose DEFLATE payload is intact but whose gzip CRC-32 / zlib Adler-32 trailer no longer
    // matches must be REJECTED -- exactly as gzip.decompress / zlib.decompress reject it. Over plain
    // HTTP this app-layer checksum is the only integrity guard, and net previously used a private,
    // weaker decoder (net::gunzip) that dropped the check, silently accepting corrupt data (SSOT).
    // Corrupting only the trailer guarantees the body still inflates, so this pins CRC/Adler checking
    // specifically, not merely a malformed-stream rejection. ------------------------------------------
    {
        const std::string orig = "role=user;balance=100;role=user;balance=100;";
        std::string gz = gzipfmt::compress(orig);
        CHECK(net::decodeBody(gz, "gzip") == orig);                 // a valid gzip body decodes
        std::string badgz = gz; badgz[gz.size() - 6] = static_cast<char>(badgz[gz.size() - 6] ^ 0x01);
        bool gzThrew = false;
        try { (void)net::decodeBody(badgz, "gzip"); } catch (const std::exception&) { gzThrew = true; }
        CHECK(gzThrew);                                             // corrupt CRC-32 -> rejected, not silent

        std::string zl = deflate::zlibCompress(orig);
        CHECK(net::decodeBody(zl, "deflate") == orig);             // a valid zlib-wrapped body decodes
        std::string badzl = zl; badzl[zl.size() - 2] = static_cast<char>(badzl[zl.size() - 2] ^ 0x01);
        bool zlThrew = false;
        try { (void)net::decodeBody(badzl, "deflate"); } catch (const std::exception&) { zlThrew = true; }
        CHECK(zlThrew);                                             // corrupt Adler-32 -> rejected, not silent
    }

    return RUN_TESTS();
}
