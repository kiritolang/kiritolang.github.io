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
#  include <netdb.h>
#  include <netinet/in.h>
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

}  // namespace kirito::netcompat

#endif
