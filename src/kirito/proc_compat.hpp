#ifndef KIRITO_PROC_COMPAT_HPP
#define KIRITO_PROC_COMPAT_HPP

// Cross-platform child-process execution: run a program (given as an argv vector) with optional
// stdin, capturing its stdout, stderr and exit code. The platform-specific bits (fork+execvp+pipe
// on POSIX; CreateProcessW+CreatePipe on Windows) live behind a single run() so the `sys` module —
// and therefore Kirito code — is identical on every platform. Mirrors net_compat.hpp's structure.
//
// stdout and stderr are each drained on their own thread so a child that fills one pipe while we feed
// stdin (or while we read the other) can never deadlock. A positive timeout kills the child (SIGKILL
// / TerminateProcess) and throws. This is for running EXTERNAL programs (ffmpeg, a shell, ...), NOT
// the `parallel` worker-VM model.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <cerrno>
#  include <csignal>
#  include <cstring>
#  include <fcntl.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace kirito::proccompat {

struct ProcResult {
    int code = 0;
    std::string out;
    std::string err;
    bool truncated = false;   // a captured stream hit kMaxCapture and was cut off
};

// Cap on captured stdout/stderr. A runaway child could otherwise grow `out`/`err` until bad_alloc —
// and that would be thrown INSIDE a drain thread with no try/catch, i.e. std::terminate / SIGABRT,
// uncatchable by Kirito (A10-4). Past the cap we stop storing but keep draining (so the child can't
// deadlock on a full pipe) and flag truncation; the caller turns it into a clean catchable error.
inline constexpr std::size_t kMaxCapture = 256ull * 1024 * 1024;

// Spawn failure (program not found, etc.) or timeout; the `sys` glue maps it to a KiritoError.
struct ProcError : std::runtime_error {
    explicit ProcError(const std::string& m) : std::runtime_error(m) {}
};

#if defined(_WIN32)

inline std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
inline std::string wideToUtf8(const wchar_t* w, int len) {
    if (len <= 0) return std::string();
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, len, s.data(), n, nullptr, nullptr);
    return s;
}
// Quote one argument per the rules CommandLineToArgvW uses, so a round-trip recovers the exact argv
// (the standard "everyone quotes the wrong way" algorithm: double the run of backslashes that
// precedes an embedded quote, and the trailing run before the closing quote).
inline std::wstring quoteArgW(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (std::size_t i = 0;; ++i) {
        unsigned bs = 0;
        while (i < arg.size() && arg[i] == L'\\') { ++i; ++bs; }
        if (i >= arg.size()) { out.append(bs * 2, L'\\'); break; }
        if (arg[i] == L'"') { out.append(bs * 2 + 1, L'\\'); out.push_back(L'"'); }
        else { out.append(bs, L'\\'); out.push_back(arg[i]); }
    }
    out.push_back(L'"');
    return out;
}
inline void drainHandle(HANDLE h, std::string& out, std::atomic<bool>& truncated) {
    char buf[65536];
    DWORD n = 0;
    try {
        while (::ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) {
            std::size_t got = n;
            if (out.size() < kMaxCapture) {
                std::size_t room = kMaxCapture - out.size();
                out.append(buf, std::min(room, got));
                if (got > room) truncated = true;
            } else {
                truncated = true;                    // keep reading (don't deadlock the child), discard
            }
        }
    } catch (...) {
        truncated = true;                            // a bad_alloc etc. must never escape the drain thread
    }
}

