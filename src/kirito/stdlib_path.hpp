#ifndef KIRITO_STDLIB_PATH_HPP
#define KIRITO_STDLIB_PATH_HPP

#include <cstring>
#include <filesystem>
#include <span>
#include <string>

#include "collections.hpp"
#include "native.hpp"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#elif defined(__APPLE__)
#  include <mach-o/dyld.h>
#endif

namespace kirito {

// Absolute path of the running interpreter executable, or "" if it can't be determined. Exposed as
// path.executable (a filesystem location, so it lives in `path`) and used by `kpm` to locate the
// `ki` binary it should replace when self-updating Kirito.
inline std::string currentExecutablePath() {
#if defined(_WIN32)
    wchar_t buf[32768];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return "";
    int need = WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), out.data(), need, nullptr, nullptr);
    return out;
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return "";
    buf.resize(std::strlen(buf.c_str()));
    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(buf, ec);
    return ec ? buf : canon.string();
#else
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : p.string();
#endif
}

// The `path` module — Kirito's os.path + os filesystem surface. It is the single home for ALL path
// and filesystem operations, so callers never have to remember whether a helper lives in `io` or
// `sys`:
//   * pure path-string manipulation: join / dirname / basename / splitext
//   * read-only filesystem queries: exists / isfile / isdir / getsize / listdir / walk / getcwd
//   * filesystem mutation: mkdir / remove / rmtree / rename / chmod
// `io` owns only actual I/O (open/print/input/read/write, streams, BytesIO) — nothing that
// interprets, queries, or mutates the filesystem by path.
//
// The mutating ops are **strict by default** (they throw rather than silently no-op), with opt-in
// leniency: `mkdir(exist_ok=False)` throws if the directory already exists; `remove`/`rmtree`
// (missing_ok=False) throw if the target does not exist. `mkdir`/`remove`/`rmtree` return a Bool
// (True = it did the work, False = the opt-in lenient no-op); `rename` returns None and throws on
// failure. The read-only listing helpers (`listdir`/`walk`) stay tolerant (a missing dir lists as []).
//
// Path strings use '/' on EVERY platform (results are identical cross-platform, unlike
// std::filesystem's '\' on Windows). A '\' in the input is still accepted as a separator by the
// splitting helpers, so native Windows paths (from path.getcwd/path.listdir) still split correctly.

// The native-binding idiom below re-uses `vm` as a bound-lambda parameter that intentionally shadows
// the enclosing setup `vm` (same VM, by design). Silence -Wshadow for these mechanical bindings; it
// stays active in the evaluator/parser/lexer core.
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wshadow"
#endif

class PathModule : public NativeModule {
public:
    std::string name() const override { return "path"; }

