#ifndef KIRITO_RAND_COMPAT_HPP
#define KIRITO_RAND_COMPAT_HPP

// Cross-platform access to the operating-system CSPRNG, isolated behind one fillRandom() so the
// `random` module (randombytes/randomhex/randomurlsafe/randombelow) is identical on every platform.
// Mirrors net_compat.hpp / proc_compat.hpp. POSIX prefers getrandom(2) (Linux/glibc), falls back to
// getentropy(3) (BSD/macOS), then to reading /dev/urandom; Windows uses BCryptGenRandom. This is the
// kernel entropy source — distinct from the `Random` object's userspace PRNGs (xoshiro/MT), which are
// deterministic and seedable and must NOT be used where unpredictability matters.

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <bcrypt.h>
#else
#  include <cerrno>
#  include <fcntl.h>
#  include <unistd.h>
#  if defined(__linux__)
#    include <sys/random.h>   // getrandom
#  endif
#endif

namespace kirito::randcompat {

// Fill buf[0..n) with cryptographically secure random bytes. Returns true on success; false only if
// every available entropy source failed (extremely rare — a broken/locked-down system). The caller
// turns false into a clear, catchable error rather than ever handing out weak/zero bytes.
inline bool fillRandom(void* buf, std::size_t n) {
    if (n == 0) return true;
    unsigned char* p = static_cast<unsigned char*>(buf);

#if defined(_WIN32)
    // BCRYPT_USE_SYSTEM_PREFERRED_RNG => no algorithm handle needed. STATUS_SUCCESS is 0.
    return BCryptGenRandom(nullptr, p, static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    std::size_t got = 0;
#  if defined(__linux__)
    // getrandom() can return short (a signal, or >256 bytes at once from the pool). Loop; retry EINTR.
    while (got < n) {
        ssize_t r = ::getrandom(p + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;                       // ENOSYS on an ancient kernel, etc. -> fall through
        }
        got += static_cast<std::size_t>(r);
    }
    if (got == n) return true;
#  elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    // getentropy caps at 256 bytes per call; chunk it.
    while (got < n) {
        std::size_t chunk = n - got < 256 ? n - got : 256;
        if (::getentropy(p + got, chunk) != 0) break;
        got += chunk;
    }
    if (got == n) return true;
#  endif
    // Fallback: read /dev/urandom (covers the getrandom ENOSYS case and platforms without getentropy).
    int fd = ::open("/dev/urandom", O_RDONLY);
    if (fd < 0) return false;
    bool ok = true;
    while (got < n) {
        ssize_t r = ::read(fd, p + got, n - got);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            ok = false;
            break;
        }
        got += static_cast<std::size_t>(r);
    }
    ::close(fd);
    return ok && got == n;
#endif
}

}  // namespace kirito::randcompat

#endif