inline ProcResult run(const std::vector<std::string>& argv, const std::string& cwd,
                      const std::string& input, double timeoutSecs) {
    if (argv.empty()) throw ProcError("empty argument list");

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
    if (!::CreatePipe(&inR, &inW, &sa, 0) || !::CreatePipe(&outR, &outW, &sa, 0) ||
        !::CreatePipe(&errR, &errW, &sa, 0)) {
        for (HANDLE h : {inR, inW, outR, outW, errR, errW}) if (h) ::CloseHandle(h);
        throw ProcError("CreatePipe failed");
    }
    // The parent's own ends must not be inherited by the child.
    ::SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inR;
    si.hStdOutput = outW;
    si.hStdError = errW;
    PROCESS_INFORMATION pi{};

    std::wstring cmdline;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i) cmdline.push_back(L' ');
        cmdline += quoteArgW(utf8ToWide(argv[i]));
    }
    std::vector<wchar_t> cmdbuf(cmdline.begin(), cmdline.end());
    cmdbuf.push_back(L'\0');  // CreateProcessW requires a mutable buffer
    std::wstring cwdW = utf8ToWide(cwd);

    // A Job Object with KILL_ON_JOB_CLOSE is the Windows analogue of a POSIX process group: assigning
    // the child to it makes a timeout's TerminateJobObject kill the ENTIRE process tree (grandchildren
    // included), so a surviving grandchild can't keep the stdout/stderr pipes open and hang the drain
    // threads. Create the child SUSPENDED, assign it, then resume — no window where it could spawn a
    // child before being placed in the job.
    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }
    BOOL ok = ::CreateProcessW(nullptr, cmdbuf.data(), nullptr, nullptr, TRUE,
                               CREATE_NO_WINDOW | CREATE_SUSPENDED,
                               nullptr, cwd.empty() ? nullptr : cwdW.c_str(), &si, &pi);
    ::CloseHandle(inR); ::CloseHandle(outW); ::CloseHandle(errW);  // child-side ends, parent is done with them
    if (!ok) {
        DWORD e = ::GetLastError();
        if (job) ::CloseHandle(job);
        ::CloseHandle(inW); ::CloseHandle(outR); ::CloseHandle(errR);
        throw ProcError("failed to start '" + argv[0] + "' (error " + std::to_string(e) + ")");
    }
    if (job) ::AssignProcessToJobObject(job, pi.hProcess);
    ::ResumeThread(pi.hThread);

    ProcResult res;
    std::atomic<bool> trunc{false};
    std::thread tout([&] { drainHandle(outR, res.out, trunc); });
    std::thread terr([&] { drainHandle(errR, res.err, trunc); });
    std::thread tin([&] {
        std::size_t off = 0;
        while (off < input.size()) {
            DWORD n = 0;
            if (!::WriteFile(inW, input.data() + off, static_cast<DWORD>(input.size() - off), &n, nullptr) || n == 0) break;
            off += n;
        }
        ::CloseHandle(inW);  // EOF for the child's stdin
    });

    bool timedOut = false;
    DWORD waitMs = timeoutSecs > 0 ? static_cast<DWORD>(timeoutSecs * 1000.0) : INFINITE;
    if (::WaitForSingleObject(pi.hProcess, waitMs) == WAIT_TIMEOUT) {
        if (job) ::TerminateJobObject(job, 1);   // kills the whole tree; falls back to the process below
        ::TerminateProcess(pi.hProcess, 1);
        ::WaitForSingleObject(pi.hProcess, INFINITE);
        timedOut = true;
    }
    tin.join(); tout.join(); terr.join();
    res.truncated = trunc.load();

    DWORD code = 0;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(outR); ::CloseHandle(errR);
    if (job) ::CloseHandle(job);
    ::CloseHandle(pi.hProcess); ::CloseHandle(pi.hThread);
    if (timedOut) throw ProcError("process timed out");
    res.code = static_cast<int>(code);
    return res;
}

#else  // ---------------------------------------------------------------------- POSIX

// Writing to a pipe whose read end the child has closed raises SIGPIPE by default, which would kill
// the whole interpreter; ignore it process-wide (once) so the write just returns EPIPE instead.
inline void ensureSigpipeIgnored() {
    static const bool once = [] { ::signal(SIGPIPE, SIG_IGN); return true; }();
    (void)once;
}
inline void drainFd(int fd, std::string& out, std::atomic<bool>& truncated) {
    char buf[65536];
    try {
        for (;;) {
            ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n > 0) {
                std::size_t got = static_cast<std::size_t>(n);
                if (out.size() < kMaxCapture) {
                    std::size_t room = kMaxCapture - out.size();
                    out.append(buf, std::min(room, got));
                    if (got > room) truncated = true;
                } else {
                    truncated = true;            // keep reading (don't deadlock the child) but discard
                }
            }
            else if (n == 0) break;              // EOF: the child closed this stream
            else if (errno == EINTR) continue;
            else break;
        }
    } catch (...) {
        truncated = true;                        // a bad_alloc etc. must never escape the drain thread
    }
}