    void setup(ModuleBuilder& m) override {
        KiritoVM& vm = m.vm();
        auto pathArg = [](KiritoVM& vm, Handle h) -> std::string { return Value(vm, h).asString("path"); };

        // join(parts...) -> the parts joined with '/'. A later component that is absolute (starts
        // with '/') resets the result. Like os.path.join it needs at least one component (throws
        // otherwise). Variadic. (A leading '\' is NOT treated as absolute — only '/' resets.)
        m.fn("join", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "join");
            if (args.empty()) throw KiritoError("join expected at least one path component");
            std::string out = args[0].asString("join");
            for (std::size_t i = 1; i < a.size(); ++i) {
                std::string part = args[i].asString("join");
                if (!part.empty() && part[0] == '/') out = part;            // absolute resets
                else if (out.empty() || out.back() == '/') out += part;     // no double separator
                else out += "/" + part;
            }
            return val(vm, out);
        });

        // dirname/basename/splitext are plain '/'- or '\'-based string ops (NOT std::filesystem,
        // whose separator and semantics are platform-dependent). They return literal substrings of
        // the input — they do not rewrite separators — so only basename (the final component) is
        // guaranteed free of a backslash; a '\' already inside a retained prefix is preserved.
        m.fn("dirname", {{"path", "String"}}, "String", [pathArg](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            std::string p = pathArg(vm, a[0]);
            std::size_t s = p.find_last_of("/\\");
            if (s == std::string::npos) return val(vm, std::string());
            std::size_t end = s;   // strip the separator run back to the parent (os.path.dirname)
            while (end > 0 && (p[end - 1] == '/' || p[end - 1] == '\\')) --end;
            // an all-separator prefix is the root: keep one separator (dirname("/a") -> "/") rather
            // than dropping it to "".
            return val(vm, end == 0 ? p.substr(0, s + 1) : p.substr(0, end));
        });
        m.fn("basename", {{"path", "String"}}, "String", [pathArg](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            std::string p = pathArg(vm, a[0]);
            std::size_t s = p.find_last_of("/\\");
            return val(vm, s == std::string::npos ? p : p.substr(s + 1));
        });
        m.fn("splitext", {{"path", "String"}}, "List", [pathArg](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            std::string path = pathArg(vm, a[0]);
            std::size_t sep = path.find_last_of("/\\");
            std::size_t baseStart = (sep == std::string::npos) ? 0 : sep + 1;
            // an extension dot must come AFTER at least one non-dot char in the final component, so a
            // whole leading run of dots is skipped — ".bashrc"/".."/"...x" have no extension, matching
            // os.path.splitext (its rule protects the leading dot *run*, not just the first char).
            std::size_t scan = baseStart;
            while (scan < path.size() && path[scan] == '.') ++scan;
            std::size_t dot = path.find_last_of('.');
            if (dot == std::string::npos || dot < scan)
                return List(vm).add(val(vm, path)).add(val(vm, std::string())).build();
            return List(vm).add(val(vm, path.substr(0, dot))).add(val(vm, path.substr(dot))).build();
        });

        // Filesystem queries. exists/isfile/isdir are tolerant (a missing/inaccessible path is simply
        // False, never a throw); getsize throws on a missing/non-regular file (there is no sensible
        // size to return).
        m.fn("exists", {{"path", "String"}}, "Bool", [pathArg](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            std::error_code ec;
            return val(vm, std::filesystem::exists(pathArg(vm, a[0]), ec));
        });
        m.fn("isfile", {{"path", "String"}}, "Bool", [pathArg](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            std::error_code ec;
            return val(vm, std::filesystem::is_regular_file(pathArg(vm, a[0]), ec));
        });
        m.fn("isdir", {{"path", "String"}}, "Bool", [pathArg](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            std::error_code ec;
            return val(vm, std::filesystem::is_directory(pathArg(vm, a[0]), ec));
        });
        m.fn("getsize", {{"path", "String"}}, "Integer", [pathArg](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            std::error_code ec;
            auto sz = std::filesystem::file_size(pathArg(vm, a[0]), ec);
            if (ec) throw KiritoError("getsize: " + ec.message());
            return val(vm, static_cast<int64_t>(sz));
        });

        // listing (read-only, tolerant: a missing/inaccessible dir lists as []).
        m.fn("listdir", {{"path", "String"}}, "List", [pathArg](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            List out(vm);
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(pathArg(vm, a[0]), ec))
                out.add(val(vm, entry.path().filename().string()));
            return out.build();
        });
        // walk(dir) -> every file path under dir, recursively (flattened; tolerant like listdir).
        m.fn("walk", {{"dir", "String"}}, "List", [pathArg](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            List out(vm);
            std::error_code ec;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(pathArg(vm, a[0]), ec))
                if (entry.is_regular_file()) out.add(val(vm, entry.path().string()));
            return out.build();
        });
        m.fn("getcwd", {}, "String", [](KiritoVM& vm, std::span<const Handle>) -> Handle {
            std::error_code ec;
            return val(vm, std::filesystem::current_path(ec).string());
        });
        // gettempdir() -> the system temp directory (honors TMPDIR/TMP/TEMP, falls back to /tmp) — a
        // stable scratch location to build temp file paths with path.join. A filesystem location, so
        // (like path.executable) it lives here in `path`, the single home for path/filesystem surface.
        m.fn("gettempdir", {}, "String", [](KiritoVM& vm, std::span<const Handle>) -> Handle {
            std::error_code ec;
            auto p = std::filesystem::temp_directory_path(ec);
            return val(vm, ec ? std::string("/tmp") : p.string());
        });
        // executable: absolute path of the running `ki` binary ("" if undeterminable) — a filesystem
        // location, used e.g. by kpm for self-replacement.
        m.value("executable", vm.makeString(currentExecutablePath()));

        // --- mutation (strict by default: throw rather than silently no-op) ---
        // mkdir(path, exist_ok=False) -> Bool: create the directory (and any missing parents). By
        // default RAISES if `path` already exists; exist_ok=True instead returns False for an existing
        // directory. Returns True when it actually creates the directory.
        m.fn("mkdir", {{"path", "String"}, {"exist_ok", "Bool", vm.makeBool(false)}}, "Bool",
             [pathArg](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            std::string p = pathArg(vm, a[0]);
            bool exist_ok = Value(vm, a[1]).asBool("exist_ok");
            std::error_code ec;
            if (std::filesystem::exists(p, ec)) {
                if (exist_ok) return val(vm, false);   // already existed — nothing created
                throw KiritoError("mkdir: '" + p + "' already exists (pass exist_ok = True to ignore)");
            }
            std::filesystem::create_directories(p, ec);
            if (ec) throw KiritoError("mkdir: " + ec.message());
            return val(vm, true);
        });
        // remove(path, missing_ok=False) -> Bool: delete a file (or an EMPTY directory). By default
        // RAISES if the path does not exist; missing_ok=True instead returns False. Returns True when
        // it removes something. A non-empty directory throws ("directory not empty") — use rmtree.
        m.fn("remove", {{"path", "String"}, {"missing_ok", "Bool", vm.makeBool(false)}}, "Bool",
             [pathArg](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            std::string p = pathArg(vm, a[0]);
            bool missing_ok = Value(vm, a[1]).asBool("missing_ok");
            std::error_code ec;
            bool removed = std::filesystem::remove(p, ec);   // false = nothing was there (no error set)
            if (ec) throw KiritoError("remove: " + ec.message());
            if (!removed && !missing_ok)
                throw KiritoError("remove: '" + p + "' does not exist (pass missing_ok = True to ignore)");
            return val(vm, removed);
        });
        // rmtree(path, missing_ok=False) -> Bool: recursively delete a directory and everything under
        // it (or a single file) — the recursive counterpart to remove (like rm -rf / shutil.rmtree). By
        // default RAISES if the path does not exist; missing_ok=True instead returns False. Returns True
        // when it removes something.
        m.fn("rmtree", {{"path", "String"}, {"missing_ok", "Bool", vm.makeBool(false)}}, "Bool",
             [pathArg](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            std::string p = pathArg(vm, a[0]);
            bool missing_ok = Value(vm, a[1]).asBool("missing_ok");
            std::error_code ec;
            auto count = std::filesystem::remove_all(p, ec);   // number of entries deleted (0 = absent)
            if (ec) throw KiritoError("rmtree: " + ec.message());
            if (count == 0 && !missing_ok)
                throw KiritoError("rmtree: '" + p + "' does not exist (pass missing_ok = True to ignore)");
            return val(vm, count > 0);
        });
        m.fn("rename", {{"src", "String"}, {"dst", "String"}}, "", [pathArg](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            std::error_code ec;
            std::filesystem::rename(pathArg(vm, a[0]), pathArg(vm, a[1]), ec);
            if (ec) throw KiritoError("rename failed: " + ec.message());
            return none(vm);
        });
        // chmod(path, mode): set permission bits from a POSIX-style octal Integer (e.g. 0o755). The low
        // 12 bits map onto std::filesystem::perms (POSIX on Unix; on Windows only the owner read/write
        // bits are meaningful). Lenient (returns success as a Bool; a missing file is False, no throw).
        m.fn("chmod", {{"path", "String"}, {"mode", "Integer"}}, "Bool",
             [pathArg](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            auto mode = static_cast<unsigned>(Value(vm, a[1]).asInt("mode")) & 0xFFFu;
            std::error_code ec;
            std::filesystem::permissions(pathArg(vm, a[0]), std::filesystem::perms(mode),
                                         std::filesystem::perm_options::replace, ec);
            return val(vm, !ec);
        });
    }
};

}  // namespace kirito

#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
#endif
