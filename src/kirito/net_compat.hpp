#ifndef KIRITO_NET_COMPAT_HPP
#define KIRITO_NET_COMPAT_HPP

// Cross-platform sockets: thin shims over Winsock (Windows) and BSD sockets (POSIX) so the rest
// of the net module is platform-agnostic. Only the primitives the module needs are abstracted.

#include <cstdint>
#include <string>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
// Link Winsock when building on MSVC; on MinGW pass -lws2_32.
#  ifdef _MSC_VER
#    pragma comment(lib, "ws2_32.lib")
#  endif
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>   // TCP_NODELAY
#  include <sys/socket.h>
#  include <unistd.h>
#  include <cerrno>
#  include <csignal>
#  include <cstring>
#endif

namespace kirito::netcompat {

#if defined(_WIN32)
using socket_t = SOCKET;
inline constexpr socket_t kInvalidSocket = INVALID_SOCKET;
inline bool isValid(socket_t s) { return s != INVALID_SOCKET; }
inline void closeSocket(socket_t s) { ::closesocket(s); }
inline std::string lastError() { return "winsock error " + std::to_string(::WSAGetLastError()); }
// One-time Winsock startup, triggered the first time the net module is used.
inline bool startup() {
    static bool ok = [] {
        WSADATA d;
        return ::WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
}
#else
using socket_t = int;
inline constexpr socket_t kInvalidSocket = -1;
inline bool isValid(socket_t s) { return s >= 0; }
inline void closeSocket(socket_t s) { ::close(s); }
inline std::string lastError() { return std::strerror(errno); }
// Ignore SIGPIPE process-wide so that writing to a peer that has closed its end returns EPIPE
// (a catchable error) instead of killing the whole process with an uncatchable signal — the
// everyday "client disconnected mid-response" case for the bundled servers. Belt-and-suspenders
// alongside MSG_NOSIGNAL below (which platforms like macOS without that flag rely on).
inline bool startup() {
    static bool once = [] { ::signal(SIGPIPE, SIG_IGN); return true; }();
    return once;
}
#endif

// MSG_NOSIGNAL keeps send() from raising SIGPIPE on a broken pipe; it is absent on some platforms
// (e.g. macOS — covered there by the SIGPIPE-ignore in startup()), so fall back to 0.
#ifndef MSG_NOSIGNAL
#  define MSG_NOSIGNAL 0
#endif

// recv/send return ssize_t on POSIX and int on Windows; normalize to long long. The length is cast to
// `int` (the Winsock signature), so clamp to a chunk first — a payload > 2 GiB would otherwise overflow
// `int` to a negative/garbage length. Both calls may transfer fewer bytes than requested; the callers
// already loop, so clamping each syscall is transparent.
inline constexpr std::size_t kIoChunk = std::size_t{1} << 24;  // 16 MiB, well within INT_MAX
inline long long sendBytes(socket_t s, const char* buf, std::size_t n) {
    if (n > kIoChunk) n = kIoChunk;
    return static_cast<long long>(::send(s, buf, static_cast<int>(n), MSG_NOSIGNAL));
}
inline long long recvBytes(socket_t s, char* buf, std::size_t n) {
    if (n > kIoChunk) n = kIoChunk;
    return static_cast<long long>(::recv(s, buf, static_cast<int>(n), 0));
}

// Datagram send/recv. Same int/size_t normalization as sendBytes/recvBytes; a datagram never
// partials, so the (never-triggered for real UDP) 16 MiB clamp is only an overflow guard.
inline long long sendTo(socket_t s, const char* buf, std::size_t n, const sockaddr* to, socklen_t tolen) {
    if (n > kIoChunk) n = kIoChunk;
    return static_cast<long long>(::sendto(s, buf, static_cast<int>(n), MSG_NOSIGNAL, to, tolen));
}
inline long long recvFrom(socket_t s, char* buf, std::size_t n, sockaddr* from, socklen_t* fromlen) {
    if (n > kIoChunk) n = kIoChunk;
    return static_cast<long long>(::recvfrom(s, buf, static_cast<int>(n), 0, from, fromlen));
}

// --- socket-foundation primitives (UDP/options/half-close/blocking/pairs) -------------------------
// setsockopt/getsockopt for an int-valued option. POSIX takes `const void*`/`void*`, Winsock `const
// char*`/`char*`; the char* cast satisfies both. `socklen_t` is `int` on Windows (ws2tcpip.h).
inline bool setSockOptInt(socket_t s, int level, int opt, int val) {
    return ::setsockopt(s, level, opt, reinterpret_cast<const char*>(&val),
                        static_cast<socklen_t>(sizeof(val))) == 0;
}
inline bool getSockOptInt(socket_t s, int level, int opt, int& val) {
    socklen_t len = static_cast<socklen_t>(sizeof(val));
    return ::getsockopt(s, level, opt, reinterpret_cast<char*>(&val), &len) == 0;
}

// Half-close a socket. POSIX SHUT_RD/WR/RDWR and Winsock SD_RECEIVE/SD_SEND/SD_BOTH share the
// numeric values 0/1/2, so `how` (0=read, 1=write, 2=both) maps straight through.
inline int shutdownSocket(socket_t s, int how) { return ::shutdown(s, how); }

// Put a socket into blocking or non-blocking mode.
inline bool setBlocking(socket_t s, bool blocking) {
#if defined(_WIN32)
    u_long mode = blocking ? 0 : 1;
    return ::ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    int flags = ::fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    flags = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
    return ::fcntl(s, F_SETFL, flags) == 0;
#endif
}

// Connect with a bounded wait. A plain blocking ::connect() to a black-hole host (silently dropped
// SYNs) hangs for the OS default (tens of seconds to minutes) regardless of any SO_*TIMEO — those
// only bound send/recv. When `seconds > 0` this switches to non-blocking connect + select on
// writability, so the wait is capped. Returns 0 on success, -1 on error/timeout (errno/WSA set as
// usual; a timeout reports ETIMEDOUT / WSAETIMEDOUT). Restores the socket's blocking mode on return.
inline int connectWithTimeout(socket_t s, const sockaddr* addr, socklen_t addrlen, double seconds) {
    if (seconds <= 0) return ::connect(s, addr, addrlen);
    if (!setBlocking(s, false)) return ::connect(s, addr, addrlen);  // fall back to blocking
    int rc = ::connect(s, addr, addrlen);
#if defined(_WIN32)
    bool inProgress = (rc != 0) && (::WSAGetLastError() == WSAEWOULDBLOCK);
#else
    bool inProgress = (rc != 0) && (errno == EINPROGRESS);
#endif
    if (rc == 0) { setBlocking(s, true); return 0; }          // connected immediately
    if (!inProgress) { int e =
#if defined(_WIN32)
        ::WSAGetLastError();
#else
        errno;
#endif
        setBlocking(s, true);
#if defined(_WIN32)
        ::WSASetLastError(e);
#else
        errno = e;
#endif
        return -1;
    }
    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(s, &wset);
    struct timeval tv;
    tv.tv_sec = static_cast<long>(seconds);
    tv.tv_usec = static_cast<long>((seconds - static_cast<double>(tv.tv_sec)) * 1e6);
    int sel = ::select(static_cast<int>(s) + 1, nullptr, &wset, nullptr, &tv);
    if (sel <= 0) {                                            // 0 = timeout, <0 = select error
        setBlocking(s, true);
#if defined(_WIN32)
        ::WSASetLastError(WSAETIMEDOUT);
#else
        errno = (sel == 0) ? ETIMEDOUT : errno;
#endif
        return -1;
    }
    int soerr = 0;
    if (!getSockOptInt(s, SOL_SOCKET, SO_ERROR, soerr) || soerr != 0) {  // connect's real result
        setBlocking(s, true);
#if defined(_WIN32)
        ::WSASetLastError(soerr ? soerr : WSAETIMEDOUT);
#else
        errno = soerr ? soerr : ETIMEDOUT;
#endif
        return -1;
    }
    setBlocking(s, true);
    return 0;
}

// A connected pair of sockets (like Python socket.socketpair). POSIX has a native socketpair() over
// AF_UNIX. Windows has none, so emulate a STREAM pair over a 127.0.0.1 loopback listener; a datagram
// pair is unsupported there (returns false → the net module throws a clear error).
//
// The family the pair ACTUALLY gets, which genuinely differs by platform — so the Socket reports what
// it is instead of claiming AF_INET everywhere.
#if defined(_WIN32)
inline constexpr int kSocketPairFamily = AF_INET;
#else
inline constexpr int kSocketPairFamily = AF_UNIX;
#endif
#if defined(_WIN32)
inline bool socketPair(int type, socket_t out[2]) {
    if (type != SOCK_STREAM) return false;
    startup();
    socket_t listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (!isValid(listener)) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    int namelen = static_cast<int>(sizeof(addr));
    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &namelen) != 0 ||
        ::listen(listener, 1) != 0) { closeSocket(listener); return false; }
    socket_t client = ::socket(AF_INET, SOCK_STREAM, 0);
    if (!isValid(client)) { closeSocket(listener); return false; }
    if (::connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closeSocket(listener); closeSocket(client); return false;
    }
    socket_t server = ::accept(listener, nullptr, nullptr);
    closeSocket(listener);
    if (!isValid(server)) { closeSocket(client); return false; }
    out[0] = client; out[1] = server;
    return true;
}
#else
inline bool socketPair(int type, socket_t out[2]) {
    return ::socketpair(AF_UNIX, type, 0, out) == 0;
}
#endif

// The local hostname (gethostname), or empty on failure.
inline std::string hostName() {
    startup();
    char buf[256];
    if (::gethostname(buf, sizeof(buf)) != 0) return "";
    buf[sizeof(buf) - 1] = '\0';
    return std::string(buf);
}

}  // namespace kirito::netcompat

#endif