inline ProcResult run(const std::vector<std::string>& argv, const std::string& cwd,
                      const std::string& input, double timeoutSecs) {
    if (argv.empty()) throw ProcError("empty argument list");
    ensureSigpipeIgnored();

    int inP[2], outP[2], errP[2], exP[2];   // exP: the exec-error channel (see below)
    if (::pipe(inP) != 0 || ::pipe(outP) != 0 || ::pipe(errP) != 0 || ::pipe(exP) != 0)
        throw ProcError(std::string("pipe failed: ") + std::strerror(errno));
    ::fcntl(exP[1], F_SETFD, FD_CLOEXEC);    // closes on a successful exec -> parent reads EOF = "started ok"

    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& s : argv) cargv.push_back(const_cast<char*>(s.c_str()));
    cargv.push_back(nullptr);

    pid_t pid = ::fork();
    if (pid < 0) {
        for (int fd : {inP[0], inP[1], outP[0], outP[1], errP[0], errP[1], exP[0], exP[1]}) ::close(fd);
        throw ProcError(std::string("fork failed: ") + std::strerror(errno));
    }
    if (pid == 0) {
        // CHILD: only async-signal-safe calls before exec.
        // Become a process-group leader (pgid == our pid) so a timeout can SIGKILL the WHOLE tree
        // (`kill(-pgid)`), not just this direct child — otherwise a grandchild (`sh -c "sleep 100 &"`)
        // survives, keeps the stdout/stderr pipes open, and the parent's drain-thread joins hang
        // forever even after the timeout fired.
        ::setpgid(0, 0);
        ::dup2(inP[0], 0); ::dup2(outP[1], 1); ::dup2(errP[1], 2);
        ::close(inP[0]); ::close(inP[1]); ::close(outP[0]); ::close(outP[1]);
        ::close(errP[0]); ::close(errP[1]); ::close(exP[0]);
        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) {
            int e = errno; ssize_t w = ::write(exP[1], &e, sizeof(e)); (void)w; ::_exit(127);
        }
        ::execvp(cargv[0], cargv.data());
        int e = errno; ssize_t w = ::write(exP[1], &e, sizeof(e)); (void)w; ::_exit(127);
    }

    // PARENT: close the child's ends.
    ::close(inP[0]); ::close(outP[1]); ::close(errP[1]); ::close(exP[1]);

    // The exec-error channel: this read blocks until the child either execs (CLOEXEC closes exP[1] ->
    // EOF here) or fails and writes its errno. This is how "command not found" becomes a clean error.
    int childErrno = 0;
    ssize_t got = ::read(exP[0], &childErrno, sizeof(childErrno));
    ::close(exP[0]);
    if (got == static_cast<ssize_t>(sizeof(childErrno))) {
        int st = 0; ::waitpid(pid, &st, 0);
        ::close(inP[1]); ::close(outP[0]); ::close(errP[0]);
        throw ProcError("failed to start '" + argv[0] + "': " + std::strerror(childErrno));
    }

    ProcResult res;
    std::atomic<bool> trunc{false};
    std::thread tout([&] { drainFd(outP[0], res.out, trunc); });
    std::thread terr([&] { drainFd(errP[0], res.err, trunc); });
    std::thread tin([&] {
        std::size_t off = 0;
        while (off < input.size()) {
            ssize_t n = ::write(inP[1], input.data() + off, input.size() - off);
            if (n > 0) off += static_cast<std::size_t>(n);
            else if (n < 0 && errno == EINTR) continue;
            else break;  // EPIPE (child closed stdin) or error
        }
        ::close(inP[1]);  // EOF for the child's stdin
    });

    int status = 0;
    bool timedOut = false;
    if (timeoutSecs > 0) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSecs);
        for (;;) {
            pid_t w = ::waitpid(pid, &status, WNOHANG);
            if (w == pid) break;
            if (w < 0 && errno != EINTR) break;
            if (std::chrono::steady_clock::now() >= deadline) {
                ::kill(-pid, SIGKILL);   // the whole process group (child is its leader) -> grandchildren too
                ::kill(pid, SIGKILL);    // fallback, in case setpgid(0,0) hadn't taken effect yet
                while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
                timedOut = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    } else {
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }

    tin.join(); tout.join(); terr.join();
    ::close(outP[0]); ::close(errP[0]);
    res.truncated = trunc.load();

    if (timedOut) throw ProcError("process timed out");
    if (WIFEXITED(status)) res.code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) res.code = 128 + WTERMSIG(status);  // conventional 128+signal
    else res.code = -1;
    return res;
}

#endif

}  // namespace kirito::proccompat

#endif
